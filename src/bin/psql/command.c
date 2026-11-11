/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/bin/psql/command.c
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <time.h>
#include <pwd.h>
#include <utime.h>
#ifndef WIN32
#include <sys/stat.h>			/* 用于 stat() */
#include <sys/time.h>			/* 用于 setitimer() */
#include <fcntl.h>				/* open() 标志 */
#include <unistd.h>				/* 用于 geteuid()、getpid()、stat() */
#else
#include <win32.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <sys/stat.h>			/* 用于 stat() */
#endif

#include "catalog/pg_class_d.h"
#include "command.h"
#include "common.h"
#include "common/logging.h"
#include "common/string.h"
#include "copy.h"
#include "crosstabview.h"
#include "describe.h"
#include "fe_utils/cancel.h"
#include "fe_utils/print.h"
#include "fe_utils/string_utils.h"
#include "help.h"
#include "input.h"
#include "large_obj.h"
#include "libpq-fe.h"
#include "libpq/pqcomm.h"
#include "mainloop.h"
#include "portability/instr_time.h"
#include "pqexpbuffer.h"
#include "psqlscanslash.h"
#include "settings.h"
#include "variables.h"

/*
 * 可编辑的数据库对象类型。
 */
typedef enum EditableObjectType
{
	EditableFunction,
	EditableView
} EditableObjectType;

/* 局部函数声明 */
static backslashResult exec_command(const char *cmd,
									PsqlScanState scan_state,
									ConditionalStack cstack,
									PQExpBuffer query_buf,
									PQExpBuffer previous_buf);
