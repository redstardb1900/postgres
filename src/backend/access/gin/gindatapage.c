/*-------------------------------------------------------------------------
 *
 * gindatapage.c
 *	  处理 GIN 倒排树页的例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/gindatapage.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/ginxlog.h"
#include "access/xloginsert.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "storage/predicate.h"
#include "utils/rel.h"

/*
 * 叶子页上所存倒排列表的最小、最大与目标尺寸，以字节计。
 *
 * 代码能处理任意尺寸，但随机访问在存放若干较小
 * 列表、而不是一个大列表时更高效。若插入会使
 * 倒排列表超过最大尺寸，则把它分裂为两个。若
 * 倒排列表会小于最小尺寸，则把它与下一个倒排
 * 列表合并。
 */
#define GinPostingListSegmentMaxSize 384
#define GinPostingListSegmentTargetSize 256
#define GinPostingListSegmentMinSize 128

/*
 * 至少这么多项能放进一个 GinPostingListSegmentMaxSize
 * 字节长的段。用于估计 N 项至少需要多少
 * 空间。
 */
#define MinTuplesPerSegment ((GinPostingListSegmentMaxSize - 2) / 6)

/*
 * 用于操作倒排树叶子页的工作结构。
 */
typedef struct
{
	dlist_head	segments;		/* leafSegmentInfo 的列表 */

	/*
	 * 若需要页分裂，下列字段表示各段如何分到各页上。
	 * 由 leafRepackItems 填写。
	 */
	dlist_node *lastleft;		/* 左页上的最后一段 */
	int			lsize;			/* 左页上的总尺寸 */
	int			rsize;			/* 右页上的总尺寸 */

	bool		oldformat;		/* 页在磁盘上为 9.4 之前的格式 */

	/*
	 * 若需要表示重建后叶子页的 WAL 数据，由
	 * computeLeafRecompressWALData 存放在这里。
	 */
	char	   *walinfo;		/* 缓冲区起点 */
	int			walinfolen;		/* 以及长度 */
} disassembledLeaf;

typedef struct
{
	dlist_node	node;			/* 链表指针 */

	/*-------------
	 * 'action' 表示此内存中段相对于磁盘上内容的状态。
	 * 它是下列 GIN_SEGMENT_* 动作码之一：
	 *
	 * UNMODIFIED	无更改
	 * DELETE		该段将被移除。'seg' 与 'items' 被
	 *				忽略
	 * INSERT		这是一个全新的段
	 * REPLACE		用新内容替换现有段
	 * ADDITEMS		类似 REPLACE，但未移除任何项，且我们
	 *				在 'modifieditems' 中详细跟踪向该段
	 *				添加了哪些项
	 *-------------
	 */
	char		action;

	ItemPointerData *modifieditems;
	uint16		nmodifieditems;

	/*
	 * 下列字段表示本段中的项。若 'items' 非 NULL，则它含有
	 * 本段项的 palloc 数组。若 'seg' 非 NULL，则它以已压缩
	 * 格式含有这些项。它可以指向磁盘页（!modified），或
	 * 指向内存中 palloc 的段。若两者都设置，则必须表示
	 * 相同的项。
	 */
	GinPostingList *seg;
	ItemPointer items;
	int			nitems;			/* 'items' 中的项数（若 items != NULL） */
} leafSegmentInfo;

static ItemPointer dataLeafPageGetUncompressed(Page page, int *nitems);
static void dataSplitPageInternal(GinBtree btree, Buffer origbuf,
								  GinBtreeStack *stack,
								  void *insertdata, BlockNumber updateblkno,
								  Page *newlpage, Page *newrpage);

static disassembledLeaf *disassembleLeaf(Page page);
static bool leafRepackItems(disassembledLeaf *leaf, ItemPointer remaining);
static bool addItemsToLeaf(disassembledLeaf *leaf, ItemPointer newItems,
						   int nNewItems);

static void computeLeafRecompressWALData(disassembledLeaf *leaf);
static void dataPlaceToPageLeafRecompress(Buffer buf, disassembledLeaf *leaf);
static void dataPlaceToPageLeafSplit(disassembledLeaf *leaf,
									 ItemPointerData lbound, ItemPointerData rbound,
									 Page lpage, Page rpage);

/*
 * 从叶子数据页读取 TIDs 到单个未压缩数组。TIDs 按
 * 升序返回。
 *
 * advancePast 是一个提示，表示调用者只关心
 * TID > advancePast。若要返回全部项，使用
 * ItemPointerSetMin。
 *
 * 注意：本函数仍可能返回小于 advancePast、但与感兴趣项
 * 处于同一倒排列表中的项，因此调用者仍须检查所有返回项。
 * 但传入该提示可让本函数跳过整段倒排列表。
 */
ItemPointer
GinDataLeafPageGetItems(Page page, int *nitems, ItemPointerData advancePast)
{
	ItemPointer result;

	if (GinPageIsCompressed(page))
	{
		GinPostingList *seg = GinDataLeafPageGetPostingList(page);
		Size		len = GinDataLeafPageGetPostingListSize(page);
		Pointer		endptr = ((Pointer) seg) + len;
		GinPostingList *next;

		/* 跳到含有 advancePast+1 的段 */
		if (ItemPointerIsValid(&advancePast))
		{
			next = GinNextPostingListSegment(seg);
			while ((Pointer) next < endptr &&
				   ginCompareItemPointers(&next->first, &advancePast) <= 0)
			{
				seg = next;
				next = GinNextPostingListSegment(seg);
			}
			len = endptr - (Pointer) seg;
		}

		if (len > 0)
			result = ginPostingListDecodeAllSegments(seg, len, nitems);
		else
		{
			result = NULL;
			*nitems = 0;
		}
	}
	else
	{
		ItemPointer tmp = dataLeafPageGetUncompressed(page, nitems);

		result = palloc((*nitems) * sizeof(ItemPointerData));
		memcpy(result, tmp, (*nitems) * sizeof(ItemPointerData));
	}

	return result;
}

/*
 * 把叶子数据页上的全部 TIDs 放入位图。
 */
int
GinDataLeafPageGetItemsToTbm(Page page, TIDBitmap *tbm)
{
	ItemPointer uncompressed;
	int			nitems;

	if (GinPageIsCompressed(page))
	{
		GinPostingList *segment = GinDataLeafPageGetPostingList(page);
		Size		len = GinDataLeafPageGetPostingListSize(page);

		nitems = ginPostingListDecodeAllSegmentsToTbm(segment, len, tbm);
	}
	else
	{
		uncompressed = dataLeafPageGetUncompressed(page, &nitems);

		if (nitems > 0)
			tbm_add_tuples(tbm, uncompressed, nitems, false);
	}

	return nitems;
}

/*
 * 获取 9.4 之前格式未压缩叶子页上未压缩项数组的
 * 指针。数组中的项数经 *nitems
 * 返回。
 */
static ItemPointer
dataLeafPageGetUncompressed(Page page, int *nitems)
{
	ItemPointer items;

	Assert(!GinPageIsCompressed(page));

	/*
	 * 在旧的 9.4 之前页格式中，整个页内容用于存放未压缩项，
	 * 项数存放在 'maxoff' 中
	 */
	items = (ItemPointer) GinDataPageGetData(page);
	*nitems = GinPageGetOpaque(page)->maxoff;

	return items;
}

/*
 * 检查是否应跟随右链去找正在搜索的
 * 项。
 *
 * 把正在插入的项指针与当前页的右界比较。
 */
static bool
dataIsMoveRight(GinBtree btree, Page page)
{
	ItemPointer iptr = GinDataPageGetRightBound(page);

	if (GinPageRightMost(page))
		return false;

	if (GinPageIsDeleted(page))
		return true;

	return (ginCompareItemPointers(&btree->itemptr, iptr) > 0);
}

