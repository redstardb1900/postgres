/*-------------------------------------------------------------------------
 *
 * generation.c
 *	  世代分配器的定义。
 *
 * Generation 是一种自定义的 MemoryContext 实现，针对寿命
 * 相近的区块场景。
 *
 * Portions Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/generation.c
 *
 *
 *	本内存上下文基于如下假设：区块大致按分配顺序释放
 *	（FIFO），或者成组释放而组内寿命相近（世代——
 *	上下文名称即源于此）。这在各种队列式使用场景中
 *	很常见，即元组被构造出来之后，
 *	经处理后即被丢弃。
 *
 *	此内存上下文用非常简单的方式进行空闲空间管理。
 *	它不用复杂全局空闲链表，让每块跟踪已分配
 *	及已释放区块数。当空闲区块数等于已分配数时，
 *	该块即被视为空。此时我们并不释放该块，而是尝试
 *	将其"回收"，即把它复用于新的分配。做法是把该块
 *	设置到上下文的 'freeblock' 字段中。若该字段已被其他
 *	空闲块占据，那么我们就直接把这个新变空的块交还
 *	给 malloc。
 *
 *	这种空闲块处理方式对于真正"先分配者先释放"的
 *	分配模式所需的 malloc/free 调用更少。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/ilist.h"
#include "port/pg_bitutils.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_memorychunk.h"
#include "utils/memutils_internal.h"


#define Generation_BLOCKHDRSZ	MAXALIGN(sizeof(GenerationBlock))
#define Generation_CHUNKHDRSZ	sizeof(MemoryChunk)

#define Generation_CHUNK_FRACTION	8

typedef struct GenerationBlock GenerationBlock; /* 前向引用 */

typedef void *GenerationPointer;

/*
 * GenerationContext 是一种简单内存上下文，不复用
 * 已分配区块，全空后释放块。
 */
typedef struct GenerationContext
{
	MemoryContextData header;	/* 标准内存上下文字段 */

	/* 世代上下文参数 */
	Size		initBlockSize;	/* 初始块大小 */
	Size		maxBlockSize;	/* 最大块大小 */
	Size		nextBlockSize;	/* 下次要分配的块大小 */
	Size		allocChunkLimit;	/* 区块大小有效上限 */

	GenerationBlock *block;		/* 当前（最近分配的）块，
								 * 刚释放了最近的块则为
								 * NULL */
	GenerationBlock *freeblock; /* 指向正被回收块的指针，
								 * 若无此块则为 NULL。 */
	GenerationBlock *keeper;	/* 保留此块以跨越重置 */
	dlist_head	blocks;			/* 块列表 */
} GenerationContext;

/*
 * GenerationBlock
 *		GenerationBlock 是 generation.c 从 malloc() 获取的内存
 *		单位。它包含零个或多个 MemoryChunk，后者是
 *		palloc() 请求的单位，并由 pfree() 释放。MemoryChunk 无法
 *		单独交还 malloc()，而是 pfree() 更新块的空闲计数器，
 *		当块内所有区块都空闲时，整块
 *		即可交还给 malloc()。
 *
 *		GenerationBlock 是块的头部数据 --- 块内可用空间从
 *		下一个对齐边界开始。
 */
struct GenerationBlock
{
	dlist_node	node;			/* 块的双向链表 */
	GenerationContext *context; /* 指回所属上下文的指针 */
	Size		blksize;		/* 本块的已分配大小 */
	int			nchunks;		/* 块中的区块数 */
	int			nfree;			/* 空闲区块数 */
	char	   *freeptr;		/* 本块空闲空间的起点 */
	char	   *endptr;			/* 本块空间的终点 */
};

/*
 * GenerationIsValid
 *		为有效世代集时为真。
 */
#define GenerationIsValid(set) \
	(PointerIsValid(set) && IsA(set, GenerationContext))

/*
 * GenerationBlockIsValid
 *		块为世代集的有效块时为真。
 */
#define GenerationBlockIsValid(block) \
	(PointerIsValid(block) && GenerationIsValid((block)->context))

/*
 * 外部区块总是存放在专用块上。这使得从外部区块取回
 * 其所在块很容易，因为该块总是块上的第一个也是唯一
 * 的区块。
 */
#define ExternalChunkGetBlock(chunk) \
	(GenerationBlock *) ((char *) chunk - Generation_BLOCKHDRSZ)

/* 内联辅助函数 */
static inline void GenerationBlockInit(GenerationContext *context,
									   GenerationBlock *block,
									   Size blksize);
