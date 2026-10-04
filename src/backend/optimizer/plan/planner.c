/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  The query optimizer external interface.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planner.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "foreign/fdwapi.h"
#include "jit/jit.h"
#include "lib/bipartite_match.h"
#include "lib/knapsack.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#ifdef OPTIMIZER_DEBUG
#include "nodes/print.h"
#endif
#include "nodes/supportnodes.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/paramassign.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteManip.h"
#include "storage/dsm_impl.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

/* GUC 参数 */
double		cursor_tuple_fraction = DEFAULT_CURSOR_TUPLE_FRACTION;
int			debug_parallel_query = DEBUG_PARALLEL_OFF;
bool		parallel_leader_participation = true;

/* 供插件在 planner() 中取得控制的钩子 */
planner_hook_type planner_hook = NULL;

/* 供插件在 grouping_planner() 规划上层关系时取得控制的钩子 */
create_upper_paths_hook_type create_upper_paths_hook = NULL;


/* preprocess_expression 的表达式种类代码 */
#define EXPRKIND_QUAL				0
#define EXPRKIND_TARGET				1
#define EXPRKIND_RTFUNC				2
#define EXPRKIND_RTFUNC_LATERAL		3
#define EXPRKIND_VALUES				4
#define EXPRKIND_VALUES_LATERAL		5
#define EXPRKIND_LIMIT				6
#define EXPRKIND_APPINFO			7
#define EXPRKIND_PHV				8
#define EXPRKIND_TABLESAMPLE		9
#define EXPRKIND_ARBITER_ELEM		10
#define EXPRKIND_TABLEFUNC			11
#define EXPRKIND_TABLEFUNC_LATERAL	12

/*
 * 分组集专用的数据
 */
typedef struct
{
	List	   *rollups;
	List	   *hash_sets_idx;
	double		dNumHashGroups;
	bool		any_hashable;
	Bitmapset  *unsortable_refs;
	Bitmapset  *unhashable_refs;
	List	   *unsortable_sets;
	int		   *tleref_to_colnum_map;
} grouping_sets_data;

/*
 * WindowClause 重排期间使用的临时结构，以便
 * 按分区/排序前缀对 WindowClauses 排序。
 */
typedef struct
{
	WindowClause *wc;
	List	   *uniqueOrder;	/* 每个 Window 一份唯一的排序/分区
								 * 子句 List */
} WindowClauseSortData;

/* standard_qp_callback 的透传数据 */
typedef struct
{
	List	   *activeWindows;	/* 活动窗口，若有 */
	grouping_sets_data *gset_data;	/* 分组集数据，若有 */
} standard_qp_extra;

/* 局部函数 */
static Node *preprocess_expression(PlannerInfo *root, Node *expr, int kind);
static void preprocess_qual_conditions(PlannerInfo *root, Node *jtnode);
static void grouping_planner(PlannerInfo *root, double tuple_fraction);
static grouping_sets_data *preprocess_grouping_sets(PlannerInfo *root);
static List *remap_to_groupclause_idx(List *groupClause, List *gsets,
									  int *tleref_to_colnum_map);
static void preprocess_rowmarks(PlannerInfo *root);
static double preprocess_limit(PlannerInfo *root,
							   double tuple_fraction,
							   int64 *offset_est, int64 *count_est);
static void remove_useless_groupby_columns(PlannerInfo *root);
static List *preprocess_groupclause(PlannerInfo *root, List *force);
static List *extract_rollup_sets(List *groupingSets);
static List *reorder_grouping_sets(List *groupingSets, List *sortclause);
static void standard_qp_callback(PlannerInfo *root, void *extra);
static double get_number_of_groups(PlannerInfo *root,
								   double path_rows,
								   grouping_sets_data *gd,
								   List *target_list);
static RelOptInfo *create_grouping_paths(PlannerInfo *root,
										 RelOptInfo *input_rel,
										 PathTarget *target,
										 bool target_parallel_safe,
										 grouping_sets_data *gd);
static bool is_degenerate_grouping(PlannerInfo *root);
static void create_degenerate_grouping_paths(PlannerInfo *root,
											 RelOptInfo *input_rel,
											 RelOptInfo *grouped_rel);
static RelOptInfo *make_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
									 PathTarget *target, bool target_parallel_safe,
									 Node *havingQual);
static void create_ordinary_grouping_paths(PlannerInfo *root,
										   RelOptInfo *input_rel,
										   RelOptInfo *grouped_rel,
										   const AggClauseCosts *agg_costs,
										   grouping_sets_data *gd,
										   GroupPathExtraData *extra,
										   RelOptInfo **partially_grouped_rel_p);
static void consider_groupingsets_paths(PlannerInfo *root,
										RelOptInfo *grouped_rel,
										Path *path,
										bool is_sorted,
										bool can_hash,
										grouping_sets_data *gd,
										const AggClauseCosts *agg_costs,
										double dNumGroups);
static RelOptInfo *create_window_paths(PlannerInfo *root,
									   RelOptInfo *input_rel,
									   PathTarget *input_target,
									   PathTarget *output_target,
									   bool output_target_parallel_safe,
									   WindowFuncLists *wflists,
									   List *activeWindows);
static void create_one_window_path(PlannerInfo *root,
								   RelOptInfo *window_rel,
								   Path *path,
								   PathTarget *input_target,
								   PathTarget *output_target,
								   WindowFuncLists *wflists,
								   List *activeWindows);
static RelOptInfo *create_distinct_paths(PlannerInfo *root,
										 RelOptInfo *input_rel);
static void create_partial_distinct_paths(PlannerInfo *root,
										  RelOptInfo *input_rel,
										  RelOptInfo *final_distinct_rel);
static RelOptInfo *create_final_distinct_paths(PlannerInfo *root,
											   RelOptInfo *input_rel,
											   RelOptInfo *distinct_rel);
static RelOptInfo *create_ordered_paths(PlannerInfo *root,
										RelOptInfo *input_rel,
										PathTarget *target,
										bool target_parallel_safe,
										double limit_tuples);
static PathTarget *make_group_input_target(PlannerInfo *root,
										   PathTarget *final_target);
static PathTarget *make_partial_grouping_target(PlannerInfo *root,
												PathTarget *grouping_target,
												Node *havingQual);
static List *postprocess_setop_tlist(List *new_tlist, List *orig_tlist);
static void optimize_window_clauses(PlannerInfo *root,
									WindowFuncLists *wflists);
static List *select_active_windows(PlannerInfo *root, WindowFuncLists *wflists);
static PathTarget *make_window_input_target(PlannerInfo *root,
											PathTarget *final_target,
											List *activeWindows);
static List *make_pathkeys_for_window(PlannerInfo *root, WindowClause *wc,
									  List *tlist);
static PathTarget *make_sort_input_target(PlannerInfo *root,
										  PathTarget *final_target,
										  bool *have_postponed_srfs);
static void adjust_paths_for_srfs(PlannerInfo *root, RelOptInfo *rel,
								  List *targets, List *targets_contain_srfs);
static void add_paths_to_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
									  RelOptInfo *grouped_rel,
									  RelOptInfo *partially_grouped_rel,
									  const AggClauseCosts *agg_costs,
									  grouping_sets_data *gd,
									  double dNumGroups,
									  GroupPathExtraData *extra);
static RelOptInfo *create_partial_grouping_paths(PlannerInfo *root,
												 RelOptInfo *grouped_rel,
												 RelOptInfo *input_rel,
												 grouping_sets_data *gd,
												 GroupPathExtraData *extra,
												 bool force_rel_creation);
static void gather_grouping_paths(PlannerInfo *root, RelOptInfo *rel);
static bool can_partial_agg(PlannerInfo *root);
static void apply_scanjoin_target_to_paths(PlannerInfo *root,
										   RelOptInfo *rel,
										   List *scanjoin_targets,
										   List *scanjoin_targets_contain_srfs,
										   bool scanjoin_target_parallel_safe,
										   bool tlist_same_exprs);
static void create_partitionwise_grouping_paths(PlannerInfo *root,
												RelOptInfo *input_rel,
												RelOptInfo *grouped_rel,
												RelOptInfo *partially_grouped_rel,
												const AggClauseCosts *agg_costs,
												grouping_sets_data *gd,
												PartitionwiseAggregateType patype,
												GroupPathExtraData *extra);
static bool group_by_has_partkey(RelOptInfo *input_rel,
								 List *targetList,
								 List *groupClause);
static int	common_prefix_cmp(const void *a, const void *b);


/*****************************************************************************
 *
 *	   查询规划入口
 *
 * 为了支持监视或修改规划器行为的可加载插件，
 * 我们提供一个钩子变量，让插件在标准规划过程之前和
 * 之后取得控制。插件通常会调用
 * standard_planner()。
 *
 * 给插件作者的说明：standard_planner() 会就地修改其 Query 输入，
 * 因此若想规划不止一次，最好复制那份数据结构。
 *
 *****************************************************************************/
PlannedStmt *
planner(Query *parse, const char *query_string, int cursorOptions,
		ParamListInfo boundParams)
{
	PlannedStmt *result;

	if (planner_hook)
		result = (*planner_hook) (parse, query_string, cursorOptions, boundParams);
	else
		result = standard_planner(parse, query_string, cursorOptions, boundParams);
	return result;
}

PlannedStmt *
standard_planner(Query *parse, const char *query_string, int cursorOptions,
				 ParamListInfo boundParams)
{
	PlannedStmt *result;
	PlannerGlobal *glob;
	double		tuple_fraction;
	PlannerInfo *root;
	RelOptInfo *final_rel;
	Path	   *best_path;
	Plan	   *top_plan;
	ListCell   *lp,
			   *lr;

	/*
	 * 为这次规划器调用建立全局状态。这些数据在
	 * 给定命令里可能存在的全部子 Query 层次都需要，
	 * 因此把它放在单独的结构体里，由每个按 Query 的
	 * PlannerInfo 链接。
	 */
	glob = makeNode(PlannerGlobal);

	glob->boundParams = boundParams;
	glob->subplans = NIL;
	glob->subroots = NIL;
	glob->rewindPlanIDs = NULL;
	glob->finalrtable = NIL;
	glob->finalrteperminfos = NIL;
	glob->finalrowmarks = NIL;
	glob->resultRelations = NIL;
	glob->appendRelations = NIL;
	glob->relationOids = NIL;
	glob->invalItems = NIL;
	glob->paramExecTypes = NIL;
	glob->lastPHId = 0;
	glob->lastRowMarkId = 0;
	glob->lastPlanNodeId = 0;
	glob->transientPlan = false;
	glob->dependsOnRole = false;

	/*
	 * 评估对本查询使用并行模式是否可行。在独立后端里
	 * 不能这么做，或者命令会试图
	 * 修改任何数据，或者这是游标操作，或者 GUCs 被设成
	 * 不允许并行的值，或者查询树里存在并行不安全的
	 * 函数。
	 *
	 * （注意我们确实允许 CREATE TABLE AS、SELECT INTO 和 CREATE
	 * MATERIALIZED VIEW 使用并行计划，但这安全只是因为
	 * 该命令写入一张全新的表，工作进程将
	 * 看不见它。若工作进程能看见该表，组
	 * 锁会使它们忽略首领的重量级
	 * GIN 页锁，从而不安全。若想一般地
	 * 允许并行插入，得想办法修好这一点；更新和删除
	 * 还有额外问题，尤其是组合 CIDs。）
	 *
	 * 目前，若正在并行工作进程内部运行，就不尝试使用并行模式。
	 * 也许最终能放宽这个
	 * 限制，但目前最好不要让并行工作进程
	 * 再去创建自己的并行工作进程。
	 */
	if ((cursorOptions & CURSOR_OPT_PARALLEL_OK) != 0 &&
		IsUnderPostmaster &&
		parse->commandType == CMD_SELECT &&
		!parse->hasModifyingCTE &&
		max_parallel_workers_per_gather > 0 &&
		!IsParallelWorker())
	{
		/* 便宜的测试都通过了，因此扫描查询树 */
		glob->maxParallelHazard = max_parallel_hazard(parse);
		glob->parallelModeOK = (glob->maxParallelHazard != PROPARALLEL_UNSAFE);
	}
	else
	{
		/* 跳过查询树扫描，直接假定它不安全 */
		glob->maxParallelHazard = PROPARALLEL_UNSAFE;
		glob->parallelModeOK = false;
	}

	/*
	 * glob->parallelModeNeeded 通常在这里设为假，若实际
	 * 建立了 Gather 或 Gather Merge 计划，则在计划建立期间改成
	 * 真（参见 create_gather_plan、create_gather_merge_plan）。
	 *
	 * 不过，若 debug_parallel_query = on 或 debug_parallel_query =
	 * regress，则只要安全就强制并行模式，即使
	 * 最终计划并不使用并行。若
	 * 查询含有任何并行不安全的东西，这么做就不安全；那种情况下 parallelModeOK 将为
	 * 假。注意此后 parallelModeOK 不能再变。
	 * 否则，查询里的一切要么并行安全，要么
	 * 并行受限，无论哪种，强制
	 * 并行模式限制都应当可以。若那最终弄坏了什么，则要么
	 * 用户放进查询的某个函数被错误地
	 * 标成并行安全或并行受限，而实际上它是
	 * 并行不安全的，要么就是查询规划器本身有 bug。
	 */
	glob->parallelModeNeeded = glob->parallelModeOK &&
		(debug_parallel_query != DEBUG_PARALLEL_OFF);

	/* 判断计划中有多大一部分可能被扫描 */
	if (cursorOptions & CURSOR_OPT_FAST_PLAN)
	{
		/*
		 * 我们并不真的知道用户最终会从游标 FETCH
		 * 多少元组，但常常他并不想要全部，
		 * 或者宁可要一个快速启动的计划，以便更早
		 * 处理其中一些元组。用一个 GUC 参数来决定
		 * 为多大比例做优化。
		 */
		tuple_fraction = cursor_tuple_fraction;

		/*
		 * 我们把 cursor_tuple_fraction 只文档化成一个分数，这意味着
		 * 边界情形 0 和 1 在这里必须特殊处理。我们把
		 * 1 转换成 0（"全部元组"），把 0 转换成一个非常小的分数。
		 */
		if (tuple_fraction >= 1.0)
			tuple_fraction = 0.0;
		else if (tuple_fraction <= 0.0)
			tuple_fraction = 1e-10;
	}
	else
	{
		/* 默认假定我们需要全部元组 */
		tuple_fraction = 0.0;
	}

	/* 核心规划入口（子查询可能会递归） */
	root = subquery_planner(glob, parse, NULL,
							false, tuple_fraction);

	/* 选出最佳 Path 并把它变成 Plan */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);
	best_path = get_cheapest_fractional_path(final_rel, tuple_fraction);

	top_plan = create_plan(root, best_path);

	/*
	 * 若为可滚动游标建立计划，确保它能按需
	 * 反向运行。必要时在顶部加一个 Material 节点。
	 */
	if (cursorOptions & CURSOR_OPT_SCROLL)
	{
		if (!ExecSupportsBackwardScan(top_plan))
			top_plan = materialize_finished_plan(top_plan);
	}

	/*
	 * 可选地为测试目的加一个 Gather 节点，前提是这么做
	 * 实际上是安全的。
	 */
	if (debug_parallel_query != DEBUG_PARALLEL_OFF && top_plan->parallel_safe)
	{
		Gather	   *gather = makeNode(Gather);

		/*
		 * 顶层计划不得有任何 initPlan，否则它不应当被
		 * 标成并行安全。
		 */
		Assert(top_plan->initPlan == NIL);

		gather->plan.targetlist = top_plan->targetlist;
		gather->plan.qual = NIL;
		gather->plan.lefttree = top_plan;
		gather->plan.righttree = NULL;
		gather->num_workers = 1;
		gather->single_copy = true;
		gather->invisible = (debug_parallel_query == DEBUG_PARALLEL_REGRESS);

		/*
		 * 由于这个 Gather 没有要发信号的并行感知后代，
		 * 不需要 rescan Param。
		 */
		gather->rescan_param = -1;

		/*
		 * 理想情况下这里会用 cost_gather，但建立哑路径数据
		 * 来满足它，似乎并不比知道它做什么更干净。
		 */
		gather->plan.startup_cost = top_plan->startup_cost +
			parallel_setup_cost;
		gather->plan.total_cost = top_plan->total_cost +
			parallel_setup_cost + parallel_tuple_cost * top_plan->plan_rows;
		gather->plan.plan_rows = top_plan->plan_rows;
		gather->plan.plan_width = top_plan->plan_width;
		gather->plan.parallel_aware = false;
		gather->plan.parallel_safe = false;

		/* 并行计划使用并行模式。 */
		root->glob->parallelModeNeeded = true;

		top_plan = &gather->plan;
	}

	/*
	 * 若生成了任何 Param，就走过计划树并计算
	 * 每个计划节点的 extParam/allParam 集合。理想情况下会把这并进
	 * set_plan_references 的树遍历，但目前必须分开，
	 * 因为需要先访问子计划，而不是在主计划之后。
	 */
	if (glob->paramExecTypes != NIL)
	{
		Assert(list_length(glob->subplans) == list_length(glob->subroots));
		forboth(lp, glob->subplans, lr, glob->subroots)
		{
			Plan	   *subplan = (Plan *) lfirst(lp);
			PlannerInfo *subroot = lfirst_node(PlannerInfo, lr);

			SS_finalize_plan(subroot, subplan);
		}
		SS_finalize_plan(root, top_plan);
	}

	/* 计划的最终清理 */
	Assert(glob->finalrtable == NIL);
	Assert(glob->finalrteperminfos == NIL);
	Assert(glob->finalrowmarks == NIL);
	Assert(glob->resultRelations == NIL);
	Assert(glob->appendRelations == NIL);
	top_plan = set_plan_references(root, top_plan);
	/* ……以及子计划（普通子计划和 initplan） */
	Assert(list_length(glob->subplans) == list_length(glob->subroots));
	forboth(lp, glob->subplans, lr, glob->subroots)
	{
		Plan	   *subplan = (Plan *) lfirst(lp);
		PlannerInfo *subroot = lfirst_node(PlannerInfo, lr);

		lfirst(lp) = set_plan_references(subroot, subplan);
	}

	/* 建立 PlannedStmt 结果 */
	result = makeNode(PlannedStmt);

	result->commandType = parse->commandType;
	result->queryId = parse->queryId;
	result->hasReturning = (parse->returningList != NIL);
	result->hasModifyingCTE = parse->hasModifyingCTE;
	result->canSetTag = parse->canSetTag;
	result->transientPlan = glob->transientPlan;
	result->dependsOnRole = glob->dependsOnRole;
	result->parallelModeNeeded = glob->parallelModeNeeded;
	result->planTree = top_plan;
	result->rtable = glob->finalrtable;
	result->permInfos = glob->finalrteperminfos;
	result->resultRelations = glob->resultRelations;
	result->appendRelations = glob->appendRelations;
	result->subplans = glob->subplans;
	result->rewindPlanIDs = glob->rewindPlanIDs;
	result->rowMarks = glob->finalrowmarks;
	result->relationOids = glob->relationOids;
	result->invalItems = glob->invalItems;
	result->paramExecTypes = glob->paramExecTypes;
	/* utilityStmt 应当为 null，但不妨复制它 */
	result->utilityStmt = parse->utilityStmt;
	result->stmt_location = parse->stmt_location;
	result->stmt_len = parse->stmt_len;

	result->jitFlags = PGJIT_NONE;
	if (jit_enabled && jit_above_cost >= 0 &&
		top_plan->total_cost > jit_above_cost)
	{
		result->jitFlags |= PGJIT_PERFORM;

		/*
		 * 决定应当为生成更好的代码投入多少精力。
		 */
		if (jit_optimize_above_cost >= 0 &&
			top_plan->total_cost > jit_optimize_above_cost)
			result->jitFlags |= PGJIT_OPT3;
		if (jit_inline_above_cost >= 0 &&
			top_plan->total_cost > jit_inline_above_cost)
			result->jitFlags |= PGJIT_INLINE;

		/*
		 * 决定哪些操作应当被 JITed。
		 */
		if (jit_expressions)
			result->jitFlags |= PGJIT_EXPR;
		if (jit_tuple_deforming)
			result->jitFlags |= PGJIT_DEFORM;
	}

	if (glob->partition_directory != NULL)
		DestroyPartitionDirectory(glob->partition_directory);

	return result;
}


/*--------------------
 * subquery_planner
 *	  Invokes the planner on a subquery.  We recurse to here for each
 *	  sub-SELECT found in the query tree.
 *
 * glob is the global state for the current planner run.
 * parse is the querytree produced by the parser & rewriter.
 * parent_root is the immediate parent Query's info (NULL at the top level).
 * hasRecursion is true if this is a recursive WITH query.
 * tuple_fraction is the fraction of tuples we expect will be retrieved.
 * tuple_fraction is interpreted as explained for grouping_planner, below.
 *
 * Basically, this routine does the stuff that should only be done once
 * per Query object.  It then calls grouping_planner.  At one time,
 * grouping_planner could be invoked recursively on the same Query object;
 * that's not currently true, but we keep the separation between the two
 * routines anyway, in case we need it again someday.
 *
 * subquery_planner will be called recursively to handle sub-Query nodes
 * found within the query's expressions and rangetable.
 *
 * Returns the PlannerInfo struct ("root") that contains all data generated
 * while planning the subquery.  In particular, the Path(s) attached to
 * the (UPPERREL_FINAL, NULL) upperrel represent our conclusions about the
 * cheapest way(s) to implement the query.  The top level will select the
 * best Path and pass it through createplan.c to produce a finished Plan.
 *--------------------
 */
PlannerInfo *
subquery_planner(PlannerGlobal *glob, Query *parse,
				 PlannerInfo *parent_root,
				 bool hasRecursion, double tuple_fraction)
{
	PlannerInfo *root;
	List	   *newWithCheckOptions;
	List	   *newHaving;
	bool		hasOuterJoins;
	bool		hasResultRTEs;
	RelOptInfo *final_rel;
	ListCell   *l;

	/* 为本子查询建立 PlannerInfo 数据结构 */
	root = makeNode(PlannerInfo);
	root->parse = parse;
	root->glob = glob;
	root->query_level = parent_root ? parent_root->query_level + 1 : 1;
	root->parent_root = parent_root;
	root->plan_params = NIL;
	root->outer_params = NULL;
	root->planner_cxt = CurrentMemoryContext;
	root->init_plans = NIL;
	root->cte_plan_ids = NIL;
	root->multiexpr_params = NIL;
	root->join_domains = NIL;
	root->eq_classes = NIL;
	root->ec_merging_done = false;
	root->last_rinfo_serial = 0;
	root->all_result_relids =
		parse->resultRelation ? bms_make_singleton(parse->resultRelation) : NULL;
	root->leaf_result_relids = NULL;	/* 叶子与否以后再弄清 */
	root->append_rel_list = NIL;
	root->row_identity_vars = NIL;
	root->rowMarks = NIL;
	memset(root->upper_rels, 0, sizeof(root->upper_rels));
	memset(root->upper_targets, 0, sizeof(root->upper_targets));
	root->processed_groupClause = NIL;
	root->processed_distinctClause = NIL;
	root->processed_tlist = NIL;
	root->update_colnos = NIL;
	root->grouping_map = NULL;
	root->minmax_aggs = NIL;
	root->qual_security_level = 0;
	root->hasPseudoConstantQuals = false;
	root->hasAlternativeSubPlans = false;
	root->placeholdersFrozen = false;
	root->hasRecursion = hasRecursion;
	if (hasRecursion)
		root->wt_param_id = assign_special_exec_param(root);
	else
		root->wt_param_id = -1;
	root->non_recursive_path = NULL;
	root->partColsUpdated = false;

	/*
	 * 建立顶层连接域。在
	 * deconstruct_jointree 填入之前，它没有有效内容，但该节点必须先存在，
	 * 才能建立引用它的 EquivalenceClasses。
	 */
	root->join_domains = list_make1(makeNode(JoinDomain));

	/*
	 * 若有 WITH 列表，就处理每个 WITH 查询，要么把它转换成
	 * RTE_SUBQUERY 的 RTE，要么为它建立一个 initplan SubPlan 结构。
	 */
	if (parse->cteList)
		SS_process_ctes(root);

	/*
	 * 若是 MERGE 命令，就适当地变换连接列表。
	 */
	transform_MERGE_to_join(parse);

	/*
	 * 若 FROM 子句为空，就用一个哑 RTE_RESULT RTE 替换它，这样
	 * 就不必为那种情形准备那么多特殊情况。
	 */
	replace_empty_jointree(parse);

	/*
	 * 在 WHERE 和 JOIN/ON 子句里找 ANY 和 EXISTS SubLinks，并试着
	 * 把它们变换成连接。注意这一步不下降
	 * 进子查询；若下面上拉了任何子查询，它们的 SubLinks 会在
	 * 上拉之前刚刚被处理。
	 */
	if (parse->hasSubLinks)
		pull_up_sublinks(root);

	/*
	 * 扫描范围表里的函数 RTEs，对它们做常量简化，
	 * 然后尽可能内联（产生也许会接着被
	 * 上拉的子查询）。这里的递归问题与
	 * SubLinks 用同样的方式处理。
	 */
	preprocess_function_rtes(root);

	/*
	 * 检查连接树里是否有任何子查询可以并进本
	 * 查询。
	 */
	pull_up_subqueries(root);

	/*
	 * 若这是简单的 UNION ALL 查询，就把它展平成 appendrel。现在
	 * 做这件事，是因为它需要把 pull_up_subqueries 应用到该 UNION ALL 的叶子
	 * 查询，上面没碰它们，因为它们
	 * 没被连接树引用（做完这件事之后就会被引用）。
	 */
	if (parse->setOperations)
		flatten_simple_union_all(root);

	/*
	 * 查看范围表，看有哪些种类的项。若没用到相关 SQL 特性，
	 * 就可以跳过后面一些处理；例如
	 * 若没有 JOIN RTEs，就可以避免做
	 * flatten_join_alias_vars() 的开销。当然必须在完成
	 * 添加范围表项之后做。（注意：处理
	 * 继承或分区关系确实可能使其子表的 RTE
	 * 以后才被加入；但那些都必须是 RTE_RELATION 项，因此
	 * 不会使这里得出的结论失效。）
	 */
	root->hasJoinRTEs = false;
	root->hasLateralRTEs = false;
	hasOuterJoins = false;
	hasResultRTEs = false;
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

		switch (rte->rtekind)
		{
			case RTE_RELATION:
				if (rte->inh)
				{
					/*
					 * 也可以检查该关系是否真的有任何子项；
					 * 若没有，就清掉 inh 标志，这样可以把它当作
					 * 普通基关系。
					 *
					 * 注意：若该关系曾经有子项但现在没有了，这可能给出假阳性。
					 * 以前发现
					 * 这一点时还能以后再清 rte->inh，
					 * 但现在不行了；必须把这类情形当作
					 * 完整的继承来处理。
					 */
					rte->inh = has_subclass(rte->relid);
				}
				break;
			case RTE_JOIN:
				root->hasJoinRTEs = true;
				if (IS_OUTER_JOIN(rte->jointype))
					hasOuterJoins = true;
				break;
			case RTE_RESULT:
				hasResultRTEs = true;
				break;
			default:
				/* 其他 RTE 类型这里无事可做 */
				break;
		}

		if (rte->lateral)
			root->hasLateralRTEs = true;

		/*
		 * 现在也可以确定任何 securityQuals 所需的最高安全级别。
		 * 加入继承子 RTEs 不会影响
		 * 这一点，因为子表没有自己的 securityQuals；见
		 * expand_single_inheritance_child()。
		 */
		if (rte->securityQuals)
			root->qual_security_level = Max(root->qual_security_level,
											list_length(rte->securityQuals));
	}

	/*
	 * 若现在已证实查询目标关系
	 * 不继承，就把它标成叶子目标。
	 */
	if (parse->resultRelation)
	{
		RangeTblEntry *rte = rt_fetch(parse->resultRelation, parse->rtable);

		if (!rte->inh)
			root->leaf_result_relids =
				bms_make_singleton(parse->resultRelation);
	}

	/*
	 * 现在检查查询里提到的全部关系的访问权限会很方便，
	 * 因为最好现在就失败，
	 * 在做任何细致规划之前。不过出于历史原因，
	 * 我们把这件事留到执行器启动时做。
	 *
	 * 但注意，必须检查查询里提到的任何视图
	 * 关系的访问权限，以防止选择率估计函数泄漏信息，
	 * 那些函数只检查底层表上的视图所有者
	 * 权限（见 all_rows_selectable() 及其
	 * 调用方）。这有点难看，因为它意味着视图的访问
	 * 权限会被检查两次，这也是为什么
	 * 最好在这里做全部 ACL 检查的另一个原因。
	 */
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

		if (rte->perminfoindex != 0 &&
			rte->relkind == RELKIND_VIEW)
		{
			RTEPermissionInfo *perminfo;
			bool		result;

			perminfo = getRTEPermissionInfo(parse->rteperminfos, rte);
			result = ExecCheckOneRelPerms(perminfo);
			if (!result)
				aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_VIEW,
							   get_rel_name(perminfo->relid));
		}
	}

	/*
	 * 预处理 RowMark 信息。必须在子查询
	 * 上拉之后做，这样全部基关系都已在场。
	 */
	preprocess_rowmarks(root);

	/*
	 * 设置 hasHavingQual，记住是否存在 HAVING 子句。需要这样做，
	 * 因为 preprocess_expression 会把常量真条件收成
	 * 空的限定列表……但 "HAVING TRUE" 在语义上并不是空操作。
	 */
	root->hasHavingQual = (parse->havingQual != NULL);

	/*
	 * 对目标列表和限定条件，以及查询树里其他
	 * 零散表达式做表达式预处理。注意不必
	 * 显式处理排序/分组表达式，因为它们实际上
	 * 是目标列表的一部分。
	 */
	parse->targetList = (List *)
		preprocess_expression(root, (Node *) parse->targetList,
							  EXPRKIND_TARGET);

	/* 常量折叠也许已经去掉了全部返回集合的函数 */
	if (parse->hasTargetSRFs)
		parse->hasTargetSRFs = expression_returns_set((Node *) parse->targetList);

	newWithCheckOptions = NIL;
	foreach(l, parse->withCheckOptions)
	{
		WithCheckOption *wco = lfirst_node(WithCheckOption, l);

		wco->qual = preprocess_expression(root, wco->qual,
										  EXPRKIND_QUAL);
		if (wco->qual != NULL)
			newWithCheckOptions = lappend(newWithCheckOptions, wco);
	}
	parse->withCheckOptions = newWithCheckOptions;

	parse->returningList = (List *)
		preprocess_expression(root, (Node *) parse->returningList,
							  EXPRKIND_TARGET);

	preprocess_qual_conditions(root, (Node *) parse->jointree);

	parse->havingQual = preprocess_expression(root, parse->havingQual,
											  EXPRKIND_QUAL);

	foreach(l, parse->windowClause)
	{
		WindowClause *wc = lfirst_node(WindowClause, l);

		/* partitionClause/orderClause 是排序/分组表达式 */
		wc->startOffset = preprocess_expression(root, wc->startOffset,
												EXPRKIND_LIMIT);
		wc->endOffset = preprocess_expression(root, wc->endOffset,
											  EXPRKIND_LIMIT);
		wc->runCondition = (List *) preprocess_expression(root,
														  (Node *) wc->runCondition,
														  EXPRKIND_TARGET);
	}

	parse->limitOffset = preprocess_expression(root, parse->limitOffset,
											   EXPRKIND_LIMIT);
	parse->limitCount = preprocess_expression(root, parse->limitCount,
											  EXPRKIND_LIMIT);

	if (parse->onConflict)
	{
		parse->onConflict->arbiterElems = (List *)
			preprocess_expression(root,
								  (Node *) parse->onConflict->arbiterElems,
								  EXPRKIND_ARBITER_ELEM);
		parse->onConflict->arbiterWhere =
			preprocess_expression(root,
								  parse->onConflict->arbiterWhere,
								  EXPRKIND_QUAL);
		parse->onConflict->onConflictSet = (List *)
			preprocess_expression(root,
								  (Node *) parse->onConflict->onConflictSet,
								  EXPRKIND_TARGET);
		parse->onConflict->onConflictWhere =
			preprocess_expression(root,
								  parse->onConflict->onConflictWhere,
								  EXPRKIND_QUAL);
		/* exclRelTlist 只含 Var，因此不必预处理 */
	}

	foreach(l, parse->mergeActionList)
	{
		MergeAction *action = (MergeAction *) lfirst(l);

		action->targetList = (List *)
			preprocess_expression(root,
								  (Node *) action->targetList,
								  EXPRKIND_TARGET);
		action->qual =
			preprocess_expression(root,
								  (Node *) action->qual,
								  EXPRKIND_QUAL);
	}

	root->append_rel_list = (List *)
		preprocess_expression(root, (Node *) root->append_rel_list,
							  EXPRKIND_APPINFO);

	/* 也需要预处理 RTEs 内部的表达式 */
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);
		int			kind;
		ListCell   *lcsq;

		if (rte->rtekind == RTE_RELATION)
		{
			if (rte->tablesample)
				rte->tablesample = (TableSampleClause *)
					preprocess_expression(root,
										  (Node *) rte->tablesample,
										  EXPRKIND_TABLESAMPLE);
		}
		else if (rte->rtekind == RTE_SUBQUERY)
		{
			/*
			 * 还不想对子查询的表达式做全部预处理，
			 * 因为规划它时会发生。但若它
			 * 含有本层次的任何连接别名，那些必须
			 * 现在就展开，因为规划该子查询时不会做。
			 * 只有该子查询是 LATERAL 时才可能。
			 */
			if (rte->lateral && root->hasJoinRTEs)
				rte->subquery = (Query *)
					flatten_join_alias_vars(root, root->parse,
											(Node *) rte->subquery);
		}
		else if (rte->rtekind == RTE_FUNCTION)
		{
			/* 完整预处理函数表达式 */
			kind = rte->lateral ? EXPRKIND_RTFUNC_LATERAL : EXPRKIND_RTFUNC;
			rte->functions = (List *)
				preprocess_expression(root, (Node *) rte->functions, kind);
		}
		else if (rte->rtekind == RTE_TABLEFUNC)
		{
			/* 完整预处理函数表达式 */
			kind = rte->lateral ? EXPRKIND_TABLEFUNC_LATERAL : EXPRKIND_TABLEFUNC;
			rte->tablefunc = (TableFunc *)
				preprocess_expression(root, (Node *) rte->tablefunc, kind);
		}
		else if (rte->rtekind == RTE_VALUES)
		{
			/* 完整预处理 values 列表 */
			kind = rte->lateral ? EXPRKIND_VALUES_LATERAL : EXPRKIND_VALUES;
			rte->values_lists = (List *)
				preprocess_expression(root, (Node *) rte->values_lists, kind);
		}

		/*
		 * 把 securityQuals 列表的每个元素当作
		 * 单独的限定表达式来处理（它确实是）。必须这样做，
		 * 才能正确规范化 AND/OR 结构。注意
		 * 这会把每个元素转换成隐式 AND 子列表。
		 */
		foreach(lcsq, rte->securityQuals)
		{
			lfirst(lcsq) = preprocess_expression(root,
												 (Node *) lfirst(lcsq),
												 EXPRKIND_QUAL);
		}
	}

	/*
	 * 既然已经做完表达式预处理，尤其是做完了
	 * 连接别名变量的展平，就丢掉 joinaliasvars 列表。
	 * 它们不再与树其余部分的表达式看起来
	 * 一样，因为我们没有预处理那些列表里的表达式（而且
	 * 也不想；例如在那里展开 SubLink 会产生
	 * 一个无用的、未被引用的子计划）。留着它们只会给
	 * 以后的树扫描制造危险。可以试着用
	 * 此后每次树扫描都带 QTW_IGNORE_JOINALIASES 来防止，
	 * 但那听起来不太可靠。
	 */
	if (root->hasJoinRTEs)
	{
		foreach(l, parse->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

			rte->joinaliasvars = NIL;
		}
	}

	/*
	 * 有些情况下也许想把 HAVING 子句转入 WHERE。
	 * 若 HAVING 子句含有聚合（显然）或
	 * 易变函数（因为 HAVING 子句应当每个组只执行
	 * 一次），就无法这么做。若有任何非空的
	 * 分组集，也无法这么做；把这样的子句移进 WHERE 可能会改变
	 * 结果，若任何被引用的列并不出现在全部分组
	 * 集里。（若只有空分组集，则 HAVING 子句
	 * 必定是下面讨论的那种退化。）
	 *
	 * 另外，该子句也许执行起来太贵，
	 * 尽管损失
	 * 选择率，每个组只做一次反而更好。不把整个
	 * 规划过程做两遍就很难估计，因此用一条启发式：含有
	 * 子计划的子句留在 HAVING 里。否则，把 HAVING
	 * 子句移动或复制进 WHERE，希望在聚合之前而不是之后
	 * 消除元组。
	 *
	 * 若查询有显式分组，就可以把这样的
	 * 子句直接移进 WHERE；通不过该子句的任何组都不会出现在
	 * 输出里，因为它的元组都到不了分组或
	 * 聚合阶段。否则就必须有一条退化的（不含变量的）
	 * HAVING 子句，把它放进 WHERE，以便 query_planner() 能在
	 * 门控 Result 节点里使用它，同时也留在 HAVING 里，以确保
	 * 不会发出一行虚假的聚合行。（这可以做得更好，但
	 * 似乎不值得优化。）
	 *
	 * 注意此时 havingQual 和 parse->jointree->quals 都是
	 * 隐式 ANDed 列表形式，尽管它们被声明
	 * 为 Node *。
	 */
	newHaving = NIL;
	foreach(l, (List *) parse->havingQual)
	{
		Node	   *havingclause = (Node *) lfirst(l);

		if ((parse->groupClause && parse->groupingSets) ||
			contain_agg_clause(havingclause) ||
			contain_volatile_functions(havingclause) ||
			contain_subplans(havingclause))
		{
			/* 留在 HAVING 里 */
			newHaving = lappend(newHaving, havingclause);
		}
		else if (parse->groupClause && !parse->groupingSets)
		{
			/* 移到 WHERE */
			parse->jointree->quals = (Node *)
				lappend((List *) parse->jointree->quals, havingclause);
		}
		else
		{
			/* 在 WHERE 里放一份副本，并留在 HAVING 里 */
			parse->jointree->quals = (Node *)
				lappend((List *) parse->jointree->quals,
						copyObject(havingclause));
			newHaving = lappend(newHaving, havingclause);
		}
	}
	parse->havingQual = (Node *) newHaving;

	/*
	 * 若有任何外连接，试着把它们收成普通内连接。
	 * 这一步在做完表达式
	 * 预处理之后最容易。
	 */
	if (hasOuterJoins)
		reduce_outer_joins(root);

	/*
	 * 若有任何 RTE_RESULT 关系，看能否从
	 * 连接树里删掉它们。也靠这一处理把外连接之下的单子
	 * FromExprs 展平。这一步在做完表达式预处理和外连接收简
	 * 之后最有效。
	 */
	if (hasResultRTEs || hasOuterJoins)
		remove_useless_result_rtes(root);

	/*
	 * 做主要规划。
	 */
	grouping_planner(root, tuple_fraction);

	/*
	 * 捕获我们能访问的外层参数 ID 集合，供以后
	 * 计算 extParam/allParam 使用。
	 */
	SS_identify_outer_params(root);

	/*
	 * 若本查询层次建立了任何 initPlan，就调整幸存
	 * Path 的代价和并行安全标志，把它们算进去。这些
	 * initPlan 要到 create_plan() 运行时才会真正接到计划树上，
	 * 但现在必须计入它们的效果。
	 */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);
	SS_charge_for_initplans(root, final_rel);

	/*
	 * 确保已经为最终关系找出了最便宜的 Path。（在这里而不是
	 * grouping_planner 里做，决策就会包含 initPlan 的代价，
	 * 尽管那不太可能改变什么。）
	 */
	set_cheapest(final_rel);

	return root;
}

