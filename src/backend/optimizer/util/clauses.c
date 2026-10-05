/*-------------------------------------------------------------------------
 *
 * clauses.c
 *	  routines to manipulate qualification clauses
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/clauses.c
 *
 * HISTORY
 *	  AUTHOR			DATE			MAJOR EVENT
 *	  Andrew Yu			Nov 3, 1994		clause.c and clauses.c combined
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"
#include "catalog/pg_language.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/functions.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/multibitmapset.h"
#include "nodes/nodeFuncs.h"
#include "nodes/subscripting.h"
#include "nodes/supportnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

typedef struct
{
	ParamListInfo boundParams;
	PlannerInfo *root;
	List	   *active_fns;
	Node	   *case_val;
	bool		estimate;
} eval_const_expressions_context;

typedef struct
{
	int			nargs;
	List	   *args;
	int		   *usecounts;
} substitute_actual_parameters_context;

typedef struct
{
	int			nargs;
	List	   *args;
	int			sublevels_up;
} substitute_actual_srf_parameters_context;

typedef struct
{
	char	   *proname;
	char	   *prosrc;
} inline_error_callback_arg;

typedef struct
{
	char		max_hazard;		/* 迄今发现的最差 proparallel 危险 */
	char		max_interesting;	/* 所关心的最差 proparallel 危险 */
	List	   *safe_param_ids; /* 要当作安全的 PARAM_EXEC Param ID */
} max_parallel_hazard_context;

static bool contain_agg_clause_walker(Node *node, void *context);
static bool find_window_functions_walker(Node *node, WindowFuncLists *lists);
static bool contain_subplans_walker(Node *node, void *context);
static bool contain_mutable_functions_walker(Node *node, void *context);
static bool contain_volatile_functions_walker(Node *node, void *context);
static bool contain_volatile_functions_not_nextval_walker(Node *node, void *context);
static bool max_parallel_hazard_walker(Node *node,
									   max_parallel_hazard_context *context);
static bool contain_nonstrict_functions_walker(Node *node, void *context);
static bool contain_exec_param_walker(Node *node, List *param_ids);
static bool contain_context_dependent_node(Node *clause);
static bool contain_context_dependent_node_walker(Node *node, int *flags);
static bool contain_leaked_vars_walker(Node *node, void *context);
static Relids find_nonnullable_rels_walker(Node *node, bool top_level);
static List *find_nonnullable_vars_walker(Node *node, bool top_level);
static bool is_strict_saop(ScalarArrayOpExpr *expr, bool falseOK);
static bool convert_saop_to_hashed_saop_walker(Node *node, void *context);
static Node *eval_const_expressions_mutator(Node *node,
											eval_const_expressions_context *context);
static bool contain_non_const_walker(Node *node, void *context);
static bool ece_function_is_safe(Oid funcid,
								 eval_const_expressions_context *context);
static List *simplify_or_arguments(List *args,
								   eval_const_expressions_context *context,
								   bool *haveNull, bool *forceTrue);
static List *simplify_and_arguments(List *args,
									eval_const_expressions_context *context,
									bool *haveNull, bool *forceFalse);
static Node *simplify_boolean_equality(Oid opno, List *args);
static Expr *simplify_function(Oid funcid,
							   Oid result_type, int32 result_typmod,
							   Oid result_collid, Oid input_collid, List **args_p,
							   bool funcvariadic, bool process_args, bool allow_non_const,
							   eval_const_expressions_context *context);
static List *reorder_function_arguments(List *args, int pronargs,
										HeapTuple func_tuple);
static List *add_function_defaults(List *args, int pronargs,
								   HeapTuple func_tuple);
static List *fetch_function_defaults(HeapTuple func_tuple);
static void recheck_cast_function_args(List *args, Oid result_type,
									   Oid *proargtypes, int pronargs,
									   HeapTuple func_tuple);
static Expr *evaluate_function(Oid funcid, Oid result_type, int32 result_typmod,
							   Oid result_collid, Oid input_collid, List *args,
							   bool funcvariadic,
							   HeapTuple func_tuple,
							   eval_const_expressions_context *context);
static Expr *inline_function(Oid funcid, Oid result_type, Oid result_collid,
							 Oid input_collid, List *args,
							 bool funcvariadic,
							 HeapTuple func_tuple,
							 eval_const_expressions_context *context);
static Node *substitute_actual_parameters(Node *expr, int nargs, List *args,
										  int *usecounts);
static Node *substitute_actual_parameters_mutator(Node *node,
												  substitute_actual_parameters_context *context);
static void sql_inline_error_callback(void *arg);
static Query *substitute_actual_srf_parameters(Query *expr,
											   int nargs, List *args);
static Node *substitute_actual_srf_parameters_mutator(Node *node,
													  substitute_actual_srf_parameters_context *context);
static bool pull_paramids_walker(Node *node, Bitmapset **context);


/*****************************************************************************
 *		聚集函数子句处理
 *****************************************************************************/

/*
 * contain_agg_clause
 *	  在子句里递归查找 Aggref 和 GroupingFunc 节点。
 *
 *	  若找到任何聚集就返回真。
 *
 * 本函数不下降进子查询，因此只应在
 * 子链接已归约成子计划之后使用，或在已知没有
 * 子查询的上下文里使用。也不该有外层聚集引用。
 *
 * （若想要类似功能但能处理子查询，
 * 见 rewriteManip.c 的 contain_aggs_of_level()。）
 */
bool
contain_agg_clause(Node *clause)
{
	return contain_agg_clause_walker(clause, NULL);
}

static bool
contain_agg_clause_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		Assert(((Aggref *) node)->agglevelsup == 0);
		return true;			/* 中止树遍历并返回真 */
	}
	if (IsA(node, GroupingFunc))
	{
		Assert(((GroupingFunc *) node)->agglevelsup == 0);
		return true;			/* 中止树遍历并返回真 */
	}
	Assert(!IsA(node, SubLink));
	return expression_tree_walker(node, contain_agg_clause_walker, context);
}

/*****************************************************************************
 *		窗口函数子句处理
 *****************************************************************************/

/*
 * contain_window_function
 *	  在子句里递归查找 WindowFunc 节点。
 *
 * 窗口函数没有层次成员，而是硬连到
 * 当前查询层次，因此这与
 * rewriteManip.c 里的函数一样。
 */
bool
contain_window_function(Node *clause)
{
	return contain_windowfuncs(clause);
}

/*
 * find_window_functions
 *	  找出表达式树里的全部 WindowFunc 节点，并按
 *	  winref ID 号把它们组织起来。
 *
 * 调用方必须提供树中预期出现的 winref ID 上界。
 */
WindowFuncLists *
find_window_functions(Node *clause, Index maxWinRef)
{
	WindowFuncLists *lists = palloc(sizeof(WindowFuncLists));

	lists->numWindowFuncs = 0;
	lists->maxWinRef = maxWinRef;
	lists->windowFuncs = (List **) palloc0((maxWinRef + 1) * sizeof(List *));
	(void) find_window_functions_walker(clause, lists);
	return lists;
}

static bool
find_window_functions_walker(Node *node, WindowFuncLists *lists)
{
	if (node == NULL)
		return false;
	if (IsA(node, WindowFunc))
	{
		WindowFunc *wfunc = (WindowFunc *) node;

		/* winref 无符号，因此单侧测试即可 */
		if (wfunc->winref > lists->maxWinRef)
			elog(ERROR, "WindowFunc contains out-of-range winref %u",
				 wfunc->winref);
		/* 消除重复，以免重复计算 */
		if (!list_member(lists->windowFuncs[wfunc->winref], wfunc))
		{
			lists->windowFuncs[wfunc->winref] =
				lappend(lists->windowFuncs[wfunc->winref], wfunc);
			lists->numWindowFuncs++;
		}

		/*
		 * 我们假定解析器已经检查过参数或过滤子句里
		 * 没有窗口函数。因此不必递归进入它们。
		 * （若解析器或规划器在这一点上出错，
		 * 执行器仍会抓住；见 ExecInitExpr。）
		 */
		return false;
	}
	Assert(!IsA(node, SubLink));
	return expression_tree_walker(node, find_window_functions_walker,
								  (void *) lists);
}


/*****************************************************************************
 *		对返回集合的表达式的支持
 *****************************************************************************/

/*
 * expression_returns_set_rows
 *	  估计集合返回表达式返回的行数。
 *	  若它不是集合返回表达式，结果为 1。
 *
 * 只应检查顶层函数或操作符；从前
 * 适合递归，现在不再适合。（即使函数输入里还有更多 SRFs，
 * 它们的乘数也是分开计算的。）
 *
 * 注意：与 nodes/nodeFuncs.c 里的 expression_returns_set() 保持同步。
 */
double
expression_returns_set_rows(PlannerInfo *root, Node *clause)
{
	if (clause == NULL)
		return 1.0;
	if (IsA(clause, FuncExpr))
	{
		FuncExpr   *expr = (FuncExpr *) clause;

		if (expr->funcretset)
			return clamp_row_est(get_function_rows(root, expr->funcid, clause));
	}
	if (IsA(clause, OpExpr))
	{
		OpExpr	   *expr = (OpExpr *) clause;

		if (expr->opretset)
		{
			set_opfuncid(expr);
			return clamp_row_est(get_function_rows(root, expr->opfuncid, clause));
		}
	}
	return 1.0;
}


/*****************************************************************************
 *		子计划子句处理
 *****************************************************************************/

/*
 * contain_subplans
 *	  在子句里递归查找子计划节点。
 *
 * 若看到 SubLink 节点就返回真。只有表达式树
 * 还没被 subselect.c 变换时才可能这样。我们并不
 * 知道该节点会产生真正的子计划还是只是 initplan，
 * 但保守假定它会是子计划。
 *
 * 若找到任何子计划就返回真。
 */
bool
contain_subplans(Node *clause)
{
	return contain_subplans_walker(clause, NULL);
}

static bool
contain_subplans_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubPlan) ||
		IsA(node, AlternativeSubPlan) ||
		IsA(node, SubLink))
		return true;			/* 中止树遍历并返回真 */
	return expression_tree_walker(node, contain_subplans_walker, context);
}


/*****************************************************************************
 *		检查子句里的可变函数
 *****************************************************************************/

/*
 * contain_mutable_functions
 *	  在子句里递归查找可变函数。
 *
 * 若找到任何可变函数（或由可变函数实现的操作符）
 * 就返回真。需要这个测试，才不会误以为
 * WHERE random() < 0.5 这类东西可以当成
 * 常量限定条件。
 *
 * 只有经过表达式预处理的子句才会给出正确答案。
 * 规划器之外的调用方通常应当改用
 * contain_mutable_functions_after_planning()，理由
 * 见那里的说明。
 *
 * 我们会递归查看 Query 节点（也就是 SubLink 子选择），
 * 但不进入 SubPlans。见 contain_volatile_functions() 的注释。
 */
bool
contain_mutable_functions(Node *clause)
{
	return contain_mutable_functions_walker(clause, NULL);
}

static bool
contain_mutable_functions_checker(Oid func_id, void *context)
{
	return (func_volatile(func_id) != PROVOLATILE_IMMUTABLE);
}

static bool
contain_mutable_functions_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	/* 检查节点本身里的可变函数 */
	if (check_functions_in_node(node, contain_mutable_functions_checker,
								context))
		return true;

	if (IsA(node, JsonConstructorExpr))
	{
		const JsonConstructorExpr *ctor = (JsonConstructorExpr *) node;
		ListCell   *lc;
		bool		is_jsonb;

		is_jsonb = ctor->returning->format->format_type == JS_FORMAT_JSONB;

		/*
		 * 专门检查 argument_type 到 json[b] 的转换。下面仍会
		 * 递归检查 args，但这里要专门
		 * 检查发出的子句是否会因为 TimeZone
		 * 等原因而并非不可变。
		 */
		foreach(lc, ctor->args)
		{
			Oid			typid = exprType(lfirst(lc));

			if (is_jsonb ?
				!to_jsonb_is_immutable(typid) :
				!to_json_is_immutable(typid))
				return true;
		}

		/* 检查全部子节点 */
	}

	if (IsA(node, SQLValueFunction))
	{
		/* SQLValueFunction 的所有变体都是稳定的 */
		return true;
	}

	if (IsA(node, NextValueExpr))
	{
		/* NextValueExpr 是易变的 */
		return true;
	}

	/*
	 * 把 MinMaxExpr 当作不可变应当是安全的，因为它会
	 * 依赖非跨类型的 btree 比较函数，而那些应当
	 * 总是不可变的。把 XmlExpr 当作不可变更可疑，
	 * 把 CoerceToDomain 当作不可变则完全危险。但我们
	 * 历史上一直如此，改掉大概会带来更多
	 * 问题而不是修好问题。实践中，若域约束
	 * 并非不可变，反正你会很痛苦。
	 */

	/* 递归检查参数 */
	if (IsA(node, Query))
	{
		/* 递归进入子选择 */
		return query_tree_walker((Query *) node,
								 contain_mutable_functions_walker,
								 context, 0);
	}
	return expression_tree_walker(node, contain_mutable_functions_walker,
								  context);
}

/*
 * contain_mutable_functions_after_planning
 *	  测试给定表达式是否含有可变函数。
 *
 * 这是 contain_mutable_functions() 的包装，从规划器外部调用
 * 是安全的。差别在于它先让表达式经过
 * expression_planner()。之所以需要那样，有两个关键理由：
 *
 * 首先，函数默认参数会被插入，这可能影响
 * 易变性（考虑 default now()）。
 *
 * 其次，可内联函数会被内联，这也许让我们得出
 * 该函数实际上没有标记的那么易变。例如，多态函数
 * 必须按它对任何输入类型的最易变行为来标记，
 * 但一旦把函数内联，也许就能得出：对正在处理的
 * 这种特定输入类型，它并没有那么
 * 易变。
 */
bool
contain_mutable_functions_after_planning(Expr *expr)
{
	/* 这里假定 expression_planner() 不会就地改写其输入 */
	expr = expression_planner(expr);

	/* 现在可以查找并非不可变的函数 */
	return contain_mutable_functions((Node *) expr);
}


/*****************************************************************************
 *		检查子句里的易变函数
 *****************************************************************************/

/*
 * contain_volatile_functions
 *	  在子句里递归查找易变函数。
 *
 * 若找到任何易变函数（或由易变函数实现的操作符）
 * 就返回真。这个测试防止例如把易变表达式
 * 无效地转换成索引扫描限定条件。
 *
 * 只有经过表达式预处理的子句才会给出正确答案。
 * 规划器之外的调用方通常应当改用
 * contain_volatile_functions_after_planning()，理由
 * 见那里的说明。
 *
 * 我们会递归查看 Query 节点（即 SubLink 子选择），
 * 但不进入 SubPlans。这有点怪，但是有意的。若正在
 * 看一个 SubLink，多半是在决定查询树变换是否安全，
 * 其中包含的子选择应当影响这个决定；
 * 例如复制一个含有易变函数的子选择
 * 会很糟。不过一旦到了已有 SubPlans 的阶段，
 * 后续规划不必考虑它们内部的易变性，因为
 * 执行器不会按易变性改变对 SubPlan 的
 * 求值规则。
 *
 * 对某些节点类型，例如 RestrictInfo 和 PathTarget，我们会缓存
 * 是否找到过任何易变函数，并在以后检查该节点时复用。
 * 判断缓存值应当设为 VOLATILITY_NOVOLATILE 还是
 * VOLATILITY_VOLATILE 的全部逻辑都属于本函数。任何改动这些节点、
 * 从而可能改变本函数结果的代码，都必须把缓存值设回
 * VOLATILITY_UNKNOWN。这样本函数在下次调用时可以重新判定
 * 正确的值，以防以后还要再判断该节点
 * 是否再次含有任何
 * 易变函数。
 */
bool
contain_volatile_functions(Node *clause)
{
	return contain_volatile_functions_walker(clause, NULL);
}

static bool
contain_volatile_functions_checker(Oid func_id, void *context)
{
	return (func_volatile(func_id) == PROVOLATILE_VOLATILE);
}

static bool
contain_volatile_functions_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	/* 检查节点本身里的易变函数 */
	if (check_functions_in_node(node, contain_volatile_functions_checker,
								context))
		return true;

	if (IsA(node, NextValueExpr))
	{
		/* NextValueExpr 是易变的 */
		return true;
	}

	if (IsA(node, RestrictInfo))
	{
		RestrictInfo *rinfo = (RestrictInfo *) node;

		/*
		 * 对 RestrictInfo，检查以前是否已经查过它的易变性。
		 * 若查过，可以直接用缓存值，不必
		 * 再查一遍。否则就检查，并缓存是否
		 * 找到了任何易变函数。
		 */
		if (rinfo->has_volatile == VOLATILITY_NOVOLATILE)
			return false;
		else if (rinfo->has_volatile == VOLATILITY_VOLATILE)
			return true;
		else
		{
			bool		hasvolatile;

			hasvolatile = contain_volatile_functions_walker((Node *) rinfo->clause,
															context);
			if (hasvolatile)
				rinfo->has_volatile = VOLATILITY_VOLATILE;
			else
				rinfo->has_volatile = VOLATILITY_NOVOLATILE;

			return hasvolatile;
		}
	}

	if (IsA(node, PathTarget))
	{
		PathTarget *target = (PathTarget *) node;

		/*
		 * 对 PathTarget 也做与上面 RestrictInfos 相同的
		 * 缓存。
		 */
		if (target->has_volatile_expr == VOLATILITY_NOVOLATILE)
			return false;
		else if (target->has_volatile_expr == VOLATILITY_VOLATILE)
			return true;
		else
		{
			bool		hasvolatile;

			hasvolatile = contain_volatile_functions_walker((Node *) target->exprs,
															context);

			if (hasvolatile)
				target->has_volatile_expr = VOLATILITY_VOLATILE;
			else
				target->has_volatile_expr = VOLATILITY_NOVOLATILE;

			return hasvolatile;
		}
	}

	/*
	 * 关于为何把 MinMaxExpr、XmlExpr 和 CoerceToDomain 当作不可变，
	 * 而 SQLValueFunction 是稳定的，见 contain_mutable_functions_walker 的说明。
	 * 因此这里它们都不在考虑之列。
	 */

	/* 递归检查参数 */
	if (IsA(node, Query))
	{
		/* 递归进入子选择 */
		return query_tree_walker((Query *) node,
								 contain_volatile_functions_walker,
								 context, 0);
	}
	return expression_tree_walker(node, contain_volatile_functions_walker,
								  context);
}

/*
 * contain_volatile_functions_after_planning
 *	  测试给定表达式是否含有易变函数。
 *
 * 这是 contain_volatile_functions() 的包装，从规划器外部调用
 * 是安全的。差别在于它先让表达式经过
 * expression_planner()。之所以需要那样，有两个关键理由：
 *
 * 首先，函数默认参数会被插入，这可能影响
 * 易变性（考虑 default random()）。
 *
 * 其次，可内联函数会被内联，这也许让我们得出
 * 该函数实际上没有标记的那么易变。例如，多态函数
 * 必须按它对任何输入类型的最易变行为来标记，
 * 但一旦把函数内联，也许就能得出：对正在处理的
 * 这种特定输入类型，它并没有那么
 * 易变。
 */
bool
contain_volatile_functions_after_planning(Expr *expr)
{
	/* 这里假定 expression_planner() 不会就地改写其输入 */
	expr = expression_planner(expr);

	/* 现在可以查找易变函数 */
	return contain_volatile_functions((Node *) expr);
}

/*
 * 供 COPY 使用的 contain_volatile_functions() 专用版本：
 * 忽略 nextval()，但其他函数照常处理。
 */
bool
contain_volatile_functions_not_nextval(Node *clause)
{
	return contain_volatile_functions_not_nextval_walker(clause, NULL);
}

static bool
contain_volatile_functions_not_nextval_checker(Oid func_id, void *context)
{
	return (func_id != F_NEXTVAL &&
			func_volatile(func_id) == PROVOLATILE_VOLATILE);
}

static bool
contain_volatile_functions_not_nextval_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	/* 检查节点本身里的易变函数 */
	if (check_functions_in_node(node,
								contain_volatile_functions_not_nextval_checker,
								context))
		return true;

	/*
	 * 关于为何把 MinMaxExpr、XmlExpr 和 CoerceToDomain 当作不可变，
	 * 而 SQLValueFunction 是稳定的，见 contain_mutable_functions_walker 的说明。
	 * 因此这里它们都不在考虑之列。
	 * 另外，既然有意忽略 nextval()，大概也
	 * 应当忽略 NextValueExpr。
	 */

	/* 递归检查参数 */
	if (IsA(node, Query))
	{
		/* 递归进入子选择 */
		return query_tree_walker((Query *) node,
								 contain_volatile_functions_not_nextval_walker,
								 context, 0);
	}
	return expression_tree_walker(node,
								  contain_volatile_functions_not_nextval_walker,
								  context);
}


/*****************************************************************************
 *		检查查询里并行不安全和/或受限的构造
 *****************************************************************************/

/*
 * max_parallel_hazard
 *		找出给定查询里最差的并行危险级别。
 *
 * 返回给定语法树里能找到的最差函数危险属性
 * （本列表中最早的一个：PROPARALLEL_UNSAFE、
 * PROPARALLEL_RESTRICTED、PROPARALLEL_SAFE）。我们用它判断查询
 * 是否完全可以并行化。调用方也会把结果存在
 * PlannerGlobal 里，以便以后对查询树各部分的检查短路，
 * 常见情形是一切都是 SAFE。
 */
