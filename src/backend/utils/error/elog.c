/*-------------------------------------------------------------------------
 *
 * elog.c
 *	  错误日志记录与报告
 *
 * 日志消息产生速率极高，
 * 因此获取任何可能被记录的信息都须注意性能成本，
 * 另外须谨记，此代码可能在已中止的事务中被调用，
 * 此时，像 syscache 查找这类操作
 * 是不安全的。
 *
 * 错误处理期间递归与错误的若干说明：
 *
 * 我们需要对递归错误场景保持健壮 --- 例如，
 * 如果内存耗尽，我们必须能够报告这一事实。
 * 这其中有诸多考量因素。
 *
 * 首先，区分重入使用与真正的递归。
 * 在计算某条错误消息的参数期间，也可能发出
 * 错误或警告消息。在这种情况下，
 * 外层消息的 errstart 已被调用，且某些字段值
 * 可能已被保存，但我们并非真正在递归。我们的
 * 处理办法是提供一个（小的）ErrorData 记录栈。内层
 * 消息可以在不干扰外层消息状态的情况下计算并发送。
 * （若内层消息实际是错误，这一点无关紧要，
 * 因为控制权不会返回外层消息的生成器 ... 但若
 * 内层消息只是调试或日志数据，这就是关键了。）
 *
 * 其次，如果错误由 elog.c 中的例程或其调用的代码
 * 报告，则会发生真正的递归。迄今最常见的此类场景
 * 是"内存耗尽"；这也是最难处理的情况，
 * 因为尝试报告该错误时很可能同样内存耗尽！
 * 对此情形，逃生通道是在尝试处理内层错误前
 * 将 ErrorContext 重置为空。由于
 * ErrorContext 保证至少有 8K 空间（见 mcxt.c），
 * 我们应当能成功处理"内存耗尽"消息。
 * 由于重置会丢失先前的错误状态，我们将无法
 * 回到对原始错误的处理，反正本来也回不去了。
 * （注意：对于内层消息严重级别低于 ERROR 的递归
 * 情况，该逃生通道不被使用；此时我们只管尝试
 * 处理并正常返回。这通常会奏效，但如果
 * 最终陷入无限递归，我们将因错误栈溢出而
 * PANIC）
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/error/elog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#ifdef HAVE_SYSLOG
#include <syslog.h>
#endif
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#include "access/transam.h"
#include "access/xact.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "nodes/miscnodes.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/varlena.h"


/* 本模块中经 err_gettext() 访问 gettext() */
#undef _
#define _(x) err_gettext(x)


/* 全局变量 */
ErrorContextCallback *error_context_stack = NULL;

sigjmp_buf *PG_exception_stack = NULL;

extern bool redirection_done;

/*
 * 在消息被发送到服务器日志之前拦截消息的钩子。
 * 注意：被 log_min_messages 抑制的消息不会调用
 * 该钩子。另注意，预加载库中实现的日志钩子将错过
 * 库加载之前生成的任何日志
 * 消息。
 */
emit_log_hook_type emit_log_hook = NULL;

/* GUC 参数 */
int			Log_error_verbosity = PGERROR_DEFAULT;
char	   *Log_line_prefix = NULL; /* 额外日志行信息的格式 */
int			Log_destination = LOG_DESTINATION_STDERR;
char	   *Log_destination_string = NULL;
bool		syslog_sequence_numbers = true;
bool		syslog_split_messages = true;

/* backtrace_symbols GUC 处理形式 */
static char *backtrace_symbol_list;

#ifdef HAVE_SYSLOG

/*
 * 发送给 syslog() 的字符串最大长度。注意这不计入
 * 我们添加的序号前缀，当然也不计入 syslog 自身
 * 添加的前缀。Solaris 和 sysklogd 会把最终消息
 * 截断到 1024 字节，因此该值给这些前缀留出 124 字节。
 * （其他 syslog 实现的上限似乎在 2KB 左右。）
 */
#ifndef PG_SYSLOG_LIMIT
#define PG_SYSLOG_LIMIT 900
#endif

static bool openlog_done = false;
static char *syslog_ident = NULL;
static int	syslog_facility = LOG_LOCAL0;

static void write_syslog(int level, const char *line);
#endif

#ifdef WIN32
extern char *event_source;

static void write_eventlog(int level, const char *line, int len);
#endif

/* 为重入场景提供一个小型 ErrorData 记录栈 */
#define ERRORDATA_STACK_SIZE  5

static ErrorData errordata[ERRORDATA_STACK_SIZE];

static int	errordata_stack_depth = -1; /* 最顶活动帧的索引 */

static int	recursion_depth = 0;	/* 检测真正的递归 */

/*
 * 已格式化时间戳的保存 timeval 与缓冲区，可能被
 * log_line_prefix 与 csvlog 共用
 */
static struct timeval saved_timeval;
static bool saved_timeval_set = false;

#define FORMATTED_TS_LEN 128
static char formatted_start_time[FORMATTED_TS_LEN];
static char formatted_log_time[FORMATTED_TS_LEN];


/* 检查 errordata_stack_depth 是否合理的宏 */
#define CHECK_STACK_DEPTH() \
	do { \
		if (errordata_stack_depth < 0) \
		{ \
			errordata_stack_depth = -1; \
			ereport(ERROR, (errmsg_internal("errstart was not called"))); \
		} \
	} while (0)


static const char *err_gettext(const char *str) pg_attribute_format_arg(1);
static ErrorData *get_error_stack_entry(void);
static void set_stack_entry_domain(ErrorData *edata, const char *domain);
static void set_stack_entry_location(ErrorData *edata,
									 const char *filename, int lineno,
									 const char *funcname);
static bool matches_backtrace_functions(const char *funcname);
static pg_noinline void set_backtrace(ErrorData *edata, int num_skip);
static void set_errdata_field(MemoryContextData *cxt, char **ptr, const char *str);
static void FreeErrorDataContents(ErrorData *edata);
static void write_console(const char *line, int len);
static const char *process_log_prefix_padding(const char *p, int *ppadding);
static void log_line_prefix(StringInfo buf, ErrorData *edata);
static void send_message_to_server_log(ErrorData *edata);
static void send_message_to_frontend(ErrorData *edata);
static void append_with_tabs(StringInfo buf, const char *str);


/*
 * is_log_level_output -- elevel 逻辑上 >= log_min_level？
 *
 * 我们把此函数用于认为 LOG 乱序排序的测试，
 * 即 LOG 排在 ERROR 与 FATAL 之间。通常对测试
 * 消息是否应进入 postmaster 日志，这是正确做法；
 * 而测试消息是否应发往客户端时，简单的 >= 测试
 */
static inline bool
is_log_level_output(int elevel, int log_min_level)
{
	if (elevel == LOG || elevel == LOG_SERVER_ONLY)
	{
		if (log_min_level == LOG || log_min_level <= ERROR)
			return true;
	}
	else if (elevel == WARNING_CLIENT_ONLY)
	{
		/* 不管 log_min_level，均不入日志 */
		return false;
	}
	else if (log_min_level == LOG)
	{
		/* elevel != LOG */
		if (elevel >= FATAL)
			return true;
	}
	/* LOG 亦然 */
	else if (elevel >= log_min_level)
		return true;

	return false;
}

/*
 * 策略设定子例程。都很简单，但把代码集中在一处
 * 似乎是明智之举。
 */

/*
 * should_output_to_server --- 给定 elevel 的消息应进日志吗？
 */
static inline bool
should_output_to_server(int elevel)
{
	return is_log_level_output(elevel, log_min_messages);
}

/*
 * should_output_to_client --- 给定 elevel 的消息应发给客户端吗？
 */
static inline bool
should_output_to_client(int elevel)
{
	if (whereToSendOutput == DestRemote && elevel != LOG_SERVER_ONLY)
	{
		/*
		 * client_min_messages 仅在完成身份认证
		 * 握手之后才被遵守。这既是出于安全原因，
		 * 也是因为许多客户端无法处理认证期间的
		 * NOTICE 消息。
		 */
		if (ClientAuthInProgress)
			return (elevel >= ERROR);
		else
			return (elevel >= client_min_messages || elevel == INFO);
	}
	return false;
}


/*
 * message_level_is_interesting --- ereport/elog 会做事吗？
 *
 * 若此 elevel 的 ereport/elog 非空操作则返回 true。
 * 这对于短路日志消息所需的任何昂贵的准备工作
 * 很有用。但把它前置到裸 ereport/elog 调用之前
 * 则毫无意义。
 */
bool
message_level_is_interesting(int elevel)
{
	/*
	 * 保持与 errstart() 中的决策逻辑同步。
	 */
	if (elevel >= ERROR ||
		should_output_to_server(elevel) ||
		should_output_to_client(elevel))
		return true;
	return false;
}


/*
 * in_error_recursion_trouble --- 是否面临无限错误递归之险？
 *
 * 此函数的存在旨在对各种回退步骤提供共同的控制，
 * 前提是我们认为自己正面临无限错误递归。
 * 详见调用方。
 */
bool
in_error_recursion_trouble(void)
{
	/* 若再递归则立即中止 */
	return (recursion_depth > 2);
}

/*
 * 其中一个回退步骤是停止尝试本地化错误消息，
 * 因为它本身恰恰很可能就是
 * 引发递归的原因。
 */
static inline const char *
err_gettext(const char *str)
{
#ifdef ENABLE_NLS
	if (in_error_recursion_trouble())
		return str;
	else
		return gettext(str);
#else
	return str;
#endif
}

/*
 * errstart_cold
 *		对 errstart 的简单包装，但提示为"冷门"。支持
 *		带此提示的编译器更倾向将含此函数的分支代码
 *		移到远离调用函数代码的区域。这能让更常执行的
 *		代码更加紧凑，并占用
 *		更少缓存行。
 */
pg_attribute_cold bool
errstart_cold(int elevel, const char *domain)
{
	return errstart(elevel, domain);
}

/*
 * errstart --- 开始错误报告周期
 *
 * 创建并初始化错误栈条目。随后将调用 errmsg() 及
 * 可能还有其他例程来进一步填充该栈条目。最后将调用
 * errfinish() 来实际处理该错误报告。
 *
 * 正常情况下返回 true。返回 false 则短路错误报告
 * （若为警告或更低级别且无处可报告）。
 */
bool
errstart(int elevel, const char *domain)
{
	ErrorData  *edata;
	bool		output_to_server;
	bool		output_to_client = false;
	int			i;

	/*
	 * 检查想把错误提升为更严重错误的某些情形。
	 * 这些逻辑均不适用于非错误消息。
	 */
	if (elevel >= ERROR)
	{
		/*
		 * 若正处于临界区内，所有错误都变成 PANIC
		 * 错误，见 miscadmin.h
		 */
		if (CritSectionCount > 0)
			elevel = PANIC;

		/*
		 * 检查将 ERROR 视为 FATAL 原因：
		 *
		 * 1. 我们没有可传错误的处理器（意味着我们处于
		 * postmaster 或后端启动中）
		 *
		 * 2. ExitOnAnyError 开关已设置（initdb 用）。
		 *
		 * 3. 错误发生在 proc_exit 已开始运行之后。（
		 * proc_exit 有责任确保这不会演变成
		 * 无限递归！）
		 */
		if (elevel == ERROR)
		{
			if (PG_exception_stack == NULL ||
				ExitOnAnyError ||
				proc_exit_inprogress)
				elevel = FATAL;
		}

		/*
		 * 如果错误级别为 ERROR 或更高，errfinish 不会
		 * 返回调用者；因此若已有栈中错误正在处理，它
		 * 将会丢失。这基本没问题，只是我们不希望
		 * 因为报告进程被较低级别的错误中断，而导致
		 * FATAL 或 PANIC 错误被降级。所以要检查栈，
		 * 确有必要时确保发生 panic。
		 */
		for (i = 0; i <= errordata_stack_depth; i++)
			elevel = Max(elevel, errordata[i].elevel);
	}

	/*
	 * 现在决定是否真的需要处理此报告；如果它是
	 * 警告或更低级别且未启用日志记录，直接返回 false，
	 * 不启动任何错误日志机制。
	 */
	output_to_server = should_output_to_server(elevel);
	output_to_client = should_output_to_client(elevel);
	if (elevel < ERROR && !output_to_server && !output_to_client)
		return false;

	/*
	 * 需要做些实际工作了。确保内存上下文初始化
	 * 已经完成，否则我们做不了任何有用的事。
	 */
	if (ErrorContext == NULL)
	{
		/* 硬崩溃时刻；这里几乎无法安全行事 */
		write_stderr("error occurred before error message processing is available\n");
		exit(2);
	}

	/*
	 * 好，建一个栈条目来存储这些信息。
	 */

	if (recursion_depth++ > 0 && elevel >= ERROR)
	{
		/*
		 * 糟了，错误处理中又出错。按文件头清空
		 * ErrorContext。我们不会回到原始错误的
		 * 报告者或处理器，所以不再需要它。
		 */
		MemoryContextReset(ErrorContext);

		/*
		 * 无限错误递归可能因上下文回溯例程损坏。
		 * 同样放弃它们。我们也放弃尝试打印错误语句
		 * （如果语句很长，它本身就可能是递归失败的
		 * 根源）。
		 */
		if (in_error_recursion_trouble())
		{
			error_context_stack = NULL;
			debug_query_string = NULL;
		}
	}

	/* 初始化此错误帧的数据 */
	edata = get_error_stack_entry();
	edata->elevel = elevel;
	edata->output_to_server = output_to_server;
	edata->output_to_client = output_to_client;
	set_stack_entry_domain(edata, domain);
	/* 基于 elevel 选择默认 errcode */
	if (elevel >= ERROR)
		edata->sqlerrcode = ERRCODE_INTERNAL_ERROR;
	else if (elevel >= WARNING)
		edata->sqlerrcode = ERRCODE_WARNING;
	else
		edata->sqlerrcode = ERRCODE_SUCCESSFUL_COMPLETION;

	/*
	 * 此错误状态级别下的所有分配都应放入 ErrorContext
	 */
	edata->assoc_context = ErrorContext;

	recursion_depth--;
	return true;
}

