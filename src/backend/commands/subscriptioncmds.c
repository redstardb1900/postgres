/*-------------------------------------------------------------------------
 *
 * subscriptioncmds.c
 *		subscription catalog manipulation functions
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/commands/subscriptioncmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid_d.h"
#include "catalog/pg_database_d.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/subscriptioncmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "replication/origin.h"
#include "replication/slot.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "replication/worker_internal.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/syscache.h"

/*
  * 用户可在 CREATE/ALTER SUBSCRIPTION 命令中指定的选项。
  *
 */
#define SUBOPT_CONNECT				0x00000001
#define SUBOPT_ENABLED				0x00000002
#define SUBOPT_CREATE_SLOT			0x00000004
#define SUBOPT_SLOT_NAME			0x00000008
#define SUBOPT_COPY_DATA			0x00000010
#define SUBOPT_SYNCHRONOUS_COMMIT	0x00000020
#define SUBOPT_REFRESH				0x00000040
#define SUBOPT_BINARY				0x00000080
#define SUBOPT_STREAMING			0x00000100
#define SUBOPT_TWOPHASE_COMMIT		0x00000200
#define SUBOPT_DISABLE_ON_ERR		0x00000400
#define SUBOPT_PASSWORD_REQUIRED	0x00000800
#define SUBOPT_RUN_AS_OWNER			0x00001000
#define SUBOPT_LSN					0x00002000
#define SUBOPT_ORIGIN				0x00004000

/* check if the 'val' has 'bits' set */
#define IsSet(val, bits)  (((val) & (bits)) == (bits))

/*
  * 保存用户提供的 CREATE/ALTER SUBSCRIPTION 命令选项位图
  * 及每个选项解析/默认值的结构体。
 */
typedef struct SubOpts
{
	bits32		specified_opts;
	char	   *slot_name;
	char	   *synchronous_commit;
	bool		connect;
	bool		enabled;
	bool		create_slot;
	bool		copy_data;
	bool		refresh;
	bool		binary;
	char		streaming;
	bool		twophase;
	bool		disableonerr;
	bool		passwordrequired;
	bool		runasowner;
	char	   *origin;
	XLogRecPtr	lsn;
} SubOpts;

static List *fetch_table_list(WalReceiverConn *wrconn, List *publications);
static void check_publications_origin(WalReceiverConn *wrconn,
									  List *publications, bool copydata,
									  char *origin, Oid *subrel_local_oids,
									  int subrel_count, char *subname);
static void check_duplicates_in_publist(List *publist, Datum *datums);
static List *merge_publications(List *oldpublist, List *newpublist, bool addpub, const char *subname);
static void ReportSlotConnectionError(List *rstates, Oid subid, char *slotname, char *err);


/*
  * CREATE 和 ALTER SUBSCRIPTION 命令的通用选项解析函数。
  *
  * 并非所有选项都同时适用于两个命令，此函数
  * 会在互斥选项被指定时报错。
 */
static void
parse_subscription_options(ParseState *pstate, List *stmt_options,
						   bits32 supported_opts, SubOpts *opts)
{
	ListCell   *lc;

	/* 从清除 opts 开始。 */
	memset(opts, 0, sizeof(SubOpts));

	/* 调用者必须预期某些选项 */
	Assert(supported_opts != 0);

	/* 若支持 connect 选项，其他选项也需支持。 */
	Assert(!IsSet(supported_opts, SUBOPT_CONNECT) ||
		   IsSet(supported_opts, SUBOPT_ENABLED | SUBOPT_CREATE_SLOT |
				 SUBOPT_COPY_DATA));

	/* 设置受支持选项的默认值。 */
	if (IsSet(supported_opts, SUBOPT_CONNECT))
		opts->connect = true;
	if (IsSet(supported_opts, SUBOPT_ENABLED))
		opts->enabled = true;
	if (IsSet(supported_opts, SUBOPT_CREATE_SLOT))
		opts->create_slot = true;
	if (IsSet(supported_opts, SUBOPT_COPY_DATA))
		opts->copy_data = true;
	if (IsSet(supported_opts, SUBOPT_REFRESH))
		opts->refresh = true;
	if (IsSet(supported_opts, SUBOPT_BINARY))
		opts->binary = false;
	if (IsSet(supported_opts, SUBOPT_STREAMING))
		opts->streaming = LOGICALREP_STREAM_OFF;
	if (IsSet(supported_opts, SUBOPT_TWOPHASE_COMMIT))
		opts->twophase = false;
	if (IsSet(supported_opts, SUBOPT_DISABLE_ON_ERR))
		opts->disableonerr = false;
	if (IsSet(supported_opts, SUBOPT_PASSWORD_REQUIRED))
		opts->passwordrequired = true;
	if (IsSet(supported_opts, SUBOPT_RUN_AS_OWNER))
		opts->runasowner = false;
	if (IsSet(supported_opts, SUBOPT_ORIGIN))
		opts->origin = pstrdup(LOGICALREP_ORIGIN_ANY);

	/* 解析选项 */
	foreach(lc, stmt_options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (IsSet(supported_opts, SUBOPT_CONNECT) &&
			strcmp(defel->defname, "connect") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_CONNECT))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_CONNECT;
			opts->connect = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_ENABLED) &&
				 strcmp(defel->defname, "enabled") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_ENABLED))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_ENABLED;
			opts->enabled = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_CREATE_SLOT) &&
				 strcmp(defel->defname, "create_slot") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_CREATE_SLOT))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_CREATE_SLOT;
			opts->create_slot = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_SLOT_NAME) &&
				 strcmp(defel->defname, "slot_name") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_SLOT_NAME))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_SLOT_NAME;
			opts->slot_name = defGetString(defel);

			/* Setting slot_name = NONE is treated as no slot name. */
			if (strcmp(opts->slot_name, "none") == 0)
				opts->slot_name = NULL;
			else
				ReplicationSlotValidateName(opts->slot_name, ERROR);
		}
		else if (IsSet(supported_opts, SUBOPT_COPY_DATA) &&
				 strcmp(defel->defname, "copy_data") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_COPY_DATA))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_COPY_DATA;
			opts->copy_data = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_SYNCHRONOUS_COMMIT) &&
				 strcmp(defel->defname, "synchronous_commit") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_SYNCHRONOUS_COMMIT))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_SYNCHRONOUS_COMMIT;
			opts->synchronous_commit = defGetString(defel);

			/* Test if the given value is valid for synchronous_commit GUC. */
			(void) set_config_option("synchronous_commit", opts->synchronous_commit,
									 PGC_BACKEND, PGC_S_TEST, GUC_ACTION_SET,
									 false, 0, false);
		}
		else if (IsSet(supported_opts, SUBOPT_REFRESH) &&
				 strcmp(defel->defname, "refresh") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_REFRESH))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_REFRESH;
			opts->refresh = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_BINARY) &&
				 strcmp(defel->defname, "binary") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_BINARY))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_BINARY;
			opts->binary = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_STREAMING) &&
				 strcmp(defel->defname, "streaming") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_STREAMING))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_STREAMING;
			opts->streaming = defGetStreamingMode(defel);
		}
		else if (strcmp(defel->defname, "two_phase") == 0)
		{
			/*
			  * 不允许切换 two_phase 选项。这可能导致
			  * 事务丢失并造成副本不一致。
			  * 参见 worker.c 顶部的注释。
			  *
			  * 注意：unsupported twophase 表明此调用来自
			  * AlterSubscription。
			 */
			if (!IsSet(supported_opts, SUBOPT_TWOPHASE_COMMIT))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized subscription parameter: \"%s\"", defel->defname)));

			if (IsSet(opts->specified_opts, SUBOPT_TWOPHASE_COMMIT))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_TWOPHASE_COMMIT;
			opts->twophase = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_DISABLE_ON_ERR) &&
				 strcmp(defel->defname, "disable_on_error") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_DISABLE_ON_ERR))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_DISABLE_ON_ERR;
			opts->disableonerr = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_PASSWORD_REQUIRED) &&
				 strcmp(defel->defname, "password_required") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_PASSWORD_REQUIRED))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_PASSWORD_REQUIRED;
			opts->passwordrequired = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_RUN_AS_OWNER) &&
				 strcmp(defel->defname, "run_as_owner") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_RUN_AS_OWNER))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_RUN_AS_OWNER;
			opts->runasowner = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_ORIGIN) &&
				 strcmp(defel->defname, "origin") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_ORIGIN))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_ORIGIN;
			pfree(opts->origin);

			/*
			  * 尽管 "origin" 参数仅允许 "none" 和 "any" 值，
			  * 但它实现为字符串类型，以便未来版本可将
			  * 该参数扩展为支持按用户指定的源名称过滤。
			  *
			 */
			opts->origin = defGetString(defel);

			if ((pg_strcasecmp(opts->origin, LOGICALREP_ORIGIN_NONE) != 0) &&
				(pg_strcasecmp(opts->origin, LOGICALREP_ORIGIN_ANY) != 0))
				ereport(ERROR,
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("unrecognized origin value: \"%s\"", opts->origin));
		}
		else if (IsSet(supported_opts, SUBOPT_LSN) &&
				 strcmp(defel->defname, "lsn") == 0)
		{
			char	   *lsn_str = defGetString(defel);
			XLogRecPtr	lsn;

			if (IsSet(opts->specified_opts, SUBOPT_LSN))
				errorConflictingDefElem(defel, pstate);

			/* Setting lsn = NONE is treated as resetting LSN */
			if (strcmp(lsn_str, "none") == 0)
				lsn = InvalidXLogRecPtr;
			else
			{
				/* Parse the argument as LSN */
				lsn = DatumGetLSN(DirectFunctionCall1(pg_lsn_in,
													  CStringGetDatum(lsn_str)));

				if (XLogRecPtrIsInvalid(lsn))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid WAL location (LSN): %s", lsn_str)));
			}

			opts->specified_opts |= SUBOPT_LSN;
			opts->lsn = lsn;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized subscription parameter: \"%s\"", defel->defname)));
	}

	/*
	  * 显式要求不连接，需要一些额外处理。
	  *
	 */
	if (!opts->connect && IsSet(supported_opts, SUBOPT_CONNECT))
	{
		/* Check for incompatible options from the user. */
		if (opts->enabled &&
			IsSet(opts->specified_opts, SUBOPT_ENABLED))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			/*- translator: both %s are strings of the form "option = value" */
					 errmsg("%s and %s are mutually exclusive options",
							"connect = false", "enabled = true")));

		if (opts->create_slot &&
			IsSet(opts->specified_opts, SUBOPT_CREATE_SLOT))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s and %s are mutually exclusive options",
							"connect = false", "create_slot = true")));

		if (opts->copy_data &&
			IsSet(opts->specified_opts, SUBOPT_COPY_DATA))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s and %s are mutually exclusive options",
							"connect = false", "copy_data = true")));

		/* Change the defaults of other options. */
		opts->enabled = false;
		opts->create_slot = false;
		opts->copy_data = false;
	}

	/*
	  * 对 slot_name = NONE 时不允许的组合进行额外检查。
	  *
	 */
	if (!opts->slot_name &&
		IsSet(opts->specified_opts, SUBOPT_SLOT_NAME))
	{
		if (opts->enabled)
		{
			if (IsSet(opts->specified_opts, SUBOPT_ENABLED))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				/*- translator: both %s are strings of the form "option = value" */
						 errmsg("%s and %s are mutually exclusive options",
								"slot_name = NONE", "enabled = true")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				/*- translator: both %s are strings of the form "option = value" */
						 errmsg("subscription with %s must also set %s",
								"slot_name = NONE", "enabled = false")));
		}

		if (opts->create_slot)
		{
			if (IsSet(opts->specified_opts, SUBOPT_CREATE_SLOT))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				/*- translator: both %s are strings of the form "option = value" */
						 errmsg("%s and %s are mutually exclusive options",
								"slot_name = NONE", "create_slot = true")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				/*- translator: both %s are strings of the form "option = value" */
						 errmsg("subscription with %s must also set %s",
								"slot_name = NONE", "create_slot = false")));
		}
	}
}

