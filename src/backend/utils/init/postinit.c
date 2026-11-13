/*-------------------------------------------------------------------------
 *
 * postinit.c
 *	  postgres initialization utilities
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/postinit.c
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/session.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_tablespace.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/postmaster.h"
#include "replication/slot.h"
#include "replication/walsender.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/sinvaladt.h"
#include "storage/smgr.h"
#include "storage/sync.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/portal.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timeout.h"

static HeapTuple GetDatabaseTuple(const char *dbname);
static HeapTuple GetDatabaseTupleByOid(Oid dboid);
static void PerformAuthentication(Port *port);
static void CheckMyDatabase(const char *name, bool am_superuser, bool override_allow_connections);
static void ShutdownPostgres(int code, Datum arg);
static void StatementTimeoutHandler(void);
static void LockTimeoutHandler(void);
static void IdleInTransactionSessionTimeoutHandler(void);
static void IdleSessionTimeoutHandler(void);
static void IdleStatsUpdateTimeoutHandler(void);
static void ClientCheckTimeoutHandler(void);
static bool ThereIsAtLeastOneRole(void);
static void process_startup_options(Port *port, bool am_superuser);
static void process_settings(Oid databaseid, Oid roleid);


/*** InitPostgres support ***/


/*
 * GetDatabaseTuple -- 获取某数据库的 pg_database 行
 *
 * 本函数在 backend 启动时尚无任何系统目录访问权时使用
 * 我们可以仅用 relcache.c 为 pg_database 创建的硬编码描述符
 * pg_database。较典型的情况下，relcache.c 能从共享 relcache 
 * 文件加载 pg_database 及其索引的描述符，因此可以
 * criticalSharedRelcachesBuilt 表明是否拿到了缓存的描述符。
 * cache file, and so we can do an indexscan.  criticalSharedRelcachesBuilt
 * tells whether we got the cached descriptors.
 */
static HeapTuple
GetDatabaseTuple(const char *dbname)
{
	HeapTuple	tuple;
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key[1];

	/*
	 * 构造扫描键
	 */
	ScanKeyInit(&key[0],
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(dbname));

	/*
	 * 打开 pg_database 并取得一个元组。如果尚未构建关键的
	 * relcache 条目（即启动时没有共享 relcache 缓存文件）
	 * 
	 */
	relation = table_open(DatabaseRelationId, AccessShareLock);
	scan = systable_beginscan(relation, DatabaseNameIndexId,
							  criticalSharedRelcachesBuilt,
							  NULL,
							  1, key);

	tuple = systable_getnext(scan);

	/* 释放缓冲区前必须先复制元组 */
	if (HeapTupleIsValid(tuple))
		tuple = heap_copytuple(tuple);

	/* 完成 */
	systable_endscan(scan);
	table_close(relation, AccessShareLock);

	return tuple;
}

/*
 * GetDatabaseTupleByOid -- 同上，但按数据库 OID 搜索
 */
static HeapTuple
GetDatabaseTupleByOid(Oid dboid)
{
	HeapTuple	tuple;
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key[1];

	/*
	 * 构造扫描键
	 */
	ScanKeyInit(&key[0],
				Anum_pg_database_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dboid));

	/*
	 * 打开 pg_database 并取得一个元组。若尚未构建关键的共
	 * relcache 条目（即启动时没有共享 relcache 缓存文件）
	 * 
	 */
	relation = table_open(DatabaseRelationId, AccessShareLock);
	scan = systable_beginscan(relation, DatabaseOidIndexId,
							  criticalSharedRelcachesBuilt,
							  NULL,
							  1, key);

	tuple = systable_getnext(scan);

	/* 释放缓冲区前必须先复制元组 */
	if (HeapTupleIsValid(tuple))
		tuple = heap_copytuple(tuple);

	/* 完成 */
	systable_endscan(scan);
	table_close(relation, AccessShareLock);

	return tuple;
}


/*
 * PerformAuthentication -- 认证远程客户端
 *
 * 返回：无。若有任何失败则根本不会返回。
 */
