/*-------------------------------------------------------------------------
 *
 * shmem.c
 *	  创建共享内存并初始化共享内存数据结构。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/shmem.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * POSTGRES 进程共享一片或多片共享内存区域。
 * 共享内存由 postmaster 创建，并由每个后端经由
 * fork()（或在某些移植上经由其他操作系统特定方法）
 * 继承。本文件中的例程用于分配共享内存数据结构
 * 并与之绑定。
 *
 * NOTES：
 *		(a) POSTGRES 可用的共享内存数据结构有三种：
 *	定长结构、队列和哈希表。定长结构包含
 *	模块全局变量之类的东西，在共享内存初始化
 *	阶段之后绝不应再分配。哈希表有固定的最大
 *	大小，但其实际大小可以动态变化。向表中加入
 *	条目时会分配更多空间。队列把已经分配的
 *	数据结构链接起来，这些结构要么位于定长结构
 *	之内，要么作为哈希桶。每个共享数据结构都有
 *	一个字符串名字来标识它（由声明它的模块
 *	指定）。
 *
 *		(b) 初始化期间，每个模块在称为
 *	"Shmem Index" 的哈希表中查找自己的共享数据结构。
 *	若该数据结构不存在，调用方可以分配一个新的
 *	并初始化它。若该数据结构存在，调用方通过
 *	在本地地址空间中初始化一个指针来 "挂接"
 *	到该结构。
 *		共享内存索引有两个用途：首先，它给出
 *	后端进程初始化时世界看起来的
 *	简单模型。
 *	若某物存在于共享内存索引中，它就是已初始化的。
 *	若不在，就是未初始化的。其次，共享内存索引
 *	让我们可以按需分配共享内存，而不必试图预先
 *	分配结构并在头文件里写死大小和位置。若你在
 *	很多不同的地方使用大量共享内存（并且在开发
 *	期间不断改动），这一点就很重要。
 *
 *		(c) 在标准的 Unix 式环境中，各个后端不需要
 *	重新建立它们指向共享内存的本地指针，因为它们
 *	经由 fork() 从 postmaster 继承了这些变量的正确值。
 *	然而这在 EXEC_BACKEND 情形下行不通。在使用
 *	EXEC_BACKEND 的移植上，新后端必须用上面 (b)
 *	所述方法建立自己的本地指针。
 *
 *		(d) 内存分配模型：共享内存一旦分配就不再释放。
 *	每张哈希表有自己的空闲链表，因此删除一个项时
 *	哈希桶可以重用。然而，若一张哈希表长得很大
 *	然后又缩小，它的空间无法再分配给其他表。
 *	需要的话我们可以做一个简单的哈希桶
 *	垃圾收集器。目前看来
 *	没有必要。
 */

#include "postgres.h"

#include "access/transam.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/builtins.h"

static void *ShmemAllocRaw(Size size, Size *allocated_size);

/* 共享内存全局变量 */

static PGShmemHeader *ShmemSegHdr;	/* 共享内存段头 */

static void *ShmemBase;			/* 共享内存的起始地址 */

static void *ShmemEnd;			/* 共享内存的末尾加一地址 */

slock_t    *ShmemLock;			/* 用于共享内存与 LWLock
								 * 分配的自旋锁 */

static HTAB *ShmemIndex = NULL; /* 共享内存的主索引哈希表 */


/*
 *	InitShmemAccess() --- 建立指向共享内存的基本指针。
 *
 * 注意：参数本应声明为 "PGShmemHeader *seghdr"，
 * 但我们使用 void，以免不得不在 shmem.h 中包含 ipc.h。
 */
void
InitShmemAccess(void *seghdr)
{
	PGShmemHeader *shmhdr = (PGShmemHeader *) seghdr;

	ShmemSegHdr = shmhdr;
	ShmemBase = (void *) shmhdr;
	ShmemEnd = (char *) ShmemBase + shmhdr->totalsize;
}

/*
 *	InitShmemAllocation() --- 建立共享内存空间分配。
 *
 * 只应在 postmaster 或独立后端中调用。
 */
