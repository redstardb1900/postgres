/*-------------------------------------------------------------------------
 *
 * jsonlog.c
 *	  JSON 日志
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/error/jsonlog.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "libpq/libpq.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/syslogger.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/backend_status.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/ps_status.h"

static void appendJSONKeyValueFmt(StringInfo buf, const char *key,
								  bool escape_key,
								  const char *fmt,...) pg_attribute_printf(4, 5);

/*
 * appendJSONKeyValue
 *
 * 向 StringInfo 追加一个逗号、一个 JSON 键及其值。
 * 键总是被转义。值可选择是否转义，这
 * 取决于键的数据类型。
 */
static void
appendJSONKeyValue(StringInfo buf, const char *key, const char *value,
				   bool escape_value)
{
	Assert(key != NULL);

	if (value == NULL)
		return;

	appendStringInfoChar(buf, ',');
	escape_json(buf, key);
	appendStringInfoChar(buf, ':');

	if (escape_value)
		escape_json(buf, value);
	else
		appendStringInfoString(buf, value);
}


/*
 * appendJSONKeyValueFmt
 *
 * 先求值 fmt 字符串，然后把结果交给
 * appendJSONKeyValue() 来生成 JSON 属性的值。
 * 两者均被转义。
 */
static void
appendJSONKeyValueFmt(StringInfo buf, const char *key,
					  bool escape_key, const char *fmt,...)
{
	int			save_errno = errno;
	size_t		len = 128;		/* 对缓冲区大小的初始假设 */
	char	   *value;

	for (;;)
	{
		va_list		args;
		size_t		newlen;

		/* 分配结果缓冲区 */
		value = (char *) palloc(len);

		/* 尝试格式化数据 */
		errno = save_errno;
		va_start(args, fmt);
		newlen = pvsnprintf(value, len, fmt, args);
		va_end(args);

		if (newlen < len)
			break;				/* 成功 */

		/* 释放缓冲区并循环，用更大的 len 重试。 */
		pfree(value);
		len = newlen;
	}

	appendJSONKeyValue(buf, key, value, escape_key);

	/* 清理 */
	pfree(value);
}

/*
 * 以 JSON 格式写日志
 */
