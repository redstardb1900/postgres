/*-------------------------------------------------------------------------
 *
 * planagg.c
 *	  聚合查询的专门规划。
 *
 * 本模块试图用子查询替换 MIN/MAX 聚合函数，
 * 其形式为
 *		(SELECT col FROM tab
 *		 WHERE col IS NOT NULL AND existing-quals
 *		 ORDER BY col ASC/DESC
 *		 LIMIT 1)
 * 若 tab.col 上有合适的索引，这可以比
 * 通用的扫描全部行的聚合计划快得多。我们可以处理多个
 * MIN/MAX 聚合，办法是生成多个子查询，而且它们的
 * 排序可以不同。不过，若查询含有任何
 * 不能这样规划的聚合，就没有意义了，因为我们反正得
 * 把所有行都扫描一遍。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planagg.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static bool can_minmax_aggs(PlannerInfo *root, List **context);
static bool build_minmax_path(PlannerInfo *root, MinMaxAggInfo *mminfo,
							  Oid eqop, Oid sortop, bool nulls_first);
static void minmax_qp_callback(PlannerInfo *root, void *extra);
static Oid	fetch_agg_sort_op(Oid aggfnoid);


/*
 * preprocess_minmax_aggregates - 预处理 MIN/MAX 聚合
 *
 * 检查查询是否含有也许能用索引扫描优化的 MIN/MAX 聚合函数。
 * 若有，并且全部聚合
 * 都有可能优化，就创建一条 MinMaxAggPath 并把它加到
 * (UPPERREL_GROUP_AGG, NULL) 上层关系。
 *
 * 这应当由 grouping_planner() 在准备调用
 * query_planner() 之前调用，因为我们靠克隆
 * 规划器的状态并对查询解析树的修改版调用 query_planner() 来生成索引扫描路径。
 * 因此，query_planner() 之前需要的全部预处理
 * 必须已经完成。这依赖 root->agginfos 里的聚合列表，
 * 所以 preprocess_aggrefs() 也必须已经调用过。
 */