/*
  * 将发布名列表添加到字符串。
 */
static void
get_publications_str(List *publications, StringInfo dest, bool quote_literal)
{
	ListCell   *lc;
	bool		first = true;

	Assert(publications != NIL);

	foreach(lc, publications)
	{
		char	   *pubname = strVal(lfirst(lc));

		if (first)
			first = false;
		else
			appendStringInfoString(dest, ", ");

		if (quote_literal)
			appendStringInfoString(dest, quote_literal_cstr(pubname));
		else
		{
			appendStringInfoChar(dest, '"');
			appendStringInfoString(dest, pubname);
			appendStringInfoChar(dest, '"');
		}
	}
}

/*
  * 检查指定发布在发布者上是否存在。
 */
static void
check_publications(WalReceiverConn *wrconn, List *publications)
{
	WalRcvExecResult *res;
	StringInfo	cmd;
	TupleTableSlot *slot;
	List	   *publicationsCopy = NIL;
	Oid			tableRow[1] = {TEXTOID};

	cmd = makeStringInfo();
	appendStringInfoString(cmd, "SELECT t.pubname FROM\n"
						   " pg_catalog.pg_publication t WHERE\n"
						   " t.pubname IN (");
	get_publications_str(publications, cmd, true);
	appendStringInfoChar(cmd, ')');

	res = walrcv_exec(wrconn, cmd->data, 1, tableRow);
	pfree(cmd->data);
	pfree(cmd);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				errmsg("could not receive list of publications from the publisher: %s",
					   res->err));

	publicationsCopy = list_copy(publications);

	/* 处理发布。 */
	slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
	{
		char	   *pubname;
		bool		isnull;

		pubname = TextDatumGetCString(slot_getattr(slot, 1, &isnull));
		Assert(!isnull);

		/* Delete the publication present in publisher from the list. */
		publicationsCopy = list_delete(publicationsCopy, makeString(pubname));
		ExecClearTuple(slot);
	}

	ExecDropSingleTupleTableSlot(slot);

	walrcv_clear_result(res);

	if (list_length(publicationsCopy))
	{
		/* Prepare the list of non-existent publication(s) for error message. */
		StringInfo	pubnames = makeStringInfo();

		get_publications_str(publicationsCopy, pubnames, false);
		ereport(WARNING,
				errcode(ERRCODE_UNDEFINED_OBJECT),
				errmsg_plural("publication %s does not exist on the publisher",
							  "publications %s do not exist on the publisher",
							  list_length(publicationsCopy),
							  pubnames->data));
	}
}

/*
  * 将发布列表转换为 text 数组的辅助函数。
 */
static Datum
publicationListToArray(List *publist)
{
	ArrayType  *arr;
	Datum	   *datums;
	MemoryContext memcxt;
	MemoryContext oldcxt;

	/* 为临时分配创建内存上下文。 */
	memcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "publicationListToArray to array",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(memcxt);

	datums = (Datum *) palloc(sizeof(Datum) * list_length(publist));

	check_duplicates_in_publist(publist, datums);

	MemoryContextSwitchTo(oldcxt);

	arr = construct_array_builtin(datums, list_length(publist), TEXTOID);

	MemoryContextDelete(memcxt);

	return PointerGetDatum(arr);
}

/*
  * 创建新订阅。
 */
