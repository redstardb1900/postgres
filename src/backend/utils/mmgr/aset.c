/*-------------------------------------------------------------------------
 *
 * aset.c
 *	  分配集定义。
 *
 * AllocSet 是抽象内存上下文类型的标准实现。
 * type.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/aset.c
 *
 * NOTE:
 *	这是分配集例程的新实现（1999-02-05）。
 *	AllocSet...() 不再使用 OrderedSet...()。
 *	取而代之的是，它自行在一个块池中管理分配，
 *	把许多小分配合并进少数更大的块中。AllocSetFree()
 *	并不真正 free() 内存，只是把释放的区域加入某个
 *	列表，供 AllocSetAlloc() 稍后重用。所有内存块在
 *	AllocSetReset() 时被一次性 free()，也就是内存上下文
 *	销毁。
 *				Jan Wieck
 *
 *	来自 Tom Lane 的性能改进（99/08）：对极大请求
 *	大小，我们确实希望内存一经 pfree() 便交还给 free()，
 *	否则大量内存可能被拴在空闲链表条目中，
 *	这些条目可能永远不可用。当调用者反复 repalloc()
 *	把块越改越大尤为需要；旧方式下，块的先前实例
 *	直到 AllocSetReset() 之前都
 *	注定被浪费。
 *
 *	进一步的改进（00/12）：就当时的代码而言，介于
 *	"small" 与 "large" 之间的中段请求处理得非常低效，
 *	因为任何足够大的空闲区块都会被拿来满足请求，
 *	即使它比实际需要的要大得多。随着时间的推移，
 *	已分配区块中浪费的空间越来越多。为了修复，
 *	我们抛弃了中段行为：现在只把"small"的 2 的幂
 *	尺寸的区块当区块处理。"large"都转交给 malloc()。
 *	修改空闲链表数即可改变 small/large 边界。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_memorychunk.h"
#include "utils/memutils_internal.h"

/*--------------------
 * 空闲链表 k 保存 1 << (k + ALLOC_MINBITS) 的区块，
 * k = 0 .. ALLOCSET_NUM_FREELISTS-1。
 *
 * 空闲链表中的所有区块都具有 2 的幂大小。
 * 这提高了可回收性：可能浪费一些空间，但随着请求
 * 的发出与释放，浪费的空间应保持相当稳定。
 *
 * 对最后一条空闲链表过大的请求，改用从 malloc()
 * 分配专用的块来处理。该块仍有块头部与区块头部，
 * 但释放该区块时，我们会把整个块交还给 malloc()，
 * 而非放入我们的空闲链表。
 *
 * 注意：ALLOC_MINBITS 必须大到足以使
 * 1<<ALLOC_MINBITS 不小于 MAXALIGN，
 * 否则我们可能无法充分对齐最小的区块。
 * 在目前所有已知的机器上，8 字节对齐就足够了。这个
 * 8 字节下限也让我们能在区块内存本身中存储指向下一
 * 条空闲链表项。
 *
 * 按当前参数，8K 以内的请求都按区块对待，更大的请求
 * 则放入专用块。修改 ALLOCSET_NUM_FREELISTS 即可
 * 调整边界点；并同步调整 ALLOCSET_SEPARATE_THRESHOLD
 * （memutils.h 同名值）保持一致。（注意：maxBlockSize
 * 小时 allocChunkLimit 可小于 8K，以免浪费空间。）
 *--------------------
 */

#define ALLOC_MINBITS		3	/* 最小区块为 8 字节 */
#define ALLOCSET_NUM_FREELISTS	11
#define ALLOC_CHUNK_LIMIT	(1 << (ALLOCSET_NUM_FREELISTS-1+ALLOC_MINBITS))
/* 我们使用固定尺寸的最大区块大小 */
#define ALLOC_CHUNK_FRACTION	4
/* 允许区块至多为 maxBlockSize 的 1/4（较少开销） */

/*--------------------
 * 为分配集分配的第一个块，其大小为 initBlockSize。
 * 每当必须再分配一个块时，就把块大小加倍（如可能
 * 且不超过 maxBlockSize），以减轻 malloc()
 * 的簿记负担。
 *
 * 为容纳超大区块而分配的块不遵循此规则；它们的大小
 * 只取决于容纳该单个区块所需的实际大小。
 *
 * 此外，若指定 minContextSize，则首个块采用该大小，
 * 而下一个块则使用 initBlockSize。
 *--------------------
 */

#define ALLOC_BLOCKHDRSZ	MAXALIGN(sizeof(AllocBlockData))
#define ALLOC_CHUNKHDRSZ	sizeof(MemoryChunk)

typedef struct AllocBlockData *AllocBlock;	/* 前向引用 */

/*
 * AllocPointer
 *		可作为分配集成员的对齐指针。
 */
typedef void *AllocPointer;

/*
 * AllocFreeListLink
 *		释放内存时，若为区块大小维护空闲链表，那么我们
 *		会用 AllocFreeListLink 指向该 AllocSetContext 空闲链
 *		表中的当前条目，然后把给定的空闲链表元素设置为
 *		指向正被释放的区块。
 */
typedef struct AllocFreeListLink
{
	MemoryChunk *next;
} AllocFreeListLink;

/*
 * 为给定区块获取 AllocFreeListLink。分配尺寸总是
 * 至少为 sizeof(AllocFreeListLink)，所以重用指针自身
 * 来存空闲链表链接。
 */
#define GetFreeListLink(chkptr) \
	(AllocFreeListLink *) ((char *) (chkptr) + ALLOC_CHUNKHDRSZ)

/* 校验从区块头部取出的空闲链表索引 */
#define FreeListIdxIsValid(fidx) \
	((fidx) >= 0 && (fidx) < ALLOCSET_NUM_FREELISTS)

/* 根据空闲链表索引确定区块大小 */
#define GetChunkSizeFromFreeListIdx(fidx) \
	((((Size) 1) << ALLOC_MINBITS) << (fidx))

/*
 * AllocSetContext 是 MemoryContext 的标准实现。
 *
 * 注意：header.isReset 表示 AllocSetReset 无事可做。
 * 这与 aset 物理上为空（blocks 列表为空）不同，
 * 因为我们仍会持有一个保留块。它也不同于集合逻辑上
 * 为空，因为我们不打算去检测 pfree 最后一个活动区块
 * 的情况。
 */
typedef struct AllocSetContext
{
	MemoryContextData header;	/* 标准内存上下文字段 */
	/* 本上下文中已分配存储的信息： */
	AllocBlock	blocks;			/* 本集合的块列表头部 */
	MemoryChunk *freelist[ALLOCSET_NUM_FREELISTS];	/* 空闲区块表 */
	/* 本上下文的分配参数： */
	Size		initBlockSize;	/* 初始块大小 */
	Size		maxBlockSize;	/* 最大块大小 */
	Size		nextBlockSize;	/* 下次分配的块大小 */
	Size		allocChunkLimit;	/* 区块大小有效上限 */
	AllocBlock	keeper;			/* 重置时保留此块 */
	/* 本上下文可放入的空闲链表，若非候选则为 -1： */
	int			freeListIndex;	/* context_freelists[] 中的索引 */
} AllocSetContext;

