/*-------------------------------------------------------------------------
 *
 * subselect.c
 *	  Planning routines for subselects.
 *
 * This module deals with SubLinks and CTEs, but not subquery RTEs (i.e.,
 * not sub-SELECT-in-FROM cases).
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/subselect.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paramassign.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


typedef struct convert_testexpr_context
{
	PlannerInfo *root;
	List	   *subst_nodes;	/* 用来替换 Param 的节点 */
} convert_testexpr_context;

typedef struct process_sublinks_context
{
	PlannerInfo *root;
	bool		isTopQual;
} process_sublinks_context;

typedef struct finalize_primnode_context
{
	PlannerInfo *root;
	Bitmapset  *paramids;		/* 找到的非局部 PARAM_EXEC 参数号 */
} finalize_primnode_context;

typedef struct inline_cte_walker_context
{
	const char *ctename;		/* 目标 CTE 的名字和相对层次 */
	int			levelsup;
	Query	   *ctequery;		/* 用来替换的查询 */
} inline_cte_walker_context;


static Node *build_subplan(PlannerInfo *root, Plan *plan, PlannerInfo *subroot,
						   List *plan_params,
						   SubLinkType subLinkType, int subLinkId,
						   Node *testexpr, List *testexpr_paramids,
						   bool unknownEqFalse);
static List *generate_subquery_params(PlannerInfo *root, List *tlist,
									  List **paramIds);
static List *generate_subquery_vars(PlannerInfo *root, List *tlist,
									Index varno);
static Node *convert_testexpr(PlannerInfo *root,
							  Node *testexpr,
							  List *subst_nodes);
static Node *convert_testexpr_mutator(Node *node,
									  convert_testexpr_context *context);
static bool subplan_is_hashable(Plan *plan);
static bool subpath_is_hashable(Path *path);
static bool testexpr_is_hashable(Node *testexpr, List *param_ids);
static bool test_opexpr_is_hashable(OpExpr *testexpr, List *param_ids);
static bool hash_ok_operator(OpExpr *expr);
static bool contain_dml(Node *node);
static bool contain_dml_walker(Node *node, void *context);
static bool contain_outer_selfref(Node *node);
static bool contain_outer_selfref_walker(Node *node, Index *depth);
static void inline_cte(PlannerInfo *root, CommonTableExpr *cte);
static bool inline_cte_walker(Node *node, inline_cte_walker_context *context);
static bool simplify_EXISTS_query(PlannerInfo *root, Query *query);
static Query *convert_EXISTS_to_ANY(PlannerInfo *root, Query *subselect,
									Node **testexpr, List **paramIds);
static Node *replace_correlation_vars_mutator(Node *node, PlannerInfo *root);
static Node *process_sublinks_mutator(Node *node,
									  process_sublinks_context *context);
static Bitmapset *finalize_plan(PlannerInfo *root,
								Plan *plan,
								int gather_param,
								Bitmapset *valid_params,
								Bitmapset *scan_params);
static bool finalize_primnode(Node *node, finalize_primnode_context *context);
static bool finalize_agg_primnode(Node *node, finalize_primnode_context *context);


/*
 * 取得计划输出第一列的数据类型/typmod/排序规则。
 *
 * 这份信息为 ARRAY_SUBLINK 执行以及
 * exprType()/exprTypmod()/exprCollation() 保存，它们没法拿到
 * 与 SubPlan 节点关联的计划。其实只对
 * EXPR_SUBLINK 和 ARRAY_SUBLINK 子计划需要这份信息，但为了一致，总是
 * 保存。
 */
static void
get_first_col_type(Plan *plan, Oid *coltype, int32 *coltypmod,
				   Oid *colcollation)
{
	/* 在 EXISTS 这类情形里目标列表可能为空；任意使用 VOID */
	if (plan->targetlist)
	{
		TargetEntry *tent = linitial_node(TargetEntry, plan->targetlist);

		if (!tent->resjunk)
		{
			*coltype = exprType((Node *) tent->expr);
			*coltypmod = exprTypmod((Node *) tent->expr);
			*colcollation = exprCollation((Node *) tent->expr);
			return;
		}
	}
	*coltype = VOIDOID;
	*coltypmod = -1;
	*colcollation = InvalidOid;
}

/*
 * 把 SubLink（由解析器创建）转换成 SubPlan。
 *
 * 给定 SubLink 所含的查询、类型、ID 和 testexpr。还
 * 会被告知该表达式是否出现在 WHERE/HAVING 限定条件的顶层。
 *
 * 注意：假定 testexpr 已经做过 AND/OR 展平（实际上
 * 已经过 eval_const_expressions），但还没转换成
 * 隐式 AND 形式；其中任何 SubLinks 应当已经
 * 转换成 SubPlans。不过子查询本身还没动过。
 *
 * 结果是要在可执行表达式里替换该 SubLink 节点的东西。
 * 若把子计划做成
 * 普通子计划，这就是构造出的 SubPlan 节点。若做成
 * InitPlan，则 SubPlan 节点改放到
 * root->init_plans，这里返回的是表示
 * InitPlan 结果的表达式树：通常只是表示
 * 单个标量结果的 Param 节点，但也可能是含有
 * 多个 Param 节点的行比较树；对 MULTIEXPR 子查询则是一个简单的 NULL 常量
 * （因为真正的输出 Param 在树的别处，而 MULTIEXPR
 * 子查询本身在一个值无足轻重的 resjunk 目标列表项里）。
 */
static Node *
make_subplan(PlannerInfo *root, Query *orig_subquery,
			 SubLinkType subLinkType, int subLinkId,
			 Node *testexpr, bool isTopQual)
{
	Query	   *subquery;
	bool		simple_exists = false;
	double		tuple_fraction;
	PlannerInfo *subroot;
	RelOptInfo *final_rel;
	Path	   *best_path;
	Plan	   *plan;
	List	   *plan_params;
	Node	   *result;

	/*
	 * 复制源 Query 节点。这是个又快又糙的权宜之计，用来解决
	 * 解析器可能生成多处链接到
	 * 同一个子 Query 节点的树，但规划器想在 Query 上涂改。
	 * 等做查询树重新设计时再设法收拾……
	 */
	subquery = copyObject(orig_subquery);

	/*
	 * 若是 EXISTS 子计划，也许能简化它。
	 */
	if (subLinkType == EXISTS_SUBLINK)
		simple_exists = simplify_EXISTS_query(root, subquery);

	/*
	 * 对 EXISTS 子计划，告诉下层规划器预期只会
	 * 取出第一个元组。对 ALL 和 ANY 子计划，若测试条件失败或匹配，
	 * 就能停止求值，因此常常
	 * 不会取出全部元组；没有更好的主意，就
	 * 指定取出 50%。对 EXPR、MULTIEXPR 和 ROWCOMPARE 子计划，
	 * 用默认行为（反正只预期输出一行）。
	 *
	 * 注意：若改这些数字，也要改 path/costsize.c 里的
	 * cost_subplan()。
	 *
	 * XXX 若 ANY 子计划不相关，build_subplan 也许会决定哈希
	 * 其输出。那时最好指定全部
	 * 取出。不过目前只能在
	 * 做好子计划之后才能检查可否哈希 :-(。（判断它是否放得进 hash_mem
	 * 才是真正难的部分。）因此，对取出元组的比例不要太
	 * 乐观，以免
	 * 选出对物化情形很差的计划。
	 */
	if (subLinkType == EXISTS_SUBLINK)
		tuple_fraction = 1.0;	/* 就像 LIMIT 1 */
	else if (subLinkType == ALL_SUBLINK ||
			 subLinkType == ANY_SUBLINK)
		tuple_fraction = 0.5;	/* 50%（一半） */
	else
		tuple_fraction = 0.0;	/* 默认行为 */

	/* 当前查询层次不应当在使用 plan_params */
	Assert(root->plan_params == NIL);

	/* 为子查询生成路径 */
	subroot = subquery_planner(root->glob, subquery,
							   root,
							   false, tuple_fraction);

	/* 分离出这个特定子计划需要的参数 */
	plan_params = root->plan_params;
	root->plan_params = NIL;

	/*
	 * 选出最佳路径并把它变成计划。至少目前，
	 * 似乎没有理由推迟这件事。
	 */
	final_rel = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
	best_path = get_cheapest_fractional_path(final_rel, tuple_fraction);

	plan = create_plan(subroot, best_path);

	/* 并转换成 SubPlan 或 InitPlan 形式。 */
	result = build_subplan(root, plan, subroot, plan_params,
						   subLinkType, subLinkId,
						   testexpr, NIL, isTopQual);

	/*
	 * 若是带有无关紧要目标列表的相关 EXISTS，也许能
	 * 把它变成等价的 IN，再用
	 * 哈希实现。现在还没有足够信息判断哪种
	 * 可能更好（取决于该 EXISTS 限定条件的预期执行次数，
	 * 而对外层查询的规划还太早，
	 * 猜不了这个）。因此尽可能生成两种计划，
	 * 留给 setrefs.c 决定用哪一种。
	 */
	if (simple_exists && IsA(result, SubPlan))
	{
		Node	   *newtestexpr;
		List	   *paramIds;

		/* 再复制一份原来的子查询 */
		subquery = copyObject(orig_subquery);
		/* 并重新简化 */
		simple_exists = simplify_EXISTS_query(root, subquery);
		Assert(simple_exists);
		/* 看能否转换成 ANY 查询 */
		subquery = convert_EXISTS_to_ANY(root, subquery,
										 &newtestexpr, &paramIds);
		if (subquery)
		{
			/* 为 ANY 子查询生成路径；需要全部行 */
			subroot = subquery_planner(root->glob, subquery,
									   root,
									   false, 0.0);

			/* 分离出这个特定子计划需要的参数 */
			plan_params = root->plan_params;
			root->plan_params = NIL;

			/* 选出最佳路径 */
			final_rel = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
			best_path = final_rel->cheapest_total_path;

			/* 现在可以检查它是否放得进 hash_mem */
			if (subpath_is_hashable(best_path))
			{
				SubPlan    *hashplan;
				AlternativeSubPlan *asplan;

				/* 好，完成 ANY 子查询的规划 */
				plan = create_plan(subroot, best_path);

				/* ……并转换成 SubPlan 形式 */
				hashplan = castNode(SubPlan,
									build_subplan(root, plan, subroot,
												  plan_params,
												  ANY_SUBLINK, 0,
												  newtestexpr,
												  paramIds,
												  true));
				/* 检查得到的是否为预期 */
				Assert(hashplan->parParam == NIL);
				Assert(hashplan->useHashTable);

				/* 留给 setrefs.c 决定用哪份计划 */
				asplan = makeNode(AlternativeSubPlan);
				asplan->subplans = list_make2(result, hashplan);
				result = (Node *) asplan;
				root->hasAlternativeSubPlans = true;
			}
		}
	}

	return result;
}

/*
 * 根据原始输入建立 SubPlan 节点 --- make_subplan 的子程序
 *
 * 返回该 SubPlan，或者若决定
 * 把它做成 InitPlan，就返回替换表达式，如 make_subplan 的注释所述。
 */
