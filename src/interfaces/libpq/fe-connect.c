/*-------------------------------------------------------------------------
 *
 * fe-connect.c
 *	  functions related to setting up a connection to the backend
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-connect.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>
#include <netdb.h>
#include <time.h>
#include <unistd.h>

#include "common/ip.h"
#include "common/link-canary.h"
#include "common/scram-common.h"
#include "common/string.h"
#include "fe-auth.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "mb/pg_wchar.h"
#include "pg_config_paths.h"
#include "port/pg_bswap.h"

#ifdef WIN32
#include "win32.h"
#ifdef _WIN32_IE
#undef _WIN32_IE
#endif
#define _WIN32_IE 0x0500
#ifdef near
#undef near
#endif
#define near
#include <shlobj.h>
#include <mstcpip.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

#ifdef ENABLE_THREAD_SAFETY
#ifdef WIN32
#include "pthread-win32.h"
#else
#include <pthread.h>
#endif
#endif

#ifdef USE_LDAP
#ifdef WIN32
#include <winldap.h>
#else
/* OpenLDAP 已弃用 RFC 1823，但我们想要符合标准      */
#define LDAP_DEPRECATED 1
#include <ldap.h>
typedef struct timeval LDAP_TIMEVAL;
#endif
static int	ldapServiceLookup(const char *purl, PQconninfoOption *options,
							  PQExpBuffer errorMessage);
#endif

#ifndef WIN32
#define PGPASSFILE ".pgpass"
#else
#define PGPASSFILE "pgpass.conf"
#endif

/*
 * 9.0 前服务器返回此 SQLSTATE。硬编码
 * 而非查 errcodes.h，因反映历史行为。
                                                                        
                                                                      
*/
#define ERRCODE_APPNAME_UNKNOWN "42704"

/* 这属于协议的一部分，直接定义     */
#define ERRCODE_INVALID_PASSWORD "28P01"
/* 同上   */
#define ERRCODE_CANNOT_CONNECT_NOW "57P03"

/*
 * 应对各平台 TCP 保活选项拼写差异。
 * 不涵盖 Windows。
                                                                                */
#if defined(TCP_KEEPIDLE)
/* TCP_KEEPIDLE 是此选项在 Linux 和 *BSD 上的名称  */
#define PG_TCP_KEEPALIVE_IDLE TCP_KEEPIDLE
#define PG_TCP_KEEPALIVE_IDLE_STR "TCP_KEEPIDLE"
#elif defined(TCP_KEEPALIVE_THRESHOLD)
/* Solaris>=11 上名 TCP_KEEPALIVE_THRESHOLD                          */
#define PG_TCP_KEEPALIVE_IDLE TCP_KEEPALIVE_THRESHOLD
#define PG_TCP_KEEPALIVE_IDLE_STR "TCP_KEEPALIVE_THRESHOLD"
#elif defined(TCP_KEEPALIVE) && defined(__darwin__)
/* TCP_KEEPALIVE 是此选项在 macOS 上的名称  */
/* 注意：Solaris 也有此符号，但含义有所不同          */
#define PG_TCP_KEEPALIVE_IDLE TCP_KEEPALIVE
#define PG_TCP_KEEPALIVE_IDLE_STR "TCP_KEEPALIVE"
#endif

/*
 * 参数/环境变量未指定时的后备选项
                                                 
*/
#define DefaultHost		"localhost"
#define DefaultOption	""
#ifdef USE_SSL
#define DefaultChannelBinding	"prefer"
#else
#define DefaultChannelBinding	"disable"
#endif
#define DefaultTargetSessionAttrs	"any"
#define DefaultLoadBalanceHosts	"disable"
#ifdef USE_SSL
#define DefaultSSLMode "prefer"
#define DefaultSSLCertMode "allow"
#else
#define DefaultSSLMode	"disable"
#define DefaultSSLCertMode "disable"
#endif
#ifdef ENABLE_GSS
#include "fe-gssapi-common.h"
#define DefaultGSSMode "prefer"
#else
#define DefaultGSSMode "disable"
#endif

/* ----------
 * conninfo 参数定义及后备资源。
 *
 * 若环境变量和编译默认值均为 NULL，
 * 则无后备。若最终无法确定选项值
 * 则报错。
 *
 * 用户名在 conninfo_add_defaults 中特
 * 殊处理。无法获取时由
 * pg_fe_getauthname() 确定。
 *
 * Label 和 Disp-Char 供想用
 * PQconndefaults() 创建连接对话框的
 * 应用使用。Disp-Char 定义：
 *		""		普通输入
 *		"*"		密码 - 隐藏
 *		"D"		调试 - 默认不显
 *
 * PQconninfoOptions[] 为常量静态数组，
 * 用于初始化动态工作副本。所有 "val"
 * *须*为 NULL。工作副本中非 NULL "val"
 * 指向 malloc 串，释放数组时一并释放
 *（见 PQconninfoFree）。
 *
 * 各 struct 前半与 libpq-fe.h 相同，
 * 因两者间需 memcpy() 复制。
 * ----------
                                                                                                                                                                                                                                                                                                                                                                                                                  */
typedef struct _internalPQconninfoOption
{
	char	   *keyword;		/* 选项的关键字          */
	char	   *envvar;			/* 后备环境变量名              */
	char	   *compiled;		/* 编译时内置的后备默认值  */
	char	   *val;			/* 选项的当前值，或 NULL    */
	char	   *label;			/* 连接对话框中字段的标签 */
	char	   *dispchar;		/* 指示字段显示方式。取值：
					 * ""原样 "*"密码隐藏
					 * "D"调试默认不显                                                                  
                                                   
*/
	int			dispsize;		/* 对话框中字段的字符大小   */
	/* ---
		 * 此注释以上须与 libpq-fe.h 的
		 * PQconninfoOption 同步，
		 * 因需 memcpy()。
		 * ---
		                                    */
	off_t		connofs;		/* PGconn 结构内偏移量，无则为 -1   */
} internalPQconninfoOption;

static const internalPQconninfoOption PQconninfoOptions[] = {
	{"service", "PGSERVICE", NULL, NULL,
	"Database-Service", "", 20, -1},

	{"user", "PGUSER", NULL, NULL,
		"Database-User", "", 20,
	offsetof(struct pg_conn, pguser)},

	{"password", "PGPASSWORD", NULL, NULL,
		"Database-Password", "*", 20,
	offsetof(struct pg_conn, pgpass)},

	{"passfile", "PGPASSFILE", NULL, NULL,
		"Database-Password-File", "", 64,
	offsetof(struct pg_conn, pgpassfile)},

	{"channel_binding", "PGCHANNELBINDING", DefaultChannelBinding, NULL,
		"Channel-Binding", "", 8,	/* sizeof("require")==8   */
	offsetof(struct pg_conn, channel_binding)},

	{"connect_timeout", "PGCONNECT_TIMEOUT", NULL, NULL,
		"Connect-timeout", "", 10,	/* strlen(INT32_MAX)==10   */
	offsetof(struct pg_conn, connect_timeout)},

	{"dbname", "PGDATABASE", NULL, NULL,
		"Database-Name", "", 20,
	offsetof(struct pg_conn, dbName)},

	{"host", "PGHOST", NULL, NULL,
		"Database-Host", "", 40,
	offsetof(struct pg_conn, pghost)},

	{"hostaddr", "PGHOSTADDR", NULL, NULL,
		"Database-Host-IP-Address", "", 45,
	offsetof(struct pg_conn, pghostaddr)},

	{"port", "PGPORT", DEF_PGPORT_STR, NULL,
		"Database-Port", "", 6,
	offsetof(struct pg_conn, pgport)},

	{"client_encoding", "PGCLIENTENCODING", NULL, NULL,
		"Client-Encoding", "", 10,
	offsetof(struct pg_conn, client_encoding_initial)},

	{"options", "PGOPTIONS", DefaultOption, NULL,
		"Backend-Options", "", 40,
	offsetof(struct pg_conn, pgoptions)},

	{"application_name", "PGAPPNAME", NULL, NULL,
		"Application-Name", "", 64,
	offsetof(struct pg_conn, appname)},

	{"fallback_application_name", NULL, NULL, NULL,
		"Fallback-Application-Name", "", 64,
	offsetof(struct pg_conn, fbappname)},

	{"keepalives", NULL, NULL, NULL,
		"TCP-Keepalives", "", 1,	/* 应仅为 '0' 或 '1'     */
	offsetof(struct pg_conn, keepalives)},

	{"keepalives_idle", NULL, NULL, NULL,
		"TCP-Keepalives-Idle", "", 10,	/* strlen(INT32_MAX)==10   */
	offsetof(struct pg_conn, keepalives_idle)},

	{"keepalives_interval", NULL, NULL, NULL,
		"TCP-Keepalives-Interval", "", 10,	/* strlen(INT32_MAX)==10   */
	offsetof(struct pg_conn, keepalives_interval)},

	{"keepalives_count", NULL, NULL, NULL,
		"TCP-Keepalives-Count", "", 10, /* strlen(INT32_MAX)==10   */
	offsetof(struct pg_conn, keepalives_count)},

	{"tcp_user_timeout", NULL, NULL, NULL,
		"TCP-User-Timeout", "", 10, /* strlen(INT32_MAX)==10   */
	offsetof(struct pg_conn, pgtcp_user_timeout)},

	/*
		 * 无 SSL 支持时 ssl 选项也允许，
		 * 因仍可处理 disable/allow。
		 * 其他对非 SSL 无影响故无需排除。
		                                                                                                                                      
*/
	{"sslmode", "PGSSLMODE", DefaultSSLMode, NULL,
		"SSL-Mode", "", 12,		/* sizeof("verify-full")==12   */
	offsetof(struct pg_conn, sslmode)},

	{"sslcompression", "PGSSLCOMPRESSION", "0", NULL,
		"SSL-Compression", "", 1,
	offsetof(struct pg_conn, sslcompression)},

	{"sslcert", "PGSSLCERT", NULL, NULL,
		"SSL-Client-Cert", "", 64,
	offsetof(struct pg_conn, sslcert)},

	{"sslkey", "PGSSLKEY", NULL, NULL,
		"SSL-Client-Key", "", 64,
	offsetof(struct pg_conn, sslkey)},

	{"sslcertmode", "PGSSLCERTMODE", NULL, NULL,
		"SSL-Client-Cert-Mode", "", 8,	/* sizeof("disable")==8   */
	offsetof(struct pg_conn, sslcertmode)},

	{"sslpassword", NULL, NULL, NULL,
		"SSL-Client-Key-Password", "*", 20,
	offsetof(struct pg_conn, sslpassword)},

	{"sslrootcert", "PGSSLROOTCERT", NULL, NULL,
		"SSL-Root-Certificate", "", 64,
	offsetof(struct pg_conn, sslrootcert)},

	{"sslcrl", "PGSSLCRL", NULL, NULL,
		"SSL-Revocation-List", "", 64,
	offsetof(struct pg_conn, sslcrl)},

	{"sslcrldir", "PGSSLCRLDIR", NULL, NULL,
		"SSL-Revocation-List-Dir", "", 64,
	offsetof(struct pg_conn, sslcrldir)},

	{"sslsni", "PGSSLSNI", "1", NULL,
		"SSL-SNI", "", 1,
	offsetof(struct pg_conn, sslsni)},

	{"requirepeer", "PGREQUIREPEER", NULL, NULL,
		"Require-Peer", "", 10,
	offsetof(struct pg_conn, requirepeer)},

	{"require_auth", "PGREQUIREAUTH", NULL, NULL,
		"Require-Auth", "", 14, /* sizeof("scram-sha-256")==14   */
	offsetof(struct pg_conn, require_auth)},

	{"ssl_min_protocol_version", "PGSSLMINPROTOCOLVERSION", "TLSv1.2", NULL,
		"SSL-Minimum-Protocol-Version", "", 8,	/* sizeof("TLSv1.x")==8   */
	offsetof(struct pg_conn, ssl_min_protocol_version)},

	{"ssl_max_protocol_version", "PGSSLMAXPROTOCOLVERSION", NULL, NULL,
		"SSL-Maximum-Protocol-Version", "", 8,	/* sizeof("TLSv1.x")==8   */
	offsetof(struct pg_conn, ssl_max_protocol_version)},

	/*
		 * 同 SSL，GSS 选项未构建也暴露。
		                                         
*/
	{"gssencmode", "PGGSSENCMODE", DefaultGSSMode, NULL,
		"GSSENC-Mode", "", 8,	/* sizeof("disable")==8   */
	offsetof(struct pg_conn, gssencmode)},

	/* Kerberos 和 GSSAPI 认证支持指定服务名                        */
	{"krbsrvname", "PGKRBSRVNAME", PG_KRB_SRVNAM, NULL,
		"Kerberos-service-name", "", 20,
	offsetof(struct pg_conn, krbsrvname)},

	{"gsslib", "PGGSSLIB", NULL, NULL,
		"GSS-library", "", 7,	/* sizeof("gssapi")==7   */
	offsetof(struct pg_conn, gsslib)},

	{"gssdelegation", "PGGSSDELEGATION", "0", NULL,
		"GSS-delegation", "", 1,
	offsetof(struct pg_conn, gssdelegation)},

	{"replication", NULL, NULL, NULL,
		"Replication", "D", 5,
	offsetof(struct pg_conn, replication)},

	{"target_session_attrs", "PGTARGETSESSIONATTRS",
		DefaultTargetSessionAttrs, NULL,
		"Target-Session-Attrs", "", 15, /* sizeof("prefer-standby")=15   */
	offsetof(struct pg_conn, target_session_attrs)},

	{"load_balance_hosts", "PGLOADBALANCEHOSTS",
		DefaultLoadBalanceHosts, NULL,
		"Load-Balance-Hosts", "", 8,	/* sizeof("disable")=8   */
	offsetof(struct pg_conn, load_balance_hosts)},

	/* 终止表项 --- 必须在最后   */
	{NULL, NULL, NULL, NULL,
	NULL, NULL, 0}
};

static const PQEnvironmentOption EnvironmentOptions[] =
{
	/* 公共用户界面设置       */
	{
		"PGDATESTYLE", "datestyle"
	},
	{
		"PGTZ", "timezone"
	},
	/* 内部性能相关设置              */
	{
		"PGGEQO", "geqo"
	},
	{
		NULL, NULL
	}
};

/* 连接 URI 必须以下列标识符之一开头：                      */
static const char uri_designator[] = "postgresql://";
static const char short_uri_designator[] = "postgres://";

static bool connectOptions1(PGconn *conn, const char *conninfo);
static bool connectOptions2(PGconn *conn);
static int	connectDBStart(PGconn *conn);
static int	connectDBComplete(PGconn *conn);
static PGPing internal_ping(PGconn *conn);
static PGconn *makeEmptyPGconn(void);
static void pqFreeCommandQueue(PGcmdQueueEntry *queue);
static bool fillPGconn(PGconn *conn, PQconninfoOption *connOptions);
static void freePGconn(PGconn *conn);
static void closePGconn(PGconn *conn);
static void release_conn_addrinfo(PGconn *conn);
static int	store_conn_addrinfo(PGconn *conn, struct addrinfo *addrlist);
static void sendTerminateConn(PGconn *conn);
static PQconninfoOption *conninfo_init(PQExpBuffer errorMessage);
static PQconninfoOption *parse_connection_string(const char *connstr,
												 PQExpBuffer errorMessage, bool use_defaults);
static int	uri_prefix_length(const char *connstr);
static bool recognized_connection_string(const char *connstr);
static PQconninfoOption *conninfo_parse(const char *conninfo,
										PQExpBuffer errorMessage, bool use_defaults);
static PQconninfoOption *conninfo_array_parse(const char *const *keywords,
											  const char *const *values, PQExpBuffer errorMessage,
											  bool use_defaults, int expand_dbname);
static bool conninfo_add_defaults(PQconninfoOption *options,
								  PQExpBuffer errorMessage);
static PQconninfoOption *conninfo_uri_parse(const char *uri,
											PQExpBuffer errorMessage, bool use_defaults);
static bool conninfo_uri_parse_options(PQconninfoOption *options,
									   const char *uri, PQExpBuffer errorMessage);
static bool conninfo_uri_parse_params(char *params,
									  PQconninfoOption *connOptions,
									  PQExpBuffer errorMessage);
static char *conninfo_uri_decode(const char *str, PQExpBuffer errorMessage);
static bool get_hexdigit(char digit, int *value);
static const char *conninfo_getval(PQconninfoOption *connOptions,
								   const char *keyword);
static PQconninfoOption *conninfo_storeval(PQconninfoOption *connOptions,
										   const char *keyword, const char *value,
										   PQExpBuffer errorMessage, bool ignoreMissing, bool uri_decode);
static PQconninfoOption *conninfo_find(PQconninfoOption *connOptions,
									   const char *keyword);
static void defaultNoticeReceiver(void *arg, const PGresult *res);
static void defaultNoticeProcessor(void *arg, const char *message);
static int	parseServiceInfo(PQconninfoOption *options,
							 PQExpBuffer errorMessage);
static int	parseServiceFile(const char *serviceFile,
							 const char *service,
							 PQconninfoOption *options,
							 PQExpBuffer errorMessage,
							 bool *group_found);
static char *pwdfMatchesString(char *buf, const char *token);
static char *passwordFromFile(const char *hostname, const char *port, const char *dbname,
							  const char *username, const char *pgpassfile);
static void pgpassfileWarning(PGconn *conn);
static void default_threadlock(int acquire);
static bool sslVerifyProtocolVersion(const char *version);
static bool sslVerifyProtocolRange(const char *min, const char *max);
static bool parse_int_param(const char *value, int *result, PGconn *conn,
							const char *context);


/* 全局变量，因为 fe-auth.c 需要访问它      */
pgthreadlock_t pg_g_threadlock = default_threadlock;


/*
 *		pqDropConnection
 *
 * 关闭物理连接，重置相关状态。
 * 不释放重连所需及后续可用状态。
 *
 * 输出缓冲可刷新，因不可能再发送。
 * 未处理输入可能有价值，调用者
 * 决定是否刷新。
                                                                                                                    
                                                                                                                  
*/
void
pqDropConnection(PGconn *conn, bool flushInput)
{
	/* 丢弃SSL状态    */
	pqsecure_close(conn);

	/* 关闭套接字本身   */
	if (conn->sock != PGINVALID_SOCKET)
		closesocket(conn->sock);
	conn->sock = PGINVALID_SOCKET;

	/* 可选弃未读数据              */
	if (flushInput)
		conn->inStart = conn->inCursor = conn->inEnd = 0;

	/* 总是丢弃未发送的数据 */
	conn->outCount = 0;

	/* 弃待处理流水线命令                      */
	pqFreeCommandQueue(conn->cmd_queue_head);
	conn->cmd_queue_head = conn->cmd_queue_tail = NULL;
	pqFreeCommandQueue(conn->cmd_queue_recycle);
	conn->cmd_queue_recycle = NULL;

	/* 释放认证/加密状态            */
#ifdef ENABLE_GSS
	{
		OM_uint32	min_s;

		if (conn->gcred != GSS_C_NO_CREDENTIAL)
		{
			gss_release_cred(&min_s, &conn->gcred);
			conn->gcred = GSS_C_NO_CREDENTIAL;
		}
		if (conn->gctx)
			gss_delete_sec_context(&min_s, &conn->gctx, GSS_C_NO_BUFFER);
		if (conn->gtarg_nam)
			gss_release_name(&min_s, &conn->gtarg_nam);
		if (conn->gss_SendBuffer)
		{
			free(conn->gss_SendBuffer);
			conn->gss_SendBuffer = NULL;
		}
		if (conn->gss_RecvBuffer)
		{
			free(conn->gss_RecvBuffer);
			conn->gss_RecvBuffer = NULL;
		}
		if (conn->gss_ResultBuffer)
		{
			free(conn->gss_ResultBuffer);
			conn->gss_ResultBuffer = NULL;
		}
		conn->gssenc = false;
	}
#endif
#ifdef ENABLE_SSPI
	if (conn->sspitarget)
	{
		free(conn->sspitarget);
		conn->sspitarget = NULL;
	}
	if (conn->sspicred)
	{
		FreeCredentialsHandle(conn->sspicred);
		free(conn->sspicred);
		conn->sspicred = NULL;
	}
	if (conn->sspictx)
	{
		DeleteSecurityContext(conn->sspictx);
		free(conn->sspictx);
		conn->sspictx = NULL;
	}
	conn->usesspi = 0;
#endif
	if (conn->sasl_state)
	{
		conn->sasl->free(conn->sasl_state);
		conn->sasl_state = NULL;
	}
}

/*
 * pqFreeCommandQueue
 * 释放 PGcmdQueueEntry 队列。
                      */
static void
pqFreeCommandQueue(PGcmdQueueEntry *queue)
{
	while (queue != NULL)
	{
		PGcmdQueueEntry *cur = queue;

		queue = cur->next;
		free(cur->query);
		free(cur);
	}
}

/*
 *		pqDropServerData
 *
 * 清除从服务器收/推断的连接状态。
 * 换不同服务器时必做，免误留旧
 * 服务器数据。
 *
 * 可能应合入 pqDropConnection，但
 * 该函数断连时立调（如 pqReadData）。
 * 数据应保留到实际新连时。
                                                                                                                                        
                                                                                                                                      
*/
static void
pqDropServerData(PGconn *conn)
{
	PGnotify   *notify;
	pgParameterStatus *pstatus;

	/* 丢弃未决的 NOTIFY  */
	notify = conn->notifyHead;
	while (notify != NULL)
	{
		PGnotify   *prev = notify;

		notify = notify->next;
		free(prev);
	}
	conn->notifyHead = conn->notifyTail = NULL;

	/* 重置 ParameterStatus 及推导变量                           */
	pstatus = conn->pstatus;
	while (pstatus != NULL)
	{
		pgParameterStatus *prev = pstatus;

		pstatus = pstatus->next;
		free(prev);
	}
	conn->pstatus = NULL;
	conn->client_encoding = PG_SQL_ASCII;
	conn->std_strings = false;
	conn->default_transaction_read_only = PG_BOOL_UNKNOWN;
	conn->in_hot_standby = PG_BOOL_UNKNOWN;
	conn->scram_sha_256_iterations = SCRAM_SHA_256_DEFAULT_ITERATIONS;
	conn->sversion = 0;

	/* 丢弃大对象查找数据   */
	free(conn->lobjfuncs);
	conn->lobjfuncs = NULL;

	/* 重置其他各类连接级状态         */
	conn->last_sqlstate[0] = '\0';
	conn->auth_req_received = false;
	conn->client_finished_auth = false;
	conn->password_needed = false;
	conn->gssapi_used = false;
	conn->write_failed = false;
	free(conn->write_err_msg);
	conn->write_err_msg = NULL;
	conn->be_pid = 0;
	conn->be_key = 0;
}


/*
 *		连接数据库
 *
 * 本 API 有六种连接方式。两种不推荐
 * 新代码用（缺选项传递扩展性）：
 * PQsetdb 和 PQsetdbLogin
 *（前者是后者的宏）。
 *
 * 同步（阻塞）用 PQconnectdb 或
 * PQconnectdbParams。前者接受
 * option=value 串或 URI 需解析；
 * 后者接受两个 NULL 终止数组。
 *
 * 异步（非阻塞）用 PQconnectStart
 * 或 PQconnectStartParams（差异同上）
 * 及 PQconnectPoll。
 *
 * 内部静态 connectDBStart、
 * connectDBComplete 是连接过程
 * 的一部分。
                                                                                                                                                                                                                                                                                                                                                                                                  */

/*
 *		PQconnectdbParams
 *
 * 用两个数组的连接信息通过 postmaster
 * 建连到 postgres 后端。
 *
 * keywords 数组：
 *
 *	   const char *params[] = {"o1",
 *							"o2", NULL}
 *
 * values 数组：
 *
 *	   const char *values[] = {"v1",
 *							"v2", NULL}
 *
 * 返回 PGconn*；malloc 失败返 NULL。
 * status 为 CONNECTION_BAD 时部分
 * 字段可能置空。
 *
 * 无论成败应调 PQfinish（conn 非空时）。
                                                                                                                                                                                                                                                          */
PGconn *
PQconnectdbParams(const char *const *keywords,
				  const char *const *values,
				  int expand_dbname)
{
	PGconn	   *conn = PQconnectStartParams(keywords, values, expand_dbname);

	if (conn && conn->status != CONNECTION_BAD)
		(void) connectDBComplete(conn);

	return conn;
}

/*
 *		PQpingParams
 *
 * 查服务器状态，参数同 PQconnectdbParams
                         */
PGPing
PQpingParams(const char *const *keywords,
			 const char *const *values,
			 int expand_dbname)
{
	PGconn	   *conn = PQconnectStartParams(keywords, values, expand_dbname);
	PGPing		ret;

	ret = internal_ping(conn);
	PQfinish(conn);

	return ret;
}

/*
 *		PQconnectdb
 *
 * 用串中连接信息通过 postmaster
 * 建连到 postgres 后端。
 *
 * conninfo 是空格分隔的
 *
 *	   option = value
 *
 * 列表或 URI（详见文档）。值含单引号
 * 时须 \' 转义。
 *
 * 返回 PGconn*；malloc 失败返 NULL。
 * status 为 CONNECTION_BAD 时部分
 * 字段可能置空。
 *
 * 无论成败应调 PQfinish（conn 非空时）。
                                                                                                               
                                                                                                              
                                                                                                             
                                                                                                             
*/
PGconn *
PQconnectdb(const char *conninfo)
{
	PGconn	   *conn = PQconnectStart(conninfo);

	if (conn && conn->status != CONNECTION_BAD)
		(void) connectDBComplete(conn);

	return conn;
}

/*
 *		PQping
 *
 * 查服务器状态，参数同 PQconnectdb
                         */