ObjectAddress
CreateSubscription(ParseState *pstate, CreateSubscriptionStmt *stmt,
				   bool isTopLevel)
{
	Relation	rel;
	ObjectAddress myself;
	Oid			subid;
	bool		nulls[Natts_pg_subscription];
	Datum		values[Natts_pg_subscription];
	Oid			owner = GetUserId();
	HeapTuple	tup;
	char	   *conninfo;
	char		originname[NAMEDATALEN];
	List	   *publications;
	bits32		supported_opts;
	SubOpts		opts = {0};
	AclResult	aclresult;

	/*
	  * 解析并检查选项。
	  *
	  * 连接和发布不应在此指定。
	 */
	supported_opts = (SUBOPT_CONNECT | SUBOPT_ENABLED | SUBOPT_CREATE_SLOT |
					  SUBOPT_SLOT_NAME | SUBOPT_COPY_DATA |
					  SUBOPT_SYNCHRONOUS_COMMIT | SUBOPT_BINARY |
					  SUBOPT_STREAMING | SUBOPT_TWOPHASE_COMMIT |
					  SUBOPT_DISABLE_ON_ERR | SUBOPT_PASSWORD_REQUIRED |
					  SUBOPT_RUN_AS_OWNER | SUBOPT_ORIGIN);
	parse_subscription_options(pstate, stmt->options, supported_opts, &opts);

	/*
	  * 由于创建复制槽是非事务性的，回滚事务时
	  * 已创建的复制槽仍会保留。因此若需创建复制槽，
	  * 不能在事务块内执行 CREATE SUBSCRIPTION。
	  *
	 */
	if (opts.create_slot)
		PreventInTransactionBlock(isTopLevel, "CREATE SUBSCRIPTION ... WITH (create_slot = true)");

	/*
	  * 不希望非特权用户能够触发任意网络目标的
	  * 连接尝试，故要求用户被明确授权创建订阅。
	  *
	 */
	if (!has_privs_of_role(owner, ROLE_PG_CREATE_SUBSCRIPTION))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create subscription"),
				 errdetail("Only roles with privileges of the \"%s\" role may create subscriptions.",
						   "pg_create_subscription")));

	/*
	  * 由于订阅是数据库对象，还需检查对数据库的
	  * CREATE 权限。
	 */
	aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId,
								owner, ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));

	/*
	  * 非超级用户必须设置密码进行认证，且目标服务器
	  * 必须使用该密码，但超级用户可豁免此要求。
	  *
	 */
	if (!opts.passwordrequired && !superuser_arg(owner))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("password_required=false is superuser-only"),
				 errhint("Subscriptions with the password_required option set to false may only be created or modified by the superuser.")));

	/*
	  * 若以适当开关编译，当回归测试的订阅命名规范
	  * 被违反时发出警告。
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(stmt->subname, "regress_", 8) != 0)
		elog(WARNING, "subscriptions created by regression test cases should have names starting with \"regress_\"");
#endif

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	/* 检查名称是否已使用 */
	subid = GetSysCacheOid2(SUBSCRIPTIONNAME, Anum_pg_subscription_oid,
							MyDatabaseId, CStringGetDatum(stmt->subname));
	if (OidIsValid(subid))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("subscription \"%s\" already exists",
						stmt->subname)));
	}

	if (!IsSet(opts.specified_opts, SUBOPT_SLOT_NAME) &&
		opts.slot_name == NULL)
		opts.slot_name = stmt->subname;

	/* 订阅的 synchronous_commit 默认为 off。 */
	if (opts.synchronous_commit == NULL)
		opts.synchronous_commit = "off";

	conninfo = stmt->conninfo;
	publications = stmt->publication;

	/* 加载提供 libpq 调用的库。 */
	load_file("libpqwalreceiver", false);

	/* 检查连接信息字符串。 */
	walrcv_check_conninfo(conninfo, opts.passwordrequired && !superuser());

	/* 一切正常，构造新元组。 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	subid = GetNewOidWithIndex(rel, SubscriptionObjectIndexId,
							   Anum_pg_subscription_oid);
	values[Anum_pg_subscription_oid - 1] = ObjectIdGetDatum(subid);
	values[Anum_pg_subscription_subdbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_subscription_subskiplsn - 1] = LSNGetDatum(InvalidXLogRecPtr);
	values[Anum_pg_subscription_subname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->subname));
	values[Anum_pg_subscription_subowner - 1] = ObjectIdGetDatum(owner);
	values[Anum_pg_subscription_subenabled - 1] = BoolGetDatum(opts.enabled);
	values[Anum_pg_subscription_subbinary - 1] = BoolGetDatum(opts.binary);
	values[Anum_pg_subscription_substream - 1] = CharGetDatum(opts.streaming);
	values[Anum_pg_subscription_subtwophasestate - 1] =
		CharGetDatum(opts.twophase ?
					 LOGICALREP_TWOPHASE_STATE_PENDING :
					 LOGICALREP_TWOPHASE_STATE_DISABLED);
	values[Anum_pg_subscription_subdisableonerr - 1] = BoolGetDatum(opts.disableonerr);
	values[Anum_pg_subscription_subpasswordrequired - 1] = BoolGetDatum(opts.passwordrequired);
	values[Anum_pg_subscription_subrunasowner - 1] = BoolGetDatum(opts.runasowner);
	values[Anum_pg_subscription_subconninfo - 1] =
		CStringGetTextDatum(conninfo);
	if (opts.slot_name)
		values[Anum_pg_subscription_subslotname - 1] =
			DirectFunctionCall1(namein, CStringGetDatum(opts.slot_name));
	else
		nulls[Anum_pg_subscription_subslotname - 1] = true;
	values[Anum_pg_subscription_subsynccommit - 1] =
		CStringGetTextDatum(opts.synchronous_commit);
	values[Anum_pg_subscription_subpublications - 1] =
		publicationListToArray(publications);
	values[Anum_pg_subscription_suborigin - 1] =
		CStringGetTextDatum(opts.origin);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* 将元组插入目录。 */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	recordDependencyOnOwner(SubscriptionRelationId, subid, owner);

	ReplicationOriginNameForLogicalRep(subid, InvalidOid, originname, sizeof(originname));
	replorigin_create(originname);

	/*
	  * 连接到远程端以执行请求的命令并获取
	  * 表信息。
	 */
	if (opts.connect)
	{
		char	   *err;
		WalReceiverConn *wrconn;
		List	   *tables;
		ListCell   *lc;
		char		table_state;
		bool		must_use_password;

		/* Try to connect to the publisher. */
		must_use_password = !superuser_arg(owner) && opts.passwordrequired;
		wrconn = walrcv_connect(conninfo, true, must_use_password,
								stmt->subname, &err);
		if (!wrconn)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not connect to the publisher: %s", err)));

		PG_TRY();
		{
			check_publications(wrconn, publications);
			check_publications_origin(wrconn, publications, opts.copy_data,
									  opts.origin, NULL, 0, stmt->subname);

			/*
			  * 根据是否要求进行数据复制来设置同步状态。
			  *
			 */
			table_state = opts.copy_data ? SUBREL_STATE_INIT : SUBREL_STATE_READY;

			/*
			  * 从发布者获取表列表并构建本地表状态
			  * 信息。
			 */
			tables = fetch_table_list(wrconn, publications);
			foreach(lc, tables)
			{
				RangeVar   *rv = (RangeVar *) lfirst(lc);
				Oid			relid;

				relid = RangeVarGetRelid(rv, AccessShareLock, false);

				/* Check for supported relkind. */
				CheckSubscriptionRelkind(get_rel_relkind(relid),
										 rv->schemaname, rv->relname);

				AddSubscriptionRelState(subid, relid, table_state,
										InvalidXLogRecPtr);
			}

			/*
			  * 若要求，为订阅创建永久槽。初始快照
			  * 不做任何用途，故无需导出。
			  *
			 */
			if (opts.create_slot)
			{
				bool		twophase_enabled = false;

				Assert(opts.slot_name);

				/*
				  * 即使设置了 two_phase，也不要创建启用两阶段的槽。
				  * 等所有表同步完成并就绪后再启用。这可避免
				  * 竞态条件——因对应表的表同步正在进行时，
				  * should_apply_changes_for_rel() 中的检查会导致预制备
				  * 事务因变更未被应用而被跳过。参见 worker.c
				  * 顶部的注释。
				  *
				  * 注意若指定了表但 copy_data 为 false，则可安全地
				  * 提前启用 two_phase，因那些表已初始处于 READY 状态。
				  * 当订阅无表时，将 twophase 状态保持为 PENDING，
				  * 以允许 ALTER SUBSCRIPTION ... REFRESH PUBLICATION 工作。
				  *
				  *
				  *
				 */
				if (opts.twophase && !opts.copy_data && tables != NIL)
					twophase_enabled = true;

				walrcv_create_slot(wrconn, opts.slot_name, false, twophase_enabled,
								   CRS_NOEXPORT_SNAPSHOT, NULL);

				if (twophase_enabled)
					UpdateTwoPhaseState(subid, LOGICALREP_TWOPHASE_STATE_ENABLED);

				ereport(NOTICE,
						(errmsg("created replication slot \"%s\" on publisher",
								opts.slot_name)));
			}
		}
		PG_FINALLY();
		{
			walrcv_disconnect(wrconn);
		}
		PG_END_TRY();
	}
	else
		ereport(WARNING,
				(errmsg("subscription was created, but is not connected"),
				 errhint("To initiate replication, you must manually create the replication slot, enable the subscription, and refresh the subscription.")));

	table_close(rel, RowExclusiveLock);

	pgstat_create_subscription(subid);

	if (opts.enabled)
		ApplyLauncherWakeupAtCommit();

	ObjectAddressSet(myself, SubscriptionRelationId, subid);

	InvokeObjectPostCreateHook(SubscriptionRelationId, subid, 0);

	return myself;
}