char
max_parallel_hazard(Query *parse)
{
	max_parallel_hazard_context context;

	context.max_hazard = PROPARALLEL_SAFE;
	context.max_interesting = PROPARALLEL_UNSAFE;
	context.safe_param_ids = NIL;
	(void) max_parallel_hazard_walker((Node *) parse, &context);
	return context.max_hazard;
}

/*
 * is_parallel_safe
 *		检测给定表达式是否只含并行安全的函数。
 *
 * root->glob->maxParallelHazard 必须事先设成
 * 对整个查询调用 max_parallel_hazard() 的结果。
 */
bool
is_parallel_safe(PlannerInfo *root, Node *node)
{
	max_parallel_hazard_context context;
	PlannerInfo *proot;
	ListCell   *l;

	/*
	 * 即使原查询树不含任何不安全的东西，若规划期间
	 * 生成了任何 PARAM_EXEC Param，仍需要搜索该表达式，
	 * 因为那些是并行受限的，这个表达式里可能有一个。
	 * 否则就不必看。
	 */
	if (root->glob->maxParallelHazard == PROPARALLEL_SAFE &&
		root->glob->paramExecTypes == NIL)
		return true;
	/* 否则用 max_parallel_hazard 的搜索逻辑，但在 RESTRICTED 处停止 */
	context.max_hazard = PROPARALLEL_SAFE;
	context.max_interesting = PROPARALLEL_RESTRICTED;
	context.safe_param_ids = NIL;

	/*
	 * 引用同一查询层次或父查询层次的参数视为
	 * 并行安全。想法是我们在 Gather 或
	 * Gather Merge 节点上计算这些参数，并把值传给工作进程。
	 */
	for (proot = root; proot != NULL; proot = proot->parent_root)
	{
		foreach(l, proot->init_plans)
		{
			SubPlan    *initsubplan = (SubPlan *) lfirst(l);

			context.safe_param_ids = list_concat(context.safe_param_ids,
												 initsubplan->setParam);
		}
	}

	return !max_parallel_hazard_walker(node, &context);
}

/* 所有并行危险检查的核心逻辑 */
static bool
max_parallel_hazard_test(char proparallel, max_parallel_hazard_context *context)
{
	switch (proparallel)
	{
		case PROPARALLEL_SAFE:
			/* 这里没什么可看的，继续 */
			break;
		case PROPARALLEL_RESTRICTED:
			/* 把 max_hazard 提高到 RESTRICTED */
			Assert(context->max_hazard != PROPARALLEL_UNSAFE);
			context->max_hazard = proparallel;
			/* 若不预期有任何不安全函数，就结束 */
			if (context->max_interesting == proparallel)
				return true;
			break;
		case PROPARALLEL_UNSAFE:
			context->max_hazard = proparallel;
			/* 一遇到第一个不安全构造就结束 */
			return true;
		default:
			elog(ERROR, "unrecognized proparallel value \"%c\"", proparallel);
			break;
	}
	return false;
}

/* check_functions_in_node 的回调 */
static bool
max_parallel_hazard_checker(Oid func_id, void *context)
{
	return max_parallel_hazard_test(func_parallel(func_id),
									(max_parallel_hazard_context *) context);
}

static bool
max_parallel_hazard_walker(Node *node, max_parallel_hazard_context *context)
{
	if (node == NULL)
		return false;

	/* 检查节点本身里的危险函数 */
	if (check_functions_in_node(node, max_parallel_hazard_checker,
								context))
		return true;

	/*
	 * 把 MinMaxExpr 当作并行安全应当可以，因为 btree
	 * 操作符类支持函数一般是并行安全的。XmlExpr
	 * 更可疑一些，但大概过得去。我们谨慎起见，
	 * 把 CoerceToDomain 当作并行受限。
	 * （注意：原则上这是错的，因为域约束可能
	 * 含有并行不安全的函数；但有用的约束大概
	 * 永远不会这样。若假定会这样，有域类型时
	 * 并行查询的使用就会严重受限。）SQLValueFunction
	 * 在所有情况下都应当安全。NextValueExpr 是并行不安全的。
	 */
	if (IsA(node, CoerceToDomain))
	{
		if (max_parallel_hazard_test(PROPARALLEL_RESTRICTED, context))
			return true;
	}

	else if (IsA(node, NextValueExpr))
	{
		if (max_parallel_hazard_test(PROPARALLEL_UNSAFE, context))
			return true;
	}

	/*
	 * 把窗口函数当作并行受限，因为我们不确定
	 * 输入行顺序是否完全确定，若不确定，窗口函数的输出
	 * 在不同工作进程之间可能不同。（在某些情形下，
	 * 例如窗口帧按主键排序时，可以放宽
	 * 这个限制。但目前似乎不值得
	 * 为此多花工夫。）
	 */
	else if (IsA(node, WindowFunc))
	{
		if (max_parallel_hazard_test(PROPARALLEL_RESTRICTED, context))
			return true;
	}

	/*
	 * 作为对调用方的记号便利，看穿 RestrictInfo。
	 */
	else if (IsA(node, RestrictInfo))
	{
		RestrictInfo *rinfo = (RestrictInfo *) node;

		return max_parallel_hazard_walker((Node *) rinfo->clause, context);
	}

	/*
	 * 在 max_interesting == restricted 的扫描期间其实不该看到 SubLink，
	 * 但若看到了，就返回真。
	 */
	else if (IsA(node, SubLink))
	{
		if (max_parallel_hazard_test(PROPARALLEL_RESTRICTED, context))
			return true;
	}

	/*
	 * 只有并行安全的 SubPlans 才能发给工作进程。在
	 * SubPlan 的 testexpr 里，代表子计划输出列的
	 * Params 可以当作并行安全，因此检查 testexpr 期间
	 * 暂时把它们的 ID 加进 safe_param_ids 列表。
	 */
	else if (IsA(node, SubPlan))
	{
		SubPlan    *subplan = (SubPlan *) node;
		List	   *save_safe_param_ids;

		if (!subplan->parallel_safe &&
			max_parallel_hazard_test(PROPARALLEL_RESTRICTED, context))
			return true;
		save_safe_param_ids = context->safe_param_ids;
		context->safe_param_ids = list_concat_copy(context->safe_param_ids,
												   subplan->paramIds);
		if (max_parallel_hazard_walker(subplan->testexpr, context))
			return true;		/* 不必恢复 safe_param_ids */
		list_free(context->safe_param_ids);
		context->safe_param_ids = save_safe_param_ids;
		/* 还必须检查 args，但那里没有特殊的 Param 处理 */
		if (max_parallel_hazard_walker((Node *) subplan->args, context))
			return true;
		/* 不想按常规递归，因此到此结束 */
		return false;
	}

	/*
	 * 目前也没法把 Params 传给工作进程，因此它们同样
	 * 是并行受限的，除非它们是 PARAM_EXTERN Params，或是
	 * 列在 safe_param_ids 里的 PARAM_EXEC Params，意思是它们可以
	 * 在工作进程内生成，也可以由主进程算好
	 * 再把值传给工作进程。
	 */
	else if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXTERN)
			return false;

		if (param->paramkind != PARAM_EXEC ||
			!list_member_int(context->safe_param_ids, param->paramid))
		{
			if (max_parallel_hazard_test(PROPARALLEL_RESTRICTED, context))
				return true;
		}
		return false;			/* 没有可递归进入的东西 */
	}

	/*
	 * 当第一次在完全未规划的树上被调用时，必须
	 * 递归进入子查询，以便在树的任何地方
	 * 定位并行不安全的构造。
	 */
	else if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;

		/* SELECT FOR UPDATE/SHARE 必须当作不安全 */
		if (query->rowMarks != NULL)
		{
			context->max_hazard = PROPARALLEL_UNSAFE;
			return true;
		}

		/* 递归进入子选择 */
		return query_tree_walker(query,
								 max_parallel_hazard_walker,
								 context, 0);
	}

	/* 递归以检查参数 */
	return expression_tree_walker(node,
								  max_parallel_hazard_walker,
								  context);
}


/*****************************************************************************
 *		检查子句里的非严格函数
 *****************************************************************************/

/*
 * contain_nonstrict_functions
 *	  在子句里递归查找非严格函数。
 *
 * 若找到任何非严格构造就返回真，也就是
 * NULL 输入仍可能得出非 NULL 输出的东西。
 *
 * 这里的想法是：调用方已经确认表达式含有
 * 一个或多个 Var 或 Param 节点（按调用方的需要），
 * 现在希望证明：这些输入中任何一个为 NULL 时，
 * 表达式结果也会是 NULL。若我们返回假，证明就成功了。
 */
bool
contain_nonstrict_functions(Node *clause)
{
	return contain_nonstrict_functions_walker(clause, NULL);
}

static bool
contain_nonstrict_functions_checker(Oid func_id, void *context)
{
	return !func_strict(func_id);
}

static bool
contain_nonstrict_functions_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		/* 聚集在输入为 null 时仍可能返回非 null */
		return true;
	}
	if (IsA(node, GroupingFunc))
	{
		/*
		 * GroupingFunc 不求值它的参数，因此必须
		 * 当作非严格。
		 */
		return true;
	}
	if (IsA(node, WindowFunc))
	{
		/* 窗口函数在输入为 null 时仍可能返回非 null */
		return true;
	}
	if (IsA(node, SubscriptingRef))
	{
		SubscriptingRef *sbsref = (SubscriptingRef *) node;
		const SubscriptRoutines *sbsroutines;

		/* 下标赋值始终假定为非严格 */
		if (sbsref->refassgnexpr != NULL)
			return true;
		/* 否则必须查找下标支持方法 */
		sbsroutines = getSubscriptingRoutines(sbsref->refcontainertype, NULL);
		if (!(sbsroutines && sbsroutines->fetch_strict))
			return true;
		/* else fall through to check args */
	}
	if (IsA(node, DistinctExpr))
	{
		/* IS DISTINCT FROM 本质上非严格 */
		return true;
	}
	if (IsA(node, NullIfExpr))
	{
		/* NULLIF 本质上非严格 */
		return true;
	}
	if (IsA(node, BoolExpr))
	{
		BoolExpr   *expr = (BoolExpr *) node;

		switch (expr->boolop)
		{
			case AND_EXPR:
			case OR_EXPR:
				/* AND、OR 本质上非严格 */
				return true;
			default:
				break;
		}
	}
	if (IsA(node, SubLink))
	{
		/* 某些情形下子链接可能是严格的，但一般不是 */
		return true;
	}
	if (IsA(node, SubPlan))
		return true;
	if (IsA(node, AlternativeSubPlan))
		return true;
	if (IsA(node, FieldStore))
		return true;
	if (IsA(node, CoerceViaIO))
	{
		/*
		 * 不论 I/O 函数是否严格，CoerceViaIO 都是严格的，
		 * 因此只去看它的参数；去问 check_functions_in_node 是
		 * 无谓的开销，而且可能给出错误答案。
		 */
		return contain_nonstrict_functions_walker((Node *) ((CoerceViaIO *) node)->arg,
												  context);
	}
	if (IsA(node, ArrayCoerceExpr))
	{
		/*
		 * ArrayCoerceExpr 在数组层次上是严格的，不论
		 * 逐元素表达式是什么；因此应当忽略 elemexpr，
		 * 只递归进入 arg。
		 */
		return contain_nonstrict_functions_walker((Node *) ((ArrayCoerceExpr *) node)->arg,
												  context);
	}
	if (IsA(node, CaseExpr))
		return true;
	if (IsA(node, ArrayExpr))
		return true;
	if (IsA(node, RowExpr))
		return true;
	if (IsA(node, RowCompareExpr))
		return true;
	if (IsA(node, CoalesceExpr))
		return true;
	if (IsA(node, MinMaxExpr))
		return true;
	if (IsA(node, XmlExpr))
		return true;
	if (IsA(node, NullTest))
		return true;
	if (IsA(node, BooleanTest))
		return true;
	if (IsA(node, JsonConstructorExpr))
		return true;

	/* 检查其他含函数的节点 */
	if (check_functions_in_node(node, contain_nonstrict_functions_checker,
								context))
		return true;

	return expression_tree_walker(node, contain_nonstrict_functions_walker,
								  context);
}

/*****************************************************************************
 *		检查子句里的 Params
 *****************************************************************************/

/*
 * contain_exec_param
 *	  在子句里递归查找 PARAM_EXEC Params。
 *
 * 若子句含有任何 paramid
 * 出现在给定 Param ID 列表里的 PARAM_EXEC Param，就返回真。不下降进
 * 子查询！
 */
bool
contain_exec_param(Node *clause, List *param_ids)
{
	return contain_exec_param_walker(clause, param_ids);
}

static bool
contain_exec_param_walker(Node *node, List *param_ids)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		Param	   *p = (Param *) node;

		if (p->paramkind == PARAM_EXEC &&
			list_member_int(param_ids, p->paramid))
			return true;
	}
	return expression_tree_walker(node, contain_exec_param_walker, param_ids);
}

/*****************************************************************************
 *		检查子句里依赖上下文的节点
 *****************************************************************************/

/*
 * contain_context_dependent_node
 *	  在子句里递归查找依赖上下文的节点。
 *
 * CaseTestExpr 节点必须直接出现在对应的 CaseExpr 里，
 * 不能嵌套在另一个里面，否则会看到错误的测试值。若
 * 一个 CaseTestExpr 裸露出现在 SQL 函数的参数里，
 * 就不能内联该 SQL 函数，以免造成这种局面。
 * ArrayCoerceExpr 的 elemexpr 里使用的 CaseTestExpr 也一样。
 *
 * 若域 CHECK 表达式能被内联进更大的表达式，
 * CoerceToDomainValue 会有同样的问题，但目前那是不可能的。
 * 不过将来也许会允许，或其他有类似问题的节点类型
 * 也许会被发明出来。因此给本函数一个通用名字，并
 * 把递归状态设成允许多个标志位。
 */
static bool
contain_context_dependent_node(Node *clause)
{
	int			flags = 0;

	return contain_context_dependent_node_walker(clause, &flags);
}

#define CCDN_CASETESTEXPR_OK	0x0001	/* 这里的 CaseTestExpr 可以吗？ */

static bool
contain_context_dependent_node_walker(Node *node, int *flags)
{
	if (node == NULL)
		return false;
	if (IsA(node, CaseTestExpr))
		return !(*flags & CCDN_CASETESTEXPR_OK);
	else if (IsA(node, CaseExpr))
	{
		CaseExpr   *caseexpr = (CaseExpr *) node;

		/*
		 * 若这个 CASE 没有测试表达式，它就不会建立
		 * CaseTestExprs 应当出现的上下文，因此直接
		 * 继续往下，把它当作普通表达式节点。
		 */
		if (caseexpr->arg)
		{
			int			save_flags = *flags;
			bool		res;

			/*
			 * 注意：原则上可以区分 CASE 构造的各个部分，
			 * 只对其中一些设置标志位，
			 * 因为我们只预期 CaseTestExprs 出现在
			 * CaseWhen 节点的 expr 子树里。但为此多写代码
			 * 似乎并不值得。若 CASE 的其他地方还有裸露的 CaseTestExprs，
			 * 那本来就已经出问题了。
			 */
			*flags |= CCDN_CASETESTEXPR_OK;
			res = expression_tree_walker(node,
										 contain_context_dependent_node_walker,
										 (void *) flags);
			*flags = save_flags;
			return res;
		}
	}
	else if (IsA(node, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *ac = (ArrayCoerceExpr *) node;
		int			save_flags;
		bool		res;

		/* 检查数组表达式 */
		if (contain_context_dependent_node_walker((Node *) ac->arg, flags))
			return true;

		/* 检查允许含有 CaseTestExpr 的 elemexpr */
		save_flags = *flags;
		*flags |= CCDN_CASETESTEXPR_OK;
		res = contain_context_dependent_node_walker((Node *) ac->elemexpr,
													flags);
		*flags = save_flags;
		return res;
	}
	return expression_tree_walker(node, contain_context_dependent_node_walker,
								  (void *) flags);
}

/*****************************************************************************
 *		  检查传给非防泄漏函数的 Vars
 *****************************************************************************/

/*
 * contain_leaked_vars
 *		递归扫描子句，看它是否含有任何
 *		（当前查询层次的）Var 节点被传给
 *		会泄漏的函数。
 *
 * 若子句含有任何非防泄漏函数，且这些函数被传入
 * 当前查询层次的 Var 节点，从而可能泄漏
 * 数据，就返回真。这类子句必须施加在任何更低层次的
 * 安全屏障子句之后。
 */
bool
contain_leaked_vars(Node *clause)
{
	return contain_leaked_vars_walker(clause, NULL);
}

static bool
contain_leaked_vars_checker(Oid func_id, void *context)
{
	return !get_func_leakproof(func_id);
}

static bool
contain_leaked_vars_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_Param:
		case T_ArrayExpr:
		case T_FieldSelect:
		case T_FieldStore:
		case T_NamedArgExpr:
		case T_BoolExpr:
		case T_RelabelType:
		case T_CollateExpr:
		case T_CaseExpr:
		case T_CaseTestExpr:
		case T_RowExpr:
		case T_SQLValueFunction:
		case T_NullTest:
		case T_BooleanTest:
		case T_NextValueExpr:
		case T_List:

			/*
			 * 我们知道这些节点类型不含函数调用；但
			 * 节点树更深处也许有。
			 */
			break;

		case T_FuncExpr:
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
		case T_ScalarArrayOpExpr:
		case T_CoerceViaIO:
		case T_ArrayCoerceExpr:

			/*
			 * 若节点含有会泄漏的函数调用，且它下面有任何 Var，
			 * 就拒绝。
			 */
			if (check_functions_in_node(node, contain_leaked_vars_checker,
										context) &&
				contain_var_clause(node))
				return true;
			break;

		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) node;
				const SubscriptRoutines *sbsroutines;

				/* 查阅下标支持方法信息 */
				sbsroutines = getSubscriptingRoutines(sbsref->refcontainertype,
													  NULL);
				if (!sbsroutines ||
					!(sbsref->refassgnexpr != NULL ?
					  sbsroutines->store_leakproof :
					  sbsroutines->fetch_leakproof))
				{
					/* 节点会泄漏，因此若它含有 Vars 就拒绝 */
					if (contain_var_clause(node))
						return true;
				}
			}
			break;

		case T_RowCompareExpr:
			{
				/*
				 * 值得特殊处理，因为会泄漏的比较
				 * 函数只危及一对行元素，而那一对
				 * 也许不含 Vars，其他元素却含有。
				 */
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;
				ListCell   *opid;
				ListCell   *larg;
				ListCell   *rarg;

				forthree(opid, rcexpr->opnos,
						 larg, rcexpr->largs,
						 rarg, rcexpr->rargs)
				{
					Oid			funcid = get_opcode(lfirst_oid(opid));

					if (!get_func_leakproof(funcid) &&
						(contain_var_clause((Node *) lfirst(larg)) ||
						 contain_var_clause((Node *) lfirst(rarg))))
						return true;
				}
			}
			break;

		case T_MinMaxExpr:
			{
				/*
				 * 若 MinMaxExpr 调用的比较函数
				 * 防泄漏，则 MinMaxExpr 也防泄漏。
				 */
				MinMaxExpr *minmaxexpr = (MinMaxExpr *) node;
				TypeCacheEntry *typentry;
				bool		leakproof;

				/* 查找该数据类型的 btree 比较函数 */
				typentry = lookup_type_cache(minmaxexpr->minmaxtype,
											 TYPECACHE_CMP_PROC);
				if (OidIsValid(typentry->cmp_proc))
					leakproof = get_func_leakproof(typentry->cmp_proc);
				else
				{
					/*
					 * 执行器会抛出错误，但这里我们只是
					 * 把缺少的函数当成会泄漏。
					 */
					leakproof = false;
				}

				if (!leakproof &&
					contain_var_clause((Node *) minmaxexpr->args))
					return true;
			}
			break;

		case T_CurrentOfExpr:

			/*
			 * WHERE CURRENT OF 不含会泄漏的函数调用。
			 * 而且必须把它视为不泄漏，
			 * 因为存在 CURRENT OF 时规划器必须始终生成 TID 扫描，
			 * 参见 cost_tidscan。
			 */
			return false;

		default:

			/*
			 * 若不认识该节点标签，就假定它可能会泄漏。
			 * 这防止有人增加一种能调用函数的新
			 * 节点类型时出现意外的安全漏洞。
			 */
			return true;
	}
	return expression_tree_walker(node, contain_leaked_vars_walker,
								  context);
}

/*
 * find_nonnullable_rels
 *		确定给定子句强制哪些基关系为非空。
 *
 * 返回子句里以如下方式引用的全部 Relids：
 * 其中任何一个是全 NULL 行时，该子句就无法
 * 返回 TRUE。（偏保守出错可以接受，因此
 * 这里的分析从简。）
 *
 * 这里的语义与 contain_nonstrict_functions 略有不同：
 * 那个函数关心任意表达式得出的 NULL，
 * 这里则假定输入是布尔表达式，并想看
 * NULL 输入能否证明导致 FALSE 或 NULL 的结果。我们预期
 * 表达式已经做了 AND/OR 展平，并转成了隐式 AND
 * 格式。
 *
 * 注意：本函数很大程度上与 find_nonnullable_vars() 重复。
 * 之所以不把它收成 find_nonnullable_vars() 的薄包装，
 * 是因为被检验的条件确实不同：
 * t1.v1 IS NOT NULL OR t1.v2 IS NOT NULL 这样的子句没法证明
 * v1 或 v2 没法为 NULL，但能证明 t1 这一行
 * 整体没法全为 NULL。对 PHVs 的行为也不同。
 *
 * top_level 在扫描顶层 AND/OR 结构时为真；此时表明
 * 结果为 FALSE 或 NULL 就够了。下降到 NOT 或严格函数之下时
 * top_level 为假：现在必须能证明
 * 子表达式会变成 NULL。
 *
 * 这里不用 expression_tree_walker，因为不想下降
 * 穿过很多类节点，只穿过能确定为严格的那些。
 */