/*
 * errfinish --- 结束错误报告周期
 *
 * 产生适当的错误报告并弹出错误栈。
 *
 * 如果传递给 errstart() 的 elevel 为 ERROR 或更严重，
 * 控制权不会返回调用者。错误级别定义见 elog.h。
 */
void
errfinish(const char *filename, int lineno, const char *funcname)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	int			elevel;
	MemoryContext oldcontext;
	ErrorContextCallback *econtext;

	recursion_depth++;
	CHECK_STACK_DEPTH();

	/* 将错误状态的最后几位信息存入栈条目 */
	set_stack_entry_location(edata, filename, lineno, funcname);

	elevel = edata->elevel;

	/*
	 * 在 ErrorContext 中处理，期望其预留空间足以
	 * 报告错误。
	 */
	oldcontext = MemoryContextSwitchTo(ErrorContext);

	/* 若启用且尚未收集，则收集回溯 */
	if (!edata->backtrace &&
		edata->funcname &&
		backtrace_functions &&
		matches_backtrace_functions(edata->funcname))
		set_backtrace(edata, 2);

	/*
	 * 调用所有上下文回调函数。回调函数中发生的错误
	 * 将按递归错误处理 --- 这确保我们能避免无限递归
	 * （见 errstart）。
	 */
	for (econtext = error_context_stack;
		 econtext != NULL;
		 econtext = econtext->previous)
		econtext->callback(econtext->arg);

	/*
	 * 若为 ERROR（不多不少），则交给当前处理器。
	 * 打印它并弹出栈是处理器的职责。
	 */
	if (elevel == ERROR)
	{
		/*
		 * 在 longjmp 之前做些最小的清理，以便处理器
		 * 在较正常的状态下执行。
		 *
		 * 重置 InterruptHoldoffCount，以防我们从
		 * 中断保持区内 ereport。此处假设处理器本身
		 * 不会处于保持区内。必要时，此类处理器可自行
		 * 保存和恢复 InterruptHoldoffCount，但这应该能让
		 * 大多数情形更轻松。）
		 */
		InterruptHoldoffCount = 0;
		QueryCancelHoldoffCount = 0;

		CritSectionCount = 0;	/* 应该不必要，但... */

		/*
		 * 注意，我们将 CurrentMemoryContext 保持设置为
		 * ErrorContext。处理器应尽快重置它。
		 */

		recursion_depth--;
		PG_RE_THROW();
	}

	/* 把消息发送到正确的地方 */
	EmitErrorReport();

	/* 现在释放栈条目附属的数据并释放该条目 */
	FreeErrorDataContents(edata);
	errordata_stack_depth--;

	/* 退出错误处理上下文 */
	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;

	/*
	 * 按 elevel 的指定执行错误恢复动作。
	 */
	if (elevel == FATAL)
	{
		/*
		 * FATAL 错误交给 proc_exit 清理并退出。
		 *
		 * 若刚报告了启动失败，客户端收到后会断开，
		 * 所以不要再向客户端发送任何消息。
		 */
		if (PG_exception_stack == NULL && whereToSendOutput == DestRemote)
			whereToSendOutput = DestNone;

		/*
		 * 这里的 fflush 只是为了提高我们能看到错误消息
		 * 的几率，以防情况糟糕到 proc_exit 都会崩溃。
		 * 你可能想在此添加的任何其他代码多半都应该放在
		 * on_proc_exit 或 on_shmem_exit 回调中。
		 */
		fflush(NULL);

		/*
		 * 让累计统计系统知情。仅无其他已知原因时，
		 * 才把会话标记为被致命错误终止。
		 */
		if (pgStatSessionEndCause == DISCONNECT_NORMAL)
			pgStatSessionEndCause = DISCONNECT_FATAL;

		/*
		 * 做正常的进程退出清理，然后返回退出码 1 表明
		 * FATAL 终止。postmaster 可能会也可能不会认为
		 * 这值得 panic，取决于哪个子进程返回它。
		 */
		proc_exit(1);
	}

	if (elevel >= PANIC)
	{
		/*
		 * 严重崩溃。postmaster 会观察 SIGABRT 进程退出
		 * 状态并杀死其余后端。
		 *
		 * XXX: 若我们就在 postmaster 内呢？abort() 杀不死
		 * 子进程
		 */
		fflush(NULL);
		abort();
	}

	/*
	 * 先检查 cancel/die 中断 --- 这样用户就可以停止
	 * 一条发出大量通知或警告消息的查询，即使它所在的
	 * 循环通常未能检查中断。
	 */
	CHECK_FOR_INTERRUPTS();
}


/*
 * errsave_start --- 开始"软"错误报告周期
 *
 * 若"context"不是 ErrorSaveContext 节点，行为同
 * errstart(ERROR, domain)，errsave() 宏最终表现得
 * 完全等同 ereport(ERROR, ...)
 *
 * 若"context"是 ErrorSaveContext 节点，但节点创建者只想
 * 获知软错误事实的通知而不要任何细节，我们只需
 * 设置 ErrorSaveContext 节点中的 error_occurred 标志并返回
 * false，这将使我们跳过其余的错误处理步骤。
 *
 * 否则，创建并初始化错误栈条目并返回 true。
 * 随后将调用 errmsg() 或许还有其他例程来进一步
 * 填充该栈条目。最后将调用 errsave_finish() 来
 * 收尾
 */
bool
errsave_start(struct Node *context, const char *domain)
{
	ErrorSaveContext *escontext;
	ErrorData  *edata;

	/*
	 * 我们有软错误报告的 context 吗？若无，直接转给
	 * errstart()
	 */
	if (context == NULL || !IsA(context, ErrorSaveContext))
		return errstart(ERROR, domain);

	/* 报告检测到软错误 */
	escontext = (ErrorSaveContext *) context;
	escontext->error_occurred = true;

	/* 若调用者不需要更多细节，则无事可做 */
	if (!escontext->details_wanted)
		return false;

	/*
	 * 好，建一个栈条目来存储这些信息。
	 */

	recursion_depth++;

	/* 初始化此错误帧的数据 */
	edata = get_error_stack_entry();
	edata->elevel = LOG;		/* 告知 errsave_finish 一切正常 */
	set_stack_entry_domain(edata, domain);
	/* 基于假定的 ERROR elevel 选择默认 errcode */
	edata->sqlerrcode = ERRCODE_INTERNAL_ERROR;

	/*
	 * 此错误状态级别的所有分配都应放入调用者上下文。
	 * 我们不需要污染 ErrorContext，甚至在此代码路径中
	 * 也不要求它存在。
	 */
	edata->assoc_context = CurrentMemoryContext;

	recursion_depth--;
	return true;
}

/*
 * errsave_finish --- 结束"软"错误报告周期
 *
 * 若 errsave_start() 判定这是常规错误，则行为同
 * errfinish()。否则，打包错误细节并将其保存
 * 至 ErrorSaveContext 节点。
 */
void
errsave_finish(struct Node *context, const char *filename, int lineno,
			   const char *funcname)
{
	ErrorSaveContext *escontext = (ErrorSaveContext *) context;
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 访问 *edata 前验证栈深度 */
	CHECK_STACK_DEPTH();

	/*
	 * 若 errsave_start 转给了 errstart，则 elevel 将是
	 * ERROR 甚或 PANIC。同样转给 errfinish
	 */
	if (edata->elevel >= ERROR)
	{
		errfinish(filename, lineno, funcname);
		pg_unreachable();
	}

	/*
	 * 否则，我们应打包栈条目内容并将其交付给
	 * 调用方
	 */
	recursion_depth++;

	/* 将错误状态的最后几位信息存入栈条目 */
	set_stack_entry_location(edata, filename, lineno, funcname);

	/* 替换 errsave_start 插入的 LOG 值 */
	edata->elevel = ERROR;

	/*
	 * 我们跳过回溯与上下文函数的调用，它们惹麻烦的
	 * 可能性比提供有用上下文更大；它们可能基于
	 * 事务即将中止的假设而行动。
	 */

	/*
	 * 为调用者复制一份错误信息。所有附属字符串都
	 * 已在调用者的上下文中，因此对栈条目做平拷贝
	 * 就够了。
	 */
	escontext->error_data = palloc_object(ErrorData);
	memcpy(escontext->error_data, edata, sizeof(ErrorData));

	/* 退出错误处理上下文 */
	errordata_stack_depth--;
	recursion_depth--;
}


/*
 * get_error_stack_entry --- 分配并初始化新的栈条目
 *
 * 用完后应通过调用 FreeErrorDataContents() 并递减
 * errordata_stack_depth 来释放该条目。
 *
 * 返回条目的地址只是记法上的便利，
 * 其最好是 errordata[errordata_stack_depth]。
 *
 * 虽然错误栈不大，但我们不预期会耗尽空间。使用多个
 * 条目意味着错误恢复期间又产生了新的错误报告，
 * 这有可能但已表明我们有麻烦了。如果耗尽了栈，
 * 几乎可以肯定我们陷入了错误恢复期间的错误无限
 * 循环，放弃并 PANIC。
 *
 * （注意这不同于 recursion_depth 检查，后者防范
 * 的是处理单个栈条目期间的递归。）
 */
static ErrorData *
get_error_stack_entry(void)
{
	ErrorData  *edata;

	/* 分配错误帧 */
	errordata_stack_depth++;
	if (unlikely(errordata_stack_depth >= ERRORDATA_STACK_SIZE))
	{
		/* 糟了，栈不够大 */
		errordata_stack_depth = -1; /* 在栈上腾空间 */
		ereport(PANIC, (errmsg_internal("ERRORDATA_STACK_SIZE exceeded")));
	}

	/* 将错误帧初始化为全零/NULL */
	edata = &errordata[errordata_stack_depth];
	memset(edata, 0, sizeof(ErrorData));

	/* 立即保存 errno，确保错误参数求值无法改变它 */
	edata->saved_errno = errno;

	return edata;
}

/*
 * set_stack_entry_domain --- 填入国际化域
 */
static void
set_stack_entry_domain(ErrorData *edata, const char *domain)
{
	/* 默认文本域是后端的 */
	edata->domain = domain ? domain : PG_TEXTDOMAIN("postgres");
	/* 同法初始化 context_domain（见 set_errcontext_domain()） */
	edata->context_domain = edata->domain;
}

/*
 * set_stack_entry_location --- 填入代码位置细节
 *
 * 保存调用点的 __FILE__、__LINE__ 和 __func__ 值。
 * 我们尽力规范化 __FILE__，因为各编译器包含多少
 * 路径成分并不一致，而我们希望行为不依赖于此
 * （尤其是不随构建路径而变）。
 */
static void
set_stack_entry_location(ErrorData *edata,
						 const char *filename, int lineno,
						 const char *funcname)
{
	if (filename)
	{
		const char *slash;

		/* 只保留基本名，对 vpath 构建尤其有用 */
		slash = strrchr(filename, '/');
		if (slash)
			filename = slash + 1;
		/* 一些 Windows 编译器在 __FILE__ 串中使用反斜杠 */
		slash = strrchr(filename, '\\');
		if (slash)
			filename = slash + 1;
	}

	edata->filename = filename;
	edata->lineno = lineno;
	edata->funcname = funcname;
}

/*
 * matches_backtrace_functions --- 检查给定 funcname 是否
 * backtrace_functions
 *
 * See check_backtrace_functions.
 */
static bool
matches_backtrace_functions(const char *funcname)
{
	const char *p;

	if (!backtrace_symbol_list || funcname == NULL || funcname[0] == '\0')
		return false;

	p = backtrace_symbol_list;
	for (;;)
	{
		if (*p == '\0')			/* backtrace_symbol_list 结束 */
			break;

		if (strcmp(funcname, p) == 0)
			return true;
		p += strlen(p) + 1;
	}

	return false;
}


/*
 * errcode --- 给错误添加 SQLSTATE 错误码
 *
 * 该码应按 MAKE_SQLSTATE() 的表示形式给出。
 */
int
errcode(int sqlerrcode)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	edata->sqlerrcode = sqlerrcode;

	return 0;					/* 返回值无关紧要 */
}


/*
 * errcode_for_file_access --- 给错误添加 SQLSTATE 错误码
 *
 * SQLSTATE 码根据保存的 errno 值选择。我们假定
 * 失败的操作是某种磁盘文件访问。
 *
 * 注意：主要错误消息串一般应包含 %m，
 * 当使用它时。
 */
