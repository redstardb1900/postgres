/*-------------------------------------------------------------------------
 *
 * slab.c
 *	  SLAB 分配器定义。
 *
 * SLAB 是一种内存上下文实现，适用于需要高效分配
 * 和释放大量等大小对象、浪费与碎片化极少
 * 的场合。
 *
 *
 * Portions Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/slab.c
 *
 *
 * NOTE:
 *	固定的分配大小使得我们能够进行大量简化，
 *	相对于更通用的分配器还有种种优化。块被划分成
 *	大小正好的区块，只浪费对已分配区块进行 MAXALIGN
 *	所需的空间。
 *
 *	SLAB 还有助于减少内存碎片，情形是：较长寿命的区块
 *	仍存储在块上而其他大多数区块已被 pfree。我们会
 *	优先把新的分配放入"最满"的块。这有助于避免周
 *	围存在过多稀疏使用的块，并让块更容易变得完全
 *	未用。
 *	从而它们最终能被 free。
 *
 *	我们通过从上下文"blocklist"数组中索引最低的
 *	非空元素里取块来确定"最满"的块。
 *	这是一个 dlist 数组，按块所含的空闲区块数划分。
 *	含空闲区块较少的块存放在
 *	dlist 数组中索引较低的槽位。全满的块放在第 0 个
 *	blocklist 数组元素上。为了让数组元素不致过多，
 *	数组中的每条 dlist 负责一个空闲区块数范围。
 *	当区块被 palloc 或 pfree 时，如果空闲区块数
 *	越过了当前链表负责的范围边界，就可能需要把块
 *	移到另一条 dlist 上。数组元素只有少数几个，
 *	这减少了必须把块移到另一 dlist 元素的
 *	次数。
 *
 *	我们用块级的空闲链表来跟踪每个块内的空闲区块。
 *	在该块中分配新区块时会查询此链表。空闲链表
 *	是一条链表，其头部由
 *	SlabBlock 的 freehead 字段指向。每个后续列表项存于
 *	空闲区块内存中。我们确保区块大到足以存下地址。
 *	address.
 *
 *	当我们分配一个新块时，严格说所有区块都是空闲的，
 *	但为了避免把整个块写出来以设置链表，我们改为存
 *	储指向块上下一个"未用"区块的指针，并跟踪这类未
 *	用区块的数量。
 *	新块被 malloc 出来时，所有区块都未
 *	用。未用指针从块上第一个区块开始，随着区块分配，
 *	未用指针随之递增。区块被 pfree 时，未用指针绝不
 *	后退。未用指针可视为块上曾并发使用的最大区块数
 *	的高水位标记。
 *	区块被 pfree 时，区块被放到空闲链表
 *	头部，未用指针保持不变。只有当链表上空闲区块耗
 *	尽时我们才会消耗更多未用区块。
 *	这种方法实际上优先使用先前用过的区块，
 *	而非先前未用的区块，由于 CPU 缓存效应，这应当
 *	有更好的表现。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/ilist.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_memorychunk.h"
#include "utils/memutils_internal.h"

#define Slab_BLOCKHDRSZ	MAXALIGN(sizeof(SlabBlock))

#ifdef MEMORY_CONTEXT_CHECKING
/*
 * 存储 SlabContext 所需内存的大小。
 * MEMORY_CONTEXT_CHECKING 构建需额外内存存 isChunkFree 数
 * 组。
 */
#define Slab_CONTEXT_HDRSZ(chunksPerBlock)	\
	(sizeof(SlabContext) + ((chunksPerBlock) * sizeof(bool)))
#else
#define Slab_CONTEXT_HDRSZ(chunksPerBlock)	sizeof(SlabContext)
#endif

/*
 * 按空闲区块数把 blocklist 划分成的分区数。必须至
 * 少为 2。
 */
#define SLAB_BLOCKLIST_COUNT 3

/* 为重用而保留的完全空块的最大数量。 */
#define SLAB_MAXIMUM_EMPTY_BLOCKS 10

/*
 * SlabContext 是 MemoryContext 的一个专用实现。
 */
typedef struct SlabContext
{
	MemoryContextData header;	/* 标准内存上下文字段 */
	/* 本上下文的分配参数： */
	Size		chunkSize;		/* 请求（未对齐）区块大小 */
	Size		fullChunkSize;	/* 加区块头部与对齐后的大小 */
	Size		blockSize;		/* 每个块的大小 */
	int32		chunksPerBlock; /* 1 块能容纳的区块数 */
	int32		curBlocklistIndex;	/* blocklist[] 元素中的索引
									 * 所指元素含最满的块 */
#ifdef MEMORY_CONTEXT_CHECKING
	bool	   *isChunkFree;	/* SlabCheck 标记空闲区块用的数组
								 * */
#endif

	int32		blocklist_shift;	/* 把 nfree 计数移位以得到
									 * blocklist[] 索引的位数 */
	dclist_head emptyblocks;	/* 先用完空块而不是
								 * malloc 新块 */

	/*
	 * 含空闲空间的块，按空闲区块数分组。全满块存
	 * 在第 0 个元素。全空块则存入 emptyblocks，或在
	 * 已有足够多空块时，
	 * 则直接释放。
	 */
	dlist_head	blocklist[SLAB_BLOCKLIST_COUNT];
} SlabContext;