PGPing
PQping(const char *conninfo)
{
	PGconn	   *conn = PQconnectStart(conninfo);
	PGPing		ret;

	ret = internal_ping(conn);
	PQfinish(conn);

	return ret;
}

/*
 *		PQconnectStartParams
 *
 * 用 struct 中连接信息开始通过
 * postmaster 建连到后端。
 *
 * 格式见 PQconnectdbParams 注释。
 *
 * 返回 PGconn*。NULL 即 malloc 错，
 * 不应继续。status 为 CONNECTION_BAD
 * 表示出错，应对结果调 PQfinish
 *（可先检查错误消息）。其他字段
 * 可能无效。status 非 BAD 则此阶段
 * 成功 - 调 PQconnectPoll 用 select(2)
 * 判断何时需调。
 *
 * 详见 PQconnectPoll。
                                                                                                                                                                                                                                                                                                                                                                          
*/
PGconn *
PQconnectStartParams(const char *const *keywords,
					 const char *const *values,
					 int expand_dbname)
{
	PGconn	   *conn;
	PQconninfoOption *connOptions;

	/*
		 * 为 conn struct 分配内存。期望
		 * conn->errorMessage 初始化空。
		 * 后续仅追加到该缓冲区。
		                                                                                            */
	conn = makeEmptyPGconn();
	if (conn == NULL)
		return NULL;

	/*
		 * 解析 conninfo 数组
		  */
	connOptions = conninfo_array_parse(keywords, values,
									   &conn->errorMessage,
									   true, expand_dbname);
	if (connOptions == NULL)
	{
		conn->status = CONNECTION_BAD;
		/* errorMessage 已被设置   */
		return conn;
	}

	/*
		 * 将选项值移入 conn struct
		       */
	if (!fillPGconn(conn, connOptions))
	{
		PQconninfoFree(connOptions);
		return conn;
	}

	/*
		 * 释放选项 - 全入 conn 了
		          */
	PQconninfoFree(connOptions);

	/*
		 * 计算派生选项
		    */
	if (!connectOptions2(conn))
		return conn;

	/*
		 * 连接数据库
		       */
	if (!connectDBStart(conn))
	{
		/* 防 connectDBStart 未设                          */
		conn->status = CONNECTION_BAD;
	}

	return conn;
}

/*
 *		PQconnectStart
 *
 * 用串中连接信息开始通过 postmaster
 * 建连到后端。
 *
 * 格式见 PQconnectdb 注释。
 *
 * 返回 PGconn*。NULL 即 malloc 错，
 * 不应继续。status 为 CONNECTION_BAD
 * 表示出错，应对结果调 PQfinish
 *（可先检查错误消息）。其他字段
 * 可能无效。status 非 BAD 则此阶段
 * 成功 - 调 PQconnectPoll 用 select(2)
 * 判断何时需调。
 *
 * 详见 PQconnectPoll。
                                                                                                                                                                                                                                                                                                                                                                               
*/
PGconn *
PQconnectStart(const char *conninfo)
{
	PGconn	   *conn;

	/*
		 * 为 conn struct 分配内存。期望
		 * conn->errorMessage 初始化空。
		 * 后续仅追加到该缓冲区。
		                                                                                            */
	conn = makeEmptyPGconn();
	if (conn == NULL)
		return NULL;

	/*
		 * 解析 conninfo 串
		     */
	if (!connectOptions1(conn, conninfo))
		return conn;

	/*
		 * 计算派生选项
		    */
	if (!connectOptions2(conn))
		return conn;

	/*
		 * 连接数据库
		       */
	if (!connectDBStart(conn))
	{
		/* 防 connectDBStart 未设                          */
		conn->status = CONNECTION_BAD;
	}

	return conn;
}

/*
 * 将选项值移入 conn struct
 *
 * 智能处理应放 connectOptions2。
 *
 * 成功返 true，失败返 false 设错。
                                                                                    
*/
static bool
fillPGconn(PGconn *conn, PQconninfoOption *connOptions)
{
	const internalPQconninfoOption *option;

	for (option = PQconninfoOptions; option->keyword; option++)
	{
		if (option->connofs >= 0)
		{
			const char *tmp = conninfo_getval(connOptions, option->keyword);

			if (tmp)
			{
				char	  **connmember = (char **) ((char *) conn + option->connofs);

				free(*connmember);
				*connmember = strdup(tmp);
				if (*connmember == NULL)
				{
					libpq_append_conn_error(conn, "out of memory");
					return false;
				}
			}
		}
	}

	return true;
}

/*
 *		connectOptions1
 *
 * 内部子过程：给定 PGconn 和 conninfo
 * 串设连接参数。派生由 connectOptions2
 * 处理。（分开因 PQsetdbLogin 中间
 * 覆盖默认值。）
 *
 * 成功返 true，出错返 false
 *（errorMessage 和 status 已设）。
                                                                                                                      */
static bool
connectOptions1(PGconn *conn, const char *conninfo)
{
	PQconninfoOption *connOptions;

	/*
		 * 解析 conninfo 串
		     */
	connOptions = parse_connection_string(conninfo, &conn->errorMessage, true);
	if (connOptions == NULL)
	{
		conn->status = CONNECTION_BAD;
		/* errorMessage 已被设置   */
		return false;
	}

	/*
		 * 将选项值移入 conn struct
		       */
	if (!fillPGconn(conn, connOptions))
	{
		conn->status = CONNECTION_BAD;
		PQconninfoFree(connOptions);
		return false;
	}

	/*
		 * 释放选项 - 全入 conn 了
		          */
	PQconninfoFree(connOptions);

	return true;
}

/*
 * 统计逗号分隔列表元素数。
                           */
static int
count_comma_separated_elems(const char *input)
{
	int			n;

	n = 1;
	for (; *input != '\0'; input++)
	{
		if (*input == ',')
			n++;
	}

	return n;
}

/*
 * 解析逗号分隔列表。
 *
 * 每次返下一元素 malloc 副本，
 * 设 *more 指示后续，更新 *startptr。
 *
 * OOM 返 NULL。
                                                                                                                                                      
*/
static char *
parse_comma_separated_list(char **startptr, bool *more)
{
	char	   *p;
	char	   *s = *startptr;
	char	   *e;
	size_t		len;

	/*
		 * 找当前元素尾：逗号或串尾。
		                                                    
*/
	e = s;
	while (*e != '\0' && *e != ',')
		++e;
	*more = (*e == ',');

	len = e - s;
	p = (char *) malloc(sizeof(char) * (len + 1));
	if (p)
	{
		memcpy(p, s, len);
		p[len] = '\0';
	}
	*startptr = e + 1;

	return p;
}

/*
 * 初始化 prng_state。若可能用高质量
 * 随机位作种，否则回退到基于地址、
 * 时间戳和 PID 的种子。
                                                                                                          
*/
static void
libpq_prng_init(PGconn *conn)
{
	uint64		rseed;
	struct timeval tval = {0};

	if (pg_prng_strong_seed(&conn->prng_state))
		return;

	gettimeofday(&tval, NULL);

	rseed = ((uintptr_t) conn) ^
		((uint64) getpid()) ^
		((uint64) tval.tv_usec) ^
		((uint64) tval.tv_sec);

	pg_prng_seed(&conn->prng_state, rseed);
}

/*
 *		connectOptions2
 *
 * 吸收所有用户信息后计派生连接选项。
 *
 * 成功返 true，出错返 false
 *（errorMessage 和 status 已设）。
                                                 */
static bool
connectOptions2(PGconn *conn)
{
	int			i;

	/*
		 * 为各可能主机分配内存。统计
		 * hostaddr/host 元素数。
		 * 均无则假定一个主机。
		                                                                                                  */
	conn->whichhost = 0;
	if (conn->pghostaddr && conn->pghostaddr[0] != '\0')
		conn->nconnhost = count_comma_separated_elems(conn->pghostaddr);
	else if (conn->pghost && conn->pghost[0] != '\0')
		conn->nconnhost = count_comma_separated_elems(conn->pghost);
	else
		conn->nconnhost = 1;
	conn->connhost = (pg_conn_host *)
		calloc(conn->nconnhost, sizeof(pg_conn_host));
	if (conn->connhost == NULL)
		goto oom_error;

	/*
		 * 每主机有 pg_conn_host。
		 * 拆参数字串填 host、hostaddr。
		                                                                         */
	if (conn->pghostaddr != NULL && conn->pghostaddr[0] != '\0')
	{
		char	   *s = conn->pghostaddr;
		bool		more = true;

		for (i = 0; i < conn->nconnhost && more; i++)
		{
			conn->connhost[i].hostaddr = parse_comma_separated_list(&s, &more);
			if (conn->connhost[i].hostaddr == NULL)
				goto oom_error;
		}

		/*
			 * 若有 hostaddr，数组按其元素
			 * 数分配，大小应正确。
			                                                                              
*/
		Assert(!more);
		Assert(i == conn->nconnhost);
	}

	if (conn->pghost != NULL && conn->pghost[0] != '\0')
	{
		char	   *s = conn->pghost;
		bool		more = true;

		for (i = 0; i < conn->nconnhost && more; i++)
		{
			conn->connhost[i].host = parse_comma_separated_list(&s, &more);
			if (conn->connhost[i].host == NULL)
				goto oom_error;
		}

		/* 检查主机条目数是否错误。  */
		if (more || i != conn->nconnhost)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "could not match %d host names to %d hostaddr values",
									count_comma_separated_elems(conn->pghost), conn->nconnhost);
			return false;
		}
	}

	/*
		 * 对各槽位识别地址类型，
		 * 无给定填默认地址。
		                                                  */
	for (i = 0; i < conn->nconnhost; i++)
	{
		pg_conn_host *ch = &conn->connhost[i];

		if (ch->hostaddr != NULL && ch->hostaddr[0] != '\0')
			ch->type = CHT_HOST_ADDRESS;
		else if (ch->host != NULL && ch->host[0] != '\0')
		{
			ch->type = CHT_HOST_NAME;
			if (is_unixsock_path(ch->host))
				ch->type = CHT_UNIX_SOCKET;
		}
		else
		{
			free(ch->host);

			/*
				 * 选择默认主机位。改此
				 * 也见 pg_regress。
				                                */
			if (DEFAULT_PGSOCKET_DIR[0])
			{
				ch->host = strdup(DEFAULT_PGSOCKET_DIR);
				ch->type = CHT_UNIX_SOCKET;
			}
			else
			{
				ch->host = strdup(DefaultHost);
				ch->type = CHT_HOST_NAME;
			}
			if (ch->host == NULL)
				goto oom_error;
		}
	}

	/*
		 * 确定各主机名对应端口号。
		 *
		 * 端口可 null 或空，读时替为
		 * DEF_PGPORT。
		                                                                                                                                            
*/
	if (conn->pgport != NULL && conn->pgport[0] != '\0')
	{
		char	   *s = conn->pgport;
		bool		more = true;

		for (i = 0; i < conn->nconnhost && more; i++)
		{
			conn->connhost[i].port = parse_comma_separated_list(&s, &more);
			if (conn->connhost[i].port == NULL)
				goto oom_error;
		}

		/*
			 * 仅一个端口则用于全部主机。
			 * 否则端口数须同主机数。
			                                               */
		if (i == 1 && !more)
		{
			for (i = 1; i < conn->nconnhost; i++)
			{
				conn->connhost[i].port = strdup(conn->connhost[0].port);
				if (conn->connhost[i].port == NULL)
					goto oom_error;
			}
		}
		else if (more || i != conn->nconnhost)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "could not match %d port numbers to %d hosts",
									count_comma_separated_elems(conn->pgport), conn->nconnhost);
			return false;
		}
	}

	/*
	 * 若未给出用户名则获取。（很可能获取失败，       
	 * 因为到达此处的唯一途径是 pg_fe_getauthname() 在         
	 * conninfo_add_defaults() 失败。现在需要错误消息。）    
	 */
	if (conn->pguser == NULL || conn->pguser[0] == '\0')
	{
		free(conn->pguser);
		conn->pguser = pg_fe_getauthname(&conn->errorMessage);
		if (!conn->pguser)
		{
			conn->status = CONNECTION_BAD;
			return false;
		}
	}

	/*
		 * 无数据库名则默认同用户名
		                        */
	if (conn->dbName == NULL || conn->dbName[0] == '\0')
	{
		free(conn->dbName);
		conn->dbName = strdup(conn->pguser);
		if (!conn->dbName)
			goto oom_error;
	}

	/*
		 * 无密码则在密码文件中查找。
		 * 每 host/port 结果可不同。
		                                                      */
	if (conn->pgpass == NULL || conn->pgpass[0] == '\0')
	{
		/* 若未指定密码文件，则使用 ~/PGPASSFILE   */
		if (conn->pgpassfile == NULL || conn->pgpassfile[0] == '\0')
		{
			char		homedir[MAXPGPATH];

			if (pqGetHomeDirectory(homedir, sizeof(homedir)))
			{
				free(conn->pgpassfile);
				conn->pgpassfile = malloc(MAXPGPATH);
				if (!conn->pgpassfile)
					goto oom_error;
				snprintf(conn->pgpassfile, MAXPGPATH, "%s/%s",
						 homedir, PGPASSFILE);
			}
		}

		if (conn->pgpassfile != NULL && conn->pgpassfile[0] != '\0')
		{
			for (i = 0; i < conn->nconnhost; i++)
			{
				/*
					 * 从文件获此主机密码。
					 * 用 host（或 hostaddr，
					 * 至少其一非空）搜索。
					                                                                          */
				const char *pwhost = conn->connhost[i].host;

				if (pwhost == NULL || pwhost[0] == '\0')
					pwhost = conn->connhost[i].hostaddr;

				conn->connhost[i].password =
					passwordFromFile(pwhost,
									 conn->connhost[i].port,
									 conn->dbName,
									 conn->pguser,
									 conn->pgpassfile);
			}
		}
	}

	/*
		 * 解析并验证 require_auth
		         */
	if (conn->require_auth && conn->require_auth[0])
	{
		char	   *s = conn->require_auth;
		bool		first,
					more;
		bool		negated = false;

		/*
			 * 默认空允许集逐步添加。
			                                       
*/
		conn->auth_required = true;
		conn->allowed_auth_methods = 0;

		for (first = true, more = true; more; first = false)
		{
			char	   *method,
					   *part;
			uint32		bits;

			part = parse_comma_separated_list(&s, &more);
			if (part == NULL)
				goto oom_error;

			/*
				 * 检查否定如 !password。
				 * 一否定则全须否定。
				                           */
			method = part;
			if (*method == '!')
			{
				if (first)
				{
					/*
						 * 切换许可集并减去。
						                                               
*/
					conn->auth_required = false;
					conn->allowed_auth_methods = -1;
				}
				else if (!negated)
				{
					conn->status = CONNECTION_BAD;
					libpq_append_conn_error(conn, "negative require_auth method \"%s\" cannot be mixed with non-negative methods",
											method);

					free(part);
					return false;
				}

				negated = true;
				method++;
			}
			else if (negated)
			{
				conn->status = CONNECTION_BAD;
				libpq_append_conn_error(conn, "require_auth method \"%s\" cannot be mixed with negative methods",
										method);

				free(part);
				return false;
			}

			if (strcmp(method, "password") == 0)
			{
				bits = (1 << AUTH_REQ_PASSWORD);
			}
			else if (strcmp(method, "md5") == 0)
			{
				bits = (1 << AUTH_REQ_MD5);
			}
			else if (strcmp(method, "gss") == 0)
			{
				bits = (1 << AUTH_REQ_GSS);
				bits |= (1 << AUTH_REQ_GSS_CONT);
			}
			else if (strcmp(method, "sspi") == 0)
			{
				bits = (1 << AUTH_REQ_SSPI);
				bits |= (1 << AUTH_REQ_GSS_CONT);
			}
			else if (strcmp(method, "scram-sha-256") == 0)
			{
				/* 目前假定仅 SCRAM 为 SASL                             */
				bits = (1 << AUTH_REQ_SASL);
				bits |= (1 << AUTH_REQ_SASL_CONT);
				bits |= (1 << AUTH_REQ_SASL_FIN);
			}
			else if (strcmp(method, "none") == 0)
			{
				/*
					 * 特殊：允许显式允许/禁止
					 * 服务器不认证时的连接，
					 * 如 trust/cert。
					                                                                                  */
				if (negated)	/* "!none" */
				{
					if (conn->auth_required)
						goto duplicate;

					conn->auth_required = true;
				}
				else			/* "none" */
				{
					if (!conn->auth_required)
						goto duplicate;

					conn->auth_required = false;
				}

				free(part);
				continue;		/* 避免下面的位掩码操作       */
			}
			else
			{
				conn->status = CONNECTION_BAD;
				libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
										"require_auth", method);

				free(part);
				return false;
			}

			/* 更新位掩码。  */
			if (negated)
			{
				if ((conn->allowed_auth_methods & bits) == 0)
					goto duplicate;

				conn->allowed_auth_methods &= ~bits;
			}
			else
			{
				if ((conn->allowed_auth_methods & bits) == bits)
					goto duplicate;

				conn->allowed_auth_methods |= bits;
			}

			free(part);
			continue;

	duplicate:

			/*
				 * 重复方法可能表明
				 * 设置有错别字。
				                                           */
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "require_auth method \"%s\" is specified more than once",
									part);

			free(part);
			return false;
		}
	}

	/*
		 * 验证 channel_binding
		        */
	if (conn->channel_binding)
	{
		if (strcmp(conn->channel_binding, "disable") != 0
			&& strcmp(conn->channel_binding, "prefer") != 0
			&& strcmp(conn->channel_binding, "require") != 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
									"channel_binding", conn->channel_binding);
			return false;
		}
	}
	else
	{
		conn->channel_binding = strdup(DefaultChannelBinding);
		if (!conn->channel_binding)
			goto oom_error;
	}

#ifndef USE_SSL

	/*
		 * sslrootcert=system 不支持。
		 * 因改默认 sslmode，
		 * 先于 sslmode 验证检查。
		                                                                                                                 */
	if (conn->sslrootcert
		&& strcmp(conn->sslrootcert, "system") == 0)
	{
		conn->status = CONNECTION_BAD;
		libpq_append_conn_error(conn, "%s value \"%s\" invalid when SSL support is not compiled in",
								"sslrootcert", conn->sslrootcert);
		return false;
	}
#endif

	/*
		 * 验证 sslmode
		        */
	if (conn->sslmode)
	{
		if (strcmp(conn->sslmode, "disable") != 0
			&& strcmp(conn->sslmode, "allow") != 0
			&& strcmp(conn->sslmode, "prefer") != 0
			&& strcmp(conn->sslmode, "require") != 0
			&& strcmp(conn->sslmode, "verify-ca") != 0
			&& strcmp(conn->sslmode, "verify-full") != 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
									"sslmode", conn->sslmode);
			return false;
		}

#ifndef USE_SSL
		switch (conn->sslmode[0])
		{
			case 'a':			/* "allow" */
			case 'p':			/* "prefer" */

				/*
					 * 警告：未编译 SSL，
					 * SSL 永不协商？
					                                         */
				break;

			case 'r':			/* "require" */
			case 'v':			/* "verify-ca"或"verify-full"  */
				conn->status = CONNECTION_BAD;
				libpq_append_conn_error(conn, "%s value \"%s\" invalid when SSL support is not compiled in",
										"sslmode", conn->sslmode);
				return false;
		}
#endif
	}
	else
	{
		conn->sslmode = strdup(DefaultSSLMode);
		if (!conn->sslmode)
			goto oom_error;
	}

#ifdef USE_SSL

	/*
	 * 如果 sslrootcert=system，请确保所选的 sslmode 兼容。  
	 */
	if (conn->sslrootcert
		&& strcmp(conn->sslrootcert, "system") == 0
		&& strcmp(conn->sslmode, "verify-full") != 0)
	{
		conn->status = CONNECTION_BAD;
		libpq_append_conn_error(conn, "weak sslmode \"%s\" may not be used with sslrootcert=system (use \"verify-full\")",
								conn->sslmode);
		return false;
	}
#endif

	/*
		 * 验证 SSL 协议版本范围
		                                                              
*/
	if (!sslVerifyProtocolVersion(conn->ssl_min_protocol_version))
	{
		conn->status = CONNECTION_BAD;
		libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
								"ssl_min_protocol_version",
								conn->ssl_min_protocol_version);
		return false;
	}
	if (!sslVerifyProtocolVersion(conn->ssl_max_protocol_version))
	{
		conn->status = CONNECTION_BAD;
		libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
								"ssl_max_protocol_version",
								conn->ssl_max_protocol_version);
		return false;
	}

	/*
		 * 检查 SSL 协议范围。早期步骤
		 * 独立于实现，免浪费周期。
		                                                                           
                                                                         
                                                                         
*/
	if (!sslVerifyProtocolRange(conn->ssl_min_protocol_version,
								conn->ssl_max_protocol_version))
	{
		conn->status = CONNECTION_BAD;
		libpq_append_conn_error(conn, "invalid SSL protocol version range");
		return false;
	}

	/*
		 * 验证 sslcertmode
		        */
	if (conn->sslcertmode)
	{
		if (strcmp(conn->sslcertmode, "disable") != 0 &&
			strcmp(conn->sslcertmode, "allow") != 0 &&
			strcmp(conn->sslcertmode, "require") != 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
									"sslcertmode", conn->sslcertmode);
			return false;
		}
#ifndef USE_SSL
		if (strcmp(conn->sslcertmode, "require") == 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "%s value \"%s\" invalid when SSL support is not compiled in",
									"sslcertmode", conn->sslcertmode);
			return false;
		}
#endif
#ifndef HAVE_SSL_CTX_SET_CERT_CB

		/*
			 * 无证书回调无法判断是否
			 * 请求证书，"require"无用。
			                                                                            
*/
		if (strcmp(conn->sslcertmode, "require") == 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "%s value \"%s\" is not supported (check OpenSSL version)",
									"sslcertmode", conn->sslcertmode);
			return false;
		}
#endif
	}
	else
	{
		conn->sslcertmode = strdup(DefaultSSLCertMode);
		if (!conn->sslcertmode)
			goto oom_error;
	}

	/*
		 * 验证 gssencmode
		        */
	if (conn->gssencmode)
	{
		if (strcmp(conn->gssencmode, "disable") != 0 &&
			strcmp(conn->gssencmode, "prefer") != 0 &&
			strcmp(conn->gssencmode, "require") != 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "invalid %s value: \"%s\"", "gssencmode", conn->gssencmode);
			return false;
		}
#ifndef ENABLE_GSS
		if (strcmp(conn->gssencmode, "require") == 0)
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "gssencmode value \"%s\" invalid when GSSAPI support is not compiled in",
									conn->gssencmode);
			return false;
		}
#endif
	}
	else
	{
		conn->gssencmode = strdup(DefaultGSSMode);
		if (!conn->gssencmode)
			goto oom_error;
	}

	/*
	 * 校验 target_session_attrs 选项并设置 target_server_type  
	 */
	if (conn->target_session_attrs)
	{
		if (strcmp(conn->target_session_attrs, "any") == 0)
			conn->target_server_type = SERVER_TYPE_ANY;
		else if (strcmp(conn->target_session_attrs, "read-write") == 0)
			conn->target_server_type = SERVER_TYPE_READ_WRITE;
		else if (strcmp(conn->target_session_attrs, "read-only") == 0)
			conn->target_server_type = SERVER_TYPE_READ_ONLY;
		else if (strcmp(conn->target_session_attrs, "primary") == 0)
			conn->target_server_type = SERVER_TYPE_PRIMARY;
		else if (strcmp(conn->target_session_attrs, "standby") == 0)
			conn->target_server_type = SERVER_TYPE_STANDBY;
		else if (strcmp(conn->target_session_attrs, "prefer-standby") == 0)
			conn->target_server_type = SERVER_TYPE_PREFER_STANDBY;
		else
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
									"target_session_attrs",
									conn->target_session_attrs);
			return false;
		}
	}
	else
		conn->target_server_type = SERVER_TYPE_ANY;

	/*
	 * 校验 load_balance_hosts 选项并设置 load_balance_type  
	 */
	if (conn->load_balance_hosts)
	{
		if (strcmp(conn->load_balance_hosts, "disable") == 0)
			conn->load_balance_type = LOAD_BALANCE_DISABLE;
		else if (strcmp(conn->load_balance_hosts, "random") == 0)
			conn->load_balance_type = LOAD_BALANCE_RANDOM;
		else
		{
			conn->status = CONNECTION_BAD;
			libpq_append_conn_error(conn, "invalid %s value: \"%s\"",
									"load_balance_hosts",
									conn->load_balance_hosts);
			return false;
		}
	}
	else
		conn->load_balance_type = LOAD_BALANCE_DISABLE;

	if (conn->load_balance_type == LOAD_BALANCE_RANDOM)
	{
		libpq_prng_init(conn);

		/*
			 * Fisher-Yates"由内而外"变体。
			 * 新值追加后再与随机元素交换
			 *（可含自身）。交换合并插入。
			                                                                              
                                                                            
                                                                            
*/
		for (i = 1; i < conn->nconnhost; i++)
		{
			int			j = pg_prng_uint64_range(&conn->prng_state, 0, i);
			pg_conn_host temp = conn->connhost[j];

			conn->connhost[j] = conn->connhost[i];
			conn->connhost[i] = temp;
		}
	}

	/*
		 * 从 locale 解析 "auto"encoding
		                     */
	if (conn->client_encoding_initial &&
		strcmp(conn->client_encoding_initial, "auto") == 0)
	{
		free(conn->client_encoding_initial);
		conn->client_encoding_initial = strdup(pg_encoding_to_char(pg_get_encoding_from_locale(NULL, true)));
		if (!conn->client_encoding_initial)
			goto oom_error;
	}

	/*
		 * 到此方可连。（需状态标志
		 * 非布尔，防 PQreset()。）
		                                                                                                                      
*/
	conn->options_valid = true;

	return true;

