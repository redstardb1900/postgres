/*-------------------------------------------------------------------------
 *
 * placeholder.c
 *	  操作 PlaceHolderVar 与 PlaceHolderInfo 的例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/placeholder.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"
#include "utils/lsyscache.h"


typedef struct contain_placeholder_references_context
{
	int			relid;
	int			sublevels_up;
} contain_placeholder_references_context;

/* 局部函数 */
static void find_placeholders_recurse(PlannerInfo *root, Node *jtnode);
static void find_placeholders_in_expr(PlannerInfo *root, Node *expr);
static bool contain_placeholder_references_walker(Node *node,
												  contain_placeholder_references_context *context);


/*
 * make_placeholder_expr
 *		为给定表达式做一个 PlaceHolderVar。
 *
 * phrels 是要归给该表达式的语法位置
 * （一组 relid）。
 *
 * 调用者负责按需要调整 phlevelsup 和
 * phnullingrels。
 * 因为这里不知道该 PHV 会关联到哪一层查询，
 * 本函数只碰 root->glob 很重要；去动 PlannerInfo 的其他部分
 * 很可能会做错事。
 */
PlaceHolderVar *
make_placeholder_expr(PlannerInfo *root, Expr *expr, Relids phrels)
{
	PlaceHolderVar *phv = makeNode(PlaceHolderVar);

	phv->phexpr = expr;
	phv->phrels = phrels;
	phv->phnullingrels = NULL;	/* 调用者以后可以改这个 */
	phv->phid = ++(root->glob->lastPHId);
	phv->phlevelsup = 0;		/* 调用者以后可以改这个 */

	return phv;
}

/*
 * find_placeholder_info
 *		取得给定 PHV 的 PlaceHolderInfo
 *
 * 若 PlaceHolderInfo 还不存在，且我们还没有冻结
 * 该查询的 PlaceHolderInfos 集合，就创建它；否则报错。
 *
 * 这与 make_placeholder_expr 分开，是因为子查询上拉必须为
 * 在上层查询中也许根本用不到、或常量表达式简化后也许不再留下的表达式
 * 建立 PlaceHolderVars。
 * 我们只为仍然出现在交给 query_planner() 的简化查询中的 PHVs
 * 建立 PlaceHolderInfos。
 *
 * 注意：只应在 query_planner() 已经开始之后调用。
 */
PlaceHolderInfo *
find_placeholder_info(PlannerInfo *root, PlaceHolderVar *phv)
{
	PlaceHolderInfo *phinfo;
	Relids		rels_used;

	/* 若这一点不再成立，就需要能到父列表里查找 */
	Assert(phv->phlevelsup == 0);

	/* 用 placeholder_array 快速查找已有的 PlaceHolderInfo */
	if (phv->phid < root->placeholder_array_size)
		phinfo = root->placeholder_array[phv->phid];
	else
		phinfo = NULL;
	if (phinfo != NULL)
	{
		Assert(phinfo->phid == phv->phid);
		return phinfo;
	}

	/* 没找到，因此创建它 */
	if (root->placeholdersFrozen)
		elog(ERROR, "too late to create a new PlaceHolderInfo");

	phinfo = makeNode(PlaceHolderInfo);

	phinfo->phid = phv->phid;
	phinfo->ph_var = copyObject(phv);

	/*
	 * 按约定，phinfo->ph_var->phnullingrels 总是空的，因为
	 * PlaceHolderInfo 代表
	 * PlaceHolderVar 最初算出来的状态。
	 * 出现在查询树中的 PlaceHolderVars 的 phnullingrels 可能各不相同，
	 * 反映计算层之上应用的外连接。
	 */
	phinfo->ph_var->phnullingrels = NULL;

	/*
	 * 任何位于该 PHV 语法范围之外的被引用关系都是
	 * LATERAL 引用，应当放进 ph_lateral，而不放进
	 * ph_eval_at。若没有任何被引用关系位于语法范围之内，
	 * 就强制在语法位置求值。
	 */
	rels_used = pull_varnos(root, (Node *) phv->phexpr);
	phinfo->ph_lateral = bms_difference(rels_used, phv->phrels);
	phinfo->ph_eval_at = bms_int_members(rels_used, phv->phrels);
	/* 若不含 var，强制在语法位置求值 */
	if (bms_is_empty(phinfo->ph_eval_at))
	{
		phinfo->ph_eval_at = bms_copy(phv->phrels);
		Assert(!bms_is_empty(phinfo->ph_eval_at));
	}
	phinfo->ph_needed = NULL;	/* 最初它未被使用 */
	/* 眼下只用数据类型信息估计宽度 */
	phinfo->ph_width = get_typavgwidth(exprType((Node *) phv->phexpr),
									   exprTypmod((Node *) phv->phexpr));

	/*
	 * 同时加入 placeholder_list 和 placeholder_array。注意：因为我们
	 * 在两个数据结构里存放指向 PlaceHolderInfos 的指针，把整个
	 * placeholder_list 结构交给 expression_tree_mutator 之类是不安全的——
	 * 至少事后必须
	 * 重建 placeholder_array。
	 */
	root->placeholder_list = lappend(root->placeholder_list, phinfo);

	if (phinfo->phid >= root->placeholder_array_size)
	{
		/* 必须分配或扩大 placeholder_array */
		int			new_size;

		new_size = root->placeholder_array_size ? root->placeholder_array_size * 2 : 8;
		while (phinfo->phid >= new_size)
			new_size *= 2;
		if (root->placeholder_array)
			root->placeholder_array =
				repalloc0_array(root->placeholder_array, PlaceHolderInfo *, root->placeholder_array_size, new_size);
		else
			root->placeholder_array =
				palloc0_array(PlaceHolderInfo *, new_size);
		root->placeholder_array_size = new_size;
	}
	root->placeholder_array[phinfo->phid] = phinfo;

	/*
	 * 该 PHV 所含表达式可能含有其他更低层的 PHVs。我们
	 * 现在知道也要把那些放进 PlaceHolderInfo 列表，因此
	 * 不妨立刻做。
	 */
	find_placeholders_in_expr(root, (Node *) phinfo->ph_var->phexpr);

	return phinfo;
}

