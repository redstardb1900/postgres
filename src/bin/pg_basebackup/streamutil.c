/*-------------------------------------------------------------------------
 *
 * streamutil.c - pg_basebackup、pg_receivewal 和 pg_recvlogical 的工具函数
 *
 * 作者：Magnus Hagander <magnus@hagander.net>
 *
 * 部分版权 (c) 1996-2023, PostgreSQL 全球开发组
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/streamutil.c
 *-------------------------------------------------------------------------
                           */

#include "postgres_fe.h"

#include <sys/time.h>
#include <unistd.h>

#include "access/xlog_internal.h"
#include "common/connect.h"
#include "common/fe_memutils.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "common/string.h"
#include "datatype/timestamp.h"
#include "port/pg_bswap.h"
#include "pqexpbuffer.h"
#include "receivelog.h"
#include "streamutil.h"

#define ERRCODE_DUPLICATE_OBJECT  "42710"

int			WalSegSz;

static bool RetrieveDataDirCreatePerm(PGconn *conn);

/* SHOW 命令用于复制连接从版本 10 开始引入               */
#define MINIMUM_VERSION_FOR_SHOW_CMD 100000

/*
 * 组访问从版本 11 开始支持。
      */
#define MINIMUM_VERSION_FOR_GROUP_ACCESS 110000

const char *progname;
char	   *connection_string = NULL;
char	   *dbhost = NULL;
char	   *dbuser = NULL;
char	   *dbport = NULL;
char	   *dbname = NULL;
int			dbgetpassword = 0;	/* 0=auto, -1=never, 1=always */
static char *password = NULL;
PGconn	   *conn = NULL;

/*
 * Connect to the server. Returns a valid PGconn pointer if connected,
 * or NULL on non-permanent error. On permanent error, the function will
 * call exit(1) directly.
 */