void
preprocess_minmax_aggregates(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	FromExpr   *jtnode;
	RangeTblRef *rtr;
	RangeTblEntry *rte;
	List	   *aggs_list;
	RelOptInfo *grouped_rel;
	ListCell   *lc;

	/* 此时 minmax_aggs 列表应当为空 */
	Assert(root->minmax_aggs == NIL);

	/* 查询没有聚合时无事可做 */
	if (!parse->hasAggs)
		return;

	Assert(!parse->setOperations);	/* 若是集合运算，不应当到这里 */
	Assert(parse->rowMarks == NIL); /* 有 FOR UPDATE 时也不是 */

	/*
	 * 拒绝无法优化的情形。
	 *
	 * 不处理 GROUP BY 或窗口，因为目前的
	 * 分组实现反正需要查看全部行，
	 * 因此优化 MIN/MAX 意义不大。
	 */
	if (parse->groupClause || list_length(parse->groupingSets) > 1 ||
		parse->hasWindowFuncs)
		return;

	/*
	 * 若查询含有任何 CTEs 就拒绝；没法对它建索引扫描，
	 * 因此这里不会成功。（若那些 CTE 未被引用，
	 * 这话不成立，但似乎不值得花工夫去检查。）
	 */
	if (parse->cteList)
		return;

	/*
	 * 还把查询限制为恰好引用一张表，因为连接
	 * 条件没法合理地处理。（也许能处理
	 * 含有笛卡尔积连接的查询，但似乎不值得
	 * 费这个事。）不过，这一张表可能埋在好几层
	 * FromExpr 里，因为有子查询。注意这张"唯一"的表也可以是
	 * 继承父表，包括已被展平为追加关系的
	 * UNION ALL 子查询。
	 */
	jtnode = parse->jointree;
	while (IsA(jtnode, FromExpr))
	{
		if (list_length(jtnode->fromlist) != 1)
			return;
		jtnode = linitial(jtnode->fromlist);
	}
	if (!IsA(jtnode, RangeTblRef))
		return;
	rtr = (RangeTblRef *) jtnode;
	rte = planner_rt_fetch(rtr->rtindex, root);
	if (rte->rtekind == RTE_RELATION)
		 /* 普通关系，可以 */ ;
	else if (rte->rtekind == RTE_SUBQUERY && rte->inh)
		 /* 已展平的 UNION ALL 子查询，可以 */ ;
	else
		return;

	/*
	 * 检查全部聚合，确认都是 MIN/MAX 聚合。一旦
	 * 发现一个不是就停。
	 */
	aggs_list = NIL;
	if (!can_minmax_aggs(root, &aggs_list))
		return;

	/*
	 * 好，至少有可能做这项优化。
	 * 为每个聚合建立一条访问路径。若任何一个聚合
	 * 证明不能走索引，就放弃；只优化
	 * 其中一部分没有意义。
	 */
	foreach(lc, aggs_list)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);
		Oid			eqop;
		bool		reverse;

		/*
		 * 需要与该聚合的
		 * 排序操作符配套的等值操作符。
		 */
		eqop = get_equality_op_for_ordering_op(mminfo->aggsortop, &reverse);
		if (!OidIsValid(eqop))	/* 不应当发生 */
			elog(ERROR, "could not find equality operator for ordering operator %u",
				 mminfo->aggsortop);

		/*
		 * 可以用给出 NULLS FIRST 的排序，也可以用给出
		 * NULLS LAST 的排序；而且两者之间不太可能有很大
		 * 性能差别，因此若第一种就命中，似乎不值得
		 * 把两种都估价。若该操作符是
		 * 逆序操作符，NULLS FIRST 更可能可用，
		 * 因此若是逆序就先试那种。
		 */
		if (build_minmax_path(root, mminfo, eqop, mminfo->aggsortop, reverse))
			continue;
		if (build_minmax_path(root, mminfo, eqop, mminfo->aggsortop, !reverse))
			continue;

		/* 这个聚合没有可索引路径，因此失败 */
		return;
	}

	/*
	 * 好，可以这样执行查询。准备创建 MinMaxAggPath
	 * 节点。
	 *
	 * 首先，为每个聚合创建一个输出 Param 节点。（若最终不用
	 * 这条 MinMaxAggPath，每个聚合会浪费一个 PARAM_EXEC 槽，
	 * 这不值得担心。可惜不能等到 create_plan 时
	 * 再决定是否做这个 Param。）
	 */
	foreach(lc, aggs_list)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);

		mminfo->param =
			SS_make_initplan_output_param(root,
										  exprType((Node *) mminfo->target),
										  -1,
										  exprCollation((Node *) mminfo->target));
	}

	/*
	 * 创建一个带有适当估计代价和
	 * 其他所需数据的 MinMaxAggPath 节点，并把它加到 UPPERREL_GROUP_AGG 上层关系，
	 * 在那里它会与标准聚合实现竞争。（它
	 * 很可能总是赢，但这里不必假定这一点。）
	 *
	 * 注意：grouping_planner 这时还没创建这个上层关系，但
	 * 我们先创建它没问题。我们还没在里面填入正确的
	 * consider_parallel 值，但 MinMaxAggPath 路径目前
	 * 反正永远不是并行安全的，所以没关系。同样，
	 * 关系里还没填 FDW 相关字段也没关系。
	 * 另外，因为没有行标记，我们知道 processed_tlist
	 * 不必再改，因此现在做路径目标是安全的。
	 */
	grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG, NULL);
	add_path(grouped_rel, (Path *)
			 create_minmaxagg_path(root, grouped_rel,
								   create_pathtarget(root,
													 root->processed_tlist),
								   aggs_list,
								   (List *) parse->havingQual));
}

/*
 * can_minmax_aggs
 *		检查查询中的全部聚合，看它们是否
 *		都是 MIN/MAX 聚合。若是，就为它们建立一份 MinMaxAggInfo
 *		节点列表。
 *
 * 若发现非 MIN/MAX 聚合则返回假，否则返回真。
 */
