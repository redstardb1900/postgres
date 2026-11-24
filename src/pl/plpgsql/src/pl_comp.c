/*-------------------------------------------------------------------------
 *
 * pl_comp.c：PL/pgSQL 的
 *	过程语言编译器
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/pl/plpgsql/src/pl_comp.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "plpgsql.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* ----------
 * 自有的局部与全局变量
 * ----------
 */
PLpgSQL_stmt_block *plpgsql_parse_result;

static int	datums_alloc;
int			plpgsql_nDatums;
PLpgSQL_datum **plpgsql_Datums;
static int	datums_last;

char	   *plpgsql_error_funcname;
bool		plpgsql_DumpExecTree = false;
bool		plpgsql_check_syntax = false;

PLpgSQL_function *plpgsql_curr_compile;

/* 编译期间短期分配适用的上下文 */
MemoryContext plpgsql_compile_tmp_cxt;

/* ----------
 * 已编译函数的散列表
 * ----------
 */
static HTAB *plpgsql_HashTable = NULL;

typedef struct plpgsql_hashent
{
	PLpgSQL_func_hashkey key;
	PLpgSQL_function *function;
} plpgsql_HashEnt;

#define FUNCS_PER_USER		128 /* 表的初始大小 */

/* ----------
 * EXCEPTION 条件名查找表
 * ----------
 */
typedef struct
{
	const char *label;
	int			sqlerrstate;
} ExceptionLabelMap;

static const ExceptionLabelMap exception_label_map[] = {
#include "plerrcodes.h"			/* pgrminclude ignore */
	{NULL, 0}
};


/* ----------
 * 静态原型
 * ----------
 */
static PLpgSQL_function *do_compile(FunctionCallInfo fcinfo,
									HeapTuple procTup,
									PLpgSQL_function *function,
									PLpgSQL_func_hashkey *hashkey,
									bool forValidator);
static void plpgsql_compile_error_callback(void *arg);
static void add_parameter_name(PLpgSQL_nsitem_type itemtype, int itemno, const char *name);
static void add_dummy_return(PLpgSQL_function *function);
static Node *plpgsql_pre_column_ref(ParseState *pstate, ColumnRef *cref);
static Node *plpgsql_post_column_ref(ParseState *pstate, ColumnRef *cref, Node *var);
static Node *plpgsql_param_ref(ParseState *pstate, ParamRef *pref);
static Node *resolve_column_ref(ParseState *pstate, PLpgSQL_expr *expr,
								ColumnRef *cref, bool error_if_no_field);
static Node *make_datum_param(PLpgSQL_expr *expr, int dno, int location);
static PLpgSQL_row *build_row_from_vars(PLpgSQL_variable **vars, int numvars);
static PLpgSQL_type *build_datatype(HeapTuple typeTup, int32 typmod,
									Oid collation, TypeName *origtypname);
static void plpgsql_start_datums(void);
static void plpgsql_finish_datums(PLpgSQL_function *function);
static void compute_function_hashkey(FunctionCallInfo fcinfo,
									 Form_pg_proc procStruct,
									 PLpgSQL_func_hashkey *hashkey,
									 bool forValidator);
static void plpgsql_resolve_polymorphic_argtypes(int numargs,
												 Oid *argtypes, char *argmodes,
												 Node *call_expr, bool forValidator,
												 const char *proname);
static PLpgSQL_function *plpgsql_HashTableLookup(PLpgSQL_func_hashkey *func_key);
static void plpgsql_HashTableInsert(PLpgSQL_function *function,
									PLpgSQL_func_hashkey *func_key);
static void plpgsql_HashTableDelete(PLpgSQL_function *function);
static void delete_function(PLpgSQL_function *func);

/* ----------
 * plpgsql_compile：为 PL/pgSQL 函数建执行树。
 *
 * 若 forValidator 为真，则只为校验而编译，
 * 因此跳过一些检查。
 *
 * 注意：若函数已编译，这里必须很快走完，
 * 这一点很重要。
 * ----------
 */
PLpgSQL_function *
plpgsql_compile(FunctionCallInfo fcinfo, bool forValidator)
{
	Oid			funcOid = fcinfo->flinfo->fn_oid;
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	PLpgSQL_function *function;
	PLpgSQL_func_hashkey hashkey;
	bool		function_valid = false;
	bool		hashkey_valid = false;

	/*
	 * 按 Oid 查 pg_proc 元组；无论怎样都要用
	 */
	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcOid));
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", funcOid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/*
	 * 看当前 FmgrInfo 是否已有缓存项。若无，
	 * 就到散列表里找一个。
	 */
	function = (PLpgSQL_function *) fcinfo->flinfo->fn_extra;

recheck:
	if (!function)
	{
		/* 用函数签名和实际参数类型算散列键 */
		compute_function_hashkey(fcinfo, procStruct, &hashkey, forValidator);
		hashkey_valid = true;

		/* 然后查找 */
		function = plpgsql_HashTableLookup(&hashkey);
	}

	if (function)
	{
		/* 已有编译好的函数，但它仍有效吗？ */
		if (function->fn_xmin == HeapTupleHeaderGetRawXmin(procTup->t_data) &&
			ItemPointerEquals(&function->fn_tid, &procTup->t_self))
			function_valid = true;
		else
		{
			/*
			 * 否，就从散列表删掉，并尝试丢弃附属
			 * 存储（若尚未做过）。
			 */
			delete_function(function);

			/*
			 * 函数若不在活跃使用中，我们就可以覆盖
			 * 该函数结构并写入新数据，让其他已有的
			 * fn_extra 指针在下次使用时用上新定义。
			 * 若正在使用就别动它并另建一个（
			 * 活跃调用会用旧定义跑完，
			 * 之后该缓存项只会泄漏；不值得
			 * 加代码清理，鉴于这只是
			 * 边角情况）。
			 *
			 * 若函数结构是经 fn_extra 找到的，则有可能
			 * 替换已经做好，所以回去复查散列表。
			 *
			 */
			if (function->use_count != 0)
			{
				function = NULL;
				if (!hashkey_valid)
					goto recheck;
			}
		}
	}

	/*
	 * 若未找到函数或已过期，就必须编译它
	 */
	if (!function_valid)
	{
		/*
		 * 若还没有散列键就现在算，要用它来存
		 * 完成的函数。
		 */
		if (!hashkey_valid)
			compute_function_hashkey(fcinfo, procStruct, &hashkey,
									 forValidator);

		/*
		 * 做困难部分
		 */
		function = do_compile(fcinfo, procTup, function,
							  &hashkey, forValidator);
	}

	ReleaseSysCache(procTup);

	/*
	 * 把指针存进 FmgrInfo，免随后调用再查找。
	 */
	fcinfo->flinfo->fn_extra = (void *) function;

	/*
	 * 最后返回编译好的函数。
	 */
	return function;
}

/*
 * 这是 plpgsql_compile() 的慢速部分。
 *
 * 传入的 function 指针或为 NULL，或是已分配、
 * 待覆盖的函数结构。
 *
 * 编译函数时，CurrentMemoryContext 是
 * 正编译的那个函数的每函数内存上下文。这
 * 意味着 palloc() 分到的存储，寿命同于
 * 函数本身。
 *
 * 因 palloc() 的存储不会立刻释放，临时
 * 分配应放在短命内存上下文里做，
 * 或显式 pfree。并非每个 backend 函数
 * 都小心释放，故进 backend 之前
 * 宜先切到短期上下文。
 * 做短期分配的合适上下文是
 * plpgsql_compile_tmp_cxt。
 *
 * 注意：此代码不可重入。假定这里做的任何事
 * 都不会导致再调用另一个 plpgsql 函数。
 */