/*
 * 在非叶子页中找到正确的 PostingItem。假定这已是正确的页，
 * 且所搜索的值应当（SHOULD）在该页上。
 */
static BlockNumber
dataLocateItem(GinBtree btree, GinBtreeStack *stack)
{
	OffsetNumber low,
				high,
				maxoff;
	PostingItem *pitem = NULL;
	int			result;
	Page		page = BufferGetPage(stack->buffer);

	Assert(!GinPageIsLeaf(page));
	Assert(GinPageIsData(page));

	if (btree->fullScan)
	{
		stack->off = FirstOffsetNumber;
		stack->predictNumber *= GinPageGetOpaque(page)->maxoff;
		return btree->getLeftMostChild(btree, page);
	}

	low = FirstOffsetNumber;
	maxoff = high = GinPageGetOpaque(page)->maxoff;
	Assert(high >= low);

	high++;

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);

		pitem = GinDataPageGetPostingItem(page, mid);

		if (mid == maxoff)
		{
			/*
			 * 右无穷，页已在 dataIsMoveRight 的帮助下
			 * 正确选定
			 */
			result = -1;
		}
		else
		{
			pitem = GinDataPageGetPostingItem(page, mid);
			result = ginCompareItemPointers(&btree->itemptr, &(pitem->key));
		}

		if (result == 0)
		{
			stack->off = mid;
			return PostingItemGetBlockNumber(pitem);
		}
		else if (result > 0)
			low = mid + 1;
		else
			high = mid;
	}

	Assert(high >= FirstOffsetNumber && high <= maxoff);

	stack->off = high;
	pitem = GinDataPageGetPostingItem(page, high);
	return PostingItemGetBlockNumber(pitem);
}

/*
 * 在非叶子页上查找指向 blkno 的链接，返回 PostingItem 的偏移
 */
static OffsetNumber
dataFindChildPtr(GinBtree btree, Page page, BlockNumber blkno, OffsetNumber storedOff)
{
	OffsetNumber i,
				maxoff = GinPageGetOpaque(page)->maxoff;
	PostingItem *pitem;

	Assert(!GinPageIsLeaf(page));
	Assert(GinPageIsData(page));

	/* 若页未更改，则返回 storedOff */
	if (storedOff >= FirstOffsetNumber && storedOff <= maxoff)
	{
		pitem = GinDataPageGetPostingItem(page, storedOff);
		if (PostingItemGetBlockNumber(pitem) == blkno)
			return storedOff;

		/*
		 * 我们希望所需指针在右侧。若未曾发生删除，
		 * 则这一点成立
		 */
		for (i = storedOff + 1; i <= maxoff; i++)
		{
			pitem = GinDataPageGetPostingItem(page, i);
			if (PostingItemGetBlockNumber(pitem) == blkno)
				return i;
		}

		maxoff = storedOff - 1;
	}

	/* 最后机会 */
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		pitem = GinDataPageGetPostingItem(page, i);
		if (PostingItemGetBlockNumber(pitem) == blkno)
			return i;
	}

	return InvalidOffsetNumber;
}

/*
 * 返回最左子结点的 blkno
 */
static BlockNumber
dataGetLeftMostPage(GinBtree btree, Page page)
{
	PostingItem *pitem;

	Assert(!GinPageIsLeaf(page));
	Assert(GinPageIsData(page));
	Assert(GinPageGetOpaque(page)->maxoff >= FirstOffsetNumber);

	pitem = GinDataPageGetPostingItem(page, FirstOffsetNumber);
	return PostingItemGetBlockNumber(pitem);
}

/*
 * 向非叶子页添加 PostingItem。
 */
void
GinDataPageAddPostingItem(Page page, PostingItem *data, OffsetNumber offset)
{
	OffsetNumber maxoff = GinPageGetOpaque(page)->maxoff;
	char	   *ptr;

	Assert(PostingItemGetBlockNumber(data) != InvalidBlockNumber);
	Assert(!GinPageIsLeaf(page));

	if (offset == InvalidOffsetNumber)
	{
		ptr = (char *) GinDataPageGetPostingItem(page, maxoff + 1);
	}
	else
	{
		ptr = (char *) GinDataPageGetPostingItem(page, offset);
		if (offset != maxoff + 1)
			memmove(ptr + sizeof(PostingItem),
					ptr,
					(maxoff - offset + 1) * sizeof(PostingItem));
	}
	memcpy(ptr, data, sizeof(PostingItem));

	maxoff++;
	GinPageGetOpaque(page)->maxoff = maxoff;

	/*
	 * 同时把 pd_lower 设到倒排项末尾，以遵循
	 * "标准"页布局，从而能从整页映像中挤掉
	 * 未用空间。
	 */
	GinDataPageSetDataSize(page, maxoff * sizeof(PostingItem));
}

/*
 * 从非叶子页删除倒排项
 */
void
GinPageDeletePostingItem(Page page, OffsetNumber offset)
{
	OffsetNumber maxoff = GinPageGetOpaque(page)->maxoff;

	Assert(!GinPageIsLeaf(page));
	Assert(offset >= FirstOffsetNumber && offset <= maxoff);

	if (offset != maxoff)
		memmove(GinDataPageGetPostingItem(page, offset),
				GinDataPageGetPostingItem(page, offset + 1),
				sizeof(PostingItem) * (maxoff - offset));

	maxoff--;
	GinPageGetOpaque(page)->maxoff = maxoff;

	GinDataPageSetDataSize(page, maxoff * sizeof(PostingItem));
}

/*
 * 准备向叶子数据页插入数据。
 *
 * 若放得下，则在进入插入临界区前做完所需准备后返回
 * GPTP_INSERT。可将 *ptp_workspace 设为向
 * execPlaceToPage 函数传递信息。
 *
 * 若放不下，则执行页分裂，并把两个临时页映像返回到
 * *newlpage 与 *newrpage，结果为 GPTP_SPLIT。
 *
 * 无论哪种情况，此处都不应修改给定的页缓冲区。
 */
