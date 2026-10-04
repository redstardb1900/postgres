/*-------------------------------------------------------------------------
 *
 * createplan.c
 *	  Routines to create the desired plan for processing a query.
 *	  Planning is complete, we just need to convert the selected
 *	  Path into a Plan.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/createplan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/sysattr.h"
#include "catalog/pg_class.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "partitioning/partprune.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"


/*
 * 可以出现在 create_plan_recurse() 的 flags 参数里的标志位。
 * 这些可以 OR 在一起。
 *
 * CP_EXACT_TLIST 指定生成的计划节点必须恰好返回
 * 路径的 pathtarget 所指定的 tlist（若也设置了
 * CP_SMALL_TLIST 和 CP_LABEL_TLIST，则覆盖它们）。否则，
 * 计划节点可以只返回求值 pathtarget 所需的 Var 和 PlaceHolderVars。
 * CP_SMALL_TLIST 指定更窄的 tlist 更可取。这由
 *
 * Sort 和 Hash 这类父节点向下传递，它们必须
 * 存储返回的元组。
 * CP_LABEL_TLIST 指定计划节点必须返回与
 *
 * 其 pathtarget 里指定的任何 sortgroupref 匹配的列，并带上适当的
 * ressortgroupref 标注。这由 Sort
 * 和 Group 这类父节点向下传递，它们需要这些值在其输入里可用。
 * CP_IGNORE_TLIST 指定调用方打算替换目标列表，
 *
 * 因此生成什么样的目标列表
 * 一点也不要紧。
 */
#define CP_EXACT_TLIST		0x0001	/* 计划必须返回指定的 tlist */
#define CP_SMALL_TLIST		0x0002	/* 更窄的 tlist 更可取 */
#define CP_LABEL_TLIST		0x0004	/* tlist 必须含有 sortgroupref */
#define CP_IGNORE_TLIST		0x0008	/* 调用方会替换 tlist */


static Plan *create_plan_recurse(PlannerInfo *root, Path *best_path,
								 int flags);
static Plan *create_scan_plan(PlannerInfo *root, Path *best_path,
							  int flags);
static List *build_path_tlist(PlannerInfo *root, Path *path);
static bool use_physical_tlist(PlannerInfo *root, Path *path, int flags);
static List *get_gating_quals(PlannerInfo *root, List *quals);
static Plan *create_gating_plan(PlannerInfo *root, Path *path, Plan *plan,
								List *gating_quals);
static Plan *create_join_plan(PlannerInfo *root, JoinPath *best_path);
static bool mark_async_capable_plan(Plan *plan, Path *path);
static Plan *create_append_plan(PlannerInfo *root, AppendPath *best_path,
								int flags);
static Plan *create_merge_append_plan(PlannerInfo *root, MergeAppendPath *best_path,
									  int flags);
static Result *create_group_result_plan(PlannerInfo *root,
										GroupResultPath *best_path);
static ProjectSet *create_project_set_plan(PlannerInfo *root, ProjectSetPath *best_path);
static Material *create_material_plan(PlannerInfo *root, MaterialPath *best_path,
									  int flags);
static Memoize *create_memoize_plan(PlannerInfo *root, MemoizePath *best_path,
									int flags);
static Plan *create_unique_plan(PlannerInfo *root, UniquePath *best_path,
								int flags);
static Gather *create_gather_plan(PlannerInfo *root, GatherPath *best_path);
static Plan *create_projection_plan(PlannerInfo *root,
									ProjectionPath *best_path,
									int flags);
static Plan *inject_projection_plan(Plan *subplan, List *tlist, bool parallel_safe);
static Sort *create_sort_plan(PlannerInfo *root, SortPath *best_path, int flags);
static IncrementalSort *create_incrementalsort_plan(PlannerInfo *root,
													IncrementalSortPath *best_path, int flags);
static Group *create_group_plan(PlannerInfo *root, GroupPath *best_path);
static Unique *create_upper_unique_plan(PlannerInfo *root, UpperUniquePath *best_path,
										int flags);
static Agg *create_agg_plan(PlannerInfo *root, AggPath *best_path);
static Plan *create_groupingsets_plan(PlannerInfo *root, GroupingSetsPath *best_path);
static Result *create_minmaxagg_plan(PlannerInfo *root, MinMaxAggPath *best_path);
static WindowAgg *create_windowagg_plan(PlannerInfo *root, WindowAggPath *best_path);
static SetOp *create_setop_plan(PlannerInfo *root, SetOpPath *best_path,
								int flags);
static RecursiveUnion *create_recursiveunion_plan(PlannerInfo *root, RecursiveUnionPath *best_path);
static LockRows *create_lockrows_plan(PlannerInfo *root, LockRowsPath *best_path,
									  int flags);
static ModifyTable *create_modifytable_plan(PlannerInfo *root, ModifyTablePath *best_path);
static Limit *create_limit_plan(PlannerInfo *root, LimitPath *best_path,
								int flags);
static SeqScan *create_seqscan_plan(PlannerInfo *root, Path *best_path,
									List *tlist, List *scan_clauses);
static SampleScan *create_samplescan_plan(PlannerInfo *root, Path *best_path,
										  List *tlist, List *scan_clauses);
static Scan *create_indexscan_plan(PlannerInfo *root, IndexPath *best_path,
								   List *tlist, List *scan_clauses, bool indexonly);
static BitmapHeapScan *create_bitmap_scan_plan(PlannerInfo *root,
											   BitmapHeapPath *best_path,
											   List *tlist, List *scan_clauses);
static Plan *create_bitmap_subplan(PlannerInfo *root, Path *bitmapqual,
								   List **qual, List **indexqual, List **indexECs);
static void bitmap_subplan_mark_shared(Plan *plan);
static TidScan *create_tidscan_plan(PlannerInfo *root, TidPath *best_path,
									List *tlist, List *scan_clauses);
static TidRangeScan *create_tidrangescan_plan(PlannerInfo *root,
											  TidRangePath *best_path,
											  List *tlist,
											  List *scan_clauses);
static SubqueryScan *create_subqueryscan_plan(PlannerInfo *root,
											  SubqueryScanPath *best_path,
											  List *tlist, List *scan_clauses);
static FunctionScan *create_functionscan_plan(PlannerInfo *root, Path *best_path,
											  List *tlist, List *scan_clauses);
static ValuesScan *create_valuesscan_plan(PlannerInfo *root, Path *best_path,
										  List *tlist, List *scan_clauses);
static TableFuncScan *create_tablefuncscan_plan(PlannerInfo *root, Path *best_path,
												List *tlist, List *scan_clauses);
static CteScan *create_ctescan_plan(PlannerInfo *root, Path *best_path,
									List *tlist, List *scan_clauses);
static NamedTuplestoreScan *create_namedtuplestorescan_plan(PlannerInfo *root,
															Path *best_path, List *tlist, List *scan_clauses);
static Result *create_resultscan_plan(PlannerInfo *root, Path *best_path,
									  List *tlist, List *scan_clauses);
static WorkTableScan *create_worktablescan_plan(PlannerInfo *root, Path *best_path,
												List *tlist, List *scan_clauses);
static ForeignScan *create_foreignscan_plan(PlannerInfo *root, ForeignPath *best_path,
											List *tlist, List *scan_clauses);
static CustomScan *create_customscan_plan(PlannerInfo *root,
										  CustomPath *best_path,
										  List *tlist, List *scan_clauses);
static NestLoop *create_nestloop_plan(PlannerInfo *root, NestPath *best_path);
static MergeJoin *create_mergejoin_plan(PlannerInfo *root, MergePath *best_path);
static HashJoin *create_hashjoin_plan(PlannerInfo *root, HashPath *best_path);
static Node *replace_nestloop_params(PlannerInfo *root, Node *expr);
static Node *replace_nestloop_params_mutator(Node *node, PlannerInfo *root);
static void fix_indexqual_references(PlannerInfo *root, IndexPath *index_path,
									 List **stripped_indexquals_p,
									 List **fixed_indexquals_p);
static List *fix_indexorderby_references(PlannerInfo *root, IndexPath *index_path);
static Node *fix_indexqual_clause(PlannerInfo *root,
								  IndexOptInfo *index, int indexcol,
								  Node *clause, List *indexcolnos);
static Node *fix_indexqual_operand(Node *node, IndexOptInfo *index, int indexcol);
static List *get_switched_clauses(List *clauses, Relids outerrelids);
static List *order_qual_clauses(PlannerInfo *root, List *clauses);
static void copy_generic_path_info(Plan *dest, Path *src);
static void copy_plan_costsize(Plan *dest, Plan *src);
static void label_sort_with_costsize(PlannerInfo *root, Sort *plan,
									 double limit_tuples);
static SeqScan *make_seqscan(List *qptlist, List *qpqual, Index scanrelid);
static SampleScan *make_samplescan(List *qptlist, List *qpqual, Index scanrelid,
								   TableSampleClause *tsc);
static IndexScan *make_indexscan(List *qptlist, List *qpqual, Index scanrelid,
								 Oid indexid, List *indexqual, List *indexqualorig,
								 List *indexorderby, List *indexorderbyorig,
								 List *indexorderbyops,
								 ScanDirection indexscandir);
static IndexOnlyScan *make_indexonlyscan(List *qptlist, List *qpqual,
										 Index scanrelid, Oid indexid,
										 List *indexqual, List *recheckqual,
										 List *indexorderby,
										 List *indextlist,
										 ScanDirection indexscandir);
static BitmapIndexScan *make_bitmap_indexscan(Index scanrelid, Oid indexid,
											  List *indexqual,
											  List *indexqualorig);
static BitmapHeapScan *make_bitmap_heapscan(List *qptlist,
											List *qpqual,
											Plan *lefttree,
											List *bitmapqualorig,
											Index scanrelid);
static TidScan *make_tidscan(List *qptlist, List *qpqual, Index scanrelid,
							 List *tidquals);
static TidRangeScan *make_tidrangescan(List *qptlist, List *qpqual,
									   Index scanrelid, List *tidrangequals);
static SubqueryScan *make_subqueryscan(List *qptlist,
									   List *qpqual,
									   Index scanrelid,
									   Plan *subplan);
static FunctionScan *make_functionscan(List *qptlist, List *qpqual,
									   Index scanrelid, List *functions, bool funcordinality);
static ValuesScan *make_valuesscan(List *qptlist, List *qpqual,
								   Index scanrelid, List *values_lists);
static TableFuncScan *make_tablefuncscan(List *qptlist, List *qpqual,
										 Index scanrelid, TableFunc *tablefunc);
static CteScan *make_ctescan(List *qptlist, List *qpqual,
							 Index scanrelid, int ctePlanId, int cteParam);
static NamedTuplestoreScan *make_namedtuplestorescan(List *qptlist, List *qpqual,
													 Index scanrelid, char *enrname);
static WorkTableScan *make_worktablescan(List *qptlist, List *qpqual,
										 Index scanrelid, int wtParam);
static RecursiveUnion *make_recursive_union(List *tlist,
											Plan *lefttree,
											Plan *righttree,
											int wtParam,
											List *distinctList,
											long numGroups);
static BitmapAnd *make_bitmap_and(List *bitmapplans);
static BitmapOr *make_bitmap_or(List *bitmapplans);
static NestLoop *make_nestloop(List *tlist,
							   List *joinclauses, List *otherclauses, List *nestParams,
							   Plan *lefttree, Plan *righttree,
							   JoinType jointype, bool inner_unique);
static HashJoin *make_hashjoin(List *tlist,
							   List *joinclauses, List *otherclauses,
							   List *hashclauses,
							   List *hashoperators, List *hashcollations,
							   List *hashkeys,
							   Plan *lefttree, Plan *righttree,
							   JoinType jointype, bool inner_unique);
static Hash *make_hash(Plan *lefttree,
					   List *hashkeys,
					   Oid skewTable,
					   AttrNumber skewColumn,
					   bool skewInherit);
static MergeJoin *make_mergejoin(List *tlist,
								 List *joinclauses, List *otherclauses,
								 List *mergeclauses,
								 Oid *mergefamilies,
								 Oid *mergecollations,
								 int *mergestrategies,
								 bool *mergenullsfirst,
								 Plan *lefttree, Plan *righttree,
								 JoinType jointype, bool inner_unique,
								 bool skip_mark_restore);
static Sort *make_sort(Plan *lefttree, int numCols,
					   AttrNumber *sortColIdx, Oid *sortOperators,
					   Oid *collations, bool *nullsFirst);
static IncrementalSort *make_incrementalsort(Plan *lefttree,
											 int numCols, int nPresortedCols,
											 AttrNumber *sortColIdx, Oid *sortOperators,
											 Oid *collations, bool *nullsFirst);
static Plan *prepare_sort_from_pathkeys(Plan *lefttree, List *pathkeys,
										Relids relids,
										const AttrNumber *reqColIdx,
										bool adjust_tlist_in_place,
										int *p_numsortkeys,
										AttrNumber **p_sortColIdx,
										Oid **p_sortOperators,
										Oid **p_collations,
										bool **p_nullsFirst);
static Sort *make_sort_from_pathkeys(Plan *lefttree, List *pathkeys,
									 Relids relids);
static IncrementalSort *make_incrementalsort_from_pathkeys(Plan *lefttree,
														   List *pathkeys, Relids relids, int nPresortedCols);
static Sort *make_sort_from_groupcols(List *groupcls,
									  AttrNumber *grpColIdx,
									  Plan *lefttree);
static Material *make_material(Plan *lefttree);
static Memoize *make_memoize(Plan *lefttree, Oid *hashoperators,
							 Oid *collations, List *param_exprs,
							 bool singlerow, bool binary_mode,
							 uint32 est_entries, Bitmapset *keyparamids);
static WindowAgg *make_windowagg(List *tlist, Index winref,
								 int partNumCols, AttrNumber *partColIdx, Oid *partOperators, Oid *partCollations,
								 int ordNumCols, AttrNumber *ordColIdx, Oid *ordOperators, Oid *ordCollations,
								 int frameOptions, Node *startOffset, Node *endOffset,
								 Oid startInRangeFunc, Oid endInRangeFunc,
								 Oid inRangeColl, bool inRangeAsc, bool inRangeNullsFirst,
								 List *runCondition, List *qual, bool topWindow,
								 Plan *lefttree);
static Group *make_group(List *tlist, List *qual, int numGroupCols,
						 AttrNumber *grpColIdx, Oid *grpOperators, Oid *grpCollations,
						 Plan *lefttree);
static Unique *make_unique_from_sortclauses(Plan *lefttree, List *distinctList);
static Unique *make_unique_from_pathkeys(Plan *lefttree,
										 List *pathkeys, int numCols);
static Gather *make_gather(List *qptlist, List *qpqual,
						   int nworkers, int rescan_param, bool single_copy, Plan *subplan);
static SetOp *make_setop(SetOpCmd cmd, SetOpStrategy strategy, Plan *lefttree,
						 List *distinctList, AttrNumber flagColIdx, int firstFlag,
						 long numGroups);
static LockRows *make_lockrows(Plan *lefttree, List *rowMarks, int epqParam);
static Result *make_result(List *tlist, Node *resconstantqual, Plan *subplan);
static ProjectSet *make_project_set(List *tlist, Plan *subplan);
static ModifyTable *make_modifytable(PlannerInfo *root, Plan *subplan,
									 CmdType operation, bool canSetTag,
									 Index nominalRelation, Index rootRelation,
									 bool partColsUpdated,
									 List *resultRelations,
									 List *updateColnosLists,
									 List *withCheckOptionLists, List *returningLists,
									 List *rowMarks, OnConflictExpr *onconflict,
									 List *mergeActionLists, int epqParam);
static GatherMerge *create_gather_merge_plan(PlannerInfo *root,
											 GatherMergePath *best_path);


/*
 * create_plan
 *	  从节点 'best_path' 开始，递归处理
 *	  想要的路径节点树，为查询建立访问计划。对
 *	  找到的每个路径节点，我们建立一个对应的计划节点，含有
 *	  适当的 id、目标列表和限定信息。
 *
 *	  计划树里的 tlist 和限定条件仍是规划器格式，
 *	  也就是说 Var 仍对应解析器的编号。这以后会由
 *	  setrefs.c 修正。
 *
 *	  best_path 是最佳访问路径
 *
 *	  返回一棵 Plan 树。
 */
Plan *
create_plan(PlannerInfo *root, Path *best_path)
{
	Plan	   *plan;

	/* plan_params 不应当在当前查询层次使用 */
	Assert(root->plan_params == NIL);

	/* 在 PlannerInfo 里初始化本模块的工作区 */
	root->curOuterRels = NULL;
	root->curOuterParams = NIL;

	/* 递归处理路径树，要求得到正确的 tlist 结果 */
	plan = create_plan_recurse(root, best_path, CP_EXACT_TLIST);

	/*
	 * 确保最顶层计划节点的目标列表露出原来的
	 * 列名和其他装饰信息。规划器内部生成的目标列表
	 * 不费心做这些，但执行时见到的
	 * 顶层 tlist 必须有这些。不过 ModifyTable 计划
	 * 节点的 tlist 与查询树目标列表并不匹配。
	 */
	if (!IsA(plan, ModifyTable))
		apply_tlist_labeling(plan->targetlist, root->processed_tlist);

	/*
	 * 把本查询层次建立的任何 initPlan 接到最顶层计划
	 * 节点。（原则上 initplan 可以放在引用它们的地方或其上的任何计划节点，
	 * 但似乎没有理由放得比
	 * 该查询层次的最顶层节点更低。另外，试图改这个之前，见
	 * SS_finalize_plan 的注释。）
	 */
	SS_attach_initplans(root, plan);

	/* 检查是否已把全部 NestLoopParams 成功分派给计划节点 */
	if (root->curOuterParams != NIL)
		elog(ERROR, "failed to assign all NestLoopParams to plan nodes");

	/*
	 * 重置 plan_params，确保用于嵌套循环参数的 param ID 以后
	 * 不会被再次使用
	 */
	root->plan_params = NIL;

	return plan;
}

/*
 * create_plan_recurse
 *	  create_plan() 的递归主体。
 */
static Plan *
create_plan_recurse(PlannerInfo *root, Path *best_path, int flags)
{
	Plan	   *plan;

	/* 防止计划过于复杂导致栈溢出 */
	check_stack_depth();

	switch (best_path->pathtype)
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
		case T_NamedTuplestoreScan:
		case T_ForeignScan:
		case T_CustomScan:
			plan = create_scan_plan(root, best_path, flags);
			break;
		case T_HashJoin:
		case T_MergeJoin:
		case T_NestLoop:
			plan = create_join_plan(root,
									(JoinPath *) best_path);
			break;
		case T_Append:
			plan = create_append_plan(root,
									  (AppendPath *) best_path,
									  flags);
			break;
		case T_MergeAppend:
			plan = create_merge_append_plan(root,
											(MergeAppendPath *) best_path,
											flags);
			break;
		case T_Result:
			if (IsA(best_path, ProjectionPath))
			{
				plan = create_projection_plan(root,
											  (ProjectionPath *) best_path,
											  flags);
			}
			else if (IsA(best_path, MinMaxAggPath))
			{
				plan = (Plan *) create_minmaxagg_plan(root,
													  (MinMaxAggPath *) best_path);
			}
			else if (IsA(best_path, GroupResultPath))
			{
				plan = (Plan *) create_group_result_plan(root,
														 (GroupResultPath *) best_path);
			}
			else
			{
				/* 简单的 RTE_RESULT 基关系 */
				Assert(IsA(best_path, Path));
				plan = create_scan_plan(root, best_path, flags);
			}
			break;
		case T_ProjectSet:
			plan = (Plan *) create_project_set_plan(root,
													(ProjectSetPath *) best_path);
			break;
		case T_Material:
			plan = (Plan *) create_material_plan(root,
												 (MaterialPath *) best_path,
												 flags);
			break;
		case T_Memoize:
			plan = (Plan *) create_memoize_plan(root,
												(MemoizePath *) best_path,
												flags);
			break;
		case T_Unique:
			if (IsA(best_path, UpperUniquePath))
			{
				plan = (Plan *) create_upper_unique_plan(root,
														 (UpperUniquePath *) best_path,
														 flags);
			}
			else
			{
				Assert(IsA(best_path, UniquePath));
				plan = create_unique_plan(root,
										  (UniquePath *) best_path,
										  flags);
			}
			break;
		case T_Gather:
			plan = (Plan *) create_gather_plan(root,
											   (GatherPath *) best_path);
			break;
		case T_Sort:
			plan = (Plan *) create_sort_plan(root,
											 (SortPath *) best_path,
											 flags);
			break;
		case T_IncrementalSort:
			plan = (Plan *) create_incrementalsort_plan(root,
														(IncrementalSortPath *) best_path,
														flags);
			break;
		case T_Group:
			plan = (Plan *) create_group_plan(root,
											  (GroupPath *) best_path);
			break;
		case T_Agg:
			if (IsA(best_path, GroupingSetsPath))
				plan = create_groupingsets_plan(root,
												(GroupingSetsPath *) best_path);
			else
			{
				Assert(IsA(best_path, AggPath));
				plan = (Plan *) create_agg_plan(root,
												(AggPath *) best_path);
			}
			break;
		case T_WindowAgg:
			plan = (Plan *) create_windowagg_plan(root,
												  (WindowAggPath *) best_path);
			break;
		case T_SetOp:
			plan = (Plan *) create_setop_plan(root,
											  (SetOpPath *) best_path,
											  flags);
			break;
		case T_RecursiveUnion:
			plan = (Plan *) create_recursiveunion_plan(root,
													   (RecursiveUnionPath *) best_path);
			break;
		case T_LockRows:
			plan = (Plan *) create_lockrows_plan(root,
												 (LockRowsPath *) best_path,
												 flags);
			break;
		case T_ModifyTable:
			plan = (Plan *) create_modifytable_plan(root,
													(ModifyTablePath *) best_path);
			break;
		case T_Limit:
			plan = (Plan *) create_limit_plan(root,
											  (LimitPath *) best_path,
											  flags);
			break;
		case T_GatherMerge:
			plan = (Plan *) create_gather_merge_plan(root,
													 (GatherMergePath *) best_path);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->pathtype);
			plan = NULL;		/* 消除编译器告警 */
			break;
	}

	return plan;
}

/*
 * create_scan_plan
 *	 为 'best_path' 的父关系建立扫描计划。
 */
