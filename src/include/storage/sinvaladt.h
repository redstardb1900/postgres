/*-------------------------------------------------------------------------
 *
 * sinvaladt.h
 *	  POSTGRES 共享缓存失效数据管理器。
 *
 * 共享缓存失效管理器负责在后端之间传递
 * 失效消息。任一后端发出的任何消息必须在
 * 可被遗忘前投递给所有已在运行的后端。
 * （若空间耗尽，则改为向落后过多的后端投递
 * "RESET" 消息。）
 *
 * 定义单条消息内容的结构类型
 * SharedInvalidationMessage 见 sinval.h。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/sinvaladt.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SINVALADT_H
#define SINVALADT_H

#include "storage/lock.h"
#include "storage/sinval.h"

/*
 * sinvaladt.c 中函数的原型
 */
extern Size SInvalShmemSize(void);
extern void CreateSharedInvalidationState(void);
extern void SharedInvalBackendInit(bool sendOnly);
extern PGPROC *BackendIdGetProc(int backendID);
extern void BackendIdGetTransactionIds(int backendID, TransactionId *xid,
									   TransactionId *xmin, int *nsubxid,
									   bool *overflowed);

extern void SIInsertDataEntries(const SharedInvalidationMessage *data, int n);
extern int	SIGetDataEntries(SharedInvalidationMessage *data, int datasize);
extern void SICleanupQueue(bool callerHasWriteLock, int minFree);
extern void SIResetAll(void);

extern LocalTransactionId GetNextLocalTransactionId(void);

#endif							/* SINVALADT_H */
