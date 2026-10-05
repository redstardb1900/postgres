/*-------------------------------------------------------------------------
 *
 * ginvacuum.c
 *	  Postgres GIN 的删除与清理例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginvacuum.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/ginxlog.h"
#include "access/xloginsert.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/memutils.h"

struct GinVacuumState
{
	Relation	index;
	IndexBulkDeleteResult *result;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	GinState	ginstate;
	BufferAccessStrategy strategy;
	MemoryContext tmpCxt;
};

/*
 * 清理一份未压缩的倒排列表。列表大小可以用项数
 * （nitems）指定。
 *
 * 若没有项需要移除，则返回 NULL。否则返回一个新
 * palloc 出来的数组，其中是剩余的项。剩余项的个数
 * 由 *nremaining 返回。
 */
ItemPointer
ginVacuumItemPointers(GinVacuumState *gvs, ItemPointerData *items,
					  int nitem, int *nremaining)
{
	int			i,
				remaining = 0;
	ItemPointer tmpitems = NULL;

	/*
	 * 遍历 TIDs 数组
	 */
	for (i = 0; i < nitem; i++)
	{
		if (gvs->callback(items + i, gvs->callback_state))
		{
			gvs->result->tuples_removed += 1;
			if (!tmpitems)
			{
				/*
				 * 第一个要删除的 TID：分配内存以保存
				 * 剩余的项。
				 */
				tmpitems = palloc(sizeof(ItemPointerData) * nitem);
				memcpy(tmpitems, items, sizeof(ItemPointerData) * i);
			}
		}
		else
		{
			gvs->result->num_index_tuples += 1;
			if (tmpitems)
				tmpitems[remaining] = items[i];
			remaining++;
		}
	}

	*nremaining = remaining;
	return tmpitems;
}

/*
 * 为清理条目树叶子页创建一条 WAL 记录。
 */
static void
xlogVacuumPage(Relation index, Buffer buffer)
{
	Page		page = BufferGetPage(buffer);
	XLogRecPtr	recptr;

	/* 这只用于条目树叶子页。 */
	Assert(!GinPageIsData(page));
	Assert(GinPageIsLeaf(page));

	if (!RelationNeedsWAL(index))
		return;

	/*
	 * 总是创建整页映像，我们并不以更细的粒度跟踪
	 * 页上的变更。这一点显然可以改进...
	 */
	XLogBeginInsert();
	XLogRegisterBuffer(0, buffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);

	recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_VACUUM_PAGE);
	PageSetLSN(page, recptr);
}


typedef struct DataPageDeleteStack
{
	struct DataPageDeleteStack *child;
	struct DataPageDeleteStack *parent;

	BlockNumber blkno;			/* 当前块号 */
	Buffer		leftBuffer;		/* 已钉住并锁定的、左侧最右的
								 * 未删除页 */
	bool		isRoot;
} DataPageDeleteStack;


/*
 * 删除一张倒排树页。
 */
static void
ginDeletePage(GinVacuumState *gvs, BlockNumber deleteBlkno, BlockNumber leftBlkno,
			  BlockNumber parentBlkno, OffsetNumber myoff, bool isParentRoot)
{
	Buffer		dBuffer;
	Buffer		lBuffer;
	Buffer		pBuffer;
	Page		page,
				parentPage;
	BlockNumber rightlink;

	/*
	 * 仅当某一父页持有排他清理锁时才能调用本函数。
	 * 这保证当前没有插入发生在此子树中。调用者还要
	 * 在待删除页、父页和左页上取得
	 * 排他锁。
	 */
	lBuffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, leftBlkno,
								 RBM_NORMAL, gvs->strategy);
	dBuffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, deleteBlkno,
								 RBM_NORMAL, gvs->strategy);
	pBuffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, parentBlkno,
								 RBM_NORMAL, gvs->strategy);

	page = BufferGetPage(dBuffer);
	rightlink = GinPageGetOpaque(page)->rightlink;

	/*
	 * 原本会进入该叶子块的任何插入，现在都会进入它的
	 * 右兄弟。
	 */
	PredicateLockPageCombine(gvs->index, deleteBlkno, rightlink);

	START_CRIT_SECTION();

	/* 通过修改左兄弟的右链接来解除该页的链接 */
	page = BufferGetPage(lBuffer);
	GinPageGetOpaque(page)->rightlink = rightlink;

	/* 从父页删除下行指针 */
	parentPage = BufferGetPage(pBuffer);