typedef AllocSetContext *AllocSet;

/*
 * AllocBlock
 *		AllocBlock 是 aset.c 从 malloc() 获取的内存单位。
 *		其中包含一个或多个 MemoryChunk，后者是 palloc()
 *		请求且由 pfree() 释放的单位。MemoryChunk 无法单独
 *		交还 malloc()，而是由 pfree() 放入空闲链表，
 *		供下一个请求大小匹配的
 *		palloc() 重用。
 *
 *		AllocBlockData 是块的头部数据 --- 块内可用空间从
 *		下一个对齐边界处开始。
 */
typedef struct AllocBlockData
{
	AllocSet	aset;			/* 拥有此块的 aset */
	AllocBlock	prev;			/* aset 块列表中的前一块，若有 */
	AllocBlock	next;			/* aset 块列表中的后一块，若有 */
	char	   *freeptr;		/* 此块空闲空间的起点 */
	char	   *endptr;			/* 此块空间的终点 */
}			AllocBlockData;

/*
 * AllocPointerIsValid
 *		指针为有效分配指针时为真。
 */
#define AllocPointerIsValid(pointer) PointerIsValid(pointer)

/*
 * AllocSetIsValid
 *		为有效分配集时为真。
 */
#define AllocSetIsValid(set) \
	(PointerIsValid(set) && IsA(set, AllocSetContext))

/*
 * AllocBlockIsValid
 *		块为分配集的有效块时为真。
 */
#define AllocBlockIsValid(block) \
	(PointerIsValid(block) && AllocSetIsValid((block)->aset))

/*
 * 我们总是把外部区块存储在专用块上。这样从外部区块
 * 获取块就很容易，因为它始终是块上的第一个且唯一的
 * 区块。
 */
#define ExternalChunkGetBlock(chunk) \
	(AllocBlock) ((char *) chunk - ALLOC_BLOCKHDRSZ)

/*
 * 与其反复创建和删除内存上下文，我们不如把已释放
 * 的上下文保留在空闲链表中，以便稍后能以少量工作重
 * 新分发它们。放入空闲链表前，先重置上下文，使其
 * 只剩初始 malloc 区块而无其他区块。
 * 要成为空闲链表候选，必须具有与列表中其他上下文
 * 相同的 minContextSize/initBlockSize；但 maxBlockSize
 * 无关，因其不影响初始区块大小。
 *
 * 目前我们为 ALLOCSET_DEFAULT_SIZES 上下文提供一条
 * 空闲链表，为 ALLOCSET_SMALL_SIZES 上下文提供另一
 * 条；后者也适用于 ALLOCSET_START_SMALL_SIZES，
 *
 * 因为只有 maxBlockSize 不同。通常，我们按后进先出
 * 顺序重用空闲链表上下文，以期改善引用局部性。但
 * 如果列表中的上下文过多，我们更倾向于丢弃最近创建
 * 的上下文，以期保持进程内存映射紧凑。
 * 我们近似的做法：列表溢出时直接删除全部现有条目，
 * 因为大量分配上下文的查询多半会按与分配大致相反
 * 的顺序释放它们。
 *
 * 空闲链表中的上下文通过其 nextchild 指针链接。
 */
#define MAX_FREE_CONTEXTS 100	/* 空闲链表长度的上限 */

typedef struct AllocSetFreeList
{
	int			num_free;		/* 列表长度 */
	AllocSetContext *first_free;	/* 表头 */
} AllocSetFreeList;

/* context_freelists[0] 为默认参数，[1] 为小型参数 */
static AllocSetFreeList context_freelists[2] =
{
	{
		0, NULL
	},
	{
		0, NULL
	}
};


/* ----------
 * AllocSetFreeIndex -
 *
 *		按分配的大小算出其属于分配集
 *		哪条空闲区块链表。调用者必须已验证
 *		size <= ALLOC_CHUNK_LIMIT。
 * ----------
 */
static inline int
AllocSetFreeIndex(Size size)
{
	int			idx;

	if (size > (1 << ALLOC_MINBITS))
	{
		/*----------
		 * 此时我们必须计算 ceil(log2(size >> ALLOC_MINBITS))。
		 * 这与下式相同
		 *		pg_leftmost_one_pos32((size - 1) >> ALLOC_MINBITS) + 1
		 * 或等价于
		 *		pg_leftmost_one_pos32(size - 1) - ALLOC_MINBITS + 1
		 *
		 * 但对缺乏内建支持的平台，我们在此复制该逻辑，
		 * 从而可以进行一项额外的优化。可以合理地假定
		 * ALLOC_CHUNK_LIMIT 能放入 16 位，因此我们可以展开
		 * pg_leftmost_one_pos32 中按字节处理的循环，只处理
		 * 最后两字节。
		 *
		 * 是的，这个函数够热门，值得下这么
		 * 大功夫。
		 *----------
		 */
#ifdef HAVE_BITSCAN_REVERSE
		idx = pg_leftmost_one_pos32((uint32) size - 1) - ALLOC_MINBITS + 1;
#else
		uint32		t,
					tsize;

		/* 静态断言我们只有 16 位的输入值。 */
		StaticAssertDecl(ALLOC_CHUNK_LIMIT < (1 << 16),
						 "ALLOC_CHUNK_LIMIT must be less than 64kB");

		tsize = size - 1;
		t = tsize >> 8;
		idx = t ? pg_leftmost_one_pos[t] + 8 : pg_leftmost_one_pos[tsize];
		idx -= ALLOC_MINBITS - 1;
#endif

		Assert(idx < ALLOCSET_NUM_FREELISTS);
	}
	else
		idx = 0;

	return idx;
}


/*
 * 公共例程
 */


/*
 * AllocSetContextCreateInternal
 *		新建 AllocSet 上下文。
 *
 * parent：父上下文，顶层上下文则为 NULL
 * name：上下文名称（必须为静态分配）
 * minContextSize：上下文最小值
 * initBlockSize：初始分配块大小
 * maxBlockSize：最大分配块大小
 *
 * 大多数调用者应当使用 ALLOCSET_DEFAULT_SIZES 之类的
 * 宏来抽象大小参数。
 *
 * 注意：不要直接调用此函数；应通过包装宏
 * AllocSetContextCreate.
 */
