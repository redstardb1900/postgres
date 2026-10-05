/*-------------------------------------------------------------------------
 *
 * heapam_handler.c
 *	  堆表访问方法代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heapam_handler.c
 *
 *
 * NOTES
 *	  本文件把下层 heapam.c 等例程接到
 *	  表 AM 抽象上。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/multixact.h"
#include "access/rewriteheap.h"
#include "access/syncscan.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"

static void reform_and_rewrite_tuple(HeapTuple tuple,
									 Relation OldHeap, Relation NewHeap,
									 Datum *values, bool *isnull, RewriteState rwstate);

static bool SampleHeapTupleVisible(TableScanDesc scan, Buffer buffer,
								   HeapTuple tuple,
								   OffsetNumber tupoffset);

static BlockNumber heapam_scan_get_blocks_done(HeapScanDesc hscan);

static const TableAmRoutine heapam_methods;


/* ------------------------------------------------------------------------
 * 堆 AM 的槽相关回调
 * ------------------------------------------------------------------------
 */

static const TupleTableSlotOps *
heapam_slot_callbacks(Relation relation)
{
	return &TTSOpsBufferHeapTuple;
}


/* ------------------------------------------------------------------------
 * 堆 AM 的索引扫描回调
 * ------------------------------------------------------------------------
 */

static IndexFetchTableData *
heapam_index_fetch_begin(Relation rel)
{
	IndexFetchHeapData *hscan = palloc0(sizeof(IndexFetchHeapData));

	hscan->xs_base.rel = rel;
	hscan->xs_cbuf = InvalidBuffer;

	return &hscan->xs_base;
}

static void
heapam_index_fetch_reset(IndexFetchTableData *scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;

	if (BufferIsValid(hscan->xs_cbuf))
	{
		ReleaseBuffer(hscan->xs_cbuf);
		hscan->xs_cbuf = InvalidBuffer;
	}
}

static void
heapam_index_fetch_end(IndexFetchTableData *scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;

	heapam_index_fetch_reset(scan);

	pfree(hscan);
}

static bool
heapam_index_fetch_tuple(struct IndexFetchTableData *scan,
						 ItemPointer tid,
						 Snapshot snapshot,
						 TupleTableSlot *slot,
						 bool *call_again, bool *all_dead)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	bool		got_heap_tuple;

	Assert(TTS_IS_BUFFERTUPLE(slot));

	/* 若正处于 HOT 链中途，可以跳过切换缓冲区的逻辑。 */
	if (!*call_again)
	{
		/* 若还没有正确的缓冲区，就切换过去 */
		Buffer		prev_buf = hscan->xs_cbuf;

		hscan->xs_cbuf = ReleaseAndReadBuffer(hscan->xs_cbuf,
											  hscan->xs_base.rel,
											  ItemPointerGetBlockNumber(tid));

		/*
		 * 剪枝该页，但仅当我们原先不在这一页上
		 */
		if (prev_buf != hscan->xs_cbuf)
			heap_page_prune_opt(hscan->xs_base.rel, hscan->xs_cbuf);
	}

	/* 取得缓冲区上的共享锁，以便检查可见性 */
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_SHARE);
	got_heap_tuple = heap_hot_search_buffer(tid,
											hscan->xs_base.rel,
											hscan->xs_cbuf,
											snapshot,
											&bslot->base.tupdata,
											all_dead,
											!*call_again);
	bslot->base.tupdata.t_self = *tid;
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);

	if (got_heap_tuple)
	{
		/*
		 * 只有在非 MVCC 快照下，HOT 链上才可能
		 * 有不止一个成员可见。
		 */
		*call_again = !IsMVCCSnapshot(snapshot);

		slot->tts_tableOid = RelationGetRelid(scan->rel);
		ExecStoreBufferHeapTuple(&bslot->base.tupdata, slot, hscan->xs_cbuf);
	}
	else
	{
		/* 已经到达 HOT 链的末端。 */
		*call_again = false;
	}

	return got_heap_tuple;
}


/* ------------------------------------------------------------------------
 * 堆 AM 中对单个元组的非修改操作回调
 * ------------------------------------------------------------------------
 */

static bool
heapam_fetch_row_version(Relation relation,
						 ItemPointer tid,
						 Snapshot snapshot,
						 TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	Buffer		buffer;

	Assert(TTS_IS_BUFFERTUPLE(slot));

	bslot->base.tupdata.t_self = *tid;
	if (heap_fetch(relation, snapshot, &bslot->base.tupdata, &buffer, false))
	{
		/* 存入槽中，并移交既有的钉 */
		ExecStorePinnedBufferHeapTuple(&bslot->base.tupdata, slot, buffer);
		slot->tts_tableOid = RelationGetRelid(relation);

		return true;
	}

	return false;
}

static bool
heapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;

	return ItemPointerIsValid(tid) &&
		ItemPointerGetBlockNumber(tid) < hscan->rs_nblocks;
}

static bool
heapam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								Snapshot snapshot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	bool		res;

	Assert(TTS_IS_BUFFERTUPLE(slot));
	Assert(BufferIsValid(bslot->buffer));

	/*
	 * 调用 HeapTupleSatisfiesVisibility 需要缓冲区的钉和锁。
	 * 调用方应当持有钉，但不应持有锁。
	 */
	LockBuffer(bslot->buffer, BUFFER_LOCK_SHARE);
	res = HeapTupleSatisfiesVisibility(bslot->base.tuple, snapshot,
									   bslot->buffer);
	LockBuffer(bslot->buffer, BUFFER_LOCK_UNLOCK);

	return res;
}


/* ----------------------------------------------------------------------------
 *  堆 AM 物理元组操作函数。
 * ----------------------------------------------------------------------------
 */

static void
heapam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
					int options, BulkInsertState bistate)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

	/* 把表 OID 填入该元组 */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	/* 执行插入，并复制得到的 ItemPointer */
	heap_insert(relation, tuple, cid, options, bistate);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	if (shouldFree)
		pfree(tuple);
}

static void
heapam_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
								CommandId cid, int options,
								BulkInsertState bistate, uint32 specToken)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

	/* 把表 OID 填入该元组 */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	HeapTupleHeaderSetSpeculativeToken(tuple->t_data, specToken);
	options |= HEAP_INSERT_SPECULATIVE;

	/* 执行插入，并复制得到的 ItemPointer */
	heap_insert(relation, tuple, cid, options, bistate);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	if (shouldFree)
		pfree(tuple);
}

static void
heapam_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
								  uint32 specToken, bool succeeded)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

	/* 据此调整元组的状态 */
	if (succeeded)
		heap_finish_speculative(relation, &slot->tts_tid);
	else
		heap_abort_speculative(relation, &slot->tts_tid);

	if (shouldFree)
		pfree(tuple);
}

static TM_Result
heapam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
					Snapshot snapshot, Snapshot crosscheck, bool wait,
					TM_FailureData *tmfd, bool changingPart)
{
	/*
	 * 目前，索引元组的删除在 VACUUM 时处理；若存储层自己
	 * 正在清理死元组，那也正是一并调用
	 * 索引元组删除的时机。
	 */
	return heap_delete(relation, tid, cid, crosscheck, wait, tmfd, changingPart);
}


static TM_Result
heapam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
					CommandId cid, Snapshot snapshot, Snapshot crosscheck,
					bool wait, TM_FailureData *tmfd,
					LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
	TM_Result	result;

	/* 把表 OID 填入该元组 */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	result = heap_update(relation, otid, tuple, cid, crosscheck, wait,
						 tmfd, lockmode, update_indexes);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	/*
	 * 决定该元组是否需要新的索引项
	 *
	 * 注意：heap_update 把新元组的 tid（位置）放在
	 * t_self 中返回。
	 *
	 * 若这次更新不是 HOT，就必须更新全部索引。若更新是
	 * HOT，则可能改了汇总列，因此我们要么只更新
	 * 汇总索引，要么一个都不更新。
	 */
	if (result != TM_Ok)
	{
		Assert(*update_indexes == TU_None);
		*update_indexes = TU_None;
	}
	else if (!HeapTupleIsHeapOnly(tuple))
		Assert(*update_indexes == TU_All);
	else
		Assert((*update_indexes == TU_Summarizing) ||
			   (*update_indexes == TU_None));

	if (shouldFree)
		pfree(tuple);

	return result;
}

static TM_Result
heapam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot,
				  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				  LockWaitPolicy wait_policy, uint8 flags,
				  TM_FailureData *tmfd)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	TM_Result	result;
	Buffer		buffer;
	HeapTuple	tuple = &bslot->base.tupdata;
	bool		follow_updates;

	follow_updates = (flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) != 0;
	tmfd->traversed = false;

	Assert(TTS_IS_BUFFERTUPLE(slot));

