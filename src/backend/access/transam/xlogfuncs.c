/*-------------------------------------------------------------------------
 *
 * xlogfuncs.c
 *
 * PostgreSQL 预写式日志管理器的用户接口函数
 *
 * 本文件包含 WAL 控制与信息函数。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xlogfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/htup_details.h"
#include "access/xlog_internal.h"
#include "access/xlogbackup.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/walreceiver.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/smgr.h"
#include "storage/standby.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/pg_lsn.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

/*
 * 与备份相关的变量。
 */
static BackupState *backup_state = NULL;
static StringInfo tablespace_map = NULL;

/* SQL 可调用备份函数的会话级上下文 */
static MemoryContext backupcontext = NULL;

/*
 * pg_backup_start：为进行在线备份转储做准备
 *
 * 本质上，本函数生成 backup_label 文件与表空间
 * 映射所需的内容。
 *
 * 本函数的权限检查通过常规
 * GRANT 系统管理。
 */
Datum
pg_backup_start(PG_FUNCTION_ARGS)
{
	text	   *backupid = PG_GETARG_TEXT_PP(0);
	bool		fast = PG_GETARG_BOOL(1);
	char	   *backupidstr;
	SessionBackupState status = get_backup_status();
	MemoryContext oldcontext;

	backupidstr = text_to_cstring(backupid);

	if (status == SESSION_BACKUP_RUNNING)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a backup is already in progress in this session")));

	/*
	 * backup_state 与 tablespace_map 需要长期存活，因为它们在
	 * pg_backup_stop() 中使用。它们分配在 TopMemoryContext 的专用
	 * 子内存上下文中，在 pg_backup_stop() 结束时删除。若在
	 * 结束备份前发生错误，该上下文中的内存会泄漏，
	 * 直到再次调用 pg_backup_start()。
	 */
	if (backupcontext == NULL)
	{
		backupcontext = AllocSetContextCreate(TopMemoryContext,
											  "on-line backup context",
											  ALLOCSET_START_SMALL_SIZES);
	}
	else
	{
		backup_state = NULL;
		tablespace_map = NULL;
		MemoryContextReset(backupcontext);
	}

	oldcontext = MemoryContextSwitchTo(backupcontext);
	backup_state = (BackupState *) palloc0(sizeof(BackupState));
	tablespace_map = makeStringInfo();
	MemoryContextSwitchTo(oldcontext);

	register_persistent_abort_backup_handler();
	do_pg_backup_start(backupidstr, fast, NULL, backup_state, tablespace_map);

	PG_RETURN_LSN(backup_state->startpoint);
}


/*
 * pg_backup_stop：完成在线备份。
 *
 * 第一个参数（变量 waitforarchive）可选，
 * 允许用户选择是否等待 WAL 被归档，
 * 还是只要 WAL 记录写完就立即返回。
 *
 * 本函数停止进行中的备份，创建 backup_label 内容，并
 * 返回备份停止 LSN、backup_label 与 tablespace_map 内容。
 *
 * backup_label 包含用户提供的标签字符串（通常用于
 * 指明备份转储将存放的位置）、起始
 * 时间、转储的起始 WAL 位置等。由调用者
 * 负责把 backup_label 与 tablespace_map 文件写入将从
 * 此备份恢复的数据目录。
 *
 * 本函数的权限检查通过常规
 * GRANT 系统管理。
 */
