/*-------------------------------------------------------------------------
 *
 * pl_exec.c：PL/pgSQL 过程语言的
 *			  执行器
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/pl/plpgsql/src/pl_exec.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "access/detoast.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "access/tupconvert.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/execExpr.h"
#include "executor/spi.h"
#include "executor/tstoreReceiver.h"
#include "funcapi.h"
#include "mb/stringinfo_mb.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_type.h"
#include "parser/scansup.h"
#include "plpgsql.h"
#include "storage/proc.h"
#include "tcop/cmdtag.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/*
 * 同一事务内，全部 plpgsql 函数执行共用同一个
 * 执行器 EState，用以求值简单表达式。每次函数调用
 * 都在此 EState 内自建 eval_econtext，即 ExprContext，供
 * 每次求值的工作区。函数正常退出时释放 eval_econtext，
 * 事务结束时释放 EState（出错时，我们假定
 * 中止机制把它全部清掉）。此外，函数内任一异常
 * 块都必须自有 eval_econtext，与所在函数的分开，
 * 以便能在子事务退出时妥善清理 ExprContext 回调。
 * 我们维护一个栈，跟踪各个 econtext，以便在子事务
 * 退出时正确清理。
 *
 * 这种安排维护起来有点烦，但麻烦是值得的，
 * 这样每次进入函数都不必重新准备简单表达式。
 * 我们假定要优化的是事务内对同一函数的多次
 * 重复。
 *
 * 但把简单表达式准备摊到多次 DO 块执行并无价值（
 * 即内联代码块），因为根本不可能有多次。
 * 若 DO 块用共享 EState，表达式状态树其实泄漏到事务
 * 结束。用户一再提交 DO 块，泄漏可能累加。
 * 因此每个 DO 块自有简单表达式 EState，离开
 * plpgsql_inline_handler() 时清理。DO 块照用
 * simple_econtext_stack，以使子事务中止清理
 * 仍能做对。
 *
 * 不过（若 DO 块执行 COMMIT、ROLLBACK，则 exec_stmt_commit
 * 与 exec_stmt_rollback 把它从该 DO 的简单表达式 EState
 * 解绑，并新建此后使用的共享 EState。原来的
 * EState 将在回到 plpgsql_inline_handler 时被清理。这
 * 有点难看，但不值得做得更好，因为这类情形
 * 无法造成状态树无限堆积。）
 */
typedef struct SimpleEcontextStackEntry
{
	ExprContext *stack_econtext;	/* 栈上的 econtext */
	SubTransactionId xact_subxid;	/* 当前子事务 ID */
	struct SimpleEcontextStackEntry *next;	/* 栈中更上一项 */
} SimpleEcontextStackEntry;

static EState *shared_simple_eval_estate = NULL;
static SimpleEcontextStackEntry *simple_econtext_stack = NULL;

/*
 * 除共享的简单求值 EState 外，还有共享资源
 * 所有者，持有本事务中已求值的任何简单表达式
 * 之 CachedPlans 的引用计数。这让我们避免在其
 * 被反复使用时不停地抓取并释放计划引用计数。
 * DO 块自用 resowner，做法与上文
 * shared_simple_eval_estate 完全一样。）
 */
static ResourceOwner shared_simple_eval_resowner = NULL;

/*
 * plpgsql 函数内的内存管理大体靠三种内存上下文
 * 如下：
 *
 * 1. 函数调用寿命的数据，例如变量值，保存在
 * 主内存上下文，亦即 SPI_connect() 所建的 SPI Proc 上下文。
 * 在本模块跑代码时，它通常就是 CurrentMemoryContext（
 * 这并不好，因粗心编码很容易造成函数寿命的
 * 内存泄漏，但我们暂时忍受。）
 *
 * 2. 有些语句执行例程需要语句寿命的工作区。
 * 内存上下文由 get_stmt_mcontext() 按需创建，且必须
 * 在该请求例程结束时重置。错误恢复将自动清理。
 * 嵌套语句若也要语句寿命工作区，便形成此类
 * 内存上下文的栈，见 push_stmt_mcontext()。
 *
 * 3. 求值用 eval_econtext 的每元组内存上下文，并作
 * 通用工作区，供短命分配之用。这类分配通常不显式
 * 释放，而留待内存上下文重置来清理，通常由
 * exec_eval_cleanup() 来做。
 *
 * 这些宏就是用来做短命分配的：
 */
#define get_eval_mcontext(estate) \
	((estate)->eval_econtext->ecxt_per_tuple_memory)
#define eval_mcontext_alloc(estate, sz) \
	MemoryContextAlloc(get_eval_mcontext(estate), sz)
#define eval_mcontext_alloc0(estate, sz) \
	MemoryContextAllocZero(get_eval_mcontext(estate), sz)

/*
 * 我们用两张连接期散列表来缓存转换信息。
 *
 * cast_expr_hash 的项（plpgsql_CastExprHashEntry）存已编译
 * 转换表达式树。它们在整个连接期内存活，并供全部
 * PL/pgSQL 函数与 DO 块共享。将来也许值得在
 * pg_cast 变更后作废它们，但此刻
 * 不费心。
 *
 * 另有散列表 shared_cast_hash（项类型为
 * plpgsql_CastHashEntry），存放这些表达式的求值
 * 状态树，管理方式与简单表达式相同（亦即
 * 我们假定转换表达式总是简单的）。
 *
 * 与简单表达式一样，DO 块不用 shared_cast_hash 表，
 * 但必须自有求值状态树。这不理想，但我们不想
 * 在一个 DO 块里对付多个 simple_eval_estates。
 */
typedef struct					/* 转换信息的查找键 */
{
	/* 注意：假定此结构不含任何填充字节 */
	Oid			srctype;		/* 转换的源类型 */
	Oid			dsttype;		/* 转换的目标类型 */
	int32		srctypmod;		/* 转换的源 typmod */
	int32		dsttypmod;		/* 转换的目标 typmod */
} plpgsql_CastHashKey;

typedef struct					/* cast_expr_hash 表项 */
{
	plpgsql_CastHashKey key;	/* 散列键须排第一 */
	Expr	   *cast_expr;		/* 转换表达式，空操作则为 NULL */
	CachedExpression *cast_cexpr;	/* 支撑上项的缓存表达式 */
} plpgsql_CastExprHashEntry;

typedef struct					/* cast_hash 表项 */
{
	plpgsql_CastHashKey key;	/* 散列键须排第一 */
	plpgsql_CastExprHashEntry *cast_centry; /* 链到匹配的 expr 项 */
	/* 仅当 cast_lxid 匹配当前 LXID 时 ExprState 才有效 */
	ExprState  *cast_exprstate; /* 表达式求值树 */
	bool		cast_in_use;	/* 执行求值树期间为真 */
	LocalTransactionId cast_lxid;
} plpgsql_CastHashEntry;

static HTAB *cast_expr_hash = NULL;
static HTAB *shared_cast_hash = NULL;

/*
 * LOOP_RC_PROCESSING 封装循环语句的共同逻辑，用来
 * 处理循环体语句给出的 return、exit、continue 结果码。
 * 打算这样使用：
 *
 *		int rc=PLPGSQL_RC_OK;
 *		for (...)
 *		{
 *			...
 *			rc = exec_stmts(estate, stmt->body);
 *			LOOP_RC_PROCESSING(stmt->label,break);
 *			...
 *		}
 *		return(rc)
 *
 * 若该循环的执行应当终止，LOOP_RC_PROCESSING 先把 rc
 * 更新为当前语句应返回的值，再执行 exit_action（
 * 通常为 break 或 goto）。若执行应当继续，
 * LOOP_RC_PROCESSING 只把 rc 重置为 PLPGSQL_RC_OK。
 *
 * estate 与 rc 是该宏的隐式参数。
 * 要检查 estate->exitlabel，并可能更新。
 */
#define LOOP_RC_PROCESSING(looplabel, exit_action) \
	if (rc == PLPGSQL_RC_RETURN) \
	{ \
		/* RETURN，故向外传播 RC_RETURN */ \
		exit_action; \
	} \
	else if (rc == PLPGSQL_RC_EXIT) \
	{ \
		if (estate->exitlabel == NULL) \
		{ \
			/* 无标号 EXIT 终止本循环 */ \
			rc = PLPGSQL_RC_OK; \
			exit_action; \
		} \
		else if ((looplabel) != NULL && \
				 strcmp(looplabel, estate->exitlabel) == 0) \
		{ \
			/* 带标号 EXIT 匹配本循环则终止循环 */ \
			estate->exitlabel = NULL; \
			rc = PLPGSQL_RC_OK; \
			exit_action; \
		} \
		else \
		{ \
			/* 不匹配的带标号 EXIT，传出 RC_EXIT */ \
			exit_action; \
		} \
	} \
	else if (rc == PLPGSQL_RC_CONTINUE) \
	{ \
		if (estate->exitlabel == NULL) \
		{ \
			/* 无标号 CONTINUE 匹配，故循环内继续 */ \
			rc = PLPGSQL_RC_OK; \
		} \
		else if ((looplabel) != NULL && \
				 strcmp(looplabel, estate->exitlabel) == 0) \
		{ \
			/* 带标号 CONTINUE 匹配，故在循环内继续 */ \
			estate->exitlabel = NULL; \
			rc = PLPGSQL_RC_OK; \
		} \
		else \
		{ \
			/* 不匹配的带标号 CONTINUE，传出 RC_CONTINUE */ \
			exit_action; \
		} \
	} \
	else \
		Assert(rc == PLPGSQL_RC_OK)

/************************************************************
 * 局部函数的前向声明
 ************************************************************/
static void coerce_function_result_tuple(PLpgSQL_execstate *estate,
										 TupleDesc tupdesc);
static void plpgsql_exec_error_callback(void *arg);
static void copy_plpgsql_datums(PLpgSQL_execstate *estate,
								PLpgSQL_function *func);
static void plpgsql_fulfill_promise(PLpgSQL_execstate *estate,
									PLpgSQL_var *var);
static MemoryContext get_stmt_mcontext(PLpgSQL_execstate *estate);
static void push_stmt_mcontext(PLpgSQL_execstate *estate);
static void pop_stmt_mcontext(PLpgSQL_execstate *estate);

static int	exec_toplevel_block(PLpgSQL_execstate *estate,
								PLpgSQL_stmt_block *block);
static int	exec_stmt_block(PLpgSQL_execstate *estate,
							PLpgSQL_stmt_block *block);
static int	exec_stmts(PLpgSQL_execstate *estate,
					   List *stmts);
static int	exec_stmt_assign(PLpgSQL_execstate *estate,
							 PLpgSQL_stmt_assign *stmt);
static int	exec_stmt_perform(PLpgSQL_execstate *estate,
							  PLpgSQL_stmt_perform *stmt);
static int	exec_stmt_call(PLpgSQL_execstate *estate,
						   PLpgSQL_stmt_call *stmt);
static int	exec_stmt_getdiag(PLpgSQL_execstate *estate,
							  PLpgSQL_stmt_getdiag *stmt);
static int	exec_stmt_if(PLpgSQL_execstate *estate,
						 PLpgSQL_stmt_if *stmt);
static int	exec_stmt_case(PLpgSQL_execstate *estate,
						   PLpgSQL_stmt_case *stmt);
static int	exec_stmt_loop(PLpgSQL_execstate *estate,
						   PLpgSQL_stmt_loop *stmt);
static int	exec_stmt_while(PLpgSQL_execstate *estate,
							PLpgSQL_stmt_while *stmt);
static int	exec_stmt_fori(PLpgSQL_execstate *estate,
						   PLpgSQL_stmt_fori *stmt);
static int	exec_stmt_fors(PLpgSQL_execstate *estate,
						   PLpgSQL_stmt_fors *stmt);
static int	exec_stmt_forc(PLpgSQL_execstate *estate,
						   PLpgSQL_stmt_forc *stmt);
static int	exec_stmt_foreach_a(PLpgSQL_execstate *estate,
								PLpgSQL_stmt_foreach_a *stmt);
static int	exec_stmt_open(PLpgSQL_execstate *estate,
						   PLpgSQL_stmt_open *stmt);
static int	exec_stmt_fetch(PLpgSQL_execstate *estate,
							PLpgSQL_stmt_fetch *stmt);
static int	exec_stmt_close(PLpgSQL_execstate *estate,
							PLpgSQL_stmt_close *stmt);
static int	exec_stmt_exit(PLpgSQL_execstate *estate,
						   PLpgSQL_stmt_exit *stmt);
static int	exec_stmt_return(PLpgSQL_execstate *estate,
							 PLpgSQL_stmt_return *stmt);
static int	exec_stmt_return_next(PLpgSQL_execstate *estate,
								  PLpgSQL_stmt_return_next *stmt);
static int	exec_stmt_return_query(PLpgSQL_execstate *estate,
								   PLpgSQL_stmt_return_query *stmt);
static int	exec_stmt_raise(PLpgSQL_execstate *estate,
							PLpgSQL_stmt_raise *stmt);
static int	exec_stmt_assert(PLpgSQL_execstate *estate,
							 PLpgSQL_stmt_assert *stmt);
static int	exec_stmt_execsql(PLpgSQL_execstate *estate,
							  PLpgSQL_stmt_execsql *stmt);
static int	exec_stmt_dynexecute(PLpgSQL_execstate *estate,
								 PLpgSQL_stmt_dynexecute *stmt);
static int	exec_stmt_dynfors(PLpgSQL_execstate *estate,
							  PLpgSQL_stmt_dynfors *stmt);
static int	exec_stmt_commit(PLpgSQL_execstate *estate,
							 PLpgSQL_stmt_commit *stmt);
static int	exec_stmt_rollback(PLpgSQL_execstate *estate,
							   PLpgSQL_stmt_rollback *stmt);

static void plpgsql_estate_setup(PLpgSQL_execstate *estate,
								 PLpgSQL_function *func,
								 ReturnSetInfo *rsi,
								 EState *simple_eval_estate,
								 ResourceOwner simple_eval_resowner);
static void exec_eval_cleanup(PLpgSQL_execstate *estate);

static void exec_prepare_plan(PLpgSQL_execstate *estate,
							  PLpgSQL_expr *expr, int cursorOptions);
static void exec_simple_check_plan(PLpgSQL_execstate *estate, PLpgSQL_expr *expr);
static bool exec_is_simple_query(PLpgSQL_expr *expr);
static void exec_save_simple_expr(PLpgSQL_expr *expr, CachedPlan *cplan);
static void exec_check_rw_parameter(PLpgSQL_expr *expr);
static void exec_check_assignable(PLpgSQL_execstate *estate, int dno);
static bool exec_eval_simple_expr(PLpgSQL_execstate *estate,
								  PLpgSQL_expr *expr,
								  Datum *result,
								  bool *isNull,
								  Oid *rettype,
								  int32 *rettypmod);

static void exec_assign_expr(PLpgSQL_execstate *estate,
							 PLpgSQL_datum *target,
							 PLpgSQL_expr *expr);
static void exec_assign_c_string(PLpgSQL_execstate *estate,
								 PLpgSQL_datum *target,
								 const char *str);
static void exec_assign_value(PLpgSQL_execstate *estate,
							  PLpgSQL_datum *target,
							  Datum value, bool isNull,
							  Oid valtype, int32 valtypmod);
static void exec_eval_datum(PLpgSQL_execstate *estate,
							PLpgSQL_datum *datum,
							Oid *typeid,
							int32 *typetypmod,
							Datum *value,
							bool *isnull);
static int	exec_eval_integer(PLpgSQL_execstate *estate,
							  PLpgSQL_expr *expr,
							  bool *isNull);
static bool exec_eval_boolean(PLpgSQL_execstate *estate,
							  PLpgSQL_expr *expr,
							  bool *isNull);
static Datum exec_eval_expr(PLpgSQL_execstate *estate,
							PLpgSQL_expr *expr,
							bool *isNull,
							Oid *rettype,
							int32 *rettypmod);
static int	exec_run_select(PLpgSQL_execstate *estate,
							PLpgSQL_expr *expr, long maxtuples, Portal *portalP);
static int	exec_for_query(PLpgSQL_execstate *estate, PLpgSQL_stmt_forq *stmt,
						   Portal portal, bool prefetch_ok);
static ParamListInfo setup_param_list(PLpgSQL_execstate *estate,
									  PLpgSQL_expr *expr);
static ParamExternData *plpgsql_param_fetch(ParamListInfo params,
											int paramid, bool speculative,
											ParamExternData *prm);
static void plpgsql_param_compile(ParamListInfo params, Param *param,
								  ExprState *state,
								  Datum *resv, bool *resnull);
static void plpgsql_param_eval_var(ExprState *state, ExprEvalStep *op,
								   ExprContext *econtext);
static void plpgsql_param_eval_var_ro(ExprState *state, ExprEvalStep *op,
									  ExprContext *econtext);
static void plpgsql_param_eval_recfield(ExprState *state, ExprEvalStep *op,
										ExprContext *econtext);
static void plpgsql_param_eval_generic(ExprState *state, ExprEvalStep *op,
									   ExprContext *econtext);
static void plpgsql_param_eval_generic_ro(ExprState *state, ExprEvalStep *op,
										  ExprContext *econtext);
static void exec_move_row(PLpgSQL_execstate *estate,
						  PLpgSQL_variable *target,
						  HeapTuple tup, TupleDesc tupdesc);
static void revalidate_rectypeid(PLpgSQL_rec *rec);
static ExpandedRecordHeader *make_expanded_record_for_rec(PLpgSQL_execstate *estate,
														  PLpgSQL_rec *rec,
														  TupleDesc srctupdesc,
														  ExpandedRecordHeader *srcerh);
static void exec_move_row_from_fields(PLpgSQL_execstate *estate,
									  PLpgSQL_variable *target,
									  ExpandedRecordHeader *newerh,
									  Datum *values, bool *nulls,
									  TupleDesc tupdesc);
static bool compatible_tupdescs(TupleDesc src_tupdesc, TupleDesc dst_tupdesc);
static HeapTuple make_tuple_from_row(PLpgSQL_execstate *estate,
									 PLpgSQL_row *row,
									 TupleDesc tupdesc);
static TupleDesc deconstruct_composite_datum(Datum value,
											 HeapTupleData *tmptup);
static void exec_move_row_from_datum(PLpgSQL_execstate *estate,
									 PLpgSQL_variable *target,
									 Datum value);
static void instantiate_empty_record_variable(PLpgSQL_execstate *estate,
											  PLpgSQL_rec *rec);
static char *convert_value_to_string(PLpgSQL_execstate *estate,
									 Datum value, Oid valtype);
static inline Datum exec_cast_value(PLpgSQL_execstate *estate,
									Datum value, bool *isnull,
									Oid valtype, int32 valtypmod,
									Oid reqtype, int32 reqtypmod);
static Datum do_cast_value(PLpgSQL_execstate *estate,
						   Datum value, bool *isnull,
						   Oid valtype, int32 valtypmod,
						   Oid reqtype, int32 reqtypmod);
static plpgsql_CastHashEntry *get_cast_hashentry(PLpgSQL_execstate *estate,
												 Oid srctype, int32 srctypmod,
												 Oid dsttype, int32 dsttypmod);
static void exec_init_tuple_store(PLpgSQL_execstate *estate);
static void exec_set_found(PLpgSQL_execstate *estate, bool state);
static void plpgsql_create_econtext(PLpgSQL_execstate *estate);
static void plpgsql_destroy_econtext(PLpgSQL_execstate *estate);
static void assign_simple_var(PLpgSQL_execstate *estate, PLpgSQL_var *var,
							  Datum newvalue, bool isnull, bool freeable);
static void assign_text_var(PLpgSQL_execstate *estate, PLpgSQL_var *var,
							const char *str);
static void assign_record_var(PLpgSQL_execstate *estate, PLpgSQL_rec *rec,
							  ExpandedRecordHeader *erh);
static ParamListInfo exec_eval_using_params(PLpgSQL_execstate *estate,
											List *params);
static Portal exec_dynquery_with_params(PLpgSQL_execstate *estate,
										PLpgSQL_expr *dynquery, List *params,
										const char *portalname, int cursorOptions);
static char *format_expr_params(PLpgSQL_execstate *estate,
								const PLpgSQL_expr *expr);
static char *format_preparedparamsdata(PLpgSQL_execstate *estate,
									   ParamListInfo paramLI);
static PLpgSQL_variable *make_callstmt_target(PLpgSQL_execstate *estate,
											  PLpgSQL_expr *expr);


/* ----------
 * plpgsql_exec_function	供调用处理器来执行
 *				该函数。
 *
 * 这也用来执行内联代码块（DO 块）。本代码
 * 唯一知道的差别是：对 DO 块，我们要
 * 用私有 simple_eval_estate 与私有 simple_eval_resowner，
 * 它们由调用者创建并传入。普通函数则传
 * NULL，意即使用 shared_simple_eval_estate 与
 * shared_simple_eval_resowner。（若用私有 simple_eval_estate，
 * 还必须再用私有转换散列表，此事交给
 * plpgsql_estate_setup。）
 * procedure_resowner 能在本函数与本过程的
 * 执行期内一直存活。只当非原子执行且有
 * CALL、DO 语句时才需要，否则可为 NULL。我们用它
 * 来持有 CALL 与 DO 这两类语句
 * 计划上的引用计数。
 * ----------
 */
Datum
plpgsql_exec_function(PLpgSQL_function *func, FunctionCallInfo fcinfo,
					  EState *simple_eval_estate,
					  ResourceOwner simple_eval_resowner,
					  ResourceOwner procedure_resowner,
					  bool atomic)
{
	PLpgSQL_execstate estate;
	ErrorContextCallback plerrcontext;
	int			i;
	int			rc;

	/*
	 * 设置执行状态
	 */
	plpgsql_estate_setup(&estate, func, (ReturnSetInfo *) fcinfo->resultinfo,
						 simple_eval_estate, simple_eval_resowner);
	estate.procedure_resowner = procedure_resowner;
	estate.atomic = atomic;

	/*
	 * 为 ereport() 设错误回溯支持
	 */
	plerrcontext.callback = plpgsql_exec_error_callback;
	plerrcontext.arg = &estate;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/*
	 * 给全部 datum 做局部执行副本
	 */
	estate.err_text = gettext_noop("during initialization of execution state");
	copy_plpgsql_datums(&estate, func);

	/*
	 * 把实际的调用实参值存入相应的变量
	 */
	estate.err_text = gettext_noop("while storing call arguments into local variables");
	for (i = 0; i < func->fn_nargs; i++)
	{
		int			n = func->fn_argvarnos[i];

		switch (estate.datums[n]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) estate.datums[n];

					assign_simple_var(&estate, var,
									  fcinfo->args[i].value,
									  fcinfo->args[i].isnull,
									  false);

					/*
					 * 强制任何数组型参数以扩展形式存进
					 * 局部变量，以期提高使用它的
					 * 效率。（这实在是权宜：为何只限数组？
					 * 哪些情形可能划算，还需多想。另见
					 * exec_assign_value 中只对 typisarray 的
					 * 启发规则。）
					 *
					 * 特例：若传入 R/W 扩展指针，就假定可以
					 * 征用该对象，而不必拷贝它。若传入的是
					 * R/O 扩展指针，暂留作该变量的值（
					 * 若变量被修改，我们再把它强制成
					 * R/W，但那也许
					 * 永远都不发生。）
					 */
					if (!var->isnull && var->datatype->typisarray)
					{
						if (VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(var->value)))
						{
							/* 接管 R/W 对象所有权 */
							assign_simple_var(&estate, var,
											  TransferExpandedObject(var->value,
																	 estate.datum_context),
											  false,
											  true);
						}
						else if (VARATT_IS_EXTERNAL_EXPANDED_RO(DatumGetPointer(var->value)))
						{
							/* R/O 指针，赋值前保持原样 */
						}
						else
						{
							/* 扁平数组则强制成扩展形式 */
							assign_simple_var(&estate, var,
											  expand_array(var->value,
														   estate.datum_context,
														   NULL),
											  false,
											  true);
						}
					}
				}
				break;

			case PLPGSQL_DTYPE_REC:
				{
					PLpgSQL_rec *rec = (PLpgSQL_rec *) estate.datums[n];

					if (!fcinfo->args[i].isnull)
					{
						/* 从复合 datum 赋给行值 */
						exec_move_row_from_datum(&estate,
												 (PLpgSQL_variable *) rec,
												 fcinfo->args[i].value);
					}
					else
					{
						/* 实参若 NULL 就把变量置 NULL */
						exec_move_row(&estate, (PLpgSQL_variable *) rec,
									  NULL, NULL);
					}
					/* exec_move_row() 之后清理 */
					exec_eval_cleanup(&estate);
				}
				break;

			default:
				/* 此外任何东西都不该是参数变量 */
				elog(ERROR, "unrecognized dtype: %d", func->datums[i]->dtype);
		}
	}

	estate.err_text = gettext_noop("during function entry");

	/*
	 * 把魔术变量 FOUND 设为假
	 */
	exec_set_found(&estate, false);

	/*
	 * 让插装插件看一眼这个函数
	 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->func_beg)
		((*plpgsql_plugin_ptr)->func_beg) (&estate, func);

	/*
	 * 现在调用顶层语句块
	 */
	estate.err_text = NULL;
	rc = exec_toplevel_block(&estate, func->action);
	if (rc != PLPGSQL_RC_RETURN)
	{
		estate.err_text = NULL;
		ereport(ERROR,
				(errcode(ERRCODE_S_R_E_FUNCTION_EXECUTED_NO_RETURN_STATEMENT),
				 errmsg("control reached end of function without RETURN")));
	}

	/*
	 * 得到返回值，接着处理它
	 */
	estate.err_text = gettext_noop("while casting return value to function's return type");

	fcinfo->isnull = estate.retisnull;

	if (estate.retisset)
	{
		ReturnSetInfo *rsi = estate.rsi;

		/* 查调用者能否处理集合结果 */
		if (!rsi || !IsA(rsi, ReturnSetInfo))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));

		if (!(rsi->allowedModes & SFRM_Materialize))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialize mode required, but it is not allowed in this context")));

		rsi->returnMode = SFRM_Materialize;

		/* 若产生了任何元组，就把结果送回 */
		if (estate.tuple_store)
		{
			MemoryContext oldcxt;

			rsi->setResult = estate.tuple_store;
			oldcxt = MemoryContextSwitchTo(estate.tuple_store_cxt);
			rsi->setDesc = CreateTupleDescCopy(estate.tuple_store_desc);
			MemoryContextSwitchTo(oldcxt);
		}
		estate.retval = (Datum) 0;
		fcinfo->isnull = true;
	}
	else if (!estate.retisnull)
	{
		/*
		 * 把结果值转换成函数声明的结果类型，并把它拷
		 * 出到上层执行器内存上下文。对元组
		 * 结果必须特殊处理，以对付诸如行类型
		 * 含已删列的情形。
		 */
		if (estate.retistuple)
		{
			/* 若已知行类型匹配，则不必强制 */
			if (func->fn_rettype == estate.rettype &&
				func->fn_rettype != RECORDOID)
			{
				/*
				 * 把元组结果拷进上层执行器内存上下文。
				 * 不过若已有 R/W 扩展 datum，就可以只
				 * 把它的所有权转出到上层上下文。
				 */
				estate.retval = SPI_datumTransfer(estate.retval,
												  false,
												  -1);
			}
			else
			{
				/*
				 * 需要查出预期的结果类型。XXX 更好是
				 * 去缓存 tupdesc，而不是反复调用
				 * get_call_result_type()，但唯一易存之处是
				 * PLpgSQL_function 结构内，而那过于
				 * 长命：复合类型在其存续期间可能
				 * 改变。
				 */
				Oid			resultTypeId;
				TupleDesc	tupdesc;

				switch (get_call_result_type(fcinfo, &resultTypeId, &tupdesc))
				{
					case TYPEFUNC_COMPOSITE:
						/* 已得期望结果行类型，现在强制 */
						coerce_function_result_tuple(&estate, tupdesc);
						break;
					case TYPEFUNC_COMPOSITE_DOMAIN:
						/* 已得期望结果行类型，现在强制 */
						coerce_function_result_tuple(&estate, tupdesc);
						/* 并检查域约束 */
						/* XXX 这里若能缓存也好 */
						domain_check(estate.retval, false, resultTypeId,
									 NULL, NULL);
						break;
					case TYPEFUNC_RECORD:

						/*
						 * RECORD 实际类型未能确定。我们
						 * 本可在此报错，但这在实践中意味着
						 * 调用者预期的是随便一种旧的
						 * 通用行类型，所以我们实在不必
						 * 苛刻。生成的结果就原样传回。
						 */
						estate.retval = SPI_datumTransfer(estate.retval,
														  false,
														  -1);
						break;
					default:
						/* retistuple 为真则不该到此 ... */
						elog(ERROR, "return type must be a row type");
						break;
				}
			}
		}
		else
		{
			/* 标量情形用 exec_cast_value */
			estate.retval = exec_cast_value(&estate,
											estate.retval,
											&fcinfo->isnull,
											estate.rettype,
											-1,
											func->fn_rettype,
											-1);

			/*
			 * 若函数的返回类型不按值，就把值拷入
			 * 上层执行器内存上下文。不过若已有 R/W
			 * 扩展 datum，就可只把它的所有权转出到
			 * 上层上下文。
			 */
			if (!fcinfo->isnull && !func->fn_retbyval)
				estate.retval = SPI_datumTransfer(estate.retval,
												  false,
												  func->fn_rettyplen);
		}
	}
	else
	{
		/*
		 * 我们返回的是 NULL，通常不必做转换，
		 * 无论数据类型如何。但若把它转换成域
		 * 返回类型，最好检查该域约束能否通过。
		 */
		if (func->fn_retisdomain)
			estate.retval = exec_cast_value(&estate,
											estate.retval,
											&fcinfo->isnull,
											estate.rettype,
											-1,
											func->fn_rettype,
											-1);
	}

	estate.err_text = gettext_noop("during function exit");

	/*
	 * 让插装插件看一眼这个函数
	 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->func_end)
		((*plpgsql_plugin_ptr)->func_end) (&estate, func);

	/* 清掉残留的临时内存 */
	plpgsql_destroy_econtext(&estate);
	exec_eval_cleanup(&estate);
	/* 函数主上下文一毁，stmt_mcontext 就随之销毁 */

	/*
	 * 弹出错误上下文栈
	 */
	error_context_stack = plerrcontext.previous;

	/*
	 * 返回该函数的结果
	 */
	return estate.retval;
}

/*
 * plpgsql_exec_function 的助手：把复合结果强制到指定
 * 元组描述符，并把它拷出到上层执行器内存。拆出来
 * 主要图外观，否则这段逻辑嵌套太深
 * 而已。
 *
 * 就地更新 estate->retval。
 */
static void
coerce_function_result_tuple(PLpgSQL_execstate *estate, TupleDesc tupdesc)
{
	HeapTuple	rettup;
	TupleDesc	retdesc;
	TupleConversionMap *tupmap;

	/* 假定 exec_stmt_return 已核实结果是复合的 */
	Assert(type_is_rowtype(estate->rettype));

	/* 扩展记录可以走特例以求快 */
	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(estate->retval)))
	{
		ExpandedRecordHeader *erh = (ExpandedRecordHeader *) DatumGetEOHP(estate->retval);

		Assert(erh->er_magic == ER_MAGIC);

		/* 取出记录的 TupleDesc */
		retdesc = expanded_record_get_tupdesc(erh);

		/* 检查行类型兼容性 */
		tupmap = convert_tuples_by_position(retdesc,
											tupdesc,
											gettext_noop("returned record type does not match expected record type"));

		/* 它可能需要转换 */
		if (tupmap)
		{
			rettup = expanded_record_get_tuple(erh);
			Assert(rettup);
			rettup = execute_attr_map_tuple(rettup, tupmap);

			/*
			 * 作为元组 Datum 拷入上层执行器内存。要
			 * 确保它标上调用者给出的元组类型。
			 */
			estate->retval = PointerGetDatum(SPI_returntuple(rettup, tupdesc));
			/* 不必释放映射，反正马上返回 */
		}
		else if (!(tupdesc->tdtypeid == erh->er_decltypeid ||
				   (tupdesc->tdtypeid == RECORDOID &&
					!ExpandedRecordIsDomain(erh))))
		{
			/*
			 * 扩展记录的物理 tupdesc 是对的，但类型
			 * 标识错了。（通常扩展记录是 RECORDOID，
			 * 而函数声明返回具名复合类型。同
			 * exec_move_row_from_datum 那样，不准由声明为
			 * 返回 RECORD 的函数返回复合域记录。）
			 * 所以必须把该记录压扁成元组 datum 并
			 * 覆盖其类型字段为正确内容。spi.c 并不提供
			 * 对付此种情形的简便途径，结果我们只好
			 * 复制 datumCopy() 的核心代码 :-(
			 */
			Size		resultsize;
			HeapTupleHeader tuphdr;

			resultsize = EOH_get_flat_size(&erh->hdr);
			tuphdr = (HeapTupleHeader) SPI_palloc(resultsize);
			EOH_flatten_into(&erh->hdr, (void *) tuphdr, resultsize);
			HeapTupleHeaderSetTypeId(tuphdr, tupdesc->tdtypeid);
			HeapTupleHeaderSetTypMod(tuphdr, tupdesc->tdtypmod);
			estate->retval = PointerGetDatum(tuphdr);
		}
		else
		{
			/*
			 * 我们只需把结果拷进上层执行器内存上下文。
			 * 不过若有 R/W 扩展 datum，就可以只转移
			 * 其所有权到上层执行器上下文。
			 */
			estate->retval = SPI_datumTransfer(estate->retval,
											   false,
											   -1);
		}
	}
	else
	{
		/* 把复合 datum 转成 HeapTuple 和 TupleDesc */
		HeapTupleData tmptup;

		retdesc = deconstruct_composite_datum(estate->retval, &tmptup);
		rettup = &tmptup;

		/* 检查行类型兼容性 */
		tupmap = convert_tuples_by_position(retdesc,
											tupdesc,
											gettext_noop("returned record type does not match expected record type"));

		/* 它可能需要转换 */
		if (tupmap)
			rettup = execute_attr_map_tuple(rettup, tupmap);

		/*
		 * 当 Datum 把元组拷入上层执行器内存。务必确保
		 * 它标着调用者给出的元组类型。
		 */
		estate->retval = PointerGetDatum(SPI_returntuple(rettup, tupdesc));

		/* 不必释放映射，反正马上返回 */

		ReleaseTupleDesc(retdesc);
	}
}


/* ----------
 * plpgsql_exec_trigger		供调用处理器来执行
 *				触发器。
 * ----------
 */