/*
 * preprocess_expression
 *		对一个表达式做 subquery_planner 的预处理工作，
 *		它可以是目标列表、WHERE 子句（包括 JOIN/ON
 *		条件）、HAVING 子句，或其他少数几样。
 */
static Node *
preprocess_expression(PlannerInfo *root, Node *expr, int kind)
{
	/*
	 * 若表达式为空就赶快退出。这种情况够常见，
	 * 值得检查。注意对隐式 AND 结果格式，
	 * null 到 null 也是正确转换。
	 */
	if (expr == NULL)
		return NULL;

	/*
	 * 若查询有任何连接 RTEs，就把连接别名变量换成
	 * 基关系变量。必须先做这个，
	 * 因为从 joinaliasvars 列表抽出的任何表达式
	 * 都还没预处理。
	 * 例如，若在子链接处理之后做，从连接别名展开的子链接
	 * 就不会被处理。但非 LATERAL 的 RTE 函数、VALUES 列表和 TABLESAMPLE 子句可以跳过，
	 * 因为它们无法包含当前查询层次的任何 Var。
	 */
	if (root->hasJoinRTEs &&
		!(kind == EXPRKIND_RTFUNC ||
		  kind == EXPRKIND_VALUES ||
		  kind == EXPRKIND_TABLESAMPLE ||
		  kind == EXPRKIND_TABLEFUNC))
		expr = flatten_join_alias_vars(root, root->parse, expr);

	/*
	 * 简化常量表达式。对函数 RTEs，preprocess_function_rtes 已经
	 * 做过了。（但注意对
	 * EXPRKIND_RTFUNC_LATERAL 必须再做一次，因为那些现在也许含有
	 * 因子查询展平或
	 * 连接别名变量而插入的、尚未简化的子表达式。）
	 *
	 * 注意：这件事的一个必要效果是把命名参数的函数
	 * 调用转换成位置表示法，并插入函数任何默认参数的
	 * 当前实际值。为确保发生，*必须*
	 * 在这里处理全部表达式。以前的 PG 版本有时会在
	 * 常量简化似乎不值得费劲时跳过它，但现在不能
	 * 再那么做了。
	 *
	 * 注意：这也会把嵌套的 AND 和 OR 表达式展平成 N 个参数的
	 * 形式。此后对限定表达式的全部处理都必须
	 * 小心保持 AND/OR 的平坦——也就是不要生成 AND 直接位于 AND 之下、
	 * 或 OR 直接位于 OR 之下的树。
	 */
	if (kind != EXPRKIND_RTFUNC)
		expr = eval_const_expressions(root, expr);

	/*
	 * 若是限定条件或 havingQual，就把它规范化。
	 */
	if (kind == EXPRKIND_QUAL)
	{
		expr = (Node *) canonicalize_qual((Expr *) expr, false);

#ifdef OPTIMIZER_DEBUG
		printf("After canonicalize_qual()\n");
		pprint(expr);
#endif
	}

	/*
	 * 检查带 Const 数组的 ANY ScalarArrayOpExpr，并为任何用哈希查找
	 * 而不是线性搜索也许会执行得更快的那些设置
	 * hashfuncid。
	 */
	if (kind == EXPRKIND_QUAL || kind == EXPRKIND_TARGET)
	{
		convert_saop_to_hashed_saop(expr);
	}

	/* 把 SubLinks 展开成 SubPlans */
	if (root->parse->hasSubLinks)
		expr = SS_process_sublinks(root, expr, (kind == EXPRKIND_QUAL));

	/*
	 * XXX 除非已经领会 SS_replace_correlation_vars 里的注释，
	 * 否则不要在这里插入任何东西……
	 */

	/* 把上层 var 换成 Param 节点（在 VALUES 里这是可能的） */
	if (root->query_level > 1)
		expr = SS_replace_correlation_vars(root, expr);

	/*
	 * 若是限定条件或 havingQual，就把它转换成隐式 AND 格式。（我们
	 * 不想在 eval_const_expressions 之前做，因为后者
	 * 将无法正确简化顶层 AND。另外，
	 * SS_process_sublinks 预期的是显式 AND 格式。）
	 */
	if (kind == EXPRKIND_QUAL)
		expr = (Node *) make_ands_implicit((Expr *) expr);

	return expr;
}

/*
 * preprocess_qual_conditions
 *		递归扫描查询的连接树，并对其中找到的每条限定条件做
 *		subquery_planner 的预处理工作。
 */
static void
preprocess_qual_conditions(PlannerInfo *root, Node *jtnode)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		/* 这里无事可做 */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			preprocess_qual_conditions(root, lfirst(l));

		f->quals = preprocess_expression(root, f->quals, EXPRKIND_QUAL);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		preprocess_qual_conditions(root, j->larg);
		preprocess_qual_conditions(root, j->rarg);

		j->quals = preprocess_expression(root, j->quals, EXPRKIND_QUAL);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * preprocess_phv_expression
 *	  对已被上拉的 PlaceHolderVar 表达式做预处理。
 *
 * 若某个 LATERAL 子查询引用了另一个子查询的输出，而该
 * 输出因中间有外连接必须包进 PlaceHolderVar，
 * 我们就会把该 PlaceHolderVar 表达式下推到子查询里，
 * 以后在 find_lateral_references 期间再拉回来；那发生在
 * subquery_planner 已经预处理完起初就在
 * 当前查询层次的全部表达式之后。因此那时需要预处理它。
 */
Expr *
preprocess_phv_expression(PlannerInfo *root, Expr *expr)
{
	return (Expr *) preprocess_expression(root, (Node *) expr, EXPRKIND_PHV);
}

/*--------------------
 * grouping_planner
 *	  Perform planning steps related to grouping, aggregation, etc.
 *
 * This function adds all required top-level processing to the scan/join
 * Path(s) produced by query_planner.
 *
 * tuple_fraction is the fraction of tuples we expect will be retrieved.
 * tuple_fraction is interpreted as follows:
 *	  0: expect all tuples to be retrieved (normal case)
 *	  0 < tuple_fraction < 1: expect the given fraction of tuples available
 *		from the plan to be retrieved
 *	  tuple_fraction >= 1: tuple_fraction is the absolute number of tuples
 *		expected to be retrieved (ie, a LIMIT specification)
 *
 * Returns nothing; the useful output is in the Paths we attach to the
 * (UPPERREL_FINAL, NULL) upperrel in *root.  In addition,
 * root->processed_tlist contains the final processed targetlist.
 *
 * Note that we have not done set_cheapest() on the final rel; it's convenient
 * to leave this to the caller.
 *--------------------
 */
static void
grouping_planner(PlannerInfo *root, double tuple_fraction)
{
	Query	   *parse = root->parse;
	int64		offset_est = 0;
	int64		count_est = 0;
	double		limit_tuples = -1.0;
	bool		have_postponed_srfs = false;
	PathTarget *final_target;
	List	   *final_targets;
	List	   *final_targets_contain_srfs;
	bool		final_target_parallel_safe;
	RelOptInfo *current_rel;
	RelOptInfo *final_rel;
	FinalPathExtraData extra;
	ListCell   *lc;

	/* 若有 LIMIT/OFFSET，就调整调用方提供的 tuple_fraction */
	if (parse->limitCount || parse->limitOffset)
	{
		tuple_fraction = preprocess_limit(root, tuple_fraction,
										  &offset_est, &count_est);

		/*
		 * 若有已知的 LIMIT，且没有未知的 OFFSET，就可以
		 * 估计使用有界排序的效果。
		 */
		if (count_est > 0 && offset_est >= 0)
			limit_tuples = (double) count_est + (double) offset_est;
	}

	/* 让下层例程能访问 tuple_fraction */
	root->tuple_fraction = tuple_fraction;

	if (parse->setOperations)
	{
		/*
		 * 若有顶层 ORDER BY，就假定必须取出全部
		 * 元组。考虑到下面为了也许避免排序而做的种种取巧，这也许过于简单；
		 * 但这里估计准确的机会
		 * 反正也很低。XXX 试着去掉这个，改为
		 * 让 plan_set_operations 同时生成快速启动和
		 * 总代价最低的路径。
		 */
		if (parse->sortClause)
			root->tuple_fraction = 0.0;

		/*
		 * 为集合运算构造 Path。结果除了也许要做顶层排序和/或 LIMIT 之外，
		 * 不需要任何
		 * 工作。注意递归并集的任何
		 * 特殊工作由 plan_set_operations 负责。
		 */
		current_rel = plan_set_operations(root);

		/*
		 * 不应当需要调用 preprocess_targetlist，因为我们必须
		 * 在一个 SELECT 查询节点里。改为使用 plan_set_operations 返回的
		 * processed_tlist（因为它会告诉我们是否返回了任何
		 * resjunk 列！），并把任何排序键信息从
		 * 原来的 tlist 转过去。
		 */
		Assert(parse->commandType == CMD_SELECT);

		/* 为安全起见，复制 processed_tlist，而不是就地修改 */
		root->processed_tlist =
			postprocess_setop_tlist(copyObject(root->processed_tlist),
									parse->targetList);

		/* 也抽出集合运算结果 tlist 的 PathTarget 形式 */
		final_target = current_rel->cheapest_total_path->pathtarget;

		/* 并检查它是否并行安全 */
		final_target_parallel_safe =
			is_parallel_safe(root, (Node *) final_target->exprs);

		/* 集合运算结果 tlist 不可能含有任何 SRFs */
		Assert(!parse->hasTargetSRFs);
		final_targets = final_targets_contain_srfs = NIL;

		/*
		 * 这里无法处理 FOR [KEY] UPDATE/SHARE（解析器应当已经
		 * 检查过，但还是确认一下）。
		 */
		if (parse->rowMarks)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/*------
			  translator: %s is a SQL row locking clause such as FOR UPDATE */
					 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
							LCS_asString(linitial_node(RowMarkClause,
													   parse->rowMarks)->strength))));

		/*
		 * 计算表示结果排序要求的路径键
		 */
		Assert(parse->distinctClause == NIL);
		root->sort_pathkeys = make_pathkeys_for_sortclauses(root,
															parse->sortClause,
															root->processed_tlist);
	}
	else
	{
		/* 没有集合运算，做常规规划 */
		PathTarget *sort_input_target;
		List	   *sort_input_targets;
		List	   *sort_input_targets_contain_srfs;
		bool		sort_input_target_parallel_safe;
		PathTarget *grouping_target;
		List	   *grouping_targets;
		List	   *grouping_targets_contain_srfs;
		bool		grouping_target_parallel_safe;
		PathTarget *scanjoin_target;
		List	   *scanjoin_targets;
		List	   *scanjoin_targets_contain_srfs;
		bool		scanjoin_target_parallel_safe;
		bool		scanjoin_target_same_exprs;
		bool		have_grouping;
		WindowFuncLists *wflists = NULL;
		List	   *activeWindows = NIL;
		grouping_sets_data *gset_data = NULL;
		standard_qp_extra qp_extra;

		/* 递归查询应当总是有 setOperations */
		Assert(!root->hasRecursion);

		/* 预处理分组集和 GROUP BY 子句，若有 */
		if (parse->groupingSets)
		{
			gset_data = preprocess_grouping_sets(root);
		}
		else if (parse->groupClause)
		{
			/* 预处理常规 GROUP BY 子句，若有 */
			root->processed_groupClause = preprocess_groupclause(root, NIL);
			/* 去掉任何冗余的 GROUP BY 列 */
			remove_useless_groupby_columns(root);
		}

		/*
		 * 预处理目标列表。注意剩余规划工作的很大一部分
		 * 会用 tlist 的 PathTarget 表示来做，但
		 * 也必须维持最终 tlist 的完整表示，以便
		 * 把它的装饰（resname 等）转到完成的 Plan 的最顶层
		 * tlist。这保存在 processed_tlist 里。
		 */
		preprocess_targetlist(root);

		/*
		 * 给全部聚合标上已解析的 aggtranstype，并检测
		 * 重复的、或可以共享转换状态的聚合。必须
		 * 在把 tlist 切成各种
		 * pathtarget 之前做，否则有些 Aggref 节点副本也许会逃脱
		 * 标注。
		 */
		if (parse->hasAggs)
		{
			preprocess_aggrefs(root, (Node *) root->processed_tlist);
			preprocess_aggrefs(root, (Node *) parse->havingQual);
		}

		/*
		 * 找出 tlist 里的任何窗口函数。（不必到
		 * 别处找，因为 ORDER BY 里用的表达式也会在那里。）
		 * 注意它们也许已全部被常量
		 * 折叠去掉，那种情况下不必再做任何工作。
		 */
		if (parse->hasWindowFuncs)
		{
			wflists = find_window_functions((Node *) root->processed_tlist,
											list_length(parse->windowClause));
			if (wflists->numWindowFuncs > 0)
			{
				/*
				 * 看能否对每个 WindowClause
				 * 做些修改，让执行器更快地
				 * 执行这些 WindowFuncs。
				 */
				optimize_window_clauses(root, wflists);

				activeWindows = select_active_windows(root, wflists);
			}
			else
				parse->hasWindowFuncs = false;
		}

		/*
		 * 预处理 MIN/MAX 聚合，若有。注意：在这里和 query_planner() 调用之间
		 * 加逻辑要小心。任何
		 * MIN/MAX 可优化情形所需要的东西，都得在
		 * planagg.c 里复制一份。
		 */
		if (parse->hasAggs)
			preprocess_minmax_aggregates(root);

		/*
		 * 弄清 query_planner 的结果子计划需要返回的行数是否有硬限制。
		 * 即使总体上知道一个
		 * 硬限制，若查询有任何
		 * 分组/聚合操作，或 tlist 里有 SRFs，它也不适用。
		 */
		if (parse->groupClause ||
			parse->groupingSets ||
			parse->distinctClause ||
			parse->hasAggs ||
			parse->hasWindowFuncs ||
			parse->hasTargetSRFs ||
			root->hasHavingQual)
			root->limit_tuples = -1.0;
		else
			root->limit_tuples = limit_tuples;

		/* 建立 standard_qp_callback 所需的数据 */
		qp_extra.activeWindows = activeWindows;
		qp_extra.gset_data = gset_data;

		/*
		 * 为本 Query 的扫描/连接
		 * 部分，即 FROM/WHERE 子句所表示的处理，生成最佳的未排序和预排序路径。
		 * （注意也许没有任何预排序路径。）
		 * 我们也（在 standard_qp_callback 里）生成该查询的排序子句、distinct 子句等的
		 * 路径键表示。
		 */
		current_rel = query_planner(root, standard_qp_callback, &qp_extra);

		/*
		 * 把查询的结果 tlist 转换成 PathTarget 格式。
		 *
		 * 注意：这无法在 query_planner() 做完
		 * appendrel 展开之前做，因为那也许会往
		 * root->processed_tlist 里加 resjunk 项。等到之后也有帮助，
		 * 因为目标宽度估计可以使用在 query_planner() 内部得到的
		 * 每个 Var 的宽度数字。
		 */
		final_target = create_pathtarget(root, root->processed_tlist);
		final_target_parallel_safe =
			is_parallel_safe(root, (Node *) final_target->exprs);

		/*
		 * 若给了 ORDER BY，考虑是否应当使用排序后
		 * 投影，若是，就计算前面步骤的
		 * 调整后目标。
		 */
		if (parse->sortClause)
		{
			sort_input_target = make_sort_input_target(root,
													   final_target,
													   &have_postponed_srfs);
			sort_input_target_parallel_safe =
				is_parallel_safe(root, (Node *) sort_input_target->exprs);
		}
		else
		{
			sort_input_target = final_target;
			sort_input_target_parallel_safe = final_target_parallel_safe;
		}

		/*
		 * 若有窗口函数要处理，任何
		 * 分组步骤的输出都需要是窗口函数想要的；
		 * 否则应当是 sort_input_target。
		 */
		if (activeWindows)
		{
			grouping_target = make_window_input_target(root,
													   final_target,
													   activeWindows);
			grouping_target_parallel_safe =
				is_parallel_safe(root, (Node *) grouping_target->exprs);
		}
		else
		{
			grouping_target = sort_input_target;
			grouping_target_parallel_safe = sort_input_target_parallel_safe;
		}

		/*
		 * 若有分组或聚合要做，最顶层的扫描/连接
		 * 计划节点必须发出分组步骤想要的东西；否则，它
		 * 应当发出 grouping_target。
		 */
		have_grouping = (parse->groupClause || parse->groupingSets ||
						 parse->hasAggs || root->hasHavingQual);
		if (have_grouping)
		{
			scanjoin_target = make_group_input_target(root, final_target);
			scanjoin_target_parallel_safe =
				is_parallel_safe(root, (Node *) scanjoin_target->exprs);
		}
		else
		{
			scanjoin_target = grouping_target;
			scanjoin_target_parallel_safe = grouping_target_parallel_safe;
		}

		/*
		 * 若目标列表里有任何 SRFs，必须把这些 PathTargets
		 * 各自分成计算 SRF 的和不含 SRF 的目标。把
		 * 每个具名目标换成不含 SRF 的版本，并记住
		 * 以后需要添加的额外投影步骤列表。
		 */
		if (parse->hasTargetSRFs)
		{
			/* final_target 不重新计算 sort_input_target 里的任何 SRFs */
			split_pathtarget_at_srfs(root, final_target, sort_input_target,
									 &final_targets,
									 &final_targets_contain_srfs);
			final_target = linitial_node(PathTarget, final_targets);
			Assert(!linitial_int(final_targets_contain_srfs));
			/* sort_input_target 相对 grouping_target 同样如此 */
			split_pathtarget_at_srfs(root, sort_input_target, grouping_target,
									 &sort_input_targets,
									 &sort_input_targets_contain_srfs);
			sort_input_target = linitial_node(PathTarget, sort_input_targets);
			Assert(!linitial_int(sort_input_targets_contain_srfs));
			/* grouping_target 相对 scanjoin_target 同样如此 */
			split_pathtarget_at_srfs(root, grouping_target, scanjoin_target,
									 &grouping_targets,
									 &grouping_targets_contain_srfs);
			grouping_target = linitial_node(PathTarget, grouping_targets);
			Assert(!linitial_int(grouping_targets_contain_srfs));
			/* 不会为 scanjoin_target 预先计算任何 SRFs */
			split_pathtarget_at_srfs(root, scanjoin_target, NULL,
									 &scanjoin_targets,
									 &scanjoin_targets_contain_srfs);
			scanjoin_target = linitial_node(PathTarget, scanjoin_targets);
			Assert(!linitial_int(scanjoin_targets_contain_srfs));
		}
		else
		{
			/* 初始化列表；对其中多数，哑值就可以 */
			final_targets = final_targets_contain_srfs = NIL;
			sort_input_targets = sort_input_targets_contain_srfs = NIL;
			grouping_targets = grouping_targets_contain_srfs = NIL;
			scanjoin_targets = list_make1(scanjoin_target);
			scanjoin_targets_contain_srfs = NIL;
		}

		/* 应用扫描/连接目标。 */
		scanjoin_target_same_exprs = list_length(scanjoin_targets) == 1
			&& equal(scanjoin_target->exprs, current_rel->reltarget->exprs);
		apply_scanjoin_target_to_paths(root, current_rel, scanjoin_targets,
									   scanjoin_targets_contain_srfs,
									   scanjoin_target_parallel_safe,
									   scanjoin_target_same_exprs);

		/*
		 * 把刚刚算出的各种上层关系 PathTargets 存进
		 * root->upper_targets[]。核心代码不用这个，但它
		 * 给扩展提供一个方便的地方来取得这些信息。为
		 * 一致起见，我们保存全部中间目标，即使本查询并不需要
		 * 某些对应的上层关系。
		 */
		root->upper_targets[UPPERREL_FINAL] = final_target;
		root->upper_targets[UPPERREL_ORDERED] = final_target;
		root->upper_targets[UPPERREL_PARTIAL_DISTINCT] = sort_input_target;
		root->upper_targets[UPPERREL_DISTINCT] = sort_input_target;
		root->upper_targets[UPPERREL_WINDOW] = sort_input_target;
		root->upper_targets[UPPERREL_GROUP_AGG] = grouping_target;

		/*
		 * 若有分组和/或聚合，就考虑实现
		 * 它们的办法。我们建立一个新的上层关系，表示这一
		 * 阶段的输出。
		 */
		if (have_grouping)
		{
			current_rel = create_grouping_paths(root,
												current_rel,
												grouping_target,
												grouping_target_parallel_safe,
												gset_data);
			/* 若 grouping_target 含有 SRFs，就收拾一下 */
			if (parse->hasTargetSRFs)
				adjust_paths_for_srfs(root, current_rel,
									  grouping_targets,
									  grouping_targets_contain_srfs);
		}

		/*
		 * 若有窗口函数，就考虑实现它们的办法。我们
		 * 建立一个新的上层关系，表示这一阶段的输出。
		 */
		if (activeWindows)
		{
			current_rel = create_window_paths(root,
											  current_rel,
											  grouping_target,
											  sort_input_target,
											  sort_input_target_parallel_safe,
											  wflists,
											  activeWindows);
			/* 若 sort_input_target 含有 SRFs，就收拾一下 */
			if (parse->hasTargetSRFs)
				adjust_paths_for_srfs(root, current_rel,
									  sort_input_targets,
									  sort_input_targets_contain_srfs);
		}

		/*
		 * 若有 DISTINCT 子句，就考虑实现它的办法。我们
		 * 建立一个新的上层关系，表示这一阶段的输出。
		 */
		if (parse->distinctClause)
		{
			current_rel = create_distinct_paths(root,
												current_rel);
		}
	}							/* if (setOperations) 结束 */

	/*
	 * 若给了 ORDER BY，就考虑实现它的办法，并生成一个
	 * 只含发出正确顺序并
	 * 投影出正确 final_target 的路径的新上层关系。这里可以在排序代价里应用原来的
	 * limit_tuples 限制，但仅当没有
	 * 被推迟的 SRFs 时。
	 */
	if (parse->sortClause)
	{
		current_rel = create_ordered_paths(root,
										   current_rel,
										   final_target,
										   final_target_parallel_safe,
										   have_postponed_srfs ? -1.0 :
										   limit_tuples);
		/* 若 final_target 含有 SRFs，就收拾一下 */
		if (parse->hasTargetSRFs)
			adjust_paths_for_srfs(root, current_rel,
								  final_targets,
								  final_targets_contain_srfs);
	}

	/*
	 * 现在准备建立最终输出的上层关系。
	 */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);

	/*
	 * 若输入关系标了 consider_parallel，且 LIMIT 子句里没有
	 * 任何并行不安全的东西，就可以把 final_rel 也标成
	 * consider_parallel。注意若查询有 rowMark 或不是
	 * SELECT，查询里每个关系的 consider_parallel 都将为
	 * 假。
	 */
	if (current_rel->consider_parallel &&
		is_parallel_safe(root, parse->limitOffset) &&
		is_parallel_safe(root, parse->limitCount))
		final_rel->consider_parallel = true;

	/*
	 * 若 current_rel 属于单个 FDW，final_rel 也是。
	 */
	final_rel->serverid = current_rel->serverid;
	final_rel->userid = current_rel->userid;
	final_rel->useridiscurrent = current_rel->useridiscurrent;
	final_rel->fdwroutine = current_rel->fdwroutine;

	/*
	 * 为 final_rel 生成路径。插入全部幸存路径，并在需要时加上
	 * LockRows、Limit 和/或 ModifyTable 步骤。
	 */
	foreach(lc, current_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		/*
		 * 若有 FOR [KEY] UPDATE/SHARE 子句，就加 LockRows 节点。
		 * （注意：这里有意测试 parse->rowMarks 而不是 root->rowMarks。
		 * 若只有非锁定的行标记，它们应当由
		 * ModifyTable 节点处理。不过，进入 LockRows 节点的是
		 * root->rowMarks。）
		 */
		if (parse->rowMarks)
		{
			path = (Path *) create_lockrows_path(root, final_rel, path,
												 root->rowMarks,
												 assign_special_exec_param(root));
		}

		/*
		 * 若有 LIMIT/OFFSET 子句，就加 LIMIT 节点。
		 */
		if (limit_needed(parse))
		{
			path = (Path *) create_limit_path(root, final_rel, path,
											  parse->limitOffset,
											  parse->limitCount,
											  parse->limitOption,
											  offset_est, count_est);
		}

		/*
		 * 若这是 INSERT/UPDATE/DELETE/MERGE，就加 ModifyTable 节点。
		 */
		if (parse->commandType != CMD_SELECT)
		{
			Index		rootRelation;
			List	   *resultRelations = NIL;
			List	   *updateColnosLists = NIL;
			List	   *withCheckOptionLists = NIL;
			List	   *returningLists = NIL;
			List	   *mergeActionLists = NIL;
			List	   *rowMarks;

			if (bms_membership(root->all_result_relids) == BMS_MULTIPLE)
			{
				/* 继承的 UPDATE/DELETE/MERGE */
				RelOptInfo *top_result_rel = find_base_rel(root,
														   parse->resultRelation);
				int			resultRelation = -1;

				/* 把根结果关系传给执行器。 */
				rootRelation = parse->resultRelation;

				/* 只把叶子子项加进 ModifyTable。 */
				while ((resultRelation = bms_next_member(root->leaf_result_relids,
														 resultRelation)) >= 0)
				{
					RelOptInfo *this_result_rel = find_base_rel(root,
																resultRelation);

					/*
					 * 也排除任何加入列表之后变成哑的叶子关系，
					 * 例如被约束排除
					 * 排除掉的那些。
					 */
					if (IS_DUMMY_REL(this_result_rel))
						continue;

					/* 建立 ModifyTable 所需的按目标关系列表 */
					resultRelations = lappend_int(resultRelations,
												  resultRelation);
					if (parse->commandType == CMD_UPDATE)
					{
						List	   *update_colnos = root->update_colnos;

						if (this_result_rel != top_result_rel)
							update_colnos =
								adjust_inherited_attnums_multilevel(root,
																	update_colnos,
																	this_result_rel->relid,
																	top_result_rel->relid);
						updateColnosLists = lappend(updateColnosLists,
													update_colnos);
					}
					if (parse->withCheckOptions)
					{
						List	   *withCheckOptions = parse->withCheckOptions;

						if (this_result_rel != top_result_rel)
							withCheckOptions = (List *)
								adjust_appendrel_attrs_multilevel(root,
																  (Node *) withCheckOptions,
																  this_result_rel,
																  top_result_rel);
						withCheckOptionLists = lappend(withCheckOptionLists,
													   withCheckOptions);
					}
					if (parse->returningList)
					{
						List	   *returningList = parse->returningList;

						if (this_result_rel != top_result_rel)
							returningList = (List *)
								adjust_appendrel_attrs_multilevel(root,
																  (Node *) returningList,
																  this_result_rel,
																  top_result_rel);
						returningLists = lappend(returningLists,
												 returningList);
					}
					if (parse->mergeActionList)
					{
						ListCell   *l;
						List	   *mergeActionList = NIL;

						/*
						 * 复制 MergeActions，并翻译
						 * 引用属性号的那些东西。
						 */
						foreach(l, parse->mergeActionList)
						{
							MergeAction *action = lfirst(l),
									   *leaf_action = copyObject(action);

							leaf_action->qual =
								adjust_appendrel_attrs_multilevel(root,
																  (Node *) action->qual,
																  this_result_rel,
																  top_result_rel);
							leaf_action->targetList = (List *)
								adjust_appendrel_attrs_multilevel(root,
																  (Node *) action->targetList,
																  this_result_rel,
																  top_result_rel);
							if (leaf_action->commandType == CMD_UPDATE)
								leaf_action->updateColnos =
									adjust_inherited_attnums_multilevel(root,
																		action->updateColnos,
																		this_result_rel->relid,
																		top_result_rel->relid);
							mergeActionList = lappend(mergeActionList,
													  leaf_action);
						}

						mergeActionLists = lappend(mergeActionLists,
												   mergeActionList);
					}
				}

				if (resultRelations == NIL)
				{
					/*
					 * 我们设法排除了每个子关系，因此用顶层目标
					 * 关系的信息生成一个
					 * 哑的单关系计划（即使它也许不是叶子目标）。
					 * 虽然显然不会更新或
					 * 删除任何数据，仍需要有一个 ModifyTable 节点，以便
					 * 执行任何语句触发器。（若修好 nodeModifyTable.c，允许
					 * 零个目标关系，这可以更干净，
					 * 但那大概不会是
					 * 净收益。）
					 */
					resultRelations = list_make1_int(parse->resultRelation);
					if (parse->commandType == CMD_UPDATE)
						updateColnosLists = list_make1(root->update_colnos);
					if (parse->withCheckOptions)
						withCheckOptionLists = list_make1(parse->withCheckOptions);
					if (parse->returningList)
						returningLists = list_make1(parse->returningList);
					if (parse->mergeActionList)
						mergeActionLists = list_make1(parse->mergeActionList);
				}
			}
			else
			{
				/* 单关系的 INSERT/UPDATE/DELETE/MERGE。 */
				rootRelation = 0;	/* 没有单独的根关系 */
				resultRelations = list_make1_int(parse->resultRelation);
				if (parse->commandType == CMD_UPDATE)
					updateColnosLists = list_make1(root->update_colnos);
				if (parse->withCheckOptions)
					withCheckOptionLists = list_make1(parse->withCheckOptions);
				if (parse->returningList)
					returningLists = list_make1(parse->returningList);
				if (parse->mergeActionList)
					mergeActionLists = list_make1(parse->mergeActionList);
			}

			/*
			 * 若有 FOR [KEY] UPDATE/SHARE 子句，LockRows 节点
			 * 会负责取出未锁定的被标记行，否则
			 * 需要让 ModifyTable 来做。
			 */
			if (parse->rowMarks)
				rowMarks = NIL;
			else
				rowMarks = root->rowMarks;

			path = (Path *)
				create_modifytable_path(root, final_rel,
										path,
										parse->commandType,
										parse->canSetTag,
										parse->resultRelation,
										rootRelation,
										root->partColsUpdated,
										resultRelations,
										updateColnosLists,
										withCheckOptionLists,
										returningLists,
										rowMarks,
										parse->onConflict,
										mergeActionLists,
										assign_special_exec_param(root));
		}

		/* 并把它塞进 final_rel */
		add_path(final_rel, path);
	}

	/*
	 * 若外层查询层次也许用得上，
	 * 也为 final_rel 生成部分路径。
	 */
	if (final_rel->consider_parallel && root->query_level > 1 &&
		!limit_needed(parse))
	{
		Assert(!parse->rowMarks && parse->commandType == CMD_SELECT);
		foreach(lc, current_rel->partial_pathlist)
		{
			Path	   *partial_path = (Path *) lfirst(lc);

			add_partial_path(final_rel, partial_path);
		}
	}

	extra.limit_needed = limit_needed(parse);
	extra.limit_tuples = limit_tuples;
	extra.count_est = count_est;
	extra.offset_est = offset_est;

	/*
	 * 若有一个 FDW 负责该查询的全部基关系，
	 * 就让它考虑添加 ForeignPaths。
	 */
	if (final_rel->fdwroutine &&
		final_rel->fdwroutine->GetForeignUpperPaths)
		final_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_FINAL,
													current_rel, final_rel,
													&extra);

	/* 让扩展也许再添加一些路径 */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_FINAL,
									current_rel, final_rel, &extra);

	/* 注意：目前留给调用方去做 set_cheapest() */
}