static inline bool GenerationBlockIsEmpty(GenerationBlock *block);
static inline void GenerationBlockMarkEmpty(GenerationBlock *block);
static inline Size GenerationBlockFreeBytes(GenerationBlock *block);
static inline void GenerationBlockFree(GenerationContext *set,
									   GenerationBlock *block);


/*
 * 公共例程
 */


/*
 * GenerationContextCreate
 *		创建 Generation 上下文。
 *
 * parent：父上下文，顶层上下文时为 NULL
 * name：上下文名称（必须静态分配）
 * minContextSize：上下文最小值
 * initBlockSize：初始分配块大小
 * maxBlockSize：最大分配块大小
 */
MemoryContext
GenerationContextCreate(MemoryContext parent,
						const char *name,
						Size minContextSize,
						Size initBlockSize,
						Size maxBlockSize)
{
	Size		firstBlockSize;
	Size		allocSize;
	GenerationContext *set;
	GenerationBlock *block;

	/* 确保 MemoryChunk 大小经 MAXALIGN 对齐 */
	StaticAssertDecl(Generation_CHUNKHDRSZ == MAXALIGN(Generation_CHUNKHDRSZ),
					 "sizeof(MemoryChunk) is not maxaligned");

	/*
	 * 首先校验分配参数。Assert 似乎已足够，因为没有人会
	 * 在运行时改变它们的参数。我们多少有些随意地
	 * 强制块大小至少为 1K。我们把最大块大小限制为
	 * MEMORYCHUNK_MAX_BLOCKOFFSET，因为 MemoryChunk 在
	 * 寻址区块与所在块偏移方面受此限制。若把块做得
	 * 这更大，我们将无法为位于块内超出
	 * MEMORYCHUNK_MAX_BLOCKOFFSET 字节处的区块
	 * 存储它与块之间的
	 * 偏移。
	 */
	Assert(initBlockSize == MAXALIGN(initBlockSize) &&
		   initBlockSize >= 1024);
	Assert(maxBlockSize == MAXALIGN(maxBlockSize) &&
		   maxBlockSize >= initBlockSize &&
		   AllocHugeSizeIsValid(maxBlockSize)); /* 必须可安全翻倍 */
	Assert(minContextSize == 0 ||
		   (minContextSize == MAXALIGN(minContextSize) &&
			minContextSize >= 1024 &&
			minContextSize <= maxBlockSize));
	Assert(maxBlockSize <= MEMORYCHUNK_MAX_BLOCKOFFSET);

	/* 确定初始块的大小 */
	allocSize = MAXALIGN(sizeof(GenerationContext)) +
		Generation_BLOCKHDRSZ + Generation_CHUNKHDRSZ;
	if (minContextSize != 0)
		allocSize = Max(allocSize, minContextSize);
	else
		allocSize = Max(allocSize, initBlockSize);

	/*
	 * 分配初始块。与 generation.c 的其他块不同，它以
	 * 上下文头部开头，其块头部紧随其后。
	 */
	set = (GenerationContext *) malloc(allocSize);
	if (set == NULL)
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while creating memory context \"%s\".",
						   name)));
	}

	/*
	 * 此处与 MemoryContextCreate 之间避免编写会失败的代码；
	 * 若在此区间 ereport 会泄漏头部。
	 */
	dlist_init(&set->blocks);

	/* 填写初始块的块头部 */
	block = (GenerationBlock *) (((char *) set) + MAXALIGN(sizeof(GenerationContext)));
	/* 确定块大小并初始化 */
	firstBlockSize = allocSize - MAXALIGN(sizeof(GenerationContext));
	GenerationBlockInit(set, block, firstBlockSize);

	/* 把它加入块的双向链表 */
	dlist_push_head(&set->blocks, &block->node);

	/* 将其用作当前分配块 */
	set->block = block;

	/* 暂无空闲块 */
	set->freeblock = NULL;

	/* 标记此块在重置时不释放 */
	set->keeper = block;

	/* 填写 GenerationContext 专属头部字段 */
	set->initBlockSize = initBlockSize;
	set->maxBlockSize = maxBlockSize;
	set->nextBlockSize = initBlockSize;

	/*
	 * 为此上下文计算分配区块大小上限。
	 *
	 * 限制非专用区块的最大尺寸，使得最大尺寸的块上至
	 * 少能容纳 Generation_CHUNK_FRACTION 个此大小的区块。
	 * 我们还必须进一步限制此值，使其不超过
	 * MEMORYCHUNK_MAX_VALUE。非外部区块无法大于该值，
	 * 因为我们在调用 MemoryChunkSetHdrMask() 时把区块
	 * 大小存入其 'value' 字段。
	 */
	set->allocChunkLimit = Min(maxBlockSize, MEMORYCHUNK_MAX_VALUE);
	while ((Size) (set->allocChunkLimit + Generation_CHUNKHDRSZ) >
		   (Size) ((Size) (maxBlockSize - Generation_BLOCKHDRSZ) / Generation_CHUNK_FRACTION))
		set->allocChunkLimit >>= 1;

	/* 最后，完成上下文创建中类型无关的部分 */
	MemoryContextCreate((MemoryContext) set,
						T_GenerationContext,
						MCTX_GENERATION_ID,
						parent,
						name);

	((MemoryContext) set)->mem_allocated = firstBlockSize;

	return (MemoryContext) set;
}