void
InitShmemAllocation(void)
{
	PGShmemHeader *shmhdr = ShmemSegHdr;
	char	   *aligned;

	Assert(shmhdr != NULL);

	/*
	 * 初始化 ShmemAlloc 使用的自旋锁。必须使用
	 * ShmemAllocUnlocked，因为显然还无法调用 ShmemAlloc。
	 */
	ShmemLock = (slock_t *) ShmemAllocUnlocked(sizeof(slock_t));

	SpinLockInit(ShmemLock);

	/*
	 * 此后的分配都应经过 ShmemAlloc，它期望
	 * 一切都按缓存行边界分配。确保第一次分配
	 * 从缓存行边界开始。
	 */
	aligned = (char *)
		(CACHELINEALIGN((((char *) shmhdr) + shmhdr->freeoffset)));
	shmhdr->freeoffset = aligned - (char *) shmhdr;

	/* ShmemIndex 还无法建立（需要先有 LWLocks） */
	shmhdr->index = NULL;
	ShmemIndex = (HTAB *) NULL;

	/*
	 * 为事务管理器初始化 ShmemVariableCache。（这其实
	 * 不该放在这里，但不值得挪走。）
	 */
	ShmemVariableCache = (VariableCache)
		ShmemAlloc(sizeof(*ShmemVariableCache));
	memset(ShmemVariableCache, 0, sizeof(*ShmemVariableCache));
}

/*
 * ShmemAlloc -- 从共享内存分配按最大对齐的块
 *
 * 若请求无法满足则抛出错误。
 *
 * 假定 ShmemLock 与 ShmemSegHdr 已经初始化。
 */
void *
ShmemAlloc(Size size)
{
	void	   *newSpace;
	Size		allocated_size;

	newSpace = ShmemAllocRaw(size, &allocated_size);
	if (!newSpace)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory (%zu bytes requested)",
						size)));
	return newSpace;
}

/*
 * ShmemAllocNoError -- 从共享内存分配按最大对齐的块
 *
 * 与 ShmemAlloc 相同，但空间不足时返回 NULL，而不是报错。
 */
void *
ShmemAllocNoError(Size size)
{
	Size		allocated_size;

	return ShmemAllocRaw(size, &allocated_size);
}

/*
 * ShmemAllocRaw -- 分配对齐的块并返回已分配大小
 *
 * 同时把 *allocated_size 设为分配的字节数，它等于
 * 请求的字节数加上我们选择添加的任何填充。
 */
static void *
ShmemAllocRaw(Size size, Size *allocated_size)
{
	Size		newStart;
	Size		newFree;
	void	   *newSpace;

	/*
	 * 确保所有空间充分对齐。过去我们只对这块
	 * 空间做 MAXALIGN，但经验证明在现代系统上
	 * 这不够好。系统的许多部分对关键数据结构
	 * 被拆到缓存行边界两侧非常敏感。为避免这一点，
	 * 尝试把分配的起点对齐到缓存行边界。
	 * 调用代码仍然需要小心使用所分配的空间
	 * ——例如把结构数组的每个元素填充到
	 * 二的幂大小——但没有这一步，即便那样
	 * 也不够。
	 */
	size = CACHELINEALIGN(size);
	*allocated_size = size;

	Assert(ShmemSegHdr != NULL);

	SpinLockAcquire(ShmemLock);

	newStart = ShmemSegHdr->freeoffset;

	newFree = newStart + size;
	if (newFree <= ShmemSegHdr->totalsize)
	{
		newSpace = (void *) ((char *) ShmemBase + newStart);
		ShmemSegHdr->freeoffset = newFree;
	}
	else
		newSpace = NULL;

	SpinLockRelease(ShmemLock);

	/* 注意：newSpace == NULL 时这个断言也没问题 */
	Assert(newSpace == (void *) CACHELINEALIGN(newSpace));

	return newSpace;
}

/*
 * ShmemAllocUnlocked -- 从共享内存分配按最大对齐的块
 *
 * 不加 ShmemLock 锁分配空间。只应用于、并且只应用于
 * 必须发生在 ShmemLock 就绪之前的分配。
 *
 * 这里认为最大对齐就够了，不必做缓存行对齐。
 */
void *
ShmemAllocUnlocked(Size size)
{
	Size		newStart;
	Size		newFree;
	void	   *newSpace;

	/*
	 * 确保所分配的空间充分对齐。
	 */
	size = MAXALIGN(size);

	Assert(ShmemSegHdr != NULL);

	newStart = ShmemSegHdr->freeoffset;

	newFree = newStart + size;
	if (newFree > ShmemSegHdr->totalsize)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory (%zu bytes requested)",
						size)));
	ShmemSegHdr->freeoffset = newFree;

	newSpace = (void *) ((char *) ShmemBase + newStart);

	Assert(newSpace == (void *) MAXALIGN(newSpace));

	return newSpace;
}

/*
 * ShmemAddrIsValid -- 测试地址是否指向共享内存
 *
 * 若指针指向共享内存段之内则返回真。
 */
bool
ShmemAddrIsValid(const void *addr)
{
	return (addr >= ShmemBase) && (addr < ShmemEnd);
}

/*
 *	InitShmemIndex() --- 建立或挂接共享内存索引表。
 */
