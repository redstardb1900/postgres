/*-------------------------------------------------------------------------
 *
 * foreign.c
 *	外部数据包装器、服务器与用户映射的支持。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/backend/foreign/foreign.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/*
 * GetForeignDataWrapper：按 OID 查外部数据包装器。
 */
ForeignDataWrapper *
GetForeignDataWrapper(Oid fdwid)
{
	return GetForeignDataWrapperExtended(fdwid, 0);
}


/*
 * GetForeignDataWrapperExtended：查外部数据包装器，
 * 按 OID。flags 含 FDW_MISSING_OK 时，对象找不到就
 * 返回 NULL，而不报错。
 */
ForeignDataWrapper *
GetForeignDataWrapperExtended(Oid fdwid, bits16 flags)
{
	Form_pg_foreign_data_wrapper fdwform;
	ForeignDataWrapper *fdw;
	Datum		datum;
	HeapTuple	tp;
	bool		isnull;

	tp = SearchSysCache1(FOREIGNDATAWRAPPEROID, ObjectIdGetDatum(fdwid));

	if (!HeapTupleIsValid(tp))
	{
		if ((flags & FDW_MISSING_OK) == 0)
			elog(ERROR, "cache lookup failed for foreign-data wrapper %u", fdwid);
		return NULL;
	}

	fdwform = (Form_pg_foreign_data_wrapper) GETSTRUCT(tp);

	fdw = (ForeignDataWrapper *) palloc(sizeof(ForeignDataWrapper));
	fdw->fdwid = fdwid;
	fdw->owner = fdwform->fdwowner;
	fdw->fdwname = pstrdup(NameStr(fdwform->fdwname));
	fdw->fdwhandler = fdwform->fdwhandler;
	fdw->fdwvalidator = fdwform->fdwvalidator;

	/* 提取 fdwoptions */
	datum = SysCacheGetAttr(FOREIGNDATAWRAPPEROID,
							tp,
							Anum_pg_foreign_data_wrapper_fdwoptions,
							&isnull);
	if (isnull)
		fdw->options = NIL;
	else
		fdw->options = untransformRelOptions(datum);

	ReleaseSysCache(tp);

	return fdw;
}


/*
 * GetForeignDataWrapperByName：查外部数据包装器
 * 定义，按名。
 */
ForeignDataWrapper *
GetForeignDataWrapperByName(const char *fdwname, bool missing_ok)
{
	Oid			fdwId = get_foreign_data_wrapper_oid(fdwname, missing_ok);

	if (!OidIsValid(fdwId))
		return NULL;

	return GetForeignDataWrapper(fdwId);
}


/*
 * GetForeignServer：查外部服务器定义。
 */
ForeignServer *
GetForeignServer(Oid serverid)
{
	return GetForeignServerExtended(serverid, 0);
}


/*
 * GetForeignServerExtended：查外部服务器定义。若
 *  flags 含 FSV_MISSING_OK，对象找不到就返回 NULL，
 * 而不报错。
 */
ForeignServer *
GetForeignServerExtended(Oid serverid, bits16 flags)
{
	Form_pg_foreign_server serverform;
	ForeignServer *server;
	HeapTuple	tp;
	Datum		datum;
	bool		isnull;

	tp = SearchSysCache1(FOREIGNSERVEROID, ObjectIdGetDatum(serverid));

	if (!HeapTupleIsValid(tp))
	{
		if ((flags & FSV_MISSING_OK) == 0)
			elog(ERROR, "cache lookup failed for foreign server %u", serverid);
		return NULL;
	}

	serverform = (Form_pg_foreign_server) GETSTRUCT(tp);

	server = (ForeignServer *) palloc(sizeof(ForeignServer));
	server->serverid = serverid;
	server->servername = pstrdup(NameStr(serverform->srvname));
	server->owner = serverform->srvowner;
	server->fdwid = serverform->srvfdw;

	/* 取服务器类型 */
	datum = SysCacheGetAttr(FOREIGNSERVEROID,
							tp,
							Anum_pg_foreign_server_srvtype,
							&isnull);
	server->servertype = isnull ? NULL : TextDatumGetCString(datum);

	/* 提取服务器版本 */
	datum = SysCacheGetAttr(FOREIGNSERVEROID,
							tp,
							Anum_pg_foreign_server_srvversion,
							&isnull);
	server->serverversion = isnull ? NULL : TextDatumGetCString(datum);

	/* 提取 srvoptions */
	datum = SysCacheGetAttr(FOREIGNSERVEROID,
							tp,
							Anum_pg_foreign_server_srvoptions,
							&isnull);
	if (isnull)
		server->options = NIL;
	else
		server->options = untransformRelOptions(datum);

	ReleaseSysCache(tp);

	return server;
}