static void
PerformAuthentication(Port *port)
{
	/* This should be set already, but let's make sure */
	ClientAuthInProgress = true;	/* 限制日志消息的可见性 */

	/*
	 * 在 EXEC_BACKEND 情况下，我们没有从 postmaster 继承
	 * pg_hba.conf 等内容，必须自己加载。
	 *
	 * FIXME: [fork/exec] 唔。有办法避免这种开销吗？
	 */
#ifdef EXEC_BACKEND

	/*
	 * 所以如果不存在就创建它（不会存在）。稍后我们会
	 * 再次删除它。
	 * again later, in PostgresMain.
	 */
	if (PostmasterContext == NULL)
		PostmasterContext = AllocSetContextCreate(TopMemoryContext,
												  "Postmaster",
												  ALLOCSET_DEFAULT_SIZES);

	if (!load_hba())
	{
		/*
		 * 如果加载 HBA 文件失败则没有理由继续，因为这
		 * 连接到数据库。
		 */
		ereport(FATAL,
		/* translator: %s is a configuration file */
				(errmsg("could not load %s", HbaFileName)));
	}

	if (!load_ident())
	{
		/*
		 * 用户名映射的任何认证方法登录。load_ident() 已将错
		 * 记录到日志中。
		 * methods that need a user name mapping. load_ident() already logged
		 * the details of error to the log.
		 */
	}
#endif

	/*
	 * 设置超时以防有缺陷或恶意的客户端在认证期间不响
	 * 由于我们在事务内部且可能做数据库访问，必须使用
	 * statement_timeout 基础设施。
	 */
	enable_timeout_after(STATEMENT_TIMEOUT, AuthenticationTimeout * 1000);

	/*
	 * 现在执行认证交换。
	 */
	set_ps_display("authentication");
	ClientAuthentication(port); /* might not return, if failure */

	/*
	 * 认证完成。禁用超时，必要时记录日志。
	 */
	disable_timeout(STATEMENT_TIMEOUT, false);

	if (Log_connections)
	{
		StringInfoData logmsg;

		initStringInfo(&logmsg);
		if (am_walsender)
			appendStringInfo(&logmsg, _("replication connection authorized: user=%s"),
							 port->user_name);
		else
			appendStringInfo(&logmsg, _("connection authorized: user=%s"),
							 port->user_name);
		if (!am_walsender)
			appendStringInfo(&logmsg, _(" database=%s"), port->database_name);

		if (port->application_name != NULL)
			appendStringInfo(&logmsg, _(" application_name=%s"),
							 port->application_name);

#ifdef USE_SSL
		if (port->ssl_in_use)
			appendStringInfo(&logmsg, _(" SSL enabled (protocol=%s, cipher=%s, bits=%d)"),
							 be_tls_get_version(port),
							 be_tls_get_cipher(port),
							 be_tls_get_cipher_bits(port));
#endif
#ifdef ENABLE_GSS
		if (port->gss)
		{
			const char *princ = be_gssapi_get_princ(port);

			if (princ)
				appendStringInfo(&logmsg,
								 _(" GSS (authenticated=%s, encrypted=%s, delegated_credentials=%s, principal=%s)"),
								 be_gssapi_get_auth(port) ? _("yes") : _("no"),
								 be_gssapi_get_enc(port) ? _("yes") : _("no"),
								 be_gssapi_get_delegation(port) ? _("yes") : _("no"),
								 princ);
			else
				appendStringInfo(&logmsg,
								 _(" GSS (authenticated=%s, encrypted=%s, delegated_credentials=%s)"),
								 be_gssapi_get_auth(port) ? _("yes") : _("no"),
								 be_gssapi_get_enc(port) ? _("yes") : _("no"),
								 be_gssapi_get_delegation(port) ? _("yes") : _("no"));
		}
#endif

		ereport(LOG, errmsg_internal("%s", logmsg.data));
		pfree(logmsg.data);
	}

	set_ps_display("startup");

	ClientAuthInProgress = false;	/* client_min_messages 现已激活 */
}


/*
 * CheckMyDatabase -- 从 pg_database 条目获取我们数据库的信息
 */
static void
CheckMyDatabase(const char *name, bool am_superuser, bool override_allow_connections)
{
	HeapTuple	tup;
	Form_pg_database dbform;
	Datum		datum;
	bool		isnull;
	char	   *collate;
	char	   *ctype;
	char	   *iculocale;

	/* Fetch our pg_database row normally, via syscache */
	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	dbform = (Form_pg_database) GETSTRUCT(tup);

	/* 这个复查纯属偏执 */
	if (strcmp(name, NameStr(dbform->datname)) != 0)
		ereport(FATAL,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" has disappeared from pg_database",
						name),
				 errdetail("Database OID %u now seems to belong to \"%s\".",
						   MyDatabaseId, NameStr(dbform->datname))));

	/*
	 * 检查连接到该数据库的权限。
	 *
	 * 独立模式下不强制这些检查，这样才有办法从禁用所
	 * 糊涂操作中恢复，如 "UPDATE pg_database SET datallowconn
	 * example "UPDATE pg_database SET datallowconn = false;".
	 */
	if (IsUnderPostmaster)
	{
		/*
		 * 检查数据库当前是否允许连接。
		 * （后台进程可通过设置 override_allow_connections 覆盖
		 * 和下一个测试。）
		 */
		if (!dbform->datallowconn && !override_allow_connections)
			ereport(FATAL,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("database \"%s\" is not currently accepting connections",
							name)));

		/*
		 * 检查连接到该数据库的权限。（am_superuser 测试是冗
		 * 但既然有这个标志，不如顺便检查一下，
		 * 节省几个周期。
		 */
		if (!am_superuser && !override_allow_connections &&
			object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(),
							ACL_CONNECT) != ACLCHECK_OK)
			ereport(FATAL,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied for database \"%s\"", name),
					 errdetail("User does not have CONNECT privilege.")));

		/*
		 * 检查此数据库的连接上限。只对常规 backend 强制
		 * 类型有各自的 PGPROC 池。
		 * PGPROC pools.
		 *
		 * PGPROC。如果两个 backend 几乎同时这么做，它们
		 * 超限，而理想情况下应一个成功一个失败。把它做
		 * 得不偿失；因此我们只在文档中说明连接上限是
		 * ideally one should succeed and one fail.  Getting that to work
		 * exactly seems more trouble than it is worth, however; instead we
		 * just document that the connection limit is approximate.
		 */
		if (dbform->datconnlimit >= 0 &&
			AmRegularBackendProcess() &&
			!am_superuser &&
			CountDBConnections(MyDatabaseId) > dbform->datconnlimit)
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("too many connections for database \"%s\"",
							name)));
	}

	/*
	 * 好了，没问题了。下一个待办事项是从 pg_database 元组
	 * 编码信息。
	 */
	SetDatabaseEncoding(dbform->encoding);
	/* 也将其记录为 GUC 内部选项 */
	SetConfigOption("server_encoding", GetDatabaseEncodingName(),
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
	/* If we have no other source of client_encoding, use server encoding */
	SetConfigOption("client_encoding", GetDatabaseEncodingName(),
					PGC_BACKEND, PGC_S_DYNAMIC_DEFAULT);

	/* assign locale variables */
	datum = SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_datcollate);
	collate = TextDatumGetCString(datum);
	datum = SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_datctype);
	ctype = TextDatumGetCString(datum);

	if (pg_perm_setlocale(LC_COLLATE, collate) == NULL)
		ereport(FATAL,
				(errmsg("database locale is incompatible with operating system"),
				 errdetail("The database was initialized with LC_COLLATE \"%s\", "
						   " which is not recognized by setlocale().", collate),
				 errhint("Recreate the database with another locale or install the missing locale.")));

	if (pg_perm_setlocale(LC_CTYPE, ctype) == NULL)
		ereport(FATAL,
				(errmsg("database locale is incompatible with operating system"),
				 errdetail("The database was initialized with LC_CTYPE \"%s\", "
						   " which is not recognized by setlocale().", ctype),
				 errhint("Recreate the database with another locale or install the missing locale.")));

	if (strcmp(ctype, "C") == 0 ||
		strcmp(ctype, "POSIX") == 0)
		database_ctype_is_c = true;

	if (dbform->datlocprovider == COLLPROVIDER_ICU)
	{
		char	   *icurules;

		datum = SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_daticulocale);
		iculocale = TextDatumGetCString(datum);

		datum = SysCacheGetAttr(DATABASEOID, tup, Anum_pg_database_daticurules, &isnull);
		if (!isnull)
			icurules = TextDatumGetCString(datum);
		else
			icurules = NULL;

		make_icu_collator(iculocale, icurules, &default_locale);
	}
	else
		iculocale = NULL;

	default_locale.provider = dbform->datlocprovider;

	/*
	 * 默认区域设置目前总是确定性的。非确定性区域设
	 * 模式匹配，如果全局应用会破坏很多东西。
	 * lot of things if applied globally.
	 */
	default_locale.deterministic = true;

	/*
	 * 检查排序规则版本。参见 pg_newlocal
	 * 类似代码。注意这里任何情况下都只是警告而非错误
	 * in any case, so that we don't prevent connecting.
	 */
	datum = SysCacheGetAttr(DATABASEOID, tup, Anum_pg_database_datcollversion,
							&isnull);
	if (!isnull)
	{
		char	   *actual_versionstr;
		char	   *collversionstr;

		collversionstr = TextDatumGetCString(datum);

		actual_versionstr = get_collation_actual_version(dbform->datlocprovider, dbform->datlocprovider == COLLPROVIDER_ICU ? iculocale : collate);
		if (!actual_versionstr)
			/* 不应发生 */
			elog(WARNING,
				 "database \"%s\" has no actual collation version, but a version was recorded",
				 name);
		else if (strcmp(actual_versionstr, collversionstr) != 0)
			ereport(WARNING,
					(errmsg("database \"%s\" has a collation version mismatch",
							name),
					 errdetail("The database was created using collation version %s, "
							   "but the operating system provides version %s.",
							   collversionstr, actual_versionstr),
					 errhint("Rebuild all objects in this database that use the default collation and run "
							 "ALTER DATABASE %s REFRESH COLLATION VERSION, "
							 "or build PostgreSQL with the right library version.",
							 quote_identifier(name))));
	}

	ReleaseSysCache(tup);
}