oom_error:
	conn->status = CONNECTION_BAD;
	libpq_append_conn_error(conn, "out of memory");
	return false;
}

/*
 *		PQconndefaults
 *
 * 构造默认连接选项数组，标识所有
 * 可用选项及从环境等获取的默认值。
 * 出错（如 OOM）返 NULL。
 *
 * 应用可确定所有可能选项及当前
 * 默认值。
 *
 * 注意：PG 7.0 起数组动态分配，
 * 不再需时用 PQconninfoFree() 释放。
 *（此前静态但非线程安全。）旧应用
 * 有少量内存泄漏直到更新。
                                                                                                                                                                                                                                                                                                                 
*/
PQconninfoOption *
PQconndefaults(void)
{
	PQExpBufferData errorBuf;
	PQconninfoOption *connOptions;

	/* 不报错但被调用方需缓冲                                   */
	initPQExpBuffer(&errorBuf);
	if (PQExpBufferDataBroken(errorBuf))
		return NULL;			/* 内存已耗尽 :-(       */

	connOptions = conninfo_init(&errorBuf);
	if (connOptions != NULL)
	{
		/* 传 NULL 忽略错误               */
		if (!conninfo_add_defaults(connOptions, NULL))
		{
			PQconninfoFree(connOptions);
			connOptions = NULL;
		}
	}

	termPQExpBuffer(&errorBuf);
	return connOptions;
}

/* ----------------
 *		PQsetdbLogin
 *
 * 通过 postmaster 在指定主机和端口
 * 建连到 postgres 后端。
 *
 * 返回 PGconn*，后续 libpq 需它
 *
 * status CONNECTION_BAD 时可能
 * 仅 errorMessage 有用。
 * ----------------
                                                                                                                     */
PGconn *
PQsetdbLogin(const char *pghost, const char *pgport, const char *pgoptions,
			 const char *pgtty, const char *dbName, const char *login,
			 const char *pwd)
{
	PGconn	   *conn;

	/*
		 * 为 conn struct 分配内存。期望
		 * conn->errorMessage 初始化空。
		 * 后续仅追加到该缓冲区。
		                                                                                            */
	conn = makeEmptyPGconn();
	if (conn == NULL)
		return NULL;

	/*
		 * 若 dbName 似连接串，
		 * 用 connectOptions1 解析。
		                                                            */
	if (dbName && recognized_connection_string(dbName))
	{
		if (!connectOptions1(conn, dbName))
			return conn;
	}
	else
	{
		/*
		 * 老式路径：首先解析一个空 conninfo 字符串，以便 
		 * 设置与 PQconnectdb() 所用相同的默认值。   
		 */
		if (!connectOptions1(conn, ""))
			return conn;

		/* 将 dbName 参数值插入结构体       */
		if (dbName && dbName[0] != '\0')
		{
			free(conn->dbName);
			conn->dbName = strdup(dbName);
			if (!conn->dbName)
				goto oom_error;
		}
	}

	/*
	 * 把其余参数插入结构体，覆盖默认值（以及 dbName     
	 * 作为 conninfo 时带来的冲突数据）。           
	 */
	if (pghost && pghost[0] != '\0')
	{
		free(conn->pghost);
		conn->pghost = strdup(pghost);
		if (!conn->pghost)
			goto oom_error;
	}

	if (pgport && pgport[0] != '\0')
	{
		free(conn->pgport);
		conn->pgport = strdup(pgport);
		if (!conn->pgport)
			goto oom_error;
	}

	if (pgoptions && pgoptions[0] != '\0')
	{
		free(conn->pgoptions);
		conn->pgoptions = strdup(pgoptions);
		if (!conn->pgoptions)
			goto oom_error;
	}

	if (login && login[0] != '\0')
	{
		free(conn->pguser);
		conn->pguser = strdup(login);
		if (!conn->pguser)
			goto oom_error;
	}

	if (pwd && pwd[0] != '\0')
	{
		free(conn->pgpass);
		conn->pgpass = strdup(pwd);
		if (!conn->pgpass)
			goto oom_error;
	}

	/*
		 * 计算派生选项
		    */
	if (!connectOptions2(conn))
		return conn;

	/*
		 * 连接数据库
		       */
	if (connectDBStart(conn))
		(void) connectDBComplete(conn);

	return conn;

oom_error:
	conn->status = CONNECTION_BAD;
	libpq_append_conn_error(conn, "out of memory");
	return conn;
}


/* ----------
 * connectNoDelay -
 * 设 TCP_NODELAY 套接字选项。
 * 成功返 1，失败返 0。
 * ----------
        */
static int
connectNoDelay(PGconn *conn)
{
#ifdef	TCP_NODELAY
	int			on = 1;

	if (setsockopt(conn->sock, IPPROTO_TCP, TCP_NODELAY,
				   (char *) &on,
				   sizeof(on)) < 0)
	{
		char		sebuf[PG_STRERROR_R_BUFLEN];

		libpq_append_conn_error(conn, "could not set socket to TCP no delay mode: %s",
								SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
		return 0;
	}
#endif

	return 1;
}

/* ----------
 * 将当前连接的 IP 地址写入 host_addr（长度 host_addr_len）。  
 * 若做不到，则设为空字符串。  
 * ----------
 */
static void
getHostaddr(PGconn *conn, char *host_addr, int host_addr_len)
{
	struct sockaddr_storage *addr = &conn->raddr.addr;

	if (addr->ss_family == AF_INET)
	{
		if (pg_inet_net_ntop(AF_INET,
							 &((struct sockaddr_in *) addr)->sin_addr.s_addr,
							 32,
							 host_addr, host_addr_len) == NULL)
			host_addr[0] = '\0';
	}
	else if (addr->ss_family == AF_INET6)
	{
		if (pg_inet_net_ntop(AF_INET6,
							 &((struct sockaddr_in6 *) addr)->sin6_addr.s6_addr,
							 128,
							 host_addr, host_addr_len) == NULL)
			host_addr[0] = '\0';
	}
	else
		host_addr[0] = '\0';
}

/*
 * emitHostIdentityInfo -
 * 识别目标地址后推测性追加
 * "connection to server ... failed: "
 * 到 conn->errorMessage。确保后续
 * 错误归属正确服务器。conn->raddr
 * 须有效且需 getHostaddr() 结果。
                                                                                                                                                  */
static void
emitHostIdentityInfo(PGconn *conn, const char *host_addr)
{
	if (conn->raddr.addr.ss_family == AF_UNIX)
	{
		char		service[NI_MAXHOST];

		pg_getnameinfo_all(&conn->raddr.addr, conn->raddr.salen,
						   NULL, 0,
						   service, sizeof(service),
						   NI_NUMERICSERV);
		appendPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("connection to server on socket \"%s\" failed: "),
						  service);
	}
	else
	{
		const char *displayed_host;
		const char *displayed_port;

		/* 我们实际连接的主机和端口是哪些？    */
		if (conn->connhost[conn->whichhost].type == CHT_HOST_ADDRESS)
			displayed_host = conn->connhost[conn->whichhost].hostaddr;
		else
			displayed_host = conn->connhost[conn->whichhost].host;
		displayed_port = conn->connhost[conn->whichhost].port;
		if (displayed_port == NULL || displayed_port[0] == '\0')
			displayed_port = DEF_PGPORT_STR;

		/*
			 * 若用户未用 hostaddr 提供 IP
			 * 且 host 缺或与查找不匹配，
			 * 显示查得 IP。
			                                                    */
		if (conn->connhost[conn->whichhost].type != CHT_HOST_ADDRESS &&
			host_addr[0] &&
			strcmp(displayed_host, host_addr) != 0)
			appendPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("connection to server at \"%s\" (%s), port %s failed: "),
							  displayed_host, host_addr,
							  displayed_port);
		else
			appendPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("connection to server at \"%s\", port %s failed: "),
							  displayed_host,
							  displayed_port);
	}
}

/* ----------
 * connectFailureMessage -
 * 连接失败时用给定 errno 创建
 * 友好错误消息。用于暗示无服务器。
 * ----------
                                                                   
*/
static void
connectFailureMessage(PGconn *conn, int errorno)
{
	char		sebuf[PG_STRERROR_R_BUFLEN];

	appendPQExpBuffer(&conn->errorMessage,
					  "%s\n",
					  SOCK_STRERROR(errorno, sebuf, sizeof(sebuf)));

	if (conn->raddr.addr.ss_family == AF_UNIX)
		libpq_append_conn_error(conn, "\tIs the server running locally and accepting connections on that socket?");
	else
		libpq_append_conn_error(conn, "\tIs the server running on that host and accepting TCP/IP connections?");
}

/*
 * 应用保活？是返 1，否返 0；
 * keepalives 非整数则返 -1。
                                                                     
*/
static int
useKeepalives(PGconn *conn)
{
	int			val;

	if (conn->keepalives == NULL)
		return 1;

	if (!parse_int_param(conn->keepalives, &val, conn, "keepalives"))
		return -1;

	return val != 0 ? 1 : 0;
}

/*
 * 解析 "value" 为整数，成功则存
 * *result；有尾随垃圾或溢出报错。
                                                                                                                                         
*/
static bool
parse_int_param(const char *value, int *result, PGconn *conn,
				const char *context)
{
	char	   *end;
	long		numval;

	Assert(value != NULL);

	*result = 0;

	/* strtol(3) 跳过前导空白字符  */
	errno = 0;
	numval = strtol(value, &end, 10);

	/*
	 * 解析无进展或出错则失败。
	 * 可正检溢出。
	                                                                 */
	if (value == end || errno != 0 || numval != (int) numval)
		goto error;

	/*
	 * 跳尾随空格；终止符前
	 * 非空格残留则失败
	                                                 */
	while (*end != '\0' && isspace((unsigned char) *end))
		end++;

	if (*end != '\0')
		goto error;

	*result = numval;
	return true;

error:
	libpq_append_conn_error(conn, "invalid integer value \"%s\" for connection option \"%s\"",
							value, context);
	return false;
}

#ifndef WIN32
/*
 * 设保活空闲定时器。
   */
