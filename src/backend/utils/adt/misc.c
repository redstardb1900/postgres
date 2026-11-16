/*-------------------------------------------------------------------------
 *
 * misc.c
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/misc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/file.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <unistd.h>

#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "catalog/system_fk_info.h"
#include "commands/dbcommands.h"
#include "commands/tablespace.h"
#include "common/keywords.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "parser/parse_type.h"
#include "parser/scansup.h"
#include "pgstat.h"
#include "postmaster/syslogger.h"
#include "rewrite/rewriteHandler.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
#include "utils/timestamp.h"


/*
 * 用于缓存 pg_input_is_valid_common 所需元数据的结构
 */
typedef struct ValidIOData
{
	Oid			typoid;
	int32		typmod;
	bool		typname_constant;
	Oid			typiofunc;
	Oid			typioparam;
	FmgrInfo	inputproc;
} ValidIOData;

static bool pg_input_is_valid_common(FunctionCallInfo fcinfo,
									 text *txt, text *typname,
									 ErrorSaveContext *escontext);


/*
 * num_nulls() 与 num_nonnulls() 的公共子程序。
 * 成功时返回 true，函数应返回 NULL 时返回 false。
 * 成功时，参数总个数与 NULL 个数通过
 * *nargs 与 *nulls 返回。
 */
static bool
count_nulls(FunctionCallInfo fcinfo,
			int32 *nargs, int32 *nulls)
{
	int32		count = 0;
	int			i;

	/* 得到 VARIADIC 数组参数，还是分开的参数？ */
	if (get_fn_expr_variadic(fcinfo->flinfo))
	{
		ArrayType  *arr;
		int			ndims,
					nitems,
				   *dims;
		bits8	   *bitmap;

		Assert(PG_NARGS() == 1);

		/*
		 * 若 VARIADIC 数组参数为 NULL，则无法就元素个数
		 * 给出任何有用的信息，因此返回 NULL。此行为
		 * 与其他 variadic 函数一致 - 参见 concat_internal。
		 */
		if (PG_ARGISNULL(0))
			return false;

		/*
		 * 非 NULL 参数必须是数组。我们假定任何
		 * 能让 get_fn_expr_variadic 返回 true 的调用上下文都
		 * 已检查过带 VARIADIC 标记的参数确实是数组。因此
		 * 直接用 Assert 断言它是数组即可，而无需
		 * 做完整的错误检查。
		 */
		Assert(OidIsValid(get_base_element_type(get_fn_expr_argtype(fcinfo->flinfo, 0))));

		/* OK，可安全获取数组值 */
		arr = PG_GETARG_ARRAYTYPE_P(0);

		/* 统计数组元素个数 */
		ndims = ARR_NDIM(arr);
		dims = ARR_DIMS(arr);
		nitems = ArrayGetNItems(ndims, dims);

		/* 统计为 NULL 的项 */
		bitmap = ARR_NULLBITMAP(arr);
		if (bitmap)
		{
			int			bitmask = 1;

			for (i = 0; i < nitems; i++)
			{
				if ((*bitmap & bitmask) == 0)
					count++;

				bitmask <<= 1;
				if (bitmask == 0x100)
				{
					bitmap++;
					bitmask = 1;
				}
			}
		}

		*nargs = nitems;
		*nulls = count;
	}
	else
	{
		/* 参数分开给出，直接计数 */
		for (i = 0; i < PG_NARGS(); i++)
		{
			if (PG_ARGISNULL(i))
				count++;
		}

		*nargs = PG_NARGS();
		*nulls = count;
	}

	return true;
}

/*
 * num_nulls()
 *	统计 NULL 参数的个数
 */
Datum
pg_num_nulls(PG_FUNCTION_ARGS)
{
	int32		nargs,
				nulls;

	if (!count_nulls(fcinfo, &nargs, &nulls))
		PG_RETURN_NULL();

	PG_RETURN_INT32(nulls);
}

/*
 * num_nonnulls()
 *	统计非 NULL 参数的个数
 */