Relids
find_nonnullable_rels(Node *clause)
{
	return find_nonnullable_rels_walker(clause, true);
}

static Relids
find_nonnullable_rels_walker(Node *node, bool top_level)
{
	Relids		result = NULL;
	ListCell   *l;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == 0)
			result = bms_make_singleton(var->varno);
	}
	else if (IsA(node, List))
	{
		/*
		 * 在顶层，我们查看的是隐式 AND 列表：若任一
		 * 分支得出 FALSE 或 NULL，结果就是 FALSE 或 NULL。若
		 * 不在顶层，我们查看的是严格函数的参数：若其中
		 * 任何一个得出 NULL，函数结果
		 * 就必须是 NULL。因此两种情况下，非空
		 * 关系集合都是各分支里找到的那些的并集，并且我们原样
		 * 向下传递 top_level 标志。
		 */
		foreach(l, (List *) node)
		{
			result = bms_join(result,
							  find_nonnullable_rels_walker(lfirst(l),
														   top_level));
		}
	}
	else if (IsA(node, FuncExpr))
	{
		FuncExpr   *expr = (FuncExpr *) node;

		if (func_strict(expr->funcid))
			result = find_nonnullable_rels_walker((Node *) expr->args, false);
	}
	else if (IsA(node, OpExpr))
	{
		OpExpr	   *expr = (OpExpr *) node;

		set_opfuncid(expr);
		if (func_strict(expr->opfuncid))
			result = find_nonnullable_rels_walker((Node *) expr->args, false);
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

		if (is_strict_saop(expr, true))
			result = find_nonnullable_rels_walker((Node *) expr->args, false);
	}
	else if (IsA(node, BoolExpr))
	{
		BoolExpr   *expr = (BoolExpr *) node;

		switch (expr->boolop)
		{
			case AND_EXPR:
				/* 在顶层可以直接递归（到 List 情形） */
				if (top_level)
				{
					result = find_nonnullable_rels_walker((Node *) expr->args,
														  top_level);
					break;
				}

				/*
				 * Below top level, even if one arm produces NULL, the result
				 * could be FALSE (hence not NULL).  However, if *all* the
				 * arms produce NULL then the result is NULL, so we can take
				 * the intersection of the sets of nonnullable rels, just as
				 * for OR.  Fall through to share code.
				 */
				/* FALL THRU */
			case OR_EXPR:

				/*
				 * 若 OR 的全部分支都严格，则 OR 也严格，因此可以取
				 * 各分支非空关系集合的交集。
				 * 这对 top_level 的两个取值都成立。
				 */
				foreach(l, expr->args)
				{
					Relids		subresult;

					subresult = find_nonnullable_rels_walker(lfirst(l),
															 top_level);
					if (result == NULL) /* 第一个子结果？ */
						result = subresult;
					else
						result = bms_int_members(result, subresult);

					/*
					 * 若交集为空，就可以停止查找。这也
					 * 说明了上面那个第一个子结果测试是合理的。
					 */
					if (bms_is_empty(result))
						break;
				}
				break;
			case NOT_EXPR:
				/* 若参数为 null，NOT 会返回 null */
				result = find_nonnullable_rels_walker((Node *) expr->args,
													  false);
				break;
			default:
				elog(ERROR, "unrecognized boolop: %d", (int) expr->boolop);
				break;
		}
	}
	else if (IsA(node, RelabelType))
	{
		RelabelType *expr = (RelabelType *) node;

		result = find_nonnullable_rels_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, CoerceViaIO))
	{
		/* 不清楚这有没有用，但不会有害 */
		CoerceViaIO *expr = (CoerceViaIO *) node;

		result = find_nonnullable_rels_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, ArrayCoerceExpr))
	{
		/* ArrayCoerceExpr 在数组层次上是严格的；忽略 elemexpr */
		ArrayCoerceExpr *expr = (ArrayCoerceExpr *) node;

		result = find_nonnullable_rels_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, ConvertRowtypeExpr))
	{
		/* 不清楚这有没有用，但不会有害 */
		ConvertRowtypeExpr *expr = (ConvertRowtypeExpr *) node;

		result = find_nonnullable_rels_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, CollateExpr))
	{
		CollateExpr *expr = (CollateExpr *) node;

		result = find_nonnullable_rels_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, NullTest))
	{
		/* IS NOT NULL 可以视为严格，但只在顶层 */
		NullTest   *expr = (NullTest *) node;

		if (top_level && expr->nulltesttype == IS_NOT_NULL && !expr->argisrow)
			result = find_nonnullable_rels_walker((Node *) expr->arg, false);
	}
	else if (IsA(node, BooleanTest))
	{
		/* 拒绝 NULL 的布尔测试在顶层是严格的 */
		BooleanTest *expr = (BooleanTest *) node;

		if (top_level &&
			(expr->booltesttype == IS_TRUE ||
			 expr->booltesttype == IS_FALSE ||
			 expr->booltesttype == IS_NOT_UNKNOWN))
			result = find_nonnullable_rels_walker((Node *) expr->arg, false);
	}
	else if (IsA(node, SubPlan))
	{
		SubPlan    *splan = (SubPlan *) node;

		/*
		 * 对某些类型的 SubPlan，可以从 testexpr
		 * （原 SubLink 的左端）里的 Vars 推断严格性。
		 *
		 * 对 ANY_SUBLINK，若子查询产生零行，结果
		 * 总是 FALSE。若子查询产生多于一行，
		 * testexpr 的逐行结果按 OR 语义合并。
		 * 因此 ANY_SUBLINK 只在顶层可以是严格的，但在那里
		 * 它和 testexpr 一样严格。
		 *
		 * 对 ROWCOMPARE_SUBLINK，若子查询产生零行，
		 * 结果总是 NULL。否则结果和 testexpr 一样严格。
		 * 因此无论 top_level 如何都可以检查。
		 *
		 * 对其他子链接类型我们没法证明任何东西（尤其要注意，
		 * ALL_SUBLINK 在子查询为空时会返回 TRUE）。
		 */
		if ((top_level && splan->subLinkType == ANY_SUBLINK) ||
			splan->subLinkType == ROWCOMPARE_SUBLINK)
			result = find_nonnullable_rels_walker(splan->testexpr, top_level);
	}
	else if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/*
		 * 若所含表达式强制任何关系为非空，PHV
		 * 也同样如此。
		 */
		result = find_nonnullable_rels_walker((Node *) phv->phexpr, top_level);

		/*
		 * 若 PHV 的语法作用域恰好是一个关系，它会被强制
		 * 在该关系上求值，因此行为就像该关系的一个 Var：
		 * 若该关系的全部输出变成 null，PHV 也会。
		 * （若语法作用域是连接，我们知道整个连接变成
		 * null 时 PHV 也会；但那是 AND 语义，而
		 * find_nonnullable_rels 的结果需要 OR 语义，因此没法
		 * 利用这一知识做任何事。）
		 */
		if (phv->phlevelsup == 0 &&
			bms_membership(phv->phrels) == BMS_SINGLETON)
			result = bms_add_members(result, phv->phrels);
	}
	return result;
}

/*
 * find_nonnullable_vars
 *		确定给定子句强制哪些 Vars 为非空。
 *
 * 返回子句里以如下方式引用的全部零层 Vars：
 * 其中任何一个为 NULL 时，该子句就无法
 * 返回 TRUE。（偏保守出错可以接受，因此
 * 这里的分析从简。）
 *
 * 这里的语义与 contain_nonstrict_functions 略有不同：
 * 那个函数关心任意表达式得出的 NULL，
 * 这里则假定输入是布尔表达式，并想看
 * NULL 输入能否证明导致 FALSE 或 NULL 的结果。我们预期
 * 表达式已经做了 AND/OR 展平，并转成了隐式 AND
 * 格式。
 *
 * 识别出的 Vars 的属性号放在 multibitmapset（Bitmapsets 的 List）里返回。
 * List 下标对应 relids（varnos），而每个关系的
 * Bitmapsets 保存按 FirstLowInvalidHeapAttributeNumber 偏移的 varattnos。
 *
 * top_level 在扫描顶层 AND/OR 结构时为真；此时表明
 * 结果为 FALSE 或 NULL 就够了。下降到 NOT 或严格函数之下时
 * top_level 为假：现在必须能证明
 * 子表达式会变成 NULL。
 *
 * 这里不用 expression_tree_walker，因为不想下降
 * 穿过很多类节点，只穿过能确定为严格的那些。
 */
List *
find_nonnullable_vars(Node *clause)
{
	return find_nonnullable_vars_walker(clause, true);
}

static List *
find_nonnullable_vars_walker(Node *node, bool top_level)
{
	List	   *result = NIL;
	ListCell   *l;

	if (node == NULL)
		return NIL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == 0)
			result = mbms_add_member(result,
									 var->varno,
									 var->varattno - FirstLowInvalidHeapAttributeNumber);
	}
	else if (IsA(node, List))
	{
		/*
		 * 在顶层，我们查看的是隐式 AND 列表：若任一
		 * 分支得出 FALSE 或 NULL，结果就是 FALSE 或 NULL。若
		 * 不在顶层，我们查看的是严格函数的参数：若其中
		 * 任何一个得出 NULL，函数结果
		 * 就必须是 NULL。因此两种情况下，非空
		 * 变量集合都是各分支里找到的那些的并集，并且我们原样
		 * 向下传递 top_level 标志。
		 */
		foreach(l, (List *) node)
		{
			result = mbms_add_members(result,
									  find_nonnullable_vars_walker(lfirst(l),
																   top_level));
		}
	}
	else if (IsA(node, FuncExpr))
	{
		FuncExpr   *expr = (FuncExpr *) node;

		if (func_strict(expr->funcid))
			result = find_nonnullable_vars_walker((Node *) expr->args, false);
	}
	else if (IsA(node, OpExpr))
	{
		OpExpr	   *expr = (OpExpr *) node;

		set_opfuncid(expr);
		if (func_strict(expr->opfuncid))
			result = find_nonnullable_vars_walker((Node *) expr->args, false);
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

		if (is_strict_saop(expr, true))
			result = find_nonnullable_vars_walker((Node *) expr->args, false);
	}
	else if (IsA(node, BoolExpr))
	{
		BoolExpr   *expr = (BoolExpr *) node;

		switch (expr->boolop)
		{
			case AND_EXPR:

				/*
				 * 在顶层可以直接递归（到 List 情形），因为
				 * 结果应当是我们在每个
				 * 分支里能证明的那些的并集。
				 */
				if (top_level)
				{
					result = find_nonnullable_vars_walker((Node *) expr->args,
														  top_level);
					break;
				}

				/*
				 * Below top level, even if one arm produces NULL, the result
				 * could be FALSE (hence not NULL).  However, if *all* the
				 * arms produce NULL then the result is NULL, so we can take
				 * the intersection of the sets of nonnullable vars, just as
				 * for OR.  Fall through to share code.
				 */
				/* FALL THRU */
			case OR_EXPR:

				/*
				 * 若 OR 的全部分支都严格，则 OR 也严格，因此可以取
				 * 各分支非空变量集合的交集。
				 * 这对 top_level 的两个取值都成立。
				 */
				foreach(l, expr->args)
				{
					List	   *subresult;

					subresult = find_nonnullable_vars_walker(lfirst(l),
															 top_level);
					if (result == NIL)	/* 第一个子结果？ */
						result = subresult;
					else
						result = mbms_int_members(result, subresult);

					/*
					 * 若交集为空，就可以停止查找。这也
					 * 说明了上面那个第一个子结果测试是合理的。
					 */
					if (result == NIL)
						break;
				}
				break;
			case NOT_EXPR:
				/* 若参数为 null，NOT 会返回 null */
				result = find_nonnullable_vars_walker((Node *) expr->args,
													  false);
				break;
			default:
				elog(ERROR, "unrecognized boolop: %d", (int) expr->boolop);
				break;
		}
	}
	else if (IsA(node, RelabelType))
	{
		RelabelType *expr = (RelabelType *) node;

		result = find_nonnullable_vars_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, CoerceViaIO))
	{
		/* 不清楚这有没有用，但不会有害 */
		CoerceViaIO *expr = (CoerceViaIO *) node;

		result = find_nonnullable_vars_walker((Node *) expr->arg, false);
	}
	else if (IsA(node, ArrayCoerceExpr))
	{
		/* ArrayCoerceExpr 在数组层次上是严格的；忽略 elemexpr */
		ArrayCoerceExpr *expr = (ArrayCoerceExpr *) node;

		result = find_nonnullable_vars_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, ConvertRowtypeExpr))
	{
		/* 不清楚这有没有用，但不会有害 */
		ConvertRowtypeExpr *expr = (ConvertRowtypeExpr *) node;

		result = find_nonnullable_vars_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, CollateExpr))
	{
		CollateExpr *expr = (CollateExpr *) node;

		result = find_nonnullable_vars_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, NullTest))
	{
		/* IS NOT NULL 可以视为严格，但只在顶层 */
		NullTest   *expr = (NullTest *) node;

		if (top_level && expr->nulltesttype == IS_NOT_NULL && !expr->argisrow)
			result = find_nonnullable_vars_walker((Node *) expr->arg, false);
	}
	else if (IsA(node, BooleanTest))
	{
		/* 拒绝 NULL 的布尔测试在顶层是严格的 */
		BooleanTest *expr = (BooleanTest *) node;

		if (top_level &&
			(expr->booltesttype == IS_TRUE ||
			 expr->booltesttype == IS_FALSE ||
			 expr->booltesttype == IS_NOT_UNKNOWN))
			result = find_nonnullable_vars_walker((Node *) expr->arg, false);
	}
	else if (IsA(node, SubPlan))
	{
		SubPlan    *splan = (SubPlan *) node;

		/* 见 find_nonnullable_rels_walker 里的分析 */
		if ((top_level && splan->subLinkType == ANY_SUBLINK) ||
			splan->subLinkType == ROWCOMPARE_SUBLINK)
			result = find_nonnullable_vars_walker(splan->testexpr, top_level);
	}
	else if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		result = find_nonnullable_vars_walker((Node *) phv->phexpr, top_level);
	}
	return result;
}

/*
 * find_forced_null_vars
 *		确定哪些 Vars 必须为 NULL，给定子句才会返回 TRUE。
 *
 * 这是 find_nonnullable_vars 的补集：找出零层 Vars，
 * 它们必须为 NULL，子句才会返回 TRUE。（偏保守出错
 * 可以接受，因此这里的分析从简。事实上，
 * 我们只检测顶层简单的 var IS NULL 测试。）
 *
 * 和 find_nonnullable_vars 一样，我们在 multibitmapset 里
 * 返回识别出的 Vars 的 varattnos。
 */
List *
find_forced_null_vars(Node *node)
{
	List	   *result = NIL;
	Var		   *var;
	ListCell   *l;

	if (node == NULL)
		return NIL;
	/* 用子程序检查单子句情形 */
	var = find_forced_null_var(node);
	if (var)
	{
		result = mbms_add_member(result,
								 var->varno,
								 var->varattno - FirstLowInvalidHeapAttributeNumber);
	}
	/* 否则处理 AND 条件 */
	else if (IsA(node, List))
	{
		/*
		 * 在顶层，我们查看的是隐式 AND 列表：若任一
		 * 分支得出 FALSE 或 NULL，结果就是 FALSE 或 NULL。
		 */
		foreach(l, (List *) node)
		{
			result = mbms_add_members(result,
									  find_forced_null_vars((Node *) lfirst(l)));
		}
	}
	else if (IsA(node, BoolExpr))
	{
		BoolExpr   *expr = (BoolExpr *) node;

		/*
		 * 我们懒得考虑 OR 情形，因为很少有人会写
		 * v1 IS NULL OR v1 IS NULL。同样，
		 * NOT 情形也不值得为此写代码。
		 */
		if (expr->boolop == AND_EXPR)
		{
			/* 在顶层可以直接递归（到 List 情形） */
			result = find_forced_null_vars((Node *) expr->args);
		}
	}
	return result;
}

/*
 * find_forced_null_var
 *		返回被给定子句强制为 null 的 Var，若它
 *		不是 IS NULL 类子句则返回 NULL。要成功，该子句必须只强制
 *		这个特定 Var 为空，不能有任何其他条件。
 *
 * 这只是 find_forced_null_vars() 的单子句情形，
 * 不允许 AND 条件。initsplan.c 对单独的
 * 限定子句使用它。之所以不直接套用 find_forced_null_vars()，
 * 是因为若一个 IS NULL 子句与别的东西的 AND 不知怎么
 * 逃过了 AND/OR 展平，initsplan.c 可能会被骗去丢掉
 * 整个子句，而其实只有其中的 IS NULL 部分被证明是多余的。
 */
Var *
find_forced_null_var(Node *node)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, NullTest))
	{
		/* 检查 var IS NULL */
		NullTest   *expr = (NullTest *) node;

		if (expr->nulltesttype == IS_NULL && !expr->argisrow)
		{
			Var		   *var = (Var *) expr->arg;

			if (var && IsA(var, Var) &&
				var->varlevelsup == 0)
				return var;
		}
	}
	else if (IsA(node, BooleanTest))
	{
		/* var IS UNKNOWN 等价于 var IS NULL */
		BooleanTest *expr = (BooleanTest *) node;

		if (expr->booltesttype == IS_UNKNOWN)
		{
			Var		   *var = (Var *) expr->arg;

			if (var && IsA(var, Var) &&
				var->varlevelsup == 0)
				return var;
		}
	}
	return NULL;
}

/*
 * 能否把 ScalarArrayOpExpr 当作严格的？
 *
 * 若 falseOK 为真，则 false 结果可以视为严格，
 * 否则必须保证 NULL 输入得出真正的 NULL 结果。
 *
 * foo op ALL array 在操作符严格且能证明
 * 数组输入不是空数组时是严格的。对数组常量
 * 和 ARRAY[] 构造这两种情形可以做这个检查。
 *
 * foo op ANY array 在 falseOK 意义上是严格的，只要操作符严格。
 * 若不是 falseOK，测试与 foo op ALL array 相同。
 */
static bool
is_strict_saop(ScalarArrayOpExpr *expr, bool falseOK)
{
	Node	   *rightop;

	/* 所含操作符必须是严格的。 */
	set_sa_opfuncid(expr);
	if (!func_strict(expr->opfuncid))
		return false;
	/* 若是 ANY 且 falseOK，需要检查的就这些。 */
	if (expr->useOr && falseOK)
		return true;
	/* 否则必须看数组能否证明非空。 */
	Assert(list_length(expr->args) == 2);
	rightop = (Node *) lsecond(expr->args);
	if (rightop && IsA(rightop, Const))
	{
		Datum		arraydatum = ((Const *) rightop)->constvalue;
		bool		arrayisnull = ((Const *) rightop)->constisnull;
		ArrayType  *arrayval;
		int			nitems;

		if (arrayisnull)
			return false;
		arrayval = DatumGetArrayTypeP(arraydatum);
		nitems = ArrayGetNItems(ARR_NDIM(arrayval), ARR_DIMS(arrayval));
		if (nitems > 0)
			return true;
	}
	else if (rightop && IsA(rightop, ArrayExpr))
	{
		ArrayExpr  *arrayexpr = (ArrayExpr *) rightop;

		if (arrayexpr->elements != NIL && !arrayexpr->multidims)
			return true;
	}
	return false;
}


/*****************************************************************************
 *		检查伪常量子句
 *****************************************************************************/

/*
 * is_pseudo_constant_clause
 *	  检测表达式是否为伪常量，即它不含
 *	  当前查询层次的变量，也不使用易变函数。
 *	  这样的表达式未必是真正的常量：它仍可以含有
 *	  Params 和外层 Vars，更不用说结果
 *	  可能随语句变化的函数。不过，该表达式的值
 *	  在当前查询的任意一次扫描中都是常量，因此可以
 *	  用作例如索引扫描键。（实际上，索引扫描
 *	  键的条件比这更弱；见 is_pseudo_constant_for_index()。）
 *
 * CAUTION：本函数漏掉了一类非常重要的
 * 非常量表达式，即聚集（Aggrefs）。按当前用法
 * 它只施加于 WHERE 子句，因此检查 Aggrefs 会
 * 浪费周期；但若想知道其他上下文里的伪常量性，
 * 务必同时检查 contain_agg_clause()。窗口函数
 * （WindowFuncs）也一样。
 */
bool
is_pseudo_constant_clause(Node *clause)
{
	/*
	 * 本来可以用一次递归扫描实现这个检查。但因为
	 * 易变函数检查既有点贵又不太可能
	 * 失败，看起来最好先找 Vars，只有找不到 Vars 时
	 * 才检查易变函数。
	 */
	if (!contain_var_clause(clause) &&
		!contain_volatile_functions(clause))
		return true;
	return false;
}