static int
setKeepalivesIdle(PGconn *conn)
{
	int			idle;

	if (conn->keepalives_idle == NULL)
		return 1;

	if (!parse_int_param(conn->keepalives_idle, &idle, conn,
						 "keepalives_idle"))
		return 0;
	if (idle < 0)
		idle = 0;

#ifdef PG_TCP_KEEPALIVE_IDLE
	if (setsockopt(conn->sock, IPPROTO_TCP, PG_TCP_KEEPALIVE_IDLE,
				   (char *) &idle, sizeof(idle)) < 0)
	{
		char		sebuf[PG_STRERROR_R_BUFLEN];

		libpq_append_conn_error(conn, "%s(%s) failed: %s",
								"setsockopt",
								PG_TCP_KEEPALIVE_IDLE_STR,
								SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
		return 0;
	}
#endif

	return 1;
}

/*
 * 设保活间隔。
          */
static int
setKeepalivesInterval(PGconn *conn)
{
	int			interval;

	if (conn->keepalives_interval == NULL)
		return 1;

	if (!parse_int_param(conn->keepalives_interval, &interval, conn,
						 "keepalives_interval"))
		return 0;
	if (interval < 0)
		interval = 0;

#ifdef TCP_KEEPINTVL
	if (setsockopt(conn->sock, IPPROTO_TCP, TCP_KEEPINTVL,
				   (char *) &interval, sizeof(interval)) < 0)
	{
		char		sebuf[PG_STRERROR_R_BUFLEN];

		libpq_append_conn_error(conn, "%s(%s) failed: %s",
								"setsockopt",
								"TCP_KEEPINTVL",
								SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
		return 0;
	}
#endif

	return 1;
}

/*
 * 设触发断连的丢包数。
                                                  
*/
static int
setKeepalivesCount(PGconn *conn)
{
	int			count;

	if (conn->keepalives_count == NULL)
		return 1;

	if (!parse_int_param(conn->keepalives_count, &count, conn,
						 "keepalives_count"))
		return 0;
	if (count < 0)
		count = 0;

#ifdef TCP_KEEPCNT
	if (setsockopt(conn->sock, IPPROTO_TCP, TCP_KEEPCNT,
				   (char *) &count, sizeof(count)) < 0)
	{
		char		sebuf[PG_STRERROR_R_BUFLEN];

		libpq_append_conn_error(conn, "%s(%s) failed: %s",
								"setsockopt",
								"TCP_KEEPCNT",
								SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
		return 0;
	}
#endif

	return 1;
}
#else							/* WIN32 */
#ifdef SIO_KEEPALIVE_VALS
/*
 * Win32 启用保活并设保活值。
 *
 * 注意：需信号安全，因 PQcancel 用。
                                                                                    
*/
static int
setKeepalivesWin32(pgsocket sock, int idle, int interval)
{
	struct tcp_keepalive ka;
	DWORD		retsize;

	if (idle <= 0)
		idle = 2 * 60 * 60;		/* 2小时=默认值 */
	if (interval <= 0)
		interval = 1;			/* 1 秒 = 默认值  */

	ka.onoff = 1;
	ka.keepalivetime = idle * 1000;
	ka.keepaliveinterval = interval * 1000;

	if (WSAIoctl(sock,
				 SIO_KEEPALIVE_VALS,
				 (LPVOID) &ka,
				 sizeof(ka),
				 NULL,
				 0,
				 &retsize,
				 NULL,
				 NULL)
		!= 0)
		return 0;
	return 1;
}

static int
prepKeepalivesWin32(PGconn *conn)
{
	int			idle = -1;
	int			interval = -1;

	if (conn->keepalives_idle &&
		!parse_int_param(conn->keepalives_idle, &idle, conn,
						 "keepalives_idle"))
		return 0;
	if (conn->keepalives_interval &&
		!parse_int_param(conn->keepalives_interval, &interval, conn,
						 "keepalives_interval"))
		return 0;

	if (!setKeepalivesWin32(conn->sock, idle, interval))
	{
		libpq_append_conn_error(conn, "%s(%s) failed: error code %d",
								"WSAIoctl", "SIO_KEEPALIVE_VALS",
								WSAGetLastError());
		return 0;
	}
	return 1;
}
#endif							/* SIO_KEEPALIVE_VALS */
#endif							/* WIN32 */

/*
 * 设 TCP 用户超时。
   */
static int
setTCPUserTimeout(PGconn *conn)
{
	int			timeout;

	if (conn->pgtcp_user_timeout == NULL)
		return 1;

	if (!parse_int_param(conn->pgtcp_user_timeout, &timeout, conn,
						 "tcp_user_timeout"))
		return 0;

	if (timeout < 0)
		timeout = 0;

#ifdef TCP_USER_TIMEOUT
	if (setsockopt(conn->sock, IPPROTO_TCP, TCP_USER_TIMEOUT,
				   (char *) &timeout, sizeof(timeout)) < 0)
	{
		char		sebuf[256];

		libpq_append_conn_error(conn, "%s(%s) failed: %s",
								"setsockopt",
								"TCP_USER_TIMEOUT",
								SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
		return 0;
	}
#endif

	return 1;
}

/* ----------
 * connectDBStart -
 *		开始建连到后端。
 *
 * 成功返 1，失败返 0。
 * ----------
                                       */
static int
connectDBStart(PGconn *conn)
{
	if (!conn)
		return 0;

	if (!conn->options_valid)
		goto connect_errReturn;

	/*
	 * 检查是否错误链接 src/common
	 * 后端内部版（见 link-canary.c）。
	 * 仅开发者见，不翻译。
	                                                                                                                 
*/
	if (!pg_link_canary_is_frontend())
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 "libpq is incorrectly linked to backend functions\n");
		goto connect_errReturn;
	}

	/* 确保缓冲区为空        */
	conn->inStart = conn->inCursor = conn->inEnd = 0;
	conn->outCount = 0;

	/*
	 * 设尝试连第一主机。
	 *（whichhost=-1 不妥但
	 * PQconnectPoll 会推到 0。）
	                                                                                */
	conn->whichhost = -1;
	conn->try_next_addr = false;
	conn->try_next_host = true;
	conn->status = CONNECTION_NEEDED;

	/* 也重置 target_server_type 如需               */
	if (conn->target_server_type == SERVER_TYPE_PREFER_STANDBY_PASS2)
		conn->target_server_type = SERVER_TYPE_PREFER_STANDBY;

	/*
	 * CONNECTION_NEEDED 处理在
	 * PQconnectPoll() 中便于重执行。
	 * 但须在此运行一次，因调用者
	 * 期望成功返回意味
	 * PGRES_POLLING_WRITING 状态。
	                                                                                                                                                                */
	if (PQconnectPoll(conn) == PGRES_POLLING_WRITING)
		return 1;

connect_errReturn:

	/*
	 * 若开套接字立关不等 PQfinish。
	 *（应用未通过 PQsocket 获取
	 * 故不破坏。）
	                                                                                                        */
	pqDropConnection(conn, true);
	conn->status = CONNECTION_BAD;
	return 0;
}


/*
 *		connectDBComplete
 *
 * 阻塞完成连接。
 *
 * 成功返 1，失败返 0。
                   */
static int
connectDBComplete(PGconn *conn)
{
	PostgresPollingStatusType flag = PGRES_POLLING_WRITING;
	time_t		finish_time = ((time_t) -1);
	int			timeout = 0;
	int			last_whichhost = -2;	/* 定非 whichhost                   */
	int			last_whichaddr = -2;	/* 定非 whichaddr                   */

	if (conn == NULL || conn->status == CONNECTION_BAD)
		return 0;

	/*
	 * connect_timeout 非零设时限。
	                  */
	if (conn->connect_timeout != NULL)
	{
		if (!parse_int_param(conn->connect_timeout, &timeout, conn,
							 "connect_timeout"))
		{
			/* 将连接标记为坏连接，以报告解析失败      */
			conn->status = CONNECTION_BAD;
			return 0;
		}

		if (timeout > 0)
		{
			/*
			 * 舍入可致过快失败；
			 * 坚持至少 2 秒。
			                                                                                             
*/
			if (timeout < 2)
				timeout = 2;
		}
		else					/* 负值表示 0   */
			timeout = 0;
	}

	for (;;)
	{
		int			ret = 0;

		/*
		 * connect_timeout 活跃且考
		 * 不同主机则重启动。已成功
		 * 不重算。
		                                                                                                       */
		if (flag != PGRES_POLLING_OK &&
			timeout > 0 &&
			(conn->whichhost != last_whichhost ||
			 conn->whichaddr != last_whichaddr))
		{
			finish_time = time(NULL) + timeout;
			last_whichhost = conn->whichhost;
			last_whichaddr = conn->whichaddr;
		}

		/*
		 * 必要时等。初始态 (PQconnectStart
		 * 后) 是等可写。
		                                                                 */
		switch (flag)
		{
			case PGRES_POLLING_OK:
				return 1;		/* 成功！*/

			case PGRES_POLLING_READING:
				ret = pqWaitTimed(1, 0, conn, finish_time);
				if (ret == -1)
				{
					/* 硬故障（例如 select() 问题）会中止一切 */
					conn->status = CONNECTION_BAD;
					return 0;
				}
				break;

			case PGRES_POLLING_WRITING:
				ret = pqWaitTimed(0, 1, conn, finish_time);
				if (ret == -1)
				{
					/* 硬故障（例如 select() 问题）会中止一切 */
					conn->status = CONNECTION_BAD;
					return 0;
				}
				break;

			default:
				/* 防 PQconnectPoll 未设                          */
				conn->status = CONNECTION_BAD;
				return 0;
		}

		if (ret == 1)			/* connect_timeout 已到  */
		{
			/*
			 * 弃当前服/址，试下个。
			                      */
			conn->try_next_addr = true;
			conn->status = CONNECTION_NEEDED;
		}

		/*
		 * 试推进状态机。
		                 */
		flag = PQconnectPoll(conn);
	}
}

/* ----------------
 *		PQconnectPoll
 *
 * 轮询异步连接。
 *
 * 返回 PostgresPollingStatusType。
 * 调前用 select(2) 确数据已到。
 *
 * 无论成败必调 PQfinish。
 *
 * 与 PQconnectStart 配合实现非阻塞
 * 连接。注意事项：
 *
 *	 o	调 PQtrace 需确保流不阻塞。
 *	 o	不提供 IP（仅主机名）则
 *		PQconnectStart 在 getaddrinfo
 *		上阻塞。用 Unix 套接字
 *		（既不提供主机名也不提供
 *		主机地址）则无问题。
 *	 o	后端需 Kerberos 认证时必
 *		须同时提供主机名和地址，
 *		否则可能在 gethostname 上
 *		阻塞。
 *
 * ----------------
                                                                                                                                                                                                                                                                                                                                                                              
*/
PostgresPollingStatusType
PQconnectPoll(PGconn *conn)
{
	bool		reset_connection_state_machine = false;
	bool		need_new_connection = false;
	PGresult   *res;
	char		sebuf[PG_STRERROR_R_BUFLEN];
	int			optval;

	if (conn == NULL)
		return PGRES_POLLING_FAILED;

	/* 获取新数据  */
	switch (conn->status)
	{
			/*
			 * 这两种不应轮询但可处理。
			                                                
*/
		case CONNECTION_BAD:
			return PGRES_POLLING_FAILED;
		case CONNECTION_OK:
			return PGRES_POLLING_OK;

			/* 读状态                */
		case CONNECTION_AWAITING_RESPONSE:
		case CONNECTION_AUTH_OK:
		case CONNECTION_CHECK_WRITABLE:
		case CONNECTION_CONSUME:
		case CONNECTION_CHECK_STANDBY:
			{
				/* 读等待数据   */
				int			n = pqReadData(conn);

				if (n < 0)
					goto error_return;
				if (n == 0)
					return PGRES_POLLING_READING;

				break;
			}

			/* 这些是写入状态，我们只需继续。 */
		case CONNECTION_STARTED:
		case CONNECTION_MADE:
			break;

			/* 特例：无需等待，直接继续。 */
		case CONNECTION_SSL_STARTUP:
		case CONNECTION_NEEDED:
		case CONNECTION_GSS_STARTUP:
		case CONNECTION_CHECK_TARGET:
			break;

		default:
			libpq_append_conn_error(conn, "invalid connection state, probably indicative of memory corruption");
			goto error_return;
	}


keep_going:						/* 回此处直到无事可做                                            
*/

	/* 推进下一地址或主机？                                      */
	if (conn->try_next_addr)
	{
		if (conn->whichaddr < conn->naddr)
		{
			conn->whichaddr++;
			reset_connection_state_machine = true;
		}
		else
			conn->try_next_host = true;
		conn->try_next_addr = false;
	}

	/* 该前进到下一个 connhost[] 吗？   */
	if (conn->try_next_host)
	{
		pg_conn_host *ch;
		struct addrinfo hint;
		struct addrinfo *addrlist;
		int			thisport;
		int			ret;
		char		portstr[MAXPGPATH];

		if (conn->whichhost + 1 < conn->nconnhost)
			conn->whichhost++;
		else
		{
			/*
			 * 无更多主机。
			 *
			 * prefer-standby 模式则
			 * 弃备库要求重来。
			 *
			 * 否则错误已设，仅需
			 * 设正确状态。
			                                                                                                              */
			if (conn->target_server_type == SERVER_TYPE_PREFER_STANDBY &&
				conn->nconnhost > 0)
			{
				conn->target_server_type = SERVER_TYPE_PREFER_STANDBY_PASS2;
				conn->whichhost = 0;
			}
			else
				goto error_return;
		}

		/* 丢弃前一个主机的地址信息    */
		release_conn_addrinfo(conn);

		/*
		 * 查新主机信息。失败记到
		 * errorMessage 循环试下个。
		                                                                                                                         
*/
		ch = &conn->connhost[conn->whichhost];

		/* 初始化提示结构     */
		MemSet(&hint, 0, sizeof(hint));
		hint.ai_socktype = SOCK_STREAM;
		hint.ai_family = AF_UNSPEC;

		/* 算出我们将要使用的端口号。        */
		if (ch->port == NULL || ch->port[0] == '\0')
			thisport = DEF_PGPORT;
		else
		{
			if (!parse_int_param(ch->port, &thisport, conn, "port"))
				goto error_return;

			if (thisport < 1 || thisport > 65535)
			{
				libpq_append_conn_error(conn, "invalid port number: \"%s\"", ch->port);
				goto keep_going;
			}
		}
		snprintf(portstr, sizeof(portstr), "%d", thisport);

		/* 使用 pg_getaddrinfo_all() 解析地址        */
		switch (ch->type)
		{
			case CHT_HOST_NAME:
				ret = pg_getaddrinfo_all(ch->host, portstr, &hint,
										 &addrlist);
				if (ret || !addrlist)
				{
					libpq_append_conn_error(conn, "could not translate host name \"%s\" to address: %s",
											ch->host, gai_strerror(ret));
					goto keep_going;
				}
				break;

			case CHT_HOST_ADDRESS:
				hint.ai_flags = AI_NUMERICHOST;
				ret = pg_getaddrinfo_all(ch->hostaddr, portstr, &hint,
										 &addrlist);
				if (ret || !addrlist)
				{
					libpq_append_conn_error(conn, "could not parse network address \"%s\": %s",
											ch->hostaddr, gai_strerror(ret));
					goto keep_going;
				}
				break;

			case CHT_UNIX_SOCKET:
				hint.ai_family = AF_UNIX;
				UNIXSOCK_PATH(portstr, thisport, ch->host);
				if (strlen(portstr) >= UNIXSOCK_PATH_BUFLEN)
				{
					libpq_append_conn_error(conn, "Unix-domain socket path \"%s\" is too long (maximum %d bytes)",
											portstr,
											(int) (UNIXSOCK_PATH_BUFLEN - 1));
					goto keep_going;
				}

				/*
				 * NULL 主机让 pg_getaddrinfo_all
				 * 解析为 Unix 域套接字。
				                               */
				ret = pg_getaddrinfo_all(NULL, portstr, &hint,
										 &addrlist);
				if (ret || !addrlist)
				{
					libpq_append_conn_error(conn, "could not translate Unix-domain socket path \"%s\" to address: %s",
											portstr, gai_strerror(ret));
					goto keep_going;
				}
				break;
		}

		/*
		 * 私有内存存 addrlist 副本
		 * 以便负载均衡随机化。
		                                     */
		ret = store_conn_addrinfo(conn, addrlist);
		pg_freeaddrinfo_all(hint.ai_family, addrlist);
		if (ret)
			goto error_return;	/* 消息已录           */

		/*
		 * 随机负载均衡则打乱地址。
		                          */
		if (conn->load_balance_type == LOAD_BALANCE_RANDOM)
		{
			/*
			 * Fisher-Yates"由内而外"。
			 * 新值追加再与随机元素交换
			 *（可含自身）。交换合插入。
			 *
			 * 无需此初始化 prng_state，
			 * connectOptions2 已做。
			                                                                                                  
                                                                                                 
                                                                                                
*/
			for (int i = 1; i < conn->naddr; i++)
			{
				int			j = pg_prng_uint64_range(&conn->prng_state, 0, i);
				AddrInfo	temp = conn->addr[j];

				conn->addr[j] = conn->addr[i];
				conn->addr[i] = temp;
			}
		}

		reset_connection_state_machine = true;
		conn->try_next_host = false;
	}

	/* 重置连接状态机？        */
	if (reset_connection_state_machine)
	{
		/*
		 * (重)初始化连接控制变量，用于
		 * 单服务器地址的若干尝试。
		 * 各尝试间保持，新服务器时重置。
		                                                                                                                          
*/
		conn->pversion = PG_PROTOCOL(3, 0);
		conn->send_appname = true;
#ifdef USE_SSL
		/* 根据 SSL 模式初始化这些值       */
		conn->allow_ssl_try = (conn->sslmode[0] != 'd');	/* "disable" */
		conn->wait_ssl_try = (conn->sslmode[0] == 'a'); /* "allow" */
#endif
#ifdef ENABLE_GSS
		conn->try_gss = (conn->gssencmode[0] != 'd');	/* "disable" */
#endif

		reset_connection_state_machine = false;
		need_new_connection = true;
	}

	/* 强制新连接（或连同服务器）？                     */
	if (need_new_connection)
	{
		/* 丢弃任何已有的连接  */
		pqDropConnection(conn, true);

		/* 重置从旧服务器所得的状态     */
		pqDropServerData(conn);

		/* 也丢弃可能持有的 PGresult    */
		conn->asyncStatus = PGASYNC_IDLE;
		conn->xactStatus = PQTRANS_IDLE;
		conn->pipelineStatus = PQ_PIPELINE_OFF;
		pqClearAsyncResult(conn);

		/* 重置 status 到正确状态                                  */
		conn->status = CONNECTION_NEEDED;

		need_new_connection = false;
	}

	/* 试推进此连接状态机                              */
	switch (conn->status)
	{
		case CONNECTION_NEEDED:
			{
				/*
				 * 试发起连接到返回地址之一。
				 * whichaddr 是下一尝试。
				 *
				 * 额外大括号历史遗留。
				                                                                             
                                                                            
*/
				{
					char		host_addr[NI_MAXHOST];
					int			sock_type;
					AddrInfo   *addr_cur;

					/*
					 * 若当前主机地址已试完，
					 * 推进下一主机。
					                                  */
					if (conn->whichaddr == conn->naddr)
					{
						conn->try_next_host = true;
						goto keep_going;
					}
					addr_cur = &conn->addr[conn->whichaddr];

					/* 记当前地址备后用                        */
					memcpy(&conn->raddr, &addr_cur->addr, sizeof(SockAddr));

					/*
					 * 也设 connip。有意忽略
					 * strdup 失败。
					                                             */
					if (conn->connip != NULL)
					{
						free(conn->connip);
						conn->connip = NULL;
					}
					getHostaddr(conn, host_addr, NI_MAXHOST);
					if (host_addr[0])
						conn->connip = strdup(host_addr);

					/* 尝试创建套接字    */
					sock_type = SOCK_STREAM;
#ifdef SOCK_CLOEXEC

					/*
					 * 尽可能原子设 close-on-exec，
					 * 免子程序继承套接字。
					                                                                  
                                                                 
*/
					sock_type |= SOCK_CLOEXEC;
#endif
#ifdef SOCK_NONBLOCK

					/*
					 * 可顺便跳非阻塞系统调用。
					                                            
*/
					sock_type |= SOCK_NONBLOCK;
#endif
					conn->sock = socket(addr_cur->family, sock_type, 0);
					if (conn->sock == PGINVALID_SOCKET)
					{
						int			errorno = SOCK_ERRNO;

						/*
						 * 有更多地址则静默忽略
						 * socket() 失败；减无用
						 * 日志（IPv4+IPv6 混合）。
						                                                                                                              
*/
						if (conn->whichaddr < conn->naddr ||
							conn->whichhost + 1 < conn->nconnhost)
						{
							conn->try_next_addr = true;
							goto keep_going;
						}
						emitHostIdentityInfo(conn, host_addr);
						libpq_append_conn_error(conn, "could not create socket: %s",
												SOCK_STRERROR(errorno, sebuf, sizeof(sebuf)));
						goto error_return;
					}

					/*
					 * 识别目标地址后除 socket()
					 * 失败外均加主机身份前缀。
					 *（连成则 errorMessage 内容
					 * 不重要，故无害。）
					                                                                                                                           
*/
					emitHostIdentityInfo(conn, host_addr);

					/*
					 * 设套接字选项：不延迟、
					 * 非阻塞、close-on-exec。
					 * 任一败试下址。
					                                                        */
					if (addr_cur->family != AF_UNIX)
					{
						if (!connectNoDelay(conn))
						{
							/* 错误消息已经生成      */
							conn->try_next_addr = true;
							goto keep_going;
						}
					}
#ifndef SOCK_NONBLOCK
					if (!pg_set_noblock(conn->sock))
					{
						libpq_append_conn_error(conn, "could not set socket to nonblocking mode: %s",
												SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
						conn->try_next_addr = true;
						goto keep_going;
					}
#endif

#ifndef SOCK_CLOEXEC
#ifdef F_SETFD
					if (fcntl(conn->sock, F_SETFD, FD_CLOEXEC) == -1)
					{
						libpq_append_conn_error(conn, "could not set socket to close-on-exec mode: %s",
												SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
						conn->try_next_addr = true;
						goto keep_going;
					}
#endif							/* F_SETFD */
#endif

					if (addr_cur->family != AF_UNIX)
					{
#ifndef WIN32
						int			on = 1;
#endif
						int			usekeepalives = useKeepalives(conn);
						int			err = 0;

						if (usekeepalives < 0)
						{
							/* 错误已被上报        */
							err = 1;
						}
						else if (usekeepalives == 0)
						{
							/* 无操作  */
						}
#ifndef WIN32
						else if (setsockopt(conn->sock,
											SOL_SOCKET, SO_KEEPALIVE,
											(char *) &on, sizeof(on)) < 0)
						{
							libpq_append_conn_error(conn, "%s(%s) failed: %s",
													"setsockopt",
													"SO_KEEPALIVE",
													SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
							err = 1;
						}
						else if (!setKeepalivesIdle(conn)
								 || !setKeepalivesInterval(conn)
								 || !setKeepalivesCount(conn))
							err = 1;
#else							/* WIN32 */
#ifdef SIO_KEEPALIVE_VALS
						else if (!prepKeepalivesWin32(conn))
							err = 1;
#endif							/* SIO_KEEPALIVE_VALS */
#endif							/* WIN32 */
						else if (!setTCPUserTimeout(conn))
							err = 1;

						if (err)
						{
							conn->try_next_addr = true;
							goto keep_going;
						}
					}

					/*----------
					 * 三种方法在 send() 时阻塞
					 * SIGPIPE：
					 *
					 *	-setsockopt(SO_NOSIGPIPE)
					 *	-send(...,MSG_NOSIGNAL)
					 *	-信号掩码设 SIG_IGN
					 *
					 * 第三法每次 send 三 syscall，
					 * 故优先前两。状态存 PGconn
					 * 成员：
					 *
					 * sigpipe_so   -已设 SO_NOSIGPIPE
					 * sigpipe_flag  -正指定 MSG_NOSIGNAL
					 *
					 * 可用 SO_NOSIGPIPE 则设
					 * sigpipe_so 即可。否则设
					 * sigpipe_flag 试 MSG_NOSIGNAL。
					 * 出错则清标志回退信号屏蔽。
					 *----------
					                                                                                                                                                             
                                                                                                                                                            
*/
					conn->sigpipe_so = false;
#ifdef MSG_NOSIGNAL
					conn->sigpipe_flag = true;
#else
					conn->sigpipe_flag = false;
#endif							/* MSG_NOSIGNAL */

#ifdef SO_NOSIGPIPE
					optval = 1;
					if (setsockopt(conn->sock, SOL_SOCKET, SO_NOSIGPIPE,
								   (char *) &optval, sizeof(optval)) == 0)
					{
						conn->sigpipe_so = true;
						conn->sigpipe_flag = false;
					}
#endif							/* SO_NOSIGPIPE */

					/*
					 * 发起连接。非阻塞不应阻塞。
					                                                                          
*/
					if (connect(conn->sock, (struct sockaddr *) &addr_cur->addr.addr,
								addr_cur->addr.salen) < 0)
					{
						if (SOCK_ERRNO == EINPROGRESS ||
#ifdef WIN32
							SOCK_ERRNO == EWOULDBLOCK ||
#endif
							SOCK_ERRNO == EINTR)
						{
							/*
							 * 正常-非阻塞连接进行中。
							 * 告知等可写。
							                                                                                  
*/
							conn->status = CONNECTION_STARTED;
							return PGRES_POLLING_WRITING;
						}
						/* 否则会出问题 */
					}
					else
					{
						/*
						 * 已连-非阻塞未非阻塞。
						 * 推进状态机。
						                                                                                   
*/
						conn->status = CONNECTION_STARTED;
						goto keep_going;
					}

					/*
					 * 连接失败。错追加到
					 * errorMessage 试下址。
					                                                  */
					connectFailureMessage(conn, SOCK_ERRNO);
					conn->try_next_addr = true;
					goto keep_going;
				}
			}

		case CONNECTION_STARTED:
			{
				socklen_t	optlen = sizeof(optval);

				/*
				 * 可写，故连接已建。
				                                                                     
*/

				/*
				 * getsockopt 检查待处理错误。
				                                                               
*/

				if (getsockopt(conn->sock, SOL_SOCKET, SO_ERROR,
							   (char *) &optval, &optlen) == -1)
				{
					libpq_append_conn_error(conn, "could not get socket error status: %s",
											SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					goto error_return;
				}
				else if (optval != 0)
				{
					/*
					 * 非阻塞 connect 通常通过
					 * getsockopt 见失败。
					                                                                              
*/
					connectFailureMessage(conn, optval);

					/*
					 * 试下址（若有），
					 * 同 connect() 返回错时。
					                                         */
					conn->try_next_addr = true;
					goto keep_going;
				}

				/* 填入客户端地址      */
				conn->laddr.salen = sizeof(conn->laddr.addr);
				if (getsockname(conn->sock,
								(struct sockaddr *) &conn->laddr.addr,
								&conn->laddr.salen) < 0)
				{
					libpq_append_conn_error(conn, "could not get client address from socket: %s",
											SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					goto error_return;
				}

				/*
					 * 确保可写再进下一步。
					                      */
				conn->status = CONNECTION_MADE;
				return PGRES_POLLING_WRITING;
			}

		case CONNECTION_MADE:
			{
				char	   *startpacket;
				int			packetlen;

				/*
					 * 若 requirepeer 且 Unix
					 * 域套接字则执行检查。
					               */
				if (conn->requirepeer && conn->requirepeer[0] &&
					conn->raddr.addr.ss_family == AF_UNIX)
				{
#ifndef WIN32
					char	   *remote_username;
#endif
					uid_t		uid;
					gid_t		gid;

					errno = 0;
					if (getpeereid(conn->sock, &uid, &gid) != 0)
					{
						/*
						 * getpeereid 为 stub 时
						 * 提供特殊错消息
						         */
						if (errno == ENOSYS)
							libpq_append_conn_error(conn, "requirepeer parameter is not supported on this platform");
						else
							libpq_append_conn_error(conn, "could not get peer credentials: %s",
													strerror_r(errno, sebuf, sizeof(sebuf)));
						goto error_return;
					}

#ifndef WIN32
					remote_username = pg_fe_getusername(uid,
														&conn->errorMessage);
					if (remote_username == NULL)
						goto error_return;	/* 消息已录           */

					if (strcmp(remote_username, conn->requirepeer) != 0)
					{
						libpq_append_conn_error(conn, "requirepeer specifies \"%s\", but actual peer user name is \"%s\"",
												conn->requirepeer, remote_username);
						free(remote_username);
						goto error_return;
					}
					free(remote_username);
#else							/* WIN32 */
					/* 上应已 ENOSYS 失败              */
					Assert(false);
#endif							/* WIN32 */
				}

				if (conn->raddr.addr.ss_family == AF_UNIX)
				{
					/* Unix 套接字不请求 SSL/GSSAPI            */
#ifdef USE_SSL
					conn->allow_ssl_try = false;
#endif
#ifdef ENABLE_GSS
					conn->try_gss = false;
#endif
				}

#ifdef ENABLE_GSS

				/*
					 * GSSAPI 加密启用则调
					 * pg_GSS_have_cred_cache()
					 * 若能获凭据返回 true
					 *（给 conn->gcred 句柄），
					 * 然后发 GSSAPI 加密请求
					 *（跳过 SSL 协商和常规启动）
					                                                                                                             */
				if (conn->try_gss && !conn->gctx)
					conn->try_gss = pg_GSS_have_cred_cache(&conn->gcred);
				if (conn->try_gss && !conn->gctx)
				{
					ProtocolVersion pv = pg_hton32(NEGOTIATE_GSS_CODE);

					if (pqPacketSend(conn, 0, &pv, sizeof(pv)) != STATUS_OK)
					{
						libpq_append_conn_error(conn, "could not send GSSAPI negotiation packet: %s",
												SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
						goto error_return;
					}

					/* 好等响应          */
					conn->status = CONNECTION_GSS_STARTUP;
					return PGRES_POLLING_READING;
				}
				else if (!conn->gctx && conn->gssencmode[0] == 'r')
				{
					libpq_append_conn_error(conn,
											"GSSAPI encryption required but was impossible (possibly no credential cache, no server support, or using a local socket)");
					goto error_return;
				}
#endif

#ifdef USE_SSL

				/*
					 * 检查 SSL 前启 libcrypto 回调。
					 * 发启动包前做因某些认证
					 *（如 MD5/SCRAM）即使无 SSL
					 * 也需这些回调
					                                                                                                                                                       
*/
				if (pqsecure_initialize(conn, false, true) < 0)
					goto error_return;

				/*
					 * SSL 启且尚无加密则在运行，
					 * 请求 SSL 而非发启动消息。
					                                                            
*/
				if (conn->allow_ssl_try && !conn->wait_ssl_try &&
					!conn->ssl_in_use
#ifdef ENABLE_GSS
					&& !conn->gssenc
#endif
					)
				{
					ProtocolVersion pv;

					/*
						 * 发 SSL 请求包。
						 *
						 * 理论可阻塞但不应，
						 * 因仅可写时才到此处。
						                                                              
*/
					pv = pg_hton32(NEGOTIATE_SSL_CODE);
					if (pqPacketSend(conn, 0, &pv, sizeof(pv)) != STATUS_OK)
					{
						libpq_append_conn_error(conn, "could not send SSL negotiation packet: %s",
												SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
						goto error_return;
					}
					/* 好等响应          */
					conn->status = CONNECTION_SSL_STARTUP;
					return PGRES_POLLING_READING;
				}
#endif							/* USE_SSL */

				/*
					 * 构建启动包。
					      */
				startpacket = pqBuildStartupPacket3(conn, &packetlen,
													EnvironmentOptions);
				if (!startpacket)
				{
					libpq_append_conn_error(conn, "out of memory");
					goto error_return;
				}

				/*
					 * 发启动包。
					 *
					 * 理论可阻塞但不该。
					                                                                                              
*/
				if (pqPacketSend(conn, 0, startpacket, packetlen) != STATUS_OK)
				{
					libpq_append_conn_error(conn, "could not send startup packet: %s",
											SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
					free(startpacket);
					goto error_return;
				}

				free(startpacket);

				conn->status = CONNECTION_AWAITING_RESPONSE;
				return PGRES_POLLING_READING;
			}

			/*
				 * SSL 协商：等 postmaster
				 * 消息并按需响应。
				                        */
		case CONNECTION_SSL_STARTUP:
			{
#ifdef USE_SSL
				PostgresPollingStatusType pollres;

				/*
					 * 首次获 postmaster 对
					 * SSL 协商包的响应。
					                               */
				if (!conn->ssl_in_use)
				{
					/*
						 * 用 pqReadData 因有区分
						 * 无数据和断连逻辑。
						                                                                                                     
*/
					char		SSLok;
					int			rdresult;

					rdresult = pqReadData(conn);
					if (rdresult < 0)
					{
						/* errorMessage 已填               */
						goto error_return;
					}
					if (rdresult == 0)
					{
						/* 调用者未能等待数据    */
						return PGRES_POLLING_READING;
					}
					if (pqGetc(&SSLok, conn) < 0)
					{
						/* 实际上不应该发生 */
						return PGRES_POLLING_READING;
					}
					if (SSLok == 'S')
					{
						/* 该字节已消费 */
						conn->inStart = conn->inCursor;

						/*
						 * 需时设全局 SSL 状态。
						 * libpq 已处理则无需。
						                                                           
                                                         
*/
						if (pqsecure_initialize(conn, true, false) != 0)
							goto error_return;
					}
					else if (SSLok == 'N')
					{
						/* 该字节已消费 */
						conn->inStart = conn->inCursor;
						/* 可不 SSL?           */
						if (conn->sslmode[0] == 'r' ||	/* "require" */
							conn->sslmode[0] == 'v')	/* "verify-ca"或
												 * "verify-full"   */
						{
							/* 要求 SSL，但服务器不想要它    */
							libpq_append_conn_error(conn, "server does not support SSL, but SSL was required");
							goto error_return;
						}
						/* 否则继续正常的启动过程      */
						conn->allow_ssl_try = false;
						/* 我们可以使用此连接继续    */
						conn->status = CONNECTION_MADE;
						return PGRES_POLLING_WRITING;
					}
					else if (SSLok == 'E')
					{
						/*
						 * 服务器故障如无法 fork
						 * 后端。不取错误消息，
						 * 因服务器未认证。
						                                                                                                            
*/
						libpq_append_conn_error(conn, "server sent an error response during SSL exchange");
						goto error_return;
					}
					else
					{
						libpq_append_conn_error(conn, "received invalid response to SSL negotiation: %c",
												SSLok);
						goto error_return;
					}
				}

				/*
					 * 开始/继续 SSL 协商。
					                  */
				pollres = pqsecure_open_client(conn);
				if (pollres == PGRES_POLLING_OK)
				{
					/*
						 * 不应有已缓冲数据。若有
						 * 是 SSL 握手前收到，未
						 * 加密，可能是中间人注入。
						                                                                                                        
*/
					if (conn->inCursor != conn->inEnd)
					{
						libpq_append_conn_error(conn, "received unencrypted data after SSL response");
						goto error_return;
					}

					/* SSL 握手完成，已可发送启动包         */
					conn->status = CONNECTION_MADE;
					return PGRES_POLLING_WRITING;
				}
				if (pollres == PGRES_POLLING_FAILED)
				{
					/*
						 * 失败... sslmode prefer
						 * 则重试非 SSL
						              */
					if (conn->sslmode[0] == 'p' /* "prefer" */
						&& conn->allow_ssl_try	/* 冗余？  */
						&& !conn->wait_ssl_try) /* 冗余？  */
					{
						/* 仅重试一次 */
						conn->allow_ssl_try = false;
						need_new_connection = true;
						goto keep_going;
					}
					/* 否则硬失败          */
					goto error_return;
				}
				/* 否则，返回 POLLING_READING 或 POLLING_WRITING    */
				return pollres;
#else							/* !USE_SSL */
				/* 不到此      */
				goto error_return;
#endif							/* USE_SSL */
			}

		case CONNECTION_GSS_STARTUP:
			{
#ifdef ENABLE_GSS
				PostgresPollingStatusType pollres;

				/*
					 * 若尚未，获 postmaster
					 * 对协商包的响应
					                        */
				if (conn->try_gss && !conn->gctx)
				{
					char		gss_ok;
					int			rdresult = pqReadData(conn);

					if (rdresult < 0)
						/* pqReadData 会填充错误消息  */
						goto error_return;
					else if (rdresult == 0)
						/* 调用者未能等待数据    */
						return PGRES_POLLING_READING;
					if (pqGetc(&gss_ok, conn) < 0)
						/* 不应发生……  */
						return PGRES_POLLING_READING;

					if (gss_ok == 'E')
					{
						/*
						 * 服务器故障，假定协议
						 * 版本不支持。不取错误
						 * 因服务器未认证。
						                                                                                
                                                                               
*/
						conn->try_gss = false;
						need_new_connection = true;
						goto keep_going;
					}

					/* 该字节已消费 */
					conn->inStart = conn->inCursor;

					if (gss_ok == 'N')
					{
						/* 服务器不要 GSSAPI；可回退则回退     */
						if (conn->gssencmode[0] == 'r')
						{
							libpq_append_conn_error(conn, "server doesn't support GSSAPI encryption, but it was required");
							goto error_return;
						}

						conn->try_gss = false;
						/* 我们可以使用此连接继续    */
						conn->status = CONNECTION_MADE;
						return PGRES_POLLING_WRITING;
					}
					else if (gss_ok != 'G')
					{
						libpq_append_conn_error(conn, "received invalid response to GSSAPI negotiation: %c",
												gss_ok);
						goto error_return;
					}
				}

				/* 开始或继续 GSSAPI 协商        */
				pollres = pqsecure_open_gss(conn);
				if (pollres == PGRES_POLLING_OK)
				{
					/*
						 * 不应有已缓冲数据。若有
						 * 是握手前收到未加密。
						                                                                           
                                                                         
*/
					if (conn->inCursor != conn->inEnd)
					{
						libpq_append_conn_error(conn, "received unencrypted data after GSSAPI encryption response");
						goto error_return;
					}

					/* 已可发送启动包      */
					conn->status = CONNECTION_MADE;
					return PGRES_POLLING_WRITING;
				}
				else if (pollres == PGRES_POLLING_FAILED)
				{
					if (conn->gssencmode[0] == 'p')
					{
						/*
						 * 失败但 prefer 可重试。
						 * 需弃当前连接再试。
						                                       */
						conn->try_gss = false;
						need_new_connection = true;
						goto keep_going;
					}
					/* 否则硬失败          */
					goto error_return;
				}
				/* 否则，返回 POLLING_READING 或 POLLING_WRITING    */
				return pollres;
#else							/* !ENABLE_GSS */
				/* 不可达   */
				goto error_return;
#endif							/* ENABLE_GSS */
			}

			/*
			 * 认证交换：等 postmaster
			 * 消息并按需响应。
			                                 */
		case CONNECTION_AWAITING_RESPONSE:
			{
				char		beresp;
				int			msgLength;
				int			avail;
				AuthRequest areq;
				int			res;

				/*
				 * 从当前位置扫描消息（若已
				 * 在缓冲则直接找到）。
				                                                                                              
*/
				conn->inCursor = conn->inStart;

				/* 读取类型   */
				if (pqGetc(&beresp, conn))
				{
					/* 等到有更多数据时我们再回来 */
					return PGRES_POLLING_READING;
				}

				/*
				 * 验证消息类型：仅期认证
				 * 请求/NegotiateProtocolVersion/
				 * ErrorResponse/ReadyForQuery。
				                                                                                          
*/
				if (!(beresp == 'R' || beresp == 'v' || beresp == 'E'))
				{
					libpq_append_conn_error(conn, "expected authentication request from server, but received %c",
											beresp);
					goto error_return;
				}

				/* 读取消息长度字    */
				if (pqGetInt(&msgLength, 4, conn))
				{
					/* 等到有更多数据时我们再回来 */
					return PGRES_POLLING_READING;
				}

				/*
				 * 先验消息长度。
				 *
				 * 认证请求不能很大，虽无
				 * 正式限制。SASL 或 GSS
				 * 认证的长度可能来自库，
				 * 可达 500MB。不直接分配
				 * 大量内存——不能太信
				 * 服务器正确性。
				                                                                                              
                                                                                            
                                                                                            
*/
				if (beresp == 'R' && (msgLength < 8 || msgLength > 2000))
				{
					libpq_append_conn_error(conn, "received invalid authentication request");
					goto error_return;
				}
				if (beresp == 'v' && (msgLength < 8 || msgLength > 2000))
				{
					libpq_append_conn_error(conn, "received invalid protocol negotiation message");
					goto error_return;
				}

#define MAX_ERRLEN 30000
				if (beresp == 'E' && (msgLength < 8 || msgLength > MAX_ERRLEN))
				{
					/* 处理 3.0 前服务器的错误   */
					conn->inCursor = conn->inStart + 1; /* 重读      */
					if (pqGets_append(&conn->errorMessage, conn))
					{
						/*
						 * 我们可能尚未认证服务器，因此      
						 * 不要让缓冲区无限增长。 
						 */
						avail = conn->inEnd - conn->inCursor;
						if (avail > MAX_ERRLEN)
						{
							libpq_append_conn_error(conn, "received invalid error message");
							goto error_return;
						}

						/* 等到有更多数据时我们再回来 */
						return PGRES_POLLING_READING;
					}
					/* 已读完该消息；标记数据已消费  */
					conn->inStart = conn->inCursor;

					/*
					 * 7.2 前 postmaster 不总以
					 * 换行终消息。若发现缺
					 * 换行则补。
					                                                     */
					if (conn->errorMessage.len == 0 ||
						conn->errorMessage.data[conn->errorMessage.len - 1] != '\n')
					{
						appendPQExpBufferChar(&conn->errorMessage, '\n');
					}

					goto error_return;
				}
#undef MAX_ERRLEN

				/*
				 * 消息体不全暂不处理。
				 *
				 * 此检查后消息与流分离，
				 * 无需再查过多过少。
				                                                                                           
                                                                                         
*/
				msgLength -= 4;
				avail = conn->inEnd - conn->inCursor;
				if (avail < msgLength)
				{
					/*
					 * 返回前试扩大输入缓冲
					 * 以容整条消息。
					                                                                      
*/
					if (pqCheckInBufferSpace(conn->inCursor + (size_t) msgLength,
											 conn))
						goto error_return;
					/* 等到有更多数据时我们再回来 */
					return PGRES_POLLING_READING;
				}

				/* 处理错误   */
				if (beresp == 'E')
				{
					if (pqGetErrorNotice3(conn, true))
					{
						libpq_append_conn_error(conn, "received invalid error message");
						goto error_return;
					}
					/* 已读完该消息；标记数据已消费  */
					conn->inStart = conn->inCursor;

					/*
					 * "cannot connect now" 则
					 * 试下一主机。
					                                                                                    
                                                                                   
                                                                                  
*/
					if (strcmp(conn->last_sqlstate,
							   ERRCODE_CANNOT_CONNECT_NOW) == 0)
					{
						conn->try_next_host = true;
						goto keep_going;
					}

					/* 检查是否应当提及 pgpassfile          */
					pgpassfileWarning(conn);

#ifdef ENABLE_GSS

					/*
					 * gssencmode prefer 且
					 * 用 GSSAPI，重试不用。
					                  */
					if (conn->gssenc && conn->gssencmode[0] == 'p')
					{
						/* 仅重试一次 */
						conn->try_gss = false;
						need_new_connection = true;
						goto keep_going;
					}
#endif

#ifdef USE_SSL

					/*
					 * sslmode allow 且未试
					 * SSL，则重试 SSL。
					                                                          */
					if (conn->sslmode[0] == 'a' /* "allow" */
						&& !conn->ssl_in_use
						&& conn->allow_ssl_try
						&& conn->wait_ssl_try)
					{
						/* 仅重试一次 */
						conn->wait_ssl_try = false;
						need_new_connection = true;
						goto keep_going;
					}

					/*
					 * sslmode prefer 且 SSL
					 * 连接，则重试非 SSL
					                               */
					if (conn->sslmode[0] == 'p' /* "prefer" */
						&& conn->ssl_in_use
						&& conn->allow_ssl_try	/* 冗余？  */
						&& !conn->wait_ssl_try) /* 冗余？  */
					{
						/* 仅重试一次 */
						conn->allow_ssl_try = false;
						need_new_connection = true;
						goto keep_going;
					}
#endif

					goto error_return;
				}
				else if (beresp == 'v')
				{
					if (pqGetNegotiateProtocolVersion3(conn))
					{
						libpq_append_conn_error(conn, "received invalid protocol negotiation message");
						goto error_return;
					}
					/* 已读完该消息；标记数据已消费  */
					conn->inStart = conn->inCursor;
					goto error_return;
				}

				/* 这是一条认证请求。      */
				conn->auth_req_received = true;

				/* 获取请求的类型。 */
				if (pqGetInt((int *) &areq, 4, conn))
				{
					/* 不会发生因已查长                           */
					libpq_append_conn_error(conn, "received invalid authentication request");
					goto error_return;
				}
				msgLength -= 4;

				/*
				 * 处理认证请求剩余部分并
				 * 响应（如需）。
				                                                      
                                                     
                                                     
*/
				res = pg_fe_sendauth(areq, msgLength, conn);

				/* OK 已处理；标记消费                           */
				conn->inStart = conn->inCursor;

				if (res != STATUS_OK)
					goto error_return;

				/*
				 * 确保 pg_fe_sendauth 数据
				 * 已刷新。虽应非阻塞，
				 * 无妨检查。
				                                                                                                 */
				if (pqFlush(conn))
					goto error_return;

				if (areq == AUTH_REQ_OK)
				{
					/* 认证交换已全部完成              */
					conn->status = CONNECTION_AUTH_OK;

					/*
					 * 设 asyncStatus 使
					 * PQgetResult 认为后续
					 * 来自服务器。
					                                                */
					conn->asyncStatus = PGASYNC_BUSY;
				}

				/* 看看是否还有更多数据。     */
				goto keep_going;
			}

		case CONNECTION_AUTH_OK:
			{
				/*
				 * 期从后端听到消息。
				 * ReadyForQuery 示启动成功，
				 * 也可收到失败错误消息。
				 *（PGRES_POLLING_FAILED 仅
				 * libpq 侧失败用。）
				                                                                                          
                                                                                         
                                                                                         
                                                                                         
*/

				if (PQisBusy(conn))
					return PGRES_POLLING_READING;

				res = PQgetResult(conn);

				/*
				 * NULL 返表示已到 IDLE，
				 * 符合预期
				                     */
				if (res)
				{
					if (res->resultStatus != PGRES_FATAL_ERROR)
						libpq_append_conn_error(conn, "unexpected message from server during startup");
					else if (conn->send_appname &&
							 (conn->appname || conn->fbappname))
					{
						/*
						 * 若试发 application_name，
						 * 检查错是否关于不识
						 * 应用名（旧服务器）忽略。
						                                                                   
                                                                  
                                                                 
                                                                 
                                                                 
*/
						const char *sqlstate;

						sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
						if (sqlstate &&
							strcmp(sqlstate, ERRCODE_APPNAME_UNKNOWN) == 0)
						{
							PQclear(res);
							conn->send_appname = false;
							need_new_connection = true;
							goto keep_going;
						}
					}

					/*
					 * resultStatus FATAL 则
					 * errorMessage 已有内容。
					                                                                                    
                                                                                   
*/
					if (conn->errorMessage.len <= 0 ||
						conn->errorMessage.data[conn->errorMessage.len - 1] != '\n')
						appendPQExpBufferChar(&conn->errorMessage, '\n');
					PQclear(res);
					goto error_return;
				}

				/* 快完成了……   */
				conn->status = CONNECTION_CHECK_TARGET;
				goto keep_going;
			}

		case CONNECTION_CHECK_TARGET:
			{
				/*
				 * 若需读写/只读/主库/备库
				 * 连接则检查服务器状态。
				                         */
				if (conn->target_server_type == SERVER_TYPE_READ_WRITE ||
					conn->target_server_type == SERVER_TYPE_READ_ONLY)
				{
					bool		read_only_server;

					/*
					 * 若服务器未在启动时报告
					 * "default_transaction_read_only"
					 * 或 "in_hot_standby"，则
					 * 用 SHOW/SELECT 确定。
					                                                                                                                               
*/
					if (conn->default_transaction_read_only == PG_BOOL_UNKNOWN ||
						conn->in_hot_standby == PG_BOOL_UNKNOWN)
					{
						/*
						 * 我们使用 PQsendQueryContinue  
						 * 使 conn->errorMessage 不被清空。我们要  
						 * 保留与之前尝试连接失败的那些主机  
						 * 相关的错误消息。                     
						 */
						conn->status = CONNECTION_OK;
						if (!PQsendQueryContinue(conn,
												 "SHOW transaction_read_only"))
							goto error_return;
						/* 得答案后回此态                              */
						conn->status = CONNECTION_CHECK_WRITABLE;
						return PGRES_POLLING_READING;
					}

					/* 现在可以进行测试 */
					read_only_server =
						(conn->default_transaction_read_only == PG_BOOL_YES ||
						 conn->in_hot_standby == PG_BOOL_YES);

					if ((conn->target_server_type == SERVER_TYPE_READ_WRITE) ?
						read_only_server : !read_only_server)
					{
						/* 服态错，拒试下主机                      */
						if (conn->target_server_type == SERVER_TYPE_READ_WRITE)
							libpq_append_conn_error(conn, "session is read-only");
						else
							libpq_append_conn_error(conn, "session is not read-only");

						/* 礼貌地关闭连接。   */
						conn->status = CONNECTION_OK;
						sendTerminateConn(conn);

						/*
						 * 试下主机但不考虑
						 * 更多地址。
						                                                */
						conn->try_next_host = true;
						goto keep_going;
					}
				}
				else if (conn->target_server_type == SERVER_TYPE_PRIMARY ||
						 conn->target_server_type == SERVER_TYPE_STANDBY ||
						 conn->target_server_type == SERVER_TYPE_PREFER_STANDBY)
				{
					/*
					 * 若启动时未报告
					 * "in_hot_standby"，则用
					 * SELECT pg_is_in_recovery()
					 * 确定是否备库。
					                                                                                                     
                                                                                                    
*/
					if (conn->sversion < 90000)
						conn->in_hot_standby = PG_BOOL_NO;

					if (conn->in_hot_standby == PG_BOOL_UNKNOWN)
					{
						/*
						 * 我们使用 PQsendQueryContinue  
						 * 使 conn->errorMessage 不被清空。我们要  
						 * 保留与之前尝试连接失败的那些主机  
						 * 相关的错误消息。                     
						 */
						conn->status = CONNECTION_OK;
						if (!PQsendQueryContinue(conn,
												 "SELECT pg_catalog.pg_is_in_recovery()"))
							goto error_return;
						/* 得答案后回此态                              */
						conn->status = CONNECTION_CHECK_STANDBY;
						return PGRES_POLLING_READING;
					}

					/* 现在可以进行测试 */
					if ((conn->target_server_type == SERVER_TYPE_PRIMARY) ?
						(conn->in_hot_standby == PG_BOOL_YES) :
						(conn->in_hot_standby == PG_BOOL_NO))
					{
						/* 服态错，拒试下主机                      */
						if (conn->target_server_type == SERVER_TYPE_PRIMARY)
							libpq_append_conn_error(conn, "server is in hot standby mode");
						else
							libpq_append_conn_error(conn, "server is not in hot standby mode");

						/* 礼貌地关闭连接。   */
						conn->status = CONNECTION_OK;
						sendTerminateConn(conn);

						/*
						 * 试下主机但不考虑
						 * 更多地址。
						                                                */
						conn->try_next_host = true;
						goto keep_going;
					}
				}

				/* 现在可以释放地址列表了。 */
				release_conn_addrinfo(conn);

				/*
				 * errorMessage 内容不再
				 * 有用（可清以免混淆）。
				                                                                                     
*/
				pqClearConnErrorState(conn);

				/* 可以开始工作了！  */
				conn->status = CONNECTION_OK;
				return PGRES_POLLING_OK;
			}

		case CONNECTION_CONSUME:
			{
				/*
				 * 确获得结果后连接空闲。
				 * 收到 NULL 后推进。
				 *（还有数据不宜推进：
				 * 可能无限循环。）
				                                                                                                 
                                                                                               
*/
				conn->status = CONNECTION_OK;
				if (!PQconsumeInput(conn))
					goto error_return;

				if (PQisBusy(conn))
				{
					conn->status = CONNECTION_CONSUME;
					return PGRES_POLLING_READING;
				}

				/* 反复 PQgetResult() 至 NULL                       */
				res = PQgetResult(conn);
				if (res != NULL)
				{
					PQclear(res);
					conn->status = CONNECTION_CONSUME;
					return PGRES_POLLING_READING;
				}

				conn->status = CONNECTION_CHECK_TARGET;
				goto keep_going;
			}

		case CONNECTION_CHECK_WRITABLE:
			{
				/*
				 * 等 SHOW transaction_read_only
				 * 结果。临时设 asyncStatus
				 * 使 PQgetResult 信来自命令。
				                                                      */
				conn->status = CONNECTION_OK;
				if (!PQconsumeInput(conn))
					goto error_return;

				if (PQisBusy(conn))
				{
					conn->status = CONNECTION_CHECK_WRITABLE;
					return PGRES_POLLING_READING;
				}

				res = PQgetResult(conn);
				if (res && PQresultStatus(res) == PGRES_TUPLES_OK &&
					PQntuples(res) == 1)
				{
					char	   *val = PQgetvalue(res, 0, 0);

					/*
					 * "on" 证明至少
					 * default_transaction_read_only
					 * 或 in_hot_standby 为 on。
					 * 无法区分但 prefer-standby
					 * 知只读即够。
					                                                                                                                                    
                                                                                                                                  
*/
					if (strncmp(val, "on", 2) == 0)
					{
						conn->default_transaction_read_only = PG_BOOL_YES;
						conn->in_hot_standby = PG_BOOL_YES;
					}
					else
					{
						conn->default_transaction_read_only = PG_BOOL_NO;
						conn->in_hot_standby = PG_BOOL_NO;
					}
					PQclear(res);

					/* 继续之前先读完所有消息         */
					conn->status = CONNECTION_CONSUME;
					goto keep_going;
				}

				/* SHOW transaction_read_only 出错                       */
				PQclear(res);

				/* 错追加 errorMessage。                  */
				libpq_append_conn_error(conn, "\"%s\" failed",
										"SHOW transaction_read_only");

				/* 礼貌地关闭连接。   */
				conn->status = CONNECTION_OK;
				sendTerminateConn(conn);

				/* 下一主机   */
				conn->try_next_host = true;
				goto keep_going;
			}

		case CONNECTION_CHECK_STANDBY:
			{
				/*
				 * 等 SELECT pg_is_in_recovery()
				 * 结果。临时设 asyncStatus
				 * 使 PQgetResult 信来自命令。
				                                                      */
				conn->status = CONNECTION_OK;
				if (!PQconsumeInput(conn))
					goto error_return;

				if (PQisBusy(conn))
				{
					conn->status = CONNECTION_CHECK_STANDBY;
					return PGRES_POLLING_READING;
				}

				res = PQgetResult(conn);
				if (res && PQresultStatus(res) == PGRES_TUPLES_OK &&
					PQntuples(res) == 1)
				{
					char	   *val = PQgetvalue(res, 0, 0);

					if (strncmp(val, "t", 1) == 0)
						conn->in_hot_standby = PG_BOOL_YES;
					else
						conn->in_hot_standby = PG_BOOL_NO;
					PQclear(res);

					/* 继续之前先读完所有消息         */
					conn->status = CONNECTION_CONSUME;
					goto keep_going;
				}

				/* SELECT pg_is_in_recovery 出错                         */
				PQclear(res);

				/* 错追加 errorMessage。                  */
				libpq_append_conn_error(conn, "\"%s\" failed",
										"SELECT pg_is_in_recovery()");

				/* 礼貌地关闭连接。   */
				conn->status = CONNECTION_OK;
				sendTerminateConn(conn);

				/* 下一主机   */
				conn->try_next_host = true;
				goto keep_going;
			}

		default:
			libpq_append_conn_error(conn,
									"invalid connection state %d, probably indicative of memory corruption",
									conn->status);
			goto error_return;
	}

	/* 不可达   */

error_return:

	/*
	 * 曾在此关套接字，但使 PQreset
	 * 尴尬。现改设阻塞模式（自行
	 * 管理）并离循环。非连态不设
	 * 阻塞。
	                                                                                                                                                                                                   
*/
	conn->status = CONNECTION_BAD;
	return PGRES_POLLING_FAILED;
}


/*
 * internal_ping
 *		确定服务器运行且可连接。
 *
 * 成功连接且状态 OK 返 PQPING_OK。
                                                     */
static PGPing
internal_ping(PGconn *conn)
{
	/* 未到 PQconnectPoll 则 "未尝试"              */
	if (!conn || !conn->options_valid)
		return PQPING_NO_ATTEMPT;

	/* 尝试完成连接                 */
	if (conn->status != CONNECTION_BAD)
		(void) connectDBComplete(conn);

	/* 成功则肯定 OK            */
	if (conn->status != CONNECTION_BAD)
		return PQPING_OK;

	/*
	 * "ping" 有趣部分：以足够粒度确定失败原因以供判断返回值。
	 * 例如，不能仅因密码无效就报告服务器未运行。实际上，任何
	 * 认证请求本身已表明服务器已启动。（需要此检查的原因是
	 * libpq 端可能已在从 postmaster 获取真正的错误之前
	 * 就切断了连接。）
	 *
	 *
	                                                                                                                                  */
	if (conn->auth_req_received)
		return PQPING_OK;

	/*
	 * 无法从 postmaster 获 ERROR
	 * 则报 PQPING_NO_RESPONSE。可
	 * 因 pre-7.4 服务器、防火墙等。
	 * 无响应也可表服务器拒 IP
	 * 等。
	 *
	 * 曾返 PQPING_NO_ATTEMPT 若
	 * 未调 connectDBStart，现不
	 * 区分。标准可改。
	                                                                                                                                                                     
                                                                                                                                                                    
                                                                                                                                                                   
*/
	if (strlen(conn->last_sqlstate) != 5)
		return PQPING_NO_RESPONSE;

	/*
	 * 若服务器声明不接连接则报 PQPING_REJECT。
	               */
	if (strcmp(conn->last_sqlstate, ERRCODE_CANNOT_CONNECT_NOW) == 0)
		return PQPING_REJECT;

	/*
	 * 其他 SQLSTATE 可认为服务器
	 * 在运行。或许需破译具体消息
	 * 判断是否可连。但这是
	 * PQPING_OK 保守解释。
	                                                                                                   */
	return PQPING_OK;
}


/*
 * makeEmptyPGconn
 *	 - 创建尚无数据的 PGconn 结构
                              */
static PGconn *
makeEmptyPGconn(void)
{
	PGconn	   *conn;

#ifdef WIN32

	/*
	 * 确保套接字支持已启动。
	 *
	 * 注：Windows 上 WSAStartup()
	 * 须在使用 Winsock 库*前*调。
	 * 尽早在此进程调较安全。
	                                                                                                                           */
	static bool wsastartup_done = false;

	if (!wsastartup_done)
	{
		WSADATA		wsaData;

		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
			return NULL;
		wsastartup_done = true;
	}

	/* 忘记先前错误       */
	WSASetLastError(0);
#endif							/* WIN32 */

	conn = (PGconn *) malloc(sizeof(PGconn));
	if (conn == NULL)
		return conn;

	/* 所有指针和布尔值清零 */
	MemSet(conn, 0, sizeof(PGconn));

	/* 零初始化其余           */
	conn->noticeHooks.noticeRec = defaultNoticeReceiver;
	conn->noticeHooks.noticeProc = defaultNoticeProcessor;

	conn->status = CONNECTION_BAD;
	conn->asyncStatus = PGASYNC_IDLE;
	conn->pipelineStatus = PQ_PIPELINE_OFF;
	conn->xactStatus = PQTRANS_IDLE;
	conn->options_valid = false;
	conn->nonblocking = false;
	conn->client_encoding = PG_SQL_ASCII;
	conn->std_strings = false;	/* 为连接提供默认值       */
	conn->default_transaction_read_only = PG_BOOL_UNKNOWN;
	conn->in_hot_standby = PG_BOOL_UNKNOWN;
	conn->scram_sha_256_iterations = SCRAM_SHA_256_DEFAULT_ITERATIONS;
	conn->verbosity = PQERRORS_DEFAULT;
	conn->show_context = PQSHOW_CONTEXT_ERRORS;
	conn->sock = PGINVALID_SOCKET;
	conn->Pfdebug = NULL;

	/*
	 * 每次至少发 8K（Unix/Windows
	 * 典型管道缓冲大小）。即 8K
	 * 块内核可一次取走，助性能。
	                                                                
                                                               
                                                               
                                                               
                                                              
                                                              
*/
	conn->inBufSize = 16 * 1024;
	conn->inBuffer = (char *) malloc(conn->inBufSize);
	conn->outBufSize = 16 * 1024;
	conn->outBuffer = (char *) malloc(conn->outBufSize);
	conn->rowBufLen = 32;
	conn->rowBuf = (PGdataValue *) malloc(conn->rowBufLen * sizeof(PGdataValue));
	initPQExpBuffer(&conn->errorMessage);
	initPQExpBuffer(&conn->workBuffer);

	if (conn->inBuffer == NULL ||
		conn->outBuffer == NULL ||
		conn->rowBuf == NULL ||
		PQExpBufferBroken(&conn->errorMessage) ||
		PQExpBufferBroken(&conn->workBuffer))
	{
		/* 内存已耗尽 :-(       */
		freePGconn(conn);
		conn = NULL;
	}

	return conn;
}

/*
 * freePGconn
 *	 - 释放空闲（已关）PGconn 结构
 *
 * 注意：不与 closePGconn() 重叠。
 * 清除瞬态属彼；释放 conn 本身
 * 属此。
                                                                                                                                                                                                              
*/
static void
freePGconn(PGconn *conn)
{
	/* 命令队列须先释放                      */
	for (int i = 0; i < conn->nEvents; i++)
	{
		PGEventConnDestroy evt;

		evt.conn = conn;
		(void) conn->events[i].proc(PGEVT_CONNDESTROY, &evt,
									conn->events[i].passThrough);
		free(conn->events[i].name);
	}

	/* freePGconn 不碰 errorMessage   */
	for (int i = 0; i < conn->nconnhost; ++i)
	{
		free(conn->connhost[i].host);
		free(conn->connhost[i].hostaddr);
		free(conn->connhost[i].port);
		if (conn->connhost[i].password != NULL)
		{
			explicit_bzero(conn->connhost[i].password, strlen(conn->connhost[i].password));
			free(conn->connhost[i].password);
		}
	}
	free(conn->connhost);

	free(conn->client_encoding_initial);
	free(conn->events);
	free(conn->pghost);
	free(conn->pghostaddr);
	free(conn->pgport);
	free(conn->connect_timeout);
	free(conn->pgtcp_user_timeout);
	free(conn->pgoptions);
	free(conn->appname);
	free(conn->fbappname);
	free(conn->dbName);
	free(conn->replication);
	free(conn->pguser);
	if (conn->pgpass)
	{
		explicit_bzero(conn->pgpass, strlen(conn->pgpass));
		free(conn->pgpass);
	}
	free(conn->pgpassfile);
	free(conn->channel_binding);
	free(conn->keepalives);
	free(conn->keepalives_idle);
	free(conn->keepalives_interval);
	free(conn->keepalives_count);
	free(conn->sslmode);
	free(conn->sslcert);
	free(conn->sslkey);
	if (conn->sslpassword)
	{
		explicit_bzero(conn->sslpassword, strlen(conn->sslpassword));
		free(conn->sslpassword);
	}
	free(conn->sslcertmode);
	free(conn->sslrootcert);
	free(conn->sslcrl);
	free(conn->sslcrldir);
	free(conn->sslcompression);
	free(conn->sslsni);
	free(conn->requirepeer);
	free(conn->require_auth);
	free(conn->ssl_min_protocol_version);
	free(conn->ssl_max_protocol_version);
	free(conn->gssencmode);
	free(conn->krbsrvname);
	free(conn->gsslib);
	free(conn->gssdelegation);
	free(conn->connip);
	/* 用户负责调后释放 PGconn！                   */
	free(conn->write_err_msg);
	free(conn->inBuffer);
	free(conn->outBuffer);
	free(conn->rowBuf);
	free(conn->target_session_attrs);
	free(conn->load_balance_hosts);
	termPQExpBuffer(&conn->errorMessage);
	termPQExpBuffer(&conn->workBuffer);

	free(conn);
}

/*
 * store_conn_addrinfo
 *	 - 复制 addrinfo 到 PGconn
 *
 * 复制 addrlist 使 libpq 可操作。
 * 正常返正数。出错返 0。
                                                                                                   
*/
static int
store_conn_addrinfo(PGconn *conn, struct addrinfo *addrlist)
{
	struct addrinfo *ai = addrlist;

	conn->whichaddr = 0;

	conn->naddr = 0;
	while (ai)
	{
		ai = ai->ai_next;
		conn->naddr++;
	}

	conn->addr = calloc(conn->naddr, sizeof(AddrInfo));
	if (conn->addr == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return 1;
	}

	ai = addrlist;
	for (int i = 0; i < conn->naddr; i++)
	{
		conn->addr[i].family = ai->ai_family;

		memcpy(&conn->addr[i].addr.addr, ai->ai_addr,
			   ai->ai_addrlen);
		conn->addr[i].addr.salen = ai->ai_addrlen;
		ai = ai->ai_next;
	}

	return 0;
}

/*
 * release_conn_addrinfo
 *	 - 释放 PGconn 中 addrinfo 列表。
  */
static void
release_conn_addrinfo(PGconn *conn)
{
	if (conn->addr)
	{
		free(conn->addr);
		conn->addr = NULL;
	}
}

/*
 * sendTerminateConn
 *	 - 向后端发送终止消息。
       */
static void
sendTerminateConn(PGconn *conn)
{
	/*
	 * 协议不允许认证期间发终止。
	                                                         
*/
	if (conn->sock != PGINVALID_SOCKET && conn->status == CONNECTION_OK)
	{
		/*
		 * 试向后端发关连消息。忽略错。
		                               
*/
		pqPutMsgStart('X', conn);
		pqPutMsgEnd(conn);
		(void) pqFlush(conn);
	}
}

/*
 * closePGconn
 *	 - 正确关连到后端
 *
 * 重置/释放所有瞬态，保留连接
 * 参数。退出时应处于可重连态
 *（若参数仍有效）。
                                                                                                                 */
static void
closePGconn(PGconn *conn)
{
	/*
	 * 如果可能，则发送 Terminate 消息以礼貌地关闭连接。 
	 */
	sendTerminateConn(conn);

	/*
	 * 须重置阻塞态以便可能重连。
	 *
	 * 若建连期间失则 sock 或为
	 * -1 或 PGINVALID_SOCKET。
	                                                         */
	conn->nonblocking = false;

	/*
	 * 关连，重置瞬态，刷 I/O。
	 * 不应在 PQconnectPoll 循环
	 * 中调用（其用 pqDropConnection）。
	                                                                                                                                                          
*/
	pqDropConnection(conn, true);
	conn->status = CONNECTION_BAD;	/* 也重置信道绑定                */
	conn->asyncStatus = PGASYNC_IDLE;
	conn->xactStatus = PQTRANS_IDLE;
	conn->pipelineStatus = PQ_PIPELINE_OFF;
	pqClearAsyncResult(conn);	/* 释放结果      */
	pqClearConnErrorState(conn);
	release_conn_addrinfo(conn);

	/* 也重置从服务器所得的状态      */
	pqDropServerData(conn);
}

/*
 * PQfinish：正确关连并释放 PGconn
 * 结构，此后不应重用。
                                                            */
void
PQfinish(PGconn *conn)
{
	if (conn)
	{
		closePGconn(conn);
		freePGconn(conn);
	}
}

/*
 * PQreset：通过关现连并用相同
 * 参数建新连来重置连接。
                                  */
void
PQreset(PGconn *conn)
{
	if (conn)
	{
		closePGconn(conn);

		if (connectDBStart(conn) && connectDBComplete(conn))
		{
			/* 连已关非错-静默忽略           
         
*/
			int			i;

			for (i = 0; i < conn->nEvents; i++)
			{
				PGEventConnReset evt;

				evt.conn = conn;
				(void) conn->events[i].proc(PGEVT_CONNRESET, &evt,
											conn->events[i].passThrough);
			}
		}
	}
}


/*
 * PQresetStart：
 * 重置连接
 * 关现连并用相同参数建新连。
                                                                        
*/
int
PQresetStart(PGconn *conn)
{
	if (conn)
	{
		closePGconn(conn);

		return connectDBStart(conn);
	}

	return 0;
}


/*
 * PQresetPoll：
 * 重置连接
 * 关现连并用相同参数建新连。
                                  */
PostgresPollingStatusType
PQresetPoll(PGconn *conn)
{
	if (conn)
	{
		PostgresPollingStatusType status = PQconnectPoll(conn);

		if (status == PGRES_POLLING_OK)
		{
			/* 填充 conn 余部                
              
*/
			int			i;

			for (i = 0; i < conn->nEvents; i++)
			{
				PGEventConnReset evt;

				evt.conn = conn;
				(void) conn->events[i].proc(PGEVT_CONNRESET, &evt,
											conn->events[i].passThrough);
			}
		}

		return status;
	}

	return PGRES_POLLING_FAILED;
}

/*
 * PQgetCancel：获取与某连接对应的 PGcancel 结构。        
 *
 * 需要制作一个副本，才能从另一个线程取消正在       
 * 运行的查询。如果使用同一结构，其所有成员就       
 * 必须分别加锁（如果锁定整个结构，那么将不可        
 * 能取消同步查询，因为该结构必须在查询期间           
 * 一直保持锁定。）                           
 */
PGcancel *
PQgetCancel(PGconn *conn)
{
	PGcancel   *cancel;

	if (!conn)
		return NULL;

	if (conn->sock == PGINVALID_SOCKET)
		return NULL;

	cancel = malloc(sizeof(PGcancel));
	if (cancel == NULL)
		return NULL;

	memcpy(&cancel->raddr, &conn->raddr, sizeof(SockAddr));
	cancel->be_pid = conn->be_pid;
	cancel->be_key = conn->be_key;
	/* 见注释                                        */
	cancel->pgtcp_user_timeout = -1;
	cancel->keepalives = -1;
	cancel->keepalives_idle = -1;
	cancel->keepalives_interval = -1;
	cancel->keepalives_count = -1;
	if (conn->pgtcp_user_timeout != NULL)
	{
		if (!parse_int_param(conn->pgtcp_user_timeout,
							 &cancel->pgtcp_user_timeout,
							 conn, "tcp_user_timeout"))
			goto fail;
	}
	if (conn->keepalives != NULL)
	{
		if (!parse_int_param(conn->keepalives,
							 &cancel->keepalives,
							 conn, "keepalives"))
			goto fail;
	}
	if (conn->keepalives_idle != NULL)
	{
		if (!parse_int_param(conn->keepalives_idle,
							 &cancel->keepalives_idle,
							 conn, "keepalives_idle"))
			goto fail;
	}
	if (conn->keepalives_interval != NULL)
	{
		if (!parse_int_param(conn->keepalives_interval,
							 &cancel->keepalives_interval,
							 conn, "keepalives_interval"))
			goto fail;
	}
	if (conn->keepalives_count != NULL)
	{
		if (!parse_int_param(conn->keepalives_count,
							 &cancel->keepalives_count,
							 conn, "keepalives_count"))
			goto fail;
	}

	return cancel;

fail:
	free(cancel);
	return NULL;
}

/* 见注释                             */
void
PQfreeCancel(PGcancel *cancel)
{
	free(cancel);
}


/*
 * 在 TCP 套接字设整数套接字选项，
 * 值非负则设。值 -1 不设。
 * 失败返 false。
                                                                                                              
*/
#if defined(TCP_USER_TIMEOUT) || !defined(WIN32)
static bool
optional_setsockopt(int fd, int protoid, int optid, int value)
{
	if (value < 0)
		return true;
	if (setsockopt(fd, protoid, optid, (char *) &value, sizeof(value)) < 0)
		return false;
	return true;
}
#endif


/*
 * PQcancel：请求查询取消
 *
 * 取消请求成功派发返 true。
 *（若不确知取消已执行返 false，
 * 或无错误。）
 *
 * 成功派发不等同取消已执行！
 * 取消可能未执行因可能仍有
 * 数据传输。后端可能收到
 * 取消但拒执行。
 *
 * 对有超时或需取消隐式事务
 * 块的应用，建议在
 * PQgetResult 循环内调
 * PQcancel 并处理 cancel。
                                                                                                                                                                                                              
                                                                                                                                                                                                             
                                                                                                                                                                                                            
*/
int
PQcancel(PGcancel *cancel, char *errbuf, int errbufsize)
{
	int			save_errno = SOCK_ERRNO;
	pgsocket	tmpsock = PGINVALID_SOCKET;
	int			maxlen;
	struct
	{
		uint32		packetlen;
		CancelRequestPacket cp;
	}			crp;

	if (!cancel)
	{
		strlcpy(errbuf, "PQcancel() -- no cancel object supplied", errbufsize);
		/* strlcpy 大概不会改变 errno，但宁可谨慎     */
		SOCK_ERRNO_SET(save_errno);
		return false;
	}

	/*
	 * 我们需要打开一条到 postmaster 的临时连接。只需       
	 * 用内核调用。
	 */
	if ((tmpsock = socket(cancel->raddr.addr.ss_family, SOCK_STREAM, 0)) == PGINVALID_SOCKET)
	{
		strlcpy(errbuf, "PQcancel() -- socket() failed: ", errbufsize);
		goto cancel_errReturn;
	}

	/*
	 * 此连接仅发单包，故不需
	 * 保活等选项。也不设环境
	 * 选项：发取消时尽量少
	 * 干扰 postmaster。
	                                                                                   
                                                                                  
                                                                                 
                                                                                 
*/
	if (cancel->raddr.addr.ss_family != AF_UNIX &&
		cancel->keepalives != 0)
	{
#ifndef WIN32
		if (!optional_setsockopt(tmpsock, SOL_SOCKET, SO_KEEPALIVE, 1))
		{
			strlcpy(errbuf, "PQcancel() -- setsockopt(SO_KEEPALIVE) failed: ", errbufsize);
			goto cancel_errReturn;
		}

#ifdef PG_TCP_KEEPALIVE_IDLE
		if (!optional_setsockopt(tmpsock, IPPROTO_TCP, PG_TCP_KEEPALIVE_IDLE,
								 cancel->keepalives_idle))
		{
			strlcpy(errbuf, "PQcancel() -- setsockopt(" PG_TCP_KEEPALIVE_IDLE_STR ") failed: ", errbufsize);
			goto cancel_errReturn;
		}
#endif

#ifdef TCP_KEEPINTVL
		if (!optional_setsockopt(tmpsock, IPPROTO_TCP, TCP_KEEPINTVL,
								 cancel->keepalives_interval))
		{
			strlcpy(errbuf, "PQcancel() -- setsockopt(TCP_KEEPINTVL) failed: ", errbufsize);
			goto cancel_errReturn;
		}
#endif

#ifdef TCP_KEEPCNT
		if (!optional_setsockopt(tmpsock, IPPROTO_TCP, TCP_KEEPCNT,
								 cancel->keepalives_count))
		{
			strlcpy(errbuf, "PQcancel() -- setsockopt(TCP_KEEPCNT) failed: ", errbufsize);
			goto cancel_errReturn;
		}
#endif

#else							/* WIN32 */

#ifdef SIO_KEEPALIVE_VALS
		if (!setKeepalivesWin32(tmpsock,
								cancel->keepalives_idle,
								cancel->keepalives_interval))
		{
			strlcpy(errbuf, "PQcancel() -- WSAIoctl(SIO_KEEPALIVE_VALS) failed: ", errbufsize);
			goto cancel_errReturn;
		}
#endif							/* SIO_KEEPALIVE_VALS */
#endif							/* WIN32 */

		/* 非阻塞                                               */
#ifdef TCP_USER_TIMEOUT
		if (!optional_setsockopt(tmpsock, IPPROTO_TCP, TCP_USER_TIMEOUT,
								 cancel->pgtcp_user_timeout))
		{
			strlcpy(errbuf, "PQcancel() -- setsockopt(TCP_USER_TIMEOUT) failed: ", errbufsize);
			goto cancel_errReturn;
		}
#endif
	}

retry3:
	if (connect(tmpsock, (struct sockaddr *) &cancel->raddr.addr,
				cancel->raddr.salen) < 0)
	{
		if (SOCK_ERRNO == EINTR)
			/* 不请求 SSL                                  */
			goto retry3;
		strlcpy(errbuf, "PQcancel() -- connect() failed: ", errbufsize);
		goto cancel_errReturn;
	}

	/* 按 host/hostaddr 连                      */

	crp.packetlen = pg_hton32((uint32) sizeof(crp));
	crp.cp.cancelRequestCode = (MsgType) pg_hton32(CANCEL_REQUEST_CODE);
	crp.cp.backendPID = pg_hton32(cancel->be_pid);
	crp.cp.cancelAuthCode = pg_hton32(cancel->be_key);

retry4:
	if (send(tmpsock, (char *) &crp, sizeof(crp), 0) != (int) sizeof(crp))
	{
		if (SOCK_ERRNO == EINTR)
			/* 中断系统调用-重试                      */
			goto retry4;
		strlcpy(errbuf, "PQcancel() -- send() failed: ", errbufsize);
		goto cancel_errReturn;
	}

	/*
	 * 等 postmaster 关连表示已
	 * 处理取消。不期待回复。
	                                                                                                    
                                                                                                  
                                                                                                  
*/
retry5:
	if (recv(tmpsock, (char *) &crp, 1, 0) < 0)
	{
		if (SOCK_ERRNO == EINTR)
			/* pqReadData() 出错或断连                   */
			goto retry5;
		/* 我们忽略其他错误情况   */
	}

	/* 完成   */
	closesocket(tmpsock);
	SOCK_ERRNO_SET(save_errno);
	return true;

cancel_errReturn:

	/*
	 * 确保不溢出错误缓冲。留 \n 空间。
	                                                                    
*/
	maxlen = errbufsize - strlen(errbuf) - 2;
	if (maxlen >= 0)
	{
		/*
		 * 不能调 strerror 非信号安全。
		 * 暂用通用消息。
		                                                                                             
*/
		int			val = SOCK_ERRNO;
		char		buf[32];
		char	   *bufp;

		bufp = buf + sizeof(buf) - 1;
		*bufp = '\0';
		do
		{
			*(--bufp) = (val % 10) + '0';
			val /= 10;
		} while (val > 0);
		bufp -= 6;
		memcpy(bufp, "error ", 6);
		strncat(errbuf, bufp, maxlen);
		strcat(errbuf, "\n");
	}
	if (tmpsock != PGINVALID_SOCKET)
		closesocket(tmpsock);
	SOCK_ERRNO_SET(save_errno);
	return false;
}


/*
 * PQrequestCancel：老的、非线程
 * 安全的请求查询取消函数。
                                                  
                                                
                                                
                                                
                                                
                                                
                                                
                                                
*/
int
PQrequestCancel(PGconn *conn)
{
	int			r;
	PGcancel   *cancel;

	/* 检查是否有开连接         */
	if (!conn)
		return false;

	if (conn->sock == PGINVALID_SOCKET)
	{
		strlcpy(conn->errorMessage.data,
				"PQrequestCancel() -- connection is not open\n",
				conn->errorMessage.maxlen);
		conn->errorMessage.len = strlen(conn->errorMessage.data);
		conn->errorReported = 0;

		return false;
	}

	cancel = PQgetCancel(conn);
	if (cancel)
	{
		r = PQcancel(cancel, conn->errorMessage.data,
					 conn->errorMessage.maxlen);
		PQfreeCancel(cancel);
	}
	else
	{
		strlcpy(conn->errorMessage.data, "out of memory",
				conn->errorMessage.maxlen);
		r = false;
	}

	if (!r)
	{
		conn->errorMessage.len = strlen(conn->errorMessage.data);
		conn->errorReported = 0;
	}

	return r;
}


/*
 * pqPacketSend() 便捷例程。
 *
 * 打包数据发到服务器。
                                                     
                                                    
                                                    
                                                    
                                                    
                                                   
                                                   
*/
int
pqPacketSend(PGconn *conn, char pack_type,
			 const void *buf, size_t buf_len)
{
	/* 开始消息。    */
	if (pqPutMsgStart(pack_type, conn))
		return STATUS_ERROR;

	/* 发送消息主体。  */
	if (pqPutnchar(buf, buf_len, conn))
		return STATUS_ERROR;

	/* 完成消息。     */
	if (pqPutMsgEnd(conn))
		return STATUS_ERROR;

	/* 刷确保后端收。            */
	if (pqFlush(conn))
		return STATUS_ERROR;

	return STATUS_OK;
}

#ifdef USE_LDAP

#define LDAP_URL	"ldap://"
#define LDAP_DEF_PORT	389
#define PGLDAP_TIMEOUT 2

#define ld_is_sp_tab(x) ((x) == ' ' || (x) == '\t')
#define ld_is_nl_cr(x) ((x) == '\r' || (x) == '\n')


/*
 *		ldapServiceLookup
 *
 * 搜索 LDAP URL，结果作连接选项
 * 解析到选项中仅填未设项。
 *
 * 出错将错误存 errorMessage 返 1，
 * 否则返 0。（返码为兼容。）
                                             
                                            
                                            
                                            
                                            
                                            
                                            
                                           
                                           
                                           
*/
static int
ldapServiceLookup(const char *purl, PQconninfoOption *options,
				  PQExpBuffer errorMessage)
{
	int			port = LDAP_DEF_PORT,
				scope,
				rc,
				size,
				state,
				oldstate,
				i;
#ifndef WIN32
	int			msgid;
#endif
	bool		found_keyword;
	char	   *url,
			   *hostname,
			   *portstr,
			   *endptr,
			   *dn,
			   *scopestr,
			   *filter,
			   *result,
			   *p,
			   *p1 = NULL,
			   *optname = NULL,
			   *optval = NULL;
	char	   *attrs[2] = {NULL, NULL};
	LDAP	   *ld = NULL;
	LDAPMessage *res,
			   *entry;
	struct berval **values;
	LDAP_TIMEVAL time = {PGLDAP_TIMEOUT, 0};

	if ((url = strdup(purl)) == NULL)
	{
		libpq_append_error(errorMessage, "out of memory");
		return 3;
	}

	/*
	 * 解析 URL 组件检查正确性。
	                                                           
                                                          
*/

	if (pg_strncasecmp(url, LDAP_URL, strlen(LDAP_URL)) != 0)
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": scheme must be ldap://", purl);
		free(url);
		return 3;
	}

	/* hostname */
	hostname = url + strlen(LDAP_URL);
	if (*hostname == '/')		/* 无主机名 */
		hostname = DefaultHost; /* 默认      */

	/* 若需用引号括       */
	p = strchr(url + strlen(LDAP_URL), '/');
	if (p == NULL || *(p + 1) == '\0' || *(p + 1) == '?')
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": missing distinguished name",
						   purl);
		free(url);
		return 3;
	}
	*p = '\0';					/* 终止主机名    */
	dn = p + 1;

	/* 属性    */
	if ((p = strchr(dn, '?')) == NULL || *(p + 1) == '\0' || *(p + 1) == '?')
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": must have exactly one attribute",
						   purl);
		free(url);
		return 3;
	}
	*p = '\0';
	attrs[0] = p + 1;

	/* scope */
	if ((p = strchr(attrs[0], '?')) == NULL || *(p + 1) == '\0' || *(p + 1) == '?')
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": must have search scope (base/one/sub)",
						   purl);
		free(url);
		return 3;
	}
	*p = '\0';
	scopestr = p + 1;

	/* filter */
	if ((p = strchr(scopestr, '?')) == NULL || *(p + 1) == '\0' || *(p + 1) == '?')
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": no filter",
						   purl);
		free(url);
		return 3;
	}
	*p = '\0';
	filter = p + 1;
	if ((p = strchr(filter, '?')) != NULL)
		*p = '\0';

	/* 端口号？ */
	if ((p1 = strchr(hostname, ':')) != NULL)
	{
		long		lport;

		*p1 = '\0';
		portstr = p1 + 1;
		errno = 0;
		lport = strtol(portstr, &endptr, 10);
		if (*portstr == '\0' || *endptr != '\0' || errno || lport < 0 || lport > 65535)
		{
			libpq_append_error(errorMessage,
							   "invalid LDAP URL \"%s\": invalid port number",
							   purl);
			free(url);
			return 3;
		}
		port = (int) lport;
	}

	/* 失败                   */
	if (strchr(attrs[0], ',') != NULL)
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": must have exactly one attribute",
						   purl);
		free(url);
		return 3;
	}

	/* 设范围 */
	if (pg_strcasecmp(scopestr, "base") == 0)
		scope = LDAP_SCOPE_BASE;
	else if (pg_strcasecmp(scopestr, "one") == 0)
		scope = LDAP_SCOPE_ONELEVEL;
	else if (pg_strcasecmp(scopestr, "sub") == 0)
		scope = LDAP_SCOPE_SUBTREE;
	else
	{
		libpq_append_error(errorMessage,
						   "invalid LDAP URL \"%s\": must have search scope (base/one/sub)",
						   purl);
		free(url);
		return 3;
	}

	/* 初始化 LDAP 结构     */
	if ((ld = ldap_init(hostname, port)) == NULL)
	{
		libpq_append_error(errorMessage, "could not create LDAP structure");
		free(url);
		return 3;
	}

	/*
	 * 执行显式匿名绑定。
	 *
	 * LDAP 不要求匿名绑定可搜索，
	 * 但某些服务器（如 NetApp）
	 * 无绑定会报错！
	                                                                   
                                                                  
                                                                  
                                                                  
                                                                  
*/
#ifdef WIN32
	/* 复制结果                                                     */
	if (ldap_connect(ld, &time) != LDAP_SUCCESS)
	{
		/* 关注估计大小               */
		free(url);
		ldap_unbind(ld);
		return 2;
	}