Datum
pg_num_nonnulls(PG_FUNCTION_ARGS)
{
	int32		nargs,
				nulls;

	if (!count_nulls(fcinfo, &nargs, &nulls))
		PG_RETURN_NULL();

	PG_RETURN_INT32(nargs - nulls);
}


/*
 * current_database()
 *	将当前数据库暴露给用户
 */
Datum
current_database(PG_FUNCTION_ARGS)
{
	Name		db;

	db = (Name) palloc(NAMEDATALEN);

	namestrcpy(db, get_database_name(MyDatabaseId));
	PG_RETURN_NAME(db);
}


/*
 * current_query()
 *	将当前查询暴露给用户（在存储过程中有用）
 *	将来或许可以使用 ActivePortal->sourceText。
 */
Datum
current_query(PG_FUNCTION_ARGS)
{
	/* 没有简便方法可访问更简洁的 'query_string' */
	if (debug_query_string)
		PG_RETURN_TEXT_P(cstring_to_text(debug_query_string));
	else
		PG_RETURN_NULL();
}

/* 找出哪些数据库在使用某表空间的函数 */

Datum
pg_tablespace_databases(PG_FUNCTION_ARGS)
{
	Oid			tablespaceOid = PG_GETARG_OID(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *location;
	DIR		   *dirdesc;
	struct dirent *de;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	if (tablespaceOid == GLOBALTABLESPACE_OID)
	{
		ereport(WARNING,
				(errmsg("global tablespace never has databases")));
		/* 返回空 tuplestore */
		return (Datum) 0;
	}

	if (tablespaceOid == DEFAULTTABLESPACE_OID)
		location = "base";
	else
		location = psprintf("pg_tblspc/%u/%s", tablespaceOid,
							TABLESPACE_VERSION_DIRECTORY);

	dirdesc = AllocateDir(location);

	if (!dirdesc)
	{
		/* 预期中唯一错误是 ENOENT */
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open directory \"%s\": %m",
							location)));
		ereport(WARNING,
				(errmsg("%u is not a tablespace OID", tablespaceOid)));
		/* 返回空 tuplestore */
		return (Datum) 0;
	}

	while ((de = ReadDir(dirdesc, location)) != NULL)
	{
		Oid			datOid = atooid(de->d_name);
		char	   *subdir;
		bool		isempty;
		Datum		values[1];
		bool		nulls[1];

		/* 该测试跳过 . 和 ..，但非常不严谨 */
		if (!datOid)
			continue;

		/* 若数据库子目录为空，则不报告表空间被使用 */

		subdir = psprintf("%s/%s", location, de->d_name);
		isempty = directory_is_empty(subdir);
		pfree(subdir);

		if (isempty)
			continue;			/* 确实为空 */

		values[0] = ObjectIdGetDatum(datOid);
		nulls[0] = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	FreeDir(dirdesc);
	return (Datum) 0;
}


/*
 * pg_tablespace_location - 获取表空间的位置
 */
Datum
pg_tablespace_location(PG_FUNCTION_ARGS)
{
	Oid			tablespaceOid = PG_GETARG_OID(0);
	char		sourcepath[MAXPGPATH];
	char		targetpath[MAXPGPATH];
	int			rllen;
	struct stat st;

	/*
	 * 将本函数用于 pg_class.reltablespace 很有用，其中
	 * 零表示"数据库的默认表空间"。因此，与其
	 * 对零抛出错误，我们选择假定这就是其含义。
	 */
	if (tablespaceOid == InvalidOid)
		tablespaceOid = MyDatabaseTableSpace;

	/*
	 * 对集簇的默认表空间返回空字符串
	 */
	if (tablespaceOid == DEFAULTTABLESPACE_OID ||
		tablespaceOid == GLOBALTABLESPACE_OID)
		PG_RETURN_TEXT_P(cstring_to_text(""));

	/*
	 * 通过读取符号链接找到表空间的位置，该链接位于 
	 * pg_tblspc/<oid> 中。
	 */
	snprintf(sourcepath, sizeof(sourcepath), "pg_tblspc/%u", tablespaceOid);

	/*
	 * 读取链接之前，先检查源路径是链接还是
	 * 联接点。注意，启用 allow_in_place_tablespaces 的表空间
	 * 可能是目录。若发现的是目录，
	 * 则返回相对于数据目录的路径。
	 */
	if (lstat(sourcepath, &st) < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						sourcepath)));
	}

	if (!S_ISLNK(st.st_mode))
		PG_RETURN_TEXT_P(cstring_to_text(sourcepath));

	/*
	 * 若存在链接或联接点，则返回其指向的路径。
	 */
	rllen = readlink(sourcepath, targetpath, sizeof(targetpath));
	if (rllen < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read symbolic link \"%s\": %m",
						sourcepath)));
	if (rllen >= sizeof(targetpath))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("symbolic link \"%s\" target is too long",
						sourcepath)));
	targetpath[rllen] = '\0';

	PG_RETURN_TEXT_P(cstring_to_text(targetpath));
}