/*
 * SlabBlock
 *		单个 slab 块的结构。
 *
 * slab：指回拥有上下文的指针
 * nfree：块上未分配的区块数
 * nunused：块上未分配且不在块空闲链表上的区块
 * 数。
 * freehead：链表头部，存指向块上第一个空闲区块
 * 的指针。后续指针存在区块内存中。NULL
 * 表示链表结尾。
 * unused：指向下一个尚未使用的区块的指针
 * node：用于上下文 blocklist 的双链表节点
 */
typedef struct SlabBlock
{
	SlabContext *slab;			/* 属主 */
	int32		nfree;			/* 空闲 + 未用区块的数量 */
	int32		nunused;		/* 未用区块数 */
	MemoryChunk *freehead;		/* 第一空闲区块指针 */
	MemoryChunk *unused;		/* 下一未用区块指针 */
	dlist_node	node;			/* blocklist[] 的双向链表 */
} SlabBlock;


#define Slab_CHUNKHDRSZ sizeof(MemoryChunk)
#define SlabChunkGetPointer(chk)	\
	((void *) (((char *) (chk)) + sizeof(MemoryChunk)))

/*
 * SlabBlockGetChunk
 *		获取块中第 n（从 0 计）个区块的指针
 */
#define SlabBlockGetChunk(slab, block, n) \
	((MemoryChunk *) ((char *) (block) + Slab_BLOCKHDRSZ	\
					+ ((n) * (slab)->fullChunkSize)))

#if defined(MEMORY_CONTEXT_CHECKING) || defined(USE_ASSERT_CHECKING)

/*
 * SlabChunkIndex
 *		获取给定区块是块中第几个区块（从 0 计）的
 *		索引。
*/
#define SlabChunkIndex(slab, block, chunk)	\
	(((char *) (chunk) - (char *) SlabBlockGetChunk(slab, block, 0)) / \
	(slab)->fullChunkSize)

/*
 * SlabChunkMod
 *		MemoryChunk 应始终位于从第 0 个区块位置起为
 *		fullChunkSize 倍数的地址。若非如此，
 *		则返回非零值。
 */
#define SlabChunkMod(slab, block, chunk)	\
	(((char *) (chunk) - (char *) SlabBlockGetChunk(slab, block, 0)) % \
	(slab)->fullChunkSize)

#endif

/*
 * SlabIsValid
 *		集合是有效 slab 分配集时为真。
 */
#define SlabIsValid(set) (PointerIsValid(set) && IsA(set, SlabContext))

/*
 * SlabBlockIsValid
 *		块为 slab 分配集的有效块时为真。
 */
#define SlabBlockIsValid(block) \
	(PointerIsValid(block) && SlabIsValid((block)->slab))

/*
 * SlabBlocklistIndex
 *		确定给定空闲区块数的块应处的
 *		blocklist 索引。
 */
static inline int32
SlabBlocklistIndex(SlabContext *slab, int nfree)
{
	int32		index;
	int32		blocklist_shift = slab->blocklist_shift;

	Assert(nfree >= 0 && nfree <= slab->chunksPerBlock);

	/*
	 * 根据空闲区块数确定 blocklist 索引。必须保证 0 个
	 * 空闲区块专用索引 0。其余的必须 >= 1 且 <
	 * SLAB_BLOCKLIST_COUNT。
	 *
	 * 为尽可能高效，我们利用了一些补码算术，在移位前
	 * 反转符号。这使 nfree 为 0 时使用索引 0，而非零值
	 * 保持非零。
	 * 这利用了补码中 0 与 -0 相同的事实。完成之
	 * 后，只需再把符号翻转回来得到
	 * 正索引。
	 */
	index = -((-nfree) >> blocklist_shift);

	if (nfree == 0)
		Assert(index == 0);
	else
		Assert(index >= 1 && index < SLAB_BLOCKLIST_COUNT);

	return index;
}

/*
 * SlabFindNextBlockListIndex
 *		在 blocklist 中搜索含有空闲区块的块，返回找到的
 *		含至少一个含空闲区块的块的 blocklist 索引。若找
 *		不到块则返回 0。
 *
 * 注意：我们优先选择更满的块，使其先于较空的块被
 * 填满。这是为了增加近乎空块最终完全变空、从而
 * 能被释放的机会。
 */