tuple_lock_retry:
	tuple->t_self = *tid;
	result = heap_lock_tuple(relation, tuple, cid, mode, wait_policy,
							 follow_updates, &buffer, tmfd);

	if (result == TM_Updated &&
		(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION))
	{
		/* 复查时不应当遇到推测元组 */
		Assert(!HeapTupleHeaderIsSpeculative(tuple->t_data));

		ReleaseBuffer(buffer);

		if (!ItemPointerEquals(&tmfd->ctid, &tuple->t_self))
		{
			SnapshotData SnapshotDirty;
			TransactionId priorXmax;

			/* 它已被更新，因此查看更新后的版本 */
			*tid = tmfd->ctid;
			/* 更新后的行，其 xmin 应当与这个 xmax 匹配 */
			priorXmax = tmfd->xmax;

			/* 表明链上更靠后的元组即将被锁定 */
			tmfd->traversed = true;

			/*
			 * 取回目标元组
			 *
			 * 在此循环，以处理已更新或正忙的元组
			 */
			InitDirtySnapshot(SnapshotDirty);
			for (;;)
			{
				if (ItemPointerIndicatesMovedPartitions(tid))
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("tuple to be locked was already moved to another partition due to concurrent update")));

				tuple->t_self = *tid;
				if (heap_fetch(relation, &SnapshotDirty, tuple, &buffer, true))
				{
					/*
					 * 若 xmin 不是我们所预期的，该槽位必定
					 * 已被回收，并重用于一个无关元组。这
					 * 意味着该行的最新版本已被删除，
					 * 因此我们无需做任何事。（不取得缓冲区的
					 * 内容锁就检查 xmin 应当是安全的。我们假定
					 * 读取 TransactionId 是原子的，并且已有元组里的 Xmin
					 * 除了变成无效或冻结之外从不会改变，
					 * 而这两种取值都不可能与 priorXmax 匹配。）
					 */
					if (!TransactionIdEquals(HeapTupleHeaderGetXmin(tuple->t_data),
											 priorXmax))
					{
						ReleaseBuffer(buffer);
						return TM_Deleted;
					}

					/* 否则 xmin 不应当是脏的... */
					if (TransactionIdIsValid(SnapshotDirty.xmin))
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg_internal("t_xmin %u is uncommitted in tuple (%u,%u) to be updated in table \"%s\"",
												 SnapshotDirty.xmin,
												 ItemPointerGetBlockNumber(&tuple->t_self),
												 ItemPointerGetOffsetNumber(&tuple->t_self),
												 RelationGetRelationName(relation))));

					/*
					 * 若元组正被其他事务更新，我们就必须等待其提交或中止，
					 * 否则就报错退出。
					 */
					if (TransactionIdIsValid(SnapshotDirty.xmax))
					{
						ReleaseBuffer(buffer);
						switch (wait_policy)
						{
							case LockWaitBlock:
								XactLockTableWait(SnapshotDirty.xmax,
												  relation, &tuple->t_self,
												  XLTW_FetchUpdated);
								break;
							case LockWaitSkip:
								if (!ConditionalXactLockTableWait(SnapshotDirty.xmax))
									/* 跳过，而不是等待 */
									return TM_WouldBlock;
								break;
							case LockWaitError:
								if (!ConditionalXactLockTableWait(SnapshotDirty.xmax))
									ereport(ERROR,
											(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
											 errmsg("could not obtain lock on row in relation \"%s\"",
													RelationGetRelationName(relation))));
								break;
						}
						continue;	/* 回到循环开头，再次 heap_fetch */
					}

					/*
					 * 若元组是我们自己的事务插入的，就必须
					 * 用 cmin 对照 cid 检查：cmin >= 当前 CID 表示
					 * 我们的命令无法看见该元组，因此应当忽略
					 * 它。否则 heap_lock_tuple() 会报错，
					 * 之后任何更新或删除该元组的尝试也会
					 * 报错。（不必检查 cmax，因为
					 * HeapTupleSatisfiesDirty 会把本事务删除的
					 * 元组视为死亡，与 cmax 无关。）我们刚刚
					 * 已经确认 priorXmax == xmin，因此可以检查该
					 * 变量，而不必再次调用 HeapTupleHeaderGetXmin。
					 */
					if (TransactionIdIsCurrentTransactionId(priorXmax) &&
						HeapTupleHeaderGetCmin(tuple->t_data) >= cid)
					{
						tmfd->xmax = priorXmax;

						/*
						 * 有问题的取值是 Cmin，因此存下它。见
						 * 上文。
						 */
						tmfd->cmax = HeapTupleHeaderGetCmin(tuple->t_data);
						ReleaseBuffer(buffer);
						return TM_SelfModified;
					}

					/*
					 * 这是活元组，因此再次尝试锁定它。
					 */
					ReleaseBuffer(buffer);
					goto tuple_lock_retry;
				}

				/*
				 * 若所引用的槽位实际上是空的，该行的最新
				 * 版本必定已被删除，因此我们无需
				 * 做任何事。
				 */
				if (tuple->t_data == NULL)
				{
					Assert(!BufferIsValid(buffer));
					return TM_Deleted;
				}

				/*
				 * 同上，若 xmin 不是我们所预期的，就什么都不做。
				 */
				if (!TransactionIdEquals(HeapTupleHeaderGetXmin(tuple->t_data),
										 priorXmax))
				{
					ReleaseBuffer(buffer);
					return TM_Deleted;
				}

				/*
				 * 若执行到这里，说明找到了元组，但它没有通过
				 * SnapshotDirty。假定 xmin 要么是已提交事务，
				 * 要么是我们自己的事务（如果我们正试图
				 * 修改该元组，就必定应当如此），这就意味着该行
				 * 已被某个已提交事务或我们自己的事务
				 * 更新或删除。若已删除，可以忽略它；若已
				 * 更新，则顺着链走到下一版本，并重复
				 * 整个过程。
				 *
				 * 同上，不持有缓冲区内容锁就检查 xmax 与 t_ctid
				 * 应当是安全的，因为它们不可能正在改变。
				 * 不过我们最好仍然持有一个缓冲区钉。
				 */
				if (ItemPointerEquals(&tuple->t_self, &tuple->t_data->t_ctid))
				{
					/* 已删除，因此不再管它 */
					ReleaseBuffer(buffer);
					return TM_Deleted;
				}

				/* 已更新，因此查看更新后的行 */
				*tid = tuple->t_data->t_ctid;
				/* 更新后的行，其 xmin 应当与这个 xmax 匹配 */
				priorXmax = HeapTupleHeaderGetUpdateXid(tuple->t_data);
				ReleaseBuffer(buffer);
				/* 回到循环开头，取链上下一个 */
			}
		}
		else
		{
			/* 元组已被删除，因此放弃 */
			return TM_Deleted;
		}
	}

	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	/* 存入槽中，并移交既有的钉 */
	ExecStorePinnedBufferHeapTuple(tuple, slot, buffer);

	return result;
}


/* ------------------------------------------------------------------------
 * 堆 AM 的 DDL 相关回调。
 * ------------------------------------------------------------------------
 */

static void
heapam_relation_set_new_filelocator(Relation rel,
									const RelFileLocator *newrlocator,
									char persistence,
									TransactionId *freezeXid,
									MultiXactId *minmulti)
{
	SMgrRelation srel;

	/*
	 * 初始化为可能把元组放入该表的最小 XID。我们
	 * 知道比 RecentXmin 更旧的事务都已不再运行，因此
	 * 这样就够了。
	 */
	*freezeXid = RecentXmin;

	/*
	 * 类似地，把最小 Multixact 初始化为可能存入
	 * 该表元组中的第一个值。正在运行的事务
	 * 可能复用其本地缓存中的值，因此我们小心地
	 * 把当前正在运行的全部多事务都考虑在内。
	 *
	 * XXX 这里还可以再细化，但值得为此费事吗？
	 */
	*minmulti = GetOldestMultiXactId();

	srel = RelationCreateStorage(*newrlocator, persistence, true);

	/*
	 * 若有需要，为不记录 WAL 的表建立初始分支，以便
	 * 重启时能正确重新初始化。即使该页已经记入日志，
	 * 也必须立即同步，因为写入没有经过
	 * shared_buffers，并发检查点可能已经把重做指针
	 * 移过我们的 xlog 记录。恢复在重放时也可能删掉它，
	 * 例如重放 XLOG_DBASE_CREATE* 或 XLOG_TBLSPC_CREATE
	 * 记录时。因此即使 wal_level=minimal 也有必要记日志。
	 */
	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
			   rel->rd_rel->relkind == RELKIND_MATVIEW ||
			   rel->rd_rel->relkind == RELKIND_TOASTVALUE);
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrlocator, INIT_FORKNUM);
		smgrimmedsync(srel, INIT_FORKNUM);
	}

	smgrclose(srel);
}

static void
heapam_relation_nontransactional_truncate(Relation rel)
{
	RelationTruncate(rel, 0);
}

static void
heapam_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation dstrel;

	dstrel = smgropen(*newrlocator, rel->rd_backend);

	/*
	 * 因为我们直接复制文件，而不查看共享缓冲区，
	 * 最好先把源关系中位于共享缓冲区里的页面
	 * 全部刷出。我们假定在持有该关系的排他锁期间，
	 * 不会再有新的修改。
	 */
	FlushRelationBuffers(rel);

	/*
	 * 创建并复制该关系的全部分支，并安排删除
	 * 旧的物理文件。
	 *
	 * 注意：relfilenumber 值的任何冲突都会在
	 * RelationCreateStorage() 中被发现。
	 */
	RelationCreateStorage(*newrlocator, rel->rd_rel->relpersistence, true);

	/* 复制主分支 */
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);

	/* 复制那些存在的其余分支 */
	for (ForkNumber forkNum = MAIN_FORKNUM + 1;
		 forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(RelationGetSmgr(rel), forkNum))
		{
			smgrcreate(dstrel, forkNum, false);

			/*
			 * 若关系是持久的，或这是不记录 WAL 的关系的
			 * 初始分支，就为创建记 WAL 日志。
			 */
			if (RelationIsPermanent(rel) ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
				 forkNum == INIT_FORKNUM))
				log_smgrcreate(newrlocator, forkNum);
			RelationCopyStorage(RelationGetSmgr(rel), dstrel, forkNum,
								rel->rd_rel->relpersistence);
		}
	}


	/* 删除旧关系，并关闭新关系 */
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

