/*-------------------------------------------------------------------------
 *
 * libpq-int.h
 *	本文件含只供内部使用的定义，给
 *	前端 libpq 库用，不给调用它的应用程序。
 *
 *	应用程序若想绕过，可以包含本文件
 *	libpq-fe.h 所定的官方 API，但这样做的代码
 *	比只用官方 API 的代码，更可能在 PostgreSQL
 *	各版本之间坏掉。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/libpq-int.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef LIBPQ_INT_H
#define LIBPQ_INT_H

/* 假定已经包含了 libpq-fe.h。 */
#include "libpq-events.h"

#include <netdb.h>
#include <sys/socket.h>
#include <time.h>
/* MinGW 有 sys/time.h，但 MSVC 没有 */
#ifndef _MSC_VER
#include <sys/time.h>
#endif

#ifdef ENABLE_THREAD_SAFETY
#ifdef WIN32
#include "pthread-win32.h"
#else
#include <pthread.h>
#endif
#include <signal.h>
#endif

/* 包含前后端共用的东西 */
#include "libpq/pqcomm.h"
/* 只包含前端才有的东西 */
#include "fe-auth-sasl.h"
#include "pqexpbuffer.h"

#ifdef ENABLE_GSS
#if defined(HAVE_GSSAPI_H)
#include <gssapi.h>
#else
#include <gssapi/gssapi.h>
#endif
#endif

#ifdef ENABLE_SSPI
#define SECURITY_WIN32
#if defined(WIN32) && !defined(_MSC_VER)
#include <ntsecapi.h>
#endif
#include <security.h>
#undef SECURITY_WIN32

#ifndef ENABLE_GSS
/*
 * 定义一个与 Unix 上 GSSAPI 兼容的假结构。
 */
typedef struct
{
	void	   *value;
	int			length;
} gss_buffer_desc;
#endif
#endif							/* ENABLE_SSPI */

#ifdef USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>

#ifndef OPENSSL_NO_ENGINE
#define USE_SSL_ENGINE
#endif
#endif							/* USE_OPENSSL */

#include "common/pg_prng.h"

/*
 * 依赖 POSTGRES 后端的常量。
 */
#define CMDSTATUS_LEN 64		/* 对齐 COMPLETION_TAG_BUFSIZE */

/*
 * PGresult 及附属类型 PGresAttDesc、PGresAttValue
 * 表示一次查询的结果（更精确说，是单条 SQL
 * 命令。交给 PQexec 的查询串可含多条命令）。
 * 注意我们假定单条命令最多返回一组元组，
 * 因此不需要多套描述符。
 */

/* PGresult 的附属存储管理结构。
 * 详见 fe-exec.c 里的空间管理例程。
 * 注意 space[k] 指从块的物理
 * 头起第 k 字节；是联合非结构！
 */
typedef union pgresult_data PGresult_data;

union pgresult_data
{
	PGresult_data *next;		/* 链到下一块，或 NULL */
	char		space[1];		/* 当作字节访问块的占位 */
};

/* 预备语句单个参数的数据 */
typedef struct pgresParamDesc
{
	Oid			typid;			/* 型id */
} PGresParamDesc;

/*
 * 单个元组的单个属性的数据
 *
 * 属性值我们用 char*。
 *
 * 值指针总指向以空字符结尾的区域；我们在
 * 后端所发内容之后补一个零字节。这仅对
 * 文本值特别有用。二进制值时，
 * 值里可能嵌有空字符，故应用程序无法用 C 字符串
 * 运算处理。但为一致仍补上零。
 * 注意值本身不含长度字。
 *
 * NULL 属性有两点特殊：其 len 字段为 NULL_LEN
 * 且其 value 指向所属 PGresult 里的 null_field。所有
 * 查询结果里的 NULL 属性都指向同一处（不必
 * 为每个单独存一份空串）。
 */

#define NULL_LEN		(-1)	/* NULL 值的 pg_result 长度 */

typedef struct pgresAttValue
{
	int			len;			/* 值的字节长度 */
	char	   *value;			/* 实际值，外加结尾的零字节 */
} PGresAttValue;

/* 消息字段列表项的类型定义 */
typedef struct pgMessageField
{
	struct pgMessageField *next;	/* 链节 */
	char		code;			/* 字段码 */
	char		contents[FLEXIBLE_ARRAY_MEMBER];	/* 值，NUL 结尾 */
} PGMessageField;

/* 通知处理所需的字段 */
typedef struct
{
	PQnoticeReceiver noticeRec; /* 通知消息接收者 */
	void	   *noticeRecArg;
	PQnoticeProcessor noticeProc;	/* 通知消息处理器 */
	void	   *noticeProcArg;
} PGNoticeHooks;

typedef struct PGEvent
{
	PGEventProc proc;			/* 事件时调用的函数 */
	char	   *name;			/* 只用于错误消息 */
	void	   *passThrough;	/* 注册时提供的指针 */
	void	   *data;			/* 可选状态（实例）数据 */
	bool		resultInitialized;	/* RESULTCREATE/COPY 成功为 T */
} PGEvent;