static int32
SlabFindNextBlockListIndex(SlabContext *slab)
{
	/* 从 1 开始，因 blocklist[0] 属满块。 */
	for (int i = 1; i < SLAB_BLOCKLIST_COUNT; i++)
	{
		/* 返回第一个找到的非空索引 */
		if (!dlist_is_empty(&slab->blocklist[i]))
			return i;
	}

	/* 无含空闲空间的块 */
	return 0;
}

/*
 * SlabGetNextFreeChunk
 *		返回块中下一个空闲区块，并更新块，
 *		以反映返回区块已被使用。
 */
static inline MemoryChunk *
SlabGetNextFreeChunk(SlabContext *slab, SlabBlock *block)
{
	MemoryChunk *chunk;

	Assert(block->nfree > 0);

	if (block->freehead != NULL)
	{
		chunk = block->freehead;

		/*
		 * 把该区块从空闲区块链表弹出。下一空闲区块的
		 * 指针存储在区块自身中。
		 */
		VALGRIND_MAKE_MEM_DEFINED(SlabChunkGetPointer(chunk), sizeof(MemoryChunk *));
		block->freehead = *(MemoryChunk **) SlabChunkGetPointer(chunk);

		/* 检查没有东西踩踏空闲区块的内存 */
		Assert(block->freehead == NULL ||
			   (block->freehead >= SlabBlockGetChunk(slab, block, 0) &&
				block->freehead <= SlabBlockGetChunk(slab, block, slab->chunksPerBlock - 1) &&
				SlabChunkMod(slab, block, block->freehead) == 0));
	}
	else
	{
		Assert(block->nunused > 0);

		chunk = block->unused;
		block->unused = (MemoryChunk *) (((char *) block->unused) + slab->fullChunkSize);
		block->nunused--;
	}

	block->nfree--;

	return chunk;
}

/*
 * SlabContextCreate
 *		新建 Slab 上下文。
 *
 * parent：父上下文，顶层上下文则为 NULL
 * name：上下文名称（必须为静态分配）
 * blockSize：分配块大小
 * chunkSize：分配区块大小
 *
 * MAXALIGN(chunkSize) 不得超过 MEMORYCHUNK_MAX_VALUE
 */
MemoryContext
SlabContextCreate(MemoryContext parent,
				  const char *name,
				  Size blockSize,
				  Size chunkSize)
{
	int			chunksPerBlock;
	Size		fullChunkSize;
	SlabContext *slab;
	int			i;

	/* 确保 MemoryChunk 大小经 MAXALIGN 对齐 */
	StaticAssertDecl(Slab_CHUNKHDRSZ == MAXALIGN(Slab_CHUNKHDRSZ),
					 "sizeof(MemoryChunk) is not maxaligned");
	Assert(MAXALIGN(chunkSize) <= MEMORYCHUNK_MAX_VALUE);

	/*
	 * 确保有足够空间把指向下一空闲区块的指针存入（本
	 * 来未用的）分配的内存中。
	 */
	if (chunkSize < sizeof(MemoryChunk *))
		chunkSize = sizeof(MemoryChunk *);

	/* 包括区块头部的最大对齐区块长度 */
#ifdef MEMORY_CONTEXT_CHECKING
	/* 确保有哨兵字节的空间 */
	fullChunkSize = Slab_CHUNKHDRSZ + MAXALIGN(chunkSize + 1);
#else
	fullChunkSize = Slab_CHUNKHDRSZ + MAXALIGN(chunkSize);
#endif

	/* 计算每个块能容纳的区块数 */
	chunksPerBlock = (blockSize - Slab_BLOCKHDRSZ) / fullChunkSize;

	/* 确保块至少能存一个区块。 */
	if (chunksPerBlock == 0)
		elog(ERROR, "block size %zu for slab is too small for %zu-byte chunks",
			 blockSize, chunkSize);



	slab = (SlabContext *) malloc(Slab_CONTEXT_HDRSZ(chunksPerBlock));
	if (slab == NULL)
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
	 * 若在此区间 ereport，我们会泄漏头部。
	 */

	/* 填写 SlabContext 专属的头部字段 */
	slab->chunkSize = chunkSize;
	slab->fullChunkSize = fullChunkSize;
	slab->blockSize = blockSize;
	slab->chunksPerBlock = chunksPerBlock;
	slab->curBlocklistIndex = 0;

	/*
	 * 计算一个移位量，保证用其对 chunksPerBlock 移位的
	 * 结果 < SLAB_BLOCKLIST_COUNT - 1。之所以在此计算中
	 * 从 SLAB_BLOCKLIST_COUNT 减 1，是因我们把第 0 个
	 * blocklist 元素留给没有空闲区块的块。
	 *
	 * 我们计算的是移位位数而非除数，
	 * 因为每次查找 blocklist 索引都做除法
	 * 会慢得多。
	 */
	slab->blocklist_shift = 0;
	while ((slab->chunksPerBlock >> slab->blocklist_shift) >= (SLAB_BLOCKLIST_COUNT - 1))
		slab->blocklist_shift++;

	/* 初始化存可重用空块的列表 */
	dclist_init(&slab->emptyblocks);

	/* 初始化每个 blocklist 槽 */
	for (i = 0; i < SLAB_BLOCKLIST_COUNT; i++)
		dlist_init(&slab->blocklist[i]);

#ifdef MEMORY_CONTEXT_CHECKING
	/* 把 isChunkFree 指针设在上下文结尾之后 */
	slab->isChunkFree = (bool *) ((char *) slab + sizeof(SlabContext));
#endif

	/* 最后，完成上下文创建中与类型无关的部分 */
	MemoryContextCreate((MemoryContext) slab,
						T_SlabContext,
						MCTX_SLAB_ID,
						parent,
						name);

	return (MemoryContext) slab;
}