HeapTuple
plpgsql_exec_trigger(PLpgSQL_function *func,
					 TriggerData *trigdata)
{
	PLpgSQL_execstate estate;
	ErrorContextCallback plerrcontext;
	int			rc;
	TupleDesc	tupdesc;
	PLpgSQL_rec *rec_new,
			   *rec_old;
	HeapTuple	rettup;

	/*
	 * 设置执行状态
	 */
	plpgsql_estate_setup(&estate, func, NULL, NULL, NULL);
	estate.trigdata = trigdata;

	/*
	 * 为 ereport() 设错误回溯支持
	 */
	plerrcontext.callback = plpgsql_exec_error_callback;
	plerrcontext.arg = &estate;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/*
	 * 给全部 datum 做局部执行副本
	 */
	estate.err_text = gettext_noop("during initialization of execution state");
	copy_plpgsql_datums(&estate, func);

	/*
	 * 把 OLD 与 NEW 元组放进记录变量
	 *
	 * 我们给两个变量都建扩展记录，尽管也许只一个
	 * 有值。这样，用于多种触发器类型的函数里
	 * 记录引用仍能成功。例如我们也许写过
	 * 如 if (TG_OP = 'INSERT' and NEW.foo = 'xyz')，它应当
	 * 不论当前是哪种触发器都能工作。若真从一个
	 * 未提供的元组里取值，读到的是 NULL。
	 */
	tupdesc = RelationGetDescr(trigdata->tg_relation);

	rec_new = (PLpgSQL_rec *) (estate.datums[func->new_varno]);
	rec_old = (PLpgSQL_rec *) (estate.datums[func->old_varno]);

	rec_new->erh = make_expanded_record_from_tupdesc(tupdesc,
													 estate.datum_context);
	rec_old->erh = make_expanded_record_from_exprecord(rec_new->erh,
													   estate.datum_context);

	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
	{
		/*
		 * 语句级触发器不用 OLD/NEW 变量
		 */
	}
	else if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
	{
		expanded_record_set_tuple(rec_new->erh, trigdata->tg_trigtuple,
								  false, false);
	}
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
	{
		expanded_record_set_tuple(rec_new->erh, trigdata->tg_newtuple,
								  false, false);
		expanded_record_set_tuple(rec_old->erh, trigdata->tg_trigtuple,
								  false, false);

		/*
		 * BEFORE 触发器中，存储生成列还没算好，
		 * 故把 NEW 行里的它们置 NULL。（只 UPDATE 分支要；
		 * INSERT 时它们已是 NULL，但 UPDATE 里该字段
		 * 仍是旧值。）另一办法是我们可以造出一个
		 * 不含生成列的全新行结构，但眼下这办法
		 * 似乎更高效，也更少让人糊涂。
		 */
		if (tupdesc->constr && tupdesc->constr->has_generated_stored &&
			TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		{
			for (int i = 0; i < tupdesc->natts; i++)
				if (TupleDescAttr(tupdesc, i)->attgenerated == ATTRIBUTE_GENERATED_STORED)
					expanded_record_set_field_internal(rec_new->erh,
													   i + 1,
													   (Datum) 0,
													   true,	/* 空否 */
													   false, false);
		}
	}
	else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
	{
		expanded_record_set_tuple(rec_old->erh, trigdata->tg_trigtuple,
								  false, false);
	}
	else
		elog(ERROR, "unrecognized trigger action: not INSERT, DELETE, or UPDATE");

	/* 让过渡表对本 SPI 连接可见 */
	rc = SPI_register_trigger_data(trigdata);
	Assert(rc >= 0);

	estate.err_text = gettext_noop("during function entry");

	/*
	 * 把魔术变量 FOUND 设为假
	 */
	exec_set_found(&estate, false);

	/*
	 * 让插装插件看一眼这个函数
	 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->func_beg)
		((*plpgsql_plugin_ptr)->func_beg) (&estate, func);

	/*
	 * 现在调用顶层语句块
	 */
	estate.err_text = NULL;
	rc = exec_toplevel_block(&estate, func->action);
	if (rc != PLPGSQL_RC_RETURN)
	{
		estate.err_text = NULL;
		ereport(ERROR,
				(errcode(ERRCODE_S_R_E_FUNCTION_EXECUTED_NO_RETURN_STATEMENT),
				 errmsg("control reached end of trigger procedure without RETURN")));
	}

	estate.err_text = gettext_noop("during function exit");

	if (estate.retisset)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("trigger procedure cannot return a set")));

	/*
	 * 检查返回的元组结构是否与触发该触发器的
	 * 关系拥有相同属性。语句级触发器始终要
	 * 返回 NULL，故我们忽略函数自身产生的任何
	 * 返回值。XXX：好主意吗？
	 *
	 * XXX 按此做法，触发器可能返回这样的元组：
	 * 其属性的 atttypmod 长度并不正确。这得靠
	 * 触发器程序员来保证此事不发生。Jan
	 */
	if (estate.retisnull || !TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		rettup = NULL;
	else
	{
		TupleDesc	retdesc;
		TupleConversionMap *tupmap;

		/* 假定 exec_stmt_return 已核实结果是复合的 */
		Assert(type_is_rowtype(estate.rettype));

		/* 扩展记录可以走特例以求快 */
		if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(estate.retval)))
		{
			ExpandedRecordHeader *erh = (ExpandedRecordHeader *) DatumGetEOHP(estate.retval);

			Assert(erh->er_magic == ER_MAGIC);

			/* 取出 HeapTuple 与 TupleDesc */
			rettup = expanded_record_get_tuple(erh);
			Assert(rettup);
			retdesc = expanded_record_get_tupdesc(erh);

			if (retdesc != RelationGetDescr(trigdata->tg_relation))
			{
				/* 检查行类型兼容性 */
				tupmap = convert_tuples_by_position(retdesc,
													RelationGetDescr(trigdata->tg_relation),
													gettext_noop("returned row structure does not match the structure of the triggering table"));
				/* 它可能需要转换 */
				if (tupmap)
					rettup = execute_attr_map_tuple(rettup, tupmap);
				/* 不必释放映射，反正马上返回 */
			}

			/*
			 * 元组拷入上层执行器内存。但若用户只是
			 * 写了 return-new、return-old 且毫无改动，便
			 * 不必拷贝；我们可以返回原来的元组（这也
			 * 在 trigger.c 和这里省些周期）。
			 */
			if (rettup != trigdata->tg_newtuple &&
				rettup != trigdata->tg_trigtuple)
				rettup = SPI_copytuple(rettup);
		}
		else
		{
			/* 把复合 datum 转成 HeapTuple 和 TupleDesc */
			HeapTupleData tmptup;

			retdesc = deconstruct_composite_datum(estate.retval, &tmptup);
			rettup = &tmptup;

			/* 检查行类型兼容性 */
			tupmap = convert_tuples_by_position(retdesc,
												RelationGetDescr(trigdata->tg_relation),
												gettext_noop("returned row structure does not match the structure of the triggering table"));
			/* 它可能需要转换 */
			if (tupmap)
				rettup = execute_attr_map_tuple(rettup, tupmap);

			ReleaseTupleDesc(retdesc);
			/* 不必释放映射，反正马上返回 */

			/* 元组拷入上层执行器内存 */
			rettup = SPI_copytuple(rettup);
		}
	}

	/*
	 * 让插装插件看一眼这个函数
	 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->func_end)
		((*plpgsql_plugin_ptr)->func_end) (&estate, func);

	/* 清掉残留的临时内存 */
	plpgsql_destroy_econtext(&estate);
	exec_eval_cleanup(&estate);
	/* 函数主上下文一毁，stmt_mcontext 就随之销毁 */

	/*
	 * 弹出错误上下文栈
	 */
	error_context_stack = plerrcontext.previous;

	/*
	 * 返回该触发器的结果
	 */
	return rettup;
}

/* ----------
 * plpgsql_exec_event_trigger		供调用处理器来执行
 *				事件触发器。
 * ----------
 */
void
plpgsql_exec_event_trigger(PLpgSQL_function *func, EventTriggerData *trigdata)
{
	PLpgSQL_execstate estate;
	ErrorContextCallback plerrcontext;
	int			rc;

	/*
	 * 设置执行状态
	 */
	plpgsql_estate_setup(&estate, func, NULL, NULL, NULL);
	estate.evtrigdata = trigdata;

	/*
	 * 为 ereport() 设错误回溯支持
	 */
	plerrcontext.callback = plpgsql_exec_error_callback;
	plerrcontext.arg = &estate;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/*
	 * 给全部 datum 做局部执行副本
	 */
	estate.err_text = gettext_noop("during initialization of execution state");
	copy_plpgsql_datums(&estate, func);

	/*
	 * 让插装插件看一眼这个函数
	 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->func_beg)
		((*plpgsql_plugin_ptr)->func_beg) (&estate, func);

	/*
	 * 现在调用顶层语句块
	 */
	estate.err_text = NULL;
	rc = exec_toplevel_block(&estate, func->action);
	if (rc != PLPGSQL_RC_RETURN)
	{
		estate.err_text = NULL;
		ereport(ERROR,
				(errcode(ERRCODE_S_R_E_FUNCTION_EXECUTED_NO_RETURN_STATEMENT),
				 errmsg("control reached end of trigger procedure without RETURN")));
	}

	estate.err_text = gettext_noop("during function exit");

	/*
	 * 让插装插件看一眼这个函数
	 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->func_end)
		((*plpgsql_plugin_ptr)->func_end) (&estate, func);

	/* 清掉残留的临时内存 */
	plpgsql_destroy_econtext(&estate);
	exec_eval_cleanup(&estate);
	/* 函数主上下文一毁，stmt_mcontext 就随之销毁 */

	/*
	 * 弹出错误上下文栈
	 */
	error_context_stack = plerrcontext.previous;
}

/*
 * 错误上下文回调，用来提供调用栈回溯
 */
static void
plpgsql_exec_error_callback(void *arg)
{
	PLpgSQL_execstate *estate = (PLpgSQL_execstate *) arg;
	int			err_lineno;

	/*
	 * 若 err_var 已设置，就报告该变量的声明行号。
	 * 否则若 err_stmt 已设，就报告它的行号。当
	 * err_stmt 未设置，我们就在函数进入、退出这类
	 * 没有挂到具体行号的地方。
	 */
	if (estate->err_var != NULL)
		err_lineno = estate->err_var->lineno;
	else if (estate->err_stmt != NULL)
		err_lineno = estate->err_stmt->lineno;
	else
		err_lineno = 0;

	if (estate->err_text != NULL)
	{
		/*
		 * 除非真需要，我们不对 err_text 花周期去跑
		 * gettext()。因此，设置 err_text 的地方应当
		 * 使用 gettext_noop()，好让这些字符串记入
		 * 消息词典。
		 */
		if (err_lineno > 0)
		{
			/*
			 * translator: last %s is a phrase such as "during statement block
			 * local variable initialization"
			 */
			errcontext("PL/pgSQL function %s line %d %s",
					   estate->func->fn_signature,
					   err_lineno,
					   _(estate->err_text));
		}
		else
		{
			/*
			 * translator: last %s is a phrase such as "while storing call
			 * arguments into local variables"
			 */
			errcontext("PL/pgSQL function %s %s",
					   estate->func->fn_signature,
					   _(estate->err_text));
		}
	}
	else if (estate->err_stmt != NULL && err_lineno > 0)
	{
		/* translator: last %s is a plpgsql statement type name */
		errcontext("PL/pgSQL function %s line %d at %s",
				   estate->func->fn_signature,
				   err_lineno,
				   plpgsql_stmt_typename(estate->err_stmt));
	}
	else
		errcontext("PL/pgSQL function %s",
				   estate->func->fn_signature);
}


/* ----------
 * 初始化局部执行变量的支撑函数
 * ----------
 */
static void
copy_plpgsql_datums(PLpgSQL_execstate *estate,
					PLpgSQL_function *func)
{
	int			ndatums = estate->ndatums;
	PLpgSQL_datum **indatums;
	PLpgSQL_datum **outdatums;
	char	   *workspace;
	char	   *ws_next;
	int			i;

	/* 分配局部 datum 指针数组 */
	estate->datums = (PLpgSQL_datum **)
		palloc(sizeof(PLpgSQL_datum *) * ndatums);

	/*
	 * 为降低 palloc 开销，我们一次 palloc 申请全部
	 * 局部实例化 datum 所需的空间。
	 */
	workspace = palloc(func->copiable_size);
	ws_next = workspace;

	/* 填充 datum 指针数组，按需把 datum 拷入工作区 */
	indatums = func->datums;
	outdatums = estate->datums;
	for (i = 0; i < ndatums; i++)
	{
		PLpgSQL_datum *indatum = indatums[i];
		PLpgSQL_datum *outdatum;

		/* 哪些可拷贝必须与 plpgsql_finish_datums 一致 */
		switch (indatum->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
			case PLPGSQL_DTYPE_PROMISE:
				outdatum = (PLpgSQL_datum *) ws_next;
				memcpy(outdatum, indatum, sizeof(PLpgSQL_var));
				ws_next += MAXALIGN(sizeof(PLpgSQL_var));
				break;

			case PLPGSQL_DTYPE_REC:
				outdatum = (PLpgSQL_datum *) ws_next;
				memcpy(outdatum, indatum, sizeof(PLpgSQL_rec));
				ws_next += MAXALIGN(sizeof(PLpgSQL_rec));
				break;

			case PLPGSQL_DTYPE_ROW:
			case PLPGSQL_DTYPE_RECFIELD:

				/*
				 * 这些 datum 记录运行时只读，所以不必
				 * 拷贝它们（RECFIELD 虽含缓存数据，但我们
				 * 宁可把缓存集中在一处）。
				 */
				outdatum = indatum;
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", indatum->dtype);
				outdatum = NULL;	/* 让编译器安静 */
				break;
		}

		outdatums[i] = outdatum;
	}

	Assert(ws_next == workspace + func->copiable_size);
}

/*
 * 若变量的承诺已武装，就算出所承诺的值
 * 并把它赋给该变量。
 * 这次赋值自动解除该承诺。
 */
static void
plpgsql_fulfill_promise(PLpgSQL_execstate *estate,
						PLpgSQL_var *var)
{
	MemoryContext oldcontext;

	if (var->promise == PLPGSQL_PROMISE_NONE)
		return;					/* 无事可做 */

	/*
	 * 这通常在短命上下文里被调用，例如
	 * mcontext。我们必须把变量值建在 estate 的 datum
	 * 内存上下文里。这个粗陋办法可能再泄漏一些
	 * 杂物，但任一承诺每次函数调用至多兑现一次，
	 * 更小心大概不值得。
	 */
	oldcontext = MemoryContextSwitchTo(estate->datum_context);

	switch (var->promise)
	{
		case PLPGSQL_PROMISE_TG_NAME:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			assign_simple_var(estate, var,
							  DirectFunctionCall1(namein,
												  CStringGetDatum(estate->trigdata->tg_trigger->tgname)),
							  false, true);
			break;

		case PLPGSQL_PROMISE_TG_WHEN:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			if (TRIGGER_FIRED_BEFORE(estate->trigdata->tg_event))
				assign_text_var(estate, var, "BEFORE");
			else if (TRIGGER_FIRED_AFTER(estate->trigdata->tg_event))
				assign_text_var(estate, var, "AFTER");
			else if (TRIGGER_FIRED_INSTEAD(estate->trigdata->tg_event))
				assign_text_var(estate, var, "INSTEAD OF");
			else
				elog(ERROR, "unrecognized trigger execution time: not BEFORE, AFTER, or INSTEAD OF");
			break;

		case PLPGSQL_PROMISE_TG_LEVEL:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			if (TRIGGER_FIRED_FOR_ROW(estate->trigdata->tg_event))
				assign_text_var(estate, var, "ROW");
			else if (TRIGGER_FIRED_FOR_STATEMENT(estate->trigdata->tg_event))
				assign_text_var(estate, var, "STATEMENT");
			else
				elog(ERROR, "unrecognized trigger event type: not ROW or STATEMENT");
			break;

		case PLPGSQL_PROMISE_TG_OP:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			if (TRIGGER_FIRED_BY_INSERT(estate->trigdata->tg_event))
				assign_text_var(estate, var, "INSERT");
			else if (TRIGGER_FIRED_BY_UPDATE(estate->trigdata->tg_event))
				assign_text_var(estate, var, "UPDATE");
			else if (TRIGGER_FIRED_BY_DELETE(estate->trigdata->tg_event))
				assign_text_var(estate, var, "DELETE");
			else if (TRIGGER_FIRED_BY_TRUNCATE(estate->trigdata->tg_event))
				assign_text_var(estate, var, "TRUNCATE");
			else
				elog(ERROR, "unrecognized trigger action: not INSERT, DELETE, UPDATE, or TRUNCATE");
			break;

		case PLPGSQL_PROMISE_TG_RELID:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			assign_simple_var(estate, var,
							  ObjectIdGetDatum(estate->trigdata->tg_relation->rd_id),
							  false, false);
			break;

		case PLPGSQL_PROMISE_TG_TABLE_NAME:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			assign_simple_var(estate, var,
							  DirectFunctionCall1(namein,
												  CStringGetDatum(RelationGetRelationName(estate->trigdata->tg_relation))),
							  false, true);
			break;

		case PLPGSQL_PROMISE_TG_TABLE_SCHEMA:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			assign_simple_var(estate, var,
							  DirectFunctionCall1(namein,
												  CStringGetDatum(get_namespace_name(RelationGetNamespace(estate->trigdata->tg_relation)))),
							  false, true);
			break;

		case PLPGSQL_PROMISE_TG_NARGS:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			assign_simple_var(estate, var,
							  Int16GetDatum(estate->trigdata->tg_trigger->tgnargs),
							  false, false);
			break;

		case PLPGSQL_PROMISE_TG_ARGV:
			if (estate->trigdata == NULL)
				elog(ERROR, "trigger promise is not in a trigger function");
			if (estate->trigdata->tg_trigger->tgnargs > 0)
			{
				/*
				 * 出于历史原因，tg_argv[] 的下标从零起
				 * 非一。故无法用 construct_array()。
				 */
				int			nelems = estate->trigdata->tg_trigger->tgnargs;
				Datum	   *elems;
				int			dims[1];
				int			lbs[1];
				int			i;

				elems = palloc(sizeof(Datum) * nelems);
				for (i = 0; i < nelems; i++)
					elems[i] = CStringGetTextDatum(estate->trigdata->tg_trigger->tgargs[i]);
				dims[0] = nelems;
				lbs[0] = 0;

				assign_simple_var(estate, var,
								  PointerGetDatum(construct_md_array(elems, NULL,
																	 1, dims, lbs,
																	 TEXTOID,
																	 -1, false, TYPALIGN_INT)),
								  false, true);
			}
			else
			{
				assign_simple_var(estate, var, (Datum) 0, true, false);
			}
			break;

		case PLPGSQL_PROMISE_TG_EVENT:
			if (estate->evtrigdata == NULL)
				elog(ERROR, "event trigger promise is not in an event trigger function");
			assign_text_var(estate, var, estate->evtrigdata->event);
			break;

		case PLPGSQL_PROMISE_TG_TAG:
			if (estate->evtrigdata == NULL)
				elog(ERROR, "event trigger promise is not in an event trigger function");
			assign_text_var(estate, var, GetCommandTagName(estate->evtrigdata->tag));
			break;

		default:
			elog(ERROR, "unrecognized promise type: %d", var->promise);
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * 若还没有，就为语句寿命的变量建内存上下文
 * 它将作为 stmt_mcontext_parent 的子级，而该父级
 * 要么是函数主上下文，要么是压下的外层 stmt_mcontext。
 */
static MemoryContext
get_stmt_mcontext(PLpgSQL_execstate *estate)
{
	if (estate->stmt_mcontext == NULL)
	{
		estate->stmt_mcontext =
			AllocSetContextCreate(estate->stmt_mcontext_parent,
								  "PLpgSQL per-statement data",
								  ALLOCSET_DEFAULT_SIZES);
	}
	return estate->stmt_mcontext;
}

/*
 * 压下当前 stmt_mcontext，免得被调语句用到它。
 * 带有语句寿命数据、且要跨若干内层语句保住
 * 它的语句才需要如此。调用者最终应当去
 * pop_stmt_mcontext()
 */
static void
push_stmt_mcontext(PLpgSQL_execstate *estate)
{
	/* 本应先调用 get_stmt_mcontext() */
	Assert(estate->stmt_mcontext != NULL);
	/* 断言我们没有弄乱栈的链接 */
	Assert(MemoryContextGetParent(estate->stmt_mcontext) == estate->stmt_mcontext_parent);
	/* 把它压下当任何嵌套 stmt 内存上下文之父 */
	estate->stmt_mcontext_parent = estate->stmt_mcontext;
	/* 并使它不能直接拿来用 */
	estate->stmt_mcontext = NULL;
}

/*
 * 撤销 push_stmt_mcontext()。假定这发生在重置调用者的
 * stmt_mcontext 前后；因该动作同时删除一切
 * 子上下文，故不必显式删除当前也许就是
 * estate->stmt_mcontext。
 */
static void
pop_stmt_mcontext(PLpgSQL_execstate *estate)
{
	/* 我们只需弹出该栈 */
	estate->stmt_mcontext = estate->stmt_mcontext_parent;
	estate->stmt_mcontext_parent = MemoryContextGetParent(estate->stmt_mcontext);
}


/*
 * exec_stmt_block 的子程序：条件列表中可有条件
 * 匹配当前这个异常？
 */
static bool
exception_matches_conditions(ErrorData *edata, PLpgSQL_condition *cond)
{
	for (; cond != NULL; cond = cond->next)
	{
		int			sqlerrstate = cond->sqlerrstate;

		/*
		 * OTHERS 匹配除查询取消与断言失败外的
		 * 一切。够鲁莽的话，你也能显式去匹配
		 * 它们。
		 */
		if (sqlerrstate == 0)
		{
			if (edata->sqlerrcode != ERRCODE_QUERY_CANCELED &&
				edata->sqlerrcode != ERRCODE_ASSERT_FAILURE)
				return true;
		}
		/* 精确吗？ */
		else if (edata->sqlerrcode == sqlerrstate)
			return true;
		/* 类别匹配？ */
		else if (ERRCODE_IS_CATEGORY(sqlerrstate) &&
				 ERRCODE_TO_CATEGORY(edata->sqlerrcode) == sqlerrstate)
			return true;
	}
	return false;
}


/* ----------
 * exec_toplevel_block 执行顶层块。
 *
 * 这有意等价于执行 exec_stmts()，并配上一个
 * 只含那一条语句的列表。一个细微差别是
 * 我们并不费心保存 estate->err_stmt 进入值；
 * 假定该值为 NULL。
 * ----------
 */
static int
exec_toplevel_block(PLpgSQL_execstate *estate, PLpgSQL_stmt_block *block)
{
	int			rc;

	estate->err_stmt = (PLpgSQL_stmt *) block;

	/* 让插件知道我们即将执行这条语句 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->stmt_beg)
		((*plpgsql_plugin_ptr)->stmt_beg) (estate, (PLpgSQL_stmt *) block);

	CHECK_FOR_INTERRUPTS();

	rc = exec_stmt_block(estate, block);

	/* 让插件知道这条语句已经执行完 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->stmt_end)
		((*plpgsql_plugin_ptr)->stmt_end) (estate, (PLpgSQL_stmt *) block);

	estate->err_stmt = NULL;

	return rc;
}


/* ----------
 * exec_stmt_block 执行一块语句
 * ----------
 */
static int
exec_stmt_block(PLpgSQL_execstate *estate, PLpgSQL_stmt_block *block)
{
	volatile int rc = -1;
	int			i;

	/*
	 * 先初始化本块中声明的全部变量
	 */
	estate->err_text = gettext_noop("during statement block local variable initialization");

	for (i = 0; i < block->n_initvars; i++)
	{
		int			n = block->initvarnos[i];
		PLpgSQL_datum *datum = estate->datums[n];

		/*
		 * 此处处理的 dtype 集必须匹配 plpgsql_add_initdatums()。
		 *
		 * 注意我们目前不支持块内的承诺 datum，
		 * 只在函数最外层作用域，故那些不必在
		 * 此.
		 *
		 * 因 RECFIELD 同样不是受支持情形，可把 PLpgSQL_datum
		 * 转成 PLpgSQL_variable。
		 */
		estate->err_var = (PLpgSQL_variable *) datum;

		switch (datum->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) datum;

					/*
					 * 释放任一旧值，以防重入块，并
					 * 初始化为 NULL
					 */
					assign_simple_var(estate, var, (Datum) 0, true, false);

					if (var->default_val == NULL)
					{
						/*
						 * 需要时让数据类型有机会拒绝
						 * NULL，靠给变量赋 NULL。我们
						 * 声称该值类型是 UNKNOWN，而非变量的
						 * 类型，否则跳过强制转换。
						 */
						if (var->datatype->typtype == TYPTYPE_DOMAIN)
							exec_assign_value(estate,
											  (PLpgSQL_datum *) var,
											  (Datum) 0,
											  true,
											  UNKNOWNOID,
											  -1);

						/* 解析器本该拒绝 NOT NULL */
						Assert(!var->notnull);
					}
					else
					{
						exec_assign_expr(estate, (PLpgSQL_datum *) var,
										 var->default_val);
					}
				}
				break;

			case PLPGSQL_DTYPE_REC:
				{
					PLpgSQL_rec *rec = (PLpgSQL_rec *) datum;

					/*
					 * 任何现存对象的删除，会在下面的
					 * 赋值过程中处理，而且有些情况下
					 * 我们事先不清除它反而更高效。
					 */
					if (rec->default_val == NULL)
					{
						/*
						 * 需要时让数据类型有机会拒绝
						 * NULL，靠给该变量赋 NULL。
						 */
						exec_move_row(estate, (PLpgSQL_variable *) rec,
									  NULL, NULL);

						/* 解析器本该拒绝 NOT NULL */
						Assert(!rec->notnull);
					}
					else
					{
						exec_assign_expr(estate, (PLpgSQL_datum *) rec,
										 rec->default_val);
					}
				}
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", datum->dtype);
		}
	}

	estate->err_var = NULL;

	if (block->exceptions)
	{
		/*
		 * 在子事务里执行该块体中的语句
		 */
		MemoryContext oldcontext = CurrentMemoryContext;
		ResourceOwner oldowner = CurrentResourceOwner;
		ExprContext *old_eval_econtext = estate->eval_econtext;
		ErrorData  *save_cur_error = estate->cur_error;
		MemoryContext stmt_mcontext;

		estate->err_text = gettext_noop("during statement block entry");

		/*
		 * 若出错，我们需要 stmt_mcontext 来存放错误数据。
		 * 最好在进入子事务之前强迫它存在，
		 * 从而我们降低错误恢复期间耗尽内存的风险，
		 * 也因为这大大简化了出错后把
		 * stmt_mcontext 栈恢复到正确状态。我们可
		 * 让被调语句也用此内存上下文来分摊代价，
		 * 因此我们在此不把它压下去。
		 */
		stmt_mcontext = get_stmt_mcontext(estate);

		BeginInternalSubTransaction(NULL);
		/* 想在函数的内存上下文里运行语句 */
		MemoryContextSwitchTo(oldcontext);

		PG_TRY();
		{
			/*
			 * 我们需用一个新的 eval_econtext 来运行块语句，
			 * 它属于当前子事务；若我们试图使用
			 * 外层 econtext，则 ExprContext 的关闭回调会
			 * 在错时机被调用。
			 */
			plpgsql_create_econtext(estate);

			estate->err_text = NULL;

			/* 运行该块的语句 */
			rc = exec_stmts(estate, block->body);

			estate->err_text = gettext_noop("during statement block exit");

			/*
			 * 若块以 RETURN 结束，我们可能要把返回
			 * 值拷出子事务的 eval_context。我们能避免
			 * 一次物理拷贝，若该值恰为 R/W 扩展对象。
			 */
			if (rc == PLPGSQL_RC_RETURN &&
				!estate->retisset &&
				!estate->retisnull)
			{
				int16		resTypLen;
				bool		resTypByVal;

				get_typlenbyval(estate->rettype, &resTypLen, &resTypByVal);
				estate->retval = datumTransfer(estate->retval,
											   resTypByVal, resTypLen);
			}

			/* 提交内层事务，回到外层事务上下文 */
			ReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(oldcontext);
			CurrentResourceOwner = oldowner;

			/* 断言 stmt_mcontext 栈没有改变 */
			Assert(stmt_mcontext == estate->stmt_mcontext);

			/*
			 * 回到外层 eval_econtext。（内层已
			 * 在子事务退出时被自动清理。）
			 */
			estate->eval_econtext = old_eval_econtext;
		}
		PG_CATCH();
		{
			ErrorData  *edata;
			ListCell   *e;

			estate->err_text = gettext_noop("during exception cleanup");

			/* 错误信息存入本 stmt_mcontext */
			MemoryContextSwitchTo(stmt_mcontext);
			edata = CopyErrorData();
			FlushErrorState();

			/* 中止内层事务 */
			RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(oldcontext);
			CurrentResourceOwner = oldowner;

			/*
			 * 把 stmt_mcontext 栈设成仿佛我们已恢复了
			 * 先前状态，然后做过 push_stmt_mcontext()。压栈
			 * 之所以需要，以免异常处理器中语句
			 * 破坏我们 stmt_mcontext 里的错误数据。
			 */
			estate->stmt_mcontext_parent = stmt_mcontext;
			estate->stmt_mcontext = NULL;

			/*
			 * 我们现在可删任何嵌套 stmt_mcontexts，可能已
			 * 作为我们的子级被创建。（注意：我们并不立刻
			 * 释放任何语句寿命数据，它们可能被留
			 * 在 stmt_mcontext 自身里。我们可尝试靠
			 * 一次 MemoryContextReset，在收集上文错误数据
			 * 之前，但在收集到错误之前做任何大量工作
			 * 似乎都太冒险。）
			 */
			MemoryContextDeleteChildren(stmt_mcontext);

			/* 恢复到外层 eval_econtext */
			estate->eval_econtext = old_eval_econtext;

			/*
			 * 也必须清理 econtext。不过，子事务里建的
			 * 任何元组表，都会在子事务中止时被 SPI 丢掉，
			 * 所以我们不必（也不得尝试）释放
			 * eval_tuptable.
			 */
			estate->eval_tuptable = NULL;
			exec_eval_cleanup(estate);

			/* 寻找匹配的异常处理器 */
			foreach(e, block->exceptions->exc_list)
			{
				PLpgSQL_exception *exception = (PLpgSQL_exception *) lfirst(e);

				if (exception_matches_conditions(edata, exception->conditions))
				{
					/*
					 * 为异常块初始化魔术变量 SQLSTATE、SQLERRM；
					 * 这也释放该异常先前使用所留的
					 * 任何值。匹配异常找到前我们不必
					 * 做这事。
					 */
					PLpgSQL_var *state_var;
					PLpgSQL_var *errm_var;

					state_var = (PLpgSQL_var *)
						estate->datums[block->exceptions->sqlstate_varno];
					errm_var = (PLpgSQL_var *)
						estate->datums[block->exceptions->sqlerrm_varno];

					assign_text_var(estate, state_var,
									unpack_sql_state(edata->sqlerrcode));
					assign_text_var(estate, errm_var, edata->message);

					/*
					 * 也设 cur_error 让错误数据可在处理器
					 * 内部访问。
					 */
					estate->cur_error = edata;

					estate->err_text = NULL;

					rc = exec_stmts(estate, exception->action);

					break;
				}
			}

			/*
			 * 恢复 cur_error 先前状态，无论我们有没有执行
			 * 过处理器。这是以防错误从某内层
			 * 块的异常处理器抛出。
			 */
			estate->cur_error = save_cur_error;

			/* 无匹配则重新抛出该错误 */
			if (e == NULL)
				ReThrowError(edata);

			/* 恢复 stmt_mcontext 栈并释放错误数据 */
			pop_stmt_mcontext(estate);
			MemoryContextReset(stmt_mcontext);
		}
		PG_END_TRY();

		Assert(save_cur_error == estate->cur_error);
	}
	else
	{
		/*
		 * 只执行该块体中的语句
		 */
		estate->err_text = NULL;

		rc = exec_stmts(estate, block->body);
	}

	estate->err_text = NULL;

	/*
	 * 处理返回码。这有意不同于
	 * LOOP_RC_PROCESSING()：CONTINUE 从不匹配块，EXIT 只在
	 * 标号匹配时才匹配块。
	 */
	switch (rc)
	{
		case PLPGSQL_RC_OK:
		case PLPGSQL_RC_RETURN:
		case PLPGSQL_RC_CONTINUE:
			return rc;

		case PLPGSQL_RC_EXIT:
			if (estate->exitlabel == NULL)
				return PLPGSQL_RC_EXIT;
			if (block->label == NULL)
				return PLPGSQL_RC_EXIT;
			if (strcmp(block->label, estate->exitlabel) != 0)
				return PLPGSQL_RC_EXIT;
			estate->exitlabel = NULL;
			return PLPGSQL_RC_OK;

		default:
			elog(ERROR, "unrecognized rc: %d", rc);
	}

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmts 遍历语句列表，
 *	只要它们的返回码为 OK
 * ----------
 */
static int
exec_stmts(PLpgSQL_execstate *estate, List *stmts)
{
	PLpgSQL_stmt *save_estmt = estate->err_stmt;
	ListCell   *s;

	if (stmts == NIL)
	{
		/*
		 * 即使没语句，也确保执行 CHECK_FOR_INTERRUPTS()。
		 * 这能防止紧循环卡住，例如当
		 * 存在一个空体的 LOOP 构造时。
		 */
		CHECK_FOR_INTERRUPTS();
		return PLPGSQL_RC_OK;
	}

	foreach(s, stmts)
	{
		PLpgSQL_stmt *stmt = (PLpgSQL_stmt *) lfirst(s);
		int			rc;

		estate->err_stmt = stmt;

		/* 让插件知道我们即将执行这条语句 */
		if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->stmt_beg)
			((*plpgsql_plugin_ptr)->stmt_beg) (estate, stmt);

		CHECK_FOR_INTERRUPTS();

		switch (stmt->cmd_type)
		{
			case PLPGSQL_STMT_BLOCK:
				rc = exec_stmt_block(estate, (PLpgSQL_stmt_block *) stmt);
				break;

			case PLPGSQL_STMT_ASSIGN:
				rc = exec_stmt_assign(estate, (PLpgSQL_stmt_assign *) stmt);
				break;

			case PLPGSQL_STMT_PERFORM:
				rc = exec_stmt_perform(estate, (PLpgSQL_stmt_perform *) stmt);
				break;

			case PLPGSQL_STMT_CALL:
				rc = exec_stmt_call(estate, (PLpgSQL_stmt_call *) stmt);
				break;

			case PLPGSQL_STMT_GETDIAG:
				rc = exec_stmt_getdiag(estate, (PLpgSQL_stmt_getdiag *) stmt);
				break;

			case PLPGSQL_STMT_IF:
				rc = exec_stmt_if(estate, (PLpgSQL_stmt_if *) stmt);
				break;

			case PLPGSQL_STMT_CASE:
				rc = exec_stmt_case(estate, (PLpgSQL_stmt_case *) stmt);
				break;

			case PLPGSQL_STMT_LOOP:
				rc = exec_stmt_loop(estate, (PLpgSQL_stmt_loop *) stmt);
				break;

			case PLPGSQL_STMT_WHILE:
				rc = exec_stmt_while(estate, (PLpgSQL_stmt_while *) stmt);
				break;

			case PLPGSQL_STMT_FORI:
				rc = exec_stmt_fori(estate, (PLpgSQL_stmt_fori *) stmt);
				break;

			case PLPGSQL_STMT_FORS:
				rc = exec_stmt_fors(estate, (PLpgSQL_stmt_fors *) stmt);
				break;

			case PLPGSQL_STMT_FORC:
				rc = exec_stmt_forc(estate, (PLpgSQL_stmt_forc *) stmt);
				break;

			case PLPGSQL_STMT_FOREACH_A:
				rc = exec_stmt_foreach_a(estate, (PLpgSQL_stmt_foreach_a *) stmt);
				break;

			case PLPGSQL_STMT_EXIT:
				rc = exec_stmt_exit(estate, (PLpgSQL_stmt_exit *) stmt);
				break;

			case PLPGSQL_STMT_RETURN:
				rc = exec_stmt_return(estate, (PLpgSQL_stmt_return *) stmt);
				break;

			case PLPGSQL_STMT_RETURN_NEXT:
				rc = exec_stmt_return_next(estate, (PLpgSQL_stmt_return_next *) stmt);
				break;

			case PLPGSQL_STMT_RETURN_QUERY:
				rc = exec_stmt_return_query(estate, (PLpgSQL_stmt_return_query *) stmt);
				break;

			case PLPGSQL_STMT_RAISE:
				rc = exec_stmt_raise(estate, (PLpgSQL_stmt_raise *) stmt);
				break;

			case PLPGSQL_STMT_ASSERT:
				rc = exec_stmt_assert(estate, (PLpgSQL_stmt_assert *) stmt);
				break;

			case PLPGSQL_STMT_EXECSQL:
				rc = exec_stmt_execsql(estate, (PLpgSQL_stmt_execsql *) stmt);
				break;

			case PLPGSQL_STMT_DYNEXECUTE:
				rc = exec_stmt_dynexecute(estate, (PLpgSQL_stmt_dynexecute *) stmt);
				break;

			case PLPGSQL_STMT_DYNFORS:
				rc = exec_stmt_dynfors(estate, (PLpgSQL_stmt_dynfors *) stmt);
				break;

			case PLPGSQL_STMT_OPEN:
				rc = exec_stmt_open(estate, (PLpgSQL_stmt_open *) stmt);
				break;

			case PLPGSQL_STMT_FETCH:
				rc = exec_stmt_fetch(estate, (PLpgSQL_stmt_fetch *) stmt);
				break;

			case PLPGSQL_STMT_CLOSE:
				rc = exec_stmt_close(estate, (PLpgSQL_stmt_close *) stmt);
				break;

			case PLPGSQL_STMT_COMMIT:
				rc = exec_stmt_commit(estate, (PLpgSQL_stmt_commit *) stmt);
				break;

			case PLPGSQL_STMT_ROLLBACK:
				rc = exec_stmt_rollback(estate, (PLpgSQL_stmt_rollback *) stmt);
				break;

			default:
				/* 把 err_stmt 指向父级，因本条似乎已损坏 */
				estate->err_stmt = save_estmt;
				elog(ERROR, "unrecognized cmd_type: %d", stmt->cmd_type);
				rc = -1;		/* 让编译器安静 */
		}

		/* 让插件知道这条语句已经执行完 */
		if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->stmt_end)
			((*plpgsql_plugin_ptr)->stmt_end) (estate, stmt);

		if (rc != PLPGSQL_RC_OK)
		{
			estate->err_stmt = save_estmt;
			return rc;
		}
	}							/* 语句循环结束 */

	estate->err_stmt = save_estmt;
	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_assign 对表达式求值并
 *	把结果放进一个变量。
 * ----------
 */
static int
exec_stmt_assign(PLpgSQL_execstate *estate, PLpgSQL_stmt_assign *stmt)
{
	Assert(stmt->varno >= 0);

	exec_assign_expr(estate, estate->datums[stmt->varno], stmt->expr);

	return PLPGSQL_RC_OK;
}

/* ----------
 * exec_stmt_perform 对查询求值并丢弃结果（但要
 *	设 FOUND，视是否至少返回一行
 *	而定）。
 * ----------
 */
static int
exec_stmt_perform(PLpgSQL_execstate *estate, PLpgSQL_stmt_perform *stmt)
{
	PLpgSQL_expr *expr = stmt->expr;

	(void) exec_run_select(estate, expr, 0, NULL);
	exec_set_found(estate, (estate->eval_processed != 0));
	exec_eval_cleanup(estate);

	return PLPGSQL_RC_OK;
}

/*
 * exec_stmt_call
 *
 * 注意：这同时用于 CALL 与 DO 语句。
 */
static int
exec_stmt_call(PLpgSQL_execstate *estate, PLpgSQL_stmt_call *stmt)
{
	PLpgSQL_expr *expr = stmt->expr;
	LocalTransactionId before_lxid;
	LocalTransactionId after_lxid;
	ParamListInfo paramLI;
	SPIExecuteOptions options;
	int			rc;

	/*
	 * 若我们还没有计划就做一个。
	 */
	if (expr->plan == NULL)
		exec_prepare_plan(estate, expr, 0);

	/*
	 * CALL 或 DO 绝不能是简单表达式。
	 */
	Assert(!expr->expr_simple_expr);

	/*
	 * 另外构造一个代表 plpgsql 变量的 DTYPE_ROW datum，
	 * 关联到过程的输出参数。然后就能用
	 * exec_move_row() 来完成赋值。
	 */
	if (stmt->is_call && stmt->target == NULL)
		stmt->target = make_callstmt_target(estate, expr);

	paramLI = setup_param_list(estate, expr);

	before_lxid = MyProc->lxid;

	/*
	 * 若我们有过程级 resowner，就为计划持有其引用计数。
	 * 这可避免引用计数泄漏的抱怨，如果被调用的
	 * 过程结束当前事务。
	 *
	 * 另外，告诉 SPI 允许非原子执行。
	 */
	memset(&options, 0, sizeof(options));
	options.params = paramLI;
	options.read_only = estate->readonly_func;
	options.allow_nonatomic = true;
	options.owner = estate->procedure_resowner;

	rc = SPI_execute_plan_extended(expr->plan, &options);

	if (rc < 0)
		elog(ERROR, "SPI_execute_plan_extended failed executing query \"%s\": %s",
			 expr->query, SPI_result_code_string(rc));

	after_lxid = MyProc->lxid;

	if (before_lxid != after_lxid)
	{
		/*
		 * 若调用之后我们处于新事务中，就需要建立新的
		 * 简单表达式设施。
		 */
		estate->simple_eval_estate = NULL;
		estate->simple_eval_resowner = NULL;
		plpgsql_create_econtext(estate);
	}

	/*
	 * 检查结果行数；若有一行，就把过程的输出
	 * 值赋回相应的变量。
	 */
	if (SPI_processed == 1)
	{
		SPITupleTable *tuptab = SPI_tuptable;

		if (!stmt->is_call)
			elog(ERROR, "DO statement returned a row");

		exec_move_row(estate, stmt->target, tuptab->vals[0], tuptab->tupdesc);
	}
	else if (SPI_processed > 1)
		elog(ERROR, "procedure call returned more than one row");

	exec_eval_cleanup(estate);
	SPI_freetuptable(SPI_tuptable);

	return PLPGSQL_RC_OK;
}

/*
 * 我们构造一个代表 plpgsql 变量的 DTYPE_ROW datum，
 * 关联到过程的输出参数。然后就能用
 * exec_move_row() 来完成赋值。
 */
static PLpgSQL_variable *
make_callstmt_target(PLpgSQL_execstate *estate, PLpgSQL_expr *expr)
{
	CachedPlan *cplan;
	PlannedStmt *pstmt;
	CallStmt   *stmt;
	FuncExpr   *funcexpr;
	HeapTuple	func_tuple;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	int			numargs;
	MemoryContext oldcontext;
	PLpgSQL_row *row;
	int			nfields;
	int			i;

	/* 这里积下的杂物用 eval_mcontext */
	oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));

	/*
	 * 取已解析的 CallStmt，并查找被调过程。我们用
	 * SPI_plan_get_cached_plan 来覆盖边缘情况：expr->plan 已
	 * 过期而需要更新。
	 */
	cplan = SPI_plan_get_cached_plan(expr->plan);
	if (cplan == NULL || list_length(cplan->stmt_list) != 1)
		elog(ERROR, "query for CALL statement is not a CallStmt");
	pstmt = linitial_node(PlannedStmt, cplan->stmt_list);
	stmt = (CallStmt *) pstmt->utilityStmt;
	if (stmt == NULL || !IsA(stmt, CallStmt))
		elog(ERROR, "query for CALL statement is not a CallStmt");

	funcexpr = stmt->funcexpr;

	func_tuple = SearchSysCache1(PROCOID,
								 ObjectIdGetDatum(funcexpr->funcid));
	if (!HeapTupleIsValid(func_tuple))
		elog(ERROR, "cache lookup failed for function %u",
			 funcexpr->funcid);

	/*
	 * 取参数名与模式，以便出错时我们能给出切中要害的
	 * 错误消息。
	 */
	numargs = get_func_arg_info(func_tuple, &argtypes, &argnames, &argmodes);

	ReleaseSysCache(func_tuple);

	/*
	 * 开始构造行 Datum；把它留在 fn_cxt，才够
	 * 长命。
	 */
	MemoryContextSwitchTo(estate->func->fn_cxt);

	row = (PLpgSQL_row *) palloc0(sizeof(PLpgSQL_row));
	row->dtype = PLPGSQL_DTYPE_ROW;
	row->refname = "(unnamed row)";
	row->lineno = -1;
	row->varnos = (int *) palloc(numargs * sizeof(int));

	MemoryContextSwitchTo(get_eval_mcontext(estate));

	/*
	 * 检查过程的参数列表。每个输出参数位置都应是
	 * 一个不加修饰的 plpgsql 变量（Datum），可插入到该行
	 * Datum
	 */
	nfields = 0;
	for (i = 0; i < numargs; i++)
	{
		if (argmodes &&
			(argmodes[i] == PROARGMODE_INOUT ||
			 argmodes[i] == PROARGMODE_OUT))
		{
			Node	   *n = list_nth(stmt->outargs, nfields);

			if (IsA(n, Param))
			{
				Param	   *param = (Param *) n;
				int			dno;

				/* paramid 偏移了 1（见 make_datum_param()） */
				dno = param->paramid - 1;
				/* 此刻必须检查可赋值性，因语法无法 */
				exec_check_assignable(estate, dno);
				row->varnos[nfields++] = dno;
			}
			else
			{
				/* 若有参数名，就用它报告错误 */
				if (argnames && argnames[i] && argnames[i][0])
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("procedure parameter \"%s\" is an output parameter but corresponding argument is not writable",
									argnames[i])));
				else
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("procedure parameter %d is an output parameter but corresponding argument is not writable",
									i + 1)));
			}
		}
	}

	Assert(nfields == list_length(stmt->outargs));

	row->nfields = nfields;

	ReleaseCachedPlan(cplan, CurrentResourceOwner);

	MemoryContextSwitchTo(oldcontext);

	return (PLpgSQL_variable *) row;
}

