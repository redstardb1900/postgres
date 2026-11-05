/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/bin/psql/common.c
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#ifndef WIN32
#include <unistd.h>				/* for write() */
#else
#include <io.h>					/* for _write() */
#include <win32.h>
#endif

#include "command.h"
#include "common.h"
#include "common/logging.h"
#include "copy.h"
#include "crosstabview.h"
#include "fe_utils/cancel.h"
#include "fe_utils/mbprint.h"
#include "fe_utils/string_utils.h"
#include "portability/instr_time.h"
#include "settings.h"

static bool DescribeQuery(const char *query, double *elapsed_msec);
static bool ExecQueryUsingCursor(const char *query, double *elapsed_msec);
static int	ExecQueryAndProcessResults(const char *query,
									   double *elapsed_msec,
									   bool *svpt_gone_p,
									   bool is_watch,
									   const printQueryOpt *opt,
									   FILE *printQueryFout);
static bool command_no_begin(const char *query);
static bool is_select_command(const char *query);


/*
 * openQueryOutputFile --- 尝试打开查询输出文件     
 *
 * fname == NULL 选 stdout，开头 '|' 选管道，               
 * 否则普通文 
 *
 * 输出文件指针存 *fout，管道标志存 *is_pipe。                
 * 管道时调用者负责调整 SIGPIPE 状态。                 
 *
 * 出错时报告适当错消息并返回 false。           
 */
bool
openQueryOutputFile(const char *fname, FILE **fout, bool *is_pipe)
{
	if (!fname || fname[0] == '\0')
	{
		*fout = stdout;
		*is_pipe = false;
	}
	else if (*fname == '|')
	{
		fflush(NULL);
		*fout = popen(fname + 1, "w");
		*is_pipe = true;
	}
	else
	{
		*fout = fopen(fname, "w");
		*is_pipe = false;
	}

	if (*fout == NULL)
	{
		pg_log_error("%s: %m", fname);
		return false;
	}

	return true;
}

/*
 * setQFout
 * -- -o 命令行选项和 \o 命令的处理函数   
 *
 *                                                                    
 * 成功用新输出文件更新 pset 返回 true。    
 */
bool
setQFout(const char *fname)
{
	FILE	   *fout;
	bool		is_pipe;

	/* 先确保能打开新输出文件/管道             */
	if (!openQueryOutputFile(fname, &fout, &is_pipe))
		return false;

	/* 关闭旧文件/管 */
	if (pset.queryFout && pset.queryFout != stdout && pset.queryFout != stderr)
	{
		if (pset.queryFoutPipe)
			SetShellResultVariables(pclose(pset.queryFout));
		else
			fclose(pset.queryFout);
	}

	pset.queryFout = fout;
	pset.queryFoutPipe = is_pipe;

	/* 适当调整 SIGPIPE 处理：管道忽略信号                */
	set_sigpipe_trap_state(is_pipe);
	restore_sigpipe_trap();

	return true;
}


/*
 * flex 词法分析器变量获取回调   
 *
 *                                                                         
 * 若变量存在，以字符串（malloc，调用者释放 
 *
 * 否则返回 NULL。                                                    
 *                                                                     
 * 若 "quote" 非 PQUOTE_PLAIN，按指定引用要求 
 *
 * 对值引用和转义（失败应打印错并返回 NULL）。             
 *                                                                     
 * "passthrough" 是给 psql_scan_set_passthrough  
 */
char *
psql_get_variable(const char *varname, PsqlScanQuoteType quote,
				  void *passthrough)
{
	char	   *result = NULL;
	const char *value;

	/* 非活跃 \if 分支抑制所有变量替换                   */
	if (passthrough && !conditional_active((ConditionalStack) passthrough))
		return NULL;

	value = GetVariable(pset.vars, varname);
	if (!value)
		return NULL;

	switch (quote)
	{
		case PQUOTE_PLAIN:
			result = pg_strdup(value);
			break;
		case PQUOTE_SQL_LITERAL:
		case PQUOTE_SQL_IDENT:
			{
				/*
				 * 此类情况用 libpq 引用函数，                   
				 * 其假定字符串为连接客户端编码。            
				 */
				char	   *escaped_value;

				if (!pset.db)
				{
					pg_log_error("cannot escape without active connection");
					return NULL;
				}

				if (quote == PQUOTE_SQL_LITERAL)
					escaped_value =
						PQescapeLiteral(pset.db, value, strlen(value));
				else
					escaped_value =
						PQescapeIdentifier(pset.db, value, strlen(value));

				if (escaped_value == NULL)
				{
					const char *error = PQerrorMessage(pset.db);

					pg_log_info("%s", error);
					return NULL;
				}

				/*
				 * 与其使词法分析器 API 复杂化需指定        
				 * free() 函数，不如多付一次                         
				 * strdup() 
				 */
				result = pg_strdup(escaped_value);
				PQfreemem(escaped_value);
				break;
			}
		case PQUOTE_SHELL_ARG:
			{
				/*
				 * 此用 appendShellStringNoError，编码无关， 
				 * 壳亦未知编码无所谓。无论何，               
				 * 唯一特殊字符 '"'，未见于                      
				 * 有效多字节字符中。                                
				 */
				PQExpBufferData buf;

				initPQExpBuffer(&buf);
				if (!appendShellStringNoError(&buf, value))
				{
					pg_log_error("shell command argument contains a newline or carriage return: \"%s\"",
								 value);
					free(buf.data);
					return NULL;
				}
				result = buf.data;
				break;
			}

			/* 无 default：编译器应对缺漏分支告警          */
	}

	return result;
}


/*
 * 处理后端通知消息（INFO，WARNING 等）
 */
void
NoticeProcessor(void *arg, const char *message)
{
	(void) arg;					/* not used */
	pg_log_info("%s", message);
}



/*
 * 支持查询取消的代码       
 *
 *                                                                       
 * 启动查询前启用 SIGINT  
 *
 * 向后端发取消请求。                                           
 *                                                                    
 * SIGINT 应中止所有长时间 psql 操作，                          
 * 不止数据库查询。多数位置通过长循环中                
 * 检查 cancel_pressed 实现。但在阻塞于用户                   
 * 输入时（readline() 或 fgets()）无效。此类位置中，        
 * 阻塞期设 sigint_interrupt_enabled 为真， 
 *
 * 指令信号捕获器通过 sigint_interrupt_jmp longjmp。          
 * 假定
 */
volatile sig_atomic_t sigint_interrupt_enabled = false;

sigjmp_buf	sigint_interrupt_jmp;

static void
psql_cancel_callback(void)
{
#ifndef WIN32
	/* 若等待输入，longjmp 跳出               */
	if (sigint_interrupt_enabled)
	{
		sigint_interrupt_enabled = false;
		siglongjmp(sigint_interrupt_jmp, 1);
	}
#endif

	/* 否则设取消标志终止长循环                 */
	cancel_pressed = true;
}

void
psql_setup_cancel_handler(void)
{
	setup_cancel_handler(psql_cancel_callback);
}


/* ConnectionUp
 *
 *                                                       
 */
static bool
ConnectionUp(void)
{
	return PQstatus(pset.db) != CONNECTION_BAD;
}



/* CheckConnection
 *
 * 验证我们与后端之间是否仍有可用连接，                 
 * 若无则尝试恢复。  
 *
 * 若连接原本仍在，或能成功恢复连接，则返回 true； 
 * 否则返回 false。但如果连接不存在，且会话为      
 * 非交互模式，程序将以退出码 EXIT_BADCONN                     
 * 直接退出。             
 */
static bool
CheckConnection(void)
{
	bool		OK;

	OK = ConnectionUp();
	if (!OK)
	{
		if (!pset.cur_cmd_interactive)
		{
			pg_log_error("connection to server was lost");
			exit(EXIT_BADCONN);
		}

		fprintf(stderr, _("The connection to the server was lost. Attempting reset: "));
		PQreset(pset.db);
		OK = ConnectionUp();
		if (!OK)
		{
			fprintf(stderr, _("Failed.\n"));

			/*
			 * 转为无连接状态；但先保存失败的连接，       
			 * 以便稍后的 \connect 尝试仍能引用其参数。     
			 * 此处状态清理代码要与 do_connect() 中的保持    
			 * 同步。         
			 */
			if (pset.dead_conn)
				PQfinish(pset.dead_conn);
			pset.dead_conn = pset.db;
			pset.db = NULL;
			ResetCancelConn();
			UnsyncVariables();
		}
		else
		{
			fprintf(stderr, _("Succeeded.\n"));

			/*
			 * 重新同步以防变化。与 do_connect() 同步。          
			 *              
			 */
			SyncVariables();
			connection_warnings(false); /* 须在 SyncVariables 之后 */
		}
	}

	return OK;
}




