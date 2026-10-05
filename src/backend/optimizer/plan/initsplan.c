/*-------------------------------------------------------------------------
 *
 * initsplan.c
 *	  目标列表、限定条件与 joininfo 的初始化例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/initsplan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/inherit.h"
#include "optimizer/joininfo.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "parser/analyze.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

/* 这些参数由 GUC 设置 */
int			from_collapse_limit;
int			join_collapse_limit;


/*
 * deconstruct_jointree 需要多趟扫描连接树，因为必须
 * 先算完 JoinDomains，才能开始分发限定条件。
 * 既然必须这样做，其他信息例如相关的
 * 限定范围也可以在第一趟一并算好。
 *
 * deconstruct_recurse 递归检查连接树，并按深度优先
 * 遍历顺序建立一份 JoinTreeItem 结构体的 List，随后由
 * deconstruct_distribute 逐项处理。若存在外
 * 连接，非退化的外连接子句在第三趟
 * deconstruct_distribute_oj_quals 里处理。
 *
 * JoinTreeItem 结构体本身可在
 * deconstruct_jointree 结束时释放，但不要修改或释放它们的子结构，
 * 因为那些 relid 集合也可能被 RestrictInfo 和
 * SpecialJoinInfo 节点指向。
 */
typedef struct JoinTreeItem
{
	/* deconstruct_recurse 期间填入的字段： */
	Node	   *jtnode;			/* 要检查的连接树节点 */
	JoinDomain *jdomain;		/* 其 ON/WHERE 子句所属的连接域 */
	struct JoinTreeItem *jti_parent;	/* 本节点父项的 JoinTreeItem，
										 * 若为顶层则为 NULL */
	Relids		qualscope;		/* 语法上包含在本连接树节点里的
								 * base+OJ Relids */
	Relids		inner_join_rels;	/* 语法上包含在本连接树节点处或其下
									 * 出现的内连接中的
									 * base+OJ Relids */
	Relids		left_rels;		/* 若为连接节点，左侧的 Relids */
	Relids		right_rels;		/* 若为连接节点，右侧的 Relids */
	Relids		nonnullable_rels;	/* 若为外连接，不可空侧的
									 * Relids */
	/* deconstruct_distribute 期间填入的字段： */
	SpecialJoinInfo *sjinfo;	/* 若为外连接，其 SpecialJoinInfo */
	List	   *oj_joinclauses; /* 尚未分发的外连接限定条件 */
	List	   *lateral_clauses;	/* 因子节点的 LATERAL 引用而推迟到本层的
									 * 限定条件 */
} JoinTreeItem;


static void extract_lateral_references(PlannerInfo *root, RelOptInfo *brel,
									   Index rtindex);
static List *deconstruct_recurse(PlannerInfo *root, Node *jtnode,
								 JoinDomain *parent_domain,
								 JoinTreeItem *parent_jtitem,
								 List **item_list);
static void deconstruct_distribute(PlannerInfo *root, JoinTreeItem *jtitem);
static void process_security_barrier_quals(PlannerInfo *root,
										   int rti, JoinTreeItem *jtitem);
static void mark_rels_nulled_by_join(PlannerInfo *root, Index ojrelid,
									 Relids lower_rels);
static SpecialJoinInfo *make_outerjoininfo(PlannerInfo *root,
										   Relids left_rels, Relids right_rels,
										   Relids inner_join_rels,
										   JoinType jointype, Index ojrelid,
										   List *clause);
static void compute_semijoin_info(PlannerInfo *root, SpecialJoinInfo *sjinfo,
								  List *clause);
static void deconstruct_distribute_oj_quals(PlannerInfo *root,
											List *jtitems,
											JoinTreeItem *jtitem);
static void distribute_quals_to_rels(PlannerInfo *root, List *clauses,
									 JoinTreeItem *jtitem,
									 SpecialJoinInfo *sjinfo,
									 Index security_level,
									 Relids qualscope,
									 Relids ojscope,
									 Relids outerjoin_nonnullable,
									 Relids incompatible_relids,
									 bool allow_equivalence,
									 bool has_clone,
									 bool is_clone,
									 List **postponed_oj_qual_list);
static void distribute_qual_to_rels(PlannerInfo *root, Node *clause,
									JoinTreeItem *jtitem,
									SpecialJoinInfo *sjinfo,
									Index security_level,
									Relids qualscope,
									Relids ojscope,
									Relids outerjoin_nonnullable,
									Relids incompatible_relids,
									bool allow_equivalence,
									bool has_clone,
									bool is_clone,
									List **postponed_oj_qual_list);
static bool check_redundant_nullability_qual(PlannerInfo *root, Node *clause);
static Relids get_join_domain_min_rels(PlannerInfo *root, Relids domain_relids);
static void check_mergejoinable(RestrictInfo *restrictinfo);
static void check_hashjoinable(RestrictInfo *restrictinfo);
static void check_memoizable(RestrictInfo *restrictinfo);


/*****************************************************************************
 *
 *	 JOIN TREES（连接树）
 *
 *****************************************************************************/

/*
 * add_base_rels_to_query
 *
 *	  扫描查询的连接树，并为全部
 *	  基关系（例如表、子查询和函数 RTEs）
 *	  建立基关系 RelOptInfos。
 *
 * 初次调用必须把 root->parse->jointree 作为
 * jtnode 的值传入。函数内部会沿连接树递归。
 *
 * 这一过程结束时，查询里用到的每个非连接 RTE
 * 都应当对应一份基关系 RelOptInfo。有些基关系
 * 可能是 appendrel 父项，其成员关系将需要额外的 "otherrel"
 * RelOptInfos，但那些要到以后才加入。
 */
void
add_base_rels_to_query(PlannerInfo *root, Node *jtnode)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		(void) build_simple_rel(root, varno, NULL);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			add_base_rels_to_query(root, lfirst(l));
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		add_base_rels_to_query(root, j->larg);
		add_base_rels_to_query(root, j->rarg);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * add_other_rels_to_query
 *	  为 appendrel 基关系的子项建立 "otherrel" RelOptInfos
 *
 * 这一过程结束时，查询将要扫描的全部关系
 * 都应当有 RelOptInfos。
 */
void
add_other_rels_to_query(PlannerInfo *root)
{
	int			rti;

	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];
		RangeTblEntry *rte = root->simple_rte_array[rti];

		/* 可能有对应非基关系 RTEs 的空槽 */
		if (rel == NULL)
			continue;

		/* 忽略任何已经加入的 "otherrels"。 */
		if (rel->reloptkind != RELOPT_BASEREL)
			continue;

		/* 若标成可继承，就去找子项。 */
		if (rte->inh)
			expand_inherited_rtentry(root, rel, rte, rti);
	}
}


/*****************************************************************************
 *
 *	 TARGET LISTS（目标列表）
 *
 *****************************************************************************/

/*
 * build_base_rel_tlists
 *	  把查询最终 tlist 所需的每个 var
 *	  （以及 HAVING 子句，若有）的目标列表项加到相应的基关系上。
 *
 * 把这类 var 标成 "关系 0" 所需，以确保它们会
 * 经全部连接计划步骤向上传播。
 */
void
build_base_rel_tlists(PlannerInfo *root, List *final_tlist)
{
	List	   *tlist_vars = pull_var_clause((Node *) final_tlist,
											 PVC_RECURSE_AGGREGATES |
											 PVC_RECURSE_WINDOWFUNCS |
											 PVC_INCLUDE_PLACEHOLDERS);

	if (tlist_vars != NIL)
	{
		add_vars_to_targetlist(root, tlist_vars, bms_make_singleton(0));
		list_free(tlist_vars);
	}

	/*
	 * 若有 HAVING 子句，也需要它用到的 Var。注意
	 * HAVING 可以含有 Aggref，但不能含有 WindowFuncs。
	 */
	if (root->parse->havingQual)
	{
		List	   *having_vars = pull_var_clause(root->parse->havingQual,
												  PVC_RECURSE_AGGREGATES |
												  PVC_INCLUDE_PLACEHOLDERS);

		if (having_vars != NIL)
		{
			add_vars_to_targetlist(root, having_vars,
								   bms_make_singleton(0));
			list_free(having_vars);
		}
	}
}

/*
 * add_vars_to_targetlist
 *	  对列表中出现的每个变量，若尚不存在，就把它加进所属
 *	  关系的目标列表，并把该变量标成
 *	  所示连接所需（若 where_needed 含有 "关系 0"，
 *	  则为最终输出所需）。
 *
 *	  列表里也可能有 PlaceHolderVars。它们不一定
 *	  只有一个所属关系；其 attr_needed 信息保存在
 *	  root->placeholder_list 里。找到或建立相关联的
 *	  PlaceHolderInfo 项，并更新其 ph_needed。
 */
void
add_vars_to_targetlist(PlannerInfo *root, List *vars,
					   Relids where_needed)
{
	ListCell   *temp;

	Assert(!bms_is_empty(where_needed));

	foreach(temp, vars)
	{
		Node	   *node = (Node *) lfirst(temp);

		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;
			RelOptInfo *rel = find_base_rel(root, var->varno);
			int			attno = var->varattno;

			if (bms_is_subset(where_needed, rel->relids))
				continue;
			Assert(attno >= rel->min_attr && attno <= rel->max_attr);
			attno -= rel->min_attr;
			if (rel->attr_needed[attno] == NULL)
			{
				/*
				 * 该变量尚未被请求，因此加进关系的目标列表。
				 *
				 * 关系扫描层可用的值还没有被
				 * 任何外连接置空，因此丢掉其 varnullingrels。
				 * （爬上连接树时会把它们放回去。）
				 */
				var = copyObject(var);
				var->varnullingrels = NULL;
				rel->reltarget->exprs = lappend(rel->reltarget->exprs, var);
				/* reltarget 的代价和宽度以后再算 */
			}
			rel->attr_needed[attno] = bms_add_members(rel->attr_needed[attno],
													  where_needed);
		}
		else if (IsA(node, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) node;
			PlaceHolderInfo *phinfo = find_placeholder_info(root, phv);

			phinfo->ph_needed = bms_add_members(phinfo->ph_needed,
												where_needed);
		}
		else
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
	}
}


/*****************************************************************************
 *
 *	  LATERAL REFERENCES（引用）
 *
 *****************************************************************************/

/*
 * find_lateral_references
 *	  对每个 LATERAL 子查询，抽出它对当前查询层次的 Var 和
 *	  PlaceHolderVars 的全部引用，并确保求值该子查询时
 *	  这些值可用。
 *
 * 后续规划步骤会保证 Var/PHVs 的源关系位于
 * 相对该 LATERAL 子查询的嵌套循环外侧，但还要
 * 保证这些 Var/PHVs 能传到嵌套循环连接那一层；这
 * 意味着要为它们设置合适的 where_needed。
 *
 * 注意这里只处理未展平的 LATERAL
 * 子查询里的 LATERAL 引用。展平 LATERAL 子查询时，其 LATERAL 引用
 * 会变成父查询里的普通 Var，但若需要被那些并不
 * 同时把该 LATERAL 子查询置空的外连接强制为 NULL，就得包进
 * PlaceHolderVars。那都在别处处理。
 *
 * 这必须在 deconstruct_jointree 之前运行，因为它可能
 * 导致建立 PlaceHolderInfos。
 */
void
find_lateral_references(PlannerInfo *root)
{
	Index		rti;

	/* 若查询不含 LATERAL RTEs，就不必做任何事 */
	if (!root->hasLateralRTEs)
		return;

	/*
	 * 检查全部基关系（rel 数组此时已经建好）。
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];

		/* 可能有对应非基关系 RTEs 的空槽 */
		if (brel == NULL)
			continue;

		Assert(brel->relid == rti); /* 对数组做健全性检查 */

		/*
		 * 这一点没有看上去那么显然。我们忽略 appendrel
		 * 的 otherrel，只考虑它们的父基关系。若某个
		 * 含 LATERAL 的 UNION ALL 子查询被上拉，真正会进入计划的
		 * 是 otherrel。但我们仍要
		 * 把其全部 LATERAL 引用标成父项所需，
		 * 因为连接规划用的是父项的 relid。
		 * 而父项的 RTEs 会包含我们需要知道的全部
		 * LATERAL 引用，因为上拉后的成员
		 * 不过是原 RTE 子查询某些部分的副本。也可以
		 * 去访问父项的子关系，再把它们的
		 * 引用变回父项的 relid，但那会复杂
		 * 得多，却没有实际收益。（这里重要的是子
		 * 成员除了被上拉之外还没接受任何处理。）
		 * 同样，继承展开建立的 appendrel
		 * 只看父关系就够了。
		 */

		/* 忽略作为 "other rels" 的 RTEs */
		if (brel->reloptkind != RELOPT_BASEREL)
			continue;

		extract_lateral_references(root, brel, rti);
	}
}

static void
extract_lateral_references(PlannerInfo *root, RelOptInfo *brel, Index rtindex)
{
	RangeTblEntry *rte = root->simple_rte_array[rtindex];
	List	   *vars;
	List	   *newvars;
	Relids		where_needed;
	ListCell   *lc;

	/* 若不是 LATERAL，就不可能有交叉引用 */
	if (!rte->lateral)
		return;

	/* 取出相应的变量 */
	if (rte->rtekind == RTE_RELATION)
		vars = pull_vars_of_level((Node *) rte->tablesample, 0);
	else if (rte->rtekind == RTE_SUBQUERY)
		vars = pull_vars_of_level((Node *) rte->subquery, 1);
	else if (rte->rtekind == RTE_FUNCTION)
		vars = pull_vars_of_level((Node *) rte->functions, 0);
	else if (rte->rtekind == RTE_TABLEFUNC)
		vars = pull_vars_of_level((Node *) rte->tablefunc, 0);
	else if (rte->rtekind == RTE_VALUES)
		vars = pull_vars_of_level((Node *) rte->values_lists, 0);
	else
	{
		Assert(false);
		return;					/* 消除编译器告警 */
	}

	if (vars == NIL)
		return;					/* 无事可做 */

	/* 复制每个 Var（或 PlaceHolderVar）并调整到本层次 */
	newvars = NIL;
	foreach(lc, vars)
	{
		Node	   *node = (Node *) lfirst(lc);

		node = copyObject(node);
		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;

			/* 调整很容易，因为它只是一个节点 */
			var->varlevelsup = 0;
		}
		else if (IsA(node, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) node;
			int			levelsup = phv->phlevelsup;

			/* 还得费点劲去调整所含的表达式 */
			if (levelsup != 0)
				IncrementVarSublevelsUp(node, -levelsup, 0);

			/*
			 * 若 PHV 是从子查询 RTE 里拉出来的，其表达式
			 * 需要预处理。不过 subquery_planner() 已经对
			 * 函数和 values RTEs 里层次为零的 PHVs 做过这件事。
			 */
			if (levelsup > 0)
				phv->phexpr = preprocess_phv_expression(root, phv->phexpr);
		}
		else
			Assert(false);
		newvars = lappend(newvars, node);
	}

	list_free(vars);

	/*
	 * 把这些 Var 标成在该 LATERAL RTE 上 "所需"。这有点
	 * 取巧：更形式化的做法是把每一个标成在
	 * 该 LATERAL RTE 与其源 RTE 的连接上所需。但这样能工作，
	 * 而且比为每个 Var 单独算一份 where_needed
	 * 省事得多。
	 */
	where_needed = bms_make_singleton(rtindex);

	/*
	 * 把 Var 推进其源关系的目标列表，把 PHVs 推进
	 * root->placeholder_list。
	 */
	add_vars_to_targetlist(root, newvars, where_needed);

	/* 记住这些 LATERAL 引用，供 create_lateral_join_info 使用 */
	brel->lateral_vars = newvars;
}

