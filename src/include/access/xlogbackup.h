/*-------------------------------------------------------------------------
 *
 * xlogbackup.h
 *		基础备份的内部定义。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/xlogbackup.h
 *-------------------------------------------------------------------------
 */

#ifndef XLOG_BACKUP_H
#define XLOG_BACKUP_H

#include "access/xlogdefs.h"
#include "pgtime.h"

/* 保存备份状态的结构。 */
typedef struct BackupState
{
	/* 备份开始时保存字段 */
	/* 备份标签名另加一字节供空终止 */
	char		name[MAXPGPATH + 1];
	XLogRecPtr	startpoint;		/* 备份开始 WAL 位置 */
	TimeLineID	starttli;		/* 备份开始 TLI */
	XLogRecPtr	checkpointloc;	/* 最后检查点位置 */
	pg_time_t	starttime;		/* 备份开始时 */
	bool		started_in_recovery;	/* 备份在恢复中启动？ */

	/* 备份结束时保存的字段 */
	XLogRecPtr	stoppoint;		/* 备份结束 WAL 位置 */
	TimeLineID	stoptli;		/* 备份结束TLI */
	pg_time_t	stoptime;		/* 备份结束时 */
} BackupState;

extern char *build_backup_content(BackupState *state,
								  bool ishistoryfile);

#endif							/* XLOG_BACKUP_H */