/*
 * find_placeholders_in_jointree
 *		在连接树中搜索 PlaceHolderVars，并建立 PlaceHolderInfos
 *
 * 不必看目标列表，因为 build_base_rel_tlists()
 * 已经为 tlist 中的任何 PHVs 建了项。
 */
void
find_placeholders_in_jointree(PlannerInfo *root)
{
	/* 这必须在冻结 PHIs 集合之前完成 */
	Assert(!root->placeholdersFrozen);

	/* 若查询不含 PlaceHolderVars，就什么也不必做 */
	if (root->glob->lastPHId != 0)
	{
		/* 从连接树顶部开始递归 */
		Assert(root->parse->jointree != NULL &&
			   IsA(root->parse->jointree, FromExpr));
		find_placeholders_recurse(root, (Node *) root->parse->jointree);
	}
}

/*
 * find_placeholders_recurse
 *	  find_placeholders_in_jointree 的一层递归。
 *
 * jtnode 是当前要检查的连接树节点。
 */
static void
find_placeholders_recurse(PlannerInfo *root, Node *jtnode)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		/* 这里没有限定条件要处理 */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		/*
		 * 先递归处理子连接。
		 */
		foreach(l, f->fromlist)
		{
			find_placeholders_recurse(root, lfirst(l));
		}

		/*
		 * 现在处理顶层限定条件。
		 */
		find_placeholders_in_expr(root, f->quals);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/*
		 * 先递归处理子连接。
		 */
		find_placeholders_recurse(root, j->larg);
		find_placeholders_recurse(root, j->rarg);

		/* 处理限定条件子句 */
		find_placeholders_in_expr(root, j->quals);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * find_placeholders_in_expr
 *		找出给定表达式中的全部 PlaceHolderVars，并为它们创建
 *		PlaceHolderInfo 项。
 */
static void
find_placeholders_in_expr(PlannerInfo *root, Node *expr)
{
	List	   *vars;
	ListCell   *vl;

	/*
	 * pull_var_clause 做的比这里需要的多，但够用，而且
	 * 用起来方便。
	 */
	vars = pull_var_clause(expr,
						   PVC_RECURSE_AGGREGATES |
						   PVC_RECURSE_WINDOWFUNCS |
						   PVC_INCLUDE_PLACEHOLDERS);
	foreach(vl, vars)
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) lfirst(vl);

		/* 忽略任何普通 Var */
		if (!IsA(phv, PlaceHolderVar))
			continue;

		/* 若还没有 PlaceHolderInfo 项，就创建一个 */
		(void) find_placeholder_info(root, phv);
	}
	list_free(vars);
}