int
errcode_for_file_access(void)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	switch (edata->saved_errno)
	{
			/* 权限被拒类失败 */
		case EPERM:				/* 非root用户 */
		case EACCES:			/* 权限不足 */
#ifdef EROFS
		case EROFS:				/* 只读文件系统 */
#endif
			edata->sqlerrcode = ERRCODE_INSUFFICIENT_PRIVILEGE;
			break;

			/* 无此文件 */
		case ENOENT:			/* 无此文件或目录 */
			edata->sqlerrcode = ERRCODE_UNDEFINED_FILE;
			break;

			/* 文件重复 */
		case EEXIST:			/* 已存在 */
			edata->sqlerrcode = ERRCODE_DUPLICATE_FILE;
			break;

			/* 对象类型/状态错误 */
		case ENOTDIR:			/* 非目录 */
		case EISDIR:			/* 是目录 */
#if defined(ENOTEMPTY) && (ENOTEMPTY != EEXIST) /* AIX 上同码 */
		case ENOTEMPTY:			/* 目录非空 */
#endif
			edata->sqlerrcode = ERRCODE_WRONG_OBJECT_TYPE;
			break;

			/* 资源不足 */
		case ENOSPC:			/* 设备无剩余空间 */
			edata->sqlerrcode = ERRCODE_DISK_FULL;
			break;

		case ENOMEM:			/* 内存耗尽 */
			edata->sqlerrcode = ERRCODE_OUT_OF_MEMORY;
			break;

		case ENFILE:			/* 文件表溢出 */
		case EMFILE:			/* 打开文件过多 */
			edata->sqlerrcode = ERRCODE_INSUFFICIENT_RESOURCES;
			break;

			/* 硬件故障 */
		case EIO:				/* I/O错误 */
			edata->sqlerrcode = ERRCODE_IO_ERROR;
			break;

			/* 其余全部归类为内部错误 */
		default:
			edata->sqlerrcode = ERRCODE_INTERNAL_ERROR;
			break;
	}

	return 0;					/* 返回值无关紧要 */
}

/*
 * errcode_for_socket_access --- 给错误添加 SQLSTATE 错误码
 *
 * SQLSTATE 码根据保存的 errno 值选择。我们假定
 * 失败的操作是某种套接字访问。
 *
 * 注意：主要错误消息串一般应包含 %m，
 * 当使用它时。
 */
int
errcode_for_socket_access(void)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	switch (edata->saved_errno)
	{
			/* 连接丢失 */
		case ALL_CONNECTION_FAILURE_ERRNOS:
			edata->sqlerrcode = ERRCODE_CONNECTION_FAILURE;
			break;

			/* 其余全部归类为内部错误 */
		default:
			edata->sqlerrcode = ERRCODE_INTERNAL_ERROR;
			break;
	}

	return 0;					/* 返回值无关紧要 */
}


/*
 * 该宏处理格式串及其相关参数的展开；
 * 它是 errmsg()、errdetail() 等的公共代码。必须在
 * 声明类似 "const char *fmt, ..." 且已设置 edata
 * 指针例程内调用。消息被赋给 edata->targetfield，
 * 或当 appendval 为 true 时追加到其上。若 translateit
 * 为 true 则翻译
 *
 * 注意：我们 pstrdup 缓冲区而不是直接移交其存储，
 * 到 edata 字段，因为缓冲区可能远大于
 * 所需大小。
 */
#define EVALUATE_MESSAGE(domain, targetfield, appendval, translateit)	\
	{ \
		StringInfoData	buf; \
		/* 国际化错误格式串 */ \
		if ((translateit) && !in_error_recursion_trouble()) \
			fmt = dgettext((domain), fmt);				  \
		initStringInfo(&buf); \
		if ((appendval) && edata->targetfield) { \
			appendStringInfoString(&buf, edata->targetfield); \
			appendStringInfoChar(&buf, '\n'); \
		} \
		/* 生成实际输出 --- 必须使用 appendStringInfoVA */ \
		for (;;) \
		{ \
			va_list		args; \
			int			needed; \
			errno = edata->saved_errno; \
			va_start(args, fmt); \
			needed = appendStringInfoVA(&buf, fmt, args); \
			va_end(args); \
			if (needed == 0) \
				break; \
			enlargeStringInfo(&buf, needed); \
		} \
		/* 将完成的消息保存到栈条目中 */ \
		if (edata->targetfield) \
			pfree(edata->targetfield); \
		edata->targetfield = pstrdup(buf.data); \
		pfree(buf.data); \
	}

/*
 * 同上，但用于复数形式的错误消息。调用例程必须声明
 * 成 "const char *fmt_singular, const char *fmt_plural,
 * unsigned long n, ..."。假定总是需要翻译。
 */
#define EVALUATE_MESSAGE_PLURAL(domain, targetfield, appendval)  \
	{ \
		const char	   *fmt; \
		StringInfoData	buf; \
		/* 国际化错误格式串 */ \
		if (!in_error_recursion_trouble()) \
			fmt = dngettext((domain), fmt_singular, fmt_plural, n); \
		else \
			fmt = (n == 1 ? fmt_singular : fmt_plural); \
		initStringInfo(&buf); \
		if ((appendval) && edata->targetfield) { \
			appendStringInfoString(&buf, edata->targetfield); \
			appendStringInfoChar(&buf, '\n'); \
		} \
		/* 生成实际输出 --- 必须使用 appendStringInfoVA */ \
		for (;;) \
		{ \
			va_list		args; \
			int			needed; \
			errno = edata->saved_errno; \
			va_start(args, n); \
			needed = appendStringInfoVA(&buf, fmt, args); \
			va_end(args); \
			if (needed == 0) \
				break; \
			enlargeStringInfo(&buf, needed); \
		} \
		/* 将完成的消息保存到栈条目中 */ \
		if (edata->targetfield) \
			pfree(edata->targetfield); \
		edata->targetfield = pstrdup(buf.data); \
		pfree(buf.data); \
	}


/*
 * errmsg --- 向当前错误添加主要错误消息文本
 *
 * 除了 printf 识别的常规 %-转义之外，fmt 中的
 * "%m" 将替换为调用者 errno 对应的错误消息。
 *
 * 注意：fmt 串尾不需要换行符，因为 ereport 会
 * 为需要它的输出方法提供换行。
 */
int
errmsg(const char *fmt,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(edata->assoc_context);

	edata->message_id = fmt;
	EVALUATE_MESSAGE(edata->domain, message, false, true);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* 返回值无关紧要 */
}

/*
 * 向所在 ereport() 调用添加回溯。旨在调试期间
 * 临时添加。
 */
int
errbacktrace(void)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(edata->assoc_context);

	set_backtrace(edata, 1);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;

	return 0;
}

/*
 * 计算回溯数据并将其添加到给定的 ErrorData。num_skip
 * 指定要跳过的内层帧数。用它来避免在回溯中显示
 * 内部回溯支持函数。这要求本函数及相关函数
 * 不被内联。
 */
static void
set_backtrace(ErrorData *edata, int num_skip)
{
	StringInfoData errtrace;

	initStringInfo(&errtrace);

#ifdef HAVE_BACKTRACE_SYMBOLS
	{
		void	   *buf[100];
		int			nframes;
		char	  **strfrms;

		nframes = backtrace(buf, lengthof(buf));
		strfrms = backtrace_symbols(buf, nframes);
		if (strfrms == NULL)
			return;

		for (int i = num_skip; i < nframes; i++)
			appendStringInfo(&errtrace, "\n%s", strfrms[i]);
		free(strfrms);
	}
#else
	appendStringInfoString(&errtrace,
						   "backtrace generation is not supported by this installation");
#endif

	edata->backtrace = errtrace.data;
}

/*
 * errmsg_internal --- 向当前错误添加主要错误消息文本
 *
 * 这与 errmsg() 完全相同，但传给 errmsg_internal 的
 * 字符串不被翻译，且通常被排除在
 * 国际化消息字典之外。这应用于"不可能发生"的
 * 情形，这些情形可能不值得花费翻译精力。我们也将其
 * 用于某些我们*绝*不能尝试翻译消息的情形，因为
 * 翻译会失败并导致无限的
 * 错误递归。
 */
int
errmsg_internal(const char *fmt,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(edata->assoc_context);

	edata->message_id = fmt;
	EVALUATE_MESSAGE(edata->domain, message, false, false);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* 返回值无关紧要 */
}


/*
 * errmsg_plural --- 向当前错误添加主要错误消息文本，
 * 并支持消息文本的复数形式
 */
int
errmsg_plural(const char *fmt_singular, const char *fmt_plural,
			  unsigned long n,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(edata->assoc_context);

	edata->message_id = fmt_singular;
	EVALUATE_MESSAGE_PLURAL(edata->domain, message, false);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* 返回值无关紧要 */
}


/*
 * errdetail --- 向当前错误添加细节错误消息文本
 */
int
errdetail(const char *fmt,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(edata->assoc_context);

	EVALUATE_MESSAGE(edata->domain, detail, false, true);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* 返回值无关紧要 */
}


/*
 * errdetail_internal --- 向当前错误添加细节错误消息文本
 *
 * 这与 errdetail() 完全相同，但传给
 * errdetail_internal 的串不被翻译，通常被排除在
 * 国际化消息字典之外。这应用于因某种原因似乎不值
 * 得翻译的细节消息
 * （通常是对普通用户没什么用处）。
 */
int
errdetail_internal(const char *fmt,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(edata->assoc_context);

	EVALUATE_MESSAGE(edata->domain, detail, false, false);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* 返回值无关紧要 */
}


/*
 * errdetail_log --- 向当前错误添加 detail_log 错误消息文本
 */
int
errdetail_log(const char *fmt,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(edata->assoc_context);

	EVALUATE_MESSAGE(edata->domain, detail_log, false, true);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* 返回值无关紧要 */
}

/*
 * errdetail_log_plural --- 向当前错误添加 detail_log 错误消息文本，
 * 并支持消息文本的复数形式
 */
int
errdetail_log_plural(const char *fmt_singular, const char *fmt_plural,
					 unsigned long n,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(edata->assoc_context);

	EVALUATE_MESSAGE_PLURAL(edata->domain, detail_log, false);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* 返回值无关紧要 */
}


/*
 * errdetail_plural --- 向当前错误添加细节错误消息文本，
 * 并支持消息文本的复数形式
 */
int
errdetail_plural(const char *fmt_singular, const char *fmt_plural,
				 unsigned long n,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(edata->assoc_context);

	EVALUATE_MESSAGE_PLURAL(edata->domain, detail, false);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* 返回值无关紧要 */
}


/*
 * errhint --- 向当前错误添加提示错误消息文本
 */
int
errhint(const char *fmt,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(edata->assoc_context);

	EVALUATE_MESSAGE(edata->domain, hint, false, true);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* 返回值无关紧要 */
}


/*
 * errhint_plural --- 向当前错误添加提示错误消息文本，
 * 并支持消息文本的复数形式
 */
int
errhint_plural(const char *fmt_singular, const char *fmt_plural,
			   unsigned long n,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(edata->assoc_context);

	EVALUATE_MESSAGE_PLURAL(edata->domain, hint, false);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* 返回值无关紧要 */
}


/*
 * errcontext_msg --- 向当前错误添加上下文错误消息文本
 *
 * 与其他情形不同，这里允许多次调用以构建一个
 * 上下文信息栈。我们假定较早的调用代表更深层
 * 状态
 */
int
errcontext_msg(const char *fmt,...)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(edata->assoc_context);

	EVALUATE_MESSAGE(edata->context_domain, context, true, true);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
	return 0;					/* 返回值无关紧要 */
}

/*
 * set_errcontext_domain --- 设置 errcontext() 使用的
 *
 * errcontext_msg() 可由与原始 ereport() 不同的模块
 * 调用，所以我们无法用 errstart() 传入的消息域来
 * 翻译它。相反，每个 errcontext_msg() 调用之前都应
 * 由 set_errcontext_domain() 调用指定域。这通常
 * 由 errcontext() 宏透明完成。
 */
int
set_errcontext_domain(const char *domain)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	/* 默认文本域是后端的 */
	edata->context_domain = domain ? domain : PG_TEXTDOMAIN("postgres");

	return 0;					/* 返回值无关紧要 */
}


/*
 * errhidestmt --- 可抑制日志条目的 STATEMENT: 字段
 *
 * 如果消息文本已包含该语句，则应调用此函数。
 */
int
errhidestmt(bool hide_stmt)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	edata->hide_stmt = hide_stmt;

	return 0;					/* 返回值无关紧要 */
}

/*
 * errhidecontext --- 可抑制日志条目的 CONTEXT: 字段
 *
 * 这仅应适用于冗长的调试消息，因为反复包含上下文会
 * 使日志量过度膨胀。
 */
int
errhidecontext(bool hide_ctx)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	edata->hide_ctx = hide_ctx;

	return 0;					/* 返回值无关紧要 */
}

/*
 * errposition --- 向当前错误添加光标位置
 */
int
errposition(int cursorpos)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	edata->cursorpos = cursorpos;

	return 0;					/* 返回值无关紧要 */
}

/*
 * internalerrposition --- 向当前错误添加内部光标位置
 */
int
internalerrposition(int cursorpos)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	edata->internalpos = cursorpos;

	return 0;					/* 返回值无关紧要 */
}

/*
 * internalerrquery --- 向当前错误添加内部查询文本
 *
 * 可传 NULL 以丢弃内部查询文本条目。此情形用于
 * 那些在对错误报告的版面进行编辑加工的错误回调
 * 子例程中使用。
 */
int
internalerrquery(const char *query)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	if (edata->internalquery)
	{
		pfree(edata->internalquery);
		edata->internalquery = NULL;
	}

	if (query)
		edata->internalquery = MemoryContextStrdup(edata->assoc_context, query);

	return 0;					/* 返回值无关紧要 */
}