/*
 * pg_split_opts -- 拆分选项字符串并追加到 argv 数组
 *
 * 调用者负责确保 argv 数组足够大。本例程最多可添加的参
 * 为 (strlen(optstr) + 1) / 2。
 * (strlen(optstr) + 1) / 2.
 *
 * Because some option values can contain spaces we allow escaping using
 * backslashes, with \\ representing a literal backslash.
 */
void
pg_split_opts(char **argv, int *argcp, const char *optstr)
{
	StringInfoData s;

	initStringInfo(&s);

	while (*optstr)
	{
		bool		last_was_escape = false;

		resetStringInfo(&s);

		/* 跳过前导空格 */
		while (isspace((unsigned char) *optstr))
			optstr++;

		if (*optstr == '\0')
			break;

		/*
		 * 解析单个选项，在第一个空格处停止，除非它被
		 * escaped.
		 */
		while (*optstr)
		{
			if (isspace((unsigned char) *optstr) && !last_was_escape)
				break;

			if (!last_was_escape && *optstr == '\\')
				last_was_escape = true;
			else
			{
				last_was_escape = false;
				appendStringInfoChar(&s, *optstr);
			}

			optstr++;
		}

		/* 现在把该选项存入下一个 argv[] 位置 */
		argv[(*argcp)++] = pstrdup(s.data);
	}

	pfree(s.data);
}

/*
 * 从配置选项初始化 MaxBackends 值。
 *
 * 必须在模块有机会更改 shared_preload_libraries 中的 GUC 之
 * 确定共享内存大小之前调用。
 *
 * 注意在 EXEC_BACKEND 环境中，值由 postmaster 经
 * SubPostmasterMain 中的 BackendParameters 传给子进程；只有
 * postmaster 本身和不受 postmaster 控制的进程才应调用此函
 * this.
 */
void
InitializeMaxBackends(void)
{
	Assert(MaxBackends == 0);

	/* 额外的单位是给 autovacuum launcher 的 */
	MaxBackends = MaxConnections + autovacuum_max_workers + 1 +
		max_worker_processes + max_wal_senders;

	/* 内部错误，因为值都已提前检查过了 */
	if (MaxBackends > MAX_BACKENDS)
		elog(ERROR, "too many backends configured");
}

/*
 * max_connections 的 GUC check_hook
 */
bool
check_max_connections(int *newval, void **extra, GucSource source)
{
	if (*newval + autovacuum_max_workers + 1 +
		max_worker_processes + max_wal_senders > MAX_BACKENDS)
		return false;
	return true;
}

