/*-------------------------------------------------------------------------
 *
 * heapam.h
 *	POSTGRES 堆访问方法定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/heapam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPAM_H
#define HEAPAM_H

#include "access/relation.h"	/* 为向后兼容 */
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/skey.h"
#include "access/table.h"		/* 为向后兼容 */
#include "access/tableam.h"
#include "nodes/lockoptions.h"
#include "nodes/primnodes.h"
#include "storage/bufpage.h"
#include "storage/dsm.h"
#include "storage/lockdefs.h"
#include "storage/shm_toc.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"


/* heap_insert 的"options"标志位 */
#define HEAP_INSERT_SKIP_FSM	TABLE_INSERT_SKIP_FSM
#define HEAP_INSERT_FROZEN		TABLE_INSERT_FROZEN
#define HEAP_INSERT_NO_LOGICAL	TABLE_INSERT_NO_LOGICAL
#define HEAP_INSERT_SPECULATIVE 0x0010

typedef struct BulkInsertStateData *BulkInsertState;
struct TupleTableSlot;
struct VacuumCutoffs;

#define MaxLockTupleMode	LockTupleExclusive

/*
 * 堆表扫描的描述符。
 */
typedef struct HeapScanDescData
{
	TableScanDescData rs_base;	/* 描述符中与 AM 无关的部分 */

	/* initscan 时建立的状态 */
	BlockNumber rs_nblocks;		/* 关系中的总块数 */
	BlockNumber rs_startblock;	/* 起始块号 */
	BlockNumber rs_numblocks;	/* 最多扫描的块数 */
	/* rs_numblocks 通常为 InvalidBlockNumber，即"扫描整个关系" */

	/* 扫描当前状态 */
	bool		rs_inited;		/* false = 扫描未初始化 */
	OffsetNumber rs_coffset;	/* 非逐页模式下的当前偏移号 */
	BlockNumber rs_cblock;		/* 扫描当前块号，如有 */
	Buffer		rs_cbuf;		/* 扫描当前缓冲区，如有 */
	/* 注意：若 rs_cbuf 非 InvalidBuffer，则已钉住该缓冲区 */

	BufferAccessStrategy rs_strategy;	/* 读取的访问策略 */

	HeapTupleData rs_ctup;		/* 扫描当前元组，如有 */

	/*
	 * 供并行扫描存放页面分配数据。非并行扫描时
	 * 为 NULL。
	 */
	ParallelBlockTableScanWorkerData *rs_parallelworkerdata;

	/* 以下字段仅用于逐页模式和位图扫描 */
	int			rs_cindex;		/* vistuples 中当前元组下标 */
	int			rs_ntuples;		/* 页上可见元组的数目 */
	OffsetNumber rs_vistuples[MaxHeapTuplesPerPage];	/* 其偏移号 */
}			HeapScanDescData;
typedef struct HeapScanDescData *HeapScanDesc;

/*
 * 经由索引从堆中取元组的描述符。
 */
typedef struct IndexFetchHeapData
{
	IndexFetchTableData xs_base;	/* 描述符中与 AM 无关的部分 */

	Buffer		xs_cbuf;		/* 扫描当前堆缓冲区，如有 */
	/* 注意：若 xs_cbuf 非 InvalidBuffer，则已钉住该缓冲区 */
} IndexFetchHeapData;

/* HeapTupleSatisfiesVacuum 的结果码 */
typedef enum
{
	HEAPTUPLE_DEAD,				/* 元组已死且可删除 */
	HEAPTUPLE_LIVE,				/* 元组存活，已提交无删除者 */
	HEAPTUPLE_RECENTLY_DEAD,	/* 元组已死，但尚不可删除 */
	HEAPTUPLE_INSERT_IN_PROGRESS,	/* 插入事务仍在进行 */
	HEAPTUPLE_DELETE_IN_PROGRESS	/* 删除事务仍在进行 */
} HTSV_Result;

