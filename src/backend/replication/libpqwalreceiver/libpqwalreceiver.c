/*-------------------------------------------------------------------------
 *
 * libpqwalreceiver.c
 *
 * 本文件包含 walreceiver 中与 libpq 相关的部分。它  
 * 以动态模块加载，以免主库二进制链接到此              
 * 库。
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/libpqwalreceiver/libpqwalreceiver.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/time.h>

#include "access/xlog.h"
#include "catalog/pg_type.h"
#include "common/connect.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "pqexpbuffer.h"
#include "replication/walreceiver.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

struct WalReceiverConn
{
	/* 当前与主库的连接（若有）      */
	PGconn	   *streamConn;
	/* 用于记住该连接是逻辑的还是物理的          */
	bool		logical;
	/* 当前所读记录的缓冲区    */
	char	   *recvBuf;
};

/* 接口函数的原型              */
static WalReceiverConn *libpqrcv_connect(const char *conninfo,
										 bool logical, bool must_use_password,
										 const char *appname, char **err);
static void libpqrcv_check_conninfo(const char *conninfo,
									bool must_use_password);
static char *libpqrcv_get_conninfo(WalReceiverConn *conn);
static void libpqrcv_get_senderinfo(WalReceiverConn *conn,
									char **sender_host, int *sender_port);
static char *libpqrcv_identify_system(WalReceiverConn *conn,
									  TimeLineID *primary_tli);
static int	libpqrcv_server_version(WalReceiverConn *conn);
static void libpqrcv_readtimelinehistoryfile(WalReceiverConn *conn,
											 TimeLineID tli, char **filename,
											 char **content, int *len);
static bool libpqrcv_startstreaming(WalReceiverConn *conn,
									const WalRcvStreamOptions *options);
static void libpqrcv_endstreaming(WalReceiverConn *conn,
								  TimeLineID *next_tli);
static int	libpqrcv_receive(WalReceiverConn *conn, char **buffer,
							 pgsocket *wait_fd);
static void libpqrcv_send(WalReceiverConn *conn, const char *buffer,
						  int nbytes);
static char *libpqrcv_create_slot(WalReceiverConn *conn,
								  const char *slotname,
								  bool temporary,
								  bool two_phase,
								  CRSSnapshotAction snapshot_action,
								  XLogRecPtr *lsn);
static pid_t libpqrcv_get_backend_pid(WalReceiverConn *conn);
static WalRcvExecResult *libpqrcv_exec(WalReceiverConn *conn,
									   const char *query,
									   const int nRetTypes,
									   const Oid *retTypes);
static void libpqrcv_disconnect(WalReceiverConn *conn);

static WalReceiverFunctionsType PQWalReceiverFunctions = {
	.walrcv_connect = libpqrcv_connect,
	.walrcv_check_conninfo = libpqrcv_check_conninfo,
	.walrcv_get_conninfo = libpqrcv_get_conninfo,
	.walrcv_get_senderinfo = libpqrcv_get_senderinfo,
	.walrcv_identify_system = libpqrcv_identify_system,
	.walrcv_server_version = libpqrcv_server_version,
	.walrcv_readtimelinehistoryfile = libpqrcv_readtimelinehistoryfile,
	.walrcv_startstreaming = libpqrcv_startstreaming,
	.walrcv_endstreaming = libpqrcv_endstreaming,
	.walrcv_receive = libpqrcv_receive,
	.walrcv_send = libpqrcv_send,
	.walrcv_create_slot = libpqrcv_create_slot,
	.walrcv_get_backend_pid = libpqrcv_get_backend_pid,
	.walrcv_exec = libpqrcv_exec,
	.walrcv_disconnect = libpqrcv_disconnect
};

/* 私有函数的原型            */
static PGresult *libpqrcv_PQexec(PGconn *streamConn, const char *query);
static PGresult *libpqrcv_PQgetResult(PGconn *streamConn);
static char *stringlist_to_identifierstr(PGconn *conn, List *strings);

/*
 * 模块初始化函数         
 */
void
_PG_init(void)
{
	if (WalReceiverFunctions != NULL)
		elog(ERROR, "libpqwalreceiver already loaded");
	WalReceiverFunctions = &PQWalReceiverFunctions;
}

/*
 * 建立到主库的连接以进行 XLOG 流式传输              
 *
 * 如果发生错误，本函数通常将返回 NULL 并把 *err          
 * 设置为 palloc 分配的错误消息。但如果 must_use_password   
 * 为真且连接使用密码失败，本函数将 ereport(ERROR)。          
 * 这样做是因为这种情况下，错误会带有细节和              
 * 提示，以便与系统的其他部分保持一致；而仅为覆盖这 
 * 一情况就添加机制把它们传回调用方并不值得            
 * 。  
 */