/*
 * autovacuum_max_workers 的 GUC check_hook
 */
bool
check_autovacuum_max_workers(int *newval, void **extra, GucSource source)
{
	if (MaxConnections + *newval + 1 +
		max_worker_processes + max_wal_senders > MAX_BACKENDS)
		return false;
	return true;
}

/*
 * max_worker_processes 的 GUC check_hook
 */
bool
check_max_worker_processes(int *newval, void **extra, GucSource source)
{
	if (MaxConnections + autovacuum_max_workers + 1 +
		*newval + max_wal_senders > MAX_BACKENDS)
		return false;
	return true;
}

/*
 * max_wal_senders 的 GUC check_hook
 */
bool
check_max_wal_senders(int *newval, void **extra, GucSource source)
{
	if (MaxConnections + autovacuum_max_workers + 1 +
		max_worker_processes + *newval > MAX_BACKENDS)
		return false;
	return true;
}

/*
 * backend 的早期初始化（独立运行或在 postmaster 下）。甚至
 * 在 InitPostgres 之前发生。
 *
 * 这与 InitPostgres 分开是因为辅助进程也会调用它，例如
 * 写进程可能根本不会调用 InitPostgres。
 * InitPostgres at all.
 */
void
BaseInit(void)
{
	Assert(MyProc != NULL);

	/*
	 * 初始化我们的输入/输出/调试文件描述符
	 */
	DebugFileOpen();

	/*
	 * 初始化文件访问。尽早完成，让其他子系统可以
	 * files.
	 */
	InitFileAccess();

	/*
	 * 初始化统计报告。需要尽早执行，确保 pgstat 的关机
	 * 所有可能产生统计的子系统的关机回调之后运行（
	 * all subsystems that can produce stats (like e.g. transaction commits
	 * can).
	 */
	pgstat_initialize();

	/* 对存储和缓冲区管理器做本地初始化 */
	InitSync();
	smgrinit();
	InitBufferPoolAccess();

	/*
	 * 在 pgstat 之后初始化临时文件访问，让临时文件关机
	 * 报告临时文件统计信息。
	 */
	InitTemporaryFileAccess();

	/*
	 * 初始化 WAL 记录构造的本地缓冲区，以防我们尝试插
	 * try to insert XLOG.
	 */
	InitXLogInsert();

	/*
	 * 在 pgstat 之后初始化复制槽。退出钩子可能需要丢弃
	 * 这又会触发统计报告。
	 */
	ReplicationSlotInitialize();
}


/* --------------------------------
 * InitPostgres
 *		初始化 POSTGRES
 *
 * 参数：
 *	in_dbname, dboid: 指定要连接的数据库，如下所述
 *	username, useroid: 指定要连接的角色，如下所述
 *	load_session_libraries: TRUE 则启用 [session|local]_preload_librarie
 *	override_allow_connections: TRUE 则无视 !datallowconn 连接
 *	out_dbname: 可选输出参数，见下；不用则传 NULL
 *
 * 数据库可以按名称指定（in_dbname 参数）或按 OID 指定（dbo
 * 未用的参数分别传 NULL 或 InvalidOid。如果提供了 dboid，
 * 实际数据库名可通过 out_dbname 返回给调用者。如果 o
 * 非 NULL，必须指向 NAMEDATALEN 大小的缓冲区。
 * NULL, it must point to a buffer of size NAMEDATALEN.
 *
 * （useroid 参数）。
 * or by OID using the useroid parameter.
 *
 * autovacuum launcher 进程也不指定这些参数，因为它只需读到
 * pg_database 就够了；它不连接任何特定数据库。autovacuum
 * worker 指定数据库但不指定用户名；反过来，物理 walsender
 * 指定用户名但不指定数据库。
 * database but not a username; conversely, a physical walsender specifies
 * 按惯例，"交互式"会
 *
 * 注意并行 worker 进程中不应为 true；那些进程有另
 * 来复制其领导者的已加载库集合。
 * background processes such as autovacuum.  Note in particular that it
 * 我们预期 InitProcess() 已被调用，因此已有 PGPROC 结
 * 但尚未完全填充。
 *
 * 注意：
 * InitPostgres 函数中的调用顺序务必非常小心
 *
 * Note:
 *		Be very careful with the order of calls in the InitPostgres function.
 * --------------------------------
 */
