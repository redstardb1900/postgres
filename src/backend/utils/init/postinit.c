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
 * 本函数用于 backend 启动时尚无法访问系统目录的场合。
 * 最坏情形下，我们可以仅凭 relcache.c 为 pg_database
 * 创建的硬编码描述符做顺序扫描。较典型情形下，
 * relcache.c 能从共享 relcache 缓存文件加载 pg_database
 * 及其索引的描述符，因此我们可以做索引扫描。
 * criticalSharedRelcachesBuilt 表明我们是否拿到了缓存的
 * 描述符。
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
	/* 本应已设置，但再确认一次 */
	ClientAuthInProgress = true;	/* 限制日志消息的可见性 */

	/*
	 * 在 EXEC_BACKEND 情况下，我们没有从 postmaster 继承
	 * pg_hba.conf 等内容，必须自己加载。
	 *
	 * FIXME: [fork/exec] 唔。有办法避免这种开销吗？
	 */
#ifdef EXEC_BACKEND

	/*
	 * load_hba() 与 load_ident() 想在 PostmasterContext 之内
	 * 工作，不存在就创建它（不会存在）。稍后我们会再
	 * 在 PostgresMain 中删除。
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
		 * 加载 IDENT 文件失败也可以继续，尽管这
		 * 意味着无法用任何需要用户名映射的认证
		 * 方法登录。load_ident() 已把错误详情记录到
		 * 日志中。
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
	ClientAuthentication(port); /* 失败时可能不返回 */

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

	/* 正常经 syscache 取我们的 pg_database 行 */
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
		 * 检查此数据库的连接上限。该限制只对常规
		 * backend 强制执行，因为其他进程类型有各自的
		 * PGPROC 池。
		 *
		 * 这里存在竞态条件 --- 我们先创建自己的 PGPROC
		 * 再去检查其他 PGPROC。若两个 backend 几乎同时
		 * 这么做，它们可能都认为自己超限，而理想
		 * 情况应是一个成功一个失败。要把这做得
		 * 完全正确似乎得不偿失；因此我们只在文档
		 * 中说明连接上限只是近似值。
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
	/* 若无其他 client_encoding 来源，则用服务器编码 */
	SetConfigOption("client_encoding", GetDatabaseEncodingName(),
					PGC_BACKEND, PGC_S_DYNAMIC_DEFAULT);

	/* 赋值 locale 变量 */
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
	 * 默认区域设置目前总是确定性的。非确定性区域
	 * 设置目前不支持模式匹配，若全局应用会破坏
	 * 很多东西。
	 */
	default_locale.deterministic = true;

	/*
	 * 检查排序规则版本。参见
	 * pg_newlocale_from_collation() 中的类似代码。注意这里
	 * 任何情况下都只警告，不阻碍连接。
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
 * 调用者负责确保 argv 数组足够大。
 * 本例程最多可添加的参数个数上限为
 * (strlen(optstr) + 1) / 2
 *
 * 因为某些选项值可能包含空格，我们允许用反斜杠
 * 转义，\\ 表示一个字面反斜杠。
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
 * backend 的早期初始化（独立运行或在 postmaster 之下）。
 * 甚至发生在 InitPostgres 之前。
 *
 * 这与 InitPostgres 分开是因为辅助进程也会调用它，例如
 * 后台写进程可能根本不会调用
 * InitPostgres。
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
	 * 初始化统计报告。这需要尽早执行，确保 pgstat
	 * 的关机回调在所有可能产生统计的子系统的关机
	 * 回调之后运行（例如事务提交这类子系统就属
	 * 于此）。
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
	 * 初始化 WAL 记录构造的本地缓冲区，以防我们哪天
	 * 去插入 XLOG。
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
 *	in_dbname, dboid: 指定要连接的数据库，见下文
 *	username 和 useroid: 指定要连接的角色，见下文
 *	load_session_libraries: TRUE 启用 [session|local]_preload_libraries
 *	override_allow_connections: TRUE 则无视 !datallowconn 连接
 *	out_dbname: 可选输出参数，见下文；不用则传 NULL
 *
 * 数据库可用 in_dbname 参数按名称指定，或用 dboid 参数
 * 按 OID 指定。未用的参数分别传 NULL 或 InvalidOid。
 * 若提供了 dboid，实际数据库名可通过 out_dbname 返回
 * 给调用者。若 out_dbname 非 NULL，它必须指向
 * NAMEDATALEN 大小的缓冲区。
 *
 * 同样，角色可用 username 参数按名称传入，或用
 * useroid 参数按 OID 传入。
 *
 * 引导模式下数据库与用户名参数为 NULL/InvalidOid。
 * autovacuum launcher 进程也不指定这些参数，因为它只
 * 进行到能读取 pg_database 的程度为止；它不连接任何
 * 特定数据库。autovacuum worker 指定数据库但不指
 * 定用户名；反过来，物理 walsender 指定用户名但不
 * 指定数据库。
 *
 * 按惯例，load_session_libraries 在"交互式"会话（含独
 * 立 backend）中应传 true，而在 autovacuum 等后台进程
 * 中应传 false。特别注意它在并行 worker 进程中不应
 * 为 true；那些进程有另一套机制来复制其领导者的
 * 已加载库集合。
 *
 * 我们预期 InitProcess() 已被调用，因此我们已有 PGPROC
 * 结构……但尚未完全填充。
 *
 * 注意：
 *		务必小心 InitPostgres 函数中的调用顺序。
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
	 * 如果这是引导进程或独立 backend，就启动 XLOG
	 * 机制，并注册在退出时关闭它。在其他情况下，
	 * 由启动进程负责启动 XLOG 机制，由检查点器负
	 * 责关闭它。
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

	/* 初始化门户管理器 */
	EnablePortalManager();

	/* 初始化状态报告 */
	pgstat_beinit();

	/*
	 * 加载共享系统目录的 relcache 条目。必须至少创建 pg_dat
	 * 和认证所用目录的条目。
	 */
	RelationCacheInitializePhase2();

	/*
	 * 设置进程退出回调做关机前清理。这是我们注册的
	 * 最早的 before_shmem_exit 回调之一；因此这将是我们
	 * 在缓冲区管理器等低级模块开始关闭之前所做的
	 * 最后几件事之一。必须在开始第一个事务之前把
	 * 它准备就绪。如果我们在初始化事务期间失败
	 * （这完全可能），就需要 AbortTransaction 调用来清
	 * 理。
	 */
	before_shmem_exit(ShutdownPostgres, 0);

	/* autovacuum launcher 在此处完成 */
	if (IsAutoVacuumLauncherProcess())
	{
		/* 在 PgBackendStatus 数组中报告此 backend */
		pgstat_bestart();

		return;
	}

	/*
	 * 在首次访问数据库前在此开始新事务并获取快
	 * 照。快照本身并无用处，但我们关心它的次级
	 * 效果：它会设置 RecentGlobalXmin（这对任何读取堆页
	 * 面的东西都至关重要，因为 HOT 可能决定修剪它们，
	 * 即使该进程并不试图修改任何
	 * 元组）。
	 *
	 * FIXME: 此注释不准确/代码有缺陷。未被推入/激活的快
	 * 照并不能可靠地阻止 HOT 修剪（例如处理缓存失效时
	 * ->xmin 就可能被清除）。
	 */
	if (!bootstrap)
	{
		/* 必须设置 statement_timestamp 超时才能正常工作 */
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();

		/*
		 * transaction_isolation 已被上面设为默认值。如果默
		 * 认值是 "serializable"，且处于热备中，不改成
		 * 更低级别就会失败。幸运的是，
		 * "读已提交"就足够好了。
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
	 * 最后几个常规连接槽保留给超级用户和具有
	 * pg_use_reserved_connections 权限的角色。这些限制不
	 * 适用于后台进程，因为它们都有各自的 PGPROC
	 * 槽池。
	 *
	 * 注意：此时新 backend 已领取了 proc 结构，所以我们
	 * 必须检查空闲槽的数量是否严格小于
	 * 保留连接限制。
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
	 * 如果这是只支持物理复制的纯 walsender，我们不想
	 * 连接任何特定数据库。只需处理启动包的选项
	 * 来完成 backend 启动，到此就全部完成
	 * 了。
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
	 * 们此刻还不会真正去碰这个数据库。
	 *
	 * 引导情形走捷径，否则我们必须查找该数据库在
	 * pg_database 中的条目。
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
		 * 如果这是未绑定特定数据库的后台 worker，
		 * 我们现在就完成了。后续内容只在绑定到特定数
		 * 据库时才有意义。但返回前需关闭先前开始的
		 * 事务。
		 */
		if (!bootstrap)
		{
			pgstat_bestart();
			CommitTransactionCommand();
		}
		return;
	}

	/*
	 * 现在，对我们试图连接的数据库取写者锁。如果
	 * 该数据库上正有 DROP DATABASE 在运行，这会阻塞
	 * 我们直到它完成（并提交其对 pg_database 的更新）
	 * 为止。
	 *
	 * 注意该锁持有时间不长，只到本次启动事务结束
	 * 为止。这没问题，因为我们会在释放锁之前在
	 * ProcArray 中宣告我们对该数据库的使用（实际上，
	 * 这正是接下来要做的事）。此点之后任何尝试 DROP
	 * DATABASE 的人一旦拿到锁就会在数组中看到我们。
	 * 这个顺序很重要，因为我们不想在拿到锁之前
	 * 就宣告自己位于此数据库中；否则我们实际上会
	 * 与 CountOtherDBBackends() 死锁。
	 *
	 * 注意：在这里使用 RowExclusiveLock 是合理的，因为
	 * 我们预期本会话是该数据库的一个并发写者。如果
	 * 我们有办法声明某会话保证只读，那么可以对此类
	 * 会话使用 AccessShareLock，从而不与 CREATE DATABASE
	 * 冲突。
	 */
	if (!bootstrap)
		LockSharedObject(DatabaseRelationId, dboid, 0, RowExclusiveLock);

	/*
	 * 复查 pg_database 以确认目标数据库尚未消失。如果发
	 * 生过并发的 DROP DATABASE，这能确保我们干净地死
	 * 去而不留下一团糟。
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
	 * 复查之后我们确信已连接到某个数据库，因此可以
	 * 设置 MyDatabaseId。
	 *
	 * 重要的是，只有在确信目标数据库不再可能被并发
	 * 删除或重命名之后才设置 MyDatabaseId。例如，若没有
	 * 这一保证，pgstat_update_dbstats() 可能会在 pgstat 关机
	 * 回调中为刚被删除的数据库创建条目，这可能使诸
	 * 如 autovacuum 调度器等其他代码路径陷
	 * 于混乱。
	 */
	MyDatabaseId = dboid;

	/*
	 * 现在可以在 PGPROC 条目上标记数据库 ID 了
	 *
	 * 我们假定这是原子存储，因而无需加锁；实际上
	 * 即使不是原子操作也能正常工作。任何在
	 * ProcArray 中搜索此数据库 ID 的人都应持有数据库锁，
	 * 因此不会与此存储并发执行。查找另一个数据
	 * 库 ID 的进程理论上可能因读到部分更新的
	 * databaseId 值而碰巧匹配；但只要所有这类搜索都像
	 * CountOtherDBBackends() 那样等待并重试，它们下一次尝试
	 * 时就必定能看到正确的值。
	 */
	MyProc->databaseId = MyDatabaseId;

	/*
	 * 读 pg_authid 或 pg_database 时建立了目录快照；但
	 * 在 MyDatabaseId 就绪之前，我们不会响应非共享目录
	 * 入 sinval 消息，因而不会察觉快照已失效。就当它
	 * 已不能再用了。
	 */
	InvalidateCatalogSnapshot();

	/*
	 * 现在应能安全访问数据库目录了。请验证
	 * 它存在且看起来正常。
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
	 * 加载系统目录的 relcache 条目。这必须至少创建最小
	 * "钉入"缓存条目集合。
	 */
	RelationCacheInitializePhase3();

	/* 设置 ACL 框架（让 CheckMyDatabase 能检查权限） */
	initialize_acl();

	/*
	 * 重新读取本数据库的 pg_database 行，检查权限并设
	 * 置数据库专属 GUC 设置。需等全部数据库访问基础
	 * 施就绪之后才能这样做。（另外，它想知道用户是否
	 * 是超级用户，所以上面那些步骤必须先行。）
	 */
	if (!bootstrap)
		CheckMyDatabase(dbname, am_superuser, override_allow_connections);

	/*
	 * 现在处理启动包中传入的任何命令行开关和额外的
	 * GUC 变量设置。之前不能这样做，因为还不知
	 * 道客户端是否为超级用户。
	 */
	if (MyProcPort != NULL)
		process_startup_options(MyProcPort, am_superuser);

	/* 处理 pg_db_role_setting 选项 */
	process_settings(MyDatabaseId, GetSessionUserId());

	/* 读完所有选项后尽快应用 PostAuthDelay */
	if (PostAuthDelay > 0)
		pg_usleep(PostAuthDelay * 1000000L);

	/*
	 * 初始化各种默认状态，要等选定活动用户并取得
	 * 正确的 GUC 设置之后才能进行。
	 */

	/* 设默认命名空间搜索路径 */
	InitializeSearchPath();

	/* 初始化客户端编码 */
	InitializeClientEncoding();

	/* 初始化此 backend 的会话状态。 */
	InitializeSession();

	/*
	 * 如果这是交互式会话，加载应在 backend 启动时预
	 * 先加载的库。由于它们由 GUC 决定，这要等 GUC 设置
	 * 完成后才能进行，但我们希望它发生在初始事务
	 * 期间，以防有任何需要数据库访问的事情需要
	 * 处理。
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
 * 处理启动包传入的任何命令行开关和额外 GUC
 * 变量设置。
 */