static WalReceiverConn *
libpqrcv_connect(const char *conninfo, bool logical, bool must_use_password,
				 const char *appname, char **err)
{
	WalReceiverConn *conn;
	PostgresPollingStatusType status;
	const char *keys[6];
	const char *vals[6];
	int			i = 0;

	/*
	 * 重新验证连接串。验证已经在 DDL 时执行过，但订阅 
	 * 的所有者可能已经改变。如果我们不使用正确的       
	 * must_use_password 重新检查，连接有可能从不同            
	 * 的来源获取密码，例如 PGPASSFILE 或 PGPASSWORD               
	 * 。        
	 */
	libpqrcv_check_conninfo(conninfo, must_use_password);

	/*
	 * 我们使用 expand_dbname 参数来处理连接串（或 URI），    
	 * 并传递一些额外的选项。 
	 */
	keys[i] = "dbname";
	vals[i] = conninfo;
	keys[++i] = "replication";
	vals[i] = logical ? "database" : "true";
	if (!logical)
	{
		/*
		 * 在复制模式下，服务器会忽略数据库名，但指定    
		 * "replication" 供 .pgpass 查找。      
		 */
		keys[++i] = "dbname";
		vals[i] = "replication";
	}
	keys[++i] = "fallback_application_name";
	vals[i] = appname;
	if (logical)
	{
		/* 告知发布者转换为我们的编码         */
		keys[++i] = "client_encoding";
		vals[i] = GetDatabaseEncodingName();

		/*
		 * 强制将各种 GUC 参数设为能确保发布者            
		 * 以无歧义的形式输出数据值的设置。                  
		 * （我们不想修改订阅者的 GUC 设置，               
		 * 会令在其中运行的用户自定义代码感到             
		 * 意外，例如触发器。）此处应做到与 pg_dump 一致   
		 * 。  
		 */
		keys[++i] = "options";
		vals[i] = "-c datestyle=ISO -c intervalstyle=postgres -c extra_float_digits=3";
	}
	keys[++i] = NULL;
	vals[i] = NULL;

	Assert(i < sizeof(keys));

	conn = palloc0(sizeof(WalReceiverConn));
	conn->streamConn = PQconnectStartParams(keys, vals,
											 /* expand_dbname = */ true);
	if (PQstatus(conn->streamConn) == CONNECTION_BAD)
		goto bad_connection_errmsg;

	/*
	 * 轮询连接直到获得 OK 或 FAILED 状态。  
	 *
	 * 按 PQconnectPoll 规范，首先等待套接字变为可写。   
	 */
	status = PGRES_POLLING_WRITING;
	do
	{
		int			io_flag;
		int			rc;

		if (status == PGRES_POLLING_READING)
			io_flag = WL_SOCKET_READABLE;
#ifdef WIN32
		/* 在等待连接建立时 Windows 需要不同的检测           */
		else if (PQstatus(conn->streamConn) == CONNECTION_STARTED)
			io_flag = WL_SOCKET_CONNECTED;
#endif
		else
			io_flag = WL_SOCKET_WRITEABLE;

		rc = WaitLatchOrSocket(MyLatch,
							   WL_EXIT_ON_PM_DEATH | WL_LATCH_SET | io_flag,
							   PQsocket(conn->streamConn),
							   0,
							   WAIT_EVENT_LIBPQWALRECEIVER_CONNECT);

		/* 中断？    */
		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			ProcessWalRcvInterrupts();
		}

		/* 如果套接字就绪，则推进 libpq 状态机   */
		if (rc & io_flag)
			status = PQconnectPoll(conn->streamConn);
	} while (status != PGRES_POLLING_OK && status != PGRES_POLLING_FAILED);

	if (PQstatus(conn->streamConn) != CONNECTION_OK)
		goto bad_connection_errmsg;

	if (must_use_password && !PQconnectionUsedPassword(conn->streamConn))
	{
		PQfinish(conn->streamConn);
		pfree(conn);

		ereport(ERROR,
				(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
				 errmsg("password is required"),
				 errdetail("Non-superuser cannot connect if the server does not request a password."),
				 errhint("Target server's authentication method must be changed, or set password_required=false in the subscription parameters.")));
	}

	if (logical)
	{
		PGresult   *res;

		res = libpqrcv_PQexec(conn->streamConn,
							  ALWAYS_SECURE_SEARCH_PATH_SQL);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			PQclear(res);
			*err = psprintf(_("could not clear search path: %s"),
							pchomp(PQerrorMessage(conn->streamConn)));
			goto bad_connection;
		}
		PQclear(res);
	}

	conn->logical = logical;

	return conn;

	/* 错误路径，用 libpq 的错误消息*/