/*
 * fix_placeholder_input_needed_levels
 *		调整占位符输入的需要于层次
 *
 * 这在我们确定完全部占位符的 eval_at 层次之后调用。
 * 必须确保求值每个占位符所需的全部 var 和占位符
 * 在进行求值的扫描或连接层可用。（扫描层的
 * 求值看起来也许没意思，但并非如此：占位符表达式中的
 * LATERAL 引用
 * 需要使被引用的 var 或
 * 占位符在求值它的扫描中被标成需要。）
 * 注意这个循环可能对其他 PlaceHolderInfos 的 ph_needed 集合产生副作用；
 * 这没问题，因为这里不检查 ph_needed，所以
 * 不必担心顺序问题。
 */
void
fix_placeholder_input_needed_levels(PlannerInfo *root)
{
	ListCell   *lc;

	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);
		List	   *vars = pull_var_clause((Node *) phinfo->ph_var->phexpr,
										   PVC_RECURSE_AGGREGATES |
										   PVC_RECURSE_WINDOWFUNCS |
										   PVC_INCLUDE_PLACEHOLDERS);

		add_vars_to_targetlist(root, vars, phinfo->ph_eval_at);
		list_free(vars);
	}
}

/*
 * add_placeholders_to_base_rels
 *		把任何所需的 PlaceHolderVars 加到基关系的目标列表。
 *
 * 若任何占位符可以在某个基关系上计算、并且在它之上被需要，
 * 就把它加到该关系的目标列表。这看起来也许可以与
 * fix_placeholder_input_needed_levels 合并，但必须分开，因为
 * 连接去除发生在两者之间，并且会改变 ph_eval_at 集合。
 * add_placeholders_to_joinrel 里本质上是同样的逻辑，但那一部分要等
 * joinrel 形成之后才能做。
 */
void
add_placeholders_to_base_rels(PlannerInfo *root)
{
	ListCell   *lc;

	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);
		Relids		eval_at = phinfo->ph_eval_at;
		int			varno;

		if (bms_get_singleton_member(eval_at, &varno) &&
			bms_nonempty_difference(phinfo->ph_needed, eval_at))
		{
			RelOptInfo *rel = find_base_rel(root, varno);

			/*
			 * 与 add_vars_to_targetlist() 一样，在扫描层算出的值
			 * 还没有被任何外连接置空，因此它的 phnullingrels
			 * 应当为空。
			 */
			Assert(phinfo->ph_var->phnullingrels == NULL);

			/* 这里复制 PHV 也许不必要，但求安全 */
			rel->reltarget->exprs = lappend(rel->reltarget->exprs,
											copyObject(phinfo->ph_var));
			/* reltarget 的代价和宽度字段以后会更新 */
		}
	}
}

/*
 * add_placeholders_to_joinrel
 *		把任何新可计算的 PlaceHolderVars 加到连接关系的目标列表；
 *		若可计算的 PHVs 含有 LATERAL 引用，把那些
 *		引用加到该 joinrel 的 direct_lateral_relids。
 *
 * 连接关系应当发出一个 PlaceHolderVar，若 (a) 该 PHV 可以在
 * 本连接层或之下计算，且 (b) 该 PHV 在本层之上被需要。
 * 调用者 build_join_rel() 已经加入了在任一连接输入关系中算出的 PHVs，
 * 因此只需把新可计算的那些加进
 * 目标列表。不过，对本连接可计算或在其下可计算的每个
 * PHV，都必须更新 direct_lateral_relids，原因见下。
 */