static Plan *
create_scan_plan(PlannerInfo *root, Path *best_path, int flags)
{
	RelOptInfo *rel = best_path->parent;
	List	   *scan_clauses;
	List	   *gating_clauses;
	List	   *tlist;
	Plan	   *plan;

	/*
	 * 从父关系抽出相关的限制子句。执行器
	 * 在扫描期间必须应用全部这些限制，下面会处理的
	 * 伪常量除外。
	 *
	 * 若这是普通索引扫描或仅索引扫描，不必考虑
	 * 由索引谓词蕴含的限制子句，因此用
	 * indrestrictinfo 而不是 baserestrictinfo。注意对
	 * 位图索引扫描不能那么做，因为不一定只有一个索引
	 * 参与；不过没关系，因为 create_bitmap_scan_plan() 反正能
	 * 通过谓词证明去掉这类子句。
	 */
	switch (best_path->pathtype)
	{
		case T_IndexScan:
		case T_IndexOnlyScan:
			scan_clauses = castNode(IndexPath, best_path)->indexinfo->indrestrictinfo;
			break;
		default:
			scan_clauses = rel->baserestrictinfo;
			break;
	}

	/*
	 * 若这是参数化扫描，还需要强制来自外层关系的全部连接
	 * 子句。
	 *
	 * 为多留个心，不要修改存着的 baserestrictinfo 列表。
	 */
	if (best_path->param_info)
		scan_clauses = list_concat_copy(scan_clauses,
										best_path->param_info->ppi_clauses);

	/*
	 * 检测是否有任何伪常量限定条件要处理。然后，若
	 * 需要一个门控 Result 节点，它将能够做投影，因此对
	 * 子节点的 tlist 没有要求。
	 */
	gating_clauses = get_gating_quals(root, scan_clauses);
	if (gating_clauses)
		flags = 0;

	/*
	 * 对表扫描，我们宁可生成一份按顺序包含全部 Var 的
	 * tlist，而不是用关系目标列表（那只含
	 * 查询实际需要的那些 Var）。这样执行器在可能时就能
	 * 优化掉表元组的投影。
	 *
	 * 但若调用方反正会忽略我们的 tlist，就完全
	 * 不必生成。这里用精确相等测试，因此
	 * 只有当 CP_IGNORE_TLIST 是唯一设置的标志时才适用。
	 */
	if (flags == CP_IGNORE_TLIST)
	{
		tlist = NULL;
	}
	else if (use_physical_tlist(root, best_path, flags))
	{
		if (best_path->pathtype == T_IndexOnlyScan)
		{
			/* 对仅索引扫描，首选 tlist 是索引的 tlist */
			tlist = copyObject(((IndexPath *) best_path)->indexinfo->indextlist);

			/*
			 * 若有请求，把 sortgroupref 数据转到替换用的 tlist
			 * （use_physical_tlist 已检查过这样可行）。
			 */
			if (flags & CP_LABEL_TLIST)
				apply_pathtarget_labeling_to_tlist(tlist, best_path->pathtarget);
		}
		else
		{
			tlist = build_physical_tlist(root, rel);
			if (tlist == NIL)
			{
				/* 因被删除的列而失败，因此用常规方法 */
				tlist = build_path_tlist(root, best_path);
			}
			else
			{
				/* 同上，把 sortgroupref 数据转到替换用的 tlist */
				if (flags & CP_LABEL_TLIST)
					apply_pathtarget_labeling_to_tlist(tlist, best_path->pathtarget);
			}
		}
	}
	else
	{
		tlist = build_path_tlist(root, best_path);
	}

	switch (best_path->pathtype)
	{
		case T_SeqScan:
			plan = (Plan *) create_seqscan_plan(root,
												best_path,
												tlist,
												scan_clauses);
			break;

		case T_SampleScan:
			plan = (Plan *) create_samplescan_plan(root,
												   best_path,
												   tlist,
												   scan_clauses);
			break;

		case T_IndexScan:
			plan = (Plan *) create_indexscan_plan(root,
												  (IndexPath *) best_path,
												  tlist,
												  scan_clauses,
												  false);
			break;

		case T_IndexOnlyScan:
			plan = (Plan *) create_indexscan_plan(root,
												  (IndexPath *) best_path,
												  tlist,
												  scan_clauses,
												  true);
			break;

		case T_BitmapHeapScan:
			plan = (Plan *) create_bitmap_scan_plan(root,
													(BitmapHeapPath *) best_path,
													tlist,
													scan_clauses);
			break;

		case T_TidScan:
			plan = (Plan *) create_tidscan_plan(root,
												(TidPath *) best_path,
												tlist,
												scan_clauses);
			break;

		case T_TidRangeScan:
			plan = (Plan *) create_tidrangescan_plan(root,
													 (TidRangePath *) best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_SubqueryScan:
			plan = (Plan *) create_subqueryscan_plan(root,
													 (SubqueryScanPath *) best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_FunctionScan:
			plan = (Plan *) create_functionscan_plan(root,
													 best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_TableFuncScan:
			plan = (Plan *) create_tablefuncscan_plan(root,
													  best_path,
													  tlist,
													  scan_clauses);
			break;

		case T_ValuesScan:
			plan = (Plan *) create_valuesscan_plan(root,
												   best_path,
												   tlist,
												   scan_clauses);
			break;

		case T_CteScan:
			plan = (Plan *) create_ctescan_plan(root,
												best_path,
												tlist,
												scan_clauses);
			break;

		case T_NamedTuplestoreScan:
			plan = (Plan *) create_namedtuplestorescan_plan(root,
															best_path,
															tlist,
															scan_clauses);
			break;

		case T_Result:
			plan = (Plan *) create_resultscan_plan(root,
												   best_path,
												   tlist,
												   scan_clauses);
			break;

		case T_WorkTableScan:
			plan = (Plan *) create_worktablescan_plan(root,
													  best_path,
													  tlist,
													  scan_clauses);
			break;

		case T_ForeignScan:
			plan = (Plan *) create_foreignscan_plan(root,
													(ForeignPath *) best_path,
													tlist,
													scan_clauses);
			break;

		case T_CustomScan:
			plan = (Plan *) create_customscan_plan(root,
												   (CustomPath *) best_path,
												   tlist,
												   scan_clauses);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->pathtype);
			plan = NULL;		/* 消除编译器告警 */
			break;
	}

	/*
	 * 若本节点挂有任何伪常量子句，就插入一个
	 * 把门控伪常量当作一次性
	 * 限定条件来求值的 Result 节点。
	 */
	if (gating_clauses)
		plan = create_gating_plan(root, best_path, plan, gating_clauses);

	return plan;
}

/*
 * 为该 Path 的输出建立目标列表（即一份 TargetEntry 列表）。
 *
 * 这几乎就是 make_tlist_from_pathtarget()，但还必须
 * 处理嵌套循环参数的替换。
 */
static List *
build_path_tlist(PlannerInfo *root, Path *path)
{
	List	   *tlist = NIL;
	Index	   *sortgrouprefs = path->pathtarget->sortgrouprefs;
	int			resno = 1;
	ListCell   *v;

	foreach(v, path->pathtarget->exprs)
	{
		Node	   *node = (Node *) lfirst(v);
		TargetEntry *tle;

		/*
		 * 若是参数化路径，tlist 里也许有 LATERAL 引用，
		 * 需要换成 Param。不必
		 * 重建 TargetEntry 节点，因此对每个列表项
		 * 分别应用。
		 */
		if (path->param_info)
			node = replace_nestloop_params(root, node);

		tle = makeTargetEntry((Expr *) node,
							  resno,
							  NULL,
							  false);
		if (sortgrouprefs)
			tle->ressortgroupref = sortgrouprefs[resno - 1];

		tlist = lappend(tlist, tle);
		resno++;
	}
	return tlist;
}

/*
 * use_physical_tlist
 *		决定是否使用与关系结构匹配的 tlist，
 *		而不是只用实际被引用的那些 Var。
 */
static bool
use_physical_tlist(PlannerInfo *root, Path *path, int flags)
{
	RelOptInfo *rel = path->parent;
	int			i;
	ListCell   *lc;

	/*
	 * 若要求精确 tlist 或窄 tlist，就别想了。
	 */
	if (flags & (CP_EXACT_TLIST | CP_SMALL_TLIST))
		return false;

	/*
	 * 对真正的关系扫描、子查询扫描、函数扫描、
	 * tablefunc 扫描、values 扫描和 CTE 扫描可以这么做（但对连接等不行）。
	 */
	if (rel->rtekind != RTE_RELATION &&
		rel->rtekind != RTE_SUBQUERY &&
		rel->rtekind != RTE_FUNCTION &&
		rel->rtekind != RTE_TABLEFUNC &&
		rel->rtekind != RTE_VALUES &&
		rel->rtekind != RTE_CTE)
		return false;

	/*
	 * 继承情形也不能这么做（主要因为 Append
	 * 不做投影；既然
	 * create_append_plan 已指示其子项返回精确 tlist，这个测试现在也许多余）。
	 */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;

	/*
	 * 对 CustomPath 也不要这么做；我们是从简单物理元组里抽
	 * 列这一前提，对它们多半不成立。
	 * （当真有意义时，自定义路径的创建者可以把路径的
	 * pathtarget 设成那样。）
	 */
	if (IsA(path, CustomPath))
		return false;

	/*
	 * 若位图扫描的 tlist 为空，就保持原样。这也许让
	 * 执行器跳过堆页读取，而且无论如何，改用
	 * 物理 tlist 的好处也很小。
	 */
	if (IsA(path, BitmapHeapPath) &&
		path->pathtarget->exprs == NIL)
		return false;

	/*
	 * 若请求了任何系统列或整行 Var，就不能这么做。
	 * （这也许能修，但我想在 setrefs.c 里要做一些
	 * 脆弱的假定。）
	 */
	for (i = rel->min_attr; i <= 0; i++)
	{
		if (!bms_is_empty(rel->attr_needed[i - rel->min_attr]))
			return false;
	}

	/*
	 * 若该关系必须发出任何占位表达式，
	 * 也不能这么做。
	 */
	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);

		if (bms_nonempty_difference(phinfo->ph_needed, rel->relids) &&
			bms_is_subset(phinfo->ph_eval_at, rel->relids))
			return false;
	}

	/*
	 * 对仅索引扫描，"物理 tlist" 就是索引的 indextlist。
	 * 只有当该索引的全部列
	 * 都可返回时，才能不经投影就返回它。
	 */
	if (path->pathtype == T_IndexOnlyScan)
	{
		IndexOptInfo *indexinfo = ((IndexPath *) path)->indexinfo;

		for (i = 0; i < indexinfo->ncolumns; i++)
		{
			if (!indexinfo->canreturn[i])
				return false;
		}
	}

	/*
	 * 另外，若指定了 CP_LABEL_TLIST，且路径被要求
	 * 发出任何不是简单 Var 的排序/分组列，也不能这么做。（若它们是
	 * 简单 Var，就应当出现在物理 tlist 里，
	 * apply_pathtarget_labeling_to_tlist 会负责再给它们
	 * 标上。）还要检查没有两列排序/分组
	 * 是同一个 Var，否则物理 tlist 的那个元素会需要
	 * 互相冲突的 ressortgroupref 标注。
	 */
	if ((flags & CP_LABEL_TLIST) && path->pathtarget->sortgrouprefs)
	{
		Bitmapset  *sortgroupatts = NULL;

		i = 0;
		foreach(lc, path->pathtarget->exprs)
		{
			Expr	   *expr = (Expr *) lfirst(lc);

			if (path->pathtarget->sortgrouprefs[i])
			{
				if (expr && IsA(expr, Var))
				{
					int			attno = ((Var *) expr)->varattno;

					attno -= FirstLowInvalidHeapAttributeNumber;
					if (bms_is_member(attno, sortgroupatts))
						return false;
					sortgroupatts = bms_add_member(sortgroupatts, attno);
				}
				else
					return false;
			}
			i++;
		}
	}

	return true;
}

/*
 * get_gating_quals
 *	  看节点的限定条件列表里有没有伪常量限定条件
 *
 * 若节点的限定条件列表含有任何伪常量限定条件，
 * 就只返回那些限定条件。
 */
static List *
get_gating_quals(PlannerInfo *root, List *quals)
{
	/* 若已知没有伪常量，就不必看 */
	if (!root->hasPseudoConstantQuals)
		return NIL;

	/* 仍为 RestrictInfo 形式时，排成可取的执行顺序 */
	quals = order_qual_clauses(root, quals);

	/* 从 RestrictInfo 列表里抽出任何伪常量限定条件 */
	return extract_actual_clauses(quals, true);
}

/*
 * create_gating_plan
 *	  处理伪常量限定子句
 *
 * 在已经建成的计划之上加一个门控 Result 节点。
 */
static Plan *
create_gating_plan(PlannerInfo *root, Path *path, Plan *plan,
				   List *gating_quals)
{
	Plan	   *gplan;
	Plan	   *splan;

	Assert(gating_quals);

	/*
	 * 也许已经有一个平凡的 Result 计划。再往上叠一个 Result
	 * 很傻，因此若适用，就丢掉输入计划。
	 * （假定它的目标列表没什么意思；它应当要么
	 * 与 build_path_tlist 的结果相同，要么是简化版。）
	 */
	splan = plan;
	if (IsA(plan, Result))
	{
		Result	   *rplan = (Result *) plan;

		if (rplan->plan.lefttree == NULL &&
			rplan->resconstantqual == NULL)
			splan = NULL;
	}

	/*
	 * 既然反正需要一个 Result 节点，就总是返回路径所要求的
	 * tlist；即使父节点没要求
	 * CP_EXACT_TLIST，这也从不是错误选择。
	 */
	gplan = (Plan *) make_result(build_path_tlist(root, path),
								 (Node *) gating_quals,
								 splan);

	/*
	 * 注意做门控时我们不改代价或大小估计。
	 * 限定条件求值的代价已经算进子计划的代价。
	 * 大小保持不动，等于假定门控限定条件会
	 * 成功，这对规划上层查询是保守估计。
	 * 我们当然不想假定输出大小为零（除非
	 * 门控限定条件实际上是常量 FALSE，那种情况在
	 * clausesel.c 里处理）。在两种情形之间插值很傻，因为它
	 * 并不反映运行时真正会发生什么，而且
	 * 多数情况下我们对门控限定条件为真的概率
	 * 只有非常糟糕的估计。
	 */
	copy_plan_costsize(gplan, plan);

	/* 门控限定条件可能不安全，因此最好用 Path 的安全标志 */
	gplan->parallel_safe = path->parallel_safe;

	return gplan;
}

/*
 * create_join_plan
 *	  为 'best_path' 建立连接计划，并（递归地）为其
 *	  内侧和外侧路径建立计划。
 */
static Plan *
create_join_plan(PlannerInfo *root, JoinPath *best_path)
{
	Plan	   *plan;
	List	   *gating_clauses;

	switch (best_path->path.pathtype)
	{
		case T_MergeJoin:
			plan = (Plan *) create_mergejoin_plan(root,
												  (MergePath *) best_path);
			break;
		case T_HashJoin:
			plan = (Plan *) create_hashjoin_plan(root,
												 (HashPath *) best_path);
			break;
		case T_NestLoop:
			plan = (Plan *) create_nestloop_plan(root,
												 (NestPath *) best_path);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->path.pathtype);
			plan = NULL;		/* 消除编译器告警 */
			break;
	}

	/*
	 * 若本节点挂有任何伪常量子句，就插入一个
	 * 把门控伪常量当作一次性
	 * 限定条件来求值的 Result 节点。
	 */
	gating_clauses = get_gating_quals(root, best_path->joinrestrictinfo);
	if (gating_clauses)
		plan = create_gating_plan(root, (Path *) best_path, plan,
								  gating_clauses);

#ifdef NOT_USED

	/*
	 * 昂贵函数上拉也许已经把本地谓词拉进
	 * 本路径节点。把它们放进计划节点的 qpqual。JMH，
	 * 6/15/92
	 */
	if (get_loc_restrictinfo(best_path) != NIL)
		set_qpqual((Plan) plan,
				   list_concat(get_qpqual((Plan) plan),
							   get_actual_clauses(get_loc_restrictinfo(best_path))));
#endif

	return plan;
}

/*
 * mark_async_capable_plan
 *		检查由 Path 节点建立的 Plan 节点是否具备异步能力，
 *		若是，就把该 Plan 节点标成如此并返回真，否则
 *		返回假。
 */
static bool
mark_async_capable_plan(Plan *plan, Path *path)
{
	switch (nodeTag(path))
	{
		case T_SubqueryScanPath:
			{
				SubqueryScan *scan_plan = (SubqueryScan *) plan;

				/*
				 * 若生成的计划节点含有门控 Result 节点，
				 * 就不能异步执行它。
				 */
				if (IsA(plan, Result))
					return false;

				/*
				 * 若异步能力计划节点之上的 SubqueryScan 节点
				 * 可删除，就把它视为具备异步能力。
				 */
				if (trivial_subqueryscan(scan_plan) &&
					mark_async_capable_plan(scan_plan->subplan,
											((SubqueryScanPath *) path)->subpath))
					break;
				return false;
			}
		case T_ForeignPath:
			{
				FdwRoutine *fdwroutine = path->parent->fdwroutine;

				/*
				 * 若生成的计划节点含有门控 Result 节点，
				 * 就不能异步执行它。
				 */
				if (IsA(plan, Result))
					return false;

				Assert(fdwroutine != NULL);
				if (fdwroutine->IsForeignPathAsyncCapable != NULL &&
					fdwroutine->IsForeignPathAsyncCapable((ForeignPath *) path))
					break;
				return false;
			}
		case T_ProjectionPath:

			/*
			 * 若生成的计划节点含有用于
			 * 投影的 Result 节点，就不能异步执行它。
			 */
			if (IsA(plan, Result))
				return false;

			/*
			 * create_projection_plan() 会上拉子计划，因此
			 * 用子路径来检查能力。
			 */
			if (mark_async_capable_plan(plan,
										((ProjectionPath *) path)->subpath))
				return true;
			return false;
		default:
			return false;
	}

	plan->async_capable = true;

	return true;
}

/*
 * create_append_plan
 *	  为 'best_path' 建立 Append 计划，并（递归地）为其
 *	  子路径建立计划。
 *
 *	  返回一个 Plan 节点。
 */
static Plan *
create_append_plan(PlannerInfo *root, AppendPath *best_path, int flags)
{
	Append	   *plan;
	List	   *tlist = build_path_tlist(root, &best_path->path);
	int			orig_tlist_length = list_length(tlist);
	bool		tlist_was_changed = false;
	List	   *pathkeys = best_path->path.pathkeys;
	List	   *subplans = NIL;
	ListCell   *subpaths;
	int			nasyncplans = 0;
	RelOptInfo *rel = best_path->path.parent;
	PartitionPruneInfo *partpruneinfo = NULL;
	int			nodenumsortkeys = 0;
	AttrNumber *nodeSortColIdx = NULL;
	Oid		   *nodeSortOperators = NULL;
	Oid		   *nodeCollations = NULL;
	bool	   *nodeNullsFirst = NULL;
	bool		consider_async = false;

	/*
	 * 若每个子项都被约束排除证明为空，子路径列表可能为空。
	 * 那种情况下生成一个不返回
	 * 任何行的哑计划。
	 *
	 * 注意，在某些根本没有追加构造、但我们知道
	 * 该关系为空的情形里，也会生成没有成员的 AppendPath
	 * （见 set_dummy_rel_pathlist 和 mark_dummy_rel）。
	 */
	if (best_path->subpaths == NIL)
	{
		/* 生成带常量 FALSE 门控限定条件的 Result 计划 */
		Plan	   *plan;

		plan = (Plan *) make_result(tlist,
									(Node *) list_make1(makeBoolConst(false,
																	  false)),
									NULL);

		copy_generic_path_info(plan, (Path *) best_path);

		return plan;
	}

	/*
	 * 否则建立 Append 计划。注意若只有一个子项，
	 * 这个 Append 相当无用；但我们等到 setrefs.c 再去掉
	 * 它。在这里做不行，因为子扫描
	 * 计划的 varno 对不上它将被要求发出的父关系 Var。
	 *
	 * 我们没有把 Append 节点的实际建立拆到
	 * 单独的 make_xxx 函数里。这是因为想在对各个
	 * 子计划做 prepare_sort_from_pathkeys 之前，先对它做，
	 * 以便更容易交叉检查排序信息。
	 */
	plan = makeNode(Append);
	plan->plan.targetlist = tlist;
	plan->plan.qual = NIL;
	plan->plan.lefttree = NULL;
	plan->plan.righttree = NULL;
	plan->apprelids = rel->relids;

	if (pathkeys != NIL)
	{
		/*
		 * 计算排序列信息，并按需要调整 Append 的 tlist。
		 * 因为传入 adjust_tlist_in_place = true，可以忽略
		 * 函数结果；它必须是同一个计划节点。不过随后
		 * 需要检测是否添加了任何 tlist 项。
		 */
		(void) prepare_sort_from_pathkeys((Plan *) plan, pathkeys,
										  best_path->path.parent->relids,
										  NULL,
										  true,
										  &nodenumsortkeys,
										  &nodeSortColIdx,
										  &nodeSortOperators,
										  &nodeCollations,
										  &nodeNullsFirst);
		tlist_was_changed = (orig_tlist_length != list_length(plan->plan.targetlist));
	}

	/* 若合适，考虑异步追加 */
	consider_async = (enable_async_append && pathkeys == NIL &&
					  !best_path->path.parallel_safe &&
					  list_length(best_path->subpaths) > 1);

	/* 为每个子项建立计划 */
	foreach(subpaths, best_path->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);
		Plan	   *subplan;

		/* 必须坚持全部子项返回相同的 tlist */
		subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

		/*
		 * 对有序 Append，若子计划的顺序
		 * 不够，就必须插入 Sort 节点。
		 */
		if (pathkeys != NIL)
		{
			int			numsortkeys;
			AttrNumber *sortColIdx;
			Oid		   *sortOperators;
			Oid		   *collations;
			bool	   *nullsFirst;

			/*
			 * 计算排序列信息，并按需要调整子计划的 tlist。
			 * 即使对不需要显式排序的子计划，也必须应用 prepare_sort_from_pathkeys，
			 * 以确保它们返回
			 * Append 所预期的相同排序键列。
			 */
			subplan = prepare_sort_from_pathkeys(subplan, pathkeys,
												 subpath->parent->relids,
												 nodeSortColIdx,
												 false,
												 &numsortkeys,
												 &sortColIdx,
												 &sortOperators,
												 &collations,
												 &nullsFirst);

			/*
			 * 检查得到的排序键信息是否相同。我们只
			 * 断言 sortop 匹配，因为那些只取决于
			 * 路径键；但显式检查排序
			 * 列号似乎是个好主意，以确保 tlist 对得上。
			 */
			Assert(numsortkeys == nodenumsortkeys);
			if (memcmp(sortColIdx, nodeSortColIdx,
					   numsortkeys * sizeof(AttrNumber)) != 0)
				elog(ERROR, "Append child's targetlist doesn't match Append");
			Assert(memcmp(sortOperators, nodeSortOperators,
						  numsortkeys * sizeof(Oid)) == 0);
			Assert(memcmp(collations, nodeCollations,
						  numsortkeys * sizeof(Oid)) == 0);
			Assert(memcmp(nullsFirst, nodeNullsFirst,
						  numsortkeys * sizeof(bool)) == 0);

			/* 现在，若子计划顺序不够，就插入 Sort 节点 */
			if (!pathkeys_contained_in(pathkeys, subpath->pathkeys))
			{
				Sort	   *sort = make_sort(subplan, numsortkeys,
											 sortColIdx, sortOperators,
											 collations, nullsFirst);

				label_sort_with_costsize(root, sort, best_path->limit_tuples);
				subplan = (Plan *) sort;
			}
		}

		/* 若需要，检查子计划能否异步执行 */
		if (consider_async && mark_async_capable_plan(subplan, subpath))
		{
			Assert(subplan->async_capable);
			++nasyncplans;
		}

		subplans = lappend(subplans, subplan);
	}

	/*
	 * 若存在任何限定条件，它们也许有助于在执行期间做进一步的分区
	 * 裁剪。收集执行器做
	 * 分区裁剪所需的信息。
	 */
	if (enable_partition_pruning)
	{
		List	   *prunequal;

		prunequal = extract_actual_clauses(rel->baserestrictinfo, false);

		if (best_path->path.param_info)
		{
			List	   *prmquals = best_path->path.param_info->ppi_clauses;

			prmquals = extract_actual_clauses(prmquals, false);
			prmquals = (List *) replace_nestloop_params(root,
														(Node *) prmquals);

			prunequal = list_concat(prunequal, prmquals);
		}

		if (prunequal != NIL)
			partpruneinfo =
				make_partition_pruneinfo(root, rel,
										 best_path->subpaths,
										 prunequal);
	}

	plan->appendplans = subplans;
	plan->nasyncplans = nasyncplans;
	plan->first_partial_plan = best_path->first_partial_path;
	plan->part_prune_info = partpruneinfo;

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	/*
	 * 若 prepare_sort_from_pathkeys 加了排序列，但我们被要求
	 * 产生精确 tlist 或窄 tlist，就应当再去掉
	 * 这些排序列。必须注入一个投影节点来做这件事。
	 */
	if (tlist_was_changed && (flags & (CP_EXACT_TLIST | CP_SMALL_TLIST)))
	{
		tlist = list_copy_head(plan->plan.targetlist, orig_tlist_length);
		return inject_projection_plan((Plan *) plan, tlist,
									  plan->plan.parallel_safe);
	}
	else
		return (Plan *) plan;
}

/*
 * create_merge_append_plan
 *	  为 'best_path' 建立 MergeAppend 计划，并（递归地）为其
 *	  子路径建立计划。
 *
 *	  返回一个 Plan 节点。
 */
static Plan *
create_merge_append_plan(PlannerInfo *root, MergeAppendPath *best_path,
						 int flags)
{
	MergeAppend *node = makeNode(MergeAppend);
	Plan	   *plan = &node->plan;
	List	   *tlist = build_path_tlist(root, &best_path->path);
	int			orig_tlist_length = list_length(tlist);
	bool		tlist_was_changed;
	List	   *pathkeys = best_path->path.pathkeys;
	List	   *subplans = NIL;
	ListCell   *subpaths;
	RelOptInfo *rel = best_path->path.parent;
	PartitionPruneInfo *partpruneinfo = NULL;

	/*
	 * 我们没有把 MergeAppend 节点的实际建立拆到
	 * 单独的 make_xxx 函数里。这是因为想在对各个
	 * 子计划做 prepare_sort_from_pathkeys 之前，先对它做，
	 * 以便更容易交叉检查排序信息。
	 */
	copy_generic_path_info(plan, (Path *) best_path);
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->apprelids = rel->relids;

	/*
	 * 计算排序列信息，并按需要调整 MergeAppend 的 tlist。
	 * 因为传入 adjust_tlist_in_place = true，可以忽略
	 * 函数结果；它必须是同一个计划节点。不过随后需要
	 * 检测是否添加了任何 tlist 项。
	 */
	(void) prepare_sort_from_pathkeys(plan, pathkeys,
									  best_path->path.parent->relids,
									  NULL,
									  true,
									  &node->numCols,
									  &node->sortColIdx,
									  &node->sortOperators,
									  &node->collations,
									  &node->nullsFirst);
	tlist_was_changed = (orig_tlist_length != list_length(plan->targetlist));

	/*
	 * 现在准备子计划。即使对不需要显式排序的子计划，也必须应用 prepare_sort_from_pathkeys，
	 * 以确保它们
	 * 返回 MergeAppend 所预期的相同排序键列。
	 */
	foreach(subpaths, best_path->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);
		Plan	   *subplan;
		int			numsortkeys;
		AttrNumber *sortColIdx;
		Oid		   *sortOperators;
		Oid		   *collations;
		bool	   *nullsFirst;

		/* 建立子计划 */
		/* 必须坚持全部子项返回相同的 tlist */
		subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

		/* 计算排序列信息，并按需要调整子计划的 tlist */
		subplan = prepare_sort_from_pathkeys(subplan, pathkeys,
											 subpath->parent->relids,
											 node->sortColIdx,
											 false,
											 &numsortkeys,
											 &sortColIdx,
											 &sortOperators,
											 &collations,
											 &nullsFirst);

		/*
		 * 检查得到的排序键信息是否相同。我们只断言
		 * sortop 匹配，因为那些只取决于路径键；
		 * 但显式检查排序列号
		 * 似乎是个好主意，以确保 tlist 真的对得上。
		 */
		Assert(numsortkeys == node->numCols);
		if (memcmp(sortColIdx, node->sortColIdx,
				   numsortkeys * sizeof(AttrNumber)) != 0)
			elog(ERROR, "MergeAppend child's targetlist doesn't match MergeAppend");
		Assert(memcmp(sortOperators, node->sortOperators,
					  numsortkeys * sizeof(Oid)) == 0);
		Assert(memcmp(collations, node->collations,
					  numsortkeys * sizeof(Oid)) == 0);
		Assert(memcmp(nullsFirst, node->nullsFirst,
					  numsortkeys * sizeof(bool)) == 0);

		/* 现在，若子计划顺序不够，就插入 Sort 节点 */
		if (!pathkeys_contained_in(pathkeys, subpath->pathkeys))
		{
			Sort	   *sort = make_sort(subplan, numsortkeys,
										 sortColIdx, sortOperators,
										 collations, nullsFirst);

			label_sort_with_costsize(root, sort, best_path->limit_tuples);
			subplan = (Plan *) sort;
		}

		subplans = lappend(subplans, subplan);
	}

	/*
	 * 若存在任何限定条件，它们也许有助于在执行期间做进一步的分区
	 * 裁剪。收集执行器做
	 * 分区裁剪所需的信息。
	 */
	if (enable_partition_pruning)
	{
		List	   *prunequal;

		prunequal = extract_actual_clauses(rel->baserestrictinfo, false);

		/* 目前不生成任何参数化的 MergeAppend 路径 */
		Assert(best_path->path.param_info == NULL);

		if (prunequal != NIL)
			partpruneinfo = make_partition_pruneinfo(root, rel,
													 best_path->subpaths,
													 prunequal);
	}

	node->mergeplans = subplans;
	node->part_prune_info = partpruneinfo;

	/*
	 * 若 prepare_sort_from_pathkeys 加了排序列，但我们被要求
	 * 产生精确 tlist 或窄 tlist，就应当再去掉
	 * 这些排序列。必须注入一个投影节点来做这件事。
	 */
	if (tlist_was_changed && (flags & (CP_EXACT_TLIST | CP_SMALL_TLIST)))
	{
		tlist = list_copy_head(plan->targetlist, orig_tlist_length);
		return inject_projection_plan(plan, tlist, plan->parallel_safe);
	}
	else
		return plan;
}

/*
 * create_group_result_plan
 *	  为 'best_path' 建立 Result 计划。
 *	  这只用于退化的分组情形。
 *
 *	  返回一个 Plan 节点。
 */