#else							/* !WIN32 */
	/* 处理超范围错误                                */
	if (ldap_set_option(ld, LDAP_OPT_NETWORK_TIMEOUT, &time) != LDAP_SUCCESS)
	{
		free(url);
		ldap_unbind(ld);
		return 3;
	}

	/* 匿名绑定   */
	if ((msgid = ldap_simple_bind(ld, NULL, NULL)) == -1)
	{
		/* 获首条目             */
		free(url);
		ldap_unbind(ld);
		return 2;
	}

	/* 获属性                                    */
	res = NULL;
	if ((rc = ldap_result(ld, msgid, LDAP_MSG_ALL, &time, &res)) == -1 ||
		res == NULL)
	{
		/* 错误或超时  */
		if (res != NULL)
			ldap_msgfree(res);
		free(url);
		ldap_unbind(ld);
		return 2;
	}
	ldap_msgfree(res);

	/* 重置超时  */
	time.tv_sec = -1;
	if (ldap_set_option(ld, LDAP_OPT_NETWORK_TIMEOUT, &time) != LDAP_SUCCESS)
	{
		free(url);
		ldap_unbind(ld);
		return 3;
	}
#endif							/* WIN32 */

	/* 搜索 */
	res = NULL;
	if ((rc = ldap_search_st(ld, dn, scope, filter, attrs, 0, &time, &res))
		!= LDAP_SUCCESS)
	{
		if (res != NULL)
			ldap_msgfree(res);
		libpq_append_error(errorMessage, "lookup on LDAP server failed: %s", ldap_err2string(rc));
		ldap_unbind(ld);
		free(url);
		return 1;
	}

	/* 此属性值数                              */
	if ((rc = ldap_count_entries(ld, res)) != 1)
	{
		if (rc > 1)
			libpq_append_error(errorMessage, "more than one entry found on LDAP lookup");
		else
			libpq_append_error(errorMessage, "no entry found on LDAP lookup");
		ldap_msgfree(res);
		ldap_unbind(ld);
		free(url);
		return 1;
	}

	/* 获条目 */
	if ((entry = ldap_first_entry(ld, res)) == NULL)
	{
		/* 不应发生        */
		libpq_append_error(errorMessage, "no entry found on LDAP lookup");
		ldap_msgfree(res);
		ldap_unbind(ld);
		free(url);
		return 1;
	}

	/* 获值     */
	if ((values = ldap_get_values_len(ld, entry, attrs[0])) == NULL)
	{
		libpq_append_error(errorMessage, "attribute has no values on LDAP lookup");
		ldap_msgfree(res);
		ldap_unbind(ld);
		free(url);
		return 1;
	}

	ldap_msgfree(res);
	free(url);

	if (values[0] == NULL)
	{
		libpq_append_error(errorMessage, "attribute has no values on LDAP lookup");
		ldap_value_free_len(values);
		ldap_unbind(ld);
		return 1;
	}

	/* 迭代所有属性值                                            */
	size = 1;					/* 为尾部 null        */
	for (i = 0; values[i] != NULL; i++)
	{
		if (values[i]->bv_len >= INT_MAX ||
			size > (INT_MAX - (values[i]->bv_len + 1)))
		{
			libpq_append_error(errorMessage,
							   "connection info string size exceeds the maximum allowed (%d)",
							   INT_MAX);
			ldap_value_free_len(values);
			ldap_unbind(ld);
			return 3;
		}

		size += values[i]->bv_len + 1;
	}

	if ((result = malloc(size)) == NULL)
	{
		libpq_append_error(errorMessage, "out of memory");
		ldap_value_free_len(values);
		ldap_unbind(ld);
		return 3;
	}
	p = result;
	for (i = 0; values[i] != NULL; i++)
	{
		memcpy(p, values[i]->bv_val, values[i]->bv_len);
		p += values[i]->bv_len;
		*(p++) = '\n';
	}
	*p = '\0';

	ldap_value_free_len(values);
	ldap_unbind(ld);

	/* NULL 终止列表   */
	oldstate = state = 0;
	for (p = result; *p != '\0'; ++p)
	{
		switch (state)
		{
			case 0:				/* 条目间       */
				if (!ld_is_sp_tab(*p) && !ld_is_nl_cr(*p))
				{
					optname = p;
					state = 1;
				}
				break;
			case 1:				/* 选项名中   */
				if (ld_is_sp_tab(*p))
				{
					*p = '\0';
					state = 2;
				}
				else if (ld_is_nl_cr(*p))
				{
					libpq_append_error(errorMessage,
									   "missing \"=\" after \"%s\" in connection info string",
									   optname);
					free(result);
					return 3;
				}
				else if (*p == '=')
				{
					*p = '\0';
					state = 3;
				}
				break;
			case 2:				/* 选项名后      */
				if (*p == '=')
				{
					state = 3;
				}
				else if (!ld_is_sp_tab(*p))
				{
					libpq_append_error(errorMessage,
									   "missing \"=\" after \"%s\" in connection info string",
									   optname);
					free(result);
					return 3;
				}
				break;
			case 3:				/* 选项值前        */
				if (*p == '\'')
				{
					optval = p + 1;
					p1 = p + 1;
					state = 5;
				}
				else if (ld_is_nl_cr(*p))
				{
					optval = optname + strlen(optname); /* 空   */
					state = 0;
				}
				else if (!ld_is_sp_tab(*p))
				{
					optval = p;
					state = 4;
				}
				break;
			case 4:				/* 释放结果             */
				if (ld_is_sp_tab(*p) || ld_is_nl_cr(*p))
				{
					*p = '\0';
					state = 0;
				}
				break;
			case 5:				/* 释放连接           */
				if (*p == '\'')
				{
					*p1 = '\0';
					state = 0;
				}
				else if (*p == '\\')
					state = 6;
				else
					*(p1++) = *p;
				break;
			case 6:				/* 释放连接                        */
				*(p1++) = *p;
				state = 5;
				break;
		}

		if (state == 0 && oldstate != 0)
		{
			found_keyword = false;
			for (i = 0; options[i].keyword; i++)
			{
				if (strcmp(options[i].keyword, optname) == 0)
				{
					if (options[i].val == NULL)
					{
						options[i].val = strdup(optval);
						if (!options[i].val)
						{
							libpq_append_error(errorMessage, "out of memory");
							free(result);
							return 3;
						}
					}
					found_keyword = true;
					break;
				}
			}
			if (!found_keyword)
			{
				libpq_append_error(errorMessage, "invalid connection option \"%s\"", optname);
				free(result);
				return 1;
			}
			optname = NULL;
			optval = NULL;
		}
		oldstate = state;
	}

	free(result);

	if (state == 5 || state == 6)
	{
		libpq_append_error(errorMessage,
						   "unterminated quoted string in connection info string");
		return 3;
	}

	return 0;
}

