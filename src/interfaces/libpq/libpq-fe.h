/*-------------------------------------------------------------------------
 *
 * libpq-fe.h
 *	本文件含结构体的定义，以及
 *	供前端 postgres 应用程序所用的函数外部声明。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/libpq-fe.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef LIBPQ_FE_H
#define LIBPQ_FE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdio.h>

/*
 * postgres_ext.h 定义后端对外可见的类型，
 * 如 Oid。
 */
#include "postgres_ext.h"

/*
 * 这些符号可能用于编译期 #ifdef 测试，以探测有无
 * 较新 libpq 功能。
 */
/* 标明存在 PQenterPipelineMode 及其同类 */
#define LIBPQ_HAS_PIPELINING 1
/* 标明存在 PQsetTraceFlags；亦为新的 PQtrace 输出格式 */
#define LIBPQ_HAS_TRACE_FLAGS 1
/* 标明 PQsslAttribute(NULL, "library") 有用 */
#define LIBPQ_HAS_SSL_LIBRARY_DETECTION 1

/*
 * PQcopyResult 的选项标志
 */
#define PG_COPYRES_ATTRS		  0x01
#define PG_COPYRES_TUPLES		  0x02	/* 隐含 PG_COPYRES_ATTRS */
#define PG_COPYRES_EVENTS		  0x04
#define PG_COPYRES_NOTICEHOOKS	  0x08

/* 应用可见的枚举类型 */

/*
 * 虽可向这些列表追加，但不再使用的值
 * 绝不可删除，常量也绝不可重定义，否则将
 * 破坏与既有代码的兼容性。
 */

typedef enum
{
	CONNECTION_OK,
	CONNECTION_BAD,
	/* 以下仅非阻塞模式 */

	/*
	 * 绝不可依赖这些状态的存在，只可用于
	 * 用户反馈或类似用途。
	 */
	CONNECTION_STARTED,			/* 等待建立连接。  */
	CONNECTION_MADE,			/* 连接正常；等待发送。     */
	CONNECTION_AWAITING_RESPONSE,	/* 等待响应，来自 
									 * postmaster。 */
	CONNECTION_AUTH_OK,			/* 已收到认证；等待
								 * 后端启动。 */
	CONNECTION_SETENV,			/* 此状态已不再使用。 */
	CONNECTION_SSL_STARTUP,		/* 协商 SSL。 */
	CONNECTION_NEEDED,			/* 内部状态：需要 connect() */
	CONNECTION_CHECK_WRITABLE,	/* 检查会话是否可读写。 */
	CONNECTION_CONSUME,			/* 正在消耗多余消息。 */
	CONNECTION_GSS_STARTUP,		/* 协商 GSSAPI。 */
	CONNECTION_CHECK_TARGET,	/* 检查目标服务器属性。 */
	CONNECTION_CHECK_STANDBY	/* 检查其是否处于备用模式。 */
} ConnStatusType;

typedef enum
{
	PGRES_POLLING_FAILED = 0,
	PGRES_POLLING_READING,		/* 二者表示再次轮询前	  */
	PGRES_POLLING_WRITING,		/* 可能先用 select。   */
	PGRES_POLLING_OK,
	PGRES_POLLING_ACTIVE		/* 未使用；暂时保留以求向后
								 * 兼容性 */
} PostgresPollingStatusType;

typedef enum
{
	PGRES_EMPTY_QUERY = 0,		/* 空查询串已执行 */
	PGRES_COMMAND_OK,			/* 不返回东西的查询命令
								 * 已被正确执行，执行方为
								 * 后端 */
	PGRES_TUPLES_OK,			/* 返回元组的查询命令已被
								 * 后端正确执行，且 PGresult 
								 * 含有结果元组 */
	PGRES_COPY_OUT,				/* COPY OUT 数据传输进行中 */
	PGRES_COPY_IN,				/* COPY IN 数据传输进行中 */
	PGRES_BAD_RESPONSE,			/* 收到了意外响应，其来自
								 * 后端 */
	PGRES_NONFATAL_ERROR,		/* 通知或警告消息 */
	PGRES_FATAL_ERROR,			/* 查询失败 */
	PGRES_COPY_BOTH,			/* COPY IN/OUT 数据传输进行中 */
	PGRES_SINGLE_TUPLE,			/* 较大结果集中的单个元组 */
	PGRES_PIPELINE_SYNC,		/* 管道同步点 */
	PGRES_PIPELINE_ABORTED		/* 命令未运行，因管道中
								 * 更早的中止 */
} ExecStatusType;