MemoryContext
AllocSetContextCreateInternal(MemoryContext parent,
							  const char *name,
							  Size minContextSize,
							  Size initBlockSize,
							  Size maxBlockSize)
{
	int			freeListIndex;
	Size		firstBlockSize;
	AllocSet	set;
	AllocBlock	block;

	/* 确保 MemoryChunk 大小经 MAXALIGN 对齐 */
	StaticAssertDecl(ALLOC_CHUNKHDRSZ == MAXALIGN(ALLOC_CHUNKHDRSZ),
					 "sizeof(MemoryChunk) is not maxaligned");
	/* 检查是否有足够空间存储空闲链表链接 */
	StaticAssertDecl(sizeof(AllocFreeListLink) <= (1 << ALLOC_MINBITS),
					 "sizeof(AllocFreeListLink) larger than minimum allocation size");

	/*
	 * 首先校验分配参数。这些检查曾是常规的运行时测试
	 * 和 elog，但实践中 Assert 似乎已足够，因为没有人
	 * 会在运行时改变它们的参数。我们多少有些随意地
	 * 强制块大小至少为 1K。我们把最大块大小限制为
	 * MEMORYCHUNK_MAX_BLOCKOFFSET，因为 MemoryChunk 在
	 * 于寻址区块与所在块间偏移方面受此限制。如果块
	 * 比这更大，我们将无法为任何位于块内超出
	 * MEMORYCHUNK_MAX_BLOCKOFFSET 字节处的区块
	 * 存储它与块之间的偏移。
	 *
	 */
	Assert(initBlockSize == MAXALIGN(initBlockSize) &&
		   initBlockSize >= 1024);
	Assert(maxBlockSize == MAXALIGN(maxBlockSize) &&
		   maxBlockSize >= initBlockSize &&
		   AllocHugeSizeIsValid(maxBlockSize)); /* 必须可安全加倍 */
	Assert(minContextSize == 0 ||
		   (minContextSize == MAXALIGN(minContextSize) &&
			minContextSize >= 1024 &&
			minContextSize <= maxBlockSize));
	Assert(maxBlockSize <= MEMORYCHUNK_MAX_BLOCKOFFSET);

	/*
	 * 检查参数是否与某条可用空闲链表匹配。我们不
	 * 需要求 maxBlockSize 也匹配。
	 */
	if (minContextSize == ALLOCSET_DEFAULT_MINSIZE &&
		initBlockSize == ALLOCSET_DEFAULT_INITSIZE)
		freeListIndex = 0;
	else if (minContextSize == ALLOCSET_SMALL_MINSIZE &&
			 initBlockSize == ALLOCSET_SMALL_INITSIZE)
		freeListIndex = 1;
	else
		freeListIndex = -1;

	/*
	 * 若有合适的空闲链表条目，直接回收该上下文。
	 */
	if (freeListIndex >= 0)
	{
		AllocSetFreeList *freelist = &context_freelists[freeListIndex];

		if (freelist->first_free != NULL)
		{
			/* 移出空闲链表条目 */
			set = freelist->first_free;
			freelist->first_free = (AllocSet) set->header.nextchild;
			freelist->num_free--;

			/* 更新其 maxBlockSize；其余应当没问题 */
			set->maxBlockSize = maxBlockSize;

			/* 重初始化其头部，装入正确名称与父上下文 */
			MemoryContextCreate((MemoryContext) set,
								T_AllocSetContext,
								MCTX_ASET_ID,
								parent,
								name);

			((MemoryContext) set)->mem_allocated =
				set->keeper->endptr - ((char *) set);

			return (MemoryContext) set;
		}
	}

	/* 确定初始块的大小 */
	firstBlockSize = MAXALIGN(sizeof(AllocSetContext)) +
		ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
	if (minContextSize != 0)
		firstBlockSize = Max(firstBlockSize, minContextSize);
	else
		firstBlockSize = Max(firstBlockSize, initBlockSize);

	/*
	 * 分配初始块。与 aset.c 的其他块不同，它以上下文头部
	 * 开头，其块头部紧随其后。
	 */
	set = (AllocSet) malloc(firstBlockSize);
	if (set == NULL)
	{
		if (TopMemoryContext)
			MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while creating memory context \"%s\".",
						   name)));
	}

	/*
	 * 此处与 MemoryContextCreate 之间避免编写会失败的代码；
	 * 若在此区间内 ereport，我们会泄漏头部/初始块。
	 */

	/* 填写初始块的块头部 */
	block = (AllocBlock) (((char *) set) + MAXALIGN(sizeof(AllocSetContext)));
	block->aset = set;
	block->freeptr = ((char *) block) + ALLOC_BLOCKHDRSZ;
	block->endptr = ((char *) set) + firstBlockSize;
	block->prev = NULL;
	block->next = NULL;

	/* 把未分配空间标为 NOACCESS；块头部不动。 */
	VALGRIND_MAKE_MEM_NOACCESS(block->freeptr, block->endptr - block->freeptr);

	/* 把块记入块列表 */
	set->blocks = block;
	/* 标记此块在重置时不释放 */
	set->keeper = block;

	/* 完成填写上下文头部中 aset 专属的部分 */
	MemSetAligned(set->freelist, 0, sizeof(set->freelist));

	set->initBlockSize = initBlockSize;
	set->maxBlockSize = maxBlockSize;
	set->nextBlockSize = initBlockSize;
	set->freeListIndex = freeListIndex;

	/*
	 * 为此上下文算出分配区块大小上限。空闲链表数量
	 * 固定，它不能超过 ALLOC_CHUNK_LIMIT。若 maxBlockSize
	 * 较小，那么超过 maxBlockSize 或其相当比例的请求也应
	 * 按大区块对待。maxBlockSize 为 2 的幂的典型情形下
	 * 区块大小上限至多为 maxBlockSize 的 1/8，因此面对
	 * 全部都是最大区块大小的请求流，至多只会浪费
	 * 已分配空间的
	 * 1/8。
	 *
	 * 此外，allocChunkLimit不得超过ALLOCSET_SEPARATE_THRESHOLD。
	 */
	StaticAssertStmt(ALLOC_CHUNK_LIMIT == ALLOCSET_SEPARATE_THRESHOLD,
					 "ALLOC_CHUNK_LIMIT != ALLOCSET_SEPARATE_THRESHOLD");

	/*
	 * 确定区块在被单独分配整个 AllocBlock 之前的最大尺
	 * 寸。我们把该尺寸的绝对上限设为 ALLOC_CHUNK_LIMIT，
	 * 但会进一步削减它，使得最大尺寸的块上大约能容纳
	 * ALLOC_CHUNK_FRACTION 个这种大小的区块。（出于历史
	 * 原因，我们选择让 allocChunkLimit 保持 2 的幂而非
	 * 是把它算得正好能让 ALLOC_CHUNK_FRACTION 个区块恰好
	 * 放进最大的块中。）
	 */
	set->allocChunkLimit = ALLOC_CHUNK_LIMIT;
	while ((Size) (set->allocChunkLimit + ALLOC_CHUNKHDRSZ) >
		   (Size) ((maxBlockSize - ALLOC_BLOCKHDRSZ) / ALLOC_CHUNK_FRACTION))
		set->allocChunkLimit >>= 1;

	/* 最后，完成上下文创建中与类型无关的部分 */
	MemoryContextCreate((MemoryContext) set,
						T_AllocSetContext,
						MCTX_ASET_ID,
						parent,
						name);

	((MemoryContext) set)->mem_allocated = firstBlockSize;

	return (MemoryContext) set;
}

/*
 * AllocSetReset
 *		释放给定集合中已分配的全部内存。
 *
 * 实际上，此例程在做法上有一定余地。
 * 它应把所有已分配区块标为已释放，但不必交出集合
 * 拥有的全部资源。我们的实际做法是交出除
 * "keeper" 块（必须保留，因为它与上下文头部共用一个
 * malloc 区块）之外的全部资源。这样，当上下文在小量
 * 分配之后被反复重置时，我们不会反复折腾 malloc()，
 * 这是每元组上下文的典型行为。
 */
