/*-------------------------------------------------------------------------
 *
 * pg_backup_db.c
 *
 *	Implements the basic DB functions used by the archiver.
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/pg_backup_db.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <unistd.h>
#include <ctype.h>
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include "common/connect.h"
#include "common/string.h"
#include "dumputils.h"
#include "fe_utils/string_utils.h"
#include "parallel.h"
#include "pg_backup_archiver.h"
#include "pg_backup_db.h"
#include "pg_backup_utils.h"

static void _check_database_version(ArchiveHandle *AH);
static void notice_processor(void *arg, const char *message);

static void
_check_database_version(ArchiveHandle *AH)
{
	const char *remoteversion_str;
	int			remoteversion;
	PGresult   *res;

	remoteversion_str = PQparameterStatus(AH->connection, "server_version");
	remoteversion = PQserverVersion(AH->connection);
	if (remoteversion == 0 || !remoteversion_str)
		pg_fatal("could not get server_version from libpq");

	AH->public.remoteVersionStr = pg_strdup(remoteversion_str);
	AH->public.remoteVersion = remoteversion;
	if (!AH->archiveRemoteVersion)
		AH->archiveRemoteVersion = AH->public.remoteVersionStr;

	if (remoteversion != PG_VERSION_NUM
		&& (remoteversion < AH->public.minRemoteVersion ||
			remoteversion > AH->public.maxRemoteVersion))
	{
		pg_log_error("aborting because of server version mismatch");
		pg_log_error_detail("server version: %s; %s version: %s",
							remoteversion_str, progname, PG_VERSION);
		exit(1);
	}

	/*
	 * 检查服务器是否处于恢复模式，这意味着我们在热备
	 * 节点上。
	 */
	res = ExecuteSqlQueryForSingleRow((Archive *) AH,
									  "SELECT pg_catalog.pg_is_in_recovery()");
	AH->public.isStandby = (strcmp(PQgetvalue(res, 0, 0), "t") == 0);
	PQclear(res);
}

/*
 * 重新连接到服务器。如果 dbname 不为 NULL，则使用该数据库，
 * 否则使用与归档句柄关联的数据库。
 */
void
ReconnectToServer(ArchiveHandle *AH, const char *dbname)
{
	PGconn	   *oldConn = AH->connection;
	RestoreOptions *ropt = AH->public.ropt;

	/*
	 * 将 dbname（如果给定）保存到 override_dbname 中，使其也
	 * 影响后续的重新连接尝试。
	 */
	if (dbname)
		ropt->cparams.override_dbname = pg_strdup(dbname);

	/*
	 * 注意：我们希望在关闭旧连接之前建立新连接，特别是更新
	 * ArchiveHandle 的 connCancel。否则，不合时宜的 SIGINT
	 * 可能会尝试访问已断开的连接。
	 */
	AH->connection = NULL;		/* 避开 ConnectDatabase 中的错误检查 */

	ConnectDatabase((Archive *) AH, &ropt->cparams, true);

	PQfinish(oldConn);
}

/*
 * 使用给定参数建立或重新建立数据库连接。
 *
 * 生成的连接句柄存储在 AHX->connection 中。
 *
 * 如果需要，会自动发出交互式密码提示。
 * 我们将其结果存储在 AHX->savedPassword 中。
 * 注意：如果用户名不断变化，使用单项密码缓存并不真正
 * 合理。但在当前使用中，用户名从不会改变，因此单个
 * savedPassword 已足够。
 */