typedef enum
{
	PQTRANS_IDLE,				/* 连接空闲 */
	PQTRANS_ACTIVE,				/* 命令进行中 */
	PQTRANS_INTRANS,			/* 空闲，位于事务块内 */
	PQTRANS_INERROR,			/* 空闲，在失败事务中 */
	PQTRANS_UNKNOWN				/* 无法判定状态 */
} PGTransactionStatusType;

typedef enum
{
	PQERRORS_TERSE,				/* 单行错误消息 */
	PQERRORS_DEFAULT,			/* 推荐风格 */
	PQERRORS_VERBOSE,			/* 诸事实，女士 */
	PQERRORS_SQLSTATE			/* 仅错误严重级与 SQLSTATE 码 */
} PGVerbosity;

typedef enum
{
	PQSHOW_CONTEXT_NEVER,		/* 永不显示 CONTEXT 域 */
	PQSHOW_CONTEXT_ERRORS,		/* 仅对错误显示 CONTEXT（默认） */
	PQSHOW_CONTEXT_ALWAYS		/* 总是显示 CONTEXT 域 */
} PGContextVisibility;

/*
 * PGPing：此枚举的顺序不可改，因为这些
 * 值经 pg_isready 对外暴露。
 */

typedef enum
{
	PQPING_OK,					/* 服务器正在接受连接 */
	PQPING_REJECT,				/* 服务器存活但拒绝连接 */
	PQPING_NO_RESPONSE,			/* 未能建立连接 */
	PQPING_NO_ATTEMPT			/* 未尝试连接（参数有误） */
} PGPing;

/*
 * PGpipelineStatus：管道模式的当前状态
 */
typedef enum
{
	PQ_PIPELINE_OFF,
	PQ_PIPELINE_ON,
	PQ_PIPELINE_ABORTED
} PGpipelineStatus;

/* PGconn 封装通往后端的连接。
 * 本结构内容不该为应用程序所知。
 */
typedef struct pg_conn PGconn;

/* PGresult 封装一次查询的结果（或更精确地说，是单条 
 * SQL 命令---交给 PQsendQuery 的查询串可含多条
 * 命令，因而返回多个 PGresult 对象）。
 * 本结构内容不该为应用程序所知。
 */
typedef struct pg_result PGresult;

/* PGcancel 封装为取消既有连接上
 * 运行中查询所需信息。
 * 本结构内容不该为应用程序所知。
 */
typedef struct pg_cancel PGcancel;

/* PGnotify 表示出现了 NOTIFY 消息。
 * 理想情况下它应是不透明 typedef，但它太简单，故
 * 不大可能变。
 * 注意：Postgres 6.4 起 be_pid 是发通知后端的 PID，
 * 更早版本则始终是你自己后端的 PID。
 */
typedef struct pgNotify
{
	char	   *relname;		/* 通知条件名 */
	int			be_pid;			/* 发通知的服务器进程 ID */
	char	   *extra;			/* 通知参数 */
	/* 以下字段为 libpq 私有；应用不该使用它们 */
	struct pgNotify *next;		/* 链节 */
} PGnotify;

/* 通知处理回调的函数类型 */
typedef void (*PQnoticeReceiver) (void *arg, const PGresult *res);
typedef void (*PQnoticeProcessor) (void *arg, const char *message);

/* PQprint() 的打印选项 */
typedef char pqbool;

