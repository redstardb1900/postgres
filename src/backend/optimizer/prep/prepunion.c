/*-------------------------------------------------------------------------
 *
 * prepunion.c
 *	  Routines to plan set-operation queries.  The filename is a leftover
 *	  from a time when only UNIONs were implemented.
 *
 * There are two code paths in the planner for set-operation queries.
 * If a subquery consists entirely of simple UNION ALL operations, it
 * is converted into an "append relation".  Otherwise, it is handled
 * by the general code in this module (plan_set_operations and its
 * subroutines).  There is some support code here for the append-relation
 * case, but most of the heavy lifting for that is done elsewhere,
 * notably in prepjointree.c and allpaths.c.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepunion.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"


static RelOptInfo *recurse_set_operations(Node *setOp, PlannerInfo *root,
										  List *colTypes, List *colCollations,
										  bool junkOK,
										  int flag, List *refnames_tlist,
										  List **pTargetList,
										  double *pNumGroups);
static RelOptInfo *generate_recursion_path(SetOperationStmt *setOp,
										   PlannerInfo *root,
										   List *refnames_tlist,
										   List **pTargetList);
static RelOptInfo *generate_union_paths(SetOperationStmt *op, PlannerInfo *root,
										List *refnames_tlist,
										List **pTargetList);
static RelOptInfo *generate_nonunion_paths(SetOperationStmt *op, PlannerInfo *root,
										   List *refnames_tlist,
										   List **pTargetList);
static List *plan_union_children(PlannerInfo *root,
								 SetOperationStmt *top_union,
								 List *refnames_tlist,
								 List **tlist_list);
static Path *make_union_unique(SetOperationStmt *op, Path *path, List *tlist,
							   PlannerInfo *root);
static void postprocess_setop_rel(PlannerInfo *root, RelOptInfo *rel);
static bool choose_hashed_setop(PlannerInfo *root, List *groupClauses,
								Path *input_path,
								double dNumGroups, double dNumOutputRows,
								const char *construct);
static List *generate_setop_tlist(List *colTypes, List *colCollations,
								  int flag,
								  Index varno,
								  bool hack_constants,
								  List *input_tlist,
								  List *refnames_tlist,
								  bool *trivial_tlist);
static List *generate_append_tlist(List *colTypes, List *colCollations,
								   bool flag,
								   List *input_tlists,
								   List *refnames_tlist);
static List *generate_setop_grouplist(SetOperationStmt *op, List *targetlist);


/*
 * plan_set_operations
 *
 *	    为集合运算树（UNION/INTERSECT/EXCEPT）规划查询
 *
 * 本例程只处理给定查询的 setOperations 树。
 * root->parse->sortClause 里请求的任何顶层 ORDER BY 将在
 * 我们返回 grouping_planner 时处理；LIMIT 同样如此。
 *
 * 我们返回的是一个至少含有一条实现集合运算树的 Path 的
 * "上层关系" RelOptInfo。另外，root->processed_tlist
 * 收到一份表示最顶层集合运算节点输出的目标列表。
 */
RelOptInfo *
plan_set_operations(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	SetOperationStmt *topop = castNode(SetOperationStmt, parse->setOperations);
	Node	   *node;
	RangeTblEntry *leftmostRTE;
	Query	   *leftmostQuery;
	RelOptInfo *setop_rel;
	List	   *top_tlist;

	Assert(topop);

	/* 检查不支持的东西 */
	Assert(parse->jointree->fromlist == NIL);
	Assert(parse->jointree->quals == NULL);
	Assert(parse->groupClause == NIL);
	Assert(parse->havingQual == NULL);
	Assert(parse->windowClause == NIL);
	Assert(parse->distinctClause == NIL);

	/*
	 * 在外层查询层次，我们没有任何真正的等价要处理；
	 * 但我们确实想能做路径键，这将需要
	 * 单成员的 EquivalenceClasses。表明等价类合并已经完成，
	 * 以免 pathkeys.c 抱怨。
	 */
	Assert(root->eq_classes == NIL);
	root->ec_merging_done = true;

	/*
	 * 我们需要为每个叶子子查询建立 RelOptInfos，它们
	 * 是这个 Query 里的 RTE_SUBQUERY 范围表项。为那些以及万一需要的 AppendRelInfos
	 * 准备下标数组。
	 */
	setup_simple_rel_arrays(root);

	/*
	 * 找出最左边的组成 Query。我们需要用它的列名作为
	 * 全部生成的 tlist 的列名（否则 SELECT INTO 不能正常工作）。
	 */
	node = topop->larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTE = root->simple_rte_array[((RangeTblRef *) node)->rtindex];
	leftmostQuery = leftmostRTE->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * 若最顶层节点是递归并集，它需要特殊处理。
	 */
	if (root->hasRecursion)
	{
		setop_rel = generate_recursion_path(topop, root,
											leftmostQuery->targetList,
											&top_tlist);
	}
	else
	{
		/*
		 * 在 setOperations 树上递归，为集合运算生成路径。
		 * 最终输出路径应当正好有顶层节点所显示的列类型，
		 * 外加也许有的 resjunk 工作列
		 * （我们可以依靠上层节点来处理那个）。
		 */
		setop_rel = recurse_set_operations((Node *) topop, root,
										   topop->colTypes, topop->colCollations,
										   true, -1,
										   leftmostQuery->targetList,
										   &top_tlist,
										   NULL);
	}

	/* 必须把建好的 tlist 放进 root->processed_tlist 返回。 */
	root->processed_tlist = top_tlist;

	return setop_rel;
}

