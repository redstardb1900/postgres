/*-------------------------------------------------------------------------
 *
 * dynahash.c
 *	  动态链接哈希表
 *
 * dynahash.c 支持后端本地哈希表，也支持共享内存哈希表。
 * 对于共享哈希表，访问互锁由调用者负责提供。
 * 最简单的约定是由单个 LWLock 保护整个哈希表。
 * 查找（HASH_FIND 或 hash_seq_search）只需要共享锁，
 * 但任何更新都需要排他锁。对于高频使用的共享表，
 * 单锁方式会造成并发瓶颈，因此我们还支持"分区"锁
 * 模式，即由多个 LWLock 守护表中不同的子集。要使用
 * 分区模式的哈希表，必须向 hash_create 传入
 * HASH_PARTITION 标志。
 * 这能防止任何运行中拆分桶的尝试。因此每个哈
 * 希桶链独立运作，且初始化后哈希头除 nentries
 * 和 freeList 之外没有字段会变化。（分区表用这
 * 些字段的多份副本由自旋锁保护，以获得额外的
 * 并发性。）
 * 这使哈希桶的任一子集都能被当作可独立加锁的
 * 分区。我们期望调用者用键哈希值低位作分区号，
 * —— 由于 calc_bucket() 把哈希值映射到桶号的方式，
 * 这样做是可行的。
 *
 * 共享内存中分配的哈希表，目录不能扩展，因为它
 * malloc 失败时返回 NULL 的语义。对于本地内存中的
 * 哈希表则通常用 palloc()，失败时会抛出错误。
 * 本文件中的代码必须同时应对这两种情况。
 *
 * dynahash.c 支持以下几类查找键：
 *
 * 1. NUL 结尾的 C 字符串（必要时截断以适应 keysize），
 * 比较如同 strcmp()。通过向 hash_create 指定
 * HASH_STRINGS 标志来选用。
 *
 * 2. 大小为 keysize 的任意二进制数据，如同 memcmp()
 * 那样比较。（调用者必须确保键中没有未定义的填充
 * 位！）通过向 hash_create 指定 HASH_BLOBS 标志
 *
 * 3. 更复杂的键行为可以通过指定用户提供的哈希、
 * 比较和/或键复制函数来选用。至少必须提供哈希
 * 函数；比较默认用 memcmp()，键复制默认用 memcpy()，
 * 前提是选用了用户定义的哈希函数。
 *
 * 与 simplehash 相比，dynahash 有以下优点：
 *
 * - 它支持分区，这对使用锁进行的共享内存访问很有用。
 *   锁。
 * - 共享内存哈希表在启动时分配在固定大小的区域中，
 *   并且可被其他进程按名字找到。
 * - 因为哈希冲突时条目无需移动，dynahash 对大条目
 *   有更好的性能。
 * - 保证条目指针稳定。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/hash/dynahash.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * 原始注释：
 *
 * 动态哈希算法，见 CACM 1988 年 4 月号 pp 446-457，
 * 由 Per-Ake Larson 撰写；ejp@ausmelb.oz 于 1988 年 7 月
 * 26 日 13:16 移植为 C。
 * 完成；并加入模拟 hsearch(3) 的 hcreate/hdestroy。
 *
 * 这些例程模拟 hsearch(3) 家族，重要差别是哈希表
 * 是动态的——可无限增长，超出初始大小（即
 * 传给 hcreate() 的大小）。
 *
 * 性能看起来与 hsearch(3) 相当。
 * hsearch(3) 手册页中提到的 'source-code' 选项
 * 未实现；其余功能完全相同。
 *
 * 编译控制：
 * HASH_DEBUG 控制一些信息性跟踪，主要用于调试。
 * HASH_STATISTICS 使 HashAccesses 和 HashCollisions
 * 得以维护；与 HASH_DEBUG 组合时由 hdestroy() 显示。
 *
 * 问题与修复请发 ejp@ausmelb.oz。警告：依赖预处理器
 * 的连接特性，存在于可能不必要的代码"优化"中。
 *
 * Modified margo@postgres.berkeley.edu February 1990
 *		增加多表接口。
 * Modified by sullivan@postgres.berkeley.edu April 1990
 *		为共享内存更改 ctl 结构。
 */

#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "common/hashfn.h"
#include "port/pg_bitutils.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/dynahash.h"
#include "utils/memutils.h"


/*
 * 常量
 *
 * 哈希表有一个顶层"目录"，其中每个条目都指向一个
 * 包含 ssize 个桶头的"段"。哈希桶的最大数量因此为
 * dsize * ssize（但 dsize 可扩展）。当然，表中的记录
 * 数可以更多，但每个桶的记录太多时
 * 性能就会下降。
 *
 * 共享内存中分配的哈希表，目录不能扩展，因为它
 * 必须保持在固定地址上。目录大小应当用
 * hash_select_dirsize 来选择（你最好对最大条目数心
 * 中有数！）。非共享哈希表的初始目录大小可留
 * 在默认值。
 */
#define DEF_SEGSIZE			   256
#define DEF_SEGSIZE_SHIFT	   8	/* log2(DEF_SEGSIZE) */
#define DEF_DIRSIZE			   256

/* 分区哈希表使用的空闲列表数量。 */
#define NUM_FREELISTS			32

/* 哈希桶是 HASHELEMENT 的链表 */
typedef HASHELEMENT *HASHBUCKET;

/* 哈希段是桶头的数组 */
typedef HASHBUCKET *HASHSEGMENT;

/*
 * 空闲列表数据
 *
 * 在分区哈希表中，每个空闲列表都关联着一组特定的
 * 哈希码（由下面的 FREELIST_IDX() 宏决定）。
 * nentries 跟踪具有这些哈希码的活动条目数（并不
 * 是空闲列表中的条目数）。
 *
 * 一个空闲列表的覆盖范围可能大于或小于一个分区，
 * 所以它需要自己的锁而不能依赖调用者的锁。即使覆
 * 盖范围相同也不行，因为偶尔要从其他空闲列表
 * "借用"条目；参见 get_hash_entry()。
 *
 * 使用 FreeListData 数组而非互斥锁、nentries 和
 * freeList 的独立数组，有助于减少不同互斥锁之间的
 * 缓存行共享。
 */
typedef struct
{
	slock_t		mutex;			/* 此空闲列表自旋锁 */
	long		nentries;		/* 关联桶中的条目数 */
	HASHELEMENT *freeList;		/* 空闲元素链 */
} FreeListData;

/*
 * 哈希表的头结构 —— 包含所有可变信息
 *
 * 在共享内存哈希表中，HASHHDR 位于共享内存，而每
 * 个后端都有一个本地 HTAB 结构。对于非共享表，
 * HASHHDR 和 HTAB 之间没有功能差异，但我们还是把
 * 分开以便在共享与非共享表间共用代码。
 */
struct HASHHDR
{
	/*
	 * 空闲列表在高并发哈希表中可能成为争用点，因此
	 * 们使用一组空闲列表，各自带有互斥锁和 nentries
	 * 计数，而非只用单个。尽管各空闲列表通常独立
	 * 运作，必要时会从哈希码默认空闲列表之外拣
	 * 选条目。
	 *
	 * 如果哈希表未分区，则只使用 freeList[0] 且完全
	 * 不使用其自旋锁；假定调用者的锁已足够。
	 */
	FreeListData freeList[NUM_FREELISTS];

	/* 这些字段可变，但在分区表中不行 */
	/* 且共享表中 dsize 不可变，即使未分区 */
	long		dsize;			/* 目录大小 */
	long		nsegs;			/* 已分配段数（<= dsize） */
	uint32		max_bucket;		/* 使用中的最大桶号 */
	uint32		high_mask;		/* 对整表取模的掩码 */
	uint32		low_mask;		/* 对表下半部分取模的掩码 */

	/* 这些字段在哈希表创建时固定 */
	Size		keysize;		/* 哈希键字节长度 */
	Size		entrysize;		/* 用户元素总字节大小 */
	long		num_partitions; /* 分区数（须为 2 的幂）或 0 */
	long		max_dsize;		/* 目录固定大小时的 'dsize' 上限 */
	long		ssize;			/* 段大小 —— 必须为 2 的幂 */
	int			sshift;			/* 段移位 = log2(ssize) */
	int			nelem_alloc;	/* 一次分配的条目数 */

#ifdef HASH_STATISTICS

