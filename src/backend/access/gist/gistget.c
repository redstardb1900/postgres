/*-------------------------------------------------------------------------
 *
 * gistget.c
 *	  从 GiST 扫描中取回元组。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistget.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/relscan.h"
#include "lib/pairingheap.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/float.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * gistkillitems() -- 为索引扫描调用者告知已杀死的项设置
 * LP_DEAD 状态。
 *
 * 我们在此重新读页，因此检查页 LSN 很重要。若自上次读取以来
 * 页已被修改（由 LSN 判定），则无法
 * 标记任何条目，因为旧条目可能已被清理
 * 掉，且 TID 已被完全不同的堆元组重用。
 */
static void
gistkillitems(IndexScanDesc scan)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		iid;
	int			i;
	bool		killedsomething = false;

	Assert(so->curBlkno != InvalidBlockNumber);
	Assert(!XLogRecPtrIsInvalid(so->curPageLSN));
	Assert(so->killedItems != NULL);

	buffer = ReadBuffer(scan->indexRelation, so->curBlkno);
	if (!BufferIsValid(buffer))
		return;

	LockBuffer(buffer, GIST_SHARE);
	gistcheckpage(scan->indexRelation, buffer);
	page = BufferGetPage(buffer);

	/*
	 * 若页 LSN 不同，表示自上次读取以来页已被修改。
	 * killedItems 可能无效，因此应用 LP_DEAD 提示是不
	 * 安全的。
	 */
	if (BufferGetLSNAtomic(buffer) != so->curPageLSN)
	{
		UnlockReleaseBuffer(buffer);
		so->numKilled = 0;		/* 重置计数器 */
		return;
	}

	Assert(GistPageIsLeaf(page));

	/*
	 * 把全部 killedItems 标为死。我们不需要额外重检，因为
	 * 若页被修改，curPageLSN 必定已变。
	 */
	for (i = 0; i < so->numKilled; i++)
	{
		offnum = so->killedItems[i];
		iid = PageGetItemId(page, offnum);
		ItemIdMarkDead(iid);
		killedsomething = true;
	}

	if (killedsomething)
	{
		GistMarkPageHasGarbage(page);
		MarkBufferDirtyHint(buffer, true);
	}

	UnlockReleaseBuffer(buffer);

	/*
	 * 总是重置扫描状态，以免在其他页上查找相同
	 * 项。
	 */
	so->numKilled = 0;
}

/*
 * gistindex_keytest() -- 此索引元组是否满足扫描 key？
 *
 * 索引元组可能表示堆元组或下层索引页，
 * 取决于包含它的页是否为叶子页。
 *
 * 对堆元组成功返回时，设置 *recheck_p 以指示
 * 限定条件是否需要重检。若任一 consistent()
 * 函数要求，我们就重检。检查非叶子
 * 条目时 recheck 并不重要，因为若有任何疑虑就必须访问下层索引页。
 * 类似地，设置 *recheck_distances_p 以指示距离是否
 * 需要重检，对非叶子条目也忽略它。
 *
 * 若在做有序扫描，则在返回成功前用 distance()
 * 函数的距离数据填充 so->distances[]。
 *
 * 必须先解压 IndexTuple 中的键，再把它传给
 * sk_funcs（实际上是操作符类的 Consistent 或 Distance 方法）。
 *
 * 注意此函数总是在短生命周期内存上下文中调用，
 * 因此不必担心清理已分配内存，无论在这里
 * 还是在任何 Consistent 或 Distance 方法的实现中。
 */