static void
heapam_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap,
								 Relation OldIndex, bool use_sort,
								 TransactionId OldestXmin,
								 TransactionId *xid_cutoff,
								 MultiXactId *multi_cutoff,
								 double *num_tuples,
								 double *tups_vacuumed,
								 double *tups_recently_dead)
{
	RewriteState rwstate;
	IndexScanDesc indexScan;
	TableScanDesc tableScan;
	HeapScanDesc heapScan;
	bool		is_system_catalog;
	Tuplesortstate *tuplesort;
	TupleDesc	oldTupDesc = RelationGetDescr(OldHeap);
	TupleDesc	newTupDesc = RelationGetDescr(NewHeap);
	TupleTableSlot *slot;
	int			natts;
	Datum	   *values;
	bool	   *isnull;
	BufferHeapTupleTableSlot *hslot;
	BlockNumber prev_cblock = InvalidBlockNumber;

	/* 记住它是否为系统目录 */
	is_system_catalog = IsSystemRelation(OldHeap);

	/*
	 * 有效的 smgr_targblock 意味着已有东西写入过该关系。
	 * 这也许无害，但本函数没有为此做准备。
	 */
	Assert(RelationGetTargetBlock(NewHeap) == InvalidBlockNumber);

	/* 预先分配 values/isnull 数组 */
	natts = newTupDesc->natts;
	values = (Datum *) palloc(natts * sizeof(Datum));
	isnull = (bool *) palloc(natts * sizeof(bool));

	/* 初始化重写操作 */
	rwstate = begin_heap_rewrite(OldHeap, NewHeap, OldestXmin, *xid_cutoff,
								 *multi_cutoff);


	/* 若需要排序，就把它建好 */
	if (use_sort)
		tuplesort = tuplesort_begin_cluster(oldTupDesc, OldIndex,
											maintenance_work_mem,
											NULL, TUPLESORT_NONE);
	else
		tuplesort = NULL;

	/*
	 * 准备扫描 OldHeap。为确保能看到仍需复制的
	 * 最近死亡元组，我们用 SnapshotAny 扫描，并用
	 * HeapTupleSatisfiesVacuum 做可见性测试。
	 */
	if (OldIndex != NULL && !use_sort)
	{
		const int	ci_index[] = {
			PROGRESS_CLUSTER_PHASE,
			PROGRESS_CLUSTER_INDEX_RELID
		};
		int64		ci_val[2];

		/* 把阶段和 OIDOldIndex 填入各列 */
		ci_val[0] = PROGRESS_CLUSTER_PHASE_INDEX_SCAN_HEAP;
		ci_val[1] = RelationGetRelid(OldIndex);
		pgstat_progress_update_multi_param(2, ci_index, ci_val);

		tableScan = NULL;
		heapScan = NULL;
		indexScan = index_beginscan(OldHeap, OldIndex, SnapshotAny, 0, 0);
		index_rescan(indexScan, NULL, 0, NULL, 0);
	}
	else
	{
		/* 在扫描并排序模式以及 VACUUM FULL 下，设置阶段 */
		pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
									 PROGRESS_CLUSTER_PHASE_SEQ_SCAN_HEAP);

		tableScan = table_beginscan(OldHeap, SnapshotAny, 0, (ScanKey) NULL);
		heapScan = (HeapScanDesc) tableScan;
		indexScan = NULL;

		/* 设置堆块总数 */
		pgstat_progress_update_param(PROGRESS_CLUSTER_TOTAL_HEAP_BLKS,
									 heapScan->rs_nblocks);
	}

	slot = table_slot_create(OldHeap, NULL);
	hslot = (BufferHeapTupleTableSlot *) slot;

	/*
	 * 扫描 OldHeap，或按 OldIndex 顺序，或顺序扫描；
	 * 把每个元组复制进 NewHeap，或暂时交给 tuplesort
	 * 模块。注意我们不费心对死元组排序（它们反正
	 * 不会进入新表）。
	 */
	for (;;)
	{
		HeapTuple	tuple;
		Buffer		buf;
		bool		isdead;

		CHECK_FOR_INTERRUPTS();

		if (indexScan != NULL)
		{
			if (!index_getnext_slot(indexScan, ForwardScanDirection, slot))
				break;

			/* 因为没有使用扫描键，绝不应当需要复查 */
			if (indexScan->xs_recheck)
				elog(ERROR, "CLUSTER does not support lossy index conditions");
		}
		else
		{
			if (!table_scan_getnextslot(tableScan, ForwardScanDirection, slot))
			{
				/*
				 * 若扫描的最后几页是空的，我们会在
				 * heap_blks_scanned != heap_blks_total 的情况下进入
				 * 下一阶段。相反，为确保表扫描阶段结束后
				 * heap_blks_scanned 与 heap_blks_total 相等，
				 * 在表扫描完成时，手工把该参数
				 * 更新为正确的值。
				 */
				pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_BLKS_SCANNED,
											 heapScan->rs_nblocks);
				break;
			}

			/*
			 * 在扫描并排序模式以及 VACUUM FULL 下，设置已扫描的
			 * 堆块数
			 *
			 * 注意 heapScan 可能从某个偏移开始并回绕，即
			 * rs_startblock 可能 >0，而 rs_cblock 结束时的编号
			 * 可能小于 rs_startblock。为避免向用户展示这种回绕，
			 * 我们用 rs_startblock 对 rs_cblock 做偏移（对 rs_nblocks 取模）。
			 */
			if (prev_cblock != heapScan->rs_cblock)
			{
				pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_BLKS_SCANNED,
											 (heapScan->rs_cblock +
											  heapScan->rs_nblocks -
											  heapScan->rs_startblock
											  ) % heapScan->rs_nblocks + 1);
				prev_cblock = heapScan->rs_cblock;
			}
		}

		tuple = ExecFetchSlotHeapTuple(slot, false, NULL);
		buf = hslot->buffer;

		LockBuffer(buf, BUFFER_LOCK_SHARE);

		switch (HeapTupleSatisfiesVacuum(tuple, OldestXmin, buf))
		{
			case HEAPTUPLE_DEAD:
				/* 确已死亡 */
				isdead = true;
				break;
			case HEAPTUPLE_RECENTLY_DEAD:
				*tups_recently_dead += 1;
				/* fall through */
			case HEAPTUPLE_LIVE:
				/* 活的或最近死亡，必须复制 */
				isdead = false;
				break;
			case HEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * 因为我们对关系持有排他锁，通常只有一种可能
				 * 看到这种情况：它是在我们自己的事务里更早
				 * 插入的。不过系统目录里也可能发生，因为我们
				 * 往往在提交之前就释放那里的写锁。若两种情形
				 * 都不成立，就发出警告；但无论如何，
				 * 我们最好还是复制它。
				 */
				if (!is_system_catalog &&
					!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple->t_data)))
					elog(WARNING, "concurrent insert in progress within table \"%s\"",
						 RelationGetRelationName(OldHeap));
				/* 按活元组处理 */
				isdead = false;
				break;
			case HEAPTUPLE_DELETE_IN_PROGRESS:

				/*
				 * 与 INSERT_IN_PROGRESS 情形类似。
				 */
				if (!is_system_catalog &&
					!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetUpdateXid(tuple->t_data)))
					elog(WARNING, "concurrent delete in progress within table \"%s\"",
						 RelationGetRelationName(OldHeap));
				/* 按最近死亡处理 */
				*tups_recently_dead += 1;
				isdead = false;
				break;
			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				isdead = false; /* 让编译器保持安静 */
				break;
		}

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		if (isdead)
		{
			*tups_vacuumed += 1;
			/* 堆重写模块仍然需要看到它... */
			if (rewrite_heap_dead_tuple(rwstate, tuple))
			{
				/* 先前一个最近死亡的元组现在已知已死 */
				*tups_vacuumed += 1;
				*tups_recently_dead -= 1;
			}
			continue;
		}

		*num_tuples += 1;
		if (tuplesort != NULL)
		{
			tuplesort_putheaptuple(tuplesort, tuple);

			/*
			 * 在扫描并排序模式下，报告已扫描元组数
			 * 的增加
			 */
			pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_TUPLES_SCANNED,
										 *num_tuples);
		}
		else
		{
			const int	ct_index[] = {
				PROGRESS_CLUSTER_HEAP_TUPLES_SCANNED,
				PROGRESS_CLUSTER_HEAP_TUPLES_WRITTEN
			};
			int64		ct_val[2];

			reform_and_rewrite_tuple(tuple, OldHeap, NewHeap,
									 values, isnull, rwstate);

			/*
			 * 在索引扫描模式以及 VACUUM FULL 下，报告已扫描
			 * 和已写入的元组数的增加
			 */
			ct_val[0] = *num_tuples;
			ct_val[1] = *num_tuples;
			pgstat_progress_update_multi_param(2, ct_index, ct_val);
		}
	}

	if (indexScan != NULL)
		index_endscan(indexScan);
	if (tableScan != NULL)
		table_endscan(tableScan);
	if (slot)
		ExecDropSingleTupleTableSlot(slot);

	/*
	 * 在扫描并排序模式下，完成排序，然后从 tuplestore
	 * 读出全部活元组，并写入新关系。
	 */
	if (tuplesort != NULL)
	{
		double		n_tuples = 0;

		/* 报告我们现在正在对元组排序 */
		pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
									 PROGRESS_CLUSTER_PHASE_SORT_TUPLES);

		tuplesort_performsort(tuplesort);

		/* 报告我们现在正在写入新堆 */
		pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
									 PROGRESS_CLUSTER_PHASE_WRITE_NEW_HEAP);

		for (;;)
		{
			HeapTuple	tuple;

			CHECK_FOR_INTERRUPTS();

			tuple = tuplesort_getheaptuple(tuplesort, true);
			if (tuple == NULL)
				break;

			n_tuples += 1;
			reform_and_rewrite_tuple(tuple,
									 OldHeap, NewHeap,
									 values, isnull,
									 rwstate);
			/* 报告 n_tuples */
			pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_TUPLES_WRITTEN,
										 n_tuples);
		}

		tuplesort_end(tuplesort);
	}

	/* 写出剩余元组，并在需要时 fsync */
	end_heap_rewrite(rwstate);

	/* 清理 */
	pfree(values);
	pfree(isnull);
}