#ifdef USE_ASSERT_CHECKING
	do
	{
		PostingItem *tod = GinDataPageGetPostingItem(parentPage, myoff);

		Assert(PostingItemGetBlockNumber(tod) == deleteBlkno);
	} while (0);
#endif
	GinPageDeletePostingItem(parentPage, myoff);

	page = BufferGetPage(dBuffer);

	/*
	 * 不应修改 rightlink 字段，以便正在进行的
	 * 搜索扫描仍能工作
	 */

	/*
	 * 把页标为已删除，并记住最后一个可能知道其
	 * 地址的 xid。
	 */
	GinPageSetDeleted(page);
	GinPageSetDeleteXid(page, ReadNextTransactionId());

	MarkBufferDirty(pBuffer);
	MarkBufferDirty(lBuffer);
	MarkBufferDirty(dBuffer);

	if (RelationNeedsWAL(gvs->index))
	{
		XLogRecPtr	recptr;
		ginxlogDeletePage data;

		/*
		 * 不能为已删除页传入 REGBUF_STANDARD，因为
		 * 9.4 之前的版本没有设置 pd_lower。该页可能
		 * 由更旧版本经二进制升级而来，因而 pd_lower
		 * 设置不正确。左页同样如此，但从父页移除该项
		 * 已经更新了它的 pd_lower，所以此刻我们知道
		 * 这样是可以的。
		 */
		XLogBeginInsert();
		XLogRegisterBuffer(0, dBuffer, 0);
		XLogRegisterBuffer(1, pBuffer, REGBUF_STANDARD);
		XLogRegisterBuffer(2, lBuffer, 0);

		data.parentOffset = myoff;
		data.rightLink = GinPageGetOpaque(page)->rightlink;
		data.deleteXid = GinPageGetDeleteXid(page);

		XLogRegisterData((char *) &data, sizeof(ginxlogDeletePage));

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_DELETE_PAGE);
		PageSetLSN(page, recptr);
		PageSetLSN(parentPage, recptr);
		PageSetLSN(BufferGetPage(lBuffer), recptr);
	}

	ReleaseBuffer(pBuffer);
	ReleaseBuffer(lBuffer);
	ReleaseBuffer(dBuffer);

	END_CRIT_SECTION();

	gvs->result->pages_newly_deleted++;
	gvs->result->pages_deleted++;
}


/*
 * 扫描倒排树并删除空页。调用者必须对根页取清理锁。
 * 扫描期间，从根到当前页的路径保持排他锁定。
 * 左页也保持排他锁定，因为 ginDeletePage()
 * 需要它。若稍后试图重新锁定左页，可能与
 * ginStepRight() 死锁。
 */