static void
AlterSubscription_refresh(Subscription *sub, bool copy_data,
						  List *validate_publications)
{
	char	   *err;
	List	   *pubrel_names;
	List	   *subrel_states;
	Oid		   *subrel_local_oids;
	Oid		   *pubrel_local_oids;
	ListCell   *lc;
	int			off;
	int			remove_rel_len;
	int			subrel_count;
	Relation	rel = NULL;
	typedef struct SubRemoveRels
	{
		Oid			relid;
		char		state;
	} SubRemoveRels;
	SubRemoveRels *sub_remove_rels;
	WalReceiverConn *wrconn;
	bool		must_use_password;

	/* 加载提供 libpq 调用的库。 */
	load_file("libpqwalreceiver", false);

	/* 尝试连接到发布者。 */
	must_use_password = !superuser_arg(sub->owner) && sub->passwordrequired;
	wrconn = walrcv_connect(sub->conninfo, true, must_use_password,
							sub->name, &err);
	if (!wrconn)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not connect to the publisher: %s", err)));

	PG_TRY();
	{
		if (validate_publications)
			check_publications(wrconn, validate_publications);

		/* Get the table list from publisher. */
		pubrel_names = fetch_table_list(wrconn, sub->publications);

		/* Get local table list. */
		subrel_states = GetSubscriptionRelations(sub->oid, false);
		subrel_count = list_length(subrel_states);

		/*
		  * 构建已排序的本地表 OID 数组以加快查找。
		  * 此数组可能含数据库中所有表，故查找速度
		  * 很重要。
		 */
		subrel_local_oids = palloc(subrel_count * sizeof(Oid));
		off = 0;
		foreach(lc, subrel_states)
		{
			SubscriptionRelState *relstate = (SubscriptionRelState *) lfirst(lc);

			subrel_local_oids[off++] = relstate->relid;
		}
		qsort(subrel_local_oids, subrel_count,
			  sizeof(Oid), oid_cmp);

		check_publications_origin(wrconn, sub->publications, copy_data,
								  sub->origin, subrel_local_oids,
								  subrel_count, sub->name);

		/*
		  * 我们想从订阅中移除的关系及其对应的槽
		  * 和源。
		 */
		sub_remove_rels = palloc(subrel_count * sizeof(SubRemoveRels));

		/*
		 * 遍历这些远程表，尝试将其与本地已知表匹配。   
		 * 如果该表在本地未知，则为它创建一个新的状态 
		 * 。
		 *
		 * 同时构建远程表的本地 OID 数组，供下一步使用。  
		 */
		off = 0;
		pubrel_local_oids = palloc(list_length(pubrel_names) * sizeof(Oid));

		foreach(lc, pubrel_names)
		{
			RangeVar   *rv = (RangeVar *) lfirst(lc);
			Oid			relid;

			relid = RangeVarGetRelid(rv, AccessShareLock, false);

			/* Check for supported relkind. */
			CheckSubscriptionRelkind(get_rel_relkind(relid),
									 rv->schemaname, rv->relname);

			pubrel_local_oids[off++] = relid;

			if (!bsearch(&relid, subrel_local_oids,
						 subrel_count, sizeof(Oid), oid_cmp))
			{
				AddSubscriptionRelState(sub->oid, relid,
										copy_data ? SUBREL_STATE_INIT : SUBREL_STATE_READY,
										InvalidXLogRecPtr);
				ereport(DEBUG1,
						(errmsg_internal("table \"%s.%s\" added to subscription \"%s\"",
										 rv->schemaname, rv->relname, sub->name)));
			}
		}

		/*
		  * 接下来用上面收集的数据移除不应再关注
		  * 的表的状态。
		 */
		qsort(pubrel_local_oids, list_length(pubrel_names),
			  sizeof(Oid), oid_cmp);

		remove_rel_len = 0;
		for (off = 0; off < subrel_count; off++)
		{
			Oid			relid = subrel_local_oids[off];

			if (!bsearch(&relid, pubrel_local_oids,
						 list_length(pubrel_names), sizeof(Oid), oid_cmp))
			{
				char		state;
				XLogRecPtr	statelsn;

				/*
				  * 用 AccessExclusiveLock 锁定 pg_subscription_rel
				  * 以防止应用工作进程与此代码同时重启动
				  * 工作进程时发生竞态。
				  *
				  * 即使此特定关系的新工作进程被重启，
				  * 由于我们持有 pg_subscription_rel 的排他锁
				  * 直到事务结束，它也无法取得任何进展。
				  * 它将因无对应 rel 条目而直接退出。
				  *
				  * 此锁定还确保在刷新操作完成前
				  * 关系状态不会改变。
				  *
				 */
				if (!rel)
					rel = table_open(SubscriptionRelRelationId, AccessExclusiveLock);

				/* Last known rel state. */
				state = GetSubscriptionRelState(sub->oid, relid, &statelsn);

				sub_remove_rels[remove_rel_len].relid = relid;
				sub_remove_rels[remove_rel_len++].state = state;

				RemoveSubscriptionRel(sub->oid, relid);

				logicalrep_worker_stop(sub->oid, relid);

				/*
				  * 对于 READY 状态，表同步源应当已被删除。
				  *
				 */
				if (state != SUBREL_STATE_READY)
				{
					char		originname[NAMEDATALEN];

					/*
					  * 若存在则删除表同步的源跟踪。
					  *
					  * 表同步工作进程的源可能尚未创建，
					  * 这在 SUBREL_STATE_FINISHEDCOPY 之前的状态
					  * 可能发生。表同步或应用工作进程也可能
					  * 并发尝试删除源，而此时源可能已被移除。
					  * 基于这些原因，传递 missing_ok = true。
					  *
					 */
					ReplicationOriginNameForLogicalRep(sub->oid, relid, originname,
													   sizeof(originname));
					replorigin_drop_by_name(originname, true, false);
				}

				ereport(DEBUG1,
						(errmsg_internal("table \"%s.%s\" removed from subscription \"%s\"",
										 get_namespace_name(get_rel_namespace(relid)),
										 get_rel_name(relid),
										 sub->name)));
			}
		}

		/*
		  * 删除与已移除表关联的表同步槽。必须在最后执行，
		  * 否则若数据库操作出错，我们将无法回滚已删除
		  * 的槽。
		 */
		for (off = 0; off < remove_rel_len; off++)
		{
			if (sub_remove_rels[off].state != SUBREL_STATE_READY &&
				sub_remove_rels[off].state != SUBREL_STATE_SYNCDONE)
			{
				char		syncslotname[NAMEDATALEN] = {0};

				/*
				  * 对于 READY/SYNCDONE 状态，我们已知表同步槽
				  * 已被表同步工作进程删除。
				  *
				  * 对于其他状态，无法确定，槽可能尚不存在。
				  * 此外，若在删除部分槽后失败，下次将再次尝试
				  * 删除已删除的槽并失败。基于这些原因，允许
				  * missing_ok = true 进行删除。
				  *
				 */
				ReplicationSlotNameForTablesync(sub->oid, sub_remove_rels[off].relid,
												syncslotname, sizeof(syncslotname));
				ReplicationSlotDropAtPubNode(wrconn, syncslotname, true);
			}
		}
	}
	PG_FINALLY();
	{
		walrcv_disconnect(wrconn);
	}
	PG_END_TRY();

	if (rel)
		table_close(rel, NoLock);
}

/*
  * 修改现有订阅。
 */