static Node *
build_subplan(PlannerInfo *root, Plan *plan, PlannerInfo *subroot,
			  List *plan_params,
			  SubLinkType subLinkType, int subLinkId,
			  Node *testexpr, List *testexpr_paramids,
			  bool unknownEqFalse)
{
	Node	   *result;
	SubPlan    *splan;
	bool		isInitPlan;
	ListCell   *lc;

	/*
	 * 初始化 SubPlan 节点。注意 plan_id、plan_name 和代价字段
	 * 在更下面设置。
	 */
	splan = makeNode(SubPlan);
	splan->subLinkType = subLinkType;
	splan->testexpr = NULL;
	splan->paramIds = NIL;
	get_first_col_type(plan, &splan->firstColType, &splan->firstColTypmod,
					   &splan->firstColCollation);
	splan->useHashTable = false;
	splan->unknownEqFalse = unknownEqFalse;
	splan->parallel_safe = plan->parallel_safe;
	splan->setParam = NIL;
	splan->parParam = NIL;
	splan->args = NIL;

	/*
	 * 建立 parParam 和 args 列表，即当前
	 * 查询层次要传给这个子计划的参数号和表达式。
	 */
	foreach(lc, plan_params)
	{
		PlannerParamItem *pitem = (PlannerParamItem *) lfirst(lc);
		Node	   *arg = pitem->item;

		/*
		 * Var、PlaceHolderVar、Aggref 或 GroupingFunc 已经
		 * 调整到正确的 varlevelsup、phlevelsup 或
		 * agglevelsup。
		 *
		 * 若是 PlaceHolderVar、Aggref 或 GroupingFunc，其参数
		 * 可能含有尚未处理的 SubLinks（见
		 * SS_replace_correlation_vars 的注释）。现在处理。
		 */
		if (IsA(arg, PlaceHolderVar) ||
			IsA(arg, Aggref) ||
			IsA(arg, GroupingFunc))
			arg = SS_process_sublinks(root, arg, false);

		splan->parParam = lappend_int(splan->parParam, pitem->paramId);
		splan->args = lappend(splan->args, arg);
	}

	/*
	 * EXISTS、EXPR、ARRAY、
	 * ROWCOMPARE 或 MULTIEXPR 类型的不相关或非直接相关计划可以用作 initPlan。对 EXISTS、
	 * EXPR 或 ARRAY，返回一个 Param，指向求值
	 * initPlan 的结果。对 ROWCOMPARE，必须修改 testexpr 树，使它
	 * 含有 PARAM_EXEC Param，而不是解析器发出的 PARAM_SUBLINK Param，
	 * 然后返回那棵树。对 MULTIEXPR，返回一个
	 * 空常量：含有该 SubLink 的 resjunk 目标列表项
	 * 不必返回任何有用的东西，因为引用它的 Param 在
	 * 别处。
	 */
	if (splan->parParam == NIL && subLinkType == EXISTS_SUBLINK)
	{
		Param	   *prm;

		Assert(testexpr == NULL);
		prm = generate_new_exec_param(root, BOOLOID, -1, InvalidOid);
		splan->setParam = list_make1_int(prm->paramid);
		isInitPlan = true;
		result = (Node *) prm;
	}
	else if (splan->parParam == NIL && subLinkType == EXPR_SUBLINK)
	{
		TargetEntry *te = linitial(plan->targetlist);
		Param	   *prm;

		Assert(!te->resjunk);
		Assert(testexpr == NULL);
		prm = generate_new_exec_param(root,
									  exprType((Node *) te->expr),
									  exprTypmod((Node *) te->expr),
									  exprCollation((Node *) te->expr));
		splan->setParam = list_make1_int(prm->paramid);
		isInitPlan = true;
		result = (Node *) prm;
	}
	else if (splan->parParam == NIL && subLinkType == ARRAY_SUBLINK)
	{
		TargetEntry *te = linitial(plan->targetlist);
		Oid			arraytype;
		Param	   *prm;

		Assert(!te->resjunk);
		Assert(testexpr == NULL);
		arraytype = get_promoted_array_type(exprType((Node *) te->expr));
		if (!OidIsValid(arraytype))
			elog(ERROR, "could not find array type for datatype %s",
				 format_type_be(exprType((Node *) te->expr)));
		prm = generate_new_exec_param(root,
									  arraytype,
									  exprTypmod((Node *) te->expr),
									  exprCollation((Node *) te->expr));
		splan->setParam = list_make1_int(prm->paramid);
		isInitPlan = true;
		result = (Node *) prm;
	}
	else if (splan->parParam == NIL && subLinkType == ROWCOMPARE_SUBLINK)
	{
		/* 调整这些 Param */
		List	   *params;

		Assert(testexpr != NULL);
		params = generate_subquery_params(root,
										  plan->targetlist,
										  &splan->paramIds);
		result = convert_testexpr(root,
								  testexpr,
								  params);
		splan->setParam = list_copy(splan->paramIds);
		isInitPlan = true;

		/*
		 * 可执行表达式被返回，成为外层
		 * 计划表达式树的一部分；它不留在 initplan 节点里。
		 */
	}
	else if (subLinkType == MULTIEXPR_SUBLINK)
	{
		/*
		 * 无论是不是 initplan，都要为每个输出列设置一个 PARAM_EXEC Param。
		 * 输出列各一个。
		 */
		List	   *params;

		Assert(testexpr == NULL);
		params = generate_subquery_params(root,
										  plan->targetlist,
										  &splan->setParam);

		/*
		 * 把替换 Param 的列表存进
		 * root->multiexpr_params 的第 n 个单元；setrefs.c 会用它替换
		 * PARAM_MULTIEXPR Param。
		 */
		while (list_length(root->multiexpr_params) < subLinkId)
			root->multiexpr_params = lappend(root->multiexpr_params, NIL);
		lc = list_nth_cell(root->multiexpr_params, subLinkId - 1);
		Assert(lfirst(lc) == NIL);
		lfirst(lc) = params;

		/* 若没有 parParam，就可以做成 initplan。 */
		if (splan->parParam == NIL)
		{
			isInitPlan = true;
			result = (Node *) makeNullConst(RECORDOID, -1, InvalidOid);
		}
		else
		{
			isInitPlan = false;
			result = (Node *) splan;
		}
	}
	else
	{
		/*
		 * 调整 testexpr 里的 Param，除非调用方已经处理过
		 * （由传入一份 Param ID 列表来表明）。
		 */
		if (testexpr && testexpr_paramids == NIL)
		{
			List	   *params;

			params = generate_subquery_params(root,
											  plan->targetlist,
											  &splan->paramIds);
			splan->testexpr = convert_testexpr(root,
											   testexpr,
											   params);
		}
		else
		{
			splan->testexpr = testexpr;
			splan->paramIds = testexpr_paramids;
		}

		/*
		 * 不能把 ALL_SUBLINK 或 ANY_SUBLINK 类型的子计划转换成
		 * initPlan，即使它们不相关或非直接相关，
		 * 因为对每个外侧
		 * 元组都要扫描子计划的输出。但若是非直接相关的 IN（= ANY）测试，
		 * 也许能用哈希表避免比较全部元组。
		 */
		if (subLinkType == ANY_SUBLINK &&
			splan->parParam == NIL &&
			subplan_is_hashable(plan) &&
			testexpr_is_hashable(splan->testexpr, splan->paramIds))
			splan->useHashTable = true;

		/*
		 * 否则，可以选择在子计划顶上加一个 Material 节点，
		 * 以降低反复读取的代价。对直接相关的子计划这
		 * 没有意义，因为反正每次都得
		 * 重算结果。对不相关/非直接
		 * 相关的子计划，除非子计划的顶层计划
		 * 节点本来就会物化输出，否则加上 Material。另外，若 enable_material
		 * 为假，则用户不希望我们做不必要的物化，
		 * 因此就不加。
		 */
		else if (splan->parParam == NIL && enable_material &&
				 !ExecMaterializesOutput(nodeTag(plan)))
			plan = materialize_finished_plan(plan);

		result = (Node *) splan;
		isInitPlan = false;
	}

	/*
	 * 把子计划及其 PlannerInfo 加进全局列表。
	 */
	root->glob->subplans = lappend(root->glob->subplans, plan);
	root->glob->subroots = lappend(root->glob->subroots, subroot);
	splan->plan_id = list_length(root->glob->subplans);

	if (isInitPlan)
		root->init_plans = lappend(root->init_plans, splan);

	/*
	 * 无参数的子计划（不是 initplan）应当准备高效处理
	 * REWIND。若它有直接参数，就没有意义，
	 * 因为每次扫描反正都会重置；若是 initplan，
	 * 也没有意义，因为没有参数变化就不会重跑。
	 * 哈希子计划的输入也不需要 REWIND。
	 */
	if (splan->parParam == NIL && !isInitPlan && !splan->useHashTable)
		root->glob->rewindPlanIDs = bms_add_member(root->glob->rewindPlanIDs,
												   splan->plan_id);

	/* 为 EXPLAIN 给子计划加标签 */
	splan->plan_name = palloc(32 + 12 * list_length(splan->setParam));
	sprintf(splan->plan_name, "%s %d",
			isInitPlan ? "InitPlan" : "SubPlan",
			splan->plan_id);
	if (splan->setParam)
	{
		char	   *ptr = splan->plan_name + strlen(splan->plan_name);

		ptr += sprintf(ptr, " (returns ");
		foreach(lc, splan->setParam)
		{
			ptr += sprintf(ptr, "$%d%s",
						   lfirst_int(lc),
						   lnext(splan->setParam, lc) ? "," : ")");
		}
	}

	/* 最后，填入代价估计供以后使用 */
	cost_subplan(root, splan, plan);

	return result;
}

/*
 * generate_subquery_params：建立表示子链接的子 SELECT
 * 输出列的 Param 列表，给定该子 SELECT 的目标列表。
 *
 * 也返回这些 Param 的参数号整数列表。
 */
static List *
generate_subquery_params(PlannerInfo *root, List *tlist, List **paramIds)
{
	List	   *result;
	List	   *ids;
	ListCell   *lc;

	result = ids = NIL;
	foreach(lc, tlist)
	{
		TargetEntry *tent = (TargetEntry *) lfirst(lc);
		Param	   *param;

		if (tent->resjunk)
			continue;

		param = generate_new_exec_param(root,
										exprType((Node *) tent->expr),
										exprTypmod((Node *) tent->expr),
										exprCollation((Node *) tent->expr));
		result = lappend(result, param);
		ids = lappend_int(ids, param->paramid);
	}

	*paramIds = ids;
	return result;
}

/*
 * generate_subquery_vars：建立表示子链接的子 SELECT
 * 输出列的 Var 列表，给定该子 SELECT 的目标列表。
 * 这些 Var 具有指定的 varno（RTE 下标）。
 */
static List *
generate_subquery_vars(PlannerInfo *root, List *tlist, Index varno)
{
	List	   *result;
	ListCell   *lc;

	result = NIL;
	foreach(lc, tlist)
	{
		TargetEntry *tent = (TargetEntry *) lfirst(lc);
		Var		   *var;

		if (tent->resjunk)
			continue;

		var = makeVarFromTargetEntry(varno, tent);
		result = lappend(result, var);
	}

	return result;
}

/*
 * convert_testexpr：把解析器给出的 testexpr 转换成
 * 真正可执行的形式。这要把 PARAM_SUBLINK Param
 * 换成表示子 SELECT 结果的 Param 或 Var。要
 * 替换进去的节点作为
 * generate_subquery_params 或 generate_subquery_vars 的 List 结果传入。
 */
static Node *
convert_testexpr(PlannerInfo *root,
				 Node *testexpr,
				 List *subst_nodes)
{
	convert_testexpr_context context;

	context.root = root;
	context.subst_nodes = subst_nodes;
	return convert_testexpr_mutator(testexpr, &context);
}

static Node *
convert_testexpr_mutator(Node *node,
						 convert_testexpr_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_SUBLINK)
		{
			if (param->paramid <= 0 ||
				param->paramid > list_length(context->subst_nodes))
				elog(ERROR, "unexpected PARAM_SUBLINK ID: %d", param->paramid);

			/*
			 * 复制列表项，以免修改后的解析树里出现双重链接的
			 * 子结构。当它是 Param 时这大概
			 * 不必要，但求稳妥。
			 */
			return (Node *) copyObject(list_nth(context->subst_nodes,
												param->paramid - 1));
		}
	}
	if (IsA(node, SubLink))
	{
		/*
		 * 若遇到嵌套的 SubLink，递归进去既不必要也
		 * 不正确：里面可能找到的任何 PARAM_SUBLINKs 属于
		 * 内层 SubLink 而不是外层。因此原样返回。
		 *
		 * 这个推理依赖一个假定：不会有东西把
		 * 子表达式拉进或拉出 SubLink 的 testexpr 字段，至少
		 * 不会在先替换 PARAM_SUBLINKs 之前这样做。若真想
		 * 那样做，就得重新考虑解析器输出的表示
		 * 整体，因为目前 PARAM_SUBLINKs 只在每个
		 * SubLink 内唯一，而不是在整个查询中全局唯一。替换成
		 * Var 或 PARAM_EXEC 节点的全部意义，就是让它们在
		 * 逃出 SubLink 的 testexpr 之前全局唯一。
		 *
		 * 注意：在 SS_process_sublinks 期间调用时不会发生这种情况，
		 * 因为它先递归处理内层 SubLinks。不过从
		 * convert_ANY_sublink_to_join 调用时可能发生。
		 */
		return node;
	}
	return expression_tree_mutator(node,
								   convert_testexpr_mutator,
								   (void *) context);
}

/*
 * subplan_is_hashable：能否用哈希实现 ANY 子计划？
 *
 * 这里不负责检查组合用的 testexpr
 * 是否适合哈希。只看子查询本身。
 */