bad_connection_errmsg:
	*err = pchomp(PQerrorMessage(conn->streamConn));

	/* 错误路径，错误已设置*/
bad_connection:
	PQfinish(conn->streamConn);
	pfree(conn);
	return NULL;
}

/*
 * 验证连接信息串，并判断它是否可能导致尝试访问   
 * 本地文件系统。                   
 *
 * 如果连接串无法被解析，本函数将抛出错误并且   
 * 不再返回。如果可以解析，则当该连接串指定了     
 * 密码时返回 true，否则返回 false。               
 */
static void
libpqrcv_check_conninfo(const char *conninfo, bool must_use_password)
{
	PQconninfoOption *opts = NULL;
	PQconninfoOption *opt;
	char	   *err = NULL;

	opts = PQconninfoParse(conninfo, &err);
	if (opts == NULL)
	{
		/* 错误串由 malloc 分配，所以必须显式释放它    */
		char	   *errcopy = err ? pstrdup(err) : "out of memory";

		PQfreemem(err);
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid connection string syntax: %s", errcopy)));
	}

	if (must_use_password)
	{
		bool		uses_password = false;

		for (opt = opts; opt->keyword != NULL; ++opt)
		{
			/* 忽略不存在的连接选项。               */
			if (opt->val == NULL)
				continue;

			if (strcmp(opt->keyword, "password") == 0 && opt->val[0] != '\0')
			{
				uses_password = true;
				break;
			}
		}

		if (!uses_password)
		{
			/* 由 malloc 分配，必须显式释放  */
			PQconninfoFree(opts);

			ereport(ERROR,
					(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
					 errmsg("password is required"),
					 errdetail("Non-superusers must provide a password in the connection string.")));
		}
	}

	PQconninfoFree(opts);
}

/*
 * 返回供用户查看的 conninfo 串。安全敏感字段               
 * 均被混淆。
 */
static char *
libpqrcv_get_conninfo(WalReceiverConn *conn)
{
	PQconninfoOption *conn_opts;
	PQconninfoOption *conn_opt;
	PQExpBufferData buf;
	char	   *retval;

	Assert(conn->streamConn != NULL);

	initPQExpBuffer(&buf);
	conn_opts = PQconninfo(conn->streamConn);

	if (conn_opts == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not parse connection string: %s",
						_("out of memory"))));

	/* 由各部分构建干净的连接串        */
	for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
	{
		bool		obfuscate;

		/* 跳过 debug 与空选项    */
		if (strchr(conn_opt->dispchar, 'D') ||
			conn_opt->val == NULL ||
			conn_opt->val[0] == '\0')
			continue;

		/* 混淆安全敏感的选项          */
		obfuscate = strchr(conn_opt->dispchar, '*') != NULL;

		appendPQExpBuffer(&buf, "%s%s=%s",
						  buf.len == 0 ? "" : " ",
						  conn_opt->keyword,
						  obfuscate ? "********" : conn_opt->val);
	}

	PQconninfoFree(conn_opts);

	retval = PQExpBufferDataBroken(buf) ? NULL : pstrdup(buf.data);
	termPQExpBuffer(&buf);
	return retval;
}

/*
 * 提供本 WAL 接收进程所连接到的发送方的信息。   
 */
static void
libpqrcv_get_senderinfo(WalReceiverConn *conn, char **sender_host,
						int *sender_port)
{
	char	   *ret = NULL;

	*sender_host = NULL;
	*sender_port = 0;

	Assert(conn->streamConn != NULL);

	ret = PQhost(conn->streamConn);
	if (ret && strlen(ret) != 0)
		*sender_host = pstrdup(ret);

	ret = PQport(conn->streamConn);
	if (ret && strlen(ret) != 0)
		*sender_port = atoi(ret);
}

/*
 * 检查主库的系统标识符是否与我们的一致，并获取主库  
 * 当前的时间线 ID。   
 */
static char *
libpqrcv_identify_system(WalReceiverConn *conn, TimeLineID *primary_tli)
{
	PGresult   *res;
	char	   *primary_sysid;

	/*
	 * 以 DataRow 消息的形式从主库获取系统标识符与           
	 * 时间线 ID。
	 */
	res = libpqrcv_PQexec(conn->streamConn, "IDENTIFY_SYSTEM");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not receive database system identifier and timeline ID from "
						"the primary server: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
	}
	/*
	 * IDENTIFY_SYSTEM 在 9.3 及更早版本返回 3 列，从 9.4         
	 * 起为 4 列。 
	 */
	if (PQnfields(res) < 3 || PQntuples(res) != 1)
	{
		int			ntuples = PQntuples(res);
		int			nfields = PQnfields(res);

		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid response from primary server"),
				 errdetail("Could not identify system: got %d rows and %d fields, expected %d rows and %d or more fields.",
						   ntuples, nfields, 1, 3)));
	}
	primary_sysid = pstrdup(PQgetvalue(res, 0, 0));
	*primary_tli = pg_strtoint32(PQgetvalue(res, 0, 1));
	PQclear(res);

	return primary_sysid;
}

