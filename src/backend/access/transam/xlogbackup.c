/*-------------------------------------------------------------------------
 *
 * xlogbackup.c
 *		基础备份的内部例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *      src/backend/access/transam/xlogbackup.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogbackup.h"

/*
 * 为 backup_label 或备份历史文件构造内容。
 *
 * 当 ishistoryfile 为真时，生成备份历史文件的内容，
 * 否则生成 backup_label 文件的内容。
 *
 * 返回以 palloc 分配的结果字符串。
 */
char *
build_backup_content(BackupState *state, bool ishistoryfile)
{
	char		startstrbuf[128];
	char		startxlogfile[MAXFNAMELEN]; /* 备份起始 WAL 文件 */
	XLogSegNo	startsegno;
	StringInfo	result = makeStringInfo();
	char	   *data;

	Assert(state != NULL);

	/* 此处使用日志时区，而非会话时区 */
	pg_strftime(startstrbuf, sizeof(startstrbuf), "%Y-%m-%d %H:%M:%S %Z",
				pg_localtime(&state->starttime, log_timezone));

	XLByteToSeg(state->startpoint, startsegno, wal_segment_size);
	XLogFileName(startxlogfile, state->starttli, startsegno, wal_segment_size);
	appendStringInfo(result, "START WAL LOCATION: %X/%X (file %s)\n",
					 LSN_FORMAT_ARGS(state->startpoint), startxlogfile);

	if (ishistoryfile)
	{
		char		stopxlogfile[MAXFNAMELEN];	/* 备份结束 WAL 文件 */
		XLogSegNo	stopsegno;

		XLByteToSeg(state->stoppoint, stopsegno, wal_segment_size);
		XLogFileName(stopxlogfile, state->stoptli, stopsegno, wal_segment_size);
		appendStringInfo(result, "STOP WAL LOCATION: %X/%X (file %s)\n",
						 LSN_FORMAT_ARGS(state->stoppoint), stopxlogfile);
	}

	appendStringInfo(result, "CHECKPOINT LOCATION: %X/%X\n",
					 LSN_FORMAT_ARGS(state->checkpointloc));
	appendStringInfo(result, "BACKUP METHOD: streamed\n");
	appendStringInfo(result, "BACKUP FROM: %s\n",
					 state->started_in_recovery ? "standby" : "primary");
	appendStringInfo(result, "START TIME: %s\n", startstrbuf);
	appendStringInfo(result, "LABEL: %s\n", state->name);
	appendStringInfo(result, "START TIMELINE: %u\n", state->starttli);

	if (ishistoryfile)
	{
		char		stopstrfbuf[128];

		/* 此处使用日志时区，而非会话时区 */
		pg_strftime(stopstrfbuf, sizeof(stopstrfbuf), "%Y-%m-%d %H:%M:%S %Z",
					pg_localtime(&state->stoptime, log_timezone));

		appendStringInfo(result, "STOP TIME: %s\n", stopstrfbuf);
		appendStringInfo(result, "STOP TIMELINE: %u\n", state->stoptli);
	}

	data = result->data;
	pfree(result);

	return data;
}
