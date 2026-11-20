/*-------------------------------------------------------------------------
 *
 * standbydefs.h
 *	   面向前端暴露的热备模式定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/standbydefs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STANDBYDEFS_H
#define STANDBYDEFS_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/lockdefs.h"
#include "storage/sinval.h"

/* StandbyRmgr (RM_STANDBY_ID) 的恢复处理函数 */
extern void standby_redo(XLogReaderState *record);
extern void standby_desc(StringInfo buf, XLogReaderState *record);
extern const char *standby_identify(uint8 info);
extern void standby_desc_invalidations(StringInfo buf,
									   int nmsgs, SharedInvalidationMessage *msgs,
									   Oid dbId, Oid tsId,
									   bool relcacheInitFileInval);

/*
 * XLOG 消息类型
 */
#define XLOG_STANDBY_LOCK			0x00
#define XLOG_RUNNING_XACTS			0x10
#define XLOG_INVALIDATIONS			0x20

typedef struct xl_standby_locks
{
	int			nlocks;			/* locks 数组中的条目数 */
	xl_standby_lock locks[FLEXIBLE_ARRAY_MEMBER];
} xl_standby_locks;

/*
 * 把运行中事务数据写入 WAL 时用此结构。
 */
typedef struct xl_running_xacts
{
	int			xcnt;			/* xids[] 中 xid 数 */
	int			subxcnt;		/* xids[] 中子事务数 */
	bool		subxid_overflow;	/* 快照溢出，子事务未含 */
	TransactionId nextXid;		/* ShmemVariableCache->nextXid 的 xid */
	TransactionId oldestRunningXid; /* *非* oldestXmin */
	TransactionId latestCompletedXid;	/* 以便设 xmax */

	TransactionId xids[FLEXIBLE_ARRAY_MEMBER];
} xl_running_xacts;

/*
 * 备库的失效消息，目前仅当事务无已分配
 * xid 却提交时。
 */
typedef struct xl_invalidations
{
	Oid			dbId;			/* MyDatabaseId */
	Oid			tsId;			/* MyDatabaseTableSpace */
	bool		relcacheInitFileInval;	/* 使 relcache-init 文件失效 */
	int			nmsgs;			/* 共享失效消息个数 */
	SharedInvalidationMessage msgs[FLEXIBLE_ARRAY_MEMBER];
} xl_invalidations;

#define MinSizeOfInvalidations offsetof(xl_invalidations, msgs)

#endif							/* STANDBYDEFS_H */