/*
 * AcceptResult
 *
 *                                                                        
 * 检查结果有效性，必要时给错误消息；        
 *
 * 确保后端连接仍存。                          
 */
static bool
AcceptResult(const PGresult *result, bool show_error)
{
	bool		OK;

	if (!result)
		OK = false;
	else
		switch (PQresultStatus(result))
		{
			case PGRES_COMMAND_OK:
			case PGRES_TUPLES_OK:
			case PGRES_EMPTY_QUERY:
			case PGRES_COPY_IN:
			case PGRES_COPY_OUT:
				/* 正常，无需  */
				OK = true;
				break;

			case PGRES_BAD_RESPONSE:
			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
				OK = false;
				break;

			default:
				OK = false;
				pg_log_error("unexpected PQresultStatus: %d",
							 PQresultStatus(result));
				break;
		}

	if (!OK && show_error)
	{
		const char *error = PQerrorMessage(pset.db);

		if (strlen(error))
			pg_log_info("%s", error);

		CheckConnection();
	}

	return OK;
}


/*
 * 从查询结果设特殊变量           
 * - ERROR: true/false，本次查询是否有错              
 * - SQLSTATE: 错码，无错 "00000"，未知 ""                    
 * - ROW_COUNT: 返回或影响行数，或 "0"                
 * - LAST_ERROR_SQLSTATE: 上次错误信息 
 * - LAST_ERROR_MESSAGE: 上次错误消息   
 *
 *                                                                     
 * 注意：仅对用户输入查询结果应用此函数，    
 */
static void
SetResultVariables(PGresult *result, bool success)
{
	if (success)
	{
		const char *ntuples = PQcmdTuples(result);

		SetVariable(pset.vars, "ERROR", "false");
		SetVariable(pset.vars, "SQLSTATE", "00000");
		SetVariable(pset.vars, "ROW_COUNT", *ntuples ? ntuples : "0");
	}
	else
	{
		const char *code = PQresultErrorField(result, PG_DIAG_SQLSTATE);
		const char *mesg = PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY);

		SetVariable(pset.vars, "ERROR", "true");

		/*
		 * 无 SQLSTATE 码用空串。libpq                                  
		 * 检测错误（连接丢失、ENOMEM）可发生。       
		 */
		if (code == NULL)
			code = "";
		SetVariable(pset.vars, "SQLSTATE", code);
		SetVariable(pset.vars, "ROW_COUNT", "0");
		SetVariable(pset.vars, "LAST_ERROR_SQLSTATE", code);
		SetVariable(pset.vars, "LAST_ERROR_MESSAGE", mesg ? mesg : "");
	}
}


/*
 * 从 shell 命令结果设特殊变量            
 * - SHELL_ERROR: true/false，退出码是否 0                  
 * - SHELL_EXIT_CODE: shell 惯例退出码                   
 *
 *                                                                    
 * 参数 wait(2)/waitpid(2) 返回等待状态  
 */
void
SetShellResultVariables(int wait_result)
{
	char		buf[32];

	SetVariable(pset.vars, "SHELL_ERROR",
				(wait_result == 0) ? "false" : "true");
	snprintf(buf, sizeof(buf), "%d", wait_result_to_exit_code(wait_result));
	SetVariable(pset.vars, "SHELL_EXIT_CODE", buf);
}


/*
 * ClearOrSaveResult
 *
 *                                                                       
 * 结果表错，记住供 \errverbose 显示
 *
 * 否则仅 PQclear()。                                              
 *                                                                     
 * 注：对全查询结果应用，含"后门"查询便调试。       
 */
static void
ClearOrSaveResult(PGresult *result)
{
	if (result)
	{
		switch (PQresultStatus(result))
		{
			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
				PQclear(pset.last_error_result);
				pset.last_error_result = result;
				break;

			default:
				PQclear(result);
				break;
		}
	}
}


/*
 * 消费全部结果 
 */
static void
ClearOrSaveAllResults(void)
{
	PGresult   *result;

	while ((result = PQgetResult(pset.db)) != NULL)
		ClearOrSaveResult(result);
}


/*
 * 打印微计时。始终打印原始毫秒；间隔 >=1 秒              
 * 分解为天/时/分/秒。                                        
 */
static void
PrintTiming(double elapsed_msec)
{
	double		seconds;
	double		minutes;
	double		hours;
	double		days;

	if (elapsed_msec < 1000.0)
	{
		/* 传统 (pre-v10) 输出格式                   */
		printf(_("Time: %.3f ms\n"), elapsed_msec);
		return;
	}

	/*
	 * 注：当总计不足 1 分钟时可只打印秒，格式类似     
	 * %06.3f。但这难以理解，除非附加 "s" 或其他标注。    
	 * 强制显示中包含分钟似乎是更好的解决方案。    
	 *                                      
	 */
	seconds = elapsed_msec / 1000.0;
	minutes = floor(seconds / 60.0);
	seconds -= 60.0 * minutes;
	if (minutes < 60.0)
	{
		printf(_("Time: %.3f ms (%02d:%06.3f)\n"),
			   elapsed_msec, (int) minutes, seconds);
		return;
	}

	hours = floor(minutes / 60.0);
	minutes -= 60.0 * hours;
	if (hours < 24.0)
	{
		printf(_("Time: %.3f ms (%02d:%02d:%06.3f)\n"),
			   elapsed_msec, (int) hours, (int) minutes, seconds);
		return;
	}

	days = floor(hours / 24.0);
	hours -= 24.0 * days;
	printf(_("Time: %.3f ms (%.0f d %02d:%02d:%06.3f)\n"),
		   elapsed_msec, days, (int) hours, (int) minutes, seconds);
}


/*
 * PSQLexec
 *
 * 此函数用于发送 "后门" 查询（非用户直接输入）      
 * 它受 -E 影响，但不受 -e 影响。    
 *
 * 如果发送了 COPY 命令，调用者需负责处理随后的    
 * 处理工作。 
 *
 * 注：我们无需检查 PQclientEncoding；假定没有调用者     
 * 经由这条路径发出 "SET CLIENT_ENCODING"。    
 */
PGresult *
PSQLexec(const char *query)
{
	PGresult   *res;

	if (!pset.db)
	{
		pg_log_error("You are currently not connected to a database.");
		return NULL;
	}

	if (pset.echo_hidden != PSQL_ECHO_HIDDEN_OFF)
	{
		printf(_("********* QUERY **********\n"
				 "%s\n"
				 "**************************\n\n"), query);
		fflush(stdout);
		if (pset.logfile)
		{
			fprintf(pset.logfile,
					_("********* QUERY **********\n"
					  "%s\n"
					  "**************************\n\n"), query);
			fflush(pset.logfile);
		}

		if (pset.echo_hidden == PSQL_ECHO_HIDDEN_NOEXEC)
			return NULL;
	}

	SetCancelConn(pset.db);

	res = PQexec(pset.db, query);

	ResetCancelConn();

	if (!AcceptResult(res, true))
	{
		ClearOrSaveResult(res);
		res = NULL;
	}

	return res;
}


/*
 * PSQLexecWatch
 *
 * 此函数供 \watch 命令用，把查询发送到服务器  
 * 并打印结果。                  
 *
 * 查询执行成功时返回 1；若查询无法再次重复（例如因  
 * 被中断），返回 0；出错返回 -1。
 */