void
ConnectDatabase(Archive *AHX,
				const ConnParams *cparams,
				bool isReconnect)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	trivalue	prompt_password;
	char	   *password;
	bool		new_pass;

	if (AH->connection)
		pg_fatal("already connected to a database");

	/* 重新连接期间绝不提示密码 */
	prompt_password = isReconnect ? TRI_NO : cparams->promptPassword;

	password = AH->savedPassword;

	if (prompt_password == TRI_YES && password == NULL)
		password = simple_prompt("Password: ", false);

	/*
	 * 开始连接。如果后端请求密码，则循环直到获取
	 * 密码。
	 */
	do
	{
		const char *keywords[8];
		const char *values[8];
		int			i = 0;

		/*
		 * 如果 dbname 是连接字符串，其条目可以覆盖从 cparams
		 * 获取的其他值；但反过来，override_dbname 可以覆盖
		 * 其中的 dbname 组件。
		 */
		keywords[i] = "host";
		values[i++] = cparams->pghost;
		keywords[i] = "port";
		values[i++] = cparams->pgport;
		keywords[i] = "user";
		values[i++] = cparams->username;
		keywords[i] = "password";
		values[i++] = password;
		keywords[i] = "dbname";
		values[i++] = cparams->dbname;
		if (cparams->override_dbname)
		{
			keywords[i] = "dbname";
			values[i++] = cparams->override_dbname;
		}
		keywords[i] = "fallback_application_name";
		values[i++] = progname;
		keywords[i] = NULL;
		values[i++] = NULL;
		Assert(i <= lengthof(keywords));

		new_pass = false;
		AH->connection = PQconnectdbParams(keywords, values, true);

		if (!AH->connection)
			pg_fatal("could not connect to database");

		if (PQstatus(AH->connection) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(AH->connection) &&
			password == NULL &&
			prompt_password != TRI_NO)
		{
			PQfinish(AH->connection);
			password = simple_prompt("Password: ", false);
			new_pass = true;
		}
	} while (new_pass);

	/* 检查后端连接是否成功建立 */
	if (PQstatus(AH->connection) == CONNECTION_BAD)
	{
		if (isReconnect)
			pg_fatal("reconnection failed: %s",
					 PQerrorMessage(AH->connection));
		else
			pg_fatal("%s",
					 PQerrorMessage(AH->connection));
	}

	/* 以严格模式启动；后续阶段可能会覆盖此设置。 */
	PQclear(ExecuteSqlQueryForSingleRow((Archive *) AH,
										ALWAYS_SECURE_SEARCH_PATH_SQL));

	if (password && password != AH->savedPassword)
		free(password);

	/*
	 * 我们希望记住连接的实际密码，无论是否通过提示获取。
	 * 因此我们不仅仅存储 password 变量。
	 */
	if (PQconnectionUsedPassword(AH->connection))
	{
		free(AH->savedPassword);
		AH->savedPassword = pg_strdup(PQpass(AH->connection));
	}

	/* 检查版本不匹配 */
	_check_database_version(AH);

	PQsetNoticeProcessor(AH->connection, notice_processor, NULL);

	/* 安排 SIGINT 在此连接上发出查询取消 */
	set_archive_cancel_info(AH, AH->connection);
}

/*
 * 关闭数据库连接，如果有正在运行的查询，
 * 也会取消。
 */
void
DisconnectDatabase(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	char		errbuf[1];

	if (!AH->connection)
		return;

	if (AH->connCancel)
	{
		/*
		 * 如果有活动查询，在关闭之前发送取消请求，忽略
		 * 任何错误。这对正常退出没有作用，但在 pg_fatal()
		 * 期间可能有用。
		 */
		if (PQtransactionStatus(AH->connection) == PQTRANS_ACTIVE)
			(void) PQcancel(AH->connCancel, errbuf, sizeof(errbuf));

		/*
		 * 防止信号处理器在此之后发送取消。
		 */
		set_archive_cancel_info(AH, NULL);
	}

	PQfinish(AH->connection);
	AH->connection = NULL;
}

PGconn *
GetConnection(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	return AH->connection;
}

static void
notice_processor(void *arg, const char *message)
{
	pg_log_info("%s", message);
}

/* 类似 pg_fatal()，但带有关于特定查询的投诉。 */
static void
die_on_query_failure(ArchiveHandle *AH, const char *query)
{
	pg_log_error("query failed: %s",
				 PQerrorMessage(AH->connection));
	pg_log_error_detail("Query was: %s", query);
	exit(1);
}

void
ExecuteSqlStatement(Archive *AHX, const char *query)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	PGresult   *res;

	res = PQexec(AH->connection, query);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		die_on_query_failure(AH, query);
	PQclear(res);
}

PGresult *
ExecuteSqlQuery(Archive *AHX, const char *query, ExecStatusType status)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	PGresult   *res;

	res = PQexec(AH->connection, query);
	if (PQresultStatus(res) != status)
		die_on_query_failure(AH, query);
	return res;
}

/*
 * 执行 SQL 查询并验证恰好返回了一行。
 */