/*
 * GetForeignServerByName：按名查外部服务器定义。
 */
ForeignServer *
GetForeignServerByName(const char *srvname, bool missing_ok)
{
	Oid			serverid = get_foreign_server_oid(srvname, missing_ok);

	if (!OidIsValid(serverid))
		return NULL;

	return GetForeignServer(serverid);
}


/*
 * GetUserMapping：查用户映射。
 *
 * 若所给用户没有映射，再查找
 *  PUBLIC 映射(userid==InvalidOid)。
 */
UserMapping *
GetUserMapping(Oid userid, Oid serverid)
{
	Datum		datum;
	HeapTuple	tp;
	bool		isnull;
	UserMapping *um;

	tp = SearchSysCache2(USERMAPPINGUSERSERVER,
						 ObjectIdGetDatum(userid),
						 ObjectIdGetDatum(serverid));

	if (!HeapTupleIsValid(tp))
	{
		/* 该用户未找到，改试 PUBLIC */
		tp = SearchSysCache2(USERMAPPINGUSERSERVER,
							 ObjectIdGetDatum(InvalidOid),
							 ObjectIdGetDatum(serverid));
	}

	if (!HeapTupleIsValid(tp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("user mapping not found for \"%s\"",
						MappingUserName(userid))));

	um = (UserMapping *) palloc(sizeof(UserMapping));
	um->umid = ((Form_pg_user_mapping) GETSTRUCT(tp))->oid;
	um->userid = userid;
	um->serverid = serverid;

	/* 提取 umoptions */
	datum = SysCacheGetAttr(USERMAPPINGUSERSERVER,
							tp,
							Anum_pg_user_mapping_umoptions,
							&isnull);
	if (isnull)
		um->options = NIL;
	else
		um->options = untransformRelOptions(datum);

	ReleaseSysCache(tp);

	return um;
}


/*
 * GetForeignTable：按关系 OID 查外部表定义。
 */
ForeignTable *
GetForeignTable(Oid relid)
{
	Form_pg_foreign_table tableform;
	ForeignTable *ft;
	HeapTuple	tp;
	Datum		datum;
	bool		isnull;

	tp = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for foreign table %u", relid);
	tableform = (Form_pg_foreign_table) GETSTRUCT(tp);

	ft = (ForeignTable *) palloc(sizeof(ForeignTable));
	ft->relid = relid;
	ft->serverid = tableform->ftserver;

	/* 提取 ftoptions */
	datum = SysCacheGetAttr(FOREIGNTABLEREL,
							tp,
							Anum_pg_foreign_table_ftoptions,
							&isnull);
	if (isnull)
		ft->options = NIL;
	else
		ft->options = untransformRelOptions(datum);

	ReleaseSysCache(tp);

	return ft;
}


/*
 * GetForeignColumnOptions：给定关系/attnum 的 attfdwoptions
 *  作 DefElem 列表
 */
List *
GetForeignColumnOptions(Oid relid, AttrNumber attnum)
{
	List	   *options;
	HeapTuple	tp;
	Datum		datum;
	bool		isnull;

	tp = SearchSysCache2(ATTNUM,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	datum = SysCacheGetAttr(ATTNUM,
							tp,
							Anum_pg_attribute_attfdwoptions,
							&isnull);
	if (isnull)
		options = NIL;
	else
		options = untransformRelOptions(datum);

	ReleaseSysCache(tp);

	return options;
}


/*
 * GetFdwRoutine：调用指定外部数据包装器的 handler，
 * 以取得其 FdwRoutine。
 */
FdwRoutine *
GetFdwRoutine(Oid fdwhandler)
{
	Datum		datum;
	FdwRoutine *routine;

	/* 检查对外部表的访问是否受限 */
	if (unlikely((restrict_nonsystem_relation_kind & RESTRICT_RELKIND_FOREIGN_TABLE) != 0))
	{
		/* 不得有内建的 FDW handler  */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("access to non-system foreign table is restricted")));
	}

	datum = OidFunctionCall0(fdwhandler);
	routine = (FdwRoutine *) DatumGetPointer(datum);

	if (routine == NULL || !IsA(routine, FdwRoutine))
		elog(ERROR, "foreign-data wrapper handler function %u did not return an FdwRoutine struct",
			 fdwhandler);

	return routine;
}