static GinPlaceToPageRC
dataBeginPlaceToPageLeaf(GinBtree btree, Buffer buf, GinBtreeStack *stack,
						 void *insertdata,
						 void **ptp_workspace,
						 Page *newlpage, Page *newrpage)
{
	GinBtreeDataLeafInsertData *items = insertdata;
	ItemPointer newItems = &items->items[items->curitem];
	int			maxitems = items->nitem - items->curitem;
	Page		page = BufferGetPage(buf);
	int			i;
	ItemPointerData rbound;
	ItemPointerData lbound;
	bool		needsplit;
	bool		append;
	int			segsize;
	Size		freespace;
	disassembledLeaf *leaf;
	leafSegmentInfo *lastleftinfo;
	ItemPointerData maxOldItem;
	ItemPointerData remaining;

	rbound = *GinDataPageGetRightBound(page);

	/*
	 * 统计有多少新项属于本页。
	 */
	if (!GinPageRightMost(page))
	{
		for (i = 0; i < maxitems; i++)
		{
			if (ginCompareItemPointers(&newItems[i], &rbound) > 0)
			{
				/*
				 * 此项需要去到树中的其他位置。（调用者
				 * 本应选好插入位置，使至少第一项
				 * 落在这里。）
				 */
				Assert(i > 0);
				break;
			}
		}
		maxitems = i;
	}

	/* 拆解页上的数据 */
	leaf = disassembleLeaf(page);

	/*
	 * 我们是在向页末追加吗？换言之，是否所有新项都
	 * 大于任何现有项。
	 */
	if (!dlist_is_empty(&leaf->segments))
	{
		lastleftinfo = dlist_container(leafSegmentInfo, node,
									   dlist_tail_node(&leaf->segments));
		if (!lastleftinfo->items)
			lastleftinfo->items = ginPostingListDecode(lastleftinfo->seg,
													   &lastleftinfo->nitems);
		maxOldItem = lastleftinfo->items[lastleftinfo->nitems - 1];
		if (ginCompareItemPointers(&newItems[0], &maxOldItem) >= 0)
			append = true;
		else
			append = false;
	}
	else
	{
		ItemPointerSetMin(&maxOldItem);
		append = true;
	}

	/*
	 * 若正在向页末追加，我们将追加尽可能多的项
	 * （在分裂之后），并在页变满时停止。否则必须
	 * 限制要插入的新项数，因为一旦开始打包就不能在
	 * 空间用尽时停下来——必须确保所有旧项仍然
	 * 放得下。
	 */
	if (GinPageIsCompressed(page))
		freespace = GinDataLeafPageGetFreeSpace(page);
	else
		freespace = 0;
	if (append)
	{
		/*
		 * 即使在追加时，尝试追加超过能放下的项也并非
		 * 完全免费，因为下面会把新旧项合并到一个数组。
		 * 最好情况下，每个新项只需一字节，且我们能用完
		 * 旧页与新页上的全部空闲空间。为简单起见，忽略
		 * 段开销等。
		 */
		maxitems = Min(maxitems, freespace + GinDataPageMaxDataSize);
	}
	else
	{
		/*
		 * 保守估计分裂后两个页上能放下多少新
		 * 项。
		 *
		 * 我们可用旧页上剩余空闲空间存放完整的
		 * 段，以及新页。每个满尺寸段至少能容纳
		 * MinTuplesPerSegment 项
		 */
		int			nnewsegments;

		nnewsegments = freespace / GinPostingListSegmentMaxSize;
		nnewsegments += GinDataPageMaxDataSize / GinPostingListSegmentMaxSize;
		maxitems = Min(maxitems, nnewsegments * MinTuplesPerSegment);
	}

	/* 把新项加入段列表 */
	if (!addItemsToLeaf(leaf, newItems, maxitems))
	{
		/* 全部项都是重复的，无事可做 */
		items->curitem += maxitems;

		return GPTP_NO_WORK;
	}

	/*
	 * 把项重新打包回压缩段，准备写入磁盘。
	 */
	needsplit = leafRepackItems(leaf, &remaining);

	/*
	 * 所有新项都放得下了吗？
	 *
	 * 若正在追加，放不下也可以。但作为健全性检查，
	 * 验证所有旧项都放得下。
	 */
	if (ItemPointerIsValid(&remaining))
	{
		if (!append || ItemPointerCompare(&maxOldItem, &remaining) >= 0)
			elog(ERROR, "could not split GIN page; all old items didn't fit");

		/* 统计有多少新项放得下了。 */
		for (i = 0; i < maxitems; i++)
		{
			if (ginCompareItemPointers(&newItems[i], &remaining) >= 0)
				break;
		}
		if (i == 0)
			elog(ERROR, "could not split GIN page; no new items fit");
		maxitems = i;
	}

	if (!needsplit)
	{
		/*
		 * 很好，所有项都能放进单页。若需要，则为描述
		 * 我们将做的更改的 WAL 记录准备数据。
		 */
		if (RelationNeedsWAL(btree->index) && !btree->isBuild)
			computeLeafRecompressWALData(leaf);

		/*
		 * 我们已准备好进入临界区，但
		 * dataExecPlaceToPageLeaf 将需要访问 "leaf" 数据。
		 */
		*ptp_workspace = leaf;

		if (append)
			elog(DEBUG2, "appended %d new items to block %u; %d bytes (%d to go)",
				 maxitems, BufferGetBlockNumber(buf), (int) leaf->lsize,
				 items->nitem - items->curitem - maxitems);
		else
			elog(DEBUG2, "inserted %d new items to block %u; %d bytes (%d to go)",
				 maxitems, BufferGetBlockNumber(buf), (int) leaf->lsize,
				 items->nitem - items->curitem - maxitems);
	}
	else
	{
		/*
		 * 必须分裂。
		 *
		 * leafRepackItems 已把各段分到左页
		 * 与右页。它尽量填满左页，其余放到右
		 * 页。在构建新索引时这很好，因为表从
		 * 始至终扫描，构建期间不会再向左页插
		 * 入。这样能把索引打包得尽可能紧。
		 * 但在其他情况下则按 50/50 分裂，把
		 * 段从左页移到右页，直到左右两页达
		 * 到平衡。
		 *
		 * 作为进一步的启发式，在向页末追加项
		 * 时，尽量让左页达到 75% 满，假定后
		 * 续插入也很可能落在末尾。这在向表追
		 * 加（很常见）时能把索引打包得稍
		 * 紧一些。
		 */
		if (!btree->isBuild)
		{
			while (dlist_has_prev(&leaf->segments, leaf->lastleft))
			{
				lastleftinfo = dlist_container(leafSegmentInfo, node, leaf->lastleft);

				/* 忽略已删除的段 */
				if (lastleftinfo->action != GIN_SEGMENT_DELETE)
				{
					segsize = SizeOfGinPostingList(lastleftinfo->seg);

					/*
					 * 注意：即使在追加时，我们也检查右页不会
					 * 变得比左页更满。有可能我们添加了足够多
					 * 的项，使两页都超过
					 * 75% 满。
					 */
					if ((leaf->lsize - segsize) - (leaf->rsize + segsize) < 0)
						break;
					if (append)
					{
						if ((leaf->lsize - segsize) < (BLCKSZ * 3) / 4)
							break;
					}

					leaf->lsize -= segsize;
					leaf->rsize += segsize;
				}
				leaf->lastleft = dlist_prev_node(&leaf->segments, leaf->lastleft);
			}
		}
		Assert(leaf->lsize <= GinDataPageMaxDataSize);
		Assert(leaf->rsize <= GinDataPageMaxDataSize);

		/*
		 * 取左页最后一段中的最大项；它成为该页的
		 * 右界。
		 */
		lastleftinfo = dlist_container(leafSegmentInfo, node, leaf->lastleft);
		if (!lastleftinfo->items)
			lastleftinfo->items = ginPostingListDecode(lastleftinfo->seg,
													   &lastleftinfo->nitems);
		lbound = lastleftinfo->items[lastleftinfo->nitems - 1];

		/*
		 * 现在分配几个临时页映像并填入它们。
		 */
		*newlpage = palloc(BLCKSZ);
		*newrpage = palloc(BLCKSZ);

		dataPlaceToPageLeafSplit(leaf, lbound, rbound,
								 *newlpage, *newrpage);

		Assert(GinPageRightMost(page) ||
			   ginCompareItemPointers(GinDataPageGetRightBound(*newlpage),
									  GinDataPageGetRightBound(*newrpage)) < 0);

		if (append)
			elog(DEBUG2, "appended %d items to block %u; split %d/%d (%d to go)",
				 maxitems, BufferGetBlockNumber(buf), (int) leaf->lsize, (int) leaf->rsize,
				 items->nitem - items->curitem - maxitems);
		else
			elog(DEBUG2, "inserted %d items to block %u; split %d/%d (%d to go)",
				 maxitems, BufferGetBlockNumber(buf), (int) leaf->lsize, (int) leaf->rsize,
				 items->nitem - items->curitem - maxitems);
	}

	items->curitem += maxitems;

	return needsplit ? GPTP_SPLIT : GPTP_INSERT;
}

/*
 * 在 beginPlaceToPage 已判定放得下之后执行数据插入。
 *
 * 在临界区内调用，且（若需要）XLOG 记录创建已
 * 开始。目标缓冲区注册在槽 0。
 */
static void
dataExecPlaceToPageLeaf(GinBtree btree, Buffer buf, GinBtreeStack *stack,
						void *insertdata, void *ptp_workspace)
{
	disassembledLeaf *leaf = (disassembledLeaf *) ptp_workspace;

	/* 把更改应用到页 */
	dataPlaceToPageLeafRecompress(buf, leaf);

	/* 若需要，注册由 computeLeafRecompressWALData 构建的 WAL 数据 */
	if (RelationNeedsWAL(btree->index) && !btree->isBuild)
	{
		XLogRegisterBufData(0, leaf->walinfo, leaf->walinfolen);
	}
}