/*
 * pg_sleep - 延迟 N 秒
 */
Datum
pg_sleep(PG_FUNCTION_ARGS)
{
	float8		secs = PG_GETARG_FLOAT8(0);
	float8		endtime;

	/*
	 * 我们通过 WaitLatch 睡眠，以确保在收到重要信号
	 * （如 SIGALRM 或 SIGINT）时能立即醒来。由于 
	 * WaitLatch 的延迟上限是 INT_MAX 毫秒，而用户可能
	 * 要求更长的延迟，因此每次最多睡 10 分钟后循环
	 * 
	 *
	 * 通过一开始就计算预期的停止时刻，我们避免了多次
	 * 睡眠之间额外延迟的累积。这也确保了当 WaitLatch 被
	 * 非查询取消类信号（如 SIGHUP）提前终止时，
	 * 延迟时间也不会短于指定值。
	 */
#define GetNowFloat()	((float8) GetCurrentTimestamp() / 1000000.0)

	endtime = GetNowFloat() + secs;

	for (;;)
	{
		float8		delay;
		long		delay_ms;

		CHECK_FOR_INTERRUPTS();

		delay = endtime - GetNowFloat();
		if (delay >= 600.0)
			delay_ms = 600000;
		else if (delay > 0.0)
			delay_ms = (long) ceil(delay * 1000.0);
		else
			break;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 delay_ms,
						 WAIT_EVENT_PG_SLEEP);
		ResetLatch(MyLatch);
	}

	PG_RETURN_VOID();
}

/* 返回语法关键字列表的函数 */
Datum
pg_get_keywords(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = tupdesc;
		funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < ScanKeywords.num_keywords)
	{
		char	   *values[5];
		HeapTuple	tuple;

		/* cast-away-const 有点丑，但没有更好的替代 */
		values[0] = unconstify(char *,
							   GetScanKeyword(funcctx->call_cntr,
											  &ScanKeywords));

		switch (ScanKeywordCategories[funcctx->call_cntr])
		{
			case UNRESERVED_KEYWORD:
				values[1] = "U";
				values[3] = _("unreserved");
				break;
			case COL_NAME_KEYWORD:
				values[1] = "C";
				values[3] = _("unreserved (cannot be function or type name)");
				break;
			case TYPE_FUNC_NAME_KEYWORD:
				values[1] = "T";
				values[3] = _("reserved (can be function or type name)");
				break;
			case RESERVED_KEYWORD:
				values[1] = "R";
				values[3] = _("reserved");
				break;
			default:			/* 不应发生 */
				values[1] = NULL;
				values[3] = NULL;
				break;
		}

		if (ScanKeywordBareLabel[funcctx->call_cntr])
		{
			values[2] = "true";
			values[4] = _("can be bare label");
		}
		else
		{
			values[2] = "false";
			values[4] = _("requires AS");
		}

		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}


