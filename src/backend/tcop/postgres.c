/*-------------------------------------------------------------------------
 *
 * postgres.c
 *	  POSTGRES C 后端接口     
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/postgres.c
 *
 * NOTES
 *	  这是 postgres 后端的 "main" 模块，           
 *	  因而也是 "traffic cop" 的主模块。 
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>

#ifdef USE_VALGRIND
#include <valgrind/valgrind.h>
#endif

#include "access/parallel.h"
#include "access/printtup.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/async.h"
#include "commands/prepare.h"
#include "common/pg_prng.h"
#include "jit/jit.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "mb/stringinfo_mb.h"
#include "miscadmin.h"
#include "nodes/print.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "pg_getopt.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "replication/slot.h"
#include "replication/walsender.h"
#include "rewrite/rewriteHandler.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/sinval.h"
#include "tcop/fastpath.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/guc_hooks.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

/* ----------------
 *		全局变量    
 * ----------------
 */
const char *debug_query_string; /* 客户端提供的查询串  */

/* 注：whereToSendOutput 为 bootstrap/standalone 场景而初始化      */
CommandDest whereToSendOutput = DestDebug;

/* 记录会话结束的标志     */
bool		Log_disconnections = false;

int			log_statement = LOGSTMT_NONE;

/* GUC 变量：最大栈深度（以千字节计）            */
int			max_stack_depth = 100;

/* 等待 N 秒以便调试器附加              */
int			PostAuthDelay = 0;

/* 检查客户端是否仍然连接的时间间隔。     */
int			client_connection_check_interval = 0;

/* 用于限制使用的非系统关系类型标志    */
int			restrict_nonsystem_relation_kind;

/* ----------------
 *		私有 typedef 等  
 * ----------------
 */

/* bind_param_error_callback 的参数类型      */
typedef struct BindParamCbData
{
	const char *portalName;
	int			paramno;		/* 从零起的参数编号，初始为 -1  */
	const char *paramval;		/* 文本输入串（若有）        */
} BindParamCbData;

/* ----------------
 *		私有变量     
 * ----------------
 */

/* max_stack_depth 换算为字节，加快检查速度     */
static long max_stack_depth_bytes = 100 * 1024L;

/*
 * 栈基指针 -- 由 PostmasterMain 初始化，子进程继承其值
 * 另见 InitPostmasterChild。                   
 */
static char *stack_base_ptr = NULL;

/*
 * 跟踪我们是否已启动事务的标志。               
 * 对于扩展查询协议，该标志需跨多条消息保留。       
 */
static bool xact_started = false;

/*
 * 指示我们正在执行外层循环的客户端读取，            
 * 以区别于 COPY FROM STDIN 等命令内部                      
 * 可能发生的随机读取。
 */
static bool DoingCommandRead = false;

/*
 * 为扩展查询协议的消息实现出错后跳过直至             
 * Sync 行为的标志。     
 */
static bool doing_extended_query_message = false;
static bool ignore_till_sync = false;

/*
 * 若存在未命名的预备语句，则存储于此。    
 * 我们将其与 commands/prepare.c 所维护的                  
 * 哈希表分开，以降低短查询的开销。    
 */
static CachedPlanSource *unnamed_stmt_psrc = NULL;

/* 各类命令行开关          */
static const char *userDoption = NULL;	/* -D 开关 */
static bool EchoQuery = false;	/* -E 开关 */
static bool UseSemiNewlineNewline = false;	/* -j 开关 */

/* 我们是否因与恢复冲突而被取消，及其原因           */
static bool RecoveryConflictPending = false;
static bool RecoveryConflictRetryable = true;
static ProcSignalReason RecoveryConflictReason;

/* 传给 SendRowDescriptionMessage() 的复用缓冲区*/
static MemoryContext row_description_context = NULL;
static StringInfoData row_description_buf;

/* ----------------------------------------------------------------
 *		仅在本文件内使用的例程声明  
 * ----------------------------------------------------------------
 */
static int	InteractiveBackend(StringInfo inBuf);
static int	interactive_getc(void);
static int	SocketBackend(StringInfo inBuf);
static int	ReadCommand(StringInfo inBuf);
static void forbidden_in_wal_sender(char firstchar);
static bool check_log_statement(List *stmt_list);
static int	errdetail_execute(List *raw_parsetree_list);
static int	errdetail_params(ParamListInfo params);
static int	errdetail_abort(void);
static int	errdetail_recovery_conflict(void);
static void bind_param_error_callback(void *arg);
static void start_xact_command(void);
static void finish_xact_command(void);
static bool IsTransactionExitStmt(Node *parsetree);
static bool IsTransactionExitStmtList(List *pstmts);
static bool IsTransactionStmtList(List *pstmts);
static void drop_unnamed_stmt(void);
static void log_disconnections(int code, Datum arg);
static void enable_statement_timeout(void);
static void disable_statement_timeout(void);


/* ----------------------------------------------------------------
 *		valgrind 调试基础设施          
 * ----------------------------------------------------------------
 */
#ifdef USE_VALGRIND
/* 该变量应在主循环顶部设置。                  */
static unsigned int old_valgrind_error_count;

/*
 * 若自 old_valgrind_error_count 更新以来 Valgrind 检测到任何      
 * 错误，则将当前查询报告为起因。应在消息处理         
 * 结束时调用。    
 */
static void
valgrind_report_error_query(const char *query)
{
	unsigned int valgrind_error_count = VALGRIND_COUNT_ERRORS;

	if (unlikely(valgrind_error_count != old_valgrind_error_count) &&
		query != NULL)
		VALGRIND_PRINTF("Valgrind detected %u error(s) during execution of \"%s\"\n",
						valgrind_error_count - old_valgrind_error_count,
						query);
}

#else							/* !USE_VALGRIND */
#define valgrind_report_error_query(query) ((void) 0)
#endif							/* USE_VALGRIND */


/* ----------------------------------------------------------------
 *		获取用户输入的例程  
 * ----------------------------------------------------------------
 */

/* ----------------
 *	针对用户交互连接而调用 InteractiveBackend()         
 *
 *	用户输入的字符串放入其参数 inBuf 中，            
 *	如同收到一条 Q 消息般处理。  
 *
 *	遇到文件结束输入则返回 EOF；此时应关闭。      
 * ----------------
 */

static int
InteractiveBackend(StringInfo inBuf)
{
	int			c;				/* 从 getc() 读到的字符 */

	/*
	 * 显示提示符并从用户获取输入        
	 */
	printf("backend> ");
	fflush(stdout);

	resetStringInfo(inBuf);

	/*
	 * 读取字符，直到遇到 EOF 或相应分隔符为止。    
	 */
	while ((c = interactive_getc()) != EOF)
	{
		if (c == '\n')
		{
			if (UseSemiNewlineNewline)
			{
				/*
				 * 在 -j 模式下，分号后跟两个换行即结束   
				 * 命令；否则将换行当作普通字符。         
				 */
				if (inBuf->len > 1 &&
					inBuf->data[inBuf->len - 1] == '\n' &&
					inBuf->data[inBuf->len - 2] == ';')
				{
					/* 不妨顺手丢弃第二个换行符  */
					break;
				}
			}
			else
			{
				/*
				 * 在普通模式下，换行结束命令，除非其前有 
				 * 反斜杠 
				 */
				if (inBuf->len > 0 &&
					inBuf->data[inBuf->len - 1] == '\\')
				{
					/* 从 inBuf 丢弃反斜杠    */
					inBuf->data[--inBuf->len] = '\0';
					/* 也丢弃换行符  */
					continue;
				}
				else
				{
					/* 保留换行符，但结束该命令。         */
					appendStringInfoChar(inBuf, '\n');
					break;
				}
			}
		}

		/* 非换行，或换行被视为普通字符           */
		appendStringInfoChar(inBuf, (char) c);
	}

	/* EOF 信号前无输入即意味着应退出。 */
	if (c == EOF && inBuf->len == 0)
		return EOF;

	/*
	 * 否则为用户查询，故予以处理。   
	 */

	/* 添加 '\0' 使其与消息情形一致。         */
	appendStringInfoChar(inBuf, (char) '\0');

	/*
	 * 若给定查询回显标志，则打印该查询.. 
	 */
	if (EchoQuery)
		printf("statement: %s\n", inBuf->data);
	fflush(stdout);

	return 'Q';
}

/*
 * interactive_getc -- 从 stdin 收集一个字符    
 *
 * 尽管我们并非从 "client" 进程读取，我们仍希望           
 * 响应信号，尤其是 SIGTERM/SIGQUIT。      
 */
static int
interactive_getc(void)
{
	int			c;

	/*
	 * 读取期间这不会处理追赶中断或通知。            
	 * 不过这些对独立后端而言本就无关紧要。为正确     
	 * 处理 SIGTERM，die() 中有一个 hack 会在这一          
	 * 阶段直接处理中断...                   
	 */
	CHECK_FOR_INTERRUPTS();

	c = getc(stdin);

	ProcessClientReadInterrupt(false);

	return c;
}

/* ----------------
 *	前端-后端连接时调用 SocketBackend()               
 *
 *	返回消息类型码，并将消息体数据装入 inBuf。          
 *
 *	连接丢失时返回 EOF。              
 * ----------------
 */
static int
SocketBackend(StringInfo inBuf)
{
	int			qtype;
	int			maxmsglen;

	/*
	 * 从前端获取消息类型码。       
	 */
	HOLD_CANCEL_INTERRUPTS();
	pq_startmsgread();
	qtype = pq_getbyte();

	if (qtype == EOF)			/* 前端已断开连接 */
	{
		if (IsTransactionState())
			ereport(COMMERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("unexpected EOF on client connection with an open transaction")));
		else
		{
			/*
			 * 此刻无法向客户端发送 DEBUG 日志消息。由于  
			 * 我们马上断开，无需恢复 whereToSendOutput。  
			 *                   
			 */
			whereToSendOutput = DestNone;
			ereport(DEBUG1,
					(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
					 errmsg_internal("unexpected EOF on client connection")));
		}
		return qtype;
	}

	/*
	 * 在尝试读取消息体之前先校验消息类型码；如果       
	 * 已失去同步，那么报告 "command unknown" 比因                
	 * 使用垃圾数据作长度字而耗尽内存要好。我们还能    
	 * 按消息类型为长度字选取合理的上限。（该上限本    
	 * 可以选得更细，但是否值得费心并不清楚。） 
	 *
	 * 这也让我们能尽早设置 doing_extended_query_message            
	 * 标志。           
	 */
	switch (qtype)
	{
		case 'Q':				/* 简单查询 */
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			break;

		case 'F':				/* fastpath 函数调用  */
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			break;

		case 'X':				/* terminate */
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			ignore_till_sync = false;
			break;

		case 'B':				/* bind */
		case 'P':				/* parse */
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			doing_extended_query_message = true;
			break;

		case 'C':				/* close */
		case 'D':				/* describe */
		case 'E':				/* execute */
		case 'H':				/* flush */
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			doing_extended_query_message = true;
			break;

		case 'S':				/* sync */
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			/* 停止活跃的 skip-till-Sync */
			ignore_till_sync = false;
			/* 标记为非扩展态，使新错误不会开始跳过。 */
			doing_extended_query_message = false;
			break;

		case 'd':				/* copy data */
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			break;

		case 'c':				/* copy done */
		case 'f':				/* copy fail */
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			break;

		default:

			/*
			 * 否则即从前端收到了垃圾数据。我们将其视为 
			 * 致命错误：很可能已丢失消息边界同步，        
			 * 并无良好的恢复途径。 
			 */
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid frontend message type %d", qtype)));
			maxmsglen = 0;		/* 使编译器安静  */
			break;
	}

	/*
	 * 在协议版本 3 中，所有前端消息在类型码之后        
	 * 都紧跟一个长度字；我们可以不依赖类型而读取       
	 * 内容。
	 */
	if (pq_getmessage(inBuf, maxmsglen))
		return EOF;				/* 相应消息已记录完毕     */
	RESUME_CANCEL_INTERRUPTS();

	return qtype;
}

/* ----------------
 *		ReadCommand 从前端或标准输入读取命令，    
 *		将其放入 inBuf，并返回                     
 *		消息类型码（消息首字节）。       
 *		文件结束则返回 EOF。   
 * ----------------
 */
static int
ReadCommand(StringInfo inBuf)
{
	int			result;

	if (whereToSendOutput == DestRemote)
		result = SocketBackend(inBuf);
	else
		result = InteractiveBackend(inBuf);
	return result;
}

/*
 * ProcessClientReadInterrupt() - 处理客户端读取特有的中断       
 *
 * 它在底层读取的前后即刻被调用。        
 * 'blocked' 为真表示无数据可读且打算重试，                 
 * 为假表示将读或已读完。      
 *
 * 勿改动 errno！  
 */
void
ProcessClientReadInterrupt(bool blocked)
{
	int			save_errno = errno;

	if (DoingCommandRead)
	{
		/* 检查在读取之前/期间到达的通用中断              */
		CHECK_FOR_INTERRUPTS();

		/* 处理 sinval 追赶中断（若有）    */
		if (catchupInterruptPending)
			ProcessCatchupInterrupt();

		/* 处理 notify 中断（若有）  */
		if (notifyInterruptPending)
			ProcessNotifyInterrupt(true);
	}
	else if (ProcDiePending)
	{
		/*
		 * 我们正在死亡。若无数据可读，那么现在处理       
		 * 是安全（也合理）的。如果我们尚未尝试读取， 
		 * 则务必设置进程 latch，这样若无数据               
		 * 我们就会回到这里死亡。如果读取已完成，也   
		 * 要务必设置进程 latch，因为读取期间可能不     
		 * 当地清除了它。    
		 */
		if (blocked)
			CHECK_FOR_INTERRUPTS();
		else
			SetLatch(MyLatch);
	}

	errno = save_errno;
}

/*
 * ProcessClientWriteInterrupt() - 处理客户端写入特有的中断        
 *
 * 它在底层写入的前后即刻被调用。         
 * 'blocked' 为真表示无数据可写且打算重试，            
 * 为假表示将写或已写完。       
 *
 * 勿改动 errno！  
 */
void
ProcessClientWriteInterrupt(bool blocked)
{
	int			save_errno = errno;

	if (ProcDiePending)
	{
		/*
		 * 我们正在死亡。如果无法写入，就应立即处理，   
		 * 否则卡住的客户端会无限期拖延我们对信号的      
		 * 响应。如果我们尚未尝试写入，则务必            
		 * 设置进程 latch，这样当写入会阻塞时               
		 * 我们就会回到这里死亡。如果写入已完成，也   
		 * 要务必设置进程 latch，因为写入期间可能不     
		 * 当地清除了它。    
		 */
		if (blocked)
		{
			/*
			 * 若 ProcessInterrupts 不处理 ProcDiePending，就不要动  
			 * whereToSendOutput。   
			 */
			if (InterruptHoldoffCount == 0 && CritSectionCount == 0)
			{
				/*
				 * 我们不想向客户端发送该错误消息，因为 a)
				 * 那样可能再次阻塞，且 b) 这很可能导致   
				 * 协议失步，因为我们可能已经                  
				 * 发送了部分协议消息。  
				 */
				if (whereToSendOutput == DestRemote)
					whereToSendOutput = DestNone;

				CHECK_FOR_INTERRUPTS();
			}
		}
		else
			SetLatch(MyLatch);
	}

	errno = save_errno;
}

/*
 * 仅做原始解析。 
 *
 * 返回解析树（RawStmt 节点）组成的列表，因为           
 * 其中可能含有多条命令。     
 *
 * 注意：对于交互式查询，务必将该例程与分析、    
 * 重写阶段分开。分析和重写无法在已中止的事务     
 * 中进行，因为它们需要访问数据库表。所以，我们    
 * 依靠原始解析器来判断是否见到了 COMMIT 或            
 * ABORT 命令；当我们处于中止状态时，其他命令           
 * 的处理不会越过原始解析阶段。                      
 */
List *
pg_parse_query(const char *query_string)
{
	List	   *raw_parsetree_list;

	TRACE_POSTGRESQL_QUERY_PARSE_START(query_string);

	if (log_parser_stats)
		ResetUsage();

	raw_parsetree_list = raw_parser(query_string, RAW_PARSE_DEFAULT);

	if (log_parser_stats)
		ShowUsage("PARSER STATISTICS");

#ifdef COPY_PARSE_PLAN_TREES
	/* 可选调试检查：让原始解析树通过 copyObject()         */
	{
		List	   *new_list = copyObject(raw_parsetree_list);

		/* 这同时检查 copyObject() 与 equal() 例程...        */
		if (!equal(new_list, raw_parsetree_list))
			elog(WARNING, "copyObject() failed to produce an equal raw parse tree");
		else
			raw_parsetree_list = new_list;
	}
#endif

	/*
	 * 可选调试检查：让原始解析树通过        
	 * outfuncs/readfuncs
	 */
#ifdef WRITE_READ_PARSE_PLAN_TREES
	{
		char	   *str = nodeToString(raw_parsetree_list);
		List	   *new_list = stringToNodeWithLocations(str);

		pfree(str);
		/* 这同时检查 outfuncs/readfuncs 与 equal() 例程...        */
		if (!equal(new_list, raw_parsetree_list))
			elog(WARNING, "outfuncs/readfuncs failed to produce an equal raw parse tree");
		else
			raw_parsetree_list = new_list;
	}
#endif

	TRACE_POSTGRESQL_QUERY_PARSE_DONE(query_string);

	return raw_parsetree_list;
}

/*
 * 给定原始解析树（gram.y 的输出）以及可选的             
 * 参数符号（$n）类型信息，执行解析分析与规则重写。    
 *
 * 返回 Query 节点列表，因为分析器或重写器都可能   
 * 把一条查询扩展为多条。          
 *
 * 注意：出于上述原因，该步骤必须与原始解析分开。     
 */
List *
pg_analyze_and_rewrite_fixedparams(RawStmt *parsetree,
								   const char *query_string,
								   const Oid *paramTypes,
								   int numParams,
								   QueryEnvironment *queryEnv)
{
	Query	   *query;
	List	   *querytree_list;

	TRACE_POSTGRESQL_QUERY_REWRITE_START(query_string);

	/*
	 * (1) 执行解析分析。  
	 */
	if (log_parser_stats)
		ResetUsage();

	query = parse_analyze_fixedparams(parsetree, query_string, paramTypes, numParams,
									  queryEnv);

	if (log_parser_stats)
		ShowUsage("PARSE ANALYSIS STATISTICS");

	/*
	 * (2) 按需重写各查询            
	 */
	querytree_list = pg_rewrite_query(query);

	TRACE_POSTGRESQL_QUERY_REWRITE_DONE(query_string);

	return querytree_list;
}