/*
 * 对 groupingSets 子句及相关数据做预处理。这处理
 * 展开分组集、把它们组织成 rollup 列表
 * 的初步步骤，并准备以后会填上
 * 大小估计的标注。
 */
static grouping_sets_data *
preprocess_grouping_sets(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	List	   *sets;
	int			maxref = 0;
	ListCell   *lc_set;
	grouping_sets_data *gd = palloc0(sizeof(grouping_sets_data));

	parse->groupingSets = expand_grouping_sets(parse->groupingSets, parse->groupDistinct, -1);

	gd->any_hashable = false;
	gd->unhashable_refs = NULL;
	gd->unsortable_refs = NULL;
	gd->unsortable_sets = NIL;

	/*
	 * 目前有分组集时，我们完全不试图优化 groupClause，
	 * 因此只在 processed_groupClause 里复制一份。
	 */
	root->processed_groupClause = parse->groupClause;

	if (parse->groupClause)
	{
		ListCell   *lc;

		foreach(lc, parse->groupClause)
		{
			SortGroupClause *gc = lfirst_node(SortGroupClause, lc);
			Index		ref = gc->tleSortGroupRef;

			if (ref > maxref)
				maxref = ref;

			if (!gc->hashable)
				gd->unhashable_refs = bms_add_member(gd->unhashable_refs, ref);

			if (!OidIsValid(gc->sortop))
				gd->unsortable_refs = bms_add_member(gd->unsortable_refs, ref);
		}
	}

	/* 为重映射分配工作区数组 */
	gd->tleref_to_colnum_map = (int *) palloc((maxref + 1) * sizeof(int));

	/*
	 * 若有任何不可排序的集合，必须在准备 rollup 之前把它们抽出来。
	 * 不可排序的集合不经过
	 * reorder_grouping_sets，因此必须在这里应用 GroupingSetData
	 * 标注。
	 */
	if (!bms_is_empty(gd->unsortable_refs))
	{
		List	   *sortable_sets = NIL;
		ListCell   *lc;

		foreach(lc, parse->groupingSets)
		{
			List	   *gset = (List *) lfirst(lc);

			if (bms_overlap_list(gd->unsortable_refs, gset))
			{
				GroupingSetData *gs = makeNode(GroupingSetData);

				gs->set = gset;
				gd->unsortable_sets = lappend(gd->unsortable_sets, gs);

				/*
				 * 必须在这里强制不可排序的集合是可哈希的；
				 * 后面的代码假定如此。语法分析只检查
				 * 每一列要么可哈希，要么可排序。
				 *
				 * 注意通过这个测试并不保证能
				 * 生成计划；也许还有其他障碍。
				 */
				if (bms_overlap_list(gd->unhashable_refs, gset))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("could not implement GROUP BY"),
							 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));
			}
			else
				sortable_sets = lappend(sortable_sets, gset);
		}

		if (sortable_sets)
			sets = extract_rollup_sets(sortable_sets);
		else
			sets = NIL;
	}
	else
		sets = extract_rollup_sets(parse->groupingSets);

	foreach(lc_set, sets)
	{
		List	   *current_sets = (List *) lfirst(lc_set);
		RollupData *rollup = makeNode(RollupData);
		GroupingSetData *gs;

		/*
		 * 把当前分组集列表重排成正确的前缀
		 * 顺序。若只需要一趟聚合，就试着让
		 * 列表匹配 ORDER BY 子句；若需要多于一趟，
		 * 就不费这个心。
		 *
		 * 注意这会把集合从成员最少的排到
		 * 成员最多的，并应用 GroupingSetData 标注，
		 * 不过数据要到以后才填。
		 */
		current_sets = reorder_grouping_sets(current_sets,
											 (list_length(sets) == 1
											  ? parse->sortClause
											  : NIL));

		/*
		 * 取得最初的（因此也是最大的）分组集。
		 */
		gs = linitial_node(GroupingSetData, current_sets);

		/*
		 * 适当地排列 groupClause。若第一个分组集
		 * 为空，则 groupClause 也必须为空；否则必须
		 * 强制 groupClause 匹配该分组集的顺序。
		 *
		 * （只有当全部非空分组集都不可排序时，第一个分组集才能为空，而 parse->groupClause
		 * 却不空。
		 * 哈希分组集的 groupClause 以后再建。）
		 */
		if (gs->set)
			rollup->groupClause = preprocess_groupclause(root, gs->set);
		else
			rollup->groupClause = NIL;

		/*
		 * 它可哈希吗？我们假装空集可哈希，尽管以后
		 * 实际上会强制它们不被哈希。但若
		 * 除了空集什么都没有，就不必费心（因为那种情况下什么都
		 * 不能哈希）。
		 */
		if (gs->set &&
			!bms_overlap_list(gd->unhashable_refs, gs->set))
		{
			rollup->hashable = true;
			gd->any_hashable = true;
		}

		/*
		 * 既然已经为这
		 * 份分组集列表钉死了 groupClause 的顺序，就需要把分组
		 * 集里的项从 sortgroupref 重映射成该分组集集合的
		 * groupClause 里的普通下标（从 0 起）。不过我们保留
		 * 原来的形式供以后使用。
		 */
		rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,
												 current_sets,
												 gd->tleref_to_colnum_map);
		rollup->gsets_data = current_sets;

		gd->rollups = lappend(gd->rollups, rollup);
	}

	if (gd->unsortable_sets)
	{
		/*
		 * 还没为这个钉死 groupclause，但为了估计，将
		 * 需要基于下标的列表。目前根据整个原来的 groupclause 构造
		 * hash_sets_idx。
		 */
		gd->hash_sets_idx = remap_to_groupclause_idx(parse->groupClause,
													 gd->unsortable_sets,
													 gd->tleref_to_colnum_map);
		gd->any_hashable = true;
	}

	return gd;
}

/*
 * 给定一份 groupclause 和一份 GroupingSetData 列表，返回映射到给定 groupclause 下标的等价集合
 * （不带标注）。
 */
static List *
remap_to_groupclause_idx(List *groupClause,
						 List *gsets,
						 int *tleref_to_colnum_map)
{
	int			ref = 0;
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, groupClause)
	{
		SortGroupClause *gc = lfirst_node(SortGroupClause, lc);

		tleref_to_colnum_map[gc->tleSortGroupRef] = ref++;
	}

	foreach(lc, gsets)
	{
		List	   *set = NIL;
		ListCell   *lc2;
		GroupingSetData *gs = lfirst_node(GroupingSetData, lc);

		foreach(lc2, gs->set)
		{
			set = lappend_int(set, tleref_to_colnum_map[lfirst_int(lc2)]);
		}

		result = lappend(result, set);
	}

	return result;
}


/*
 * preprocess_rowmarks - 若需要，建立 PlanRowMarks
 */
static void
preprocess_rowmarks(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	Bitmapset  *rels;
	List	   *prowmarks;
	ListCell   *l;
	int			i;

	if (parse->rowMarks)
	{
		/*
		 * 若 FOR [KEY] UPDATE/SHARE 出现在
		 * 分组内部，就有麻烦了，因为分组会使对单个元组
		 * CTIDs 的引用无效。解析时也检查这个，但那
		 * 不够，因为有规则替换、查询上拉等。
		 */
		CheckSelectLocking(parse, linitial_node(RowMarkClause,
												parse->rowMarks)->strength);
	}
	else
	{
		/*
		 * 只有 UPDATE、DELETE、MERGE 或 FOR [KEY]
		 * UPDATE/SHARE 才需要行标记。
		 */
		if (parse->commandType != CMD_UPDATE &&
			parse->commandType != CMD_DELETE &&
			parse->commandType != CMD_MERGE)
			return;
	}

	/*
	 * 除目标之外的全部基关系都需要行标记。我们
	 * 做一份全部基关系的位图集，然后去掉不
	 * 需要的、或已有 FOR [KEY] UPDATE/SHARE 标记的项。
	 */
	rels = get_relids_in_jointree((Node *) parse->jointree, false, false);
	if (parse->resultRelation)
		rels = bms_del_member(rels, parse->resultRelation);

	/*
	 * 把 RowMarkClauses 转换成 PlanRowMark 表示。
	 */
	prowmarks = NIL;
	foreach(l, parse->rowMarks)
	{
		RowMarkClause *rc = lfirst_node(RowMarkClause, l);
		RangeTblEntry *rte = rt_fetch(rc->rti, parse->rtable);
		PlanRowMark *newrc;

		/*
		 * 目前，语法上不可能把 FOR UPDATE 等
		 * 用到更新/删除目标关系上。若那以后变得
		 * 可能，就应当把目标从 PlanRowMark 列表里去掉。
		 */
		Assert(rc->rti != parse->resultRelation);

		/*
		 * 忽略子查询的 RowMarkClauses；它们不是真表，
		 * 没法支持真正的锁定。被展平进
		 * 主查询的子查询应当完全忽略。没被展平的那些会在
		 * 下一轮循环里得到 ROW_MARK_COPY 项。
		 */
		if (rte->rtekind != RTE_RELATION)
			continue;

		rels = bms_del_member(rels, rc->rti);

		newrc = makeNode(PlanRowMark);
		newrc->rti = newrc->prti = rc->rti;
		newrc->rowmarkId = ++(root->glob->lastRowMarkId);
		newrc->markType = select_rowmark_type(rte, rc->strength);
		newrc->allMarkTypes = (1 << newrc->markType);
		newrc->strength = rc->strength;
		newrc->waitPolicy = rc->waitPolicy;
		newrc->isParent = false;

		prowmarks = lappend(prowmarks, newrc);
	}

	/*
	 * 现在，为任何非目标、未锁定的基关系加上行标记。
	 */
	i = 0;
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);
		PlanRowMark *newrc;

		i++;
		if (!bms_is_member(i, rels))
			continue;

		newrc = makeNode(PlanRowMark);
		newrc->rti = newrc->prti = i;
		newrc->rowmarkId = ++(root->glob->lastRowMarkId);
		newrc->markType = select_rowmark_type(rte, LCS_NONE);
		newrc->allMarkTypes = (1 << newrc->markType);
		newrc->strength = LCS_NONE;
		newrc->waitPolicy = LockWaitBlock;	/* 无关紧要 */
		newrc->isParent = false;

		prowmarks = lappend(prowmarks, newrc);
	}

	root->rowMarks = prowmarks;
}

/*
 * 为给定表选择要使用的 RowMarkType
 */
RowMarkType
select_rowmark_type(RangeTblEntry *rte, LockClauseStrength strength)
{
	if (rte->rtekind != RTE_RELATION)
	{
		/* 若它根本不是表，就用 ROW_MARK_COPY */
		return ROW_MARK_COPY;
	}
	else if (rte->relkind == RELKIND_FOREIGN_TABLE)
	{
		/* 若 FDW 愿意，就让它选择行标记类型 */
		FdwRoutine *fdwroutine = GetFdwRoutineByRelId(rte->relid);

		if (fdwroutine->GetForeignRowMarkType != NULL)
			return fdwroutine->GetForeignRowMarkType(rte, strength);
		/* 否则，默认使用 ROW_MARK_COPY */
		return ROW_MARK_COPY;
	}
	else
	{
		/* 普通表，应用适当的锁类型 */
		switch (strength)
		{
			case LCS_NONE:

				/*
				 * 不需要元组锁，只需要能重新取出
				 * 该行。
				 */
				return ROW_MARK_REFERENCE;
				break;
			case LCS_FORKEYSHARE:
				return ROW_MARK_KEYSHARE;
				break;
			case LCS_FORSHARE:
				return ROW_MARK_SHARE;
				break;
			case LCS_FORNOKEYUPDATE:
				return ROW_MARK_NOKEYEXCLUSIVE;
				break;
			case LCS_FORUPDATE:
				return ROW_MARK_EXCLUSIVE;
				break;
		}
		elog(ERROR, "unrecognized LockClauseStrength %d", (int) strength);
		return ROW_MARK_EXCLUSIVE;	/* 消除编译器告警 */
	}
}

/*
 * preprocess_limit - 为 LIMIT 和/或 OFFSET 子句做预估计
 *
 * 我们试着估计 LIMIT/OFFSET 子句的值，并把
 * 结果放回 *count_est 和 *offset_est。若对应子句不存在，这些变量设为
 * 0；若存在
 * 但估不出值，则设为 -1。（"0" 这个约定对
 * OFFSET 可以，对 LIMIT 则有点勉强：实际上我们把
 * LIMIT 0 估成好像是 LIMIT 1。但这符合规划器
 * 从不把行数估成少于一行的惯例。）这些值将
 * 传给 create_limit_path，若改这段代码请看那边。
 *
 * 返回值是规划该查询时应当使用的、适当调整过的 tuple_fraction。
 * 这个调整不可覆盖，因为它反映的是
 * grouping_planner() 肯定会采取的计划动作，而不是关于上下文的
 * 假设。
 */
static double
preprocess_limit(PlannerInfo *root, double tuple_fraction,
				 int64 *offset_est, int64 *count_est)
{
	Query	   *parse = root->parse;
	Node	   *est;
	double		limit_fraction;

	/* 除非有 LIMIT 或 OFFSET，否则不应当被调用 */
	Assert(parse->limitCount || parse->limitOffset);

	/*
	 * 试着取得子句的值。我们用 estimate_expression_value，
	 * 主要是因为它有时能对 Param 做点有用的事。
	 */
	if (parse->limitCount)
	{
		est = estimate_expression_value(root, parse->limitCount);
		if (est && IsA(est, Const))
		{
			if (((Const *) est)->constisnull)
			{
				/* NULL 表示 LIMIT ALL，即没有限制 */
				*count_est = 0; /* 当作不存在 */
			}
			else
			{
				*count_est = DatumGetInt64(((Const *) est)->constvalue);
				if (*count_est <= 0)
					*count_est = 1; /* 强制至少为 1 */
			}
		}
		else
			*count_est = -1;	/* 无法估计 */
	}
	else
		*count_est = 0;			/* 不存在 */

	if (parse->limitOffset)
	{
		est = estimate_expression_value(root, parse->limitOffset);
		if (est && IsA(est, Const))
		{
			if (((Const *) est)->constisnull)
			{
				/* 把 NULL 当作没有偏移；执行器也会这么做 */
				*offset_est = 0;	/* 当作不存在 */
			}
			else
			{
				*offset_est = DatumGetInt64(((Const *) est)->constvalue);
				if (*offset_est < 0)
					*offset_est = 0;	/* 当作不存在 */
			}
		}
		else
			*offset_est = -1;	/* 无法估计 */
	}
	else
		*offset_est = 0;		/* 不存在 */

	if (*count_est != 0)
	{
		/*
		 * LIMIT 子句限制返回元组的绝对数量。
		 * 不过，若它不是常量 LIMIT，就只好猜；在
		 * 没有更好主意时，假定想要计划结果的 10%。
		 */
		if (*count_est < 0 || *offset_est < 0)
		{
			/* LIMIT 或 OFFSET 是表达式……放弃…… */
			limit_fraction = 0.10;
		}
		else
		{
			/* LIMIT（加上 OFFSET，若有）是所需元组的最大数量 */
			limit_fraction = (double) *count_est + (double) *offset_est;
		}

		/*
		 * 若调用方和 LIMIT 都给了绝对限制，就用
		 * 较小的值；两者都是分数时同样如此。若一个是
		 * 分数、另一个是绝对数，不容易判断哪个
		 * 更小，但我们用这条启发式：绝对数通常
		 * 更小。
		 */
		if (tuple_fraction >= 1.0)
		{
			if (limit_fraction >= 1.0)
			{
				/* 两者都是绝对数 */
				tuple_fraction = Min(tuple_fraction, limit_fraction);
			}
			else
			{
				/* 调用方是绝对数，限制是分数；用调用方的值 */
			}
		}
		else if (tuple_fraction > 0.0)
		{
			if (limit_fraction >= 1.0)
			{
				/* 调用方是分数，限制是绝对数；用限制 */
				tuple_fraction = limit_fraction;
			}
			else
			{
				/* 两者都是分数 */
				tuple_fraction = Min(tuple_fraction, limit_fraction);
			}
		}
		else
		{
			/* 调用方没有信息，就用限制 */
			tuple_fraction = limit_fraction;
		}
	}
	else if (*offset_est != 0 && tuple_fraction > 0.0)
	{
		/*
		 * 有 OFFSET 但没有 LIMIT。这与
		 * LIMIT 的情形完全不同：这里需要增大而不是减小
		 * 调用方的 tuple_fraction，因为 OFFSET 会使取出的
		 * 元组变多而不是变少。不过这只在得到的
		 * tuple_fraction > 0 时才要紧。
		 *
		 * 同上，若 OFFSET 存在但估不出，就用 10%。
		 */
		if (*offset_est < 0)
			limit_fraction = 0.10;
		else
			limit_fraction = (double) *offset_est;

		/*
		 * 若调用方和 OFFSET 都给了绝对计数，就把它们
		 * 加在一起；两者都是分数时同样如此。若一个是
		 * 分数、另一个是绝对数，我们想取较大的，并
		 * 启发式地假定那是分数那个。
		 */
		if (tuple_fraction >= 1.0)
		{
			if (limit_fraction >= 1.0)
			{
				/* 两者都是绝对数，因此加在一起 */
				tuple_fraction += limit_fraction;
			}
			else
			{
				/* 调用方是绝对数，限制是分数；用限制 */
				tuple_fraction = limit_fraction;
			}
		}
		else
		{
			if (limit_fraction >= 1.0)
			{
				/* 调用方是分数，限制是绝对数；用调用方的值 */
			}
			else
			{
				/* 两者都是分数，因此加在一起 */
				tuple_fraction += limit_fraction;
				if (tuple_fraction >= 1.0)
					tuple_fraction = 0.0;	/* 假定全部取出 */
			}
		}
	}

	return tuple_fraction;
}

/*
 * limit_needed - 是否真的需要一个 Limit 计划节点？
 *
 * 若 OFFSET 是常量零且 LIMIT 是常量 null，就可以跳过添加
 * Limit 节点。这值得检查，因为 "OFFSET 0" 是一种常见的
 * 优化栅栏说法。（因为规划器里的其他地方
 * 只检查 parse->limitOffset 是否非 NULL，它仍会作为
 * 优化栅栏起作用——我们只是压掉不必要的运行时
 * 开销。）
 *
 * 这看起来也许可以并进 preprocess_limit，但有一个
 * 关键区别：这里需要 OFFSET/LIMIT 里的硬常量，而
 * 在 preprocess_limit 里考虑估计值就够了。
 */
bool
limit_needed(Query *parse)
{
	Node	   *node;

	node = parse->limitCount;
	if (node)
	{
		if (IsA(node, Const))
		{
			/* NULL 表示 LIMIT ALL，即没有限制 */
			if (!((Const *) node)->constisnull)
				return true;	/* 带常量值的 LIMIT */
		}
		else
			return true;		/* 非常量的 LIMIT */
	}

	node = parse->limitOffset;
	if (node)
	{
		if (IsA(node, Const))
		{
			/* 把 NULL 当作没有偏移；执行器也会 */
			if (!((Const *) node)->constisnull)
			{
				int64		offset = DatumGetInt64(((Const *) node)->constvalue);

				if (offset != 0)
					return true;	/* 带非零值的 OFFSET */
			}
		}
		else
			return true;		/* 非常量的 OFFSET */
	}

	return false;				/* 不需要 Limit 计划节点 */
}


/*
 * remove_useless_groupby_columns
 *		去掉 GROUP BY 子句里因函数依赖于其他 GROUP BY 列而冗余的
 *		任何列。
 *
 * 由于有些其他 DBMSes 不允许引用未分组的列，
 * 把全部列都列进 GROUP BY 并不少见，即使只列
 * 主键列就够了。删掉这些多余的列
 * 可以避免冗余的排序工作，因此值得做。
 *
 * 关系缓存失效会确保当主键约束的底层索引被删除时，
 * 缓存的计划失效。
 *
 * 目前我们只用主键约束来做这件事，不过以后
 * 也许想再进一步，也使用带 NOT NULL 列的唯一约束。
 * 那种情况下计划失效仍然
 * 有效，因为当 NOT NULL
 * 约束被删除时，关系会收到一次关系缓存失效。
 */
static void
remove_useless_groupby_columns(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	Bitmapset **groupbyattnos;
	Bitmapset **surplusvars;
	ListCell   *lc;
	int			relid;

	/* 若 GROUP BY 项少于两项，就没有机会做任何事 */
	if (list_length(root->processed_groupClause) < 2)
		return;

	/* 若查询有分组集，就不要改动 GROUP BY 子句 */
	if (parse->groupingSets)
		return;

	/*
	 * 扫描 GROUP BY 子句，找出是简单 Var 的 GROUP BY 项。
	 * 用 RTE k 中作为 GROUP BY 项的列 attno 的位图集
	 * 填 groupbyattnos[k]。
	 */
	groupbyattnos = (Bitmapset **) palloc0(sizeof(Bitmapset *) *
										   (list_length(parse->rtable) + 1));
	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, parse->targetList);
		Var		   *var = (Var *) tle->expr;

		/*
		 * 忽略非 Var，以及来自其他查询层次的 Var。
		 *
		 * XXX 原则上，含 Var 的稳定表达式也可以
		 * 去掉，若全部 Var 都函数依赖于其他 GROUP
		 * BY 项。但不清楚这类情形是否常见到
		 * 值得费心。
		 */
		if (!IsA(var, Var) ||
			var->varlevelsup > 0)
			continue;

		/* 好，记住我们有这个 Var */
		relid = var->varno;
		Assert(relid <= list_length(parse->rtable));
		groupbyattnos[relid] = bms_add_member(groupbyattnos[relid],
											  var->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	/*
	 * 考虑每个关系，看能否从 GROUP BY 里去掉它的一些
	 * Var。为了简单和速度，实际删除
	 * 放在单独一趟。这里只把 RTE k 中可删除的 GROUP BY 项的列 attno 的位图集
	 * 填进 surplusvars[k]。
	 */
	surplusvars = NULL;			/* 除非需要，否则不分配数组 */
	relid = 0;
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
		Bitmapset  *relattnos;
		Bitmapset  *pkattnos;
		Oid			constraintOid;

		relid++;

		/* 只有普通关系才可能有主键约束 */
		if (rte->rtekind != RTE_RELATION)
			continue;

		/*
		 * 必须跳过继承父表，因为有些子关系
		 * 也许会造成重复行。不过分区
		 * 表不会发生这种情况。
		 */
		if (rte->inh && rte->relkind != RELKIND_PARTITIONED_TABLE)
			continue;

		/* 除非该关系在 GROUP BY 里有多个 Var，否则无事可做 */
		relattnos = groupbyattnos[relid];
		if (bms_membership(relattnos) != BMS_MULTIPLE)
			continue;

		/*
		 * 若没有合适的（即不可延迟的）主键约束，
		 * 就不能去掉该关系的任何列。
		 */
		pkattnos = get_primary_key_attnos(rte->relid, false, &constraintOid);
		if (pkattnos == NULL)
			continue;

		/*
		 * 若主键是 relattnos 的真子集，则
		 * GROUP BY 里有些项可以去掉。
		 */
		if (bms_subset_compare(pkattnos, relattnos) == BMS_SUBSET1)
		{
			/*
			 * 为了容易记住是否找到了要做的事，在找到东西之前
			 * 不分配 surplusvars[] 数组。
			 */
			if (surplusvars == NULL)
				surplusvars = (Bitmapset **) palloc0(sizeof(Bitmapset *) *
													 (list_length(parse->rtable) + 1));

			/* 记住可删除列的 attno */
			surplusvars[relid] = bms_difference(relattnos, pkattnos);
		}
	}

	/*
	 * 若找到了任何多余的 Var，就建一份不含它们的新 GROUP BY 子句。
	 * （注意：这也许会留下一些 ressortgroupref
	 * 标记未被引用的 TLEs，但那无害。）
	 */
	if (surplusvars != NULL)
	{
		List	   *new_groupby = NIL;

		foreach(lc, root->processed_groupClause)
		{
			SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
			TargetEntry *tle = get_sortgroupclause_tle(sgc, parse->targetList);
			Var		   *var = (Var *) tle->expr;

			/*
			 * 新列表必须包括非 Var、外层 Var，以及任何没
			 * 标成多余的东西。
			 */
			if (!IsA(var, Var) ||
				var->varlevelsup > 0 ||
				!bms_is_member(var->varattno - FirstLowInvalidHeapAttributeNumber,
							   surplusvars[var->varno]))
				new_groupby = lappend(new_groupby, sgc);
		}

		root->processed_groupClause = new_groupby;
	}
}

/*
 * preprocess_groupclause - 对 GROUP BY 子句做准备工作
 *
 * 这里的想法是调整 GROUP BY 元素的顺序
 * （这本身在语义上无关紧要），使之匹配 ORDER BY，
 * 从而允许一次排序既实现 ORDER BY 要求，又为实现 GROUP BY 的 Unique 步骤做好准备。
 * 原则上也许有兴趣考虑 GROUP BY 元素的其他顺序，
 *
 * 它们可以匹配其他
 * 可能计划的排序（例如索引扫描），从而降低代价。我们
 * 不费这个心。哈希分组反正经常会赢。
 * 注意：distinctClause 不需要可比的处理，因为
 *
 * 解析器已经强制它匹配 ORDER BY。
 * 注意：我们返回一份新 List，但其元素是出现在
 *
 * parse->groupClause 里的同一批
 * SortGroupClauses。这很重要，
 * 因为以后的处理也许会修改 processed_groupClause 列表。
 *
 * 对分组集，项的顺序改为强制与
 * 分组集一致（不在分组集里的项被跳过）。把
 * 分组集元素的顺序排成尽可能匹配 ORDER BY 的
 * 工作在别处做。
 */
static List *
preprocess_groupclause(PlannerInfo *root, List *force)
{
	Query	   *parse = root->parse;
	List	   *new_groupclause = NIL;
	bool		partial_match;
	ListCell   *sl;
	ListCell   *gl;

	/* 对分组集，需要强制顺序 */
	if (force)
	{
		foreach(sl, force)
		{
			Index		ref = lfirst_int(sl);
			SortGroupClause *cl = get_sortgroupref_clause(ref, parse->groupClause);

			new_groupclause = lappend(new_groupclause, cl);
		}

		return new_groupclause;
	}

	/* 若没有 ORDER BY，这里没什么有用的可做 */
	if (parse->sortClause == NIL)
		return list_copy(parse->groupClause);

	/*
	 * 扫描 ORDER BY 子句，构造一份匹配的 GROUP BY
	 * 项列表，但只做到能构成匹配前缀为止。
	 *
	 * 这段代码假定 sortClause 不含重复项。
	 */
	foreach(sl, parse->sortClause)
	{
		SortGroupClause *sc = lfirst_node(SortGroupClause, sl);

		foreach(gl, parse->groupClause)
		{
			SortGroupClause *gc = lfirst_node(SortGroupClause, gl);

			if (equal(gc, sc))
			{
				new_groupclause = lappend(new_groupclause, gc);
				break;
			}
		}
		if (gl == NULL)
			break;				/* 没有匹配，因此停止扫描 */
	}

	/* 是匹配了全部 ORDER BY 列表，还是只匹配了一部分？ */
	partial_match = (sl != NULL);

	/* 若完全没有匹配，重排 GROUP BY 就没有意义 */
	if (new_groupclause == NIL)
		return list_copy(parse->groupClause);

	/*
	 * 把任何剩余的 GROUP BY 项加进新列表，但仅当我们
	 * 能做成完整匹配时。换句话说，只有当结果是一个列表是另一个的前缀时，
	 * 才重排
	 * GROUP BY 列表——否则不可能有共同排序。另外，若有任何不可排序的 GROUP BY 项就放弃，
	 * 因为那时
	 * 反正没有希望。
	 */
	foreach(gl, parse->groupClause)
	{
		SortGroupClause *gc = lfirst_node(SortGroupClause, gl);

		if (list_member_ptr(new_groupclause, gc))
			continue;			/* 它匹配了一个 ORDER BY 项 */
		if (partial_match)		/* 放弃，不可能有共同排序 */
			return list_copy(parse->groupClause);
		if (!OidIsValid(gc->sortop))	/* 放弃，GROUP BY 无法排序 */
			return list_copy(parse->groupClause);
		new_groupclause = lappend(new_groupclause, gc);
	}

	/* 成功——装上重排后的 GROUP BY 列表 */
	Assert(list_length(parse->groupClause) == list_length(new_groupclause));
	return new_groupclause;
}