/* 返回系统目录外键关系列表的函数 */
Datum
pg_get_catalog_foreign_keys(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	FmgrInfo   *arrayinp;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/*
		 * 我们用 array_in 把 sys_fk_relationships[] 中的 C 字符串
		 * 转换为 text 数组，但无法用 DirectFunctionCallN 来
		 * 调用 array_in，即便能做效率也不高。填充一个
		 * 供调用的 FmgrInfo。
		 */
		arrayinp = (FmgrInfo *) palloc(sizeof(FmgrInfo));
		fmgr_info(F_ARRAY_IN, arrayinp);
		funcctx->user_fctx = arrayinp;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	arrayinp = (FmgrInfo *) funcctx->user_fctx;

	if (funcctx->call_cntr < lengthof(sys_fk_relationships))
	{
		const SysFKRelationship *fkrel = &sys_fk_relationships[funcctx->call_cntr];
		Datum		values[6];
		bool		nulls[6];
		HeapTuple	tuple;

		memset(nulls, false, sizeof(nulls));

		values[0] = ObjectIdGetDatum(fkrel->fk_table);
		values[1] = FunctionCall3(arrayinp,
								  CStringGetDatum(fkrel->fk_columns),
								  ObjectIdGetDatum(TEXTOID),
								  Int32GetDatum(-1));
		values[2] = ObjectIdGetDatum(fkrel->pk_table);
		values[3] = FunctionCall3(arrayinp,
								  CStringGetDatum(fkrel->pk_columns),
								  ObjectIdGetDatum(TEXTOID),
								  Int32GetDatum(-1));
		values[4] = BoolGetDatum(fkrel->is_array);
		values[5] = BoolGetDatum(fkrel->is_opt);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}


/*
 * 返回参数的类型。
 */
Datum
pg_typeof(PG_FUNCTION_ARGS)
{
	PG_RETURN_OID(get_fn_expr_argtype(fcinfo->flinfo, 0));
}


/*
 * COLLATE FOR 表达式的实现；返回参数的
 * 排序规则。
 */
Datum
pg_collation_for(PG_FUNCTION_ARGS)
{
	Oid			typeid;
	Oid			collid;

	typeid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (!typeid)
		PG_RETURN_NULL();
	if (!type_is_collatable(typeid) && typeid != UNKNOWNOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("collations are not supported by type %s",
						format_type_be(typeid))));

	collid = PG_GET_COLLATION();
	if (!collid)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(generate_collation_name(collid)));
}


/*
 * pg_relation_is_updatable - 判断指定关系支持哪些
 * 更新事件。
 *
 * 这依赖 rewriteHandler.c 中的 relation_is_updatable()，
 * 更多信息见该处。
 */
Datum
pg_relation_is_updatable(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);
	bool		include_triggers = PG_GETARG_BOOL(1);

	PG_RETURN_INT32(relation_is_updatable(reloid, NIL, include_triggers, NULL));
}

/*
 * pg_column_is_updatable - 判断列是否可更新。
 *
 * 本函数封装了 information_schema.columns.
 * is_updatable 究竟意味着什么。尚不清楚是否也应要求
 * 列所属关系是可删除的，所以我们希望将这一决策
 * 放在 C 代码中，这样无需 initdb 就能修改它。
 */
Datum
pg_column_is_updatable(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);
	AttrNumber	attnum = PG_GETARG_INT16(1);
	AttrNumber	col = attnum - FirstLowInvalidHeapAttributeNumber;
	bool		include_triggers = PG_GETARG_BOOL(2);
	int			events;

	/* 系统列永远不可更新 */
	if (attnum <= 0)
		PG_RETURN_BOOL(false);

	events = relation_is_updatable(reloid, NIL, include_triggers,
								   bms_make_singleton(col));

	/* 我们要求关系同时具备可更新性与可删除性 */
#define REQ_EVENTS ((1 << CMD_UPDATE) | (1 << CMD_DELETE))

	PG_RETURN_BOOL((events & REQ_EVENTS) == REQ_EVENTS);
}


