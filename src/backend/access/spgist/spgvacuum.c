/*-------------------------------------------------------------------------
 *
 * spgvacuum.c
 *	  SP-GiST 的清理例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgvacuum.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/spgist_private.h"
#include "access/spgxlog.h"
#include "access/transam.h"
#include "access/xloginsert.h"
#include "catalog/storage_xlog.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/snapmgr.h"


/* 待重访 TIDs 挂起列表中的项 */
typedef struct spgVacPendingItem
{
	ItemPointerData tid;		/* 要访问的重定向目标 */
	bool		done;			/* 是否已处理过？ */
	struct spgVacPendingItem *next; /* 链表指针 */
} spgVacPendingItem;

/* 清理操作的本地状态 */
typedef struct spgBulkDeleteState
{
	/* 传入 spgvacuumscan 的参数 */
	IndexVacuumInfo *info;
	IndexBulkDeleteResult *stats;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;

	/* 额外工作状态 */
	SpGistState spgstate;		/* 供需要该状态的 SPGiST 操作使用 */
	spgVacPendingItem *pendingList; /* 需要（重新）访问的 TIDs */
	TransactionId myXmin;		/* 用于检测新加入的重定向 */
	BlockNumber lastFilledBlock;	/* 最后一个不可删除的块 */
} spgBulkDeleteState;


/*
 * 将 TID 加入 pendingList，但仅当尚未存在时。
 *
 * 注意新项总是追加在列表末尾；这确保对列表的扫描
 * 不会漏掉扫描过程中新加入的项。
 */
static void
spgAddPendingTID(spgBulkDeleteState *bds, ItemPointer tid)
{
	spgVacPendingItem *pitem;
	spgVacPendingItem **listLink;

	/* 在列表中查找是否已有该项 */
	listLink = &bds->pendingList;
	while (*listLink != NULL)
	{
		pitem = *listLink;
		if (ItemPointerEquals(tid, &pitem->tid))
			return;				/* 已在列表中，无需操作 */
		listLink = &pitem->next;
	}
	/* 不在列表中，追加新项 */
	pitem = (spgVacPendingItem *) palloc(sizeof(spgVacPendingItem));
	pitem->tid = *tid;
	pitem->done = false;
	pitem->next = NULL;
	*listLink = pitem;
}

/*
 * 清空 pendingList
 */
static void
spgClearPendingList(spgBulkDeleteState *bds)
{
	spgVacPendingItem *pitem;
	spgVacPendingItem *nitem;

	for (pitem = bds->pendingList; pitem != NULL; pitem = nitem)
	{
		nitem = pitem->next;
		/* 列表中所有项都应已被处理 */
		Assert(pitem->done);
		pfree(pitem);
	}
	bds->pendingList = NULL;
}

/*
 * 清理普通（非根）叶子页
 *
 * 我们必须删除 VACUUM 指定要删除的元组，
 * 但不得移动任何被外部链接引用的元组；我们假定
 * 那些就是链的头。
 *
 * 若发现由并发事务创建的 REDIRECT，
 * 必须把它的目标 TID 加入 pendingList。（我们不立即访问
 * 目标，一是因为不想让 VACUUM 同时锁定多于
 * 一个缓冲区，二是因为 spgAddPendingTID 中的去重
 * 逻辑有助于确保在持续并发插入时不会陷入
 * 无限循环。）
 *
 * 若 forPending 为真，则我们是因追踪重定向链接而
 * 检查该页，而非正常顺序扫描的一部分。
 * 我们仍正常清理该页，但不递增关于存活
 * 元组的统计；否则会重复计数，因为该页
 * 在顺序扫描中已经或将会被访问。
 */
