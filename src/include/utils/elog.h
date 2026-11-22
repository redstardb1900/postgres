/*-------------------------------------------------------------------------
 *
 * elog.h
 *	  POSTGRES error reporting/logging definitions.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/elog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ELOG_H
#define ELOG_H

#include <setjmp.h>

#include "lib/stringinfo.h"

/*  尚无法含 nodes.h，故前向声明结构体 Node  */
struct Node;


/*  错误级代码  */
#define DEBUG5		10			/*  调试消息，按细节递减的
								 * 类别。  */
#define DEBUG4		11
#define DEBUG3		12
#define DEBUG2		13
#define DEBUG1		14			/*  供 GUC debug_* 变量用  */
#define LOG			15			/*  服务器运行消息；默认仅发到
								 * 服务器日志。  */
#define LOG_SERVER_ONLY 16		/*  服务器报告同 LOG，但永不
								 * 发给客户.  */
#define COMMERROR	LOG_SERVER_ONLY /*  客户通信问题；服务器同
									 * LOG，但永不
									 * 发给客户.  */
#define INFO		17			/*  用户明确请求的消息(如
								 * VACUUM VERBOSE 输出)；总发给
								 * 客户端，不论 client_min_messages，
								 * 但默认不发服务器日志。  */
#define NOTICE		18			/*  查询操作的有助用户消息；
								 * 默认发给客户端、不发服务器
								 * 日志。  */
#define WARNING		19			/*  警告。NOTICE 用于预期消息，
								 * 如 SERIAL 隐式建序列。
								 * WARNING 用于意外消息。  */
#define PGWARNING	19			/*  须等于 WARNING；见下 NOTE.  */
#define WARNING_CLIENT_ONLY	20	/*  照常发给客户端的警告，但
								 *  永不发服日志。  */
#define ERROR		21			/*  用户错误 - 中止事务；返回到
								 * 已知态  */
#define PGERROR		21			/*  须等于 ERROR；见下 NOTE.  */
#define FATAL		22			/*  致命错 - 中止进程  */
#define PANIC		23			/*  连带搞垮其他后端  */

/*
 * 注：备用名 PGWARNING 与 PGERROR 便于处理把 WARNING
 * 和/或 ERROR 另作定义的第三方头。例如包含此类头后
 * 可将 ERROR 重定义为 PGERROR。
 * 见上。
 */


/*  紧凑表示 SQLSTATE 串的宏  */
#define PGSIXBIT(ch)	(((ch) - '0') & 0x3F)
#define PGUNSIXBIT(val) (((val) & 0x3F) + '0')

#define MAKE_SQLSTATE(ch1,ch2,ch3,ch4,ch5)	\
	(PGSIXBIT(ch1) + (PGSIXBIT(ch2) << 6) + (PGSIXBIT(ch3) << 12) + \
	 (PGSIXBIT(ch4) << 18) + (PGSIXBIT(ch5) << 24))

/*  这些宏依赖 '0' 在 PGSIXBIT 中变为零  */
#define ERRCODE_TO_CATEGORY(ec)  ((ec) & ((1 << 12) - 1))
#define ERRCODE_IS_CATEGORY(ec)  (((ec) & ~((1 << 12) - 1)) == 0)

/*  错误的 SQLSTATE 码在单独文件中定义  */
#include "utils/errcodes.h"

/*
 * Provide a way to prevent "errno" from being accidentally used inside an
 * elog() or ereport() invocation.  Since we know that some operating systems
 * define errno as something involving a function call, we'll put a local
 * variable of the same name as that function in the local scope to force a
 * compile error.  On platforms that don't define errno in that way, nothing
 * happens, so we get no warning ... but we can live with that as long as it
 * happens on some popular platforms.
 */
#if defined(errno) && defined(__linux__)
#define pg_prevent_errno_in_scope() int __errno_location pg_attribute_unused()
#elif defined(errno) && (defined(__darwin__) || defined(__FreeBSD__))
#define pg_prevent_errno_in_scope() int __error pg_attribute_unused()
#else
#define pg_prevent_errno_in_scope()
#endif