/*
 * pg_input_is_valid - 测试字符串是否为类型的有效输入。
 *
 * 有效则 true，否则 false。
 *
 * 只有当数据类型的输入函数已改为通过 errsave/ereturn
 * 返回"软"错误时才能发挥作用。
 */
Datum
pg_input_is_valid(PG_FUNCTION_ARGS)
{
	text	   *txt = PG_GETARG_TEXT_PP(0);
	text	   *typname = PG_GETARG_TEXT_PP(1);
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	PG_RETURN_BOOL(pg_input_is_valid_common(fcinfo, txt, typname,
											&escontext));
}

/*
 * pg_input_error_info - 测试字符串是否为类型的有效输入。
 *
 * 正常时返回 NULL，否则返回主消息、详细消息、提示
 * 消息以及 SQL 错误码。
 *
 * 只有当数据类型的输入函数已改为通过 errsave/ereturn
 * 返回"软"错误时才能发挥作用。
 */
Datum
pg_input_error_info(PG_FUNCTION_ARGS)
{
	text	   *txt = PG_GETARG_TEXT_PP(0);
	text	   *typname = PG_GETARG_TEXT_PP(1);
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		isnull[4];

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* 启用 details_wanted */
	escontext.details_wanted = true;

	if (pg_input_is_valid_common(fcinfo, txt, typname,
								 &escontext))
		memset(isnull, true, sizeof(isnull));
	else
	{
		char	   *sqlstate;

		Assert(escontext.error_occurred);
		Assert(escontext.error_data != NULL);
		Assert(escontext.error_data->message != NULL);

		memset(isnull, false, sizeof(isnull));

		values[0] = CStringGetTextDatum(escontext.error_data->message);

		if (escontext.error_data->detail != NULL)
			values[1] = CStringGetTextDatum(escontext.error_data->detail);
		else
			isnull[1] = true;

		if (escontext.error_data->hint != NULL)
			values[2] = CStringGetTextDatum(escontext.error_data->hint);
		else
			isnull[2] = true;

		sqlstate = unpack_sql_state(escontext.error_data->sqlerrcode);
		values[3] = CStringGetTextDatum(sqlstate);
	}

	return HeapTupleGetDatum(heap_form_tuple(tupdesc, values, isnull));
}

/* 上述函数的公共子程序 */
static bool
pg_input_is_valid_common(FunctionCallInfo fcinfo,
						 text *txt, text *typname,
						 ErrorSaveContext *escontext)
{
	char	   *str = text_to_cstring(txt);
	ValidIOData *my_extra;
	Datum		converted;

	/*
	 * 我们安排在每轮调用序列中只查找一次所需的 
	 * I/O 信息，前提是数据类型不在中途改变。
	 */
	my_extra = (ValidIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   sizeof(ValidIOData));
		my_extra = (ValidIOData *) fcinfo->flinfo->fn_extra;
		my_extra->typoid = InvalidOid;
		/* 检测 typname 参数是否为常量。 */
		my_extra->typname_constant = get_fn_expr_arg_stable(fcinfo->flinfo, 1);
	}

	/*
	 * 若 typname 参数是常量，则只需在第一次调用时
	 * 解析它。
	 */
	if (my_extra->typoid == InvalidOid || !my_extra->typname_constant)
	{
		char	   *typnamestr = text_to_cstring(typname);
		Oid			typoid;

		/* 解析类型名参数得到类型 OID 和 typmod 编码值。 */
		(void) parseTypeString(typnamestr, &typoid, &my_extra->typmod, NULL);

		/* typoid 变化时更新类型专属信息。 */
		if (my_extra->typoid != typoid)
		{
			getTypeInputInfo(typoid,
							 &my_extra->typiofunc,
							 &my_extra->typioparam);
			fmgr_info_cxt(my_extra->typiofunc, &my_extra->inputproc,
						  fcinfo->flinfo->fn_mcxt);
			my_extra->typoid = typoid;
		}
	}

	/* 现在可以尝试执行转换了。 */
	return InputFunctionCallSafe(&my_extra->inputproc,
								 str,
								 my_extra->typioparam,
								 my_extra->typmod,
								 (Node *) escontext,
								 &converted);
}