/*
 * GenerationReset
 *		释放给定集合中已分配的全部内存。
 *
 * 代码只是释放上下文中的所有块——我们不保留任何
 * 保留块或类似的东西。
 */
void
GenerationReset(MemoryContext context)
{
	GenerationContext *set = (GenerationContext *) context;
	dlist_mutable_iter miter;

	Assert(GenerationIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* 释放前检查损坏与泄漏 */
	GenerationCheck(context);
#endif

	/*
	 * 将空闲块指针置为 NULL。必须在此之前调用
	 * GenerationBlockFree，该函数从不期望释放空闲
	 * 块。
	 */
	set->freeblock = NULL;

	dlist_foreach_modify(miter, &set->blocks)
	{
		GenerationBlock *block = dlist_container(GenerationBlock, node, miter.cur);

		if (block == set->keeper)
			GenerationBlockMarkEmpty(block);
		else
			GenerationBlockFree(set, block);
	}

	/* 设置它使新分配利用保留块 */
	set->block = set->keeper;

	/* 同时重置块大小分配序列 */
	set->nextBlockSize = set->initBlockSize;

	/* 确保 dlist 中只有 1 项 */
	Assert(!dlist_is_empty(&set->blocks));
	Assert(!dlist_has_next(&set->blocks, dlist_head_node(&set->blocks)));
}

/*
 * GenerationDelete
 *		释放在给定上下文中分配的全部内存。
 */
void
GenerationDelete(MemoryContext context)
{
	/* 重置以释放全部可释放 GenerationBlock */
	GenerationReset(context);
	/* 然后释放上下文头部与保留块 */
	free(context);
}

/*
 * GenerationAlloc
 *		返回给定大小的已分配内存指针，若无法完成
 *		则返回 NULL；内存会被加入集合。
 *
 * 请求不得超过：
 *		MAXALIGN_DOWN(SIZE_MAX) - Generation_BLOCKHDRSZ - Generation_CHUNKHDRSZ
 * 各调用方限制都低得多。
 *
 * 注意：使用 valgrind 时，返回分配如何标记并不重要，
 * 因为 mcxt.c 会将其设为 UNDEFINED。在某些路径中我们
 * 会返回标记为 NOACCESS 的空间——GenerationRealloc 当心！
 */
void *
GenerationAlloc(MemoryContext context, Size size)
{
	GenerationContext *set = (GenerationContext *) context;
	GenerationBlock *block;
	MemoryChunk *chunk;
	Size		chunk_size;
	Size		required_size;

	Assert(GenerationIsValid(set));

#ifdef MEMORY_CONTEXT_CHECKING
	/* 确保有哨兵字节的空间 */
	chunk_size = MAXALIGN(size + 1);
#else
	chunk_size = MAXALIGN(size);
#endif
	required_size = chunk_size + Generation_CHUNKHDRSZ;

	/* 是否为超大区块？若是，则分配专用块 */
	if (chunk_size > set->allocChunkLimit)
	{
		Size		blksize = required_size + Generation_BLOCKHDRSZ;

		block = (GenerationBlock *) malloc(blksize);
		if (block == NULL)
			return NULL;

		context->mem_allocated += blksize;

		/* 单个（已用）区块的块 */
		block->context = set;
		block->blksize = blksize;
		block->nchunks = 1;
		block->nfree = 0;

		/* 该块已完全满 */
		block->freeptr = block->endptr = ((char *) block) + blksize;

		chunk = (MemoryChunk *) (((char *) block) + Generation_BLOCKHDRSZ);

		/* 将 MemoryChunk 标记为外部管理 */
		MemoryChunkSetHdrMaskExternal(chunk, MCTX_GENERATION_ID);

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

		/* 把该块加入已分配块列表 */
		dlist_push_head(&set->blocks, &block->node);

		/* 确保填充字节均标为 NOACCESS。 */
		VALGRIND_MAKE_MEM_NOACCESS((char *) MemoryChunkGetPointer(chunk) + size,
								   chunk_size - size);

		/* 禁止访问区块头部。 */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);

		return MemoryChunkGetPointer(chunk);
	}

	/*
	 * 非超大区块。我们首先尝试利用当前块，但如果其中
	 * 空间不足，则不分配新块，而是查看 freeblock 是否
	 * 为空且有足够空间。若不然，我们还会用 keeper 块
	 * 同样尝试。keeper 块可能已变空，
	 * 若不在此显式尝试使用它，我们就没有其他办法让
	 * 它重新得到复用。
	 *
	 * 我们不希望在当前块填满之前就开始使用 freeblock，
	 * 否则在 FIFO 类工作负载中可能造成碎片化。只有当
	 * 这些块完全为空时，我们才切换到使用 freeblock 或
	 * keeper 块。如果不这样做，连续的分配可能被分散到
	 * 多个块上，而这个问题
	 * 会随时间不断累积。
	 */
	block = set->block;

	if (block == NULL ||
		GenerationBlockFreeBytes(block) < required_size)
	{
		Size		blksize;
		GenerationBlock *freeblock = set->freeblock;

		if (freeblock != NULL &&
			GenerationBlockIsEmpty(freeblock) &&
			GenerationBlockFreeBytes(freeblock) >= required_size)
		{
			block = freeblock;

			/*
			 * 将 freeblock 清零，因为下面会把它设为当前块
			 *
			 */
			set->freeblock = NULL;
		}
		else if (GenerationBlockIsEmpty(set->keeper) &&
				 GenerationBlockFreeBytes(set->keeper) >= required_size)
		{
			block = set->keeper;
		}
		else
		{
			/*
			 * 第一个这样的块大小为 initBlockSize，后续每个块
			 * 的空间都翻倍，但不超过 maxBlockSize。
			 */
			blksize = set->nextBlockSize;
			set->nextBlockSize <<= 1;
			if (set->nextBlockSize > set->maxBlockSize)
				set->nextBlockSize = set->maxBlockSize;

			/* 还需要块头部，故将其计入所需大小 */
			required_size += Generation_BLOCKHDRSZ;

			/* 将大小向上取整到 2 的幂 */
			if (blksize < required_size)
				blksize = pg_nextpower2_size_t(required_size);

			block = (GenerationBlock *) malloc(blksize);

			if (block == NULL)
				return NULL;

			context->mem_allocated += blksize;

			/* 初始化新块 */
			GenerationBlockInit(set, block, blksize);

			/* 把它加入块的双向链表 */
			dlist_push_head(&set->blocks, &block->node);

			/* 将 freeblock 清零，以防它已满 */
			set->freeblock = NULL;
		}

		/* 并将其用作当前分配块 */
		set->block = block;
	}

	/* 此时应有一块具有足够的空闲空间 */
	Assert(block != NULL);
	Assert((block->endptr - block->freeptr) >= Generation_CHUNKHDRSZ + chunk_size);

	chunk = (MemoryChunk *) block->freeptr;

	/* 准备初始化区块头部。 */
	VALGRIND_MAKE_MEM_UNDEFINED(chunk, Generation_CHUNKHDRSZ);

	block->nchunks += 1;
	block->freeptr += (Generation_CHUNKHDRSZ + chunk_size);

	Assert(block->freeptr <= block->endptr);

	MemoryChunkSetHdrMask(chunk, block, chunk_size, MCTX_GENERATION_ID);
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

	/* 确保填充字节均标为 NOACCESS。 */
	VALGRIND_MAKE_MEM_NOACCESS((char *) MemoryChunkGetPointer(chunk) + size,
							   chunk_size - size);

	/* 禁止访问区块头部。 */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);

	return MemoryChunkGetPointer(chunk);
}