/* ----------
 * exec_stmt_getdiag 把内部 PG 信息放入
 *	指定的变量。
 * ----------
 */
static int
exec_stmt_getdiag(PLpgSQL_execstate *estate, PLpgSQL_stmt_getdiag *stmt)
{
	ListCell   *lc;

	/*
	 * GET STACKED DIAGNOSTICS 只在异常处理器内有效。
	 *
	 * 注意：我们信任语法在非 is_stacked 时已禁用相关种类
	 * 否则下面就会核心转储。
	 */
	if (stmt->is_stacked && estate->cur_error == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_STACKED_DIAGNOSTICS_ACCESSED_WITHOUT_ACTIVE_HANDLER),
				 errmsg("GET STACKED DIAGNOSTICS cannot be used outside an exception handler")));

	foreach(lc, stmt->diag_items)
	{
		PLpgSQL_diag_item *diag_item = (PLpgSQL_diag_item *) lfirst(lc);
		PLpgSQL_datum *var = estate->datums[diag_item->target];

		switch (diag_item->kind)
		{
			case PLPGSQL_GETDIAG_ROW_COUNT:
				exec_assign_value(estate, var,
								  UInt64GetDatum(estate->eval_processed),
								  false, INT8OID, -1);
				break;

			case PLPGSQL_GETDIAG_ROUTINE_OID:
				exec_assign_value(estate, var,
								  ObjectIdGetDatum(estate->func->fn_oid),
								  false, OIDOID, -1);
				break;

			case PLPGSQL_GETDIAG_ERROR_CONTEXT:
				exec_assign_c_string(estate, var,
									 estate->cur_error->context);
				break;

			case PLPGSQL_GETDIAG_ERROR_DETAIL:
				exec_assign_c_string(estate, var,
									 estate->cur_error->detail);
				break;

			case PLPGSQL_GETDIAG_ERROR_HINT:
				exec_assign_c_string(estate, var,
									 estate->cur_error->hint);
				break;

			case PLPGSQL_GETDIAG_RETURNED_SQLSTATE:
				exec_assign_c_string(estate, var,
									 unpack_sql_state(estate->cur_error->sqlerrcode));
				break;

			case PLPGSQL_GETDIAG_COLUMN_NAME:
				exec_assign_c_string(estate, var,
									 estate->cur_error->column_name);
				break;

			case PLPGSQL_GETDIAG_CONSTRAINT_NAME:
				exec_assign_c_string(estate, var,
									 estate->cur_error->constraint_name);
				break;

			case PLPGSQL_GETDIAG_DATATYPE_NAME:
				exec_assign_c_string(estate, var,
									 estate->cur_error->datatype_name);
				break;

			case PLPGSQL_GETDIAG_MESSAGE_TEXT:
				exec_assign_c_string(estate, var,
									 estate->cur_error->message);
				break;

			case PLPGSQL_GETDIAG_TABLE_NAME:
				exec_assign_c_string(estate, var,
									 estate->cur_error->table_name);
				break;

			case PLPGSQL_GETDIAG_SCHEMA_NAME:
				exec_assign_c_string(estate, var,
									 estate->cur_error->schema_name);
				break;

			case PLPGSQL_GETDIAG_CONTEXT:
				{
					char	   *contextstackstr;
					MemoryContext oldcontext;

					/* 短命字符串用 eval_mcontext */
					oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
					contextstackstr = GetErrorContextStack();
					MemoryContextSwitchTo(oldcontext);

					exec_assign_c_string(estate, var, contextstackstr);
				}
				break;

			default:
				elog(ERROR, "unrecognized diagnostic item kind: %d",
					 diag_item->kind);
		}
	}

	exec_eval_cleanup(estate);

	return PLPGSQL_RC_OK;
}

/* ----------
 * exec_stmt_if 求值布尔表达式并
 *	按条件执行其真体或
 *	假体。
 * ----------
 */
static int
exec_stmt_if(PLpgSQL_execstate *estate, PLpgSQL_stmt_if *stmt)
{
	bool		value;
	bool		isnull;
	ListCell   *lc;

	value = exec_eval_boolean(estate, stmt->cond, &isnull);
	exec_eval_cleanup(estate);
	if (!isnull && value)
		return exec_stmts(estate, stmt->then_body);

	foreach(lc, stmt->elsif_list)
	{
		PLpgSQL_if_elsif *elif = (PLpgSQL_if_elsif *) lfirst(lc);

		value = exec_eval_boolean(estate, elif->cond, &isnull);
		exec_eval_cleanup(estate);
		if (!isnull && value)
			return exec_stmts(estate, elif->stmts);
	}

	return exec_stmts(estate, stmt->else_body);
}


/*-----------
 * exec_stmt_case
 *-----------
 */
static int
exec_stmt_case(PLpgSQL_execstate *estate, PLpgSQL_stmt_case *stmt)
{
	PLpgSQL_var *t_var = NULL;
	bool		isnull;
	ListCell   *l;

	if (stmt->t_expr != NULL)
	{
		/* 简单 CASE */
		Datum		t_val;
		Oid			t_typoid;
		int32		t_typmod;

		t_val = exec_eval_expr(estate, stmt->t_expr,
							   &isnull, &t_typoid, &t_typmod);

		t_var = (PLpgSQL_var *) estate->datums[stmt->t_varno];

		/*
		 * 当期望数据类型与实际不同时就改它。注意
		 * 我们这里改的是该 datum 的一份执行副本，故
		 * 这不影响原本存的函数语法树。（
		 * 理论上，若表达式数据类型在执行中不断改变，
		 * 这可能造成函数寿命级的内存泄漏。似乎
		 * 并不值得担心。）
		 */
		if (t_var->datatype->typoid != t_typoid ||
			t_var->datatype->atttypmod != t_typmod)
			t_var->datatype = plpgsql_build_datatype(t_typoid,
													 t_typmod,
													 estate->func->fn_input_collation,
													 NULL);

		/* 现在我们可赋给该变量 */
		exec_assign_value(estate,
						  (PLpgSQL_datum *) t_var,
						  t_val,
						  isnull,
						  t_typoid,
						  t_typmod);

		exec_eval_cleanup(estate);
	}

	/* 现在搜索成功的 WHEN 子句 */
	foreach(l, stmt->case_when_list)
	{
		PLpgSQL_case_when *cwt = (PLpgSQL_case_when *) lfirst(l);
		bool		value;

		value = exec_eval_boolean(estate, cwt->expr, &isnull);
		exec_eval_cleanup(estate);
		if (!isnull && value)
		{
			/* 得之 */

			/* 现在可以丢掉临时变量上原有的值 */
			if (t_var != NULL)
				assign_simple_var(estate, t_var, (Datum) 0, true, false);

			/* 求值这些语句，我们即完成 */
			return exec_stmts(estate, cwt->stmts);
		}
	}

	/* 现在可以丢掉临时变量上原有的值 */
	if (t_var != NULL)
		assign_simple_var(estate, t_var, (Datum) 0, true, false);

	/* 若没有 ELSE 子句，SQL2003 强制此错误 */
	if (!stmt->have_else)
		ereport(ERROR,
				(errcode(ERRCODE_CASE_NOT_FOUND),
				 errmsg("case not found"),
				 errhint("CASE statement is missing ELSE part.")));

	/* 求值 ELSE 语句，我们即完成 */
	return exec_stmts(estate, stmt->else_stmts);
}


/* ----------
 * exec_stmt_loop 循环语句直到
 *	出现退出。
 * ----------
 */
static int
exec_stmt_loop(PLpgSQL_execstate *estate, PLpgSQL_stmt_loop *stmt)
{
	int			rc = PLPGSQL_RC_OK;

	for (;;)
	{
		rc = exec_stmts(estate, stmt->body);

		LOOP_RC_PROCESSING(stmt->label, break);
	}

	return rc;
}


/* ----------
 * exec_stmt_while 循环语句，只要
 *	某表达式求值得
 *	真，或出现退出。
 * ----------
 */
static int
exec_stmt_while(PLpgSQL_execstate *estate, PLpgSQL_stmt_while *stmt)
{
	int			rc = PLPGSQL_RC_OK;

	for (;;)
	{
		bool		value;
		bool		isnull;

		value = exec_eval_boolean(estate, stmt->cond, &isnull);
		exec_eval_cleanup(estate);

		if (isnull || !value)
			break;

		rc = exec_stmts(estate, stmt->body);

		LOOP_RC_PROCESSING(stmt->label, break);
	}

	return rc;
}


/* ----------
 * exec_stmt_fori 迭代一个整数变量，
 *	从下限到上限，
 *	按 BY 值递增或递减
 * ----------
 */
static int
exec_stmt_fori(PLpgSQL_execstate *estate, PLpgSQL_stmt_fori *stmt)
{
	PLpgSQL_var *var;
	Datum		value;
	bool		isnull;
	Oid			valtype;
	int32		valtypmod;
	int32		loop_value;
	int32		end_value;
	int32		step_value;
	bool		found = false;
	int			rc = PLPGSQL_RC_OK;

	var = (PLpgSQL_var *) (estate->datums[stmt->var->dno]);

	/*
	 * 取下限的值
	 */
	value = exec_eval_expr(estate, stmt->lower,
						   &isnull, &valtype, &valtypmod);
	value = exec_cast_value(estate, value, &isnull,
							valtype, valtypmod,
							var->datatype->typoid,
							var->datatype->atttypmod);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("lower bound of FOR loop cannot be null")));
	loop_value = DatumGetInt32(value);
	exec_eval_cleanup(estate);

	/*
	 * 取上限的值
	 */
	value = exec_eval_expr(estate, stmt->upper,
						   &isnull, &valtype, &valtypmod);
	value = exec_cast_value(estate, value, &isnull,
							valtype, valtypmod,
							var->datatype->typoid,
							var->datatype->atttypmod);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("upper bound of FOR loop cannot be null")));
	end_value = DatumGetInt32(value);
	exec_eval_cleanup(estate);

	/*
	 * 取步长值
	 */
	if (stmt->step)
	{
		value = exec_eval_expr(estate, stmt->step,
							   &isnull, &valtype, &valtypmod);
		value = exec_cast_value(estate, value, &isnull,
								valtype, valtypmod,
								var->datatype->typoid,
								var->datatype->atttypmod);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("BY value of FOR loop cannot be null")));
		step_value = DatumGetInt32(value);
		exec_eval_cleanup(estate);
		if (step_value <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("BY value of FOR loop must be greater than zero")));
	}
	else
		step_value = 1;

	/*
	 * 现在做循环
	 */
	for (;;)
	{
		/*
		 * 对照上限检查
		 */
		if (stmt->reverse)
		{
			if (loop_value < end_value)
				break;
		}
		else
		{
			if (loop_value > end_value)
				break;
		}

		found = true;			/* 至少循环一次 */

		/*
		 * 把当前值赋给循环变量
		 */
		assign_simple_var(estate, var, Int32GetDatum(loop_value), false, false);

		/*
		 * 执行这些语句
		 */
		rc = exec_stmts(estate, stmt->body);

		LOOP_RC_PROCESSING(stmt->label, break);

		/*
		 * 增减循环值，除非会溢出，那种情况
		 * 则退出循环。
		 */
		if (stmt->reverse)
		{
			if (loop_value < (PG_INT32_MIN + step_value))
				break;
			loop_value -= step_value;
		}
		else
		{
			if (loop_value > (PG_INT32_MAX - step_value))
				break;
			loop_value += step_value;
		}
	}

	/*
	 * 设 FOUND 变量，标明执行该循环的结果，
	 * 即：是否至少循环过一次。必须在此设置
	 * 以免干扰 FOUND 变量的值，于
	 * 循环处理本身内。
	 */
	exec_set_found(estate, found);

	return rc;
}


/* ----------
 * exec_stmt_fors 执行查询，把每个
 *	元组赋给记录或行并
 *	执行一组语句
 *	为它。
 * ----------
 */
static int
exec_stmt_fors(PLpgSQL_execstate *estate, PLpgSQL_stmt_fors *stmt)
{
	Portal		portal;
	int			rc;

	/*
	 * 用 exec_run_select 为该语句打开隐式游标
	 */
	exec_run_select(estate, stmt->query, 0, &portal);

	/*
	 * 执行该循环
	 */
	rc = exec_for_query(estate, (PLpgSQL_stmt_forq *) stmt, portal, true);

	/*
	 * 关闭隐式游标
	 */
	SPI_cursor_close(portal);

	return rc;
}


/* ----------
 * exec_stmt_forc 对游标的每一行执行循环。
 * ----------
 */
static int
exec_stmt_forc(PLpgSQL_execstate *estate, PLpgSQL_stmt_forc *stmt)
{
	PLpgSQL_var *curvar;
	MemoryContext stmt_mcontext = NULL;
	char	   *curname = NULL;
	PLpgSQL_expr *query;
	ParamListInfo paramLI;
	Portal		portal;
	int			rc;

	/* ----------
	 * 取游标变量，若已有名字就检查
	 * 它当前并未在用。
	 * ----------
	 */
	curvar = (PLpgSQL_var *) (estate->datums[stmt->curvar]);
	if (!curvar->isnull)
	{
		MemoryContext oldcontext;

		/* 只需 stmt_mcontext 留住游标名字符串 */
		stmt_mcontext = get_stmt_mcontext(estate);
		oldcontext = MemoryContextSwitchTo(stmt_mcontext);
		curname = TextDatumGetCString(curvar->value);
		MemoryContextSwitchTo(oldcontext);

		if (SPI_cursor_find(curname) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_CURSOR),
					 errmsg("cursor \"%s\" already in use", curname)));
	}

	/* ----------
	 * 像 OPEN 命令那样打开游标。
	 *
	 * 注意：解析器本该已经检查过该语句提供了
	 * 仅当游标需要才给参，安全起见再查一次。
	 * ----------
	 */
	if (stmt->argquery != NULL)
	{
		/* ----------
		 * 带参 OPEN CURSOR。假造 SELECT ... INTO
		 * 语句，以求值参数并放入
		 * 内部行。
		 * ----------
		 */
		PLpgSQL_stmt_execsql set_args;

		if (curvar->cursor_explicit_argrow < 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("arguments given for cursor without arguments")));

		memset(&set_args, 0, sizeof(set_args));
		set_args.cmd_type = PLPGSQL_STMT_EXECSQL;
		set_args.lineno = stmt->lineno;
		set_args.sqlstmt = stmt->argquery;
		set_args.into = true;
		/* XXX 历来这里不是 STRICT */
		set_args.target = (PLpgSQL_variable *)
			(estate->datums[curvar->cursor_explicit_argrow]);

		if (exec_stmt_execsql(estate, &set_args) != PLPGSQL_RC_OK)
			elog(ERROR, "open cursor failed during argument processing");
	}
	else
	{
		if (curvar->cursor_explicit_argrow >= 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("arguments required for cursor")));
	}

	query = curvar->cursor_explicit_expr;
	Assert(query);

	if (query->plan == NULL)
		exec_prepare_plan(estate, query, curvar->cursor_options);

	/*
	 * 为该查询建 ParamListInfo
	 */
	paramLI = setup_param_list(estate, query);

	/*
	 * 打开游标（参数列表会拷进门户）
	 */
	portal = SPI_cursor_open_with_paramlist(curname, query->plan,
											paramLI,
											estate->readonly_func);
	if (portal == NULL)
		elog(ERROR, "could not open cursor: %s",
			 SPI_result_code_string(SPI_result));

	/*
	 * 若游标变量为 NULL，就把生成的门户名存入，
	 * 先确认可以赋值给它。
	 */
	if (curname == NULL)
	{
		exec_check_assignable(estate, stmt->curvar);
		assign_text_var(estate, curvar, portal->name);
	}

	/*
	 * 进入 exec_for_query 之前先清理
	 */
	exec_eval_cleanup(estate);
	if (stmt_mcontext)
		MemoryContextReset(stmt_mcontext);

	/*
	 * 执行该循环。我们无法预取，因用户能访问游标，
	 * 例如在循环里用 UPDATE WHERE CURRENT OF。
	 */
	rc = exec_for_query(estate, (PLpgSQL_stmt_forq *) stmt, portal, false);

	/* ----------
	 * 关闭门户，若游标变量最初为 NULL 就把它恢复。
	 * ----------
	 */
	SPI_cursor_close(portal);

	if (curname == NULL)
		assign_simple_var(estate, curvar, (Datum) 0, true, false);

	return rc;
}


/* ----------
 * exec_stmt_foreach_a 循环数组的元素或切片。
 *
 * 遍历元素时，循环变量与数组所存类型相同。
 * 例如 integer。遍历切片时，循环变量
 * 是数组，其尺寸与维数要匹配切片的大小。
 * ----------
 */
static int
exec_stmt_foreach_a(PLpgSQL_execstate *estate, PLpgSQL_stmt_foreach_a *stmt)
{
	ArrayType  *arr;
	Oid			arrtype;
	int32		arrtypmod;
	PLpgSQL_datum *loop_var;
	Oid			loop_var_elem_type;
	bool		found = false;
	int			rc = PLPGSQL_RC_OK;
	MemoryContext stmt_mcontext;
	MemoryContext oldcontext;
	ArrayIterator array_iterator;
	Oid			iterator_result_type;
	int32		iterator_result_typmod;
	Datum		value;
	bool		isnull;

	/* 取数组表达式的值 */
	value = exec_eval_expr(estate, stmt->expr, &isnull, &arrtype, &arrtypmod);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("FOREACH expression must not be null")));

	/*
	 * 下面的代码尽量在 stmt_mcontext 里做，以免任何
	 * 被调例程泄漏。我们需私有 stmt_mcontext，因为
	 * 我们会调用任意的语句代码。
	 */
	stmt_mcontext = get_stmt_mcontext(estate);
	push_stmt_mcontext(estate);
	oldcontext = MemoryContextSwitchTo(stmt_mcontext);

	/* 检查表达式的类型，必须是数组 */
	if (!OidIsValid(get_element_type(arrtype)))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("FOREACH expression must yield an array, not type %s",
						format_type_be(arrtype))));

	/*
	 * 我们必须把数组拷进 stmt_mcontext，否则它会在
	 * exec_eval_cleanup 里消失。这很讨厌，但清理肯定会在
	 * 跑循环体时发生，故我们几乎没选择。
	 */
	arr = DatumGetArrayTypePCopy(value);

	/* 清掉残留的临时内存 */
	exec_eval_cleanup(estate);

	/* 切片维数必须小于或等于数组维数 */
	if (stmt->slice < 0 || stmt->slice > ARR_NDIM(arr))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("slice dimension (%d) is out of the valid range 0..%d",
						stmt->slice, ARR_NDIM(arr))));

	/* 设好循环变量，并看它是否为数组类型 */
	loop_var = estate->datums[stmt->varno];
	if (loop_var->dtype == PLPGSQL_DTYPE_REC ||
		loop_var->dtype == PLPGSQL_DTYPE_ROW)
	{
		/*
		 * 记录或行变量肯定不是数组类型，且可能还
		 * 完全没初始化，故别试图取它的类型
		 */
		loop_var_elem_type = InvalidOid;
	}
	else
		loop_var_elem_type = get_element_type(plpgsql_exec_get_datum_type(estate,
																		  loop_var));

	/*
	 * 健全检查循环变量类型。我们这里不很较真，且
	 * 不该太挑剔，因为 exec_assign_value 也许能强制
	 * 不同类型的值。但若循环变量的数组性不对，
	 * 抱怨一番似乎值得。
	 */
	if (stmt->slice > 0 && loop_var_elem_type == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("FOREACH ... SLICE loop variable must be of an array type")));
	if (stmt->slice == 0 && loop_var_elem_type != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("FOREACH loop variable must not be of an array type")));

	/* 建迭代器来逐步走过该数组 */
	array_iterator = array_create_iterator(arr, stmt->slice, NULL);

	/* 认出迭代器结果类型 */
	if (stmt->slice > 0)
	{
		/* 切片时，结果的名义类型与数组类型相同 */
		iterator_result_type = arrtype;
		iterator_result_typmod = arrtypmod;
	}
	else
	{
		/* 不切片时，结果是单独的数组元素 */
		iterator_result_type = ARR_ELEMTYPE(arr);
		iterator_result_typmod = arrtypmod;
	}

	/* 遍历数组的元素或切片 */
	while (array_iterate(array_iterator, &value, &isnull))
	{
		found = true;			/* 至少循环一次 */

		/* exec_assign_value 与 exec_stmts 必须在主上下文运行 */
		MemoryContextSwitchTo(oldcontext);

		/* 把当前元素或切片赋给循环变量 */
		exec_assign_value(estate, loop_var, value, isnull,
						  iterator_result_type, iterator_result_typmod);

		/* 切片情形下值是临时的，必须释放以免泄漏 */
		if (stmt->slice > 0)
			pfree(DatumGetPointer(value));

		/*
		 * 执行这些语句
		 */
		rc = exec_stmts(estate, stmt->body);

		LOOP_RC_PROCESSING(stmt->label, break);

		MemoryContextSwitchTo(stmt_mcontext);
	}

	/* 恢复内存上下文状态 */
	MemoryContextSwitchTo(oldcontext);
	pop_stmt_mcontext(estate);

	/* 释放临时内存，包括该数组值 */
	MemoryContextReset(stmt_mcontext);

	/*
	 * 设 FOUND 变量，标明执行该循环的结果，
	 * 即：是否至少循环过一次。必须在此设置
	 * 以免干扰 FOUND 变量的值，于
	 * 循环处理本身内。
	 */
	exec_set_found(estate, found);

	return rc;
}


/* ----------
 * exec_stmt_exit 实现 EXIT 与 CONTINUE
 *
 * 这开始退出或重启某循环的过程。
 * ----------
 */
static int
exec_stmt_exit(PLpgSQL_execstate *estate, PLpgSQL_stmt_exit *stmt)
{
	/*
	 * 若退出或继续带有条件，就求值它
	 */
	if (stmt->cond != NULL)
	{
		bool		value;
		bool		isnull;

		value = exec_eval_boolean(estate, stmt->cond, &isnull);
		exec_eval_cleanup(estate);
		if (isnull || value == false)
			return PLPGSQL_RC_OK;
	}

	estate->exitlabel = stmt->label;
	if (stmt->is_exit)
		return PLPGSQL_RC_EXIT;
	else
		return PLPGSQL_RC_CONTINUE;
}


/* ----------
 * exec_stmt_return 对表达式求值并开始
 *	从该函数返回。
 *
 * 注意：结果可能在 eval_mcontext 里。因此，我们绝不可
 * 在展开控制栈时做 exec_eval_cleanup。
 * ----------
 */
static int
exec_stmt_return(PLpgSQL_execstate *estate, PLpgSQL_stmt_return *stmt)
{
	/*
	 * 若在处理返回集合的 PL/pgSQL 函数，最后的 RETURN
	 * 表明该函数已结束产生元组。其余工作
	 * 将在顶层完成。
	 */
	if (estate->retisset)
		return PLPGSQL_RC_RETURN;

	/* 为 NULL 结果初始化 */
	estate->retval = (Datum) 0;
	estate->retisnull = true;
	estate->rettype = InvalidOid;

	/*
	 * 特例路径：当 RETURN 表达式是简单变量
	 * 引用；函数里此路径必经，只要含有
	 * 若干 OUT 参数。
	 *
	 * 此特例对返回具有 R/W 扩展值的变量特别高效：
	 * 我们可把 R/W 指针直接放进 estate->retval，
	 * 从而廉价地把值转到调用者上下文。若走
	 * exec_eval_expr，结果会是一个
	 * R/O 指针。这里可以跳过 MakeExpandedObjectReadOnly，因为
	 * 我们知道函数内再也不需要该变量的值了。
	 */
	if (stmt->retvarno >= 0)
	{
		PLpgSQL_datum *retvar = estate->datums[stmt->retvarno];

		switch (retvar->dtype)
		{
			case PLPGSQL_DTYPE_PROMISE:
				/* 需要时兑现承诺，再当普通变量处理 */
				plpgsql_fulfill_promise(estate, (PLpgSQL_var *) retvar);

				/* FALL THRU */

			case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) retvar;

					estate->retval = var->value;
					estate->retisnull = var->isnull;
					estate->rettype = var->datatype->typoid;

					/*
					 * PLpgSQL_var 不可能是复合类型，故
					 * 若 retistuple，转换必失败。我们抛出
					 * 定制错误，主要为了符合历史行为。
					 * 同一原因：结果为 NULL 时我们不报错。（
					 * 注意 plpgsql_exec_trigger 假定任何
					 * 非空结果都已核实为复合的。）
					 */
					if (estate->retistuple && !estate->retisnull)
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("cannot return non-composite value from function returning composite type")));
				}
				break;

			case PLPGSQL_DTYPE_REC:
				{
					PLpgSQL_rec *rec = (PLpgSQL_rec *) retvar;

					/* 记录若空，我们返回 NULL 而非一行空值 */
					if (rec->erh && !ExpandedRecordIsEmpty(rec->erh))
					{
						estate->retval = ExpandedRecordGetDatum(rec->erh);
						estate->retisnull = false;
						estate->rettype = rec->rectypeid;
					}
				}
				break;

			case PLPGSQL_DTYPE_ROW:
				{
					PLpgSQL_row *row = (PLpgSQL_row *) retvar;
					int32		rettypmod;

					/* 多个 OUT 参数时走到这里 */
					exec_eval_datum(estate,
									(PLpgSQL_datum *) row,
									&estate->rettype,
									&rettypmod,
									&estate->retval,
									&estate->retisnull);
				}
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", retvar->dtype);
		}

		return PLPGSQL_RC_RETURN;
	}

	if (stmt->expr != NULL)
	{
		int32		rettypmod;

		estate->retval = exec_eval_expr(estate, stmt->expr,
										&(estate->retisnull),
										&(estate->rettype),
										&rettypmod);

		/*
		 * 如上面对 DTYPE_VAR 的情形，若返回了非空、
		 * 非复合值，而函数要返回元组，就抛定制错。
		 */
		if (estate->retistuple && !estate->retisnull &&
			!type_is_rowtype(estate->rettype))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot return non-composite value from function returning composite type")));

		return PLPGSQL_RC_RETURN;
	}

	/*
	 * VOID 函数的特殊权宜：不返回 NULL，改返回一个
	 * 非空 VOID 值。其重要性可疑，但仍保留，为了
	 * 向后兼容。不过对过程我们不这么做。
	 */
	if (estate->fn_rettype == VOIDOID &&
		estate->func->fn_prokind != PROKIND_PROCEDURE)
	{
		estate->retval = (Datum) 0;
		estate->retisnull = false;
		estate->rettype = VOIDOID;
	}

	return PLPGSQL_RC_RETURN;
}