int
PSQLexecWatch(const char *query, const printQueryOpt *opt, FILE *printQueryFout)
{
	bool		timing = pset.timing;
	double		elapsed_msec = 0;
	int			res;

	if (!pset.db)
	{
		pg_log_error("You are currently not connected to a database.");
		return 0;
	}

	SetCancelConn(pset.db);

	res = ExecQueryAndProcessResults(query, &elapsed_msec, NULL, true, opt, printQueryFout);

	ResetCancelConn();

	/* 可能微计时输出       */
	if (timing)
		PrintTiming(elapsed_msec);

	return res;
}


/*
 * PrintNotifications: 检查异步通知并打印                             
 */
static void
PrintNotifications(void)
{
	PGnotify   *notify;

	PQconsumeInput(pset.db);
	while ((notify = PQnotifies(pset.db)) != NULL)
	{
		/* 向后兼容，仅非空负载显示                      */
		if (notify->extra[0])
			fprintf(pset.queryFout, _("Asynchronous notification \"%s\" with payload \"%s\" received from server process with PID %d.\n"),
					notify->relname, notify->extra, notify->be_pid);
		else
			fprintf(pset.queryFout, _("Asynchronous notification \"%s\" received from server process with PID %d.\n"),
					notify->relname, notify->be_pid);
		fflush(pset.queryFout);
		PQfreemem(notify);
		PQconsumeInput(pset.db);
	}
}


/*
 * PrintQueryTuples: 假定查询结果正常，打印其元组   
 *
 * 我们使用 opt 给出的选项；若 opt 为 NULL，则使用  
 * pset.popt。     
 *
 * 输出到 printQueryFout；若其为 NULL，则使用        
 * pset.queryFout。     
 *
 * 成功时返回 true，否则返回 false。
 */
static bool
PrintQueryTuples(const PGresult *result, const printQueryOpt *opt,
				 FILE *printQueryFout)
{
	bool		ok = true;
	FILE	   *fout = printQueryFout ? printQueryFout : pset.queryFout;

	printQuery(result, opt ? opt : &pset.popt, fout, false, pset.logfile);
	fflush(fout);
	if (ferror(fout))
	{
		pg_log_error("could not print result table: %m");
		ok = false;
	}

	return ok;
}


/*
 * StoreQueryTuple: 查询正常数据存变量                          
 *
 *                                             
 */
static bool
StoreQueryTuple(const PGresult *result)
{
	bool		success = true;

	if (PQntuples(result) < 1)
	{
		pg_log_error("no rows returned for \\gset");
		success = false;
	}
	else if (PQntuples(result) > 1)
	{
		pg_log_error("more than one row returned for \\gset");
		success = false;
	}
	else
	{
		int			i;

		for (i = 0; i < PQnfields(result); i++)
		{
			char	   *colname = PQfname(result, i);
			char	   *varname;
			char	   *value;

			/* 拼接前缀和列名              */
			varname = psprintf("%s%s", pset.gset_prefix, colname);

			if (VariableHasHook(pset.vars, varname))
			{
				pg_log_warning("attempt to \\gset into specially treated variable \"%s\" ignored",
							   varname);
				continue;
			}

			if (!PQgetisnull(result, 0, i))
				value = PQgetvalue(result, 0, i);
			else
			{
				/* NULL 值取消设置非设置变量                */
				value = NULL;
			}

			if (!SetVariable(pset.vars, varname, value))
			{
				free(varname);
				success = false;
				break;
			}

			free(varname);
		}
	}

	return success;
}


/*
 * ExecQueryTuples: 查询正常每字段作 SQL 执行            
 *                                
 *
 * 成功 true，否则 false。               
 */
static bool
ExecQueryTuples(const PGresult *result)
{
	bool		success = true;
	int			nrows = PQntuples(result);
	int			ncolumns = PQnfields(result);
	int			r,
				c;

	/*
	 * 须关 gexec_flag 免无限递归。这允许                      
	 * ExecQueryUsingCursor 用于各查询结果。SendQuery 阻止         
	 * 其在取待执行查询时应用，因同不能处理递归。     
	 *                                                              
	 */
	pset.gexec_flag = false;

	for (r = 0; r < nrows; r++)
	{
		for (c = 0; c < ncolumns; c++)
		{
			if (!PQgetisnull(result, r, c))
			{
				const char *query = PQgetvalue(result, r, c);

				/* 若 cancel_pressed 则放弃执行  */
				if (cancel_pressed)
					goto loop_exit;

				/*
				 * ECHO_ALL 应回显这些查询，但 SendQuery        
				 * 假定 MainLoop 已做，故我们在此做。           
				 */
				if (pset.echo == PSQL_ECHO_ALL && !pset.singlestep)
				{
					puts(query);
					fflush(stdout);
				}

				if (!SendQuery(query))
				{
					/* 出错 - ON_ERROR_STOP 放弃执行        */
					success = false;
					if (pset.on_error_stop)
						goto loop_exit;
				}
			}
		}
	}

loop_exit:

	/*
	 * 恢复状态。gexec_flag 原开，否则不来此。                
	 * （命令结束时自关，不归我们管。）                     
	 *             
	 */
	pset.gexec_flag = true;

	/* 全部查询成功则 true                 */
	return success;
}


/*
 * 编组 COPY 数据。任一路径使连接脱 CO
 * 后调一次 PQresultStatus() 报错。返是否 OK。      
 *                                                       
 *
 * COPY OUT: 输到 copystream，NULL 丢弃。                             
 * COPY IN: pset.copyStream 设则用，否则 cur_cmd_source  
 *                          
 *
 * 需续处理更新 *resultp；否则 NULL。                              
 * queryFout 安全输出状态时返结果：COPY IN，                    
 * 或 COPY OUT 写入非 pset.queryFout 时。                         
 * 返回 NULL 防命令状态被打印——                             
 * 状态行不应视为 COPY 数据部分时需此。                
 */
static bool
HandleCopyResult(PGresult **resultp, FILE *copystream)
{
	bool		success;
	PGresult   *copy_result;
	ExecStatusType result_status = PQresultStatus(*resultp);

	Assert(result_status == PGRES_COPY_OUT ||
		   result_status == PGRES_COPY_IN);

	SetCancelConn(pset.db);

	if (result_status == PGRES_COPY_OUT)
	{
		success = handleCopyOut(pset.db,
								copystream,
								&copy_result)
			&& (copystream != NULL);

		/*
		 * 若状态与 COPY 数据同位置，抑制状态打印。        
		 * 这不阻止错报，handleCopyOut 已做。                
		 *                                         
		 */
		if (copystream == pset.queryFout)
		{
			PQclear(copy_result);
			copy_result = NULL;
		}
	}
	else
	{
		/* COPY IN */
		/* 忽略传函数的 copystream 参数                  */
		copystream = pset.copyStream ? pset.copyStream : pset.cur_cmd_source;
		success = handleCopyIn(pset.db,
							   copystream,
							   PQbinaryTuples(*resultp),
							   &copy_result);
	}
	ResetCancelConn();

	/*
	 * 用 COPY 退出状态替 PGRES_COPY_OUT/IN，                        
	 * 或 NULL 抑制所有打印。                        
	 */
	PQclear(*resultp);
	*resultp = copy_result;

	return success;
}

/*
 * PrintQueryStatus: 按要求报命令状态         
 *
 *                                                           
 */
static void
PrintQueryStatus(PGresult *result, FILE *printQueryFout)
{
	char		buf[16];
	FILE	   *fout = printQueryFout ? printQueryFout : pset.queryFout;

	if (!pset.quiet)
	{
		if (pset.popt.topt.format == PRINT_HTML)
		{
			fputs("<p>", fout);
			html_escaped_print(PQcmdStatus(result), fout);
			fputs("</p>\n", fout);
		}
		else
			fprintf(fout, "%s\n", PQcmdStatus(result));
		fflush(fout);
	}

	if (pset.logfile)
		fprintf(pset.logfile, "%s\n", PQcmdStatus(result));

	snprintf(buf, sizeof(buf), "%u", (unsigned int) PQoidValue(result));
	SetVariable(pset.vars, "LASTOID", buf);
}


/*
 * PrintQueryResult: 按要求打印/存/执行查询结果                  
 *
 *                                                    
 *
 * 注：仅 SendQuery() 用工具函数。                    
 *                                                            
 * last true 表命令串最后结果。                                      
 *
 * opt 和 printQueryFout 同 PrintQueryTuples。                   
 */