/*----------
 * New-style error reporting API: to be used in this way:
 *		ereport(ERROR,
 *				errcode(ERRCODE_UNDEFINED_CURSOR),
 *				errmsg("portal \"%s\" not found", stmt->portalname),
 *				... other errxxx() fields as needed ...);
 *
 * The error level is required, and so is a primary error message (errmsg
 * or errmsg_internal).  All else is optional.  errcode() defaults to
 * ERRCODE_INTERNAL_ERROR if elevel is ERROR or more, ERRCODE_WARNING
 * if elevel is WARNING, or ERRCODE_SUCCESSFUL_COMPLETION if elevel is
 * NOTICE or below.
 *
 * Before Postgres v12, extra parentheses were required around the
 * list of auxiliary function calls; that's now optional.
 *
 * ereport_domain() allows a message domain to be specified, for modules that
 * wish to use a different message catalog from the backend's.  To avoid having
 * one copy of the default text domain per .o file, we define it as NULL here
 * and have errstart insert the default text domain.  Modules can either use
 * ereport_domain() directly, or preferably they can override the TEXTDOMAIN
 * macro.
 *
 * When __builtin_constant_p is available and elevel >= ERROR we make a call
 * to errstart_cold() instead of errstart().  This version of the function is
 * marked with pg_attribute_cold which will coax supporting compilers into
 * generating code which is more optimized towards non-ERROR cases.  Because
 * we use __builtin_constant_p() in the condition, when elevel is not a
 * compile-time constant, or if it is, but it's < ERROR, the compiler has no
 * need to generate any code for this branch.  It can simply call errstart()
 * unconditionally.
 *
 * If elevel >= ERROR, the call will not return; we try to inform the compiler
 * of that via pg_unreachable().  However, no useful optimization effect is
 * obtained unless the compiler sees elevel as a compile-time constant, else
 * we're just adding code bloat.  So, if __builtin_constant_p is available,
 * use that to cause the second if() to vanish completely for non-constant
 * cases.  We avoid using a local variable because it's not necessary and
 * prevents gcc from making the unreachability deduction at optlevel -O0.
 *----------
 */
#ifdef HAVE__BUILTIN_CONSTANT_P
#define ereport_domain(elevel, domain, ...)	\
	do { \
		pg_prevent_errno_in_scope(); \
		if (__builtin_constant_p(elevel) && (elevel) >= ERROR ? \
			errstart_cold(elevel, domain) : \
			errstart(elevel, domain)) \
			__VA_ARGS__, errfinish(__FILE__, __LINE__, __func__); \
		if (__builtin_constant_p(elevel) && (elevel) >= ERROR) \
			pg_unreachable(); \
	} while(0)
#else							/* !HAVE__BUILTIN_CONSTANT_P */
#define ereport_domain(elevel, domain, ...)	\
	do { \
		const int elevel_ = (elevel); \
		pg_prevent_errno_in_scope(); \
		if (errstart(elevel_, domain)) \
			__VA_ARGS__, errfinish(__FILE__, __LINE__, __func__); \
		if (elevel_ >= ERROR) \
			pg_unreachable(); \
	} while(0)
#endif							/* HAVE__BUILTIN_CONSTANT_P */

#define ereport(elevel, ...)	\
	ereport_domain(elevel, TEXTDOMAIN, __VA_ARGS__)

#define TEXTDOMAIN NULL

extern bool message_level_is_interesting(int elevel);

extern bool errstart(int elevel, const char *domain);
extern pg_attribute_cold bool errstart_cold(int elevel, const char *domain);
extern void errfinish(const char *filename, int lineno, const char *funcname);

extern int	errcode(int sqlerrcode);

extern int	errcode_for_file_access(void);
extern int	errcode_for_socket_access(void);

extern int	errmsg(const char *fmt,...) pg_attribute_printf(1, 2);
extern int	errmsg_internal(const char *fmt,...) pg_attribute_printf(1, 2);

extern int	errmsg_plural(const char *fmt_singular, const char *fmt_plural,
						  unsigned long n,...) pg_attribute_printf(1, 4) pg_attribute_printf(2, 4);

extern int	errdetail(const char *fmt,...) pg_attribute_printf(1, 2);
extern int	errdetail_internal(const char *fmt,...) pg_attribute_printf(1, 2);

extern int	errdetail_log(const char *fmt,...) pg_attribute_printf(1, 2);

extern int	errdetail_log_plural(const char *fmt_singular,
								 const char *fmt_plural,
								 unsigned long n,...) pg_attribute_printf(1, 4) pg_attribute_printf(2, 4);

extern int	errdetail_plural(const char *fmt_singular, const char *fmt_plural,
							 unsigned long n,...) pg_attribute_printf(1, 4) pg_attribute_printf(2, 4);

extern int	errhint(const char *fmt,...) pg_attribute_printf(1, 2);

extern int	errhint_plural(const char *fmt_singular, const char *fmt_plural,
						   unsigned long n,...) pg_attribute_printf(1, 4) pg_attribute_printf(2, 4);

