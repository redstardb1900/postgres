/*-------------------------------------------------------------------------
 *
 * ginget.c
 *	  从 GIN 扫描中取回元组。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginget.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/relscan.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "storage/predicate.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* GUC 参数 */
int			GinFuzzySearchLimit = 0;

typedef struct pendingPosition
{
	Buffer		pendingBuffer;
	OffsetNumber firstOffset;
	OffsetNumber lastOffset;
	ItemPointerData item;
	bool	   *hasMatchKey;
} pendingPosition;


/*
 * 若当前偏移超出范围则前进到下一页
 */
static bool
moveRightIfItNeeded(GinBtreeData *btree, GinBtreeStack *stack, Snapshot snapshot)
{
	Page		page = BufferGetPage(stack->buffer);

	if (stack->off > PageGetMaxOffsetNumber(page))
	{
		/*
		 * 整页已扫完，应取右页
		 */
		if (GinPageRightMost(page))
			return false;		/* 没有更多页 */

		stack->buffer = ginStepRight(stack->buffer, btree->index, GIN_SHARE);
		stack->blkno = BufferGetBlockNumber(stack->buffer);
		stack->off = FirstOffsetNumber;
		PredicateLockPage(btree->index, stack->blkno, snapshot);
	}

	return true;
}

/*
 * 扫描倒排树的所有页，并把其中全部堆 ItemPointers
 * 存入 scanEntry->matchBitmap
 */
static void
scanPostingTree(Relation index, GinScanEntry scanEntry,
				BlockNumber rootPostingTree, Snapshot snapshot)
{
	GinBtreeData btree;
	GinBtreeStack *stack;
	Buffer		buffer;
	Page		page;

	/* 下降到最左叶子页 */
	stack = ginScanBeginPostingTree(&btree, index, rootPostingTree, snapshot);
	buffer = stack->buffer;

	IncrBufferRefCount(buffer); /* 防止 freeGinBtreeStack 中 unpin */

	freeGinBtreeStack(stack);

	/*
	 * 循环遍历倒排树的全部叶子页
	 */
	for (;;)
	{
		page = BufferGetPage(buffer);
		if ((GinPageGetOpaque(page)->flags & GIN_DELETED) == 0)
		{
			int			n = GinDataLeafPageGetItemsToTbm(page, scanEntry->matchBitmap);

			scanEntry->predictNumberResult += n;
		}

		if (GinPageRightMost(page))
			break;				/* 没有更多页 */

		buffer = ginStepRight(buffer, index, GIN_SHARE);
	}

	UnlockReleaseBuffer(buffer);
}

/*
 * 为所有匹配搜索条目的堆元组，把 TIDs 收集进
 * scanEntry->matchBitmap。支持三种不同匹配模式：
 *
 * 1. 部分匹配支持：从当前点扫描，直到
 *	  comparePartialFn 表示结束。
 * 2. SEARCH_MODE_ALL：从当前点（应为当前 attnum
 *	  的第一个键）扫描，直到遇到空项或 attnum 结束
 * 3. SEARCH_MODE_EVERYTHING：从当前点（应为当前 attnum
 *	  的第一个键）扫描，直到 attnum 结束
 *
 * 完成返回 true；若必须从头重启扫描则返回 false
 */
static bool
collectMatchBitmap(GinBtreeData *btree, GinBtreeStack *stack,
				   GinScanEntry scanEntry, Snapshot snapshot)
{
	OffsetNumber attnum;
	Form_pg_attribute attr;

	/* 初始化空位图结果 */
	scanEntry->matchBitmap = tbm_create(work_mem * 1024L, NULL);

	/* 空查询无法部分匹配任何东西 */
	if (scanEntry->isPartialMatch &&
		scanEntry->queryCategory != GIN_CAT_NORM_KEY)
		return true;

	/* 定位键列的 tupdesc 条目（供 attbyval/attlen 数据使用） */
	attnum = scanEntry->attnum;
	attr = TupleDescAttr(btree->ginstate->origTupdesc, attnum - 1);

	/*
	 * 对条目叶子页加谓词锁，后续页由
	 * moveRightIfItNeeded() 加锁
	 */
	PredicateLockPage(btree->index,
					  BufferGetBlockNumber(stack->buffer),
					  snapshot);

	for (;;)
	{
		Page		page;
		IndexTuple	itup;
		Datum		idatum;
		GinNullCategory icategory;

		/*
		 * stack->off 指向关心的条目，缓冲区已加锁
		 */
		if (moveRightIfItNeeded(btree, stack, snapshot) == false)
			return true;

		page = BufferGetPage(stack->buffer);
		TestForOldSnapshot(snapshot, btree->index, page);
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stack->off));

		/*
		 * 若元组存放另一属性则停止扫描
		 */
		if (gintuple_get_attrnum(btree->ginstate, itup) != attnum)
			return true;

		/* 可安全取属性值 */
		idatum = gintuple_get_key(btree->ginstate, itup, &icategory);

		/*
		 * 检查适当的扫描停止条件
		 */
		if (scanEntry->isPartialMatch)
		{
			int32		cmp;

			/*
			 * 部分匹配时，在任何空值处停止扫描（包括
			 * 占位符）；部分匹配从不匹配空值
			 */
			if (icategory != GIN_CAT_NORM_KEY)
				return true;

			/*----------
			 * 检查部分匹配。
			 * case cmp == 0 => 匹配
			 * case cmp > 0 => 不匹配并结束扫描
			 * case cmp < 0 => 不匹配并继续扫描
			 *----------
			 */
			cmp = DatumGetInt32(FunctionCall4Coll(&btree->ginstate->comparePartialFn[attnum - 1],
												  btree->ginstate->supportCollation[attnum - 1],
												  scanEntry->queryKey,
												  idatum,
												  UInt16GetDatum(scanEntry->strategy),
												  PointerGetDatum(scanEntry->extra_data)));

			if (cmp > 0)
				return true;
			else if (cmp < 0)
			{
				stack->off++;
				continue;
			}
		}
		else if (scanEntry->searchMode == GIN_SEARCH_MODE_ALL)
		{
			/*
			 * 在 ALL 模式下，我们对空项不感兴趣，因此若
			 * 到达空项占位符（给定 attnum 的最后一条）
			 * 即可停止。不过仍要包含 NULL_KEY
			 * 和 EMPTY_ITEM 条目。
			 */
			if (icategory == GIN_CAT_NULL_ITEM)
				return true;
		}

		/*
		 * 好，我们要返回本条目中列出的 TIDs。
		 */
		if (GinIsPostingTree(itup))
		{
			BlockNumber rootPostingTree = GinGetPostingTree(itup);

			/*
			 * 在树扫描期间应解锁当前页（但不 unpin），
			 * 以免与 vacuum 进程死锁。
			 *
			 * 保存当前条目值 (idatum)，以便重新加锁后
			 * 能重新找到我们的元组
			 */
			if (icategory == GIN_CAT_NORM_KEY)
				idatum = datumCopy(idatum, attr->attbyval, attr->attlen);

			LockBuffer(stack->buffer, GIN_UNLOCK);

			/*
			 * 对倒排树加谓词锁。我们已持有条目页上的
			 * 锁，但对倒排树的插入不会在该层级
			 * 检查冲突。
			 */
			PredicateLockPage(btree->index, rootPostingTree, snapshot);

			/* 收集本条目倒排树中的全部 TIDs */
			scanPostingTree(btree->index, scanEntry, rootPostingTree,
							snapshot);

			/*
			 * 再次锁定条目页；解锁期间可能发生插入，
			 * 因此需要重新找到我们的位置。
			 */
			LockBuffer(stack->buffer, GIN_SHARE);
			page = BufferGetPage(stack->buffer);
			if (!GinPageIsLeaf(page))
			{
				/*
				 * 解锁期间根页变成非叶子。我们将
				 * 重新开始；这种情况不常发生——根在
				 * 索引生命周期中只会变成非叶子一次。
				 */
				return false;
			}

			/* 向前搜索以重新找到 idatum */
			for (;;)
			{
				if (moveRightIfItNeeded(btree, stack, snapshot) == false)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("failed to re-find tuple within index \"%s\"",
									RelationGetRelationName(btree->index))));

				page = BufferGetPage(stack->buffer);
				itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stack->off));

				if (gintuple_get_attrnum(btree->ginstate, itup) == attnum)
				{
					Datum		newDatum;
					GinNullCategory newCategory;

					newDatum = gintuple_get_key(btree->ginstate, itup,
												&newCategory);

					if (ginCompareEntries(btree->ginstate, attnum,
										  newDatum, newCategory,
										  idatum, icategory) == 0)
						break;	/* 找到了！ */
				}

				stack->off++;
			}

			if (icategory == GIN_CAT_NORM_KEY && !attr->attbyval)
				pfree(DatumGetPointer(idatum));
		}
		else
		{
			ItemPointer ipd;
			int			nipd;

			ipd = ginReadTuple(btree->ginstate, scanEntry->attnum, itup, &nipd);
			tbm_add_tuples(scanEntry->matchBitmap, ipd, nipd, false);
			scanEntry->predictNumberResult += GinGetNPosting(itup);
			pfree(ipd);
		}

		/*
		 * 本条目处理完毕，转到下一条
		 */
		stack->off++;
	}
}