void
InitPostgres(const char *in_dbname, Oid dboid,
			 const char *username, Oid useroid,
			 bool load_session_libraries,
			 bool override_allow_connections,
			 char *out_dbname)
{
	bool		bootstrap = IsBootstrapProcessingMode();
	bool		am_superuser;
	char	   *fullpath;
	char		dbname[NAMEDATALEN];
	int			nfree = 0;

	elog(DEBUG3, "InitPostgres");

	/*
	 * 把我的 PGPROC 结构加入 ProcArra
	 *
	 * 一旦做完这件事，我就对其他 backend 可见
	 */
	InitProcessPhase2();

	/*
	 * 在共享失效管理器的每 backend 数据数组中初始化
	 * 
	 *
	 * 设置 MyBackendId（唯一的 backend 标识符
	 */
	MyBackendId = InvalidBackendId;

	SharedInvalBackendInit(false);

	if (MyBackendId > MaxBackends || MyBackendId <= 0)
		elog(FATAL, "bad backend ID: %d", MyBackendId);

	/* 有了 BackendId，就可以参与 ProcSignal 了 */
	ProcSignalInit(MyBackendId);

	/*
	 * 同时设置 backend 操作所需的超时处理器。除引导外
	 * 情况都需要这些。
	 */
	if (!bootstrap)
	{
		RegisterTimeout(DEADLOCK_TIMEOUT, CheckDeadLockAlert);
		RegisterTimeout(STATEMENT_TIMEOUT, StatementTimeoutHandler);
		RegisterTimeout(LOCK_TIMEOUT, LockTimeoutHandler);
		RegisterTimeout(IDLE_IN_TRANSACTION_SESSION_TIMEOUT,
						IdleInTransactionSessionTimeoutHandler);
		RegisterTimeout(IDLE_SESSION_TIMEOUT, IdleSessionTimeoutHandler);
		RegisterTimeout(CLIENT_CONNECTION_CHECK_TIMEOUT, ClientCheckTimeoutHandler);
		RegisterTimeout(IDLE_STATS_UPDATE_TIMEOUT,
						IdleStatsUpdateTimeoutHandler);
	}

	/*
	 * 如果这是引导进程或独立 backend，就启动 XLOG 机制，并
	 * 册在退出时关闭它。其他情况下，启动进程负责启
	 * 检查点器负责关闭它。
	 * XLOG machinery, and the checkpointer for closing it down.
	 */
	if (!IsUnderPostmaster)
	{
		/*
		 * 我们还没有辅助进程资源所有者，但 StartupXLOG 和
		 * ShutdownXLOG 会需要一个。因此创建所述资源所有者
		 * 一个在 ShutdownXLOG 运行后清理的回调）。
		 */
		CreateAuxProcessResourceOwner();

		StartupXLOG();
		/* 释放（并警告）StartupXLOG 中泄漏的缓冲区 pin */
		ReleaseAuxProcessResources(true);
		/* 暂时将 CurrentResourceOwner 重置为空 */
		CurrentResourceOwner = NULL;

		/*
		 * 使用 before_shmem_exit() 使 ShutdownXLOG() 能依赖 DSM 
		 * 正常工作（这又是 pgstats 所需的）。
		 */
		before_shmem_exit(pgstat_before_server_shutdown, 0);
		before_shmem_exit(ShutdownXLOG, 0);
	}

	/*
	 * 初始化关系缓存和系统目录缓存。注意这里不发生目
	 * 我们只设置哈希表结构。必须在开始事务之前做这件
	 * 事务中止会试图碰这些哈希表。
	 * 
	 */
	RelationCacheInitialize();
	InitCatalogCache();
	InitPlanCache();

	/* Initialize portal manager */
	EnablePortalManager();

	/* 初始化状态报告 */
	pgstat_beinit();

	/*
	 * 加载共享系统目录的 relcache 条目。必须至少创建 pg_dat
	 * 和认证所用目录的条目。
	 */
	RelationCacheInitializePhase2();

	/*
	 * 设置进程退出回调做关机前清理。这是我们注册的第
	 * before_shmem_exit 回调之一；因此这将是我们做的最后
	 * 事之一，在缓冲区管理器等低级模块开始关闭之前
	 * 在开始第一个事务之前就把它准备好 —— 如果初
	 * 中失败（完全可能），我们需要 AbortTransaction 调
	 * 
	 * AbortTransaction call to clean up.
	 */
	before_shmem_exit(ShutdownPostgres, 0);

	/* The autovacuum launcher is done here */
	if (IsAutoVacuumLauncherProcess())
	{
		/* 在 PgBackendStatus 数组中报告此 backend */
		pgstat_bestart();

		return;
	}

	/*
	 * 在首次访问数据库前在这里开始新事务并获取快
	 * 没有用处，但我们关心其次级效果：设置 RecentGlob
	 * 这对任何读取堆页面的东西都至关重要，因为 HOT 可能
	 * 修剪它们，即使该进程并不试图修改任何元组。
	 * to prune them even if the process doesn't attempt to modify any
	 * FIXME: 
	 *
	 * 
	 * not pushed/active does not reliably prevent HOT pruning (->xmin could
	 * e.g. be cleared when cache invalidations are processed).
	 */
	if (!bootstrap)
	{
		/* 必须设置 statement_timestamp 超时才能正常工作 */
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();

		/*
		 * transaction_isolation 已被上面设为默认值。如果默
		 * "serializable"且我们处于热备中，不改成更低级
		 * 幸运的是，"read committed" 足够好了。
		 * Fortunately, "read committed" is plenty good enough.
		 */
		XactIsoLevel = XACT_READ_COMMITTED;

		(void) GetTransactionSnapshot();
	}

	/*
	 * 必要时执行客户端认证，然后确定我们的 postgres
	 * 并检查是否为超级用户。
	 *
	 * 独立模式和 autovacuum worker 进程使用固定 ID，否则从
	 * 用户名推算。
	 */
	if (bootstrap || IsAutoVacuumWorkerProcess())
	{
		InitializeSessionUserIdStandalone();
		am_superuser = true;
	}
	else if (!IsUnderPostmaster)
	{
		InitializeSessionUserIdStandalone();
		am_superuser = true;
		if (!ThereIsAtLeastOneRole())
			ereport(WARNING,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("no roles are defined in this database system"),
					 errhint("You should immediately run CREATE USER \"%s\" SUPERUSER;.",
							 username != NULL ? username : "postgres")));
	}
	else if (IsBackgroundWorker)
	{
		if (username == NULL && !OidIsValid(useroid))
		{
			InitializeSessionUserIdStandalone();
			am_superuser = true;
		}
		else
		{
			InitializeSessionUserId(username, useroid);
			am_superuser = superuser();
		}
	}
	else
	{
		/* 常规多用户情况 */
		Assert(MyProcPort != NULL);
		PerformAuthentication(MyProcPort);
		InitializeSessionUserId(username, useroid);
		/* 确保 auth_method 确实有效，即 authn_id 非 NULL */
		if (MyClientConnectionInfo.authn_id)
			InitializeSystemUser(MyClientConnectionInfo.authn_id,
								 hba_authname(MyClientConnectionInfo.auth_method));
		am_superuser = superuser();
	}

	/*
	 * 二进制升级只允许超级用户连接
	 */
	if (IsBinaryUpgrade && !am_superuser)
	{
		ereport(FATAL,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to connect in binary upgrade mode")));
	}

	/*
	 * 最后几个常规连接槽保留给超级用户和具有 pg_use_rese
	 * connections 权限的角色。后台进程不适用这些限制，因
	 * 都有自己的 PGPROC 槽池。
	 * pools of PGPROC slots.
	 *
	 * 空闲槽数是否严格小于保留连接限制。
	 * so we must check whether the number of free slots is strictly less than
	 * the reserved connection limits.
	 */
	if (AmRegularBackendProcess() && !am_superuser &&
		(SuperuserReservedConnections + ReservedConnections) > 0 &&
		!HaveNFreeProcs(SuperuserReservedConnections + ReservedConnections, &nfree))
	{
		if (nfree < SuperuserReservedConnections)
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("remaining connection slots are reserved for roles with the %s attribute",
							"SUPERUSER")));

		if (!has_privs_of_role(GetUserId(), ROLE_PG_USE_RESERVED_CONNECTIONS))
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("remaining connection slots are reserved for roles with privileges of the \"%s\" role",
							"pg_use_reserved_connections")));
	}

	/* 检查 walsender 进程所需的复制权限。 */
	if (am_walsender)
	{
		Assert(!bootstrap);

		if (!has_rolreplication(GetUserId()))
			ereport(FATAL,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to start WAL sender"),
					 errdetail("Only roles with the %s attribute may start a WAL sender process.",
							   "REPLICATION")));
	}

	/*
	 * 如果这是只支持物理复制的纯 walsender，我们不想连
	 * 特定数据库。只需处理启动包中的选项来完成 back
	 * 就完成了。
	 * we're done.
	 */
	if (am_walsender && !am_db_walsender)
	{
		/* 处理启动包中传入的选项 */
		if (MyProcPort != NULL)
			process_startup_options(MyProcPort, am_superuser);

		/* 读完所有选项后尽快应用 PostAuthDelay */
		if (PostAuthDelay > 0)
			pg_usleep(PostAuthDelay * 1000000L);

		/* 初始化客户端编码 */
		InitializeClientEncoding();

		/* 在 PgBackendStatus 数组中报告此 backend */
		pgstat_bestart();

		/* 关闭上面开始的事务 */
		CommitTransactionCommand();

		return;
	}

	/*
	 * 设置保存数据库 ID 和默认表空间的全局变量。注意我
	 * 真正去碰数据库。
	 *
	 * 引导情形走捷径，否则须在 pg_database 中查找该数据库
	 * the db's entry in pg_database.
	 */
	if (bootstrap)
	{
		dboid = Template1DbOid;
		MyDatabaseTableSpace = DEFAULTTABLESPACE_OID;
	}
	else if (in_dbname != NULL)
	{
		HeapTuple	tuple;
		Form_pg_database dbform;

		tuple = GetDatabaseTuple(in_dbname);
		if (!HeapTupleIsValid(tuple))
			ereport(FATAL,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", in_dbname)));
		dbform = (Form_pg_database) GETSTRUCT(tuple);
		dboid = dbform->oid;
	}
	else if (!OidIsValid(dboid))
	{
		/*
		 * 如果这是不绑定任何特定数据库的后台 worker
		 * 后续内容只在绑定到特定数据库时才有意义。返回
		 * 先前开始的事务。
		 * transaction we started before returning.
		 */
		if (!bootstrap)
		{
			pgstat_bestart();
			CommitTransactionCommand();
		}
		return;
	}

	/*
	 * 现在对我们试图连接的数据库加写者锁。如果该数据
	 * DROP DATABASE 在运行，这会阻塞我们直到它完成（并提交
	 * pg_database 的更新）。
	 * pg_database).
	 *
	 * 释放锁之前就会在 ProcArray 中宣告对该数据库的使用（
	 * 正是下一件事）。此后任何尝试 DROP DATABASE 的人
	 * 在数组中看到我们。顺序很重要，因为我们不想在拿
	 * 自己在此数据库中；否则会与 CountOtherDBBackends() 形成
	 * see us in the array once they have the lock.  Ordering is important for
	 * 注意：这里用 RowExclusiveLock 是合理的，因为我们把
	 * 该数据库的并发写者。如果有办法声明会话保证只读
	 * 这类会话用 AccessShareLock，从
	 *
	 * Note: use of RowExclusiveLock here is reasonable because we envision
	 * our session as being a concurrent writer of the database.  If we had a
	 * way of declaring a session as being guaranteed-read-only, we could use
	 * AccessShareLock for such sessions and thereby not conflict against
	 * CREATE DATABASE.
	 */
	if (!bootstrap)
		LockSharedObject(DatabaseRelationId, dboid, 0, RowExclusiveLock);

	/*
	 * 复查 pg_database 确认目标数据库没有消失。如果发生过
	 * DROP DATABASE，这能确保我们干净地死掉而不造成混
	 * cleanly without creating a mess.
	 */
	if (!bootstrap)
	{
		HeapTuple	tuple;
		Form_pg_database datform;

		tuple = GetDatabaseTupleByOid(dboid);
		if (HeapTupleIsValid(tuple))
			datform = (Form_pg_database) GETSTRUCT(tuple);

		if (!HeapTupleIsValid(tuple) ||
			(in_dbname && namestrcmp(&datform->datname, in_dbname)))
		{
			if (in_dbname)
				ereport(FATAL,
						(errcode(ERRCODE_UNDEFINED_DATABASE),
						 errmsg("database \"%s\" does not exist", in_dbname),
						 errdetail("It seems to have just been dropped or renamed.")));
			else
				ereport(FATAL,
						(errcode(ERRCODE_UNDEFINED_DATABASE),
						 errmsg("database %u does not exist", dboid)));
		}

		strlcpy(dbname, NameStr(datform->datname), sizeof(dbname));

		if (database_is_invalid_form(datform))
		{
			ereport(FATAL,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("cannot connect to invalid database \"%s\"", dbname),
					errhint("Use DROP DATABASE to drop invalid databases."));
		}

		MyDatabaseTableSpace = datform->dattablespace;
		/* 把数据库名传回调用者 */
		if (out_dbname)
			strcpy(out_dbname, dbname);
	}

	/*
	 * 复查之后我们确信已连接到数据库，因此可以设置 MyDa
	 * thus can set MyDatabaseId.
	 *
	 * 设置 MyDatabaseId。例如没有此保证时，pgstat_update_dbstats(
	 * 可能在 pgstat 关机回调中为刚被删除的数据库创建条
	 * 迷惑 autovacuum 调度器等其他代码路径。
	 * entries for databases that were just dropped in the pgstat shutdown
	 * callback, which could confuse other code paths like the autovacuum
	 * scheduler.
	 */
	MyDatabaseId = dboid;

	/*
	 * 现在可以在 PGPROC 条目上标记数据库 ID 了
	 *
	 * 我们假定这是原子存储因此无需加锁；实际上即使不
	 * 正常工作。搜索 ProcArray 找此数据库 ID 的人应持有数
	 * 因此不会与此存储并发执行。找另一数据库 ID 的进程
	 * 读到部分更新的 databaseId 值而碰巧匹配；但只要这类
	 * CountOtherDBBackends() 那样等待重试，下次一定能看到正确
	 * a partially-updated databaseId value; but as long as all such searches
	 * wait and retry, as in CountOtherDBBackends(), they will certainly see
	 * the correct value on their next try.
	 */
	MyProc->databaseId = MyDatabaseId;

	/*
	 * 读 pg_authid 和/或 pg_database 时建立了目录快照；但
	 * MyDatabaseId 之前，我们不会响应非共享目录的传入 sinv
	 * 所以不会意识到快照已失效。假定它已不好用了。
	 * if the snapshot has been invalidated.  Assume it's no good anymore.
	 */
	InvalidateCatalogSnapshot();

	/*
	 * 现在应该能安全访问数据库目录了。验证它存在且看
	 * it's there and looks reasonable.
	 */
	fullpath = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);

	if (!bootstrap)
	{
		if (access(fullpath, F_OK) == -1)
		{
			if (errno == ENOENT)
				ereport(FATAL,
						(errcode(ERRCODE_UNDEFINED_DATABASE),
						 errmsg("database \"%s\" does not exist",
								dbname),
						 errdetail("The database subdirectory \"%s\" is missing.",
								   fullpath)));
			else
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("could not access directory \"%s\": %m",
								fullpath)));
		}

		ValidatePgVersion(fullpath);
	}

	SetDatabasePath(fullpath);
	pfree(fullpath);

	/*
	 * 现在可以对系统目录做真正的访问了。
	 *
	 * 加载系统目录的 relcache 条目。必须至少创建最小"
	 * least the minimum set of "nailed-in" cache entries.
	 */
	RelationCacheInitializePhase3();

	/* 设置 ACL 框架（让 CheckMyDatabase 能检查权限） */
	initialize_acl();

	/*
	 * 重新读取我们数据库的 pg_database 行，检查权限并设置
	 * 的 GUC 设置。要等所有数据库访问基础设施就绪后
	 * 它要知道用户是否为超级用户，所以上面的步骤必须
	 * user is a superuser, so the above stuff has to happen first.)
	 */
	if (!bootstrap)
		CheckMyDatabase(dbname, am_superuser, override_allow_connections);

	/*
	 * 现在处理启动包中传入的任何命令行开关和额外的 GU
	 * 之前不能做，因为还不知道客户端是否为超级用户
	 * because we didn't know if client is a superuser.
	 */
	if (MyProcPort != NULL)
		process_startup_options(MyProcPort, am_superuser);

	/* 处理 pg_db_role_setting 选项 */
	process_settings(MyDatabaseId, GetSessionUserId());

	/* 读完所有选项后尽快应用 PostAuthDelay */
	if (PostAuthDelay > 0)
		pg_usleep(PostAuthDelay * 1000000L);

	/*
	 * 初始化选定了活动用户并获得正确 GUC 设置后才能
	 * selected the active user and gotten the right GUC settings.
	 */

	/* set default namespace search path */
	InitializeSearchPath();

	/* 初始化客户端编码 */
	InitializeClientEncoding();

	/* 初始化此 backend 的会话状态。 */
	InitializeSession();

	/*
	 * 如果这是交互式会话，加载应在 backend 启动时预加
	 * 它们由 GUC 决定，这要等 GUC 设置完成才能做，但我
	 * 初始事务期间发生，以防需要做任何数据库访问的事
	 * 
	 * access needs to be done.
	 */
	if (load_session_libraries)
		process_session_preload_libraries();

	/* 在 PgBackendStatus 数组中报告此 backend */
	if (!bootstrap)
		pgstat_bestart();

	/* 关闭上面开始的事务 */
	if (!bootstrap)
		CommitTransactionCommand();
}