/*
 * GetForeignServerIdByRelId：查外部服务器，
 * 供给定外部表，并返回其 OID。
 */
Oid
GetForeignServerIdByRelId(Oid relid)
{
	HeapTuple	tp;
	Form_pg_foreign_table tableform;
	Oid			serverid;

	tp = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for foreign table %u", relid);
	tableform = (Form_pg_foreign_table) GETSTRUCT(tp);
	serverid = tableform->ftserver;
	ReleaseSysCache(tp);

	return serverid;
}


/*
 * GetFdwRoutineByServerId：查外部数据包装器的 handler，
 * 供给定外部服务器，并取其 FdwRoutine。
 */
FdwRoutine *
GetFdwRoutineByServerId(Oid serverid)
{
	HeapTuple	tp;
	Form_pg_foreign_data_wrapper fdwform;
	Form_pg_foreign_server serverform;
	Oid			fdwid;
	Oid			fdwhandler;

	/* 取该服务器的外部数据包装器 OID */
	tp = SearchSysCache1(FOREIGNSERVEROID, ObjectIdGetDatum(serverid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for foreign server %u", serverid);
	serverform = (Form_pg_foreign_server) GETSTRUCT(tp);
	fdwid = serverform->srvfdw;
	ReleaseSysCache(tp);

	/* 取该 FDW 的 handler 函数 OID */
	tp = SearchSysCache1(FOREIGNDATAWRAPPEROID, ObjectIdGetDatum(fdwid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for foreign-data wrapper %u", fdwid);
	fdwform = (Form_pg_foreign_data_wrapper) GETSTRUCT(tp);
	fdwhandler = fdwform->fdwhandler;

	/* 若 FDW 被设为 NO HANDLER 则报错 */
	if (!OidIsValid(fdwhandler))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("foreign-data wrapper \"%s\" has no handler",
						NameStr(fdwform->fdwname))));

	ReleaseSysCache(tp);

	/* 最后调用该 handler 函数 */
	return GetFdwRoutine(fdwhandler);
}


/*
 * GetFdwRoutineByRelId：查外部数据包装器的 handler，
 * 供给定外部表，并取其 FdwRoutine。
 */
FdwRoutine *
GetFdwRoutineByRelId(Oid relid)
{
	Oid			serverid;

	/* 取该外部表的服务器 OID */
	serverid = GetForeignServerIdByRelId(relid);

	/* 再取服务器的 FdwRoutine */
	return GetFdwRoutineByServerId(serverid);
}

/*
 * GetFdwRoutineForRelation：查外部数据包装器的 handler，
 * 供给定外部表，并取其 FdwRoutine。
 *
 * 本函数优于 GetFdwRoutineByRelId，因它把数据
 * 缓存在 relcache 条目里，少做若干系统目录查找。
 *
 * 若 makecopy 为真，返回的数据在调用者的
 * 内存上下文中新 palloc。否则它是 relcache 数据的指针，
 * 任何 relcache 重置都会丢失它，故勿长期依赖。
 */
FdwRoutine *
GetFdwRoutineForRelation(Relation relation, bool makecopy)
{
	FdwRoutine *fdwroutine;
	FdwRoutine *cfdwroutine;

	if (relation->rd_fdwroutine == NULL)
	{
		/* 查阅系统目录与 FDW 代码来取得该信息 */
		fdwroutine = GetFdwRoutineByRelId(RelationGetRelid(relation));

		/* 把数据存进 CacheMemoryContext 供以后复用 */
		cfdwroutine = (FdwRoutine *) MemoryContextAlloc(CacheMemoryContext,
														sizeof(FdwRoutine));
		memcpy(cfdwroutine, fdwroutine, sizeof(FdwRoutine));
		relation->rd_fdwroutine = cfdwroutine;

		/* 不论 makecopy 与否，都交还本地 palloc 的副本 */
		return fdwroutine;
	}

	/* 已有有效缓存。调用者要一份副本吗 */
	if (makecopy)
	{
		fdwroutine = (FdwRoutine *) palloc(sizeof(FdwRoutine));
		memcpy(fdwroutine, relation->rd_fdwroutine, sizeof(FdwRoutine));
		return fdwroutine;
	}

	/* 只需短时引用，故直接交还缓存副本 */
	return relation->rd_fdwroutine;
}


/*
 * IsImportableForeignTable：为 IMPORT FOREIGN SCHEMA 过滤表名。
 *
 * 若给定表名按该语句的导入过滤选项
 * 应被导入，就返回 true。
 */
bool
IsImportableForeignTable(const char *tablename,
						 ImportForeignSchemaStmt *stmt)
{
	ListCell   *lc;

	switch (stmt->list_type)
	{
		case FDW_IMPORT_SCHEMA_ALL:
			return true;

		case FDW_IMPORT_SCHEMA_LIMIT_TO:
			foreach(lc, stmt->table_list)
			{
				RangeVar   *rv = (RangeVar *) lfirst(lc);

				if (strcmp(tablename, rv->relname) == 0)
					return true;
			}
			return false;

		case FDW_IMPORT_SCHEMA_EXCEPT:
			foreach(lc, stmt->table_list)
			{
				RangeVar   *rv = (RangeVar *) lfirst(lc);

				if (strcmp(tablename, rv->relname) == 0)
					return false;
			}
			return true;
	}
	return false;				/* 不应到达此处 */
}


/*
 * pg_options_to_table：把选项数组转成名/值表。
 *
 * 便于给 information_schema 和 pg_dump 提供细节。
 */
Datum
pg_options_to_table(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	ListCell   *cell;
	List	   *options;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	options = untransformRelOptions(array);
	rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* 准备结果集 */
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	foreach(cell, options)
	{
		DefElem    *def = lfirst(cell);
		Datum		values[2];
		bool		nulls[2];

		values[0] = CStringGetTextDatum(def->defname);
		nulls[0] = false;
		if (def->arg)
		{
			values[1] = CStringGetTextDatum(strVal(def->arg));
			nulls[1] = false;
		}
		else
		{
			values[1] = (Datum) 0;
			nulls[1] = true;
		}
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	return (Datum) 0;
}


/*
 * 描述 postgresql FDW、服务器与用户映射的有效选项。
 */
struct ConnectionOption
{
	const char *optname;
	Oid			optcontext;		/* 选项可以出现在其中的目录 OID */
};

/*
 * 抄自 fe-connect.c PQconninfoOptions。
 *
 * 表还小，若一直这样就不必用 bsearch。
 */
static const struct ConnectionOption libpq_conninfo_options[] = {
	{"authtype", ForeignServerRelationId},
	{"service", ForeignServerRelationId},
	{"user", UserMappingRelationId},
	{"password", UserMappingRelationId},
	{"connect_timeout", ForeignServerRelationId},
	{"dbname", ForeignServerRelationId},
	{"host", ForeignServerRelationId},
	{"hostaddr", ForeignServerRelationId},
	{"port", ForeignServerRelationId},
	{"tty", ForeignServerRelationId},
	{"options", ForeignServerRelationId},
	{"requiressl", ForeignServerRelationId},
	{"sslmode", ForeignServerRelationId},
	{"gsslib", ForeignServerRelationId},
	{"gssdelegation", ForeignServerRelationId},
	{NULL, InvalidOid}
};


/*
 * 检查所给选项是否属于 libpq 的 conninfo 选项。
 * context 是选项来源目录的 OID；不在意则为 0。
 *
 */
static bool
is_conninfo_option(const char *option, Oid context)
{
	const struct ConnectionOption *opt;

	for (opt = libpq_conninfo_options; opt->optname; opt++)
		if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
			return true;
	return false;
}


/*
 * 校验交给 SERVER 或 USER MAPPING 的通用选项。
 * 若该选项或其值被视为无效，则报 ERROR。
 *
 * 服务器有效选项即 libpq 的 conninfo 各项，
 * user 与 password除外；二者只可在 USER MAPPING 选项中。
 *
 * 注意：此函数已弃用，现仅供测试
 * 之用，因它所知的选项列表未必
 * 与你正用的那个 libpq 实例所知相符。
 * 应改为询问 libpq 自身。
 */
Datum
postgresql_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);

	ListCell   *cell;

	foreach(cell, options_list)
	{
		DefElem    *def = lfirst(cell);

		if (!is_conninfo_option(def->defname, catalog))
		{
			const struct ConnectionOption *opt;
			const char *closest_match;
			ClosestMatchState match_state;
			bool		has_valid_options = false;

			/*
			 * 指定了未知选项就报错，并提示
			 * 一个形近的有效选项，若存在的话。
			 */
			initClosestMatch(&match_state, def->defname, 4);
			for (opt = libpq_conninfo_options; opt->optname; opt++)
			{
				if (catalog == opt->optcontext)
				{
					has_valid_options = true;
					updateClosestMatch(&match_state, opt->optname);
				}
			}

			closest_match = getClosestMatch(&match_state);
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid option \"%s\"", def->defname),
					 has_valid_options ? closest_match ?
					 errhint("Perhaps you meant the option \"%s\".",
							 closest_match) : 0 :
					 errhint("There are no valid options in this context.")));

			PG_RETURN_BOOL(false);
		}
	}

	PG_RETURN_BOOL(true);
}