typedef struct _PQprintOpt
{
	pqbool		header;			/* 打印输出字段标题与行数 */
	pqbool		align;			/* 填充对齐各字段 */
	pqbool		standard;		/* 陈旧的脑死格式 */
	pqbool		html3;			/* 输出 HTML 表 */
	pqbool		expanded;		/* 展开表 */
	pqbool		pager;			/* 需要时用分页器输出 */
	char	   *fieldSep;		/* 字段分隔符 */
	char	   *tableOpt;		/* 插入 HTML <table ...> */
	char	   *caption;		/* HTML标题 */
	char	  **fieldName;		/* 以空字符结尾的替换字段名数
								 * 组 */
} PQprintOpt;

/* ----------------
 * 由 PQconndefaults 返回的 conninfo 参数定义结构，或
 * PQconninfoParse。
 *
 * 除 val 外各字段均指向静态串，不得更改。
 * val 或为 NULL，或为 malloc 出的当前值串。PQconninfoFree() 
 * 将释放 val 串与 PQconninfoOption 数组本身。
 * ----------------
 */
typedef struct _PQconninfoOption
{
	char	   *keyword;		/* 该选项的关键字			*/
	char	   *envvar;			/* 回退用的环境变量名	*/
	char	   *compiled;		/* 回退用的编译期默认值	*/
	char	   *val;			/* 选项当前值，或 NULL		 */
	char	   *label;			/* 连接对话框里的字段标签	*/
	char	   *dispchar;		/* 指明连接对话框中本字段
								 * 怎么显示。取值如下：空串
								 * 则原样显示所输；*口令字段，
								 * 隐藏值；D为调试项且默认不
								 * 显示。 */
	int			dispsize;		/* 对话框字段的字符宽度	*/
} PQconninfoOption;

/* ----------------
 * PQArgBlock：PQfn() 参数用的结构
 * ----------------
 */
typedef struct
{
	int			len;
	int			isint;
	union
	{
		int		   *ptr;		/* 不能用 void，DEC编译器报错	 */
		int			integer;
	}			u;
} PQArgBlock;

/* ----------------
 * PGresAttDesc：查询结果里单个属性（列）的数据
 * ----------------
 */
typedef struct pgresAttDesc
{
	char	   *name;			/* 列名 */
	Oid			tableid;		/* 源表，若已知 */
	int			columnid;		/* 源列，若已知 */
	int			format;			/* 值格式码（文本/二进制） */
	Oid			typid;			/* 型id */
	int			typlen;			/* 型大小 */
	int			atttypmod;		/* 类型专属的修饰信息 */
} PGresAttDesc;

/* ----------------
 * libpq 导出的函数
 * ----------------
 */

/* === in fe-connect.c === */

/* 新建一条通往后端的客户端连接 */
/* 异步（非阻塞） */
extern PGconn *PQconnectStart(const char *conninfo);
extern PGconn *PQconnectStartParams(const char *const *keywords,
									const char *const *values, int expand_dbname);
extern PostgresPollingStatusType PQconnectPoll(PGconn *conn);

/* 同步（阻塞） */
extern PGconn *PQconnectdb(const char *conninfo);
extern PGconn *PQconnectdbParams(const char *const *keywords,
								 const char *const *values, int expand_dbname);
extern PGconn *PQsetdbLogin(const char *pghost, const char *pgport,
							const char *pgoptions, const char *pgtty,
							const char *dbName,
							const char *login, const char *pwd);

#define PQsetdb(M_PGHOST,M_PGPORT,M_PGOPT,M_PGTTY,M_DBNAME)  \
	PQsetdbLogin(M_PGHOST, M_PGPORT, M_PGOPT, M_PGTTY, M_DBNAME, NULL, NULL)

/* 关闭当前连接并释放 PGconn 数据结构 */
extern void PQfinish(PGconn *conn);

/* 获取 PQconnectdb 所知的连接选项信息 */
extern PQconninfoOption *PQconndefaults(void);

/* 用与 PQconnectdb 相同方式解析连接选项 */
extern PQconninfoOption *PQconninfoParse(const char *conninfo, char **errmsg);

/* 返回活动连接所用的连接选项 */
extern PQconninfoOption *PQconninfo(PGconn *conn);