/* ----------
 * exec_stmt_return_next 对表达式求值并把它加入
 *	当前返回的元组列表，来自
 *	SRF。
 * ----------
 */
static int
exec_stmt_return_next(PLpgSQL_execstate *estate,
					  PLpgSQL_stmt_return_next *stmt)
{
	TupleDesc	tupdesc;
	int			natts;
	HeapTuple	tuple;
	MemoryContext oldcontext;

	if (!estate->retisset)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use RETURN NEXT in a non-SETOF function")));

	if (estate->tuple_store == NULL)
		exec_init_tuple_store(estate);

	/* tuple_store_desc 将由 exec_init_tuple_store 填好 */
	tupdesc = estate->tuple_store_desc;
	natts = tupdesc->natts;

	/*
	 * 特例路径：当 RETURN NEXT 表达式是简单变量
	 * 引用；函数里此路径必经，只要含有
	 * 若干 OUT 参数。
	 *
	 * 与 exec_stmt_return 不同，这里对 R/W 扩展
	 * 值并无特别好处，因它们必须压扁才进 tuplestore。
	 * 其实我们最好把它们改成 R/O，以免转换步有风险
	 * 就地改掉它们。
	 */
	if (stmt->retvarno >= 0)
	{
		PLpgSQL_datum *retvar = estate->datums[stmt->retvarno];

		switch (retvar->dtype)
		{
			case PLPGSQL_DTYPE_PROMISE:
				/* 需要时兑现承诺，再当普通变量处理 */
				plpgsql_fulfill_promise(estate, (PLpgSQL_var *) retvar);

				/* FALL THRU */

			case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) retvar;
					Datum		retval = var->value;
					bool		isNull = var->isnull;
					Form_pg_attribute attr = TupleDescAttr(tupdesc, 0);

					if (natts != 1)
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("wrong result type supplied in RETURN NEXT")));

					/* 对这次转换我们非常多疑 */
					retval = MakeExpandedObjectReadOnly(retval,
														isNull,
														var->datatype->typlen);

					/* 需要时强制类型 */
					retval = exec_cast_value(estate,
											 retval,
											 &isNull,
											 var->datatype->typoid,
											 var->datatype->atttypmod,
											 attr->atttypid,
											 attr->atttypmod);

					tuplestore_putvalues(estate->tuple_store, tupdesc,
										 &retval, &isNull);
				}
				break;

			case PLPGSQL_DTYPE_REC:
				{
					PLpgSQL_rec *rec = (PLpgSQL_rec *) retvar;
					TupleDesc	rec_tupdesc;
					TupleConversionMap *tupmap;

					/* 若 rec 为 NULL，试把它转成一行空值 */
					if (rec->erh == NULL)
						instantiate_empty_record_variable(estate, rec);
					if (ExpandedRecordIsEmpty(rec->erh))
						deconstruct_expanded_record(rec->erh);

					/* 元组转换用 eval_mcontext */
					oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
					rec_tupdesc = expanded_record_get_tupdesc(rec->erh);
					tupmap = convert_tuples_by_position(rec_tupdesc,
														tupdesc,
														gettext_noop("wrong record type supplied in RETURN NEXT"));
					tuple = expanded_record_get_tuple(rec->erh);
					if (tupmap)
						tuple = execute_attr_map_tuple(tuple, tupmap);
					tuplestore_puttuple(estate->tuple_store, tuple);
					MemoryContextSwitchTo(oldcontext);
				}
				break;

			case PLPGSQL_DTYPE_ROW:
				{
					PLpgSQL_row *row = (PLpgSQL_row *) retvar;

					/* 多个 OUT 参数时走到这里 */

					/* 元组转换用 eval_mcontext */
					oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
					tuple = make_tuple_from_row(estate, row, tupdesc);
					if (tuple == NULL)	/* 不该发生 */
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("wrong record type supplied in RETURN NEXT")));
					tuplestore_puttuple(estate->tuple_store, tuple);
					MemoryContextSwitchTo(oldcontext);
				}
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", retvar->dtype);
				break;
		}
	}
	else if (stmt->expr)
	{
		Datum		retval;
		bool		isNull;
		Oid			rettype;
		int32		rettypmod;

		retval = exec_eval_expr(estate,
								stmt->expr,
								&isNull,
								&rettype,
								&rettypmod);

		if (estate->retistuple)
		{
			/* 表达式应为 RECORD 或复合类型 */
			if (!isNull)
			{
				HeapTupleData tmptup;
				TupleDesc	retvaldesc;
				TupleConversionMap *tupmap;

				if (!type_is_rowtype(rettype))
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("cannot return non-composite value from function returning composite type")));

				/* 元组转换用 eval_mcontext */
				oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
				retvaldesc = deconstruct_composite_datum(retval, &tmptup);
				tuple = &tmptup;
				tupmap = convert_tuples_by_position(retvaldesc, tupdesc,
													gettext_noop("returned record type does not match expected record type"));
				if (tupmap)
					tuple = execute_attr_map_tuple(tuple, tupmap);
				tuplestore_puttuple(estate->tuple_store, tuple);
				ReleaseTupleDesc(retvaldesc);
				MemoryContextSwitchTo(oldcontext);
			}
			else
			{
				/* 复合 NULL，存一行空值 */
				Datum	   *nulldatums;
				bool	   *nullflags;

				nulldatums = (Datum *)
					eval_mcontext_alloc0(estate, natts * sizeof(Datum));
				nullflags = (bool *)
					eval_mcontext_alloc(estate, natts * sizeof(bool));
				memset(nullflags, true, natts * sizeof(bool));
				tuplestore_putvalues(estate->tuple_store, tupdesc,
									 nulldatums, nullflags);
			}
		}
		else
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, 0);

			/* 简单标量结果 */
			if (natts != 1)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("wrong result type supplied in RETURN NEXT")));

			/* 需要时强制类型 */
			retval = exec_cast_value(estate,
									 retval,
									 &isNull,
									 rettype,
									 rettypmod,
									 attr->atttypid,
									 attr->atttypmod);

			tuplestore_putvalues(estate->tuple_store, tupdesc,
								 &retval, &isNull);
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("RETURN NEXT must have a parameter")));
	}

	exec_eval_cleanup(estate);

	return PLPGSQL_RC_OK;
}

/* ----------
 * exec_stmt_return_query 求值查询并把它加入
 *	当前返回的元组列表，来自
 *	SRF。
 * ----------
 */
static int
exec_stmt_return_query(PLpgSQL_execstate *estate,
					   PLpgSQL_stmt_return_query *stmt)
{
	int64		tcount;
	DestReceiver *treceiver;
	int			rc;
	uint64		processed;
	MemoryContext stmt_mcontext = get_stmt_mcontext(estate);
	MemoryContext oldcontext;

	if (!estate->retisset)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use RETURN QUERY in a non-SETOF function")));

	if (estate->tuple_store == NULL)
		exec_init_tuple_store(estate);
	/* tuplestore 里可能已经有一些元组 */
	tcount = tuplestore_tuple_count(estate->tuple_store);

	/*
	 * 设好 DestReceiver，把结果直接转入 tuplestore，
	 * 必要时转行类型。DestReceiver 住在内存上下文里。
	 */
	oldcontext = MemoryContextSwitchTo(stmt_mcontext);
	treceiver = CreateDestReceiver(DestTuplestore);
	SetTuplestoreDestReceiverParams(treceiver,
									estate->tuple_store,
									estate->tuple_store_cxt,
									false,
									estate->tuple_store_desc,
									gettext_noop("structure of query does not match function result type"));
	MemoryContextSwitchTo(oldcontext);

	if (stmt->query != NULL)
	{
		/* 静态查询 */
		PLpgSQL_expr *expr = stmt->query;
		ParamListInfo paramLI;
		SPIExecuteOptions options;

		/*
		 * 该表达式第一次调用时生成计划。
		 */
		if (expr->plan == NULL)
			exec_prepare_plan(estate, expr, CURSOR_OPT_PARALLEL_OK);

		/*
		 * 建 ParamListInfo 传给执行器
		 */
		paramLI = setup_param_list(estate, expr);

		/*
		 * 执行该查询
		 */
		memset(&options, 0, sizeof(options));
		options.params = paramLI;
		options.read_only = estate->readonly_func;
		options.must_return_tuples = true;
		options.dest = treceiver;

		rc = SPI_execute_plan_extended(expr->plan, &options);
		if (rc < 0)
			elog(ERROR, "SPI_execute_plan_extended failed executing query \"%s\": %s",
				 expr->query, SPI_result_code_string(rc));
	}
	else
	{
		/* RETURN QUERY EXECUTE */
		Datum		query;
		bool		isnull;
		Oid			restype;
		int32		restypmod;
		char	   *querystr;
		SPIExecuteOptions options;

		/*
		 * 对 EXECUTE 关键字之后的字符串表达式求值。其
		 * 结果就是要执行的查询串。
		 */
		Assert(stmt->dynquery != NULL);
		query = exec_eval_expr(estate, stmt->dynquery,
							   &isnull, &restype, &restypmod);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("query string argument of EXECUTE is null")));

		/* 取 C 字符串表示 */
		querystr = convert_value_to_string(estate, query, restype);

		/* 清理前把它拷进 stmt_mcontext */
		querystr = MemoryContextStrdup(stmt_mcontext, querystr);

		exec_eval_cleanup(estate);

		/* 执行查询，必要时传入参数 */
		memset(&options, 0, sizeof(options));
		options.params = exec_eval_using_params(estate,
												stmt->params);
		options.read_only = estate->readonly_func;
		options.must_return_tuples = true;
		options.dest = treceiver;

		rc = SPI_execute_extended(querystr, &options);
		if (rc < 0)
			elog(ERROR, "SPI_execute_extended failed executing query \"%s\": %s",
				 querystr, SPI_result_code_string(rc));
	}

	/* 清理 */
	treceiver->rDestroy(treceiver);
	exec_eval_cleanup(estate);
	MemoryContextReset(stmt_mcontext);

	/* 数我们得到的元组 */
	processed = tuplestore_tuple_count(estate->tuple_store) - tcount;

	estate->eval_processed = processed;
	exec_set_found(estate, processed != 0);

	return PLPGSQL_RC_OK;
}

static void
exec_init_tuple_store(PLpgSQL_execstate *estate)
{
	ReturnSetInfo *rsi = estate->rsi;
	MemoryContext oldcxt;
	ResourceOwner oldowner;

	/*
	 * 查调用者能否如我们所要处理集合结果
	 */
	if (!rsi || !IsA(rsi, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	if (!(rsi->allowedModes & SFRM_Materialize) ||
		rsi->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/*
	 * 切换到正确的内存上下文与资源所有者，以便存放
	 * 返回集合的 tuplestore。若我们处于为
	 * 异常块打开的子事务中（例如），我们仍必须
	 * 在进入本函数时活跃的资源所有者中建立它，
	 * 不在子事务资源所有者中。
	 */
	oldcxt = MemoryContextSwitchTo(estate->tuple_store_cxt);
	oldowner = CurrentResourceOwner;
	CurrentResourceOwner = estate->tuple_store_owner;

	estate->tuple_store =
		tuplestore_begin_heap(rsi->allowedModes & SFRM_Materialize_Random,
							  false, work_mem);

	CurrentResourceOwner = oldowner;
	MemoryContextSwitchTo(oldcxt);

	estate->tuple_store_desc = rsi->expectedDesc;
}

#define SET_RAISE_OPTION_TEXT(opt, name) \
do { \
	if (opt) \
		ereport(ERROR, \
				(errcode(ERRCODE_SYNTAX_ERROR), \
				 errmsg("RAISE option already specified: %s", \
						name))); \
	opt = MemoryContextStrdup(stmt_mcontext, extval); \
} while (0)

/* ----------
 * exec_stmt_raise 构造一条消息并用 elog() 抛出
 * ----------
 */
static int
exec_stmt_raise(PLpgSQL_execstate *estate, PLpgSQL_stmt_raise *stmt)
{
	int			err_code = 0;
	char	   *condname = NULL;
	char	   *err_message = NULL;
	char	   *err_detail = NULL;
	char	   *err_hint = NULL;
	char	   *err_column = NULL;
	char	   *err_constraint = NULL;
	char	   *err_datatype = NULL;
	char	   *err_table = NULL;
	char	   *err_schema = NULL;
	MemoryContext stmt_mcontext;
	ListCell   *lc;

	/* 无参数的 RAISE：重新抛出当前异常 */
	if (stmt->condname == NULL && stmt->message == NULL &&
		stmt->options == NIL)
	{
		if (estate->cur_error != NULL)
			ReThrowError(estate->cur_error);
		/* 我们并不在处理器内 */
		ereport(ERROR,
				(errcode(ERRCODE_STACKED_DIAGNOSTICS_ACCESSED_WITHOUT_ACTIVE_HANDLER),
				 errmsg("RAISE without parameters cannot be used outside an exception handler")));
	}

	/* 我们需要在 stmt_mcontext 里累积各个字符串 */
	stmt_mcontext = get_stmt_mcontext(estate);

	if (stmt->condname)
	{
		err_code = plpgsql_recognize_err_condition(stmt->condname, true);
		condname = MemoryContextStrdup(stmt_mcontext, stmt->condname);
	}

	if (stmt->message)
	{
		StringInfoData ds;
		ListCell   *current_param;
		char	   *cp;
		MemoryContext oldcontext;

		/* 在 stmt_mcontext 中建串 */
		oldcontext = MemoryContextSwitchTo(stmt_mcontext);
		initStringInfo(&ds);
		MemoryContextSwitchTo(oldcontext);

		current_param = list_head(stmt->params);

		for (cp = stmt->message; *cp; cp++)
		{
			/*
			 * 单个 % 的出现被换成下一个参数的
			 * 外部表示。两个 % 被转换成一个 %。
			 */
			if (cp[0] == '%')
			{
				Oid			paramtypeid;
				int32		paramtypmod;
				Datum		paramvalue;
				bool		paramisnull;
				char	   *extval;

				if (cp[1] == '%')
				{
					appendStringInfoChar(&ds, '%');
					cp++;
					continue;
				}

				/* 编译时就该检查过 */
				if (current_param == NULL)
					elog(ERROR, "unexpected RAISE parameter list length");

				paramvalue = exec_eval_expr(estate,
											(PLpgSQL_expr *) lfirst(current_param),
											&paramisnull,
											&paramtypeid,
											&paramtypmod);

				if (paramisnull)
					extval = "<NULL>";
				else
					extval = convert_value_to_string(estate,
													 paramvalue,
													 paramtypeid);
				appendStringInfoString(&ds, extval);
				current_param = lnext(stmt->params, current_param);
				exec_eval_cleanup(estate);
			}
			else
				appendStringInfoChar(&ds, cp[0]);
		}

		/* 编译时就该检查过 */
		if (current_param != NULL)
			elog(ERROR, "unexpected RAISE parameter list length");

		err_message = ds.data;
	}

	foreach(lc, stmt->options)
	{
		PLpgSQL_raise_option *opt = (PLpgSQL_raise_option *) lfirst(lc);
		Datum		optionvalue;
		bool		optionisnull;
		Oid			optiontypeid;
		int32		optiontypmod;
		char	   *extval;

		optionvalue = exec_eval_expr(estate, opt->expr,
									 &optionisnull,
									 &optiontypeid,
									 &optiontypmod);
		if (optionisnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("RAISE statement option cannot be null")));

		extval = convert_value_to_string(estate, optionvalue, optiontypeid);

		switch (opt->opt_type)
		{
			case PLPGSQL_RAISEOPTION_ERRCODE:
				if (err_code)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("RAISE option already specified: %s",
									"ERRCODE")));
				err_code = plpgsql_recognize_err_condition(extval, true);
				condname = MemoryContextStrdup(stmt_mcontext, extval);
				break;
			case PLPGSQL_RAISEOPTION_MESSAGE:
				SET_RAISE_OPTION_TEXT(err_message, "MESSAGE");
				break;
			case PLPGSQL_RAISEOPTION_DETAIL:
				SET_RAISE_OPTION_TEXT(err_detail, "DETAIL");
				break;
			case PLPGSQL_RAISEOPTION_HINT:
				SET_RAISE_OPTION_TEXT(err_hint, "HINT");
				break;
			case PLPGSQL_RAISEOPTION_COLUMN:
				SET_RAISE_OPTION_TEXT(err_column, "COLUMN");
				break;
			case PLPGSQL_RAISEOPTION_CONSTRAINT:
				SET_RAISE_OPTION_TEXT(err_constraint, "CONSTRAINT");
				break;
			case PLPGSQL_RAISEOPTION_DATATYPE:
				SET_RAISE_OPTION_TEXT(err_datatype, "DATATYPE");
				break;
			case PLPGSQL_RAISEOPTION_TABLE:
				SET_RAISE_OPTION_TEXT(err_table, "TABLE");
				break;
			case PLPGSQL_RAISEOPTION_SCHEMA:
				SET_RAISE_OPTION_TEXT(err_schema, "SCHEMA");
				break;
			default:
				elog(ERROR, "unrecognized raise option: %d", opt->opt_type);
		}

		exec_eval_cleanup(estate);
	}

	/* 未指定时用默认码 */
	if (err_code == 0 && stmt->elog_level >= ERROR)
		err_code = ERRCODE_RAISE_EXCEPTION;

	/* 未指定时的默认错误消息 */
	if (err_message == NULL)
	{
		if (condname)
		{
			err_message = condname;
			condname = NULL;
		}
		else
			err_message = MemoryContextStrdup(stmt_mcontext,
											  unpack_sql_state(err_code));
	}

	/*
	 * 抛错（可能回也可能不回）
	 */
	ereport(stmt->elog_level,
			(err_code ? errcode(err_code) : 0,
			 errmsg_internal("%s", err_message),
			 (err_detail != NULL) ? errdetail_internal("%s", err_detail) : 0,
			 (err_hint != NULL) ? errhint("%s", err_hint) : 0,
			 (err_column != NULL) ?
			 err_generic_string(PG_DIAG_COLUMN_NAME, err_column) : 0,
			 (err_constraint != NULL) ?
			 err_generic_string(PG_DIAG_CONSTRAINT_NAME, err_constraint) : 0,
			 (err_datatype != NULL) ?
			 err_generic_string(PG_DIAG_DATATYPE_NAME, err_datatype) : 0,
			 (err_table != NULL) ?
			 err_generic_string(PG_DIAG_TABLE_NAME, err_table) : 0,
			 (err_schema != NULL) ?
			 err_generic_string(PG_DIAG_SCHEMA_NAME, err_schema) : 0));

	/* 清理短命字符串 */
	MemoryContextReset(stmt_mcontext);

	return PLPGSQL_RC_OK;
}

/* ----------
 * exec_stmt_assert 断言语句
 * ----------
 */
static int
exec_stmt_assert(PLpgSQL_execstate *estate, PLpgSQL_stmt_assert *stmt)
{
	bool		value;
	bool		isnull;

	/* 断言未启用时什么都不做 */
	if (!plpgsql_check_asserts)
		return PLPGSQL_RC_OK;

	value = exec_eval_boolean(estate, stmt->cond, &isnull);
	exec_eval_cleanup(estate);

	if (isnull || !value)
	{
		char	   *message = NULL;

		if (stmt->message != NULL)
		{
			Datum		val;
			Oid			typeid;
			int32		typmod;

			val = exec_eval_expr(estate, stmt->message,
								 &isnull, &typeid, &typmod);
			if (!isnull)
				message = convert_value_to_string(estate, val, typeid);
			/* 我们此处禁做 exec_eval_cleanup */
		}

		ereport(ERROR,
				(errcode(ERRCODE_ASSERT_FAILURE),
				 message ? errmsg_internal("%s", message) :
				 errmsg("assertion failed")));
	}

	return PLPGSQL_RC_OK;
}

/* ----------
 * 初始化一个几乎空的执行态
 * ----------
 */
static void
plpgsql_estate_setup(PLpgSQL_execstate *estate,
					 PLpgSQL_function *func,
					 ReturnSetInfo *rsi,
					 EState *simple_eval_estate,
					 ResourceOwner simple_eval_resowner)
{
	HASHCTL		ctl;

	/* 此链接将在离开 plpgsql_call_handler 时恢复 */
	func->cur_estate = estate;

	estate->func = func;
	estate->trigdata = NULL;
	estate->evtrigdata = NULL;

	estate->retval = (Datum) 0;
	estate->retisnull = true;
	estate->rettype = InvalidOid;

	estate->fn_rettype = func->fn_rettype;
	estate->retistuple = func->fn_retistuple;
	estate->retisset = func->fn_retset;

	estate->readonly_func = func->fn_readonly;
	estate->atomic = true;

	estate->exitlabel = NULL;
	estate->cur_error = NULL;

	estate->tuple_store = NULL;
	estate->tuple_store_desc = NULL;
	if (rsi)
	{
		estate->tuple_store_cxt = rsi->econtext->ecxt_per_query_memory;
		estate->tuple_store_owner = CurrentResourceOwner;
	}
	else
	{
		estate->tuple_store_cxt = NULL;
		estate->tuple_store_owner = NULL;
	}
	estate->rsi = rsi;

	estate->found_varno = func->found_varno;
	estate->ndatums = func->ndatums;
	estate->datums = NULL;
	/* datums 数组将由 copy_plpgsql_datums() 填好 */
	estate->datum_context = CurrentMemoryContext;

	/* 用合适的钩子函数初始化我们的 ParamListInfo */
	estate->paramLI = makeParamList(0);
	estate->paramLI->paramFetch = plpgsql_param_fetch;
	estate->paramLI->paramFetchArg = (void *) estate;
	estate->paramLI->paramCompile = plpgsql_param_compile;
	estate->paramLI->paramCompileArg = NULL;	/* 不需要 */
	estate->paramLI->parserSetup = (ParserSetupHook) plpgsql_parser_setup;
	estate->paramLI->parserSetupArg = NULL; /* 使用中填入 */
	estate->paramLI->numParams = estate->ndatums;

	/* 若还没有，就建会话范围的转换表达式散列表 */
	if (cast_expr_hash == NULL)
	{
		ctl.keysize = sizeof(plpgsql_CastHashKey);
		ctl.entrysize = sizeof(plpgsql_CastExprHashEntry);
		cast_expr_hash = hash_create("PLpgSQL cast expressions",
									 16,	/* 先小再扩展 */
									 &ctl,
									 HASH_ELEM | HASH_BLOBS);
	}

	/* 设好以便使用合适的简单表达式 EState 与转换散列表 */
	if (simple_eval_estate)
	{
		estate->simple_eval_estate = simple_eval_estate;
		/* 私有转换散列只存在于函数的主上下文 */
		ctl.keysize = sizeof(plpgsql_CastHashKey);
		ctl.entrysize = sizeof(plpgsql_CastHashEntry);
		ctl.hcxt = CurrentMemoryContext;
		estate->cast_hash = hash_create("PLpgSQL private cast cache",
										16, /* 先小再扩展 */
										&ctl,
										HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	else
	{
		estate->simple_eval_estate = shared_simple_eval_estate;
		/* 若还没有，就建会话范围的转换信息散列表 */
		if (shared_cast_hash == NULL)
		{
			ctl.keysize = sizeof(plpgsql_CastHashKey);
			ctl.entrysize = sizeof(plpgsql_CastHashEntry);
			shared_cast_hash = hash_create("PLpgSQL cast cache",
										   16,	/* 先小再扩展 */
										   &ctl,
										   HASH_ELEM | HASH_BLOBS);
		}
		estate->cast_hash = shared_cast_hash;
	}
	/* 简单表达式资源所有者同样如此 */
	if (simple_eval_resowner)
		estate->simple_eval_resowner = simple_eval_resowner;
	else
		estate->simple_eval_resowner = shared_simple_eval_resowner;

	/* 若有过程的 resowner，稍后会填上它 */
	estate->procedure_resowner = NULL;

	/*
	 * 我们起初没有 stmt_mcontext；只在需要时才建一个。
	 * 该上下文将是函数主执行上下文的直接子级。
	 * 还可能创建更多 stmt_mcontexts，作为它的子级。
	 */
	estate->stmt_mcontext = NULL;
	estate->stmt_mcontext_parent = CurrentMemoryContext;

	estate->eval_tuptable = NULL;
	estate->eval_processed = 0;
	estate->eval_econtext = NULL;

	estate->err_stmt = NULL;
	estate->err_var = NULL;
	estate->err_text = NULL;

	estate->plugin_info = NULL;

	/*
	 * 建一个 EState 与 ExprContext，供简单表达式求值。
	 */
	plpgsql_create_econtext(estate);

	/*
	 * 有插装插件则让它先看函数，我们再初始化局部
	 * 的 PL/pgSQL 变量。注意我们也交给插件几个函数
	 * 指针，使它能回调 PL/pgSQL 去做诸如
	 * 变量赋值与栈回溯等。
	 */
	if (*plpgsql_plugin_ptr)
	{
		(*plpgsql_plugin_ptr)->error_callback = plpgsql_exec_error_callback;
		(*plpgsql_plugin_ptr)->assign_expr = exec_assign_expr;
		(*plpgsql_plugin_ptr)->assign_value = exec_assign_value;
		(*plpgsql_plugin_ptr)->eval_datum = exec_eval_datum;
		(*plpgsql_plugin_ptr)->cast_value = exec_cast_value;

		if ((*plpgsql_plugin_ptr)->func_setup)
			((*plpgsql_plugin_ptr)->func_setup) (estate, func);
	}
}

/* ----------
 * 释放表达式与子查询求值所用的临时内存。
 *
 * 注意：做完之后，求值结果就不再有效，
 * 除非它是按值传递类型。
 * ----------
 */
static void
exec_eval_cleanup(PLpgSQL_execstate *estate)
{
	/* 清掉完整 SPI_execute 的结果 */
	if (estate->eval_tuptable != NULL)
		SPI_freetuptable(estate->eval_tuptable);
	estate->eval_tuptable = NULL;

	/*
	 * 清 exec_eval_simple_expr 的结果（但保留 econtext）。这
	 * 也清掉经由 get_eval_mcontext 做的任何短命分配。
	 */
	if (estate->eval_econtext != NULL)
		ResetExprContext(estate->eval_econtext);
}


/* ----------
 * 生成预备计划。
 *
 * 警告：本函数有可能在已经
 * 建好 SPIPlan 并写入 expr->plan 后抛错。因此要当心做
 * 那些以 expr->plan 为 NULL 为前提的额外事。亦即给定
 * 代码：
 *
 *	若query->plan为NULL
 *	{
 *	// 此处可放准备代码
 *	exec_prepare_plan(estate, query, ...);
 *	// 此处不可再放更多逻辑
 *	}
 *
 * 末尾多出的步骤并不安全，因为当
 * 重跑调用语句时它们并不执行，如果 exec_prepare_plan 首次
 * 失败的话。这很易错且讨厌，但其他办法更差。
 * ----------
 */
static void
exec_prepare_plan(PLpgSQL_execstate *estate,
				  PLpgSQL_expr *expr, int cursorOptions)
{
	SPIPlanPtr	plan;
	SPIPrepareOptions options;

	/*
	 * 语法在建语法树时无法方便地设置 expr->func，
	 * 故要确保在解析钩子需要它前已设好。
	 */
	expr->func = estate->func;

	/*
	 * 生成并保存该计划
	 */
	memset(&options, 0, sizeof(options));
	options.parserSetup = (ParserSetupHook) plpgsql_parser_setup;
	options.parserSetupArg = (void *) expr;
	options.parseMode = expr->parseMode;
	options.cursorOptions = cursorOptions;
	plan = SPI_prepare_extended(expr->query, &options);
	if (plan == NULL)
		elog(ERROR, "SPI_prepare_extended failed for \"%s\": %s",
			 expr->query, SPI_result_code_string(SPI_result));

	SPI_keepplan(plan);
	expr->plan = plan;

	/* 检查它是不是简单表达式 */
	exec_simple_check_plan(estate, expr);
}


/* ----------
 * exec_stmt_execsql 执行一条 SQL 语句（可能带 INTO）。
 *
 * 注意：有些调用者依赖这里不碰 stmt_mcontext。若它将来
 * 要用它，就改那些调用者去压弹 stmt_mcontext。
 * ----------
 */
static int
exec_stmt_execsql(PLpgSQL_execstate *estate,
				  PLpgSQL_stmt_execsql *stmt)
{
	ParamListInfo paramLI;
	long		tcount;
	int			rc;
	PLpgSQL_expr *expr = stmt->sqlstmt;
	int			too_many_rows_level = 0;

	if (plpgsql_extra_errors & PLPGSQL_XCHECK_TOOMANYROWS)
		too_many_rows_level = ERROR;
	else if (plpgsql_extra_warnings & PLPGSQL_XCHECK_TOOMANYROWS)
		too_many_rows_level = WARNING;

	/*
	 * 该语句第一次被调用时生成计划，并检测
	 * 它是否为 INSERT/UPDATE/DELETE/MERGE
	 */
	if (expr->plan == NULL)
		exec_prepare_plan(estate, expr, CURSOR_OPT_PARALLEL_OK);

	if (!stmt->mod_stmt_set)
	{
		ListCell   *l;

		stmt->mod_stmt = false;
		foreach(l, SPI_plan_get_plan_sources(expr->plan))
		{
			CachedPlanSource *plansource = (CachedPlanSource *) lfirst(l);

			/*
			 * 我们本可看 raw_parse_tree，但查命令标签
			 * 似乎更简单。注意我们绝不去看 Query
			 * 树，因它们是重写结果，可能已过期，
			 * 也可能被改成别的东西，改得十分
			 * 彻底。
			 */
			if (plansource->commandTag == CMDTAG_INSERT ||
				plansource->commandTag == CMDTAG_UPDATE ||
				plansource->commandTag == CMDTAG_DELETE ||
				plansource->commandTag == CMDTAG_MERGE)
			{
				stmt->mod_stmt = true;
				break;
			}
		}
		stmt->mod_stmt_set = true;
	}

	/*
	 * 建 ParamListInfo 传给执行器
	 */
	paramLI = setup_param_list(estate, expr);

	/*
	 * 若有 INTO，则只需取回一行……但若有 INTO
	 * STRICT 还是额外的 too_many_rows 检查，就取两行以便
	 * 核实该语句只返回一行。INSERT/UPDATE/DELETE 总是
	 * 被严格对待。无 INTO 就把语句跑完（
	 * tcount=0)
	 *
	 * 用 INTO 时本可总是要两行，但有些情形
	 * 多要的那一行耗时很多，例如迫使一次
	 * 顺序扫描跑完。故除非我们需要强制严格，
	 * 否则不做。
	 */
	if (stmt->into)
	{
		if (stmt->strict || stmt->mod_stmt || too_many_rows_level)
			tcount = 2;
		else
			tcount = 1;
	}
	else
		tcount = 0;

	/*
	 * 执行该计划
	 */
	rc = SPI_execute_plan_with_paramlist(expr->plan, paramLI,
										 estate->readonly_func, tcount);

	/*
	 * 检查有无错误，并在适当时设 FOUND（出于历史原因
	 * 我们只对某些查询类型设 FOUND）。再断言我们
	 * 认定的语句类型与 SPI 认定的相同。
	 */
	switch (rc)
	{
		case SPI_OK_SELECT:
			Assert(!stmt->mod_stmt);
			exec_set_found(estate, (SPI_processed != 0));
			break;

		case SPI_OK_INSERT:
		case SPI_OK_UPDATE:
		case SPI_OK_DELETE:
		case SPI_OK_INSERT_RETURNING:
		case SPI_OK_UPDATE_RETURNING:
		case SPI_OK_DELETE_RETURNING:
		case SPI_OK_MERGE:
			Assert(stmt->mod_stmt);
			exec_set_found(estate, (SPI_processed != 0));
			break;

		case SPI_OK_SELINTO:
		case SPI_OK_UTILITY:
			Assert(!stmt->mod_stmt);
			break;

		case SPI_OK_REWRITTEN:

			/*
			 * 该命令被重写成另一种命令。此时
			 * FOUND 意味什么并不清楚（而且 SPI 也不
			 * 返回行数），故只把它设为假。注意
			 * 我们在此无法对 mod_stmt 断言任何事。
			 */
			exec_set_found(estate, false);
			break;

			/* 有些 SPI 错误该用专门的错误消息 */
		case SPI_ERROR_COPY:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot COPY to/from client in PL/pgSQL")));
			break;

		case SPI_ERROR_TRANSACTION:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported transaction command in PL/pgSQL")));
			break;

		default:
			elog(ERROR, "SPI_execute_plan_with_paramlist failed executing query \"%s\": %s",
				 expr->query, SPI_result_code_string(rc));
			break;
	}

	/* 所有变体都应为 GET DIAGNOSTICS 保存结果信息 */
	estate->eval_processed = SPI_processed;

	/* 若有 INTO 就处理 */
	if (stmt->into)
	{
		SPITupleTable *tuptab = SPI_tuptable;
		uint64		n = SPI_processed;
		PLpgSQL_variable *target;

		/* 语句若没返回元组表，就报错 */
		if (tuptab == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("INTO used with a command that cannot return data")));

		/* 取目标的 datum 项 */
		target = (PLpgSQL_variable *) estate->datums[stmt->target->dno];

		/*
		 * 若 SELECT ... INTO 指定了 STRICT，且查询没找到
		 * 恰好一行就报错。若未指定 STRICT，则
		 * 允许查询找到任意行数。
		 */
		if (n == 0)
		{
			if (stmt->strict)
			{
				char	   *errdetail;

				if (estate->func->print_strict_params)
					errdetail = format_expr_params(estate, expr);
				else
					errdetail = NULL;

				ereport(ERROR,
						(errcode(ERRCODE_NO_DATA_FOUND),
						 errmsg("query returned no rows"),
						 errdetail ? errdetail_internal("parameters: %s", errdetail) : 0));
			}
			/* 把目标设成 NULL */
			exec_move_row(estate, target, NULL, tuptab->tupdesc);
		}
		else
		{
			if (n > 1 && (stmt->strict || stmt->mod_stmt || too_many_rows_level))
			{
				char	   *errdetail;
				int			errlevel;

				if (estate->func->print_strict_params)
					errdetail = format_expr_params(estate, expr);
				else
					errdetail = NULL;

				errlevel = (stmt->strict || stmt->mod_stmt) ? ERROR : too_many_rows_level;

				ereport(errlevel,
						(errcode(ERRCODE_TOO_MANY_ROWS),
						 errmsg("query returned more than one row"),
						 errdetail ? errdetail_internal("parameters: %s", errdetail) : 0,
						 errhint("Make sure the query returns a single row, or use LIMIT 1.")));
			}
			/* 把第一行结果放进目标 */
			exec_move_row(estate, target, tuptab->vals[0], tuptab->tupdesc);
		}

		/* 清理 */
		exec_eval_cleanup(estate);
		SPI_freetuptable(SPI_tuptable);
	}
	else
	{
		/* 若该语句返回了元组表，就报错 */
		if (SPI_tuptable != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("query has no destination for result data"),
					 (rc == SPI_OK_SELECT) ? errhint("If you want to discard the results of a SELECT, use PERFORM instead.") : 0));
	}

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_dynexecute 执行动态 SQL 查询
 *	可能带有 INTO。
 * ----------
 */
static int
exec_stmt_dynexecute(PLpgSQL_execstate *estate,
					 PLpgSQL_stmt_dynexecute *stmt)
{
	Datum		query;
	bool		isnull;
	Oid			restype;
	int32		restypmod;
	char	   *querystr;
	int			exec_res;
	ParamListInfo paramLI;
	SPIExecuteOptions options;
	MemoryContext stmt_mcontext = get_stmt_mcontext(estate);

	/*
	 * 先对 EXECUTE 关键字之后的字符串表达式求值。其
	 * 结果就是要执行的查询串。
	 */
	query = exec_eval_expr(estate, stmt->query, &isnull, &restype, &restypmod);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("query string argument of EXECUTE is null")));

	/* 取 C 字符串表示 */
	querystr = convert_value_to_string(estate, query, restype);

	/* 清理前把它拷进 stmt_mcontext */
	querystr = MemoryContextStrdup(stmt_mcontext, querystr);

	exec_eval_cleanup(estate);

	/*
	 * 执行该查询，但不准备保存的计划。
	 */
	paramLI = exec_eval_using_params(estate, stmt->params);

	memset(&options, 0, sizeof(options));
	options.params = paramLI;
	options.read_only = estate->readonly_func;

	exec_res = SPI_execute_extended(querystr, &options);

	switch (exec_res)
	{
		case SPI_OK_SELECT:
		case SPI_OK_INSERT:
		case SPI_OK_UPDATE:
		case SPI_OK_DELETE:
		case SPI_OK_INSERT_RETURNING:
		case SPI_OK_UPDATE_RETURNING:
		case SPI_OK_DELETE_RETURNING:
		case SPI_OK_MERGE:
		case SPI_OK_UTILITY:
		case SPI_OK_REWRITTEN:
			break;

		case 0:

			/*
			 * 也允许返回零，这表示该查询串
			 * 里不含命令。
			 */
			break;

		case SPI_OK_SELINTO:

			/*
			 * 我们目前想禁止 SELECT INTO，因其行为
			 * 与普通 plpgsql 语境里的 SELECT INTO 不一致。
			 * 我们需重做 EXECUTE，把字符串解析成一条
			 * plpgsql 命令，而不只是喂给 SPI_execute。这并
			 * 非功能限制，因为允许 CREATE TABLE AS。
			 */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("EXECUTE of SELECT ... INTO is not implemented"),
					 errhint("You might want to use EXECUTE ... INTO or EXECUTE CREATE TABLE ... AS instead.")));
			break;

			/* 有些 SPI 错误该用专门的错误消息 */
		case SPI_ERROR_COPY:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot COPY to/from client in PL/pgSQL")));
			break;

		case SPI_ERROR_TRANSACTION:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("EXECUTE of transaction commands is not implemented")));
			break;

		default:
			elog(ERROR, "SPI_execute_extended failed executing query \"%s\": %s",
				 querystr, SPI_result_code_string(exec_res));
			break;
	}

	/* 为 GET DIAGNOSTICS 留结果信息 */
	estate->eval_processed = SPI_processed;

	/* 若有 INTO 就处理 */
	if (stmt->into)
	{
		SPITupleTable *tuptab = SPI_tuptable;
		uint64		n = SPI_processed;
		PLpgSQL_variable *target;

		/* 语句若没返回元组表，就报错 */
		if (tuptab == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("INTO used with a command that cannot return data")));

		/* 取目标的 datum 项 */
		target = (PLpgSQL_variable *) estate->datums[stmt->target->dno];

		/*
		 * 若 SELECT ... INTO 指定了 STRICT，且查询没找到
		 * 恰好一行就报错。若未指定 STRICT，则
		 * 允许查询找到任意行数。
		 */
		if (n == 0)
		{
			if (stmt->strict)
			{
				char	   *errdetail;

				if (estate->func->print_strict_params)
					errdetail = format_preparedparamsdata(estate, paramLI);
				else
					errdetail = NULL;

				ereport(ERROR,
						(errcode(ERRCODE_NO_DATA_FOUND),
						 errmsg("query returned no rows"),
						 errdetail ? errdetail_internal("parameters: %s", errdetail) : 0));
			}
			/* 把目标设成 NULL */
			exec_move_row(estate, target, NULL, tuptab->tupdesc);
		}
		else
		{
			if (n > 1 && stmt->strict)
			{
				char	   *errdetail;

				if (estate->func->print_strict_params)
					errdetail = format_preparedparamsdata(estate, paramLI);
				else
					errdetail = NULL;

				ereport(ERROR,
						(errcode(ERRCODE_TOO_MANY_ROWS),
						 errmsg("query returned more than one row"),
						 errdetail ? errdetail_internal("parameters: %s", errdetail) : 0));
			}

			/* 把第一行结果放进目标 */
			exec_move_row(estate, target, tuptab->vals[0], tuptab->tupdesc);
		}
		/* exec_move_row() 之后清理 */
		exec_eval_cleanup(estate);
	}
	else
	{
		/*
		 * 查询若返回了正被忽略的元组，报错也许
		 * 是好主意，但历来我们并未这么做这
		 * 事
		 */
	}

	/* 释放 SPI_execute 的任何结果，以及临时数据 */
	SPI_freetuptable(SPI_tuptable);
	MemoryContextReset(stmt_mcontext);

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_dynfors 执行动态查询，把每个
 *	元组赋给记录或行并
 *	执行一组语句
 *	为它。
 * ----------
 */