/*
 * 执行解析分析与重写。它与                 
 * pg_analyze_and_rewrite_fixedparams 等同，只是允许          
 * 从上下文推知 $n 符号的数据类型。     
 */
List *
pg_analyze_and_rewrite_varparams(RawStmt *parsetree,
								 const char *query_string,
								 Oid **paramTypes,
								 int *numParams,
								 QueryEnvironment *queryEnv)
{
	Query	   *query;
	List	   *querytree_list;

	TRACE_POSTGRESQL_QUERY_REWRITE_START(query_string);

	/*
	 * (1) 执行解析分析。  
	 */
	if (log_parser_stats)
		ResetUsage();

	query = parse_analyze_varparams(parsetree, query_string, paramTypes, numParams,
									queryEnv);

	/*
	 * 检查所有参数类型均已确定。  
	 */
	for (int i = 0; i < *numParams; i++)
	{
		Oid			ptype = (*paramTypes)[i];

		if (ptype == InvalidOid || ptype == UNKNOWNOID)
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_DATATYPE),
					 errmsg("could not determine data type of parameter $%d",
							i + 1)));
	}

	if (log_parser_stats)
		ShowUsage("PARSE ANALYSIS STATISTICS");

	/*
	 * (2) 按需重写各查询            
	 */
	querytree_list = pg_rewrite_query(query);

	TRACE_POSTGRESQL_QUERY_REWRITE_DONE(query_string);

	return querytree_list;
}

/*
 * 执行解析分析与重写。它与                 
 * pg_analyze_and_rewrite_fixedparams 等同，只是并非使用固定的   
 * 参数数据类型列表，而是提供一个解析器回调  
 * 来完成外部参数解析及其他一些事情。     
 */
List *
pg_analyze_and_rewrite_withcb(RawStmt *parsetree,
							  const char *query_string,
							  ParserSetupHook parserSetup,
							  void *parserSetupArg,
							  QueryEnvironment *queryEnv)
{
	Query	   *query;
	List	   *querytree_list;

	TRACE_POSTGRESQL_QUERY_REWRITE_START(query_string);

	/*
	 * (1) 执行解析分析。  
	 */
	if (log_parser_stats)
		ResetUsage();

	query = parse_analyze_withcb(parsetree, query_string, parserSetup, parserSetupArg,
								 queryEnv);

	if (log_parser_stats)
		ShowUsage("PARSE ANALYSIS STATISTICS");

	/*
	 * (2) 按需重写各查询            
	 */
	querytree_list = pg_rewrite_query(query);

	TRACE_POSTGRESQL_QUERY_REWRITE_DONE(query_string);

	return querytree_list;
}

/*
 * 对解析分析产出的查询执行重写。           
 *
 * 注意：查询必须刚出自解析器，因为我们未对它调用
 * AcquireRewriteLocks()。    
 */
List *
pg_rewrite_query(Query *query)
{
	List	   *querytree_list;

	if (Debug_print_parse)
		elog_node_display(LOG, "parse tree", query,
						  Debug_pretty_print);

	if (log_parser_stats)
		ResetUsage();

	if (query->commandType == CMD_UTILITY)
	{
		/* 工具命令不做重写，直接放入结果列表     */
		querytree_list = list_make1(query);
	}
	else
	{
		/* 重写常规查询      */
		querytree_list = QueryRewrite(query);
	}

	if (log_parser_stats)
		ShowUsage("REWRITER STATISTICS");

#ifdef COPY_PARSE_PLAN_TREES
	/* 可选调试检查：让查询树经过 copyObject()          */
	{
		List	   *new_list;

		new_list = copyObject(querytree_list);
		/* 这会同时检查 copyObject() 与 equal()               */
		if (!equal(new_list, querytree_list))
			elog(WARNING, "copyObject() failed to produce an equal rewritten parse tree");
		else
			querytree_list = new_list;
	}
#endif

#ifdef WRITE_READ_PARSE_PLAN_TREES
	/* 可选调试检查：让查询树经过 outfuncs/readfuncs          */
	{
		List	   *new_list = NIL;
		ListCell   *lc;

		foreach(lc, querytree_list)
		{
			Query	   *curr_query = lfirst_node(Query, lc);
			char	   *str = nodeToString(curr_query);
			Query	   *new_query = stringToNodeWithLocations(str);

			/*
			 * queryId 不会存入存储规则，但必须保留它，     
			 * 以免破坏 pg_stat_statements。        
			 */
			new_query->queryId = curr_query->queryId;

			new_list = lappend(new_list, new_query);
			pfree(str);
		}

		/* 这会同时检查 outfuncs/readfuncs 与 equal()               */
		if (!equal(new_list, querytree_list))
			elog(WARNING, "outfuncs/readfuncs failed to produce an equal rewritten parse tree");
		else
			querytree_list = new_list;
	}
#endif

	if (Debug_print_rewritten)
		elog_node_display(LOG, "rewritten parse tree", querytree_list,
						  Debug_pretty_print);

	return querytree_list;
}


/*
 * 为一条已完成重写的查询生成计划。     
 * 它是 planner() 的薄封装，参数与其相同。                 
 */
PlannedStmt *
pg_plan_query(Query *querytree, const char *query_string, int cursorOptions,
			  ParamListInfo boundParams)
{
	PlannedStmt *plan;

	/* 工具命令没有计划。     */
	if (querytree->commandType == CMD_UTILITY)
		return NULL;

	/* 规划器必须持有快照，以防其调用用户定义的函数。 */
	Assert(ActiveSnapshotSet());

	TRACE_POSTGRESQL_QUERY_PLAN_START();

	if (log_planner_stats)
		ResetUsage();

	/* 调用优化器    */
	plan = planner(querytree, query_string, cursorOptions, boundParams);

	if (log_planner_stats)
		ShowUsage("PLANNER STATISTICS");

#ifdef COPY_PARSE_PLAN_TREES
	/* 可选调试检查：让计划树经过 copyObject()          */
	{
		PlannedStmt *new_plan = copyObject(plan);

		/*
		 * equal() 目前尚无比较 Plan 节点的例程，故             
		 * 此处先不测试相等性。也许日后修复？   
		 */
#ifdef NOT_USED
		/* 这会同时检查 copyObject() 与 equal()               */
		if (!equal(new_plan, plan))
			elog(WARNING, "copyObject() failed to produce an equal plan tree");
		else
#endif
			plan = new_plan;
	}
#endif

#ifdef WRITE_READ_PARSE_PLAN_TREES
	/* 可选调试检查：让计划树经过 outfuncs/readfuncs          */
	{
		char	   *str;
		PlannedStmt *new_plan;

		str = nodeToString(plan);
		new_plan = stringToNodeWithLocations(str);
		pfree(str);

		/*
		 * equal() 目前尚无比较 Plan 节点的例程，故             
		 * 此处先不测试相等性。也许日后修复？   
		 */
#ifdef NOT_USED
		/* 这会同时检查 outfuncs/readfuncs 与 equal()               */
		if (!equal(new_plan, plan))
			elog(WARNING, "outfuncs/readfuncs failed to produce an equal plan tree");
		else
#endif
			plan = new_plan;
	}
#endif

	/*
	 * 调试时打印计划。
	 */
	if (Debug_print_plan)
		elog_node_display(LOG, "plan", plan, Debug_pretty_print);

	TRACE_POSTGRESQL_QUERY_PLAN_DONE();

	return plan;
}

/*
 * 为一组已完成重写的查询批量生成计划。 
 *
 * 常规可优化语句调用规划器；工具语句仅构造包装 
 * PlannedStmt 节点。                            
 *
 * 结果为 PlannedStmt 节点列表。     
 */
List *
pg_plan_queries(List *querytrees, const char *query_string, int cursorOptions,
				ParamListInfo boundParams)
{
	List	   *stmt_list = NIL;
	ListCell   *query_list;

	foreach(query_list, querytrees)
	{
		Query	   *query = lfirst_node(Query, query_list);
		PlannedStmt *stmt;

		if (query->commandType == CMD_UTILITY)
		{
			/* 工具命令无需规划。           */
			stmt = makeNode(PlannedStmt);
			stmt->commandType = CMD_UTILITY;
			stmt->canSetTag = query->canSetTag;
			stmt->utilityStmt = query->utilityStmt;
			stmt->stmt_location = query->stmt_location;
			stmt->stmt_len = query->stmt_len;
			stmt->queryId = query->queryId;
		}
		else
		{
			stmt = pg_plan_query(query, query_string, cursorOptions,
								 boundParams);
		}

		stmt_list = lappend(stmt_list, stmt);
	}

	return stmt_list;
}


/*
 * exec_simple_query
 *
 * 执行 simple Query 协议消息。       
 */
static void
exec_simple_query(const char *query_string)
{
	CommandDest dest = whereToSendOutput;
	MemoryContext oldcontext;
	List	   *parsetree_list;
	ListCell   *parsetree_item;
	bool		save_log_statement_stats = log_statement_stats;
	bool		was_logged = false;
	bool		use_implicit_block;
	char		msec_str[32];

	/*
	 * 将查询上报给各类监控设施。       
	 */
	debug_query_string = query_string;

	pgstat_report_activity(STATE_RUNNING, query_string);

	TRACE_POSTGRESQL_QUERY_START(query_string);

	/*
	 * 我们使用 save_log_statement_stats，以免 ShowUsage             
	 * ResetUsage 未调用时结果将失真。
	 */
	if (save_log_statement_stats)
		ResetUsage();

	/*
	 * 启动一个事务命令。查询字符串产生的各查询 
	 * 默认处于同一命令块，除非遇到 BEGIN/COMMIT/ABORT；   
	 * 此后必须强制新开事务命令，否则 xact.c 会出乱子。   
	 * （注意：这通常会改变当前内存上下文。）           
	 *                                              
	 */
	start_xact_command();

	/*
	 * 清除任何先前存在的未命名语句。（虽非严格必需，  
	 * 但将 simple Query 模式定义为使用未命名                 
	 * 语句与门户为宜；以确保回收先前操作的存储。）     
	 *                     
	 */
	drop_unnamed_stmt();

	/*
	 * 切换到合适的上下文以构建解析树。          
	 */
	oldcontext = MemoryContextSwitchTo(MessageContext);

	/*
	 * 对查询做基础解析（处于事务中止状态也应安全）   
	 *                                      
	 */
	parsetree_list = pg_parse_query(query_string);

	/* log_statement 要求时立即记录日志    */
	if (check_log_statement(parsetree_list))
	{
		ereport(LOG,
				(errmsg("statement: %s", query_string),
				 errhidestmt(true),
				 errdetail_execute(parsetree_list)));
		was_logged = true;
	}

	/*
	 * 切回事务上下文再进入循环。              
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * 由于历史原因，若 simple Query 消息里给出多条 SQL  
	 * 语句，我们会把它们当作单个事务执行，                 
	 * 除非其中包含显式事务控制命令使列表的各段     
	 * 使其各自成为独立事务。为此我们使用              
	 *                                                                     
	 * 隐式事务块。
	 */
	use_implicit_block = (list_length(parsetree_list) > 1);

	/*
	 * 遍历各原始解析树并逐个处理。            
	 */
	foreach(parsetree_item, parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, parsetree_item);
		bool		snapshot_set = false;
		CommandTag	commandTag;
		QueryCompletion qc;
		MemoryContext per_parsetree_context = NULL;
		List	   *querytree_list,
				   *plantree_list;
		Portal		portal;
		DestReceiver *receiver;
		int16		format;
		const char *cmdtagname;
		size_t		cmdtaglen;

		pgstat_report_query_id(0, true);

		/*
		 * 取得命令名用于状态显示（它还会成为                
		 * PortalRun 内部的默认完成标签）。设置 ps_status 并   
		 * 执行目标所需的任何 SQL 命令启动特殊处理。 
		 *             
		 */
		commandTag = CreateCommandTag(parsetree->stmt);
		cmdtagname = GetCommandTagNameAndLen(commandTag, &cmdtaglen);

		set_ps_display_with_len(cmdtagname, cmdtaglen);

		BeginCommand(commandTag, dest);

		/*
		 * 若处于已中止事务，拒绝除                           
		 * COMMIT/ABORT 外的全部命令。检查须先于解析分析、 
		 * 重写或规划之前，这些阶段会访问数据库，中止   
		 * 状态可能失败。（此时放开更多工具命令也许   
		 * 是安全的，但不会很多……）                        
		 *                        
		 */
		if (IsAbortedTransactionBlockState() &&
			!IsTransactionExitStmt(parsetree->stmt))
			ereport(ERROR,
					(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
					 errmsg("current transaction is aborted, "
							"commands ignored until end of transaction block"),
					 errdetail_abort()));

		/* 确保我们处于事务命令中         */
		start_xact_command();

		/*
		 * 若用隐式块且尚未在事务块中，则先开隐式块，   
		 * 强制本语句与后续语句归为一组。（每次进循环   
		 *                                                                  
		 * 都必须这样做；否则列表中的 COMMIT/ROLLBACK 会使 
		 * 后续语句不再归组。）                       
		 */
		if (use_implicit_block)
			BeginImplicitTransactionBlock();

		/* 若解析或先前命令期间收到取消信号则退出   */
		CHECK_FOR_INTERRUPTS();

		/*
		 * 若解析分析/规划需要快照则先行建立。       
		 */
		if (analyze_requires_snapshot(parsetree))
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			snapshot_set = true;
		}

		/*
		 * 可解析、重写与规划该查询了。  
		 *
		 * 切换到合适的上下文构建查询树与计划树             
		 * （它们不能放事务上下文里，因 COMMIT/ROLLBACK 时    
		 * 该上下文会被重置）。若存在多棵解析树， 
		 * 则为每棵树用独立上下文，以便处理下一棵之前   
		 * 释放这部分内存。但对最后（或唯一）解析树，
		 * 直接使用 MessageContext，反正它很快就会被重置。 
		 *                                                                  
		 * 出错后 per_parsetree_context 随 MessageContext 重置删除。 
		 */
		if (lnext(parsetree_list, parsetree_item) != NULL)
		{
			per_parsetree_context =
				AllocSetContextCreate(MessageContext,
									  "per-parsetree message context",
									  ALLOCSET_DEFAULT_SIZES);
			oldcontext = MemoryContextSwitchTo(per_parsetree_context);
		}
		else
			oldcontext = MemoryContextSwitchTo(MessageContext);

		querytree_list = pg_analyze_and_rewrite_fixedparams(parsetree, query_string,
															NULL, 0, NULL);

		plantree_list = pg_plan_queries(querytree_list, query_string,
										CURSOR_OPT_PARALLEL_OK, NULL);

		/*
		 * 解析/规划所用快照已用毕。            
		 *
		 * 同一快照复用于查询执行（至少简单协议下） 
		 *                                                                  
		 * 看似可行，但会导致使用加锁前取得的快照，     
		 * 即锁任何表之前已取得的快照。这会造成       
		 * 用户可见异常故不复用。详见 
		 * https://postgr.es/m/flat/5075D8DF.6050500@fuzzy.cz             
		 */
		if (snapshot_set)
			PopActiveSnapshot();

		/* 若解析或规划期间收到取消信号则退出     */
		CHECK_FOR_INTERRUPTS();

		/*
		 * 创建未命名门户运行查询，已有则静默删除。  
		 *                                  
		 */
		portal = CreatePortal("", true, true);
		/* 不在 pg_cursors 中显示该门户   */
		portal->visible = false;

		/*
		 * 无需向门户复制任何东西，因为这里传入的一切   
		 * 都位于 MessageContext 或                   
		 * per_parsetree_context 中，反正都比门户活得久。   
		 */
		PortalDefineQuery(portal,
						  NULL,
						  query_string,
						  commandTag,
						  plantree_list,
						  NULL);

		/*
		 * 启动门户。此处无参数。     
		 */
		PortalStart(portal, NULL, 0, InvalidSnapshot);

		/*
		 * 选择输出格式：除非对二进制游标做 FETCH，       
		 *                                                                    
		 * （在此做实属狼狈，但免别处更狼狈。唉，     
		 * 向后兼容之乐…）  
		 */
		format = 0;				/* TEXT 为默认  */
		if (IsA(parsetree->stmt, FetchStmt))
		{
			FetchStmt  *stmt = (FetchStmt *) parsetree->stmt;

			if (!stmt->ismove)
			{
				Portal		fportal = GetPortalByName(stmt->portalname);

				if (PortalIsValid(fportal) &&
					(fportal->cursorOptions & CURSOR_OPT_BINARY))
					format = 1; /* BINARY */
			}
		}
		PortalSetResultFormat(portal, 1, &format);

		/*
		 * 现在可以创建目标接收器对象了。     
		 */
		receiver = CreateDestReceiver(dest);
		if (dest == DestRemote)
			SetRemoteDestReceiverParams(receiver, portal);

		/*
		 * 切回事务上下文以供执行。             
		 */
		MemoryContextSwitchTo(oldcontext);

		/*
		 * 运行门户至完成，然后删除它（连同接收器）。   
		 */
		(void) PortalRun(portal,
						 FETCH_ALL,
						 true,	/* 总是顶层     */
						 true,	/* 忽略  */
						 receiver,
						 receiver,
						 &qc);

		receiver->rDestroy(receiver);

		PortalDrop(portal, false);

		if (lnext(parsetree_list, parsetree_item) == NULL)
		{
			/*
			 * 若是查询串最后一棵解析树，须在命令完成前 
			 * 结束事务语句。事务收尾错误都会赶在           
			 *                                                             
			 * 完成消息发出前上报，免客户端困惑——    
			 * 它们期待的是命令完成消息或错误之一，         
			 * 而非接连出现两个。此外若用隐式事务块，  
			 *                                                          
			 */
			if (use_implicit_block)
				EndImplicitTransactionBlock();
			finish_xact_command();
		}
		else if (IsA(parsetree->stmt, TransactionStmt))
		{
			/*
			 * 若这是事务控制语句则提交。下一条命令将      
			 * 开启新的事务命令。                   
			 */
			finish_xact_command();
		}
		else
		{
			/*
			 * 若没有调用 finish_xact_command()，就绝不该见到 
			 * XACT_FLAGS_NEEDIMMEDIATECOMMIT 被置位。（隐式   
			 * 事务块本应阻止它被置位。）                      
			 */
			Assert(!(MyXactFlags & XACT_FLAGS_NEEDIMMEDIATECOMMIT));

			/*
			 * 每条查询之后都需要 CommandCounterIncrement，     
			 * 开启或结束事务块的查询除外。  
			 */
			CommandCounterIncrement();

			/*
			 * 多条查询串的各查询之间停用语句超时，    
			 * 使超时对各查询分别生效。                         
			 * （下一轮循环会重新开启超时。）        
			 */
			disable_statement_timeout();
		}

		/*
		 * 告知客户端本查询已完毕。注意每棵原始解析树   
		 * 恰发出一个 EndCommand 报告，即客户端发来的每条 SQL
		 * 命令各一，与重写无关。（但被错误中止的命令 
		 * 根本不会发出任何 EndCommand 报告。）            
		 */
		EndCommand(&qc, dest, false);

		/* 现在可以删除每解析树上下文（若创建过）。   */
		if (per_parsetree_context)
			MemoryContextDelete(per_parsetree_context);
	}							/* 解析树循环结束    */

	/*
	 * 结束事务语句（若仍开着）。（仅当解析树列表为空
	 * 时才起作用；否则最后一轮循环已处理过）。      
	 *                           
	 */
	finish_xact_command();

	/*
	 * 若无解析树则返回 EmptyQueryResponse 消息。          
	 */
	if (!parsetree_list)
		NullCommand(dest);

	/*
	 * 视情况发出时长日志。       
	 */
	switch (check_log_duration(msec_str, was_logged))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  statement: %s",
							msec_str, query_string),
					 errhidestmt(true),
					 errdetail_execute(parsetree_list)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("QUERY STATISTICS");

	TRACE_POSTGRESQL_QUERY_DONE(query_string);

	debug_query_string = NULL;
}