static PLpgSQL_function *
do_compile(FunctionCallInfo fcinfo,
		   HeapTuple procTup,
		   PLpgSQL_function *function,
		   PLpgSQL_func_hashkey *hashkey,
		   bool forValidator)
{
	Form_pg_proc procStruct = (Form_pg_proc) GETSTRUCT(procTup);
	bool		is_dml_trigger = CALLED_AS_TRIGGER(fcinfo);
	bool		is_event_trigger = CALLED_AS_EVENT_TRIGGER(fcinfo);
	Datum		prosrcdatum;
	char	   *proc_source;
	HeapTuple	typeTup;
	Form_pg_type typeStruct;
	PLpgSQL_variable *var;
	PLpgSQL_rec *rec;
	int			i;
	ErrorContextCallback plerrcontext;
	int			parse_rc;
	Oid			rettypeid;
	int			numargs;
	int			num_in_args = 0;
	int			num_out_args = 0;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	int		   *in_arg_varnos = NULL;
	PLpgSQL_variable **out_arg_variables;
	MemoryContext func_cxt;

	/*
	 * 设置扫描器输入和错误信息。我们假定本函数
	 * 无法被递归调用，故不必保存并恢复
	 * 此处用到的静态变量。
	 */
	prosrcdatum = SysCacheGetAttrNotNull(PROCOID, procTup, Anum_pg_proc_prosrc);
	proc_source = TextDatumGetCString(prosrcdatum);
	plpgsql_scanner_init(proc_source);

	plpgsql_error_funcname = pstrdup(NameStr(procStruct->proname));

	/*
	 * 为 ereport() 设错误回溯支持
	 */
	plerrcontext.callback = plpgsql_compile_error_callback;
	plerrcontext.arg = forValidator ? proc_source : NULL;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/*
	 * 校验函数定义时多做语法检查。真正为执行而编译时
	 * 我们为了性能而跳过这一步。
	 *
	 */
	plpgsql_check_syntax = forValidator;

	/*
	 * 若尚未建好，就创建新的函数结构。这些
	 * 结构永不丢弃，故留在 TopMemoryContext。
	 */
	if (function == NULL)
	{
		function = (PLpgSQL_function *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(PLpgSQL_function));
	}
	else
	{
		/* 正在复用先前已有的结构，所以清空它 */
		memset(function, 0, sizeof(PLpgSQL_function));
	}
	plpgsql_curr_compile = function;

	/*
	 * 编译的永久产出（如解析树）都留在
	 * 每函数内存上下文里，便于回收。
	 */
	func_cxt = AllocSetContextCreate(TopMemoryContext,
									 "PL/pgSQL function",
									 ALLOCSET_DEFAULT_SIZES);
	plpgsql_compile_tmp_cxt = MemoryContextSwitchTo(func_cxt);

	function->fn_signature = format_procedure(fcinfo->flinfo->fn_oid);
	MemoryContextSetIdentifier(func_cxt, function->fn_signature);
	function->fn_oid = fcinfo->flinfo->fn_oid;
	function->fn_xmin = HeapTupleHeaderGetRawXmin(procTup->t_data);
	function->fn_tid = procTup->t_self;
	function->fn_input_collation = fcinfo->fncollation;
	function->fn_cxt = func_cxt;
	function->out_param_varno = -1; /* 设为无 OUT 参数 */
	function->resolve_option = plpgsql_variable_conflict;
	function->print_strict_params = plpgsql_print_strict_params;
	/* 只在 CREATE FUNCTION 时才提升额外警告与错误 */
	function->extra_warnings = forValidator ? plpgsql_extra_warnings : 0;
	function->extra_errors = forValidator ? plpgsql_extra_errors : 0;

	if (is_dml_trigger)
		function->fn_is_trigger = PLPGSQL_DML_TRIGGER;
	else if (is_event_trigger)
		function->fn_is_trigger = PLPGSQL_EVENT_TRIGGER;
	else
		function->fn_is_trigger = PLPGSQL_NOT_TRIGGER;

	function->fn_prokind = procStruct->prokind;

	function->nstatements = 0;
	function->requires_procedure_resowner = false;

	/*
	 * 初始化编译器，尤其是命名空间栈。最外层
	 * 命名空间含函数参数和其他特殊变量（
	 * 如 FOUND），其名与函数本身相同。
	 */
	plpgsql_ns_init();
	plpgsql_ns_push(NameStr(procStruct->proname), PLPGSQL_LABEL_BLOCK);
	plpgsql_DumpExecTree = false;
	plpgsql_start_datums();

	switch (function->fn_is_trigger)
	{
		case PLPGSQL_NOT_TRIGGER:

			/*
			 * 取过程参数的信息。这些分配不必永久
			 * 保留，故放在临时上下文里做。
			 *
			 * 还须解析任何多态的输入或输出
			 * 参数类型。校验模式下做不到，于是
			 * 就任意假定处理的是整数。
			 */
			MemoryContextSwitchTo(plpgsql_compile_tmp_cxt);

			numargs = get_func_arg_info(procTup,
										&argtypes, &argnames, &argmodes);

			plpgsql_resolve_polymorphic_argtypes(numargs, argtypes, argmodes,
												 fcinfo->flinfo->fn_expr,
												 forValidator,
												 plpgsql_error_funcname);

			in_arg_varnos = (int *) palloc(numargs * sizeof(int));
			out_arg_variables = (PLpgSQL_variable **) palloc(numargs * sizeof(PLpgSQL_variable *));

			MemoryContextSwitchTo(func_cxt);

			/*
			 * 为过程参数创建变量。
			 */
			for (i = 0; i < numargs; i++)
			{
				char		buf[32];
				Oid			argtypeid = argtypes[i];
				char		argmode = argmodes ? argmodes[i] : PROARGMODE_IN;
				PLpgSQL_type *argdtype;
				PLpgSQL_variable *argvariable;
				PLpgSQL_nsitem_type argitemtype;

				/* 为变量建 $n 名字 */
				snprintf(buf, sizeof(buf), "$%d", i + 1);

				/* 建 datatype 信息 */
				argdtype = plpgsql_build_datatype(argtypeid,
												  -1,
												  function->fn_input_collation,
												  NULL);

				/* 禁止伪类型参数 */
				/* (注意我们已经替换过多态类型) */
				/* (build_variable 虽会做，但消息错) */
				if (argdtype->ttype == PLPGSQL_TTYPE_PSEUDO)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("PL/pgSQL functions cannot accept type %s",
									format_type_be(argtypeid))));

				/*
				 * 建变量并加入 datum 列表。若参数有名，
				 * 就用该名作引用名，否则用 $n 名。
				 */
				argvariable = plpgsql_build_variable((argnames &&
													  argnames[i][0] != '\0') ?
													 argnames[i] : buf,
													 0, argdtype, false);

				if (argvariable->dtype == PLPGSQL_DTYPE_VAR)
				{
					argitemtype = PLPGSQL_NSTYPE_VAR;
				}
				else
				{
					Assert(argvariable->dtype == PLPGSQL_DTYPE_REC);
					argitemtype = PLPGSQL_NSTYPE_REC;
				}

				/* 把参数记入相应的数组 */
				if (argmode == PROARGMODE_IN ||
					argmode == PROARGMODE_INOUT ||
					argmode == PROARGMODE_VARIADIC)
					in_arg_varnos[num_in_args++] = argvariable->dno;
				if (argmode == PROARGMODE_OUT ||
					argmode == PROARGMODE_INOUT ||
					argmode == PROARGMODE_TABLE)
					out_arg_variables[num_out_args++] = argvariable;

				/* 以 $n 名加入命名空间 */
				add_parameter_name(argitemtype, argvariable->dno, buf);

				/* 若参数有名字，就建一个别名 */
				if (argnames && argnames[i][0] != '\0')
					add_parameter_name(argitemtype, argvariable->dno,
									   argnames[i]);
			}

			/*
			 * 若只有一个 OUT 参数，out_param_varno 直指
			 * 它。若多于一个，就建一行来装
			 * 全部。过程也返回一行，哪怕只有一个 OUT
			 * 参数。
			 */
			if (num_out_args > 1 ||
				(num_out_args == 1 && function->fn_prokind == PROKIND_PROCEDURE))
			{
				PLpgSQL_row *row = build_row_from_vars(out_arg_variables,
													   num_out_args);

				plpgsql_adddatum((PLpgSQL_datum *) row);
				function->out_param_varno = row->dno;
			}
			else if (num_out_args == 1)
				function->out_param_varno = out_arg_variables[0]->dno;

			/*
			 * 检查返回类型是否多态。若是，且已有
			 * 调用方 FuncExpr 节点，就用其中的实际
			 * 返回类型。（校验模式下任意假定在处理
			 * 整数。)
			 *
			 * 注意：errcode 用 FEATURE_NOT_SUPPORTED，因本应
			 * 总能成功；若失败，就是某处没把该信息
			 * 提供出来。
			 */
			rettypeid = procStruct->prorettype;
			if (IsPolymorphicType(rettypeid))
			{
				if (forValidator)
				{
					if (rettypeid == ANYARRAYOID ||
						rettypeid == ANYCOMPATIBLEARRAYOID)
						rettypeid = INT4ARRAYOID;
					else if (rettypeid == ANYRANGEOID ||
							 rettypeid == ANYCOMPATIBLERANGEOID)
						rettypeid = INT4RANGEOID;
					else if (rettypeid == ANYMULTIRANGEOID)
						rettypeid = INT4MULTIRANGEOID;
					else		/* ANYELEMENT、ANYNONARRAY或ANYCOMPATIBLE */
						rettypeid = INT4OID;
					/* XXX ANYENUM 能用什么？ */
				}
				else
				{
					rettypeid = get_fn_expr_rettype(fcinfo->flinfo);
					if (!OidIsValid(rettypeid))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("could not determine actual return type "
										"for polymorphic function \"%s\"",
										plpgsql_error_funcname)));
				}
			}

			/*
			 * 普通函数有确定的返回类型
			 */
			function->fn_rettype = rettypeid;
			function->fn_retset = procStruct->proretset;

			/*
			 * 查找函数的返回类型
			 */
			typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(rettypeid));
			if (!HeapTupleIsValid(typeTup))
				elog(ERROR, "cache lookup failed for type %u", rettypeid);
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			/* 禁止伪类型结果，VOID 或 RECORD 除外 */
			/* (注意我们已经替换过多态类型) */
			if (typeStruct->typtype == TYPTYPE_PSEUDO)
			{
				if (rettypeid == VOIDOID ||
					rettypeid == RECORDOID)
					 /* 可 */ ;
				else if (rettypeid == TRIGGEROID || rettypeid == EVENT_TRIGGEROID)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions can only be called as triggers")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("PL/pgSQL functions cannot return type %s",
									format_type_be(rettypeid))));
			}

			function->fn_retistuple = type_is_rowtype(rettypeid);
			function->fn_retisdomain = (typeStruct->typtype == TYPTYPE_DOMAIN);
			function->fn_retbyval = typeStruct->typbyval;
			function->fn_rettyplen = typeStruct->typlen;

			/*
			 * 装上 $0 引用，但仅用于多态返回类型，
			 * 返回经输出指定时则不装此引用，因输出
			 * 参数。
			 */
			if (IsPolymorphicType(procStruct->prorettype) &&
				num_out_args == 0)
			{
				(void) plpgsql_build_variable("$0", 0,
											  build_datatype(typeTup,
															 -1,
															 function->fn_input_collation,
															 NULL),
											  true);
			}

			ReleaseSysCache(typeTup);
			break;

		case PLPGSQL_DML_TRIGGER:
			/* 触发器过程的返回类型尚未可知 */
			function->fn_rettype = InvalidOid;
			function->fn_retbyval = false;
			function->fn_retistuple = true;
			function->fn_retisdomain = false;
			function->fn_retset = false;

			/* 不该有任何已声明参数 */
			if (procStruct->pronargs != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("trigger functions cannot have declared arguments"),
						 errhint("The arguments of the trigger can be accessed through TG_NARGS and TG_ARGV instead.")));

			/* 添加引用 NEW 行的记录 */
			rec = plpgsql_build_record("new", 0, NULL, RECORDOID, true);
			function->new_varno = rec->dno;

			/* 添加引用 OLD 行的记录 */
			rec = plpgsql_build_record("old", 0, NULL, RECORDOID, true);
			function->old_varno = rec->dno;

			/* 添加变量 tg_name */
			var = plpgsql_build_variable("tg_name", 0,
										 plpgsql_build_datatype(NAMEOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_NAME;

			/* 添加变量 tg_when */
			var = plpgsql_build_variable("tg_when", 0,
										 plpgsql_build_datatype(TEXTOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_WHEN;

			/* 添加变量 tg_level */
			var = plpgsql_build_variable("tg_level", 0,
										 plpgsql_build_datatype(TEXTOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_LEVEL;

			/* 添加变量 tg_op */
			var = plpgsql_build_variable("tg_op", 0,
										 plpgsql_build_datatype(TEXTOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_OP;

			/* 添加变量 tg_relid */
			var = plpgsql_build_variable("tg_relid", 0,
										 plpgsql_build_datatype(OIDOID,
																-1,
																InvalidOid,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_RELID;

			/* 添加变量 tg_relname */
			var = plpgsql_build_variable("tg_relname", 0,
										 plpgsql_build_datatype(NAMEOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_TABLE_NAME;

			/* 现优先用 tg_table_name 而非 tg_relname */
			var = plpgsql_build_variable("tg_table_name", 0,
										 plpgsql_build_datatype(NAMEOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_TABLE_NAME;

			/* 添加变量 tg_table_schema */
			var = plpgsql_build_variable("tg_table_schema", 0,
										 plpgsql_build_datatype(NAMEOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_TABLE_SCHEMA;

			/* 添加变量 tg_nargs */
			var = plpgsql_build_variable("tg_nargs", 0,
										 plpgsql_build_datatype(INT4OID,
																-1,
																InvalidOid,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_NARGS;

			/* 添加变量 tg_argv */
			var = plpgsql_build_variable("tg_argv", 0,
										 plpgsql_build_datatype(TEXTARRAYOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_ARGV;

			break;

		case PLPGSQL_EVENT_TRIGGER:
			function->fn_rettype = VOIDOID;
			function->fn_retbyval = false;
			function->fn_retistuple = true;
			function->fn_retisdomain = false;
			function->fn_retset = false;

			/* 不该有任何已声明参数 */
			if (procStruct->pronargs != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("event trigger functions cannot have declared arguments")));

			/* 添加变量 tg_event */
			var = plpgsql_build_variable("tg_event", 0,
										 plpgsql_build_datatype(TEXTOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_EVENT;

			/* 添加变量 tg_tag */
			var = plpgsql_build_variable("tg_tag", 0,
										 plpgsql_build_datatype(TEXTOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_TAG;

			break;

		default:
			elog(ERROR, "unrecognized function typecode: %d",
				 (int) function->fn_is_trigger);
			break;
	}

	/* 记住函数是否为 STABLE/IMMUTABLE */
	function->fn_readonly = (procStruct->provolatile != PROVOLATILE_VOLATILE);

	/*
	 * 建立魔术 FOUND 变量。
	 */
	var = plpgsql_build_variable("found", 0,
								 plpgsql_build_datatype(BOOLOID,
														-1,
														InvalidOid,
														NULL),
								 true);
	function->found_varno = var->dno;

	/*
	 * 现在解析函数文本
	 */
	parse_rc = plpgsql_yyparse();
	if (parse_rc != 0)
		elog(ERROR, "plpgsql parser returned %d", parse_rc);
	function->action = plpgsql_parse_result;

	plpgsql_scanner_finish();
	pfree(proc_source);

	/*
	 * 若有 OUT 参数、返回 VOID 或返回集合，就允许
	 * 控制流没有显式 RETURN 就落到末尾。最简单
	 * 的实现法是在解析时给语句列表末尾
	 * 添上一条 RETURN 语句。
	 */
	if (num_out_args > 0 || function->fn_rettype == VOIDOID ||
		function->fn_retset)
		add_dummy_return(function);

	/*
	 * 补全该函数的信息
	 */
	function->fn_nargs = procStruct->pronargs;
	for (i = 0; i < function->fn_nargs; i++)
		function->fn_argvarnos[i] = in_arg_varnos[i];

	plpgsql_finish_datums(function);

	/* 为完成的函数做调试转储 */
	if (plpgsql_DumpExecTree)
		plpgsql_dumptree(function);

	/*
	 * 把它加入散列表。
	 */
	plpgsql_HashTableInsert(function, hashkey);

	/*
	 * 弹出错误上下文栈
	 */
	error_context_stack = plerrcontext.previous;
	plpgsql_error_funcname = NULL;

	plpgsql_check_syntax = false;

	MemoryContextSwitchTo(plpgsql_compile_tmp_cxt);
	plpgsql_compile_tmp_cxt = NULL;
	return function;
}

/* ----------
 * plpgsql_compile_inline：为匿名代码块建执行树。
 *
 * 注意：这大体平行于 do_compile()，二者值得尝试
 * 合并吗？
 *
 * 注意：假定该块会被丢掉，故不必建立
 * 持久的数据结构。
 * ----------
 */
PLpgSQL_function *
plpgsql_compile_inline(char *proc_source)
{
	char	   *func_name = "inline_code_block";
	PLpgSQL_function *function;
	ErrorContextCallback plerrcontext;
	PLpgSQL_variable *var;
	int			parse_rc;
	MemoryContext func_cxt;

	/*
	 * 设置扫描器输入和错误信息。我们假定本函数
	 * 无法被递归调用，故不必保存并恢复
	 * 此处用到的静态变量。
	 */
	plpgsql_scanner_init(proc_source);

	plpgsql_error_funcname = func_name;

	/*
	 * 为 ereport() 设错误回溯支持
	 */
	plerrcontext.callback = plpgsql_compile_error_callback;
	plerrcontext.arg = proc_source;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/* 若 check_function_bodies 开启就多做语法检查 */
	plpgsql_check_syntax = check_function_bodies;

	/* 函数结构活不过当前语句 */
	function = (PLpgSQL_function *) palloc0(sizeof(PLpgSQL_function));

	plpgsql_curr_compile = function;

	/*
	 * 其余编译期存储（如解析树）都留在
	 * 自己的内存上下文里，便于回收。
	 */
	func_cxt = AllocSetContextCreate(CurrentMemoryContext,
									 "PL/pgSQL inline code context",
									 ALLOCSET_DEFAULT_SIZES);
	plpgsql_compile_tmp_cxt = MemoryContextSwitchTo(func_cxt);

	function->fn_signature = pstrdup(func_name);
	function->fn_is_trigger = PLPGSQL_NOT_TRIGGER;
	function->fn_input_collation = InvalidOid;
	function->fn_cxt = func_cxt;
	function->out_param_varno = -1; /* 设为无 OUT 参数 */
	function->resolve_option = plpgsql_variable_conflict;
	function->print_strict_params = plpgsql_print_strict_params;

	/*
	 * 内联代码不做额外校验，以免运行时刷屏
	 *
	 */
	function->extra_warnings = 0;
	function->extra_errors = 0;

	function->nstatements = 0;
	function->requires_procedure_resowner = false;

	plpgsql_ns_init();
	plpgsql_ns_push(func_name, PLPGSQL_LABEL_BLOCK);
	plpgsql_DumpExecTree = false;
	plpgsql_start_datums();

	/* 按返回 VOID 的函数来设置 */
	function->fn_rettype = VOIDOID;
	function->fn_retset = false;
	function->fn_retistuple = false;
	function->fn_retisdomain = false;
	function->fn_prokind = PROKIND_FUNCTION;
	/* 此处对 VOID 类型有一点硬编码知识 */
	function->fn_retbyval = true;
	function->fn_rettyplen = sizeof(int32);

	/*
	 * 记住函数是否 STABLE/IMMUTABLE。XXX 若在只读
	 * 事务里把它设为真会更好吗？不清楚。
	 */
	function->fn_readonly = false;

	/*
	 * 建立魔术 FOUND 变量。
	 */
	var = plpgsql_build_variable("found", 0,
								 plpgsql_build_datatype(BOOLOID,
														-1,
														InvalidOid,
														NULL),
								 true);
	function->found_varno = var->dno;

	/*
	 * 现在解析函数文本
	 */
	parse_rc = plpgsql_yyparse();
	if (parse_rc != 0)
		elog(ERROR, "plpgsql parser returned %d", parse_rc);
	function->action = plpgsql_parse_result;

	plpgsql_scanner_finish();

	/*
	 * 若返回 VOID（目前总是如此），就允许控制流
	 * 没有显式 RETURN 就落到末尾。
	 */
	if (function->fn_rettype == VOIDOID)
		add_dummy_return(function);

	/*
	 * 补全该函数的信息
	 */
	function->fn_nargs = 0;

	plpgsql_finish_datums(function);

	/*
	 * 弹出错误上下文栈
	 */
	error_context_stack = plerrcontext.previous;
	plpgsql_error_funcname = NULL;

	plpgsql_check_syntax = false;

	MemoryContextSwitchTo(plpgsql_compile_tmp_cxt);
	plpgsql_compile_tmp_cxt = NULL;
	return function;
}


/*
 * 错误上下文回调，用来补上调用栈回溯。
 * 若正在校验或执行匿名代码块，函数源文本
 * 就作为参数传入。
 */
static void
plpgsql_compile_error_callback(void *arg)
{
	if (arg)
	{
		/*
		 * 试着把语法错位置改指原先的 CREATE
		 * FUNCTION 或 DO 的文本。
		 */
		if (function_parse_error_transpose((const char *) arg))
			return;

		/*
		 * 若已报出语法错误位置就结束；否则只好
		 * 退回按行号报告。
		 */
	}

	if (plpgsql_error_funcname)
		errcontext("compilation of PL/pgSQL function \"%s\" near line %d",
				   plpgsql_error_funcname, plpgsql_latest_lineno());
}


/*
 * 把函数参数名加入该函数的命名空间
 */
static void
add_parameter_name(PLpgSQL_nsitem_type itemtype, int itemno, const char *name)
{
	/*
	 * 加名字前先查重。即便 functioncmds.c 已有
	 * 类似检查也需要做，因为那边明确
	 * 不抱怨相冲突的 IN 与 OUT 参数名。在
	 * plpgsql 里这些名字在同一命名空间，故
	 * 无法消歧
	 */
	if (plpgsql_ns_lookup(plpgsql_ns_top(), true,
						  name, NULL, NULL,
						  NULL) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("parameter name \"%s\" used more than once",
						name)));

	/* OK 加上此名 */
	plpgsql_ns_additem(itemtype, itemno, name);
}

/*
 * 给该函数体添一条假的 RETURN 语句
 */
static void
add_dummy_return(PLpgSQL_function *function)
{
	/*
	 * 若外层块有 EXCEPTION 子句，就得新做一个外层
	 * 块，因为加上的 RETURN 不该表现得像在
	 * EXCEPTION 子句里面。同样，若有标签，就用新外层
	 * 块包住，免得 EXIT 跳过 RETURN。
	 */
	if (function->action->exceptions != NULL ||
		function->action->label != NULL)
	{
		PLpgSQL_stmt_block *new;

		new = palloc0(sizeof(PLpgSQL_stmt_block));
		new->cmd_type = PLPGSQL_STMT_BLOCK;
		new->stmtid = ++function->nstatements;
		new->body = list_make1(function->action);

		function->action = new;
	}
	if (function->action->body == NIL ||
		((PLpgSQL_stmt *) llast(function->action->body))->cmd_type != PLPGSQL_STMT_RETURN)
	{
		PLpgSQL_stmt_return *new;

		new = palloc0(sizeof(PLpgSQL_stmt_return));
		new->cmd_type = PLPGSQL_STMT_RETURN;
		new->stmtid = ++function->nstatements;
		new->expr = NULL;
		new->retvarno = function->out_param_varno;

		function->action->body = lappend(function->action->body, new);
	}
}


/*
 * plpgsql_parser_setup：为动态参数装上解析器钩子
 *
 * 注意：本例程及其准备的钩子函数，逻辑上属于
 * plpgsql 解析。但它们其实在函数执行期间运行，
 * 那时我们准备求值一条尚未解析并规划过的
 * SQL 查询或表达式。
 */
void
plpgsql_parser_setup(struct ParseState *pstate, PLpgSQL_expr *expr)
{
	pstate->p_pre_columnref_hook = plpgsql_pre_column_ref;
	pstate->p_post_columnref_hook = plpgsql_post_column_ref;
	pstate->p_paramref_hook = plpgsql_param_ref;
	/* 不必使用 p_coerce_param_hook */
	pstate->p_ref_hook_state = (void *) expr;
}

/*
 * plpgsql_pre_column_ref：解析 ColumnRef 前的回调
 */
static Node *
plpgsql_pre_column_ref(ParseState *pstate, ColumnRef *cref)
{
	PLpgSQL_expr *expr = (PLpgSQL_expr *) pstate->p_ref_hook_state;

	if (expr->func->resolve_option == PLPGSQL_RESOLVE_VARIABLE)
		return resolve_column_ref(pstate, expr, cref, false);
	else
		return NULL;
}

/*
 * plpgsql_post_column_ref：解析 ColumnRef 后的回调
 */
static Node *
plpgsql_post_column_ref(ParseState *pstate, ColumnRef *cref, Node *var)
{
	PLpgSQL_expr *expr = (PLpgSQL_expr *) pstate->p_ref_hook_state;
	Node	   *myvar;

	if (expr->func->resolve_option == PLPGSQL_RESOLVE_VARIABLE)
		return NULL;			/* 我们已经发现没有匹配。 */

	if (expr->func->resolve_option == PLPGSQL_RESOLVE_COLUMN && var != NULL)
		return NULL;			/* 有表列时优先用那个 */

	/*
	 * 若找到记录或行变量，但对不上字段名，
	 * 而 ColumnRef 也没有核心解析结果，就报错。
	 * 这种情况下该引用势必失败，抱怨
	 * 记录或行变量，往往比核心解析器的报错
	 * 更切题。可惜我们这里拿不到
	 * transformColumnRef 内部的 crerr 状态，因此若与
	 * 表名冲突，这仍可能不是最有帮助的
	 * 错误消息。
	 */
	myvar = resolve_column_ref(pstate, expr, cref, (var == NULL));

	if (myvar != NULL && var != NULL)
	{
		/*
		 * 本可留给核心解析器抛这个错，但我们
		 * 能加上比核心更有用的详情消息。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_AMBIGUOUS_COLUMN),
				 errmsg("column reference \"%s\" is ambiguous",
						NameListToString(cref->fields)),
				 errdetail("It could refer to either a PL/pgSQL variable or a table column."),
				 parser_errposition(pstate, cref->location)));
	}

	return myvar;
}

/*
 * plpgsql_param_ref：ParamRefs（$n 符号）的解析回调
 */
static Node *
plpgsql_param_ref(ParseState *pstate, ParamRef *pref)
{
	PLpgSQL_expr *expr = (PLpgSQL_expr *) pstate->p_ref_hook_state;
	char		pname[32];
	PLpgSQL_nsitem *nse;

	snprintf(pname, sizeof(pname), "$%d", pref->number);

	nse = plpgsql_ns_lookup(expr->ns, false,
							pname, NULL, NULL,
							NULL);

	if (nse == NULL)
		return NULL;			/* plpgsql 不识此名 */

	return make_datum_param(expr, nse->itemno, pref->location);
}

/*
 * resolve_column_ref：试把 ColumnRef 解成 plpgsql 变量
 *
 * 返回翻译后的节点，若找不到名字则返回 NULL
 *
 * error_if_no_field 决定：能对上记录或行的名字、
 * 但对不上字段名时，是报错还是安静返回 NULL。
 */
static Node *
resolve_column_ref(ParseState *pstate, PLpgSQL_expr *expr,
				   ColumnRef *cref, bool error_if_no_field)
{
	PLpgSQL_execstate *estate;
	PLpgSQL_nsitem *nse;
	const char *name1;
	const char *name2 = NULL;
	const char *name3 = NULL;
	const char *colname = NULL;
	int			nnames;
	int			nnames_scalar = 0;
	int			nnames_wholerow = 0;
	int			nnames_field = 0;

	/*
	 * 我们用函数当前的 estate 来解析参数数据类型。
	 * 这其实很不靠谱，因为类型一变，计划并没有
	 * 更新的安排……
	 */
	estate = expr->func->cur_estate;

	/*----------
	 * 允许的语法有：
	 *
	 * A 标量变量引用，或整行记录引用。
	 * A.B 限定的标量或整行引用，或字段引用。
	 * A.B.C 限定的记录字段引用。
	 * A.* 整行记录引用。
	 * A.B.* 限定的整行记录引用。
	 *----------
	 */
	switch (list_length(cref->fields))
	{
		case 1:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);

				name1 = strVal(field1);
				nnames_scalar = 1;
				nnames_wholerow = 1;
				break;
			}
		case 2:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);
				Node	   *field2 = (Node *) lsecond(cref->fields);

				name1 = strVal(field1);

				/* 整行引用？ */
				if (IsA(field2, A_Star))
				{
					/* 把 name2 设上，以防匹配到标量变量 */
					name2 = "*";
					nnames_wholerow = 1;
					break;
				}

				name2 = strVal(field2);
				colname = name2;
				nnames_scalar = 2;
				nnames_wholerow = 2;
				nnames_field = 1;
				break;
			}
		case 3:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);
				Node	   *field2 = (Node *) lsecond(cref->fields);
				Node	   *field3 = (Node *) lthird(cref->fields);

				name1 = strVal(field1);
				name2 = strVal(field2);

				/* 整行引用？ */
				if (IsA(field3, A_Star))
				{
					/* 把 name3 设上，以防匹配到标量变量 */
					name3 = "*";
					nnames_wholerow = 2;
					break;
				}

				name3 = strVal(field3);
				colname = name3;
				nnames_field = 2;
				break;
			}
		default:
			/* 名字太多，忽略 */
			return NULL;
	}

	nse = plpgsql_ns_lookup(expr->ns, false,
							name1, name2, name3,
							&nnames);

	if (nse == NULL)
		return NULL;			/* plpgsql 不识此名 */

	switch (nse->itemtype)
	{
		case PLPGSQL_NSTYPE_VAR:
			if (nnames == nnames_scalar)
				return make_datum_param(expr, nse->itemno, cref->location);
			break;
		case PLPGSQL_NSTYPE_REC:
			if (nnames == nnames_wholerow)
				return make_datum_param(expr, nse->itemno, cref->location);
			if (nnames == nnames_field)
			{
				/* 列名可能是该记录的一个字段 */
				PLpgSQL_rec *rec = (PLpgSQL_rec *) estate->datums[nse->itemno];
				int			i;

				/* 查找引用该字段的 datum */
				i = rec->firstfield;
				while (i >= 0)
				{
					PLpgSQL_recfield *fld = (PLpgSQL_recfield *) estate->datums[i];

					Assert(fld->dtype == PLPGSQL_DTYPE_RECFIELD &&
						   fld->recparentno == nse->itemno);
					if (strcmp(fld->fieldname, colname) == 0)
					{
						return make_datum_param(expr, i, cref->location);
					}
					i = fld->nextfield;
				}

				/*
				 * 不该到此，因该记录每个可能的限定
				 * 引用，解析时都应已建好 RECFIELD datum。
				 * 但若真走到了，就按字段未找到处理：
				 * 报错，或返回 NULL。
				 */
				if (error_if_no_field)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("record \"%s\" has no field \"%s\"",
									(nnames_field == 1) ? name1 : name2,
									colname),
							 parser_errposition(pstate, cref->location)));
			}
			break;
		default:
			elog(ERROR, "unrecognized plpgsql itemtype: %d", nse->itemtype);
	}

	/* 名字格式与 plpgsql 变量类型不符 */
	return NULL;
}

/*
 * 列引用助手：为 plpgsql 的 datum 建 Param，
 * 并确保该 datum 列在表达式的 paramnos 里。
 */
static Node *
make_datum_param(PLpgSQL_expr *expr, int dno, int location)
{
	PLpgSQL_execstate *estate;
	PLpgSQL_datum *datum;
	Param	   *param;
	MemoryContext oldcontext;

	/* 见 resolve_column_ref 的注释 */
	estate = expr->func->cur_estate;
	Assert(dno >= 0 && dno < estate->ndatums);
	datum = estate->datums[dno];

	/*
	 * Bitmapset 必须分配在函数的永久内存上下文里
	 */
	oldcontext = MemoryContextSwitchTo(expr->func->fn_cxt);
	expr->paramnos = bms_add_member(expr->paramnos, dno);
	MemoryContextSwitchTo(oldcontext);

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = dno + 1;
	plpgsql_exec_get_datum_type_info(estate,
									 datum,
									 &param->paramtype,
									 &param->paramtypmod,
									 &param->paramcollid);
	param->location = location;

	return (Node *) param;
}


/* ----------
 * plpgsql_parse_word：扫描器调用它来后解析
 *	任何非保留关键字的单词。
 *
 * word1 是小写或去引号的标识符，须 palloc 在
 * 函数的长期内存上下文中。
 *
 * yytxt 是原始记号文本；要靠它检查是否加了引号，
 * 好让之后对非保留关键字的检查正确。
 *
 * 仅当 lookup 为真，且 plpgsql_IdentifierLookup 上下文
 * 允许时，才试着把记号认作变量。
 *
 * 若认作变量，就填好 *wdatum 并返回真；
 * 若认不出，就填 *word 并返回假（
 * 注意二者其实指向同一联合体的成员，
 * 只因记法而分开传入）。
 * ----------
 */
bool
plpgsql_parse_word(char *word1, const char *yytxt, bool lookup,
				   PLwdatum *wdatum, PLword *word)
{
	PLpgSQL_nsitem *ns;

	/*
	 * 在 DECLARE 段里不该查找变量。在 SQL
	 * 表达式里也不必做——编译表达式时
	 * 自然会查。
	 */
	if (lookup && plpgsql_IdentifierLookup == IDENTIFIER_LOOKUP_NORMAL)
	{
		/*
		 * 在当前命名空间栈中查找
		 */
		ns = plpgsql_ns_lookup(plpgsql_ns_top(), false,
							   word1, NULL, NULL,
							   NULL);

		if (ns != NULL)
		{
			switch (ns->itemtype)
			{
				case PLPGSQL_NSTYPE_VAR:
				case PLPGSQL_NSTYPE_REC:
					wdatum->datum = plpgsql_Datums[ns->itemno];
					wdatum->ident = word1;
					wdatum->quoted = (yytxt[0] == '"');
					wdatum->idents = NIL;
					return true;

				default:
					/* plpgsql_ns_lookup 绝不该返回别的东西 */
					elog(ERROR, "unrecognized plpgsql itemtype: %d",
						 ns->itemtype);
			}
		}
	}

	/*
	 * 尚未找到：这词对我们至今没有特殊含义
	 *
	 */
	word->ident = word1;
	word->quoted = (yytxt[0] == '"');
	return false;
}


/* ----------
 * plpgsql_parse_dblword：同样查找两个词
 *	以点号分开。
 * ----------
 */
bool
plpgsql_parse_dblword(char *word1, char *word2,
					  PLwdatum *wdatum, PLcword *cword)
{
	PLpgSQL_nsitem *ns;
	List	   *idents;
	int			nnames;

	idents = list_make2(makeString(word1),
						makeString(word2));

	/*
	 * 在 DECLARE 段里应什么都不做。在 SQL 表达式里，
	 * 其实只需确保需要时建好 RECFIELD datum。
	 * 本函数处理的各种情况里，返回带双词
	 * idents 串的 T_DATUM 就是对的。
	 */
	if (plpgsql_IdentifierLookup != IDENTIFIER_LOOKUP_DECLARE)
	{
		/*
		 * 在当前命名空间栈中查找
		 */
		ns = plpgsql_ns_lookup(plpgsql_ns_top(), false,
							   word1, word2, NULL,
							   &nnames);
		if (ns != NULL)
		{
			switch (ns->itemtype)
			{
				case PLPGSQL_NSTYPE_VAR:
					/* 带块限定的标量变量引用。 */
					wdatum->datum = plpgsql_Datums[ns->itemno];
					wdatum->ident = NULL;
					wdatum->quoted = false; /* 未用 */
					wdatum->idents = idents;
					return true;

				case PLPGSQL_NSTYPE_REC:
					if (nnames == 1)
					{
						/*
						 * 首词是记录名，故次词可能是
						 * 该记录的字段。无论是不是，都建
						 * 一个 RECFIELD datum，有错则
						 * 稍后发现。
						 */
						PLpgSQL_rec *rec;
						PLpgSQL_recfield *new;

						rec = (PLpgSQL_rec *) (plpgsql_Datums[ns->itemno]);
						new = plpgsql_build_recfield(rec, word2);

						wdatum->datum = (PLpgSQL_datum *) new;
					}
					else
					{
						/* 带块限定的记录变量引用。 */
						wdatum->datum = plpgsql_Datums[ns->itemno];
					}
					wdatum->ident = NULL;
					wdatum->quoted = false; /* 未用 */
					wdatum->idents = idents;
					return true;

				default:
					break;
			}
		}
	}

	/* 未找到 */
	cword->idents = idents;
	return false;
}


/* ----------
 * plpgsql_parse_tripword：同样查找三个词
 *	由点号分隔。
 * ----------
 */
bool
plpgsql_parse_tripword(char *word1, char *word2, char *word3,
					   PLwdatum *wdatum, PLcword *cword)
{
	PLpgSQL_nsitem *ns;
	List	   *idents;
	int			nnames;

	/*
	 * 在 DECLARE 段里应什么都不做。在 SQL 表达式里须
	 * 确保需要时建好 RECFIELD datum，还须
	 * 小心有多少个名字被报成属于
	 * 该 T_DATUM：第三词可能是子字段引用，这里
	 * 并不关心。
	 */
	if (plpgsql_IdentifierLookup != IDENTIFIER_LOOKUP_DECLARE)
	{
		/*
		 * 在当前命名空间栈中查找。必须找到记录
		 * 引用，否则忽略
		 */
		ns = plpgsql_ns_lookup(plpgsql_ns_top(), false,
							   word1, word2, word3,
							   &nnames);
		if (ns != NULL)
		{
			switch (ns->itemtype)
			{
				case PLPGSQL_NSTYPE_REC:
					{
						PLpgSQL_rec *rec;
						PLpgSQL_recfield *new;

						rec = (PLpgSQL_rec *) (plpgsql_Datums[ns->itemno]);
						if (nnames == 1)
						{
							/*
							 * 首词是记录名，所以次词可能
							 * 是此记录字段（第三词为子字段）
							 * 无论是不是都建一个 RECFIELD
							 * datum，有错则
							 * 稍后发现。
							 */
							new = plpgsql_build_recfield(rec, word2);
							idents = list_make2(makeString(word1),
												makeString(word2));
						}
						else
						{
							/* 带块限定的记录变量引用。 */
							new = plpgsql_build_recfield(rec, word3);
							idents = list_make3(makeString(word1),
												makeString(word2),
												makeString(word3));
						}
						wdatum->datum = (PLpgSQL_datum *) new;
						wdatum->ident = NULL;
						wdatum->quoted = false; /* 未用 */
						wdatum->idents = idents;
						return true;
					}

				default:
					break;
			}
		}
	}

	/* 未找到 */
	idents = list_make3(makeString(word1),
						makeString(word2),
						makeString(word3));
	cword->idents = idents;
	return false;
}


/* ----------
 * plpgsql_parse_wordtype：扫到 word%TYPE，word 可为
 *	变量名或基类型。
 *
 * 返回数据类型结构；word 无匹配则返回 NULL。
 * ----------
 */
PLpgSQL_type *
plpgsql_parse_wordtype(char *ident)
{
	PLpgSQL_type *dtype;
	PLpgSQL_nsitem *nse;
	TypeName   *typeName;
	HeapTuple	typeTup;

	/*
	 * 在当前命名空间栈中查找
	 */
	nse = plpgsql_ns_lookup(plpgsql_ns_top(), false,
							ident, NULL, NULL,
							NULL);

	if (nse != NULL)
	{
		switch (nse->itemtype)
		{
			case PLPGSQL_NSTYPE_VAR:
				return ((PLpgSQL_var *) (plpgsql_Datums[nse->itemno]))->datatype;

				/* XXX 也许允许 REC 与 ROW？ */

			default:
				return NULL;
		}
	}

	/*
	 * 词不在命名空间栈里。试按此名找数据类型，
	 * 但忽略壳类型和复杂类型。
	 */
	typeName = makeTypeName(ident);
	typeTup = LookupTypeName(NULL, typeName, NULL, false);
	if (typeTup)
	{
		Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

		if (!typeStruct->typisdefined ||
			typeStruct->typrelid != InvalidOid)
		{
			ReleaseSysCache(typeTup);
			return NULL;
		}

		dtype = build_datatype(typeTup, -1,
							   plpgsql_curr_compile->fn_input_collation,
							   typeName);

		ReleaseSysCache(typeTup);
		return dtype;
	}

	/*
	 * 尚未找到：这词对我们至今没有特殊含义
	 *
	 */
	return NULL;
}


/* ----------
 * plpgsql_parse_cwordtype：复合词%TYPE 的同样查找
 * ----------
 */
PLpgSQL_type *
plpgsql_parse_cwordtype(List *idents)
{
	PLpgSQL_type *dtype = NULL;
	PLpgSQL_nsitem *nse;
	const char *fldname;
	Oid			classOid;
	HeapTuple	classtup = NULL;
	HeapTuple	attrtup = NULL;
	HeapTuple	typetup = NULL;
	Form_pg_class classStruct;
	Form_pg_attribute attrStruct;
	MemoryContext oldCxt;

	/* 避免函数长期上下文中的内存泄漏 */
	oldCxt = MemoryContextSwitchTo(plpgsql_compile_tmp_cxt);

	if (list_length(idents) == 2)
	{
		/*
		 * 在当前命名空间栈中查找。不必检查
		 * 匹配上的名字个数，因为我们只考虑标量
		 * 变量。
		 */
		nse = plpgsql_ns_lookup(plpgsql_ns_top(), false,
								strVal(linitial(idents)),
								strVal(lsecond(idents)),
								NULL,
								NULL);

		if (nse != NULL && nse->itemtype == PLPGSQL_NSTYPE_VAR)
		{
			dtype = ((PLpgSQL_var *) (plpgsql_Datums[nse->itemno]))->datatype;
			goto done;
		}

		/*
		 * 首词也可能是表名
		 */
		classOid = RelnameGetRelid(strVal(linitial(idents)));
		if (!OidIsValid(classOid))
			goto done;
		fldname = strVal(lsecond(idents));
	}
	else if (list_length(idents) == 3)
	{
		RangeVar   *relvar;

		relvar = makeRangeVar(strVal(linitial(idents)),
							  strVal(lsecond(idents)),
							  -1);
		/* 不能锁关系，因为我们可能没有权限。 */
		classOid = RangeVarGetRelid(relvar, NoLock, true);
		if (!OidIsValid(classOid))
			goto done;
		fldname = strVal(lthird(idents));
	}
	else
		goto done;

	classtup = SearchSysCache1(RELOID, ObjectIdGetDatum(classOid));
	if (!HeapTupleIsValid(classtup))
		goto done;
	classStruct = (Form_pg_class) GETSTRUCT(classtup);

	/*
	 * 它必须是关系、序列、视图、物化视图、复合
	 * 类型或外部表
	 */
	if (classStruct->relkind != RELKIND_RELATION &&
		classStruct->relkind != RELKIND_SEQUENCE &&
		classStruct->relkind != RELKIND_VIEW &&
		classStruct->relkind != RELKIND_MATVIEW &&
		classStruct->relkind != RELKIND_COMPOSITE_TYPE &&
		classStruct->relkind != RELKIND_FOREIGN_TABLE &&
		classStruct->relkind != RELKIND_PARTITIONED_TABLE)
		goto done;

	/*
	 * 取出具名表字段及其类型
	 */
	attrtup = SearchSysCacheAttName(classOid, fldname);
	if (!HeapTupleIsValid(attrtup))
		goto done;
	attrStruct = (Form_pg_attribute) GETSTRUCT(attrtup);

	typetup = SearchSysCache1(TYPEOID,
							  ObjectIdGetDatum(attrStruct->atttypid));
	if (!HeapTupleIsValid(typetup))
		elog(ERROR, "cache lookup failed for type %u", attrStruct->atttypid);

	/*
	 * 找到了就在调用方上下文里建编译器类型结构
	 * 并返回。注意我们把该类型当成按 OID 找到的；
	 * 失效时不会再按类型名重新查找。
	 */
	MemoryContextSwitchTo(oldCxt);
	dtype = build_datatype(typetup,
						   attrStruct->atttypmod,
						   attrStruct->attcollation,
						   NULL);
	MemoryContextSwitchTo(plpgsql_compile_tmp_cxt);

done:
	if (HeapTupleIsValid(classtup))
		ReleaseSysCache(classtup);
	if (HeapTupleIsValid(attrtup))
		ReleaseSysCache(attrtup);
	if (HeapTupleIsValid(typetup))
		ReleaseSysCache(typetup);

	MemoryContextSwitchTo(oldCxt);
	return dtype;
}

/* ----------
 * plpgsql_parse_wordrowtype：扫到 word%ROWTYPE。
 *	所以 word 必须是表名。
 * ----------
 */
PLpgSQL_type *
plpgsql_parse_wordrowtype(char *ident)
{
	Oid			classOid;
	Oid			typOid;

	/*
	 * 查关系。注意关系的行类型与关系同名，
	 * 所以这同样可以当成类型查找来做；我们走
	 * 关系查找只是因为这里抛出的错误历来
	 * 说的是关系而不是类型。但我们仍会做
	 * 一个 TypeName，以防以后必须重新查类型。
	 */
	classOid = RelnameGetRelid(ident);
	if (!OidIsValid(classOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s\" does not exist", ident)));

	/* 有些 relkind 无类型OIDs */
	typOid = get_rel_type_id(classOid);
	if (!OidIsValid(typOid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" does not have a composite type",
						ident)));

	/* 构建并返回行类型结构。 */
	return plpgsql_build_datatype(typOid, -1, InvalidOid,
								  makeTypeName(ident));
}

/* ----------
 * plpgsql_parse_cwordrowtype：扫描器见到复合词%ROWTYPE。
 *	故该词须是命名空间限定的表名。
 * ----------
 */
PLpgSQL_type *
plpgsql_parse_cwordrowtype(List *idents)
{
	Oid			classOid;
	Oid			typOid;
	RangeVar   *relvar;
	MemoryContext oldCxt;

	/*
	 * 同上，此为关系查找；若不为错误措辞向后兼容，
	 * 本来也可以做成类型查找。
	 */
	if (list_length(idents) != 2)
		return NULL;

	/* 避免长期函数上下文里内存泄漏 */
	oldCxt = MemoryContextSwitchTo(plpgsql_compile_tmp_cxt);

	/* 查关系名。不能锁它，因为我们可能没有权限。 */
	relvar = makeRangeVar(strVal(linitial(idents)),
						  strVal(lsecond(idents)),
						  -1);
	classOid = RangeVarGetRelid(relvar, NoLock, false);

	/* 有些 relkind 无类型OIDs */
	typOid = get_rel_type_id(classOid);
	if (!OidIsValid(typOid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" does not have a composite type",
						strVal(lsecond(idents)))));

	MemoryContextSwitchTo(oldCxt);

	/* 构建并返回行类型结构。 */
	return plpgsql_build_datatype(typOid, -1, InvalidOid,
								  makeTypeNameFromNameList(idents));
}

/*
 * plpgsql_build_variable：按给定类型建 datum 数组项
 *
 *
 * 返回的结构可能是 PLpgSQL_var 或 PLpgSQL_rec，
 * 视给定类型而定，经 palloc 分配。
 * 该结构会自动加入当前 datum 数组，
 * 并可选择加入当前命名空间。
 */
PLpgSQL_variable *
plpgsql_build_variable(const char *refname, int lineno, PLpgSQL_type *dtype,
					   bool add2namespace)
{
	PLpgSQL_variable *result;

	switch (dtype->ttype)
	{
		case PLPGSQL_TTYPE_SCALAR:
			{
				/* 普通标量数据类型 */
				PLpgSQL_var *var;

				var = palloc0(sizeof(PLpgSQL_var));
				var->dtype = PLPGSQL_DTYPE_VAR;
				var->refname = pstrdup(refname);
				var->lineno = lineno;
				var->datatype = dtype;
				/* 其余字段留 0，调用者可能改掉。 */

				/* 预设为 NULL */
				var->value = 0;
				var->isnull = true;
				var->freeval = false;

				plpgsql_adddatum((PLpgSQL_datum *) var);
				if (add2namespace)
					plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR,
									   var->dno,
									   refname);
				result = (PLpgSQL_variable *) var;
				break;
			}
		case PLPGSQL_TTYPE_REC:
			{
				/* 复合类型：建一个记录变量 */
				PLpgSQL_rec *rec;

				rec = plpgsql_build_record(refname, lineno,
										   dtype, dtype->typoid,
										   add2namespace);
				result = (PLpgSQL_variable *) rec;
				break;
			}
		case PLPGSQL_TTYPE_PSEUDO:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("variable \"%s\" has pseudo-type %s",
							refname, format_type_be(dtype->typoid))));
			result = NULL;		/* 让编译器安静 */
			break;
		default:
			elog(ERROR, "unrecognized ttype: %d", dtype->ttype);
			result = NULL;		/* 让编译器安静 */
			break;
	}

	return result;
}

/*
 * 建一个空的具名记录变量，并可选择加入命名空间。
 */
PLpgSQL_rec *
plpgsql_build_record(const char *refname, int lineno,
					 PLpgSQL_type *dtype, Oid rectypeid,
					 bool add2namespace)
{
	PLpgSQL_rec *rec;

	rec = palloc0(sizeof(PLpgSQL_rec));
	rec->dtype = PLPGSQL_DTYPE_REC;
	rec->refname = pstrdup(refname);
	rec->lineno = lineno;
	/* 其余字段留 0，调用者可能改掉。 */
	rec->datatype = dtype;
	rec->rectypeid = rectypeid;
	rec->firstfield = -1;
	rec->erh = NULL;
	plpgsql_adddatum((PLpgSQL_datum *) rec);
	if (add2namespace)
		plpgsql_ns_additem(PLPGSQL_NSTYPE_REC, rec->dno, rec->refname);

	return rec;
}

/*
 * 按成员变量建行变量的数据结构。
 * 附带 rowtupdesc，因为稍后要物化行结果。
 */
static PLpgSQL_row *
build_row_from_vars(PLpgSQL_variable **vars, int numvars)
{
	PLpgSQL_row *row;
	int			i;

	row = palloc0(sizeof(PLpgSQL_row));
	row->dtype = PLPGSQL_DTYPE_ROW;
	row->refname = "(unnamed row)";
	row->lineno = -1;
	row->rowtupdesc = CreateTemplateTupleDesc(numvars);
	row->nfields = numvars;
	row->fieldnames = palloc(numvars * sizeof(char *));
	row->varnos = palloc(numvars * sizeof(int));

	for (i = 0; i < numvars; i++)
	{
		PLpgSQL_variable *var = vars[i];
		Oid			typoid;
		int32		typmod;
		Oid			typcoll;

		/* 行的成员变量绝不该是 const */
		Assert(!var->isconst);

		switch (var->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
			case PLPGSQL_DTYPE_PROMISE:
				typoid = ((PLpgSQL_var *) var)->datatype->typoid;
				typmod = ((PLpgSQL_var *) var)->datatype->atttypmod;
				typcoll = ((PLpgSQL_var *) var)->datatype->collation;
				break;

			case PLPGSQL_DTYPE_REC:
				/* 按理不必再校验已……的 rectypeid */
				typoid = ((PLpgSQL_rec *) var)->rectypeid;
				typmod = -1;	/* 即便用到也不知道 typmod */
				typcoll = InvalidOid;	/* 复合类型没有排序规则。 */
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", var->dtype);
				typoid = InvalidOid;	/* 让编译器安静 */
				typmod = 0;
				typcoll = InvalidOid;
				break;
		}

		row->fieldnames[i] = var->refname;
		row->varnos[i] = var->dno;

		TupleDescInitEntry(row->rowtupdesc, i + 1,
						   var->refname,
						   typoid, typmod,
						   0);
		TupleDescInitEntryCollation(row->rowtupdesc, i + 1, typcoll);
	}

	return row;
}

/*
 * 为指定记录变量的具名字段建一个 RECFIELD datum
 *
 * 若已有这样的 datum，直接返回它；不需要重复的。
 */
PLpgSQL_recfield *
plpgsql_build_recfield(PLpgSQL_rec *rec, const char *fldname)
{
	PLpgSQL_recfield *recfield;
	int			i;

	/* 查找已有的、引用该字段的 datum */
	i = rec->firstfield;
	while (i >= 0)
	{
		PLpgSQL_recfield *fld = (PLpgSQL_recfield *) plpgsql_Datums[i];

		Assert(fld->dtype == PLPGSQL_DTYPE_RECFIELD &&
			   fld->recparentno == rec->dno);
		if (strcmp(fld->fieldname, fldname) == 0)
			return fld;
		i = fld->nextfield;
	}

	/* 没有就新建一个 */
	recfield = palloc0(sizeof(PLpgSQL_recfield));
	recfield->dtype = PLPGSQL_DTYPE_RECFIELD;
	recfield->fieldname = pstrdup(fldname);
	recfield->recparentno = rec->dno;
	recfield->rectupledescid = INVALID_TUPLEDESC_IDENTIFIER;

	plpgsql_adddatum((PLpgSQL_datum *) recfield);

	/* 现在可以把它链进父级的链上。 */
	recfield->nextfield = rec->firstfield;
	rec->firstfield = recfield->dno;

	return recfield;
}

/*
 * plpgsql_build_datatype
 *	建 PLpgSQL_type：给定 OID、typmod、排序规则
 *	及类型的解析名。
 *
 * 若排序规则不是 InvalidOid，就覆盖该类型的默认
 * 排序规则。但类型若不能排序，则忽略排序规则。
 *
 * origtypname 是用户所写类型名的解析形式。
 * 它可为 NULL：若类型不可能是复合类型，或一开始
 * 就按 OID 认出（例如函数参数类型）。
 */
PLpgSQL_type *
plpgsql_build_datatype(Oid typeOid, int32 typmod,
					   Oid collation, TypeName *origtypname)
{
	HeapTuple	typeTup;
	PLpgSQL_type *typ;

	typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(typeTup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);

	typ = build_datatype(typeTup, typmod, collation, origtypname);

	ReleaseSysCache(typeTup);

	return typ;
}

/*
 * 工具子程序：按一条 pg_type 记录和附加细节
 * 建出 PLpgSQL_type（详见 plpgsql_build_datatype 注释）。
 */
static PLpgSQL_type *
build_datatype(HeapTuple typeTup, int32 typmod,
			   Oid collation, TypeName *origtypname)
{
	Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
	PLpgSQL_type *typ;

	if (!typeStruct->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" is only a shell",
						NameStr(typeStruct->typname))));

	typ = (PLpgSQL_type *) palloc(sizeof(PLpgSQL_type));

	typ->typname = pstrdup(NameStr(typeStruct->typname));
	typ->typoid = typeStruct->oid;
	switch (typeStruct->typtype)
	{
		case TYPTYPE_BASE:
		case TYPTYPE_ENUM:
		case TYPTYPE_RANGE:
		case TYPTYPE_MULTIRANGE:
			typ->ttype = PLPGSQL_TTYPE_SCALAR;
			break;
		case TYPTYPE_COMPOSITE:
			typ->ttype = PLPGSQL_TTYPE_REC;
			break;
		case TYPTYPE_DOMAIN:
			if (type_is_rowtype(typeStruct->typbasetype))
				typ->ttype = PLPGSQL_TTYPE_REC;
			else
				typ->ttype = PLPGSQL_TTYPE_SCALAR;
			break;
		case TYPTYPE_PSEUDO:
			if (typ->typoid == RECORDOID)
				typ->ttype = PLPGSQL_TTYPE_REC;
			else
				typ->ttype = PLPGSQL_TTYPE_PSEUDO;
			break;
		default:
			elog(ERROR, "unrecognized typtype: %d",
				 (int) typeStruct->typtype);
			break;
	}
	typ->typlen = typeStruct->typlen;
	typ->typbyval = typeStruct->typbyval;
	typ->typtype = typeStruct->typtype;
	typ->collation = typeStruct->typcollation;
	if (OidIsValid(collation) && OidIsValid(typ->collation))
		typ->collation = collation;
	/* 判断类型是否真数组，或其域 */
	/* 注意：这只用来决定是否应用 expand_array */
	if (typeStruct->typtype == TYPTYPE_BASE)
	{
		/*
		 * 此测试应含 get_element_type() 的检查。另外
		 * 禁止不能 TOAST 的数组（即 oidvector、int2vector）。
		 */
		typ->typisarray = (IsTrueArrayType(typeStruct) &&
						   typeStruct->typstorage != TYPSTORAGE_PLAIN);
	}
	else if (typeStruct->typtype == TYPTYPE_DOMAIN)
	{
		/* 若不是 varlena，就可短路、不再查基类型 */
		typ->typisarray = (typeStruct->typlen == -1 &&
						   typeStruct->typstorage != TYPSTORAGE_PLAIN &&
						   OidIsValid(get_base_element_type(typeStruct->typbasetype)));
	}
	else
		typ->typisarray = false;
	typ->atttypmod = typmod;

	/*
	 * 若是具名复合类型（或其上的域），就找 typcache
	 * 项并记下当前 tupdesc 标识，以便察觉变化（
	 * 含被删除）。目前不支持当场替换
	 * 非复合类型，否则也许也想对它们这么做。
	 */
	if (typ->ttype == PLPGSQL_TTYPE_REC && typ->typoid != RECORDOID)
	{
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(typ->typoid,
									 TYPECACHE_TUPDESC |
									 TYPECACHE_DOMAIN_BASE_INFO);
		if (typentry->typtype == TYPTYPE_DOMAIN)
			typentry = lookup_type_cache(typentry->domainBaseType,
										 TYPECACHE_TUPDESC);
		if (typentry->tupDesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(typ->typoid))));

		typ->origtypname = origtypname;
		typ->tcache = typentry;
		typ->tupdesc_id = typentry->tupDesc_identifier;
	}
	else
	{
		typ->origtypname = NULL;
		typ->tcache = NULL;
		typ->tupdesc_id = 0;
	}

	return typ;
}

/*
 *	plpgsql_recognize_err_condition
 *	检查条件名并把它译成 SQLSTATE。
 *
 * 注意：有些情况下同一条件名在表里有多条
 * 记录。我们任意返回第一条匹配。
 */
int
plpgsql_recognize_err_condition(const char *condname, bool allow_sqlstate)
{
	int			i;

	if (allow_sqlstate)
	{
		if (strlen(condname) == 5 &&
			strspn(condname, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") == 5)
			return MAKE_SQLSTATE(condname[0],
								 condname[1],
								 condname[2],
								 condname[3],
								 condname[4]);
	}

	for (i = 0; exception_label_map[i].label != NULL; i++)
	{
		if (strcmp(condname, exception_label_map[i].label) == 0)
			return exception_label_map[i].sqlerrstate;
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("unrecognized exception condition \"%s\"",
					condname)));
	return 0;					/* 让编译器安静 */
}

/*
 * plpgsql_parse_err_condition
 *	为一个异常条件名生成若干 PLpgSQL_condition entry
 *
 * 必须能返回一个列表，因为错误码名字表里
 * 有些名字是重复的。
 */
PLpgSQL_condition *
plpgsql_parse_err_condition(char *condname)
{
	int			i;
	PLpgSQL_condition *new;
	PLpgSQL_condition *prev;

	/*
	 * XXX 日后想在此查找用户定义的异常名。
	 *
	 */

	/*
	 * OTHERS 表示为代码 0（那会对应到 00000，但我们
	 * 并不需要把它表示成异常条件）。
	 */
	if (strcmp(condname, "others") == 0)
	{
		new = palloc(sizeof(PLpgSQL_condition));
		new->sqlerrstate = 0;
		new->condname = condname;
		new->next = NULL;
		return new;
	}

	prev = NULL;
	for (i = 0; exception_label_map[i].label != NULL; i++)
	{
		if (strcmp(condname, exception_label_map[i].label) == 0)
		{
			new = palloc(sizeof(PLpgSQL_condition));
			new->sqlerrstate = exception_label_map[i].sqlerrstate;
			new->condname = condname;
			new->next = prev;
			prev = new;
		}
	}

	if (!prev)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("unrecognized exception condition \"%s\"",
						condname)));

	return prev;
}

/* ----------
 * plpgsql_start_datums：编译开始时初始化 datum 列表。
 * ----------
 */
static void
plpgsql_start_datums(void)
{
	datums_alloc = 128;
	plpgsql_nDatums = 0;
	/* 这是短命的，不必在函数上下文里分配 */
	plpgsql_Datums = MemoryContextAlloc(plpgsql_compile_tmp_cxt,
										sizeof(PLpgSQL_datum *) * datums_alloc);
	/* datums_last 记录 plpgsql_add_initdatums() 已见过的 */
	datums_last = 0;
}

/* ----------
 * plpgsql_adddatum：把变量、记录或行
 *	加入编译器 datum 列表。
 * ----------
 */
void
plpgsql_adddatum(PLpgSQL_datum *newdatum)
{
	if (plpgsql_nDatums == datums_alloc)
	{
		datums_alloc *= 2;
		plpgsql_Datums = repalloc(plpgsql_Datums, sizeof(PLpgSQL_datum *) * datums_alloc);
	}

	newdatum->dno = plpgsql_nDatums;
	plpgsql_Datums[plpgsql_nDatums++] = newdatum;
}

/* ----------
 * plpgsql_finish_datums：把完成的 datum 信息拷进函数结构
 * ----------
 */
static void
plpgsql_finish_datums(PLpgSQL_function *function)
{
	Size		copiable_size = 0;
	int			i;

	function->ndatums = plpgsql_nDatums;
	function->datums = palloc(sizeof(PLpgSQL_datum *) * plpgsql_nDatums);
	for (i = 0; i < plpgsql_nDatums; i++)
	{
		function->datums[i] = plpgsql_Datums[i];

		/* 哪些可复制必须与 copy_plpgsql_datums 一致 */
		switch (function->datums[i]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
			case PLPGSQL_DTYPE_PROMISE:
				copiable_size += MAXALIGN(sizeof(PLpgSQL_var));
				break;
			case PLPGSQL_DTYPE_REC:
				copiable_size += MAXALIGN(sizeof(PLpgSQL_rec));
				break;
			default:
				break;
		}
	}
	function->copiable_size = copiable_size;
}


/* ----------
 * plpgsql_add_initdatums：做成自上次调用本函数
 *	以来全部新建的可初始化 datum
 *	的编号数组。
 *
 * 若 varnos 为 NULL，我们就忘掉上次调用后的新 datum。
 *
 *
 * 用于 DECLARE 段周围，列出进入块时必须初始化的
 * 那些 datum。注意 datum 也能在 DECLARE 以外创建，
 * 例如 FOR 循环，但那时就得由专用代码负责
 * 初始化它们。
 * ----------
 */
int
plpgsql_add_initdatums(int **varnos)
{
	int			i;
	int			n = 0;

	/*
	 * 此处认的 dtype 集必须和 exec_stmt_block()
	 * 入口处关心再初始化的一致。
	 */
	for (i = datums_last; i < plpgsql_nDatums; i++)
	{
		switch (plpgsql_Datums[i]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
			case PLPGSQL_DTYPE_REC:
				n++;
				break;

			default:
				break;
		}
	}

	if (varnos != NULL)
	{
		if (n > 0)
		{
			*varnos = (int *) palloc(sizeof(int) * n);

			n = 0;
			for (i = datums_last; i < plpgsql_nDatums; i++)
			{
				switch (plpgsql_Datums[i]->dtype)
				{
					case PLPGSQL_DTYPE_VAR:
					case PLPGSQL_DTYPE_REC:
						(*varnos)[n++] = plpgsql_Datums[i]->dno;

					default:
						break;
				}
			}
		}
		else
			*varnos = NULL;
	}

	datums_last = plpgsql_nDatums;
	return n;
}


/*
 * 计算给定函数调用的散列键
 *
 * 散列键写进调用方提供的 *hashkey 存储里。
 */
static void
compute_function_hashkey(FunctionCallInfo fcinfo,
						 Form_pg_proc procStruct,
						 PLpgSQL_func_hashkey *hashkey,
						 bool forValidator)
{
	/* 确保该结构里未用的字节都是零 */
	MemSet(hashkey, 0, sizeof(PLpgSQL_func_hashkey));

	/* 取函数 OID */
	hashkey->funcOid = fcinfo->flinfo->fn_oid;

	/* 取调用环境 */
	hashkey->isTrigger = CALLED_AS_TRIGGER(fcinfo);
	hashkey->isEventTrigger = CALLED_AS_EVENT_TRIGGER(fcinfo);

	/*
	 * 若是 DML 触发器，就把触发器 OID 算进散列，好让
	 * 每次触发器用法各得一个散列项，以便例如
	 * 关系行类型或过渡表名不同。校验模式下并不
	 * 知道打算使用哪个关系或过渡表名，因此
	 * 把 trigOid 留为 0；此时建的散列项绝不会
	 * 用于实际调用。
	 *
	 * 目前不必用同样方式区分不同的事件触发器用法，
	 * 因为那些特殊参数变量在该情况下
	 * 类型并不变。
	 */
	if (hashkey->isTrigger && !forValidator)
	{
		TriggerData *trigdata = (TriggerData *) fcinfo->context;

		hashkey->trigOid = trigdata->tg_trigger->tgoid;
	}

	/* 取输入 collation 若已知 */
	hashkey->inputCollation = fcinfo->fncollation;

	if (procStruct->pronargs > 0)
	{
		/* 取参数类型 */
		memcpy(hashkey->argtypes, procStruct->proargtypes.values,
			   procStruct->pronargs * sizeof(Oid));

		/* 解析任何多态的参数类型 */
		plpgsql_resolve_polymorphic_argtypes(procStruct->pronargs,
											 hashkey->argtypes,
											 NULL,
											 fcinfo->flinfo->fn_expr,
											 forValidator,
											 NameStr(procStruct->proname));
	}
}

/*
 * 这与标准函数 resolve_polymorphic_argtypes() 相同，
 * 区别是：
 * 1. 解析不了类型时，我们就直接报错。
 * 2. 我们把 RECORD 型输入参数（不是输出参数）
 * 当成多态，若能确定实际输入类型就换上。
 * 这样我们就能为每个具名复合类型
 * 各建独立的函数缓存项，只要它被传给该
 * 参数。
 * 3. 校验模式下没有输入可看，故假定多态参数
 * 是整数、整数数组或整数范围。
 */
static void
plpgsql_resolve_polymorphic_argtypes(int numargs,
									 Oid *argtypes, char *argmodes,
									 Node *call_expr, bool forValidator,
									 const char *proname)
{
	int			i;

	if (!forValidator)
	{
		int			inargno;

		/* 普通情况，交给标准例程 */
		if (!resolve_polymorphic_argtypes(numargs, argtypes, argmodes,
										  call_expr))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not determine actual argument "
							"type for polymorphic function \"%s\"",
							proname)));
		/* 同样把 RECORD 输入（但非输出）当成多态 */
		inargno = 0;
		for (i = 0; i < numargs; i++)
		{
			char		argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

			if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				continue;
			if (argtypes[i] == RECORDOID || argtypes[i] == RECORDARRAYOID)
			{
				Oid			resolvedtype = get_call_expr_argtype(call_expr,
																 inargno);

				if (OidIsValid(resolvedtype))
					argtypes[i] = resolvedtype;
			}
			inargno++;
		}
	}
	else
	{
		/* 特殊的校验情形（RECORD 不必做任何事） */
		for (i = 0; i < numargs; i++)
		{
			switch (argtypes[i])
			{
				case ANYELEMENTOID:
				case ANYNONARRAYOID:
				case ANYENUMOID:	/* XXX 存疑 */
				case ANYCOMPATIBLEOID:
				case ANYCOMPATIBLENONARRAYOID:
					argtypes[i] = INT4OID;
					break;
				case ANYARRAYOID:
				case ANYCOMPATIBLEARRAYOID:
					argtypes[i] = INT4ARRAYOID;
					break;
				case ANYRANGEOID:
				case ANYCOMPATIBLERANGEOID:
					argtypes[i] = INT4RANGEOID;
					break;
				case ANYMULTIRANGEOID:
					argtypes[i] = INT4MULTIRANGEOID;
					break;
				default:
					break;
			}
		}
	}
}

