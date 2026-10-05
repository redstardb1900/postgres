/*-------------------------------------------------------------------------
 *
 * indexam.c
 *	  通用索引访问方法例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/index/indexam.c
 *
 * 接口例程（INTERFACE ROUTINES）
 *		index_open		- 按关系 OID 打开索引关系
 *		index_close		- 关闭索引关系
 *		index_beginscan - 用 amgettuple 开始索引扫描
 *		index_beginscan_bitmap - 用 amgetbitmap 开始索引扫描
 *		index_rescan	- 重新开始索引扫描
 *		index_endscan	- 结束扫描
 *		index_insert	- 向关系中插入索引元组
 *		index_markpos	- 标记扫描位置
 *		index_restrpos	- 恢复扫描位置
 *		index_parallelscan_estimate - 估算并行扫描的共享内存
 *		index_parallelscan_initialize - 初始化并行扫描
 *		index_parallelrescan  - （重新）开始索引的并行扫描
 *		index_beginscan_parallel - 加入并行索引扫描
 *		index_getnext_tid	- 从扫描取得下一个 TID
 *		index_fetch_heap		- 取得扫描的下一个堆元组
 *		index_getnext_slot	- 从扫描取得下一个元组
 *		index_getbitmap - 从扫描取得全部元组
 *		index_bulk_delete	- 批量删除索引元组
 *		index_vacuum_cleanup	- 删除之后清理索引
 *		index_can_return	- 索引是否支持仅索引扫描？
 *		index_getprocid - 取得支持过程 OID
 *		index_getprocinfo - 取得支持过程的查找信息
 *
 * 说明（NOTES）
 *		本文件包含原先散落在 access/genam 中的
 *		index_ 例程。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/heapam.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/* ----------------------------------------------------------------
 *					index_ 例程中使用的宏
 *
 * 注意：RELATION_CHECKS 中的 ReindexIsProcessingIndex() 检查，
 * 是为了确认我们不会去扫描，或向正在重建或等待
 * 重建的索引做逐条插入。这有助于抓住重建系统目录
 * 索引时行不通的事情，并防止用户错误，例如访问
 * 自身表的索引表达式。该检查并不阻止实际重建，
 * 因为调用索引 AM 的 ambuild 例程时我们不用
 * RELATION_CHECKS，而且 ambuild 没有理由通过本文件
 * 调用其子例程。
 * ----------------------------------------------------------------
 */
#define RELATION_CHECKS \
do { \
	Assert(RelationIsValid(indexRelation)); \
	Assert(PointerIsValid(indexRelation->rd_indam)); \
	if (unlikely(ReindexIsProcessingIndex(RelationGetRelid(indexRelation)))) \
		ereport(ERROR, \
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
				 errmsg("cannot access index \"%s\" while it is being reindexed", \
						RelationGetRelationName(indexRelation)))); \
} while(0)

#define SCAN_CHECKS \
( \
	AssertMacro(IndexScanIsValid(scan)), \
	AssertMacro(RelationIsValid(scan->indexRelation)), \
	AssertMacro(PointerIsValid(scan->indexRelation->rd_indam)) \
)

#define CHECK_REL_PROCEDURE(pname) \
do { \
	if (indexRelation->rd_indam->pname == NULL) \
		elog(ERROR, "function \"%s\" is not defined for index \"%s\"", \
			 CppAsString(pname), RelationGetRelationName(indexRelation)); \
} while(0)

#define CHECK_SCAN_PROCEDURE(pname) \
do { \
	if (scan->indexRelation->rd_indam->pname == NULL) \
		elog(ERROR, "function \"%s\" is not defined for index \"%s\"", \
			 CppAsString(pname), RelationGetRelationName(scan->indexRelation)); \
} while(0)

static IndexScanDesc index_beginscan_internal(Relation indexRelation,
											  int nkeys, int norderbys, Snapshot snapshot,
											  ParallelIndexScanDesc pscan, bool temp_snap);
static inline void validate_relation_kind(Relation r);


/* ----------------------------------------------------------------
 *				   index_ 接口函数
 * ----------------------------------------------------------------
 */