/*
 * exec_parse_message
 *
 * 执行 Parse 协议消息。       
 */
static void
exec_parse_message(const char *query_string,	/* 待执行字符串*/
				   const char *stmt_name,	/* 预备语句名        */
				   Oid *paramTypes, /* 参数类型    */
				   int numParams)	/* 参数个数         */
{
	MemoryContext unnamed_stmt_context = NULL;
	MemoryContext oldcontext;
	List	   *parsetree_list;
	RawStmt    *raw_parse_tree;
	List	   *querytree_list;
	CachedPlanSource *psrc;
	bool		is_named;
	bool		save_log_statement_stats = log_statement_stats;
	char		msec_str[32];

	/*
	 * 将查询上报给各类监控设施。       
	 */
	debug_query_string = query_string;

	pgstat_report_activity(STATE_RUNNING, query_string);

	set_ps_display("PARSE");

	if (save_log_statement_stats)
		ResetUsage();

	ereport(DEBUG2,
			(errmsg_internal("parse %s: %s",
							 *stmt_name ? stmt_name : "<unnamed>",
							 query_string)));

	/*
	 * 启动一个事务命令以便执行解析分析等。（注意       
	 * 这通常会改变内存上下文。）若已在事务中则无事。  
	 * 必要时它还会启用语句超时。                           
	 *           
	 */
	start_xact_command();

	/*
	 * 切换到合适的上下文以构建解析树。          
	 *
	 * 按预备语句是否命名有两种策略。命名的预备语句   
	 *                                                                
	 * 在 MessageContext 中解析，再把完成的树复制进  
	 * 预备语句的 plancache 条目；随后 MessageContext 的          
	 * 重置会释放解析与重写占用的临时空间。未命名预备 
	 * 语句则假定语句不会久留，快速清理临时空间未必    
	 *                                                                     
	 * 值得付出复制解析树的代价。于是此处创建 plancache   
	 * 条目的 query_context，并在其中完成解析。   
	 */
	is_named = (stmt_name[0] != '\0');
	if (is_named)
	{
		/* 命名预备语句——在 MessageContext 中解析 */
		oldcontext = MemoryContextSwitchTo(MessageContext);
	}
	else
	{
		/* 未命名预备语句——释放先前的未命名语句     */
		drop_unnamed_stmt();
		/* 创建解析用上下文   */
		unnamed_stmt_context =
			AllocSetContextCreate(MessageContext,
								  "unnamed prepared statement",
								  ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(unnamed_stmt_context);
	}

	/*
	 * 对查询做基础解析（处于事务中止状态也应安全）   
	 *                                      
	 */
	parsetree_list = pg_parse_query(query_string);

	/*
	 * 预备语句中只允许一条用户语句。这是为了让          
	 * 协议保持简单——否则须操心多重结果 tupdesc 等    
	 *                                                     
	 */
	if (list_length(parsetree_list) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot insert multiple commands into a prepared statement")));

	if (parsetree_list != NIL)
	{
		bool		snapshot_set = false;

		raw_parse_tree = linitial_node(RawStmt, parsetree_list);

		/*
		 * 若处于已中止事务，拒绝除                           
		 * COMMIT/ROLLBACK 外的命令。检查须先于解析分析、   
		 * 重写或规划之前，这些阶段会访问数据库，中止
		 * 状态可能失败。（此时放开更多工具命令也许      
		 * 是安全的，但不会很多……）                            
		 *                        
		 */
		if (IsAbortedTransactionBlockState() &&
			!IsTransactionExitStmt(raw_parse_tree->stmt))
			ereport(ERROR,
					(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
					 errmsg("current transaction is aborted, "
							"commands ignored until end of transaction block"),
					 errdetail_abort()));

		/*
		 * 解析分析之前先创建 CachedPlanSource，因为它需要   
		 * 看到未经修改的原始解析树。    
		 */
		psrc = CreateCachedPlan(raw_parse_tree, query_string,
								CreateCommandTag(raw_parse_tree->stmt));

		/*
		 * 若解析分析需要快照则先行建立。     
		 */
		if (analyze_requires_snapshot(raw_parse_tree))
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			snapshot_set = true;
		}

		/*
		 * 对查询做解析与重写。注意最初指定的参数集      
		 * 不要求完整，因此必须使用                           
		 * pg_analyze_and_rewrite_varparams() 
		 */
		querytree_list = pg_analyze_and_rewrite_varparams(raw_parse_tree,
														  query_string,
														  &paramTypes,
														  &numParams,
														  NULL);

		/* 解析所用快照已使用完毕       */
		if (snapshot_set)
			PopActiveSnapshot();
	}
	else
	{
		/* 输入串为空是合法的。      */
		raw_parse_tree = NULL;
		psrc = CreateCachedPlan(raw_parse_tree, query_string,
								CMDTAG_UNKNOWN);
		querytree_list = NIL;
	}

	/*
	 * CachedPlanSource 必须是 MessageContext 的直接子节点，    
	 * 然后才能把 unnamed_stmt_context 重新挂到其下，否则   
	 * 环形子图不连通。虽笨拙，但好过反复切换上下文。  
	 *       
	 */
	if (unnamed_stmt_context)
		MemoryContextSetParent(psrc->context, MessageContext);

	/* 完成 CachedPlanSource 的填充      */
	CompleteCachedPlan(psrc,
					   querytree_list,
					   unnamed_stmt_context,
					   paramTypes,
					   numParams,
					   NULL,
					   NULL,
					   CURSOR_OPT_PARALLEL_OK,	/* 允许并行模式  */
					   true);	/* 固定结果 */

	/* 若在分析期间收到取消信号，则退出*/
	CHECK_FOR_INTERRUPTS();

	if (is_named)
	{
		/*
		 * 将该查询存储为预备语句。    
		 */
		StorePreparedStatement(stmt_name, psrc, false);
	}
	else
	{
		/*
		 * 只需把 CachedPlanSource 存入 unnamed_stmt_psrc。   
		 */
		SaveCachedPlan(psrc);
		unnamed_stmt_psrc = psrc;
	}

	MemoryContextSwitchTo(oldcontext);

	/*
	 * 此处不关闭已开启的事务命令；只有当客户端发送  
	 * Sync 时才关闭。此处转而执行 CommandCounterIncrement       
	 * 以防万一 parse/plan 期间有变动。   
	 */
	CommandCounterIncrement();

	/*
	 * 发送ParseComplete
	 */
	if (whereToSendOutput == DestRemote)
		pq_putemptymessage('1');

	/*
	 * 在适当时输出时长日志。    
	 */
	switch (check_log_duration(msec_str, false))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  parse %s: %s",
							msec_str,
							*stmt_name ? stmt_name : "<unnamed>",
							query_string),
					 errhidestmt(true)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("PARSE MESSAGE STATISTICS");

	debug_query_string = NULL;
}

/*
 * exec_bind_message
 *
 * 处理 "Bind" 消息，从预备语句创建门户。                
 */
static void
exec_bind_message(StringInfo input_message)
{
	const char *portal_name;
	const char *stmt_name;
	int			numPFormats;
	int16	   *pformats = NULL;
	int			numParams;
	int			numRFormats;
	int16	   *rformats = NULL;
	CachedPlanSource *psrc;
	CachedPlan *cplan;
	Portal		portal;
	char	   *query_string;
	char	   *saved_stmt_name;
	ParamListInfo params;
	MemoryContext oldContext;
	bool		save_log_statement_stats = log_statement_stats;
	bool		snapshot_set = false;
	char		msec_str[32];
	ParamsErrorCbData params_data;
	ErrorContextCallback params_errcxt;
	ListCell   *lc;

	/* 获取消息的固定部分       */
	portal_name = pq_getmsgstring(input_message);
	stmt_name = pq_getmsgstring(input_message);

	ereport(DEBUG2,
			(errmsg_internal("bind %s to %s",
							 *portal_name ? portal_name : "<unnamed>",
							 *stmt_name ? stmt_name : "<unnamed>")));

	/* 查找预备语句      */
	if (stmt_name[0] != '\0')
	{
		PreparedStatement *pstmt;

		pstmt = FetchPreparedStatement(stmt_name, true);
		psrc = pstmt->plansource;
	}
	else
	{
		/* 特殊处理未命名语句        */
		psrc = unnamed_stmt_psrc;
		if (!psrc)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
					 errmsg("unnamed prepared statement does not exist")));
	}

	/*
	 * 向各种监控设施上报查询。          
	 */
	debug_query_string = psrc->query_string;

	pgstat_report_activity(STATE_RUNNING, psrc->query_string);

	foreach(lc, psrc->query_list)
	{
		Query	   *query = lfirst_node(Query, lc);

		if (query->queryId != UINT64CONST(0))
		{
			pgstat_report_query_id(query->queryId, false);
			break;
		}
	}

	set_ps_display("BIND");

	if (save_log_statement_stats)
		ResetUsage();

	/*
	 * 启动一个事务命令，以便能调用函数等。（注意：     
	 * 这通常会改变当前内存上下文。）若已处于其中，   
	 * 则不做任何动作。必要时还会布防语句超时。   
	 *           
	 */
	start_xact_command();

	/* 切换回消息上下文       */
	MemoryContextSwitchTo(MessageContext);

	/* 获取参数格式码          */
	numPFormats = pq_getmsgint(input_message, 2);
	if (numPFormats > 0)
	{
		pformats = palloc_array(int16, numPFormats);
		for (int i = 0; i < numPFormats; i++)
			pformats[i] = pq_getmsgint(input_message, 2);
	}

	/* 获取参数值个数         */
	numParams = pq_getmsgint(input_message, 2);

	if (numPFormats > 1 && numPFormats != numParams)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("bind message has %d parameter formats but %d parameters",
						numPFormats, numParams)));

	if (numParams != psrc->num_params)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("bind message supplies %d parameters, but prepared statement \"%s\" requires %d",
						numParams, stmt_name, psrc->num_params)));

	/*
	 * 若事务已中止，则唯一能真正运行的门户是      
	 * 只包含 COMMIT 或 ROLLBACK 命令的门户。对其余        
	 * 绑定一律禁止，以免给要求在有效事务内运行的     
	 * 基础设施造成问题。参数绑定同样被禁止，        
	 * 因为我们不能冒险调用用户自定义的 I/O 函数。      
	 *           
	 */
	if (IsAbortedTransactionBlockState() &&
		(!(psrc->raw_parse_tree &&
		   IsTransactionExitStmt(psrc->raw_parse_tree->stmt)) ||
		 numParams != 0))
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	/*
	 * 创建门户。仅在指定未命名门户时，才允许静默替换  
	 * 既有门户。                    
	 */
	if (portal_name[0] == '\0')
		portal = CreatePortal(portal_name, true, true);
	else
		portal = CreatePortal(portal_name, false, false);

	/*
	 * 准备向门户的内存上下文复制内容。这些复制全部     
	 * 提前完成，复制可能失败（内存耗尽），不希望     
	 * 失败发生在 GetCachedPlan 与                      
	 * PortalDefineQuery 之间，那会泄漏 plancache 的引用计数。   
	 */
	oldContext = MemoryContextSwitchTo(portal->portalContext);

	/* 把计划的查询字符串复制进门户   */
	query_string = pstrdup(psrc->query_string);

	/* 同样复制一份语句名，除非它是未命名的          */
	if (stmt_name[0])
		saved_stmt_name = pstrdup(stmt_name);
	else
		saved_stmt_name = NULL;

	/*
	 * 若有参数待取（输入函数需要快照），或查询  
	 * 不是工具命令（因而可能需要重新解析分析并      
	 * 规划）。我们让快照一直保持有效直至完工，这样    
	 * plancache.c 就不必再取新的快照。                          
	 *               
	 */
	if (numParams > 0 ||
		(psrc->raw_parse_tree &&
		 analyze_requires_snapshot(psrc->raw_parse_tree)))
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot_set = true;
	}

	/*
	 * 取回参数（若有），并存入门户的内存上下文。    
	 */
	if (numParams > 0)
	{
		char	  **knownTextValues = NULL; /* 首次使用时分配 */
		BindParamCbData one_param_data;

		/*
		 * 设置错误回调，这样若本阶段出错，就能报告引发 
		 * 问题的具体参数。                                 
		 */
		one_param_data.portalName = portal->name;
		one_param_data.paramno = -1;
		one_param_data.paramval = NULL;
		params_errcxt.previous = error_context_stack;
		params_errcxt.callback = bind_param_error_callback;
		params_errcxt.arg = (void *) &one_param_data;
		error_context_stack = &params_errcxt;

		params = makeParamList(numParams);

		for (int paramno = 0; paramno < numParams; paramno++)
		{
			Oid			ptype = psrc->param_types[paramno];
			int32		plength;
			Datum		pval;
			bool		isNull;
			StringInfoData pbuf;
			char		csave;
			int16		pformat;

			one_param_data.paramno = paramno;
			one_param_data.paramval = NULL;

			plength = pq_getmsgint(input_message, 4);
			isNull = (plength == -1);

			if (!isNull)
			{
				const char *pvalue = pq_getmsgbytes(input_message, plength);

				/*
				 * 无需复制数据，只设一个指向消息缓冲区 
				 * 中正确部分的伪 StringInfo。                      
				 * 我们假定消息缓冲可被随意涂写，以维持     
				 * StringInfo 末尾空字符的惯例。虽不体面，
				 * 但处理超大参数串时是一大赢面。       
				 *                                           
				 */
				pbuf.data = unconstify(char *, pvalue);
				pbuf.maxlen = plength + 1;
				pbuf.len = plength;
				pbuf.cursor = 0;

				csave = pbuf.data[plength];
				pbuf.data[plength] = '\0';
			}
			else
			{
				pbuf.data = NULL;	/* 让编译器安静  */
				csave = 0;
			}

			if (numPFormats > 1)
				pformat = pformats[paramno];
			else if (numPFormats > 0)
				pformat = pformats[0];
			else
				pformat = 0;	/* 默认 = text  */

			if (pformat == 0)	/* 文本    */
			{
				Oid			typinput;
				Oid			typioparam;
				char	   *pstring;

				getTypeInputInfo(ptype, &typinput, &typioparam);

				/*
				 * 调用 typinput 例程前必须先做编码转换。
				 *                  
				 */
				if (isNull)
					pstring = NULL;
				else
					pstring = pg_client_to_server(pbuf.data, plength);

				/* 现可记录输入串，供出错时使用       */
				one_param_data.paramval = pstring;

				pval = OidInputFunctionCall(typinput, pstring, typioparam, -1);

				one_param_data.paramval = NULL;

				/*
				 * 若稍后可能记录参数，把转换串副本存入  
				 * MessageContext；随后释放编码转换结果（   
				 * 若有进行过转换）。                    
				 */
				if (pstring)
				{
					if (log_parameter_max_length_on_error != 0)
					{
						MemoryContext oldcxt;

						oldcxt = MemoryContextSwitchTo(MessageContext);

						if (knownTextValues == NULL)
							knownTextValues = palloc0_array(char *, numParams);

						if (log_parameter_max_length_on_error < 0)
							knownTextValues[paramno] = pstrdup(pstring);
						else
						{
							/*
							 * 保存串可截短，不会打印全部。   
							 * 但所复制完整字符数至少要比    
							 * BuildParamLogString 多两个，   
							 * 否则可能无法包含末尾省略号。    
							 *                                             
							 */
							knownTextValues[paramno] =
								pnstrdup(pstring,
										 log_parameter_max_length_on_error
										 + 2 * MAX_MULTIBYTE_CHAR_LEN);
						}

						MemoryContextSwitchTo(oldcxt);
					}
					if (pstring != pbuf.data)
						pfree(pstring);
				}
			}
			else if (pformat == 1)	/* 二进制   */
			{
				Oid			typreceive;
				Oid			typioparam;
				StringInfo	bufptr;

				/*
				 * 调用参数类型的二进制输入转换器。
				 */
				getTypeBinaryInputInfo(ptype, &typreceive, &typioparam);

				if (isNull)
					bufptr = NULL;
				else
					bufptr = &pbuf;

				pval = OidReceiveFunctionCall(typreceive, bufptr, typioparam, -1);

				/* 若它未消费整个缓冲区则有问题*/
				if (!isNull && pbuf.cursor != pbuf.len)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("incorrect binary data format in bind parameter %d",
									paramno + 1)));
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unsupported format code: %d",
								pformat)));
				pval = 0;		/* 让编译器安静  */
			}

			/* 恢复消息缓冲区的内容  */
			if (!isNull)
				pbuf.data[plength] = csave;

			params->params[paramno].value = pval;
			params->params[paramno].isnull = isNull;

			/*
			 * 把参数标记为 CONST。这确保任何自定义计划      
			 * 都能充分利用参数值。         
			 */
			params->params[paramno].pflags = PARAM_FLAG_CONST;
			params->params[paramno].ptype = ptype;
		}

		/* 弹出每参数错误回调          */
		error_context_stack = error_context_stack->previous;

		/*
		 * 全部参数接收完毕后，若已配置如此，则准备好  
		 * 在以后的错误中打印它们。（这些会保存在门户 
		 * 中，供查询日后执行时显示。）                       
		 */
		if (log_parameter_max_length_on_error != 0)
			params->paramValuesStr =
				BuildParamLogString(params,
									knownTextValues,
									log_parameter_max_length_on_error);
	}
	else
		params = NULL;

	/* 门户上下文中的存储工作完成*/
	MemoryContextSwitchTo(oldContext);

	/*
	 * 再设置另一个错误回调，这样若在 BIND 处理的          
	 * 剩余过程中出错，所有参数都会被记录。 
	 */
	params_data.portalName = portal->name;
	params_data.params = params;
	params_errcxt.previous = error_context_stack;
	params_errcxt.callback = ParamsErrorCallback;
	params_errcxt.arg = (void *) &params_data;
	error_context_stack = &params_errcxt;

	/* 获取结果格式码       */
	numRFormats = pq_getmsgint(input_message, 2);
	if (numRFormats > 0)
	{
		rformats = palloc_array(int16, numRFormats);
		for (int i = 0; i < numRFormats; i++)
			rformats[i] = pq_getmsgint(input_message, 2);
	}

	pq_getmsgend(input_message);

	/*
	 * 从 CachedPlanSource 获取计划。（重）规划产生的         
	 * 垃圾都会生成在 MessageContext 中。计划引用计数  
	 * 将分配给门户，因此会在门户销毁时释放。            
	 */
	cplan = GetCachedPlan(psrc, params, NULL, NULL);

	/*
	 * 现在即可定义门户。  
	 *
	 * 切勿在上面的 GetCachedPlan 调用与此处之间放置      
	 * 任何可能抛错的代码。    
	 */
	PortalDefineQuery(portal,
					  saved_stmt_name,
					  query_string,
					  psrc->commandTag,
					  cplan->stmt_list,
					  cplan);

	/* 参数 I/O 与解析/规划所用快照已使用完毕             */
	if (snapshot_set)
		PopActiveSnapshot();

	/*
	 * 至此可以启动门户执行了。      
	 */
	PortalStart(portal, params, 0, InvalidSnapshot);

	/*
	 * 把结果格式请求应用到门户上。     
	 */
	PortalSetResultFormat(portal, numRFormats, rformats);

	/*
	 * 绑定完成；移除参数错误回调。此后发出的日志     
	 * 条目各自独立决定是否记录参数。                      
	 */
	error_context_stack = error_context_stack->previous;

	/*
	 * 发送BindComplete
	 */
	if (whereToSendOutput == DestRemote)
		pq_putemptymessage('2');

	/*
	 * 在适当时输出时长日志。    
	 */
	switch (check_log_duration(msec_str, false))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  bind %s%s%s: %s",
							msec_str,
							*stmt_name ? stmt_name : "<unnamed>",
							*portal_name ? "/" : "",
							*portal_name ? portal_name : "",
							psrc->query_string),
					 errhidestmt(true),
					 errdetail_params(params)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("BIND MESSAGE STATISTICS");

	valgrind_report_error_query(debug_query_string);

	debug_query_string = NULL;
}