static Result *
create_group_result_plan(PlannerInfo *root, GroupResultPath *best_path)
{
	Result	   *plan;
	List	   *tlist;
	List	   *quals;

	tlist = build_path_tlist(root, &best_path->path);

	/* best_path->quals 只是裸子句 */
	quals = order_qual_clauses(root, best_path->quals);

	plan = make_result(tlist, (Node *) quals, NULL);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_project_set_plan
 *	  为 'best_path' 建立 ProjectSet 计划。
 *
 *	  返回一个 Plan 节点。
 */
static ProjectSet *
create_project_set_plan(PlannerInfo *root, ProjectSetPath *best_path)
{
	ProjectSet *plan;
	Plan	   *subplan;
	List	   *tlist;

	/* 既然打算做投影，就不必约束子节点的 tlist */
	subplan = create_plan_recurse(root, best_path->subpath, 0);

	tlist = build_path_tlist(root, &best_path->path);

	plan = make_project_set(tlist, subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_material_plan
 *	  为 'best_path' 建立 Material 计划，并（递归地）为其
 *	  子路径建立计划。
 *
 *	  返回一个 Plan 节点。
 */
static Material *
create_material_plan(PlannerInfo *root, MaterialPath *best_path, int flags)
{
	Material   *plan;
	Plan	   *subplan;

	/*
	 * 不想在物化元组里有任何多余的列，因此要求
	 * 更小的 tlist。否则，由于 Material 不做投影，tlist
	 * 要求会透传。
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_SMALL_TLIST);

	plan = make_material(subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_memoize_plan
 *	  为 'best_path' 建立 Memoize 计划，并（递归地）为其
 *	  子路径建立计划。
 *
 *	  返回一个 Plan 节点。
 */
static Memoize *
create_memoize_plan(PlannerInfo *root, MemoizePath *best_path, int flags)
{
	Memoize    *plan;
	Bitmapset  *keyparamids;
	Plan	   *subplan;
	Oid		   *operators;
	Oid		   *collations;
	List	   *param_exprs = NIL;
	ListCell   *lc;
	ListCell   *lc2;
	int			nkeys;
	int			i;

	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_SMALL_TLIST);

	param_exprs = (List *) replace_nestloop_params(root, (Node *)
												   best_path->param_exprs);

	nkeys = list_length(param_exprs);
	Assert(nkeys > 0);
	operators = palloc(nkeys * sizeof(Oid));
	collations = palloc(nkeys * sizeof(Oid));

	i = 0;
	forboth(lc, param_exprs, lc2, best_path->hash_operators)
	{
		Expr	   *param_expr = (Expr *) lfirst(lc);
		Oid			opno = lfirst_oid(lc2);

		operators[i] = opno;
		collations[i] = exprCollation((Node *) param_expr);
		i++;
	}

	keyparamids = pull_paramids((Expr *) param_exprs);

	plan = make_memoize(subplan, operators, collations, param_exprs,
						best_path->singlerow, best_path->binary_mode,
						best_path->est_entries, keyparamids);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_unique_plan
 *	  为 'best_path' 建立 Unique 计划，并（递归地）为其
 *	  子路径建立计划。
 *
 *	  返回一个 Plan 节点。
 */
static Plan *
create_unique_plan(PlannerInfo *root, UniquePath *best_path, int flags)
{
	Plan	   *plan;
	Plan	   *subplan;
	List	   *in_operators;
	List	   *uniq_exprs;
	List	   *newtlist;
	int			nextresno;
	bool		newitems;
	int			numGroupCols;
	AttrNumber *groupColIdx;
	Oid		   *groupCollations;
	int			groupColPos;
	ListCell   *l;

	/* Unique 不做投影，因此 tlist 要求透传 */
	subplan = create_plan_recurse(root, best_path->subpath, flags);

	/* 若不需要做任何实际的唯一化，就做完了 */
	if (best_path->umethod == UNIQUE_PATH_NOOP)
		return subplan;

	/*
	 * 建成时，子计划有一份 "平坦" tlist，只含这里和
	 * 上层所需的 Var。我们应当做唯一化的值
	 * 也许是这些变量上的表达式。必须把任何
	 * 这类表达式加进子计划的 tlist。
	 *
	 * 若子计划是简单扫描计划，它也许有一份 "物理" tlist。若
	 * 要排序，就应当把它收成常规 tlist，这样
	 * 就不会排序超出需要的数据。对哈希，若不需要添加任何表达式，
	 * tlist 应当保持原样；但若
	 * 确实必须添加表达式，运行时反正需要投影步骤，
	 * 因此不妨去掉不需要的项。因此
	 * newtlist 从 build_path_tlist() 开始，而不是只复制
	 * 子计划的 tlist；除非正在
	 * 排序或必须添加内容，否则不把它装进子计划。
	 */
	in_operators = best_path->in_operators;
	uniq_exprs = best_path->uniq_exprs;

	/* 把修改后的子计划 tlist 初始化为只含 "所需" 的 var */
	newtlist = build_path_tlist(root, &best_path->path);
	nextresno = list_length(newtlist) + 1;
	newitems = false;

	foreach(l, uniq_exprs)
	{
		Expr	   *uniqexpr = lfirst(l);
		TargetEntry *tle;

		tle = tlist_member(uniqexpr, newtlist);
		if (!tle)
		{
			tle = makeTargetEntry((Expr *) uniqexpr,
								  nextresno,
								  NULL,
								  false);
			newtlist = lappend(newtlist, tle);
			nextresno++;
			newitems = true;
		}
	}

	/* 万一需要插入 Result 节点，就用 change_plan_targetlist */
	if (newitems || best_path->umethod == UNIQUE_PATH_SORT)
		subplan = change_plan_targetlist(subplan, newtlist,
										 best_path->path.parallel_safe);

	/*
	 * 建立控制信息，表明分组步骤要检查子计划的哪些输出列。
	 * 可惜不能把这与前一个循环合并，因为那时还不知道最终会用
	 * 子计划 tlist 的
	 * 哪一版。
	 */
	newtlist = subplan->targetlist;
	numGroupCols = list_length(uniq_exprs);
	groupColIdx = (AttrNumber *) palloc(numGroupCols * sizeof(AttrNumber));
	groupCollations = (Oid *) palloc(numGroupCols * sizeof(Oid));

	groupColPos = 0;
	foreach(l, uniq_exprs)
	{
		Expr	   *uniqexpr = lfirst(l);
		TargetEntry *tle;

		tle = tlist_member(uniqexpr, newtlist);
		if (!tle)				/* 不应当发生 */
			elog(ERROR, "failed to find unique expression in subplan tlist");
		groupColIdx[groupColPos] = tle->resno;
		groupCollations[groupColPos] = exprCollation((Node *) tle->expr);
		groupColPos++;
	}

	if (best_path->umethod == UNIQUE_PATH_HASH)
	{
		Oid		   *groupOperators;

		/*
		 * 取得 Agg 节点要用的可哈希等值操作符。
		 * 通常它们与 IN 子句操作符相同，但若
		 * 那些是跨类型操作符，则等值操作符是
		 * IN 子句操作符右侧数据类型的那些。
		 */
		groupOperators = (Oid *) palloc(numGroupCols * sizeof(Oid));
		groupColPos = 0;
		foreach(l, in_operators)
		{
			Oid			in_oper = lfirst_oid(l);
			Oid			eq_oper;

			if (!get_compatible_hash_operators(in_oper, NULL, &eq_oper))
				elog(ERROR, "could not find compatible hash operator for operator %u",
					 in_oper);
			groupOperators[groupColPos++] = eq_oper;
		}

		/*
		 * 既然 Agg 节点反正要做投影，可以给它
		 * 最小输出 tlist，不必带上也许已经加进
		 * 子计划 tlist 的那些东西。
		 */
		plan = (Plan *) make_agg(build_path_tlist(root, &best_path->path),
								 NIL,
								 AGG_HASHED,
								 AGGSPLIT_SIMPLE,
								 numGroupCols,
								 groupColIdx,
								 groupOperators,
								 groupCollations,
								 NIL,
								 NIL,
								 best_path->path.rows,
								 0,
								 subplan);
	}
	else
	{
		List	   *sortList = NIL;
		Sort	   *sort;

		/* 建立一份 ORDER BY 列表，以便按兼容方式排序输入 */
		groupColPos = 0;
		foreach(l, in_operators)
		{
			Oid			in_oper = lfirst_oid(l);
			Oid			sortop;
			Oid			eqop;
			TargetEntry *tle;
			SortGroupClause *sortcl;

			sortop = get_ordering_op_for_equality_op(in_oper, false);
			if (!OidIsValid(sortop))	/* 不应当发生 */
				elog(ERROR, "could not find ordering operator for equality operator %u",
					 in_oper);

			/*
			 * Unique 节点将需要等值操作符。通常这些
			 * 与 IN 子句操作符相同，但若那些是
			 * 跨类型操作符，则等值操作符是
			 * IN 子句操作符右侧数据类型的那些。
			 */
			eqop = get_equality_op_for_ordering_op(sortop, NULL);
			if (!OidIsValid(eqop))	/* 不应当发生 */
				elog(ERROR, "could not find equality operator for ordering operator %u",
					 sortop);

			tle = get_tle_by_resno(subplan->targetlist,
								   groupColIdx[groupColPos]);
			Assert(tle != NULL);

			sortcl = makeNode(SortGroupClause);
			sortcl->tleSortGroupRef = assignSortGroupRef(tle,
														 subplan->targetlist);
			sortcl->eqop = eqop;
			sortcl->sortop = sortop;
			sortcl->nulls_first = false;
			sortcl->hashable = false;	/* 不必弄得很精确 */
			sortList = lappend(sortList, sortcl);
			groupColPos++;
		}
		sort = make_sort_from_sortclauses(sortList, subplan);
		label_sort_with_costsize(root, sort, -1.0);
		plan = (Plan *) make_unique_from_sortclauses((Plan *) sort, sortList);
	}

	/* 把代价数据从 Path 复制到 Plan */
	copy_generic_path_info(plan, &best_path->path);

	return plan;
}

/*
 * create_gather_plan
 *
 *	  为 'best_path' 建立 Gather 计划，并（递归地）为其
 *	  子路径建立计划。
 */
static Gather *
create_gather_plan(PlannerInfo *root, GatherPath *best_path)
{
	Gather	   *gather_plan;
	Plan	   *subplan;
	List	   *tlist;

	/*
	 * 把投影下推到子节点。这样投影工作
	 * 就被并行化，而且结果里不会有系统列（它们
	 * 不能穿过元组队列，因为队列用的是 MinimalTuple
	 * 表示）。
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_EXACT_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	gather_plan = make_gather(tlist,
							  NIL,
							  best_path->num_workers,
							  assign_special_exec_param(root),
							  best_path->single_copy,
							  subplan);

	copy_generic_path_info(&gather_plan->plan, &best_path->path);

	/* 并行计划使用并行模式。 */
	root->glob->parallelModeNeeded = true;

	return gather_plan;
}

/*
 * create_gather_merge_plan
 *
 *	  为 'best_path' 建立 Gather Merge 计划，并（递归地）
 *	  为其子路径建立计划。
 */
static GatherMerge *
create_gather_merge_plan(PlannerInfo *root, GatherMergePath *best_path)
{
	GatherMerge *gm_plan;
	Plan	   *subplan;
	List	   *pathkeys = best_path->path.pathkeys;
	List	   *tlist = build_path_tlist(root, &best_path->path);

	/* 与 Gather 一样，在工作进程里把列投影掉。 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_EXACT_TLIST);

	/* 为 GatherMerge 计划建一个外壳。 */
	gm_plan = makeNode(GatherMerge);
	gm_plan->plan.targetlist = tlist;
	gm_plan->num_workers = best_path->num_workers;
	copy_generic_path_info(&gm_plan->plan, &best_path->path);

	/* 分配重新扫描 Param。 */
	gm_plan->rescan_param = assign_special_exec_param(root);

	/* 没有路径键时 Gather Merge 没有意义；改用 Gather。 */
	Assert(pathkeys != NIL);

	/* 计算排序列信息，并按需要调整子计划的 tlist */
	subplan = prepare_sort_from_pathkeys(subplan, pathkeys,
										 best_path->subpath->parent->relids,
										 gm_plan->sortColIdx,
										 false,
										 &gm_plan->numCols,
										 &gm_plan->sortColIdx,
										 &gm_plan->sortOperators,
										 &gm_plan->collations,
										 &gm_plan->nullsFirst);


	/*
	 * 所有 gather merge 路径都应当已经保证了必要的
	 * 排序顺序，要么加了显式排序节点，要么用了预排序的
	 * 输入。不能简单地在这里按额外路径键再加一次排序，因为
	 * 不能保证该排序是安全的。例如表达式也许
	 * 是易变的，或在其他方面并行不安全。
	 */
	if (!pathkeys_contained_in(pathkeys, best_path->subpath->pathkeys))
		elog(ERROR, "gather merge input not sufficiently sorted");

	/* 现在把子计划插到 GatherMerge 之下。 */
	gm_plan->plan.lefttree = subplan;

	/* 并行计划使用并行模式。 */
	root->glob->parallelModeNeeded = true;

	return gm_plan;
}

/*
 * create_projection_plan
 *
 *	  建立做投影步骤的计划树，并（递归地）为其
 *	  子路径建立计划。投影也许需要一个 Result 节点，
 *	  但有时可以直接让子计划做这项工作。
 */
static Plan *
create_projection_plan(PlannerInfo *root, ProjectionPath *best_path, int flags)
{
	Plan	   *plan;
	Plan	   *subplan;
	List	   *tlist;
	bool		needs_result_node = false;

	/*
	 * 把子路径转换成 Plan，并判断是否需要 Result
	 * 节点。
	 *
	 * 在多数不需要投影的情况下，create_projection_path
	 * 会设置 dummypp，但并不总是如此。首先，createplan.c 的某些
	 * 例程会改自己节点的 tlist。（例如
	 * create_merge_append_plan 也许会给 MergeAppend 加 resjunk 排序列。）
	 * 其次，create_projection_path 无法知道
	 * 投影路径之上会放哪个路径节点，
	 * 因此不能预测它是否需要精确 tlist。由于
	 * 这两个原因，这里必须再检查一次。
	 */
	if (use_physical_tlist(root, &best_path->path, flags))
	{
		/*
		 * 调用方并不真的在乎我们返回什么 tlist，因此实际上
		 * 不必做投影。不过，若调用方在乎 sortgroupref 标注，
		 * 也许仍需确保它们正确。
		 */
		subplan = create_plan_recurse(root, best_path->subpath, 0);
		tlist = subplan->targetlist;
		if (flags & CP_LABEL_TLIST)
			apply_pathtarget_labeling_to_tlist(tlist,
											   best_path->path.pathtarget);
	}
	else if (is_projection_capable_path(best_path->subpath))
	{
		/*
		 * 调用方需要我们返回精确的 tlist，但不需要单独的
		 * 结果节点，因为子路径有投影能力。
		 * 告诉 create_plan_recurse 我们将忽略它产生的
		 * tlist。
		 */
		subplan = create_plan_recurse(root, best_path->subpath,
									  CP_IGNORE_TLIST);
		Assert(is_projection_capable_plan(subplan));
		tlist = build_path_tlist(root, &best_path->path);
	}
	else
	{
		/*
		 * 看起来需要一个结果节点，除非运气好，
		 * 所要求的 tlist 恰好就是子节点想产生的那份。
		 */
		subplan = create_plan_recurse(root, best_path->subpath, 0);
		tlist = build_path_tlist(root, &best_path->path);
		needs_result_node = !tlist_same_exprs(tlist, subplan->targetlist);
	}

	/*
	 * 若我们关于是否包含 Result 节点的决定与
	 * create_projection_path 不同，代价估计就会略有偏差；
	 * 但给计划标上我们实际使用的代价估计，
	 * 而不是 "修正后" 的。XXX 若把更多
	 * 排序列设置逻辑挪进 Path 的建立，这可以收拾干净，但那会给
	 * 建立也许最终不用的 Path 增加开销。）
	 */
	if (!needs_result_node)
	{
		/* 不需要单独的 Result，只把 tlist 赋给子计划 */
		plan = subplan;
		plan->targetlist = tlist;

		/* 用我们实际使用的估计代价给计划做标注 */
		plan->startup_cost = best_path->path.startup_cost;
		plan->total_cost = best_path->path.total_cost;
		plan->plan_rows = best_path->path.rows;
		plan->plan_width = best_path->path.pathtarget->width;
		plan->parallel_safe = best_path->path.parallel_safe;
		/* ……但不要改子计划的 parallel_aware 标志 */
	}
	else
	{
		/* 我们需要一个 Result 节点 */
		plan = (Plan *) make_result(tlist, NULL, subplan);

		copy_generic_path_info(plan, (Path *) best_path);
	}

	return plan;
}

/*
 * inject_projection_plan
 *	  插入一个 Result 节点来做投影步骤。
 *
 * 这用在少数地方：我们临时决定，为某个 Path 节点生成的树
 * 需要一个投影步骤作为其一部分。
 * 应当设法去掉这个，改成更老实的做法。
 *
 * 它难看的一个原因是，必须被告知要应用的正确 parallel_safe 标注
 * （因为即使子计划安全，tlist 也可能不安全）。
 */
static Plan *
inject_projection_plan(Plan *subplan, List *tlist, bool parallel_safe)
{
	Plan	   *plan;

	plan = (Plan *) make_result(tlist, NULL, subplan);

	/*
	 * 原则上，应当按每行收取 tlist 求值代价加上 cpu_per_tuple，
	 * 作为 Result 节点的费用。但前者大概已经
	 * 算进去了，后者在 Path 构建期间没计入，
	 * 因此形式上正确也许只会使 EXPLAIN 输出看起来更不一致，
	 * 而不是更一致。因此只复制子计划的代价。
	 */
	copy_plan_costsize(plan, subplan);
	plan->parallel_safe = parallel_safe;

	return plan;
}

/*
 * change_plan_targetlist
 *	  inject_projection_plan 的外部可用包装。
 *
 * 这是给 FDW 计划生成函数用的，它们也许
 * 想调整某棵子计划树算出的 tlist。一般
 * 需要一个 Result 节点来计算新 tlist，但有些情形可以优化。
 * 多数情况下，tlist_parallel_safe 可以直接作为
 *
 * 该 FDW 自己的 Path 节点的
 * parallel_safe 标志传入。
 */
Plan *
change_plan_targetlist(Plan *subplan, List *tlist, bool tlist_parallel_safe)
{
	/*
	 * 若顶层计划节点不能做投影，且其现有目标列表
	 * 还不是我们需要的，就需要加一个 Result 节点来
	 * 帮忙。
	 */
	if (!is_projection_capable_plan(subplan) &&
		!tlist_same_exprs(tlist, subplan->targetlist))
		subplan = inject_projection_plan(subplan, tlist,
										 subplan->parallel_safe &&
										 tlist_parallel_safe);
	else
	{
		/* 否则可以直接替换该计划节点的 tlist */
		subplan->targetlist = tlist;
		subplan->parallel_safe &= tlist_parallel_safe;
	}
	return subplan;
}

/*
 * create_sort_plan
 *
 *	  为 'best_path' 建立 Sort 计划，并（递归地）为其
 *	  子路径建立计划。
 */
static Sort *
create_sort_plan(PlannerInfo *root, SortPath *best_path, int flags)
{
	Sort	   *plan;
	Plan	   *subplan;

	/*
	 * 不想在排好序的元组里有任何多余的列，因此要求
	 * 更小的 tlist。否则，由于 Sort 不做投影，tlist
	 * 要求会透传。
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_SMALL_TLIST);

	/*
	 * make_sort_from_pathkeys 间接调用 find_ec_member_matching_expr，
	 * 它会忽略不属于给定
	 * relid 的任何子等价类成员。因此，若这条排序路径基于子关系，必须
	 * 传入它的 relid。
	 */
	plan = make_sort_from_pathkeys(subplan, best_path->path.pathkeys,
								   IS_OTHER_REL(best_path->subpath->parent) ?
								   best_path->path.parent->relids : NULL);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_incrementalsort_plan
 *
 *	  与 create_sort_plan 相同，但建立 IncrementalSort 计划。
 */
static IncrementalSort *
create_incrementalsort_plan(PlannerInfo *root, IncrementalSortPath *best_path,
							int flags)
{
	IncrementalSort *plan;
	Plan	   *subplan;

	/* 见上面 create_sort_plan() 的注释 */
	subplan = create_plan_recurse(root, best_path->spath.subpath,
								  flags | CP_SMALL_TLIST);
	plan = make_incrementalsort_from_pathkeys(subplan,
											  best_path->spath.path.pathkeys,
											  IS_OTHER_REL(best_path->spath.subpath->parent) ?
											  best_path->spath.path.parent->relids : NULL,
											  best_path->nPresortedCols);

	copy_generic_path_info(&plan->sort.plan, (Path *) best_path);

	return plan;
}

/*
 * create_group_plan
 *
 *	  为 'best_path' 建立 Group 计划，并（递归地）为其
 *	  子路径建立计划。
 */
static Group *
create_group_plan(PlannerInfo *root, GroupPath *best_path)
{
	Group	   *plan;
	Plan	   *subplan;
	List	   *tlist;
	List	   *quals;

	/*
	 * Group 可以做投影，因此不必对子 tlist 太挑剔，
	 * 但分组列必须可用
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_LABEL_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	quals = order_qual_clauses(root, best_path->qual);

	plan = make_group(tlist,
					  quals,
					  list_length(best_path->groupClause),
					  extract_grouping_cols(best_path->groupClause,
											subplan->targetlist),
					  extract_grouping_ops(best_path->groupClause),
					  extract_grouping_collations(best_path->groupClause,
												  subplan->targetlist),
					  subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_upper_unique_plan
 *
 *	  为 'best_path' 建立 Unique 计划，并（递归地）为其
 *	  子路径建立计划。
 */
static Unique *
create_upper_unique_plan(PlannerInfo *root, UpperUniquePath *best_path, int flags)
{
	Unique	   *plan;
	Plan	   *subplan;

	/*
	 * Unique 不做投影，因此 tlist 要求透传；而且
	 * 分组列必须被标注。
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_LABEL_TLIST);

	plan = make_unique_from_pathkeys(subplan,
									 best_path->path.pathkeys,
									 best_path->numkeys);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_agg_plan
 *
 *	  为 'best_path' 建立 Agg 计划，并（递归地）为其
 *	  子路径建立计划。
 */
static Agg *
create_agg_plan(PlannerInfo *root, AggPath *best_path)
{
	Agg		   *plan;
	Plan	   *subplan;
	List	   *tlist;
	List	   *quals;

	/*
	 * Agg 可以做投影，因此不必对子 tlist 太挑剔，但
	 * 分组列必须可用
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_LABEL_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	quals = order_qual_clauses(root, best_path->qual);

	plan = make_agg(tlist, quals,
					best_path->aggstrategy,
					best_path->aggsplit,
					list_length(best_path->groupClause),
					extract_grouping_cols(best_path->groupClause,
										  subplan->targetlist),
					extract_grouping_ops(best_path->groupClause),
					extract_grouping_collations(best_path->groupClause,
												subplan->targetlist),
					NIL,
					NIL,
					best_path->numGroups,
					best_path->transitionSpace,
					subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * 给定一组分组集的 groupclause，产生
 * 对应的 groupColIdx。
 *
 * root->grouping_map 把 tleSortGroupRef 映射到输入元组里的实际列位置。
 * 因此从 groupclause 的项里取出引用，并
 * 在那里查找它们。
 */
static AttrNumber *
remap_groupColIdx(PlannerInfo *root, List *groupClause)
{
	AttrNumber *grouping_map = root->grouping_map;
	AttrNumber *new_grpColIdx;
	ListCell   *lc;
	int			i;

	Assert(grouping_map);

	new_grpColIdx = palloc0(sizeof(AttrNumber) * list_length(groupClause));

	i = 0;
	foreach(lc, groupClause)
	{
		SortGroupClause *clause = lfirst(lc);

		new_grpColIdx[i++] = grouping_map[clause->tleSortGroupRef];
	}

	return new_grpColIdx;
}

/*
 * create_groupingsets_plan
 *	  为 'best_path' 建立计划，并（递归地）为其
 *	  子路径建立计划。
 *
 *	  发出的是一个 Agg 计划，侧面挂着一些残留的 Agg 和 Sort 节点。
 *	  顶层 Agg 实现 GroupingSetsPath 里指定的最后一个分组集，
 *	  任何额外的分组集
 *	  各自在顶层 Agg 的
 *	  "chain" 列表里产生一个附属 Agg 和 Sort 节点。这些节点不直接参与计划，
 *	  但它们是表示额外步骤所需数据的
 *	  方便办法。
 *
 *	  返回一个 Plan 节点。
 */
static Plan *
create_groupingsets_plan(PlannerInfo *root, GroupingSetsPath *best_path)
{
	Agg		   *plan;
	Plan	   *subplan;
	List	   *rollups = best_path->rollups;
	AttrNumber *grouping_map;
	int			maxref;
	List	   *chain;
	ListCell   *lc;

	/* 没有分组集不应当走到这里 */
	Assert(root->parse->groupingSets);
	Assert(rollups != NIL);

	/*
	 * Agg 可以做投影，因此不必对子 tlist 太挑剔，但
	 * 分组列必须可用
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_LABEL_TLIST);

	/*
	 * 计算从 tleSortGroupRef 到子节点 tlist 中列下标的映射。
	 * 首先，识别 groupClause 里最大的 SortGroupRef，以便
	 * 确定数组大小。
	 */
	maxref = 0;
	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *gc = (SortGroupClause *) lfirst(lc);

		if (gc->tleSortGroupRef > maxref)
			maxref = gc->tleSortGroupRef;
	}

	grouping_map = (AttrNumber *) palloc0((maxref + 1) * sizeof(AttrNumber));

	/* 现在在子节点的 tlist 里查找列号 */
	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *gc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(gc, subplan->targetlist);

		grouping_map[gc->tleSortGroupRef] = tle->resno;
	}

	/*
	 * 在 setrefs.c 期间，需要 grouping_map 来修正 GroupingFunc 节点里的
	 * 列列表。保存它供 setrefs.c 使用。
	 */
	Assert(root->grouping_map == NULL);
	root->grouping_map = grouping_map;

	/*
	 * 生成描述顶层之外其他排序和分组
	 * 操作的侧面节点。注意我们不操心给侧面节点放上
	 * 准确的代价估计；EXPLAIN 只会显示最顶层 Agg 节点的
	 * 代价。
	 */
	chain = NIL;
	if (list_length(rollups) > 1)
	{
		bool		is_first_sort = ((RollupData *) linitial(rollups))->is_hashed;

		for_each_from(lc, rollups, 1)
		{
			RollupData *rollup = lfirst(lc);
			AttrNumber *new_grpColIdx;
			Plan	   *sort_plan = NULL;
			Plan	   *agg_plan;
			AggStrategy strat;

			new_grpColIdx = remap_groupColIdx(root, rollup->groupClause);

			if (!rollup->is_hashed && !is_first_sort)
			{
				sort_plan = (Plan *)
					make_sort_from_groupcols(rollup->groupClause,
											 new_grpColIdx,
											 subplan);
			}

			if (!rollup->is_hashed)
				is_first_sort = false;

			if (rollup->is_hashed)
				strat = AGG_HASHED;
			else if (linitial(rollup->gsets) == NIL)
				strat = AGG_PLAIN;
			else
				strat = AGG_SORTED;

			agg_plan = (Plan *) make_agg(NIL,
										 NIL,
										 strat,
										 AGGSPLIT_SIMPLE,
										 list_length((List *) linitial(rollup->gsets)),
										 new_grpColIdx,
										 extract_grouping_ops(rollup->groupClause),
										 extract_grouping_collations(rollup->groupClause, subplan->targetlist),
										 rollup->gsets,
										 NIL,
										 rollup->numGroups,
										 best_path->transitionSpace,
										 sort_plan);

			/*
			 * 去掉不需要的东西，以免调试输出膨胀。
			 */
			if (sort_plan)
			{
				sort_plan->targetlist = NIL;
				sort_plan->lefttree = NULL;
			}

			chain = lappend(chain, agg_plan);
		}
	}

	/*
	 * 现在建立真正的 Agg 节点
	 */
	{
		RollupData *rollup = linitial(rollups);
		AttrNumber *top_grpColIdx;
		int			numGroupCols;

		top_grpColIdx = remap_groupColIdx(root, rollup->groupClause);

		numGroupCols = list_length((List *) linitial(rollup->gsets));

		plan = make_agg(build_path_tlist(root, &best_path->path),
						best_path->qual,
						best_path->aggstrategy,
						AGGSPLIT_SIMPLE,
						numGroupCols,
						top_grpColIdx,
						extract_grouping_ops(rollup->groupClause),
						extract_grouping_collations(rollup->groupClause, subplan->targetlist),
						rollup->gsets,
						chain,
						rollup->numGroups,
						best_path->transitionSpace,
						subplan);

		/* 把代价数据从 Path 复制到 Plan */
		copy_generic_path_info(&plan->plan, &best_path->path);
	}

	return (Plan *) plan;
}

/*
 * create_minmaxagg_plan
 *
 *	  为 'best_path' 建立 Result 计划，并（递归地）为其
 *	  子路径建立计划。
 */
static Result *
create_minmaxagg_plan(PlannerInfo *root, MinMaxAggPath *best_path)
{
	Result	   *plan;
	List	   *tlist;
	ListCell   *lc;

	/* 为每个聚合的子查询准备一个 InitPlan。 */
	foreach(lc, best_path->mmaggregates)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);
		PlannerInfo *subroot = mminfo->subroot;
		Query	   *subparse = subroot->parse;
		Plan	   *plan;

		/*
		 * 为该子查询生成计划。已经有一条 Path，但必须
		 * 把它转换成 Plan，并在上面接一个 LIMIT 节点。
		 * 由于正在进入不同的规划器上下文（subroot），
		 * 递归到 create_plan，而不是 create_plan_recurse。
		 */
		plan = create_plan(subroot, mminfo->path);

		plan = (Plan *) make_limit(plan,
								   subparse->limitOffset,
								   subparse->limitCount,
								   subparse->limitOption,
								   0, NULL, NULL, NULL);

		/* 必须把正确的代价/宽度数据应用到 Limit 节点 */
		plan->startup_cost = mminfo->path->startup_cost;
		plan->total_cost = mminfo->pathcost;
		plan->plan_rows = 1;
		plan->plan_width = mminfo->path->pathtarget->width;
		plan->parallel_aware = false;
		plan->parallel_safe = mminfo->path->parallel_safe;

		/* 把该计划转换成外层查询里的 InitPlan。 */
		SS_make_initplan_from_plan(root, subroot, plan, mminfo->param);
	}

	/* 生成输出计划——基本上就是一个 Result */
	tlist = build_path_tlist(root, &best_path->path);

	plan = make_result(tlist, (Node *) best_path->quals, NULL);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	/*
	 * 在 setrefs.c 期间，需要把对 Agg 节点的引用
	 * 换成 InitPlan 的输出参数。（不能只在
	 * MinMaxAgg 节点里局部做，因为上面的路径节点也许也有 Agg 引用。）
	 * 保存 mmaggregates 列表，告诉 setrefs.c 去做这件事。
	 */
	Assert(root->minmax_aggs == NIL);
	root->minmax_aggs = best_path->mmaggregates;

	return plan;
}

/*
 * create_windowagg_plan
 *
 *	  为 'best_path' 建立 WindowAgg 计划，并（递归地）为其
 *	  子路径建立计划。
 */
static WindowAgg *
create_windowagg_plan(PlannerInfo *root, WindowAggPath *best_path)
{
	WindowAgg  *plan;
	WindowClause *wc = best_path->winclause;
	int			numPart = list_length(wc->partitionClause);
	int			numOrder = list_length(wc->orderClause);
	Plan	   *subplan;
	List	   *tlist;
	int			partNumCols;
	AttrNumber *partColIdx;
	Oid		   *partOperators;
	Oid		   *partCollations;
	int			ordNumCols;
	AttrNumber *ordColIdx;
	Oid		   *ordOperators;
	Oid		   *ordCollations;
	ListCell   *lc;

	/*
	 * 这里选择 tlist 的动机是：WindowAgg 会把
	 * 窗口帧的输入行存在 tuplestore 里；因此
	 * 我们应当要求小的 tlist，以免浪费空间。当然
	 * 分组列必须可用。
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  CP_LABEL_TLIST | CP_SMALL_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	/*
	 * 把 SortGroupClause 列表转换成执行器想要的属性下标和等值
	 * 操作符数组。（注意：原则上，若路径键逻辑证明某些排序列冗余，
	 * 可以丢掉它们。
	 * 不过，似乎不值得特意
	 * 去优化这类情形。无论如何，对 RANGE OFFSET 情形，*不得*
	 * 去掉那个排序列，因为执行器做 in_range
	 * 测试时需要它，即使已知它等于某个分区列。）
	 */
	partColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numPart);
	partOperators = (Oid *) palloc(sizeof(Oid) * numPart);
	partCollations = (Oid *) palloc(sizeof(Oid) * numPart);

	partNumCols = 0;
	foreach(lc, wc->partitionClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, subplan->targetlist);

		Assert(OidIsValid(sgc->eqop));
		partColIdx[partNumCols] = tle->resno;
		partOperators[partNumCols] = sgc->eqop;
		partCollations[partNumCols] = exprCollation((Node *) tle->expr);
		partNumCols++;
	}

	ordColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numOrder);
	ordOperators = (Oid *) palloc(sizeof(Oid) * numOrder);
	ordCollations = (Oid *) palloc(sizeof(Oid) * numOrder);

	ordNumCols = 0;
	foreach(lc, wc->orderClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, subplan->targetlist);

		Assert(OidIsValid(sgc->eqop));
		ordColIdx[ordNumCols] = tle->resno;
		ordOperators[ordNumCols] = sgc->eqop;
		ordCollations[ordNumCols] = exprCollation((Node *) tle->expr);
		ordNumCols++;
	}

	/* 最后可以建立 WindowAgg 节点了 */
	plan = make_windowagg(tlist,
						  wc->winref,
						  partNumCols,
						  partColIdx,
						  partOperators,
						  partCollations,
						  ordNumCols,
						  ordColIdx,
						  ordOperators,
						  ordCollations,
						  wc->frameOptions,
						  wc->startOffset,
						  wc->endOffset,
						  wc->startInRangeFunc,
						  wc->endInRangeFunc,
						  wc->inRangeColl,
						  wc->inRangeAsc,
						  wc->inRangeNullsFirst,
						  wc->runCondition,
						  best_path->qual,
						  best_path->topwindow,
						  subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_setop_plan
 *
 *	  为 'best_path' 建立 SetOp 计划，并（递归地）为其
 *	  子路径建立计划。
 */
static SetOp *
create_setop_plan(PlannerInfo *root, SetOpPath *best_path, int flags)
{
	SetOp	   *plan;
	Plan	   *subplan;
	long		numGroups;

	/*
	 * SetOp 不做投影，因此 tlist 要求透传；而且
	 * 分组列必须被标注。
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_LABEL_TLIST);

	/* 把 numGroups 转成 long int——但要当心溢出！ */
	numGroups = clamp_cardinality_to_long(best_path->numGroups);

	plan = make_setop(best_path->cmd,
					  best_path->strategy,
					  subplan,
					  best_path->distinctList,
					  best_path->flagColIdx,
					  best_path->firstFlag,
					  numGroups);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_recursiveunion_plan
 *
 *	  为 'best_path' 建立 RecursiveUnion 计划，并（递归地）为其
 *	  子路径建立计划。
 */
static RecursiveUnion *
create_recursiveunion_plan(PlannerInfo *root, RecursiveUnionPath *best_path)
{
	RecursiveUnion *plan;
	Plan	   *leftplan;
	Plan	   *rightplan;
	List	   *tlist;
	long		numGroups;

	/* 需要两个子节点产生相同的 tlist，因此强制如此 */
	leftplan = create_plan_recurse(root, best_path->leftpath, CP_EXACT_TLIST);
	rightplan = create_plan_recurse(root, best_path->rightpath, CP_EXACT_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	/* 把 numGroups 转成 long int——但要当心溢出！ */
	numGroups = clamp_cardinality_to_long(best_path->numGroups);

	plan = make_recursive_union(tlist,
								leftplan,
								rightplan,
								best_path->wtParam,
								best_path->distinctList,
								numGroups);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_lockrows_plan
 *
 *	  为 'best_path' 建立 LockRows 计划，并（递归地）为其
 *	  子路径建立计划。
 */
static LockRows *
create_lockrows_plan(PlannerInfo *root, LockRowsPath *best_path,
					 int flags)
{
	LockRows   *plan;
	Plan	   *subplan;

	/* LockRows 不做投影，因此 tlist 要求透传 */
	subplan = create_plan_recurse(root, best_path->subpath, flags);

	plan = make_lockrows(subplan, best_path->rowMarks, best_path->epqParam);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_modifytable_plan
 *	  为 'best_path' 建立 ModifyTable 计划。
 *
 *	  返回一个 Plan 节点。
 */
static ModifyTable *
create_modifytable_plan(PlannerInfo *root, ModifyTablePath *best_path)
{
	ModifyTable *plan;
	Path	   *subpath = best_path->subpath;
	Plan	   *subplan;

	/* 子计划必须恰好产生指定的 tlist */
	subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

	/* 也转移 resname/resjunk 标注，让执行器满意 */
	apply_tlist_labeling(subplan->targetlist, root->processed_tlist);

	plan = make_modifytable(root,
							subplan,
							best_path->operation,
							best_path->canSetTag,
							best_path->nominalRelation,
							best_path->rootRelation,
							best_path->partColsUpdated,
							best_path->resultRelations,
							best_path->updateColnosLists,
							best_path->withCheckOptionLists,
							best_path->returningLists,
							best_path->rowMarks,
							best_path->onconflict,
							best_path->mergeActionLists,
							best_path->epqParam);

	copy_generic_path_info(&plan->plan, &best_path->path);

	return plan;
}

/*
 * create_limit_plan
 *
 *	  为 'best_path' 建立 Limit 计划，并（递归地）为其
 *	  子路径建立计划。
 */
static Limit *
create_limit_plan(PlannerInfo *root, LimitPath *best_path, int flags)
{
	Limit	   *plan;
	Plan	   *subplan;
	int			numUniqkeys = 0;
	AttrNumber *uniqColIdx = NULL;
	Oid		   *uniqOperators = NULL;
	Oid		   *uniqCollations = NULL;

	/* Limit 不做投影，因此 tlist 要求透传 */
	subplan = create_plan_recurse(root, best_path->subpath, flags);

	/* 抽出 WITH TIES 比较行所需的信息。 */
	if (best_path->limitOption == LIMIT_OPTION_WITH_TIES)
	{
		Query	   *parse = root->parse;
		ListCell   *l;

		numUniqkeys = list_length(parse->sortClause);
		uniqColIdx = (AttrNumber *) palloc(numUniqkeys * sizeof(AttrNumber));
		uniqOperators = (Oid *) palloc(numUniqkeys * sizeof(Oid));
		uniqCollations = (Oid *) palloc(numUniqkeys * sizeof(Oid));

		numUniqkeys = 0;
		foreach(l, parse->sortClause)
		{
			SortGroupClause *sortcl = (SortGroupClause *) lfirst(l);
			TargetEntry *tle = get_sortgroupclause_tle(sortcl, parse->targetList);

			uniqColIdx[numUniqkeys] = tle->resno;
			uniqOperators[numUniqkeys] = sortcl->eqop;
			uniqCollations[numUniqkeys] = exprCollation((Node *) tle->expr);
			numUniqkeys++;
		}
	}

	plan = make_limit(subplan,
					  best_path->limitOffset,
					  best_path->limitCount,
					  best_path->limitOption,
					  numUniqkeys, uniqColIdx, uniqOperators, uniqCollations);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}


/*****************************************************************************
 *
 *	BASE-RELATION SCAN METHODS（基关系扫描方法）
 *
 *****************************************************************************/


/*
 * create_seqscan_plan
 *	 返回 'best_path' 所扫描的基关系的顺序扫描计划，
 *	 限制子句为 'scan_clauses'，目标列表为 'tlist'。
 */
static SeqScan *
create_seqscan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses)
{
	SeqScan    *scan_plan;
	Index		scan_relid = best_path->parent->relid;

	/* 它应当是一个基关系…… */
	Assert(scan_relid > 0);
	Assert(best_path->parent->rtekind == RTE_RELATION);

	/* 把子句排成最佳执行顺序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* 把任何外层关系变量换成嵌套循环参数 */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_seqscan(tlist,
							 scan_clauses,
							 scan_relid);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_samplescan_plan
 *	 返回 'best_path' 所扫描的基关系的采样扫描计划，
 *	 限制子句为 'scan_clauses'，目标列表为 'tlist'。
 */
static SampleScan *
create_samplescan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses)
{
	SampleScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	TableSampleClause *tsc;

	/* 它应当是带 tablesample 子句的基关系…… */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_RELATION);
	tsc = rte->tablesample;
	Assert(tsc != NULL);

	/* 把子句排成最佳执行顺序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* 把任何外层关系变量换成嵌套循环参数 */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		tsc = (TableSampleClause *)
			replace_nestloop_params(root, (Node *) tsc);
	}

	scan_plan = make_samplescan(tlist,
								scan_clauses,
								scan_relid,
								tsc);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_indexscan_plan
 *	  返回 'best_path' 所扫描的基关系的索引扫描计划，
 *	  限制子句为 'scan_clauses'，目标列表为 'tlist'。
 *
 * 普通 IndexScans 和 IndexOnlyScans 都用这个，因为两者的
 * 限定条件预处理工作相同。注意由调用方告诉我们
 * 建哪一种——我们不看 best_path->path.pathtype，因为
 * create_bitmap_subplan 需要能够推翻先前的决定。
 */
static Scan *
create_indexscan_plan(PlannerInfo *root,
					  IndexPath *best_path,
					  List *tlist,
					  List *scan_clauses,
					  bool indexonly)
{
	Scan	   *scan_plan;
	List	   *indexclauses = best_path->indexclauses;
	List	   *indexorderbys = best_path->indexorderbys;
	Index		baserelid = best_path->path.parent->relid;
	IndexOptInfo *indexinfo = best_path->indexinfo;
	Oid			indexoid = indexinfo->indexoid;
	List	   *qpqual;
	List	   *stripped_indexquals;
	List	   *fixed_indexquals;
	List	   *fixed_indexorderbys;
	List	   *indexorderbyops = NIL;
	ListCell   *l;

	/* 它应当是一个基关系…… */
	Assert(baserelid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);
	/* 检查扫描方向是否有效 */
	Assert(best_path->indexscandir == ForwardScanDirection ||
		   best_path->indexscandir == BackwardScanDirection);

	/*
	 * 从 IndexClauses 列表抽出索引限定表达式（去掉 RestrictInfos），
	 * 并准备一份用索引 Var 替换了
	 * 表 Var 的副本。（这一步也对
	 * fixed_indexquals 做 replace_nestloop_params。）
	 */
	fix_indexqual_references(root, best_path,
							 &stripped_indexquals,
							 &fixed_indexquals);

	/*
	 * 同样修正 ORDER BY 表达式里的索引属性引用。
	 */
	fixed_indexorderbys = fix_indexorderby_references(root, best_path);

	/*
	 * qpqual 列表必须包含索引不会自动处理的全部限制，
	 * 将由单独的门控计划节点处理的伪常量子句除外。
	 * indexquals 里的全部谓词都会被检查（由索引本身，
	 * 或由 nodeIndexscan.c），
	 * 但若涉及任何 "特殊" 操作符，就必须
	 * 把它们放进 qpqual。结果是 qpqual 必须包含
	 * scan_clauses 减去出现在 indexquals 里的那些。
	 *
	 * is_redundant_with_indexclauses() 检测扫描子句
	 * 出现在 indexclauses 列表里、或与某条 indexclause 由同一个
	 * EquivalenceClass 生成因而与它冗余、
	 * 尽管并不相等的情形。（后者发生在 indxpath.c 更喜欢一条
	 * 与 generate_join_implied_equalities
	 * 为参数化扫描的 ppi_clauses 所选不同的推导等值时。）注意它不会
	 * 匹配有损索引子句，这很关键，因为那种情况下必须
	 * 把原子句放进 qpqual。
	 *
	 * 有些情形（尤其是 OR 起来的索引条件）里，也许有
	 * 与索引限定并不相等、但在逻辑上被它们蕴含的
	 * scan_clauses；因此也试一次 predicate_implied_by() 检查，看
	 * 能否那样丢掉限定条件。（predicate_implied_by 假定其
	 * 第一个输入只含不变函数，因此必须检查
	 * 这一点。）
	 *
	 * 注意：若改这段代码，也应当看
	 * costsize.c 里的 extract_nonindex_conditions()。
	 */
	qpqual = NIL;
	foreach(l, scan_clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		if (rinfo->pseudoconstant)
			continue;			/* 这里也许丢掉伪常量 */
		if (is_redundant_with_indexclauses(rinfo, indexclauses))
			continue;			/* 重复，或由同一个 EquivalenceClass 推导而来 */
		if (!contain_mutable_functions((Node *) rinfo->clause) &&
			predicate_implied_by(list_make1(rinfo->clause), stripped_indexquals,
								 false))
			continue;			/* 可证明被 indexquals 蕴含 */
		qpqual = lappend(qpqual, rinfo);
	}

	/* 把子句排成最佳执行顺序 */
	qpqual = order_qual_clauses(root, qpqual);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	qpqual = extract_actual_clauses(qpqual, false);

	/*
	 * 必须把 indexqualorig、qpqual 和 indexorderbyorig 表达式里的
	 * 任何外层关系变量换成嵌套循环参数。要与
	 * fix_indexqual_references 里的处理分开做，有点
	 * 烦人——推广内侧
	 * 索引扫描支持时再重新考虑。但注意不能真的更早做，因为
	 * 那会破坏上面与谓词的比较……（还是会？那些
	 * 不会有外层引用）
	 */
	if (best_path->path.param_info)
	{
		stripped_indexquals = (List *)
			replace_nestloop_params(root, (Node *) stripped_indexquals);
		qpqual = (List *)
			replace_nestloop_params(root, (Node *) qpqual);
		indexorderbys = (List *)
			replace_nestloop_params(root, (Node *) indexorderbys);
	}

	/*
	 * 若有 ORDER BY 表达式，就查找其
	 * 结果数据类型的排序操作符。
	 */
	if (indexorderbys)
	{
		ListCell   *pathkeyCell,
				   *exprCell;

		/*
		 * PathKey 含有我们据以排序的 btree opfamily 的 OID，但
		 * 这还不太够，因为需要该表达式的数据类型，
		 * 才能在操作符族里查找排序操作符。
		 */
		Assert(list_length(best_path->path.pathkeys) == list_length(indexorderbys));
		forboth(pathkeyCell, best_path->path.pathkeys, exprCell, indexorderbys)
		{
			PathKey    *pathkey = (PathKey *) lfirst(pathkeyCell);
			Node	   *expr = (Node *) lfirst(exprCell);
			Oid			exprtype = exprType(expr);
			Oid			sortop;

			/* 从 opfamily 取得排序操作符 */
			sortop = get_opfamily_member(pathkey->pk_opfamily,
										 exprtype,
										 exprtype,
										 pathkey->pk_strategy);
			if (!OidIsValid(sortop))
				elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
					 pathkey->pk_strategy, exprtype, exprtype, pathkey->pk_opfamily);
			indexorderbyops = lappend_oid(indexorderbyops, sortop);
		}
	}

	/*
	 * 对仅索引扫描，若 indextlist 项是索引访问方法不能返回的
	 * 列，就必须把它们标成 resjunk；这提示 setrefs.c
	 * 不要生成对这些列的引用。
	 */
	if (indexonly)
	{
		int			i = 0;

		foreach(l, indexinfo->indextlist)
		{
			TargetEntry *indextle = (TargetEntry *) lfirst(l);

			indextle->resjunk = !indexinfo->canreturn[i];
			i++;
		}
	}

	/* 终于可以建立计划节点了 */
	if (indexonly)
		scan_plan = (Scan *) make_indexonlyscan(tlist,
												qpqual,
												baserelid,
												indexoid,
												fixed_indexquals,
												stripped_indexquals,
												fixed_indexorderbys,
												indexinfo->indextlist,
												best_path->indexscandir);
	else
		scan_plan = (Scan *) make_indexscan(tlist,
											qpqual,
											baserelid,
											indexoid,
											fixed_indexquals,
											stripped_indexquals,
											fixed_indexorderbys,
											indexorderbys,
											indexorderbyops,
											best_path->indexscandir);

	copy_generic_path_info(&scan_plan->plan, &best_path->path);

	return scan_plan;
}

/*
 * create_bitmap_scan_plan
 *	  返回 'best_path' 所扫描的基关系的位图扫描计划，
 *	  限制子句为 'scan_clauses'，目标列表为 'tlist'。
 */
static BitmapHeapScan *
create_bitmap_scan_plan(PlannerInfo *root,
						BitmapHeapPath *best_path,
						List *tlist,
						List *scan_clauses)
{
	Index		baserelid = best_path->path.parent->relid;
	Plan	   *bitmapqualplan;
	List	   *bitmapqualorig;
	List	   *indexquals;
	List	   *indexECs;
	List	   *qpqual;
	ListCell   *l;
	BitmapHeapScan *scan_plan;

	/* 它应当是一个基关系…… */
	Assert(baserelid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/* 把 bitmapqual 树处理成 Plan 树和限定列表 */
	bitmapqualplan = create_bitmap_subplan(root, best_path->bitmapqual,
										   &bitmapqualorig, &indexquals,
										   &indexECs);

	if (best_path->path.parallel_aware)
		bitmap_subplan_mark_shared(bitmapqualplan);

	/*
	 * qpqual 列表必须包含索引不会自动处理的全部限制，
	 * 将由单独的门控计划节点处理的伪常量子句除外。
	 * indexquals 里的全部谓词都会被检查（由索引本身，或由
	 * nodeBitmapHeapscan.c），但若涉及任何 "特殊" 操作符，
	 * 就必须把它们加进 qpqual。结果是 qpqual
	 * 必须包含 scan_clauses 减去出现在 indexquals 里的那些。
	 * 这个循环与 create_indexscan_plan() 里可比较的代码相似，
	 *
	 * 但有些差别，因为它必须把扫描子句与
	 * 去掉了 RestrictInfos 的 indexquals 比较。更多
	 * 信息见那里的注释。
	 * 正常情况下简单的 equal() 检查就足以发现重复
	 *
	 * 子句，因此先试那个。接着看扫描子句是否因由
	 * 同一个等价类生成而与任何顶层 indexqual
	 * 冗余。然后再试 predicate_implied_by()。
	 * 与 create_indexscan_plan() 不同，
	 *
	 * 这里的 predicate_implied_by() 测试
	 * 对去掉被索引谓词蕴含的 qpqual 有用，
	 * 因为谓词条件包含在
	 * create_bitmap_subplan() 返回的 "indexquals" 里。位图扫描必须那样
	 * 做，因为若扫描
	 * 变成有损的，谓词条件需要重新检查，因此必须放进 bitmapqualorig。
	 */
	qpqual = NIL;
	foreach(l, scan_clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);
		Node	   *clause = (Node *) rinfo->clause;

		if (rinfo->pseudoconstant)
			continue;			/* 这里也许丢掉伪常量 */
		if (list_member(indexquals, clause))
			continue;			/* 简单重复 */
		if (rinfo->parent_ec && list_member_ptr(indexECs, rinfo->parent_ec))
			continue;			/* 由同一个 EquivalenceClass 推导而来 */
		if (!contain_mutable_functions(clause) &&
			predicate_implied_by(list_make1(clause), indexquals, false))
			continue;			/* 可证明被 indexquals 蕴含 */
		qpqual = lappend(qpqual, rinfo);
	}

	/* 把子句排成最佳执行顺序 */
	qpqual = order_qual_clauses(root, qpqual);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	qpqual = extract_actual_clauses(qpqual, false);

	/*
	 * 处理特殊操作符时，此时
	 * qpqual 和 bitmapqualorig 里会有重复子句。不妨从
	 * bitmapqualorig 里丢掉它们，因为没有必要把测试
	 * 做两遍。
	 */
	bitmapqualorig = list_difference_ptr(bitmapqualorig, qpqual);

	/*
	 * 必须把 qpqual 和 bitmapqualorig 表达式里的
	 * 任何外层关系变量换成嵌套循环参数。（挂在 bitmapqualplan 树里计划节点上的
	 * 表达式已经做过了。）
	 */
	if (best_path->path.param_info)
	{
		qpqual = (List *)
			replace_nestloop_params(root, (Node *) qpqual);
		bitmapqualorig = (List *)
			replace_nestloop_params(root, (Node *) bitmapqualorig);
	}

	/* 终于可以建立计划节点了 */
	scan_plan = make_bitmap_heapscan(tlist,
									 qpqual,
									 bitmapqualplan,
									 bitmapqualorig,
									 baserelid);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * 给定一棵 bitmapqual 树，生成实现它的 Plan 树
 *
 * 作为副产品，也在 *qual 和 *indexqual 里返回限定列表
 * （隐式 AND 形式，不带 RestrictInfos），描述原来的索引
 * 条件和生成的 indexqual 条件。（简单情况下它们相同，
 * 但涉及特殊索引操作符时，前一份
 * 列表包含特殊条件，后一份包含由它们推导出的实际
 * 可索引条件。）两份列表都包含部分索引
 * 谓词，因为若位图扫描变成有损的，必须重新检查谓词以及索引
 * 条件。
 *
 * 此外，为所有
 * 可能从等价类冗余推导出的顶层 indexqual 返回一份 EquivalenceClass 指针列表。
 * 这样就能去掉与这类限定冗余的 scan_clauses。
 * （不试图检测 OR 子树内部限定的这类冗余。
 * 若以 RestrictInfo 形式返回
 * indexquals，可以用不那么取巧的办法做，但那会更慢，而且仍然相当
 * 乱，因为许多情况下得建立新的 RestrictInfos。）
 */
static Plan *
create_bitmap_subplan(PlannerInfo *root, Path *bitmapqual,
					  List **qual, List **indexqual, List **indexECs)
{
	Plan	   *plan;

	if (IsA(bitmapqual, BitmapAndPath))
	{
		BitmapAndPath *apath = (BitmapAndPath *) bitmapqual;
		List	   *subplans = NIL;
		List	   *subquals = NIL;
		List	   *subindexquals = NIL;
		List	   *subindexECs = NIL;
		ListCell   *l;

		/*
		 * 子计划之间很可能有冗余限定，因为一条
		 * 顶层 WHERE 限定也许被用来形成好几条
		 * 不同的索引限定。我们并不极度努力消除
		 * 冗余，但确实用
		 * list_concat_unique 消除明显的重复。
		 */
		foreach(l, apath->bitmapquals)
		{
			Plan	   *subplan;
			List	   *subqual;
			List	   *subindexqual;
			List	   *subindexEC;

			subplan = create_bitmap_subplan(root, (Path *) lfirst(l),
											&subqual, &subindexqual,
											&subindexEC);
			subplans = lappend(subplans, subplan);
			subquals = list_concat_unique(subquals, subqual);
			subindexquals = list_concat_unique(subindexquals, subindexqual);
			/* indexECs 里的重复不值得去掉 */
			subindexECs = list_concat(subindexECs, subindexEC);
		}
		plan = (Plan *) make_bitmap_and(subplans);
		plan->startup_cost = apath->path.startup_cost;
		plan->total_cost = apath->path.total_cost;
		plan->plan_rows =
			clamp_row_est(apath->bitmapselectivity * apath->path.parent->tuples);
		plan->plan_width = 0;	/* 无意义 */
		plan->parallel_aware = false;
		plan->parallel_safe = apath->path.parallel_safe;
		*qual = subquals;
		*indexqual = subindexquals;
		*indexECs = subindexECs;
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		BitmapOrPath *opath = (BitmapOrPath *) bitmapqual;
		List	   *subplans = NIL;
		List	   *subquals = NIL;
		List	   *subindexquals = NIL;
		bool		const_true_subqual = false;
		bool		const_true_subindexqual = false;
		ListCell   *l;

		/*
		 * 这里只检测没有限定条件的子计划。没有限定条件的子计划会
		 * 使我们生成 "... OR true ..."，不妨把它收成
		 * 只剩 "true"。不试图消除冗余子子句，
		 * 因为（a）不像 AND 情形那么可能，而且（b）我们
		 * 很可能在处理成百甚至上千个 OR 条件，
		 * 也许来自一份很长的 IN 列表。list_append_unique 的性能
		 * 会不可接受。
		 */
		foreach(l, opath->bitmapquals)
		{
			Plan	   *subplan;
			List	   *subqual;
			List	   *subindexqual;
			List	   *subindexEC;

			subplan = create_bitmap_subplan(root, (Path *) lfirst(l),
											&subqual, &subindexqual,
											&subindexEC);
			subplans = lappend(subplans, subplan);
			if (subqual == NIL)
				const_true_subqual = true;
			else if (!const_true_subqual)
				subquals = lappend(subquals,
								   make_ands_explicit(subqual));
			if (subindexqual == NIL)
				const_true_subindexqual = true;
			else if (!const_true_subindexqual)
				subindexquals = lappend(subindexquals,
										make_ands_explicit(subindexqual));
		}

		/*
		 * 存在 ScalarArrayOpExpr 限定时，也许已经建成
		 * 只有一条子路径的 BitmapOrPaths；不要加 OR 步骤。
		 */
		if (list_length(subplans) == 1)
		{
			plan = (Plan *) linitial(subplans);
		}
		else
		{
			plan = (Plan *) make_bitmap_or(subplans);
			plan->startup_cost = opath->path.startup_cost;
			plan->total_cost = opath->path.total_cost;
			plan->plan_rows =
				clamp_row_est(opath->bitmapselectivity * opath->path.parent->tuples);
			plan->plan_width = 0;	/* 无意义 */
			plan->parallel_aware = false;
			plan->parallel_safe = opath->path.parallel_safe;
		}

		/*
		 * 若有常量 TRUE 的子限定，这个 OR 就收成常量
		 * TRUE。也避免生成单元素 OR，那可能因
		 * 冗余消除或 ScalarArrayOpExpr 限定而发生。
		 */
		if (const_true_subqual)
			*qual = NIL;
		else if (list_length(subquals) <= 1)
			*qual = subquals;
		else
			*qual = list_make1(make_orclause(subquals));
		if (const_true_subindexqual)
			*indexqual = NIL;
		else if (list_length(subindexquals) <= 1)
			*indexqual = subindexquals;
		else
			*indexqual = list_make1(make_orclause(subindexquals));
		*indexECs = NIL;
	}
	else if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) bitmapqual;
		IndexScan  *iscan;
		List	   *subquals;
		List	   *subindexquals;
		List	   *subindexECs;
		ListCell   *l;

		/* 使用常规的索引扫描计划建立机制…… */
		iscan = castNode(IndexScan,
						 create_indexscan_plan(root, ipath,
											   NIL, NIL, false));
		/* 然后转换成位图索引扫描 */
		plan = (Plan *) make_bitmap_indexscan(iscan->scan.scanrelid,
											  iscan->indexid,
											  iscan->indexqual,
											  iscan->indexqualorig);
		/* 并适当设置其代价/宽度字段 */
		plan->startup_cost = 0.0;
		plan->total_cost = ipath->indextotalcost;
		plan->plan_rows =
			clamp_row_est(ipath->indexselectivity * ipath->path.parent->tuples);
		plan->plan_width = 0;	/* 无意义 */
		plan->parallel_aware = false;
		plan->parallel_safe = ipath->path.parallel_safe;
		/* 抽出原来的索引子句、实际的索引限定、相关的等价类 */
		subquals = NIL;
		subindexquals = NIL;
		subindexECs = NIL;
		foreach(l, ipath->indexclauses)
		{
			IndexClause *iclause = (IndexClause *) lfirst(l);
			RestrictInfo *rinfo = iclause->rinfo;

			Assert(!rinfo->pseudoconstant);
			subquals = lappend(subquals, rinfo->clause);
			subindexquals = list_concat(subindexquals,
										get_actual_clauses(iclause->indexquals));
			if (rinfo->parent_ec)
				subindexECs = lappend(subindexECs, rinfo->parent_ec);
		}
		/* 也可以加上任何索引谓词条件 */
		foreach(l, ipath->indexinfo->indpred)
		{
			Expr	   *pred = (Expr *) lfirst(l);

			/*
			 * 我们知道索引谓词必定已被整个
			 * 查询条件蕴含，但它也许被推进 bitmapqual 的
			 * 那些条件蕴含，也许没有。避免
			 * 生成冗余条件。
			 */
			if (!predicate_implied_by(list_make1(pred), subquals, false))
			{
				subquals = lappend(subquals, pred);
				subindexquals = lappend(subindexquals, pred);
			}
		}
		*qual = subquals;
		*indexqual = subindexquals;
		*indexECs = subindexECs;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d", nodeTag(bitmapqual));
		plan = NULL;			/* 消除编译器告警 */
	}

	return plan;
}

/*
 * create_tidscan_plan
 *	 返回 'best_path' 所扫描的基关系的 tid 扫描计划，
 *	 限制子句为 'scan_clauses'，目标列表为 'tlist'。
 */
static TidScan *
create_tidscan_plan(PlannerInfo *root, TidPath *best_path,
					List *tlist, List *scan_clauses)
{
	TidScan    *scan_plan;
	Index		scan_relid = best_path->path.parent->relid;
	List	   *tidquals = best_path->tidquals;

	/* 它应当是一个基关系…… */
	Assert(scan_relid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/*
	 * qpqual 列表必须包含 tidquals 列表没有强制的全部限制。
	 * 由于 tidquals 是 OR 语义，把它与 scan_clauses 对上时必须小心。
	 * 把单条 tidqual 的情形与多条 tidqual 的情形
	 * 分开处理比较方便。在
	 * 单条 tidqual 的情形里，当 scan_clauses 仍
	 * 为 RestrictInfo 形式时查看它们，并丢掉与该
	 * tidqual 冗余的那些。
	 *
	 * 正常情况下简单的指针相等检查就足以发现
	 * 重复的 RestrictInfos，因此先试那个。
	 *
	 * 另一种常见情形是某条 scan_clauses 项与某条 tidqual 由同一个
	 * EquivalenceClass 生成，因而与它冗余，
	 * 尽管并不相等。
	 *
	 * 与索引路径不同，我们不费心用 predicate_implied_by()；
	 * 它能赢的情形相当少。
	 */
	if (list_length(tidquals) == 1)
	{
		List	   *qpqual = NIL;
		ListCell   *l;

		foreach(l, scan_clauses)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

			if (rinfo->pseudoconstant)
				continue;		/* 这里也许丢掉伪常量 */
			if (list_member_ptr(tidquals, rinfo))
				continue;		/* 简单重复 */
			if (is_redundant_derived_clause(rinfo, tidquals))
				continue;		/* 由同一个 EquivalenceClass 推导而来 */
			qpqual = lappend(qpqual, rinfo);
		}
		scan_clauses = qpqual;
	}

	/* 把子句排成最佳执行顺序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	tidquals = extract_actual_clauses(tidquals, false);
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/*
	 * 若有多条 tidqual，在去掉 RestrictInfos 之后再去掉重复的
	 * scan_clauses 更方便。这种情况下，
	 * 因为 tidquals 表示 OR 子子句，它们不可能来自
	 * EquivalenceClasses，因此不必担心匹配
	 * 并不相同的子句。另一方面，因为 tidpath.c 会从
	 * 某条 OR 子句抽出那些子子句并建立自己的列表，
	 * 我们肯定不会与任何扫描子句有指针相等。因此
	 * 把 tidquals 列表转换成显式 OR 子句，看能否
	 * 用 equal() 匹配到任何扫描子句。
	 */
	if (list_length(tidquals) > 1)
		scan_clauses = list_difference(scan_clauses,
									   list_make1(make_orclause(tidquals)));

	/* 把任何外层关系变量换成嵌套循环参数 */
	if (best_path->path.param_info)
	{
		tidquals = (List *)
			replace_nestloop_params(root, (Node *) tidquals);
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_tidscan(tlist,
							 scan_clauses,
							 scan_relid,
							 tidquals);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * create_tidrangescan_plan
 *	 返回 'best_path' 所扫描的基关系的 tid 范围扫描计划，
 *	 限制子句为 'scan_clauses'，目标列表为 'tlist'。
 */
static TidRangeScan *
create_tidrangescan_plan(PlannerInfo *root, TidRangePath *best_path,
						 List *tlist, List *scan_clauses)
{
	TidRangeScan *scan_plan;
	Index		scan_relid = best_path->path.parent->relid;
	List	   *tidrangequals = best_path->tidrangequals;

	/* 它应当是一个基关系…… */
	Assert(scan_relid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/*
	 * qpqual 列表必须包含 tidrangequals 列表没有强制的全部限制。
	 * tidrangequals 是 AND 语义，因此可以简单地
	 * 去掉出现在其中的任何限定。
	 */
	{
		List	   *qpqual = NIL;
		ListCell   *l;

		foreach(l, scan_clauses)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

			if (rinfo->pseudoconstant)
				continue;		/* 这里也许丢掉伪常量 */
			if (list_member_ptr(tidrangequals, rinfo))
				continue;		/* 简单重复 */
			qpqual = lappend(qpqual, rinfo);
		}
		scan_clauses = qpqual;
	}

	/* 把子句排成最佳执行顺序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	tidrangequals = extract_actual_clauses(tidrangequals, false);
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* 把任何外层关系变量换成嵌套循环参数 */
	if (best_path->path.param_info)
	{
		tidrangequals = (List *)
			replace_nestloop_params(root, (Node *) tidrangequals);
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_tidrangescan(tlist,
								  scan_clauses,
								  scan_relid,
								  tidrangequals);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * create_subqueryscan_plan
 *	 返回 'best_path' 所扫描的基关系的子查询扫描计划，
 *	 限制子句为 'scan_clauses'，目标列表为 'tlist'。
 */
static SubqueryScan *
create_subqueryscan_plan(PlannerInfo *root, SubqueryScanPath *best_path,
						 List *tlist, List *scan_clauses)
{
	SubqueryScan *scan_plan;
	RelOptInfo *rel = best_path->path.parent;
	Index		scan_relid = rel->relid;
	Plan	   *subplan;

	/* 它应当是一个子查询基关系…… */
	Assert(scan_relid > 0);
	Assert(rel->rtekind == RTE_SUBQUERY);

	/*
	 * 从子查询的 Path 递归建立 Plan。由于正在进入
	 * 不同的规划器上下文（subroot），递归到 create_plan，而不是
	 * create_plan_recurse。
	 */
	subplan = create_plan(rel->subroot, best_path->subpath);

	/* 把子句排成最佳执行顺序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* 把任何外层关系变量换成嵌套循环参数 */
	if (best_path->path.param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		process_subquery_nestloop_params(root,
										 rel->subplan_params);
	}

	scan_plan = make_subqueryscan(tlist,
								  scan_clauses,
								  scan_relid,
								  subplan);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * create_functionscan_plan
 *	 返回 'best_path' 所扫描的基关系的函数扫描计划，
 *	 限制子句为 'scan_clauses'，目标列表为 'tlist'。
 */
static FunctionScan *
create_functionscan_plan(PlannerInfo *root, Path *best_path,
						 List *tlist, List *scan_clauses)
{
	FunctionScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	List	   *functions;

	/* 它应当是一个函数基关系…… */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_FUNCTION);
	functions = rte->functions;

	/* 把子句排成最佳执行顺序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* 把任何外层关系变量换成嵌套循环参数 */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		/* 函数表达式里也可能含有嵌套循环参数 */
		functions = (List *) replace_nestloop_params(root, (Node *) functions);
	}

	scan_plan = make_functionscan(tlist, scan_clauses, scan_relid,
								  functions, rte->funcordinality);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_tablefuncscan_plan
 *	 返回 'best_path' 所扫描的基关系的 tablefunc 扫描计划，
 *	 限制子句为 'scan_clauses'，目标列表为 'tlist'。
 */
static TableFuncScan *
create_tablefuncscan_plan(PlannerInfo *root, Path *best_path,
						  List *tlist, List *scan_clauses)
{
	TableFuncScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	TableFunc  *tablefunc;

	/* 它应当是一个函数基关系…… */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_TABLEFUNC);
	tablefunc = rte->tablefunc;

	/* 把子句排成最佳执行顺序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* 把任何外层关系变量换成嵌套循环参数 */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		/* 函数表达式里也可能含有嵌套循环参数 */
		tablefunc = (TableFunc *) replace_nestloop_params(root, (Node *) tablefunc);
	}

	scan_plan = make_tablefuncscan(tlist, scan_clauses, scan_relid,
								   tablefunc);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_valuesscan_plan
 *	 返回 'best_path' 所扫描的基关系的 values 扫描计划，
 *	 限制子句为 'scan_clauses'，目标列表为 'tlist'。
 */
static ValuesScan *
create_valuesscan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses)
{
	ValuesScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	List	   *values_lists;

	/* 它应当是一个 values 基关系…… */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_VALUES);
	values_lists = rte->values_lists;

	/* 把子句排成最佳执行顺序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* 把任何外层关系变量换成嵌套循环参数 */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		/* values 列表里也可能含有嵌套循环参数 */
		values_lists = (List *)
			replace_nestloop_params(root, (Node *) values_lists);
	}

	scan_plan = make_valuesscan(tlist, scan_clauses, scan_relid,
								values_lists);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_ctescan_plan
 *	 返回 'best_path' 所扫描的基关系的 CTE 扫描计划，
 *	 限制子句为 'scan_clauses'，目标列表为 'tlist'。
 */
static CteScan *
create_ctescan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses)
{
	CteScan    *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	SubPlan    *ctesplan = NULL;
	int			plan_id;
	int			cte_param_id;
	PlannerInfo *cteroot;
	Index		levelsup;
	int			ndx;
	ListCell   *lc;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_CTE);
	Assert(!rte->self_reference);

	/*
	 * 找到被引用的 CTE，并定位先前为它建立的 SubPlan。
	 */
	levelsup = rte->ctelevelsup;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* 不应当发生 */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}

	/*
	 * 注意：若仍在规划这些 CTEs（也就是说，这是来自另一个 CTE 的侧面引用），
	 * cte_plan_ids 可以比 cteList 短。
	 * 因此这里不得用 forboth。
	 */
	ndx = 0;
	foreach(lc, cteroot->parse->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (strcmp(cte->ctename, rte->ctename) == 0)
			break;
		ndx++;
	}
	if (lc == NULL)				/* 不应当发生 */
		elog(ERROR, "could not find CTE \"%s\"", rte->ctename);
	if (ndx >= list_length(cteroot->cte_plan_ids))
		elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);
	plan_id = list_nth_int(cteroot->cte_plan_ids, ndx);
	if (plan_id <= 0)
		elog(ERROR, "no plan was made for CTE \"%s\"", rte->ctename);
	foreach(lc, cteroot->init_plans)
	{
		ctesplan = (SubPlan *) lfirst(lc);
		if (ctesplan->plan_id == plan_id)
			break;
	}
	if (lc == NULL)				/* 不应当发生 */
		elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);

	/*
	 * 需要该 CTE 的参数 ID，它是该 SubPlan 的
	 * setParam 列表的唯一成员。
	 */
	cte_param_id = linitial_int(ctesplan->setParam);

	/* 把子句排成最佳执行顺序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* 把任何外层关系变量换成嵌套循环参数 */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_ctescan(tlist, scan_clauses, scan_relid,
							 plan_id, cte_param_id);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_namedtuplestorescan_plan
 *	 返回 'best_path' 所扫描的基关系的
 *	元组存储扫描计划，限制子句为 'scan_clauses'，目标列表为
 *	'tlist'。
 */