/*
 * delete_function：尽量清掉过期的函数缓存
 *
 * 不能释放 PLpgSQL_function 结构本身，因为
 * 可能有 fn_extra 指针指向它。附属存储可以释放，
 * 但仅当没有进行中的活跃求值时。否则
 * 那些存储就只会泄漏。因为只有在嵌套的
 * 递归调用里发现 pg_proc 更新时才会这样，
 * 所以泄漏看来可以接受。
 *
 * 注意：多个 fn_extra 指向同一缓存时可能被调多次。
 * 故须小心，同一件事不要做
 * 两次
 */
static void
delete_function(PLpgSQL_function *func)
{
	/* 从散列表移除函数（可能已经做过） */
	plpgsql_HashTableDelete(func);

	/* 若安全且尚未做过，就释放该函数的存储 */
	if (func->use_count == 0)
		plpgsql_free_function_memory(func);
}

/* 导出，以便能从 _PG_init() 调用 */
void
plpgsql_HashTableInit(void)
{
	HASHCTL		ctl;

	/* 不允许重复初始化 */
	Assert(plpgsql_HashTable == NULL);

	ctl.keysize = sizeof(PLpgSQL_func_hashkey);
	ctl.entrysize = sizeof(plpgsql_HashEnt);
	plpgsql_HashTable = hash_create("PLpgSQL function hash",
									FUNCS_PER_USER,
									&ctl,
									HASH_ELEM | HASH_BLOBS);
}