/*
 * create_lateral_join_info
 *	  填入每个基关系的 direct_lateral_relids、lateral_relids
 *	  和 lateral_referencers 集合。
 */
void
create_lateral_join_info(PlannerInfo *root)
{
	bool		found_laterals = false;
	Index		rti;
	ListCell   *lc;

	/* 若查询不含 LATERAL RTEs，就不必做任何事 */
	if (!root->hasLateralRTEs)
		return;

	/* 将需要 PlaceHolderVars 的 ph_eval_at 值 */
	Assert(root->placeholdersFrozen);

	/*
	 * 检查全部基关系（rel 数组此时已经建好）。
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];
		Relids		lateral_relids;

		/* 可能有对应非基关系 RTEs 的空槽 */
		if (brel == NULL)
			continue;

		Assert(brel->relid == rti); /* 对数组做健全性检查 */

		/* 忽略作为 "other rels" 的 RTEs */
		if (brel->reloptkind != RELOPT_BASEREL)
			continue;

		lateral_relids = NULL;

		/* 考虑每个被 LATERAL 引用的 Var 或 PHV */
		foreach(lc, brel->lateral_vars)
		{
			Node	   *node = (Node *) lfirst(lc);

			if (IsA(node, Var))
			{
				Var		   *var = (Var *) node;

				found_laterals = true;
				lateral_relids = bms_add_member(lateral_relids,
												var->varno);
			}
			else if (IsA(node, PlaceHolderVar))
			{
				PlaceHolderVar *phv = (PlaceHolderVar *) node;
				PlaceHolderInfo *phinfo = find_placeholder_info(root, phv);

				found_laterals = true;
				lateral_relids = bms_add_members(lateral_relids,
												 phinfo->ph_eval_at);
			}
			else
				Assert(false);
		}

		/* 现在已有来自该关系的全部简单 LATERAL 引用 */
		brel->direct_lateral_relids = lateral_relids;
		brel->lateral_relids = bms_copy(lateral_relids);
	}

	/*
	 * 接着检查 PlaceHolderVars 内部的 LATERAL 引用，并把它们的
	 * eval_at 关系标成对源关系有 LATERAL 引用。
	 *
	 * 若某个 PHV 将在基关系上求值，就把其源关系
	 * 标成该基关系的直接 LATERAL 依赖（加到上面已记录的那些上）。
	 * 若它将在连接上求值，就把其源关系
	 * 标成该连接中每个基关系的间接 LATERAL 依赖，
	 * 也就是放进 lateral_relids，但不放进 direct_lateral_relids。这样是
	 * 合适的，因为不能把任何这种基关系放在与该
	 * PHV 的某个 LATERAL 依赖做连接时的外侧，但另一方面也
	 * 还不能直接把它连到那个依赖上。
	 */
	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);
		Relids		eval_at = phinfo->ph_eval_at;
		Relids		lateral_refs;
		int			varno;

		if (phinfo->ph_lateral == NULL)
			continue;			/* 若 PHV 没有 LATERAL 引用，就没什么意思 */

		found_laterals = true;

		/*
		 * 求值点的 LATERAL relid 里只包括基关系，不包括外连接。
		 * 这样可避免外连接顺序被重排时出问题，
		 * 并且仍应保证需要时 LATERAL 值
		 * 已经可用。
		 */
		lateral_refs = bms_intersect(phinfo->ph_lateral, root->all_baserels);
		Assert(!bms_is_empty(lateral_refs));

		if (bms_get_singleton_member(eval_at, &varno))
		{
			/* 求值点是一个基关系 */
			RelOptInfo *brel = find_base_rel(root, varno);

			brel->direct_lateral_relids =
				bms_add_members(brel->direct_lateral_relids,
								lateral_refs);
			brel->lateral_relids =
				bms_add_members(brel->lateral_relids,
								lateral_refs);
		}
		else
		{
			/* 求值点是一个连接 */
			varno = -1;
			while ((varno = bms_next_member(eval_at, varno)) >= 0)
			{
				RelOptInfo *brel = find_base_rel_ignore_join(root, varno);

				if (brel == NULL)
					continue;	/* 忽略 eval_at 里的外连接 */
				brel->lateral_relids = bms_add_members(brel->lateral_relids,
													   lateral_refs);
			}
		}
	}

	/*
	 * 若没找到实际的 LATERAL 引用，就做完了；但要重置
	 * hasLateralRTEs 标志，以免以后做无用功。
	 */
	if (!found_laterals)
	{
		root->hasLateralRTEs = false;
		return;
	}

	/*
	 * 计算 lateral_relids 集合的传递闭包，使它们
	 * 同时描述直接和间接的 LATERAL 引用。若关系
	 * X LATERAL 引用 Y，而 Y LATERAL 引用 Z，则
	 * 必须在与 Z 的嵌套循环内侧扫描 X，因此就
	 * 各种实际目的而言，X 也 LATERAL 依赖于 Z。
	 *
	 * 这段代码本质上是 Warshall 的传递闭包算法。
	 * 外层循环考虑每个基关系，并把它的 LATERAL
	 * 依赖传播给那些对它有 LATERAL 依赖的基关系。
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];
		Relids		outer_lateral_relids;
		Index		rti2;

		if (brel == NULL || brel->reloptkind != RELOPT_BASEREL)
			continue;

		/* 若基关系没有 LATERAL 引用，就不必再考虑它 */
		outer_lateral_relids = brel->lateral_relids;
		if (outer_lateral_relids == NULL)
			continue;

		/* 否则扫描全部基关系 */
		for (rti2 = 1; rti2 < root->simple_rel_array_size; rti2++)
		{
			RelOptInfo *brel2 = root->simple_rel_array[rti2];

			if (brel2 == NULL || brel2->reloptkind != RELOPT_BASEREL)
				continue;

			/* 若 brel2 对 brel 有 LATERAL 引用，就传播 brel 的引用 */
			if (bms_is_member(rti, brel2->lateral_relids))
				brel2->lateral_relids = bms_add_members(brel2->lateral_relids,
														outer_lateral_relids);
		}
	}

	/*
	 * 既然已经识别出全部 LATERAL 引用，就给每个基关系标上
	 * LATERAL 引用它（可能是间接的）的那些关系的 relid 集合
	 * ——也就是 lateral_relids 的逆映射。
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];
		Relids		lateral_relids;
		int			rti2;

		if (brel == NULL || brel->reloptkind != RELOPT_BASEREL)
			continue;

		/* 对没有 LATERAL 引用的关系无事可做 */
		lateral_relids = brel->lateral_relids;
		if (bms_is_empty(lateral_relids))
			continue;

		/* 任何关系都不应当 LATERAL 依赖自身 */
		Assert(!bms_is_member(rti, lateral_relids));

		/* 标记引用本关系的那些关系 */
		rti2 = -1;
		while ((rti2 = bms_next_member(lateral_relids, rti2)) >= 0)
		{
			RelOptInfo *brel2 = root->simple_rel_array[rti2];

			if (brel2 == NULL)
				continue;		/* 必须是一个外连接 */

			Assert(brel2->reloptkind == RELOPT_BASEREL);
			brel2->lateral_referencers =
				bms_add_member(brel2->lateral_referencers, rti);
		}
	}
}


/*****************************************************************************
 *
 *	  JOIN TREE PROCESSING（连接树处理）
 *
 *****************************************************************************/

/*
 * deconstruct_jointree
 *	  递归扫描查询连接树中的 WHERE 和 JOIN/ON 限定
 *	  子句，并把它们加到基 RelOptInfos 所属的相应 restrictinfo 和 joininfo
 *	  列表。同时，为查询树中出现的任何外连接把 SpecialJoinInfo 节点
 *	  加进 root->join_info_list。
 *	  返回一份 "joinlist" 数据结构，表示
 *	  make_one_rel() 需要做出的连接顺序决策。
 *
 * "joinlist" 结果是一份列表，元素要么是 RangeTblRef
 * 连接树节点，要么是子 joinlist。同一层
 * joinlist 的全部元素必须按 make_one_rel() 将要决定的顺序连接
 * （注意合法顺序可能受 SpecialJoinInfo 节点约束）。
 * 子 joinlist 表示一个要单独规划的子问题。目前
 * 子 joinlist 只来自 FULL OUTER JOIN，或当子问题的折叠
 * 被 join_collapse_limit 或 from_collapse_limit 停住时。
 */
List *
deconstruct_jointree(PlannerInfo *root)
{
	List	   *result;
	JoinDomain *top_jdomain;
	List	   *item_list = NIL;
	ListCell   *lc;

	/*
	 * 此后不得再建立 PlaceHolderInfos，因为
	 * make_outerjoininfo 需要全部活动占位符都已在
	 * root->placeholder_list 里，我们才会沿连接树向上爬。
	 */
	root->placeholdersFrozen = true;

	/* 取出已经为该查询建好的顶层连接域 */
	top_jdomain = linitial_node(JoinDomain, root->join_domains);
	top_jdomain->jd_relids = NULL;	/* 在 deconstruct_recurse 期间填入 */

	/* 从连接树顶部开始递归 */
	Assert(root->parse->jointree != NULL &&
		   IsA(root->parse->jointree, FromExpr));

	/* 扫描连接树时填入这些 */
	root->all_baserels = NULL;
	root->outer_join_rels = NULL;

	/* 对连接树做初次扫描 */
	result = deconstruct_recurse(root, (Node *) root->parse->jointree,
								 top_jdomain, NULL,
								 &item_list);

	/* 现在也可以形成 all_query_rels 的值了 */
	root->all_query_rels = bms_union(root->all_baserels, root->outer_join_rels);

	/* ……它应当与为顶层连接域算出的值一致 */
	Assert(bms_equal(root->all_query_rels, top_jdomain->jd_relids));

	/* 再扫描全部连接树节点，并分发限定条件 */
	foreach(lc, item_list)
	{
		JoinTreeItem *jtitem = (JoinTreeItem *) lfirst(lc);

		deconstruct_distribute(root, jtitem);
	}

	/*
	 * 若有任何特殊连接，也许还有一些推迟的 LEFT
	 * JOIN 子句要处理。
	 */
	if (root->join_info_list)
	{
		foreach(lc, item_list)
		{
			JoinTreeItem *jtitem = (JoinTreeItem *) lfirst(lc);

			if (jtitem->oj_joinclauses != NIL)
				deconstruct_distribute_oj_quals(root, item_list, jtitem);
		}
	}

	/* 不再需要这些 JoinTreeItems */
	list_free_deep(item_list);

	return result;
}

/*
 * deconstruct_recurse
 *	  deconstruct_jointree 初次扫描连接树的一层递归。
 *
 * jtnode 是要检查的连接树节点，parent_domain 是
 * 外层连接域。（必须把这里或更下层出现的全部 base+OJ relid
 * 加进 parent_domain。）parent_jtitem 是父连接树节点的
 * JoinTreeItem，在递归顶层则为 NULL。
 *
 * item_list 是输入/输出参数：对每个连接树节点，都把一个
 * JoinTreeItem 结构体按深度优先遍历顺序加进该列表。
 * （因此每次调用之后，列表最后一项对应该次的 jtnode。）
 *
 * 返回值是这个连接树节点对应的 joinlist。
 */