/*
 * SlabReset
 *		释放给定集合中已分配的全部内存。
 *
 * 代码只是释放上下文中的所有块 —— 我们不保留任何
 * 保留块或类似的东西。
 */
void
SlabReset(MemoryContext context)
{
	SlabContext *slab = (SlabContext *) context;
	dlist_mutable_iter miter;
	int			i;

	Assert(SlabIsValid(slab));

#ifdef MEMORY_CONTEXT_CHECKING
	/* 释放前检查损坏与泄漏 */
	SlabCheck(context);
#endif

	/* 释放保留的空块 */
	dclist_foreach_modify(miter, &slab->emptyblocks)
	{
		SlabBlock  *block = dlist_container(SlabBlock, node, miter.cur);

		dclist_delete_from(&slab->emptyblocks, miter.cur);

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(block, slab->blockSize);
#endif
		free(block);
		context->mem_allocated -= slab->blockSize;
	}

	/* 遍历 blocklist 并释放各块 */
	for (i = 0; i < SLAB_BLOCKLIST_COUNT; i++)
	{
		dlist_foreach_modify(miter, &slab->blocklist[i])
		{
			SlabBlock  *block = dlist_container(SlabBlock, node, miter.cur);

			dlist_delete(miter.cur);

#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(block, slab->blockSize);
#endif
			free(block);
			context->mem_allocated -= slab->blockSize;
		}
	}

	slab->curBlocklistIndex = 0;

	Assert(context->mem_allocated == 0);
}

/*
 * SlabDelete
 *		释放给定上下文中已分配的全部内存。
 */
void
SlabDelete(MemoryContext context)
{
	/* 重置以释放所有 SlabBlocks */
	SlabReset(context);
	/* 然后释放上下文头部 */
	free(context);
}

/*
 * SlabAlloc
 *		返回给定大小已分配内存指针；请求无法完成
 *		则返回 NULL；内存会被加入 slab。
 */