struct pg_result
{
	int			ntups;
	int			numAttributes;
	PGresAttDesc *attDescs;
	PGresAttValue **tuples;		/* 每个 PGresult 元组是一组
								 * PGresAttValue */
	int			tupArrSize;		/* 元组数组的已分配大小 */
	int			numParameters;
	PGresParamDesc *paramDescs;
	ExecStatusType resultStatus;
	char		cmdStatus[CMDSTATUS_LEN];	/* 查询的命令状态 */
	int			binary;			/* binary 为 1 时值是二进制，
								 * 否则文本 */

	/*
	 * 这些字段从源 PGconn 复制而来，以便对
	 * PGresult 的操作不必再引用 PGconn。
	 */
	PGNoticeHooks noticeHooks;
	PGEvent    *events;
	int			nEvents;
	int			client_encoding;	/* 编码 id */

	/*
	 * 错误信息（若不是错误结果则全为 NULL）。errMsg 是
	 * PQresultErrorMessage 返回的总错误消息。若有
	 * 逐字段信息，就存在一个链表里。
	 */
	char	   *errMsg;			/* 错误消息，没有则为 NULL */
	PGMessageField *errFields;	/* 消息拆成的字段 */
	char	   *errQuery;		/* 触发查询的文本，若有的话 */

	/* 查询结果中所有 NULL 属性都指向这条空串 */
	char		null_field[1];

	/*
	 * 空间管理信息。注意 attDescs 与错误材料若
	 * 非空，就指向已分配的块。但 tuples 指向
	 * 单独 malloc 的块，以便我们能 realloc 它。
	 */
	PGresult_data *curBlock;	/* 最近分配的块 */
	int			curOffset;		/* 块内空闲空间的起始偏移 */
	int			spaceLeft;		/* 块内剩余的空闲字节数 */

	size_t		memorySize;		/* 此 PGresult 分配的总空间 */
};

/* PGAsyncStatusType 定义查询执行状态机的状态 */
typedef enum
{
	PGASYNC_IDLE,				/* 啥也没发生 */
	PGASYNC_BUSY,				/* 查询进行中 */
	PGASYNC_READY,				/* 查询完成，等客户端来取
								 * 结果 */
	PGASYNC_READY_MORE,			/* 查询完成，等客户端来取
								 * 结果，更多结果将来自此查
								 * 询 */
	PGASYNC_COPY_IN,			/* COPY IN 数据传输进行中 */
	PGASYNC_COPY_OUT,			/* COPY OUT 数据传输进行中 */
	PGASYNC_COPY_BOTH,			/* COPY IN/OUT 数据传输进行中 */
	PGASYNC_PIPELINE_IDLE,		/* 管线模式里命令之间的空闲 */
} PGAsyncStatusType;

/* 目标服务器类型（解码 target_session_attrs） */
typedef enum
{
	SERVER_TYPE_ANY = 0,		/* 任意（默认） */
	SERVER_TYPE_READ_WRITE,		/* 读写服务器 */
	SERVER_TYPE_READ_ONLY,		/* 只读服务器 */
	SERVER_TYPE_PRIMARY,		/* 主服务器 */
	SERVER_TYPE_STANDBY,		/* 备服务器 */
	SERVER_TYPE_PREFER_STANDBY, /* 优先备用服务器 */
	SERVER_TYPE_PREFER_STANDBY_PASS2	/* 第二遍，行为同 ANY */
} PGTargetServerType;

/* 目标服务器类型（解码 load_balance_hosts） */
typedef enum
{
	LOAD_BALANCE_DISABLE = 0,	/* 沿用现有主机顺序（默认） */
	LOAD_BALANCE_RANDOM,		/* 随机打乱主机 */
} PGLoadBalanceType;

/* 布尔值加未知态，供也许要取的 GUCs */
typedef enum
{
	PG_BOOL_UNKNOWN = 0,		/* 目前未知 */
	PG_BOOL_YES,				/* 是(真) */
	PG_BOOL_NO					/* 否(假) */
} PGTernaryBool;

/* EnvironmentOptions[] 的类型定义 */
typedef struct PQEnvironmentOption
{
	const char *envName,		/* 某个环境变量的名字 */
			   *pgName;			/* 对应 SET 变量的名字 */
} PQEnvironmentOption;

/* 参数状态列表项的类型定义 */
typedef struct pgParameterStatus
{
	struct pgParameterStatus *next; /* 链节 */
	char	   *name;			/* 参数名 */
	char	   *value;			/* 参数值 */
	/* 注意：name 与 value 和该结构存在同一 malloc 块里 */
} pgParameterStatus;

