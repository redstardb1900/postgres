/*-------------------------------------------------------------------------
 *
 * walsender_private.h
 *	  Private definitions from replication/walsender.c.
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * src/include/replication/walsender_private.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALSENDER_PRIVATE_H
#define _WALSENDER_PRIVATE_H

#include "access/xlog.h"
#include "lib/ilist.h"
#include "nodes/nodes.h"
#include "nodes/replnodes.h"
#include "replication/syncrep.h"
#include "storage/condition_variable.h"
#include "storage/latch.h"
#include "storage/shmem.h"
#include "storage/spin.h"

typedef enum WalSndState
{
	WALSNDSTATE_STARTUP = 0,
	WALSNDSTATE_BACKUP,
	WALSNDSTATE_CATCHUP,
	WALSNDSTATE_STREAMING,
	WALSNDSTATE_STOPPING
} WalSndState;

/*
 * Each walsender has a WalSnd struct in shared memory.
 *
 * This struct is protected by its 'mutex' spinlock field, except that some
 * members are only written by the walsender process itself, and thus that
 * process is free to read those members without holding spinlock.  pid and
 * needreload always require the spinlock to be held for all accesses.
 */
typedef struct WalSnd
{
	pid_t		pid;			/* this walsender's PID, or 0 if not active */

	WalSndState state;			/* this walsender's state */
	XLogRecPtr	sentPtr;		/*  WAL 已发送至此点  */
	bool		needreload;		/* does currently-open file need to be
								 * reloaded? */

	/*
	 * The xlog locations that have been written, flushed, and applied by
	 * standby-side. These may be invalid if the standby-side has not offered
	 * values yet.
	 */
	XLogRecPtr	write;
	XLogRecPtr	flush;
	XLogRecPtr	apply;

	/*  测得延迟,未知/无则为 -1。  */
	TimeOffset	writeLag;
	TimeOffset	flushLag;
	TimeOffset	applyLag;

	/*
	 * The priority order of the standby managed by this WALSender, as listed
	 * in synchronous_standby_names, or 0 if not-listed.
	 */
	int			sync_standby_priority;

	/*  保护本结构中的共享变量。  */
	slock_t		mutex;

	/*
	 * Pointer to the walsender's latch. Used by backends to wake up this
	 * walsender when it has work to do. NULL if the walsender isn't active.
	 */
	Latch	   *latch;

	/*
	 * 自备库收到的最后消息时间戳。
	 */
	TimestampTz replyTime;

	ReplicationKind kind;
} WalSnd;

extern PGDLLIMPORT WalSnd *MyWalSnd;

/*  整个数据库集簇共用一个 WalSndCtl 结构  */
typedef struct
{
	/*
	 * Synchronous replication queue with one queue per request type.
	 * Protected by SyncRepLock.
	 */
	dlist_head	SyncRepQueue[NUM_SYNC_REP_WAIT_MODE];

	/*
	 * Current location of the head of the queue. All waiters should have a
	 * waitLSN that follows this value. Protected by SyncRepLock.
	 */
	XLogRecPtr	lsn[NUM_SYNC_REP_WAIT_MODE];

	/*
	 * Status of data related to the synchronous standbys.  Waiting backends
	 * can't reload the config file safely, so checkpointer updates this value
	 * as needed. Protected by SyncRepLock.
	 */
	bits8		sync_standbys_status;

	/*  用作待唤醒物理/逻辑 walsender 的登记表  */
	ConditionVariable wal_flush_cv;
	ConditionVariable wal_replay_cv;

	WalSnd		walsnds[FLEXIBLE_ARRAY_MEMBER];
} WalSndCtlData;

/* Flags for WalSndCtlData->sync_standbys_status */

/*
 * Is the synchronous standby data initialized from the GUC?  This is set the
 * first time synchronous_standby_names is processed by the checkpointer.
 */
#define SYNC_STANDBY_INIT			(1 << 0)

/*
 * Is the synchronous standby data defined?  This is set when
 * synchronous_standby_names has some data, after being processed by the
 * checkpointer.
 */
#define SYNC_STANDBY_DEFINED		(1 << 1)

extern PGDLLIMPORT WalSndCtlData *WalSndCtl;


extern void WalSndSetState(WalSndState state);

/*
 * Internal functions for parsing the replication grammar, in repl_gram.y and
 * repl_scanner.l
 */
extern int	replication_yyparse(void);
extern int	replication_yylex(void);
extern void replication_yyerror(const char *message) pg_attribute_noreturn();
extern void replication_scanner_init(const char *str);
extern void replication_scanner_finish(void);
extern bool replication_scanner_is_replication_command(void);

extern PGDLLIMPORT Node *replication_parse_result;

#endif							/* _WALSENDER_PRIVATE_H */