static NamedTuplestoreScan *
create_namedtuplestorescan_plan(PlannerInfo *root, Path *best_path,
								List *tlist, List *scan_clauses)
{
	NamedTuplestoreScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_NAMEDTUPLESTORE);

	/* 把子句排成最佳执行顺序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* 把任何外层关系变量换成嵌套循环参数 */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_namedtuplestorescan(tlist, scan_clauses, scan_relid,
										 rte->enrname);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_resultscan_plan
 *	 返回 'best_path' 所扫描的 RTE_RESULT 基关系的 Result 计划，
 *	限制子句为 'scan_clauses'，目标列表为
 *	'tlist'。
 */
static Result *
create_resultscan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses)
{
	Result	   *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte PG_USED_FOR_ASSERTS_ONLY;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_RESULT);

	/* 把子句排成最佳执行顺序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* 把任何外层关系变量换成嵌套循环参数 */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_result(tlist, (Node *) scan_clauses, NULL);

	copy_generic_path_info(&scan_plan->plan, best_path);

	return scan_plan;
}

/*
 * create_worktablescan_plan
 *	 返回 'best_path' 所扫描的基关系的工作表扫描计划，
 *	 限制子句为 'scan_clauses'，目标列表为 'tlist'。
 */
static WorkTableScan *
create_worktablescan_plan(PlannerInfo *root, Path *best_path,
						  List *tlist, List *scan_clauses)
{
	WorkTableScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	Index		levelsup;
	PlannerInfo *cteroot;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_CTE);
	Assert(rte->self_reference);

	/*
	 * 需要找到工作表参数 ID，它在处理递归 UNION 的
	 * 那个计划层次里，比 CTE 的来源
	 * 低一层。
	 */
	levelsup = rte->ctelevelsup;
	if (levelsup == 0)			/* 不应当发生 */
		elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	levelsup--;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* 不应当发生 */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}
	if (cteroot->wt_param_id < 0)	/* 不应当发生 */
		elog(ERROR, "could not find param ID for CTE \"%s\"", rte->ctename);

	/* 把子句排成最佳执行顺序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 把 RestrictInfo 列表收成裸表达式；忽略伪常量 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* 把任何外层关系变量换成嵌套循环参数 */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_worktablescan(tlist, scan_clauses, scan_relid,
								   cteroot->wt_param_id);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_foreignscan_plan
 *	 返回 'best_path' 所扫描关系的外部扫描计划，
 *	 限制子句为 'scan_clauses'，目标列表为 'tlist'。
 */