/*
 * err_generic_string -- 用于设置由 PG_DIAG_xxx 码标识的
 * ErrorData 各串字段
 *
 * 这有意只支持不使用本地化字符串的字段，
 * 从而无需考虑翻译问题。
 *
 * 大多数潜在调用者不应直接使用它，而应优先选用
 * 如 errtablecol() 之类（见 relcache.c）。
 */
int
err_generic_string(int field, const char *str)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	switch (field)
	{
		case PG_DIAG_SCHEMA_NAME:
			set_errdata_field(edata->assoc_context, &edata->schema_name, str);
			break;
		case PG_DIAG_TABLE_NAME:
			set_errdata_field(edata->assoc_context, &edata->table_name, str);
			break;
		case PG_DIAG_COLUMN_NAME:
			set_errdata_field(edata->assoc_context, &edata->column_name, str);
			break;
		case PG_DIAG_DATATYPE_NAME:
			set_errdata_field(edata->assoc_context, &edata->datatype_name, str);
			break;
		case PG_DIAG_CONSTRAINT_NAME:
			set_errdata_field(edata->assoc_context, &edata->constraint_name, str);
			break;
		default:
			elog(ERROR, "unsupported ErrorData field id: %d", field);
			break;
	}

	return 0;					/* 返回值无关紧要 */
}

/*
 * set_errdata_field --- 设置 ErrorData 字段
 */
static void
set_errdata_field(MemoryContextData *cxt, char **ptr, const char *str)
{
	Assert(*ptr == NULL);
	*ptr = MemoryContextStrdup(cxt, str);
}

/*
 * geterrcode --- 返回现设 SQLSTATE 错误码
 *
 * 它仅打算用于错误回调子例程，因为在 elog.c 之外
 * 没有其他地方该概念是有意义的。
 */
int
geterrcode(void)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	return edata->sqlerrcode;
}

/*
 * geterrlevel --- 返回现设 SQLSTATE 错误级别
 *
 * 它仅打算用于错误回调子例程，因为在 elog.c 之外
 * 没有其他地方该概念是有意义的。
 */
int
geterrlevel(void)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	return edata->elevel;
}

/*
 * geterrposition --- 返回现设错误位置（无则为 0）
 *
 * 它仅打算用于错误回调子例程，因为在 elog.c 之外
 * 没有其他地方该概念是有意义的。
 */
int
geterrposition(void)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	return edata->cursorpos;
}

/*
 * getinternalerrposition --- 内部错误位置同理
 *
 * 它仅打算用于错误回调子例程，因为在 elog.c 之外
 * 没有其他地方该概念是有意义的。
 */
int
getinternalerrposition(void)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];

	/* 不必费心递增 recursion_depth */
	CHECK_STACK_DEPTH();

	return edata->internalpos;
}


/*
 * 这些函数允许独立于 ereport() 调用本身来构建错误
 * 消息字符串。
 *
 * 预期的调用约定是
 *
 *	pre_format_elog_string(errno, domain), var = format_elog_string(format,...)
 *
 * 可隐藏在诸如 GUC_check_errdetail() 的宏之后。我们
 * 假定在 format_elog_string() 参数中调用的任何函数
 * 都不会导致这些函数被重入 --- 否则可能用错文本域，
 * 或者为 %m 代入错误的 errno。对当前配合 GUC 检查钩子
 * 的使用没问题，但未来某天可能需要
 * 更多努力
 *
 * format_elog_string() 结果存于 ErrorContext，
 * 故在调用 FlushErrorState() 前一直存续。
 */
static int	save_format_errnumber;
static const char *save_format_domain;

void
pre_format_elog_string(int errnumber, const char *domain)
{
	/* 在参数函数求值可能改变它之前保存 errno */
	save_format_errnumber = errnumber;
	/* 存调用者文本域 */
	save_format_domain = domain;
}

char *
format_elog_string(const char *fmt,...)
{
	ErrorData	errdata;
	ErrorData  *edata;
	MemoryContext oldcontext;

	/* 初始化近似虚设的错误帧 */
	edata = &errdata;
	MemSet(edata, 0, sizeof(ErrorData));
	/* 默认文本域是后端的 */
	edata->domain = save_format_domain ? save_format_domain : PG_TEXTDOMAIN("postgres");
	/* 设置用于解释 %m 的 errno */
	edata->saved_errno = save_format_errnumber;

	oldcontext = MemoryContextSwitchTo(ErrorContext);

	edata->message_id = fmt;
	EVALUATE_MESSAGE(edata->domain, message, false, true);

	MemoryContextSwitchTo(oldcontext);

	return edata->message;
}


/*
 * 栈顶错误消息的实际输出
 *
 * 在 ereport(ERROR) 情形下由 PostgresMain 调用（或根本
 * 不调用，若错误被他人捕获）。对所有其他严重级别，
 * 由 errfinish 调用
 */
void
EmitErrorReport(void)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	MemoryContext oldcontext;

	recursion_depth++;
	CHECK_STACK_DEPTH();
	oldcontext = MemoryContextSwitchTo(edata->assoc_context);

	/*
	 * 在向日志发送消息之前调用钩子。钩子函数可以把
	 * edata->output_to_server 关闭，所以事后必须复查。
	 * 对 edata 内容的其他任何更改都不被视为
	 * 受支持
	 *
	 * 注意：钩子之所以只能关闭 output_to_server 而不能
	 * 打开它，是因为那不可靠：如果 errstart() 判定消息
	 * 无趣，我们根本不会到这里。能朝那个方向做决策的
	 * 钩子必须挂接到 errstart() 中，而那里可用的信息要
	 * 少得多。emit_log_hook 旨在用于自定义日志过滤和自定
	 * 义日志消息传输
	 * 机制。
	 *
	 * 日志钩子可以访问翻译后的和原始英文的错误消息
	 * 文本，传递后者是为了让它能用作消息标识符。注意
	 * 原始文本对 detail、detail_log、hint 和 context 文本
	 * 元素不可用。
	 */
	if (edata->output_to_server && emit_log_hook)
		(*emit_log_hook) (edata);

	/* 启用时送服务器日志 */
	if (edata->output_to_server)
		send_message_to_server_log(edata);

	/* 启用时送客户端 */
	if (edata->output_to_client)
		send_message_to_frontend(edata);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;
}

/*
 * CopyErrorData --- 获取最顶错误栈条目的副本
 *
 * 这仅用于错误处理器代码中。数据被复制到当前内存
 * 上下文，故调用者应始终先离开 ErrorContext；
 * 否则当 FlushErrorState 完成时数据将丢失。
 */
ErrorData *
CopyErrorData(void)
{
	ErrorData  *edata = &errordata[errordata_stack_depth];
	ErrorData  *newedata;

	/*
	 * 我们不递增 recursion_depth，因这里内存耗尽并
	 * 不表示错误子系统内部有问题。
	 */
	CHECK_STACK_DEPTH();

	Assert(CurrentMemoryContext != ErrorContext);

	/* 复制结构体本身 */
	newedata = (ErrorData *) palloc(sizeof(ErrorData));
	memcpy(newedata, edata, sizeof(ErrorData));

	/*
	 * 为单独分配的字符串制作副本。注意我们连理论
	 * 常量字符串（如 filename）也复制。这是因为它们可能
	 * 指向 JIT 建的代码段，其可能在事务清理时卸载
	 * 某些情况下需复制的 ErrorData 存活过事务边界，
	 * 所以最好也复制那些字符串。
	 */
	if (newedata->filename)
		newedata->filename = pstrdup(newedata->filename);
	if (newedata->funcname)
		newedata->funcname = pstrdup(newedata->funcname);
	if (newedata->domain)
		newedata->domain = pstrdup(newedata->domain);
	if (newedata->context_domain)
		newedata->context_domain = pstrdup(newedata->context_domain);
	if (newedata->message)
		newedata->message = pstrdup(newedata->message);
	if (newedata->detail)
		newedata->detail = pstrdup(newedata->detail);
	if (newedata->detail_log)
		newedata->detail_log = pstrdup(newedata->detail_log);
	if (newedata->hint)
		newedata->hint = pstrdup(newedata->hint);
	if (newedata->context)
		newedata->context = pstrdup(newedata->context);
	if (newedata->backtrace)
		newedata->backtrace = pstrdup(newedata->backtrace);
	if (newedata->message_id)
		newedata->message_id = pstrdup(newedata->message_id);
	if (newedata->schema_name)
		newedata->schema_name = pstrdup(newedata->schema_name);
	if (newedata->table_name)
		newedata->table_name = pstrdup(newedata->table_name);
	if (newedata->column_name)
		newedata->column_name = pstrdup(newedata->column_name);
	if (newedata->datatype_name)
		newedata->datatype_name = pstrdup(newedata->datatype_name);
	if (newedata->constraint_name)
		newedata->constraint_name = pstrdup(newedata->constraint_name);
	if (newedata->internalquery)
		newedata->internalquery = pstrdup(newedata->internalquery);

	/* 用调用者上下文进行字符串分配 */
	newedata->assoc_context = CurrentMemoryContext;

	return newedata;
}

/*
 * FreeErrorData --- 释放 CopyErrorData 返回的结构。
 *
 * 错误处理器应优先使用它，而不是假定自己知道
 * 所有单独分配字段。
 */
void
FreeErrorData(ErrorData *edata)
{
	FreeErrorDataContents(edata);
	pfree(edata);
}

/*
 * FreeErrorDataContents --- 释放 ErrorData 的附属数据。
 *
 * 它既可用于错误栈条目，也可用于复制的 ErrorData。
 */
static void
FreeErrorDataContents(ErrorData *edata)
{
	if (edata->message)
		pfree(edata->message);
	if (edata->detail)
		pfree(edata->detail);
	if (edata->detail_log)
		pfree(edata->detail_log);
	if (edata->hint)
		pfree(edata->hint);
	if (edata->context)
		pfree(edata->context);
	if (edata->backtrace)
		pfree(edata->backtrace);
	if (edata->schema_name)
		pfree(edata->schema_name);
	if (edata->table_name)
		pfree(edata->table_name);
	if (edata->column_name)
		pfree(edata->column_name);
	if (edata->datatype_name)
		pfree(edata->datatype_name);
	if (edata->constraint_name)
		pfree(edata->constraint_name);
	if (edata->internalquery)
		pfree(edata->internalquery);
}

/*
 * FlushErrorState --- 错误恢复后冲刷错误状态
 *
 * 错误处理器处理完错误后应调用它；或者一旦完成
 * CopyErrorData，若打算做可能再引发错误的事情时
 * 就调用。做完之前你尚未"脱离"
 * 错误子系统。
 */
void
FlushErrorState(void)
{
	/*
	 * 把栈重置为空。深度超过一层的唯一情形，是我们
	 * 处理过某个打断了另一条消息构建的错误。假定
	 * 控制权已逃离该消息构建且永不
	 * 会再回来。
	 */
	errordata_stack_depth = -1;
	recursion_depth = 0;
	/* 删除 ErrorContext 数据 */
	MemoryContextResetAndDeleteChildren(ErrorContext);
}

/*
 * ThrowErrorData --- 报告一个 ErrorData 结构描述的错误
 *
 * 这有点像 ReThrowError，但它允许 ERROR 以外的 elevel，
 * 且 output_to_server 等布尔标志按默认规则计算，
 * 而不是从给定的 ErrorData 复制。
 * 主要用于重新报告最初由后台工作进程报告、
 * 然后（经修改或不经修改）传播到负责它们的
 * 后端的错误。
 */
void
ThrowErrorData(ErrorData *edata)
{
	ErrorData  *newedata;
	MemoryContext oldcontext;

	if (!errstart(edata->elevel, edata->domain))
		return;					/* 错误完全不予报告 */

	newedata = &errordata[errordata_stack_depth];
	recursion_depth++;
	oldcontext = MemoryContextSwitchTo(newedata->assoc_context);

	/* 把给定的字段复制到错误栈条目。 */
	if (edata->sqlerrcode != 0)
		newedata->sqlerrcode = edata->sqlerrcode;
	if (edata->message)
		newedata->message = pstrdup(edata->message);
	if (edata->detail)
		newedata->detail = pstrdup(edata->detail);
	if (edata->detail_log)
		newedata->detail_log = pstrdup(edata->detail_log);
	if (edata->hint)
		newedata->hint = pstrdup(edata->hint);
	if (edata->context)
		newedata->context = pstrdup(edata->context);
	if (edata->backtrace)
		newedata->backtrace = pstrdup(edata->backtrace);
	/* 假定 message_id 不可用 */
	if (edata->schema_name)
		newedata->schema_name = pstrdup(edata->schema_name);
	if (edata->table_name)
		newedata->table_name = pstrdup(edata->table_name);
	if (edata->column_name)
		newedata->column_name = pstrdup(edata->column_name);
	if (edata->datatype_name)
		newedata->datatype_name = pstrdup(edata->datatype_name);
	if (edata->constraint_name)
		newedata->constraint_name = pstrdup(edata->constraint_name);
	newedata->cursorpos = edata->cursorpos;
	newedata->internalpos = edata->internalpos;
	if (edata->internalquery)
		newedata->internalquery = pstrdup(edata->internalquery);

	MemoryContextSwitchTo(oldcontext);
	recursion_depth--;

	/* 处理该错误 */
	errfinish(edata->filename, edata->lineno, edata->funcname);
}

/*
 * ReThrowError --- 重抛先前复制的错误
 *
 * 处理器可以执行 CopyErrorData/FlushErrorState 以离开
 * 错误子系统，然后做些处理，最后用 ReThrowError 重抛
 * 原始错误。这比 PG_RE_THROW() 慢，但若"某些处理"
 * 很可能引发另一个错误，就应使用它。
 */