	/*
	 * 在此统计。注意：统计代码不碰互斥锁，分区表中
	 * 的计数值可能稍有损坏。
	 */
	long		accesses;
	long		collisions;
#endif
};

#define IS_PARTITIONED(hctl)  ((hctl)->num_partitions != 0)

#define FREELIST_IDX(hctl, hashcode) \
	(IS_PARTITIONED(hctl) ? (hashcode) % NUM_FREELISTS : 0)

/*
 * 哈希表的顶层控制结构 —— 共享表中每个后端都有
 * 自己的副本（因运行时无字段变化）
 */
struct HTAB
{
	HASHHDR    *hctl;			/* => 共享控制信息 */
	HASHSEGMENT *dir;			/* 段起始的目录 */
	HashValueFunc hash;			/* 哈希函数 */
	HashCompareFunc match;		/* 键比较函数 */
	HashCopyFunc keycopy;		/* 键复制函数 */
	HashAllocFunc alloc;		/* 内存分配器 */
	MemoryContext hcxt;			/* 用默认分配器时的内存上下文 */
	char	   *tabname;		/* 表名（用于错误消息） */
	bool		isshared;		/* 表在共享内存中则为真 */
	bool		isfixed;		/* 为真则不扩大 */

	/* 共享表不允许冻结，所以可以在此保存状态 */
	bool		frozen;			/* 真 = 不再允许插入 */

	/* 保存这些固定值的本地副本以减少争用 */
	Size		keysize;		/* 哈希键字节长度 */
	long		ssize;			/* 段大小 —— 必须为 2 的幂 */
	int			sshift;			/* 段移位 = log2(ssize) */
};

/*
 * HASHELEMENT 键/条目部分
 */
#define ELEMENTKEY(helem)  (((char *)(helem)) + MAXALIGN(sizeof(HASHELEMENT)))

/*
 * 由键指针求取元素指针
 */
#define ELEMENT_FROM_KEY(key)  \
	((HASHELEMENT *) (((char *) (key)) - MAXALIGN(sizeof(HASHELEMENT))))

/*
 * 快速取模运算，假定 y 为 2 的幂！
 */
#define MOD(x,y)			   ((x) & ((y)-1))

#ifdef HASH_STATISTICS
static long hash_accesses,
			hash_collisions,
			hash_expansions;
#endif

/*
 * 私有函数原型
 */
static void *DynaHashAlloc(Size size);
static HASHSEGMENT seg_alloc(HTAB *hashp);
static bool element_alloc(HTAB *hashp, int nelem, int freelist_idx);
static bool dir_realloc(HTAB *hashp);
static bool expand_table(HTAB *hashp);
static HASHBUCKET get_hash_entry(HTAB *hashp, int freelist_idx);
static void hdefault(HTAB *hashp);
static int	choose_nelem_alloc(Size entrysize);
static bool init_htab(HTAB *hashp, long nelem);
static void hash_corrupted(HTAB *hashp);
static long next_pow2_long(long num);
static int	next_pow2_int(long num);
static void register_seq_scan(HTAB *hashp);
static void deregister_seq_scan(HTAB *hashp);
static bool has_seq_scans(HTAB *hashp);


/*
 * 内存分配支持
 */
static MemoryContext CurrentDynaHashCxt = NULL;

static void *
DynaHashAlloc(Size size)
{
	Assert(MemoryContextIsValid(CurrentDynaHashCxt));
	return MemoryContextAllocExtended(CurrentDynaHashCxt, size,
									  MCXT_ALLOC_NO_OOM);
}


/*
 * 字符串键的 HashCompareFunc
 *
 * 因为我们用 strlcpy() 复制键，键会在 keysize-1 字节
 * 处被截断，所以只能比较那么多 …… 因此 strncmp 差
 * 并非完全合适。
 */
static int
string_compare(const char *key1, const char *key2, Size keysize)
{
	return strncmp(key1, key2, keysize - 1);
}


/************************** 创建例程 **********************/

/*
 * hash_create -- 创建新动态哈希表
 *
 *	tabname: 表名（用于调试）
 *	nelem: 预期的最大元素数
 *	*info: 附加表参数，由 flags 指示
 *	flags: 位掩码，指示从 *info 取哪些参数
 *
 * flags 值*必须*包含 HASH_ELEM。（以前名义上可选，
 * 但默认的 keysize 和 entrysize 值毫无用处。）
 * flags 值还必须恰好包含 HASH_STRINGS、HASH_BLOBS、
 * 或 HASH_FUNCTION 之一，以定义键哈希语义（分别对
 * 应 C 字符串、二进制块或自定义）。指定自定义
 * 哈希函数的调用者多半还想用 HASH_COMPARE，或许还
 * 要用 HASH_KEYCOPY，以控制键比较与复制。
 * 另一个常用标志是 HASH_CONTEXT，用于把哈希表分配
 * 在 info->hcxt 之下而非 TopMemoryContext 之下；默认
 * 行为只适合会话生命周期的哈希表。
 * 其他标志位是特殊用途且很少使用，除了那些与共享
 * 内存哈希表相关的标志，参见 ShmemInitHash()。
 *
 * 只有在相关 flags 位被设置时才会读取 *info 中的字
 * 段。无需初始化 *info 的其他字段。
 * tabname 和 *info 都无需在 hash_create() 调用后继续
 *
 * 注意：不推荐 hash_create() 的调用者显式指定
 * string_hash、tag_hash、uint32_hash 或 oid_hash。仅
 * 当需要的键哈希行为不同时才用 HASH_FUNCTION。平时设
 * 置即可。
 *
 * 注意：共享内存哈希表的 nelem 必须相当准确，
 * 估计值，因为我们无法动态扩展表。但非共享哈希表
 * 可以动态扩展，所以 nelem 宁小勿大，超出时让表自
 * 行增长即可。过大的 nelem 会拖慢 hash_seq_search 而
 * 收益甚微。
 */