/*
 * exec_execute_message
 *
 * 处理某门户的 "Execute" 消息。   
 */
static void
exec_execute_message(const char *portal_name, long max_rows)
{
	CommandDest dest;
	DestReceiver *receiver;
	Portal		portal;
	bool		completed;
	QueryCompletion qc;
	const char *sourceText;
	const char *prepStmtName;
	ParamListInfo portalParams;
	bool		save_log_statement_stats = log_statement_stats;
	bool		is_xact_command;
	bool		execute_is_fetch;
	bool		was_logged = false;
	char		msec_str[32];
	ParamsErrorCbData params_data;
	ErrorContextCallback params_errcxt;
	const char *cmdtagname;
	size_t		cmdtaglen;
	ListCell   *lc;

	/* 调整目的地，告知 printtup.c 该做什么 */
	dest = whereToSendOutput;
	if (dest == DestRemote)
		dest = DestRemoteExecute;

	portal = GetPortalByName(portal_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("portal \"%s\" does not exist", portal_name)));

	/*
	 * 若原始查询是空串，则直接返回          
	 * EmptyQueryResponse 
	 */
	if (portal->commandTag == CMDTAG_UNKNOWN)
	{
		Assert(portal->stmts == NIL);
		NullCommand(dest);
		return;
	}

	/* 门户中是否含有事务命令？           */
	is_xact_command = IsTransactionStmtList(portal->stmts);

	/*
	 * 必须把 sourceText 和 prepStmtName 复制到 MessageContext，  
	 * 以防门户在 finish_xact_command 期间被销毁。但          
	 * portalParams 则不复制，那种情况下干脆不打印它们。 
	 *                   
	 */
	sourceText = pstrdup(portal->sourceText);
	if (portal->prepStmtName)
		prepStmtName = pstrdup(portal->prepStmtName);
	else
		prepStmtName = "<unnamed>";
	portalParams = portal->portalParams;

	/*
	 * 向各种监控设施上报查询。          
	 */
	debug_query_string = sourceText;

	pgstat_report_activity(STATE_RUNNING, sourceText);

	foreach(lc, portal->stmts)
	{
		PlannedStmt *stmt = lfirst_node(PlannedStmt, lc);

		if (stmt->queryId != UINT64CONST(0))
		{
			pgstat_report_query_id(stmt->queryId, false);
			break;
		}
	}

	cmdtagname = GetCommandTagNameAndLen(portal->commandTag, &cmdtaglen);

	set_ps_display_with_len(cmdtagname, cmdtaglen);

	if (save_log_statement_stats)
		ResetUsage();

	BeginCommand(portal->commandTag, dest);

	/*
	 * 在 MessageContext 中创建目标接收器（不要放在             
	 * 事务上下文，因为门户含 VACUUM 时可能被删除）。
	 */
	receiver = CreateDestReceiver(dest);
	if (dest == DestRemoteExecute)
		SetRemoteDestReceiverParams(receiver, portal);

	/*
	 * 确保当前处于事务命令中（由于先前已执行 BIND，  
	 * 通常理应如此）。        
	 */
	start_xact_command();

	/*
	 * 若针对既有门户重新发出 Execute 协议请求，那么       
	 * 我们只是取回更多行，而非从头完整地重新执行        
	 * 查询。对 v3 门户 atStart 永不复位，因此用这个          
	 * 检查是安全的。      
	 */
	execute_is_fetch = !portal->atStart;

	/* 若 log_statement 规定则立即记录日志*/
	if (check_log_statement(portal->stmts))
	{
		ereport(LOG,
				(errmsg("%s %s%s%s: %s",
						execute_is_fetch ?
						_("execute fetch from") :
						_("execute"),
						prepStmtName,
						*portal_name ? "/" : "",
						*portal_name ? portal_name : "",
						sourceText),
				 errhidestmt(true),
				 errdetail_params(portalParams)));
		was_logged = true;
	}

	/*
	 * 若事务已中止，则唯一能真正运行的门户是      
	 * 只包含 COMMIT 或 ROLLBACK 命令的那些门户。        
	 */
	if (IsAbortedTransactionBlockState() &&
		!IsTransactionExitStmtList(portal->stmts))
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	/* 开始执行之前检查取消信号              */
	CHECK_FOR_INTERRUPTS();

	/*
	 * 可以运行门户了。设置错误回调，使参数能被记入    
	 * 日志。参数必须已在绑定阶段保存过。                
	 */
	params_data.portalName = portal->name;
	params_data.params = portalParams;
	params_errcxt.previous = error_context_stack;
	params_errcxt.callback = ParamsErrorCallback;
	params_errcxt.arg = (void *) &params_data;
	error_context_stack = &params_errcxt;

	if (max_rows <= 0)
		max_rows = FETCH_ALL;

	completed = PortalRun(portal,
						  max_rows,
						  true, /* 始终为顶层  */
						  true, /* 忽略  */
						  receiver,
						  receiver,
						  &qc);

	receiver->rDestroy(receiver);

	/* 执行完毕；移除参数错误回调          */
	error_context_stack = error_context_stack->previous;

	if (completed)
	{
		if (is_xact_command || (MyXactFlags & XACT_FLAGS_NEEDIMMEDIATECOMMIT))
		{
			/*
			 * 若是事务控制语句则提交。将为下一条命令  
			 * （若有）开始 xact 命令。若语句要求立即提交
			 * 也一样。若无这一保障，就要等收到 Sync 之后 
			 * 才强制提交。若客户端流水线发送立即   
			 * 提交语句，就会产生风险。                   
			 *                                      
			 */
			finish_xact_command();

			/*
			 * 这类命令通常没有任何参数，即便有，现在也无
			 * 法打印，因为存储在 finish_xact_command 期间已  
			 * 不存在了。所以假装没有参数。                   
			 */
			portalParams = NULL;
		}
		else
		{
			/*
			 * 每条查询之后都需要 CommandCounterIncrement，     
			 * 开启或结束事务块的查询除外。  
			 */
			CommandCounterIncrement();

			/*
			 * 每当 Execute 消息完成而未立即提交事务时，
			 * 就设置 XACT_FLAGS_PIPELINING。                     
			 */
			MyXactFlags |= XACT_FLAGS_PIPELINING;

			/*
			 * 每当 Execute 消息完成时都禁用语句超时。   
			 * 下一条协议消息会重新开始新的超时。            
			 */
			disable_statement_timeout();
		}

		/* 向客户端发送相应的 CommandComplete*/
		EndCommand(&qc, dest, false);
	}
	else
	{
		/* 门户未运行完毕，故发送 PortalSuspended*/
		if (whereToSendOutput == DestRemote)
			pq_putemptymessage('s');

		/*
		 * 挂起 Execute 消息时同样设置 XACT_FLAGS_PIPELINING。    
		 *     
		 */
		MyXactFlags |= XACT_FLAGS_PIPELINING;
	}

	/*
	 * 在适当时输出时长日志。    
	 */
	switch (check_log_duration(msec_str, was_logged))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  %s %s%s%s: %s",
							msec_str,
							execute_is_fetch ?
							_("execute fetch from") :
							_("execute"),
							prepStmtName,
							*portal_name ? "/" : "",
							*portal_name ? portal_name : "",
							sourceText),
					 errhidestmt(true),
					 errdetail_params(portalParams)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("EXECUTE MESSAGE STATISTICS");

	valgrind_report_error_query(debug_query_string);

	debug_query_string = NULL;
}

/*
 * check_log_statement
 *		判断命令是否因 log_statement 而应记录日志。          
 *
 * stmt_list 可以是文法原始输出或已规划语句列表  
 *           
 */
static bool
check_log_statement(List *stmt_list)
{
	ListCell   *stmt_item;

	if (log_statement == LOGSTMT_NONE)
		return false;
	if (log_statement == LOGSTMT_ALL)
		return true;

	/* 否则必须检查各语句以确定是否记录               */
	foreach(stmt_item, stmt_list)
	{
		Node	   *stmt = (Node *) lfirst(stmt_item);

		if (GetCommandLogLevel(stmt) <= log_statement)
			return true;
	}

	return false;
}

/*
 * check_log_duration
 *		判断当前命令的时长是否应记入日志，          
 *		同时检查本事务中的该语句是否必须记录            
 *		（无论时长如何）。  
 *
 * 返回  
 *		0 无需记录日志     
 *		1 仅记录时长                      
 *		2 记录时长和查询详情                   
 *
 * 若需要记录日志，则以毫秒为单位的时长会被格式化进
 * msec_str[]，须为 32 字节  
 *
 * 若调用者已记录过查询详情，则 was_logged 应为真       
 * （这基本上阻止了返回 2）。      
 */
int
check_log_duration(char *msec_str, bool was_logged)
{
	if (log_duration || log_min_duration_sample >= 0 ||
		log_min_duration_statement >= 0 || xact_is_sampled)
	{
		long		secs;
		int			usecs;
		int			msecs;
		bool		exceeded_duration;
		bool		exceeded_sample_duration;
		bool		in_sample = false;

		TimestampDifference(GetCurrentStatementStartTimestamp(),
							GetCurrentTimestamp(),
							&secs, &usecs);
		msecs = usecs / 1000;

		/*
		 * 对 log_min_duration_* 是否超限的这一怪异检查      
		 * 是为了避免极长时长下的整数溢出：                  
		 * 在确认 secs * 1000 能装入 int 之前不计算它。   
		 */
		exceeded_duration = (log_min_duration_statement == 0 ||
							 (log_min_duration_statement > 0 &&
							  (secs > log_min_duration_statement / 1000 ||
							   secs * 1000 + msecs >= log_min_duration_statement)));

		exceeded_sample_duration = (log_min_duration_sample == 0 ||
									(log_min_duration_sample > 0 &&
									 (secs > log_min_duration_sample / 1000 ||
									  secs * 1000 + msecs >= log_min_duration_sample)));

		/*
		 * log_statement_sample_rate = 0 时不记录日志；         
		 * 若 log_statement_sample_rate <= 1 则记录抽样；若         
		 * 是 1 就省去 PRNG 调用。
		 */
		if (exceeded_sample_duration)
			in_sample = log_statement_sample_rate != 0 &&
				(log_statement_sample_rate == 1 ||
				 pg_prng_double(&pg_global_prng_state) <= log_statement_sample_rate);

		if (exceeded_duration || in_sample || log_duration || xact_is_sampled)
		{
			snprintf(msec_str, 32, "%ld.%03d",
					 secs * 1000 + msecs, usecs % 1000);
			if ((exceeded_duration || in_sample || xact_is_sampled) && !was_logged)
				return 2;
			else
				return 1;
		}
	}

	return 0;
}

/*
 * errdetail_execute
 *
 * 添加 errdetail() 行，显示 EXECUTE 所引用的查询（若有）。  
 * 参数是原始的 parsetree 列表。 
 */
static int
errdetail_execute(List *raw_parsetree_list)
{
	ListCell   *parsetree_item;

	foreach(parsetree_item, raw_parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, parsetree_item);

		if (IsA(parsetree->stmt, ExecuteStmt))
		{
			ExecuteStmt *stmt = (ExecuteStmt *) parsetree->stmt;
			PreparedStatement *pstmt;

			pstmt = FetchPreparedStatement(stmt->name, false);
			if (pstmt)
			{
				errdetail("prepare: %s", pstmt->plansource->query_string);
				return 0;
			}
		}
	}

	return 0;
}

/*
 * errdetail_params
 *
 * 添加 errdetail() 行，显示绑定参数数据（若有）。  
 * 注意这只用于语句日志，故由 log_parameter_max_length      
 * 而非 log_parameter_max_length_on_error 控制。                
 */
static int
errdetail_params(ParamListInfo params)
{
	if (params && params->numParams > 0 && log_parameter_max_length != 0)
	{
		char	   *str;

		str = BuildParamLogString(params, NULL, log_parameter_max_length);
		if (str && str[0] != '\0')
			errdetail("parameters: %s", str);
	}

	return 0;
}

/*
 * errdetail_abort
 *
 * 添加 errdetail() 行显示中止原因（若有） 
 */
static int
errdetail_abort(void)
{
	if (MyProc->recoveryConflictPending)
		errdetail("abort reason: recovery conflict");

	return 0;
}

/*
 * errdetail_recovery_conflict
 *
 * 添加 errdetail() 行以显示冲突来源。  
 */