#endif							/* USE_LDAP */

/*
 * parseServiceInfo：若给服务名则
 * 查找并吸收选项到现有数组中。
 *
 * 返 0 成功非零失败。
                                                                                    
                                                                                   
                                                                                  
                                                                                  
*/
static int
parseServiceInfo(PQconninfoOption *options, PQExpBuffer errorMessage)
{
	const char *service = conninfo_getval(options, "service");
	char		serviceFile[MAXPGPATH];
	char	   *env;
	bool		group_found = false;
	int			status;
	struct stat stat_buf;

	/*
	 * 须特殊处理 PGSERVICE 因
	 * 服务名默认由此指定。
	                                                                                                               
*/
	if (service == NULL)
		service = getenv("PGSERVICE");

	/* 未指定服务名，无事可做       */
	if (service == NULL)
		return 0;

	/*
	 * 试 PGSERVICEFILE，否则
	 * ~/.pg_service.conf。
	                              */
	if ((env = getenv("PGSERVICEFILE")) != NULL)
		strlcpy(serviceFile, env, sizeof(serviceFile));
	else
	{
		char		homedir[MAXPGPATH];

		if (!pqGetHomeDirectory(homedir, sizeof(homedir)))
			goto next_file;
		snprintf(serviceFile, MAXPGPATH, "%s/%s", homedir, ".pg_service.conf");
		if (stat(serviceFile, &stat_buf) != 0)
			goto next_file;
	}

	status = parseServiceFile(serviceFile, service, options, errorMessage, &group_found);
	if (group_found || status != 0)
		return status;

next_file:

	/*
	 * 任何应用可用，故不能用
	 * 二进制位置定位 sysconfdir。
	                               */
	snprintf(serviceFile, MAXPGPATH, "%s/pg_service.conf",
			 getenv("PGSYSCONFDIR") ? getenv("PGSYSCONFDIR") : SYSCONFDIR);
	if (stat(serviceFile, &stat_buf) != 0)
		goto last_file;

	status = parseServiceFile(serviceFile, service, options, errorMessage, &group_found);
	if (status != 0)
		return status;

last_file:
	if (!group_found)
	{
		libpq_append_error(errorMessage, "definition of service \"%s\" not found", service);
		return 3;
	}

	return 0;
}