static bool
subplan_is_hashable(Plan *plan)
{
	double		subquery_size;

	/*
	 * 子查询结果的估计规模必须放进 hash_mem。（注意：
	 * 这里用堆元组开销，尽管元组实际会
	 * 存成 MinimalTuples；这为哈希表
	 * 开销留一点余量。）
	 */
	subquery_size = plan->plan_rows *
		(MAXALIGN(plan->plan_width) + MAXALIGN(SizeofHeapTupleHeader));
	if (subquery_size > get_hash_memory_limit())
		return false;

	return true;
}

/*
 * subpath_is_hashable：能否用哈希实现 ANY 子计划？
 *
 * 与 subplan_is_hashable 相同，但从子计划的 Path 出发。
 */
static bool
subpath_is_hashable(Path *path)
{
	double		subquery_size;

	/*
	 * 子查询结果的估计规模必须放进 hash_mem。（注意：
	 * 这里用堆元组开销，尽管元组实际会
	 * 存成 MinimalTuples；这为哈希表
	 * 开销留一点余量。）
	 */
	subquery_size = path->rows *
		(MAXALIGN(path->pathtarget->width) + MAXALIGN(SizeofHeapTupleHeader));
	if (subquery_size > get_hash_memory_limit())
		return false;

	return true;
}

/*
 * testexpr_is_hashable：ANY SubLink 的测试表达式可否哈希？
 *
 * 要区分哈希表达式的左侧和右侧，必须给定
 * 该 SubLink 子查询的输出 Param ID 列表。
 */
static bool
testexpr_is_hashable(Node *testexpr, List *param_ids)
{
	/*
	 * testexpr 必须是单个 OpExpr，或只含
	 * OpExprs 的 AND 子句，且每个都满足 test_opexpr_is_hashable()。
	 */
	if (testexpr && IsA(testexpr, OpExpr))
	{
		if (test_opexpr_is_hashable((OpExpr *) testexpr, param_ids))
			return true;
	}
	else if (is_andclause(testexpr))
	{
		ListCell   *l;

		foreach(l, ((BoolExpr *) testexpr)->args)
		{
			Node	   *andarg = (Node *) lfirst(l);

			if (!IsA(andarg, OpExpr))
				return false;
			if (!test_opexpr_is_hashable((OpExpr *) andarg, param_ids))
				return false;
		}
		return true;
	}

	return false;
}

static bool
test_opexpr_is_hashable(OpExpr *testexpr, List *param_ids)
{
	/*
	 * 组合操作符必须可哈希且严格。需要
	 * 可哈希性是显然的，因为要用哈希。没有
	 * 严格性，有空值时的行为太不可预测。实际上
	 * 还必须假定比普通严格性更多：对非空输入它也不能得出
	 * NULL（见 nodeSubplan.c）。不过哈希
	 * 索引和哈希连接也这样假定。
	 */
	if (!hash_ok_operator(testexpr))
		return false;

	/*
	 * 左右输入必须分别属于外层查询和内层查询；
	 * 因此将由子查询提供的 Param 不得
	 * 出现在左侧，外层查询的 Var 也不得出现在
	 * 右侧。（通常这必定为真，因为
	 * 解析器构建 ANY SubLink 的 testexpr 的方式……但函数内联
	 * 可能已经改变表达式结构，所以必须检查。
	 * 这类情形不够常见，不值得设法优化，因此
	 * 不必操心交换子句之类的事；
	 * 只须确保不建立无效计划。）
	 */
	if (list_length(testexpr->args) != 2)
		return false;
	if (contain_exec_param((Node *) linitial(testexpr->args), param_ids))
		return false;
	if (contain_var_clause((Node *) lsecond(testexpr->args)))
		return false;
	return true;
}

/*
 * 检查表达式是否可哈希且严格
 *
 * 可以用 op_hashjoinable() 和 op_strict()，但这样做是为了
 * 避免一次多余的缓存查找。
 */
static bool
hash_ok_operator(OpExpr *expr)
{
	Oid			opid = expr->opno;

	/* 若不是二元操作符就快速退出 */
	if (list_length(expr->args) != 2)
		return false;
	if (opid == ARRAY_EQ_OP ||
		opid == RECORD_EQ_OP)
	{
		/* 这些是严格的，但必须检查输入类型以确保可哈希 */
		Node	   *leftarg = linitial(expr->args);

		return op_hashjoinable(opid, exprType(leftarg));
	}
	else
	{
		/* 否则必须查找操作符属性 */
		HeapTuple	tup;
		Form_pg_operator optup;

		tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(opid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for operator %u", opid);
		optup = (Form_pg_operator) GETSTRUCT(tup);
		if (!optup->oprcanhash || !func_strict(optup->oprcode))
		{
			ReleaseSysCache(tup);
			return false;
		}
		ReleaseSysCache(tup);
		return true;
	}
}


/*
 * SS_process_ctes：处理查询的 WITH 列表
 *
 * 考虑 WITH 列表中的每个 CTE，或者忽略它（若是
 * 未被引用的 SELECT），或者"内联"它以建立普通的 FROM 中子 SELECT，
 * 或者把它转换成 initplan。
 *
 * 副作用是填好 root->cte_plan_ids，这份列表
 * 与 root->parse->cteList 平行，并给出
 * 每个 CTE 的 initplan 的子计划 ID，若没做 initplan 则为哑 ID（-1）。
 */
void
SS_process_ctes(PlannerInfo *root)
{
	ListCell   *lc;

	Assert(root->cte_plan_ids == NIL);

	foreach(lc, root->parse->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);
		CmdType		cmdType = ((Query *) cte->ctequery)->commandType;
		Query	   *subquery;
		PlannerInfo *subroot;
		RelOptInfo *final_rel;
		Path	   *best_path;
		Plan	   *plan;
		SubPlan    *splan;
		int			paramid;

		/*
		 * 忽略实际上没有任何地方引用的 SELECT CTEs。
		 */
		if (cte->cterefcount == 0 && cmdType == CMD_SELECT)
		{
			/* 在 cte_plan_ids 里做一个哑项 */
			root->cte_plan_ids = lappend_int(root->cte_plan_ids, -1);
			continue;
		}

		/*
		 * 考虑内联该 CTE（建立 RTE_SUBQUERY 的 RTE），而不是
		 * 把它实现成单独规划的 CTE。
		 *
		 * 若下列任一条件成立，就无法内联：
		 *
		 * 1. 用户说不要（CTEMaterializeAlways 选项）。
		 *
		 * 2. 该 CTE 是递归的。
		 *
		 * 3. 该 CTE 有副作用；这包括不是普通
		 * SELECT，或含有易变函数。内联可能改变
		 * 副作用，那会很糟。
		 *
		 * 4. 该 CTE 被多次引用，并且含有对
		 * 自身之外某个递归 CTE 的自引用。内联会造成多处
		 * 递归自引用，我们不支持。
		 *
		 * 否则，内联与否是可选的。若只有
		 * 一处引用，那应当总是划算的，但若该 CTE
		 * 被多次引用就不清楚：内联会增加重复
		 * 计算，但吸收外层
		 * 查询层次限制的能力可能抵消这一点。此时远远没有足够
		 * 信息来判断是否如此，因此让
		 * 用户表达偏好。默认行为是只内联
		 * 单次引用的 CTEs，但标了 CTEMaterializeNever 的 CTE
		 * 即使被多次引用也会内联。
		 *
		 * 注意：最后检查易变函数，因为那比
		 * 其他所需测试更贵。
		 */
		if ((cte->ctematerialized == CTEMaterializeNever ||
			 (cte->ctematerialized == CTEMaterializeDefault &&
			  cte->cterefcount == 1)) &&
			!cte->cterecursive &&
			cmdType == CMD_SELECT &&
			!contain_dml(cte->ctequery) &&
			(cte->cterefcount <= 1 ||
			 !contain_outer_selfref(cte->ctequery)) &&
			!contain_volatile_functions(cte->ctequery))
		{
			inline_cte(root, cte);
			/* 在 cte_plan_ids 里做一个哑项 */
			root->cte_plan_ids = lappend_int(root->cte_plan_ids, -1);
			continue;
		}

		/*
		 * 复制源 Query 节点。大概不必要，但让
		 * 这与 make_subplan 保持相似。
		 */
		subquery = (Query *) copyObject(cte->ctequery);

		/* 当前查询层次不应当在使用 plan_params */
		Assert(root->plan_params == NIL);

		/*
		 * 为 CTE 查询生成路径。总是按全部取出来规划
		 * --- 没有足够信息做别的预测。
		 */
		subroot = subquery_planner(root->glob, subquery,
								   root,
								   cte->cterecursive, 0.0);

		/*
		 * 由于当前查询层次还没有任何 RTEs，
		 * CTE 不可能请求
		 * 这一层的参数。
		 */
		if (root->plan_params)
			elog(ERROR, "unexpected outer reference in CTE query");

		/*
		 * 选出最佳路径并把它变成计划。至少目前，
		 * 似乎没有理由推迟这件事。
		 */
		final_rel = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
		best_path = final_rel->cheapest_total_path;

		plan = create_plan(subroot, best_path);

		/*
		 * 为它做一个 SubPlan 节点。这和
		 * build_subplan 差别刚好大到不能共享代码。
		 *
		 * 注意 plan_id、plan_name 和代价字段在更下面设置。
		 */
		splan = makeNode(SubPlan);
		splan->subLinkType = CTE_SUBLINK;
		splan->testexpr = NULL;
		splan->paramIds = NIL;
		get_first_col_type(plan, &splan->firstColType, &splan->firstColTypmod,
						   &splan->firstColCollation);
		splan->useHashTable = false;
		splan->unknownEqFalse = false;

		/*
		 * CTE 扫描不考虑并行（参见
		 * set_rel_consider_parallel），即便考虑，initPlan 也不是
		 * 并行安全的。
		 */
		splan->parallel_safe = false;
		splan->setParam = NIL;
		splan->parParam = NIL;
		splan->args = NIL;

		/*
		 * 该节点不能有任何输入（因为它是 initplan），因此
		 * parParam 和 args 列表保持为空。（它可能含有对
		 * 更早 CTEs 输出参数号的引用，但 CTE 输出不是
		 * 经由 args 列表传播的。）
		 */

		/*
		 * 分配一个参数号来表示该 CTE 的输出。这个参数槽从来不会做普通的
		 * "求值"，但我们用这个参数
		 * 号做 setParam/chgParam 信号，就好像该 CTE 计划
		 * 返回一个简单标量输出。（执行器还会滥用
		 * 这个参数号的 ParamExecData 槽，供
		 * 可能在扫描该 CTE 的多个 CteScan 节点之间通信。）
		 */
		paramid = assign_special_exec_param(root);
		splan->setParam = list_make1_int(paramid);

		/*
		 * 把子计划及其 PlannerInfo 加进全局列表。
		 */
		root->glob->subplans = lappend(root->glob->subplans, plan);
		root->glob->subroots = lappend(root->glob->subroots, subroot);
		splan->plan_id = list_length(root->glob->subplans);

		root->init_plans = lappend(root->init_plans, splan);

		root->cte_plan_ids = lappend_int(root->cte_plan_ids, splan->plan_id);

		/* 为 EXPLAIN 给子计划加标签 */
		splan->plan_name = psprintf("CTE %s", cte->ctename);

		/* 最后，填入代价估计供以后使用 */
		cost_subplan(root, splan, plan);
	}
}

/*
 * contain_dml：是否有子查询不是普通 SELECT？
 *
 * 也拒绝 SELECT FOR UPDATE/SHARE 以及 INSERT 等。
 */
static bool
contain_dml(Node *node)
{
	return contain_dml_walker(node, NULL);
}

static bool
contain_dml_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;

		if (query->commandType != CMD_SELECT ||
			query->rowMarks != NIL)
			return true;

		return query_tree_walker(query, contain_dml_walker, context, 0);
	}
	return expression_tree_walker(node, contain_dml_walker, context);
}

/*
 * contain_outer_selfref：是否有外部递归自引用？
 */
static bool
contain_outer_selfref(Node *node)
{
	Index		depth = 0;

	/*
	 * 应当从一个 Query 开始，因此检查其直接内容时
	 * 深度为 1。
	 */
	Assert(IsA(node, Query));

	return contain_outer_selfref_walker(node, &depth);
}