Datum
pg_backup_stop(PG_FUNCTION_ARGS)
{
#define PG_BACKUP_STOP_V2_COLS 3
	TupleDesc	tupdesc;
	Datum		values[PG_BACKUP_STOP_V2_COLS] = {0};
	bool		nulls[PG_BACKUP_STOP_V2_COLS] = {0};
	bool		waitforarchive = PG_GETARG_BOOL(0);
	char	   *backup_label;
	SessionBackupState status = get_backup_status();

	/* 初始化元组描述符中的属性信息 */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if (status != SESSION_BACKUP_RUNNING)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("backup is not in progress"),
				 errhint("Did you call pg_backup_start()?")));

	Assert(backup_state != NULL);
	Assert(tablespace_map != NULL);

	/* 停止备份 */
	do_pg_backup_stop(backup_state, waitforarchive);

	/* 构造 backup_label 的内容 */
	backup_label = build_backup_content(backup_state, false);

	values[0] = LSNGetDatum(backup_state->stoppoint);
	values[1] = CStringGetTextDatum(backup_label);
	values[2] = CStringGetTextDatum(tablespace_map->data);

	/* 释放与备份相关的变量 */
	pfree(backup_label);

	/* 清理会话级状态及其内存上下文 */
	backup_state = NULL;
	tablespace_map = NULL;
	MemoryContextDelete(backupcontext);
	backupcontext = NULL;

	/* 以 Datum 返回该记录 */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * pg_switch_wal：切换到下一个 xlog 文件
 *
 * 本函数的权限检查通过常规
 * GRANT 系统管理。
 */
Datum
pg_switch_wal(PG_FUNCTION_ARGS)
{
	XLogRecPtr	switchpoint;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("WAL control functions cannot be executed during recovery.")));

	switchpoint = RequestXLogSwitch(false);

	/*
	 * 为方便起见，返回切换记录的 WAL 位置
	 */
	PG_RETURN_LSN(switchpoint);
}

/*
 * pg_log_standby_snapshot：调用 LogStandbySnapshot()
 *
 * 本函数的权限检查通过常规
 * GRANT 系统管理。
 */
Datum
pg_log_standby_snapshot(PG_FUNCTION_ARGS)
{
	XLogRecPtr	recptr;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("%s cannot be executed during recovery.",
						 "pg_log_standby_snapshot()")));

	if (!XLogStandbyInfoActive())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_log_standby_snapshot() can only be used if wal_level >= replica")));

	recptr = LogStandbySnapshot();

	/*
	 * 为方便起见，返回最后插入记录的 WAL 位置
	 */
	PG_RETURN_LSN(recptr);
}

/*
 * pg_create_restore_point：用于恢复的命名点
 *
 * 本函数的权限检查通过常规
 * GRANT 系统管理。
 */
Datum
pg_create_restore_point(PG_FUNCTION_ARGS)
{
	text	   *restore_name = PG_GETARG_TEXT_PP(0);
	char	   *restore_name_str;
	XLogRecPtr	restorepoint;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("WAL control functions cannot be executed during recovery.")));

	if (!XLogIsNeeded())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("WAL level not sufficient for creating a restore point"),
				 errhint("wal_level must be set to \"replica\" or \"logical\" at server start.")));

	restore_name_str = text_to_cstring(restore_name);

	if (strlen(restore_name_str) >= MAXFNAMELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("value too long for restore point (maximum %d characters)", MAXFNAMELEN - 1)));

	restorepoint = XLogRestorePoint(restore_name_str);

	/*
	 * 为方便起见，返回恢复点记录的 WAL 位置
	 */
	PG_RETURN_LSN(restorepoint);
}

/*
 * 报告当前 WAL 写位置（格式与 pg_backup_start 等相同）
 *
 * 这有助于确定外部归档进程能看到
 * 多少 WAL。注意此点之前的数据已写出
 * 到内核，但不一定已同步到磁盘。
 */
Datum
pg_current_wal_lsn(PG_FUNCTION_ARGS)
{
	XLogRecPtr	current_recptr;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("WAL control functions cannot be executed during recovery.")));

	current_recptr = GetXLogWriteRecPtr();

	PG_RETURN_LSN(current_recptr);
}

/*
 * 报告当前 WAL 插入位置（格式与 pg_backup_start 等相同）
 *
 * 本函数主要用于调试。
 */
Datum
pg_current_wal_insert_lsn(PG_FUNCTION_ARGS)
{
	XLogRecPtr	current_recptr;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("WAL control functions cannot be executed during recovery.")));

	current_recptr = GetXLogInsertRecPtr();

	PG_RETURN_LSN(current_recptr);
}