/*
 * 清理倒排树叶子页。
 */
void
ginVacuumPostingTreeLeaf(Relation indexrel, Buffer buffer, GinVacuumState *gvs)
{
	Page		page = BufferGetPage(buffer);
	disassembledLeaf *leaf;
	bool		removedsomething = false;
	dlist_iter	iter;

	leaf = disassembleLeaf(page);

	/* 清理每一段。 */
	dlist_foreach(iter, &leaf->segments)
	{
		leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node, iter.cur);
		int			oldsegsize;
		ItemPointer cleaned;
		int			ncleaned;

		if (!seginfo->items)
			seginfo->items = ginPostingListDecode(seginfo->seg,
												  &seginfo->nitems);
		if (seginfo->seg)
			oldsegsize = SizeOfGinPostingList(seginfo->seg);
		else
			oldsegsize = GinDataPageMaxDataSize;

		cleaned = ginVacuumItemPointers(gvs,
										seginfo->items,
										seginfo->nitems,
										&ncleaned);
		pfree(seginfo->items);
		seginfo->items = NULL;
		seginfo->nitems = 0;
		if (cleaned)
		{
			if (ncleaned > 0)
			{
				int			npacked;

				seginfo->seg = ginCompressPostingList(cleaned,
													  ncleaned,
													  oldsegsize,
													  &npacked);
				/* 移除项绝不会使段的尺寸增大 */
				if (npacked != ncleaned)
					elog(ERROR, "could not fit vacuumed posting list");
				seginfo->action = GIN_SEGMENT_REPLACE;
			}
			else
			{
				seginfo->seg = NULL;
				seginfo->items = NULL;
				seginfo->action = GIN_SEGMENT_DELETE;
			}
			seginfo->nitems = ncleaned;

			removedsomething = true;
		}
	}

	/*
	 * 若移除了任何项，则从碎片重建该页。
	 *
	 * 我们不在这里尝试重新编码各段，即使其
	 * 中一些在移除项后可能已经很小。这似乎
	 * 是浪费力气，因为较大的段本身并无真正
	 * 好处；较大的段只是有助于在相同空间中
	 * 装入更多项。不妨推迟到下次插入时再做，
	 * 那时反正至少要重新编码页的一
	 * 部分。
	 *
	 * 另请注意：若页原先是未压缩的 9.4 之前格式，
	 * 现在会表示为一个包含全部项的巨大段。把它
	 * 分裂以加快随机访问或许有意义，但我们不费
	 * 这个劲。若想充分获得新紧凑索引格式的收益，
	 * 反正得做 REINDEX。
	 */
	if (removedsomething)
	{
		bool		modified;

		/*
		 * 确保从第一个被修改的段起，其后所有段都有
		 * palloc 副本。（dataPlaceToPageLeafRecompress
		 * 需要如此）。
		 */
		modified = false;
		dlist_foreach(iter, &leaf->segments)
		{
			leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node,
													   iter.cur);

			if (seginfo->action != GIN_SEGMENT_UNMODIFIED)
				modified = true;
			if (modified && seginfo->action != GIN_SEGMENT_DELETE)
			{
				int			segsize = SizeOfGinPostingList(seginfo->seg);
				GinPostingList *tmp = (GinPostingList *) palloc(segsize);

				memcpy(tmp, seginfo->seg, segsize);
				seginfo->seg = tmp;
			}
		}

		if (RelationNeedsWAL(indexrel))
			computeLeafRecompressWALData(leaf);

		/* 把更改应用到页 */
		START_CRIT_SECTION();

		dataPlaceToPageLeafRecompress(buffer, leaf);

		MarkBufferDirty(buffer);

		if (RelationNeedsWAL(indexrel))
		{
			XLogRecPtr	recptr;

			XLogBeginInsert();
			XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
			XLogRegisterBufData(0, leaf->walinfo, leaf->walinfolen);
			recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_VACUUM_DATA_LEAF_PAGE);
			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();
	}
}

/*
 * 构造表示 *leaf 中更改的 ginxlogRecompressDataLeaf 记录。
 * （因为这需要 palloc，我们必须在进入实际更新页的
 * 临界区之前完成。）
 */
static void
computeLeafRecompressWALData(disassembledLeaf *leaf)
{
	int			nmodified = 0;
	char	   *walbufbegin;
	char	   *walbufend;
	dlist_iter	iter;
	int			segno;
	ginxlogRecompressDataLeaf *recompress_xlog;

	/* 统计已修改的段 */
	dlist_foreach(iter, &leaf->segments)
	{
		leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node,
												   iter.cur);

		if (seginfo->action != GIN_SEGMENT_UNMODIFIED)
			nmodified++;
	}

	walbufbegin =
		palloc(sizeof(ginxlogRecompressDataLeaf) +
			   BLCKSZ +			/* 存放段数据所需的最大尺寸 */
			   nmodified * 2	/* 每个动作的 (segno + action) */
		);
	walbufend = walbufbegin;

	recompress_xlog = (ginxlogRecompressDataLeaf *) walbufend;
	walbufend += sizeof(ginxlogRecompressDataLeaf);

	recompress_xlog->nactions = nmodified;

	segno = 0;
	dlist_foreach(iter, &leaf->segments)
	{
		leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node,
												   iter.cur);
		int			segsize = 0;
		int			datalen;
		uint8		action = seginfo->action;

		if (action == GIN_SEGMENT_UNMODIFIED)
		{
			segno++;
			continue;
		}

		if (action != GIN_SEGMENT_DELETE)
			segsize = SizeOfGinPostingList(seginfo->seg);

		/*
		 * 若存放所添加项指针的未压缩列表会比按原样
		 * 存放压缩段占用更多空间，则改为那样
		 * 做。
		 */
		if (action == GIN_SEGMENT_ADDITEMS &&
			seginfo->nmodifieditems * sizeof(ItemPointerData) > segsize)
		{
			action = GIN_SEGMENT_REPLACE;
		}

		*((uint8 *) (walbufend++)) = segno;
		*(walbufend++) = action;

		switch (action)
		{
			case GIN_SEGMENT_DELETE:
				datalen = 0;
				break;

			case GIN_SEGMENT_ADDITEMS:
				datalen = seginfo->nmodifieditems * sizeof(ItemPointerData);
				memcpy(walbufend, &seginfo->nmodifieditems, sizeof(uint16));
				memcpy(walbufend + sizeof(uint16), seginfo->modifieditems, datalen);
				datalen += sizeof(uint16);
				break;

			case GIN_SEGMENT_INSERT:
			case GIN_SEGMENT_REPLACE:
				datalen = SHORTALIGN(segsize);
				memcpy(walbufend, seginfo->seg, segsize);
				break;

			default:
				elog(ERROR, "unexpected GIN leaf action %d", action);
		}
		walbufend += datalen;

		if (action != GIN_SEGMENT_INSERT)
			segno++;
	}

	/* 经 *leaf 回传所构造的信息 */
	leaf->walinfo = walbufbegin;
	leaf->walinfolen = walbufend - walbufbegin;
}

/*
 * 把已拆解的倒排树叶子页重新组装回缓冲区。
 *
 * 这只更新目标缓冲区；WAL 事宜由调用者负责。
 *
 * 注意：段指针不得直接指向同一缓冲区，除非该段
 * 未被修改，且其前面的段也未被
 * 修改。
 */