/*
 * Start* 函数设置搜索的起始状态：找到正确缓冲区并钉住它。
 */
static void
startScanEntry(GinState *ginstate, GinScanEntry entry, Snapshot snapshot)
{
	GinBtreeData btreeEntry;
	GinBtreeStack *stackEntry;
	Page		page;
	bool		needUnlock;

restartScanEntry:
	entry->buffer = InvalidBuffer;
	ItemPointerSetMin(&entry->curItem);
	entry->offset = InvalidOffsetNumber;
	if (entry->list)
		pfree(entry->list);
	entry->list = NULL;
	entry->nlist = 0;
	entry->matchBitmap = NULL;
	entry->matchResult = NULL;
	entry->reduceResult = false;
	entry->predictNumberResult = 0;

	/*
	 * 应找到条目，并开始扫描倒排树，或只是把
	 * 倒排列表存入内存
	 */
	ginPrepareEntryScan(&btreeEntry, entry->attnum,
						entry->queryKey, entry->queryCategory,
						ginstate);
	stackEntry = ginFindLeafPage(&btreeEntry, true, false, snapshot);
	page = BufferGetPage(stackEntry->buffer);

	/* ginFindLeafPage() 应已检查过快照年龄。 */
	needUnlock = true;

	entry->isFinished = true;

	if (entry->isPartialMatch ||
		entry->queryCategory == GIN_CAT_EMPTY_QUERY)
	{
		/*
		 * btreeEntry.findItem 定位第一个 >= 给定搜索键的项。
		 * （对 GIN_CAT_EMPTY_QUERY，会找到最左索引项，
		 * 因为 GIN_CAT_EMPTY_QUERY 类别码的
		 * 赋值方式。）我们从那里向前扫描，收集该条目
		 * 类型所需的全部 TIDs。
		 */
		btreeEntry.findItem(&btreeEntry, stackEntry);
		if (collectMatchBitmap(&btreeEntry, stackEntry, entry, snapshot)
			== false)
		{
			/*
			 * GIN 树被严重重构，因此将清理所有
			 * 已找到数据并重新扫描。见 collectMatchBitmap()
			 * 中 'return false' 附近的注释
			 */
			if (entry->matchBitmap)
			{
				if (entry->matchIterator)
					tbm_end_iterate(entry->matchIterator);
				entry->matchIterator = NULL;
				tbm_free(entry->matchBitmap);
				entry->matchBitmap = NULL;
			}
			LockBuffer(stackEntry->buffer, GIN_UNLOCK);
			freeGinBtreeStack(stackEntry);
			goto restartScanEntry;
		}

		if (entry->matchBitmap && !tbm_is_empty(entry->matchBitmap))
		{
			entry->matchIterator = tbm_begin_iterate(entry->matchBitmap);
			entry->isFinished = false;
		}
	}
	else if (btreeEntry.findItem(&btreeEntry, stackEntry))
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stackEntry->off));

		if (GinIsPostingTree(itup))
		{
			BlockNumber rootPostingTree = GinGetPostingTree(itup);
			GinBtreeStack *stack;
			Page		entrypage;
			ItemPointerData minItem;

			/*
			 * 这是相等扫描，因此锁定倒排树的根。
			 * 它表示对精确键值的锁，并覆盖倒排树中的
			 * 全部项。
			 */
			PredicateLockPage(ginstate->index, rootPostingTree, snapshot);

			/*
			 * 在接触倒排树之前应解锁条目页，以免与
			 * vacuum 进程死锁。因为条目从不从页中
			 * 删除，且倒排树从不收缩为
			 * 倒排列表，在取得倒排树根的
			 * BlockNumber 后即可解锁该页。
			 */
			LockBuffer(stackEntry->buffer, GIN_UNLOCK);
			needUnlock = false;

			stack = ginScanBeginPostingTree(&entry->btree, ginstate->index,
											rootPostingTree, snapshot);
			entry->buffer = stack->buffer;

			/*
			 * 保持缓冲区钉住，因为需要防止扫描期间
			 * 删除该页。见 GIN 的 vacuum 实现。增加
			 * RefCount，以便 freeGinBtreeStack() 调用后仍钉住。
			 */
			IncrBufferRefCount(entry->buffer);

			entrypage = BufferGetPage(entry->buffer);

			/*
			 * 把第一页载入内存。
			 */
			ItemPointerSetMin(&minItem);
			entry->list = GinDataLeafPageGetItems(entrypage, &entry->nlist, minItem);

			entry->predictNumberResult = stack->predictNumber * entry->nlist;

			LockBuffer(entry->buffer, GIN_UNLOCK);
			freeGinBtreeStack(stack);
			entry->isFinished = false;
		}
		else
		{
			/*
			 * 锁定条目叶子页。这比必要的粒度更粗，
			 * 因为它会与落在同一叶子页上的任何插入
			 * 冲突，而不仅是我们搜索的精确键。
			 * 但锁定单个元组需要在插入或 vacuum 使其
			 * 移动时更新该锁，这似乎
			 * 过于复杂。
			 */
			PredicateLockPage(ginstate->index,
							  BufferGetBlockNumber(stackEntry->buffer),
							  snapshot);
			if (GinGetNPosting(itup) > 0)
			{
				entry->list = ginReadTuple(ginstate, entry->attnum, itup,
										   &entry->nlist);
				entry->predictNumberResult = entry->nlist;

				entry->isFinished = false;
			}
		}
	}
	else
	{
		/*
		 * 未找到条目。对叶子页加谓词锁，锁定若存在
		 * 条目本应所在的位置。
		 */
		PredicateLockPage(ginstate->index,
						  BufferGetBlockNumber(stackEntry->buffer), snapshot);
	}

	if (needUnlock)
		LockBuffer(stackEntry->buffer, GIN_UNLOCK);
	freeGinBtreeStack(stackEntry);
}

/*
 * 扫描条目下标的比较函数。按 predictNumberResult 排序，
 * 最少见的项在前。
 */
static int
entryIndexByFrequencyCmp(const void *a1, const void *a2, void *arg)
{
	const GinScanKey key = (const GinScanKey) arg;
	int			i1 = *(const int *) a1;
	int			i2 = *(const int *) a2;
	uint32		n1 = key->scanEntry[i1]->predictNumberResult;
	uint32		n2 = key->scanEntry[i2]->predictNumberResult;

	if (n1 < n2)
		return -1;
	else if (n1 == n2)
		return 0;
	else
		return 1;
}