/*
 * 抽出可以用单趟
 * rollup 型聚合分别实现的分组集列表。返回分组集列表的列表。
 *
 * 输入必须按最小集合在前排序。结果的每个子列表
 * 都按最小集合在前排序。
 *
 * 我们想在这里产生绝对最少的列表数，以免
 * 多余的排序。幸好有一个算法；把偏序集划分成最少条链
 * 的问题
 * （这正是我们需要的，把分组集列表看成按集合包含排序的偏序集）
 * 可以映射成在二分图上找最大
 * 基数匹配的问题，它在多项式
 * 时间内可解，最坏不差于 O(n^2.5)，通常好得多。
 * 由于我们的 N 最多 4096，不必考虑退回到
 * 启发式或近似方法。（即使关掉优化并打开断言，12 维立方体的规划时间在我这台普通机器上也
 * 不到
 * 半秒。）
 */
static List *
extract_rollup_sets(List *groupingSets)
{
	int			num_sets_raw = list_length(groupingSets);
	int			num_empty = 0;
	int			num_sets = 0;	/* 互异的集合 */
	int			num_chains = 0;
	List	   *result = NIL;
	List	  **results;
	List	  **orig_sets;
	Bitmapset **set_masks;
	int		   *chains;
	short	  **adjacency;
	short	   *adjacency_buf;
	BipartiteMatchState *state;
	int			i;
	int			j;
	int			j_size;
	ListCell   *lc1 = list_head(groupingSets);
	ListCell   *lc;

	/*
	 * 先剥掉空集。算法并不需要这个，
	 * 但规划器目前需要全部空集都放回
	 * 第一个列表，因此在这里剥掉，之后再加回去。
	 */
	while (lc1 && lfirst(lc1) == NIL)
	{
		++num_empty;
		lc1 = lnext(groupingSets, lc1);
	}

	/* 若结果发现我们只有空集，现在就退出。 */
	if (!lc1)
		return list_make1(groupingSets);

	/*----------
	 * We don't strictly need to remove duplicate sets here, but if we don't,
	 * they tend to become scattered through the result, which is a bit
	 * confusing (and irritating if we ever decide to optimize them out).
	 * So we remove them here and add them back after.
	 *
	 * For each non-duplicate set, we fill in the following:
	 *
	 * orig_sets[i] = list of the original set lists
	 * set_masks[i] = bitmapset for testing inclusion
	 * adjacency[i] = array [n, v1, v2, ... vn] of adjacency indices
	 *
	 * chains[i] will be the result group this set is assigned to.
	 *
	 * We index all of these from 1 rather than 0 because it is convenient
	 * to leave 0 free for the NIL node in the graph algorithm.
	 *----------
	 */
	orig_sets = palloc0((num_sets_raw + 1) * sizeof(List *));
	set_masks = palloc0((num_sets_raw + 1) * sizeof(Bitmapset *));
	adjacency = palloc0((num_sets_raw + 1) * sizeof(short *));
	adjacency_buf = palloc((num_sets_raw + 1) * sizeof(short));

	j_size = 0;
	j = 0;
	i = 1;

	for_each_cell(lc, groupingSets, lc1)
	{
		List	   *candidate = (List *) lfirst(lc);
		Bitmapset  *candidate_set = NULL;
		ListCell   *lc2;
		int			dup_of = 0;

		foreach(lc2, candidate)
		{
			candidate_set = bms_add_member(candidate_set, lfirst_int(lc2));
		}

		/* 只有与先前某个集合等长时，我们才可能是重复 */
		if (j_size == list_length(candidate))
		{
			int			k;

			for (k = j; k < i; ++k)
			{
				if (bms_equal(set_masks[k], candidate_set))
				{
					dup_of = k;
					break;
				}
			}
		}
		else if (j_size < list_length(candidate))
		{
			j_size = list_length(candidate);
			j = i;
		}

		if (dup_of > 0)
		{
			orig_sets[dup_of] = lappend(orig_sets[dup_of], candidate);
			bms_free(candidate_set);
		}
		else
		{
			int			k;
			int			n_adj = 0;

			orig_sets[i] = list_make1(candidate);
			set_masks[i] = candidate_set;

			/* 填邻接表；不必比较等大小的集合 */

			for (k = j - 1; k > 0; --k)
			{
				if (bms_is_subset(set_masks[k], candidate_set))
					adjacency_buf[++n_adj] = k;
			}

			if (n_adj > 0)
			{
				adjacency_buf[0] = n_adj;
				adjacency[i] = palloc((n_adj + 1) * sizeof(short));
				memcpy(adjacency[i], adjacency_buf, (n_adj + 1) * sizeof(short));
			}
			else
				adjacency[i] = NULL;

			++i;
		}
	}

	num_sets = i - 1;

	/*
	 * 应用图匹配算法来做这项工作。
	 */
	state = BipartiteMatch(num_sets, num_sets, adjacency);

	/*
	 * 现在，state->pair* 字段有把集合分配到
	 * 链所需的信息。若 pair_uv[u] = v 或
	 * pair_vu[v] = u，则两个集合 (u,v) 属于同一条链（两者都会为真，但我们两个都查，以便
	 * 一趟做完）
	 */
	chains = palloc0((num_sets + 1) * sizeof(int));

	for (i = 1; i <= num_sets; ++i)
	{
		int			u = state->pair_vu[i];
		int			v = state->pair_uv[i];

		if (u > 0 && u < i)
			chains[i] = chains[u];
		else if (v > 0 && v < i)
			chains[i] = chains[v];
		else
			chains[i] = ++num_chains;
	}

	/* 建立结果列表。 */
	results = palloc0((num_chains + 1) * sizeof(List *));

	for (i = 1; i <= num_sets; ++i)
	{
		int			c = chains[i];

		Assert(c > 0);

		results[c] = list_concat(results[c], orig_sets[i]);
	}

	/* 把任何空集推回第一个列表。 */
	while (num_empty-- > 0)
		results[1] = lcons(NIL, results[1]);

	/* 做结果列表 */
	for (i = 1; i <= num_chains; ++i)
		result = lappend(result, results[i]);

	/*
	 * 把这些东西都释放掉。
	 *
	 * （对小集合这过于讲究，但对大集合我们也许
	 * 占用了不少内存。）
	 */
	BipartiteMatchFree(state);
	pfree(results);
	pfree(chains);
	for (i = 1; i <= num_sets; ++i)
		if (adjacency[i])
			pfree(adjacency[i]);
	pfree(adjacency);
	pfree(adjacency_buf);
	pfree(orig_sets);
	for (i = 1; i <= num_sets; ++i)
		bms_free(set_masks[i]);
	pfree(set_masks);

	return result;
}

/*
 * 重排分组集列表的元素，使它们有正确的
 * 前缀关系。也插入 GroupingSetData 标注。
 *
 * 输入必须按最小集合在前排序；结果按
 * 最大集合在前返回。注意结果与输入不共享任何列表子结构，
 * 因此调用方以后修改它是安全的。
 *
 * 若传入一份 sortclause，就尽可能遵循它的列顺序，
 * 以尽量减少加入不必要排序的机会。
 * （这里试图确保 GROUPING SETS ((a,b,c),(c)) ORDER BY c,b,a
 * 用一趟实现。）
 */
static List *
reorder_grouping_sets(List *groupingSets, List *sortclause)
{
	ListCell   *lc;
	List	   *previous = NIL;
	List	   *result = NIL;

	foreach(lc, groupingSets)
	{
		List	   *candidate = (List *) lfirst(lc);
		List	   *new_elems = list_difference_int(candidate, previous);
		GroupingSetData *gs = makeNode(GroupingSetData);

		while (list_length(sortclause) > list_length(previous) &&
			   new_elems != NIL)
		{
			SortGroupClause *sc = list_nth(sortclause, list_length(previous));
			int			ref = sc->tleSortGroupRef;

			if (list_member_int(new_elems, ref))
			{
				previous = lappend_int(previous, ref);
				new_elems = list_delete_int(new_elems, ref);
			}
			else
			{
				/* 与 sortclause 分叉了；对它放弃 */
				sortclause = NIL;
				break;
			}
		}

		previous = list_concat(previous, new_elems);

		gs->set = list_copy(previous);
		result = lcons(gs, result);
	}

	list_free(previous);

	return result;
}

/*
 * has_volatile_pathkey
 *		若 'keys' 里任何 PathKey 的 EquivalenceClass
 *		含有易变函数，则返回真。否则返回假。
 */
static bool
has_volatile_pathkey(List *keys)
{
	ListCell   *lc;

	foreach(lc, keys)
	{
		PathKey    *pathkey = lfirst_node(PathKey, lc);

		if (pathkey->pk_eclass->ec_has_volatile)
			return true;
	}

	return false;
}

/*
 * adjust_group_pathkeys_for_groupagg
 *		向 root->group_pathkeys 添加路径键，以反映有序聚合的最佳
 *		预排序输入集合。
 *
 * 我们把"最佳"定义为适合最多
 * 聚合函数的路径键。找法是看第一个 ORDER BY /
 * DISTINCT 聚合，先取它的路径键，再搜索
 * 需要同样路径键、或其更严格变体的
 * 其他聚合。然后对任何剩余的、
 * 路径键不同的聚合重复该过程；若找到另一组适合
 * 更多聚合的路径键，就改选那些路径键。
 *
 * 找到最佳路径键时，也把每个能使用
 * 那些路径键的 Aggref 标成 aggpresorted = true。
 *
 * 注意：当聚合函数的 ORDER BY / DISTINCT 子句含有任何
 * 易变函数时，我们从不使用这些路径键。我们想确保
 * 使用易变函数的排序在每个 Aggref 里独立完成，
 * 而不是在查询层次做一次。若允许那样，则排序顺序兼容的 Aggref
 * 会在那些路径键被认定为最佳排序路径键时，全部以同样
 * 顺序转换其行。
 * 而若另一些 Aggref 的路径键碰巧被认定
 * 为更好的排序路径键，则带易变函数的 Aggref 就会
 * 被留下各自做排序。为避免这种不一致的
 * 行为——它会使 Aggref 结果取决于查询还含有哪些其他 Aggref——
 * 我们总是强制带易变函数的 Aggref 自己
 * 做排序。
 */
static void
adjust_group_pathkeys_for_groupagg(PlannerInfo *root)
{
	List	   *grouppathkeys = root->group_pathkeys;
	List	   *bestpathkeys;
	Bitmapset  *bestaggs;
	Bitmapset  *unprocessed_aggs;
	ListCell   *lc;
	int			i;

	/* 若有分组集，就不应当到这里 */
	Assert(root->parse->groupingSets == NIL);
	/* 除非有一些有序聚合，否则不应当到这里 */
	Assert(root->numOrderedAggs > 0);

	/* 若被禁用，就什么都不做 */
	if (!enable_presorted_aggregate)
		return;

	/*
	 * 先扫一遍全部 AggInfos，收集一份含有
	 * 下面要处理的全部 AggInfo 下标的 Bitmapset。
	 */
	unprocessed_aggs = NULL;
	foreach(lc, root->agginfos)
	{
		AggInfo    *agginfo = lfirst_node(AggInfo, lc);
		Aggref	   *aggref = linitial_node(Aggref, agginfo->aggrefs);

		if (AGGKIND_IS_ORDERED_SET(aggref->aggkind))
			continue;

		/* 除非有 DISTINCT 或 ORDER BY 子句，否则跳过 */
		if (aggref->aggdistinct == NIL && aggref->aggorder == NIL)
			continue;

		/* 若有 FILTER 子句，就需要额外的安全检查 */
		if (aggref->aggfilter != NULL)
		{
			ListCell   *lc2;
			bool		allow_presort = true;

			/*
			 * 当 Aggref 有 FILTER 子句时，
			 * 过滤也许会去掉那些无法排序的行，因为
			 * 排序所依据的表达式在
			 * 求值时会出错。这对预排序是个问题，因为预排序发生在
			 * FILTER 之前，而没有预排序时，Aggregate
			 * 节点会在排序*之前*应用 FILTER。为了绝不
			 * 去排序任何也许会出错的东西，这里我们打算跳过
			 * 任何参数表达式在
			 * 求值时也许会造成 ERROR 的 Aggref。Var 和 Const 没问题。也许
			 * 还有更多应当允许的情形，但还需要更多
			 * 考虑。宁可谨慎。
			 */
			foreach(lc2, aggref->args)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(lc2);
				Expr	   *expr = tle->expr;

				while (IsA(expr, RelabelType))
					expr = (Expr *) (castNode(RelabelType, expr))->arg;

				/* 常见情形，Var 和 Const 没问题 */
				if (IsA(expr, Var) || IsA(expr, Const))
					continue;

				/* 不支持。不要为这个 Aggref 尝试预排序 */
				allow_presort = false;
				break;
			}

			/* 跳过不支持的 Aggref */
			if (!allow_presort)
				continue;
		}

		unprocessed_aggs = bms_add_member(unprocessed_aggs,
										  foreach_current_index(lc));
	}

	/*
	 * 现在处理全部 unprocessed_aggs，为给定的聚合集合找出最佳路径键
	 * 集合。
	 *
	 * 这里第一次外层循环时 'bestaggs' 为空。我们会在
	 * 第一次循环里用第一个
	 * AggInfo 的路径键填充它，再从任何路径键集合更严格且兼容的其他 AggInfos 取更强的路径键。
	 * 外层循环
	 * 完成后，标出全部路径键兼容的聚合，然后
	 * 把它们从 unprocessed_aggs 里去掉，并重复该过程，试着
	 * 再找一组适合更多
	 * 聚合的路径键。当剩余的
	 * 未处理聚合不够多、不可能找到一组
	 * 适合更多聚合的路径键时，外层循环停止。
	 */
	bestpathkeys = NIL;
	bestaggs = NULL;
	while (bms_num_members(unprocessed_aggs) > bms_num_members(bestaggs))
	{
		Bitmapset  *aggindexes = NULL;
		List	   *currpathkeys = NIL;

		i = -1;
		while ((i = bms_next_member(unprocessed_aggs, i)) >= 0)
		{
			AggInfo    *agginfo = list_nth_node(AggInfo, root->agginfos, i);
			Aggref	   *aggref = linitial_node(Aggref, agginfo->aggrefs);
			List	   *sortlist;
			List	   *pathkeys;

			if (aggref->aggdistinct != NIL)
				sortlist = aggref->aggdistinct;
			else
				sortlist = aggref->aggorder;

			pathkeys = make_pathkeys_for_sortclauses(root, sortlist,
													 aggref->args);

			/*
			 * 忽略 ORDER BY
			 * 或 DISTINCT 子句里有易变函数的 Aggref。
			 */
			if (has_volatile_pathkey(pathkeys))
			{
				unprocessed_aggs = bms_del_member(unprocessed_aggs, i);
				continue;
			}

			/*
			 * 尚未设置时，从第一个未处理的
			 * 聚合取路径键。
			 */
			if (currpathkeys == NIL)
			{
				currpathkeys = pathkeys;

				/* 若存在，就把 GROUP BY 路径键包括进去 */
				if (grouppathkeys != NIL)
					currpathkeys = append_pathkeys(list_copy(grouppathkeys),
												   currpathkeys);

				/* 记下我们为这个聚合找到了路径键 */
				aggindexes = bms_add_member(aggindexes, i);
			}
			else
			{
				/* 现在寻找一组更强的匹配路径键 */

				/* 若存在，就把 GROUP BY 路径键包括进去 */
				if (grouppathkeys != NIL)
					pathkeys = append_pathkeys(list_copy(grouppathkeys),
											   pathkeys);

				/* 'pathkeys' 与 'currpathkeys' 兼容，还是更好？ */
				switch (compare_pathkeys(currpathkeys, pathkeys))
				{
					case PATHKEYS_BETTER2:
						/* 'pathkeys' 更强，改用这些 */
						currpathkeys = pathkeys;
						/* FALLTHROUGH */

					case PATHKEYS_BETTER1:
						/* 'pathkeys' 没那么严格 */
						/* FALLTHROUGH */

					case PATHKEYS_EQUAL:
						/* 把这个聚合标成已被 'currpathkeys' 覆盖 */
						aggindexes = bms_add_member(aggindexes, i);
						break;

					case PATHKEYS_DIFFERENT:
						break;
				}
			}
		}

		/* 去掉刚刚处理过的聚合 */
		unprocessed_aggs = bms_del_members(unprocessed_aggs, aggindexes);

		/*
		 * 若这一趟包含的聚合比先前的最佳还多，就
		 * 用这些作为最佳集合。
		 */
		if (bms_num_members(aggindexes) > bms_num_members(bestaggs))
		{
			bestaggs = aggindexes;
			bestpathkeys = currpathkeys;
		}
	}

	/*
	 * 若找到了任何有序聚合，就更新 root->group_pathkeys，加上
	 * 最佳的聚合路径键集合。注意 bestpathkeys 已经包括
	 * 原来的 GROUP BY 路径键。
	 */
	if (bestpathkeys != NIL)
		root->group_pathkeys = bestpathkeys;

	/*
	 * 既然找到了最佳聚合集合，就可以设置
	 * 预排序标志，告诉执行器不必费心
	 * 为这些 Aggref 做排序。现在可以这么做，因为
	 * 不可能有 Hash Aggregate 计划：create_grouping_paths
	 * 会因存在
	 * 有序聚合而不把 GROUP BY 标成 GROUPING_CAN_USE_HASH。
	 */
	i = -1;
	while ((i = bms_next_member(bestaggs, i)) >= 0)
	{
		AggInfo    *agginfo = list_nth_node(AggInfo, root->agginfos, i);

		foreach(lc, agginfo->aggrefs)
		{
			Aggref	   *aggref = lfirst_node(Aggref, lc);

			aggref->aggpresorted = true;
		}
	}
}

/*
 * 在计划生成期间计算 query_pathkeys 和其他路径键
 */
static void
standard_qp_callback(PlannerInfo *root, void *extra)
{
	Query	   *parse = root->parse;
	standard_qp_extra *qp_extra = (standard_qp_extra *) extra;
	List	   *tlist = root->processed_tlist;
	List	   *activeWindows = qp_extra->activeWindows;

	/*
	 * 计算表示分组/排序和/或有序
	 * 聚合要求的路径键。
	 */
	if (qp_extra->gset_data)
	{
		/*
		 * 有分组集时，只用第一个 RollupData 的 groupClause。我们
		 * 有分组集时并不费力去优化分组子句，也
		 * 没法把聚合排序键与
		 * 分组合并。
		 */
		List	   *rollups = qp_extra->gset_data->rollups;
		List	   *groupClause = (rollups ? linitial_node(RollupData, rollups)->groupClause : NIL);

		if (grouping_is_sortable(groupClause))
		{
			root->group_pathkeys = make_pathkeys_for_sortclauses(root,
																 groupClause,
																 tlist);
			root->num_groupby_pathkeys = list_length(root->group_pathkeys);
		}
		else
		{
			root->group_pathkeys = NIL;
			root->num_groupby_pathkeys = 0;
		}
	}
	else if (parse->groupClause || root->numOrderedAggs > 0)
	{
		/*
		 * 对普通 GROUP BY 列表，可以去掉任何被 EquivalenceClass 处理证明冗余的分组项。
		 * 例如，
		 * 给定 "WHERE x = y GROUP BY x, y"，可以去掉 y。这些并不是
		 * 特别常见的情形，但检测几乎不花代价。注意
		 * 我们从 processed_groupClause 去掉冗余项，但不从
		 * 原来的 parse->groupClause 去掉。
		 */
		bool		sortable;

		root->group_pathkeys =
			make_pathkeys_for_sortclauses_extended(root,
												   &root->processed_groupClause,
												   tlist,
												   true,
												   &sortable);
		if (!sortable)
		{
			/* 无法排序；考虑聚合排序也没有意义 */
			root->group_pathkeys = NIL;
			root->num_groupby_pathkeys = 0;
		}
		else
		{
			root->num_groupby_pathkeys = list_length(root->group_pathkeys);
			/* 若有有序聚合，考虑加到 group_pathkeys 上 */
			if (root->numOrderedAggs > 0)
				adjust_group_pathkeys_for_groupagg(root);
		}
	}
	else
	{
		root->group_pathkeys = NIL;
		root->num_groupby_pathkeys = 0;
	}

	/* 在路径键逻辑里，我们只考虑第一个（最底层）窗口 */
	if (activeWindows != NIL)
	{
		WindowClause *wc = linitial_node(WindowClause, activeWindows);

		root->window_pathkeys = make_pathkeys_for_window(root,
														 wc,
														 tlist);
	}
	else
		root->window_pathkeys = NIL;

	/*
	 * 与 GROUP BY 一样，可以丢掉任何被 EquivalenceClass 处理证明
	 * 冗余的 DISTINCT 项。非冗余列表保存在
	 * root->processed_distinctClause 里，原来的
	 * parse->distinctClause 不动。
	 */
	if (parse->distinctClause)
	{
		bool		sortable;

		/* 做一份副本，因为路径键处理也许会修改该列表 */
		root->processed_distinctClause = list_copy(parse->distinctClause);
		root->distinct_pathkeys =
			make_pathkeys_for_sortclauses_extended(root,
												   &root->processed_distinctClause,
												   tlist,
												   true,
												   &sortable);
		if (!sortable)
			root->distinct_pathkeys = NIL;
	}
	else
		root->distinct_pathkeys = NIL;

	root->sort_pathkeys =
		make_pathkeys_for_sortclauses(root,
									  parse->sortClause,
									  tlist);

	/*
	 * 弄清是否想要 query_planner 给出排序后的结果。
	 *
	 * 若有可排序的 GROUP BY 子句，就想要为分组
	 * 正确排好序的结果。否则，若有窗口函数要
	 * 求值，就试着为第一个窗口排序。否则，若有比 ORDER BY 子句更严格的
	 * 可排序 DISTINCT 子句，
	 * 就试着产生对
	 * DISTINCT 排序得足够好的输出。否则，若有 ORDER BY 子句，就想按
	 * ORDER BY 子句排序。
	 *
	 * 注意：若同时有 ORDER BY 和 GROUP BY，且 ORDER BY 是 GROUP BY 的超集，
	 * 也许会想请求按 ORDER BY 排序——但
	 * 那也许只是让我们完全无法利用已有的排序。
	 * 还需要更多考虑。DISTINCT 相对 ORDER BY 的选择
	 * 容易得多，因为我们知道解析器已确保其中一个是
	 * 另一个的超集。
	 */
	if (root->group_pathkeys)
		root->query_pathkeys = root->group_pathkeys;
	else if (root->window_pathkeys)
		root->query_pathkeys = root->window_pathkeys;
	else if (list_length(root->distinct_pathkeys) >
			 list_length(root->sort_pathkeys))
		root->query_pathkeys = root->distinct_pathkeys;
	else if (root->sort_pathkeys)
		root->query_pathkeys = root->sort_pathkeys;
	else
		root->query_pathkeys = NIL;
}

/*
 * 估计分组子句产生的组数（若不分组则为 1）
 *
 * path_rows：扫描/连接步骤的输出行数
 * gd：分组集数据，包括分组集及其子句的列表
 * target_list：含有分组子句引用的目标列表
 *
 * 若在做分组集，也给 gsets 数据标注每个集合和
 * 每个单独 rollup 列表的估计，以便以后
 * 判断它们的某种组合是否可以改为哈希。
 */
static double
get_number_of_groups(PlannerInfo *root,
					 double path_rows,
					 grouping_sets_data *gd,
					 List *target_list)
{
	Query	   *parse = root->parse;
	double		dNumGroups;

	if (parse->groupClause)
	{
		List	   *groupExprs;

		if (parse->groupingSets)
		{
			/* 把每个分组集的估计加起来 */
			ListCell   *lc;

			Assert(gd);			/* 消除 Coverity 告警 */

			dNumGroups = 0;

			foreach(lc, gd->rollups)
			{
				RollupData *rollup = lfirst_node(RollupData, lc);
				ListCell   *lc2;
				ListCell   *lc3;

				groupExprs = get_sortgrouplist_exprs(rollup->groupClause,
													 target_list);

				rollup->numGroups = 0.0;

				forboth(lc2, rollup->gsets, lc3, rollup->gsets_data)
				{
					List	   *gset = (List *) lfirst(lc2);
					GroupingSetData *gs = lfirst_node(GroupingSetData, lc3);
					double		numGroups = estimate_num_groups(root,
																groupExprs,
																path_rows,
																&gset,
																NULL);

					gs->numGroups = numGroups;
					rollup->numGroups += numGroups;
				}

				dNumGroups += rollup->numGroups;
			}

			if (gd->hash_sets_idx)
			{
				ListCell   *lc2;

				gd->dNumHashGroups = 0;

				groupExprs = get_sortgrouplist_exprs(parse->groupClause,
													 target_list);

				forboth(lc, gd->hash_sets_idx, lc2, gd->unsortable_sets)
				{
					List	   *gset = (List *) lfirst(lc);
					GroupingSetData *gs = lfirst_node(GroupingSetData, lc2);
					double		numGroups = estimate_num_groups(root,
																groupExprs,
																path_rows,
																&gset,
																NULL);

					gs->numGroups = numGroups;
					gd->dNumHashGroups += numGroups;
				}

				dNumGroups += gd->dNumHashGroups;
			}
		}
		else
		{
			/* 普通 GROUP BY——根据优化后的 groupClause 估计 */
			groupExprs = get_sortgrouplist_exprs(root->processed_groupClause,
												 target_list);

			dNumGroups = estimate_num_groups(root, groupExprs, path_rows,
											 NULL, NULL);
		}
	}
	else if (parse->groupingSets)
	{
		/* 空分组集……每个产生一行结果 */
		dNumGroups = list_length(parse->groupingSets);
	}
	else if (parse->hasAggs || root->hasHavingQual)
	{
		/* 普通聚合，一行结果 */
		dNumGroups = 1;
	}
	else
	{
		/* 不分组 */
		dNumGroups = 1;
	}

	return dNumGroups;
}

/*
 * create_grouping_paths
 *
 * 建立一个新的上层关系，含有分组和/或聚合的 Path。
 * 沿途也建立一个上层关系，给部分
 * 分组和/或部分聚合的 Path。部分分组和/或部分聚合的路径
 * 还得有一个 FinalizeAggregate 节点才能完成聚合。目前，
 * 我们建立的部分分组路径也都是部分路径；也就是说，
 * 它们还得有 Gather，然后再有 FinalizeAggregate。
 *
 * input_rel：含有源数据 Path
 * target：结果 Path 要计算的 pathtarget
 * gd：分组集数据，包括分组集及其子句的列表
 *
 * 注意：input_rel 里的全部 Path 都预期返回
 * make_group_input_target 算出的目标。
 */
static RelOptInfo *
create_grouping_paths(PlannerInfo *root,
					  RelOptInfo *input_rel,
					  PathTarget *target,
					  bool target_parallel_safe,
					  grouping_sets_data *gd)
{
	Query	   *parse = root->parse;
	RelOptInfo *grouped_rel;
	RelOptInfo *partially_grouped_rel;
	AggClauseCosts agg_costs;

	MemSet(&agg_costs, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &agg_costs);

	/*
	 * 建立分组关系，用来存放完全聚合的分组和/或
	 * 聚合路径。
	 */
	grouped_rel = make_grouping_rel(root, input_rel, target,
									target_parallel_safe, parse->havingQual);

	/*
	 * 按情况建立退化分组的路径，或普通
	 * 分组的路径。
	 */
	if (is_degenerate_grouping(root))
		create_degenerate_grouping_paths(root, input_rel, grouped_rel);
	else
	{
		int			flags = 0;
		GroupPathExtraData extra;

		/*
		 * 判断是否可能做基于排序的
		 * 分组实现。（注意若 processed_groupClause
		 * 为空，grouping_is_sortable() 平凡为真，且全部
		 * pathkeys_contained_in() 测试也会成功，因此我们会
		 * 考虑每条幸存的输入路径。）
		 *
		 * 若有分组集，也许能对其中一些排序，但不能对全部
		 * 排序；这种情况下，只要必须
		 * 考虑任何排序输入计划，就需要 can_sort 为真。
		 */
		if ((gd && gd->rollups != NIL)
			|| grouping_is_sortable(root->processed_groupClause))
			flags |= GROUPING_CAN_USE_SORT;

		/*
		 * 判断是否应当考虑基于哈希的
		 * 分组实现。
		 *
		 * 哈希聚合只在我们正在分组时适用。若有
		 * 分组集，有些组也许可哈希，另一些不行；这种
		 * 情况下，只要没有全局性的东西
		 * 阻止我们哈希（因此应当考虑带
		 * 哈希的计划），就把 can_hash 设为真。
		 *
		 * 执行器不支持带 DISTINCT 或 ORDER
		 * BY 聚合的哈希聚合。（那样做意味着把*全部*输入
		 * 值存进哈希表，和/或并行跑许多排序，
		 * 两者看起来都必输。）同样，哈希聚合里也不
		 * 支持有序集聚合，但那种情形
		 * 也算在 numOrderedAggs 计数里。
		 *
		 * 注意：grouping_is_hashable() 比
		 * 其他门控条件贵得多，因此想把它放在最后。
		 */
		if ((parse->groupClause != NIL &&
			 root->numOrderedAggs == 0 &&
			 (gd ? gd->any_hashable : grouping_is_hashable(root->processed_groupClause))))
			flags |= GROUPING_CAN_USE_HASH;

		/*
		 * 判断部分聚合是否可能。
		 */
		if (can_partial_agg(root))
			flags |= GROUPING_CAN_PARTIAL_AGG;

		extra.flags = flags;
		extra.target_parallel_safe = target_parallel_safe;
		extra.havingQual = parse->havingQual;
		extra.targetList = parse->targetList;
		extra.partial_costs_set = false;

		/*
		 * 判断按分区聚合在理论上是否可能。
		 * 用户可以禁用它，而且目前我们不试图
		 * 支持分组集。create_ordinary_grouping_paths() 会检查
		 * 额外条件，例如 input_rel 是否已分区。
		 */
		if (enable_partitionwise_aggregate && !parse->groupingSets)
			extra.patype = PARTITIONWISE_AGGREGATE_FULL;
		else
			extra.patype = PARTITIONWISE_AGGREGATE_NONE;

		create_ordinary_grouping_paths(root, input_rel, grouped_rel,
									   &agg_costs, gd, &extra,
									   &partially_grouped_rel);
	}

	set_cheapest(grouped_rel);
	return grouped_rel;
}

/*
 * make_grouping_rel
 *
 * 建立一个新的分组关系并设置基本属性。
 *
 * input_rel 表示底层的扫描/连接关系。
 * target 是该分组关系预期的输出。
 */
static RelOptInfo *
make_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
				  PathTarget *target, bool target_parallel_safe,
				  Node *havingQual)
{
	RelOptInfo *grouped_rel;

	if (IS_OTHER_REL(input_rel))
	{
		grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG,
									  input_rel->relids);
		grouped_rel->reloptkind = RELOPT_OTHER_UPPER_REL;
	}
	else
	{
		/*
		 * 按传统，主分组关系的 relids 集合是
		 * NULL。（这可以改，但也许需要在
		 * 别处做调整。）
		 */
		grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG, NULL);
	}

	/* 设置目标。 */
	grouped_rel->reltarget = target;

	/*
	 * 若输入关系不是并行安全的，则分组后的关系
	 * 也不可能是并行安全的。否则，若
	 * 目标列表和 HAVING 限定条件并行安全，它就是并行安全的。
	 */
	if (input_rel->consider_parallel && target_parallel_safe &&
		is_parallel_safe(root, (Node *) havingQual))
		grouped_rel->consider_parallel = true;

	/*
	 * 若输入关系属于单个 FDW，分组后的关系也是。
	 */
	grouped_rel->serverid = input_rel->serverid;
	grouped_rel->userid = input_rel->userid;
	grouped_rel->useridiscurrent = input_rel->useridiscurrent;
	grouped_rel->fdwroutine = input_rel->fdwroutine;

	return grouped_rel;
}

/*
 * is_degenerate_grouping
 *
 * 退化分组是指查询有 HAVING 限定条件和/或
 * 分组集，但没有聚合也没有 GROUP BY（这意味着
 * 分组集全是空的）。
 */
static bool
is_degenerate_grouping(PlannerInfo *root)
{
	Query	   *parse = root->parse;

	return (root->hasHavingQual || parse->groupingSets) &&
		!parse->hasAggs && parse->groupClause == NIL;
}

/*
 * create_degenerate_grouping_paths
 *
 * 当分组是退化的（见 is_degenerate_grouping）时，我们
 * 应当根据 HAVING 是否成功，为每个分组集发出零行或一行。
 * 此外，HAVING 或目标列表里都无法出现任何变量，
 * 因此实际上根本不需要 FROM 表！
 * 可以直接丢掉到目前为止的计划，生成一个 Result 节点。
 * 这是足够少见的边角情形，不值得为了避免一开始就生成
 * 那些更早的路径而扭曲
 * 本模块的结构。
 */