/*
 * 报告当前 WAL 刷盘位置（格式与 pg_backup_start 等相同）
 *
 * 本函数主要用于调试。
 */
Datum
pg_current_wal_flush_lsn(PG_FUNCTION_ARGS)
{
	XLogRecPtr	current_recptr;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("WAL control functions cannot be executed during recovery.")));

	current_recptr = GetFlushRecPtr(NULL);

	PG_RETURN_LSN(current_recptr);
}

/*
 * 报告最后 WAL 接收位置（格式与 pg_backup_start 等相同）
 *
 * 这有助于确定有多少 WAL 已由 WAL 接收进程
 * 保证接收并同步到磁盘。
 */
Datum
pg_last_wal_receive_lsn(PG_FUNCTION_ARGS)
{
	XLogRecPtr	recptr;

	recptr = GetWalRcvFlushRecPtr(NULL, NULL);

	if (recptr == 0)
		PG_RETURN_NULL();

	PG_RETURN_LSN(recptr);
}

/*
 * 报告最后 WAL 重放位置（格式与 pg_backup_start 等相同）
 *
 * 这有助于确定恢复期间只读连接
 * 能看到多少 WAL。
 */
Datum
pg_last_wal_replay_lsn(PG_FUNCTION_ARGS)
{
	XLogRecPtr	recptr;

	recptr = GetXLogReplayRecPtr(NULL);

	if (recptr == 0)
		PG_RETURN_NULL();

	PG_RETURN_LSN(recptr);
}

/*
 * 根据 WAL 位置计算 xlog file 名与十进制字节偏移，
 * 该位置例如由 pg_backup_stop() 或 pg_switch_wal() 返回。
 *
 * 注意恰好位于段边界的位置被视为属于
 * 前一个段。这通常是正确做法，因为
 * 预期用途是确定哪些 xlog file 已可归档。
 */
Datum
pg_walfile_name_offset(PG_FUNCTION_ARGS)
{
	XLogSegNo	xlogsegno;
	uint32		xrecoff;
	XLogRecPtr	locationpoint = PG_GETARG_LSN(0);
	char		xlogfilename[MAXFNAMELEN];
	Datum		values[2];
	bool		isnull[2];
	TupleDesc	resultTupleDesc;
	HeapTuple	resultHeapTuple;
	Datum		result;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("%s cannot be executed during recovery.",
						 "pg_walfile_name_offset()")));

	/*
	 * 为结果行构造元组描述符。必须与本
	 * 函数的 pg_proc 条目匹配！
	 */
	resultTupleDesc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(resultTupleDesc, (AttrNumber) 1, "file_name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(resultTupleDesc, (AttrNumber) 2, "file_offset",
					   INT4OID, -1, 0);

	resultTupleDesc = BlessTupleDesc(resultTupleDesc);

	/*
	 * xlogfilename
	 */
	XLByteToPrevSeg(locationpoint, xlogsegno, wal_segment_size);
	XLogFileName(xlogfilename, GetWALInsertionTimeLine(), xlogsegno,
				 wal_segment_size);

	values[0] = CStringGetTextDatum(xlogfilename);
	isnull[0] = false;

	/*
	 * offset
	 */
	xrecoff = XLogSegmentOffset(locationpoint, wal_segment_size);

	values[1] = UInt32GetDatum(xrecoff);
	isnull[1] = false;

	/*
	 * 元组拼装：先准备好 Datum，再压到一起
	 */
	resultHeapTuple = heap_form_tuple(resultTupleDesc, values, isnull);

	result = HeapTupleGetDatum(resultHeapTuple);

	PG_RETURN_DATUM(result);
}

/*
 * 根据 WAL 位置计算 xlog 文件名，
 * 该位置例如由 pg_backup_stop() 或 pg_switch_wal() 返回。
 */