PGconn *
GetConnection(void)
{
	PGconn	   *tmpconn;
	int			argcount = 7;	/* dbname, replication, fallback_app_name, host, user, port, password            */
	int			i;
	const char **keywords;
	const char **values;
	const char *tmpparam;
	bool		need_password;
	PQconninfoOption *conn_opts = NULL;
	PQconninfoOption *conn_opt;
	char	   *err_msg = NULL;

	/* pg_recvlogical uses dbname only; others use connection_string only. */
	Assert(dbname == NULL || connection_string == NULL);

	/*
	 * 合并以连接字符串、选项和默认值（dbname=replication、
	 * replication=true 等）形式提供的连接信息输入。
	 * 显式丢弃连接字符串中的任何 dbname 值；否则，
	 * PQconnectdbParams() 会将该值解释为连接字符串本身。
	                                            */
	i = 0;
	if (connection_string)
	{
		conn_opts = PQconninfoParse(connection_string, &err_msg);
		if (conn_opts == NULL)
			pg_fatal("%s", err_msg);

		for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
		{
			if (conn_opt->val != NULL && conn_opt->val[0] != '\0' &&
				strcmp(conn_opt->keyword, "dbname") != 0)
				argcount++;
		}

		keywords = pg_malloc0((argcount + 1) * sizeof(*keywords));
		values = pg_malloc0((argcount + 1) * sizeof(*values));

		for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
		{
			if (conn_opt->val != NULL && conn_opt->val[0] != '\0' &&
				strcmp(conn_opt->keyword, "dbname") != 0)
			{
				keywords[i] = conn_opt->keyword;
				values[i] = conn_opt->val;
				i++;
			}
		}
	}
	else
	{
		keywords = pg_malloc0((argcount + 1) * sizeof(*keywords));
		values = pg_malloc0((argcount + 1) * sizeof(*values));
	}

	keywords[i] = "dbname";
	values[i] = dbname == NULL ? "replication" : dbname;
	i++;
	keywords[i] = "replication";
	values[i] = dbname == NULL ? "true" : "database";
	i++;
	keywords[i] = "fallback_application_name";
	values[i] = progname;
	i++;

	if (dbhost)
	{
		keywords[i] = "host";
		values[i] = dbhost;
		i++;
	}
	if (dbuser)
	{
		keywords[i] = "user";
		values[i] = dbuser;
		i++;
	}
	if (dbport)
	{
		keywords[i] = "port";
		values[i] = dbport;
		i++;
	}

	/* 如果给定了 -W，强制提示密码，但仅第一次           */
	need_password = (dbgetpassword == 1 && !password);

	do
	{
		/* 如果适当，获取新密码    */
		if (need_password)
		{
			free(password);
			password = simple_prompt("Password: ", false);
			need_password = false;
		}

		/* 如果有密码，使用（或在后续连接中复用）密码   */
		if (password)
		{
			keywords[i] = "password";
			values[i] = password;
		}
		else
		{
			keywords[i] = NULL;
			values[i] = NULL;
		}

		tmpconn = PQconnectdbParams(keywords, values, true);

		/*
		 * 如果内存不足，甚至无法分配 PGconn 对象，PQconnectdbParams
		 * 返回 NULL，我们直接调用 exit(1)。
		          */
		if (!tmpconn)
			pg_fatal("could not connect to server");

		/* If we need a password and -w wasn't given, loop back and get one */
		if (PQstatus(tmpconn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(tmpconn) &&
			dbgetpassword != -1)
		{
			PQfinish(tmpconn);
			need_password = true;
		}
	}
	while (need_password);

	if (PQstatus(tmpconn) != CONNECTION_OK)
	{
		pg_log_error("%s", PQerrorMessage(tmpconn));
		PQfinish(tmpconn);
		free(values);
		free(keywords);
		PQconninfoFree(conn_opts);
		return NULL;
	}

	/* Connection ok! */
	free(values);
	free(keywords);
	PQconninfoFree(conn_opts);

	/*
	 * Set always-secure search path, so malicious users can't get control.
	 * The capacity to run normal SQL queries was added in PostgreSQL 10, so
	 * the search path cannot be changed (by us or attackers) on earlier
	 * versions.
	 */
	if (dbname != NULL && PQserverVersion(tmpconn) >= 100000)
	{
		PGresult   *res;

		res = PQexec(tmpconn, ALWAYS_SECURE_SEARCH_PATH_SQL);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			pg_log_error("could not clear search_path: %s",
						 PQerrorMessage(tmpconn));
			PQclear(res);
			PQfinish(tmpconn);
			exit(1);
		}
		PQclear(res);
	}

	/*
	 * 确保我们与所连接的服务器具有相同的 integer_datetimes
	 * （现在始终为 "on"）值。
	   */
	tmpparam = PQparameterStatus(tmpconn, "integer_datetimes");
	if (!tmpparam)
	{
		pg_log_error("could not determine server setting for integer_datetimes");
		PQfinish(tmpconn);
		exit(1);
	}

	if (strcmp(tmpparam, "on") != 0)
	{
		pg_log_error("integer_datetimes compile flag does not match server");
		PQfinish(tmpconn);
		exit(1);
	}

	/*
	 * 获取源数据目录模式，并使用它来构建 umask 以创建目录和文件。
	                           */
	if (!RetrieveDataDirCreatePerm(tmpconn))
	{
		PQfinish(tmpconn);
		exit(1);
	}

	return tmpconn;
}

/*
 * From version 10, explicitly set wal segment size using SHOW wal_segment_size
 * since ControlFile is not accessible here.
 */
