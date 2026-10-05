/*-------------------------------------------------------------------------
 *
 * setrefs.c
 *	  对已完成的计划树做后处理：修正对子计划
 *	  var 的引用，计算操作符的 regproc 值，等等
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/setrefs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "tcop/utility.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


typedef enum
{
	NRM_EQUAL,					/* 预期 nullingrels 精确匹配 */
	NRM_SUBSET,					/* 实际 Var 的输入可能是子集 */
	NRM_SUPERSET				/* 实际 Var 的输入可能是超集 */
} NullingRelsMatch;

typedef struct
{
	int			varno;			/* Var 的 RT 下标 */
	AttrNumber	varattno;		/* Var 的属性号 */
	AttrNumber	resno;			/* Var 的 TLE 位置 */
	Bitmapset  *varnullingrels; /* Var 的 varnullingrels */
} tlist_vinfo;

typedef struct
{
	List	   *tlist;			/* 底层目标列表 */
	int			num_vars;		/* 普通 Var 的 tlist 项个数 */
	bool		has_ph_vars;	/* 有没有 PlaceHolderVar 项？ */
	bool		has_non_vars;	/* 有没有其他项？ */
	tlist_vinfo vars[FLEXIBLE_ARRAY_MEMBER];	/* 有 num_vars 项 */
} indexed_tlist;

typedef struct
{
	PlannerInfo *root;
	int			rtoffset;
	double		num_exec;
} fix_scan_expr_context;

typedef struct
{
	PlannerInfo *root;
	indexed_tlist *outer_itlist;
	indexed_tlist *inner_itlist;
	Index		acceptable_rel;
	int			rtoffset;
	NullingRelsMatch nrm_match;
	double		num_exec;
} fix_join_expr_context;

typedef struct
{
	PlannerInfo *root;
	indexed_tlist *subplan_itlist;
	int			newvarno;
	int			rtoffset;
	NullingRelsMatch nrm_match;
	double		num_exec;
} fix_upper_expr_context;

typedef struct
{
	PlannerInfo *root;
	indexed_tlist *subplan_itlist;
	int			newvarno;
} fix_windowagg_cond_context;

/* flatten_rtes_walker() 的上下文信息 */
typedef struct
{
	PlannerGlobal *glob;
	Query	   *query;
} flatten_rtes_walker_context;

/*
 * 在 AlternativeSubPlan 表达式里选出最佳备选，需要
 * 估计该表达式会被求值多少次。对计划节点目标列表里的
 * 表达式，显然用计划估计的输出行数；但对限定条件里的表达式就
 * 远没那么清楚。由于 AlternativeSubPlans 用得不多，我们不想
 * 花很多周期去做这类估计。所用的是输出行数的两倍。
 * 这并非全无根据：我们知道
 * clause_selectivity() 对任何 SubPlan 都会退回到默认选择率估计
 * 0.5，因此若包含该 SubPlan 的限定条件是最后
 * 被应用的（多亏 order_qual_clauses()，很可能如此），
 * 这就与用费力得多的办法所能估计出的一致。
 * 显然还有许多其他情形，但大概不值得费劲
 * 去改进这个估计，尤其是当
 * 我们对 SubPlan 限定条件本身的选择率
 * 并没有更好的估计时。
 */
#define NUM_EXEC_TLIST(parentplan)  ((parentplan)->plan_rows)
#define NUM_EXEC_QUAL(parentplan)   ((parentplan)->plan_rows * 2.0)

/*
 * 检查 Const 节点是否为 regclass 值。普通 OID 也接受，
 * 因为若 regclass Const 是 oideq 或类似操作符的参数，
 * 会被折叠成那种类型。（这也许会使计划的关系依赖列表里
 * 出现一些多余的值，但最坏结果
 * 不过是偶尔做一次无用的重新规划。）
 */
#define ISREGCLASSCONST(con) \
	(((con)->consttype == REGCLASSOID || (con)->consttype == OIDOID) && \
	 !(con)->constisnull)

#define fix_scan_list(root, lst, rtoffset, num_exec) \
	((List *) fix_scan_expr(root, (Node *) (lst), rtoffset, num_exec))

static void add_rtes_to_flat_rtable(PlannerInfo *root, bool recursing);
static void flatten_unplanned_rtes(PlannerGlobal *glob, RangeTblEntry *rte);
static bool flatten_rtes_walker(Node *node, flatten_rtes_walker_context *cxt);
static void add_rte_to_flat_rtable(PlannerGlobal *glob, List *rteperminfos,
								   RangeTblEntry *rte);
static Plan *set_plan_refs(PlannerInfo *root, Plan *plan, int rtoffset);
static Plan *set_indexonlyscan_references(PlannerInfo *root,
										  IndexOnlyScan *plan,
										  int rtoffset);
static Plan *set_subqueryscan_references(PlannerInfo *root,
										 SubqueryScan *plan,
										 int rtoffset);
static Plan *clean_up_removed_plan_level(Plan *parent, Plan *child);
static void set_foreignscan_references(PlannerInfo *root,
									   ForeignScan *fscan,
									   int rtoffset);
static void set_customscan_references(PlannerInfo *root,
									  CustomScan *cscan,
									  int rtoffset);
static Plan *set_append_references(PlannerInfo *root,
								   Append *aplan,
								   int rtoffset);
static Plan *set_mergeappend_references(PlannerInfo *root,
										MergeAppend *mplan,
										int rtoffset);
static void set_hash_references(PlannerInfo *root, Plan *plan, int rtoffset);
static Relids offset_relid_set(Relids relids, int rtoffset);
static Node *fix_scan_expr(PlannerInfo *root, Node *node,
						   int rtoffset, double num_exec);
static Node *fix_scan_expr_mutator(Node *node, fix_scan_expr_context *context);
static bool fix_scan_expr_walker(Node *node, fix_scan_expr_context *context);
static void set_join_references(PlannerInfo *root, Join *join, int rtoffset);
static void set_upper_references(PlannerInfo *root, Plan *plan, int rtoffset);
static void set_param_references(PlannerInfo *root, Plan *plan);
static Node *convert_combining_aggrefs(Node *node, void *context);
static void set_dummy_tlist_references(Plan *plan, int rtoffset);
static indexed_tlist *build_tlist_index(List *tlist);
static Var *search_indexed_tlist_for_var(Var *var,
										 indexed_tlist *itlist,
										 int newvarno,
										 int rtoffset,
										 NullingRelsMatch nrm_match);
static Var *search_indexed_tlist_for_phv(PlaceHolderVar *phv,
										 indexed_tlist *itlist,
										 int newvarno,
										 NullingRelsMatch nrm_match);
static Var *search_indexed_tlist_for_non_var(Expr *node,
											 indexed_tlist *itlist,
											 int newvarno);
static Var *search_indexed_tlist_for_sortgroupref(Expr *node,
												  Index sortgroupref,
												  indexed_tlist *itlist,
												  int newvarno);
static List *fix_join_expr(PlannerInfo *root,
						   List *clauses,
						   indexed_tlist *outer_itlist,
						   indexed_tlist *inner_itlist,
						   Index acceptable_rel,
						   int rtoffset,
						   NullingRelsMatch nrm_match,
						   double num_exec);
static Node *fix_join_expr_mutator(Node *node,
								   fix_join_expr_context *context);
static Node *fix_upper_expr(PlannerInfo *root,
							Node *node,
							indexed_tlist *subplan_itlist,
							int newvarno,
							int rtoffset,
							NullingRelsMatch nrm_match,
							double num_exec);
static Node *fix_upper_expr_mutator(Node *node,
									fix_upper_expr_context *context);
static List *set_returning_clause_references(PlannerInfo *root,
											 List *rlist,
											 Plan *topplan,
											 Index resultRelation,
											 int rtoffset);
static List *set_windowagg_runcondition_references(PlannerInfo *root,
												   List *runcondition,
												   Plan *plan);


/*****************************************************************************
 *
 *		SUBPLAN REFERENCES（子计划引用）
 *
 *****************************************************************************/

/*
 * set_plan_references
 *
 * 这是规划器的最后一遍处理。计划
 * 树已经完成；只需为执行器方便调整一些表示细节：
 * 1. 把各个子查询范围表展平成一份列表，并
 *
 * 把对执行器没用的 RangeTblEntry 字段清零。
 * 2. 调整扫描节点里的 Var，使之与展平后的范围表一致。
 *
 * 3. 调整上层计划节点里的 Var，使之引用其子计划的
 *
 * 输出。
 * 4. 在涉及部分聚合或 minmax 聚合优化的某些情况下，
 *
 * 需要调整 Agg 计划节点里的 Aggref。
 * 5. PARAM_MULTIEXPR 参数被换成普通的 PARAM_EXEC 参数，
 *
 * 因为已经完成全部 MULTIEXPR 子计划的规划。
 * 6. AlternativeSubPlan 表达式被换成其中
 *
 * 一个备选，依据是它们将被执行多少次的估计。
 * 7. 为操作符计算 regproc OIDs（也就是查找实现
 *
 * 每个操作的函数）。
 * 8. 建立该计划所依赖的特定对象列表。
 *
 * 这将由 plancache.c 用来驱动缓存计划的失效。
 * 关系依赖用 OIDs 表示，其余一切用
 * PlanInvalItems 表示（这一区分源于共享失效 APIs）。
 * 目前，关系、用户定义函数和域是唯一
 * 以这种方式显式跟踪的对象类型。
 * 9. 给树中每个计划节点分配一个唯一 ID。
 *
 * 我们还做最后一步优化，即删掉那些没有
 *
 * 做任何有用事情的 SubqueryScan、Append 和 MergeAppend 计划节点。
 * 放在最后做的原因是，
 * 在 set_plan_references 之前不容易做，因为那会
 * 破坏 set_upper_references：子计划顶层 tlist 里的 Var
 * 会与外层计划树里的 Var 对不上。
 * SubqueryScan
 * 作为外层查询与子查询变量编号之间的缓冲是必要的
 * ……但展平范围表之后这就不再是问题，因为那时只有一个 rtindex 命名空间。
 * 同样，Append 和 MergeAppend 在 appendrel 的父子 var 之间起缓冲，
 * 但做完
 * set_plan_references 之后就不必再担心。
 * set_plan_references 递归遍历整棵计划树。
 *
 * 返回值通常就是传入的那个 Plan 节点，但也可以
 *
 * 不同，当传入的 Plan 是我们判定不需要的节点时。
 * 展平后的范围表项追加到 root->glob->finalrtable。
 *
 * 另外，rowmarks 项追加到 root->glob->finalrowmarks，并且
 * ModifyTable 结果关系的 RT 下标
 * 追加到 root->glob->resultRelations，
 * 展平后的 AppendRelInfos 追加到 root->glob->appendRelations。
 * 计划依赖追加到 root->glob->relationOids（关系）
 * 和 root->glob->invalItems（其余一切）。
 *
 * 注意我们就地修改 Plan 节点，但用 expression_tree_mutator
 * 处理目标列表和限定表达式。可以假定这些 Plan
 * 节点刚由规划器建成、没有被多处引用，但
 * 对表达式树节点做同样的假定就不那么安全。
 */
Plan *
set_plan_references(PlannerInfo *root, Plan *plan)
{
	Plan	   *result;
	PlannerGlobal *glob = root->glob;
	int			rtoffset = list_length(glob->finalrtable);
	ListCell   *lc;

	/*
	 * 把该查询的全部 RTEs 加进展平后的范围表。活着的那些
	 * 其范围表下标会加上 rtoffset。（计划树未引用的额外
	 * RTEs 也许会加在它们后面。）
	 */
	add_rtes_to_flat_rtable(root, false);

	/*
	 * 调整 PlanRowMarks 的 RT 下标，并加进最终 rowmarks 列表
	 */
	foreach(lc, root->rowMarks)
	{
		PlanRowMark *rc = lfirst_node(PlanRowMark, lc);
		PlanRowMark *newrc;

		/* 浅拷贝就够了，因为全部字段都是标量 */
		newrc = (PlanRowMark *) palloc(sizeof(PlanRowMark));
		memcpy(newrc, rc, sizeof(PlanRowMark));

		/* 调整下标……但 *不* 调整 rowmarkId */
		newrc->rti += rtoffset;
		newrc->prti += rtoffset;

		glob->finalrowmarks = lappend(glob->finalrowmarks, newrc);
	}

	/*
	 * 调整 AppendRelInfos 的 RT 下标，并加进最终 appendrels 列表。
	 * 假定这些 AppendRelInfos 是规划期间建成的，不必
	 * 复制。
	 */
	foreach(lc, root->append_rel_list)
	{
		AppendRelInfo *appinfo = lfirst_node(AppendRelInfo, lc);

		/* 调整 RT 下标 */
		appinfo->parent_relid += rtoffset;
		appinfo->child_relid += rtoffset;

		/*
		 * 与其调整 translated_vars 项，不如直接丢掉。
		 * 执行器和 EXPLAIN 目前都不需要那份数据。
		 */
		appinfo->translated_vars = NIL;

		glob->appendRelations = lappend(glob->appendRelations, appinfo);
	}

	/* 若需要，为处理 AlternativeSubPlans 建立工作区 */
	if (root->hasAlternativeSubPlans)
	{
		root->isAltSubplan = (bool *)
			palloc0(list_length(glob->subplans) * sizeof(bool));
		root->isUsedSubplan = (bool *)
			palloc0(list_length(glob->subplans) * sizeof(bool));
	}

	/* 现在修正 Plan 树 */
	result = set_plan_refs(root, plan, rtoffset);

	/*
	 * 若有 AlternativeSubPlans，现在 glob->subplans 里很可能有一些
	 * 未被引用的子计划。为避免以后在
	 * 那些子计划上花周期，把那些列表项设成
	 * NULL 来丢掉它们。（注意：不能在处理某个
	 * AlternativeSubPlan 时立刻这么做，因为也许有多份
	 * AlternativeSubPlan 副本，而且它们的解析结果可以不同。）
	 */
	if (root->hasAlternativeSubPlans)
	{
		foreach(lc, glob->subplans)
		{
			int			ndx = foreach_current_index(lc);

			/*
			 * 若它被本查询层次的某个 AlternativeSubPlan 用过，
			 * 但没有任何 AlternativeSubPlan 把它选为最佳，那就
			 * 不需要它。不要动那些不属于
			 * AlternativeSubPlans 的子计划。
			 */
			if (root->isAltSubplan[ndx] && !root->isUsedSubplan[ndx])
				lfirst(lc) = NULL;
		}
	}

	return result;
}

/*
 * 从计划的范围表抽出 RangeTblEntries，并加进展平范围表
 *
 * 这可以递归进入子查询计划；若如此，"recursing" 为真。
 *
 * 这里似乎也适合把查询的 RTEPermissionInfos 加进
 * 展平后的 rteperminfos。
 */