void *
SlabAlloc(MemoryContext context, Size size)
{
	SlabContext *slab = (SlabContext *) context;
	SlabBlock  *block;
	MemoryChunk *chunk;

	Assert(SlabIsValid(slab));

	/* 健全性检查此指针指向有效的 blocklist */
	Assert(slab->curBlocklistIndex >= 0);
	Assert(slab->curBlocklistIndex <= SlabBlocklistIndex(slab, slab->chunksPerBlock));

	/* 确保只允许正确的请求大小 */
	if (unlikely(size != slab->chunkSize))
		elog(ERROR, "unexpected alloc chunk size %zu (expected %zu)",
			 size, slab->chunkSize);

	/*
	 * 在没有半满块可用时处理。SlabFree() 会更新
	 * curBlocklistIndex 把它设为 0，表示已释放最后一个
	 * 块。此外 SlabAlloc() 稍后填满最后一块时也
	 * 会把 curBlocklistIndex 设为
	 * 0。
	 */
	if (unlikely(slab->curBlocklistIndex == 0))
	{
		dlist_head *blocklist;
		int			blocklist_idx;

		/* 为了省去分配新块，先检查空块列表 */
		if (dclist_count(&slab->emptyblocks) > 0)
		{
			dlist_node *node = dclist_pop_head_node(&slab->emptyblocks);

			block = dlist_container(SlabBlock, node, node);

			/*
			 * SlabFree() 应已让此块处于全空闲的有效状态。
			 * 务必确认情况如此。
			 */
			Assert(block->nfree == slab->chunksPerBlock);

			/* 从此块取下一个区块 */
			chunk = SlabGetNextFreeChunk(slab, block);
		}
		else
		{
			block = (SlabBlock *) malloc(slab->blockSize);

			if (unlikely(block == NULL))
				return NULL;

			block->slab = slab;
			context->mem_allocated += slab->blockSize;

			/* 使用新块中的第一个区块 */
			chunk = SlabBlockGetChunk(slab, block, 0);

			block->nfree = slab->chunksPerBlock - 1;
			block->unused = SlabBlockGetChunk(slab, block, 1);
			block->freehead = NULL;
			block->nunused = slab->chunksPerBlock - 1;
		}

		/* 找到存含 1 个已用区块的块的 blocklist 元素 */
		blocklist_idx = SlabBlocklistIndex(slab, block->nfree);
		blocklist = &slab->blocklist[blocklist_idx];

		/* 此处最好是空的。我们刚加块时以为它如此 */
		Assert(dlist_is_empty(blocklist));

		dlist_push_head(blocklist, &block->node);

		slab->curBlocklistIndex = blocklist_idx;
	}
	else
	{
		dlist_head *blocklist = &slab->blocklist[slab->curBlocklistIndex];
		int			new_blocklist_idx;

		Assert(!dlist_is_empty(blocklist));

		/* 从 blocklist 中抓取块 */
		block = dlist_head_element(SlabBlock, node, blocklist);

		/* 确保确实得到了有效的块，且 nfree 匹配 */
		Assert(block != NULL);
		Assert(slab->curBlocklistIndex == SlabBlocklistIndex(slab, block->nfree));
		Assert(block->nfree > 0);

		/* 从此块取下一个区块 */
		chunk = SlabGetNextFreeChunk(slab, block);

		/* 按新的空闲区块数得到新的 blocklist 索引 */
		new_blocklist_idx = SlabBlocklistIndex(slab, block->nfree);

		/*
		 * 处理 blocklist 索引变化的情况。这也处理块变满的
		 * 情形，因为只有全满块位于索引 0。
		 */
		if (unlikely(slab->curBlocklistIndex != new_blocklist_idx))
		{
			dlist_delete_from(blocklist, &block->node);
			dlist_push_head(&slab->blocklist[new_blocklist_idx], &block->node);

			if (dlist_is_empty(blocklist))
				slab->curBlocklistIndex = SlabFindNextBlockListIndex(slab);
		}
	}

	/*
	 * 检查区块指针确实位于块上某处，并
	 * 按预期对齐。
	 */
	Assert(chunk >= SlabBlockGetChunk(slab, block, 0));
	Assert(chunk <= SlabBlockGetChunk(slab, block, slab->chunksPerBlock - 1));
	Assert(SlabChunkMod(slab, block, chunk) == 0);

	/* 准备初始化区块头部。 */
	VALGRIND_MAKE_MEM_UNDEFINED(chunk, Slab_CHUNKHDRSZ);

	MemoryChunkSetHdrMask(chunk, block, MAXALIGN(slab->chunkSize),
						  MCTX_SLAB_ID);
#ifdef MEMORY_CONTEXT_CHECKING
	/* slab 标记以捕获"未用"空间覆写 */
	Assert(slab->chunkSize < (slab->fullChunkSize - Slab_CHUNKHDRSZ));
	set_sentinel(MemoryChunkGetPointer(chunk), size);
	VALGRIND_MAKE_MEM_NOACCESS(((char *) chunk) +
							   Slab_CHUNKHDRSZ + slab->chunkSize,
							   slab->fullChunkSize -
							   (slab->chunkSize + Slab_CHUNKHDRSZ));
#endif

#ifdef RANDOMIZE_ALLOCATED_MEMORY
	/* 用垃圾填充已分配空间 */
	randomize_mem((char *) MemoryChunkGetPointer(chunk), size);
#endif

	/* 禁止访问区块头部。 */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Slab_CHUNKHDRSZ);

	return MemoryChunkGetPointer(chunk);
}

/*
 * SlabFree
 *		释放已分配内存；内存从 slab 中移除。
 */
void
SlabFree(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SlabBlock  *block;
	SlabContext *slab;
	int			curBlocklistIdx;
	int			newBlocklistIdx;

	/* 允许访问区块头部。 */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Slab_CHUNKHDRSZ);

	block = MemoryChunkGetBlock(chunk);

	/*
	 * 出于速度考虑只断言被引用的块是好的。未来的
	 * 实践经验或许表明，此断言最好变成常规运行时
	 * test-and-elog 检查。
	 */
	Assert(SlabBlockIsValid(block));
	slab = block->slab;

#ifdef MEMORY_CONTEXT_CHECKING
	/* 测试是否有人涂写了区块内未用空间 */
	Assert(slab->chunkSize < (slab->fullChunkSize - Slab_CHUNKHDRSZ));
	if (!sentinel_ok(pointer, slab->chunkSize))
		elog(WARNING, "detected write past chunk end in %s %p",
			 slab->header.name, chunk);