bool
RetrieveWalSegSize(PGconn *conn)
{
	PGresult   *res;
	char		xlog_unit[3];
	int			xlog_val,
				multiplier = 1;

	/* 检查连接是否存在   */
	Assert(conn != NULL);

	/* for previous versions set the default xlog seg size */
	if (PQserverVersion(conn) < MINIMUM_VERSION_FOR_SHOW_CMD)
	{
		WalSegSz = DEFAULT_XLOG_SEG_SIZE;
		return true;
	}

	res = PQexec(conn, "SHOW wal_segment_size");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not send replication command \"%s\": %s",
					 "SHOW wal_segment_size", PQerrorMessage(conn));

		PQclear(res);
		return false;
	}
	if (PQntuples(res) != 1 || PQnfields(res) < 1)
	{
		pg_log_error("could not fetch WAL segment size: got %d rows and %d fields, expected %d rows and %d or more fields",
					 PQntuples(res), PQnfields(res), 1, 1);

		PQclear(res);
		return false;
	}

	/* 从结果中获取 xlog 值和单位      */
	if (sscanf(PQgetvalue(res, 0, 0), "%d%2s", &xlog_val, xlog_unit) != 2)
	{
		pg_log_error("WAL segment size could not be parsed");
		PQclear(res);
		return false;
	}

	PQclear(res);

	/* 根据单位设置乘数以将 xlog_val 转换为字节       */
	if (strcmp(xlog_unit, "MB") == 0)
		multiplier = 1024 * 1024;
	else if (strcmp(xlog_unit, "GB") == 0)
		multiplier = 1024 * 1024 * 1024;

	/* 转换并设置 WalSegSz */
	WalSegSz = xlog_val * multiplier;

	if (!IsValidWalSegSize(WalSegSz))
	{
		pg_log_error(ngettext("WAL segment size must be a power of two between 1 MB and 1 GB, but the remote server reported a value of %d byte",
							  "WAL segment size must be a power of two between 1 MB and 1 GB, but the remote server reported a value of %d bytes",
							  WalSegSz),
					 WalSegSz);
		return false;
	}

	return true;
}

/*
 * RetrieveDataDirCreatePerm
 *
 * 此函数用于确定服务器 PG 数据目录的权限，并基于此设置
 * 我们创建的目录和文件的权限。
 *
 * PG11 添加了对数据目录设置（可选的）组读取/执行权限的支持。
 * 在 PG11 之前，只有所有者才能拥有数据目录的权限。
                                                                                  */
static bool
RetrieveDataDirCreatePerm(PGconn *conn)
{
	PGresult   *res;
	int			data_directory_mode;

	/* 检查连接是否存在   */
	Assert(conn != NULL);

	/* 对于以前的版本，保持默认的组访问     */
	if (PQserverVersion(conn) < MINIMUM_VERSION_FOR_GROUP_ACCESS)
		return true;

	res = PQexec(conn, "SHOW data_directory_mode");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not send replication command \"%s\": %s",
					 "SHOW data_directory_mode", PQerrorMessage(conn));

		PQclear(res);
		return false;
	}
	if (PQntuples(res) != 1 || PQnfields(res) < 1)
	{
		pg_log_error("could not fetch group access flag: got %d rows and %d fields, expected %d rows and %d or more fields",
					 PQntuples(res), PQnfields(res), 1, 1);

		PQclear(res);
		return false;
	}

	if (sscanf(PQgetvalue(res, 0, 0), "%o", &data_directory_mode) != 1)
	{
		pg_log_error("group access flag could not be parsed: %s",
					 PQgetvalue(res, 0, 0));

		PQclear(res);
		return false;
	}

	SetDataDirectoryCreatePerm(data_directory_mode);

	PQclear(res);
	return true;
}

/*
 * 通过给定的连接运行 IDENTIFY_SYSTEM，并根据请求返回一些结果信息：
 * - 系统标识符
 * - 当前时间线 ID
 * - 起始 LSN 位置
 * - 数据库名称（9.4 之前的服务器中为 NULL）
                  */