static PLpgSQL_function *
plpgsql_HashTableLookup(PLpgSQL_func_hashkey *func_key)
{
	plpgsql_HashEnt *hentry;

	hentry = (plpgsql_HashEnt *) hash_search(plpgsql_HashTable,
											 func_key,
											 HASH_FIND,
											 NULL);
	if (hentry)
		return hentry->function;
	else
		return NULL;
}

static void
plpgsql_HashTableInsert(PLpgSQL_function *function,
						PLpgSQL_func_hashkey *func_key)
{
	plpgsql_HashEnt *hentry;
	bool		found;

	hentry = (plpgsql_HashEnt *) hash_search(plpgsql_HashTable,
											 func_key,
											 HASH_ENTER,
											 &found);
	if (found)
		elog(WARNING, "trying to insert a function that already exists");

	hentry->function = function;
	/* 准备从函数指回散列表键的反向链接 */
	function->fn_hashkey = &hentry->key;
}

static void
plpgsql_HashTableDelete(PLpgSQL_function *function)
{
	plpgsql_HashEnt *hentry;

	/* 不在表中就不做 */
	if (function->fn_hashkey == NULL)
		return;

	hentry = (plpgsql_HashEnt *) hash_search(plpgsql_HashTable,
											 function->fn_hashkey,
											 HASH_REMOVE,
											 NULL);
	if (hentry == NULL)
		elog(WARNING, "trying to delete function that does not exist");

	/* 去掉反向链接，它不再指向已分配的存储 */
	function->fn_hashkey = NULL;
}