static bool
contain_outer_selfref_walker(Node *node, Index *depth)
{
	if (node == NULL)
		return false;
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		/*
		 * 检查是否自引用了位于我们
		 * 开始搜索的那个 Query 之上的 CTE。
		 */
		if (rte->rtekind == RTE_CTE &&
			rte->self_reference &&
			rte->ctelevelsup >= *depth)
			return true;
		return false;			/* 允许 range_table_walker 继续 */
	}
	if (IsA(node, Query))
	{
		/* 递归进入子查询，正确跟踪嵌套深度 */
		Query	   *query = (Query *) node;
		bool		result;

		(*depth)++;

		result = query_tree_walker(query, contain_outer_selfref_walker,
								   (void *) depth, QTW_EXAMINE_RTES_BEFORE);

		(*depth)--;

		return result;
	}
	return expression_tree_walker(node, contain_outer_selfref_walker,
								  (void *) depth);
}

/*
 * inline_cte：把对给定 CTE 的 RTE_CTE 引用转换成 RTE_SUBQUERYs
 */
static void
inline_cte(PlannerInfo *root, CommonTableExpr *cte)
{
	struct inline_cte_walker_context context;

	context.ctename = cte->ctename;
	/* 从 levelsup = -1 开始，因为马上就会把它加一 */
	context.levelsup = -1;
	context.ctequery = castNode(Query, cte->ctequery);

	(void) inline_cte_walker((Node *) root->parse, &context);
}

static bool
inline_cte_walker(Node *node, inline_cte_walker_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;

		context->levelsup++;

		/*
		 * 在查询的 RTE 节点的内容之后再访问这些节点；否则
		 * query_tree_walker 会下降进入新内联的 CTE 查询，
		 * 那不是我们要的。
		 */
		(void) query_tree_walker(query, inline_cte_walker, context,
								 QTW_EXAMINE_RTES_AFTER);

		context->levelsup--;

		return false;
	}
	else if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		if (rte->rtekind == RTE_CTE &&
			strcmp(rte->ctename, context->ctename) == 0 &&
			rte->ctelevelsup == context->levelsup)
		{
			/*
			 * 找到一处要替换的引用。生成该 CTE 查询的一份副本，
			 * 对外层引用（例如
			 * 其他 CTEs）做适当的层次调整。
			 */
			Query	   *newquery = copyObject(context->ctequery);

			if (context->levelsup > 0)
				IncrementVarSublevelsUp((Node *) newquery, context->levelsup, 1);

			/*
			 * 把 RTE_CTE 的 RTE 转换成 RTE_SUBQUERY。
			 *
			 * 历史上，FOR UPDATE 子句被当作延伸
			 * 进视图和子查询，但不进 CTEs。我们保留这一
			 * 区别，不试图把行标记推进新的
			 * 子查询。
			 */
			rte->rtekind = RTE_SUBQUERY;
			rte->subquery = newquery;
			rte->security_barrier = false;

			/* 把 CTE 专用字段清零 */
			rte->ctename = NULL;
			rte->ctelevelsup = 0;
			rte->self_reference = false;
			rte->coltypes = NIL;
			rte->coltypmods = NIL;
			rte->colcollations = NIL;
		}

		return false;
	}

	return expression_tree_walker(node, inline_cte_walker, context);
}


/*
 * convert_ANY_sublink_to_join：试着把 ANY SubLink 转换成连接
 *
 * 调用方在查询的某条
 * 限定子句顶层找到了 ANY SubLink，但还没进一步检查该 SubLink 的性质。
 * 决定是否适合用连接风格处理这个 SubLink。
 * 若适合，就形成一个 JoinExpr 并返回它。若该 SubLink 无法
 * 转换成连接，就返回 NULL。
 *
 * 唯一不显然的输入参数是 available_rels：这是
 * 可以在子链接表达式里安全引用的查询关系集合。
 * （必须限制这一点，以免子链接
 * 出现在外连接的 ON 限定条件里时改变语义。）若转换后的限定条件会引用
 * 这些父查询 relid 以外的任何东西，转换必须失败。
 *
 * 成功时，返回的 JoinExpr 的 larg = NULL，rarg = 表示
 * 上拉后子查询的 jointree 项。调用方必须设置 larg，以
 * 表示新连接左侧的关系，并把
 * 该 JoinExpr 插到上层查询 jointree 的适当位置
 * （通常是左侧关系原来所在的地方）。注意传入的
 * SubLink 也必须从查询限定条件里的原位置去掉，
 * 因为返回的 JoinExpr 的限定条件取代了它。
 * （概念上，把该 SubLink 换成常量 TRUE，再从限定条件里
 * 消掉这个多余常量。）
 *
 * 成功时，调用方还负责对返回的 JoinExpr 的 rarg 和限定条件
 * 递归应用
 * pull_up_sublinks 处理。
 * （失败时不必做任何事，因为当我们递归规划子 SELECT 时
 *
 * 会应用 pull_up_sublinks。）
 * 成功转换的副作用包括把该 SubLink 的
 * 子查询加进查询的范围表，以便在 JoinExpr 的 rarg 里引用它。
 */
JoinExpr *
convert_ANY_sublink_to_join(PlannerInfo *root, SubLink *sublink,
							Relids available_rels)
{
	JoinExpr   *result;
	Query	   *parse = root->parse;
	Query	   *subselect = (Query *) sublink->subselect;
	Relids		upper_varnos;
	int			rtindex;
	ParseNamespaceItem *nsitem;
	RangeTblEntry *rte;
	RangeTblRef *rtr;
	List	   *subquery_vars;
	Node	   *quals;
	ParseState *pstate;

	Assert(sublink->subLinkType == ANY_SUBLINK);

	/*
	 * 子查询不得引用父查询的任何 Var。（更高
	 * 层次的 Var 应当可以。）
	 */
	if (contain_vars_of_level((Node *) subselect, 1))
		return NULL;

	/*
	 * 测试表达式必须含有父查询的一些 Var，否则
	 * 它不会成为连接。（注意它不会有引用
	 * 子查询的 Var，而是 Param。）
	 */
	upper_varnos = pull_varnos(root, sublink->testexpr);
	if (bms_is_empty(upper_varnos))
		return NULL;

	/*
	 * 不过，它不能引用 available_rels 以外的任何东西。
	 */
	if (!bms_is_subset(upper_varnos, available_rels))
		return NULL;

	/*
	 * 组合操作符和左侧表达式不得是易变的。
	 */
	if (contain_volatile_functions(sublink->testexpr))
		return NULL;

	/* 为 addRangeTableEntryForSubquery 创建一个哑 ParseState */
	pstate = make_parsestate(NULL);

	/*
	 * 好，把子查询上拉到上层范围表。
	 *
	 * 这里依赖一个假定：外层查询没有对
	 * 内层的引用（除了下面要建的 Var，这必然为真）。
	 * 因此这比 pull_up_subqueries
	 * 要经历的简单得多。
	 */
	nsitem = addRangeTableEntryForSubquery(pstate,
										   subselect,
										   makeAlias("ANY_subquery", NIL),
										   false,
										   false);
	rte = nsitem->p_rte;
	parse->rtable = lappend(parse->rtable, rte);
	rtindex = list_length(parse->rtable);

	/*
	 * 为上拉的子查询形成一个 RangeTblRef。
	 */
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = rtindex;

	/*
	 * 建立表示子查询输出的 Var 列表。
	 */
	subquery_vars = generate_subquery_vars(root,
										   subselect->targetList,
										   rtindex);

	/*
	 * 建立新连接的限定表达式，用这些 Var 替换 Param。
	 */
	quals = convert_testexpr(root, sublink->testexpr, subquery_vars);

	/*
	 * 最后，建立 JoinExpr 节点。
	 */
	result = makeNode(JoinExpr);
	result->jointype = JOIN_SEMI;
	result->isNatural = false;
	result->larg = NULL;		/* 调用方必须填上这个 */
	result->rarg = (Node *) rtr;
	result->usingClause = NIL;
	result->join_using_alias = NULL;
	result->quals = quals;
	result->alias = NULL;
	result->rtindex = 0;		/* 不需要为它建 RTE */

	return result;
}

/*
 * convert_EXISTS_sublink_to_join：试着把 EXISTS SubLink 转换成连接
 *
 * 本函数的 API 与 convert_ANY_sublink_to_join 相同，
 * 只是还支持调用方找到的是 NOT EXISTS 的情形，
 * 因此需要额外的输入参数 "under_not"。
 */
JoinExpr *
convert_EXISTS_sublink_to_join(PlannerInfo *root, SubLink *sublink,
							   bool under_not, Relids available_rels)
{
	JoinExpr   *result;
	Query	   *parse = root->parse;
	Query	   *subselect = (Query *) sublink->subselect;
	Node	   *whereClause;
	int			rtoffset;
	int			varno;
	Relids		clause_varnos;
	Relids		upper_varnos;

	Assert(sublink->subLinkType == EXISTS_SUBLINK);

	/*
	 * 若含有 WITH 就不能展平。（可以安排把
	 * WITH 上拉到父查询的 cteList，但那有改变
	 * 语义的风险，因为 WITH 应当对每次关联的查询
	 * 调用执行一次。）注意 convert_ANY_sublink_to_join 不必拒绝
	 * 这种情形，因为它只产生一个子查询 RTE，不必
	 * 展平进父查询。
	 */
	if (subselect->cteList)
		return NULL;

	/*
	 * 复制子查询以便安全地修改它（见
	 * make_subplan 里的注释）。
	 */
	subselect = copyObject(subselect);

	/*
	 * 看能否根据它用于 EXISTS() 这一点来简化子查询。
	 * 若没法去掉它的
	 * 目标列表，就必须失败，因为上拉操作之后没有
	 * 地方求值目标列表。
	 */
	if (!simplify_EXISTS_query(root, subselect))
		return NULL;

	/*
	 * 把 WHERE 子句分离出来。（理论上也可以去掉
	 * 顶层的普通 JOIN/ON 子句，但大概不值得
	 * 费这个事。）
	 */
	whereClause = subselect->jointree->quals;
	subselect->jointree->quals = NULL;

	/*
	 * 子查询的其余部分不得引用父查询的任何 Var。
	 * （更高层次的 Var 应当可以。）
	 */
	if (contain_vars_of_level((Node *) subselect, 1))
		return NULL;

	/*
	 * 另一方面，WHERE 子句必须含有父查询的一些 Var，
	 * 否则它不会成为连接。
	 */
	if (!contain_vars_of_level(whereClause, 1))
		return NULL;

	/*
	 * 若 WHERE 子句是易变的，也不冒险优化。
	 */
	if (contain_volatile_functions(whereClause))
		return NULL;

	/*
	 * 子查询必须有非空的 jointree，但我们可以让它如此。
	 */
	replace_empty_jointree(subselect);

	/*
	 * 准备把子查询上拉到顶层范围表。
	 *
	 * 这里依赖一个假定：外层查询没有对
	 * 内层的引用（这必然为真）。因此这比
	 * pull_up_subqueries 要经历的简单得多。
	 *
	 * 事实上，这甚至比 convert_ANY_sublink_to_join 要
	 * 做的更简单。simplify_EXISTS_query 的处理确保了子查询里除了
	 * rtable 和 jointree 没有别的有意思的东西，而且
	 * 连 jointree 的 FromExpr 也不再有限定条件。因此可以只把
	 * rtable 追加到我们自己的，并在 jointree 里使用该 FromExpr。但首先，
	 * 调整子查询里所有层次为零的 varno，以计入 rtable
	 * 合并。
	 */
	rtoffset = list_length(parse->rtable);
	OffsetVarNodes((Node *) subselect, rtoffset, 0);
	OffsetVarNodes(whereClause, rtoffset, 0);

	/*
	 * 子查询里的上层变量现在离它们的
	 * 父项比以前近一层；尤其是，原来是层次 1 的
	 * 变成层次零。
	 */
	IncrementVarSublevelsUp((Node *) subselect, -1, 1);
	IncrementVarSublevelsUp(whereClause, -1, 1);

	/*
	 * 既然 WHERE 子句已调整到匹配父查询
	 * 环境，就能轻易识别它使用的全部层次为零的关系。
	 * 那些 <= rtoffset 的属于上层查询；那些 > rtoffset 的
	 * 不属于。
	 */
	clause_varnos = pull_varnos(root, whereClause);
	upper_varnos = NULL;
	varno = -1;
	while ((varno = bms_next_member(clause_varnos, varno)) >= 0)
	{
		if (varno <= rtoffset)
			upper_varnos = bms_add_member(upper_varnos, varno);
	}
	bms_free(clause_varnos);
	Assert(!bms_is_empty(upper_varnos));

	/*
	 * 既然有了上层 varno 的集合，就可以做最后
	 * 一项检查：只能引用 available_rels。
	 */
	if (!bms_is_subset(upper_varnos, available_rels))
		return NULL;

	/*
	 * 现在可以把修改后的子查询 rtable 接到父项上。这也会
	 * 把子查询的 RTEPermissionInfos 加进上层查询。
	 */
	CombineRangeTables(&parse->rtable, &parse->rteperminfos,
					   subselect->rtable, subselect->rteperminfos);

	/*
	 * 最后，建立 JoinExpr 节点。
	 */
	result = makeNode(JoinExpr);
	result->jointype = under_not ? JOIN_ANTI : JOIN_SEMI;
	result->isNatural = false;
	result->larg = NULL;		/* 调用方必须填上这个 */
	/* 若 FromExpr 节点没用，就把它展平 */
	if (list_length(subselect->jointree->fromlist) == 1)
		result->rarg = (Node *) linitial(subselect->jointree->fromlist);
	else
		result->rarg = (Node *) subselect->jointree;
	result->usingClause = NIL;
	result->join_using_alias = NULL;
	result->quals = whereClause;
	result->alias = NULL;
	result->rtindex = 0;		/* 不需要为它建 RTE */

	return result;
}