/*
 * GenerationBlockInit
 *		按 'blksize' 初始化 'block'。不更新上下文的 mem_allocated
 *		字段。
 */
static inline void
GenerationBlockInit(GenerationContext *context, GenerationBlock *block,
					Size blksize)
{
	block->context = context;
	block->blksize = blksize;
	block->nchunks = 0;
	block->nfree = 0;

	block->freeptr = ((char *) block) + Generation_BLOCKHDRSZ;
	block->endptr = ((char *) block) + blksize;

	/* 未分配空间标 NOACCESS。 */
	VALGRIND_MAKE_MEM_NOACCESS(block->freeptr,
							   blksize - Generation_BLOCKHDRSZ);
}

/*
 * GenerationBlockIsEmpty
 *		当 'block' 不含任何区块时返回真
 */
static inline bool
GenerationBlockIsEmpty(GenerationBlock *block)
{
	return (block->nchunks == 0);
}

/*
 * GenerationBlockMarkEmpty
 *		将块标记为空。不释放该块。
 */
static inline void
GenerationBlockMarkEmpty(GenerationBlock *block)
{
#if defined(USE_VALGRIND) || defined(CLOBBER_FREED_MEMORY)
	char	   *datastart = ((char *) block) + Generation_BLOCKHDRSZ;
#endif

#ifdef CLOBBER_FREED_MEMORY
	wipe_mem(datastart, block->freeptr - datastart);
#else
	/* wipe_mem() 本会完成此事 */
	VALGRIND_MAKE_MEM_NOACCESS(datastart, block->freeptr - datastart);
#endif

	/* 重置块，但不交还 malloc */
	block->nchunks = 0;
	block->nfree = 0;
	block->freeptr = ((char *) block) + Generation_BLOCKHDRSZ;
}