static int
exec_stmt_dynfors(PLpgSQL_execstate *estate, PLpgSQL_stmt_dynfors *stmt)
{
	Portal		portal;
	int			rc;

	portal = exec_dynquery_with_params(estate, stmt->query, stmt->params,
									   NULL, CURSOR_OPT_NO_SCROLL);

	/*
	 * 执行该循环
	 */
	rc = exec_for_query(estate, (PLpgSQL_stmt_forq *) stmt, portal, true);

	/*
	 * 关闭隐式游标
	 */
	SPI_cursor_close(portal);

	return rc;
}


/* ----------
 * exec_stmt_open 执行一条 OPEN 游标语句
 * ----------
 */
static int
exec_stmt_open(PLpgSQL_execstate *estate, PLpgSQL_stmt_open *stmt)
{
	PLpgSQL_var *curvar;
	MemoryContext stmt_mcontext = NULL;
	char	   *curname = NULL;
	PLpgSQL_expr *query;
	Portal		portal;
	ParamListInfo paramLI;

	/* ----------
	 * 取游标变量，若已有名字就检查
	 * 它当前并未在用。
	 * ----------
	 */
	curvar = (PLpgSQL_var *) (estate->datums[stmt->curvar]);
	if (!curvar->isnull)
	{
		MemoryContext oldcontext;

		/* 只需 stmt_mcontext 留住游标名字符串 */
		stmt_mcontext = get_stmt_mcontext(estate);
		oldcontext = MemoryContextSwitchTo(stmt_mcontext);
		curname = TextDatumGetCString(curvar->value);
		MemoryContextSwitchTo(oldcontext);

		if (SPI_cursor_find(curname) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_CURSOR),
					 errmsg("cursor \"%s\" already in use", curname)));
	}

	/* ----------
	 * 按它的类型来处理这个 OPEN。
	 * ----------
	 */
	if (stmt->query != NULL)
	{
		/* ----------
		 * 这是 OPEN refcursor FOR SELECT ...
		 *
		 * 我们只确保该查询已有计划。真正的工作
		 * 在下层完成
		 * ----------
		 */
		query = stmt->query;
		if (query->plan == NULL)
			exec_prepare_plan(estate, query, stmt->cursor_options);
	}
	else if (stmt->dynquery != NULL)
	{
		/* ----------
		 * 这是 OPEN refcursor FOR EXECUTE ...
		 * ----------
		 */
		portal = exec_dynquery_with_params(estate,
										   stmt->dynquery,
										   stmt->params,
										   curname,
										   stmt->cursor_options);

		/*
		 * 若游标变量为 NULL，就把生成的门户名存入，
		 * 先确认可以赋值给它。
		 *
		 * 注意：exec_dynquery_with_params 已重置 stmt_mcontext，故
		 * 此处 curname 是悬空指针；但测它是否为 NULL 仍
		 * 可
		 */
		if (curname == NULL)
		{
			exec_check_assignable(estate, stmt->curvar);
			assign_text_var(estate, curvar, portal->name);
		}

		return PLPGSQL_RC_OK;
	}
	else
	{
		/* ----------
		 * 这是 OPEN 游标
		 *
		 * 注意：解析器本该已经检查过该语句提供了
		 * 仅当游标需要才给参，安全起见再查一次。
		 * ----------
		 */
		if (stmt->argquery != NULL)
		{
			/* ----------
			 * 带参 OPEN CURSOR。假造 SELECT ... INTO
			 * 语句，以求值参数并放入
			 * 内部行。
			 * ----------
			 */
			PLpgSQL_stmt_execsql set_args;

			if (curvar->cursor_explicit_argrow < 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("arguments given for cursor without arguments")));

			memset(&set_args, 0, sizeof(set_args));
			set_args.cmd_type = PLPGSQL_STMT_EXECSQL;
			set_args.lineno = stmt->lineno;
			set_args.sqlstmt = stmt->argquery;
			set_args.into = true;
			/* XXX 历来这里不是 STRICT */
			set_args.target = (PLpgSQL_variable *)
				(estate->datums[curvar->cursor_explicit_argrow]);

			if (exec_stmt_execsql(estate, &set_args) != PLPGSQL_RC_OK)
				elog(ERROR, "open cursor failed during argument processing");
		}
		else
		{
			if (curvar->cursor_explicit_argrow >= 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("arguments required for cursor")));
		}

		query = curvar->cursor_explicit_expr;
		if (query->plan == NULL)
			exec_prepare_plan(estate, query, curvar->cursor_options);
	}

	/*
	 * 为该查询建 ParamListInfo
	 */
	paramLI = setup_param_list(estate, query);

	/*
	 * 打开游标（参数列表会拷进门户）
	 */
	portal = SPI_cursor_open_with_paramlist(curname, query->plan,
											paramLI,
											estate->readonly_func);
	if (portal == NULL)
		elog(ERROR, "could not open cursor: %s",
			 SPI_result_code_string(SPI_result));

	/*
	 * 若游标变量为 NULL，就把生成的门户名存入，
	 * 先确认可以赋值给它。
	 */
	if (curname == NULL)
	{
		exec_check_assignable(estate, stmt->curvar);
		assign_text_var(estate, curvar, portal->name);
	}

	/* 若有过任何临时数据，就清掉 */
	exec_eval_cleanup(estate);
	if (stmt_mcontext)
		MemoryContextReset(stmt_mcontext);

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_fetch 从游标取入目标，还是只
 *	移动该游标的当前位置
 * ----------
 */
static int
exec_stmt_fetch(PLpgSQL_execstate *estate, PLpgSQL_stmt_fetch *stmt)
{
	PLpgSQL_var *curvar;
	long		how_many = stmt->how_many;
	SPITupleTable *tuptab;
	Portal		portal;
	char	   *curname;
	uint64		n;
	MemoryContext oldcontext;

	/* ----------
	 * 按名字取游标的门户
	 * ----------
	 */
	curvar = (PLpgSQL_var *) (estate->datums[stmt->curvar]);
	if (curvar->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("cursor variable \"%s\" is null", curvar->refname)));

	/* 短命字符串用 eval_mcontext */
	oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
	curname = TextDatumGetCString(curvar->value);
	MemoryContextSwitchTo(oldcontext);

	portal = SPI_cursor_find(curname);
	if (portal == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", curname)));

	/* 为 FETCH_RELATIVE 还是 FETCH_ABSOLUTE 计算位置 */
	if (stmt->expr)
	{
		bool		isnull;

		/* XXX 本该用 LONG 而非 INT 宽度 */
		how_many = exec_eval_integer(estate, stmt->expr, &isnull);

		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("relative or absolute cursor position is null")));

		exec_eval_cleanup(estate);
	}

	if (!stmt->is_move)
	{
		PLpgSQL_variable *target;

		/* ----------
		 * 从游标取出 1 个元组
		 * ----------
		 */
		SPI_scroll_cursor_fetch(portal, stmt->direction, how_many);
		tuptab = SPI_tuptable;
		n = SPI_processed;

		/* ----------
		 * 恰当地设置该目标。
		 * ----------
		 */
		target = (PLpgSQL_variable *) estate->datums[stmt->target->dno];
		if (n == 0)
			exec_move_row(estate, target, NULL, tuptab->tupdesc);
		else
			exec_move_row(estate, target, tuptab->vals[0], tuptab->tupdesc);

		exec_eval_cleanup(estate);
		SPI_freetuptable(tuptab);
	}
	else
	{
		/* 移动该游标 */
		SPI_scroll_cursor_move(portal, stmt->direction, how_many);
		n = SPI_processed;
	}

	/* 恰当地设置 ROW_COUNT 与全局 FOUND 变量。 */
	estate->eval_processed = n;
	exec_set_found(estate, n != 0);

	return PLPGSQL_RC_OK;
}

/* ----------
 * exec_stmt_close 关闭游标
 * ----------
 */
static int
exec_stmt_close(PLpgSQL_execstate *estate, PLpgSQL_stmt_close *stmt)
{
	PLpgSQL_var *curvar;
	Portal		portal;
	char	   *curname;
	MemoryContext oldcontext;

	/* ----------
	 * 按名字取游标的门户
	 * ----------
	 */
	curvar = (PLpgSQL_var *) (estate->datums[stmt->curvar]);
	if (curvar->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("cursor variable \"%s\" is null", curvar->refname)));

	/* 短命字符串用 eval_mcontext */
	oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
	curname = TextDatumGetCString(curvar->value);
	MemoryContextSwitchTo(oldcontext);

	portal = SPI_cursor_find(curname);
	if (portal == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", curname)));

	/* ----------
	 * 并关闭它
	 * ----------
	 */
	SPI_cursor_close(portal);

	return PLPGSQL_RC_OK;
}

/*
 * exec_stmt_commit
 *
 * 提交该事务。
 */
static int
exec_stmt_commit(PLpgSQL_execstate *estate, PLpgSQL_stmt_commit *stmt)
{
	if (stmt->chain)
		SPI_commit_and_chain();
	else
		SPI_commit();

	/*
	 * 必须重建简单表达式设施，因为旧的
	 * 数据结构已消失。
	 */
	estate->simple_eval_estate = NULL;
	estate->simple_eval_resowner = NULL;
	plpgsql_create_econtext(estate);

	return PLPGSQL_RC_OK;
}

/*
 * exec_stmt_rollback
 *
 * 中止该事务。
 */
static int
exec_stmt_rollback(PLpgSQL_execstate *estate, PLpgSQL_stmt_rollback *stmt)
{
	if (stmt->chain)
		SPI_rollback_and_chain();
	else
		SPI_rollback();

	/*
	 * 必须重建简单表达式设施，因为旧的
	 * 数据结构已消失。
	 */
	estate->simple_eval_estate = NULL;
	estate->simple_eval_resowner = NULL;
	plpgsql_create_econtext(estate);

	return PLPGSQL_RC_OK;
}

/* ----------
 * exec_assign_expr 把表达式的结果放入一个变量。
 * ----------
 */
static void
exec_assign_expr(PLpgSQL_execstate *estate, PLpgSQL_datum *target,
				 PLpgSQL_expr *expr)
{
	Datum		value;
	bool		isnull;
	Oid			valtype;
	int32		valtypmod;

	/*
	 * 若是第一次，就为该表达式建计划。
	 */
	if (expr->plan == NULL)
	{
		/*
		 * 若目标是简单变量，就把该表达式标成赋值源。
		 * 这有点乱，但似乎比为此去改
		 * exec_prepare_plan 的接口更干净。我们反正需把
		 * 目标 dno 塞进该 expr，以便我们必须重做
		 * 计划时它仍可用。
		 */
		if (target->dtype == PLPGSQL_DTYPE_VAR)
			expr->target_param = target->dno;
		else
			expr->target_param = -1;	/* 本该已经如此 */

		exec_prepare_plan(estate, expr, 0);
	}

	value = exec_eval_expr(estate, expr, &isnull, &valtype, &valtypmod);
	exec_assign_value(estate, target, value, isnull, valtype, valtypmod);
	exec_eval_cleanup(estate);
}


/* ----------
 * exec_assign_c_string 把C字符串放入文本变量。
 *
 * 我们把 NULL 指针看成空串，而非 SQL 空值。
 *
 * 与底层的 exec_assign_value 一样，期望调用者稍后做
 * exec_eval_cleanup。
 * ----------
 */
static void
exec_assign_c_string(PLpgSQL_execstate *estate, PLpgSQL_datum *target,
					 const char *str)
{
	text	   *value;
	MemoryContext oldcontext;

	/* 短命文本值用 eval_mcontext */
	oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
	if (str != NULL)
		value = cstring_to_text(str);
	else
		value = cstring_to_text("");
	MemoryContextSwitchTo(oldcontext);

	exec_assign_value(estate, target, PointerGetDatum(value), false,
					  TEXTOID, -1);
}


/* ----------
 * exec_assign_value 把一个值放入目标 datum
 *
 * 注意：有些代码路径将在 eval_mcontext 泄漏内存；
 * 我们假定稍后由 exec_eval_cleanup 清掉。我们无法
 * 在此调用它，唯恐毁掉输入的 Datum 值。
 * ----------
 */
static void
exec_assign_value(PLpgSQL_execstate *estate,
				  PLpgSQL_datum *target,
				  Datum value, bool isNull,
				  Oid valtype, int32 valtypmod)
{
	switch (target->dtype)
	{
		case PLPGSQL_DTYPE_VAR:
		case PLPGSQL_DTYPE_PROMISE:
			{
				/*
				 * 目标是变量
				 */
				PLpgSQL_var *var = (PLpgSQL_var *) target;
				Datum		newvalue;

				newvalue = exec_cast_value(estate,
										   value,
										   &isNull,
										   valtype,
										   valtypmod,
										   var->datatype->typoid,
										   var->datatype->atttypmod);

				if (isNull && var->notnull)
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("null value cannot be assigned to variable \"%s\" declared NOT NULL",
									var->refname)));

				/*
				 * 若类型按引用，就把新值（它多半位于
				 * eval_mcontext 里）拷进该过程的主
				 * 内存上下文。但若它是指向扩展对象的读写
				 * 引用，就不需物理拷贝；最多我们需要
				 * 改挂该对象的内存上下文。
				 *
				 * 若它是数组，就强制以 R/W 扩展形式存放
				 * 该值。函数若后来对变量做很多下标操作，
				 * 这样便划算，否则也许吃亏。我们也许要
				 * 换一种启发，但现在判断还太早。另外，
				 * 是否有这种情形：把非数组的值也强制
				 * 存成扩展形式才有用
				 * 呢？
				 */
				if (!var->datatype->typbyval && !isNull)
				{
					if (var->datatype->typisarray &&
						!VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(newvalue)))
					{
						/* 数组且还不是 R/W，就用 expand_array */
						newvalue = expand_array(newvalue,
												estate->datum_context,
												NULL);
					}
					else
					{
						/* 否则R/W就移交值，不然只 datumCopy */
						newvalue = datumTransfer(newvalue,
												 false,
												 var->datatype->typlen);
					}
				}

				/*
				 * 现在释放旧值（若有）并赋上新值。但
				 * 若新旧值相同，就跳过这次赋值。
				 * 注意对扩展对象，此检查有必要，且
				 * 无法更早可靠做出；我们必须正看着
				 * 该对象的标准 R/W 指针，才能确定指针
				 * 相等有意义。
				 *
				 * 另外若它是承诺变量，任何情况下我们
				 * 都应解除承诺，否则向已武装的承诺
				 * 变量赋 NULL 将无法解除该承诺。
				 */
				if (var->value != newvalue || var->isnull || isNull)
					assign_simple_var(estate, var, newvalue, isNull,
									  (!var->datatype->typbyval && !isNull));
				else
					var->promise = PLPGSQL_PROMISE_NONE;
				break;
			}

		case PLPGSQL_DTYPE_ROW:
			{
				/*
				 * 目标是行变量
				 */
				PLpgSQL_row *row = (PLpgSQL_row *) target;

				if (isNull)
				{
					/* 若源为 NULL，就只对该行赋空值 */
					exec_move_row(estate, (PLpgSQL_variable *) row,
								  NULL, NULL);
				}
				else
				{
					/* 源必须是 RECORD 或复合类型 */
					if (!type_is_rowtype(valtype))
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("cannot assign non-composite value to a row variable")));
					exec_move_row_from_datum(estate, (PLpgSQL_variable *) row,
											 value);
				}
				break;
			}

		case PLPGSQL_DTYPE_REC:
			{
				/*
				 * 目标是记录变量
				 */
				PLpgSQL_rec *rec = (PLpgSQL_rec *) target;

				if (isNull)
				{
					if (rec->notnull)
						ereport(ERROR,
								(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
								 errmsg("null value cannot be assigned to variable \"%s\" declared NOT NULL",
										rec->refname)));

					/* 把变量设成简单 NULL */
					exec_move_row(estate, (PLpgSQL_variable *) rec,
								  NULL, NULL);
				}
				else
				{
					/* 源必须是 RECORD 或复合类型 */
					if (!type_is_rowtype(valtype))
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("cannot assign non-composite value to a record variable")));
					exec_move_row_from_datum(estate, (PLpgSQL_variable *) rec,
											 value);
				}
				break;
			}

		case PLPGSQL_DTYPE_RECFIELD:
			{
				/*
				 * 目标是记录的字段
				 */
				PLpgSQL_recfield *recfield = (PLpgSQL_recfield *) target;
				PLpgSQL_rec *rec;
				ExpandedRecordHeader *erh;

				rec = (PLpgSQL_rec *) (estate->datums[recfield->recparentno]);
				erh = rec->erh;

				/*
				 * 记录变量若为 NULL，就在它有
				 * 具名复合类型则建，否则报错。（这不改
				 * 记录的逻辑状态，但若我们下面成功赋值
				 * 则未赋值字段都将变成 NULL。）
				 */
				if (erh == NULL)
				{
					instantiate_empty_record_variable(estate, rec);
					erh = rec->erh;
				}

				/*
				 * 若尚未查过字段属性，或
				 * 元组描述符标识自上次变了。
				 */
				if (unlikely(recfield->rectupledescid != erh->er_tupdesc_id))
				{
					if (!expanded_record_lookup_field(erh,
													  recfield->fieldname,
													  &recfield->finfo))
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("record \"%s\" has no field \"%s\"",
										rec->refname, recfield->fieldname)));
					recfield->rectupledescid = erh->er_tupdesc_id;
				}

				/* 我们不支持对系统列赋值。 */
				if (recfield->finfo.fnumber <= 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot assign to system column \"%s\"",
									recfield->fieldname)));

				/* 需要时把新值转换成正确类型。 */
				value = exec_cast_value(estate,
										value,
										&isNull,
										valtype,
										valtypmod,
										recfield->finfo.ftypeid,
										recfield->finfo.ftypmod);

				/* 给它赋值 */
				expanded_record_set_field(erh, recfield->finfo.fnumber,
										  value, isNull, !estate->atomic);
				break;
			}

		default:
			elog(ERROR, "unrecognized dtype: %d", target->dtype);
	}
}

/*
 * exec_eval_datum 取得 PLpgSQL_datum 的当前值
 *
 * 返回类型 oid、typmod、Datum 形式的值，以及空标志。
 *
 * 目前这不处理 PLpgSQL_expr 这种 datum；那并不需要，
 * 因为我们从不把此类datum的引用传给 SPI。
 *
 * 注意：按引用传递时，返回的 Datum 直指所存的
 * 值。一般调用者应注意不要修改所存的值。
 * 不过有些调用者有意操作读写扩展指针所引用
 * 的变量；让结果在语义上正确，是那些调用者
 * 的责任。
 *
 * 有些情形我们必须 palloc 一个返回值，这时把它
 * 放进 estate 的 eval_mcontext。
 */
static void
exec_eval_datum(PLpgSQL_execstate *estate,
				PLpgSQL_datum *datum,
				Oid *typeid,
				int32 *typetypmod,
				Datum *value,
				bool *isnull)
{
	MemoryContext oldcontext;

	switch (datum->dtype)
	{
		case PLPGSQL_DTYPE_PROMISE:
			/* 需要时兑现承诺，再当普通变量处理 */
			plpgsql_fulfill_promise(estate, (PLpgSQL_var *) datum);

			/* FALL THRU */

		case PLPGSQL_DTYPE_VAR:
			{
				PLpgSQL_var *var = (PLpgSQL_var *) datum;

				*typeid = var->datatype->typoid;
				*typetypmod = var->datatype->atttypmod;
				*value = var->value;
				*isnull = var->isnull;
				break;
			}

		case PLPGSQL_DTYPE_ROW:
			{
				PLpgSQL_row *row = (PLpgSQL_row *) datum;
				HeapTuple	tup;

				/* 多个 OUT 参数时走到这里 */
				if (!row->rowtupdesc)	/* 不该发生 */
					elog(ERROR, "row variable has no tupdesc");
				/* 确保我们持有有效的类型与 typmod */
				BlessTupleDesc(row->rowtupdesc);
				oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
				tup = make_tuple_from_row(estate, row, row->rowtupdesc);
				if (tup == NULL)	/* 不该发生 */
					elog(ERROR, "row not compatible with its own tupdesc");
				*typeid = row->rowtupdesc->tdtypeid;
				*typetypmod = row->rowtupdesc->tdtypmod;
				*value = HeapTupleGetDatum(tup);
				*isnull = false;
				MemoryContextSwitchTo(oldcontext);
				break;
			}

		case PLPGSQL_DTYPE_REC:
			{
				PLpgSQL_rec *rec = (PLpgSQL_rec *) datum;

				if (rec->erh == NULL)
				{
					/* 把尚未实例化的记录当成简单 NULL */
					*value = (Datum) 0;
					*isnull = true;
					/* 报告变量声明的类型 */
					*typeid = rec->rectypeid;
					*typetypmod = -1;
				}
				else
				{
					if (ExpandedRecordIsEmpty(rec->erh))
					{
						/* 空记录同样是 NULL */
						*value = (Datum) 0;
						*isnull = true;
					}
					else
					{
						*value = ExpandedRecordGetDatum(rec->erh);
						*isnull = false;
					}
					if (rec->rectypeid != RECORDOID)
					{
						/* 非 RECORD 时报告变量声明的类型 */
						*typeid = rec->rectypeid;
						*typetypmod = -1;
					}
					else
					{
						/* 声明 RECORD 时报告记录实际类型 */
						*typeid = rec->erh->er_typeid;
						*typetypmod = rec->erh->er_typmod;
					}
				}
				break;
			}

		case PLPGSQL_DTYPE_RECFIELD:
			{
				PLpgSQL_recfield *recfield = (PLpgSQL_recfield *) datum;
				PLpgSQL_rec *rec;
				ExpandedRecordHeader *erh;

				rec = (PLpgSQL_rec *) (estate->datums[recfield->recparentno]);
				erh = rec->erh;

				/*
				 * 记录变量若为 NULL，就在它有
				 * 具名复合类型则建，否则报错。（这不改
				 * 记录的逻辑状态：它仍是 NULL）。
				 */
				if (erh == NULL)
				{
					instantiate_empty_record_variable(estate, rec);
					erh = rec->erh;
				}

				/*
				 * 若尚未查过字段属性，或
				 * 元组描述符标识自上次变了。
				 */
				if (unlikely(recfield->rectupledescid != erh->er_tupdesc_id))
				{
					if (!expanded_record_lookup_field(erh,
													  recfield->fieldname,
													  &recfield->finfo))
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("record \"%s\" has no field \"%s\"",
										rec->refname, recfield->fieldname)));
					recfield->rectupledescid = erh->er_tupdesc_id;
				}

				/* 报类型数据 */
				*typeid = recfield->finfo.ftypeid;
				*typetypmod = recfield->finfo.ftypmod;

				/* 并取出字段的值。 */
				*value = expanded_record_get_field(erh,
												   recfield->finfo.fnumber,
												   isnull);
				break;
			}

		default:
			elog(ERROR, "unrecognized dtype: %d", datum->dtype);
	}
}

/*
 * plpgsql_exec_get_datum_type 取 PLpgSQL_datum 的类型
 *
 * 逻辑与 exec_eval_datum 相同，但我们跳过获取
 * 变量的实际值。而且不必支持 DTYPE_ROW。
 */
Oid
plpgsql_exec_get_datum_type(PLpgSQL_execstate *estate,
							PLpgSQL_datum *datum)
{
	Oid			typeid;

	switch (datum->dtype)
	{
		case PLPGSQL_DTYPE_VAR:
		case PLPGSQL_DTYPE_PROMISE:
			{
				PLpgSQL_var *var = (PLpgSQL_var *) datum;

				typeid = var->datatype->typoid;
				break;
			}

		case PLPGSQL_DTYPE_REC:
			{
				PLpgSQL_rec *rec = (PLpgSQL_rec *) datum;

				if (rec->erh == NULL || rec->rectypeid != RECORDOID)
				{
					/* 报告变量声明的类型 */
					typeid = rec->rectypeid;
				}
				else
				{
					/* 声明 RECORD 时报告记录实际类型 */
					typeid = rec->erh->er_typeid;
				}
				break;
			}

		case PLPGSQL_DTYPE_RECFIELD:
			{
				PLpgSQL_recfield *recfield = (PLpgSQL_recfield *) datum;
				PLpgSQL_rec *rec;

				rec = (PLpgSQL_rec *) (estate->datums[recfield->recparentno]);

				/*
				 * 记录变量若为 NULL，就在它有
				 * 具名复合类型则建，否则报错。（这不改
				 * 记录的逻辑状态：它仍是 NULL）。
				 */
				if (rec->erh == NULL)
					instantiate_empty_record_variable(estate, rec);

				/*
				 * 若尚未查过字段属性，或
				 * 元组描述符标识自上次变了。
				 */
				if (unlikely(recfield->rectupledescid != rec->erh->er_tupdesc_id))
				{
					if (!expanded_record_lookup_field(rec->erh,
													  recfield->fieldname,
													  &recfield->finfo))
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("record \"%s\" has no field \"%s\"",
										rec->refname, recfield->fieldname)));
					recfield->rectupledescid = rec->erh->er_tupdesc_id;
				}

				typeid = recfield->finfo.ftypeid;
				break;
			}

		default:
			elog(ERROR, "unrecognized dtype: %d", datum->dtype);
			typeid = InvalidOid;	/* 让编译器安静 */
			break;
	}

	return typeid;
}

/*
 * plpgsql_exec_get_datum_type_info 取 PLpgSQL_datum 的类型等
 *
 * 这是 plpgsql_exec_get_datum_type 的扩展版，也取回
 * 该 datum 的 typmod 与排序规则。但请注意我们不报告
 * RECORD 值那可能变化的 typmod，而总是说 -1。
 */
void
plpgsql_exec_get_datum_type_info(PLpgSQL_execstate *estate,
								 PLpgSQL_datum *datum,
								 Oid *typeId, int32 *typMod, Oid *collation)
{
	switch (datum->dtype)
	{
		case PLPGSQL_DTYPE_VAR:
		case PLPGSQL_DTYPE_PROMISE:
			{
				PLpgSQL_var *var = (PLpgSQL_var *) datum;

				*typeId = var->datatype->typoid;
				*typMod = var->datatype->atttypmod;
				*collation = var->datatype->collation;
				break;
			}

		case PLPGSQL_DTYPE_REC:
			{
				PLpgSQL_rec *rec = (PLpgSQL_rec *) datum;

				if (rec->erh == NULL || rec->rectypeid != RECORDOID)
				{
					/* 报告变量声明的类型 */
					*typeId = rec->rectypeid;
					*typMod = -1;
				}
				else
				{
					/* 声明 RECORD 时报告记录实际类型 */
					*typeId = rec->erh->er_typeid;
					/* 切勿返回 RECORD 变量可变的 typmod */
					*typMod = -1;
				}
				/* 复合类型从不可带排序规则 */
				*collation = InvalidOid;
				break;
			}

		case PLPGSQL_DTYPE_RECFIELD:
			{
				PLpgSQL_recfield *recfield = (PLpgSQL_recfield *) datum;
				PLpgSQL_rec *rec;

				rec = (PLpgSQL_rec *) (estate->datums[recfield->recparentno]);

				/*
				 * 记录变量若为 NULL，就在它有
				 * 具名复合类型则建，否则报错。（这不改
				 * 记录的逻辑状态：它仍是 NULL）。
				 */
				if (rec->erh == NULL)
					instantiate_empty_record_variable(estate, rec);

				/*
				 * 若尚未查过字段属性，或
				 * 元组描述符标识自上次变了。
				 */
				if (unlikely(recfield->rectupledescid != rec->erh->er_tupdesc_id))
				{
					if (!expanded_record_lookup_field(rec->erh,
													  recfield->fieldname,
													  &recfield->finfo))
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("record \"%s\" has no field \"%s\"",
										rec->refname, recfield->fieldname)));
					recfield->rectupledescid = rec->erh->er_tupdesc_id;
				}

				*typeId = recfield->finfo.ftypeid;
				*typMod = recfield->finfo.ftypmod;
				*collation = recfield->finfo.fcollation;
				break;
			}

		default:
			elog(ERROR, "unrecognized dtype: %d", datum->dtype);
			*typeId = InvalidOid;	/* 让编译器安静 */
			*typMod = -1;
			*collation = InvalidOid;
			break;
	}
}

/* ----------
 * exec_eval_integer 对表达式求值，把结果强制为 int4
 *
 * 注意这里不做 exec_eval_cleanup；调用者必须在
 * 稍后再做。（我们这么做，因调用者可能正持有
 * 别的按引用传递的表达式求值结果，例如
 * 待下标的数组值。）
 * ----------
 */
static int
exec_eval_integer(PLpgSQL_execstate *estate,
				  PLpgSQL_expr *expr,
				  bool *isNull)
{
	Datum		exprdatum;
	Oid			exprtypeid;
	int32		exprtypmod;

	exprdatum = exec_eval_expr(estate, expr, isNull, &exprtypeid, &exprtypmod);
	exprdatum = exec_cast_value(estate, exprdatum, isNull,
								exprtypeid, exprtypmod,
								INT4OID, -1);
	return DatumGetInt32(exprdatum);
}

/* ----------
 * exec_eval_boolean 对表达式求值，把结果强制为 bool
 *
 * 注意这里不做 exec_eval_cleanup；调用者必须在
 * 稍后再做。
 * ----------
 */
