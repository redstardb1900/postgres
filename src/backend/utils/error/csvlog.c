/*-------------------------------------------------------------------------
 *
 * csvlog.c
 *	  CSV 日志
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/error/csvlog.c
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
#include "utils/ps_status.h"


/*
 * 把字符串的 CSV 形式追加到 StringInfo
 * CSV 使用 PostgreSQL 默认值，即 quote = escape = '"'
 * 若为 NULL 则不追加。
 */
static inline void
appendCSVLiteral(StringInfo buf, const char *data)
{
	const char *p = data;
	char		c;

	/* 避免把空字符串与 NULL 混淆 */
	if (p == NULL)
		return;

	appendStringInfoCharMacro(buf, '"');
	while ((c = *p++) != '\0')
	{
		if (c == '"')
			appendStringInfoCharMacro(buf, '"');
		appendStringInfoCharMacro(buf, c);
	}
	appendStringInfoCharMacro(buf, '"');
}

/*
 * write_csvlog -- 生成并写入 CSV 日志条目
 *
 * 根据传入的 Errordata 构造错误消息，采用 CSV
 * 格式，详见 doc/src/sgml/config.sgml。
 */
void
write_csvlog(ErrorData *edata)
{
	StringInfoData buf;
	bool		print_stmt = false;

	/* 行号的静态计数器 */
	static long log_line_number = 0;

	/* 计数器在当前进程是否已重置？ */
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

	/* 带毫秒的时间戳 */
	appendStringInfoString(&buf, get_formatted_log_time());
	appendStringInfoChar(&buf, ',');

	/* 用户名*/
	if (MyProcPort)
		appendCSVLiteral(&buf, MyProcPort->user_name);
	appendStringInfoChar(&buf, ',');

	/* 数据库名 */
	if (MyProcPort)
		appendCSVLiteral(&buf, MyProcPort->database_name);
	appendStringInfoChar(&buf, ',');

	/* 进程 ID */
	if (MyProcPid != 0)
		appendStringInfo(&buf, "%d", MyProcPid);
	appendStringInfoChar(&buf, ',');

	/* 远程主机与端口*/
	if (MyProcPort && MyProcPort->remote_host)
	{
		appendStringInfoChar(&buf, '"');
		appendStringInfoString(&buf, MyProcPort->remote_host);
		if (MyProcPort->remote_port && MyProcPort->remote_port[0] != '\0')
		{
			appendStringInfoChar(&buf, ':');
			appendStringInfoString(&buf, MyProcPort->remote_port);
		}
		appendStringInfoChar(&buf, '"');
	}
	appendStringInfoChar(&buf, ',');

	/* 会话 id */
	appendStringInfo(&buf, "%" INT64_MODIFIER "x.%x", MyStartTime, MyProcPid);
	appendStringInfoChar(&buf, ',');

	/* 行号 */
	appendStringInfo(&buf, "%ld", log_line_number);
	appendStringInfoChar(&buf, ',');

	/* PS 显示 */
	if (MyProcPort)
	{
		StringInfoData msgbuf;
		const char *psdisp;
		int			displen;

		initStringInfo(&msgbuf);

		psdisp = get_ps_display(&displen);
		appendBinaryStringInfo(&msgbuf, psdisp, displen);
		appendCSVLiteral(&buf, msgbuf.data);

		pfree(msgbuf.data);
	}
	appendStringInfoChar(&buf, ',');

	/* 会话启动时间戳 */
	appendStringInfoString(&buf, get_formatted_start_time());
	appendStringInfoChar(&buf, ',');

	/* 虚拟事务 id */
	/* VXID 格式与 lockfuncs.c 保持一致 */
	if (MyProc != NULL && MyProc->backendId != InvalidBackendId)
		appendStringInfo(&buf, "%d/%u", MyProc->backendId, MyProc->lxid);
	appendStringInfoChar(&buf, ',');

	/* 事务 id */
	appendStringInfo(&buf, "%u", GetTopTransactionIdIfAny());
	appendStringInfoChar(&buf, ',');

	/* 严重级别 */
	appendStringInfoString(&buf, _(error_severity(edata->elevel)));
	appendStringInfoChar(&buf, ',');

	/* SQL 状态码 */
	appendStringInfoString(&buf, unpack_sql_state(edata->sqlerrcode));
	appendStringInfoChar(&buf, ',');

	/* errmessage */
	appendCSVLiteral(&buf, edata->message);
	appendStringInfoChar(&buf, ',');

	/* errdetail 或 errdetail_log*/
	if (edata->detail_log)
		appendCSVLiteral(&buf, edata->detail_log);
	else
		appendCSVLiteral(&buf, edata->detail);
	appendStringInfoChar(&buf, ',');

	/* errhint */
	appendCSVLiteral(&buf, edata->hint);
	appendStringInfoChar(&buf, ',');

	/* 内部查询 */
	appendCSVLiteral(&buf, edata->internalquery);
	appendStringInfoChar(&buf, ',');

	/* 已打印内部查询时，也打印内部位置 */
	if (edata->internalpos > 0 && edata->internalquery != NULL)
		appendStringInfo(&buf, "%d", edata->internalpos);
	appendStringInfoChar(&buf, ',');

	/* errcontext */
	if (!edata->hide_ctx)
		appendCSVLiteral(&buf, edata->context);
	appendStringInfoChar(&buf, ',');

	/* 用户查询 --- 仅在调用方未禁用时才报告 */
	print_stmt = check_log_of_query(edata);
	if (print_stmt)
		appendCSVLiteral(&buf, debug_query_string);
	appendStringInfoChar(&buf, ',');
	if (print_stmt && edata->cursorpos > 0)
		appendStringInfo(&buf, "%d", edata->cursorpos);
	appendStringInfoChar(&buf, ',');

	/* 文件错误位置 */
	if (Log_error_verbosity >= PGERROR_VERBOSE)
	{
		StringInfoData msgbuf;

		initStringInfo(&msgbuf);

		if (edata->funcname && edata->filename)
			appendStringInfo(&msgbuf, "%s, %s:%d",
							 edata->funcname, edata->filename,
							 edata->lineno);
		else if (edata->filename)
			appendStringInfo(&msgbuf, "%s:%d",
							 edata->filename, edata->lineno);
		appendCSVLiteral(&buf, msgbuf.data);
		pfree(msgbuf.data);
	}
	appendStringInfoChar(&buf, ',');

	/* 应用名 */
	if (application_name)
		appendCSVLiteral(&buf, application_name);

	appendStringInfoChar(&buf, ',');

	/* 后端类型 */
	appendCSVLiteral(&buf, get_backend_type_for_log());
	appendStringInfoChar(&buf, ',');

	/*领导者PID*/
	if (MyProc)
	{
		PGPROC	   *leader = MyProc->lockGroupLeader;

		/*
		 * 仅对活跃的并行 worker 显示领导者。这会略去
		 * 并行组的领导者。
		 */
		if (leader && leader->pid != MyProcPid)
			appendStringInfo(&buf, "%d", leader->pid);
	}
	appendStringInfoChar(&buf, ',');

	/*查询 id */
	appendStringInfo(&buf, "%lld", (long long) pgstat_get_my_query_id());

	appendStringInfoChar(&buf, '\n');

	/* 若处于 syslogger 进程，则尝试把消息直接写入文件 */
	if (MyBackendType == B_LOGGER)
		write_syslogger_file(buf.data, buf.len, LOG_DESTINATION_CSVLOG);
	else
		write_pipe_chunks(buf.data, buf.len, LOG_DESTINATION_CSVLOG);

	pfree(buf.data);
}