/*
 * is_pseudo_constant_clause_relids
 *	  与上面相同，只是调用方已经有了表达式的
 *	  变量成员关系；这样就可以避免 contain_var_clause() 扫描。
 */
bool
is_pseudo_constant_clause_relids(Node *clause, Relids relids)
{
	if (bms_is_empty(relids) &&
		!contain_volatile_functions(clause))
		return true;
	return false;
}


/*****************************************************************************
 *																			 *
 *		通用子句处理例程
 *																			 *
 *****************************************************************************/

/*
 * NumRelids
 *		（原名 clause_relids）。
 *
 * 返回 clause 里引用的不同基关系个数。
 */
int
NumRelids(PlannerInfo *root, Node *clause)
{
	int			result;
	Relids		varnos = pull_varnos(root, clause);

	varnos = bms_del_members(varnos, root->outer_join_rels);
	result = bms_num_members(varnos);
	bms_free(varnos);
	return result;
}

/*
 * CommuteOpExpr：交换二元操作符子句。
 *
 * XXX 该子句会被破坏性修改！
 */
void
CommuteOpExpr(OpExpr *clause)
{
	Oid			opoid;
	Node	   *temp;

	/* 健全性检查：若这些失败，是调用方的错 */
	if (!is_opclause(clause) ||
		list_length(clause->args) != 2)
		elog(ERROR, "cannot commute non-binary-operator clause");

	opoid = get_commutator(clause->opno);

	if (!OidIsValid(opoid))
		elog(ERROR, "could not find commutator for operator %u",
			 clause->opno);

	/*
	 * 就地修改该子句！
	 */
	clause->opno = opoid;
	clause->opfuncid = InvalidOid;
	/* opresulttype、opretset、opcollid、inputcollid 不必改变 */

	temp = linitial(clause->args);
	linitial(clause->args) = lsecond(clause->args);
	lsecond(clause->args) = temp;
}

/*
 * eval_const_expressions 的辅助函数：检查属性的数据类型
 * 是否仍是表达式被解析时的类型。这是为了
 * 防止 ALTER COLUMN TYPE 之后做不当简化。（XXX 我们
 * 很可能还需要在别处做类似检查？）
 *
 * rowtypeid 可能来自整行 Var，因此它可以是复合类型上的域，
 * 但就本目的而言，我们只关心检查
 * 所含列的类型。
 */
static bool
rowtype_field_matches(Oid rowtypeid, int fieldnum,
					  Oid expectedtype, int32 expectedtypmod,
					  Oid expectedcollation)
{
	TupleDesc	tupdesc;
	Form_pg_attribute attr;

	/* 对 RECORD 没有问题，因为没法 ALTER 这种类型 */
	if (rowtypeid == RECORDOID)
		return true;
	tupdesc = lookup_rowtype_tupdesc_domain(rowtypeid, -1, false);
	if (fieldnum <= 0 || fieldnum > tupdesc->natts)
	{
		ReleaseTupleDesc(tupdesc);
		return false;
	}
	attr = TupleDescAttr(tupdesc, fieldnum - 1);
	if (attr->attisdropped ||
		attr->atttypid != expectedtype ||
		attr->atttypmod != expectedtypmod ||
		attr->attcollation != expectedcollation)
	{
		ReleaseTupleDesc(tupdesc);
		return false;
	}
	ReleaseTupleDesc(tupdesc);
	return true;
}


/*--------------------
 * eval_const_expressions
 *
 * Reduce any recognizably constant subexpressions of the given
 * expression tree, for example "2 + 2" => "4".  More interestingly,
 * we can reduce certain boolean expressions even when they contain
 * non-constant subexpressions: "x OR true" => "true" no matter what
 * the subexpression x is.  (XXX We assume that no such subexpression
 * will have important side-effects, which is not necessarily a good
 * assumption in the presence of user-defined functions; do we need a
 * pg_proc flag that prevents discarding the execution of a function?)
 *
 * We do understand that certain functions may deliver non-constant
 * results even with constant inputs, "nextval()" being the classic
 * example.  Functions that are not marked "immutable" in pg_proc
 * will not be pre-evaluated here, although we will reduce their
 * arguments as far as possible.
 *
 * Whenever a function is eliminated from the expression by means of
 * constant-expression evaluation or inlining, we add the function to
 * root->glob->invalItems.  This ensures the plan is known to depend on
 * such functions, even though they aren't referenced anymore.
 *
 * We assume that the tree has already been type-checked and contains
 * only operators and functions that are reasonable to try to execute.
 *
 * NOTE: "root" can be passed as NULL if the caller never wants to do any
 * Param substitutions nor receive info about inlined functions.
 *
 * NOTE: the planner assumes that this will always flatten nested AND and
 * OR clauses into N-argument form.  See comments in prepqual.c.
 *
 * NOTE: another critical effect is that any function calls that require
 * default arguments will be expanded, and named-argument calls will be
 * converted to positional notation.  The executor won't handle either.
 *--------------------
 */
Node *
eval_const_expressions(PlannerInfo *root, Node *node)
{
	eval_const_expressions_context context;

	if (root)
		context.boundParams = root->glob->boundParams;	/* 已绑定的 Params */
	else
		context.boundParams = NULL;
	context.root = root;		/* 供内联函数依赖使用 */
	context.active_fns = NIL;	/* 没有正在被递归简化的东西 */
	context.case_val = NULL;	/* 没有正在检查的 CASE */
	context.estimate = false;	/* 只做安全变换 */
	return eval_const_expressions_mutator(node, &context);
}

#define MIN_ARRAY_SIZE_FOR_HASHED_SAOP 9
/*--------------------
 * convert_saop_to_hashed_saop
 *
 * Recursively search 'node' for ScalarArrayOpExprs and fill in the hash
 * function for any ScalarArrayOpExpr that looks like it would be useful to
 * evaluate using a hash table rather than a linear search.
 *
 * We'll use a hash table if all of the following conditions are met:
 * 1. The 2nd argument of the array contain only Consts.
 * 2. useOr is true or there is a valid negator operator for the
 *	  ScalarArrayOpExpr's opno.
 * 3. There's valid hash function for both left and righthand operands and
 *	  these hash functions are the same.
 * 4. If the array contains enough elements for us to consider it to be
 *	  worthwhile using a hash table rather than a linear search.
 */
void
convert_saop_to_hashed_saop(Node *node)
{
	(void) convert_saop_to_hashed_saop_walker(node, NULL);
}

static bool
convert_saop_to_hashed_saop_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;
		Expr	   *arrayarg = (Expr *) lsecond(saop->args);
		Oid			lefthashfunc;
		Oid			righthashfunc;

		if (arrayarg && IsA(arrayarg, Const) &&
			!((Const *) arrayarg)->constisnull)
		{
			if (saop->useOr)
			{
				if (get_op_hash_functions(saop->opno, &lefthashfunc, &righthashfunc) &&
					lefthashfunc == righthashfunc)
				{
					Datum		arrdatum = ((Const *) arrayarg)->constvalue;
					ArrayType  *arr = (ArrayType *) DatumGetPointer(arrdatum);
					int			nitems;

					/*
					 * 只有当数组看起来
					 * 大到值得做哈希而不是
					 * 线性搜索时，才填入哈希函数。
					 */
					nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));

					if (nitems >= MIN_ARRAY_SIZE_FOR_HASHED_SAOP)
					{
						/* 看起来不错。填入哈希函数 */
						saop->hashfuncid = lefthashfunc;
					}
					return false;
				}
			}
			else				/* !saop->useOr（不是 OR 形式） */
			{
				Oid			negator = get_negator(saop->opno);

				/*
				 * 检查这是不是用可哈希否定符的 NOT IN。
				 * 若是，仍然可以建哈希表，并
				 * 确保查找项不在哈希表里。
				 */
				if (OidIsValid(negator) &&
					get_op_hash_functions(negator, &lefthashfunc, &righthashfunc) &&
					lefthashfunc == righthashfunc)
				{
					Datum		arrdatum = ((Const *) arrayarg)->constvalue;
					ArrayType  *arr = (ArrayType *) DatumGetPointer(arrdatum);
					int			nitems;

					/*
					 * 只有当数组看起来
					 * 大到值得做哈希而不是
					 * 线性搜索时，才填入哈希函数。
					 */
					nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));

					if (nitems >= MIN_ARRAY_SIZE_FOR_HASHED_SAOP)
					{
						/* 看起来不错。填入哈希函数 */
						saop->hashfuncid = lefthashfunc;

						/*
						 * 同时设置 negfuncid。执行器
						 * 做哈希表查找时需要它。
						 */
						saop->negfuncid = get_opcode(negator);
					}
					return false;
				}
			}
		}
	}

	return expression_tree_walker(node, convert_saop_to_hashed_saop_walker, NULL);
}


/*--------------------
 * estimate_expression_value
 *
 * This function attempts to estimate the value of an expression for
 * planning purposes.  It is in essence a more aggressive version of
 * eval_const_expressions(): we will perform constant reductions that are
 * not necessarily 100% safe, but are reasonable for estimation purposes.
 *
 * Currently the extra steps that are taken in this mode are:
 * 1. Substitute values for Params, where a bound Param value has been made
 *	  available by the caller of planner(), even if the Param isn't marked
 *	  constant.  This effectively means that we plan using the first supplied
 *	  value of the Param.
 * 2. Fold stable, as well as immutable, functions to constants.
 * 3. Reduce PlaceHolderVar nodes to their contained expressions.
 *--------------------
 */
Node *
estimate_expression_value(PlannerInfo *root, Node *node)
{
	eval_const_expressions_context context;

	context.boundParams = root->glob->boundParams;	/* 已绑定的 Params */
	/* 不必把计划标记为依赖内联函数 */
	context.root = NULL;
	context.active_fns = NIL;	/* 没有正在被递归简化的东西 */
	context.case_val = NULL;	/* 没有正在检查的 CASE */
	context.estimate = true;	/* 不安全变换也可以 */
	return eval_const_expressions_mutator(node, &context);
}

/*
 * eval_const_expressions_mutator 的通用情形是用
 * expression_tree_mutator 递归，它会原样复制给定节点，但
 * 尽可能对参数（若有）做常量简化。若节点
 * 本身做不可变处理，且每个参数都归约
 * 成了 Const，就可以用 evaluate_expr 把它归约成 Const。（有些
 * 节点类型需要更复杂的逻辑；例如 CASE 表达式
 * 即使并非所有子树都是常量，也可能归约成常量。）
 */
#define ece_generic_processing(node) \
	expression_tree_mutator((Node *) (node), eval_const_expressions_mutator, \
							(void *) context)

/*
 * 检查给定节点的全部参数是否都已归约成 Const。
 * 直接走到 expression_tree_walker 时，contain_non_const_walker
 * 不会施加于节点本身，只施加于其子节点。
 */
#define ece_all_arguments_const(node) \
	(!expression_tree_walker((Node *) (node), contain_non_const_walker, NULL))

/* 用于施加 evaluate_expr 的通用宏 */
#define ece_evaluate_expr(node) \
	((Node *) evaluate_expr((Expr *) (node), \
							exprType((Node *) (node)), \
							exprTypmod((Node *) (node)), \
							exprCollation((Node *) (node))))

/*
 * eval_const_expressions/estimate_expression_value 的递归主体
 */