/*
 * GenerationBlockFreeBytes
 *		返回 'block' 中空闲的字节数
 */
static inline Size
GenerationBlockFreeBytes(GenerationBlock *block)
{
	return (block->endptr - block->freeptr);
}

/*
 * GenerationBlockFree
 *		从 'set' 中移除 'block' 并释放其占用的内存。
 */
static inline void
GenerationBlockFree(GenerationContext *set, GenerationBlock *block)
{
	/* 确保无人试图释放保留块 */
	Assert(block != set->keeper);
	/* 我们也不应释放 freeblock */
	Assert(block != set->freeblock);

	/* 从块列表中释放该块 */
	dlist_delete(&block->node);

	((MemoryContext) set)->mem_allocated -= block->blksize;

#ifdef CLOBBER_FREED_MEMORY
	wipe_mem(block, block->blksize);
#endif

	free(block);
}

/*
 * GenerationFree
 *		更新块中区块数，若块内所有区块此时都已空闲，
 *		则丢弃该块。
 */
void
GenerationFree(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	GenerationBlock *block;
	GenerationContext *set;
#if (defined(MEMORY_CONTEXT_CHECKING) && defined(USE_ASSERT_CHECKING)) \
	|| defined(CLOBBER_FREED_MEMORY)
	Size		chunksize;
#endif

	/* 允许访问区块头部。 */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Generation_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
	{
		block = ExternalChunkGetBlock(chunk);

		/*
		 * 尝试核验块指针是否正常：块头部应引用一个
		 * generation 上下文。
		 */
		if (!GenerationBlockIsValid(block))
			elog(ERROR, "could not find block containing chunk %p", chunk);

#if (defined(MEMORY_CONTEXT_CHECKING) && defined(USE_ASSERT_CHECKING)) \
	|| defined(CLOBBER_FREED_MEMORY)
		chunksize = block->endptr - (char *) pointer;
#endif
	}
	else
	{
		block = MemoryChunkGetBlock(chunk);

		/*
		 * 在此路径中，出于速度考虑我们仅用 Assert 断言所
		 * 引用的块完好。日后实践可能表明，此
		 * Assert 宜改为常规运行时 test-and-elog 检查。
		 */
		Assert(GenerationBlockIsValid(block));

#if (defined(MEMORY_CONTEXT_CHECKING) && defined(USE_ASSERT_CHECKING)) \
	|| defined(CLOBBER_FREED_MEMORY)
		chunksize = MemoryChunkGetValue(chunk);
#endif
	}

#ifdef MEMORY_CONTEXT_CHECKING
	/* 测试是否有人涂写了区块内未用空间 */
	Assert(chunk->requested_size < chunksize);
	if (!sentinel_ok(pointer, chunk->requested_size))
		elog(WARNING, "detected write past chunk end in %s %p",
			 ((MemoryContext) block->context)->name, chunk);
#endif

#ifdef CLOBBER_FREED_MEMORY
	wipe_mem(pointer, chunksize);
#endif

#ifdef MEMORY_CONTEXT_CHECKING
	/* 已释放区块的 requested_size 置 InvalidAllocSize */
	chunk->requested_size = InvalidAllocSize;
#endif

	block->nfree += 1;

	Assert(block->nchunks > 0);
	Assert(block->nfree <= block->nchunks);

	/* 若块内仍有已分配区块，则到此为止。 */
	if (block->nfree < block->nchunks)
		return;

	set = block->context;

	/* 不要试图释放保留块，仅将其标记为空 */
	if (block == set->keeper)
	{
		GenerationBlockMarkEmpty(block);
		return;
	}

	/*
	 * 若尚未设置 freeblock，或本块就是 freeblock，则不
	 * 释放这块内存，而是将其保留下来，让新分配有机会
	 * 回收它。
	 */
	if (set->freeblock == NULL || set->freeblock == block)
	{
		/* XXX 是否只应回收 maxBlockSize 大小的块？ */
		set->freeblock = block;
		GenerationBlockMarkEmpty(block);
		return;
	}

	/* 同时确保该块未被标记为当前块。 */
	if (set->block == block)
		set->block = NULL;

	/*
	 * 该块已空，把它处理掉。先从块列表中移除它，
	 * 然后将其交还 malloc()。
	 */
	dlist_delete(&block->node);

	set->header.mem_allocated -= block->blksize;
	free(block);
}