static bool
PrintQueryResult(PGresult *result, bool last,
				 const printQueryOpt *opt, FILE *printQueryFout,
				 FILE *printStatusFout)
{
	bool		success;
	const char *cmdstatus;

	if (!result)
		return false;

	switch (PQresultStatus(result))
	{
		case PGRES_TUPLES_OK:
			/* 存储或执行或打印数据 ...     */
			if (last && pset.gset_prefix)
				success = StoreQueryTuple(result);
			else if (last && pset.gexec_flag)
				success = ExecQueryTuples(result);
			else if (last && pset.crosstab_flag)
				success = PrintResultInCrosstab(result);
			else if (last || pset.show_all_results)
				success = PrintQueryTuples(result, opt, printQueryFout);
			else
				success = true;

			/* INSERT/UPDATE/DELETE RETURNING 也打状态               */
			if (last || pset.show_all_results)
			{
				cmdstatus = PQcmdStatus(result);
				if (strncmp(cmdstatus, "INSERT", 6) == 0 ||
					strncmp(cmdstatus, "UPDATE", 6) == 0 ||
					strncmp(cmdstatus, "DELETE", 6) == 0)
					PrintQueryStatus(result, printStatusFout);
			}

			break;

		case PGRES_COMMAND_OK:
			if (last || pset.show_all_results)
				PrintQueryStatus(result, printStatusFout);
			success = true;
			break;

		case PGRES_EMPTY_QUERY:
			success = true;
			break;

		case PGRES_COPY_OUT:
		case PGRES_COPY_IN:
			/* 此处无需操作：已处理        */
			success = true;
			break;

		case PGRES_BAD_RESPONSE:
		case PGRES_NONFATAL_ERROR:
		case PGRES_FATAL_ERROR:
			success = false;
			break;

		default:
			success = false;
			pg_log_error("unexpected PQresultStatus: %d",
						 PQresultStatus(result));
			break;
	}

	return success;
}

/*
 * SendQuery: 发查询串到后端               
 * （并打印结果） 
 *
 *                                                                       
 * 注："前门"方式，用户实际输入查询。                         
 * 受单步模式  
 * "后门"查询用 PSQLexec()。                                     
 *                                
 *
 * 成功 true，否则 false。                                    
 */
bool
SendQuery(const char *query)
{
	bool		timing = pset.timing;
	PGTransactionStatusType transaction_status;
	double		elapsed_msec = 0;
	bool		OK = false;
	int			i;
	bool		on_error_rollback_savepoint = false;
	bool		svpt_gone = false;

	if (!pset.db)
	{
		pg_log_error("You are currently not connected to a database.");
		goto sendquery_cleanup;
	}

	if (pset.singlestep)
	{
		char		buf[3];

		fflush(stderr);
		printf(_("***(Single step mode: verify command)*******************************************\n"
				 "%s\n"
				 "***(press return to proceed or enter x and return to cancel)********************\n"),
			   query);
		fflush(stdout);
		if (fgets(buf, sizeof(buf), stdin) != NULL)
			if (buf[0] == 'x')
				goto sendquery_cleanup;
		if (cancel_pressed)
			goto sendquery_cleanup;
	}
	else if (pset.echo == PSQL_ECHO_QUERIES)
	{
		puts(query);
		fflush(stdout);
	}

	if (pset.logfile)
	{
		fprintf(pset.logfile,
				_("********* QUERY **********\n"
				  "%s\n"
				  "**************************\n\n"), query);
		fflush(pset.logfile);
	}

	SetCancelConn(pset.db);

	transaction_status = PQtransactionStatus(pset.db);

	if (transaction_status == PQTRANS_IDLE &&
		!pset.autocommit &&
		!command_no_begin(query))
	{
		PGresult   *result;

		result = PQexec(pset.db, "BEGIN");
		if (PQresultStatus(result) != PGRES_COMMAND_OK)
		{
			pg_log_info("%s", PQerrorMessage(pset.db));
			ClearOrSaveResult(result);
			goto sendquery_cleanup;
		}
		ClearOrSaveResult(result);
		transaction_status = PQtransactionStatus(pset.db);
	}

	if (transaction_status == PQTRANS_INTRANS &&
		pset.on_error_rollback != PSQL_ERROR_ROLLBACK_OFF &&
		(pset.cur_cmd_interactive ||
		 pset.on_error_rollback == PSQL_ERROR_ROLLBACK_ON))
	{
		PGresult   *result;

		result = PQexec(pset.db, "SAVEPOINT pg_psql_temporary_savepoint");
		if (PQresultStatus(result) != PGRES_COMMAND_OK)
		{
			pg_log_info("%s", PQerrorMessage(pset.db));
			ClearOrSaveResult(result);
			goto sendquery_cleanup;
		}
		ClearOrSaveResult(result);
		on_error_rollback_savepoint = true;
	}

	if (pset.gdesc_flag)
	{
		/* 描述查询结果列，不执行                     */
		OK = DescribeQuery(query, &elapsed_msec);
	}
	else if (pset.fetch_count <= 0 || pset.gexec_flag ||
			 pset.crosstab_flag || !is_select_command(query))
	{
		/* 默认一次全取打印模式      */
		OK = (ExecQueryAndProcessResults(query, &elapsed_msec, &svpt_gone, false, NULL, NULL) > 0);
	}
	else
	{
		/* 分段获取模式     */
		OK = ExecQueryUsingCursor(query, &elapsed_msec);
	}

	if (!OK && pset.echo == PSQL_ECHO_ERRORS)
		pg_log_info("STATEMENT:  %s", query);

	/* 创临时保存点，试释放/回滚                       */
	if (on_error_rollback_savepoint)
	{
		const char *svptcmd = NULL;

		transaction_status = PQtransactionStatus(pset.db);

		switch (transaction_status)
		{
			case PQTRANS_INERROR:
				/* 出错总是回滚             */
				svptcmd = "ROLLBACK TO pg_psql_temporary_savepoint";
				break;

			case PQTRANS_IDLE:
				/* 不再事务中则无操作                             */
				break;

			case PQTRANS_INTRANS:

				/*
				 * 释放保存点，若他们自操保存点不做事      
				 *                           
				 */
				if (!svpt_gone)
					svptcmd = "RELEASE pg_psql_temporary_savepoint";
				break;

			case PQTRANS_ACTIVE:
			case PQTRANS_UNKNOWN:
			default:
				OK = false;
				/* 连接断时期望 PQTRANS_UNKNOWN                     */
				if (transaction_status != PQTRANS_UNKNOWN || ConnectionUp())
					pg_log_error("unexpected transaction status (%d)",
								 transaction_status);
				break;
		}

		if (svptcmd)
		{
			PGresult   *svptres;

			svptres = PQexec(pset.db, svptcmd);
			if (PQresultStatus(svptres) != PGRES_COMMAND_OK)
			{
				pg_log_info("%s", PQerrorMessage(pset.db));
				ClearOrSaveResult(svptres);
				OK = false;

				goto sendquery_cleanup;
			}
			PQclear(svptres);
		}
	}

	/* 可能微计时输出       */
	if (timing)
		PrintTiming(elapsed_msec);

	/* 检查查询执行期可能事件                      */

	if (pset.encoding != PQclientEncoding(pset.db) &&
		PQclientEncoding(pset.db) >= 0)
	{
		/* 跟踪 SET CLIENT_ENCODING 影响    */
		pset.encoding = PQclientEncoding(pset.db);
		pset.popt.topt.encoding = pset.encoding;
		SetVariable(pset.vars, "ENCODING",
					pg_encoding_to_char(pset.encoding));
	}

	PrintNotifications();

	/* 任何查询尝试后执行清理                           */

sendquery_cleanup:

	/* 全局取消重置        */
	ResetCancelConn();

	/* 重置 \g 输出到文件触发器    */
	if (pset.gfname)
	{
		free(pset.gfname);
		pset.gfname = NULL;
	}

	/* \g 更改打印设置则恢复            */
	if (pset.gsavepopt)
	{
		restorePsetInfo(&pset.popt, pset.gsavepopt);
		pset.gsavepopt = NULL;
	}

	/* 清理 \bind 后状  */
	clean_bind_state();

	/* 重置 \gset 触发 */
	if (pset.gset_prefix)
	{
		free(pset.gset_prefix);
		pset.gset_prefix = NULL;
	}

	/* 重置 \gdesc 触发 */
	pset.gdesc_flag = false;

	/* 重置 \gexec 触发 */
	pset.gexec_flag = false;

	/* 重置 \crosstabview 触发 */
	pset.crosstab_flag = false;
	for (i = 0; i < lengthof(pset.ctv_args); i++)
	{
		pg_free(pset.ctv_args[i]);
		pset.ctv_args[i] = NULL;
	}

	return OK;
}