static void
dataPlaceToPageLeafRecompress(Buffer buf, disassembledLeaf *leaf)
{
	Page		page = BufferGetPage(buf);
	char	   *ptr;
	int			newsize;
	bool		modified = false;
	dlist_iter	iter;
	int			segsize;

	/*
	 * 若页原先是 9.4 之前的格式，则转换页头，并强制
	 * 无论是否修改都把所有段复制到
	 * 页上。
	 */
	if (!GinPageIsCompressed(page))
	{
		Assert(leaf->oldformat);
		GinPageSetCompressed(page);
		GinPageGetOpaque(page)->maxoff = InvalidOffsetNumber;
		modified = true;
	}

	ptr = (char *) GinDataLeafPageGetPostingList(page);
	newsize = 0;
	dlist_foreach(iter, &leaf->segments)
	{
		leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node, iter.cur);

		if (seginfo->action != GIN_SEGMENT_UNMODIFIED)
			modified = true;

		if (seginfo->action != GIN_SEGMENT_DELETE)
		{
			segsize = SizeOfGinPostingList(seginfo->seg);

			if (modified)
				memcpy(ptr, seginfo->seg, segsize);

			ptr += segsize;
			newsize += segsize;
		}
	}

	Assert(newsize <= GinDataPageMaxDataSize);
	GinDataPageSetDataSize(page, newsize);
}

/*
 * 类似 dataPlaceToPageLeafRecompress，但把已拆解的叶子
 * 段写入两页而不是一页。
 *
 * 与非分裂情形的不同之处在于：这不直接修改原页，
 * 而是写入新左页与右页的临时
 * 内存副本。
 */
static void
dataPlaceToPageLeafSplit(disassembledLeaf *leaf,
						 ItemPointerData lbound, ItemPointerData rbound,
						 Page lpage, Page rpage)
{
	char	   *ptr;
	int			segsize;
	int			lsize;
	int			rsize;
	dlist_node *node;
	dlist_node *firstright;
	leafSegmentInfo *seginfo;

	/* 初始化临时页以容纳新的左页与右页 */
	GinInitPage(lpage, GIN_DATA | GIN_LEAF | GIN_COMPRESSED, BLCKSZ);
	GinInitPage(rpage, GIN_DATA | GIN_LEAF | GIN_COMPRESSED, BLCKSZ);

	/*
	 * 复制应落到左页的各段。
	 *
	 * XXX：我们应跳过复制左页未修改的部分，就像
	 * 在重新压缩时所做的那样。
	 */
	lsize = 0;
	ptr = (char *) GinDataLeafPageGetPostingList(lpage);
	firstright = dlist_next_node(&leaf->segments, leaf->lastleft);
	for (node = dlist_head_node(&leaf->segments);
		 node != firstright;
		 node = dlist_next_node(&leaf->segments, node))
	{
		seginfo = dlist_container(leafSegmentInfo, node, node);

		if (seginfo->action != GIN_SEGMENT_DELETE)
		{
			segsize = SizeOfGinPostingList(seginfo->seg);
			memcpy(ptr, seginfo->seg, segsize);
			ptr += segsize;
			lsize += segsize;
		}
	}
	Assert(lsize == leaf->lsize);
	GinDataPageSetDataSize(lpage, lsize);
	*GinDataPageGetRightBound(lpage) = lbound;

	/* 复制应落到右页的各段 */
	ptr = (char *) GinDataLeafPageGetPostingList(rpage);
	rsize = 0;
	for (node = firstright;
		 ;
		 node = dlist_next_node(&leaf->segments, node))
	{
		seginfo = dlist_container(leafSegmentInfo, node, node);

		if (seginfo->action != GIN_SEGMENT_DELETE)
		{
			segsize = SizeOfGinPostingList(seginfo->seg);
			memcpy(ptr, seginfo->seg, segsize);
			ptr += segsize;
			rsize += segsize;
		}

		if (!dlist_has_next(&leaf->segments, node))
			break;
	}
	Assert(rsize == leaf->rsize);
	GinDataPageSetDataSize(rpage, rsize);
	*GinDataPageGetRightBound(rpage) = rbound;
}

/*
 * 准备向内部数据页插入数据。
 *
 * 若放得下，则在进入插入临界区前做完所需准备后返回
 * GPTP_INSERT。可将 *ptp_workspace 设为向
 * execPlaceToPage 函数传递信息。
 *
 * 若放不下，则执行页分裂，并把两个临时页映像返回到
 * *newlpage 与 *newrpage，结果为 GPTP_SPLIT。
 *
 * 无论哪种情况，此处都不应修改给定的页缓冲区。
 *
 * 注意：向内部结点插入时，除插入给定项外，
 * stack->off 处现有项的下行指针也会被更新为
 * 指向 updateblkno。
 */
static GinPlaceToPageRC
dataBeginPlaceToPageInternal(GinBtree btree, Buffer buf, GinBtreeStack *stack,
							 void *insertdata, BlockNumber updateblkno,
							 void **ptp_workspace,
							 Page *newlpage, Page *newrpage)
{
	Page		page = BufferGetPage(buf);

	/* 若放不下，则处理分裂情形 */
	if (GinNonLeafDataPageGetFreeSpace(page) < sizeof(PostingItem))
	{
		dataSplitPageInternal(btree, buf, stack, insertdata, updateblkno,
							  newlpage, newrpage);
		return GPTP_SPLIT;
	}

	/* 否则，我们已准备好继续插入 */
	return GPTP_INSERT;
}

/*
 * 在 beginPlaceToPage 已判定放得下之后执行数据插入。
 *
 * 在临界区内调用，且（若需要）XLOG 记录创建已
 * 开始。目标缓冲区注册在槽 0。
 */
static void
dataExecPlaceToPageInternal(GinBtree btree, Buffer buf, GinBtreeStack *stack,
							void *insertdata, BlockNumber updateblkno,
							void *ptp_workspace)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber off = stack->off;
	PostingItem *pitem;

	/* 更新现有下行指针以指向下一页（在内部页上） */
	pitem = GinDataPageGetPostingItem(page, off);
	PostingItemSetBlockNumber(pitem, updateblkno);

	/* 添加新项 */
	pitem = (PostingItem *) insertdata;
	GinDataPageAddPostingItem(page, pitem, off);

	if (RelationNeedsWAL(btree->index) && !btree->isBuild)
	{
		/*
		 * 这必须是 static，因为它必须存活到 XLogInsert，
		 * 而此处不能 palloc。难看，但 XLogInsert 基础设施
		 * 反正也不是可重入的。
		 */
		static ginxlogInsertDataInternal data;

		data.offset = off;
		data.newitem = *pitem;

		XLogRegisterBufData(0, (char *) &data,
							sizeof(ginxlogInsertDataInternal));
	}
}

/*
 * 准备向倒排树数据页插入数据。
 *
 * 若放得下，则在进入插入临界区前做完所需准备后返回
 * GPTP_INSERT。可将 *ptp_workspace 设为向
 * execPlaceToPage 函数传递信息。
 *
 * 若放不下，则执行页分裂，并把两个临时页映像返回到
 * *newlpage 与 *newrpage，结果为 GPTP_SPLIT。
 *
 * 无论哪种情况，此处都不应修改给定的页缓冲区。
 *
 * 注意：向内部结点插入时，除插入给定项外，
 * stack->off 处现有项的下行指针也会被更新为
 * 指向 updateblkno。
 *
 * 对内部页或叶子页调用相应函数，因为它们的处理方式
 * 很不相同。
 */
static GinPlaceToPageRC
dataBeginPlaceToPage(GinBtree btree, Buffer buf, GinBtreeStack *stack,
					 void *insertdata, BlockNumber updateblkno,
					 void **ptp_workspace,
					 Page *newlpage, Page *newrpage)
{
	Page		page = BufferGetPage(buf);

	Assert(GinPageIsData(page));

	if (GinPageIsLeaf(page))
		return dataBeginPlaceToPageLeaf(btree, buf, stack, insertdata,
										ptp_workspace,
										newlpage, newrpage);
	else
		return dataBeginPlaceToPageInternal(btree, buf, stack,
											insertdata, updateblkno,
											ptp_workspace,
											newlpage, newrpage);
}