static void
startScanKey(GinState *ginstate, GinScanOpaque so, GinScanKey key)
{
	MemoryContext oldCtx = CurrentMemoryContext;
	int			i;
	int			j;
	int		   *entryIndexes;

	ItemPointerSetMin(&key->curItem);
	key->curItemMatches = false;
	key->recheckCurItem = false;
	key->isFinished = false;

	/*
	 * 把条目分成两个不相交的集合：必需集与附加集。
	 * 附加条目单独不足以构成匹配，还需要必需集中的
	 * 某些项，但 consistent 函数需要它们来
	 * 判定某项是否匹配。扫描时，可以跳过在任何
	 * 必需条目中都没有对应匹配的附加条目中的项。
	 * 这会显著加速类似 "frequent & rare" 的查询，
	 * 若能把频繁词元放进附加集的话。
	 *
	 * 把条目划分进这两个集合可以有许多合法方式。
	 * 保守做法是把一切放进必需集，但放进附加集的
	 * 越多，扫描时能跳过的就越多。为最大化跳过，
	 * 我们尽量把频繁项放进附加集，把较少见的
	 * 放进必需集。为此按频率
	 * (predictNumberResult) 排序条目，并按该顺序
	 * 把条目放进必需集，直到 consistent 函数表示
	 * 其余条目在没有任何必需集项时都不能形成
	 * 匹配。
	 * 其余进入附加集。
	 *
	 * 仅排除型扫描键已知没有必需条目。
	 */
	if (key->excludeOnly)
	{
		MemoryContextSwitchTo(so->keyCtx);

		key->nrequired = 0;
		key->nadditional = key->nentries;
		key->additionalEntries = palloc(key->nadditional * sizeof(GinScanEntry));
		for (i = 0; i < key->nadditional; i++)
			key->additionalEntries[i] = key->scanEntry[i];
	}
	else if (key->nentries > 1)
	{
		MemoryContextSwitchTo(so->tempCtx);

		entryIndexes = (int *) palloc(sizeof(int) * key->nentries);
		for (i = 0; i < key->nentries; i++)
			entryIndexes[i] = i;
		qsort_arg(entryIndexes, key->nentries, sizeof(int),
				  entryIndexByFrequencyCmp, key);

		for (i = 1; i < key->nentries; i++)
			key->entryRes[entryIndexes[i]] = GIN_MAYBE;
		for (i = 0; i < key->nentries - 1; i++)
		{
			/* 把所有 <= i 的条目传为 FALSE，其余为 MAYBE */
			key->entryRes[entryIndexes[i]] = GIN_FALSE;

			if (key->triConsistentFn(key) == GIN_FALSE)
				break;

			/* 键很多时使本循环可中断 */
			CHECK_FOR_INTERRUPTS();
		}
		/* i 现在是最后一个必需条目。 */

		MemoryContextSwitchTo(so->keyCtx);

		key->nrequired = i + 1;
		key->nadditional = key->nentries - key->nrequired;
		key->requiredEntries = palloc(key->nrequired * sizeof(GinScanEntry));
		key->additionalEntries = palloc(key->nadditional * sizeof(GinScanEntry));

		j = 0;
		for (i = 0; i < key->nrequired; i++)
			key->requiredEntries[i] = key->scanEntry[entryIndexes[j++]];
		for (i = 0; i < key->nadditional; i++)
			key->additionalEntries[i] = key->scanEntry[entryIndexes[j++]];

		/* consistentFn 调用后清理（也释放 entryIndexes） */
		MemoryContextReset(so->tempCtx);
	}
	else
	{
		MemoryContextSwitchTo(so->keyCtx);

		key->nrequired = 1;
		key->nadditional = 0;
		key->requiredEntries = palloc(1 * sizeof(GinScanEntry));
		key->requiredEntries[0] = key->scanEntry[0];
	}
	MemoryContextSwitchTo(oldCtx);
}

static void
startScan(IndexScanDesc scan)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	GinState   *ginstate = &so->ginstate;
	uint32		i;

	for (i = 0; i < so->totalentries; i++)
		startScanEntry(ginstate, so->entries[i], scan->xs_snapshot);

	if (GinFuzzySearchLimit > 0)
	{
		/*
		 * 若所有键都超过阈值，我们将尝试缩减结果；我们
		 * 希望（也仅仅是希望，对数组的交集运算我们的
		 * 假定并不成立）总结果不会超过
		 * 最小的 predictNumberResult。
		 */
		bool		reduce = true;

		for (i = 0; i < so->totalentries; i++)
		{
			if (so->entries[i]->predictNumberResult <= so->totalentries * GinFuzzySearchLimit)
			{
				reduce = false;
				break;
			}
		}
		if (reduce)
		{
			for (i = 0; i < so->totalentries; i++)
			{
				so->entries[i]->predictNumberResult /= so->totalentries;
				so->entries[i]->reduceResult = true;
			}
		}
	}

	/*
	 * 既然已有条目频率估计，完成
	 * 扫描键的初始化。
	 */
	for (i = 0; i < so->nkeys; i++)
		startScanKey(ginstate, so, so->keys + i);
}

/*
 * 从倒排树加载下一批项指针。
 *
 * 注意：我们把页复制进 GinScanEntry->list 数组并解锁它，但
 * 保持钉住以防止与 vacuum 互相干扰。
 */
static void
entryLoadMoreItems(GinState *ginstate, GinScanEntry entry,
				   ItemPointerData advancePast, Snapshot snapshot)
{
	Page		page;
	int			i;
	bool		stepright;

	if (!BufferIsValid(entry->buffer))
	{
		entry->isFinished = true;
		return;
	}

	/*
	 * 寻找正确页有两种策略：从当前页向右步进，
	 * 或从根再次下降树。若
	 * advancePast 等于当前项，则下一个匹配项应在
	 * 下一页，因此向右步进。否则从根下降。
	 */
	if (ginCompareItemPointers(&entry->curItem, &advancePast) == 0)
	{
		stepright = true;
		LockBuffer(entry->buffer, GIN_SHARE);
	}
	else
	{
		GinBtreeStack *stack;

		ReleaseBuffer(entry->buffer);

		/*
		 * 设置搜索键，并找到正确的叶子页。
		 */
		if (ItemPointerIsLossyPage(&advancePast))
		{
			ItemPointerSet(&entry->btree.itemptr,
						   GinItemPointerGetBlockNumber(&advancePast) + 1,
						   FirstOffsetNumber);
		}
		else
		{
			ItemPointerSet(&entry->btree.itemptr,
						   GinItemPointerGetBlockNumber(&advancePast),
						   OffsetNumberNext(GinItemPointerGetOffsetNumber(&advancePast)));
		}
		entry->btree.fullScan = false;
		stack = ginFindLeafPage(&entry->btree, true, false, snapshot);

		/* 我们不需要栈，只要缓冲区。 */
		entry->buffer = stack->buffer;
		IncrBufferRefCount(entry->buffer);
		freeGinBtreeStack(stack);
		stepright = false;
	}

	elog(DEBUG2, "entryLoadMoreItems, %u/%u, skip: %d",
		 GinItemPointerGetBlockNumber(&advancePast),
		 GinItemPointerGetOffsetNumber(&advancePast),
		 !stepright);

	page = BufferGetPage(entry->buffer);
	for (;;)
	{
		entry->offset = InvalidOffsetNumber;
		if (entry->list)
		{
			pfree(entry->list);
			entry->list = NULL;
			entry->nlist = 0;
		}

		if (stepright)
		{
			/*
			 * 本页上的条目都已处理完。若它是树中的
			 * 最后一页，我们就完成了。
			 */
			if (GinPageRightMost(page))
			{
				UnlockReleaseBuffer(entry->buffer);
				entry->buffer = InvalidBuffer;
				entry->isFinished = true;
				return;
			}

			/*
			 * 沿右链步进到下一页，然后找到
			 * 第一个大于 advancePast 的 ItemPointer。
			 */
			entry->buffer = ginStepRight(entry->buffer,
										 ginstate->index,
										 GIN_SHARE);
			page = BufferGetPage(entry->buffer);
		}
		stepright = true;

		if (GinPageGetOpaque(page)->flags & GIN_DELETED)
			continue;			/* 页被并发 vacuum 删除 */

		/*
		 * 第一个 > advancePast 的项可能不在本页，而在
		 * 右侧某处——若页发生过分裂，或查询中另一键的
		 * 不匹配使我们跳过了本条目的某些项。
		 * 继续沿右链前进，直到重新找到正确
		 * 页。
		 */
		if (!GinPageRightMost(page) &&
			ginCompareItemPointers(&advancePast, GinDataPageGetRightBound(page)) >= 0)
		{
			/*
			 * 我们要找的项 > 本页右界，因此它
			 * 不可能在本页上。
			 */
			continue;
		}

		entry->list = GinDataLeafPageGetItems(page, &entry->nlist, advancePast);

		for (i = 0; i < entry->nlist; i++)
		{
			if (ginCompareItemPointers(&advancePast, &entry->list[i]) < 0)
			{
				entry->offset = i;

				if (GinPageRightMost(page))
				{
					/* 处理完复制的项后，我们就完成了。 */
					UnlockReleaseBuffer(entry->buffer);
					entry->buffer = InvalidBuffer;
				}
				else
					LockBuffer(entry->buffer, GIN_UNLOCK);
				return;
			}
		}
	}
}