static List *
deconstruct_recurse(PlannerInfo *root, Node *jtnode,
					JoinDomain *parent_domain,
					JoinTreeItem *parent_jtitem,
					List **item_list)
{
	List	   *joinlist;
	JoinTreeItem *jtitem;

	Assert(jtnode != NULL);

	/* 建立新的 JoinTreeItem，但先不要加进 item_list */
	jtitem = palloc0_object(JoinTreeItem);
	jtitem->jtnode = jtnode;
	jtitem->jti_parent = parent_jtitem;

	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		/* 遇到基关系连接树节点时填入 all_baserels */
		root->all_baserels = bms_add_member(root->all_baserels, varno);
		/* 本节点属于 parent_domain */
		jtitem->jdomain = parent_domain;
		parent_domain->jd_relids = bms_add_member(parent_domain->jd_relids,
												  varno);
		/* qualscope 就是这一个 RTE */
		jtitem->qualscope = bms_make_singleton(varno);
		/* 单个基关系并不构成内连接 */
		jtitem->inner_join_rels = NULL;
		joinlist = list_make1(jtnode);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		int			remaining;
		ListCell   *l;

		/* 本节点属于 parent_domain，其子节点也是 */
		jtitem->jdomain = parent_domain;

		/*
		 * 递归处理子节点，并算出输出 joinlist。只要得到的
		 * joinlist 不会超过 from_collapse_limit 个成员，就把子问题
		 * 折叠进同一份 joinlist。另外，单元素子问题总是
		 * 折叠，因为那反正不会把
		 * joinlist 变长。
		 */
		jtitem->qualscope = NULL;
		jtitem->inner_join_rels = NULL;
		joinlist = NIL;
		remaining = list_length(f->fromlist);
		foreach(l, f->fromlist)
		{
			JoinTreeItem *sub_item;
			List	   *sub_joinlist;
			int			sub_members;

			sub_joinlist = deconstruct_recurse(root, lfirst(l),
											   parent_domain,
											   jtitem,
											   item_list);
			sub_item = (JoinTreeItem *) llast(*item_list);
			jtitem->qualscope = bms_add_members(jtitem->qualscope,
												sub_item->qualscope);
			jtitem->inner_join_rels = sub_item->inner_join_rels;
			sub_members = list_length(sub_joinlist);
			remaining--;
			if (sub_members <= 1 ||
				list_length(joinlist) + sub_members + remaining <= from_collapse_limit)
				joinlist = list_concat(joinlist, sub_joinlist);
			else
				joinlist = lappend(joinlist, sub_joinlist);
		}

		/*
		 * 含有多于一个列表元素的 FROM 是一个内连接，并入
		 * 其下的全部内容，因此应当报告 inner_join_rels = qualscope。若
		 * 恰好只有一个元素，就应当（而且已经）报告
		 * 它自己的 inner_join_rels。若一个元素都没有（现在
		 * 还可能吗？），循环前的初始化已经把它定好了。
		 */
		if (list_length(f->fromlist) > 1)
			jtitem->inner_join_rels = jtitem->qualscope;
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		JoinDomain *child_domain,
				   *fj_domain;
		JoinTreeItem *left_item,
				   *right_item;
		List	   *leftjoinlist,
				   *rightjoinlist;

		switch (j->jointype)
		{
			case JOIN_INNER:
				/* 本节点属于 parent_domain，其子节点也是 */
				jtitem->jdomain = parent_domain;
				/* 继续递归 */
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   parent_domain,
												   jtitem,
												   item_list);
				left_item = (JoinTreeItem *) llast(*item_list);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													parent_domain,
													jtitem,
													item_list);
				right_item = (JoinTreeItem *) llast(*item_list);
				/* 计算 qualscope 等 */
				jtitem->qualscope = bms_union(left_item->qualscope,
											  right_item->qualscope);
				jtitem->inner_join_rels = jtitem->qualscope;
				jtitem->left_rels = left_item->qualscope;
				jtitem->right_rels = right_item->qualscope;
				/* 内连接不为限定条件增加限制 */
				jtitem->nonnullable_rels = NULL;
				break;
			case JOIN_LEFT:
			case JOIN_ANTI:
				/* 为我的限定条件和右侧建立新的连接域 */
				child_domain = makeNode(JoinDomain);
				child_domain->jd_relids = NULL; /* 由递归填入 */
				root->join_domains = lappend(root->join_domains, child_domain);
				jtitem->jdomain = child_domain;
				/* 继续递归 */
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   parent_domain,
												   jtitem,
												   item_list);
				left_item = (JoinTreeItem *) llast(*item_list);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													child_domain,
													jtitem,
													item_list);
				right_item = (JoinTreeItem *) llast(*item_list);
				/* 计算连接域内容、qualscope 等 */
				parent_domain->jd_relids =
					bms_add_members(parent_domain->jd_relids,
									child_domain->jd_relids);
				jtitem->qualscope = bms_union(left_item->qualscope,
											  right_item->qualscope);
				/* 注意：由 SEMI 推导出的 ANTI 连接会缺少 rtindex */
				if (j->rtindex != 0)
				{
					parent_domain->jd_relids =
						bms_add_member(parent_domain->jd_relids,
									   j->rtindex);
					jtitem->qualscope = bms_add_member(jtitem->qualscope,
													   j->rtindex);
					root->outer_join_rels = bms_add_member(root->outer_join_rels,
														   j->rtindex);
					mark_rels_nulled_by_join(root, j->rtindex,
											 right_item->qualscope);
				}
				jtitem->inner_join_rels = bms_union(left_item->inner_join_rels,
													right_item->inner_join_rels);
				jtitem->left_rels = left_item->qualscope;
				jtitem->right_rels = right_item->qualscope;
				jtitem->nonnullable_rels = left_item->qualscope;
				break;
			case JOIN_SEMI:
				/* 本节点属于 parent_domain，其子节点也是 */
				jtitem->jdomain = parent_domain;
				/* 继续递归 */
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   parent_domain,
												   jtitem,
												   item_list);
				left_item = (JoinTreeItem *) llast(*item_list);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													parent_domain,
													jtitem,
													item_list);
				right_item = (JoinTreeItem *) llast(*item_list);
				/* 计算 qualscope 等 */
				jtitem->qualscope = bms_union(left_item->qualscope,
											  right_item->qualscope);
				/* SEMI 连接从没有 rtindex，因此不要加进任何集合 */
				Assert(j->rtindex == 0);
				jtitem->inner_join_rels = bms_union(left_item->inner_join_rels,
													right_item->inner_join_rels);
				jtitem->left_rels = left_item->qualscope;
				jtitem->right_rels = right_item->qualscope;
				/* 半连接不为限定条件增加限制 */
				jtitem->nonnullable_rels = NULL;
				break;
			case JOIN_FULL:
				/* FULL JOIN 的限定条件需要完全属于自己的域 */
				fj_domain = makeNode(JoinDomain);
				root->join_domains = lappend(root->join_domains, fj_domain);
				jtitem->jdomain = fj_domain;
				/* 递归，给每一侧各自的连接域 */
				child_domain = makeNode(JoinDomain);
				child_domain->jd_relids = NULL; /* 由递归填入 */
				root->join_domains = lappend(root->join_domains, child_domain);
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   child_domain,
												   jtitem,
												   item_list);
				left_item = (JoinTreeItem *) llast(*item_list);
				fj_domain->jd_relids = bms_copy(child_domain->jd_relids);
				child_domain = makeNode(JoinDomain);
				child_domain->jd_relids = NULL; /* 由递归填入 */
				root->join_domains = lappend(root->join_domains, child_domain);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													child_domain,
													jtitem,
													item_list);
				right_item = (JoinTreeItem *) llast(*item_list);
				/* 计算 qualscope 等 */
				fj_domain->jd_relids = bms_add_members(fj_domain->jd_relids,
													   child_domain->jd_relids);
				parent_domain->jd_relids = bms_add_members(parent_domain->jd_relids,
														   fj_domain->jd_relids);
				jtitem->qualscope = bms_union(left_item->qualscope,
											  right_item->qualscope);
				Assert(j->rtindex != 0);
				parent_domain->jd_relids = bms_add_member(parent_domain->jd_relids,
														  j->rtindex);
				jtitem->qualscope = bms_add_member(jtitem->qualscope,
												   j->rtindex);
				root->outer_join_rels = bms_add_member(root->outer_join_rels,
													   j->rtindex);
				mark_rels_nulled_by_join(root, j->rtindex,
										 left_item->qualscope);
				mark_rels_nulled_by_join(root, j->rtindex,
										 right_item->qualscope);
				jtitem->inner_join_rels = bms_union(left_item->inner_join_rels,
													right_item->inner_join_rels);
				jtitem->left_rels = left_item->qualscope;
				jtitem->right_rels = right_item->qualscope;
				/* 每一侧既是外侧也是内侧 */
				jtitem->nonnullable_rels = jtitem->qualscope;
				break;
			default:
				/* JOIN_RIGHT 已在 reduce_outer_joins() 期间被消掉 */
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				leftjoinlist = rightjoinlist = NIL; /* 消除编译器告警 */
				break;
		}

		/*
		 * 计算输出 joinlist。除了在 FULL JOIN 处，或会超过
		 * join_collapse_limit 的地方，都把子问题折到一起。
		 */
		if (j->jointype == JOIN_FULL)
		{
			/* 在本节点强制连接顺序完全固定 */
			joinlist = list_make1(list_make2(leftjoinlist, rightjoinlist));
		}
		else if (list_length(leftjoinlist) + list_length(rightjoinlist) <=
				 join_collapse_limit)
		{
			/* 可以把子问题合并 */
			joinlist = list_concat(leftjoinlist, rightjoinlist);
		}
		else
		{
			/* 不能合并，但不必强制此处以上的连接顺序 */
			Node	   *leftpart,
					   *rightpart;

			/* 避免建立无用的单元素子列表 */
			if (list_length(leftjoinlist) == 1)
				leftpart = (Node *) linitial(leftjoinlist);
			else
				leftpart = (Node *) leftjoinlist;
			if (list_length(rightjoinlist) == 1)
				rightpart = (Node *) linitial(rightjoinlist);
			else
				rightpart = (Node *) rightjoinlist;
			joinlist = list_make2(leftpart, rightpart);
		}
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
		joinlist = NIL;			/* 消除编译器告警 */
	}

	/* 最后可以把新的 JoinTreeItem 加进 item_list */
	*item_list = lappend(*item_list, jtitem);

	return joinlist;
}

/*
 * deconstruct_distribute
 *	  处理 deconstruct_jointree 第二阶段中的一个连接树节点。
 *
 * 把该节点的限定条件分发到相应的限制列表和连接列表。
 * 此外，外连接会向 root->join_info_list 添加项。
 */
static void
deconstruct_distribute(PlannerInfo *root, JoinTreeItem *jtitem)
{
	Node	   *jtnode = jtitem->jtnode;

	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		/* 处理挂在该 RTE 上的任何 securityQuals */
		if (root->qual_security_level > 0)
			process_security_barrier_quals(root,
										   varno,
										   jtitem);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;

		/*
		 * 处理被子节点推迟到本层的、带 LATERAL 引用的
		 * 限定条件。
		 */
		distribute_quals_to_rels(root, jtitem->lateral_clauses,
								 jtitem,
								 NULL,
								 root->qual_security_level,
								 jtitem->qualscope,
								 NULL, NULL, NULL,
								 true, false, false,
								 NULL);

		/*
		 * 现在处理顶层限定条件。
		 */
		distribute_quals_to_rels(root, (List *) f->quals,
								 jtitem,
								 NULL,
								 root->qual_security_level,
								 jtitem->qualscope,
								 NULL, NULL, NULL,
								 true, false, false,
								 NULL);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		Relids		ojscope;
		List	   *my_quals;
		SpecialJoinInfo *sjinfo;
		List	  **postponed_oj_qual_list;

		/*
		 * 把从子节点推迟来的、带 LATERAL 引用的限定条件并入
		 * my_quals，以便在 make_outerjoininfo 里
		 * 正确处理。（这会破坏
		 * jtitem->lateral_clauses，但以后不再使用它。）
		 */
		my_quals = list_concat(jtitem->lateral_clauses,
							   (List *) j->quals);

		/*
		 * 对 OJ，现在就建立 SpecialJoinInfo，以便传给
		 * distribute_qual_to_rels。也必须算出它的 ojscope。
		 *
		 * 半连接有点混合：我们建立 SpecialJoinInfo，但希望
		 * 传给 distribute_qual_to_rels 的 ojscope 为 NULL。
		 */
		if (j->jointype != JOIN_INNER)
		{
			sjinfo = make_outerjoininfo(root,
										jtitem->left_rels,
										jtitem->right_rels,
										jtitem->inner_join_rels,
										j->jointype,
										j->rtindex,
										my_quals);
			jtitem->sjinfo = sjinfo;
			if (j->jointype == JOIN_SEMI)
				ojscope = NULL;
			else
				ojscope = bms_union(sjinfo->min_lefthand,
									sjinfo->min_righthand);
		}
		else
		{
			sjinfo = NULL;
			ojscope = NULL;
		}

		/*
		 * 若这是左连接，且有一条对左侧严格的连接子句，
		 * 就需要推迟处理任何非退化的连接
		 * 子句，以防该连接能按恒等式 3 与另一个左连接
		 * 交换。（退化子句不必推迟，因为
		 * 它们反正会落到本连接之下。）
		 */
		if (j->jointype == JOIN_LEFT && sjinfo->lhs_strict)
		{
			postponed_oj_qual_list = &jtitem->oj_joinclauses;

			/*
			 * 把从 min_lefthand 或 min_righthand 里去掉的、可交换的下层 OJ relid
			 * 加回去，否则 distribute_qual_to_rels 里的 ojscope
			 * 交叉检查会抱怨。由于正在推迟
			 * 非退化子句的处理，这次加回除了那次交叉检查之外
			 * 不影响任何东西。真正的子句
			 * 定位决策要到以后重访这些
			 * 被推迟的子句时才做。
			 */
			ojscope = bms_add_members(ojscope, sjinfo->commute_below_l);
			ojscope = bms_add_members(ojscope, sjinfo->commute_below_r);
		}
		else
			postponed_oj_qual_list = NULL;

		/* 处理该 JOIN 的限定子句 */
		distribute_quals_to_rels(root, my_quals,
								 jtitem,
								 sjinfo,
								 root->qual_security_level,
								 jtitem->qualscope,
								 ojscope, jtitem->nonnullable_rels,
								 NULL,	/* incompatible_relids（不相容的 relid） */
								 true,	/* allow_equivalence（允许等价） */
								 false, false,	/* 不是克隆 */
								 postponed_oj_qual_list);

		/* 并把该 SpecialJoinInfo 加进 join_info_list */
		if (sjinfo)
			root->join_info_list = lappend(root->join_info_list, sjinfo);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	}
}

/*
 * process_security_barrier_quals
 *	  把安全屏障限定条件转入关系的 baserestrictinfo 列表。
 *
 * 改写器把相关的安全屏障条件放进了 RTE 的
 * securityQuals 字段，现在该把它们复制进该关系的
 * baserestrictinfo。
 *
 * 继承情况下，这里只考虑挂在父关系上的限定条件；
 * 它们对全部子项也有效，因此可以拿来做
 * 等价类建立之类的用途。挂在
 * 单个子关系上的限定条件在路径建立期间再处理。
 */
static void
process_security_barrier_quals(PlannerInfo *root,
							   int rti, JoinTreeItem *jtitem)
{
	RangeTblEntry *rte = root->simple_rte_array[rti];
	Index		security_level = 0;
	ListCell   *lc;

	/*
	 * securityQuals 列表的每个元素都已预处理成一份
	 * 隐式 ANDed 的子句列表。同一子列表里的全部子句
	 * 应当得到相同的安全级别，但相继的子列表级别
	 * 越来越高。
	 */
	foreach(lc, rte->securityQuals)
	{
		List	   *qualset = (List *) lfirst(lc);

		/*
		 * 我们取巧，传入 ojscope = qualscope，而不是
		 * 更合乎逻辑的 NULL。这样做的唯一效果是
		 * 迫使不含 Var 的限定条件在该关系上求值，而不是被
		 * 推到树顶，那正是我们不想要的。
		 */
		distribute_quals_to_rels(root, qualset,
								 jtitem,
								 NULL,
								 security_level,
								 jtitem->qualscope,
								 jtitem->qualscope,
								 NULL,
								 NULL,
								 true,
								 false, false,	/* 不是克隆 */
								 NULL);
		security_level++;
	}

	/* 断言 qual_security_level 高于刚刚用过的任何级别 */
	Assert(security_level <= root->qual_security_level);
}