/*
 * 处理启动包中传入的任何命令行开关和额外的 GUC 
 * settings passed in the startup packet.
 */
static void
process_startup_options(Port *port, bool am_superuser)
{
	GucContext	gucctx;
	ListCell   *gucopts;

	gucctx = am_superuser ? PGC_SU_BACKEND : PGC_BACKEND;

	/*
	 * 如果是常规 backend，先处理启动包中包含的命令行
	 * startup packet, if we are in a regular backend.
	 */
	if (port->cmdline_options != NULL)
	{
		/*
		 * port->cmdline_options 可能产生的命令行参数最大个
		 * (strlen + 1) / 2；参见 pg_split_opts()。
		 * pg_split_opts().
		 */
		char	  **av;
		int			maxac;
		int			ac;

		maxac = 2 + (strlen(port->cmdline_options) + 1) / 2;

		av = (char **) palloc(maxac * sizeof(char *));
		ac = 0;

		av[ac++] = "postgres";

		pg_split_opts(av, &ac, port->cmdline_options);

		av[ac] = NULL;

		Assert(ac < maxac);

		(void) process_postgres_switches(ac, av, gucctx, NULL);
	}

	/*
	 * 处理启动包中传入的额外 GUC 变量设置。处理方式与
	 * These are handled exactly like command-line variables.
	 */
	gucopts = list_head(port->guc_options);
	while (gucopts)
	{
		char	   *name;
		char	   *value;

		name = lfirst(gucopts);
		gucopts = lnext(port->guc_options, gucopts);

		value = lfirst(gucopts);
		gucopts = lnext(port->guc_options, gucopts);

		SetConfigOption(name, value, gucctx, PGC_S_CLIENT);
	}
}