static void
create_degenerate_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
								 RelOptInfo *grouped_rel)
{
	Query	   *parse = root->parse;
	int			nrows;
	Path	   *path;

	nrows = list_length(parse->groupingSets);
	if (nrows > 1)
	{
		/*
		 * 似乎不值得写代码来拼出一个 generate_series
		 * 或 values 扫描以发出多行。改为做 N 份克隆
		 * 再把它们追加起来。（若 HAVING 子句易变，这意味着你
		 * 也许会得到 0 到 N 行输出。我一时认为这正是
		 * 想要的。）
		 */
		List	   *paths = NIL;

		while (--nrows >= 0)
		{
			path = (Path *)
				create_group_result_path(root, grouped_rel,
										 grouped_rel->reltarget,
										 (List *) parse->havingQual);
			paths = lappend(paths, path);
		}
		path = (Path *)
			create_append_path(root,
							   grouped_rel,
							   paths,
							   NIL,
							   NIL,
							   NULL,
							   0,
							   false,
							   -1);
	}
	else
	{
		/* 没有分组集，或只有一个，因此一行输出 */
		path = (Path *)
			create_group_result_path(root, grouped_rel,
									 grouped_rel->reltarget,
									 (List *) parse->havingQual);
	}

	add_path(grouped_rel, path);
}

/*
 * create_ordinary_grouping_paths
 *
 * 为普通（即非退化）情形建立分组路径。
 *
 * 需要在同一个函数里考虑排序聚合和哈希聚合，
 * 否则 (1) 若两种办法都不行，更难给出适当的错误
 * 消息，且 (2) 若排序不可能，不应当让哈希表大小
 * 方面的考虑劝阻我们使用哈希。
 *
 * *partially_grouped_rel_p 会被设成这个
 * 函数建立的部分分组关系，若没建立则为 NULL。
 */
static void
create_ordinary_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
							   RelOptInfo *grouped_rel,
							   const AggClauseCosts *agg_costs,
							   grouping_sets_data *gd,
							   GroupPathExtraData *extra,
							   RelOptInfo **partially_grouped_rel_p)
{
	Path	   *cheapest_path = input_rel->cheapest_total_path;
	RelOptInfo *partially_grouped_rel = NULL;
	double		dNumGroups;
	PartitionwiseAggregateType patype = PARTITIONWISE_AGGREGATE_NONE;

	/*
	 * 若这是最顶层的分组关系，或父关系正在
	 * 做某种形式的按分区聚合，则我们在这一层也许也能做。
	 * 不过，若输入关系没有
	 * 分区，按分区聚合就不可能。
	 */
	if (extra->patype != PARTITIONWISE_AGGREGATE_NONE &&
		IS_PARTITIONED_REL(input_rel))
	{
		/*
		 * 若这是最顶层关系，或父关系正在做
		 * 完整的按分区聚合，则只要 GROUP BY 子句包含本层全部
		 * 分区列，且 GROUP
		 * BY 使用的排序规则匹配分区排序规则，就可以做完整的按分区
		 * 聚合。否则，最多只能做
		 * 部分按分区聚合。但若一般不支持部分聚合，
		 * 就不能把它用于按分区
		 * 聚合。
		 *
		 * 检查 parse->groupClause 而不是 processed_groupClause，因为
		 * 即使某些分区列被证明冗余也没关系。
		 */
		if (extra->patype == PARTITIONWISE_AGGREGATE_FULL &&
			group_by_has_partkey(input_rel, extra->targetList,
								 root->parse->groupClause))
			patype = PARTITIONWISE_AGGREGATE_FULL;
		else if ((extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0)
			patype = PARTITIONWISE_AGGREGATE_PARTIAL;
		else
			patype = PARTITIONWISE_AGGREGATE_NONE;
	}

	/*
	 * 在为 grouped_rel 生成路径之前，先生成任何可能的
	 * 部分分组路径；这样后面的代码就容易同时考虑
	 * 并行和非并行的分组办法。
	 */
	if ((extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0)
	{
		bool		force_rel_creation;

		/*
		 * 若在这一层做按分区聚合，就强制
		 * 建立一个 partially_grouped_rel，以便把按分区
		 * 路径加进去。
		 */
		force_rel_creation = (patype == PARTITIONWISE_AGGREGATE_PARTIAL);

		partially_grouped_rel =
			create_partial_grouping_paths(root,
										  grouped_rel,
										  input_rel,
										  gd,
										  extra,
										  force_rel_creation);
	}

	/* 设置输出参数。 */
	*partially_grouped_rel_p = partially_grouped_rel;

	/* 若可能，应用按分区聚合技术。 */
	if (patype != PARTITIONWISE_AGGREGATE_NONE)
		create_partitionwise_grouping_paths(root, input_rel, grouped_rel,
											partially_grouped_rel, agg_costs,
											gd, patype, extra);

	/* 若只做部分聚合，就返回。 */
	if (extra->patype == PARTITIONWISE_AGGREGATE_PARTIAL)
	{
		Assert(partially_grouped_rel);

		if (partially_grouped_rel->pathlist)
			set_cheapest(partially_grouped_rel);

		return;
	}

	/* 收集任何部分分组的部分路径。 */
	if (partially_grouped_rel && partially_grouped_rel->partial_pathlist)
	{
		gather_grouping_paths(root, partially_grouped_rel);
		set_cheapest(partially_grouped_rel);
	}

	/*
	 * 估计组数。
	 */
	dNumGroups = get_number_of_groups(root,
									  cheapest_path->rows,
									  gd,
									  extra->targetList);

	/* 建立最终的分组路径 */
	add_paths_to_grouping_rel(root, input_rel, grouped_rel,
							  partially_grouped_rel, agg_costs, gd,
							  dNumGroups, extra);

	/* 若没能找到任何实现，就给出有帮助的错误 */
	if (grouped_rel->pathlist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement GROUP BY"),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/*
	 * 若有一个 FDW 负责该查询的全部基关系，
	 * 就让它考虑添加 ForeignPaths。
	 */
	if (grouped_rel->fdwroutine &&
		grouped_rel->fdwroutine->GetForeignUpperPaths)
		grouped_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_GROUP_AGG,
													  input_rel, grouped_rel,
													  extra);

	/* 让扩展也许再添加一些路径 */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_GROUP_AGG,
									input_rel, grouped_rel,
									extra);
}

/*
 * 对给定的输入路径，考虑用哈希和排序的组合在其上做分组集的
 * 可能办法。这也许被调用多次，
 * 因此重要的是它不要就地修改输入。不返回结果，
 * 但任何生成的路径都加进 grouped_rel。
 */
static void
consider_groupingsets_paths(PlannerInfo *root,
							RelOptInfo *grouped_rel,
							Path *path,
							bool is_sorted,
							bool can_hash,
							grouping_sets_data *gd,
							const AggClauseCosts *agg_costs,
							double dNumGroups)
{
	Query	   *parse = root->parse;
	Size		hash_mem_limit = get_hash_memory_limit();

	/*
	 * 若提供的不是已排序输入，就只考虑
	 * 完全靠哈希就能完成的计划。
	 *
	 * 若看起来能放进 hash_mem，就可以把一切都哈希。但若
	 * 输入实际上已排序，尽管没被宣传成这样，我们
	 * 更愿意利用这一点以少用内存。
	 *
	 * 若没有一个分组集可排序，就忽略 hash_mem
	 * 限制，反正生成一条路径，否则只会失败。
	 */
	if (!is_sorted)
	{
		List	   *new_rollups = NIL;
		RollupData *unhashed_rollup = NULL;
		List	   *sets_data;
		List	   *empty_sets_data = NIL;
		List	   *empty_sets = NIL;
		ListCell   *lc;
		ListCell   *l_start = list_head(gd->rollups);
		AggStrategy strat = AGG_HASHED;
		double		hashsize;
		double		exclude_groups = 0.0;

		Assert(can_hash);

		/*
		 * 若输入碰巧按有用的方式排好了序（即使 is_sorted 为假也可能发生，
		 * 因为那只意味着调用方
		 * 已经为我们安排好了排序），就利用这一点来节省一些哈希表空间。
		 * 但要注意退化情形：
		 *
		 * 1) 若有任何空分组集，则当全部非空分组集都不可排序时，group_pathkeys 也许
		 * 为 NIL。这种情况下，
		 * 会有一个只含空组的 rollup，而
		 * pathkeys_contained_in 测试空洞为真；这没问题。
		 *
		 * XXX：上面依赖于 group_pathkeys 是从
		 * 第一个 rollup 生成的这一事实。若我们加上考虑分组输入多种
		 * 排序的能力，这个假定也许会失败。
		 *
		 * 2) 若没有空集且只有不可排序的集合，则
		 * rollups 列表将为空（因此 l_start == NULL），且
		 * group_pathkeys 将为 NIL；必须确保空洞为真的
		 * pathkeys_contained_in 测试不会让我们崩溃。
		 */
		if (l_start != NULL &&
			pathkeys_contained_in(root->group_pathkeys, path->pathkeys))
		{
			unhashed_rollup = lfirst_node(RollupData, l_start);
			exclude_groups = unhashed_rollup->numGroups;
			l_start = lnext(gd->rollups, l_start);
		}

		hashsize = estimate_hashagg_tablesize(root,
											  path,
											  agg_costs,
											  dNumGroups - exclude_groups);

		/*
		 * 若只有不可排序的列可处理，gd->rollups 就为空。
		 * 那种情况下覆盖 hash_mem；否则就依靠
		 * 已排序输入的情形来生成可用的混合路径。
		 */
		if (hashsize > hash_mem_limit && gd->rollups)
			return;				/* 不行，放不下 */

		/*
		 * 需要把现有的 rollups 列表拆成单独的分组
		 * 集，并为每个集合重新计算 groupClause。
		 */
		sets_data = list_copy(gd->unsortable_sets);

		for_each_cell(lc, gd->rollups, l_start)
		{
			RollupData *rollup = lfirst_node(RollupData, lc);

			/*
			 * 若发现一个没被上面"实际上已排序"检查跳过的不可哈希 rollup，
			 * 我们就应付不了；会需要已排序的
			 * 输入（用另一种排序），但这里拿不到。
			 * 因此退出；改为从 is_sorted 情形
			 * 得到一条有效路径。
			 *
			 * 仅仅存在空分组集并不会使 rollup
			 * 不可哈希（见 preprocess_grouping_sets），我们在下面
			 * 特殊处理那些。
			 */
			if (!rollup->hashable)
				return;

			sets_data = list_concat(sets_data, rollup->gsets_data);
		}
		foreach(lc, sets_data)
		{
			GroupingSetData *gs = lfirst_node(GroupingSetData, lc);
			List	   *gset = gs->set;
			RollupData *rollup;

			if (gset == NIL)
			{
				/* 空分组集无法哈希。 */
				empty_sets_data = lappend(empty_sets_data, gs);
				empty_sets = lappend(empty_sets, NIL);
			}
			else
			{
				rollup = makeNode(RollupData);

				rollup->groupClause = preprocess_groupclause(root, gset);
				rollup->gsets_data = list_make1(gs);
				rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,
														 rollup->gsets_data,
														 gd->tleref_to_colnum_map);
				rollup->numGroups = gs->numGroups;
				rollup->hashable = true;
				rollup->is_hashed = true;
				new_rollups = lappend(new_rollups, rollup);
			}
		}

		/*
		 * 若没找到任何非空的可哈希东西，就退出。我们会
		 * 从 is_sorted 情形生成一条路径。
		 */
		if (new_rollups == NIL)
			return;

		/*
		 * 若有空分组集，它们应当在
		 * 第一个 rollup 里。
		 */
		Assert(!unhashed_rollup || !empty_sets);

		if (unhashed_rollup)
		{
			new_rollups = lappend(new_rollups, unhashed_rollup);
			strat = AGG_MIXED;
		}
		else if (empty_sets)
		{
			RollupData *rollup = makeNode(RollupData);

			rollup->groupClause = NIL;
			rollup->gsets_data = empty_sets_data;
			rollup->gsets = empty_sets;
			rollup->numGroups = list_length(empty_sets);
			rollup->hashable = false;
			rollup->is_hashed = false;
			new_rollups = lappend(new_rollups, rollup);
			strat = AGG_MIXED;
		}

		add_path(grouped_rel, (Path *)
				 create_groupingsets_path(root,
										  grouped_rel,
										  path,
										  (List *) parse->havingQual,
										  strat,
										  new_rollups,
										  agg_costs));
		return;
	}

	/*
	 * 若有已排序输入，但没什么能用它做的，就退出。
	 */
	if (gd->rollups == NIL)
		return;

	/*
	 * 给定已排序输入，我们试着做两条路径：一条排序，一条混合
	 * 排序/哈希。（两条都要试，因为 hashagg 也许被禁用，或
	 * 有些列也许不可排序。）
	 *
	 * 若别处有障碍（例如
	 * 有序聚合）意味着根本不应当考虑哈希，就传入 can_hash 为假。
	 */
	if (can_hash && gd->any_hashable)
	{
		List	   *rollups = NIL;
		List	   *hash_sets = list_copy(gd->unsortable_sets);
		double		availspace = hash_mem_limit;
		ListCell   *lc;

		/*
		 * 先计入完全无法排序的组所需的空间。
		 */
		availspace -= estimate_hashagg_tablesize(root,
												 path,
												 agg_costs,
												 gd->dNumHashGroups);

		if (availspace > 0 && list_length(gd->rollups) > 1)
		{
			double		scale;
			int			num_rollups = list_length(gd->rollups);
			int			k_capacity;
			int		   *k_weights = palloc(num_rollups * sizeof(int));
			Bitmapset  *hash_items = NULL;
			int			i;

			/*
			 * 我们把这当作背包问题：背包容量
			 * 表示 hash_mem，物品重量是实现单个 rollup 所需哈希表的估计内存
			 * 用量，
			 * 而且我们实在应当用节省的代价作为物品价值；
			 * 不过目前赋给排序节点的代价并不能
			 * 很好地反映比较代价，因此把全部物品
			 * 视为等值（每个改为哈希的 rollup 为我们省一次排序）。
			 *
			 * 要用离散背包，就需要把值缩放到
			 * 一个合理的小有界范围。我们选择允许 5% 的误差
			 * 幅度；最坏情况下 rollup 不超过 4096 个，
			 * 5% 误差幅度将需要略多于 42MB
			 * 的工作区。（谁想规划那么复杂的查询，
			 * 最好有相应的内存。在更合理的情形下，
			 * rollup 不超过几十个时，内存用量将
			 * 可以忽略。）
			 *
			 * k_capacity 自然是有界的，但我们夹紧
			 * 比例和重量的值（见下），以免上溢或下溢（或
			 * 无谓地试图使用小于 1 字节的比例因子）。
			 */
			scale = Max(availspace / (20.0 * num_rollups), 1.0);
			k_capacity = (int) floor(availspace / scale);

			/*
			 * 我们不考虑第一个 rollup，因为它是
			 * 匹配输入排序的那个。我们只给那些考虑哈希的项分配下标 "i"；
			 * 下面的第二个循环
			 * 必须使用同样的条件。
			 */
			i = 0;
			for_each_from(lc, gd->rollups, 1)
			{
				RollupData *rollup = lfirst_node(RollupData, lc);

				if (rollup->hashable)
				{
					double		sz = estimate_hashagg_tablesize(root,
																path,
																agg_costs,
																rollup->numGroups);

					/*
					 * 若 sz 极大，但 hash_mem（因而 scale）
					 * 很小，就避免这里的整数溢出。
					 */
					k_weights[i] = (int) Min(floor(sz / scale),
											 k_capacity + 1.0);
					++i;
				}
			}

			/*
			 * 应用背包算法；计算使所存价值
			 * （这里是省下的排序次数）最大化、同时总大小
			 * （近似）保持在容量之内的
			 * 物品集合。
			 */
			if (i > 0)
				hash_items = DiscreteKnapsack(k_capacity, i, k_weights, NULL);

			if (!bms_is_empty(hash_items))
			{
				rollups = list_make1(linitial(gd->rollups));

				i = 0;
				for_each_from(lc, gd->rollups, 1)
				{
					RollupData *rollup = lfirst_node(RollupData, lc);

					if (rollup->hashable)
					{
						if (bms_is_member(i, hash_items))
							hash_sets = list_concat(hash_sets,
													rollup->gsets_data);
						else
							rollups = lappend(rollups, rollup);
						++i;
					}
					else
						rollups = lappend(rollups, rollup);
				}
			}
		}

		if (!rollups && hash_sets)
			rollups = list_copy(gd->rollups);

		foreach(lc, hash_sets)
		{
			GroupingSetData *gs = lfirst_node(GroupingSetData, lc);
			RollupData *rollup = makeNode(RollupData);

			Assert(gs->set != NIL);

			rollup->groupClause = preprocess_groupclause(root, gs->set);
			rollup->gsets_data = list_make1(gs);
			rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,
													 rollup->gsets_data,
													 gd->tleref_to_colnum_map);
			rollup->numGroups = gs->numGroups;
			rollup->hashable = true;
			rollup->is_hashed = true;
			rollups = lcons(rollup, rollups);
		}

		if (rollups)
		{
			add_path(grouped_rel, (Path *)
					 create_groupingsets_path(root,
											  grouped_rel,
											  path,
											  (List *) parse->havingQual,
											  AGG_MIXED,
											  rollups,
											  agg_costs));
		}
	}

	/*
	 * 现在试简单的已排序情形。
	 */
	if (!gd->unsortable_sets)
		add_path(grouped_rel, (Path *)
				 create_groupingsets_path(root,
										  grouped_rel,
										  path,
										  (List *) parse->havingQual,
										  AGG_SORTED,
										  gd->rollups,
										  agg_costs));
}

/*
 * create_window_paths
 *
 * 建立一个新的上层关系，含有窗口函数求值的 Path。
 *
 * input_rel：含有源数据 Path
 * input_target：make_window_input_target 的结果
 * output_target：最顶层 WindowAggPath 应当返回的东西
 * wflists：find_window_functions 的结果
 * activeWindows：select_active_windows 的结果
 *
 * 注意：input_rel 里的全部 Path 都预期返回 input_target。
 */
static RelOptInfo *
create_window_paths(PlannerInfo *root,
					RelOptInfo *input_rel,
					PathTarget *input_target,
					PathTarget *output_target,
					bool output_target_parallel_safe,
					WindowFuncLists *wflists,
					List *activeWindows)
{
	RelOptInfo *window_rel;
	ListCell   *lc;

	/* 目前，全部工作都在 (WINDOW, NULL) 上层关系里做 */
	window_rel = fetch_upper_rel(root, UPPERREL_WINDOW, NULL);

	/*
	 * 若输入关系不是并行安全的，则窗口关系
	 * 也不可能是并行安全的。否则，需要检查
	 * 目标列表和活动窗口里有没有并行不安全的构造。
	 */
	if (input_rel->consider_parallel && output_target_parallel_safe &&
		is_parallel_safe(root, (Node *) activeWindows))
		window_rel->consider_parallel = true;

	/*
	 * 若输入关系属于单个 FDW，窗口关系也是。
	 */
	window_rel->serverid = input_rel->serverid;
	window_rel->userid = input_rel->userid;
	window_rel->useridiscurrent = input_rel->useridiscurrent;
	window_rel->fdwroutine = input_rel->fdwroutine;

	/*
	 * 考虑从现有的
	 * 总代价最低路径（很可能需要一次排序）开始计算窗口函数，也考虑任何
	 * 满足或部分满足 root->window_pathkeys 的现有路径。
	 */
	foreach(lc, input_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);
		int			presorted_keys;

		if (path == input_rel->cheapest_total_path ||
			pathkeys_count_contained_in(root->window_pathkeys, path->pathkeys,
										&presorted_keys) ||
			presorted_keys > 0)
			create_one_window_path(root,
								   window_rel,
								   path,
								   input_target,
								   output_target,
								   wflists,
								   activeWindows);
	}

	/*
	 * 若有一个 FDW 负责该查询的全部基关系，
	 * 就让它考虑添加 ForeignPaths。
	 */
	if (window_rel->fdwroutine &&
		window_rel->fdwroutine->GetForeignUpperPaths)
		window_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_WINDOW,
													 input_rel, window_rel,
													 NULL);

	/* 让扩展也许再添加一些路径 */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_WINDOW,
									input_rel, window_rel, NULL);

	/* 现在选出最佳路径 */
	set_cheapest(window_rel);

	return window_rel;
}

/*
 * 在给定 Path 之上叠上窗口函数实现步骤，并把
 * 结果加进 window_rel。
 *
 * window_rel：将包含结果的上层关系
 * path：要使用的输入 Path（必须返回 input_target）
 * input_target：make_window_input_target 的结果
 * output_target：最顶层 WindowAggPath 应当返回的东西
 * wflists：find_window_functions 的结果
 * activeWindows：select_active_windows 的结果
 */
static void
create_one_window_path(PlannerInfo *root,
					   RelOptInfo *window_rel,
					   Path *path,
					   PathTarget *input_target,
					   PathTarget *output_target,
					   WindowFuncLists *wflists,
					   List *activeWindows)
{
	PathTarget *window_target;
	ListCell   *l;
	List	   *topqual = NIL;

	/*
	 * 由于每个窗口子句也许需要不同的排序，我们为每个子句叠上
	 * 一个 WindowAgg 节点，并在它们之间按
	 * 需要加上排序步骤。（我们假定 select_active_windows 为
	 * 子句的执行选了好顺序。）
	 *
	 * input_target 应当包含结果所需的全部 Var 和 Agg。
	 * （有些情况下并不需要把这些一路
	 * 传到顶层，因为它们也许只用作 WindowFuncs 的输入。
	 * 不过试图优化那个大概不值得。）它也必须
	 * 包含全部窗口分区和排序表达式，以确保
	 * 它们只在栈底计算一次（这对
	 * 易变函数至关重要）。随着沿栈往上，我们会为
	 * 每一层算出的 WindowFuncs 加上输出。
	 */
	window_target = input_target;

	foreach(l, activeWindows)
	{
		WindowClause *wc = lfirst_node(WindowClause, l);
		List	   *window_pathkeys;
		int			presorted_keys;
		bool		is_sorted;
		bool		topwindow;

		window_pathkeys = make_pathkeys_for_window(root,
												   wc,
												   root->processed_tlist);

		is_sorted = pathkeys_count_contained_in(window_pathkeys,
												path->pathkeys,
												&presorted_keys);

		/* 必要时排序 */
		if (!is_sorted)
		{
			/*
			 * 没有预排序键，或增量排序被禁用，就做一次
			 * 完整排序。
			 */
			if (presorted_keys == 0 || !enable_incremental_sort)
				path = (Path *) create_sort_path(root, window_rel,
												 path,
												 window_pathkeys,
												 -1.0);
			else
			{
				/*
				 * 既然有预排序键且增量排序
				 * 已启用，就用增量排序。
				 */
				path = (Path *) create_incremental_sort_path(root,
															 window_rel,
															 path,
															 window_pathkeys,
															 presorted_keys,
															 -1.0);
			}
		}

		if (lnext(activeWindows, l))
		{
			/*
			 * 把当前的 WindowFuncs 加进这个
			 * 中间 WindowAggPath 的输出目标。必须复制 window_target，
			 * 以免改变前一条路径的目标。
			 *
			 * 注意：WindowFunc 不给目标的求值代价增加任何东西；但
			 * 确实需要计入 tlist 宽度的增加。
			 */
			ListCell   *lc2;

			window_target = copy_pathtarget(window_target);
			foreach(lc2, wflists->windowFuncs[wc->winref])
			{
				WindowFunc *wfunc = lfirst_node(WindowFunc, lc2);

				add_column_to_pathtarget(window_target, (Expr *) wfunc, 0);
				window_target->width += get_typavgwidth(wfunc->wintype, -1);
			}
		}
		else
		{
			/* 把目标装进最顶层的 WindowAgg */
			window_target = output_target;
		}

		/* 把列表里的最后一项标成顶层窗口 */
		topwindow = foreach_current_index(l) == list_length(activeWindows) - 1;

		/*
		 * 累积每个中间
		 * WindowClause 的全部 runConditions。顶层 WindowAgg 必须把这些作为限定条件传入，以便
		 * 正确地滤掉不想要的元组。
		 */
		if (!topwindow)
			topqual = list_concat(topqual, wc->runCondition);

		path = (Path *)
			create_windowagg_path(root, window_rel, path, window_target,
								  wflists->windowFuncs[wc->winref],
								  wc, topwindow ? topqual : NIL, topwindow);
	}

	add_path(window_rel, path);
}

/*
 * create_distinct_paths
 *
 * 建立一个新的上层关系，含有 SELECT DISTINCT 求值的 Path。
 *
 * input_rel：含有源数据 Path
 *
 * 注意：输入路径应当已经算出想要的 pathtarget，因为
 * Sort/Unique 不会投影任何东西。
 */
static RelOptInfo *
create_distinct_paths(PlannerInfo *root, RelOptInfo *input_rel)
{
	RelOptInfo *distinct_rel;

	/* 目前，全部工作都在 (DISTINCT, NULL) 上层关系里做 */
	distinct_rel = fetch_upper_rel(root, UPPERREL_DISTINCT, NULL);

	/*
	 * 我们在这一层不计算任何东西，因此若输入关系并行安全，distinct_rel 就
	 * 并行安全。尤其是，若
	 * 有 DISTINCT ON (...) 子句，input_rel 的任何路径都会
	 * 输出那些表达式，除非那些
	 * 表达式并行安全，否则不会并行安全。
	 */
	distinct_rel->consider_parallel = input_rel->consider_parallel;

	/*
	 * 若输入关系属于单个 FDW，distinct_rel 也是。
	 */
	distinct_rel->serverid = input_rel->serverid;
	distinct_rel->userid = input_rel->userid;
	distinct_rel->useridiscurrent = input_rel->useridiscurrent;
	distinct_rel->fdwroutine = input_rel->fdwroutine;

	/* 根据 input_rel 的 pathlist 建立互异路径 */
	create_final_distinct_paths(root, input_rel, distinct_rel);

	/* 现在根据 input_rel 的 partial_pathlist 建立互异路径 */
	create_partial_distinct_paths(root, input_rel, distinct_rel);

	/* 若没能建立任何路径，就给出有帮助的错误 */
	if (distinct_rel->pathlist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement DISTINCT"),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/*
	 * 若有一个 FDW 负责该查询的全部基关系，
	 * 就让它考虑添加 ForeignPaths。
	 */
	if (distinct_rel->fdwroutine &&
		distinct_rel->fdwroutine->GetForeignUpperPaths)
		distinct_rel->fdwroutine->GetForeignUpperPaths(root,
													   UPPERREL_DISTINCT,
													   input_rel,
													   distinct_rel,
													   NULL);

	/* 让扩展也许再添加一些路径 */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_DISTINCT, input_rel,
									distinct_rel, NULL);

	/* 现在选出最佳路径 */
	set_cheapest(distinct_rel);

	return distinct_rel;
}

/*
 * create_partial_distinct_paths
 *
 * 处理 'input_rel' 的部分路径，并把 unique/aggregate 路径加进
 * UPPERREL_PARTIAL_DISTINCT 关系。对已建立的路径，在上面加上 Gather/GatherMerge
 * 路径，并加一条最终的 unique/aggregate 路径，以去掉合并
 * 并行工作进程的行所产生的任何重复。
 */
static void
create_partial_distinct_paths(PlannerInfo *root, RelOptInfo *input_rel,
							  RelOptInfo *final_distinct_rel)
{
	RelOptInfo *partial_distinct_rel;
	Query	   *parse;
	List	   *distinctExprs;
	double		numDistinctRows;
	Path	   *cheapest_partial_path;
	ListCell   *lc;

	/* 输入关系里没有部分路径时无事可做 */
	if (!input_rel->consider_parallel || input_rel->partial_pathlist == NIL)
		return;

	parse = root->parse;

	/* 无法做并行 DISTINCT ON */
	if (parse->hasDistinctOn)
		return;

	partial_distinct_rel = fetch_upper_rel(root, UPPERREL_PARTIAL_DISTINCT,
										   NULL);
	partial_distinct_rel->reltarget = root->upper_targets[UPPERREL_PARTIAL_DISTINCT];
	partial_distinct_rel->consider_parallel = input_rel->consider_parallel;

	/*
	 * 若 input_rel 属于单个 FDW，partial_distinct_rel 也是。
	 */
	partial_distinct_rel->serverid = input_rel->serverid;
	partial_distinct_rel->userid = input_rel->userid;
	partial_distinct_rel->useridiscurrent = input_rel->useridiscurrent;
	partial_distinct_rel->fdwroutine = input_rel->fdwroutine;

	cheapest_partial_path = linitial(input_rel->partial_pathlist);

	distinctExprs = get_sortgrouplist_exprs(root->processed_distinctClause,
											parse->targetList);

	/* 估计每个工作进程会得到多少互异行 */
	numDistinctRows = estimate_num_groups(root, distinctExprs,
										  cheapest_partial_path->rows,
										  NULL, NULL);

	/*
	 * 试着对总代价最低的路径排序，并对任何带
	 * 预排序键的路径做增量排序，再在那些上面放 unique 路径。
	 */
	if (grouping_is_sortable(root->processed_distinctClause))
	{
		foreach(lc, input_rel->partial_pathlist)
		{
			Path	   *input_path = (Path *) lfirst(lc);
			Path	   *sorted_path;
			bool		is_sorted;
			int			presorted_keys;

			is_sorted = pathkeys_count_contained_in(root->distinct_pathkeys,
													input_path->pathkeys,
													&presorted_keys);

			if (is_sorted)
				sorted_path = input_path;
			else
			{
				/*
				 * 至少试着对总代价最低的路径排序，也试着
				 * 对任何已经部分排好序的路径做
				 * 增量排序（增量排序被禁用时，不必处理带预排序
				 * 键的路径，除非它是
				 * 总代价最低的部分路径）。
				 */
				if (input_path != cheapest_partial_path &&
					(presorted_keys == 0 || !enable_incremental_sort))
					continue;

				/*
				 * 不必同时考虑排序和增量排序。
				 * 没有预排序键时就做排序，有
				 * 预排序键时做增量排序。
				 */
				if (presorted_keys == 0 || !enable_incremental_sort)
					sorted_path = (Path *) create_sort_path(root,
															partial_distinct_rel,
															input_path,
															root->distinct_pathkeys,
															-1.0);
				else
					sorted_path = (Path *) create_incremental_sort_path(root,
																		partial_distinct_rel,
																		input_path,
																		root->distinct_pathkeys,
																		presorted_keys,
																		-1.0);
			}

			add_partial_path(partial_distinct_rel, (Path *)
							 create_upper_unique_path(root, partial_distinct_rel,
													  sorted_path,
													  list_length(root->distinct_pathkeys),
													  numDistinctRows));
		}
	}

	/*
	 * 现在若已启用且哈希可能，就试哈希聚合路径。由于
	 * 我们并不负责尽力在这里至少建立一条
	 * 路径，因此把 enable_hashagg 当作硬开关，而不是
	 * create_final_distinct_paths 里那种稍软的变体。
	 */
	if (enable_hashagg && grouping_is_hashable(root->processed_distinctClause))
	{
		add_partial_path(partial_distinct_rel, (Path *)
						 create_agg_path(root,
										 partial_distinct_rel,
										 cheapest_partial_path,
										 cheapest_partial_path->pathtarget,
										 AGG_HASHED,
										 AGGSPLIT_SIMPLE,
										 root->processed_distinctClause,
										 NIL,
										 NULL,
										 numDistinctRows));
	}

	/*
	 * 若有一个 FDW 负责该查询的全部基关系，
	 * 就让它考虑添加 ForeignPaths。
	 */
	if (partial_distinct_rel->fdwroutine &&
		partial_distinct_rel->fdwroutine->GetForeignUpperPaths)
		partial_distinct_rel->fdwroutine->GetForeignUpperPaths(root,
															   UPPERREL_PARTIAL_DISTINCT,
															   input_rel,
															   partial_distinct_rel,
															   NULL);

	/* 让扩展也许再添加一些部分路径 */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_PARTIAL_DISTINCT,
									input_rel, partial_distinct_rel, NULL);

	if (partial_distinct_rel->partial_pathlist != NIL)
	{
		generate_gather_paths(root, partial_distinct_rel, true);
		set_cheapest(partial_distinct_rel);

		/*
		 * 最后，建立使最终结果互异的路径。这一步
		 * 是为了去掉因合并
		 * 并行工作进程的行而产生的任何重复。
		 */
		create_final_distinct_paths(root, partial_distinct_rel,
									final_distinct_rel);
	}
}

/*
 * create_final_distinct_paths
 *		根据 'input_rel' 的 pathlist 在 'distinct_rel' 里建立互异路径
 *
 * input_rel：含有源数据路径
 * distinct_rel：存放所建路径的目标关系
 */