/*
 * mark_rels_nulled_by_join
 *	  填写被此外连接置空的基关系的 RelOptInfo.nulling_relids
 *
 * 输入：
 *	ojrelid：连接 RTE 的 RT 下标（不得为 0）
 *	lower_rels：语法上位于连接可空侧之下的 base+OJ Relids
 */
static void
mark_rels_nulled_by_join(PlannerInfo *root, Index ojrelid,
						 Relids lower_rels)
{
	int			relid = -1;

	while ((relid = bms_next_member(lower_rels, relid)) > 0)
	{
		RelOptInfo *rel = root->simple_rel_array[relid];

		if (rel == NULL)		/* 必须是一个外连接 */
		{
			Assert(bms_is_member(relid, root->outer_join_rels));
			continue;
		}
		rel->nulling_relids = bms_add_member(rel->nulling_relids, ojrelid);
	}
}

/*
 * make_outerjoininfo
 *	  为当前外连接建立一份 SpecialJoinInfo
 *
 * 输入：
 *	left_rels：语法上位于连接外侧的 base+OJ Relids
 *	right_rels：语法上位于连接内侧的 base+OJ Relids
 *	inner_join_rels：参与本连接之下内连接的 base+OJ Relids
 *	jointype：字面含义（必须总是 LEFT、FULL、SEMI 或 ANTI）
 *	ojrelid：连接 RTE 的 RT 下标（SEMI 为 0，它不在 RT 列表里）
 *	clause：该外连接的连接条件（隐式 AND 格式）
 *
 * 该节点最终应追加到 root->join_info_list，但
 * 这里不做这件事。
 *
 * 注意：假定本函数自底向上调用，因此
 * root->join_info_list 已经含有语法上位于本连接之下的
 * 全部外连接的项。
 */
static SpecialJoinInfo *
make_outerjoininfo(PlannerInfo *root,
				   Relids left_rels, Relids right_rels,
				   Relids inner_join_rels,
				   JoinType jointype, Index ojrelid,
				   List *clause)
{
	SpecialJoinInfo *sjinfo = makeNode(SpecialJoinInfo);
	Relids		clause_relids;
	Relids		strict_relids;
	Relids		min_lefthand;
	Relids		min_righthand;
	Relids		commute_below_l;
	Relids		commute_below_r;
	ListCell   *l;

	/*
	 * 这里不应当见到 RIGHT JOIN，因为左右侧
	 * 先前已经对调过。
	 */
	Assert(jointype != JOIN_INNER);
	Assert(jointype != JOIN_RIGHT);

	/*
	 * 目前执行器无法支持对外连接
	 * 可空侧上出现的关系做 FOR [KEY] UPDATE/SHARE 标记。（那到底
	 * 意味着什么也不太清楚：当结果
	 * 行并非由可空关系的任何元素生成时，该标记什么？）因此，
	 * 若任何可空关系带有 FOR [KEY] UPDATE/SHARE，就报错。
	 *
	 * 也许会问为什么不在更上游的
	 * 解析器里做这个检查。那是因为解析器信息不够——考虑
	 * 对视图使用 FOR UPDATE 的情况。只有改写并展平之后，
	 * 才知道该视图是否含有外连接。
	 *
	 * 这里使用原始的 RowMarkClause 列表；PlanRowMark 列表会
	 * 把所有东西都列出来。
	 */
	foreach(l, root->parse->rowMarks)
	{
		RowMarkClause *rc = (RowMarkClause *) lfirst(l);

		if (bms_is_member(rc->rti, right_rels) ||
			(jointype == JOIN_FULL && bms_is_member(rc->rti, left_rels)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/*------
			 translator: %s is a SQL row locking clause such as FOR UPDATE */
					 errmsg("%s cannot be applied to the nullable side of an outer join",
							LCS_asString(rc->strength))));
	}

	sjinfo->syn_lefthand = left_rels;
	sjinfo->syn_righthand = right_rels;
	sjinfo->jointype = jointype;
	sjinfo->ojrelid = ojrelid;
	/* 这些字段以后可能会再追加： */
	sjinfo->commute_above_l = NULL;
	sjinfo->commute_above_r = NULL;
	sjinfo->commute_below_l = NULL;
	sjinfo->commute_below_r = NULL;

	compute_semijoin_info(root, sjinfo, clause);

	/* 若是全连接，就不必太精明 */
	if (jointype == JOIN_FULL)
	{
		sjinfo->min_lefthand = bms_copy(left_rels);
		sjinfo->min_righthand = bms_copy(right_rels);
		sjinfo->lhs_strict = false; /* 这个不用管 */
		return sjinfo;
	}

	/*
	 * 取出连接子句里提到的全部 relid。
	 */
	clause_relids = pull_varnos(root, (Node *) clause);

	/*
	 * 该子句对哪些 relid 是严格的，即若该关系的
	 * 列全为 NULL，它就无法成功？
	 */
	strict_relids = find_nonnullable_rels((Node *) clause);

	/* 记住该子句是否对任何左侧关系严格 */
	sjinfo->lhs_strict = bms_overlap(strict_relids, left_rels);

	/*
	 * 必需的左侧总是包括子句里提到的左侧关系。根据
	 * 更下层的外连接，也许还得再加一些关系；见下文。
	 */
	min_lefthand = bms_intersect(clause_relids, left_rels);

	/*
	 * 右侧同理。但这里还必须包括任何更下层的
	 * 内连接，以确保不会试图与它们中的任何一个交换。
	 */
	min_righthand = bms_int_members(bms_union(clause_relids, inner_join_rels),
									right_rels);

	/*
	 * 现在检查先前的外连接，看有没有顺序限制。
	 *
	 * commute_below_l 和 commute_below_r 累积我们认为本连接
	 * 可以与之交换的下层外连接的 relid。这些判断
	 * 在本循环里只是暂定的，因为也许会发现某个
	 * 中间外连接阻止交换。留下来的 relid
	 * 随后会并入 SpecialJoinInfo 结构体。
	 */
	commute_below_l = commute_below_r = NULL;
	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *otherinfo = (SpecialJoinInfo *) lfirst(l);
		bool		have_unsafe_phvs;

		/*
		 * 全连接是优化屏障：不能向它内部或
		 * 外部做结合。因此，若它与当前关系的左侧或右侧有重叠，
		 * 就把那一侧的最小关系集扩大到覆盖整个全连接。
		 */
		if (otherinfo->jointype == JOIN_FULL)
		{
			Assert(otherinfo->ojrelid != 0);
			if (bms_overlap(left_rels, otherinfo->syn_lefthand) ||
				bms_overlap(left_rels, otherinfo->syn_righthand))
			{
				min_lefthand = bms_add_members(min_lefthand,
											   otherinfo->syn_lefthand);
				min_lefthand = bms_add_members(min_lefthand,
											   otherinfo->syn_righthand);
				min_lefthand = bms_add_member(min_lefthand,
											  otherinfo->ojrelid);
			}
			if (bms_overlap(right_rels, otherinfo->syn_lefthand) ||
				bms_overlap(right_rels, otherinfo->syn_righthand))
			{
				min_righthand = bms_add_members(min_righthand,
												otherinfo->syn_lefthand);
				min_righthand = bms_add_members(min_righthand,
												otherinfo->syn_righthand);
				min_righthand = bms_add_member(min_righthand,
											   otherinfo->ojrelid);
			}
			/* 对这个全连接不必再做别的事 */
			continue;
		}

		/*
		 * 若连接条件含有任何需要在下层 OJ 之上求值的
		 * PlaceHolderVars，就不能与它交换。
		 */
		if (otherinfo->ojrelid != 0)
			have_unsafe_phvs =
				contain_placeholder_references_to(root,
												  (Node *) clause,
												  otherinfo->ojrelid);
		else
			have_unsafe_phvs = false;

		/*
		 * 对我们左侧的下层 OJ，若连接条件用到该下层
		 * 连接的右侧，且对该关系不严格，就必须保持这两个
		 * OJ 的顺序，因此把下层 OJ 的全部语法关系集加进
		 * min_lefthand。（必须用它的全部语法关系集，而不是只用
		 * min_lefthand + min_righthand。这是因为本 OJ 之下也许还有
		 * 其他可以与本 OJ 交换的 OJ，但若无法与本 OJ 交换，
		 * 也就无法与那些 OJ 交换。）另外，若有
		 * 不安全的 PHVs，或当前连接是半连接或反连接，则无论
		 * 是否严格都必须保持顺序。
		 *
		 * 注意：我认为必须坚持对下层 OJ 的 min_righthand 里
		 * 至少一个关系严格，而不是对其整个 syn_righthand 严格。
		 *
		 * 当不需要保持顺序时，检查外连接
		 * 恒等式 3 是否适用；若适用，就从
		 * min_lefthand 里去掉下层 OJ 的 ojrelid，以便允许交换。
		 */
		if (bms_overlap(left_rels, otherinfo->syn_righthand))
		{
			if (bms_overlap(clause_relids, otherinfo->syn_righthand) &&
				(have_unsafe_phvs ||
				 jointype == JOIN_SEMI || jointype == JOIN_ANTI ||
				 !bms_overlap(strict_relids, otherinfo->min_righthand)))
			{
				/* 保持顺序 */
				min_lefthand = bms_add_members(min_lefthand,
											   otherinfo->syn_lefthand);
				min_lefthand = bms_add_members(min_lefthand,
											   otherinfo->syn_righthand);
				if (otherinfo->ojrelid != 0)
					min_lefthand = bms_add_member(min_lefthand,
												  otherinfo->ojrelid);
			}
			else if (jointype == JOIN_LEFT &&
					 otherinfo->jointype == JOIN_LEFT &&
					 bms_overlap(strict_relids, otherinfo->min_righthand) &&
					 !bms_overlap(clause_relids, otherinfo->syn_lefthand))
			{
				/* 恒等式 3 适用，因此去掉顺序限制 */
				min_lefthand = bms_del_member(min_lefthand, otherinfo->ojrelid);
				/* 记录（仍然暂定的）可交换关系 */
				commute_below_l =
					bms_add_member(commute_below_l, otherinfo->ojrelid);
			}
		}

		/*
		 * 对我们右侧的下层 OJ，若连接条件不用该下层
		 * 连接的右侧，且下层 OJ 的连接条件是严格的，就可以
		 * 对调这两个 OJ 的顺序；否则必须把
		 * 下层 OJ 的全部语法关系集加进 min_righthand。
		 *
		 * 另外，若连接条件也不用该下层连接的左侧，
		 * 就强制保持顺序。否则最后会得到
		 * min_righthands 相同的 SpecialJoinInfos，那会
		 * 把 join_is_legal 搞糊涂（见 backend/optimizer/README 里的讨论）。
		 *
		 * 另外，若有不安全的 PHVs，或本连接与下层 OJ 中
		 * 任一个是半连接或反连接，无论如何都必须保持顺序。
		 *
		 * 当不需要保持顺序时，检查外连接
		 * 恒等式 3 是否适用；若适用，就从
		 * min_righthand 里去掉下层 OJ 的 ojrelid，以便允许交换。
		 */
		if (bms_overlap(right_rels, otherinfo->syn_righthand))
		{
			if (bms_overlap(clause_relids, otherinfo->syn_righthand) ||
				!bms_overlap(clause_relids, otherinfo->min_lefthand) ||
				have_unsafe_phvs ||
				jointype == JOIN_SEMI ||
				jointype == JOIN_ANTI ||
				otherinfo->jointype == JOIN_SEMI ||
				otherinfo->jointype == JOIN_ANTI ||
				!otherinfo->lhs_strict)
			{
				/* 保持顺序 */
				min_righthand = bms_add_members(min_righthand,
												otherinfo->syn_lefthand);
				min_righthand = bms_add_members(min_righthand,
												otherinfo->syn_righthand);
				if (otherinfo->ojrelid != 0)
					min_righthand = bms_add_member(min_righthand,
												   otherinfo->ojrelid);
			}
			else if (jointype == JOIN_LEFT &&
					 otherinfo->jointype == JOIN_LEFT &&
					 otherinfo->lhs_strict)
			{
				/* 恒等式 3 适用，因此去掉顺序限制 */
				min_righthand = bms_del_member(min_righthand,
											   otherinfo->ojrelid);
				/* 记录（仍然暂定的）可交换关系 */
				commute_below_r =
					bms_add_member(commute_below_r, otherinfo->ojrelid);
			}
		}
	}

	/*
	 * 检查 PlaceHolderVars。若某个 PHV 应当在
	 * 本连接的可空侧内求值，就要确保 min_righthand 含有该
	 * PHV 的完整 eval_at 集合。这样才保证该 PHV 真的能在
	 * 右侧内求值。注意这能成立，只是因为对语法上位于
	 * 本连接之内的任何 PHV，应当已经
	 * 确定了最终的 eval_at 层次。
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);
		Relids		ph_syn_level = phinfo->ph_var->phrels;

		/* 若占位符在语法上不是来自右侧，就忽略 */
		if (!bms_is_subset(ph_syn_level, right_rels))
			continue;

		/* 否则，阻止在求值该 PHV 之前形成连接 */
		min_righthand = bms_add_members(min_righthand, phinfo->ph_eval_at);
	}

	/*
	 * 若 min_lefthand 里什么都没放进去，就放弃并把它设成完整
	 * 左侧，以免出现空的 min_lefthand，那会把后面的
	 * 处理搞糊涂。（这种情形不求聪明，只求正确。）
	 * min_righthand 同理。
	 */
	if (bms_is_empty(min_lefthand))
		min_lefthand = bms_copy(left_rels);
	if (bms_is_empty(min_righthand))
		min_righthand = bms_copy(right_rels);

	/* 现在它们最好都非空 */
	Assert(!bms_is_empty(min_lefthand));
	Assert(!bms_is_empty(min_righthand));
	/* 也不应当重叠 */
	Assert(!bms_overlap(min_lefthand, min_righthand));

	sjinfo->min_lefthand = min_lefthand;
	sjinfo->min_righthand = min_righthand;

	/*
	 * 既然已经确定了正确的 min_lefthand 和 min_righthand，
	 * 任何尚未被加回这些集合的 commute_below_l 或 commute_below_r relid
	 * （因为中间还有外连接）就确实
	 * 可以与本连接交换。
	 *
	 * 首先删掉后来又加回去的 relid（这比
	 * 在上面全过程里精确维护 commute_below_l/r 容易）。
	 */
	commute_below_l = bms_del_members(commute_below_l, min_lefthand);
	commute_below_r = bms_del_members(commute_below_r, min_righthand);

	/* 还剩什么吗？ */
	if (commute_below_l || commute_below_r)
	{
		/* 有，因此必须更新 SpecialJoinInfos 里推导出的数据 */
		sjinfo->commute_below_l = commute_below_l;
		sjinfo->commute_below_r = commute_below_r;
		foreach(l, root->join_info_list)
		{
			SpecialJoinInfo *otherinfo = (SpecialJoinInfo *) lfirst(l);

			if (bms_is_member(otherinfo->ojrelid, commute_below_l))
				otherinfo->commute_above_l =
					bms_add_member(otherinfo->commute_above_l, ojrelid);
			else if (bms_is_member(otherinfo->ojrelid, commute_below_r))
				otherinfo->commute_above_r =
					bms_add_member(otherinfo->commute_above_r, ojrelid);
		}
	}

	return sjinfo;
}