void
AllocSetReset(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block;
	Size		keepersize PG_USED_FOR_ASSERTS_ONLY;

	Assert(AllocSetIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* 释放前检查损坏与泄漏 */
	AllocSetCheck(context);
#endif

	/* 为下方断言记住保留块大小 */
	keepersize = set->keeper->endptr - ((char *) set);

	/* 清空空闲链表 */
	MemSetAligned(set->freelist, 0, sizeof(set->freelist));

	block = set->blocks;

	/* 新块列表将只含保留块 */
	set->blocks = set->keeper;

	while (block != NULL)
	{
		AllocBlock	next = block->next;

		if (block == set->keeper)
		{
			/* 重置该块，但不交还 malloc */
			char	   *datastart = ((char *) block) + ALLOC_BLOCKHDRSZ;

#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(datastart, block->freeptr - datastart);
#else
			/* wipe_mem() 本会完成此事 */
			VALGRIND_MAKE_MEM_NOACCESS(datastart, block->freeptr - datastart);
#endif
			block->freeptr = datastart;
			block->prev = NULL;
			block->next = NULL;
		}
		else
		{
			/* 常规情形，释放该块 */
			context->mem_allocated -= block->endptr - ((char *) block);

#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(block, block->freeptr - ((char *) block));
#endif
			free(block);
		}
		block = next;
	}

	Assert(context->mem_allocated == keepersize);

	/* 同时重置块大小分配序列 */
	set->nextBlockSize = set->initBlockSize;
}

/*
 * AllocSetDelete
 *		释放给定集合中已分配的全部内存，
 *		为删除该集合做准备。
 *
 * 与 AllocSetReset 不同，*必须*释放集合全部资源
 */
void
AllocSetDelete(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block = set->blocks;
	Size		keepersize PG_USED_FOR_ASSERTS_ONLY;

	Assert(AllocSetIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* 释放前检查损坏与泄漏 */
	AllocSetCheck(context);
#endif

	/* 为下方断言记住保留块大小 */
	keepersize = set->keeper->endptr - ((char *) set);

	/*
	 * 如果该上下文可成为某空闲链表的候选，则把它放入
	 * 链表而非销毁它。
	 */
	if (set->freeListIndex >= 0)
	{
		AllocSetFreeList *freelist = &context_freelists[set->freeListIndex];

		/*
		 * 重置该上下文（如有需要），使我们不再持有超出
		 * 初始 malloc 区块的内容。
		 */
		if (!context->isReset)
			MemoryContextResetOnly(context);

		/*
		 * 若空闲链表已满，直接丢弃其中已有条目。参见
		 * context_freelists[]的注释。
		 */
		if (freelist->num_free >= MAX_FREE_CONTEXTS)
		{
			while (freelist->first_free != NULL)
			{
				AllocSetContext *oldset = freelist->first_free;

				freelist->first_free = (AllocSetContext *) oldset->header.nextchild;
				freelist->num_free--;

				/* 剩下的就是释放头部/初始块 */
				free(oldset);
			}
			Assert(freelist->num_free == 0);
		}

		/* 把刚删除的上下文加入空闲链表。 */
		set->header.nextchild = (MemoryContext) freelist->first_free;
		freelist->first_free = set;
		freelist->num_free++;

		return;
	}

	/* 释放全部块，作为上下文头部一部分的保留块除外 */
	while (block != NULL)
	{
		AllocBlock	next = block->next;

		if (block != set->keeper)
			context->mem_allocated -= block->endptr - ((char *) block);

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(block, block->freeptr - ((char *) block));
#endif

		if (block != set->keeper)
			free(block);

		block = next;
	}

	Assert(context->mem_allocated == keepersize);

	/* 最后释放上下文头部，包括保留块 */
	free(set);
}

/*
 * AllocSetAlloc
 *		返回给定大小的已分配内存指针；若请求无法
 *		完成则返回 NULL；内存会被加入集合。
 *
 * 请求不得超过：
 *		MAXALIGN_DOWN(SIZE_MAX) - ALLOC_BLOCKHDRSZ - ALLOC_CHUNKHDRSZ
 * 调用者用的上限低得多。
 *
 * 注意：使用 valgrind 时，返回分配如何标记并不重要，
 * 因为 mcxt.c 会将其置为 UNDEFINED。有些路径会返回
 * 标记为 NOACCESS 的空间——AllocSetRealloc 当心！
 */
void *
AllocSetAlloc(MemoryContext context, Size size)
{
	AllocSet	set = (AllocSet) context;
	AllocBlock	block;
	MemoryChunk *chunk;
	int			fidx;
	Size		chunk_size;
	Size		blksize;

	Assert(AllocSetIsValid(set));

	/*
	 * 如果请求的大小超过区块的最大值，则为该请求分配
	 * 整个块。
	 */
	if (size > set->allocChunkLimit)
	{
#ifdef MEMORY_CONTEXT_CHECKING
		/* 确保有哨兵字节的空间 */
		chunk_size = MAXALIGN(size + 1);
#else
		chunk_size = MAXALIGN(size);
#endif

		blksize = chunk_size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
		block = (AllocBlock) malloc(blksize);
		if (block == NULL)
			return NULL;

		context->mem_allocated += blksize;

		block->aset = set;
		block->freeptr = block->endptr = ((char *) block) + blksize;

		chunk = (MemoryChunk *) (((char *) block) + ALLOC_BLOCKHDRSZ);

		/* 将 MemoryChunk 标记为外部管理 */
		MemoryChunkSetHdrMaskExternal(chunk, MCTX_ASET_ID);

#ifdef MEMORY_CONTEXT_CHECKING
		chunk->requested_size = size;
		/* 设置标记以捕获"未用"空间覆写 */
		Assert(size < chunk_size);
		set_sentinel(MemoryChunkGetPointer(chunk), size);
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* 用垃圾填充已分配空间 */
		randomize_mem((char *) MemoryChunkGetPointer(chunk), size);
#endif

		/*
		 * 把新块垫在当前活动分配块（若有）的下面，
		 * 这样就不会失去其中剩余空间的使用权。
		 */
		if (set->blocks != NULL)
		{
			block->prev = set->blocks;
			block->next = set->blocks->next;
			if (block->next)
				block->next->prev = block;
			set->blocks->next = block;
		}
		else
		{
			block->prev = NULL;
			block->next = NULL;
			set->blocks = block;
		}

		/* 确保填充字节均标为 NOACCESS。 */
		VALGRIND_MAKE_MEM_NOACCESS((char *) MemoryChunkGetPointer(chunk) + size,
								   chunk_size - size);

		/* 禁止访问区块头部。 */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

		return MemoryChunkGetPointer(chunk);
	}

	/*
	 * 请求足够小，可按区块对待。在空闲链表里查
	 * 是否存在可重用的空闲区块。如果找到，就把它从空
	 * 闲链表中移除，重新使其成为分配集的成员，并返回
	 * 其数据地址。
	 *
	 * 注意，这里不确保哨兵字节的空间。我们预期
	 * 相当大比例的分配请求本来就是 2 的幂大小。若
	 * 在 MEMORY_CONTEXT_CHECKING 构建中总留出哨兵字节
	 * 的空间，那么这类分配的内存需求
	 * 会翻倍。
	 */
	fidx = AllocSetFreeIndex(size);
	chunk = set->freelist[fidx];
	if (chunk != NULL)
	{
		AllocFreeListLink *link = GetFreeListLink(chunk);

		/* 允许访问区块头部。 */
		VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);

		Assert(fidx == MemoryChunkGetValue(chunk));

		/* 把此区块弹出空闲链表 */
		VALGRIND_MAKE_MEM_DEFINED(link, sizeof(AllocFreeListLink));
		set->freelist[fidx] = link->next;
		VALGRIND_MAKE_MEM_NOACCESS(link, sizeof(AllocFreeListLink));

#ifdef MEMORY_CONTEXT_CHECKING
		chunk->requested_size = size;
		/* 设置标记以捕获"未用"空间覆写 */
		if (size < GetChunkSizeFromFreeListIdx(fidx))
			set_sentinel(MemoryChunkGetPointer(chunk), size);
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* 用垃圾填充已分配空间 */
		randomize_mem((char *) MemoryChunkGetPointer(chunk), size);
#endif

		/* 确保填充字节均标为 NOACCESS。 */
		VALGRIND_MAKE_MEM_NOACCESS((char *) MemoryChunkGetPointer(chunk) + size,
								   GetChunkSizeFromFreeListIdx(fidx) - size);

		/* 禁止访问区块头部。 */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

		return MemoryChunkGetPointer(chunk);
	}

	/*
	 * 选择要分配的实际区块大小。
	 */
	chunk_size = GetChunkSizeFromFreeListIdx(fidx);
	Assert(chunk_size >= size);

	/*
	 * 如果当前活动分配块中有足够的空间，我们把区块放
	 * 入该块。否则必须新起一块。
	 */
	if ((block = set->blocks) != NULL)
	{
		Size		availspace = block->endptr - block->freeptr;

		if (availspace < (chunk_size + ALLOC_CHUNKHDRSZ))
		{
			/*
			 * 活动（顶层）块放不下本次分配，但仍有
			 * 可观空间。一旦压入块列表深处，
			 * 就不会再从它那里分配空间了。在那之前，
			 * 先把它的空闲空间切成区块，放入
			 * 集合的
			 * 空闲链表。
			 *
			 * 由于只有当块中剩余空间少于
			 * ALLOC_CHUNK_LIMIT 时，此循环至多迭代
			 * ALLOCSET_NUM_FREELISTS-1 次。
			 */
			while (availspace >= ((1 << ALLOC_MINBITS) + ALLOC_CHUNKHDRSZ))
			{
				AllocFreeListLink *link;
				Size		availchunk = availspace - ALLOC_CHUNKHDRSZ;
				int			a_fidx = AllocSetFreeIndex(availchunk);

				/*
				 * 多数情况下，我们会得到比所需更大一级的
				 * 空闲链表索引。例外情形是
				 * availchunk 恰好为 2 的幂时。
				 */
				if (availchunk != GetChunkSizeFromFreeListIdx(a_fidx))
				{
					a_fidx--;
					Assert(a_fidx >= 0);
					availchunk = GetChunkSizeFromFreeListIdx(a_fidx);
				}

				chunk = (MemoryChunk *) (block->freeptr);

				/* 准备初始化区块头部。 */
				VALGRIND_MAKE_MEM_UNDEFINED(chunk, ALLOC_CHUNKHDRSZ);
				block->freeptr += (availchunk + ALLOC_CHUNKHDRSZ);
				availspace -= (availchunk + ALLOC_CHUNKHDRSZ);

				/* 空闲链表索引存入 value 字段 */
				MemoryChunkSetHdrMask(chunk, block, a_fidx, MCTX_ASET_ID);
#ifdef MEMORY_CONTEXT_CHECKING
				chunk->requested_size = InvalidAllocSize;	/* 标记为空 */
#endif
				/* 把此区块压入空闲链表 */
				link = GetFreeListLink(chunk);

				VALGRIND_MAKE_MEM_DEFINED(link, sizeof(AllocFreeListLink));
				link->next = set->freelist[a_fidx];
				VALGRIND_MAKE_MEM_NOACCESS(link, sizeof(AllocFreeListLink));

				set->freelist[a_fidx] = chunk;
			}
			/* 标记需要创建新块 */
			block = NULL;
		}
	}

	/*
	 * 该创建新的常规（多区块）块了吗？
	 */
	if (block == NULL)
	{
		Size		required_size;

		/*
		 * 第一个这种块大小为 initBlockSize，后续每块
		 * 空间都翻倍，但不超过 maxBlockSize。
		 */
		blksize = set->nextBlockSize;
		set->nextBlockSize <<= 1;
		if (set->nextBlockSize > set->maxBlockSize)
			set->nextBlockSize = set->maxBlockSize;

		/*
		 * 如果 initBlockSize 小于 ALLOC_CHUNK_LIMIT，可能需
		 * 要更多空间，但保持 2 的幂。
		 */
		required_size = chunk_size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
		while (blksize < required_size)
			blksize <<= 1;

		/* 尝试分配它 */
		block = (AllocBlock) malloc(blksize);

		/*
		 * 请求的块可能相当大，因此要应对 malloc 失败
		 * 的情况。但若只剩大约 1 MB 或更少，就放弃……
		 */
		while (block == NULL && blksize > 1024 * 1024)
		{
			blksize >>= 1;
			if (blksize < required_size)
				break;
			block = (AllocBlock) malloc(blksize);
		}

		if (block == NULL)
			return NULL;

		context->mem_allocated += blksize;

		block->aset = set;
		block->freeptr = ((char *) block) + ALLOC_BLOCKHDRSZ;
		block->endptr = ((char *) block) + blksize;

		/* 未分配空间标 NOACCESS。 */
		VALGRIND_MAKE_MEM_NOACCESS(block->freeptr,
								   blksize - ALLOC_BLOCKHDRSZ);

		block->prev = NULL;
		block->next = set->blocks;
		if (block->next)
			block->next->prev = block;
		set->blocks = block;
	}

	/*
	 * 好，执行分配
	 */
	chunk = (MemoryChunk *) (block->freeptr);

	/* 准备初始化区块头部。 */
	VALGRIND_MAKE_MEM_UNDEFINED(chunk, ALLOC_CHUNKHDRSZ);

	block->freeptr += (chunk_size + ALLOC_CHUNKHDRSZ);
	Assert(block->freeptr <= block->endptr);

	/* 把空闲链表索引存入值字段 */
	MemoryChunkSetHdrMask(chunk, block, fidx, MCTX_ASET_ID);

#ifdef MEMORY_CONTEXT_CHECKING
	chunk->requested_size = size;
	/* 设置标记以捕获"未用"空间覆写 */
	if (size < chunk_size)
		set_sentinel(MemoryChunkGetPointer(chunk), size);
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
	/* 用垃圾填充已分配空间 */
	randomize_mem((char *) MemoryChunkGetPointer(chunk), size);
#endif

	/* 确保填充字节均标为 NOACCESS。 */
	VALGRIND_MAKE_MEM_NOACCESS((char *) MemoryChunkGetPointer(chunk) + size,
							   chunk_size - size);

	/* 禁止访问区块头部。 */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

	return MemoryChunkGetPointer(chunk);
}

/*
 * AllocSetFree
 *		释放已分配内存；内存从集合中移除。
 */
void
AllocSetFree(void *pointer)
{
	AllocSet	set;
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);

	/* 允许访问区块头部。 */
	VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
	{
		/* 释放单区块块。 */
		AllocBlock	block = ExternalChunkGetBlock(chunk);

		/*
		 * 校验块指针是否正常：块头部应引用一个 aset，
		 * 且 freeptr 应与 endptr 一致。
		 */
		if (!AllocBlockIsValid(block) || block->freeptr != block->endptr)
			elog(ERROR, "could not find block containing chunk %p", chunk);

		set = block->aset;

#ifdef MEMORY_CONTEXT_CHECKING
		{
			/* 测试是否有人涂写了区块内未用空间 */
			Assert(chunk->requested_size < (block->endptr - (char *) pointer));
			if (!sentinel_ok(pointer, chunk->requested_size))
				elog(WARNING, "detected write past chunk end in %s %p",
					 set->header.name, chunk);
		}
#endif

		/* 好，把块从 aset 列表移除并释放 */
		if (block->prev)
			block->prev->next = block->next;
		else
			set->blocks = block->next;
		if (block->next)
			block->next->prev = block->prev;

		set->header.mem_allocated -= block->endptr - ((char *) block);

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(block, block->freeptr - ((char *) block));
#endif
		free(block);
	}
	else
	{
		AllocBlock	block = MemoryChunkGetBlock(chunk);
		int			fidx;
		AllocFreeListLink *link;

		/*
		 * 此路径中出于速度考虑只断言被引用的块是好的。
		 * 还可断言 value 字段正常。
		 * 未来实践经验或许会表明，这些断言最好成为
		 * 常规运行时 test-and-elog 检查。
		 */
		Assert(AllocBlockIsValid(block));
		set = block->aset;

		fidx = MemoryChunkGetValue(chunk);
		Assert(FreeListIdxIsValid(fidx));
		link = GetFreeListLink(chunk);

#ifdef MEMORY_CONTEXT_CHECKING
		/* 测试是否有人涂写了区块内未用空间 */
		if (chunk->requested_size < GetChunkSizeFromFreeListIdx(fidx))
			if (!sentinel_ok(pointer, chunk->requested_size))
				elog(WARNING, "detected write past chunk end in %s %p",
					 set->header.name, chunk);
#endif

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(pointer, GetChunkSizeFromFreeListIdx(fidx));
#endif
		/* 把此区块压入空闲链表顶部 */
		VALGRIND_MAKE_MEM_DEFINED(link, sizeof(AllocFreeListLink));
		link->next = set->freelist[fidx];
		VALGRIND_MAKE_MEM_NOACCESS(link, sizeof(AllocFreeListLink));
		set->freelist[fidx] = chunk;

#ifdef MEMORY_CONTEXT_CHECKING

		/*
		 * 把空闲链表区块 requested_size 重置为 InvalidAllocSize。
		 * list.
		 */
		chunk->requested_size = InvalidAllocSize;
#endif
	}
}