static Node *
eval_const_expressions_mutator(Node *node,
							   eval_const_expressions_context *context)
{

	/* 因为本函数会递归，可能被推进到栈溢出 */
	check_stack_depth();

	if (node == NULL)
		return NULL;
	switch (nodeTag(node))
	{
		case T_Param:
			{
				Param	   *param = (Param *) node;
				ParamListInfo paramLI = context->boundParams;

				/* 看看是否已经为这个 Param 给出了值 */
				if (param->paramkind == PARAM_EXTERN &&
					paramLI != NULL &&
					param->paramid > 0 &&
					param->paramid <= paramLI->numParams)
				{
					ParamExternData *prm;
					ParamExternData prmdata;

					/*
					 * 给钩子一个机会，以防参数是动态的。告诉
					 * 它这次读取是推测性的，因此若参数不可用，
					 * 它应当避免报错退出。
					 */
					if (paramLI->paramFetch != NULL)
						prm = paramLI->paramFetch(paramLI, param->paramid,
												  true, &prmdata);
					else
						prm = &paramLI->params[param->paramid - 1];

					/*
					 * 我们不只检查 OidIsValid，还坚持
					 * 取到的类型与 Param 匹配，以防钩子做了
					 * 意外的事。不过这里不必抛错；
					 * 留给运行时。
					 */
					if (OidIsValid(prm->ptype) &&
						prm->ptype == param->paramtype)
					{
						/* 可以替换参数值吗？ */
						if (context->estimate ||
							(prm->pflags & PARAM_FLAG_CONST))
						{
							/*
							 * 返回表示参数值的 Const。
							 * 必须复制按引用传递的数据类型，因为
							 * Param 可能处在一个比我们输出计划
							 * 更短命的内存上下文里。
							 */
							int16		typLen;
							bool		typByVal;
							Datum		pval;
							Const	   *con;

							get_typlenbyval(param->paramtype,
											&typLen, &typByVal);
							if (prm->isnull || typByVal)
								pval = prm->value;
							else
								pval = datumCopy(prm->value, typByVal, typLen);
							con = makeConst(param->paramtype,
											param->paramtypmod,
											param->paramcollid,
											(int) typLen,
											pval,
											prm->isnull,
											typByVal);
							con->location = param->location;
							return (Node *) con;
						}
					}
				}

				/*
				 * 不可替换，因此只复制 Param（不必
				 * 递归）
				 */
				return (Node *) copyObject(param);
			}
		case T_WindowFunc:
			{
				WindowFunc *expr = (WindowFunc *) node;
				Oid			funcid = expr->winfnoid;
				List	   *args;
				Expr	   *aggfilter;
				HeapTuple	func_tuple;
				WindowFunc *newexpr;

				/*
				 * We can't really simplify a WindowFunc node, but we mustn't
				 * just fall through to the default processing, because we
				 * have to apply expand_function_arguments to its argument
				 * list.  That takes care of inserting default arguments and
				 * expanding named-argument notation.
				 */
				func_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
				if (!HeapTupleIsValid(func_tuple))
					elog(ERROR, "cache lookup failed for function %u", funcid);

				args = expand_function_arguments(expr->args,
												 false, expr->wintype,
												 func_tuple);

				ReleaseSysCache(func_tuple);

				/* 现在递归简化参数（它们是一个 List） */
				args = (List *)
					expression_tree_mutator((Node *) args,
											eval_const_expressions_mutator,
											(void *) context);
				/* ……以及过滤表达式，它不是 List */
				aggfilter = (Expr *)
					eval_const_expressions_mutator((Node *) expr->aggfilter,
												   context);

				/* 然后构造替换的 WindowFunc 节点 */
				newexpr = makeNode(WindowFunc);
				newexpr->winfnoid = expr->winfnoid;
				newexpr->wintype = expr->wintype;
				newexpr->wincollid = expr->wincollid;
				newexpr->inputcollid = expr->inputcollid;
				newexpr->args = args;
				newexpr->aggfilter = aggfilter;
				newexpr->winref = expr->winref;
				newexpr->winstar = expr->winstar;
				newexpr->winagg = expr->winagg;
				newexpr->location = expr->location;

				return (Node *) newexpr;
			}
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;
				List	   *args = expr->args;
				Expr	   *simple;
				FuncExpr   *newexpr;

				/*
				 * 操作符/函数归约的代码相当长，因此把它拆成
				 * 单独的函数。注意：exprTypmod 对 FuncExpr 通常返回
				 * -1，但当节点明显是
				 * 长度转换时不是；若是那样，我们希望在最终的 Const 里
				 * 保留 typmod。
				 */
				simple = simplify_function(expr->funcid,
										   expr->funcresulttype,
										   exprTypmod(node),
										   expr->funccollid,
										   expr->inputcollid,
										   &args,
										   expr->funcvariadic,
										   true,
										   true,
										   context);
				if (simple)		/* 成功简化了它 */
					return (Node *) simple;

				/*
				 * 该表达式无法再进一步简化，因此构造
				 * 并返回一个替换的 FuncExpr 节点，使用
				 * 可能已简化的参数。注意我们也已经
				 * 把参数列表转换成了位置表示法。
				 */
				newexpr = makeNode(FuncExpr);
				newexpr->funcid = expr->funcid;
				newexpr->funcresulttype = expr->funcresulttype;
				newexpr->funcretset = expr->funcretset;
				newexpr->funcvariadic = expr->funcvariadic;
				newexpr->funcformat = expr->funcformat;
				newexpr->funccollid = expr->funccollid;
				newexpr->inputcollid = expr->inputcollid;
				newexpr->args = args;
				newexpr->location = expr->location;
				return (Node *) newexpr;
			}
		case T_OpExpr:
			{
				OpExpr	   *expr = (OpExpr *) node;
				List	   *args = expr->args;
				Expr	   *simple;
				OpExpr	   *newexpr;

				/*
				 * 需要取得底层函数的 OID。在这个程度上
				 * 就地修改输入是可以的。
				 */
				set_opfuncid(expr);

				/*
				 * 操作符/函数归约的代码相当长，因此把它拆成
				 * 单独的函数。
				 */
				simple = simplify_function(expr->opfuncid,
										   expr->opresulttype, -1,
										   expr->opcollid,
										   expr->inputcollid,
										   &args,
										   false,
										   true,
										   true,
										   context);
				if (simple)		/* 成功简化了它 */
					return (Node *) simple;

				/*
				 * 若操作符是布尔相等或不等，我们知道
				 * 如何简化一个常量加一个
				 * 非常量参数的情形。
				 */
				if (expr->opno == BooleanEqualOperator ||
					expr->opno == BooleanNotEqualOperator)
				{
					simple = (Expr *) simplify_boolean_equality(expr->opno,
																args);
					if (simple) /* 成功简化了它 */
						return (Node *) simple;
				}

				/*
				 * 该表达式无法再进一步简化，因此构造
				 * 并返回一个替换的 OpExpr 节点，使用
				 * 可能已简化的参数。
				 */
				newexpr = makeNode(OpExpr);
				newexpr->opno = expr->opno;
				newexpr->opfuncid = expr->opfuncid;
				newexpr->opresulttype = expr->opresulttype;
				newexpr->opretset = expr->opretset;
				newexpr->opcollid = expr->opcollid;
				newexpr->inputcollid = expr->inputcollid;
				newexpr->args = args;
				newexpr->location = expr->location;
				return (Node *) newexpr;
			}
		case T_DistinctExpr:
			{
				DistinctExpr *expr = (DistinctExpr *) node;
				List	   *args;
				ListCell   *arg;
				bool		has_null_input = false;
				bool		all_null_input = true;
				bool		has_nonconst_input = false;
				Expr	   *simple;
				DistinctExpr *newexpr;

				/*
				 * 归约 DistinctExpr 参数里的常量。我们知
				 * 道 args 要么是 NIL，要么是 List 节点，因此可以直接调用
				 * expression_tree_mutator，而不必递归到
				 * 自身。
				 */
				args = (List *) expression_tree_mutator((Node *) expr->args,
														eval_const_expressions_mutator,
														(void *) context);

				/*
				 * 必须自己检查 NULLs，因为 DistinctExpr 对
				 * NULL 输入的结果与底层
				 * 操作符不同。
				 */
				foreach(arg, args)
				{
					if (IsA(lfirst(arg), Const))
					{
						has_null_input |= ((Const *) lfirst(arg))->constisnull;
						all_null_input &= ((Const *) lfirst(arg))->constisnull;
					}
					else
						has_nonconst_input = true;
				}

				/* 全是常量？那就可以优化掉 */
				if (!has_nonconst_input)
				{
					/* 全是空？那就不是相异 */
					if (all_null_input)
						return makeBoolConst(false, false);

					/* 一个空？那就是相异 */
					if (has_null_input)
						return makeBoolConst(true, false);

					/* 否则尝试求值 '=' 操作符 */
					/* （不过尝试内联它是不可以的！） */

					/*
					 * 需要取得底层函数的 OID。在这个程度上
					 * 就地修改输入是可以的。
					 */
					set_opfuncid((OpExpr *) expr);	/* 依赖结构体
													 * 等价。 */

					/*
					 * 操作符/函数归约的代码相当长，因此把它
					 * 拆成单独的函数。
					 */
					simple = simplify_function(expr->opfuncid,
											   expr->opresulttype, -1,
											   expr->opcollid,
											   expr->inputcollid,
											   &args,
											   false,
											   false,
											   false,
											   context);
					if (simple) /* 成功简化了它 */
					{
						/*
						 * 因为底层操作符是 =，必须否定
						 * 它的结果。
						 */
						Const	   *csimple = castNode(Const, simple);

						csimple->constvalue =
							BoolGetDatum(!DatumGetBool(csimple->constvalue));
						return (Node *) csimple;
					}
				}

				/*
				 * 该表达式无法再进一步简化，因此构造
				 * 并返回一个替换的 DistinctExpr 节点，使用
				 * 可能已简化的参数。
				 */
				newexpr = makeNode(DistinctExpr);
				newexpr->opno = expr->opno;
				newexpr->opfuncid = expr->opfuncid;
				newexpr->opresulttype = expr->opresulttype;
				newexpr->opretset = expr->opretset;
				newexpr->opcollid = expr->opcollid;
				newexpr->inputcollid = expr->inputcollid;
				newexpr->args = args;
				newexpr->location = expr->location;
				return (Node *) newexpr;
			}
		case T_NullIfExpr:
			{
				NullIfExpr *expr;
				ListCell   *arg;
				bool		has_nonconst_input = false;

				/* 复制该节点并对其参数做常量简化 */
				expr = (NullIfExpr *) ece_generic_processing(node);

				/* 若任一参数为 NULL，它们就不可能相等 */
				foreach(arg, expr->args)
				{
					if (!IsA(lfirst(arg), Const))
						has_nonconst_input = true;
					else if (((Const *) lfirst(arg))->constisnull)
						return (Node *) linitial(expr->args);
				}

				/*
				 * 在检查该函数是否适合求值之前，需要取得
				 * 底层函数的 OID。
				 */
				set_opfuncid((OpExpr *) expr);

				if (!has_nonconst_input &&
					ece_function_is_safe(expr->opfuncid, context))
					return ece_evaluate_expr(expr);

				return (Node *) expr;
			}
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *saop;

				/* 复制该节点并对其参数做常量简化 */
				saop = (ScalarArrayOpExpr *) ece_generic_processing(node);

				/* 确保我们知道底层函数 */
				set_sa_opfuncid(saop);

				/*
				 * 若全部参数都是 Const，且函数是安全的，就可以
				 * 折叠成常量
				 */
				if (ece_all_arguments_const(saop) &&
					ece_function_is_safe(saop->opfuncid, context))
					return ece_evaluate_expr(saop);
				return (Node *) saop;
			}
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				switch (expr->boolop)
				{
					case OR_EXPR:
						{
							List	   *newargs;
							bool		haveNull = false;
							bool		forceTrue = false;

							newargs = simplify_or_arguments(expr->args,
															context,
															&haveNull,
															&forceTrue);
							if (forceTrue)
								return makeBoolConst(true, false);
							if (haveNull)
								newargs = lappend(newargs,
												  makeBoolConst(false, true));
							/* 若全部输入都是 FALSE，结果就是 FALSE */
							if (newargs == NIL)
								return makeBoolConst(false, false);

							/*
							 * 若只有一个非常量或 NULL 输入，它就是
							 * 结果
							 */
							if (list_length(newargs) == 1)
								return (Node *) linitial(newargs);
							/* 否则仍然需要一个 OR 节点 */
							return (Node *) make_orclause(newargs);
						}
					case AND_EXPR:
						{
							List	   *newargs;
							bool		haveNull = false;
							bool		forceFalse = false;

							newargs = simplify_and_arguments(expr->args,
															 context,
															 &haveNull,
															 &forceFalse);
							if (forceFalse)
								return makeBoolConst(false, false);
							if (haveNull)
								newargs = lappend(newargs,
												  makeBoolConst(false, true));
							/* 若全部输入都是 TRUE，结果就是 TRUE */
							if (newargs == NIL)
								return makeBoolConst(true, false);

							/*
							 * 若只有一个非常量或 NULL 输入，它就是
							 * 结果
							 */
							if (list_length(newargs) == 1)
								return (Node *) linitial(newargs);
							/* 否则仍然需要一个 AND 节点 */
							return (Node *) make_andclause(newargs);
						}
					case NOT_EXPR:
						{
							Node	   *arg;

							Assert(list_length(expr->args) == 1);
							arg = eval_const_expressions_mutator(linitial(expr->args),
																 context);

							/*
							 * 用 negate_clause() 看能否简化
							 * 掉这个 NOT。
							 */
							return negate_clause(arg);
						}
					default:
						elog(ERROR, "unrecognized boolop: %d",
							 (int) expr->boolop);
						break;
				}
				break;
			}

		case T_JsonValueExpr:
			{
				JsonValueExpr *jve = (JsonValueExpr *) node;
				Node	   *raw_expr = (Node *) jve->raw_expr;
				Node	   *formatted_expr = (Node *) jve->formatted_expr;

				/*
				 * 若能把 formatted_expr 折叠成常量，就可以整个
				 * 省掉 JsonValueExpr。否则还必须处理
				 * raw_expr。但 JsonFormat 是扁平节点，
				 * 不需要简化，只要复制。
				 */
				formatted_expr = eval_const_expressions_mutator(formatted_expr,
																context);
				if (formatted_expr && IsA(formatted_expr, Const))
					return formatted_expr;

				raw_expr = eval_const_expressions_mutator(raw_expr, context);

				return (Node *) makeJsonValueExpr((Expr *) raw_expr,
												  (Expr *) formatted_expr,
												  copyObject(jve->format));
			}

		case T_SubPlan:
		case T_AlternativeSubPlan:

			/*
			 * 原样返回 SubPlan，已经来不及对它做任何事。
			 *
			 * XXX 是否应当改为在这里 ereport()？大概本例程
			 * 在创建 SubPlan 之后不应当再被调用。
			 */
			return node;
		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) node;
				Node	   *arg;

				/* 简化输入…… */
				arg = eval_const_expressions_mutator((Node *) relabel->arg,
													 context);
				/* ……并在需要时挂上一个新的 RelabelType 节点 */
				return applyRelabelType(arg,
										relabel->resulttype,
										relabel->resulttypmod,
										relabel->resultcollid,
										relabel->relabelformat,
										relabel->location,
										true);
			}
		case T_CoerceViaIO:
			{
				CoerceViaIO *expr = (CoerceViaIO *) node;
				List	   *args;
				Oid			outfunc;
				bool		outtypisvarlena;
				Oid			infunc;
				Oid			intypioparam;
				Expr	   *simple;
				CoerceViaIO *newexpr;

				/* 造一个 List，以便使用 simplify_function */
				args = list_make1(expr->arg);

				/*
				 * CoerceViaIO 表示先调用源类型的输出
				 * 函数，再调用结果类型的输入函数。因此，试着
				 * 把它当成这样一叠两个函数
				 * 调用来简化。首先需要知道这些函数是什么。
				 *
				 * 注意假定转换函数不关心
				 * 输入排序规则，因此那里只传 InvalidOid。
				 */
				getTypeOutputInfo(exprType((Node *) expr->arg),
								  &outfunc, &outtypisvarlena);
				getTypeInputInfo(expr->resulttype,
								 &infunc, &intypioparam);

				simple = simplify_function(outfunc,
										   CSTRINGOID, -1,
										   InvalidOid,
										   InvalidOid,
										   &args,
										   false,
										   true,
										   true,
										   context);
				if (simple)		/* 成功简化了输出函数 */
				{
					/*
					 * 输入函数可能需要 1 到 3 个参数。我们始终
					 * 提供全部三个，相信下游不会
					 * 抱怨。
					 */
					args = list_make3(simple,
									  makeConst(OIDOID,
												-1,
												InvalidOid,
												sizeof(Oid),
												ObjectIdGetDatum(intypioparam),
												false,
												true),
									  makeConst(INT4OID,
												-1,
												InvalidOid,
												sizeof(int32),
												Int32GetDatum(-1),
												false,
												true));

					simple = simplify_function(infunc,
											   expr->resulttype, -1,
											   expr->resultcollid,
											   InvalidOid,
											   &args,
											   false,
											   false,
											   true,
											   context);
					if (simple) /* 成功简化了输入函数 */
						return (Node *) simple;
				}

				/*
				 * 该表达式无法再进一步简化，因此构造
				 * 并返回一个替换的 CoerceViaIO 节点，使用
				 * 可能已简化的参数。
				 */
				newexpr = makeNode(CoerceViaIO);
				newexpr->arg = (Expr *) linitial(args);
				newexpr->resulttype = expr->resulttype;
				newexpr->resultcollid = expr->resultcollid;
				newexpr->coerceformat = expr->coerceformat;
				newexpr->location = expr->location;
				return (Node *) newexpr;
			}
		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *ac = makeNode(ArrayCoerceExpr);
				Node	   *save_case_val;

				/*
				 * 复制该节点并对其参数做常量简化。这里不能
				 * 用 ece_generic_processing()，因为只在处理 elemexpr 期间
				 * 才需要折腾 case_val。
				 */
				memcpy(ac, node, sizeof(ArrayCoerceExpr));
				ac->arg = (Expr *)
					eval_const_expressions_mutator((Node *) ac->arg,
												   context);

				/*
				 * 为 elemexpr 里所含的 CaseTestExpr 节点做准备。
				 * 必须防止它吸收任何外层 CASE 值。
				 */
				save_case_val = context->case_val;
				context->case_val = NULL;

				ac->elemexpr = (Expr *)
					eval_const_expressions_mutator((Node *) ac->elemexpr,
												   context);

				context->case_val = save_case_val;

				/*
				 * 若参数是常量且逐元素表达式
				 * 不可变，就可以把整个东西简化成常量。
				 * 例外：虽然 contain_mutable_functions 出于历史原因把
				 * CoerceToDomain 当作不可变，这里不要
				 * 那样做；这保证到数组域的转换
				 * 要到运行时才施加域约束。
				 */
				if (ac->arg && IsA(ac->arg, Const) &&
					ac->elemexpr && !IsA(ac->elemexpr, CoerceToDomain) &&
					!contain_mutable_functions((Node *) ac->elemexpr))
					return ece_evaluate_expr(ac);

				return (Node *) ac;
			}
		case T_CollateExpr:
			{
				/*
				 * 我们用 RelabelType 替换 CollateExpr，以便提高
				 * 表达式表示的一致性，从而简化
				 * 表达式比较。因此这看起来几乎和
				 * RelabelType 情形一样，也可以用同样的
				 * 优化来避免不必要的 RelabelTypes。
				 */
				CollateExpr *collate = (CollateExpr *) node;
				Node	   *arg;

				/* 简化输入…… */
				arg = eval_const_expressions_mutator((Node *) collate->arg,
													 context);
				/* ……并在需要时挂上一个新的 RelabelType 节点 */
				return applyRelabelType(arg,
										exprType(arg),
										exprTypmod(arg),
										collate->collOid,
										COERCE_IMPLICIT_CAST,
										collate->location,
										true);
			}
		case T_CaseExpr:
			{
				/*----------
				 * CASE expressions can be simplified if there are constant
				 * condition clauses:
				 *		FALSE (or NULL): drop the alternative
				 *		TRUE: drop all remaining alternatives
				 * If the first non-FALSE alternative is a constant TRUE,
				 * we can simplify the entire CASE to that alternative's
				 * expression.  If there are no non-FALSE alternatives,
				 * we simplify the entire CASE to the default result (ELSE).
				 *
				 * If we have a simple-form CASE with constant test
				 * expression, we substitute the constant value for contained
				 * CaseTestExpr placeholder nodes, so that we have the
				 * opportunity to reduce constant test conditions.  For
				 * example this allows
				 *		CASE 0 WHEN 0 THEN 1 ELSE 1/0 END
				 * to reduce to 1 rather than drawing a divide-by-0 error.
				 * Note that when the test expression is constant, we don't
				 * have to include it in the resulting CASE; for example
				 *		CASE 0 WHEN x THEN y ELSE z END
				 * is transformed by the parser to
				 *		CASE 0 WHEN CaseTestExpr = x THEN y ELSE z END
				 * which we can simplify to
				 *		CASE WHEN 0 = x THEN y ELSE z END
				 * It is not necessary for the executor to evaluate the "arg"
				 * expression when executing the CASE, since any contained
				 * CaseTestExprs that might have referred to it will have been
				 * replaced by the constant.
				 *----------
				 */
				CaseExpr   *caseexpr = (CaseExpr *) node;
				CaseExpr   *newcase;
				Node	   *save_case_val;
				Node	   *newarg;
				List	   *newargs;
				bool		const_true_cond;
				Node	   *defresult = NULL;
				ListCell   *arg;

				/* 若有测试表达式，就简化它 */
				newarg = eval_const_expressions_mutator((Node *) caseexpr->arg,
														context);

				/* 为所含的 CaseTestExpr 节点做准备 */
				save_case_val = context->case_val;
				if (newarg && IsA(newarg, Const))
				{
					context->case_val = newarg;
					newarg = NULL;	/* 不再需要，见上面 */
				}
				else
					context->case_val = NULL;

				/* 简化 WHEN 子句 */
				newargs = NIL;
				const_true_cond = false;
				foreach(arg, caseexpr->args)
				{
					CaseWhen   *oldcasewhen = lfirst_node(CaseWhen, arg);
					Node	   *casecond;
					Node	   *caseresult;

					/* 简化这个候选的测试条件 */
					casecond = eval_const_expressions_mutator((Node *) oldcasewhen->expr,
															  context);

					/*
					 * 若测试条件是常量 FALSE（或 NULL），就
					 * 完全丢掉这个 WHEN 子句，而不处理
					 * 结果。
					 */
					if (casecond && IsA(casecond, Const))
					{
						Const	   *const_input = (Const *) casecond;

						if (const_input->constisnull ||
							!DatumGetBool(const_input->constvalue))
							continue;	/* 丢掉条件为 FALSE 的候选 */
						/* 否则它是常量 TRUE */
						const_true_cond = true;
					}

					/* 简化这个候选的结果值 */
					caseresult = eval_const_expressions_mutator((Node *) oldcasewhen->result,
																context);

					/* 若测试条件不是常量，就发出一个新的 WHEN 节点 */
					if (!const_true_cond)
					{
						CaseWhen   *newcasewhen = makeNode(CaseWhen);

						newcasewhen->expr = (Expr *) casecond;
						newcasewhen->result = (Expr *) caseresult;
						newcasewhen->location = oldcasewhen->location;
						newargs = lappend(newargs, newcasewhen);
						continue;
					}

					/*
					 * 找到了 TRUE 条件，因此其余
					 * 候选都到达不了。我们把该结果当作
					 * 默认结果。
					 */
					defresult = caseresult;
					break;
				}

				/* 简化默认结果，除非上面已经替换过它 */
				if (!const_true_cond)
					defresult = eval_const_expressions_mutator((Node *) caseexpr->defresult,
															   context);

				context->case_val = save_case_val;

				/*
				 * 若没有非 FALSE 的候选，CASE 就归约成默认
				 * 结果。
				 */
				if (newargs == NIL)
					return defresult;
				/* 否则需要一个新的 CASE 节点 */
				newcase = makeNode(CaseExpr);
				newcase->casetype = caseexpr->casetype;
				newcase->casecollid = caseexpr->casecollid;
				newcase->arg = (Expr *) newarg;
				newcase->args = newargs;
				newcase->defresult = (Expr *) defresult;
				newcase->location = caseexpr->location;
				return (Node *) newcase;
			}
		case T_CaseTestExpr:
			{
				/*
				 * 若知道当前 CASE 构造的常量测试值，
				 * 就用它替换占位符。否则
				 * 原样返回占位符。
				 */
				if (context->case_val)
					return copyObject(context->case_val);
				else
					return copyObject(node);
			}
		case T_SubscriptingRef:
		case T_ArrayExpr:
		case T_RowExpr:
		case T_MinMaxExpr:
			{
				/*
				 * 对自身处理已知是不可变的节点类型做通用处理，
				 * 并且除了所有输入都是常量就简化之外，
				 * 不需要别的智能。
				 *
				 * 这样对待 SubscriptingRef，等于假定下标
				 * 读取和赋值都是不可变的。这约束了
				 * 特定类型的下标实现；也许将来
				 * 应当放宽。
				 *
				 * 这样对待 MinMaxExpr，等于假定它调用的
				 * btree 比较函数是不可变的；理由见
				 * contain_mutable_functions_walker。
				 */

				/* 复制该节点并对其参数做常量简化 */
				node = ece_generic_processing(node);
				/* 若全部参数都是 Const，就可以折叠成常量 */
				if (ece_all_arguments_const(node))
					return ece_evaluate_expr(node);
				return node;
			}
		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesceexpr = (CoalesceExpr *) node;
				CoalesceExpr *newcoalesce;
				List	   *newargs;
				ListCell   *arg;

				newargs = NIL;
				foreach(arg, coalesceexpr->args)
				{
					Node	   *e;

					e = eval_const_expressions_mutator((Node *) lfirst(arg),
													   context);

					/*
					 * 可以从列表里去掉空常量。对
					 * 非空常量，若它前面还没有
					 * 其他非空常量表达式，它就是
					 * 结果。否则它是下一个参数，但可以
					 * 丢掉后面的参数，因为它们永远
					 * 不会被到达。
					 */
					if (IsA(e, Const))
					{
						if (((Const *) e)->constisnull)
							continue;	/* 丢掉空常量 */
						if (newargs == NIL)
							return e;	/* 第一个表达式 */
						newargs = lappend(newargs, e);
						break;
					}
					newargs = lappend(newargs, e);
				}

				/*
				 * 若全部参数都是常量空，结果就只是
				 * 空
				 */
				if (newargs == NIL)
					return (Node *) makeNullConst(coalesceexpr->coalescetype,
												  -1,
												  coalesceexpr->coalescecollid);

				newcoalesce = makeNode(CoalesceExpr);
				newcoalesce->coalescetype = coalesceexpr->coalescetype;
				newcoalesce->coalescecollid = coalesceexpr->coalescecollid;
				newcoalesce->args = newargs;
				newcoalesce->location = coalesceexpr->location;
				return (Node *) newcoalesce;
			}
		case T_SQLValueFunction:
			{
				/*
				 * SQLValueFunction 的所有变体都是稳定的，因此若正在
				 * 估计表达式的值，就应当求值
				 * 当前函数值。否则只复制。
				 */
				SQLValueFunction *svf = (SQLValueFunction *) node;

				if (context->estimate)
					return (Node *) evaluate_expr((Expr *) svf,
												  svf->type,
												  svf->typmod,
												  InvalidOid);
				else
					return copyObject((Node *) svf);
			}
		case T_FieldSelect:
			{
				/*
				 * 可以把整行 Var 的列选取优化成
				 * 简单 Var。（解析器不会直接生成这种情况，因为
				 * ParseComplexProjection 会把它短路。
				 * 但简化函数时可能出现。）另外，也可以
				 * 优化来自 RowExpr 构造的列选取，当然也可以
				 * 优化来自常量的列选取。
				 *
				 * 不过，这样替换整行 Var 有一个
				 * 陷阱：若已经为源关系建好了关系目标列表，则
				 * 整行 Var 已安排由关系扫描产生，但简单 Var 大概
				 * 没有，这会在 setrefs.c 里失败。处理简单的
				 * 单层查询时这不是问题，因为
				 * 表达式简化总是先发生。不过，
				 * 来自子查询的 lateral 引用却有风险。
				 * 为避免这种失败，不要优化
				 * 上层引用。
				 *
				 * 还必须检查该列的声明类型是否仍与
				 * 创建 FieldSelect 时相同。若有人对行类型做了
				 * ALTER COLUMN TYPE，类型可能已经变了。
				 * 若不相同，就跳过这个优化；该情况大概会在运行时失败，
				 * 但那不是这里要管的问题。
				 */
				FieldSelect *fselect = (FieldSelect *) node;
				FieldSelect *newfselect;
				Node	   *arg;

				arg = eval_const_expressions_mutator((Node *) fselect->arg,
													 context);
				if (arg && IsA(arg, Var) &&
					((Var *) arg)->varattno == InvalidAttrNumber &&
					((Var *) arg)->varlevelsup == 0)
				{
					if (rowtype_field_matches(((Var *) arg)->vartype,
											  fselect->fieldnum,
											  fselect->resulttype,
											  fselect->resulttypmod,
											  fselect->resultcollid))
					{
						Var		   *newvar;

						newvar = makeVar(((Var *) arg)->varno,
										 fselect->fieldnum,
										 fselect->resulttype,
										 fselect->resulttypmod,
										 fselect->resultcollid,
										 ((Var *) arg)->varlevelsup);
						/* 新 Var 与旧 Var 由同样的关系决定可空性 */
						newvar->varnullingrels = ((Var *) arg)->varnullingrels;
						return (Node *) newvar;
					}
				}
				if (arg && IsA(arg, RowExpr))
				{
					RowExpr    *rowexpr = (RowExpr *) arg;

					if (fselect->fieldnum > 0 &&
						fselect->fieldnum <= list_length(rowexpr->args))
					{
						Node	   *fld = (Node *) list_nth(rowexpr->args,
															fselect->fieldnum - 1);

						if (rowtype_field_matches(rowexpr->row_typeid,
												  fselect->fieldnum,
												  fselect->resulttype,
												  fselect->resulttypmod,
												  fselect->resultcollid) &&
							fselect->resulttype == exprType(fld) &&
							fselect->resulttypmod == exprTypmod(fld) &&
							fselect->resultcollid == exprCollation(fld))
							return fld;
					}
				}
				newfselect = makeNode(FieldSelect);
				newfselect->arg = (Expr *) arg;
				newfselect->fieldnum = fselect->fieldnum;
				newfselect->resulttype = fselect->resulttype;
				newfselect->resulttypmod = fselect->resulttypmod;
				newfselect->resultcollid = fselect->resultcollid;
				if (arg && IsA(arg, Const))
				{
					Const	   *con = (Const *) arg;

					if (rowtype_field_matches(con->consttype,
											  newfselect->fieldnum,
											  newfselect->resulttype,
											  newfselect->resulttypmod,
											  newfselect->resultcollid))
						return ece_evaluate_expr(newfselect);
				}
				return (Node *) newfselect;
			}
		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;
				NullTest   *newntest;
				Node	   *arg;

				arg = eval_const_expressions_mutator((Node *) ntest->arg,
													 context);
				if (ntest->argisrow && arg && IsA(arg, RowExpr))
				{
					/*
					 * 我们把 ROW(...) IS [NOT] NULL 拆成对其
					 * 各个成员的单独测试。这种形式通常
					 * 求值更高效，也更便于
					 * 优化。
					 */
					RowExpr    *rarg = (RowExpr *) arg;
					List	   *newargs = NIL;
					ListCell   *l;

					foreach(l, rarg->args)
					{
						Node	   *relem = (Node *) lfirst(l);

						/*
						 * 常量成员若空性不对，就会否定整个 NullTest；
						 * 否则可以丢掉它。
						 */
						if (relem && IsA(relem, Const))
						{
							Const	   *carg = (Const *) relem;

							if (carg->constisnull ?
								(ntest->nulltesttype == IS_NOT_NULL) :
								(ntest->nulltesttype == IS_NULL))
								return makeBoolConst(false, false);
							continue;
						}

						/*
						 * 否则，为这个成员做一个标量（argisrow == false）的 NullTest。
						 * 需要标量语义，
						 * 因为 IS [NOT] NULL 不递归；见
						 * ExecEvalRowNullInt() 里的注释。
						 */
						newntest = makeNode(NullTest);
						newntest->arg = (Expr *) relem;
						newntest->nulltesttype = ntest->nulltesttype;
						newntest->argisrow = false;
						newntest->location = ntest->location;
						newargs = lappend(newargs, newntest);
					}
					/* 若全部输入都是常量，结果就是 TRUE */
					if (newargs == NIL)
						return makeBoolConst(true, false);
					/* 若只有一个非常量输入，它就是结果 */
					if (list_length(newargs) == 1)
						return (Node *) linitial(newargs);
					/* 否则需要一个 AND 节点 */
					return (Node *) make_andclause(newargs);
				}
				if (!ntest->argisrow && arg && IsA(arg, Const))
				{
					Const	   *carg = (Const *) arg;
					bool		result;

					switch (ntest->nulltesttype)
					{
						case IS_NULL:
							result = carg->constisnull;
							break;
						case IS_NOT_NULL:
							result = !carg->constisnull;
							break;
						default:
							elog(ERROR, "unrecognized nulltesttype: %d",
								 (int) ntest->nulltesttype);
							result = false; /* 消除编译器告警 */
							break;
					}

					return makeBoolConst(result, false);
				}

				newntest = makeNode(NullTest);
				newntest->arg = (Expr *) arg;
				newntest->nulltesttype = ntest->nulltesttype;
				newntest->argisrow = ntest->argisrow;
				newntest->location = ntest->location;
				return (Node *) newntest;
			}
		case T_BooleanTest:
			{
				/*
				 * 这个情形本可以并进 ArrayExpr 等用的通用处理。
				 * 但简化逻辑
				 * 太简单，用 evaluate_expr() 来做会是
				 * 很重的开销。BooleanTest 大概足够常见，
				 * 值得保留这个专门实现。
				 */
				BooleanTest *btest = (BooleanTest *) node;
				BooleanTest *newbtest;
				Node	   *arg;

				arg = eval_const_expressions_mutator((Node *) btest->arg,
													 context);
				if (arg && IsA(arg, Const))
				{
					Const	   *carg = (Const *) arg;
					bool		result;

					switch (btest->booltesttype)
					{
						case IS_TRUE:
							result = (!carg->constisnull &&
									  DatumGetBool(carg->constvalue));
							break;
						case IS_NOT_TRUE:
							result = (carg->constisnull ||
									  !DatumGetBool(carg->constvalue));
							break;
						case IS_FALSE:
							result = (!carg->constisnull &&
									  !DatumGetBool(carg->constvalue));
							break;
						case IS_NOT_FALSE:
							result = (carg->constisnull ||
									  DatumGetBool(carg->constvalue));
							break;
						case IS_UNKNOWN:
							result = carg->constisnull;
							break;
						case IS_NOT_UNKNOWN:
							result = !carg->constisnull;
							break;
						default:
							elog(ERROR, "unrecognized booltesttype: %d",
								 (int) btest->booltesttype);
							result = false; /* 消除编译器告警 */
							break;
					}

					return makeBoolConst(result, false);
				}

				newbtest = makeNode(BooleanTest);
				newbtest->arg = (Expr *) arg;
				newbtest->booltesttype = btest->booltesttype;
				newbtest->location = btest->location;
				return (Node *) newbtest;
			}
		case T_CoerceToDomain:
			{
				/*
				 * 若域当前没有约束，就把
				 * CoerceToDomain 节点换成简单的 RelabelType，它
				 * 执行起来快得多，也更便于以后
				 * 优化。然后必须把计划标记为：若域约束改变
				 * 就需要重建。
				 *
				 * 另外，在估计模式下，始终替换 CoerceToDomain
				 * 节点，实际上假定转换会成功。
				 */
				CoerceToDomain *cdomain = (CoerceToDomain *) node;
				CoerceToDomain *newcdomain;
				Node	   *arg;

				arg = eval_const_expressions_mutator((Node *) cdomain->arg,
													 context);
				if (context->estimate ||
					!DomainHasConstraints(cdomain->resulttype))
				{
					/* 若这不是估计模式，就记录依赖 */
					if (context->root && !context->estimate)
						record_plan_type_dependency(context->root,
													cdomain->resulttype);

					/* 生成 RelabelType 来替换 CoerceToDomain */
					return applyRelabelType(arg,
											cdomain->resulttype,
											cdomain->resulttypmod,
											cdomain->resultcollid,
											cdomain->coercionformat,
											cdomain->location,
											true);
				}

				newcdomain = makeNode(CoerceToDomain);
				newcdomain->arg = (Expr *) arg;
				newcdomain->resulttype = cdomain->resulttype;
				newcdomain->resulttypmod = cdomain->resulttypmod;
				newcdomain->resultcollid = cdomain->resultcollid;
				newcdomain->coercionformat = cdomain->coercionformat;
				newcdomain->location = cdomain->location;
				return (Node *) newcdomain;
			}
		case T_PlaceHolderVar:

			/*
			 * 在估计模式下，直接整个剥掉 PlaceHolderVar 节点；
			 * 这等于估计所含的值
			 * 不会被外连接强制成 null。在常规模式下我们
			 * 只用默认行为（即简化表达式，但
			 * 保留 PlaceHolderVar 节点）。
			 */
			if (context->estimate)
			{
				PlaceHolderVar *phv = (PlaceHolderVar *) node;

				return eval_const_expressions_mutator((Node *) phv->phexpr,
													  context);
			}
			break;
		case T_ConvertRowtypeExpr:
			{
				ConvertRowtypeExpr *cre = castNode(ConvertRowtypeExpr, node);
				Node	   *arg;
				ConvertRowtypeExpr *newcre;

				arg = eval_const_expressions_mutator((Node *) cre->arg,
													 context);

				newcre = makeNode(ConvertRowtypeExpr);
				newcre->resulttype = cre->resulttype;
				newcre->convertformat = cre->convertformat;
				newcre->location = cre->location;

				/*
				 * 若出现嵌套的 ConvertRowtypeExpr，可以把
				 * 叶子行直接转换成最外层行格式，不必做任何
				 * 中间转换。（这能成立是因为
				 * ConvertRowtypeExpr 只用于继承树里的子到父
				 * 转换，靠列名精确匹配，而中间
				 * 结果里没有的列不可能出现在
				 * 最终结果里。）
				 *
				 * 不必检查超过一层，因为
				 * 上面的递归已经展平了其他东西。
				 */
				if (arg != NULL && IsA(arg, ConvertRowtypeExpr))
				{
					ConvertRowtypeExpr *argcre = (ConvertRowtypeExpr *) arg;

					arg = (Node *) argcre->arg;

					/*
					 * 确保外层隐式转换没法藏住
					 * 内层显式转换。
					 */
					if (newcre->convertformat == COERCE_IMPLICIT_CAST)
						newcre->convertformat = argcre->convertformat;
				}

				newcre->arg = (Expr *) arg;

				if (arg != NULL && IsA(arg, Const))
					return ece_evaluate_expr((Node *) newcre);
				return (Node *) newcre;
			}
		default:
			break;
	}

	/*
	 * 对上面没处理的节点类型，原样复制节点，但
	 * 对其子表达式做常量简化。对行为可能在规划
	 * 和执行之间改变的节点类型，这样做是对的，例如
	 * CurrentOfExpr。对本例程不认识的新节点类型，
	 * 这也是安全的默认。
	 */
	return ece_generic_processing(node);
}