/*
 * recurse_set_operations
 *	    递归处理集合运算树里的一步
 *
 * colTypes：集合运算结果列数据类型的 OID 列表
 * colCollations：集合运算结果列排序规则的 OID 列表
 * junkOK：若为真，子项的 resjunk 列可以留在结果里
 * flag：若 >= 0，加一个表示 flag 值的 resjunk 输出列
 * refnames_tlist：从中取得列名的目标列表
 *
 * 返回该子树的 RelOptInfo，以及这些输出参数：
 * *pTargetList：收到该子树顶层计划的完整 tlist
 * *pNumGroups：若非 NULL，我们估计结果里互异组的数目
 *			并存到那里
 *
 * pTargetList 输出参数与返回的 RelOptInfo 的 pathtarget
 * 大体冗余，但目前我们需要它，因为本文件里的许多
 * 逻辑依赖于标志列被标成 resjunk。
 * 在重新设计那套机制之前，这是容易的出路。
 *
 * 这里不必关心 typmod：集合运算输入与输出 typmod 唯一允许的差别
 * 是输入是特定 typmod
 * 而输出是 -1，那不需要强制转换。
 */
static RelOptInfo *
recurse_set_operations(Node *setOp, PlannerInfo *root,
					   List *colTypes, List *colCollations,
					   bool junkOK,
					   int flag, List *refnames_tlist,
					   List **pTargetList,
					   double *pNumGroups)
{
	RelOptInfo *rel = NULL;		/* 消除编译器告警 */

	/* 防止因集合运算嵌套过深而栈溢出 */
	check_stack_depth();

	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = root->simple_rte_array[rtr->rtindex];
		Query	   *subquery = rte->subquery;
		PlannerInfo *subroot;
		RelOptInfo *final_rel;
		Path	   *subpath;
		Path	   *path;
		List	   *tlist;
		bool		trivial_tlist;

		Assert(subquery != NULL);

		/* 为这个叶子子查询建立一个 RelOptInfo。 */
		rel = build_simple_rel(root, rtr->rtindex, NULL);

		/* plan_params 不应当在当前查询层次使用 */
		Assert(root->plan_params == NIL);

		/* 为该子查询生成一个子根和若干 Path */
		subroot = rel->subroot = subquery_planner(root->glob, subquery,
												  root,
												  false,
												  root->tuple_fraction);

		/*
		 * 原始查询不应当可能含有对集合运算树里其他
		 * 原始查询的交叉引用。
		 */
		if (root->plan_params)
			elog(ERROR, "unexpected outer reference in set operation subquery");

		/* 弄清这个子查询的适当目标列表。 */
		tlist = generate_setop_tlist(colTypes, colCollations,
									 flag,
									 rtr->rtindex,
									 true,
									 subroot->processed_tlist,
									 refnames_tlist,
									 &trivial_tlist);
		rel->reltarget = create_pathtarget(root, tlist);

		/* 也把完整的 tlist 返回给调用方 */
		*pTargetList = tlist;

		/*
		 * 用估计的输出行数、宽度等标注该关系。注意必须
		 * 在生成外层查询路径之前做，否则
		 * cost_subqueryscan 会不高兴。
		 */
		set_subquery_size_estimates(root, rel);

		/*
		 * 由于我们也许想给这个关系加一条部分路径，必须
		 * 正确设置它的 consider_parallel 标志。
		 */
		final_rel = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
		rel->consider_parallel = final_rel->consider_parallel;

		/*
		 * 目前，我们对该子查询只考虑单条 Path。
		 * 这应当很快改变（让它看起来更像
		 * set_subquery_pathlist）。
		 */
		subpath = get_cheapest_fractional_path(final_rel,
											   root->tuple_fraction);

		/*
		 * 在那上面贴一条 SubqueryScanPath。
		 *
		 * 我们不费心确定子查询的输出顺序，因为
		 * 反正它不会反映在集合运算结果里；因此只把
		 * SubqueryScanPath 标成空路径键。（XXX 那应当也很快
		 * 改变。）
		 */
		path = (Path *) create_subqueryscan_path(root, rel, subpath,
												 trivial_tlist,
												 NIL, NULL);

		add_path(rel, path);

		/*
		 * 若子关系有一条部分路径，就可以用它
		 * 为这个关系建一条部分路径。但除了总代价最低的那条，
		 * 考虑任何路径都没有意义。
		 */
		if (rel->consider_parallel && bms_is_empty(rel->lateral_relids) &&
			final_rel->partial_pathlist != NIL)
		{
			Path	   *partial_subpath;
			Path	   *partial_path;

			partial_subpath = linitial(final_rel->partial_pathlist);
			partial_path = (Path *)
				create_subqueryscan_path(root, rel, partial_subpath,
										 trivial_tlist,
										 NIL, NULL);
			add_partial_path(rel, partial_path);
		}

		/*
		 * 若调用方想要，就估计组数。若子查询用了
		 * 分组或聚合，它的输出大概反正大体
		 * 互异；否则做统计估计。
		 *
		 * XXX 你其实不想知道这个：我们用子查询原来的
		 * 目标列表表达式做估计，
		 * 而不是看起来更合适的
		 * subroot->processed_tlist。原因是若子查询本身是集合运算，它也许返回一份
		 * 含有 generate_append_tlist 生成的 "varno 0" Var 的
		 * processed_tlist，那些会把 estimate_num_groups
		 * 搞得一塌糊涂。我们应当去掉 "varno 0" 这个权宜之计，但那
		 * 需要重新设计集合运算的解析树表示，以便
		 * 每个集合运算的输出都能有一个对应的 RTE。
		 */
		if (pNumGroups)
		{
			if (subquery->groupClause || subquery->groupingSets ||
				subquery->distinctClause ||
				subroot->hasHavingQual || subquery->hasAggs)
				*pNumGroups = subpath->rows;
			else
				*pNumGroups = estimate_num_groups(subroot,
												  get_tlist_exprs(subquery->targetList, false),
												  subpath->rows,
												  NULL,
												  NULL);
		}
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/* UNIONs 与 INTERSECT/EXCEPT 大不相同 */
		if (op->op == SETOP_UNION)
			rel = generate_union_paths(op, root,
									   refnames_tlist,
									   pTargetList);
		else
			rel = generate_nonunion_paths(op, root,
										  refnames_tlist,
										  pTargetList);
		if (pNumGroups)
			*pNumGroups = rel->rows;

		/*
		 * 若有必要，加一个 Result 节点来投影调用方请求的
		 * 输出列。
		 *
		 * XXX 你其实不想知道这个：setrefs.c 会对 Result 节点的 tlist 应用
		 * fix_upper_expr()。若 generate_setop_tlist() 生成的 Var
		 * 与子计划对应的 tlist 项并非恰好 equal()，
		 * 这会失败。不过，由于
		 * 子计划是由 generate_union_paths() 或
		 * generate_nonunion_paths() 生成的，因而它的 tlist 是由
		 * generate_append_tlist() 生成的，这能工作。我们只告诉
		 * generate_setop_tlist() 使用 varno 0。
		 */
		if (flag >= 0 ||
			!tlist_same_datatypes(*pTargetList, colTypes, junkOK) ||
			!tlist_same_collations(*pTargetList, colCollations, junkOK))
		{
			PathTarget *target;
			bool		trivial_tlist;
			ListCell   *lc;

			*pTargetList = generate_setop_tlist(colTypes, colCollations,
												flag,
												0,
												false,
												*pTargetList,
												refnames_tlist,
												&trivial_tlist);
			target = create_pathtarget(root, *pTargetList);

			/* 对每条路径应用投影 */
			foreach(lc, rel->pathlist)
			{
				Path	   *subpath = (Path *) lfirst(lc);
				Path	   *path;

				Assert(subpath->param_info == NULL);
				path = apply_projection_to_path(root, subpath->parent,
												subpath, target);
				/* 若不得不加 Result，path 就与 subpath 不同 */
				if (path != subpath)
					lfirst(lc) = path;
			}

			/* 对每条部分路径应用投影 */
			foreach(lc, rel->partial_pathlist)
			{
				Path	   *subpath = (Path *) lfirst(lc);
				Path	   *path;

				Assert(subpath->param_info == NULL);

				/* 避免 apply_projection_to_path，以防多次引用 */
				path = (Path *) create_projection_path(root, subpath->parent,
													   subpath, target);
				lfirst(lc) = path;
			}
		}
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
		*pTargetList = NIL;
	}

	postprocess_setop_rel(root, rel);

	return rel;
}