static bool
gistindex_keytest(IndexScanDesc scan,
				  IndexTuple tuple,
				  Page page,
				  OffsetNumber offset,
				  bool *recheck_p,
				  bool *recheck_distances_p)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	GISTSTATE  *giststate = so->giststate;
	ScanKey		key = scan->keyData;
	int			keySize = scan->numberOfKeys;
	IndexOrderByDistance *distance_p;
	Relation	r = scan->indexRelation;

	*recheck_p = false;
	*recheck_distances_p = false;

	/*
	 * 若是 pre-9.1 遗留的无效元组，则把它当作匹配，并取
	 * 尽可能小的距离。这意味着我们总会跟随它到
	 * 所引用的页。
	 */
	if (GistTupleIsInvalid(tuple))
	{
		int			i;

		if (GistPageIsLeaf(page))	/* 不应发生 */
			elog(ERROR, "invalid GiST tuple found on leaf page");
		for (i = 0; i < scan->numberOfOrderBys; i++)
		{
			so->distances[i].value = -get_float8_infinity();
			so->distances[i].isnull = false;
		}
		return true;
	}

	/* 按 Consistent 函数检查是否匹配 */
	while (keySize > 0)
	{
		Datum		datum;
		bool		isNull;

		datum = index_getattr(tuple,
							  key->sk_attno,
							  giststate->leafTupdesc,
							  &isNull);

		if (key->sk_flags & SK_ISNULL)
		{
			/*
			 * 在非叶子页上，我们不能断定子节点没有 NULL
			 * 值，因为 GiST 假定：union (VAL, NULL) 是 VAL。
			 * 但若非叶子页上键 IS NULL，则所有子节点都是
			 * NULL。
			 */
			if (key->sk_flags & SK_SEARCHNULL)
			{
				if (GistPageIsLeaf(page) && !isNull)
					return false;
			}
			else
			{
				Assert(key->sk_flags & SK_SEARCHNOTNULL);
				if (isNull)
					return false;
			}
		}
		else if (isNull)
		{
			return false;
		}
		else
		{
			Datum		test;
			bool		recheck;
			GISTENTRY	de;

			gistdentryinit(giststate, key->sk_attno - 1, &de,
						   datum, r, page, offset,
						   false, isNull);

			/*
			 * 调用 Consistent 函数以求值测试。
			 * 参数是索引 datum（作为 GISTENTRY*）、比较
			 * datum、比较操作符的策略号与子类型，来自
			 * pg_amop，以及 recheck 标志。
			 *
			 * （目前无需传递 subtype，因为它
			 * 总是零，但为将来可能的使用
			 * 不妨仍传递。）
			 *
			 * 我们把 recheck 标志初始化为 true（最安全的假定），
			 * 以防 Consistent 函数忘记设置它。
			 */
			recheck = true;

			test = FunctionCall5Coll(&key->sk_func,
									 key->sk_collation,
									 PointerGetDatum(&de),
									 key->sk_argument,
									 Int16GetDatum(key->sk_strategy),
									 ObjectIdGetDatum(key->sk_subtype),
									 PointerGetDatum(&recheck));

			if (!DatumGetBool(test))
				return false;
			*recheck_p |= recheck;
		}

		key++;
		keySize--;
	}

	/* 通过了 --- 现在来计算距离 */
	key = scan->orderByData;
	distance_p = so->distances;
	keySize = scan->numberOfOrderBys;
	while (keySize > 0)
	{
		Datum		datum;
		bool		isNull;

		datum = index_getattr(tuple,
							  key->sk_attno,
							  giststate->leafTupdesc,
							  &isNull);

		if ((key->sk_flags & SK_ISNULL) || isNull)
		{
			/* 假定距离计算结果为空 */
			distance_p->value = 0.0;
			distance_p->isnull = true;
		}
		else
		{
			Datum		dist;
			bool		recheck;
			GISTENTRY	de;

			gistdentryinit(giststate, key->sk_attno - 1, &de,
						   datum, r, page, offset,
						   false, isNull);

			/*
			 * 调用 Distance 函数以计算距离。
			 * 参数是索引 datum（作为 GISTENTRY*）、比较
			 * datum、排序操作符的策略号与子类型，来自
			 * pg_amop，以及 recheck 标志。
			 *
			 * （目前无需传递 subtype，因为它
			 * 总是零，但为将来可能的使用
			 * 不妨仍传递。）
			 *
			 * 若函数设置了 recheck 标志，则返回的距离是
			 * 真实距离的下界，须重检。
			 * 我们把标志初始化为 'false'。此标志在
			 * 9.5 版加入；此前编写的距离函数不会知道
			 * 该标志，但预期永不有损。
			 */
			recheck = false;
			dist = FunctionCall5Coll(&key->sk_func,
									 key->sk_collation,
									 PointerGetDatum(&de),
									 key->sk_argument,
									 Int16GetDatum(key->sk_strategy),
									 ObjectIdGetDatum(key->sk_subtype),
									 PointerGetDatum(&recheck));
			*recheck_distances_p |= recheck;
			distance_p->value = DatumGetFloat8(dist);
			distance_p->isnull = false;
		}

		key++;
		distance_p++;
		keySize--;
	}

	return true;
}