/*
 * compute_semijoin_info
 *	  填写新 SpecialJoinInfo 中与半连接相关的字段
 *
 * 注意：这只依赖该 SpecialJoinInfo 的 jointype 和 syn_righthand 字段；
 * 其余字段也许还没设置。
 */
static void
compute_semijoin_info(PlannerInfo *root, SpecialJoinInfo *sjinfo, List *clause)
{
	List	   *semi_operators;
	List	   *semi_rhs_exprs;
	bool		all_btree;
	bool		all_hash;
	ListCell   *lc;

	/* 先初始化半连接相关字段，以防无法做唯一化 */
	sjinfo->semi_can_btree = false;
	sjinfo->semi_can_hash = false;
	sjinfo->semi_operators = NIL;
	sjinfo->semi_rhs_exprs = NIL;

	/* 若不是半连接，就没有更多要做的 */
	if (sjinfo->jointype != JOIN_SEMI)
		return;

	/*
	 * 看看该半连接的连接限定条件是否由 AND 起来的
	 * 等值操作符组成，且每个操作符只有一侧（仅）出现右侧变量。
	 * 若是，就能弄清如何保证右侧的唯一性。
	 *
	 * 注意输入子句列表是在语法上
	 * 与该半连接相关联的限定条件，实践中就是
	 * IN 的合成比较列表，或 EXISTS 的 WHERE。
	 * 尤其是后一种情况，其中也许含有在语义上并不
	 * 与该连接相关联、而只引用某一侧的子句。
	 * 这里可以忽略这类子句，因为它们只会
	 * 落到某一侧内部去处理。（这里只考虑
	 * 语法上相关联的子句是可以的，因为对
	 * 半连接而言，更高层的限定条件无法引用右侧，因此不存在
	 * 其他在语义上与本连接相关联的限定条件。
	 * 这样做是因为，在能够抽出真正在语义上
	 * 与该特定连接相关联的限定条件列表之前，先有一套潜在的
	 * 唯一化表达式是有用的。）
	 *
	 * 注意 semi_operators 列表由这些连接限定操作符
	 * 本身组成（必要时会交换，把右侧值放到右边）。
	 * 它们可能是跨类型操作符，此时唯一性实际
	 * 需要的是一个相关的单类型操作符。这里
	 * 假定到时候该操作符可以从 btree 或哈希
	 * opclass 得到……若得不到，create_unique_plan() 会失败。
	 */
	semi_operators = NIL;
	semi_rhs_exprs = NIL;
	all_btree = true;
	all_hash = enable_hashagg;	/* 若未启用哈希，就不考虑哈希 */
	foreach(lc, clause)
	{
		OpExpr	   *op = (OpExpr *) lfirst(lc);
		Oid			opno;
		Node	   *left_expr;
		Node	   *right_expr;
		Relids		left_varnos;
		Relids		right_varnos;
		Relids		all_varnos;
		Oid			opinputtype;

		/* 它是二元操作子句吗？ */
		if (!IsA(op, OpExpr) ||
			list_length(op->args) != 2)
		{
			/* 不是，但它是否引用了两侧？ */
			all_varnos = pull_varnos(root, (Node *) op);
			if (!bms_overlap(all_varnos, sjinfo->syn_righthand) ||
				bms_is_subset(all_varnos, sjinfo->syn_righthand))
			{
				/*
				 * 该子句只引用一个关系，因此忽略它——除非它
				 * 含有易变函数，那种情况下最好
				 * 放弃。
				 */
				if (contain_volatile_functions((Node *) op))
					return;
				continue;
			}
			/* 引用两侧的非操作符子句，必须放弃 */
			return;
		}

		/* 从二元操作子句里取出数据 */
		opno = op->opno;
		left_expr = linitial(op->args);
		right_expr = lsecond(op->args);
		left_varnos = pull_varnos(root, left_expr);
		right_varnos = pull_varnos(root, right_expr);
		all_varnos = bms_union(left_varnos, right_varnos);
		opinputtype = exprType(left_expr);

		/* 它是否引用了两侧？ */
		if (!bms_overlap(all_varnos, sjinfo->syn_righthand) ||
			bms_is_subset(all_varnos, sjinfo->syn_righthand))
		{
			/*
			 * 该子句只引用一个关系，因此忽略它——除非它
			 * 含有易变函数，那种情况下最好放弃。
			 */
			if (contain_volatile_functions((Node *) op))
				return;
			continue;
		}

		/* 检查参数的关系归属 */
		if (!bms_is_empty(right_varnos) &&
			bms_is_subset(right_varnos, sjinfo->syn_righthand) &&
			!bms_overlap(left_varnos, sjinfo->syn_righthand))
		{
			/* 典型情形，right_expr 是右侧变量 */
		}
		else if (!bms_is_empty(left_varnos) &&
				 bms_is_subset(left_varnos, sjinfo->syn_righthand) &&
				 !bms_overlap(right_varnos, sjinfo->syn_righthand))
		{
			/* 对调情形，left_expr 是右侧变量 */
			opno = get_commutator(opno);
			if (!OidIsValid(opno))
				return;
			right_expr = left_expr;
		}
		else
		{
			/* 参数归属混杂，放弃 */
			return;
		}

		/* 全部操作符都必须是 btree 等值或哈希等值 */
		if (all_btree)
		{
			/* oprcanmerge 只当作提示…… */
			if (!op_mergejoinable(opno, opinputtype) ||
				get_mergejoin_opfamilies(opno) == NIL)
				all_btree = false;
		}
		if (all_hash)
		{
			/* ……但 oprcanhash 最好是对的 */
			if (!op_hashjoinable(opno, opinputtype))
				all_hash = false;
		}
		if (!(all_btree || all_hash))
			return;

		/* 到目前为止没问题，继续建列表 */
		semi_operators = lappend_oid(semi_operators, opno);
		semi_rhs_exprs = lappend(semi_rhs_exprs, copyObject(right_expr));
	}

	/* 若没找到至少一列来做唯一化，就放弃 */
	if (semi_rhs_exprs == NIL)
		return;

	/*
	 * 用来做唯一化的表达式不得是易变的。
	 */
	if (contain_volatile_functions((Node *) semi_rhs_exprs))
		return;

	/*
	 * 若走到这里，就可以用排序和哈希中的至少一种
	 * 对该半连接的右侧做唯一化。保存如何做到这一点的信息。
	 */
	sjinfo->semi_can_btree = all_btree;
	sjinfo->semi_can_hash = all_hash;
	sjinfo->semi_operators = semi_operators;
	sjinfo->semi_rhs_exprs = semi_rhs_exprs;
}

/*
 * deconstruct_distribute_oj_quals
 *	  调整 LEFT JOIN 限定条件，使之适合左连接被交换的情形，
 *	  然后推进连接限定列表和 EquivalenceClass 结构。
 *
 * 这紧接在完成 deconstruct_distribute 扫描之后运行。
 * jtitems 含有全部 JoinTreeItems（深度优先顺序），jtitem
 * 是其中一个，它有推迟的 oj_joinclauses 要处理。
 */
static void
deconstruct_distribute_oj_quals(PlannerInfo *root,
								List *jtitems,
								JoinTreeItem *jtitem)
{
	SpecialJoinInfo *sjinfo = jtitem->sjinfo;
	Relids		qualscope,
				ojscope,
				nonnullable_rels;

	/* 重新计算这个左连接的语法范围和语义范围 */
	qualscope = bms_union(sjinfo->syn_lefthand, sjinfo->syn_righthand);
	qualscope = bms_add_member(qualscope, sjinfo->ojrelid);
	ojscope = bms_union(sjinfo->min_lefthand, sjinfo->min_righthand);
	nonnullable_rels = sjinfo->syn_lefthand;

	/*
	 * 若本连接能按外连接恒等式 3 与其他连接交换，
	 * 并且提供灵活语义连接子句的是它，
	 * 就必须生成带有不同
	 * nullingrels 标注的连接子句变体。否则就把推迟的子句
	 * 原样推出去。
	 */
	Assert(sjinfo->lhs_strict); /* 否则不应当走到这里 */
	if (sjinfo->commute_above_r || sjinfo->commute_below_l)
	{
		Relids		joins_above;
		Relids		joins_below;
		Relids		incompatible_joins;
		Relids		joins_so_far;
		List	   *quals;
		int			save_last_rinfo_serial;
		ListCell   *lc;

		/* 识别本连接可以与之交换的外连接 */
		joins_above = sjinfo->commute_above_r;
		joins_below = sjinfo->commute_below_l;

		/*
		 * 生成带有不同 nullingrels 位集合的限定变体。
		 *
		 * 只需要那些对应该连接及其
		 * 可交换者在语法上逐层变浅的嵌套子集的位集合。
		 * 首先，显然只有这些形式的
		 * Var 和 PHVs 才可能出现在查询的别处；
		 * 其次，外连接恒等式并不提供一种
		 * 重排这类连接、从而需要不同标注的办法。
		 * （也就是说，当前连接也许能与另外几个交换，
		 * 但那些连接彼此之间不能交换。）要按语法嵌套顺序
		 * 访问这些有意思的连接，就依赖
		 * jtitems 列表已经按那种顺序排好。
		 *
		 * 先去掉对应本连接之下那些
		 * 可交换连接的全部 nullingrels 位，然后沿连接栈向上爬时
		 * 再逐个把它们放回去。
		 */
		quals = jtitem->oj_joinclauses;
		if (!bms_is_empty(joins_below))
			quals = (List *) remove_nulling_relids((Node *) quals,
												   joins_below,
												   NULL);

		/*
		 * 需要把限定条件的下层版本标成不能安全地
		 * 用到栈上尚未处理的连接之上。这可以防止
		 * 可能在错误的连接层应用克隆限定条件。
		 */
		incompatible_joins = bms_union(joins_below, joins_above);
		incompatible_joins = bms_add_member(incompatible_joins,
											sjinfo->ojrelid);

		/*
		 * 每次从这些限定条件产生 RestrictInfos 时，都重置
		 * last_rinfo_serial 计数器，使 "同一"
		 * 限定条件的 RestrictInfos 得到相同的序号。（这依赖
		 * 我们没有以任何会影响
		 * 由此建立的 RestrictInfos 个数的方式改动限定列表。）这样以后就能
		 * 检测重复使用的限定条件。
		 */
		save_last_rinfo_serial = root->last_rinfo_serial;

		joins_so_far = NULL;
		foreach(lc, jtitems)
		{
			JoinTreeItem *otherjtitem = (JoinTreeItem *) lfirst(lc);
			SpecialJoinInfo *othersj = otherjtitem->sjinfo;
			bool		below_sjinfo = false;
			bool		above_sjinfo = false;
			Relids		this_qualscope;
			Relids		this_ojscope;
			bool		allow_equivalence,
						has_clone,
						is_clone;

			if (othersj == NULL)
				continue;		/* 不是外连接项，忽略 */

			if (bms_is_member(othersj->ojrelid, joins_below))
			{
				/* othersj 从左侧下方与 sjinfo 可交换 */
				below_sjinfo = true;
			}
			else if (othersj == sjinfo)
			{
				/* 按语法顺序找到了我们的连接 */
				Assert(bms_equal(joins_so_far, joins_below));
			}
			else if (bms_is_member(othersj->ojrelid, joins_above))
			{
				/* othersj 从上方与 sjinfo 可交换 */
				above_sjinfo = true;
			}
			else
			{
				/* othersj 无关，忽略 */
				continue;
			}

			/* 为这一版限定条件重置序号计数器 */
			root->last_rinfo_serial = save_last_rinfo_serial;

			/*
			 * 当查看 sjinfo 之上的连接时，设想的是
			 * 把 sjinfo 推到 othersj 之上，因此在分发限定条件之前
			 * 先加上 othersj 的置空位。应当把它加到来自
			 * 当前连接左侧的 Var 上：要把 OJ 恒等式 3 的第二种
			 * 形式变成第一种形式，其中关系 B 的 Var 会在
			 * Pbc 子句里表现为被语法上更上层的 OJ
			 * 置空，而关系 C 的则不会。（用
			 * optimizer/README 的记号，就是把形如
			 * Pbc 的限定条件转换成 Pb*c。）当然，也必须从
			 * incompatible_joins 的值里去掉该
			 * 位，否则会做出一条哪里都放不了的限定条件。
			 */
			if (above_sjinfo)
			{
				quals = (List *)
					add_nulling_relids((Node *) quals,
									   sjinfo->syn_lefthand,
									   bms_make_singleton(othersj->ojrelid));
				incompatible_joins = bms_del_member(incompatible_joins,
													othersj->ojrelid);
			}

			/* 计算本连接层的 qualscope 和 ojscope */
			this_qualscope = bms_union(qualscope, joins_so_far);
			this_ojscope = bms_union(ojscope, joins_so_far);
			if (above_sjinfo)
			{
				/* othersj 还不在 joins_so_far 里，但需要它 */
				this_qualscope = bms_add_member(this_qualscope,
												othersj->ojrelid);
				this_ojscope = bms_add_member(this_ojscope,
											  othersj->ojrelid);
				/* sjinfo 在 joins_so_far 里，而且不想要它 */
				this_ojscope = bms_del_member(this_ojscope,
											  sjinfo->ojrelid);
			}

			/*
			 * 只从第一种形式的
			 * 限定条件生成 EquivalenceClasses，也就是置上的 nullingrels 位最少的那种。
			 * 由这一版限定条件做成的等价类在外连接
			 * 嵌套之下会有用，而置了一些 nullingrels 位的版本则
			 * 不会。无法从多于一个版本生成等价类，否则
			 * 会得出荒谬的结论，认为置了 nullingrels
			 * 位的 Var 等于没有那些位的版本。幸好
			 * 这类等价类无论如何也不太有用，因为它们把
			 * 连接嵌套之外观察不到的值等同起来。（见
			 * optimizer/README。）
			 *
			 * 限定条件的第一种形式也是唯一被标成
			 * has_clone 而不是 is_clone 的那种。
			 */
			allow_equivalence = (joins_so_far == NULL);
			has_clone = allow_equivalence;
			is_clone = !has_clone;

			distribute_quals_to_rels(root, quals,
									 otherjtitem,
									 sjinfo,
									 root->qual_security_level,
									 this_qualscope,
									 this_ojscope, nonnullable_rels,
									 bms_copy(incompatible_joins),
									 allow_equivalence,
									 has_clone,
									 is_clone,
									 NULL); /* 不再推迟 */

			/*
			 * 若需要，为再上一层调整限定条件的置空位。完全
			 * 不想放入 sjinfo 自己的位，而且若已经在
			 * sjinfo 之上，那一步已经做过了。这里应当标记全部
			 * 来自下层连接右侧的 Var。（同样，是在把形如
			 * Pbc 的限定条件转换成 Pb*c，但现在是把
			 * 解析器输出里原有、并在上面被临时
			 * 剥掉的位放回去。）也更新 incompatible_joins。
			 */
			if (below_sjinfo)
			{
				quals = (List *)
					add_nulling_relids((Node *) quals,
									   othersj->syn_righthand,
									   bms_make_singleton(othersj->ojrelid));
				incompatible_joins = bms_del_member(incompatible_joins,
													othersj->ojrelid);
			}

			/* ……并跟踪到目前为止处理过的连接 */
			joins_so_far = bms_add_member(joins_so_far, othersj->ojrelid);
		}
	}
	else
	{
		/* 不可能交换，就只处理被推迟的子句 */
		distribute_quals_to_rels(root, jtitem->oj_joinclauses,
								 jtitem,
								 sjinfo,
								 root->qual_security_level,
								 qualscope,
								 ojscope, nonnullable_rels,
								 NULL,	/* incompatible_relids（不相容的 relid） */
								 true,	/* allow_equivalence（允许等价） */
								 false, false,	/* 不是克隆 */
								 NULL); /* 不再推迟 */
	}
}


