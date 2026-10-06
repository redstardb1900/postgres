/*-------------------------------------------------------------------------
 *
 * gistbuildbuffers.c
 *	  GiST 缓冲构建算法的节点缓冲区管理函数。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistbuildbuffers.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

static GISTNodeBufferPage *gistAllocateNewPageBuffer(GISTBuildBuffers *gfbb);
static void gistAddLoadedBuffer(GISTBuildBuffers *gfbb,
								GISTNodeBuffer *nodeBuffer);
static void gistLoadNodeBuffer(GISTBuildBuffers *gfbb,
							   GISTNodeBuffer *nodeBuffer);
static void gistUnloadNodeBuffer(GISTBuildBuffers *gfbb,
								 GISTNodeBuffer *nodeBuffer);
static void gistPlaceItupToPage(GISTNodeBufferPage *pageBuffer,
								IndexTuple itup);
static void gistGetItupFromPage(GISTNodeBufferPage *pageBuffer,
								IndexTuple *itup);
static long gistBuffersGetFreeBlock(GISTBuildBuffers *gfbb);
static void gistBuffersReleaseBlock(GISTBuildBuffers *gfbb, long blocknum);

static void ReadTempFileBlock(BufFile *file, long blknum, void *ptr);
static void WriteTempFileBlock(BufFile *file, long blknum, const void *ptr);


/*
 * 初始化 GiST 构建缓冲区。
 */
