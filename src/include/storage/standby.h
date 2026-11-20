/*-------------------------------------------------------------------------
 *
 * standby.h
 *	  热备模式的定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/standby.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STANDBY_H
#define STANDBY_H

#include "datatype/timestamp.h"
#include "storage/lock.h"
#include "storage/procsignal.h"
#include "storage/relfilelocator.h"
#include "storage/standbydefs.h"

/* 用户可设的 GUC 参数 */
extern PGDLLIMPORT int max_standby_archive_delay;
extern PGDLLIMPORT int max_standby_streaming_delay;
extern PGDLLIMPORT bool log_recovery_conflict_waits;

extern void InitRecoveryTransactionEnvironment(void);
extern void ShutdownRecoveryTransactionEnvironment(void);

extern void ResolveRecoveryConflictWithSnapshot(TransactionId snapshotConflictHorizon,
												bool isCatalogRel,
												RelFileLocator locator);
extern void ResolveRecoveryConflictWithSnapshotFullXid(FullTransactionId snapshotConflictHorizon,
													   bool isCatalogRel,
													   RelFileLocator locator);
extern void ResolveRecoveryConflictWithTablespace(Oid tsid);
extern void ResolveRecoveryConflictWithDatabase(Oid dbid);

extern void ResolveRecoveryConflictWithLock(LOCKTAG locktag, bool logging_conflict);
extern void ResolveRecoveryConflictWithBufferPin(void);
extern void CheckRecoveryConflictDeadlock(void);
extern void StandbyDeadLockHandler(void);
extern void StandbyTimeoutHandler(void);
extern void StandbyLockTimeoutHandler(void);
extern void LogRecoveryConflict(ProcSignalReason reason, TimestampTz wait_start,
								TimestampTz now, VirtualTransactionId *wait_list,
								bool still_waiting);

/*
 * StandbyRmgr (RM_STANDBY_ID)
 *
 * StandbyRmgr 用于执行热备所需的动作。
 * 包括记录事务取得的 AccessExclusiveLocks，以及
 * running-xacts 快照。
 */
extern void StandbyAcquireAccessExclusiveLock(TransactionId xid, Oid dbOid, Oid relOid);
extern void StandbyReleaseLockTree(TransactionId xid,
								   int nsubxids, TransactionId *subxids);
extern void StandbyReleaseAllLocks(void);
extern void StandbyReleaseOldLocks(TransactionId oldxid);

#define MinSizeOfXactRunningXacts offsetof(xl_running_xacts, xids)


/*
 * GetRunningTransactionData() 的声明。类似 Snapshots，但
 * 不完全一样。这与本服务器上的可见性毫无关系，
 * 故与 snapmgr.c 和 snapmgr.h 完全分离。
 * 这些数据对在备库上创建初始快照状态很重要。
 * 我们需要比普通快照更多的信息，故用专用
 * 数据结构。这些数据在每次检查点后立即作为
 * 独立记录写入 WAL。无论从何处启动备库，
 * 几乎马上就能看到开始执行查询所需的
 * 数据。
 */

typedef enum
{
	SUBXIDS_IN_ARRAY,			/* xids 含全部运行中子事务 */
	SUBXIDS_MISSING,			/* 快照溢出，子事务未含 */
	SUBXIDS_IN_SUBTRANS,		/* 子事务未纳入 'xids'，但
								 * pg_subtrans 已完全最新 */
} subxids_array_status;

typedef struct RunningTransactionsData
{
	int			xcnt;			/* xids[] 中 xid 数 */
	int			subxcnt;		/* xids[] 中子事务数 */
	subxids_array_status subxid_status;
	TransactionId nextXid;		/* ShmemVariableCache->nextXid 的 xid */
	TransactionId oldestRunningXid; /* *非* oldestXmin */
	TransactionId latestCompletedXid;	/* 以便设 xmax */

	TransactionId *xids;		/* 仍在运行的 (子)xids 数组 */
} RunningTransactionsData;

typedef RunningTransactionsData *RunningTransactions;

extern void LogAccessExclusiveLock(Oid dbOid, Oid relOid);
extern void LogAccessExclusiveLockPrepare(void);

extern XLogRecPtr LogStandbySnapshot(void);
extern void LogStandbyInvalidations(int nmsgs, SharedInvalidationMessage *msgs,
									bool relcacheInitFileInval);

#endif							/* STANDBY_H */