#define gin_rand() pg_prng_double(&pg_global_prng_state)
#define dropItem(e) ( gin_rand() > ((double)GinFuzzySearchLimit)/((double)((e)->predictNumberResult)) )

/*
 * 为某个扫描键的一个条目，把 entry->curItem 设为下一个
 * > advancePast 的堆项指针；若没有更多则设 entry->isFinished 为 true。
 *
 * 项指针按升序返回。
 *
 * 注意：这可以返回"有损页"项指针，表示该
 * 条目可能匹配该堆页上的全部项。但不允许
 * 对同一页既返回有损页指针又返回精确（常规）
 * 项指针。（那样会破坏 keyGetItem 与 scanGetItem
 * 中的键组合逻辑；见 scanGetItem 中的注释。）当前
 * 实现由 tidbitmap 的行为保证这一点。
 */
static void
entryGetItem(GinState *ginstate, GinScanEntry entry,
			 ItemPointerData advancePast, Snapshot snapshot)
{
	Assert(!entry->isFinished);

	Assert(!ItemPointerIsValid(&entry->curItem) ||
		   ginCompareItemPointers(&entry->curItem, &advancePast) <= 0);

	if (entry->matchBitmap)
	{
		/* 位图结果 */
		BlockNumber advancePastBlk = GinItemPointerGetBlockNumber(&advancePast);
		OffsetNumber advancePastOff = GinItemPointerGetOffsetNumber(&advancePast);

		for (;;)
		{
			/*
			 * 若本块上的项已耗尽，移到位图中的
			 * 下一块。
			 */
			while (entry->matchResult == NULL ||
				   (entry->matchResult->ntuples >= 0 &&
					entry->offset >= entry->matchResult->ntuples) ||
				   entry->matchResult->blockno < advancePastBlk ||
				   (ItemPointerIsLossyPage(&advancePast) &&
					entry->matchResult->blockno == advancePastBlk))
			{
				entry->matchResult = tbm_iterate(entry->matchIterator);

				if (entry->matchResult == NULL)
				{
					ItemPointerSetInvalid(&entry->curItem);
					tbm_end_iterate(entry->matchIterator);
					entry->matchIterator = NULL;
					entry->isFinished = true;
					break;
				}

				/*
				 * 把计数器重置到 entry->matchResult 开头。注意：
				 * 若 matchResult 有损，entry->offset 仍会大于
				 * matchResult->ntuples。因此下次调用时我们将从
				 * TIDBitmap 取得下一个结果。
				 */
				entry->offset = 0;
			}
			if (entry->isFinished)
				break;

			/*
			 * 我们现在位于 advancePast 之后第一个含有
			 * 项的页。若为有损结果，则返回它。
			 */
			if (entry->matchResult->ntuples < 0)
			{
				ItemPointerSetLossyPage(&entry->curItem,
										entry->matchResult->blockno);

				/*
				 * 我们不妨直接跳出循环；即便启用了缩减，
				 * 也无法估计本页上的结果数以支持正确的
				 * 结果缩减。
				 */
				break;
			}

			/*
			 * 不是有损页。跳过任何 <= advancePast 的偏移，并
			 * 返回该项。
			 */
			if (entry->matchResult->blockno == advancePastBlk)
			{
				/*
				 * 首先，对照页上最后一个偏移做快速检查。
				 * 若它 > advancePast，则其余偏移也都是，
				 * 因此直接回到顶部取下一页。
				 */
				if (entry->matchResult->offsets[entry->matchResult->ntuples - 1] <= advancePastOff)
				{
					entry->offset = entry->matchResult->ntuples;
					continue;
				}

				/* 否则扫描以找到第一个 > advancePast 的项 */
				while (entry->matchResult->offsets[entry->offset] <= advancePastOff)
					entry->offset++;
			}

			ItemPointerSet(&entry->curItem,
						   entry->matchResult->blockno,
						   entry->matchResult->offsets[entry->offset]);
			entry->offset++;

			/* 除非需要缩减结果，否则完成 */
			if (!entry->reduceResult || !dropItem(entry))
				break;
		}
	}
	else if (!BufferIsValid(entry->buffer))
	{
		/*
		 * 来自条目元组的倒排列表，或倒排树的
		 * 最后一页。
		 */
		for (;;)
		{
			if (entry->offset >= entry->nlist)
			{
				ItemPointerSetInvalid(&entry->curItem);
				entry->isFinished = true;
				break;
			}

			entry->curItem = entry->list[entry->offset++];

			/* 若尚未越过 advancePast，继续扫描 */
			if (ginCompareItemPointers(&entry->curItem, &advancePast) <= 0)
				continue;

			/* 除非需要缩减结果，否则完成 */
			if (!entry->reduceResult || !dropItem(entry))
				break;
		}
	}
	else
	{
		/* 倒排树 */
		for (;;)
		{
			/* 若当前批次已处理完，加载更多项 */
			while (entry->offset >= entry->nlist)
			{
				entryLoadMoreItems(ginstate, entry, advancePast, snapshot);

				if (entry->isFinished)
				{
					ItemPointerSetInvalid(&entry->curItem);
					return;
				}
			}

			entry->curItem = entry->list[entry->offset++];

			/* 若尚未越过 advancePast，继续扫描 */
			if (ginCompareItemPointers(&entry->curItem, &advancePast) <= 0)
				continue;

			/* 除非需要缩减结果，否则完成 */
			if (!entry->reduceResult || !dropItem(entry))
				break;

			/*
			 * 推进 advancePast（以便 entryLoadMoreItems 加载
			 * 正确数据），并继续扫描
			 */
			advancePast = entry->curItem;
		}
	}
}