/* 释放 PQconndefaults() 或 PQconninfoParse() 返回的数据结构 */
extern void PQconninfoFree(PQconninfoOption *connOptions);

/*
 * 关闭当前连接并重建，沿用相同
 * 参数
 */
/* 异步（非阻塞） */
extern int	PQresetStart(PGconn *conn);
extern PostgresPollingStatusType PQresetPoll(PGconn *conn);

/* 同步（阻塞） */
extern void PQreset(PGconn *conn);

/* 请求一个取消结构 */
extern PGcancel *PQgetCancel(PGconn *conn);

/* 释放取消结构 */
extern void PQfreeCancel(PGcancel *cancel);

/* 发出取消请求 */
extern int	PQcancel(PGcancel *cancel, char *errbuf, int errbufsize);

/* PQcancel 的向后兼容版本；非线程安全 */
extern int	PQrequestCancel(PGconn *conn);

/* PGconn 对象的存取函数 */
extern char *PQdb(const PGconn *conn);
extern char *PQuser(const PGconn *conn);
extern char *PQpass(const PGconn *conn);
extern char *PQhost(const PGconn *conn);
extern char *PQhostaddr(const PGconn *conn);
extern char *PQport(const PGconn *conn);
extern char *PQtty(const PGconn *conn);
extern char *PQoptions(const PGconn *conn);
extern ConnStatusType PQstatus(const PGconn *conn);
extern PGTransactionStatusType PQtransactionStatus(const PGconn *conn);
extern const char *PQparameterStatus(const PGconn *conn,
									 const char *paramName);
extern int	PQprotocolVersion(const PGconn *conn);
extern int	PQserverVersion(const PGconn *conn);
extern char *PQerrorMessage(const PGconn *conn);
extern int	PQsocket(const PGconn *conn);
extern int	PQbackendPID(const PGconn *conn);
extern PGpipelineStatus PQpipelineStatus(const PGconn *conn);
extern int	PQconnectionNeedsPassword(const PGconn *conn);
extern int	PQconnectionUsedPassword(const PGconn *conn);
extern int	PQconnectionUsedGSSAPI(const PGconn *conn);
extern int	PQclientEncoding(const PGconn *conn);
extern int	PQsetClientEncoding(PGconn *conn, const char *encoding);

/* SSL 信息函数 */
extern int	PQsslInUse(PGconn *conn);
extern void *PQsslStruct(PGconn *conn, const char *struct_name);
extern const char *PQsslAttribute(PGconn *conn, const char *attribute_name);
extern const char *const *PQsslAttributeNames(PGconn *conn);

/* 取得与连接关联的 OpenSSL 结构。对未加密连接，
 * 或在使用其他 TLS 库时，返回 NULL。 */
extern void *PQgetssl(PGconn *conn);

/* 告知 libpq 是否需要初始化 OpenSSL */
extern void PQinitSSL(int do_init);

/* 以更细的方式告知 libpq 是否需要初始化 OpenSSL */
extern void PQinitOpenSSL(int do_ssl, int do_crypto);

/* 若正在使用 GSSAPI 加密则返回真 */
extern int	PQgssEncInUse(PGconn *conn);

/* 使用 GSSAPI 时返回其上下文 */
extern void *PQgetgssctx(PGconn *conn);

/* 设置 PQerrorMessage 与 PQresultErrorMessage 的详略 */
extern PGVerbosity PQsetErrorVerbosity(PGconn *conn, PGVerbosity verbosity);

/* 设 PQerrorMessage 与 PQresultErrorMessage 的 CONTEXT 可见性 */
extern PGContextVisibility PQsetErrorContextVisibility(PGconn *conn,
													   PGContextVisibility show_context);

/* 覆盖默认的通知处理例程 */
extern PQnoticeReceiver PQsetNoticeReceiver(PGconn *conn,
											PQnoticeReceiver proc,
											void *arg);
extern PQnoticeProcessor PQsetNoticeProcessor(PGconn *conn,
											  PQnoticeProcessor proc,
											  void *arg);