static bool
ginScanToDelete(GinVacuumState *gvs, BlockNumber blkno, bool isRoot,
				DataPageDeleteStack *parent, OffsetNumber myoff)
{
	DataPageDeleteStack *me;
	Buffer		buffer;
	Page		page;
	bool		meDelete = false;
	bool		isempty;

	if (isRoot)
	{
		me = parent;
	}
	else
	{
		if (!parent->child)
		{
			me = (DataPageDeleteStack *) palloc0(sizeof(DataPageDeleteStack));
			me->parent = parent;
			parent->child = me;
			me->leftBuffer = InvalidBuffer;
		}
		else
			me = parent->child;
	}

	buffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, blkno,
								RBM_NORMAL, gvs->strategy);

	if (!isRoot)
		LockBuffer(buffer, GIN_EXCLUSIVE);

	page = BufferGetPage(buffer);

	Assert(GinPageIsData(page));

	if (!GinPageIsLeaf(page))
	{
		OffsetNumber i;

		me->blkno = blkno;
		for (i = FirstOffsetNumber; i <= GinPageGetOpaque(page)->maxoff; i++)
		{
			PostingItem *pitem = GinDataPageGetPostingItem(page, i);

			if (ginScanToDelete(gvs, PostingItemGetBlockNumber(pitem), false, me, i))
				i--;
		}

		if (GinPageRightMost(page) && BufferIsValid(me->child->leftBuffer))
		{
			UnlockReleaseBuffer(me->child->leftBuffer);
			me->child->leftBuffer = InvalidBuffer;
		}
	}

	if (GinPageIsLeaf(page))
		isempty = GinDataLeafPageIsEmpty(page);
	else
		isempty = GinPageGetOpaque(page)->maxoff < FirstOffsetNumber;

	if (isempty)
	{
		/* 我们从不删除最左或最右分支 */
		if (BufferIsValid(me->leftBuffer) && !GinPageRightMost(page))
		{
			Assert(!isRoot);
			ginDeletePage(gvs, blkno, BufferGetBlockNumber(me->leftBuffer),
						  me->parent->blkno, myoff, me->parent->isRoot);
			meDelete = true;
		}
	}

	if (!meDelete)
	{
		if (BufferIsValid(me->leftBuffer))
			UnlockReleaseBuffer(me->leftBuffer);
		me->leftBuffer = buffer;
	}
	else
	{
		if (!isRoot)
			LockBuffer(buffer, GIN_UNLOCK);

		ReleaseBuffer(buffer);
	}

	if (isRoot)
		ReleaseBuffer(buffer);

	return meDelete;
}


/*
 * 扫描倒排树的各叶子页，删除空元组。若至少有一张
 * 空页，则返回 true。
 */
static bool
ginVacuumPostingTreeLeaves(GinVacuumState *gvs, BlockNumber blkno)
{
	Buffer		buffer;
	Page		page;
	bool		hasVoidPage = false;
	MemoryContext oldCxt;

	/* 找到倒排树的最左叶子页，并以排他模式锁定它 */
	while (true)
	{
		PostingItem *pitem;

		buffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, gvs->strategy);
		LockBuffer(buffer, GIN_SHARE);
		page = BufferGetPage(buffer);

		Assert(GinPageIsData(page));

		if (GinPageIsLeaf(page))
		{
			LockBuffer(buffer, GIN_UNLOCK);
			LockBuffer(buffer, GIN_EXCLUSIVE);
			break;
		}

		Assert(PageGetMaxOffsetNumber(page) >= FirstOffsetNumber);

		pitem = GinDataPageGetPostingItem(page, FirstOffsetNumber);
		blkno = PostingItemGetBlockNumber(pitem);
		Assert(blkno != InvalidBlockNumber);

		UnlockReleaseBuffer(buffer);
	}

	/* 沿右链接遍历倒排树的全部叶子页并清理它们 */
	while (true)
	{
		oldCxt = MemoryContextSwitchTo(gvs->tmpCxt);
		ginVacuumPostingTreeLeaf(gvs->index, buffer, gvs);
		MemoryContextSwitchTo(oldCxt);
		MemoryContextReset(gvs->tmpCxt);

		if (GinDataLeafPageIsEmpty(page))
			hasVoidPage = true;

		blkno = GinPageGetOpaque(page)->rightlink;

		UnlockReleaseBuffer(buffer);

		if (blkno == InvalidBlockNumber)
			break;

		buffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, gvs->strategy);
		LockBuffer(buffer, GIN_EXCLUSIVE);
		page = BufferGetPage(buffer);
	}

	return hasVoidPage;
}