void
ReThrowError(ErrorData *edata)
{
	ErrorData  *newedata;

	Assert(edata->elevel == ERROR);

	/* 把数据推回错误上下文 */
	recursion_depth++;
	MemoryContextSwitchTo(ErrorContext);

	newedata = get_error_stack_entry();
	memcpy(newedata, edata, sizeof(ErrorData));

	/* 复制单独分配的字段 */
	if (newedata->message)
		newedata->message = pstrdup(newedata->message);
	if (newedata->detail)
		newedata->detail = pstrdup(newedata->detail);
	if (newedata->detail_log)
		newedata->detail_log = pstrdup(newedata->detail_log);
	if (newedata->hint)
		newedata->hint = pstrdup(newedata->hint);
	if (newedata->context)
		newedata->context = pstrdup(newedata->context);
	if (newedata->backtrace)
		newedata->backtrace = pstrdup(newedata->backtrace);
	if (newedata->schema_name)
		newedata->schema_name = pstrdup(newedata->schema_name);
	if (newedata->table_name)
		newedata->table_name = pstrdup(newedata->table_name);
	if (newedata->column_name)
		newedata->column_name = pstrdup(newedata->column_name);
	if (newedata->datatype_name)
		newedata->datatype_name = pstrdup(newedata->datatype_name);
	if (newedata->constraint_name)
		newedata->constraint_name = pstrdup(newedata->constraint_name);
	if (newedata->internalquery)
		newedata->internalquery = pstrdup(newedata->internalquery);

	/* 把 assoc_context 重置为 ErrorContext */
	newedata->assoc_context = ErrorContext;

	recursion_depth--;
	PG_RE_THROW();
}

/*
 * pg_re_throw --- PG_RE_THROW() 宏的非内联实现
 */
void
pg_re_throw(void)
{
	/* 如有可能，把错误抛给下一外层 setjmp 处理器 */
	if (PG_exception_stack != NULL)
		siglongjmp(*PG_exception_stack, 1);
	else
	{
		/*
		 * 到这里说明 elog(ERROR) 在 PG_TRY 块内抛出，
		 * 而现在退出了块却发现无外层 setjmp 处理器
		 * 可接收错误。若错误起初在块外抛出，
		 * 我们本会把它提升为 FATAL，故现在正确的做法
		 * 是将其变为 FATAL：即发出它并调用
		 * proc_exit。
		 */
		ErrorData  *edata = &errordata[errordata_stack_depth];

		Assert(errordata_stack_depth >= 0);
		Assert(edata->elevel == ERROR);
		edata->elevel = FATAL;

		/*
		 * 至少原则上，严重级别提升可能改变输出去向
		 * 的决策，所以重新计算。
		 */
		edata->output_to_server = should_output_to_server(FATAL);
		edata->output_to_client = should_output_to_client(FATAL);

		/*
		 * 其余可用 errfinish()，但不想让它二次调用
		 * 任何错误上下文例程。既知即将退出，
		 * 直接清空上下文栈应该没问题。
		 */
		error_context_stack = NULL;

		errfinish(edata->filename, edata->lineno, edata->funcname);
	}

	/* 不返回 ... */
	ExceptionalCondition("pg_re_throw tried to return", __FILE__, __LINE__);
}


/*
 * GetErrorContextStack - 返回上下文栈供显示/诊断
 *
 * 在调用者的上下文中返回一个 pstrdup 的字符串，其中
 * 包含 PG 错误调用栈。调用者有责确保用完后此串
 * 被 pfree（或其上下文被清理）。
 *
 * 这些信息通过遍历错误上下文并调用每个上下文的
 * 回调函数来收集，每个回调都应调用 errcontext()
 * 返回一个可呈现给用户的字符串。
 */
char *
GetErrorContextStack(void)
{
	ErrorData  *edata;
	ErrorContextCallback *econtext;

	/*
	 * 建一个栈条目来存储这些信息。
	 */
	recursion_depth++;

	edata = get_error_stack_entry();

	/*
	 * 把 assoc_context 设为调用者的上下文，这样任何
	 * 分配（含 edata->context）都将用其上下文。
	 */
	edata->assoc_context = CurrentMemoryContext;

	/*
	 * 调用所有上下文回调函数以把上下文信息收集到
	 * edata->context
	 *
	 * 回调函数中发生的错误应走常规错误处理代码，
	 * 它应处理任何递归错误，不过为保险起见我们在上方
	 * 做了双重检查。
	 */
	for (econtext = error_context_stack;
		 econtext != NULL;
		 econtext = econtext->previous)
		econtext->callback(econtext->arg);

	/*
	 * 把自己从栈上清除。已做的任何分配本应使用
	 * edata->assoc_context，即我们先前设为调用者上下文的
	 * 那个，所以可直接把条目从栈上移除并递减
	 * 递归深度后退出。
	 */
	errordata_stack_depth--;
	recursion_depth--;

	/*
	 * 返回指向调用者所请求字符串的指针，它应已分配
	 * 在其上下文中。
	 */
	return edata->context;
}


/*
 * 错误输出文件的初始化
 */
void
DebugFileOpen(void)
{
	int			fd,
				istty;

	if (OutputFileName[0])
	{
		/*
		 * 给定了调试输出文件名
		 *
		 * 确保可写该文件，并查明它是否 tty。
		 */
		if ((fd = open(OutputFileName, O_CREAT | O_APPEND | O_WRONLY,
					   0666)) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", OutputFileName)));
		istty = isatty(fd);
		close(fd);

		/*
		 * 将 stderr 重定向到调试输出文件
		 */
		if (!freopen(OutputFileName, "a", stderr))
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not reopen file \"%s\" as stderr: %m",
							OutputFileName)));

		/*
		 * 若文件是 tty 且运行在 postmaster 下，则尝试
		 * 把 stdout 也发到那里（若它不是 tty，stderr 会
		 * 挡住 stdout，所以不妨让 stdout 照旧流
		 * 动
		 */
		if (istty && IsUnderPostmaster)
			if (!freopen(OutputFileName, "a", stdout))
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("could not reopen file \"%s\" as stdout: %m",
								OutputFileName)));
	}
}


/*
 * backtrace_functions 的检查钩子
 *
 * 我们拆分输入串，其中逗号分隔函数名，
 * 某些空白字符被忽略，拆成以 \0 分隔（并以
 * \0\0 结尾）的名字列表。此形式便于抛错时
 * 扫描，同时避免使用不可重入的 strtok()，
 * 并把输出数据保持在单个 palloc()
 * 块中。
 */
bool
check_backtrace_functions(char **newval, void **extra, GucSource source)
{
	int			newvallen = strlen(*newval);
	char	   *someval;
	int			validlen;
	int			i;
	int			j;

	/*
	 * 允许可作为 C 标识符的字符和逗号作分隔符，
	 * 也可以一些空白增进可读性。
	 */
	validlen = strspn(*newval,
					  "0123456789_"
					  "abcdefghijklmnopqrstuvwxyz"
					  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
					  ", \n\t");
	if (validlen != newvallen)
	{
		GUC_check_errdetail("invalid character");
		return false;
	}

	if (*newval[0] == '\0')
	{
		*extra = NULL;
		return true;
	}

	/*
	 * 为输出分配空间并创建副本。本可不计空白字符
	 * 以节省一些内存，但似乎不值得
	 * 费事
	 */
	someval = guc_malloc(ERROR, newvallen + 1 + 1);
	for (i = 0, j = 0; i < newvallen; i++)
	{
		if ((*newval)[i] == ',')
			someval[j++] = '\0';	/* 下一项 */
		else if ((*newval)[i] == ' ' ||
				 (*newval)[i] == '\n' ||
				 (*newval)[i] == '\t')
			;					/* 忽略这些 */
		else
			someval[j++] = (*newval)[i];	/* 复制其他一切 */
	}

	/* 双 \0 结束设置 */
	someval[j] = '\0';
	someval[j + 1] = '\0';

	*extra = someval;
	return true;
}

/*
 * backtrace_functions 的赋值钩子
 */
void
assign_backtrace_functions(const char *newval, void *extra)
{
	backtrace_symbol_list = (char *) extra;
}

/*
 * log_destination 检查钩子
 */
bool
check_log_destination(char **newval, void **extra, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	int			newlogdest = 0;
	int		   *myextra;

	/* 需字符串的可修改副本 */
	rawstring = pstrdup(*newval);

	/* 把字符串解析为标识符列表 */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* 列表语法错误 */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);

		if (pg_strcasecmp(tok, "stderr") == 0)
			newlogdest |= LOG_DESTINATION_STDERR;
		else if (pg_strcasecmp(tok, "csvlog") == 0)
			newlogdest |= LOG_DESTINATION_CSVLOG;
		else if (pg_strcasecmp(tok, "jsonlog") == 0)
			newlogdest |= LOG_DESTINATION_JSONLOG;
#ifdef HAVE_SYSLOG
		else if (pg_strcasecmp(tok, "syslog") == 0)
			newlogdest |= LOG_DESTINATION_SYSLOG;
#endif
#ifdef WIN32
		else if (pg_strcasecmp(tok, "eventlog") == 0)
			newlogdest |= LOG_DESTINATION_EVENTLOG;
#endif
		else
		{
			GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
			pfree(rawstring);
			list_free(elemlist);
			return false;
		}
	}

	pfree(rawstring);
	list_free(elemlist);

	myextra = (int *) guc_malloc(ERROR, sizeof(int));
	*myextra = newlogdest;
	*extra = (void *) myextra;

	return true;
}

/*
 * log_destination 的赋值钩子
 */
void
assign_log_destination(const char *newval, void *extra)
{
	Log_destination = *((int *) extra);
}

/*
 * syslog_ident 的赋值钩子
 */
void
assign_syslog_ident(const char *newval, void *extra)
{
#ifdef HAVE_SYSLOG
	/*
	 * guc.c 很可能以相同参数反复调用我们，所以不要
	 * 不必要地折腾 syslog 连接。另外，我们直到需要时
	 * 才重新打开连接，因为不管 Log_destination 是否真的
	 * 提到 syslog，本例程都会被调用。
	 *
	 * 注意我们自己制作 ident 串副本而不依赖 guc.c。
	 * 可能过分谨慎，但这确保不会意外释放 syslog
	 * 仍在使用的字符串。
	 */
	if (syslog_ident == NULL || strcmp(syslog_ident, newval) != 0)
	{
		if (openlog_done)
		{
			closelog();
			openlog_done = false;
		}
		free(syslog_ident);
		syslog_ident = strdup(newval);
		/* 若 strdup 失败，将在 write_syslog() 中应对 */
	}
#endif
	/* 无 syslog 支持时直接忽略 */
}

/*
 * syslog_facility 的赋值钩子
 */
void
assign_syslog_facility(int newval, void *extra)
{
#ifdef HAVE_SYSLOG
	/*
	 * 同上，不要不必要地折腾 syslog 连接。
	 */
	if (syslog_facility != newval)
	{
		if (openlog_done)
		{
			closelog();
			openlog_done = false;
		}
		syslog_facility = newval;
	}
#endif
	/* 无 syslog 支持时直接忽略 */
}

#ifdef HAVE_SYSLOG

/*
 * 写一行消息到 syslog
 */
static void
write_syslog(int level, const char *line)
{
	static unsigned long seq = 0;

	int			len;
	const char *nlpos;

	/* 若未打开则打开 syslog 连接 */
	if (!openlog_done)
	{
		openlog(syslog_ident ? syslog_ident : "postgres",
				LOG_PID | LOG_NDELAY | LOG_NOWAIT,
				syslog_facility);
		openlog_done = true;
	}

	/*
	 * 我们给每条日志消息加序号以抑制"相同"
	 * 消息
	 */
	seq++;

	/*
	 * 我们的问题在于许多 syslog 实现不能以可接受的
	 * 方式处理长消息。虽然此函数无助于改变这一事实，
	 * 但它通过把消息拆成较小的片段来绕过。
	 *
	 * 如果消息太长或包含内嵌换行符，我们就分成多次
	 * syslog() 调用。
	 */
	len = strlen(line);
	nlpos = strchr(line, '\n');
	if (syslog_split_messages && (len > PG_SYSLOG_LIMIT || nlpos != NULL))
	{
		int			chunk_nr = 0;

		while (len > 0)
		{
			char		buf[PG_SYSLOG_LIMIT + 1];
			int			buflen;
			int			i;

			/* 若起点是换行符，则前移一字符 */
			if (line[0] == '\n')
			{
				line++;
				len--;
				/* 也需重新计算下一换行符的位置 */
				nlpos = strchr(line, '\n');
				continue;
			}

			/* 复制一行或能放下的量到 buf */
			if (nlpos != NULL)
				buflen = nlpos - line;
			else
				buflen = len;
			buflen = Min(buflen, PG_SYSLOG_LIMIT);
			memcpy(buf, line, buflen);
			buf[buflen] = '\0';

			/* 修剪到多字节字符边界 */
			buflen = pg_mbcliplen(buf, buflen, buflen);
			if (buflen <= 0)
				return;
			buf[buflen] = '\0';

			/* 已是词边界吗？  */
			if (line[buflen] != '\0' &&
				!isspace((unsigned char) line[buflen]))
			{
				/* 尝试在词边界处分割 */
				i = buflen - 1;
				while (i > 0 && !isspace((unsigned char) buf[i]))
					i--;

				if (i > 0)		/* 否则无法按词边界分割 */
				{
					buflen = i;
					buf[i] = '\0';
				}
			}

			chunk_nr++;

			if (syslog_sequence_numbers)
				syslog(level, "[%lu-%d] %s", seq, chunk_nr, buf);
			else
				syslog(level, "[%d] %s", chunk_nr, buf);

			line += buflen;
			len -= buflen;
		}
	}
	else
	{
		/* 消息足够短 */
		if (syslog_sequence_numbers)
			syslog(level, "[%lu] %s", seq, line);
		else
			syslog(level, "%s", line);
	}
}
#endif							/* HAVE_SYSLOG */