/* ----------------
 *		index_open - 按关系 OID 打开索引关系
 *
 *		若 lockmode 不是 "NoLock"，则在索引上取得
 *		指定种类的锁。（一般而言，只有调用方已知
 *		自己已经持有索引上某种适当的锁时，
 *		才应使用 NoLock。）
 *
 *		若索引不存在则报错。
 *
 *		这是为索引扫描使用而改写的便捷例程。
 *		有些调用方可能更愿意直接使用 relation_open。
 * ----------------
 */
Relation
index_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	r = relation_open(relationId, lockmode);

	validate_relation_kind(r);

	return r;
}

/* ----------------
 *		try_index_open - 按关系 OID 打开索引关系
 *
 *		与 index_open 相同，只是关系不存在时
 *		返回 NULL 而不是失败。
 * ----------------
 */
Relation
try_index_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	r = try_relation_open(relationId, lockmode);

	/* 若索引不存在则离开 */
	if (!r)
		return NULL;

	validate_relation_kind(r);

	return r;
}

/* ----------------
 *		index_close - 关闭索引关系
 *
 *		若 lockmode 不是 "NoLock"，则接着释放指定的锁。
 *
 *		注意：把锁保持到 index_close 之后常常是合理的；
 *		那种情况下，锁在事务结束时自动释放。
 * ----------------
 */
void
index_close(Relation relation, LOCKMODE lockmode)
{
	LockRelId	relid = relation->rd_lockInfo.lockRelId;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* relcache 做真正的工作... */
	RelationClose(relation);

	if (lockmode != NoLock)
		UnlockRelationId(&relid, lockmode);
}

/* ----------------
 *		validate_relation_kind - 检查关系的种类
 *
 *		确保 relkind 是索引或分区索引。
 * ----------------
 */
static inline void
validate_relation_kind(Relation r)
{
	if (r->rd_rel->relkind != RELKIND_INDEX &&
		r->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index",
						RelationGetRelationName(r))));
}


/* ----------------
 *		index_insert - 向关系中插入索引元组
 * ----------------
 */
bool
index_insert(Relation indexRelation,
			 Datum *values,
			 bool *isnull,
			 ItemPointer heap_t_ctid,
			 Relation heapRelation,
			 IndexUniqueCheck checkUnique,
			 bool indexUnchanged,
			 IndexInfo *indexInfo)
{
	RELATION_CHECKS;
	CHECK_REL_PROCEDURE(aminsert);

	if (!(indexRelation->rd_indam->ampredlocks))
		CheckForSerializableConflictIn(indexRelation,
									   (ItemPointer) NULL,
									   InvalidBlockNumber);

	return indexRelation->rd_indam->aminsert(indexRelation, values, isnull,
											 heap_t_ctid, heapRelation,
											 checkUnique, indexUnchanged,
											 indexInfo);
}

/*
 * index_beginscan - 用 amgettuple 开始索引扫描
 *
 * 调用方必须在堆和索引上持有合适的锁。
 */
IndexScanDesc
index_beginscan(Relation heapRelation,
				Relation indexRelation,
				Snapshot snapshot,
				int nkeys, int norderbys)
{
	IndexScanDesc scan;

	Assert(snapshot != InvalidSnapshot);

	scan = index_beginscan_internal(indexRelation, nkeys, norderbys, snapshot, NULL, false);

	/*
	 * 把额外参数存入 scandesc。其余都已由
	 * RelationGetIndexScan 设好。
	 */
	scan->heapRelation = heapRelation;
	scan->xs_snapshot = snapshot;

	/* 准备从表中取出索引匹配 */
	scan->xs_heapfetch = table_index_fetch_begin(heapRelation);

	return scan;
}

/*
 * index_beginscan_bitmap - 用 amgetbitmap 开始索引扫描
 *
 * 同上，调用方最好在父堆关系上持有某把锁，
 * 尽管这里没有明确提到它。
 */