static ForeignScan *
create_foreignscan_plan(PlannerInfo *root, ForeignPath *best_path,
						List *tlist, List *scan_clauses)
{
	ForeignScan *scan_plan;
	RelOptInfo *rel = best_path->path.parent;
	Index		scan_relid = rel->relid;
	Oid			rel_oid = InvalidOid;
	Plan	   *outer_plan = NULL;

	Assert(rel->fdwroutine != NULL);

	/* 若有子路径，就变换它 */
	if (best_path->fdw_outerpath)
		outer_plan = create_plan_recurse(root, best_path->fdw_outerpath,
										 CP_EXACT_TLIST);

	/*
	 * 若扫描的是基关系，就取出它的 OID。（若
	 * 扫描的是连接关系，则无关。）
	 */
	if (scan_relid > 0)
	{
		RangeTblEntry *rte;

		Assert(rel->rtekind == RTE_RELATION);
		rte = planner_rt_fetch(scan_relid, root);
		Assert(rte->rtekind == RTE_RELATION);
		rel_oid = rte->relid;
	}

	/*
	 * 把子句排成最佳执行顺序。先做这个，因为 FDW
	 * 也许比我们有更多信息，并希望调整顺序。
	 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/*
	 * 让 FDW 对限制子句做处理，并
	 * 生成计划节点。注意 FDW 也许会去掉它打算
	 * 在远端执行的限制子句，甚至再加一些（若它
	 * 选了某些连接子句到远端使用，但还想在
	 * 本地重新检查）。
	 */
	scan_plan = rel->fdwroutine->GetForeignPlan(root, rel, rel_oid,
												best_path,
												tlist, scan_clauses,
												outer_plan);

	/* 把代价数据从 Path 复制到 Plan；不必让 FDW 做这件事 */
	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	/* 把要以其身份访问的用户 OID 复制过去；同样不必让 FDW 做这件事 */
	scan_plan->checkAsUser = rel->userid;

	/* 复制外部服务器 OID；同样不必让 FDW 做这件事 */
	scan_plan->fs_server = rel->serverid;

	/*
	 * 同样，复制本次外部扫描所代表的 relid。上层
	 * 关系没有设置 relids，但它覆盖底层扫描/连接所参与的全部关系，
	 * 因此用 root->all_query_rels。
	 */
	if (rel->reloptkind == RELOPT_UPPER_REL)
		scan_plan->fs_relids = root->all_query_rels;
	else
		scan_plan->fs_relids = best_path->path.parent->relids;

	/*
	 * 连接的 relid 集合包含相关的外连接，但 FDWs 也许需要知道
	 * 其中包含哪些基关系。没有
	 * 规划时数据结构的访问，这有点麻烦，因此在这里计算。
	 */
	scan_plan->fs_base_relids = bms_difference(scan_plan->fs_relids,
											   root->outer_join_rels);

	/*
	 * 若这是外部连接，而且为了能下推，不得不
	 * 假定当前用户与查询里显式指名的某个用户相同，
	 * 就把完成的计划标成依赖当前用户。
	 */
	if (rel->useridiscurrent)
		root->glob->dependsOnRole = true;

	/*
	 * 把限定条件、fdw_exprs 和 fdw_recheck_quals 表达式里的
	 * 任何外层关系变量换成嵌套循环参数。放在最后做，这样
	 * FDW 就不必参与。（注意 fdw_exprs 或
	 * fdw_recheck_quals 的某些部分也许来自连接子句，因此事先对
	 * scan_clauses 做这件事行不通。）假定
	 * fdw_scan_tlist 不含这类变量。
	 */
	if (best_path->path.param_info)
	{
		scan_plan->scan.plan.qual = (List *)
			replace_nestloop_params(root, (Node *) scan_plan->scan.plan.qual);
		scan_plan->fdw_exprs = (List *)
			replace_nestloop_params(root, (Node *) scan_plan->fdw_exprs);
		scan_plan->fdw_recheck_quals = (List *)
			replace_nestloop_params(root,
									(Node *) scan_plan->fdw_recheck_quals);
	}

	/*
	 * 若 rel 是基关系，检测是否向该关系请求了任何系统列。
	 * （若 rel 是连接关系，rel->relid 将为
	 * 0，但该关系的目标列表或限制子句里不可能有 relid 为 0 的 Var，
	 * 因此那种情况跳过。注意被连接的基关系里的任何这类
	 * 列都假定包含在
	 * fdw_scan_tlist 里。）这有点凑合，也许有一天
	 * 会消失，因此有意不放进呈现给 FDWs 的 API。
	 */
	scan_plan->fsSystemCol = false;
	if (scan_relid > 0)
	{
		Bitmapset  *attrs_used = NULL;
		ListCell   *lc;
		int			i;

		/*
		 * 首先检查连接或最终输出所需的全部属性。
		 * 注意：必须看 rel 的目标列表，而不是 attr_needed 数据，
		 * 因为不为继承子关系计算 attr_needed。
		 */
		pull_varattnos((Node *) rel->reltarget->exprs, scan_relid, &attrs_used);

		/* 再加上限制子句用到的全部属性。 */
		foreach(lc, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			pull_varattnos((Node *) rinfo->clause, scan_relid, &attrs_used);
		}

		/* 现在，是否向 rel 请求了任何系统列？ */
		for (i = FirstLowInvalidHeapAttributeNumber + 1; i < 0; i++)
		{
			if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber, attrs_used))
			{
				scan_plan->fsSystemCol = true;
				break;
			}
		}

		bms_free(attrs_used);
	}

	return scan_plan;
}

/*
 * create_customscan_plan
 *
 * 把 CustomPath 变换成 Plan。
 */
static CustomScan *
create_customscan_plan(PlannerInfo *root, CustomPath *best_path,
					   List *tlist, List *scan_clauses)
{
	CustomScan *cplan;
	RelOptInfo *rel = best_path->path.parent;
	List	   *custom_plans = NIL;
	ListCell   *lc;

	/* 递归变换子路径。 */
	foreach(lc, best_path->custom_paths)
	{
		Plan	   *plan = create_plan_recurse(root, (Path *) lfirst(lc),
											   CP_EXACT_TLIST);

		custom_plans = lappend(custom_plans, plan);
	}

	/*
	 * 把子句排成最佳执行顺序，尽管自定义扫描
	 * 提供者可以再重排它们。
	 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/*
	 * 调用自定义计划提供者，建立该 CustomPath 所表示的
	 * Plan 节点。
	 */
	cplan = castNode(CustomScan,
					 best_path->methods->PlanCustomPath(root,
														rel,
														best_path,
														tlist,
														scan_clauses,
														custom_plans));

	/*
	 * 把代价数据从 Path 复制到 Plan；不必让自定义计划提供者
	 * 做这件事
	 */
	copy_generic_path_info(&cplan->scan.plan, &best_path->path);

	/* 同样，复制本次自定义扫描所代表的 relid */
	cplan->custom_relids = best_path->path.parent->relids;

	/*
	 * 把限定条件和 custom_exprs 表达式里的
	 * 任何外层关系变量换成嵌套循环参数。放在最后做，这样自定义计划
	 * 提供者就不必参与。（注意 custom_exprs 的某些部分
	 * 也许来自连接子句，因此事先对
	 * scan_clauses 做这件事行不通。）假定 custom_scan_tlist 不含
	 * 这类变量。
	 */
	if (best_path->path.param_info)
	{
		cplan->scan.plan.qual = (List *)
			replace_nestloop_params(root, (Node *) cplan->scan.plan.qual);
		cplan->custom_exprs = (List *)
			replace_nestloop_params(root, (Node *) cplan->custom_exprs);
	}

	return cplan;
}


/*****************************************************************************
 *
 *	JOIN METHODS（连接方法）
 *
 *****************************************************************************/