/*
 * 为递归 UNION 节点生成路径
 */
static RelOptInfo *
generate_recursion_path(SetOperationStmt *setOp, PlannerInfo *root,
						List *refnames_tlist,
						List **pTargetList)
{
	RelOptInfo *result_rel;
	Path	   *path;
	RelOptInfo *lrel,
			   *rrel;
	Path	   *lpath;
	Path	   *rpath;
	List	   *lpath_tlist;
	List	   *rpath_tlist;
	List	   *tlist;
	List	   *groupList;
	double		dNumGroups;

	/* 解析器应当已经拒绝了其他情形 */
	if (setOp->op != SETOP_UNION)
		elog(ERROR, "only UNION queries can be recursive");
	/* 应当已经分配了工作表 ID */
	Assert(root->wt_param_id >= 0);

	/*
	 * 与普通 UNION 节点不同，左右输入分开处理，
	 * 并不打算把它们合成一个 Append。
	 */
	lrel = recurse_set_operations(setOp->larg, root,
								  setOp->colTypes, setOp->colCollations,
								  false, -1,
								  refnames_tlist,
								  &lpath_tlist,
								  NULL);
	lpath = lrel->cheapest_total_path;
	/* 右路径会想看左路径…… */
	root->non_recursive_path = lpath;
	rrel = recurse_set_operations(setOp->rarg, root,
								  setOp->colTypes, setOp->colCollations,
								  false, -1,
								  refnames_tlist,
								  &rpath_tlist,
								  NULL);
	rpath = rrel->cheapest_total_path;
	root->non_recursive_path = NULL;

	/*
	 * 为 RecursiveUnion 路径节点生成 tlist——与 Append 情形相同
	 */
	tlist = generate_append_tlist(setOp->colTypes, setOp->colCollations, false,
								  list_make2(lpath_tlist, rpath_tlist),
								  refnames_tlist);

	*pTargetList = tlist;

	/* 建立结果关系。 */
	result_rel = fetch_upper_rel(root, UPPERREL_SETOP,
								 bms_union(lrel->relids, rrel->relids));
	result_rel->reltarget = create_pathtarget(root, tlist);

	/*
	 * 若是 UNION，识别分组操作符
	 */
	if (setOp->all)
	{
		groupList = NIL;
		dNumGroups = 0;
	}
	else
	{
		/* 识别分组语义 */
		groupList = generate_setop_grouplist(setOp, tlist);

		/* 这里我们只支持哈希 */
		if (!grouping_is_hashable(groupList))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not implement recursive UNION"),
					 errdetail("All column datatypes must be hashable.")));

		/*
		 * 目前，把互异组数取成等于
		 * 总输入大小，即最坏情况。
		 */
		dNumGroups = lpath->rows + rpath->rows * 10;
	}

	/*
	 * 并做出路径节点。
	 */
	path = (Path *) create_recursiveunion_path(root,
											   result_rel,
											   lpath,
											   rpath,
											   result_rel->reltarget,
											   groupList,
											   root->wt_param_id,
											   dNumGroups);

	add_path(result_rel, path);
	postprocess_setop_rel(root, result_rel);
	return result_rel;
}

/*
 * 为 UNION 或 UNION ALL 节点生成路径
 */