HTAB *
hash_create(const char *tabname, long nelem, const HASHCTL *info, int flags)
{
	HTAB	   *hashp;
	HASHHDR    *hctl;

	/*
	 * 哈希表会为键和数据分配空间，但需指明数量。
	 * 分配多少空间。
	 */
	Assert(flags & HASH_ELEM);
	Assert(info->keysize > 0);
	Assert(info->entrysize >= info->keysize);

	/*
	 * 对于共享哈希表，本地哈希头（HTAB 结构）分配在
	 * TopMemoryContext 中；其余都在共享内存中。
	 *
	 * 非共享哈希表的一切（包括哈希头）都位于专门为哈
	 * 希表创建的内存上下文中 —— 这使 hash_destroy 非常
	 * 简单。其内存上下文的父上下文为调用者指定的，
	 * 如果没有指定则是 TopMemoryContext
	 * 。
	 */
	if (flags & HASH_SHARED_MEM)
	{
		/* 准备分配哈希头 */
		CurrentDynaHashCxt = TopMemoryContext;
	}
	else
	{
		/* 创建哈希表私有的内存上下文 */
		if (flags & HASH_CONTEXT)
			CurrentDynaHashCxt = info->hcxt;
		else
			CurrentDynaHashCxt = TopMemoryContext;
		CurrentDynaHashCxt = AllocSetContextCreate(CurrentDynaHashCxt,
												   "dynahash",
												   ALLOCSET_DEFAULT_SIZES);
	}

	/* 初始化哈希头，外加表名的一份副本 */
	hashp = (HTAB *) MemoryContextAlloc(CurrentDynaHashCxt,
										sizeof(HTAB) + strlen(tabname) + 1);
	MemSet(hashp, 0, sizeof(HTAB));

	hashp->tabname = (char *) (hashp + 1);
	strcpy(hashp->tabname, tabname);

	/* 若有私有上下文，用哈希表名给它打标签 */
	if (!(flags & HASH_SHARED_MEM))
		MemoryContextSetIdentifier(CurrentDynaHashCxt, hashp->tabname);

	/*
	 * 选择合适的哈希函数（参见文件头部的注释）。
	 */
	if (flags & HASH_FUNCTION)
	{
		Assert(!(flags & (HASH_BLOBS | HASH_STRINGS)));
		hashp->hash = info->hash;
	}
	else if (flags & HASH_BLOBS)
	{
		Assert(!(flags & HASH_STRINGS));
		/* 可以对常见键大小优化哈希 */
		if (info->keysize == sizeof(uint32))
			hashp->hash = uint32_hash;
		else
			hashp->hash = tag_hash;
	}
	else
	{
		/*
		 * string_hash 曾被视为默认哈希法，非断言构建中
		 * 它实际仍是。但现在不显式给出 HASH_STRINGS 会
		 * 被视作断言错误。为防 HASH_STRINGS 误用，我们
		 * 求较长字符串：keysize 仅 4/8 时几乎必
		 * 是整数或指针而非字符串
		 * 。
		 */
		Assert(flags & HASH_STRINGS);
		Assert(info->keysize > 8);

		hashp->hash = string_hash;
	}

	/*
	 * 如果不指定匹配函数，默认在 string_hash 时为
	 * string_compare，否则为 memcmp。
	 *
	 * 注意：显式指定 string_hash 已不推荐，因为在某些
	 * 平台上，这对可加载模块中的调用者可能无效，因为
	 * 它引用的是蹦床函数而非 string_hash 函数本体。请改
	 * 用 HASH_STRINGS。
	 */
	if (flags & HASH_COMPARE)
		hashp->match = info->match;
	else if (hashp->hash == string_hash)
		hashp->match = (HashCompareFunc) string_compare;
	else
		hashp->match = memcmp;

	/*
	 * 类似地，键复制函数默认为 strlcpy 或 memcpy。
	 */
	if (flags & HASH_KEYCOPY)
		hashp->keycopy = info->keycopy;
	else if (hashp->hash == string_hash)
	{
		/*
		 * keycopy 的签名源自 memcpy()（返回 void*），而
		 * strlcpy() 返回 size_t。由于我们从不使用 keycopy 的
		 * 返回值，且 size_t 几乎总是与 void * 大小相同，所以
		 * 这应该安全。中间的额外强转是为避免
		 * -Wcast-function-type 的警告。
		 */
		hashp->keycopy = (HashCopyFunc) (pg_funcptr_t) strlcpy;
	}
	else
		hashp->keycopy = memcpy;

	/* 同时选择条目分配函数。 */
	if (flags & HASH_ALLOC)
		hashp->alloc = info->alloc;
	else
		hashp->alloc = DynaHashAlloc;

	if (flags & HASH_SHARED_MEM)
	{
		/*
		 * 共享表的 ctl 结构和目录已预分配。最好把
		 * HASH_DIRSIZE 和 HASH_ALLOC 也一并设置好
		 * 。
		 */
		hashp->hctl = info->hctl;
		hashp->dir = (HASHSEGMENT *) (((char *) info->hctl) + sizeof(HASHHDR));
		hashp->hcxt = NULL;
		hashp->isshared = true;

		/* 哈希表已存在，我们只是附加上去 */
		if (flags & HASH_ATTACH)
		{
			/* 复制一些高频使用的值到本地 */
			hctl = hashp->hctl;
			hashp->keysize = hctl->keysize;
			hashp->ssize = hctl->ssize;
			hashp->sshift = hctl->sshift;

			return hashp;
		}
	}
	else
	{
		/* 设置哈希表默认值 */
		hashp->hctl = NULL;
		hashp->dir = NULL;
		hashp->hcxt = CurrentDynaHashCxt;
		hashp->isshared = false;
	}

	if (!hashp->hctl)
	{
		hashp->hctl = (HASHHDR *) hashp->alloc(sizeof(HASHHDR));
		if (!hashp->hctl)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	hashp->frozen = false;

	hdefault(hashp);

	hctl = hashp->hctl;

	if (flags & HASH_PARTITION)
	{
		/* 对本地哈希表分区没有意义 */
		Assert(flags & HASH_SHARED_MEM);

		/*
		 * 分区数最好是 2 的幂，且必须小于 INT_MAX（见
		 * init_htab()），所以要调用 int 版的 next_pow2
		 * 。
		 */
		Assert(info->num_partitions == next_pow2_int(info->num_partitions));

		hctl->num_partitions = info->num_partitions;
	}

	if (flags & HASH_SEGMENT)
	{
		hctl->ssize = info->ssize;
		hctl->sshift = my_log2(info->ssize);
		/* ssize 最好是 2 的幂 */
		Assert(hctl->ssize == (1L << hctl->sshift));
	}

	/*
	 * 共享表的目录大小固定，由调用者传入。
	 */
	if (flags & HASH_DIRSIZE)
	{
		hctl->max_dsize = info->max_dsize;
		hctl->dsize = info->dsize;
	}

	/* 也记住条目大小 */
	hctl->keysize = info->keysize;
	hctl->entrysize = info->entrysize;

	/* 复制高频使用的固定字段到本地 */
	hashp->keysize = hctl->keysize;
	hashp->ssize = hctl->ssize;
	hashp->sshift = hctl->sshift;

	/* 构建哈希目录结构 */
	if (!init_htab(hashp, nelem))
		elog(ERROR, "failed to initialize hash table \"%s\"", hashp->tabname);

	/*
	 * 共享哈希表要预分配所请求数量的元素，这能减少
	 * 共享内存运行时耗尽带来的问题。
	 *
	 * 对于非共享哈希表，若请求数量小于我们选定的
	 * nelem_alloc，则也预分配。这样调用者正确估计较小
	 * 表大小时就不会浪费空间。
	 */
	if ((flags & HASH_SHARED_MEM) ||
		nelem < hctl->nelem_alloc)
	{
		int			i,
					freelist_partitions,
					nelem_alloc,
					nelem_alloc_first;

		/*
		 * 若分区，每个空闲列表均分初始分配额。
		 * 否则只使用 freeList[0]。
		 */
		if (IS_PARTITIONED(hashp->hctl))
			freelist_partitions = NUM_FREELISTS;
		else
			freelist_partitions = 1;

		nelem_alloc = nelem / freelist_partitions;
		if (nelem_alloc <= 0)
			nelem_alloc = 1;

		/*
		 * 确保会分配所有请求的元素；如果请求不能被
		 * NUM_FREELISTS 整除，多余部分归 freeList[0]。
		 */
		if (nelem_alloc * freelist_partitions < nelem)
			nelem_alloc_first =
				nelem - nelem_alloc * (freelist_partitions - 1);
		else
			nelem_alloc_first = nelem_alloc;

		for (i = 0; i < freelist_partitions; i++)
		{
			int			temp = (i == 0) ? nelem_alloc_first : nelem_alloc;

			if (!element_alloc(hashp, temp, i))
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
		}
	}

	if (flags & HASH_FIXED_SIZE)
		hashp->isfixed = true;
	return hashp;
}

/*
 * 设置默认 HASHHDR 参数。
 */
static void
hdefault(HTAB *hashp)
{
	HASHHDR    *hctl = hashp->hctl;

	MemSet(hctl, 0, sizeof(HASHHDR));

	hctl->dsize = DEF_DIRSIZE;
	hctl->nsegs = 0;

	hctl->num_partitions = 0;	/* 未分区 */

	/* 表无固定最大大小 */
	hctl->max_dsize = NO_MAX_DSIZE;

	hctl->ssize = DEF_SEGSIZE;
	hctl->sshift = DEF_SEGSIZE_SHIFT;

#ifdef HASH_STATISTICS
	hctl->accesses = hctl->collisions = 0;
#endif
}

/*
 * 根据用户指定的条目大小选择 nelem_alloc，即当我
 * 们需要更多时向哈希表添加的元素数。
 */
static int
choose_nelem_alloc(Size entrysize)
{
	int			nelem_alloc;
	Size		elementSize;
	Size		allocSize;

	/* 每个元素有 HASHELEMENT 头外加用户数据。 */
	/* 注意：最好与 element_alloc() 一致 */
	elementSize = MAXALIGN(sizeof(HASHELEMENT)) + MAXALIGN(entrysize);

	/*
	 * 这里的思路是选择至少 32 的 nelem_alloc，但向上取
	 * 整使分配请求恰为 2 的幂或略小。这对共享内存
	 * 中的哈希表影响甚微；但 palloc 管理的表，请求
	 * 反正会被取整到 2 的幂。若不考虑这点，我们将浪
	 * 费多达一半的分配空间
	 * 。
	 */
	allocSize = 32 * 4;			/* 假定 elementSize ≥ 8 */
	do
	{
		allocSize <<= 1;
		nelem_alloc = allocSize / elementSize;
	} while (nelem_alloc < 32);

	return nelem_alloc;
}

/*
 * 计算 hctl 的派生字段并构建初始的目录/段
 * 数组
 */
static bool
init_htab(HTAB *hashp, long nelem)
{
	HASHHDR    *hctl = hashp->hctl;
	HASHSEGMENT *segp;
	int			nbuckets;
	int			nsegs;
	int			i;

	/*
	 * 如果是分区表则初始化互斥锁
	 */
	if (IS_PARTITIONED(hctl))
		for (i = 0; i < NUM_FREELISTS; i++)
			SpinLockInit(&(hctl->freeList[i].mutex));

	/*
	 * 为下一个更大 2 幂的桶数分配空间，假定最大装载
	 * 因子为 1。
	 */
	nbuckets = next_pow2_int(nelem);

	/*
	 * 在分区表中，nbuckets 必须至少等于
	 * num_partitions；否则，表面哈希分区号不同的键会映
	 * 射到同一个桶，破坏分区独立性。（通常 nbuckets 会
	 * 大得多；这只是个安全检查。）
	 */
	while (nbuckets < hctl->num_partitions)
		nbuckets <<= 1;

	hctl->max_bucket = hctl->low_mask = nbuckets - 1;
	hctl->high_mask = (nbuckets << 1) - 1;

	/*
	 * 算出所需目录段数，并向上取整到 2 的幂
	 */
	nsegs = (nbuckets - 1) / hctl->ssize + 1;
	nsegs = next_pow2_int(nsegs);

	/*
	 * 确保目录足够大。预分配的目录太小则中止（说明
	 * 调用者搞错了）。
	 */
	if (nsegs > hctl->dsize)
	{
		if (!(hashp->dir))
			hctl->dsize = nsegs;
		else
			return false;
	}

	/* 分配一个目录 */
	if (!(hashp->dir))
	{
		CurrentDynaHashCxt = hashp->hcxt;
		hashp->dir = (HASHSEGMENT *)
			hashp->alloc(hctl->dsize * sizeof(HASHSEGMENT));
		if (!hashp->dir)
			return false;
	}

	/* 分配初始段 */
	for (segp = hashp->dir; hctl->nsegs < nsegs; hctl->nsegs++, segp++)
	{
		*segp = seg_alloc(hashp);
		if (*segp == NULL)
			return false;
	}

	/* 选择一次分配的条目数 */
	hctl->nelem_alloc = choose_nelem_alloc(hctl->entrysize);

#ifdef HASH_DEBUG
	fprintf(stderr, "init_htab:\n%s%p\n%s%ld\n%s%ld\n%s%d\n%s%u\n%s%x\n%s%x\n%s%ld\n",
			"TABLE POINTER   ", hashp,
			"DIRECTORY SIZE  ", hctl->dsize,
			"SEGMENT SIZE    ", hctl->ssize,
			"SEGMENT SHIFT   ", hctl->sshift,
			"MAX BUCKET      ", hctl->max_bucket,
			"HIGH MASK       ", hctl->high_mask,
			"LOW  MASK       ", hctl->low_mask,
			"NSEGS           ", hctl->nsegs);
#endif
	return true;
}

/*
 * 估算包含给定数量、给定大小条目的哈希表所需的
 * 空间。
 * 注意：用于估算共享内存哈希表的占用；因此未
 * 把本地内存中的 HTAB 计入。
 * 注意：假定所有哈希结构参数均为默认值！
 */
Size
hash_estimate_size(long num_entries, Size entrysize)
{
	Size		size;
	long		nBuckets,
				nSegments,
				nDirEntries,
				nElementAllocs,
				elementSize,
				elementAllocCnt;

	/* 估算所需桶数 */
	nBuckets = next_pow2_long(num_entries);
	/* nBuckets 所需的段数 */
	nSegments = next_pow2_long((nBuckets - 1) / DEF_SEGSIZE + 1);
	/* 目录条目 */
	nDirEntries = DEF_DIRSIZE;
	while (nDirEntries < nSegments)
		nDirEntries <<= 1;		/* dir_alloc 每次调用 dsize 翻倍 */

	/* 固定控制信息 */
	size = MAXALIGN(sizeof(HASHHDR));	/* HTAB 除外，见上 */
	/* 目录 */
	size = add_size(size, mul_size(nDirEntries, sizeof(HASHSEGMENT)));
	/* 段 */
	size = add_size(size, mul_size(nSegments,
								   MAXALIGN(DEF_SEGSIZE * sizeof(HASHBUCKET))));
	/* 元素 —— 按 choose_nelem_alloc() 条目为一组分配 */
	elementAllocCnt = choose_nelem_alloc(entrysize);
	nElementAllocs = (num_entries - 1) / elementAllocCnt + 1;
	elementSize = MAXALIGN(sizeof(HASHELEMENT)) + MAXALIGN(entrysize);
	size = add_size(size,
					mul_size(nElementAllocs,
							 mul_size(elementAllocCnt, elementSize)));

	return size;
}

/*
 * 为给定最大条目数的哈希表选择合适的目录大小
 * 。
 * 仅共享内存哈希表需要：其目录无法动态扩展
 * 。
 * 注意：假定所有哈希结构参数均为默认值！
 *
 * XXX 这最好与 init_htab() 的行为一致……
 */
long
hash_select_dirsize(long num_entries)
{
	long		nBuckets,
				nSegments,
				nDirEntries;

	/* 估算所需桶数 */
	nBuckets = next_pow2_long(num_entries);
	/* nBuckets 所需的段数 */
	nSegments = next_pow2_long((nBuckets - 1) / DEF_SEGSIZE + 1);
	/* 目录条目 */
	nDirEntries = DEF_DIRSIZE;
	while (nDirEntries < nSegments)
		nDirEntries <<= 1;		/* dir_alloc 每次调用 dsize 翻倍 */

	return nDirEntries;
}

/*
 * 为给定参数的共享内存哈希表计算所需的初始内存
 * 分配。需要为 HASHHDR 和（不可扩展的）目录留出
 * 空间。
 */
Size
hash_get_shared_size(HASHCTL *info, int flags)
{
	Assert(flags & HASH_DIRSIZE);
	Assert(info->dsize == info->max_dsize);
	return sizeof(HASHHDR) + info->dsize * sizeof(HASHSEGMENT);
}


/********************** 销毁例程 ************************/

void
hash_destroy(HTAB *hashp)
{
	if (hashp != NULL)
	{
		/* 分配方法必须也是我们知道如何释放的 */
		Assert(hashp->alloc == DynaHashAlloc);
		/* 所以该哈希表得有自己的上下文 */
		Assert(hashp->hcxt != NULL);

		hash_stats("destroy", hashp);

		/*
		 * 通过销毁哈希表的内存上下文来释放一切。
		 */
		MemoryContextDelete(hashp->hcxt);
	}
}

void
hash_stats(const char *where, HTAB *hashp)
{
#ifdef HASH_STATISTICS
	fprintf(stderr, "%s: this HTAB -- accesses %ld collisions %ld\n",
			where, hashp->hctl->accesses, hashp->hctl->collisions);

	fprintf(stderr, "hash_stats: entries %ld keysize %ld maxp %u segmentcount %ld\n",
			hash_get_num_entries(hashp), (long) hashp->hctl->keysize,
			hashp->hctl->max_bucket, hashp->hctl->nsegs);
	fprintf(stderr, "%s: total accesses %ld total collisions %ld\n",
			where, hash_accesses, hash_collisions);
	fprintf(stderr, "hash_stats: total expansions %ld\n",
			hash_expansions);
#endif
}

/*******************************搜索例程 *****************************/


/*
 * get_hash_value -- 导出例程，计算键的哈希值
 *
 * 导出原因：分区表的调用者需要在查找之前，先
 * 由哈希值的低位算出分区号，然后才能查找
 * 。
 */
uint32
get_hash_value(HTAB *hashp, const void *keyPtr)
{
	return hashp->hash(keyPtr, hashp->keysize);
}

/* 把哈希值转换为桶号 */
static inline uint32
calc_bucket(HASHHDR *hctl, uint32 hash_val)
{
	uint32		bucket;

	bucket = hash_val & hctl->high_mask;
	if (bucket > hctl->max_bucket)
		bucket = bucket & hctl->low_mask;

	return bucket;
}

/*
 * hash_search -- 在表中查找键并执行动作
 * hash_search_with_hash_value -- 同上，但键的哈希值已
 *
 * action 为：
 *		HASH_FIND: 在表中查找键
 *		HASH_ENTER: 查找键，不存在则创建条目
 *		HASH_ENTER_NULL: 同上，但内存耗尽时返回 NULL
 *		HASH_REMOVE: 查找键，存在则移除条目
 *
 * 返回值是指向所找到/进入/移除元素的指针，若无匹
 * 配则返回 NULL。（REMOVE 动作的结果是悬空指针，
 * 不应被解引用！）
 *
 * HASH_ENTER 通常在无法创建新条目时报通用的"内存不
 * 足"错误。HASH_ENTER_NULL 操作相同，但内存耗尽时
 * 返回 NULL。
 *
 * 如果 foundPtr 非 NULL，则找到表中现有条目时
 * *foundPtr 置真，否则置假。HASH_ENTER 时需要，其余
 * 情况下它与返回值冗余。
 *
 * 对于 hash_search_with_hash_value，其 hashvalue 参数
 * 须经 get_hash_value() 算出
 */
void *
hash_search(HTAB *hashp,
			const void *keyPtr,
			HASHACTION action,
			bool *foundPtr)
{
	return hash_search_with_hash_value(hashp,
									   keyPtr,
									   hashp->hash(keyPtr, hashp->keysize),
									   action,
									   foundPtr);
}

void *
hash_search_with_hash_value(HTAB *hashp,
							const void *keyPtr,
							uint32 hashvalue,
							HASHACTION action,
							bool *foundPtr)
{
	HASHHDR    *hctl = hashp->hctl;
	int			freelist_idx = FREELIST_IDX(hctl, hashvalue);
	Size		keysize;
	uint32		bucket;
	long		segment_num;
	long		segment_ndx;
	HASHSEGMENT segp;
	HASHBUCKET	currBucket;
	HASHBUCKET *prevBucketPtr;
	HashCompareFunc match;

#ifdef HASH_STATISTICS
	hash_accesses++;
	hctl->accesses++;
#endif

	/*
	 * 插入时检查是否该拆分桶。
	 *
	 * 注意：扩展失败并非致命，只是意味着必须以更高
	 * 的装载因子运行。但用 palloc 分配器时内存耗尽反
	 * 正会抛错，所以必须在修改表之前做这一步
	 * 。
	 */
	if (action == HASH_ENTER || action == HASH_ENTER_NULL)
	{
		/*
		 * 分区模式下不能拆分，冻结时也不能，表正被
		 * 活动的 hash_seq_search 扫描时同样不能。
		 */
		if (hctl->freeList[0].nentries > (long) hctl->max_bucket &&
			!IS_PARTITIONED(hctl) && !hashp->frozen &&
			!has_seq_scans(hashp))
			(void) expand_table(hashp);
	}

	/*
	 * 进行初始查找
	 */
	bucket = calc_bucket(hctl, hashvalue);

	segment_num = bucket >> hashp->sshift;
	segment_ndx = MOD(bucket, hashp->ssize);

	segp = hashp->dir[segment_num];

	if (segp == NULL)
		hash_corrupted(hashp);

	prevBucketPtr = &segp[segment_ndx];
	currBucket = *prevBucketPtr;

	/*
	 * 沿冲突链查找匹配的键
	 */
	match = hashp->match;		/* 内循环省一次取值 */
	keysize = hashp->keysize;	/* 同 */

	while (currBucket != NULL)
	{
		if (currBucket->hashvalue == hashvalue &&
			match(ELEMENTKEY(currBucket), keyPtr, keysize) == 0)
			break;
		prevBucketPtr = &(currBucket->link);
		currBucket = *prevBucketPtr;
#ifdef HASH_STATISTICS
		hash_collisions++;
		hctl->collisions++;
#endif
	}

	if (foundPtr)
		*foundPtr = (bool) (currBucket != NULL);

	/*
	 * 现在呢
	 */
	switch (action)
	{
		case HASH_FIND:
			if (currBucket != NULL)
				return (void *) ELEMENTKEY(currBucket);
			return NULL;

		case HASH_REMOVE:
			if (currBucket != NULL)
			{
				/* 分区时必须加锁才能动 nentries 和 freeList */
				if (IS_PARTITIONED(hctl))
					SpinLockAcquire(&(hctl->freeList[freelist_idx].mutex));

				/* 从相应的 nentries 计数器中删除该记录。 */
				Assert(hctl->freeList[freelist_idx].nentries > 0);
				hctl->freeList[freelist_idx].nentries--;

				/* 从哈希桶链中移除该记录。 */
				*prevBucketPtr = currBucket->link;

				/* 把该记录加入相应的空闲列表。 */
				currBucket->link = hctl->freeList[freelist_idx].freeList;
				hctl->freeList[freelist_idx].freeList = currBucket;

				if (IS_PARTITIONED(hctl))
					SpinLockRelease(&hctl->freeList[freelist_idx].mutex);

				/*
				 * 最好盼调用者正同步对该元素的访问，
				 * 因为下次有人向表中添加内容时就会复用它
				 * 。
				 */
				return (void *) ELEMENTKEY(currBucket);
			}
			return NULL;

		case HASH_ENTER:
		case HASH_ENTER_NULL:
			/* 找到则返回现有元素，否则创建 */
			if (currBucket != NULL)
				return (void *) ELEMENTKEY(currBucket);

			/* 冻结时禁止插入 */
			if (hashp->frozen)
				elog(ERROR, "cannot insert into frozen hashtable \"%s\"",
					 hashp->tabname);

			currBucket = get_hash_entry(hashp, freelist_idx);
			if (currBucket == NULL)
			{
				/* 内存不足 */
				if (action == HASH_ENTER_NULL)
					return NULL;
				/* 报告通用消息 */
				if (hashp->isshared)
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of shared memory")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of memory")));
			}

			/* 链入哈希桶链 */
			*prevBucketPtr = currBucket;
			currBucket->link = NULL;

			/* 复制键到记录 */
			currBucket->hashvalue = hashvalue;
			hashp->keycopy(ELEMENTKEY(currBucket), keyPtr, keysize);

			/*
			 * 调用者返回时应填充数据；切勿在此插入任何
			 * 可能抛错的代码，否则表条目不完整而损坏调
			 * 用者的数据结构
			 * 。
			 */

			return (void *) ELEMENTKEY(currBucket);
	}

	elog(ERROR, "unrecognized hash action code: %d", (int) action);

	return NULL;				/* 安抚编译器 */
}