GISTBuildBuffers *
gistInitBuildBuffers(int pagesPerBuffer, int levelStep, int maxLevel)
{
	GISTBuildBuffers *gfbb;
	HASHCTL		hashCtl;

	gfbb = palloc(sizeof(GISTBuildBuffers));
	gfbb->pagesPerBuffer = pagesPerBuffer;
	gfbb->levelStep = levelStep;

	/*
	 * 创建临时文件，用于保存被换出内存的
	 * 缓冲区页。
	 */
	gfbb->pfile = BufFileCreateTemp(false);
	gfbb->nFileBlocks = 0;

	/* 初始化空闲页管理。 */
	gfbb->nFreeBlocks = 0;
	gfbb->freeBlocksLen = 32;
	gfbb->freeBlocks = (long *) palloc(gfbb->freeBlocksLen * sizeof(long));

	/*
	 * 当前内存上下文将用于缓冲构建期间持久存在的
	 * 缓冲区的全部内存数据结构。
	 */
	gfbb->context = CurrentMemoryContext;

	/*
	 * nodeBuffersTab 哈希表是索引块与其
	 * 缓冲区之间的关联。
	 */
	hashCtl.keysize = sizeof(BlockNumber);
	hashCtl.entrysize = sizeof(GISTNodeBuffer);
	hashCtl.hcxt = CurrentMemoryContext;
	gfbb->nodeBuffersTab = hash_create("gistbuildbuffers",
									   1024,
									   &hashCtl,
									   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	gfbb->bufferEmptyingQueue = NIL;

	/*
	 * 供最终清空缓冲区过程使用的按层节点缓冲区列表。节点
	 * 缓冲区在创建时插入此处。
	 */
	gfbb->buffersOnLevelsLen = 1;
	gfbb->buffersOnLevels = (List **) palloc(sizeof(List *) *
											 gfbb->buffersOnLevelsLen);
	gfbb->buffersOnLevels[0] = NIL;

	/*
	 * 末页当前已装入主存的节点缓冲区的
	 * 块号。
	 */
	gfbb->loadedBuffersLen = 32;
	gfbb->loadedBuffers = (GISTNodeBuffer **) palloc(gfbb->loadedBuffersLen *
													 sizeof(GISTNodeBuffer *));
	gfbb->loadedBuffersCount = 0;

	gfbb->rootlevel = maxLevel;

	return gfbb;
}

/*
 * 返回给定块的节点缓冲区。若尚不存在则
 * 创建它。
 */
GISTNodeBuffer *
gistGetNodeBuffer(GISTBuildBuffers *gfbb, GISTSTATE *giststate,
				  BlockNumber nodeBlocknum, int level)
{
	GISTNodeBuffer *nodeBuffer;
	bool		found;

	/* 在哈希表中查找节点缓冲区 */
	nodeBuffer = (GISTNodeBuffer *) hash_search(gfbb->nodeBuffersTab,
												&nodeBlocknum,
												HASH_ENTER,
												&found);
	if (!found)
	{
		/*
		 * 未找到节点缓冲区。将新缓冲区初始化为空。
		 */
		MemoryContext oldcxt = MemoryContextSwitchTo(gfbb->context);

		/* nodeBuffer->nodeBlocknum 是哈希键且已填入 */
		nodeBuffer->blocksCount = 0;
		nodeBuffer->pageBlocknum = InvalidBlockNumber;
		nodeBuffer->pageBuffer = NULL;
		nodeBuffer->queuedForEmptying = false;
		nodeBuffer->isTemp = false;
		nodeBuffer->level = level;

		/*
		 * 把此缓冲区加入本层的缓冲区列表。若需要则
		 * 扩大 buffersOnLevels 数组。
		 */
		if (level >= gfbb->buffersOnLevelsLen)
		{
			int			i;

			gfbb->buffersOnLevels =
				(List **) repalloc(gfbb->buffersOnLevels,
								   (level + 1) * sizeof(List *));

			/* 初始化扩大后的部分 */
			for (i = gfbb->buffersOnLevelsLen; i <= level; i++)
				gfbb->buffersOnLevels[i] = NIL;
			gfbb->buffersOnLevelsLen = level + 1;
		}

		/*
		 * 把新缓冲区前置到本层缓冲区列表的开头。把
		 * 新缓冲区放在列表开头并非随意：在最终清空
		 * 阶段我们会遍历每层的全部缓冲区，并冲刷
		 * 它们。若清空期间发生页分裂，先冲刷因分裂
		 * 新建的页，再处理该层上原有的页更高效。页
		 * 分裂期间刚创建的缓冲区很可能仍在缓存中，
		 * 因此立即冲刷它们比放到队列
		 * 末尾更
		 * 高效。
		 */
		gfbb->buffersOnLevels[level] = lcons(nodeBuffer,
											 gfbb->buffersOnLevels[level]);

		MemoryContextSwitchTo(oldcxt);
	}

	return nodeBuffer;
}

/*
 * 为缓冲区页分配内存。
 */
static GISTNodeBufferPage *
gistAllocateNewPageBuffer(GISTBuildBuffers *gfbb)
{
	GISTNodeBufferPage *pageBuffer;

	pageBuffer = (GISTNodeBufferPage *) MemoryContextAllocZero(gfbb->context,
															   BLCKSZ);
	pageBuffer->prev = InvalidBlockNumber;

	/* 设置页空闲空间 */
	PAGE_FREE_SPACE(pageBuffer) = BLCKSZ - BUFFER_PAGE_DATA_OFFSET;
	return pageBuffer;
}

/*
 * 把指定缓冲区加入 loadedBuffers 数组。
 */
static void
gistAddLoadedBuffer(GISTBuildBuffers *gfbb, GISTNodeBuffer *nodeBuffer)
{
	/* 切勿把临时缓冲区加入该数组 */
	if (nodeBuffer->isTemp)
		return;

	/* 若需要则扩大数组 */
	if (gfbb->loadedBuffersCount >= gfbb->loadedBuffersLen)
	{
		gfbb->loadedBuffersLen *= 2;
		gfbb->loadedBuffers = (GISTNodeBuffer **)
			repalloc(gfbb->loadedBuffers,
					 gfbb->loadedBuffersLen * sizeof(GISTNodeBuffer *));
	}

	gfbb->loadedBuffers[gfbb->loadedBuffersCount] = nodeBuffer;
	gfbb->loadedBuffersCount++;
}

/*
 * 把节点缓冲区的末页装入主存。
 */
static void
gistLoadNodeBuffer(GISTBuildBuffers *gfbb, GISTNodeBuffer *nodeBuffer)
{
	/* 检查是否真的需要装入 */
	if (!nodeBuffer->pageBuffer && nodeBuffer->blocksCount > 0)
	{
		/* 为页分配内存 */
		nodeBuffer->pageBuffer = gistAllocateNewPageBuffer(gfbb);

		/* 从临时文件读取块 */
		ReadTempFileBlock(gfbb->pfile, nodeBuffer->pageBlocknum,
						  nodeBuffer->pageBuffer);

		/* 把文件块标为空闲 */
		gistBuffersReleaseBlock(gfbb, nodeBuffer->pageBlocknum);

		/* 把节点缓冲区标为已装入 */
		gistAddLoadedBuffer(gfbb, nodeBuffer);
		nodeBuffer->pageBlocknum = InvalidBlockNumber;
	}
}

/*
 * 把节点缓冲区的末页写到磁盘。
 */
static void
gistUnloadNodeBuffer(GISTBuildBuffers *gfbb, GISTNodeBuffer *nodeBuffer)
{
	/* 检查是否有内容要写 */
	if (nodeBuffer->pageBuffer)
	{
		BlockNumber blkno;

		/* 获取空闲文件块 */
		blkno = gistBuffersGetFreeBlock(gfbb);

		/* 把块写入临时文件 */
		WriteTempFileBlock(gfbb->pfile, blkno, nodeBuffer->pageBuffer);

		/* 释放该页的内存 */
		pfree(nodeBuffer->pageBuffer);
		nodeBuffer->pageBuffer = NULL;

		/* 保存块号 */
		nodeBuffer->pageBlocknum = blkno;
	}
}

/*
 * 把全部节点缓冲区的末页写到磁盘。
 */
void
gistUnloadNodeBuffers(GISTBuildBuffers *gfbb)
{
	int			i;

	/* 卸下所有在内存中装有一页的缓冲区。 */
	for (i = 0; i < gfbb->loadedBuffersCount; i++)
		gistUnloadNodeBuffer(gfbb, gfbb->loadedBuffers[i]);

	/* 现在已无装有末页的节点缓冲区 */
	gfbb->loadedBuffersCount = 0;
}

/*
 * 把索引元组加入缓冲区页。
 */
static void
gistPlaceItupToPage(GISTNodeBufferPage *pageBuffer, IndexTuple itup)
{
	Size		itupsz = IndexTupleSize(itup);
	char	   *ptr;

	/* 应有足够空间。 */
	Assert(PAGE_FREE_SPACE(pageBuffer) >= MAXALIGN(itupsz));

	/* 减小页的空闲空间值，为元组预留位置。 */
	PAGE_FREE_SPACE(pageBuffer) -= MAXALIGN(itupsz);

	/* 获取指向所预留位置的指针（即空闲空间末尾）。 */
	ptr = (char *) pageBuffer + BUFFER_PAGE_DATA_OFFSET
		+ PAGE_FREE_SPACE(pageBuffer);

	/* 把索引元组复制到那里。 */
	memcpy(ptr, itup, itupsz);
}

/*
 * 从缓冲区页取出最后一项并从页中移除。
 */
static void
gistGetItupFromPage(GISTNodeBufferPage *pageBuffer, IndexTuple *itup)
{
	IndexTuple	ptr;
	Size		itupsz;

	Assert(!PAGE_IS_EMPTY(pageBuffer)); /* 页不应为空 */

	/* 获取指向最后一个索引元组的指针 */
	ptr = (IndexTuple) ((char *) pageBuffer
						+ BUFFER_PAGE_DATA_OFFSET
						+ PAGE_FREE_SPACE(pageBuffer));
	itupsz = IndexTupleSize(ptr);

	/* 制作元组的副本 */
	*itup = (IndexTuple) palloc(itupsz);
	memcpy(*itup, ptr, itupsz);

	/* 把该元组占用的空间标为空闲 */
	PAGE_FREE_SPACE(pageBuffer) += MAXALIGN(itupsz);
}

/*
 * 把索引元组压入节点缓冲区。
 */
void
gistPushItupToNodeBuffer(GISTBuildBuffers *gfbb, GISTNodeBuffer *nodeBuffer,
						 IndexTuple itup)
{
	/*
	 * 大部分内存操作将在缓冲构建的持久
	 * 上下文中进行。因此切换到该上下文。
	 */
	MemoryContext oldcxt = MemoryContextSwitchTo(gfbb->context);

	/*
	 * 若缓冲区当前为空，则创建第一页。
	 */
	if (nodeBuffer->blocksCount == 0)
	{
		nodeBuffer->pageBuffer = gistAllocateNewPageBuffer(gfbb);
		nodeBuffer->blocksCount = 1;
		gistAddLoadedBuffer(gfbb, nodeBuffer);
	}

	/* 若节点缓冲区末页尚不在内存中则装入 */
	if (!nodeBuffer->pageBuffer)
		gistLoadNodeBuffer(gfbb, nodeBuffer);

	/*
	 * 检查末页上是否有容纳该元组的足够空间。
	 */
	if (PAGE_NO_SPACE(nodeBuffer->pageBuffer, itup))
	{
		/*
		 * 没有。把前一块换出到磁盘并分配新块。
		 */
		BlockNumber blkno;

		/* 把已填满的页写到磁盘 */
		blkno = gistBuffersGetFreeBlock(gfbb);
		WriteTempFileBlock(gfbb->pfile, blkno, nodeBuffer->pageBuffer);

		/*
		 * 把内存中的页重置为空，并通过在 prev 链接中
		 * 存储其块号，把前一块链接到新页。
		 */
		PAGE_FREE_SPACE(nodeBuffer->pageBuffer) =
			BLCKSZ - MAXALIGN(offsetof(GISTNodeBufferPage, tupledata));
		nodeBuffer->pageBuffer->prev = blkno;

		/* 我们刚又增加了一页 */
		nodeBuffer->blocksCount++;
	}

	gistPlaceItupToPage(nodeBuffer->pageBuffer, itup);

	/*
	 * 若缓冲区刚刚溢出，则把它加入清空队列。
	 */
	if (BUFFER_HALF_FILLED(nodeBuffer, gfbb) && !nodeBuffer->queuedForEmptying)
	{
		gfbb->bufferEmptyingQueue = lcons(nodeBuffer,
										  gfbb->bufferEmptyingQueue);
		nodeBuffer->queuedForEmptying = true;
	}

	/* 恢复内存上下文 */
	MemoryContextSwitchTo(oldcxt);
}

/*
 * 从节点缓冲区移除一个索引元组。成功返回 true，若
 * 节点缓冲区为空则返回 false。
 */
bool
gistPopItupFromNodeBuffer(GISTBuildBuffers *gfbb, GISTNodeBuffer *nodeBuffer,
						  IndexTuple *itup)
{
	/*
	 * 若节点缓冲区为空则返回 false。
	 */
	if (nodeBuffer->blocksCount <= 0)
		return false;

	/* 若需要则装入节点缓冲区末页 */
	if (!nodeBuffer->pageBuffer)
		gistLoadNodeBuffer(gfbb, nodeBuffer);

	/*
	 * 从最后一个非空页获取索引元组。
	 */
	gistGetItupFromPage(nodeBuffer->pageBuffer, itup);

	/*
	 * 若刚从该页移除最后一个元组，则取回本节点缓冲区上
	 * 的前一页（若有）。
	 */
	if (PAGE_IS_EMPTY(nodeBuffer->pageBuffer))
	{
		BlockNumber prevblkno;

		/*
		 * blocksCount 包含 pageBuffer 中的页，因此现在减小它。
		 */
		nodeBuffer->blocksCount--;

		/*
		 * 若还有更多页，则取回前一页。
		 */
		prevblkno = nodeBuffer->pageBuffer->prev;
		if (prevblkno != InvalidBlockNumber)
		{
			/* 存在前一页。取回它。 */
			Assert(nodeBuffer->blocksCount > 0);
			ReadTempFileBlock(gfbb->pfile, prevblkno, nodeBuffer->pageBuffer);

			/*
			 * 现已把该块读入内存，可以释放其
			 * 磁盘块以便重用。
			 */
			gistBuffersReleaseBlock(gfbb, prevblkno);
		}
		else
		{
			/* 没有更多页。释放内存。 */
			Assert(nodeBuffer->blocksCount == 0);
			pfree(nodeBuffer->pageBuffer);
			nodeBuffer->pageBuffer = NULL;
		}
	}
	return true;
}

/*
 * 选择一个当前未使用的块供写入。
 */
static long
gistBuffersGetFreeBlock(GISTBuildBuffers *gfbb)
{
	/*
	 * 若有多个空闲块，我们选择 freeBlocks[] 中出现在
	 * 最后的那个。若一个都没有，则分配文件末尾的下一块
	 * （导致文件扩展）。
	 */
	if (gfbb->nFreeBlocks > 0)
		return gfbb->freeBlocks[--gfbb->nFreeBlocks];
	else
		return gfbb->nFileBlocks++;
}

/*
 * 把块号归还到空闲列表。
 */
static void
gistBuffersReleaseBlock(GISTBuildBuffers *gfbb, long blocknum)
{
	int			ndx;

	/* 若 freeBlocks 数组已满则扩大。 */
	if (gfbb->nFreeBlocks >= gfbb->freeBlocksLen)
	{
		gfbb->freeBlocksLen *= 2;
		gfbb->freeBlocks = (long *) repalloc(gfbb->freeBlocks,
											 gfbb->freeBlocksLen *
											 sizeof(long));
	}

	/* 把 blocknum 加入数组 */
	ndx = gfbb->nFreeBlocks++;
	gfbb->freeBlocks[ndx] = blocknum;
}

/*
 * 释放缓冲构建数据结构。
 */
void
gistFreeBuildBuffers(GISTBuildBuffers *gfbb)
{
	/* 关闭缓冲区文件。 */
	BufFileClose(gfbb->pfile);

	/* 其余内容将在内存上下文释放时一并释放 */
}

/*
 * 表示从已分裂节点缓冲区重定位索引元组时，关于
 * 节点缓冲区信息的数据结构。
 */
typedef struct
{
	GISTENTRY	entry[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	GISTPageSplitInfo *splitinfo;
	GISTNodeBuffer *nodeBuffer;
} RelocationBufferInfo;

/*
 * 在页分裂时，把被分裂页缓冲区中的元组分配到
 * 为所创建页半部准备的新缓冲区。这也会调整
 * 'splitinfo' 中的下行指针，以包含缓冲区中的元组。
 */
void
gistRelocateBuildBuffersOnSplit(GISTBuildBuffers *gfbb, GISTSTATE *giststate,
								Relation r, int level,
								Buffer buffer, List *splitinfo)
{
	RelocationBufferInfo *relocationBuffersInfos;
	bool		found;
	GISTNodeBuffer *nodeBuffer;
	BlockNumber blocknum;
	IndexTuple	itup;
	int			splitPagesCount = 0;
	GISTENTRY	entry[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	GISTNodeBuffer oldBuf;
	ListCell   *lc;

	/* 若被分裂的页没有缓冲区，则无事可做。 */
	if (!LEVEL_HAS_BUFFERS(level, gfbb))
		return;

	/*
	 * 获取被分裂页的节点缓冲区。
	 */
	blocknum = BufferGetBlockNumber(buffer);
	nodeBuffer = hash_search(gfbb->nodeBuffersTab, &blocknum,
							 HASH_FIND, &found);
	if (!found)
	{
		/* 该页没有缓冲区，因此无事可做。 */
		return;
	}

	/*
	 * 制作旧缓冲区的副本，因为我们将把它重用为新左页的
	 * 缓冲区，而新左页与旧页在同一块上。
	 * 对根页并非如此，但没关系，因为我们从不在根页上
	 * 设置缓冲区。Arge 等人描述的原算法有过，但并无
	 * 用处，因为不如直接从堆读取元组，而不是从根
	 * 缓冲区读。
	 */
	Assert(blocknum != GIST_ROOT_BLKNO);
	memcpy(&oldBuf, nodeBuffer, sizeof(GISTNodeBuffer));
	oldBuf.isTemp = true;

	/* 重置旧缓冲区，此后用作新左页的缓冲区 */
	nodeBuffer->blocksCount = 0;
	nodeBuffer->pageBuffer = NULL;
	nodeBuffer->pageBlocknum = InvalidBlockNumber;

	/*
	 * 为重定位缓冲区的信息分配内存。
	 */
	splitPagesCount = list_length(splitinfo);
	relocationBuffersInfos =
		(RelocationBufferInfo *) palloc(sizeof(RelocationBufferInfo) *
										splitPagesCount);

	/*
	 * 为分裂产生的各页的节点缓冲区填充
	 * 重定位缓冲区信息。
	 */
	foreach(lc, splitinfo)
	{
		GISTPageSplitInfo *si = (GISTPageSplitInfo *) lfirst(lc);
		GISTNodeBuffer *newNodeBuffer;
		int			i = foreach_current_index(lc);

		/* 解压缩节点缓冲区页的父索引元组。 */
		gistDeCompressAtt(giststate, r,
						  si->downlink, NULL, (OffsetNumber) 0,
						  relocationBuffersInfos[i].entry,
						  relocationBuffersInfos[i].isnull);

		/*
		 * 为该页创建节点缓冲区。最左半部与分裂前的旧页
		 * 在同一块上，因此对最左半部这将返回原缓冲区。
		 * 原缓冲区上的元组已重链到临时缓冲区，因此
		 * 原缓冲区现在
		 * 为空。
		 */
		newNodeBuffer = gistGetNodeBuffer(gfbb, giststate, BufferGetBlockNumber(si->buf), level);

		relocationBuffersInfos[i].nodeBuffer = newNodeBuffer;
		relocationBuffersInfos[i].splitinfo = si;
	}

	/*
	 * 遍历正被分裂的页的缓冲区中全部索引元组，
	 * 把它们移到新页的缓冲区。我们尽量把每个元组移到
	 * 会使前导列惩罚最低的页；若并列，则移到最早
	 * 不并列的那一列惩罚最低的页。
	 *
	 * 页搜索逻辑与 gistchoose()
	 * 非常相似。
	 */
	while (gistPopItupFromNodeBuffer(gfbb, &oldBuf, &itup))
	{
		float		best_penalty[INDEX_MAX_KEYS];
		int			i,
					which;
		IndexTuple	newtup;
		RelocationBufferInfo *targetBufferInfo;

		gistDeCompressAtt(giststate, r,
						  itup, NULL, (OffsetNumber) 0, entry, isnull);

		/* 默认使用第一页（不应有影响） */
		which = 0;

		/*
		 * best_penalty[j] 是迄今为止在第 j 列见过的最佳
		 * 惩罚，尚未检查第 j 列时为 -1。第一个 -1
		 * 右侧的数组元素未定义。
		 */
		best_penalty[0] = -1;

		/*
		 * 遍历可能的目标页，寻找要把此元组移到
		 * 的那一页。
		 */
		for (i = 0; i < splitPagesCount; i++)
		{
			RelocationBufferInfo *splitPageInfo = &relocationBuffersInfos[i];
			bool		zero_penalty;
			int			j;

			zero_penalty = true;

			/* 遍历索引属性。 */
			for (j = 0; j < IndexRelationGetNumberOfKeyAttributes(r); j++)
			{
				float		usize;

				/* 计算该列的惩罚。 */
				usize = gistpenalty(giststate, j,
									&splitPageInfo->entry[j],
									splitPageInfo->isnull[j],
									&entry[j], isnull[j]);
				if (usize > 0)
					zero_penalty = false;

				if (best_penalty[j] < 0 || usize < best_penalty[j])
				{
					/*
					 * 该列出现新的最佳惩罚。暂定选择此
					 * 页作为目标，并记下最佳惩罚。然后
					 * 将下一列的惩罚重置为 "unknown"（并
					 * 间接地，对其右侧所有列同样处理）。
					 * 这将迫使我们在后续循环迭代中采用
					 * 此页的惩罚值作为其余各列的
					 * 最佳值。
					 */
					which = i;
					best_penalty[j] = usize;

					if (j < IndexRelationGetNumberOfKeyAttributes(r) - 1)
						best_penalty[j + 1] = -1;
				}
				else if (best_penalty[j] == usize)
				{
					/*
					 * 当前页在该列上与迄今最佳页
					 * 同样好。本循环的下一次迭代
					 * 将比较下一列。
					 */
				}
				else
				{
					/*
					 * 当前页在该列上比迄今最佳
					 * 页更差。跳过其余列，继续
					 * 处理下一页（若有）。
					 */
					zero_penalty = false;	/* 使外层循环不会退出 */
					break;
				}
			}

			/*
			 * 若找到各列惩罚均为零的页，则无需再
			 * 检查其余页；直接跳出循环并
			 * 返回它。
			 */
			if (zero_penalty)
				break;
		}

		/* 好，"which" 是要把元组压入的页索引 */
		targetBufferInfo = &relocationBuffersInfos[which];

		/* 把项压入所选节点缓冲区 */
		gistPushItupToNodeBuffer(gfbb, targetBufferInfo->nodeBuffer, itup);

		/* 若需要，调整该页的下行指针。 */
		newtup = gistgetadjusted(r, targetBufferInfo->splitinfo->downlink,
								 itup, giststate);
		if (newtup)
		{
			gistDeCompressAtt(giststate, r,
							  newtup, NULL, (OffsetNumber) 0,
							  targetBufferInfo->entry,
							  targetBufferInfo->isnull);

			targetBufferInfo->splitinfo->downlink = newtup;
		}
	}

	pfree(relocationBuffersInfos);
}


/*
 * BufFile 操作的包装。主要区别是这些包装用
 * ereport() 报告错误，因此调用者不必
 * 检查返回码。
 */

static void
ReadTempFileBlock(BufFile *file, long blknum, void *ptr)
{
	if (BufFileSeekBlock(file, blknum) != 0)
		elog(ERROR, "could not seek to block %ld in temporary file", blknum);
	BufFileReadExact(file, ptr, BLCKSZ);
}

static void
WriteTempFileBlock(BufFile *file, long blknum, const void *ptr)
{
	if (BufFileSeekBlock(file, blknum) != 0)
		elog(ERROR, "could not seek to block %ld in temporary file", blknum);
	BufFileWrite(file, ptr, BLCKSZ);
}