static void
ginVacuumPostingTree(GinVacuumState *gvs, BlockNumber rootBlkno)
{
	if (ginVacuumPostingTreeLeaves(gvs, rootBlkno))
	{
		/*
		 * 至少有一张空页。因此必须重新扫描这棵树，
		 * 删除空页。
		 */
		Buffer		buffer;
		DataPageDeleteStack root,
				   *ptr,
				   *tmp;

		buffer = ReadBufferExtended(gvs->index, MAIN_FORKNUM, rootBlkno,
									RBM_NORMAL, gvs->strategy);

		/*
		 * 对倒排树根取清理锁，以确保没有
		 * 并发插入。
		 */
		LockBufferForCleanup(buffer);

		memset(&root, 0, sizeof(DataPageDeleteStack));
		root.leftBuffer = InvalidBuffer;
		root.isRoot = true;

		ginScanToDelete(gvs, rootBlkno, true, &root, InvalidOffsetNumber);

		ptr = root.child;

		while (ptr)
		{
			tmp = ptr->child;
			pfree(ptr);
			ptr = tmp;
		}

		UnlockReleaseBuffer(buffer);
	}
}

/*
 * 返回修改后的页；若页未被修改则返回 NULL。
 * 函数在首次发生变更之前使用原始页，
 * 然后把页复制到临时页中。
 */
static Page
ginVacuumEntryPage(GinVacuumState *gvs, Buffer buffer, BlockNumber *roots, uint32 *nroot)
{
	Page		origpage = BufferGetPage(buffer),
				tmppage;
	OffsetNumber i,
				maxoff = PageGetMaxOffsetNumber(origpage);

	tmppage = origpage;

	*nroot = 0;

	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(tmppage, PageGetItemId(tmppage, i));

		if (GinIsPostingTree(itup))
		{
			/*
			 * 保存倒排树的根，供后续处理；此刻不能
			 * 清理它，以免与扫描/插入发生死锁
			 */
			roots[*nroot] = GinGetDownlink(itup);
			(*nroot)++;
		}
		else if (GinGetNPosting(itup) > 0)
		{
			int			nitems;
			ItemPointer items_orig;
			bool		free_items_orig;
			ItemPointer items;

			/* 从元组取得项指针列表。 */
			if (GinItupIsCompressed(itup))
			{
				items_orig = ginPostingListDecode((GinPostingList *) GinGetPosting(itup), &nitems);
				free_items_orig = true;
			}
			else
			{
				items_orig = (ItemPointer) GinGetPosting(itup);
				nitems = GinGetNPosting(itup);
				free_items_orig = false;
			}

			/* 从列表中移除任何需要清理的项。 */
			items = ginVacuumItemPointers(gvs, items_orig, nitems, &nitems);

			if (free_items_orig)
				pfree(items_orig);

			/* 若移除了任何项指针，则重建元组。 */
			if (items)
			{
				OffsetNumber attnum;
				Datum		key;
				GinNullCategory category;
				GinPostingList *plist;
				int			plistsize;

				if (nitems > 0)
				{
					plist = ginCompressPostingList(items, nitems, GinMaxItemSize, NULL);
					plistsize = SizeOfGinPostingList(plist);
				}
				else
				{
					plist = NULL;
					plistsize = 0;
				}

				/*
				 * 若已经创建了临时页，则就地
				 * 修改
				 */
				if (tmppage == origpage)
				{
					/*
					 * 首次出现差异时，创建该页的临时副本，
					 * 并把元组的倒排列表复制过去。
					 */
					tmppage = PageGetTempPageCopy(origpage);

					/* 把 itup 指针设为新页 */
					itup = (IndexTuple) PageGetItem(tmppage, PageGetItemId(tmppage, i));
				}

				attnum = gintuple_get_attrnum(&gvs->ginstate, itup);
				key = gintuple_get_key(&gvs->ginstate, itup, &category);
				itup = GinFormTuple(&gvs->ginstate, attnum, key, category,
									(char *) plist, plistsize,
									nitems, true);
				if (plist)
					pfree(plist);
				PageIndexTupleDelete(tmppage, i);

				if (PageAddItem(tmppage, (Item) itup, IndexTupleSize(itup), i, false, false) != i)
					elog(ERROR, "failed to add item to index page in \"%s\"",
						 RelationGetRelationName(gvs->index));

				pfree(itup);
				pfree(items);
			}
		}
	}

	return (tmppage == origpage) ? NULL : tmppage;
}