static bool
exec_eval_boolean(PLpgSQL_execstate *estate,
				  PLpgSQL_expr *expr,
				  bool *isNull)
{
	Datum		exprdatum;
	Oid			exprtypeid;
	int32		exprtypmod;

	exprdatum = exec_eval_expr(estate, expr, isNull, &exprtypeid, &exprtypmod);
	exprdatum = exec_cast_value(estate, exprdatum, isNull,
								exprtypeid, exprtypmod,
								BOOLOID, -1);
	return DatumGetBool(exprdatum);
}

/* ----------
 * exec_eval_expr 对表达式求值并返回
 *	结果 Datum，连同数据类型与 typmod。
 *
 * 注意：调用者用完 Datum 后必须做 exec_eval_cleanup。
 * ----------
 */
static Datum
exec_eval_expr(PLpgSQL_execstate *estate,
			   PLpgSQL_expr *expr,
			   bool *isNull,
			   Oid *rettype,
			   int32 *rettypmod)
{
	Datum		result = 0;
	int			rc;
	Form_pg_attribute attr;

	/*
	 * 若是第一次，就为该表达式建计划。
	 */
	if (expr->plan == NULL)
		exec_prepare_plan(estate, expr, CURSOR_OPT_PARALLEL_OK);

	/*
	 * 若这是简单表达式，就绕过 SPI，改用执行器
	 * 直接
	 */
	if (exec_eval_simple_expr(estate, expr,
							  &result, isNull, rettype, rettypmod))
		return result;

	/*
	 * 否则经由 exec_run_select 走难路
	 */
	rc = exec_run_select(estate, expr, 0, NULL);
	if (rc != SPI_OK_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("query did not return data"),
				 errcontext("query: %s", expr->query)));

	/*
	 * 检查该表达式恰好返回一列……
	 */
	if (estate->eval_tuptable->tupdesc->natts != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg_plural("query returned %d column",
							   "query returned %d columns",
							   estate->eval_tuptable->tupdesc->natts,
							   estate->eval_tuptable->tupdesc->natts),
				 errcontext("query: %s", expr->query)));

	/*
	 * 并取得该列的数据类型。
	 */
	attr = TupleDescAttr(estate->eval_tuptable->tupdesc, 0);
	*rettype = attr->atttypid;
	*rettypmod = attr->atttypmod;

	/*
	 * 若没有选中任何行，结果就是该类型的一个 NULL。
	 */
	if (estate->eval_processed == 0)
	{
		*isNull = true;
		return (Datum) 0;
	}

	/*
	 * 检查该表达式返回的行不超过一行。
	 */
	if (estate->eval_processed != 1)
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("query returned more than one row"),
				 errcontext("query: %s", expr->query)));

	/*
	 * 返回唯一的结果Datum。
	 */
	return SPI_getbinval(estate->eval_tuptable->vals[0],
						 estate->eval_tuptable->tupdesc, 1, isNull);
}


/* ----------
 * exec_run_select 执行选择查询
 *
 * 注意：maxtuples 不为 0（即返回全部元组）已
 * 被弃用，因它将阻止该查询并行执行。
 * 但我们仍保留该参数，以备将来需要它。
 * ----------
 */
static int
exec_run_select(PLpgSQL_execstate *estate,
				PLpgSQL_expr *expr, long maxtuples, Portal *portalP)
{
	ParamListInfo paramLI;
	int			rc;

	/*
	 * 该表达式第一次调用时生成计划。
	 *
	 * 若我们不需要返回门户，那就只是马上
	 * 执行该查询，故即便所取行数有限，用并行
	 * 计划也行。若我们确实需要返回门户
	 * 也就是给 FOR 循环用的，用户代码可能在
	 * FOR 循环里再调用别的操作，从而使并行查询
	 * 不安全。无论如何，我们并不期望做任何游标
	 * 操作，故为效率与语义安全指定 NO_SCROLL。
	 */
	if (expr->plan == NULL)
	{
		int			cursorOptions = CURSOR_OPT_NO_SCROLL;

		if (portalP == NULL)
			cursorOptions |= CURSOR_OPT_PARALLEL_OK;
		exec_prepare_plan(estate, expr, cursorOptions);
	}

	/*
	 * 建 ParamListInfo 传给执行器
	 */
	paramLI = setup_param_list(estate, expr);

	/*
	 * 若请求了门户，就把查询与参数列表放进该门户
	 */
	if (portalP != NULL)
	{
		*portalP = SPI_cursor_open_with_paramlist(NULL, expr->plan,
												  paramLI,
												  estate->readonly_func);
		if (*portalP == NULL)
			elog(ERROR, "could not open implicit cursor for query \"%s\": %s",
				 expr->query, SPI_result_code_string(SPI_result));
		exec_eval_cleanup(estate);
		return SPI_OK_CURSOR;
	}

	/*
	 * 执行该查询
	 */
	rc = SPI_execute_plan_with_paramlist(expr->plan, paramLI,
										 estate->readonly_func, maxtuples);
	if (rc != SPI_OK_SELECT)
	{
		/*
		 * SELECT INTO 该有专门的错误消息，因为"查询不是
		 * SELECT"此时帮助不大。
		 */
		if (rc == SPI_OK_SELINTO)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("query is SELECT INTO, but it should be plain SELECT"),
					 errcontext("query: %s", expr->query)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("query is not a SELECT"),
					 errcontext("query: %s", expr->query)));
	}

	/* 保存查询结果，以便最终清理 */
	Assert(estate->eval_tuptable == NULL);
	estate->eval_tuptable = SPI_tuptable;
	estate->eval_processed = SPI_processed;

	return rc;
}


/*
 * exec_for_query 对来自门户的每一行执行 FOR 循环体
 *
 * 供 exec_stmt_fors、exec_stmt_forc与exec_stmt_dynfors 用
 */
static int
exec_for_query(PLpgSQL_execstate *estate, PLpgSQL_stmt_forq *stmt,
			   Portal portal, bool prefetch_ok)
{
	PLpgSQL_variable *var;
	SPITupleTable *tuptab;
	bool		found = false;
	int			rc = PLPGSQL_RC_OK;
	uint64		previous_id = INVALID_TUPLEDESC_IDENTIFIER;
	bool		tupdescs_match = true;
	uint64		n;

	/* 取循环变量的 datum 项 */
	var = (PLpgSQL_variable *) estate->datums[stmt->var->dno];

	/*
	 * 确保该门户不被我们所执行的用户语句
	 * 关闭
	 */
	PinPortal(portal);

	/*
	 * 在非原子语境里，即便本来安全，我们也不敢
	 * 预取。除了那可能造成的任何语义风险，若我们
	 * 预取了 toast 数据，然后用户提交事务，
	 * toast 引用就可能变成悬空指针。（尚未从该
	 * 游标取出的行是安全的，因 PersistHoldablePortal
	 * 机制处理此情形。）
	 */
	if (!estate->atomic)
		prefetch_ok = false;

	/*
	 * 取初始的元组。若允许预取，我们就再多抓
	 * 几行，以免多次付出执行器的启动
	 * 开销。
	 */
	SPI_cursor_fetch(portal, true, prefetch_ok ? 10 : 1);
	tuptab = SPI_tuptable;
	n = SPI_processed;

	/*
	 * 若查询没有返回任何行，就把目标设为 NULL 并
	 * 以 found 为假直落
	 */
	if (n == 0)
	{
		exec_move_row(estate, var, NULL, tuptab->tupdesc);
		exec_eval_cleanup(estate);
	}
	else
		found = true;			/* 已处理至少一个元组 */

	/*
	 * 现在做循环
	 */
	while (n > 0)
	{
		uint64		i;

		for (i = 0; i < n; i++)
		{
			/*
			 * 把该元组赋给目标。在此，因我们知道所有
			 * 循环迭代都应赋同一元组描述符，故能
			 * 省掉反复创建描述符相同的扩展记录。
			 * 检测 er_tupdesc_id 变化依然可靠，即便循环
			 * 体含有把目标值整个替换掉的赋值，
			 * 因为它取自进程全局计数器。描述符不
			 * 相符时，也许能比这段代码处理得更高效，
			 * 但额外工夫是否值得，目前并不
			 * 清楚。
			 */
			if (var->dtype == PLPGSQL_DTYPE_REC)
			{
				PLpgSQL_rec *rec = (PLpgSQL_rec *) var;

				if (rec->erh &&
					rec->erh->er_tupdesc_id == previous_id &&
					tupdescs_match)
				{
					/* 只需赋一个新的元组值 */
					expanded_record_set_tuple(rec->erh, tuptab->vals[i],
											  true, !estate->atomic);
				}
				else
				{
					/*
					 * 首次，还是变量描述符于循环中变了，
					 * 还是因要做类型强制而必须走难
					 * 路。
					 */
					exec_move_row(estate, var,
								  tuptab->vals[i], tuptab->tupdesc);

					/*
					 * 检查下次物理赋值是否可行。
					 * 元组描述符比较一旦失败过，我们就不
					 * 再在随后的循环迭代里复查。
					 */
					if (tupdescs_match)
					{
						tupdescs_match =
							(rec->rectypeid == RECORDOID ||
							 rec->rectypeid == tuptab->tupdesc->tdtypeid ||
							 compatible_tupdescs(tuptab->tupdesc,
												 expanded_record_get_tupdesc(rec->erh)));
					}
					previous_id = rec->erh->er_tupdesc_id;
				}
			}
			else
				exec_move_row(estate, var, tuptab->vals[i], tuptab->tupdesc);

			exec_eval_cleanup(estate);

			/*
			 * 执行这些语句
			 */
			rc = exec_stmts(estate, stmt->body);

			LOOP_RC_PROCESSING(stmt->label, goto loop_exit);
		}

		SPI_freetuptable(tuptab);

		/*
		 * 再取更多元组。若允许预取，一次抓 50 个。
		 */
		SPI_cursor_fetch(portal, true, prefetch_ok ? 50 : 1);
		tuptab = SPI_tuptable;
		n = SPI_processed;
	}

loop_exit:

	/*
	 * 释放上一组元组（若有）
	 */
	SPI_freetuptable(tuptab);

	UnpinPortal(portal);

	/*
	 * 设 FOUND 变量，标明执行该循环的结果，
	 * 即：是否至少循环过一次。必须最后设置
	 * 以免干扰 FOUND 变量的值，于
	 * 循环处理本身内。
	 */
	exec_set_found(estate, found);

	return rc;
}


/* ----------
 * exec_eval_simple_expr 求简单表达式的值，返回
 *	Datum，靠直接调用 ExecEvalExpr()。
 *
 * 若成功，就把结果存入 *result、*isNull、*rettype、*rettypmod
 * 并返回真。若该表达式无法用简单求值处理，就
 * 返回假。
 *
 * 因我们对一条简单表达式只存一棵执行树，故
 * 无法处理递归。因此若看到该树在当前事务里
 * 已在求值，我们就返回假，让调用者用难的方式
 * 去跑该表达式。（其他办法，例如为递归调用
 * 新建一棵树，要么引入内存泄漏，要么添上不少
 * 记账，反正未必划算。）expr_simple_in_use 还覆盖
 * 另一种情形：该树上次执行因出错而中止，树里
 * 可能含有虚假状态，故我们不敢
 * 再重用它。
 *
 * 我们有可能需要重做一条简单表达式的计划；例如
 * 有人可能重定义一个已内联进该简单表达式的 SQL
 * 函数。那无法让简单表达式变成非简单（反过来
 * 也一样），但我们必须处理表达式树的替换。
 *
 * 注意：若按引用传递，结果就在 eval_mcontext 里。
 * exec_eval_cleanup 做完时它将被释放。
 * ----------
 */
static bool
exec_eval_simple_expr(PLpgSQL_execstate *estate,
					  PLpgSQL_expr *expr,
					  Datum *result,
					  bool *isNull,
					  Oid *rettype,
					  int32 *rettypmod)
{
	ExprContext *econtext = estate->eval_econtext;
	LocalTransactionId curlxid = MyProc->lxid;
	ParamListInfo paramLI;
	void	   *save_setup_arg;
	bool		need_snapshot;
	MemoryContext oldcontext;

	/*
	 * 若表达式原先不简单，就忘掉它。
	 */
	if (expr->expr_simple_expr == NULL)
		return false;

	/*
	 * 若表达式正在当前事务里用，就别碰它。
	 */
	if (unlikely(expr->expr_simple_in_use) &&
		expr->expr_simple_lxid == curlxid)
		return false;

	/*
	 * 确保有门户级快照，以防这条简单表达式
	 * 是 COMMIT 还是 ROLLBACK 之后第一个被求值的。
	 * 执行该表达式之前我们反正必须这么做，故
	 * 不妨现在就做，以确保任何可能的重计划都
	 * 不需要取新快照。
	 */
	EnsurePortalSnapshotExists();

	/*
	 * 检查缓存计划是否已失效。若没有，且这是
	 * 当前事务里的首次使用，就把计划引用计数存入
	 * 简单表达式所有者。
	 */
	if (likely(CachedPlanIsSimplyValid(expr->expr_simple_plansource,
									   expr->expr_simple_plan,
									   (expr->expr_simple_plan_lxid != curlxid ?
										estate->simple_eval_resowner : NULL))))
	{
		/*
		 * 它仍然有效，故只需记住我们在当前
		 * 事务里对该计划有引用计数。（若已有，本次
		 * 赋值即空操作）
		 */
		expr->expr_simple_plan_lxid = curlxid;
	}
	else
	{
		/* 须重计划 */
		CachedPlan *cplan;

		/*
		 * 若我们对计划的某先前版本持有有效引用计数，
		 * 就释放它，以免在事务之内泄漏计划。
		 */
		if (expr->expr_simple_plan_lxid == curlxid)
			ReleaseCachedPlan(expr->expr_simple_plan,
							  estate->simple_eval_resowner);

		/*
		 * 重置成非简单，以便重做计划若失败，留下
		 * 没有悬空指针的健全状态。不过 expr_simple_plansource
		 * 可以不动，因它无法移动。
		 */
		expr->expr_simple_expr = NULL;
		expr->expr_rw_param = NULL;
		expr->expr_simple_plan = NULL;
		expr->expr_simple_plan_lxid = InvalidLocalTransactionId;

		/* 在 eval_mcontext 里做重计划的工作 */
		oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
		cplan = SPI_plan_get_cached_plan(expr->plan);
		MemoryContextSwitchTo(oldcontext);

		/*
		 * 这里我们无法失败，因为 SPI 计划里
		 * CachedPlanSources 数量无法不同于
		 * exec_simple_check_plan 所见。这属原始分析树，
		 * 由查询文本生成。
		 */
		Assert(cplan != NULL);

		/*
		 * 复查 exec_is_simple_query，它在边缘情形现在可能
		 * 报出假，例如非 SRF 被换成了
		 * SRF。再查 CachedPlanAllowsSimpleValidityCheck 以求
		 * 确认。若任一测试失败，就把该计划当成
		 * 非简单。成功时我们取得新计划的引用计数，
		 * 存入 simple_eval_resowner。
		 */
		if (exec_is_simple_query(expr) &&
			CachedPlanAllowsSimpleValidityCheck(expr->expr_simple_plansource,
												cplan,
												estate->simple_eval_resowner))
		{
			/* 记住我们已持有引用计数 */
			expr->expr_simple_plan = cplan;
			expr->expr_simple_plan_lxid = curlxid;
		}
		else
		{
			/* 释放 SPI_plan_get_cached_plan 计数 */
			ReleaseCachedPlan(cplan, CurrentResourceOwner);
			return false;
		}

		/*
		 * SPI_plan_get_cached_plan 取得的计划引用计数存放在
		 * 活跃所有者里。我们不再需要它，故释放。
		 */
		ReleaseCachedPlan(cplan, CurrentResourceOwner);

		/* 从缓存计划抽出想要的标量表达式 */
		exec_save_simple_expr(expr, cplan);
	}

	/*
	 * 把先前确定的结果类型传回去。
	 */
	*rettype = expr->expr_simple_type;
	*rettypmod = expr->expr_simple_typmod;

	/*
	 * 建 ParamListInfo 给执行器。安全起见，保存并恢复
	 * 使用参数列表前后的 estate->paramLI->parserSetupArg。
	 */
	paramLI = estate->paramLI;
	save_setup_arg = paramLI->parserSetupArg;

	/*
	 * 我们可以不调用 setup_param_list()，只无条件地
	 * 这么做，因为不必做那种优化：把
	 * ecxt_param_list_info 可能设成 NULL；我们已强制使用
	 * 通用计划。
	 */
	paramLI->parserSetupArg = (void *) expr;
	econtext->ecxt_param_list_info = paramLI;

	/*
	 * 若当前还没准备好，就准备执行该表达式，于
	 * 当前事务。（若上面调用了 exec_save_simple_expr，则
	 * 此事将被迫发生。）
	 */
	if (unlikely(expr->expr_simple_lxid != curlxid))
	{
		oldcontext = MemoryContextSwitchTo(estate->simple_eval_estate->es_query_cxt);
		expr->expr_simple_state =
			ExecInitExprWithParams(expr->expr_simple_expr,
								   econtext->ecxt_param_list_info);
		expr->expr_simple_in_use = false;
		expr->expr_simple_lxid = curlxid;
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * 我们必须做一些 SPI_execute_plan 将做的事，尤其
	 * 是压入新快照，让表达式里的稳定函数能看见
	 * 我们自己的函数至今做的更新。不过有些情形
	 * 我们可以跳过（只用传给本函数的同一快照去
	 * 调用该表达式），这很有用，因为相对一条简单
	 * 表达式的代价，这一步相当贵。若表达式里没有
	 * 稳定函数，也没有易变函数，就能跳过；不变函数
	 * 不该需要看见我们的更新。另外，若这是只读
	 * 函数，我们并未做过任何更新，故再次可以
	 * 跳过
	 */
	oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
	need_snapshot = (expr->expr_simple_mutable && !estate->readonly_func);
	if (need_snapshot)
	{
		CommandCounterIncrement();
		PushActiveSnapshot(GetTransactionSnapshot());
	}

	/*
	 * 在 ExecEvalExpr 调用期间把该表达式标成忙碌。
	 */
	expr->expr_simple_in_use = true;

	/*
	 * 最后我们就能调用执行器来对该表达式求值
	 */
	*result = ExecEvalExpr(expr->expr_simple_state,
						   econtext,
						   isNull);

	/* 各项清理 */
	expr->expr_simple_in_use = false;

	econtext->ecxt_param_list_info = NULL;

	paramLI->parserSetupArg = save_setup_arg;

	if (need_snapshot)
		PopActiveSnapshot();

	MemoryContextSwitchTo(oldcontext);

	/*
	 * 完毕。
	 */
	return true;
}


/*
 * 建一个 ParamListInfo 传给 SPI
 *
 * 我们用同一个 ParamListInfo 结构，供本 estate 中为求值
 * PLpgSQL_exprs 而做的全部 SPI 调用。它没有逐参数数据，
 * 只有钩子函数，故对SPI实际上只读。
 *
 * 对纯只读的一处例外是 parserSetupArg 指向正在
 * 求值的那个具体 PLpgSQL_expr。对语句级调用者这不成
 * 问题，但更低层的调用者必须保存并恢复
 * estate->paramLI->parserSetupArg，以防外层正有一次活跃
 * 求值。（说得通的替代是为每个 PLpgSQL_expr
 * 各建一个 ParamListInfo，但眼下那
 * 像是浪费内存。）
 */
static ParamListInfo
setup_param_list(PLpgSQL_execstate *estate, PLpgSQL_expr *expr)
{
	ParamListInfo paramLI;

	/*
	 * 我们必须已经建好 SPIPlan（故查询文本已经
	 * 至少解析分析过一次）；否则无法依赖 expr->paramnos。
	 */
	Assert(expr->plan != NULL);

	/*
	 * 只有表达式带参数时，我们才需要 ParamListInfo。
	 */
	if (!bms_is_empty(expr->paramnos))
	{
		/* 用共用 ParamListInfo */
		paramLI = estate->paramLI;

		/*
		 * 设链接到活跃表达式，钩子能在那找到它。
		 * 若有任何可能打断参数的活跃使用，调用者必须
		 * 保存并恢复 parserSetupArg。
		 */
		paramLI->parserSetupArg = (void *) expr;

		/*
		 * 还要在解析器钩子需要它之前确保已设好。一旦
		 * 设上，该值总是对的，故不必保存再恢复。
		 * 本该已经设好，但让我们确认一下。
		 */
		expr->func = estate->func;
	}
	else
	{
		/*
		 * 表达式不需要参数。务必把这种情况表示成
		 * NULL 的 ParamListInfo，好让 plancache.c 知道没有
		 * 定制计划意义。
		 */
		paramLI = NULL;
	}
	return paramLI;
}

/*
 * plpgsql_param_fetch 动态取参数的 paramFetch 回调
 *
 * 我们总是用调用者的工作区来构造返回的结构。
 *
 * 注意：查询执行期间已不再用它。它用于计划
 * 阶段（speculative 为真时），以及当我们提供的
 * ParamListInfo 被拷进游标门户，还是传到某个
 * 并行子进程时。
 */
static ParamExternData *
plpgsql_param_fetch(ParamListInfo params,
					int paramid, bool speculative,
					ParamExternData *prm)
{
	int			dno;
	PLpgSQL_execstate *estate;
	PLpgSQL_expr *expr;
	PLpgSQL_datum *datum;
	bool		ok = true;
	int32		prmtypmod;

	/* paramid 从 1 起，dno 从 0 起 */
	dno = paramid - 1;
	Assert(dno >= 0 && dno < params->numParams);

	/* 取回钩子数据 */
	estate = (PLpgSQL_execstate *) params->paramFetchArg;
	expr = (PLpgSQL_expr *) params->parserSetupArg;
	Assert(params->numParams == estate->ndatums);

	/* 现在可以访问目标 datum */
	datum = estate->datums[dno];

	/*
	 * 因 copyParamList() 还是 SerializeParamList() 将试图
	 * 物化每一个参数槽，故当被问到一个不该被这条
	 * SQL 表达式使用的 datum 时，返回假参数很重要。
	 * 否则我们可能在 exec_eval_datum() 中失败，还是
	 * 拷贝远多过必要的数据。
	 */
	if (!bms_is_member(dno, expr->paramnos))
		ok = false;

	/*
	 * 若访问是推测性的，我们宁可不给数据，
	 * 也不在 exec_eval_datum() 失败。查可能的失败处。
	 */
	else if (speculative)
	{
		switch (datum->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
			case PLPGSQL_DTYPE_PROMISE:
				/* 总安全 */
				break;

			case PLPGSQL_DTYPE_ROW:
				/* 在所有要紧的情形里都应安全 */
				break;

			case PLPGSQL_DTYPE_REC:
				/* 总是安全（可能返回 NULL，无妨） */
				break;

			case PLPGSQL_DTYPE_RECFIELD:
				{
					PLpgSQL_recfield *recfield = (PLpgSQL_recfield *) datum;
					PLpgSQL_rec *rec;

					rec = (PLpgSQL_rec *) (estate->datums[recfield->recparentno]);

					/*
					 * 记录变量若 NULL，别冒任何风险。
					 */
					if (rec->erh == NULL)
						ok = false;

					/*
					 * 若尚未查过字段属性，或
					 * 元组描述符标识自上次变了。
					 */
					else if (unlikely(recfield->rectupledescid != rec->erh->er_tupdesc_id))
					{
						if (expanded_record_lookup_field(rec->erh,
														 recfield->fieldname,
														 &recfield->finfo))
							recfield->rectupledescid = rec->erh->er_tupdesc_id;
						else
							ok = false;
					}
					break;
				}

			default:
				ok = false;
				break;
		}
	}

	/* 若不行就返回"无此参数" */
	if (!ok)
	{
		prm->value = (Datum) 0;
		prm->isnull = true;
		prm->pflags = 0;
		prm->ptype = InvalidOid;
		return prm;
	}

	/* 好，求值并把值存进返回结构 */
	exec_eval_datum(estate, datum,
					&prm->ptype, &prmtypmod,
					&prm->value, &prm->isnull);
	/* 为执行器着想，我们总能把参数标成"const" */
	prm->pflags = PARAM_FLAG_CONST;

	/*
	 * 若它是读写扩展 datum，就把引用转成只读。
	 * 试图优化读写参数意义不大，鉴于本函数
	 * 被用到的那些情形。
	 */
	if (datum->dtype == PLPGSQL_DTYPE_VAR)
		prm->value = MakeExpandedObjectReadOnly(prm->value,
												prm->isnull,
												((PLpgSQL_var *) datum)->datatype->typlen);
	else if (datum->dtype == PLPGSQL_DTYPE_REC)
		prm->value = MakeExpandedObjectReadOnly(prm->value,
												prm->isnull,
												-1);

	return prm;
}

/*
 * plpgsql_param_compile：plpgsql 参数的 paramCompile 回调
 */
static void
plpgsql_param_compile(ParamListInfo params, Param *param,
					  ExprState *state,
					  Datum *resv, bool *resnull)
{
	PLpgSQL_execstate *estate;
	PLpgSQL_expr *expr;
	int			dno;
	PLpgSQL_datum *datum;
	ExprEvalStep scratch;

	/* 取回钩子数据 */
	estate = (PLpgSQL_execstate *) params->paramFetchArg;
	expr = (PLpgSQL_expr *) params->parserSetupArg;

	/* paramid 从 1 起，dno 从 0 起 */
	dno = param->paramid - 1;
	Assert(dno >= 0 && dno < estate->ndatums);

	/* 现在可以访问目标 datum */
	datum = estate->datums[dno];

	scratch.opcode = EEOP_PARAM_CALLBACK;
	scratch.resvalue = resv;
	scratch.resnull = resnull;

	/*
	 * 选择合适的求值函数。为了性能，特判
	 * DTYPE_VAR 与 DTYPE_RECFIELD 似乎值得。我们也能事先
	 * 断定是否需要 MakeExpandedObjectReadOnly()。
	 * 目前只有 VAR/PROMISE 与 REC 这些 datum 可能含读写
	 * 扩展对象。
	 */
	if (datum->dtype == PLPGSQL_DTYPE_VAR)
	{
		if (param != expr->expr_rw_param &&
			((PLpgSQL_var *) datum)->datatype->typlen == -1)
			scratch.d.cparam.paramfunc = plpgsql_param_eval_var_ro;
		else
			scratch.d.cparam.paramfunc = plpgsql_param_eval_var;
	}
	else if (datum->dtype == PLPGSQL_DTYPE_RECFIELD)
		scratch.d.cparam.paramfunc = plpgsql_param_eval_recfield;
	else if (datum->dtype == PLPGSQL_DTYPE_PROMISE)
	{
		if (param != expr->expr_rw_param &&
			((PLpgSQL_var *) datum)->datatype->typlen == -1)
			scratch.d.cparam.paramfunc = plpgsql_param_eval_generic_ro;
		else
			scratch.d.cparam.paramfunc = plpgsql_param_eval_generic;
	}
	else if (datum->dtype == PLPGSQL_DTYPE_REC &&
			 param != expr->expr_rw_param)
		scratch.d.cparam.paramfunc = plpgsql_param_eval_generic_ro;
	else
		scratch.d.cparam.paramfunc = plpgsql_param_eval_generic;

	/*
	 * 注意：想用 paramarg 存下 estate 指针，从而在求值
	 * 函数里省掉一两次间接寻址，这很诱人。但那
	 * 行不通，因为编译好的表达式可能配上不同的
	 * estate，却用于同一个 PL/pgSQL 函数。
	 */
	scratch.d.cparam.paramarg = NULL;
	scratch.d.cparam.paramid = param->paramid;
	scratch.d.cparam.paramtype = param->paramtype;
	ExprEvalPushStep(state, &scratch);
}

/*
 * plpgsql_param_eval_var 对 EEOP_PARAM_CALLBACK 步求值
 *
 * 这专用于这样一种 DTYPE_VAR 变量：
 * 我们不必调用 MakeExpandedObjectReadOnly。
 */
static void
plpgsql_param_eval_var(ExprState *state, ExprEvalStep *op,
					   ExprContext *econtext)
{
	ParamListInfo params;
	PLpgSQL_execstate *estate;
	int			dno = op->d.cparam.paramid - 1;
	PLpgSQL_var *var;

	/* 取回钩子数据 */
	params = econtext->ecxt_param_list_info;
	estate = (PLpgSQL_execstate *) params->paramFetchArg;
	Assert(dno >= 0 && dno < estate->ndatums);

	/* 现在可以访问目标 datum */
	var = (PLpgSQL_var *) estate->datums[dno];
	Assert(var->dtype == PLPGSQL_DTYPE_VAR);

	/* exec_eval_datum() 的内联版 */
	*op->resvalue = var->value;
	*op->resnull = var->isnull;

	/* 安全检查，有断言就够了 */
	Assert(var->datatype->typoid == op->d.cparam.paramtype);
}

/*
 * plpgsql_param_eval_var_ro 对 EEOP_PARAM_CALLBACK 步求值
 *
 * 这专用于这样一种 DTYPE_VAR 变量：
 * 我们要调用 MakeExpandedObjectReadOnly。
 */
static void
plpgsql_param_eval_var_ro(ExprState *state, ExprEvalStep *op,
						  ExprContext *econtext)
{
	ParamListInfo params;
	PLpgSQL_execstate *estate;
	int			dno = op->d.cparam.paramid - 1;
	PLpgSQL_var *var;

	/* 取回钩子数据 */
	params = econtext->ecxt_param_list_info;
	estate = (PLpgSQL_execstate *) params->paramFetchArg;
	Assert(dno >= 0 && dno < estate->ndatums);

	/* 现在可以访问目标 datum */
	var = (PLpgSQL_var *) estate->datums[dno];
	Assert(var->dtype == PLPGSQL_DTYPE_VAR);

	/*
	 * exec_eval_datum() 的内联版……顺便把
	 * 扩展 datum 转只读。
	 */
	*op->resvalue = MakeExpandedObjectReadOnly(var->value,
											   var->isnull,
											   -1);
	*op->resnull = var->isnull;

	/* 安全检查，有断言就够了 */
	Assert(var->datatype->typoid == op->d.cparam.paramtype);
}

/*
 * plpgsql_param_eval_recfield 对 EEOP_PARAM_CALLBACK 步求值
 *
 * 这专用于 DTYPE_RECFIELD 变量，对它们
 * 我们从不需调用 MakeExpandedObjectReadOnly。
 */
static void
plpgsql_param_eval_recfield(ExprState *state, ExprEvalStep *op,
							ExprContext *econtext)
{
	ParamListInfo params;
	PLpgSQL_execstate *estate;
	int			dno = op->d.cparam.paramid - 1;
	PLpgSQL_recfield *recfield;
	PLpgSQL_rec *rec;
	ExpandedRecordHeader *erh;

	/* 取回钩子数据 */
	params = econtext->ecxt_param_list_info;
	estate = (PLpgSQL_execstate *) params->paramFetchArg;
	Assert(dno >= 0 && dno < estate->ndatums);

	/* 现在可以访问目标 datum */
	recfield = (PLpgSQL_recfield *) estate->datums[dno];
	Assert(recfield->dtype == PLPGSQL_DTYPE_RECFIELD);

	/* 内联 exec_eval_datum 里相关的部分 */
	rec = (PLpgSQL_rec *) (estate->datums[recfield->recparentno]);
	erh = rec->erh;

	/*
	 * 记录变量若为 NULL，就在它有
	 * 具名复合类型则建，否则报错。（这不改记录的
	 * 逻辑状态：仍NULL）
	 */
	if (erh == NULL)
	{
		instantiate_empty_record_variable(estate, rec);
		erh = rec->erh;
	}

	/*
	 * 若尚未查过字段属性，或元组
	 * 描述符标识自上次变了。
	 */
	if (unlikely(recfield->rectupledescid != erh->er_tupdesc_id))
	{
		if (!expanded_record_lookup_field(erh,
										  recfield->fieldname,
										  &recfield->finfo))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("record \"%s\" has no field \"%s\"",
							rec->refname, recfield->fieldname)));
		recfield->rectupledescid = erh->er_tupdesc_id;
	}

	/* 可以取该字段的值。 */
	*op->resvalue = expanded_record_get_field(erh,
											  recfield->finfo.fnumber,
											  op->resnull);

	/* 安全检查，例如记录字段需要 */
	if (unlikely(recfield->finfo.ftypeid != op->d.cparam.paramtype))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
						op->d.cparam.paramid,
						format_type_be(recfield->finfo.ftypeid),
						format_type_be(op->d.cparam.paramtype))));
}

/*
 * plpgsql_param_eval_generic 对 EEOP_PARAM_CALLBACK 步求值
 *
 * 这处理全部变量类型，但假定我们不必调用
 * MakeExpandedObjectReadOnly
 */
static void
plpgsql_param_eval_generic(ExprState *state, ExprEvalStep *op,
						   ExprContext *econtext)
{
	ParamListInfo params;
	PLpgSQL_execstate *estate;
	int			dno = op->d.cparam.paramid - 1;
	PLpgSQL_datum *datum;
	Oid			datumtype;
	int32		datumtypmod;

	/* 取回钩子数据 */
	params = econtext->ecxt_param_list_info;
	estate = (PLpgSQL_execstate *) params->paramFetchArg;
	Assert(dno >= 0 && dno < estate->ndatums);

	/* 现在可以访问目标 datum */
	datum = estate->datums[dno];

	/* 取出 datum 的值 */
	exec_eval_datum(estate, datum,
					&datumtype, &datumtypmod,
					op->resvalue, op->resnull);

	/* 安全检查，例如记录字段需要 */
	if (unlikely(datumtype != op->d.cparam.paramtype))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
						op->d.cparam.paramid,
						format_type_be(datumtype),
						format_type_be(op->d.cparam.paramtype))));
}

/*
 * plpgsql_param_eval_generic_ro 对 EEOP_PARAM_CALLBACK 步求值
 *
 * 这处理全部变量类型，但假定我们需要调用
 * MakeExpandedObjectReadOnly（故变量必须是变长类型）。
 */
static void
plpgsql_param_eval_generic_ro(ExprState *state, ExprEvalStep *op,
							  ExprContext *econtext)
{
	ParamListInfo params;
	PLpgSQL_execstate *estate;
	int			dno = op->d.cparam.paramid - 1;
	PLpgSQL_datum *datum;
	Oid			datumtype;
	int32		datumtypmod;

	/* 取回钩子数据 */
	params = econtext->ecxt_param_list_info;
	estate = (PLpgSQL_execstate *) params->paramFetchArg;
	Assert(dno >= 0 && dno < estate->ndatums);

	/* 现在可以访问目标 datum */
	datum = estate->datums[dno];

	/* 取出 datum 的值 */
	exec_eval_datum(estate, datum,
					&datumtype, &datumtypmod,
					op->resvalue, op->resnull);

	/* 安全检查，例如记录字段需要 */
	if (unlikely(datumtype != op->d.cparam.paramtype))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
						op->d.cparam.paramid,
						format_type_be(datumtype),
						format_type_be(op->d.cparam.paramtype))));

	/* 把该值强制成只读 */
	*op->resvalue = MakeExpandedObjectReadOnly(*op->resvalue,
											   *op->resnull,
											   -1);
}


/*
 * exec_move_row 把一个元组的值移入记录还是行
 *
 * tup 与 tupdesc 都可能为 NULL，若只是在赋一个不确定的
 * 复合 NULL 给目标。另一做法是让 tup 为 NULL，且
 * tupdesc 不是 NULL，此时就给目标赋一行 NULL。
 *
 * 这里用 mcontext 当工作区，调用者最终应调用
 * exec_eval_cleanup，免得长期内存泄漏。
 */