/* 大对象访问数据。只有用到大对象代码才分配。 */
typedef struct pgLobjfuncs
{
	Oid			fn_lo_open;		/* 后端函数 lo_open 的 OID		*/
	Oid			fn_lo_close;	/* 后端函数 lo_close 的 OID		*/
	Oid			fn_lo_creat;	/* 后端函数 lo_creat 的 OID		*/
	Oid			fn_lo_create;	/* 后端函数 lo_create 的 OID	*/
	Oid			fn_lo_unlink;	/* 后端函数 lo_unlink 的 OID	*/
	Oid			fn_lo_lseek;	/* 后端函数 lo_lseek 的 OID		*/
	Oid			fn_lo_lseek64;	/* 后端函数 lo_lseek64 的 OID	*/
	Oid			fn_lo_tell;		/* 后端函数 lo_tell 的 OID		*/
	Oid			fn_lo_tell64;	/* 后端函数 lo_tell64 的 OID	*/
	Oid			fn_lo_truncate; /* 后端函数 lo_truncate 的 OID	*/
	Oid			fn_lo_truncate64;	/* 函数 lo_truncate64 的 OID */
	Oid			fn_lo_read;		/* 后端函数 LOread 的 OID		*/
	Oid			fn_lo_write;	/* 后端函数 LOwrite 的 OID		*/
} PGlobjfuncs;

/* PGdataValue 表示传给行处理器的一个数据字段值。
 * 它可能是文本或二进制；文本不以零结尾。
 * SQL NULL 用 len < 0 表示；此时 value 仍有效，但那里
 * 没有数据字节。
 */
typedef struct pgDataValue
{
	int			len;			/* 数据字节长度，NULL 时 <0 */
	const char *value;			/* 数据值，没有零结尾 */
} PGdataValue;

/* pg_conn_host 的主机地址类型枚举 */
typedef enum pg_conn_host_type
{
	CHT_HOST_NAME,
	CHT_HOST_ADDRESS,
	CHT_UNIX_SOCKET
} pg_conn_host_type;

/*
 * PGQueryClass 跟踪每条命令队列项在用哪种查询协议，
 * 或执行中的特殊操作
 */
typedef enum
{
	PGQUERY_SIMPLE,				/* 简单查询协议（PQexec） */
	PGQUERY_EXTENDED,			/* 完整扩展协议（PQexecParams） */
	PGQUERY_PREPARE,			/* 只解析(PQprepare) */
	PGQUERY_DESCRIBE,			/* 描述语句或门户 */
	PGQUERY_SYNC,				/* 同步（管线末尾） */
	PGQUERY_CLOSE
} PGQueryClass;

/*
 * 待处理命令队列中的一项。
 */
typedef struct PGcmdQueueEntry
{
	PGQueryClass queryclass;	/* 查询类 */
	char	   *query;			/* SQL 命令，NULL 表无/未知/OOM */
	struct PGcmdQueueEntry *next;	/* 链节 */
} PGcmdQueueEntry;

/*
 * pg_conn_host 存放连接串里可能有的每一台主机的
 * 全部信息。多数字段由拆分
 * 相应连接参数（如 pghost）的逗号而来。
 */
typedef struct pg_conn_host
{
	pg_conn_host_type type;		/* 主机地址类型 */
	char	   *host;			/* 主机名或套接字径 */
	char	   *hostaddr;		/* 主机数字 IP 地址 */
	char	   *port;			/* 端口号；空或 NULL 时用
								 * DEF_PGPORT[_STR] */
	char	   *password;		/* 此主机的口令，读自
								 * 口令文件；若未查找或未
								 * 找到则 NULL。 */
} pg_conn_host;

/*
 * PGconn 存放单条后端连接的
 * 全状态。
 */
struct pg_conn
{
	/* 连接选项的保存值 */
	char	   *pghost;			/* 服务器所在机器，或 UNIX 域
								 * 套接字路径，或逗号分隔的
								 * 机器与路径列表；NULL 时用
								 * DEFAULT_PGSOCKET_DIR */
	char	   *pghostaddr;		/* 服务器所在机器的数字
								 * IP 地址，或同样的
								 * 逗号分隔列表。其优先级
								 * 高于 pghost。 */
	char	   *pgport;			/* 服务器的通信端口号，或
								 * 逗号分隔的端口列表 */
	char	   *connect_timeout;	/* 连接超时（数字串） */
	char	   *pgtcp_user_timeout; /* TCP 用户超时（数字串） */
	char	   *client_encoding_initial;	/* 所用编码 */
	char	   *pgoptions;		/* 启动后端时的选项 */
	char	   *appname;		/* 应用名 */
	char	   *fbappname;		/* 后备应用名 */
	char	   *dbName;			/* 数据库名 */
	char	   *replication;	/* 是否作为复制备机连接？ */
	char	   *pguser;			/* Postgres 用户名与口令，若有 */
	char	   *pgpass;
	char	   *pgpassfile;		/* 含有口令的文件路径 */
	char	   *channel_binding;	/* 通道绑定模式
									 * require/prefer/disable */
	char	   *keepalives;		/* 用 TCP 保活？ */
	char	   *keepalives_idle;	/* TCP 保活间隔 */
	char	   *keepalives_interval;	/* TCP 保活的间隔
										 * 重传数 */
	char	   *keepalives_count;	/* TCP 保活最大次数
									 * 重传数 */
	char	   *sslmode;		/* SSL 模式 require/prefer/allow/disable */
	char	   *sslcompression; /* SSL 压缩（0 或 1） */
	char	   *sslkey;			/* 密钥文件名 */
	char	   *sslcert;		/* 客户端证书文件名 */
	char	   *sslpassword;	/* 密钥文件的口令 */
	char	   *sslcertmode;	/* 客户证书模式 require/allow/disable */
	char	   *sslrootcert;	/* 根证书文件名 */
	char	   *sslcrl;			/* 证书吊销列表文件名 */
	char	   *sslcrldir;		/* 证书吊销列表目录名 */
	char	   *sslsni;			/* 用 SSL SNI 扩展否（0/1） */
	char	   *requirepeer;	/* 本地套接字所要求的对端凭据 */
	char	   *gssencmode;		/* GSS 模式 require/prefer/disable */
	char	   *krbsrvname;		/* Kerberos 服务名 */
	char	   *gsslib;			/* 用哪种 GSS 库（gssapi 或
								 * sspi） */
	char	   *gssdelegation;	/* 尝试委托 GSS 凭据？（0 或 1） */
	char	   *ssl_min_protocol_version;	/* 最低 TLS 协议版本 */
	char	   *ssl_max_protocol_version;	/* 最高 TLS 协议版本 */
	char	   *target_session_attrs;	/* 期望的会话属性 */
	char	   *require_auth;	/* 期望的认证方法名 */
	char	   *load_balance_hosts; /* 主机间负载均衡 */