ObjectAddress
AlterSubscription(ParseState *pstate, AlterSubscriptionStmt *stmt,
				  bool isTopLevel)
{
	Relation	rel;
	ObjectAddress myself;
	bool		nulls[Natts_pg_subscription];
	bool		replaces[Natts_pg_subscription];
	Datum		values[Natts_pg_subscription];
	HeapTuple	tup;
	Oid			subid;
	bool		update_tuple = false;
	Subscription *sub;
	Form_pg_subscription form;
	bits32		supported_opts;
	SubOpts		opts = {0};

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	/* 获取现有元组。 */
	tup = SearchSysCacheCopy2(SUBSCRIPTIONNAME, MyDatabaseId,
							  CStringGetDatum(stmt->subname));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("subscription \"%s\" does not exist",
						stmt->subname)));

	form = (Form_pg_subscription) GETSTRUCT(tup);
	subid = form->oid;

	/* 必须是所有者 */
	if (!object_ownercheck(SubscriptionRelationId, subid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SUBSCRIPTION,
					   stmt->subname);

	sub = GetSubscription(subid, false);

	/*
	  * 不允许非超级用户修改
	  * password_required=false 的订阅。
	 */
	if (!sub->passwordrequired && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("password_required=false is superuser-only"),
				 errhint("Subscriptions with the password_required option set to false may only be created or modified by the superuser.")));

	/* 锁定订阅以防他人操作。 */
	LockSharedObject(SubscriptionRelationId, subid, 0, AccessExclusiveLock);

	/* 构造新元组。 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	switch (stmt->kind)
	{
		case ALTER_SUBSCRIPTION_OPTIONS:
			{
				supported_opts = (SUBOPT_SLOT_NAME |
								  SUBOPT_SYNCHRONOUS_COMMIT | SUBOPT_BINARY |
								  SUBOPT_STREAMING | SUBOPT_DISABLE_ON_ERR |
								  SUBOPT_PASSWORD_REQUIRED |
								  SUBOPT_RUN_AS_OWNER | SUBOPT_ORIGIN);

				parse_subscription_options(pstate, stmt->options,
										   supported_opts, &opts);

				if (IsSet(opts.specified_opts, SUBOPT_SLOT_NAME))
				{
					/*
					  * 订阅必须处于禁用状态才能将 slot_name
					  * 设置为 'none'，否则 apply worker 将重复尝试
					  * 使用该 slot_name 流式传输数据，而该槽在
					  * 发布者上不存在，用户也不被允许创建它。
					  *
					 */
					if (sub->enabled && !opts.slot_name)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("cannot set %s for enabled subscription",
										"slot_name = NONE")));

					if (opts.slot_name)
						values[Anum_pg_subscription_subslotname - 1] =
							DirectFunctionCall1(namein, CStringGetDatum(opts.slot_name));
					else
						nulls[Anum_pg_subscription_subslotname - 1] = true;
					replaces[Anum_pg_subscription_subslotname - 1] = true;
				}

				if (opts.synchronous_commit)
				{
					values[Anum_pg_subscription_subsynccommit - 1] =
						CStringGetTextDatum(opts.synchronous_commit);
					replaces[Anum_pg_subscription_subsynccommit - 1] = true;
				}

				if (IsSet(opts.specified_opts, SUBOPT_BINARY))
				{
					values[Anum_pg_subscription_subbinary - 1] =
						BoolGetDatum(opts.binary);
					replaces[Anum_pg_subscription_subbinary - 1] = true;
				}

				if (IsSet(opts.specified_opts, SUBOPT_STREAMING))
				{
					values[Anum_pg_subscription_substream - 1] =
						CharGetDatum(opts.streaming);
					replaces[Anum_pg_subscription_substream - 1] = true;
				}

				if (IsSet(opts.specified_opts, SUBOPT_DISABLE_ON_ERR))
				{
					values[Anum_pg_subscription_subdisableonerr - 1]
						= BoolGetDatum(opts.disableonerr);
					replaces[Anum_pg_subscription_subdisableonerr - 1]
						= true;
				}

				if (IsSet(opts.specified_opts, SUBOPT_PASSWORD_REQUIRED))
				{
					/* Non-superuser may not disable password_required. */
					if (!opts.passwordrequired && !superuser())
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("password_required=false is superuser-only"),
								 errhint("Subscriptions with the password_required option set to false may only be created or modified by the superuser.")));

					values[Anum_pg_subscription_subpasswordrequired - 1]
						= BoolGetDatum(opts.passwordrequired);
					replaces[Anum_pg_subscription_subpasswordrequired - 1]
						= true;
				}

				if (IsSet(opts.specified_opts, SUBOPT_RUN_AS_OWNER))
				{
					values[Anum_pg_subscription_subrunasowner - 1] =
						BoolGetDatum(opts.runasowner);
					replaces[Anum_pg_subscription_subrunasowner - 1] = true;
				}

				if (IsSet(opts.specified_opts, SUBOPT_ORIGIN))
				{
					values[Anum_pg_subscription_suborigin - 1] =
						CStringGetTextDatum(opts.origin);
					replaces[Anum_pg_subscription_suborigin - 1] = true;
				}

				update_tuple = true;
				break;
			}

		case ALTER_SUBSCRIPTION_ENABLED:
			{
				parse_subscription_options(pstate, stmt->options,
										   SUBOPT_ENABLED, &opts);
				Assert(IsSet(opts.specified_opts, SUBOPT_ENABLED));

				if (!sub->slotname && opts.enabled)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("cannot enable subscription that does not have a slot name")));

				values[Anum_pg_subscription_subenabled - 1] =
					BoolGetDatum(opts.enabled);
				replaces[Anum_pg_subscription_subenabled - 1] = true;

				if (opts.enabled)
					ApplyLauncherWakeupAtCommit();

				update_tuple = true;
				break;
			}

		case ALTER_SUBSCRIPTION_CONNECTION:
			/* Load the library providing us libpq calls. */
			load_file("libpqwalreceiver", false);
			/* Check the connection info string. */
			walrcv_check_conninfo(stmt->conninfo,
								  sub->passwordrequired && !superuser_arg(sub->owner));

			values[Anum_pg_subscription_subconninfo - 1] =
				CStringGetTextDatum(stmt->conninfo);
			replaces[Anum_pg_subscription_subconninfo - 1] = true;
			update_tuple = true;
			break;

		case ALTER_SUBSCRIPTION_SET_PUBLICATION:
			{
				supported_opts = SUBOPT_COPY_DATA | SUBOPT_REFRESH;
				parse_subscription_options(pstate, stmt->options,
										   supported_opts, &opts);

				values[Anum_pg_subscription_subpublications - 1] =
					publicationListToArray(stmt->publication);
				replaces[Anum_pg_subscription_subpublications - 1] = true;

				update_tuple = true;

				/* Refresh if user asked us to. */
				if (opts.refresh)
				{
					if (!sub->enabled)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("ALTER SUBSCRIPTION with refresh is not allowed for disabled subscriptions"),
								 errhint("Use ALTER SUBSCRIPTION ... SET PUBLICATION ... WITH (refresh = false).")));

					/*
					  * 关于为何不允许，详见
					  * ALTER_SUBSCRIPTION_REFRESH。
					 */
					if (sub->twophasestate == LOGICALREP_TWOPHASE_STATE_ENABLED && opts.copy_data)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("ALTER SUBSCRIPTION with refresh and copy_data is not allowed when two_phase is enabled"),
								 errhint("Use ALTER SUBSCRIPTION ... SET PUBLICATION with refresh = false, or with copy_data = false, or use DROP/CREATE SUBSCRIPTION.")));

					PreventInTransactionBlock(isTopLevel, "ALTER SUBSCRIPTION with refresh");

					/* Make sure refresh sees the new list of publications. */
					sub->publications = stmt->publication;

					AlterSubscription_refresh(sub, opts.copy_data,
											  stmt->publication);
				}

				break;
			}

		case ALTER_SUBSCRIPTION_ADD_PUBLICATION:
		case ALTER_SUBSCRIPTION_DROP_PUBLICATION:
			{
				List	   *publist;
				bool		isadd = stmt->kind == ALTER_SUBSCRIPTION_ADD_PUBLICATION;

				supported_opts = SUBOPT_REFRESH | SUBOPT_COPY_DATA;
				parse_subscription_options(pstate, stmt->options,
										   supported_opts, &opts);

				publist = merge_publications(sub->publications, stmt->publication, isadd, stmt->subname);
				values[Anum_pg_subscription_subpublications - 1] =
					publicationListToArray(publist);
				replaces[Anum_pg_subscription_subpublications - 1] = true;

				update_tuple = true;

				/* Refresh if user asked us to. */
				if (opts.refresh)
				{
					/* We only need to validate user specified publications. */
					List	   *validate_publications = (isadd) ? stmt->publication : NULL;

					if (!sub->enabled)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("ALTER SUBSCRIPTION with refresh is not allowed for disabled subscriptions"),
						/* translator: %s is an SQL ALTER command */
								 errhint("Use %s instead.",
										 isadd ?
										 "ALTER SUBSCRIPTION ... ADD PUBLICATION ... WITH (refresh = false)" :
										 "ALTER SUBSCRIPTION ... DROP PUBLICATION ... WITH (refresh = false)")));

					/*
					  * 关于为何不允许，详见
					  * ALTER_SUBSCRIPTION_REFRESH。
					 */
					if (sub->twophasestate == LOGICALREP_TWOPHASE_STATE_ENABLED && opts.copy_data)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("ALTER SUBSCRIPTION with refresh and copy_data is not allowed when two_phase is enabled"),
						/* translator: %s is an SQL ALTER command */
								 errhint("Use %s with refresh = false, or with copy_data = false, or use DROP/CREATE SUBSCRIPTION.",
										 isadd ?
										 "ALTER SUBSCRIPTION ... ADD PUBLICATION" :
										 "ALTER SUBSCRIPTION ... DROP PUBLICATION")));

					PreventInTransactionBlock(isTopLevel, "ALTER SUBSCRIPTION with refresh");

					/* Refresh the new list of publications. */
					sub->publications = publist;

					AlterSubscription_refresh(sub, opts.copy_data,
											  validate_publications);
				}

				break;
			}

		case ALTER_SUBSCRIPTION_REFRESH:
			{
				if (!sub->enabled)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("ALTER SUBSCRIPTION ... REFRESH is not allowed for disabled subscriptions")));

				parse_subscription_options(pstate, stmt->options,
										   SUBOPT_COPY_DATA, &opts);

				/*
				 * 订阅选项 "two_phase" 要求复制已经      
				 * 通过初始表同步阶段，two_phase 才能          
				 * 被正确启用。                                  
				 *
				 * 但达到两阶段提交"已启用"状态后，就不    
				 * 允许再做任何后续表初始化。故当用户     
				 * 请求了 two_phase = on 模式时，ALTER                
				 * SUBSCRIPTION ... REFRESH 不被允许。        
				 *
				 * 这一限制的例外是 copy_data =                 
				 * false 的情形，因为当 copy_data 为 false 时，   
				 * tablesync 将以 READY 状态启动并直接退出，      
				 * 不做事。   
				 *
				 * 更多细节见 worker.c 顶部的注释。 
				 */
				if (sub->twophasestate == LOGICALREP_TWOPHASE_STATE_ENABLED && opts.copy_data)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("ALTER SUBSCRIPTION ... REFRESH with copy_data is not allowed when two_phase is enabled"),
							 errhint("Use ALTER SUBSCRIPTION ... REFRESH with copy_data = false, or use DROP/CREATE SUBSCRIPTION.")));

				PreventInTransactionBlock(isTopLevel, "ALTER SUBSCRIPTION ... REFRESH");

				AlterSubscription_refresh(sub, opts.copy_data, NULL);

				break;
			}

		case ALTER_SUBSCRIPTION_SKIP:
			{
				parse_subscription_options(pstate, stmt->options, SUBOPT_LSN, &opts);

				/* ALTER SUBSCRIPTION ... SKIP supports only LSN option */
				Assert(IsSet(opts.specified_opts, SUBOPT_LSN));

				/*
				  * 若用户设置 subskiplsn，进行合理性检查确保
				  * 指定 LSN 是合理值。
				 */
				if (!XLogRecPtrIsInvalid(opts.lsn))
				{
					RepOriginId originid;
					char		originname[NAMEDATALEN];
					XLogRecPtr	remote_lsn;

					ReplicationOriginNameForLogicalRep(subid, InvalidOid,
													   originname, sizeof(originname));
					originid = replorigin_by_name(originname, false);
					remote_lsn = replorigin_get_progress(originid, false);

					/* Check the given LSN is at least a future LSN */
					if (!XLogRecPtrIsInvalid(remote_lsn) && opts.lsn < remote_lsn)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("skip WAL location (LSN %X/%X) must be greater than origin LSN %X/%X",
										LSN_FORMAT_ARGS(opts.lsn),
										LSN_FORMAT_ARGS(remote_lsn))));
				}

				values[Anum_pg_subscription_subskiplsn - 1] = LSNGetDatum(opts.lsn);
				replaces[Anum_pg_subscription_subskiplsn - 1] = true;

				update_tuple = true;
				break;
			}

		default:
			elog(ERROR, "unrecognized ALTER SUBSCRIPTION kind %d",
				 stmt->kind);
	}

	/* 若需要则更新目录。 */
	if (update_tuple)
	{
		tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
								replaces);

		CatalogTupleUpdate(rel, &tup->t_self, tup);

		heap_freetuple(tup);
	}

	table_close(rel, RowExclusiveLock);

	ObjectAddressSet(myself, SubscriptionRelationId, subid);

	InvokeObjectPostAlterHook(SubscriptionRelationId, subid, 0);

	/* 唤醒相关复制工作进程以快速处理此变更。 */
	LogicalRepWorkersWakeupAtCommit(subid);

	return myself;
}