static NestLoop *
create_nestloop_plan(PlannerInfo *root,
					 NestPath *best_path)
{
	NestLoop   *join_plan;
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	List	   *tlist = build_path_tlist(root, &best_path->jpath.path);
	List	   *joinrestrictclauses = best_path->jpath.joinrestrictinfo;
	List	   *joinclauses;
	List	   *otherclauses;
	Relids		outerrelids;
	List	   *nestParams;
	Relids		saveOuterRels = root->curOuterRels;

	/* NestLoop 可以做投影，因此不必对子 tlist 挑剔 */
	outer_plan = create_plan_recurse(root, best_path->jpath.outerjoinpath, 0);

	/* 对嵌套循环，把外侧 relid 放进内侧的 curOuterRels */
	root->curOuterRels = bms_union(root->curOuterRels,
								   best_path->jpath.outerjoinpath->parent->relids);

	inner_plan = create_plan_recurse(root, best_path->jpath.innerjoinpath, 0);

	/* 恢复 curOuterRels */
	bms_free(root->curOuterRels);
	root->curOuterRels = saveOuterRels;

	/* 把连接限定子句排成最佳执行顺序 */
	joinrestrictclauses = order_qual_clauses(root, joinrestrictclauses);

	/* 取得连接限定子句（普通表达式形式） */
	/* 这里忽略任何伪常量子句 */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		extract_actual_join_clauses(joinrestrictclauses,
									best_path->jpath.path.parent->relids,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* 对内连接，可以一视同仁地对待全部子句 */
		joinclauses = extract_actual_clauses(joinrestrictclauses, false);
		otherclauses = NIL;
	}

	/* 把任何外层关系变量换成嵌套循环参数 */
	if (best_path->jpath.path.param_info)
	{
		joinclauses = (List *)
			replace_nestloop_params(root, (Node *) joinclauses);
		otherclauses = (List *)
			replace_nestloop_params(root, (Node *) otherclauses);
	}

	/*
	 * 识别应当由本连接节点提供的任何嵌套循环参数，
	 * 并把它们从 root->curOuterParams 去掉。
	 */
	outerrelids = best_path->jpath.outerjoinpath->parent->relids;
	nestParams = identify_current_nestloop_params(root, outerrelids);

	join_plan = make_nestloop(tlist,
							  joinclauses,
							  otherclauses,
							  nestParams,
							  outer_plan,
							  inner_plan,
							  best_path->jpath.jointype,
							  best_path->jpath.inner_unique);

	copy_generic_path_info(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}

static MergeJoin *
create_mergejoin_plan(PlannerInfo *root,
					  MergePath *best_path)
{
	MergeJoin  *join_plan;
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	List	   *tlist = build_path_tlist(root, &best_path->jpath.path);
	List	   *joinclauses;
	List	   *otherclauses;
	List	   *mergeclauses;
	List	   *outerpathkeys;
	List	   *innerpathkeys;
	int			nClauses;
	Oid		   *mergefamilies;
	Oid		   *mergecollations;
	int		   *mergestrategies;
	bool	   *mergenullsfirst;
	PathKey    *opathkey;
	EquivalenceClass *opeclass;
	int			i;
	ListCell   *lc;
	ListCell   *lop;
	ListCell   *lip;
	Path	   *outer_path = best_path->jpath.outerjoinpath;
	Path	   *inner_path = best_path->jpath.innerjoinpath;

	/*
	 * MergeJoin 可以做投影，因此不必要求输入给出精确 tlist。
	 * 不过，若打算对某个输入的结果排序，最好
	 * 要求小的 tlist，这样就不会排序超出
	 * 必要的数据。
	 */
	outer_plan = create_plan_recurse(root, best_path->jpath.outerjoinpath,
									 (best_path->outersortkeys != NIL) ? CP_SMALL_TLIST : 0);

	inner_plan = create_plan_recurse(root, best_path->jpath.innerjoinpath,
									 (best_path->innersortkeys != NIL) ? CP_SMALL_TLIST : 0);

	/* 把连接限定子句排成最佳执行顺序 */
	/* 注意：不要重排 mergeclauses */
	joinclauses = order_qual_clauses(root, best_path->jpath.joinrestrictinfo);

	/* 取得连接限定子句（普通表达式形式） */
	/* 这里忽略任何伪常量子句 */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		extract_actual_join_clauses(joinclauses,
									best_path->jpath.path.parent->relids,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* 对内连接，可以一视同仁地对待全部子句 */
		joinclauses = extract_actual_clauses(joinclauses, false);
		otherclauses = NIL;
	}

	/*
	 * 从连接限定子句列表里去掉 mergeclauses，留下
	 * 必须作为 qpqual 检查的限定列表。
	 */
	mergeclauses = get_actual_clauses(best_path->path_mergeclauses);
	joinclauses = list_difference(joinclauses, mergeclauses);

	/*
	 * 把任何外层关系变量换成嵌套循环参数。
	 * mergeclauses 里不应当有任何这类变量。
	 */
	if (best_path->jpath.path.param_info)
	{
		joinclauses = (List *)
			replace_nestloop_params(root, (Node *) joinclauses);
		otherclauses = (List *)
			replace_nestloop_params(root, (Node *) otherclauses);
	}

	/*
	 * 若需要，重排 mergeclauses，使外侧变量总在
	 * 左边；用正确的 outer_is_left 状态
	 * 标记这些 mergeclause 的 restrictinfo。
	 */
	mergeclauses = get_switched_clauses(best_path->path_mergeclauses,
										best_path->jpath.outerjoinpath->parent->relids);

	/*
	 * 若必要，为外侧和内侧路径建立显式排序节点。
	 */
	if (best_path->outersortkeys)
	{
		Relids		outer_relids = outer_path->parent->relids;
		Sort	   *sort = make_sort_from_pathkeys(outer_plan,
												   best_path->outersortkeys,
												   outer_relids);

		label_sort_with_costsize(root, sort, -1.0);
		outer_plan = (Plan *) sort;
		outerpathkeys = best_path->outersortkeys;
	}
	else
		outerpathkeys = best_path->jpath.outerjoinpath->pathkeys;

	if (best_path->innersortkeys)
	{
		Relids		inner_relids = inner_path->parent->relids;
		Sort	   *sort = make_sort_from_pathkeys(inner_plan,
												   best_path->innersortkeys,
												   inner_relids);

		label_sort_with_costsize(root, sort, -1.0);
		inner_plan = (Plan *) sort;
		innerpathkeys = best_path->innersortkeys;
	}
	else
		innerpathkeys = best_path->jpath.innerjoinpath->pathkeys;

	/*
	 * 若指定了，就加一个物化节点，使内侧计划不必
	 * 处理 mark/restore。
	 */
	if (best_path->materialize_inner)
	{
		Plan	   *matplan = (Plan *) make_material(inner_plan);

		/*
		 * 假定物化不会溢出到磁盘，因此
		 * 每个元组只收取 cpu_operator_cost。（让这个估计与
		 * final_cost_mergejoin 保持同步。）
		 */
		copy_plan_costsize(matplan, inner_plan);
		matplan->total_cost += cpu_operator_cost * matplan->plan_rows;

		inner_plan = matplan;
	}

	/*
	 * 计算执行器需要的 opfamily/collation/strategy/nullsfirst 数组。
	 * 信息在两个输入的路径键里，但
	 * 必须小心 mergeclause 共享一条
	 * 路径键的可能，以及内侧路径键的顺序与
	 * mergeclauses 不一致的可能。
	 */
	nClauses = list_length(mergeclauses);
	Assert(nClauses == list_length(best_path->path_mergeclauses));
	mergefamilies = (Oid *) palloc(nClauses * sizeof(Oid));
	mergecollations = (Oid *) palloc(nClauses * sizeof(Oid));
	mergestrategies = (int *) palloc(nClauses * sizeof(int));
	mergenullsfirst = (bool *) palloc(nClauses * sizeof(bool));

	opathkey = NULL;
	opeclass = NULL;
	lop = list_head(outerpathkeys);
	lip = list_head(innerpathkeys);
	i = 0;
	foreach(lc, best_path->path_mergeclauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		EquivalenceClass *oeclass;
		EquivalenceClass *ieclass;
		PathKey    *ipathkey = NULL;
		EquivalenceClass *ipeclass = NULL;
		bool		first_inner_match = false;

		/* 从 mergeclause 取出外侧/内侧等价类 */
		if (rinfo->outer_is_left)
		{
			oeclass = rinfo->left_ec;
			ieclass = rinfo->right_ec;
		}
		else
		{
			oeclass = rinfo->right_ec;
			ieclass = rinfo->left_ec;
		}
		Assert(oeclass != NULL);
		Assert(ieclass != NULL);

		/*
		 * 必须通过匹配等价类，识别与本子句相关联的路径键元素
		 * （这应当给出唯一匹配，因为
		 * 路径键列表应当是规范的）。典型情况下，归并
		 * 子句与路径键一一对应，但处理
		 * 部分冗余的查询条件时，事情更复杂。
		 *
		 * lop 和 lip 指向第一个尚未匹配的路径键元素。
		 * 若它们为 NULL，则全部路径键元素都已匹配。
		 *
		 * 外侧路径键的顺序应当与 mergeclauses 一致，
		 * 这是构造出来的（见 find_mergeclauses_for_outer_pathkeys()）。
		 * 同一条外侧路径键可以有多于一条 mergeclause，但
		 * 不能把任何路径键整个跳过。
		 */
		if (oeclass != opeclass)	/* 多次匹配没什么意思 */
		{
			/* 与当前 opathkey 不匹配，因此必须匹配下一条 */
			if (lop == NULL)
				elog(ERROR, "outer pathkeys do not match mergeclauses");
			opathkey = (PathKey *) lfirst(lop);
			opeclass = opathkey->pk_eclass;
			lop = lnext(outerpathkeys, lop);
			if (oeclass != opeclass)
				elog(ERROR, "outer pathkeys do not match mergeclauses");
		}

		/*
		 * 内侧路径键同样不应当有被跳过的键，但
		 * 若有冗余路径键，一条 mergeclause 可能引用更早的某条内侧
		 * 路径键。例如也许有
		 * "o.a = i.x AND o.b = i.y AND o.c = i.x" 这样的 mergeclause。
		 * 蕴含的内侧顺序于是是 "ORDER BY x, y, x"，但路径键
		 * 机制会把第二次按 x 排序当作冗余丢掉，而这段代码
		 * 必须应付。
		 *
		 * 蕴含的内侧关系顺序也可能像
		 * "ORDER BY x, y, x DESC"。我们仍把 x 的第二次出现当作
		 * 冗余丢掉；但这意味着冗余
		 * 内侧路径键的排序方向不应当被认为有意义。因此必须
		 * 检测这是不是匹配某条内侧路径键的第一条子句。
		 */
		if (lip)
		{
			ipathkey = (PathKey *) lfirst(lip);
			ipeclass = ipathkey->pk_eclass;
			if (ieclass == ipeclass)
			{
				/* 成功第一次匹配到这条内侧路径键 */
				lip = lnext(innerpathkeys, lip);
				first_inner_match = true;
			}
		}
		if (!first_inner_match)
		{
			/* 冗余子句……必须匹配 lip 之前的某个东西 */
			ListCell   *l2;

			foreach(l2, innerpathkeys)
			{
				if (l2 == lip)
					break;
				ipathkey = (PathKey *) lfirst(l2);
				ipeclass = ipathkey->pk_eclass;
				if (ieclass == ipeclass)
					break;
			}
			if (ieclass != ipeclass)
				elog(ERROR, "inner pathkeys do not match mergeclauses");
		}

		/*
		 * 路径键在 opfamily 和
		 * collation（影响等值）上应当总是彼此匹配，但若考虑的是
		 * 冗余的内侧路径键，其排序方向也许不匹配。
		 * 这种情况下可以忽略内侧路径键的排序方向，改用
		 * 外侧的。（实际上是在对执行器谎报这根内侧列的
		 * 排序方向，但这没关系，因为
		 * 运行时的行比较只有在更早那一列（含同一个等价类）
		 * 相等时才会到达这一列。
		 * 对更早那一列取给定值的内侧行范围内，这一列
		 * 只能有一个值，因此我们把这一列想象成
		 * 哪种顺序都没关系。）但
		 * 非冗余的内侧路径键最好也与外侧的顺序匹配。
		 */
		if (opathkey->pk_opfamily != ipathkey->pk_opfamily ||
			opathkey->pk_eclass->ec_collation != ipathkey->pk_eclass->ec_collation)
			elog(ERROR, "left and right pathkeys do not match in mergejoin");
		if (first_inner_match &&
			(opathkey->pk_strategy != ipathkey->pk_strategy ||
			 opathkey->pk_nulls_first != ipathkey->pk_nulls_first))
			elog(ERROR, "left and right pathkeys do not match in mergejoin");

		/* 好，把信息保存给执行器 */
		mergefamilies[i] = opathkey->pk_opfamily;
		mergecollations[i] = opathkey->pk_eclass->ec_collation;
		mergestrategies[i] = opathkey->pk_strategy;
		mergenullsfirst[i] = opathkey->pk_nulls_first;
		i++;
	}

	/*
	 * 注意：若还有额外的路径键元素（也就是说
	 * 这里 lop 或 lip 不是 NULL），并不是错误。输入路径的排序也许比
	 * 当前归并连接所需的更好。
	 */

	/*
	 * 现在可以建立归并连接节点了。
	 */
	join_plan = make_mergejoin(tlist,
							   joinclauses,
							   otherclauses,
							   mergeclauses,
							   mergefamilies,
							   mergecollations,
							   mergestrategies,
							   mergenullsfirst,
							   outer_plan,
							   inner_plan,
							   best_path->jpath.jointype,
							   best_path->jpath.inner_unique,
							   best_path->skip_mark_restore);

	/* 排序和物化步骤的代价已经包含在路径代价里 */
	copy_generic_path_info(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}

static HashJoin *
create_hashjoin_plan(PlannerInfo *root,
					 HashPath *best_path)
{
	HashJoin   *join_plan;
	Hash	   *hash_plan;
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	List	   *tlist = build_path_tlist(root, &best_path->jpath.path);
	List	   *joinclauses;
	List	   *otherclauses;
	List	   *hashclauses;
	List	   *hashoperators = NIL;
	List	   *hashcollations = NIL;
	List	   *inner_hashkeys = NIL;
	List	   *outer_hashkeys = NIL;
	Oid			skewTable = InvalidOid;
	AttrNumber	skewColumn = InvalidAttrNumber;
	bool		skewInherit = false;
	ListCell   *lc;

	/*
	 * HashJoin 可以做投影，因此不必要求输入给出精确 tlist。
	 * 不过，最好要求内侧给出小的 tlist，
	 * 这样就不会存储超出必要的数据。同样，若
	 * 预期会分批，就要求外侧给出小的 tlist，这样
	 * 就不会把额外数据放进外侧批次文件。
	 */
	outer_plan = create_plan_recurse(root, best_path->jpath.outerjoinpath,
									 (best_path->num_batches > 1) ? CP_SMALL_TLIST : 0);

	inner_plan = create_plan_recurse(root, best_path->jpath.innerjoinpath,
									 CP_SMALL_TLIST);

	/* 把连接限定子句排成最佳执行顺序 */
	joinclauses = order_qual_clauses(root, best_path->jpath.joinrestrictinfo);
	/* 给哈希子句排序没有意义…… */

	/* 取得连接限定子句（普通表达式形式） */
	/* 这里忽略任何伪常量子句 */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		extract_actual_join_clauses(joinclauses,
									best_path->jpath.path.parent->relids,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* 对内连接，可以一视同仁地对待全部子句 */
		joinclauses = extract_actual_clauses(joinclauses, false);
		otherclauses = NIL;
	}

	/*
	 * 从连接限定子句列表里去掉 hashclauses，留下
	 * 必须作为 qpqual 检查的限定列表。
	 */
	hashclauses = get_actual_clauses(best_path->path_hashclauses);
	joinclauses = list_difference(joinclauses, hashclauses);

	/*
	 * 把任何外层关系变量换成嵌套循环参数。
	 * hashclauses 里不应当有任何这类变量。
	 */
	if (best_path->jpath.path.param_info)
	{
		joinclauses = (List *)
			replace_nestloop_params(root, (Node *) joinclauses);
		otherclauses = (List *)
			replace_nestloop_params(root, (Node *) otherclauses);
	}

	/*
	 * 若需要，重排 hashclauses，使外侧变量总在
	 * 左边。
	 */
	hashclauses = get_switched_clauses(best_path->path_hashclauses,
									   best_path->jpath.outerjoinpath->parent->relids);

	/*
	 * 若只有一条连接子句，且能把外侧变量
	 * 识别为简单列引用，就提供它的标识，以备
	 * 偏斜优化使用。（注意：原则上多条连接子句也能做偏斜优化，
	 * 但必须能确定
	 * 外侧值最常见的组合，而目前没有
	 * 足够的统计信息。）
	 */
	if (list_length(hashclauses) == 1)
	{
		OpExpr	   *clause = (OpExpr *) linitial(hashclauses);
		Node	   *node;

		Assert(is_opclause(clause));
		node = (Node *) linitial(clause->args);
		if (IsA(node, RelabelType))
			node = (Node *) ((RelabelType *) node)->arg;
		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;
			RangeTblEntry *rte;

			rte = root->simple_rte_array[var->varno];
			if (rte->rtekind == RTE_RELATION)
			{
				skewTable = rte->relid;
				skewColumn = var->varattno;
				skewInherit = rte->inh;
			}
		}
	}

	/*
	 * 收集与哈希相关的信息。被哈希的表达式被
	 * 拆成外侧/内侧表达式，以便分开计算
	 * （内侧表达式经 Hash 用来建哈希表，
	 * 外侧表达式用来在哈希表里查找来自 HashJoin 外侧
	 * 计划的元组）。也收集建哈希表所需的
	 * 操作符信息。
	 */
	foreach(lc, hashclauses)
	{
		OpExpr	   *hclause = lfirst_node(OpExpr, lc);

		hashoperators = lappend_oid(hashoperators, hclause->opno);
		hashcollations = lappend_oid(hashcollations, hclause->inputcollid);
		outer_hashkeys = lappend(outer_hashkeys, linitial(hclause->args));
		inner_hashkeys = lappend(inner_hashkeys, lsecond(hclause->args));
	}

	/*
	 * 建立哈希节点和哈希连接节点。
	 */
	hash_plan = make_hash(inner_plan,
						  inner_hashkeys,
						  skewTable,
						  skewColumn,
						  skewInherit);

	/*
	 * 把 Hash 节点的启动代价和总代价设成输入
	 * 计划的总代价；这只影响 EXPLAIN 的显示，不影响决策。
	 */
	copy_plan_costsize(&hash_plan->plan, inner_plan);
	hash_plan->plan.startup_cost = hash_plan->plan.total_cost;

	/*
	 * 若是并行感知的，执行器还得有一份对全部
	 * 参与者预期返回的总行数的估计，以便确定
	 * 共享哈希表的大小。
	 */
	if (best_path->jpath.path.parallel_aware)
	{
		hash_plan->plan.parallel_aware = true;
		hash_plan->rows_total = best_path->inner_rows_total;
	}

	join_plan = make_hashjoin(tlist,
							  joinclauses,
							  otherclauses,
							  hashclauses,
							  hashoperators,
							  hashcollations,
							  outer_hashkeys,
							  outer_plan,
							  (Plan *) hash_plan,
							  best_path->jpath.jointype,
							  best_path->jpath.inner_unique);

	copy_generic_path_info(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}


/*****************************************************************************
 *
 *	SUPPORTING ROUTINES（支撑例程）
 *
 *****************************************************************************/

/*
 * replace_nestloop_params
 *	  把给定表达式里的外层关系 Var 和 PlaceHolderVars
 *	  换成嵌套循环 Param
 *
 * 属于 root->curOuterRels 所标识的那些关系的全部 Var 和 PlaceHolderVars
 * 都被换成 Param；若 root->curOuterParams 里还没有，
 * 就添加项。
 */
static Node *
replace_nestloop_params(PlannerInfo *root, Node *expr)
{
	/* 树遍历不需要准备，那就开始 */
	return replace_nestloop_params_mutator(expr, root);
}

static Node *
replace_nestloop_params_mutator(Node *node, PlannerInfo *root)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/* 到这时，上层 Var 早该没了 */
		Assert(var->varlevelsup == 0);
		/* 若不必替换，可以直接返回未改的 Var */
		if (IS_SPECIAL_VARNO(var->varno) ||
			!bms_is_member(var->varno, root->curOuterRels))
			return node;
		/* 把该 Var 换成嵌套循环 Param */
		return (Node *) replace_nestloop_param_var(root, var);
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* 到这时，上层 PlaceHolderVars 早该没了 */
		Assert(phv->phlevelsup == 0);

		/* 检查是否需要替换该 PHV */
		if (!bms_is_subset(find_placeholder_info(root, phv)->ph_eval_at,
						   root->curOuterRels))
		{
			/*
			 * 不能替换整个 PHV，但也许仍需要
			 * 替换其表达式里的 Var 或 PHVs，以防它最终
			 * 真的在这里求值。（它也许在
			 * 本计划节点或某个子节点里求值；后一种情况下其实不必
			 * 在这里处理该表达式，但没有
			 * 足够信息来判断是不是那种情况。）浅拷贝该 PHV
			 * 节点，然后对其表达式递归。
			 *
			 * 注意做完之后，计划树的不同
			 * 部分里，同一个 PHV 的内容也许有不同的
			 * 表示。这没关系，因为 equal() 只
			 * 按 phid/phlevelsup 匹配，因此 setrefs.c 仍能识别
			 * 对同一个 PHV 的下层副本的上层引用。
			 */
			PlaceHolderVar *newphv = makeNode(PlaceHolderVar);

			memcpy(newphv, phv, sizeof(PlaceHolderVar));
			newphv->phexpr = (Expr *)
				replace_nestloop_params_mutator((Node *) phv->phexpr,
												root);
			return (Node *) newphv;
		}
		/* 把该 PlaceHolderVar 换成嵌套循环 Param */
		return (Node *) replace_nestloop_param_placeholdervar(root, phv);
	}
	return expression_tree_mutator(node,
								   replace_nestloop_params_mutator,
								   (void *) root);
}

/*
 * fix_indexqual_references
 *	  把 indexqual 子句调整成执行器的 indexqual
 *	  机制所要的形式。
 *
 * 这里有三件事：
 *	从输入的 IndexClause 列表里选出实际的限定子句，
 *	  并从限定子句里去掉 RestrictInfo 节点。
 *	把任何外层关系的 Var 或 PHV 节点换成嵌套循环 Param。
 *	  （XXX 最终，这个责任应当放到别处？）
 *	索引键必须用 varattno 设成
 *	  索引属性号、而不是原关系属性号的 Var 节点表示。
 *
 * *stripped_indexquals_p 接收实际限定子句的列表。
 *
 * *fixed_indexquals_p 接收调整后的限定列表。这是一份
 * 与原件不共享子结构的副本；万一里面
 * 有子计划，就需要这个（需要子计划树的两份独立副本，否则
 * 事情会出错）。
 */
static void
fix_indexqual_references(PlannerInfo *root, IndexPath *index_path,
						 List **stripped_indexquals_p, List **fixed_indexquals_p)
{
	IndexOptInfo *index = index_path->indexinfo;
	List	   *stripped_indexquals;
	List	   *fixed_indexquals;
	ListCell   *lc;

	stripped_indexquals = fixed_indexquals = NIL;

	foreach(lc, index_path->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		int			indexcol = iclause->indexcol;
		ListCell   *lc2;

		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
			Node	   *clause = (Node *) rinfo->clause;

			stripped_indexquals = lappend(stripped_indexquals, clause);
			clause = fix_indexqual_clause(root, index, indexcol,
										  clause, iclause->indexcols);
			fixed_indexquals = lappend(fixed_indexquals, clause);
		}
	}

	*stripped_indexquals_p = stripped_indexquals;
	*fixed_indexquals_p = fixed_indexquals;
}

/*
 * fix_indexorderby_references
 *	  把 indexorderby 子句调整成执行器的索引
 *	  机制所要的形式。
 *
 * 这是 fix_indexqual_references 的简化版。输入是
 * 裸子句和单独的 indexcol 列表，而不是 IndexClauses。
 */
static List *
fix_indexorderby_references(PlannerInfo *root, IndexPath *index_path)
{
	IndexOptInfo *index = index_path->indexinfo;
	List	   *fixed_indexorderbys;
	ListCell   *lcc,
			   *lci;

	fixed_indexorderbys = NIL;

	forboth(lcc, index_path->indexorderbys, lci, index_path->indexorderbycols)
	{
		Node	   *clause = (Node *) lfirst(lcc);
		int			indexcol = lfirst_int(lci);

		clause = fix_indexqual_clause(root, index, indexcol, clause, NIL);
		fixed_indexorderbys = lappend(fixed_indexorderbys, clause);
	}

	return fixed_indexorderbys;
}

/*
 * fix_indexqual_clause
 *	  把单条 indexqual 子句转换成执行器所要的形式。
 *
 * 这里替换嵌套循环参数，并把索引键变量
 * 或表达式换成索引 Var 节点。
 */
static Node *
fix_indexqual_clause(PlannerInfo *root, IndexOptInfo *index, int indexcol,
					 Node *clause, List *indexcolnos)
{
	/*
	 * 把任何外层关系变量换成嵌套循环参数。
	 *
	 * 这也会复制该子句，因此下面就地修改
	 * 是安全的。
	 */
	clause = replace_nestloop_params(root, clause);

	if (IsA(clause, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) clause;

		/* 把索引键表达式换成索引 Var。 */
		linitial(op->args) = fix_indexqual_operand(linitial(op->args),
												   index,
												   indexcol);
	}
	else if (IsA(clause, RowCompareExpr))
	{
		RowCompareExpr *rc = (RowCompareExpr *) clause;
		ListCell   *lca,
				   *lcai;

		/* 把索引键表达式换成索引 Var。 */
		Assert(list_length(rc->largs) == list_length(indexcolnos));
		forboth(lca, rc->largs, lcai, indexcolnos)
		{
			lfirst(lca) = fix_indexqual_operand(lfirst(lca),
												index,
												lfirst_int(lcai));
		}
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

		/* 把索引键表达式换成索引 Var。 */
		linitial(saop->args) = fix_indexqual_operand(linitial(saop->args),
													 index,
													 indexcol);
	}
	else if (IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;

		/* 把索引键表达式换成索引 Var。 */
		nt->arg = (Expr *) fix_indexqual_operand((Node *) nt->arg,
												 index,
												 indexcol);
	}
	else
		elog(ERROR, "unsupported indexqual type: %d",
			 (int) nodeTag(clause));

	return clause;
}

/*
 * fix_indexqual_operand
 *	  把 indexqual 表达式转换成引用该索引列的 Var。
 *
 * 我们用 varno == INDEX_VAR、且 varattno
 * 等于索引属性号（索引列位置）的 Var 节点表示索引键。
 *
 * 这里大部分代码只是健全性交叉检查：给定的
 * 表达式确实与它所声称的索引列匹配。
 */
static Node *
fix_indexqual_operand(Node *node, IndexOptInfo *index, int indexcol)
{
	Var		   *result;
	int			pos;
	ListCell   *indexpr_item;

	/*
	 * 去掉索引键上任何二进制兼容的 relabel
	 */
	if (IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;

	Assert(indexcol >= 0 && indexcol < index->ncolumns);

	if (index->indexkeys[indexcol] != 0)
	{
		/* 它是简单的索引列 */
		if (IsA(node, Var) &&
			((Var *) node)->varno == index->rel->relid &&
			((Var *) node)->varattno == index->indexkeys[indexcol])
		{
			result = (Var *) copyObject(node);
			result->varno = INDEX_VAR;
			result->varattno = indexcol + 1;
			return (Node *) result;
		}
		else
			elog(ERROR, "index key does not match expected index column");
	}

	/* 它是索引表达式，因此找出并交叉检查该表达式 */
	indexpr_item = list_head(index->indexprs);
	for (pos = 0; pos < index->ncolumns; pos++)
	{
		if (index->indexkeys[pos] == 0)
		{
			if (indexpr_item == NULL)
				elog(ERROR, "too few entries in indexprs list");
			if (pos == indexcol)
			{
				Node	   *indexkey;

				indexkey = (Node *) lfirst(indexpr_item);
				if (indexkey && IsA(indexkey, RelabelType))
					indexkey = (Node *) ((RelabelType *) indexkey)->arg;
				if (equal(node, indexkey))
				{
					result = makeVar(INDEX_VAR, indexcol + 1,
									 exprType(lfirst(indexpr_item)), -1,
									 exprCollation(lfirst(indexpr_item)),
									 0);
					return (Node *) result;
				}
				else
					elog(ERROR, "index key does not match expected index column");
			}
			indexpr_item = lnext(index->indexprs, indexpr_item);
		}
	}

	/* 糟糕…… */
	elog(ERROR, "index key does not match expected index column");
	return NULL;				/* 消除编译器告警 */
}

/*
 * get_switched_clauses
 *	  给定一份归并或哈希连接子句列表（作为 RestrictInfo 节点），
 *	  抽出裸子句，并在需要时重排子句内部的元素，
 *	  使外侧连接变量在左、
 *	  内侧在右。不触碰原来的子句数据结构；
 *	  返回修改后的列表。不过会设置每个 RestrictInfo 里暂时的
 *	  outer_is_left 字段，表明哪一侧是哪一侧。
 */
static List *
get_switched_clauses(List *clauses, Relids outerrelids)
{
	List	   *t_list = NIL;
	ListCell   *l;

	foreach(l, clauses)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);
		OpExpr	   *clause = (OpExpr *) restrictinfo->clause;

		Assert(is_opclause(clause));
		if (bms_is_subset(restrictinfo->right_relids, outerrelids))
		{
			/*
			 * 只复制刚好够交换该
			 * 子句、又不改原列表的那部分结构。可以用
			 * copyObject，但完整深拷贝是杀鸡用牛刀。
			 */
			OpExpr	   *temp = makeNode(OpExpr);

			temp->opno = clause->opno;
			temp->opfuncid = InvalidOid;
			temp->opresulttype = clause->opresulttype;
			temp->opretset = clause->opretset;
			temp->opcollid = clause->opcollid;
			temp->inputcollid = clause->inputcollid;
			temp->args = list_copy(clause->args);
			temp->location = clause->location;
			/* 交换它——注意这会就地修改临时节点。 */
			CommuteOpExpr(temp);
			t_list = lappend(t_list, temp);
			restrictinfo->outer_is_left = false;
		}
		else
		{
			Assert(bms_is_subset(restrictinfo->left_relids, outerrelids));
			t_list = lappend(t_list, clause);
			restrictinfo->outer_is_left = true;
		}
	}
	return t_list;
}