	/* 可选的跟踪信息输出文件 */
	FILE	   *Pfdebug;
	int			traceFlags;

	/* 通知消息处理的回调过程 */
	PGNoticeHooks noticeHooks;

	/* 经 PQregisterEventProc 注册的事件过程 */
	PGEvent    *events;			/* 可扩展的事件数据数组 */
	int			nEvents;		/* 活动事件数 */
	int			eventArraySize; /* 数组分配大小 */

	/* 状态指示 */
	ConnStatusType status;
	PGAsyncStatusType asyncStatus;
	PGTransactionStatusType xactStatus; /* 从不变成 ACTIVE */
	char		last_sqlstate[6];	/* 上次的 SQLSTATE */
	bool		options_valid;	/* 可以尝试连接则为真 */
	bool		nonblocking;	/* 此连接是否使用非阻塞
								 * 发送语义 */
	PGpipelineStatus pipelineStatus;	/* 管线模式的状态 */
	bool		singleRowMode;	/* 是否逐行返回当前查询结果？ */
	char		copy_is_binary; /* 1 二进制拷贝，0 文本 */
	int			copy_already_done;	/* COPY OUT 已返回的字节数 */
	PGnotify   *notifyHead;		/* 最旧的未报告 Notify */
	PGnotify   *notifyTail;		/* 最新的未报告 Notify */

	/* 连接串里支持多台主机 */
	int			nconnhost;		/* 连接串中点名的主机数 */
	int			whichhost;		/* 当前正在尝试或已连上的主机 */
	pg_conn_host *connhost;		/* 各主机的详情 */
	char	   *connip;			/* 当前网络连接的 IP 地址 */

	/*
	 * 待处理命令队列，做成单链表。头是
	 * 当前在执行的命令，尾是新命令加入处。
	 */
	PGcmdQueueEntry *cmd_queue_head;
	PGcmdQueueEntry *cmd_queue_tail;

	/*
	 * 为省 malloc 开销，我们不立刻释放项；而是
	 * 把它们存此表，供可能复用。
	 */
	PGcmdQueueEntry *cmd_queue_recycle;

	/* 连接数据 */
	pgsocket	sock;			/* FD；PGINVALID_SOCKET 若
								 * 未连接 */
	SockAddr	laddr;			/* 本地地址 */
	SockAddr	raddr;			/* 远端地址 */
	ProtocolVersion pversion;	/* 在用的 FE/BE 协议版本 */
	int			sversion;		/* 服务器版本，70401 即 7.4.1 */
	bool		auth_req_received;	/* 收到过认证请求则为真 */
	bool		password_needed;	/* 服务器要过口令则为真 */
	bool		gssapi_used;	/* 若经 gssapi 认证则为真 */
	bool		sigpipe_so;		/* 已用 SO_NOSIGPIPE 挡 SIGPIPE？ */
	bool		sigpipe_flag;	/* 能用 MSG_NOSIGNAL 挡 SIGPIPE？ */
	bool		write_failed;	/* 套接字上发生过写失败吗？ */
	char	   *write_err_msg;	/* 写错误消息，OOM 则 NULL */

	bool		auth_required;	/* 是否要求对方发认证
								 * 质询？ */
	uint32		allowed_auth_methods;	/* 可接受 AuthRequest 的掩
										 * 码 */
	bool		client_finished_auth;	/* 认证交换中我们这一半
										 * 是否已经做完？ */