static void
vacuumLeafPage(spgBulkDeleteState *bds, Relation index, Buffer buffer,
			   bool forPending)
{
	Page		page = BufferGetPage(buffer);
	spgxlogVacuumLeaf xlrec;
	OffsetNumber toDead[MaxIndexTuplesPerPage];
	OffsetNumber toPlaceholder[MaxIndexTuplesPerPage];
	OffsetNumber moveSrc[MaxIndexTuplesPerPage];
	OffsetNumber moveDest[MaxIndexTuplesPerPage];
	OffsetNumber chainSrc[MaxIndexTuplesPerPage];
	OffsetNumber chainDest[MaxIndexTuplesPerPage];
	OffsetNumber predecessor[MaxIndexTuplesPerPage + 1];
	bool		deletable[MaxIndexTuplesPerPage + 1];
	int			nDeletable;
	OffsetNumber i,
				max = PageGetMaxOffsetNumber(page);

	memset(predecessor, 0, sizeof(predecessor));
	memset(deletable, 0, sizeof(deletable));
	nDeletable = 0;

	/* 扫描页，识别待删除元组，累计统计 */
	for (i = FirstOffsetNumber; i <= max; i++)
	{
		SpGistLeafTuple lt;

		lt = (SpGistLeafTuple) PageGetItem(page,
										   PageGetItemId(page, i));
		if (lt->tupstate == SPGIST_LIVE)
		{
			Assert(ItemPointerIsValid(&lt->heapPtr));

			if (bds->callback(&lt->heapPtr, bds->callback_state))
			{
				bds->stats->tuples_removed += 1;
				deletable[i] = true;
				nDeletable++;
			}
			else
			{
				if (!forPending)
					bds->stats->num_index_tuples += 1;
			}

			/* 同时构建前驱映射 */
			if (SGLT_GET_NEXTOFFSET(lt) != InvalidOffsetNumber)
			{
				/* 对损坏的链链接保持警惕 */
				if (SGLT_GET_NEXTOFFSET(lt) < FirstOffsetNumber ||
					SGLT_GET_NEXTOFFSET(lt) > max ||
					predecessor[SGLT_GET_NEXTOFFSET(lt)] != InvalidOffsetNumber)
					elog(ERROR, "inconsistent tuple chain links in page %u of index \"%s\"",
						 BufferGetBlockNumber(buffer),
						 RelationGetRelationName(index));
				predecessor[SGLT_GET_NEXTOFFSET(lt)] = i;
			}
		}
		else if (lt->tupstate == SPGIST_REDIRECT)
		{
			SpGistDeadTuple dt = (SpGistDeadTuple) lt;

			Assert(SGLT_GET_NEXTOFFSET(dt) == InvalidOffsetNumber);
			Assert(ItemPointerIsValid(&dt->pointer));

			/*
			 * 若该重定向可能发生在 VACUUM 开始之后，则把目标
			 * TID 加入挂起列表。（若 xid 无效，则假定它一定
			 * 发生在 VACUUM 开始之前，因为 REINDEX
			 * CONCURRENTLY 会排除 VACUUM。）
			 *
			 * 注意：我们本可通过检查 xid 在活动快照下是否仍
			 * "运行中"来做更严格的测试；但 snapmgr.c
			 * 目前未导出合适的 API，而且更严格的测试是否
			 * 值得那些额外周期也并不完全清楚。
			 */
			if (TransactionIdFollowsOrEquals(dt->xid, bds->myXmin))
				spgAddPendingTID(bds, &dt->pointer);
		}
		else
		{
			Assert(SGLT_GET_NEXTOFFSET(lt) == InvalidOffsetNumber);
		}
	}

	if (nDeletable == 0)
		return;					/* 没有更多事可做 */

	/*----------
	 * 弄清我们究竟要做什么。我们把这一步与实际
	 * 修改页分开，主要是为了得到一种可写入 WAL
	 * 的表示，然后重放代码就能做完全相同的事。
	 * 本步的输出由六个数组组成，描述四类操作，
	 * 按以下顺序执行：
	 *
	 * toDead[]: 要用 DEAD 元组替换的元组编号
	 * toPlaceholder[]: 要用 PLACEHOLDER 元组替换的元组编号
	 * moveSrc[]: 需要搬到另一偏移的元组编号
	 * （替换那里的元组）然后再用 PLACEHOLDER 元组替换
	 * moveDest[]: moveSrc 元组的新位置
	 * chainSrc[]: 链链接（nextOffset）需要更新的元组编号
	 * chainDest[]: chainSrc 成员的 nextOffset 新值
	 *
	 * 最容易的做法是按元组链来弄清要做什么，
	 * 因此我们遍历所有元组（不只是可删除的那些！）
	 * 以识别链头，再沿每条链向下追查，并为链内
	 * 可删除元组生成工作项。
	 *----------
	 */
	xlrec.nDead = xlrec.nPlaceholder = xlrec.nMove = xlrec.nChain = 0;

	for (i = FirstOffsetNumber; i <= max; i++)
	{
		SpGistLeafTuple head;
		bool		interveningDeletable;
		OffsetNumber prevLive;
		OffsetNumber j;

		head = (SpGistLeafTuple) PageGetItem(page,
											 PageGetItemId(page, i));
		if (head->tupstate != SPGIST_LIVE)
			continue;			/* 不可能是链成员 */
		if (predecessor[i] != 0)
			continue;			/* 不是链头 */

		/* 初始化…… */
		interveningDeletable = false;
		prevLive = deletable[i] ? InvalidOffsetNumber : i;

		/* 沿链向下扫描…… */
		j = SGLT_GET_NEXTOFFSET(head);
		while (j != InvalidOffsetNumber)
		{
			SpGistLeafTuple lt;

			lt = (SpGistLeafTuple) PageGetItem(page,
											   PageGetItemId(page, j));
			if (lt->tupstate != SPGIST_LIVE)
			{
				/* 链中所有元组都应为存活 */
				elog(ERROR, "unexpected SPGiST tuple state: %d",
					 lt->tupstate);
			}

			if (deletable[j])
			{
				/* 该元组应由占位元组替换 */
				toPlaceholder[xlrec.nPlaceholder] = j;
				xlrec.nPlaceholder++;
				/* 前一个存活元组的链链接将需要更新 */
				interveningDeletable = true;
			}
			else if (prevLive == InvalidOffsetNumber)
			{
				/*
				 * 这是链中第一个存活元组。它必须移到
				 * 头位置。
				 */
				moveSrc[xlrec.nMove] = j;
				moveDest[xlrec.nMove] = i;
				xlrec.nMove++;
				/* 链更新将在移动之后应用 */
				prevLive = i;
				interveningDeletable = false;
			}
			else
			{
				/*
				 * 第二个或更靠后的存活元组。若中间有空隙，
				 * 安排把它重新链接到前一个存活元组。
				 */
				if (interveningDeletable)
				{
					chainSrc[xlrec.nChain] = prevLive;
					chainDest[xlrec.nChain] = j;
					xlrec.nChain++;
				}
				prevLive = j;
				interveningDeletable = false;
			}

			j = SGLT_GET_NEXTOFFSET(lt);
		}

		if (prevLive == InvalidOffsetNumber)
		{
			/* 整条链都可移除，因此需要一个 DEAD 元组 */
			toDead[xlrec.nDead] = i;
			xlrec.nDead++;
		}
		else if (interveningDeletable)
		{
			/* 链末有一处或多处删除，因此把它封上 */
			chainSrc[xlrec.nChain] = prevLive;
			chainDest[xlrec.nChain] = InvalidOffsetNumber;
			xlrec.nChain++;
		}
	}

	/* 完整性检查…… */
	if (nDeletable != xlrec.nDead + xlrec.nPlaceholder + xlrec.nMove)
		elog(ERROR, "inconsistent counts of deletable tuples");

	/* 执行更新 */
	START_CRIT_SECTION();

	spgPageIndexMultiDelete(&bds->spgstate, page,
							toDead, xlrec.nDead,
							SPGIST_DEAD, SPGIST_DEAD,
							InvalidBlockNumber, InvalidOffsetNumber);

	spgPageIndexMultiDelete(&bds->spgstate, page,
							toPlaceholder, xlrec.nPlaceholder,
							SPGIST_PLACEHOLDER, SPGIST_PLACEHOLDER,
							InvalidBlockNumber, InvalidOffsetNumber);

	/*
	 * 我们通过交换源与目标元组的行指针来实现
	 * 移动步骤，然后把新成为源的元组替换为
	 * 占位元组。这也许过分依赖页数据表示的
	 * 细节，但很快，并且在待搬迁元组很大时
	 * 不会有页溢出风险。
	 */
	for (i = 0; i < xlrec.nMove; i++)
	{
		ItemId		idSrc = PageGetItemId(page, moveSrc[i]);
		ItemId		idDest = PageGetItemId(page, moveDest[i]);
		ItemIdData	tmp;

		tmp = *idSrc;
		*idSrc = *idDest;
		*idDest = tmp;
	}

	spgPageIndexMultiDelete(&bds->spgstate, page,
							moveSrc, xlrec.nMove,
							SPGIST_PLACEHOLDER, SPGIST_PLACEHOLDER,
							InvalidBlockNumber, InvalidOffsetNumber);

	for (i = 0; i < xlrec.nChain; i++)
	{
		SpGistLeafTuple lt;

		lt = (SpGistLeafTuple) PageGetItem(page,
										   PageGetItemId(page, chainSrc[i]));
		Assert(lt->tupstate == SPGIST_LIVE);
		SGLT_SET_NEXTOFFSET(lt, chainDest[i]);
	}

	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(index))
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();

		STORE_STATE(&bds->spgstate, xlrec.stateSrc);

		XLogRegisterData((char *) &xlrec, SizeOfSpgxlogVacuumLeaf);
		/* sizeof(xlrec) 应为 sizeof(OffsetNumber) 的整数倍 */
		XLogRegisterData((char *) toDead, sizeof(OffsetNumber) * xlrec.nDead);
		XLogRegisterData((char *) toPlaceholder, sizeof(OffsetNumber) * xlrec.nPlaceholder);
		XLogRegisterData((char *) moveSrc, sizeof(OffsetNumber) * xlrec.nMove);
		XLogRegisterData((char *) moveDest, sizeof(OffsetNumber) * xlrec.nMove);
		XLogRegisterData((char *) chainSrc, sizeof(OffsetNumber) * xlrec.nChain);
		XLogRegisterData((char *) chainDest, sizeof(OffsetNumber) * xlrec.nChain);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_VACUUM_LEAF);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();
}