/*
 *	用于设置回调，以防并发访问 
 *	libpq 需要的非线程安全函数。
 *	默认实现使用 libpq 内部的互斥锁。
 *	只对多线程应用有要求：它们在应用内
 *	和 postgresql 连接中都使用 kerberos。
 */
typedef void (*pgthreadlock_t) (int acquire);

extern pgthreadlock_t PQregisterThreadLock(pgthreadlock_t newhandler);

/* === in fe-trace.c === */
extern void PQtrace(PGconn *conn, FILE *debug_port);
extern void PQuntrace(PGconn *conn);

/* 控制跟踪输出的标志： */
/* 每行省略时间戳 */
#define PQTRACE_SUPPRESS_TIMESTAMPS		(1<<0)
/* 涂黑部分消息，供测试框架使用 */
#define PQTRACE_REGRESS_MODE			(1<<1)
extern void PQsetTraceFlags(PGconn *conn, int flags);

/* === in fe-exec.c === */

/* 简单的同步查询 */
extern PGresult *PQexec(PGconn *conn, const char *query);
extern PGresult *PQexecParams(PGconn *conn,
							  const char *command,
							  int nParams,
							  const Oid *paramTypes,
							  const char *const *paramValues,
							  const int *paramLengths,
							  const int *paramFormats,
							  int resultFormat);
extern PGresult *PQprepare(PGconn *conn, const char *stmtName,
						   const char *query, int nParams,
						   const Oid *paramTypes);
extern PGresult *PQexecPrepared(PGconn *conn,
								const char *stmtName,
								int nParams,
								const char *const *paramValues,
								const int *paramLengths,
								const int *paramFormats,
								int resultFormat);

/* 多结果或异步查询的接口 */
#define PQ_QUERY_PARAM_MAX_LIMIT 65535

extern int	PQsendQuery(PGconn *conn, const char *query);
extern int	PQsendQueryParams(PGconn *conn,
							  const char *command,
							  int nParams,
							  const Oid *paramTypes,
							  const char *const *paramValues,
							  const int *paramLengths,
							  const int *paramFormats,
							  int resultFormat);
extern int	PQsendPrepare(PGconn *conn, const char *stmtName,
						  const char *query, int nParams,
						  const Oid *paramTypes);
extern int	PQsendQueryPrepared(PGconn *conn,
								const char *stmtName,
								int nParams,
								const char *const *paramValues,
								const int *paramLengths,
								const int *paramFormats,
								int resultFormat);
extern int	PQsetSingleRowMode(PGconn *conn);
extern PGresult *PQgetResult(PGconn *conn);

/* 管理异步查询的例程 */
extern int	PQisBusy(PGconn *conn);
extern int	PQconsumeInput(PGconn *conn);

/* 管道模式管理例程 */
extern int	PQenterPipelineMode(PGconn *conn);
extern int	PQexitPipelineMode(PGconn *conn);
extern int	PQpipelineSync(PGconn *conn);
extern int	PQsendFlushRequest(PGconn *conn);

/* LISTEN/NOTIFY 支持 */
extern PGnotify *PQnotifies(PGconn *conn);

/* COPY IN/OUT 的例程 */
extern int	PQputCopyData(PGconn *conn, const char *buffer, int nbytes);
extern int	PQputCopyEnd(PGconn *conn, const char *errormsg);
extern int	PQgetCopyData(PGconn *conn, char **buffer, int async);

/* 已废弃的 COPY IN/OUT 例程 */
extern int	PQgetline(PGconn *conn, char *buffer, int length);
extern int	PQputline(PGconn *conn, const char *string);
extern int	PQgetlineAsync(PGconn *conn, char *buffer, int bufsize);
extern int	PQputnbytes(PGconn *conn, const char *buffer, int nbytes);
extern int	PQendcopy(PGconn *conn);

/* 设置通往后端的阻塞/非阻塞连接 */
extern int	PQsetnonblocking(PGconn *conn, int arg);
extern int	PQisnonblocking(const PGconn *conn);
extern int	PQisthreadsafe(void);
extern PGPing PQping(const char *conninfo);
extern PGPing PQpingParams(const char *const *keywords,
						   const char *const *values, int expand_dbname);