static void
process_startup_options(Port *port, bool am_superuser)
{
	GucContext	gucctx;
	ListCell   *gucopts;

	gucctx = am_superuser ? PGC_SU_BACKEND : PGC_BACKEND;

	/*
	 * 如果是常规 backend，先处理启动包所含的命令行
	 * 开关。
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
	 * 处理启动包中传入的任何额外 GUC 变量设置。它们的
	 * 处理方式与命令行变量完全相同。
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
 * 我们尝试数据库/角色组合的特定设置，以及此数据
 * 库和此用户的一般设置。
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

	/* 后面的设置若已设置过则被忽略 */
	ApplySetting(snapshot, databaseid, roleid, relsetting, PGC_S_DATABASE_USER);
	ApplySetting(snapshot, InvalidOid, roleid, relsetting, PGC_S_USER);
	ApplySetting(snapshot, databaseid, InvalidOid, relsetting, PGC_S_DATABASE);
	ApplySetting(snapshot, InvalidOid, InvalidOid, relsetting, PGC_S_GLOBAL);

	UnregisterSnapshot(snapshot);
	table_close(relsetting, AccessShareLock);
}

/*
 * backend 关机回调。做我们确信要在各支持模块
 * 都借它们自己的回调把大门关上之前
 * 必做的清理。
 *
 * 用户级清理（如临时关系移除和 UNLISTEN）通过单独的
 * 回调执行，且先于本回调。我们不合并这些回调，
 * 因为即使用户级清理失败，我们仍希望本回
 * 调发生。
 */
static void
ShutdownPostgres(int code, Datum arg)
{
	/* 确保我们已杀死任何活动事务 */
	AbortOutOfAnyTransaction();

	/*
	 * 用户锁不由事务结束释放，所以务必显式地释放
	 * 它们。
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