#endif

	/* 把此区块压入块空闲链表的头部 */
	*(MemoryChunk **) pointer = block->freehead;
	block->freehead = chunk;

	block->nfree++;

	Assert(block->nfree > 0);
	Assert(block->nfree <= slab->chunksPerBlock);

#ifdef CLOBBER_FREED_MEMORY
	/* 勿抹除区块中存储的空闲链表 MemoryChunk 指针 */
	wipe_mem((char *) pointer + sizeof(MemoryChunk *),
			 slab->chunkSize - sizeof(MemoryChunk *));
#endif

	curBlocklistIdx = SlabBlocklistIndex(slab, block->nfree - 1);
	newBlocklistIdx = SlabBlocklistIndex(slab, block->nfree);

	/*
	 * 检查块是否因多 1 个空闲区块而需移到
	 * blocklist 的另一元素上。
	 */
	if (unlikely(curBlocklistIdx != newBlocklistIdx))
	{
		/* 搬移 */
		dlist_delete_from(&slab->blocklist[curBlocklistIdx], &block->node);
		dlist_push_head(&slab->blocklist[newBlocklistIdx], &block->node);

		/*
		 * blocklist[curBlocklistIdx] 现在可能空了，或者现在
		 * 能用更低层的 blocklist 元素。当前 blocklist 若被
		 * 改动，或更低层元素有变，需重新确定
		 * slab->curBlocklistIndex 应为多少。必须确保我们使用
		 * 的链表含有最满的块。
		 */
		if (slab->curBlocklistIndex >= curBlocklistIdx)
		{
			slab->curBlocklistIndex = SlabFindNextBlockListIndex(slab);

			/*
			 * 必有一个块含至少 1 个未用区块，因为我们刚
			 * pfree 了一个。确保 curBlocklistIndex 反映这一点。
			 */
			Assert(slab->curBlocklistIndex > 0);
		}
	}

	/* 处理块完全变空的情形 */
	if (unlikely(block->nfree == slab->chunksPerBlock))
	{
		/* 移除该块 */
		dlist_delete_from(&slab->blocklist[newBlocklistIdx], &block->node);

		/*
		 * 为避免 malloc/free 折腾，我们保留一个空块列表，
		 * 可以再次重用而不必 malloc 新块。
		 */
		if (dclist_count(&slab->emptyblocks) < SLAB_MAXIMUM_EMPTY_BLOCKS)
			dclist_push_head(&slab->emptyblocks, &block->node);
		else
		{
			/*
			 * 空块存够多时，我们才真正释放
			 * 该块。
			 */
#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(block, slab->blockSize);
#endif
			free(block);
			slab->header.mem_allocated -= slab->blockSize;
		}

		/*
		 * 检查是否需要重置 blocklist 索引。当该块所在的
		 * blocklist 变为完全空时需要这样做。
		 */
		if (slab->curBlocklistIndex == newBlocklistIdx &&
			dlist_is_empty(&slab->blocklist[newBlocklistIdx]))
			slab->curBlocklistIndex = SlabFindNextBlockListIndex(slab);
	}
}

/*
 * SlabRealloc
 *		更改区块的已分配大小。
 *
 * 由于 Slab 为分配等大小区块而设计，它无法真正改
 * 变区块大小。我们尽量温和，允许大小完全相同的调
 * 用；此时可直接返回同一区块。当大小不同时，
 * 我们抛出错误。
 *
 * 我们也可以允许 size < chunkSize 的请求。然而这
 * 似乎没什么意义 —— Slab 面向恒定大小的区块，而且
 * realloc 通常用于扩大区块。
 */
void *
SlabRealloc(void *pointer, Size size)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SlabBlock  *block;
	SlabContext *slab;

	/* 允许访问区块头部。 */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Slab_CHUNKHDRSZ);

	block = MemoryChunkGetBlock(chunk);

	/* 禁止访问区块头部。 */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Slab_CHUNKHDRSZ);

	/*
	 * 校验块指针是否正常：块头部应引用 slab上下文。
	 * （我们用 test-and-elog 而非 Assert，因为
	 * 到这儿来多半就已经错
	 * 了。）
	 */
	if (!SlabBlockIsValid(block))
		elog(ERROR, "could not find block containing chunk %p", chunk);
	slab = block->slab;

	/* slab 无法真正 realloc，但让我们尽量温和 */
	if (size == slab->chunkSize)
		return pointer;

	elog(ERROR, "slab allocator does not support realloc()");
	return NULL;				/* 让编译器安静 */
}

/*
 * SlabGetChunkContext
 *		返回 'pointer' 所属的内存上下文。
 */