/*
 * DescribeQuery: 描述查询结果的各个列，而不执行查询         
 *
 * 若操作执行成功则返回 true，否则返回 false。          
 *
 * 若 pset.timing 开启，查询总耗时（不含结果打印时间）  
 * 将存入 *elapsed_msec。
 */
static bool
DescribeQuery(const char *query, double *elapsed_msec)
{
	bool		timing = pset.timing;
	PGresult   *result;
	bool		OK;
	instr_time	before,
				after;

	*elapsed_msec = 0;

	if (timing)
		INSTR_TIME_SET_CURRENT(before);
	else
		INSTR_TIME_SET_ZERO(before);

	/*
	 * 解析不执行，用匿名预编语句预编译。                    
	 * psql 用户不可见，因无法访问匿名预编语句。           
	 * 下一 Parse/Query 协议消息覆盖之。                           
	 * （无需清除，libpq 无简便方法。）                     
	 *                                                                     
	 *                                                           
	 */
	result = PQprepare(pset.db, "", query, 0, NULL);
	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		pg_log_info("%s", PQerrorMessage(pset.db));
		SetResultVariables(result, false);
		ClearOrSaveResult(result);
		return false;
	}
	PQclear(result);

	result = PQdescribePrepared(pset.db, "");
	OK = AcceptResult(result, true) &&
		(PQresultStatus(result) == PGRES_COMMAND_OK);
	if (OK && result)
	{
		if (PQnfields(result) > 0)
		{
			PQExpBufferData buf;
			int			i;

			initPQExpBuffer(&buf);

			printfPQExpBuffer(&buf,
							  "SELECT name AS \"%s\", pg_catalog.format_type(tp, tpm) AS \"%s\"\n"
							  "FROM (VALUES ",
							  gettext_noop("Column"),
							  gettext_noop("Type"));

			for (i = 0; i < PQnfields(result); i++)
			{
				const char *name;
				char	   *escname;

				if (i > 0)
					appendPQExpBufferStr(&buf, ",");

				name = PQfname(result, i);
				escname = PQescapeLiteral(pset.db, name, strlen(name));

				if (escname == NULL)
				{
					pg_log_info("%s", PQerrorMessage(pset.db));
					PQclear(result);
					termPQExpBuffer(&buf);
					return false;
				}

				appendPQExpBuffer(&buf, "(%s, '%u'::pg_catalog.oid, %d)",
								  escname,
								  PQftype(result, i),
								  PQfmod(result, i));

				PQfreemem(escname);
			}

			appendPQExpBufferStr(&buf, ") s(name, tp, tpm)");
			PQclear(result);

			result = PQexec(pset.db, buf.data);
			OK = AcceptResult(result, true);

			if (timing)
			{
				INSTR_TIME_SET_CURRENT(after);
				INSTR_TIME_SUBTRACT(after, before);
				*elapsed_msec += INSTR_TIME_GET_MILLISEC(after);
			}

			if (OK && result)
				OK = PrintQueryResult(result, true, NULL, NULL, NULL);

			termPQExpBuffer(&buf);
		}
		else
			fprintf(pset.queryFout,
					_("The command has no result, or the result has no columns.\n"));
	}

	SetResultVariables(result, OK);
	ClearOrSaveResult(result);

	return OK;
}


/*
 * ExecQueryAndProcessResults: SendQuery()                            
 * 和 PSQLexecWatch() 
 *
 *                                                 
 *
 * 发查询并循环 PGresult 对象。                                   
 *                                                                          
 * 含 COPY FROM STDIN / COPY TO STDOUT，                                 
 *
 * 须提供输入输出流处理关联 PGresult。                          
 * 为 COP
 *
 *                                                                          
 * 其他命令正常处理，视状态而定。                           
 *                                    
 *
 * 完全成功 1，中断 0，错误 -1。                             
 * 失败含 
 */
