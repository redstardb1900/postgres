/*-------------------------------------------------------------------------
 *
 * syncrep.h
 *	  Exports from replication/syncrep.c.
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/replication/syncrep.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _SYNCREP_H
#define _SYNCREP_H

#include "access/xlogdefs.h"

#define SyncRepRequested() \
	(max_wal_senders > 0 && synchronous_commit > SYNCHRONOUS_COMMIT_LOCAL_FLUSH)

/* SyncRepWaitMode */
#define SYNC_REP_NO_WAIT		(-1)
#define SYNC_REP_WAIT_WRITE		0
#define SYNC_REP_WAIT_FLUSH		1
#define SYNC_REP_WAIT_APPLY		2

#define NUM_SYNC_REP_WAIT_MODE	3

/* syncRepState */
#define SYNC_REP_NOT_WAITING		0
#define SYNC_REP_WAITING			1
#define SYNC_REP_WAIT_COMPLETE		2

/* syncrep_method of SyncRepConfigData */
#define SYNC_REP_PRIORITY		0
#define SYNC_REP_QUORUM		1

/*
 * SyncRepGetCandidateStandbys returns an array of these structs,
 * one per candidate synchronous walsender.
 */
typedef struct SyncRepStandbyData
{
	/*  自 WalSnd 共享内存结构复制的相关字段  */
	pid_t		pid;
	XLogRecPtr	write;
	XLogRecPtr	flush;
	XLogRecPtr	apply;
	int			sync_standby_priority;
	/*  本 walsender 在 WalSnd 共享内存数组中的下标  */
	int			walsnd_index;
	/*  此结构是否关于本进程  */
	bool		is_me;
} SyncRepStandbyData;

/*
 * Struct for the configuration of synchronous replication.
 *
 * Note: this must be a flat representation that can be held in a single
 * chunk of malloc'd memory, so that it can be stored as the "extra" data
 * for the synchronous_standby_names GUC.
 */
typedef struct SyncRepConfigData
{
	int			config_size;	/*  本结构总大小,字节  */
	int			num_sync;		/*  需等待的同步备机
								 * 个数  */
	uint8		syncrep_method; /*  选择同步备机的方法  */
	int			nmembers;		/*  下列列表中的成员数  */
	/*  member_names 含 nmembers 个连续空终止 C 串  */
	char		member_names[FLEXIBLE_ARRAY_MEMBER];
} SyncRepConfigData;

extern PGDLLIMPORT SyncRepConfigData *SyncRepConfig;

/*  解析 synchronous_standby_names GUC 的通信变量  */
extern PGDLLIMPORT SyncRepConfigData *syncrep_parse_result;
extern PGDLLIMPORT char *syncrep_parse_error_msg;

/*  同步复制的用户可设参数  */
extern PGDLLIMPORT char *SyncRepStandbyNames;

/* called by user backend */
extern void SyncRepWaitForLSN(XLogRecPtr lsn, bool commit);

/* called at backend exit */
extern void SyncRepCleanupAtProcExit(void);

/* called by wal sender */
extern void SyncRepInitConfig(void);
extern void SyncRepReleaseWaiters(void);

/* called by wal sender and user backend */
extern int	SyncRepGetCandidateStandbys(SyncRepStandbyData **standbys);

/* called by checkpointer */
extern void SyncRepUpdateSyncStandbysDefined(void);

/*
 * Internal functions for parsing synchronous_standby_names grammar,
 * in syncrep_gram.y and syncrep_scanner.l
 */
extern int	syncrep_yyparse(void);
extern int	syncrep_yylex(void);
extern void syncrep_yyerror(const char *str);
extern void syncrep_scanner_init(const char *str);
extern void syncrep_scanner_finish(void);

#endif							/* _SYNCREP_H */