/*
 * 当根页本身也是叶子时清理根页
 *
 * 在根上，我们只删除任何已死的叶子元组；不做花哨处理
 */
static void
vacuumLeafRoot(spgBulkDeleteState *bds, Relation index, Buffer buffer)
{
	Page		page = BufferGetPage(buffer);
	spgxlogVacuumRoot xlrec;
	OffsetNumber toDelete[MaxIndexTuplesPerPage];
	OffsetNumber i,
				max = PageGetMaxOffsetNumber(page);

	xlrec.nDelete = 0;

	/* 扫描页，识别待删除元组，累计统计 */
	for (i = FirstOffsetNumber; i <= max; i++)
	{
		SpGistLeafTuple lt;

		lt = (SpGistLeafTuple) PageGetItem(page,
										   PageGetItemId(page, i));
		if (lt->tupstate == SPGIST_LIVE)
		{
			Assert(ItemPointerIsValid(&lt->heapPtr));

			if (bds->callback(&lt->heapPtr, bds->callback_state))
			{
				bds->stats->tuples_removed += 1;
				toDelete[xlrec.nDelete] = i;
				xlrec.nDelete++;
			}
			else
			{
				bds->stats->num_index_tuples += 1;
			}
		}
		else
		{
			/* 根上所有元组都应为存活 */
			elog(ERROR, "unexpected SPGiST tuple state: %d",
				 lt->tupstate);
		}
	}

	if (xlrec.nDelete == 0)
		return;					/* 没有更多事可做 */

	/* 执行更新 */
	START_CRIT_SECTION();

	/* 元组编号有序，因此可用 PageIndexMultiDelete */
	PageIndexMultiDelete(page, toDelete, xlrec.nDelete);

	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(index))
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();

		/* 准备 WAL 记录 */
		STORE_STATE(&bds->spgstate, xlrec.stateSrc);

		XLogRegisterData((char *) &xlrec, SizeOfSpgxlogVacuumRoot);
		/* sizeof(xlrec) 应为 sizeof(OffsetNumber) 的整数倍 */
		XLogRegisterData((char *) toDelete,
						 sizeof(OffsetNumber) * xlrec.nDelete);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_VACUUM_ROOT);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();
}