static RelOptInfo *
create_final_distinct_paths(PlannerInfo *root, RelOptInfo *input_rel,
							RelOptInfo *distinct_rel)
{
	Query	   *parse = root->parse;
	Path	   *cheapest_input_path = input_rel->cheapest_total_path;
	double		numDistinctRows;
	bool		allow_hash;

	/* 估计将会有多少互异行 */
	if (parse->groupClause || parse->groupingSets || parse->hasAggs ||
		root->hasHavingQual)
	{
		/*
		 * 若有过分组或聚合，就用输入行数
		 * 作为 DISTINCT 行数的估计（即假定输入
		 * 已经大体互异）。
		 */
		numDistinctRows = cheapest_input_path->rows;
	}
	else
	{
		/*
		 * 否则，UNIQUE 过滤的效果与 GROUP BY 相当。
		 */
		List	   *distinctExprs;

		distinctExprs = get_sortgrouplist_exprs(root->processed_distinctClause,
												parse->targetList);
		numDistinctRows = estimate_num_groups(root, distinctExprs,
											  cheapest_input_path->rows,
											  NULL, NULL);
	}

	/*
	 * 若可能，考虑 DISTINCT 的基于排序的实现。
	 */
	if (grouping_is_sortable(root->processed_distinctClause))
	{
		/*
		 * 首先，若有任何预排序得足够好的路径，就在那些上面
		 * 贴一个 Unique 节点。我们也考虑对
		 * 总代价最低的输入路径做一次显式排序再 Unique。若任何路径有
		 * 预排序键，就在那些上面建一次增量排序，
		 * 然后再在顶上加 unique 节点。
		 *
		 * 有 DISTINCT ON 时，必须按 DISTINCT 和 ORDER BY 中更严格的那个
		 * 排序，否则就不会有想要的行为。
		 * 另外，若确实得做一次显式排序，不妨用
		 * 更严格的顺序，以免以后再排第二次。（注意
		 * 解析器会确保一个子句是
		 * 另一个的前缀。）
		 */
		List	   *needed_pathkeys;
		ListCell   *lc;
		double		limittuples = root->distinct_pathkeys == NIL ? 1.0 : -1.0;

		if (parse->hasDistinctOn &&
			list_length(root->distinct_pathkeys) <
			list_length(root->sort_pathkeys))
			needed_pathkeys = root->sort_pathkeys;
		else
			needed_pathkeys = root->distinct_pathkeys;

		foreach(lc, input_rel->pathlist)
		{
			Path	   *input_path = (Path *) lfirst(lc);
			Path	   *sorted_path;
			bool		is_sorted;
			int			presorted_keys;

			is_sorted = pathkeys_count_contained_in(needed_pathkeys,
													input_path->pathkeys,
													&presorted_keys);

			if (is_sorted)
				sorted_path = input_path;
			else
			{
				/*
				 * 至少试着对总代价最低的路径排序，也试着
				 * 对任何已经部分排好序的路径做
				 * 增量排序（增量排序被禁用时，不必处理带预排序
				 * 键的路径，除非它是
				 * 总代价最低的输入路径）。
				 */
				if (input_path != cheapest_input_path &&
					(presorted_keys == 0 || !enable_incremental_sort))
					continue;

				/*
				 * 不必同时考虑排序和增量排序。
				 * 没有预排序键时就做排序，有
				 * 预排序键时做增量排序。
				 */
				if (presorted_keys == 0 || !enable_incremental_sort)
					sorted_path = (Path *) create_sort_path(root,
															distinct_rel,
															input_path,
															needed_pathkeys,
															limittuples);
				else
					sorted_path = (Path *) create_incremental_sort_path(root,
																		distinct_rel,
																		input_path,
																		needed_pathkeys,
																		presorted_keys,
																		limittuples);
			}

			/*
			 * 若全部路径键都被判定为冗余，distinct_pathkeys 也许已经变空。
			 * 若全部路径键都
			 * 冗余，则每个 DISTINCT 目标都只能允许单个
			 * 值，因此全部结果元组都必定相同（或至少
			 * 用等值检查无法区分）。我们可以
			 * 只取第一个元组就使这些元组互异。这里所做的
			 * 全部就是在 'sorted_path' 之上加一条做 "LIMIT 1" 的路径。做
			 * DISTINCT ON 时，sort_pathkeys
			 * 列表也许仍非 NIL，因此仍必须只对已按
			 * sort_pathkeys 正确排序的路径这么做。
			 */
			if (root->distinct_pathkeys == NIL)
			{
				Node	   *limitCount;

				limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid,
												sizeof(int64),
												Int64GetDatum(1), false,
												FLOAT8PASSBYVAL);

				/*
				 * 若查询已经有 LIMIT 子句，最终计划里也许会
				 * 出现重复的 LimitPath。这
				 * 似乎不值得太费心。
				 */
				add_path(distinct_rel, (Path *)
						 create_limit_path(root, distinct_rel, sorted_path,
										   NULL, limitCount,
										   LIMIT_OPTION_COUNT, 0, 1));
			}
			else
			{
				add_path(distinct_rel, (Path *)
						 create_upper_unique_path(root, distinct_rel,
												  sorted_path,
												  list_length(root->distinct_pathkeys),
												  numDistinctRows));
			}
		}
	}

	/*
	 * 若可能，考虑 DISTINCT 的基于哈希的实现。
	 *
	 * 若没能做出任何其他类型的路径，就*必须*哈希，
	 * 否则就失败。若确实有其他选择，有两件事
	 * 应当阻止选择哈希：若查询使用 DISTINCT ON
	 * （因为若哈希，就不会真有预期的行为），或若
	 * enable_hashagg 关闭。
	 *
	 * 注意：grouping_is_hashable() 比其他
	 * 门控条件贵得多，因此想把它放在最后。
	 */
	if (distinct_rel->pathlist == NIL)
		allow_hash = true;		/* 我们没有替代方案 */
	else if (parse->hasDistinctOn || !enable_hashagg)
		allow_hash = false;		/* 基于策略决定不哈希 */
	else
		allow_hash = true;		/* 默认 */

	if (allow_hash && grouping_is_hashable(root->processed_distinctClause))
	{
		/* 生成哈希聚合路径——不需要排序 */
		add_path(distinct_rel, (Path *)
				 create_agg_path(root,
								 distinct_rel,
								 cheapest_input_path,
								 cheapest_input_path->pathtarget,
								 AGG_HASHED,
								 AGGSPLIT_SIMPLE,
								 root->processed_distinctClause,
								 NIL,
								 NULL,
								 numDistinctRows));
	}

	return distinct_rel;
}

/*
 * create_ordered_paths
 *
 * 建立一个新的上层关系，含有 ORDER BY 求值的 Path。
 *
 * 结果里的全部路径都必须满足 ORDER BY 顺序。
 * 我们需要考虑的新路径只有对现有总代价最低路径的一次显式完整排序
 * 和增量排序。
 *
 * input_rel：含有源数据 Path
 * target：结果 Path 必须发出的输出 tlist
 * limit_tuples：输出元组数的估计上界，
 *		若没有 LIMIT 或估不出则为 -1
 *
 * XXX 这只看 sort_pathkeys。不知道是否还得看
 * 其他路径键（分组……），像 generate_useful_gather_paths 那样。
 */
static RelOptInfo *
create_ordered_paths(PlannerInfo *root,
					 RelOptInfo *input_rel,
					 PathTarget *target,
					 bool target_parallel_safe,
					 double limit_tuples)
{
	Path	   *cheapest_input_path = input_rel->cheapest_total_path;
	RelOptInfo *ordered_rel;
	ListCell   *lc;

	/* 目前，全部工作都在 (ORDERED, NULL) 上层关系里做 */
	ordered_rel = fetch_upper_rel(root, UPPERREL_ORDERED, NULL);

	/*
	 * 若输入关系不是并行安全的，则有序关系
	 * 也不可能是并行安全的。否则，若
	 * 目标列表并行安全，它就是并行安全的。
	 */
	if (input_rel->consider_parallel && target_parallel_safe)
		ordered_rel->consider_parallel = true;

	/*
	 * 若输入关系属于单个 FDW，ordered_rel 也是。
	 */
	ordered_rel->serverid = input_rel->serverid;
	ordered_rel->userid = input_rel->userid;
	ordered_rel->useridiscurrent = input_rel->useridiscurrent;
	ordered_rel->fdwroutine = input_rel->fdwroutine;

	foreach(lc, input_rel->pathlist)
	{
		Path	   *input_path = (Path *) lfirst(lc);
		Path	   *sorted_path;
		bool		is_sorted;
		int			presorted_keys;

		is_sorted = pathkeys_count_contained_in(root->sort_pathkeys,
												input_path->pathkeys, &presorted_keys);

		if (is_sorted)
			sorted_path = input_path;
		else
		{
			/*
			 * 至少试着对总代价最低的路径排序，也试着
			 * 对任何已经部分排好序的路径做
			 * 增量排序（增量排序被禁用时，不必处理带预排序键的
			 * 路径，除非它是总代价最低的
			 * 输入路径）。
			 */
			if (input_path != cheapest_input_path &&
				(presorted_keys == 0 || !enable_incremental_sort))
				continue;

			/*
			 * 不必同时考虑排序和增量排序。
			 * 没有预排序键时就做排序，有
			 * 预排序键时做增量排序。
			 */
			if (presorted_keys == 0 || !enable_incremental_sort)
				sorted_path = (Path *) create_sort_path(root,
														ordered_rel,
														input_path,
														root->sort_pathkeys,
														limit_tuples);
			else
				sorted_path = (Path *) create_incremental_sort_path(root,
																	ordered_rel,
																	input_path,
																	root->sort_pathkeys,
																	presorted_keys,
																	limit_tuples);
		}

		/* 若需要，加上投影步骤 */
		if (sorted_path->pathtarget != target)
			sorted_path = apply_projection_to_path(root, ordered_rel,
												   sorted_path, target);

		add_path(ordered_rel, sorted_path);
	}

	/*
	 * generate_gather_paths() 若有最佳并行路径，已经为它生成了一条简单的 Gather
	 * 路径，上面的循环会
	 * 考虑对它排序。同样，generate_gather_paths() 也会
	 * 生成保序的 Gather Merge 计划，若它们碰巧匹配 sort_pathkeys，就可以
	 * 不排序使用，上面的循环
	 * 也会处理那些。不过还有一种
	 * 可能：按所需输出顺序对总代价最低的部分路径
	 * 排序，然后再用 Gather Merge，也许有意义。
	 */
	if (ordered_rel->consider_parallel && root->sort_pathkeys != NIL &&
		input_rel->partial_pathlist != NIL)
	{
		Path	   *cheapest_partial_path;

		cheapest_partial_path = linitial(input_rel->partial_pathlist);

		/*
		 * 若总代价最低的部分路径不需要排序，这就与已经试过的
		 * 重复了。
		 */
		if (!pathkeys_contained_in(root->sort_pathkeys,
								   cheapest_partial_path->pathkeys))
		{
			Path	   *path;
			double		total_groups;

			path = (Path *) create_sort_path(root,
											 ordered_rel,
											 cheapest_partial_path,
											 root->sort_pathkeys,
											 limit_tuples);

			total_groups = cheapest_partial_path->rows *
				cheapest_partial_path->parallel_workers;
			path = (Path *)
				create_gather_merge_path(root, ordered_rel,
										 path,
										 path->pathtarget,
										 root->sort_pathkeys, NULL,
										 &total_groups);

			/* 若需要，加上投影步骤 */
			if (path->pathtarget != target)
				path = apply_projection_to_path(root, ordered_rel,
												path, target);

			add_path(ordered_rel, path);
		}

		/*
		 * 考虑在部分路径上用 gather merge 做增量排序。
		 *
		 * 当 sort_pathkeys 只有一项时，也可以跳过整个循环，
		 * 因为那时不可能有该列表的预排序
		 * 前缀，除非列表已完全排好序。
		 */
		if (enable_incremental_sort && list_length(root->sort_pathkeys) > 1)
		{
			foreach(lc, input_rel->partial_pathlist)
			{
				Path	   *input_path = (Path *) lfirst(lc);
				Path	   *sorted_path;
				bool		is_sorted;
				int			presorted_keys;
				double		total_groups;

				/*
				 * 我们不在乎这是不是总代价最低的部分路径——
				 * 不能简单跳过它，因为它也许已部分排好序，
				 * 那种情况下我们想考虑加上增量排序
				 * （而不是上面发生的完整排序）。
				 */

				is_sorted = pathkeys_count_contained_in(root->sort_pathkeys,
														input_path->pathkeys,
														&presorted_keys);

				/* 在已完全排好序的路径上加增量排序没有意义。 */
				if (is_sorted)
					continue;

				if (presorted_keys == 0)
					continue;

				/* 既然有预排序键，就考虑增量排序。 */
				sorted_path = (Path *) create_incremental_sort_path(root,
																	ordered_rel,
																	input_path,
																	root->sort_pathkeys,
																	presorted_keys,
																	limit_tuples);
				total_groups = input_path->rows *
					input_path->parallel_workers;
				sorted_path = (Path *)
					create_gather_merge_path(root, ordered_rel,
											 sorted_path,
											 sorted_path->pathtarget,
											 root->sort_pathkeys, NULL,
											 &total_groups);

				/* 若需要，加上投影步骤 */
				if (sorted_path->pathtarget != target)
					sorted_path = apply_projection_to_path(root, ordered_rel,
														   sorted_path, target);

				add_path(ordered_rel, sorted_path);
			}
		}
	}

	/*
	 * 若有一个 FDW 负责该查询的全部基关系，
	 * 就让它考虑添加 ForeignPaths。
	 */
	if (ordered_rel->fdwroutine &&
		ordered_rel->fdwroutine->GetForeignUpperPaths)
		ordered_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_ORDERED,
													  input_rel, ordered_rel,
													  NULL);

	/* 让扩展也许再添加一些路径 */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_ORDERED,
									input_rel, ordered_rel, NULL);

	/*
	 * 不必费心在这里做 set_cheapest；grouping_planner 不
	 * 需要我们做。
	 */
	Assert(ordered_rel->pathlist != NIL);

	return ordered_rel;
}


/*
 * make_group_input_target
 *	  为分组节点的初始输入生成适当的 PathTarget。
 *
 * 若有分组或聚合，扫描/连接子计划无法发出
 * 该查询的最终目标列表；例如，它肯定没法发出任何
 * 聚合函数调用。本例程为
 * 扫描/连接子计划生成正确的目标。
 *
 * 解析器传来的查询目标列表已经含有全部
 * ORDER BY 和 GROUP BY 表达式的项，但不会有
 * 只在 HAVING 子句里使用的变量的项；因此需要把那些
 * 变量加进子计划目标列表。另外，除 GROUP BY 项之外，我们把全部表达式
 * 展平成它们的组成变量；其他表达式
 * 将由上层计划节点而不是子计划计算。
 * 例如，给定这样的查询
 *		SELECT a+b,SUM(c+d) FROM table GROUP BY a+b;
 * 我们想把这份目标列表传给子计划：
 *		a+b,c,d
 * 其中 a+b 目标将由 Sort/Group 步骤使用，而
 * 其他目标将用于计算结果。
 *
 * 'final_target' 是该查询的最终目标列表（PathTarget 形式）
 *
 * 结果是 query_planner() 返回的 Path 要计算的
 * PathTarget。
 */
static PathTarget *
make_group_input_target(PlannerInfo *root, PathTarget *final_target)
{
	Query	   *parse = root->parse;
	PathTarget *input_target;
	List	   *non_group_cols;
	List	   *non_group_vars;
	int			i;
	ListCell   *lc;

	/*
	 * 必须建立一个包含全部分组列、外加查询目标列表和 HAVING 限定条件里提到的任何其他
	 * Var 的目标。
	 */
	input_target = create_empty_pathtarget();
	non_group_cols = NIL;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(final_target, i);

		if (sgref && root->processed_groupClause &&
			get_sortgroupref_clause_noerr(sgref,
										  root->processed_groupClause) != NULL)
		{
			/*
			 * 它是分组列，因此按原样加进输入目标。
			 */
			add_column_to_pathtarget(input_target, expr, sgref);
		}
		else
		{
			/*
			 * 非分组列，因此只记住该表达式，供以后
			 * 调用 pull_var_clause。
			 */
			non_group_cols = lappend(non_group_cols, expr);
		}

		i++;
	}

	/*
	 * 若有 HAVING 子句，也需要它使用的 Var。
	 */
	if (parse->havingQual)
		non_group_cols = lappend(non_group_cols, parse->havingQual);

	/*
	 * 抽出非分组列（外加 HAVING）里提到的全部 Var，若尚未
	 * 出现就加进输入目标。（直接用作
	 * GROUP BY 项的 Var 已经在场。）注意这也
	 * 包括 resjunk 项里使用的 Var，因此覆盖了
	 * ORDER BY 和窗口说明的需要。Aggref 和
	 * WindowFuncs 内部使用的 Var 也会在这里抽出。
	 */
	non_group_vars = pull_var_clause((Node *) non_group_cols,
									 PVC_RECURSE_AGGREGATES |
									 PVC_RECURSE_WINDOWFUNCS |
									 PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(input_target, non_group_vars);

	/* 清理杂物 */
	list_free(non_group_vars);
	list_free(non_group_cols);

	/* XXX 这会造成一些冗余的代价计算…… */
	return set_pathtarget_cost_width(root, input_target);
}

/*
 * make_partial_grouping_target
 *	  为部分聚合
 *	  （或若没有聚合，则为部分分组）节点的输出生成适当的 PathTarget。
 *
 * 部分聚合节点需要发出常规聚合节点会发出的全部同样聚合，
 * 外加 HAVING 里使用的任何聚合；
 * 只不过 Aggref 节点应当标成部分聚合。
 *
 * 另外，最好发出聚合 tlist 和 HAVING 里
 * Aggref 之外使用的任何 Var 和 PlaceHolderVars。（想必
 * 这些是被分组或用于分组表达式的 Var。）
 *
 * grouping_target 是最顶层聚合步骤要发出的 tlist。
 * havingQual 表示 HAVING 子句。
 */
static PathTarget *
make_partial_grouping_target(PlannerInfo *root,
							 PathTarget *grouping_target,
							 Node *havingQual)
{
	PathTarget *partial_target;
	List	   *non_group_cols;
	List	   *non_group_exprs;
	int			i;
	ListCell   *lc;

	partial_target = create_empty_pathtarget();
	non_group_cols = NIL;

	i = 0;
	foreach(lc, grouping_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(grouping_target, i);

		if (sgref && root->processed_groupClause &&
			get_sortgroupref_clause_noerr(sgref,
										  root->processed_groupClause) != NULL)
		{
			/*
			 * 它是分组列，因此按原样加进 partial_target。
			 * （这允许上层聚合步骤重复分组计算。）
			 */
			add_column_to_pathtarget(partial_target, expr, sgref);
		}
		else
		{
			/*
			 * 非分组列，因此只记住该表达式，供以后
			 * 调用 pull_var_clause。
			 */
			non_group_cols = lappend(non_group_cols, expr);
		}

		i++;
	}

	/*
	 * 若有 HAVING 子句，也需要它使用的 Var/Aggref。
	 */
	if (havingQual)
		non_group_cols = lappend(non_group_cols, havingQual);

	/*
	 * 抽出非分组列（外加 HAVING）里提到的全部 Var、PlaceHolderVars 和 Aggref，若尚未
	 * 出现就加进 partial_target。
	 * （直接用作 GROUP BY 项的表达式
	 * 已经在场。）注意这也包括 resjunk 项里使用的 Var，因此
	 * 覆盖了 ORDER BY 和窗口说明的需要。
	 */
	non_group_exprs = pull_var_clause((Node *) non_group_cols,
									  PVC_INCLUDE_AGGREGATES |
									  PVC_RECURSE_WINDOWFUNCS |
									  PVC_INCLUDE_PLACEHOLDERS);

	add_new_columns_to_pathtarget(partial_target, non_group_exprs);

	/*
	 * 调整 Aggref，把它们放进部分模式。此时全部 Aggref
	 * 都在目标列表的顶层，因此可以直接扫描该列表，
	 * 而不必递归表达式树。
	 */
	foreach(lc, partial_target->exprs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(lc);

		if (IsA(aggref, Aggref))
		{
			Aggref	   *newaggref;

			/*
			 * 不应当需要复制 Aggref 节点的子结构，
			 * 但要平复制节点本身，以免损坏其他树。
			 */
			newaggref = makeNode(Aggref);
			memcpy(newaggref, aggref, sizeof(Aggref));

			/* 目前，假定需要序列化 */
			mark_partial_aggref(newaggref, AGGSPLIT_INITIAL_SERIAL);

			lfirst(lc) = newaggref;
		}
	}

	/* 清理杂物 */
	list_free(non_group_exprs);
	list_free(non_group_cols);

	/* XXX 这会造成一些冗余的代价计算…… */
	return set_pathtarget_cost_width(root, partial_target);
}

/*
 * mark_partial_aggref
 *	  调整一个 Aggref，使它表示部分聚合步骤。
 *
 * Aggref 节点被就地修改；调用方必须做任何所需的复制。
 */
void
mark_partial_aggref(Aggref *agg, AggSplit aggsplit)
{
	/* 到这时应当已经算出 aggtranstype */
	Assert(OidIsValid(agg->aggtranstype));
	/* ……但 aggsplit 仍应是解析器留下的样子 */
	Assert(agg->aggsplit == AGGSPLIT_SIMPLE);

	/* 用预定的部分聚合模式标注该 Aggref */
	agg->aggsplit = aggsplit;

	/*
	 * 若需要，调整结果类型。通常，部分聚合返回
	 * 该聚合的转换类型；但若那是 INTERNAL 且我们正在
	 * 序列化，它就改为返回 BYTEA。
	 */
	if (DO_AGGSPLIT_SKIPFINAL(aggsplit))
	{
		if (agg->aggtranstype == INTERNALOID && DO_AGGSPLIT_SERIALIZE(aggsplit))
			agg->aggtype = BYTEAOID;
		else
			agg->aggtype = agg->aggtranstype;
	}
}

/*
 * postprocess_setop_tlist
 *	  修正 plan_set_operations() 返回的目标列表。
 *
 * 需要把排序键信息从 orig_tlist 转置进 new_tlist。
 * 注意：若我们支持集合运算结果的 resjunk 排序键，
 * 这就不够了——那时需要投影一份全新的
 * tlist 来求值 resjunk 列。目前，若在
 * orig_tlist 里发现任何 resjunk 列，就直接 ereport。
 */
static List *
postprocess_setop_tlist(List *new_tlist, List *orig_tlist)
{
	ListCell   *l;
	ListCell   *orig_tlist_item = list_head(orig_tlist);

	foreach(l, new_tlist)
	{
		TargetEntry *new_tle = lfirst_node(TargetEntry, l);
		TargetEntry *orig_tle;

		/* 忽略集合运算结果里的 resjunk 列 */
		if (new_tle->resjunk)
			continue;

		Assert(orig_tlist_item != NULL);
		orig_tle = lfirst_node(TargetEntry, orig_tlist_item);
		orig_tlist_item = lnext(orig_tlist, orig_tlist_item);
		if (orig_tle->resjunk)	/* 不应当发生 */
			elog(ERROR, "resjunk output columns are not implemented");
		Assert(new_tle->resno == orig_tle->resno);
		new_tle->ressortgroupref = orig_tle->ressortgroupref;
	}
	if (orig_tlist_item != NULL)
		elog(ERROR, "resjunk output columns are not implemented");
	return new_tlist;
}

/*
 * optimize_window_clauses
 *		调用每个 WindowFunc 的 prosupport 函数，看能否
 *		对任何 WindowClause 做调整，让执行器
 *		以更优的方式执行窗口函数。
 *
 * 目前只允许调整 WindowClause 的 frameOptions。以后
 * 也许允许在这里做更多事。
 */
static void
optimize_window_clauses(PlannerInfo *root, WindowFuncLists *wflists)
{
	List	   *windowClause = root->parse->windowClause;
	ListCell   *lc;

	foreach(lc, windowClause)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);
		ListCell   *lc2;
		int			optimizedFrameOptions = 0;

		Assert(wc->winref <= wflists->maxWinRef);

		/* 跳过没有任何 WindowFuncs 的 WindowClauses */
		if (wflists->windowFuncs[wc->winref] == NIL)
			continue;

		foreach(lc2, wflists->windowFuncs[wc->winref])
		{
			SupportRequestOptimizeWindowClause req;
			SupportRequestOptimizeWindowClause *res;
			WindowFunc *wfunc = lfirst_node(WindowFunc, lc2);
			Oid			prosupport;

			prosupport = get_func_support(wfunc->winfnoid);

			/* 检查 'wfunc' 是否有支撑函数 */
			if (!OidIsValid(prosupport))
				break;			/* 无法优化这个 WindowClause */

			req.type = T_SupportRequestOptimizeWindowClause;
			req.window_clause = wc;
			req.window_func = wfunc;
			req.frameOptions = wc->frameOptions;

			/* 调用支撑函数 */
			res = (SupportRequestOptimizeWindowClause *)
				DatumGetPointer(OidFunctionCall1(prosupport,
												 PointerGetDatum(&req)));

			/*
			 * 若支撑函数不
			 * 支持这种请求类型，就跳到下一个 WindowClause。
			 */
			if (res == NULL)
				break;

			/*
			 * 为这个 WindowClause 的第一个 WindowFunc
			 * 保存这些 frameOptions。
			 */
			if (foreach_current_index(lc2) == 0)
				optimizedFrameOptions = res->frameOptions;

			/*
			 * 对后续的 WindowFuncs，若 frameOptions 不相同，
			 * 就没法优化这个
			 * WindowClause 的 frameOptions。
			 */
			else if (optimizedFrameOptions != res->frameOptions)
				break;			/* 若有，就跳到下一个 WindowClause */
		}

		/* 若全部 WindowFunc 都同意可以，就调整 frameOptions */
		if (lc2 == NULL && wc->frameOptions != optimizedFrameOptions)
		{
			ListCell   *lc3;

			/* 应用新的帧选项 */
			wc->frameOptions = optimizedFrameOptions;

			/*
			 * 现在检查改变 frameOptions 是否使
			 * 这个 WindowClause 与某个其他 WindowClause 重复。
			 * 这只有在有多个 WindowClauses 时才会发生，因此
			 * 若只有 1 个就不必费心。
			 */
			if (list_length(windowClause) == 1)
				continue;

			/*
			 * 做重复检查，若
			 * 找到重复就复用现有的 WindowClause。
			 */
			foreach(lc3, windowClause)
			{
				WindowClause *existing_wc = lfirst_node(WindowClause, lc3);

				/* 跳过当前正在编辑的 WindowClause */
				if (existing_wc == wc)
					continue;

				/*
				 * 做与 transformWindowFuncCall 里相同的
				 * 重复检查。
				 */
				if (equal(wc->partitionClause, existing_wc->partitionClause) &&
					equal(wc->orderClause, existing_wc->orderClause) &&
					wc->frameOptions == existing_wc->frameOptions &&
					equal(wc->startOffset, existing_wc->startOffset) &&
					equal(wc->endOffset, existing_wc->endOffset))
				{
					ListCell   *lc4;

					/*
					 * 现在把 'wc' 里的每个 WindowFunc 移进 'existing_wc'。
					 * 这需要调整每个 WindowFunc 的 winref，并把
					 * 'wc' 里的 WindowFuncs 移到
					 * 'existing_wc' 的 WindowFuncs 列表。
					 */
					foreach(lc4, wflists->windowFuncs[wc->winref])
					{
						WindowFunc *wfunc = lfirst_node(WindowFunc, lc4);

						wfunc->winref = existing_wc->winref;
					}

					/* 移动列表项 */
					wflists->windowFuncs[existing_wc->winref] = list_concat(wflists->windowFuncs[existing_wc->winref],
																			wflists->windowFuncs[wc->winref]);
					wflists->windowFuncs[wc->winref] = NIL;

					/*
					 * transformWindowFuncCall() 应当已经确保没有
					 * 其他重复，因此不必费心再往
					 * 下看。
					 */
					break;
				}
			}
		}
	}
}

/*
 * select_active_windows
 *		建立"活动"窗口子句的列表（即被
 *		未删除的 WindowFuncs 引用的那些），按它们将要执行的顺序。
 */
static List *
select_active_windows(PlannerInfo *root, WindowFuncLists *wflists)
{
	List	   *windowClause = root->parse->windowClause;
	List	   *result = NIL;
	ListCell   *lc;
	int			nActive = 0;
	WindowClauseSortData *actives = palloc(sizeof(WindowClauseSortData)
										   * list_length(windowClause));

	/* 首先，构造活动窗口的数组 */
	foreach(lc, windowClause)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);

		/* 只有当 wflists 显示有相关 WindowFuncs 时它才活动 */
		Assert(wc->winref <= wflists->maxWinRef);
		if (wflists->windowFuncs[wc->winref] == NIL)
			continue;

		actives[nActive].wc = wc;	/* 原来的子句 */

		/*
		 * 为了排序，我们想要分区键列表后面跟着
		 * 排序键列表。但路径键构造会去掉两者之间的重复，
		 * 因此我们也可以（尽管没法检测全部
		 * 重复，因为有些也许来自等价类——那也许意味着
		 * 我们在这里错过优化机会）。不过必须确保
		 * 我们所保留的那些项的顺序
		 * 得到保持。
		 *
		 * partitionClause 和 orderClause 在
		 * 语法分析里已经去掉了各自的重复，因此这里只关心去掉
		 * 也出现在 partitionClause 里的 orderClause 项。
		 */
		actives[nActive].uniqueOrder =
			list_concat_unique(list_copy(wc->partitionClause),
							   wc->orderClause);
		nActive++;
	}

	/*
	 * 按分区/排序子句对活动窗口排序，忽略
	 * 任何帧子句，这样需要同样排序的窗口在列表里
	 * 相邻。等我们来生成路径时，这会避免
	 * 插入额外的 Sort 节点。
	 *
	 * 我们这样实现 SQL 标准的一项具体要求，
	 * 它说当两个或更多窗口排序等价（即
	 * 分区和排序子句匹配，即使它们的名字或
	 * 帧子句不同）时，全部对等行在所有这些窗口里都必须以
	 * 同样的顺序呈现。若对这类情形允许多个排序节点，
	 * 就有风险因排序不稳定而使对等行在
	 * 等价窗口里以不同顺序结束。（见 SQL2008 - SQL2016 中
	 * <window clause> 的 General Rule 4。）
	 *
	 * 另外，若一个窗口的整个子句列表是另一个的前缀，
	 * 就把排序要求更强的窗口放在前面。
	 * 这样我们会先为更强的窗口排序，而不必为
	 * 更弱的那个再排一次。
	 */
	qsort(actives, nActive, sizeof(WindowClauseSortData), common_prefix_cmp);

	/* 建立原来的 WindowClause 节点的有序列表 */
	for (int i = 0; i < nActive; i++)
		result = lappend(result, actives[i].wc);

	pfree(actives);

	return result;
}

/*
 * common_prefix_cmp
 *	  WindowClauseSortData 的 QSort 比较函数
 *
 * 按所需排序子句对窗口排序。首先比较排序
 * 子句本身。其次，若一个窗口的子句是另一个
 * 的子句的前缀，就把排序子句更多的窗口放在前面。
 *
 * 我们有意先按最高的 tleSortGroupRef 排序。由于
 * tleSortGroupRef 先赋给查询的 DISTINCT 和 ORDER BY，
 * 而这里我们把最低的 tleSortGroupRef 排在最后，若某个
 * WindowClause 与查询的 DISTINCT 或
 * ORDER BY 子句共享一个 tleSortGroupRef，这就使最终的 WindowAgg 更可能
 * 为查询的 DISTINCT 或 ORDER BY 子句提供预排序输入，从而
 * 减少该查询所需的排序总次数。
 */
static int
common_prefix_cmp(const void *a, const void *b)
{
	const WindowClauseSortData *wcsa = a;
	const WindowClauseSortData *wcsb = b;
	ListCell   *item_a;
	ListCell   *item_b;

	forboth(item_a, wcsa->uniqueOrder, item_b, wcsb->uniqueOrder)
	{
		SortGroupClause *sca = lfirst_node(SortGroupClause, item_a);
		SortGroupClause *scb = lfirst_node(SortGroupClause, item_b);

		if (sca->tleSortGroupRef > scb->tleSortGroupRef)
			return -1;
		else if (sca->tleSortGroupRef < scb->tleSortGroupRef)
			return 1;
		else if (sca->sortop > scb->sortop)
			return -1;
		else if (sca->sortop < scb->sortop)
			return 1;
		else if (sca->nulls_first && !scb->nulls_first)
			return -1;
		else if (!sca->nulls_first && scb->nulls_first)
			return 1;
		/* 不必比较 eqop，因为它完全由 sortop 决定 */
	}

	if (list_length(wcsa->uniqueOrder) > list_length(wcsb->uniqueOrder))
		return -1;
	else if (list_length(wcsa->uniqueOrder) < list_length(wcsb->uniqueOrder))
		return 1;

	return 0;
}

/*
 * make_window_input_target
 *	  为 WindowAgg 节点的初始输入生成适当的 PathTarget。
 *
 * 当查询有窗口函数时，本函数计算紧挨第一个 WindowAgg 之下的节点
 * 应当计算的目标。
 * 这份 tlist 必须包含求值窗口函数、
 * 计算最终目标列表以及做任何所需最终排序步骤所需的全部值。
 * 若需要多个 WindowAggs，每个中间的都把其窗口
 * 函数结果加到这份基础 tlist 上；只有最顶层的 WindowAgg 计算
 * 实际想要的目标列表。
 *
 * 本函数很像 make_group_input_target，虽然还没像到
 * 可以共享代码。与那个函数一样，我们把多数表达式
 * 展平成它们的组成变量。但不想展平窗口
 * PARTITION BY/ORDER BY 子句，因为那也许导致对它们多次
 * 求值，那会很糟（若它们含有易变函数，甚至可能得出
 * 不一致的答案）。
 * 另外，不得展平 make_group_input_target 留下未展平的 GROUP BY 子句，
 * 因为我们也许不再能访问
 * 其中的各个 Var。
 *
 * 与 make_group_input_target 的另一个关键区别是，我们不
 * 展平 Aggref 表达式，因为那些要在
 * 窗口函数之下计算，在其上则像 Var 一样被引用。
 *
 * 'final_target' 是该查询的最终目标列表（PathTarget 形式）
 * 'activeWindows' 是先前由
 *			select_active_windows 识别出的活动窗口列表。
 *
 * 结果是紧挨第一个 WindowAgg 节点之下的计划节点要计算的
 * PathTarget。
 */