/*
 * GenerationRealloc
 *		处理 repalloc 时，我们仅分配新区块，复制数据
 *		并丢弃旧块。唯一例外是新尺寸能装进旧区块——
 *		此时只更新区块头部。
 */
void *
GenerationRealloc(void *pointer, Size size)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	GenerationContext *set;
	GenerationBlock *block;
	GenerationPointer newPointer;
	Size		oldsize;

	/* 允许访问区块头部。 */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Generation_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
	{
		block = ExternalChunkGetBlock(chunk);

		/*
		 * 尝试核验块指针是否正常：块头部应引用一个
		 * generation 上下文。
		 */
		if (!GenerationBlockIsValid(block))
			elog(ERROR, "could not find block containing chunk %p", chunk);

		oldsize = block->endptr - (char *) pointer;
	}
	else
	{
		block = MemoryChunkGetBlock(chunk);

		/*
		 * 在此路径中，出于速度考虑我们仅用 Assert 断言所
		 * 引用的块完好。日后实践可能表明，此
		 * Assert 宜改为常规运行时 test-and-elog 检查。
		 */
		Assert(GenerationBlockIsValid(block));

		oldsize = MemoryChunkGetValue(chunk);
	}

	set = block->context;

#ifdef MEMORY_CONTEXT_CHECKING
	/* 测试是否有人涂写了区块内未用空间 */
	Assert(chunk->requested_size < oldsize);
	if (!sentinel_ok(pointer, chunk->requested_size))
		elog(WARNING, "detected write past chunk end in %s %p",
			 ((MemoryContext) set)->name, chunk);
#endif

	/*
	 * 也许已分配区域已经够大。（特别是，只要请求的是
	 * 缩小，我们总会走到这里。）
	 *
	 * 本内存上下文不用 2 的幂区块大小，而是把区块切
	 * 分得尽可能小，所以大多数 repalloc() 调用都会走
	 * 到 palloc/memcpy/pfree 分支。
	 *
	 * XXX 或许应当给此条件加上 unlikely() 标注？
	 */
#ifdef MEMORY_CONTEXT_CHECKING
	/* MEMORY_CONTEXT_CHECKING 下需要为哨兵多留 1 字节 */
	if (oldsize > size)
#else
	if (oldsize >= size)
#endif
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
		 * 若是增大，把新可用的部分标为 UNDEFINED。
		 * 否则把过时的部分标为 NOACCESS。
		 */
		if (size > oldrequest)
			VALGRIND_MAKE_MEM_UNDEFINED((char *) pointer + oldrequest,
										size - oldrequest);
		else
			VALGRIND_MAKE_MEM_NOACCESS((char *) pointer + size,
									   oldsize - size);

		/* 设置标记以捕获"未用"空间覆写 */
		set_sentinel(pointer, size);
#else							/* !MEMORY_CONTEXT_CHECKING */

		/*
		 * 我们无从判断是在增大
		 * 还是缩小旧请求，故保守地把整个
		 * 新分配标为 DEFINED。
		 */
		VALGRIND_MAKE_MEM_NOACCESS(pointer, oldsize);
		VALGRIND_MAKE_MEM_DEFINED(pointer, size);