/*
 * hash_update_hash_key -- 更改现有表条目的哈希键
 *
 * 这等价于移除条目、创建新条目并复制其数据，只是
 * 该条目从不进入表的空闲列表。因此即使有其他进程
 * 在哈希表的其他分区中操作，这也不会遭遇内存耗尽
 * 失败。
 *
 * 成功则返回真；所请求的新哈希键已存在则返回假。
 * 如果指定的条目指针并非表成员则抛出错误
 * 。
 *
 * 注意：目前对旧新哈希键相同的情况没有特殊处理，
 * 这意味着这种情况会报告假。对现有用途而言这是
 * 更好的。
 *
 * 注意：分区哈希表中，若新哈希键将属于另一分区，
 * 则调用者必须持有两个相关分区上的锁。
 */
bool
hash_update_hash_key(HTAB *hashp,
					 void *existingEntry,
					 const void *newKeyPtr)
{
	HASHELEMENT *existingElement = ELEMENT_FROM_KEY(existingEntry);
	HASHHDR    *hctl = hashp->hctl;
	uint32		newhashvalue;
	Size		keysize;
	uint32		bucket;
	uint32		newbucket;
	long		segment_num;
	long		segment_ndx;
	HASHSEGMENT segp;
	HASHBUCKET	currBucket;
	HASHBUCKET *prevBucketPtr;
	HASHBUCKET *oldPrevPtr;
	HashCompareFunc match;

#ifdef HASH_STATISTICS
	hash_accesses++;
	hctl->accesses++;
#endif

	/* 冻结时禁止更新 */
	if (hashp->frozen)
		elog(ERROR, "cannot update in frozen hashtable \"%s\"",
			 hashp->tabname);

	/*
	 * 以保存的哈希值查找现有元素：这是为了能将它从
	 * 哈希链上摘下，但附带的好处是可以验证传入的
	 * 现有条目指针是否有效。
	 */
	bucket = calc_bucket(hctl, existingElement->hashvalue);

	segment_num = bucket >> hashp->sshift;
	segment_ndx = MOD(bucket, hashp->ssize);

	segp = hashp->dir[segment_num];

	if (segp == NULL)
		hash_corrupted(hashp);

	prevBucketPtr = &segp[segment_ndx];
	currBucket = *prevBucketPtr;

	while (currBucket != NULL)
	{
		if (currBucket == existingElement)
			break;
		prevBucketPtr = &(currBucket->link);
		currBucket = *prevBucketPtr;
	}

	if (currBucket == NULL)
		elog(ERROR, "hash_update_hash_key argument is not in hashtable \"%s\"",
			 hashp->tabname);

	oldPrevPtr = prevBucketPtr;

	/*
	 * 现在执行与 HASH_ENTER 等价的操作，以定位我们要放
	 * 入该条目的哈希链。
	 */
	newhashvalue = hashp->hash(newKeyPtr, hashp->keysize);

	newbucket = calc_bucket(hctl, newhashvalue);

	segment_num = newbucket >> hashp->sshift;
	segment_ndx = MOD(newbucket, hashp->ssize);

	segp = hashp->dir[segment_num];

	if (segp == NULL)
		hash_corrupted(hashp);

	prevBucketPtr = &segp[segment_ndx];
	currBucket = *prevBucketPtr;

	/*
	 * 沿冲突链查找匹配的键
	 */
	match = hashp->match;		/* 内循环省一次取值 */
	keysize = hashp->keysize;	/* 同 */

	while (currBucket != NULL)
	{
		if (currBucket->hashvalue == newhashvalue &&
			match(ELEMENTKEY(currBucket), newKeyPtr, keysize) == 0)
			break;
		prevBucketPtr = &(currBucket->link);
		currBucket = *prevBucketPtr;
#ifdef HASH_STATISTICS
		hash_collisions++;
		hctl->collisions++;
#endif
	}

	if (currBucket != NULL)
		return false;			/* 与现有条目冲突 */

	currBucket = existingElement;

	/*
	 * 若新旧哈希值同桶，链链接不必改也不应改，
	 * 因为 currBucket 是最后一个元素时这种简单更新会
	 * 损坏链表。（但不能提前退出：还需要扫描该桶
	 * 来检查是否有重复键。
	 * ）
	 */
	if (bucket != newbucket)
	{
		/* 可以从旧哈希桶链中移除该记录了。 */
		*oldPrevPtr = currBucket->link;

		/* 链入新哈希桶链 */
		*prevBucketPtr = currBucket;
		currBucket->link = NULL;
	}

	/* 把新键复制进记录 */
	currBucket->hashvalue = newhashvalue;
	hashp->keycopy(ELEMENTKEY(currBucket), newKeyPtr, keysize);

	/* 记录其余部分不动 */

	return true;
}