/*
 * get_foreign_data_wrapper_oid：给定 FDW 名，查 OID。
 *
 *  missing_ok 为假则找不到名就报错，为真只返回 InvalidOid
 *
 */
Oid
get_foreign_data_wrapper_oid(const char *fdwname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(FOREIGNDATAWRAPPERNAME,
						  Anum_pg_foreign_data_wrapper_oid,
						  CStringGetDatum(fdwname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign-data wrapper \"%s\" does not exist",
						fdwname)));
	return oid;
}


/*
 * get_foreign_server_oid：给定服务器名，查 OID。
 *
 *  missing_ok 为假则找不到名就报错，为真只返回 InvalidOid
 *
 */
Oid
get_foreign_server_oid(const char *servername, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(FOREIGNSERVERNAME, Anum_pg_foreign_server_oid,
						  CStringGetDatum(servername));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("server \"%s\" does not exist", servername)));
	return oid;
}

/*
 * 取给定连接关系一条现有本地路径的副本。
 *
 * 本函数通常用来取得另一条供 EPQ 检查的本地路径。
 *
 *
 * 目前本函数只支持无参数的外部连接，因此我们
 * 只在给定路径表里搜无参数路径。因为我们
 * 要找能替外部连接构造替代本地计划的路径，
 * 故只找 MergeJoin、HashJoin 或 NestLoop 路径。
 *
 *
 * 若所选路径的内子路径或外子路径是 ForeignScan，就
 * 把它换成其外子路径。为此，也因为
 * 规划器稍后可能释放原路径，本函数返回的路径
 * 是原路径的浅拷贝。不必复制
 * 子结构，故不复制。
 *
 * 因为用此路径建成的计划大概只用来
 * 执行 EPQ 检查，路径效率无需操心。但因为
 *  RelOptInfo 里的路径表反正按总代价排序，我们多半会
 * 选中最有效的路径，那再好不过。
 */