/*
 * libpq 的薄封装，用于获取服务器版本。
 */
static int
libpqrcv_server_version(WalReceiverConn *conn)
{
	return PQserverVersion(conn->streamConn);
}

/*
 * 按给定的流选项开始流式传输 WAL 数据。 
 *
 * 如果成功切换到 copy-both 模式则返回 true。False     
 * 意味着服务器已接收该命令并成功执行，但没有切     
 * 换到 copy 模式。这意味着在所请求的时间线与起     
 * 始点上没有 WAL，因为服务器已切换到另一条时间     
 * 线，切换点在所请求的起始点或其之前。失败时将     
 * 抛出 ERROR。 
 */
static bool
libpqrcv_startstreaming(WalReceiverConn *conn,
						const WalRcvStreamOptions *options)
{
	StringInfoData cmd;
	PGresult   *res;

	Assert(options->logical == conn->logical);
	Assert(options->slotname || !options->logical);

	initStringInfo(&cmd);

	/* 构建该命令。 */
	appendStringInfoString(&cmd, "START_REPLICATION");
	if (options->slotname != NULL)
		appendStringInfo(&cmd, " SLOT \"%s\"",
						 options->slotname);

	if (options->logical)
		appendStringInfoString(&cmd, " LOGICAL");

	appendStringInfo(&cmd, " %X/%X", LSN_FORMAT_ARGS(options->startpoint));

	/*
	 * 额外选项会有所不同，取决于我们进行的是逻辑复制
	 * 还是物理复制。   
	 */
	if (options->logical)
	{
		char	   *pubnames_str;
		List	   *pubnames;
		char	   *pubnames_literal;

		appendStringInfoString(&cmd, " (");

		appendStringInfo(&cmd, "proto_version '%u'",
						 options->proto.logical.proto_version);

		if (options->proto.logical.streaming_str)
			appendStringInfo(&cmd, ", streaming '%s'",
							 options->proto.logical.streaming_str);

		if (options->proto.logical.twophase &&
			PQserverVersion(conn->streamConn) >= 150000)
			appendStringInfoString(&cmd, ", two_phase 'on'");

		if (options->proto.logical.origin &&
			PQserverVersion(conn->streamConn) >= 160000)
			appendStringInfo(&cmd, ", origin '%s'",
							 options->proto.logical.origin);

		pubnames = options->proto.logical.publication_names;
		pubnames_str = stringlist_to_identifierstr(conn->streamConn, pubnames);
		if (!pubnames_str)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),	/* 可能猜测 */
					 errmsg("could not start WAL streaming: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));
		pubnames_literal = PQescapeLiteral(conn->streamConn, pubnames_str,
										   strlen(pubnames_str));
		if (!pubnames_literal)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),	/* 可能猜测 */
					 errmsg("could not start WAL streaming: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));
		appendStringInfo(&cmd, ", publication_names %s", pubnames_literal);
		PQfreemem(pubnames_literal);
		pfree(pubnames_str);

		if (options->proto.logical.binary &&
			PQserverVersion(conn->streamConn) >= 140000)
			appendStringInfoString(&cmd, ", binary 'true'");

		appendStringInfoChar(&cmd, ')');
	}
	else
		appendStringInfo(&cmd, " TIMELINE %u",
						 options->proto.physical.startpointTLI);

	/* 开始流传输  */
	res = libpqrcv_PQexec(conn->streamConn, cmd.data);
	pfree(cmd.data);

	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return false;
	}
	else if (PQresultStatus(res) != PGRES_COPY_BOTH)
	{
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not start WAL streaming: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
	}
	PQclear(res);
	return true;
}

/*
 * 停止流式传输 WAL 数据。将下一条时间线的 ID 放入      
 * *next_tli 中，按服务器报告；未报则 0。  
 */