/*
  * 删除订阅
 */
void
DropSubscription(DropSubscriptionStmt *stmt, bool isTopLevel)
{
	Relation	rel;
	ObjectAddress myself;
	HeapTuple	tup;
	Oid			subid;
	Oid			subowner;
	Datum		datum;
	bool		isnull;
	char	   *subname;
	char	   *conninfo;
	char	   *slotname;
	List	   *subworkers;
	ListCell   *lc;
	char		originname[NAMEDATALEN];
	char	   *err = NULL;
	WalReceiverConn *wrconn;
	Form_pg_subscription form;
	List	   *rstates;
	bool		must_use_password;

	/*
	  * 启动器可能并行为此订阅启动新工作进程。
	  * 初始化期间工作进程检查订阅有效性，若订阅
	  * 已被删除则退出。参见 InitializeLogRepWorker。
	  *
	 */
	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	tup = SearchSysCache2(SUBSCRIPTIONNAME, MyDatabaseId,
						  CStringGetDatum(stmt->subname));

	if (!HeapTupleIsValid(tup))
	{
		table_close(rel, NoLock);

		if (!stmt->missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("subscription \"%s\" does not exist",
							stmt->subname)));
		else
			ereport(NOTICE,
					(errmsg("subscription \"%s\" does not exist, skipping",
							stmt->subname)));

		return;
	}

	form = (Form_pg_subscription) GETSTRUCT(tup);
	subid = form->oid;
	subowner = form->subowner;
	must_use_password = !superuser_arg(subowner) && form->subpasswordrequired;

	/* 必须是所有者 */
	if (!object_ownercheck(SubscriptionRelationId, subid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SUBSCRIPTION,
					   stmt->subname);

	/* 正在删除的订阅的 DROP 钩子 */
	InvokeObjectDropHook(SubscriptionRelationId, subid, 0);

	/*
	  * 锁定订阅，不让任何人（包括复制工作进程）
	  * 对其做任何操作。
	 */
	LockSharedObject(SubscriptionRelationId, subid, 0, AccessExclusiveLock);

	/* 获取 subname */
	datum = SysCacheGetAttrNotNull(SUBSCRIPTIONOID, tup,
								   Anum_pg_subscription_subname);
	subname = pstrdup(NameStr(*DatumGetName(datum)));

	/* 获取 conninfo */
	datum = SysCacheGetAttrNotNull(SUBSCRIPTIONOID, tup,
								   Anum_pg_subscription_subconninfo);
	conninfo = TextDatumGetCString(datum);

	/* 获取 slotname */
	datum = SysCacheGetAttr(SUBSCRIPTIONOID, tup,
							Anum_pg_subscription_subslotname, &isnull);
	if (!isnull)
		slotname = pstrdup(NameStr(*DatumGetName(datum)));
	else
		slotname = NULL;

	/*
	  * 由于删除复制槽是非事务性的，即使回滚事务
	  * 复制槽也保持删除。因此若需删除复制槽，
	  * 不能在事务块内运行 DROP SUBSCRIPTION。
	  * 同时在此情况下我们向累积统计系统报告
	  * 删除订阅的消息。
	  *
	  * XXX 命令名实际上应类似 "DROP SUBSCRIPTION of a
	  * subscription that is associated with a replication slot"，
	  * 但我们没有适当的设施支持。
	 */
	if (slotname)
		PreventInTransactionBlock(isTopLevel, "DROP SUBSCRIPTION");

	ObjectAddressSet(myself, SubscriptionRelationId, subid);
	EventTriggerSQLDropAddObject(&myself, true, true);

	/* 从目录中删除元组。 */
	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	/*
	 * 立即停止所有订阅工作进程。       
	 *
	 * 如果我们要删除复制槽，那么这是必要的，这样该槽 
	 * 才能变得可访问。
	 *
	 * 如果订阅已被禁用，且是在同一事务中被禁用的，   
	 * 那么工作进程尚未看到禁用操作，仍会继续运         
	 * 行，这会导致稍后我们删除复制源时发生挂起。      
	 * 如果订阅在本事务之前已被禁用，那么应该不再      
	 * 有任何工作进程留下来，所以不会有影响。          
	 *                         
	 *
	 * 新的工作进程不会被启动，因为我们在事务结束前   
	 * 一直持有该订阅上的排他锁。      
	 */
	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
	subworkers = logicalrep_workers_find(subid, false);
	LWLockRelease(LogicalRepWorkerLock);
	foreach(lc, subworkers)
	{
		LogicalRepWorker *w = (LogicalRepWorker *) lfirst(lc);

		logicalrep_worker_stop(w->subid, w->relid);
	}
	list_free(subworkers);

	/*
	  * 删除启动器 apply worker 启动时间表中
	  * 不再有用的条目。
	  *
	  * 若此事务回滚，启动器可能在
	  * wal_retrieve_retry_interval 毫秒之前重启
	  * 失败的 apply worker，但这无害。
	 */
	ApplyLauncherForgetWorkerStartTime(subid);

	/*
	  * 清理表同步复制源。
	  *
	  * 任何 READY 状态的关系已自行完成清理。
	  *
	  * 注意状态不会改变，因为我们已停止 apply
	  * 和 tablesync 工作进程，且它们因订阅的
	  * 排他锁无法重启。
	 */
	rstates = GetSubscriptionRelations(subid, true);
	foreach(lc, rstates)
	{
		SubscriptionRelState *rstate = (SubscriptionRelState *) lfirst(lc);
		Oid			relid = rstate->relid;

		/* Only cleanup resources of tablesync workers */
		if (!OidIsValid(relid))
			continue;

		/*
		  * 若存在则删除表同步的源跟踪。
		  *
		  * 表同步工作进程的源可能尚未创建，故传递
		  * missing_ok = true。这在 SUBREL_STATE_FINISHEDCOPY
		  * 之前的状态可能发生。
		 */
		ReplicationOriginNameForLogicalRep(subid, relid, originname,
										   sizeof(originname));
		replorigin_drop_by_name(originname, true, false);
	}

	/* 清理依赖 */
	deleteSharedDependencyRecordsFor(SubscriptionRelationId, subid, 0);

	/* 删除关联的关系同步状态。 */
	RemoveSubscriptionRel(subid, InvalidOid);

	/* 若存在则删除源跟踪。 */
	ReplicationOriginNameForLogicalRep(subid, InvalidOid, originname, sizeof(originname));
	replorigin_drop_by_name(originname, true, false);

	/*
	  * 告知累积统计系统订阅正在被删除。
	  *
	 */
	pgstat_drop_subscription(subid);

	/*
	 * 如果订阅没有关联的复制槽，那么即可到此结束。 
	 *      
	 */
	if (!slotname && rstates == NIL)
	{
		table_close(rel, NoLock);
		return;
	}

	/*
	 * 尝试获取删除槽所需的连接。                    
	 *
	 * 注意：如果 slotname 为 NONE/NULL，那么我们允许命令完  
	 * 成，用户之后需要手动清理 apply 和 tablesync worker 槽    
	 * 。   
	 *
	 * 这一步必须在最后进行，否则如果在执行数据库操作 
	 * 期间发生错误，我们将无法回滚已删除的槽         
	 * 。  
	 */
	load_file("libpqwalreceiver", false);

	wrconn = walrcv_connect(conninfo, true, must_use_password,
							subname, &err);
	if (wrconn == NULL)
	{
		if (!slotname)
		{
			/* be tidy */
			list_free(rstates);
			table_close(rel, NoLock);
			return;
		}
		else
		{
			ReportSlotConnectionError(rstates, subid, slotname, err);
		}
	}

	PG_TRY();
	{
		foreach(lc, rstates)
		{
			SubscriptionRelState *rstate = (SubscriptionRelState *) lfirst(lc);
			Oid			relid = rstate->relid;

			/* Only cleanup resources of tablesync workers */
			if (!OidIsValid(relid))
				continue;

			/*
			  * 删除与已移除表关联的表同步槽。
			  *
			  * 对于 SYNCDONE/READY 状态，已知表同步槽
			  * 已被表同步工作进程删除。
			  *
			  * 对于其他状态无法确定，槽可能尚不存在。
			  * 此外，若在删除部分槽后失败，下次将再次
			  * 尝试删除已删除槽并失败。基于这些原因，
			  * 允许 missing_ok = true 进行删除。
			  *
			 */
			if (rstate->state != SUBREL_STATE_SYNCDONE)
			{
				char		syncslotname[NAMEDATALEN] = {0};

				ReplicationSlotNameForTablesync(subid, relid, syncslotname,
												sizeof(syncslotname));
				ReplicationSlotDropAtPubNode(wrconn, syncslotname, true);
			}
		}

		list_free(rstates);

		/*
		 * 如果订阅有关联的复制槽，那么就在发布者上      
		 * 删除复制槽。                
		 */
		if (slotname)
			ReplicationSlotDropAtPubNode(wrconn, slotname, false);
	}
	PG_FINALLY();
	{
		walrcv_disconnect(wrconn);
	}
	PG_END_TRY();

	table_close(rel, NoLock);
}