	/* 建立连接期间需要的短暂状态 */
	PGTargetServerType target_server_type;	/* 期望的会话属性 */
	PGLoadBalanceType load_balance_type;	/* 期望的负载均衡
											 * 算法 */
	bool		try_next_addr;	/* 该进下一地址/主机了吗？ */
	bool		try_next_host;	/* 该进下一个 connhost[]？ */
	int			naddr;			/* getaddrinfo 返回的地址数 */
	int			whichaddr;		/* 当前正在尝试的地址 */
	AddrInfo   *addr;			/* 当前所试主机的地址
								 * 数组 */
	bool		send_appname;	/* 能发 application_name？ */

	/* 杂项 */
	int			be_pid;			/* 后端 PID，取消时需要 */
	int			be_key;			/* 后端密钥，取消时需要 */
	pgParameterStatus *pstatus; /* ParameterStatus */
	int			client_encoding;	/* 编码 id */
	bool		std_strings;	/* standard_conforming_strings */
	PGTernaryBool default_transaction_read_only;	/* default_transaction_read_only */
	PGTernaryBool in_hot_standby;	/* in_hot_standby */
	PGVerbosity verbosity;		/* 错误与通知的详细度 */
	PGContextVisibility show_context;	/* 是否显示 CONTEXT 字段 */
	PGlobjfuncs *lobjfuncs;		/* 大对象访问函数的私有状态 */
	pg_prng_state prng_state;	/* 负载均衡连接的伪随机态 */


	/*
	 * 存放从后端收到、尚未处理的数据的缓冲。
	 *
	 * 注意：我们依赖 inBufSize/outBufSize 最大为 INT_MAX（因而
	 * 任何及所有包内容的大小都有 INT_MAX 上限），以免
	 * 溢出；例如在 reportErrorPosition() 里。改类型不但要
	 * 调整 pqCheck{In,Out}BufferSpace() 里的溢出保护，还要
	 * 仔细审计 libpq 里所有在尺寸计算中
	 * 使用 int 的代码。
	 */
	char	   *inBuffer;		/* 当前已分配缓冲 */
	int			inBufSize;		/* 缓冲区已分配大小 */
	int			inStart;		/* 首个未消费数据的偏移 */
	int			inCursor;		/* 下一个试探消费的字节 */
	int			inEnd;			/* 可用数据后首位置的偏移 */

	/* 尚未发给后端的数据缓冲 */
	char	   *outBuffer;		/* 当前已分配缓冲 */
	int			outBufSize;		/* 缓冲区已分配大小 */
	int			outCount;		/* 缓冲里等待的字符数 */

	/* 在 outBuffer 里构造消息的状态 */
	int			outMsgStart;	/* 消息起点偏移；-1 表示
								 * 消息没有长度字 */
	int			outMsgEnd;		/* 消息结尾的偏移 */

	/* 行处理器接口的工作区 */
	PGdataValue *rowBuf;		/* 传给 rowProcessor 的值数组 */
	int			rowBufLen;		/* rowBuf 里已分配的项数 */

	/*
	 * 异步构造结果的状态。若 result 非 NULL，它
	 * 是一个正在构造或准备返回的结果。若 result 为 NULL
	 * 且 error_result 为真，则我们需要返回一个
	 * PGRES_FATAL_ERROR 结果，但还没构造它；错误文本已
	 * 追加到 conn->errorMessage。（推迟构造简化了
	 * 对内存不足情形的处理。）若 next_result 非 NULL，它是
	 * 将替换刚返回的那个 PGresult。
	 */
	PGresult   *result;			/* 正在构造的结果 */
	bool		error_result;	/* 要做 ERROR 结果吗？ */
	PGresult   *next_result;	/* 下一结果（单行模式） */

	/* SASL、SSL、GSS 等的各种状态 */
	const pg_fe_sasl_mech *sasl;
	void	   *sasl_state;
	int			scram_sha_256_iterations;

	/* SSL 结构 */
	bool		ssl_in_use;
	bool		ssl_cert_requested; /* 服务器要过证书吗？ */
	bool		ssl_cert_sent;	/* 回复里送过吗？ */

#ifdef USE_SSL
	bool		allow_ssl_try;	/* 允许尝试 SSL 协商 */
	bool		wait_ssl_try;	/* 把 SSL 协商推迟到
								 * 尝试普通连接之后 */
#ifdef USE_OPENSSL
	SSL		   *ssl;			/* 若有 SSL 连接，其 SSL 状态 */
	X509	   *peer;			/* 服务器 X509 证 */
#ifdef USE_SSL_ENGINE
	ENGINE	   *engine;			/* SSL引擎(若有) */
#else
	void	   *engine;			/* 占位以使结构不变，若
								 * OpenSSL 版本一变 */
#endif
	bool		crypto_loaded;	/* 记录本连接是否已做
								 * libcrypto 加锁回调。可以在
								 * 去掉 OpenSSL 1.0.2 支持后删掉，
								 * 因为这种加锁在
								 * OpenSSL >= 1.1.0 内部处理 */
#endif							/* USE_OPENSSL */
#endif							/* USE_SSL */

#ifdef ENABLE_GSS
	gss_ctx_id_t gctx;			/* GSS 语境 */
	gss_name_t	gtarg_nam;		/* GSS 目标名 */