/*
 * eval_const_expressions 的子程序：检查非 Const 节点。
 *
 * 一发现非 Const 节点就可以立即中止递归。这对
 * 性能至关重要，否则 eval_const_expressions_mutator 在
 * 无法简化的树上会花 O(N^2) 时间。不过，我们确实需要下降
 * 进 List 节点，因为 expression_tree_walker 有时直接对
 * List 子树调用遍历函数。
 */
static bool
contain_non_const_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Const))
		return false;
	if (IsA(node, List))
		return expression_tree_walker(node, contain_non_const_walker, context);
	/* 否则中止树遍历并返回真 */
	return true;
}

/*
 * eval_const_expressions 的子程序：检查函数是否适合求值
 */
static bool
ece_function_is_safe(Oid funcid, eval_const_expressions_context *context)
{
	char		provolatile = func_volatile(funcid);

	/*
	 * 通常只允许简化不可变函数。但为了
	 * 估计，我们认为简化仅仅稳定的函数也可以；
	 * 结果从规划时刻变到执行时刻的风险，
	 * 比起完全没法估计这个值，
	 * 是值得承担的。
	 */
	if (provolatile == PROVOLATILE_IMMUTABLE)
		return true;
	if (context->estimate && provolatile == PROVOLATILE_STABLE)
		return true;
	return false;
}

/*
 * eval_const_expressions 的子程序：处理 OR 子句的参数。
 *
 * 这包括展平嵌套的 OR，以及递归调用
 * eval_const_expressions 来简化 OR 参数。
 *
 * 简化之后，OR 参数按如下方式处理：
 *		非常量：保留
 *		FALSE：丢掉（不影响结果）
 *		TRUE：把结果强制为 TRUE
 *		NULL：只保留一个
 * 必须保留一个 NULL 输入，因为没有任何
 * 输入为 TRUE 且至少有一个为 NULL 时，OR 表达式求值为 NULL。这里其实不把 NULL
 * 放进来，那应当由调用方来做。
 *
 * 输出参数 *haveNull 和 *forceTrue 必须由调用方
 * 初始化为假。若在参数列表的任何地方发现 NULL 常量或 TRUE 常量，
 * 它们会分别被设为真。
 */
static List *
simplify_or_arguments(List *args,
					  eval_const_expressions_context *context,
					  bool *haveNull, bool *forceTrue)
{
	List	   *newargs = NIL;
	List	   *unprocessed_args;

	/*
	 * 我们希望保证紧挨在另一个 OR 下面的任何 OR 都被
	 * 展平成单个 OR 列表，以便简化以后的推理。
	 *
	 * 为避免 eval_const_expressions 递归造成栈溢出，这里
	 * 只好写得紧一些：我们保留一份尚未处理的
	 * 输入列表，并通过往待办
	 * 列表前面追加来展平嵌套 OR，而不是递归。既然解析器已从简单列表生成 N 参数
	 * OR，这种复杂性大概不像
	 * 从前那么必要，但我们不妨保留这段逻辑。
	 */
	unprocessed_args = list_copy(args);
	while (unprocessed_args)
	{
		Node	   *arg = (Node *) linitial(unprocessed_args);

		unprocessed_args = list_delete_first(unprocessed_args);

		/* 按上面的注释展平嵌套 OR */
		if (is_orclause(arg))
		{
			List	   *subargs = ((BoolExpr *) arg)->args;
			List	   *oldlist = unprocessed_args;

			unprocessed_args = list_concat_copy(subargs, unprocessed_args);
			/* 为避免泄漏旧列表而写得也许过紧的代码 */
			list_free(oldlist);
			continue;
		}

		/* 若它不是 OR，就简化它 */
		arg = eval_const_expressions_mutator(arg, context);

		/*
		 * 非 OR 子句的简化不太可能、但也不是不可能产生 OR。
		 * 再检查一次，但不必对此过于紧张，
		 * 因为这不是主流情形。尤其是我们不担心
		 * 把输入常量简化两次，也不担心列表泄漏。
		 */
		if (is_orclause(arg))
		{
			List	   *subargs = ((BoolExpr *) arg)->args;

			unprocessed_args = list_concat_copy(subargs, unprocessed_args);
			continue;
		}

		/*
		 * 好，我们有了一个常量简化后的非 OR 参数。按
		 * 上面的注释处理它。
		 */
		if (IsA(arg, Const))
		{
			Const	   *const_input = (Const *) arg;

			if (const_input->constisnull)
				*haveNull = true;
			else if (DatumGetBool(const_input->constvalue))
			{
				*forceTrue = true;

				/*
				 * 一旦发现 TRUE 结果就可以立即
				 * 退出循环。不过，若将来增加不可移除函数的概念，
				 * 就需要继续扫描。
				 */
				return NIL;
			}
			/* 否则可以丢掉常量假输入 */
			continue;
		}

		/* 否则把简化后的参数发到结果列表里 */
		newargs = lappend(newargs, arg);
	}

	return newargs;
}

/*
 * eval_const_expressions 的子程序：处理 AND 子句的参数。
 *
 * 这包括展平嵌套的 ANDs，以及递归调用
 * eval_const_expressions 来简化 AND 参数。
 *
 * 简化之后，AND 参数按如下方式处理：
 *		非常量：保留
 *		TRUE：丢掉（不影响结果）
 *		FALSE：把结果强制为 FALSE
 *		NULL：只保留一个
 * 必须保留一个 NULL 输入，因为没有任何
 * 输入为 FALSE 且至少有一个为 NULL 时，AND 表达式求值为 NULL。这里其实不把 NULL
 * 放进来，那应当由调用方来做。
 *
 * 输出参数 *haveNull 和 *forceFalse 必须由调用方
 * 初始化为假。若在参数列表的任何地方发现空常量或假常量，
 * 它们会分别被设为真。
 */
static List *
simplify_and_arguments(List *args,
					   eval_const_expressions_context *context,
					   bool *haveNull, bool *forceFalse)
{
	List	   *newargs = NIL;
	List	   *unprocessed_args;

	/* 见 simplify_or_arguments 里的注释 */
	unprocessed_args = list_copy(args);
	while (unprocessed_args)
	{
		Node	   *arg = (Node *) linitial(unprocessed_args);

		unprocessed_args = list_delete_first(unprocessed_args);

		/* 按上面的注释展平嵌套 ANDs */
		if (is_andclause(arg))
		{
			List	   *subargs = ((BoolExpr *) arg)->args;
			List	   *oldlist = unprocessed_args;

			unprocessed_args = list_concat_copy(subargs, unprocessed_args);
			/* 为避免泄漏旧列表而写得也许过紧的代码 */
			list_free(oldlist);
			continue;
		}

		/* 若它不是 AND，就简化它 */
		arg = eval_const_expressions_mutator(arg, context);

		/*
		 * 非 AND 子句的简化不太可能、但也不是不可能产生 AND。
		 * 再检查一次，但不必对此过于紧张，
		 * 因为这不是主流情形。尤其是我们不担心
		 * 把输入常量简化两次，也不担心列表泄漏。
		 */
		if (is_andclause(arg))
		{
			List	   *subargs = ((BoolExpr *) arg)->args;

			unprocessed_args = list_concat_copy(subargs, unprocessed_args);
			continue;
		}

		/*
		 * 好，我们有了一个常量简化后的非 AND 参数。按
		 * 上面的注释处理它。
		 */
		if (IsA(arg, Const))
		{
			Const	   *const_input = (Const *) arg;

			if (const_input->constisnull)
				*haveNull = true;
			else if (!DatumGetBool(const_input->constvalue))
			{
				*forceFalse = true;

				/*
				 * 一旦发现 FALSE 结果就可以立即
				 * 退出循环。不过，若将来增加不可移除函数的概念，
				 * 就需要继续扫描。
				 */
				return NIL;
			}
			/* 否则可以丢掉常量真输入 */
			continue;
		}

		/* 否则把简化后的参数发到结果列表里 */
		newargs = lappend(newargs, arg);
	}

	return newargs;
}

/*
 * eval_const_expressions 的子程序：尝试简化布尔相等
 * 或不等条件。
 *
 * 输入是操作符 OID 以及该操作符已简化的参数。
 * 成功时返回简化后的表达式，若无法
 * 简化该表达式则返回 NULL。
 *
 * 这里的想法是把 x = true 归约成 x，把 x = false 归约成 NOT x，
 * 或者类似地把 x <> true 归约成 NOT x，把 x <> false 归约成 x。
 * 这本身只有一点点用，但在常量折叠里这样做，
 * 就能保证我们认出这些形式是等价的，例如
 * 在部分索引匹配时。
 *
 * 只有 simplify_function 失败时才会到这里；因此无法
 * 看到两个常量输入，也不会看到常量 NULL 输入。
 */
static Node *
simplify_boolean_equality(Oid opno, List *args)
{
	Node	   *leftop;
	Node	   *rightop;

	Assert(list_length(args) == 2);
	leftop = linitial(args);
	rightop = lsecond(args);
	if (leftop && IsA(leftop, Const))
	{
		Assert(!((Const *) leftop)->constisnull);
		if (opno == BooleanEqualOperator)
		{
			if (DatumGetBool(((Const *) leftop)->constvalue))
				return rightop; /* true = foo（左端为真） */
			else
				return negate_clause(rightop);	/* false = foo（左端为假） */
		}
		else
		{
			if (DatumGetBool(((Const *) leftop)->constvalue))
				return negate_clause(rightop);	/* true <> foo（左端为真，不等） */
			else
				return rightop; /* false <> foo（左端为假，不等） */
		}
	}
	if (rightop && IsA(rightop, Const))
	{
		Assert(!((Const *) rightop)->constisnull);
		if (opno == BooleanEqualOperator)
		{
			if (DatumGetBool(((Const *) rightop)->constvalue))
				return leftop;	/* foo = true（右端为真） */
			else
				return negate_clause(leftop);	/* foo = false（右端为假） */
		}
		else
		{
			if (DatumGetBool(((Const *) rightop)->constvalue))
				return negate_clause(leftop);	/* foo <> true（右端为真，不等） */
			else
				return leftop;	/* foo <> false（右端为假，不等） */
		}
	}
	return NULL;
}

/*
 * eval_const_expressions 的子程序：尝试简化函数调用
 * （它原先可能是操作符；我们不在乎）
 *
 * 输入是函数 OID、实际结果类型 OID（多态函数
 * 需要它）、结果 typmod、结果排序规则、函数要使用的输入
 * 排序规则、原始参数列表（除非 process_args 为假，否则
 * 尚未做常量简化），以及一些标志；
 * 还有 eval_const_expressions 的上下文数据。
 *
 * 成功时返回简化后的表达式，若无法
 * 简化该函数调用则返回 NULL。
 *
 * 本函数还负责把命名表示法的参数
 * 列表转换成位置表示法，和/或补上所需的默认参数
 * 表达式；这有点难看，但避免额外读取
 * 函数的 pg_proc 元组。因此 args 列表是
 * 按引用传递的。即使函数调用本身
 * 无法简化，参数列表的转换和常量简化
 * 仍会进行。
 */