/*
 * 在 beginPlaceToPage 已判定放得下之后执行数据插入。
 *
 * 在临界区内调用，且（若需要）XLOG 记录创建已
 * 开始。目标缓冲区注册在槽 0。
 *
 * 对内部页或叶子页调用相应函数，因为它们的处理方式
 * 很不相同。
 */
static void
dataExecPlaceToPage(GinBtree btree, Buffer buf, GinBtreeStack *stack,
					void *insertdata, BlockNumber updateblkno,
					void *ptp_workspace)
{
	Page		page = BufferGetPage(buf);

	if (GinPageIsLeaf(page))
		dataExecPlaceToPageLeaf(btree, buf, stack, insertdata,
								ptp_workspace);
	else
		dataExecPlaceToPageInternal(btree, buf, stack, insertdata,
									updateblkno, ptp_workspace);
}

/*
 * 分裂内部页并插入新数据。
 *
 * 把新的临时页返回到 *newlpage 与 *newrpage。
 * 原缓冲区保持不动。
 */
static void
dataSplitPageInternal(GinBtree btree, Buffer origbuf,
					  GinBtreeStack *stack,
					  void *insertdata, BlockNumber updateblkno,
					  Page *newlpage, Page *newrpage)
{
	Page		oldpage = BufferGetPage(origbuf);
	OffsetNumber off = stack->off;
	int			nitems = GinPageGetOpaque(oldpage)->maxoff;
	int			nleftitems;
	int			nrightitems;
	Size		pageSize = PageGetPageSize(oldpage);
	ItemPointerData oldbound = *GinDataPageGetRightBound(oldpage);
	ItemPointer bound;
	Page		lpage;
	Page		rpage;
	OffsetNumber separator;
	PostingItem allitems[(BLCKSZ / sizeof(PostingItem)) + 1];

	lpage = PageGetTempPage(oldpage);
	rpage = PageGetTempPage(oldpage);
	GinInitPage(lpage, GinPageGetOpaque(oldpage)->flags, pageSize);
	GinInitPage(rpage, GinPageGetOpaque(oldpage)->flags, pageSize);

	/*
	 * 首先构造一份新的 PostingItems 列表，其中包含全部旧项
	 * 以及新项。
	 */
	memcpy(allitems, GinDataPageGetPostingItem(oldpage, FirstOffsetNumber),
		   (off - 1) * sizeof(PostingItem));

	allitems[off - 1] = *((PostingItem *) insertdata);
	memcpy(&allitems[off], GinDataPageGetPostingItem(oldpage, off),
		   (nitems - (off - 1)) * sizeof(PostingItem));
	nitems++;

	/* 更新现有下行指针以指向下一页 */
	PostingItemSetBlockNumber(&allitems[off], updateblkno);

	/*
	 * 在创建新索引时，在左页上尽量多放元组，假定表
	 * 从始至终扫描。这样能把索引打包得尽可能
	 * 紧。
	 */
	if (btree->isBuild && GinPageRightMost(oldpage))
		separator = GinNonLeafDataPageGetFreeSpace(rpage) / sizeof(PostingItem);
	else
		separator = nitems / 2;
	nleftitems = separator;
	nrightitems = nitems - separator;

	memcpy(GinDataPageGetPostingItem(lpage, FirstOffsetNumber),
		   allitems,
		   nleftitems * sizeof(PostingItem));
	GinPageGetOpaque(lpage)->maxoff = nleftitems;
	memcpy(GinDataPageGetPostingItem(rpage, FirstOffsetNumber),
		   &allitems[separator],
		   nrightitems * sizeof(PostingItem));
	GinPageGetOpaque(rpage)->maxoff = nrightitems;

	/*
	 * 也为两页设置 pd_lower，如同 GinDataPageAddPostingItem 所做。
	 */
	GinDataPageSetDataSize(lpage, nleftitems * sizeof(PostingItem));
	GinDataPageSetDataSize(rpage, nrightitems * sizeof(PostingItem));

	/* 设置左页的右界 */
	bound = GinDataPageGetRightBound(lpage);
	*bound = GinDataPageGetPostingItem(lpage, nleftitems)->key;

	/* 设置右页的右界 */
	*GinDataPageGetRightBound(rpage) = oldbound;

	/* 把临时页返回给调用者 */
	*newlpage = lpage;
	*newrpage = rpage;
}

/*
 * 构造插入有效载荷，用于插入给定缓冲区的下行指针。
 */
static void *
dataPrepareDownlink(GinBtree btree, Buffer lbuf)
{
	PostingItem *pitem = palloc(sizeof(PostingItem));
	Page		lpage = BufferGetPage(lbuf);

	PostingItemSetBlockNumber(pitem, BufferGetBlockNumber(lbuf));
	pitem->key = *GinDataPageGetRightBound(lpage);

	return pitem;
}

/*
 * 用子结点的右界值填充新根。
 * 也从 ginxlog 调用，不应使用 btree
 */
void
ginDataFillRoot(GinBtree btree, Page root, BlockNumber lblkno, Page lpage, BlockNumber rblkno, Page rpage)
{
	PostingItem li,
				ri;

	li.key = *GinDataPageGetRightBound(lpage);
	PostingItemSetBlockNumber(&li, lblkno);
	GinDataPageAddPostingItem(root, &li, InvalidOffsetNumber);

	ri.key = *GinDataPageGetRightBound(rpage);
	PostingItemSetBlockNumber(&ri, rblkno);
	GinDataPageAddPostingItem(root, &ri, InvalidOffsetNumber);
}


/*** 处理已拆解叶子页的函数 ***/

/*
 * 把页拆解为 disassembledLeaf 结构。
 */
static disassembledLeaf *
disassembleLeaf(Page page)
{
	disassembledLeaf *leaf;
	GinPostingList *seg;
	Pointer		segbegin;
	Pointer		segend;

	leaf = palloc0(sizeof(disassembledLeaf));
	dlist_init(&leaf->segments);

	if (GinPageIsCompressed(page))
	{
		/*
		 * 为每个段创建一个 leafSegmentInfo 条目。
		 */
		seg = GinDataLeafPageGetPostingList(page);
		segbegin = (Pointer) seg;
		segend = segbegin + GinDataLeafPageGetPostingListSize(page);
		while ((Pointer) seg < segend)
		{
			leafSegmentInfo *seginfo = palloc(sizeof(leafSegmentInfo));

			seginfo->action = GIN_SEGMENT_UNMODIFIED;
			seginfo->seg = seg;
			seginfo->items = NULL;
			seginfo->nitems = 0;
			dlist_push_tail(&leaf->segments, &seginfo->node);

			seg = GinNextPostingListSegment(seg);
		}
		leaf->oldformat = false;
	}
	else
	{
		/*
		 * 9.4 之前格式的未压缩页用单个段表示，带有一个
		 * 项数组。边界情形是不含任何项的未压缩页，表示为
		 * 没有段。
		 */
		ItemPointer uncompressed;
		int			nuncompressed;
		leafSegmentInfo *seginfo;

		uncompressed = dataLeafPageGetUncompressed(page, &nuncompressed);

		if (nuncompressed > 0)
		{
			seginfo = palloc(sizeof(leafSegmentInfo));

			seginfo->action = GIN_SEGMENT_REPLACE;
			seginfo->seg = NULL;
			seginfo->items = palloc(nuncompressed * sizeof(ItemPointerData));
			memcpy(seginfo->items, uncompressed, nuncompressed * sizeof(ItemPointerData));
			seginfo->nitems = nuncompressed;

			dlist_push_tail(&leaf->segments, &seginfo->node);
		}

		leaf->oldformat = true;
	}

	return leaf;
}