MemoryContext
SlabGetChunkContext(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SlabBlock  *block;

	/* 允许访问区块头部。 */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Slab_CHUNKHDRSZ);

	block = MemoryChunkGetBlock(chunk);

	/* 禁止访问区块头部。 */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Slab_CHUNKHDRSZ);

	Assert(SlabBlockIsValid(block));

	return &block->slab->header;
}

/*
 * SlabGetChunkSpace
 *		给定当前已分配的区块，确定其占用的总空间
 *		（包括所有内存分配开销）。
 */
Size
SlabGetChunkSpace(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SlabBlock  *block;
	SlabContext *slab;

	/* 允许访问区块头部。 */
	VALGRIND_MAKE_MEM_DEFINED(chunk, Slab_CHUNKHDRSZ);

	block = MemoryChunkGetBlock(chunk);

	/* 禁止访问区块头部。 */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, Slab_CHUNKHDRSZ);

	Assert(SlabBlockIsValid(block));
	slab = block->slab;

	return slab->fullChunkSize;
}

/*
 * SlabIsEmpty
 *		slab 是否没有已分配空间？
 */
bool
SlabIsEmpty(MemoryContext context)
{
	Assert(SlabIsValid((SlabContext *) context));

	return (context->mem_allocated == 0);
}

/*
 * SlabStats
 *		计算一个 Slab 上下文的内存消耗统计。
 *
 * printfunc：若非 NULL，则把可读统计字符串传给它。
 * passthru：把此指针透传给 printfunc。
 * totals：非 NULL 时把此上下文统计加进 *totals。
 * print_to_stderr：为真时打印到 stderr，否则 elog。
 */