/*
 * 如可能则分配新的哈希表条目；内存耗尽则返回 NULL。
 * （若底层空间分配器对内存耗尽抛错，则根本不会
 * 返回。）
 */
static HASHBUCKET
get_hash_entry(HTAB *hashp, int freelist_idx)
{
	HASHHDR    *hctl = hashp->hctl;
	HASHBUCKET	newElement;

	for (;;)
	{
		/* 分区时必须加锁才能动 nentries 和 freeList */
		if (IS_PARTITIONED(hctl))
			SpinLockAcquire(&hctl->freeList[freelist_idx].mutex);

		/* 尝试从空闲列表取条目 */
		newElement = hctl->freeList[freelist_idx].freeList;

		if (newElement != NULL)
			break;

		if (IS_PARTITIONED(hctl))
			SpinLockRelease(&hctl->freeList[freelist_idx].mutex);

		/*
		 * 此空闲列表无空闲元素。分区表的其他空闲列表
		 * 可能还有条目，但为减少争用我们先向主共享
		 * 内存分配器再要一块桶。但如果失败，我们*必
		 * 须*先翻遍所有其他空闲列表才能放弃。有多个
		 * 调用者假定：初始请求的表大小中的每个元素都
		 * 分配，或删除一个元素就能保证插入新元素，
		 * 哪怕共享内存完全占满。
		 * 因为所需元素在别的空闲列表里而失败，这种
		 * 情形是不能接受的
		 * 。
		 */
		if (!element_alloc(hashp, hctl->nelem_alloc, freelist_idx))
		{
			int			borrow_from_idx;

			if (!IS_PARTITIONED(hctl))
				return NULL;	/* 内存不足 */

			/* 尝试从其他空闲列表借元素 */
			borrow_from_idx = freelist_idx;
			for (;;)
			{
				borrow_from_idx = (borrow_from_idx + 1) % NUM_FREELISTS;
				if (borrow_from_idx == freelist_idx)
					break;		/* 空闲列表查遍，失败 */

				SpinLockAcquire(&(hctl->freeList[borrow_from_idx].mutex));
				newElement = hctl->freeList[borrow_from_idx].freeList;

				if (newElement != NULL)
				{
					hctl->freeList[borrow_from_idx].freeList = newElement->link;
					SpinLockRelease(&(hctl->freeList[borrow_from_idx].mutex));

					/* 注意：把新元素计入其所属的空闲列表 */
					SpinLockAcquire(&hctl->freeList[freelist_idx].mutex);
					hctl->freeList[freelist_idx].nentries++;
					SpinLockRelease(&hctl->freeList[freelist_idx].mutex);

					return newElement;
				}

				SpinLockRelease(&(hctl->freeList[borrow_from_idx].mutex));
			}

			/* 也没有可借的元素了，所以内存不足 */
			return NULL;
		}
	}

	/* 移除空闲列表条目，增 nentries */
	hctl->freeList[freelist_idx].freeList = newElement->link;
	hctl->freeList[freelist_idx].nentries++;

	if (IS_PARTITIONED(hctl))
		SpinLockRelease(&hctl->freeList[freelist_idx].mutex);

	return newElement;
}