static void
exec_move_row(PLpgSQL_execstate *estate,
			  PLpgSQL_variable *target,
			  HeapTuple tup, TupleDesc tupdesc)
{
	ExpandedRecordHeader *newerh = NULL;

	/*
	 * 若目标是 RECORD，我们也许能避开逐字段处理。
	 */
	if (target->dtype == PLPGSQL_DTYPE_REC)
	{
		PLpgSQL_rec *rec = (PLpgSQL_rec *) target;

		/*
		 * 若没有源 tupdesc，就把记录变量设为 NULL。
		 * 若有源 tupdesc 却没有元组，就把该
		 * 变量改成一行 NULL。这也许古怪，但
		 * 仍向后兼容。
		 */
		if (tupdesc == NULL)
		{
			if (rec->datatype &&
				rec->datatype->typtype == TYPTYPE_DOMAIN)
			{
				/*
				 * 若它是复合域，NULL 也许并非合法
				 * 值，故改为建一个空的扩展记录
				 * 并确保域类型检查得以完成。若已
				 * 有扩展记录，就搭上它已做的查找。
				 */
				newerh = make_expanded_record_for_rec(estate, rec,
													  NULL, rec->erh);
				expanded_record_set_tuple(newerh, NULL, false, false);
				assign_record_var(estate, rec, newerh);
			}
			else
			{
				/* 只把它清成 NULL */
				if (rec->erh)
					DeleteExpandedObject(ExpandedRecordGetDatum(rec->erh));
				rec->erh = NULL;
			}
			return;
		}

		/*
		 * 按合适的 tupdesc 建一个新扩展记录。
		 */
		newerh = make_expanded_record_for_rec(estate, rec, tupdesc, NULL);

		/*
		 * 要么行类型相符，要么本来就没有元组，就能
		 * 不必逐字段处理，直接完成赋值。
		 *
		 * 这里的检测大致按从便宜到贵排列。我们
		 * 能轻易看出可行，只要目标声明为 RECORD
		 * 还是 typeid 与源相同。但从查询
		 * 结果赋值时，源 tupdesc 常被标成 RECORD
		 * 但实际上与具名复合类型目标物理相容，
		 * 所以值得多花些周期去检查。
		 */
		if (rec->rectypeid == RECORDOID ||
			rec->rectypeid == tupdesc->tdtypeid ||
			!HeapTupleIsValid(tup) ||
			compatible_tupdescs(tupdesc, expanded_record_get_tupdesc(newerh)))
		{
			if (!HeapTupleIsValid(tup))
			{
				/* 无数据，故把记录强行置成全 NULL 态 */
				deconstruct_expanded_record(newerh);
			}
			else
			{
				/* 不必强制，直接赋上该行的值 */
				expanded_record_set_tuple(newerh, tup, true, !estate->atomic);
			}

			/* 完成这次赋值 */
			assign_record_var(estate, rec, newerh);

			return;
		}
	}

	/*
	 * 否则拆开该元组并逐字段赋值，
	 * 靠 exec_move_row_from_fields
	 */
	if (tupdesc && HeapTupleIsValid(tup))
	{
		int			td_natts = tupdesc->natts;
		Datum	   *values;
		bool	   *nulls;
		Datum		values_local[64];
		bool		nulls_local[64];

		/*
		 * 需要工作区数组。若 td_natts 够小，就靠局部
		 * 用数组以免 palloc。即便不小，也能
		 * 一次 palloc 同时分出 Datum 与 isnull 数组。
		 */
		if (td_natts <= lengthof(values_local))
		{
			values = values_local;
			nulls = nulls_local;
		}
		else
		{
			char	   *chunk;

			chunk = eval_mcontext_alloc(estate,
										td_natts * (sizeof(Datum) + sizeof(bool)));
			values = (Datum *) chunk;
			nulls = (bool *) (chunk + td_natts * sizeof(Datum));
		}

		heap_deform_tuple(tup, tupdesc, values, nulls);

		exec_move_row_from_fields(estate, target, newerh,
								  values, nulls, tupdesc);
	}
	else
	{
		/*
		 * 赋上全 NULL。
		 */
		exec_move_row_from_fields(estate, target, newerh,
								  NULL, NULL, NULL);
	}
}

/*
 * 核实 PLpgSQL_rec 的 rectypeid 已是最新。
 */
static void
revalidate_rectypeid(PLpgSQL_rec *rec)
{
	PLpgSQL_type *typ = rec->datatype;
	TypeCacheEntry *typentry;

	if (rec->rectypeid == RECORDOID)
		return;					/* 它是 RECORD，无需动作 */
	Assert(typ != NULL);
	if (typ->tcache &&
		typ->tcache->tupDesc_identifier == typ->tupdesc_id)
	{
		/*
		 * 虽已知 *typ 最新，rectypeid 仍可能
		 * 并非如此，因每次函数启动都从一份
		 * 没有好办法更新的副本克隆 *rec。故强制
		 * 修 rectypeid 再返回。
		 */
		rec->rectypeid = typ->typoid;
		return;
	}

	/*
	 * typcache 项已失效，故重新查找类型名，
	 * 若办得到，再复查类型 OID。若没有
	 * TypeName，就只能拿现有 OID 硬撑下去。
	 */
	if (typ->origtypname != NULL)
	{
		/* 这段应匹配 pl_gram.y 里的 parse_datatype() */
		typenameTypeIdAndMod(NULL, typ->origtypname,
							 &typ->typoid,
							 &typ->atttypmod);
	}

	/* 这段应匹配 pl_comp.c 里的 build_datatype() */
	typentry = lookup_type_cache(typ->typoid,
								 TYPECACHE_TUPDESC |
								 TYPECACHE_DOMAIN_BASE_INFO);
	if (typentry->typtype == TYPTYPE_DOMAIN)
		typentry = lookup_type_cache(typentry->domainBaseType,
									 TYPECACHE_TUPDESC);
	if (typentry->tupDesc == NULL)
	{
		/*
		 * 若走到这里，用户试图把复合类型换成
		 * 非复合类型。我们不支持那样。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("type %s is not composite",
						format_type_be(typ->typoid))));
	}

	/*
	 * 更新 tcache 与 tupdesc_id。因不支持改成
	 * 非复合类型，*typ 其余部分都不必改。
	 */
	typ->tcache = typentry;
	typ->tupdesc_id = typentry->tupDesc_identifier;

	/*
	 * 也更新 *rec。（附属 RECFIELDs 按需再处理。）
	 */
	rec->rectypeid = typ->typoid;
}

/*
 * 建一个适于赋给 rec 的扩展记录对象。
 *
 * 调用者必须二择一：源元组描述符，要么源扩展
 * 记录（不能都给）。若记录变量声明为 RECORD，
 * 它将采纳源的行类型。即便不采纳，也可能
 * 搭源扩展记录的便车，省一次 typcache 查找。
 *
 * 调用者必须填入数据，再调用 assign_record_var()。
 *
 * 新记录起初放进 mcontext，因此若于到达
 * assign_record_var() 前失败即被清掉。
 */
static ExpandedRecordHeader *
make_expanded_record_for_rec(PLpgSQL_execstate *estate,
							 PLpgSQL_rec *rec,
							 TupleDesc srctupdesc,
							 ExpandedRecordHeader *srcerh)
{
	ExpandedRecordHeader *newerh;
	MemoryContext mcontext = get_eval_mcontext(estate);

	if (rec->rectypeid != RECORDOID)
	{
		/*
		 * 使用前确保 rec->rectypeid 是最新的。
		 */
		revalidate_rectypeid(rec);

		/*
		 * 新记录必须是所需类型，但也许 srcerh 已经
		 * 做过全部相同查找
		 */
		if (srcerh && rec->rectypeid == srcerh->er_decltypeid)
			newerh = make_expanded_record_from_exprecord(srcerh,
														 mcontext);
		else
			newerh = make_expanded_record_from_typeid(rec->rectypeid, -1,
													  mcontext);
	}
	else
	{
		/*
		 * 我们将采纳输入 tupdesc。仍可用
		 * make_expanded_record_from_exprecord，若 srcerh 非复合
		 * 域。（若是，实际采纳的是其基类型。）
		 */
		if (srcerh && !ExpandedRecordIsDomain(srcerh))
			newerh = make_expanded_record_from_exprecord(srcerh,
														 mcontext);
		else
		{
			if (!srctupdesc)
				srctupdesc = expanded_record_get_tupdesc(srcerh);
			newerh = make_expanded_record_from_tupdesc(srctupdesc,
													   mcontext);
		}
	}

	return newerh;
}

/*
 * exec_move_row_from_fields 把字段值数组移入记录还是行
 *
 * 赋给记录时，调用者必须已经建好一个合适的
 * 新扩展记录对象 newerh。赋给行时传入 NULL。
 *
 * tupdesc 描述输入行，其列可能与目标的
 * 类型不同，已删列的位置也不同。
 * 若只想赋 NULL，values/nulls/tupdesc 都可以为 NULL，赋给
 * 记录还是行的各字段。
 *
 * 这里用 mcontext 当工作区，调用者最终应调用
 * exec_eval_cleanup，免得长期内存泄漏。
 */
static void
exec_move_row_from_fields(PLpgSQL_execstate *estate,
						  PLpgSQL_variable *target,
						  ExpandedRecordHeader *newerh,
						  Datum *values, bool *nulls,
						  TupleDesc tupdesc)
{
	int			td_natts = tupdesc ? tupdesc->natts : 0;
	int			fnum;
	int			anum;
	int			strict_multiassignment_level = 0;

	/*
	 * 额外检查 strict_multi_assignment 仅能在指定了
	 * 输入 tupdesc 时启用。
	 */
	if (tupdesc != NULL)
	{
		if (plpgsql_extra_errors & PLPGSQL_XCHECK_STRICTMULTIASSIGNMENT)
			strict_multiassignment_level = ERROR;
		else if (plpgsql_extra_warnings & PLPGSQL_XCHECK_STRICTMULTIASSIGNMENT)
			strict_multiassignment_level = WARNING;
	}

	/* 处理 RECORD 目标 */
	if (target->dtype == PLPGSQL_DTYPE_REC)
	{
		PLpgSQL_rec *rec = (PLpgSQL_rec *) target;
		TupleDesc	var_tupdesc;
		Datum		newvalues_local[64];
		bool		newnulls_local[64];

		Assert(newerh != NULL); /* 调用者必须已建好新对象 */

		var_tupdesc = expanded_record_get_tupdesc(newerh);

		/*
		 * 需要时强制字段值。这可能要对付
		 * 不同的已删列集合，以及强制各个列
		 * 的类型。这有点麻烦，但 plpgsql 历来允许
		 * 这样做，故我们保留该行为。不过值得快速
		 * 查一下这些 tupdesc 是否相同。（因 expandedrecord.c
		 * 优先用 typcache 里带引用计数的描述符，故扩展
		 * 记录若行类型相同，其 tupdesc 指针相等。）
		 */
		if (var_tupdesc != tupdesc)
		{
			int			vtd_natts = var_tupdesc->natts;
			Datum	   *newvalues;
			bool	   *newnulls;

			/*
			 * 需要工作区数组。若 vtd_natts 够小，就靠局部
			 * 用数组以免 palloc。即便不小，也能
			 * 一次 palloc 同时分出 Datum 与 isnull 数组。
			 */
			if (vtd_natts <= lengthof(newvalues_local))
			{
				newvalues = newvalues_local;
				newnulls = newnulls_local;
			}
			else
			{
				char	   *chunk;

				chunk = eval_mcontext_alloc(estate,
											vtd_natts * (sizeof(Datum) + sizeof(bool)));
				newvalues = (Datum *) chunk;
				newnulls = (bool *) (chunk + vtd_natts * sizeof(Datum));
			}

			/* 遍历目标各列 */
			anum = 0;
			for (fnum = 0; fnum < vtd_natts; fnum++)
			{
				Form_pg_attribute attr = TupleDescAttr(var_tupdesc, fnum);
				Datum		value;
				bool		isnull;
				Oid			valtype;
				int32		valtypmod;

				if (attr->attisdropped)
				{
					/* expanded_record_set_fields 应忽略此列 */
					continue;	/* 跳过记录中已删列 */
				}

				while (anum < td_natts &&
					   TupleDescAttr(tupdesc, anum)->attisdropped)
					anum++;		/* 跳过元组中已删列 */

				if (anum < td_natts)
				{
					value = values[anum];
					isnull = nulls[anum];
					valtype = TupleDescAttr(tupdesc, anum)->atttypid;
					valtypmod = TupleDescAttr(tupdesc, anum)->atttypmod;
					anum++;
				}
				else
				{
					/* 目标列没有来源 */
					value = (Datum) 0;
					isnull = true;
					valtype = UNKNOWNOID;
					valtypmod = -1;

					/* 当源值缺失时 */
					if (strict_multiassignment_level)
						ereport(strict_multiassignment_level,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("number of source and target fields in assignment does not match"),
						/* translator: %s represents a name of an extra check */
								 errdetail("%s check of %s is active.",
										   "strict_multi_assignment",
										   strict_multiassignment_level == ERROR ? "extra_errors" :
										   "extra_warnings"),
								 errhint("Make sure the query returns the exact list of columns.")));
				}

				/* 需要时把新值转换成正确类型。 */
				newvalues[fnum] = exec_cast_value(estate,
												  value,
												  &isnull,
												  valtype,
												  valtypmod,
												  attr->atttypid,
												  attr->atttypmod);
				newnulls[fnum] = isnull;
			}

			/*
			 * 额外检查 strict_multiassignment 启用时，则确保
			 * 没有未赋值的源属性。
			 */
			if (strict_multiassignment_level && anum < td_natts)
			{
				/* 跳过源描述符中的已删列 */
				while (anum < td_natts &&
					   TupleDescAttr(tupdesc, anum)->attisdropped)
					anum++;

				if (anum < td_natts)
					ereport(strict_multiassignment_level,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("number of source and target fields in assignment does not match"),
					/* translator: %s represents a name of an extra check */
							 errdetail("%s check of %s is active.",
									   "strict_multi_assignment",
									   strict_multiassignment_level == ERROR ? "extra_errors" :
									   "extra_warnings"),
							 errhint("Make sure the query returns the exact list of columns.")));
			}

			values = newvalues;
			nulls = newnulls;
		}

		/* 把强制后的字段值插入新扩展记录 */
		expanded_record_set_fields(newerh, values, nulls, !estate->atomic);

		/* 完成这次赋值 */
		assign_record_var(estate, rec, newerh);

		return;
	}

	/* 非 RECORD 情形不该传入 newerh */
	Assert(newerh == NULL);

	/*
	 * 对于行，我们把各字段值赋给该行所
	 * 指向变量
	 *
	 * 注意：这段与上面记录代码都默默忽略多余
	 * 的源列，并对缺失列假定为 NULL。这
	 * 相当可疑，不过这是历史行为。
	 *
	 * 若全然没有输入数据，就给行变量的全部列
	 * 赋上 NULL。
	 */
	if (target->dtype == PLPGSQL_DTYPE_ROW)
	{
		PLpgSQL_row *row = (PLpgSQL_row *) target;

		anum = 0;
		for (fnum = 0; fnum < row->nfields; fnum++)
		{
			PLpgSQL_var *var;
			Datum		value;
			bool		isnull;
			Oid			valtype;
			int32		valtypmod;

			var = (PLpgSQL_var *) (estate->datums[row->varnos[fnum]]);

			while (anum < td_natts &&
				   TupleDescAttr(tupdesc, anum)->attisdropped)
				anum++;			/* 跳过元组中已删列 */

			if (anum < td_natts)
			{
				value = values[anum];
				isnull = nulls[anum];
				valtype = TupleDescAttr(tupdesc, anum)->atttypid;
				valtypmod = TupleDescAttr(tupdesc, anum)->atttypmod;
				anum++;
			}
			else
			{
				/* 目标列没有来源 */
				value = (Datum) 0;
				isnull = true;
				valtype = UNKNOWNOID;
				valtypmod = -1;

				if (strict_multiassignment_level)
					ereport(strict_multiassignment_level,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("number of source and target fields in assignment does not match"),
					/* translator: %s represents a name of an extra check */
							 errdetail("%s check of %s is active.",
									   "strict_multi_assignment",
									   strict_multiassignment_level == ERROR ? "extra_errors" :
									   "extra_warnings"),
							 errhint("Make sure the query returns the exact list of columns.")));
			}

			exec_assign_value(estate, (PLpgSQL_datum *) var,
							  value, isnull, valtype, valtypmod);
		}

		/*
		 * 当额外检查 strict_multiassignment 启用时，确保
		 * 没有未赋值的源属性。
		 */
		if (strict_multiassignment_level && anum < td_natts)
		{
			while (anum < td_natts &&
				   TupleDescAttr(tupdesc, anum)->attisdropped)
				anum++;			/* 跳过元组中已删列 */

			if (anum < td_natts)
				ereport(strict_multiassignment_level,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("number of source and target fields in assignment does not match"),
				/* translator: %s represents a name of an extra check */
						 errdetail("%s check of %s is active.",
								   "strict_multi_assignment",
								   strict_multiassignment_level == ERROR ? "extra_errors" :
								   "extra_warnings"),
						 errhint("Make sure the query returns the exact list of columns.")));
		}

		return;
	}

	elog(ERROR, "unsupported target type: %d", target->dtype);
}

/*
 * compatible_tupdescs：检测两个 tupdesc 是否物理相容
 *
 * TRUE 表示满足 src_tupdesc 的元组可直接当作
 * 使用 dst_tupdesc 的复合变量的值。
 */
static bool
compatible_tupdescs(TupleDesc src_tupdesc, TupleDesc dst_tupdesc)
{
	int			i;

	/* 也许可以允许 src_tupdesc 再带额外的列？ */
	if (dst_tupdesc->natts != src_tupdesc->natts)
		return false;

	for (i = 0; i < dst_tupdesc->natts; i++)
	{
		Form_pg_attribute dattr = TupleDescAttr(dst_tupdesc, i);
		Form_pg_attribute sattr = TupleDescAttr(src_tupdesc, i);

		if (dattr->attisdropped != sattr->attisdropped)
			return false;
		if (!dattr->attisdropped)
		{
			/* 普通列必须按类型与 typmod 匹配 */
			if (dattr->atttypid != sattr->atttypid ||
				(dattr->atttypmod >= 0 &&
				 dattr->atttypmod != sattr->atttypmod))
				return false;
		}
		else
		{
			/* 已删列只要长度与对齐一致就可以 */
			if (dattr->attlen != sattr->attlen ||
				dattr->attalign != sattr->attalign)
				return false;
		}
	}
	return true;
}

/* ----------
 * make_tuple_from_row 用行对象的各值造出元组
 *
 * 返回 NULL 表示行类型不符；调用者必须报出合适错误
 *
 * 结果元组在调用者内存上下文中新 palloc。有些垃圾
 * 也许也留在 eval_mcontext 里。
 * ----------
 */
static HeapTuple
make_tuple_from_row(PLpgSQL_execstate *estate,
					PLpgSQL_row *row,
					TupleDesc tupdesc)
{
	int			natts = tupdesc->natts;
	HeapTuple	tuple;
	Datum	   *dvalues;
	bool	   *nulls;
	int			i;

	if (natts != row->nfields)
		return NULL;

	dvalues = (Datum *) eval_mcontext_alloc0(estate, natts * sizeof(Datum));
	nulls = (bool *) eval_mcontext_alloc(estate, natts * sizeof(bool));

	for (i = 0; i < natts; i++)
	{
		Oid			fieldtypeid;
		int32		fieldtypmod;

		if (TupleDescAttr(tupdesc, i)->attisdropped)
		{
			nulls[i] = true;	/* 将该列留为 NULL */
			continue;
		}

		exec_eval_datum(estate, estate->datums[row->varnos[i]],
						&fieldtypeid, &fieldtypmod,
						&dvalues[i], &nulls[i]);
		if (fieldtypeid != TupleDescAttr(tupdesc, i)->atttypid)
			return NULL;
		/* XXX 是否也该坚持 typmod 相符？ */
	}

	tuple = heap_form_tuple(tupdesc, dvalues, nulls);

	return tuple;
}

/*
 * deconstruct_composite_datum 从复合 Datum 抽出元组与描述符
 *
 * 调用者必须提供一个 HeapTupleData 变量，我们在其中建
 * 好指向复合 datum 体的元组头。要让该元组
 * 值活过那个变量，调用者就得再调用 heap_copytuple...
 * 但当前调用者反正只需要短命的元组值。
 *
 * 返回指向该 datum 行类型的 TupleDesc 的指针。
 * 调用者负责用完后调用 ReleaseTupleDesc。
 *
 * 注意：调用者负责确保值是复合类型。
 * 另，最好于短命内存上下文调用，因可能漏内存。
 */
static TupleDesc
deconstruct_composite_datum(Datum value, HeapTupleData *tmptup)
{
	HeapTupleHeader td;
	Oid			tupType;
	int32		tupTypmod;

	/* 取元组体（注意这可能要解 toast） */
	td = DatumGetHeapTupleHeader(value);

	/* 建一个临时 HeapTuple 控制结构 */
	tmptup->t_len = HeapTupleHeaderGetDatumLength(td);
	ItemPointerSetInvalid(&(tmptup->t_self));
	tmptup->t_tableOid = InvalidOid;
	tmptup->t_data = td;

	/* 抽出行类型信息并找到 tupdesc */
	tupType = HeapTupleHeaderGetTypeId(td);
	tupTypmod = HeapTupleHeaderGetTypMod(td);
	return lookup_rowtype_tupdesc(tupType, tupTypmod);
}

/*
 * exec_move_row_from_datum 把复合 Datum 移入记录还是行
 *
 * 这等价于 deconstruct_composite_datum() 之后再接
 * exec_move_row()；Datum 属扩展记录引用则优化
 * 即可。
 *
 * 注意：调用者负责确保值是复合类型。
 */
static void
exec_move_row_from_datum(PLpgSQL_execstate *estate,
						 PLpgSQL_variable *target,
						 Datum value)
{
	/* 检查源是不是一个扩展记录 */
	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(value)))
	{
		ExpandedRecordHeader *erh = (ExpandedRecordHeader *) DatumGetEOHP(value);
		ExpandedRecordHeader *newerh = NULL;

		Assert(erh->er_magic == ER_MAGIC);

		/* 以下情形适用于目标是记录而非行时 */
		if (target->dtype == PLPGSQL_DTYPE_REC)
		{
			PLpgSQL_rec *rec = (PLpgSQL_rec *) target;

			/*
			 * 若它就是变量里已经存着的同一记录，则
			 * 什么都不做。这只出在傻情形，像 r := r，
			 * 但仍需要一点检查，以免可能释放掉变量
			 * 下方仍活着的值。注意即便我们持有的是
			 * R/O 指针。
			 */
			if (erh == rec->erh)
				return;

			/*
			 * 使用前确保 rec->rectypeid 是最新的。
			 */
			revalidate_rectypeid(rec);

			/*
			 * 若持有 R/W 指针，就允许直接夺走
			 * 该扩展记录的所有权。若类型适合放进
			 * 记录变量，就这么做。（注意我们不接受
			 * 把复合域类型的扩展记录当作 RECORD
			 * 值。我们改把它当成基复合类型；
			 * 逻辑见 make_expanded_record_for_rec。）
			 */
			if (VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(value)) &&
				(rec->rectypeid == erh->er_decltypeid ||
				 (rec->rectypeid == RECORDOID &&
				  !ExpandedRecordIsDomain(erh))))
			{
				assign_record_var(estate, rec, erh);
				return;
			}

			/*
			 * 若目标变量里已有扩展记录对象
			 * 且源记录含合法元组
			 * 表示且行类型正确，于是我们可省去再造
			 * 扩展记录，只把元组赋进去，用
			 * expanded_record_set_tuple。（若必须逐字段赋值
			 * 就无法照样做，因出错时那样并不原子。）
			 * 我们认为有行类型匹配，
			 * 仅当是同一具名复合类型，或
			 * 同一已注册行类型；去查匿名行类型
			 * 是否匹配则太贵，不值当。
			 */
			if (rec->erh &&
				(erh->flags & ER_FLAG_FVALUE_VALID) &&
				erh->er_typeid == rec->erh->er_typeid &&
				(erh->er_typeid != RECORDOID ||
				 (erh->er_typmod == rec->erh->er_typmod &&
				  erh->er_typmod >= 0)))
			{
				expanded_record_set_tuple(rec->erh, erh->fvalue,
										  true, !estate->atomic);
				return;
			}

			/*
			 * 否则就需要一个新的扩展记录对象。造在
			 * 此处，指望能搭上源对象先前做过的
			 * typcache 查找。
			 */
			newerh = make_expanded_record_for_rec(estate, rec, NULL, erh);

			/*
			 * 若该扩展记录含有合法的元组表示，
			 * 且不需要做行类型转换，那就只拷贝该
			 * 元组，多半比逐字段处理更快。（这
			 * 并非重复先前那次检查，因为这里将
			 * 抓住记录变量先前为空的情形。）
			 */
			if ((erh->flags & ER_FLAG_FVALUE_VALID) &&
				(rec->rectypeid == RECORDOID ||
				 rec->rectypeid == erh->er_typeid))
			{
				expanded_record_set_tuple(newerh, erh->fvalue,
										  true, !estate->atomic);
				assign_record_var(estate, rec, newerh);
				return;
			}

			/*
			 * 空的源记录必须走特例，否则下文将
			 * 漏 newerh
			 */
			if (ExpandedRecordIsEmpty(erh))
			{
				/* 把 newerh 设成一行 NULL */
				deconstruct_expanded_record(newerh);
				assign_record_var(estate, rec, newerh);
				return;
			}
		}						/* 记录目标分支到此结束 */

		/*
		 * 若源扩展记录为空，应把它当成一个
		 * NULL 元组值。（不大可能见到，但必须
		 * 检查；deconstruct_expanded_record 将改变
		 * 逻辑状态，这不行。）
		 */
		if (ExpandedRecordIsEmpty(erh))
		{
			exec_move_row(estate, target, NULL,
						  expanded_record_get_tupdesc(erh));
			return;
		}

		/*
		 * 否则，确保源记录已经被拆开，并
		 * 按其字段值来赋值。
		 */
		deconstruct_expanded_record(erh);
		exec_move_row_from_fields(estate, target, newerh,
								  erh->dvalues, erh->dnulls,
								  expanded_record_get_tupdesc(erh));
	}
	else
	{
		/*
		 * 不，这是普通复合 Datum。拆开它；但我们
		 * 不用 deconstruct_composite_datum()，因我们可能
		 * 跳过 lookup_rowtype_tupdesc()。
		 */
		HeapTupleHeader td;
		HeapTupleData tmptup;
		Oid			tupType;
		int32		tupTypmod;
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		/* 确保任何解 toast 后的数据落进 eval_mcontext */
		oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
		/* 取元组体（注意这可能要解 toast） */
		td = DatumGetHeapTupleHeader(value);
		MemoryContextSwitchTo(oldcontext);

		/* 建一个临时 HeapTuple 控制结构 */
		tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
		ItemPointerSetInvalid(&(tmptup.t_self));
		tmptup.t_tableOid = InvalidOid;
		tmptup.t_data = td;

		/* 取行类型信息 */
		tupType = HeapTupleHeaderGetTypeId(td);
		tupTypmod = HeapTupleHeaderGetTypMod(td);

		/* 现在，若目标是记录而非行，也许可以优化 */
		if (target->dtype == PLPGSQL_DTYPE_REC)
		{
			PLpgSQL_rec *rec = (PLpgSQL_rec *) target;

			/*
			 * 若目标变量里已有扩展记录对象
			 * 且源 datum 的行类型匹配，则我们
			 * 可以省去新建扩展记录，只把元组赋进去，用
			 * expanded_record_set_tuple。行类型匹配，
			 * 仅当是同一具名复合类型，或
			 * 同一已注册行类型。（要拒绝匿名
			 * 行类型本属多余，但还是求稳。）
			 */
			if (rec->erh &&
				tupType == rec->erh->er_typeid &&
				(tupType != RECORDOID ||
				 (tupTypmod == rec->erh->er_typmod &&
				  tupTypmod >= 0)))
			{
				expanded_record_set_tuple(rec->erh, &tmptup,
										  true, !estate->atomic);
				return;
			}

			/*
			 * 若源 datum 的行类型与目标
			 * 变量相容，就建新扩展记录并把元组赋入
			 * 其中。这里用 make_expanded_record_from_typeid() 可省
			 * 一次 typcache 查找，比起下文。
			 */
			if (rec->rectypeid == RECORDOID || rec->rectypeid == tupType)
			{
				ExpandedRecordHeader *newerh;
				MemoryContext mcontext = get_eval_mcontext(estate);

				newerh = make_expanded_record_from_typeid(tupType, tupTypmod,
														  mcontext);
				expanded_record_set_tuple(newerh, &tmptup,
										  true, !estate->atomic);
				assign_record_var(estate, rec, newerh);
				return;
			}

			/*
			 * 否则就需要转换，因此直落到
			 * 用难的方式做
			 */
		}

		/*
		 * ROW 目标，还是无法优化的 RECORD，必须花一次
		 * 查找来取得源 datum 的 tupdesc。
		 */
		tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

		/* 做移动 */
		exec_move_row(estate, target, &tmptup, tupdesc);

		/* 释放 tupdesc 使用计数 */
		ReleaseTupleDesc(tupdesc);
	}
}

/*
 * 若尚未建立扩展记录来存放记录变量的
 * 值，就这么建。扩展记录将为"空"，故这并不
 * 改变记录的逻辑状态：它仍是 NULL。
 * 不过现在就有 tupdesc，例如可以用来查字段。
 */
static void
instantiate_empty_record_variable(PLpgSQL_execstate *estate, PLpgSQL_rec *rec)
{
	Assert(rec->erh == NULL);	/* 否则主调错 */

	/* 若声明类型是 RECORD，就无法实例化 */
	if (rec->rectypeid == RECORDOID)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("record \"%s\" is not assigned yet", rec->refname),
				 errdetail("The tuple structure of a not-yet-assigned record is indeterminate.")));

	/* 使用前确保 rec->rectypeid 是最新的。 */
	revalidate_rectypeid(rec);

	/* 好就做 */
	rec->erh = make_expanded_record_from_typeid(rec->rectypeid, -1,
												estate->datum_context);
}

/* ----------
 * convert_value_to_string 把非 NULL 的 Datum 转成 C 串
 *
 * 注意：结果在 estate 的 eval_mcontext 里，并将被清掉
 * 于下次 exec_eval_cleanup() 调用。被调输出函数可能
 * 也在那里再留废物，故只 pfree 结果串
 * 并不足以避免内存泄漏，若我们不这样做。
 * 多数用法里，传入的 Datum 也在那一内存上下文（若
 * 是按引用传递），故反正也要调用 exec_eval_cleanup()。
 *
 * 注意：不缓存转换函数查找，对性能不好。
 * 不过此函数当前并未用在多一两次
 * 目录查找就算大事的地方。
 * ----------
 */
static char *
convert_value_to_string(PLpgSQL_execstate *estate, Datum value, Oid valtype)
{
	char	   *result;
	MemoryContext oldcontext;
	Oid			typoutput;
	bool		typIsVarlena;

	oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
	getTypeOutputInfo(valtype, &typoutput, &typIsVarlena);
	result = OidOutputFunctionCall(typoutput, value);
	MemoryContextSwitchTo(oldcontext);

	return result;
}

/* ----------
 * exec_cast_value 需要时转换该值
 *
 * 注意 *isnull 既是输入也是输出参数。虽说
 * 一次转换不大可能从非 NULL 造出 NULL，反之亦
 * 然，原则上仍可能发生。
 *
 * 注意：estate 的 eval_mcontext 用于临时存储，并可能
 * 也盛放结果 Datum，若我们必须转换成按
 * 引用传递的数据类型。务必调用 exec_eval_cleanup()，在
 * 用完结果后。
 * ----------
 */
static inline Datum
exec_cast_value(PLpgSQL_execstate *estate,
				Datum value, bool *isnull,
				Oid valtype, int32 valtypmod,
				Oid reqtype, int32 reqtypmod)
{
	/*
	 * 若给定值的类型并非所要求的，就转换它。
	 */
	if (valtype != reqtype ||
		(valtypmod != reqtypmod && reqtypmod != -1))
	{
		/* 我们把慢路径留在线外。 */
		value = do_cast_value(estate, value, isnull, valtype, valtypmod,
							  reqtype, reqtypmod);
	}

	return value;
}

/* ----------
 * do_cast_value：exec_cast_value 的慢路径
 * ----------
 */
static Datum
do_cast_value(PLpgSQL_execstate *estate,
			  Datum value, bool *isnull,
			  Oid valtype, int32 valtypmod,
			  Oid reqtype, int32 reqtypmod)
{
	plpgsql_CastHashEntry *cast_entry;

	cast_entry = get_cast_hashentry(estate,
									valtype, valtypmod,
									reqtype, reqtypmod);
	if (cast_entry)
	{
		ExprContext *econtext = estate->eval_econtext;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));

		econtext->caseValue_datum = value;
		econtext->caseValue_isNull = *isnull;

		cast_entry->cast_in_use = true;

		value = ExecEvalExpr(cast_entry->cast_exprstate, econtext,
							 isnull);

		cast_entry->cast_in_use = false;

		MemoryContextSwitchTo(oldcontext);
	}

	return value;
}

/* ----------
 * get_cast_hashentry 查找如何做类型转换
 *
 * 若必须对表达式求值，就返回 plpgsql_CastHashEntry
 * 否则 NULL，当转换只是空操作重标。若还有工作待
 * 做，则 cast_exprstate 字段含一棵表达式求值树
 * 以 CaseTestExpr 为输入，且 cast_in_use 字段应设成
 * 执行它时为真。
 * ----------
 */