/*
 * 清理给定页上的重定向与占位元组
 *
 * 重定向元组足够旧之后可标为占位。
 * 若不改变非占位元组的偏移，则可移除
 * 占位元组。
 *
 * 与上面的例程不同，这对叶子页和内部页都适用。
 */
static void
vacuumRedirectAndPlaceholder(Relation index, Relation heaprel, Buffer buffer)
{
	Page		page = BufferGetPage(buffer);
	SpGistPageOpaque opaque = SpGistPageGetOpaque(page);
	OffsetNumber i,
				max = PageGetMaxOffsetNumber(page),
				firstPlaceholder = InvalidOffsetNumber;
	bool		hasNonPlaceholder = false;
	bool		hasUpdate = false;
	OffsetNumber itemToPlaceholder[MaxIndexTuplesPerPage];
	OffsetNumber itemnos[MaxIndexTuplesPerPage];
	spgxlogVacuumRedirect xlrec;
	GlobalVisState *vistest;

	xlrec.isCatalogRel = RelationIsAccessibleInLogicalDecoding(heaprel);
	xlrec.nToPlaceholder = 0;
	xlrec.snapshotConflictHorizon = InvalidTransactionId;

	vistest = GlobalVisTestFor(heaprel);

	START_CRIT_SECTION();

	/*
	 * 向后扫描，把旧的重定向元组转换为占位元组，
	 * 并同时确定最后一个非占位元组的位置。
	 */
	for (i = max;
		 i >= FirstOffsetNumber &&
		 (opaque->nRedirection > 0 || !hasNonPlaceholder);
		 i--)
	{
		SpGistDeadTuple dt;

		dt = (SpGistDeadTuple) PageGetItem(page, PageGetItemId(page, i));

		/*
		 * 若不再可能有索引扫描仍 "in flight" 指向某个 REDIRECT，
		 * 我们就可把它转为 PLACEHOLDER。这样的索引扫描必须处于
		 * 其快照仍视该 REDIRECT 的 XID 为运行中的事务中，因此
		 * 拿 XID 与全局 xmin 比较是保守而安全的测试。若 XID
		 * 无效，则它一定是由 REINDEX CONCURRENTLY 插入的，
		 * 因此我们可以立即清除它。
		 */
		if (dt->tupstate == SPGIST_REDIRECT &&
			(!TransactionIdIsValid(dt->xid) ||
			 GlobalVisTestIsRemovableXid(vistest, dt->xid)))
		{
			dt->tupstate = SPGIST_PLACEHOLDER;
			Assert(opaque->nRedirection > 0);
			opaque->nRedirection--;
			opaque->nPlaceholder++;

			/* 记住被移除重定向中最新的 XID */
			if (!TransactionIdIsValid(xlrec.snapshotConflictHorizon) ||
				TransactionIdPrecedes(xlrec.snapshotConflictHorizon, dt->xid))
				xlrec.snapshotConflictHorizon = dt->xid;

			ItemPointerSetInvalid(&dt->pointer);

			itemToPlaceholder[xlrec.nToPlaceholder] = i;
			xlrec.nToPlaceholder++;

			hasUpdate = true;
		}

		if (dt->tupstate == SPGIST_PLACEHOLDER)
		{
			if (!hasNonPlaceholder)
				firstPlaceholder = i;
		}
		else
		{
			hasNonPlaceholder = true;
		}
	}

	/*
	 * 页末的任何占位元组都可安全移除。不过，我们不能
	 * 移除最后一个非占位元组之前的那些，因为我们不能
	 * 改变非占位元组的偏移号。
	 */
	if (firstPlaceholder != InvalidOffsetNumber)
	{
		/*
		 * 我们不把此数组存入 rdata，因为它很容易重建。
		 */
		for (i = firstPlaceholder; i <= max; i++)
			itemnos[i - firstPlaceholder] = i;

		i = max - firstPlaceholder + 1;
		Assert(opaque->nPlaceholder >= i);
		opaque->nPlaceholder -= i;

		/* 该数组肯定已排序，因此可用 PageIndexMultiDelete */
		PageIndexMultiDelete(page, itemnos, i);

		hasUpdate = true;
	}

	xlrec.firstPlaceholder = firstPlaceholder;

	if (hasUpdate)
		MarkBufferDirty(buffer);

	if (hasUpdate && RelationNeedsWAL(index))
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();

		XLogRegisterData((char *) &xlrec, SizeOfSpgxlogVacuumRedirect);
		XLogRegisterData((char *) itemToPlaceholder,
						 sizeof(OffsetNumber) * xlrec.nToPlaceholder);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_VACUUM_REDIRECT);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();
}