/*
 * heap_prepare_freeze_tuple 可要求 heap_freeze_execute_prepared
 * 用 pg_xact 检查任一元组待冻结的 xmin 和/或 xmax 状态
 */
#define		HEAP_FREEZE_CHECK_XMIN_COMMITTED	0x01
#define		HEAP_FREEZE_CHECK_XMAX_ABORTED		0x02

/* heap_prepare_freeze_tuple 的状态，描述如何冻结元组 */
typedef struct HeapTupleFreeze
{
	/* 描述如何处理元组的字段 */
	TransactionId xmax;
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		frzflags;

	/* xmin/xmax检查标志 */
	uint8		checkflags;
	/* 元组的页内偏移号 */
	OffsetNumber offset;
} HeapTupleFreeze;

/*
 * 供 VACUUM 跟踪冻结某堆页上全部符合条件元组之
 * 细节的状态。
 *
 * VACUUM 经 heap_prepare_freeze_tuple 调用逐页准备冻结计划（
 * 每个带存储的元组各调用一次）。该页级冻结
 * 状态在每次调用间不断更新，最终决定是否必须
 * 冻结该页。
 *
 * 除是否冻结这一基本问题外，该状态还跟踪整张表中现存
 * 最旧的 XID/MXID，以便稍后推进 pg_class 中的
 * relfrozenxid/relminmxid 值。每次 heap_prepare_freeze_tuple 调用
 * 都会按需把 NewRelfrozenXid 和/或 NewRelminMxid 往回推，
 * 以免 pg_class 中最终值不安全。VACUUM 结束后仍留存的
 * 任何未冻结 XIDs 或 MXIDs，其值 _必须_ 不小于
 * pg_class 中最终的 relfrozenxid/relminmxid 值。这包括
 * 任一元组 xmax 中仍作 MultiXact 成员而保留的 XIDs。
 *
 * 检查完所有元组后，若未设置 'freeze_required' 标志，
 * 是否冻结由 vacuumlazy.c 作最终决定。它可按自认合适的
 * 任何标准触发冻结。不过，建议 vacuumlazy.c 在
 * 冻结后仍无法在可见性映射中把目标页设为全冻结时，
 * 避免提前冻结。
 */
typedef struct HeapPageFreeze
{
	/* heap_prepare_freeze_tuple 调用方必须冻结该页吗？ */
	bool		freeze_required;

	/*
	 * 冻结 NewRelfrozenXid/NewRelminMxid跟踪器。
	 *
	 * 在 heap_freeze_execute_prepared 执行冻结时，或某页
	 * 冻结计划数为零时使用的跟踪器。按定义，vacuumlazy.c
	 * 总可冻结任何页。这甚至包括没有需考虑的带存储
	 * 元组的页。这样，heap_prepare_freeze_tuple 得出的
	 * 'totally_frozen' 结果总可同样使用，即便无需执行
	 * 任何冻结计划来"冻结该页"。此方案下只有"冻结"
	 * 路径需要考虑是否要在可见性映射中把页设为
	 * 全冻结。
	 *
	 * 冻结一页时，通常冻结所有小于 OldestXmin 的 XID，只
	 * 留下不可冻结的 XID（若有）。因此你可能想问
	 * 这些跟踪器究竟为何必要；为何 VACUUM 冻结的
	 * _任何_ 页 _竟然_ 会留有 XIDs/MXIDs，而使
	 * 顶层 NewRelfrozenXid/NewRelminMxid 跟踪器回退？
	 *
	 * 采用不过度限定 MultiXacts 受影响的"冻结页"定义
	 * 是有益的。heap_prepare_freeze_tuple 通常倾向于尽早
	 * 移除 Multi，但若惰性处理能让 VACUUM 避免分配新
	 * Multi，则采用惰性处理。
	 * "冻结该页"跟踪器使该灵活性成为可能。
	 */
	TransactionId FreezePageRelfrozenXid;
	MultiXactId FreezePageRelminMxid;

	/*
	 * 不冻结 NewRelfrozenXid/NewRelminMxid跟踪器。
	 *
	 * 这些跟踪器的维护方式与 VACUUM 扫描未加清理锁的页时
	 * 所用跟踪器相同。两处代码基于同一总体思路：
	 * 在进行中的 VACUUM 里少为该页做些工作，代价是
	 * 不得不接受更旧的最终值。
	 */
	TransactionId NoFreezePageRelfrozenXid;
	MultiXactId NoFreezePageRelminMxid;

} HeapPageFreeze;