static int
parseServiceFile(const char *serviceFile,
				 const char *service,
				 PQconninfoOption *options,
				 PQExpBuffer errorMessage,
				 bool *group_found)
{
	int			result = 0,
				linenr = 0,
				i;
	FILE	   *f;
	char	   *line;
	char		buf[1024];

	*group_found = false;

	f = fopen(serviceFile, "r");
	if (f == NULL)
	{
		libpq_append_error(errorMessage, "service file \"%s\" not found", serviceFile);
		return 1;
	}

	while ((line = fgets(buf, sizeof(buf), f)) != NULL)
	{
		int			len;

		linenr++;

		if (strlen(line) >= sizeof(buf) - 1)
		{
			libpq_append_error(errorMessage,
							   "line %d too long in service file \"%s\"",
							   linenr,
							   serviceFile);
			result = 2;
			goto exit;
		}

		/* 忽略行尾空白，尤其换行符                     */
		len = strlen(line);
		while (len > 0 && isspace((unsigned char) line[len - 1]))
			line[--len] = '\0';

		/* 忽略前导空格            */
		while (*line && isspace((unsigned char) line[0]))
			line++;

		/* 忽略注释和空行           */
		if (line[0] == '\0' || line[0] == '#')
			continue;

		/* 检查正确的组名     */
		if (line[0] == '[')
		{
			if (*group_found)
			{
				/* 达目标组尾,返回成功                 */
				goto exit;
			}

			if (strncmp(line + 1, service, strlen(service)) == 0 &&
				line[strlen(service) + 1] == ']')
				*group_found = true;
			else
				*group_found = false;
		}
		else
		{
			if (*group_found)
			{
				/*
				 * 最终已进入正确组，可解析该行
				                */
				char	   *key,
						   *val;
				bool		found_keyword;

#ifdef USE_LDAP
				if (strncmp(line, "ldap", 4) == 0)
				{
					int			rc = ldapServiceLookup(line, options, errorMessage);

					/* rc=2 继续读后备                  */
					switch (rc)
					{
						case 0:
							goto exit;
						case 1:
						case 3:
							result = 3;
							goto exit;
						case 2:
							continue;
					}
				}
#endif

				key = line;
				val = strchr(line, '=');
				if (val == NULL)
				{
					libpq_append_error(errorMessage,
									   "syntax error in service file \"%s\", line %d",
									   serviceFile,
									   linenr);
					result = 3;
					goto exit;
				}
				*val++ = '\0';

				if (strcmp(key, "service") == 0)
				{
					libpq_append_error(errorMessage,
									   "nested service specifications not supported in service file \"%s\", line %d",
									   serviceFile,
									   linenr);
					result = 3;
					goto exit;
				}

				/*
				 * 设参数但不覆任何先前
				 * 显式设置的值。
				                    */
				found_keyword = false;
				for (i = 0; options[i].keyword; i++)
				{
					if (strcmp(options[i].keyword, key) == 0)
					{
						if (options[i].val == NULL)
							options[i].val = strdup(val);
						if (!options[i].val)
						{
							libpq_append_error(errorMessage, "out of memory");
							result = 3;
							goto exit;
						}
						found_keyword = true;
						break;
					}
				}

				if (!found_keyword)
				{
					libpq_append_error(errorMessage,
									   "syntax error in service file \"%s\", line %d",
									   serviceFile,
									   linenr);
					result = 3;
					goto exit;
				}
			}
		}
	}

exit:
	fclose(f);

	return result;
}


/*
 *		PQconninfoParse
 *
 * 如 PQconnectdb() 解析串，返
 * 连接选项数组。失败返 NULL。
 * 仅含显式指定项不含默认值。
 *
 * errmsg 非 NULL 时 *errmsg
 * 设 NULL 成功或 malloc 错。
 * 需 PQfreemem() 释放。
                                                                                     
                                                                                    
                                                                                    
                                                                                   
*/
PQconninfoOption *
PQconninfoParse(const char *conninfo, char **errmsg)
{
	PQExpBufferData errorBuf;
	PQconninfoOption *connOptions;

	if (errmsg)
		*errmsg = NULL;			/* 默认  */
	initPQExpBuffer(&errorBuf);
	if (PQExpBufferDataBroken(errorBuf))
		return NULL;			/* 内存已耗尽 :-(       */
	connOptions = parse_connection_string(conninfo, &errorBuf, false);
	if (connOptions == NULL && errmsg)
		*errmsg = errorBuf.data;
	else
		termPQExpBuffer(&errorBuf);
	return connOptions;
}

/*
 * 构建常量 PQconninfoOptions 数组的工作副本。
       */
static PQconninfoOption *
conninfo_init(PQExpBuffer errorMessage)
{
	PQconninfoOption *options;
	PQconninfoOption *opt_dest;
	const internalPQconninfoOption *cur_opt;

	/*
	 * 取够内存容所有选项，
	 * 即使部分最后不用。
	                                      */
	options = (PQconninfoOption *) malloc(sizeof(PQconninfoOption) * sizeof(PQconninfoOptions) / sizeof(PQconninfoOptions[0]));
	if (options == NULL)
	{
		libpq_append_error(errorMessage, "out of memory");
		return NULL;
	}
	opt_dest = options;

	for (cur_opt = PQconninfoOptions; cur_opt->keyword; cur_opt++)
	{
		/* 默认设置不在需设置项中                              */
		memcpy(opt_dest, cur_opt, sizeof(PQconninfoOption));
		opt_dest++;
	}
	MemSet(opt_dest, 0, sizeof(PQconninfoOption));

	return options;
}

/*
 * 连接串解析器
 *
 * 返 malloc 的选项数组或 NULL。
                                                          
                                                        
                                                        
                                                        
*/
static PQconninfoOption *
parse_connection_string(const char *connstr, PQExpBuffer errorMessage,
						bool use_defaults)
{
	/* 若识为连接串试作 URI 解析                  */
	if (uri_prefix_length(connstr) != 0)
		return conninfo_uri_parse(connstr, errorMessage, use_defaults);

	/* 返连接选项            */
	return conninfo_parse(connstr, errorMessage, use_defaults);
}

/*
 * 检查是否以有效 URI 前缀开头。
                                   
                                  
                                  
                                  
                                  
*/
static int
uri_prefix_length(const char *connstr)
{
	if (strncmp(connstr, uri_designator,
				sizeof(uri_designator) - 1) == 0)
		return sizeof(uri_designator) - 1;

	if (strncmp(connstr, short_uri_designator,
				sizeof(short_uri_designator) - 1) == 0)
		return sizeof(short_uri_designator) - 1;

	return 0;
}

/*
 * 识别的连接串以 URI 前缀
 * 开头或含 "="。
                                                
                                               
                                               
                                              
                                              
*/
static bool
recognized_connection_string(const char *connstr)
{
	return uri_prefix_length(connstr) != 0 || strchr(connstr, '=') != NULL;
}

/*
 * parse_connection_string 子过程
 *
 * 处理含 "=" 的串。
                             */
static PQconninfoOption *
conninfo_parse(const char *conninfo, PQExpBuffer errorMessage,
			   bool use_defaults)
{
	char	   *pname;
	char	   *pval;
	char	   *buf;
	char	   *cp;
	char	   *cp2;
	PQconninfoOption *options;

	/* 创建 PQconninfoOptions 工作副本    */
	options = conninfo_init(errorMessage);
	if (options == NULL)
		return NULL;

	/* 要可修改的输入串副本             */
	if ((buf = strdup(conninfo)) == NULL)
	{
		libpq_append_error(errorMessage, "out of memory");
		PQconninfoFree(options);
		return NULL;
	}
	cp = buf;

	while (*cp)
	{
		/* 跳过参数名前的空白           */
		if (isspace((unsigned char) *cp))
		{
			cp++;
			continue;
		}

		/* 获取参数名        */
		pname = cp;
		while (*cp)
		{
			if (*cp == '=')
				break;
			if (isspace((unsigned char) *cp))
			{
				*cp++ = '\0';
				while (*cp)
				{
					if (!isspace((unsigned char) *cp))
						break;
					cp++;
				}
				break;
			}
			cp++;
		}

		/* 检查是否有后继 =             */
		if (*cp != '=')
		{
			libpq_append_error(errorMessage,
							   "missing \"=\" after \"%s\" in connection info string",
							   pname);
			PQconninfoFree(options);
			free(buf);
			return NULL;
		}
		*cp++ = '\0';

		/* 跳过 = 后空格        */
		while (*cp)
		{
			if (!isspace((unsigned char) *cp))
				break;
			cp++;
		}

		/* 取参数值            */
		pval = cp;

		if (*cp != '\'')
		{
			cp2 = pval;
			while (*cp)
			{
				if (isspace((unsigned char) *cp))
				{
					*cp++ = '\0';
					break;
				}
				if (*cp == '\\')
				{
					cp++;
					if (*cp != '\0')
						*cp2++ = *cp++;
				}
				else
					*cp2++ = *cp++;
			}
			*cp2 = '\0';
		}
		else
		{
			cp2 = pval;
			cp++;
			for (;;)
			{
				if (*cp == '\0')
				{
					libpq_append_error(errorMessage, "unterminated quoted string in connection info string");
					PQconninfoFree(options);
					free(buf);
					return NULL;
				}
				if (*cp == '\\')
				{
					cp++;
					if (*cp != '\0')
						*cp2++ = *cp++;
					continue;
				}
				if (*cp == '\'')
				{
					*cp2 = '\0';
					cp++;
					break;
				}
				*cp2++ = *cp++;
			}
		}

		/*
		 * 已取得名称和值，存储该记录。
		                 */
		if (!conninfo_storeval(options, pname, pval, errorMessage, false, false))
		{
			PQconninfoFree(options);
			free(buf);
			return NULL;
		}
	}

	/* 可变输入串已用完              */
	free(buf);

	/*
	 * 若调用者需要则加入默认值。
	   */
	if (use_defaults)
	{
		if (!conninfo_add_defaults(options, errorMessage))
		{
			PQconninfoFree(options);
			return NULL;
		}
	}

	return options;
}

/*
 * Conninfo 数组解析器
 *
 * 成功返 malloc 选项数组。
 * 不成功返 NULL。
 *
 * use_defaults 为 TRUE 用默认值
 * 填选项；否则仅设显式值。
 *
 * expand_dbname 非零扩展 dbname。
                                                                                                    
                                                                                                   
                                                                                                   
                                                                                                   
                                                                                                   
                                                                                                  
                                                                                                  
                                                                                                  
*/
static PQconninfoOption *
conninfo_array_parse(const char *const *keywords, const char *const *values,
					 PQExpBuffer errorMessage, bool use_defaults,
					 int expand_dbname)
{
	PQconninfoOption *options;
	PQconninfoOption *dbname_options = NULL;
	PQconninfoOption *option;
	int			i = 0;

	/*
	 * expand_dbname 非零检查
	 * dbname 是否连接串。
	                                                           */
	while (expand_dbname && keywords[i])
	{
		const char *pname = keywords[i];
		const char *pvalue = values[i];

		/* 先找 "dbname"            */
		if (strcmp(pname, "dbname") == 0 && pvalue)
		{
			/*
			 * 若是连接串解析但不
			 * 用默认值。
			                                                                                                                               
*/
			if (recognized_connection_string(pvalue))
			{
				dbname_options = parse_connection_string(pvalue, errorMessage, false);
				if (dbname_options == NULL)
					return NULL;
			}
			break;
		}
		++i;
	}

	/* 创建 PQconninfoOptions 工作副本    */
	options = conninfo_init(errorMessage);
	if (options == NULL)
	{
		PQconninfoFree(dbname_options);
		return NULL;
	}

	/* 解析关键词/值数组        */
	i = 0;
	while (keywords[i])
	{
		const char *pname = keywords[i];
		const char *pvalue = values[i];

		if (pvalue != NULL && pvalue[0] != '\0')
		{
			/* 未解析用原始值       */
			for (option = options; option->keyword != NULL; option++)
			{
				if (strcmp(option->keyword, pname) == 0)
					break;
			}

			/* 检查无效连接选项            */
			if (option->keyword == NULL)
			{
				libpq_append_error(errorMessage, "invalid connection option \"%s\"", pname);
				PQconninfoFree(options);
				PQconninfoFree(dbname_options);
				return NULL;
			}

			/*
			 * 首个 dbname 且有解析
			 * 连接串，替换现选项。
			                                                                                                      
*/
			if (strcmp(pname, "dbname") == 0 && dbname_options)
			{
				PQconninfoOption *str_option;

				for (str_option = dbname_options; str_option->keyword != NULL; str_option++)
				{
					if (str_option->val != NULL)
					{
						int			k;

						for (k = 0; options[k].keyword; k++)
						{
							if (strcmp(options[k].keyword, str_option->keyword) == 0)
							{
								free(options[k].val);
								options[k].val = strdup(str_option->val);
								if (!options[k].val)
								{
									libpq_append_error(errorMessage, "out of memory");
									PQconninfoFree(options);
									PQconninfoFree(dbname_options);
									return NULL;
								}
								break;
							}
						}
					}
				}

				/*
				 * 忘掉解析连接串，
				 * 后续 dbname 覆盖。
				                                                    */
				PQconninfoFree(dbname_options);
				dbname_options = NULL;
			}
			else
			{
				/* 用解析连接覆现选项               
              
*/
				free(option->val);
				option->val = strdup(pvalue);
				if (!option->val)
				{
					libpq_append_error(errorMessage, "out of memory");
					PQconninfoFree(options);
					PQconninfoFree(dbname_options);
					return NULL;
				}
			}
		}
		++i;
	}
	PQconninfoFree(dbname_options);

	/* 对基连接无用了             
            
*/
	if (use_defaults)
	{
		if (!conninfo_add_defaults(options, errorMessage))
		{
			PQconninfoFree(options);
			return NULL;
		}
	}

	return options;
}

/*
 * 为未指定选项添加默认值。
 *
 * 若有服务说明符用其获取
 * 未显式给的值。否则试环境
 * 变量。再否则用编译选项。
 *
 * 填充后仅连接串未指定的
 * 选项标 val=NULL，表示
 * 隐式设默认值。
                                                                                                                                                    */
static bool
conninfo_add_defaults(PQconninfoOption *options, PQExpBuffer errorMessage)
{
	PQconninfoOption *option;
	PQconninfoOption *sslmode_default = NULL,
			   *sslrootcert = NULL;
	char	   *tmp;

	/*
	 * 若有服务说明符用其获取
	 * 未显式给的值。
	                                                                                                                                          
*/
	if (parseServiceInfo(options, errorMessage) != 0 && errorMessage)
		return false;

	/*
	 * 获取未指定参数的后备资源。
	                                                            
*/
	for (option = options; option->keyword != NULL; option++)
	{
		if (strcmp(option->keyword, "sslrootcert") == 0)
			sslrootcert = option;	/* 留后用      */

		if (option->val != NULL)
			continue;			/* 环境变量未设               */

		/* 无值则报错                   
                 
*/
		if (option->envvar != NULL)
		{
			if ((tmp = getenv(option->envvar)) != NULL)
			{
				option->val = strdup(tmp);
				if (!option->val)
				{
					if (errorMessage)
						libpq_append_error(errorMessage, "out of memory");
					return false;
				}
				continue;
			}
		}

		/*
		 * 解释弃用 PGREQUIRESSL。
		 * 转换：1/true=>sslmode=require
		 * 其他=>作正常 sslmode 处理
		                                                                                                                                                                           
*/
		if (strcmp(option->keyword, "sslmode") == 0)
		{
			const char *requiresslenv = getenv("PGREQUIRESSL");

			if (requiresslenv != NULL && requiresslenv[0] == '1')
			{
				option->val = strdup("require");
				if (!option->val)
				{
					if (errorMessage)
						libpq_append_error(errorMessage, "out of memory");
					return false;
				}
				continue;
			}

			/*
			 * sslmode 未指定。
			 * 用编译默认填。
			                                                                                                                          
*/
			sslmode_default = option;
		}

		/*
		 * 无环境变量或未设-试编译默认
		                                                  
*/
		if (option->compiled != NULL)
		{
			option->val = strdup(option->compiled);
			if (!option->val)
			{
				if (errorMessage)
					libpq_append_error(errorMessage, "out of memory");
				return false;
			}
			continue;
		}

		/*
		 * "user" 选项特殊处理。注意
		 * pg_fe_getauthname() 在其
		 * 他方法前后也会失败而
		 * 不存错误消息。
		                                                                                                                                                                                                                       
*/
		if (strcmp(option->keyword, "user") == 0)
		{
			option->val = pg_fe_getauthname(NULL);
			continue;
		}
	}

	/*
	 * sslrootcert=system 且 sslmode
	 * 未显式定义的特殊处理。
	                                                                                      
*/
	if (sslmode_default && sslrootcert)
	{
		if (sslrootcert->val && strcmp(sslrootcert->val, "system") == 0)
		{
			free(sslmode_default->val);

			sslmode_default->val = strdup("verify-full");
			if (!sslmode_default->val)
			{
				if (errorMessage)
					libpq_append_error(errorMessage, "out of memory");
				return false;
			}
		}
	}

	return true;
}

/*
 * parse_connection_string 子过程
 *
 * 处理 URI 连接串。
                 */
static PQconninfoOption *
conninfo_uri_parse(const char *uri, PQExpBuffer errorMessage,
				   bool use_defaults)
{
	PQconninfoOption *options;

	/* 创建 PQconninfoOptions 工作副本    */
	options = conninfo_init(errorMessage);
	if (options == NULL)
		return NULL;

	if (!conninfo_uri_parse_options(options, uri, errorMessage))
	{
		PQconninfoFree(options);
		return NULL;
	}

	/*
	 * 若调用者需要则加入默认值。
	   */
	if (use_defaults)
	{
		if (!conninfo_add_defaults(options, errorMessage))
		{
			PQconninfoFree(options);
			return NULL;
		}
	}

	return options;
}

/*
 * conninfo_uri_parse_options
 *		实际 URI 解析器。
 *
 * 成功返 true 选项用 URI 值填。
 * URI 有问题返 false 设错误。
                                             
                                            
                                            
                                           
                                           
                                           
                                           
                                           
                                           
                                           
                                           
                                           
                                           
                                           
*/
static bool
conninfo_uri_parse_options(PQconninfoOption *options, const char *uri,
						   PQExpBuffer errorMessage)
{
	int			prefix_len;
	char	   *p;
	char	   *buf = NULL;
	char	   *start;
	char		prevchar = '\0';
	char	   *user = NULL;
	char	   *host = NULL;
	bool		retval = false;
	PQExpBufferData hostbuf;
	PQExpBufferData portbuf;

	initPQExpBuffer(&hostbuf);
	initPQExpBuffer(&portbuf);
	if (PQExpBufferDataBroken(hostbuf) || PQExpBufferDataBroken(portbuf))
	{
		libpq_append_error(errorMessage, "out of memory");
		goto cleanup;
	}

	/* 需输入 URI 可修改副本           */
	buf = strdup(uri);
	if (buf == NULL)
	{
		libpq_append_error(errorMessage, "out of memory");
		goto cleanup;
	}
	start = buf;

	/* 跳过 URI 前缀   */
	prefix_len = uri_prefix_length(uri);
	if (prefix_len == 0)
	{
		/* 需以 // 开头    */
		libpq_append_error(errorMessage,
						   "invalid URI propagated to internal parser routine: \"%s\"",
						   uri);
		goto cleanup;
	}
	start += prefix_len;
	p = start;

	/* 用 strchr 找尾                                   */
	while (*p && *p != '@' && *p != '/')
		++p;
	if (*p == '@')
	{
		/*
		 * 找到用户/密码指示符，
		 * URI 为 schema://user:pass@host
		                                       */
		user = start;

		p = user;
		while (*p != ':' && *p != '@')
			++p;

		/* 提取用户名                                */
		prevchar = *p;
		*p = '\0';

		if (*user &&
			!conninfo_storeval(options, "user", user,
							   errorMessage, false, true))
			goto cleanup;

		if (prevchar == ':')
		{
			const char *password = p + 1;

			while (*p != '@')
				++p;
			*p = '\0';

			if (*password &&
				!conninfo_storeval(options, "password", password,
								   errorMessage, false, true))
				goto cleanup;
		}

		/* 提取密码                                           */
		++p;
	}
	else
	{
		/* 移过用户/密码                          
                        
*/
		p = start;
	}

	/*
	 * 可有多个 netloc[:port] 对，
	 * 逗号分隔。
	                                                                                       
                                                                                      
                                                                                     
                                                                                     
*/
	for (;;)
	{
		/* 一次拆一个对      
     
*/
		if (*p == '[')
		{
			host = ++p;
			while (*p && *p != ']')
				++p;
			if (!*p)
			{
				libpq_append_error(errorMessage,
								   "end of string reached when looking for matching \"]\" in IPv6 host address in URI: \"%s\"",
								   uri);
				goto cleanup;
			}
			if (p == host)
			{
				libpq_append_error(errorMessage,
								   "IPv6 host address may not be empty in URI: \"%s\"",
								   uri);
				goto cleanup;
			}

			/* 推进始指针                 */
			*(p++) = '\0';

			/*
			 * 地址后可跟端口/斜杠/问号。
			                                                           
*/
			if (*p && *p != ':' && *p != '/' && *p != '?' && *p != ',')
			{
				libpq_append_error(errorMessage,
								   "unexpected character \"%c\" at position %d in URI (expected \":\" or \"/\"): \"%s\"",
								   *p, (int) (p - buf + 1), uri);
				goto cleanup;
			}
		}
		else
		{
			/* 开括号后或地址开始后                */
			host = p;

			/*
			 * 找端口（冒号）或主机
			 * 尾（斜杠）或问号。
			                                                           */
			while (*p && *p != ':' && *p != '/' && *p != '?' && *p != ',')
				++p;
		}

		/* IPv6 址无端口号不开括号               */
		prevchar = *p;
		*p = '\0';

		appendPQExpBufferStr(&hostbuf, host);

		if (prevchar == ':')
		{
			const char *port = ++p; /* 前进越过主机终止符  */

			while (*p && *p != '/' && *p != '?' && *p != ',')
				++p;

			prevchar = *p;
			*p = '\0';

			appendPQExpBufferStr(&portbuf, port);
		}

		if (prevchar != ',')
			break;
		++p;					/* 若给端口则取           */
		appendPQExpBufferChar(&hostbuf, ',');
		appendPQExpBufferChar(&portbuf, ',');
	}

	/* 确认有其余 *p                   */
	if (PQExpBufferDataBroken(hostbuf) || PQExpBufferDataBroken(portbuf))
		goto cleanup;
	if (hostbuf.data[0] &&
		!conninfo_storeval(options, "host", hostbuf.data,
						   errorMessage, false, true))
		goto cleanup;
	if (portbuf.data[0] &&
		!conninfo_storeval(options, "port", portbuf.data,
						   errorMessage, false, true))
		goto cleanup;

	if (prevchar && prevchar != '?')
	{
		const char *dbname = ++p;	/* 找 dbname                   */

		/* 无路径则 dbname=""    */
		while (*p && *p != '?')
			++p;

		prevchar = *p;
		*p = '\0';

		/*
		 * 免 dbname 空串强制默认
		 * 为用户名。
		                                                                                                        
*/
		if (*dbname &&
			!conninfo_storeval(options, "dbname", dbname,
							   errorMessage, false, true))
			goto cleanup;
	}

	if (prevchar)
	{
		++p;					/* 跳过斜杠            */

		if (!conninfo_uri_parse_params(p, options, errorMessage))
			goto cleanup;
	}

	/* 解析全 OK           */
	retval = true;

cleanup:
	termPQExpBuffer(&hostbuf);
	termPQExpBuffer(&portbuf);
	free(buf);
	return retval;
}