/*
 * errcontext() is typically called in error context callback functions, not
 * within an ereport() invocation. The callback function can be in a different
 * module than the ereport() call, so the message domain passed in errstart()
 * is not usually the correct domain for translating the context message.
 * set_errcontext_domain() first sets the domain to be used, and
 * errcontext_msg() passes the actual message.
 */
#define errcontext	set_errcontext_domain(TEXTDOMAIN),	errcontext_msg

extern int	set_errcontext_domain(const char *domain);

extern int	errcontext_msg(const char *fmt,...) pg_attribute_printf(1, 2);

extern int	errhidestmt(bool hide_stmt);
extern int	errhidecontext(bool hide_ctx);

extern int	errbacktrace(void);

extern int	errposition(int cursorpos);

extern int	internalerrposition(int cursorpos);
extern int	internalerrquery(const char *query);

extern int	err_generic_string(int field, const char *str);

extern int	geterrcode(void);
extern int	geterrlevel(void);
extern int	geterrposition(void);
extern int	getinternalerrposition(void);


/*----------
 * Old-style error reporting API: to be used in this way:
 *		elog(ERROR, "portal \"%s\" not found", stmt->portalname);
 *----------
 */
#define elog(elevel, ...)  \
	ereport(elevel, errmsg_internal(__VA_ARGS__))


/*----------
 * Support for reporting "soft" errors that don't require a full transaction
 * abort to clean up.  This is to be used in this way:
 *		errsave(context,
 *				errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
 *				errmsg("invalid input syntax for type %s: \"%s\"",
 *					   "boolean", in_str),
 *				... other errxxx() fields as needed ...);
 *
 * "context" is a node pointer or NULL, and the remaining auxiliary calls
 * provide the same error details as in ereport().  If context is not a
 * pointer to an ErrorSaveContext node, then errsave(context, ...)
 * behaves identically to ereport(ERROR, ...).  If context is a pointer
 * to an ErrorSaveContext node, then the information provided by the
 * auxiliary calls is stored in the context node and control returns
 * normally.  The caller of errsave() must then do any required cleanup
 * and return control back to its caller.  That caller must check the
 * ErrorSaveContext node to see whether an error occurred before
 * it can trust the function's result to be meaningful.
 *
 * errsave_domain() allows a message domain to be specified; it is
 * precisely analogous to ereport_domain().
 *----------
 */
#define errsave_domain(context, domain, ...)	\
	do { \
		struct Node *context_ = (context); \
		pg_prevent_errno_in_scope(); \
		if (errsave_start(context_, domain)) \
			__VA_ARGS__, errsave_finish(context_, __FILE__, __LINE__, __func__); \
	} while(0)

#define errsave(context, ...)	\
	errsave_domain(context, TEXTDOMAIN, __VA_ARGS__)

/*
 * "ereturn(context, dummy_value, ...);" is exactly the same as
 * "errsave(context, ...); return dummy_value;".  This saves a bit
 * of typing in the common case where a function has no cleanup
 * actions to take after reporting a soft error.  "dummy_value"
 * can be empty if the function returns void.
 */
#define ereturn_domain(context, dummy_value, domain, ...)	\
	do { \
		errsave_domain(context, domain, __VA_ARGS__); \
		return dummy_value; \
	} while(0)

#define ereturn(context, dummy_value, ...)	\
	ereturn_domain(context, dummy_value, TEXTDOMAIN, __VA_ARGS__)

extern bool errsave_start(struct Node *context, const char *domain);
extern void errsave_finish(struct Node *context,
						   const char *filename, int lineno,
						   const char *funcname);


/*  支持在 ereport() 调用外单独构造错误串  */

extern void pre_format_elog_string(int errnumber, const char *domain);
extern char *format_elog_string(const char *fmt,...) pg_attribute_printf(1, 2);


/*  支持把上下文信息附到错误报告  */

typedef struct ErrorContextCallback
{
	struct ErrorContextCallback *previous;
	void		(*callback) (void *arg);
	void	   *arg;
} ErrorContextCallback;

extern PGDLLIMPORT ErrorContextCallback *error_context_stack;