/* ----------------
 *		堆访问方法的函数原型
 *
 * heap_create、heap_create_with_catalog 和 heap_drop_with_catalog
 * 声明在 catalog/heap.h 中
 * ----------------
 */


/*
 * HeapScanIsValid
 *	仅当堆扫描有效时为真。
 */
#define HeapScanIsValid(scan) PointerIsValid(scan)

extern TableScanDesc heap_beginscan(Relation relation, Snapshot snapshot,
									int nkeys, ScanKey key,
									ParallelTableScanDesc parallel_scan,
									uint32 flags);
extern void heap_setscanlimits(TableScanDesc sscan, BlockNumber startBlk,
							   BlockNumber numBlks);
extern void heapgetpage(TableScanDesc sscan, BlockNumber block);
extern void heap_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
						bool allow_strat, bool allow_sync, bool allow_pagemode);
extern void heap_endscan(TableScanDesc sscan);
extern HeapTuple heap_getnext(TableScanDesc sscan, ScanDirection direction);
extern bool heap_getnextslot(TableScanDesc sscan,
							 ScanDirection direction, struct TupleTableSlot *slot);
extern void heap_set_tidrange(TableScanDesc sscan, ItemPointer mintid,
							  ItemPointer maxtid);
extern bool heap_getnextslot_tidrange(TableScanDesc sscan,
									  ScanDirection direction,
									  TupleTableSlot *slot);
extern bool heap_fetch(Relation relation, Snapshot snapshot,
					   HeapTuple tuple, Buffer *userbuf, bool keep_buf);
extern bool heap_hot_search_buffer(ItemPointer tid, Relation relation,
								   Buffer buffer, Snapshot snapshot, HeapTuple heapTuple,
								   bool *all_dead, bool first_call);

extern void heap_get_latest_tid(TableScanDesc sscan, ItemPointer tid);

extern BulkInsertState GetBulkInsertState(void);
extern void FreeBulkInsertState(BulkInsertState);
extern void ReleaseBulkInsertStatePin(BulkInsertState bistate);

extern void heap_insert(Relation relation, HeapTuple tup, CommandId cid,
						int options, BulkInsertState bistate);
extern void heap_multi_insert(Relation relation, struct TupleTableSlot **slots,
							  int ntuples, CommandId cid, int options,
							  BulkInsertState bistate);
extern TM_Result heap_delete(Relation relation, ItemPointer tid,
							 CommandId cid, Snapshot crosscheck, bool wait,
							 struct TM_FailureData *tmfd, bool changingPart);
extern void heap_finish_speculative(Relation relation, ItemPointer tid);
extern void heap_abort_speculative(Relation relation, ItemPointer tid);
extern TM_Result heap_update(Relation relation, ItemPointer otid,
							 HeapTuple newtup,
							 CommandId cid, Snapshot crosscheck, bool wait,
							 struct TM_FailureData *tmfd, LockTupleMode *lockmode,
							 TU_UpdateIndexes *update_indexes);
extern TM_Result heap_lock_tuple(Relation relation, HeapTuple tuple,
								 CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy,
								 bool follow_updates,
								 Buffer *buffer, struct TM_FailureData *tmfd);

extern bool heap_inplace_lock(Relation relation,
							  HeapTuple oldtup_ptr, Buffer buffer,
							  void (*release_callback) (void *), void *arg);
extern void heap_inplace_update_and_unlock(Relation relation,
										   HeapTuple oldtup, HeapTuple tuple,
										   Buffer buffer);
extern void heap_inplace_unlock(Relation relation,
								HeapTuple oldtup, Buffer buffer);