/*
 * 在本扫描键的输入条目流中，识别大于
 * advancePast 的"当前"项，并测试它是否通过扫描键
 * 限定条件。
 *
 * 当前项是各输入中最小的 curItem。key->curItem
 * 设为该值。key->curItemMatches 表示该
 * TID 是否通过 consistentFn 测试。若通过，则当本项指针
 * 需要 recheck 时把 key->recheckCurItem 设为 true
 * （包括项指针为有损页指针的情形）。
 *
 * 若所有条目流都已耗尽，把 key->isFinished 设为 true。
 *
 * 项指针必须按升序返回。
 *
 * 注意：这可以返回"有损页"项指针，表示该
 * 键可能匹配该堆页上的全部项。但不允许
 * 对同一页既返回有损页指针又返回精确（常规）
 * 项指针。（那样会破坏 scanGetItem 中的键组合
 * 逻辑。）
 */
static void
keyGetItem(GinState *ginstate, MemoryContext tempCtx, GinScanKey key,
		   ItemPointerData advancePast, Snapshot snapshot)
{
	ItemPointerData minItem;
	ItemPointerData curPageLossy;
	uint32		i;
	bool		haveLossyEntry;
	GinScanEntry entry;
	GinTernaryValue res;
	MemoryContext oldCtx;
	bool		allFinished;

	Assert(!key->isFinished);

	/*
	 * 我们可能已经测试过该项；若是，无需重复工作。
	 * （注意：">" 情形可能发生——若 advancePast 精确，但我们
	 * 先前不得不把 curItem 设为有损页指针。）
	 */
	if (ginCompareItemPointers(&key->curItem, &advancePast) > 0)
		return;

	/*
	 * 在活跃条目流中查找 > advancePast 的最小项。
	 *
	 * 注意：有损页条目编码为偏移取最大值
	 * (0xffff) 的 ItemPointer，因此它会排在同一页任何
	 * 精确条目之后。于是我们更倾向于返回精确指针而非有损
	 * 指针，这很好。
	 */
	ItemPointerSetMax(&minItem);
	allFinished = true;
	for (i = 0; i < key->nrequired; i++)
	{
		entry = key->requiredEntries[i];

		if (entry->isFinished)
			continue;

		/*
		 * 必要时推进本流。
		 *
		 * 特别地，由于 entry->curItem 用
		 * ItemPointerSetMin 初始化，这保证首次调用时为每个
		 * 条目取回第一项。
		 */
		if (ginCompareItemPointers(&entry->curItem, &advancePast) <= 0)
		{
			entryGetItem(ginstate, entry, advancePast, snapshot);
			if (entry->isFinished)
				continue;
		}

		allFinished = false;
		if (ginCompareItemPointers(&entry->curItem, &minItem) < 0)
			minItem = entry->curItem;
	}

	if (allFinished && !key->excludeOnly)
	{
		/* 所有条目都已结束 */
		key->isFinished = true;
		return;
	}

	if (!key->excludeOnly)
	{
		/*
		 * 对普通扫描键，我们现在知道没有 < minItem 的匹配。
		 *
		 * 若 minItem 有损，意味着 requiredEntries 在该页上
		 * 没有精确项，因为有损指针排在精确项之后。
		 * 不过 additionalEntries 中同一页上可能仍有精确项，
		 * 因此不得越过它们。
		 */
		if (ItemPointerIsLossyPage(&minItem))
		{
			if (GinItemPointerGetBlockNumber(&advancePast) <
				GinItemPointerGetBlockNumber(&minItem))
			{
				ItemPointerSet(&advancePast,
							   GinItemPointerGetBlockNumber(&minItem),
							   InvalidOffsetNumber);
			}
		}
		else
		{
			Assert(GinItemPointerGetOffsetNumber(&minItem) > 0);
			ItemPointerSet(&advancePast,
						   GinItemPointerGetBlockNumber(&minItem),
						   OffsetNumberPrev(GinItemPointerGetOffsetNumber(&minItem)));
		}
	}
	else
	{
		/*
		 * excludeOnly 扫描键没有任何必定出现在匹配项中的
		 * 条目。因此，我们考虑紧接
		 * advancePast 之后的项。
		 */
		Assert(key->nrequired == 0);
		ItemPointerSet(&minItem,
					   GinItemPointerGetBlockNumber(&advancePast),
					   OffsetNumberNext(GinItemPointerGetOffsetNumber(&advancePast)));
	}

	/*
	 * 我们可能尚未为本 TID 加载全部条目流。可以
	 * 对那些条目传入 MAYBE 调用 consistent 函数，看它能否
	 * 根据已有信息判定本 TID 是否匹配。但若
	 * consistent 函数很昂贵，且实际上无法用部分信息
	 * 做出决定，那可能损失很大。因此在调用
	 * consistent 函数之前，先加载全部附加条目。
	 */
	for (i = 0; i < key->nadditional; i++)
	{
		entry = key->additionalEntries[i];

		if (entry->isFinished)
			continue;

		if (ginCompareItemPointers(&entry->curItem, &advancePast) <= 0)
		{
			entryGetItem(ginstate, entry, advancePast, snapshot);
			if (entry->isFinished)
				continue;
		}

		/*
		 * 通常，additionalEntries 中任何项的 curItem 都不会
		 * 大于 minItem。但若 minItem 是有损页，则
		 * additionalEntries 中同一页上可能有精确项。
		 */
		if (ginCompareItemPointers(&entry->curItem, &minItem) < 0)
		{
			Assert(ItemPointerIsLossyPage(&minItem));
			minItem = entry->curItem;
		}
	}

	/*
	 * 好，我们已把所有条目推进到 minItem。设置 key->curItem，
	 * 并执行 consistentFn 测试。
	 *
	 * 有损页条目带来问题，因为我们不知道应传给
	 * consistentFn 的正确 entryRes 状态，也不知道其
	 * 组合逻辑是什么（可能是 AND、OR，甚至 NOT）。若
	 * 逻辑是 OR，则即便其他条目都不匹配，consistentFn
	 * 仍可能对有损页上的全部项成功。
	 *
	 * 我们的策略是调用三值 consistent 函数，把
	 * 有损页条目设为 MAYBE，其余条目为 FALSE。若它
	 * 返回 FALSE，则单独的有损项不足以匹配，因此
	 * 无需返回有损页指针。否则返回有损页指针，
	 * 表示必须检查整个堆页。（后续调用中，在
	 * minItem 完全越过该页之前我们什么也不做，从而
	 * 确保从不对同一页既返回常规指针又返回有损
	 * 指针。）
	 *
	 * 例外是：对"隐藏"条目中的有损指针传什么
	 * 无关紧要，因为 consistentFn 的结果不能依赖它们。
	 * 也可以把它们传为 MAYBE，但若使用的是三值
	 * consistent 函数的 "shim" 实现（见
	 * ginlogic.c），最好尽量少传 MAYBEs。因此把它们
	 * 传为 true。
	 *
	 * 注意只有指向当前项所在页的有损页条目
	 * 才应触发本处理；条目数组中可能还有未来的
	 * 有损页，但它们尚不相关。
	 */
	key->curItem = minItem;
	ItemPointerSetLossyPage(&curPageLossy,
							GinItemPointerGetBlockNumber(&key->curItem));
	haveLossyEntry = false;
	for (i = 0; i < key->nentries; i++)
	{
		entry = key->scanEntry[i];
		if (entry->isFinished == false &&
			ginCompareItemPointers(&entry->curItem, &curPageLossy) == 0)
		{
			if (i < key->nuserentries)
				key->entryRes[i] = GIN_MAYBE;
			else
				key->entryRes[i] = GIN_TRUE;
			haveLossyEntry = true;
		}
		else
			key->entryRes[i] = GIN_FALSE;
	}

	/* 准备在临时上下文中调用 consistentFn */
	oldCtx = MemoryContextSwitchTo(tempCtx);

	if (haveLossyEntry)
	{
		/* 有有损页条目，因此看整页是否匹配 */
		res = key->triConsistentFn(key);

		if (res == GIN_TRUE || res == GIN_MAYBE)
		{
			/* 是，因此清理 ... */
			MemoryContextSwitchTo(oldCtx);
			MemoryContextReset(tempCtx);

			/* 并返回整页的有损指针 */
			key->curItem = curPageLossy;
			key->curItemMatches = true;
			key->recheckCurItem = true;
			return;
		}
	}

	/*
	 * 此时我们知道无需返回有损整页
	 * 指针，但可能对各个精确项指针有匹配，
	 * 或许还与有损指针组合。把有损指针作为
	 * MAYBE 传给三值 consistent 函数，让它判定本
	 * 元组是否满足整体键，即便我们不知道有损
	 * 条目是否匹配。
	 *
	 * 准备传给 consistentFn 的 entryRes 数组。
	 */
	for (i = 0; i < key->nentries; i++)
	{
		entry = key->scanEntry[i];
		if (entry->isFinished)
			key->entryRes[i] = GIN_FALSE;
#if 0

		/*
		 * 当前不可能发生本情形，因为我们先前已加载了
		 * 本项的全部条目。
		 */
		else if (ginCompareItemPointers(&entry->curItem, &advancePast) <= 0)
			key->entryRes[i] = GIN_MAYBE;
#endif
		else if (ginCompareItemPointers(&entry->curItem, &curPageLossy) == 0)
			key->entryRes[i] = GIN_MAYBE;
		else if (ginCompareItemPointers(&entry->curItem, &minItem) == 0)
			key->entryRes[i] = GIN_TRUE;
		else
			key->entryRes[i] = GIN_FALSE;
	}

	res = key->triConsistentFn(key);

	switch (res)
	{
		case GIN_TRUE:
			key->curItemMatches = true;
			/* triConsistentFn 设置了 recheckCurItem */
			break;

		case GIN_FALSE:
			key->curItemMatches = false;
			break;

		case GIN_MAYBE:
			key->curItemMatches = true;
			key->recheckCurItem = true;
			break;

		default:

			/*
			 * 'default' 情形本不应发生，但若 consistent
			 * 函数返回了无意义的值，这是安全结果
			 */
			key->curItemMatches = true;
			key->recheckCurItem = true;
			break;
	}

	/*
	 * 我们有一个元组，并知道它是否匹配。若不匹配，
	 * 本可继续找下一个匹配元组，但我们选择跳出，
	 * 让 scanGetItem 有机会推进其他键。它们或许能
	 * 跳到高得多的 TID，从而节省工作。
	 */

	/* consistentFn 调用后清理 */
	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(tempCtx);
}