/*----------
 * API for catching ereport(ERROR) exits.  Use these macros like so:
 *
 *		PG_TRY();
 *		{
 *			... code that might throw ereport(ERROR) ...
 *		}
 *		PG_CATCH();
 *		{
 *			... error recovery code ...
 *		}
 *		PG_END_TRY();
 *
 * (The braces are not actually necessary, but are recommended so that
 * pgindent will indent the construct nicely.)  The error recovery code
 * can either do PG_RE_THROW to propagate the error outwards, or do a
 * (sub)transaction abort. Failure to do so may leave the system in an
 * inconsistent state for further processing.
 *
 * For the common case that the error recovery code and the cleanup in the
 * normal code path are identical, the following can be used instead:
 *
 *		PG_TRY();
 *		{
 *			... code that might throw ereport(ERROR) ...
 *		}
 *		PG_FINALLY();
 *		{
 *			... cleanup code ...
 *		}
 *      PG_END_TRY();
 *
 * The cleanup code will be run in either case, and any error will be rethrown
 * afterwards.
 *
 * You cannot use both PG_CATCH() and PG_FINALLY() in the same
 * PG_TRY()/PG_END_TRY() block.
 *
 * Note: while the system will correctly propagate any new ereport(ERROR)
 * occurring in the recovery section, there is a small limit on the number
 * of levels this will work for.  It's best to keep the error recovery
 * section simple enough that it can't generate any new errors, at least
 * not before popping the error stack.
 *
 * Note: an ereport(FATAL) will not be caught by this construct; control will
 * exit straight through proc_exit().  Therefore, do NOT put any cleanup
 * of non-process-local resources into the error recovery section, at least
 * not without taking thought for what will happen during ereport(FATAL).
 * The PG_ENSURE_ERROR_CLEANUP macros provided by storage/ipc.h may be
 * helpful in such cases.
 *
 * Note: if a local variable of the function containing PG_TRY is modified
 * in the PG_TRY section and used in the PG_CATCH section, that variable
 * must be declared "volatile" for POSIX compliance.  This is not mere
 * pedantry; we have seen bugs from compilers improperly optimizing code
 * away when such a variable was not marked.  Beware that gcc's -Wclobbered
 * warnings are just about entirely useless for catching such oversights.
 *
 * Each of these macros accepts an optional argument which can be specified
 * to apply a suffix to the variables declared within the macros.  This suffix
 * can be used to avoid the compiler emitting warnings about shadowed
 * variables when compiling with -Wshadow in situations where nested PG_TRY()
 * statements are required.  The optional suffix may contain any character
 * that's allowed in a variable name.  The suffix, if specified, must be the
 * same within each component macro of the given PG_TRY() statement.
 *----------
 */