IndexBulkDeleteResult *
ginbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			  IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	BlockNumber blkno = GIN_ROOT_BLKNO;
	GinVacuumState gvs;
	Buffer		buffer;
	BlockNumber rootOfPostingTree[BLCKSZ / (sizeof(IndexTupleData) + sizeof(ItemId))];
	uint32		nRoot;

	gvs.tmpCxt = AllocSetContextCreate(CurrentMemoryContext,
									   "Gin vacuum temporary context",
									   ALLOCSET_DEFAULT_SIZES);
	gvs.index = index;
	gvs.callback = callback;
	gvs.callback_state = callback_state;
	gvs.strategy = info->strategy;
	initGinState(&gvs.ginstate, index);

	/* 第一次？ */
	if (stats == NULL)
	{
		/* 是的，因此把统计信息初始化为零 */
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

		/*
		 * 并清理任何挂起插入
		 */
		ginInsertCleanup(&gvs.ginstate, !IsAutoVacuumWorkerProcess(),
						 false, true, stats);
	}

	/* 每次都会重新统计元组 */
	stats->num_index_tuples = 0;
	gvs.result = stats;

	buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
								RBM_NORMAL, info->strategy);

	/* 寻找叶子页 */
	for (;;)
	{
		Page		page = BufferGetPage(buffer);
		IndexTuple	itup;

		LockBuffer(buffer, GIN_SHARE);

		Assert(!GinPageIsData(page));

		if (GinPageIsLeaf(page))
		{
			LockBuffer(buffer, GIN_UNLOCK);
			LockBuffer(buffer, GIN_EXCLUSIVE);

			if (blkno == GIN_ROOT_BLKNO && !GinPageIsLeaf(page))
			{
				LockBuffer(buffer, GIN_UNLOCK);
				continue;		/* 再检查一次 */
			}
			break;
		}

		Assert(PageGetMaxOffsetNumber(page) >= FirstOffsetNumber);

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, FirstOffsetNumber));
		blkno = GinGetDownlink(itup);
		Assert(blkno != InvalidBlockNumber);

		UnlockReleaseBuffer(buffer);
		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);
	}

	/* 此刻已找到条目 B-树中的最左页 */

	for (;;)
	{
		Page		page = BufferGetPage(buffer);
		Page		resPage;
		uint32		i;

		Assert(!GinPageIsData(page));

		resPage = ginVacuumEntryPage(&gvs, buffer, rootOfPostingTree, &nRoot);

		blkno = GinPageGetOpaque(page)->rightlink;

		if (resPage)
		{
			START_CRIT_SECTION();
			PageRestoreTempPage(resPage, page);
			MarkBufferDirty(buffer);
			xlogVacuumPage(gvs.index, buffer);
			UnlockReleaseBuffer(buffer);
			END_CRIT_SECTION();
		}
		else
		{
			UnlockReleaseBuffer(buffer);
		}

		vacuum_delay_point();

		for (i = 0; i < nRoot; i++)
		{
			ginVacuumPostingTree(&gvs, rootOfPostingTree[i]);
			vacuum_delay_point();
		}

		if (blkno == InvalidBlockNumber)	/* 最右页 */
			break;

		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);
		LockBuffer(buffer, GIN_EXCLUSIVE);
	}

	MemoryContextDelete(gvs.tmpCxt);

	return gvs.result;
}