static void
libpqrcv_endstreaming(WalReceiverConn *conn, TimeLineID *next_tli)
{
	PGresult   *res;

	/*
	 * 发送 copy-end 消息。如同 libpqrcv_PQexec，理论上            
	 * 可能阻塞，风险甚小。  
	 */
	if (PQputCopyEnd(conn->streamConn, NULL) <= 0 ||
		PQflush(conn->streamConn))
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send end-of-streaming message to primary: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));

	*next_tli = 0;

	/*
	 * COPY 结束后，我们应当收到一个结果集，指明下一    
	 * 条时间线的 ID；服务器关闭时只有 CommandComplete     
	 * 。  
	 *
	 * 如果我们尚未从后端收到 CopyDone，则 PGRES_COPY_OUT        
	 * 也可能出现，即我们在流中途中止复制。  
	 */
	res = libpqrcv_PQgetResult(conn->streamConn);
	if (PQresultStatus(res) == PGRES_TUPLES_OK)
	{
		/*
		 * 读取下一条时间线的 ID。服务器也发送时间线的  
		 * 起始点，但它被忽略。    
		 */
		if (PQnfields(res) < 2 || PQntuples(res) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected result set after end-of-streaming")));
		*next_tli = pg_strtoint32(PQgetvalue(res, 0, 0));
		PQclear(res);

		/* 结果集之后应当跟随 CommandComplete          */
		res = libpqrcv_PQgetResult(conn->streamConn);
	}
	else if (PQresultStatus(res) == PGRES_COPY_OUT)
	{
		PQclear(res);

		/* 结束复制 */
		if (PQendcopy(conn->streamConn))
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("error while shutting down streaming COPY: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));

		/* CommandComplete 紧随其后  */
		res = libpqrcv_PQgetResult(conn->streamConn);
	}

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("error reading result of streaming command: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
	PQclear(res);

	/* 验证没有更多的结果。        */
	res = libpqrcv_PQgetResult(conn->streamConn);
	if (res != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("unexpected result after CommandComplete: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
}

/*
 * 从主库获取 'tli' 的时间线历史文件。      
 */
static void
libpqrcv_readtimelinehistoryfile(WalReceiverConn *conn,
								 TimeLineID tli, char **filename,
								 char **content, int *len)
{
	PGresult   *res;
	char		cmd[64];

	Assert(!conn->logical);

	/*
	 * 请求主库发送给定时间线的历史文件。                  
	 */
	snprintf(cmd, sizeof(cmd), "TIMELINE_HISTORY %u", tli);
	res = libpqrcv_PQexec(conn->streamConn, cmd);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not receive timeline history file from "
						"the primary server: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
	}
	if (PQnfields(res) != 2 || PQntuples(res) != 1)
	{
		int			ntuples = PQntuples(res);
		int			nfields = PQnfields(res);

		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid response from primary server"),
				 errdetail("Expected 1 tuple with 2 fields, got %d tuples with %d fields.",
						   ntuples, nfields)));
	}
	*filename = pstrdup(PQgetvalue(res, 0, 0));

	*len = PQgetlength(res, 0, 1);
	*content = palloc(*len);
	memcpy(*content, PQgetvalue(res, 0, 1), *len);
	PQclear(res);
}

/*
 * 发送一个查询，并使用异步的 libpq 函数与套接字     
 * 就绪事件来等待结果。        
 *
 * 本函数仿照 libpqsrv_exec()，行为差异在于它会              
 * 调用 ProcessWalRcvInterrupts()。作为一项优化，它           
 * 跳过 try/catch，因错误均会终止进程。       
 *
 * 失败时可能返回 NULL，而非错误结果。       
 */
static PGresult *
libpqrcv_PQexec(PGconn *streamConn, const char *query)
{
	PGresult   *lastResult = NULL;

	/*
	 * PQexec() 会静默丢弃该连接上任何先前的查询结果。   
	 * 本函数不需要这一点，因为预期调用者                    
	 * （在所有情况下都是本库）会正确行事，而我们不必  
	 * 向后兼容旧的 libpq。                    
	 */

	/*
	 * 提交查询。由于我们未使用非阻塞模式，这理论上 
	 * 可能会阻塞。实际上，由于我们不会发送很长的查询 
	 * 串，风险看来可以忽略。  
	 */
	if (!PQsendQuery(streamConn, query))
		return NULL;

	for (;;)
	{
		/* 等待并收集下一个 PGresult。      */
		PGresult   *result;

		result = libpqrcv_PQgetResult(streamConn);
		if (result == NULL)
			break;				/* 查询已完成，或失败   */

		/*
		 * 模拟 PQexec() 的行为：当结果有多个时返回最后      
		 * 一个。我们只返回最后的错误消息即可。        
		 */
		PQclear(lastResult);
		lastResult = result;

		if (PQresultStatus(lastResult) == PGRES_COPY_IN ||
			PQresultStatus(lastResult) == PGRES_COPY_OUT ||
			PQresultStatus(lastResult) == PGRES_COPY_BOTH ||
			PQstatus(streamConn) == CONNECTION_BAD)
			break;
	}

	return lastResult;
}

/*
 * 执行与 PQgetResult() 等价的操作，但会留意中断。   
 */
static PGresult *
libpqrcv_PQgetResult(PGconn *streamConn)
{
	/*
	 * 收集数据，直到 PQgetResult 无需阻塞即可获得       
	 * 结果。
	 */
	while (PQisBusy(streamConn))
	{
		int			rc;

		/*
		 * 我们不需要把睡眠拆分成更小的增量，           
		 * 因为信号会中断我们，我们可以在此处处理
		 * 任何中断。 
		 */
		rc = WaitLatchOrSocket(MyLatch,
							   WL_EXIT_ON_PM_DEATH | WL_SOCKET_READABLE |
							   WL_LATCH_SET,
							   PQsocket(streamConn),
							   0,
							   WAIT_EVENT_LIBPQWALRECEIVER_RECEIVE);

		/* 中断？    */
		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			ProcessWalRcvInterrupts();
		}

		/* 消费套接字上任何可用的数据            */
		if (PQconsumeInput(streamConn) == 0)
		{
			/* 出错；返回 NULL */
			return NULL;
		}
	}

	/* 现在可以收集并返回下一个 PGresult   */
	return PQgetResult(streamConn);
}