/*
 * 在 bulkdelete 扫描期间处理一页
 */
static void
spgvacuumpage(spgBulkDeleteState *bds, BlockNumber blkno)
{
	Relation	index = bds->info->index;
	Buffer		buffer;
	Page		page;

	/* 在未持有任何缓冲区锁时调用 vacuum_delay_point */
	vacuum_delay_point();

	buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
								RBM_NORMAL, bds->info->strategy);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = (Page) BufferGetPage(buffer);

	if (PageIsNew(page))
	{
		/*
		 * 发现全零页，这可能发生在数据库刚扩展文件后
		 * 崩溃时。回收它。
		 */
	}
	else if (PageIsEmpty(page))
	{
		/* 无事可做 */
	}
	else if (SpGistPageIsLeaf(page))
	{
		if (SpGistBlockIsRoot(blkno))
		{
			vacuumLeafRoot(bds, index, buffer);
			/* 无需调用 vacuumRedirectAndPlaceholder */
		}
		else
		{
			vacuumLeafPage(bds, index, buffer, false);
			vacuumRedirectAndPlaceholder(index, bds->info->heaprel, buffer);
		}
	}
	else
	{
		/* 内部页 */
		vacuumRedirectAndPlaceholder(index, bds->info->heaprel, buffer);
	}

	/*
	 * 根页绝不能被删除，也不能在 FSM 中标为可用，
	 * 因为我们不希望它们被 "寻找放置新元组位置" 的
	 * 搜索返回。否则，检查空页，并确保 FSM
	 * 知道它。
	 */
	if (!SpGistBlockIsRoot(blkno))
	{
		if (PageIsNew(page) || PageIsEmpty(page))
		{
			RecordFreeIndexPage(index, blkno);
			bds->stats->pages_deleted++;
		}
		else
		{
			SpGistSetLastUsedPage(index, buffer);
			bds->lastFilledBlock = blkno;
		}
	}

	UnlockReleaseBuffer(buffer);
}