static RelOptInfo *
generate_union_paths(SetOperationStmt *op, PlannerInfo *root,
					 List *refnames_tlist,
					 List **pTargetList)
{
	Relids		relids = NULL;
	RelOptInfo *result_rel;
	double		save_fraction = root->tuple_fraction;
	ListCell   *lc;
	List	   *pathlist = NIL;
	List	   *partial_pathlist = NIL;
	bool		partial_paths_valid = true;
	bool		consider_parallel = true;
	List	   *rellist;
	List	   *tlist_list;
	List	   *tlist;
	Path	   *path;

	/*
	 * 若是普通 UNION，告诉子项取出全部元组。
	 *
	 * 注意：在 UNION ALL 里，我们把顶层 tuple_fraction 原样传给
	 * UNION ALL 的每一支。也可以主张对后面的支减少元组
	 * 比例（按更早各支结果的预期大小打折），但似乎不值得费劲。
	 * tuple_fraction 还不是零的正常
	 * 情形是顶层有 LIMIT，
	 * 原样传下去通常就足以得到
	 * 偏好快速启动计划这一想要的结果。
	 */
	if (!op->all)
		root->tuple_fraction = 0.0;

	/*
	 * 若我的任何子项是相同的 UNION 节点（同样的操作、all 标志，以及
	 * colTypes/colCollations），就可以把它们并进这个节点，以便
	 * 我们只为这一批生成一个 Append 和一次去重。递归
	 * 找出这样的节点并计算其子项的路径。
	 */
	rellist = plan_union_children(root, op, refnames_tlist, &tlist_list);

	/*
	 * 为 Append 计划节点生成 tlist。
	 *
	 * 就 Append 而言，Append 计划的 tlist 并不重要，
	 * 但为了再上一层计划的利益，
	 * 我们仍必须让它看起来真实。
	 */
	tlist = generate_append_tlist(op->colTypes, op->colCollations, false,
								  tlist_list, refnames_tlist);

	*pTargetList = tlist;

	/* 建立路径列表和 relid 集合。 */
	foreach(lc, rellist)
	{
		RelOptInfo *rel = lfirst(lc);

		pathlist = lappend(pathlist, rel->cheapest_total_path);

		if (consider_parallel)
		{
			if (!rel->consider_parallel)
			{
				consider_parallel = false;
				partial_paths_valid = false;
			}
			else if (rel->partial_pathlist == NIL)
				partial_paths_valid = false;
			else
				partial_pathlist = lappend(partial_pathlist,
										   linitial(rel->partial_pathlist));
		}

		relids = bms_union(relids, rel->relids);
	}

	/* 建立结果关系。 */
	result_rel = fetch_upper_rel(root, UPPERREL_SETOP, relids);
	result_rel->reltarget = create_pathtarget(root, tlist);
	result_rel->consider_parallel = consider_parallel;

	/*
	 * 把子项结果追加到一起。
	 */
	path = (Path *) create_append_path(root, result_rel, pathlist, NIL,
									   NIL, NULL, 0, false, -1);

	/*
	 * 对 UNION ALL，我们只需要 Append 路径。对 UNION，需要加上
	 * 去掉重复的节点。
	 */
	if (!op->all)
		path = make_union_unique(op, path, tlist, root);

	add_path(result_rel, path);

	/*
	 * 估计组数。目前我们只假定输出是互异的
	 * ——对 UNION 情形这肯定为真，而且我们反正想要最坏情况
	 * 估计。
	 */
	result_rel->rows = path->rows;

	/*
	 * 现在考虑用部分路径加上 Append
	 * 再加上 Gather 做同样的事。
	 */
	if (partial_paths_valid)
	{
		Path	   *ppath;
		int			parallel_workers = 0;

		/* 找出任何子路径所请求的最高工作进程数。 */
		foreach(lc, partial_pathlist)
		{
			Path	   *subpath = lfirst(lc);

			parallel_workers = Max(parallel_workers,
								   subpath->parallel_workers);
		}
		Assert(parallel_workers > 0);

		/*
		 * 若允许使用并行 append，总是至少请求
		 * log2（子项数）条路径。我们假定这种情况下多一些
		 * 工作进程会有用，因为它们会分散到
		 * 各个子项上。精确公式只是猜测；见
		 * add_paths_to_append_rel。
		 */
		if (enable_parallel_append)
		{
			parallel_workers = Max(parallel_workers,
								   pg_leftmost_one_pos32(list_length(partial_pathlist)) + 1);
			parallel_workers = Min(parallel_workers,
								   max_parallel_workers_per_gather);
		}
		Assert(parallel_workers > 0);

		ppath = (Path *)
			create_append_path(root, result_rel, NIL, partial_pathlist,
							   NIL, NULL,
							   parallel_workers, enable_parallel_append,
							   -1);
		ppath = (Path *)
			create_gather_path(root, result_rel, ppath,
							   result_rel->reltarget, NULL, NULL);
		if (!op->all)
			ppath = make_union_unique(op, ppath, tlist, root);
		add_path(result_rel, ppath);
	}

	/* 撤销也许把 tuple_fraction 强制成 0 的效果 */
	root->tuple_fraction = save_fraction;

	return result_rel;
}

/*
 * 为 INTERSECT、INTERSECT ALL、EXCEPT 或 EXCEPT ALL 节点生成路径
 */