static bool
heapam_scan_analyze_next_block(TableScanDesc scan, BlockNumber blockno,
							   BufferAccessStrategy bstrategy)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;

	/*
	 * 我们必须保持对目标页缓冲区的钉，以确保
	 * 并发活动（例如 HOT 剪枝）不会在我们查看期间
	 * 把元组删掉。因此，在看完该页之前一直钉住它。我们
	 * 还选择在整个过程中持有缓冲区上的共享锁——也可以
	 * 对每个元组释放并重新取得共享锁，但因为每个元组
	 * 上做的工作不多，额外的锁往来大概还是
	 * 避免为好。
	 */
	hscan->rs_cblock = blockno;
	hscan->rs_cindex = FirstOffsetNumber;
	hscan->rs_cbuf = ReadBufferExtended(scan->rs_rd, MAIN_FORKNUM,
										blockno, RBM_NORMAL, bstrategy);
	LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);

	/* 在堆中，所有块都可以包含元组，因此总是返回真 */
	return true;
}

static bool
heapam_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin,
							   double *liverows, double *deadrows,
							   TupleTableSlot *slot)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;
	Page		targpage;
	OffsetNumber maxoffset;
	BufferHeapTupleTableSlot *hslot;

	Assert(TTS_IS_BUFFERTUPLE(slot));

	hslot = (BufferHeapTupleTableSlot *) slot;
	targpage = BufferGetPage(hscan->rs_cbuf);
	maxoffset = PageGetMaxOffsetNumber(targpage);

	/* 内层循环，遍历所选页上的全部元组 */
	for (; hscan->rs_cindex <= maxoffset; hscan->rs_cindex++)
	{
		ItemId		itemid;
		HeapTuple	targtuple = &hslot->base.tupdata;
		bool		sample_it = false;

		itemid = PageGetItemId(targpage, hscan->rs_cindex);

		/*
		 * 我们忽略未使用的和重定向行指针。DEAD 行指针
		 * 应当计为死亡，因为需要 VACUUM 来清除
		 * 它们。注意这条规则与 heap_page_prune()
		 * 的计数方式一致。
		 */
		if (!ItemIdIsNormal(itemid))
		{
			if (ItemIdIsDead(itemid))
				*deadrows += 1;
			continue;
		}

		ItemPointerSet(&targtuple->t_self, hscan->rs_cblock, hscan->rs_cindex);

		targtuple->t_tableOid = RelationGetRelid(scan->rs_rd);
		targtuple->t_data = (HeapTupleHeader) PageGetItem(targpage, itemid);
		targtuple->t_len = ItemIdGetLength(itemid);

		switch (HeapTupleSatisfiesVacuum(targtuple, OldestXmin,
										 hscan->rs_cbuf))
		{
			case HEAPTUPLE_LIVE:
				sample_it = true;
				*liverows += 1;
				break;

			case HEAPTUPLE_DEAD:
			case HEAPTUPLE_RECENTLY_DEAD:
				/* 把死亡行和最近死亡行计入 */
				*deadrows += 1;
				break;

			case HEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * 正在插入的行不计入。我们假定
				 * 插入事务提交或中止时，会
				 * 发出统计消息来增加正确的计数。这
				 * 只有在该事务于我们分析完该表之后结束时才正确；若事情
				 * 以相反的顺序发生，
				 * 它的统计更新会被我们的覆盖。不过，
				 * 只有当另一事务运行得足够久、
				 * 插入了许多元组时，误差才会很大，因此假定它会在我们
				 * 之后结束是更稳妥的选择。
				 *
				 * 一种特殊情形是，插入事务可能就是
				 * 我们自己的。这时应当计入并采样该行，
				 * 以照顾那些在同一事务中装入表并加以分析的
				 * 用户。（pgstat_report_analyze 必须调整
				 * 我们报告给累积统计系统的数字，才能使
				 * 结果正确。）
				 */
				if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(targtuple->t_data)))
				{
					sample_it = true;
					*liverows += 1;
				}
				break;

			case HEAPTUPLE_DELETE_IN_PROGRESS:

				/*
				 * 我们把正在删除的行与活行同样计入并采样，以便
				 * 删除事务在我们之后提交时，统计计数器的结果仍然正确，
				 * 依据与上文
				 * 相同的理由。
				 *
				 * 不过，若删除是由我们自己的事务完成的，就必须
				 * 把该行计为死亡，才能使 pgstat_report_analyze 的
				 * 统计调整结果正确。（注意：当该行在我们的事务中
				 * 既被插入又被删除时，这样处理
				 * 正好合适。）
				 *
				 * 这些选择的净效果是：我们表现得仿佛一个
				 * IN_PROGRESS 事务尚未发生，除非它
				 * 是我们自己的事务，而我们假定它已经发生。
				 *
				 * 这种方法保证，若我们同时看到某一行的前映像和后映像，
				 * 而该行正被并发事务更新，行为是合理的：我们会采样
				 * 前映像，而不会采样后映像。若该并发事务
				 * 从不提交，
				 * 得到的结果也同样合理。
				 */
				if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetUpdateXid(targtuple->t_data)))
					*deadrows += 1;
				else
				{
					sample_it = true;
					*liverows += 1;
				}
				break;

			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}

		if (sample_it)
		{
			ExecStoreBufferHeapTuple(targtuple, slot, hscan->rs_cbuf);
			hscan->rs_cindex++;

			/* 注意，这里我们让缓冲区保持锁定！ */
			return true;
		}
	}

	/* 现在释放该页上的锁和钉 */
	UnlockReleaseBuffer(hscan->rs_cbuf);
	hscan->rs_cbuf = InvalidBuffer;

	/* 同时避免旧的槽内容仍然钉住该页 */
	ExecClearTuple(slot);

	return false;
}