/*
 * 在主扫描的各页之间处理挂起 TID 列表
 */
static void
spgprocesspending(spgBulkDeleteState *bds)
{
	Relation	index = bds->info->index;
	spgVacPendingItem *pitem;
	spgVacPendingItem *nitem;
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;

	for (pitem = bds->pendingList; pitem != NULL; pitem = pitem->next)
	{
		if (pitem->done)
			continue;			/* 忽略已完成的项 */

		/* 在未持有任何缓冲区锁时调用 vacuum_delay_point */
		vacuum_delay_point();

		/* 检查被引用的页 */
		blkno = ItemPointerGetBlockNumber(&pitem->tid);
		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, bds->info->strategy);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = (Page) BufferGetPage(buffer);

		if (PageIsNew(page) || SpGistPageIsDeleted(page))
		{
			/* 大概不应发生，但忽略它 */
		}
		else if (SpGistPageIsLeaf(page))
		{
			if (SpGistBlockIsRoot(blkno))
			{
				/* 这绝对不应发生 */
				elog(ERROR, "redirection leads to root page of index \"%s\"",
					 RelationGetRelationName(index));
			}

			/* 处理任何可删除的元组 */
			vacuumLeafPage(bds, index, buffer, true);
			/* 既然在这里，不妨也做这件事 */
			vacuumRedirectAndPlaceholder(index, bds->info->heaprel, buffer);

			SpGistSetLastUsedPage(index, buffer);

			/*
			 * 我们不仅可把本项标为已完成，也可把后面指向
			 * 同一页的项标为已完成，因为我们清理了整页。
			 */
			pitem->done = true;
			for (nitem = pitem->next; nitem != NULL; nitem = nitem->next)
			{
				if (ItemPointerGetBlockNumber(&nitem->tid) == blkno)
					nitem->done = true;
			}
		}
		else
		{
			/*
			 * 在内部页上，访问被引用的内部元组，并将其所有
			 * 下行链接加入挂起列表。同一页上可能有针对多个
			 * 内部元组的挂起项（按空间分配方式，这其实很
			 * 常见），因此趁我们在这里时一并获取
			 * 它们。
			 */
			for (nitem = pitem; nitem != NULL; nitem = nitem->next)
			{
				if (nitem->done)
					continue;
				if (ItemPointerGetBlockNumber(&nitem->tid) == blkno)
				{
					OffsetNumber offset;
					SpGistInnerTuple innerTuple;

					offset = ItemPointerGetOffsetNumber(&nitem->tid);
					innerTuple = (SpGistInnerTuple) PageGetItem(page,
																PageGetItemId(page, offset));
					if (innerTuple->tupstate == SPGIST_LIVE)
					{
						SpGistNodeTuple node;
						int			i;

						SGITITERATE(innerTuple, i, node)
						{
							if (ItemPointerIsValid(&node->t_tid))
								spgAddPendingTID(bds, &node->t_tid);
						}
					}
					else if (innerTuple->tupstate == SPGIST_REDIRECT)
					{
						/* 把注意力转到重定向目标 */
						spgAddPendingTID(bds,
										 &((SpGistDeadTuple) innerTuple)->pointer);
					}
					else
						elog(ERROR, "unexpected SPGiST tuple state: %d",
							 innerTuple->tupstate);

					nitem->done = true;
				}
			}
		}

		UnlockReleaseBuffer(buffer);
	}

	spgClearPendingList(bds);
}