static int
errdetail_recovery_conflict(void)
{
	switch (RecoveryConflictReason)
	{
		case PROCSIG_RECOVERY_CONFLICT_BUFFERPIN:
			errdetail("User was holding shared buffer pin for too long.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOCK:
			errdetail("User was holding a relation lock for too long.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_TABLESPACE:
			errdetail("User was or might have been using tablespace that must be dropped.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_SNAPSHOT:
			errdetail("User query might have needed to see row versions that must be removed.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT:
			errdetail("User was using a logical replication slot that must be invalidated.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK:
			errdetail("User transaction caused buffer deadlock with recovery.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_DATABASE:
			errdetail("User was connected to a database that must be dropped.");
			break;
		default:
			break;
			/* 无 errdetail*/
	}

	return 0;
}

/*
 * bind_param_error_callback
 *
 * 在解析 Bind 消息中的参数时使用的错误上下文回调。 
 */
static void
bind_param_error_callback(void *arg)
{
	BindParamCbData *data = (BindParamCbData *) arg;
	StringInfoData buf;
	char	   *quotedval;

	if (data->paramno < 0)
		return;

	/* 若有文本值，则加引号，必要时做裁剪。      */
	if (data->paramval)
	{
		initStringInfo(&buf);
		appendStringInfoStringQuoted(&buf, data->paramval,
									 log_parameter_max_length_on_error);
		quotedval = buf.data;
	}
	else
		quotedval = NULL;

	if (data->portalName && data->portalName[0] != '\0')
	{
		if (quotedval)
			errcontext("portal \"%s\" parameter $%d = %s",
					   data->portalName, data->paramno + 1, quotedval);
		else
			errcontext("portal \"%s\" parameter $%d",
					   data->portalName, data->paramno + 1);
	}
	else
	{
		if (quotedval)
			errcontext("unnamed portal parameter $%d = %s",
					   data->paramno + 1, quotedval);
		else
			errcontext("unnamed portal parameter $%d",
					   data->paramno + 1);
	}

	if (quotedval)
		pfree(quotedval);
}

/*
 * exec_describe_statement_message
 *
 * 处理针对预备语句的 "Describe" 消息        
 */
static void
exec_describe_statement_message(const char *stmt_name)
{
	CachedPlanSource *psrc;

	/*
	 * 启动一个事务命令。（注意这通常会改变当前内存  
	 * 上下文。）若已处于事务中则什么都不做。         
	 */
	start_xact_command();

	/* 切换回消息上下文       */
	MemoryContextSwitchTo(MessageContext);

	/* 查找预备语句      */
	if (stmt_name[0] != '\0')
	{
		PreparedStatement *pstmt;

		pstmt = FetchPreparedStatement(stmt_name, true);
		psrc = pstmt->plansource;
	}
	else
	{
		/* 对未命名语句做特例处理  */
		psrc = unnamed_stmt_psrc;
		if (!psrc)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
					 errmsg("unnamed prepared statement does not exist")));
	}

	/* 预备语句不应有可变更的结果描述符           */
	Assert(psrc->fixed_result);

	/*
	 * 若处于事务中止状态，就不能运行       
	 * SendRowDescriptionMessage()，因为需要访问系统目录。  
	 * 因此，拒绝 Describe 返回数据的语句。（我们不        
	 * 应拒绝所有 Describe，因为那可能破坏某些客户端      
	 * 发出 COMMIT 或 ROLLBACK 命令的能力，如果它们的       
	 * 代码盲目 Describe 所做的每件事。）我们可以      
	 * 无危险地 Describe 参数，所以对此不做限制。  
	 */
	if (IsAbortedTransactionBlockState() &&
		psrc->resultDesc)
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	if (whereToSendOutput != DestRemote)
		return;					/* 实际上做不了什么      */

	/*
	 * 先描述参数……           
	 */
	pq_beginmessage_reuse(&row_description_buf, 't');	/* parameter description
														 * message type */
	pq_sendint16(&row_description_buf, psrc->num_params);

	for (int i = 0; i < psrc->num_params; i++)
	{
		Oid			ptype = psrc->param_types[i];

		pq_sendint32(&row_description_buf, (int) ptype);
	}
	pq_endmessage_reuse(&row_description_buf);

	/*
	 * 接着发送 RowDescription 或 NoData 描述结果……   
	 */
	if (psrc->resultDesc)
	{
		List	   *tlist;

		/* 获取该计划的主目标列表 */
		tlist = CachedPlanGetTargetList(psrc, NULL);

		SendRowDescriptionMessage(&row_description_buf,
								  psrc->resultDesc,
								  tlist,
								  NULL);
	}
	else
		pq_putemptymessage('n');	/* NoData */
}

/*
 * exec_describe_portal_message
 *
 * 处理针对门户的 "Describe" 消息  
 */
static void
exec_describe_portal_message(const char *portal_name)
{
	Portal		portal;

	/*
	 * 启动一个事务命令。（注意这通常会改变当前内存  
	 * 上下文。）若已处于事务中则什么都不做。         
	 */
	start_xact_command();

	/* 切换回消息上下文       */
	MemoryContextSwitchTo(MessageContext);

	portal = GetPortalByName(portal_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("portal \"%s\" does not exist", portal_name)));

	/*
	 * 若处于事务中止状态，就不能运行       
	 * SendRowDescriptionMessage()，因为需要访问系统目录。  
	 * 因此，拒绝 Describe 返回数据的门户。（不应             
	 * 拒绝所有 Describe，因为那可能破坏某些客户端    
	 * 发出 COMMIT 或 ROLLBACK 命令的能力，如果它们的       
	 * 代码盲目 Describe 一切。）  
	 */
	if (IsAbortedTransactionBlockState() &&
		portal->tupDesc)
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	if (whereToSendOutput != DestRemote)
		return;					/* 实际上做不了什么      */

	if (portal->tupDesc)
		SendRowDescriptionMessage(&row_description_buf,
								  portal->tupDesc,
								  FetchPortalTargetList(portal),
								  portal->formats);
	else
		pq_putemptymessage('n');	/* NoData */
}


/*
 * 用于启动/提交单个命令的便捷例程。             
 */
static void
start_xact_command(void)
{
	if (!xact_started)
	{
		StartTransactionCommand();

		xact_started = true;
	}

	/*
	 * 必要时启动语句超时。注意，这里有意不在已启动的 
	 * 超时上重置时钟，以避免在 start_xact_command() 被         
	 * 反复调用且中间没有 finish_xact_command() 时（如         
	 * parse/bind/execute）产生计时开销。如果不希望如此，     
	 * 必须显式禁用该超时。                         
	 */
	enable_statement_timeout();

	/* 必要时为检查客户端是否已消失而启动超时。         */
	if (client_connection_check_interval > 0 &&
		IsUnderPostmaster &&
		MyProcPort &&
		!get_timeout_active(CLIENT_CONNECTION_CHECK_TIMEOUT))
		enable_timeout_after(CLIENT_CONNECTION_CHECK_TIMEOUT,
							 client_connection_check_interval);
}

static void
finish_xact_command(void)
{
	/* 在每条命令之后取消活跃的语句超时   */
	disable_statement_timeout();

	if (xact_started)
	{
		CommitTransactionCommand();

#ifdef MEMORY_CONTEXT_CHECKING
		/* 检查提交期间未被释放的所有内存上下文     */
		/* （被释放的那些在删除前已检查过）     */
		MemoryContextCheck(TopMemoryContext);
#endif

#ifdef SHOW_MEMORY_STATS
		/* 每次提交后打印内存统计以追踪泄漏    */
		MemoryContextStats(TopMemoryContext);
#endif

		xact_started = false;
	}
}


/*
 * 用于检查某语句是否属于我们在事务中止状态下    
 * 仍允许的那些语句的便捷例程。      
 */

/* 测试裸解析树    */
static bool
IsTransactionExitStmt(Node *parsetree)
{
	if (parsetree && IsA(parsetree, TransactionStmt))
	{
		TransactionStmt *stmt = (TransactionStmt *) parsetree;

		if (stmt->kind == TRANS_STMT_COMMIT ||
			stmt->kind == TRANS_STMT_PREPARE ||
			stmt->kind == TRANS_STMT_ROLLBACK ||
			stmt->kind == TRANS_STMT_ROLLBACK_TO)
			return true;
	}
	return false;
}

/* 测试包含 PlannedStmt 节点的列表    */
static bool
IsTransactionExitStmtList(List *pstmts)
{
	if (list_length(pstmts) == 1)
	{
		PlannedStmt *pstmt = linitial_node(PlannedStmt, pstmts);

		if (pstmt->commandType == CMD_UTILITY &&
			IsTransactionExitStmt(pstmt->utilityStmt))
			return true;
	}
	return false;
}

/* 测试包含 PlannedStmt 节点的列表    */
static bool
IsTransactionStmtList(List *pstmts)
{
	if (list_length(pstmts) == 1)
	{
		PlannedStmt *pstmt = linitial_node(PlannedStmt, pstmts);

		if (pstmt->commandType == CMD_UTILITY &&
			IsA(pstmt->utilityStmt, TransactionStmt))
			return true;
	}
	return false;
}

/* 释放任何已存在的未命名预备语句   */
static void
drop_unnamed_stmt(void)
{
	/* 出于谨慎，避免出错时留下悬空指针      */
	if (unnamed_stmt_psrc)
	{
		CachedPlanSource *psrc = unnamed_stmt_psrc;

		unnamed_stmt_psrc = NULL;
		DropCachedPlan(psrc);
	}
}


/* --------------------------------
 *		PostgresMain() 中使用的信号处理例程 
 * --------------------------------
 */

/*
 * postmaster 发来 SIGQUIT 信号时调用 quickdie()。   
 *
 * 要么是某个后端已经崩溃，要么是我们被告知            
 * "立即"关闭；因此需停止手头工作并退出。    
 */
void
quickdie(SIGNAL_ARGS)
{
	sigaddset(&BlockSig, SIGQUIT);	/* 防止嵌套调用   */
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	/*
	 * 退出期间屏蔽中断；虽然我们刚刚阻塞了会排队      
	 * 新中断的信号，但可能已有中断挂起。我们不希望     
	 * quickdie() 被降为普通查询取消。    
	 */
	HOLD_INTERRUPTS();

	/*
	 * 若正在客户端认证中中止，勿冒险向客户端      
	 * 发送任何内容；那样很可能违反协议，更不用说    
	 * 我们可能已打断了 OpenSSL 或某个认证库的          
	 * 内部。              
	 */
	if (ClientAuthInProgress && whereToSendOutput == DestRemote)
		whereToSendOutput = DestNone;

	/*
	 * 退出前通知客户端，给出发生了什么的线索。      
	 *
	 * 在信号处理器中调用 ereport() 是有疑问的。它当然     
	 * 不是异步信号安全的。但尝试一下似乎好过骤然       
	 * 断开连接并让客户端疑惑发生了什么。在尝试发送   
	 * 消息时我们崩溃或挂起是有可能的，但收到           
	 * SIGQUIT 就表明已经出了严重问题，所以没什么        
	 * 可损失的。假设 postmaster 仍在运行，若我们因         
	 * 某种原因卡住，它很快就会 SIGKILL 我们。           
	 *
	 * 为更安全一点，我们能做的一件事是清空错误      
	 * 上下文栈，这样就不会调用上下文回调。这样可达    
	 * 到的代码少了很多，而且上下文信息无论如何都       
	 * 不太可能与 SIGQUIT 报告相关。        
	 */
	error_context_stack = NULL;

	/*
	 * 响应 postmaster 发出的信号时，我们只把消息发给        
	 * 客户端；发到服务器日志只会制造日志垃圾，而且  
	 * 信号处理器中需要指望能工作的代码也更多。      
	 *
	 * 理想情况下这些应是 ereport(FATAL)，但那样我们         
	 * 就无法重获控制以强制正确退出。  
	 */
	switch (GetQuitSignalReason())
	{
		case PMQUIT_NOT_SENT:
			/* 嗯，SIGQUIT 突然不期而至     */
			ereport(WARNING,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection because of unexpected SIGQUIT signal")));
			break;
		case PMQUIT_FOR_CRASH:
			/* 一次崩溃-重启循环正在进行中 */
			ereport(WARNING_CLIENT_ONLY,
					(errcode(ERRCODE_CRASH_SHUTDOWN),
					 errmsg("terminating connection because of crash of another server process"),
					 errdetail("The postmaster has commanded this server process to roll back"
							   " the current transaction and exit, because another"
							   " server process exited abnormally and possibly corrupted"
							   " shared memory."),
					 errhint("In a moment you should be able to reconnect to the"
							 " database and repeat your command.")));
			break;
		case PMQUIT_FOR_STOP:
			/* 立即模式停止  */
			ereport(WARNING_CLIENT_ONLY,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection due to immediate shutdown command")));
			break;
	}

	/*
	 * 我们绝不希望运行 proc_exit() 或 atexit() 回调，          
	 * 我们落到这里是因为共享内存可能已损坏，所以   
	 * 我们不想再尝试清理事务。钉死窗户，赶紧离开。  
	 * 反正从信号处理器中运行这些回调也不安全。       
	 *        
	 *
	 * 注意我们执行 _exit(2) 而非 _exit(0)。这是为了在          
	 * 有人向随机后端手工发送 SIGQUIT 时，迫使            
	 * postmaster 进入系统重置循环。正是因为我们不        
	 * 清理共享内存状态，这才必须如此。（pmsignal.c       
	 * 中的"死人开关"机制确保 postmaster 亦将此视为崩溃，  
	 * 但复核无妨） 
	 */
	_exit(2);
}

/*
 * 来自 postmaster 的关闭信号：中止事务，        
 * 在方便时尽早退出  
 */
void
die(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/* 别妨碍 proc_exit 的工作       */
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		ProcDiePending = true;
	}

	/* 供累积统计系统使用     */
	pgStatSessionEndCause = DISCONNECT_KILLED;

	/* 若仍在此处，唤醒任何等待进程锁存器的任务     */
	SetLatch(MyLatch);

	/*
	 * 若处于单用户模式，我们想立即退出：我们无法     
	 * 依赖锁存器，因为当 stdin/stdout 是文件时它们无法 
	 * 工作。这相当丑陋，但仅仅为了单用户模式的利益 
	 * 而投入更多精力恐怕不值得。         
	 */
	if (DoingCommandRead && whereToSendOutput != DestRemote)
		ProcessInterrupts();

	errno = save_errno;
}

/*
 * 来自 postmaster 的查询取消信号：在合适时机     
 * 中止当前事务        
 */
void
StatementCancelHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/*
	 * 别妨碍 proc_exit 的工作      
	 */
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		QueryCancelPending = true;
	}

	/* 若仍在此处，唤醒任何等待进程锁存器的任务     */
	SetLatch(MyLatch);

	errno = save_errno;
}

/* 浮点异常的信号处理器              */
void
FloatExceptionHandler(SIGNAL_ARGS)
{
	/* 我们不返回，因此无须保存 errno    */
	ereport(ERROR,
			(errcode(ERRCODE_FLOATING_POINT_EXCEPTION),
			 errmsg("floating-point exception"),
			 errdetail("An invalid floating-point operation was signaled. "
					   "This probably means an out-of-range result or an "
					   "invalid operation, such as division by zero.")));
}

/*
 * RecoveryConflictInterrupt：恢复冲突处理中在收到         
 * SIGUSR1 之后执行的外部部分。设计上类似于 die()        
 * 和 StatementCancelHandler()。仅由在恢复期间开始        
 * 事务的普通用户后端调用。      
 */
void
RecoveryConflictInterrupt(ProcSignalReason reason)
{
	int			save_errno = errno;

	/*
	 * 别妨碍 proc_exit 的工作      
	 */
	if (!proc_exit_inprogress)
	{
		RecoveryConflictReason = reason;
		switch (reason)
		{
			case PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK:

				/*
				 * 若未在等待锁，则绝不可能死锁。         
				 */
				if (!IsWaitingForLock())
					return;

				/* Intentional fall through to check wait for pin */
				/* FALLTHROUGH */

			case PROCSIG_RECOVERY_CONFLICT_BUFFERPIN:

				/*
				 * 若请求了 PROCSIG_RECOVERY_CONFLICT_BUFFERPIN          
				 * 但我们并未阻塞启动进程，那就无事         
				 * 可做
				 *
				 * 当请求 PROCSIG_RECOVERY_CONFLICT_STARTUP_      
				 * DEADLOCK 时，若我们在等锁且启动进程未  
				 * 等待 buffer pin（也在等锁），则设标志让    
				 * ProcSleep() 检查是否死锁。                         
				 *               
				 */
				if (!HoldingBufferPinThatDelaysRecovery())
				{
					if (reason == PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK &&
						GetStartupBufferPinWaitBufId() < 0)
						CheckDeadLockAlert();
					return;
				}

				MyProc->recoveryConflictPending = true;

				/* Intentional fall through to error handling */
				/* FALLTHROUGH */

			case PROCSIG_RECOVERY_CONFLICT_LOCK:
			case PROCSIG_RECOVERY_CONFLICT_TABLESPACE:
			case PROCSIG_RECOVERY_CONFLICT_SNAPSHOT:

				/*
				 * 如果我们已不再处于事务中，则忽略。  
				 */
				if (!IsTransactionOrTransactionBlock())
					return;

				/*
				 * 若只能中止当前子事务，那么抛出 ERROR 来    
				 * 解决冲突即可。否则落入 FATAL 情形。      
				 *                                
				 *
				 * XXX 其他可只抛 ERROR 的情形*可能*是          
				 * PROCSIG_RECOVERY_CONFLICT_LOCK（若父事务         
				 * 未持锁）。    
				 *
				 * PROCSIG_RECOVERY_CONFLICT_SNAPSHOT（若父事务未       
				 * 持有快照且事务并非事务快照模式） 
				 *                          
				 *
				 * PROCSIG_RECOVERY_CONFLICT_TABLESPACE（若父事务     
				 * 中无临时文件或游标）     
				 */
				if (!IsSubTransaction())
				{
					/*
					 * 若我们已经中止，则不再需要取消。       
					 * 在此处这样做，因为我们不希望忽略已   
					 * 中止的子事务，当前必须引发 FATAL。   
					 */
					if (IsAbortedTransactionBlockState())
						return;

					RecoveryConflictPending = true;
					QueryCancelPending = true;
					InterruptPending = true;
					break;
				}

				/* Intentional fall through to session cancel */
				/* FALLTHROUGH */

			case PROCSIG_RECOVERY_CONFLICT_DATABASE:
				RecoveryConflictPending = true;
				ProcDiePending = true;
				InterruptPending = true;
				break;

			case PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT:
				RecoveryConflictPending = true;
				QueryCancelPending = true;
				InterruptPending = true;
				break;

			default:
				elog(FATAL, "unrecognized conflict mode: %d",
					 (int) reason);
		}

		Assert(RecoveryConflictPending && (QueryCancelPending || ProcDiePending));

		/*
		 * 除数据库外的所有冲突都造成动态错误，这类    
		 * 冲突中命令或事务可在稍后重试并有望成功。    
		 * 无须重置此值，因为不可重试的冲突错误当前     
		 * 都是 FATAL。                     
		 */
		if (reason == PROCSIG_RECOVERY_CONFLICT_DATABASE)
			RecoveryConflictRetryable = false;
	}

	/*
	 * 设置进程锁存器。本函数本质上是在模拟 die()    
	 * 和 StatementCancelHandler() 之类的信号处理器，             
	 * 故行为类似较为稳妥。 
	 */
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * ProcessInterrupts：CHECK_FOR_INTERRUPTS() 宏的外部部分         
 *
 * 若中断条件挂起且可安全处理，则清除标志并      
 * 接受该中断。仅在 InterruptPending 为真时调用。   
 *                          
 *
 * 注意：若 INTERRUPTS_CAN_BE_PROCESSED() 为真，则               
 * ProcessInterrupts 保证在返回前清除 InterruptPending       
 * 标志。（这并不等同于保证我们返回时它仍被清    
 * 除；可能有新的中断到来。但我们承诺任何先前   
 * 已存在的中断都已被处理。）       
 */
void
ProcessInterrupts(void)
{
	/* 现在可接受任何中断吗？*/
	if (InterruptHoldoffCount != 0 || CritSectionCount != 0)
		return;
	InterruptPending = false;

	if (ProcDiePending)
	{
		ProcDiePending = false;
		QueryCancelPending = false; /* ProcDie 优先 QueryCancel */
		LockErrorCleanup();
		/* 与 quickdie 一样，认证期间勿冒险发送        */
		if (ClientAuthInProgress && whereToSendOutput == DestRemote)
			whereToSendOutput = DestNone;
		if (ClientAuthInProgress)
			ereport(FATAL,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling authentication due to timeout")));
		else if (IsAutoVacuumWorkerProcess())
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating autovacuum process due to administrator command")));
		else if (IsLogicalWorker())
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating logical replication worker due to administrator command")));
		else if (IsLogicalLauncher())
		{
			ereport(DEBUG1,
					(errmsg_internal("logical replication launcher shutting down")));

			/*
			 * 逻辑复制启动器可在任何时候被停止。         
			 * 使用退出状态 1 让后台工作进程重启。     
			 */
			proc_exit(1);
		}
		else if (RecoveryConflictPending && RecoveryConflictRetryable)
		{
			pgstat_report_recovery_conflict(RecoveryConflictReason);
			ereport(FATAL,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("terminating connection due to conflict with recovery"),
					 errdetail_recovery_conflict()));
		}
		else if (RecoveryConflictPending)
		{
			/* 目前只有一种不可重试的恢复冲突               */
			Assert(RecoveryConflictReason == PROCSIG_RECOVERY_CONFLICT_DATABASE);
			pgstat_report_recovery_conflict(RecoveryConflictReason);
			ereport(FATAL,
					(errcode(ERRCODE_DATABASE_DROPPED),
					 errmsg("terminating connection due to conflict with recovery"),
					 errdetail_recovery_conflict()));
		}
		else if (IsBackgroundWorker)
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating background worker \"%s\" due to administrator command",
							MyBgworkerEntry->bgw_type)));
		else
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection due to administrator command")));
	}

	if (CheckClientConnectionPending)
	{
		CheckClientConnectionPending = false;

		/*
		 * 检查连接是否丢失，若仍有配置则重新激活，      
		 * 但如果已回到 DoingCommandRead 状态则不这样做。      
		 * 我们不想唤醒空闲会话，其已能检测连接丢失   
		 *             
		 */
		if (!DoingCommandRead && client_connection_check_interval > 0)
		{
			if (!pq_check_connection())
				ClientConnectionLost = true;
			else
				enable_timeout_after(CLIENT_CONNECTION_CHECK_TIMEOUT,
									 client_connection_check_interval);
		}
	}

	if (ClientConnectionLost)
	{
		QueryCancelPending = false; /* 连接丢失优先 QueryCancel     */
		LockErrorCleanup();
		/* 不向客户端发送，我们已经知道连接已死。        */
		whereToSendOutput = DestNone;
		ereport(FATAL,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("connection to client lost")));
	}

	/*
	 * 如果在等待客户端输入期间发生恢复冲突，那么       
	 * 客户端多半只是闲坐在事务中，阻碍恢复取得        
	 * 进展。终止该连接以把它清除掉。                         
	 *             
	 */
	if (RecoveryConflictPending && DoingCommandRead)
	{
		QueryCancelPending = false; /* 它优先 QueryCancel   */
		RecoveryConflictPending = false;
		LockErrorCleanup();
		pgstat_report_recovery_conflict(RecoveryConflictReason);
		ereport(FATAL,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("terminating connection due to conflict with recovery"),
				 errdetail_recovery_conflict(),
				 errhint("In a moment you should be able to reconnect to the"
						 " database and repeat your command.")));
	}

	/*
	 * 从客户端读取输入时不允许查询取消中断，因为 
	 * 我们可能在 FE/BE 协议中失去同步。（Die 中断    
	 * 没有问题，因为那种情况下我们不会再从客户端       
	 * 读取消息。）   
	 */
	if (QueryCancelPending && QueryCancelHoldoffCount != 0)
	{
		/*
		 * 重新设置 InterruptPending，以便在读完消息后立即  
		 * 处理取消请求。（XXX 这相当丑陋：它使           
		 * INTERRUPTS_CAN_BE_PROCESSED() 变得复杂，而且意味          
		 * 着我们不能把该宏直接用作本函数的初始测试，    
		 * 这也意味着该代码同样可能引入其他缺陷。）       
		 *         
		 */
		InterruptPending = true;
	}
	else if (QueryCancelPending)
	{
		bool		lock_timeout_occurred;
		bool		stmt_timeout_occurred;

		QueryCancelPending = false;

		/*
		 * 若 LOCK_TIMEOUT 与 STATEMENT_TIMEOUT 指示都已设置，     
		 * 两者都须清除，故总取两者。  
		 */
		lock_timeout_occurred = get_timeout_indicator(LOCK_TIMEOUT, true);
		stmt_timeout_occurred = get_timeout_indicator(STATEMENT_TIMEOUT, true);

		/*
		 * 若两者都已设置，我们希望报告先完成的那个   
		 * 超时；这样即便机器慢到第二个超时在我们到达 
		 * 之前就触发，行为也能保持一致。平手时武断地   
		 * 选择报告锁超时。                                   
		 */
		if (lock_timeout_occurred && stmt_timeout_occurred &&
			get_timeout_finish_time(STATEMENT_TIMEOUT) < get_timeout_finish_time(LOCK_TIMEOUT))
			lock_timeout_occurred = false;	/* 报告语句超时  */

		if (lock_timeout_occurred)
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("canceling statement due to lock timeout")));
		}
		if (stmt_timeout_occurred)
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling statement due to statement timeout")));
		}
		if (IsAutoVacuumWorkerProcess())
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling autovacuum task")));
		}
		if (RecoveryConflictPending)
		{
			RecoveryConflictPending = false;
			LockErrorCleanup();
			pgstat_report_recovery_conflict(RecoveryConflictReason);
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("canceling statement due to conflict with recovery"),
					 errdetail_recovery_conflict()));
		}

		/*
		 * 如果我们正在从客户端读取命令，就直接忽略该    
		 * 取消请求，再发一条错误消息无用。否则就  
		 * 继续抛出该错误。                           
		 */
		if (!DoingCommandRead)
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling statement due to user request")));
		}
	}

	if (IdleInTransactionSessionTimeoutPending)
	{
		/*
		 * 若该 GUC 已重置为零，则忽略该信号。这一      
		 * 点很重要，因为 GUC 更新本身不会禁用任何挂起   
		 * 中断。 
		 */
		if (IdleInTransactionSessionTimeout > 0)
			ereport(FATAL,
					(errcode(ERRCODE_IDLE_IN_TRANSACTION_SESSION_TIMEOUT),
					 errmsg("terminating connection due to idle-in-transaction timeout")));
		else
			IdleInTransactionSessionTimeoutPending = false;
	}

	if (IdleSessionTimeoutPending)
	{
		/* 同上，若该 GUC 已重置为零，则忽略该信号。    */
		if (IdleSessionTimeout > 0)
			ereport(FATAL,
					(errcode(ERRCODE_IDLE_SESSION_TIMEOUT),
					 errmsg("terminating connection due to idle-session timeout")));
		else
			IdleSessionTimeoutPending = false;
	}

	/*
	 * 若存在待处理的统计更新，且当前确实空闲         
	 * （与 PostgresMain() 相符时），立即上报统计。   
	 */
	if (IdleStatsUpdateTimeoutPending &&
		DoingCommandRead && !IsTransactionOrTransactionBlock())
	{
		IdleStatsUpdateTimeoutPending = false;
		pgstat_report_stat(true);
	}

	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	if (ParallelMessagePending)
		HandleParallelMessages();

	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();

	if (ParallelApplyMessagePending)
		HandleParallelApplyMessages();
}