#ifdef WIN32
/*
 * 获取 Windows ANSI 代码页的 PostgreSQL 等价物。"ANSI"
 * 系统接口（如 CreateFileA()）期望字符串参数采用这种
 * 编码。给定系统中的每个进程在任何时候都会得到
 */
static int
GetACPEncoding(void)
{
	static int	encoding = -2;

	if (encoding == -2)
		encoding = pg_codepage_to_encoding(GetACP());

	return encoding;
}

/*
 * 向 Windows 事件日志写一行消息
 */
static void
write_eventlog(int level, const char *line, int len)
{
	WCHAR	   *utf16;
	int			eventlevel = EVENTLOG_ERROR_TYPE;
	static HANDLE evtHandle = INVALID_HANDLE_VALUE;

	if (evtHandle == INVALID_HANDLE_VALUE)
	{
		evtHandle = RegisterEventSource(NULL,
										event_source ? event_source : DEFAULT_EVENT_SOURCE);
		if (evtHandle == NULL)
		{
			evtHandle = INVALID_HANDLE_VALUE;
			return;
		}
	}

	switch (level)
	{
		case DEBUG5:
		case DEBUG4:
		case DEBUG3:
		case DEBUG2:
		case DEBUG1:
		case LOG:
		case LOG_SERVER_ONLY:
		case INFO:
		case NOTICE:
			eventlevel = EVENTLOG_INFORMATION_TYPE;
			break;
		case WARNING:
		case WARNING_CLIENT_ONLY:
			eventlevel = EVENTLOG_WARNING_TYPE;
			break;
		case ERROR:
		case FATAL:
		case PANIC:
		default:
			eventlevel = EVENTLOG_ERROR_TYPE;
			break;
	}

	/*
	 * 若消息字符编码与 ReportEventA() 期望的相符，
	 * 则调用它以避免转换的风险。否则，尝试把消息转成
	 * UTF16 并用 ReportEventW() 写出。若转换失败则回退到
	 * ReportEventA()。
	 *
	 * 由于转换所需结构是 palloc 的，若尚未设置
	 * CurrentMemoryContext，就回退为写
	 * 未转换内容。
	 *
	 * 还要验证我们没有正因 pgwin32_message_to_UTF16()
	 * 深处抛出的错误消息而步入错误递归困境。
	 */
	if (!in_error_recursion_trouble() &&
		CurrentMemoryContext != NULL &&
		GetMessageEncoding() != GetACPEncoding())
	{
		utf16 = pgwin32_message_to_UTF16(line, len, NULL);
		if (utf16)
		{
			ReportEventW(evtHandle,
						 eventlevel,
						 0,
						 0,		/* 事件 Id 均为 0 */
						 NULL,
						 1,
						 0,
						 (LPCWSTR *) &utf16,
						 NULL);
			/* XXX ReportEventW() 失败时用 ReportEventA()? */

			pfree(utf16);
			return;
		}
	}
	ReportEventA(evtHandle,
				 eventlevel,
				 0,
				 0,				/* 事件 Id 均为 0 */
				 NULL,
				 1,
				 0,
				 &line,
				 NULL);
}
#endif							/* WIN32 */

static void
write_console(const char *line, int len)
{
	int			rc;

#ifdef WIN32

	/*
	 * 尝试把消息转成 UTF16 并用 WriteConsoleW() 写出。
	 * 失败则回退到 write()。
	 *
	 * 与 write_eventlog() 不同，不要基于适用的编码就
	 * 直接跳到 write()。与 WriteConsoleW() 不同，write()
	 * 依赖于控制台输出代码页是否合适。由于我们在
	 * SubPostmasterMain() 中把 stderr 置为二进制模式，
	 * write() 会跳过翻译。
	 *
	 * WriteConsoleW() 在 stderr 被重定向时会失败，所以
	 * 此情形就直接写未转换内容到日志文件
	 *
	 * 由于转换所需结构是 palloc 的，若尚未设置
	 * CurrentMemoryContext，就回退为写
	 * 未转换内容。
	 */
	if (!in_error_recursion_trouble() &&
		!redirection_done &&
		CurrentMemoryContext != NULL)
	{
		WCHAR	   *utf16;
		int			utf16len;

		utf16 = pgwin32_message_to_UTF16(line, len, &utf16len);
		if (utf16 != NULL)
		{
			HANDLE		stdHandle;
			DWORD		written;

			stdHandle = GetStdHandle(STD_ERROR_HANDLE);
			if (WriteConsoleW(stdHandle, utf16, utf16len, &written, NULL))
			{
				pfree(utf16);
				return;
			}

			/*
			 * 万一 WriteConsoleW() 失败，回退为写未
			 * 转换的消息
			 */
			pfree(utf16);
		}
	}
#else

	/*
	 * 非 win32 平台上的转换尚未实现。它需要一个
	 * pg_do_encoding_conversion() 非抛出版本，把无法
	 * 转换的字符变成 '?' 且不报错。
	 *
	 * XXX: 现已有无抛出版本，不过不转换成 '?'。
	 */
#endif

	/*
	 * 我们忽略 write() 在这里的任何错误。没有
	 * 缺乏有用的上报手段 ... 向 stderr 抱怨多半无用
	 */
	rc = write(fileno(stderr), line, len);
	(void) rc;
}

/*
 * get_formatted_log_time -- 计算并获取日志时间戳
 *
 * 时间戳若尚未设置则计算，以使其在所有要求一致的
 * 日志目标中保持一致。注意计算出的时间戳返回在
 * 静态缓冲区中，并非经 palloc
 * 分配。
 */
char *
get_formatted_log_time(void)
{
	pg_time_t	stamp_time;
	char		msbuf[13];

	/* 已计算则返回 */
	if (formatted_log_time[0] != '\0')
		return formatted_log_time;

	if (!saved_timeval_set)
	{
		gettimeofday(&saved_timeval, NULL);
		saved_timeval_set = true;
	}

	stamp_time = (pg_time_t) saved_timeval.tv_sec;

	/*
	 * 注意：我们预期 guc.c 会确保 log_timezone 已设置
	 * （至少为最小 GMT 值）后，Log_line_prefix 才能变为
	 * 非空或选择 CSV 模式。
	 */
	pg_strftime(formatted_log_time, FORMATTED_TS_LEN,
	/* 为毫秒留出空间... */
				"%Y-%m-%d %H:%M:%S     %Z",
				pg_localtime(&stamp_time, log_timezone));

	/* 把毫秒'粘贴'到位... */
	sprintf(msbuf, ".%03d", (int) (saved_timeval.tv_usec / 1000));
	memcpy(formatted_log_time + 19, msbuf, 4);

	return formatted_log_time;
}

/*
 * reset_formatted_start_time -- 重置启动时间戳
 */
void
reset_formatted_start_time(void)
{
	formatted_start_time[0] = '\0';
}

/*
 * get_formatted_start_time -- 计算并获取启动时间戳。
 *
 * 时间戳若尚未设置则计算。注意计算出的时间戳
 * 返回在静态缓冲区中，而非 palloc() 的。
 */
char *
get_formatted_start_time(void)
{
	pg_time_t	stamp_time = (pg_time_t) MyStartTime;

	/* 已计算则返回 */
	if (formatted_start_time[0] != '\0')
		return formatted_start_time;

	/*
	 * 注意：我们预期 guc.c 会确保 log_timezone 已设置
	 * （至少为最小 GMT 值）后，Log_line_prefix 才能变为
	 * 非空或选择 CSV 模式。
	 */
	pg_strftime(formatted_start_time, FORMATTED_TS_LEN,
				"%Y-%m-%d %H:%M:%S %Z",
				pg_localtime(&stamp_time, log_timezone));

	return formatted_start_time;
}

/*
 * check_log_of_query -- 检查查询能否被记录
 */
bool
check_log_of_query(ErrorData *edata)
{
	/* 需要日志 */
	if (!is_log_level_output(edata->elevel, log_min_error_statement))
		return false;

	/* 想记录查询 */
	if (edata->hide_stmt)
		return false;

	/* 查询串可用吗 */
	if (debug_query_string == NULL)
		return false;

	return true;
}

/*
 * get_backend_type_for_log -- 日志条目的后端类型
 *
 * 返回静态缓冲区指针，并非 palloc 的。
 */
const char *
get_backend_type_for_log(void)
{
	const char *backend_type_str;

	if (MyProcPid == PostmasterPid)
		backend_type_str = "postmaster";
	else if (MyBackendType == B_BG_WORKER)
		backend_type_str = MyBgworkerEntry->bgw_type;
	else
		backend_type_str = GetBackendTypeDesc(MyBackendType);

	return backend_type_str;
}

/*
 * process_log_prefix_padding --- 处理 log_line_prefix 的
 * 格式串辅助函数
 *
 * 注意：若在格式串中发现它认为无效的东西，
 * 则返回 NULL。
 */
static const char *
process_log_prefix_padding(const char *p, int *ppadding)
{
	int			paddingsign = 1;
	int			padding = 0;

	if (*p == '-')
	{
		p++;

		if (*p == '\0')			/* buf 是否以 %- 结尾 */
			return NULL;
		paddingsign = -1;
	}

	/* 生成数字串的整数形式 */
	while (*p >= '0' && *p <= '9')
		padding = padding * 10 + (*p++ - '0');

	/* 若格式以填充数字结尾，则该格式无效 */
	if (*p == '\0')
		return NULL;

	padding *= paddingsign;
	*ppadding = padding;
	return p;
}

/*
 * 用 Log_line_prefix 格式化日志状态信息
 */
static void
log_line_prefix(StringInfo buf, ErrorData *edata)
{
	log_status_format(buf, Log_line_prefix, edata);
}

/*
 * 格式化日志状态信息，追加到缓冲区。
 */