static PathTarget *
make_window_input_target(PlannerInfo *root,
						 PathTarget *final_target,
						 List *activeWindows)
{
	PathTarget *input_target;
	Bitmapset  *sgrefs;
	List	   *flattenable_cols;
	List	   *flattenable_vars;
	int			i;
	ListCell   *lc;

	Assert(root->parse->hasWindowFuncs);

	/*
	 * 把窗口 PARTITION/ORDER BY 子句的 sortgroupref 号
	 * 收进一个位图集，便于下面引用。
	 */
	sgrefs = NULL;
	foreach(lc, activeWindows)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);
		ListCell   *lc2;

		foreach(lc2, wc->partitionClause)
		{
			SortGroupClause *sortcl = lfirst_node(SortGroupClause, lc2);

			sgrefs = bms_add_member(sgrefs, sortcl->tleSortGroupRef);
		}
		foreach(lc2, wc->orderClause)
		{
			SortGroupClause *sortcl = lfirst_node(SortGroupClause, lc2);

			sgrefs = bms_add_member(sgrefs, sortcl->tleSortGroupRef);
		}
	}

	/* 也把 GROUP BY 子句的 sortgroupref 号加进去 */
	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *grpcl = lfirst_node(SortGroupClause, lc);

		sgrefs = bms_add_member(sgrefs, grpcl->tleSortGroupRef);
	}

	/*
	 * 构造一个包含全部不可展平目标列表项的目标，
	 * 并暂时把其他项存到一边。
	 */
	input_target = create_empty_pathtarget();
	flattenable_cols = NIL;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(final_target, i);

		/*
		 * 不想拆开窗口子句或 GROUP BY 项。（注意
		 * 这类项没法包含窗口函数，因此在
		 * WindowAgg 节点之下计算它们没问题。）
		 */
		if (sgref != 0 && bms_is_member(sgref, sgrefs))
		{
			/*
			 * 不想拆开这个值，因此按原样加进输入
			 * 目标。
			 */
			add_column_to_pathtarget(input_target, expr, sgref);
		}
		else
		{
			/*
			 * 该列要被展平，因此只记住该表达式，供
			 * 以后调用 pull_var_clause。
			 */
			flattenable_cols = lappend(flattenable_cols, expr);
		}

		i++;
	}

	/*
	 * 抽出可展平列里提到的全部 Var 和 Aggref，若尚未
	 * 出现就加进输入目标。（有些也许已经
	 * 在那里，因为它们被直接用作窗口/分组子句。）
	 *
	 * 注意：这里必须使用 PVC_INCLUDE_AGGREGATES，以便任何
	 * Aggref 被放进 Agg 节点的 tlist，而不是留到
	 * 更高层去计算。另一方面，应当递归进
	 * WindowFuncs，以确保它们的输入表达式可用。
	 */
	flattenable_vars = pull_var_clause((Node *) flattenable_cols,
									   PVC_INCLUDE_AGGREGATES |
									   PVC_RECURSE_WINDOWFUNCS |
									   PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(input_target, flattenable_vars);

	/* 清理杂物 */
	list_free(flattenable_vars);
	list_free(flattenable_cols);

	/* XXX 这会造成一些冗余的代价计算…… */
	return set_pathtarget_cost_width(root, input_target);
}

/*
 * make_pathkeys_for_window
 *		建立一份描述给定 WindowClause 所需输入顺序的
 *		路径键列表。
 *
 * 所需顺序首先是 PARTITION 键，然后是 ORDER 键。
 * 将来也许试着用哈希实现窗口，那种
 * 情况下顺序可以放宽，但目前我们总是排序。
 */
static List *
make_pathkeys_for_window(PlannerInfo *root, WindowClause *wc,
						 List *tlist)
{
	List	   *window_pathkeys;
	List	   *window_sortclauses;

	/* 若无法排序就抛出错误 */
	if (!grouping_is_sortable(wc->partitionClause))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement window PARTITION BY"),
				 errdetail("Window partitioning columns must be of sortable datatypes.")));
	if (!grouping_is_sortable(wc->orderClause))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement window ORDER BY"),
				 errdetail("Window ordering columns must be of sortable datatypes.")));

	/* 好，做出合并后的路径键 */
	window_sortclauses = list_concat_copy(wc->partitionClause, wc->orderClause);
	window_pathkeys = make_pathkeys_for_sortclauses(root,
													window_sortclauses,
													tlist);
	list_free(window_sortclauses);
	return window_pathkeys;
}

/*
 * make_sort_input_target
 *	  为 Sort 步骤的初始输入生成适当的 PathTarget。
 *
 * 若查询有 ORDER BY，本函数选择紧挨 Sort 之下的节点要计算的目标
 * （以及 DISTINCT，若有，因为 Unique 没法
 * 投影）。这也许与查询的最终
 * 输出目标相同，也许不同。
 *
 * 让排序输入 tlist 与最终目标保持相同的主要论据
 * 是我们避免一个单独的投影节点（若它们
 * 不同就会需要，因为 Sort 没法投影）。不过，把 tlist 求值推迟到排序之后也有
 * 好处：它确保
 * tlist 里任何易变函数的求值顺序一致，
 * 而且若还有 LIMIT，我们可以在从未计算
 * 后面那些行的 tlist 函数时就停掉查询，这对易变函数和
 * 昂贵函数都有益。
 *
 * 我们目前的策略是无条件把易变表达式推迟到排序之后
 * （假定那是可能的，即它们在普通 tlist
 * 列里，而不在 ORDER BY/GROUP BY/DISTINCT 列里）。我们也更愿意
 * 推迟返回集合的表达式，因为事先运行它们会
 * 使排序数据集膨胀，而且若排序不稳定，也许会造成意外的输出顺序。
 * 不过对此有一个约束：tlist 里的全部 SRFs
 * 应当在同一步计划里求值，以便它们能
 * 在 nodeProjectSet 里同步运行。因此若任何 SRFs 在排序列里，就
 * 不得推迟任何 SRFs。（注意原则上该策略大概
 * 也应当应用到组/窗口输入目标列表，但我们
 * 历史上没那么做。）最后，若有 LIMIT，或若 root->tuple_fraction 表明
 * 可以对该查询做部分求值（若两者都不为真，我们预期
 * 反正得对每一行求值这些表达式），或若有
 * 任何易变或返回集合的表达式（因为一旦已经放进一个
 * 投影，再推迟更多东西不会增加代价），就推迟昂贵表达式。
 * 这里也许还可以考虑的另一个问题是，
 *
 * 求值 tlist 表达式得出的数据也许比输入 Var 更宽
 * 或更窄，从而改变必须经过 Sort 的数据量。
 * 不过，对任何比 Var 更复杂的表达式，我们通常只有非常糟的
 * 输出宽度概念，
 * 因此目前基于这一点去优化似乎太冒险。
 * 注意若我们确实产生了修改过的排序输入目标，而查询
 *
 * 最终没用显式 Sort，并没有特别的害处：
 * 我们起初会对前面的路径节点使用修改过的目标，
 * 但随后用 apply_projection_to_path 把它们改成最终目标。
 * 而且在这种情况下，关于易变函数求值顺序的保证
 * 仍然成立，因为行已经排好序。
 * 本函数与 make_group_input_target 和
 *
 * make_window_input_target 有一些共同之处，尽管做什么的具体规则
 * 不同。我们从不展平/推迟任何分组或排序列；
 * 那些在排序之前就需要。若确实展平某个
 * 表达式，就不动 Aggref 和 WindowFunc 节点，因为那些
 * 更早已经算过。
 * 'final_target' 是该查询的最终目标列表（PathTarget 形式）
 *
 * 'have_postponed_srfs' 是一个输出参数，见下
 * 结果是紧挨 Sort 步骤（以及 Distinct 步骤，若有）之下的
 *
 * 计划节点要计算的 PathTarget。
 * 若判定投影步骤没有帮助，这就会
 * 正好是 final_target。
 *
 * 另外，若选择把任何返回集合的函数推迟到
 * Sort 之后，就把 *have_postponed_srfs 设为真。
 */
static PathTarget *
make_sort_input_target(PlannerInfo *root,
					   PathTarget *final_target,
					   bool *have_postponed_srfs)
{
	Query	   *parse = root->parse;
	PathTarget *input_target;
	int			ncols;
	bool	   *col_is_srf;
	bool	   *postpone_col;
	bool		have_srf;
	bool		have_volatile;
	bool		have_expensive;
	bool		have_srf_sortcols;
	bool		postpone_srfs;
	List	   *postponable_cols;
	List	   *postponable_vars;
	int			i;
	ListCell   *lc;

	/* 除非查询有 ORDER BY，否则不应当到这里 */
	Assert(parse->sortClause);

	*have_postponed_srfs = false;	/* 默认结果 */

	/* 检查 tlist 并收集按列的信息 */
	ncols = list_length(final_target->exprs);
	col_is_srf = (bool *) palloc0(ncols * sizeof(bool));
	postpone_col = (bool *) palloc0(ncols * sizeof(bool));
	have_srf = have_volatile = have_expensive = have_srf_sortcols = false;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		/*
		 * 若该列有 sortgroupref，就假定它必须在
		 * 排序之前求值。一般这类列会是 ORDER BY、GROUP
		 * BY 等目标。一个例外是被 remove_useless_groupby_columns() 从
		 * GROUP BY 去掉的列……但那些
		 * 反正只会是 Var。似乎没有任何情形值得
		 * 费心再复查。
		 */
		if (get_pathtarget_sortgroupref(final_target, i) == 0)
		{
			/*
			 * 检查 SRF 或易变函数。先检查 SRF 情形，
			 * 因为必须知道是否有任何被推迟的 SRFs。
			 */
			if (parse->hasTargetSRFs &&
				expression_returns_set((Node *) expr))
			{
				/* 下面再决定这些是否可推迟 */
				col_is_srf[i] = true;
				have_srf = true;
			}
			else if (contain_volatile_functions((Node *) expr))
			{
				/* 无条件推迟 */
				postpone_col[i] = true;
				have_volatile = true;
			}
			else
			{
				/*
				 * 否则检查代价。XXX 烦人的是 set_pathtarget_cost_width() 刚刚做过，
				 * 这里还得再做。重构以
				 * 允许共享这项工作？
				 */
				QualCost	cost;

				cost_qual_eval_node(&cost, (Node *) expr, root);

				/*
				 * 我们任意把"昂贵"定义为"超过 10 倍
				 * cpu_operator_cost"。注意这会包括任何
				 * 使用默认代价的 PL 函数。
				 */
				if (cost.per_tuple > 10 * cpu_operator_cost)
				{
					postpone_col[i] = true;
					have_expensive = true;
				}
			}
		}
		else
		{
			/* 对 sortgroupref 列，只检查是否有任何含有 SRFs */
			if (!have_srf_sortcols &&
				parse->hasTargetSRFs &&
				expression_returns_set((Node *) expr))
				have_srf_sortcols = true;
		}

		i++;
	}

	/*
	 * 若有一些 SRFs 但都不在 sortgroupref 列里，就可以推迟 SRFs。
	 */
	postpone_srfs = (have_srf && !have_srf_sortcols);

	/*
	 * 若不需要排序后投影，就直接返回 final_target。
	 */
	if (!(postpone_srfs || have_volatile ||
		  (have_expensive &&
		   (parse->limitCount || root->tuple_fraction > 0))))
		return final_target;

	/*
	 * 报告排序后投影是否会包含返回集合的
	 * 函数。这很重要，因为它影响 Sort 能否
	 * 依靠查询的 LIMIT（若有）来限制它需要返回的
	 * 行数。
	 */
	*have_postponed_srfs = postpone_srfs;

	/*
	 * 构造排序输入目标，取全部不可推迟的列，然后
	 * 加上在可推迟的那些里找到的 Var、PlaceHolderVars、Aggref
	 * 和 WindowFuncs。
	 */
	input_target = create_empty_pathtarget();
	postponable_cols = NIL;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		if (postpone_col[i] || (postpone_srfs && col_is_srf[i]))
			postponable_cols = lappend(postponable_cols, expr);
		else
			add_column_to_pathtarget(input_target, expr,
									 get_pathtarget_sortgroupref(final_target, i));

		i++;
	}

	/*
	 * 抽出可推迟列里提到的全部 Var、Aggref 和 WindowFuncs，若尚未
	 * 出现就加进排序输入目标。
	 * （有些也许已经在那里。）这里不得
	 * 拆开 Aggref 或 WindowFunc，因为投影节点
	 * 将没法重新计算它们。
	 */
	postponable_vars = pull_var_clause((Node *) postponable_cols,
									   PVC_INCLUDE_AGGREGATES |
									   PVC_INCLUDE_WINDOWFUNCS |
									   PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(input_target, postponable_vars);

	/* 清理杂物 */
	list_free(postponable_vars);
	list_free(postponable_cols);

	/* XXX 这代表更多冗余的代价计算…… */
	return set_pathtarget_cost_width(root, input_target);
}

/*
 * get_cheapest_fractional_path
 *	  找出取出给定关系预期返回的全部
 *	  元组中指定比例时总代价最低的路径。
 *
 * 我们按与 grouping_planner 相同的方式解释 tuple_fraction。
 *
 * 我们假定已经对给定关系跑过 set_cheapest()。
 */
Path *
get_cheapest_fractional_path(RelOptInfo *rel, double tuple_fraction)
{
	Path	   *best_path = rel->cheapest_total_path;
	ListCell   *l;

	/* 若将取出全部元组，就直接返回总代价最低的路径 */
	if (tuple_fraction <= 0.0)
		return best_path;

	/* 把绝对元组数转换成比例；不必夹到 0..1 */
	if (tuple_fraction >= 1.0 && best_path->rows > 0)
		tuple_fraction /= best_path->rows;

	foreach(l, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(l);

		if (path == rel->cheapest_total_path ||
			compare_fractional_path_costs(best_path, path, tuple_fraction) <= 0)
			continue;

		best_path = path;
	}

	return best_path;
}

/*
 * adjust_paths_for_srfs
 *		修正给定上层关系的 Path，以正确处理 tSRF。
 *
 * 执行器只能处理出现在 ProjectSet 计划节点目标列表
 * 顶层的返回集合函数。若有任何不在顶层的 SRFs，
 * 就需要把求值拆成多层
 * 计划，使每一层都满足这个约束。本函数
 * 修改上层关系中（也许）在其
 * 输出 tlist 里计算任何 SRFs 的每条 Path，插入适当的投影步骤。
 *
 * 给定的 targets 和 targets_contain_srfs 列表来自
 * split_pathtarget_at_srfs()。我们假定现有 Path 发出 targets 里的第一个
 * 目标。
 */
static void
adjust_paths_for_srfs(PlannerInfo *root, RelOptInfo *rel,
					  List *targets, List *targets_contain_srfs)
{
	ListCell   *lc;

	Assert(list_length(targets) == list_length(targets_contain_srfs));
	Assert(!linitial_int(targets_contain_srfs));

	/* 若这一计划层没有 SRFs 出现，就无事可做 */
	if (list_length(targets) == 1)
		return;

	/*
	 * 在该关系的每条路径之上叠上 SRF 求值节点。
	 *
	 * 原则上应当在这里重跑 set_cheapest() 来识别
	 * 总代价最低的路径，但给全部路径加上同样的 tlist 求值
	 * 代价似乎不太可能改变那个，因此不费心。改为
	 * 假定启动代价最低和总代价最低的路径仍然
	 * 如此。（不应当再有参数化路径，因此不必
	 * 操心更新 cheapest_parameterized_paths。）
	 */
	foreach(lc, rel->pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		Path	   *newpath = subpath;
		ListCell   *lc1,
				   *lc2;

		Assert(subpath->param_info == NULL);
		forboth(lc1, targets, lc2, targets_contain_srfs)
		{
			PathTarget *thistarget = lfirst_node(PathTarget, lc1);
			bool		contains_srfs = (bool) lfirst_int(lc2);

			/* 若这一层不含 SRFs，就做常规投影 */
			if (contains_srfs)
				newpath = (Path *) create_set_projection_path(root,
															  rel,
															  newpath,
															  thistarget);
			else
				newpath = (Path *) apply_projection_to_path(root,
															rel,
															newpath,
															thistarget);
		}
		lfirst(lc) = newpath;
		if (subpath == rel->cheapest_startup_path)
			rel->cheapest_startup_path = newpath;
		if (subpath == rel->cheapest_total_path)
			rel->cheapest_total_path = newpath;
	}

	/* 若有，部分路径同样如此 */
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		Path	   *newpath = subpath;
		ListCell   *lc1,
				   *lc2;

		Assert(subpath->param_info == NULL);
		forboth(lc1, targets, lc2, targets_contain_srfs)
		{
			PathTarget *thistarget = lfirst_node(PathTarget, lc1);
			bool		contains_srfs = (bool) lfirst_int(lc2);

			/* 若这一层不含 SRFs，就做常规投影 */
			if (contains_srfs)
				newpath = (Path *) create_set_projection_path(root,
															  rel,
															  newpath,
															  thistarget);
			else
			{
				/* 避免 apply_projection_to_path，以防多次引用 */
				newpath = (Path *) create_projection_path(root,
														  rel,
														  newpath,
														  thistarget);
			}
		}
		lfirst(lc) = newpath;
	}
}

/*
 * expression_planner
 *		对独立表达式做规划器的变换。
 *
 * 各种实用命令需要计算不属于
 * 可规划查询一部分的表达式。它们可以用执行器的常规
 * 表达式执行机制来做，但表达式必须先送到
 * 这里，从解析器输出变换成可执行的东西。
 *
 * 目前，我们不允许独立表达式里有子链接，因此这里没有
 * 真正的"规划"。（不过那也许并不总是如此。）
 * 我们必须做的是跑 eval_const_expressions，以确保任何函数
 * 调用都被转换成位置表示法，并且插入函数默认参数。
 * 常量子表达式被简化这一事实是一个
 * 副作用，当表达式会被求值不止
 * 一次时有用。另外，必须修正操作符函数 ID。
 *
 * 这不返回关于该表达式依赖的任何信息。
 * 因此调用方应当只在当前
 * 查询期间使用结果。想把结果缓存更久的调用方应当使用
 * expression_planner_with_deps，大概经由计划缓存。
 *
 * 注意：这不得对传入的表达式
 * 树做任何有害的改动。（实际上对它应用 fix_opfuncids 是可以的，但由于
 * 我们先做基于 expression_tree_mutator 的遍历，返回的将
 * 是一棵新的节点树。）结果在当前内存
 * 上下文里构造；注意这也可能在那里泄漏很多额外的东西。
 */
Expr *
expression_planner(Expr *expr)
{
	Node	   *result;

	/*
	 * 转换命名参数的函数调用，插入默认参数，并
	 * 简化常量子表达式
	 */
	result = eval_const_expressions(NULL, (Node *) expr);

	/* 若缺少，就填上 opfuncid 值 */
	fix_opfuncids(result);

	return (Expr *) result;
}

/*
 * expression_planner_with_deps
 *		对独立表达式做规划器的变换，
 *		并连同结果返回表达式依赖信息。
 *
 * 这与 expression_planner() 相同，只不过它还返回
 * 关于该表达式可能依赖的信息，即其定义会影响结果的
 * 对象的身份。与 PlannedStmt 里一样，这些
 * 表示为关系 Oid 的列表和 PlanInvalItems 的列表。
 */
Expr *
expression_planner_with_deps(Expr *expr,
							 List **relationOids,
							 List **invalItems)
{
	Node	   *result;
	PlannerGlobal glob;
	PlannerInfo root;

	/* 构造一份占位的规划器状态，以便使用 setrefs 机制 */
	MemSet(&glob, 0, sizeof(glob));
	glob.type = T_PlannerGlobal;
	glob.relationOids = NIL;
	glob.invalItems = NIL;

	MemSet(&root, 0, sizeof(root));
	root.type = T_PlannerInfo;
	root.glob = &glob;

	/*
	 * 转换命名参数的函数调用，插入默认参数，并
	 * 简化常量子表达式。也收集被内联的函数
	 * 和被省略的域的身份。
	 */
	result = eval_const_expressions(&root, (Node *) expr);

	/* 若缺少，就填上 opfuncid 值 */
	fix_opfuncids(result);

	/*
	 * 现在遍历完成的表达式，找出其他应当
	 * 记为表达式依赖的东西。
	 */
	(void) extract_query_dependencies_walker(result, &root);

	*relationOids = glob.relationOids;
	*invalItems = glob.invalItems;

	return (Expr *) result;
}


/*
 * plan_cluster_use_sort
 *		使用规划器来决定 CLUSTER 应当如何实现排序
 *
 * tableOid 是要按其索引 indexOid 做 CLUSTER 的表的 OID
 * （已知那是一个 B-树索引）。决定执行 CLUSTER 时，
 * 做索引扫描还是顺序扫描加排序更便宜。
 * 返回真表示使用排序，假表示使用索引扫描。
 *
 * 注意：调用方最好已经对该表持有某种锁。
 */
bool
plan_cluster_use_sort(Oid tableOid, Oid indexOid)
{
	PlannerInfo *root;
	Query	   *query;
	PlannerGlobal *glob;
	RangeTblEntry *rte;
	RelOptInfo *rel;
	IndexOptInfo *indexInfo;
	QualCost	indexExprCost;
	Cost		comparisonCost;
	Path	   *seqScanPath;
	Path		seqScanAndSortPath;
	IndexPath  *indexScanPath;
	ListCell   *lc;

	/* 若索引扫描被禁用，就可以短路代价比较 */
	if (!enable_indexscan)
		return true;			/* 使用排序 */

	/* 建立大体上占位的规划器状态 */
	query = makeNode(Query);
	query->commandType = CMD_SELECT;

	glob = makeNode(PlannerGlobal);

	root = makeNode(PlannerInfo);
	root->parse = query;
	root->glob = glob;
	root->query_level = 1;
	root->planner_cxt = CurrentMemoryContext;
	root->wt_param_id = -1;
	root->join_domains = list_make1(makeNode(JoinDomain));

	/* 为该关系建立一个最小的 RTE */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = tableOid;
	rte->relkind = RELKIND_RELATION;	/* 不要太挑剔。 */
	rte->rellockmode = AccessShareLock;
	rte->lateral = false;
	rte->inh = false;
	rte->inFromCl = true;
	query->rtable = list_make1(rte);
	addRTEPermissionInfo(&query->rteperminfos, rte);

	/* 建立 RTE/RelOptInfo 数组 */
	setup_simple_rel_arrays(root);

	/* 建立 RelOptInfo */
	rel = build_simple_rel(root, 1, NULL);

	/* 找出目标索引的 IndexOptInfo */
	indexInfo = NULL;
	foreach(lc, rel->indexlist)
	{
		indexInfo = lfirst_node(IndexOptInfo, lc);
		if (indexInfo->indexoid == indexOid)
			break;
	}

	/*
	 * get_relation_info 也许没有为
	 * 想要的索引生成 IndexOptInfo；若它还没到达
	 * indcheckxmin 可用性视界，或它是系统索引而我们
	 * 正在忽略系统索引，就会这样。这种情况下应当告诉 CLUSTER 不要
	 * 信任索引内容，而使用顺序扫描加排序。
	 */
	if (lc == NULL)				/* 不在列表里？ */
		return true;			/* 使用排序 */

	/*
	 * 与其做使用 set_baserel_size_estimates 所需的全部繁琐步骤，
	 * 不如对行数和宽度做个快速的权宜处理。
	 */
	rel->rows = rel->tuples;
	rel->reltarget->width = get_relation_data_width(tableOid, NULL);

	root->total_table_pages = rel->pages;

	/*
	 * 确定索引表达式的求值代价，若有。对排序期间发生的每次元组比较，
	 * 需要把那个数量计两遍，
	 * 因为 tuplesort.c 每次都得重新求值索引
	 * 表达式。（XXX 那相当低效……）
	 */
	cost_qual_eval(&indexExprCost, indexInfo->indexprs, root);
	comparisonCost = 2.0 * (indexExprCost.startup + indexExprCost.per_tuple);

	/* 估计顺序扫描加排序的代价 */
	seqScanPath = create_seqscan_path(root, rel, NULL, 0);
	cost_sort(&seqScanAndSortPath, root, NIL,
			  seqScanPath->total_cost, rel->tuples, rel->reltarget->width,
			  comparisonCost, maintenance_work_mem, -1.0);

	/* 估计索引扫描的代价 */
	indexScanPath = create_index_path(root, indexInfo,
									  NIL, NIL, NIL, NIL,
									  ForwardScanDirection, false,
									  NULL, 1.0, false);

	return (seqScanAndSortPath.total_cost < indexScanPath->path.total_cost);
}

/*
 * plan_create_index_workers
 *		使用规划器来决定 CREATE INDEX 应当请求多少
 *		并行工作进程来使用
 *
 * tableOid 是要在其上建索引的表。indexOid 是
 * 要创建或重建的索引的 OID（必须是 B-树索引）。
 *
 * 返回值是要请求的并行工作进程数。若这是 0，
 * 继续下去也许不安全。注意这不包括
 * 作为工作进程参与的首领（该值始终是并行
 * 工作进程的数量）。
 *
 * 注意：调用方最好已经对该表和
 * 索引持有某种锁。
 */
int
plan_create_index_workers(Oid tableOid, Oid indexOid)
{
	PlannerInfo *root;
	Query	   *query;
	PlannerGlobal *glob;
	RangeTblEntry *rte;
	Relation	heap;
	Relation	index;
	RelOptInfo *rel;
	int			parallel_workers;
	BlockNumber heap_blocks;
	double		reltuples;
	double		allvisfrac;

	/*
	 * 在独立后端里，或
	 * 并行被禁用时，不允许执行并行操作。
	 */
	if (!IsUnderPostmaster || max_parallel_maintenance_workers == 0)
		return 0;

	/* 建立大体上占位的规划器状态 */
	query = makeNode(Query);
	query->commandType = CMD_SELECT;

	glob = makeNode(PlannerGlobal);

	root = makeNode(PlannerInfo);
	root->parse = query;
	root->glob = glob;
	root->query_level = 1;
	root->planner_cxt = CurrentMemoryContext;
	root->wt_param_id = -1;
	root->join_domains = list_make1(makeNode(JoinDomain));

	/*
	 * 建立一个最小的 RTE。
	 *
	 * 把该 RTE 标成 inh = true。这是一个权宜之计，用来阻止
	 * get_relation_info() 去取索引信息，这是必要的，
	 * 因为它并不预期当前有任何 IndexOptInfo 正在
	 * 进行 REINDEX。
	 */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = tableOid;
	rte->relkind = RELKIND_RELATION;	/* 不要太挑剔。 */
	rte->rellockmode = AccessShareLock;
	rte->lateral = false;
	rte->inh = true;
	rte->inFromCl = true;
	query->rtable = list_make1(rte);
	addRTEPermissionInfo(&query->rteperminfos, rte);

	/* 建立 RTE/RelOptInfo 数组 */
	setup_simple_rel_arrays(root);

	/* 建立 RelOptInfo */
	rel = build_simple_rel(root, 1, NULL);

	/* 假定关系已被调用方锁定 */
	heap = table_open(tableOid, NoLock);
	index = index_open(indexOid, NoLock);

	/*
	 * 判断继续下去是否安全。
	 *
	 * 目前，并行工作进程无法访问首领的临时表。
	 * 此外，任何索引谓词或索引表达式都必须并行
	 * 安全。
	 */
	if (heap->rd_rel->relpersistence == RELPERSISTENCE_TEMP ||
		!is_parallel_safe(root, (Node *) RelationGetIndexExpressions(index)) ||
		!is_parallel_safe(root, (Node *) RelationGetIndexPredicate(index)))
	{
		parallel_workers = 0;
		goto done;
	}

	/*
	 * 若为该表设置了 parallel_workers 存储参数，就接受它
	 * 作为要启动的并行工作进程数（但仍以
	 * max_parallel_maintenance_workers 为上限）。注意当设置了 parallel_workers 时，我们有意不
	 * 考虑任何其他因素。（例如工作进程的内存
	 * 使用。）
	 */
	if (rel->rel_parallel_workers != -1)
	{
		parallel_workers = Min(rel->rel_parallel_workers,
							   max_parallel_maintenance_workers);
		goto done;
	}

	/*
	 * 自己估计堆关系大小，因为无法信任 rel->pages
	 * （堆 RTE 被标成了继承父表）
	 */
	estimate_rel_size(heap, NULL, &heap_blocks, &reltuples, &allvisfrac);

	/*
	 * 用通用
	 * 模型确定扫描该堆关系的工作进程数
	 */
	parallel_workers = compute_parallel_worker(rel, heap_blocks, -1,
											   max_parallel_maintenance_workers);

	/*
	 * 按需要根据可用的 maintenance_work_mem 给工作进程封顶。
	 *
	 * 注意每个 tuplesort 参与者分到总
	 * maintenance_work_mem 预算的均等份额。目标是让参与者
	 * （包括作为参与者的首领）不少于 32MB
	 * 内存。这使得 maintenance_work_mem 设为 64MB 的情形
	 * 刚好越过能够启动单个
	 * 并行工作进程来排序的门槛。
	 */
	while (parallel_workers > 0 &&
		   maintenance_work_mem / (parallel_workers + 1) < 32768L)
		parallel_workers--;

done:
	index_close(index, NoLock);
	table_close(heap, NoLock);

	return parallel_workers;
}

/*
 * add_paths_to_grouping_rel
 *
 * 向上层分组关系添加非部分路径。
 */
static void
add_paths_to_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
						  RelOptInfo *grouped_rel,
						  RelOptInfo *partially_grouped_rel,
						  const AggClauseCosts *agg_costs,
						  grouping_sets_data *gd, double dNumGroups,
						  GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	Path	   *cheapest_path = input_rel->cheapest_total_path;
	ListCell   *lc;
	bool		can_hash = (extra->flags & GROUPING_CAN_USE_HASH) != 0;
	bool		can_sort = (extra->flags & GROUPING_CAN_USE_SORT) != 0;
	List	   *havingQual = (List *) extra->havingQual;
	AggClauseCosts *agg_final_costs = &extra->agg_final_costs;

	if (can_sort)
	{
		/*
		 * 用任何可用的、排序得合适的路径作为输入，也考虑
		 * 对总代价最低的路径排序，并对任何带
		 * 预排序键的路径做增量排序。
		 */
		foreach(lc, input_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);
			bool		is_sorted;
			int			presorted_keys;

			is_sorted = pathkeys_count_contained_in(root->group_pathkeys,
													path->pathkeys,
													&presorted_keys);

			if (!is_sorted)
			{
				/*
				 * 至少试着对总代价最低的路径排序，也试着
				 * 对任何已经部分排好序的路径做
				 * 增量排序（增量排序被禁用时，不必处理带预排序
				 * 键的路径，除非它是
				 * 总代价最低的输入路径）。
				 */
				if (path != cheapest_path &&
					(presorted_keys == 0 || !enable_incremental_sort))
					continue;

				/*
				 * 不必同时考虑排序和增量排序。
				 * 没有预排序键时就做排序，有
				 * 预排序键时做增量排序。
				 */
				if (presorted_keys == 0 || !enable_incremental_sort)
					path = (Path *) create_sort_path(root,
													 grouped_rel,
													 path,
													 root->group_pathkeys,
													 -1.0);
				else
					path = (Path *) create_incremental_sort_path(root,
																 grouped_rel,
																 path,
																 root->group_pathkeys,
																 presorted_keys,
																 -1.0);
			}

			/* 现在决定在它上面贴什么 */
			if (parse->groupingSets)
			{
				consider_groupingsets_paths(root, grouped_rel,
											path, true, can_hash,
											gd, agg_costs, dNumGroups);
			}
			else if (parse->hasAggs)
			{
				/*
				 * 我们有聚合，也许带普通 GROUP BY。做一个
				 * AggPath。
				 */
				add_path(grouped_rel, (Path *)
						 create_agg_path(root,
										 grouped_rel,
										 path,
										 grouped_rel->reltarget,
										 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
										 AGGSPLIT_SIMPLE,
										 root->processed_groupClause,
										 havingQual,
										 agg_costs,
										 dNumGroups));
			}
			else if (parse->groupClause)
			{
				/*
				 * 我们有 GROUP BY，但没有聚合或分组集。做一个
				 * GroupPath。
				 */
				add_path(grouped_rel, (Path *)
						 create_group_path(root,
										   grouped_rel,
										   path,
										   root->processed_groupClause,
										   havingQual,
										   dNumGroups));
			}
			else
			{
				/* 其他情形应当已经在上面处理过 */
				Assert(false);
			}
		}

		/*
		 * 可以不直接在输入关系上操作，而
		 * 考虑完成一条部分聚合的路径。
		 */
		if (partially_grouped_rel != NULL)
		{
			foreach(lc, partially_grouped_rel->pathlist)
			{
				Path	   *path = (Path *) lfirst(lc);
				bool		is_sorted;
				int			presorted_keys;

				is_sorted = pathkeys_count_contained_in(root->group_pathkeys,
														path->pathkeys,
														&presorted_keys);

				if (!is_sorted)
				{
					/*
					 * 至少试着对总代价最低的路径排序，也试着
					 * 对任何已经部分
					 * 排好序的路径做增量排序（增量排序被禁用时，不必处理带
					 * 预排序键的路径，除非
					 * 它是总代价最低的输入路径）。
					 */
					if (path != partially_grouped_rel->cheapest_total_path &&
						(presorted_keys == 0 || !enable_incremental_sort))
						continue;

					/*
					 * 不必同时考虑排序和增量
					 * 排序。没有预排序
					 * 键时就做排序，有预排序
					 * 键时做增量排序。
					 */
					if (presorted_keys == 0 || !enable_incremental_sort)
						path = (Path *) create_sort_path(root,
														 grouped_rel,
														 path,
														 root->group_pathkeys,
														 -1.0);
					else
						path = (Path *) create_incremental_sort_path(root,
																	 grouped_rel,
																	 path,
																	 root->group_pathkeys,
																	 presorted_keys,
																	 -1.0);
				}

				if (parse->hasAggs)
					add_path(grouped_rel, (Path *)
							 create_agg_path(root,
											 grouped_rel,
											 path,
											 grouped_rel->reltarget,
											 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
											 AGGSPLIT_FINAL_DESERIAL,
											 root->processed_groupClause,
											 havingQual,
											 agg_final_costs,
											 dNumGroups));
				else
					add_path(grouped_rel, (Path *)
							 create_group_path(root,
											   grouped_rel,
											   path,
											   root->processed_groupClause,
											   havingQual,
											   dNumGroups));

			}
		}
	}

	if (can_hash)
	{
		if (parse->groupingSets)
		{
			/*
			 * 试着在未排序输入上做一条只用哈希的分组集路径。
			 */
			consider_groupingsets_paths(root, grouped_rel,
										cheapest_path, false, true,
										gd, agg_costs, dNumGroups);
		}
		else
		{
			/*
			 * 生成一条 HashAgg 路径。只需要在总代价最低的输入路径上放一个 Agg，
			 * 因为输入顺序无关紧要。
			 */
			add_path(grouped_rel, (Path *)
					 create_agg_path(root, grouped_rel,
									 cheapest_path,
									 grouped_rel->reltarget,
									 AGG_HASHED,
									 AGGSPLIT_SIMPLE,
									 root->processed_groupClause,
									 havingQual,
									 agg_costs,
									 dNumGroups));
		}

		/*
		 * 在总代价最低的部分
		 * 分组路径之上生成一条 Finalize HashAgg 路径，假定有一条
		 */
		if (partially_grouped_rel && partially_grouped_rel->pathlist)
		{
			Path	   *path = partially_grouped_rel->cheapest_total_path;

			add_path(grouped_rel, (Path *)
					 create_agg_path(root,
									 grouped_rel,
									 path,
									 grouped_rel->reltarget,
									 AGG_HASHED,
									 AGGSPLIT_FINAL_DESERIAL,
									 root->processed_groupClause,
									 havingQual,
									 agg_final_costs,
									 dNumGroups));
		}
	}

	/*
	 * 使用按分区聚合时，部分路径列表里也许有完全聚合的
	 * 路径，因为 add_paths_to_append_rel() 会
	 * 考虑一条由各子项非部分路径的 Parallel Append 组成的
	 * grouped_rel 路径。
	 */
	if (grouped_rel->partial_pathlist != NIL)
		gather_grouping_paths(root, grouped_rel);
}