static int
ExecQueryAndProcessResults(const char *query,
						   double *elapsed_msec, bool *svpt_gone_p,
						   bool is_watch,
						   const printQueryOpt *opt, FILE *printQueryFout)
{
	bool		timing = pset.timing;
	bool		success;
	instr_time	before,
				after;
	PGresult   *result;
	FILE	   *gfile_fout = NULL;
	bool		gfile_is_pipe = false;

	if (timing)
		INSTR_TIME_SET_CURRENT(before);
	else
		INSTR_TIME_SET_ZERO(before);

	if (pset.bind_flag)
		success = PQsendQueryParams(pset.db, query, pset.bind_nparams, NULL, (const char *const *) pset.bind_params, NULL, NULL, 0);
	else
		success = PQsendQuery(pset.db, query);

	if (!success)
	{
		const char *error = PQerrorMessage(pset.db);

		if (strlen(error))
			pg_log_info("%s", error);

		CheckConnection();

		return -1;
	}

	/*
	 * 查询期发 SIGINT，中断被消费。                              
	 * 用户欲取消整个 watch，检测取消退出。                   
	 *                                                                      
	 */
	if (is_watch && cancel_pressed)
	{
		ClearOrSaveAllResults();
		return 0;
	}

	/* 首个结果 */
	result = PQgetResult(pset.db);

	while (result != NULL)
	{
		ExecStatusType result_status;
		PGresult   *next_result;
		bool		last;

		if (!AcceptResult(result, false))
		{
			/*
			 * 发生错误，服务器端故障                             
			 * 或提交命令串失败。记录。       
			 */
			const char *error = PQresultErrorMessage(result);

			if (strlen(error))
				pg_log_info("%s", error);

			CheckConnection();
			if (!is_watch)
				SetResultVariables(result, false);

			/* 清除前保留结果状态               */
			result_status = PQresultStatus(result);
			ClearOrSaveResult(result);
			success = false;

			/*
			 * 切换到下一结果
			 */
			if (result_status == PGRES_COPY_BOTH ||
				result_status == PGRES_COPY_OUT ||
				result_status == PGRES_COPY_IN)

				/*
				 * 出于某种晦涩原因，COPY 时 PQgetResult 并    
				 * *不会*返回 NULL，尽管结果已被清除，         
				 * 而是持续返回须手动忽略的 "空" 结果。 
				 *                 
				 */
				result = NULL;
			else
				result = PQgetResult(pset.db);

			/*
			 * 获取当前计时，防错误发生              
			 */
			if (timing)
			{
				INSTR_TIME_SET_CURRENT(after);
				INSTR_TIME_SUBTRACT(after, before);
				*elapsed_msec = INSTR_TIME_GET_MILLISEC(after);
			}

			continue;
		}
		else if (svpt_gone_p && !*svpt_gone_p)
		{
			/*
			 * 查用户是否执行破坏内部保存点命令：     
			 * 执行 COMMIT AND CHAIN、RELEASE 或 ROLLBACK，            
			 * 保存点消失。发 SAVEPOINT，                             
			 * 释放我们的会移除他们的  
			 */
			const char *cmd = PQcmdStatus(result);

			*svpt_gone_p = (strcmp(cmd, "COMMIT") == 0 ||
							strcmp(cmd, "SAVEPOINT") == 0 ||
							strcmp(cmd, "RELEASE") == 0 ||
							strcmp(cmd, "ROLLBACK") == 0);
		}

		result_status = PQresultStatus(result);

		/* 须改当前结果前处理 COPY                    */
		Assert(result_status != PGRES_COPY_BOTH);
		if (result_status == PGRES_COPY_IN ||
			result_status == PGRES_COPY_OUT)
		{
			FILE	   *copy_stream = NULL;

			/*
			 * COPY OUT: 输到默认位（\watch 可评分页管道），   
			 * \copy 用 pset.copyStream，                              
			 * 否则 pset.gfname 设定向之，                  
			 * 最后 pset.que
			 */
			if (result_status == PGRES_COPY_OUT)
			{
				if (is_watch)
				{
					/* 由 \watch 调用 */
					copy_stream = printQueryFout ? printQueryFout : pset.queryFout;
				}
				else if (pset.copyStream)
				{
					/* 由 \copy 调用 */
					copy_stream = pset.copyStream;
				}
				else if (pset.gfname)
				{
					/* 发到 \g 文件，可能已开                   */
					if (gfile_fout == NULL)
					{
						if (openQueryOutputFile(pset.gfname,
												&gfile_fout, &gfile_is_pipe))
						{
							if (gfile_is_pipe)
								disable_sigpipe_trap();
							copy_stream = gfile_fout;
						}
						else
							success = false;
					}
					else
						copy_stream = gfile_fout;
				}
				else
				{
					/* 回退到通用查询输出流               */
					copy_stream = pset.queryFout;
				}
			}

			/*
			 * 即使输出流未开，用 NULL 流调               
			 * HandleCopyResult() 收集丢弃 COPY 数据。             
			 *                       
			 */
			success &= HandleCopyResult(&result, copy_stream);
		}

		/*
		 * 再查 PQgetResult()。典型单命令串                          
		 * 返 NULL。否则有其他结果需处理。                     
		 * 需处理判断是否最后。                                    
		 */
		next_result = PQgetResult(pset.db);
		last = (next_result == NULL);

		/*
		 * 更新当前计时。         
		 *
		 *                                                              
		 * 含先前结果显示时间，无法避免，                 
		 * 因显前结果时服务器在处后续查询。               
		 * 服务器时间短并行显示隐藏之。                         
		 *               
		 *
		 * 组合查询计时为处理耗时上限。                        
		 *                                   
		 */
		if (timing)
		{
			INSTR_TIME_SET_CURRENT(after);
			INSTR_TIME_SUBTRACT(after, before);
			*elapsed_msec = INSTR_TIME_GET_MILLISEC(after);
		}

		/* 据设置可能打印或不打印                         */
		if (result != NULL)
		{
			/*
			 * 若结果需打印到 \g 指定文件，打开              
			 * （除非已开）。pset.gfname 设时，                    
			 * 传入 printQueryFout 不用元组输出，               
			 * 但仍用状态输出。                            
			 */
			FILE	   *tuples_fout = printQueryFout;
			bool		do_print = true;

			if (PQresultStatus(result) == PGRES_TUPLES_OK &&
				pset.gfname)
			{
				if (gfile_fout == NULL)
				{
					if (openQueryOutputFile(pset.gfname,
											&gfile_fout, &gfile_is_pipe))
					{
						if (gfile_is_pipe)
							disable_sigpipe_trap();
					}
					else
						success = do_print = false;
				}
				tuples_fout = gfile_fout;
			}
			if (do_print)
				success &= PrintQueryResult(result, last, opt,
											tuples_fout, printQueryFout);
		}

		/* 若顺利最后结果设变量                */
		if (!is_watch && last && success)
			SetResultVariables(result, true);

		ClearOrSaveResult(result);
		result = next_result;

		if (cancel_pressed)
		{
			/* 丢弃下一结果及未读取的                         */
			ClearOrSaveResult(result);
			ClearOrSaveAllResults();
			break;
		}
	}

	/* 关闭 \g 文件（若已开  */
	if (gfile_fout)
	{
		if (gfile_is_pipe)
		{
			SetShellResultVariables(pclose(gfile_fout));
			restore_sigpipe_trap();
		}
		else
			fclose(gfile_fout);
	}

	/* 需此从 COPY 期连接丢失恢复                */
	if (!CheckConnection())
		return -1;

	return cancel_pressed ? 0 : success ? 1 : -1;
}


/*
 * ExecQueryUsingCursor: 使用游标运行类似 SELECT 的   
 *
 * 查询。该特性可用于处理比 RAM 更大的结果集。   
 *
 * 查询执行成功时返回 true，否则返回 false。         
 *
 * 若 pset.timing 开启，查询总耗时（不含结果打印时间）  
 * 将存入 *elapsed_msec。
 */
static bool
ExecQueryUsingCursor(const char *query, double *elapsed_msec)
{
	bool		OK = true;
	PGresult   *result;
	PQExpBufferData buf;
	printQueryOpt my_popt = pset.popt;
	bool		timing = pset.timing;
	FILE	   *fout;
	bool		is_pipe;
	bool		is_pager = false;
	bool		started_txn = false;
	int64		total_tuples = 0;
	int			ntuples;
	int			fetch_count;
	char		fetch_cmd[64];
	instr_time	before,
				after;
	int			flush_error;

	*elapsed_msec = 0;

	/* 初始化部分表输出打印选项              */
	my_popt.topt.start_table = true;
	my_popt.topt.stop_table = false;
	my_popt.topt.prior_records = 0;

	if (timing)
		INSTR_TIME_SET_CURRENT(before);
	else
		INSTR_TIME_SET_ZERO(before);

	/* 不在事务中则启动一个           */
	if (PQtransactionStatus(pset.db) == PQTRANS_IDLE)
	{
		result = PQexec(pset.db, "BEGIN");
		OK = AcceptResult(result, true) &&
			(PQresultStatus(result) == PGRES_COMMAND_OK);
		ClearOrSaveResult(result);
		if (!OK)
			return false;
		started_txn = true;
	}

	/* 发送 DECLARE CURSO*/
	initPQExpBuffer(&buf);
	appendPQExpBuffer(&buf, "DECLARE _psql_cursor NO SCROLL CURSOR FOR\n%s",
					  query);

	result = PQexec(pset.db, buf.data);
	OK = AcceptResult(result, true) &&
		(PQresultStatus(result) == PGRES_COMMAND_OK);
	if (!OK)
		SetResultVariables(result, OK);
	ClearOrSaveResult(result);
	termPQExpBuffer(&buf);
	if (!OK)
		goto cleanup;

	if (timing)
	{
		INSTR_TIME_SET_CURRENT(after);
		INSTR_TIME_SUBTRACT(after, before);
		*elapsed_msec += INSTR_TIME_GET_MILLISEC(after);
	}

	/*
	 * \gset 模式取计数强制 2，                                      
	 * 查询返多行时抛适当错。                            
	 */
	if (pset.gset_prefix)
		fetch_count = 2;
	else
		fetch_count = pset.fetch_count;

	snprintf(fetch_cmd, sizeof(fetch_cmd),
			 "FETCH FORWARD %d FROM _psql_cursor",
			 fetch_count);

	/* 准备写入 \g 参数指定输出             */
	if (pset.gfname)
	{
		if (!openQueryOutputFile(pset.gfname, &fout, &is_pipe))
		{
			OK = false;
			goto cleanup;
		}
		if (is_pipe)
			disable_sigpipe_trap();
	}
	else
	{
		fout = pset.queryFout;
		is_pipe = false;		/* 无关紧要   */
	}

	/* 清除输出流先前存在错指示                         */
	clearerr(fout);

	for (;;)
	{
		if (timing)
			INSTR_TIME_SET_CURRENT(before);

		/* 每次取 fetch_count 元组     */
		result = PQexec(pset.db, fetch_cmd);

		if (timing)
		{
			INSTR_TIME_SET_CURRENT(after);
			INSTR_TIME_SUBTRACT(after, before);
			*elapsed_msec += INSTR_TIME_GET_MILLISEC(after);
		}

		if (PQresultStatus(result) != PGRES_TUPLES_OK)
		{
			/* 打印错消息前关闭分页器             */
			if (is_pager)
			{
				ClosePager(fout);
				is_pager = false;
			}

			OK = AcceptResult(result, true);
			Assert(!OK);
			SetResultVariables(result, OK);
			ClearOrSaveResult(result);
			break;
		}

		if (pset.gset_prefix)
		{
			/* StoreQueryTuple 非一行时报错                   */
			OK = StoreQueryTuple(result);
			ClearOrSaveResult(result);
			break;
		}

		/*
		 * 注：此处不处理 \gdesc/\gexec/\crosstabview                  
		 */

		ntuples = PQntuples(result);
		total_tuples += ntuples;

		if (ntuples < fetch_count)
		{
			/* 最后结果集，允许页脚装饰                    */
			my_popt.topt.stop_table = true;
		}
		else if (fout == stdout && !is_pager)
		{
			/*
			 * 若查询需多结果集，hack 确保                     
			 * 全程仅一个分页器实例                    
			 */
			fout = PageOutput(INT_MAX, &(my_popt.topt));
			is_pager = true;
		}

		printQuery(result, &my_popt, fout, is_pager, pset.logfile);

		ClearOrSaveResult(result);

		/* 首个结果集后禁止表头装饰                   */
		my_popt.topt.start_table = false;
		my_popt.topt.prior_records += ntuples;

		/*
		 * 刷新输出流使中间结果立即可见。                    
		 * 查结果因分页器若终止/退出，                           
		 * 再投数据无意义。                                        
		 *    
		 */
		flush_error = fflush(fout);

		/*
		 * 查是否到末尾、按取消、或刷新出错，             
		 * 或输出流任何错误。                                    
		 * 写入出错推测 $PAGER 消失，                         
		 * 停止继续取数据。                                           
		 *                                       
		 */
		if (ntuples < fetch_count || cancel_pressed || flush_error ||
			ferror(fout))
			break;
	}

	if (pset.gfname)
	{
		/* 关闭 \g 参数文件/管  */
		if (is_pipe)
		{
			SetShellResultVariables(pclose(fout));
			restore_sigpipe_trap();
		}
		else
			fclose(fout);
	}
	else if (is_pager)
	{
		/* 关闭临时分页器 */
		ClosePager(fout);
	}

	if (OK)
	{
		/*
		 * 此处无 PGresult，有也不会有正确行数，               
		 * 模拟 SetResultVariables()。错况下                            
		 * 已在上方设了结果变量。         
		 */
		char		buf[32];

		SetVariable(pset.vars, "ERROR", "false");
		SetVariable(pset.vars, "SQLSTATE", "00000");
		snprintf(buf, sizeof(buf), INT64_FORMAT, total_tuples);
		SetVariable(pset.vars, "ROW_COUNT", buf);
	}

cleanup:
	if (timing)
		INSTR_TIME_SET_CURRENT(before);

	/*
	 * 无论成败均尝试关游标，失败时                             
	 * 忽略结果（可仅中止事务中抱怨）                          
	 *             
	 */
	result = PQexec(pset.db, "CLOSE _psql_cursor");
	if (OK)
	{
		OK = AcceptResult(result, true) &&
			(PQresultStatus(result) == PGRES_COMMAND_OK);
		ClearOrSaveResult(result);
	}
	else
		PQclear(result);

	if (started_txn)
	{
		result = PQexec(pset.db, OK ? "COMMIT" : "ROLLBACK");
		OK &= AcceptResult(result, true) &&
			(PQresultStatus(result) == PGRES_COMMAND_OK);
		ClearOrSaveResult(result);
	}

	if (timing)
	{
		INSTR_TIME_SET_CURRENT(after);
		INSTR_TIME_SUBTRACT(after, before);
		*elapsed_msec += INSTR_TIME_GET_MILLISEC(after);
	}

	return OK;
}