/*
 * 字符为有效标识符首字符？
 * 须与 scan.l 的 {ident_start} 字符类一致。
 */
static bool
is_ident_start(unsigned char c)
{
	/* 下划线和 ASCII 字母均可 */
	if (c == '_')
		return true;
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
		return true;
	/* 任何高位字符均可（可能是多字节字符的一部分） */
	if (IS_HIGHBIT_SET(c))
		return true;
	return false;
}

/*
 * 字符为有效的标识符后续字符吗？
 * 须与 scan.l 的 {ident_cont} 字符类一致。
 */
static bool
is_ident_cont(unsigned char c)
{
	/* 可为数字或美元符号 ... */
	if ((c >= '0' && c <= '9') || c == '$')
		return true;
	/* ... 或一个标识符起始字符 */
	return is_ident_start(c);
}

/*
 * parse_ident - 将 SQL 限定标识符解析成多个标识符。
 * 启用严格模式时（第二个参数），最后一个标识符
 * 之后的字符均不允许。
 */
Datum
parse_ident(PG_FUNCTION_ARGS)
{
	text	   *qualname = PG_GETARG_TEXT_PP(0);
	bool		strict = PG_GETARG_BOOL(1);
	char	   *qualname_str = text_to_cstring(qualname);
	ArrayBuildState *astate = NULL;
	char	   *nextp;
	bool		after_dot = false;

	/*
	 * 下面代码在某些情况下会改写 qualname_str，所以
	 * 若需在错误消息中显示原串，则重新转换 qualname。
	 * 
	 */
	nextp = qualname_str;

	/* 跳过前导空白 */
	while (scanner_isspace(*nextp))
		nextp++;

	for (;;)
	{
		char	   *curname;
		bool		missing_ident = true;

		if (*nextp == '"')
		{
			char	   *endp;

			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '"');
				if (endp == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("string is not a valid identifier: \"%s\"",
									text_to_cstring(qualname)),
							 errdetail("String has unclosed double quotes.")));
				if (endp[1] != '"')
					break;
				memmove(endp, endp + 1, strlen(endp));
				nextp = endp;
			}
			nextp = endp + 1;
			*endp = '\0';

			if (endp - curname == 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("string is not a valid identifier: \"%s\"",
								text_to_cstring(qualname)),
						 errdetail("Quoted identifier must not be empty.")));

			astate = accumArrayResult(astate, CStringGetTextDatum(curname),
									  false, TEXTOID, CurrentMemoryContext);
			missing_ident = false;
		}
		else if (is_ident_start((unsigned char) *nextp))
		{
			char	   *downname;
			int			len;
			text	   *part;

			curname = nextp++;
			while (is_ident_cont((unsigned char) *nextp))
				nextp++;

			len = nextp - curname;

			/*
			 * 我们不隐式截断标识符。这便于用户检查
			 * 标识符的特定部分是否过长。要让用户获得截断
			 * 的名称也很容易：只要把我们的输出
			 * 转型为 name[] 即可。
			 */
			downname = downcase_identifier(curname, len, false, false);
			part = cstring_to_text_with_len(downname, len);
			astate = accumArrayResult(astate, PointerGetDatum(part), false,
									  TEXTOID, CurrentMemoryContext);
			missing_ident = false;
		}

		if (missing_ident)
		{
			/* 根据失败位置给出不同的错误消息。 */
			if (*nextp == '.')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("string is not a valid identifier: \"%s\"",
								text_to_cstring(qualname)),
						 errdetail("No valid identifier before \".\".")));
			else if (after_dot)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("string is not a valid identifier: \"%s\"",
								text_to_cstring(qualname)),
						 errdetail("No valid identifier after \".\".")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("string is not a valid identifier: \"%s\"",
								text_to_cstring(qualname))));
		}

		while (scanner_isspace(*nextp))
			nextp++;

		if (*nextp == '.')
		{
			after_dot = true;
			nextp++;
			while (scanner_isspace(*nextp))
				nextp++;
		}
		else if (*nextp == '\0')
		{
			break;
		}
		else
		{
			if (strict)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("string is not a valid identifier: \"%s\"",
								text_to_cstring(qualname))));
			break;
		}
	}

	PG_RETURN_DATUM(makeArrayResult(astate, CurrentMemoryContext));
}