/*
 * create_partial_grouping_paths
 *
 * 建立一个新的上层关系，表示部分聚合的结果，
 * 并用适当的路径填充它。注意这里并不定稿
 * 路径列表，因此调用方可以再加部分或非部分
 * 路径，之后必须对该返回的上层关系调用 gather_grouping_paths 和 set_cheapest。
 * 这个新上层关系的全部路径——部分和非部分——
 *
 * 都已部分聚合，但需要后续的 FinalizeAggregate
 * 步骤。
 * 注意：若本函数判定并没有真正必要
 *
 * 建立一个新的 RelOptInfo，
 * 就允许它返回 NULL。
 */
static RelOptInfo *
create_partial_grouping_paths(PlannerInfo *root,
							  RelOptInfo *grouped_rel,
							  RelOptInfo *input_rel,
							  grouping_sets_data *gd,
							  GroupPathExtraData *extra,
							  bool force_rel_creation)
{
	Query	   *parse = root->parse;
	RelOptInfo *partially_grouped_rel;
	AggClauseCosts *agg_partial_costs = &extra->agg_partial_costs;
	AggClauseCosts *agg_final_costs = &extra->agg_final_costs;
	Path	   *cheapest_partial_path = NULL;
	Path	   *cheapest_total_path = NULL;
	double		dNumPartialGroups = 0;
	double		dNumPartialPartialGroups = 0;
	ListCell   *lc;
	bool		can_hash = (extra->flags & GROUPING_CAN_USE_HASH) != 0;
	bool		can_sort = (extra->flags & GROUPING_CAN_USE_SORT) != 0;

	/*
	 * 考虑是否应当生成部分聚合的非部分
	 * 路径。只有当我们有一条非部分路径，且只有当
	 * 输入关系的父项正在做部分按分区
	 * 聚合时，才能这么做。（注意 extra->patype 是父层正在使用的按分区
	 * 聚合类型，不是这一层的。）
	 */
	if (input_rel->pathlist != NIL &&
		extra->patype == PARTITIONWISE_AGGREGATE_PARTIAL)
		cheapest_total_path = input_rel->cheapest_total_path;

	/*
	 * 若 grouped_rel 可能并行，就应当考虑
	 * 生成部分分组的部分路径。不过，若输入关系
	 * 没有部分路径，就不能。
	 */
	if (grouped_rel->consider_parallel && input_rel->partial_pathlist != NIL)
		cheapest_partial_path = linitial(input_rel->partial_pathlist);

	/*
	 * 若不能对部分路径做部分聚合，也不能对
	 * 非部分路径做部分聚合，就不必建立新的
	 * RelOptInfo，除非调用方指定了 force_rel_creation。
	 */
	if (cheapest_total_path == NULL &&
		cheapest_partial_path == NULL &&
		!force_rel_creation)
		return NULL;

	/*
	 * 建立一个新的上层关系，表示对输入关系的行做部分
	 * 聚合的结果。
	 */
	partially_grouped_rel = fetch_upper_rel(root,
											UPPERREL_PARTIAL_GROUP_AGG,
											grouped_rel->relids);
	partially_grouped_rel->consider_parallel =
		grouped_rel->consider_parallel;
	partially_grouped_rel->reloptkind = grouped_rel->reloptkind;
	partially_grouped_rel->serverid = grouped_rel->serverid;
	partially_grouped_rel->userid = grouped_rel->userid;
	partially_grouped_rel->useridiscurrent = grouped_rel->useridiscurrent;
	partially_grouped_rel->fdwroutine = grouped_rel->fdwroutine;

	/*
	 * 为部分聚合路径建立目标列表。这些路径无法只
	 * 发出与常规聚合路径相同的 tlist，因为 (1) 必须
	 * 包括 HAVING 里需要的 Var 和 Aggref，它们也许不出现在
	 * 结果 tlist 里，且 (2) Aggref 必须设成部分模式。
	 */
	partially_grouped_rel->reltarget =
		make_partial_grouping_target(root, grouped_rel->reltarget,
									 extra->havingQual);

	if (!extra->partial_costs_set)
	{
		/*
		 * 收集关于聚合的统计，以估计
		 * 并行执行聚合的代价。
		 */
		MemSet(agg_partial_costs, 0, sizeof(AggClauseCosts));
		MemSet(agg_final_costs, 0, sizeof(AggClauseCosts));
		if (parse->hasAggs)
		{
			/* 部分阶段 */
			get_agg_clause_costs(root, AGGSPLIT_INITIAL_SERIAL,
								 agg_partial_costs);

			/* 最终阶段 */
			get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL,
								 agg_final_costs);
		}

		extra->partial_costs_set = true;
	}

	/* 估计部分组的数量。 */
	if (cheapest_total_path != NULL)
		dNumPartialGroups =
			get_number_of_groups(root,
								 cheapest_total_path->rows,
								 gd,
								 extra->targetList);
	if (cheapest_partial_path != NULL)
		dNumPartialPartialGroups =
			get_number_of_groups(root,
								 cheapest_partial_path->rows,
								 gd,
								 extra->targetList);

	if (can_sort && cheapest_total_path != NULL)
	{
		/* 这应当先前已经检查过 */
		Assert(parse->hasAggs || parse->groupClause);

		/*
		 * 用任何可用的、排序得合适的路径作为输入，也考虑
		 * 对总代价最低的部分路径排序。
		 */
		foreach(lc, input_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);
			bool		is_sorted;
			int			presorted_keys;

			is_sorted = pathkeys_count_contained_in(root->group_pathkeys,
													path->pathkeys,
													&presorted_keys);
			if (!is_sorted)
			{
				/*
				 * 至少试着对总代价最低的路径排序，也试着
				 * 对任何已经部分排好序的路径做
				 * 增量排序（增量排序被禁用时，不必处理带预排序
				 * 键的路径，除非它是
				 * 总代价最低的输入路径）。
				 */
				if (path != cheapest_total_path &&
					(presorted_keys == 0 || !enable_incremental_sort))
					continue;

				/*
				 * 不必同时考虑排序和增量排序。
				 * 没有预排序键时就做排序，有
				 * 预排序键时做增量排序。
				 */
				if (presorted_keys == 0 || !enable_incremental_sort)
					path = (Path *) create_sort_path(root,
													 partially_grouped_rel,
													 path,
													 root->group_pathkeys,
													 -1.0);
				else
					path = (Path *) create_incremental_sort_path(root,
																 partially_grouped_rel,
																 path,
																 root->group_pathkeys,
																 presorted_keys,
																 -1.0);
			}

			if (parse->hasAggs)
				add_path(partially_grouped_rel, (Path *)
						 create_agg_path(root,
										 partially_grouped_rel,
										 path,
										 partially_grouped_rel->reltarget,
										 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
										 AGGSPLIT_INITIAL_SERIAL,
										 root->processed_groupClause,
										 NIL,
										 agg_partial_costs,
										 dNumPartialGroups));
			else
				add_path(partially_grouped_rel, (Path *)
						 create_group_path(root,
										   partially_grouped_rel,
										   path,
										   root->processed_groupClause,
										   NIL,
										   dNumPartialGroups));
		}
	}

	if (can_sort && cheapest_partial_path != NULL)
	{
		/* 与上面的逻辑类似，但是针对部分路径。 */
		foreach(lc, input_rel->partial_pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);
			bool		is_sorted;
			int			presorted_keys;

			is_sorted = pathkeys_count_contained_in(root->group_pathkeys,
													path->pathkeys,
													&presorted_keys);

			if (!is_sorted)
			{
				/*
				 * 至少试着对总代价最低的路径排序，也试着
				 * 对任何已经部分排好序的路径做
				 * 增量排序（增量排序被禁用时，不必处理带预排序
				 * 键的路径，除非它是
				 * 总代价最低的输入路径）。
				 */
				if (path != cheapest_partial_path &&
					(presorted_keys == 0 || !enable_incremental_sort))
					continue;

				/*
				 * 不必同时考虑排序和增量排序。
				 * 没有预排序键时就做排序，有
				 * 预排序键时做增量排序。
				 */
				if (presorted_keys == 0 || !enable_incremental_sort)
					path = (Path *) create_sort_path(root,
													 partially_grouped_rel,
													 path,
													 root->group_pathkeys,
													 -1.0);
				else
					path = (Path *) create_incremental_sort_path(root,
																 partially_grouped_rel,
																 path,
																 root->group_pathkeys,
																 presorted_keys,
																 -1.0);
			}

			if (parse->hasAggs)
				add_partial_path(partially_grouped_rel, (Path *)
								 create_agg_path(root,
												 partially_grouped_rel,
												 path,
												 partially_grouped_rel->reltarget,
												 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
												 AGGSPLIT_INITIAL_SERIAL,
												 root->processed_groupClause,
												 NIL,
												 agg_partial_costs,
												 dNumPartialPartialGroups));
			else
				add_partial_path(partially_grouped_rel, (Path *)
								 create_group_path(root,
												   partially_grouped_rel,
												   path,
												   root->processed_groupClause,
												   NIL,
												   dNumPartialPartialGroups));
		}
	}

	/*
	 * 在可能的地方加上一条部分分组的 HashAgg 路径
	 */
	if (can_hash && cheapest_total_path != NULL)
	{
		/* 上面已检查过 */
		Assert(parse->hasAggs || parse->groupClause);

		add_path(partially_grouped_rel, (Path *)
				 create_agg_path(root,
								 partially_grouped_rel,
								 cheapest_total_path,
								 partially_grouped_rel->reltarget,
								 AGG_HASHED,
								 AGGSPLIT_INITIAL_SERIAL,
								 root->processed_groupClause,
								 NIL,
								 agg_partial_costs,
								 dNumPartialGroups));
	}

	/*
	 * 现在在可能的地方加上一条部分分组的 HashAgg 部分路径
	 */
	if (can_hash && cheapest_partial_path != NULL)
	{
		add_partial_path(partially_grouped_rel, (Path *)
						 create_agg_path(root,
										 partially_grouped_rel,
										 cheapest_partial_path,
										 partially_grouped_rel->reltarget,
										 AGG_HASHED,
										 AGGSPLIT_INITIAL_SERIAL,
										 root->processed_groupClause,
										 NIL,
										 agg_partial_costs,
										 dNumPartialPartialGroups));
	}

	/*
	 * 若有一个 FDW 负责该查询的全部基关系，
	 * 就让它考虑添加部分分组的 ForeignPaths。
	 */
	if (partially_grouped_rel->fdwroutine &&
		partially_grouped_rel->fdwroutine->GetForeignUpperPaths)
	{
		FdwRoutine *fdwroutine = partially_grouped_rel->fdwroutine;

		fdwroutine->GetForeignUpperPaths(root,
										 UPPERREL_PARTIAL_GROUP_AGG,
										 input_rel, partially_grouped_rel,
										 extra);
	}

	return partially_grouped_rel;
}

/*
 * 为分组关系或部分
 * 分组关系生成 Gather 和 Gather Merge 路径。
 *
 * generate_useful_gather_paths 做大部分工作，但我们也考虑一种
 * 特殊情形：可以试着按 group_pathkeys 对数据排序，然后
 * 应用 Gather Merge。
 *
 * 注意：本函数不应当用于分组或
 * 部分分组关系之外的任何东西，不仅因为
 * 它显式引用 group_pathkeys，而且我们把 "true" 作为第三个参数传给
 * generate_useful_gather_paths()。
 */
static void
gather_grouping_paths(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *lc;
	Path	   *cheapest_partial_path;
	List	   *groupby_pathkeys;

	/*
	 * 这发生在任何部分聚合之后，因此裁掉
	 * 为 ORDER BY / DISTINCT 聚合加上的任何路径键。
	 */
	if (list_length(root->group_pathkeys) > root->num_groupby_pathkeys)
		groupby_pathkeys = list_copy_head(root->group_pathkeys,
										  root->num_groupby_pathkeys);
	else
		groupby_pathkeys = root->group_pathkeys;

	/* 对无序路径试 Gather，对有序路径试 Gather Merge。 */
	generate_useful_gather_paths(root, rel, true);

	/* 试总代价最低的部分路径 + 显式 Sort + Gather Merge。 */
	cheapest_partial_path = linitial(rel->partial_pathlist);
	if (!pathkeys_contained_in(groupby_pathkeys,
							   cheapest_partial_path->pathkeys))
	{
		Path	   *path;
		double		total_groups;

		total_groups =
			cheapest_partial_path->rows * cheapest_partial_path->parallel_workers;
		path = (Path *) create_sort_path(root, rel, cheapest_partial_path,
										 groupby_pathkeys,
										 -1.0);
		path = (Path *)
			create_gather_merge_path(root,
									 rel,
									 path,
									 rel->reltarget,
									 groupby_pathkeys,
									 NULL,
									 &total_groups);

		add_path(rel, path);
	}

	/*
	 * 若已启用，就考虑对全部部分路径做增量排序。
	 *
	 * 当 groupby_pathkeys 只有一项时，也可以跳过整个循环，
	 * 因为那时不可能有该列表的预排序前缀，
	 * 除非列表已完全排好序。
	 */
	if (!enable_incremental_sort || list_length(groupby_pathkeys) == 1)
		return;

	/* 若已启用，也考虑对部分路径做增量排序 */
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);
		bool		is_sorted;
		int			presorted_keys;
		double		total_groups;

		is_sorted = pathkeys_count_contained_in(groupby_pathkeys,
												path->pathkeys,
												&presorted_keys);

		if (is_sorted)
			continue;

		if (presorted_keys == 0)
			continue;

		path = (Path *) create_incremental_sort_path(root,
													 rel,
													 path,
													 groupby_pathkeys,
													 presorted_keys,
													 -1.0);

		path = (Path *)
			create_gather_merge_path(root,
									 rel,
									 path,
									 rel->reltarget,
									 groupby_pathkeys,
									 NULL,
									 &total_groups);

		add_path(rel, path);
	}
}

/*
 * can_partial_agg
 *
 * 判断部分分组和/或聚合是否可能。
 * 可能时返回真，否则返回假。
 */
static bool
can_partial_agg(PlannerInfo *root)
{
	Query	   *parse = root->parse;

	if (!parse->hasAggs && parse->groupClause == NIL)
	{
		/*
		 * 除非有
		 * 一些聚合或一条分组子句，否则不知道如何做并行聚合。
		 */
		return false;
	}
	else if (parse->groupingSets)
	{
		/* 不知道如何并行做分组集。 */
		return false;
	}
	else if (root->hasNonPartialAggs || root->hasNonSerialAggs)
	{
		/* 对部分模式的支持不足。 */
		return false;
	}

	/* 一切看起来都好。 */
	return true;
}

/*
 * apply_scanjoin_target_to_paths
 *
 * 调整最终的扫描/连接关系，并递归调整其全部子项，
 * 以生成最终的扫描/连接目标。把这建成顶层和
 * 每个子关系都有新 RelOptInfo 的单独规划步骤会更正确，
 * 但这样做明显更便宜。
 * 也许那个问题以后能解决，但目前就这么做。
 *
 * 若 tlist_same_exprs 为真，则要应用的扫描/连接目标与
 * 现有 reltarget 有同样的表达式，因此只需插入
 * 适当的 sortgroupref 信息。避免建立
 * 投影路径，我们立刻和计划建立时都能省力气。
 */
static void
apply_scanjoin_target_to_paths(PlannerInfo *root,
							   RelOptInfo *rel,
							   List *scanjoin_targets,
							   List *scanjoin_targets_contain_srfs,
							   bool scanjoin_target_parallel_safe,
							   bool tlist_same_exprs)
{
	bool		rel_is_partitioned = IS_PARTITIONED_REL(rel);
	PathTarget *scanjoin_target;
	ListCell   *lc;

	/* 这会递归，所以要多疑。 */
	check_stack_depth();

	/*
	 * 若该关系已分区，就想丢掉它现有的路径并
	 * 生成新的。即使保留
	 * 现有路径，本函数仍然正确：我们会修改它们，使分区 Append 之上
	 * 生成正确的目标，然后它们会与在
	 * Append 之下生成目标的路径比代价。不过，在我们目前的代价
	 * 模型里，后一种办法总是同样或更低的代价，因此修改
	 * 现有路径只会是无用功。而且，当代价
	 * 相同时，变化的舍入误差有时也许会让一条现有
	 * 路径被选中，造成不受欢迎的跨平台计划
	 * 差异。因此我们丢掉旧路径，从而迫使工作在
	 * Append 之下完成，除非目标不是并行安全的。
	 *
	 * 需要小心，因为必须允许
	 * generate_useful_gather_paths 在下一段
	 * 看见旧的部分路径。因此这里清掉主路径列表，然后允许
	 * generate_useful_gather_paths 往主列表加路径，
	 * 最后清掉部分路径列表。
	 */
	if (rel_is_partitioned)
		rel->pathlist = NIL;

	/*
	 * 若扫描/连接目标不是并行安全的，部分路径无法
	 * 生成它。
	 */
	if (!scanjoin_target_parallel_safe)
	{
		/*
		 * 既然无法在并行
		 * 工作进程里生成最终的扫描/连接目标，这就是使用任何现存部分路径的
		 * 最后机会；因此建立使用它们、并发出
		 * 当前 reltarget 的 Gather 路径。目标
		 * 并行安全时不这么做，因为在应用最终扫描/连接目标
		 * 之后再做，我们能生成更好的
		 * 路径。
		 */
		generate_useful_gather_paths(root, rel, false);

		/* 在这一层之上不能使用并行查询。 */
		rel->partial_pathlist = NIL;
		rel->consider_parallel = false;
	}

	/* 按上面的注释，完成丢掉已分区关系的旧路径 */
	if (rel_is_partitioned)
		rel->partial_pathlist = NIL;

	/* 抽出不含 SRF 的扫描/连接目标。 */
	scanjoin_target = linitial_node(PathTarget, scanjoin_targets);

	/*
	 * 把不含 SRF 的扫描/连接目标应用到每条现有路径。
	 *
	 * 若 tlist 表达式相同，就可以把 sortgroupref
	 * 信息注入现有的 pathtarget。否则，把每条
	 * 路径换成生成不含 SRF 的扫描/连接
	 * 目标的投影路径。这不会改变 rel->pathlist 里路径的顺序，
	 * 因此就地修改该列表。
	 */
	foreach(lc, rel->pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);

		/* 不应当再有任何参数化路径 */
		Assert(subpath->param_info == NULL);

		if (tlist_same_exprs)
			subpath->pathtarget->sortgrouprefs =
				scanjoin_target->sortgrouprefs;
		else
		{
			Path	   *newpath;

			newpath = (Path *) create_projection_path(root, rel, subpath,
													  scanjoin_target);
			lfirst(lc) = newpath;
		}
	}

	/* 同样调整任何部分路径的目标。 */
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);

		/* 不应当再有任何参数化路径 */
		Assert(subpath->param_info == NULL);

		if (tlist_same_exprs)
			subpath->pathtarget->sortgrouprefs =
				scanjoin_target->sortgrouprefs;
		else
		{
			Path	   *newpath;

			newpath = (Path *) create_projection_path(root, rel, subpath,
													  scanjoin_target);
			lfirst(lc) = newpath;
		}
	}

	/*
	 * 现在，若最终扫描/连接目标含有 SRFs，就在每条现有路径之上插入 ProjectSetPath。
	 * （注意本函数不看
	 * 总代价最低路径字段，这是好事，因为它们现在
	 * 是假的。）
	 */
	if (root->parse->hasTargetSRFs)
		adjust_paths_for_srfs(root, rel,
							  scanjoin_targets,
							  scanjoin_targets_contain_srfs);

	/*
	 * 把该关系的目标更新为最终的（含 SRFs）扫描/连接目标。
	 * 这现在与全部路径的实际输出匹配，若它们不一致，
	 * 在 createplan.c 里也许会搞混。必须现在做，以便
	 * 下一部分建立的任何 append 路径使用正确的
	 * pathtarget（参见 create_append_path）。
	 *
	 * 若对最终扫描/连接关系或其任何子项调用 GetForeignUpperPaths()，
	 * 这也是必要的，因为
	 * FDW 也许会看该关系的目标来建立 ForeignPaths。
	 */
	rel->reltarget = llast_node(PathTarget, scanjoin_targets);

	/*
	 * 若该关系已分区，就递归地把扫描/连接目标
	 * 应用到全部分区，并生成全新的 Append 路径，使
	 * 扫描/连接目标在 Append 之下而不是之上计算。
	 * 由于 Append 不能做投影，那也许省掉一个单独的
	 * Result 节点，而且对按分区聚合也很重要。
	 */
	if (rel_is_partitioned)
	{
		List	   *live_children = NIL;
		int			i;

		/* 调整每个分区。 */
		i = -1;
		while ((i = bms_next_member(rel->live_parts, i)) >= 0)
		{
			RelOptInfo *child_rel = rel->part_rels[i];
			AppendRelInfo **appinfos;
			int			nappinfos;
			List	   *child_scanjoin_targets = NIL;

			Assert(child_rel != NULL);

			/* 哑的子项可以忽略。 */
			if (IS_DUMMY_REL(child_rel))
				continue;

			/* 为这个子项翻译扫描/连接目标。 */
			appinfos = find_appinfos_by_relids(root, child_rel->relids,
											   &nappinfos);
			foreach(lc, scanjoin_targets)
			{
				PathTarget *target = lfirst_node(PathTarget, lc);

				target = copy_pathtarget(target);
				target->exprs = (List *)
					adjust_appendrel_attrs(root,
										   (Node *) target->exprs,
										   nappinfos, appinfos);
				child_scanjoin_targets = lappend(child_scanjoin_targets,
												 target);
			}
			pfree(appinfos);

			/* 递归才做真正的工作。 */
			apply_scanjoin_target_to_paths(root, child_rel,
										   child_scanjoin_targets,
										   scanjoin_targets_contain_srfs,
										   scanjoin_target_parallel_safe,
										   tlist_same_exprs);

			/* 把非哑子项留给 Append 路径。 */
			if (!IS_DUMMY_REL(child_rel))
				live_children = lappend(live_children, child_rel);
		}

		/* 通过追加子路径为这个关系建立新路径。 */
		add_paths_to_append_rel(root, rel, live_children);
	}

	/*
	 * 考虑生成 Gather 或 Gather Merge 路径。只有当
	 * 该关系并行安全时才这么做，并且不为子关系做，以免
	 * 在同一计划里建立多个 Gather 节点。必须在
	 * 全部路径都已生成之后、set_cheapest 之前做，因为
	 * 生成的路径之一也许会是总代价最低的。
	 */
	if (rel->consider_parallel && !IS_OTHER_REL(rel))
		generate_useful_gather_paths(root, rel, false);

	/*
	 * 重新评估哪些路径总代价最低，现在我们也许已经给
	 * 这个关系加上了新的 Gather（或 Gather Merge）和/或
	 * Append（或 MergeAppend）路径。
	 */
	set_cheapest(rel);
}

/*
 * create_partitionwise_grouping_paths
 *
 * 若输入关系的分区键是 GROUP BY 子句的一部分，属于
 * 给定组的全部行都来自单个分区。这
 * 允许把分区关系上的聚合/分组分解
 * 成每个分区上的聚合/分组。这应当不比
 * 常规办法更差，而且常常更好。
 *
 * 不过，若 GROUP BY 子句不含全部分区键，
 * 给定组的行也许散布在多个分区上。那种
 * 情况下，我们对每个组做部分聚合，追加结果，
 * 然后完成聚合。这比
 * 前一种情形更不一定会赢。若 PartialAggregate 阶段大大减少
 * 组数，它也许会赢，因为更少的行会经过 Append 节点。
 * 若有很多小组，它也许会输。
 */
static void
create_partitionwise_grouping_paths(PlannerInfo *root,
									RelOptInfo *input_rel,
									RelOptInfo *grouped_rel,
									RelOptInfo *partially_grouped_rel,
									const AggClauseCosts *agg_costs,
									grouping_sets_data *gd,
									PartitionwiseAggregateType patype,
									GroupPathExtraData *extra)
{
	List	   *grouped_live_children = NIL;
	List	   *partially_grouped_live_children = NIL;
	PathTarget *target = grouped_rel->reltarget;
	bool		partial_grouping_valid = true;
	int			i;

	Assert(patype != PARTITIONWISE_AGGREGATE_NONE);
	Assert(patype != PARTITIONWISE_AGGREGATE_PARTIAL ||
		   partially_grouped_rel != NULL);

	/* 为按分区聚合/分组添加路径。 */
	i = -1;
	while ((i = bms_next_member(input_rel->live_parts, i)) >= 0)
	{
		RelOptInfo *child_input_rel = input_rel->part_rels[i];
		PathTarget *child_target;
		AppendRelInfo **appinfos;
		int			nappinfos;
		GroupPathExtraData child_extra;
		RelOptInfo *child_grouped_rel;
		RelOptInfo *child_partially_grouped_rel;

		Assert(child_input_rel != NULL);

		/* 哑的子项可以忽略。 */
		if (IS_DUMMY_REL(child_input_rel))
			continue;

		child_target = copy_pathtarget(target);

		/*
		 * 按原样复制给定的 "extra" 结构，然后覆盖
		 * 这个子项专用的成员。
		 */
		memcpy(&child_extra, extra, sizeof(child_extra));

		appinfos = find_appinfos_by_relids(root, child_input_rel->relids,
										   &nappinfos);

		child_target->exprs = (List *)
			adjust_appendrel_attrs(root,
								   (Node *) target->exprs,
								   nappinfos, appinfos);

		/* 翻译 havingQual 和 targetList。 */
		child_extra.havingQual = (Node *)
			adjust_appendrel_attrs(root,
								   extra->havingQual,
								   nappinfos, appinfos);
		child_extra.targetList = (List *)
			adjust_appendrel_attrs(root,
								   (Node *) extra->targetList,
								   nappinfos, appinfos);

		/*
		 * extra->patype 是为我们的父关系算出的值；patype 是
		 * 这个关系的值。对子项来说，我们的值就是它的
		 * 父关系的值。
		 */
		child_extra.patype = patype;

		/*
		 * 建立分组关系，用来存放该子项的完全聚合分组和/或
		 * 聚合路径。
		 */
		child_grouped_rel = make_grouping_rel(root, child_input_rel,
											  child_target,
											  extra->target_parallel_safe,
											  child_extra.havingQual);

		/* 为这个子关系建立分组路径。 */
		create_ordinary_grouping_paths(root, child_input_rel,
									   child_grouped_rel,
									   agg_costs, gd, &child_extra,
									   &child_partially_grouped_rel);

		if (child_partially_grouped_rel)
		{
			partially_grouped_live_children =
				lappend(partially_grouped_live_children,
						child_partially_grouped_rel);
		}
		else
			partial_grouping_valid = false;

		if (patype == PARTITIONWISE_AGGREGATE_FULL)
		{
			set_cheapest(child_grouped_rel);
			grouped_live_children = lappend(grouped_live_children,
											child_grouped_rel);
		}

		pfree(appinfos);
	}

	/*
	 * 试着为部分分组的子项建立 append 路径。对完整的
	 * 按分区聚合，若并行聚合可能，partial_pathlist 里也许有路径。
	 * 对部分按分区
	 * 聚合，pathlist 和 partial_pathlist 里也许都有路径。
	 *
	 * 注意：必须每个子项都有一条部分分组路径，才能
	 * 为这个关系生成一条部分分组路径。
	 */
	if (partially_grouped_rel && partial_grouping_valid)
	{
		Assert(partially_grouped_live_children != NIL);

		add_paths_to_append_rel(root, partially_grouped_rel,
								partially_grouped_live_children);

		/*
		 * 需要调用 set_cheapest，因为完成步骤会使用该
		 * 关系的总代价最低路径。
		 */
		if (partially_grouped_rel->pathlist)
			set_cheapest(partially_grouped_rel);
	}

	/* 若可能，为完全分组的子项建立 append 路径。 */
	if (patype == PARTITIONWISE_AGGREGATE_FULL)
	{
		Assert(grouped_live_children != NIL);

		add_paths_to_append_rel(root, grouped_rel, grouped_live_children);
	}
}

/*
 * group_by_has_partkey
 *
 * 若给定关系的全部分区键都是
 * GROUP BY 子句的一部分，包括排序规则匹配，则返回真，否则返回假。
 */
static bool
group_by_has_partkey(RelOptInfo *input_rel,
					 List *targetList,
					 List *groupClause)
{
	List	   *groupexprs = get_sortgrouplist_exprs(groupClause, targetList);
	int			cnt = 0;
	int			partnatts;

	/* 输入关系应当已分区。 */
	Assert(input_rel->part_scheme);

	/* 若没有分区键，就提早排除。 */
	if (!input_rel->partexprs)
		return false;

	partnatts = input_rel->part_scheme->partnatts;

	for (cnt = 0; cnt < partnatts; cnt++)
	{
		List	   *partexprs = input_rel->partexprs[cnt];
		ListCell   *lc;
		bool		found = false;

		foreach(lc, partexprs)
		{
			ListCell   *lg;
			Expr	   *partexpr = lfirst(lc);
			Oid			partcoll = input_rel->part_scheme->partcollation[cnt];

			foreach(lg, groupexprs)
			{
				Expr	   *groupexpr = lfirst(lg);
				Oid			groupcoll = exprCollation((Node *) groupexpr);

				/*
				 * 注意：可以假定最多有一个 RelabelType 节点；
				 * 若多于
				 * 一个，eval_const_expressions() 会已经简化过。
				 */
				if (IsA(groupexpr, RelabelType))
					groupexpr = ((RelabelType *) groupexpr)->arg;

				if (equal(groupexpr, partexpr))
				{
					/*
					 * 若分组排序规则与
					 * 分区排序规则不匹配，就拒绝该匹配。
					 */
					if (OidIsValid(partcoll) && OidIsValid(groupcoll) &&
						partcoll != groupcoll)
						return false;

					found = true;
					break;
				}
			}

			if (found)
				break;
		}

		/*
		 * 若没有一个分区键表达式与任何
		 * GROUP BY 表达式匹配，就返回假。
		 */
		if (!found)
			return false;
	}

	return true;
}