Path *
GetExistingLocalJoinPath(RelOptInfo *joinrel)
{
	ListCell   *lc;

	Assert(IS_JOIN_REL(joinrel));

	foreach(lc, joinrel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);
		JoinPath   *joinpath = NULL;

		/* 跳过带参数的路径 */
		if (path->param_info != NULL)
			continue;

		switch (path->pathtype)
		{
			case T_HashJoin:
				{
					HashPath   *hash_path = makeNode(HashPath);

					memcpy(hash_path, path, sizeof(HashPath));
					joinpath = (JoinPath *) hash_path;
				}
				break;

			case T_NestLoop:
				{
					NestPath   *nest_path = makeNode(NestPath);

					memcpy(nest_path, path, sizeof(NestPath));
					joinpath = (JoinPath *) nest_path;
				}
				break;

			case T_MergeJoin:
				{
					MergePath  *merge_path = makeNode(MergePath);

					memcpy(merge_path, path, sizeof(MergePath));
					joinpath = (JoinPath *) merge_path;
				}
				break;

			default:

				/*
				 * 其余一律跳过。我们不知道对应计划是否
				 * 会用基关系的整行引用来构建输出行，
				 * 并执行 EPQ 检查。
				 */
				break;
		}

		/* 此路径不适合我们，看下一条 */
		if (!joinpath)
			continue;

		/*
		 * 若内路径或外路径是 ForeignPath，且对应某个
		 * 下推连接，就换成 fdw_outerpath，来维持
		 * 全由本地连接策略构成的 EPQ 检查路径。
		 *
		 */
		if (IsA(joinpath->outerjoinpath, ForeignPath))
		{
			ForeignPath *foreign_path;

			foreign_path = (ForeignPath *) joinpath->outerjoinpath;
			if (IS_JOIN_REL(foreign_path->path.parent))
				joinpath->outerjoinpath = foreign_path->fdw_outerpath;
		}

		if (IsA(joinpath->innerjoinpath, ForeignPath))
		{
			ForeignPath *foreign_path;

			foreign_path = (ForeignPath *) joinpath->innerjoinpath;
			if (IS_JOIN_REL(foreign_path->path.parent))
				joinpath->innerjoinpath = foreign_path->fdw_outerpath;
		}

		return (Path *) joinpath;
	}
	return NULL;
}