static double
heapam_index_build_range_scan(Relation heapRelation,
							  Relation indexRelation,
							  IndexInfo *indexInfo,
							  bool allow_sync,
							  bool anyvisible,
							  bool progress,
							  BlockNumber start_blockno,
							  BlockNumber numblocks,
							  IndexBuildCallback callback,
							  void *callback_state,
							  TableScanDesc scan)
{
	HeapScanDesc hscan;
	bool		is_system_catalog;
	bool		checking_uniqueness;
	HeapTuple	heapTuple;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	double		reltuples;
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	Snapshot	snapshot;
	bool		need_unregister_snapshot = false;
	TransactionId OldestXmin;
	BlockNumber previous_blkno = InvalidBlockNumber;
	BlockNumber root_blkno = InvalidBlockNumber;
	OffsetNumber root_offsets[MaxHeapTuplesPerPage];

	/*
	 * 健全性检查
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));

	/* 记住它是否为系统目录 */
	is_system_catalog = IsSystemRelation(heapRelation);

	/* 查看我们是否在验证唯一性或排他属性 */
	checking_uniqueness = (indexInfo->ii_Unique ||
						   indexInfo->ii_ExclusionOps != NULL);

	/*
	 * "任何可见"模式与唯一性检查不兼容；确保
	 * 只请求了其中之一。
	 */
	Assert(!(anyvisible && checking_uniqueness));

	/*
	 * 求值索引表达式和部分索引谓词需要一个 EState。
	 * 还需要一个槽来保存当前元组。
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heapRelation, NULL);

	/* 让 econtext 的扫描元组成为受测元组 */
	econtext->ecxt_scantuple = slot;

	/* 若有谓词，为其建立执行状态。 */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * 准备扫描基础关系。普通索引构建使用
	 * SnapshotAny，因为我们必须取出全部元组并自行做时间
	 * 限定检查（因为必须为 RECENTLY_DEAD 元组建索引）。并发
	 * 构建时，或在 bootstrap 期间，我们取一份常规 MVCC 快照，
	 * 并按该快照为仍然活着的内容建索引。
	 */
	OldestXmin = InvalidTransactionId;

	/* 这里可以忽略惰性 VACUUMs */
	if (!IsBootstrapProcessingMode() && !indexInfo->ii_Concurrent)
		OldestXmin = GetOldestNonRemovableTransactionId(heapRelation);

	if (!scan)
	{
		/*
		 * 串行索引构建。
		 *
		 * 这种情况下必须自己开始堆扫描。我们也许还需要
		 * 注册一个生命周期由我们直接控制的快照。
		 */
		if (!TransactionIdIsValid(OldestXmin))
		{
			snapshot = RegisterSnapshot(GetTransactionSnapshot());
			need_unregister_snapshot = true;
		}
		else
			snapshot = SnapshotAny;

		scan = table_beginscan_strat(heapRelation,	/* 关系 */
									 snapshot,	/* 快照 */
									 0, /* 键的个数 */
									 NULL,	/* 扫描键 */
									 true,	/* 允许缓冲区访问策略 */
									 allow_sync);	/* 允许同步扫描？ */
	}
	else
	{
		/*
		 * 并行索引构建。
		 *
		 * 并行情形从不自行注册或注销快照。快照
		 * 取自并行堆扫描，按与串行情形相同的标准，
		 * 它是 SnapshotAny 或一份 MVCC 快照。
		 */
		Assert(!IsBootstrapProcessingMode());
		Assert(allow_sync);
		snapshot = scan->rs_snapshot;
	}

	hscan = (HeapScanDesc) scan;

	/*
	 * 若使用 SnapshotAny，就必须已调用过 GetOldestNonRemovableTransactionId()。
	 * 对 MVCC 快照则不应当调用。（并行构建尤其
	 * 值得检查这一点，因为支持并行构建的 ambuild 例程
	 * 必须自己把这些细节理清。）
	 */
	Assert(snapshot == SnapshotAny || IsMVCCSnapshot(snapshot));
	Assert(snapshot == SnapshotAny ? TransactionIdIsValid(OldestXmin) :
		   !TransactionIdIsValid(OldestXmin));
	Assert(snapshot == SnapshotAny || !anyvisible);

	/* 公布要扫描的块数 */
	if (progress)
	{
		BlockNumber nblocks;

		if (hscan->rs_base.rs_parallel != NULL)
		{
			ParallelBlockTableScanDesc pbscan;

			pbscan = (ParallelBlockTableScanDesc) hscan->rs_base.rs_parallel;
			nblocks = pbscan->phs_nblocks;
		}
		else
			nblocks = hscan->rs_nblocks;

		pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL,
									 nblocks);
	}

	/* 设置我们的扫描端点 */
	if (!allow_sync)
		heap_setscanlimits(scan, start_blockno, numblocks);
	else
	{
		/* 同步扫描只能对整个关系请求 */
		Assert(start_blockno == 0);
		Assert(numblocks == InvalidBlockNumber);
	}

	reltuples = 0;

	/*
	 * 扫描基础关系中的全部元组。
	 */
	while ((heapTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		bool		tupleIsAlive;

		CHECK_FOR_INTERRUPTS();

		/* 若被要求，就报告扫描进度。 */
		if (progress)
		{
			BlockNumber blocks_done = heapam_scan_get_blocks_done(hscan);

			if (blocks_done != previous_blkno)
			{
				pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
											 blocks_done);
				previous_blkno = blocks_done;
			}
		}

		/*
		 * 处理已更新元组组成的 HOT 链时，我们希望为
		 * 活元组（若有）的值建索引，但索引时使用该链
		 * 根元组的 TID。要保持堆里的
		 * HOT 链结构，就必须这样做。因此，对处于 HOT 链中的每个元组，
		 * 我们都要能找到其根项偏移。当
		 * 第一次到达该关系的新页时，调用
		 * heap_get_root_tuples()，为该页建立根项偏移
		 * 映射。
		 *
		 * 跨缓冲区加锁与解锁来使用这些信息，看起来也许不安全。
		 * 不过我们对表持有 ShareLock，因此不应当发生
		 * 普通的插入、更新或删除；而且在访问该页的整个期间
		 * 我们都钉住缓冲区，因此也不会发生剪枝
		 * 操作。
		 *
		 * 若对表只持有 ShareUpdateExclusiveLock，
		 * 就可能出现一些我们在第一次读该页时
		 * 还不知道的 HOT 元组。为处理这种情况，当某个 HOT 元组指向
		 * 一个我们不知道的根项时，就重新取得
		 * 根偏移列表。
		 *
		 * 另外，虽然扫描该页时，我们对元组是否存活的判断可能改变
		 * （由于并发事务的提交或中止），
		 * 链根的位置不会变，因此这份信息不需要在
		 * 等待另一事务之后重建。
		 *
		 * 注意隐含假定：每条 HOT 链上活元组不超过一个
		 * ——否则我们可能创建出不止一个索引
		 * 项，都指向同一个根元组。
		 */
		if (hscan->rs_cblock != root_blkno)
		{
			Page		page = BufferGetPage(hscan->rs_cbuf);

			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);
			heap_get_root_tuples(page, root_offsets);
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			root_blkno = hscan->rs_cblock;
		}

		if (snapshot == SnapshotAny)
		{
			/* 自行做时间限定检查 */
			bool		indexIt;
			TransactionId xwait;

	recheck:

			/*
			 * 这里也许可以不加锁就检查缓冲区，
			 * 因为调用方应当对关系持有 ShareLock，但我们
			 * 还是保守一些。（即使有 HOT 剪枝，这句话仍然正确：
			 * 我们对缓冲区的钉会阻止剪枝。）
			 */
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);

			/*
			 * 本段里把元组计为活的标准，必须与
			 * analyze.c 中 heapam_scan_analyze_next_tuple() 的做法一致，
			 * 否则 CREATE INDEX 与 ANALYZE 可能得到差异极大的
			 * reltuples 值，例如存在许多最近死亡
			 * 元组时。
			 */
			switch (HeapTupleSatisfiesVacuum(heapTuple, OldestXmin,
											 hscan->rs_cbuf))
			{
				case HEAPTUPLE_DEAD:
					/* 确已死亡，可以忽略 */
					indexIt = false;
					tupleIsAlive = false;
					break;
				case HEAPTUPLE_LIVE:
					/* 正常情形，为其建索引并做唯一性检查 */
					indexIt = true;
					tupleIsAlive = true;
					/* 也把它计为活的 */
					reltuples += 1;
					break;
				case HEAPTUPLE_RECENTLY_DEAD:

					/*
					 * 若元组是最近删除的，仍然必须为它建索引，
					 * 以保持 MVCC 语义。（先前已经存在的
					 * 事务可能在我们建完索引后去使用它，
					 * 并且可能需要看到这些元组。）
					 *
					 * 不过，若它是 HOT 更新产生的，就只能为
					 * HOT 链末端的活元组建索引。由于这会
					 * 破坏已有快照的语义，把该索引
					 * 标为对那些快照不可用。
					 *
					 * 即使为最近死亡的元组建了索引，也不把它们计入 reltuples；
					 * 见 heapam_scan_analyze_next_tuple()。
					 */
					if (HeapTupleIsHotUpdated(heapTuple))
					{
						indexIt = false;
						/* 把索引标为对旧快照不安全 */
						indexInfo->ii_BrokenHotChain = true;
					}
					else
						indexIt = true;
					/* 无论如何，都不对该元组做唯一性检查 */
					tupleIsAlive = false;
					break;
				case HEAPTUPLE_INSERT_IN_PROGRESS:

					/*
					 * 在 "anyvisible" 模式下，该元组是可见的，
					 * 不需要进一步检查。
					 */
					if (anyvisible)
					{
						indexIt = true;
						tupleIsAlive = true;
						reltuples += 1;
						break;
					}

					/*
					 * 因为调用方应当持有 ShareLock 或更强的锁，通常
					 * 只有一种可能看到这种情况：它是在我们自己的事务里
					 * 更早插入的。不过系统目录里也可能发生，因为我们
					 * 往往在提交之前就释放那里的写锁。若两种情形
					 * 都不成立，
					 * 就发出警告。
					 */
					xwait = HeapTupleHeaderGetXmin(heapTuple->t_data);
					if (!TransactionIdIsCurrentTransactionId(xwait))
					{
						if (!is_system_catalog)
							elog(WARNING, "concurrent insert in progress within table \"%s\"",
								 RelationGetRelationName(heapRelation));

						/*
						 * 若我们正在做唯一性检查，为这样的元组
						 * 建索引可能导致虚假的唯一性
						 * 失败。这种情况下我们等待插入
						 * 事务结束，然后再检查一次。
						 */
						if (checking_uniqueness)
						{
							/*
							 * 等待之前必须先放下缓冲区上的锁
							 */
							LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
							XactLockTableWait(xwait, heapRelation,
											  &heapTuple->t_self,
											  XLTW_InsertIndexUnique);
							CHECK_FOR_INTERRUPTS();
							goto recheck;
						}
					}
					else
					{
						/*
						 * 为了与
						 * heapam_scan_analyze_next_tuple() 保持一致，仅当
						 * HEAPTUPLE_INSERT_IN_PROGRESS 元组是由我们自己的
						 * 事务插入时，才把它计为活的。
						 */
						reltuples += 1;
					}

					/*
					 * 我们必须为这些元组建索引，因为若索引构建
					 * 提交，它们就是有效的。
					 */
					indexIt = true;
					tupleIsAlive = true;
					break;
				case HEAPTUPLE_DELETE_IN_PROGRESS:

					/*
					 * 与 INSERT_IN_PROGRESS 情形一样，这是意外的，
					 * 除非是我们自己的删除，或发生在系统目录中；但在
					 * anyvisible 模式下，该元组是可见的。
					 */
					if (anyvisible)
					{
						indexIt = true;
						tupleIsAlive = false;
						reltuples += 1;
						break;
					}

					xwait = HeapTupleHeaderGetUpdateXid(heapTuple->t_data);
					if (!TransactionIdIsCurrentTransactionId(xwait))
					{
						if (!is_system_catalog)
							elog(WARNING, "concurrent delete in progress within table \"%s\"",
								 RelationGetRelationName(heapRelation));

						/*
						 * 若我们正在做唯一性检查，把该元组
						 * 假定为已死，可能导致漏掉一次
						 * 唯一性违反。这种情况下，我们等待
						 * 删除事务结束，然后再检查一次。
						 *
						 * 另外，若它是经过 HOT 更新的元组，不应当为它
						 * 建索引，而应当为 HOT 链末端的活元组建索引。
						 * 不过删除事务可能中止，最终仍可能让
						 * 这个元组成为活的，那时就必须为它建索引。
						 * 要知道该怎么做，唯一办法就是等待
						 * 删除事务结束，
						 * 然后再检查一次。
						 */
						if (checking_uniqueness ||
							HeapTupleIsHotUpdated(heapTuple))
						{
							/*
							 * 等待之前必须先放下缓冲区上的锁
							 */
							LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
							XactLockTableWait(xwait, heapRelation,
											  &heapTuple->t_self,
											  XLTW_InsertIndexUnique);
							CHECK_FOR_INTERRUPTS();
							goto recheck;
						}

						/*
						 * 否则就为它建索引，但不检查唯一性，
						 * 与 RECENTLY_DEAD 元组相同。
						 */
						indexIt = true;

						/*
						 * 把 HEAPTUPLE_DELETE_IN_PROGRESS 元组计为活的，
						 * 若它们不是由当前事务
						 * 删除的。heapam_scan_analyze_next_tuple()
						 * 就是这样做的，我们希望
						 * 行为保持一致。
						 */
						reltuples += 1;
					}
					else if (HeapTupleIsHotUpdated(heapTuple))
					{
						/*
						 * 这是由我们自己的事务删除的、经过 HOT 更新的元组。
						 * 可以假定删除会提交（否则索引内容
						 * 无关紧要），因此与 RECENTLY_DEAD 的、经过
						 * HOT 更新的元组同样处理。
						 */
						indexIt = false;
						/* 把索引标为对旧快照不安全 */
						indexInfo->ii_BrokenHotChain = true;
					}
					else
					{
						/*
						 * 这是由我们自己的事务删除的普通元组。为它
						 * 建索引，但不检查唯一性，也不计入
						 * reltuples，与 RECENTLY_DEAD 元组相同。
						 */
						indexIt = true;
					}
					/* 无论如何，都不对该元组做唯一性检查 */
					tupleIsAlive = false;
					break;
				default:
					elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
					indexIt = tupleIsAlive = false; /* 让编译器保持安静 */
					break;
			}

			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			if (!indexIt)
				continue;
		}
		else
		{
			/* heap_getnext 已经做过时间限定检查 */
			tupleIsAlive = true;
			reltuples += 1;
		}

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		/* 为谓词或表达式求值做准备 */
		ExecStoreBufferHeapTuple(heapTuple, slot, hscan->rs_cbuf);

		/*
		 * 在部分索引中，丢弃不满足
		 * 谓词的元组。
		 */
		if (predicate != NULL)
		{
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * 对当前堆元组，取出本索引使用的全部属性，
		 * 并记下哪些为空。这也会对所需的
		 * 任何表达式求值。
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/*
		 * 你也许以为应当在这里直接构造索引元组，但
		 * 有些索引 AM 想先对数据做进一步处理。因此
		 * 改为传入 values[] 与 isnull[] 数组。
		 */

		if (HeapTupleIsHeapOnly(heapTuple))
		{
			/*
			 * 对堆内元组，把它的 TID 假装成根的 TID。讨论见
			 * src/backend/access/heap/README.HOT。
			 */
			ItemPointerData tid;
			OffsetNumber offnum;

			offnum = ItemPointerGetOffsetNumber(&heapTuple->t_self);

			/*
			 * 若 HOT 元组指向一个我们不知道的根，
			 * 就重新取得根项。若仍然失败，就把它报告为
			 * 损坏。
			 */
			if (root_offsets[offnum - 1] == InvalidOffsetNumber)
			{
				Page		page = BufferGetPage(hscan->rs_cbuf);

				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);
				heap_get_root_tuples(page, root_offsets);
				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
			}

			if (!OffsetNumberIsValid(root_offsets[offnum - 1]))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("failed to find parent tuple for heap-only tuple at (%u,%u) in table \"%s\"",
										 ItemPointerGetBlockNumber(&heapTuple->t_self),
										 offnum,
										 RelationGetRelationName(heapRelation))));

			ItemPointerSet(&tid, ItemPointerGetBlockNumber(&heapTuple->t_self),
						   root_offsets[offnum - 1]);

			/* 调用该 AM 的回调例程来处理元组 */
			callback(indexRelation, &tid, values, isnull, tupleIsAlive,
					 callback_state);
		}
		else
		{
			/* 调用该 AM 的回调例程来处理元组 */
			callback(indexRelation, &heapTuple->t_self, values, isnull,
					 tupleIsAlive, callback_state);
		}
	}

	/* 最后再报告一次扫描进度。 */
	if (progress)
	{
		BlockNumber blks_done;

		if (hscan->rs_base.rs_parallel != NULL)
		{
			ParallelBlockTableScanDesc pbscan;

			pbscan = (ParallelBlockTableScanDesc) hscan->rs_base.rs_parallel;
			blks_done = pbscan->phs_nblocks;
		}
		else
			blks_done = hscan->rs_nblocks;

		pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
									 blks_done);
	}

	table_endscan(scan);

	/* 若快照是我们设置并注册的，现在可以忘掉它 */
	if (need_unregister_snapshot)
		UnregisterSnapshot(snapshot);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* 这些可能一直指向已经消失的 estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;

	return reltuples;
}