/*****************************************************************************
 *
 *	  QUALIFICATIONS（限定条件）
 *
 *****************************************************************************/

/*
 * distribute_quals_to_rels
 *	  方便例程：对一份 AND 起来的子句列表的每个元素
 *	  应用 distribute_qual_to_rels。
 */
static void
distribute_quals_to_rels(PlannerInfo *root, List *clauses,
						 JoinTreeItem *jtitem,
						 SpecialJoinInfo *sjinfo,
						 Index security_level,
						 Relids qualscope,
						 Relids ojscope,
						 Relids outerjoin_nonnullable,
						 Relids incompatible_relids,
						 bool allow_equivalence,
						 bool has_clone,
						 bool is_clone,
						 List **postponed_oj_qual_list)
{
	ListCell   *lc;

	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);

		distribute_qual_to_rels(root, clause,
								jtitem,
								sjinfo,
								security_level,
								qualscope,
								ojscope,
								outerjoin_nonnullable,
								incompatible_relids,
								allow_equivalence,
								has_clone,
								is_clone,
								postponed_oj_qual_list);
	}
}

/*
 * distribute_qual_to_rels
 *	  把子句信息加进该子句提到的每个基关系的
 *	  baserestrictinfo 或 joininfo 列表
 *	  （取决于该子句是不是连接）。为每个关系建立 RestrictInfo 节点并加进
 *	  相应列表。或者，若该子句使用
 *	  可归并连接的操作符，就把其左右侧表达式送进
 *	  查询的 EquivalenceClasses。
 *
 * 有些情况下，限定条件会被加进父 jtitem 的 lateral_clauses，
 * 或加进 postponed_oj_qual_list，而不是马上处理。
 * 这些会在以后的 deconstruct_distribute 调用里处理。
 *
 * 'clause'：要分发的限定子句
 * 'jtitem'：包含它的连接树节点的 JoinTreeItem
 * 'sjinfo'：连接的 SpecialJoinInfo（内连接或 WHERE 子句则为 NULL）
 * 'security_level'：要赋给该限定条件的 security_level
 * 'qualscope'：该限定条件语法范围覆盖的 base+OJ 关系集合
 * 'ojscope'：若不是外连接限定条件则为 NULL，否则是形成本连接
 *		所需的最小 base+OJ 关系集合
 * 'outerjoin_nonnullable'：若不是外连接限定条件则为 NULL，否则是
 *		出现在连接外侧（不可空侧）的 base+OJ 关系集合
 *		（对 FULL JOIN，这包括连接的两侧，并且事实上必须
 *		等于 qualscope）
 * 'incompatible_relids'：不得在本限定条件之下
 *		计算的外连接 relid 集合。只对
 *		"clone" 限定条件费心计算它，否则可以留 NULL。
 * 'allow_equivalence'：若可以把该子句转换成一个
 *		EquivalenceClass 则为真
 * 'has_clone'：要赋给该限定条件的 has_clone 属性
 * 'is_clone'：要赋给该限定条件的 is_clone 属性
 * 'postponed_oj_qual_list'：若不为 NULL，非退化的外连接子句
 *		应当加进这个列表，而不是被处理（列表项
 *		就是裸子句）
 *
 * 'qualscope' 标明该限定条件在语法上来自哪一层 JOIN。
 * 'ojscope' 在决定把该限定条件强制提升到外连接
 * 层时需要，那一层是 ojscope，不一定是 qualscope。
 *
 * 调用时，root->join_info_list 必须至少含有
 * 语法上位于本限定条件之下的那些特殊连接的项。
 * （现在只有检测冗余的 IS NULL 限定条件才需要它。）
 */
static void
distribute_qual_to_rels(PlannerInfo *root, Node *clause,
						JoinTreeItem *jtitem,
						SpecialJoinInfo *sjinfo,
						Index security_level,
						Relids qualscope,
						Relids ojscope,
						Relids outerjoin_nonnullable,
						Relids incompatible_relids,
						bool allow_equivalence,
						bool has_clone,
						bool is_clone,
						List **postponed_oj_qual_list)
{
	Relids		relids;
	bool		is_pushed_down;
	bool		pseudoconstant = false;
	bool		maybe_equivalence;
	bool		maybe_outer_join;
	RestrictInfo *restrictinfo;

	/*
	 * 取出该子句里提到的全部 relid。
	 */
	relids = pull_varnos(root, clause);

	/*
	 * 在普通 SQL 里，WHERE 或 JOIN/ON 子句不能引用任何
	 * 不在其语法范围之内的关系；但如果上拉了一个
	 * LATERAL 子查询，就可能在已被上拉的限定条件里
	 * 发现这类引用。需要把这类限定条件当作属于那个
	 * 包含它们所引用的每个关系的连接
	 * 层。虽然可以让
	 * pull_up_subqueries() 一开始就把这类限定条件放对位置，但
	 * 在这里处理更容易。发现某条子句含有
	 * 其语法范围之外的 Var 时，找到包含全部
	 * 所需关系的最近父连接层，并把该子句加进那一层的 lateral_clauses 列表。到达该连接层时再处理它。
	 */
	if (!bms_is_subset(relids, qualscope))
	{
		JoinTreeItem *pitem;

		Assert(root->hasLateralRTEs);	/* 否则不应当发生 */
		Assert(sjinfo == NULL); /* 不得推迟到外连接之后 */
		for (pitem = jtitem->jti_parent; pitem; pitem = pitem->jti_parent)
		{
			if (bms_is_subset(relids, pitem->qualscope))
			{
				pitem->lateral_clauses = lappend(pitem->lateral_clauses,
												 clause);
				return;
			}

			/*
			 * 不应当把任何限定条件推迟到外连接之后。若
			 * 这条断言触发，就是 pull_up_subqueries() 搞错了。
			 */
			Assert(pitem->sjinfo == NULL);
		}
		elog(ERROR, "failed to postpone qual containing lateral reference");
	}

	/*
	 * 若是外连接子句，还要检查 relids 是
	 * ojscope 的子集。（若语法范围检查已通过，这里不应当失败。）
	 */
	if (ojscope && !bms_is_subset(relids, ojscope))
		elog(ERROR, "JOIN qualification cannot refer to other relations");

	/*
	 * 若该子句不含变量，通常把它
	 * 下推到所提到的关系的启发式就不起作用，因为一个关系都没有。
	 *
	 * 若该子句是外连接子句，必须把它强制到该 OJ 的
	 * 语义层，以保持语义。
	 *
	 * 否则，当子句含有易变函数时，就强制它在
	 * 原来的语法层求值。这保持
	 * 预期语义。
	 *
	 * 当子句也不含易变函数时，它实际上是一条
	 * 伪常量子句，在计划的任何一次
	 * 执行期间都不会改变值，因此可以在一个
	 * 门控 Result 计划节点里用作一次性限定条件。暂时把这类子句放进常规的
	 * RestrictInfo 列表，但最终 createplan.c 会
	 * 把它抽出来，并在该伪常量子句被分配到的
	 * 计划节点正上方做一个门控 Result 节点。通常最好
	 * 把门控节点放在计划树里尽可能高的位置。
	 */
	if (bms_is_empty(relids))
	{
		if (ojscope)
		{
			/* 子句挂在外连接上，就在那里求值 */
			relids = bms_copy(ojscope);
			/* 不得用作门控限定条件，因此不要标成伪常量 */
		}
		else if (contain_volatile_functions(clause))
		{
			/* 在原来的语法层求值 */
			relids = bms_copy(qualscope);
			/* 同样，不能标成伪常量 */
		}
		else
		{
			/*
			 * 若处于顶层连接域，可以把该限定条件推到
			 * 计划树顶部。否则就保守一点，在
			 * 原来的语法层求值。（理想情况下各种情形都把它推到
			 * 当前连接域的顶部，但若以后重排外连接的求值顺序，
			 * 那会出
			 * 问题。顶层之下的伪常量限定条件是相当古怪的情形，
			 * 因此不清楚是否值得下大力气。）
			 */
			if (jtitem->jdomain == (JoinDomain *) linitial(root->join_domains))
				relids = bms_copy(jtitem->jdomain->jd_relids);
			else
				relids = bms_copy(qualscope);
			/* 标成门控限定条件 */
			pseudoconstant = true;
			/* 告诉 createplan.c 去检查门控限定条件 */
			root->hasPseudoConstantQuals = true;
		}
	}

	/*----------
	 * Check to see if clause application must be delayed by outer-join
	 * considerations.
	 *
	 * A word about is_pushed_down: we mark the qual as "pushed down" if
	 * it is (potentially) applicable at a level different from its original
	 * syntactic level.  This flag is used to distinguish OUTER JOIN ON quals
	 * from other quals pushed down to the same joinrel.  The rules are:
	 *		WHERE quals and INNER JOIN quals: is_pushed_down = true.
	 *		Non-degenerate OUTER JOIN quals: is_pushed_down = false.
	 *		Degenerate OUTER JOIN quals: is_pushed_down = true.
	 * A "degenerate" OUTER JOIN qual is one that doesn't mention the
	 * non-nullable side, and hence can be pushed down into the nullable side
	 * without changing the join result.  It is correct to treat it as a
	 * regular filter condition at the level where it is evaluated.
	 *
	 * Note: it is not immediately obvious that a simple boolean is enough
	 * for this: if for some reason we were to attach a degenerate qual to
	 * its original join level, it would need to be treated as an outer join
	 * qual there.  However, this cannot happen, because all the rels the
	 * clause mentions must be in the outer join's min_righthand, therefore
	 * the join it needs must be formed before the outer join; and we always
	 * attach quals to the lowest level where they can be evaluated.  But
	 * if we were ever to re-introduce a mechanism for delaying evaluation
	 * of "expensive" quals, this area would need work.
	 *
	 * Note: generally, use of is_pushed_down has to go through the macro
	 * RINFO_IS_PUSHED_DOWN, because that flag alone is not always sufficient
	 * to tell whether a clause must be treated as pushed-down in context.
	 * This seems like another reason why it should perhaps be rethought.
	 *----------
	 */
	if (bms_overlap(relids, outerjoin_nonnullable))
	{
		/*
		 * 该限定条件挂在外连接上，并提到不可空侧的
		 * （某些）关系，因此不是退化的。若
		 * 调用方想推迟处理这类子句，就把它加进
		 * postponed_oj_qual_list 并返回。（到这里为止做的工作
		 * 以后得重做，但量不大。）
		 */
		if (postponed_oj_qual_list != NULL)
		{
			*postponed_oj_qual_list = lappend(*postponed_oj_qual_list, clause);
			return;
		}

		/*
		 * 不能用这类子句推导等价（连接之上左右
		 * 两侧也许不相等，因为其中一侧已经
		 * 变成了 NULL）……但若它可归并连接，也许还能用来做更有限的
		 * 推导。因此考虑把它加进
		 * 搁置的外连接子句列表。
		 */
		is_pushed_down = false;
		maybe_equivalence = false;
		maybe_outer_join = true;

		/*
		 * 现在强制该限定条件恰好在对应此外连接的
		 * 连接层求值。无法让它被下推
		 * 到不可空侧，否则对任何
		 * 通不过该限定条件的不可空侧行，产生的会是零行输出，
		 * 而不是预期的那一行空扩展行。
		 */
		Assert(ojscope);
		relids = ojscope;
		Assert(!pseudoconstant);
	}
	else
	{
		/*
		 * 普通限定子句或退化的外连接子句。无论哪种，都可以
		 * 把它标成已下推。
		 */
		is_pushed_down = true;

		/*
		 * 这条 IS NULL 子句有可能与
		 * 更下层的反连接冗余；若是，直接丢掉即可。其他情形不必测试，
		 * 因为只有
		 * 已下推的子句才可能这样。
		 */
		if (check_redundant_nullability_qual(root, clause))
			return;

		/* 若调用方允许，就把限定条件送进等价机制 */
		maybe_equivalence = allow_equivalence;

		/*
		 * 既然它没提到左侧，即使它在某个 OJ 里，也肯定不能当作
		 * 搁置的 OJ 子句来用。
		 */
		maybe_outer_join = false;
	}

	/*
	 * 建立 RestrictInfo 节点本身。
	 */
	restrictinfo = make_restrictinfo(root,
									 (Expr *) clause,
									 is_pushed_down,
									 has_clone,
									 is_clone,
									 pseudoconstant,
									 security_level,
									 relids,
									 incompatible_relids,
									 outerjoin_nonnullable);

	/*
	 * 若是连接子句，就把子句里用到的 var 加进其关系的目标列表，
	 * 以便扫描那些关系的计划节点会把它们发出去
	 * （否则到连接节点时它们不可用！）。
	 *
	 * 通常把这些 var 标成在 "relids" 所标识的连接上所需。
	 * 不过，若这是克隆子句，就忽略该集合里的外连接 relid。
	 * 否则，出现在克隆子句里的 var 最后会被
	 * 标成必须传播到那些可交换
	 * 连接中最高的一个，那常常是高估。对这类子句，正确的
	 * var 传播靠让 ojscope 包含连接
	 * 两侧的输入关系来保证。
	 *
	 * 注意：若该子句被吸收进某个 EquivalenceClass，这一步
	 * 也许多余，但目前还是得做，以覆盖
	 * 该等价类变成 ec_broken、于是把原来的
	 * 子句重新插回计划的情形。
	 */
	if (bms_membership(relids) == BMS_MULTIPLE)
	{
		List	   *vars = pull_var_clause(clause,
										   PVC_RECURSE_AGGREGATES |
										   PVC_RECURSE_WINDOWFUNCS |
										   PVC_INCLUDE_PLACEHOLDERS);
		Relids		where_needed;

		if (is_clone)
			where_needed = bms_intersect(relids, root->all_baserels);
		else
			where_needed = relids;
		add_vars_to_targetlist(root, vars, where_needed);
		list_free(vars);
	}

	/*
	 * 对每条子句都检查 "可归并连接性"，不只是连接子句，
	 * 因为想知道同一
	 * 关系的 var 之间、或 var 与常量之间的等价。
	 */
	check_mergejoinable(restrictinfo);

	/*
	 * 若它是真正的等价子句，就送给 EquivalenceClass
	 * 机制。并不把它直接挂到任何限制列表或连接
	 * 列表上。等价类代码以后会把它传播到适当的地方。
	 *
	 * 若该子句有可归并连接的操作符，却因为是外连接子句而不是等价，
	 * 等价类代码也许仍能对它
	 * 做点什么。把它加进相应列表，供以后
	 * 进一步考虑。具体来说：
	 *
	 * 若它是把外连接两侧联系起来的左或右外连接限定
	 * （没有 leftvar1 = leftvar2 +
	 * rightvar 这类花样），就按不可空
	 * 变量出现在哪一侧，把它加进 root->left_join_clauses 或
	 * root->right_join_clauses。
	 *
	 * 若它是全外连接限定，就把它加进
	 * root->full_join_clauses。（理想情况下会像左右连接那样丢掉不是
	 * leftvar = rightvar 的情形，但本例程
	 * 没有做这件事所需的信息；而 full_join_clauses 列表目前的用法
	 * 并不需要那样，所以目前
	 * 不值得把本例程的 API 弄复杂来使那成为可能。）
	 *
	 * 若以上都不成立，就把它交给
	 * distribute_restrictinfo_to_rels()。
	 *
	 * 无论哪种情况，初始化可归并连接子句的 left_ec 和 right_ec
	 * 字段都很重要，这样所有可能可归并连接的
	 * 表达式在 EquivalenceClasses 里都有表示。若
	 * process_equivalence 成功，它会负责这件事；
	 * 否则就得调用 initialize_mergeclause_eclasses 来做。
	 */
	if (restrictinfo->mergeopfamilies)
	{
		if (maybe_equivalence)
		{
			if (process_equivalence(root, &restrictinfo, jtitem->jdomain))
				return;
			/* 等价类拒绝了它，因此用笨办法设置 left_ec/right_ec…… */
			if (restrictinfo->mergeopfamilies)	/* 等价类也许已经改过这个 */
				initialize_mergeclause_eclasses(root, restrictinfo);
			/* ... and fall through to distribute_restrictinfo_to_rels */
		}
		else if (maybe_outer_join && restrictinfo->can_join)
		{
			/* 仍需要用笨办法设置 left_ec/right_ec */
			initialize_mergeclause_eclasses(root, restrictinfo);
			/* 现在看它是否应当进入任何外连接列表 */
			Assert(sjinfo != NULL);
			if (bms_is_subset(restrictinfo->left_relids,
							  outerjoin_nonnullable) &&
				!bms_overlap(restrictinfo->right_relids,
							 outerjoin_nonnullable))
			{
				/* 得到的是 outervar = innervar */
				OuterJoinClauseInfo *ojcinfo = makeNode(OuterJoinClauseInfo);

				ojcinfo->rinfo = restrictinfo;
				ojcinfo->sjinfo = sjinfo;
				root->left_join_clauses = lappend(root->left_join_clauses,
												  ojcinfo);
				return;
			}
			if (bms_is_subset(restrictinfo->right_relids,
							  outerjoin_nonnullable) &&
				!bms_overlap(restrictinfo->left_relids,
							 outerjoin_nonnullable))
			{
				/* 得到的是 innervar = outervar */
				OuterJoinClauseInfo *ojcinfo = makeNode(OuterJoinClauseInfo);

				ojcinfo->rinfo = restrictinfo;
				ojcinfo->sjinfo = sjinfo;
				root->right_join_clauses = lappend(root->right_join_clauses,
												   ojcinfo);
				return;
			}
			if (sjinfo->jointype == JOIN_FULL)
			{
				/* FULL JOIN（上面的测试在这种情况下无法匹配） */
				OuterJoinClauseInfo *ojcinfo = makeNode(OuterJoinClauseInfo);

				ojcinfo->rinfo = restrictinfo;
				ojcinfo->sjinfo = sjinfo;
				root->full_join_clauses = lappend(root->full_join_clauses,
												  ojcinfo);
				return;
			}
			/* nope, so fall through to distribute_restrictinfo_to_rels */
		}
		else
		{
			/* 仍需要设置 left_ec/right_ec */
			initialize_mergeclause_eclasses(root, restrictinfo);
		}
	}

	/* 没有等价类特例适用，因此推进子句列表 */
	distribute_restrictinfo_to_rels(root, restrictinfo);
}