PGresult *
ExecuteSqlQueryForSingleRow(Archive *fout, const char *query)
{
	PGresult   *res;
	int			ntups;

	res = ExecuteSqlQuery(fout, query, PGRES_TUPLES_OK);

	/* 仅期待单个结果 */
	ntups = PQntuples(res);
	if (ntups != 1)
		pg_fatal(ngettext("query returned %d row instead of one: %s",
						  "query returned %d rows instead of one: %s",
						  ntups),
				 ntups, query);

	return res;
}

/*
 * 发送查询的便捷函数。
 * 监控结果以检测 COPY 语句
 */
static void
ExecuteSqlCommand(ArchiveHandle *AH, const char *qry, const char *desc)
{
	PGconn	   *conn = AH->connection;
	PGresult   *res;

#ifdef NOT_USED
	fprintf(stderr, "Executing: '%s'\n\n", qry);
#endif
	res = PQexec(conn, qry);

	switch (PQresultStatus(res))
	{
		case PGRES_COMMAND_OK:
		case PGRES_TUPLES_OK:
		case PGRES_EMPTY_QUERY:
			/* 一切正常 */
			break;
		case PGRES_COPY_IN:
			/* 假定这是预期结果 */
			AH->pgCopyIn = true;
			break;
		default:
			/* 出问题了 */
			warn_or_exit_horribly(AH, "%s: %sCommand was: %s",
								  desc, PQerrorMessage(conn), qry);
			break;
	}

	PQclear(res);
}


/*
 * 处理非 COPY 表数据（即 INSERT 命令）。
 *
 * 这些命令为了可压缩性被合并为一个长字符串，
 * 我们以任意边界的缓冲区块接收它们，
 * 因此我们必须定位命令边界并跨调用保存部分命令。
 * 所有状态必须保存在 AH->sqlparse 中，而不能存在于该例程的
 * 局部变量中。我们假定 AH->sqlparse 在创建时已填充零。
 *
 * 我们必须对数据进行词法分析，以识别字面量和带引号的
 * 标识符，从而识别语句终止分号。我们假定 INSERT 数据不包含
 * SQL 注释、E'' 字面量或美元引号字符串，因此这比完整的 SQL 词法
 * 分析器简单得多。
 *
 * 注意：从 9.0 之前的导出文件恢复时，此代码也用于
 * 处理 BLOB COMMENTS 数据，该数据同样存在包含多个可能
 * 跨缓冲区块分割的 SQL 命令的问题。幸运的是，
 * 那些数据也不会包含任何复杂的词法内容。
 */
static void
ExecuteSimpleCommands(ArchiveHandle *AH, const char *buf, size_t bufLen)
{
	const char *qry = buf;
	const char *eos = buf + bufLen;

	/* 如果是初次处理，则初始化命令缓冲区 */
	if (AH->sqlparse.curCmd == NULL)
		AH->sqlparse.curCmd = createPQExpBuffer();

	for (; qry < eos; qry++)
	{
		char		ch = *qry;

		/* 为整洁起见，跳过命令间的换行符 */
		if (!(ch == '\n' && AH->sqlparse.curCmd->len == 0))
			appendPQExpBufferChar(AH->sqlparse.curCmd, ch);

		switch (AH->sqlparse.state)
		{
			case SQL_SCAN:		/* 默认状态 == 0，在 _allocAH 中设置 */
				if (ch == ';')
				{
					/*
					 * 已找到语句结尾。发送并重置
					 * 缓冲区。
					 */
					ExecuteSqlCommand(AH, AH->sqlparse.curCmd->data,
									  "could not execute query");
					resetPQExpBuffer(AH->sqlparse.curCmd);
				}
				else if (ch == '\'')
				{
					AH->sqlparse.state = SQL_IN_SINGLE_QUOTE;
					AH->sqlparse.backSlash = false;
				}
				else if (ch == '"')
				{
					AH->sqlparse.state = SQL_IN_DOUBLE_QUOTE;
				}
				break;

			case SQL_IN_SINGLE_QUOTE:
				/* 我们无需特殊处理 '' */
				if (ch == '\'' && !AH->sqlparse.backSlash)
					AH->sqlparse.state = SQL_SCAN;
				else if (ch == '\\' && !AH->public.std_strings)
					AH->sqlparse.backSlash = !AH->sqlparse.backSlash;
				else
					AH->sqlparse.backSlash = false;
				break;

			case SQL_IN_DOUBLE_QUOTE:
				/* 我们无需特殊处理 "" */
				if (ch == '"')
					AH->sqlparse.state = SQL_SCAN;
				break;
		}
	}
}


