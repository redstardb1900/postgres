/*-------------------------------------------------------------------------
 *
 * predicate.h
 *	  POSTGRES 谓词加锁公共定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/predicate.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREDICATE_H
#define PREDICATE_H

#include "storage/lock.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"


/*
 * GUC 变量
 */
extern PGDLLIMPORT int max_predicate_locks_per_xact;
extern PGDLLIMPORT int max_predicate_locks_per_relation;
extern PGDLLIMPORT int max_predicate_locks_per_page;


/* Serial SLRU 所用 SLRU 缓冲区数 */
#define NUM_SERIAL_BUFFERS		16

/*
 * 用于在并行查询参与者之间共享 SERIALIZABLEXACT
 * 对象的句柄。
 */
typedef void *SerializableXactHandle;

/*
 * 函数原型
 */

/* 共享内存谓词锁结构的内务处理 */
extern void InitPredicateLocks(void);
extern Size PredicateLockShmemSize(void);

extern void CheckPointPredicate(void);

/* 谓词锁报告 */
extern bool PageIsPredicateLocked(Relation relation, BlockNumber blkno);

/* 谓词锁维护 */
extern Snapshot GetSerializableTransactionSnapshot(Snapshot snapshot);
extern void SetSerializableTransactionSnapshot(Snapshot snapshot,
											   VirtualTransactionId *sourcevxid,
											   int sourcepid);
extern void RegisterPredicateLockingXid(TransactionId xid);
extern void PredicateLockRelation(Relation relation, Snapshot snapshot);
extern void PredicateLockPage(Relation relation, BlockNumber blkno, Snapshot snapshot);
extern void PredicateLockTID(Relation relation, ItemPointer tid, Snapshot snapshot,
							 TransactionId tuple_xid);
extern void PredicateLockPageSplit(Relation relation, BlockNumber oldblkno, BlockNumber newblkno);
extern void PredicateLockPageCombine(Relation relation, BlockNumber oldblkno, BlockNumber newblkno);
extern void TransferPredicateLocksToHeapRelation(Relation relation);
extern void ReleasePredicateLocks(bool isCommit, bool isReadOnlySafe);

/* 冲突检测（也可能触发回滚） */
extern bool CheckForSerializableConflictOutNeeded(Relation relation, Snapshot snapshot);
extern void CheckForSerializableConflictOut(Relation relation, TransactionId xid, Snapshot snapshot);
extern void CheckForSerializableConflictIn(Relation relation, ItemPointer tid, BlockNumber blkno);
extern void CheckTableForSerializableConflictIn(Relation relation);

/* 最终回滚检查 */
extern void PreCommit_CheckForSerializationFailure(void);

/* 两阶段提交支持 */
extern void AtPrepare_PredicateLocks(void);
extern void PostPrepare_PredicateLocks(TransactionId xid);
extern void PredicateLockTwoPhaseFinish(TransactionId xid, bool isCommit);
extern void predicatelock_twophase_recover(TransactionId xid, uint16 info,
										   void *recdata, uint32 len);

/* 并行查询支持 */
extern SerializableXactHandle ShareSerializableXact(void);
extern void AttachSerializableXact(SerializableXactHandle handle);

#endif							/* PREDICATE_H */