/*
 * order_qual_clauses
 *		给定一份将在同一个
 *		计划节点上求值的限定子句列表，按运行时想检查限定条件的
 *		顺序排序。
 *
 * 查询使用安全屏障限定时，列表里也许有
 * 不同安全级别的限定。较低 security_level 的限定
 * 必须排在较高 security_level 的限定之前，
 * 但可以破例
 * 把防泄漏的限定提前。当安全级别
 * 不强制决定时，我们宁可按估计的
 *
 * 执行代价排序，最便宜的在前。
 * 理想情况下顺序应由执行代价和
 * 选择率共同驱动，但如何同时计入两者并不立刻清楚，
 * 而且估计不确定，决策的可靠性
 * 反正也可疑。因此只按安全级别、然后按
 * 估计的每元组代价排序，并小心在
 *
 * （常常如此）估计相同时不改变顺序。
 * 虽然这对裸子句或 RestrictInfos 都能工作，
 * 应用到 RestrictInfos 上快得多，因为它能复用
 * 缓存在 RestrictInfos 里的代价信息。XXX 对裸子句
 * 情形，也不能应用安全方面的考虑。目前
 * 这没关系，因为可能出现屏障限定的地方不会出现裸子句情形，但最好
 * 把它去掉。
 *
 * 注意：有些调用方传入的列表含有以后会被
 * 去掉的项；这是让本例程见到 RestrictInfos
 * 而不是裸子句的最容易办法。这也是试图在排序里考虑
 * 选择率很可能会做错事的另一个原因。
 */
static List *
order_qual_clauses(PlannerInfo *root, List *clauses)
{
	typedef struct
	{
		Node	   *clause;
		Cost		cost;
		Index		security_level;
	} QualItem;
	int			nitems = list_length(clauses);
	QualItem   *items;
	ListCell   *lc;
	int			i;
	List	   *result;

	/* 0 条或 1 条子句不必费劲 */
	if (nitems <= 1)
		return clauses;

	/*
	 * 把各项和代价收进一个数组。这样若输入不是 RestrictInfos，
	 * 就可避免重复的 cost_qual_eval 工作。
	 */
	items = (QualItem *) palloc(nitems * sizeof(QualItem));
	i = 0;
	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);
		QualCost	qcost;

		cost_qual_eval_node(&qcost, clause, root);
		items[i].clause = clause;
		items[i].cost = qcost.per_tuple;
		if (IsA(clause, RestrictInfo))
		{
			RestrictInfo *rinfo = (RestrictInfo *) clause;

			/*
			 * 若某条子句防泄漏，就不必受
			 * 其名义安全级别约束。若它也相当便宜
			 * （这里定义为 cpu_operator_cost 的 10 倍），就假装它的
			 * security_level 为 0，这样它可以排到
			 * 更低安全级别、更贵的限定之前。当然，那
			 * 也会迫使它排到自己
			 * 安全级别里更便宜的限定之前，这不太好，但可以靠
			 * 应用代价上限来减轻该风险。
			 */
			if (rinfo->leakproof && items[i].cost < 10 * cpu_operator_cost)
				items[i].security_level = 0;
			else
				items[i].security_level = rinfo->security_level;
		}
		else
			items[i].security_level = 0;
		i++;
	}

	/*
	 * 排序。不用 qsort()，因为它对
	 * 相等键不保证稳定。预期的项数少到
	 * 简单插入排序就够了。
	 */
	for (i = 1; i < nitems; i++)
	{
		QualItem	newitem = items[i];
		int			j;

		/* 把 newitem 插入已经排好序的子数组 */
		for (j = i; j > 0; j--)
		{
			QualItem   *olditem = &items[j - 1];

			if (newitem.security_level > olditem->security_level ||
				(newitem.security_level == olditem->security_level &&
				 newitem.cost >= olditem->cost))
				break;
			items[j] = *olditem;
		}
		items[j] = newitem;
	}

	/* 转换回列表 */
	result = NIL;
	for (i = 0; i < nitems; i++)
		result = lappend(result, items[i].clause);

	return result;
}

/*
 * 把代价和大小信息从 Path 节点复制到由它建立的 Plan 节点。
 * 执行器通常不用这些信息，但 EXPLAIN 需要。
 * 也复制与并行相关的标志，执行器 *会* 用到。
 */
static void
copy_generic_path_info(Plan *dest, Path *src)
{
	dest->startup_cost = src->startup_cost;
	dest->total_cost = src->total_cost;
	dest->plan_rows = src->rows;
	dest->plan_width = src->pathtarget->width;
	dest->parallel_aware = src->parallel_aware;
	dest->parallel_safe = src->parallel_safe;
}

/*
 * 把代价和大小信息从更低的计划节点复制到插入的节点。
 * （多数调用方复制后再改这些信息。）
 */
static void
copy_plan_costsize(Plan *dest, Plan *src)
{
	dest->startup_cost = src->startup_cost;
	dest->total_cost = src->total_cost;
	dest->plan_rows = src->plan_rows;
	dest->plan_width = src->plan_width;
	/* 假定插入的节点不是并行感知的。 */
	dest->parallel_aware = false;
	/* 若子计划是并行安全的，就假定插入的节点也是。 */
	dest->parallel_safe = src->parallel_safe;
}

/*
 * 本文件有些地方建立的 Sort 节点没有直接
 * 对应的 Path 节点。排序的代价已经、或应当已经
 * 包含在我们据以工作的 Path 节点的代价里，但既然它
 * 没有拆出来，就得用 cost_sort() 重新算。这只是
 * 为了让 EXPLAIN 把 Sort 节点标得好看。
 *
 * limit_tuples 与 cost_sort 相同（尤其是，若没有限制就传 -1）
 */
static void
label_sort_with_costsize(PlannerInfo *root, Sort *plan, double limit_tuples)
{
	Plan	   *lefttree = plan->plan.lefttree;
	Path		sort_path;		/* cost_sort 结果的占位 */

	/*
	 * 本函数不必处理 IncrementalSort 计划，因为
	 * 它们只从对应的 Path 节点建立。
	 */
	Assert(IsA(plan, Sort));

	cost_sort(&sort_path, root, NIL,
			  lefttree->total_cost,
			  lefttree->plan_rows,
			  lefttree->plan_width,
			  0.0,
			  work_mem,
			  limit_tuples);
	plan->plan.startup_cost = sort_path.startup_cost;
	plan->plan.total_cost = sort_path.total_cost;
	plan->plan.plan_rows = lefttree->plan_rows;
	plan->plan.plan_width = lefttree->plan_width;
	plan->plan.parallel_aware = false;
	plan->plan.parallel_safe = lefttree->parallel_safe;
}

/*
 * bitmap_subplan_mark_shared
 *	 设置位图子计划的 isshared 标志，使它在
 *	 共享内存里建立。
 */
static void
bitmap_subplan_mark_shared(Plan *plan)
{
	if (IsA(plan, BitmapAnd))
		bitmap_subplan_mark_shared(linitial(((BitmapAnd *) plan)->bitmapplans));
	else if (IsA(plan, BitmapOr))
	{
		((BitmapOr *) plan)->isshared = true;
		bitmap_subplan_mark_shared(linitial(((BitmapOr *) plan)->bitmapplans));
	}
	else if (IsA(plan, BitmapIndexScan))
		((BitmapIndexScan *) plan)->isshared = true;
	else
		elog(ERROR, "unrecognized node type: %d", nodeTag(plan));
}

/*****************************************************************************
 *
 *	PLAN NODE BUILDING ROUTINES（计划节点建立例程）
 *
 * 一般这些函数不接收原来的 Path，因此
 * 留给调用方根据 Path 填入代价/宽度字段，
 * 通常通过调用 copy_generic_path_info()。这个约定
 * 有点历史原因，但它确实支持上面少数地方：我们
 * 建立计划节点时并没有精确对应的 Path 节点。任何情况下
 * 这些函数都不应当自己算代价，
 * 因为那会与建立 Path 时做的计算重复。
 *
 *****************************************************************************/

static SeqScan *
make_seqscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid)
{
	SeqScan    *node = makeNode(SeqScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;

	return node;
}

static SampleScan *
make_samplescan(List *qptlist,
				List *qpqual,
				Index scanrelid,
				TableSampleClause *tsc)
{
	SampleScan *node = makeNode(SampleScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tablesample = tsc;

	return node;
}

static IndexScan *
make_indexscan(List *qptlist,
			   List *qpqual,
			   Index scanrelid,
			   Oid indexid,
			   List *indexqual,
			   List *indexqualorig,
			   List *indexorderby,
			   List *indexorderbyorig,
			   List *indexorderbyops,
			   ScanDirection indexscandir)
{
	IndexScan  *node = makeNode(IndexScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->indexqualorig = indexqualorig;
	node->indexorderby = indexorderby;
	node->indexorderbyorig = indexorderbyorig;
	node->indexorderbyops = indexorderbyops;
	node->indexorderdir = indexscandir;

	return node;
}

static IndexOnlyScan *
make_indexonlyscan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   Oid indexid,
				   List *indexqual,
				   List *recheckqual,
				   List *indexorderby,
				   List *indextlist,
				   ScanDirection indexscandir)
{
	IndexOnlyScan *node = makeNode(IndexOnlyScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->recheckqual = recheckqual;
	node->indexorderby = indexorderby;
	node->indextlist = indextlist;
	node->indexorderdir = indexscandir;

	return node;
}

static BitmapIndexScan *
make_bitmap_indexscan(Index scanrelid,
					  Oid indexid,
					  List *indexqual,
					  List *indexqualorig)
{
	BitmapIndexScan *node = makeNode(BitmapIndexScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = NIL;		/* 未使用 */
	plan->qual = NIL;			/* 未使用 */
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->indexqualorig = indexqualorig;

	return node;
}

static BitmapHeapScan *
make_bitmap_heapscan(List *qptlist,
					 List *qpqual,
					 Plan *lefttree,
					 List *bitmapqualorig,
					 Index scanrelid)
{
	BitmapHeapScan *node = makeNode(BitmapHeapScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->bitmapqualorig = bitmapqualorig;

	return node;
}

static TidScan *
make_tidscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid,
			 List *tidquals)
{
	TidScan    *node = makeNode(TidScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tidquals = tidquals;

	return node;
}

static TidRangeScan *
make_tidrangescan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  List *tidrangequals)
{
	TidRangeScan *node = makeNode(TidRangeScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tidrangequals = tidrangequals;

	return node;
}

static SubqueryScan *
make_subqueryscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  Plan *subplan)
{
	SubqueryScan *node = makeNode(SubqueryScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->subplan = subplan;
	node->scanstatus = SUBQUERY_SCAN_UNKNOWN;

	return node;
}

static FunctionScan *
make_functionscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  List *functions,
				  bool funcordinality)
{
	FunctionScan *node = makeNode(FunctionScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->functions = functions;
	node->funcordinality = funcordinality;

	return node;
}

static TableFuncScan *
make_tablefuncscan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   TableFunc *tablefunc)
{
	TableFuncScan *node = makeNode(TableFuncScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tablefunc = tablefunc;

	return node;
}

static ValuesScan *
make_valuesscan(List *qptlist,
				List *qpqual,
				Index scanrelid,
				List *values_lists)
{
	ValuesScan *node = makeNode(ValuesScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->values_lists = values_lists;

	return node;
}

static CteScan *
make_ctescan(List *qptlist,
			 List *qpqual,
			 Index scanrelid,
			 int ctePlanId,
			 int cteParam)
{
	CteScan    *node = makeNode(CteScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->ctePlanId = ctePlanId;
	node->cteParam = cteParam;

	return node;
}

static NamedTuplestoreScan *
make_namedtuplestorescan(List *qptlist,
						 List *qpqual,
						 Index scanrelid,
						 char *enrname)
{
	NamedTuplestoreScan *node = makeNode(NamedTuplestoreScan);
	Plan	   *plan = &node->scan.plan;

	/* 代价应由调用方填入 */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->enrname = enrname;

	return node;
}

static WorkTableScan *
make_worktablescan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   int wtParam)
{
	WorkTableScan *node = makeNode(WorkTableScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->wtParam = wtParam;

	return node;
}

ForeignScan *
make_foreignscan(List *qptlist,
				 List *qpqual,
				 Index scanrelid,
				 List *fdw_exprs,
				 List *fdw_private,
				 List *fdw_scan_tlist,
				 List *fdw_recheck_quals,
				 Plan *outer_plan)
{
	ForeignScan *node = makeNode(ForeignScan);
	Plan	   *plan = &node->scan.plan;

	/* 代价将由 create_foreignscan_plan 填入 */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = outer_plan;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;

	/* 这些也许会被 FDW 的 PlanDirectModify 回调覆盖。 */
	node->operation = CMD_SELECT;
	node->resultRelation = 0;

	/* checkAsUser、fs_server 将由 create_foreignscan_plan 填入 */
	node->checkAsUser = InvalidOid;
	node->fs_server = InvalidOid;
	node->fdw_exprs = fdw_exprs;
	node->fdw_private = fdw_private;
	node->fdw_scan_tlist = fdw_scan_tlist;
	node->fdw_recheck_quals = fdw_recheck_quals;
	/* fs_relids、fs_base_relids 将由 create_foreignscan_plan 填入 */
	node->fs_relids = NULL;
	node->fs_base_relids = NULL;
	/* fsSystemCol 将由 create_foreignscan_plan 填入 */
	node->fsSystemCol = false;

	return node;
}

static RecursiveUnion *
make_recursive_union(List *tlist,
					 Plan *lefttree,
					 Plan *righttree,
					 int wtParam,
					 List *distinctList,
					 long numGroups)
{
	RecursiveUnion *node = makeNode(RecursiveUnion);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->wtParam = wtParam;

	/*
	 * 把 SortGroupClause 列表转换成执行器想要的属性下标和等值
	 * 操作符数组
	 */
	node->numCols = numCols;
	if (numCols > 0)
	{
		int			keyno = 0;
		AttrNumber *dupColIdx;
		Oid		   *dupOperators;
		Oid		   *dupCollations;
		ListCell   *slitem;

		dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
		dupOperators = (Oid *) palloc(sizeof(Oid) * numCols);
		dupCollations = (Oid *) palloc(sizeof(Oid) * numCols);

		foreach(slitem, distinctList)
		{
			SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
			TargetEntry *tle = get_sortgroupclause_tle(sortcl,
													   plan->targetlist);

			dupColIdx[keyno] = tle->resno;
			dupOperators[keyno] = sortcl->eqop;
			dupCollations[keyno] = exprCollation((Node *) tle->expr);
			Assert(OidIsValid(dupOperators[keyno]));
			keyno++;
		}
		node->dupColIdx = dupColIdx;
		node->dupOperators = dupOperators;
		node->dupCollations = dupCollations;
	}
	node->numGroups = numGroups;

	return node;
}

static BitmapAnd *
make_bitmap_and(List *bitmapplans)
{
	BitmapAnd  *node = makeNode(BitmapAnd);
	Plan	   *plan = &node->plan;

	plan->targetlist = NIL;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->bitmapplans = bitmapplans;

	return node;
}

static BitmapOr *
make_bitmap_or(List *bitmapplans)
{
	BitmapOr   *node = makeNode(BitmapOr);
	Plan	   *plan = &node->plan;

	plan->targetlist = NIL;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->bitmapplans = bitmapplans;

	return node;
}

static NestLoop *
make_nestloop(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  List *nestParams,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype,
			  bool inner_unique)
{
	NestLoop   *node = makeNode(NestLoop);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->join.jointype = jointype;
	node->join.inner_unique = inner_unique;
	node->join.joinqual = joinclauses;
	node->nestParams = nestParams;

	return node;
}

static HashJoin *
make_hashjoin(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  List *hashclauses,
			  List *hashoperators,
			  List *hashcollations,
			  List *hashkeys,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype,
			  bool inner_unique)
{
	HashJoin   *node = makeNode(HashJoin);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->hashclauses = hashclauses;
	node->hashoperators = hashoperators;
	node->hashcollations = hashcollations;
	node->hashkeys = hashkeys;
	node->join.jointype = jointype;
	node->join.inner_unique = inner_unique;
	node->join.joinqual = joinclauses;

	return node;
}

static Hash *
make_hash(Plan *lefttree,
		  List *hashkeys,
		  Oid skewTable,
		  AttrNumber skewColumn,
		  bool skewInherit)
{
	Hash	   *node = makeNode(Hash);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->hashkeys = hashkeys;
	node->skewTable = skewTable;
	node->skewColumn = skewColumn;
	node->skewInherit = skewInherit;

	return node;
}

static MergeJoin *
make_mergejoin(List *tlist,
			   List *joinclauses,
			   List *otherclauses,
			   List *mergeclauses,
			   Oid *mergefamilies,
			   Oid *mergecollations,
			   int *mergestrategies,
			   bool *mergenullsfirst,
			   Plan *lefttree,
			   Plan *righttree,
			   JoinType jointype,
			   bool inner_unique,
			   bool skip_mark_restore)
{
	MergeJoin  *node = makeNode(MergeJoin);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->skip_mark_restore = skip_mark_restore;
	node->mergeclauses = mergeclauses;
	node->mergeFamilies = mergefamilies;
	node->mergeCollations = mergecollations;
	node->mergeStrategies = mergestrategies;
	node->mergeNullsFirst = mergenullsfirst;
	node->join.jointype = jointype;
	node->join.inner_unique = inner_unique;
	node->join.joinqual = joinclauses;

	return node;
}

/*
 * make_sort --- 建立 Sort 计划节点的基本例程
 *
 * 调用方必须已经建好 sortColIdx、sortOperators、collations 和
 * nullsFirst 数组。
 */
static Sort *
make_sort(Plan *lefttree, int numCols,
		  AttrNumber *sortColIdx, Oid *sortOperators,
		  Oid *collations, bool *nullsFirst)
{
	Sort	   *node;
	Plan	   *plan;

	node = makeNode(Sort);

	plan = &node->plan;
	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->numCols = numCols;
	node->sortColIdx = sortColIdx;
	node->sortOperators = sortOperators;
	node->collations = collations;
	node->nullsFirst = nullsFirst;

	return node;
}

/*
 * make_incrementalsort --- 建立 IncrementalSort 计划节点的基本例程
 *
 * 调用方必须已经建好 sortColIdx、sortOperators、collations 和
 * nullsFirst 数组。
 */
static IncrementalSort *
make_incrementalsort(Plan *lefttree, int numCols, int nPresortedCols,
					 AttrNumber *sortColIdx, Oid *sortOperators,
					 Oid *collations, bool *nullsFirst)
{
	IncrementalSort *node;
	Plan	   *plan;

	node = makeNode(IncrementalSort);

	plan = &node->sort.plan;
	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->nPresortedCols = nPresortedCols;
	node->sort.numCols = numCols;
	node->sort.sortColIdx = sortColIdx;
	node->sort.sortOperators = sortOperators;
	node->sort.collations = collations;
	node->sort.nullsFirst = nullsFirst;

	return node;
}

/*
 * prepare_sort_from_pathkeys
 *	  准备按给定路径键排序
 *
 * 这用来为 Sort、MergeAppend 和 Gather Merge 节点做设置。它
 * 计算执行器对排序键信息的表示，并
 * 在需要时调整计划目标列表，以添加 resjunk 排序列。
 *
 * 输入参数：
 *	  'lefttree' 是产生输入元组的计划节点
 *	  'pathkeys' 是结果要据以排序的路径键列表
 *	  'relids' 标识正在排序的子关系，若有的话
 *	  'reqColIdx' 为 NULL，或一份所需排序键列号的数组
 *	  'adjust_tlist_in_place' 为真表示必须就地修改 lefttree
 *
 * 必须把路径键信息转换成排序键列
 * 号、排序操作符 OIDs、排序规则 OIDs 和 nulls-first 标志的数组，
 * 这是执行器想要的表示。它们返回到
 * 输出参数 *p_numsortkeys 等。
 *
 * 寻找与某个 EquivalenceClass 的成员匹配时，只有
 * 属于给定 'relids' 的子等价类成员才会被
 * 考虑。这防止错误地匹配到不含
 * Var 的子表达式。
 *
 * 若 reqColIdx 不是 NULL，它就含有我们应当匹配的排序键列号。
 * 建立 MergeAppend 的子计划时用这个；
 * 若匹配不上这些列，就是错误。
 *
 * 若路径键含有不是简单 Var 的表达式，通常
 * 需要往输入计划的目标列表里加 resjunk 项来
 * 计算这些表达式，因为 Sort 或 MergeAppend 节点本身不会
 * 做这类计算。若输入计划类型不能做
 * 投影，这就意味着加一个 Result 节点，只为了做投影。
 * 不过，调用方可以传入 adjust_tlist_in_place = true，强制
 * 就地修改 lefttree 的 tlist，不论该节点类型
 * 能否投影——我们用这个来修正 MergeAppend 自己的 tlist。
 *
 * 返回应当作为 Sort 输入的节点（要么是 lefttree，
 * 要么是叠在 lefttree 之上的 Result）。
 */
static Plan *
prepare_sort_from_pathkeys(Plan *lefttree, List *pathkeys,
						   Relids relids,
						   const AttrNumber *reqColIdx,
						   bool adjust_tlist_in_place,
						   int *p_numsortkeys,
						   AttrNumber **p_sortColIdx,
						   Oid **p_sortOperators,
						   Oid **p_collations,
						   bool **p_nullsFirst)
{
	List	   *tlist = lefttree->targetlist;
	ListCell   *i;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/*
	 * 最多需要 list_length(pathkeys) 个排序列；也许更少
	 */
	numsortkeys = list_length(pathkeys);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;

	foreach(i, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(i);
		EquivalenceClass *ec = pathkey->pk_eclass;
		EquivalenceMember *em;
		TargetEntry *tle = NULL;
		Oid			pk_datatype = InvalidOid;
		Oid			sortop;
		ListCell   *j;

		if (ec->ec_has_volatile)
		{
			/*
			 * 若该路径键的 EquivalenceClass 是易变的，那它一定
			 * 来自 ORDER BY 子句，我们必须把它匹配到
			 * 同一个目标列表项。
			 */
			if (ec->ec_sortref == 0)	/* 不可能发生 */
				elog(ERROR, "volatile EquivalenceClass has no sortref");
			tle = get_sortgroupref_tle(ec->ec_sortref, tlist);
			Assert(tle);
			Assert(list_length(ec->ec_members) == 1);
			pk_datatype = ((EquivalenceMember *) linitial(ec->ec_members))->em_datatype;
		}
		else if (reqColIdx != NULL)
		{
			/*
			 * If we are given a sort column number to match, only consider
			 * the single TLE at that position.  It's possible that there is
			 * no such TLE, in which case fall through and generate a resjunk
			 * targetentry (we assume this must have happened in the parent
			 * plan as well).  If there is a TLE but it doesn't match the
			 * pathkey's EC, we do the same, which is probably the wrong thing
			 * but we'll leave it to caller to complain about the mismatch.
			 */
			tle = get_tle_by_resno(tlist, reqColIdx[numsortkeys]);
			if (tle)
			{
				em = find_ec_member_matching_expr(ec, tle->expr, relids);
				if (em)
				{
					/* 在 tlist 的正确位置找到了表达式 */
					pk_datatype = em->em_datatype;
				}
				else
					tle = NULL;
			}
		}
		else
		{
			/*
			 * 否则，可以按该路径键的 EquivalenceClass 里列出的任何非常量表达式
			 * 排序。目前取
			 * 等价类里找到的第一个 tlist 项。若没有匹配，将用输入变量上的、
			 * 作为表达式的第一个等价类成员，生成
			 * 一个 resjunk 项。
			 *
			 * XXX 若有选择，有没有办法弄清哪个
			 * 执行起来也许最便宜？（例如 int4lt 很可能
			 * 比 numericlt 便宜得多，但两者也许出现在
			 * 同一个等价类里……）实践中是否会有
			 * 有意思的选择并不清楚，因此也许没关系。
			 */
			foreach(j, tlist)
			{
				tle = (TargetEntry *) lfirst(j);
				em = find_ec_member_matching_expr(ec, tle->expr, relids);
				if (em)
				{
					/* 表达式已经在 tlist 里找到了 */
					pk_datatype = em->em_datatype;
					break;
				}
				tle = NULL;
			}
		}

		if (!tle)
		{
			/*
			 * 没有匹配的 tlist 项；找一个可计算的表达式。
			 */
			em = find_computable_ec_member(NULL, ec, tlist, relids, false);
			if (!em)
				elog(ERROR, "could not find pathkey item to sort");
			pk_datatype = em->em_datatype;

			/*
			 * 需要插入 Result 节点吗？
			 */
			if (!adjust_tlist_in_place &&
				!is_projection_capable_plan(lefttree))
			{
				/* 需要复制，这样下面才不会修改输入的 tlist */
				tlist = copyObject(tlist);
				lefttree = inject_projection_plan(lefttree, tlist,
												  lefttree->parallel_safe);
			}

			/* 不必再测试 is_projection_capable_plan */
			adjust_tlist_in_place = true;

			/*
			 * 往输入的 tlist 里加 resjunk 项
			 */
			tle = makeTargetEntry(copyObject(em->em_expr),
								  list_length(tlist) + 1,
								  NULL,
								  true);
			tlist = lappend(tlist, tle);
			lefttree->targetlist = tlist;	/* 以防万一之前是 NIL */
		}

		/*
		 * 从 PathKey 略微
		 * 抽象的表示里查找正确的排序操作符。
		 */
		sortop = get_opfamily_member(pathkey->pk_opfamily,
									 pk_datatype,
									 pk_datatype,
									 pathkey->pk_strategy);
		if (!OidIsValid(sortop))	/* 不应当发生 */
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 pathkey->pk_strategy, pk_datatype, pk_datatype,
				 pathkey->pk_opfamily);

		/* 把该列加进排序数组 */
		sortColIdx[numsortkeys] = tle->resno;
		sortOperators[numsortkeys] = sortop;
		collations[numsortkeys] = ec->ec_collation;
		nullsFirst[numsortkeys] = pathkey->pk_nulls_first;
		numsortkeys++;
	}

	/* 返回结果 */
	*p_numsortkeys = numsortkeys;
	*p_sortColIdx = sortColIdx;
	*p_sortOperators = sortOperators;
	*p_collations = collations;
	*p_nullsFirst = nullsFirst;

	return lefttree;
}

/*
 * make_sort_from_pathkeys
 *	  建立按给定路径键排序的排序计划
 *
 *	  'lefttree' 是产生输入元组的节点
 *	  'pathkeys' 是结果要据以排序的路径键列表
 *	  'relids' 是 prepare_sort_from_pathkeys() 所要求的关系集合
 */
static Sort *
make_sort_from_pathkeys(Plan *lefttree, List *pathkeys, Relids relids)
{
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* 计算排序列信息，并按需要调整 lefttree */
	lefttree = prepare_sort_from_pathkeys(lefttree, pathkeys,
										  relids,
										  NULL,
										  false,
										  &numsortkeys,
										  &sortColIdx,
										  &sortOperators,
										  &collations,
										  &nullsFirst);

	/* 现在建立 Sort 节点 */
	return make_sort(lefttree, numsortkeys,
					 sortColIdx, sortOperators,
					 collations, nullsFirst);
}

/*
 * make_incrementalsort_from_pathkeys
 *	  建立按给定路径键排序的排序计划
 *
 *	  'lefttree' 是产生输入元组的节点
 *	  'pathkeys' 是结果要据以排序的路径键列表
 *	  'relids' 是 prepare_sort_from_pathkeys() 所要求的关系集合
 *	  'nPresortedCols' 是输入元组里已预排序的列数
 */
static IncrementalSort *
make_incrementalsort_from_pathkeys(Plan *lefttree, List *pathkeys,
								   Relids relids, int nPresortedCols)
{
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* 计算排序列信息，并按需要调整 lefttree */
	lefttree = prepare_sort_from_pathkeys(lefttree, pathkeys,
										  relids,
										  NULL,
										  false,
										  &numsortkeys,
										  &sortColIdx,
										  &sortOperators,
										  &collations,
										  &nullsFirst);

	/* 现在建立 Sort 节点 */
	return make_incrementalsort(lefttree, numsortkeys, nPresortedCols,
								sortColIdx, sortOperators,
								collations, nullsFirst);
}

/*
 * make_sort_from_sortclauses
 *	  建立按给定排序子句排序的排序计划
 *
 *	  'sortcls' 是一份 SortGroupClauses 列表
 *	  'lefttree' 是产生输入元组的节点
 */
Sort *
make_sort_from_sortclauses(List *sortcls, Plan *lefttree)
{
	List	   *sub_tlist = lefttree->targetlist;
	ListCell   *l;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* 把偏列表的表示转换成执行器想要的数组 */
	numsortkeys = list_length(sortcls);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;
	foreach(l, sortcls)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(l);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, sub_tlist);

		sortColIdx[numsortkeys] = tle->resno;
		sortOperators[numsortkeys] = sortcl->sortop;
		collations[numsortkeys] = exprCollation((Node *) tle->expr);
		nullsFirst[numsortkeys] = sortcl->nulls_first;
		numsortkeys++;
	}

	return make_sort(lefttree, numsortkeys,
					 sortColIdx, sortOperators,
					 collations, nullsFirst);
}

/*
 * make_sort_from_groupcols
 *	  建立基于分组列的排序计划
 *
 * 'groupcls' 是 SortGroupClauses 列表
 * 'grpColIdx' 给出要用的列号
 *
 * 这看起来也许可以与 make_sort_from_sortclauses 合并，
 * 但目前 *必须* 用 grpColIdx[] 数组来定位排序列，
 * 因为子计划的 tlist 没有标上适合
 * 该分组节点的 ressortgroupref 信息。因此 SortGroupClause 项里只用
 * 排序顺序信息。
 */
static Sort *
make_sort_from_groupcols(List *groupcls,
						 AttrNumber *grpColIdx,
						 Plan *lefttree)
{
	List	   *sub_tlist = lefttree->targetlist;
	ListCell   *l;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* 把偏列表的表示转换成执行器想要的数组 */
	numsortkeys = list_length(groupcls);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;
	foreach(l, groupcls)
	{
		SortGroupClause *grpcl = (SortGroupClause *) lfirst(l);
		TargetEntry *tle = get_tle_by_resno(sub_tlist, grpColIdx[numsortkeys]);

		if (!tle)
			elog(ERROR, "could not retrieve tle for sort-from-groupcols");

		sortColIdx[numsortkeys] = tle->resno;
		sortOperators[numsortkeys] = grpcl->sortop;
		collations[numsortkeys] = exprCollation((Node *) tle->expr);
		nullsFirst[numsortkeys] = grpcl->nulls_first;
		numsortkeys++;
	}

	return make_sort(lefttree, numsortkeys,
					 sortColIdx, sortOperators,
					 collations, nullsFirst);
}

static Material *
make_material(Plan *lefttree)
{
	Material   *node = makeNode(Material);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

/*
 * materialize_finished_plan：在已完成的计划之上贴一个 Material 节点
 *
 * 有几处想在 create_plan() 完成之后接上 Material 节点，
 * 而没有任何 MaterialPath 路径。
 * 那些地方也许有一天应当重构，在
 * Path 表示上做这件事，但目前还不值得费那个劲。
 */
Plan *
materialize_finished_plan(Plan *subplan)
{
	Plan	   *matplan;
	Path		matpath;		/* cost_material 结果的占位 */

	matplan = (Plan *) make_material(subplan);

	/*
	 * XXX 难看的凑合：若子计划挂有任何 initPlan，
	 * 就把它们上移到 Material 节点，它现在实际上是其查询层次的顶层
	 * 计划节点。这防止
	 * SS_finalize_plan() 失败，注释见该函数。我们不费心为此调整
	 * 子计划的代价估计。
	 */
	matplan->initPlan = subplan->initPlan;
	subplan->initPlan = NIL;

	/* 设置代价数据 */
	cost_material(&matpath,
				  subplan->startup_cost,
				  subplan->total_cost,
				  subplan->plan_rows,
				  subplan->plan_width);
	matplan->startup_cost = matpath.startup_cost;
	matplan->total_cost = matpath.total_cost;
	matplan->plan_rows = subplan->plan_rows;
	matplan->plan_width = subplan->plan_width;
	matplan->parallel_aware = false;
	matplan->parallel_safe = subplan->parallel_safe;

	return matplan;
}

static Memoize *
make_memoize(Plan *lefttree, Oid *hashoperators, Oid *collations,
			 List *param_exprs, bool singlerow, bool binary_mode,
			 uint32 est_entries, Bitmapset *keyparamids)
{
	Memoize    *node = makeNode(Memoize);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->numKeys = list_length(param_exprs);
	node->hashOperators = hashoperators;
	node->collations = collations;
	node->param_exprs = param_exprs;
	node->singlerow = singlerow;
	node->binary_mode = binary_mode;
	node->est_entries = est_entries;
	node->keyparamids = keyparamids;

	return node;
}

Agg *
make_agg(List *tlist, List *qual,
		 AggStrategy aggstrategy, AggSplit aggsplit,
		 int numGroupCols, AttrNumber *grpColIdx, Oid *grpOperators, Oid *grpCollations,
		 List *groupingSets, List *chain, double dNumGroups,
		 Size transitionSpace, Plan *lefttree)
{
	Agg		   *node = makeNode(Agg);
	Plan	   *plan = &node->plan;
	long		numGroups;

	/* 收成 long，但要当心溢出！ */
	numGroups = clamp_cardinality_to_long(dNumGroups);

	node->aggstrategy = aggstrategy;
	node->aggsplit = aggsplit;
	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;
	node->grpOperators = grpOperators;
	node->grpCollations = grpCollations;
	node->numGroups = numGroups;
	node->transitionSpace = transitionSpace;
	node->aggParams = NULL;		/* SS_finalize_plan() 会填这个 */
	node->groupingSets = groupingSets;
	node->chain = chain;

	plan->qual = qual;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

static WindowAgg *
make_windowagg(List *tlist, Index winref,
			   int partNumCols, AttrNumber *partColIdx, Oid *partOperators, Oid *partCollations,
			   int ordNumCols, AttrNumber *ordColIdx, Oid *ordOperators, Oid *ordCollations,
			   int frameOptions, Node *startOffset, Node *endOffset,
			   Oid startInRangeFunc, Oid endInRangeFunc,
			   Oid inRangeColl, bool inRangeAsc, bool inRangeNullsFirst,
			   List *runCondition, List *qual, bool topWindow, Plan *lefttree)
{
	WindowAgg  *node = makeNode(WindowAgg);
	Plan	   *plan = &node->plan;

	node->winref = winref;
	node->partNumCols = partNumCols;
	node->partColIdx = partColIdx;
	node->partOperators = partOperators;
	node->partCollations = partCollations;
	node->ordNumCols = ordNumCols;
	node->ordColIdx = ordColIdx;
	node->ordOperators = ordOperators;
	node->ordCollations = ordCollations;
	node->frameOptions = frameOptions;
	node->startOffset = startOffset;
	node->endOffset = endOffset;
	node->runCondition = runCondition;
	/* 上面那份的副本，给 EXPLAIN 用 */
	node->runConditionOrig = runCondition;
	node->startInRangeFunc = startInRangeFunc;
	node->endInRangeFunc = endInRangeFunc;
	node->inRangeColl = inRangeColl;
	node->inRangeAsc = inRangeAsc;
	node->inRangeNullsFirst = inRangeNullsFirst;
	node->topWindow = topWindow;

	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	plan->qual = qual;

	return node;
}

static Group *
make_group(List *tlist,
		   List *qual,
		   int numGroupCols,
		   AttrNumber *grpColIdx,
		   Oid *grpOperators,
		   Oid *grpCollations,
		   Plan *lefttree)
{
	Group	   *node = makeNode(Group);
	Plan	   *plan = &node->plan;

	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;
	node->grpOperators = grpOperators;
	node->grpCollations = grpCollations;

	plan->qual = qual;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

/*
 * distinctList 是一份 SortGroupClauses 列表，标识 Unique 过滤器应当考虑的目标列表项。
 * 输入路径必须
 * 已经按此排好序。
 */
static Unique *
make_unique_from_sortclauses(Plan *lefttree, List *distinctList)
{
	Unique	   *node = makeNode(Unique);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);
	int			keyno = 0;
	AttrNumber *uniqColIdx;
	Oid		   *uniqOperators;
	Oid		   *uniqCollations;
	ListCell   *slitem;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * 把 SortGroupClause 列表转换成执行器想要的属性下标和等值
	 * 操作符数组
	 */
	Assert(numCols > 0);
	uniqColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	uniqOperators = (Oid *) palloc(sizeof(Oid) * numCols);
	uniqCollations = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(slitem, distinctList)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, plan->targetlist);

		uniqColIdx[keyno] = tle->resno;
		uniqOperators[keyno] = sortcl->eqop;
		uniqCollations[keyno] = exprCollation((Node *) tle->expr);
		Assert(OidIsValid(uniqOperators[keyno]));
		keyno++;
	}

	node->numCols = numCols;
	node->uniqColIdx = uniqColIdx;
	node->uniqOperators = uniqOperators;
	node->uniqCollations = uniqCollations;

	return node;
}

/*
 * 同上，但用路径键识别排序列和语义
 */
static Unique *
make_unique_from_pathkeys(Plan *lefttree, List *pathkeys, int numCols)
{
	Unique	   *node = makeNode(Unique);
	Plan	   *plan = &node->plan;
	int			keyno = 0;
	AttrNumber *uniqColIdx;
	Oid		   *uniqOperators;
	Oid		   *uniqCollations;
	ListCell   *lc;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * 把路径键列表转换成执行器想要的属性下标和等值
	 * 操作符数组。这与
	 * prepare_sort_from_pathkeys 有很多共同之处……也许以后统一？
	 */
	Assert(numCols >= 0 && numCols <= list_length(pathkeys));
	uniqColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	uniqOperators = (Oid *) palloc(sizeof(Oid) * numCols);
	uniqCollations = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(lc, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(lc);
		EquivalenceClass *ec = pathkey->pk_eclass;
		EquivalenceMember *em;
		TargetEntry *tle = NULL;
		Oid			pk_datatype = InvalidOid;
		Oid			eqop;
		ListCell   *j;

		/* 忽略超出指定列数的路径键 */
		if (keyno >= numCols)
			break;

		if (ec->ec_has_volatile)
		{
			/*
			 * 若该路径键的 EquivalenceClass 是易变的，那它一定
			 * 来自 ORDER BY 子句，我们必须把它匹配到
			 * 同一个目标列表项。
			 */
			if (ec->ec_sortref == 0)	/* 不可能发生 */
				elog(ERROR, "volatile EquivalenceClass has no sortref");
			tle = get_sortgroupref_tle(ec->ec_sortref, plan->targetlist);
			Assert(tle);
			Assert(list_length(ec->ec_members) == 1);
			pk_datatype = ((EquivalenceMember *) linitial(ec->ec_members))->em_datatype;
		}
		else
		{
			/*
			 * 否则，可以用该路径键的 EquivalenceClass 里列出的任何非常量表达式。
			 * 目前取等价类里找到的第一个
			 * tlist 项。
			 */
			foreach(j, plan->targetlist)
			{
				tle = (TargetEntry *) lfirst(j);
				em = find_ec_member_matching_expr(ec, tle->expr, NULL);
				if (em)
				{
					/* 表达式已经在 tlist 里找到了 */
					pk_datatype = em->em_datatype;
					break;
				}
				tle = NULL;
			}
		}

		if (!tle)
			elog(ERROR, "could not find pathkey item to sort");

		/*
		 * 从 PathKey 略微
		 * 抽象的表示里查找正确的等值操作符。
		 */
		eqop = get_opfamily_member(pathkey->pk_opfamily,
								   pk_datatype,
								   pk_datatype,
								   BTEqualStrategyNumber);
		if (!OidIsValid(eqop))	/* 不应当发生 */
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 BTEqualStrategyNumber, pk_datatype, pk_datatype,
				 pathkey->pk_opfamily);

		uniqColIdx[keyno] = tle->resno;
		uniqOperators[keyno] = eqop;
		uniqCollations[keyno] = ec->ec_collation;

		keyno++;
	}

	node->numCols = numCols;
	node->uniqColIdx = uniqColIdx;
	node->uniqOperators = uniqOperators;
	node->uniqCollations = uniqCollations;

	return node;
}

static Gather *
make_gather(List *qptlist,
			List *qpqual,
			int nworkers,
			int rescan_param,
			bool single_copy,
			Plan *subplan)
{
	Gather	   *node = makeNode(Gather);
	Plan	   *plan = &node->plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->num_workers = nworkers;
	node->rescan_param = rescan_param;
	node->single_copy = single_copy;
	node->invisible = false;
	node->initParam = NULL;

	return node;
}

/*
 * distinctList 是一份 SortGroupClauses 列表，标识
 * SetOp 过滤器应当考虑的目标列表项。输入路径必须
 * 已经按此排好序。
 */
static SetOp *
make_setop(SetOpCmd cmd, SetOpStrategy strategy, Plan *lefttree,
		   List *distinctList, AttrNumber flagColIdx, int firstFlag,
		   long numGroups)
{
	SetOp	   *node = makeNode(SetOp);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);
	int			keyno = 0;
	AttrNumber *dupColIdx;
	Oid		   *dupOperators;
	Oid		   *dupCollations;
	ListCell   *slitem;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * 把 SortGroupClause 列表转换成执行器想要的属性下标和等值
	 * 操作符数组
	 */
	dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	dupOperators = (Oid *) palloc(sizeof(Oid) * numCols);
	dupCollations = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(slitem, distinctList)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, plan->targetlist);

		dupColIdx[keyno] = tle->resno;
		dupOperators[keyno] = sortcl->eqop;
		dupCollations[keyno] = exprCollation((Node *) tle->expr);
		Assert(OidIsValid(dupOperators[keyno]));
		keyno++;
	}

	node->cmd = cmd;
	node->strategy = strategy;
	node->numCols = numCols;
	node->dupColIdx = dupColIdx;
	node->dupOperators = dupOperators;
	node->dupCollations = dupCollations;
	node->flagColIdx = flagColIdx;
	node->firstFlag = firstFlag;
	node->numGroups = numGroups;

	return node;
}