/*
 * simplify_EXISTS_query：去掉 EXISTS 的子查询里任何无用的东西
 *
 * EXISTS 查询唯一要紧的是它返回
 * 零行还是多于零行。因此可以去掉不影响这一点的 SQL 特性。
 * 典型用法里真正可能要紧的是简化目标列表：
 * 人们习惯写
 * "SELECT * FROM"，即使不必求值任何列。
 *
 * 注意：压掉目标列表可能造成可观察的行为
 * 变化，即求值目标列表时可能发生的任何错误
 * 不会发生，易变函数的其他副作用也不会。这在实践中似乎
 * 不太会打扰到谁。
 *
 * 若能丢掉目标列表则返回真，否则返回假。
 */
static bool
simplify_EXISTS_query(PlannerInfo *root, Query *query)
{
	/*
	 * 若查询使用集合运算、
	 * 聚合、分组集、SRFs、修改性 CTEs、HAVING、OFFSET 或 FOR
	 * UPDATE/SHARE，就完全不尝试简化；这些在正常用法里似乎都不太可能，而且它们
	 * 可能的效应很复杂。（注意：可以忽略 "OFFSET 0"
	 * 子句，但那传统上用作优化栅栏，因此
	 * 不忽略。）
	 */
	if (query->commandType != CMD_SELECT ||
		query->setOperations ||
		query->hasAggs ||
		query->groupingSets ||
		query->hasWindowFuncs ||
		query->hasTargetSRFs ||
		query->hasModifyingCTE ||
		query->havingQual ||
		query->limitOffset ||
		query->rowMarks)
		return false;

	/*
	 * 带常量正值（或 NULL）的 LIMIT 不影响
	 * EXISTS 的语义，因此忽略这种子句。值得做，
	 * 因为习惯了某些其他 DBMSes 的人可能习惯
	 * 把 EXISTS(SELECT ... LIMIT 1) 写成一种优化。若
	 * LIMIT 的参数是别的东西，就不能简化。
	 */
	if (query->limitCount)
	{
		/*
		 * LIMIT 子句还没经过 eval_const_expressions，
		 * 所以必须在这里应用。这似乎是浪费
		 * 周期，因为唯一值得操心的情形大概是
		 * "LIMIT 1"……但实际会看到的是 "LIMIT int8(1::int4)"，
		 * 因此必须折叠常量，否则认不出来。
		 */
		Node	   *node = eval_const_expressions(root, query->limitCount);
		Const	   *limit;

		/* 若简化了该子句，不妨更新查询。 */
		query->limitCount = node;

		if (!IsA(node, Const))
			return false;

		limit = (Const *) node;
		Assert(limit->consttype == INT8OID);
		if (!limit->constisnull && DatumGetInt64(limit->constvalue) <= 0)
			return false;

		/* 无论目标列表是否安全，都可以丢掉 LIMIT。 */
		query->limitCount = NULL;
	}

	/*
	 * 否则可以扔掉目标列表，以及任何 GROUP、
	 * WINDOW、DISTINCT 和 ORDER BY 子句；这些子句都不会
	 * 把非零行结果变成零行，或反过来。（而且，
	 * 由于这些子句的解析树表示依赖
	 * 目标列表，若丢掉目标列表就最好把它们也扔掉。）
	 */
	query->targetList = NIL;
	query->groupClause = NIL;
	query->windowClause = NIL;
	query->distinctClause = NIL;
	query->sortClause = NIL;
	query->hasDistinctOn = false;

	return true;
}

/*
 * convert_EXISTS_to_ANY：试着把 EXISTS 转换成可哈希的 ANY 子链接
 *
 * 子查询预期是一份可以改动的新副本，
 * 并且已经成功通过 simplify_EXISTS_query。
 *
 * 成功时返回修改后的子查询，并把合适的
 * 上层测试表达式存进 *testexpr，再加上子查询
 * 输出 Param 的列表到 *paramIds。（测试表达式已经 Param 化，
 * 因此不必经过 convert_testexpr，这就是必须
 * 特别处理 Param ID 的原因。）
 *
 * 失败时返回 NULL。
 */
static Query *
convert_EXISTS_to_ANY(PlannerInfo *root, Query *subselect,
					  Node **testexpr, List **paramIds)
{
	Node	   *whereClause;
	List	   *leftargs,
			   *rightargs,
			   *opids,
			   *opcollations,
			   *newWhere,
			   *tlist,
			   *testlist,
			   *paramids;
	ListCell   *lc,
			   *rc,
			   *oc,
			   *cc;
	AttrNumber	resno;

	/*
	 * 查询不得需要目标列表，因为必须插入一个新的。
	 * 调用方应当已经处理过这种情形。
	 */
	Assert(subselect->targetList == NIL);

	/*
	 * 把 WHERE 子句分离出来。（理论上也可以去掉
	 * 顶层的普通 JOIN/ON 子句，但大概不值得
	 * 费这个事。）
	 */
	whereClause = subselect->jointree->quals;
	subselect->jointree->quals = NULL;

	/*
	 * 子查询的其余部分不得引用父查询的任何 Var。
	 * （更高层次的 Var 应当可以。）
	 *
	 * 注意：不必单独检查 Aggref，因为知道
	 * 子查询还没优化；任何上层 Aggref 因此
	 * 必定含有上层 Var 引用。下面的情形不是这样……
	 */
	if (contain_vars_of_level((Node *) subselect, 1))
		return NULL;

	/*
	 * 若 WHERE 子句是易变的，也不冒险优化。
	 */
	if (contain_volatile_functions(whereClause))
		return NULL;

	/*
	 * 对 WHERE 子句做常量简化等清理。
	 * 除了简化即将做的处理，这对
	 * 能把 WHERE 子句的一些片段上拉到
	 * 父查询也很重要。因为我们是在父项的
	 * preprocess_expression() 工作进行到一半时被调用的，除非在这里做，否则 preprocess_expression() 的更早步骤
	 * 不会应用到上拉的东西。对
	 * 放回子查询的那部分 WHERE 子句，
	 * 这项工作有部分重复，但不应当有害。
	 *
	 * 注意：不跑 flatten_join_alias_vars。这没问题，因为任何
	 * 父项别名已经展平，而且不会把任何
	 * 子 Var（无论哪种）拉进父项。
	 *
	 * 注意：把父项的 root 传给 eval_const_expressions 在
	 * 技术上是错的，但可以蒙混过关，因为只用到
	 * boundParams（若有），而那些在
	 * 子 root 里也会相同。
	 */
	whereClause = eval_const_expressions(root, whereClause);
	whereClause = (Node *) canonicalize_qual((Expr *) whereClause, false);
	whereClause = (Node *) make_ands_implicit((Expr *) whereClause);

	/*
	 * 现在有一份展平的隐式 AND 子句列表，试着把它
	 * 拆成 "outervar = innervar" 哈希子句。任何
	 * 拆不开的就放回 newWhere 列表。注意
	 * 还没努力确保每一侧只有外侧或只有
	 * 内侧；若能走到最后再检查。
	 */
	leftargs = rightargs = opids = opcollations = newWhere = NIL;
	foreach(lc, (List *) whereClause)
	{
		OpExpr	   *expr = (OpExpr *) lfirst(lc);

		if (IsA(expr, OpExpr) &&
			hash_ok_operator(expr))
		{
			Node	   *leftarg = (Node *) linitial(expr->args);
			Node	   *rightarg = (Node *) lsecond(expr->args);

			if (contain_vars_of_level(leftarg, 1))
			{
				leftargs = lappend(leftargs, leftarg);
				rightargs = lappend(rightargs, rightarg);
				opids = lappend_oid(opids, expr->opno);
				opcollations = lappend_oid(opcollations, expr->inputcollid);
				continue;
			}
			if (contain_vars_of_level(rightarg, 1))
			{
				/*
				 * 必须交换该子句，把外侧变量放在
				 * 左边，因为 nodeSubplan.c 里的哈希代码预期
				 * 如此。这大概不应当失败，因为可哈希
				 * 操作符应当有交换子，但还是谨慎一点。
				 */
				expr->opno = get_commutator(expr->opno);
				if (OidIsValid(expr->opno) && hash_ok_operator(expr))
				{
					leftargs = lappend(leftargs, rightarg);
					rightargs = lappend(rightargs, leftarg);
					opids = lappend_oid(opids, expr->opno);
					opcollations = lappend_oid(opcollations, expr->inputcollid);
					continue;
				}
				/* 若没有交换子，就没机会优化 WHERE 子句 */
				return NULL;
			}
		}
		/* 没法把它当作哈希子句处理 */
		newWhere = lappend(newWhere, expr);
	}

	/*
	 * 若没找到任何可以转换的东西，就失败。
	 */
	if (leftargs == NIL)
		return NULL;

	/*
	 * 打算放回子查询的东西里不得有任何父项 Var 或聚合。
	 * 注意：也许会觉得不必
	 * 单独检查聚合，因为上层聚合的参数里必定含有
	 * 上层 Var。但上层 Var 有可能
	 * 被 eval_const_expressions 优化掉。考虑
	 *
	 * SUM(CASE WHEN false THEN uplevelvar ELSE 0 END)
	 */
	if (contain_vars_of_level((Node *) newWhere, 1) ||
		contain_vars_of_level((Node *) rightargs, 1))
		return NULL;
	if (root->parse->hasAggs &&
		(contain_aggs_of_level((Node *) newWhere, 1) ||
		 contain_aggs_of_level((Node *) rightargs, 1)))
		return NULL;

	/*
	 * 而且打算上拉的东西里不能有任何子 Var。
	 * （注意：本来也得检查子聚合，只是知道子查询
	 * 因 simplify_EXISTS_query 的检查而完全没有聚合。窗口函数
	 * 也一样。）
	 */
	if (contain_vars_of_level((Node *) leftargs, 0))
		return NULL;

	/*
	 * 也拒绝打算上拉的东西里的子链接。（也许
	 * 可以支持，但似乎不值得为此增加复杂度。）
	 */
	if (contain_subplans((Node *) leftargs))
		return NULL;

	/*
	 * 好，调整正在上拉的东西里的 sublevelsup。
	 */
	IncrementVarSublevelsUp((Node *) leftargs, -1, 1);

	/*
	 * 把任何只属于子层次的 WHERE 子句放回去。
	 */
	if (newWhere)
		subselect->jointree->quals = (Node *) make_ands_explicit(newWhere);

	/*
	 * 为子查询建立新的目标列表，发出需要的表达式。
	 * 同时用 Param 为父项建立 testexpr，以
	 * 引用子输出。（因为这里直接生成 Param，
	 * build_subplan 里就不必转换 testexpr。）
	 */
	tlist = testlist = paramids = NIL;
	resno = 1;
	forfour(lc, leftargs, rc, rightargs, oc, opids, cc, opcollations)
	{
		Node	   *leftarg = (Node *) lfirst(lc);
		Node	   *rightarg = (Node *) lfirst(rc);
		Oid			opid = lfirst_oid(oc);
		Oid			opcollation = lfirst_oid(cc);
		Param	   *param;

		param = generate_new_exec_param(root,
										exprType(rightarg),
										exprTypmod(rightarg),
										exprCollation(rightarg));
		tlist = lappend(tlist,
						makeTargetEntry((Expr *) rightarg,
										resno++,
										NULL,
										false));
		testlist = lappend(testlist,
						   make_opclause(opid, BOOLOID, false,
										 (Expr *) leftarg, (Expr *) param,
										 InvalidOid, opcollation));
		paramids = lappend_int(paramids, param->paramid);
	}

	/* 把一切放到该放的地方，就完成了 */
	subselect->targetList = tlist;
	*testexpr = (Node *) make_ands_explicit(testlist);
	*paramIds = paramids;

	return subselect;
}