IndexScanDesc
index_beginscan_bitmap(Relation indexRelation,
					   Snapshot snapshot,
					   int nkeys)
{
	IndexScanDesc scan;

	Assert(snapshot != InvalidSnapshot);

	scan = index_beginscan_internal(indexRelation, nkeys, 0, snapshot, NULL, false);

	/*
	 * 把额外参数存入 scandesc。其余都已由
	 * RelationGetIndexScan 设好。
	 */
	scan->xs_snapshot = snapshot;

	return scan;
}

/*
 * index_beginscan_internal --- index_beginscan 各变体的公共代码
 */
static IndexScanDesc
index_beginscan_internal(Relation indexRelation,
						 int nkeys, int norderbys, Snapshot snapshot,
						 ParallelIndexScanDesc pscan, bool temp_snap)
{
	IndexScanDesc scan;

	RELATION_CHECKS;
	CHECK_REL_PROCEDURE(ambeginscan);

	if (!(indexRelation->rd_indam->ampredlocks))
		PredicateLockRelation(indexRelation, snapshot);

	/*
	 * 整个扫描期间持有 relcache 项的引用计数。
	 */
	RelationIncrementReferenceCount(indexRelation);

	/*
	 * 让 AM 打开一次扫描。
	 */
	scan = indexRelation->rd_indam->ambeginscan(indexRelation, nkeys,
												norderbys);
	/* 初始化并行扫描的信息。 */
	scan->parallel_scan = pscan;
	scan->xs_temp_snap = temp_snap;

	return scan;
}

/* ----------------
 *		index_rescan  - （重新）开始索引扫描
 *
 * 重新开始时，调用方可以指定一套新的 scankeys 和/或
 * orderbykeys；但键的数目必须与 index_beginscan
 * 当初所知的相同。（以后也许放宽为 "不得超出"，但目前
 * 索引 AM 倾向于相信 scan->numberOfKeys。）
 * 要不改键就重新开始扫描，给键数组传 NULL。
 * （当然，除非 scan->numberOfKeys 为零，否则第一次
 * 调用 *必须* 传入键。）
 * ----------------
 */
void
index_rescan(IndexScanDesc scan,
			 ScanKey keys, int nkeys,
			 ScanKey orderbys, int norderbys)
{
	SCAN_CHECKS;
	CHECK_SCAN_PROCEDURE(amrescan);

	Assert(nkeys == scan->numberOfKeys);
	Assert(norderbys == scan->numberOfOrderBys);

	/* 释放表访问留下的资源（例如缓冲区钉住） */
	if (scan->xs_heapfetch)
		table_index_fetch_reset(scan->xs_heapfetch);

	scan->kill_prior_tuple = false; /* 为安全起见 */
	scan->xs_heap_continue = false;

	scan->indexRelation->rd_indam->amrescan(scan, keys, nkeys,
											orderbys, norderbys);
}

/* ----------------
 *		index_endscan - 结束扫描
 * ----------------
 */
void
index_endscan(IndexScanDesc scan)
{
	SCAN_CHECKS;
	CHECK_SCAN_PROCEDURE(amendscan);

	/* 释放表访问留下的资源（例如缓冲区钉住） */
	if (scan->xs_heapfetch)
	{
		table_index_fetch_end(scan->xs_heapfetch);
		scan->xs_heapfetch = NULL;
	}

	/* 结束 AM 的扫描 */
	scan->indexRelation->rd_indam->amendscan(scan);

	/* 释放 index_beginscan 取得的索引引用计数 */
	RelationDecrementReferenceCount(scan->indexRelation);

	if (scan->xs_temp_snap)
		UnregisterSnapshot(scan->xs_snapshot);

	/* 释放扫描数据结构本身 */
	IndexScanEnd(scan);
}

/* ----------------
 *		index_markpos  - 标记扫描位置
 * ----------------
 */
void
index_markpos(IndexScanDesc scan)
{
	SCAN_CHECKS;
	CHECK_SCAN_PROCEDURE(ammarkpos);

	scan->indexRelation->rd_indam->ammarkpos(scan);
}