/*
 * 执行 bulkdelete 扫描
 */
static void
spgvacuumscan(spgBulkDeleteState *bds)
{
	Relation	index = bds->info->index;
	bool		needLock;
	BlockNumber num_pages,
				blkno;

	/* 完成 spgBulkDeleteState 的设置 */
	initSpGistState(&bds->spgstate, index);
	bds->pendingList = NULL;
	bds->myXmin = GetActiveSnapshot()->xmin;
	bds->lastFilledBlock = SPGIST_LAST_FIXED_BLKNO;

	/*
	 * 重置将在扫描期间递增的计数；在单次 VACUUM
	 * 命令期间多次扫描时需要这样做
	 */
	bds->stats->estimated_count = false;
	bds->stats->num_index_tuples = 0;
	bds->stats->pages_deleted = 0;

	/* 对新关系或临时关系可跳过加锁 */
	needLock = !RELATION_IS_LOCAL(index);

	/*
	 * 外层循环按物理顺序遍历除元页外的所有索引页
	 * （我们希望内核配合提供预读以提高速度）。关键是
	 * 访问所有叶子页，包括扫描开始后新加入的，否则
	 * 可能无法删除某些可删除元组。关于这一点见
	 * btvacuumscan() 中更详尽的
	 * 注释。
	 */
	blkno = SPGIST_METAPAGE_BLKNO + 1;
	for (;;)
	{
		/* 获取当前关系长度 */
		if (needLock)
			LockRelationForExtension(index, ExclusiveLock);
		num_pages = RelationGetNumberOfBlocks(index);
		if (needLock)
			UnlockRelationForExtension(index, ExclusiveLock);

		/* 若已扫描完整个关系则退出 */
		if (blkno >= num_pages)
			break;
		/* 遍历各页，然后循环回去重新检查长度 */
		for (; blkno < num_pages; blkno++)
		{
			spgvacuumpage(bds, blkno);
			/* 每处理一页后清空挂起列表 */
			if (bds->pendingList != NULL)
				spgprocesspending(bds);
		}
	}

	/* 把本地 lastUsedPages 缓存传播到元块 */
	SpGistUpdateMetaPage(index);

	/*
	 * 若发现任何空页（并已记入 FSM），则强制更新上层
	 * FSM 页，以确保搜索者能找到它们。这些页也可能在
	 * 先前扫描中已被发现，因而这是浪费时间，但相对于
	 * 扫描索引来说足够便宜，应无大碍，而且尽早而非
	 * 尽晚让空闲页可用似乎
	 * 值得。
	 *
	 * 注意：若不存在空页，我们根本
	 * 不费心清理
	 * FSM。
	 */
	if (bds->stats->pages_deleted > 0)
		IndexFreeSpaceMapVacuum(index);

	/*
	 * 若可能则截断索引
	 *
	 * XXX 已禁用，因为在可能有并发插入时不安全。
	 * 我们得重新扫描各页以确保它们仍为空，而这似乎
	 * 不值得。注意 btree 也不这样做。
	 *
	 * 不截断的另一原因是，它会使元页及其他后端关系
	 * 缓存中缓存的 "有空闲空间的页" 指针失效，也就是
	 * 让它们指向不存在的页。为保护使用这些指针的
	 * 地方而添加 RelationGetNumberOfBlocks 调用会
	 * 过于昂贵。
	 */
#ifdef NOT_USED
	if (num_pages > bds->lastFilledBlock + 1)
	{
		BlockNumber lastBlock = num_pages - 1;

		num_pages = bds->lastFilledBlock + 1;
		RelationTruncate(index, num_pages);
		bds->stats->pages_removed += lastBlock - bds->lastFilledBlock;
		bds->stats->pages_deleted -= lastBlock - bds->lastFilledBlock;
	}
#endif

	/* 报告最终统计 */
	bds->stats->num_pages = num_pages;
	bds->stats->pages_newly_deleted = bds->stats->pages_deleted;
	bds->stats->pages_free = bds->stats->pages_deleted;
}