void
log_status_format(StringInfo buf, const char *format, ErrorData *edata)
{
	/* 行号专用静态计数器 */
	static long log_line_number = 0;

	/* 计数器当前进程中是否已重置 */
	static int	log_my_pid = 0;
	int			padding;
	const char *p;

	/*
	 * 这是少数我们不愿从 postmaster 继承静态变量值
	 * 之一。但既然会继承，就在 MyProcPid 变化时
	 * 重置它。MyProcPid 变化时 MyStartTime 也会变，所以
	 * 也重置格式化的启动时间戳。
	 */
	if (log_my_pid != MyProcPid)
	{
		log_line_number = 0;
		log_my_pid = MyProcPid;
		reset_formatted_start_time();
	}
	log_line_number++;

	if (format == NULL)
		return;					/* 以防 guc 尚未运行 */

	for (p = format; *p != '\0'; p++)
	{
		if (*p != '%')
		{
			/* 字面字符，复制 */
			appendStringInfoChar(buf, *p);
			continue;
		}

		/* 必为 '%'，故跳到下一字符 */
		p++;
		if (*p == '\0')
			break;				/* 格式错误 - 忽略 */
		else if (*p == '%')
		{
			/* 串含 %% */
			appendStringInfoChar(buf, '%');
			continue;
		}


		/*
		 * 处理 '%' 之后可能存在的任何格式。注意
		 * 有填充数字时 process_log_prefix_padding 会把 p 移
		 * 过
		 *
		 * 注意：只有 '-'、'0' 到 '9' 是有效格式字符，
		 * 可在此快速预检查格式。若字符不是格式字符，
		 * 那么我们可以跳过无用的函数调用。
		 *
		 * 另注：至少某些平台上，传 %*s 而非 %s 给
		 * appendStringInfo() 明显更慢，故下面多数情形
		 * 都避免这样做，除非实际指定了非零
		 * 填充
		 */
		if (*p > '9')
			padding = 0;
		else if ((p = process_log_prefix_padding(p, &padding)) == NULL)
			break;

		/* 处理该选项 */
		switch (*p)
		{
			case 'a':
				if (MyProcPort)
				{
					const char *appname = application_name;

					if (appname == NULL || *appname == '\0')
						appname = _("[unknown]");
					if (padding != 0)
						appendStringInfo(buf, "%*s", padding, appname);
					else
						appendStringInfoString(buf, appname);
				}
				else if (padding != 0)
					appendStringInfoSpaces(buf,
										   padding > 0 ? padding : -padding);

				break;
			case 'b':
				{
					const char *backend_type_str = get_backend_type_for_log();

					if (padding != 0)
						appendStringInfo(buf, "%*s", padding, backend_type_str);
					else
						appendStringInfoString(buf, backend_type_str);
					break;
				}
			case 'u':
				if (MyProcPort)
				{
					const char *username = MyProcPort->user_name;

					if (username == NULL || *username == '\0')
						username = _("[unknown]");
					if (padding != 0)
						appendStringInfo(buf, "%*s", padding, username);
					else
						appendStringInfoString(buf, username);
				}
				else if (padding != 0)
					appendStringInfoSpaces(buf,
										   padding > 0 ? padding : -padding);
				break;
			case 'd':
				if (MyProcPort)
				{
					const char *dbname = MyProcPort->database_name;

					if (dbname == NULL || *dbname == '\0')
						dbname = _("[unknown]");
					if (padding != 0)
						appendStringInfo(buf, "%*s", padding, dbname);
					else
						appendStringInfoString(buf, dbname);
				}
				else if (padding != 0)
					appendStringInfoSpaces(buf,
										   padding > 0 ? padding : -padding);
				break;
			case 'c':
				if (padding != 0)
				{
					char		strfbuf[128];

					snprintf(strfbuf, sizeof(strfbuf) - 1, "%" INT64_MODIFIER "x.%x",
							 MyStartTime, MyProcPid);
					appendStringInfo(buf, "%*s", padding, strfbuf);
				}
				else
					appendStringInfo(buf, "%" INT64_MODIFIER "x.%x", MyStartTime, MyProcPid);
				break;
			case 'p':
				if (padding != 0)
					appendStringInfo(buf, "%*d", padding, MyProcPid);
				else
					appendStringInfo(buf, "%d", MyProcPid);
				break;

			case 'P':
				if (MyProc)
				{
					PGPROC	   *leader = MyProc->lockGroupLeader;

					/*
					 * 仅活跃并行工作进程显示领导者。这
					 * 漏掉并行组中的领导者。
					 */
					if (leader == NULL || leader->pid == MyProcPid)
						appendStringInfoSpaces(buf,
											   padding > 0 ? padding : -padding);
					else if (padding != 0)
						appendStringInfo(buf, "%*d", padding, leader->pid);
					else
						appendStringInfo(buf, "%d", leader->pid);
				}
				else if (padding != 0)
					appendStringInfoSpaces(buf,
										   padding > 0 ? padding : -padding);
				break;

			case 'l':
				if (padding != 0)
					appendStringInfo(buf, "%*ld", padding, log_line_number);
				else
					appendStringInfo(buf, "%ld", log_line_number);
				break;
			case 'm':
				/* 强制日志时间戳重置 */
				formatted_log_time[0] = '\0';
				(void) get_formatted_log_time();

				if (padding != 0)
					appendStringInfo(buf, "%*s", padding, formatted_log_time);
				else
					appendStringInfoString(buf, formatted_log_time);
				break;
			case 't':
				{
					pg_time_t	stamp_time = (pg_time_t) time(NULL);
					char		strfbuf[128];

					pg_strftime(strfbuf, sizeof(strfbuf),
								"%Y-%m-%d %H:%M:%S %Z",
								pg_localtime(&stamp_time, log_timezone));
					if (padding != 0)
						appendStringInfo(buf, "%*s", padding, strfbuf);
					else
						appendStringInfoString(buf, strfbuf);
				}
				break;
			case 'n':
				{
					char		strfbuf[128];

					if (!saved_timeval_set)
					{
						gettimeofday(&saved_timeval, NULL);
						saved_timeval_set = true;
					}

					snprintf(strfbuf, sizeof(strfbuf), "%ld.%03d",
							 (long) saved_timeval.tv_sec,
							 (int) (saved_timeval.tv_usec / 1000));

					if (padding != 0)
						appendStringInfo(buf, "%*s", padding, strfbuf);
					else
						appendStringInfoString(buf, strfbuf);
				}
				break;
			case 's':
				{
					char	   *start_time = get_formatted_start_time();

					if (padding != 0)
						appendStringInfo(buf, "%*s", padding, start_time);
					else
						appendStringInfoString(buf, start_time);
				}
				break;
			case 'i':
				if (MyProcPort)
				{
					const char *psdisp;
					int			displen;

					psdisp = get_ps_display(&displen);
					if (padding != 0)
						appendStringInfo(buf, "%*s", padding, psdisp);
					else
						appendBinaryStringInfo(buf, psdisp, displen);
				}
				else if (padding != 0)
					appendStringInfoSpaces(buf,
										   padding > 0 ? padding : -padding);
				break;
			case 'r':
				if (MyProcPort && MyProcPort->remote_host)
				{
					if (padding != 0)
					{
						if (MyProcPort->remote_port && MyProcPort->remote_port[0] != '\0')
						{
							/*
							 * 此选项略特殊，因为端口号
							 * 可能附加在末尾。这里需构建
							 * 一个含 remote_host 及可选
							 * remote_port（若设了）的串，
							 * 以便正确对齐该字符串。
							 */

							char	   *hostport;

							hostport = psprintf("%s(%s)", MyProcPort->remote_host, MyProcPort->remote_port);
							appendStringInfo(buf, "%*s", padding, hostport);
							pfree(hostport);
						}
						else
							appendStringInfo(buf, "%*s", padding, MyProcPort->remote_host);
					}
					else
					{
						/* 填充为 0，故无需临时缓冲区 */
						appendStringInfoString(buf, MyProcPort->remote_host);
						if (MyProcPort->remote_port &&
							MyProcPort->remote_port[0] != '\0')
							appendStringInfo(buf, "(%s)",
											 MyProcPort->remote_port);
					}
				}
				else if (padding != 0)
					appendStringInfoSpaces(buf,
										   padding > 0 ? padding : -padding);
				break;
			case 'h':
				if (MyProcPort && MyProcPort->remote_host)
				{
					if (padding != 0)
						appendStringInfo(buf, "%*s", padding, MyProcPort->remote_host);
					else
						appendStringInfoString(buf, MyProcPort->remote_host);
				}
				else if (padding != 0)
					appendStringInfoSpaces(buf,
										   padding > 0 ? padding : -padding);
				break;
			case 'q':
				/* postmaster 及其同类中见 %q 即停 */
				/* 后端中直接忽略 */
				if (MyProcPort == NULL)
					return;
				break;
			case 'v':
				/* 与 lockfuncs.c 保持 VXID 格式同步 */
				if (MyProc != NULL && MyProc->backendId != InvalidBackendId)
				{
					if (padding != 0)
					{
						char		strfbuf[128];

						snprintf(strfbuf, sizeof(strfbuf) - 1, "%d/%u",
								 MyProc->backendId, MyProc->lxid);
						appendStringInfo(buf, "%*s", padding, strfbuf);
					}
					else
						appendStringInfo(buf, "%d/%u", MyProc->backendId, MyProc->lxid);
				}
				else if (padding != 0)
					appendStringInfoSpaces(buf,
										   padding > 0 ? padding : -padding);
				break;
			case 'x':
				if (padding != 0)
					appendStringInfo(buf, "%*u", padding, GetTopTransactionIdIfAny());
				else
					appendStringInfo(buf, "%u", GetTopTransactionIdIfAny());
				break;
			case 'e':
				if (padding != 0)
					appendStringInfo(buf, "%*s", padding, unpack_sql_state(edata->sqlerrcode));
				else
					appendStringInfoString(buf, unpack_sql_state(edata->sqlerrcode));
				break;
			case 'Q':
				if (padding != 0)
					appendStringInfo(buf, "%*lld", padding,
									 (long long) pgstat_get_my_query_id());
				else
					appendStringInfo(buf, "%lld",
									 (long long) pgstat_get_my_query_id());
				break;
			default:
				/* 格式错误 - 忽略 */
				break;
		}
	}
}

/*
 * 解包 MAKE_SQLSTATE 码。注意返回的是静态缓冲区
 * 指针。
 */
char *
unpack_sql_state(int sql_state)
{
	static char buf[12];
	int			i;

	for (i = 0; i < 5; i++)
	{
		buf[i] = PGUNSIXBIT(sql_state);
		sql_state >>= 6;
	}

	buf[i] = '\0';
	return buf;
}


/*
 * 向服务器日志写错误报告
 */
static void
send_message_to_server_log(ErrorData *edata)
{
	StringInfoData buf;
	bool		fallback_to_stderr = false;

	initStringInfo(&buf);

	saved_timeval_set = false;
	formatted_log_time[0] = '\0';

	log_line_prefix(&buf, edata);
	appendStringInfo(&buf, "%s:  ", _(error_severity(edata->elevel)));

	if (Log_error_verbosity >= PGERROR_VERBOSE)
		appendStringInfo(&buf, "%s: ", unpack_sql_state(edata->sqlerrcode));

	if (edata->message)
		append_with_tabs(&buf, edata->message);
	else
		append_with_tabs(&buf, _("missing error text"));

	if (edata->cursorpos > 0)
		appendStringInfo(&buf, _(" at character %d"),
						 edata->cursorpos);
	else if (edata->internalpos > 0)
		appendStringInfo(&buf, _(" at character %d"),
						 edata->internalpos);

	appendStringInfoChar(&buf, '\n');

	if (Log_error_verbosity >= PGERROR_DEFAULT)
	{
		if (edata->detail_log)
		{
			log_line_prefix(&buf, edata);
			appendStringInfoString(&buf, _("DETAIL:  "));
			append_with_tabs(&buf, edata->detail_log);
			appendStringInfoChar(&buf, '\n');
		}
		else if (edata->detail)
		{
			log_line_prefix(&buf, edata);
			appendStringInfoString(&buf, _("DETAIL:  "));
			append_with_tabs(&buf, edata->detail);
			appendStringInfoChar(&buf, '\n');
		}
		if (edata->hint)
		{
			log_line_prefix(&buf, edata);
			appendStringInfoString(&buf, _("HINT:  "));
			append_with_tabs(&buf, edata->hint);
			appendStringInfoChar(&buf, '\n');
		}
		if (edata->internalquery)
		{
			log_line_prefix(&buf, edata);
			appendStringInfoString(&buf, _("QUERY:  "));
			append_with_tabs(&buf, edata->internalquery);
			appendStringInfoChar(&buf, '\n');
		}
		if (edata->context && !edata->hide_ctx)
		{
			log_line_prefix(&buf, edata);
			appendStringInfoString(&buf, _("CONTEXT:  "));
			append_with_tabs(&buf, edata->context);
			appendStringInfoChar(&buf, '\n');
		}
		if (Log_error_verbosity >= PGERROR_VERBOSE)
		{
			/* 假定 funcname/filename 无换行... */
			if (edata->funcname && edata->filename)
			{
				log_line_prefix(&buf, edata);
				appendStringInfo(&buf, _("LOCATION:  %s, %s:%d\n"),
								 edata->funcname, edata->filename,
								 edata->lineno);
			}
			else if (edata->filename)
			{
				log_line_prefix(&buf, edata);
				appendStringInfo(&buf, _("LOCATION:  %s:%d\n"),
								 edata->filename, edata->lineno);
			}
		}
		if (edata->backtrace)
		{
			log_line_prefix(&buf, edata);
			appendStringInfoString(&buf, _("BACKTRACE:  "));
			append_with_tabs(&buf, edata->backtrace);
			appendStringInfoChar(&buf, '\n');
		}
	}

	/*
	 * 如果用户想要记录产生此错误的查询，那就做。
	 */
	if (check_log_of_query(edata))
	{
		log_line_prefix(&buf, edata);
		appendStringInfoString(&buf, _("STATEMENT:  "));
		append_with_tabs(&buf, debug_query_string);
		appendStringInfoChar(&buf, '\n');
	}

#ifdef HAVE_SYSLOG
	/* 启用时写 syslog */
	if (Log_destination & LOG_DESTINATION_SYSLOG)
	{
		int			syslog_level;

		switch (edata->elevel)
		{
			case DEBUG5:
			case DEBUG4:
			case DEBUG3:
			case DEBUG2:
			case DEBUG1:
				syslog_level = LOG_DEBUG;
				break;
			case LOG:
			case LOG_SERVER_ONLY:
			case INFO:
				syslog_level = LOG_INFO;
				break;
			case NOTICE:
			case WARNING:
			case WARNING_CLIENT_ONLY:
				syslog_level = LOG_NOTICE;
				break;
			case ERROR:
				syslog_level = LOG_WARNING;
				break;
			case FATAL:
				syslog_level = LOG_ERR;
				break;
			case PANIC:
			default:
				syslog_level = LOG_CRIT;
				break;
		}

		write_syslog(syslog_level, buf.data);
	}
#endif							/* HAVE_SYSLOG */

#ifdef WIN32
	/* 启用时写事件日志 */
	if (Log_destination & LOG_DESTINATION_EVENTLOG)
	{
		write_eventlog(edata->elevel, buf.data, buf.len);
	}
#endif							/* WIN32 */

	/* 启用时写 csvlog */
	if (Log_destination & LOG_DESTINATION_CSVLOG)
	{
		/*
		 * 安全时（syslogger 不需管道）发送 CSV 数据。
		 * 若做不到，回退为写入一条
		 * stderr.
		 */
		if (redirection_done || MyBackendType == B_LOGGER)
			write_csvlog(edata);
		else
			fallback_to_stderr = true;
	}

	/* 启用时写 JSON 日志 */
	if (Log_destination & LOG_DESTINATION_JSONLOG)
	{
		/*
		 * 安全时（syslogger 不需管道）发送 JSON 数据。
		 * 若做不到，回退为写入一条
		 * stderr.
		 */
		if (redirection_done || MyBackendType == B_LOGGER)
		{
			write_jsonlog(edata);
		}
		else
			fallback_to_stderr = true;
	}

	/*
	 * 若启用，或因先前的限制而需要，
	 * 写 stderr
	 */
	if ((Log_destination & LOG_DESTINATION_STDERR) ||
		whereToSendOutput == DestDebug ||
		fallback_to_stderr)
	{
		/*
		 * 若已知 syslogger 应捕获 stderr 输出，
		 * 且我们自己不是 syslogger，则使用分块协议。
		 * 否则只做普通的 stderr 写入。
		 */
		if (redirection_done && MyBackendType != B_LOGGER)
			write_pipe_chunks(buf.data, buf.len, LOG_DESTINATION_STDERR);
#ifdef WIN32

		/*
		 * 在 win32 服务环境中没有可用的 stderr。捕获
		 * 发往那里的内容并改为写事件日志。
		 *
		 * 若 stderr 重定向在活动，上面写 stderr 没问题，
		 * 因为那实为通向 syslogger 的管道。
		 */
		else if (pgwin32_is_service())
			write_eventlog(edata->elevel, buf.data, buf.len);
#endif
		else
			write_console(buf.data, buf.len);
	}

	/* 若在 syslogger 进程中，尝试把消息直接写入文件 */
	if (MyBackendType == B_LOGGER)
		write_syslogger_file(buf.data, buf.len, LOG_DESTINATION_STDERR);

	/* 不再需要 stderr 格式化的消息 */
	pfree(buf.data);
}