static Expr *
simplify_function(Oid funcid, Oid result_type, int32 result_typmod,
				  Oid result_collid, Oid input_collid, List **args_p,
				  bool funcvariadic, bool process_args, bool allow_non_const,
				  eval_const_expressions_context *context)
{
	List	   *args = *args_p;
	HeapTuple	func_tuple;
	Form_pg_proc func_form;
	Expr	   *newexpr;

	/*
	 * 我们有三种简化策略：执行函数以
	 * 给出常量结果，用变换函数生成
	 * 替代节点树，或内联展开函数
	 * 定义体（这只对简单 SQL 语言函数有效，但
	 * 那是常见情形）。每种情形都需要访问函数的
	 * pg_proc 元组，因此只取一次。
	 *
	 * 注意：allow_non_const 标志会抑制第二和第三
	 * 种策略；因此若 !allow_non_const，simplify_function 只能返回
	 * Const 或 NULL。不过参数列表重写仍会发生。
	 */
	func_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(func_tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	func_form = (Form_pg_proc) GETSTRUCT(func_tuple);

	/*
	 * 处理函数参数，除非调用方已经处理过。
	 *
	 * 这里必须处理命名参数或带默认值的参数，然后
	 * 对整个参数列表递归应用 eval_const_expressions。
	 */
	if (process_args)
	{
		args = expand_function_arguments(args, false, result_type, func_tuple);
		args = (List *) expression_tree_mutator((Node *) args,
												eval_const_expressions_mutator,
												(void *) context);
		/* 参数处理完毕，交还给调用方 */
		*args_p = args;
	}

	/* 现在尝试简化函数调用本身。 */

	newexpr = evaluate_function(funcid, result_type, result_typmod,
								result_collid, input_collid,
								args, funcvariadic,
								func_tuple, context);

	if (!newexpr && allow_non_const && OidIsValid(func_form->prosupport))
	{
		/*
		 * 构造一个 SupportRequestSimplify 节点传给支持
		 * 函数，指向一个含有
		 * 已简化参数列表的哑 FuncExpr 节点。我们用这种方式向支持函数提供统一
		 * 接口，不论目标
		 * 函数实际上是怎么被调用的。
		 */
		SupportRequestSimplify req;
		FuncExpr	fexpr;

		fexpr.xpr.type = T_FuncExpr;
		fexpr.funcid = funcid;
		fexpr.funcresulttype = result_type;
		fexpr.funcretset = func_form->proretset;
		fexpr.funcvariadic = funcvariadic;
		fexpr.funcformat = COERCE_EXPLICIT_CALL;
		fexpr.funccollid = result_collid;
		fexpr.inputcollid = input_collid;
		fexpr.args = args;
		fexpr.location = -1;

		req.type = T_SupportRequestSimplify;
		req.root = context->root;
		req.fcall = &fexpr;

		newexpr = (Expr *)
			DatumGetPointer(OidFunctionCall1(func_form->prosupport,
											 PointerGetDatum(&req)));

		/* 抓住可能的 API 误解 */
		Assert(newexpr != (Expr *) &fexpr);
	}

	if (!newexpr && allow_non_const)
		newexpr = inline_function(funcid, result_type, result_collid,
								  input_collid, args, funcvariadic,
								  func_tuple, context);

	ReleaseSysCache(func_tuple);

	return newexpr;
}

/*
 * expand_function_arguments: convert named-notation args to positional args
 * and/or insert default args, as needed
 *
 * Returns a possibly-transformed version of the args list.
 *
 * If include_out_arguments is true, then the args list and the result
 * include OUT arguments.
 *
 * The expected result type of the call must be given, for sanity-checking
 * purposes.  Also, we ask the caller to provide the function's actual
 * pg_proc tuple, not just its OID.
 *
 * If we need to change anything, the input argument list is copied, not
 * modified.
 *
 * Note: this gets applied to operator argument lists too, even though the
 * cases it handles should never occur there.  This should be OK since it
 * will fall through very quickly if there's nothing to do.
 */
List *
expand_function_arguments(List *args, bool include_out_arguments,
						  Oid result_type, HeapTuple func_tuple)
{
	Form_pg_proc funcform = (Form_pg_proc) GETSTRUCT(func_tuple);
	Oid		   *proargtypes = funcform->proargtypes.values;
	int			pronargs = funcform->pronargs;
	bool		has_named_args = false;
	ListCell   *lc;

	/*
	 * 若要去匹配 OUT 参数，就使用 proallargtypes
	 * 数组（它包含那些参数）；否则使用 proargtypes（它
	 * 不包含）。当然，若 proallargtypes 为空，就始终使用
	 * proargtypes。（读取 proallargtypes 贵得烦人，
	 * 考虑到这里也许无事可做，但幸运的是
	 * 常见情形是 include_out_arguments == false。）
	 */
	if (include_out_arguments)
	{
		Datum		proallargtypes;
		bool		isNull;

		proallargtypes = SysCacheGetAttr(PROCOID, func_tuple,
										 Anum_pg_proc_proallargtypes,
										 &isNull);
		if (!isNull)
		{
			ArrayType  *arr = DatumGetArrayTypeP(proallargtypes);

			pronargs = ARR_DIMS(arr)[0];
			if (ARR_NDIM(arr) != 1 ||
				pronargs < 0 ||
				ARR_HASNULL(arr) ||
				ARR_ELEMTYPE(arr) != OIDOID)
				elog(ERROR, "proallargtypes is not a 1-D Oid array or it contains nulls");
			Assert(pronargs >= funcform->pronargs);
			proargtypes = (Oid *) ARR_DATA_PTR(arr);
		}
	}

	/* 有没有命名参数？ */
	foreach(lc, args)
	{
		Node	   *arg = (Node *) lfirst(lc);

		if (IsA(arg, NamedArgExpr))
		{
			has_named_args = true;
			break;
		}
	}

	/* 若有，就必须施加 reorder_function_arguments */
	if (has_named_args)
	{
		args = reorder_function_arguments(args, pronargs, func_tuple);
		/* 重新检查参数类型，并在需要时加转换 */
		recheck_cast_function_args(args, result_type,
								   proargtypes, pronargs,
								   func_tuple);
	}
	else if (list_length(args) < pronargs)
	{
		/* 没有命名参数，但似乎缺了一些默认值 */
		args = add_function_defaults(args, pronargs, func_tuple);
		/* 重新检查参数类型，并在需要时加转换 */
		recheck_cast_function_args(args, result_type,
								   proargtypes, pronargs,
								   func_tuple);
	}

	return args;
}

/*
 * reorder_function_arguments：把命名表示法参数转换成位置参数。
 *
 * 本函数也会按需插入默认参数值，因为不那样
 * 就不可能形成真正有效的位置调用。
 */
static List *
reorder_function_arguments(List *args, int pronargs, HeapTuple func_tuple)
{
	Form_pg_proc funcform = (Form_pg_proc) GETSTRUCT(func_tuple);
	int			nargsprovided = list_length(args);
	Node	   *argarray[FUNC_MAX_ARGS];
	ListCell   *lc;
	int			i;

	Assert(nargsprovided <= pronargs);
	if (pronargs < 0 || pronargs > FUNC_MAX_ARGS)
		elog(ERROR, "too many function arguments");
	memset(argarray, 0, pronargs * sizeof(Node *));

	/* 把参数列表拆成按 argnumber 索引的数组 */
	i = 0;
	foreach(lc, args)
	{
		Node	   *arg = (Node *) lfirst(lc);

		if (!IsA(arg, NamedArgExpr))
		{
			/* 位置参数，假定出现在所有命名参数之前 */
			Assert(argarray[i] == NULL);
			argarray[i++] = arg;
		}
		else
		{
			NamedArgExpr *na = (NamedArgExpr *) arg;

			Assert(na->argnumber >= 0 && na->argnumber < pronargs);
			Assert(argarray[na->argnumber] == NULL);
			argarray[na->argnumber] = (Node *) na->arg;
		}
	}

	/*
	 * 若需要，取默认表达式，并插入数组里的适当
	 * 位置（它们不一定连续，也不一定全用上）
	 */
	if (nargsprovided < pronargs)
	{
		List	   *defaults = fetch_function_defaults(func_tuple);

		i = pronargs - funcform->pronargdefaults;
		foreach(lc, defaults)
		{
			if (argarray[i] == NULL)
				argarray[i] = (Node *) lfirst(lc);
			i++;
		}
	}

	/* 现在按正确顺序重建参数列表 */
	args = NIL;
	for (i = 0; i < pronargs; i++)
	{
		Assert(argarray[i] != NULL);
		args = lappend(args, argarray[i]);
	}

	return args;
}

/*
 * add_function_defaults：从默认值补上缺少的函数参数。
 *
 * 这只在参数列表一开始就是位置参数时使用，
 * 因此我们知道只需在末尾补上默认值。
 */
static List *
add_function_defaults(List *args, int pronargs, HeapTuple func_tuple)
{
	int			nargsprovided = list_length(args);
	List	   *defaults;
	int			ndelete;

	/* 从 pg_proc 元组取出全部默认表达式 */
	defaults = fetch_function_defaults(func_tuple);

	/* 从列表里删掉任何未使用的默认值 */
	ndelete = nargsprovided + list_length(defaults) - pronargs;
	if (ndelete < 0)
		elog(ERROR, "not enough default arguments");
	if (ndelete > 0)
		defaults = list_delete_first_n(defaults, ndelete);

	/* 然后形成合并后的参数列表，不修改输入列表 */
	return list_concat_copy(args, defaults);
}

/*
 * fetch_function_defaults：把函数的默认参数取成表达式列表
 */
static List *
fetch_function_defaults(HeapTuple func_tuple)
{
	List	   *defaults;
	Datum		proargdefaults;
	char	   *str;

	proargdefaults = SysCacheGetAttrNotNull(PROCOID, func_tuple,
											Anum_pg_proc_proargdefaults);
	str = TextDatumGetCString(proargdefaults);
	defaults = castNode(List, stringToNode(str));
	pfree(str);
	return defaults;
}

/*
 * recheck_cast_function_args：重新检查函数参数并按需做类型转换
 * 在补上默认值之后。
 *
 * 有些带默认值的参数可能是多态的；
 * 因此不能假定默认表达式已经有正确的
 * 数据类型。必须重新解析多态并做转换，
 * 就像解析器做过的那样。
 *
 * 若没有多态参数，这应当是空操作，
 * 但为了保险我们仍然做。
 *
 * 注意：若需要任何转换，参数列表会被就地修改；
 * 调用方应当已经复制了列表结构。
 */
static void
recheck_cast_function_args(List *args, Oid result_type,
						   Oid *proargtypes, int pronargs,
						   HeapTuple func_tuple)
{
	Form_pg_proc funcform = (Form_pg_proc) GETSTRUCT(func_tuple);
	int			nargs;
	Oid			actual_arg_types[FUNC_MAX_ARGS];
	Oid			declared_arg_types[FUNC_MAX_ARGS];
	Oid			rettype;
	ListCell   *lc;

	if (list_length(args) > FUNC_MAX_ARGS)
		elog(ERROR, "too many function arguments");
	nargs = 0;
	foreach(lc, args)
	{
		actual_arg_types[nargs++] = exprType((Node *) lfirst(lc));
	}
	Assert(nargs == pronargs);
	memcpy(declared_arg_types, proargtypes, pronargs * sizeof(Oid));
	rettype = enforce_generic_type_consistency(actual_arg_types,
											   declared_arg_types,
											   nargs,
											   funcform->prorettype,
											   false);
	/* 我们就检查一下是否得到了与解析器相同的答案…… */
	if (rettype != result_type)
		elog(ERROR, "function's resolved result type changed during planning");

	/* 对参数做任何必要的类型转换 */
	make_fn_arguments(NULL, args, actual_arg_types, declared_arg_types);
}

/*
 * evaluate_function：尝试预先求值函数调用。
 *
 * 若函数是严格的且有任何常量 NULL 输入，就可以这样做
 * （直接返回一个空常量）；或者函数不可变且全部
 * 输入都是常量（调用它并把结果作为 Const 节点返回）。在
 * 估计模式下我们也愿意预先求值稳定函数。
 *
 * 成功时返回简化后的表达式，若无法
 * 简化该函数则返回 NULL。
 */
static Expr *
evaluate_function(Oid funcid, Oid result_type, int32 result_typmod,
				  Oid result_collid, Oid input_collid, List *args,
				  bool funcvariadic,
				  HeapTuple func_tuple,
				  eval_const_expressions_context *context)
{
	Form_pg_proc funcform = (Form_pg_proc) GETSTRUCT(func_tuple);
	bool		has_nonconst_input = false;
	bool		has_null_input = false;
	ListCell   *arg;
	FuncExpr   *newexpr;

	/*
	 * 若它返回集合就没法简化。
	 */
	if (funcform->proretset)
		return NULL;

	/*
	 * 若它返回 RECORD 就没法简化。眼前的问题是它
	 * 会要用一个预期的 tupdesc，而这里没法提供。
	 *
	 * 若它有 OUT 参数，可以从那些参数造出一个预期的
	 * tupdesc，但也许还藏着别的陷阱。尤其是，
	 * 若函数返回 NULL，我们会造出一个
	 * 空常量，不再标明它是哪种具体记录
	 * 类型。目前看来最好不要归约这个函数调用。
	 */
	if (funcform->prorettype == RECORDOID)
		return NULL;

	/*
	 * 检查常量输入，尤其是常量 NULL 输入。
	 */
	foreach(arg, args)
	{
		if (IsA(lfirst(arg), Const))
			has_null_input |= ((Const *) lfirst(arg))->constisnull;
		else
			has_nonconst_input = true;
	}

	/*
	 * 若函数是严格的且有常量 NULL 输入，它根本
	 * 不会被调用，因此可以把调用换成 NULL 常量，即使
	 * 还有其他输入不是常量，即使该
	 * 函数在其他方面并非不可变。
	 */
	if (funcform->proisstrict && has_null_input)
		return (Expr *) makeNullConst(result_type, result_typmod,
									  result_collid);

	/*
	 * 否则，只有全部输入都是常量时才能简化。（对
	 * 非严格函数，常量 NULL 输入与
	 * 常量非 NULL 输入同样对待。）
	 */
	if (has_nonconst_input)
		return NULL;

	/*
	 * 通常只允许简化不可变函数。但为了
	 * 估计，我们认为简化仅仅稳定的函数也可以；
	 * 结果从规划时刻变到执行时刻的风险，
	 * 比起完全没法估计这个值，
	 * 是值得承担的。
	 */
	if (funcform->provolatile == PROVOLATILE_IMMUTABLE)
		 /* 可以 */ ;
	else if (context->estimate && funcform->provolatile == PROVOLATILE_STABLE)
		 /* 可以 */ ;
	else
		return NULL;

	/*
	 * 好，看起来可以简化这个操作符或函数。
	 *
	 * 构造一个含有已简化参数的新 FuncExpr 节点。
	 */
	newexpr = makeNode(FuncExpr);
	newexpr->funcid = funcid;
	newexpr->funcresulttype = result_type;
	newexpr->funcretset = false;
	newexpr->funcvariadic = funcvariadic;
	newexpr->funcformat = COERCE_EXPLICIT_CALL; /* 无所谓 */
	newexpr->funccollid = result_collid;	/* 无所谓 */
	newexpr->inputcollid = input_collid;
	newexpr->args = args;
	newexpr->location = -1;

	return evaluate_expr((Expr *) newexpr, result_type, result_typmod,
						 result_collid);
}

/*
 * inline_function：尝试把函数调用内联展开。
 *
 * 若函数是足够简单的 SQL 语言函数
 * （只是 SELECT expression），就可以内联它，避开 SQL 函数
 * 相当高的每次调用开销。此外，这还能暴露
 * 在函数表达式内部做常量折叠的机会。
 *
 * 不过必须小心一些特殊情形。直接或
 * 间接递归的函数会让我们永远递归下去，
 * 因此我们记录正在展开哪些函数，并且
 * 不再重复展开它们。另外，若某个参数在
 * SQL 函数体里使用超过一次，我们需要它不含任何易变
 * 函数（易变函数可能给出不一致的答案），也不得
 * 贵得不合理。开销检查不仅
 * 防止运行时对昂贵参数做多次求值，也是一个安全阀，
 * 用来限制表达式因反复内联
 * 而膨胀。
 *
 * 还必须小心，不要因为内联而改变
 * 函数的易变性或严格性。
 *
 * 另外，目前没法内联返回 RECORD 的函数。这在
 * 一般情形下行不通，因为它丢掉了
 * OUT 参数声明之类的信息。
 *
 * 另外，参数列表里依赖上下文的表达式节点也是麻烦。
 *
 * 成功时返回简化后的表达式，若无法
 * 简化该函数则返回 NULL。
 */
static Expr *
inline_function(Oid funcid, Oid result_type, Oid result_collid,
				Oid input_collid, List *args,
				bool funcvariadic,
				HeapTuple func_tuple,
				eval_const_expressions_context *context)
{
	Form_pg_proc funcform = (Form_pg_proc) GETSTRUCT(func_tuple);
	char	   *src;
	Datum		tmp;
	bool		isNull;
	MemoryContext oldcxt;
	MemoryContext mycxt;
	inline_error_callback_arg callback_arg;
	ErrorContextCallback sqlerrcontext;
	FuncExpr   *fexpr;
	SQLFunctionParseInfoPtr pinfo;
	TupleDesc	rettupdesc;
	ParseState *pstate;
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	Query	   *querytree;
	Node	   *newexpr;
	int		   *usecounts;
	ListCell   *arg;
	int			i;

	/*
	 * 若函数不是 SQL 语言或有其他致命
	 * 属性，就放弃。（prokind 和 nargs 检查只是偏执。）
	 */
	if (funcform->prolang != SQLlanguageId ||
		funcform->prokind != PROKIND_FUNCTION ||
		funcform->prosecdef ||
		funcform->proretset ||
		funcform->prorettype == RECORDOID ||
		!heap_attisnull(func_tuple, Anum_pg_proc_proconfig, NULL) ||
		funcform->pronargs != list_length(args))
		return NULL;

	/* 检查递归函数，若是就放弃尝试展开 */
	if (list_member_oid(context->active_fns, funcid))
		return NULL;

	/* 检查调用函数的权限（若没有，以后再失败） */
	if (object_aclcheck(ProcedureRelationId, funcid, GetUserId(), ACL_EXECUTE) != ACLCHECK_OK)
		return NULL;

	/* 检查插件是否想钩住函数进入/退出 */
	if (FmgrHookIsNeeded(funcid))
		return NULL;

	/*
	 * 造一个临时内存上下文，以免泄漏解析
	 * 可能创建的全部东西。
	 */
	mycxt = AllocSetContextCreate(CurrentMemoryContext,
								  "inline_function",
								  ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(mycxt);

	/*
	 * 我们需要一个含有已简化
	 * 参数的哑 FuncExpr 节点。（有些情形其实并不需要它，但造它
	 * 足够便宜，不值得为了避开而扭来扭去。）
	 */
	fexpr = makeNode(FuncExpr);
	fexpr->funcid = funcid;
	fexpr->funcresulttype = result_type;
	fexpr->funcretset = false;
	fexpr->funcvariadic = funcvariadic;
	fexpr->funcformat = COERCE_EXPLICIT_CALL;	/* 无所谓 */
	fexpr->funccollid = result_collid;	/* 无所谓 */
	fexpr->inputcollid = input_collid;
	fexpr->args = args;
	fexpr->location = -1;

	/* 取函数体 */
	tmp = SysCacheGetAttrNotNull(PROCOID, func_tuple, Anum_pg_proc_prosrc);
	src = TextDatumGetCString(tmp);

	/*
	 * 为 ereport() 设置错误回溯支持。这样就能
	 * 指出坏信息来自哪个函数。
	 */
	callback_arg.proname = NameStr(funcform->proname);
	callback_arg.prosrc = src;

	sqlerrcontext.callback = sql_inline_error_callback;
	sqlerrcontext.arg = (void *) &callback_arg;
	sqlerrcontext.previous = error_context_stack;
	error_context_stack = &sqlerrcontext;

	/* 若有 prosqlbody，就注意它而不是 prosrc */
	tmp = SysCacheGetAttr(PROCOID,
						  func_tuple,
						  Anum_pg_proc_prosqlbody,
						  &isNull);
	if (!isNull)
	{
		Node	   *n;
		List	   *query_list;

		n = stringToNode(TextDatumGetCString(tmp));
		if (IsA(n, List))
			query_list = linitial_node(List, castNode(List, n));
		else
			query_list = list_make1(n);
		if (list_length(query_list) != 1)
			goto fail;
		querytree = linitial(query_list);

		/*
		 * 因为下面会坚持查询树的 rtable 为空
		 * 且没有子链接，它无法有任何需要
		 * 加锁或重写的关系引用。因此可以省掉那些步骤。
		 */
	}
	else
	{
		/* 准备在解析函数体时处理参数。 */
		pinfo = prepare_sql_fn_parse_info(func_tuple,
										  (Node *) fexpr,
										  input_collid);

		/*
		 * 我们只做解析和语法分析，不做重写，因为
		 * 重写不会影响无表且只有 SELECT 的查询，而那
		 * 正是我们关心的全部。另外，一旦发现
		 * 函数体里有多于一条命令就可以放弃。
		 */
		raw_parsetree_list = pg_parse_query(src);
		if (list_length(raw_parsetree_list) != 1)
			goto fail;

		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = src;
		sql_fn_parser_setup(pstate, pinfo);

		querytree = transformTopLevelStmt(pstate, linitial(raw_parsetree_list));

		free_parsestate(pstate);
	}

	/*
	 * 那唯一一条命令必须是简单的 SELECT expression。
	 *
	 * 注意：若改变这里涉及的测试，也请看 plpgsql 的
	 * exec_simple_check_plan()。那一般需要对什么是
	 * 简单表达式有同样的认识，这样内联一个
	 * 以前没被内联的函数就不会改变 plpgsql 的结论。
	 */
	if (!IsA(querytree, Query) ||
		querytree->commandType != CMD_SELECT ||
		querytree->hasAggs ||
		querytree->hasWindowFuncs ||
		querytree->hasTargetSRFs ||
		querytree->hasSubLinks ||
		querytree->cteList ||
		querytree->rtable ||
		querytree->jointree->fromlist ||
		querytree->jointree->quals ||
		querytree->groupClause ||
		querytree->groupingSets ||
		querytree->havingQual ||
		querytree->windowClause ||
		querytree->distinctClause ||
		querytree->sortClause ||
		querytree->limitOffset ||
		querytree->limitCount ||
		querytree->setOperations ||
		list_length(querytree->targetList) != 1)
		goto fail;

	/* 若函数结果是复合类型，就解析它 */
	(void) get_expr_result_type((Node *) fexpr,
								NULL,
								&rettupdesc);

	/*
	 * 确认函数（仍然）返回它所声明的东西。若不对
	 * 会报错，但这没关系，因为函数反正
	 * 会在运行时失败。注意 check_sql_fn_retval 还会插入
	 * 必要的转换，使 tlist 表达式匹配函数
	 * 声明的类型。
	 *
	 * 注意：在确认不需要重写之前我们不尝试这个；
	 * 这大概不重要，但还是小心为好。
	 */
	querytree_list = list_make1(querytree);
	if (check_sql_fn_retval_ext(list_make1(querytree_list),
								result_type, rettupdesc,
								funcform->prokind,
								false, NULL))
		goto fail;				/* 拒绝整行元组结果的情形 */

	/*
	 * 按上面的测试，check_sql_fn_retval 不应当决定
	 * 注入一个投影步骤，但我们还是确认一下。
	 */
	if (querytree != linitial(querytree_list))
		goto fail;

	/* 现在可以取 tlist 表达式 */
	newexpr = (Node *) ((TargetEntry *) linitial(querytree->targetList))->expr;

	/*
	 * 若 SQL 函数返回 VOID，只有当它是
	 * 对返回 VOID 的表达式做 SELECT（也就是只是重定向到
	 * 另一个返回 VOID 的函数）时才能内联。在所有非 VOID 返回情形下，
	 * check_sql_fn_retval 应当保证 newexpr 返回函数
	 * 声明的结果类型，因此这个测试不应当另有失败；但若失败了，
	 * 我们也不妨从容应对。
	 */
	if (exprType(newexpr) != result_type)
		goto fail;

	/*
	 * 对表达式再做一些有效性检查。它的易变性不得高于
	 * 外围函数（这是为了避免破坏那些权宜做法：
	 * 假装函数是不可变的，而它其实并非不可变）。
	 * 若外围函数声明为严格，则表达式
	 * 必须只含严格构造，并且必须使用该函数的全部
	 * 参数（这有点过头，但精确分析很难）。
	 */
	if (funcform->provolatile == PROVOLATILE_IMMUTABLE &&
		contain_mutable_functions(newexpr))
		goto fail;
	else if (funcform->provolatile == PROVOLATILE_STABLE &&
			 contain_volatile_functions(newexpr))
		goto fail;

	if (funcform->proisstrict &&
		contain_nonstrict_functions(newexpr))
		goto fail;

	/*
	 * 若任何参数表达式含有依赖上下文的节点，就不能
	 * 内联，以免把这种节点放进错误的上下文。
	 */
	if (contain_context_dependent_node((Node *) args))
		goto fail;

	/*
	 * 也许可以做；参数使用上还有检查
	 * 要做，但那些最容易和实际
	 * 替换输入一起做。因此开始构造已替换
	 * 输入的表达式。
	 */
	usecounts = (int *) palloc0(funcform->pronargs * sizeof(int));
	newexpr = substitute_actual_parameters(newexpr, funcform->pronargs,
										   args, usecounts);

	/* 现在检查参数使用 */
	i = 0;
	foreach(arg, args)
	{
		Node	   *param = lfirst(arg);

		if (usecounts[i] == 0)
		{
			/* Param 完全没用到：若函数是严格的，这不合适 */
			if (funcform->proisstrict)
				goto fail;
		}
		else if (usecounts[i] != 1)
		{
			/* Param 使用了多次：若昂贵或易变，这不合适 */
			QualCost	eval_cost;

			/*
			 * 我们把昂贵定义为含有任何子计划，或超过 10
			 * 个操作符。注意子计划搜索必须
			 * 显式做，因为 cost_qual_eval() 会对未规划的
			 * 子选择报错。
			 */
			if (contain_subplans(param))
				goto fail;
			cost_qual_eval(&eval_cost, list_make1(param), NULL);
			if (eval_cost.startup + eval_cost.per_tuple >
				10 * cpu_operator_cost)
				goto fail;

			/*
			 * 最后检查易变性，因为这比
			 * 上面的测试更贵
			 */
			if (contain_volatile_functions(param))
				goto fail;
		}
		i++;
	}

	/*
	 * 好，可以做替换了。把修改后的表达式
	 * 从临时内存上下文复制出来，并清理。
	 */
	MemoryContextSwitchTo(oldcxt);

	newexpr = copyObject(newexpr);

	MemoryContextDelete(mycxt);

	/*
	 * 若结果是可排序的类型，就强制结果暴露
	 * 正确的排序规则。大多数情形下这不要紧，但
	 * 函数结果有可能直接用作排序键，或用在
	 * 我们预期 exprCollation() 会说真话的其他地方。
	 */
	if (OidIsValid(result_collid))
	{
		Oid			exprcoll = exprCollation(newexpr);

		if (OidIsValid(exprcoll) && exprcoll != result_collid)
		{
			CollateExpr *newnode = makeNode(CollateExpr);

			newnode->arg = (Expr *) newexpr;
			newnode->collOid = result_collid;
			newnode->location = -1;

			newexpr = (Node *) newnode;
		}
	}

	/*
	 * 既然计划树里现在没有该函数的痕迹，必须
	 * 显式记录计划对该函数的依赖。
	 */
	if (context->root)
		record_plan_function_dependency(context->root, funcid);

	/*
	 * 递归尝试简化修改后的表达式。这里必须把
	 * 当前函数加进活动函数的上下文列表。
	 */
	context->active_fns = lappend_oid(context->active_fns, funcid);
	newexpr = eval_const_expressions_mutator(newexpr, context);
	context->active_fns = list_delete_last(context->active_fns);

	error_context_stack = sqlerrcontext.previous;

	return (Expr *) newexpr;

	/* 这里若函数不可内联：释放临时内存并返回 NULL */
fail:
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(mycxt);
	error_context_stack = sqlerrcontext.previous;

	return NULL;
}

/*
 * 用适当的实际参数替换 Param 节点
 */
static Node *
substitute_actual_parameters(Node *expr, int nargs, List *args,
							 int *usecounts)
{
	substitute_actual_parameters_context context;

	context.nargs = nargs;
	context.args = args;
	context.usecounts = usecounts;

	return substitute_actual_parameters_mutator(expr, &context);
}

static Node *
substitute_actual_parameters_mutator(Node *node,
									 substitute_actual_parameters_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind != PARAM_EXTERN)
			elog(ERROR, "unexpected paramkind: %d", (int) param->paramkind);
		if (param->paramid <= 0 || param->paramid > context->nargs)
			elog(ERROR, "invalid paramid: %d", param->paramid);

		/* 统计参数的使用次数 */
		context->usecounts[param->paramid - 1]++;

		/* 选择适当的实际参数并用它替换 Param */
		/* 此时不必复制（以后会做） */
		return list_nth(context->args, param->paramid - 1);
	}
	return expression_tree_mutator(node, substitute_actual_parameters_mutator,
								   (void *) context);
}