static void
add_rtes_to_flat_rtable(PlannerInfo *root, bool recursing)
{
	PlannerGlobal *glob = root->glob;
	Index		rti;
	ListCell   *lc;

	/*
	 * 把查询自己的 RTEs 加进展平后的范围表。
	 *
	 * 在顶层，必须加入全部 RTEs，使它们在
	 * 展平范围表里的下标与原来的下标对齐。当
	 * 递归时，只关心抽出关系 RTEs（以及曾经是关系 RTE 的子查询
	 * RTEs）。
	 */
	foreach(lc, root->parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (!recursing || rte->rtekind == RTE_RELATION ||
			(rte->rtekind == RTE_SUBQUERY && OidIsValid(rte->relid)))
			add_rte_to_flat_rtable(glob, root->parse->rteperminfos, rte);
	}

	/*
	 * 若有任何死子查询，它们在 Plan
	 * 树里没有引用，因此必须把它们所含的 RTEs 单独加进
	 * 展平范围表。（若不这么做，执行器就不会对这类子查询里提到的表
	 * 做预期的权限检查。）
	 *
	 * 注意：这一趟范围表扫描不能与前一趟
	 * 合并，因为那会搞乱活 RTEs 在
	 * 展平范围表里的编号。
	 */
	rti = 1;
	foreach(lc, root->parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		/*
		 * 应当忽略继承父项 RTEs：它们的内容已经
		 * 上拉进我们的范围表。也忽略任何没有匹配 RelOptInfos 的子查询
		 * RTEs，因为它们同样已经
		 * 被上拉。
		 */
		if (rte->rtekind == RTE_SUBQUERY && !rte->inh &&
			rti < root->simple_rel_array_size)
		{
			RelOptInfo *rel = root->simple_rel_array[rti];

			if (rel != NULL)
			{
				Assert(rel->relid == rti);	/* 对数组做健全性检查 */

				/*
				 * 该子查询也许根本没被规划过，若它
				 * 因本查询层次里自相矛盾的约束
				 * 而被排除。这种情况下应用
				 * flatten_unplanned_rtes。
				 *
				 * 若它被规划过但结果关系是哑的，就假定
				 * 它已从我们的计划树里省略（见
				 * set_subquery_pathlist），并递归上拉它的 RTEs。
				 *
				 * 否则，它应当由计划树某处的一个 SubqueryScan 节点
				 * 表示，处理该计划节点时
				 * 会上拉它的 RTEs。
				 *
				 * 不过，若正在递归，则无论该子查询是不是哑的，都应当上拉 RTEs，
				 * 因为已经发现
				 * 某个更上层查询把这一层当作哑的，
				 * 因此根本不会扫描本层的计划树。
				 */
				if (rel->subroot == NULL)
					flatten_unplanned_rtes(glob, rte);
				else if (recursing ||
						 IS_DUMMY_REL(fetch_upper_rel(rel->subroot,
													  UPPERREL_FINAL, NULL)))
					add_rtes_to_flat_rtable(rel->subroot, true);
			}
		}
		rti++;
	}
}

/*
 * 从根本没被规划过的子查询里抽出 RangeTblEntries
 */

static void
flatten_unplanned_rtes(PlannerGlobal *glob, RangeTblEntry *rte)
{
	flatten_rtes_walker_context cxt = {glob, rte->subquery};

	/* 用 query_tree_walker 找出解析树里的全部 RTEs */
	(void) query_tree_walker(rte->subquery,
							 flatten_rtes_walker,
							 (void *) &cxt,
							 QTW_EXAMINE_RTES_BEFORE);
}

static bool
flatten_rtes_walker(Node *node, flatten_rtes_walker_context *cxt)
{
	if (node == NULL)
		return false;
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		/* 同上，只需保存关系 RTEs 和曾经的关系 */
		if (rte->rtekind == RTE_RELATION ||
			(rte->rtekind == RTE_SUBQUERY && OidIsValid(rte->relid)))
			add_rte_to_flat_rtable(cxt->glob, cxt->query->rteperminfos, rte);
		return false;
	}
	if (IsA(node, Query))
	{
		/*
		 * 递归进入子选择。必须把 cxt->query 更新为本查询，使
		 * rtable 与 rteperminfos 彼此对应。
		 */
		Query	   *save_query = cxt->query;
		bool		result;

		cxt->query = (Query *) node;
		result = query_tree_walker((Query *) node,
								   flatten_rtes_walker,
								   (void *) cxt,
								   QTW_EXAMINE_RTES_BEFORE);
		cxt->query = save_query;
		return result;
	}
	return expression_tree_walker(node, flatten_rtes_walker,
								  (void *) cxt);
}

/*
 * 把给定 RTE 的一份副本加进最终范围表，若有对应的
 * RTEPermissionInfo，也加进最终 rteperminfos。
 *
 * 在展平范围表里，把执行器不需要的子结构指针清零；
 * 这减少缓存计划的存储空间和复制代价。
 * 只保留 EXPLAIN 需要的 ctename、alias、eref Alias 字段，
 * 以及执行器取该 RTE 的 RTEPermissionInfo
 * 所需的 perminfoindex。
 */
static void
add_rte_to_flat_rtable(PlannerGlobal *glob, List *rteperminfos,
					   RangeTblEntry *rte)
{
	RangeTblEntry *newrte;

	/* 浅拷贝以复制全部标量字段 */
	newrte = (RangeTblEntry *) palloc(sizeof(RangeTblEntry));
	memcpy(newrte, rte, sizeof(RangeTblEntry));

	/* 清掉不需要的子结构 */
	newrte->tablesample = NULL;
	newrte->subquery = NULL;
	newrte->joinaliasvars = NIL;
	newrte->joinleftcols = NIL;
	newrte->joinrightcols = NIL;
	newrte->join_using_alias = NULL;
	newrte->functions = NIL;
	newrte->tablefunc = NULL;
	newrte->values_lists = NIL;
	newrte->coltypes = NIL;
	newrte->coltypmods = NIL;
	newrte->colcollations = NIL;
	newrte->securityQuals = NIL;

	glob->finalrtable = lappend(glob->finalrtable, newrte);

	/*
	 * 若是普通关系 RTE（或曾经是视图引用的子查询），
	 * 就把关系 OID 加进 relationOids。
	 *
	 * 即使该 RTE 在计划树里也许未被引用也这么做；
	 * 这对应视图被展开、子
	 * 表被约束排除去掉等情形。对这种关系的模式
	 * 失效仍必须迫使计划重建。
	 *
	 * 注意我们不费心避免列表项重复。可以避免，
	 * 但花的周期大概比省下的多。
	 */
	if (newrte->rtekind == RTE_RELATION ||
		(newrte->rtekind == RTE_SUBQUERY && OidIsValid(newrte->relid)))
		glob->relationOids = lappend_oid(glob->relationOids, newrte->relid);

	/*
	 * 若有与该 RTE 对应的 RTEPermissionInfo，就把它的一份副本
	 * 加进展平后的全局列表。
	 */
	if (rte->perminfoindex > 0)
	{
		RTEPermissionInfo *perminfo;
		RTEPermissionInfo *newperminfo;

		/* 从本查询的 rteperminfos 取出已有的那份。 */
		perminfo = getRTEPermissionInfo(rteperminfos, newrte);

		/*
		 * 向 finalrteperminfos 加一份新的，并把已有那份的内容
		 * 复制进去。注意 addRTEPermissionInfo() 还会
		 * 更新 newrte->perminfoindex，使之指向
		 * finalrteperminfos 里的 newperminfo。
		 */
		newrte->perminfoindex = 0;	/* addRTEPermissionInfo() 所预期 */
		newperminfo = addRTEPermissionInfo(&glob->finalrteperminfos, newrte);
		memcpy(newperminfo, perminfo, sizeof(RTEPermissionInfo));
	}
}

/*
 * set_plan_refs：递归走过单个子查询层次的 Plan 节点
 */