/*
 * 批量删除指向一组堆元组的所有索引项。
 * 目标元组集合由回调例程指定，该例程告知
 * 任一给定堆元组（由 ItemPointer 标识）是否正在被删除。
 *
 * 结果：一个 palloc 出来的结构，含供 VACUUM 显示的统计信息。
 */
IndexBulkDeleteResult *
spgbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			  IndexBulkDeleteCallback callback, void *callback_state)
{
	spgBulkDeleteState bds;

	/* 若首次进入则分配 stats，否则复用已有结构 */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	bds.info = info;
	bds.stats = stats;
	bds.callback = callback;
	bds.callback_state = callback_state;

	spgvacuumscan(&bds);

	return stats;
}

/* spgvacuumcleanup 期间不删除任何元组的哑回调 */
static bool
dummy_callback(ItemPointer itemptr, void *state)
{
	return false;
}

/*
 * VACUUM 之后的清理。
 *
 * 结果：一个 palloc 出来的结构，含供 VACUUM 显示的统计信息。
 */
IndexBulkDeleteResult *
spgvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	spgBulkDeleteState bds;

	/* ANALYZE ONLY 模式下为空操作 */
	if (info->analyze_only)
		return stats;

	/*
	 * 若前面已有 bulkdelete 遍，则无需再扫描索引。
	 * 否则做一遍不删除任何存活元组的扫描，但仍可能
	 * 通过重定向/占位清理和/或 FSM 维护做些有用的事，
	 * 并且无论如何都会提供统计。
	 */
	if (stats == NULL)
	{
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
		bds.info = info;
		bds.stats = stats;
		bds.callback = dummy_callback;
		bds.callback_state = NULL;

		spgvacuumscan(&bds);
	}

	/*
	 * 我们很容易被并发元组移动迷惑，从而把某些索引元组
	 * 重复计数，因此若底层堆的计数是准确的，则不相信任何
	 * 超过堆计数的总数……若我们确实准确知道堆计数。否则
	 * 这可能只会把事情弄得更糟。
	 */
	if (!info->estimated_count)
	{
		if (stats->num_index_tuples > info->num_heap_tuples)
			stats->num_index_tuples = info->num_heap_tuples;
	}

	return stats;
}