static void
heapam_index_validate_scan(Relation heapRelation,
						   Relation indexRelation,
						   IndexInfo *indexInfo,
						   Snapshot snapshot,
						   ValidateIndexState *state)
{
	TableScanDesc scan;
	HeapScanDesc hscan;
	HeapTuple	heapTuple;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	BlockNumber root_blkno = InvalidBlockNumber;
	OffsetNumber root_offsets[MaxHeapTuplesPerPage];
	bool		in_index[MaxHeapTuplesPerPage];
	BlockNumber previous_blkno = InvalidBlockNumber;

	/* 归并所用的状态变量 */
	ItemPointer indexcursor = NULL;
	ItemPointerData decoded;
	bool		tuplesort_empty = false;

	/*
	 * 健全性检查
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));

	/*
	 * 求值索引表达式和部分索引谓词需要一个 EState。
	 * 还需要一个槽来保存当前元组。
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRelation),
									&TTSOpsHeapTuple);

	/* 让 econtext 的扫描元组成为受测元组 */
	econtext->ecxt_scantuple = slot;

	/* 若有谓词，为其建立执行状态。 */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * 准备扫描基础关系。我们只需要满足传入的
	 * 参照快照的那些元组。这里必须禁用同步扫描，
	 * 因为我们必须从第零块向前读，才能与
	 * 已排序的 TIDs 对齐。
	 */
	scan = table_beginscan_strat(heapRelation,	/* 关系 */
								 snapshot,	/* 快照 */
								 0, /* 键的个数 */
								 NULL,	/* 扫描键 */
								 true,	/* 允许缓冲区访问策略 */
								 false);	/* 不允许同步扫描 */
	hscan = (HeapScanDesc) scan;

	pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL,
								 hscan->rs_nblocks);

	/*
	 * 扫描所有匹配该快照的元组。
	 */
	while ((heapTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		ItemPointer heapcursor = &heapTuple->t_self;
		ItemPointerData rootTuple;
		OffsetNumber root_offnum;

		CHECK_FOR_INTERRUPTS();

		state->htups += 1;

		if ((previous_blkno == InvalidBlockNumber) ||
			(hscan->rs_cblock != previous_blkno))
		{
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
										 hscan->rs_cblock);
			previous_blkno = hscan->rs_cblock;
		}

		/*
		 * 正如 table_index_build_scan 中的注释所说，堆内
		 * 元组应当按其根元组的 TIDs 来索引；因此当我们进入
		 * 新的堆页时，为该页建立根项偏移的映射。
		 *
		 * 这使与 tuplesort 输出做归并变得复杂：我们会
		 * 按偏移顺序访问活元组，但要与索引内容比较的根
		 * 偏移，顺序可能不同。因此也许得在 tuplesort
		 * 输出内部"回头看"，但只在当前页之内。我们的处理办法是
		 * 保留 bool 数组 in_index[]，标出当前页上所有
		 * 已经略过的 tuplesort 输出 TIDs。当我们
		 * 前进到新的堆页时，
		 * 在这里清空该数组。
		 */
		if (hscan->rs_cblock != root_blkno)
		{
			Page		page = BufferGetPage(hscan->rs_cbuf);

			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);
			heap_get_root_tuples(page, root_offsets);
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			memset(in_index, 0, sizeof(in_index));

			root_blkno = hscan->rs_cblock;
		}

		/* 把实际元组 TID 转换成根 TID */
		rootTuple = *heapcursor;
		root_offnum = ItemPointerGetOffsetNumber(heapcursor);

		if (HeapTupleIsHeapOnly(heapTuple))
		{
			root_offnum = root_offsets[root_offnum - 1];
			if (!OffsetNumberIsValid(root_offnum))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("failed to find parent tuple for heap-only tuple at (%u,%u) in table \"%s\"",
										 ItemPointerGetBlockNumber(heapcursor),
										 ItemPointerGetOffsetNumber(heapcursor),
										 RelationGetRelationName(heapRelation))));
			ItemPointerSetOffsetNumber(&rootTuple, root_offnum);
		}

		/*
		 * "归并"的做法是跳过索引元组，直到找到或越过
		 * 当前根元组。
		 */
		while (!tuplesort_empty &&
			   (!indexcursor ||
				ItemPointerCompare(indexcursor, &rootTuple) < 0))
		{
			Datum		ts_val;
			bool		ts_isnull;

			if (indexcursor)
			{
				/*
				 * 记住当前堆页上较早见过的索引项
				 */
				if (ItemPointerGetBlockNumber(indexcursor) == root_blkno)
					in_index[ItemPointerGetOffsetNumber(indexcursor) - 1] = true;
			}

			tuplesort_empty = !tuplesort_getdatum(state->tuplesort, true,
												  false, &ts_val, &ts_isnull,
												  NULL);
			Assert(tuplesort_empty || !ts_isnull);
			if (!tuplesort_empty)
			{
				itemptr_decode(&decoded, DatumGetInt64(ts_val));
				indexcursor = &decoded;
			}
			else
			{
				/* 保持整洁 */
				indexcursor = NULL;
			}
		}

		/*
		 * 若 tuplesort 已经越过，并且我们先前没有见过匹配，
		 * 则索引中缺少该元组，因此插入它。
		 */
		if ((tuplesort_empty ||
			 ItemPointerCompare(indexcursor, &rootTuple) > 0) &&
			!in_index[root_offnum - 1])
		{
			MemoryContextReset(econtext->ecxt_per_tuple_memory);

			/* 为谓词或表达式求值做准备 */
			ExecStoreHeapTuple(heapTuple, slot, false);

			/*
			 * 在部分索引中，丢弃不满足
			 * 谓词的元组。
			 */
			if (predicate != NULL)
			{
				if (!ExecQual(predicate, econtext))
					continue;
			}

			/*
			 * 对当前堆元组，取出我们在本索引中使用的全部
			 * 属性，并记下哪些为空。这也会对所需的
			 * 任何表达式求值。
			 */
			FormIndexDatum(indexInfo,
						   slot,
						   estate,
						   values,
						   isnull);

			/*
			 * 你也许以为应当在这里直接构造索引元组，
			 * 但有些索引 AM 想先对数据做进一步处理。
			 * 因此改为传入 values[] 与 isnull[] 数组。
			 */

			/*
			 * 若该元组已经提交死亡，你也许以为
			 * 可以省去唯一性检查，但有了 HOT 之后这不再成立，
			 * 因为这次插入实际上是整条 HOT 链上
			 * 唯一性检查的代理。也就是说，这里的
			 * 元组可能因为已经 HOT 更新过而是死的；若是这样，
			 * 更新事务不会认为自己应当插入索引项。
			 * 索引 AM 会检查整条 HOT 链，
			 * 并在确实存在冲突时
			 * 正确地发现它。
			 */

			index_insert(indexRelation,
						 values,
						 isnull,
						 &rootTuple,
						 heapRelation,
						 indexInfo->ii_Unique ?
						 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
						 false,
						 indexInfo);

			state->tups_inserted += 1;
		}
	}

	table_endscan(scan);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* 这些可能一直指向已经消失的 estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
}