static RelOptInfo *
generate_nonunion_paths(SetOperationStmt *op, PlannerInfo *root,
						List *refnames_tlist,
						List **pTargetList)
{
	RelOptInfo *result_rel;
	RelOptInfo *lrel,
			   *rrel;
	double		save_fraction = root->tuple_fraction;
	Path	   *lpath,
			   *rpath,
			   *path;
	List	   *lpath_tlist,
			   *rpath_tlist,
			   *tlist_list,
			   *tlist,
			   *groupList,
			   *pathlist;
	double		dLeftGroups,
				dRightGroups,
				dNumGroups,
				dNumOutputRows;
	bool		use_hash;
	SetOpCmd	cmd;
	int			firstFlag;

	/*
	 * 告诉子项取出全部元组。
	 */
	root->tuple_fraction = 0.0;

	/* 在子项上递归，确保它们的输出被标注 */
	lrel = recurse_set_operations(op->larg, root,
								  op->colTypes, op->colCollations,
								  false, 0,
								  refnames_tlist,
								  &lpath_tlist,
								  &dLeftGroups);
	lpath = lrel->cheapest_total_path;
	rrel = recurse_set_operations(op->rarg, root,
								  op->colTypes, op->colCollations,
								  false, 1,
								  refnames_tlist,
								  &rpath_tlist,
								  &dRightGroups);
	rpath = rrel->cheapest_total_path;

	/* 撤销把 tuple_fraction 强制成 0 的效果 */
	root->tuple_fraction = save_fraction;

	/*
	 * 对 EXCEPT，必须把左输入放在前面。对 INTERSECT，哪种
	 * 顺序都应当给出同样结果，我们更愿意把较小的
	 * 输入放在前面，以尽量减小哈希情形下哈希表的大小。
	 * "较小"指组数较少的那个。
	 */
	if (op->op == SETOP_EXCEPT || dLeftGroups <= dRightGroups)
	{
		pathlist = list_make2(lpath, rpath);
		tlist_list = list_make2(lpath_tlist, rpath_tlist);
		firstFlag = 0;
	}
	else
	{
		pathlist = list_make2(rpath, lpath);
		tlist_list = list_make2(rpath_tlist, lpath_tlist);
		firstFlag = 1;
	}

	/*
	 * 为 Append 计划节点生成 tlist。
	 *
	 * 就 Append 而言，Append 计划的 tlist 并不重要，
	 * 但为了上一层计划，我们仍必须让它看起来真实。
	 * 事实上，它必须真实到标志
	 * 列显示为变量而不是常量，否则 setrefs.c 会
	 * 搞混。
	 */
	tlist = generate_append_tlist(op->colTypes, op->colCollations, true,
								  tlist_list, refnames_tlist);

	*pTargetList = tlist;

	/* 建立结果关系。 */
	result_rel = fetch_upper_rel(root, UPPERREL_SETOP,
								 bms_union(lrel->relids, rrel->relids));
	result_rel->reltarget = create_pathtarget(root, tlist);

	/*
	 * 把子项结果追加到一起。
	 */
	path = (Path *) create_append_path(root, result_rel, pathlist, NIL,
									   NIL, NULL, 0, false, -1);

	/* 识别分组语义 */
	groupList = generate_setop_grouplist(op, tlist);

	/*
	 * 估计我们需要哈希表项的互异组数；
	 * 对 EXCEPT 这是左手输入的大小，对 INTERSECT 则是较小的
	 * 输入。也估计最终输出行数。
	 * 在非 ALL 情形，我们估计每个组产生一行输出；在
	 * ALL 情形使用相关关系的大小。这些当然是最坏情况
	 * 估计，但我们需要保守。
	 */
	if (op->op == SETOP_EXCEPT)
	{
		dNumGroups = dLeftGroups;
		dNumOutputRows = op->all ? lpath->rows : dNumGroups;
	}
	else
	{
		dNumGroups = Min(dLeftGroups, dRightGroups);
		dNumOutputRows = op->all ? Min(lpath->rows, rpath->rows) : dNumGroups;
	}

	/*
	 * 决定哈希还是排序，并在需要时加一个排序节点。
	 */
	use_hash = choose_hashed_setop(root, groupList, path,
								   dNumGroups, dNumOutputRows,
								   (op->op == SETOP_INTERSECT) ? "INTERSECT" : "EXCEPT");

	if (groupList && !use_hash)
		path = (Path *) create_sort_path(root,
										 result_rel,
										 path,
										 make_pathkeys_for_sortclauses(root,
																	   groupList,
																	   tlist),
										 -1.0);

	/*
	 * 最后，加一个 SetOp 路径节点以生成正确的输出。
	 */
	switch (op->op)
	{
		case SETOP_INTERSECT:
			cmd = op->all ? SETOPCMD_INTERSECT_ALL : SETOPCMD_INTERSECT;
			break;
		case SETOP_EXCEPT:
			cmd = op->all ? SETOPCMD_EXCEPT_ALL : SETOPCMD_EXCEPT;
			break;
		default:
			elog(ERROR, "unrecognized set op: %d", (int) op->op);
			cmd = SETOPCMD_INTERSECT;	/* 消除编译器告警 */
			break;
	}
	path = (Path *) create_setop_path(root,
									  result_rel,
									  path,
									  cmd,
									  use_hash ? SETOP_HASHED : SETOP_SORTED,
									  groupList,
									  list_length(op->colTypes) + 1,
									  use_hash ? firstFlag : -1,
									  dNumGroups,
									  dNumOutputRows);

	result_rel->rows = path->rows;
	add_path(result_rel, path);
	return result_rel;
}

/*
 * 上拉属性相同的 UNIONs 的 UNION 子项，
 * 并对 N 路 UNION 下面的查询做规划。
 *
 * 结果是一份含有子节点 Path 的 RelOptInfos 列表，
 * 每个作为叶子查询或非相同集合运算的后代一项。
 * 我们也返回一份子项目标列表的并行列表。
 *
 * 注意：也可以把 UNION ALL 上拉进 UNION，因为互异的
 * 输出行反正会丢掉。
 */