/* 强制写出写缓冲（或至少尝试） */
extern int	PQflush(PGconn *conn);

/*
 * 快速路径接口并不真推荐给应用使
 * 用
 */
extern PGresult *PQfn(PGconn *conn,
					  int fnid,
					  int *result_buf,
					  int *result_len,
					  int result_is_int,
					  const PQArgBlock *args,
					  int nargs);

/* PGresult 对象的存取函数 */
extern ExecStatusType PQresultStatus(const PGresult *res);
extern char *PQresStatus(ExecStatusType status);
extern char *PQresultErrorMessage(const PGresult *res);
extern char *PQresultVerboseErrorMessage(const PGresult *res,
										 PGVerbosity verbosity,
										 PGContextVisibility show_context);
extern char *PQresultErrorField(const PGresult *res, int fieldcode);
extern int	PQntuples(const PGresult *res);
extern int	PQnfields(const PGresult *res);
extern int	PQbinaryTuples(const PGresult *res);
extern char *PQfname(const PGresult *res, int field_num);
extern int	PQfnumber(const PGresult *res, const char *field_name);
extern Oid	PQftable(const PGresult *res, int field_num);
extern int	PQftablecol(const PGresult *res, int field_num);
extern int	PQfformat(const PGresult *res, int field_num);
extern Oid	PQftype(const PGresult *res, int field_num);
extern int	PQfsize(const PGresult *res, int field_num);
extern int	PQfmod(const PGresult *res, int field_num);
extern char *PQcmdStatus(PGresult *res);
extern char *PQoidStatus(const PGresult *res);	/* 又旧又丑 */
extern Oid	PQoidValue(const PGresult *res);	/* 新且改进过 */
extern char *PQcmdTuples(PGresult *res);
extern char *PQgetvalue(const PGresult *res, int tup_num, int field_num);
extern int	PQgetlength(const PGresult *res, int tup_num, int field_num);
extern int	PQgetisnull(const PGresult *res, int tup_num, int field_num);
extern int	PQnparams(const PGresult *res);
extern Oid	PQparamtype(const PGresult *res, int param_num);

/* 描述预备语句与门户 */
extern PGresult *PQdescribePrepared(PGconn *conn, const char *stmt);
extern PGresult *PQdescribePortal(PGconn *conn, const char *portal);
extern int	PQsendDescribePrepared(PGconn *conn, const char *stmt);
extern int	PQsendDescribePortal(PGconn *conn, const char *portal);

/* 删除 PGresult */
extern void PQclear(PGresult *res);

/* 用于释放其他已分配结果，如 PGnotify 结构 */
extern void PQfreemem(void *ptr);

/* 为向后兼容而存在。 bjm 2003-03-24 */
#define PQfreeNotify(ptr) PQfreemem(ptr)

/* 未给出密码时的错误。 */
/* 注意：依赖它已废弃；改用 PQconnectionNeedsPassword()。 */
#define PQnoPasswordSupplied	"fe_sendauth: no password supplied\n"

/* 创建并操作 PGresults */
extern PGresult *PQmakeEmptyPGresult(PGconn *conn, ExecStatusType status);
extern PGresult *PQcopyResult(const PGresult *src, int flags);
extern int	PQsetResultAttrs(PGresult *res, int numAttributes, PGresAttDesc *attDescs);
extern void *PQresultAlloc(PGresult *res, size_t nBytes);
extern size_t PQresultMemorySize(const PGresult *res);
extern int	PQsetvalue(PGresult *res, int tup_num, int field_num, char *value, int len);

/* 纳入查询前给字符串加引号。 */
extern size_t PQescapeStringConn(PGconn *conn,
								 char *to, const char *from, size_t length,
								 int *error);
extern char *PQescapeLiteral(PGconn *conn, const char *str, size_t len);
extern char *PQescapeIdentifier(PGconn *conn, const char *str, size_t len);
extern unsigned char *PQescapeByteaConn(PGconn *conn,
										const unsigned char *from, size_t from_length,
										size_t *to_length);