/*
 * 扫描 *pageItem 所标识的 GiST 索引页上的全部项，并把它们
 * 插入队列（或直接写入输出区）
 *
 * scan: 我们正在执行的索引扫描
 * pageItem: 标识待扫描索引页的搜索队列项
 * myDistances: 与 pageItem 关联的距离数组，在根处为 NULL
 * tbm: 若非 NULL，则为 gistgetbitmap 的输出位图
 * ntids: 若非 NULL，则为 gistgetbitmap 的输出元组计数器
 *
 * 若 tbm/ntids 非 NULL，我们在做 amgetbitmap 扫描，堆
 * 元组应直接报告到位图中。若它们为 NULL，
 * 我们在做普通或有序 indexscan。对普通 indexscan，堆
 * 元组 TIDs 返回到 so->pageData[]。对有序 indexscan，
 * 堆元组 TIDs 被推入各个搜索队列项。在
 * 仅索引扫描中，重建的索引元组与
 * TIDs 一并返回。
 *
 * 若我们检测到自在父中看到其下行指针以来索引页已分裂，
 * 则把它的新右兄弟推入队列，以便
 * 接下来处理该兄弟。
 */
static void
gistScanPage(IndexScanDesc scan, GISTSearchItem *pageItem,
			 IndexOrderByDistance *myDistances, TIDBitmap *tbm, int64 *ntids)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	GISTSTATE  *giststate = so->giststate;
	Relation	r = scan->indexRelation;
	Buffer		buffer;
	Page		page;
	GISTPageOpaque opaque;
	OffsetNumber maxoff;
	OffsetNumber i;
	MemoryContext oldcxt;

	Assert(!GISTSearchItemIsHeap(*pageItem));

	buffer = ReadBuffer(scan->indexRelation, pageItem->blkno);
	LockBuffer(buffer, GIST_SHARE);
	PredicateLockPage(r, BufferGetBlockNumber(buffer), scan->xs_snapshot);
	gistcheckpage(scan->indexRelation, buffer);
	page = BufferGetPage(buffer);
	TestForOldSnapshot(scan->xs_snapshot, r, page);
	opaque = GistPageGetOpaque(page);

	/*
	 * 检查是否需要跟随右链接。若自访问父以来页被
	 * 并发分裂（此时 parentlsn < nsn），或系统在页分裂后
	 * 崩溃、但下行指针尚未插入父，则需要
	 * 跟随它。
	 */
	if (!XLogRecPtrIsInvalid(pageItem->data.parentlsn) &&
		(GistFollowRight(page) ||
		 pageItem->data.parentlsn < GistPageGetNSN(page)) &&
		opaque->rightlink != InvalidBlockNumber /* 健全性检查 */ )
	{
		/* 发生了页分裂，跟随右链接以添加页 */
		GISTSearchItem *item;

		/* 从根开始时不可能发生 */
		Assert(myDistances != NULL);

		oldcxt = MemoryContextSwitchTo(so->queueCxt);

		/* 为右兄弟索引页创建新的 GISTSearchItem */
		item = palloc(SizeOfGISTSearchItem(scan->numberOfOrderBys));
		item->blkno = opaque->rightlink;
		item->data.parentlsn = pageItem->data.parentlsn;

		/* 用与本页相同的距离把它插入队列 */
		memcpy(item->distances, myDistances,
			   sizeof(item->distances[0]) * scan->numberOfOrderBys);

		pairingheap_add(so->queue, &item->phNode);

		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * 检查我们看到下行指针之后页是否已被删除。已删除页上
	 * 没有感兴趣的内容。注意必须在检查并发分裂的
	 * NSN 之后做这件事！有可能该页
	 * 原先包含一些对我们可见的元组，但被分裂
	 * 使得全部可见元组移到另一页，然后
	 * 本页被删除。
	 */
	if (GistPageIsDeleted(page))
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	so->nPageData = so->curPageData = 0;
	scan->xs_hitup = NULL;		/* 可能指向 pageDataCxt 内 */
	if (so->pageDataCxt)
		MemoryContextReset(so->pageDataCxt);

	/*
	 * 我们在读取时保存页的 LSN，以便知道稍后是否
	 * 可安全对该页应用 LP_DEAD 提示。这让我们能为 MVCC
	 * 扫描放下 pin，从而使清理避免阻塞。
	 */
	so->curPageLSN = BufferGetLSNAtomic(buffer);

	/*
	 * 检查页上全部元组
	 */
	maxoff = PageGetMaxOffsetNumber(page);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		ItemId		iid = PageGetItemId(page, i);
		IndexTuple	it;
		bool		match;
		bool		recheck;
		bool		recheck_distances;

		/*
		 * 若扫描指定不返回已杀死的元组，则我们把
		 * 已杀死元组当作未通过限定条件。
		 */
		if (scan->ignore_killed_tuples && ItemIdIsDead(iid))
			continue;

		it = (IndexTuple) PageGetItem(page, iid);

		/*
		 * 必须在 tempCxt 中调用 gistindex_keytest，并随后清理任何
		 * 残留垃圾。
		 */
		oldcxt = MemoryContextSwitchTo(so->giststate->tempCxt);

		match = gistindex_keytest(scan, it, page, i,
								  &recheck, &recheck_distances);

		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(so->giststate->tempCxt);

		/* 若不匹配则忽略元组 */
		if (!match)
			continue;

		if (tbm && GistPageIsLeaf(page))
		{
			/*
			 * getbitmap 扫描，因此只需把堆元组 TIDs 推进位图，
			 * 不必担心顺序
			 */
			tbm_add_tuples(tbm, &it->t_tid, 1, recheck);
			(*ntids)++;
		}
		else if (scan->numberOfOrderBys == 0 && GistPageIsLeaf(page))
		{
			/*
			 * 非有序扫描，因此在 so->pageData[] 中报告元组
			 */
			so->pageData[so->nPageData].heapPtr = it->t_tid;
			so->pageData[so->nPageData].recheck = recheck;
			so->pageData[so->nPageData].offnum = i;

			/*
			 * 在仅索引扫描中，还从元组取回数据。
			 * 重建的元组存放在 pageDataCxt 中。
			 */
			if (scan->xs_want_itup)
			{
				oldcxt = MemoryContextSwitchTo(so->pageDataCxt);
				so->pageData[so->nPageData].recontup =
					gistFetchTuple(giststate, r, it);
				MemoryContextSwitchTo(oldcxt);
			}
			so->nPageData++;
		}
		else
		{
			/*
			 * 必须把项推入搜索队列。对任何下层
			 * 索引页都会到这里，若在做有序搜索，对堆元组
			 * 也会到这里。
			 */
			GISTSearchItem *item;
			int			nOrderBys = scan->numberOfOrderBys;

			oldcxt = MemoryContextSwitchTo(so->queueCxt);

			/* 为本项创建新的 GISTSearchItem */
			item = palloc(SizeOfGISTSearchItem(scan->numberOfOrderBys));

			if (GistPageIsLeaf(page))
			{
				/* 创建堆元组 GISTSearchItem */
				item->blkno = InvalidBlockNumber;
				item->data.heap.heapPtr = it->t_tid;
				item->data.heap.recheck = recheck;
				item->data.heap.recheckDistances = recheck_distances;

				/*
				 * 在仅索引扫描中，还从元组取回数据。
				 */
				if (scan->xs_want_itup)
					item->data.heap.recontup = gistFetchTuple(giststate, r, it);
			}
			else
			{
				/* 创建索引页 GISTSearchItem */
				item->blkno = ItemPointerGetBlockNumber(&it->t_tid);

				/*
				 * 当前页的 LSN 是子节点的父页 lsn。我们
				 * 只有共享锁，因此需要原子地
				 * 取得 LSN。
				 */
				item->data.parentlsn = BufferGetLSNAtomic(buffer);
			}

			/* 用新的距离数据把它插入队列 */
			memcpy(item->distances, so->distances,
				   sizeof(item->distances[0]) * nOrderBys);

			pairingheap_add(so->queue, &item->phNode);

			MemoryContextSwitchTo(oldcxt);
		}
	}

	UnlockReleaseBuffer(buffer);
}