/*
 * set_stack_base：设置栈深检查的参考点                
 *
 * 返回旧参考点（若有）。       
 */
pg_stack_base_t
set_stack_base(void)
{
#ifndef HAVE__BUILTIN_FRAME_ADDRESS
	char		stack_base;
#endif
	pg_stack_base_t old;

	old = stack_base_ptr;

	/*
	 * 设置栈深检查的参考点。在较新的 gcc 上，我们用     
	 * __builtin_frame_address() 来避免把局部变量地址          
	 * 存入长寿命变量而引发的警告。  
	 */
#ifdef HAVE__BUILTIN_FRAME_ADDRESS
	stack_base_ptr = __builtin_frame_address(0);
#else
	stack_base_ptr = &stack_base;
#endif

	return old;
}

/*
 * restore_stack_base：恢复栈深检查的参考点                 
 *
 * 可在 set_stack_base() 之后用它恢复旧值。                   
 * 目前仅在 PL/Java 中使用。当 PL/Java 从另一线程            
 * 调用后端函数时，该线程的栈位置与主线程栈不同，    
 * 因此它在调用前设置基指针，调用之后再恢复。          
 *                        
 */
void
restore_stack_base(pg_stack_base_t base)
{
	stack_base_ptr = base;
}

/*
 * check_stack_depth/stack_is_too_deep：检查过深的递归              
 *
 * 任何可能递归得足够深以致溢出栈的递归例程，都应在    
 * 其中某处调用本检查。多数 Unix 把栈溢出视为       
 * 不可恢复的 SIGSEGV，因此我们希望在触及硬件            
 * 限制之前自行报错退出。 
 *
 * check_stack_depth() 只是简单直接地抛出错误。                
 * stack_is_too_deep() 则供想自行处理该错误的代码使用。
 */
void
check_stack_depth(void)
{
	if (stack_is_too_deep())
	{
		ereport(ERROR,
				(errcode(ERRCODE_STATEMENT_TOO_COMPLEX),
				 errmsg("stack depth limit exceeded"),
				 errhint("Increase the configuration parameter \"max_stack_depth\" (currently %dkB), "
						 "after ensuring the platform's stack depth limit is adequate.",
						 max_stack_depth)));
	}
}

bool
stack_is_too_deep(void)
{
	char		stack_top_loc;
	long		stack_depth;

	/*
	 * 计算从参考点到局部变量的距离                 
	 */
	stack_depth = (long) (stack_base_ptr - &stack_top_loc);

	/*
	 * 取绝对值，因为有的机器栈向上生长，有的向下      
	 */
	if (stack_depth < 0)
		stack_depth = -stack_depth;

	/*
	 * 异常  
	 *
	 * 对 stack_base_ptr 的测试可防止在进程建立期间或     
	 * 非后端进程中调用时便报错退出。按理说该检查       
	 * 本应最先执行，但放在此处可省去常规情形下的       
	 * 开销
	 */
	if (stack_depth > max_stack_depth_bytes &&
		stack_base_ptr != NULL)
		return true;

	return false;
}

/* max_stack_depth GUC 检查钩子   */
bool
check_max_stack_depth(int *newval, void **extra, GucSource source)
{
	long		newval_bytes = *newval * 1024L;
	long		stack_rlimit = get_stack_depth_rlimit();

	if (stack_rlimit > 0 && newval_bytes > stack_rlimit - STACK_DEPTH_SLOP)
	{
		GUC_check_errdetail("\"max_stack_depth\" must not exceed %ldkB.",
							(stack_rlimit - STACK_DEPTH_SLOP) / 1024L);
		GUC_check_errhint("Increase the platform's stack depth limit via \"ulimit -s\" or local equivalent.");
		return false;
	}
	return true;
}

/* max_stack_depth 的 GUC 赋值钩子*/
void
assign_max_stack_depth(int newval, void *extra)
{
	long		newval_bytes = newval * 1024L;

	max_stack_depth_bytes = newval_bytes;
}

/*
 * client_connection_check_interval GUC 检查钩子  
 */
bool
check_client_connection_check_interval(int *newval, void **extra, GucSource source)
{
	if (!WaitEventSetCanReportClosed() && *newval != 0)
	{
		GUC_check_errdetail("client_connection_check_interval must be set to 0 on this platform.");
		return false;
	}
	return true;
}

/*
 * log_parser_stats、log_planner_stats、log_executor_stats 的 GUC 钩子  
 *
 * 本函数与 check_log_stats 相互配合，防止这些变量被         
 * 设置成不允许的组合。这是一个并不能真正奏效的        
 * 补丁；例如在应用 pg_db_role_setting 的值时它可能         
 * 失败，即便最终状态本可接受。不过，由于这些变量   
 * 是遗留设置，生产使用极少，我们对此予以容忍。     
 *                  
 */
bool
check_stage_log_stats(bool *newval, void **extra, GucSource source)
{
	if (*newval && log_statement_stats)
	{
		GUC_check_errdetail("Cannot enable parameter when \"log_statement_stats\" is true.");
		return false;
	}
	return true;
}

/*
 * log_statement_stats GUC 检查钩子  
 */
bool
check_log_stats(bool *newval, void **extra, GucSource source)
{
	if (*newval &&
		(log_parser_stats || log_planner_stats || log_executor_stats))
	{
		GUC_check_errdetail("Cannot enable \"log_statement_stats\" when "
							"\"log_parser_stats\", \"log_planner_stats\", "
							"or \"log_executor_stats\" is true.");
		return false;
	}
	return true;
}

/*
 * restrict_nonsystem_relation_kind GUC 检查钩子  
 */
bool
check_restrict_nonsystem_relation_kind(char **newval, void **extra, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	int			flags = 0;

	/* 需要字符串的可修改副本*/
	rawstring = pstrdup(*newval);

	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* 列表语法错误   */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);

		if (pg_strcasecmp(tok, "view") == 0)
			flags |= RESTRICT_RELKIND_VIEW;
		else if (pg_strcasecmp(tok, "foreign-table") == 0)
			flags |= RESTRICT_RELKIND_FOREIGN_TABLE;
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

	/* 把标志保存在 *extra 中，供赋值函数使用    */
	*extra = guc_malloc(ERROR, sizeof(int));
	*((int *) *extra) = flags;

	return true;
}

/*
 * restrict_nonsystem_relation_kind GUC 赋值钩子   
 */
void
assign_restrict_nonsystem_relation_kind(const char *newval, void *extra)
{
	int		   *flags = (int *) extra;

	restrict_nonsystem_relation_kind = *flags;
}

/*
 * set_debug_options --- 处理 "-d N" 命令行选项   
 *
 * -d 与设置 log_min_messages 不完全相同，它会启用           
 * 其他输出选项。
 */
void
set_debug_options(int debug_flag, GucContext context, GucSource source)
{
	if (debug_flag > 0)
	{
		char		debugstr[64];

		sprintf(debugstr, "debug%d", debug_flag);
		SetConfigOption("log_min_messages", debugstr, context, source);
	}
	else
		SetConfigOption("log_min_messages", "notice", context, source);

	if (debug_flag >= 1 && context == PGC_POSTMASTER)
	{
		SetConfigOption("log_connections", "true", context, source);
		SetConfigOption("log_disconnections", "true", context, source);
	}
	if (debug_flag >= 2)
		SetConfigOption("log_statement", "all", context, source);
	if (debug_flag >= 3)
		SetConfigOption("debug_print_parse", "true", context, source);
	if (debug_flag >= 4)
		SetConfigOption("debug_print_plan", "true", context, source);
	if (debug_flag >= 5)
		SetConfigOption("debug_print_rewritten", "true", context, source);
}


bool
set_plan_disabling_options(const char *arg, GucContext context, GucSource source)
{
	const char *tmp = NULL;

	switch (arg[0])
	{
		case 's':				/* seqscan */
			tmp = "enable_seqscan";
			break;
		case 'i':				/* indexscan */
			tmp = "enable_indexscan";
			break;
		case 'o':				/* indexonlyscan */
			tmp = "enable_indexonlyscan";
			break;
		case 'b':				/* bitmapscan */
			tmp = "enable_bitmapscan";
			break;
		case 't':				/* tidscan */
			tmp = "enable_tidscan";
			break;
		case 'n':				/* nestloop */
			tmp = "enable_nestloop";
			break;
		case 'm':				/* mergejoin */
			tmp = "enable_mergejoin";
			break;
		case 'h':				/* hashjoin */
			tmp = "enable_hashjoin";
			break;
	}
	if (tmp)
	{
		SetConfigOption(tmp, "false", context, source);
		return true;
	}
	else
		return false;
}


const char *
get_stats_option_name(const char *arg)
{
	switch (arg[0])
	{
		case 'p':
			if (optarg[1] == 'a')	/* "parser" */
				return "log_parser_stats";
			else if (optarg[1] == 'l')	/* "planner" */
				return "log_planner_stats";
			break;

		case 'e':				/* "executor" */
			return "log_executor_stats";
			break;
	}

	return NULL;
}


/* ----------------------------------------------------------------
 * process_postgres_switches
 *	   解析后端的命令行参数           
 *
 * 该函数被调用两次：一次处理来自 postmaster 或命令 
 * 行的"安全"选项，另一次处理来自客户端启动包的     
 * "不安全"选项；后者的语法相同，                          
 * 但能做的事可能受到限制。  
 *
 * 两种情形下都忽略 argv[0]（假定它是程序名）。          
 *
 * ctx 对安全选项取 PGC_POSTMASTER；对来自客户端的             
 * 不安全选项取 PGC_BACKEND；对来自超级用户客户端的        
 * PGC_SU_BACKEND。  
 *
 * 若命令行参数里含有数据库名，则将其返回到     
 * *dbname 中（仅当 *dbname 初始为 NULL 时才允许这样做）。   
 * ----------------------------------------------------------------
 */
void
process_postgres_switches(int argc, char *argv[], GucContext ctx,
						  const char **dbname)
{
	bool		secure = (ctx == PGC_POSTMASTER);
	int			errs = 0;
	GucSource	gucsource;
	int			flag;

	if (secure)
	{
		gucsource = PGC_S_ARGV; /* 选项来自命令行           */

		/* 若存在初始的 --single 参数，则忽略它*/
		if (argc > 1 && strcmp(argv[1], "--single") == 0)
		{
			argv++;
			argc--;
		}
	}
	else
	{
		gucsource = PGC_S_CLIENT;	/* 选项来自客户端     */
	}

#ifdef HAVE_INT_OPTERR

	/*
	 * 把它关掉：因为它要么打到 stderr 而非我们想要的  
	 * 日志，要么此时 argv[0] 已变成 "--single"，会导致        
	 * 奇怪的错误消息。我们在下面自行打印报错。 
	 */
	opterr = 0;
#endif

	/*
	 * 解析命令行选项。注意：保持与                  
	 * postmaster/postmaster.c（两者选项集不应冲突）以及        
	 * main/main.c 中的公共 help() 同步。 
	 */
	while ((flag = getopt(argc, argv, "B:bC:c:D:d:EeFf:h:ijk:lN:nOPp:r:S:sTt:v:W:-:")) != -1)
	{
		switch (flag)
		{
			case 'B':
				SetConfigOption("shared_buffers", optarg, ctx, gucsource);
				break;

			case 'b':
				/* 用于二进制升级的未公开标志    */
				if (secure)
					IsBinaryUpgrade = true;
				break;

			case 'C':
				/* 为与 postmaster 保持一致而忽略     */
				break;

			case 'c':
			case '-':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (flag == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optarg)));
					}
					SetConfigOption(name, value, ctx, gucsource);
					pfree(name);
					pfree(value);
					break;
				}

			case 'D':
				if (secure)
					userDoption = strdup(optarg);
				break;

			case 'd':
				set_debug_options(atoi(optarg), ctx, gucsource);
				break;

			case 'E':
				if (secure)
					EchoQuery = true;
				break;

			case 'e':
				SetConfigOption("datestyle", "euro", ctx, gucsource);
				break;

			case 'F':
				SetConfigOption("fsync", "false", ctx, gucsource);
				break;

			case 'f':
				if (!set_plan_disabling_options(optarg, ctx, gucsource))
					errs++;
				break;

			case 'h':
				SetConfigOption("listen_addresses", optarg, ctx, gucsource);
				break;

			case 'i':
				SetConfigOption("listen_addresses", "*", ctx, gucsource);
				break;

			case 'j':
				if (secure)
					UseSemiNewlineNewline = true;
				break;

			case 'k':
				SetConfigOption("unix_socket_directories", optarg, ctx, gucsource);
				break;

			case 'l':
				SetConfigOption("ssl", "true", ctx, gucsource);
				break;

			case 'N':
				SetConfigOption("max_connections", optarg, ctx, gucsource);
				break;

			case 'n':
				/* 为与 postmaster 一致而忽略       */
				break;

			case 'O':
				SetConfigOption("allow_system_table_mods", "true", ctx, gucsource);
				break;

			case 'P':
				SetConfigOption("ignore_system_indexes", "true", ctx, gucsource);
				break;

			case 'p':
				SetConfigOption("port", optarg, ctx, gucsource);
				break;

			case 'r':
				/* 把输出（stdout 和 stderr）送到指定文件*/
				if (secure)
					strlcpy(OutputFileName, optarg, MAXPGPATH);
				break;

			case 'S':
				SetConfigOption("work_mem", optarg, ctx, gucsource);
				break;

			case 's':
				SetConfigOption("log_statement_stats", "true", ctx, gucsource);
				break;

			case 'T':
				/* 为与 postmaster 保持一致而忽略     */
				break;

			case 't':
				{
					const char *tmp = get_stats_option_name(optarg);

					if (tmp)
						SetConfigOption(tmp, "true", ctx, gucsource);
					else
						errs++;
					break;
				}

			case 'v':

				/*
				 * -v 在正常运行中已不再使用，因为  
				 * 在此之前 FrontendProtocol 就已设置好。保留     
				 * 该开关仅供独立运行时可能用到，以防将来
				 * 会支持独立后端使用普通 FE/BE 协议。         
				 *                    
				 */
				if (secure)
					FrontendProtocol = (ProtocolVersion) atoi(optarg);
				break;

			case 'W':
				SetConfigOption("post_auth_delay", optarg, ctx, gucsource);
				break;

			default:
				errs++;
				break;
		}

		if (errs)
			break;
	}

	/*
	 * 可选的数据库名应仅在 *dbname 为 NULL 时才存在。
	 */
	if (!errs && dbname && *dbname == NULL && argc - optind >= 1)
		*dbname = strdup(argv[optind++]);

	if (errs || argc != optind)
	{
		if (errs)
			optind--;			/* 对前一个参数报错             */

		/* 根据上下文把错误消息的措辞稍作变化            */
		if (IsUnderPostmaster)
			ereport(FATAL,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("invalid command-line argument for server process: %s", argv[optind]),
					errhint("Try \"%s --help\" for more information.", progname));
		else
			ereport(FATAL,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("%s: invalid command-line argument: %s",
						   progname, argv[optind]),
					errhint("Try \"%s --help\" for more information.", progname));
	}

	/*
	 * 重置 getopt(3) 库，使其在子进程中、或本函数带着     
	 * 另一个数组被再次调用时也能正确工作。           
	 */
	optind = 1;