/*
 * 返回本次扫描自开始以来已经读过的块数。
 * 这是给进度报告用的，并不要求完全
 * 精确：并行扫描中，工作进程可能正在并发读取
 * 比我们所报告的更靠前的块。
 */
static BlockNumber
heapam_scan_get_blocks_done(HeapScanDesc hscan)
{
	ParallelBlockTableScanDesc bpscan = NULL;
	BlockNumber startblock;
	BlockNumber blocks_done;

	if (hscan->rs_base.rs_parallel != NULL)
	{
		bpscan = (ParallelBlockTableScanDesc) hscan->rs_base.rs_parallel;
		startblock = bpscan->phs_startblock;
	}
	else
		startblock = hscan->rs_startblock;

	/*
	 * 若 startblock 不是零，可能已经绕过关系的
	 * 末端回到开头。
	 */
	if (hscan->rs_cblock > startblock)
		blocks_done = hscan->rs_cblock - startblock;
	else
	{
		BlockNumber nblocks;

		nblocks = bpscan != NULL ? bpscan->phs_nblocks : hscan->rs_nblocks;
		blocks_done = nblocks - startblock +
			hscan->rs_cblock;
	}

	return blocks_done;
}


/* ------------------------------------------------------------------------
 * 堆 AM 的杂项回调
 * ------------------------------------------------------------------------
 */

/*
 * 检查该表是否需要 TOAST 表。仅当同时满足时才需要：
 * （1）存在任何可 TOAST 的属性，并且（2）元组的最大长度
 * 可能超过 TOAST_TUPLE_THRESHOLD。（我们不想为
 * "f1 varchar(20)" 这类东西创建 TOAST 表。）
 */
static bool
heapam_relation_needs_toast_table(Relation rel)
{
	int32		data_length = 0;
	bool		maxlength_unknown = false;
	bool		has_toastable_attrs = false;
	TupleDesc	tupdesc = rel->rd_att;
	int32		tuple_length;
	int			i;

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		data_length = att_align_nominal(data_length, att->attalign);
		if (att->attlen > 0)
		{
			/* 定长类型从不可 TOAST */
			data_length += att->attlen;
		}
		else
		{
			int32		maxlen = type_maximum_size(att->atttypid,
												   att->atttypmod);

			if (maxlen < 0)
				maxlength_unknown = true;
			else
				data_length += maxlen;
			if (att->attstorage != TYPSTORAGE_PLAIN)
				has_toastable_attrs = true;
		}
	}
	if (!has_toastable_attrs)
		return false;			/* 没有需要 TOAST 的内容？ */
	if (maxlength_unknown)
		return true;			/* 有长度不限的属性吗？ */
	tuple_length = MAXALIGN(SizeofHeapTupleHeader +
							BITMAPLEN(tupdesc->natts)) +
		MAXALIGN(data_length);
	return (tuple_length > TOAST_TUPLE_THRESHOLD);
}

/*
 * 堆关系的 TOAST 表本身也只是堆关系。
 */
static Oid
heapam_relation_toast_am(Relation rel)
{
	return rel->rd_rel->relam;
}


/* ------------------------------------------------------------------------
 * 堆 AM 的规划器相关回调
 * ------------------------------------------------------------------------
 */

#define HEAP_OVERHEAD_BYTES_PER_TUPLE \
	(MAXALIGN(SizeofHeapTupleHeader) + sizeof(ItemIdData))
#define HEAP_USABLE_BYTES_PER_PAGE \
	(BLCKSZ - SizeOfPageHeaderData)

static void
heapam_estimate_rel_size(Relation rel, int32 *attr_widths,
						 BlockNumber *pages, double *tuples,
						 double *allvisfrac)
{
	table_block_relation_estimate_size(rel, attr_widths, pages,
									   tuples, allvisfrac,
									   HEAP_OVERHEAD_BYTES_PER_TUPLE,
									   HEAP_USABLE_BYTES_PER_PAGE);
}


/* ------------------------------------------------------------------------
 * 堆 AM 的执行器相关回调
 * ------------------------------------------------------------------------
 */

static bool
heapam_scan_bitmap_next_block(TableScanDesc scan,
							  TBMIterateResult *tbmres)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;
	BlockNumber block = tbmres->blockno;
	Buffer		buffer;
	Snapshot	snapshot;
	int			ntup;

	hscan->rs_cindex = 0;
	hscan->rs_ntuples = 0;

	/*
	 * 忽略任何我们认为已经超出关系末端的声称项。
	 * 扫描开始之后关系可能已被扩展（我们只持有
	 * AccessShareLock，而且可能是本后端自己的插入）。
	 * 不过在 SERIALIZABLE 隔离级别下不做这一优化，
	 * 因为我们必须检查索引能够到达的全部不可见
	 * 元组。
	 */
	if (!IsolationIsSerializable() && block >= hscan->rs_nblocks)
		return false;

	/*
	 * 取得目标堆页上的钉，换掉我们先前持有的任何钉。
	 */
	hscan->rs_cbuf = ReleaseAndReadBuffer(hscan->rs_cbuf,
										  scan->rs_rd,
										  block);
	hscan->rs_cblock = block;
	buffer = hscan->rs_cbuf;
	snapshot = scan->rs_snapshot;

	ntup = 0;

	/*
	 * 若可能，对整页做剪枝并修复碎片。
	 */
	heap_page_prune_opt(scan->rs_rd, buffer);

	/*
	 * 检查元组可见性时，必须持有缓冲区内容上的共享锁。
	 * 不过此后，只要我们仍持有缓冲区钉，已判定为
	 * 可见的元组就保证仍然有效。
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	/*
	 * 有损与非有损两种情形需要两套不同的策略。
	 */
	if (tbmres->ntuples >= 0)
	{
		/*
		 * 位图是非有损的，因此只查看 tbmres 里列出的
		 * 偏移；但必须跟随从每个这种偏移开始的
		 * 任何 HOT 链。
		 */
		int			curslot;

		for (curslot = 0; curslot < tbmres->ntuples; curslot++)
		{
			OffsetNumber offnum = tbmres->offsets[curslot];
			ItemPointerData tid;
			HeapTupleData heapTuple;

			ItemPointerSet(&tid, block, offnum);
			if (heap_hot_search_buffer(&tid, scan->rs_rd, buffer, snapshot,
									   &heapTuple, NULL, true))
				hscan->rs_vistuples[ntup++] = ItemPointerGetOffsetNumber(&tid);
		}
	}
	else
	{
		/*
		 * 位图是有损的，因此必须检查该页上的每个行指针。
		 * 但可以忽略 HOT 链，因为我们反正会检查每个元组。
		 */
		Page		page = BufferGetPage(buffer);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		OffsetNumber offnum;

		for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum = OffsetNumberNext(offnum))
		{
			ItemId		lp;
			HeapTupleData loctup;
			bool		valid;

			lp = PageGetItemId(page, offnum);
			if (!ItemIdIsNormal(lp))
				continue;
			loctup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
			loctup.t_len = ItemIdGetLength(lp);
			loctup.t_tableOid = scan->rs_rd->rd_id;
			ItemPointerSet(&loctup.t_self, block, offnum);
			valid = HeapTupleSatisfiesVisibility(&loctup, snapshot, buffer);
			if (valid)
			{
				hscan->rs_vistuples[ntup++] = offnum;
				PredicateLockTID(scan->rs_rd, &loctup.t_self, snapshot,
								 HeapTupleHeaderGetXmin(loctup.t_data));
			}
			HeapCheckForSerializableConflictOut(valid, scan->rs_rd, &loctup,
												buffer, snapshot);
		}
	}

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	Assert(ntup <= MaxHeapTuplesPerPage);
	hscan->rs_ntuples = ntup;

	return ntup > 0;
}

static bool
heapam_scan_bitmap_next_tuple(TableScanDesc scan,
							  TBMIterateResult *tbmres,
							  TupleTableSlot *slot)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;
	OffsetNumber targoffset;
	Page		page;
	ItemId		lp;

	/*
	 * 超出范围？若是，这一页上没有更多要看的了
	 */
	if (hscan->rs_cindex < 0 || hscan->rs_cindex >= hscan->rs_ntuples)
		return false;

	targoffset = hscan->rs_vistuples[hscan->rs_cindex];
	page = BufferGetPage(hscan->rs_cbuf);
	lp = PageGetItemId(page, targoffset);
	Assert(ItemIdIsNormal(lp));

	hscan->rs_ctup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	hscan->rs_ctup.t_len = ItemIdGetLength(lp);
	hscan->rs_ctup.t_tableOid = scan->rs_rd->rd_id;
	ItemPointerSet(&hscan->rs_ctup.t_self, hscan->rs_cblock, targoffset);

	pgstat_count_heap_fetch(scan->rs_rd);

	/*
	 * 让结果槽指向这个元组。注意该槽
	 * 会取得缓冲区上的钉。
	 */
	ExecStoreBufferHeapTuple(&hscan->rs_ctup,
							 slot,
							 hscan->rs_cbuf);

	hscan->rs_cindex++;

	return true;
}