void
write_jsonlog(ErrorData *edata)
{
	StringInfoData buf;
	char	   *start_time;
	char	   *log_time;

	/* 行号的静态计数器 */
	static long log_line_number = 0;

	/* 计数器是否已在当前进程中重置过？ */
	static int	log_my_pid = 0;

	/*
	 * 这是少数几个我们不希望从 postmaster 继承静态
	 * 变量值的场所之一。但我们既然会继承，就在 MyProcPid
	 * 变化时重置。
	 */
	if (log_my_pid != MyProcPid)
	{
		log_line_number = 0;
		log_my_pid = MyProcPid;
		reset_formatted_start_time();
	}
	log_line_number++;

	initStringInfo(&buf);

	/* 初始化字符串*/
	appendStringInfoChar(&buf, '{');

	/* 带毫秒的时间戳 */
	log_time = get_formatted_log_time();

	/*
	 * 第一个属性不使用 appendJSONKeyValue，因为它没有
	 * 逗号前缀
	 */
	escape_json(&buf, "timestamp");
	appendStringInfoChar(&buf, ':');
	escape_json(&buf, log_time);

	/* 用户名*/
	if (MyProcPort)
		appendJSONKeyValue(&buf, "user", MyProcPort->user_name, true);

	/* 数据库名 */
	if (MyProcPort)
		appendJSONKeyValue(&buf, "dbname", MyProcPort->database_name, true);

	/* 进程 ID */
	if (MyProcPid != 0)
		appendJSONKeyValueFmt(&buf, "pid", false, "%d", MyProcPid);

	/* 远程主机与端口*/
	if (MyProcPort && MyProcPort->remote_host)
	{
		appendJSONKeyValue(&buf, "remote_host", MyProcPort->remote_host, true);
		if (MyProcPort->remote_port && MyProcPort->remote_port[0] != '\0')
			appendJSONKeyValue(&buf, "remote_port", MyProcPort->remote_port, false);
	}

	/* 会话 id */
	appendJSONKeyValueFmt(&buf, "session_id", true, "%" INT64_MODIFIER "x.%x",
						  MyStartTime, MyProcPid);

	/* 行号 */
	appendJSONKeyValueFmt(&buf, "line_num", false, "%ld", log_line_number);

	/* PS 显示 */
	if (MyProcPort)
	{
		StringInfoData msgbuf;
		const char *psdisp;
		int			displen;

		initStringInfo(&msgbuf);
		psdisp = get_ps_display(&displen);
		appendBinaryStringInfo(&msgbuf, psdisp, displen);
		appendJSONKeyValue(&buf, "ps", msgbuf.data, true);

		pfree(msgbuf.data);
	}

	/* 会话启动时间戳 */
	start_time = get_formatted_start_time();
	appendJSONKeyValue(&buf, "session_start", start_time, true);

	/* 虚拟事务 id */
	/* VXID 格式与 lockfuncs.c 保持一致 */
	if (MyProc != NULL && MyProc->backendId != InvalidBackendId)
		appendJSONKeyValueFmt(&buf, "vxid", true, "%d/%u", MyProc->backendId,
							  MyProc->lxid);

	/* 事务 id */
	appendJSONKeyValueFmt(&buf, "txid", false, "%u",
						  GetTopTransactionIdIfAny());

	/* 严重级别 */
	if (edata->elevel)
		appendJSONKeyValue(&buf, "error_severity",
						   (char *) error_severity(edata->elevel), true);

	/* SQL 状态码 */
	if (edata->sqlerrcode)
		appendJSONKeyValue(&buf, "state_code",
						   unpack_sql_state(edata->sqlerrcode), true);

	/* errmessage */
	appendJSONKeyValue(&buf, "message", edata->message, true);

	/*errdetail/error_detail 日志 */
	if (edata->detail_log)
		appendJSONKeyValue(&buf, "detail", edata->detail_log, true);
	else
		appendJSONKeyValue(&buf, "detail", edata->detail, true);

	/* errhint */
	if (edata->hint)
		appendJSONKeyValue(&buf, "hint", edata->hint, true);

	/* 内部查询 */
	if (edata->internalquery)
		appendJSONKeyValue(&buf, "internal_query", edata->internalquery,
						   true);

	/* 已打印内部查询时，也打印内部位置 */
	if (edata->internalpos > 0 && edata->internalquery != NULL)
		appendJSONKeyValueFmt(&buf, "internal_position", false, "%d",
							  edata->internalpos);

	/* errcontext */
	if (edata->context && !edata->hide_ctx)
		appendJSONKeyValue(&buf, "context", edata->context, true);

	/* 用户查询 --- 仅在调用方未禁用时才报告 */
	if (check_log_of_query(edata))
	{
		appendJSONKeyValue(&buf, "statement", debug_query_string, true);
		if (edata->cursorpos > 0)
			appendJSONKeyValueFmt(&buf, "cursor_position", false, "%d",
								  edata->cursorpos);
	}

	/* 文件错误位置 */
	if (Log_error_verbosity >= PGERROR_VERBOSE)
	{
		if (edata->funcname)
			appendJSONKeyValue(&buf, "func_name", edata->funcname, true);
		if (edata->filename)
		{
			appendJSONKeyValue(&buf, "file_name", edata->filename, true);
			appendJSONKeyValueFmt(&buf, "file_line_num", false, "%d",
								  edata->lineno);
		}
	}

	/* 应用名 */
	if (application_name && application_name[0] != '\0')
		appendJSONKeyValue(&buf, "application_name", application_name, true);

	/* 后端类型 */
	appendJSONKeyValue(&buf, "backend_type", get_backend_type_for_log(), true);

	/*领导者PID*/
	if (MyProc)
	{
		PGPROC	   *leader = MyProc->lockGroupLeader;

		/*
		 * 仅对活跃的并行 worker 显示领导者。这会略去
		 * 并行组的领导者。
		 */
		if (leader && leader->pid != MyProcPid)
			appendJSONKeyValueFmt(&buf, "leader_pid", false, "%d",
								  leader->pid);
	}

	/*查询 id */
	appendJSONKeyValueFmt(&buf, "query_id", false, "%lld",
						  (long long) pgstat_get_my_query_id());

	/* 收尾 */
	appendStringInfoChar(&buf, '}');
	appendStringInfoChar(&buf, '\n');

	/* 若处于 syslogger 进程，则尝试把消息直接写入文件 */
	if (MyBackendType == B_LOGGER)
		write_syslogger_file(buf.data, buf.len, LOG_DESTINATION_JSONLOG);
	else
		write_pipe_chunks(buf.data, buf.len, LOG_DESTINATION_JSONLOG);

	pfree(buf.data);
}