/*
 * 按顺序从搜索队列取出下一项
 *
 * 返回 GISTSearchItem 或 NULL。用完后调用者必须 pfree 该项。
 */
static GISTSearchItem *
getNextGISTSearchItem(GISTScanOpaque so)
{
	GISTSearchItem *item;

	if (!pairingheap_is_empty(so->queue))
	{
		item = (GISTSearchItem *) pairingheap_remove_first(so->queue);
	}
	else
	{
		/* 两个堆都空时完成 */
		item = NULL;
	}

	/* 返回项；调用者负责 pfree 它 */
	return item;
}

/*
 * 在有序搜索中取回下一个堆元组
 */
static bool
getNextNearest(IndexScanDesc scan)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	bool		res = false;

	if (scan->xs_hitup)
	{
		/* 释放先前返回的元组 */
		pfree(scan->xs_hitup);
		scan->xs_hitup = NULL;
	}

	do
	{
		GISTSearchItem *item = getNextGISTSearchItem(so);

		if (!item)
			break;

		if (GISTSearchItemIsHeap(*item))
		{
			/* 在当前最小距离处找到堆项 */
			scan->xs_heaptid = item->data.heap.heapPtr;
			scan->xs_recheck = item->data.heap.recheck;

			index_store_float8_orderby_distances(scan, so->orderByTypes,
												 item->distances,
												 item->data.heap.recheckDistances);

			/* 在仅索引扫描中，还返回重建的元组。 */
			if (scan->xs_want_itup)
				scan->xs_hitup = item->data.heap.recontup;
			res = true;
		}
		else
		{
			/* 访问索引页，把它的项提取入队列 */
			CHECK_FOR_INTERRUPTS();

			gistScanPage(scan, item, item->distances, NULL, NULL);
		}

		pfree(item);
	} while (!res);

	return res;
}

