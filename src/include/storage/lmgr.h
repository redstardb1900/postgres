/*-------------------------------------------------------------------------
 *
 * lmgr.h
 *	  POSTGRES 锁管理器定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LMGR_H
#define LMGR_H

#include "lib/stringinfo.h"
#include "storage/itemptr.h"
#include "storage/lock.h"
#include "utils/rel.h"


/* XactLockTableWait 操作 */
typedef enum XLTW_Oper
{
	XLTW_None,
	XLTW_Update,
	XLTW_Delete,
	XLTW_Lock,
	XLTW_LockUpdated,
	XLTW_InsertIndex,
	XLTW_InsertIndexUnique,
	XLTW_FetchUpdated,
	XLTW_RecheckExclusionConstr
} XLTW_Oper;

extern void RelationInitLockInfo(Relation relation);

/* 锁住关系 */
extern void LockRelationOid(Oid relid, LOCKMODE lockmode);
extern void LockRelationId(LockRelId *relid, LOCKMODE lockmode);
extern bool ConditionalLockRelationOid(Oid relid, LOCKMODE lockmode);
extern void UnlockRelationId(LockRelId *relid, LOCKMODE lockmode);
extern void UnlockRelationOid(Oid relid, LOCKMODE lockmode);

extern void LockRelation(Relation relation, LOCKMODE lockmode);
extern bool ConditionalLockRelation(Relation relation, LOCKMODE lockmode);
extern void UnlockRelation(Relation relation, LOCKMODE lockmode);
extern bool CheckRelationLockedByMe(Relation relation, LOCKMODE lockmode,
									bool orstronger);
extern bool CheckRelationOidLockedByMe(Oid relid, LOCKMODE lockmode,
									   bool orstronger);
extern bool LockHasWaitersRelation(Relation relation, LOCKMODE lockmode);

extern void LockRelationIdForSession(LockRelId *relid, LOCKMODE lockmode);
extern void UnlockRelationIdForSession(LockRelId *relid, LOCKMODE lockmode);

/* 为扩展而锁住关系 */
extern void LockRelationForExtension(Relation relation, LOCKMODE lockmode);
extern void UnlockRelationForExtension(Relation relation, LOCKMODE lockmode);
extern bool ConditionalLockRelationForExtension(Relation relation,
												LOCKMODE lockmode);
extern int	RelationExtensionLockWaiterCount(Relation relation);

/* 为在当前库重算 pg_database.datfrozenxid 而加锁 */
extern void LockDatabaseFrozenIds(LOCKMODE lockmode);

/* 锁住页（目前仅用于索引内） */
extern void LockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode);
extern bool ConditionalLockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode);
extern void UnlockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode);

/* 锁住元组（先看 heap_lock_tuple 再假定你懂） */
extern void LockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode);
extern bool ConditionalLockTuple(Relation relation, ItemPointer tid,
								 LOCKMODE lockmode);
extern void UnlockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode);

/* 锁住 XID（用于等待事务结束） */
extern void XactLockTableInsert(TransactionId xid);
extern void XactLockTableDelete(TransactionId xid);
extern void XactLockTableWait(TransactionId xid, Relation rel,
							  ItemPointer ctid, XLTW_Oper oper);
extern bool ConditionalXactLockTableWait(TransactionId xid);

/* 锁住 VXIDs，由冲突的 locktag 指定 */
extern void WaitForLockers(LOCKTAG heaplocktag, LOCKMODE lockmode, bool progress);
extern void WaitForLockersMultiple(List *locktags, LOCKMODE lockmode, bool progress);

/* 为元组插入锁住 XID（用于等待插入完成） */
extern uint32 SpeculativeInsertionLockAcquire(TransactionId xid);
extern void SpeculativeInsertionLockRelease(TransactionId xid);
extern void SpeculativeInsertionWait(TransactionId xid, uint32 token);

/* 锁住当前库的一般对象（非关系） */
extern void LockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
							   LOCKMODE lockmode);
extern bool ConditionalLockDatabaseObject(Oid classid, Oid objid,
										  uint16 objsubid, LOCKMODE lockmode);
extern void UnlockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
								 LOCKMODE lockmode);

/* 锁住跨库共享对象（非关系） */
extern void LockSharedObject(Oid classid, Oid objid, uint16 objsubid,
							 LOCKMODE lockmode);
extern void UnlockSharedObject(Oid classid, Oid objid, uint16 objsubid,
							   LOCKMODE lockmode);

extern void LockSharedObjectForSession(Oid classid, Oid objid, uint16 objsubid,
									   LOCKMODE lockmode);
extern void UnlockSharedObjectForSession(Oid classid, Oid objid, uint16 objsubid,
										 LOCKMODE lockmode);

extern void LockApplyTransactionForSession(Oid suboid, TransactionId xid, uint16 objid,
										   LOCKMODE lockmode);
extern void UnlockApplyTransactionForSession(Oid suboid, TransactionId xid, uint16 objid,
											 LOCKMODE lockmode);

/* 为错误消息描述 locktag */
extern void DescribeLockTag(StringInfo buf, const LOCKTAG *tag);

extern const char *GetLockNameFromTagType(uint16 locktag_type);

#endif							/* LMGR_H */