/*
 * 将字指针推进过空白和 SQL 注释。                     
 */
static const char *
skip_white_space(const char *query)
{
	int			cnestlevel = 0; /* 斜杠-星号注释嵌套层  */

	while (*query)
	{
		int			mblen = PQmblenBounded(query, pset.encoding);

		/*
		 * 注：假设编码是 ASCII 超集，故                        
		 * "query[0] == '/'" 有意义。但不假设                         
		 * 多字节后续字节不像 ASCII 字符；                  
		 * 关键：未精确认跳过字符时，                          
		 * 须按 mblen 非 1 推进。                               
		 *                                
		 */
		if (isspace((unsigned char) *query))
			query += mblen;
		else if (query[0] == '/' && query[1] == '*')
		{
			cnestlevel++;
			query += 2;
		}
		else if (cnestlevel > 0 && query[0] == '*' && query[1] == '/')
		{
			cnestlevel--;
			query += 2;
		}
		else if (cnestlevel == 0 && query[0] == '-' && query[1] == '-')
		{
			query += 2;

			/*
			 * 须跳到行尾，-- 注释内斜杠-星号                   
			 * 不启动斜杠-星号注释。                
			 */
			while (*query)
			{
				if (*query == '\n')
				{
					query++;
					break;
				}
				query += PQmblenBounded(query, pset.encoding);
			}
		}
		else if (cnestlevel > 0)
			query += mblen;
		else
			break;				/* 找到第一个记*/
	}

	return query;
}


/*
 * 检查是否不应启动新事务块                                 
 * （即发前置 BEGIN）命令。                    
 *
 *                                                                  
 * 含事务控制语句及后端                                          
 */