/*
 * gistgettuple() -- 获取扫描中的下一个元组
 */
bool
gistgettuple(IndexScanDesc scan, ScanDirection dir)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;

	if (dir != ForwardScanDirection)
		elog(ERROR, "GiST only supports forward scan direction");

	if (!so->qual_ok)
		return false;

	if (so->firstCall)
	{
		/* 通过处理根页开始扫描 */
		GISTSearchItem fakeItem;

		pgstat_count_index_scan(scan->indexRelation);

		so->firstCall = false;
		so->curPageData = so->nPageData = 0;
		scan->xs_hitup = NULL;
		if (so->pageDataCxt)
			MemoryContextReset(so->pageDataCxt);

		fakeItem.blkno = GIST_ROOT_BLKNO;
		memset(&fakeItem.data.parentlsn, 0, sizeof(GistNSN));
		gistScanPage(scan, &fakeItem, NULL, NULL, NULL);
	}

	if (scan->numberOfOrderBys > 0)
	{
		/* 必须按严格距离顺序取回元组 */
		return getNextNearest(scan);
	}
	else
	{
		/* 一次一索引页地取回元组 */
		for (;;)
		{
			if (so->curPageData < so->nPageData)
			{
				if (scan->kill_prior_tuple && so->curPageData > 0)
				{

					if (so->killedItems == NULL)
					{
						MemoryContext oldCxt =
							MemoryContextSwitchTo(so->giststate->scanCxt);

						so->killedItems =
							(OffsetNumber *) palloc(MaxIndexTuplesPerPage
													* sizeof(OffsetNumber));

						MemoryContextSwitchTo(oldCxt);
					}
					if (so->numKilled < MaxIndexTuplesPerPage)
						so->killedItems[so->numKilled++] =
							so->pageData[so->curPageData - 1].offnum;
				}
				/* 继续从叶子页返回元组 */
				scan->xs_heaptid = so->pageData[so->curPageData].heapPtr;
				scan->xs_recheck = so->pageData[so->curPageData].recheck;

				/* 在仅索引扫描中，还返回重建的元组 */
				if (scan->xs_want_itup)
					scan->xs_hitup = so->pageData[so->curPageData].recontup;

				so->curPageData++;

				return true;
			}

			/*
			 * 检查上次返回的元组，必要时把它加入
			 * killedItems
			 */
			if (scan->kill_prior_tuple
				&& so->curPageData > 0
				&& so->curPageData == so->nPageData)
			{

				if (so->killedItems == NULL)
				{
					MemoryContext oldCxt =
						MemoryContextSwitchTo(so->giststate->scanCxt);

					so->killedItems =
						(OffsetNumber *) palloc(MaxIndexTuplesPerPage
												* sizeof(OffsetNumber));

					MemoryContextSwitchTo(oldCxt);
				}
				if (so->numKilled < MaxIndexTuplesPerPage)
					so->killedItems[so->numKilled++] =
						so->pageData[so->curPageData - 1].offnum;
			}
			/* 查找并处理下一个索引页 */
			do
			{
				GISTSearchItem *item;

				if ((so->curBlkno != InvalidBlockNumber) && (so->numKilled > 0))
					gistkillitems(scan);

				item = getNextGISTSearchItem(so);

				if (!item)
					return false;

				CHECK_FOR_INTERRUPTS();

				/* 保存当前项的 BlockNumber 供下次 gistkillitems() 调用 */
				so->curBlkno = item->blkno;

				/*
				 * 扫描叶子页时，匹配堆元组的 ItemPointers
				 * 存放在 so->pageData 中。若本页上有，
				 * 我们就退出内层 "do" 并绕回循环
				 * 以返回它们。
				 */
				gistScanPage(scan, item, item->distances, NULL, NULL);

				pfree(item);
			} while (so->nPageData == 0);
		}
	}
}