bool
RunIdentifySystem(PGconn *conn, char **sysid, TimeLineID *starttli,
				  XLogRecPtr *startpos, char **db_name)
{
	PGresult   *res;
	uint32		hi,
				lo;

	/* 检查连接是否存在   */
	Assert(conn != NULL);

	res = PQexec(conn, "IDENTIFY_SYSTEM");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not send replication command \"%s\": %s",
					 "IDENTIFY_SYSTEM", PQerrorMessage(conn));

		PQclear(res);
		return false;
	}
	if (PQntuples(res) != 1 || PQnfields(res) < 3)
	{
		pg_log_error("could not identify system: got %d rows and %d fields, expected %d rows and %d or more fields",
					 PQntuples(res), PQnfields(res), 1, 3);

		PQclear(res);
		return false;
	}

	/* 获取系统标识符 */
	if (sysid != NULL)
		*sysid = pg_strdup(PQgetvalue(res, 0, 0));

	/* 获取开始流传输的时间线 ID    */
	if (starttli != NULL)
		*starttli = atoi(PQgetvalue(res, 0, 1));

	/* Get LSN start position if necessary */
	if (startpos != NULL)
	{
		if (sscanf(PQgetvalue(res, 0, 2), "%X/%X", &hi, &lo) != 2)
		{
			pg_log_error("could not parse write-ahead log location \"%s\"",
						 PQgetvalue(res, 0, 2));

			PQclear(res);
			return false;
		}
		*startpos = ((uint64) hi) << 32 | lo;
	}

	/* 获取数据库名称，仅在 9.4 及更新版本中可用 */
	if (db_name != NULL)
	{
		*db_name = NULL;
		if (PQserverVersion(conn) >= 90400)
		{
			if (PQnfields(res) < 4)
			{
				pg_log_error("could not identify system: got %d rows and %d fields, expected %d rows and %d or more fields",
							 PQntuples(res), PQnfields(res), 1, 4);

				PQclear(res);
				return false;
			}
			if (!PQgetisnull(res, 0, 3))
				*db_name = pg_strdup(PQgetvalue(res, 0, 3));
		}
	}

	PQclear(res);
	return true;
}

/*
 * 通过给定的连接运行 READ_REPLICATION_SLOT，并根据请求为此槽返回
 * 一些结果信息：
 * - 起始 LSN 位置，如果未知则为 InvalidXLogRecPtr。
 * - 当前时间线 ID，如果未知则为 0。
 * 失败时返回 false，成功时返回 true。
   */
bool
GetSlotInformation(PGconn *conn, const char *slot_name,
				   XLogRecPtr *restart_lsn, TimeLineID *restart_tli)
{
	PGresult   *res;
	PQExpBuffer query;
	XLogRecPtr	lsn_loc = InvalidXLogRecPtr;
	TimeLineID	tli_loc = 0;

	if (restart_lsn)
		*restart_lsn = lsn_loc;
	if (restart_tli)
		*restart_tli = tli_loc;

	query = createPQExpBuffer();
	appendPQExpBuffer(query, "READ_REPLICATION_SLOT %s", slot_name);
	res = PQexec(conn, query->data);
	destroyPQExpBuffer(query);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not send replication command \"%s\": %s",
					 "READ_REPLICATION_SLOT", PQerrorMessage(conn));
		PQclear(res);
		return false;
	}

	/* 该命令始终应精确地返回一个元组和三个字段          */
	if (PQntuples(res) != 1 || PQnfields(res) != 3)
	{
		pg_log_error("could not read replication slot \"%s\": got %d rows and %d fields, expected %d rows and %d fields",
					 slot_name, PQntuples(res), PQnfields(res), 1, 3);
		PQclear(res);
		return false;
	}

	/*
	 * 当槽不存在时，该命令返回一个带有 NULL 值的元组。
	 * 这只检查槽类型字段。
	              */
	if (PQgetisnull(res, 0, 0))
	{
		pg_log_error("replication slot \"%s\" does not exist", slot_name);
		PQclear(res);
		return false;
	}

	/*
	 * Note that this cannot happen as READ_REPLICATION_SLOT supports only
	 * physical slots, but play it safe.
	 */
	if (strcmp(PQgetvalue(res, 0, 0), "physical") != 0)
	{
		pg_log_error("expected a physical replication slot, got type \"%s\" instead",
					 PQgetvalue(res, 0, 0));
		PQclear(res);
		return false;
	}

	/* restart LSN */
	if (!PQgetisnull(res, 0, 1))
	{
		uint32		hi,
					lo;

		if (sscanf(PQgetvalue(res, 0, 1), "%X/%X", &hi, &lo) != 2)
		{
			pg_log_error("could not parse restart_lsn \"%s\" for replication slot \"%s\"",
						 PQgetvalue(res, 0, 1), slot_name);
			PQclear(res);
			return false;
		}
		lsn_loc = ((uint64) hi) << 32 | lo;
	}

	/* 当前 TLI  */
	if (!PQgetisnull(res, 0, 2))
		tli_loc = (TimeLineID) atol(PQgetvalue(res, 0, 2));

	PQclear(res);

	/* 如果请求，分配结果 */
	if (restart_lsn)
		*restart_lsn = lsn_loc;
	if (restart_tli)
		*restart_tli = tli_loc;

	return true;
}