extern void heap_inplace_update(Relation relation, HeapTuple tuple);
extern bool heap_prepare_freeze_tuple(HeapTupleHeader tuple,
									  const struct VacuumCutoffs *cutoffs,
									  HeapPageFreeze *pagefrz,
									  HeapTupleFreeze *frz, bool *totally_frozen);
extern void heap_freeze_execute_prepared(Relation rel, Buffer buffer,
										 TransactionId snapshotConflictHorizon,
										 HeapTupleFreeze *tuples, int ntuples);
extern bool heap_freeze_tuple(HeapTupleHeader tuple,
							  TransactionId relfrozenxid, TransactionId relminmxid,
							  TransactionId FreezeLimit, TransactionId MultiXactCutoff);
extern bool heap_tuple_should_freeze(HeapTupleHeader tuple,
									 const struct VacuumCutoffs *cutoffs,
									 TransactionId *NoFreezePageRelfrozenXid,
									 MultiXactId *NoFreezePageRelminMxid);
extern bool heap_tuple_needs_eventual_freeze(HeapTupleHeader tuple);

extern void simple_heap_insert(Relation relation, HeapTuple tup);
extern void simple_heap_delete(Relation relation, ItemPointer tid);
extern void simple_heap_update(Relation relation, ItemPointer otid,
							   HeapTuple tup, TU_UpdateIndexes *update_indexes);

extern TransactionId heap_index_delete_tuples(Relation rel,
											  TM_IndexDeleteOp *delstate);

/* in heap/pruneheap.c */
struct GlobalVisState;
extern void heap_page_prune_opt(Relation relation, Buffer buffer);
extern int	heap_page_prune(Relation relation, Buffer buffer,
							TransactionId oldest_xmin,
							struct GlobalVisState *vistest,
							TransactionId old_snap_xmin,
							TimestampTz old_snap_ts,
							int *nnewlpdead,
							OffsetNumber *off_loc);
extern void heap_page_prune_execute(Buffer buffer,
									OffsetNumber *redirected, int nredirected,
									OffsetNumber *nowdead, int ndead,
									OffsetNumber *nowunused, int nunused);
extern void heap_get_root_tuples(Page page, OffsetNumber *root_offsets);

/* in heap/vacuumlazy.c */
struct VacuumParams;
extern void heap_vacuum_rel(Relation rel,
							struct VacuumParams *params, BufferAccessStrategy bstrategy);

/* in heap/heapam_visibility.c */
extern bool HeapTupleSatisfiesVisibility(HeapTuple htup, Snapshot snapshot,
										 Buffer buffer);
extern TM_Result HeapTupleSatisfiesUpdate(HeapTuple htup, CommandId curcid,
										  Buffer buffer);
extern HTSV_Result HeapTupleSatisfiesVacuum(HeapTuple htup, TransactionId OldestXmin,
											Buffer buffer);
extern HTSV_Result HeapTupleSatisfiesVacuumHorizon(HeapTuple htup, Buffer buffer,
												   TransactionId *dead_after);
extern void HeapTupleSetHintBits(HeapTupleHeader tuple, Buffer buffer,
								 uint16 infomask, TransactionId xid);
extern bool HeapTupleHeaderIsOnlyLocked(HeapTupleHeader tuple);
extern bool HeapTupleIsSurelyDead(HeapTuple htup,
								  struct GlobalVisState *vistest);

/*
 * 为避免过多泄露 reorderbuffer 实现细节的相关知识，
 * 这一实现位于 reorderbuffer.c，而非 heapam_visibility.c
 */
struct HTAB;
extern bool ResolveCminCmaxDuringDecoding(struct HTAB *tuplecid_data,
										  Snapshot snapshot,
										  HeapTuple htup,
										  Buffer buffer,
										  CommandId *cmin, CommandId *cmax);
extern void HeapCheckForSerializableConflictOut(bool visible, Relation relation, HeapTuple tuple,
												Buffer buffer, Snapshot snapshot);

#endif							/* HEAPAM_H */