#endif

		/* 禁止访问区块头部。 */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);

		return pointer;
	}

	/* 分配新区块 */
	newPointer = GenerationAlloc((MemoryContext) set, size);

	/* 若请求未完成则立即返回 */
	if (newPointer == NULL)
	{
		/* 禁止访问区块头部。 */
		VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);
		return NULL;
	}

	/*
	 * GenerationAlloc() 返回的区域可能仍是 NOACCESS。
	 * 先暂时将其改为 UNDEFINED；随后 memcpy() 会把已定义
	 * 状态从旧分配传递到新分配。若我们知道旧分配的
	 * 大小，就只复制那么多。否则，把整个旧区块标为
	 * 已定义，以免复制尾部仍为 NOACCESS 的字节。
	 *
	 */
	VALGRIND_MAKE_MEM_UNDEFINED(newPointer, size);
#ifdef MEMORY_CONTEXT_CHECKING
	oldsize = chunk->requested_size;
#else
	VALGRIND_MAKE_MEM_DEFINED(pointer, oldsize);
#endif

	/* 传送已有数据（必定装得下） */
	memcpy(newPointer, pointer, oldsize);

	/* 释放旧区块*/
	GenerationFree(pointer);

	return newPointer;
}

/*
 * GenerationGetChunkContext
 *		返回 'pointer' 所属的 MemoryContext。
 */
MemoryContext
GenerationGetChunkContext(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	GenerationBlock *block;

	/* 允许访问区块头部。 */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Generation_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
		block = ExternalChunkGetBlock(chunk);
	else
		block = (GenerationBlock *) MemoryChunkGetBlock(chunk);

	/* 禁止访问区块头部。 */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);

	Assert(GenerationBlockIsValid(block));
	return &block->context->header;
}

/*
 * GenerationGetChunkSpace
 *		给定当前已分配的区块，确定其占用的总空间
 *		（包括全部内存分配开销）。
 */
Size
GenerationGetChunkSpace(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	Size		chunksize;

	/* 允许访问区块头部。 */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Generation_CHUNKHDRSZ);

	if (MemoryChunkIsExternal(chunk))
	{
		GenerationBlock *block = ExternalChunkGetBlock(chunk);

		Assert(GenerationBlockIsValid(block));
		chunksize = block->endptr - (char *) pointer;
	}
	else
		chunksize = MemoryChunkGetValue(chunk);

	/* 禁止访问区块头部。 */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);

	return Generation_CHUNKHDRSZ + chunksize;
}

/*
 * GenerationIsEmpty
 *		GenerationContext 是否不含已分配空间？
 */
bool
GenerationIsEmpty(MemoryContext context)
{
	GenerationContext *set = (GenerationContext *) context;
	dlist_iter	iter;

	Assert(GenerationIsValid(set));

	dlist_foreach(iter, &set->blocks)
	{
		GenerationBlock *block = dlist_container(GenerationBlock, node, iter.cur);

		if (block->nchunks > 0)
			return false;
	}

	return true;
}

/*
 * GenerationStats
 *		计算一个 Generation 上下文的内存消耗统计。
 *
 * printfunc：若非 NULL，则把可读统计字符串传给它。
 * passthru：把此指针透传给 printfunc。
 * totals：非 NULL 时把此上下文统计加进 *totals。
 * print_to_stderr：为真时打印到 stderr，否则 elog。
 *
 * XXX freespace 只统计块末尾的空闲空间，不含已释放
 * 区块的空间（该值未知）。
 */
void
GenerationStats(MemoryContext context,
				MemoryStatsPrintFunc printfunc, void *passthru,
				MemoryContextCounters *totals, bool print_to_stderr)
{
	GenerationContext *set = (GenerationContext *) context;
	Size		nblocks = 0;
	Size		nchunks = 0;
	Size		nfreechunks = 0;
	Size		totalspace;
	Size		freespace = 0;
	dlist_iter	iter;

	Assert(GenerationIsValid(set));

	/* 把上下文头部计入 totalspace */
	totalspace = MAXALIGN(sizeof(GenerationContext));

	dlist_foreach(iter, &set->blocks)
	{
		GenerationBlock *block = dlist_container(GenerationBlock, node, iter.cur);

		nblocks++;
		nchunks += block->nchunks;
		nfreechunks += block->nfree;
		totalspace += block->blksize;
		freespace += (block->endptr - block->freeptr);
	}

	if (printfunc)
	{
		char		stats_string[200];

		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu blocks (%zu chunks); %zu free (%zu chunks); %zu used",
				 totalspace, nblocks, nchunks, freespace,
				 nfreechunks, totalspace - freespace);
		printfunc(context, passthru, stats_string, print_to_stderr);
	}

	if (totals)
	{
		totals->nblocks += nblocks;
		totals->freechunks += nfreechunks;
		totals->totalspace += totalspace;
		totals->freespace += freespace;
	}
}