/*
 * 断开与主库的连接（若有）。  
 */
static void
libpqrcv_disconnect(WalReceiverConn *conn)
{
	PQfinish(conn->streamConn);
	PQfreemem(conn->recvBuf);
	pfree(conn);
}

/*
 * 从 XLOG 流中接收一条可用的消息。
 *
 * 返回  
 *
 *	 如果收到了数据，则返回数据的长度。*buffer 会被设   
 *	 置为指向一个保存着所收消息的缓冲区。其有效期仅   
 *	 至下次 libpqrcv_* 调用。 
 *
 *	 如果没有数据立即可用，则返回 0，并将 *wait_fd 设       
 *	 置为一个套接字描述符，重试前可等待它。    
 *
 *	 -1 若服务器已结束 COPY。
 *
 * 出错 ereport。 
 */
static int
libpqrcv_receive(WalReceiverConn *conn, char **buffer,
				 pgsocket *wait_fd)
{
	int			rawlen;

	PQfreemem(conn->recvBuf);
	conn->recvBuf = NULL;

	/* 尝试接收一条 CopyData 消息*/
	rawlen = PQgetCopyData(conn->streamConn, &conn->recvBuf, 1);
	if (rawlen == 0)
	{
		/* 尝试消费一些数据 */
		if (PQconsumeInput(conn->streamConn) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not receive data from WAL stream: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));

		/* 已消费了一些输入，重试             */
		rawlen = PQgetCopyData(conn->streamConn, &conn->recvBuf, 1);
		if (rawlen == 0)
		{
			/* 套接字就绪时，告知调用方重试。      */
			*wait_fd = PQsocket(conn->streamConn);
			return 0;
		}
	}
	if (rawlen == -1)			/* 流传输结束或出错  */
	{
		PGresult   *res;

		res = libpqrcv_PQgetResult(conn->streamConn);
		if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			PQclear(res);

			/* 验证已没有更多的结果。      */
			res = libpqrcv_PQgetResult(conn->streamConn);
			if (res != NULL)
			{
				PQclear(res);

				/*
				 * 若对端以正常方式关闭了连接（否则我们    
				 * 应已看到错误或 PGRES_COPY_IN）就不必在此      
				 * 报错，交给调用者处理。  
				 */
				if (PQstatus(conn->streamConn) == CONNECTION_BAD)
					return -1;

				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("unexpected result after CommandComplete: %s",
								PQerrorMessage(conn->streamConn))));
			}

			return -1;
		}
		else if (PQresultStatus(res) == PGRES_COPY_IN)
		{
			PQclear(res);
			return -1;
		}
		else
		{
			PQclear(res);
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("could not receive data from WAL stream: %s",
							pchomp(PQerrorMessage(conn->streamConn)))));
		}
	}
	if (rawlen < -1)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not receive data from WAL stream: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));

	/* 返回收到的消息给调用者  */
	*buffer = conn->recvBuf;
	return rawlen;
}

/*
 * 发送消息到 XLOG 流。   
 *
 * 出错 ereport。 
 */
static void
libpqrcv_send(WalReceiverConn *conn, const char *buffer, int nbytes)
{
	if (PQputCopyData(conn->streamConn, buffer, nbytes) <= 0 ||
		PQflush(conn->streamConn))
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send data to WAL stream: %s",
						pchomp(PQerrorMessage(conn->streamConn)))));
}