/*
 * 实现用于直连数据库恢复的 ahwrite()
 */
int
ExecuteSqlCommandBuf(Archive *AHX, const char *buf, size_t bufLen)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	if (AH->outputKind == OUTPUT_COPYDATA)
	{
		/*
		 * COPY 数据。
		 *
		 * 如果 libpq 未能进入 COPY 模式，我们将丢弃数据；
		 * 这使我们在 COPY 命令出错后尝试继续时
		 * 能够合理地运行。
		 */
		if (AH->pgCopyIn &&
			PQputCopyData(AH->connection, buf, bufLen) <= 0)
			pg_fatal("error returned by PQputCopyData: %s",
					 PQerrorMessage(AH->connection));
	}
	else if (AH->outputKind == OUTPUT_OTHERDATA)
	{
		/*
		 * 以 INSERT 命令表示的表数据；或在旧的导出文件中，
		 * BLOB COMMENTS 数据（以 COMMENT ON 命令表示）。
		 */
		ExecuteSimpleCommands(AH, buf, bufLen);
	}
	else
	{
		/*
		 * 通用 SQL 命令；我们假定命令不会跨调用
		 * 分割。
		 *
		 * 在大多数情况下，传递给我们的数据是空终止
		 * 字符串，但如果不是，我们必须添加尾随 null。
		 */
		if (buf[bufLen] == '\0')
			ExecuteSqlCommand(AH, buf, "could not execute query");
		else
		{
			char	   *str = (char *) pg_malloc(bufLen + 1);

			memcpy(str, buf, bufLen);
			str[bufLen] = '\0';
			ExecuteSqlCommand(AH, str, "could not execute query");
			free(str);
		}
	}

	return bufLen;
}

/*
 * 在直连数据库恢复期间终止 COPY 操作
 */
void
EndDBCopyMode(Archive *AHX, const char *tocEntryTag)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	if (AH->pgCopyIn)
	{
		PGresult   *res;

		if (PQputCopyEnd(AH->connection, NULL) <= 0)
			pg_fatal("error returned by PQputCopyEnd: %s",
					 PQerrorMessage(AH->connection));

		/* 检查命令状态并返回到正常的 libpq 状态 */
		res = PQgetResult(AH->connection);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			warn_or_exit_horribly(AH, "COPY failed for table \"%s\": %s",
								  tocEntryTag, PQerrorMessage(AH->connection));
		PQclear(res);

		/* 执行此操作以确保将 libpq 泵回空闲状态 */
		if (PQgetResult(AH->connection) != NULL)
			pg_log_warning("unexpected extra results during COPY of table \"%s\"",
						   tocEntryTag);

		AH->pgCopyIn = false;
	}
}

void
StartTransaction(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	ExecuteSqlCommand(AH, "BEGIN", "could not start database transaction");
}

void
CommitTransaction(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	ExecuteSqlCommand(AH, "COMMIT", "could not commit database transaction");
}

void
DropLOIfExists(ArchiveHandle *AH, Oid oid)
{
	/*
	 * 如果不恢复到直接数据库连接，我们必须猜测如何
	 * 检测 LO 是否存在。假定为新型。
	 */
	if (AH->connection == NULL ||
		PQserverVersion(AH->connection) >= 90000)
	{
		ahprintf(AH,
				 "SELECT pg_catalog.lo_unlink(oid) "
				 "FROM pg_catalog.pg_largeobject_metadata "
				 "WHERE oid = '%u';\n",
				 oid);
	}
	else
	{
		/* 恢复到 9.0 之前的服务器，因此使用旧方式 */
		ahprintf(AH,
				 "SELECT CASE WHEN EXISTS("
				 "SELECT 1 FROM pg_catalog.pg_largeobject WHERE loid = '%u'"
				 ") THEN pg_catalog.lo_unlink('%u') END;\n",
				 oid, oid);
	}
}