#ifdef MEMORY_CONTEXT_CHECKING

/*
 * GenerationCheck
 *		遍历各区块并检查内存一致性。
 *
 * 注意：以 WARNING 而非 ERROR 或 FATAL 报告错误。否则
 * 出问题时会陷入无限循环，因为 elog 清理要释放
 * 内存时会再次进入此例程！
 */
void
GenerationCheck(MemoryContext context)
{
	GenerationContext *gen = (GenerationContext *) context;
	const char *name = context->name;
	dlist_iter	iter;
	Size		total_allocated = 0;

	/* 遍历此上下文的所有块 */
	dlist_foreach(iter, &gen->blocks)
	{
		GenerationBlock *block = dlist_container(GenerationBlock, node, iter.cur);
		int			nfree,
					nchunks;
		char	   *ptr;
		bool		has_external_chunk = false;

		total_allocated += block->blksize;

		/*
		 * nfree > nchunks 必定是错的。允许相等，因为该块
		 * 可能是 freeblock 而完全为空。
		 */
		if (block->nfree > block->nchunks)
			elog(WARNING, "problem in Generation %s: number of free chunks %d in block %p exceeds %d allocated",
				 name, block->nfree, block, block->nchunks);

		/* 检查块属于正确的上下文 */
		if (block->context != gen)
			elog(WARNING, "problem in Generation %s: bogus context link in block %p",
				 name, block);

		/* 现在遍历各区块并计数。 */
		nfree = 0;
		nchunks = 0;
		ptr = ((char *) block) + Generation_BLOCKHDRSZ;

		while (ptr < block->freeptr)
		{
			MemoryChunk *chunk = (MemoryChunk *) ptr;
			GenerationBlock *chunkblock;
			Size		chunksize;

			/* 允许访问区块头部。 */
			VALGRIND_MAKE_MEM_DEFINED(chunk, Generation_CHUNKHDRSZ);

			if (MemoryChunkIsExternal(chunk))
			{
				chunkblock = ExternalChunkGetBlock(chunk);
				chunksize = block->endptr - (char *) MemoryChunkGetPointer(chunk);
				has_external_chunk = true;
			}
			else
			{
				chunkblock = MemoryChunkGetBlock(chunk);
				chunksize = MemoryChunkGetValue(chunk);
			}

			/* 移到下一区块 */
			ptr += (chunksize + Generation_CHUNKHDRSZ);

			nchunks += 1;

			/* 区块兼有块与上下文指针，故两者都检查 */
			if (chunkblock != block)
				elog(WARNING, "problem in Generation %s: bogus block link in block %p, chunk %p",
					 name, block, chunk);


			/* 区块已分配？ */
			if (chunk->requested_size != InvalidAllocSize)
			{
				/* 现在确保区块大小正确 */
				if (chunksize < chunk->requested_size ||
					chunksize != MAXALIGN(chunksize))
					elog(WARNING, "problem in Generation %s: bogus chunk size in block %p, chunk %p",
						 name, block, chunk);

				/* 检查哨兵 */
				Assert(chunk->requested_size < chunksize);
				if (!sentinel_ok(chunk, Generation_CHUNKHDRSZ + chunk->requested_size))
					elog(WARNING, "problem in Generation %s: detected write past chunk end in block %p, chunk %p",
						 name, block, chunk);
			}
			else
				nfree += 1;

			/* 若区块已分配，则禁止访问区块头部 */
			if (chunk->requested_size != InvalidAllocSize)
				VALGRIND_MAKE_MEM_NOACCESS(chunk, Generation_CHUNKHDRSZ);
		}

		/*
		 * 确保已分配与空闲区块数符合预期（即块头部
		 * 中记录的值）。
		 */
		if (nchunks != block->nchunks)
			elog(WARNING, "problem in Generation %s: number of allocated chunks %d in block %p does not match header %d",
				 name, nchunks, block, block->nchunks);

		if (nfree != block->nfree)
			elog(WARNING, "problem in Generation %s: number of free chunks %d in block %p does not match header %d",
				 name, nfree, block, block->nfree);

		if (has_external_chunk && nchunks > 1)
			elog(WARNING, "problem in Generation %s: external chunk on non-dedicated block %p",
				 name, block);

	}

	Assert(total_allocated == context->mem_allocated);
}

#endif							/* MEMORY_CONTEXT_CHECKING */