#ifdef HAVE_INT_OPTRESET
	optreset = 1;				/* 某些系统也需要这样*/
#endif
}


/*
 * PostgresSingleUserMain
 *     单用户模式的入口点。argc/argv 是要使用的命令行
 *     参数。            
 *
 * 先执行单用户特有的设置，然后调用 PostgresMain() 来      
 * 实际处理查询。单用户模式特有的设置应尽量放在     
 * 这里，而不是 PostgresMain() 或 InitPostgres() 中。    
 */
void
PostgresSingleUserMain(int argc, char *argv[],
					   const char *username)
{
	const char *dbname = NULL;

	Assert(!IsUnderPostmaster);

	/* 初始化启动进程环境。          */
	InitStandaloneProcess(argv[0]);

	/*
	 * 为命令行选项设置默认值。        
	 */
	InitializeGUCOptions();

	/*
	 * 解析命令行选项。   
	 */
	process_postgres_switches(argc, argv, PGC_POSTMASTER, &dbname);

	/* 必须已得到数据库名，或已有默认值（即用户名） */
	if (dbname == NULL)
	{
		dbname = username;
		if (dbname == NULL)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s: no database nor user name specified",
							progname)));
	}

	/* 获取配置参数               */
	if (!SelectConfigFiles(userDoption, progname))
		proc_exit(1);

	/*
	 * 校验给定的 DataDir 看起来是否合理，并切换到其中 
	 *         
	 */
	checkDataDir();
	ChangeToDataDir();

	/*
	 * 为数据目录创建锁文件。  
	 */
	CreateDataDirLockFile(false);

	/* 读取控制文件（含错误检查与配置信息）  */
	LocalProcessControlFile(false);

	/*
	 * 处理应当在 postmaster 启动时预加载的库               
	 */
	process_shared_preload_libraries();

	/* 初始化 MaxBackends  */
	InitializeMaxBackends();

	/*
	 * 给预加载的库一个请求额外共享内存的机会。          
	 */
	process_shmem_requests();

	/*
	 * 既然可加载模块已经获得了请求额外共享内存的      
	 * 机会，现在可以确定那些依赖所需共享内存量的     
	 * 运行时计算 GUC 的值。                  
	 */
	InitializeShmemGUCs();

	/*
	 * 既然模块已加载，现在可以处理 wal_consistency_checking  
	 * GUC 中指定的自定义资源管理器。            
	 */
	InitializeWalConsistencyChecking();

	CreateSharedMemoryAndSemaphores();

	/*
	 * 记住独立后端的启动时间，大致与 postmaster 记录的 
	 * 启动时点相同。                  
	 */
	PgStartTime = GetCurrentTimestamp();

	/*
	 * 在共享内存中为每个后端创建 PGPROC 结构体。必须先
	 * 完成才能用 LWLock。 
	 */
	InitProcess();

	/*
	 * 既然足够的基础设施已初始化完毕，PostgresMain() 便     
	 * 可完成其余 
	 */
	PostgresMain(dbname, username);
}


/* ----------------------------------------------------------------
 * PostgresMain
 *	   postgres 主循环——所有后端（交互与否）都在此循环 
 *
 * dbname 是要连接的数据库名，username 是会话所用的 
 * PostgreSQL 用户名。                         
 *
 * 注意：单用户模式特有的设置应尽量放进                   
 * PostgresSingleUserMain 
 * ----------------------------------------------------------------
 */
void
PostgresMain(const char *dbname, const char *username)
{
	sigjmp_buf	local_sigjmp_buf;

	/* 这些必须声明为 volatile，确保状态在 longjmp 后保留  */
	volatile bool send_ready_for_query = true;
	volatile bool idle_in_transaction_timeout_enabled = false;
	volatile bool idle_session_timeout_enabled = false;

	Assert(dbname != NULL);
	Assert(username != NULL);

	SetProcessingMode(InitProcessing);

	/*
	 * 设置信号处理器。（InitPostmasterChild 或                    
	 * InitStandaloneProcess 已设置好 BlockSig 为活动掩码。） 
	 *
	 * 注意 postmaster 在派生子进程前阻塞了所有信号，       
	 * 因此不存在我们在设置好处理器之前收到信号的       
	 * 竞态条件。            
	 *
	 * 另请注意：最好不使用任何在 postmaster 中被              
	 * SIG_IGN 忽略的信号。若此类信号在我们能把处理器     
	 * 改成非 SIG_IGN 之前到达，会被丢弃。替代做法是  
	 * 在 postmaster 中设置哑处理器来占住该信号。（当然，  
	 * 如 SIGALRM 和 SIGPIPE 这类本地生成的信号，这并非     
	 * 问题）
	 */
	if (am_walsender)
		WalSndSignals();
	else
	{
		pqsignal(SIGHUP, SignalHandlerForConfigReload);
		pqsignal(SIGINT, StatementCancelHandler);	/* 取消当前查询   */
		pqsignal(SIGTERM, die); /* 取消当前查询并退出   */

		/*
		 * 在 postmaster 子后端中，把 SignalHandlerForCrashExit     
		 * 替换为 quickdie，以便告知客户端将死。  
		 *
		 * 在独立后端中，SIGQUIT 很容易从键盘产生，而        
		 * SIGTERM 不能，所以让这两个信号都执行 die()      
		 * 而不是 quickdie()。
		 */
		if (IsUnderPostmaster)
			pqsignal(SIGQUIT, quickdie);	/* 硬崩溃时间 */
		else
			pqsignal(SIGQUIT, die); /* 取消当前查询并退出   */
		InitializeTimeouts();	/* 建立 SIGALRM 处理器    */

		/*
		 * 忽略向前端写失败。注意：若前端关闭连接， 
		 * 会在控制流下次返回外层循环时察觉并干净地    
		 * 退出。这似乎比在不知何种操作的输出中途强行  
		 * 退出更安全……                             
		 */
		pqsignal(SIGPIPE, SIG_IGN);
		pqsignal(SIGUSR1, procsignal_sigusr1_handler);
		pqsignal(SIGUSR2, SIG_IGN);
		pqsignal(SIGFPE, FloatExceptionHandler);

		/*
		 * 重置某些 postmaster 接受但后端不接受的          
		 * 信号 
		 */
		pqsignal(SIGCHLD, SIG_DFL); /* system() requires this on some
									 * platforms */
	}

	/* 早期初始化      */
	BaseInit();

	/* 在初始事务期间需要允许 SIGINT 等信号          */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * 通用初始化。     
	 *
	 * 注意：如果你想在这附近添加代码，请考虑放进        
	 * InitPostgres() 中。特别是任何涉及数据库访问的   
	 * 代码都应放在那里，而不是这里。      
	 */
	InitPostgres(dbname, InvalidOid,	/* 要连接的数据库  */
				 username, InvalidOid,	/* 连接所用角色 */
				 !am_walsender, /* 遵循 session_preload_libraries?*/
				 false,			/* 不忽略 datallowconn    */
				 NULL);			/* 无 out_dbname*/

	/*
	 * 若 PostmasterContext 仍然存在，就回收其空间；           
	 * InitPostgres 完成后就不再需要它。注意这不会破坏       
	 * *MyProcPort，因为 ConnCreate() 是用 malloc() 分配的         
	 * 那块空间，否则就得先复制 Port 数据。此外，诸如    
	 * 用户名等附属数据亦不丢失；见 ProcessStartupPacket(). 
	 */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	SetProcessingMode(NormalProcessing);

	/*
	 * 现在所有 GUC 状态均已设置完毕，如果合适就   
	 * 上报。   
	 */
	BeginReportingGUCOptions();

	/*
	 * 同时设置记录会话结束的处理器；必须等到现在，    
	 * 才能确知 Log_disconnections 的终值。
	 */
	if (IsUnderPostmaster && Log_disconnections)
		on_proc_exit(log_disconnections, 0);

	pgstat_report_connect(MyDatabaseId);

	/* 执行 WAL 发送进程特有的初始化。             */
	if (am_walsender)
		InitWalSender();

	/*
	 * 把本后端的取消信息发送给前端。         
	 */
	if (whereToSendOutput == DestRemote)
	{
		StringInfoData buf;

		pq_beginmessage(&buf, 'K');
		pq_sendint32(&buf, (int32) MyProcPid);
		pq_sendint32(&buf, (int32) MyCancelKey);
		pq_endmessage(&buf);
		/* 无需刷新，ReadyForQuery 会代为完成。*/
	}

	/* 独立运行时的欢迎横幅     */
	if (whereToSendOutput == DestDebug)
		printf("\nPostgreSQL stand-alone backend %s\n", PG_VERSION);

	/*
	 * 创建主循环中要使用的内存上下文。       
	 *
	 * MessageContext 每逢主循环迭代一次就会                     
	 * 被重置，即在处理完客户端每条命令消息后。     
	 */
	MessageContext = AllocSetContextCreate(TopMemoryContext,
										   "MessageContext",
										   ALLOCSET_DEFAULT_SIZES);

	/*
	 * 创建 RowDescription 消息所用的内存上下文与缓冲区。  
	 * SendRowDescriptionMessage() 经由 exec_describe_statement_message()  
	 * 会在每条语句上频繁执行，因此我们不想         
	 * 每次都分配单独的缓冲区。  
	 */
	row_description_context = AllocSetContextCreate(TopMemoryContext,
													"RowDescriptionContext",
													ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(row_description_context);
	initStringInfo(&row_description_buf);
	MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * POSTGRES 主处理循环由此开始     
	 *
	 * 若遇到异常，处理会回到这里，以便我们中止           
	 * 当前事务并开起新事务。       
	 *
	 * 你可能奇怪为何不写成围绕 PG_TRY 的无限循环。    
	 * 原因在于这里是异常栈的底部：若采用 PG_TRY，  
	 * 那么在 CATCH 阶段就完全不会有生效的异常处理器。    
	 * 保持最外层 setjmp 始终活跃，错误恢复期间我们         
	 * 至少还有一些从错误中复原的机会。                       
	 * （若因此陷入无限循环，                                   
	 * 它很快就会被 elog.c 内部状态栈的溢出所制止。）  
	 *
	 * 注意我们采用 sigsetjmp(..., 1)，本函数的信号掩码        
	 * （即 UnBlockSig）在 longjmp 跳回此处时会被恢复。       
	 * 这很关键：若在某平台从信号处理器中 longjmp 跳出，  
	 * 而信号仍处于阻塞状态，这并不与                     
	 * AbortTransaction() 里那个解除阻塞，因为后者只有在     
	 * 事务内时才调用。  
	 */

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/*
		 * 注意：若你想在此 if 块中添加更多代码，    
		 * 请考虑它很可能应该放进                 
		 * AbortTransaction() 中。这里直接完成的唯一工作    
		 * 应当是保证*只*适用于外层错误恢复的内容，       
		 * 比如调整 FE/BE 协议状态。                          
		 */

		/* 由于没用 PG_TRY，必须手工重置错误栈      */
		error_context_stack = NULL;

		/* 清理期间禁止中断             */
		HOLD_INTERRUPTS();

		/*
		 * 忘掉待处理的 QueryCancel 请求，反正我们要回到   
		 * 空闲循环，并取消所有活动中的超时请求。（将来
		 * 我们也许会想允许某些超时请求存续，但             
		 * 至少也得调用 reschedule_timeouts()，以防                
		 * 我们到达此处是因为查询取消中断了 SIGALRM      
		 * 中断处理程序。）请特别注意，我们必须清除 
		 * 语句与锁超时指示器，以免今后普通的查询取消   
		 * 被误报为超时（万一我们                             
		 * 忘记取消超时）。    
		 */
		disable_all_timeouts(false);	/* 先执行以免竞态条件      */
		QueryCancelPending = false;
		idle_in_transaction_timeout_enabled = false;
		idle_session_timeout_enabled = false;

		/* 不再从客户端读取。          */
		DoingCommandRead = false;

		/* 确保 libpq 处于良好状态    */
		pq_comm_reset();

		/* 把错误上报给客户端和/或服务器日志*/
		EmitErrorReport();

		/*
		 * 若 Valgrind 在出错查询期间注意到了什么，就打印   
		 * 查询字符串（若存在）。  
		 */
		valgrind_report_error_query(debug_query_string);

		/*
		 * 确保 debug_query_string 在我们可能破坏它指向的       
		 * 存储之前被重置。 
		 */
		debug_query_string = NULL;

		/*
		 * 中止当前事务以便恢复。                 
		 */
		AbortCurrentTransaction();

		if (am_walsender)
			WalSndErrorCleanup();

		PortalErrorCleanup();

		/*
		 * 我们无法在 AbortTransaction() 中释放复制槽，因为    
		 * 持有槽时仍需能开始并中止事务。但我们从不需要 
		 * 跨顶层错误持有它们，所以在这里释放即可。     
		 * 此外还有一个 before_shmem_exit() 回调                 
		 * 确保在 FATAL 错误时正确清理。          
		 */
		if (MyReplicationSlot != NULL)
			ReplicationSlotRelease();

		/* 出错时我们还想清理临时槽。           */
		ReplicationSlotCleanup();

		jit_reset_after_error();

		/*
		 * 现在回到正常顶层上下文，并清空 ErrorContext，    
		 * 下次。 
		 */
		MemoryContextSwitchTo(TopMemoryContext);
		FlushErrorState();

		/*
		 * 若我们当时正在处理扩展查询协议消息，则启动 
		 * 跳过直至下一个 Sync。这也让我们不发    
		 * ReadyForQuery（直至 Sync）。 
		 */
		if (doing_extended_query_message)
			ignore_till_sync = true;

		/* 我们不再有打开的事务命令了          */
		xact_started = false;

		/*
		 * 如果在从客户端读取消息期间发生了错误，    
		 * 我们可能已无法分辨前一条消息在哪里结束、
		 * 下一条消息从哪里开始。尽管在其他方面    
		 * 我们已从错误中恢复，但也无法再安全地读取      
		 * 来自客户端的消息了，所以对这个连接我们       
		 * 已无计可施。 
		 */
		if (pq_is_reading_msg())
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("terminating connection because protocol synchronization was lost")));

		/* 现在可以重新允许中断    */
		RESUME_INTERRUPTS();
	}

	/* 现在能处理 ereport(ERROR)   */
	PG_exception_stack = &local_sigjmp_buf;

	if (!ignore_till_sync)
		send_ready_for_query = true;	/* 初始时或出错后     */

	/*
	 * 无错查询在此循环。 
	 */

	for (;;)
	{
		int			firstchar;
		StringInfoData input_message;

		/*
		 * 在循环顶部重置扩展查询消息标志，使得        
		 * "空闲"状态下遇到的错误不会引发跳过。 
		 */
		doing_extended_query_message = false;

		/*
		 * 为 valgrind 报告起见，"当前查询"由此开始。        
		 */
#ifdef USE_VALGRIND
		old_valgrind_error_count = VALGRIND_COUNT_ERRORS;
#endif

		/*
		 * 释放上一次查询周期遗留的存储，并在清空后的   
		 * MessageContext 中新建查询输入缓冲区。 
		 */
		MemoryContextSwitchTo(MessageContext);
		MemoryContextResetAndDeleteChildren(MessageContext);

		initStringInfo(&input_message);

		/*
		 * 如有目录快照也考虑释放，这样等待客户端期间  
		 * 它不会阻碍全局 xmin 前进。                               
		 */
		InvalidateCatalogSnapshotConditionally();

		/*
		 * (1) 如果已到达空闲状态，就告知前端准备好        
		 * 新查询。
		 *
		 * 注意：包括对先前剩余输出执行 fflush()。        
		 *
		 * 这也是把已收集的统计信息刷入累计统计系统的  
		 * 好时机，同时更新 PS 统计显示。我们               
		 * 避免每轮消息循环都做这些事，因为那会拖慢      
		 * 成批消息的处理，而且我们不想报告未提交的       
		 * 更新（那会让 autovacuum 困惑）。通知              
		 * 处理器也想得到调用，若我们不在              
		 * 事务块中。   
		 *
		 * 另外，若启用空闲超时，则为其启动计时器。  
		 */
		if (send_ready_for_query)
		{
			if (IsAbortedTransactionBlockState())
			{
				set_ps_display("idle in transaction (aborted)");
				pgstat_report_activity(STATE_IDLEINTRANSACTION_ABORTED, NULL);

				/* 启动事务内空闲计时器      */
				if (IdleInTransactionSessionTimeout > 0)
				{
					idle_in_transaction_timeout_enabled = true;
					enable_timeout_after(IDLE_IN_TRANSACTION_SESSION_TIMEOUT,
										 IdleInTransactionSessionTimeout);
				}
			}
			else if (IsTransactionOrTransactionBlock())
			{
				set_ps_display("idle in transaction");
				pgstat_report_activity(STATE_IDLEINTRANSACTION, NULL);

				/* 启动事务内空闲计时器      */
				if (IdleInTransactionSessionTimeout > 0)
				{
					idle_in_transaction_timeout_enabled = true;
					enable_timeout_after(IDLE_IN_TRANSACTION_SESSION_TIMEOUT,
										 IdleInTransactionSessionTimeout);
				}
			}
			else
			{
				long		stats_timeout;

				/*
				 * 处理到来的通知（含自通知），如果有， 
				 * 并把相关消息发给客户端。在这里处理     
				 * 有助于让测试行为保持稳定：若有任何通知  
				 * 是在刚结束的事务期间收到的，客户端就会  
				 * 先于 ReadyForQuery 看到它们。          
				 */
				if (notifyInterruptPending)
					ProcessNotifyInterrupt(false);

				/*
				 * 检查是否需要上报统计。若 pgstat_report_stat()
				 * 判定刷新待处理统计为时尚早，或锁竞争 
				 * 阻止了上报，它就会告诉我们何时        
				 * 应再次尝试上报（这样统计更新就不会    
				 * 在连接长时间空闲时被过度拖延）。我们 
				 * 只在没有超时进行时才启用它，因为我们    
				 * 并不会在下面禁用它。enable_timeout_after()    
				 * 需要确定当前时间戳，这可能带来           
				 * 负面的性能影响。这无妨，因为      
				 * pgstat_report_stat() 不会让我们早于上次醒来。 
				 *                                                
				 */
				stats_timeout = pgstat_report_stat(false);
				if (stats_timeout > 0)
				{
					if (!get_timeout_active(IDLE_STATS_UPDATE_TIMEOUT))
						enable_timeout_after(IDLE_STATS_UPDATE_TIMEOUT,
											 stats_timeout);
				}
				else
				{
					/* 统计已全部刷完，无需该超时    */
					if (get_timeout_active(IDLE_STATS_UPDATE_TIMEOUT))
						disable_timeout(IDLE_STATS_UPDATE_TIMEOUT, false);
				}

				set_ps_display("idle");
				pgstat_report_activity(STATE_IDLE, NULL);

				/* 启动空闲会话计时器  */
				if (IdleSessionTimeout > 0)
				{
					idle_session_timeout_enabled = true;
					enable_timeout_after(IDLE_SESSION_TIMEOUT,
										 IdleSessionTimeout);
				}
			}

			/* 上报最近更改过的 GUC 选项     */
			ReportChangedGUCOptions();

			ReadyForQuery(whereToSendOutput);
			send_ready_for_query = false;
		}

		/*
		 * (2) 允许异步信号在我们等待客户端输入期间       
		 * 到来时立即得到执行。（这必须是有条件的，
		 * 因为我们不想让诸如 COPY FROM STDIN 的读取             
		 * 也做同样的事情。） 
		 */
		DoingCommandRead = true;

		/*
		 * (3) 读入命令，循环阻塞于此
		 */
		firstchar = ReadCommand(&input_message);

		/*
		 * (4) 若事务内空闲与空闲会话超时处于活动状态， 
		 * 就将其关闭。在第 (5) 步之前这样做是为了确保     
		 * 最后一刻的超时必被察觉。  
		 *
		 * 这些超时至多只有一个处于活动状态，所以无需    
		 * 费心把 timeout.c 的调用合并成一个。    
		 */
		if (idle_in_transaction_timeout_enabled)
		{
			disable_timeout(IDLE_IN_TRANSACTION_SESSION_TIMEOUT, false);
			idle_in_transaction_timeout_enabled = false;
		}
		if (idle_session_timeout_enabled)
		{
			disable_timeout(IDLE_SESSION_TIMEOUT, false);
			idle_session_timeout_enabled = false;
		}

		/*
		 * (5) 再次禁用异步信号条件。     
		 *
		 * 查询取消在没有查询进行时应当是空操作，       
		 * 所以如果查询取消在我们空闲时到达，只需      
		 * 重置 QueryCancelPending 即可。ProcessInterrupts() 在        
		 * DoingCommandRead 被设置时被调用就会产生这种效果，
		 * 故先检查中断再重置。    
		 */
		CHECK_FOR_INTERRUPTS();
		DoingCommandRead = false;

		/*
		 * (6) 检查我们睡眠期间发生的任何其他值得关注的 
		 * 事件
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * (7) 处理该命令。但若正在跳至 Sync 则忽略之。 
		 *      
		 */
		if (ignore_till_sync && firstchar != EOF)
			continue;

		switch (firstchar)
		{
			case 'Q':			/* 简单查询 */
				{
					const char *query_string;

					/* 设置 statement_timestamp*/
					SetCurrentStatementStartTimestamp();

					query_string = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					if (am_walsender)
					{
						if (!exec_replication_command(query_string))
							exec_simple_query(query_string);
					}
					else
						exec_simple_query(query_string);

					valgrind_report_error_query(query_string);

					send_ready_for_query = true;
				}
				break;

			case 'P':			/* parse */
				{
					const char *stmt_name;
					const char *query_string;
					int			numParams;
					Oid		   *paramTypes = NULL;

					forbidden_in_wal_sender(firstchar);

					/* 设置 statement_timestamp*/
					SetCurrentStatementStartTimestamp();

					stmt_name = pq_getmsgstring(&input_message);
					query_string = pq_getmsgstring(&input_message);
					numParams = pq_getmsgint(&input_message, 2);
					if (numParams > 0)
					{
						paramTypes = palloc_array(Oid, numParams);
						for (int i = 0; i < numParams; i++)
							paramTypes[i] = pq_getmsgint(&input_message, 4);
					}
					pq_getmsgend(&input_message);

					exec_parse_message(query_string, stmt_name,
									   paramTypes, numParams);

					valgrind_report_error_query(query_string);
				}
				break;

			case 'B':			/* bind */
				forbidden_in_wal_sender(firstchar);

				/* 设置 statement_timestamp*/
				SetCurrentStatementStartTimestamp();

				/*
				 * 这条消息足够复杂，最好把字段提取工作  
				 * 放到外部函数实现。     
				 */
				exec_bind_message(&input_message);

				/* exec_bind_message 做 valgrind 查询报告        */
				break;

			case 'E':			/* execute */
				{
					const char *portal_name;
					int			max_rows;

					forbidden_in_wal_sender(firstchar);

					/* 设置 statement_timestamp*/
					SetCurrentStatementStartTimestamp();

					portal_name = pq_getmsgstring(&input_message);
					max_rows = pq_getmsgint(&input_message, 4);
					pq_getmsgend(&input_message);

					exec_execute_message(portal_name, max_rows);

					/* exec_execute_message 做 valgrind 查询报告        */
				}
				break;

			case 'F':			/* fastpath 函数调用  */
				forbidden_in_wal_sender(firstchar);

				/* 设置 statement_timestamp*/
				SetCurrentStatementStartTimestamp();

				/* 把查询上报给各种监控设施。        */
				pgstat_report_activity(STATE_FASTPATH, NULL);
				set_ps_display("<FASTPATH>");

				/* 为本次函数调用启动一个事务    */
				start_xact_command();

				/*
				 * 注意：此时我们可能处在已中止的  
				 * 事务。读完函数调用消息之前无法为它    
				 * 抛出错误，因此                         
				 * HandleFunctionRequest() 必须在事后检查它。      
				 * 当心不要做任何假定我们此处处于有效      
				 * 事务中的事情。  
				 */

				/* 切回消息上下文          */
				MemoryContextSwitchTo(MessageContext);

				HandleFunctionRequest(&input_message);

				/* 提交函数调用事务                   */
				finish_xact_command();

				valgrind_report_error_query("fastpath function call");

				send_ready_for_query = true;
				break;

			case 'C':			/* close */
				{
					int			close_type;
					const char *close_target;

					forbidden_in_wal_sender(firstchar);

					close_type = pq_getmsgbyte(&input_message);
					close_target = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					switch (close_type)
					{
						case 'S':
							if (close_target[0] != '\0')
								DropPreparedStatement(close_target, false);
							else
							{
								/* 对未命名语句做特殊处理  */
								drop_unnamed_stmt();
							}
							break;
						case 'P':
							{
								Portal		portal;

								portal = GetPortalByName(close_target);
								if (PortalIsValid(portal))
									PortalDrop(portal, false);
							}
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("invalid CLOSE message subtype %d",
											close_type)));
							break;
					}

					if (whereToSendOutput == DestRemote)
						pq_putemptymessage('3');	/* CloseComplete */

					valgrind_report_error_query("CLOSE message");
				}
				break;

			case 'D':			/* describe */
				{
					int			describe_type;
					const char *describe_target;

					forbidden_in_wal_sender(firstchar);

					/* 设置 statement_timestamp（xact 需要） */
					SetCurrentStatementStartTimestamp();

					describe_type = pq_getmsgbyte(&input_message);
					describe_target = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					switch (describe_type)
					{
						case 'S':
							exec_describe_statement_message(describe_target);
							break;
						case 'P':
							exec_describe_portal_message(describe_target);
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("invalid DESCRIBE message subtype %d",
											describe_type)));
							break;
					}

					valgrind_report_error_query("DESCRIBE message");
				}
				break;

			case 'H':			/* flush */
				pq_getmsgend(&input_message);
				if (whereToSendOutput == DestRemote)
					pq_flush();
				break;

			case 'S':			/* sync */
				pq_getmsgend(&input_message);
				finish_xact_command();
				valgrind_report_error_query("SYNC message");
				send_ready_for_query = true;
				break;

				/*
				 * 'X' 表示前端正在关闭套接字。EOF 表示         
				 * 前端连接意外断开。无论哪种情况，         
				 * 都执行正常关闭。
				 */
			case EOF:

				/* 供累计统计系统使用          */
				pgStatSessionEndCause = DISCONNECT_CLIENT_EOF;

				/* FALLTHROUGH */

			case 'X':

				/*
				 * 重置 whereToSendOutput，以防止 ereport 试图       
				 * 再向客户端发送任何消息。
				 */
				if (whereToSendOutput == DestRemote)
					whereToSendOutput = DestNone;

				/*
				 * 注意：若你想在此加代码，千万别！      
				 * 你原本想做的事情应当改为设置成        
				 * on_proc_exit 或 on_shmem_exit 回调。否则            
				 * 它在其他后端关闭场景下将不会被调用。 
				 *           
				 */
				proc_exit(0);

			case 'd':			/* copy data */
			case 'c':			/* copy done */
			case 'f':			/* copy fail */

				/*
				 * 按照协议规范接受但忽略这些消息；我们 
				 * 多半是因为某次 COPY 失败才到这里，而前端
				 * 仍在发送数据。 
				 */
				break;

			default:
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid frontend message type %d",
								firstchar)));
		}
	}							/* 输入读取循环结束  */
}