Datum
pg_walfile_name(PG_FUNCTION_ARGS)
{
	XLogSegNo	xlogsegno;
	XLogRecPtr	locationpoint = PG_GETARG_LSN(0);
	char		xlogfilename[MAXFNAMELEN];

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("%s cannot be executed during recovery.",
						 "pg_walfile_name()")));

	XLByteToPrevSeg(locationpoint, xlogsegno, wal_segment_size);
	XLogFileName(xlogfilename, GetWALInsertionTimeLine(), xlogsegno,
				 wal_segment_size);

	PG_RETURN_TEXT_P(cstring_to_text(xlogfilename));
}

/*
 * 从给定的 WAL 文件名中提取序号与
 * 时间线 ID。
 */
Datum
pg_split_walfile_name(PG_FUNCTION_ARGS)
{
#define PG_SPLIT_WALFILE_NAME_COLS 2
	char	   *fname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *fname_upper;
	char	   *p;
	TimeLineID	tli;
	XLogSegNo	segno;
	Datum		values[PG_SPLIT_WALFILE_NAME_COLS] = {0};
	bool		isnull[PG_SPLIT_WALFILE_NAME_COLS] = {0};
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	char		buf[256];
	Datum		result;

	fname_upper = pstrdup(fname);

	/* 将 WAL 文件名转为大写。 */
	for (p = fname_upper; *p; p++)
		*p = pg_toupper((unsigned char) *p);

	if (!IsXLogFileName(fname_upper))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid WAL file name \"%s\"", fname)));

	XLogFromFileName(fname_upper, &tli, &segno, wal_segment_size);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* 转换为 numeric。 */
	snprintf(buf, sizeof buf, UINT64_FORMAT, segno);
	values[0] = DirectFunctionCall3(numeric_in,
									CStringGetDatum(buf),
									ObjectIdGetDatum(0),
									Int32GetDatum(-1));

	values[1] = Int64GetDatum(tli);

	tuple = heap_form_tuple(tupdesc, values, isnull);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);

#undef PG_SPLIT_WALFILE_NAME_COLS
}

/*
 * pg_wal_replay_pause - 请求暂停恢复
 *
 * 本函数的权限检查通过常规
 * GRANT 系统管理。
 */
Datum
pg_wal_replay_pause(PG_FUNCTION_ARGS)
{
	if (!RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is not in progress"),
				 errhint("Recovery control functions can only be executed during recovery.")));

	if (PromoteIsTriggered())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("standby promotion is ongoing"),
				 errhint("%s cannot be executed after promotion is triggered.",
						 "pg_wal_replay_pause()")));

	SetRecoveryPause(true);

	/* 唤醒恢复进程，以便它能处理暂停请求 */
	WakeupRecovery();

	PG_RETURN_VOID();
}

/*
 * pg_wal_replay_resume - 立即恢复
 *
 * 本函数的权限检查通过常规
 * GRANT 系统管理。
 */
Datum
pg_wal_replay_resume(PG_FUNCTION_ARGS)
{
	if (!RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is not in progress"),
				 errhint("Recovery control functions can only be executed during recovery.")));

	if (PromoteIsTriggered())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("standby promotion is ongoing"),
				 errhint("%s cannot be executed after promotion is triggered.",
						 "pg_wal_replay_resume()")));

	SetRecoveryPause(false);

	PG_RETURN_VOID();
}

/*
 * pg_is_wal_replay_paused
 */
Datum
pg_is_wal_replay_paused(PG_FUNCTION_ARGS)
{
	if (!RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is not in progress"),
				 errhint("Recovery control functions can only be executed during recovery.")));

	PG_RETURN_BOOL(GetRecoveryPauseState() != RECOVERY_NOT_PAUSED);
}

/*
 * pg_get_wal_replay_pause_state - 返回恢复暂停状态。
 *
 * 返回值：
 *
 * 'not paused' - 若未请求暂停
 * 'pause requested' - 若已请求暂停但恢复尚未暂停
 * 'paused' - 若恢复已暂停
 */