/*
 * AllocSetRealloc
 *		返回给定大小的已分配内存新指针；若请求无法
 *		完成则返回 NULL；该内存会被加入集合。
 *		与给定指针关联的内存会被复制到新内存中，
 *		旧内存则被释放。
 *
 * 没有 MEMORY_CONTEXT_CHECKING 时，我们不知道
 * 旧请求的大小，使 Valgrind 客户端请求不够精确，
 * 可能造成假阴性。（原则上可用 VALGRIND_GET_VBITS()
 * 找回。）
 */
void *
AllocSetRealloc(void *pointer, Size size)
{
	AllocBlock	block;
	AllocSet	set;
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	Size		oldchksize;
	int			fidx;

	/* 允许访问区块头部。 */
	VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
	{
		/*
		 * 该区块必是作为单区块块分配的，用 realloc() 扩
		 * 大或缩小包含它的块，
		 * 尽量省空间。
		 */
		Size		chksize;
		Size		blksize;
		Size		oldblksize;

		block = ExternalChunkGetBlock(chunk);

		/*
		 * 校验块指针是否正常：块头部应引用一个 aset，
		 * 且 freeptr 应与 endptr 一致。
		 */
		if (!AllocBlockIsValid(block) || block->freeptr != block->endptr)
			elog(ERROR, "could not find block containing chunk %p", chunk);

		set = block->aset;

		oldchksize = block->endptr - (char *) pointer;

#ifdef MEMORY_CONTEXT_CHECKING
		/* 测试是否有人涂写了区块内未用空间 */
		Assert(chunk->requested_size < oldchksize);
		if (!sentinel_ok(pointer, chunk->requested_size))
			elog(WARNING, "detected write past chunk end in %s %p",
				 set->header.name, chunk);
#endif

#ifdef MEMORY_CONTEXT_CHECKING
		/* 确保有哨兵字节的空间 */
		chksize = MAXALIGN(size + 1);
#else
		chksize = MAXALIGN(size);
#endif

		/* 做 realloc */
		blksize = chksize + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
		oldblksize = block->endptr - ((char *) block);

		block = (AllocBlock) realloc(block, blksize);
		if (block == NULL)
		{
			/* 禁止访问区块头部。 */
			VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);
			return NULL;
		}

		/* 单独更新，以免 (oldblksize > blksize) 时下溢 */
		set->header.mem_allocated -= oldblksize;
		set->header.mem_allocated += blksize;

		block->freeptr = block->endptr = ((char *) block) + blksize;

		/* 块可能已被移动，更新各指针 */
		chunk = (MemoryChunk *) (((char *) block) + ALLOC_BLOCKHDRSZ);
		pointer = MemoryChunkGetPointer(chunk);
		if (block->prev)
			block->prev->next = block;
		else
			set->blocks = block;
		if (block->next)
			block->next->prev = block;

#ifdef MEMORY_CONTEXT_CHECKING
#ifdef RANDOMIZE_ALLOCATED_MEMORY

		/*
		 * 只有知道先前请求时才能随机化额外空间。使用
		 * Valgrind 时，randomize_mem() 还会把内存标为 UNDEFINED
		 */
		if (size > chunk->requested_size)
			randomize_mem((char *) pointer + chunk->requested_size,
						  size - chunk->requested_size);
#else

		/*
		 * 若为扩大，realloc() 会把新分配部分（从
		 * oldchksize 到 chksize）标为 UNDEFINED，但我们还需
		 * 要调整旧分配的尾部字节（从 chunk->requested_size
		 * 到 oldchksize），因为它们被标为 NOACCESS。
		 * 务必不要标记过多字节，以防 chunk->requested_size <
		 * size < oldchksize。
		 */
#ifdef USE_VALGRIND
		if (Min(size, oldchksize) > chunk->requested_size)
			VALGRIND_MAKE_MEM_UNDEFINED((char *) pointer + chunk->requested_size,
										Min(size, oldchksize) - chunk->requested_size);
#endif
#endif

		chunk->requested_size = size;
		/* 设置标记以捕获"未用"空间覆写 */
		Assert(size < chksize);
		set_sentinel(pointer, size);
#else							/* !MEMORY_CONTEXT_CHECKING */

		/*
		 * 可能需要调整旧分配字节的标记，因为其中一些
		 * 可能已被标为 NOACCESS。我们不知道旧区块大小中
		 * 有多少是请求大小；它可能小至 1 字节。我们必须
		 * 保守行事，把整个旧部分都标为 DEFINED。
		 * 务必不要标记超出新分配的内存，
		 * 以防它小于旧的分配。
		 */
		VALGRIND_MAKE_MEM_DEFINED(pointer, Min(size, oldchksize));
#endif

		/* 确保填充字节均标为 NOACCESS。 */
		VALGRIND_MAKE_MEM_NOACCESS((char *) pointer + size, chksize - size);

		/* 禁止访问区块头部。 */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

		return pointer;
	}

	block = MemoryChunkGetBlock(chunk);

	/*
	 * 此路径中出于速度考虑只断言被引用的块是好的。
	 * 还可断言 value 字段正常。
	 * 未来实践经验或许会表明，这些断言最好变成常规的
	 * test-and-elog 检查。
	 */
	Assert(AllocBlockIsValid(block));
	set = block->aset;

	fidx = MemoryChunkGetValue(chunk);
	Assert(FreeListIdxIsValid(fidx));
	oldchksize = GetChunkSizeFromFreeListIdx(fidx);