/* ----------------
 *		index_restrpos	- 恢复扫描位置
 *
 * 注意：这只恢复索引 AM 的内部扫描状态。见
 * ExecRestrPos() 的注释。
 *
 * 注意：对堆而言，存在 HOT 链时，标记/恢复只有在
 * 扫描快照是 MVCC 安全的时候才正确；这保证每条 HOT 链
 * 最多有一个可返回的元组，因此按索引 AM 的粒度
 * 恢复先前状态就够了。由于标记/恢复目前唯一的
 * 用户是 nodeMergejoin.c，这实际上意味着归并连接计划
 * 只适用于 MVCC 快照。必要时可以修，
 * 但眼下似乎并不重要。
 * ----------------
 */
void
index_restrpos(IndexScanDesc scan)
{
	Assert(IsMVCCSnapshot(scan->xs_snapshot));

	SCAN_CHECKS;
	CHECK_SCAN_PROCEDURE(amrestrpos);

	/* 释放表访问留下的资源（例如缓冲区钉住） */
	if (scan->xs_heapfetch)
		table_index_fetch_reset(scan->xs_heapfetch);

	scan->kill_prior_tuple = false; /* 为安全起见 */
	scan->xs_heap_continue = false;

	scan->indexRelation->rd_indam->amrestrpos(scan);
}

/*
 * index_parallelscan_estimate - 估算并行扫描的共享内存
 *
 * 目前我们不向 AM 专用的估算器传递任何信息，
 * 因此它大概只能返回一个常量。将来也许需要
 * 传递更多信息。
 */
Size
index_parallelscan_estimate(Relation indexRelation, Snapshot snapshot)
{
	Size		nbytes;

	Assert(snapshot != InvalidSnapshot);

	RELATION_CHECKS;

	nbytes = offsetof(ParallelIndexScanDescData, ps_snapshot_data);
	nbytes = add_size(nbytes, EstimateSnapshotSpace(snapshot));
	nbytes = MAXALIGN(nbytes);

	/*
	 * 若未提供 amestimateparallelscan，则假定不需要
	 * AM 专用数据。（很难相信那样能工作，
	 * 但在这里迁就它很容易。）
	 */
	if (indexRelation->rd_indam->amestimateparallelscan != NULL)
		nbytes = add_size(nbytes,
						  indexRelation->rd_indam->amestimateparallelscan());

	return nbytes;
}

/*
 * index_parallelscan_initialize - 初始化并行扫描
 *
 * 我们既初始化 ParallelIndexScanDesc 本身，也初始化
 * 紧随其后的 AM 专用信息。
 *
 * 本函数调用访问方法专用的初始化例程，以初始化
 * am 专用信息。只在领导者进程中调用一次；
 * 然后各个工作进程通过 index_beginscan_parallel 接入。
 */
void
index_parallelscan_initialize(Relation heapRelation, Relation indexRelation,
							  Snapshot snapshot, ParallelIndexScanDesc target)
{
	Size		offset;

	Assert(snapshot != InvalidSnapshot);

	RELATION_CHECKS;

	offset = add_size(offsetof(ParallelIndexScanDescData, ps_snapshot_data),
					  EstimateSnapshotSpace(snapshot));
	offset = MAXALIGN(offset);

	target->ps_relid = RelationGetRelid(heapRelation);
	target->ps_indexid = RelationGetRelid(indexRelation);
	target->ps_offset = offset;
	SerializeSnapshot(snapshot, target->ps_snapshot_data);

	/* aminitparallelscan 可选；AM 未提供则假定为空操作 */
	if (indexRelation->rd_indam->aminitparallelscan != NULL)
	{
		void	   *amtarget;

		amtarget = OffsetToPointer(target, offset);
		indexRelation->rd_indam->aminitparallelscan(amtarget);
	}
}

/* ----------------
 *		index_parallelrescan  - （重新）开始索引的并行扫描
 * ----------------
 */
void
index_parallelrescan(IndexScanDesc scan)
{
	SCAN_CHECKS;

	if (scan->xs_heapfetch)
		table_index_fetch_reset(scan->xs_heapfetch);

	/* amparallelrescan 可选；AM 未提供则假定为空操作 */
	if (scan->indexRelation->rd_indam->amparallelrescan != NULL)
		scan->indexRelation->rd_indam->amparallelrescan(scan);
}