/*
 * check_redundant_nullability_qual
 *	  检查该限定条件是否为与更下层
 *	  JOIN_ANTI 连接冗余的 IS NULL 限定条件。
 *
 * 想抑制冗余的 IS NULL 限定条件，与其说是为了省周期，
 * 不如说是为了避免给它们生成虚假的选择率估计。因此若
 * 在这里检测到冗余，distribute_qual_to_rels() 就直接丢掉
 * 该限定条件。
 */
static bool
check_redundant_nullability_qual(PlannerInfo *root, Node *clause)
{
	Var		   *forced_null_var;
	ListCell   *lc;

	/* 检查是否为 IS NULL，并识别被强制为 NULL 的 Var */
	forced_null_var = find_forced_null_var(clause);
	if (forced_null_var == NULL)
		return false;

	/*
	 * 若该 Var 来自更下层反连接的可空侧，这条 IS
	 * NULL 条件必然为真。若它没被任何东西置空，
	 * 就没有必要搜索 join_info_list。否则，需要
	 * 弄清置空它的关系是不是反连接。
	 */
	if (forced_null_var->varnullingrels == NULL)
		return false;

	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		/*
		 * 若 sjinfo->ojrelid 为零，这个测试不会成功；由半连接
		 * 转换来的反连接就可能如此；但那种情况下该 Var
		 * 不可能来自它的可空侧。
		 */
		if (sjinfo->jointype == JOIN_ANTI && sjinfo->ojrelid != 0 &&
			bms_is_member(sjinfo->ojrelid, forced_null_var->varnullingrels))
			return true;
	}

	return false;
}

/*
 * distribute_restrictinfo_to_rels
 *	  把一份完成的 RestrictInfo 推进适当的限制子句或连接
 *	  子句列表。
 *
 * 这是普通限定
 * 子句在 distribute_qual_to_rels() 里的最后一步。对等价类处理有意思的
 * 子句会转给等价类机制，但最终也许会再送回这里。
 */
void
distribute_restrictinfo_to_rels(PlannerInfo *root,
								RestrictInfo *restrictinfo)
{
	Relids		relids = restrictinfo->required_relids;
	RelOptInfo *rel;

	switch (bms_membership(relids))
	{
		case BMS_SINGLETON:

			/*
			 * 参与该子句的关系只有一个，因此它
			 * 是该关系的限制子句。
			 */
			rel = find_base_rel(root, bms_singleton_member(relids));

			/* 把子句加进关系的限制列表 */
			rel->baserestrictinfo = lappend(rel->baserestrictinfo,
											restrictinfo);
			/* 更新安全级别信息 */
			rel->baserestrict_min_security = Min(rel->baserestrict_min_security,
												 restrictinfo->security_level);
			break;
		case BMS_MULTIPLE:

			/*
			 * 该子句是连接子句，因为它的 relid 集合里
			 * 有多于一个关系。
			 */

			/*
			 * 检查可哈希连接的操作符。（除了真正的连接子句，
			 * 不费心设置哈希连接信息。）
			 */
			check_hashjoinable(restrictinfo);

			/*
			 * 同样，检查该子句是否适合配合 Memoize
			 * 节点，在参数化
			 * 嵌套循环期间缓存内侧元组。
			 */
			check_memoizable(restrictinfo);

			/*
			 * 把子句加进全部相关关系的连接列表。
			 */
			add_join_clause_to_rels(root, restrictinfo, relids);
			break;
		default:

			/*
			 * 该子句不引用任何关系，因此没有地方
			 * 挂它。若调用方工作正常，不应当走到这里。
			 */
			elog(ERROR, "cannot cope with variable-free clause");
			break;
	}
}

/*
 * process_implied_equality
 *	  建立一条表示 "item1 op item2" 的 restrictinfo 项，并把它推进
 *	  适当的列表。（实践中 opno 总是 btree
 *	  等值操作符。）
 *
 * "qualscope" 是要归于该 restrictinfo 的名义语法层。
 * 它必须至少包含表达式里用到的全部关系，但只在两个表达式都
 * 不含变量时，才用来设置限定条件的应用层。
 * （因此它通常应当匹配该子句所适用的连接域。）
 * 否则该限定条件应用在能提供其全部变量的
 * 最低连接层。
 *
 * "security_level" 是要赋给新 restrictinfo 的安全级别。
 *
 * "both_const" 表示两项是否都已知为伪常量；
 * 这种情况下值得应用 eval_const_expressions()，看能否
 * 得出常量 TRUE 或常量 FALSE。（否则就不值得，
 * 因为这些表达式已经过 eval_const_expressions。）
 *
 * 返回生成的 RestrictInfo，若有的话。若
 * both_const 为真且成功把该子句化成
 * 常量 TRUE，结果为 NULL。
 *
 * 注意：本函数会复制 item1 和 item2，但保证 Relids 参数是新副本、
 * 不与其他用途
 * 共享，是调用方的责任。
 *
 * 注意：这里不做 initialize_mergeclause_eclasses()。
 * 按需要设置 left_ec/right_ec 是调用方的责任。
 */
RestrictInfo *
process_implied_equality(PlannerInfo *root,
						 Oid opno,
						 Oid collation,
						 Expr *item1,
						 Expr *item2,
						 Relids qualscope,
						 Index security_level,
						 bool both_const)
{
	RestrictInfo *restrictinfo;
	Node	   *clause;
	Relids		relids;
	bool		pseudoconstant = false;

	/*
	 * 建立新子句。复本确保它与
	 * 原件不共享子结构（里面若有子查询，这是必要的……）
	 */
	clause = (Node *) make_opclause(opno,
									BOOLOID,	/* opresulttype（操作结果类型） */
									false,	/* opretset（操作返回集合） */
									copyObject(item1),
									copyObject(item2),
									InvalidOid,
									collation);

	/* 若两者都是常量，就试着化成布尔常量。 */
	if (both_const)
	{
		clause = eval_const_expressions(root, clause);

		/* 若得到常量 TRUE，就丢掉该子句 */
		if (clause && IsA(clause, Const))
		{
			Const	   *cclause = (Const *) clause;

			Assert(cclause->consttype == BOOLOID);
			if (!cclause->constisnull && DatumGetBool(cclause->constvalue))
				return NULL;
		}
	}

	/*
	 * 其余部分是 distribute_qual_to_rels 的大幅精简版。
	 * 可以跳过其中大部分工作，但仍有几种特殊
	 * 情形必须处理。
	 *
	 * 取出可能已简化的子句里提到的全部 relid。
	 */
	relids = pull_varnos(root, clause);
	Assert(bms_is_subset(relids, qualscope));

	/*
	 * 若该子句不含变量，通常把它
	 * 下推到所提到的关系的启发式就不起作用，因为一个关系都没有。
	 * 把它当作门控限定条件用在适当的层（见
	 * get_join_domain_min_rels 的注释）。
	 */
	if (bms_is_empty(relids))
	{
		/* 在连接域的安全层求值 */
		relids = get_join_domain_min_rels(root, qualscope);
		/* 标成门控限定条件 */
		pseudoconstant = true;
		/* 告诉 createplan.c 去检查门控限定条件 */
		root->hasPseudoConstantQuals = true;
	}

	/*
	 * 建立 RestrictInfo 节点本身。
	 */
	restrictinfo = make_restrictinfo(root,
									 (Expr *) clause,
									 true,	/* is_pushed_down（是否已下推） */
									 false, /* !has_clone（不是 has_clone） */
									 false, /* !is_clone（不是 is_clone） */
									 pseudoconstant,
									 security_level,
									 relids,
									 NULL,	/* incompatible_relids（不相容的 relid） */
									 NULL); /* outer_relids（外层 relid） */

	/*
	 * 若是连接子句，就把子句里用到的 var 加进其关系的目标列表，
	 * 以便扫描那些关系的计划节点会把它们发出去
	 * （否则到连接节点时它们不可用！）。
	 *
	 * 通常在这些组成表达式
	 * 第一次被 distribute_qual_to_rels 看到时就已经做过；但有可能
	 * 某些 Var 漏掉了，因为它们原先只
	 * 出现在单关系子句里。因此为
	 * 稳妥起见在这里再做一次。
	 */
	if (bms_membership(relids) == BMS_MULTIPLE)
	{
		List	   *vars = pull_var_clause(clause,
										   PVC_RECURSE_AGGREGATES |
										   PVC_RECURSE_WINDOWFUNCS |
										   PVC_INCLUDE_PLACEHOLDERS);

		add_vars_to_targetlist(root, vars, relids);
		list_free(vars);
	}

	/*
	 * 检查可归并连接性。这通常会成功，因为该操作符来自
	 * 某个 EquivalenceClass；但也许已经把原子句
	 * 化成了常量。
	 */
	check_mergejoinable(restrictinfo);

	/*
	 * 注意这里不做 initialize_mergeclause_eclasses()；调用方可以
	 * 比我们便宜得多地处理那一步。在那之前调用
	 * distribute_restrictinfo_to_rels() 是可以的。
	 */

	/*
	 * 把新子句推进全部适当的 restrictinfo 列表。
	 */
	distribute_restrictinfo_to_rels(root, restrictinfo);

	return restrictinfo;
}