static List *
plan_union_children(PlannerInfo *root,
					SetOperationStmt *top_union,
					List *refnames_tlist,
					List **tlist_list)
{
	List	   *pending_rels = list_make1(top_union);
	List	   *result = NIL;
	List	   *child_tlist;

	*tlist_list = NIL;

	while (pending_rels != NIL)
	{
		Node	   *setOp = linitial(pending_rels);

		pending_rels = list_delete_first(pending_rels);

		if (IsA(setOp, SetOperationStmt))
		{
			SetOperationStmt *op = (SetOperationStmt *) setOp;

			if (op->op == top_union->op &&
				(op->all == top_union->all || op->all) &&
				equal(op->colTypes, top_union->colTypes) &&
				equal(op->colCollations, top_union->colCollations))
			{
				/* 同一个 UNION，因此把子项折进父项 */
				pending_rels = lcons(op->rarg, pending_rels);
				pending_rels = lcons(op->larg, pending_rels);
				continue;
			}
		}

		/*
		 * 不相同，因此单独规划这个子项。
		 *
		 * 注意我们不允许子结果里有任何 resjunk 列。这是
		 * 必要的，因为实现并集的 Append 节点不会做
		 * 任何投影，若我们的一些输出元组有杂项、一些没有，上层会搞混。
		 * 只有当
		 * 子项是 EXCEPT 或 INTERSECT 时才会出现这种情形，否则反正不会有
		 * resjunk。
		 */
		result = lappend(result, recurse_set_operations(setOp, root,
														top_union->colTypes,
														top_union->colCollations,
														false, -1,
														refnames_tlist,
														&child_tlist,
														NULL));
		*tlist_list = lappend(*tlist_list, child_tlist);
	}

	return result;
}

/*
 * 向给定路径树加节点，使 UNION 的结果互异。
 */
static Path *
make_union_unique(SetOperationStmt *op, Path *path, List *tlist,
				  PlannerInfo *root)
{
	RelOptInfo *result_rel = fetch_upper_rel(root, UPPERREL_SETOP, NULL);
	List	   *groupList;
	double		dNumGroups;

	/* 识别分组语义 */
	groupList = generate_setop_grouplist(op, tlist);

	/*
	 * XXX 目前，把互异组数取成等于
	 * 总输入大小，即最坏情况。这过于保守，但
	 * 不清楚如何得到真实大小的像样估计。也应当
	 * 注意到新手即使不预期任何重复，也倾向写 UNION 而不是
	 * UNION ALL……
	 */
	dNumGroups = path->rows;

	/* 决定哈希还是排序 */
	if (choose_hashed_setop(root, groupList, path,
							dNumGroups, dNumGroups,
							"UNION"))
	{
		/* 哈希聚合计划——不需要排序 */
		path = (Path *) create_agg_path(root,
										result_rel,
										path,
										create_pathtarget(root, tlist),
										AGG_HASHED,
										AGGSPLIT_SIMPLE,
										groupList,
										NIL,
										NULL,
										dNumGroups);
	}
	else
	{
		/* 排序然后 Unique */
		if (groupList)
			path = (Path *)
				create_sort_path(root,
								 result_rel,
								 path,
								 make_pathkeys_for_sortclauses(root,
															   groupList,
															   tlist),
								 -1.0);
		path = (Path *) create_upper_unique_path(root,
												 result_rel,
												 path,
												 list_length(path->pathkeys),
												 dNumGroups);
	}

	return path;
}

/*
 * postprocess_setop_rel - 执行添加路径之后所需的步骤
 */
static void
postprocess_setop_rel(PlannerInfo *root, RelOptInfo *rel)
{
	/*
	 * 目前我们不操心让 FDWs 为这个关系贡献路径，
	 * 但给扩展一个机会。
	 */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_SETOP,
									NULL, rel, NULL);

	/* 选择总代价最低的路径 */
	set_cheapest(rel);
}

/*
 * choose_hashed_setop - 集合运算是否应当使用哈希？
 */
static bool
choose_hashed_setop(PlannerInfo *root, List *groupClauses,
					Path *input_path,
					double dNumGroups, double dNumOutputRows,
					const char *construct)
{
	int			numGroupCols = list_length(groupClauses);
	Size		hash_mem_limit = get_hash_memory_limit();
	bool		can_sort;
	bool		can_hash;
	Size		hashentrysize;
	Path		hashed_p;
	Path		sorted_p;
	double		tuple_fraction;

	/* 检查操作符是否支持排序或哈希 */
	can_sort = grouping_is_sortable(groupClauses);
	can_hash = grouping_is_hashable(groupClauses);
	if (can_hash && can_sort)
	{
		/* 我们有一个有意义的选择要做，继续…… */
	}
	else if (can_hash)
		return true;
	else if (can_sort)
		return false;
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/* translator: %s is UNION, INTERSECT, or EXCEPT */
				 errmsg("could not implement %s", construct),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/* 当 enable_hashagg 关闭时偏好排序 */
	if (!enable_hashagg)
		return false;

	/*
	 * 若看起来哈希表放不进
	 * hash_mem，就不要做。
	 */
	hashentrysize = MAXALIGN(input_path->pathtarget->width) + MAXALIGN(SizeofMinimalTupleHeader);

	if (hashentrysize * dNumGroups > hash_mem_limit)
		return false;

	/*
	 * 看估计代价是否不超过用另一种办法做。
	 *
	 * 我们需要考虑 input_plan + 哈希聚合相对于 input_plan + 排序 +
	 * 分组。注意实际结果计划也许涉及 SetOp 或
	 * Unique 节点，而不是 Agg 或 Group，但 Agg 和 Group 的代价估计
	 * 对我们这里的目的应当足够接近。
	 *
	 * 这些路径变量是只保存代价字段的占位；我们不为
	 * 这些步骤做真正的 Path。
	 */
	cost_agg(&hashed_p, root, AGG_HASHED, NULL,
			 numGroupCols, dNumGroups,
			 NIL,
			 input_path->startup_cost, input_path->total_cost,
			 input_path->rows, input_path->pathtarget->width);

	/*
	 * 现在是已排序情形。注意输入*总是*未排序的，
	 * 因为它是把互不相关的子关系追加而成的。
	 */
	sorted_p.startup_cost = input_path->startup_cost;
	sorted_p.total_cost = input_path->total_cost;
	/* XXX cost_sort 实际上不看路径键，因此就传 NIL */
	cost_sort(&sorted_p, root, NIL, sorted_p.total_cost,
			  input_path->rows, input_path->pathtarget->width,
			  0.0, work_mem, -1.0);
	cost_group(&sorted_p, root, numGroupCols, dNumGroups,
			   NIL,
			   sorted_p.startup_cost, sorted_p.total_cost,
			   input_path->rows);

	/*
	 * 现在用顶层元组比例做决定。首先必须
	 * 把绝对计数（LIMIT）转换成比例形式。
	 */
	tuple_fraction = root->tuple_fraction;
	if (tuple_fraction >= 1.0)
		tuple_fraction /= dNumOutputRows;

	if (compare_fractional_path_costs(&hashed_p, &sorted_p,
									  tuple_fraction) < 0)
	{
		/* 哈希更便宜，因此用它 */
		return true;
	}
	return false;
}