/*
 * 创建新的复制槽。    
 * 对于逻辑槽返回所导出快照的名称，对于物理槽则    
 * 返回 NULL。
 */
static char *
libpqrcv_create_slot(WalReceiverConn *conn, const char *slotname,
					 bool temporary, bool two_phase, CRSSnapshotAction snapshot_action,
					 XLogRecPtr *lsn)
{
	PGresult   *res;
	StringInfoData cmd;
	char	   *snapshot;
	int			use_new_options_syntax;

	use_new_options_syntax = (PQserverVersion(conn->streamConn) >= 150000);

	initStringInfo(&cmd);

	appendStringInfo(&cmd, "CREATE_REPLICATION_SLOT \"%s\"", slotname);

	if (temporary)
		appendStringInfoString(&cmd, " TEMPORARY");

	if (conn->logical)
	{
		appendStringInfoString(&cmd, " LOGICAL pgoutput ");
		if (use_new_options_syntax)
			appendStringInfoChar(&cmd, '(');
		if (two_phase)
		{
			appendStringInfoString(&cmd, "TWO_PHASE");
			if (use_new_options_syntax)
				appendStringInfoString(&cmd, ", ");
			else
				appendStringInfoChar(&cmd, ' ');
		}

		if (use_new_options_syntax)
		{
			switch (snapshot_action)
			{
				case CRS_EXPORT_SNAPSHOT:
					appendStringInfoString(&cmd, "SNAPSHOT 'export'");
					break;
				case CRS_NOEXPORT_SNAPSHOT:
					appendStringInfoString(&cmd, "SNAPSHOT 'nothing'");
					break;
				case CRS_USE_SNAPSHOT:
					appendStringInfoString(&cmd, "SNAPSHOT 'use'");
					break;
			}
		}
		else
		{
			switch (snapshot_action)
			{
				case CRS_EXPORT_SNAPSHOT:
					appendStringInfoString(&cmd, "EXPORT_SNAPSHOT");
					break;
				case CRS_NOEXPORT_SNAPSHOT:
					appendStringInfoString(&cmd, "NOEXPORT_SNAPSHOT");
					break;
				case CRS_USE_SNAPSHOT:
					appendStringInfoString(&cmd, "USE_SNAPSHOT");
					break;
			}
		}

		if (use_new_options_syntax)
			appendStringInfoChar(&cmd, ')');
	}
	else
	{
		if (use_new_options_syntax)
			appendStringInfoString(&cmd, " PHYSICAL (RESERVE_WAL)");
		else
			appendStringInfoString(&cmd, " PHYSICAL RESERVE_WAL");
	}

	res = libpqrcv_PQexec(conn->streamConn, cmd.data);
	pfree(cmd.data);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not create replication slot \"%s\": %s",
						slotname, pchomp(PQerrorMessage(conn->streamConn)))));
	}

	if (lsn)
		*lsn = DatumGetLSN(DirectFunctionCall1Coll(pg_lsn_in, InvalidOid,
												   CStringGetDatum(PQgetvalue(res, 0, 1))));

	if (!PQgetisnull(res, 0, 2))
		snapshot = pstrdup(PQgetvalue(res, 0, 2));
	else
		snapshot = NULL;

	PQclear(res);

	return snapshot;
}

/*
 * 返回远端后端进程的 PID。   
 */
static pid_t
libpqrcv_get_backend_pid(WalReceiverConn *conn)
{
	return PQbackendPID(conn->streamConn);
}

/*
 * 元组查询结果转为 tuplestore。   
 */