/*
 * build_implied_join_equality --- 为一条推导出的等值建立 RestrictInfo
 *
 * 这与 process_implied_equality() 的功能有重叠，但
 * 不得把该 RestrictInfo 推进 joininfo 树。
 *
 * 注意：本函数会复制 item1 和 item2，但保证 Relids 参数是新副本、
 * 不与其他用途
 * 共享，是调用方的责任。
 *
 * 注意：这里不做 initialize_mergeclause_eclasses()。
 * 按需要设置 left_ec/right_ec 是调用方的责任。
 */
RestrictInfo *
build_implied_join_equality(PlannerInfo *root,
							Oid opno,
							Oid collation,
							Expr *item1,
							Expr *item2,
							Relids qualscope,
							Index security_level)
{
	RestrictInfo *restrictinfo;
	Expr	   *clause;

	/*
	 * 建立新子句。复本确保它与
	 * 原件不共享子结构（里面若有子查询，这是必要的……）
	 */
	clause = make_opclause(opno,
						   BOOLOID, /* opresulttype（操作结果类型） */
						   false,	/* opretset（操作返回集合） */
						   copyObject(item1),
						   copyObject(item2),
						   InvalidOid,
						   collation);

	/*
	 * 建立 RestrictInfo 节点本身。
	 */
	restrictinfo = make_restrictinfo(root,
									 clause,
									 true,	/* is_pushed_down（是否已下推） */
									 false, /* !has_clone（不是 has_clone） */
									 false, /* !is_clone（不是 is_clone） */
									 false, /* 伪常量 */
									 security_level,	/* security_level（安全级别） */
									 qualscope, /* required_relids（所需的 relid） */
									 NULL,	/* incompatible_relids（不相容的 relid） */
									 NULL); /* outer_relids（外层 relid） */

	/* 设置可归并连接/可哈希连接标志 */
	check_mergejoinable(restrictinfo);
	check_hashjoinable(restrictinfo);
	check_memoizable(restrictinfo);

	return restrictinfo;
}

/*
 * get_join_domain_min_rels
 *	  为属于给定 relid 的连接域的推导限定条件
 *	  确定适当的连接层。
 *
 * 当从某个 EquivalenceClass 推导出伪常量（不含 Var）子句时，
 * 理想情况下把该子句用在该等价类连接域的顶层。
 * 不过，若该域内有任何外连接与域外的连接
 * 发生了交换，就会找不到应用
 * 该子句的正确位置。改为从 relid 集合里去掉任何下层外连接，
 * 只把该子句用到剩下的关系上。这仍然得到
 * 正确答案，因为若该子句产生 FALSE，这些
 * 连接的左侧就会为空，从而连接结果为空。
 *
 * 不过，若这是查询的顶层
 * 连接域，就不必去掉外连接，因为没有别的东西可以交换。
 *
 * 注意：在 distribute_qual_to_rels 处理伪常量子句的地方用这个很诱人；
 * 但不能那么做，因为那时必要的
 * SpecialJoinInfos 还没有全部形成。
 *
 * 结果总是新 palloc 出来的；不修改 domain_relids。
 */
static Relids
get_join_domain_min_rels(PlannerInfo *root, Relids domain_relids)
{
	Relids		result = bms_copy(domain_relids);
	ListCell   *lc;

	/* 是顶层连接域吗？ */
	if (bms_equal(result, root->all_query_rels))
		return result;

	/* 不是，就找可能交换出去的下层外连接 */
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (sjinfo->jointype == JOIN_LEFT &&
			bms_is_member(sjinfo->ojrelid, result))
		{
			result = bms_del_member(result, sjinfo->ojrelid);
			result = bms_del_members(result, sjinfo->syn_righthand);
		}
	}
	return result;
}


/*
 * match_foreign_keys_to_quals
 *		把外键约束与等价类和连接限定条件匹配
 *
 * 这里的想法是看哪些查询连接条件匹配某个外键
 * 关系的等值约束。对这类连接条件，
 * 可以用外键语义做出比根据统计估计
 * 更可靠的选择率估计，对多列
 * 外键尤其如此，因为条件相互独立的通常假定往往会失败。
 *
 * 本函数给 root->fkey_list 里的 ForeignKeyOptInfos
 * 标注它们匹配哪些等价类和连接限定子句，并
 * 丢掉对查询无关的任何 ForeignKeyOptInfos。
 */
void
match_foreign_keys_to_quals(PlannerInfo *root)
{
	List	   *newlist = NIL;
	ListCell   *lc;

	foreach(lc, root->fkey_list)
	{
		ForeignKeyOptInfo *fkinfo = (ForeignKeyOptInfo *) lfirst(lc);
		RelOptInfo *con_rel;
		RelOptInfo *ref_rel;
		int			colno;

		/*
		 * 任一 relid 都可能标识查询 rtable 里的某个关系，但它
		 * 没被连接树引用，或已被连接
		 * 消除去掉，因此不会有 RelOptInfo。所以这里不要用
		 * find_base_rel()。这类外键可以忽略。
		 */
		if (fkinfo->con_relid >= root->simple_rel_array_size ||
			fkinfo->ref_relid >= root->simple_rel_array_size)
			continue;			/* 只是多留个心 */
		con_rel = root->simple_rel_array[fkinfo->con_relid];
		if (con_rel == NULL)
			continue;
		ref_rel = root->simple_rel_array[fkinfo->ref_relid];
		if (ref_rel == NULL)
			continue;

		/*
		 * 除非两个关系都是基关系，否则忽略该外键。这去掉了
		 * 链到继承子关系（otherrels）的外键。
		 */
		if (con_rel->reloptkind != RELOPT_BASEREL ||
			ref_rel->reloptkind != RELOPT_BASEREL)
			continue;

		/*
		 * 扫描这些列，试着把它们匹配到等价类和限定条件。
		 *
		 * 注意：对简单内连接，任何匹配都应当在某个等价类里。
		 * 语法上匹配某条外键等值的 "松散" 限定条件，一定是因为
		 * 它们是外连接限定条件或
		 * 类似情形，才被拒绝进入等价类。仍可以把它们看成匹配该外键。
		 */
		for (colno = 0; colno < fkinfo->nkeys; colno++)
		{
			EquivalenceClass *ec;
			AttrNumber	con_attno,
						ref_attno;
			Oid			fpeqop;
			ListCell   *lc2;

			ec = match_eclasses_to_foreign_key_col(root, fkinfo, colno);
			/* 若已经得到等价类匹配，就不必再找松散限定条件 */
			if (ec != NULL)
			{
				fkinfo->nmatched_ec++;
				if (ec->ec_has_const)
					fkinfo->nconst_ec++;
				continue;
			}

			/*
			 * 扫描 joininfo 列表里的相关子句。任一关系的 joininfo
			 * 列表都同样合适；这里用 con_rel 的。
			 */
			con_attno = fkinfo->conkey[colno];
			ref_attno = fkinfo->confkey[colno];
			fpeqop = InvalidOid;	/* 只在需要时才查这个 */

			foreach(lc2, con_rel->joininfo)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc2);
				OpExpr	   *clause = (OpExpr *) rinfo->clause;
				Var		   *leftvar;
				Var		   *rightvar;

				/* 只有二元 OpExprs 值得考虑 */
				if (!IsA(clause, OpExpr) ||
					list_length(clause->args) != 2)
					continue;
				leftvar = (Var *) get_leftop((Expr *) clause);
				rightvar = (Var *) get_rightop((Expr *) clause);

				/* 操作数必须是 Var，也许带有 RelabelType */
				while (leftvar && IsA(leftvar, RelabelType))
					leftvar = (Var *) ((RelabelType *) leftvar)->arg;
				if (!(leftvar && IsA(leftvar, Var)))
					continue;
				while (rightvar && IsA(rightvar, RelabelType))
					rightvar = (Var *) ((RelabelType *) rightvar)->arg;
				if (!(rightvar && IsA(rightvar, Var)))
					continue;

				/* 现在试着把这些 var 匹配到当前外键列 */
				if (fkinfo->ref_relid == leftvar->varno &&
					ref_attno == leftvar->varattno &&
					fkinfo->con_relid == rightvar->varno &&
					con_attno == rightvar->varattno)
				{
					/* Var 匹配了，但操作符对吗？ */
					if (clause->opno == fkinfo->conpfeqop[colno])
					{
						fkinfo->rinfos[colno] = lappend(fkinfo->rinfos[colno],
														rinfo);
						fkinfo->nmatched_ri++;
					}
				}
				else if (fkinfo->ref_relid == rightvar->varno &&
						 ref_attno == rightvar->varattno &&
						 fkinfo->con_relid == leftvar->varno &&
						 con_attno == leftvar->varattno)
				{
					/*
					 * 反向匹配时，必须检查交换子操作符。若还没
					 * 查过就查一下。（最坏情况下这里也许会
					 * 做多次查找，但那将需要一个没有交换子的外键
					 * 等值操作符，这
					 * 不太可能。）
					 */
					if (!OidIsValid(fpeqop))
						fpeqop = get_commutator(fkinfo->conpfeqop[colno]);
					if (clause->opno == fpeqop)
					{
						fkinfo->rinfos[colno] = lappend(fkinfo->rinfos[colno],
														rinfo);
						fkinfo->nmatched_ri++;
					}
				}
			}
			/* 若找到任何匹配的松散限定条件，就把该列计为已匹配 */
			if (fkinfo->rinfos[colno])
				fkinfo->nmatched_rcols++;
		}

		/*
		 * 目前丢掉没有与查询完全匹配的多列外键。
		 * 以后也许会弄清如何从它们推导某种
		 * 估计，那时这个测试应放宽为
		 * "if ((fkinfo->nmatched_ec + fkinfo->nmatched_rcols) > 0)"。
		 */
		if ((fkinfo->nmatched_ec + fkinfo->nmatched_rcols) == fkinfo->nkeys)
			newlist = lappend(newlist, fkinfo);
	}
	/* 替换 fkey_list，从而丢掉任何无用项 */
	root->fkey_list = newlist;
}


/*****************************************************************************
 *
 *	 CHECKS FOR MERGEJOINABLE AND HASHJOINABLE CLAUSES（可归并连接与可哈希连接子句检查）
 *
 *****************************************************************************/

/*
 * check_mergejoinable
 *	  若该 restrictinfo 的子句可归并连接，就在该 restrictinfo 里设置归并连接
 *	  信息字段。
 *
 *	  目前，对二元操作子句支持归并连接，其中
 *	  操作符是可归并连接的操作符。参数可以是
 *	  任何东西——只要里面没有易变函数。
 */
static void
check_mergejoinable(RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	Oid			opno;
	Node	   *leftarg;

	if (restrictinfo->pseudoconstant)
		return;
	if (!is_opclause(clause))
		return;
	if (list_length(((OpExpr *) clause)->args) != 2)
		return;

	opno = ((OpExpr *) clause)->opno;
	leftarg = linitial(((OpExpr *) clause)->args);

	if (op_mergejoinable(opno, exprType(leftarg)) &&
		!contain_volatile_functions((Node *) restrictinfo))
		restrictinfo->mergeopfamilies = get_mergejoin_opfamilies(opno);

	/*
	 * 注意：op_mergejoinable 只是提示；若在任何 btree opfamily 里都找不到该操作符，
	 * mergeopfamilies 就仍为 NIL，于是该子句
	 * 不按可归并连接处理。
	 */
}

/*
 * check_hashjoinable
 *	  若该 restrictinfo 的子句可哈希连接，就在该 restrictinfo 里设置哈希连接
 *	  信息字段。
 *
 *	  目前，对二元操作子句支持哈希连接，其中
 *	  操作符是可哈希连接的操作符。参数可以是
 *	  任何东西——只要里面没有易变函数。
 */
static void
check_hashjoinable(RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	Oid			opno;
	Node	   *leftarg;

	if (restrictinfo->pseudoconstant)
		return;
	if (!is_opclause(clause))
		return;
	if (list_length(((OpExpr *) clause)->args) != 2)
		return;

	opno = ((OpExpr *) clause)->opno;
	leftarg = linitial(((OpExpr *) clause)->args);

	if (op_hashjoinable(opno, exprType(leftarg)) &&
		!contain_volatile_functions((Node *) restrictinfo))
		restrictinfo->hashjoinoperator = opno;
}

/*
 * check_memoizable
 *	  若该 restrictinfo 的子句适合用于 Memoize 节点，
 *	  就把 left_hasheqoperator 和 right_hasheqoperator 设成缓存期间
 *	  将需要的哈希等值操作符。
 */
static void
check_memoizable(RestrictInfo *restrictinfo)
{
	TypeCacheEntry *typentry;
	Expr	   *clause = restrictinfo->clause;
	Oid			lefttype;
	Oid			righttype;

	if (restrictinfo->pseudoconstant)
		return;
	if (!is_opclause(clause))
		return;
	if (list_length(((OpExpr *) clause)->args) != 2)
		return;

	lefttype = exprType(linitial(((OpExpr *) clause)->args));

	typentry = lookup_type_cache(lefttype, TYPECACHE_HASH_PROC |
								 TYPECACHE_EQ_OPR);

	if (OidIsValid(typentry->hash_proc) && OidIsValid(typentry->eq_opr))
		restrictinfo->left_hasheqoperator = typentry->eq_opr;

	righttype = exprType(lsecond(((OpExpr *) clause)->args));

	/*
	 * 查找右侧类型，除非它与左侧类型相同；那种情况下
	 * typentry 已经指向所需的 TypeCacheEntry。
	 */
	if (lefttype != righttype)
		typentry = lookup_type_cache(righttype, TYPECACHE_HASH_PROC |
									 TYPECACHE_EQ_OPR);

	if (OidIsValid(typentry->hash_proc) && OidIsValid(typentry->eq_opr))
		restrictinfo->right_hasheqoperator = typentry->eq_opr;
}