IndexBulkDeleteResult *
ginvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	index = info->index;
	bool		needLock;
	BlockNumber npages,
				blkno;
	BlockNumber totFreePages;
	GinState	ginstate;
	GinStatsData idxStat;

	/*
	 * 在自动清理执行的 ANALYZE 中，我们要清理挂起插入。
	 * 否则，仅 ANALYZE 的调用是空操作。
	 */
	if (info->analyze_only)
	{
		if (IsAutoVacuumWorkerProcess())
		{
			initGinState(&ginstate, index);
			ginInsertCleanup(&ginstate, false, true, true, stats);
		}
		return stats;
	}

	/*
	 * 若未调用 ginbulkdelete，则建立全零统计信息并清理
	 * 挂起插入
	 */
	if (stats == NULL)
	{
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
		initGinState(&ginstate, index);
		ginInsertCleanup(&ginstate, !IsAutoVacuumWorkerProcess(),
						 false, true, stats);
	}

	memset(&idxStat, 0, sizeof(idxStat));

	/*
	 * XXX 我们总是把堆元组数报告为索引条目数。
	 * 若索引是部分索引，这就不准确，但要弄清一个 GIN 索引
	 * 引用了多少个不同的堆条目实在很难。
	 */
	stats->num_index_tuples = Max(info->num_heap_tuples, 0);
	stats->estimated_count = info->estimated_count;

	/*
	 * 除非它是本后端本地的，否则需要加锁。
	 */
	needLock = !RELATION_IS_LOCAL(index);

	if (needLock)
		LockRelationForExtension(index, ExclusiveLock);
	npages = RelationGetNumberOfBlocks(index);
	if (needLock)
		UnlockRelationForExtension(index, ExclusiveLock);

	totFreePages = 0;

	for (blkno = GIN_ROOT_BLKNO; blkno < npages; blkno++)
	{
		Buffer		buffer;
		Page		page;

		vacuum_delay_point();

		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);
		LockBuffer(buffer, GIN_SHARE);
		page = (Page) BufferGetPage(buffer);

		if (GinPageIsRecyclable(page))
		{
			Assert(blkno != GIN_ROOT_BLKNO);
			RecordFreeIndexPage(index, blkno);
			totFreePages++;
		}
		else if (GinPageIsData(page))
		{
			idxStat.nDataPages++;
		}
		else if (!GinPageIsList(page))
		{
			idxStat.nEntryPages++;

			if (GinPageIsLeaf(page))
				idxStat.nEntries += PageGetMaxOffsetNumber(page);
		}

		UnlockReleaseBuffer(buffer);
	}

	/* 用准确的页数和条目数更新元页 */
	idxStat.nTotalPages = npages;
	ginUpdateStats(info->index, &idxStat, false);

	/* 最后，清理 FSM */
	IndexFreeSpaceMapVacuum(info->index);

	stats->pages_free = totFreePages;

	if (needLock)
		LockRelationForExtension(index, ExclusiveLock);
	stats->num_pages = RelationGetNumberOfBlocks(index);
	if (needLock)
		UnlockRelationForExtension(index, ExclusiveLock);

	return stats;
}

/*
 * 返回该页能否安全回收。
 */
bool
GinPageIsRecyclable(Page page)
{
	TransactionId delete_xid;

	if (PageIsNew(page))
		return true;

	if (!GinPageIsDeleted(page))
		return false;

	delete_xid = GinPageGetDeleteXid(page);

	if (!TransactionIdIsValid(delete_xid))
		return true;

	/*
	 * 若已没有后端仍会把 delete_xid 看作正在运行，则所有与
	 * ginDeletePage() 并发的扫描必定已经结束。
	 */
	return GlobalVisCheckRemovableXid(NULL, delete_xid);
}