/*
 * 用 Param 替换相关变量（上层变量）。
 *
 * 上层的 PlaceHolderVars 和聚合也会被替换。
 *
 * 注意：必须紧接在 SS_process_sublinks 之后运行，这很关键。
 * 因为不递归进入上层 PHVs 和聚合的参数，
 * 它们会被复制到父查询里适当的子计划参数列表，
 * 上层变量不被 Param 替换，只调整层次
 * （见 replace_outer_placeholdervar 和 replace_outer_agg）。对父层次的变量，这正是
 * 我们要的 --- 但若某个 PHV 或
 * 聚合的参数含有更上层的变量，那些变量轮到它们时
 * 必须被 Param 替换。那会发生在父层次
 * 运行 SS_replace_correlation_vars 时。因此它必须在展开
 * 其子链接为子计划之后这样做。而且中间不要有任何步骤，否则
 * 那些步骤也永远不会应用到参数表达式，无论在
 * 父层次还是子层次。
 *
 * 这里另一件相当棘手的事是上层 PHV/聚合参数里的 SubLinks
 * 的处理。在中间查询层次内部也不碰
 * 它们。而是在把 PHV 或 Aggref 表达式复制到父计划层次之后，
 * 由 SS_process_sublinks 对它们递归
 * （这实际上在 build_subplan 里处理）。
 */
Node *
SS_replace_correlation_vars(PlannerInfo *root, Node *expr)
{
	/* 树遍历不需要准备，因此出发 */
	return replace_correlation_vars_mutator(expr, root);
}

static Node *
replace_correlation_vars_mutator(Node *node, PlannerInfo *root)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup > 0)
			return (Node *) replace_outer_var(root, (Var *) node);
	}
	if (IsA(node, PlaceHolderVar))
	{
		if (((PlaceHolderVar *) node)->phlevelsup > 0)
			return (Node *) replace_outer_placeholdervar(root,
														 (PlaceHolderVar *) node);
	}
	if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->agglevelsup > 0)
			return (Node *) replace_outer_agg(root, (Aggref *) node);
	}
	if (IsA(node, GroupingFunc))
	{
		if (((GroupingFunc *) node)->agglevelsup > 0)
			return (Node *) replace_outer_grouping(root, (GroupingFunc *) node);
	}
	return expression_tree_mutator(node,
								   replace_correlation_vars_mutator,
								   (void *) root);
}

/*
 * 把给定表达式里的 SubLinks 展开成 SubPlans。
 *
 * isQual 参数说明该表达式是否是 WHERE/HAVING
 * 限定表达式。若是，出现在顶层的任何子链接
 * 不必区分 FALSE 和 UNKNOWN 返回值。
 */
Node *
SS_process_sublinks(PlannerInfo *root, Node *expr, bool isQual)
{
	process_sublinks_context context;

	context.root = root;
	context.isTopQual = isQual;
	return process_sublinks_mutator(expr, &context);
}

static Node *
process_sublinks_mutator(Node *node, process_sublinks_context *context)
{
	process_sublinks_context locContext;

	locContext.root = context->root;

	if (node == NULL)
		return NULL;
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		Node	   *testexpr;

		/*
		 * 首先，若有左侧表达式，就递归处理它们。
		 * 它们不再是顶层。
		 */
		locContext.isTopQual = false;
		testexpr = process_sublinks_mutator(sublink->testexpr, &locContext);

		/*
		 * 现在建立 SubPlan 节点，并做出要返回的表达式。
		 */
		return make_subplan(context->root,
							(Query *) sublink->subselect,
							sublink->subLinkType,
							sublink->subLinkId,
							testexpr,
							context->isTopQual);
	}

	/*
	 * 这里不要递归进入外层 PHV、Aggref 或
	 * GroupingFunc 的参数。参数里的任何 SubLinks 必须在
	 * 外层查询层次处理；当 build_subplan
	 * 把该 PHV、Aggref 或 GroupingFunc 收进要
	 * 传给当前子计划的参数时会处理它们。
	 */
	if (IsA(node, PlaceHolderVar))
	{
		if (((PlaceHolderVar *) node)->phlevelsup > 0)
			return node;
	}
	else if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->agglevelsup > 0)
			return node;
	}
	else if (IsA(node, GroupingFunc))
	{
		if (((GroupingFunc *) node)->agglevelsup > 0)
			return node;
	}

	/*
	 * 输入里永远不应当看到 SubPlan 表达式（因为本例程
	 * 正是一开始创建它们的例程）。也不应当发现
	 * 自己被直接调用在一个 Query 上。
	 */
	Assert(!IsA(node, SubPlan));
	Assert(!IsA(node, AlternativeSubPlan));
	Assert(!IsA(node, Query));

	/*
	 * 因为 make_subplan() 可能返回 AND 或 OR 子句，必须
	 * 采取措施保持限定条件的 AND/OR 平坦。假定输入
	 * 已经 AND/OR 展平，因此这里不需要递归。
	 *
	 * （由于这里的写法，不会在 AND 的 List 子节点上被调用；
	 * 而且输入 *还没有* 变成隐式 AND 格式。因此
	 * 不需要对裸 List 做检查。）
	 *
	 * 在顶层 AND/OR 子句结构内的任何地方，都可以告诉
	 * make_subplan()：NULL 和 FALSE 可互换。因此 isTopQual
	 * 在两种情形里都向下传播。（注意这不同于 Postgres 里多数其他地方
	 * 所用的"顶层限定条件"的含义。）
	 */
	if (is_andclause(node))
	{
		List	   *newargs = NIL;
		ListCell   *l;

		/* 仍在限定条件顶层 */
		locContext.isTopQual = context->isTopQual;

		foreach(l, ((BoolExpr *) node)->args)
		{
			Node	   *newarg;

			newarg = process_sublinks_mutator(lfirst(l), &locContext);
			if (is_andclause(newarg))
				newargs = list_concat(newargs, ((BoolExpr *) newarg)->args);
			else
				newargs = lappend(newargs, newarg);
		}
		return (Node *) make_andclause(newargs);
	}

	if (is_orclause(node))
	{
		List	   *newargs = NIL;
		ListCell   *l;

		/* 仍在限定条件顶层 */
		locContext.isTopQual = context->isTopQual;

		foreach(l, ((BoolExpr *) node)->args)
		{
			Node	   *newarg;

			newarg = process_sublinks_mutator(lfirst(l), &locContext);
			if (is_orclause(newarg))
				newargs = list_concat(newargs, ((BoolExpr *) newarg)->args);
			else
				newargs = lappend(newargs, newarg);
		}
		return (Node *) make_orclause(newargs);
	}

	/*
	 * 若递归经过 AND 或 OR 节点以外的任何东西，就
	 * 肯定不再处于顶层限定条件。
	 */
	locContext.isTopQual = false;

	return expression_tree_mutator(node,
								   process_sublinks_mutator,
								   (void *) &locContext);
}

/*
 * SS_identify_outer_params - 识别外层可用的 Param
 *
 * 这必须在给定查询层次及其全部
 * 后代层次都完成 SS_replace_correlation_vars 和 SS_process_sublinks
 * 处理之后运行（因此最实际的是在该
 * 查询层次处理结束时做）。计算外层
 * 会提供给这一层加后代的 paramId 集合，并记到
 * root->outer_params，供最终
 * 计划清理时计算 extParam/allParam 集合使用。（那时不能直接算，因为上层的
 * plan_params 列表是临时的，到那时已经没了。）
 */
void
SS_identify_outer_params(PlannerInfo *root)
{
	Bitmapset  *outer_params;
	PlannerInfo *proot;
	ListCell   *l;

	/*
	 * 若整棵树里都没分配过参数，这里当然
	 * 不必做任何事。
	 */
	if (root->glob->paramExecTypes == NIL)
		return;

	/*
	 * 扫描这一层之上的全部查询层次，看哪些参数将由
	 * 它们提供，或者因为更低的查询层次
	 * 请求了它们（经由 plan_params），或者因为它们将由
	 * 那些层次的 initPlan 提供。
	 */
	outer_params = NULL;
	for (proot = root->parent_root; proot != NULL; proot = proot->parent_root)
	{
		/* 包括普通的 Var/PHV/Aggref/GroupingFunc 参数 */
		foreach(l, proot->plan_params)
		{
			PlannerParamItem *pitem = (PlannerParamItem *) lfirst(l);

			outer_params = bms_add_member(outer_params, pitem->paramId);
		}
		/* 包括外层 initPlan 的任何输出 */
		foreach(l, proot->init_plans)
		{
			SubPlan    *initsubplan = (SubPlan *) lfirst(l);
			ListCell   *l2;

			foreach(l2, initsubplan->setParam)
			{
				outer_params = bms_add_member(outer_params, lfirst_int(l2));
			}
		}
		/* 若正在规划递归查询，就包括工作表 ID */
		if (proot->wt_param_id >= 0)
			outer_params = bms_add_member(outer_params, proot->wt_param_id);
	}
	root->outer_params = outer_params;
}

/*
 * SS_charge_for_initplans - 在路径代价和并行性里计入 initplan
 *
 * 若当前查询层次创建了任何 initPlan，它们会
 * 接到从给定关系选出的那条路径所建立的计划树上。
 * 把该关系全部路径的代价都加上它们，
 * 并确保这些路径被标成并行不安全，因为目前不能
 * 把 initPlan 传给并行工作进程。
 *
 * 这与 SS_attach_initplans 分开，因为 create_plan() 期间也许会按条件
 * 再创建 initPlan，取决于选出哪条路径。
 * 不过，会产生这种 initPlan 的路径预期
 * 已经计入了它们的代价和并行安全效应。
 */
void
SS_charge_for_initplans(PlannerInfo *root, RelOptInfo *final_rel)
{
	Cost		initplan_cost;
	ListCell   *lc;

	/* 若没有 initPlan 就无事可做 */
	if (root->init_plans == NIL)
		return;

	/*
	 * 代价增量只算一次，因为对所有
	 * 路径都相同。假定每个 initPlan 在顶层计划启动期间跑一次。
	 * 这是保守的高估，因为事实上 initPlan 也许
	 * 在计划启动之后才执行，甚至根本不执行。
	 */
	initplan_cost = 0;
	foreach(lc, root->init_plans)
	{
		SubPlan    *initsubplan = (SubPlan *) lfirst(lc);

		initplan_cost += initsubplan->startup_cost + initsubplan->per_call_cost;
	}

	/*
	 * 现在调整代价和 parallel_safe 标志。
	 */
	foreach(lc, final_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		path->startup_cost += initplan_cost;
		path->total_cost += initplan_cost;
		path->parallel_safe = false;
	}

	/*
	 * 忘掉任何部分路径，并清掉 consider_parallel；
	 * 若接上了 initPlan，它们就不可用。
	 */
	final_rel->partial_pathlist = NIL;
	final_rel->consider_parallel = false;

	/* 这里不必做 set_cheapest()，调用方会做 */
}

/*
 * SS_attach_initplans - 把 initplan 接到最顶层计划节点
 *
 * 把当前查询层次创建的任何 initplan 接到指定的
 * 计划节点，它通常应当是该查询层次的最顶层节点。
 * （原则上 initPlan 可以放在引用它们的地方或其上的任何节点；
 * 但似乎没有理由放得比
 * 最顶层节点更低，因此不费心精确跟踪它们从哪来。）
 *
 * 不碰计划节点的代价或 parallel_safe 标志。这些 initplan
 * 必须已在 SS_charge_for_initplans 里计入，或由后来
 * 经由 SS_make_initplan_from_plan 添加 initplan 的代码计入。
 */
void
SS_attach_initplans(PlannerInfo *root, Plan *plan)
{
	plan->initPlan = root->init_plans;
}

/*
 * SS_finalize_plan - 对已完成的计划做最终参数处理。
 *
 * 这递归计算给定计划树中每个计划
 * 节点的 extParam 和 allParam 集合。（还有 RangeTblFunction.funcparams。）
 *
 * 假定对计划树可能引用的任何 initplan 或
 * 子计划已经跑过 SS_finalize_plan。
 */
void
SS_finalize_plan(PlannerInfo *root, Plan *plan)
{
	/* 不需要准备，只要递归走过计划树。 */
	(void) finalize_plan(root, plan, -1, root->outer_params, NULL);
}