static plpgsql_CastHashEntry *
get_cast_hashentry(PLpgSQL_execstate *estate,
				   Oid srctype, int32 srctypmod,
				   Oid dsttype, int32 dsttypmod)
{
	plpgsql_CastHashKey cast_key;
	plpgsql_CastHashEntry *cast_entry;
	plpgsql_CastExprHashEntry *expr_entry;
	bool		found;
	LocalTransactionId curlxid;
	MemoryContext oldcontext;

	/* 查找已有项 */
	cast_key.srctype = srctype;
	cast_key.dsttype = dsttype;
	cast_key.srctypmod = srctypmod;
	cast_key.dsttypmod = dsttypmod;
	cast_entry = (plpgsql_CastHashEntry *) hash_search(estate->cast_hash,
													   &cast_key,
													   HASH_ENTER, &found);
	if (!found)					/* 若新项则初始化 */
	{
		/* 需要第二次查找，看 cast_expr_hash 项在不在 */
		expr_entry = (plpgsql_CastExprHashEntry *) hash_search(cast_expr_hash,
															   &cast_key,
															   HASH_ENTER,
															   &found);
		if (!found)				/* 新表达式项则初始化 */
			expr_entry->cast_cexpr = NULL;

		cast_entry->cast_centry = expr_entry;
		cast_entry->cast_exprstate = NULL;
		cast_entry->cast_in_use = false;
		cast_entry->cast_lxid = InvalidLocalTransactionId;
	}
	else
	{
		/* 靠始终有效的链接，省第二次散列查找 */
		expr_entry = cast_entry->cast_centry;
	}

	if (expr_entry->cast_cexpr == NULL ||
		!expr_entry->cast_cexpr->is_valid)
	{
		/*
		 * 我们先前没查过这回强制；查过的话，缓存的
		 * 表达式已经失效。
		 */
		Node	   *cast_expr;
		CachedExpression *cast_cexpr;
		CaseTestExpr *placeholder;

		/*
		 * 若有旧的缓存表达式就丢掉。
		 */
		if (expr_entry->cast_cexpr)
		{
			FreeCachedExpression(expr_entry->cast_cexpr);
			expr_entry->cast_cexpr = NULL;
		}

		/*
		 * 因很容易失败（无此强制），故造一个
		 * 临时强制表达式树，放在短命的
		 * eval_mcontext 里，成功再存成 CachedExpression。
		 */
		oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));

		/*
		 * 我们用 CaseTestExpr 当强制树的基底，因它
		 * 塞入源值非常便宜。
		 */
		placeholder = makeNode(CaseTestExpr);
		placeholder->typeId = srctype;
		placeholder->typeMod = srctypmod;
		placeholder->collation = get_typcollation(srctype);

		/*
		 * 施加强制。我们使用特殊的强制上下文
		 * COERCION_PLPGSQL，以符合 plpgsql 的历史行为，即
		 * ASSIGNMENT 层没有的任何转换，都将实现为
		 * 一次 I/O 强制。（有点可疑：我们偏好 I/O
		 * 强制，胜过 EXPLICIT 层已有的转换路径。改动
		 * 那样却带来种种细小行为差异，而且
		 * 想要显式转换行为的用户总能写一个
		 * 显式转换)
		 *
		 * 若源类型是 UNKNOWN，coerce_to_target_type 将失败（它只
		 * 指望在 Const 输入节点上见到它），故别调用；我们
		 * 改用 CoerceViaIO。同样，它目前也不能把
		 * RECORD 强制成别的类型，那种也跳过。
		 */
		if (srctype == UNKNOWNOID || srctype == RECORDOID)
			cast_expr = NULL;
		else
			cast_expr = coerce_to_target_type(NULL,
											  (Node *) placeholder, srctype,
											  dsttype, dsttypmod,
											  COERCION_PLPGSQL,
											  COERCE_IMPLICIT_CAST,
											  -1);

		/*
		 * 若按解析器没有转换路径，就退回使用
		 * 一次 I/O 强制；语义上可疑，但符合 plpgsql 的
		 * 历史行为。对 UNKNOWN 字面量总得如此。（
		 * 如今大概只在这种时候到得了：
		 * srctype 为 UNKNOWN/RECORD。）
		 */
		if (cast_expr == NULL)
		{
			CoerceViaIO *iocoerce = makeNode(CoerceViaIO);

			iocoerce->arg = (Expr *) placeholder;
			iocoerce->resulttype = dsttype;
			iocoerce->resultcollid = InvalidOid;
			iocoerce->coerceformat = COERCE_IMPLICIT_CAST;
			iocoerce->location = -1;
			cast_expr = (Node *) iocoerce;
			if (dsttypmod != -1)
				cast_expr = coerce_to_target_type(NULL,
												  cast_expr, dsttype,
												  dsttype, dsttypmod,
												  COERCION_ASSIGNMENT,
												  COERCE_IMPLICIT_CAST,
												  -1);
		}

		/* 注意：我们不费心给表达式树标上排序规则 */

		/* 规划该表达式并建出 CachedExpression */
		cast_cexpr = GetCachedExpression(cast_expr);
		cast_expr = cast_cexpr->expr;

		/* 检测这是不是空操作（RelabelType）强制 */
		if (IsA(cast_expr, RelabelType) &&
			((RelabelType *) cast_expr)->arg == (Expr *) placeholder)
			cast_expr = NULL;

		/* 现在可以填好表达式散列表的项。 */
		expr_entry->cast_cexpr = cast_cexpr;
		expr_entry->cast_expr = (Expr *) cast_expr;

		/* 也务必重置 exprstate 那一项散列表。 */
		cast_entry->cast_exprstate = NULL;
		cast_entry->cast_in_use = false;
		cast_entry->cast_lxid = InvalidLocalTransactionId;

		MemoryContextSwitchTo(oldcontext);
	}

	/* 若已判定这是空操作转换，就此结束。 */
	if (expr_entry->cast_expr == NULL)
		return NULL;

	/*
	 * 若当前还没准备好，就准备执行该表达式，于
	 * 当前事务里；另外，若它在当前
	 * 事务里标成忙，就丢掉那棵表达式树再新建，以便
	 * 避开递归转换与失败执行的问题。（若此类
	 * 情形很多，事务内将漏掉一些内存，
	 * 但我们并不指望那样。）更新散列表可以，因为
	 * 同一事务里全部 plpgsql 函数都共用着同一个
	 * simple_eval_estate。（不过，常规函数才
	 * 如此；DO 块有私有的 simple_eval_estates，以及私有
	 * 配套的转换散列表。）
	 */
	curlxid = MyProc->lxid;
	if (cast_entry->cast_lxid != curlxid || cast_entry->cast_in_use)
	{
		oldcontext = MemoryContextSwitchTo(estate->simple_eval_estate->es_query_cxt);
		cast_entry->cast_exprstate = ExecInitExpr(expr_entry->cast_expr, NULL);
		cast_entry->cast_in_use = false;
		cast_entry->cast_lxid = curlxid;
		MemoryContextSwitchTo(oldcontext);
	}

	return cast_entry;
}


/* ----------
 * exec_simple_check_plan：检查计划是否简单到能
 *	改由 ExecEvalExpr() 来求值
 *	而非 SPI。
 *
 * 注意：本函数的引用计数操作假定 expr->plan
 * 是已保存的 SPI 计划。从调用者看这有点讨厌，
 * 但除此很难避免在失败时泄漏该计划。
 * ----------
 */
static void
exec_simple_check_plan(PLpgSQL_execstate *estate, PLpgSQL_expr *expr)
{
	List	   *plansources;
	CachedPlanSource *plansource;
	CachedPlan *cplan;
	MemoryContext oldcontext;

	/*
	 * 初始化成非简单。
	 */
	expr->expr_simple_expr = NULL;
	expr->expr_rw_param = NULL;

	/*
	 * 检查查询经分析并重写后的形式，看我们是否将
	 * 能够把它当成简单表达式。因本函数只在
	 * 刚建好 CachedPlanSource 之后被调用，故不必
	 * 担心该查询已经过时。
	 */
	if (!exec_is_simple_query(expr))
		return;

	/* exec_is_simple_query 已核实只有一个 CachedPlanSource */
	plansources = SPI_plan_get_plan_sources(expr->plan);
	plansource = (CachedPlanSource *) linitial(plansources);

	/*
	 * 取该查询的通用计划。若需要重新规划，就把该
	 * 工作放在 eval_mcontext 里。（注意重新规划可能抛错，
	 * 此时 expr 仍标成非简单，这没关系。）
	 */
	oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));
	cplan = SPI_plan_get_cached_plan(expr->plan);
	MemoryContextSwitchTo(oldcontext);

	/* 无法失败，因上面已查过只有一个 CachedPlanSource */
	Assert(cplan != NULL);

	/*
	 * 核实 plancache.c 认为该计划简单到足以使用
	 * CachedPlanIsSimplyValid。有上面那些限制，不大可能
	 * 失败；但万一失败，就把计划当成非简单。一旦
	 * 成功，就把计划引用计数存进简单表达式 resowner。
	 */
	if (CachedPlanAllowsSimpleValidityCheck(plansource, cplan,
											estate->simple_eval_resowner))
	{
		/* 记住我们已持有引用计数 */
		expr->expr_simple_plansource = plansource;
		expr->expr_simple_plan = cplan;
		expr->expr_simple_plan_lxid = MyProc->lxid;

		/* 其余工作与重规划的代码路径共用 */
		exec_save_simple_expr(expr, cplan);
	}

	/*
	 * 释放 SPI_plan_get_cached_plan 取得的计划引用计数。（此
	 * 计数由错误的 resowner 持有，故无法直接改派。）
	 */
	ReleaseCachedPlan(cplan, CurrentResourceOwner);
}

/*
 * exec_is_simple_query 预检一棵查询树，看它是否也许简单
 *
 * 检查某查询经分析并重写后的形式，看我们是否将
 * 能够把它当成简单表达式。调用者有责任
 * 让 CachedPlanSource 保持最新。
 */
static bool
exec_is_simple_query(PLpgSQL_expr *expr)
{
	List	   *plansources;
	CachedPlanSource *plansource;
	Query	   *query;

	/*
	 * 我们只能测试恰好得到一个 CachedPlanSource 的查询。
	 */
	plansources = SPI_plan_get_plan_sources(expr->plan);
	if (list_length(plansources) != 1)
		return false;
	plansource = (CachedPlanSource *) linitial(plansources);

	/*
	 * 1. 必须只有一棵查询树。
	 */
	if (list_length(plansource->query_list) != 1)
		return false;
	query = (Query *) linitial(plansource->query_list);

	/*
	 * 2. 必须是没有任何输入表的普通 SELECT。
	 */
	if (!IsA(query, Query))
		return false;
	if (query->commandType != CMD_SELECT)
		return false;
	if (query->rtable != NIL)
		return false;

	/*
	 * 3. 也无法有任何子计划、聚集、限定子句。（这些
	 * 检测大体应合 inline_function() 在把 SQL 函数
	 * 内联之前所做的检查；不然内联可能改掉我们
	 * 关于表达式是否简单的结论，这并非我们所要。）
	 */
	if (query->hasAggs ||
		query->hasWindowFuncs ||
		query->hasTargetSRFs ||
		query->hasSubLinks ||
		query->cteList ||
		query->jointree->fromlist ||
		query->jointree->quals ||
		query->groupClause ||
		query->groupingSets ||
		query->havingQual ||
		query->windowClause ||
		query->distinctClause ||
		query->sortClause ||
		query->limitOffset ||
		query->limitCount ||
		query->setOperations)
		return false;

	/*
	 * 4. 该查询必须有单个属性作为结果。
	 */
	if (list_length(query->targetList) != 1)
		return false;

	/*
	 * 好，可把它当简单计划。
	 */
	return true;
}

/*
 * exec_save_simple_expr 从 CachedPlan 抽出简单表达式
 */
static void
exec_save_simple_expr(PLpgSQL_expr *expr, CachedPlan *cplan)
{
	PlannedStmt *stmt;
	Plan	   *plan;
	Expr	   *tle_expr;

	/*
	 * 鉴于 exec_simple_check_plan 已做的检查，这些 Assert 无一
	 * 该在这里失败。
	 */

	/* 抽出那唯一的 PlannedStmt */
	Assert(list_length(cplan->stmt_list) == 1);
	stmt = linitial_node(PlannedStmt, cplan->stmt_list);
	Assert(stmt->commandType == CMD_SELECT);

	/*
	 * 通常计划节点应是简单 Result。不过若
	 * debug_parallel_query 开着，规划器也许塞进 Gather 节点
	 * 在那之上；以及若此计划用于可滚动游标，规划器
	 * 也许又在上面塞了 Material 节点。最简单的处理是
	 * 看穿 Gather 以及 Material 这些节点。上层
	 * 节点的 tlist 通常含一个 Var，指向子节点的
	 * 输出，但也可能是 Param，还可能是 Const，由
	 * setrefs.c 照抄。
	 */
	plan = stmt->planTree;
	for (;;)
	{
		/* 抽出那唯一的 tlist 表达式 */
		Assert(list_length(plan->targetlist) == 1);
		tle_expr = linitial_node(TargetEntry, plan->targetlist)->expr;

		if (IsA(plan, Result))
		{
			Assert(plan->lefttree == NULL &&
				   plan->righttree == NULL &&
				   plan->initPlan == NULL &&
				   plan->qual == NULL &&
				   ((Result *) plan)->resconstantqual == NULL);
			break;
		}
		else if (IsA(plan, Gather) || IsA(plan, Material))
		{
			Assert(plan->lefttree != NULL &&
				   plan->righttree == NULL &&
				   plan->initPlan == NULL &&
				   plan->qual == NULL);
			/* 若 setrefs.c 向上拷了 Const，不必再看 */
			if (IsA(tle_expr, Const))
				break;
			/* 否则它最好是 Param，还是外层 Var */
			Assert(IsA(tle_expr, Param) || (IsA(tle_expr, Var) &&
											((Var *) tle_expr)->varno == OUTER_VAR));
			/* 下降到子节点 */
			plan = plan->lefttree;
		}
		else
			elog(ERROR, "unexpected plan node type: %d",
				 (int) nodeTag(plan));
	}

	/*
	 * 保存简单表达式，并把状态初始化成当前事务中
	 * 并非有效。
	 */
	expr->expr_simple_expr = tle_expr;
	expr->expr_simple_state = NULL;
	expr->expr_simple_in_use = false;
	expr->expr_simple_lxid = InvalidLocalTransactionId;
	/* 也把表达式的结果类型收起来 */
	expr->expr_simple_type = exprType((Node *) tle_expr);
	expr->expr_simple_typmod = exprTypmod((Node *) tle_expr);
	/* 我们也要记住它到底可不可变 */
	expr->expr_simple_mutable = contain_mutable_functions((Node *) tle_expr);

	/*
	 * 最后，检查有没有可能去优化一个
	 * 读写参数。
	 */
	exec_check_rw_parameter(expr);
}

/*
 * exec_check_rw_parameter 能否把扩展对象当读写参数传？
 *
 * 若赋值形如 x := array_append(x, foo)，其中的
 * 顶层函数被信任为出错时不破坏其参数。于是当
 * x 的值是扩展对象，就可以安全地传递该
 * 值，当作读写指针，并让函数修改该值
 * 就地。
 *
 * 本函数检查表达式是否安全，并设置 expr->expr_rw_param
 * 为表达式中任一可按此传递的 Param 的地址，按
 * 读写来传（只能有一个）；没有安全 Param 时则置 NULL。
 *
 * 注意此机制有意只把安全标记打在
 * 一个 Param 上；表达式里可能还有别的 Param 指向目标
 * 变量，但那些仍必须当成只读。
 *
 * 另外注意，我们只在简单表达式里做此优化。
 * 对非简单表达式没有意义，因为
 * exec_run_select 那条路径反正要压扁任何扩展结果。
 * 另外，可放心假定 expr_simple_expr 树在编译前
 * 并不被拷到别处，因此按指针相等去找
 * expr_rw_param 就能对上目标 Param。更一般的情形里
 * 那就虚得多。
 */
static void
exec_check_rw_parameter(PLpgSQL_expr *expr)
{
	int			target_dno;
	Oid			funcid;
	List	   *fargs;
	ListCell   *lc;

	/* 当不安全 */
	expr->expr_rw_param = NULL;

	/* 若表达式不是赋值来源就结束 */
	target_dno = expr->target_param;
	if (target_dno < 0)
		return;

	/*
	 * 若目标变量没被表达式引用，则无需
	 * 再看
	 */
	if (!bms_is_member(target_dno, expr->paramnos))
		return;

	/* 非简单表达式不该走到这里 */
	Assert(expr->expr_simple_expr != NULL);

	/*
	 * 表达式顶层必须是简单 FuncExpr、OpExpr，还是
	 * SubscriptingRef，否则无法优化。
	 */
	if (IsA(expr->expr_simple_expr, FuncExpr))
	{
		FuncExpr   *fexpr = (FuncExpr *) expr->expr_simple_expr;

		funcid = fexpr->funcid;
		fargs = fexpr->args;
	}
	else if (IsA(expr->expr_simple_expr, OpExpr))
	{
		OpExpr	   *opexpr = (OpExpr *) expr->expr_simple_expr;

		funcid = opexpr->opfuncid;
		fargs = opexpr->args;
	}
	else if (IsA(expr->expr_simple_expr, SubscriptingRef))
	{
		SubscriptingRef *sbsref = (SubscriptingRef *) expr->expr_simple_expr;

		/* 我们只信任标准 varlena 数组是安全的 */
		if (get_typsubscript(sbsref->refcontainertype, NULL) !=
			F_ARRAY_SUBSCRIPT_HANDLER)
			return;

		/* 若 refexpr 就是目标就能优化，否则不能 */
		if (sbsref->refexpr && IsA(sbsref->refexpr, Param))
		{
			Param	   *param = (Param *) sbsref->refexpr;

			if (param->paramkind == PARAM_EXTERN &&
				param->paramid == target_dno + 1)
			{
				/* 找到要按读写传递的 Param */
				expr->expr_rw_param = param;
				return;
			}
		}

		return;
	}
	else
		return;

	/*
	 * 顶层函数必须是我们所信任为安全的那一个。
	 * 目前名单是写死的，但非常希望能够
	 * 允许扩展把自家函数标成安全。
	 */
	if (!(funcid == F_ARRAY_APPEND ||
		  funcid == F_ARRAY_PREPEND))
		return;

	/*
	 * 目标变量（以 Param 形式）必须作为直接
	 * 参数出现在顶层函数上。树里更下面的引用
	 * 无法优化；但另一方面，它们并不否定
	 * 对顶层调用的优化，因那个调用最后才执行。
	 */
	foreach(lc, fargs)
	{
		Node	   *arg = (Node *) lfirst(lc);

		if (arg && IsA(arg, Param))
		{
			Param	   *param = (Param *) arg;

			if (param->paramkind == PARAM_EXTERN &&
				param->paramid == target_dno + 1)
			{
				/* 找到要按读写传递的 Param */
				expr->expr_rw_param = param;
				return;
			}
		}
	}
}

/*
 * exec_check_assignable 给所指 datum 赋值是否可以？
 *
 * 这应合 pl_gram.y 的 check_assignable()。
 */
static void
exec_check_assignable(PLpgSQL_execstate *estate, int dno)
{
	PLpgSQL_datum *datum;

	Assert(dno >= 0 && dno < estate->ndatums);
	datum = estate->datums[dno];
	switch (datum->dtype)
	{
		case PLPGSQL_DTYPE_VAR:
		case PLPGSQL_DTYPE_PROMISE:
		case PLPGSQL_DTYPE_REC:
			if (((PLpgSQL_variable *) datum)->isconst)
				ereport(ERROR,
						(errcode(ERRCODE_ERROR_IN_ASSIGNMENT),
						 errmsg("variable \"%s\" is declared CONSTANT",
								((PLpgSQL_variable *) datum)->refname)));
			break;
		case PLPGSQL_DTYPE_ROW:
			/* 始终可赋值；成员变量在编译时已检查 */
			break;
		case PLPGSQL_DTYPE_RECFIELD:
			/* 父记录可赋则自己也可 */
			exec_check_assignable(estate,
								  ((PLpgSQL_recfield *) datum)->recparentno);
			break;
		default:
			elog(ERROR, "unrecognized dtype: %d", datum->dtype);
			break;
	}
}

/* ----------
 * exec_set_found 把全局 FOUND 设成真还是假
 * ----------
 */
static void
exec_set_found(PLpgSQL_execstate *estate, bool state)
{
	PLpgSQL_var *var;

	var = (PLpgSQL_var *) (estate->datums[estate->found_varno]);
	assign_simple_var(estate, var, BoolGetDatum(state), false, false);
}

/*
 * plpgsql_create_econtext 为当前函数建 eval_econtext
 *
 * 我们可能也要新建 shared_simple_eval_estate，若还没有
 * 一个已经服务于当前事务。EState 将在
 * 事务结束清掉。shared_simple_eval_resowner 同。
 */
static void
plpgsql_create_econtext(PLpgSQL_execstate *estate)
{
	SimpleEcontextStackEntry *entry;

	/*
	 * 若还没有，就建一个供简单表达式求值用的 EState
	 * 于当前事务中。该 EState 被做成子级，其父为
	 * TopTransactionContext，从而寿命正确。
	 *
	 * 注意：开始一个 DO 块时从不走这条路；所需
	 * EState 已由 plpgsql_inline_handler 建好。不过，
	 * 若该 DO 块执行 COMMIT 还是 ROLLBACK，我们就来这里并
	 * 建一个共享 EState，供该 DO 块余下部分使用。可以；
	 * 见 shared_simple_eval_estate 的注释。（另请注意，DO
	 * 块在余下过程中继续用它私有的转换散列表。
	 * 眼下没问题，但将来也许惹出麻烦。）
	 */
	if (estate->simple_eval_estate == NULL)
	{
		MemoryContext oldcontext;

		if (shared_simple_eval_estate == NULL)
		{
			oldcontext = MemoryContextSwitchTo(TopTransactionContext);
			shared_simple_eval_estate = CreateExecutorState();
			MemoryContextSwitchTo(oldcontext);
		}
		estate->simple_eval_estate = shared_simple_eval_estate;
	}

	/*
	 * 简单表达式的资源所有者亦同样。
	 */
	if (estate->simple_eval_resowner == NULL)
	{
		if (shared_simple_eval_resowner == NULL)
			shared_simple_eval_resowner =
				ResourceOwnerCreate(TopTransactionResourceOwner,
									"PL/pgSQL simple expressions");
		estate->simple_eval_resowner = shared_simple_eval_resowner;
	}

	/*
	 * 为当前函数建一个子级 econtext。
	 */
	estate->eval_econtext = CreateExprContext(estate->simple_eval_estate);

	/*
	 * 做一条栈项，以便子事务结束时清理该 econtext。
	 * 为求简单，栈项保存在 TopTransactionContext 里。
	 */
	entry = (SimpleEcontextStackEntry *)
		MemoryContextAlloc(TopTransactionContext,
						   sizeof(SimpleEcontextStackEntry));

	entry->stack_econtext = estate->eval_econtext;
	entry->xact_subxid = GetCurrentSubTransactionId();

	entry->next = simple_econtext_stack;
	simple_econtext_stack = entry;
}

/*
 * plpgsql_destroy_econtext 销毁函数的 econtext
 *
 * 我们检查它与栈顶项相符，并销毁该栈
 * 项，连同该上下文。
 */
static void
plpgsql_destroy_econtext(PLpgSQL_execstate *estate)
{
	SimpleEcontextStackEntry *next;

	Assert(simple_econtext_stack != NULL);
	Assert(simple_econtext_stack->stack_econtext == estate->eval_econtext);

	next = simple_econtext_stack->next;
	pfree(simple_econtext_stack);
	simple_econtext_stack = next;

	FreeExprContext(estate->eval_econtext, true);
	estate->eval_econtext = NULL;
}

/*
 * plpgsql_xact_cb 在事务提交、中止之后做清理
 *
 * 若当前事务里建过简单表达式 EState，
 * 就必须清理它。简单表达式的 resowner 也同样。
 */
void
plpgsql_xact_cb(XactEvent event, void *arg)
{
	/*
	 * 若在做干净的事务关闭，就释放 EState，并告知
	 * 该 resowner 放开它持有的 plancache 引用，以便
	 * 其余资源都能被正确释放。（我们不必
	 * 在此真正删除 resowner；删除
	 * TopTransactionResourceOwner 将负责此事。）
	 *
	 * 在中止时，我们指望常规中止恢复过程去释放
	 * 一切相关之物，故只清掉我们的指针。
	 */
	if (event == XACT_EVENT_COMMIT ||
		event == XACT_EVENT_PARALLEL_COMMIT ||
		event == XACT_EVENT_PREPARE)
	{
		simple_econtext_stack = NULL;

		if (shared_simple_eval_estate)
			FreeExecutorState(shared_simple_eval_estate);
		shared_simple_eval_estate = NULL;
		if (shared_simple_eval_resowner)
			ResourceOwnerReleaseAllPlanCacheRefs(shared_simple_eval_resowner);
		shared_simple_eval_resowner = NULL;
	}
	else if (event == XACT_EVENT_ABORT ||
			 event == XACT_EVENT_PARALLEL_ABORT)
	{
		simple_econtext_stack = NULL;
		shared_simple_eval_estate = NULL;
		shared_simple_eval_resowner = NULL;
	}
}

/*
 * plpgsql_subxact_cb 在子事务提交、中止之后做清理
 *
 * 确保当前子事务里建的任何简单表达式 econtext
 * 都得到清理。我们必须显式来做，因为
 * 没有别的代码知道哪个 econtext 属于哪层子事务。
 */
void
plpgsql_subxact_cb(SubXactEvent event, SubTransactionId mySubid,
				   SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_COMMIT_SUB || event == SUBXACT_EVENT_ABORT_SUB)
	{
		while (simple_econtext_stack != NULL &&
			   simple_econtext_stack->xact_subxid == mySubid)
		{
			SimpleEcontextStackEntry *next;

			FreeExprContext(simple_econtext_stack->stack_econtext,
							(event == SUBXACT_EVENT_COMMIT_SUB));
			next = simple_econtext_stack->next;
			pfree(simple_econtext_stack);
			simple_econtext_stack = next;
		}
	}
}

/*
 * assign_simple_var 给任一 VAR 型 datum 赋新值
 *
 * 这应是给简单变量赋值的唯一机制，
 * 以免我们不正确地释放旧值（且不说
 * 解 toast 之事）。
 */
static void
assign_simple_var(PLpgSQL_execstate *estate, PLpgSQL_var *var,
				  Datum newvalue, bool isnull, bool freeable)
{
	Assert(var->dtype == PLPGSQL_DTYPE_VAR ||
		   var->dtype == PLPGSQL_DTYPE_PROMISE);

	/*
	 * 在非原子上下文里，我们不想把 TOAST 指针存进
	 * 变量里，因这类指针提交之后可能变陈旧。
	 * 这类情形就强制解 toast。但不想解掉（压扁）
	 * 扩展对象；那些跨事务边界应当可以，
	 * 因它们只是驻留内存的对象。（本模块别处
	 * 对扩展记录的操作同样需要请求
	 * 在 !estate->atomic 时解开记录字段的 toast。扩展数组
	 * 不是问题，因数组各项始终已经解过 toast。）
	 */
	if (!estate->atomic && !isnull && var->datatype->typlen == -1 &&
		VARATT_IS_EXTERNAL_NON_EXPANDED(DatumGetPointer(newvalue)))
	{
		MemoryContext oldcxt;
		Datum		detoasted;

		/*
		 * 在 eval_mcontext 里解 toast，以免长期泄漏
		 * toast 抓取可能漏出的任何内存。然后必须把
		 * 已解 toast 的 datum 拷进函数主内存上下文，这很
		 * 痛，但选择很少。
		 */
		oldcxt = MemoryContextSwitchTo(get_eval_mcontext(estate));
		detoasted = PointerGetDatum(detoast_external_attr((struct varlena *) DatumGetPointer(newvalue)));
		MemoryContextSwitchTo(oldcxt);
		/* 现在正适合：若输入值可释放，就别泄漏它 */
		if (freeable)
			pfree(DatumGetPointer(newvalue));
		/* 一旦把值拷好，它就肯定可以释放 */
		newvalue = datumCopy(detoasted, false, -1);
		freeable = true;
		/* 这里无法清理 eval_mcontext，但很快便做完 */
	}

	/* 需要时释放旧值 */
	if (var->freeval)
	{
		if (DatumIsReadWriteExpandedObject(var->value,
										   var->isnull,
										   var->datatype->typlen))
			DeleteExpandedObject(var->value);
		else
			pfree(DatumGetPointer(var->value));
	}
	/* 把新值赋给 datum */
	var->value = newvalue;
	var->isnull = isnull;
	var->freeval = freeable;

	/*
	 * 若它是承诺变量，则要么刚赋了所承诺的
	 * 值，要么用户显式赋了一个覆盖值。两种
	 * 情形都取消承诺。
	 */
	var->promise = PLPGSQL_PROMISE_NONE;
}

/*
 * 释放文本变量的旧值，并从 C 字符串赋上新值
 */
static void
assign_text_var(PLpgSQL_execstate *estate, PLpgSQL_var *var, const char *str)
{
	assign_simple_var(estate, var, CStringGetTextDatum(str), false, true);
}

/*
 * assign_record_var 给任一 REC 型 datum 赋新值
 */
static void
assign_record_var(PLpgSQL_execstate *estate, PLpgSQL_rec *rec,
				  ExpandedRecordHeader *erh)
{
	Assert(rec->dtype == PLPGSQL_DTYPE_REC);

	/* 把新记录对象转入 datum_context */
	TransferExpandedRecord(erh, estate->datum_context);

	/* 释放旧值 */
	if (rec->erh)
		DeleteExpandedObject(ExpandedRecordGetDatum(rec->erh));

	/* 并装上新的 */
	rec->erh = erh;
}

/*
 * exec_eval_using_params 求值 USING 子句的参数
 *
 * 结果数据结构建在 stmt_mcontext 里，并且应当
 * 靠重置该上下文来释放。
 */
static ParamListInfo
exec_eval_using_params(PLpgSQL_execstate *estate, List *params)
{
	ParamListInfo paramLI;
	int			nargs;
	MemoryContext stmt_mcontext;
	MemoryContext oldcontext;
	int			i;
	ListCell   *lc;

	/* 无参数快路径：可直接返回 NULL */
	if (params == NIL)
		return NULL;

	nargs = list_length(params);
	stmt_mcontext = get_stmt_mcontext(estate);
	oldcontext = MemoryContextSwitchTo(stmt_mcontext);
	paramLI = makeParamList(nargs);
	MemoryContextSwitchTo(oldcontext);

	i = 0;
	foreach(lc, params)
	{
		PLpgSQL_expr *param = (PLpgSQL_expr *) lfirst(lc);
		ParamExternData *prm = &paramLI->params[i];
		int32		ppdtypmod;

		/*
		 * 始终把参数标成 const，因结果只用于
		 * 单次计划。
		 */
		prm->pflags = PARAM_FLAG_CONST;

		prm->value = exec_eval_expr(estate, param,
									&prm->isnull,
									&prm->ptype,
									&ppdtypmod);

		oldcontext = MemoryContextSwitchTo(stmt_mcontext);

		if (prm->ptype == UNKNOWNOID)
		{
			/*
			 * 把 unknown 参数当成 text，因那正是多数
			 * 人所期望的。SPI 函数能更聪明地强制 unknown
			 * 常量，但不能同样处理 unknown 的 Param。
			 * 这段代码也负责拷贝到正确的内存上下文。
			 * 注意我们假定 unknown 具有 C 字符串的表示。
			 */
			prm->ptype = TEXTOID;
			if (!prm->isnull)
				prm->value = CStringGetTextDatum(DatumGetCString(prm->value));
		}
		/* 按引用传递的非 NULL 值必须拷进 stmt_mcontext */
		else if (!prm->isnull)
		{
			int16		typLen;
			bool		typByVal;

			get_typlenbyval(prm->ptype, &typLen, &typByVal);
			if (!typByVal)
				prm->value = datumCopy(prm->value, typByVal, typLen);
		}

		MemoryContextSwitchTo(oldcontext);

		exec_eval_cleanup(estate);

		i++;
	}

	return paramLI;
}

/*
 * 为动态查询打开门户
 *
 * 小心：退出时重置 stmt_mcontext。我们也许最终需要
 * 把该责任移给调用者，但目前没有调用者需要
 * 有能活过此处的语句寿命临时数据，所以
 * 这里更简单。
 */
static Portal
exec_dynquery_with_params(PLpgSQL_execstate *estate,
						  PLpgSQL_expr *dynquery,
						  List *params,
						  const char *portalname,
						  int cursorOptions)
{
	Portal		portal;
	Datum		query;
	bool		isnull;
	Oid			restype;
	int32		restypmod;
	char	   *querystr;
	SPIParseOpenOptions options;
	MemoryContext stmt_mcontext = get_stmt_mcontext(estate);

	/*
	 * 求值 EXECUTE 关键字之后的字符串表达式。其结果是
	 * 我们必须执行的查询串。
	 */
	query = exec_eval_expr(estate, dynquery, &isnull, &restype, &restypmod);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("query string argument of EXECUTE is null")));

	/* 取 C 字符串表示 */
	querystr = convert_value_to_string(estate, query, restype);

	/* 清理前把它拷进 stmt_mcontext */
	querystr = MemoryContextStrdup(stmt_mcontext, querystr);

	exec_eval_cleanup(estate);

	/*
	 * 为该查询打开隐式游标。即便没有参数，我们也用
	 * SPI_cursor_parse_open，因这样避免建立并释放
	 * 一份计划拷贝。
	 */
	memset(&options, 0, sizeof(options));
	options.params = exec_eval_using_params(estate, params);
	options.cursorOptions = cursorOptions;
	options.read_only = estate->readonly_func;

	portal = SPI_cursor_parse_open(portalname, querystr, &options);

	if (portal == NULL)
		elog(ERROR, "could not open implicit cursor for query \"%s\": %s",
			 querystr, SPI_result_code_string(SPI_result));

	/* 释放瞬时数据 */
	MemoryContextReset(stmt_mcontext);

	return portal;
}

/*
 * 返回带有某表达式参数信息的格式化字符串，
 * 若该表达式不带任何参数则返回 NULL。
 * 结果在 eval_mcontext 里。
 */
static char *
format_expr_params(PLpgSQL_execstate *estate,
				   const PLpgSQL_expr *expr)
{
	int			paramno;
	int			dno;
	StringInfoData paramstr;
	MemoryContext oldcontext;

	if (!expr->paramnos)
		return NULL;

	oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));

	initStringInfo(&paramstr);
	paramno = 0;
	dno = -1;
	while ((dno = bms_next_member(expr->paramnos, dno)) >= 0)
	{
		Datum		paramdatum;
		Oid			paramtypeid;
		bool		paramisnull;
		int32		paramtypmod;
		PLpgSQL_var *curvar;

		curvar = (PLpgSQL_var *) estate->datums[dno];

		exec_eval_datum(estate, (PLpgSQL_datum *) curvar,
						&paramtypeid, &paramtypmod,
						&paramdatum, &paramisnull);

		appendStringInfo(&paramstr, "%s%s = ",
						 paramno > 0 ? ", " : "",
						 curvar->refname);

		if (paramisnull)
			appendStringInfoString(&paramstr, "NULL");
		else
			appendStringInfoStringQuoted(&paramstr,
										 convert_value_to_string(estate,
																 paramdatum,
																 paramtypeid),
										 -1);

		paramno++;
	}

	MemoryContextSwitchTo(oldcontext);

	return paramstr.data;
}

/*
 * 返回带有参数值信息的格式化字符串，
 * 若没有参数则返回 NULL。
 * 结果在 eval_mcontext 里。
 */
static char *
format_preparedparamsdata(PLpgSQL_execstate *estate,
						  ParamListInfo paramLI)
{
	int			paramno;
	StringInfoData paramstr;
	MemoryContext oldcontext;

	if (!paramLI)
		return NULL;

	oldcontext = MemoryContextSwitchTo(get_eval_mcontext(estate));

	initStringInfo(&paramstr);
	for (paramno = 0; paramno < paramLI->numParams; paramno++)
	{
		ParamExternData *prm = &paramLI->params[paramno];

		/*
		 * 注意：目前这只用于由下列产生的 ParamListInfos
		 * exec_eval_using_params()，故不担心去调用
		 * paramFetch 钩子与跳过无用参数。
		 */
		appendStringInfo(&paramstr, "%s$%d = ",
						 paramno > 0 ? ", " : "",
						 paramno + 1);

		if (prm->isnull)
			appendStringInfoString(&paramstr, "NULL");
		else
			appendStringInfoStringQuoted(&paramstr,
										 convert_value_to_string(estate,
																 prm->value,
																 prm->ptype),
										 -1);
	}

	MemoryContextSwitchTo(oldcontext);

	return paramstr.data;
}