/*
 * 把 newItems 分配到各段。
 *
 * 获得新项的任何段都会被解码，新项与旧项
 * 合并。
 *
 * 若添加了任何新项则返回 true。false 表示它们都是
 * 页上现有项的重复。
 */
static bool
addItemsToLeaf(disassembledLeaf *leaf, ItemPointer newItems, int nNewItems)
{
	dlist_iter	iter;
	ItemPointer nextnew = newItems;
	int			newleft = nNewItems;
	bool		modified = false;
	leafSegmentInfo *newseg;

	/*
	 * 若页完全为空，则只构造一个新段来容纳
	 * 全部新项。
	 */
	if (dlist_is_empty(&leaf->segments))
	{
		newseg = palloc(sizeof(leafSegmentInfo));
		newseg->seg = NULL;
		newseg->items = newItems;
		newseg->nitems = nNewItems;
		newseg->action = GIN_SEGMENT_INSERT;
		dlist_push_tail(&leaf->segments, &newseg->node);
		return true;
	}

	dlist_foreach(iter, &leaf->segments)
	{
		leafSegmentInfo *cur = (leafSegmentInfo *) dlist_container(leafSegmentInfo, node, iter.cur);
		int			nthis;
		ItemPointer tmpitems;
		int			ntmpitems;

		/*
		 * 有多少新项落入本段？
		 */
		if (!dlist_has_next(&leaf->segments, iter.cur))
			nthis = newleft;
		else
		{
			leafSegmentInfo *next;
			ItemPointerData next_first;

			next = (leafSegmentInfo *) dlist_container(leafSegmentInfo, node,
													   dlist_next_node(&leaf->segments, iter.cur));
			if (next->items)
				next_first = next->items[0];
			else
			{
				Assert(next->seg != NULL);
				next_first = next->seg->first;
			}

			nthis = 0;
			while (nthis < newleft && ginCompareItemPointers(&nextnew[nthis], &next_first) < 0)
				nthis++;
		}
		if (nthis == 0)
			continue;

		/* 把新项与现有项合并。 */
		if (!cur->items)
			cur->items = ginPostingListDecode(cur->seg, &cur->nitems);

		/*
		 * 向页末追加这一重要特殊情形的快速路径：不要让
		 * 页上最后一段长过目标尺寸，在那之前创建
		 * 新段。
		 */
		if (!dlist_has_next(&leaf->segments, iter.cur) &&
			ginCompareItemPointers(&cur->items[cur->nitems - 1], &nextnew[0]) < 0 &&
			cur->seg != NULL &&
			SizeOfGinPostingList(cur->seg) >= GinPostingListSegmentTargetSize)
		{
			newseg = palloc(sizeof(leafSegmentInfo));
			newseg->seg = NULL;
			newseg->items = nextnew;
			newseg->nitems = nthis;
			newseg->action = GIN_SEGMENT_INSERT;
			dlist_push_tail(&leaf->segments, &newseg->node);
			modified = true;
			break;
		}

		tmpitems = ginMergeItemPointers(cur->items, cur->nitems,
										nextnew, nthis,
										&ntmpitems);
		if (ntmpitems != cur->nitems)
		{
			/*
			 * 若没有重复，则跟踪所添加的项，以便稍后发出
			 * 紧凑的 ADDITEMS WAL 记录。（若有重复，似乎
			 * 不值得再检查哪些项是重复
			 * 的）
			 */
			if (ntmpitems == nthis + cur->nitems &&
				cur->action == GIN_SEGMENT_UNMODIFIED)
			{
				cur->action = GIN_SEGMENT_ADDITEMS;
				cur->modifieditems = nextnew;
				cur->nmodifieditems = nthis;
			}
			else
				cur->action = GIN_SEGMENT_REPLACE;

			cur->items = tmpitems;
			cur->nitems = ntmpitems;
			cur->seg = NULL;
			modified = true;
		}

		nextnew += nthis;
		newleft -= nthis;
		if (newleft == 0)
			break;
	}

	return modified;
}

/*
 * 重新压缩所有已修改的段。
 *
 * 若并非全部项都能放进两页（即分裂后），则存放能放下的
 * 尽可能多项，并把 *remaining 设为放不下的第一项。
 * 若全部放得下，则把 *remaining 设为无效。
 *
 * 若页必须分裂则返回 true。
 */
static bool
leafRepackItems(disassembledLeaf *leaf, ItemPointer remaining)
{
	int			pgused = 0;
	bool		needsplit = false;
	dlist_iter	iter;
	int			segsize;
	leafSegmentInfo *nextseg;
	int			npacked;
	bool		modified;
	dlist_node *cur_node;
	dlist_node *next_node;

	ItemPointerSetInvalid(remaining);

	/*
	 * 此处无法用 dlist_foreach_modify，因为我们在迭代时
	 * 会插入相邻项。
	 */
	for (cur_node = dlist_head_node(&leaf->segments);
		 cur_node != NULL;
		 cur_node = next_node)
	{
		leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node,
												   cur_node);

		if (dlist_has_next(&leaf->segments, cur_node))
			next_node = dlist_next_node(&leaf->segments, cur_node);
		else
			next_node = NULL;

		/* 若有必要则压缩倒排列表 */
		if (seginfo->action != GIN_SEGMENT_DELETE)
		{
			if (seginfo->seg == NULL)
			{
				if (seginfo->nitems > GinPostingListSegmentMaxSize)
					npacked = 0;	/* 不可能放得下。 */
				else
				{
					seginfo->seg = ginCompressPostingList(seginfo->items,
														  seginfo->nitems,
														  GinPostingListSegmentMaxSize,
														  &npacked);
				}
				if (npacked != seginfo->nitems)
				{
					/*
					 * 太大了。再次压缩到目标尺寸，并创建
					 * 一个新段表示剩余项。新段插在本段之后，
					 * 因此会在本循环的下一次迭代中
					 * 处理。
					 */
					if (seginfo->seg)
						pfree(seginfo->seg);
					seginfo->seg = ginCompressPostingList(seginfo->items,
														  seginfo->nitems,
														  GinPostingListSegmentTargetSize,
														  &npacked);
					if (seginfo->action != GIN_SEGMENT_INSERT)
						seginfo->action = GIN_SEGMENT_REPLACE;

					nextseg = palloc(sizeof(leafSegmentInfo));
					nextseg->action = GIN_SEGMENT_INSERT;
					nextseg->seg = NULL;
					nextseg->items = &seginfo->items[npacked];
					nextseg->nitems = seginfo->nitems - npacked;
					next_node = &nextseg->node;
					dlist_insert_after(cur_node, next_node);
				}
			}

			/*
			 * 若段非常小，则把它与下一段合并。
			 */
			if (SizeOfGinPostingList(seginfo->seg) < GinPostingListSegmentMinSize && next_node)
			{
				int			nmerged;

				nextseg = dlist_container(leafSegmentInfo, node, next_node);

				if (seginfo->items == NULL)
					seginfo->items = ginPostingListDecode(seginfo->seg,
														  &seginfo->nitems);
				if (nextseg->items == NULL)
					nextseg->items = ginPostingListDecode(nextseg->seg,
														  &nextseg->nitems);
				nextseg->items =
					ginMergeItemPointers(seginfo->items, seginfo->nitems,
										 nextseg->items, nextseg->nitems,
										 &nmerged);
				Assert(nmerged == seginfo->nitems + nextseg->nitems);
				nextseg->nitems = nmerged;
				nextseg->seg = NULL;

				nextseg->action = GIN_SEGMENT_REPLACE;
				nextseg->modifieditems = NULL;
				nextseg->nmodifieditems = 0;

				if (seginfo->action == GIN_SEGMENT_INSERT)
				{
					dlist_delete(cur_node);
					continue;
				}
				else
				{
					seginfo->action = GIN_SEGMENT_DELETE;
					seginfo->seg = NULL;
				}
			}

			seginfo->items = NULL;
			seginfo->nitems = 0;
		}

		if (seginfo->action == GIN_SEGMENT_DELETE)
			continue;

		/*
		 * 好了，我们现在有了本段的压缩版本，准备复制到页上。
		 * 是否已超过单页能放下的尺寸？
		 */
		segsize = SizeOfGinPostingList(seginfo->seg);
		if (pgused + segsize > GinDataPageMaxDataSize)
		{
			if (!needsplit)
			{
				/* 切换到右页 */
				Assert(pgused > 0);
				leaf->lastleft = dlist_prev_node(&leaf->segments, cur_node);
				needsplit = true;
				leaf->lsize = pgused;
				pgused = 0;
			}
			else
			{
				/*
				 * 两页都已填满。我们构造的最后一段放
				 * 不下。
				 */
				*remaining = seginfo->seg->first;

				/*
				 * 从列表中移除所有放不下的段。
				 */
				while (dlist_has_next(&leaf->segments, cur_node))
					dlist_delete(dlist_next_node(&leaf->segments, cur_node));
				dlist_delete(cur_node);
				break;
			}
		}

		pgused += segsize;
	}

	if (!needsplit)
	{
		leaf->lsize = pgused;
		leaf->rsize = 0;
	}
	else
		leaf->rsize = pgused;

	Assert(leaf->lsize <= GinDataPageMaxDataSize);
	Assert(leaf->rsize <= GinDataPageMaxDataSize);

	/*
	 * 为第一个被修改段之后的每个段做一份 palloc 副本，
	 * 因为当我们开始把项复制到原页时，可能会覆盖现有
	 * 段。
	 */
	modified = false;
	dlist_foreach(iter, &leaf->segments)
	{
		leafSegmentInfo *seginfo = dlist_container(leafSegmentInfo, node,
												   iter.cur);

		if (!modified && seginfo->action != GIN_SEGMENT_UNMODIFIED)
		{
			modified = true;
		}
		else if (modified && seginfo->action == GIN_SEGMENT_UNMODIFIED)
		{
			GinPostingList *tmp;

			segsize = SizeOfGinPostingList(seginfo->seg);
			tmp = palloc(segsize);
			memcpy(tmp, seginfo->seg, segsize);
			seginfo->seg = tmp;
		}
	}

	return needsplit;
}