/*
 * pg_current_logfile
 *
 * 扫描 current_logfiles 以报告日志收集器当前所用日志文件。
 */
Datum
pg_current_logfile(PG_FUNCTION_ARGS)
{
	FILE	   *fd;
	char		lbuffer[MAXPGPATH];
	char	   *logfmt;

	/* 日志格式参数是可选的 */
	if (PG_NARGS() == 0 || PG_ARGISNULL(0))
		logfmt = NULL;
	else
	{
		logfmt = text_to_cstring(PG_GETARG_TEXT_PP(0));

		if (strcmp(logfmt, "stderr") != 0 &&
			strcmp(logfmt, "csvlog") != 0 &&
			strcmp(logfmt, "jsonlog") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("log format \"%s\" is not supported", logfmt),
					 errhint("The supported log formats are \"stderr\", \"csvlog\", and \"jsonlog\".")));
	}

	fd = AllocateFile(LOG_METAINFO_DATAFILE, "r");
	if (fd == NULL)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							LOG_METAINFO_DATAFILE)));
		PG_RETURN_NULL();
	}

#ifdef WIN32
	/* syslogger.c 在 Windows 上写入 CRLF 行尾 */
	_setmode(_fileno(fd), _O_TEXT);
#endif

	/*
	 * 读取该文件收集 syslogger 注册的当前日志文件名。
	 * 
	 */
	while (fgets(lbuffer, sizeof(lbuffer), fd) != NULL)
	{
		char	   *log_format;
		char	   *log_filepath;
		char	   *nlpos;

		/* 从该行提取日志格式和日志文件路径。 */
		log_format = lbuffer;
		log_filepath = strchr(lbuffer, ' ');
		if (log_filepath == NULL)
		{
			/* 糟糕，未找到空格，文件内容已损坏。 */
			elog(ERROR,
				 "missing space character in \"%s\"", LOG_METAINFO_DATAFILE);
			break;
		}

		*log_filepath = '\0';
		log_filepath++;
		nlpos = strchr(log_filepath, '\n');
		if (nlpos == NULL)
		{
			/* 糟糕，未找到换行符，文件内容已损坏。 */
			elog(ERROR,
				 "missing newline character in \"%s\"", LOG_METAINFO_DATAFILE);
			break;
		}
		*nlpos = '\0';

		if (logfmt == NULL || strcmp(logfmt, log_format) == 0)
		{
			FreeFile(fd);
			PG_RETURN_TEXT_P(cstring_to_text(log_filepath));
		}
	}

	/* 关闭当前日志文件名文件。 */
	FreeFile(fd);

	PG_RETURN_NULL();
}

/*
 * 报告日志收集器在用的日志文件（单参数版本）
 *
 * note: 此包装是通过 opr_sanity 一致性检查所必需的，
 * 该检查会验证共享同一实现 C 函数的所有内建函数
 * 具有相同的参数个数
 */
Datum
pg_current_logfile_1arg(PG_FUNCTION_ARGS)
{
	return pg_current_logfile(fcinfo);
}

/*
 * RelationGetReplicaIndex() 的 SQL 包装。
 */
Datum
pg_get_replica_identity_index(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);
	Oid			idxoid;
	Relation	rel;

	rel = table_open(reloid, AccessShareLock);
	idxoid = RelationGetReplicaIndex(rel);
	table_close(rel, AccessShareLock);

	if (OidIsValid(idxoid))
		PG_RETURN_OID(idxoid);
	else
		PG_RETURN_NULL();
}

/*
 * ANY_VALUE 聚合的转移函数
 */
Datum
any_value_transfn(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}