/*
 * 递归处理计划树中的全部节点
 *
 * gather_param 是某个祖先 Gather/GatherMerge 的 rescan_param，
 * 若没有则为 -1。
 *
 * valid_params 是外层计划层次提供的、
 * 在本计划节点或其子节点里可以合法引用的参数号集合。
 *
 * scan_params 是要强制扫描计划节点引用的参数号集合。
 * 这是为了 EvalPlanQual 支持，
 * 在递归的顶层总是 NULL。
 *
 * 返回值是给定计划节点算出的 allParam 集合。
 * 这只是内部记法上的方便：可以把子
 * 计划的 allParams 加进本层关心的参数号集合，
 * 就在递归到该子节点的同一条语句里。
 *
 * 不要涂改调用方的 valid_params 或 scan_params 值！
 *
 * 注意：虽然试图处理树中任何地方的 initPlan，
 * 逻辑并不真的正确。问题是计划节点可能把
 * 其 initPlan 的输出 Param 作为目标列表项返回，这时父计划层次
 * 引用同一个 Param 是合法的；父项的使用
 * 会被 setrefs.c 转换成引用子计划节点的 Var。
 * 但本函数会把父项的引用看成超出范围并
 * 抱怨。目前这没关系，因为规划器只
 * 把 initPlan 接到查询层次的最顶层计划节点，因此这种情况
 * 不会出现。若将来把这项处理并进 setrefs.c，也许能
 * 处理得更干净。
 */
static Bitmapset *
finalize_plan(PlannerInfo *root, Plan *plan,
			  int gather_param,
			  Bitmapset *valid_params,
			  Bitmapset *scan_params)
{
	finalize_primnode_context context;
	int			locally_added_param;
	Bitmapset  *nestloop_params;
	Bitmapset  *initExtParam;
	Bitmapset  *initSetParam;
	Bitmapset  *child_params;
	ListCell   *l;

	if (plan == NULL)
		return NULL;

	context.root = root;
	context.paramids = NULL;	/* 把集合初始化为空 */
	locally_added_param = -1;	/* 没有那么一个 */
	nestloop_params = NULL;		/* 一个也没有 */

	/*
	 * 检查任何 initPlan，以确定它们引用的外部参数集合
	 * 以及它们提供的输出参数集合。（假定
	 * 已经对它们跑过 SS_finalize_plan。）
	 */
	initExtParam = initSetParam = NULL;
	foreach(l, plan->initPlan)
	{
		SubPlan    *initsubplan = (SubPlan *) lfirst(l);
		Plan	   *initplan = planner_subplan_get_plan(root, initsubplan);
		ListCell   *l2;

		initExtParam = bms_add_members(initExtParam, initplan->extParam);
		foreach(l2, initsubplan->setParam)
		{
			initSetParam = bms_add_member(initSetParam, lfirst_int(l2));
		}
	}

	/* 任何 setParam 在本节点及子节点里都可以合法引用 */
	if (initSetParam)
		valid_params = bms_union(valid_params, initSetParam);

	/*
	 * 调用 finalize_primnode 时，context.paramids 集合会自动
	 * 合并在一起。但递归到自身时，必须用笨
	 * 办法。希望 paramids 集合既包括子计划里的参数，也包括
	 * 这一层的参数。
	 */

	/* 找出目标列表和限定条件里的参数 */
	finalize_primnode((Node *) plan->targetlist, &context);
	finalize_primnode((Node *) plan->qual, &context);

	/*
	 * 若是并行感知的扫描节点，把它标成依赖父项
	 * Gather/GatherMerge 的重新扫描 Param。
	 */
	if (plan->parallel_aware)
	{
		if (gather_param < 0)
			elog(ERROR, "parallel-aware plan node is not below a Gather");
		context.paramids =
			bms_add_member(context.paramids, gather_param);
	}

	/* 检查节点类型特有的附加字段 */
	switch (nodeTag(plan))
	{
		case T_Result:
			finalize_primnode(((Result *) plan)->resconstantqual,
							  &context);
			break;

		case T_SeqScan:
			context.paramids = bms_add_members(context.paramids, scan_params);
			break;

		case T_SampleScan:
			finalize_primnode((Node *) ((SampleScan *) plan)->tablesample,
							  &context);
			context.paramids = bms_add_members(context.paramids, scan_params);
			break;

		case T_IndexScan:
			finalize_primnode((Node *) ((IndexScan *) plan)->indexqual,
							  &context);
			finalize_primnode((Node *) ((IndexScan *) plan)->indexorderby,
							  &context);

			/*
			 * 不必看 indexqualorig，因为它与 indexqual 有相同的
			 * 参数引用。同样可以忽略
			 * indexorderbyorig。
			 */
			context.paramids = bms_add_members(context.paramids, scan_params);
			break;

		case T_IndexOnlyScan:
			finalize_primnode((Node *) ((IndexOnlyScan *) plan)->indexqual,
							  &context);
			finalize_primnode((Node *) ((IndexOnlyScan *) plan)->recheckqual,
							  &context);
			finalize_primnode((Node *) ((IndexOnlyScan *) plan)->indexorderby,
							  &context);

			/*
			 * 不必看 indextlist，因为它无法含有 Param。
			 */
			context.paramids = bms_add_members(context.paramids, scan_params);
			break;

		case T_BitmapIndexScan:
			finalize_primnode((Node *) ((BitmapIndexScan *) plan)->indexqual,
							  &context);

			/*
			 * 不必看 indexqualorig，因为它与 indexqual 有相同的
			 * 参数引用。
			 */
			break;

		case T_BitmapHeapScan:
			finalize_primnode((Node *) ((BitmapHeapScan *) plan)->bitmapqualorig,
							  &context);
			context.paramids = bms_add_members(context.paramids, scan_params);
			break;

		case T_TidScan:
			finalize_primnode((Node *) ((TidScan *) plan)->tidquals,
							  &context);
			context.paramids = bms_add_members(context.paramids, scan_params);
			break;

		case T_TidRangeScan:
			finalize_primnode((Node *) ((TidRangeScan *) plan)->tidrangequals,
							  &context);
			context.paramids = bms_add_members(context.paramids, scan_params);
			break;

		case T_SubqueryScan:
			{
				SubqueryScan *sscan = (SubqueryScan *) plan;
				RelOptInfo *rel;
				Bitmapset  *subquery_params;

				/* 必须对子查询跑 finalize_plan */
				rel = find_base_rel(root, sscan->scan.scanrelid);
				subquery_params = rel->subroot->outer_params;
				if (gather_param >= 0)
					subquery_params = bms_add_member(bms_copy(subquery_params),
													 gather_param);
				finalize_plan(rel->subroot, sscan->subplan, gather_param,
							  subquery_params, NULL);

				/* 现在可以把它的 extParams 加到父项的参数里 */
				context.paramids = bms_add_members(context.paramids,
												   sscan->subplan->extParam);
				/* 不过也需要 scan_params */
				context.paramids = bms_add_members(context.paramids,
												   scan_params);
			}
			break;

		case T_FunctionScan:
			{
				FunctionScan *fscan = (FunctionScan *) plan;
				ListCell   *lc;

				/*
				 * 对每个函数
				 * 表达式独立调用 finalize_primnode，以便记录
				 * 各自引用了哪些参数，从而决定重新扫描时哪些需要
				 * 重新求值。
				 */
				foreach(lc, fscan->functions)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);
					finalize_primnode_context funccontext;

					funccontext = context;
					funccontext.paramids = NULL;

					finalize_primnode(rtfunc->funcexpr, &funccontext);

					/* 记住结果供执行使用 */
					rtfunc->funcparams = funccontext.paramids;

					/* 把该函数的参数加进总集合 */
					context.paramids = bms_add_members(context.paramids,
													   funccontext.paramids);
				}

				context.paramids = bms_add_members(context.paramids,
												   scan_params);
			}
			break;

		case T_TableFuncScan:
			finalize_primnode((Node *) ((TableFuncScan *) plan)->tablefunc,
							  &context);
			context.paramids = bms_add_members(context.paramids, scan_params);
			break;

		case T_ValuesScan:
			finalize_primnode((Node *) ((ValuesScan *) plan)->values_lists,
							  &context);
			context.paramids = bms_add_members(context.paramids, scan_params);
			break;

		case T_CteScan:
			{
				/*
				 * 也许会觉得应当把该节点的 cteParam 加进
				 * paramids，但不应当，因为那个参数只是
				 * 同一 CTE 的多个 CteScan 节点之间的
				 * 连接机制；它从不用作参数变化信号。必须
				 * 做的是找到被引用的 CTE 计划，并
				 * 并入它的外部 paramids，这样若该 CTE 引用外层
				 * 变量，就会发生正确的事。
				 * 见 bug #4902 的测试用例。（假定
				 * 已经对 CTE 计划跑过 SS_finalize_plan。）
				 */
				int			plan_id = ((CteScan *) plan)->ctePlanId;
				Plan	   *cteplan;

				/* 因此，做这个…… */
				if (plan_id < 1 || plan_id > list_length(root->glob->subplans))
					elog(ERROR, "could not find plan for CteScan referencing plan ID %d",
						 plan_id);
				cteplan = (Plan *) list_nth(root->glob->subplans, plan_id - 1);
				context.paramids =
					bms_add_members(context.paramids, cteplan->extParam);

#ifdef NOT_USED
				/* ……但不要做这个 */
				context.paramids =
					bms_add_member(context.paramids,
								   ((CteScan *) plan)->cteParam);
#endif

				context.paramids = bms_add_members(context.paramids,
												   scan_params);
			}
			break;

		case T_WorkTableScan:
			context.paramids =
				bms_add_member(context.paramids,
							   ((WorkTableScan *) plan)->wtParam);
			context.paramids = bms_add_members(context.paramids, scan_params);
			break;

		case T_NamedTuplestoreScan:
			context.paramids = bms_add_members(context.paramids, scan_params);
			break;

		case T_ForeignScan:
			{
				ForeignScan *fscan = (ForeignScan *) plan;

				finalize_primnode((Node *) fscan->fdw_exprs,
								  &context);
				finalize_primnode((Node *) fscan->fdw_recheck_quals,
								  &context);

				/* 假定 fdw_scan_tlist 无法含有 Param */
				context.paramids = bms_add_members(context.paramids,
												   scan_params);
			}
			break;

		case T_CustomScan:
			{
				CustomScan *cscan = (CustomScan *) plan;
				ListCell   *lc;

				finalize_primnode((Node *) cscan->custom_exprs,
								  &context);
				/* 假定 custom_scan_tlist 无法含有 Param */
				context.paramids =
					bms_add_members(context.paramids, scan_params);

				/* 若有子节点 */
				foreach(lc, cscan->custom_plans)
				{
					context.paramids =
						bms_add_members(context.paramids,
										finalize_plan(root,
													  (Plan *) lfirst(lc),
													  gather_param,
													  valid_params,
													  scan_params));
				}
			}
			break;

		case T_ModifyTable:
			{
				ModifyTable *mtplan = (ModifyTable *) plan;

				/* 强制后代扫描节点引用 epqParam */
				locally_added_param = mtplan->epqParam;
				valid_params = bms_add_member(bms_copy(valid_params),
											  locally_added_param);
				scan_params = bms_add_member(bms_copy(scan_params),
											 locally_added_param);
				finalize_primnode((Node *) mtplan->returningLists,
								  &context);
				finalize_primnode((Node *) mtplan->onConflictSet,
								  &context);
				finalize_primnode((Node *) mtplan->onConflictWhere,
								  &context);
				/* exclRelTlist 只含 Var，不必检查 */
			}
			break;

		case T_Append:
			{
				foreach(l, ((Append *) plan)->appendplans)
				{
					context.paramids =
						bms_add_members(context.paramids,
										finalize_plan(root,
													  (Plan *) lfirst(l),
													  gather_param,
													  valid_params,
													  scan_params));
				}
			}
			break;

		case T_MergeAppend:
			{
				foreach(l, ((MergeAppend *) plan)->mergeplans)
				{
					context.paramids =
						bms_add_members(context.paramids,
										finalize_plan(root,
													  (Plan *) lfirst(l),
													  gather_param,
													  valid_params,
													  scan_params));
				}
			}
			break;

		case T_BitmapAnd:
			{
				foreach(l, ((BitmapAnd *) plan)->bitmapplans)
				{
					context.paramids =
						bms_add_members(context.paramids,
										finalize_plan(root,
													  (Plan *) lfirst(l),
													  gather_param,
													  valid_params,
													  scan_params));
				}
			}
			break;

		case T_BitmapOr:
			{
				foreach(l, ((BitmapOr *) plan)->bitmapplans)
				{
					context.paramids =
						bms_add_members(context.paramids,
										finalize_plan(root,
													  (Plan *) lfirst(l),
													  gather_param,
													  valid_params,
													  scan_params));
				}
			}
			break;

		case T_NestLoop:
			{
				finalize_primnode((Node *) ((Join *) plan)->joinqual,
								  &context);
				/* 收集要传给右子节点的参数集合 */
				foreach(l, ((NestLoop *) plan)->nestParams)
				{
					NestLoopParam *nlp = (NestLoopParam *) lfirst(l);

					nestloop_params = bms_add_member(nestloop_params,
													 nlp->paramno);
				}
			}
			break;

		case T_MergeJoin:
			finalize_primnode((Node *) ((Join *) plan)->joinqual,
							  &context);
			finalize_primnode((Node *) ((MergeJoin *) plan)->mergeclauses,
							  &context);
			break;

		case T_HashJoin:
			finalize_primnode((Node *) ((Join *) plan)->joinqual,
							  &context);
			finalize_primnode((Node *) ((HashJoin *) plan)->hashclauses,
							  &context);
			break;

		case T_Hash:
			finalize_primnode((Node *) ((Hash *) plan)->hashkeys,
							  &context);
			break;

		case T_Limit:
			finalize_primnode(((Limit *) plan)->limitOffset,
							  &context);
			finalize_primnode(((Limit *) plan)->limitCount,
							  &context);
			break;

		case T_RecursiveUnion:
			/* 允许子节点引用 wtParam */
			locally_added_param = ((RecursiveUnion *) plan)->wtParam;
			valid_params = bms_add_member(bms_copy(valid_params),
										  locally_added_param);
			/* wtParam *不* 加进 scan_params */
			break;

		case T_LockRows:
			/* 强制后代扫描节点引用 epqParam */
			locally_added_param = ((LockRows *) plan)->epqParam;
			valid_params = bms_add_member(bms_copy(valid_params),
										  locally_added_param);
			scan_params = bms_add_member(bms_copy(scan_params),
										 locally_added_param);
			break;

		case T_Agg:
			{
				Agg		   *agg = (Agg *) plan;

				/*
				 * AGG_HASHED 计划需要知道聚合调用里引用了哪些 Param。
				 * 另做一次扫描来识别它们。
				 */
				if (agg->aggstrategy == AGG_HASHED)
				{
					finalize_primnode_context aggcontext;

					aggcontext.root = root;
					aggcontext.paramids = NULL;
					finalize_agg_primnode((Node *) agg->plan.targetlist,
										  &aggcontext);
					finalize_agg_primnode((Node *) agg->plan.qual,
										  &aggcontext);
					agg->aggParams = aggcontext.paramids;
				}
			}
			break;

		case T_WindowAgg:
			finalize_primnode(((WindowAgg *) plan)->startOffset,
							  &context);
			finalize_primnode(((WindowAgg *) plan)->endOffset,
							  &context);
			break;

		case T_Gather:
			/* 若有 rescan_param，允许子节点引用它 */
			locally_added_param = ((Gather *) plan)->rescan_param;
			if (locally_added_param >= 0)
			{
				valid_params = bms_add_member(bms_copy(valid_params),
											  locally_added_param);

				/*
				 * 目前不支持嵌套的 Gather。就
				 * 本函数而言，问题是如何识别
				 * 哪些子节点依赖哪个 Gather。
				 */
				Assert(gather_param < 0);
				/* 把 rescan_param 传给并行感知的子节点 */
				gather_param = locally_added_param;
			}
			/* rescan_param *不* 加进 scan_params */
			break;

		case T_GatherMerge:
			/* 若有 rescan_param，允许子节点引用它 */
			locally_added_param = ((GatherMerge *) plan)->rescan_param;
			if (locally_added_param >= 0)
			{
				valid_params = bms_add_member(bms_copy(valid_params),
											  locally_added_param);

				/*
				 * 目前不支持嵌套的 Gather。就
				 * 本函数而言，问题是如何识别
				 * 哪些子节点依赖哪个 Gather。
				 */
				Assert(gather_param < 0);
				/* 把 rescan_param 传给并行感知的子节点 */
				gather_param = locally_added_param;
			}
			/* rescan_param *不* 加进 scan_params */
			break;

		case T_Memoize:
			finalize_primnode((Node *) ((Memoize *) plan)->param_exprs,
							  &context);
			break;

		case T_ProjectSet:
		case T_Material:
		case T_Sort:
		case T_IncrementalSort:
		case T_Unique:
		case T_SetOp:
		case T_Group:
			/* 没有需要按节点类型修正的字段 */
			break;

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(plan));
	}

	/* 处理左右子计划，若有的话 */
	child_params = finalize_plan(root,
								 plan->lefttree,
								 gather_param,
								 valid_params,
								 scan_params);
	context.paramids = bms_add_members(context.paramids, child_params);

	if (nestloop_params)
	{
		/* 右子节点可以引用 nestloop_params 以及 valid_params */
		child_params = finalize_plan(root,
									 plan->righttree,
									 gather_param,
									 bms_union(nestloop_params, valid_params),
									 scan_params);
		/* ……而且它们不算我这一层使用的参数 */
		child_params = bms_difference(child_params, nestloop_params);
		bms_free(nestloop_params);
	}
	else
	{
		/* 简单情形 */
		child_params = finalize_plan(root,
									 plan->righttree,
									 gather_param,
									 valid_params,
									 scan_params);
	}
	context.paramids = bms_add_members(context.paramids, child_params);

	/*
	 * 任何本地生成的参数都不算其生成
	 * 计划节点的外部依赖。（注意：若改了 valid_params
	 * 和/或 scan_params，就会泄漏那些位图集；不值得为记法
	 * 麻烦去清理它们。）
	 */
	if (locally_added_param >= 0)
	{
		context.paramids = bms_del_member(context.paramids,
										  locally_added_param);
	}

	/* 现在有了本节点及子节点引用的全部 paramids */

	if (!bms_is_subset(context.paramids, valid_params))
		elog(ERROR, "plan should not reference subplan's variable");

	/*
	 * 计划节点的 allParam 和 extParam 字段应当包括它
	 * 引用的全部 paramids，加上任何子 initPlan 的贡献。
	 * 不过，initPlan 的任何 setParam 不应当出现在
	 * 父节点的 extParams 里，只在 allParams 里。（有可能
	 * 某些 initPlan 的 extParams 是其他 initPlan 的 setParams。）
	 */

	/* allParam 必须包括 initplan 的 extParams 和 setParams */
	plan->allParam = bms_union(context.paramids, initExtParam);
	plan->allParam = bms_add_members(plan->allParam, initSetParam);
	/* extParam 必须包括任何 initplan 的 extParams */
	plan->extParam = bms_union(context.paramids, initExtParam);
	/* 但不包括任何 initplan 的 setParams */
	plan->extParam = bms_del_members(plan->extParam, initSetParam);

	return plan->allParam;
}