/*** 导出给 GIN 其余代码的函数 ***/

/*
 * 创建包含给定 TIDs 的新倒排树。返回新倒排树根的
 * 页号。
 *
 * items[] 必须已排序且无重复。
 */
BlockNumber
createPostingTree(Relation index, ItemPointerData *items, uint32 nitems,
				  GinStatsData *buildStats, Buffer entrybuffer)
{
	BlockNumber blkno;
	Buffer		buffer;
	Page		tmppage;
	Page		page;
	Pointer		ptr;
	int			nrootitems;
	int			rootsize;
	bool		is_build = (buildStats != NULL);

	/* 先在内存中构造新的根页。 */
	tmppage = (Page) palloc(BLCKSZ);
	GinInitPage(tmppage, GIN_DATA | GIN_LEAF | GIN_COMPRESSED, BLCKSZ);
	GinPageGetOpaque(tmppage)->rightlink = InvalidBlockNumber;

	/*
	 * 把能放下的项尽量多地写入根页。按每段最大
	 * GinPostingListSegmentMaxSize 字节分段。
	 */
	nrootitems = 0;
	rootsize = 0;
	ptr = (Pointer) GinDataLeafPageGetPostingList(tmppage);
	while (nrootitems < nitems)
	{
		GinPostingList *segment;
		int			npacked;
		int			segsize;

		segment = ginCompressPostingList(&items[nrootitems],
										 nitems - nrootitems,
										 GinPostingListSegmentMaxSize,
										 &npacked);
		segsize = SizeOfGinPostingList(segment);
		if (rootsize + segsize > GinDataPageMaxDataSize)
			break;

		memcpy(ptr, segment, segsize);
		ptr += segsize;
		rootsize += segsize;
		nrootitems += npacked;
		pfree(segment);
	}
	GinDataPageSetDataSize(tmppage, rootsize);

	/*
	 * 一切就绪。获取新的物理页，并把内存中的页复制过去。
	 */
	buffer = GinNewBuffer(index);
	page = BufferGetPage(buffer);
	blkno = BufferGetBlockNumber(buffer);

	/*
	 * 把条目树叶子（含倒排列表）上的任何谓词锁
	 * 复制到倒排树。
	 */
	PredicateLockPageSplit(index, BufferGetBlockNumber(entrybuffer), blkno);

	START_CRIT_SECTION();

	PageRestoreTempPage(tmppage, page);
	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(index) && !is_build)
	{
		XLogRecPtr	recptr;
		ginxlogCreatePostingTree data;

		data.size = rootsize;

		XLogBeginInsert();
		XLogRegisterData((char *) &data, sizeof(ginxlogCreatePostingTree));

		XLogRegisterData((char *) GinDataLeafPageGetPostingList(page),
						 rootsize);
		XLogRegisterBuffer(0, buffer, REGBUF_WILL_INIT);

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_CREATE_PTREE);
		PageSetLSN(page, recptr);
	}

	UnlockReleaseBuffer(buffer);

	END_CRIT_SECTION();

	/* 在索引构建期间，统计新添加的数据页 */
	if (buildStats)
		buildStats->nDataPages++;

	elog(DEBUG2, "created GIN posting tree with %d items", nrootitems);

	/*
	 * 把任何剩余 TIDs 加入新创建的倒排树。
	 */
	if (nitems > nrootitems)
	{
		ginInsertItemPointers(index, blkno,
							  items + nrootitems,
							  nitems - nrootitems,
							  buildStats);
	}

	return blkno;
}

static void
ginPrepareDataScan(GinBtree btree, Relation index, BlockNumber rootBlkno)
{
	memset(btree, 0, sizeof(GinBtreeData));

	btree->index = index;
	btree->rootBlkno = rootBlkno;

	btree->findChildPage = dataLocateItem;
	btree->getLeftMostChild = dataGetLeftMostPage;
	btree->isMoveRight = dataIsMoveRight;
	btree->findItem = NULL;
	btree->findChildPtr = dataFindChildPtr;
	btree->beginPlaceToPage = dataBeginPlaceToPage;
	btree->execPlaceToPage = dataExecPlaceToPage;
	btree->fillRoot = ginDataFillRoot;
	btree->prepareDownlink = dataPrepareDownlink;

	btree->isData = true;
	btree->fullScan = false;
	btree->isBuild = false;
}

/*
 * 插入项指针数组，可能执行多次树扫描（很少见）
 */
void
ginInsertItemPointers(Relation index, BlockNumber rootBlkno,
					  ItemPointerData *items, uint32 nitem,
					  GinStatsData *buildStats)
{
	GinBtreeData btree;
	GinBtreeDataLeafInsertData insertdata;
	GinBtreeStack *stack;

	ginPrepareDataScan(&btree, index, rootBlkno);
	btree.isBuild = (buildStats != NULL);
	insertdata.items = items;
	insertdata.nitem = nitem;
	insertdata.curitem = 0;

	while (insertdata.curitem < insertdata.nitem)
	{
		/* 搜索第一项应落入的叶子页 */
		btree.itemptr = insertdata.items[insertdata.curitem];
		stack = ginFindLeafPage(&btree, false, true, NULL);

		ginInsertValue(&btree, stack, &insertdata, buildStats);
	}
}

/*
 * 在倒排树上开始一次新扫描。
 */
GinBtreeStack *
ginScanBeginPostingTree(GinBtree btree, Relation index, BlockNumber rootBlkno,
						Snapshot snapshot)
{
	GinBtreeStack *stack;

	ginPrepareDataScan(btree, index, rootBlkno);

	btree->fullScan = true;

	stack = ginFindLeafPage(btree, true, false, snapshot);

	return stack;
}