/*
 * gistgetbitmap() -- 获取全部堆元组位置的位图
 */
int64
gistgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	int64		ntids = 0;
	GISTSearchItem fakeItem;

	if (!so->qual_ok)
		return 0;

	pgstat_count_index_scan(scan->indexRelation);

	/* 通过处理根页开始扫描 */
	so->curPageData = so->nPageData = 0;
	scan->xs_hitup = NULL;
	if (so->pageDataCxt)
		MemoryContextReset(so->pageDataCxt);

	fakeItem.blkno = GIST_ROOT_BLKNO;
	memset(&fakeItem.data.parentlsn, 0, sizeof(GistNSN));
	gistScanPage(scan, &fakeItem, NULL, tbm, &ntids);

	/*
	 * 扫描叶子页时，匹配堆元组的 ItemPointers 将
	 * 直接存入 tbm，因此这里不必处理它们。
	 */
	for (;;)
	{
		GISTSearchItem *item = getNextGISTSearchItem(so);

		if (!item)
			break;

		CHECK_FOR_INTERRUPTS();

		gistScanPage(scan, item, item->distances, tbm, &ntids);

		pfree(item);
	}

	return ntids;
}

/*
 * 能否对给定索引列做仅索引扫描？
 *
 * 实现了 fetch 函数的操作符类支持仅索引扫描。
 * 没有压缩函数的操作符类也支持仅索引扫描。
 * 包含属性总能被取回用于仅索引扫描。
 */
bool
gistcanreturn(Relation index, int attno)
{
	if (attno > IndexRelationGetNumberOfKeyAttributes(index) ||
		OidIsValid(index_getprocid(index, attno, GIST_FETCH_PROC)) ||
		!OidIsValid(index_getprocid(index, attno, GIST_COMPRESS_PROC)))
		return true;
	else
		return false;
}