static void
libpqrcv_processTuples(PGresult *pgres, WalRcvExecResult *walres,
					   const int nRetTypes, const Oid *retTypes)
{
	int			tupn;
	int			coln;
	int			nfields = PQnfields(pgres);
	HeapTuple	tuple;
	AttInMetadata *attinmeta;
	MemoryContext rowcontext;
	MemoryContext oldcontext;

	/* 确保得到了预期数量的字段。     */
	if (nfields != nRetTypes)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid query response"),
				 errdetail("Expected %d fields, got %d fields.",
						   nRetTypes, nfields)));

	walres->tuplestore = tuplestore_begin_heap(true, false, work_mem);

	/* 创建与预期结果相对应的元组描述符。       */
	walres->tupledesc = CreateTemplateTupleDesc(nRetTypes);
	for (coln = 0; coln < nRetTypes; coln++)
		TupleDescInitEntry(walres->tupledesc, (AttrNumber) coln + 1,
						   PQfname(pgres, coln), retTypes[coln], -1, 0);
	attinmeta = TupleDescGetAttInMetadata(walres->tupledesc);

	/* 若没有元组返回，此处再做更多便无意义。     */
	if (PQntuples(pgres) == 0)
		return;

	/* 创建用于本地分配的临时上下文。   */
	rowcontext = AllocSetContextCreate(CurrentMemoryContext,
									   "libpqrcv query result context",
									   ALLOCSET_DEFAULT_SIZES);

	/* 处理返回的行。  */
	for (tupn = 0; tupn < PQntuples(pgres); tupn++)
	{
		char	   *cstrs[MaxTupleAttributeNumber];

		ProcessWalRcvInterrupts();

		/* 在临时上下文中进行分配。     */
		oldcontext = MemoryContextSwitchTo(rowcontext);

		/*
		 * 用各列值的以空值结尾的字符串填充 cstrs。
		 */
		for (coln = 0; coln < nfields; coln++)
		{
			if (PQgetisnull(pgres, tupn, coln))
				cstrs[coln] = NULL;
			else
				cstrs[coln] = PQgetvalue(pgres, tupn, coln);
		}

		/* 把该行转换为元组，并将其加入 tuplestore*/
		tuple = BuildTupleFromCStrings(attinmeta, cstrs);
		tuplestore_puttuple(walres->tuplestore, tuple);

		/* 清理   */
		MemoryContextSwitchTo(oldcontext);
		MemoryContextReset(rowcontext);
	}

	MemoryContextDelete(rowcontext);
}

/*
 * 用于发送通用查询（和命令）的公共接口。   
 *
 * 本函数只能由已连接到数据库的进程调用。  
 */
static WalRcvExecResult *
libpqrcv_exec(WalReceiverConn *conn, const char *query,
			  const int nRetTypes, const Oid *retTypes)
{
	PGresult   *pgres = NULL;
	WalRcvExecResult *walres = palloc0(sizeof(WalRcvExecResult));
	char	   *diag_sqlstate;

	if (MyDatabaseId == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the query interface requires a database connection")));

	pgres = libpqrcv_PQexec(conn->streamConn, query);

	switch (PQresultStatus(pgres))
	{
		case PGRES_SINGLE_TUPLE:
		case PGRES_TUPLES_OK:
			walres->status = WALRCV_OK_TUPLES;
			libpqrcv_processTuples(pgres, walres, nRetTypes, retTypes);
			break;

		case PGRES_COPY_IN:
			walres->status = WALRCV_OK_COPY_IN;
			break;

		case PGRES_COPY_OUT:
			walres->status = WALRCV_OK_COPY_OUT;
			break;

		case PGRES_COPY_BOTH:
			walres->status = WALRCV_OK_COPY_BOTH;
			break;

		case PGRES_COMMAND_OK:
			walres->status = WALRCV_OK_COMMAND;
			break;

			/* 空查询被视为错误。      */
		case PGRES_EMPTY_QUERY:
			walres->status = WALRCV_ERROR;
			walres->err = _("empty query");
			break;

		case PGRES_PIPELINE_SYNC:
		case PGRES_PIPELINE_ABORTED:
			walres->status = WALRCV_ERROR;
			walres->err = _("unexpected pipeline mode");
			break;

		case PGRES_NONFATAL_ERROR:
		case PGRES_FATAL_ERROR:
		case PGRES_BAD_RESPONSE:
			walres->status = WALRCV_ERROR;
			walres->err = pchomp(PQerrorMessage(conn->streamConn));
			diag_sqlstate = PQresultErrorField(pgres, PG_DIAG_SQLSTATE);
			if (diag_sqlstate)
				walres->sqlstate = MAKE_SQLSTATE(diag_sqlstate[0],
												 diag_sqlstate[1],
												 diag_sqlstate[2],
												 diag_sqlstate[3],
												 diag_sqlstate[4]);
			break;
	}

	PQclear(pgres);

	return walres;
}

/*
 * 给定字符串 List，将其作为单个逗号分隔       
 * 字符串，按需引用标识符。  
 *
 * 这本质上是 SplitIdentifierString 的逆操作。    
 *
 * 调用者应当释放结果。    
 */
static char *
stringlist_to_identifierstr(PGconn *conn, List *strings)
{
	ListCell   *lc;
	StringInfoData res;
	bool		first = true;

	initStringInfo(&res);

	foreach(lc, strings)
	{
		char	   *val = strVal(lfirst(lc));
		char	   *val_escaped;

		if (first)
			first = false;
		else
			appendStringInfoChar(&res, ',');

		val_escaped = PQescapeIdentifier(conn, val, strlen(val));
		if (!val_escaped)
		{
			free(res.data);
			return NULL;
		}
		appendStringInfoString(&res, val_escaped);
		PQfreemem(val_escaped);
	}

	return res.data;
}