	/* 以下几项只用于加密 */
	bool		try_gss;		/* 允许尝试 GSS */
	bool		gssenc;			/* GSS 加密可用 */
	gss_cred_id_t gcred;		/* GSS 凭据的临时存放。 */

	/* GSS 加密的 I/O 状态，见 fe-secure-gssapi.c */
	char	   *gss_SendBuffer; /* 等待发送的已加密数据 */
	int			gss_SendLength; /* gss_SendBuffer 里可用数据的末尾 */
	int			gss_SendNext;	/* 发送下标，取自
								 * gss_SendBuffer */
	int			gss_SendConsumed;	/* 源字节已加密，但还
									 * 没报成已发送 */
	char	   *gss_RecvBuffer; /* 收到的已加密数据 */
	int			gss_RecvLength; /* gss_RecvBuffer 里可用数据的末尾 */
	char	   *gss_ResultBuffer;	/* gss_RecvBuffer 中数据的解密 */
	int			gss_ResultLength;	/* 可用数据的末端在
									 * gss_ResultBuffer */
	int			gss_ResultNext; /* 读下一字节的索引，从
								 * gss_ResultBuffer */
	uint32		gss_MaxPktSize; /* 我们能加密并塞进
								 * 输出缓冲的最大结果 */
#endif

#ifdef ENABLE_SSPI
	CredHandle *sspicred;		/* SSPI 凭据句柄 */
	CtxtHandle *sspictx;		/* SSPI 语境 */
	char	   *sspitarget;		/* SSPI 目标名 */
	int			usesspi;		/* 表明是否在连接上用
								 * SSPI */
#endif

	/*
	 * 当前错误消息的缓冲。任何连接尝试或
	 * 查询周期开始时清掉它；此后所有代码只应追加
	 * 消息进去，绝不覆盖。
	 *
	 * 有些情况下，一个查询周期里我们可能不止一次
	 * 报告错误。那样 errorMessage 便累加全部错误文本，
	 * errorReported 记录我们已经报告了多少，以便
	 * 各个错误 PGresult 对象不含重复文本。
	 */
	PQExpBufferData errorMessage;	/* 可扩展的串 */
	int			errorReported;	/* 字符串里已报告的字节数 */

	/* 接收消息各部分的缓冲 */
	PQExpBufferData workBuffer; /* 可扩展的串 */
};

/* PGcancel 存放安全取消一条连接所需的全部数据。其副本
 * 才能安全取消跑在另一个
 * 线程
 */
struct pg_cancel
{
	SockAddr	raddr;			/* 远端地址 */
	int			be_pid;			/* 后端 PID，取消时需要 */
	int			be_key;			/* 后端密钥，取消时需要 */
	int			pgtcp_user_timeout; /* TCP 用户超时 */
	int			keepalives;		/* 用 TCP 保活？ */
	int			keepalives_idle;	/* TCP 保活间隔 */
	int			keepalives_interval;	/* TCP 保活的间隔
										 * 重传数 */
	int			keepalives_count;	/* TCP 保活最大次数
									 * 重传数 */
};


/* ExecStatusTypes 的字符串说明。
 * 直接使用此数组已弃用；请改调 PQresStatus()。
 */
extern char *const pgresStatus[];


#ifdef USE_SSL

#ifndef WIN32
#define USER_CERT_FILE		".postgresql/postgresql.crt"
#define USER_KEY_FILE		".postgresql/postgresql.key"
#define ROOT_CERT_FILE		".postgresql/root.crt"
#define ROOT_CRL_FILE		".postgresql/root.crl"
#else
/* 在 Windows 上，home 目录已经是 PostgreSQL 专用的 */
#define USER_CERT_FILE		"postgresql.crt"
#define USER_KEY_FILE		"postgresql.key"
#define ROOT_CERT_FILE		"root.crt"
#define ROOT_CRL_FILE		"root.crl"
#endif

#endif							/* USE_SSL */

/* ----------------
 * libpq 的内部函数
 * 这里声明的函数需要在 libpq 各文件间可见，
 * 但不打算给应用程序调用。我们用
 * 约定内部函数叫 pqXXX，而给应用看的叫
 * PQxxx 这些名字。
 * ----------------
 */

/* === in fe-connect.c === */

extern void pqDropConnection(PGconn *conn, bool flushInput);
extern int	pqPacketSend(PGconn *conn, char pack_type,
						 const void *buf, size_t buf_len);
extern bool pqGetHomeDirectory(char *buf, int bufsize);

#ifdef ENABLE_THREAD_SAFETY
extern pgthreadlock_t pg_g_threadlock;

#define pglock_thread()		pg_g_threadlock(true)
#define pgunlock_thread()	pg_g_threadlock(false)
#else
#define pglock_thread()		((void) 0)
#define pgunlock_thread()	((void) 0)
#endif

/* === in fe-exec.c === */