/*
 * 从扫描取得下一个堆项指针（在 advancePast 之后）。
 * 若找到任何东西则返回 true。
 * 成功时设置 *item 与 *recheck。
 *
 * 注意：这与 keyGetItem() 中的逻辑几乎相同，只是
 * 我们知道键以 AND 逻辑组合，而在
 * keyGetItem() 中组合逻辑仅由 consistentFn 知道。
 */
static bool
scanGetItem(IndexScanDesc scan, ItemPointerData advancePast,
			ItemPointerData *item, bool *recheck)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	uint32		i;
	bool		match;

	/*----------
	 * 同步推进扫描键，直到找到匹配所有键的项。
	 * 若任一键报告 isFinished，表示其条目子集已
	 * 耗尽，我们可以停止。否则把 *item 设为下一个
	 * 匹配项。
	 *
	 * 本逻辑仅在 keyGetItem 流从不可能对同一页
	 * 既含精确指针又含有损指针时成立。否则可能出现
	 * 如下情形
	 *
	 *		stream 1		stream 2
	 *		...             ...
	 *		42/6			42/7
	 *		50/1			42/0xffff
	 *		...             ...
	 *
	 * 我们会断定 42/6 不匹配并推进流 1，
	 * 从而永远检测不到流 2 中对有损指针的匹配。
	 * （keyGetItem 相对 entryGetItem 有类似问题。）
	 *----------
	 */
	do
	{
		CHECK_FOR_INTERRUPTS();

		ItemPointerSetMin(item);
		match = true;
		for (i = 0; i < so->nkeys && match; i++)
		{
			GinScanKey	key = so->keys + i;

			/*
			 * 若正在考虑有损页，跳过 excludeOnly 键。它们
			 * 反正无法排除整页。
			 */
			if (ItemPointerIsLossyPage(item) && key->excludeOnly)
			{
				/*
				 * ginNewScanKey() 绝不应把第一个键标为
				 * excludeOnly。
				 */
				Assert(i > 0);
				continue;
			}

			/* 为本键取回 > advancePast 的下一项。 */
			keyGetItem(&so->ginstate, so->tempCtx, key, advancePast,
					   scan->xs_snapshot);

			if (key->isFinished)
				return false;

			/*
			 * 若不匹配，可立即断定没有任何
			 * <= 该项的匹配，无需检查其余键。
			 */
			if (!key->curItemMatches)
			{
				advancePast = key->curItem;
				match = false;
				break;
			}

			/*
			 * 这是匹配。可断定没有任何 < 的匹配，因此
			 * 其他键流可跳到本项。
			 *
			 * 不过要当心有损指针；从有损指针我们只能
			 * 断定没有任何小于本*块*的匹配。
			 */
			if (ItemPointerIsLossyPage(&key->curItem))
			{
				if (GinItemPointerGetBlockNumber(&advancePast) <
					GinItemPointerGetBlockNumber(&key->curItem))
				{
					ItemPointerSet(&advancePast,
								   GinItemPointerGetBlockNumber(&key->curItem),
								   InvalidOffsetNumber);
				}
			}
			else
			{
				Assert(GinItemPointerGetOffsetNumber(&key->curItem) > 0);
				ItemPointerSet(&advancePast,
							   GinItemPointerGetBlockNumber(&key->curItem),
							   OffsetNumberPrev(GinItemPointerGetOffsetNumber(&key->curItem)));
			}

			/*
			 * 若这是第一个键，记住此位置作为潜在
			 * 匹配，并继续检查其余键。
			 *
			 * 否则，检查这是否与我们为先前键检查的是
			 * 同一项（或同一页的有损指针）。若
			 * 不是，循环回去为该项检查先前的键（我们
			 * 也会再检查本键，但 keyGetItem 对此会快速
			 * 返回）
			 */
			if (i == 0)
			{
				*item = key->curItem;
			}
			else
			{
				if (ItemPointerIsLossyPage(&key->curItem) ||
					ItemPointerIsLossyPage(item))
				{
					Assert(GinItemPointerGetBlockNumber(&key->curItem) >= GinItemPointerGetBlockNumber(item));
					match = (GinItemPointerGetBlockNumber(&key->curItem) ==
							 GinItemPointerGetBlockNumber(item));
				}
				else
				{
					Assert(ginCompareItemPointers(&key->curItem, item) >= 0);
					match = (ginCompareItemPointers(&key->curItem, item) == 0);
				}
			}
		}
	} while (!match);

	Assert(!ItemPointerIsMin(item));

	/*
	 * 现在 *item 包含前一结果之后第一个满足全部键
	 * 的精确 TID 的 ItemPointer，或对同一页的
	 * 有损引用。
	 *
	 * 若任一键标为 recheck，我们必须返回 recheck = true。
	 */
	*recheck = false;
	for (i = 0; i < so->nkeys; i++)
	{
		GinScanKey	key = so->keys + i;

		if (key->recheckCurItem)
		{
			*recheck = true;
			break;
		}
	}

	return true;
}


/*
 * 扫描挂起列表的函数
 */