static backslashResult exec_command_a(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_bind(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_C(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_connect(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_cd(PsqlScanState scan_state, bool active_branch,
									   const char *cmd);
static backslashResult exec_command_conninfo(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_copy(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_copyright(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_crosstabview(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_d(PsqlScanState scan_state, bool active_branch,
									  const char *cmd);
static bool exec_command_dfo(PsqlScanState scan_state, const char *cmd,
							 const char *pattern,
							 bool show_verbose, bool show_system);
static backslashResult exec_command_edit(PsqlScanState scan_state, bool active_branch,
										 PQExpBuffer query_buf, PQExpBuffer previous_buf);
static backslashResult exec_command_ef_ev(PsqlScanState scan_state, bool active_branch,
										  PQExpBuffer query_buf, bool is_func);
static backslashResult exec_command_echo(PsqlScanState scan_state, bool active_branch,
										 const char *cmd);
static backslashResult exec_command_elif(PsqlScanState scan_state, ConditionalStack cstack,
										 PQExpBuffer query_buf);
static backslashResult exec_command_else(PsqlScanState scan_state, ConditionalStack cstack,
										 PQExpBuffer query_buf);
static backslashResult exec_command_endif(PsqlScanState scan_state, ConditionalStack cstack,
										  PQExpBuffer query_buf);
static backslashResult exec_command_encoding(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_errverbose(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_f(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_g(PsqlScanState scan_state, bool active_branch,
									  const char *cmd);
static backslashResult process_command_g_options(char *first_option,
												 PsqlScanState scan_state,
												 bool active_branch,
												 const char *cmd);
static backslashResult exec_command_gdesc(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_getenv(PsqlScanState scan_state, bool active_branch,
										   const char *cmd);
static backslashResult exec_command_gexec(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_gset(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_help(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_html(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_include(PsqlScanState scan_state, bool active_branch,
											const char *cmd);
static backslashResult exec_command_if(PsqlScanState scan_state, ConditionalStack cstack,
									   PQExpBuffer query_buf);
static backslashResult exec_command_list(PsqlScanState scan_state, bool active_branch,
										 const char *cmd);
static backslashResult exec_command_lo(PsqlScanState scan_state, bool active_branch,
									   const char *cmd);
static backslashResult exec_command_out(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_print(PsqlScanState scan_state, bool active_branch,
										  PQExpBuffer query_buf, PQExpBuffer previous_buf);
static backslashResult exec_command_password(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_prompt(PsqlScanState scan_state, bool active_branch,
										   const char *cmd);
static backslashResult exec_command_pset(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_quit(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_reset(PsqlScanState scan_state, bool active_branch,
										  PQExpBuffer query_buf);
static backslashResult exec_command_restrict(PsqlScanState scan_state, bool active_branch,
											 const char *cmd);
static backslashResult exec_command_s(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_set(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_setenv(PsqlScanState scan_state, bool active_branch,
										   const char *cmd);
static backslashResult exec_command_sf_sv(PsqlScanState scan_state, bool active_branch,
										  const char *cmd, bool is_func);
static backslashResult exec_command_t(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_T(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_timing(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_unrestrict(PsqlScanState scan_state, bool active_branch,
											   const char *cmd);
static backslashResult exec_command_unset(PsqlScanState scan_state, bool active_branch,
										  const char *cmd);
static backslashResult exec_command_write(PsqlScanState scan_state, bool active_branch,
										  const char *cmd,
										  PQExpBuffer query_buf, PQExpBuffer previous_buf);
static backslashResult exec_command_watch(PsqlScanState scan_state, bool active_branch,
										  PQExpBuffer query_buf, PQExpBuffer previous_buf);
static backslashResult exec_command_x(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_z(PsqlScanState scan_state, bool active_branch,
									  const char *cmd);
static backslashResult exec_command_shell_escape(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_slash_command_help(PsqlScanState scan_state, bool active_branch);
static char *read_connect_arg(PsqlScanState scan_state);
static PQExpBuffer gather_boolean_expression(PsqlScanState scan_state);
static bool is_true_boolean_expression(PsqlScanState scan_state, const char *name);
static void ignore_boolean_expression(PsqlScanState scan_state);
static void ignore_slash_options(PsqlScanState scan_state);
static void ignore_slash_filepipe(PsqlScanState scan_state);
static void ignore_slash_whole_line(PsqlScanState scan_state);
static bool is_branching_command(const char *cmd);
static void save_query_text_state(PsqlScanState scan_state, ConditionalStack cstack,
								  PQExpBuffer query_buf);
static void discard_query_text(PsqlScanState scan_state, ConditionalStack cstack,
							   PQExpBuffer query_buf);
static bool copy_previous_query(PQExpBuffer query_buf, PQExpBuffer previous_buf);
static bool do_connect(enum trivalue reuse_previous_specification,
					   char *dbname, char *user, char *host, char *port);
static bool do_edit(const char *filename_arg, PQExpBuffer query_buf,
					int lineno, bool discard_on_quit, bool *edited);
static bool do_shell(const char *command);
static bool do_watch(PQExpBuffer query_buf, double sleep, int iter);
static bool lookup_object_oid(EditableObjectType obj_type, const char *desc,
							  Oid *obj_oid);
static bool get_create_object_cmd(EditableObjectType obj_type, Oid oid,
								  PQExpBuffer buf);
static int	strip_lineno_from_objdesc(char *obj);
static int	count_lines_in_buf(PQExpBuffer buf);
static void print_with_linenumbers(FILE *output, char *lines, bool is_func);
static void minimal_error_message(PGresult *res);

static void printSSLInfo(void);
static void printGSSInfo(void);
static bool printPsetInfo(const char *param, printQueryOpt *popt);
static char *pset_value_string(const char *param, printQueryOpt *popt);

#ifdef WIN32
static void checkWin32Codepage(void);
#endif

static bool restricted;
static char *restrict_key;


/*----------
 * HandleSlashCmds:
 *
 * 处理所有以 '\' 开头的各种命令。           
 * 通常由 MainLoop() 调用。  
 *
 * scan_state 是一个词法分析器工作状态，被设置为      
 * 在 '\' 之后继续扫描。返回时词法分析器越过命令  
 * 及所有参数。  
 *
 * cstack 是当前的 \if 栈状态。它将被检查，            
 * 并可能被条件命令修改。         
 *
 * query_buf 包含目前为止的查询，它可能被         
 * 反斜杠命令的执行所修改（例如 \r 清空它）。  
 *
 * previous_buf 包含最近发送到服务器的查询            
 * （如果尚无则为空）。此处不应修改它，但某些 
 * 命令将其内容复制到 query_buf。 
 *
 * 执行 "-c" 命令行选项时 query_buf 和 previous_buf    
 * 将为 NULL。      
 *
 * 返回一个指明所期望动作的状态码，参见 command.h。    
 *----------
 */

backslashResult
HandleSlashCmds(PsqlScanState scan_state,
				ConditionalStack cstack,
				PQExpBuffer query_buf,
				PQExpBuffer previous_buf)
{
	backslashResult status;
	char	   *cmd;
	char	   *arg;

	Assert(scan_state != NULL);
	Assert(cstack != NULL);

	/* 解析出命令名称 */
	cmd = psql_scan_slash_command(scan_state);

	/*
	 * 然后尝试执行它。
	 *
	 * 如果我们处于"受限"模式，唯一允许的反斜杠命令是   
	 * \unrestrict（用于退出受限模式）。
	 */
	if (restricted && strcmp(cmd, "unrestrict") != 0)
	{
		pg_log_error("backslash commands are restricted; only \\unrestrict is allowed");
		status = PSQL_CMD_ERROR;
	}
	else
		status = exec_command(cmd, scan_state, cstack, query_buf, previous_buf);

	if (status == PSQL_CMD_UNKNOWN)
	{
		pg_log_error("invalid command \\%s", cmd);
		if (pset.cur_cmd_interactive)
			pg_log_error_hint("Try \\? for help.");
		status = PSQL_CMD_ERROR;
	}

	if (status != PSQL_CMD_ERROR)
	{
		/*
 * 消费有效命令后的所有剩余参数。我们想要       
 * 在这种情况下抑制反引号的求值，因此临时           
 * 压入一个非活动的条件栈条目。 
   */
		bool		active_branch = conditional_active(cstack);

		conditional_stack_push(cstack, IFSTATE_IGNORED);
		while ((arg = psql_scan_slash_option(scan_state,
											 OT_NORMAL, NULL, false)))
		{
			if (active_branch)
				pg_log_warning("\\%s: extra argument \"%s\" ignored", cmd, arg);
			free(arg);
		}
		conditional_stack_pop(cstack);
	}
	else
	{
		/* 在错误命令后静默丢弃该行的剩余部分 */
		while ((arg = psql_scan_slash_option(scan_state,
											 OT_WHOLE_LINE, NULL, false)))
			free(arg);
	}

	/* 如果有尾随的 \\\\，吞掉它 */
	psql_scan_slash_command_end(scan_state);

	free(cmd);

	/* 某些命令会写入 queryFout，因此确保输出已发送 */
	fflush(pset.queryFout);

	return status;
}


/*
 * 实际尝试执行反斜杠命令的子例程。
 *
 * 典型的"成功"结果码是 PSQL_CMD_SKIP_LINE，尽管某些
 * 命令返回其他结果。失败结果码是 PSQL_CMD_ERROR，
 * 除非 PSQL_CMD_UNKNOWN 更合适。
 */
static backslashResult
exec_command(const char *cmd,
			 PsqlScanState scan_state,
			 ConditionalStack cstack,
			 PQExpBuffer query_buf,
			 PQExpBuffer previous_buf)
{
	backslashResult status;
	bool		active_branch = conditional_active(cstack);

	/*
	 * 在交互模式下，当我们在假的 \if 分支中忽略命令时发出警告。
	 * 但我们继续执行，以便解析并丢弃正确数量的参数文本。
	 * 各个反斜杠命令子例程负责在 !active_branch 时，          
	 * 在丢弃适当的参数后不做任何事。                
	  *
	 */
	if (pset.cur_cmd_interactive && !active_branch &&
		!is_branching_command(cmd))
	{
		pg_log_warning("\\%s command ignored; use \\endif or Ctrl-C to exit current \\if block",
					   cmd);
	}

	if (strcmp(cmd, "a") == 0)
		status = exec_command_a(scan_state, active_branch);
	else if (strcmp(cmd, "bind") == 0)
		status = exec_command_bind(scan_state, active_branch);
	else if (strcmp(cmd, "C") == 0)
		status = exec_command_C(scan_state, active_branch);
	else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "connect") == 0)
		status = exec_command_connect(scan_state, active_branch);
	else if (strcmp(cmd, "cd") == 0)
		status = exec_command_cd(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "conninfo") == 0)
		status = exec_command_conninfo(scan_state, active_branch);
	else if (pg_strcasecmp(cmd, "copy") == 0)
		status = exec_command_copy(scan_state, active_branch);
	else if (strcmp(cmd, "copyright") == 0)
		status = exec_command_copyright(scan_state, active_branch);
	else if (strcmp(cmd, "crosstabview") == 0)
		status = exec_command_crosstabview(scan_state, active_branch);
	else if (cmd[0] == 'd')
		status = exec_command_d(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "e") == 0 || strcmp(cmd, "edit") == 0)
		status = exec_command_edit(scan_state, active_branch,
								   query_buf, previous_buf);
	else if (strcmp(cmd, "ef") == 0)
		status = exec_command_ef_ev(scan_state, active_branch, query_buf, true);
	else if (strcmp(cmd, "ev") == 0)
		status = exec_command_ef_ev(scan_state, active_branch, query_buf, false);
	else if (strcmp(cmd, "echo") == 0 || strcmp(cmd, "qecho") == 0 ||
			 strcmp(cmd, "warn") == 0)
		status = exec_command_echo(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "elif") == 0)
		status = exec_command_elif(scan_state, cstack, query_buf);
	else if (strcmp(cmd, "else") == 0)
		status = exec_command_else(scan_state, cstack, query_buf);
	else if (strcmp(cmd, "endif") == 0)
		status = exec_command_endif(scan_state, cstack, query_buf);
	else if (strcmp(cmd, "encoding") == 0)
		status = exec_command_encoding(scan_state, active_branch);
	else if (strcmp(cmd, "errverbose") == 0)
		status = exec_command_errverbose(scan_state, active_branch);
	else if (strcmp(cmd, "f") == 0)
		status = exec_command_f(scan_state, active_branch);
	else if (strcmp(cmd, "g") == 0 || strcmp(cmd, "gx") == 0)
		status = exec_command_g(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "gdesc") == 0)
		status = exec_command_gdesc(scan_state, active_branch);
	else if (strcmp(cmd, "getenv") == 0)
		status = exec_command_getenv(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "gexec") == 0)
		status = exec_command_gexec(scan_state, active_branch);
	else if (strcmp(cmd, "gset") == 0)
		status = exec_command_gset(scan_state, active_branch);
	else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0)
		status = exec_command_help(scan_state, active_branch);
	else if (strcmp(cmd, "H") == 0 || strcmp(cmd, "html") == 0)
		status = exec_command_html(scan_state, active_branch);
	else if (strcmp(cmd, "i") == 0 || strcmp(cmd, "include") == 0 ||
			 strcmp(cmd, "ir") == 0 || strcmp(cmd, "include_relative") == 0)
		status = exec_command_include(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "if") == 0)
		status = exec_command_if(scan_state, cstack, query_buf);
	else if (strcmp(cmd, "l") == 0 || strcmp(cmd, "list") == 0 ||
			 strcmp(cmd, "l+") == 0 || strcmp(cmd, "list+") == 0)
		status = exec_command_list(scan_state, active_branch, cmd);
	else if (strncmp(cmd, "lo_", 3) == 0)
		status = exec_command_lo(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "o") == 0 || strcmp(cmd, "out") == 0)
		status = exec_command_out(scan_state, active_branch);
	else if (strcmp(cmd, "p") == 0 || strcmp(cmd, "print") == 0)
		status = exec_command_print(scan_state, active_branch,
									query_buf, previous_buf);
	else if (strcmp(cmd, "password") == 0)
		status = exec_command_password(scan_state, active_branch);
	else if (strcmp(cmd, "prompt") == 0)
		status = exec_command_prompt(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "pset") == 0)
		status = exec_command_pset(scan_state, active_branch);
	else if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0)
		status = exec_command_quit(scan_state, active_branch);
	else if (strcmp(cmd, "r") == 0 || strcmp(cmd, "reset") == 0)
		status = exec_command_reset(scan_state, active_branch, query_buf);
	else if (strcmp(cmd, "restrict") == 0)
		status = exec_command_restrict(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "s") == 0)
		status = exec_command_s(scan_state, active_branch);
	else if (strcmp(cmd, "set") == 0)
		status = exec_command_set(scan_state, active_branch);
	else if (strcmp(cmd, "setenv") == 0)
		status = exec_command_setenv(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "sf") == 0 || strcmp(cmd, "sf+") == 0)
		status = exec_command_sf_sv(scan_state, active_branch, cmd, true);
	else if (strcmp(cmd, "sv") == 0 || strcmp(cmd, "sv+") == 0)
		status = exec_command_sf_sv(scan_state, active_branch, cmd, false);
	else if (strcmp(cmd, "t") == 0)
		status = exec_command_t(scan_state, active_branch);
	else if (strcmp(cmd, "T") == 0)
		status = exec_command_T(scan_state, active_branch);
	else if (strcmp(cmd, "timing") == 0)
		status = exec_command_timing(scan_state, active_branch);
	else if (strcmp(cmd, "unrestrict") == 0)
		status = exec_command_unrestrict(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "unset") == 0)
		status = exec_command_unset(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "w") == 0 || strcmp(cmd, "write") == 0)
		status = exec_command_write(scan_state, active_branch, cmd,
									query_buf, previous_buf);
	else if (strcmp(cmd, "watch") == 0)
		status = exec_command_watch(scan_state, active_branch,
									query_buf, previous_buf);
	else if (strcmp(cmd, "x") == 0)
		status = exec_command_x(scan_state, active_branch);
	else if (strcmp(cmd, "z") == 0 || strcmp(cmd, "zS") == 0)
		status = exec_command_z(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "!") == 0)
		status = exec_command_shell_escape(scan_state, active_branch);
	else if (strcmp(cmd, "?") == 0)
		status = exec_command_slash_command_help(scan_state, active_branch);
	else
		status = PSQL_CMD_UNKNOWN;

	/*
	 * 所有返回 PSQL_CMD_SEND 的命令都希望在 query_buf 为空时   
	 * 执行 previous_buf。为方便起见，我们在此处实现它，   
	 * 而不是在各个命令子例程中实现。
	 */
	if (status == PSQL_CMD_SEND)
		(void) copy_previous_query(query_buf, previous_buf);

	return status;
}


/*
 * \a —— 切换字段对齐
 *
 * 这没什么意义，但我们保留它。
 */
static backslashResult
exec_command_a(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		if (pset.popt.topt.format != PRINT_ALIGNED)
			success = do_pset("format", "aligned", &pset.popt, pset.quiet);
		else
			success = do_pset("format", "unaligned", &pset.popt, pset.quiet);
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \bind —— 设置查询参数
 */
static backslashResult
exec_command_bind(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		char	   *opt;
		int			nparams = 0;
		int			nalloc = 0;

		clean_bind_state();

		while ((opt = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, false)))
		{
			nparams++;
			if (nparams > nalloc)
			{
				nalloc = nalloc ? nalloc * 2 : 1;
				pset.bind_params = pg_realloc_array(pset.bind_params, char *, nalloc);
			}
			pset.bind_params[nparams - 1] = opt;
		}

		pset.bind_nparams = nparams;
		pset.bind_flag = true;
	}

	return status;
}

/*
 * \C —— 覆盖表标题（以前是修改 HTML 标题）
 */
static backslashResult
exec_command_C(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);

		success = do_pset("title", opt, &pset.popt, pset.quiet);
		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \c 或 \connect —— 使用指定参数连接到数据库。       
 *
 * \c [-reuse-previous=BOOL] dbname user host port
 *
 * 将参数指定为 '-' 等同于省略它。示例：
 *
 * \c - - hst		以当前用户身份连接到当前端口上 
 *					主机 "hst" 的当前数据库。
 * \c - usr - prt	以用户 "usr" 身份连接到当前主机上           
 *					端口 "prt" 的当前数据库。
 * \c dbs			以当前用户身份连接到当前主机上            
 *					当前端口的数据库 "dbs"。
 */
static backslashResult
exec_command_connect(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		static const char prefix[] = "-reuse-previous=";
		char	   *opt1,
				   *opt2,
				   *opt3,
				   *opt4;
		enum trivalue reuse_previous = TRI_DEFAULT;

		opt1 = read_connect_arg(scan_state);
		if (opt1 != NULL && strncmp(opt1, prefix, sizeof(prefix) - 1) == 0)
		{
			bool		on_off;

			success = ParseVariableBool(opt1 + sizeof(prefix) - 1,
										"-reuse-previous",
										&on_off);
			if (success)
			{
				reuse_previous = on_off ? TRI_YES : TRI_NO;
				free(opt1);
				opt1 = read_connect_arg(scan_state);
			}
		}

		if (success)			/* 如果 reuse_previous 无效则放弃 */
		{
			opt2 = read_connect_arg(scan_state);
			opt3 = read_connect_arg(scan_state);
			opt4 = read_connect_arg(scan_state);

			success = do_connect(reuse_previous, opt1, opt2, opt3, opt4);

			free(opt2);
			free(opt3);
			free(opt4);
		}
		free(opt1);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \cd —— 更改目录
 */
static backslashResult
exec_command_cd(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);
		char	   *dir;

		if (opt)
			dir = opt;
		else
		{
#ifndef WIN32
			/* 这应与 get_home_path() 匹配 */
			dir = getenv("HOME");
			if (dir == NULL || dir[0] == '\0')
			{
				uid_t		user_id = geteuid();
				struct passwd *pw;

				errno = 0;		/* 调用前清除 errno */
				pw = getpwuid(user_id);
				if (pw)
					dir = pw->pw_dir;
				else
				{
					pg_log_error("could not get home directory for user ID %ld: %s",
								 (long) user_id,
								 errno ? strerror(errno) : _("user does not exist"));
					success = false;
				}
			}
#else							/* WIN32 */

			/*
			 * 在 Windows 上，不带参数执行 'cd' 会打印当前
			 * 目录，如果有人想在这里也实现这个功能的话…
			 */
			dir = "/";
#endif							/* WIN32 */
		}

		if (success &&
			chdir(dir) < 0)
		{
			pg_log_error("\\%s: could not change directory to \"%s\": %m",
						 cmd, dir);
			success = false;
		}

		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \conninfo —— 显示当前连接的信息                 
 */
static backslashResult
exec_command_conninfo(PsqlScanState scan_state, bool active_branch)
{
	if (active_branch)
	{
		char	   *db = PQdb(pset.db);

		if (db == NULL)
			printf(_("You are currently not connected to a database.\n"));
		else
		{
			char	   *host = PQhost(pset.db);
			char	   *hostaddr = PQhostaddr(pset.db);

			if (is_unixsock_path(host))
			{
				/* hostaddr 会覆盖 host */
				if (hostaddr && *hostaddr)
					printf(_("You are connected to database \"%s\" as user \"%s\" on address \"%s\" at port \"%s\".\n"),
						   db, PQuser(pset.db), hostaddr, PQport(pset.db));
				else
					printf(_("You are connected to database \"%s\" as user \"%s\" via socket in \"%s\" at port \"%s\".\n"),
						   db, PQuser(pset.db), host, PQport(pset.db));
			}
			else
			{
				if (hostaddr && *hostaddr && strcmp(host, hostaddr) != 0)
					printf(_("You are connected to database \"%s\" as user \"%s\" on host \"%s\" (address \"%s\") at port \"%s\".\n"),
						   db, PQuser(pset.db), host, hostaddr, PQport(pset.db));
				else
					printf(_("You are connected to database \"%s\" as user \"%s\" on host \"%s\" at port \"%s\".\n"),
						   db, PQuser(pset.db), host, PQport(pset.db));
			}
			printSSLInfo();
			printGSSInfo();
		}
	}

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \copy —— 运行 COPY 命令
 */
static backslashResult
exec_command_copy(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_WHOLE_LINE, NULL, false);

		success = do_copy(opt);
		free(opt);
	}
	else
		ignore_slash_whole_line(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \copyright -- print copyright notice
 */
static backslashResult
exec_command_copyright(PsqlScanState scan_state, bool active_branch)
{
	if (active_branch)
		print_copyright();

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \crosstabview —— 执行查询并以交叉表形式显示结果
 */
static backslashResult
exec_command_crosstabview(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		int			i;

		for (i = 0; i < lengthof(pset.ctv_args); i++)
			pset.ctv_args[i] = psql_scan_slash_option(scan_state,
													  OT_NORMAL, NULL, true);
		pset.crosstab_flag = true;
		status = PSQL_CMD_SEND;
	}
	else
		ignore_slash_options(scan_state);

	return status;
}

/*
 * \d* 命令  
 */
static backslashResult
exec_command_d(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;
	bool		success = true;

	if (active_branch)
	{
		char	   *pattern;
		bool		show_verbose,
					show_system;

		/* 我们尚未对模式执行 SQLID 缩减 */
		pattern = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, true);

		show_verbose = strchr(cmd, '+') ? true : false;
		show_system = strchr(cmd, 'S') ? true : false;

		switch (cmd[1])
		{
			case '\0':
			case '+':
			case 'S':
				if (pattern)
					success = describeTableDetails(pattern, show_verbose, show_system);
				else
					/* 有趣对象的标准列表 */
					success = listTables("tvmsE", NULL, show_verbose, show_system);
				break;
			case 'A':
				{
					char	   *pattern2 = NULL;

					if (pattern && cmd[2] != '\0' && cmd[2] != '+')
						pattern2 = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, true);

					switch (cmd[2])
					{
						case '\0':
						case '+':
							success = describeAccessMethods(pattern, show_verbose);
							break;
						case 'c':
							success = listOperatorClasses(pattern, pattern2, show_verbose);
							break;
						case 'f':
							success = listOperatorFamilies(pattern, pattern2, show_verbose);
							break;
						case 'o':
							success = listOpFamilyOperators(pattern, pattern2, show_verbose);
							break;
						case 'p':
							success = listOpFamilyFunctions(pattern, pattern2, show_verbose);
							break;
						default:
							status = PSQL_CMD_UNKNOWN;
							break;
					}

					free(pattern2);
				}
				break;
			case 'a':
				success = describeAggregates(pattern, show_verbose, show_system);
				break;
			case 'b':
				success = describeTablespaces(pattern, show_verbose);
				break;
			case 'c':
				if (strncmp(cmd, "dconfig", 7) == 0)
					success = describeConfigurationParameters(pattern,
															  show_verbose,
															  show_system);
				else
					success = listConversions(pattern,
											  show_verbose,
											  show_system);
				break;
			case 'C':
				success = listCasts(pattern, show_verbose);
				break;
			case 'd':
				if (strncmp(cmd, "ddp", 3) == 0)
					success = listDefaultACLs(pattern);
				else
					success = objectDescription(pattern, show_system);
				break;
			case 'D':
				success = listDomains(pattern, show_verbose, show_system);
				break;
			case 'f':			/* 函数子系统 */
				switch (cmd[2])
				{
					case '\0':
					case '+':
					case 'S':
					case 'a':
					case 'n':
					case 'p':
					case 't':
					case 'w':
						success = exec_command_dfo(scan_state, cmd, pattern,
												   show_verbose, show_system);
						break;
					default:
						status = PSQL_CMD_UNKNOWN;
						break;
				}
				break;
			case 'g':
				/* 不再与 \du 区分        */
				success = describeRoles(pattern, show_verbose, show_system);
				break;
			case 'l':
				success = listLargeObjects(show_verbose);
				break;
			case 'L':
				success = listLanguages(pattern, show_verbose, show_system);
				break;
			case 'n':
				success = listSchemas(pattern, show_verbose, show_system);
				break;
			case 'o':
				success = exec_command_dfo(scan_state, cmd, pattern,
										   show_verbose, show_system);
				break;
			case 'O':
				success = listCollations(pattern, show_verbose, show_system);
				break;
			case 'p':
				success = permissionsList(pattern, show_system);
				break;
			case 'P':
				{
					switch (cmd[2])
					{
						case '\0':
						case '+':
						case 't':
						case 'i':
						case 'n':
							success = listPartitionedTables(&cmd[2], pattern, show_verbose);
							break;
						default:
							status = PSQL_CMD_UNKNOWN;
							break;
					}
				}
				break;
			case 'T':
				success = describeTypes(pattern, show_verbose, show_system);
				break;
			case 't':
			case 'v':
			case 'm':
			case 'i':
			case 's':
			case 'E':
				success = listTables(&cmd[1], pattern, show_verbose, show_system);
				break;
			case 'r':
				if (cmd[2] == 'd' && cmd[3] == 's')
				{
					char	   *pattern2 = NULL;

					if (pattern)
						pattern2 = psql_scan_slash_option(scan_state,
														  OT_NORMAL, NULL, true);
					success = listDbRoleSettings(pattern, pattern2);

					free(pattern2);
				}
				else if (cmd[2] == 'g')
					success = describeRoleGrants(pattern, show_system);
				else
					status = PSQL_CMD_UNKNOWN;
				break;
			case 'R':
				switch (cmd[2])
				{
					case 'p':
						if (show_verbose)
							success = describePublications(pattern);
						else
							success = listPublications(pattern);
						break;
					case 's':
						success = describeSubscriptions(pattern, show_verbose);
						break;
					default:
						status = PSQL_CMD_UNKNOWN;
				}
				break;
			case 'u':
				success = describeRoles(pattern, show_verbose, show_system);
				break;
			case 'F':			/* 文本搜索子系统 */
				switch (cmd[2])
				{
					case '\0':
					case '+':
						success = listTSConfigs(pattern, show_verbose);
						break;
					case 'p':
						success = listTSParsers(pattern, show_verbose);
						break;
					case 'd':
						success = listTSDictionaries(pattern, show_verbose);
						break;
					case 't':
						success = listTSTemplates(pattern, show_verbose);
						break;
					default:
						status = PSQL_CMD_UNKNOWN;
						break;
				}
				break;
			case 'e':			/* SQL/MED 子系统 */
				switch (cmd[2])
				{
					case 's':
						success = listForeignServers(pattern, show_verbose);
						break;
					case 'u':
						success = listUserMappings(pattern, show_verbose);
						break;
					case 'w':
						success = listForeignDataWrappers(pattern, show_verbose);
						break;
					case 't':
						success = listForeignTables(pattern, show_verbose);
						break;
					default:
						status = PSQL_CMD_UNKNOWN;
						break;
				}
				break;
			case 'x':			/* 扩展 */
				if (show_verbose)
					success = listExtensionContents(pattern);
				else
					success = listExtensions(pattern);
				break;
			case 'X':			/* 扩展统计信息 */
				success = listExtendedStats(pattern);
				break;
			case 'y':			/* 事件触发器 */
				success = listEventTriggers(pattern, show_verbose);
				break;
			default:
				status = PSQL_CMD_UNKNOWN;
		}

		free(pattern);
	}
	else
		ignore_slash_options(scan_state);

	if (!success)
		status = PSQL_CMD_ERROR;

	return status;
}

/* \df 和 \do；足够复杂，从 exec_command_d 中分离出来 */
static bool
exec_command_dfo(PsqlScanState scan_state, const char *cmd,
				 const char *pattern,
				 bool show_verbose, bool show_system)
{
	bool		success;
	char	   *arg_patterns[FUNC_MAX_ARGS];
	int			num_arg_patterns = 0;

	/* 同时收集参数类型模式 */
	if (pattern)				/* 否则它只是 \df 或 \do      */
	{
		char	   *ap;

		while ((ap = psql_scan_slash_option(scan_state,
											OT_NORMAL, NULL, true)) != NULL)
		{
			arg_patterns[num_arg_patterns++] = ap;
			if (num_arg_patterns >= FUNC_MAX_ARGS)
				break;			/* 保护大小有限的数组 */
		}
	}

	if (cmd[1] == 'f')
		success = describeFunctions(&cmd[2], pattern,
									arg_patterns, num_arg_patterns,
									show_verbose, show_system);
	else
		success = describeOperators(pattern,
									arg_patterns, num_arg_patterns,
									show_verbose, show_system);

	while (--num_arg_patterns >= 0)
		free(arg_patterns[num_arg_patterns]);

	return success;
}

/*
 * \e 或 \edit —— 编辑当前查询缓冲区，或编辑文件并
 * 将其设为查询缓冲区
 */
static backslashResult
exec_command_edit(PsqlScanState scan_state, bool active_branch,
				  PQExpBuffer query_buf, PQExpBuffer previous_buf)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		if (!query_buf)
		{
			pg_log_error("no query buffer");
			status = PSQL_CMD_ERROR;
		}
		else
		{
			char	   *fname;
			char	   *ln = NULL;
			int			lineno = -1;

			fname = psql_scan_slash_option(scan_state,
										   OT_NORMAL, NULL, true);
			if (fname)
			{
				/* 尝试获取单独的行号参数 */
				ln = psql_scan_slash_option(scan_state,
											OT_NORMAL, NULL, true);
				if (ln == NULL)
				{
					/* 只有一个参数；也许它是行号而不是文件名 */
					if (fname[0] &&
						strspn(fname, "0123456789") == strlen(fname))
					{
						/* 全部为数字，因此假定它是行号 */
						ln = fname;
						fname = NULL;
					}
				}
			}
			if (ln)
			{
				lineno = atoi(ln);
				if (lineno < 1)
				{
					pg_log_error("invalid line number: %s", ln);
					status = PSQL_CMD_ERROR;
				}
			}
			if (status != PSQL_CMD_ERROR)
			{
				bool		discard_on_quit;

				expand_tilde(&fname);
				if (fname)
				{
					canonicalize_path_enc(fname, pset.encoding);
					/* 如果文件未被修改，始终清空缓冲区 */
					discard_on_quit = true;
				}
				else
				{
					/*
 * 如果 query_buf 为空，调用之前的查询进行
 * 编辑。但在这种情况下，如果编辑未修改文件，
 * 则应清空查询缓冲区。
 */
					discard_on_quit = copy_previous_query(query_buf,
														  previous_buf);
				}

				if (do_edit(fname, query_buf, lineno, discard_on_quit, NULL))
					status = PSQL_CMD_NEWEDIT;
				else
					status = PSQL_CMD_ERROR;
			}
			free(fname);
			free(ln);
		}
	}
	else
		ignore_slash_options(scan_state);

	return status;
}

/*
 * \ef/\ev —— 编辑命名的函数或视图，或
 * 在未提供参数时显示空白的 CREATE FUNCTION/VIEW 模板
 */
static backslashResult
exec_command_ef_ev(PsqlScanState scan_state, bool active_branch,
				   PQExpBuffer query_buf, bool is_func)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		char	   *obj_desc = psql_scan_slash_option(scan_state,
													  OT_WHOLE_LINE,
													  NULL, true);
		int			lineno = -1;

		if (!query_buf)
		{
			pg_log_error("no query buffer");
			status = PSQL_CMD_ERROR;
		}
		else
		{
			Oid			obj_oid = InvalidOid;
			EditableObjectType eot = is_func ? EditableFunction : EditableView;

			lineno = strip_lineno_from_objdesc(obj_desc);
			if (lineno == 0)
			{
				/* 错误已报告 */
				status = PSQL_CMD_ERROR;
			}
			else if (!obj_desc)
			{
				/* 设置一个空命令来填充 */
				resetPQExpBuffer(query_buf);
				if (is_func)
					appendPQExpBufferStr(query_buf,
										 "CREATE FUNCTION ( )\n"
										 " RETURNS \n"
										 " LANGUAGE \n"
										 " -- common options:  IMMUTABLE  STABLE  STRICT  SECURITY DEFINER\n"
										 "AS $function$\n"
										 "\n$function$\n");
				else
					appendPQExpBufferStr(query_buf,
										 "CREATE VIEW  AS\n"
										 " SELECT \n"
										 "  -- something...\n");
			}
			else if (!lookup_object_oid(eot, obj_desc, &obj_oid))
			{
				/* 错误已报告 */
				status = PSQL_CMD_ERROR;
			}
			else if (!get_create_object_cmd(eot, obj_oid, query_buf))
			{
				/* 错误已报告 */
				status = PSQL_CMD_ERROR;
			}
			else if (is_func && lineno > 0)
			{
				/*
				 * 行号 "1" 应对应于函数体的第一行。      
				 * 我们期望 pg_get_functiondef() 将其输出在以    
				 * "AS "、"BEGIN " 或 "RETURN " 开头的行上，     
				 * 且在函数体的真正开始之前不会有这样的行。
				 * 将 lineno 递增该行之前的行数，使其相对于
				 * 函数定义的第一行。                         
				  *
				 */
				const char *lines = query_buf->data;

				while (*lines != '\0')
				{
					if (strncmp(lines, "AS ", 3) == 0 ||
						strncmp(lines, "BEGIN ", 6) == 0 ||
						strncmp(lines, "RETURN ", 7) == 0)
						break;
					lineno++;
					/* 查找下一行的开头 */
					lines = strchr(lines, '\n');
					if (!lines)
						break;
					lines++;
				}
			}
		}

		if (status != PSQL_CMD_ERROR)
		{
			bool		edited = false;

			if (!do_edit(NULL, query_buf, lineno, true, &edited))
				status = PSQL_CMD_ERROR;
			else if (!edited)
				puts(_("No changes"));
			else
				status = PSQL_CMD_NEWEDIT;
		}

		free(obj_desc);
	}
	else
		ignore_slash_whole_line(scan_state);

	return status;
}

/*
 * \echo、\qecho 和 \warn —— 将参数回显到 stdout、查询输出或 stderr
 */
static backslashResult
exec_command_echo(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	if (active_branch)
	{
		char	   *value;
		char		quoted;
		bool		no_newline = false;
		bool		first = true;
		FILE	   *fout;

		if (strcmp(cmd, "qecho") == 0)
			fout = pset.queryFout;
		else if (strcmp(cmd, "warn") == 0)
			fout = stderr;
		else
			fout = stdout;

		while ((value = psql_scan_slash_option(scan_state,
											   OT_NORMAL, &quoted, false)))
		{
			if (first && !no_newline && !quoted && strcmp(value, "-n") == 0)
				no_newline = true;
			else
			{
				if (first)
					first = false;
				else
					fputc(' ', fout);
				fputs(value, fout);
			}
			free(value);
		}
		if (!no_newline)
			fputs("\n", fout);
	}
	else
		ignore_slash_options(scan_state);

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \encoding —— 设置或显示客户端编码
 */
static backslashResult
exec_command_encoding(PsqlScanState scan_state, bool active_branch)
{
	if (active_branch)
	{
		char	   *encoding = psql_scan_slash_option(scan_state,
													  OT_NORMAL, NULL, false);

		if (!encoding)
		{
			/* 显示编码 */
			puts(pg_encoding_to_char(pset.encoding));
		}
		else
		{
			/* 设置编码 */
			if (PQsetClientEncoding(pset.db, encoding) == -1)
				pg_log_error("%s: invalid encoding name or conversion procedure not found", encoding);
			else
			{
				/* 将编码信息保存到 psql 内部数据中 */
				pset.encoding = PQclientEncoding(pset.db);
				pset.popt.topt.encoding = pset.encoding;
				setFmtEncoding(pset.encoding);
				SetVariable(pset.vars, "ENCODING",
							pg_encoding_to_char(pset.encoding));
			}
			free(encoding);
		}
	}
	else
		ignore_slash_options(scan_state);

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \errverbose —— 显示上次失败查询的详细消息   
 */
static backslashResult
exec_command_errverbose(PsqlScanState scan_state, bool active_branch)
{
	if (active_branch)
	{
		if (pset.last_error_result)
		{
			char	   *msg;

			msg = PQresultVerboseErrorMessage(pset.last_error_result,
											  PQERRORS_VERBOSE,
											  PQSHOW_CONTEXT_ALWAYS);
			if (msg)
			{
				pg_log_error("%s", msg);
				PQfreemem(msg);
			}
			else
				puts(_("out of memory"));
		}
		else
			puts(_("There is no previous error."));
	}

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \f —— 更改字段分隔符
 */
static backslashResult
exec_command_f(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, false);

		success = do_pset("fieldsep", fname, &pset.popt, pset.quiet);
		free(fname);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \g  [(pset-option[=pset-value] ...)] [filename/shell-command]
 * \gx [(pset-option[=pset-value] ...)] [filename/shell-command]
 *
 * 发送当前查询。如果指定了 pset 选项，它们仅在此查询中生效。
 * 如果给出了文件名或管道命令，查询输出将重定向到那里。
 * \gx 隐式设置 "expanded=on" 以及任何其他指定的 pset 选项。
  *
 */
static backslashResult
exec_command_g(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;
	char	   *fname;

	/*
 * 由于此处的选项处理相当复杂，我们先处理它，
 * 然后再决定分支是否处于活动状态。
 */
	fname = psql_scan_slash_option(scan_state,
								   OT_FILEPIPE, NULL, false);

	if (fname && fname[0] == '(')
	{
		/* 消费 pset 选项直到尾随的 ')' … */
		status = process_command_g_options(fname + 1, scan_state,
										   active_branch, cmd);
		free(fname);
		/* …然后再次尝试扫描文件名。 */
		fname = psql_scan_slash_option(scan_state,
									   OT_FILEPIPE, NULL, false);
	}

	if (status == PSQL_CMD_SKIP_LINE && active_branch)
	{
		if (!fname)
			pset.gfname = NULL;
		else
		{
			expand_tilde(&fname);
			pset.gfname = pg_strdup(fname);
		}
		if (strcmp(cmd, "gx") == 0)
		{
			/* 如果尚未保存设置，则保存之，然后强制 expanded=on */
			if (pset.gsavepopt == NULL)
				pset.gsavepopt = savePsetInfo(&pset.popt);
			pset.popt.topt.expanded = 1;
		}
		status = PSQL_CMD_SEND;
	}

	free(fname);

	return status;
}

/*
 * 处理 \g 的括号内 pset 选项       
 *
 * 注意：可以修改 first_option，但不能释放它；由调用者执行。
 */
static backslashResult
process_command_g_options(char *first_option, PsqlScanState scan_state,
						  bool active_branch, const char *cmd)
{
	bool		success = true;
	bool		found_r_paren = false;

	do
	{
		char	   *option;
		size_t		optlen;

		/* 如果不是第一次遍历，收集一个新选项 */
		if (first_option)
			option = first_option;
		else
		{
			option = psql_scan_slash_option(scan_state,
											OT_NORMAL, NULL, false);
			if (!option)
			{
				if (active_branch)
				{
					pg_log_error("\\%s: missing right parenthesis", cmd);
					success = false;
				}
				break;
			}
		}

		/* 检查终止右括号，并将其从字符串中删除 */
		optlen = strlen(option);
		if (optlen > 0 && option[optlen - 1] == ')')
		{
			option[--optlen] = '\0';
			found_r_paren = true;
		}

		/* 如果括号之外还有内容，解析并执行它 */
		if (optlen > 0)
		{
			/* 我们可以有 "name" 或 "name=value" */
			char	   *valptr = strchr(option, '=');

			if (valptr)
				*valptr++ = '\0';
			if (active_branch)
			{
				/* 如果尚未保存设置，则保存之，然后应用选项 */
				if (pset.gsavepopt == NULL)
					pset.gsavepopt = savePsetInfo(&pset.popt);
				success &= do_pset(option, valptr, &pset.popt, true);
			}
		}

		/* 清理此选项。我们不应释放 first_option。 */
		if (first_option)
			first_option = NULL;
		else
			free(option);
	} while (!found_r_paren);

	/* 如果在更改某些选项后失败，撤销副作用 */
	if (!success && active_branch && pset.gsavepopt)
	{
		restorePsetInfo(&pset.popt, pset.gsavepopt);
		pset.gsavepopt = NULL;
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \gdesc —— 描述查询结果
 */
static backslashResult
exec_command_gdesc(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		pset.gdesc_flag = true;
		status = PSQL_CMD_SEND;
	}

	return status;
}

/*
 * \getenv —— 从环境变量设置变量       
 */
static backslashResult
exec_command_getenv(PsqlScanState scan_state, bool active_branch,
					const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *myvar = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, false);
		char	   *envvar = psql_scan_slash_option(scan_state,
													OT_NORMAL, NULL, false);

		if (!myvar || !envvar)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			success = false;
		}
		else
		{
			char	   *envval = getenv(envvar);

			if (envval && !SetVariable(pset.vars, myvar, envval))
				success = false;
		}
		free(myvar);
		free(envvar);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \gexec —— 发送查询并执行结果的每个字段
 */
static backslashResult
exec_command_gexec(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		pset.gexec_flag = true;
		status = PSQL_CMD_SEND;
	}

	return status;
}

/*
 * \gset [prefix] —— 发送查询并将结果存储到变量中
 */
static backslashResult
exec_command_gset(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		char	   *prefix = psql_scan_slash_option(scan_state,
													OT_NORMAL, NULL, false);

		if (prefix)
			pset.gset_prefix = prefix;
		else
		{
			/* 我们必须设置非 NULL 前缀才能触发存储 */
			pset.gset_prefix = pg_strdup("");
		}
		/* gset_prefix 稍后释放 */
		status = PSQL_CMD_SEND;
	}
	else
		ignore_slash_options(scan_state);

	return status;
}

/*
 * \help [topic] —— 打印 SQL 命令的帮助信息
 */
static backslashResult
exec_command_help(PsqlScanState scan_state, bool active_branch)
{
	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_WHOLE_LINE, NULL, false);
		size_t		len;

		/* 去除尾随的空格和分号 */
		if (opt)
		{
			len = strlen(opt);
			while (len > 0 &&
				   (isspace((unsigned char) opt[len - 1])
					|| opt[len - 1] == ';'))
				opt[--len] = '\0';
		}

		helpSQL(opt, pset.popt.topt.pager);
		free(opt);
	}
	else
		ignore_slash_whole_line(scan_state);

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \H 和 \html —— 切换 HTML 格式化
 */
static backslashResult
exec_command_html(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		if (pset.popt.topt.format != PRINT_HTML)
			success = do_pset("format", "html", &pset.popt, pset.quiet);
		else
			success = do_pset("format", "aligned", &pset.popt, pset.quiet);
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \i 和 \ir —— 包含一个文件
 */
static backslashResult
exec_command_include(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, true);

		if (!fname)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			success = false;
		}
		else
		{
			bool		include_relative;

			include_relative = (strcmp(cmd, "ir") == 0
								|| strcmp(cmd, "include_relative") == 0);
			expand_tilde(&fname);
			success = (process_file(fname, include_relative) == EXIT_SUCCESS);
			free(fname);
		}
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \if <expr> —— 一个 \if..\endif 块的开始
 *
 * <expr> 被解析为布尔表达式。无效的表达式将发出
 * 警告并被视为 false。跟在 false 表达式后面的语句
 * 将被解析但被忽略。请注意，如果 \if 语句本身位于     
 * 块的非活动部分中，则整个内部 \if..\endif 块            
 * 将被解析但被忽略。
 */
static backslashResult
exec_command_if(PsqlScanState scan_state, ConditionalStack cstack,
				PQExpBuffer query_buf)
{
	if (conditional_active(cstack))
	{
		/*
 * 首先，压入一个新的活动栈条目；这确保词法分析器
 * 在扫描表达式时执行变量替换和反引号求值。
 * （无论如何都应该发生，因为我们知道我们在活动的
 * 外部分支中，但为了确保万无一失。）
 */
		conditional_stack_push(cstack, IFSTATE_TRUE);

		/* 记住当前查询状态，以备后续恢复之需 */
		save_query_text_state(scan_state, cstack, query_buf);

		/*
 * 求值表达式；如果为 false，则切换到非活动状态。
 */
		if (!is_true_boolean_expression(scan_state, "\\if expression"))
			conditional_stack_poke(cstack, IFSTATE_FALSE);
	}
	else
	{
		/*
 * 我们在一个非活动的外部分支中，因此整个 \if 块
 * 将被忽略。我们不想求值表达式，因此在扫描之前
 * 压入 "ignored" 栈状态。
 */
		conditional_stack_push(cstack, IFSTATE_IGNORED);

		/* 记住当前查询状态，以备后续恢复之需 */
		save_query_text_state(scan_state, cstack, query_buf);

		ignore_boolean_expression(scan_state);
	}

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \elif <expr> —— \if..\endif 块中的备选分支     
 *
 * <expr> 的求值方式与 \if <expr> 相同。
 */
static backslashResult
exec_command_elif(PsqlScanState scan_state, ConditionalStack cstack,
				  PQExpBuffer query_buf)
{
	bool		success = true;

	switch (conditional_stack_peek(cstack))
	{
		case IFSTATE_TRUE:

			/*
 * 刚刚完成此 \if 块的活动分支。更新保存的       
 * 状态，以便保留活动分支放入 query_buf 的
 * 任何数据。
 */
			save_query_text_state(scan_state, cstack, query_buf);

			/*
 * 丢弃 \elif 表达式并忽略直到 \endif 的内容。    
 * 在读取表达式之前切换状态以确保正确的词法分析器行为。
			  *
 */
			conditional_stack_poke(cstack, IFSTATE_IGNORED);
			ignore_boolean_expression(scan_state);
			break;
		case IFSTATE_FALSE:

			/*
 * 丢弃刚刚跳过的分支添加的查询文本。
 */
			discard_query_text(scan_state, cstack, query_buf);

			/*
 * 尚未在此 \if 块中找到 true 表达式，因此这          
 * 可能是第一个。我们必须在检查表达式之前更改状态，
 * 否则词法分析器不会做正确的事。
 */
			conditional_stack_poke(cstack, IFSTATE_TRUE);
			if (!is_true_boolean_expression(scan_state, "\\elif expression"))
				conditional_stack_poke(cstack, IFSTATE_FALSE);
			break;
		case IFSTATE_IGNORED:

			/*
 * 丢弃刚刚跳过的分支添加的查询文本。
 */
			discard_query_text(scan_state, cstack, query_buf);

			/*
 * 跳过表达式继续。\if 块要么已经有一个活动部分，
 * 要么整个块正在被跳过。
 */
			ignore_boolean_expression(scan_state);
			break;
		case IFSTATE_ELSE_TRUE:
		case IFSTATE_ELSE_FALSE:
			pg_log_error("\\elif: cannot occur after \\else");
			success = false;
			break;
		case IFSTATE_NONE:
			/* 没有 \if 来从中 elif */
			pg_log_error("\\elif: no matching \\if");
			success = false;
			break;
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \else —— \if..\endif 块中的最终备选分支
 *
 * \else 分支中的语句仅在所有先前的             
 * \if 和 \elif 表达式都求值为 false                
 * 且该块本身未被忽略时才执行。
 */
static backslashResult
exec_command_else(PsqlScanState scan_state, ConditionalStack cstack,
				  PQExpBuffer query_buf)
{
	bool		success = true;

	switch (conditional_stack_peek(cstack))
	{
		case IFSTATE_TRUE:

			/*
 * 刚刚完成此 \if 块的活动分支。更新保存的       
 * 状态，以便保留活动分支放入 query_buf 的
 * 任何数据。
 */
			save_query_text_state(scan_state, cstack, query_buf);

			/* 现在跳过 \else 分支 */
			conditional_stack_poke(cstack, IFSTATE_ELSE_FALSE);
			break;
		case IFSTATE_FALSE:

			/*
 * 丢弃刚刚跳过的分支添加的查询文本。
 */
			discard_query_text(scan_state, cstack, query_buf);

			/*
 * 我们尚未找到任何 true 的 \if 或 \elif 表达式，   
 * 因此执行 \else 分支。
 */
			conditional_stack_poke(cstack, IFSTATE_ELSE_TRUE);
			break;
		case IFSTATE_IGNORED:

			/*
 * 丢弃刚刚跳过的分支添加的查询文本。
 */
			discard_query_text(scan_state, cstack, query_buf);

			/*
 * 我们要么之前处理过此 \if 块的活动分支，        
 * 要么整个 \if 块正在被跳过。无论哪种情况，      
 * 都跳过 \else 分支。
 */
			conditional_stack_poke(cstack, IFSTATE_ELSE_FALSE);
			break;
		case IFSTATE_ELSE_TRUE:
		case IFSTATE_ELSE_FALSE:
			pg_log_error("\\else: cannot occur after \\else");
			success = false;
			break;
		case IFSTATE_NONE:
			/* 没有 \if 来从中 else */
			pg_log_error("\\else: no matching \\if");
			success = false;
			break;
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \endif —— 结束 \if...\endif 块
 */
static backslashResult
exec_command_endif(PsqlScanState scan_state, ConditionalStack cstack,
				   PQExpBuffer query_buf)
{
	bool		success = true;

	switch (conditional_stack_peek(cstack))
	{
		case IFSTATE_TRUE:
		case IFSTATE_ELSE_TRUE:
			/* 关闭 \if 块，保留查询文本         */
			success = conditional_stack_pop(cstack);
			Assert(success);
			break;
		case IFSTATE_FALSE:
		case IFSTATE_IGNORED:
		case IFSTATE_ELSE_FALSE:

			/*
 * 丢弃刚刚跳过的分支添加的查询文本。
 */
			discard_query_text(scan_state, cstack, query_buf);

			/* 关闭 \if 块      */
			success = conditional_stack_pop(cstack);
			Assert(success);
			break;
		case IFSTATE_NONE:
			/* 没有 \if 来结束 */
			pg_log_error("\\endif: no matching \\if");
			success = false;
			break;
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \l —— 列出数据库
 */
static backslashResult
exec_command_list(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *pattern;
		bool		show_verbose;

		pattern = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, true);

		show_verbose = strchr(cmd, '+') ? true : false;

		success = listAllDbs(pattern, show_verbose);

		free(pattern);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \lo_* —— 大对象操作    
 */
static backslashResult
exec_command_lo(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;
	bool		success = true;

	if (active_branch)
	{
		char	   *opt1,
				   *opt2;

		opt1 = psql_scan_slash_option(scan_state,
									  OT_NORMAL, NULL, true);
		opt2 = psql_scan_slash_option(scan_state,
									  OT_NORMAL, NULL, true);

		if (strcmp(cmd + 3, "export") == 0)
		{
			if (!opt2)
			{
				pg_log_error("\\%s: missing required argument", cmd);
				success = false;
			}
			else
			{
				expand_tilde(&opt2);
				success = do_lo_export(opt1, opt2);
			}
		}

		else if (strcmp(cmd + 3, "import") == 0)
		{
			if (!opt1)
			{
				pg_log_error("\\%s: missing required argument", cmd);
				success = false;
			}
			else
			{
				expand_tilde(&opt1);
				success = do_lo_import(opt1, opt2);
			}
		}

		else if (strcmp(cmd + 3, "list") == 0)
			success = listLargeObjects(false);
		else if (strcmp(cmd + 3, "list+") == 0)
			success = listLargeObjects(true);

		else if (strcmp(cmd + 3, "unlink") == 0)
		{
			if (!opt1)
			{
				pg_log_error("\\%s: missing required argument", cmd);
				success = false;
			}
			else
				success = do_lo_unlink(opt1);
		}

		else
			status = PSQL_CMD_UNKNOWN;

		free(opt1);
		free(opt2);
	}
	else
		ignore_slash_options(scan_state);

	if (!success)
		status = PSQL_CMD_ERROR;

	return status;
}

/*
 * \o —— 设置查询输出
 */
static backslashResult
exec_command_out(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_FILEPIPE, NULL, true);

		expand_tilde(&fname);
		success = setQFout(fname);
		free(fname);
	}
	else
		ignore_slash_filepipe(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \p —— 打印当前查询缓冲区
 */
static backslashResult
exec_command_print(PsqlScanState scan_state, bool active_branch,
				   PQExpBuffer query_buf, PQExpBuffer previous_buf)
{
	if (active_branch)
	{
		/*
 * 我们想要打印 \g 将会执行的相同内容，但不改变     
 * 查询缓冲区的状态；因此我们不能使用
 * copy_previous_query()。另外，注意缓冲区指针可能为 NULL。
 */
		if (query_buf && query_buf->len > 0)
			puts(query_buf->data);
		else if (previous_buf && previous_buf->len > 0)
			puts(previous_buf->data);
		else if (!pset.quiet)
			puts(_("Query buffer is empty."));
		fflush(stdout);
	}

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \password —— 设置用户密码
 */
static backslashResult
exec_command_password(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *user = psql_scan_slash_option(scan_state,
												  OT_SQLID, NULL, true);
		char	   *pw1 = NULL;
		char	   *pw2 = NULL;
		PQExpBufferData buf;
		PromptInterruptContext prompt_ctx;

		if (user == NULL)
		{
			/* 默认情况下，该命令应用于 CURRENT_USER */
			PGresult   *res;

			res = PSQLexec("SELECT CURRENT_USER");
			if (!res)
				return PSQL_CMD_ERROR;

			user = pg_strdup(PQgetvalue(res, 0, 0));
			PQclear(res);
		}

		/* 设置以允许 SIGINT 取消 simple_prompt_extended() */
		prompt_ctx.jmpbuf = sigint_interrupt_jmp;
		prompt_ctx.enabled = &sigint_interrupt_enabled;
		prompt_ctx.canceled = false;

		initPQExpBuffer(&buf);
		printfPQExpBuffer(&buf, _("Enter new password for user \"%s\": "), user);

		pw1 = simple_prompt_extended(buf.data, false, &prompt_ctx);
		if (!prompt_ctx.canceled)
			pw2 = simple_prompt_extended("Enter it again: ", false, &prompt_ctx);

		if (prompt_ctx.canceled)
		{
			/* 静默失败 */
			success = false;
		}
		else if (strcmp(pw1, pw2) != 0)
		{
			pg_log_error("Passwords didn't match.");
			success = false;
		}
		else
		{
			char	   *encrypted_password;

			encrypted_password = PQencryptPasswordConn(pset.db, pw1, user, NULL);

			if (!encrypted_password)
			{
				pg_log_info("%s", PQerrorMessage(pset.db));
				success = false;
			}
			else
			{
				PGresult   *res;

				printfPQExpBuffer(&buf, "ALTER USER %s PASSWORD ",
								  fmtId(user));
				appendStringLiteralConn(&buf, encrypted_password, pset.db);
				res = PSQLexec(buf.data);
				if (!res)
					success = false;
				else
					PQclear(res);
				PQfreemem(encrypted_password);
			}
		}

		free(user);
		free(pw1);
		free(pw2);
		termPQExpBuffer(&buf);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \prompt —— 提示并设置变量
 */
static backslashResult
exec_command_prompt(PsqlScanState scan_state, bool active_branch,
					const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt,
				   *prompt_text = NULL;
		char	   *arg1,
				   *arg2;

		arg1 = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, false);
		arg2 = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, false);

		if (!arg1)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			success = false;
		}
		else
		{
			char	   *result;
			PromptInterruptContext prompt_ctx;

			/* 设置以允许 SIGINT 取消 simple_prompt_extended() */
			prompt_ctx.jmpbuf = sigint_interrupt_jmp;
			prompt_ctx.enabled = &sigint_interrupt_enabled;
			prompt_ctx.canceled = false;

			if (arg2)
			{
				prompt_text = arg1;
				opt = arg2;
			}
			else
				opt = arg1;

			if (!pset.inputfile)
			{
				result = simple_prompt_extended(prompt_text, true, &prompt_ctx);
			}
			else
			{
				if (prompt_text)
				{
					fputs(prompt_text, stdout);
					fflush(stdout);
				}
				result = gets_fromFile(stdin);
				if (!result)
				{
					pg_log_error("\\%s: could not read value for variable",
								 cmd);
					success = false;
				}
			}

			if (prompt_ctx.canceled ||
				(result && !SetVariable(pset.vars, opt, result)))
				success = false;

			free(result);
			free(prompt_text);
			free(opt);
		}
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \pset —— 设置打印参数 
 */
static backslashResult
exec_command_pset(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt0 = psql_scan_slash_option(scan_state,
												  OT_NORMAL, NULL, false);
		char	   *opt1 = psql_scan_slash_option(scan_state,
												  OT_NORMAL, NULL, false);

		if (!opt0)
		{
			/* 列出所有变量 */

			int			i;
			static const char *const my_list[] = {
				"border", "columns", "csv_fieldsep", "expanded", "fieldsep",
				"fieldsep_zero", "footer", "format", "linestyle", "null",
				"numericlocale", "pager", "pager_min_lines",
				"recordsep", "recordsep_zero",
				"tableattr", "title", "tuples_only",
				"unicode_border_linestyle",
				"unicode_column_linestyle",
				"unicode_header_linestyle",
				"xheader_width",
				NULL
			};

			for (i = 0; my_list[i] != NULL; i++)
			{
				char	   *val = pset_value_string(my_list[i], &pset.popt);

				printf("%-24s %s\n", my_list[i], val);
				free(val);
			}

			success = true;
		}
		else
			success = do_pset(opt0, opt1, &pset.popt, pset.quiet);

		free(opt0);
		free(opt1);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \q 或 \quit —— 退出 psql
 */
static backslashResult
exec_command_quit(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
		status = PSQL_CMD_TERMINATE;

	return status;
}

/*
 * \r —— 重置（清空）查询缓冲区
 */
static backslashResult
exec_command_reset(PsqlScanState scan_state, bool active_branch,
				   PQExpBuffer query_buf)
{
	if (active_branch)
	{
		resetPQExpBuffer(query_buf);
		psql_scan_reset(scan_state);
		if (!pset.quiet)
			puts(_("Query buffer reset (cleared)."));
	}

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \restrict —— 使用提供的密钥进入"受限模式"
 */
static backslashResult
exec_command_restrict(PsqlScanState scan_state, bool active_branch,
					  const char *cmd)
{
	if (active_branch)
	{
		char	   *opt;

		Assert(!restricted);

		opt = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, true);
		if (opt == NULL || opt[0] == '\0')
		{
			pg_log_error("\\%s: missing required argument", cmd);
			return PSQL_CMD_ERROR;
		}

		restrict_key = pstrdup(opt);
		restricted = true;
	}
	else
		ignore_slash_options(scan_state);

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \s —— 将历史保存到文件或在屏幕上显示
 */
static backslashResult
exec_command_s(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, true);

		expand_tilde(&fname);
		success = printHistory(fname, pset.popt.topt.pager);
		if (success && !pset.quiet && fname)
			printf(_("Wrote history to file \"%s\".\n"), fname);
		if (!fname)
			putchar('\n');
		free(fname);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \set —— 设置变量
 */
static backslashResult
exec_command_set(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt0 = psql_scan_slash_option(scan_state,
												  OT_NORMAL, NULL, false);

		if (!opt0)
		{
			/* 列出所有变量 */
			PrintVariables(pset.vars);
			success = true;
		}
		else
		{
			/*
 * 将变量设置为参数的串联。
 */
			char	   *newval;
			char	   *opt;

			opt = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, false);
			newval = pg_strdup(opt ? opt : "");
			free(opt);

			while ((opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, false)))
			{
				newval = pg_realloc(newval, strlen(newval) + strlen(opt) + 1);
				strcat(newval, opt);
				free(opt);
			}

			if (!SetVariable(pset.vars, opt0, newval))
				success = false;

			free(newval);
		}
		free(opt0);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \setenv —— 设置环境变量  
 */
static backslashResult
exec_command_setenv(PsqlScanState scan_state, bool active_branch,
					const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *envvar = psql_scan_slash_option(scan_state,
													OT_NORMAL, NULL, false);
		char	   *envval = psql_scan_slash_option(scan_state,
													OT_NORMAL, NULL, false);

		if (!envvar)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			success = false;
		}
		else if (strchr(envvar, '=') != NULL)
		{
			pg_log_error("\\%s: environment variable name must not contain \"=\"",
						 cmd);
			success = false;
		}
		else if (!envval)
		{
			/* 无参数 —— 取消设置环境变量 */
			unsetenv(envvar);
			success = true;
		}
		else
		{
			/* 将变量设置为下一个参数的值 */
			setenv(envvar, envval, 1);
			success = true;
		}
		free(envvar);
		free(envval);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \sf/\sv —— 显示函数或视图的源代码
 */
static backslashResult
exec_command_sf_sv(PsqlScanState scan_state, bool active_branch,
				   const char *cmd, bool is_func)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		bool		show_linenumbers = (strchr(cmd, '+') != NULL);
		PQExpBuffer buf;
		char	   *obj_desc;
		Oid			obj_oid = InvalidOid;
		EditableObjectType eot = is_func ? EditableFunction : EditableView;

		buf = createPQExpBuffer();
		obj_desc = psql_scan_slash_option(scan_state,
										  OT_WHOLE_LINE, NULL, true);
		if (!obj_desc)
		{
			if (is_func)
				pg_log_error("function name is required");
			else
				pg_log_error("view name is required");
			status = PSQL_CMD_ERROR;
		}
		else if (!lookup_object_oid(eot, obj_desc, &obj_oid))
		{
			/* 错误已报告 */
			status = PSQL_CMD_ERROR;
		}
		else if (!get_create_object_cmd(eot, obj_oid, buf))
		{
			/* 错误已报告 */
			status = PSQL_CMD_ERROR;
		}
		else
		{
			FILE	   *output;
			bool		is_pager;

			/* 选择输出流：stdout、分页器或文件 */
			if (pset.queryFout == stdout)
			{
				/* 计算函数中的行数以查看是否需要分页器 */
				int			lineno = count_lines_in_buf(buf);

				output = PageOutput(lineno, &(pset.popt.topt));
				is_pager = true;
			}
			else
			{
				/* 使用先前设置的输出文件，不使用分页器 */
				output = pset.queryFout;
				is_pager = false;
			}

			if (show_linenumbers)
			{
				/* 添加行号 */
				print_with_linenumbers(output, buf->data, is_func);
			}
			else
			{
				/* 只需将定义发送到输出 */
				fputs(buf->data, output);
			}

			if (is_pager)
				ClosePager(output);
		}

		free(obj_desc);
		destroyPQExpBuffer(buf);
	}
	else
		ignore_slash_whole_line(scan_state);

	return status;
}

/*
 * \t —— 关闭表头和行计数        
 */
static backslashResult
exec_command_t(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);

		success = do_pset("tuples_only", opt, &pset.popt, pset.quiet);
		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \T —— 定义 html <table ...> 属性
 */
static backslashResult
exec_command_T(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *value = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, false);

		success = do_pset("tableattr", value, &pset.popt, pset.quiet);
		free(value);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \timing —— 启用或禁用查询计时 
 */
static backslashResult
exec_command_timing(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, false);

		if (opt)
			success = ParseVariableBool(opt, "\\timing", &pset.timing);
		else
			pset.timing = !pset.timing;
		if (!pset.quiet)
		{
			if (pset.timing)
				puts(_("Timing is on."));
			else
				puts(_("Timing is off."));
		}
		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \unrestrict —— 如果提供的密钥匹配，则退出"受限模式"
 */
static backslashResult
exec_command_unrestrict(PsqlScanState scan_state, bool active_branch,
						const char *cmd)
{
	if (active_branch)
	{
		char	   *opt;

		opt = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, true);
		if (opt == NULL || opt[0] == '\0')
		{
			pg_log_error("\\%s: missing required argument", cmd);
			return PSQL_CMD_ERROR;
		}

		if (!restricted)
		{
			pg_log_error("\\%s: not currently in restricted mode", cmd);
			return PSQL_CMD_ERROR;
		}
		else if (strcmp(opt, restrict_key) == 0)
		{
			pfree(restrict_key);
			restricted = false;
		}
		else
		{
			pg_log_error("\\%s: wrong key", cmd);
			return PSQL_CMD_ERROR;
		}
	}
	else
		ignore_slash_options(scan_state);

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \unset —— 取消设置变量
 */
static backslashResult
exec_command_unset(PsqlScanState scan_state, bool active_branch,
				   const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, false);

		if (!opt)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			success = false;
		}
		else if (!SetVariable(pset.vars, opt, NULL))
			success = false;

		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \w —— 将查询缓冲区写入文件
 */
static backslashResult
exec_command_write(PsqlScanState scan_state, bool active_branch,
				   const char *cmd,
				   PQExpBuffer query_buf, PQExpBuffer previous_buf)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_FILEPIPE, NULL, true);
		FILE	   *fd = NULL;
		bool		is_pipe = false;

		if (!query_buf)
		{
			pg_log_error("no query buffer");
			status = PSQL_CMD_ERROR;
		}
		else
		{
			if (!fname)
			{
				pg_log_error("\\%s: missing required argument", cmd);
				status = PSQL_CMD_ERROR;
			}
			else
			{
				expand_tilde(&fname);
				if (fname[0] == '|')
				{
					is_pipe = true;
					fflush(NULL);
					disable_sigpipe_trap();
					fd = popen(&fname[1], "w");
				}
				else
				{
					canonicalize_path_enc(fname, pset.encoding);
					fd = fopen(fname, "w");
				}
				if (!fd)
				{
					pg_log_error("%s: %m", fname);
					status = PSQL_CMD_ERROR;
				}
			}
		}

		if (fd)
		{
			int			result;

			/*
 * 我们想要打印 \g 将会执行的相同内容，但不改变
 * 查询缓冲区的状态；因此我们不能使用
 * copy_previous_query()。另外，注意缓冲区指针
 * 可能为 NULL。
 */
			if (query_buf && query_buf->len > 0)
				fprintf(fd, "%s\n", query_buf->data);
			else if (previous_buf && previous_buf->len > 0)
				fprintf(fd, "%s\n", previous_buf->data);

			if (is_pipe)
			{
				result = pclose(fd);

				if (result != 0)
				{
					pg_log_error("%s: %s", fname, wait_result_to_str(result));
					status = PSQL_CMD_ERROR;
				}
				SetShellResultVariables(result);
			}
			else
			{
				result = fclose(fd);

				if (result == EOF)
				{
					pg_log_error("%s: %m", fname);
					status = PSQL_CMD_ERROR;
				}
			}
		}

		if (is_pipe)
			restore_sigpipe_trap();

		free(fname);
	}
	else
		ignore_slash_filepipe(scan_state);

	return status;
}

/*
 * \watch —— 每 N 秒执行一次查询。
 * 可选地，在 M 次迭代后停止。
 */
static backslashResult
exec_command_watch(PsqlScanState scan_state, bool active_branch,
				   PQExpBuffer query_buf, PQExpBuffer previous_buf)
{
	bool		success = true;

	if (active_branch)
	{
		bool		have_sleep = false;
		bool		have_iter = false;
		double		sleep = 2;
		int			iter = 0;

		/*
 * 解析参数。我们允许无标签的间隔值或
 * "name=value" 格式，其中 name 来自集合
 *（'i'、'interval'、'c'、'count'）。
 */
		while (success)
		{
			char	   *opt = psql_scan_slash_option(scan_state,
													 OT_NORMAL, NULL, true);
			char	   *valptr;
			char	   *opt_end;

			if (!opt)
				break;			/* 没有更多参数 */

			valptr = strchr(opt, '=');
			if (valptr)
			{
				/* 带标签的参数 */
				valptr++;
				if (strncmp("i=", opt, strlen("i=")) == 0 ||
					strncmp("interval=", opt, strlen("interval=")) == 0)
				{
					if (have_sleep)
					{
						pg_log_error("\\watch: interval value is specified more than once");
						success = false;
					}
					else
					{
						have_sleep = true;
						errno = 0;
						sleep = strtod(valptr, &opt_end);
						if (sleep < 0 || *opt_end || errno == ERANGE)
						{
							pg_log_error("\\watch: incorrect interval value \"%s\"", valptr);
							success = false;
						}
					}
				}
				else if (strncmp("c=", opt, strlen("c=")) == 0 ||
						 strncmp("count=", opt, strlen("count=")) == 0)
				{
					if (have_iter)
					{
						pg_log_error("\\watch: iteration count is specified more than once");
						success = false;
					}
					else
					{
						have_iter = true;
						errno = 0;
						iter = strtoint(valptr, &opt_end, 10);
						if (iter <= 0 || *opt_end || errno == ERANGE)
						{
							pg_log_error("\\watch: incorrect iteration count \"%s\"", valptr);
							success = false;
						}
					}
				}
				else
				{
					pg_log_error("\\watch: unrecognized parameter \"%s\"", opt);
					success = false;
				}
			}
			else
			{
				/* 无标签参数：将其视为间隔 */
				if (have_sleep)
				{
					pg_log_error("\\watch: interval value is specified more than once");
					success = false;
				}
				else
				{
					have_sleep = true;
					errno = 0;
					sleep = strtod(opt, &opt_end);
					if (sleep < 0 || *opt_end || errno == ERANGE)
					{
						pg_log_error("\\watch: incorrect interval value \"%s\"", opt);
						success = false;
					}
				}
			}

			free(opt);
		}

		/* 如果参数解析成功，则执行命令 */
		if (success)
		{
			/* 如果 query_buf 为空，则调用并执行之前的查询 */
			(void) copy_previous_query(query_buf, previous_buf);

			success = do_watch(query_buf, sleep, iter);
		}

		/* 像 \r 一样重置查询缓冲区      */
		resetPQExpBuffer(query_buf);
		psql_scan_reset(scan_state);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \x —— 设置或切换展开表表示         
 */
static backslashResult
exec_command_x(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);

		success = do_pset("expanded", opt, &pset.popt, pset.quiet);
		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \z —— 列出表权限（等同于 \dp）   
 */
static backslashResult
exec_command_z(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *pattern;
		bool		show_system;

		pattern = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, true);

		show_system = strchr(cmd, 'S') ? true : false;

		success = permissionsList(pattern, show_system);

		free(pattern);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \\! —— 执行 shell 命令
 */
static backslashResult
exec_command_shell_escape(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_WHOLE_LINE, NULL, false);

		success = do_shell(opt);
		free(opt);
	}
	else
		ignore_slash_whole_line(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \\? —— 打印反斜杠命令的帮助信息
 */
static backslashResult
exec_command_slash_command_help(PsqlScanState scan_state, bool active_branch)
{
	if (active_branch)
	{
		char	   *opt0 = psql_scan_slash_option(scan_state,
												  OT_NORMAL, NULL, false);

		if (!opt0 || strcmp(opt0, "commands") == 0)
			slashUsage(pset.popt.topt.pager);
		else if (strcmp(opt0, "options") == 0)
			usage(pset.popt.topt.pager);
		else if (strcmp(opt0, "variables") == 0)
			helpVariables(pset.popt.topt.pager);
		else
			slashUsage(pset.popt.topt.pager);

		free(opt0);
	}
	else
		ignore_slash_options(scan_state);

	return PSQL_CMD_SKIP_LINE;
}


/*
 * 读取并解释 \connect 反斜杠命令的参数。         
 *
 * 返回一个 malloc 分配的字符串，如果没有参数或参数为空则返回 NULL。
 */
static char *
read_connect_arg(PsqlScanState scan_state)
{
	char	   *result;
	char		quote;

	/*
 * 理想情况下，我们应该将参数视为 SQL 标识符。但为了与
 * 7.2 及更早版本的 pg_dump 文件向后兼容，我们必须
 * 原样接受未引用的参数（不进行小写转换）。目前，
 * 双引号参数可能会被去除双引号（好像 SQL
 * 标识符一样）。到 7.4 左右，可以期望 pg_dump 文件
 * 对所有混合大小写的 \connect 参数使用双引号，           
 * 届时我们可以去掉 OT_SQLIDHACK。
 */
	result = psql_scan_slash_option(scan_state, OT_SQLIDHACK, &quote, true);

	if (!result)
		return NULL;

	if (quote)
		return result;

	if (*result == '\0' || strcmp(result, "-") == 0)
	{
		free(result);
		return NULL;
	}

	return result;
}

/*
 * 读取一个布尔表达式，将其作为 PQExpBuffer 字符串返回。
 *
 * 注意：多于或少于一个标记的任何内容肯定无法被
 * ParseVariableBool 解析，因此我们不必担心在此处抱怨。
 * 此例程的返回数据结构无论如何都需要重新考虑，
 * 以支持未来的扩展，例如 "\if defined VARNAME"。        
 */
static PQExpBuffer
gather_boolean_expression(PsqlScanState scan_state)
{
	PQExpBuffer exp_buf = createPQExpBuffer();
	int			num_options = 0;
	char	   *value;

	/* 收集条件命令的所有参数到 exp_buf 中 */
	while ((value = psql_scan_slash_option(scan_state,
										   OT_NORMAL, NULL, false)) != NULL)
	{
		/* 在标记之间添加空格 */
		if (num_options > 0)
			appendPQExpBufferChar(exp_buf, ' ');
		appendPQExpBufferStr(exp_buf, value);
		num_options++;
		free(value);
	}

	return exp_buf;
}

/*
 * 读取一个布尔表达式，如果表达式是求值为 true 的
 * 有效布尔表达式则返回 true。
 * 否则返回 false。
 *
 * 注意：条件栈的顶部状态必须为活动状态，否则词法分析器
 * 将无法展开变量和反引号。
 */
static bool
is_true_boolean_expression(PsqlScanState scan_state, const char *name)
{
	PQExpBuffer buf = gather_boolean_expression(scan_state);
	bool		value = false;
	bool		success = ParseVariableBool(buf->data, name, &value);

	destroyPQExpBuffer(buf);
	return success && value;
}

/*
 * 读取一个布尔表达式，但不做任何操作。
 *
 * 注意：条件栈的顶部状态必须为 INACTIVE，否则词法分析器
 * 将展开变量和反引号，这是我们在这里不希望的。
 */
static void
ignore_boolean_expression(PsqlScanState scan_state)
{
	PQExpBuffer buf = gather_boolean_expression(scan_state);

	destroyPQExpBuffer(buf);
}

/*
 * 读取并丢弃"普通"反斜杠命令选项。
 *
 * 这应用于任何消费一个或多个 OT_NORMAL、OT_SQLID 或
 * OT_SQLIDHACK 参数的反斜杠命令的非活动分支处理。
 * 我们不需要担心它到底消费多少个，因为 HandleSlashCmds
 * 中的清理逻辑无论如何都会静默丢弃任何多余的内容。
 */
static void
ignore_slash_options(PsqlScanState scan_state)
{
	char	   *arg;

	while ((arg = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, false)) != NULL)
		free(arg);
}

/*
 * 读取并丢弃 FILEPIPE 反斜杠命令参数。
 *
 * 对于任何接受 OT_FILEPIPE 选项的反斜杠命令，
 * 这*必须*用于非活动分支处理。否则我们可能在
 * 活动和非活动情况下消费不同数量的选项文本。
 */
static void
ignore_slash_filepipe(PsqlScanState scan_state)
{
	char	   *arg = psql_scan_slash_option(scan_state,
											 OT_FILEPIPE, NULL, false);

	free(arg);
}

/*
 * 读取并丢弃整行反斜杠命令参数。
 *
 * 对于任何接受 OT_WHOLE_LINE 选项的反斜杠命令，
 * 这*必须*用于非活动分支处理。否则我们可能在
 * 活动和非活动情况下消费不同数量的选项文本。
 */
static void
ignore_slash_whole_line(PsqlScanState scan_state)
{
	char	   *arg = psql_scan_slash_option(scan_state,
											 OT_WHOLE_LINE, NULL, false);

	free(arg);
}

/*
 * 如果给定命令是分支命令，则返回 true。
 */
static bool
is_branching_command(const char *cmd)
{
	return (strcmp(cmd, "if") == 0 ||
			strcmp(cmd, "elif") == 0 ||
			strcmp(cmd, "else") == 0 ||
			strcmp(cmd, "endif") == 0);
}

/*
 * 准备以在可能的情况下将查询缓冲区恢复到当前状态
 * （参见 discard_query_text）。
 *
 * 我们需要记住查询缓冲区的长度，以及词法分析器
 * 对括号嵌套深度的记录。
 */
static void
save_query_text_state(PsqlScanState scan_state, ConditionalStack cstack,
					  PQExpBuffer query_buf)
{
	if (query_buf)
		conditional_stack_set_query_len(cstack, query_buf->len);
	conditional_stack_set_paren_depth(cstack,
									  psql_scan_get_paren_depth(scan_state));
}

/*
 * 丢弃在非活动条件分支期间吸收的任何查询文本。
 *
 * 我们必须丢弃在非活动 \if 分支期间追加到 query_buf 的数据。
 * 如果没有 query_buf，则无需在此做任何事。
 *
 * 另外，将词法分析器状态重置为此前的相同括号深度。
 * （其余状态不需要关注，因为我们不可能在           
 * 注释、字面量或部分标记内部。）
 */
static void
discard_query_text(PsqlScanState scan_state, ConditionalStack cstack,
				   PQExpBuffer query_buf)
{
	if (query_buf)
	{
		int			new_len = conditional_stack_get_query_len(cstack);

		Assert(new_len >= 0 && new_len <= query_buf->len);
		query_buf->len = new_len;
		query_buf->data[new_len] = '\0';
	}
	psql_scan_set_paren_depth(scan_state,
							  conditional_stack_get_paren_depth(cstack));
}

/*
 * 如果 query_buf 为空，则将 previous_buf 复制到其中。
 *
 * 各种反斜杠命令使用此功能，这些命令通常用于
 * 重新执行之前的查询。为方便起见，我们允许
 * query_buf == NULL 的情况（不做任何事）。
 *
 * 如果之前的查询已复制到查询缓冲区，
 * 则返回 "true"，否则返回 "false"。
 */
static bool
copy_previous_query(PQExpBuffer query_buf, PQExpBuffer previous_buf)
{
	if (query_buf && query_buf->len == 0)
	{
		appendPQExpBufferStr(query_buf, previous_buf->data);
		return true;
	}
	return false;
}

/*
 * 向用户询问密码；'username' 是密码所属的
 * 用户名（如果已显式指定）。
 * 返回一个 malloc 分配的字符串。
 * 如果提供了 'canceled'，若提示通过 SIGINT 取消，
 * 则 *canceled 将设置为 true，否则为 false。
 */
static char *
prompt_for_password(const char *username, bool *canceled)
{
	char	   *result;
	PromptInterruptContext prompt_ctx;

	/* 设置以允许 SIGINT 取消 simple_prompt_extended() */
	prompt_ctx.jmpbuf = sigint_interrupt_jmp;
	prompt_ctx.enabled = &sigint_interrupt_enabled;
	prompt_ctx.canceled = false;

	if (username == NULL || username[0] == '\0')
		result = simple_prompt_extended("Password: ", false, &prompt_ctx);
	else
	{
		char	   *prompt_text;

		prompt_text = psprintf(_("Password for user %s: "), username);
		result = simple_prompt_extended(prompt_text, false, &prompt_ctx);
		free(prompt_text);
	}

	if (canceled)
		*canceled = prompt_ctx.canceled;

	return result;
}

static bool
param_is_newly_set(const char *old_val, const char *new_val)
{
	if (new_val == NULL)
		return false;

	if (old_val == NULL || strcmp(old_val, new_val) != 0)
		return true;

	return false;
}

/*
 * do_connect —— \connect 的处理程序
 *
 * 使用给定参数连接到数据库。如果要求我们重用参数，
 * 则使用来自先前连接的参数，其中命令自身的选项
 * 未提供的值由先前连接的参数填充。否则，使用
 * libpq 的默认值。
 *
 * 在交互模式下，如果使用给定参数连接失败，
 * 将保留旧连接。
 */
static bool
do_connect(enum trivalue reuse_previous_specification,
		   char *dbname, char *user, char *host, char *port)
{
	PGconn	   *o_conn = pset.db,
			   *n_conn = NULL;
	PQconninfoOption *cinfo;
	int			nconnopts = 0;
	bool		same_host = false;
	char	   *password = NULL;
	char	   *client_encoding;
	bool		success = true;
	bool		keep_password = true;
	bool		has_connection_string;
	bool		reuse_previous;

	has_connection_string = dbname ?
		recognized_connection_string(dbname) : false;

	/* 如果在连接字符串之后还有额外参数，则报错。 */
	if (has_connection_string && (user || host || port))
	{
		pg_log_error("Do not give user, host, or port separately when using a connection string");
		return false;
	}

	switch (reuse_previous_specification)
	{
		case TRI_YES:
			reuse_previous = true;
			break;
		case TRI_NO:
			reuse_previous = false;
			break;
		default:
			reuse_previous = !has_connection_string;
			break;
	}

	/*
 * 如果我们打算重用连接参数，则从旧连接中收集它们，
 * 然后根据需要替换各个值。（如果连接之前已断开，
 * 我们可能需要借助 pset.dead_conn。）否则，
 * 获取一个包含 libpq 默认值的 PQconninfoOption 数组，
 * 并修改该数组。注意，此函数假定 PQconninfo、
 * PQconndefaults 和 PQconninfoParse 都会生成
 * 包含相同顺序相同选项的数组。
 */
	if (reuse_previous)
	{
		if (o_conn)
			cinfo = PQconninfo(o_conn);
		else if (pset.dead_conn)
			cinfo = PQconninfo(pset.dead_conn);
		else
		{
			/* 这在非交互式 \connect 失败后可到达             */
			pg_log_error("No database connection exists to re-use parameters from");
			return false;
		}
	}
	else
		cinfo = PQconndefaults();

	if (cinfo)
	{
		if (has_connection_string)
		{
			/* 解析连接字符串并将值插入 cinfo */
			PQconninfoOption *replcinfo;
			char	   *errmsg;

			replcinfo = PQconninfoParse(dbname, &errmsg);
			if (replcinfo)
			{
				PQconninfoOption *ci;
				PQconninfoOption *replci;
				bool		have_password = false;

				for (ci = cinfo, replci = replcinfo;
					 ci->keyword && replci->keyword;
					 ci++, replci++)
				{
					Assert(strcmp(ci->keyword, replci->keyword) == 0);
					/* 如果提供了连接字符串的值，则插入之 */
					if (replci->val)
					{
						/*
 * 我们知道这两个 val 字符串都是由 libpq 分配的，
 * 因此避免内存泄漏的最简洁方法是
 * 交换它们。
 */
						char	   *swap = replci->val;

						replci->val = ci->val;
						ci->val = swap;

						/*
 * 检查连接字符串是否提供影响密码重用的选项。
 * 虽然用户、主机、hostaddr 或端口的任何更改
 * 会导致我们忽略旧连接的密码，但我们不对
 * dbname 强制执行此操作，
 * 因为密码不是特定于数据库的。
 */
						if (replci->val == NULL ||
							strcmp(ci->val, replci->val) != 0)
						{
							if (strcmp(replci->keyword, "user") == 0 ||
								strcmp(replci->keyword, "host") == 0 ||
								strcmp(replci->keyword, "hostaddr") == 0 ||
								strcmp(replci->keyword, "port") == 0)
								keep_password = false;
						}
						/* 同时记录连接字符串是否包含密码。 */
						if (strcmp(replci->keyword, "password") == 0)
							have_password = true;
					}
					else if (!reuse_previous)
					{
						/*
 * 当我们有连接字符串且不重用时，
 * 交换*所有*条目，甚至包括连接字符串未设置的那些。
 * 这避免了从 PQconndefaults() 的结果中吸收
 * 依赖于环境的默认值。我们不希望这样做，
 * 因为如果 connstring 指定了 service 参数，
 * 它们会覆盖 service-file 条目，而优先级应该
 * 是相反的。libpq 当然可以重新计算我们未在此处
 * 传递的任何默认值。（在这种情况下，调用
 * PQconndefaults() 有点浪费，但不这样做
 * 将需要又一个主要代码路径。）
						  *
						  *
 */
						replci->val = ci->val;
						ci->val = NULL;
					}
				}
				Assert(ci->keyword == NULL && replci->keyword == NULL);

				/* 在此过程中，确定有多少个选项槽 */
				nconnopts = ci - cinfo;

				PQconninfoFree(replcinfo);

				/*
 * 如果连接字符串包含密码，告诉下面的循环
 * 我们可以使用它，无论其他设置如何（即，
 * cinfo 的密码不再是"旧"密码）。
 */
				if (have_password)
					keep_password = true;

				/* 不要让下面的代码尝试将 dbname 注入参数。 */
				dbname = NULL;
			}
			else
			{
				/* PQconninfoParse 失败 */
				if (errmsg)
				{
					pg_log_error("%s", errmsg);
					PQfreemem(errmsg);
				}
				else
					pg_log_error("out of memory");
				success = false;
			}
		}
		else
		{
			/*
 * 如果 dbname 不是连接字符串，那么我们将其和其他参数
 * 注入到下面的关键字数组中。（我们不能轻松地将它们
 * 插入 cinfo 数组，因为存在内存管理问题：
 * PQconninfoFree 在 Windows 上会表现异常。）
 * 然而，为了避免依赖参数在数组中出现的顺序，
 * 先进行一次初步扫描以正确设置
 * keep_password 和 same_host。
 *
 * 虽然用户、主机或端口的任何更改会导致我们忽略
 * 旧连接的密码，但我们不对 dbname 强制执行此操作，
 * 因为密码不是特定于数据库的。
 */
			PQconninfoOption *ci;

			for (ci = cinfo; ci->keyword; ci++)
			{
				if (user && strcmp(ci->keyword, "user") == 0)
				{
					if (!(ci->val && strcmp(user, ci->val) == 0))
						keep_password = false;
				}
				else if (host && strcmp(ci->keyword, "host") == 0)
				{
					if (ci->val && strcmp(host, ci->val) == 0)
						same_host = true;
					else
						keep_password = false;
				}
				else if (port && strcmp(ci->keyword, "port") == 0)
				{
					if (!(ci->val && strcmp(port, ci->val) == 0))
						keep_password = false;
				}
			}

			/* 在此过程中，确定有多少个选项槽 */
			nconnopts = ci - cinfo;
		}
	}
	else
	{
		/* 创建 cinfo 结构失败 */
		pg_log_error("out of memory");
		success = false;
	}

	/*
 * 如果用户要求提示输入密码，则现在询问。如果没有，
 * 则使用旧连接的密码，前提是用户名等未更改。
 * 否则，先尝试无密码连接，
 * 然后在需要时再询问密码。
 *
 * XXX：此行为会导致在 postmaster 的日志中记录
 * 虚假的连接尝试。但 libpq 没有提供 API 让我们可以
 * 获取密码然后继续第一次连接尝试。
 */
	if (pset.getPassword == TRI_YES && success)
	{
		bool		canceled = false;

		/*
 * 如果提供了连接字符串或 URI，我们不知道将使用
 * 哪个用户名，因为我们尚未从连接字符串中提取
 * 该信息。不要冒险发出误导性的提示。
 * 与 startup.c 中一样，进一步的工作似乎不值得，
 * 因为此 getPassword 设置通常仅用于非交互式情况。
 */
		password = prompt_for_password(has_connection_string ? NULL : user,
									   &canceled);
		success = !canceled;
	}

	/*
 * 考虑是否将 client_encoding 强制设为 "auto"
 *（覆盖连接字符串中的任何设置）。如果我们有终端
 * 连接且没有 PGCLIENTENCODING 环境设置，则这样做。
 */
	if (pset.notty || getenv("PGCLIENTENCODING"))
		client_encoding = NULL;
	else
		client_encoding = "auto";

	/* 循环直到我们连接成功或失败，可能已经发生了 */
	while (success)
	{
		const char **keywords = pg_malloc((nconnopts + 1) * sizeof(*keywords));
		const char **values = pg_malloc((nconnopts + 1) * sizeof(*values));
		int			paramnum = 0;
		PQconninfoOption *ci;

		/*
 * 将非默认设置复制到 PQconnectdbParams 参数数组中；
 * 但注入任何以旧式方式指定的值，以及任何
 * 交互式获取的密码，还有几个我们希望
 * 强制设置的字段。
 *
 * 如果修改此代码，另请参见 main() 中的
 * 初始连接代码。
 */
		for (ci = cinfo; ci->keyword; ci++)
		{
			keywords[paramnum] = ci->keyword;

			if (dbname && strcmp(ci->keyword, "dbname") == 0)
				values[paramnum++] = dbname;
			else if (user && strcmp(ci->keyword, "user") == 0)
				values[paramnum++] = user;
			else if (host && strcmp(ci->keyword, "host") == 0)
				values[paramnum++] = host;
			else if (host && !same_host && strcmp(ci->keyword, "hostaddr") == 0)
			{
				/* 如果我们正在更改 host 值，则丢弃任何旧的 hostaddr */
				values[paramnum++] = NULL;
			}
			else if (port && strcmp(ci->keyword, "port") == 0)
				values[paramnum++] = port;
			/* 如果 !keep_password，则无条件丢弃旧密码 */
			else if ((password || !keep_password) &&
					 strcmp(ci->keyword, "password") == 0)
				values[paramnum++] = password;
			else if (strcmp(ci->keyword, "fallback_application_name") == 0)
				values[paramnum++] = pset.progname;
			else if (client_encoding &&
					 strcmp(ci->keyword, "client_encoding") == 0)
				values[paramnum++] = client_encoding;
			else if (ci->val)
				values[paramnum++] = ci->val;
			/* 否则，不必让 libpq 解析此关键字 */
		}
		/* 添加数组终止符 */
		keywords[paramnum] = NULL;
		values[paramnum] = NULL;

		/* 注意我们不希望 libpq 重新展开 dbname 参数 */
		n_conn = PQconnectdbParams(keywords, values, false);

		pg_free(keywords);
		pg_free(values);

		if (PQstatus(n_conn) == CONNECTION_OK)
			break;

		/*
 * 连接尝试失败；要么使用新密码重试连接尝试，
 * 要么放弃。
 */
		if (!password && PQconnectionNeedsPassword(n_conn) && pset.getPassword != TRI_NO)
		{
			bool		canceled = false;

			/*
 * 使用实际连接的用户名提示输入密码——
 * 它可能来自 "dbname" 而非 "user"。
 */
			password = prompt_for_password(PQuser(n_conn), &canceled);
			PQfinish(n_conn);
			n_conn = NULL;
			success = !canceled;
			continue;
		}

		/*
 * 我们将在下面报告错误…除非 n_conn 为 NULL，
 * 表示 libpq 没有足够的内存来创建 PGconn。
 */
		if (n_conn == NULL)
			pg_log_error("out of memory");

		success = false;
	}							/* 重试循环结束 */

	/* 释放局部分配的数据，无论成功与否 */
	pg_free(password);
	PQconninfoFree(cinfo);

	if (!success)
	{
		/*
 * 连接到数据库失败。在交互模式下，保留与数据库的
 * 先前连接；在脚本模式下，也关闭我们的
 * 先前连接。
 */
		if (pset.cur_cmd_interactive)
		{
			if (n_conn)
			{
				pg_log_info("%s", PQerrorMessage(n_conn));
				PQfinish(n_conn);
			}

			/* pset.db 保持未修改 */
			if (o_conn)
				pg_log_info("Previous connection kept");
		}
		else
		{
			if (n_conn)
			{
				pg_log_error("\\connect: %s", PQerrorMessage(n_conn));
				PQfinish(n_conn);
			}

			if (o_conn)
			{
				/*
 * 过渡到无连接状态。
 *
 * 与 CheckConnection() 不同，我们立即关闭旧连接
 * 以防止其参数被重用。这样做是为了让脚本
 * 不会意外重用其不期望的参数。
 * 否则，状态清理应与 CheckConnection() 中相同。
				  *
 */
				PQfinish(o_conn);
				pset.db = NULL;
				ResetCancelConn();
				UnsyncVariables();
			}

			/* 基于同样的理由，释放任何 dead_conn 以防止重用 */
			if (pset.dead_conn)
			{
				PQfinish(pset.dead_conn);
				pset.dead_conn = NULL;
			}
		}

		return false;
	}

	/*
 * 用新连接替换旧连接，并更新
 * 依赖于连接的变量。保持重新同步逻辑
 * 与 CheckConnection() 同步。
 */
	PQsetNoticeProcessor(n_conn, NoticeProcessor, NULL);
	pset.db = n_conn;
	SyncVariables();
	connection_warnings(false); /* 必须在 SyncVariables 之后 */

	/* 告知用户有关新连接的信息 */
	if (!pset.quiet)
	{
		if (!o_conn ||
			param_is_newly_set(PQhost(o_conn), PQhost(pset.db)) ||
			param_is_newly_set(PQport(o_conn), PQport(pset.db)))
		{
			char	   *connhost = PQhost(pset.db);
			char	   *hostaddr = PQhostaddr(pset.db);

			if (is_unixsock_path(connhost))
			{
				/* hostaddr 会覆盖 connhost */
				if (hostaddr && *hostaddr)
					printf(_("You are now connected to database \"%s\" as user \"%s\" on address \"%s\" at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), hostaddr, PQport(pset.db));
				else
					printf(_("You are now connected to database \"%s\" as user \"%s\" via socket in \"%s\" at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), connhost, PQport(pset.db));
			}
			else
			{
				if (hostaddr && *hostaddr && strcmp(connhost, hostaddr) != 0)
					printf(_("You are now connected to database \"%s\" as user \"%s\" on host \"%s\" (address \"%s\") at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), connhost, hostaddr, PQport(pset.db));
				else
					printf(_("You are now connected to database \"%s\" as user \"%s\" on host \"%s\" at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), connhost, PQport(pset.db));
			}
		}
		else
			printf(_("You are now connected to database \"%s\" as user \"%s\".\n"),
				   PQdb(pset.db), PQuser(pset.db));
	}

	/* 丢弃不再需要的连接 */
	if (o_conn)
		PQfinish(o_conn);
	if (pset.dead_conn)
	{
		PQfinish(pset.dead_conn);
		pset.dead_conn = NULL;
	}

	return true;
}


void
connection_warnings(bool in_startup)
{
	if (!pset.quiet && !pset.notty)
	{
		int			client_ver = PG_VERSION_NUM;
		char		cverbuf[32];
		char		sverbuf[32];

		if (pset.sversion != client_ver)
		{
			const char *server_version;

			/* 尝试获取完整文本形式，可能包含 "devel" 等 */
			server_version = PQparameterStatus(pset.db, "server_version");
			/* 否则回退到 pset.sversion */
			if (!server_version)
			{
				formatPGVersionNumber(pset.sversion, true,
									  sverbuf, sizeof(sverbuf));
				server_version = sverbuf;
			}

			printf(_("%s (%s, server %s)\n"),
				   pset.progname, PG_VERSION, server_version);
		}
		/* 对于版本匹配，仅在启动时打印 psql 横幅。 */
		else if (in_startup)
			printf("%s (%s)\n", pset.progname, PG_VERSION);

		/*
 * 如果服务器的主版本比我们的新，或者服务器
 * 早于我们的支持截止版本（当前为 9.2），则发出警告。
 */
		if (pset.sversion / 100 > client_ver / 100 ||
			pset.sversion < 90200)
			printf(_("WARNING: %s major version %s, server major version %s.\n"
					 "         Some psql features might not work.\n"),
				   pset.progname,
				   formatPGVersionNumber(client_ver, false,
										 cverbuf, sizeof(cverbuf)),
				   formatPGVersionNumber(pset.sversion, false,
										 sverbuf, sizeof(sverbuf)));

#ifdef WIN32
		if (in_startup)
			checkWin32Codepage();
#endif
		printSSLInfo();
		printGSSInfo();
	}
}


/*
 * printSSLInfo
 *
 * 打印有关当前 SSL 连接的信息（如果 SSL 正在使用中）
 */
static void
printSSLInfo(void)
{
	const char *protocol;
	const char *cipher;
	const char *compression;

	if (!PQsslInUse(pset.db))
		return;					/* 无 SSL */

	protocol = PQsslAttribute(pset.db, "protocol");
	cipher = PQsslAttribute(pset.db, "cipher");
	compression = PQsslAttribute(pset.db, "compression");

	printf(_("SSL connection (protocol: %s, cipher: %s, compression: %s)\n"),
		   protocol ? protocol : _("unknown"),
		   cipher ? cipher : _("unknown"),
		   (compression && strcmp(compression, "off") != 0) ? _("on") : _("off"));
}

/*
 * printGSSInfo
 *
 * 打印有关当前 GSSAPI 连接的信息（如果 GSSAPI 加密正在使用中）
 */
static void
printGSSInfo(void)
{
	if (!PQgssEncInUse(pset.db))
		return;					/* 未使用 GSSAPI 加密 */

	printf(_("GSSAPI-encrypted connection\n"));
}


/*
 * checkWin32Codepage
 *
 * 当 win32 控制台代码页与 Windows 代码页不同时打印警告
 */
#ifdef WIN32
static void
checkWin32Codepage(void)
{
	unsigned int wincp,
				concp;

	wincp = GetACP();
	concp = GetConsoleCP();
	if (wincp != concp)
	{
		printf(_("WARNING: Console code page (%u) differs from Windows code page (%u)\n"
				 "         8-bit characters might not work correctly. See psql reference\n"
				 "         page \"Notes for Windows users\" for details.\n"),
			   concp, wincp);
	}
}
#endif


/*
 * SyncVariables
 *
 * 在建立新连接后使 psql 的内部变量
 * 与连接状态一致。
 */
void
SyncVariables(void)
{
	char		vbuf[32];
	const char *server_version;

	/* 从连接获取信息 */
	pset.encoding = PQclientEncoding(pset.db);
	pset.popt.topt.encoding = pset.encoding;
	pset.sversion = PQserverVersion(pset.db);

	setFmtEncoding(pset.encoding);

	SetVariable(pset.vars, "DBNAME", PQdb(pset.db));
	SetVariable(pset.vars, "USER", PQuser(pset.db));
	SetVariable(pset.vars, "HOST", PQhost(pset.db));
	SetVariable(pset.vars, "PORT", PQport(pset.db));
	SetVariable(pset.vars, "ENCODING", pg_encoding_to_char(pset.encoding));

	/* 此部分应与 connection_warnings() 匹配： */
	/* 尝试获取版本的完整文本形式，可能包含 "devel" 等 */
	server_version = PQparameterStatus(pset.db, "server_version");
	/* 否则回退到 pset.sversion */
	if (!server_version)
	{
		formatPGVersionNumber(pset.sversion, true, vbuf, sizeof(vbuf));
		server_version = vbuf;
	}
	SetVariable(pset.vars, "SERVER_VERSION_NAME", server_version);

	snprintf(vbuf, sizeof(vbuf), "%d", pset.sversion);
	SetVariable(pset.vars, "SERVER_VERSION_NUM", vbuf);

	/* 也向它发送信息 */
	PQsetErrorVerbosity(pset.db, pset.verbosity);
	PQsetErrorContextVisibility(pset.db, pset.show_context);
}

/*
 * UnsyncVariables
 *
 * 清除在无连接时不应设置的变量。
 */
void
UnsyncVariables(void)
{
	SetVariable(pset.vars, "DBNAME", NULL);
	SetVariable(pset.vars, "USER", NULL);
	SetVariable(pset.vars, "HOST", NULL);
	SetVariable(pset.vars, "PORT", NULL);
	SetVariable(pset.vars, "ENCODING", NULL);
	SetVariable(pset.vars, "SERVER_VERSION_NAME", NULL);
	SetVariable(pset.vars, "SERVER_VERSION_NUM", NULL);
}


/*
 * do_edit() 的辅助函数：实际调用编辑器
 *
 * 成功时返回 true，如果未能调用编辑器或
 * 它返回非零状态则返回 false。（对于无法调用的情况
 * 会打印错误消息，但编辑器返回非零状态时不会。）
 */
static bool
editFile(const char *fname, int lineno)
{
	const char *editorName;
	const char *editor_lineno_arg = NULL;
	char	   *sys;
	int			result;

	Assert(fname != NULL);

	/* 查找要使用的编辑器 */
	editorName = getenv("PSQL_EDITOR");
	if (!editorName)
		editorName = getenv("EDITOR");
	if (!editorName)
		editorName = getenv("VISUAL");
	if (!editorName)
		editorName = DEFAULT_EDITOR;

	/* 如果需要，获取行号参数。 */
	if (lineno > 0)
	{
		editor_lineno_arg = getenv("PSQL_EDITOR_LINENUMBER_ARG");
#ifdef DEFAULT_EDITOR_LINENUMBER_ARG
		if (!editor_lineno_arg)
			editor_lineno_arg = DEFAULT_EDITOR_LINENUMBER_ARG;
#endif
		if (!editor_lineno_arg)
		{
			pg_log_error("environment variable PSQL_EDITOR_LINENUMBER_ARG must be set to specify a line number");
			return false;
		}
	}

	/*
 * 在 Unix 上，EDITOR 值*不应*加引号，因为它可能包含
 * 开关，例如 EDITOR="pico -t"；如有必要，由用户自行
 * 加引号。但此策略在 Windows 上不太可行，因为其
 * 命令 shell 存在严重缺陷，加上标准
 * 程序路径包含空格。
 */
#ifndef WIN32
	if (lineno > 0)
		sys = psprintf("exec %s %s%d '%s'",
					   editorName, editor_lineno_arg, lineno, fname);
	else
		sys = psprintf("exec %s '%s'",
					   editorName, fname);
#else
	if (lineno > 0)
		sys = psprintf("\"%s\" %s%d \"%s\"",
					   editorName, editor_lineno_arg, lineno, fname);
	else
		sys = psprintf("\"%s\" \"%s\"",
					   editorName, fname);
#endif
	fflush(NULL);
	result = system(sys);
	if (result == -1)
		pg_log_error("could not start editor \"%s\"", editorName);
	else if (result == 127)
		pg_log_error("could not start /bin/sh");
	free(sys);

	return result == 0;
}


/*
 * do_edit —— \e 的处理程序
 *
 * 如果不指定文件名，当前查询缓冲区将被复制
 * 到一个临时文件中。
 *
 * 此函数完成后，结果文件将被复制回
 * 查询缓冲区。例外：如果文件未被修改
 * （或编辑器失败）且调用者传递                               
 * "discard_on_quit" = true，则查询缓冲区将被清空。
 *
 * 如果 "edited" 不为 NULL，当查询缓冲区
 * 成功替换时，*edited 将设置为 true。
 */
static bool
do_edit(const char *filename_arg, PQExpBuffer query_buf,
		int lineno, bool discard_on_quit, bool *edited)
{
	char		fnametmp[MAXPGPATH];
	FILE	   *stream = NULL;
	const char *fname;
	bool		error = false;
	int			fd;
	struct stat before,
				after;

	if (filename_arg)
		fname = filename_arg;
	else
	{
		/* 创建临时文件进行编辑 */
#ifndef WIN32
		const char *tmpdir = getenv("TMPDIR");

		if (!tmpdir)
			tmpdir = "/tmp";
#else
		char		tmpdir[MAXPGPATH];
		int			ret;

		ret = GetTempPath(MAXPGPATH, tmpdir);
		if (ret == 0 || ret > MAXPGPATH)
		{
			pg_log_error("could not locate temporary directory: %s",
						 !ret ? strerror(errno) : "");
			return false;
		}
#endif

		/*
 * 此处不使用 canonicalize_path()。从 CMD.EXE 运行的
 * EDIT.EXE 会将当前目录添加到提供的路径前面，
 * 除非我们只使用反斜杠，因此我们就这样做。
 */
#ifndef WIN32
		snprintf(fnametmp, sizeof(fnametmp), "%s%spsql.edit.%d.sql", tmpdir,
				 "/", (int) getpid());
#else
		snprintf(fnametmp, sizeof(fnametmp), "%s%spsql.edit.%d.sql", tmpdir,
				 "" /* 尾随分隔符已存在 */ , (int) getpid());
#endif

		fname = (const char *) fnametmp;

		fd = open(fname, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd != -1)
			stream = fdopen(fd, "w");

		if (fd == -1 || !stream)
		{
			pg_log_error("could not open temporary file \"%s\": %m", fname);
			error = true;
		}
		else
		{
			unsigned int ql = query_buf->len;

			/* 强制将发送给编辑器的内容以换行终止 */
			if (ql > 0 && query_buf->data[ql - 1] != '\n')
			{
				appendPQExpBufferChar(query_buf, '\n');
				ql++;
			}

			if (fwrite(query_buf->data, 1, ql, stream) != ql)
			{
				pg_log_error("%s: %m", fname);

				if (fclose(stream) != 0)
					pg_log_error("%s: %m", fname);

				if (remove(fname) != 0)
					pg_log_error("%s: %m", fname);

				error = true;
			}
			else if (fclose(stream) != 0)
			{
				pg_log_error("%s: %m", fname);
				if (remove(fname) != 0)
					pg_log_error("%s: %m", fname);
				error = true;
			}
			else
			{
				struct utimbuf ut;

				/*
 * 尝试将临时文件的文件修改时间设置为
 * 几秒前。否则，我们使用 stat(2) 可以可移植
 * 测量的低粒度（一秒，在某些文件系统上甚至更差）
 * 可能导致我们无法识别修改，如果用户输入
 * 非常快的话。
 *
 * 这是一种相当不太可能的竞态条件，因此如果
 * utime(2) 调用失败也不要报错——那样会使
 * 治疗比疾病更糟。
 */
				ut.modtime = ut.actime = time(NULL) - 2;
				(void) utime(fname, &ut);
			}
		}
	}

	if (!error && stat(fname, &before) != 0)
	{
		pg_log_error("%s: %m", fname);
		error = true;
	}

	/* 调用编辑器 */
	if (!error)
		error = !editFile(fname, lineno);

	if (!error && stat(fname, &after) != 0)
	{
		pg_log_error("%s: %m", fname);
		error = true;
	}

	/* 如果大小或修改时间已更改，则文件已被编辑 */
	if (!error &&
		(before.st_size != after.st_size ||
		 before.st_mtime != after.st_mtime))
	{
		stream = fopen(fname, PG_BINARY_R);
		if (!stream)
		{
			pg_log_error("%s: %m", fname);
			error = true;
		}
		else
		{
			/* 将文件读回 query_buf */
			char		line[1024];

			resetPQExpBuffer(query_buf);
			while (fgets(line, sizeof(line), stream) != NULL)
				appendPQExpBufferStr(query_buf, line);

			if (ferror(stream))
			{
				pg_log_error("%s: %m", fname);
				error = true;
				resetPQExpBuffer(query_buf);
			}
			else if (edited)
			{
				*edited = true;
			}

			fclose(stream);
		}
	}
	else
	{
		/*
 * 如果文件未被修改，且调用者请求了，
 * 则丢弃查询缓冲区。
 */
		if (discard_on_quit)
			resetPQExpBuffer(query_buf);
	}

	/* 删除临时文件 */
	if (!filename_arg)
	{
		if (remove(fname) == -1)
		{
			pg_log_error("%s: %m", fname);
			error = true;
		}
	}

	return !error;
}



/*
 * process_file
 *
 * 从 filename 读取命令并将其传递给主处理循环。
 * \i 和 \ir 的处理程序，但也可用于其他目的。               
 * 返回 MainLoop() 错误码。
 *
 * 如果 use_relative_path 为 true 且 filename 不是
 * 绝对路径，则从当前处理的文件（如果有）所在的位置打开文件。
 */
int
process_file(char *filename, bool use_relative_path)
{
	FILE	   *fd;
	int			result;
	char	   *oldfilename;
	char		relpath[MAXPGPATH];

	if (!filename)
	{
		fd = stdin;
		filename = NULL;
	}
	else if (strcmp(filename, "-") != 0)
	{
		canonicalize_path_enc(filename, pset.encoding);

		/*
 * 如果要求我们相对于当前正在执行的脚本的
 * 位置解析路径名，且存在该脚本，且这是一个
 * 相对路径名，则在当前脚本的路径名前面
 * 加上除最后一个组件外的所有路径名组件。
 */
		if (use_relative_path && pset.inputfile &&
			!is_absolute_path(filename) && !has_drive_prefix(filename))
		{
			strlcpy(relpath, pset.inputfile, sizeof(relpath));
			get_parent_directory(relpath);
			join_path_components(relpath, relpath, filename);
			canonicalize_path_enc(relpath, pset.encoding);

			filename = relpath;
		}

		fd = fopen(filename, PG_BINARY_R);

		if (!fd)
		{
			pg_log_error("%s: %m", filename);
			return EXIT_FAILURE;
		}
	}
	else
	{
		fd = stdin;
		filename = "<stdin>";	/* 用于将来的错误消息 */
	}

	oldfilename = pset.inputfile;
	pset.inputfile = filename;

	pg_logging_config(pset.inputfile ? 0 : PG_LOG_FLAG_TERSE);

	result = MainLoop(fd);

	if (fd != stdin)
		fclose(fd);

	pset.inputfile = oldfilename;

	pg_logging_config(pset.inputfile ? 0 : PG_LOG_FLAG_TERSE);

	return result;
}



static const char *
_align2string(enum printFormat in)
{
	switch (in)
	{
		case PRINT_NOTHING:
			return "nothing";
			break;
		case PRINT_ALIGNED:
			return "aligned";
			break;
		case PRINT_ASCIIDOC:
			return "asciidoc";
			break;
		case PRINT_CSV:
			return "csv";
			break;
		case PRINT_HTML:
			return "html";
			break;
		case PRINT_LATEX:
			return "latex";
			break;
		case PRINT_LATEX_LONGTABLE:
			return "latex-longtable";
			break;
		case PRINT_TROFF_MS:
			return "troff-ms";
			break;
		case PRINT_UNALIGNED:
			return "unaligned";
			break;
		case PRINT_WRAPPED:
			return "wrapped";
			break;
	}
	return "unknown";
}

/*
 * 解析输入的 Unicode 线条样式。如果有效，则更新 *linestyle
 * 并返回 true，否则返回 false。
 */
static bool
set_unicode_line_style(const char *value, size_t vallen,
					   unicode_linestyle *linestyle)
{
	if (pg_strncasecmp("single", value, vallen) == 0)
		*linestyle = UNICODE_LINESTYLE_SINGLE;
	else if (pg_strncasecmp("double", value, vallen) == 0)
		*linestyle = UNICODE_LINESTYLE_DOUBLE;
	else
		return false;
	return true;
}

static const char *
_unicode_linestyle2string(int linestyle)
{
	switch (linestyle)
	{
		case UNICODE_LINESTYLE_SINGLE:
			return "single";
			break;
		case UNICODE_LINESTYLE_DOUBLE:
			return "double";
			break;
	}
	return "unknown";
}

/*
 * do_pset
 *
 * 执行赋值 "param = value"，其中 value 可以为 NULL；
 * 对于某些参数，这会产生翻转等效果，对于其他参数
 * 则不执行任何操作。
 *
 * 调整 *popt 处的格式化选项状态。（在实践中，
 * 它始终是 pset.popt，但或许某一天可能会不同。）
 *
 * 如果成功且 quiet 为 false，则调用 printPsetInfo()
 * 报告更改。
 *
 * 成功时返回 true，失败时返回 false（例如无效的参数或值）。
 */
bool
do_pset(const char *param, const char *value, printQueryOpt *popt, bool quiet)
{
	size_t		vallen = 0;

	Assert(param != NULL);

	if (value)
		vallen = strlen(value);

	/* 设置格式 */
	if (strcmp(param, "format") == 0)
	{
		static const struct fmt
		{
			const char *name;
			enum printFormat number;
		}			formats[] =
		{
			/* 添加更多内容时记得更新下面的错误消息 */
			{"aligned", PRINT_ALIGNED},
			{"asciidoc", PRINT_ASCIIDOC},
			{"csv", PRINT_CSV},
			{"html", PRINT_HTML},
			{"latex", PRINT_LATEX},
			{"troff-ms", PRINT_TROFF_MS},
			{"unaligned", PRINT_UNALIGNED},
			{"wrapped", PRINT_WRAPPED}
		};

		if (!value)
			;
		else
		{
			int			match_pos = -1;

			for (int i = 0; i < lengthof(formats); i++)
			{
				if (pg_strncasecmp(formats[i].name, value, vallen) == 0)
				{
					if (match_pos < 0)
						match_pos = i;
					else
					{
						pg_log_error("\\pset: ambiguous abbreviation \"%s\" matches both \"%s\" and \"%s\"",
									 value,
									 formats[match_pos].name, formats[i].name);
						return false;
					}
				}
			}
			if (match_pos >= 0)
				popt->topt.format = formats[match_pos].number;
			else if (pg_strncasecmp("latex-longtable", value, vallen) == 0)
			{
				/*
 * 我们必须特殊处理 latex-longtable，因为 latex 是
 * 它的前缀；如果两者都在上表中，我们会认为
 * "latex" 是歧义的。
 */
				popt->topt.format = PRINT_LATEX_LONGTABLE;
			}
			else
			{
				pg_log_error("\\pset: allowed formats are aligned, asciidoc, csv, html, latex, latex-longtable, troff-ms, unaligned, wrapped");
				return false;
			}
		}
	}

	/* 设置表线样式 */
	else if (strcmp(param, "linestyle") == 0)
	{
		if (!value)
			;
		else if (pg_strncasecmp("ascii", value, vallen) == 0)
			popt->topt.line_style = &pg_asciiformat;
		else if (pg_strncasecmp("old-ascii", value, vallen) == 0)
			popt->topt.line_style = &pg_asciiformat_old;
		else if (pg_strncasecmp("unicode", value, vallen) == 0)
			popt->topt.line_style = &pg_utf8format;
		else
		{
			pg_log_error("\\pset: allowed line styles are ascii, old-ascii, unicode");
			return false;
		}
	}

	/* 设置 unicode 边框线样式 */
	else if (strcmp(param, "unicode_border_linestyle") == 0)
	{
		if (!value)
			;
		else if (set_unicode_line_style(value, vallen,
										&popt->topt.unicode_border_linestyle))
			refresh_utf8format(&(popt->topt));
		else
		{
			pg_log_error("\\pset: allowed Unicode border line styles are single, double");
			return false;
		}
	}

	/* 设置 unicode 列线样式 */
	else if (strcmp(param, "unicode_column_linestyle") == 0)
	{
		if (!value)
			;
		else if (set_unicode_line_style(value, vallen,
										&popt->topt.unicode_column_linestyle))
			refresh_utf8format(&(popt->topt));
		else
		{
			pg_log_error("\\pset: allowed Unicode column line styles are single, double");
			return false;
		}
	}

	/* 设置 unicode 标题线样式 */
	else if (strcmp(param, "unicode_header_linestyle") == 0)
	{
		if (!value)
			;
		else if (set_unicode_line_style(value, vallen,
										&popt->topt.unicode_header_linestyle))
			refresh_utf8format(&(popt->topt));
		else
		{
			pg_log_error("\\pset: allowed Unicode header line styles are single, double");
			return false;
		}
	}

	/* 设置边框样式/宽度 */
	else if (strcmp(param, "border") == 0)
	{
		if (value)
			popt->topt.border = atoi(value);
	}

	/* 设置展开/垂直模式 */
	else if (strcmp(param, "x") == 0 ||
			 strcmp(param, "expanded") == 0 ||
			 strcmp(param, "vertical") == 0)
	{
		if (value && pg_strcasecmp(value, "auto") == 0)
			popt->topt.expanded = 2;
		else if (value)
		{
			bool		on_off;

			if (ParseVariableBool(value, NULL, &on_off))
				popt->topt.expanded = on_off ? 1 : 0;
			else
			{
				PsqlVarEnumError(param, value, "on, off, auto");
				return false;
			}
		}
		else
			popt->topt.expanded = !popt->topt.expanded;
	}

	/* 展开模式下的标题行宽度 */
	else if (strcmp(param, "xheader_width") == 0)
	{
		if (!value)
			;
		else if (pg_strcasecmp(value, "full") == 0)
			popt->topt.expanded_header_width_type = PRINT_XHEADER_FULL;
		else if (pg_strcasecmp(value, "column") == 0)
			popt->topt.expanded_header_width_type = PRINT_XHEADER_COLUMN;
		else if (pg_strcasecmp(value, "page") == 0)
			popt->topt.expanded_header_width_type = PRINT_XHEADER_PAGE;
		else
		{
			int			intval = atoi(value);

			if (intval == 0)
			{
				pg_log_error("\\pset: allowed xheader_width values are \"%s\" (default), \"%s\", \"%s\", or a number specifying the exact width", "full", "column", "page");
				return false;
			}

			popt->topt.expanded_header_width_type = PRINT_XHEADER_EXACT_WIDTH;
			popt->topt.expanded_header_exact_width = intval;
		}
	}

	/* CSV 格式的字段分隔符 */
	else if (strcmp(param, "csv_fieldsep") == 0)
	{
		if (value)
		{
			/* CSV 分隔符必须是单字节字符 */
			if (strlen(value) != 1)
			{
				pg_log_error("\\pset: csv_fieldsep must be a single one-byte character");
				return false;
			}
			if (value[0] == '"' || value[0] == '\n' || value[0] == '\r')
			{
				pg_log_error("\\pset: csv_fieldsep cannot be a double quote, a newline, or a carriage return");
				return false;
			}
			popt->topt.csvFieldSep[0] = value[0];
		}
	}

	/* 区域敏感的数值输出 */
	else if (strcmp(param, "numericlocale") == 0)
	{
		if (value)
			return ParseVariableBool(value, param, &popt->topt.numericLocale);
		else
			popt->topt.numericLocale = !popt->topt.numericLocale;
	}

	/* null 显示 */
	else if (strcmp(param, "null") == 0)
	{
		if (value)
		{
			free(popt->nullPrint);
			popt->nullPrint = pg_strdup(value);
		}
	}

	/* 不对齐文本的字段分隔符 */
	else if (strcmp(param, "fieldsep") == 0)
	{
		if (value)
		{
			free(popt->topt.fieldSep.separator);
			popt->topt.fieldSep.separator = pg_strdup(value);
			popt->topt.fieldSep.separator_zero = false;
		}
	}

	else if (strcmp(param, "fieldsep_zero") == 0)
	{
		free(popt->topt.fieldSep.separator);
		popt->topt.fieldSep.separator = NULL;
		popt->topt.fieldSep.separator_zero = true;
	}

	/* 不对齐文本的记录分隔符 */
	else if (strcmp(param, "recordsep") == 0)
	{
		if (value)
		{
			free(popt->topt.recordSep.separator);
			popt->topt.recordSep.separator = pg_strdup(value);
			popt->topt.recordSep.separator_zero = false;
		}
	}

	else if (strcmp(param, "recordsep_zero") == 0)
	{
		free(popt->topt.recordSep.separator);
		popt->topt.recordSep.separator = NULL;
		popt->topt.recordSep.separator_zero = true;
	}

	/* 在完整格式和仅元组格式之间切换 */
	else if (strcmp(param, "t") == 0 || strcmp(param, "tuples_only") == 0)
	{
		if (value)
			return ParseVariableBool(value, param, &popt->topt.tuples_only);
		else
			popt->topt.tuples_only = !popt->topt.tuples_only;
	}

	/* 设置标题覆盖 */
	else if (strcmp(param, "C") == 0 || strcmp(param, "title") == 0)
	{
		free(popt->title);
		if (!value)
			popt->title = NULL;
		else
			popt->title = pg_strdup(value);
	}

	/* 设置 HTML 表标签选项 */
	else if (strcmp(param, "T") == 0 || strcmp(param, "tableattr") == 0)
	{
		free(popt->topt.tableAttr);
		if (!value)
			popt->topt.tableAttr = NULL;
		else
			popt->topt.tableAttr = pg_strdup(value);
	}

	/* 切换分页器的使用 */
	else if (strcmp(param, "pager") == 0)
	{
		if (value && pg_strcasecmp(value, "always") == 0)
			popt->topt.pager = 2;
		else if (value)
		{
			bool		on_off;

			if (!ParseVariableBool(value, NULL, &on_off))
			{
				PsqlVarEnumError(param, value, "on, off, always");
				return false;
			}
			popt->topt.pager = on_off ? 1 : 0;
		}
		else if (popt->topt.pager == 1)
			popt->topt.pager = 0;
		else
			popt->topt.pager = 1;
	}

	/* 设置使用分页器的最小行数 */
	else if (strcmp(param, "pager_min_lines") == 0)
	{
		if (value &&
			!ParseVariableNum(value, "pager_min_lines", &popt->topt.pager_min_lines))
			return false;
	}

	/* 禁用 "(x rows)" 页脚 */
	else if (strcmp(param, "footer") == 0)
	{
		if (value)
			return ParseVariableBool(value, param, &popt->topt.default_footer);
		else
			popt->topt.default_footer = !popt->topt.default_footer;
	}

	/* 设置边框样式/宽度 */
	else if (strcmp(param, "columns") == 0)
	{
		if (value)
			popt->topt.columns = atoi(value);
	}
	else
	{
		pg_log_error("\\pset: unknown option: %s", param);
		return false;
	}

	if (!quiet)
		printPsetInfo(param, &pset.popt);

	return true;
}

/*
 * printPsetInfo：打印 popt 中 "param" 格式化参数的状态。
 */
static bool
printPsetInfo(const char *param, printQueryOpt *popt)
{
	Assert(param != NULL);

	/* 显示边框样式/宽度 */
	if (strcmp(param, "border") == 0)
		printf(_("Border style is %d.\n"), popt->topt.border);

	/* 显示 wrapped 格式的目标宽度 */
	else if (strcmp(param, "columns") == 0)
	{
		if (!popt->topt.columns)
			printf(_("Target width is unset.\n"));
		else
			printf(_("Target width is %d.\n"), popt->topt.columns);
	}

	/* 显示展开/垂直模式 */
	else if (strcmp(param, "x") == 0 || strcmp(param, "expanded") == 0 || strcmp(param, "vertical") == 0)
	{
		if (popt->topt.expanded == 1)
			printf(_("Expanded display is on.\n"));
		else if (popt->topt.expanded == 2)
			printf(_("Expanded display is used automatically.\n"));
		else
			printf(_("Expanded display is off.\n"));
	}

	/* 显示 xheader 宽度值 */
	else if (strcmp(param, "xheader_width") == 0)
	{
		if (popt->topt.expanded_header_width_type == PRINT_XHEADER_FULL)
			printf(_("Expanded header width is \"%s\".\n"), "full");
		else if (popt->topt.expanded_header_width_type == PRINT_XHEADER_COLUMN)
			printf(_("Expanded header width is \"%s\".\n"), "column");
		else if (popt->topt.expanded_header_width_type == PRINT_XHEADER_PAGE)
			printf(_("Expanded header width is \"%s\".\n"), "page");
		else if (popt->topt.expanded_header_width_type == PRINT_XHEADER_EXACT_WIDTH)
			printf(_("Expanded header width is %d.\n"), popt->topt.expanded_header_exact_width);
	}

	/* 显示 CSV 格式的字段分隔符 */
	else if (strcmp(param, "csv_fieldsep") == 0)
	{
		printf(_("Field separator for CSV is \"%s\".\n"),
			   popt->topt.csvFieldSep);
	}

	/* 显示不对齐文本的字段分隔符 */
	else if (strcmp(param, "fieldsep") == 0)
	{
		if (popt->topt.fieldSep.separator_zero)
			printf(_("Field separator is zero byte.\n"));
		else
			printf(_("Field separator is \"%s\".\n"),
				   popt->topt.fieldSep.separator);
	}

	else if (strcmp(param, "fieldsep_zero") == 0)
	{
		printf(_("Field separator is zero byte.\n"));
	}

	/* 显示 "(x rows)" 页脚的禁用状态 */
	else if (strcmp(param, "footer") == 0)
	{
		if (popt->topt.default_footer)
			printf(_("Default footer is on.\n"));
		else
			printf(_("Default footer is off.\n"));
	}

	/* 显示格式 */
	else if (strcmp(param, "format") == 0)
	{
		printf(_("Output format is %s.\n"), _align2string(popt->topt.format));
	}

	/* 显示表线样式 */
	else if (strcmp(param, "linestyle") == 0)
	{
		printf(_("Line style is %s.\n"),
			   get_line_style(&popt->topt)->name);
	}

	/* 显示 null 显示方式 */
	else if (strcmp(param, "null") == 0)
	{
		printf(_("Null display is \"%s\".\n"),
			   popt->nullPrint ? popt->nullPrint : "");
	}

	/* 显示区域敏感的数值输出 */
	else if (strcmp(param, "numericlocale") == 0)
	{
		if (popt->topt.numericLocale)
			printf(_("Locale-adjusted numeric output is on.\n"));
		else
			printf(_("Locale-adjusted numeric output is off.\n"));
	}

	/* 显示分页器使用的切换状态 */
	else if (strcmp(param, "pager") == 0)
	{
		if (popt->topt.pager == 1)
			printf(_("Pager is used for long output.\n"));
		else if (popt->topt.pager == 2)
			printf(_("Pager is always used.\n"));
		else
			printf(_("Pager usage is off.\n"));
	}

	/* 显示使用分页器的最小行数 */
	else if (strcmp(param, "pager_min_lines") == 0)
	{
		printf(ngettext("Pager won't be used for less than %d line.\n",
						"Pager won't be used for less than %d lines.\n",
						popt->topt.pager_min_lines),
			   popt->topt.pager_min_lines);
	}

	/* 显示不对齐文本的记录分隔符 */
	else if (strcmp(param, "recordsep") == 0)
	{
		if (popt->topt.recordSep.separator_zero)
			printf(_("Record separator is zero byte.\n"));
		else if (strcmp(popt->topt.recordSep.separator, "\n") == 0)
			printf(_("Record separator is <newline>.\n"));
		else
			printf(_("Record separator is \"%s\".\n"),
				   popt->topt.recordSep.separator);
	}

	else if (strcmp(param, "recordsep_zero") == 0)
	{
		printf(_("Record separator is zero byte.\n"));
	}

	/* 显示 HTML 表标签选项 */
	else if (strcmp(param, "T") == 0 || strcmp(param, "tableattr") == 0)
	{
		if (popt->topt.tableAttr)
			printf(_("Table attributes are \"%s\".\n"),
				   popt->topt.tableAttr);
		else
			printf(_("Table attributes unset.\n"));
	}

	/* 显示标题覆盖 */
	else if (strcmp(param, "C") == 0 || strcmp(param, "title") == 0)
	{
		if (popt->title)
			printf(_("Title is \"%s\".\n"), popt->title);
		else
			printf(_("Title is unset.\n"));
	}

	/* 显示完整格式和仅元组格式之间的切换状态 */
	else if (strcmp(param, "t") == 0 || strcmp(param, "tuples_only") == 0)
	{
		if (popt->topt.tuples_only)
			printf(_("Tuples only is on.\n"));
		else
			printf(_("Tuples only is off.\n"));
	}

	/* Unicode 样式格式化 */
	else if (strcmp(param, "unicode_border_linestyle") == 0)
	{
		printf(_("Unicode border line style is \"%s\".\n"),
			   _unicode_linestyle2string(popt->topt.unicode_border_linestyle));
	}

	else if (strcmp(param, "unicode_column_linestyle") == 0)
	{
		printf(_("Unicode column line style is \"%s\".\n"),
			   _unicode_linestyle2string(popt->topt.unicode_column_linestyle));
	}

	else if (strcmp(param, "unicode_header_linestyle") == 0)
	{
		printf(_("Unicode header line style is \"%s\".\n"),
			   _unicode_linestyle2string(popt->topt.unicode_header_linestyle));
	}

	else
	{
		pg_log_error("\\pset: unknown option: %s", param);
		return false;
	}

	return true;
}

/*
 * savePsetInfo：创建 *popt 中数据的 malloc 副本。
 *
 * 也许这应该放在别处，但它有点特定于 psql。
 */
printQueryOpt *
savePsetInfo(const printQueryOpt *popt)
{
	printQueryOpt *save;

	save = (printQueryOpt *) pg_malloc(sizeof(printQueryOpt));

	/* 平面复制所有标量字段，然后复制子结构。 */
	memcpy(save, popt, sizeof(printQueryOpt));

	/* topt.line_style 指向不需要复制的 const 数据 */
	if (popt->topt.fieldSep.separator)
		save->topt.fieldSep.separator = pg_strdup(popt->topt.fieldSep.separator);
	if (popt->topt.recordSep.separator)
		save->topt.recordSep.separator = pg_strdup(popt->topt.recordSep.separator);
	if (popt->topt.tableAttr)
		save->topt.tableAttr = pg_strdup(popt->topt.tableAttr);
	if (popt->nullPrint)
		save->nullPrint = pg_strdup(popt->nullPrint);
	if (popt->title)
		save->title = pg_strdup(popt->title);

	/*
 * footers 和 translate_columns 在 psql 的打印设置中
 * 从未设置，因此我们无需编写复制它们的代码。
 */
	Assert(popt->footers == NULL);
	Assert(popt->translate_columns == NULL);

	return save;
}

/*
 * restorePsetInfo：从先前保存的副本 *save 恢复 *popt，
 * 然后释放 *save。
 */
void
restorePsetInfo(printQueryOpt *popt, printQueryOpt *save)
{
	/* 释放我们即将覆盖其指针的所有旧数据。 */

	/* topt.line_style 指向不需要复制的 const 数据 */
	free(popt->topt.fieldSep.separator);
	free(popt->topt.recordSep.separator);
	free(popt->topt.tableAttr);
	free(popt->nullPrint);
	free(popt->title);

	/*
 * footers 和 translate_columns 在 psql 的打印设置中
 * 从未设置，因此我们无需编写复制它们的代码。
 */
	Assert(popt->footers == NULL);
	Assert(popt->translate_columns == NULL);

	/* 现在我们可以平面复制所有字段，包括指针。 */
	memcpy(popt, save, sizeof(printQueryOpt));

	/* 最后，释放 "save"…但其子结构现在属于 popt。 */
	free(save);
}

static const char *
pset_bool_string(bool val)
{
	return val ? "on" : "off";
}


static char *
pset_quoted_string(const char *str)
{
	char	   *ret = pg_malloc(strlen(str) * 2 + 3);
	char	   *r = ret;

	*r++ = '\'';

	for (; *str; str++)
	{
		if (*str == '\n')
		{
			*r++ = '\\';
			*r++ = 'n';
		}
		else if (*str == '\'')
		{
			*r++ = '\\';
			*r++ = '\'';
		}
		else
			*r++ = *str;
	}

	*r++ = '\'';
	*r = '\0';

	return ret;
}


/*
 * 为 \pset 值返回一个 malloc 分配的字符串。
 *
 * 注意，对于某些字符串参数，print.c 区分未设置
 * 和空字符串，但对于其他参数则不区分。此函数应产生
 * 在反馈给 \pset 时产生正确设置的输出。              
 */
static char *
pset_value_string(const char *param, printQueryOpt *popt)
{
	Assert(param != NULL);

	if (strcmp(param, "border") == 0)
		return psprintf("%d", popt->topt.border);
	else if (strcmp(param, "columns") == 0)
		return psprintf("%d", popt->topt.columns);
	else if (strcmp(param, "csv_fieldsep") == 0)
		return pset_quoted_string(popt->topt.csvFieldSep);
	else if (strcmp(param, "expanded") == 0)
		return pstrdup(popt->topt.expanded == 2
					   ? "auto"
					   : pset_bool_string(popt->topt.expanded));
	else if (strcmp(param, "fieldsep") == 0)
		return pset_quoted_string(popt->topt.fieldSep.separator
								  ? popt->topt.fieldSep.separator
								  : "");
	else if (strcmp(param, "fieldsep_zero") == 0)
		return pstrdup(pset_bool_string(popt->topt.fieldSep.separator_zero));
	else if (strcmp(param, "footer") == 0)
		return pstrdup(pset_bool_string(popt->topt.default_footer));
	else if (strcmp(param, "format") == 0)
		return pstrdup(_align2string(popt->topt.format));
	else if (strcmp(param, "linestyle") == 0)
		return pstrdup(get_line_style(&popt->topt)->name);
	else if (strcmp(param, "null") == 0)
		return pset_quoted_string(popt->nullPrint
								  ? popt->nullPrint
								  : "");
	else if (strcmp(param, "numericlocale") == 0)
		return pstrdup(pset_bool_string(popt->topt.numericLocale));
	else if (strcmp(param, "pager") == 0)
		return psprintf("%d", popt->topt.pager);
	else if (strcmp(param, "pager_min_lines") == 0)
		return psprintf("%d", popt->topt.pager_min_lines);
	else if (strcmp(param, "recordsep") == 0)
		return pset_quoted_string(popt->topt.recordSep.separator
								  ? popt->topt.recordSep.separator
								  : "");
	else if (strcmp(param, "recordsep_zero") == 0)
		return pstrdup(pset_bool_string(popt->topt.recordSep.separator_zero));
	else if (strcmp(param, "tableattr") == 0)
		return popt->topt.tableAttr ? pset_quoted_string(popt->topt.tableAttr) : pstrdup("");
	else if (strcmp(param, "title") == 0)
		return popt->title ? pset_quoted_string(popt->title) : pstrdup("");
	else if (strcmp(param, "tuples_only") == 0)
		return pstrdup(pset_bool_string(popt->topt.tuples_only));
	else if (strcmp(param, "unicode_border_linestyle") == 0)
		return pstrdup(_unicode_linestyle2string(popt->topt.unicode_border_linestyle));
	else if (strcmp(param, "unicode_column_linestyle") == 0)
		return pstrdup(_unicode_linestyle2string(popt->topt.unicode_column_linestyle));
	else if (strcmp(param, "unicode_header_linestyle") == 0)
		return pstrdup(_unicode_linestyle2string(popt->topt.unicode_header_linestyle));
	else if (strcmp(param, "xheader_width") == 0)
	{
		if (popt->topt.expanded_header_width_type == PRINT_XHEADER_FULL)
			return pstrdup("full");
		else if (popt->topt.expanded_header_width_type == PRINT_XHEADER_COLUMN)
			return pstrdup("column");
		else if (popt->topt.expanded_header_width_type == PRINT_XHEADER_PAGE)
			return pstrdup("page");
		else
		{
			/* 必须是 PRINT_XHEADER_EXACT_WIDTH */
			char		wbuff[32];

			snprintf(wbuff, sizeof(wbuff), "%d",
					 popt->topt.expanded_header_exact_width);
			return pstrdup(wbuff);
		}
	}
	else
		return pstrdup("ERROR");
}



#ifndef WIN32
#define DEFAULT_SHELL "/bin/sh"
#else
/*
 * CMD.EXE 在不同 Win32 版本中位于不同位置，
 * 因此我们必须依赖路径来找到它。
 */
#define DEFAULT_SHELL "cmd.exe"
#endif

static bool
do_shell(const char *command)
{
	int			result;

	fflush(NULL);
	if (!command)
	{
		char	   *sys;
		const char *shellName;

		shellName = getenv("SHELL");
#ifdef WIN32
		if (shellName == NULL)
			shellName = getenv("COMSPEC");
#endif
		if (shellName == NULL)
			shellName = DEFAULT_SHELL;

		/* 参见 EDITOR 处理注释的解释 */
#ifndef WIN32
		sys = psprintf("exec %s", shellName);
#else
		sys = psprintf("\"%s\"", shellName);
#endif
		result = system(sys);
		free(sys);
	}
	else
		result = system(command);

	SetShellResultVariables(result);

	if (result == 127 || result == -1)
	{
		pg_log_error("\\!: failed");
		return false;
	}
	return true;
}

/*
 * do_watch —— \watch 的处理程序
 *
 * 我们将其从 exec_command 中分离出来，以避免在
 * exec_command 的许多变量上贴上 "volatile" 以安抚更愚蠢的编译器。
 *
 * "sleep" 是每次循环中睡眠的时间量，以秒为单位。
 * 此函数的内部应使用 "sleep_ms" 进行
 * 精确的睡眠时间计算。
 */
static bool
do_watch(PQExpBuffer query_buf, double sleep, int iter)
{
	long		sleep_ms = (long) (sleep * 1000);
	printQueryOpt myopt = pset.popt;
	const char *strftime_fmt;
	const char *user_title;
	char	   *title;
	const char *pagerprog = NULL;
	FILE	   *pagerpipe = NULL;
	int			title_len;
	int			res = 0;
#ifndef WIN32
	sigset_t	sigalrm_sigchld_sigint;
	sigset_t	sigalrm_sigchld;
	sigset_t	sigint;
	struct itimerval interval;
	bool		done = false;
#endif

	if (!query_buf || query_buf->len <= 0)
	{
		pg_log_error("\\watch cannot be used with an empty query");
		return false;
	}

#ifndef WIN32
	sigemptyset(&sigalrm_sigchld_sigint);
	sigaddset(&sigalrm_sigchld_sigint, SIGCHLD);
	sigaddset(&sigalrm_sigchld_sigint, SIGALRM);
	sigaddset(&sigalrm_sigchld_sigint, SIGINT);

	sigemptyset(&sigalrm_sigchld);
	sigaddset(&sigalrm_sigchld, SIGCHLD);
	sigaddset(&sigalrm_sigchld, SIGALRM);

	sigemptyset(&sigint);
	sigaddset(&sigint, SIGINT);

	/*
	 * 在启动定时器和分页器（如果已配置）之前阻塞      
	 * SIGALRM 和 SIGCHLD，以避免竞态条件。sigwait() 将接收它们。
	 */
	sigprocmask(SIG_BLOCK, &sigalrm_sigchld, NULL);

	/*
 * 设置一个定时器来中断 sigwait()，以便我们可以
 * 按照请求的间隔运行查询。
 */
	interval.it_value.tv_sec = sleep_ms / 1000;
	interval.it_value.tv_usec = (sleep_ms % 1000) * 1000;
	interval.it_interval = interval.it_value;
	if (setitimer(ITIMER_REAL, &interval, NULL) < 0)
	{
		pg_log_error("could not set timer: %m");
		done = true;
	}
#endif

	/*
 * 对于 \watch，我们忽略结果的大小，只要我们在与终端
 * 通信且 "\pset pager" 已启用，就始终使用分页器。      
 * 但是，我们只使用 PSQL_WATCH_PAGER 标识的分页器。
 * 我们忽略常规的 PSQL_PAGER 或 PAGER 环境变量，
 * 因为传统的分页器对于显示结果流
 * 可能不太有用。
 */
#ifndef WIN32
	pagerprog = getenv("PSQL_WATCH_PAGER");
	/* 如果变量为空或全为空白，则不使用分页器 */
	if (pagerprog && strspn(pagerprog, " \t\r\n") == strlen(pagerprog))
		pagerprog = NULL;
#endif
	if (pagerprog && myopt.topt.pager &&
		isatty(fileno(stdin)) && isatty(fileno(stdout)))
	{
		fflush(NULL);
		disable_sigpipe_trap();
		pagerpipe = popen(pagerprog, "w");

		if (!pagerpipe)
			/* 静默地继续而不使用分页器 */
			restore_sigpipe_trap();
	}

	/*
	 * 选择时间戳的格式。我们可能最终会将其作为 \pset  
	 * 选项。在此期间，使用变量来存储格式可以抑制     
	 * 过于挑剔的 gcc 关于 %c 对 Y2K 敏感的警告。        
	 */
	strftime_fmt = "%c";

	/*
 * 设置渲染选项，特别是禁用分页器，除非
 * PSQL_WATCH_PAGER 已成功启动。
 */
	if (!pagerpipe)
		myopt.topt.pager = 0;


	/*
 * 如果用户配置中有标题，请确保标题缓冲区
 * 中有足够的空间。为时间戳预留 128 字节，
 * 为其余内容预留 128 字节。
 */
	user_title = myopt.title;
	title_len = (user_title ? strlen(user_title) : 0) + 256;
	title = pg_malloc(title_len);

	for (;;)
	{
		time_t		timer;
		char		timebuf[128];

		/*
 * 准备输出的标题。请注意，我们有意在标题末尾
 * 包含一个换行符；这有些历史原因，但在简单情况下
 * 会产生格式相当好的输出。
 */
		timer = time(NULL);
		strftime(timebuf, sizeof(timebuf), strftime_fmt, localtime(&timer));

		if (user_title)
			snprintf(title, title_len, _("%s\t%s (every %gs)\n"),
					 user_title, timebuf, sleep_ms / 1000.0);
		else
			snprintf(title, title_len, _("%s (every %gs)\n"),
					 timebuf, sleep_ms / 1000.0);
		myopt.title = title;

		/* 运行查询并打印结果 */
		res = PSQLexecWatch(query_buf->data, &myopt, pagerpipe);

		/*
 * PSQLexecWatch 处理我们无法再重复查询的情况，
 * 并返回 0 或 -1。
 */
		if (res <= 0)
			break;

		/* 如果我们有迭代计数，检查是否尚未超出 */
		if (iter && (--iter <= 0))
			break;

		if (pagerpipe && ferror(pagerpipe))
			break;

		/* 紧密循环，无需等待 */
		if (sleep_ms == 0)
			continue;

#ifdef WIN32

		/*
 * 通过 SIGINT 设置 \watch 的取消。每次循环都重做       
 * 此操作，因为可以想象 PSQLexecWatch 内部
 * 可能会更改 sigint_interrupt_jmp。
 */
		if (sigsetjmp(sigint_interrupt_jmp, 1) != 0)
			break;

		/*
 * 启用 \watch 取消并等待一段时间，然后再次运行    
 * 查询。将睡眠分成短间隔（最多 1 秒）。
 */
		sigint_interrupt_enabled = true;
		for (long i = sleep_ms; i > 0;)
		{
			long		s = Min(i, 1000L);

			pg_usleep(s * 1000L);
			if (cancel_pressed)
				break;
			i -= s;
		}
		sigint_interrupt_enabled = false;
#else
		/* sigwait() 将处理 SIGINT。 */
		sigprocmask(SIG_BLOCK, &sigint, NULL);
		if (cancel_pressed)
			done = true;

		/* 等待 SIGINT、SIGCHLD 或 SIGALRM。 */
		while (!done)
		{
			int			signal_received;

			errno = sigwait(&sigalrm_sigchld_sigint, &signal_received);
			if (errno != 0)
			{
				/* 有其它信号到达？ */
				if (errno == EINTR)
					continue;
				else
				{
					pg_log_error("could not wait for signals: %m");
					done = true;
					break;
				}
			}
			/* 在 ^C 或分页器退出时，是时候停止运行查询了。 */
			if (signal_received == SIGINT || signal_received == SIGCHLD)
				done = true;
			/* 否则，一定是 SIGALRM。是时候再次运行查询了。 */
			break;
		}

		/* 取消阻塞 SIGINT，以便可以中断慢速查询。 */
		sigprocmask(SIG_UNBLOCK, &sigint, NULL);
		if (done)
			break;
#endif
	}

	if (pagerpipe)
	{
		pclose(pagerpipe);
		restore_sigpipe_trap();
	}
	else
	{
		/*
 * 如果终端驱动程序回显了 "^C"，libedit/libreadline
 * 可能对光标位置感到困惑。因此，在显示下一个提示符
 * 之前注入一个换行符。我们仅在不使用分页器时才这样做，
 * 因为分页器预期在退出时将屏幕恢复到
 * 正常状态。
 */
		fprintf(stdout, "\n");
		fflush(stdout);
	}

#ifndef WIN32
	/* 禁用间隔定时器。 */
	memset(&interval, 0, sizeof(interval));
	setitimer(ITIMER_REAL, &interval, NULL);
	/* 取消阻塞 SIGINT、SIGCHLD 和 SIGALRM。 */
	sigprocmask(SIG_UNBLOCK, &sigalrm_sigchld_sigint, NULL);
#endif

	pg_free(title);
	return (res >= 0);
}

/*
 * 从 PSQLexec() 借用的一小段代码来管理 ECHO_HIDDEN 输出。
 * 除非我们有 ECHO_HIDDEN_NOEXEC，否则返回 true。
 */
static bool
echo_hidden_command(const char *query)
{
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
			return false;
	}
	return true;
}

/*
 * 查找由 obj_type 和 desc 标识的对象。如果成功，
 * 将其 OID 存储在 *obj_oid 中并返回 true，否则返回 false。
 *
 * 注意，如果对象不存在，或者存在多个匹配的候选对象，
 * 或者对象描述存在语法错误，我们都会失败；
 * 遗憾的是，很难区分这些情况。
 */
static bool
lookup_object_oid(EditableObjectType obj_type, const char *desc,
				  Oid *obj_oid)
{
	bool		result = true;
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;

	switch (obj_type)
	{
		case EditableFunction:

			/*
 * 我们有一个函数描述，例如 "x" 或 "x(int)"。
 * 发出查询以使用到 regproc 或 regprocedure 的
 * 转换（视情况而定）检索函数的 OID。
 */
			appendPQExpBufferStr(query, "SELECT ");
			appendStringLiteralConn(query, desc, pset.db);
			appendPQExpBuffer(query, "::pg_catalog.%s::pg_catalog.oid",
							  strchr(desc, '(') ? "regprocedure" : "regproc");
			break;

		case EditableView:

			/*
 * 将视图名称（可能带模式限定）转换为 OID。注意：
 * 此代码不检查关系是否实际上是视图。
 * 我们将在 get_create_object_cmd() 中检测到这一点。
 */
			appendPQExpBufferStr(query, "SELECT ");
			appendStringLiteralConn(query, desc, pset.db);
			appendPQExpBufferStr(query, "::pg_catalog.regclass::pg_catalog.oid");
			break;
	}

	if (!echo_hidden_command(query->data))
	{
		destroyPQExpBuffer(query);
		return false;
	}
	res = PQexec(pset.db, query->data);
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1)
		*obj_oid = atooid(PQgetvalue(res, 0, 0));
	else
	{
		minimal_error_message(res);
		result = false;
	}

	PQclear(res);
	destroyPQExpBuffer(query);

	return result;
}

/*
 * 构造描述指定数据库对象的 "CREATE OR REPLACE ..." 命令。
 * 如果成功，结果存储在 buf 中。
 */
static bool
get_create_object_cmd(EditableObjectType obj_type, Oid oid,
					  PQExpBuffer buf)
{
	bool		result = true;
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;

	switch (obj_type)
	{
		case EditableFunction:
			printfPQExpBuffer(query,
							  "SELECT pg_catalog.pg_get_functiondef(%u)",
							  oid);
			break;

		case EditableView:

			/*
 * pg_get_viewdef() 只打印查询，因此我们必须自己
 * 在前面加上 CREATE。我们必须完全限定视图名称，
 * 以确保替换正确的视图。另外，检查关系类型
 * 以确保它是视图。
 *
 * 从 PG 9.4 开始，视图可以有 WITH [LOCAL|CASCADED]
 * CHECK OPTION。这些不是 pg_get_viewdef() 返回的
 * 视图定义的一部分，因此需要单独检索。
 * 物化视图（在 9.3 中引入）可以有
 * 任意的存储参数 reloptions。
 */
			if (pset.sversion >= 90400)
			{
				printfPQExpBuffer(query,
								  "SELECT nspname, relname, relkind, "
								  "pg_catalog.pg_get_viewdef(c.oid, true), "
								  "pg_catalog.array_remove(pg_catalog.array_remove(c.reloptions,'check_option=local'),'check_option=cascaded') AS reloptions, "
								  "CASE WHEN 'check_option=local' = ANY (c.reloptions) THEN 'LOCAL'::text "
								  "WHEN 'check_option=cascaded' = ANY (c.reloptions) THEN 'CASCADED'::text ELSE NULL END AS checkoption "
								  "FROM pg_catalog.pg_class c "
								  "LEFT JOIN pg_catalog.pg_namespace n "
								  "ON c.relnamespace = n.oid WHERE c.oid = %u",
								  oid);
			}
			else
			{
				printfPQExpBuffer(query,
								  "SELECT nspname, relname, relkind, "
								  "pg_catalog.pg_get_viewdef(c.oid, true), "
								  "c.reloptions AS reloptions, "
								  "NULL AS checkoption "
								  "FROM pg_catalog.pg_class c "
								  "LEFT JOIN pg_catalog.pg_namespace n "
								  "ON c.relnamespace = n.oid WHERE c.oid = %u",
								  oid);
			}
			break;
	}

	if (!echo_hidden_command(query->data))
	{
		destroyPQExpBuffer(query);
		return false;
	}
	res = PQexec(pset.db, query->data);
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1)
	{
		resetPQExpBuffer(buf);
		switch (obj_type)
		{
			case EditableFunction:
				appendPQExpBufferStr(buf, PQgetvalue(res, 0, 0));
				break;

			case EditableView:
				{
					char	   *nspname = PQgetvalue(res, 0, 0);
					char	   *relname = PQgetvalue(res, 0, 1);
					char	   *relkind = PQgetvalue(res, 0, 2);
					char	   *viewdef = PQgetvalue(res, 0, 3);
					char	   *reloptions = PQgetvalue(res, 0, 4);
					char	   *checkoption = PQgetvalue(res, 0, 5);

					/*
 * 如果后端将来支持 CREATE OR REPLACE
 * MATERIALIZED VIEW，则在此处允许；但截至目前，
 * 它还不支持，因此以这种方式编辑物化视图定义
 * 是不可能的。
 */
					switch (relkind[0])
					{
#ifdef NOT_USED
						case RELKIND_MATVIEW:
							appendPQExpBufferStr(buf, "CREATE OR REPLACE MATERIALIZED VIEW ");
							break;
#endif
						case RELKIND_VIEW:
							appendPQExpBufferStr(buf, "CREATE OR REPLACE VIEW ");
							break;
						default:
							pg_log_error("\"%s.%s\" is not a view",
										 nspname, relname);
							result = false;
							break;
					}
					appendPQExpBuffer(buf, "%s.", fmtId(nspname));
					appendPQExpBufferStr(buf, fmtId(relname));

					/* reloptions，如果不是空数组 "{}" */
					if (reloptions != NULL && strlen(reloptions) > 2)
					{
						appendPQExpBufferStr(buf, "\n WITH (");
						if (!appendReloptionsArray(buf, reloptions, "",
												   pset.encoding,
												   standard_strings()))
						{
							pg_log_error("could not parse reloptions array");
							result = false;
						}
						appendPQExpBufferChar(buf, ')');
					}

					/* 来自 pg_get_viewdef 的视图定义（一个 SELECT 查询） */
					appendPQExpBuffer(buf, " AS\n%s", viewdef);

					/* 去掉 pg_get_viewdef 追加的分号 */
					if (buf->len > 0 && buf->data[buf->len - 1] == ';')
						buf->data[--(buf->len)] = '\0';

					/* WITH [LOCAL|CASCADED] CHECK OPTION */
					if (checkoption && checkoption[0] != '\0')
						appendPQExpBuffer(buf, "\n WITH %s CHECK OPTION",
										  checkoption);
				}
				break;
		}
		/* 确保结果以换行符结束 */
		if (buf->len > 0 && buf->data[buf->len - 1] != '\n')
			appendPQExpBufferChar(buf, '\n');
	}
	else
	{
		minimal_error_message(res);
		result = false;
	}

	PQclear(res);
	destroyPQExpBuffer(query);

	return result;
}

/*
 * 如果 \ef 或 \ev 的给定参数以行号结尾，则从参数            
 * 字符串中删除行号并将其作为整数返回。（我们需要
 * 这种 hack，因为我们太懒于仔细解析 \ef 的函数或 \ev 的视图
 * 参数——我们只是用 OT_WHOLE_LINE 模式吞掉它。）
 *
 * 如果没有行号则返回 -1，出错时返回 0，或成功时
 * 返回正值。
 */
static int
strip_lineno_from_objdesc(char *obj)
{
	char	   *c;
	int			lineno;

	if (!obj || obj[0] == '\0')
		return -1;

	c = obj + strlen(obj) - 1;

	/*
	 * 在多字节环境中向后解析是非常危险的：        
	 * 我们没有理由相信我们正在查看字符的第一个字节，
	 * 也不一定在"安全"编码中工作。幸运的是，我们正在
	 * 查找的位模式不太可能作为非首字节出现，但要小心
	 * 尝试扩展可识别的情况集。我们也必须                
	 * 先使用 isascii() 来保护 <ctype.h> 宏。                    
	 *                                                          
	 */

	/* 跳过尾随空白 */
	while (c > obj && isascii((unsigned char) *c) && isspace((unsigned char) *c))
		c--;

	/* 最后一个非空格字符必须是数字 */
	if (c == obj || !isascii((unsigned char) *c) || !isdigit((unsigned char) *c))
		return -1;

	/* 查找数字字符串的开头 */
	while (c > obj && isascii((unsigned char) *c) && isdigit((unsigned char) *c))
		c--;

	/* 数字必须通过空格或闭括号与对象名称分开 */
	/* 还请注意，我们不允许空的对象名称… */
	if (c == obj || !isascii((unsigned char) *c) ||
		!(isspace((unsigned char) *c) || *c == ')'))
		return -1;

	/* 解析数字字符串 */
	c++;
	lineno = atoi(c);
	if (lineno < 1)
	{
		pg_log_error("invalid line number: %s", c);
		return 0;
	}

	/* 从对象名称中去除数字字符串 */
	*c = '\0';

	return lineno;
}

/*
 * 计算缓冲区中的行数。
 * 用于测试是否需要分页器。
 */
static int
count_lines_in_buf(PQExpBuffer buf)
{
	int			lineno = 0;
	const char *lines = buf->data;

	while (*lines != '\0')
	{
		lineno++;
		/* 查找下一行的开头 */
		lines = strchr(lines, '\n');
		if (!lines)
			break;
		lines++;
	}

	return lineno;
}

/*
 * 将 *lines 中的文本以带行号的方式写入输出。
 *
 * 对于函数，行号 "1" 应对应于函数体的第一行；
 * 之前的行不编号。我们期望 pg_get_functiondef()
 * 将其输出在以 "AS "、"BEGIN " 或 "RETURN " 开头
 * 的行上，且在函数体的真正开始之前不会有
 * 这样的行。
 *
 * 注意：这会涂写 *lines。
 */
static void
print_with_linenumbers(FILE *output, char *lines, bool is_func)
{
	bool		in_header = is_func;
	int			lineno = 0;

	while (*lines != '\0')
	{
		char	   *eol;

		if (in_header &&
			(strncmp(lines, "AS ", 3) == 0 ||
			 strncmp(lines, "BEGIN ", 6) == 0 ||
			 strncmp(lines, "RETURN ", 7) == 0))
			in_header = false;

		/* 仅为函数体的行递增 lineno */
		if (!in_header)
			lineno++;

		/* 查找并标记当前行的末尾 */
		eol = strchr(lines, '\n');
		if (eol != NULL)
			*eol = '\0';

		/* 适当地显示当前行 */
		if (in_header)
			fprintf(output, "        %s\n", lines);
		else
			fprintf(output, "%-7d %s\n", lineno, lines);

		/* 前进到下一行（如果有） */
		if (eol == NULL)
			break;
		lines = ++eol;
	}
}

/*
 * 仅报告主要错误；这是为了避免输出杂乱，
 * 例如重新显示内部生成的查询
 */
static void
minimal_error_message(PGresult *res)
{
	PQExpBuffer msg;
	const char *fld;

	msg = createPQExpBuffer();

	fld = PQresultErrorField(res, PG_DIAG_SEVERITY);
	if (fld)
		printfPQExpBuffer(msg, "%s:  ", fld);
	else
		printfPQExpBuffer(msg, "ERROR:  ");
	fld = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	if (fld)
		appendPQExpBufferStr(msg, fld);
	else
		appendPQExpBufferStr(msg, "(not available)");
	appendPQExpBufferChar(msg, '\n');

	pg_log_error("%s", msg->data);

	destroyPQExpBuffer(msg);
}