/*
 * 若我们是 WAL 发送进程则报错。    
 *
 * 此机制用于禁止 WAL 发送进程收到简单查询协议          
 * 之外的任何消息。'firstchar' 指明收到哪种被禁止的      
 * 消息，并用于构造错误消息。                          
 */
static void
forbidden_in_wal_sender(char firstchar)
{
	if (am_walsender)
	{
		if (firstchar == 'F')
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("fastpath function calls not supported in a replication connection")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("extended query protocol not supported in a replication connection")));
	}
}


/*
 * 获取平台栈深度上限（字节）     
 *
 * 未知则返回 -1  
 */
long
get_stack_depth_rlimit(void)
{
#if defined(HAVE_GETRLIMIT)
	static long val = 0;

	/* 进程启动后此值不变，因此只需检查一次     */
	if (val == 0)
	{
		struct rlimit rlim;

		if (getrlimit(RLIMIT_STACK, &rlim) < 0)
			val = -1;
		else if (rlim.rlim_cur == RLIM_INFINITY)
			val = LONG_MAX;
		/* rlim_cur 很可能是无符号类型，因此要检查溢出    */
		else if (rlim.rlim_cur >= LONG_MAX)
			val = LONG_MAX;
		else
			val = rlim.rlim_cur;
	}
	return val;
#else
	/* Windows 上后端栈大小在 src/backend/Makefile 中设置     */
	return WIN32_STACK_RLIMIT;
#endif
}


static struct rusage Save_r;
static struct timeval Save_t;

void
ResetUsage(void)
{
	getrusage(RUSAGE_SELF, &Save_r);
	gettimeofday(&Save_t, NULL);
}

void
ShowUsage(const char *title)
{
	StringInfoData str;
	struct timeval user,
				sys;
	struct timeval elapse_t;
	struct rusage r;

	getrusage(RUSAGE_SELF, &r);
	gettimeofday(&elapse_t, NULL);
	memcpy((char *) &user, (char *) &r.ru_utime, sizeof(user));
	memcpy((char *) &sys, (char *) &r.ru_stime, sizeof(sys));
	if (elapse_t.tv_usec < Save_t.tv_usec)
	{
		elapse_t.tv_sec--;
		elapse_t.tv_usec += 1000000;
	}
	if (r.ru_utime.tv_usec < Save_r.ru_utime.tv_usec)
	{
		r.ru_utime.tv_sec--;
		r.ru_utime.tv_usec += 1000000;
	}
	if (r.ru_stime.tv_usec < Save_r.ru_stime.tv_usec)
	{
		r.ru_stime.tv_sec--;
		r.ru_stime.tv_usec += 1000000;
	}

	/*
	 * 此处唯一未显示的统计是 ixrss、idrss、isrss。          
	 * 解释它们颇费功夫，且多数平台并不填充这些值。 
	 */
	initStringInfo(&str);

	appendStringInfoString(&str, "! system usage stats:\n");
	appendStringInfo(&str,
					 "!\t%ld.%06ld s user, %ld.%06ld s system, %ld.%06ld s elapsed\n",
					 (long) (r.ru_utime.tv_sec - Save_r.ru_utime.tv_sec),
					 (long) (r.ru_utime.tv_usec - Save_r.ru_utime.tv_usec),
					 (long) (r.ru_stime.tv_sec - Save_r.ru_stime.tv_sec),
					 (long) (r.ru_stime.tv_usec - Save_r.ru_stime.tv_usec),
					 (long) (elapse_t.tv_sec - Save_t.tv_sec),
					 (long) (elapse_t.tv_usec - Save_t.tv_usec));
	appendStringInfo(&str,
					 "!\t[%ld.%06ld s user, %ld.%06ld s system total]\n",
					 (long) user.tv_sec,
					 (long) user.tv_usec,
					 (long) sys.tv_sec,
					 (long) sys.tv_usec);
#ifndef WIN32

	/*
	 * 下面的 rusage 字段并未由 POSIX 定义，但它们        
	 * 存在于当前所有类 Unix 系统上，所以我们无需       
	 * 特殊检查即可使用。其中一些可在我们的 Windows 
	 * 模拟 src/port/win32getrusage.c 中补齐。         
	 */
	appendStringInfo(&str,
					 "!\t%ld kB max resident size\n",
#if defined(__darwin__)
	/* macOS 上按字节*/
					 r.ru_maxrss / 1024
#else
	/* 多数其他平台上按千字节    */
					 r.ru_maxrss
#endif
		);
	appendStringInfo(&str,
					 "!\t%ld/%ld [%ld/%ld] filesystem blocks in/out\n",
					 r.ru_inblock - Save_r.ru_inblock,
	/* 他们只在 dec 喝咖啡    */
					 r.ru_oublock - Save_r.ru_oublock,
					 r.ru_inblock, r.ru_oublock);
	appendStringInfo(&str,
					 "!\t%ld/%ld [%ld/%ld] page faults/reclaims, %ld [%ld] swaps\n",
					 r.ru_majflt - Save_r.ru_majflt,
					 r.ru_minflt - Save_r.ru_minflt,
					 r.ru_majflt, r.ru_minflt,
					 r.ru_nswap - Save_r.ru_nswap,
					 r.ru_nswap);
	appendStringInfo(&str,
					 "!\t%ld [%ld] signals rcvd, %ld/%ld [%ld/%ld] messages rcvd/sent\n",
					 r.ru_nsignals - Save_r.ru_nsignals,
					 r.ru_nsignals,
					 r.ru_msgrcv - Save_r.ru_msgrcv,
					 r.ru_msgsnd - Save_r.ru_msgsnd,
					 r.ru_msgrcv, r.ru_msgsnd);
	appendStringInfo(&str,
					 "!\t%ld/%ld [%ld/%ld] voluntary/involuntary context switches\n",
					 r.ru_nvcsw - Save_r.ru_nvcsw,
					 r.ru_nivcsw - Save_r.ru_nivcsw,
					 r.ru_nvcsw, r.ru_nivcsw);
#endif							/* !WIN32 */

	/* 去掉末尾换行符   */
	if (str.data[str.len - 1] == '\n')
		str.data[--str.len] = '\0';

	ereport(LOG,
			(errmsg_internal("%s", title),
			 errdetail_internal("%s", str.data)));

	pfree(str.data);
}

/*
 * on_proc_exit 处理器记录会话结束  
 */
static void
log_disconnections(int code, Datum arg)
{
	Port	   *port = MyProcPort;
	long		secs;
	int			usecs;
	int			msecs;
	int			hours,
				minutes,
				seconds;

	TimestampDifference(MyStartTimestamp,
						GetCurrentTimestamp(),
						&secs, &usecs);
	msecs = usecs / 1000;

	hours = secs / SECS_PER_HOUR;
	secs %= SECS_PER_HOUR;
	minutes = secs / SECS_PER_MINUTE;
	seconds = secs % SECS_PER_MINUTE;

	ereport(LOG,
			(errmsg("disconnection: session time: %d:%02d:%02d.%03d "
					"user=%s database=%s host=%s%s%s",
					hours, minutes, seconds, msecs,
					port->user_name, port->database_name, port->remote_host,
					port->remote_port[0] ? " port=" : "", port->remote_port)));
}

/*
 * 启用时启动语句超时计时器。   
 *
 * 如果已有超时在运行，就不要重启计时器。这样     
 * 可在超时精确性与启动超时开销之间作出折中。        
 *         
 */
static void
enable_statement_timeout(void)
{
	/* 必须处于事务内  */
	Assert(xact_started);

	if (StatementTimeout > 0)
	{
		if (!get_timeout_active(STATEMENT_TIMEOUT))
			enable_timeout_after(STATEMENT_TIMEOUT, StatementTimeout);
	}
	else
	{
		if (get_timeout_active(STATEMENT_TIMEOUT))
			disable_timeout(STATEMENT_TIMEOUT, false);
	}
}

/*
 * 语句超时若活动则禁用。    
 */
static void
disable_statement_timeout(void)
{
	if (get_timeout_active(STATEMENT_TIMEOUT))
		disable_timeout(STATEMENT_TIMEOUT, false);
}
