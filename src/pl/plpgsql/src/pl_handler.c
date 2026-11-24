/*-------------------------------------------------------------------------
 *
 * pl_handler.c：PL/pgSQL 过程语言的
 *	处理函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/pl/plpgsql/src/pl_handler.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "plpgsql.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

static bool plpgsql_extra_checks_check_hook(char **newvalue, void **extra, GucSource source);
static void plpgsql_extra_warnings_assign_hook(const char *newvalue, void *extra);
static void plpgsql_extra_errors_assign_hook(const char *newvalue, void *extra);

PG_MODULE_MAGIC;

/* 定制 GUC 变量 */
static const struct config_enum_entry variable_conflict_options[] = {
	{"error", PLPGSQL_RESOLVE_ERROR, false},
	{"use_variable", PLPGSQL_RESOLVE_VARIABLE, false},
	{"use_column", PLPGSQL_RESOLVE_COLUMN, false},
	{NULL, 0, false}
};

int			plpgsql_variable_conflict = PLPGSQL_RESOLVE_ERROR;

bool		plpgsql_print_strict_params = false;

bool		plpgsql_check_asserts = true;

char	   *plpgsql_extra_warnings_string = NULL;
char	   *plpgsql_extra_errors_string = NULL;
int			plpgsql_extra_warnings;
int			plpgsql_extra_errors;

/* 插件钩子 */
PLpgSQL_plugin **plpgsql_plugin_ptr = NULL;


static bool
plpgsql_extra_checks_check_hook(char **newvalue, void **extra, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	int			extrachecks = 0;
	int		   *myextra;

	if (pg_strcasecmp(*newvalue, "all") == 0)
		extrachecks = PLPGSQL_XCHECK_ALL;
	else if (pg_strcasecmp(*newvalue, "none") == 0)
		extrachecks = PLPGSQL_XCHECK_NONE;
	else
	{
		/* 需可改的字符串副本 */
		rawstring = pstrdup(*newvalue);

		/* 把字符串解析成标识符列表 */
		if (!SplitIdentifierString(rawstring, ',', &elemlist))
		{
			/* 列表内语法错 */
			GUC_check_errdetail("List syntax is invalid.");
			pfree(rawstring);
			list_free(elemlist);
			return false;
		}

		foreach(l, elemlist)
		{
			char	   *tok = (char *) lfirst(l);

			if (pg_strcasecmp(tok, "shadowed_variables") == 0)
				extrachecks |= PLPGSQL_XCHECK_SHADOWVAR;
			else if (pg_strcasecmp(tok, "too_many_rows") == 0)
				extrachecks |= PLPGSQL_XCHECK_TOOMANYROWS;
			else if (pg_strcasecmp(tok, "strict_multi_assignment") == 0)
				extrachecks |= PLPGSQL_XCHECK_STRICTMULTIASSIGNMENT;
			else if (pg_strcasecmp(tok, "all") == 0 || pg_strcasecmp(tok, "none") == 0)
			{
				GUC_check_errdetail("Key word \"%s\" cannot be combined with other key words.", tok);
				pfree(rawstring);
				list_free(elemlist);
				return false;
			}
			else
			{
				GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
				pfree(rawstring);
				list_free(elemlist);
				return false;
			}
		}

		pfree(rawstring);
		list_free(elemlist);
	}

	myextra = (int *) guc_malloc(LOG, sizeof(int));
	if (!myextra)
		return false;
	*myextra = extrachecks;
	*extra = (void *) myextra;

	return true;
}

static void
plpgsql_extra_warnings_assign_hook(const char *newvalue, void *extra)
{
	plpgsql_extra_warnings = *((int *) extra);
}

static void
plpgsql_extra_errors_assign_hook(const char *newvalue, void *extra)
{
	plpgsql_extra_errors = *((int *) extra);
}


/*
 * _PG_init()：库加载时的初始化。
 *
 * 别改成 static，也别改它的名字！
 */