#ifdef MEMORY_CONTEXT_CHECKING
	/* 测试是否有人涂写了区块内未用空间 */
	if (chunk->requested_size < oldchksize)
		if (!sentinel_ok(pointer, chunk->requested_size))
			elog(WARNING, "detected write past chunk end in %s %p",
				 set->header.name, chunk);
#endif

	/*
	 * AllocSetAlloc() 中区块大小按 2 的幂对齐。已分配的
	 * 区域可能已 >= 新大小。（尤其是当
	 * 请求为缩小时，我们会从此处跳出。）
	 */
	if (oldchksize >= size)
	{
#ifdef MEMORY_CONTEXT_CHECKING
		Size		oldrequest = chunk->requested_size;

#ifdef RANDOMIZE_ALLOCATED_MEMORY
		/* 只有知道先前请求时才能填充额外空间 */
		if (size > oldrequest)
			randomize_mem((char *) pointer + oldrequest,
						  size - oldrequest);
#endif

		chunk->requested_size = size;

		/*
		 * 如果是扩大，把任何新可用的部分标为 UNDEFINED。
		 * 否则把过时的部分标为 NOACCESS。
		 */
		if (size > oldrequest)
			VALGRIND_MAKE_MEM_UNDEFINED((char *) pointer + oldrequest,
										size - oldrequest);
		else
			VALGRIND_MAKE_MEM_NOACCESS((char *) pointer + size,
									   oldchksize - size);

		/* 设置标记以捕获"未用"空间覆写 */
		if (size < oldchksize)
			set_sentinel(pointer, size);
#else							/* !MEMORY_CONTEXT_CHECKING */

		/*
		 * 我们没有信息判断是在扩大
		 * 还是缩小旧请求，因此保守地把整个
		 * 新分配标为 DEFINED。
		 */
		VALGRIND_MAKE_MEM_NOACCESS(pointer, oldchksize);
		VALGRIND_MAKE_MEM_DEFINED(pointer, size);
#endif

		/* 禁止访问区块头部。 */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

		return pointer;
	}
	else
	{
		/*
		 * 扩大小区块的情形。我们直接用蛮力，即分配
		 * 一个新区块并复制数据。由于已知现有数据不会很
		 * 大，这不会带来多大 memcpy 开销，不值得做得更
		 * 聪明。（我们一度尝试在能原地扩大时避免
		 * memcpy，但结果是在反复的 palloc/repalloc/pfree 循环
		 * 中表现极糟：最终被释放的区块会落到
		 * 错误的空闲链表，满足不了下一个初始 palloc
		 * 请求，于是无限期泄漏内存。参见 pgsql-hackers
		 * 邮件列表 2007-08-11 的归档。）
		 */
		AllocPointer newPointer;
		Size		oldsize;

		/* 分配新区块 */
		newPointer = AllocSetAlloc((MemoryContext) set, size);

		/* 若请求未完成则立即返回 */
		if (newPointer == NULL)
		{
			/* 禁止访问区块头部。 */
			VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);
			return NULL;
		}

		/*
		 * AllocSetAlloc() 可能返回仍处于 NOACCESS 状态的区域。
		 * 暂时改为 UNDEFINED；随后 memcpy() 会把旧分配的
		 * 已定义状态转移到新分配上。若知道旧的分配，
		 * 只复制那么多；否则把旧区块整体置为已定义，
		 * 以避免复制当前 NOACCESS 尾部字节
		 * 时出错。
		 */
		VALGRIND_MAKE_MEM_UNDEFINED(newPointer, size);