/*
 * 从 pg_db_role_setting 加载 GUC 设置
 *
 * 我们尝试数据库/角色组合的特定设置，以及该数据库
 * general for this database and for this user.
 */
static void
process_settings(Oid databaseid, Oid roleid)
{
	Relation	relsetting;
	Snapshot	snapshot;

	if (!IsUnderPostmaster)
		return;

	relsetting = table_open(DbRoleSettingRelationId, AccessShareLock);

	/* 为效率在同一快照下读取所有设置 */
	snapshot = RegisterSnapshot(GetCatalogSnapshot(DbRoleSettingRelationId));

	/* Later settings are ignored if set earlier. */
	ApplySetting(snapshot, databaseid, roleid, relsetting, PGC_S_DATABASE_USER);
	ApplySetting(snapshot, InvalidOid, roleid, relsetting, PGC_S_USER);
	ApplySetting(snapshot, databaseid, InvalidOid, relsetting, PGC_S_DATABASE);
	ApplySetting(snapshot, InvalidOid, InvalidOid, relsetting, PGC_S_GLOBAL);

	UnregisterSnapshot(snapshot);
	table_close(relsetting, AccessShareLock);
}

/*
 * backend 关机回调。做我们确信在所有支持模块开始通
 * 关上大门之前发生的清理。
 * their own callbacks.
 *
 * 独立回调完成。我们不合并回调，因为即使用户级清
 * 希望这个回调发生。
 * callbacks because we still want this one to happen if the user-level
 * cleanup fails.
 */