/*
 * make_lockrows
 *	  建立一个 LockRows 计划节点
 */
static LockRows *
make_lockrows(Plan *lefttree, List *rowMarks, int epqParam)
{
	LockRows   *node = makeNode(LockRows);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->rowMarks = rowMarks;
	node->epqParam = epqParam;

	return node;
}

/*
 * make_limit
 *	  建立一个 Limit 计划节点
 */
Limit *
make_limit(Plan *lefttree, Node *limitOffset, Node *limitCount,
		   LimitOption limitOption, int uniqNumCols, AttrNumber *uniqColIdx,
		   Oid *uniqOperators, Oid *uniqCollations)
{
	Limit	   *node = makeNode(Limit);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->limitOffset = limitOffset;
	node->limitCount = limitCount;
	node->limitOption = limitOption;
	node->uniqNumCols = uniqNumCols;
	node->uniqColIdx = uniqColIdx;
	node->uniqOperators = uniqOperators;
	node->uniqCollations = uniqCollations;

	return node;
}

/*
 * make_result
 *	  建立一个 Result 计划节点
 */
static Result *
make_result(List *tlist,
			Node *resconstantqual,
			Plan *subplan)
{
	Result	   *node = makeNode(Result);
	Plan	   *plan = &node->plan;

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->resconstantqual = resconstantqual;

	return node;
}

/*
 * make_project_set
 *	  建立一个 ProjectSet 计划节点
 */
static ProjectSet *
make_project_set(List *tlist,
				 Plan *subplan)
{
	ProjectSet *node = makeNode(ProjectSet);
	Plan	   *plan = &node->plan;

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = subplan;
	plan->righttree = NULL;

	return node;
}

/*
 * make_modifytable
 *	  建立一个 ModifyTable 计划节点
 */
static ModifyTable *
make_modifytable(PlannerInfo *root, Plan *subplan,
				 CmdType operation, bool canSetTag,
				 Index nominalRelation, Index rootRelation,
				 bool partColsUpdated,
				 List *resultRelations,
				 List *updateColnosLists,
				 List *withCheckOptionLists, List *returningLists,
				 List *rowMarks, OnConflictExpr *onconflict,
				 List *mergeActionLists, int epqParam)
{
	ModifyTable *node = makeNode(ModifyTable);
	bool		transition_tables = false;
	bool		transition_tables_valid = false;
	List	   *fdw_private_list;
	Bitmapset  *direct_modify_plans;
	ListCell   *lc;
	int			i;

	Assert(operation == CMD_MERGE ||
		   (operation == CMD_UPDATE ?
			list_length(resultRelations) == list_length(updateColnosLists) :
			updateColnosLists == NIL));
	Assert(withCheckOptionLists == NIL ||
		   list_length(resultRelations) == list_length(withCheckOptionLists));
	Assert(returningLists == NIL ||
		   list_length(resultRelations) == list_length(returningLists));

	node->plan.lefttree = subplan;
	node->plan.righttree = NULL;
	node->plan.qual = NIL;
	/* 若需要，setrefs.c 会填入目标列表 */
	node->plan.targetlist = NIL;

	node->operation = operation;
	node->canSetTag = canSetTag;
	node->nominalRelation = nominalRelation;
	node->rootRelation = rootRelation;
	node->partColsUpdated = partColsUpdated;
	node->resultRelations = resultRelations;
	if (!onconflict)
	{
		node->onConflictAction = ONCONFLICT_NONE;
		node->onConflictSet = NIL;
		node->onConflictCols = NIL;
		node->onConflictWhere = NULL;
		node->arbiterIndexes = NIL;
		node->exclRelRTI = 0;
		node->exclRelTlist = NIL;
	}
	else
	{
		node->onConflictAction = onconflict->action;

		/*
		 * 这里把 ON CONFLICT UPDATE 的 tlist（若有）转换成
		 * 执行器的连续 resno 约定。实际的
		 * 目标列号保存在 node->onConflictCols 里。（这
		 * 可以更早做，但似乎没有必要。）
		 */
		node->onConflictSet = onconflict->onConflictSet;
		node->onConflictCols =
			extract_update_targetlist_colnos(node->onConflictSet);
		node->onConflictWhere = onconflict->onConflictWhere;

		/*
		 * 若提供了一组唯一索引推断元素（一份
		 * INSERT...ON CONFLICT "推断说明"），就推断
		 * 适当的唯一索引（若一个都没有，
		 * 就抛错）。
		 */
		node->arbiterIndexes = infer_arbiter_indexes(root);

		node->exclRelRTI = onconflict->exclRelIndex;
		node->exclRelTlist = onconflict->exclRelTlist;
	}
	node->updateColnosLists = updateColnosLists;
	node->withCheckOptionLists = withCheckOptionLists;
	node->returningLists = returningLists;
	node->rowMarks = rowMarks;
	node->mergeActionLists = mergeActionLists;
	node->epqParam = epqParam;

	/*
	 * 对作为外部表的每个结果关系，允许 FDW
	 * 构造私有计划数据，并全部累积进一份列表。
	 */
	fdw_private_list = NIL;
	direct_modify_plans = NULL;
	i = 0;
	foreach(lc, resultRelations)
	{
		Index		rti = lfirst_int(lc);
		FdwRoutine *fdwroutine;
		List	   *fdw_private;
		bool		direct_modify;

		/*
		 * 若可能，想从该表的 RelOptInfo 取得 FdwRoutine。
		 * 但有时没有 RelOptInfo，必须用
		 * 费劲的办法取得。（在 INSERT 里，目标关系不被扫描，
		 * 因此它不是基关系；可更新视图也有
		 * 目标关系不是基关系的边角情形。）
		 */
		if (rti < root->simple_rel_array_size &&
			root->simple_rel_array[rti] != NULL)
		{
			RelOptInfo *resultRel = root->simple_rel_array[rti];

			fdwroutine = resultRel->fdwroutine;
		}
		else
		{
			RangeTblEntry *rte = planner_rt_fetch(rti, root);

			if (rte->rtekind == RTE_RELATION &&
				rte->relkind == RELKIND_FOREIGN_TABLE)
			{
				/* 检查对外部表的访问是否受限制 */
				if (unlikely((restrict_nonsystem_relation_kind & RESTRICT_RELKIND_FOREIGN_TABLE) != 0))
				{
					/* 不得有内建的外部表 */
					Assert(rte->relid >= FirstNormalObjectId);
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("access to non-system foreign table is restricted")));
				}

				fdwroutine = GetFdwRoutineByRelId(rte->relid);
			}
			else
				fdwroutine = NULL;
		}

		/*
		 * 目前外部表不支持 MERGE。当查询里提到的表是外部表时，
		 * 已经检查过了；但
		 * 若分区表有外部表作为
		 * 分区，仍可能走到这里。现在就禁止，以免以后
		 * 出现更难看的错误信息。
		 */
		if (operation == CMD_MERGE && fdwroutine != NULL)
		{
			RangeTblEntry *rte = planner_rt_fetch(rti, root);

			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot execute MERGE on relation \"%s\"",
						   get_rel_name(rte->relid)),
					errdetail_relkind_not_supported(rte->relkind));
		}

		/*
		 * 若（1）FDW 提供了
		 * 所需的回调函数，且（2）没有必须对每一被修改行运行的本地
		 * 结构：外部表上的行级
		 * 触发器、存储生成列、父视图的 WITH CHECK
		 * OPTIONs、具名关系上的转换表，就尝试直接修改外部表。
		 */
		direct_modify = false;
		if (fdwroutine != NULL &&
			fdwroutine->PlanDirectModify != NULL &&
			fdwroutine->BeginDirectModify != NULL &&
			fdwroutine->IterateDirectModify != NULL &&
			fdwroutine->EndDirectModify != NULL &&
			withCheckOptionLists == NIL &&
			!has_row_triggers(root, rti, operation) &&
			!has_stored_generated_columns(root, rti))
		{
			/* transition_tables 对全部结果关系都相同 */
			if (!transition_tables_valid)
			{
				transition_tables = has_transition_tables(root,
														  nominalRelation,
														  operation);
				transition_tables_valid = true;
			}
			if (!transition_tables)
				direct_modify = fdwroutine->PlanDirectModify(root, node,
															 rti, i);
		}
		if (direct_modify)
			direct_modify_plans = bms_add_member(direct_modify_plans, i);

		if (!direct_modify &&
			fdwroutine != NULL &&
			fdwroutine->PlanForeignModify != NULL)
			fdw_private = fdwroutine->PlanForeignModify(root, node, rti, i);
		else
			fdw_private = NIL;
		fdw_private_list = lappend(fdw_private_list, fdw_private);
		i++;
	}
	node->fdwPrivLists = fdw_private_list;
	node->fdwDirectModifyPlans = direct_modify_plans;

	return node;
}

/*
 * is_projection_capable_path
 *		检查给定的 Path 节点能否做投影。
 */
bool
is_projection_capable_path(Path *path)
{
	/* 多数计划类型都能做投影，因此只列出不能的那些 */
	switch (path->pathtype)
	{
		case T_Hash:
		case T_Material:
		case T_Memoize:
		case T_Sort:
		case T_IncrementalSort:
		case T_Unique:
		case T_SetOp:
		case T_LockRows:
		case T_Limit:
		case T_ModifyTable:
		case T_MergeAppend:
		case T_RecursiveUnion:
			return false;
		case T_CustomScan:
			if (castNode(CustomPath, path)->flags & CUSTOMPATH_SUPPORT_PROJECTION)
				return true;
			return false;
		case T_Append:

			/*
			 * Append 不能做投影，但若 AppendPath 被用来
			 * 表示哑路径，实际生成的将是一个
			 * 能做投影的 Result。
			 */
			return IS_DUMMY_APPEND(path);
		case T_ProjectSet:

			/*
			 * 虽然 ProjectSet 当然会做投影，仍说 "不"，因为不想
			 * 让规划器随意把它的 tlist 换成
			 * 别的东西；SRFs 必须留在顶层。这也许
			 * 以后会放宽。
			 */
			return false;
		default:
			break;
	}
	return true;
}

/*
 * is_projection_capable_plan
 *		检查给定的 Plan 节点能否做投影。
 */
bool
is_projection_capable_plan(Plan *plan)
{
	/* 多数计划类型都能做投影，因此只列出不能的那些 */
	switch (nodeTag(plan))
	{
		case T_Hash:
		case T_Material:
		case T_Memoize:
		case T_Sort:
		case T_Unique:
		case T_SetOp:
		case T_LockRows:
		case T_Limit:
		case T_ModifyTable:
		case T_Append:
		case T_MergeAppend:
		case T_RecursiveUnion:
			return false;
		case T_CustomScan:
			if (((CustomScan *) plan)->flags & CUSTOMPATH_SUPPORT_PROJECTION)
				return true;
			return false;
		case T_ProjectSet:

			/*
			 * 虽然 ProjectSet 当然会做投影，仍说 "不"，因为不想
			 * 让规划器随意把它的 tlist 换成
			 * 别的东西；SRFs 必须留在顶层。这也许
			 * 以后会放宽。
			 */
			return false;
		default:
			break;
	}
	return true;
}