/*
 * 用分块协议把数据发送给 syslogger
 *
 * 注意：当有多个后端写入 syslogger 管道时，每次写
 * 入都必须不可分割地进入管道，且不与其他进程的
 * 数据交错。幸运的是，POSIX 规范要求只要不超过
 * PIPE_BUF 字节，对管道的写入就是原子的。因此
 * 我们把长消息分成不超过该长度的块，每次 write()
 * 调用发送一个块。收集器进程知道如何重组
 * 这些块。
 *
 * 因原子写要求，这里 write() 只可能有两种结果：
 * -1 表示失败，或请求的字节数。对失败
 * 无能为力；重试可能是无限循环，而且我们也无法
 * 有意义地报告它。（也没有别处可发送它！）
 * 所以不妨干脆忽略 write() 结果。但在某些平台
 * 上忽略 write() 的结果会得到编译器警告，所以用把
 * rc 强转成 void 的手法让编译器
 * 闭嘴。
 */
void
write_pipe_chunks(char *data, int len, int dest)
{
	PipeProtoChunk p;
	int			fd = fileno(stderr);
	int			rc;

	Assert(len > 0);

	p.proto.nuls[0] = p.proto.nuls[1] = '\0';
	p.proto.pid = MyProcPid;
	p.proto.flags = 0;
	if (dest == LOG_DESTINATION_STDERR)
		p.proto.flags |= PIPE_PROTO_DEST_STDERR;
	else if (dest == LOG_DESTINATION_CSVLOG)
		p.proto.flags |= PIPE_PROTO_DEST_CSVLOG;
	else if (dest == LOG_DESTINATION_JSONLOG)
		p.proto.flags |= PIPE_PROTO_DEST_JSONLOG;

	/* 写出除末块外各块 */
	while (len > PIPE_MAX_PAYLOAD)
	{
		/* 尚无需设置 PIPE_PROTO_IS_LAST */
		p.proto.len = PIPE_MAX_PAYLOAD;
		memcpy(p.proto.data, data, PIPE_MAX_PAYLOAD);
		rc = write(fd, &p, PIPE_HEADER_SIZE + PIPE_MAX_PAYLOAD);
		(void) rc;
		data += PIPE_MAX_PAYLOAD;
		len -= PIPE_MAX_PAYLOAD;
	}

	/* 写最后一块 */
	p.proto.flags |= PIPE_PROTO_IS_LAST;
	p.proto.len = len;
	memcpy(p.proto.data, data, len);
	rc = write(fd, &p, PIPE_HEADER_SIZE + len);
	(void) rc;
}


/*
 * 把文本串追加到正在为客户端构建的错误报告中。
 *
 * 这通常同 pq_sendstring()，但若我们陷入
 * 错误递归困境就跳过编码转换，因为问题可能是
 * 编码转换子系统本身的失败。其他地方的代码应
 * 确保传入串在此情形下将是纯 7 位 ASCII，
 * 因而无需转换。（特别地，我们为此禁用了
 * 错误消息本地化
 * 以确保这一点。）
 */
static void
err_sendstring(StringInfo buf, const char *str)
{
	if (in_error_recursion_trouble())
		pq_send_ascii_string(buf, str);
	else
		pq_sendstring(buf, str);
}

/*
 * 错误报告送客户端
 */
static void
send_message_to_frontend(ErrorData *edata)
{
	StringInfoData msgbuf;

	/*
	 * 除此处外，我们已不支持 3.0 前的 FE/BE 协议。
	 * 如果客户端尝试用更旧的协议版本连接，那么以
	 * 客户端懂的形式发送"协议版本不支持"错误是
	 * 很好的做法。若协议尚未设置，即启动早期，
	 * 则假定是现代协议。
	 */
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3 || FrontendProtocol == 0)
	{
		/* 带独立字段的新式 */
		const char *sev;
		char		tbuf[12];

		/* 'N'（Notice）用于非致命情形，'E' 用于错误 */
		pq_beginmessage(&msgbuf, (edata->elevel < ERROR) ? 'N' : 'E');

		sev = error_severity(edata->elevel);
		pq_sendbyte(&msgbuf, PG_DIAG_SEVERITY);
		err_sendstring(&msgbuf, _(sev));
		pq_sendbyte(&msgbuf, PG_DIAG_SEVERITY_NONLOCALIZED);
		err_sendstring(&msgbuf, sev);

		pq_sendbyte(&msgbuf, PG_DIAG_SQLSTATE);
		err_sendstring(&msgbuf, unpack_sql_state(edata->sqlerrcode));

		/* 协议要求 M 字段，所以总要发送些内容 */
		pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_PRIMARY);
		if (edata->message)
			err_sendstring(&msgbuf, edata->message);
		else
			err_sendstring(&msgbuf, _("missing error text"));

		if (edata->detail)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_DETAIL);
			err_sendstring(&msgbuf, edata->detail);
		}

		/* 此处有意不使用 detail_log */

		if (edata->hint)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_HINT);
			err_sendstring(&msgbuf, edata->hint);
		}

		if (edata->context)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_CONTEXT);
			err_sendstring(&msgbuf, edata->context);
		}

		if (edata->schema_name)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_SCHEMA_NAME);
			err_sendstring(&msgbuf, edata->schema_name);
		}

		if (edata->table_name)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_TABLE_NAME);
			err_sendstring(&msgbuf, edata->table_name);
		}

		if (edata->column_name)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_COLUMN_NAME);
			err_sendstring(&msgbuf, edata->column_name);
		}

		if (edata->datatype_name)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_DATATYPE_NAME);
			err_sendstring(&msgbuf, edata->datatype_name);
		}

		if (edata->constraint_name)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_CONSTRAINT_NAME);
			err_sendstring(&msgbuf, edata->constraint_name);
		}

		if (edata->cursorpos > 0)
		{
			snprintf(tbuf, sizeof(tbuf), "%d", edata->cursorpos);
			pq_sendbyte(&msgbuf, PG_DIAG_STATEMENT_POSITION);
			err_sendstring(&msgbuf, tbuf);
		}

		if (edata->internalpos > 0)
		{
			snprintf(tbuf, sizeof(tbuf), "%d", edata->internalpos);
			pq_sendbyte(&msgbuf, PG_DIAG_INTERNAL_POSITION);
			err_sendstring(&msgbuf, tbuf);
		}

		if (edata->internalquery)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_INTERNAL_QUERY);
			err_sendstring(&msgbuf, edata->internalquery);
		}

		if (edata->filename)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_SOURCE_FILE);
			err_sendstring(&msgbuf, edata->filename);
		}

		if (edata->lineno > 0)
		{
			snprintf(tbuf, sizeof(tbuf), "%d", edata->lineno);
			pq_sendbyte(&msgbuf, PG_DIAG_SOURCE_LINE);
			err_sendstring(&msgbuf, tbuf);
		}

		if (edata->funcname)
		{
			pq_sendbyte(&msgbuf, PG_DIAG_SOURCE_FUNCTION);
			err_sendstring(&msgbuf, edata->funcname);
		}

		pq_sendbyte(&msgbuf, '\0'); /* 终止符 */

		pq_endmessage(&msgbuf);
	}
	else
	{
		/* 旧式 --- 生成一个向后兼容的消息 */
		StringInfoData buf;

		initStringInfo(&buf);

		appendStringInfo(&buf, "%s:  ", _(error_severity(edata->elevel)));

		if (edata->message)
			appendStringInfoString(&buf, edata->message);
		else
			appendStringInfoString(&buf, _("missing error text"));

		appendStringInfoChar(&buf, '\n');

		/* 'N'（Notice）用于非致命情形，'E' 用于错误 */
		pq_putmessage_v2((edata->elevel < ERROR) ? 'N' : 'E', buf.data, buf.len + 1);

		pfree(buf.data);
	}

	/*
	 * 此 flush 通常不必要，因为控制权返回主循环时
	 * postgres.c 会冲刷待发数据。但留它在此似乎最好，
	 * 这样，如果后端在回到主循环之前就死了，客户端还
	 * 能对发生的事情有些线索 ... 错误/通知消息反正
	 * 不该是性能关键路径，所以额外的 flush 不会
	 * 有多大影响 ...
	 */
	pq_flush();
}


/*
 * 格式化错误消息的支持例程。
 */


/*
 * error_severity --- elevel 对应的串
 *
 * 此处的字符串不做本地化，但我们把它们标记为可
 * 译，便于调用者对结果调 _()。
 */
const char *
error_severity(int elevel)
{
	const char *prefix;

	switch (elevel)
	{
		case DEBUG1:
		case DEBUG2:
		case DEBUG3:
		case DEBUG4:
		case DEBUG5:
			prefix = gettext_noop("DEBUG");
			break;
		case LOG:
		case LOG_SERVER_ONLY:
			prefix = gettext_noop("LOG");
			break;
		case INFO:
			prefix = gettext_noop("INFO");
			break;
		case NOTICE:
			prefix = gettext_noop("NOTICE");
			break;
		case WARNING:
		case WARNING_CLIENT_ONLY:
			prefix = gettext_noop("WARNING");
			break;
		case ERROR:
			prefix = gettext_noop("ERROR");
			break;
		case FATAL:
			prefix = gettext_noop("FATAL");
			break;
		case PANIC:
			prefix = gettext_noop("PANIC");
			break;
		default:
			prefix = "???";
			break;
	}

	return prefix;
}


/*
 *	append_with_tabs
 *
 *	把串追加到 StringInfo 缓冲区，并在每个换行符后
 *	插tab
 */
static void
append_with_tabs(StringInfo buf, const char *str)
{
	char		ch;

	while ((ch = *str++) != '\0')
	{
		appendStringInfoCharMacro(buf, ch);
		if (ch == '\n')
			appendStringInfoCharMacro(buf, '\t');
	}
}


/*
 * 把错误写到 stderr (或同等手段，
 * 不可用）。在 ereport/elog 能安全用之前
 * （内存上下文、GUC 等）
 */
void
write_stderr(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	vwrite_stderr(fmt, ap);
	va_end(ap);
}


/*
 * 把错误写到 stderr (或同等手段，
 * stderr 不可用)-va_list 版
 */
void
vwrite_stderr(const char *fmt, va_list ap)
{
#ifdef WIN32
	char		errbuf[2048];	/* 大小随意 */
#endif

	fmt = _(fmt);
#ifndef WIN32
	/* Unix 上只是 fprintf 到 stderr */
	vfprintf(stderr, fmt, ap);
	fflush(stderr);
#else
	vsnprintf(errbuf, sizeof(errbuf), fmt, ap);

	/*
	 * 在 Win32 上，若运行在控制台则打印到 stderr，
	 * 服务运行则写事件日志
	 */
	if (pgwin32_is_service())	/* 作为服务运行 */
	{
		write_eventlog(ERROR, errbuf, strlen(errbuf));
	}
	else
	{
		/* 非服务运行，写到 stderr */
		write_console(errbuf, strlen(errbuf));
		fflush(stderr);
	}
#endif
}


/*
 * 仅使用异步信号安全函数向 STDERR 写消息。这可以
 * 用于从信号处理器安全地发出消息。
 *
 * TODO: 很可能可以安全地做有限量的字符串插值（
 * 如 %s 和 %d），但目前尚不支持。
 */
void
write_stderr_signal_safe(const char *str)
{
	int			nwritten = 0;
	int			ntotal = strlen(str);

	while (nwritten < ntotal)
	{
		int			rc;

		rc = write(STDERR_FILENO, str + nwritten, ntotal - nwritten);

		/* 出错时干脆放弃。我们也没多少别的办法 */
		if (rc == -1)
			return;

		nwritten += rc;
	}
}


/*
 * 按 trace_recovery_messages 调整恢复相关消息的级别。
 *
 * 参数是消息的默认日志级别，如 DEBUG2。（仅
 * 适用于 DEBUGn 日志消息，否则它就是空操作。）
 * 如果级别 >= trace_recovery_messages，我们返回 LOG，
 * 使消息无条件记录（log_min_messages 多数
 * 设置而言）。否则，我们原样返回参数。
 * 之后消息将根据 log_min_messages 的设置来显示。
 *
 * 意图是在整个 9.0 生产发布期间保留它，以便我们能
 * 更容易地诊断现场的生产问题。不过它最终应被移除，
 * 因为它是一个丑陋且难以解释的
 * 权宜补丁。
 */
int
trace_recovery(int trace_level)
{
	if (trace_level < LOG &&
		trace_level >= trace_recovery_messages)
		return LOG;

	return trace_level;
}