/*
 * 为给定的连接创建一个复制槽。此函数在成功时返回 true。
                         */
bool
CreateReplicationSlot(PGconn *conn, const char *slot_name, const char *plugin,
					  bool is_temporary, bool is_physical, bool reserve_wal,
					  bool slot_exists_ok, bool two_phase)
{
	PQExpBuffer query;
	PGresult   *res;
	bool		use_new_option_syntax = (PQserverVersion(conn) >= 150000);

	query = createPQExpBuffer();

	Assert((is_physical && plugin == NULL) ||
		   (!is_physical && plugin != NULL));
	Assert(!(two_phase && is_physical));
	Assert(slot_name != NULL);

	/* 构建查询的基础部分 */
	appendPQExpBuffer(query, "CREATE_REPLICATION_SLOT \"%s\"", slot_name);
	if (is_temporary)
		appendPQExpBufferStr(query, " TEMPORARY");
	if (is_physical)
		appendPQExpBufferStr(query, " PHYSICAL");
	else
		appendPQExpBuffer(query, " LOGICAL \"%s\"", plugin);

	/* Add any requested options */
	if (use_new_option_syntax)
		appendPQExpBufferStr(query, " (");
	if (is_physical)
	{
		if (reserve_wal)
			AppendPlainCommandOption(query, use_new_option_syntax,
									 "RESERVE_WAL");
	}
	else
	{
		if (two_phase && PQserverVersion(conn) >= 150000)
			AppendPlainCommandOption(query, use_new_option_syntax,
									 "TWO_PHASE");

		if (PQserverVersion(conn) >= 100000)
		{
			/* pg_recvlogical 不使用导出的快照，因此禁用       */
			if (use_new_option_syntax)
				AppendStringCommandOption(query, use_new_option_syntax,
										  "SNAPSHOT", "nothing");
			else
				AppendPlainCommandOption(query, use_new_option_syntax,
										 "NOEXPORT_SNAPSHOT");
		}
	}
	if (use_new_option_syntax)
	{
		/* 如果选项列表为空，则禁用它，否则终止         */
		if (query->data[query->len - 1] == '(')
		{
			query->len -= 2;
			query->data[query->len] = '\0';
		}
		else
			appendPQExpBufferChar(query, ')');
	}

	/* Now run the query */
	res = PQexec(conn, query->data);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);

		if (slot_exists_ok &&
			sqlstate &&
			strcmp(sqlstate, ERRCODE_DUPLICATE_OBJECT) == 0)
		{
			destroyPQExpBuffer(query);
			PQclear(res);
			return true;
		}
		else
		{
			pg_log_error("could not send replication command \"%s\": %s",
						 query->data, PQerrorMessage(conn));

			destroyPQExpBuffer(query);
			PQclear(res);
			return false;
		}
	}

	if (PQntuples(res) != 1 || PQnfields(res) != 4)
	{
		pg_log_error("could not create replication slot \"%s\": got %d rows and %d fields, expected %d rows and %d fields",
					 slot_name,
					 PQntuples(res), PQnfields(res), 1, 4);

		destroyPQExpBuffer(query);
		PQclear(res);
		return false;
	}

	destroyPQExpBuffer(query);
	PQclear(res);
	return true;
}

/*
 * 删除给定连接的复制槽。此函数在成功时返回 true。
                                */
bool
DropReplicationSlot(PGconn *conn, const char *slot_name)
{
	PQExpBuffer query;
	PGresult   *res;

	Assert(slot_name != NULL);

	query = createPQExpBuffer();

	/* Build query */
	appendPQExpBuffer(query, "DROP_REPLICATION_SLOT \"%s\"",
					  slot_name);
	res = PQexec(conn, query->data);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("could not send replication command \"%s\": %s",
					 query->data, PQerrorMessage(conn));

		destroyPQExpBuffer(query);
		PQclear(res);
		return false;
	}

	if (PQntuples(res) != 0 || PQnfields(res) != 0)
	{
		pg_log_error("could not drop replication slot \"%s\": got %d rows and %d fields, expected %d rows and %d fields",
					 slot_name,
					 PQntuples(res), PQnfields(res), 0, 0);

		destroyPQExpBuffer(query);
		PQclear(res);
		return false;
	}

	destroyPQExpBuffer(query);
	PQclear(res);
	return true;
}