Datum
pg_get_wal_replay_pause_state(PG_FUNCTION_ARGS)
{
	char	   *statestr = NULL;

	if (!RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is not in progress"),
				 errhint("Recovery control functions can only be executed during recovery.")));

	/* 取得恢复暂停状态 */
	switch (GetRecoveryPauseState())
	{
		case RECOVERY_NOT_PAUSED:
			statestr = "not paused";
			break;
		case RECOVERY_PAUSE_REQUESTED:
			statestr = "pause requested";
			break;
		case RECOVERY_PAUSED:
			statestr = "paused";
			break;
	}

	Assert(statestr != NULL);
	PG_RETURN_TEXT_P(cstring_to_text(statestr));
}

/*
 * 返回最近处理的提交/中止记录的时间戳。
 *
 * 当服务器在无恢复情况下正常启动时，本函数
 * 返回 NULL。
 */
Datum
pg_last_xact_replay_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz xtime;

	xtime = GetLatestXTime();
	if (xtime == 0)
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(xtime);
}

/*
 * 返回表示当前恢复模式的布尔值，这是全局状态。
 */
Datum
pg_is_in_recovery(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(RecoveryInProgress());
}

/*
 * 计算两个 WAL 位置之间的字节差。
 */
Datum
pg_wal_lsn_diff(PG_FUNCTION_ARGS)
{
	Datum		result;

	result = DirectFunctionCall2(pg_lsn_mi,
								 PG_GETARG_DATUM(0),
								 PG_GETARG_DATUM(1));

	PG_RETURN_DATUM(result);
}

/*
 * 提升备库服务器。
 *
 * 结果为 "true" 表示：若 wait 为 "true" 则提升已完成，
 * 若 wait 为 false 则提升已启动。
 */
Datum
pg_promote(PG_FUNCTION_ARGS)
{
	bool		wait = PG_GETARG_BOOL(0);
	int			wait_seconds = PG_GETARG_INT32(1);
	FILE	   *promote_file;
	int			i;

	if (!RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is not in progress"),
				 errhint("Recovery control functions can only be executed during recovery.")));

	if (wait_seconds <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("\"wait_seconds\" must not be negative or zero")));

	/* 创建提升信号文件 */
	promote_file = AllocateFile(PROMOTE_SIGNAL_FILE, "w");
	if (!promote_file)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						PROMOTE_SIGNAL_FILE)));

	if (FreeFile(promote_file))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m",
						PROMOTE_SIGNAL_FILE)));

	/* 向 postmaster 发信号 */
	if (kill(PostmasterPid, SIGUSR1) != 0)
	{
		ereport(WARNING,
				(errmsg("failed to send signal to postmaster: %m")));
		(void) unlink(PROMOTE_SIGNAL_FILE);
		PG_RETURN_BOOL(false);
	}

	/* 若未请求等待则立即返回 */
	if (!wait)
		PG_RETURN_BOOL(true);

	/* 按所需时长等待提升完成 */
#define WAITS_PER_SECOND 10
	for (i = 0; i < WAITS_PER_SECOND * wait_seconds; i++)
	{
		int			rc;

		ResetLatch(MyLatch);

		if (!RecoveryInProgress())
			PG_RETURN_BOOL(true);

		CHECK_FOR_INTERRUPTS();

		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   1000L / WAITS_PER_SECOND,
					   WAIT_EVENT_PROMOTE);

		/*
		 * 若 postmaster 已死亡则紧急退出。这是为了避免
		 * 必须手动清理所有 postmaster 子进程。
		 */
		if (rc & WL_POSTMASTER_DEATH)
			PG_RETURN_BOOL(false);
	}

	ereport(WARNING,
			(errmsg_plural("server did not promote within %d second",
						   "server did not promote within %d seconds",
						   wait_seconds,
						   wait_seconds)));
	PG_RETURN_BOOL(false);
}