static Plan *
set_plan_refs(PlannerInfo *root, Plan *plan, int rtoffset)
{
	ListCell   *l;

	if (plan == NULL)
		return NULL;

	/* 给本节点分配一个唯一 ID。 */
	plan->plan_node_id = root->glob->lastPlanNodeId++;

	/*
	 * 按计划类型做的修正
	 */
	switch (nodeTag(plan))
	{
		case T_SeqScan:
			{
				SeqScan    *splan = (SeqScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_SampleScan:
			{
				SampleScan *splan = (SampleScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->tablesample = (TableSampleClause *)
					fix_scan_expr(root, (Node *) splan->tablesample,
								  rtoffset, 1);
			}
			break;
		case T_IndexScan:
			{
				IndexScan  *splan = (IndexScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->indexqual =
					fix_scan_list(root, splan->indexqual,
								  rtoffset, 1);
				splan->indexqualorig =
					fix_scan_list(root, splan->indexqualorig,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->indexorderby =
					fix_scan_list(root, splan->indexorderby,
								  rtoffset, 1);
				splan->indexorderbyorig =
					fix_scan_list(root, splan->indexorderbyorig,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_IndexOnlyScan:
			{
				IndexOnlyScan *splan = (IndexOnlyScan *) plan;

				return set_indexonlyscan_references(root, splan, rtoffset);
			}
			break;
		case T_BitmapIndexScan:
			{
				BitmapIndexScan *splan = (BitmapIndexScan *) plan;

				splan->scan.scanrelid += rtoffset;
				/* 不必修正目标列表和限定条件 */
				Assert(splan->scan.plan.targetlist == NIL);
				Assert(splan->scan.plan.qual == NIL);
				splan->indexqual =
					fix_scan_list(root, splan->indexqual, rtoffset, 1);
				splan->indexqualorig =
					fix_scan_list(root, splan->indexqualorig,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_BitmapHeapScan:
			{
				BitmapHeapScan *splan = (BitmapHeapScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->bitmapqualorig =
					fix_scan_list(root, splan->bitmapqualorig,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_TidScan:
			{
				TidScan    *splan = (TidScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->tidquals =
					fix_scan_list(root, splan->tidquals,
								  rtoffset, 1);
			}
			break;
		case T_TidRangeScan:
			{
				TidRangeScan *splan = (TidRangeScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->tidrangequals =
					fix_scan_list(root, splan->tidrangequals,
								  rtoffset, 1);
			}
			break;
		case T_SubqueryScan:
			/* 需要特殊处理，见下面的注释 */
			return set_subqueryscan_references(root,
											   (SubqueryScan *) plan,
											   rtoffset);
		case T_FunctionScan:
			{
				FunctionScan *splan = (FunctionScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->functions =
					fix_scan_list(root, splan->functions, rtoffset, 1);
			}
			break;
		case T_TableFuncScan:
			{
				TableFuncScan *splan = (TableFuncScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->tablefunc = (TableFunc *)
					fix_scan_expr(root, (Node *) splan->tablefunc,
								  rtoffset, 1);
			}
			break;
		case T_ValuesScan:
			{
				ValuesScan *splan = (ValuesScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
				splan->values_lists =
					fix_scan_list(root, splan->values_lists,
								  rtoffset, 1);
			}
			break;
		case T_CteScan:
			{
				CteScan    *splan = (CteScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_NamedTuplestoreScan:
			{
				NamedTuplestoreScan *splan = (NamedTuplestoreScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_WorkTableScan:
			{
				WorkTableScan *splan = (WorkTableScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(root, splan->scan.plan.targetlist,
								  rtoffset, NUM_EXEC_TLIST(plan));
				splan->scan.plan.qual =
					fix_scan_list(root, splan->scan.plan.qual,
								  rtoffset, NUM_EXEC_QUAL(plan));
			}
			break;
		case T_ForeignScan:
			set_foreignscan_references(root, (ForeignScan *) plan, rtoffset);
			break;
		case T_CustomScan:
			set_customscan_references(root, (CustomScan *) plan, rtoffset);
			break;

		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			set_join_references(root, (Join *) plan, rtoffset);
			break;

		case T_Gather:
		case T_GatherMerge:
			{
				set_upper_references(root, plan, rtoffset);
				set_param_references(root, plan);
			}
			break;

		case T_Hash:
			set_hash_references(root, plan, rtoffset);
			break;

		case T_Memoize:
			{
				Memoize    *mplan = (Memoize *) plan;

				/*
				 * Memoize 不求值自己的目标列表。它只用
				 * 外层子节点的同一份目标列表。
				 */
				set_dummy_tlist_references(plan, rtoffset);

				mplan->param_exprs = fix_scan_list(root, mplan->param_exprs,
												   rtoffset,
												   NUM_EXEC_TLIST(plan));
				break;
			}

		case T_Material:
		case T_Sort:
		case T_IncrementalSort:
		case T_Unique:
		case T_SetOp:

			/*
			 * 这些计划类型实际上并不费心求值自己的
			 * 目标列表，因为它们只是返回未经修改的输入
			 * 元组。即使执行器不会用到目标列表，我们仍修正它，
			 * 以备 EXPLAIN 使用（更
			 * 不必说便于调试——错误的 varno 非常容易让人糊涂）。
			 */
			set_dummy_tlist_references(plan, rtoffset);

			/*
			 * 由于这些计划类型也不检查限定条件，不应当
			 * 发现挂在它们上面的限定表达式。
			 */
			Assert(plan->qual == NIL);
			break;
		case T_LockRows:
			{
				LockRows   *splan = (LockRows *) plan;

				/*
				 * 与上面那些计划类型一样，LockRows 不求值自己的
				 * tlist 或限定条件。但必须修正其 rowmarks 里的
				 * RT 下标。
				 */
				set_dummy_tlist_references(plan, rtoffset);
				Assert(splan->plan.qual == NIL);

				foreach(l, splan->rowMarks)
				{
					PlanRowMark *rc = (PlanRowMark *) lfirst(l);

					rc->rti += rtoffset;
					rc->prti += rtoffset;
				}
			}
			break;
		case T_Limit:
			{
				Limit	   *splan = (Limit *) plan;

				/*
				 * 与上面那些计划类型一样，Limit 不求值自己的 tlist
				 * 或限定条件。不过它有活的 limit/offset 表达式；
				 * 而这些无法包含子计划变量引用，因此
				 * fix_scan_expr 对它们适用。
				 */
				set_dummy_tlist_references(plan, rtoffset);
				Assert(splan->plan.qual == NIL);

				splan->limitOffset =
					fix_scan_expr(root, splan->limitOffset, rtoffset, 1);
				splan->limitCount =
					fix_scan_expr(root, splan->limitCount, rtoffset, 1);
			}
			break;
		case T_Agg:
			{
				Agg		   *agg = (Agg *) plan;

				/*
				 * 若本节点在合并部分聚合的结果，就必须
				 * 把它的 Aggref 转换成包含对那些
				 * 将从子计划节点得到的部分聚合子表达式的
				 * 引用。
				 */
				if (DO_AGGSPLIT_COMBINE(agg->aggsplit))
				{
					plan->targetlist = (List *)
						convert_combining_aggrefs((Node *) plan->targetlist,
												  NULL);
					plan->qual = (List *)
						convert_combining_aggrefs((Node *) plan->qual,
												  NULL);
				}

				set_upper_references(root, plan, rtoffset);
			}
			break;
		case T_Group:
			set_upper_references(root, plan, rtoffset);
			break;
		case T_WindowAgg:
			{
				WindowAgg  *wplan = (WindowAgg *) plan;

				/*
				 * 调整 WindowAgg 的运行条件，把
				 * WindowFuncs 引用换成扫描槽里的 Var，
				 * 这样执行器求值
				 * runCondition 时，拿到的是结果刚刚存入的那个槽里的
				 * WindowFunc 值，而不是
				 * 把 WindowFunc 再求值一遍。
				 */
				wplan->runCondition = set_windowagg_runcondition_references(root,
																			wplan->runCondition,
																			(Plan *) wplan);

				set_upper_references(root, plan, rtoffset);

				/*
				 * 与 Limit 节点的 limit/offset 表达式一样，WindowAgg 有
				 * 帧偏移表达式，它们无法包含子计划
				 * 变量引用，因此 fix_scan_expr 对它们适用。
				 */
				wplan->startOffset =
					fix_scan_expr(root, wplan->startOffset, rtoffset, 1);
				wplan->endOffset =
					fix_scan_expr(root, wplan->endOffset, rtoffset, 1);
				wplan->runCondition = fix_scan_list(root,
													wplan->runCondition,
													rtoffset,
													NUM_EXEC_TLIST(plan));
				wplan->runConditionOrig = fix_scan_list(root,
														wplan->runConditionOrig,
														rtoffset,
														NUM_EXEC_TLIST(plan));
			}
			break;
		case T_Result:
			{
				Result	   *splan = (Result *) plan;

				/*
				 * Result 也许有子计划，也许没有；若没有，它更像
				 * 扫描节点而不是上层节点。
				 */
				if (splan->plan.lefttree != NULL)
					set_upper_references(root, plan, rtoffset);
				else
				{
					/*
					 * 无子节点的 Result 的 tlist 里可能含有
					 * 尚未解析的 ROWID_VAR Var，当它表示一个因
					 * 约束排除而完全为空的
					 * 目标关系时。把任何这类 Var 换成空
					 * 常量，就像它们已为一个不支持它们的叶子
					 * 扫描节点解析过一样。可以让
					 * fix_scan_expr 来做，但由于这种情况只
					 * 预期出现在这里，在这里专门处理、并保持
					 * fix_scan_expr 不应当见到 ROWID_VARs
					 * 的断言，似乎更安全。
					 */
					foreach(l, splan->plan.targetlist)
					{
						TargetEntry *tle = (TargetEntry *) lfirst(l);
						Var		   *var = (Var *) tle->expr;

						if (var && IsA(var, Var) && var->varno == ROWID_VAR)
							tle->expr = (Expr *) makeNullConst(var->vartype,
															   var->vartypmod,
															   var->varcollid);
					}

					splan->plan.targetlist =
						fix_scan_list(root, splan->plan.targetlist,
									  rtoffset, NUM_EXEC_TLIST(plan));
					splan->plan.qual =
						fix_scan_list(root, splan->plan.qual,
									  rtoffset, NUM_EXEC_QUAL(plan));
				}
				/* resconstantqual 不能包含任何子计划变量引用 */
				splan->resconstantqual =
					fix_scan_expr(root, splan->resconstantqual, rtoffset, 1);
			}
			break;
		case T_ProjectSet:
			set_upper_references(root, plan, rtoffset);
			break;
		case T_ModifyTable:
			{
				ModifyTable *splan = (ModifyTable *) plan;
				Plan	   *subplan = outerPlan(splan);

				Assert(splan->plan.targetlist == NIL);
				Assert(splan->plan.qual == NIL);

				splan->withCheckOptionLists =
					fix_scan_list(root, splan->withCheckOptionLists,
								  rtoffset, 1);

				if (splan->returningLists)
				{
					List	   *newRL = NIL;
					ListCell   *lcrl,
							   *lcrr;

					/*
					 * 把每个按结果关系的 returningList 传给
					 * set_returning_clause_references()。
					 */
					Assert(list_length(splan->returningLists) == list_length(splan->resultRelations));
					forboth(lcrl, splan->returningLists,
							lcrr, splan->resultRelations)
					{
						List	   *rlist = (List *) lfirst(lcrl);
						Index		resultrel = lfirst_int(lcrr);

						rlist = set_returning_clause_references(root,
																rlist,
																subplan,
																resultrel,
																rtoffset);
						newRL = lappend(newRL, rlist);
					}
					splan->returningLists = newRL;

					/*
					 * 把可见的计划目标列表设成与
					 * 第一份 RETURNING 列表相同。这是给
					 * EXPLAIN 用的；执行器不会理会
					 * 目标列表。把这一步推迟到这里，这样就不必对相同的目标列表
					 * 做两次
					 * set_returning_clause_references()。
					 */
					splan->plan.targetlist = copyObject(linitial(newRL));
				}

				/*
				 * 把带 ON CONFLICT 的 ModifyTable 当作一种 "伪
				 * 连接"，其内侧是 EXCLUDED 元组。
				 * 因此用 fix_join_expr 把相关变量
				 * 设成 INNER_VAR。我们明确不建立任何 OUTER_VARs，因为
				 * 那些已经被 RETURNING 占用，不互相
				 * 冲突似乎更好。
				 */
				if (splan->onConflictSet)
				{
					indexed_tlist *itlist;

					itlist = build_tlist_index(splan->exclRelTlist);

					splan->onConflictSet =
						fix_join_expr(root, splan->onConflictSet,
									  NULL, itlist,
									  linitial_int(splan->resultRelations),
									  rtoffset, NRM_EQUAL, NUM_EXEC_QUAL(plan));

					splan->onConflictWhere = (Node *)
						fix_join_expr(root, (List *) splan->onConflictWhere,
									  NULL, itlist,
									  linitial_int(splan->resultRelations),
									  rtoffset, NRM_EQUAL, NUM_EXEC_QUAL(plan));

					pfree(itlist);

					splan->exclRelTlist =
						fix_scan_list(root, splan->exclRelTlist, rtoffset, 1);
				}

				/*
				 * MERGE 语句通过在目标关系与源
				 * 关系（可以是普通关系或子查询）之间做右连接，来产生目标行。
				 * 该 MERGE 语句的 INSERT 和 UPDATE 动作
				 * 需要访问源关系的列。我们
				 * 把事情安排成源关系属性可作为
				 * INNER_VAR，而目标关系属性
				 * 可从扫描元组
				 * 取得。
				 */
				if (splan->mergeActionLists != NIL)
				{
					ListCell   *lca,
							   *lcr;

					/*
					 * 修正各个动作节点的 targetList，使
					 * 所谓 "源关系" 的 Var 被引用为
					 * INNER_VAR。注意，要在
					 * 执行期间正确工作，必须把 ecxt_innertuple 设成
					 * 执行子计划得到的元组，那才
					 * 构成 "源关系"。
					 *
					 * 结果关系（即
					 * 目标关系）的 Var 保持不变，也就是说那些 Var 会从
					 * 扫描槽取出。因此执行期间必须
					 * 确保 ecxt_scantuple 设置正确，指向
					 * 目标关系的元组。
					 */
					indexed_tlist *itlist;

					itlist = build_tlist_index(subplan->targetlist);

					forboth(lca, splan->mergeActionLists,
							lcr, splan->resultRelations)
					{
						List	   *mergeActionList = lfirst(lca);
						Index		resultrel = lfirst_int(lcr);

						foreach(l, mergeActionList)
						{
							MergeAction *action = (MergeAction *) lfirst(l);

							/* 修正每个动作的 targetList。 */
							action->targetList = fix_join_expr(root,
															   action->targetList,
															   NULL, itlist,
															   resultrel,
															   rtoffset,
															   NRM_EQUAL,
															   NUM_EXEC_TLIST(plan));

							/* 也修正限定条件。 */
							action->qual = (Node *) fix_join_expr(root,
																  (List *) action->qual,
																  NULL, itlist,
																  resultrel,
																  rtoffset,
																  NRM_EQUAL,
																  NUM_EXEC_QUAL(plan));
						}
					}
				}

				splan->nominalRelation += rtoffset;
				if (splan->rootRelation)
					splan->rootRelation += rtoffset;
				splan->exclRelRTI += rtoffset;

				foreach(l, splan->resultRelations)
				{
					lfirst_int(l) += rtoffset;
				}
				foreach(l, splan->rowMarks)
				{
					PlanRowMark *rc = (PlanRowMark *) lfirst(l);

					rc->rti += rtoffset;
					rc->prti += rtoffset;
				}

				/*
				 * 把本 ModifyTable 节点最终结果关系的 RT
				 * 下标追加到该计划的全局列表。
				 */
				root->glob->resultRelations =
					list_concat(root->glob->resultRelations,
								splan->resultRelations);
				if (splan->rootRelation)
				{
					root->glob->resultRelations =
						lappend_int(root->glob->resultRelations,
									splan->rootRelation);
				}
			}
			break;
		case T_Append:
			/* 需要特殊处理，见下面的注释 */
			return set_append_references(root,
										 (Append *) plan,
										 rtoffset);
		case T_MergeAppend:
			/* 需要特殊处理，见下面的注释 */
			return set_mergeappend_references(root,
											  (MergeAppend *) plan,
											  rtoffset);
		case T_RecursiveUnion:
			/* 它也不求值目标列表或检查限定条件 */
			set_dummy_tlist_references(plan, rtoffset);
			Assert(plan->qual == NIL);
			break;
		case T_BitmapAnd:
			{
				BitmapAnd  *splan = (BitmapAnd *) plan;

				/* BitmapAnd 的工作方式像 Append，但没有 tlist */
				Assert(splan->plan.targetlist == NIL);
				Assert(splan->plan.qual == NIL);
				foreach(l, splan->bitmapplans)
				{
					lfirst(l) = set_plan_refs(root,
											  (Plan *) lfirst(l),
											  rtoffset);
				}
			}
			break;
		case T_BitmapOr:
			{
				BitmapOr   *splan = (BitmapOr *) plan;

				/* BitmapOr 的工作方式像 Append，但没有 tlist */
				Assert(splan->plan.targetlist == NIL);
				Assert(splan->plan.qual == NIL);
				foreach(l, splan->bitmapplans)
				{
					lfirst(l) = set_plan_refs(root,
											  (Plan *) lfirst(l),
											  rtoffset);
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(plan));
			break;
	}

	/*
	 * 现在递归进入子计划，若有的话
	 *
	 * 注意：必须在设置
	 * 本计划 tlist 和限定条件里的子计划引用 AFTER 才递归进子计划。若自底向上做
	 * 引用调整，就无法把本
	 * 计划的 var 节点与子节点已经改过的节点匹配上。
	 */
	plan->lefttree = set_plan_refs(root, plan->lefttree, rtoffset);
	plan->righttree = set_plan_refs(root, plan->righttree, rtoffset);

	return plan;
}

/*
 * set_indexonlyscan_references
 *		对 IndexOnlyScan 做 set_plan_references 处理
 *
 * 这与普通 IndexScan 的处理不同，因为必须把
 * 引用堆的 Var 转换成引用索引的 Var。
 * 可以用 fix_upper_expr 机制来做，依据一份
 * 描述索引列的目标列表。
 */
static Plan *
set_indexonlyscan_references(PlannerInfo *root,
							 IndexOnlyScan *plan,
							 int rtoffset)
{
	indexed_tlist *index_itlist;
	List	   *stripped_indextlist;
	ListCell   *lc;

	/*
	 * 计划节点的目标列表、限定条件和 recheckqual 里的 Var 只能
	 * 引用索引访问方法实际能返回的列。为确保
	 * 这一点，从索引 tlist 里去掉不可返回的列（它们被标成 resjunk）。
	 * 可以直接丢掉，因为 indexed_tlist
	 * 机制看的是 TLE 的 resno，不是物理列表位置。
	 */
	stripped_indextlist = NIL;
	foreach(lc, plan->indextlist)
	{
		TargetEntry *indextle = (TargetEntry *) lfirst(lc);

		if (!indextle->resjunk)
			stripped_indextlist = lappend(stripped_indextlist, indextle);
	}

	index_itlist = build_tlist_index(stripped_indextlist);

	plan->scan.scanrelid += rtoffset;
	plan->scan.plan.targetlist = (List *)
		fix_upper_expr(root,
					   (Node *) plan->scan.plan.targetlist,
					   index_itlist,
					   INDEX_VAR,
					   rtoffset,
					   NRM_EQUAL,
					   NUM_EXEC_TLIST((Plan *) plan));
	plan->scan.plan.qual = (List *)
		fix_upper_expr(root,
					   (Node *) plan->scan.plan.qual,
					   index_itlist,
					   INDEX_VAR,
					   rtoffset,
					   NRM_EQUAL,
					   NUM_EXEC_QUAL((Plan *) plan));
	plan->recheckqual = (List *)
		fix_upper_expr(root,
					   (Node *) plan->recheckqual,
					   index_itlist,
					   INDEX_VAR,
					   rtoffset,
					   NRM_EQUAL,
					   NUM_EXEC_QUAL((Plan *) plan));
	/* indexqual 已经转换成引用索引列 */
	plan->indexqual = fix_scan_list(root, plan->indexqual,
									rtoffset, 1);
	/* indexorderby 已经转换成引用索引列 */
	plan->indexorderby = fix_scan_list(root, plan->indexorderby,
									   rtoffset, 1);
	/* indextlist 不得被改成引用索引列 */
	plan->indextlist = fix_scan_list(root, plan->indextlist,
									 rtoffset, NUM_EXEC_TLIST((Plan *) plan));

	pfree(index_itlist);

	return (Plan *) plan;
}

/*
 * set_subqueryscan_references
 *		对 SubqueryScan 做 set_plan_references 处理
 *
 * 我们试图把 SubqueryScan 整个剥掉；若做不到，就得
 * 对它做正常处理。
 */
static Plan *
set_subqueryscan_references(PlannerInfo *root,
							SubqueryScan *plan,
							int rtoffset)
{
	RelOptInfo *rel;
	Plan	   *result;

	/* 需要查出该子查询的 RelOptInfo，因为需要它的 subroot */
	rel = find_base_rel(root, plan->scan.scanrelid);

	/* 递归处理子计划 */
	plan->subplan = set_plan_references(rel->subroot, plan->subplan);

	if (trivial_subqueryscan(plan))
	{
		/*
		 * 可以省略 SubqueryScan 节点，直接上拉子计划。
		 */
		result = clean_up_removed_plan_level((Plan *) plan, plan->subplan);
	}
	else
	{
		/*
		 * 保留 SubqueryScan 节点。必须做
		 * set_plan_references 本来会对它做的处理。注意这里
		 * 不做 set_upper_references()，因为 SubqueryScan
		 * 一开始建立时，对子计划输出的引用
		 * 就已经是对的。
		 */
		plan->scan.scanrelid += rtoffset;
		plan->scan.plan.targetlist =
			fix_scan_list(root, plan->scan.plan.targetlist,
						  rtoffset, NUM_EXEC_TLIST((Plan *) plan));
		plan->scan.plan.qual =
			fix_scan_list(root, plan->scan.plan.qual,
						  rtoffset, NUM_EXEC_QUAL((Plan *) plan));

		result = (Plan *) plan;
	}

	return result;
}

/*
 * trivial_subqueryscan
 *		检测能否从计划树里删掉一个 SubqueryScan。
 *
 * 若它没有要检查的限定条件，且目标列表只是
 * 原样吐出子计划的输出，就可以删。
 *
 * create_append_plan() 的辅助函数 mark_async_capable_plan() 可以在
 * set_subqueryscan_references() 之前调用本函数，以判断
 * 作为 Append 节点子项的 SubqueryScan 是否平凡。因此我们
 * 把结果缓存在 SubqueryScan 节点里，避免重复计算。
 *
 * 注意：从 mark_async_capable_plan() 调用时，我们在
 * 对 SubqueryScan 节点运行 finalize_plan()（若需要）以及对子计划树运行
 * set_plan_references() 之前就判定结果，但这是安全的，因为
 * 1) finalize_plan() 不修改该 SubqueryScan
 *	  节点（或子计划树中任何计划节点）的 tlist 或限定条件，并且
 * 2) set_plan_references() 会修改子计划树中每个计划节点的 tlist，
 *	  但把 const/resjunk 列保持为 const/resjunk，并
 *	  保持 tlist 的长度和顺序，并且
 * 3) set_plan_references() 也许会从子计划树删掉最顶层的计划节点，例如 Append
 *	  或 MergeAppend，并上拉子计划节点，
 *	  但那种情况下，子计划节点的 tlist 与
 *	  父项完全一致。
 */
bool
trivial_subqueryscan(SubqueryScan *plan)
{
	int			attrno;
	ListCell   *lp,
			   *lc;

	/* 也许已经检测过；那种情况下复用结果 */
	if (plan->scanstatus == SUBQUERY_SCAN_TRIVIAL)
		return true;
	if (plan->scanstatus == SUBQUERY_SCAN_NONTRIVIAL)
		return false;
	Assert(plan->scanstatus == SUBQUERY_SCAN_UNKNOWN);
	/* 起初把该 SubqueryScan 标成不能从计划树删除 */
	plan->scanstatus = SUBQUERY_SCAN_NONTRIVIAL;

	if (plan->scan.plan.qual != NIL)
		return false;

	if (list_length(plan->scan.plan.targetlist) !=
		list_length(plan->subplan->targetlist))
		return false;			/* tlist 长度不同 */

	attrno = 1;
	forboth(lp, plan->scan.plan.targetlist, lc, plan->subplan->targetlist)
	{
		TargetEntry *ptle = (TargetEntry *) lfirst(lp);
		TargetEntry *ctle = (TargetEntry *) lfirst(lc);

		if (ptle->resjunk != ctle->resjunk)
			return false;		/* tlist 的 junk 状态对不上 */

		/*
		 * 我们接受引用子计划 tlist 对应元素的 Var，
		 * 或等于该子计划元素的 Const。动机见
		 * generate_setop_tlist()。
		 */
		if (ptle->expr && IsA(ptle->expr, Var))
		{
			Var		   *var = (Var *) ptle->expr;

			Assert(var->varno == plan->scan.scanrelid);
			Assert(var->varlevelsup == 0);
			if (var->varattno != attrno)
				return false;	/* 顺序不对 */
		}
		else if (ptle->expr && IsA(ptle->expr, Const))
		{
			if (!equal(ptle->expr, ctle->expr))
				return false;
		}
		else
			return false;

		attrno++;
	}

	/* 把该 SubqueryScan 重新标成可从计划树删除 */
	plan->scanstatus = SUBQUERY_SCAN_TRIVIAL;

	return true;
}

/*
 * clean_up_removed_plan_level
 *		剥掉 SubqueryScan、Append 等时做必要的清理
 *
 * 我们丢掉 "父" 计划，只返回它的 "子" 计划。
 * 需要做几处小调整。
 */
static Plan *
clean_up_removed_plan_level(Plan *parent, Plan *child)
{
	/*
	 * 必须确保不丢失任何 initplan，因此把挂在
	 * 父计划上的那些挪到子计划。若挪动了任何 initplan，子计划
	 * 就不再是并行安全的。
	 */
	if (parent->initPlan)
		child->parallel_safe = false;

	/*
	 * 按这种方式挂接计划，使父项的 initplan 在
	 * 子计划任何已有 initplan 之前被处理。大概无所谓，但
	 * 为防万一还是保持这个顺序。
	 */
	child->initPlan = list_concat(parent->initPlan,
								  child->initPlan);

	/*
	 * 还必须把父项的列标注信息转给
	 * 子项，否则若这是最顶层计划，发给客户端的列
	 * 会被错误标注。resjunk 之类也许也很重要。
	 */
	apply_tlist_labeling(child->targetlist, parent->targetlist);

	return child;
}

/*
 * set_foreignscan_references
 *	   对 ForeignScan 做 set_plan_references 处理
 */
static void
set_foreignscan_references(PlannerInfo *root,
						   ForeignScan *fscan,
						   int rtoffset)
{
	/* 若 scanrelid 有效，就调整它 */
	if (fscan->scan.scanrelid > 0)
		fscan->scan.scanrelid += rtoffset;

	if (fscan->fdw_scan_tlist != NIL || fscan->scan.scanrelid == 0)
	{
		/*
		 * 调整 tlist、qual、fdw_exprs、fdw_recheck_quals，使之引用
		 * 外部扫描元组
		 */
		indexed_tlist *itlist = build_tlist_index(fscan->fdw_scan_tlist);

		fscan->scan.plan.targetlist = (List *)
			fix_upper_expr(root,
						   (Node *) fscan->scan.plan.targetlist,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_TLIST((Plan *) fscan));
		fscan->scan.plan.qual = (List *)
			fix_upper_expr(root,
						   (Node *) fscan->scan.plan.qual,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_QUAL((Plan *) fscan));
		fscan->fdw_exprs = (List *)
			fix_upper_expr(root,
						   (Node *) fscan->fdw_exprs,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_QUAL((Plan *) fscan));
		fscan->fdw_recheck_quals = (List *)
			fix_upper_expr(root,
						   (Node *) fscan->fdw_recheck_quals,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_QUAL((Plan *) fscan));
		pfree(itlist);
		/* fdw_scan_tlist 本身只需 fix_scan_list() 调整 */
		fscan->fdw_scan_tlist =
			fix_scan_list(root, fscan->fdw_scan_tlist,
						  rtoffset, NUM_EXEC_TLIST((Plan *) fscan));
	}
	else
	{
		/*
		 * 按标准方式调整 tlist、qual、
		 * fdw_exprs 和 fdw_recheck_quals
		 */
		fscan->scan.plan.targetlist =
			fix_scan_list(root, fscan->scan.plan.targetlist,
						  rtoffset, NUM_EXEC_TLIST((Plan *) fscan));
		fscan->scan.plan.qual =
			fix_scan_list(root, fscan->scan.plan.qual,
						  rtoffset, NUM_EXEC_QUAL((Plan *) fscan));
		fscan->fdw_exprs =
			fix_scan_list(root, fscan->fdw_exprs,
						  rtoffset, NUM_EXEC_QUAL((Plan *) fscan));
		fscan->fdw_recheck_quals =
			fix_scan_list(root, fscan->fdw_recheck_quals,
						  rtoffset, NUM_EXEC_QUAL((Plan *) fscan));
	}

	fscan->fs_relids = offset_relid_set(fscan->fs_relids, rtoffset);
	fscan->fs_base_relids = offset_relid_set(fscan->fs_base_relids, rtoffset);

	/* 若 resultRelation 有效，就调整它 */
	if (fscan->resultRelation > 0)
		fscan->resultRelation += rtoffset;
}

/*
 * set_customscan_references
 *	   对 CustomScan 做 set_plan_references 处理
 */
static void
set_customscan_references(PlannerInfo *root,
						  CustomScan *cscan,
						  int rtoffset)
{
	ListCell   *lc;

	/* 若 scanrelid 有效，就调整它 */
	if (cscan->scan.scanrelid > 0)
		cscan->scan.scanrelid += rtoffset;

	if (cscan->custom_scan_tlist != NIL || cscan->scan.scanrelid == 0)
	{
		/* 调整 tlist、qual、custom_exprs，使之引用自定义扫描元组 */
		indexed_tlist *itlist = build_tlist_index(cscan->custom_scan_tlist);

		cscan->scan.plan.targetlist = (List *)
			fix_upper_expr(root,
						   (Node *) cscan->scan.plan.targetlist,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_TLIST((Plan *) cscan));
		cscan->scan.plan.qual = (List *)
			fix_upper_expr(root,
						   (Node *) cscan->scan.plan.qual,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_QUAL((Plan *) cscan));
		cscan->custom_exprs = (List *)
			fix_upper_expr(root,
						   (Node *) cscan->custom_exprs,
						   itlist,
						   INDEX_VAR,
						   rtoffset,
						   NRM_EQUAL,
						   NUM_EXEC_QUAL((Plan *) cscan));
		pfree(itlist);
		/* custom_scan_tlist 本身只需 fix_scan_list() 调整 */
		cscan->custom_scan_tlist =
			fix_scan_list(root, cscan->custom_scan_tlist,
						  rtoffset, NUM_EXEC_TLIST((Plan *) cscan));
	}
	else
	{
		/* 按标准方式调整 tlist、qual、custom_exprs */
		cscan->scan.plan.targetlist =
			fix_scan_list(root, cscan->scan.plan.targetlist,
						  rtoffset, NUM_EXEC_TLIST((Plan *) cscan));
		cscan->scan.plan.qual =
			fix_scan_list(root, cscan->scan.plan.qual,
						  rtoffset, NUM_EXEC_QUAL((Plan *) cscan));
		cscan->custom_exprs =
			fix_scan_list(root, cscan->custom_exprs,
						  rtoffset, NUM_EXEC_QUAL((Plan *) cscan));
	}

	/* 若需要，递归调整子计划节点 */
	foreach(lc, cscan->custom_plans)
	{
		lfirst(lc) = set_plan_refs(root, (Plan *) lfirst(lc), rtoffset);
	}

	cscan->custom_relids = offset_relid_set(cscan->custom_relids, rtoffset);
}

/*
 * set_append_references
 *		对 Append 做 set_plan_references 处理
 *
 * 我们试图把 Append 整个剥掉；若做不到，就得
 * 对它做正常处理。
 */
static Plan *
set_append_references(PlannerInfo *root,
					  Append *aplan,
					  int rtoffset)
{
	ListCell   *l;

	/*
	 * Append 像 Sort 等一样，实际上不求值自己的目标列表，也不
	 * 检查限定条件。若它恰好只有一个子计划，那就完全
	 * 没做任何有用的事，可以把它剥掉。
	 */
	Assert(aplan->plan.qual == NIL);

	/* 首先，得对子项递归 */
	foreach(l, aplan->appendplans)
	{
		lfirst(l) = set_plan_refs(root, (Plan *) lfirst(l), rtoffset);
	}

	/*
	 * 看是否可以安全地整个去掉 Append。要安全，
	 * 必须只有一个子计划，且该子计划的并行
	 * 感知必须与 Append 一致。后者的原因是，
	 * 若 Append 是并行感知的而子项不是，则调用方
	 * 计划也许会把这个非并行感知的子项执行多次。（若
	 * 改这些规则，也要更新 create_append_path 使之一致。）
	 */
	if (list_length(aplan->appendplans) == 1)
	{
		Plan	   *p = (Plan *) linitial(aplan->appendplans);

		if (p->parallel_aware == aplan->plan.parallel_aware)
			return clean_up_removed_plan_level((Plan *) aplan, p);
	}

	/*
	 * 否则，按需要清理 Append。可以在
	 * 对子项递归之后做，因为 set_dummy_tlist_references 并不
	 * 看那些。
	 */
	set_dummy_tlist_references((Plan *) aplan, rtoffset);

	aplan->apprelids = offset_relid_set(aplan->apprelids, rtoffset);

	if (aplan->part_prune_info)
	{
		foreach(l, aplan->part_prune_info->prune_infos)
		{
			List	   *prune_infos = lfirst(l);
			ListCell   *l2;

			foreach(l2, prune_infos)
			{
				PartitionedRelPruneInfo *pinfo = lfirst(l2);

				pinfo->rtindex += rtoffset;
				pinfo->initial_pruning_steps =
					fix_scan_list(root, pinfo->initial_pruning_steps,
								  rtoffset, 1);
				pinfo->exec_pruning_steps =
					fix_scan_list(root, pinfo->exec_pruning_steps,
								  rtoffset, 1);
			}
		}
	}

	/* 不必递归到 lefttree 或 righttree…… */
	Assert(aplan->plan.lefttree == NULL);
	Assert(aplan->plan.righttree == NULL);

	return (Plan *) aplan;
}

/*
 * set_mergeappend_references
 *		对 MergeAppend 做 set_plan_references 处理
 *
 * 我们试图把 MergeAppend 整个剥掉；若做不到，就得
 * 对它做正常处理。
 */
static Plan *
set_mergeappend_references(PlannerInfo *root,
						   MergeAppend *mplan,
						   int rtoffset)
{
	ListCell   *l;

	/*
	 * MergeAppend 像 Sort 等一样，实际上不求值自己的目标列表，
	 * 也不检查限定条件。若它恰好只有一个子计划，那就完全
	 * 没做任何有用的事，可以把它剥掉。
	 */
	Assert(mplan->plan.qual == NIL);

	/* 首先，得对子项递归 */
	foreach(l, mplan->mergeplans)
	{
		lfirst(l) = set_plan_refs(root, (Plan *) lfirst(l), rtoffset);
	}

	/*
	 * 看是否可以安全地整个去掉 MergeAppend。要安全，
	 * 必须只有一个子计划，且该子计划的
	 * 并行感知必须与 MergeAppend 一致。后者的原因是，
	 * 若 MergeAppend 是并行感知的而子项
	 * 不是，则调用方计划也许会把这个非并行感知的子项
	 * 执行多次。（若改这些规则，也要更新
	 * create_merge_append_path 使之一致。）
	 */
	if (list_length(mplan->mergeplans) == 1)
	{
		Plan	   *p = (Plan *) linitial(mplan->mergeplans);

		if (p->parallel_aware == mplan->plan.parallel_aware)
			return clean_up_removed_plan_level((Plan *) mplan, p);
	}

	/*
	 * 否则，按需要清理 MergeAppend。可以在
	 * 对子项递归之后做，因为 set_dummy_tlist_references
	 * 并不看那些。
	 */
	set_dummy_tlist_references((Plan *) mplan, rtoffset);

	mplan->apprelids = offset_relid_set(mplan->apprelids, rtoffset);

	if (mplan->part_prune_info)
	{
		foreach(l, mplan->part_prune_info->prune_infos)
		{
			List	   *prune_infos = lfirst(l);
			ListCell   *l2;

			foreach(l2, prune_infos)
			{
				PartitionedRelPruneInfo *pinfo = lfirst(l2);

				pinfo->rtindex += rtoffset;
				pinfo->initial_pruning_steps =
					fix_scan_list(root, pinfo->initial_pruning_steps,
								  rtoffset, 1);
				pinfo->exec_pruning_steps =
					fix_scan_list(root, pinfo->exec_pruning_steps,
								  rtoffset, 1);
			}
		}
	}

	/* 不必递归到 lefttree 或 righttree…… */
	Assert(mplan->plan.lefttree == NULL);
	Assert(mplan->plan.righttree == NULL);

	return (Plan *) mplan;
}

/*
 * set_hash_references
 *	   对 Hash 节点做 set_plan_references 处理
 */
static void
set_hash_references(PlannerInfo *root, Plan *plan, int rtoffset)
{
	Hash	   *hplan = (Hash *) plan;
	Plan	   *outer_plan = plan->lefttree;
	indexed_tlist *outer_itlist;

	/*
	 * Hash 的 hashkeys 在把元组送进哈希表时使用，
	 * 因此让它们引用 Hash 的外层计划（它本身是
	 * HashJoin 的内层计划）。
	 */
	outer_itlist = build_tlist_index(outer_plan->targetlist);
	hplan->hashkeys = (List *)
		fix_upper_expr(root,
					   (Node *) hplan->hashkeys,
					   outer_itlist,
					   OUTER_VAR,
					   rtoffset,
					   NRM_EQUAL,
					   NUM_EXEC_QUAL(plan));

	/* Hash 不做投影 */
	set_dummy_tlist_references(plan, rtoffset);

	/* Hash 节点没有自己的限定条件 */
	Assert(plan->qual == NIL);
}

/*
 * offset_relid_set
 *		对 Relids 集合的成员应用 rtoffset。
 */
static Relids
offset_relid_set(Relids relids, int rtoffset)
{
	Relids		result = NULL;
	int			rtindex;

	/* 若没有要应用的偏移，就不必重新计算该值 */
	if (rtoffset == 0)
		return relids;
	rtindex = -1;
	while ((rtindex = bms_next_member(relids, rtindex)) >= 0)
		result = bms_add_member(result, rtindex + rtoffset);
	return result;
}

/*
 * copyVar
 *		复制一个 Var 节点。
 *
 * fix_scan_expr 及其同类做这件事的次数够多，值得有
 * 一个专用例程，而不是用通用的 copyObject() 函数。
 */
static inline Var *
copyVar(Var *var)
{
	Var		   *newvar = (Var *) palloc(sizeof(Var));

	*newvar = *var;
	return newvar;
}

/*
 * fix_expr_common
 *		对表达式节点做通用的 set_plan_references 处理
 *
 * 这是各种表达式修正变体共用的代码。
 * 必须为 OpExpr 及相关节点查找操作符操作码信息，
 * 把 regclass Const 节点的 OIDs 加进 root->glob->relationOids，并
 * 把用户定义函数的 PlanInvalItems 加进 root->glob->invalItems。
 * 我们也填写 GROUPING() 表达式的列下标列表。
 *
 * 假定就地更新操作码信息是可以的。因此这也许会
 * 涂改规划器的输入数据结构，但没关系。
 */
static void
fix_expr_common(PlannerInfo *root, Node *node)
{
	/* 假定调用方不会对 NULL 指针调用我们 */
	if (IsA(node, Aggref))
	{
		record_plan_function_dependency(root,
										((Aggref *) node)->aggfnoid);
	}
	else if (IsA(node, WindowFunc))
	{
		record_plan_function_dependency(root,
										((WindowFunc *) node)->winfnoid);
	}
	else if (IsA(node, FuncExpr))
	{
		record_plan_function_dependency(root,
										((FuncExpr *) node)->funcid);
	}
	else if (IsA(node, OpExpr))
	{
		set_opfuncid((OpExpr *) node);
		record_plan_function_dependency(root,
										((OpExpr *) node)->opfuncid);
	}
	else if (IsA(node, DistinctExpr))
	{
		set_opfuncid((OpExpr *) node);	/* 依赖结构体等价 */
		record_plan_function_dependency(root,
										((DistinctExpr *) node)->opfuncid);
	}
	else if (IsA(node, NullIfExpr))
	{
		set_opfuncid((OpExpr *) node);	/* 依赖结构体等价 */
		record_plan_function_dependency(root,
										((NullIfExpr *) node)->opfuncid);
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;

		set_sa_opfuncid(saop);
		record_plan_function_dependency(root, saop->opfuncid);

		if (OidIsValid(saop->hashfuncid))
			record_plan_function_dependency(root, saop->hashfuncid);

		if (OidIsValid(saop->negfuncid))
			record_plan_function_dependency(root, saop->negfuncid);
	}
	else if (IsA(node, Const))
	{
		Const	   *con = (Const *) node;

		/* 检查是否为 regclass 引用 */
		if (ISREGCLASSCONST(con))
			root->glob->relationOids =
				lappend_oid(root->glob->relationOids,
							DatumGetObjectId(con->constvalue));
	}
	else if (IsA(node, GroupingFunc))
	{
		GroupingFunc *g = (GroupingFunc *) node;
		AttrNumber *grouping_map = root->grouping_map;

		/* 若没有分组集，就不需要这个。 */

		Assert(grouping_map || g->cols == NIL);

		if (grouping_map)
		{
			ListCell   *lc;
			List	   *cols = NIL;

			foreach(lc, g->refs)
			{
				cols = lappend_int(cols, grouping_map[lfirst_int(lc)]);
			}

			Assert(!g->cols || equal(cols, g->cols));

			if (!g->cols)
				g->cols = cols;
		}
	}
}

/*
 * fix_param_node
 *		对 Param 做 set_plan_references 处理
 *
 * 若它是 PARAM_MULTIEXPR，就用 root->multiexpr_params 里的相应 Param 替换它；
 * 否则不必改动。
 * 只是为了多留个心，无论哪种情况都复制该节点。
 */
static Node *
fix_param_node(PlannerInfo *root, Param *p)
{
	if (p->paramkind == PARAM_MULTIEXPR)
	{
		int			subqueryid = p->paramid >> 16;
		int			colno = p->paramid & 0xFFFF;
		List	   *params;

		if (subqueryid <= 0 ||
			subqueryid > list_length(root->multiexpr_params))
			elog(ERROR, "unexpected PARAM_MULTIEXPR ID: %d", p->paramid);
		params = (List *) list_nth(root->multiexpr_params, subqueryid - 1);
		if (colno <= 0 || colno > list_length(params))
			elog(ERROR, "unexpected PARAM_MULTIEXPR ID: %d", p->paramid);
		return copyObject(list_nth(params, colno - 1));
	}
	return (Node *) copyObject(p);
}

/*
 * fix_alternative_subplan
 *		对 AlternativeSubPlan 做 set_plan_references 处理
 *
 * 选出一种备选实现，只返回那一个，
 * 丢掉 AlternativeSubPlan 结构的其余部分。
 * 注意：调用方仍必须递归进入结果！
 *
 * 我们完全不试图修正父计划
 * 节点或更高层节点里的代价估计。
 */
static Node *
fix_alternative_subplan(PlannerInfo *root, AlternativeSubPlan *asplan,
						double num_exec)
{
	SubPlan    *bestplan = NULL;
	Cost		bestcost = 0;
	ListCell   *lc;

	/*
	 * 假定执行 num_exec
	 * 次，计算每个子计划的估计代价，并保留最便宜的。若估计
	 * 完全相等，我们偏向较后的计划；这有点任意，但在
	 * 当前用法里，它使我们在平局时不选快速启动的子计划。
	 */
	Assert(asplan->subplans != NIL);

	foreach(lc, asplan->subplans)
	{
		SubPlan    *curplan = (SubPlan *) lfirst(lc);
		Cost		curcost;

		curcost = curplan->startup_cost + num_exec * curplan->per_call_cost;
		if (bestplan == NULL || curcost <= bestcost)
		{
			bestplan = curplan;
			bestcost = curcost;
		}

		/* 也标记 AlternativeSubPlans 里的全部子计划 */
		root->isAltSubplan[curplan->plan_id - 1] = true;
	}

	/* 标记我们选中的子计划 */
	root->isUsedSubplan[bestplan->plan_id - 1] = true;

	return (Node *) bestplan;
}

/*
 * fix_scan_expr
 *		对扫描层表达式做 set_plan_references 处理
 *
 * 这包括把全部 Var 的 varno 加上 rtoffset，
 * 替换 PARAM_MULTIEXPR 参数，展开 PlaceHolderVars，
 * 替换应当被 initplan 输出 Param 替换的 Aggref 节点，
 * 为 AlternativeSubPlans 选择最佳实现，
 * 为 OpExpr 及相关节点查找操作符操作码信息，
 * 并把 regclass Const 节点的 OIDs 加进 root->glob->relationOids。
 *
 * 'node'：要修改的表达式
 * 'rtoffset'：varno 要增加多少
 * 'num_exec'：表达式的估计执行次数
 *
 * 表达式树要么复制后修改，要么在看起来安全时
 * 就地修改。
 */
static Node *
fix_scan_expr(PlannerInfo *root, Node *node, int rtoffset, double num_exec)
{
	fix_scan_expr_context context;

	context.root = root;
	context.rtoffset = rtoffset;
	context.num_exec = num_exec;

	if (rtoffset != 0 ||
		root->multiexpr_params != NIL ||
		root->glob->lastPHId != 0 ||
		root->minmax_aggs != NIL ||
		root->hasAlternativeSubPlans)
	{
		return fix_scan_expr_mutator(node, &context);
	}
	else
	{
		/*
		 * 若 rtoffset == 0，就不必改任何 Var；若没有
		 * MULTIEXPR 子查询，就不必替换
		 * PARAM_MULTIEXPR 参数；若到处都没有占位符，
		 * 就不必去掉它们；若没有 minmax Aggref，
		 * 就不必替换它们；若没有 AlternativeSubPlans，
		 * 就不必去掉它们。那时可以直接涂改
		 * 输入节点树，而不必复制（因为唯一的改动是填上
		 * 任何未设置的 opfuncid 字段，这是无害的）。这省下的
		 * 周期在平凡查询上刚好能察觉到。
		 */
		(void) fix_scan_expr_walker(node, &context);
		return node;
	}
}

static Node *
fix_scan_expr_mutator(Node *node, fix_scan_expr_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = copyVar((Var *) node);

		Assert(var->varlevelsup == 0);

		/*
		 * 不应当见到标成 INNER_VAR、OUTER_VAR 或 ROWID_VAR 的 Var。
		 * 但 indexqual 表达式可以含有 INDEX_VAR Var。
		 */
		Assert(var->varno != INNER_VAR);
		Assert(var->varno != OUTER_VAR);
		Assert(var->varno != ROWID_VAR);
		if (!IS_SPECIAL_VARNO(var->varno))
			var->varno += context->rtoffset;
		if (var->varnosyn > 0)
			var->varnosyn += context->rtoffset;
		return (Node *) var;
	}
	if (IsA(node, Param))
		return fix_param_node(context->root, (Param *) node);
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		Param	   *aggparam;

		/* 看该 Aggref 是否应当被 Param 替换 */
		aggparam = find_minmax_agg_replacement_param(context->root, aggref);
		if (aggparam != NULL)
		{
			/* 为多留个心，复制一份 Param */
			return (Node *) copyObject(aggparam);
		}
		/* If no match, just fall through to process it normally */
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) copyObject(node);

		Assert(!IS_SPECIAL_VARNO(cexpr->cvarno));
		cexpr->cvarno += context->rtoffset;
		return (Node *) cexpr;
	}
	if (IsA(node, PlaceHolderVar))
	{
		/* 在扫描层，应当总是直接求值所含的表达式 */
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* XXX 能否对 phnullingrels 断言点什么？ */
		return fix_scan_expr_mutator((Node *) phv->phexpr, context);
	}
	if (IsA(node, AlternativeSubPlan))
		return fix_scan_expr_mutator(fix_alternative_subplan(context->root,
															 (AlternativeSubPlan *) node,
															 context->num_exec),
									 context);
	fix_expr_common(context->root, node);
	return expression_tree_mutator(node, fix_scan_expr_mutator,
								   (void *) context);
}

static bool
fix_scan_expr_walker(Node *node, fix_scan_expr_context *context)
{
	if (node == NULL)
		return false;
	Assert(!(IsA(node, Var) && ((Var *) node)->varno == ROWID_VAR));
	Assert(!IsA(node, PlaceHolderVar));
	Assert(!IsA(node, AlternativeSubPlan));
	fix_expr_common(context->root, node);
	return expression_tree_walker(node, fix_scan_expr_walker,
								  (void *) context);
}

/*
 * set_join_references
 *	  修改连接节点的目标列表和限定条件，使之引用其子计划，
 *	  做法是把 varno 设成 OUTER_VAR 或 INNER_VAR，并把
 *	  attno 设成对应的
 *	  外侧或内侧连接元组项的结果域号。也对这些
 *	  表达式做操作码查找，并把 regclass OIDs 加进 root->glob->relationOids。
 */
static void
set_join_references(PlannerInfo *root, Join *join, int rtoffset)
{
	Plan	   *outer_plan = join->plan.lefttree;
	Plan	   *inner_plan = join->plan.righttree;
	indexed_tlist *outer_itlist;
	indexed_tlist *inner_itlist;

	outer_itlist = build_tlist_index(outer_plan->targetlist);
	inner_itlist = build_tlist_index(inner_plan->targetlist);

	/*
	 * 先处理连接限定条件（包括归并或哈希子句）。它们
	 * 在逻辑上位于连接之下，因此总能使用输入 tlist 提供的
	 * 全部值。现在也处理
	 * NestLoopParams 是可以的，因为那些不能引用可空的
	 * 子表达式。
	 */
	join->joinqual = fix_join_expr(root,
								   join->joinqual,
								   outer_itlist,
								   inner_itlist,
								   (Index) 0,
								   rtoffset,
								   NRM_EQUAL,
								   NUM_EXEC_QUAL((Plan *) join));

	/* 现在做连接类型特有的事 */
	if (IsA(join, NestLoop))
	{
		NestLoop   *nl = (NestLoop *) join;
		ListCell   *lc;

		foreach(lc, nl->nestParams)
		{
			NestLoopParam *nlp = (NestLoopParam *) lfirst(lc);

			/*
			 * 因为我们不按参数化路径被使用的
			 * 外连接层去重新参数化它们，NestLoopParam 表达式里见到的 Var
			 * 其 nullingrels 也许只是外侧实际可用的那些 Var 的
			 * nullingrels 的子集。（LATERAL 引用也会造成这一点，如
			 * identify_current_nestloop_params 的注释所解释。）并不精确
			 * 检查这一点有点粗糙，
			 * 但要让
			 * 事情完全对上所需的工作，似乎与
			 * 其价值完全不成比例。
			 */
			nlp->paramval = (Var *) fix_upper_expr(root,
												   (Node *) nlp->paramval,
												   outer_itlist,
												   OUTER_VAR,
												   rtoffset,
												   NRM_SUBSET,
												   NUM_EXEC_TLIST(outer_plan));
			/* 检查是否已把任何 PlaceHolderVar 换成简单 Var */
			if (!(IsA(nlp->paramval, Var) &&
				  nlp->paramval->varno == OUTER_VAR))
				elog(ERROR, "NestLoopParam was not reduced to a simple Var");
		}
	}
	else if (IsA(join, MergeJoin))
	{
		MergeJoin  *mj = (MergeJoin *) join;

		mj->mergeclauses = fix_join_expr(root,
										 mj->mergeclauses,
										 outer_itlist,
										 inner_itlist,
										 (Index) 0,
										 rtoffset,
										 NRM_EQUAL,
										 NUM_EXEC_QUAL((Plan *) join));
	}
	else if (IsA(join, HashJoin))
	{
		HashJoin   *hj = (HashJoin *) join;

		hj->hashclauses = fix_join_expr(root,
										hj->hashclauses,
										outer_itlist,
										inner_itlist,
										(Index) 0,
										rtoffset,
										NRM_EQUAL,
										NUM_EXEC_QUAL((Plan *) join));

		/*
		 * HashJoin 的 hashkeys 用来在哈希表里查找其
		 * 外层计划（不是 Hash 节点！）中的匹配元组。
		 */
		hj->hashkeys = (List *) fix_upper_expr(root,
											   (Node *) hj->hashkeys,
											   outer_itlist,
											   OUTER_VAR,
											   rtoffset,
											   NRM_EQUAL,
											   NUM_EXEC_QUAL((Plan *) join));
	}

	/*
	 * 现在需要修正目标列表和 qpqual，它们在逻辑上
	 * 位于连接之上。这意味着，若不是内连接，这里出现的任何 Var
	 * 和 PHVs 的 nullingrels 应当包括该
	 * 外连接的效果，也就是等于输入
	 * Var 的 nullingrels 再加上外连接加上的那一位。我们
	 * 目前在这里没有足够信息来识别那应当
	 * 是什么，因此只告诉 fix_join_expr 接受 nullingrels 的超集
	 * 匹配，而不是精确匹配。
	 */
	join->plan.targetlist = fix_join_expr(root,
										  join->plan.targetlist,
										  outer_itlist,
										  inner_itlist,
										  (Index) 0,
										  rtoffset,
										  (join->jointype == JOIN_INNER ? NRM_EQUAL : NRM_SUPERSET),
										  NUM_EXEC_TLIST((Plan *) join));
	join->plan.qual = fix_join_expr(root,
									join->plan.qual,
									outer_itlist,
									inner_itlist,
									(Index) 0,
									rtoffset,
									(join->jointype == JOIN_INNER ? NRM_EQUAL : NRM_SUPERSET),
									NUM_EXEC_QUAL((Plan *) join));

	pfree(outer_itlist);
	pfree(inner_itlist);
}

/*
 * set_upper_references
 *	  更新上层计划节点的目标列表和限定条件，
 *	  使之引用其 lefttree 子计划返回的元组。
 *	  也对这些表达式做操作码查找，并
 *	  把 regclass OIDs 加进 root->glob->relationOids。
 *
 * 这用于 Agg、Group、Result 这类单输入计划类型。
 *
 * 多数情况下，必须把 tlist 和
 * 限定表达式里的各个 Var 与子计划 tlist 的元素匹配（子计划 tlist 是
 * 把这些同样的表达式展平后生成的，因此应当有全部
 * 所需变量）。不过有一个重要例外：
 * 取决于我们在计划树的什么位置，排序/分组列也许已被
 * 未展平地推进子计划 tlist。若输出也
 * 需要这些值，就应当引用子计划 tlist 元素，
 * 而不是重新计算该表达式。
 */
static void
set_upper_references(PlannerInfo *root, Plan *plan, int rtoffset)
{
	Plan	   *subplan = plan->lefttree;
	indexed_tlist *subplan_itlist;
	List	   *output_targetlist;
	ListCell   *l;

	subplan_itlist = build_tlist_index(subplan->targetlist);

	output_targetlist = NIL;
	foreach(l, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Node	   *newexpr;

		/* 若是排序/分组项，先试着按 sortref 匹配 */
		if (tle->ressortgroupref != 0)
		{
			newexpr = (Node *)
				search_indexed_tlist_for_sortgroupref(tle->expr,
													  tle->ressortgroupref,
													  subplan_itlist,
													  OUTER_VAR);
			if (!newexpr)
				newexpr = fix_upper_expr(root,
										 (Node *) tle->expr,
										 subplan_itlist,
										 OUTER_VAR,
										 rtoffset,
										 NRM_EQUAL,
										 NUM_EXEC_TLIST(plan));
		}
		else
			newexpr = fix_upper_expr(root,
									 (Node *) tle->expr,
									 subplan_itlist,
									 OUTER_VAR,
									 rtoffset,
									 NRM_EQUAL,
									 NUM_EXEC_TLIST(plan));
		tle = flatCopyTargetEntry(tle);
		tle->expr = (Expr *) newexpr;
		output_targetlist = lappend(output_targetlist, tle);
	}
	plan->targetlist = output_targetlist;

	plan->qual = (List *)
		fix_upper_expr(root,
					   (Node *) plan->qual,
					   subplan_itlist,
					   OUTER_VAR,
					   rtoffset,
					   NRM_EQUAL,
					   NUM_EXEC_QUAL(plan));

	pfree(subplan_itlist);
}

/*
 * set_param_references
 *	  初始化 Gather 或 Gather merge 节点的 initParam 列表，
 *	  使它包含该节点执行之前
 *	  需要求值的全部参数的引用。
 *	  它包含正被传给其下计划节点的 initplan 参数。
 */
static void
set_param_references(PlannerInfo *root, Plan *plan)
{
	Assert(IsA(plan, Gather) || IsA(plan, GatherMerge));

	if (plan->lefttree->extParam)
	{
		PlannerInfo *proot;
		Bitmapset  *initSetParam = NULL;
		ListCell   *l;

		for (proot = root; proot != NULL; proot = proot->parent_root)
		{
			foreach(l, proot->init_plans)
			{
				SubPlan    *initsubplan = (SubPlan *) lfirst(l);
				ListCell   *l2;

				foreach(l2, initsubplan->setParam)
				{
					initSetParam = bms_add_member(initSetParam, lfirst_int(l2));
				}
			}
		}

		/*
		 * 记住 Gather 或 Gather merge 节点的子节点所使用的
		 * 全部外部 initplan 参数列表。
		 */
		if (IsA(plan, Gather))
			((Gather *) plan)->initParam =
				bms_intersect(plan->lefttree->extParam, initSetParam);
		else
			((GatherMerge *) plan)->initParam =
				bms_intersect(plan->lefttree->extParam, initSetParam);
	}
}

/*
 * 递归扫描表达式树，把 Aggref 转换成合并聚合所用的适当
 * 中间形式。这意味着（1）把每一个的参数列表换成单个参数，
 * 该参数是改成表示部分聚合的原来的 Aggref，
 * 以及（2）把上层 Aggref 改成
 * 表示合并聚合。
 *
 * 这一步之后，set_upper_references 会把部分 Aggref
 * 换成引用下层 Agg 计划节点输出的 Var，因此执行器最终
 * 见到的形式是以 Var 为输入的合并 Aggref。
 *
 * 把这一步推迟到 setrefs.c 相当乱；理想情况下应在
 * createplan.c 里做。困难在于一旦修改 Aggref
 * 表达式，它们就不再与原来的形式 equal()，
 * 于是跨计划节点层次的匹配会失败。因此这必须发生在
 * Agg 之上的计划节点解析完其子计划引用之后。
 */
static Node *
convert_combining_aggrefs(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Aggref))
	{
		Aggref	   *orig_agg = (Aggref *) node;
		Aggref	   *child_agg;
		Aggref	   *parent_agg;

		/* 断言我们没有把任何不支持的情形部分化 */
		Assert(orig_agg->aggorder == NIL);
		Assert(orig_agg->aggdistinct == NIL);

		/*
		 * 由于聚合调用不能嵌套，不必递归进入
		 * 参数。但为安全起见，浅拷贝 Aggref 节点本身，而不是
		 * 就地修改它。
		 */
		child_agg = makeNode(Aggref);
		memcpy(child_agg, orig_agg, sizeof(Aggref));

		/*
		 * 对父 Aggref，想复制原聚合的全部字段，
		 * *除了* 下面要替换的 args 列表，以及只应由
		 * 子项而不是父项应用的 aggfilter 表达式。
		 * 与其在这里显式知道
		 * 所有其他字段，不如暂时改一下 child_agg，
		 * 让它成为 copyObject 的合适来源。
		 */
		child_agg->args = NIL;
		child_agg->aggfilter = NULL;
		parent_agg = copyObject(child_agg);
		child_agg->args = orig_agg->args;
		child_agg->aggfilter = orig_agg->aggfilter;

		/*
		 * 现在，把 child_agg 设成表示部分
		 * 聚合的第一阶段。目前假定需要序列化。
		 */
		mark_partial_aggref(child_agg, AGGSPLIT_INITIAL_SERIAL);

		/*
		 * 并把 parent_agg 设成表示第二阶段。
		 */
		parent_agg->args = list_make1(makeTargetEntry((Expr *) child_agg,
													  1, NULL, false));
		mark_partial_aggref(parent_agg, AGGSPLIT_FINAL_DESERIAL);

		return (Node *) parent_agg;
	}
	return expression_tree_mutator(node, convert_combining_aggrefs,
								   (void *) context);
}

/*
 * set_dummy_tlist_references
 *	  把上层计划节点的目标列表换成一份简单的、
 *	  对其子项的 OUTER_VAR 引用列表。
 *
 * 这用于 Sort 和 Append 这类不求值
 * 自己目标列表的计划类型。虽然执行器完全不在乎 tlist 里
 * 有什么，EXPLAIN 需要它看起来真实。
 *
 * 注意：这里几乎可以用 set_upper_references()，但它对
 * Append 会失败，因为没有 lefttree 子计划。专用代码反正
 * 更快。
 */
static void
set_dummy_tlist_references(Plan *plan, int rtoffset)
{
	List	   *output_targetlist;
	ListCell   *l;

	output_targetlist = NIL;
	foreach(l, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Var		   *oldvar = (Var *) tle->expr;
		Var		   *newvar;

		/*
		 * 与 search_indexed_tlist_for_non_var() 一样，我们宁可把 Const
		 * 保持为 Const，而不是引用 Const 的 Var。这里没有速度
		 * 上的好处，但能让 EXPLAIN 输出看起来更干净，并且
		 * 同样避免把执行器搞糊涂。
		 */
		if (IsA(oldvar, Const))
		{
			/* 直接复用已有的 TLE 节点 */
			output_targetlist = lappend(output_targetlist, tle);
			continue;
		}

		newvar = makeVar(OUTER_VAR,
						 tle->resno,
						 exprType((Node *) oldvar),
						 exprTypmod((Node *) oldvar),
						 exprCollation((Node *) oldvar),
						 0);
		if (IsA(oldvar, Var) &&
			oldvar->varnosyn > 0)
		{
			newvar->varnosyn = oldvar->varnosyn + rtoffset;
			newvar->varattnosyn = oldvar->varattnosyn;
		}
		else
		{
			newvar->varnosyn = 0;	/* 从来就不是普通 Var */
			newvar->varattnosyn = 0;
		}

		tle = flatCopyTargetEntry(tle);
		tle->expr = (Expr *) newvar;
		output_targetlist = lappend(output_targetlist, tle);
	}
	plan->targetlist = output_targetlist;

	/* 这里不碰 plan->qual */
}


/*
 * build_tlist_index --- 为子 tlist 建立索引数据结构
 *
 * 多数情况下，子计划 tlist 是只含 Var 的 "平坦" tlist，
 * 因此我们试着预先抽出关于 Var 的信息来优化那种情形。
 * 把父 tlist 匹配到子 tlist 仍然是 O(N^2)
 * 操作，但至少常数因子比普通的
 * tlist_member() 搜索小得多。
 *
 * 本函数的结果是一份 indexed_tlist 结构，传给
 * search_indexed_tlist_for_var() 及其同类。
 * 用完后，indexed_tlist 可以用一次 pfree() 释放。
 */
static indexed_tlist *
build_tlist_index(List *tlist)
{
	indexed_tlist *itlist;
	tlist_vinfo *vinfo;
	ListCell   *l;

	/* 建立有足够槽位容纳全部 tlist 项的数据结构 */
	itlist = (indexed_tlist *)
		palloc(offsetof(indexed_tlist, vars) +
			   list_length(tlist) * sizeof(tlist_vinfo));

	itlist->tlist = tlist;
	itlist->has_ph_vars = false;
	itlist->has_non_vars = false;

	/* 找出 Var 并填入索引数组 */
	vinfo = itlist->vars;
	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->expr && IsA(tle->expr, Var))
		{
			Var		   *var = (Var *) tle->expr;

			vinfo->varno = var->varno;
			vinfo->varattno = var->varattno;
			vinfo->resno = tle->resno;
			vinfo->varnullingrels = var->varnullingrels;
			vinfo++;
		}
		else if (tle->expr && IsA(tle->expr, PlaceHolderVar))
			itlist->has_ph_vars = true;
		else
			itlist->has_non_vars = true;
	}

	itlist->num_vars = (vinfo - itlist->vars);

	return itlist;
}

/*
 * build_tlist_index_other_vars --- 建立受限的 tlist 索引
 *
 * 这像 build_tlist_index，但只索引那些
 * 属于指定关系以外某个关系的 Var 的 tlist 项。我们会设置
 * has_ph_vars（允许匹配 PlaceHolderVars），但不设置 has_non_vars
 * （因此除了 Var 和 PlaceHolderVars，什么都不能匹配）。
 */
static indexed_tlist *
build_tlist_index_other_vars(List *tlist, int ignore_rel)
{
	indexed_tlist *itlist;
	tlist_vinfo *vinfo;
	ListCell   *l;

	/* 建立有足够槽位容纳全部 tlist 项的数据结构 */
	itlist = (indexed_tlist *)
		palloc(offsetof(indexed_tlist, vars) +
			   list_length(tlist) * sizeof(tlist_vinfo));

	itlist->tlist = tlist;
	itlist->has_ph_vars = false;
	itlist->has_non_vars = false;

	/* 找出所需的 Var 并填入索引数组 */
	vinfo = itlist->vars;
	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->expr && IsA(tle->expr, Var))
		{
			Var		   *var = (Var *) tle->expr;

			if (var->varno != ignore_rel)
			{
				vinfo->varno = var->varno;
				vinfo->varattno = var->varattno;
				vinfo->resno = tle->resno;
				vinfo->varnullingrels = var->varnullingrels;
				vinfo++;
			}
		}
		else if (tle->expr && IsA(tle->expr, PlaceHolderVar))
			itlist->has_ph_vars = true;
	}

	itlist->num_vars = (vinfo - itlist->vars);

	return itlist;
}

/*
 * search_indexed_tlist_for_var --- 在带索引的 tlist 里找一个 Var
 *
 * 若找到匹配，就返回给定 Var 的一份副本，并适当
 * 修改 varno/varattno（即 newvarno 和该 TLE 项的 resno）。
 * 也确保 varnosyn 加上 rtoffset。
 * 若没有匹配，返回 NULL。
 *
 * 在调试构建里，我们按 nrm_match 交叉检查子计划
 * 输出 Var 的 varnullingrels。多数调用点应当传入 NRM_EQUAL，
 * 表示我们预期精确匹配。不过有些地方
 * 还没完全收拾干净，只好接受
 * 子集或超集匹配。
 */
static Var *
search_indexed_tlist_for_var(Var *var, indexed_tlist *itlist,
							 int newvarno, int rtoffset,
							 NullingRelsMatch nrm_match)
{
	int			varno = var->varno;
	AttrNumber	varattno = var->varattno;
	tlist_vinfo *vinfo;
	int			i;

	vinfo = itlist->vars;
	i = itlist->num_vars;
	while (i-- > 0)
	{
		if (vinfo->varno == varno && vinfo->varattno == varattno)
		{
			/* 找到了匹配 */
			Var		   *newvar = copyVar(var);

			/*
			 * 核实我们把 nullingrels 的那些花样都弄对了。
			 *
			 * XXX 对系统列和整行 Var 跳过这项检查。
			 * 那是因为这类 Var 也许是行标识 Var，它们生成时
			 * 没有任何 varnullingrels。很难不这样做，
			 * 因为它们通常在规划很早的时候就造好了，
			 * 那时还没看过连接树，不知道哪些
			 * 连接也许会把这类 Var 置空。似乎不值得花那个代价
			 * 让它们完全有效。虽然因此失去对用户写出的、对这类
			 * 列的引用的检查有点烦人，
			 * 但 nullingrels 逻辑里的 bug 似乎不太会
			 * 只影响系统列。
			 */
			if (!(varattno <= 0 ||
				  (nrm_match == NRM_SUBSET ?
				   bms_is_subset(var->varnullingrels, vinfo->varnullingrels) :
				   nrm_match == NRM_SUPERSET ?
				   bms_is_subset(vinfo->varnullingrels, var->varnullingrels) :
				   bms_equal(vinfo->varnullingrels, var->varnullingrels))))
				elog(ERROR, "wrong varnullingrels %s (expected %s) for Var %d/%d",
					 bmsToString(var->varnullingrels),
					 bmsToString(vinfo->varnullingrels),
					 varno, varattno);

			newvar->varno = newvarno;
			newvar->varattno = vinfo->resno;
			if (newvar->varnosyn > 0)
				newvar->varnosyn += rtoffset;
			return newvar;
		}
		vinfo++;
	}
	return NULL;				/* 没有匹配 */
}

/*
 * search_indexed_tlist_for_phv --- 在带索引的 tlist 里找一个 PlaceHolderVar
 *
 * 若找到匹配，就返回构造好的、引用该 tlist 项的 Var。
 * 若没有匹配，返回 NULL。
 *
 * 像 search_indexed_tlist_for_var 那样交叉检查 phnullingrels。
 *
 * 注意：除非 itlist->has_ph_vars，否则调用这个是浪费时间。
 */
static Var *
search_indexed_tlist_for_phv(PlaceHolderVar *phv,
							 indexed_tlist *itlist, int newvarno,
							 NullingRelsMatch nrm_match)
{
	ListCell   *lc;

	foreach(lc, itlist->tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tle->expr && IsA(tle->expr, PlaceHolderVar))
		{
			PlaceHolderVar *subphv = (PlaceHolderVar *) tle->expr;
			Var		   *newvar;

			/*
			 * 与 search_indexed_tlist_for_var 类似，我们只按 phid
			 * 匹配。不用 equal()，部分是为了速度，但主要是
			 * 因为 phnullingrels 也许并不精确相等。
			 */
			if (phv->phid != subphv->phid)
				continue;

			/* 核实我们把 nullingrels 的那些花样都弄对了 */
			if (!(nrm_match == NRM_SUBSET ?
				  bms_is_subset(phv->phnullingrels, subphv->phnullingrels) :
				  nrm_match == NRM_SUPERSET ?
				  bms_is_subset(subphv->phnullingrels, phv->phnullingrels) :
				  bms_equal(subphv->phnullingrels, phv->phnullingrels)))
				elog(ERROR, "wrong phnullingrels %s (expected %s) for PlaceHolderVar %d",
					 bmsToString(phv->phnullingrels),
					 bmsToString(subphv->phnullingrels),
					 phv->phid);

			/* 找到了匹配的子计划输出表达式 */
			newvar = makeVarFromTargetEntry(newvarno, tle);
			newvar->varnosyn = 0;	/* 从来就不是普通 Var */
			newvar->varattnosyn = 0;
			return newvar;
		}
	}
	return NULL;				/* 没有匹配 */
}

/*
 * search_indexed_tlist_for_non_var --- 在带索引的 tlist 里找非 Var/PHV
 *
 * 若找到匹配，就返回构造好的、引用该 tlist 项的 Var。
 * 若没有匹配，返回 NULL。
 *
 * 注意：除非 itlist->has_non_vars，否则调用这个是浪费时间。
 */
static Var *
search_indexed_tlist_for_non_var(Expr *node,
								 indexed_tlist *itlist, int newvarno)
{
	TargetEntry *tle;

	/*
	 * 若它是简单 Const，即使用下层恰好有一个相同的 Const，
	 * 把它换成 Var 也很傻；执行 Var 比执行
	 * Const 更贵。而且，替换它会把执行器里某些
	 * 预期见到简单 Const 的地方搞糊涂，例如
	 * 被删除的列。
	 */
	if (IsA(node, Const))
		return NULL;

	tle = tlist_member(node, itlist->tlist);
	if (tle)
	{
		/* 找到了匹配的子计划输出表达式 */
		Var		   *newvar;

		newvar = makeVarFromTargetEntry(newvarno, tle);
		newvar->varnosyn = 0;	/* 从来就不是普通 Var */
		newvar->varattnosyn = 0;
		return newvar;
	}
	return NULL;				/* 没有匹配 */
}

/*
 * search_indexed_tlist_for_sortgroupref ---
 *
 * 在带索引的 tlist 里找一个排序/分组表达式
 * 若找到匹配，就返回构造好的、引用该 tlist 项的 Var。
 *
 * 若没有匹配，返回 NULL。
 * 当存在多个文本上 equal() 但易变的排序表达式时，需要这个来确保选出正确的子计划 TLE。
 * 而且它也比 search_indexed_tlist_for_non_var 更快。
 */
static Var *
search_indexed_tlist_for_sortgroupref(Expr *node,
									  Index sortgroupref,
									  indexed_tlist *itlist,
									  int newvarno)
{
	ListCell   *lc;

	foreach(lc, itlist->tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		/* equal() 检查应当是多余的，但还是多留个心 */
		if (tle->ressortgroupref == sortgroupref &&
			equal(node, tle->expr))
		{
			/* 找到了匹配的子计划输出表达式 */
			Var		   *newvar;

			newvar = makeVarFromTargetEntry(newvarno, tle);
			newvar->varnosyn = 0;	/* 从来就不是普通 Var */
			newvar->varattnosyn = 0;
			return newvar;
		}
	}
	return NULL;				/* 没有匹配 */
}

/*
 * fix_join_expr
 *	   通过改变子句里变量的 varno/varattno 值，
 *	   使之引用外侧和内侧连接
 *	   关系目标列表里的目标列表值，建立一套新的目标列表项或连接限定子句。
 *	   也做操作码查找，并把
 *	   regclass OIDs 加进 root->glob->relationOids。
 *
 * 这用在四种不同情形：
 * 1) 普通连接子句，其中子句里的全部 Var *必须*
 *	  被换成 OUTER_VAR 或 INNER_VAR 引用。这种情况下
 *	  acceptable_rel 应当为零，这样任何 Var 匹配失败都会
 *	  被报告为错误。
 * 2) RETURNING 子句，其中可能既含目标关系的 Var，
 *	  也含其他关系的 Var。这种情况下想把
 *	  其他关系的 Var 换成 OUTER_VAR 引用，而目标 Var
 *	  保持不动。因此应传入 inner_itlist = NULL，acceptable_rel = 目标
 *	  关系的 ID。
 * 3) ON CONFLICT UPDATE 的 SET/WHERE 子句。这里对 EXCLUDED 的引用
 *	  要换成 INNER_VAR 引用，而目标 Var（
 *	  待更新的关系）保持不动。相应地 inner_itlist 应当是
 *	  EXCLUDED 元素，outer_itlist = NULL，acceptable_rel 为该目标
 *	  关系。
 * 4) MERGE。这种情况下，对源关系的引用要
 *    换成 INNER_VAR 引用，目标
 *    关系（待修改的关系）的 Var 保持不动。因此 inner_itlist 应当是
 *    源关系元素，outer_itlist = NULL，acceptable_rel
 *    为该目标关系。
 *
 * 'clauses' 是目标列表或连接子句列表
 * 'outer_itlist' 是外侧连接关系的带索引目标列表，
 *		或 NULL
 * 'inner_itlist' 是内侧连接关系的带索引目标列表，
 *		或 NULL
 * 'acceptable_rel' 要么为零，要么是某个关系的范围表下标，
 *		该关系的 Var 可以出现在子句里而不引发错误
 * 'rtoffset'：varno 要增加多少
 * 'nrm_match'：同 search_indexed_tlist_for_var()
 * 'num_exec'：表达式的估计执行次数
 *
 * 返回新的表达式树。原来的子句结构
 * 不被修改。
 */
static List *
fix_join_expr(PlannerInfo *root,
			  List *clauses,
			  indexed_tlist *outer_itlist,
			  indexed_tlist *inner_itlist,
			  Index acceptable_rel,
			  int rtoffset,
			  NullingRelsMatch nrm_match,
			  double num_exec)
{
	fix_join_expr_context context;

	context.root = root;
	context.outer_itlist = outer_itlist;
	context.inner_itlist = inner_itlist;
	context.acceptable_rel = acceptable_rel;
	context.rtoffset = rtoffset;
	context.nrm_match = nrm_match;
	context.num_exec = num_exec;
	return (List *) fix_join_expr_mutator((Node *) clauses, &context);
}

static Node *
fix_join_expr_mutator(Node *node, fix_join_expr_context *context)
{
	Var		   *newvar;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/* 在输入 tlist 里找这个 var，先在外侧 */
		if (context->outer_itlist)
		{
			newvar = search_indexed_tlist_for_var(var,
												  context->outer_itlist,
												  OUTER_VAR,
												  context->rtoffset,
												  context->nrm_match);
			if (newvar)
				return (Node *) newvar;
		}

		/* 然后在内侧。 */
		if (context->inner_itlist)
		{
			newvar = search_indexed_tlist_for_var(var,
												  context->inner_itlist,
												  INNER_VAR,
												  context->rtoffset,
												  context->nrm_match);
			if (newvar)
				return (Node *) newvar;
		}

		/* 若它属于 acceptable_rel，就调整并返回它 */
		if (var->varno == context->acceptable_rel)
		{
			var = copyVar(var);
			var->varno += context->rtoffset;
			if (var->varnosyn > 0)
				var->varnosyn += context->rtoffset;
			return (Node *) var;
		}

		/* 没找到 Var 的所指对象 */
		elog(ERROR, "variable not found in subplan target lists");
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* 看该 PlaceHolderVar 是否从更低的计划节点冒上来 */
		if (context->outer_itlist && context->outer_itlist->has_ph_vars)
		{
			newvar = search_indexed_tlist_for_phv(phv,
												  context->outer_itlist,
												  OUTER_VAR,
												  context->nrm_match);
			if (newvar)
				return (Node *) newvar;
		}
		if (context->inner_itlist && context->inner_itlist->has_ph_vars)
		{
			newvar = search_indexed_tlist_for_phv(phv,
												  context->inner_itlist,
												  INNER_VAR,
												  context->nrm_match);
			if (newvar)
				return (Node *) newvar;
		}

		/* 若不是由输入计划提供，就求值所含的表达式 */
		/* XXX 能否对 phnullingrels 断言点什么？ */
		return fix_join_expr_mutator((Node *) phv->phexpr, context);
	}
	/* 若 tlist 里有，也试着匹配更复杂的表达式 */
	if (context->outer_itlist && context->outer_itlist->has_non_vars)
	{
		newvar = search_indexed_tlist_for_non_var((Expr *) node,
												  context->outer_itlist,
												  OUTER_VAR);
		if (newvar)
			return (Node *) newvar;
	}
	if (context->inner_itlist && context->inner_itlist->has_non_vars)
	{
		newvar = search_indexed_tlist_for_non_var((Expr *) node,
												  context->inner_itlist,
												  INNER_VAR);
		if (newvar)
			return (Node *) newvar;
	}
	/* 特殊情形（只在匹配下层 tlist 失败 AFTER 才应用） */
	if (IsA(node, Param))
		return fix_param_node(context->root, (Param *) node);
	if (IsA(node, AlternativeSubPlan))
		return fix_join_expr_mutator(fix_alternative_subplan(context->root,
															 (AlternativeSubPlan *) node,
															 context->num_exec),
									 context);
	fix_expr_common(context->root, node);
	return expression_tree_mutator(node,
								   fix_join_expr_mutator,
								   (void *) context);
}

/*
 * fix_upper_expr
 *		修改表达式树，使全部 Var 节点引用某个子计划的
 *		输出。也寻找应当被
 *		initplan 输出 Param 替换的 Aggref 节点。也做操作码查找，并把
 *		regclass OIDs 加进 root->glob->relationOids。
 *
 * 这用来修正非连接的上层
 * 计划节点以及仅索引扫描节点的目标和限定表达式。
 *
 * 若在子计划 tlist 里找不到匹配的 var，就报错
 * ——因此本例程只应当用于这样的节点：其子计划的
 * 目标列表是把父节点所用表达式展平后生成的。
 * 若 itlist->has_non_vars 为真，就试着把整个子表达式
 *
 * 与子计划 tlist 的元素匹配，以便避免重新计算
 * 子计划已经算过的表达式。（这相对
 * 较贵，因此在子计划
 * tlist 只是一份展平的 Var 列表这种常见情形下不想尝试。）
 * 'node'：要修正的树（一个目标项或限定条件）
 *
 * 'subplan_itlist'：子计划（或索引）的带索引目标列表
 * 'newvarno'：引用 tlist 元素的 Var 所用的 varno
 * 'rtoffset'：varno 要增加多少
 * 'nrm_match'：同 search_indexed_tlist_for_var()
 * 'num_exec'：表达式的估计执行次数
 * 得到的树是原树的副本，其中全部 Var 节点的
 *
 * varno = newvarno，varattno = 对应目标列表元素的 resno。
 * 原来的那棵树
 * 不被修改。
 */
static Node *
fix_upper_expr(PlannerInfo *root,
			   Node *node,
			   indexed_tlist *subplan_itlist,
			   int newvarno,
			   int rtoffset,
			   NullingRelsMatch nrm_match,
			   double num_exec)
{
	fix_upper_expr_context context;

	context.root = root;
	context.subplan_itlist = subplan_itlist;
	context.newvarno = newvarno;
	context.rtoffset = rtoffset;
	context.nrm_match = nrm_match;
	context.num_exec = num_exec;
	return fix_upper_expr_mutator(node, &context);
}

static Node *
fix_upper_expr_mutator(Node *node, fix_upper_expr_context *context)
{
	Var		   *newvar;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		newvar = search_indexed_tlist_for_var(var,
											  context->subplan_itlist,
											  context->newvarno,
											  context->rtoffset,
											  context->nrm_match);
		if (!newvar)
			elog(ERROR, "variable not found in subplan target list");
		return (Node *) newvar;
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* 看该 PlaceHolderVar 是否从更低的计划节点冒上来 */
		if (context->subplan_itlist->has_ph_vars)
		{
			newvar = search_indexed_tlist_for_phv(phv,
												  context->subplan_itlist,
												  context->newvarno,
												  context->nrm_match);
			if (newvar)
				return (Node *) newvar;
		}
		/* 若不是由输入计划提供，就求值所含的表达式 */
		/* XXX 能否对 phnullingrels 断言点什么？ */
		return fix_upper_expr_mutator((Node *) phv->phexpr, context);
	}
	/* 若 tlist 里有，也试着匹配更复杂的表达式 */
	if (context->subplan_itlist->has_non_vars)
	{
		newvar = search_indexed_tlist_for_non_var((Expr *) node,
												  context->subplan_itlist,
												  context->newvarno);
		if (newvar)
			return (Node *) newvar;
	}
	/* 特殊情形（只在匹配下层 tlist 失败 AFTER 才应用） */
	if (IsA(node, Param))
		return fix_param_node(context->root, (Param *) node);
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		Param	   *aggparam;

		/* 看该 Aggref 是否应当被 Param 替换 */
		aggparam = find_minmax_agg_replacement_param(context->root, aggref);
		if (aggparam != NULL)
		{
			/* 为多留个心，复制一份 Param */
			return (Node *) copyObject(aggparam);
		}
		/* If no match, just fall through to process it normally */
	}
	if (IsA(node, AlternativeSubPlan))
		return fix_upper_expr_mutator(fix_alternative_subplan(context->root,
															  (AlternativeSubPlan *) node,
															  context->num_exec),
									  context);
	fix_expr_common(context->root, node);
	return expression_tree_mutator(node,
								   fix_upper_expr_mutator,
								   (void *) context);
}

/*
 * set_returning_clause_references
 *		对 RETURNING 目标列表做 setrefs.c 的工作
 *
 * 若查询涉及的不只是结果表，就必须
 * 调整任何引用其他表的 Var，使之引用顶层子计划目标列表里的
 * junk tlist 项。不过引用结果
 * 表的 Var 应当保持不动（执行器会在触发触发器之后，若有的话，
 * 用实际的堆元组求值它们）。在
 * 调整后的 RETURNING 列表里，结果表 Var 会保持原来的
 * varno（加上 rtoffset），但其他关系的 Var 的 varno 将是 OUTER_VAR。
 *
 * 也必须做操作码查找，并把 regclass OIDs 加进
 * root->glob->relationOids。
 *
 * 'rlist'：要修正的 RETURNING 目标列表
 * 'topplan'：将位于 ModifyTable
 *		节点正下方的顶层子计划节点（注意它还没经过 set_plan_refs）
 * 'resultRelation'：相关联的结果关系的 RT 下标
 * 'rtoffset'：varno 要增加多少
 *
 * 注意：给定的 'root' 属于父查询层次，不属于 'topplan'。
 * 目前这没关系，因为我们只访问 root->glob 里的依赖项
 * 列表，但若想要一个真正匹配该子计划的 root，
 * 就得做些改动。
 *
 * 注意：resultRelation 还没按 rtoffset 调整。
 */
static List *
set_returning_clause_references(PlannerInfo *root,
								List *rlist,
								Plan *topplan,
								Index resultRelation,
								int rtoffset)
{
	indexed_tlist *itlist;

	/*
	 * 可以通过挪用以前处理内侧索引扫描修正的 fix_join_expr
	 * 机制，来做想要的 Var 修正。我们在
	 * 顶层计划的目标列表里搜索非结果关系的 Var，并用
	 * fix_join_expr 把 RETURNING 的 Var 转换成对这些 tlist
	 * 项的引用，而结果关系的 Var 保持原样。
	 *
	 * 也会在目标列表里找 PlaceHolderVars，但不会找
	 * 更复杂的表达式。注意 PlaceHolderVar 不可能
	 * 引用结果关系，因为结果
	 * 从不会位于外连接之下。若那种情况可能发生，就得准备好
	 * 拆开该 PlaceHolderVar，改为求值它所含的
	 * 表达式。
	 */
	itlist = build_tlist_index_other_vars(topplan->targetlist, resultRelation);

	rlist = fix_join_expr(root,
						  rlist,
						  itlist,
						  NULL,
						  resultRelation,
						  rtoffset,
						  NRM_EQUAL,
						  NUM_EXEC_TLIST(topplan));

	pfree(itlist);

	return rlist;
}

/*
 * fix_windowagg_condition_expr_mutator
 *		把 WindowFuncs 替换成目标列表里引用该 WindowFunc 的对应 Var
 *		的变更函数。
 */
static Node *
fix_windowagg_condition_expr_mutator(Node *node,
									 fix_windowagg_cond_context *context)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, WindowFunc))
	{
		Var		   *newvar;

		newvar = search_indexed_tlist_for_non_var((Expr *) node,
												  context->subplan_itlist,
												  context->newvarno);
		if (newvar)
			return (Node *) newvar;
		elog(ERROR, "WindowFunc not found in subplan target lists");
	}

	return expression_tree_mutator(node,
								   fix_windowagg_condition_expr_mutator,
								   (void *) context);
}

/*
 * fix_windowagg_condition_expr
 *		转换 'runcondition' 里的引用，使任何 WindowFunc
 *		引用都被换成一个 Var，该 Var 引用 'subplan_itlist' 里匹配的
 *		WindowFunc。
 */
static List *
fix_windowagg_condition_expr(PlannerInfo *root,
							 List *runcondition,
							 indexed_tlist *subplan_itlist)
{
	fix_windowagg_cond_context context;

	context.root = root;
	context.subplan_itlist = subplan_itlist;
	context.newvarno = 0;

	return (List *) fix_windowagg_condition_expr_mutator((Node *) runcondition,
														 &context);
}

/*
 * set_windowagg_runcondition_references
 *		转换 'runcondition' 里的引用，使任何 WindowFunc
 *		引用都被换成一个 Var，该 Var 引用 'plan' 目标列表里匹配的
 *		WindowFunc。
 */
static List *
set_windowagg_runcondition_references(PlannerInfo *root,
									  List *runcondition,
									  Plan *plan)
{
	List	   *newlist;
	indexed_tlist *itlist;

	itlist = build_tlist_index(plan->targetlist);

	newlist = fix_windowagg_condition_expr(root, runcondition, itlist);

	pfree(itlist);

	return newlist;
}

/*
 * find_minmax_agg_replacement_param
 *		若给定的 Aggref 是我们正在优化成子查询的那一个
 *		（参见 planagg.c），就返回应当替换它的 Param。
 *		否则返回 NULL。
 *
 * 把它导出，是为了 SS_finalize_plan 能在 setrefs.c 运行之前使用。
 * 注意，在我们从 MinMaxAggPath 建成 Plan 之前，它什么也找不到，
 * 因为否则 root->minmax_aggs 永远不会被填上。
 */
Param *
find_minmax_agg_replacement_param(PlannerInfo *root, Aggref *aggref)
{
	if (root->minmax_aggs != NIL &&
		list_length(aggref->args) == 1)
	{
		TargetEntry *curTarget = (TargetEntry *) linitial(aggref->args);
		ListCell   *lc;

		foreach(lc, root->minmax_aggs)
		{
			MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);

			if (mminfo->aggfnoid == aggref->aggfnoid &&
				equal(mminfo->target, curTarget->expr))
				return mminfo->param;
		}
	}
	return NULL;
}


/*****************************************************************************
 *					QUERY DEPENDENCY MANAGEMENT（查询依赖管理）
 *****************************************************************************/

/*
 * record_plan_function_dependency
 *		把当前计划标成依赖某个特定函数。
 *
 * 把它导出，是为了函数内联代码能记录对
 * 它从计划树里去掉的某个函数的依赖。
 */
void
record_plan_function_dependency(PlannerInfo *root, Oid funcid)
{
	/*
	 * 出于性能原因，我们不费心跟踪内建函数；
	 * 只假定它们永远不会变（至少不会以会使使用它们的计划
	 * 失效的方式变）。为此可以把
	 * OID 小于 FirstUnpinnedObjectId 的函数看作内建函数。
	 * 注意 OID 生成器保证在启动之后永不生成这样的 OID，
	 * 即使在 OID 回绕时也是如此。
	 */
	if (funcid >= (Oid) FirstUnpinnedObjectId)
	{
		PlanInvalItem *inval_item = makeNode(PlanInvalItem);

		/*
		 * 用 pg_proc 上的任何系统缓存都能工作，但最容易的是
		 * PROCOID，因为手头已经有该函数的 OID。注意
		 * plancache.c 知道我们用的是 PROCOID。
		 */
		inval_item->cacheId = PROCOID;
		inval_item->hashValue = GetSysCacheHashValue1(PROCOID,
													  ObjectIdGetDatum(funcid));

		root->glob->invalItems = lappend(root->glob->invalItems, inval_item);
	}
}

/*
 * record_plan_type_dependency
 *		把当前计划标成依赖某个特定类型。
 *
 * 把它导出，是为了 eval_const_expressions 能记录对
 * 它已去掉 CoerceToDomain 节点的某个域的依赖。
 *
 * 目前不需要记录计划里仍含有 CoerceToDomain 节点的那些域的依赖，
 * 不过将来也许会变。
 * 因此本模块实际上并不调用它，不过
 * 有一天 fix_expr_common 也许会调用它。
 */
void
record_plan_type_dependency(PlannerInfo *root, Oid typid)
{
	/*
	 * 与 record_plan_function_dependency 一样，忽略有人会
	 * 修改内建域的可能性。
	 */
	if (typid >= (Oid) FirstUnpinnedObjectId)
	{
		PlanInvalItem *inval_item = makeNode(PlanInvalItem);

		/*
		 * 用 pg_type 上的任何系统缓存都能工作，但最容易的是
		 * TYPEOID，因为手头已经有该类型的 OID。注意
		 * plancache.c 知道我们用的是 TYPEOID。
		 */
		inval_item->cacheId = TYPEOID;
		inval_item->hashValue = GetSysCacheHashValue1(TYPEOID,
													  ObjectIdGetDatum(typid));

		root->glob->invalItems = lappend(root->glob->invalItems, inval_item);
	}
}

/*
 * extract_query_dependencies
 *		给定一份已改写但尚未规划的查询或一组查询
 *		（即一个 Query 节点或 Query 节点列表），像
 *		set_plan_references 那样抽出依赖。也检测是否有任何
 *		改写步骤受到了 RLS 影响。
 *
 * plancache.c 需要这个来处理未规划的缓存
 * 查询的失效。
 *
 * 注意：这不经过 eval_const_expressions，因此不会
 * 反映它往 invalItems 列表里加入的内联函数和被省掉的 CoerceToDomain 节点。
 * 对函数这显然没问题，因为反正会在
 * 原来的查询树里见到它们。对域也没问题，因为
 * 除非域被省掉，否则我们不关心。也就是说，计划也许
 * 有查询树所没有的域依赖。
 */
void
extract_query_dependencies(Node *query,
						   List **relationOids,
						   List **invalItems,
						   bool *hasRowSecurity)
{
	PlannerGlobal glob;
	PlannerInfo root;

	/* 编造一份哑的规划器状态，以便使用本模块的机制 */
	MemSet(&glob, 0, sizeof(glob));
	glob.type = T_PlannerGlobal;
	glob.relationOids = NIL;
	glob.invalItems = NIL;
	/* 取巧：用 glob.dependsOnRole 收集 hasRowSecurity 标志 */
	glob.dependsOnRole = false;

	MemSet(&root, 0, sizeof(root));
	root.type = T_PlannerInfo;
	root.glob = &glob;

	(void) extract_query_dependencies_walker(query, &root);

	*relationOids = glob.relationOids;
	*invalItems = glob.invalItems;
	*hasRowSecurity = glob.dependsOnRole;
}

/*
 * extract_query_dependencies 的树遍历器。
 *
 * 把它导出，是为了 expression_planner_with_deps 能对
 * 简单表达式调用它（那种情况下是规划之后，不是规划之前）。
 * 在那种用法里，glob.dependsOnRole 没有意义，但 relationOids
 * 和 invalItems 列表会按需要追加。
 */
bool
extract_query_dependencies_walker(Node *node, PlannerInfo *context)
{
	if (node == NULL)
		return false;
	Assert(!IsA(node, PlaceHolderVar));
	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *lc;

		if (query->commandType == CMD_UTILITY)
		{
			/*
			 * 这段逻辑必须处理任何语法
			 * 分析并非平凡的实用命令（参见 stmt_requires_parse_analysis）。
			 *
			 * 尤其是，CALL 需要它自己的处理。
			 */
			if (IsA(query->utilityStmt, CallStmt))
			{
				CallStmt   *callstmt = (CallStmt *) query->utilityStmt;

				/* 不必检查 funccall，只看变换后的表达式 */
				(void) extract_query_dependencies_walker((Node *) callstmt->funcexpr,
														 context);
				(void) extract_query_dependencies_walker((Node *) callstmt->outargs,
														 context);
				return false;
			}

			/*
			 * 忽略其他实用语句，除了那些（例如 EXPLAIN）
			 * 含有已解析但未规划的查询的语句。对那些，我们
			 * 只需把注意力转到所含的查询上。
			 */
			query = UtilityContainsQuery(query->utilityStmt);
			if (query == NULL)
				return false;
		}

		/* 记住是否有任何 Query 被改写器应用了 RLS 限定条件 */
		if (query->hasRowSecurity)
			context->glob->dependsOnRole = true;

		/* 收集本 Query 的 rtable 里的关系 OIDs */
		foreach(lc, query->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

			if (rte->rtekind == RTE_RELATION ||
				(rte->rtekind == RTE_SUBQUERY && OidIsValid(rte->relid)) ||
				(rte->rtekind == RTE_NAMEDTUPLESTORE && OidIsValid(rte->relid)))
				context->glob->relationOids =
					lappend_oid(context->glob->relationOids, rte->relid);
		}

		/* 并递归进入该查询的子表达式 */
		return query_tree_walker(query, extract_query_dependencies_walker,
								 (void *) context, 0);
	}
	/* 抽出函数依赖，并检查 regclass Const */
	fix_expr_common(context, node);
	return expression_tree_walker(node, extract_query_dependencies_walker,
								  (void *) context);
}