/*
 * hash_get_num_entries -- 获取哈希表中的条目数
 */
long
hash_get_num_entries(HTAB *hashp)
{
	int			i;
	long		sum = hashp->hctl->freeList[0].nentries;

	/*
	 * 目前不获取互斥锁；只有持有全部分区锁时
	 * 调用此函数才有意义
	 * 。
	 */
	if (IS_PARTITIONED(hashp->hctl))
	{
		for (i = 1; i < NUM_FREELISTS; i++)
			sum += hashp->hctl->freeList[i].nentries;
	}

	return sum;
}

/*
 * hash_seq_init/_search/_term
 *			顺序扫描哈希表，逐个返回元素，
 *			没有更多时返回 NULL。
 *
 * 只有当扫描在中途被放弃时才应调用 hash_seq_term；如果
 * hash_seq_search 返回了 NULL，则结束扫描的清理工作已
 * 经完成。
 *
 * 注意：调用者可以在继续扫描之前删除返回的元素。
 * 但在扫描进行期间删除任何其他元素是未定义行为
 * （它可能正是 curIndex 所指！）。此外，如果扫
 * 描进行期间有元素加入表中，扫描是否会访问它们并
 * 没有定论。
 *
 * 注意：若先用 hash_freeze 锁表禁止再插入，则可放心
 * 使用 hash_seq_init/hash_seq_search 而无需担心
 * hash_seq_term 的清理。
 *
 * 注意：在分区表上使用时，调用者最好全程持有该表
 * 的全部分区至少持有共享锁！本后端自己的插入或删
 * 除，但*不能*应付其他后端的并
 * 发插入或删除。
 */