static void
ShutdownPostgres(int code, Datum arg)
{
	/* 确保我们已杀死任何活动事务 */
	AbortOutOfAnyTransaction();

	/*
	 * 用户锁不由事务结束释放，所以务必显式释放。
	 * them explicitly.
	 */
	LockReleaseAll(USER_LOCKMETHOD, true);
}


/*
 * STATEMENT_TIMEOUT 处理器：触发查询取消中断。
 */
static void
StatementTimeoutHandler(void)
{
	int			sig = SIGINT;

	/*
	 * 认证期间超时用于处理 authentication_timeout 
	 * 这类超时而退出。
	 */
	if (ClientAuthInProgress)
		sig = SIGTERM;

#ifdef HAVE_SETSID
	/* 尝试向整个进程组发信号 */
	kill(-MyProcPid, sig);
#endif
	kill(MyProcPid, sig);
}

/*
 * LOCK_TIMEOUT 处理器：触发查询取消中断。
 */
static void
LockTimeoutHandler(void)
{
#ifdef HAVE_SETSID
	/* 尝试向整个进程组发信号 */
	kill(-MyProcPid, SIGINT);
#endif
	kill(MyProcPid, SIGINT);
}

static void
IdleInTransactionSessionTimeoutHandler(void)
{
	IdleInTransactionSessionTimeoutPending = true;
	InterruptPending = true;
	SetLatch(MyLatch);
}

static void
IdleSessionTimeoutHandler(void)
{
	IdleSessionTimeoutPending = true;
	InterruptPending = true;
	SetLatch(MyLatch);
}

static void
IdleStatsUpdateTimeoutHandler(void)
{
	IdleStatsUpdateTimeoutPending = true;
	InterruptPending = true;
	SetLatch(MyLatch);
}

static void
ClientCheckTimeoutHandler(void)
{
	CheckClientConnectionPending = true;
	InterruptPending = true;
	SetLatch(MyLatch);
}

/*
 * 如果此数据库集群中至少定义了一个角色则返回真。
 */
static bool
ThereIsAtLeastOneRole(void)
{
	Relation	pg_authid_rel;
	TableScanDesc scan;
	bool		result;

	pg_authid_rel = table_open(AuthIdRelationId, AccessShareLock);

	scan = table_beginscan_catalog(pg_authid_rel, 0, NULL);
	result = (heap_getnext(scan, ForwardScanDirection) != NULL);

	table_endscan(scan);
	table_close(pg_authid_rel, AccessShareLock);

	return result;
}