void
add_placeholders_to_joinrel(PlannerInfo *root, RelOptInfo *joinrel,
							RelOptInfo *outer_rel, RelOptInfo *inner_rel,
							SpecialJoinInfo *sjinfo)
{
	Relids		relids = joinrel->relids;
	ListCell   *lc;

	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);

		/* 能在这里计算吗？ */
		if (bms_is_subset(phinfo->ph_eval_at, relids))
		{
			/* 在这个 joinrel 之上是否仍被需要？ */
			if (bms_nonempty_difference(phinfo->ph_needed, relids))
			{
				/*
				 * 是，但只有当它不是在任一输入中算出时才加入 tlist；
				 * 否则它应当已经在那里。另外，若该 PHV
				 * 可以在这里计算、
				 * 但不能在任一输入中计算，我们计入所含表达式的求值代价。
				 * 这有点勉强，因为我们根据为该
				 * joinrel 考虑的第一对可能输入关系来做决定。
				 * 换其他对时，也许可以在其中一个输入里算出
				 * 该 PHV，于是会对某些连接路径重复
				 * 计入该 PHV 的代价。眼下先
				 * 忍受这一点；以后也许想改进，办法是
				 * 为每一对输入重算 reltarget 代价。
				 */
				if (!bms_is_subset(phinfo->ph_eval_at, outer_rel->relids) &&
					!bms_is_subset(phinfo->ph_eval_at, inner_rel->relids))
				{
					/* 这里复制也许不必要，但求安全 */
					PlaceHolderVar *phv = copyObject(phinfo->ph_var);
					QualCost	cost;

					/*
					 * 它一开始不会被任何东西置空。本连接之上的连接
					 * 以后可能以与 Var 大致相同的方式
					 * 往它的 phnullingrels 里加东西。
					 */
					Assert(phv->phnullingrels == NULL);

					joinrel->reltarget->exprs = lappend(joinrel->reltarget->exprs,
														phv);
					cost_qual_eval_node(&cost, (Node *) phv->phexpr, root);
					joinrel->reltarget->cost.startup += cost.startup;
					joinrel->reltarget->cost.per_tuple += cost.per_tuple;
					joinrel->reltarget->width += phinfo->ph_width;
				}
			}

			/*
			 * 也调整 joinrel 的 direct_lateral_relids，以纳入
			 * 该 PHV 的源关系。即使实际上不打算发出该 PHV，也必须这样做，
			 * 否则 join_is_legal() 会
			 * 拒绝合法的连接顺序。（原则上也许可以
			 * 改为去掉该 joinrel 对 lateral_relids 的依赖；但
			 * 要做对很复杂，而不打算
			 * 发出该 PHV 的情形太少，不值得做这件工作。）
			 *
			 * 原则上只应在该连接尚未
			 * 包含该 PHV 的源关系时才这样做。但调用者
			 * build_join_rel() 会通过从 direct_lateral_relids 中去掉该连接
			 * 自身的 relid 来收拾，因此这里不必
			 * 考虑这一点。
			 */
			joinrel->direct_lateral_relids =
				bms_add_members(joinrel->direct_lateral_relids,
								phinfo->ph_lateral);
		}
	}
}

/*
 * contain_placeholder_references_to
 *		检测给定子句中是否有任何 PlaceHolderVars 含有
 *		对给定 relid（通常是外连接 relid）的引用。
 *
 * 含有是指在该 PHV 所含表达式内部使用了该 relid，
 * 因此改变该关系的可空状态
 * 可能改变该 PHV 算出的值。
 *
 * 这里应付上层 PHVs 的代码很可能是死的，但仍保留
 * 以防万一。
 */
bool
contain_placeholder_references_to(PlannerInfo *root, Node *clause,
								  int relid)
{
	contain_placeholder_references_context context;

	/* 常见情况下根本没有 PHVs，可以很快回答 */
	if (root->glob->lastPHId == 0)
		return false;
	/* 否则进行递归搜索 */
	context.relid = relid;
	context.sublevels_up = 0;
	return contain_placeholder_references_walker(clause, &context);
}

static bool
contain_placeholder_references_walker(Node *node,
									  contain_placeholder_references_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* 只应看穿其他查询层的 PHVs */
		if (phv->phlevelsup == context->sublevels_up)
		{
			/* 若 phrels 匹配，就找到了要找的东西 */
			if (bms_is_member(context->relid, phv->phrels))
				return true;

			/*
			 * 不应当检查 phnullingrels：要找的是
			 * 所含表达式中的引用，而不是事后可能把结果
			 * 置空的外连接。另外不必递归进入
			 * 所含表达式，因为 phrels 应当充分
			 * 概括其中的内容。因此到这里就结束了。
			 */
			return false;
		}
	}
	else if (IsA(node, Query))
	{
		/* 递归进入 RTE 子查询或尚未规划的子链接子查询 */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   contain_placeholder_references_walker,
								   context,
								   0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, contain_placeholder_references_walker,
								  context);
}