/*
 * 错误上下文回调，让我们提供调用栈回溯
 */
static void
sql_inline_error_callback(void *arg)
{
	inline_error_callback_arg *callback_arg = (inline_error_callback_arg *) arg;
	int			syntaxerrposition;

	/* 若是语法错误，就转成内部语法错误报告 */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0)
	{
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(callback_arg->prosrc);
	}

	errcontext("SQL function \"%s\" during inlining", callback_arg->proname);
}

/*
 * evaluate_expr：预先求值常量表达式。
 *
 * 我们用执行器例程 ExecEvalExpr()，以免重复
 * 代码，并保证得到与执行器相同的结果。
 */
Expr *
evaluate_expr(Expr *expr, Oid result_type, int32 result_typmod,
			  Oid result_collation)
{
	EState	   *estate;
	ExprState  *exprstate;
	MemoryContext oldcontext;
	Datum		const_val;
	bool		const_is_null;
	int16		resultTypLen;
	bool		resultTypByVal;

	/*
	 * 要用执行器，就需要一个 EState。
	 */
	estate = CreateExecutorState();

	/* 可以用 estate 的工作上下文来避免内存泄漏。 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* 确保任何 opfuncids 都已填好。 */
	fix_opfuncids((Node *) expr);

	/*
	 * 准备表达式以便执行。（注意：不能用 ExecPrepareExpr，
	 * 因为那会递归调用 eval_const_expressions。）
	 */
	exprstate = ExecInitExpr(expr, NULL);

	/*
	 * 然后求值它。
	 *
	 * 可以用默认 econtext，因为此情形下用到的 ExecEvalExpr()
	 * 代码都不会使用 econtext。这看起来像碰巧，
	 * 其实也合理：常量表达式按定义
	 * 不依赖上下文。
	 */
	const_val = ExecEvalExprSwitchContext(exprstate,
										  GetPerTupleExprContext(estate),
										  &const_is_null);

	/* 取得结果数据类型所需的信息 */
	get_typlenbyval(result_type, &resultTypLen, &resultTypByVal);

	/* 回到外层内存上下文 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * 必须把结果从表达式求值所用的子上下文里复制出来。
	 *
	 * 另外，若它是 varlena，就强制解 TOAST。这防止我们把
	 * TOAST 指针存进可能比所引用数据
	 * 活得更久的计划里。（makeConst 反正会处理解 TOAST，但这里
	 * 多写几行是值得的，这样复制和解 TOAST 可以一步完成。）
	 */
	if (!const_is_null)
	{
		if (resultTypLen == -1)
			const_val = PointerGetDatum(PG_DETOAST_DATUM_COPY(const_val));
		else
			const_val = datumCopy(const_val, resultTypByVal, resultTypLen);
	}

	/* 释放刚刚创建的全部杂物 */
	FreeExecutorState(estate);

	/*
	 * 构造常量结果节点。
	 */
	return (Expr *) makeConst(result_type, result_typmod, result_collation,
							  resultTypLen,
							  const_val, const_is_null,
							  resultTypByVal);
}


/*
 * inline_set_returning_function
 *		尝试内联 FROM 子句里的集合返回函数。
 *
 * rte 是一个 RTE_FUNCTION 范围表项。若它表示对一个
 * 可以安全内联的集合返回 SQL 函数的调用，就展开该函数
 * 并返回替代的 Query 结构。否则返回 NULL。
 *
 * 我们假定 RTE 的表达式已经过
 * eval_const_expressions()，这会处理
 * 默认参数和命名参数表示法。
 *
 * 这和 inline_function() 有不少相似之处，但那个
 * 是非集合返回的情形，差别已经够多，
 * 值得分成两个函数。
 */
Query *
inline_set_returning_function(PlannerInfo *root, RangeTblEntry *rte)
{
	RangeTblFunction *rtfunc;
	FuncExpr   *fexpr;
	Oid			func_oid;
	HeapTuple	func_tuple;
	Form_pg_proc funcform;
	char	   *src;
	Datum		tmp;
	bool		isNull;
	MemoryContext oldcxt;
	MemoryContext mycxt;
	inline_error_callback_arg callback_arg;
	ErrorContextCallback sqlerrcontext;
	SQLFunctionParseInfoPtr pinfo;
	TypeFuncClass functypclass;
	TupleDesc	rettupdesc;
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	Query	   *querytree;

	Assert(rte->rtekind == RTE_FUNCTION);

	/*
	 * SQL SRF 在自己的 FROM 子句里引用自己没什么意义，
	 * 因为那在运行时必然造成无限递归。
	 * 这也会让这段代码递归，因此检查栈溢出。
	 * （不必再做更多。）
	 */
	check_stack_depth();

	/* 若 RTE 有 ORDINALITY 就失败，这里不实现那个。 */
	if (rte->funcordinality)
		return NULL;

	/* 若 RTE 不是单个简单的 FuncExpr 就失败 */
	if (list_length(rte->functions) != 1)
		return NULL;
	rtfunc = (RangeTblFunction *) linitial(rte->functions);

	if (!IsA(rtfunc->funcexpr, FuncExpr))
		return NULL;
	fexpr = (FuncExpr *) rtfunc->funcexpr;

	func_oid = fexpr->funcid;

	/*
	 * 函数必须声明为返回集合，否则内联会
	 * 改变结果，如果所含 SELECT 不是恰好返回一
	 * 行。
	 */
	if (!fexpr->funcretset)
		return NULL;

	/*
	 * 若参数含有任何易变函数或
	 * 子选择，就拒绝内联。拒绝易变函数是因为内联可能
	 * 导致参数被求值多次，有改变
	 * 行为的风险。拒绝子选择，一部分是实现
	 * 原因（再往下推一层可能改变它们的行为），
	 * 一部分是因为它们往往很贵，因此多次
	 * 求值会很糟。
	 */
	if (contain_volatile_functions((Node *) fexpr->args) ||
		contain_subplans((Node *) fexpr->args))
		return NULL;

	/* 检查调用函数的权限（若没有，以后再失败） */
	if (object_aclcheck(ProcedureRelationId, func_oid, GetUserId(), ACL_EXECUTE) != ACLCHECK_OK)
		return NULL;

	/* 检查插件是否想钩住函数进入/退出 */
	if (FmgrHookIsNeeded(func_oid))
		return NULL;

	/*
	 * 好，来看一下函数的 pg_proc 项。
	 */
	func_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(func_oid));
	if (!HeapTupleIsValid(func_tuple))
		elog(ERROR, "cache lookup failed for function %u", func_oid);
	funcform = (Form_pg_proc) GETSTRUCT(func_tuple);

	/*
	 * 若函数不是 SQL 语言或有其他致命
	 * 属性，就放弃。尤其是它不得声明为 STRICT，因为我们
	 * 没法强制这一点。它也不得是 VOLATILE，因为那意味着
	 * 它应当用自己的快照执行，而不是
	 * 共享调用查询的快照。我们也不允许返回
	 * SETOF VOID，因为内联会暴露该函数最后一条 SELECT 的实际结果，
	 * 而那种情况下不应当发生。（重新检查 prokind、proretset 和 pronargs
	 * 只是偏执。）
	 */
	if (funcform->prolang != SQLlanguageId ||
		funcform->prokind != PROKIND_FUNCTION ||
		funcform->proisstrict ||
		funcform->provolatile == PROVOLATILE_VOLATILE ||
		funcform->prorettype == VOIDOID ||
		funcform->prosecdef ||
		!funcform->proretset ||
		list_length(fexpr->args) != funcform->pronargs ||
		!heap_attisnull(func_tuple, Anum_pg_proc_proconfig, NULL))
	{
		ReleaseSysCache(func_tuple);
		return NULL;
	}

	/*
	 * 造一个临时内存上下文，以免泄漏解析
	 * 可能创建的全部东西。
	 */
	mycxt = AllocSetContextCreate(CurrentMemoryContext,
								  "inline_set_returning_function",
								  ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(mycxt);

	/* 取函数体 */
	tmp = SysCacheGetAttrNotNull(PROCOID, func_tuple, Anum_pg_proc_prosrc);
	src = TextDatumGetCString(tmp);

	/*
	 * 为 ereport() 设置错误回溯支持。这样就能
	 * 指出坏信息来自哪个函数。
	 */
	callback_arg.proname = NameStr(funcform->proname);
	callback_arg.prosrc = src;

	sqlerrcontext.callback = sql_inline_error_callback;
	sqlerrcontext.arg = (void *) &callback_arg;
	sqlerrcontext.previous = error_context_stack;
	error_context_stack = &sqlerrcontext;

	/* 若有 prosqlbody，就注意它而不是 prosrc */
	tmp = SysCacheGetAttr(PROCOID,
						  func_tuple,
						  Anum_pg_proc_prosqlbody,
						  &isNull);
	if (!isNull)
	{
		Node	   *n;

		n = stringToNode(TextDatumGetCString(tmp));
		if (IsA(n, List))
			querytree_list = linitial_node(List, castNode(List, n));
		else
			querytree_list = list_make1(n);
		if (list_length(querytree_list) != 1)
			goto fail;
		querytree = linitial(querytree_list);

		/* 取得必要的锁，然后施加重写器。 */
		AcquireRewriteLocks(querytree, true, false);
		querytree_list = pg_rewrite_query(querytree);
		if (list_length(querytree_list) != 1)
			goto fail;
		querytree = linitial(querytree_list);
	}
	else
	{
		/*
		 * 准备在解析函数体时处理参数。可以
		 * 把刚创建的 FuncExpr 用作
		 * prepare_sql_fn_parse_info 的输入。
		 */
		pinfo = prepare_sql_fn_parse_info(func_tuple,
										  (Node *) fexpr,
										  fexpr->inputcollid);

		/*
		 * 解析、分析并重写（与 inline_function() 不同，这里不能
		 * 跳过重写）。不过一旦发现多于一条
		 * 查询就可以失败返回。
		 */
		raw_parsetree_list = pg_parse_query(src);
		if (list_length(raw_parsetree_list) != 1)
			goto fail;

		querytree_list = pg_analyze_and_rewrite_withcb(linitial(raw_parsetree_list),
													   src,
													   (ParserSetupHook) sql_fn_parser_setup,
													   pinfo, NULL);
		if (list_length(querytree_list) != 1)
			goto fail;
		querytree = linitial(querytree_list);
	}

	/*
	 * 若是复合类型，还要解析实际的函数结果 tupdesc。若
	 * 有 coldeflist，就相信它；否则用 get_expr_result_type。
	 * （这段逻辑应当与 ExecInitFunctionScan 一致。）
	 */
	if (rtfunc->funccolnames != NIL)
	{
		functypclass = TYPEFUNC_RECORD;
		rettupdesc = BuildDescFromLists(rtfunc->funccolnames,
										rtfunc->funccoltypes,
										rtfunc->funccoltypmods,
										rtfunc->funccolcollations);
	}
	else
		functypclass = get_expr_result_type((Node *) fexpr, NULL, &rettupdesc);

	/*
	 * 那唯一一条命令必须是普通的 SELECT。
	 */
	if (!IsA(querytree, Query) ||
		querytree->commandType != CMD_SELECT)
		goto fail;

	/*
	 * 确认函数（仍然）返回它所声明的东西。若不对
	 * 会报错，但这没关系，因为函数反正
	 * 会在运行时失败。注意 check_sql_fn_retval 还会插入
	 * 必要的转换，使 tlist 表达式匹配函数
	 * 声明的类型。我们还让它为 rettupdesc 里任何
	 * 已删除的列插入哑 NULL 列，使修改后 tlist 的
	 * 各元素与属性号对得上。
	 *
	 * 若函数返回复合类型，除非检查表明它返回的是
	 * 整行元组结果，否则不要内联；不然它返回的是
	 * 单个复合列，并不是我们要的。
	 */
	if (!check_sql_fn_retval_ext(list_make1(querytree_list),
								 fexpr->funcresulttype, rettupdesc,
								 funcform->prokind,
								 true, NULL) &&
		(functypclass == TYPEFUNC_COMPOSITE ||
		 functypclass == TYPEFUNC_COMPOSITE_DOMAIN ||
		 functypclass == TYPEFUNC_RECORD))
		goto fail;				/* 拒绝非整行元组结果的情形 */

	/*
	 * check_sql_fn_retval 也许插入了投影步骤，但那
	 * 没关系；只要确保使用上层 Query。
	 */
	querytree = linitial_node(Query, querytree_list);

	/*
	 * 看起来不错，把参数替换进查询。
	 */
	querytree = substitute_actual_srf_parameters(querytree,
												 funcform->pronargs,
												 fexpr->args);

	/*
	 * 把修改后的查询从临时内存上下文复制出来，并
	 * 清理。
	 */
	MemoryContextSwitchTo(oldcxt);

	querytree = copyObject(querytree);

	MemoryContextDelete(mycxt);
	error_context_stack = sqlerrcontext.previous;
	ReleaseSysCache(func_tuple);

	/*
	 * 这里不必修正排序规则，因为上层查询已经
	 * 解析过了，也就是说算数的是 RTE 里的排序规则。
	 */

	/*
	 * 既然计划树里现在没有该函数的痕迹，必须
	 * 显式记录计划对该函数的依赖。
	 */
	record_plan_function_dependency(root, func_oid);

	/*
	 * 还必须注意插入的查询是否因 RLS 限定条件
	 * 而对调用角色增加了依赖。
	 */
	if (querytree->hasRowSecurity)
		root->glob->dependsOnRole = true;

	return querytree;

	/* 这里若函数不可内联：释放临时内存并返回 NULL */
fail:
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(mycxt);
	error_context_stack = sqlerrcontext.previous;
	ReleaseSysCache(func_tuple);

	return NULL;
}

/*
 * 用适当的实际参数替换 Param 节点。
 *
 * 这和 substitute_actual_parameters() 的差别刚好够大，
 * 因此需要自己的代码。
 */
static Query *
substitute_actual_srf_parameters(Query *expr, int nargs, List *args)
{
	substitute_actual_srf_parameters_context context;

	context.nargs = nargs;
	context.args = args;
	context.sublevels_up = 1;

	return query_tree_mutator(expr,
							  substitute_actual_srf_parameters_mutator,
							  &context,
							  0);
}

static Node *
substitute_actual_srf_parameters_mutator(Node *node,
										 substitute_actual_srf_parameters_context *context)
{
	Node	   *result;

	if (node == NULL)
		return NULL;
	if (IsA(node, Query))
	{
		context->sublevels_up++;
		result = (Node *) query_tree_mutator((Query *) node,
											 substitute_actual_srf_parameters_mutator,
											 (void *) context,
											 0);
		context->sublevels_up--;
		return result;
	}
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXTERN)
		{
			if (param->paramid <= 0 || param->paramid > context->nargs)
				elog(ERROR, "invalid paramid: %d", param->paramid);

			/*
			 * 因为参数正被插入子查询，必须
			 * 调整层次。
			 */
			result = copyObject(list_nth(context->args, param->paramid - 1));
			IncrementVarSublevelsUp(result, context->sublevels_up, 0);
			return result;
		}
	}
	return expression_tree_mutator(node,
								   substitute_actual_srf_parameters_mutator,
								   (void *) context);
}

/*
 * pull_paramids
 *		返回包含 expr 里全部 Params 的 paramids 的 Bitmapset。
 */
Bitmapset *
pull_paramids(Expr *expr)
{
	Bitmapset  *result = NULL;

	(void) pull_paramids_walker((Node *) expr, &result);

	return result;
}

static bool
pull_paramids_walker(Node *node, Bitmapset **context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		*context = bms_add_member(*context, param->paramid);
		return false;
	}
	return expression_tree_walker(node, pull_paramids_walker,
								  (void *) context);
}