/*
 * 使用复制连接删除发布者节点上的复制槽               
 * 。        
 *
 * missing_ok - 若为 true，则槽不存在时仅记录 LOG 消息     
 * 。   
 */
void
ReplicationSlotDropAtPubNode(WalReceiverConn *wrconn, char *slotname, bool missing_ok)
{
	StringInfoData cmd;

	Assert(wrconn);

	load_file("libpqwalreceiver", false);

	initStringInfo(&cmd);
	appendStringInfo(&cmd, "DROP_REPLICATION_SLOT %s WAIT", quote_identifier(slotname));

	PG_TRY();
	{
		WalRcvExecResult *res;

		res = walrcv_exec(wrconn, cmd.data, 0, NULL);

		if (res->status == WALRCV_OK_COMMAND)
		{
			/* NOTICE. Success. */
			ereport(NOTICE,
					(errmsg("dropped replication slot \"%s\" on publisher",
							slotname)));
		}
		else if (res->status == WALRCV_ERROR &&
				 missing_ok &&
				 res->sqlstate == ERRCODE_UNDEFINED_OBJECT)
		{
			/* LOG. Error, but missing_ok = true. */
			ereport(LOG,
					(errmsg("could not drop replication slot \"%s\" on publisher: %s",
							slotname, res->err)));
		}
		else
		{
			/* ERROR. */
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not drop replication slot \"%s\" on publisher: %s",
							slotname, res->err)));
		}

		walrcv_clear_result(res);
	}
	PG_FINALLY();
	{
		pfree(cmd.data);
	}
	PG_END_TRY();
}

/*
  * 更改订阅所有者的内部核心函数
 */
static void
AlterSubscriptionOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_subscription form;
	AclResult	aclresult;

	form = (Form_pg_subscription) GETSTRUCT(tup);

	if (form->subowner == newOwnerId)
		return;

	if (!object_ownercheck(SubscriptionRelationId, form->oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SUBSCRIPTION,
					   NameStr(form->subname));

	/*
	  * 不允许非超级用户修改
	  * password_required=false 的订阅。
	 */
	if (!form->subpasswordrequired && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("password_required=false is superuser-only"),
				 errhint("Subscriptions with the password_required option set to false may only be created or modified by the superuser.")));

	/* 必须能成为新所有者 */
	check_can_set_role(GetUserId(), newOwnerId);

	/*
	  * 当前所有者必须对数据库有 CREATE 权限
	  *
	  * 这与 ALTER SCHEMA ... OWNER TO 的行为一致，
	  * 但某些其他对象类型行为不同（例如不能将表
	  * 给缺少模式 CREATE 权限的用户）。
	 */
	aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId,
								GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));

	form->subowner = newOwnerId;
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* 更新所有者依赖引用 */
	changeDependencyOnOwner(SubscriptionRelationId,
							form->oid,
							newOwnerId);

	InvokeObjectPostAlterHook(SubscriptionRelationId,
							  form->oid, 0);

	/* 唤醒相关后台进程以快速处理此变更。 */
	ApplyLauncherWakeupAtCommit();
	LogicalRepWorkersWakeupAtCommit(form->oid);
}

/*
  * 更改订阅所有者——按名称
 */
ObjectAddress
AlterSubscriptionOwner(const char *name, Oid newOwnerId)
{
	Oid			subid;
	HeapTuple	tup;
	Relation	rel;
	ObjectAddress address;
	Form_pg_subscription form;

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy2(SUBSCRIPTIONNAME, MyDatabaseId,
							  CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("subscription \"%s\" does not exist", name)));

	form = (Form_pg_subscription) GETSTRUCT(tup);
	subid = form->oid;

	AlterSubscriptionOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, SubscriptionRelationId, subid);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/*
  * 更改订阅所有者——按 OID
 */
void
AlterSubscriptionOwner_oid(Oid subid, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(SUBSCRIPTIONOID, ObjectIdGetDatum(subid));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("subscription with OID %u does not exist", subid)));

	AlterSubscriptionOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);
}

/*
  * 若发布者从其他发布者订阅了同一表、其分区祖先
  * （若为分区）或其分区子表（若为分区表），则检查并
  * 记录警告。此检查仅在 CREATE SUBSCRIPTION 和
  * ALTER SUBSCRIPTION ... REFRESH 语句中
  * "copy_data = true" 且 "origin = none" 时需要，
  * 以通知用户可能复制了带来源标识的数据。
  *
  * 对已添加的表不需执行此检查，
  * 因为那些表将通过 WAL 增量同步，
  * 数据来源可从 WAL 记录中识别。
  *
  * subrel_local_oids 包含已在订阅者上的
  * 关系 OID 列表。
 */
static void
check_publications_origin(WalReceiverConn *wrconn, List *publications,
						  bool copydata, char *origin, Oid *subrel_local_oids,
						  int subrel_count, char *subname)
{
	WalRcvExecResult *res;
	StringInfoData cmd;
	TupleTableSlot *slot;
	Oid			tableRow[1] = {TEXTOID};
	List	   *publist = NIL;
	int			i;

	if (!copydata || !origin ||
		(pg_strcasecmp(origin, LOGICALREP_ORIGIN_NONE) != 0))
		return;

	initStringInfo(&cmd);
	appendStringInfoString(&cmd,
						   "SELECT DISTINCT P.pubname AS pubname\n"
						   "FROM pg_publication P,\n"
						   "     LATERAL pg_get_publication_tables(P.pubname) GPT\n"
						   "     JOIN pg_subscription_rel PS ON (GPT.relid = PS.srrelid OR"
						   "     GPT.relid IN (SELECT relid FROM pg_partition_ancestors(PS.srrelid) UNION"
						   "                   SELECT relid FROM pg_partition_tree(PS.srrelid))),\n"
						   "     pg_class C JOIN pg_namespace N ON (N.oid = C.relnamespace)\n"
						   "WHERE C.oid = GPT.relid AND P.pubname IN (");
	get_publications_str(publications, &cmd, true);
	appendStringInfoString(&cmd, ")\n");

	/*
	  * 对于 ALTER SUBSCRIPTION ... REFRESH，
	  * subrel_local_oids 包含已在订阅者上的
	  * 关系 OID 列表。应对这些表跳过此检查。
	 */
	for (i = 0; i < subrel_count; i++)
	{
		Oid			relid = subrel_local_oids[i];
		char	   *schemaname = get_namespace_name(get_rel_namespace(relid));
		char	   *tablename = get_rel_name(relid);

		appendStringInfo(&cmd, "AND NOT (N.nspname = '%s' AND C.relname = '%s')\n",
						 schemaname, tablename);
	}

	res = walrcv_exec(wrconn, cmd.data, 1, tableRow);
	pfree(cmd.data);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not receive list of replicated tables from the publisher: %s",
						res->err)));

	/* 处理表。 */
	slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
	{
		char	   *pubname;
		bool		isnull;

		pubname = TextDatumGetCString(slot_getattr(slot, 1, &isnull));
		Assert(!isnull);

		ExecClearTuple(slot);
		publist = list_append_unique(publist, makeString(pubname));
	}

	/*
	  * 若发布者从其他发布者订阅了同一表，记录警告。
	  * 初始同步期间无法知晓数据来源。数据来源
	  * 只能从 WAL 中通过源 ID 识别。
	  *
	  * XXX：为简单起见，不检查表是否有数据。
	  * 若表无数据则无需区分有来源和无来源数据，
	  * 此时可避免记录警告。
	  *
	  *
	 */
	if (publist)
	{
		StringInfo	pubnames = makeStringInfo();

		/* Prepare the list of publication(s) for warning message. */
		get_publications_str(publist, pubnames, false);
		ereport(WARNING,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("subscription \"%s\" requested copy_data with origin = NONE but might copy data that had a different origin",
					   subname),
				errdetail_plural("The subscription being created subscribes to a publication (%s) that contains tables that are written to by other subscriptions.",
								 "The subscription being created subscribes to publications (%s) that contain tables that are written to by other subscriptions.",
								 list_length(publist), pubnames->data),
				errhint("Verify that initial data copied from the publisher tables did not come from other origins."));
	}

	ExecDropSingleTupleTableSlot(slot);

	walrcv_clear_result(res);
}

