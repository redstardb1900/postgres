/*-------------------------------------------------------------------------
 *
 * snapmgr.h
 *	  POSTGRES 快照管理器
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/snapmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SNAPMGR_H
#define SNAPMGR_H

#include "access/transam.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/snapshot.h"


/*
 * The structure used to map times to TransactionId values for the "snapshot
 * too old" feature must have a few entries at the tail to hold old values;
 * otherwise the lookup will often fail and the expected early pruning or
 * vacuum will not usually occur.  It is best if this padding is for a number
 * of minutes greater than a thread would normally be stalled, but it's OK if
 * early vacuum opportunities are occasionally missed, so there's no need to
 * use an extreme value or get too fancy.  10 minutes seems plenty.
 */
#define OLD_SNAPSHOT_PADDING_ENTRIES 10
#define OLD_SNAPSHOT_TIME_MAP_ENTRIES (old_snapshot_threshold + OLD_SNAPSHOT_PADDING_ENTRIES)

/*
 * 允许提前裁剪/vacuum 的关系属性通用定义，当
 * old_snapshot_threshold >= 0。
 */
#define RelationAllowsEarlyPruning(rel) \
( \
	 RelationIsPermanent(rel) && !IsCatalogRelation(rel) \
  && !RelationIsAccessibleInLogicalDecoding(rel) \
)

#define EarlyPruningEnabled(rel) (old_snapshot_threshold >= 0 && RelationAllowsEarlyPruning(rel))

/* GUC 变量 */
extern PGDLLIMPORT int old_snapshot_threshold;


extern Size SnapMgrShmemSize(void);
extern void SnapMgrInit(void);
extern TimestampTz GetSnapshotCurrentTimestamp(void);
extern TimestampTz GetOldSnapshotThresholdTimestamp(void);
extern void SnapshotTooOldMagicForTest(void);

extern PGDLLIMPORT bool FirstSnapshotSet;

extern PGDLLIMPORT TransactionId TransactionXmin;
extern PGDLLIMPORT TransactionId RecentXmin;

/* 表示各种特殊快照语义的变量 */
extern PGDLLIMPORT SnapshotData SnapshotSelfData;
extern PGDLLIMPORT SnapshotData SnapshotAnyData;
extern PGDLLIMPORT SnapshotData CatalogSnapshotData;

#define SnapshotSelf		(&SnapshotSelfData)
#define SnapshotAny			(&SnapshotAnyData)

/*
 * 不提供静态 SnapshotDirty 变量，因其不可重入。
 * 该快照类型的用户应声明 SnapshotData 局部变量，
 * 并用本宏初始化。
 */
#define InitDirtySnapshot(snapshotdata)  \
	((snapshotdata).snapshot_type = SNAPSHOT_DIRTY)

/*
 * NonVacuumable 快照同样需初始化。调用方须提供
 * 要用的可见性截止状态（参见 GlobalVisTestFor()
 * ）。
 */
#define InitNonVacuumableSnapshot(snapshotdata, vistestp)  \
	((snapshotdata).snapshot_type = SNAPSHOT_NON_VACUUMABLE, \
	 (snapshotdata).vistest = (vistestp))

/*
 * SnapshotToast 同样需要一些初始化。须正确设置
 * lsn 与 whenTaken 以支持 snapshot_too_old。
 */
#define InitToastSnapshot(snapshotdata, l, w)  \
	((snapshotdata).snapshot_type = SNAPSHOT_TOAST, \
	 (snapshotdata).lsn = (l),					\
	 (snapshotdata).whenTaken = (w))

/* 本宏编码哪些快照是 MVCC 安全的知识 */
#define IsMVCCSnapshot(snapshot)  \
	((snapshot)->snapshot_type == SNAPSHOT_MVCC || \
	 (snapshot)->snapshot_type == SNAPSHOT_HISTORIC_MVCC)

#ifndef FRONTEND
static inline bool
OldSnapshotThresholdActive(void)
{
	return old_snapshot_threshold >= 0;
}
#endif