void
InitShmemIndex(void)
{
	HASHCTL		info;

	/*
	 * 创建共享内存的 shmem 索引。
	 *
	 * 因为 ShmemInitHash 调用 ShmemInitStruct，而后者期望
	 * ShmemIndex 哈希表已存在，初始化 ShmemIndex 本身
	 * 就有一点循环问题。特殊的 "ShmemIndex" 哈希表名
	 * 会告诉 ShmemInitStruct 先假装它已存在。
	 */
	info.keysize = SHMEM_INDEX_KEYSIZE;
	info.entrysize = sizeof(ShmemIndexEnt);

	ShmemIndex = ShmemInitHash("ShmemIndex",
							   SHMEM_INDEX_SIZE, SHMEM_INDEX_SIZE,
							   &info,
							   HASH_ELEM | HASH_STRINGS);
}

/*
 * ShmemInitHash -- 创建并初始化，或挂接到一个
 *		共享内存哈希表。
 *
 * 我们假定调用方做了某种同步，以免两个进程
 * 同时试图创建/初始化同一张表。（实践中，所有创建
 * 都在 postmaster 进程中完成；子进程应当总是
 * 挂接到已有的表。）
 *
 * max_size 是哈希表条目数的估计最大值。这不是
 * 硬限制，但若大幅超出，访问效率会下降
 * （因为它用来计算目录大小，哈希表的桶
 * 会过满）。
 *
 * init_size 是要预分配的哈希表条目数。对于最大大小
 * 确定的表，它应当等于 max_size；这样可以保证运行时
 * 不会发生共享内存不足的失败。
 *
 * *infoP 与 hash_flags 至少必须说明条目大小和键
 * 比较语义（见 hash_create()）。特定于共享内存哈希表的
 * 标志位和值在这里添加，但调用方可以选择指定
 * HASH_PARTITION 与/或 HASH_FIXED_SIZE。
 *
 * 注意：Postgres 9.0 之前，本函数在某些失败情况下返回
 * NULL。现在它总是改为抛出错误，因此调用方不必检查
 * NULL。
 */
HTAB *
ShmemInitHash(const char *name,		/* 共享内存索引用的表字符串名 */
			  long init_size,	/* 初始表大小 */
			  long max_size,	/* 表的最大大小 */
			  HASHCTL *infoP,	/* 关于键和桶大小的信息 */
			  int hash_flags)	/* 关于 infoP 的信息 */
{
	bool		found;
	void	   *location;

	/*
	 * 分配在共享内存中的哈希表有固定目录；它无法增长，
	 * 否则其他后端就找不到它。因此务必一开始就把它
	 * 做得足够大。
	 *
	 * 也必须指定共享内存分配器。
	 */
	infoP->dsize = infoP->max_dsize = hash_select_dirsize(max_size);
	infoP->alloc = ShmemAllocNoError;
	hash_flags |= HASH_SHARED_MEM | HASH_ALLOC | HASH_DIRSIZE;

	/* 在共享内存索引中查找它 */
	location = ShmemInitStruct(name,
							   hash_get_shared_size(infoP, hash_flags),
							   &found);

	/*
	 * 若它已存在，就挂接上去，而不是分配并初始化
	 * 新空间
	 */
	if (found)
		hash_flags |= HASH_ATTACH;

	/* 把哈希表头的位置传给 hash_create */
	infoP->hctl = (HASHHDR *) location;

	return hash_create(name, init_size, infoP, hash_flags);
}

/*
 * ShmemInitStruct -- 在共享内存中创建/挂接一个结构。
 *
 *		初始化期间调用，以查找或分配
 *		共享内存中的数据结构。若还没有其他进程
 *		创建该结构，本例程就为它分配空间。
 *		若已存在，则返回指向现有结构的
 *		指针。
 *
 *	返回：指向该对象的指针。若对象已在共享内存索引中
 *		（因而已经初始化），*foundPtr 被置为真。
 *
 *	注意：Postgres 9.0 之前，本函数在某些失败情况下
 *	返回 NULL。现在它总是改为抛出错误，因此调用方
 *	不必检查 NULL。
 */