static bool
can_minmax_aggs(PlannerInfo *root, List **context)
{
	ListCell   *lc;

	/*
	 * 这个函数以前得自己扫描查询，现在可以
	 * 直接翻 preprocess_aggrefs 做好的 AggInfo 列表。
	 */
	foreach(lc, root->agginfos)
	{
		AggInfo    *agginfo = lfirst_node(AggInfo, lc);
		Aggref	   *aggref = linitial_node(Aggref, agginfo->aggrefs);
		Oid			aggsortop;
		TargetEntry *curTarget;
		MinMaxAggInfo *mminfo;

		Assert(aggref->agglevelsup == 0);
		if (list_length(aggref->args) != 1)
			return false;		/* 它不可能是 MIN/MAX */

		/*
		 * ORDER BY 对 MIN/MAX 通常无关，但若聚合排序操作符的操作符类
		 * 把并不相同的
		 * 值认作相等，它就能改变结果。例如，按
		 * numeric_ops，4.0 和 4.00 相等，却可以区分。若 MIN() 收到不止一个
		 * 等于 4.0 的值，且没有小于 4.0 的值，则未指定
		 * MIN() 返回这些相等值中的哪一个。一个对参数表达式
		 * 这些相等值各不相同的 ORDER BY 表达式
		 * 能让结果再次可预测。这是
		 * 一种小众需求，我们不用子查询路径来实现它。
		 * 无论如何，这项测试让我们能迅速
		 * 拒绝有序集聚合。
		 */
		if (aggref->aggorder != NIL)
			return false;
		/* 注意：并不在意是否提到了 DISTINCT…… */

		/*
		 * 有 FILTER 子句时，也许可以把过滤器加到生成的子查询的限定条件里
		 * 来实现这项优化。
		 * 目前直接放弃。
		 */
		if (aggref->aggfilter != NULL)
			return false;

		aggsortop = fetch_agg_sort_op(aggref->aggfnoid);
		if (!OidIsValid(aggsortop))
			return false;		/* 不是 MIN/MAX 聚合 */

		curTarget = (TargetEntry *) linitial(aggref->args);

		if (contain_mutable_functions((Node *) curTarget->expr))
			return false;		/* 没有可能走索引 */

		if (type_is_rowtype(exprType((Node *) curTarget->expr)))
			return false;		/* IS NOT NULL 的语义会很怪 */

		mminfo = makeNode(MinMaxAggInfo);
		mminfo->aggfnoid = aggref->aggfnoid;
		mminfo->aggsortop = aggsortop;
		mminfo->target = curTarget->expr;
		mminfo->subroot = NULL; /* 先不算路径 */
		mminfo->path = NULL;
		mminfo->pathcost = 0;
		mminfo->param = NULL;

		*context = lappend(*context, mminfo);
	}
	return true;
}

/*
 * build_minmax_path
 *		给定一个 MIN/MAX 聚合，试着建立一条可以用来
 *		优化它的索引扫描路径。
 *
 * 若成功，把最佳路径存进 *mminfo 并返回真。
 * 否则返回假。
 */