static bool
heapam_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno;

	/* 若关系为空，立即返回假 */
	if (hscan->rs_nblocks == 0)
		return false;

	if (tsm->NextSampleBlock)
	{
		blockno = tsm->NextSampleBlock(scanstate, hscan->rs_nblocks);
		hscan->rs_cblock = blockno;
	}
	else
	{
		/* 正在顺序扫描表 */

		if (hscan->rs_cblock == InvalidBlockNumber)
		{
			Assert(!hscan->rs_inited);
			blockno = hscan->rs_startblock;
		}
		else
		{
			Assert(hscan->rs_inited);

			blockno = hscan->rs_cblock + 1;

			if (blockno >= hscan->rs_nblocks)
			{
				/* 绕回关系开头；开始时未必是 0 */
				blockno = 0;
			}

			/*
			 * 出于同步的目的，报告新的扫描位置。
			 *
			 * 注意：我们在检查扫描是否结束之前做这件事，以便
			 * 位置提示的最终状态回到该关系的
			 * 开头。这并不是严格必要的，但否则当你
			 * 多次运行同一查询时，每次调用的起始位置都会
			 * 稍微往回退一点，这会让人困惑。
			 * 不过一般来说，我们并不保证任何特定的顺序。
			 */
			if (scan->rs_flags & SO_ALLOW_SYNC)
				ss_report_location(scan->rs_rd, blockno);

			if (blockno == hscan->rs_startblock)
			{
				blockno = InvalidBlockNumber;
			}
		}
	}

	if (!BlockNumberIsValid(blockno))
	{
		if (BufferIsValid(hscan->rs_cbuf))
			ReleaseBuffer(hscan->rs_cbuf);
		hscan->rs_cbuf = InvalidBuffer;
		hscan->rs_cblock = InvalidBlockNumber;
		hscan->rs_inited = false;

		return false;
	}

	heapgetpage(scan, blockno);
	hscan->rs_inited = true;

	return true;
}

static bool
heapam_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
							  TupleTableSlot *slot)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno = hscan->rs_cblock;
	bool		pagemode = (scan->rs_flags & SO_ALLOW_PAGEMODE) != 0;

	Page		page;
	bool		all_visible;
	OffsetNumber maxoffset;

	/*
	 * 未使用页模式时，在元组可见性检查期间
	 * 必须锁定缓冲区。
	 */
	if (!pagemode)
		LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);

	page = (Page) BufferGetPage(hscan->rs_cbuf);
	all_visible = PageIsAllVisible(page) &&
		!scan->rs_snapshot->takenDuringRecovery;
	maxoffset = PageGetMaxOffsetNumber(page);

	for (;;)
	{
		OffsetNumber tupoffset;

		CHECK_FOR_INTERRUPTS();

		/* 询问表采样方法要检查本页上的哪些元组。 */
		tupoffset = tsm->NextSampleTuple(scanstate,
										 blockno,
										 maxoffset);

		if (OffsetNumberIsValid(tupoffset))
		{
			ItemId		itemid;
			bool		visible;
			HeapTuple	tuple = &(hscan->rs_ctup);

			/* 跳过无效的元组指针。 */
			itemid = PageGetItemId(page, tupoffset);
			if (!ItemIdIsNormal(itemid))
				continue;

			tuple->t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple->t_len = ItemIdGetLength(itemid);
			ItemPointerSet(&(tuple->t_self), blockno, tupoffset);


			if (all_visible)
				visible = true;
			else
				visible = SampleHeapTupleVisible(scan, hscan->rs_cbuf,
												 tuple, tupoffset);

			/* 在页模式下，heapgetpage 已经替我们做过这件事 */
			if (!pagemode)
				HeapCheckForSerializableConflictOut(visible, scan->rs_rd, tuple,
													hscan->rs_cbuf, scan->rs_snapshot);

			/* 尝试同一页上的下一个元组。 */
			if (!visible)
				continue;

			/* 找到可见元组，返回它。 */
			if (!pagemode)
				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			ExecStoreBufferHeapTuple(tuple, slot, hscan->rs_cbuf);

			/* 把成功取到的元组计入堆获取 */
			pgstat_count_heap_getnext(scan->rs_rd);

			return true;
		}
		else
		{
			/*
			 * 若执行到这里，说明本页上的项已经用尽，
			 * 该前进到下一页了。
			 */
			if (!pagemode)
				LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			ExecClearTuple(slot);
			return false;
		}
	}

	Assert(0);
}


/* ----------------------------------------------------------------------------
 *  上述函数的辅助函数。
 * ----------------------------------------------------------------------------
 */

/*
 * 重构并重写给定的元组
 *
 * 我们无法原样复制该元组，原因有几方面：
 *
 * 1. 我们希望挤掉任何已删除列的值，既为了
 * 节省空间，也避免边角情形下的失败。（例如
 * 新表可能没有 TOAST 表，因而无法存放已删除列的
 * 任何大值。）
 *
 * 2. 该元组对新表而言甚至可能不合法；目前
 * 已知这只会作为 ALTER TABLE
 * SET WITHOUT OIDS 的后续影响出现。
 *
 * 因此，必须由组成它的各个 Datum 来重构该元组。
 */
static void
reform_and_rewrite_tuple(HeapTuple tuple,
						 Relation OldHeap, Relation NewHeap,
						 Datum *values, bool *isnull, RewriteState rwstate)
{
	TupleDesc	oldTupDesc = RelationGetDescr(OldHeap);
	TupleDesc	newTupDesc = RelationGetDescr(NewHeap);
	HeapTuple	copiedTuple;
	int			i;

	heap_deform_tuple(tuple, oldTupDesc, values, isnull);

	/* 务必把任何已删除列置为空 */
	for (i = 0; i < newTupDesc->natts; i++)
	{
		if (TupleDescAttr(newTupDesc, i)->attisdropped)
			isnull[i] = true;
	}

	copiedTuple = heap_form_tuple(newTupDesc, values, isnull);

	/* 其余工作由堆重写模块完成 */
	rewrite_heap_tuple(rwstate, tuple, copiedTuple);

	heap_freetuple(copiedTuple);
}

/*
 * 检查该元组的可见性。
 */
static bool
SampleHeapTupleVisible(TableScanDesc scan, Buffer buffer,
					   HeapTuple tuple,
					   OffsetNumber tupoffset)
{
	HeapScanDesc hscan = (HeapScanDesc) scan;

	if (scan->rs_flags & SO_ALLOW_PAGEMODE)
	{
		/*
		 * 在一次一页模式下，heapgetpage() 已经做过可见性检查，
		 * 因此只需查看它留在 rs_vistuples[] 里的信息。
		 *
		 * 我们对已知有序的数组做二分查找。注意：若坚持让
		 * NextSampleTuple 按递增顺序选择元组，可以省一些工夫，
		 * 但还不清楚收益是否大到值得加上
		 * 这一限制。
		 */
		int			start = 0,
					end = hscan->rs_ntuples - 1;

		while (start <= end)
		{
			int			mid = (start + end) / 2;
			OffsetNumber curoffset = hscan->rs_vistuples[mid];

			if (tupoffset == curoffset)
				return true;
			else if (tupoffset < curoffset)
				end = mid - 1;
			else
				start = mid + 1;
		}

		return false;
	}
	else
	{
		/* 否则，就必须单独检查该元组。 */
		return HeapTupleSatisfiesVisibility(tuple, scan->rs_snapshot,
											buffer);
	}
}


/* ------------------------------------------------------------------------
 * 堆表访问方法的定义。
 * ------------------------------------------------------------------------
 */

static const TableAmRoutine heapam_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = heapam_slot_callbacks,

	.scan_begin = heap_beginscan,
	.scan_end = heap_endscan,
	.scan_rescan = heap_rescan,
	.scan_getnextslot = heap_getnextslot,

	.scan_set_tidrange = heap_set_tidrange,
	.scan_getnextslot_tidrange = heap_getnextslot_tidrange,

	.parallelscan_estimate = table_block_parallelscan_estimate,
	.parallelscan_initialize = table_block_parallelscan_initialize,
	.parallelscan_reinitialize = table_block_parallelscan_reinitialize,

	.index_fetch_begin = heapam_index_fetch_begin,
	.index_fetch_reset = heapam_index_fetch_reset,
	.index_fetch_end = heapam_index_fetch_end,
	.index_fetch_tuple = heapam_index_fetch_tuple,

	.tuple_insert = heapam_tuple_insert,
	.tuple_insert_speculative = heapam_tuple_insert_speculative,
	.tuple_complete_speculative = heapam_tuple_complete_speculative,
	.multi_insert = heap_multi_insert,
	.tuple_delete = heapam_tuple_delete,
	.tuple_update = heapam_tuple_update,
	.tuple_lock = heapam_tuple_lock,

	.tuple_fetch_row_version = heapam_fetch_row_version,
	.tuple_get_latest_tid = heap_get_latest_tid,
	.tuple_tid_valid = heapam_tuple_tid_valid,
	.tuple_satisfies_snapshot = heapam_tuple_satisfies_snapshot,
	.index_delete_tuples = heap_index_delete_tuples,

	.relation_set_new_filelocator = heapam_relation_set_new_filelocator,
	.relation_nontransactional_truncate = heapam_relation_nontransactional_truncate,
	.relation_copy_data = heapam_relation_copy_data,
	.relation_copy_for_cluster = heapam_relation_copy_for_cluster,
	.relation_vacuum = heap_vacuum_rel,
	.scan_analyze_next_block = heapam_scan_analyze_next_block,
	.scan_analyze_next_tuple = heapam_scan_analyze_next_tuple,
	.index_build_range_scan = heapam_index_build_range_scan,
	.index_validate_scan = heapam_index_validate_scan,

	.relation_size = table_block_relation_size,
	.relation_needs_toast_table = heapam_relation_needs_toast_table,
	.relation_toast_am = heapam_relation_toast_am,
	.relation_fetch_toast_slice = heap_fetch_toast_slice,

	.relation_estimate_size = heapam_estimate_rel_size,

	.scan_bitmap_next_block = heapam_scan_bitmap_next_block,
	.scan_bitmap_next_tuple = heapam_scan_bitmap_next_tuple,
	.scan_sample_next_block = heapam_scan_sample_next_block,
	.scan_sample_next_tuple = heapam_scan_sample_next_tuple
};


const TableAmRoutine *
GetHeapamTableAmRoutine(void)
{
	return &heapam_methods;
}

Datum
heap_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&heapam_methods);
}