/*
 * index_beginscan_parallel - 加入并行索引扫描
 *
 * 调用方必须在堆和索引上持有合适的锁。
 */
IndexScanDesc
index_beginscan_parallel(Relation heaprel, Relation indexrel, int nkeys,
						 int norderbys, ParallelIndexScanDesc pscan)
{
	Snapshot	snapshot;
	IndexScanDesc scan;

	Assert(RelationGetRelid(heaprel) == pscan->ps_relid);
	snapshot = RestoreSnapshot(pscan->ps_snapshot_data);
	RegisterSnapshot(snapshot);
	scan = index_beginscan_internal(indexrel, nkeys, norderbys, snapshot,
									pscan, true);

	/*
	 * 把额外参数存入 scandesc。其余都已由
	 * index_beginscan_internal 设好。
	 */
	scan->heapRelation = heaprel;
	scan->xs_snapshot = snapshot;

	/* 准备从表中取出索引匹配 */
	scan->xs_heapfetch = table_index_fetch_begin(heaprel);

	return scan;
}

/* ----------------
 * index_getnext_tid - 从扫描取得下一个 TID
 *
 * 结果是满足扫描键的下一个 TID，
 * 若不再有匹配元组则为 NULL。
 * ----------------
 */
ItemPointer
index_getnext_tid(IndexScanDesc scan, ScanDirection direction)
{
	bool		found;

	SCAN_CHECKS;
	CHECK_SCAN_PROCEDURE(amgettuple);

	/* XXX：应当断言已压入或已登记快照 */
	Assert(TransactionIdIsValid(RecentXmin));

	/*
	 * AM 的 amgettuple 过程找出下一个匹配扫描键的索引项，
	 * 并把 TID 放入 scan->xs_heaptid。它还应设置 scan->xs_recheck，
	 * 并可能设置 scan->xs_itup/scan->xs_hitup，不过我们在这里
	 * 不关注那些字段。
	 */
	found = scan->indexRelation->rd_indam->amgettuple(scan, direction);

	/* 为安全起见立即复位杀死标志 */
	scan->kill_prior_tuple = false;
	scan->xs_heap_continue = false;

	/* 若索引项已用尽，则结束 */
	if (!found)
	{
		/* 释放表访问留下的资源（例如缓冲区钉住） */
		if (scan->xs_heapfetch)
			table_index_fetch_reset(scan->xs_heapfetch);

		return NULL;
	}
	Assert(ItemPointerIsValid(&scan->xs_heaptid));

	pgstat_count_index_tuples(scan->indexRelation, 1);

	/* 返回找到的元组的 TID。 */
	return &scan->xs_heaptid;
}

/* ----------------
 *		index_fetch_heap - 取得扫描的下一个堆元组
 *
 * 结果是与 index_getnext_tid 最近取出的索引 TID
 * 相关联的可见堆元组，若不再有匹配元组则为 NULL。
 * （由于 HOT 链，可能有多于一个匹配元组，
 * 不过使用 MVCC 快照时，不可能存在多于
 * 一个这样的元组。）
 *
 * 成功时，含有该堆元组的缓冲区被钉住（该钉住将在
 * 将来的 index_getnext_tid、index_fetch_heap 或
 * index_endscan 调用中解除）。
 *
 * 注意：调用方必须检查 scan->xs_recheck，并在需要时
 * 重查扫描键。我们不在这里做，因为一般情况下
 * 没有足够信息可以高效地做。
 * ----------------
 */
bool
index_fetch_heap(IndexScanDesc scan, TupleTableSlot *slot)
{
	bool		all_dead = false;
	bool		found;

	found = table_index_fetch_tuple(scan->xs_heapfetch, &scan->xs_heaptid,
									scan->xs_snapshot, slot,
									&scan->xs_heap_continue, &all_dead);

	if (found)
		pgstat_count_heap_fetch(scan->indexRelation);

	/*
	 * 若扫完整条 HOT 链却只找到死元组，则告诉索引
	 * AM 杀死该 TID 的项（这将在下一次 amgettuple 调用中
	 * 生效，位于 index_getnext_tid）。恢复期间不这样做，
	 * 因为那样可能破坏 MVCC。见
	 * RelationGetIndexScan() 中的注释。
	 */
	if (!scan->xactStartedInRecovery)
		scan->kill_prior_tuple = all_dead;

	return found;
}