/*
 * 为集合运算计划节点生成目标列表
 *
 * colTypes：集合运算结果列数据类型的 OID 列表
 * colCollations：集合运算结果列排序规则的 OID 列表
 * flag：若不需要标志列则为 -1，0 或 1 则建立一个常量标志列
 * varno：生成的 Var 里使用的 varno
 * hack_constants：为真则上复制常量（见代码里的注释）
 * input_tlist：这个节点的输入节点的目标列表
 * refnames_tlist：从中取得列名的目标列表
 * trivial_tlist：输出参数，若目标列表平凡则设为真
 */
static List *
generate_setop_tlist(List *colTypes, List *colCollations,
					 int flag,
					 Index varno,
					 bool hack_constants,
					 List *input_tlist,
					 List *refnames_tlist,
					 bool *trivial_tlist)
{
	List	   *tlist = NIL;
	int			resno = 1;
	ListCell   *ctlc,
			   *cclc,
			   *itlc,
			   *rtlc;
	TargetEntry *tle;
	Node	   *expr;

	*trivial_tlist = true;		/* 直到被证明并非如此 */

	forfour(ctlc, colTypes, cclc, colCollations,
			itlc, input_tlist, rtlc, refnames_tlist)
	{
		Oid			colType = lfirst_oid(ctlc);
		Oid			colColl = lfirst_oid(cclc);
		TargetEntry *inputtle = (TargetEntry *) lfirst(itlc);
		TargetEntry *reftle = (TargetEntry *) lfirst(rtlc);

		Assert(inputtle->resno == resno);
		Assert(reftle->resno == resno);
		Assert(!inputtle->resjunk);
		Assert(!reftle->resjunk);

		/*
		 * 生成引用输入列、并具有适当
		 * 数据类型和列名的列。在
		 * 必要处插入数据类型强制转换。
		 *
		 * 权宜：输入目标列表里的常量按原样上复制，
		 * 而不是作为子查询输出被引用。这主要是
		 * 为了确保当我们试图把它们强制成输出列的
		 * 数据类型时，对 UNKNOWN 常量发生正确的事。但只在
		 * 子查询扫描计划的第一层这么做；我们不想让
		 * 假常量出现在上层节点的
		 * 输出 tlist 里！
		 *
		 * 注意，复制常量本身并不需要我们把
		 * tlist 标成非平凡；见 setrefs.c 里的 trivial_subqueryscan()。
		 */
		if (hack_constants && inputtle->expr && IsA(inputtle->expr, Const))
			expr = (Node *) inputtle->expr;
		else
			expr = (Node *) makeVar(varno,
									inputtle->resno,
									exprType((Node *) inputtle->expr),
									exprTypmod((Node *) inputtle->expr),
									exprCollation((Node *) inputtle->expr),
									0);

		if (exprType(expr) != colType)
		{
			/*
			 * 注意：在这里应用 coerce_to_common_type
			 * 并不真的妥当；一个显著点是 assign_expr_collations 从不
			 * 在任何生成的节点上运行。目前那不是
			 * 问题，因为我们在下面强制正确的暴露排序规则。
			 * 不过，最好让解析器从一开始就为每个集合运算生成正确的
			 * 输出 tlist。
			 */
			expr = coerce_to_common_type(NULL,	/* 这里没有 UNKNOWNs */
										 expr,
										 colType,
										 "UNION/INTERSECT/EXCEPT");
			*trivial_tlist = false; /* 强制转换使它不再平凡 */
		}

		/*
		 * 确保 tlist 项暴露的排序规则匹配集合运算。这是
		 * 必要的，因为 plan_set_operations() 把结果
		 * 顺序报告为一份 SortGroupClauses 列表，它们本身不携带排序规则，
		 * 只引用 tlist 项。若我们不显示
		 * 正确的排序规则，planner.c 在
		 * 更高层查询里也许会做错。
		 *
		 * 注意我们用 RelabelType 而不是 CollateExpr，因为这个表达式
		 * 将不再经任何进一步处理就到达执行器。
		 */
		if (exprCollation(expr) != colColl)
		{
			expr = applyRelabelType(expr,
									exprType(expr), exprTypmod(expr), colColl,
									COERCE_IMPLICIT_CAST, -1, false);
			*trivial_tlist = false; /* 重标使它不再平凡 */
		}

		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup(reftle->resname),
							  false);

		/*
		 * 按约定，集合运算树里全部非 resjunk 列的
		 * ressortgroupref 等于它们的 resno。有些情况下并不需要这个引用，
		 * 但这比以后修改 tlist 更干净。
		 */
		tle->ressortgroupref = tle->resno;

		tlist = lappend(tlist, tle);
	}

	if (flag >= 0)
	{
		/* 加一个 resjunk 标志列 */
		/* 标志值是给定的常量 */
		expr = (Node *) makeConst(INT4OID,
								  -1,
								  InvalidOid,
								  sizeof(int32),
								  Int32GetDatum(flag),
								  false,
								  true);
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup("flag"),
							  true);
		tlist = lappend(tlist, tle);
		*trivial_tlist = false; /* 这个额外项使它不再平凡 */
	}

	return tlist;
}