extern void pqSetResultError(PGresult *res, PQExpBuffer errorMessage, int offset);
extern void *pqResultAlloc(PGresult *res, size_t nBytes, bool isBinary);
extern char *pqResultStrdup(PGresult *res, const char *str);
extern void pqClearAsyncResult(PGconn *conn);
extern void pqSaveErrorResult(PGconn *conn);
extern PGresult *pqPrepareAsyncResult(PGconn *conn);
extern void pqInternalNotice(const PGNoticeHooks *hooks, const char *fmt,...) pg_attribute_printf(2, 3);
extern void pqSaveMessageField(PGresult *res, char code,
							   const char *value);
extern void pqSaveParameterStatus(PGconn *conn, const char *name,
								  const char *value);
extern int	pqRowProcessor(PGconn *conn, const char **errmsgp);
extern void pqCommandQueueAdvance(PGconn *conn, bool isReadyForQuery,
								  bool gotSync);
extern int	PQsendQueryContinue(PGconn *conn, const char *query);

/* === in fe-protocol3.c === */

extern char *pqBuildStartupPacket3(PGconn *conn, int *packetlen,
								   const PQEnvironmentOption *options);
extern void pqParseInput3(PGconn *conn);
extern int	pqGetErrorNotice3(PGconn *conn, bool isError);
extern void pqBuildErrorMessage3(PQExpBuffer msg, const PGresult *res,
								 PGVerbosity verbosity, PGContextVisibility show_context);
extern int	pqGetNegotiateProtocolVersion3(PGconn *conn);
extern int	pqGetCopyData3(PGconn *conn, char **buffer, int async);
extern int	pqGetline3(PGconn *conn, char *s, int maxlen);
extern int	pqGetlineAsync3(PGconn *conn, char *buffer, int bufsize);
extern int	pqEndcopy3(PGconn *conn);
extern PGresult *pqFunctionCall3(PGconn *conn, Oid fnid,
								 int *result_buf, int *actual_result_len,
								 int result_is_int,
								 const PQArgBlock *args, int nargs);

/* === in fe-misc.c === */

 /*
  * 取与放例程成功回 0，否则 EOF。注意对
  * 取而言，EOF 只表示缓冲尽了，并非
  * 一定有错误。
  */
extern int	pqCheckOutBufferSpace(size_t bytes_needed, PGconn *conn);
extern int	pqCheckInBufferSpace(size_t bytes_needed, PGconn *conn);
extern int	pqGetc(char *result, PGconn *conn);
extern int	pqPutc(char c, PGconn *conn);
extern int	pqGets(PQExpBuffer buf, PGconn *conn);
extern int	pqGets_append(PQExpBuffer buf, PGconn *conn);
extern int	pqPuts(const char *s, PGconn *conn);
extern int	pqGetnchar(char *s, size_t len, PGconn *conn);
extern int	pqSkipnchar(size_t len, PGconn *conn);
extern int	pqPutnchar(const char *s, size_t len, PGconn *conn);
extern int	pqGetInt(int *result, size_t bytes, PGconn *conn);
extern int	pqPutInt(int value, size_t bytes, PGconn *conn);
extern int	pqPutMsgStart(char msg_type, PGconn *conn);
extern int	pqPutMsgEnd(PGconn *conn);
extern int	pqReadData(PGconn *conn);
extern int	pqFlush(PGconn *conn);
extern int	pqWait(int forRead, int forWrite, PGconn *conn);
extern int	pqWaitTimed(int forRead, int forWrite, PGconn *conn,
						time_t finish_time);
extern int	pqReadReady(PGconn *conn);
extern int	pqWriteReady(PGconn *conn);

/* === in fe-secure.c === */

extern int	pqsecure_initialize(PGconn *, bool, bool);
extern PostgresPollingStatusType pqsecure_open_client(PGconn *);
extern void pqsecure_close(PGconn *);
extern ssize_t pqsecure_read(PGconn *, void *ptr, size_t len);
extern ssize_t pqsecure_write(PGconn *, const void *ptr, size_t len);
extern ssize_t pqsecure_raw_read(PGconn *, void *ptr, size_t len);
extern ssize_t pqsecure_raw_write(PGconn *, const void *ptr, size_t len);

#if defined(ENABLE_THREAD_SAFETY) && !defined(WIN32)
extern int	pq_block_sigpipe(sigset_t *osigset, bool *sigpipe_pending);
extern void pq_reset_sigpipe(sigset_t *osigset, bool sigpipe_pending,
							 bool got_epipe);
#endif

/* SSL 部分 */

/*
 * SSL 实现提供这些函数。
 */

/*
 *	PQinitSSL() 的实现。
 */
extern void pgtls_init_library(bool do_ssl, int do_crypto);

/*
 * 初始化 SSL 库。
 *
 * conn 参数只用来回传错误
 * 消息，这里不做连接局部的设置。do_ssl 控制
 * 是否初始化 SSL，do_crypto 对加密部分同样
 * 做
 *
 * 成功 0，失败 -1（往 conn->errorMessage 加消息）。
 */
extern int	pgtls_init(PGconn *conn, bool do_ssl, bool do_crypto);