static bool
build_minmax_path(PlannerInfo *root, MinMaxAggInfo *mminfo,
				  Oid eqop, Oid sortop, bool nulls_first)
{
	PlannerInfo *subroot;
	Query	   *parse;
	TargetEntry *tle;
	List	   *tlist;
	NullTest   *ntest;
	SortGroupClause *sortcl;
	RelOptInfo *final_rel;
	Path	   *sorted_path;
	Cost		path_cost;
	double		path_fraction;

	/*
	 * 我们将构造的实际上是一条子 SELECT 查询，因此
	 * 克隆当前查询层次的状态并调整它，使它看起来
	 * 像一个子查询。任何外部引用现在都比以前高一层。
	 * （这意味着做完之后不会有层次 1 的 Var，
	 * 所以这个子查询可以变成 initplan。）
	 */
	subroot = (PlannerInfo *) palloc(sizeof(PlannerInfo));
	memcpy(subroot, root, sizeof(PlannerInfo));
	subroot->query_level++;
	subroot->parent_root = root;
	/* 重置与子计划相关的东西 */
	subroot->plan_params = NIL;
	subroot->outer_params = NULL;
	subroot->init_plans = NIL;
	subroot->agginfos = NIL;
	subroot->aggtransinfos = NIL;

	subroot->parse = parse = copyObject(root->parse);
	IncrementVarSublevelsUp((Node *) parse, 1, 1);

	/* append_rel_list 里会不会有外部 Var？ */
	subroot->append_rel_list = copyObject(root->append_rel_list);
	IncrementVarSublevelsUp((Node *) subroot->append_rel_list, 1, 1);
	/* 目前应当还没有任何外连接信息要翻译 */
	Assert(subroot->join_info_list == NIL);
	/* 而且我们也还没做等价类 */
	Assert(subroot->eq_classes == NIL);
	/* 而且我们也还没创建 PlaceHolderInfos */
	Assert(subroot->placeholder_list == NIL);

	/*----------
	 * Generate modified query of the form
	 *		(SELECT col FROM tab
	 *		 WHERE col IS NOT NULL AND existing-quals
	 *		 ORDER BY col ASC/DESC
	 *		 LIMIT 1)
	 *----------
	 */
	/* 单个目标列表项，就是聚合目标 */
	tle = makeTargetEntry(copyObject(mminfo->target),
						  (AttrNumber) 1,
						  pstrdup("agg_target"),
						  false);
	tlist = list_make1(tle);
	subroot->processed_tlist = parse->targetList = tlist;

	/* 不再有 HAVING、DISTINCT、聚合 */
	parse->havingQual = NULL;
	subroot->hasHavingQual = false;
	parse->distinctClause = NIL;
	parse->hasDistinctOn = false;
	parse->hasAggs = false;

	/* 建立 "target IS NOT NULL" 表达式 */
	ntest = makeNode(NullTest);
	ntest->nulltesttype = IS_NOT_NULL;
	ntest->arg = copyObject(mminfo->target);
	/* 已在 can_minmax_aggs 里检查过它不是行类型 */
	ntest->argisrow = false;
	ntest->location = -1;

	/* 用户也许已经在 WHERE 里写过这个 */
	if (!list_member((List *) parse->jointree->quals, ntest))
		parse->jointree->quals = (Node *)
			lcons(ntest, (List *) parse->jointree->quals);

	/* 建立合适的 ORDER BY 子句 */
	sortcl = makeNode(SortGroupClause);
	sortcl->tleSortGroupRef = assignSortGroupRef(tle, subroot->processed_tlist);
	sortcl->eqop = eqop;
	sortcl->sortop = sortop;
	sortcl->nulls_first = nulls_first;
	sortcl->hashable = false;	/* 不必把这个做得精确 */
	parse->sortClause = list_make1(sortcl);

	/* 为 LIMIT 1 设置表达式 */
	parse->limitOffset = NULL;
	parse->limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid,
										   sizeof(int64),
										   Int64GetDatum(1), false,
										   FLOAT8PASSBYVAL);

	/*
	 * 为这个查询生成最佳路径，并告诉 query_planner 我们
	 * 有 LIMIT 1。
	 */
	subroot->tuple_fraction = 1.0;
	subroot->limit_tuples = 1.0;

	final_rel = query_planner(subroot, minmax_qp_callback, NULL);

	/*
	 * 由于没有经过 subquery_planner() 来处理这个子查询，
	 * 必须做它会做的一部分清理，尤其是应付
	 * 这个子查询内部用到的参数和 initplan。（若最终
	 * 不用这个子计划，这就无所谓。）
	 */
	SS_identify_outer_params(subroot);
	SS_charge_for_initplans(subroot, final_rel);

	/*
	 * 取得最佳的预排序路径，也就是只取
	 * 一行时最便宜的那条。若没有这样的路径，就失败。
	 */
	if (final_rel->rows > 1.0)
		path_fraction = 1.0 / final_rel->rows;
	else
		path_fraction = 1.0;

	sorted_path =
		get_cheapest_fractional_path_for_pathkeys(final_rel->pathlist,
												  subroot->query_pathkeys,
												  NULL,
												  path_fraction);
	if (!sorted_path)
		return false;

	/*
	 * 这条路径返回的也许并不正好是我们要的，因此修正它。（假定
	 * 这不会改变关于哪条是
	 * 最便宜路径的任何结论。）
	 */
	sorted_path = apply_projection_to_path(subroot, final_rel, sorted_path,
										   create_pathtarget(subroot,
															 subroot->processed_tlist));

	/*
	 * 确定只取预排序路径第一行的代价。
	 *
	 * 注意：这里的代价计算应当与
	 * compare_fractional_path_costs() 一致。
	 */
	path_cost = sorted_path->startup_cost +
		path_fraction * (sorted_path->total_cost - sorted_path->startup_cost);

	/* 保存状态供后续处理 */
	mminfo->subroot = subroot;
	mminfo->path = sorted_path;
	mminfo->pathcost = path_cost;

	return true;
}

/*
 * 在 query_planner() 期间计算 query_pathkeys 和其他路径键
 */
static void
minmax_qp_callback(PlannerInfo *root, void *extra)
{
	root->group_pathkeys = NIL;
	root->window_pathkeys = NIL;
	root->distinct_pathkeys = NIL;

	root->sort_pathkeys =
		make_pathkeys_for_sortclauses(root,
									  root->parse->sortClause,
									  root->parse->targetList);

	root->query_pathkeys = root->sort_pathkeys;
}

/*
 * 取得与某个聚合关联的排序操作符 OID，若有的话。
 * 若没有这样的操作符则返回 InvalidOid。
 */
static Oid
fetch_agg_sort_op(Oid aggfnoid)
{
	HeapTuple	aggTuple;
	Form_pg_aggregate aggform;
	Oid			aggsortop;

	/* 从 pg_aggregate 取出聚合项 */
	aggTuple = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(aggTuple))
		return InvalidOid;
	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);
	aggsortop = aggform->aggsortop;
	ReleaseSysCache(aggTuple);

	return aggsortop;
}