/*
 * 从挂起列表取得下一个待检查堆行的 ItemPointer。
 * 若没有更多则返回 false。一页上有多行堆行时
 * 分别返回每一行；页上只有堆行一部分时返回
 * 按页数据。设置 pos->firstOffset 与 pos->lastOffset 以标识
 * 属于本堆行的挂起列表元组范围。
 *
 * 进入时假定 pendingBuffer 已钉住并共享锁定；成功
 * 退出时仍钉住并共享锁定。失败退出时释放它。
 */
static bool
scanGetCandidate(IndexScanDesc scan, pendingPosition *pos)
{
	OffsetNumber maxoff;
	Page		page;
	IndexTuple	itup;

	ItemPointerSetInvalid(&pos->item);
	for (;;)
	{
		page = BufferGetPage(pos->pendingBuffer);
		TestForOldSnapshot(scan->xs_snapshot, scan->indexRelation, page);

		maxoff = PageGetMaxOffsetNumber(page);
		if (pos->firstOffset > maxoff)
		{
			BlockNumber blkno = GinPageGetOpaque(page)->rightlink;

			if (blkno == InvalidBlockNumber)
			{
				UnlockReleaseBuffer(pos->pendingBuffer);
				pos->pendingBuffer = InvalidBuffer;

				return false;
			}
			else
			{
				/*
				 * 这里必须防止 insertcleanup 进程删除下一页，
				 * 该进程可能正在试图对当前页取得
				 * 排他锁。因此在释放当前页之前先
				 * 锁定下一页
				 */
				Buffer		tmpbuf = ReadBuffer(scan->indexRelation, blkno);

				LockBuffer(tmpbuf, GIN_SHARE);
				UnlockReleaseBuffer(pos->pendingBuffer);

				pos->pendingBuffer = tmpbuf;
				pos->firstOffset = FirstOffsetNumber;
			}
		}
		else
		{
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, pos->firstOffset));
			pos->item = itup->t_tid;
			if (GinPageHasFullRow(page))
			{
				/*
				 * 找到指向下一行的项指针
				 */
				for (pos->lastOffset = pos->firstOffset + 1; pos->lastOffset <= maxoff; pos->lastOffset++)
				{
					itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, pos->lastOffset));
					if (!ItemPointerEquals(&pos->item, &itup->t_tid))
						break;
				}
			}
			else
			{
				/*
				 * 本页上所有项指针都相同
				 */
				pos->lastOffset = maxoff + 1;
			}

			/*
			 * 现在 pos->firstOffset 指向当前堆行的第一个元组，
			 * pos->lastOffset 指向下一堆行的第一个元组
			 * （或指向页末）
			 */
			break;
		}
	}

	return true;
}

/*
 * 从当前元组 (off) 起扫描挂起列表页，直到以下最先发生者：
 * - 找到匹配（然后返回 true）
 * - 不可能再有更晚的匹配
 * - 元组的属性号不等于条目的 attrnum
 * - 到达页末
 *
 * datum[]/category[]/datumExtracted[] 数组用于缓存当前页上
 * gintuple_get_key() 的结果。
 */
static bool
matchPartialInPendingList(GinState *ginstate, Page page,
						  OffsetNumber off, OffsetNumber maxoff,
						  GinScanEntry entry,
						  Datum *datum, GinNullCategory *category,
						  bool *datumExtracted)
{
	IndexTuple	itup;
	int32		cmp;

	/* 不可能对空值做部分匹配 */
	if (entry->queryCategory != GIN_CAT_NORM_KEY)
		return false;

	while (off < maxoff)
	{
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		if (gintuple_get_attrnum(ginstate, itup) != entry->attnum)
			return false;

		if (datumExtracted[off - 1] == false)
		{
			datum[off - 1] = gintuple_get_key(ginstate, itup,
											  &category[off - 1]);
			datumExtracted[off - 1] = true;
		}

		/* 一旦遇到空值，不可能再有更远匹配 */
		if (category[off - 1] != GIN_CAT_NORM_KEY)
			return false;

		/*----------
		 * 检查部分匹配。
		 * case cmp == 0 => 匹配
		 * case cmp > 0 => 不匹配并结束扫描（不可能再有更晚匹配）
		 * case cmp < 0 => 不匹配并继续扫描
		 *----------
		 */
		cmp = DatumGetInt32(FunctionCall4Coll(&ginstate->comparePartialFn[entry->attnum - 1],
											  ginstate->supportCollation[entry->attnum - 1],
											  entry->queryKey,
											  datum[off - 1],
											  UInt16GetDatum(entry->strategy),
											  PointerGetDatum(entry->extra_data)));
		if (cmp == 0)
			return true;
		else if (cmp > 0)
			return false;

		off++;
	}

	return false;
}

/*
 * 通过查看挂起列表中当前堆行的
 * 每个条目，为每个键设置 entryRes 数组。
 *
 * 若每个扫描键至少有一个 entryRes 匹配则返回 true。
 * 这对应于普通索引搜索会尝试对该键
 * 应用 consistentFn 的情形。（不满足该要求的元组
 * 不会被普通搜索返回，因为没有任何条目流会
 * 产出其 TID。）
 *
 * 进入时假定 pendingBuffer 已钉住并共享锁定。
 */