void
SlabStats(MemoryContext context,
		  MemoryStatsPrintFunc printfunc, void *passthru,
		  MemoryContextCounters *totals,
		  bool print_to_stderr)
{
	SlabContext *slab = (SlabContext *) context;
	Size		nblocks = 0;
	Size		freechunks = 0;
	Size		totalspace;
	Size		freespace = 0;
	int			i;

	Assert(SlabIsValid(slab));

	/* 上下文头部计入 totalspace */
	totalspace = Slab_CONTEXT_HDRSZ(slab->chunksPerBlock);

	/* 加上 emptyblocks 列表中块消耗的空间 */
	totalspace += dclist_count(&slab->emptyblocks) * slab->blockSize;

	for (i = 0; i < SLAB_BLOCKLIST_COUNT; i++)
	{
		dlist_iter	iter;

		dlist_foreach(iter, &slab->blocklist[i])
		{
			SlabBlock  *block = dlist_container(SlabBlock, node, iter.cur);

			nblocks++;
			totalspace += slab->blockSize;
			freespace += slab->fullChunkSize * block->nfree;
			freechunks += block->nfree;
		}
	}

	if (printfunc)
	{
		char		stats_string[200];

		/* XXX 是否应计入空块上的空闲区块？ */
		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu blocks; %u empty blocks; %zu free (%zu chunks); %zu used",
				 totalspace, nblocks, dclist_count(&slab->emptyblocks),
				 freespace, freechunks, totalspace - freespace);
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
 * SlabCheck
 *		遍历所有块以查找不一致之处。
 *
 * 注意：以 WARNING 而非 ERROR 或 FATAL 报告错误。否则
 * 出问题时会陷入无限循环，因为 elog 清理要释放
 * 内存时会再次进入此例程！
 */
void
SlabCheck(MemoryContext context)
{
	SlabContext *slab = (SlabContext *) context;
	int			i;
	int			nblocks = 0;
	const char *name = slab->header.name;
	dlist_iter	iter;

	Assert(SlabIsValid(slab));
	Assert(slab->chunksPerBlock > 0);

	/*
	 * 看看空块。这些块的区块都应标为空闲。务必确认
	 * 情况如此。
	 */
	dclist_foreach(iter, &slab->emptyblocks)
	{
		SlabBlock  *block = dlist_container(SlabBlock, node, iter.cur);

		if (block->nfree != slab->chunksPerBlock)
			elog(WARNING, "problem in slab %s: empty block %p should have %d free chunks but has %d chunks free",
				 name, block, slab->chunksPerBlock, block->nfree);
	}

	/* 遍历非空块列表 */
	for (i = 0; i < SLAB_BLOCKLIST_COUNT; i++)
	{
		int			j,
					nfree;

		/* 遍历此 blocklist 上的块 */
		dlist_foreach(iter, &slab->blocklist[i])
		{
			SlabBlock  *block = dlist_container(SlabBlock, node, iter.cur);
			MemoryChunk *cur_chunk;

			/*
			 * 确保（块头部中的）空闲区块数与该块在
			 * blocklist 中的位置一致。
			 */
			if (SlabBlocklistIndex(slab, block->nfree) != i)
				elog(WARNING, "problem in slab %s: block %p is on blocklist %d but should be on blocklist %d",
					 name, block, i, SlabBlocklistIndex(slab, block->nfree));

			/* 确保块不是空的 */
			if (block->nfree >= slab->chunksPerBlock)
				elog(WARNING, "problem in slab %s: empty block %p incorrectly stored on blocklist element %d",
					 name, block, i);

			/* 确保 slab 指针正确地指向此上下文 */
			if (block->slab != slab)
				elog(WARNING, "problem in slab %s: bogus slab link in block %p",
					 name, block);

			/* 重置此块的空闲区块数组 */
			memset(slab->isChunkFree, 0, (slab->chunksPerBlock * sizeof(bool)));
			nfree = 0;

			/* 遍历块的空闲链表区块 */
			cur_chunk = block->freehead;
			while (cur_chunk != NULL)
			{
				int			chunkidx = SlabChunkIndex(slab, block, cur_chunk);

				/*
				 * 确保链表链接指向块上某处，地址
				 * 对齐（按完整区块大小 fullChunkSize）。
				 */
				if (cur_chunk < SlabBlockGetChunk(slab, block, 0) ||
					cur_chunk > SlabBlockGetChunk(slab, block, slab->chunksPerBlock - 1) ||
					SlabChunkMod(slab, block, cur_chunk) != 0)
					elog(WARNING, "problem in slab %s: bogus free list link %p in block %p",
						 name, cur_chunk, block);

				/* 计数该区块并在空闲区块数组标记空闲 */
				nfree++;
				slab->isChunkFree[chunkidx] = true;

				/* 读取下一空闲区块的指针 */
				VALGRIND_MAKE_MEM_DEFINED(MemoryChunkGetPointer(cur_chunk), sizeof(MemoryChunk *));
				cur_chunk = *(MemoryChunk **) SlabChunkGetPointer(cur_chunk);
			}

			/* 检查未用指针与 nunused 所称一致 */
			if (SlabBlockGetChunk(slab, block, slab->chunksPerBlock - block->nunused) !=
				block->unused)
				elog(WARNING, "problem in slab %s: mismatch detected between nunused chunks and unused pointer in block %p",
					 name, block);

			/*
			 * 数一数尚未进入块空闲链表的
			 * 剩余空闲区块。
			 */
			cur_chunk = block->unused;
			for (j = 0; j < block->nunused; j++)
			{
				int			chunkidx = SlabChunkIndex(slab, block, cur_chunk);


				/* 把区块计为空闲并在数组中如此标记 */
				nfree++;
				if (chunkidx < slab->chunksPerBlock)
					slab->isChunkFree[chunkidx] = true;

				/* 前移一区块 */
				cur_chunk = (MemoryChunk *) (((char *) cur_chunk) + slab->fullChunkSize);
			}

			for (j = 0; j < slab->chunksPerBlock; j++)
			{
				if (!slab->isChunkFree[j])
				{
					MemoryChunk *chunk = SlabBlockGetChunk(slab, block, j);
					SlabBlock  *chunkblock;

					/* 允许访问区块头部。 */
					VALGRIND_MAKE_MEM_DEFINED(chunk, Slab_CHUNKHDRSZ);

					chunkblock = (SlabBlock *) MemoryChunkGetBlock(chunk);

					/* 禁止访问区块头部。 */
					VALGRIND_MAKE_MEM_NOACCESS(chunk, Slab_CHUNKHDRSZ);

					/*
					 * 检查区块的块偏移是否正确指回
					 * 该块。
					 */
					if (chunkblock != block)
						elog(WARNING, "problem in slab %s: bogus block link in block %p, chunk %p",
							 name, block, chunk);

					/* 检查哨兵字节完好 */
					Assert(slab->chunkSize < (slab->fullChunkSize - Slab_CHUNKHDRSZ));
					if (!sentinel_ok(chunk, Slab_CHUNKHDRSZ + slab->chunkSize))
						elog(WARNING, "problem in slab %s: detected write past chunk end in block %p, chunk %p",
							 name, block, chunk);
				}
			}

			/*
			 * 确保空闲区块数与预期一致（块头部所跟踪的
			 * 那样）。
			 */
			if (nfree != block->nfree)
				elog(WARNING, "problem in slab %s: nfree in block %p is %d but %d chunk were found as free",
					 name, block, block->nfree, nfree);

			nblocks++;
		}
	}

	/* 所存的空块也计入 mem_allocated */
	nblocks += dclist_count(&slab->emptyblocks);

	Assert(nblocks * slab->blockSize == context->mem_allocated);
}

#endif							/* MEMORY_CONTEXT_CHECKING */