void
_PG_init(void)
{
	/* 确保初始化只做一次（现在本该是多余的） */
	static bool inited = false;

	if (inited)
		return;

	pg_bindtextdomain(TEXTDOMAIN);

	DefineCustomEnumVariable("plpgsql.variable_conflict",
							 gettext_noop("Sets handling of conflicts between PL/pgSQL variable names and table column names."),
							 NULL,
							 &plpgsql_variable_conflict,
							 PLPGSQL_RESOLVE_ERROR,
							 variable_conflict_options,
							 PGC_SUSET, 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("plpgsql.print_strict_params",
							 gettext_noop("Print information about parameters in the DETAIL part of the error messages generated on INTO ... STRICT failures."),
							 NULL,
							 &plpgsql_print_strict_params,
							 false,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("plpgsql.check_asserts",
							 gettext_noop("Perform checks given in ASSERT statements."),
							 NULL,
							 &plpgsql_check_asserts,
							 true,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("plpgsql.extra_warnings",
							   gettext_noop("List of programming constructs that should produce a warning."),
							   NULL,
							   &plpgsql_extra_warnings_string,
							   "none",
							   PGC_USERSET, GUC_LIST_INPUT,
							   plpgsql_extra_checks_check_hook,
							   plpgsql_extra_warnings_assign_hook,
							   NULL);

	DefineCustomStringVariable("plpgsql.extra_errors",
							   gettext_noop("List of programming constructs that should produce an error."),
							   NULL,
							   &plpgsql_extra_errors_string,
							   "none",
							   PGC_USERSET, GUC_LIST_INPUT,
							   plpgsql_extra_checks_check_hook,
							   plpgsql_extra_errors_assign_hook,
							   NULL);

	MarkGUCPrefixReserved("plpgsql");

	plpgsql_HashTableInit();
	RegisterXactCallback(plpgsql_xact_cb, NULL);
	RegisterSubXactCallback(plpgsql_subxact_cb, NULL);

	/* 与可选的插装插件建立一个会合点 */
	plpgsql_plugin_ptr = (PLpgSQL_plugin **) find_rendezvous_variable("PLpgSQL_plugin");

	inited = true;
}

/* ----------
 * plpgsql_call_handler
 *
 * PostgreSQL 函数管理器与触发器管理器
 * 调用本函数来执行 PL/pgSQL 过程。
 * ----------
 */
PG_FUNCTION_INFO_V1(plpgsql_call_handler);

Datum
plpgsql_call_handler(PG_FUNCTION_ARGS)
{
	bool		nonatomic;
	PLpgSQL_function *func;
	PLpgSQL_execstate *save_cur_estate;
	ResourceOwner procedure_resowner;
	volatile Datum retval = (Datum) 0;
	int			rc;

	nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;

	/*
	 * 连接 SPI 管理器
	 */
	if ((rc = SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0)) != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed: %s", SPI_result_code_string(rc));

	/* 查找或编译该函数 */
	func = plpgsql_compile(fcinfo, false);

	/* 必须保存并恢复 cur_estate 的原值 */
	save_cur_estate = func->cur_estate;

	/* 把函数标为忙，以免它在我们脚下被删掉 */
	func->use_count++;

	/*
	 * 若执行任何 CALL 或 DO 需要过程寿命的 resowner，
	 * 现在就创建。因该 resowner 不系于任何父级，
	 * 不释放它就会造成进程寿命的泄漏。
	 * 因此从这里到 PG_TRY 块之间，加代码要非常小心。
	 *
	 */
	procedure_resowner =
		(nonatomic && func->requires_procedure_resowner) ?
		ResourceOwnerCreate(NULL, "PL/pgSQL procedure resources") : NULL;

	PG_TRY();
	{
		/*
		 * 判断是作为函数还是触发器被调用，并调用相应
		 * 子例程
		 */
		if (CALLED_AS_TRIGGER(fcinfo))
			retval = PointerGetDatum(plpgsql_exec_trigger(func,
														  (TriggerData *) fcinfo->context));
		else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
		{
			plpgsql_exec_event_trigger(func,
									   (EventTriggerData *) fcinfo->context);
			/* 这种情况下没有返回值 */
		}
		else
			retval = plpgsql_exec_function(func, fcinfo,
										   NULL, NULL,
										   procedure_resowner,
										   !nonatomic);
	}
	PG_FINALLY();
	{
		/* 递减使用计数，恢复 cur_estate */
		func->use_count--;
		func->cur_estate = save_cur_estate;

		/* 若有过程 resowner，务必释放它 */
		if (procedure_resowner)
		{
			ResourceOwnerReleaseAllPlanCacheRefs(procedure_resowner);
			ResourceOwnerDelete(procedure_resowner);
		}
	}
	PG_END_TRY();

	/*
	 * 断开 SPI 管理器连接
	 */
	if ((rc = SPI_finish()) != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed: %s", SPI_result_code_string(rc));

	return retval;
}

/* ----------
 * plpgsql_inline_handler
 *
 * 由 PostgreSQL 调用来执行匿名代码块
 * ----------
 */
PG_FUNCTION_INFO_V1(plpgsql_inline_handler);

Datum
plpgsql_inline_handler(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(fake_fcinfo, 0);
	InlineCodeBlock *codeblock = castNode(InlineCodeBlock, DatumGetPointer(PG_GETARG_DATUM(0)));
	PLpgSQL_function *func;
	FmgrInfo	flinfo;
	EState	   *simple_eval_estate;
	ResourceOwner simple_eval_resowner;
	Datum		retval;
	int			rc;

	/*
	 * 连接 SPI 管理器
	 */
	if ((rc = SPI_connect_ext(codeblock->atomic ? 0 : SPI_OPT_NONATOMIC)) != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed: %s", SPI_result_code_string(rc));

	/* 编译该匿名代码块 */
	func = plpgsql_compile_inline(codeblock->source_text);

	/* 把函数标为忙，只是走个形式 */
	func->use_count++;

	/*
	 * 建一个假的 fcinfo，信息刚够满足
	 *  plpgsql_exec_function()。特别是这里
	 * 不传入任何参数。
	 */
	MemSet(fake_fcinfo, 0, SizeForFunctionCallInfo(0));
	MemSet(&flinfo, 0, sizeof(flinfo));
	fake_fcinfo->flinfo = &flinfo;
	flinfo.fn_oid = InvalidOid;
	flinfo.fn_mcxt = CurrentMemoryContext;

	/*
	 * 为简单表达式执行建私有 EState 与 resowner。
	 * 注意它们并不绑到事务级资源上；它们必须
	 * 熬过 DO 块执行的任何 COMMIT/ROLLBACK，因为下面
	 * 我们会无条件清理它们。（因此从这里到 PG_TRY
	 *  块之间，别加任何可能失败的东西。）见
	 *  shared_simple_eval_estate 的注释。
	 *
	 * 因为该 resowner 不绑到调用方事务，我们也
	 * 可以把它用作任何 CALL 语句的过程 resowner。这
	 * 有助于减少这里失败的机会。
	 */
	simple_eval_estate = CreateExecutorState();
	simple_eval_resowner =
		ResourceOwnerCreate(NULL, "PL/pgSQL DO block simple expressions");

	/* 再运行该函数 */
	PG_TRY();
	{
		retval = plpgsql_exec_function(func, fake_fcinfo,
									   simple_eval_estate,
									   simple_eval_resowner,
									   simple_eval_resowner,	/* 见上文 */
									   codeblock->atomic);
	}
	PG_CATCH();
	{
		/*
		 * 必须清理否则会长期存活的资源，它们是
		 * 失败的 DO 块积下来的，主要是语句的缓存计划
		 * 可用 plpgsql_free_function_memory 冲掉，
		 * 简单表达式的执行树（在私有 EState 里），以及
		 * 私有 resowner 持有的缓存计划引用计数。
		 *
		 * 释放私有 EState 之前，必须清掉任何指向
		 * 它内部的 simple_econtext_stack 项，这可以靠
		 * 调用子事务回调来做。（若外层稍后中止子事务
		 * 还会再调一次，但没有害处。）我们取巧，知道
		 *  plpgsql_subxact_cb 并不理会它的
		 *  parentSubid 参数。
		 */
		plpgsql_subxact_cb(SUBXACT_EVENT_ABORT_SUB,
						   GetCurrentSubTransactionId(),
						   0, NULL);

		/* 清理私有 EState 与 resowner */
		FreeExecutorState(simple_eval_estate);
		ResourceOwnerReleaseAllPlanCacheRefs(simple_eval_resowner);
		ResourceOwnerDelete(simple_eval_resowner);

		/* 函数现应无剩余使用计数…… */
		func->use_count--;
		Assert(func->use_count == 0);

		/* ……因此可以释放附属存储 */
		plpgsql_free_function_memory(func);

		/* 再把错误传出去 */
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* 清理私有 EState 与 resowner */
	FreeExecutorState(simple_eval_estate);
	ResourceOwnerReleaseAllPlanCacheRefs(simple_eval_resowner);
	ResourceOwnerDelete(simple_eval_resowner);

	/* 函数现应无剩余使用计数…… */
	func->use_count--;
	Assert(func->use_count == 0);

	/* ……因此可以释放附属存储 */
	plpgsql_free_function_memory(func);

	/*
	 * 断开 SPI 管理器连接
	 */
	if ((rc = SPI_finish()) != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed: %s", SPI_result_code_string(rc));

	return retval;
}

/* ----------
 * plpgsql_validator
 *
 * 本函数试图在 CREATE FUNCTION 时验证
 *  PL/pgSQL 函数。
 * ----------
 */
PG_FUNCTION_INFO_V1(plpgsql_validator);

Datum
plpgsql_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc proc;
	char		functyptype;
	int			numargs;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	bool		is_dml_trigger = false;
	bool		is_event_trigger = false;
	int			i;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	/* 取得新函数的 pg_proc 项 */
	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	functyptype = get_typtype(proc->prorettype);

	/* 禁止伪类型结果 */
	/* 但 TRIGGER、EVTTRIGGER、RECORD、VOID 或多态除外 */
	if (functyptype == TYPTYPE_PSEUDO)
	{
		if (proc->prorettype == TRIGGEROID)
			is_dml_trigger = true;
		else if (proc->prorettype == EVENT_TRIGGEROID)
			is_event_trigger = true;
		else if (proc->prorettype != RECORDOID &&
				 proc->prorettype != VOIDOID &&
				 !IsPolymorphicType(proc->prorettype))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("PL/pgSQL functions cannot return type %s",
							format_type_be(proc->prorettype))));
	}

	/* 禁止参数中的伪类型(无论 IN 或 OUT) */
	/* 但 RECORD 与多态除外 */
	numargs = get_func_arg_info(tuple,
								&argtypes, &argnames, &argmodes);
	for (i = 0; i < numargs; i++)
	{
		if (get_typtype(argtypes[i]) == TYPTYPE_PSEUDO)
		{
			if (argtypes[i] != RECORDOID &&
				!IsPolymorphicType(argtypes[i]))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("PL/pgSQL functions cannot accept type %s",
								format_type_be(argtypes[i]))));
		}
	}

	/* 若 !check_function_bodies 则延后查体 */
	if (check_function_bodies)
	{
		LOCAL_FCINFO(fake_fcinfo, 0);
		FmgrInfo	flinfo;
		int			rc;
		TriggerData trigdata;
		EventTriggerData etrigdata;

		/*
		 * 连接 SPI 管理器（编译需要这样做吗？）
		 */
		if ((rc = SPI_connect()) != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed: %s", SPI_result_code_string(rc));

		/*
		 * 建一个假 fcinfo，信息刚够满足
		 *  plpgsql_compile()
		 */
		MemSet(fake_fcinfo, 0, SizeForFunctionCallInfo(0));
		MemSet(&flinfo, 0, sizeof(flinfo));
		fake_fcinfo->flinfo = &flinfo;
		flinfo.fn_oid = funcoid;
		flinfo.fn_mcxt = CurrentMemoryContext;
		if (is_dml_trigger)
		{
			MemSet(&trigdata, 0, sizeof(trigdata));
			trigdata.type = T_TriggerData;
			fake_fcinfo->context = (Node *) &trigdata;
		}
		else if (is_event_trigger)
		{
			MemSet(&etrigdata, 0, sizeof(etrigdata));
			etrigdata.type = T_EventTriggerData;
			fake_fcinfo->context = (Node *) &etrigdata;
		}

		/* 试编译该函数 */
		plpgsql_compile(fake_fcinfo, true);

		/*
		 * 断开 SPI 管理器连接
		 */
		if ((rc = SPI_finish()) != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish failed: %s", SPI_result_code_string(rc));
	}

	ReleaseSysCache(tuple);

	PG_RETURN_VOID();
}