/*
 * URI 参数解析器
 *
 * 成功返 true 选项用 params
 * 填充。有问题返 false 设错。
                                                                       
                                                                     
*/
static bool
conninfo_uri_parse_params(char *params,
						  PQconninfoOption *connOptions,
						  PQExpBuffer errorMessage)
{
	while (*params)
	{
		char	   *keyword = params;
		char	   *value = NULL;
		char	   *p = params;
		bool		malloced = false;
		int			oldmsglen;

		/*
		 * 扫描 params 找 = 和 &，
		 * 标记关键词和值尾。
		                                   */
		for (;;)
		{
			if (*p == '=')
			{
				/* 现为单独值        */
				if (value != NULL)
				{
					libpq_append_error(errorMessage,
									   "extra key/value separator \"=\" in URI query parameter: \"%s\"",
									   keyword);
					return false;
				}
				/* 关键词后须跟 =              */
				*p++ = '\0';
				value = p;
			}
			else if (*p == '&' || *p == '\0')
			{
				/*
				 * 若非尾，截值进；
				 * p 指向 & 或尾。
				                                                          */
				if (*p != '\0')
					*p++ = '\0';
				/* 有 = 吗？          */
				if (value == NULL)
				{
					libpq_append_error(errorMessage,
									   "missing key/value separator \"=\" in URI query parameter: \"%s\"",
									   keyword);
					return false;
				}
				/* 让 pvalue 指值头                    */
				break;
			}
			else
				++p;			/* 只剩关键词               */
		}

		keyword = conninfo_uri_decode(keyword, errorMessage);
		if (keyword == NULL)
		{
			/* 检关键词有效                               */
			return false;
		}
		value = conninfo_uri_decode(value, errorMessage);
		if (value == NULL)
		{
			/* p 指向 & 或 \0                                */
			free(keyword);
			return false;
		}
		malloced = true;

		/* 现有 keyword=pkey, value=pvalue                
               
*/
		if (strcmp(keyword, "ssl") == 0 &&
			strcmp(value, "true") == 0)
		{
			free(keyword);
			free(value);
			malloced = false;

			keyword = "sslmode";
			value = "require";
		}

		/*
		 * 对应选项存在则存值；
		 * 否则忽略。正则连串中
		 * 未知关键词会报错。
		                                        */
		oldmsglen = errorMessage->len;
		if (!conninfo_storeval(connOptions, keyword, value,
							   errorMessage, true, false))
		{
			/* p 指 & 则前进                                            */
			if (errorMessage->len == oldmsglen)
				libpq_append_error(errorMessage,
								   "invalid URI query parameter: \"%s\"",
								   keyword);
			/* 失败。 */
			if (malloced)
			{
				free(keyword);
				free(value);
			}
			return false;
		}

		if (malloced)
		{
			free(keyword);
			free(value);
		}

		/* 无更多                              */
		params = p;
	}

	return true;
}

/*
 * URI 解码器
 *
 * 成功返含解码结果的 malloc 串。
 * 解码问题返 NULL 设错。
                                                                   
                                                                 
                                                                 
                                                                 
                                                                 
                                                                 
*/
static char *
conninfo_uri_decode(const char *str, PQExpBuffer errorMessage)
{
	char	   *buf;
	char	   *p;
	const char *q = str;

	buf = malloc(strlen(str) + 1);
	if (buf == NULL)
	{
		libpq_append_error(errorMessage, "out of memory");
		return NULL;
	}
	p = buf;

	for (;;)
	{
		if (*q != '%')
		{
			/* 仅解码含 % 的串             */
			if (!(*(p++) = *(q++)))
				break;
		}
		else
		{
			int			hi;
			int			lo;
			int			c;

			++q;				/* 无编码                    */

			/*
			 * EOL 会被首次 get_hexdigit
			 * 捕获，故不独检。
			                                                         */
			if (!(get_hexdigit(*q++, &hi) && get_hexdigit(*q++, &lo)))
			{
				libpq_append_error(errorMessage,
								   "invalid percent-encoded token: \"%s\"",
								   str);
				free(buf);
				return NULL;
			}

			c = (hi << 4) | lo;
			if (c == 0)
			{
				libpq_append_error(errorMessage,
								   "forbidden value %%00 in percent-encoded value: \"%s\"",
								   str);
				free(buf);
				return NULL;
			}
			*(p++) = c;
		}
	}

	return buf;
}

/*
 * 十六进制数字字符转整数值。
 *
 * 成功返 true 值存 *value。
 * 不成功返 false。
                                                                            
                                                                           
*/
static bool
get_hexdigit(char digit, int *value)
{
	if ('0' <= digit && digit <= '9')
		*value = digit - '0';
	else if ('A' <= digit && digit <= 'F')
		*value = digit - 'A' + 10;
	else if ('a' <= digit && digit <= 'f')
		*value = digit - 'a' + 10;
	else
		return false;

	return true;
}

/*
 * 在 connOptions 中找关键词
 * 对应的选项值。
                                                                  
                                                                
*/
static const char *
conninfo_getval(PQconninfoOption *connOptions,
				const char *keyword)
{
	PQconninfoOption *option;

	option = conninfo_find(connOptions, keyword);

	return option ? option->val : NULL;
}

/*
 * 在 connOptions 中存关键词对应
 * 的（新）选项值。
 *
 * uri_decode 为 true 值经 URI 解码。
 * ignoreMissing TRUE 时未找到关键
 * 词选项非致命。关键词大小写
 * 不敏感。
                                                                        
                                                                      
                                                                      
                                                                      
                                                                      
                                                                      
*/
static PQconninfoOption *
conninfo_storeval(PQconninfoOption *connOptions,
				  const char *keyword, const char *value,
				  PQExpBuffer errorMessage, bool ignoreMissing,
				  bool uri_decode)
{
	PQconninfoOption *option;
	char	   *value_copy;

	/*
	 * 向后兼容 requiressl=1 转
	 * sslmode=require，其他
	 * 当 sslmode 处理。
	                                                                                         */
	if (strcmp(keyword, "requiressl") == 0)
	{
		keyword = "sslmode";
		if (value[0] == '1')
			value = "require";
		else
			value = "prefer";
	}

	option = conninfo_find(connOptions, keyword);
	if (option == NULL)
	{
		if (!ignoreMissing)
			libpq_append_error(errorMessage,
							   "invalid connection option \"%s\"",
							   keyword);
		return NULL;
	}

	if (uri_decode)
	{
		value_copy = conninfo_uri_decode(value, errorMessage);
		if (value_copy == NULL)
			/* 不期待此处故障                            */
			return NULL;
	}
	else
	{
		value_copy = strdup(value);
		if (value_copy == NULL)
		{
			libpq_append_error(errorMessage, "out of memory");
			return NULL;
		}
	}

	free(option->val);
	option->val = value_copy;

	return option;
}

/*
 * 在 connOptions 中找关键词
 * 对应 PQconninfoOption。
                                       
                                      
                                     
                                     
*/
static PQconninfoOption *
conninfo_find(PQconninfoOption *connOptions, const char *keyword)
{
	PQconninfoOption *option;

	for (option = connOptions; option->keyword != NULL; option++)
	{
		if (strcmp(option->keyword, keyword) == 0)
			return option;
	}

	return NULL;
}


/* 选项数组以 keyword=NULL 终             
           
*/
PQconninfoOption *
PQconninfo(PGconn *conn)
{
	PQExpBufferData errorBuf;
	PQconninfoOption *connOptions;

	if (conn == NULL)
		return NULL;

	/*
	 * 其实这里并不报告错误，但被调用方需要缓冲区，  
	 * 而我们不想破坏 conn 的 errorMessage。     
	 */
	initPQExpBuffer(&errorBuf);
	if (PQExpBufferDataBroken(errorBuf))
		return NULL;			/* 内存已耗尽 :-(       */

	connOptions = conninfo_init(&errorBuf);

	if (connOptions != NULL)
	{
		const internalPQconninfoOption *option;

		for (option = PQconninfoOptions; option->keyword; option++)
		{
			char	  **connmember;

			if (option->connofs < 0)
				continue;

			connmember = (char **) ((char *) conn + option->connofs);

			if (*connmember)
				conninfo_storeval(connOptions, option->keyword, *connmember,
								  &errorBuf, true, false);
		}
	}

	termPQExpBuffer(&errorBuf);

	return connOptions;
}


void
PQconninfoFree(PQconninfoOption *connOptions)
{
	if (connOptions == NULL)
		return;

	for (PQconninfoOption *option = connOptions; option->keyword != NULL; option++)
		free(option->val);
	free(connOptions);
}


/* =========== PGconn 访问器 =========              */
char *
PQdb(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->dbName;
}

char *
PQuser(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->pguser;
}

char *
PQpass(const PGconn *conn)
{
	char	   *password = NULL;

	if (!conn)
		return NULL;
	if (conn->connhost != NULL)
		password = conn->connhost[conn->whichhost].password;
	if (password == NULL)
		password = conn->pgpass;
	/* OK 返串                                                         */
	if (password == NULL)
		password = "";
	return password;
}

char *
PQhost(const PGconn *conn)
{
	if (!conn)
		return NULL;

	if (conn->connhost != NULL)
	{
		/*
		 * 返用户提供的主机原值，
		 * 缺则返 hostaddr。
		                   */
		if (conn->connhost[conn->whichhost].host != NULL &&
			conn->connhost[conn->whichhost].host[0] != '\0')
			return conn->connhost[conn->whichhost].host;
		else if (conn->connhost[conn->whichhost].hostaddr != NULL &&
				 conn->connhost[conn->whichhost].hostaddr[0] != '\0')
			return conn->connhost[conn->whichhost].hostaddr;
	}

	return "";
}

char *
PQhostaddr(const PGconn *conn)
{
	if (!conn)
		return NULL;

	/* 失败返默认端口        */
	if (conn->connhost != NULL && conn->connip != NULL)
		return conn->connip;

	return "";
}

char *
PQport(const PGconn *conn)
{
	if (!conn)
		return NULL;

	if (conn->connhost != NULL &&
		conn->connhost[conn->whichhost].port != NULL &&
		conn->connhost[conn->whichhost].port[0] != '\0')
		return conn->connhost[conn->whichhost].port;

	return "";
}

/*
 * 不再做任何事，但为 API 向后
 * 兼容保留。
                             */
char *
PQtty(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return "";
}

char *
PQoptions(const PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->pgoptions;
}

ConnStatusType
PQstatus(const PGconn *conn)
{
	if (!conn)
		return CONNECTION_BAD;
	return conn->status;
}

PGTransactionStatusType
PQtransactionStatus(const PGconn *conn)
{
	if (!conn || conn->status != CONNECTION_OK)
		return PQTRANS_UNKNOWN;
	if (conn->asyncStatus != PGASYNC_IDLE)
		return PQTRANS_ACTIVE;
	return conn->xactStatus;
}

const char *
PQparameterStatus(const PGconn *conn, const char *paramName)
{
	const pgParameterStatus *pstatus;

	if (!conn || !paramName)
		return NULL;
	for (pstatus = conn->pstatus; pstatus != NULL; pstatus = pstatus->next)
	{
		if (strcmp(pstatus->name, paramName) == 0)
			return pstatus->value;
	}
	return NULL;
}

int
PQprotocolVersion(const PGconn *conn)
{
	if (!conn)
		return 0;
	if (conn->status == CONNECTION_BAD)
		return 0;
	return PG_PROTOCOL_MAJOR(conn->pversion);
}

int
PQserverVersion(const PGconn *conn)
{
	if (!conn)
		return 0;
	if (conn->status == CONNECTION_BAD)
		return 0;
	return conn->sversion;
}

char *
PQerrorMessage(const PGconn *conn)
{
	if (!conn)
		return libpq_gettext("connection pointer is NULL\n");

	/*
	 * errorMessage 缓冲可能因
	 * 先前溢出标 "broken"。
	 * 若如此重置以获得消息。
	                                                                                            */
	if (PQExpBufferBroken(&conn->errorMessage))
		return libpq_gettext("out of memory\n");

	return conn->errorMessage.data;
}

/*
 * Windows 上套接字为无符号，
 * INVALID_SOCKET 非 <0。此宏
 * 封装该测试。
                                                                                                             
                                                                                                            
                                                                                                            
                                                                                                            
*/
int
PQsocket(const PGconn *conn)
{
	if (!conn)
		return -1;
	return (conn->sock != PGINVALID_SOCKET) ? conn->sock : -1;
}

int
PQbackendPID(const PGconn *conn)
{
	if (!conn || conn->status != CONNECTION_OK)
		return 0;
	return conn->be_pid;
}

PGpipelineStatus
PQpipelineStatus(const PGconn *conn)
{
	if (!conn)
		return PQ_PIPELINE_OFF;

	return conn->pipelineStatus;
}

int
PQconnectionNeedsPassword(const PGconn *conn)
{
	char	   *password;

	if (!conn)
		return false;
	password = PQpass(conn);
	if (conn->password_needed &&
		(password == NULL || password[0] == '\0'))
		return true;
	else
		return false;
}

int
PQconnectionUsedPassword(const PGconn *conn)
{
	if (!conn)
		return false;
	if (conn->password_needed)
		return true;
	else
		return false;
}

int
PQconnectionUsedGSSAPI(const PGconn *conn)
{
	if (!conn)
		return false;
	if (conn->gssapi_used)
		return true;
	else
		return false;
}

int
PQclientEncoding(const PGconn *conn)
{
	if (!conn || conn->status != CONNECTION_OK)
		return -1;
	return conn->client_encoding;
}

int
PQsetClientEncoding(PGconn *conn, const char *encoding)
{
	char		qbuf[128];
	static const char query[] = "set client_encoding to '%s'";
	PGresult   *res;
	int			status;

	if (!conn || conn->status != CONNECTION_OK)
		return -1;

	if (!encoding)
		return -1;

	/* 从 locale 解析特殊 "auto" 值           */
	if (strcmp(encoding, "auto") == 0)
		encoding = pg_encoding_to_char(pg_get_encoding_from_locale(NULL, true));

	/* 检查查询缓冲溢出    */
	if (sizeof(qbuf) < (sizeof(query) + strlen(encoding)))
		return -1;

	/* OK 发查询         */
	sprintf(qbuf, query, encoding);
	res = PQexec(conn, qbuf);

	if (res == NULL)
		return -1;
	if (res->resultStatus != PGRES_COMMAND_OK)
		status = -1;
	else
	{
		/*
		 * 靠后端报参数值；
		 * 收 ParameterStatus 时改。
		                                     */
		status = 0;				/* 无更多选项  */
	}
	PQclear(res);
	return status;
}

PGVerbosity
PQsetErrorVerbosity(PGconn *conn, PGVerbosity verbosity)
{
	PGVerbosity old;

	if (!conn)
		return PQERRORS_DEFAULT;
	old = conn->verbosity;
	conn->verbosity = verbosity;
	return old;
}

PGContextVisibility
PQsetErrorContextVisibility(PGconn *conn, PGContextVisibility show_context)
{
	PGContextVisibility old;

	if (!conn)
		return PQSHOW_CONTEXT_ERRORS;
	old = conn->show_context;
	conn->show_context = show_context;
	return old;
}

PQnoticeReceiver
PQsetNoticeReceiver(PGconn *conn, PQnoticeReceiver proc, void *arg)
{
	PQnoticeReceiver old;

	if (conn == NULL)
		return NULL;

	old = conn->noticeHooks.noticeRec;
	if (proc)
	{
		conn->noticeHooks.noticeRec = proc;
		conn->noticeHooks.noticeRecArg = arg;
	}
	return old;
}

PQnoticeProcessor
PQsetNoticeProcessor(PGconn *conn, PQnoticeProcessor proc, void *arg)
{
	PQnoticeProcessor old;

	if (conn == NULL)
		return NULL;

	old = conn->noticeHooks.noticeProc;
	if (proc)
	{
		conn->noticeHooks.noticeProc = proc;
		conn->noticeHooks.noticeProcArg = arg;
	}
	return old;
}

/*
 * 默认通知接收器仅取标准通知
 * 文本并传给处理器。
                                                                                     
                                                                                   
*/
static void
defaultNoticeReceiver(void *arg, const PGresult *res)
{
	(void) arg;					/* 未用   */
	if (res->noticeHooks.noticeProc != NULL)
		res->noticeHooks.noticeProc(res->noticeHooks.noticeProcArg,
									PQresultErrorMessage(res));
}

/*
 * 默认通知处理器仅打印
 * 消息到 stderr。
                                                                                             
                                                                                            
*/
static void
defaultNoticeProcessor(void *arg, const char *message)
{
	(void) arg;					/* 未用   */
	/* 暂不做任何事                                                 */
	fprintf(stderr, "%s", message);
}

/*
 * 匹配则返下一词元指针，
 * 不匹配则返 NULL。
                      */
static char *
pwdfMatchesString(char *buf, const char *token)
{
	char	   *tbuf;
	const char *ttok;
	bool		bslash = false;

	if (buf == NULL || token == NULL)
		return NULL;
	tbuf = buf;
	ttok = token;
	if (tbuf[0] == '*' && tbuf[1] == ':')
		return tbuf + 2;
	while (*tbuf != 0)
	{
		if (*tbuf == '\\' && !bslash)
		{
			tbuf++;
			bslash = true;
		}
		if (*tbuf == ':' && *ttok == 0 && !bslash)
			return tbuf + 1;
		bslash = false;
		if (*ttok == 0)
			return NULL;
		if (*tbuf == *ttok)
		{
			tbuf++;
			ttok++;
		}
		else
			return NULL;
	}
	return NULL;
}

/* 前进到非空格/制表                                        */
static char *
passwordFromFile(const char *hostname, const char *port, const char *dbname,
				 const char *username, const char *pgpassfile)
{
	FILE	   *fp;
	struct stat stat_buf;
	PQExpBufferData buf;

	if (dbname == NULL || dbname[0] == '\0')
		return NULL;

	if (username == NULL || username[0] == '\0')
		return NULL;

	/* 比较词元                                                     */
	if (hostname == NULL || hostname[0] == '\0')
		hostname = DefaultHost;
	else if (is_unixsock_path(hostname))

		/*
		 * 我们或许应该使用 canonicalize_path()，但那样       
		 * 就得把 path.c 引入 libpq，似乎不值得。    
		 */
		if (strcmp(hostname, DEFAULT_PGSOCKET_DIR) == 0)
			hostname = DefaultHost;

	if (port == NULL || port[0] == '\0')
		port = DEF_PGPORT_STR;

	/* 返有意义错误消息                      */
	if (stat(pgpassfile, &stat_buf) != 0)
		return NULL;

#ifndef WIN32
	if (!S_ISREG(stat_buf.st_mode))
	{
		fprintf(stderr,
				libpq_gettext("WARNING: password file \"%s\" is not a plain file\n"),
				pgpassfile);
		return NULL;
	}

	/* 用到用户主目录                                       */
	if (stat_buf.st_mode & (S_IRWXG | S_IRWXO))
	{
		fprintf(stderr,
				libpq_gettext("WARNING: password file \"%s\" has group or world access; permissions should be u=rw (0600) or less\n"),
				pgpassfile);
		return NULL;
	}
#else

	/*
	 * Win32 目录受保护故不
	 * 检查文件权限。
	                         */
#endif

	fp = fopen(pgpassfile, "r");
	if (fp == NULL)
		return NULL;

	/* 获取文件状态                                                 */
	initPQExpBuffer(&buf);

	while (!feof(fp) && !ferror(fp))
	{
		/* 非用户或全局可写则警告                           */
		if (!enlargePQExpBuffer(&buf, 128))
			break;

		/* 非普通文件或无读权限则警告              */
		if (fgets(buf.data + buf.len, buf.maxlen - buf.len, fp) == NULL)
			break;
		buf.len += strlen(buf.data + buf.len);

		/* 文件空忽略                                             */
		if (!(buf.len > 0 && buf.data[buf.len - 1] == '\n') && !feof(fp))
			continue;

		/* 跳过注释    */
		if (buf.data[0] != '#')
		{
			char	   *t = buf.data;
			int			len;

			/* 处理行继续吃换行                   */
			len = pg_strip_crlf(t);

			if (len > 0 &&
				(t = pwdfMatchesString(t, hostname)) != NULL &&
				(t = pwdfMatchesString(t, port)) != NULL &&
				(t = pwdfMatchesString(t, dbname)) != NULL &&
				(t = pwdfMatchesString(t, username)) != NULL)
			{
				/* 找到匹配   */
				char	   *ret,
						   *p1,
						   *p2;

				ret = strdup(t);

				fclose(fp);
				explicit_bzero(buf.data, buf.maxlen);
				termPQExpBuffer(&buf);

				if (!ret)
				{
					/* 逐字符匹配                                     */
					return NULL;
				}

				/* 反转义密码。  */
				for (p1 = p2 = ret; *p1 != ':' && *p1 != '\0'; ++p1, ++p2)
				{
					if (*p1 == '\\' && p1[1] != '\0')
						++p1;
					*p2 = *p1;
				}
				*p2 = '\0';

				return ret;
			}
		}

		/* 现有匹配                                     */
		buf.len = 0;
	}

	fclose(fp);
	explicit_bzero(buf.data, buf.maxlen);
	termPQExpBuffer(&buf);
	return NULL;
}


/*
 *	若因密码错连败，应提及
 *	是否从密码文件获密码。
                                         */
static void
pgpassfileWarning(PGconn *conn)
{
	/* 若用 pgpass 行提供密码标                          */
	/* 仅 >=9.0 服务器可用      */
	if (conn->password_needed &&
		conn->connhost[conn->whichhost].password != NULL &&
		conn->result)
	{
		const char *sqlstate = PQresultErrorField(conn->result,
												  PG_DIAG_SQLSTATE);

		if (sqlstate && strcmp(sqlstate, ERRCODE_INVALID_PASSWORD) == 0)
			libpq_append_conn_error(conn, "password retrieved from file \"%s\"",
									conn->pgpassfile);
	}
}

/*
 * 检查 SSL 协议值是否有效。
 * 与 SSL 实现无关。
                                                                                                                                   
*/
static bool
sslVerifyProtocolVersion(const char *version)
{
	/*
	 * 空串和 NULL 视有效，
	 * 等价不设。
	                                                          */
	if (!version || strlen(version) == 0)
		return true;

	if (pg_strcasecmp(version, "TLSv1") == 0 ||
		pg_strcasecmp(version, "TLSv1.1") == 0 ||
		pg_strcasecmp(version, "TLSv1.2") == 0 ||
		pg_strcasecmp(version, "TLSv1.3") == 0)
		return true;

	/* 其他皆错           */
	return false;
}


/*
 * 确保 SSL 协议范围正确。
 * 与实现无关。
                                                                                                                                                                        
*/
static bool
sslVerifyProtocolRange(const char *min, const char *max)
{
	Assert(sslVerifyProtocolVersion(min) &&
		   sslVerifyProtocolVersion(max));

	/* 未设视为有效                                           */
	if (min == NULL || max == NULL || strlen(min) == 0 || strlen(max) == 0)
		return true;

	/*
	 * 最低版是我们接受的最低版
	 * 则所有选项有效。
	                                   */
	if (pg_strcasecmp(min, "TLSv1") == 0)
		return true;

	/*
	 * 下限有效非 TLSv1，故
	 * TLSv1 作上限无效。
	                                          */
	if (pg_strcasecmp(max, "TLSv1") == 0)
		return false;

	/*
	 * 知有 TLSv1.1-1.3 混合。
	 * 确保 min <= max。
	                         */
	if (pg_strcasecmp(min, max) > 0)
		return false;

	return true;
}


/*
 * 获取用户主目录返存入缓冲。
 *
 * Unix 上返用户主目录。
 * Windows 上获 "My Documents"
 * 下子目录 "postgresql"。
 *
 * 注意需对抗返回值超限。
 * 缓冲太小则略截断（仍可
 * 接受），始终以 \0 终。
                                                                           
                                                                          
                                                                          
                                                                          
                                                                         
                                                                         
                                                                         
*/
bool
pqGetHomeDirectory(char *buf, int bufsize)
{
#ifndef WIN32
	const char *home;

	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return pg_get_user_home_dir(geteuid(), buf, bufsize);
	strlcpy(buf, home, bufsize);
	return true;
#else
	char		tmppath[MAX_PATH];

	ZeroMemory(tmppath, sizeof(tmppath));
	if (SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, tmppath) != S_OK)
		return false;
	snprintf(buf, bufsize, "%s/postgresql", tmppath);
	return true;
#endif
}

/*
 * 保持 API 一致，即使未启用
 * 线程安全也提供锁定桩。
                                                     
                                                    
                                                    
                                                   
                                                   
*/

static void
default_threadlock(int acquire)
{
#ifdef ENABLE_THREAD_SAFETY
	static pthread_mutex_t singlethread_lock = PTHREAD_MUTEX_INITIALIZER;

	if (acquire)
	{
		if (pthread_mutex_lock(&singlethread_lock))
			Assert(false);
	}
	else
	{
		if (pthread_mutex_unlock(&singlethread_lock))
			Assert(false);
	}
#endif
}

pgthreadlock_t
PQregisterThreadLock(pgthreadlock_t newhandler)
{
	pgthreadlock_t prev = pg_g_threadlock;

	if (newhandler)
		pg_g_threadlock = newhandler;
	else
		pg_g_threadlock = default_threadlock;

	return prev;
}