#ifdef MEMORY_CONTEXT_CHECKING
		oldsize = chunk->requested_size;
#else
		oldsize = oldchksize;
		VALGRIND_MAKE_MEM_DEFINED(pointer, oldsize);
#endif

		/* 转移现有数据（必放得下） */
		memcpy(newPointer, pointer, oldsize);

		/* 释放旧块 */
		AllocSetFree(pointer);

		return newPointer;
	}
}

/*
 * AllocSetGetChunkContext
 *		返回 'pointer' 所属的内存上下文。
 */
MemoryContext
AllocSetGetChunkContext(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	AllocBlock	block;
	AllocSet	set;

	/* 允许访问区块头部。 */
	VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
		block = ExternalChunkGetBlock(chunk);
	else
		block = (AllocBlock) MemoryChunkGetBlock(chunk);

	/* 禁止访问区块头部。 */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

	Assert(AllocBlockIsValid(block));
	set = block->aset;

	return &set->header;
}

/*
 * AllocSetGetChunkSpace
 *		给定当前已分配的区块，确定其占用的总空间
 *		（包括所有内存分配开销）。
 */
Size
AllocSetGetChunkSpace(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	int			fidx;

	/* 允许访问区块头部。 */
	VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
	{
		AllocBlock	block = ExternalChunkGetBlock(chunk);

		/* 禁止访问区块头部。 */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

		Assert(AllocBlockIsValid(block));

		return block->endptr - (char *) chunk;
	}

	fidx = MemoryChunkGetValue(chunk);
	Assert(FreeListIdxIsValid(fidx));

	/* 禁止访问区块头部。 */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

	return GetChunkSizeFromFreeListIdx(fidx) + ALLOC_CHUNKHDRSZ;
}

/*
 * AllocSetIsEmpty
 *		分配集是否没有已分配空间？
 */
bool
AllocSetIsEmpty(MemoryContext context)
{
	Assert(AllocSetIsValid(context));

	/*
	 * 目前只有当上下文是新的或刚重置过时才说它
	 * 为"空"。我们可以检查各空闲链表来确定所有空间
	 * 是否都已释放，但就现有用途而言不值得
	 * 麻烦。
	 */
	if (context->isReset)
		return true;
	return false;
}