/* ----------------
 *		index_getnext_slot - 从扫描取得下一个元组
 *
 * 若找到满足扫描键和快照的元组则结果为真，
 * 否则为假。元组存放在指定的槽中。
 *
 * 成功时，多半会持有资源（例如缓冲区钉住），并将由
 * 将来的 index_getnext_tid、index_fetch_heap 或 index_endscan
 * 调用解除。
 *
 * 注意：调用方必须检查 scan->xs_recheck，并在需要时
 * 重查扫描键。我们不在这里做，因为一般情况下
 * 没有足够信息可以高效地做。
 * ----------------
 */
bool
index_getnext_slot(IndexScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	for (;;)
	{
		if (!scan->xs_heap_continue)
		{
			ItemPointer tid;

			/* 该从索引取出下一个 TID 了 */
			tid = index_getnext_tid(scan, direction);

			/* 若索引项已用尽，则结束 */
			if (tid == NULL)
				break;

			Assert(ItemPointerEquals(tid, &scan->xs_heaptid));
		}

		/*
		 * 为这个索引项取出下一个（或仅有的）可见堆元组。
		 * 若什么都没找到，则循环回去，从索引抓取
		 * 下一个 TID。
		 */
		Assert(ItemPointerIsValid(&scan->xs_heaptid));
		if (index_fetch_heap(scan, slot))
			return true;
	}

	return false;
}

/* ----------------
 *		index_getbitmap - 从索引扫描一次取得全部元组
 *
 * 把满足扫描键的全部堆元组的 TIDs 加入位图。
 * 由于索引扫描与最终的堆访问之间没有互锁，
 * 这只对基于 MVCC 的快照安全：等到我们
 * 访问堆项槽时，它可能已被更新的元组
 * 替换。
 *
 * 返回找到的匹配元组数。（注意：这可能只是
 * 近似值，因此只应用于统计目的。）
 * ----------------
 */
int64
index_getbitmap(IndexScanDesc scan, TIDBitmap *bitmap)
{
	int64		ntids;

	SCAN_CHECKS;
	CHECK_SCAN_PROCEDURE(amgetbitmap);

	/* 只是确保它为假... */
	scan->kill_prior_tuple = false;

	/*
	 * 让 am 的 getbitmap 过程做全部工作。
	 */
	ntids = scan->indexRelation->rd_indam->amgetbitmap(scan, bitmap);

	pgstat_count_index_tuples(scan->indexRelation, ntids);

	return ntids;
}

/* ----------------
 *		index_bulk_delete - 批量删除索引项
 *
 *		回调例程告知给定的主堆元组
 *		是否应删除
 *
 *		返回值是可选的、palloc 出来的统计信息结构
 * ----------------
 */
IndexBulkDeleteResult *
index_bulk_delete(IndexVacuumInfo *info,
				  IndexBulkDeleteResult *istat,
				  IndexBulkDeleteCallback callback,
				  void *callback_state)
{
	Relation	indexRelation = info->index;

	RELATION_CHECKS;
	CHECK_REL_PROCEDURE(ambulkdelete);

	return indexRelation->rd_indam->ambulkdelete(info, istat,
												 callback, callback_state);
}

/* ----------------
 *		index_vacuum_cleanup - 删除之后清理索引
 *
 *		返回值是可选的、palloc 出来的统计信息结构
 * ----------------
 */
IndexBulkDeleteResult *
index_vacuum_cleanup(IndexVacuumInfo *info,
					 IndexBulkDeleteResult *istat)
{
	Relation	indexRelation = info->index;

	RELATION_CHECKS;
	CHECK_REL_PROCEDURE(amvacuumcleanup);

	return indexRelation->rd_indam->amvacuumcleanup(info, istat);
}

/* ----------------
 *		index_can_return
 *
 *		索引访问方法是否支持给定列的
 *		仅索引扫描？
 * ----------------
 */
