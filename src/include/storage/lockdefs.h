/*-------------------------------------------------------------------------
 *
 * lockdefs.h
 *	   面向前端的 postgres 低层锁机制部分。
 *
 * lockdefs.h 与 lock.h 的拆分不很原则。本文件含
 * 被 FRONTEND 代码包含时必须（间接）可用的
 * 定义。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lockdefs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOCKDEFS_H_
#define LOCKDEFS_H_

/*
 * LOCKMODE 是整数 (1..N)，表示锁类型。LOCKMASK 是位
 * 掩码，表示一组已持有或已请求的锁类型（位 1<<mode
 * 对应某一锁模式）。
 */
typedef int LOCKMASK;
typedef int LOCKMODE;

/*
 * 以下为所有标准锁方法（DEFAULT 与 USER）的
 * 合法 LOCKMODE 值。
 */

/* NoLock 不是锁模式，而是表示"不取锁"的标志值 */
#define NoLock					0

#define AccessShareLock			1	/* SELECT */
#define RowShareLock			2	/* SELECT FOR UPDATE/FOR SHARE */
#define RowExclusiveLock		3	/* INSERT, UPDATE, DELETE */
#define ShareUpdateExclusiveLock 4	/* VACUUM (non-FULL), ANALYZE, CREATE
									 * INDEX CONCURRENTLY */
#define ShareLock				5	/* CREATE INDEX (WITHOUT CONCURRENTLY) */
#define ShareRowExclusiveLock	6	/* like EXCLUSIVE MODE, but allows ROW
									 * SHARE */
#define ExclusiveLock			7	/* blocks ROW SHARE/SELECT...FOR UPDATE */
#define AccessExclusiveLock		8	/* ALTER TABLE, DROP TABLE, VACUUM FULL,
									 * and unqualified LOCK TABLE */

#define MaxLockMode				8	/* 最高标准锁模式 */

/* See README.tuplock section "Locking to write inplace-updated tables" */
#define InplaceUpdateTupleLock ExclusiveLock

/* WAL 中表上 AccessExclusiveLock 的表示 */
typedef struct xl_standby_lock
{
	TransactionId xid;			/* 持有 AccessExclusiveLock 的 xid */
	Oid			dbOid;			/* 含该表的 DB */
	Oid			relOid;			/* 表的 OID */
} xl_standby_lock;

#endif							/* LOCKDEFS_H_ */