static bool
collectMatchesForHeapRow(IndexScanDesc scan, pendingPosition *pos)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	OffsetNumber attrnum;
	Page		page;
	IndexTuple	itup;
	int			i,
				j;

	/*
	 * 重置全部 entryRes 与 hasMatchKey 标志
	 */
	for (i = 0; i < so->nkeys; i++)
	{
		GinScanKey	key = so->keys + i;

		memset(key->entryRes, GIN_FALSE, key->nentries);
	}
	memset(pos->hasMatchKey, false, so->nkeys);

	/*
	 * 当单个堆行的条目跨越多个挂起列表页时，
	 * 外层循环遍历这些页。
	 */
	for (;;)
	{
		Datum		datum[BLCKSZ / sizeof(IndexTupleData)];
		GinNullCategory category[BLCKSZ / sizeof(IndexTupleData)];
		bool		datumExtracted[BLCKSZ / sizeof(IndexTupleData)];

		Assert(pos->lastOffset > pos->firstOffset);
		memset(datumExtracted + pos->firstOffset - 1, 0,
			   sizeof(bool) * (pos->lastOffset - pos->firstOffset));

		page = BufferGetPage(pos->pendingBuffer);
		TestForOldSnapshot(scan->xs_snapshot, scan->indexRelation, page);

		for (i = 0; i < so->nkeys; i++)
		{
			GinScanKey	key = so->keys + i;

			for (j = 0; j < key->nentries; j++)
			{
				GinScanEntry entry = key->scanEntry[j];
				OffsetNumber StopLow = pos->firstOffset,
							StopHigh = pos->lastOffset,
							StopMiddle;

				/* 若先前页上已匹配，不做额外工作 */
				if (key->entryRes[j])
					continue;

				/*
				 * 关心的元组位于 pos->firstOffset 到
				 * pos->lastOffset，并按 (attnum, Datum) 排序，与
				 * 条目树中相同。因此可用二分搜索
				 * 避免线性扫描。
				 */
				while (StopLow < StopHigh)
				{
					int			res;

					StopMiddle = StopLow + ((StopHigh - StopLow) >> 1);

					itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, StopMiddle));

					attrnum = gintuple_get_attrnum(&so->ginstate, itup);

					if (key->attnum < attrnum)
					{
						StopHigh = StopMiddle;
						continue;
					}
					if (key->attnum > attrnum)
					{
						StopLow = StopMiddle + 1;
						continue;
					}

					if (datumExtracted[StopMiddle - 1] == false)
					{
						datum[StopMiddle - 1] =
							gintuple_get_key(&so->ginstate, itup,
											 &category[StopMiddle - 1]);
						datumExtracted[StopMiddle - 1] = true;
					}

					if (entry->queryCategory == GIN_CAT_EMPTY_QUERY)
					{
						/* 依 searchMode 的特殊行为 */
						if (entry->searchMode == GIN_SEARCH_MODE_ALL)
						{
							/* 匹配除 NULL_ITEM 外的任何东西 */
							if (category[StopMiddle - 1] == GIN_CAT_NULL_ITEM)
								res = -1;
							else
								res = 0;
						}
						else
						{
							/* 匹配一切 */
							res = 0;
						}
					}
					else
					{
						res = ginCompareEntries(&so->ginstate,
												entry->attnum,
												entry->queryKey,
												entry->queryCategory,
												datum[StopMiddle - 1],
												category[StopMiddle - 1]);
					}

					if (res == 0)
					{
						/*
						 * 找到精确匹配（除 EMPTY_QUERY 模式外
						 * 只能有一个）。
						 *
						 * 若做部分匹配，从这里向前扫描到
						 * 页末以检查匹配。
						 *
						 * 见上文关于元组排序的注释。
						 */
						if (entry->isPartialMatch)
							key->entryRes[j] =
								matchPartialInPendingList(&so->ginstate,
														  page,
														  StopMiddle,
														  pos->lastOffset,
														  entry,
														  datum,
														  category,
														  datumExtracted);
						else
							key->entryRes[j] = true;

						/* 二分搜索结束 */
						break;
					}
					else if (res < 0)
						StopHigh = StopMiddle;
					else
						StopLow = StopMiddle + 1;
				}

				if (StopLow >= StopHigh && entry->isPartialMatch)
				{
					/*
					 * 本页上没有精确匹配。若做部分匹配，
					 * 从第一个大于目标值的元组扫描到
					 * 页末。注意由于我们不记得
					 * comparePartialFn 是否在先前页上
					 * 告诉我们提早停止，会对每个后续页的
					 * 第一个元组徒劳地再应用 comparePartialFn。
					 */
					key->entryRes[j] =
						matchPartialInPendingList(&so->ginstate,
												  page,
												  StopHigh,
												  pos->lastOffset,
												  entry,
												  datum,
												  category,
												  datumExtracted);
				}

				pos->hasMatchKey[i] |= key->entryRes[j];
			}
		}

		/* 把 firstOffset 推进过已扫描的元组 */
		pos->firstOffset = pos->lastOffset;

		if (GinPageHasFullRow(page))
		{
			/*
			 * 已检查当前堆行的全部挂起条目。
			 * 跳出遍历页的循环。
			 */
			break;
		}
		else
		{
			/*
			 * 前进到当前堆行挂起条目的下一页。
			 * 若没有则报错。
			 */
			ItemPointerData item = pos->item;

			if (scanGetCandidate(scan, pos) == false ||
				!ItemPointerEquals(&pos->item, &item))
				elog(ERROR, "could not find additional pending pages for same heap tuple");
		}
	}

	/*
	 * 除 excludeOnly 外的所有扫描键都需要至少一个条目匹配。
	 * excludeOnly 键是例外，因为其隐含的
	 * GIN_CAT_EMPTY_QUERY scanEntry 总是匹配。因此若所有
	 * 非 excludeOnly 扫描键至少有一个匹配则返回 "true"。
	 */
	for (i = 0; i < so->nkeys; i++)
	{
		if (pos->hasMatchKey[i] == false && !so->keys[i].excludeOnly)
			return false;
	}

	return true;
}

/*
 * 把挂起列表中所有匹配行收集进位图。
 */
static void
scanPendingInsert(IndexScanDesc scan, TIDBitmap *tbm, int64 *ntids)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	MemoryContext oldCtx;
	bool		recheck,
				match;
	int			i;
	pendingPosition pos;
	Buffer		metabuffer = ReadBuffer(scan->indexRelation, GIN_METAPAGE_BLKNO);
	Page		page;
	BlockNumber blkno;

	*ntids = 0;

	/*
	 * 对元页加谓词锁，以便与任何 fastupdate
	 * 插入冲突。
	 */
	PredicateLockPage(scan->indexRelation, GIN_METAPAGE_BLKNO, scan->xs_snapshot);

	LockBuffer(metabuffer, GIN_SHARE);
	page = BufferGetPage(metabuffer);
	TestForOldSnapshot(scan->xs_snapshot, scan->indexRelation, page);
	blkno = GinPageGetMeta(page)->head;

	/*
	 * 在解锁元页之前取列表头。头页必须钉住，
	 * 以防止被 vacuum 进程删除
	 */
	if (blkno == InvalidBlockNumber)
	{
		/* 无挂起列表，因此继续普通扫描 */
		UnlockReleaseBuffer(metabuffer);
		return;
	}

	pos.pendingBuffer = ReadBuffer(scan->indexRelation, blkno);
	LockBuffer(pos.pendingBuffer, GIN_SHARE);
	pos.firstOffset = FirstOffsetNumber;
	UnlockReleaseBuffer(metabuffer);
	pos.hasMatchKey = palloc(sizeof(bool) * so->nkeys);

	/*
	 * 对每个堆行循环。scanGetCandidate 返回整行或
	 * 第一页上的该行元组。
	 */
	while (scanGetCandidate(scan, &pos))
	{
		/*
		 * 检查元组中的条目并设置 entryRes 数组。
		 *
		 * 若属于当前堆行的挂起元组分散在
		 * 若干页上，collectMatchesForHeapRow 将读完
		 * 那些页。
		 */
		if (!collectMatchesForHeapRow(scan, &pos))
			continue;

		/*
		 * 一行条目的匹配已完成，因此用
		 * consistent 函数检查该行。
		 */
		oldCtx = MemoryContextSwitchTo(so->tempCtx);
		recheck = false;
		match = true;

		for (i = 0; i < so->nkeys; i++)
		{
			GinScanKey	key = so->keys + i;

			if (!key->boolConsistentFn(key))
			{
				match = false;
				break;
			}
			recheck |= key->recheckCurItem;
		}

		MemoryContextSwitchTo(oldCtx);
		MemoryContextReset(so->tempCtx);

		if (match)
		{
			tbm_add_tuples(tbm, &pos.item, 1, recheck);
			(*ntids)++;
		}
	}

	pfree(pos.hasMatchKey);
}


#define GinIsVoidRes(s)		( ((GinScanOpaque) scan->opaque)->isVoidRes )

int64
gingetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	int64		ntids;
	ItemPointerData iptr;
	bool		recheck;

	/*
	 * 设置扫描键，并检查不可满足的查询。
	 */
	ginFreeScanKeys(so);		/* 此时应还没有键，但为保险
								 * 起见 */
	ginNewScanKey(scan);

	if (GinIsVoidRes(scan))
		return 0;

	ntids = 0;

	/*
	 * 首先扫描挂起列表，把任何匹配条目收集进
	 * 位图。扫描某个挂起项之后，其他后端可能把它
	 * 发布进主索引，因此主扫描期间我们可能
	 * 再次访问它。这没问题，因为我们只会在位图中
	 * 再次设置同一位。（重复访问的可能性正是 GIN
	 * 无法支持 amgettuple API 的主要原因。）注意
	 * 不能先扫描主索引再扫描挂起列表，因为并发
	 * 清理可能使我们完全漏掉条目。
	 */
	scanPendingInsert(scan, tbm, &ntids);

	/*
	 * 现在扫描主索引。
	 */
	startScan(scan);

	ItemPointerSetMin(&iptr);

	for (;;)
	{
		if (!scanGetItem(scan, iptr, &iptr, &recheck))
			break;

		if (ItemPointerIsLossyPage(&iptr))
			tbm_add_page(tbm, ItemPointerGetBlockNumber(&iptr));
		else
			tbm_add_tuples(tbm, &iptr, 1, recheck);
		ntids++;
	}

	return ntids;
}