/*
 * 为集合运算 Append 节点生成目标列表
 *
 * colTypes：集合运算结果列数据类型的 OID 列表
 * colCollations：集合运算结果列排序规则的 OID 列表
 * flag：为真则建立一个从子计划上复制的标志列
 * input_tlists：该 Append 的子计划的 tlist 列表
 * refnames_tlist：从中取得列名的目标列表
 *
 * Append 目标列表里的项应当总是简单 Var；
 * 我们只需要确保它们有正确的数据类型/typmod/排序规则。
 * 这些 Var 总是用 varno 0 生成。
 *
 * XXX varno 为零这个办法的一个问题是，set_pathtarget_cost_width
 * 无法为我们在这里做的 tlist 算出现实的宽度。但我们
 * 反正应当重构这段代码，直接产生 PathTarget。
 */
static List *
generate_append_tlist(List *colTypes, List *colCollations,
					  bool flag,
					  List *input_tlists,
					  List *refnames_tlist)
{
	List	   *tlist = NIL;
	int			resno = 1;
	ListCell   *curColType;
	ListCell   *curColCollation;
	ListCell   *ref_tl_item;
	int			colindex;
	TargetEntry *tle;
	Node	   *expr;
	ListCell   *tlistl;
	int32	   *colTypmods;

	/*
	 * 首先抽出要使用的 typmod。
	 *
	 * 若全部输入在某一列的类型和 typmod 上一致，就用
	 * 那个 typmod；否则用 -1。
	 */
	colTypmods = (int32 *) palloc(list_length(colTypes) * sizeof(int32));

	foreach(tlistl, input_tlists)
	{
		List	   *subtlist = (List *) lfirst(tlistl);
		ListCell   *subtlistl;

		curColType = list_head(colTypes);
		colindex = 0;
		foreach(subtlistl, subtlist)
		{
			TargetEntry *subtle = (TargetEntry *) lfirst(subtlistl);

			if (subtle->resjunk)
				continue;
			Assert(curColType != NULL);
			if (exprType((Node *) subtle->expr) == lfirst_oid(curColType))
			{
				/* 若是第一个子计划，就复制 typmod；否则比较 */
				int32		subtypmod = exprTypmod((Node *) subtle->expr);

				if (tlistl == list_head(input_tlists))
					colTypmods[colindex] = subtypmod;
				else if (subtypmod != colTypmods[colindex])
					colTypmods[colindex] = -1;
			}
			else
			{
				/* 类型不一致，因此把 typmod 强制成 -1 */
				colTypmods[colindex] = -1;
			}
			curColType = lnext(colTypes, curColType);
			colindex++;
		}
		Assert(curColType == NULL);
	}

	/*
	 * 现在可以为 Append 建立 tlist。
	 */
	colindex = 0;
	forthree(curColType, colTypes, curColCollation, colCollations,
			 ref_tl_item, refnames_tlist)
	{
		Oid			colType = lfirst_oid(curColType);
		int32		colTypmod = colTypmods[colindex++];
		Oid			colColl = lfirst_oid(curColCollation);
		TargetEntry *reftle = (TargetEntry *) lfirst(ref_tl_item);

		Assert(reftle->resno == resno);
		Assert(!reftle->resjunk);
		expr = (Node *) makeVar(0,
								resno,
								colType,
								colTypmod,
								colColl,
								0);
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup(reftle->resname),
							  false);

		/*
		 * 按约定，集合运算树里全部非 resjunk 列的
		 * ressortgroupref 等于它们的 resno。有些情况下并不需要这个引用，
		 * 但这比以后修改 tlist 更干净。
		 */
		tle->ressortgroupref = tle->resno;

		tlist = lappend(tlist, tle);
	}

	if (flag)
	{
		/* 加一个 resjunk 标志列 */
		/* 标志值显示为从子计划上复制 */
		expr = (Node *) makeVar(0,
								resno,
								INT4OID,
								-1,
								InvalidOid,
								0);
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup("flag"),
							  true);
		tlist = lappend(tlist, tle);
	}

	pfree(colTypmods);

	return tlist;
}

/*
 * generate_setop_grouplist
 *			建立一份定义集合运算输出列的排序/分组属性的
 *			SortGroupClause 列表。
 *
 * 语法分析已经确定了这些属性并建了一份合适的
 * 列表，只不过项没有设置 sortgroupref，因为
 * 解析器输出表示不为每个
 * 集合运算包含一份 tlist。因此这里需要做的是复制那份列表，并装上
 * 正确的 sortgroupref（从目标列表复制那些）。
 */
static List *
generate_setop_grouplist(SetOperationStmt *op, List *targetlist)
{
	List	   *grouplist = copyObject(op->groupClauses);
	ListCell   *lg;
	ListCell   *lt;

	lg = list_head(grouplist);
	foreach(lt, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lt);
		SortGroupClause *sgc;

		if (tle->resjunk)
		{
			/* resjunk 列不应当有 sortgroupref */
			Assert(tle->ressortgroupref == 0);
			continue;			/* 忽略 resjunk 列 */
		}

		/* 非 resjunk 列应当有 sortgroupref = resno */
		Assert(tle->ressortgroupref == tle->resno);

		/* 非 resjunk 列应当有分组子句 */
		Assert(lg != NULL);
		sgc = (SortGroupClause *) lfirst(lg);
		lg = lnext(grouplist, lg);
		Assert(sgc->tleSortGroupRef == 0);

		sgc->tleSortGroupRef = tle->ressortgroupref;
	}
	Assert(lg == NULL);
	return grouplist;
}