void *
ShmemInitStruct(const char *name, Size size, bool *foundPtr)
{
	ShmemIndexEnt *result;
	void	   *structPtr;

	LWLockAcquire(ShmemIndexLock, LW_EXCLUSIVE);

	if (!ShmemIndex)
	{
		PGShmemHeader *shmemseghdr = ShmemSegHdr;

		/* 必定是在试图创建/挂接 ShmemIndex 本身 */
		Assert(strcmp(name, "ShmemIndex") == 0);

		if (IsUnderPostmaster)
		{
			/* 必定是在初始化一个（非独立的）后端 */
			Assert(shmemseghdr->index != NULL);
			structPtr = shmemseghdr->index;
			*foundPtr = true;
		}
		else
		{
			/*
			 * 若共享内存索引不存在，我们正在引导：必定是在
			 * 试图初始化共享内存索引本身。
			 *
			 * 注意 ShmemIndexLock 在共享内存索引初始化之前
			 * 就被释放了。这应当没问题，因为还没有其他进程
			 * 能够访问共享内存。
			 */
			Assert(shmemseghdr->index == NULL);
			structPtr = ShmemAlloc(size);
			shmemseghdr->index = structPtr;
			*foundPtr = false;
		}
		LWLockRelease(ShmemIndexLock);
		return structPtr;
	}

	/* 在共享内存索引中查找它 */
	result = (ShmemIndexEnt *)
		hash_search(ShmemIndex, name, HASH_ENTER_NULL, foundPtr);

	if (!result)
	{
		LWLockRelease(ShmemIndexLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create ShmemIndex entry for data structure \"%s\"",
						name)));
	}

	if (*foundPtr)
	{
		/*
		 * 结构已在共享内存索引中，所以别人已经分配了它。
		 * 大小最好与我们试图初始化的大小相同，否则就是
		 * 名字冲突（或者更糟）。
		 */
		if (result->size != size)
		{
			LWLockRelease(ShmemIndexLock);
			ereport(ERROR,
					(errmsg("ShmemIndex entry size is wrong for data structure"
							" \"%s\": expected %zu, actual %zu",
							name, size, result->size)));
		}
		structPtr = result->location;
	}
	else
	{
		Size		allocated_size;

		/* 它还不在表中。分配并初始化它 */
		structPtr = ShmemAllocRaw(size, &allocated_size);
		if (structPtr == NULL)
		{
			/* 内存不足；移除失败的 ShmemIndex 条目 */
			hash_search(ShmemIndex, name, HASH_REMOVE, NULL);
			LWLockRelease(ShmemIndexLock);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("not enough shared memory for data structure"
							" \"%s\" (%zu bytes requested)",
							name, size)));
		}
		result->size = size;
		result->allocated_size = allocated_size;
		result->location = structPtr;
	}

	LWLockRelease(ShmemIndexLock);

	Assert(ShmemAddrIsValid(structPtr));

	Assert(structPtr == (void *) CACHELINEALIGN(structPtr));

	return structPtr;
}


/*
 * 把两个 Size 值相加，并检查溢出
 */
Size
add_size(Size s1, Size s2)
{
	Size		result;

	result = s1 + s2;
	/* 这里假定 Size 是无符号类型... */
	if (result < s1 || result < s2)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested shared memory size overflows size_t")));
	return result;
}

/*
 * 把两个 Size 值相乘，并检查溢出
 */
Size
mul_size(Size s1, Size s2)
{
	Size		result;

	if (s1 == 0 || s2 == 0)
		return 0;
	result = s1 * s2;
	/* 这里假定 Size 是无符号类型... */
	if (result / s2 != s1)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested shared memory size overflows size_t")));
	return result;
}

/* 显示已分配共享内存的 SQL SRF */
Datum
pg_get_shmem_allocations(PG_FUNCTION_ARGS)
{
#define PG_GET_SHMEM_SIZES_COLS 4
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS hstat;
	ShmemIndexEnt *ent;
	Size		named_allocated = 0;
	Datum		values[PG_GET_SHMEM_SIZES_COLS];
	bool		nulls[PG_GET_SHMEM_SIZES_COLS];

	InitMaterializedSRF(fcinfo, 0);

	LWLockAcquire(ShmemIndexLock, LW_SHARED);

	hash_seq_init(&hstat, ShmemIndex);

	/* 输出所有已分配的条目 */
	memset(nulls, 0, sizeof(nulls));
	while ((ent = (ShmemIndexEnt *) hash_seq_search(&hstat)) != NULL)
	{
		values[0] = CStringGetTextDatum(ent->key);
		values[1] = Int64GetDatum((char *) ent->location - (char *) ShmemSegHdr);
		values[2] = Int64GetDatum(ent->size);
		values[3] = Int64GetDatum(ent->allocated_size);
		named_allocated += ent->allocated_size;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	/* 输出已分配但未经由共享内存索引计入的共享内存 */
	values[0] = CStringGetTextDatum("<anonymous>");
	nulls[1] = true;
	values[2] = Int64GetDatum(ShmemSegHdr->freeoffset - named_allocated);
	values[3] = values[2];
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	/* 输出尚未使用的共享内存 */
	nulls[0] = true;
	values[1] = Int64GetDatum(ShmemSegHdr->freeoffset);
	nulls[1] = false;
	values[2] = Int64GetDatum(ShmemSegHdr->totalsize - ShmemSegHdr->freeoffset);
	values[3] = values[2];
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	LWLockRelease(ShmemIndexLock);

	return (Datum) 0;
}