extern unsigned char *PQunescapeBytea(const unsigned char *strtext,
									  size_t *retbuflen);

/* 这些形式已废弃！ */
extern size_t PQescapeString(char *to, const char *from, size_t length);
extern unsigned char *PQescapeBytea(const unsigned char *from, size_t from_length,
									size_t *to_length);



/* === in fe-print.c === */

extern void PQprint(FILE *fout, /* 输出流 */
					const PGresult *res,
					const PQprintOpt *po);	/* 选项结构 */

/*
 * 极旧的打印例程
 */
extern void PQdisplayTuples(const PGresult *res,
							FILE *fp,	/* 输出发往何处 */
							int fillAlign,	/* 用空格填充字段 */
							const char *fieldSep,	/* 字段分隔符 */
							int printHeader,	/* 显示表头？ */
							int quiet);

extern void PQprintTuples(const PGresult *res,
						  FILE *fout,	/* 输出流 */
						  int PrintAttNames,	/* 打印属性名 */
						  int TerseOutput,	/* 分隔竖线 */
						  int colWidth);	/* 列宽，若为 0 则用
											 * 可变宽度 */


/* === in fe-lobj.c === */

/* 大对象访问例程 */
extern int	lo_open(PGconn *conn, Oid lobjId, int mode);
extern int	lo_close(PGconn *conn, int fd);
extern int	lo_read(PGconn *conn, int fd, char *buf, size_t len);
extern int	lo_write(PGconn *conn, int fd, const char *buf, size_t len);
extern int	lo_lseek(PGconn *conn, int fd, int offset, int whence);
extern pg_int64 lo_lseek64(PGconn *conn, int fd, pg_int64 offset, int whence);
extern Oid	lo_creat(PGconn *conn, int mode);
extern Oid	lo_create(PGconn *conn, Oid lobjId);
extern int	lo_tell(PGconn *conn, int fd);
extern pg_int64 lo_tell64(PGconn *conn, int fd);
extern int	lo_truncate(PGconn *conn, int fd, size_t len);
extern int	lo_truncate64(PGconn *conn, int fd, pg_int64 len);
extern int	lo_unlink(PGconn *conn, Oid lobjId);
extern Oid	lo_import(PGconn *conn, const char *filename);
extern Oid	lo_import_with_oid(PGconn *conn, const char *filename, Oid lobjId);
extern int	lo_export(PGconn *conn, Oid lobjId, const char *filename);

/* === in fe-misc.c === */

/* 取得正在使用的 libpq 库版本 */
extern int	PQlibVersion(void);

/* 确定 *s 处多字节编码字符的长度 */
extern int	PQmblen(const char *s, int encoding);

/* 同上，但不超过到字符串 s 末尾的距离 */
extern int	PQmblenBounded(const char *s, int encoding);

/* 确定 *s 处多字节编码字符的显示长度 */
extern int	PQdsplen(const char *s, int encoding);

/* 从环境变量 PGCLIENTENCODING 取得编码 id */
extern int	PQenv2encoding(void);

/* === in fe-auth.c === */

extern char *PQencryptPassword(const char *passwd, const char *user);
extern char *PQencryptPasswordConn(PGconn *conn, const char *passwd, const char *user, const char *algorithm);

/* === in encnames.c === */

extern int	pg_char_to_encoding(const char *name);
extern const char *pg_encoding_to_char(int encoding);
extern int	pg_valid_server_encoding_id(int encoding);

/* === in fe-secure-openssl.c === */

/* 支持用回调覆盖 sslpassword 的处理 */
typedef int (*PQsslKeyPassHook_OpenSSL_type) (char *buf, int size, PGconn *conn);
extern PQsslKeyPassHook_OpenSSL_type PQgetSSLKeyPassHook_OpenSSL(void);
extern void PQsetSSLKeyPassHook_OpenSSL(PQsslKeyPassHook_OpenSSL_type hook);
extern int	PQdefaultSSLKeyPassHook_OpenSSL(char *buf, int size, PGconn *conn);

#ifdef __cplusplus
}
#endif

#endif							/* LIBPQ_FE_H */