void
hash_seq_init(HASH_SEQ_STATUS *status, HTAB *hashp)
{
	status->hashp = hashp;
	status->curBucket = 0;
	status->curEntry = NULL;
	if (!hashp->frozen)
		register_seq_scan(hashp);
}

void *
hash_seq_search(HASH_SEQ_STATUS *status)
{
	HTAB	   *hashp;
	HASHHDR    *hctl;
	uint32		max_bucket;
	long		ssize;
	long		segment_num;
	long		segment_ndx;
	HASHSEGMENT segp;
	uint32		curBucket;
	HASHELEMENT *curElem;

	if ((curElem = status->curEntry) != NULL)
	{
		/* 继续扫描 curBucket…… */
		status->curEntry = curElem->link;
		if (status->curEntry == NULL)	/* 本桶结束 */
			++status->curBucket;
		return (void *) ELEMENTKEY(curElem);
	}

	/*
	 * 从 curBucket 开始搜索下一个非空桶
	 */
	curBucket = status->curBucket;
	hashp = status->hashp;
	hctl = hashp->hctl;
	ssize = hashp->ssize;
	max_bucket = hctl->max_bucket;

	if (curBucket > max_bucket)
	{
		hash_seq_term(status);
		return NULL;			/* 搜索完成 */
	}

	/*
	 * 先在表目录中找到正确的段
	 */
	segment_num = curBucket >> hashp->sshift;
	segment_ndx = MOD(curBucket, ssize);

	segp = hashp->dir[segment_num];

	/*
	 * 取桶链首项。非空即搜索，否则继续前进
	 * 下一个非空桶。由于搜索近空表要大量迭代
	 * 此循环，我们尝试对该情形做些优化，以便
	 * 加速这一过程。
	 */
	while ((curElem = segp[segment_ndx]) == NULL)
	{
		/* 空桶，前进到下一个 */
		if (++curBucket > max_bucket)
		{
			status->curBucket = curBucket;
			hash_seq_term(status);
			return NULL;		/* 搜索完成 */
		}
		if (++segment_ndx >= ssize)
		{
			segment_num++;
			segment_ndx = 0;
			segp = hashp->dir[segment_num];
		}
	}

	/* 开始扫描 curBucket… */
	status->curEntry = curElem->link;
	if (status->curEntry == NULL)	/* 本桶结束 */
		++curBucket;
	status->curBucket = curBucket;
	return (void *) ELEMENTKEY(curElem);
}

void
hash_seq_term(HASH_SEQ_STATUS *status)
{
	if (!status->hashp->frozen)
		deregister_seq_scan(status->hashp);
}

/*
 * hash_freeze
 *			冻结哈希表使其不能再插入（删除仍允
 *			许）。
 *
 * 原因：阻止后续桶分裂后，就不再需要登记
 * hash_seq_search 扫描，调用者也无须小心确保
 * hash_seq_term 的调用时机
 * 。
 *
 * hash_freeze() 可重复调用，但有活动扫描的表不能冻
 * 结（因为那样 hash_seq_term 会做错事）。
 */
void
hash_freeze(HTAB *hashp)
{
	if (hashp->isshared)
		elog(ERROR, "cannot freeze shared hashtable \"%s\"", hashp->tabname);
	if (!hashp->frozen && has_seq_scans(hashp))
		elog(ERROR, "cannot freeze hashtable \"%s\" because it has active scans",
			 hashp->tabname);
	hashp->frozen = true;
}


/********************************* 工具 ************************/

/*
 * 通过增加一个哈希桶来扩展表。
 */
static bool
expand_table(HTAB *hashp)
{
	HASHHDR    *hctl = hashp->hctl;
	HASHSEGMENT old_seg,
				new_seg;
	long		old_bucket,
				new_bucket;
	long		new_segnum,
				new_segndx;
	long		old_segnum,
				old_segndx;
	HASHBUCKET *oldlink,
			   *newlink;
	HASHBUCKET	currElement,
				nextElement;

	Assert(!IS_PARTITIONED(hctl));

#ifdef HASH_STATISTICS
	hash_expansions++;
#endif

	new_bucket = hctl->max_bucket + 1;
	new_segnum = new_bucket >> hashp->sshift;
	new_segndx = MOD(new_bucket, hashp->ssize);

	if (new_segnum >= hctl->nsegs)
	{
		/* 必要时分配新段 —— 目录满则可能失败 */
		if (new_segnum >= hctl->dsize)
			if (!dir_realloc(hashp))
				return false;
		if (!(hashp->dir[new_segnum] = seg_alloc(hashp)))
			return false;
		hctl->nsegs++;
	}

	/* 已创建新桶 */
	hctl->max_bucket++;

	/*
	 * 改掩码前，先找到同哈希值的旧桶，值或须迁往
	 * 新桶。注意此时 new_bucket 必然大于 low_mask，
	 * 所以可以跳过常规哈希掩码计算的第一步
	 * 。
	 */
	old_bucket = (new_bucket & hctl->low_mask);

	/*
	 * 跨过 2 的幂时重新调整掩码
	 */
	if ((uint32) new_bucket > hctl->high_mask)
	{
		hctl->low_mask = hctl->high_mask;
		hctl->high_mask = (uint32) new_bucket | hctl->low_mask;
	}

	/*
	 * 把记录迁往新桶。注意：由于 calc_bucket 掩码
	 * 的做法此刻只有一个旧桶需要分裂。换别的
	 * 哈希值归约方法，那可能就不成立
	 * ！
	 */
	old_segnum = old_bucket >> hashp->sshift;
	old_segndx = MOD(old_bucket, hashp->ssize);

	old_seg = hashp->dir[old_segnum];
	new_seg = hashp->dir[new_segnum];

	oldlink = &old_seg[old_segndx];
	newlink = &new_seg[new_segndx];

	for (currElement = *oldlink;
		 currElement != NULL;
		 currElement = nextElement)
	{
		nextElement = currElement->link;
		if ((long) calc_bucket(hctl, currElement->hashvalue) == old_bucket)
		{
			*oldlink = currElement;
			oldlink = &currElement->link;
		}
		else
		{
			*newlink = currElement;
			newlink = &currElement->link;
		}
	}
	/* 别忘了终止重建的哈希链…… */
	*oldlink = NULL;
	*newlink = NULL;

	return true;
}


static bool
dir_realloc(HTAB *hashp)
{
	HASHSEGMENT *p;
	HASHSEGMENT *old_p;
	long		new_dsize;
	long		old_dirsize;
	long		new_dirsize;

	if (hashp->hctl->max_dsize != NO_MAX_DSIZE)
		return false;

	/* 重新分配目录 */
	new_dsize = hashp->hctl->dsize << 1;
	old_dirsize = hashp->hctl->dsize * sizeof(HASHSEGMENT);
	new_dirsize = new_dsize * sizeof(HASHSEGMENT);

	old_p = hashp->dir;
	CurrentDynaHashCxt = hashp->hcxt;
	p = (HASHSEGMENT *) hashp->alloc((Size) new_dirsize);

	if (p != NULL)
	{
		memcpy(p, old_p, old_dirsize);
		MemSet(((char *) p) + old_dirsize, 0, new_dirsize - old_dirsize);
		hashp->dir = p;
		hashp->hctl->dsize = new_dsize;

		/* XXX 假定分配器是 palloc，故知如何释放 */
		Assert(hashp->alloc == DynaHashAlloc);
		pfree(old_p);

		return true;
	}

	return false;
}