#define PG_TRY(...)  \
	do { \
		sigjmp_buf *_save_exception_stack##__VA_ARGS__ = PG_exception_stack; \
		ErrorContextCallback *_save_context_stack##__VA_ARGS__ = error_context_stack; \
		sigjmp_buf _local_sigjmp_buf##__VA_ARGS__; \
		bool _do_rethrow##__VA_ARGS__ = false; \
		if (sigsetjmp(_local_sigjmp_buf##__VA_ARGS__, 0) == 0) \
		{ \
			PG_exception_stack = &_local_sigjmp_buf##__VA_ARGS__

#define PG_CATCH(...)	\
		} \
		else \
		{ \
			PG_exception_stack = _save_exception_stack##__VA_ARGS__; \
			error_context_stack = _save_context_stack##__VA_ARGS__

#define PG_FINALLY(...) \
		} \
		else \
			_do_rethrow##__VA_ARGS__ = true; \
		{ \
			PG_exception_stack = _save_exception_stack##__VA_ARGS__; \
			error_context_stack = _save_context_stack##__VA_ARGS__

#define PG_END_TRY(...)  \
		} \
		if (_do_rethrow##__VA_ARGS__) \
				PG_RE_THROW(); \
		PG_exception_stack = _save_exception_stack##__VA_ARGS__; \
		error_context_stack = _save_context_stack##__VA_ARGS__; \
	} while (0)

/*
 * Some compilers understand pg_attribute_noreturn(); for other compilers,
 * insert pg_unreachable() so that the compiler gets the point.
 */
#ifdef HAVE_PG_ATTRIBUTE_NORETURN
#define PG_RE_THROW()  \
	pg_re_throw()
#else
#define PG_RE_THROW()  \
	(pg_re_throw(), pg_unreachable())
#endif

extern PGDLLIMPORT sigjmp_buf *PG_exception_stack;


/*  错误处理器可能想用的东西  */

/*
 * ErrorData holds the data accumulated during any one ereport() cycle.
 * Any non-NULL pointers must point to palloc'd data.
 * (The const pointers are an exception; we assume they point at non-freeable
 * constant strings.)
 */
typedef struct ErrorData
{
	int			elevel;			/*  错误级  */
	bool		output_to_server;	/*  报服务器日志？  */
	bool		output_to_client;	/*  报客户端？  */
	bool		hide_stmt;		/*  为真则不包含 STATEMENT:  */
	bool		hide_ctx;		/*  为真则不包含 CONTEXT:  */
	const char *filename;		/*  ereport 处 __FILE__  */
	int			lineno;			/*  ereport 处 __LINE__  */
	const char *funcname;		/*  ereport 处 __func__  */
	const char *domain;			/*  消息域  */
	const char *context_domain; /*  上下文消息的消息域  */
	int			sqlerrcode;		/* encoded ERRSTATE */
	char	   *message;		/*  主错误消息(已翻译)  */
	char	   *detail;			/*  详细错误消息  */
	char	   *detail_log;		/*  仅服务器日志的详细消息  */
	char	   *hint;			/*  提示  */
	char	   *context;		/*  上下文  */
	char	   *backtrace;		/*  回溯  */
	const char *message_id;		/*  主消息的 id(原始串)  */
	char	   *schema_name;	/*  模式名  */
	char	   *table_name;		/*  表名  */
	char	   *column_name;	/*  列名  */
	char	   *datatype_name;	/*  类型名  */
	char	   *constraint_name;	/*  约束名  */
	int			cursorpos;		/*  查询串中的游标下标  */
	int			internalpos;	/*  internalquery 中游标下标  */
	char	   *internalquery;	/*  内部生成查询的文本  */
	int			saved_errno;	/* errno at entry */

	/*  容纳相关非常量串的上下文  */
	struct MemoryContextData *assoc_context;
} ErrorData;

extern void EmitErrorReport(void);
extern ErrorData *CopyErrorData(void);
extern void FreeErrorData(ErrorData *edata);
extern void FlushErrorState(void);
extern void ReThrowError(ErrorData *edata) pg_attribute_noreturn();
extern void ThrowErrorData(ErrorData *edata);
extern void pg_re_throw(void) pg_attribute_noreturn();

extern char *GetErrorContextStack(void);

/*  消息发到服务器日志前的拦截钩子  */
typedef void (*emit_log_hook_type) (ErrorData *edata);
extern PGDLLIMPORT emit_log_hook_type emit_log_hook;


/*  GUC 可配置参数  */

typedef enum
{
	PGERROR_TERSE,				/*  单行错误消息  */
	PGERROR_DEFAULT,			/*  推荐风格  */
	PGERROR_VERBOSE				/*  全部事实  */
}			PGErrorVerbosity;

extern PGDLLIMPORT int Log_error_verbosity;
extern PGDLLIMPORT char *Log_line_prefix;
extern PGDLLIMPORT int Log_destination;
extern PGDLLIMPORT char *Log_destination_string;
extern PGDLLIMPORT bool syslog_sequence_numbers;
extern PGDLLIMPORT bool syslog_split_messages;

/*  日志目的地图  */
#define LOG_DESTINATION_STDERR	 1
#define LOG_DESTINATION_SYSLOG	 2
#define LOG_DESTINATION_EVENTLOG 4
#define LOG_DESTINATION_CSVLOG	 8
#define LOG_DESTINATION_JSONLOG	16

/*  其他导出函数  */
extern void log_status_format(StringInfo buf, const char *format,
							  ErrorData *edata);
extern void DebugFileOpen(void);
extern char *unpack_sql_state(int sql_state);
extern bool in_error_recursion_trouble(void);

/*  跨目的地共享的公共函数  */
extern void reset_formatted_start_time(void);
extern char *get_formatted_start_time(void);
extern char *get_formatted_log_time(void);
extern const char *get_backend_type_for_log(void);
extern bool check_log_of_query(ErrorData *edata);
extern const char *error_severity(int elevel);
extern void write_pipe_chunks(char *data, int len, int dest);

/*  目的地专用函数  */
extern void write_csvlog(ErrorData *edata);
extern void write_jsonlog(ErrorData *edata);

/*
 * 把错误写到 stderr(或不可用时等效手段)。
 * 在 ereport/elog 可安全使用前使用
 * (内存上下文、GUC 加载等)
 */
extern void write_stderr(const char *fmt,...) pg_attribute_printf(1, 2);
extern void vwrite_stderr(const char *fmt, va_list ap) pg_attribute_printf(1, 0);

/*
 * 仅用异步信号安全函数向 STDERR 写消息。可用于从
 * 信号处理程序安全地发出消息。
 */
extern void write_stderr_signal_safe(const char *fmt);

#endif							/* ELOG_H */