extern Snapshot GetTransactionSnapshot(void);
extern Snapshot GetLatestSnapshot(void);
extern void SnapshotSetCommandId(CommandId curcid);
extern Snapshot GetOldestSnapshot(void);

extern Snapshot GetCatalogSnapshot(Oid relid);
extern Snapshot GetNonHistoricCatalogSnapshot(Oid relid);
extern void InvalidateCatalogSnapshot(void);
extern void InvalidateCatalogSnapshotConditionally(void);

extern void PushActiveSnapshot(Snapshot snapshot);
extern void PushActiveSnapshotWithLevel(Snapshot snapshot, int snap_level);
extern void PushCopiedSnapshot(Snapshot snapshot);
extern void UpdateActiveSnapshotCommandId(void);
extern void PopActiveSnapshot(void);
extern Snapshot GetActiveSnapshot(void);
extern bool ActiveSnapshotSet(void);

extern Snapshot RegisterSnapshot(Snapshot snapshot);
extern void UnregisterSnapshot(Snapshot snapshot);
extern Snapshot RegisterSnapshotOnOwner(Snapshot snapshot, ResourceOwner owner);
extern void UnregisterSnapshotFromOwner(Snapshot snapshot, ResourceOwner owner);

extern void AtSubCommit_Snapshot(int level);
extern void AtSubAbort_Snapshot(int level);
extern void AtEOXact_Snapshot(bool isCommit, bool resetXmin);

extern void ImportSnapshot(const char *idstr);
extern bool XactHasExportedSnapshots(void);
extern void DeleteAllExportedSnapshotFiles(void);
extern void WaitForOlderSnapshots(TransactionId limitXmin, bool progress);
extern bool ThereAreNoPriorRegisteredSnapshots(void);
extern bool HaveRegisteredOrActiveSnapshot(void);
extern bool TransactionIdLimitedForOldSnapshots(TransactionId recentXmin,
												Relation relation,
												TransactionId *limit_xid,
												TimestampTz *limit_ts);
extern void SetOldSnapshotThresholdTimestamp(TimestampTz ts, TransactionId xlimit);
extern void MaintainOldSnapshotTimeMapping(TimestampTz whenTaken,
										   TransactionId xmin);

extern char *ExportSnapshot(Snapshot snapshot);

/*
 * 这些住在 procarray.c，因其与 procarray 内容紧
 * 密相关，但主题上更适合放在 snapmgr.h。
 */
typedef struct GlobalVisState GlobalVisState;
extern GlobalVisState *GlobalVisTestFor(Relation rel);
extern bool GlobalVisTestIsRemovableXid(GlobalVisState *state, TransactionId xid);
extern bool GlobalVisTestIsRemovableFullXid(GlobalVisState *state, FullTransactionId fxid);
extern FullTransactionId GlobalVisTestNonRemovableFullHorizon(GlobalVisState *state);
extern TransactionId GlobalVisTestNonRemovableHorizon(GlobalVisState *state);
extern bool GlobalVisCheckRemovableXid(Relation rel, TransactionId xid);
extern bool GlobalVisCheckRemovableFullXid(Relation rel, FullTransactionId fxid);

/*
 * 供表 AM 实现可见性例程的工具函数。
 */
extern bool XidInMVCCSnapshot(TransactionId xid, Snapshot snapshot);

/* 逻辑解码的目录时间旅行支持 */
struct HTAB;
extern struct HTAB *HistoricSnapshotGetTupleCids(void);
extern void SetupHistoricSnapshot(Snapshot historic_snapshot, struct HTAB *tuplecids);
extern void TeardownHistoricSnapshot(bool is_error);
extern bool HistoricSnapshotActive(void);

extern Size EstimateSnapshotSpace(Snapshot snapshot);
extern void SerializeSnapshot(Snapshot snapshot, char *start_address);
extern Snapshot RestoreSnapshot(char *start_address);
extern void RestoreTransactionSnapshot(Snapshot snapshot, void *source_pgproc);

#endif							/* SNAPMGR_H */