/*
 * finalize_primnode：把出现（或将
 * 出现）在给定表达式树里的全部 PARAM_EXEC 参数的 ID 加进结果集。
 */
static bool
finalize_primnode(Node *node, finalize_primnode_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		if (((Param *) node)->paramkind == PARAM_EXEC)
		{
			int			paramid = ((Param *) node)->paramid;

			context->paramids = bms_add_member(context->paramids, paramid);
		}
		return false;			/* 这里没有更多要做的 */
	}
	else if (IsA(node, Aggref))
	{
		/*
		 * 检查该聚合是否会被替换成引用
		 * 子查询输出的 Param，在 setrefs.c 期间。若是，必须
		 * 在这里计入那个 Param。（由于种种原因，不便于
		 * 在 setrefs.c 之前做那次替换，也不便于
		 * 在 setrefs.c 之后做这项处理。因此这里需要一块
		 * 补丁。）
		 */
		Aggref	   *aggref = (Aggref *) node;
		Param	   *aggparam;

		aggparam = find_minmax_agg_replacement_param(context->root, aggref);
		if (aggparam != NULL)
			context->paramids = bms_add_member(context->paramids,
											   aggparam->paramid);
		/* Fall through to examine the agg's arguments */
	}
	else if (IsA(node, SubPlan))
	{
		SubPlan    *subplan = (SubPlan *) node;
		Plan	   *plan = planner_subplan_get_plan(context->root, subplan);
		ListCell   *lc;
		Bitmapset  *subparamids;

		/* 递归进入 testexpr，但不进入 Plan */
		finalize_primnode(subplan->testexpr, context);

		/*
		 * 去掉 testexpr 里引用过的、该子计划输出参数的任何参数号。
		 * 这些对
		 * 参数变化信号没意思，因为总是重新求值子计划。
		 * 注意若计划别处可能使用
		 * 相同的参数号，这就不太行，但那不会发生，因为
		 * generate_new_exec_param 从不试图合并参数。
		 */
		foreach(lc, subplan->paramIds)
		{
			context->paramids = bms_del_member(context->paramids,
											   lfirst_int(lc));
		}

		/* 也检查 args 列表 */
		finalize_primnode((Node *) subplan->args, context);

		/*
		 * 把子计划需要的参数加进 paramids，但不包括那些
		 * 将向下传给它的。（假定已经对
		 * 该子计划跑过 SS_finalize_plan。）
		 */
		subparamids = bms_copy(plan->extParam);
		foreach(lc, subplan->parParam)
		{
			subparamids = bms_del_member(subparamids, lfirst_int(lc));
		}
		context->paramids = bms_join(context->paramids, subparamids);

		return false;			/* 这里没有更多要做的 */
	}
	return expression_tree_walker(node, finalize_primnode,
								  (void *) context);
}

/*
 * finalize_agg_primnode：找出给定表达式树里的全部 Aggref 节点，
 * 并把出现在它们的聚合
 * 参数里的全部 PARAM_EXEC 参数的 ID 加进结果集。
 */
static bool
finalize_agg_primnode(Node *node, finalize_primnode_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		Aggref	   *agg = (Aggref *) node;

		/* 不应当考虑直接参数，若有的话 */
		finalize_primnode((Node *) agg->args, context);
		finalize_primnode((Node *) agg->aggfilter, context);
		return false;			/* 这里下面不可能有任何 Aggref */
	}
	return expression_tree_walker(node, finalize_agg_primnode,
								  (void *) context);
}

/*
 * SS_make_initplan_output_param - 为 initPlan 的输出做一个 Param
 *
 * 该计划预期返回给定类型/排序规则的标量值。
 *
 * 注意有些情况下 initplan 也许永远不会出现在完成的
 * 计划树里。若发生那种情况，会浪费一个 PARAM_EXEC 槽，
 * 这不是大事。
 */
Param *
SS_make_initplan_output_param(PlannerInfo *root,
							  Oid resulttype, int32 resulttypmod,
							  Oid resultcollation)
{
	return generate_new_exec_param(root, resulttype,
								   resulttypmod, resultcollation);
}

/*
 * SS_make_initplan_from_plan - 给定一棵计划树，把它做成 InitPlan
 *
 * 建立一个 EXPR_SUBLINK SubPlan 节点，并把它放进外层查询层次的 initplan
 * 列表。表示该 initplan
 * 输出的 Param 已经用 SS_make_initplan_output_param 分配好了。
 */
void
SS_make_initplan_from_plan(PlannerInfo *root,
						   PlannerInfo *subroot, Plan *plan,
						   Param *prm)
{
	SubPlan    *node;

	/*
	 * 把子计划及其 PlannerInfo 加进全局列表。
	 */
	root->glob->subplans = lappend(root->glob->subplans, plan);
	root->glob->subroots = lappend(root->glob->subroots, subroot);

	/*
	 * 创建一个 SubPlan 节点并把它加到外层 InitPlans 列表。注意
	 * 它必须出现在它可能依赖的任何其他 InitPlans 之后（见
	 * ExecReScan 里的注释）。
	 */
	node = makeNode(SubPlan);
	node->subLinkType = EXPR_SUBLINK;
	node->plan_id = list_length(root->glob->subplans);
	node->plan_name = psprintf("InitPlan %d (returns $%d)",
							   node->plan_id, prm->paramid);
	get_first_col_type(plan, &node->firstColType, &node->firstColTypmod,
					   &node->firstColCollation);
	node->setParam = list_make1_int(prm->paramid);

	root->init_plans = lappend(root->init_plans, node);

	/*
	 * 该节点不能有任何输入（因为它是 initplan），因此
	 * parParam 和 args 列表保持为空。
	 */

	/* 用计划树里的信息设置 SubPlan 的代价 */
	cost_subplan(subroot, node, plan);
}