bool
index_can_return(Relation indexRelation, int attno)
{
	RELATION_CHECKS;

	/* amcanreturn 可选；AM 未提供则假定为假 */
	if (indexRelation->rd_indam->amcanreturn == NULL)
		return false;

	return indexRelation->rd_indam->amcanreturn(indexRelation, attno);
}

/* ----------------
 *		index_getprocid
 *
 *		索引访问方法通常需要一些支持例程，它们
 *		并不直接实现任何 WHERE 子句的查询操作符 operator(s)，
 *		因此无法放在 pg_amop 中。这类例程改放在
 *		pg_amproc 中。这些已登记的过程 OIDs
 *		按访问方法确立的约定编号。
 *		通用索引代码对涉及的例程一无所知；
 *		它只是为定义了索引的每个属性
 *		建立它们的有序列表。
 *
 *		自 Postgres 8.3 起，操作符族内的支持例程
 *		进一步按它们所支持的查询操作符的 "left type"
 *		与 "right type" 细分。特定被索引属性的
 *		"default" 函数，是两种类型都等于索引操作符类
 *		opcintype 的那些（注意这与被索引属性自身的类型
 *		有细微差别：它可能是二进制兼容类型）。
 *		只有默认函数存放在 relcache 项中 ---
 *		访问方法可用 syscache 查找非默认
 *		函数。
 *
 *		本例程返回特定被索引属性所请求的
 *		默认过程 OID。
 * ----------------
 */
RegProcedure
index_getprocid(Relation irel,
				AttrNumber attnum,
				uint16 procnum)
{
	RegProcedure *loc;
	int			nproc;
	int			procindex;

	nproc = irel->rd_indam->amsupport;

	Assert(procnum > 0 && procnum <= (uint16) nproc);

	procindex = (nproc * (attnum - 1)) + (procnum - 1);

	loc = irel->rd_support;

	Assert(loc != NULL);

	return loc[procindex];
}

/* ----------------
 *		index_getprocinfo
 *
 *		本例程让索引 AM 把支持过程的 fmgr 查找信息
 *		留在 relcache 中。同上，只有 "default" 函数
 *		会被缓存，针对任何特定被索引属性。
 *
 * 注意：返回值指向缓存数据，任何 relcache 重建都会丢失它！
 * 因此要么立刻使用 callinfo，要么只在已取得索引关系上
 * 某种锁之后再保存它。
 * ----------------
 */
FmgrInfo *
index_getprocinfo(Relation irel,
				  AttrNumber attnum,
				  uint16 procnum)
{
	FmgrInfo   *locinfo;
	int			nproc;
	int			optsproc;
	int			procindex;

	nproc = irel->rd_indam->amsupport;
	optsproc = irel->rd_indam->amoptsprocnum;

	Assert(procnum > 0 && procnum <= (uint16) nproc);

	procindex = (nproc * (attnum - 1)) + (procnum - 1);

	locinfo = irel->rd_supportinfo;

	Assert(locinfo != NULL);

	locinfo += procindex;

	/* 若是第一次经过，则初始化查找信息 */
	if (locinfo->fn_oid == InvalidOid)
	{
		RegProcedure *loc = irel->rd_support;
		RegProcedure procId;

		Assert(loc != NULL);

		procId = loc[procindex];

		/*
		 * 若 IndexSupportInitialize 期间未找到函数则报错。
		 * 除非系统表含有该索引操作符类的虚假项，
		 * 否则不该发生。（若 AM 想允许支持函数
		 * 可选，可以使用 index_getprocid。）
		 */
		if (!RegProcedureIsValid(procId))
			elog(ERROR, "missing support function %d for attribute %d of index \"%s\"",
				 procnum, attnum, RelationGetRelationName(irel));

		fmgr_info_cxt(procId, locinfo, irel->rd_indexcxt);

		if (procnum != optsproc)
		{
			/* 用操作符类选项 Const 初始化 locinfo->fn_expr */
			bytea	  **attoptions = RelationGetIndexAttOptions(irel, false);
			MemoryContext oldcxt = MemoryContextSwitchTo(irel->rd_indexcxt);

			set_fn_opclass_options(locinfo, attoptions[attnum - 1]);

			MemoryContextSwitchTo(oldcxt);
		}
	}

	return locinfo;
}