/*
 * Append a "plain" option - one with no value - to a server command that
 * is being constructed.
 *
 * In the old syntax, all options were parser keywords, so you could just
 * write things like SOME_COMMAND OPTION1 OPTION2 'opt2value' OPTION3 42. The
 * new syntax uses a comma-separated list surrounded by parentheses, so the
 * equivalent is SOME_COMMAND (OPTION1, OPTION2 'optvalue', OPTION3 42).
 */
void
AppendPlainCommandOption(PQExpBuffer buf, bool use_new_option_syntax,
						 char *option_name)
{
	if (buf->len > 0 && buf->data[buf->len - 1] != '(')
	{
		if (use_new_option_syntax)
			appendPQExpBufferStr(buf, ", ");
		else
			appendPQExpBufferChar(buf, ' ');
	}

	appendPQExpBuffer(buf, " %s", option_name);
}

/*
 * 将一个带有关联字符串值的选项附加到正在构建的服务器命令中。
 *
 * 请参阅上方 AppendPlainCommandOption 的注释。
        */
void
AppendStringCommandOption(PQExpBuffer buf, bool use_new_option_syntax,
						  char *option_name, char *option_value)
{
	AppendPlainCommandOption(buf, use_new_option_syntax, option_name);

	if (option_value != NULL)
	{
		size_t		length = strlen(option_value);
		char	   *escaped_value = palloc(1 + 2 * length);

		PQescapeStringConn(conn, escaped_value, option_value, length, NULL);
		appendPQExpBuffer(buf, " '%s'", escaped_value);
		pfree(escaped_value);
	}
}

/*
 * 将一个带有关联整数值的选项附加到正在构建的服务器命令中。
 *
 * 请参阅上方 AppendPlainCommandOption 的注释。
       */
void
AppendIntegerCommandOption(PQExpBuffer buf, bool use_new_option_syntax,
						   char *option_name, int32 option_value)
{
	AppendPlainCommandOption(buf, use_new_option_syntax, option_name);

	appendPQExpBuffer(buf, " %d", option_value);
}

/*
 * GetCurrentTimestamp() 的前端版本，因为我们没有链接后端代码。
          */
TimestampTz
feGetCurrentTimestamp(void)
{
	TimestampTz result;
	struct timeval tp;

	gettimeofday(&tp, NULL);

	result = (TimestampTz) tp.tv_sec -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
	result = (result * USECS_PER_SEC) + tp.tv_usec;

	return result;
}

/*
 * TimestampDifference() 的前端版本，因为我们没有链接后端代码。
          */
void
feTimestampDifference(TimestampTz start_time, TimestampTz stop_time,
					  long *secs, int *microsecs)
{
	TimestampTz diff = stop_time - start_time;

	if (diff <= 0)
	{
		*secs = 0;
		*microsecs = 0;
	}
	else
	{
		*secs = (long) (diff / USECS_PER_SEC);
		*microsecs = (int) (diff % USECS_PER_SEC);
	}
}

/*
 * TimestampDifferenceExceeds() 的前端版本，因为我们没有链接后端代码。
          */
bool
feTimestampDifferenceExceeds(TimestampTz start_time,
							 TimestampTz stop_time,
							 int msec)
{
	TimestampTz diff = stop_time - start_time;

	return (diff >= msec * INT64CONST(1000));
}

/*
 * 将 int64 转换为网络字节序。
    */
void
fe_sendint64(int64 i, char *buf)
{
	uint64		n64 = pg_hton64(i);

	memcpy(buf, &n64, sizeof(n64));
}

/*
 * 将 int64 从网络字节序转换为本机格式。
        */
int64
fe_recvint64(char *buf)
{
	uint64		n64;

	memcpy(&n64, buf, sizeof(n64));

	return pg_ntoh64(n64);
}