/*
 * AllocSetStats
 *		计算一个分配集的内存消耗统计。
 *
 * printfunc：若非 NULL，则把可读统计字符串传给它。
 * passthru：把此指针透传给 printfunc。
 * totals：非 NULL 时把此上下文统计加进 *totals。
 * print_to_stderr：为真时打印到 stderr，否则 elog。
 */
void
AllocSetStats(MemoryContext context,
			  MemoryStatsPrintFunc printfunc, void *passthru,
			  MemoryContextCounters *totals, bool print_to_stderr)
{
	AllocSet	set = (AllocSet) context;
	Size		nblocks = 0;
	Size		freechunks = 0;
	Size		totalspace;
	Size		freespace = 0;
	AllocBlock	block;
	int			fidx;

	Assert(AllocSetIsValid(set));

	/* 上下文头部计入 totalspace */
	totalspace = MAXALIGN(sizeof(AllocSetContext));

	for (block = set->blocks; block != NULL; block = block->next)
	{
		nblocks++;
		totalspace += block->endptr - ((char *) block);
		freespace += block->endptr - block->freeptr;
	}
	for (fidx = 0; fidx < ALLOCSET_NUM_FREELISTS; fidx++)
	{
		Size		chksz = GetChunkSizeFromFreeListIdx(fidx);
		MemoryChunk *chunk = set->freelist[fidx];

		while (chunk != NULL)
		{
			AllocFreeListLink *link = GetFreeListLink(chunk);

			/* 允许访问区块头部。 */
			VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);
			Assert(MemoryChunkGetValue(chunk) == fidx);
			VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

			freechunks++;
			freespace += chksz + ALLOC_CHUNKHDRSZ;

			VALGRIND_MAKE_MEM_DEFINED(link, sizeof(AllocFreeListLink));
			chunk = link->next;
			VALGRIND_MAKE_MEM_NOACCESS(link, sizeof(AllocFreeListLink));
		}
	}

	if (printfunc)
	{
		char		stats_string[200];

		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu blocks; %zu free (%zu chunks); %zu used",
				 totalspace, nblocks, freespace, freechunks,
				 totalspace - freespace);
		printfunc(context, passthru, stats_string, print_to_stderr);
	}

	if (totals)
	{
		totals->nblocks += nblocks;
		totals->freechunks += freechunks;
		totals->totalspace += totalspace;
		totals->freespace += freespace;
	}
}


#ifdef MEMORY_CONTEXT_CHECKING

/*
 * AllocSetCheck
 *		遍历各区块并检查内存的一致性。
 *
 * 注意：以 WARNING 而非 ERROR 或 FATAL 报告错误。否则
 * 出问题时会陷入无限循环，因为 elog 清理要释放
 * 内存时会再次进入此例程！
 */
void
AllocSetCheck(MemoryContext context)
{
	AllocSet	set = (AllocSet) context;
	const char *name = set->header.name;
	AllocBlock	prevblock;
	AllocBlock	block;
	Size		total_allocated = 0;

	for (prevblock = NULL, block = set->blocks;
		 block != NULL;
		 prevblock = block, block = block->next)
	{
		char	   *bpoz = ((char *) block) + ALLOC_BLOCKHDRSZ;
		Size		blk_used = block->freeptr - bpoz;
		Size		blk_data = 0;
		Size		nchunks = 0;
		bool		has_external_chunk = false;

		if (set->keeper == block)
			total_allocated += block->endptr - ((char *) set);
		else
			total_allocated += block->endptr - ((char *) block);

		/*
		 * 空块 —— 空的只能是保留块
		 */
		if (!blk_used)
		{
			if (set->keeper != block)
				elog(WARNING, "problem in alloc set %s: empty block %p",
					 name, block);
		}

		/*
		 * 检查块头部字段
		 */
		if (block->aset != set ||
			block->prev != prevblock ||
			block->freeptr < bpoz ||
			block->freeptr > block->endptr)
			elog(WARNING, "problem in alloc set %s: corrupt header in block %p",
				 name, block);

		/*
		 * 区块遍历
		 */
		while (bpoz < block->freeptr)
		{
			MemoryChunk *chunk = (MemoryChunk *) bpoz;
			Size		chsize,
						dsize;

			/* 允许访问区块头部。 */
			VALGRIND_MAKE_MEM_DEFINED(chunk, ALLOC_CHUNKHDRSZ);

			if (MemoryChunkIsExternal(chunk))
			{
				chsize = block->endptr - (char *) MemoryChunkGetPointer(chunk); /* 对齐区块大小 */
				has_external_chunk = true;

				/* 确保此区块占用整个块 */
				if (chsize + ALLOC_CHUNKHDRSZ != blk_used)
					elog(WARNING, "problem in alloc set %s: bad single-chunk %p in block %p",
						 name, chunk, block);
			}
			else
			{
				int			fidx = MemoryChunkGetValue(chunk);

				if (!FreeListIdxIsValid(fidx))
					elog(WARNING, "problem in alloc set %s: bad chunk size for chunk %p in block %p",
						 name, chunk, block);

				chsize = GetChunkSizeFromFreeListIdx(fidx); /* 对齐区块大小 */

				/*
				 * 检查所存块偏移是否正确引用此
				 * 块。
				 */
				if (block != MemoryChunkGetBlock(chunk))
					elog(WARNING, "problem in alloc set %s: bad block offset for chunk %p in block %p",
						 name, chunk, block);
			}
			dsize = chunk->requested_size;	/* 真数据 */

			/* 已分配区块的 requested_size 必须 <= chsize */
			if (dsize != InvalidAllocSize && dsize > chsize)
				elog(WARNING, "problem in alloc set %s: req size > alloc size for chunk %p in block %p",
					 name, chunk, block);

			/* chsize 不得小于第一条空闲链表的大小 */
			if (chsize < (1 << ALLOC_MINBITS))
				elog(WARNING, "problem in alloc set %s: bad size %zu for chunk %p in block %p",
					 name, chsize, chunk, block);

			/*
			 * 检查已分配区块的填充空间是否被覆写。
			 */
			if (dsize != InvalidAllocSize && dsize < chsize &&
				!sentinel_ok(chunk, ALLOC_CHUNKHDRSZ + dsize))
				elog(WARNING, "problem in alloc set %s: detected write past chunk end in block %p, chunk %p",
					 name, block, chunk);

			/* 若区块已分配，禁止访问区块头部 */
			if (dsize != InvalidAllocSize)
				VALGRIND_MAKE_MEM_NOACCESS(chunk, ALLOC_CHUNKHDRSZ);

			blk_data += chsize;
			nchunks++;

			bpoz += ALLOC_CHUNKHDRSZ + chsize;
		}

		if ((blk_data + (nchunks * ALLOC_CHUNKHDRSZ)) != blk_used)
			elog(WARNING, "problem in alloc set %s: found inconsistent memory block %p",
				 name, block);

		if (has_external_chunk && nchunks > 1)
			elog(WARNING, "problem in alloc set %s: external chunk on non-dedicated block %p",
				 name, block);
	}

	Assert(total_allocated == context->mem_allocated);
}

#endif							/* MEMORY_CONTEXT_CHECKING */