/*
 * 获取属于发布者连接上指定发布的那些表的列表。  
 * 。                  
 *
 * 注意，我们不支持同一个表在不同发布中具有不同列     
 * 列表的情况，以避免为某些行发送不需要的列信息。    
 * 当为不同的发布同时指定了列列表和行过滤器时，就 
 * 可能出现这种情况。                                  
 */
static List *
fetch_table_list(WalReceiverConn *wrconn, List *publications)
{
	WalRcvExecResult *res;
	StringInfoData cmd;
	TupleTableSlot *slot;
	Oid			tableRow[3] = {TEXTOID, TEXTOID, InvalidOid};
	List	   *tablelist = NIL;
	int			server_version = walrcv_server_version(wrconn);
	bool		check_columnlist = (server_version >= 150000);

	initStringInfo(&cmd);

	/* 从发布者获取表列表。 */
	if (server_version >= 160000)
	{
		StringInfoData pub_names;

		tableRow[2] = INT2VECTOROID;
		initStringInfo(&pub_names);
		get_publications_str(publications, &pub_names, true);

		/*
		  * 从版本 16 开始，允许向 pg_get_publication_tables
		  * 函数传递多个发布。这有助于过滤掉其祖先也在此
		  * 发布数组中发布的分区表。
		  *
		  * 将 pg_get_publication_tables 与 pg_publication 连接以
		  * 排除不存在的发布。
		  *
		  * 注意 attrs 始终以排序顺序存储，因此无需担心
		  * 不同发布以不同顺序指定它们。参见
		  * publication_translate_columns。
		  *
		 */
		appendStringInfo(&cmd, "SELECT DISTINCT n.nspname, c.relname, gpt.attrs\n"
						 "       FROM pg_class c\n"
						 "         JOIN pg_namespace n ON n.oid = c.relnamespace\n"
						 "         JOIN ( SELECT (pg_get_publication_tables(VARIADIC array_agg(pubname::text))).*\n"
						 "                FROM pg_publication\n"
						 "                WHERE pubname IN ( %s )) AS gpt\n"
						 "             ON gpt.relid = c.oid\n",
						 pub_names.data);

		pfree(pub_names.data);
	}
	else
	{
		tableRow[2] = NAMEARRAYOID;
		appendStringInfoString(&cmd, "SELECT DISTINCT t.schemaname, t.tablename \n");

		/* Get column lists for each relation if the publisher supports it */
		if (check_columnlist)
			appendStringInfoString(&cmd, ", t.attnames\n");

		appendStringInfoString(&cmd, "FROM pg_catalog.pg_publication_tables t\n"
							   " WHERE t.pubname IN (");
		get_publications_str(publications, &cmd, true);
		appendStringInfoChar(&cmd, ')');
	}

	res = walrcv_exec(wrconn, cmd.data, check_columnlist ? 3 : 2, tableRow);
	pfree(cmd.data);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not receive list of replicated tables from the publisher: %s",
						res->err)));

	/* 处理表。 */
	slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
	{
		char	   *nspname;
		char	   *relname;
		bool		isnull;
		RangeVar   *rv;

		nspname = TextDatumGetCString(slot_getattr(slot, 1, &isnull));
		Assert(!isnull);
		relname = TextDatumGetCString(slot_getattr(slot, 2, &isnull));
		Assert(!isnull);

		rv = makeRangeVar(nspname, relname, -1);

		if (check_columnlist && list_member(tablelist, rv))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot use different column lists for table \"%s.%s\" in different publications",
						   nspname, relname));
		else
			tablelist = lappend(tablelist, rv);

		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);

	walrcv_clear_result(res);

	return tablelist;
}

/*
  * 报告删除复制槽时连接失败的错误。
  * 这里对所有表同步槽发出 WARNING，以便用户
  * 可在需要时手动删除它们。
 */
static void
ReportSlotConnectionError(List *rstates, Oid subid, char *slotname, char *err)
{
	ListCell   *lc;

	foreach(lc, rstates)
	{
		SubscriptionRelState *rstate = (SubscriptionRelState *) lfirst(lc);
		Oid			relid = rstate->relid;

		/* Only cleanup resources of tablesync workers */
		if (!OidIsValid(relid))
			continue;

		/*
		  * 调用者需确保 relstate 不在我们之下更改。
		  * 参见 DropSubscription 中我们获取 relstates 的方式。
		 */
		if (rstate->state != SUBREL_STATE_SYNCDONE)
		{
			char		syncslotname[NAMEDATALEN] = {0};

			ReplicationSlotNameForTablesync(subid, relid, syncslotname,
											sizeof(syncslotname));
			elog(WARNING, "could not drop tablesync replication slot \"%s\"",
				 syncslotname);
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_CONNECTION_FAILURE),
			 errmsg("could not connect to publisher when attempting to drop replication slot \"%s\": %s",
					slotname, err),
	/* translator: %s is an SQL ALTER command */
			 errhint("Use %s to disable the subscription, and then use %s to disassociate it from the slot.",
					 "ALTER SUBSCRIPTION ... DISABLE",
					 "ALTER SUBSCRIPTION ... SET (slot_name = NONE)")));
}

/*
  * 检查给定发布列表中是否有重复项，如有则报错。
  * 若 datums 非 NULL，将发布添加为 text 数据。
  *
 */
static void
check_duplicates_in_publist(List *publist, Datum *datums)
{
	ListCell   *cell;
	int			j = 0;

	foreach(cell, publist)
	{
		char	   *name = strVal(lfirst(cell));
		ListCell   *pcell;

		foreach(pcell, publist)
		{
			char	   *pname = strVal(lfirst(pcell));

			if (pcell == cell)
				break;

			if (strcmp(name, pname) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("publication name \"%s\" used more than once",
								pname)));
		}

		if (datums)
			datums[j++] = CStringGetTextDatum(name);
	}
}

/*
  * 合并当前订阅的发布和用户从 ADD/DROP PUBLICATIONS
  * 指定的发布。
  *
  * 若 addpub 为 true，将发布列表添加到 oldpublist。
  * 否则从 oldpublist 删除发布列表。
  * 返回的列表是副本，oldpublist 本身不变。
  *
  * subname 是订阅名称，用于错误消息。
 */
static List *
merge_publications(List *oldpublist, List *newpublist, bool addpub, const char *subname)
{
	ListCell   *lc;

	oldpublist = list_copy(oldpublist);

	check_duplicates_in_publist(newpublist, NULL);

	foreach(lc, newpublist)
	{
		char	   *name = strVal(lfirst(lc));
		ListCell   *lc2;
		bool		found = false;

		foreach(lc2, oldpublist)
		{
			char	   *pubname = strVal(lfirst(lc2));

			if (strcmp(name, pubname) == 0)
			{
				found = true;
				if (addpub)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("publication \"%s\" is already in subscription \"%s\"",
									name, subname)));
				else
					oldpublist = foreach_delete_current(oldpublist, lc2);

				break;
			}
		}

		if (addpub && !found)
			oldpublist = lappend(oldpublist, makeString(name));
		else if (!addpub && !found)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("publication \"%s\" is not in subscription \"%s\"",
							name, subname)));
	}

	/*
	  * XXX 可能没有充分理由，但目前是为了让 ALTER SUBSCRIPTION
	  * ... DROP PUBLICATION 与 SET PUBLICATION 保持一致。
	 */
	if (!oldpublist)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("cannot drop all the publications from a subscription")));

	return oldpublist;
}

/*
  * 从 DefElem 提取流式模式值。类似 defGetBoolean()，
  * 但也接受特殊值 "parallel"。
 */
char
defGetStreamingMode(DefElem *def)
{
	/*
	  * 若未给参数值，假定意为 "true"。
	 */
	if (!def->arg)
		return LOGICALREP_STREAM_ON;

	/*
	  * 允许 0, 1, "false", "true", "off", "on" 或 "parallel"。
	 */
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			switch (intVal(def->arg))
			{
				case 0:
					return LOGICALREP_STREAM_OFF;
				case 1:
					return LOGICALREP_STREAM_ON;
				default:
					/* otherwise, error out below */
					break;
			}
			break;
		default:
			{
				char	   *sval = defGetString(def);

				/*
				  * 此处接受的字符串集应与语法中的
				  * opt_boolean_or_string 产生式匹配。
				 */
				if (pg_strcasecmp(sval, "false") == 0 ||
					pg_strcasecmp(sval, "off") == 0)
					return LOGICALREP_STREAM_OFF;
				if (pg_strcasecmp(sval, "true") == 0 ||
					pg_strcasecmp(sval, "on") == 0)
					return LOGICALREP_STREAM_ON;
				if (pg_strcasecmp(sval, "parallel") == 0)
					return LOGICALREP_STREAM_PARALLEL;
			}
			break;
	}

	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("%s requires a Boolean value or \"parallel\"",
					def->defname)));
	return LOGICALREP_STREAM_OFF;	/* keep compiler quiet */
}