static bool
command_no_begin(const char *query)
{
	int			wordlen;

	/*
	 * 先推进过任何空白和注释。                   
	 */
	query = skip_white_space(query);

	/*
	 * 查单词长度（"beginx" 非 "begin"）。      
	 */
	wordlen = 0;
	while (isalpha((unsigned char) query[wordlen]))
		wordlen += PQmblenBounded(&query[wordlen], pset.encoding);

	/*
	 * 事务控制命令。含后端语法产生                            
	 * TransactionStmt 关键字（保存点除外）。                     
	 *                            
	 *
	 * （假设 START 必 START TRANSACTION，                       
	 * 因无其他 "START foo" 命令。）   
	 */
	if (wordlen == 5 && pg_strncasecmp(query, "abort", 5) == 0)
		return true;
	if (wordlen == 5 && pg_strncasecmp(query, "begin", 5) == 0)
		return true;
	if (wordlen == 5 && pg_strncasecmp(query, "start", 5) == 0)
		return true;
	if (wordlen == 6 && pg_strncasecmp(query, "commit", 6) == 0)
		return true;
	if (wordlen == 3 && pg_strncasecmp(query, "end", 3) == 0)
		return true;
	if (wordlen == 8 && pg_strncasecmp(query, "rollback", 8) == 0)
		return true;
	if (wordlen == 7 && pg_strncasecmp(query, "prepare", 7) == 0)
	{
		/* PREPARE TRANSACTION TC 命令，PREPARE foo 非         */
		query += wordlen;

		query = skip_white_space(query);

		wordlen = 0;
		while (isalpha((unsigned char) query[wordlen]))
			wordlen += PQmblenBounded(&query[wordlen], pset.encoding);

		if (wordlen == 11 && pg_strncasecmp(query, "transaction", 11) == 0)
			return true;
		return false;
	}

	/*
	 * 事务中不准命令。与后端                                    
	 * PreventInTransactionBlock() 调用一致。                          
	 *             
	 */
	if (wordlen == 6 && pg_strncasecmp(query, "vacuum", 6) == 0)
		return true;
	if (wordlen == 7 && pg_strncasecmp(query, "cluster", 7) == 0)
	{
		/* CLUSTER 带参数事务中允许                      */
		query += wordlen;

		query = skip_white_space(query);

		if (isalpha((unsigned char) query[0]))
			return false;		/* 有其他单词      */
		return true;			/* 是不带参数 CLUSTER        */
	}

	if (wordlen == 6 && pg_strncasecmp(query, "create", 6) == 0)
	{
		query += wordlen;

		query = skip_white_space(query);

		wordlen = 0;
		while (isalpha((unsigned char) query[wordlen]))
			wordlen += PQmblenBounded(&query[wordlen], pset.encoding);

		if (wordlen == 8 && pg_strncasecmp(query, "database", 8) == 0)
			return true;
		if (wordlen == 10 && pg_strncasecmp(query, "tablespace", 10) == 0)
			return true;

		/* CREATE [UNIQUE] INDEX CONCURRENTLY 事务中不许        */
		if (wordlen == 6 && pg_strncasecmp(query, "unique", 6) == 0)
		{
			query += wordlen;

			query = skip_white_space(query);

			wordlen = 0;
			while (isalpha((unsigned char) query[wordlen]))
				wordlen += PQmblenBounded(&query[wordlen], pset.encoding);
		}

		if (wordlen == 5 && pg_strncasecmp(query, "index", 5) == 0)
		{
			query += wordlen;

			query = skip_white_space(query);

			wordlen = 0;
			while (isalpha((unsigned char) query[wordlen]))
				wordlen += PQmblenBounded(&query[wordlen], pset.encoding);

			if (wordlen == 12 && pg_strncasecmp(query, "concurrently", 12) == 0)
				return true;
		}

		return false;
	}

	if (wordlen == 5 && pg_strncasecmp(query, "alter", 5) == 0)
	{
		query += wordlen;

		query = skip_white_space(query);

		wordlen = 0;
		while (isalpha((unsigned char) query[wordlen]))
			wordlen += PQmblenBounded(&query[wordlen], pset.encoding);

		/* ALTER SYSTEM 事务中不许        */
		if (wordlen == 6 && pg_strncasecmp(query, "system", 6) == 0)
			return true;

		return false;
	}

	/*
	 * 注：测试匹配 DROP SYSTEM 和 REINDEX TABLESPACE，              
	 * 非真有效命令无需在意。                                 
	 * 其他四种匹配正确。  
	 */
	if ((wordlen == 4 && pg_strncasecmp(query, "drop", 4) == 0) ||
		(wordlen == 7 && pg_strncasecmp(query, "reindex", 7) == 0))
	{
		query += wordlen;

		query = skip_white_space(query);

		wordlen = 0;
		while (isalpha((unsigned char) query[wordlen]))
			wordlen += PQmblenBounded(&query[wordlen], pset.encoding);

		if (wordlen == 8 && pg_strncasecmp(query, "database", 8) == 0)
			return true;
		if (wordlen == 6 && pg_strncasecmp(query, "system", 6) == 0)
			return true;
		if (wordlen == 10 && pg_strncasecmp(query, "tablespace", 10) == 0)
			return true;
		if (wordlen == 5 && (pg_strncasecmp(query, "index", 5) == 0 ||
							 pg_strncasecmp(query, "table", 5) == 0))
		{
			query += wordlen;
			query = skip_white_space(query);
			wordlen = 0;
			while (isalpha((unsigned char) query[wordlen]))
				wordlen += PQmblenBounded(&query[wordlen], pset.encoding);

			/*
			 * REINDEX [ TABLE | INDEX ] CONCURRENTLY                   
			 * 事务
			 */
			if (wordlen == 12 && pg_strncasecmp(query, "concurrently", 12) == 0)
				return true;
		}

		/* DROP INDEX CONCURRENTLY 事务中不许        */
		if (wordlen == 5 && pg_strncasecmp(query, "index", 5) == 0)
		{
			query += wordlen;

			query = skip_white_space(query);

			wordlen = 0;
			while (isalpha((unsigned char) query[wordlen]))
				wordlen += PQmblenBounded(&query[wordlen], pset.encoding);

			if (wordlen == 12 && pg_strncasecmp(query, "concurrently", 12) == 0)
				return true;

			return false;
		}

		return false;
	}

	/* DISCARD ALL 事务中不许，其他变体允许。                 */
	if (wordlen == 7 && pg_strncasecmp(query, "discard", 7) == 0)
	{
		query += wordlen;

		query = skip_white_space(query);

		wordlen = 0;
		while (isalpha((unsigned char) query[wordlen]))
			wordlen += PQmblenBounded(&query[wordlen], pset.encoding);

		if (wordlen == 3 && pg_strncasecmp(query, "all", 3) == 0)
			return true;
		return false;
	}

	return false;
}


/*
 * 检查是否为 SELECT（或 VALUES）。                   
 */
static bool
is_select_command(const char *query)
{
	int			wordlen;

	/*
	 * 先推进空白、注释和左括号。                          
	 */
	for (;;)
	{
		query = skip_white_space(query);
		if (query[0] == '(')
			query++;
		else
			break;
	}

	/*
	 * 查单词长（"selectx" 非 "select"）。         
	 */
	wordlen = 0;
	while (isalpha((unsigned char) query[wordlen]))
		wordlen += PQmblenBounded(&query[wordlen], pset.encoding);

	if (wordlen == 6 && pg_strncasecmp(query, "select", 6) == 0)
		return true;

	if (wordlen == 6 && pg_strncasecmp(query, "values", 6) == 0)
		return true;

	return false;
}


/*
 * 测试当前用户是否数据库超级用户。 
 */
bool
is_superuser(void)
{
	const char *val;

	if (!pset.db)
		return false;

	val = PQparameterStatus(pset.db, "is_superuser");

	if (val && strcmp(val, "on") == 0)
		return true;

	return false;
}


/*
 * 测试当前会话用标准字符串字面量。          
 */
bool
standard_strings(void)
{
	const char *val;

	if (!pset.db)
		return false;

	val = PQparameterStatus(pset.db, "standard_conforming_strings");

	if (val && strcmp(val, "on") == 0)
		return true;

	return false;
}


/*
 * 返回当前连接会话用户。                 
 */
const char *
session_username(void)
{
	const char *val;

	if (!pset.db)
		return NULL;

	val = PQparameterStatus(pset.db, "session_authorization");
	if (val)
		return val;
	else
		return PQuser(pset.db);
}


/* expand_tilde
 *
 *                                                                 
 *
 */
void
expand_tilde(char **filename)
{
	if (!filename || !(*filename))
		return;

	/*
	 * WIN32 不用波浪号扩文件名。用波浪号                     
	 * 作长文件名短版，但波浪号通常在尾部               
	 * 非开头。                         
	 */
#ifndef WIN32

	/* 尝试波浪号扩  */
	if (**filename == '~')
	{
		char	   *fn;
		char		oldp,
				   *p;
		struct passwd *pw;
		char		home[MAXPGPATH];

		fn = *filename;
		*home = '\0';

		p = fn + 1;
		while (*p != '/' && *p != '\0')
			p++;

		oldp = *p;
		*p = '\0';

		if (*(fn + 1) == '\0')
			get_home_path(home);	/* ~ 或 ~/     */
		else if ((pw = getpwnam(fn + 1)) != NULL)
			strlcpy(home, pw->pw_dir, sizeof(home));	/* ~user */

		*p = oldp;
		if (strlen(home) != 0)
		{
			char	   *newfn;

			newfn = psprintf("%s%s", home, p);
			free(fn);
			*filename = newfn;
		}
	}
#endif
}

/*
 * 检查连接串是否以任一有效的 URI 前缀指示符           
 * 开头。   
 *
 * 返回 URI 前缀的长度；若串不含 URI 前缀，则返回 0。       
 *
 * XXX 这与 libpq 中的同名函数重复。            
 */
static int
uri_prefix_length(const char *connstr)
{
	/* 连接 URI 须以下指示符开头：                                  */
	static const char uri_designator[] = "postgresql://";
	static const char short_uri_designator[] = "postgres://";

	if (strncmp(connstr, uri_designator,
				sizeof(uri_designator) - 1) == 0)
		return sizeof(uri_designator) - 1;

	if (strncmp(connstr, short_uri_designator,
				sizeof(short_uri_designator) - 1) == 0)
		return sizeof(short_uri_designator) - 1;

	return 0;
}

/*
 * 重置 \bind 相关状态   
 *
 *                                                                         
 * 清理绑定参数和 bind_flag 状态。                   
 */
void
clean_bind_state(void)
{
	if (pset.bind_flag)
	{
		for (int i = 0; i < pset.bind_nparams; i++)
			free(pset.bind_params[i]);
		free(pset.bind_params);
	}

	pset.bind_params = NULL;
	pset.bind_flag = false;
}

/*
 * 可识别连接串有效 URI 前缀开头                            
 * 或含 "="。        
 *
 *                                                                         
 * 须与 parse_connection_string 一致：                              
 *
 * 此返回 true 至少可被解析。                    
 */
bool
recognized_connection_string(const char *connstr)
{
	return uri_prefix_length(connstr) != 0 || strchr(connstr, '=') != NULL;
}