static HASHSEGMENT
seg_alloc(HTAB *hashp)
{
	HASHSEGMENT segp;

	CurrentDynaHashCxt = hashp->hcxt;
	segp = (HASHSEGMENT) hashp->alloc(sizeof(HASHBUCKET) * hashp->ssize);

	if (!segp)
		return NULL;

	MemSet(segp, 0, sizeof(HASHBUCKET) * hashp->ssize);

	return segp;
}

/*
 * 分配一些新元素并把它们链入指定的空闲列表
 */
static bool
element_alloc(HTAB *hashp, int nelem, int freelist_idx)
{
	HASHHDR    *hctl = hashp->hctl;
	Size		elementSize;
	HASHELEMENT *firstElement;
	HASHELEMENT *tmpElement;
	HASHELEMENT *prevElement;
	int			i;

	if (hashp->isfixed)
		return false;

	/* 每个元素有 HASHELEMENT 头外加用户数据。 */
	elementSize = MAXALIGN(sizeof(HASHELEMENT)) + MAXALIGN(hctl->entrysize);

	CurrentDynaHashCxt = hashp->hcxt;
	firstElement = (HASHELEMENT *) hashp->alloc(nelem * elementSize);

	if (!firstElement)
		return false;

	/* 准备把所有新条目链入空闲列表 */
	prevElement = NULL;
	tmpElement = firstElement;
	for (i = 0; i < nelem; i++)
	{
		tmpElement->link = prevElement;
		prevElement = tmpElement;
		tmpElement = (HASHELEMENT *) (((char *) tmpElement) + elementSize);
	}

	/* 分区时必须加锁才能动 freeList */
	if (IS_PARTITIONED(hctl))
		SpinLockAcquire(&hctl->freeList[freelist_idx].mutex);

	/* 若两个后端并发执行，空闲列表可能非空 */
	firstElement->link = hctl->freeList[freelist_idx].freeList;
	hctl->freeList[freelist_idx].freeList = prevElement;

	if (IS_PARTITIONED(hctl))
		SpinLockRelease(&hctl->freeList[freelist_idx].mutex);

	return true;
}

/* 检测到哈希表损坏时报错 */
static void
hash_corrupted(HTAB *hashp)
{
	/*
	 * 共享表损坏，就须强制全系统重启；否则
	 * 只需关闭这一个后端。
	 */
	if (hashp->isshared)
		elog(PANIC, "hash table \"%s\" corrupted", hashp->tabname);
	else
		elog(FATAL, "hash table \"%s\" corrupted", hashp->tabname);
}

/* 计算 num 的 ceil(log2) */
int
my_log2(long num)
{
	/*
	 * 防范过大输入，否则 pg_ceil_log2_*() 会
	 * 无效
	 */
	if (num > LONG_MAX / 2)
		num = LONG_MAX / 2;

#if SIZEOF_LONG < 8
	return pg_ceil_log2_32(num);
#else
	return pg_ceil_log2_64(num);
#endif
}

/* 计算 >= num 的第一个 2 的幂，限 long 可容纳范围 */
static long
next_pow2_long(long num)
{
	/* my_log2 内部的范围检查已足够 */
	return 1L << my_log2(num);
}

/* 计算 >= num 的第一个 2 的幂，限 int 可容纳范围 */
static int
next_pow2_int(long num)
{
	if (num > INT_MAX / 2)
		num = INT_MAX / 2;
	return 1 << my_log2(num);
}


/************************* 扫描跟踪 ************************/

/*
 * 我们在这里跟踪活动的 hash_seq_search 扫描。需要
 * 这一机制的原因是：扫描进行中发生桶分裂会让扫
 * 描混乱：可能重访两次，或漏掉一些（若正扫到
 * 分裂桶中间）。故表上有活动扫描时要阻止对正
 * 在插入的表进行桶分裂。
 * 这在当前用法中相当罕见，因此只需把分裂推迟
 * 到下次插入时似乎就够了。
 *
 * 就现有用法而言，并发打开的扫描不会多，固
 * 定大小的打开扫描栈就够用了；也不担心线性搜
 * 索太慢。注意我们确实允许对同一哈希表并发打开
 * 多个扫描。
 *
 * 该机制可支持同一后端对共享表并发扫描和插入；
 * 别的后端会失败，但锁的原因本来就排除了这
 * 类情形，所以我们不担心
 * 。
 *
 * 若临时哈希表未通知我们即被删除，这种安排也相
 * 当健壮。最坏的情况是我们可能阻止了另一张后来
 * 恰在同一地址创建的表阻止分裂。事务结束时我
 * 们会就引用泄漏告警，故导致漏通知的缺陷应该很
 * 容易抓住。
 */

#define MAX_SEQ_SCANS 100

static HTAB *seq_scan_tables[MAX_SEQ_SCANS];	/* 正被扫描的表 */
static int	seq_scan_level[MAX_SEQ_SCANS];	/* 子事务嵌套层级 */
static int	num_seq_scans = 0;


/* 把表登记为有活动的 hash_seq_search 扫描 */
static void
register_seq_scan(HTAB *hashp)
{
	if (num_seq_scans >= MAX_SEQ_SCANS)
		elog(ERROR, "too many active hash_seq_search scans, cannot start one on \"%s\"",
			 hashp->tabname);
	seq_scan_tables[num_seq_scans] = hashp;
	seq_scan_level[num_seq_scans] = GetCurrentTransactionNestLevel();
	num_seq_scans++;
}

/* 注销一个活动扫描 */
static void
deregister_seq_scan(HTAB *hashp)
{
	int			i;

	/* 向后搜索，因为它最可能在栈顶 */
	for (i = num_seq_scans - 1; i >= 0; i--)
	{
		if (seq_scan_tables[i] == hashp)
		{
			seq_scan_tables[i] = seq_scan_tables[num_seq_scans - 1];
			seq_scan_level[i] = seq_scan_level[num_seq_scans - 1];
			num_seq_scans--;
			return;
		}
	}
	elog(ERROR, "no hash_seq_search scan for hash table \"%s\"",
		 hashp->tabname);
}

/* 检查某表是否有活动扫描 */
static bool
has_seq_scans(HTAB *hashp)
{
	int			i;

	for (i = 0; i < num_seq_scans; i++)
	{
		if (seq_scan_tables[i] == hashp)
			return true;
	}
	return false;
}

/* 事务结束时清理所有打开的扫描 */
void
AtEOXact_HashTables(bool isCommit)
{
	/*
	 * 中止清理时开着的扫描属正常，静默清理即可
	 * 提交时有开着的扫描说明有人漏了 hash_seq_term()
	 * 故要抱怨。
	 *
	 * 注意：想打印 tabname，但怕碰到已释放内存，反
	 * 正不是面向用户的消息，所以
	 * 无需好看。
	 */
	if (isCommit)
	{
		int			i;

		for (i = 0; i < num_seq_scans; i++)
		{
			elog(WARNING, "leaked hash_seq_search scan for hash table %p",
				 seq_scan_tables[i]);
		}
	}
	num_seq_scans = 0;
}

/* 子事务结束时清理所有打开的扫描 */
void
AtEOSubXact_HashTables(bool isCommit, int nestDepth)
{
	int			i;

	/*
	 * 向后搜以便清理；须查所有条目，不只尾部，因
	 * 删除手法不保证条目保持有序，所以
	 * 如此。
	 */
	for (i = num_seq_scans - 1; i >= 0; i--)
	{
		if (seq_scan_level[i] >= nestDepth)
		{
			if (isCommit)
				elog(WARNING, "leaked hash_seq_search scan for hash table %p",
					 seq_scan_tables[i]);
			seq_scan_tables[i] = seq_scan_tables[num_seq_scans - 1];
			seq_scan_level[i] = seq_scan_level[num_seq_scans - 1];
			num_seq_scans--;
		}
	}
}