/*
 *	开始或继续协商安全会话。
 */
extern PostgresPollingStatusType pgtls_open_client(PGconn *conn);

/*
 *	关闭 SSL 连接。
 */
extern void pgtls_close(PGconn *conn);

/*
 *	从安全连接读取数据。
 *
 * 失败时，本函数负责追加一条合适的消息
 * 到 conn->errorMessage。调用者仍须查看 errno，但仅
 * 以判断出错后要继续还是重试。
 */
extern ssize_t pgtls_read(PGconn *conn, void *ptr, size_t len);

/*
 *	SSL 读缓冲里还有未读的数据吗？
 */
extern bool pgtls_read_pending(PGconn *conn);

/*
 *	向安全连接写数据。
 *
 * 失败时，本函数负责追加一条合适的消息
 * 到 conn->errorMessage。调用者仍须查看 errno，但仅
 * 以判断出错后要继续还是重试。
 */
extern ssize_t pgtls_write(PGconn *conn, const void *ptr, size_t len);

/*
 * 取得服务器证书的散列，用于 SCRAM 通道绑定类型
 * tls-server-end-point
 *
 * 出错时向调用者回送 NULL，并带一条
 * 供调用者使用的错误消息。
 *
 * 无 X509_get_signature_nid() 的旧 OpenSSL
 * 不支持。
 */
#if defined(USE_OPENSSL) && (defined(HAVE_X509_GET_SIGNATURE_NID) || defined(HAVE_X509_GET_SIGNATURE_INFO))
#define HAVE_PGTLS_GET_PEER_CERTIFICATE_HASH
extern char *pgtls_get_peer_certificate_hash(PGconn *conn, size_t *len);
#endif

/*
 * 核实服务器证书与我们所连主机名是否匹配。
 *
 * 要考虑证书的通用名与主题备用名。
 *
 * 名字匹配返回 1，不匹配返回 0。出错时返回
 * 负1，并设置 libpq 错误消息
 *
 */
extern int	pgtls_verify_peer_name_matches_certificate_guts(PGconn *conn,
															int *names_examined,
															char **first_name);

/* GSSAPI 部分 */

#ifdef ENABLE_GSS

/*
 * 建立一条 GSSAPI 加密的连接。
 */
extern PostgresPollingStatusType pqsecure_open_gss(PGconn *conn);

/*
 * 用于 GSSAPI 加密连接的读写函数，带内部
 * 缓冲，以处理非阻塞套接字。
 */
extern ssize_t pg_GSS_write(PGconn *conn, const void *ptr, size_t len);
extern ssize_t pg_GSS_read(PGconn *conn, void *ptr, size_t len);
#endif

/* === in fe-trace.c === */

extern void pqTraceOutputMessage(PGconn *conn, const char *message,
								 bool toServer);
extern void pqTraceOutputNoTypeByteMessage(PGconn *conn, const char *message);

/* 杂项宏 */

/*
 * 重置该连接的错误报告状态。
 */
#define pqClearConnErrorState(conn) \
	(resetPQExpBuffer(&(conn)->errorMessage), \
	 (conn)->errorReported = 0)

/*
 * 检查是否有待返回的 PGresult，要么是
 * conn->result 中造好的，或一个虚拟错误结果，我们
 * 不打算在查询周期结束前把它具体化。
 */
#define pgHavePendingResult(conn) \
	((conn)->result != NULL || (conn)->error_result)

/*
 * 这样我们就能在内部检查连接是否非阻塞，
 * 省掉函数调用开销
 */
#define pqIsnonblocking(conn)	((conn)->nonblocking)

/*
 * 连接输出缓冲的阈值，用于管线模式。
 */
#define OUTBUFFER_THRESHOLD	65536

#ifdef ENABLE_NLS
extern char *libpq_gettext(const char *msgid) pg_attribute_format_arg(1);
extern char *libpq_ngettext(const char *msgid, const char *msgid_plural, unsigned long n) pg_attribute_format_arg(1) pg_attribute_format_arg(2);
#else
#define libpq_gettext(x) (x)
#define libpq_ngettext(s, p, n) ((n) == 1 ? (s) : (p))
#endif
/*
 * libpq 代码应使用上面的，不要用 _()，否则将用到
 * 外层程序的消息目录。
 */
#undef _

extern void libpq_append_error(PQExpBuffer errorMessage, const char *fmt,...) pg_attribute_printf(2, 3);
extern void libpq_append_conn_error(PGconn *conn, const char *fmt,...) pg_attribute_printf(2, 3);

/*
 * 这些宏让错误处理代码能在 Unix 与 Windows 之间移植。
 * 唉。
 */
#ifdef WIN32
#define SOCK_ERRNO (WSAGetLastError())
#define SOCK_STRERROR winsock_strerror
#define SOCK_ERRNO_SET(e) WSASetLastError(e)
#else
#define SOCK_ERRNO errno
#define SOCK_STRERROR strerror_r
#define SOCK_ERRNO_SET(e) (errno = (e))
#endif

#endif							/* LIBPQ_INT_H */