/* ----------------
 *		index_store_float8_orderby_distances
 *
 *		把 AM 距离函数的结果（可能不精确）
 *		转换成 ORDER BY 类型，并存入
 *		xs_orderbyvals/xs_orderbynulls，以便可能的重查。
 * ----------------
 */
void
index_store_float8_orderby_distances(IndexScanDesc scan, Oid *orderByTypes,
									 IndexOrderByDistance *distances,
									 bool recheckOrderBy)
{
	int			i;

	Assert(distances || !recheckOrderBy);

	scan->xs_recheckorderby = recheckOrderBy;

	for (i = 0; i < scan->numberOfOrderBys; i++)
	{
		if (orderByTypes[i] == FLOAT8OID)
		{
#ifndef USE_FLOAT8_BYVAL
			/* 必须释放任何旧值，以免内存泄漏 */
			if (!scan->xs_orderbynulls[i])
				pfree(DatumGetPointer(scan->xs_orderbyvals[i]));
#endif
			if (distances && !distances[i].isnull)
			{
				scan->xs_orderbyvals[i] = Float8GetDatum(distances[i].value);
				scan->xs_orderbynulls[i] = false;
			}
			else
			{
				scan->xs_orderbyvals[i] = (Datum) 0;
				scan->xs_orderbynulls[i] = true;
			}
		}
		else if (orderByTypes[i] == FLOAT4OID)
		{
			/* 把距离函数的结果转换成 ORDER BY 类型 */
			if (distances && !distances[i].isnull)
			{
				scan->xs_orderbyvals[i] = Float4GetDatum((float4) distances[i].value);
				scan->xs_orderbynulls[i] = false;
			}
			else
			{
				scan->xs_orderbyvals[i] = (Datum) 0;
				scan->xs_orderbynulls[i] = true;
			}
		}
		else
		{
			/*
			 * 若排序操作符的返回值是其他类型，我们
			 * 不知道如何把距离函数算出的 float8 界限
			 * 转换成那个类型。若没有有损结果，执行器实际上
			 * 并不需要我们这里返回的 order by 值，因此
			 * 只在设置了 *recheck 标志时
			 * 才坚持转换。
			 */
			if (scan->xs_recheckorderby)
				elog(ERROR, "ORDER BY operator must return float8 or float4 if the distance function is lossy");
			scan->xs_orderbynulls[i] = true;
		}
	}
}

/* ----------------
 *      index_opclass_options
 *
 *      解析索引列的操作符类专用选项。
 * ----------------
 */
bytea *
index_opclass_options(Relation indrel, AttrNumber attnum, Datum attoptions,
					  bool validate)
{
	int			amoptsprocnum = indrel->rd_indam->amoptsprocnum;
	Oid			procid = InvalidOid;
	FmgrInfo   *procinfo;
	local_relopts relopts;

	/* 若已指定，则取出选项支持过程 */
	if (amoptsprocnum != 0)
		procid = index_getprocid(indrel, attnum, amoptsprocnum);

	if (!OidIsValid(procid))
	{
		Oid			opclass;
		Datum		indclassDatum;
		oidvector  *indclass;

		if (!DatumGetPointer(attoptions))
			return NULL;		/* 好，没有选项，也没有过程 */

		/*
		 * 若操作符类的选项解析过程不存在，
		 * 但指定了操作符类选项，则报错。
		 */
		indclassDatum = SysCacheGetAttrNotNull(INDEXRELID, indrel->rd_indextuple,
											   Anum_pg_index_indclass);
		indclass = (oidvector *) DatumGetPointer(indclassDatum);
		opclass = indclass->values[attnum - 1];

		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("operator class %s has no options",
						generate_opclass_name(opclass))));
	}

	init_local_reloptions(&relopts, 0);

	procinfo = index_getprocinfo(indrel, attnum, amoptsprocnum);

	(void) FunctionCall1(procinfo, PointerGetDatum(&relopts));

	return build_local_reloptions(&relopts, attoptions, validate);
}
