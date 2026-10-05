/*-------------------------------------------------------------------------
 *
 * prepjointree.c
 *	  规划器对子查询的预处理，以及连接树操作。
 *
 * 说明：调用这些操作的预定顺序是
 *		replace_empty_jointree
 *		pull_up_sublinks
 *		preprocess_function_rtes
 *		pull_up_subqueries
 *		flatten_simple_union_all
 *		做表达式预处理（包括展平 JOIN 别名 Var）
 *		reduce_outer_joins
 *		remove_useless_result_rtes
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepjointree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/multibitmapset.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/placeholder.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"


typedef struct nullingrel_info
{
	/*
	 * 对于每个叶子 RTE，nullingrels[rti] 是可能把该 RTE 置空的外连接的 relid 集合
	 * 中的那些 relid。
	 */
	Relids	   *nullingrels;
	/* 范围表长度（nullingrels[] 中的最大下标） */
	int			rtlength;		/* 仅用于断言检查 */
} nullingrel_info;

typedef struct pullup_replace_vars_context
{
	PlannerInfo *root;
	List	   *targetlist;		/* 正在上拉的子查询的 tlist */
	RangeTblEntry *target_rte;	/* 子查询的 RTE */
	Relids		relids;			/* 子查询内部的 relid，按上拉之后的编号
								 * （仅当 target_rte->lateral 时设置） */
	nullingrel_info *nullinfo;	/* 每个 RTE 的 nullingrel 信息（仅当
								 * target_rte->lateral 时设置） */
	bool	   *outer_hasSubLinks;	/* -> 外层查询的 hasSubLinks */
	int			varno;			/* 子查询的 varno */
	bool		wrap_non_vars;	/* 是否需要把所有非 Var 输出都做成 PHVs？ */
	Node	  **rv_cache;		/* 带 PHVs 的结果缓存 */
} pullup_replace_vars_context;

typedef struct reduce_outer_joins_pass1_state
{
	Relids		relids;			/* 本子树内的基表 relid */
	bool		contains_outer; /* 子树是否包含外连接？ */
	List	   *sub_states;		/* 子树各组成部分的状态列表 */
} reduce_outer_joins_pass1_state;

typedef struct reduce_outer_joins_pass2_state
{
	Relids		inner_reduced;	/* 已简化为普通内连接的外连接 relid */
	List	   *partial_reduced;	/* 部分简化的 FULL 连接列表 */
} reduce_outer_joins_pass2_state;

typedef struct reduce_outer_joins_partial_state
{
	int			full_join_rti;	/* 原先为 FULL 连接的 RT 下标 */
	Relids		unreduced_side; /* 其仍然可空一侧中的 relid */
} reduce_outer_joins_partial_state;

static Node *pull_up_sublinks_jointree_recurse(PlannerInfo *root, Node *jtnode,
											   Relids *relids);
static Node *pull_up_sublinks_qual_recurse(PlannerInfo *root, Node *node,
										   Node **jtlink1, Relids available_rels1,
										   Node **jtlink2, Relids available_rels2);
static Node *pull_up_subqueries_recurse(PlannerInfo *root, Node *jtnode,
										JoinExpr *lowest_outer_join,
										AppendRelInfo *containing_appendrel);
static Node *pull_up_simple_subquery(PlannerInfo *root, Node *jtnode,
									 RangeTblEntry *rte,
									 JoinExpr *lowest_outer_join,
									 AppendRelInfo *containing_appendrel);
static Node *pull_up_simple_union_all(PlannerInfo *root, Node *jtnode,
									  RangeTblEntry *rte);
static void pull_up_union_leaf_queries(Node *setOp, PlannerInfo *root,
									   int parentRTindex, Query *setOpQuery,
									   int childRToffset);
static void make_setop_translation_list(Query *query, int newvarno,
										AppendRelInfo *appinfo);
static bool is_simple_subquery(PlannerInfo *root, Query *subquery,
							   RangeTblEntry *rte,
							   JoinExpr *lowest_outer_join);
static Node *pull_up_simple_values(PlannerInfo *root, Node *jtnode,
								   RangeTblEntry *rte);
static bool is_simple_values(PlannerInfo *root, RangeTblEntry *rte);
static Node *pull_up_constant_function(PlannerInfo *root, Node *jtnode,
									   RangeTblEntry *rte,
									   AppendRelInfo *containing_appendrel);
static bool is_simple_union_all(Query *subquery);
static bool is_simple_union_all_recurse(Node *setOp, Query *setOpQuery,
										List *colTypes);
static bool is_safe_append_member(Query *subquery);
static bool jointree_contains_lateral_outer_refs(PlannerInfo *root,
												 Node *jtnode, bool restricted,
												 Relids safe_upper_varnos);
static void perform_pullup_replace_vars(PlannerInfo *root,
										pullup_replace_vars_context *rvcontext,
										AppendRelInfo *containing_appendrel);
static void replace_vars_in_jointree(Node *jtnode,
									 pullup_replace_vars_context *context);
static Node *pullup_replace_vars(Node *expr,
								 pullup_replace_vars_context *context);
static Node *pullup_replace_vars_callback(Var *var,
										  replace_rte_variables_context *context);
static Query *pullup_replace_vars_subquery(Query *query,
										   pullup_replace_vars_context *context);
static reduce_outer_joins_pass1_state *reduce_outer_joins_pass1(Node *jtnode);
static void reduce_outer_joins_pass2(Node *jtnode,
									 reduce_outer_joins_pass1_state *state1,
									 reduce_outer_joins_pass2_state *state2,
									 PlannerInfo *root,
									 Relids nonnullable_rels,
									 List *forced_null_vars);
static void report_reduced_full_join(reduce_outer_joins_pass2_state *state2,
									 int rtindex, Relids relids);
static Node *remove_useless_results_recurse(PlannerInfo *root, Node *jtnode,
											Node **parent_quals,
											Relids *dropped_outer_joins);
static int	get_result_relid(PlannerInfo *root, Node *jtnode);
static void remove_result_refs(PlannerInfo *root, int varno, Node *newjtloc);
static bool find_dependent_phvs(PlannerInfo *root, int varno);
static bool find_dependent_phvs_in_jointree(PlannerInfo *root,
											Node *node, int varno);
static void substitute_phv_relids(Node *node,
								  int varno, Relids subrelids);
static void fix_append_rel_relids(PlannerInfo *root, int varno,
								  Relids subrelids);
static Node *find_jointree_node_for_rel(Node *jtnode, int relid);
static nullingrel_info *get_nullingrels(Query *parse);
static void get_nullingrels_recurse(Node *jtnode, Relids upper_nullingrels,
									nullingrel_info *info);


/*
 * transform_MERGE_to_join
 *		把 MERGE 的连接树替换为同时包含目标关系。
 */
void
transform_MERGE_to_join(Query *parse)
{
	RangeTblEntry *joinrte;
	JoinExpr   *joinexpr;
	JoinType	jointype;
	int			joinrti;
	List	   *vars;

	if (parse->commandType != CMD_MERGE)
		return;

	/* XXX 多半是错的 */
	vars = NIL;

	/*
	 * 若存在任何 WHEN NOT MATCHED THEN INSERT 子句，就需要使用
	 * 外连接，以便处理源关系中所有未匹配的元组。
	 * 若一个都不存在，则可以使用内连接。
	 */
	if (parse->mergeUseOuterJoin)
		jointype = JOIN_RIGHT;
	else
		jointype = JOIN_INNER;

	/* 造出一个供使用的连接 RTE。 */
	joinrte = makeNode(RangeTblEntry);
	joinrte->rtekind = RTE_JOIN;
	joinrte->jointype = jointype;
	joinrte->joinmergedcols = 0;
	joinrte->joinaliasvars = vars;
	joinrte->joinleftcols = NIL;	/* MERGE 不允许 JOIN USING */
	joinrte->joinrightcols = NIL;	/* 同上 */
	joinrte->join_using_alias = NULL;

	joinrte->alias = NULL;
	joinrte->eref = makeAlias("*MERGE*", NIL);
	joinrte->lateral = false;
	joinrte->inh = false;
	joinrte->inFromCl = true;

	/*
	 * 把完成的 RTE 加入 pstate 的范围表列表，以便知道它的
	 * 下标。
	 */
	parse->rtable = lappend(parse->rtable, joinrte);
	joinrti = list_length(parse->rtable);

	/*
	 * 在目标与源关系之间创建一个 JOIN。
	 */
	joinexpr = makeNode(JoinExpr);
	joinexpr->jointype = jointype;
	joinexpr->isNatural = false;
	joinexpr->larg = (Node *) makeNode(RangeTblRef);
	((RangeTblRef *) joinexpr->larg)->rtindex = parse->resultRelation;
	joinexpr->rarg = linitial(parse->jointree->fromlist);	/* 原来的连接 */
	joinexpr->usingClause = NIL;
	joinexpr->join_using_alias = NULL;
	/* 限定条件从连接树中取出，放进这个特定连接 */
	joinexpr->quals = parse->jointree->quals;
	joinexpr->alias = NULL;
	joinexpr->rtindex = joinrti;

	/* 让新连接成为查询连接树中的唯一项 */
	parse->jointree->fromlist = list_make1(joinexpr);
	parse->jointree->quals = NULL;
}

/*
 * replace_empty_jointree
 *		若 Query 的连接树为空，用一个哑 RTE_RESULT
 *		关系替换它。
 *
 * 这样做可以避开以前为省略 FROM 子句的 SELECTs 所存在的一批边角情况。
 * 例如，连接树为空的子查询以前无法上拉，因为那会
 * 得到空的 relid 集合，使该子查询无法被唯一
 * 标识，从而妨碍连接或 PlaceHolderVar 处理。
 * 与本文件中多数其他函数不同，本函数不递归；
 *
 * 我们依靠其他处理在合适的时机
 * 对子查询调用它。
 */
void
replace_empty_jointree(Query *parse)
{
	RangeTblEntry *rte;
	Index		rti;
	RangeTblRef *rtr;

	/* 连接树已经非空则无需处理 */
	if (parse->jointree->fromlist != NIL)
		return;

	/* 在集合运算树的顶层也不得改动它 */
	if (parse->setOperations)
		return;

	/* 创建合适的 RTE */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RESULT;
	rte->eref = makeAlias("*RESULT*", NIL);

	/* 把它加入范围表 */
	parse->rtable = lappend(parse->rtable, rte);
	rti = list_length(parse->rtable);

	/* 并把一个引用塞进连接树 */
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = rti;
	parse->jointree->fromlist = list_make1(rtr);
}

/*
 * pull_up_sublinks
 *		尝试把 ANY 和 EXISTS SubLinks 上拉，当作
 *		半连接或反半连接来处理。
 *
 * 子句 "foo op ANY (sub-SELECT)" 可以把
 * sub-SELECT 上拉成范围表项，并把隐含的
 * 比较当作半连接的限定条件。不过这种处理只在
 * WHERE 或 JOIN/ON 子句的顶层有效，因为我们无法
 * 区分在涉及 NULL 输入时 ANY 应当返回 FALSE 还是 NULL。
 * 另外，在外连接的 ON 子句中，只有当子链接退化
 * （即只引用连接的可空一侧）时才能这样做。
 * 这时可以把半连接下推到连接的可空一侧。
 * 若子链接引用了任何不可空一侧的变量，
 * 它就必须作为
 * 外连接的一部分求值，事情会变得过于复杂。
 *
 * 在类似条件下，EXISTS 和 NOT EXISTS 子句可以
 * 通过上拉 sub-SELECT 并建立半连接或反半连接来处理。
 *
 * 本例程搜索这类子句，若找到则做必要的解析树
 * 变换。
 *
 * 本例程必须在 preprocess_expression() 之前运行，因此限定
 * 条件子句尚未归约为隐式 AND 形式，也不保证
 * 已经 AND/OR 展平。这意味着我们需要递归搜索
 * 显式 AND 子句。一遇到非 AND 项就停止。
 */
void
pull_up_sublinks(PlannerInfo *root)
{
	Node	   *jtnode;
	Relids		relids;

	/* 开始在连接树上递归 */
	jtnode = pull_up_sublinks_jointree_recurse(root,
											   (Node *) root->parse->jointree,
											   &relids);

	/*
	 * root->parse->jointree 必须始终是 FromExpr，因此若递归得到裸节点，就插入一个哑的
	 * FromExpr；对象是裸 RangeTblRef 或 JoinExpr。
	 */
	if (IsA(jtnode, FromExpr))
		root->parse->jointree = (FromExpr *) jtnode;
	else
		root->parse->jointree = makeFromExpr(list_make1(jtnode), NULL);
}

/*
 * 为 pull_up_sublinks() 在连接树节点上递归
 *
 * 除了返回可能已修改的连接树节点，还把所含关系的
 * relid 集合返回到 *relids。
 */
static Node *
pull_up_sublinks_jointree_recurse(PlannerInfo *root, Node *jtnode,
								  Relids *relids)
{
	/* 由于本函数递归，可能被逼到栈溢出。 */
	check_stack_depth();

	if (jtnode == NULL)
	{
		*relids = NULL;
	}
	else if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		*relids = bms_make_singleton(varno);
		/* jtnode 原样返回 */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *newfromlist = NIL;
		Relids		frelids = NULL;
		FromExpr   *newf;
		Node	   *jtlink;
		ListCell   *l;

		/* 先递归处理子节点并收集它们的 relid */
		foreach(l, f->fromlist)
		{
			Node	   *newchild;
			Relids		childrelids;

			newchild = pull_up_sublinks_jointree_recurse(root,
														 lfirst(l),
														 &childrelids);
			newfromlist = lappend(newfromlist, newchild);
			frelids = bms_join(frelids, childrelids);
		}
		/* 构造替换用的 FromExpr；尚无限定条件 */
		newf = makeFromExpr(newfromlist, NULL);
		/* 建立代表重建后连接树的链接 */
		jtlink = (Node *) newf;
		/* 现在处理限定条件——所有子节点都可供使用 */
		newf->quals = pull_up_sublinks_qual_recurse(root, f->quals,
													&jtlink, frelids,
													NULL, NULL);

		/*
		 * 注意结果要么是 newf，要么是以 newf 为底的一叠 JoinExprs。
		 * 我们依靠后续步骤
		 * 把这叠展平并按需要重排连接。
		 *
		 * 虽然可以把上拉的子查询放进返回的
		 * relid，但没有必要，因为上层限定条件反正无法引用它们的
		 * 输出。
		 */
		*relids = frelids;
		jtnode = jtlink;
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j;
		Relids		leftrelids;
		Relids		rightrelids;
		Node	   *jtlink;

		/*
		 * 复制一份可修改的连接节点，但先不复制它的
		 * 子节点。
		 */
		j = (JoinExpr *) palloc(sizeof(JoinExpr));
		memcpy(j, jtnode, sizeof(JoinExpr));
		jtlink = (Node *) j;

		/* 递归处理子节点并收集它们的 relid */
		j->larg = pull_up_sublinks_jointree_recurse(root, j->larg,
													&leftrelids);
		j->rarg = pull_up_sublinks_jointree_recurse(root, j->rarg,
													&rightrelids);

		/*
		 * 现在处理限定条件，把适当的子 relid 标为可用，
		 * 并把上拉得到的连接树项挂到正确位置。在
		 * 内连接情况下，我们把新的 JoinExprs 放在现有连接之上（很像
		 * FromExpr 风格的连接）。在外连接情况下，新的
		 * JoinExprs 必须进入外连接的可空一侧。
		 * available_rels 这套处理的目的是确保只
		 * 上拉那些这样做没问题的限定条件。
		 *
		 * 这里不应当看到任何预先存在的 JOIN_SEMI、JOIN_ANTI 或
		 * JOIN_RIGHT_ANTI 连接类型。
		 */
		switch (j->jointype)
		{
			case JOIN_INNER:
				j->quals = pull_up_sublinks_qual_recurse(root, j->quals,
														 &jtlink,
														 bms_union(leftrelids,
																   rightrelids),
														 NULL, NULL);
				break;
			case JOIN_LEFT:
				j->quals = pull_up_sublinks_qual_recurse(root, j->quals,
														 &j->rarg,
														 rightrelids,
														 NULL, NULL);
				break;
			case JOIN_FULL:
				/* 对全连接的限定条件无能为力 */
				break;
			case JOIN_RIGHT:
				j->quals = pull_up_sublinks_qual_recurse(root, j->quals,
														 &j->larg,
														 leftrelids,
														 NULL, NULL);
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				break;
		}

		/*
		 * 虽然可以把上拉的子查询放进返回的
		 * relid，但没有必要，因为上层限定条件反正无法引用它们的
		 * 输出。但我们确实需要纳入该连接自身的 rtindex，
		 * 因为还没有折叠连接别名变量，否则上层
		 * 会误以为无法使用对本连接的
		 * 引用。
		 */
		*relids = bms_join(leftrelids, rightrelids);
		if (j->rtindex)
			*relids = bms_add_member(*relids, j->rtindex);
		jtnode = jtlink;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return jtnode;
}

/*
 * 为 pull_up_sublinks() 在顶层限定条件节点上递归
 *
 * jtlink1 指向连接树中应当插入新 JoinExprs 的链接，
 * 条件是它们引用 available_rels1（即 available_rels1
 * 表示 jtlink1 之下存在的关系）。可选地，jtlink2 可以
 * 指向第二条链接，若新 JoinExprs
 * 引用 available_rels2 就插到那里（不用时这两个参数都传 NULL）。
 * 注意同时引用两套变量的 SubLinks 无法被优化。
 * 若找到多个可上拉的 SubLinks，它们会按遇到的顺序堆到 jtlink1
 * 和/或 jtlink2 上。我们依靠后续
 * 处理在合适时重排这叠节点。
 *
 * 返回替换后的限定条件节点；若该限定条件应删除则返回 NULL。
 */
static Node *
pull_up_sublinks_qual_recurse(PlannerInfo *root, Node *node,
							  Node **jtlink1, Relids available_rels1,
							  Node **jtlink2, Relids available_rels2)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		JoinExpr   *j;
		Relids		child_rels;

		/* 它是可转换的 ANY 或 EXISTS 子句吗？ */
		if (sublink->subLinkType == ANY_SUBLINK)
		{
			if ((j = convert_ANY_sublink_to_join(root, sublink,
												 available_rels1)) != NULL)
			{
				/* 是；把新连接节点插入连接树 */
				j->larg = *jtlink1;
				*jtlink1 = (Node *) j;
				/* 递归处理上拉得到的连接树节点 */
				j->rarg = pull_up_sublinks_jointree_recurse(root,
															j->rarg,
															&child_rels);

				/*
				 * 现在递归处理上拉得到的限定条件。任何插入的
				 * 连接都可以堆到 j->larg 或 j->rarg 上，
				 * 取决于它们引用哪些关系。
				 */
				j->quals = pull_up_sublinks_qual_recurse(root,
														 j->quals,
														 &j->larg,
														 available_rels1,
														 &j->rarg,
														 child_rels);
				/* 返回表示常量 TRUE 的 NULL */
				return NULL;
			}
			if (available_rels2 != NULL &&
				(j = convert_ANY_sublink_to_join(root, sublink,
												 available_rels2)) != NULL)
			{
				/* 是；把新连接节点插入连接树 */
				j->larg = *jtlink2;
				*jtlink2 = (Node *) j;
				/* 递归处理上拉得到的连接树节点 */
				j->rarg = pull_up_sublinks_jointree_recurse(root,
															j->rarg,
															&child_rels);

				/*
				 * 现在递归处理上拉得到的限定条件。任何插入的
				 * 连接都可以堆到 j->larg 或 j->rarg 上，
				 * 取决于它们引用哪些关系。
				 */
				j->quals = pull_up_sublinks_qual_recurse(root,
														 j->quals,
														 &j->larg,
														 available_rels2,
														 &j->rarg,
														 child_rels);
				/* 返回表示常量 TRUE 的 NULL */
				return NULL;
			}
		}
		else if (sublink->subLinkType == EXISTS_SUBLINK)
		{
			if ((j = convert_EXISTS_sublink_to_join(root, sublink, false,
													available_rels1)) != NULL)
			{
				/* 是；把新连接节点插入连接树 */
				j->larg = *jtlink1;
				*jtlink1 = (Node *) j;
				/* 递归处理上拉得到的连接树节点 */
				j->rarg = pull_up_sublinks_jointree_recurse(root,
															j->rarg,
															&child_rels);

				/*
				 * 现在递归处理上拉得到的限定条件。任何插入的
				 * 连接都可以堆到 j->larg 或 j->rarg 上，
				 * 取决于它们引用哪些关系。
				 */
				j->quals = pull_up_sublinks_qual_recurse(root,
														 j->quals,
														 &j->larg,
														 available_rels1,
														 &j->rarg,
														 child_rels);
				/* 返回表示常量 TRUE 的 NULL */
				return NULL;
			}
			if (available_rels2 != NULL &&
				(j = convert_EXISTS_sublink_to_join(root, sublink, false,
													available_rels2)) != NULL)
			{
				/* 是；把新连接节点插入连接树 */
				j->larg = *jtlink2;
				*jtlink2 = (Node *) j;
				/* 递归处理上拉得到的连接树节点 */
				j->rarg = pull_up_sublinks_jointree_recurse(root,
															j->rarg,
															&child_rels);

				/*
				 * 现在递归处理上拉得到的限定条件。任何插入的
				 * 连接都可以堆到 j->larg 或 j->rarg 上，
				 * 取决于它们引用哪些关系。
				 */
				j->quals = pull_up_sublinks_qual_recurse(root,
														 j->quals,
														 &j->larg,
														 available_rels2,
														 &j->rarg,
														 child_rels);
				/* 返回表示常量 TRUE 的 NULL */
				return NULL;
			}
		}
		/* 否则原样返回 */
		return node;
	}
	if (is_notclause(node))
	{
		/* 若 NOT 的直接参数是 EXISTS，尝试转换 */
		SubLink    *sublink = (SubLink *) get_notclausearg((Expr *) node);
		JoinExpr   *j;
		Relids		child_rels;

		if (sublink && IsA(sublink, SubLink))
		{
			if (sublink->subLinkType == EXISTS_SUBLINK)
			{
				if ((j = convert_EXISTS_sublink_to_join(root, sublink, true,
														available_rels1)) != NULL)
				{
					/* 是；把新连接节点插入连接树 */
					j->larg = *jtlink1;
					*jtlink1 = (Node *) j;
					/* 递归处理上拉得到的连接树节点 */
					j->rarg = pull_up_sublinks_jointree_recurse(root,
																j->rarg,
																&child_rels);

					/*
					 * 现在递归处理上拉得到的限定条件。因为
					 * 我们处在 NOT 之下，无法上拉引用左侧内容的
					 * 子链接，但上拉引用 j->rarg 的子链接
					 * 仍然可以。
					 */
					j->quals = pull_up_sublinks_qual_recurse(root,
															 j->quals,
															 &j->rarg,
															 child_rels,
															 NULL, NULL);
					/* 返回表示常量 TRUE 的 NULL */
					return NULL;
				}
				if (available_rels2 != NULL &&
					(j = convert_EXISTS_sublink_to_join(root, sublink, true,
														available_rels2)) != NULL)
				{
					/* 是；把新连接节点插入连接树 */
					j->larg = *jtlink2;
					*jtlink2 = (Node *) j;
					/* 递归处理上拉得到的连接树节点 */
					j->rarg = pull_up_sublinks_jointree_recurse(root,
																j->rarg,
																&child_rels);

					/*
					 * 现在递归处理上拉得到的限定条件。因为
					 * 我们处在 NOT 之下，无法上拉引用左侧内容的
					 * 子链接，但上拉引用 j->rarg 的子链接
					 * 仍然可以。
					 */
					j->quals = pull_up_sublinks_qual_recurse(root,
															 j->quals,
															 &j->rarg,
															 child_rels,
															 NULL, NULL);
					/* 返回表示常量 TRUE 的 NULL */
					return NULL;
				}
			}
		}
		/* 否则原样返回 */
		return node;
	}
	if (is_andclause(node))
	{
		/* 递归进入 AND 子句 */
		List	   *newclauses = NIL;
		ListCell   *l;

		foreach(l, ((BoolExpr *) node)->args)
		{
			Node	   *oldclause = (Node *) lfirst(l);
			Node	   *newclause;

			newclause = pull_up_sublinks_qual_recurse(root,
													  oldclause,
													  jtlink1,
													  available_rels1,
													  jtlink2,
													  available_rels2);
			if (newclause)
				newclauses = lappend(newclauses, newclause);
		}
		/* 返回的子句可能比开始时少 */
		if (newclauses == NIL)
			return NULL;
		else if (list_length(newclauses) == 1)
			return (Node *) linitial(newclauses);
		else
			return (Node *) make_andclause(newclauses);
	}
	/* 若不是 AND 则停止 */
	return node;
}

/*
 * preprocess_function_rtes
 *		对 FROM 子句中的 FUNCTION RTEs 做常量简化，然后
 *		尝试把其中的集合返回函数内联。
 *
 * 若 RTE_FUNCTION 范围表项调用的集合返回函数
 * 只含一条简单 SELECT，可以把该范围表项转换成
 * 直接暴露该 SELECT 的 RTE_SUBQUERY 项。若子查询随后还能
 * 上拉以继续处理，这尤其有用，
 * 但即使不上拉我们也这样做，以减少执行器开销。
 *
 * 这必须在开始对子查询做任何处理之前完成，
 * 否则这些步骤不会作用到
 * 通过内联得到的子查询上。不过我们在 pull_up_sublinks
 * 之后做，以便内联 SubLink 子查询中使用的函数。
 *
 * 在此阶段做常量简化的原因是：
 * (a) 为了内联 SRF 反正也要做，(b) 现在做可以
 * 确保 pull_up_constant_function() 在有常量时能看到常量。
 * 这种做法还保证
 * 每个 FUNCTION RTE 都已做常量简化，从而允许 planner.c 的
 * preprocess_expression() 跳过再做一次。
 *
 * 和规划器的大部分一样，本函数可以随意就地修改其输入数据
 * 结构。
 */
void
preprocess_function_rtes(PlannerInfo *root)
{
	ListCell   *rt;

	foreach(rt, root->parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

		if (rte->rtekind == RTE_FUNCTION)
		{
			Query	   *funcquery;

			/* 做常量简化 */
			rte->functions = (List *)
				eval_const_expressions(root, (Node *) rte->functions);

			/* 检查展开是否安全，若可能则展开 */
			funcquery = inline_set_returning_function(root, rte);
			if (funcquery)
			{
				/* 展开成功，把 RTE 转换成子查询 */
				rte->rtekind = RTE_SUBQUERY;
				rte->subquery = funcquery;
				rte->security_barrier = false;

				/*
				 * 清除子查询 RTE 中不该设置的字段。
				 * 不过暂时保留 rte->functions，
				 * 以防 makeWholeRowVar 需要查阅它。我们将在
				 * setrefs.c 中清除它（见 add_rte_to_flat_rtable），以免这种
				 * 对数据结构的滥用逃出规划器。
				 */
				rte->funcordinality = false;
			}
		}
	}
}

/*
 * pull_up_subqueries
 *		在范围表中寻找可以上拉进
 *		父查询的子查询。若子查询没有分组/聚集之类的特殊特性，
 *		就可以把它并入父查询的连接树。
 *		另外，简单 UNION ALL 结构的子查询可以
 *		转换成 "append 关系"。
 */
void
pull_up_subqueries(PlannerInfo *root)
{
	/* 连接树顶层必须始终是 FromExpr */
	Assert(IsA(root->parse->jointree, FromExpr));
	/* 递归开始时没有包含它的连接，也没有 appendrel */
	root->parse->jointree = (FromExpr *)
		pull_up_subqueries_recurse(root, (Node *) root->parse->jointree,
								   NULL, NULL);
	/* 此时仍应有一个 FromExpr */
	Assert(IsA(root->parse->jointree, FromExpr));
}

/*
 * pull_up_subqueries_recurse
 *		pull_up_subqueries 的递归主体。
 *
 * 本函数递归处理连接树并返回修改后的连接树。
 *
 * 若此连接树节点位于外连接的任一侧，则
 * lowest_outer_join 指向最低的那个 JoinExpr 节点；否则
 * 它为 NULL。我们用它来
 *
 * 约束 LATERAL 子查询的影响。
 * 若正在查看 append 关系的成员子查询，
 * containing_appendrel 描述该关系；否则为 NULL。
 * 这会强制对所有非 Var 的目标列表项使用 PlaceHolderVar 机制，
 *
 * 并对可以上拉的内容加一些额外限制。
 * 这段代码有个棘手之处：若上拉子查询，就必须在整个
 * 父查询中替换引用该子查询输出的 Var，包括挂在当前正在处理的
 * 连接树节点之上的那些节点上的限定条件！我们的办法是在递归时
 * 小心地维持连接树结构的有效性，具体是：
 * 每次递归时，树中所有限定条件表达式都必须能从
 * 顶层到达，以防递归调用需要修改它们。
 *
 * 还要注意，不能把 pullup_replace_vars 放开用到整棵
 * 连接树上，因为它会返回变异后的树副本；我们只能
 * 把它作用在限定条件上。正是这种行为使得
 * 把 lowest_outer_join 当作指针传递是合理的，而不必用某种
 * 更间接的方式标识最低的外连接。同样，我们不
 * 替换 append_rel_list 的成员，只替换它们的子结构，因此
 * containing_appendrel 引用可以安全使用。
 */
static Node *
pull_up_subqueries_recurse(PlannerInfo *root, Node *jtnode,
						   JoinExpr *lowest_outer_join,
						   AppendRelInfo *containing_appendrel)
{
	/* 由于本函数递归，可能被逼到栈溢出。 */
	check_stack_depth();
	/* 另外，因为这有点贵，顺便检查查询是否被取消。 */
	CHECK_FOR_INTERRUPTS();

	Assert(jtnode != NULL);
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, root->parse->rtable);

		/*
		 * 这是子查询 RTE 吗？若是，子查询是否简单到可以
		 * 上拉？
		 *
		 * 若正在查看 append 关系的成员，则不能上拉，
		 * 除非 is_safe_append_member 允许。
		 */
		if (rte->rtekind == RTE_SUBQUERY &&
			is_simple_subquery(root, rte->subquery, rte, lowest_outer_join) &&
			(containing_appendrel == NULL ||
			 is_safe_append_member(rte->subquery)))
			return pull_up_simple_subquery(root, jtnode, rte,
										   lowest_outer_join,
										   containing_appendrel);

		/*
		 * 或者，它是简单的 UNION ALL 子查询吗？若是，展平
		 * 成一个 "append 关系"。
		 *
		 * 无论本查询自身是不是
		 * appendrel 成员，这样做都是安全的。（若你觉得应当把
		 * 两层 appendrel 展平到一起，没错；但那是在
		 * set_append_rel_pathlist 里处理，不在这里。）
		 */
		if (rte->rtekind == RTE_SUBQUERY &&
			is_simple_union_all(rte->subquery))
			return pull_up_simple_union_all(root, jtnode, rte);

		/*
		 * 或者它是简单的 VALUES RTE？
		 *
		 * 不允许在外连接之下或进入
		 * appendrel 时上拉 VALUES（这类情况目前反正不可能）。
		 */
		if (rte->rtekind == RTE_VALUES &&
			lowest_outer_join == NULL &&
			containing_appendrel == NULL &&
			is_simple_values(root, rte))
			return pull_up_simple_values(root, jtnode, rte);

		/*
		 * 或者它是可以内联的 FUNCTION RTE？
		 */
		if (rte->rtekind == RTE_FUNCTION)
			return pull_up_constant_function(root, jtnode, rte,
											 containing_appendrel);

		/* 否则，在此节点什么也不做。 */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		Assert(containing_appendrel == NULL);
		/* 递归变换所有子节点 */
		foreach(l, f->fromlist)
		{
			lfirst(l) = pull_up_subqueries_recurse(root, lfirst(l),
												   lowest_outer_join,
												   NULL);
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		Assert(containing_appendrel == NULL);
		/* 递归，并留意在进入外连接内部时告诉自己 */
		switch (j->jointype)
		{
			case JOIN_INNER:
				j->larg = pull_up_subqueries_recurse(root, j->larg,
													 lowest_outer_join,
													 NULL);
				j->rarg = pull_up_subqueries_recurse(root, j->rarg,
													 lowest_outer_join,
													 NULL);
				break;
			case JOIN_LEFT:
			case JOIN_SEMI:
			case JOIN_ANTI:
				j->larg = pull_up_subqueries_recurse(root, j->larg,
													 j,
													 NULL);
				j->rarg = pull_up_subqueries_recurse(root, j->rarg,
													 j,
													 NULL);
				break;
			case JOIN_FULL:
				j->larg = pull_up_subqueries_recurse(root, j->larg,
													 j,
													 NULL);
				j->rarg = pull_up_subqueries_recurse(root, j->rarg,
													 j,
													 NULL);
				break;
			case JOIN_RIGHT:
				j->larg = pull_up_subqueries_recurse(root, j->larg,
													 j,
													 NULL);
				j->rarg = pull_up_subqueries_recurse(root, j->rarg,
													 j,
													 NULL);
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				break;
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return jtnode;
}

/*
 * pull_up_simple_subquery
 *		尝试上拉单个简单子查询。
 *
 * jtnode 是已被初步认定为简单
 * 子查询的 RangeTblRef，认定者是 pull_up_subqueries。我们返回替换后的连接树节点，
 * 或者在最终判定该子查询不能上拉时
 * 返回 jtnode 本身。
 *
 * rte 是 jtnode 所引用的 RangeTblEntry。其余参数与
 * pull_up_subqueries_recurse 相同。
 */
static Node *
pull_up_simple_subquery(PlannerInfo *root, Node *jtnode, RangeTblEntry *rte,
						JoinExpr *lowest_outer_join,
						AppendRelInfo *containing_appendrel)
{
	Query	   *parse = root->parse;
	int			varno = ((RangeTblRef *) jtnode)->rtindex;
	Query	   *subquery;
	PlannerInfo *subroot;
	int			rtoffset;
	pullup_replace_vars_context rvcontext;
	ListCell   *lc;

	/*
	 * 复制一份可修改的子查询来改，这样若下面判定
	 * 终究不能上拉，RTE 可以
	 * 保持不变。
	 */
	subquery = copyObject(rte->subquery);

	/*
	 * 为这个子查询创建 PlannerInfo 数据结构。
	 *
	 * 注意：下面几步应当与 subquery_planner() 开头的处理一致。
	 * 能否重构以避免代码重复，
	 * 还是那样只会更难看？
	 */
	subroot = makeNode(PlannerInfo);
	subroot->parse = subquery;
	subroot->glob = root->glob;
	subroot->query_level = root->query_level;
	subroot->parent_root = root->parent_root;
	subroot->plan_params = NIL;
	subroot->outer_params = NULL;
	subroot->planner_cxt = CurrentMemoryContext;
	subroot->init_plans = NIL;
	subroot->cte_plan_ids = NIL;
	subroot->multiexpr_params = NIL;
	subroot->join_domains = NIL;
	subroot->eq_classes = NIL;
	subroot->ec_merging_done = false;
	subroot->last_rinfo_serial = 0;
	subroot->all_result_relids = NULL;
	subroot->leaf_result_relids = NULL;
	subroot->append_rel_list = NIL;
	subroot->row_identity_vars = NIL;
	subroot->rowMarks = NIL;
	memset(subroot->upper_rels, 0, sizeof(subroot->upper_rels));
	memset(subroot->upper_targets, 0, sizeof(subroot->upper_targets));
	subroot->processed_groupClause = NIL;
	subroot->processed_distinctClause = NIL;
	subroot->processed_tlist = NIL;
	subroot->update_colnos = NIL;
	subroot->grouping_map = NULL;
	subroot->minmax_aggs = NIL;
	subroot->qual_security_level = 0;
	subroot->placeholdersFrozen = false;
	subroot->hasRecursion = false;
	subroot->wt_param_id = -1;
	subroot->non_recursive_path = NULL;
	/* 目前不需要为 subroot 准备顶层 JoinDomain */

	/* 没有 CTEs 需要操心 */
	Assert(subquery->cteList == NIL);

	/*
	 * 若 FROM 子句为空，用一个哑 RTE_RESULT RTE 替换它，这样
	 * 就不必为这种情况准备那么多特殊分支。
	 */
	replace_empty_jointree(subquery);

	/*
	 * 上拉子查询限定条件中的任何 SubLinks，以免
	 * 留下未经处理的 SubLinks。
	 */
	if (subquery->hasSubLinks)
		pull_up_sublinks(subroot);

	/*
	 * 同样，预处理它的函数 RTEs，把范围表中的集合返回
	 * 函数内联。
	 */
	preprocess_function_rtes(subroot);

	/*
	 * 递归上拉该子查询的子查询，使
	 * pull_up_subqueries 对其连接树和
	 * 范围表的处理完成。
	 *
	 * 注意：子查询的递归以 NULL 作为
	 * 包含连接的信息开始是可以的，即使我们处在上层
	 * 查询的外连接之内；下层查询对外连接语义是从头开始的。
	 * 同样，不必把 appendrel 状态传下去。
	 */
	pull_up_subqueries(subroot);

	/*
	 * 现在必须重新检查子查询是否仍然简单到可以上拉。
	 * 若否，放弃处理它。
	 *
	 * 其实不必重新检查全部条件，但
	 * 让这个 "if" 与 pull_up_subqueries_recurse 中的那个
	 * 看起来一样更容易。
	 */
	if (is_simple_subquery(root, subquery, rte, lowest_outer_join) &&
		(containing_appendrel == NULL || is_safe_append_member(subquery)))
	{
		/* 可以继续 */
	}
	else
	{
		/*
		 * 放弃，返回未修改的 RangeTblRef。
		 *
		 * 注意：刚才做的工作会在子查询单独
		 * 规划时重做。也许可以把
		 * 修改后的子查询存回范围表来避免重做，
		 * 但现在我不想冒这个险。
		 */
		return jtnode;
	}

	/*
	 * 必须展平子查询目标列表中的任何连接别名 Var，
	 * 因为上拉该子查询的子查询可能已把它们的
	 * 展开式改成任意表达式，这会影响
	 * pullup_replace_vars 关于 tlist 项是否需要 PlaceHolderVar 包装的
	 * 决定。（更好的做法也许是在更早阶段对整棵查询树做
	 * flatten_join_alias_vars，
	 * 甚至在重写器里；但眼下只在这里修这个情况。）
	 */
	subquery->targetList = (List *)
		flatten_join_alias_vars(subroot, subroot->parse,
								(Node *) subquery->targetList);

	/*
	 * 调整子查询中层次 0 的 varno，以便把它的范围表
	 * 追加到上层查询。还必须修正子查询的
	 * append_rel_list。
	 */
	rtoffset = list_length(parse->rtable);
	OffsetVarNodes((Node *) subquery, rtoffset, 0);
	OffsetVarNodes((Node *) subroot->append_rel_list, rtoffset, 0);

	/*
	 * 子查询中的上层 var 现在比以前离它们的父查询
	 * 近了一层。
	 */
	IncrementVarSublevelsUp((Node *) subquery, -1, 1);
	IncrementVarSublevelsUp((Node *) subroot->append_rel_list, -1, 1);

	/*
	 * 子查询的目标列表项现在已适合
	 * 插入顶层查询，只是也许需要用
	 * PlaceHolderVars 包起来。为 pullup_replace_vars 准备所需的上下文。
	 * （注意应当把子查询的内连接纳入 relid，
	 * 因为它可能含有引用它们的连接别名 var。）
	 */
	rvcontext.root = root;
	rvcontext.targetlist = subquery->targetList;
	rvcontext.target_rte = rte;
	if (rte->lateral)
	{
		rvcontext.relids = get_relids_in_jointree((Node *) subquery->jointree,
												  true, true);
		rvcontext.nullinfo = get_nullingrels(parse);
	}
	else						/* 不会用到这些值 */
	{
		rvcontext.relids = NULL;
		rvcontext.nullinfo = NULL;
	}
	rvcontext.outer_hasSubLinks = &parse->hasSubLinks;
	rvcontext.varno = varno;
	/* 若需要，下面会设置此标志 */
	rvcontext.wrap_non_vars = false;
	/* 用下标 0 .. length(tlist) 初始化缓存数组 */
	rvcontext.rv_cache = palloc0((list_length(subquery->targetList) + 1) *
								 sizeof(Node *));

	/*
	 * 若处理的是 appendrel 成员，则任何不是
	 * 简单 Var 的东西都必须变成 PlaceHolderVar。我们强制这样做，
	 * 以确保上拉的内容在后续处理中不会并入周围的
	 * 表达式，然后无法匹配
	 * appendrel 实际能提供的表达式。
	 */
	if (containing_appendrel != NULL)
		rvcontext.wrap_non_vars = true;

	/*
	 * 若父查询使用分组集，任何不是简单 Var 的东西都需要 PlaceHolderVar。
	 * 这同样确保表达式
	 * 保持各自的身份，以便在合适时匹配分组集
	 * 列。（只包装分组集
	 * 列中使用的值，并且只在 tlist 和 havingQual 的非聚集部分这样做就够了，
	 * 但那需要大量基础设施，
	 * 而 pullup_replace_vars 目前还没有。）
	 */
	if (parse->groupingSets)
		rvcontext.wrap_non_vars = true;

	/*
	 * 把顶层查询中对子查询输出的全部引用替换为
	 * 调整后的 subtlist 项的副本，小心不要
	 * 替换连接树结构的任何部分。
	 */
	perform_pullup_replace_vars(root, &rvcontext,
								containing_appendrel);

	/*
	 * 若子查询带有 LATERAL 标记，把它传播到任何
	 * 现在可能含有 LATERAL 交叉引用的子 RTEs。
	 * 这些子项也许含有、也许不含有实际的 LATERAL
	 * 交叉引用，但必须给上拉后的子 RTEs 做标记，以便
	 * 后续规划器阶段检查这种情况。
	 */
	if (rte->lateral)
	{
		foreach(lc, subquery->rtable)
		{
			RangeTblEntry *child_rte = (RangeTblEntry *) lfirst(lc);

			switch (child_rte->rtekind)
			{
				case RTE_RELATION:
					if (child_rte->tablesample)
						child_rte->lateral = true;
					break;
				case RTE_SUBQUERY:
				case RTE_FUNCTION:
				case RTE_VALUES:
				case RTE_TABLEFUNC:
					child_rte->lateral = true;
					break;
				case RTE_JOIN:
				case RTE_CTE:
				case RTE_NAMEDTUPLESTORE:
				case RTE_RESULT:
					/* 这些里面不可能有 LATERAL 引用 */
					break;
			}
		}
	}

	/*
	 * 现在把调整后的 rtable 项及其 perminfo 追加到上层
	 * 查询。（我们等到修正上层 rtable 项之后再做；
	 * 没必要对子查询的那些项也跑那段代码。）
	 */
	CombineRangeTables(&parse->rtable, &parse->rteperminfos,
					   subquery->rtable, subquery->rteperminfos);

	/*
	 * 也上拉任何 FOR UPDATE/SHARE 标记。（OffsetVarNodes 已经
	 * 调整了标记的 rtindex，所以只需拼接列表。）
	 */
	parse->rowMarks = list_concat(parse->rowMarks, subquery->rowMarks);

	/*
	 * 还必须修正父查询中任何 PlaceHolderVar 节点的 relid 集合。
	 * （这也许可以由 pullup_replace_vars() 完成，
	 * 但分两遍似乎更干净。）特别要注意，任何
	 * 刚由 pullup_replace_vars() 创建的 PlaceHolderVar 节点都会被
	 * 调整，因此用子查询的 varno 创建它们是对的。
	 *
	 * 同样，出现在 AppendRelInfo 节点中的 relid 也必须修正。我们
	 * 已经检查过，这不需要把多个 subrelid
	 * 引入单槽的 AppendRelInfo 结构。
	 */
	if (root->glob->lastPHId != 0 || root->append_rel_list)
	{
		Relids		subrelids;

		subrelids = get_relids_in_jointree((Node *) subquery->jointree,
										   true, false);
		if (root->glob->lastPHId != 0)
			substitute_phv_relids((Node *) parse, varno, subrelids);
		fix_append_rel_relids(root, varno, subrelids);
	}

	/*
	 * 现在把子查询的 AppendRelInfos 加入我们的列表。
	 */
	root->append_rel_list = list_concat(root->append_rel_list,
										subroot->append_rel_list);

	/*
	 * 不必对外连接信息做同等的簿记，
	 * 因为那还没建立。placeholder_list 也一样。
	 */
	Assert(root->join_info_list == NIL);
	Assert(subroot->join_info_list == NIL);
	Assert(root->placeholder_list == NIL);
	Assert(subroot->placeholder_list == NIL);

	/*
	 * 不再需要 RTE 中那份子查询的查询树。丢掉它
	 * 对这一层查询本身省不了什么；
	 * 但若这一层查询接着被上拉进父查询，
	 * 复制这棵已不再使用的查询树会浪费周期。
	 */
	rte->subquery = NULL;

	/*
	 * 杂项收拾。
	 *
	 * 虽然 replace_rte_variables() 在从子查询目标列表复制出任何 SubLinks 时，
	 * 忠实地更新了 parse->hasSubLinks，我们仍
	 * 可能在从子查询复制上来的 FUNCTION
	 * 和 VALUES RTEs 的表达式里给查询增加 SubLinks。因此仍然必须复制
	 * subquery->hasSubLinks。也许将来可以改进。
	 */
	parse->hasSubLinks |= subquery->hasSubLinks;

	/* 若子查询有任何 RLS 条件，现在主查询也有 */
	parse->hasRowSecurity |= subquery->hasRowSecurity;

	/*
	 * 若子查询有 hasAggs、hasWindowFuncs 或
	 * hasTargetSRFs，它不会被上拉，因此这些标志无需处理
	 */

	/*
	 * 返回调整后的子查询连接树，以替换父连接树中的 RangeTblRef 项；
	 * 或者，若 FromExpr 已退化，只返回
	 * 它的唯一成员。
	 */
	Assert(IsA(subquery->jointree, FromExpr));
	Assert(subquery->jointree->fromlist != NIL);
	if (subquery->jointree->quals == NULL &&
		list_length(subquery->jointree->fromlist) == 1)
		return (Node *) linitial(subquery->jointree->fromlist);

	return (Node *) subquery->jointree;
}

/*
 * pull_up_simple_union_all
 *		上拉单个简单的 UNION ALL 子查询。
 *
 * jtnode 是已被认定为简单 UNION ALL
 * 子查询的 RangeTblRef，认定者是 pull_up_subqueries。我们上拉叶子子查询并
 * 为该并集建立一个 "append 关系"。返回值就是
 * jtnode，因为实际上不必改查询的连接树。
 */
static Node *
pull_up_simple_union_all(PlannerInfo *root, Node *jtnode, RangeTblEntry *rte)
{
	int			varno = ((RangeTblRef *) jtnode)->rtindex;
	Query	   *subquery = rte->subquery;
	int			rtoffset = list_length(root->parse->rtable);
	List	   *rtable;

	/*
	 * 复制一份可修改的子查询 rtable，以便调整
	 * 其中的上层 Var。setOperations
	 * 树本身没有这种 Var，所以修正 rtable 就够了。
	 */
	rtable = copyObject(subquery->rtable);

	/*
	 * 子查询中的上层 var 现在比以前离它们的父查询近了一层。
	 * 不过不必担心偏移 varno，
	 * 因为 UNION 的叶子查询不能互相交叉引用。
	 */
	IncrementVarSublevelsUp_rtable(rtable, -1, 1);

	/*
	 * 若 UNION ALL 子查询带有 LATERAL 标记，把它传播到所有
	 * 子项。各个子项也许含有、也许不含有
	 * 实际的 LATERAL 交叉引用，但必须给上拉后的
	 * 子 RTEs 做标记，以便后续规划器阶段检查这种情况。
	 */
	if (rte->lateral)
	{
		ListCell   *rt;

		foreach(rt, rtable)
		{
			RangeTblEntry *child_rte = (RangeTblEntry *) lfirst(rt);

			Assert(child_rte->rtekind == RTE_SUBQUERY);
			child_rte->lateral = true;
		}
	}

	/*
	 * 把子 RTEs（及其 perminfo）追加到父 rtable。
	 */
	CombineRangeTables(&root->parse->rtable, &root->parse->rteperminfos,
					   rtable, subquery->rteperminfos);

	/*
	 * 递归扫描子查询的 setOperations 树，为叶子子查询向父查询的
	 * append_rel_list 添加 AppendRelInfo 节点。
	 * 也对叶子子查询应用 pull_up_subqueries。
	 */
	Assert(subquery->setOperations);
	pull_up_union_leaf_queries(subquery->setOperations, root, varno, subquery,
							   rtoffset);

	/*
	 * 把父节点标成 append 关系。
	 */
	rte->inh = true;

	return jtnode;
}

/*
 * pull_up_union_leaf_queries -- pull_up_simple_union_all 的递归主体
 *
 * 为集合运算树中的每个叶子查询建立一个 AppendRelInfo，然后
 * 对叶子查询应用 pull_up_subqueries。
 *
 * 注意 setOpQuery 是包含 setOp 节点的 Query，其 tlist
 * 含有对全部集合运算输出列的引用。当从
 * pull_up_simple_union_all 调用时，它并不是 root->parse，后者才是
 * 我们正在上拉进入的父 Query。
 *
 * parentRTindex 是 appendrel 父项在 root->parse->rtable 中的下标。
 *
 * 子 RTEs 已经复制到父查询。childRToffset
 * 告诉我们它们被复制到父范围表的何处。当从
 * flatten_simple_union_all 调用时，childRToffset 为 0，因为子 RTEs
 * 已经在 root->parse->rtable 中，不需要调整 RT 下标。
 */
static void
pull_up_union_leaf_queries(Node *setOp, PlannerInfo *root, int parentRTindex,
						   Query *setOpQuery, int childRToffset)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		int			childRTindex;
		AppendRelInfo *appinfo;

		/*
		 * 计算在父范围表中的下标
		 */
		childRTindex = childRToffset + rtr->rtindex;

		/*
		 * 建立一个合适的 AppendRelInfo，并挂到父列表上。
		 */
		appinfo = makeNode(AppendRelInfo);
		appinfo->parent_relid = parentRTindex;
		appinfo->child_relid = childRTindex;
		appinfo->parent_reltype = InvalidOid;
		appinfo->child_reltype = InvalidOid;
		make_setop_translation_list(setOpQuery, childRTindex, appinfo);
		appinfo->parent_reloid = InvalidOid;
		root->append_rel_list = lappend(root->append_rel_list, appinfo);

		/*
		 * 对新的子 RTE 递归应用 pull_up_subqueries。（我们
		 * 必须先建立 AppendRelInfo，因为这一步会修改它；
		 * 实际上，此时上层查询中唯一可能存在引用
		 * childRTindex 的 Var 的地方就是它。）
		 *
		 * 注意，即使实际上处在外连接之下，也可以为包含连接的信息传入 NULL，
		 * 因为子表达式
		 * 不会向上传播到该连接。另外，我们忽略
		 * pull_up_subqueries_recurse() 返回与传入不同的
		 * 连接树节点的可能性；若真如此，重要的是
		 * 它替换了 AppendRelInfo 节点中的子 relid。
		 */
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = childRTindex;
		(void) pull_up_subqueries_recurse(root, (Node *) rtr,
										  NULL, appinfo);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/* 递归以到达叶子查询 */
		pull_up_union_leaf_queries(op->larg, root, parentRTindex, setOpQuery,
								   childRToffset);
		pull_up_union_leaf_queries(op->rarg, root, parentRTindex, setOpQuery,
								   childRToffset);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
}

/*
 * make_setop_translation_list
 *	  为 UNION ALL 成员建立从父 Var 到子 Var 的翻译列表。
 *	  （此时它只是一份简单的
 *	  引用 Var 列表，但若成功上拉该成员
 *	  子查询，这些 Var 会被上拉后的表达式替换。）
 *	  同时建立相当平凡的反向翻译数组。
 */
static void
make_setop_translation_list(Query *query, int newvarno,
							AppendRelInfo *appinfo)
{
	List	   *vars = NIL;
	AttrNumber *pcolnos;
	ListCell   *l;

	/* 把反向翻译数组的全部项初始化为零 */
	/* （resjunk 列的项会保持这样） */
	appinfo->num_child_cols = list_length(query->targetList);
	appinfo->parent_colnos = pcolnos =
		(AttrNumber *) palloc0(appinfo->num_child_cols * sizeof(AttrNumber));

	foreach(l, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
			continue;

		vars = lappend(vars, makeVarFromTargetEntry(newvarno, tle));
		pcolnos[tle->resno - 1] = tle->resno;
	}

	appinfo->translated_vars = vars;
}

/*
 * is_simple_subquery
 *	  检查范围表中的子查询是否简单到
 *	  可以上拉进父查询。
 *
 * rte 是包含该子查询的 RTE_SUBQUERY RangeTblEntry。
 * （注意 subquery 不一定等于 rte->subquery；它可能是
 * 后者经过处理的副本。）
 * lowest_outer_join 是子查询之上最低的外连接，或为 NULL。
 */
static bool
is_simple_subquery(PlannerInfo *root, Query *subquery, RangeTblEntry *rte,
				   JoinExpr *lowest_outer_join)
{
	/*
	 * 先确认它是有效的子查询 ...
	 */
	if (!IsA(subquery, Query) ||
		subquery->commandType != CMD_SELECT)
		elog(ERROR, "subquery is bogus");

	/*
	 * 目前不能上拉带集合运算的查询（除非是简单 UNION
	 * ALL，那由另一条代码路径处理）。也许等查询树
	 * 重新设计之后...
	 */
	if (subquery->setOperations)
		return false;

	/*
	 * 不能上拉涉及分组、聚集、SRFs、
	 * 排序、限制或 WITH 的子查询。（XXX WITH 以后也许可以允许）
	 *
	 * 也不上拉带有显式 FOR UPDATE/SHARE
	 * 子句的子查询，因为上拉会使加锁在语义上发生在
	 * 比应有位置更高的地方。隐式 FOR UPDATE/SHARE 可以，因为
	 * 那种情况下锁本来就是在上层查询中
	 * 声明的。
	 */
	if (subquery->hasAggs ||
		subquery->hasWindowFuncs ||
		subquery->hasTargetSRFs ||
		subquery->groupClause ||
		subquery->groupingSets ||
		subquery->havingQual ||
		subquery->sortClause ||
		subquery->distinctClause ||
		subquery->limitOffset ||
		subquery->limitCount ||
		subquery->hasForUpdate ||
		subquery->cteList)
		return false;

	/*
	 * 若 RTE 表示安全屏障视图，不要上拉；一旦 RTE 的 Var 散落到
	 * 上层查询各处，我们就无法阻止
	 * 信息泄漏。
	 */
	if (rte->security_barrier)
		return false;

	/*
	 * 若子查询是 LATERAL，检查由此带来的上拉限制。
	 */
	if (rte->lateral)
	{
		bool		restricted;
		Relids		safe_upper_varnos;

		/*
		 * 子查询的 WHERE 和 JOIN/ON 限定条件不得含有指向
		 * 更高层外连接之外的关系的 LATERAL 引用（包括
		 * 外连接就在子查询内部的情况）。在这种
		 * 情况下，上拉会造成需要把外连接之下的限定条件
		 * 推迟到外连接之上，这
		 * 多半完全是错的，而且无论如何是眼下
		 * 不值得处理的复杂情况。
		 */
		if (lowest_outer_join != NULL)
		{
			restricted = true;
			safe_upper_varnos = get_relids_in_jointree((Node *) lowest_outer_join,
													   true, true);
		}
		else
		{
			restricted = false;
			safe_upper_varnos = NULL;	/* 无关紧要 */
		}

		if (jointree_contains_lateral_outer_refs(root,
												 (Node *) subquery->jointree,
												 restricted, safe_upper_varnos))
			return false;

		/*
		 * 若 LATERAL 子查询之上还有外连接，并且子查询的目标列表有任何指向
		 * 该外连接之外的关系的引用，也禁止
		 * 上拉，因为这些引用可能被拉进
		 * 子查询之上（但在外连接之中或之下）的限定条件，然后导致
		 * 与上面检查过的情况类似的限定条件推迟问题。
		 * （若外层查询的限定条件中没有这种引用，就不必禁止上拉，
		 * 但这里没有足够信息来检查
		 * 这一点。另外，若强制把这种引用包进 PlaceHolderVars，即使它们在
		 * 最近的外连接之下，也许可以去掉这条限制？但这是相当勉强的用法，
		 * 不清楚值不值得费这个劲。）
		 * 若改这里，也请看
		 *
		 * pullup_replace_vars_callback() 里关于 LATERAL 引用的
		 * 注释。
		 */
		if (lowest_outer_join != NULL)
		{
			Relids		lvarnos = pull_varnos_of_level(root,
													   (Node *) subquery->targetList,
													   1);

			if (!bms_is_subset(lvarnos, safe_upper_varnos))
				return false;
		}
	}

	/*
	 * 不要上拉目标列表中含有任何易变函数的子查询。
	 * 否则若这些函数被复制到上层查询的多个地方，
	 * 可能引入多次求值，
	 * 导致意外结果。（注意：PlaceHolderVar 机制
	 * 并不能完全保证只求值一次；否则我们仍可上拉，
	 * 只是把这些项包进 PlaceHolderVars ...）
	 */
	if (contain_volatile_functions((Node *) subquery->targetList))
		return false;

	return true;
}

/*
 * pull_up_simple_values
 *		上拉单个简单的 VALUES RTE。
 *
 * jtnode 是已被 pull_up_subqueries 认定为简单 VALUES RTE 的
 * RangeTblRef。我们总是返回一个表示
 * RESULT RTE 的 RangeTblRef 来替换它（所有失败情况应由
 * is_simple_values() 事先检出）。实际上返回的就是 jtnode，因为
 * 我们是在范围表里用 RESULT RTE 替换 VALUES RTE。
 *
 * rte 是 jtnode 所引用的 RangeTblEntry。由于 VALUES RTEs 的
 * 可能用法有限，不需要 pull_up_subqueries_recurse 的
 * 其余参数。
 */
static Node *
pull_up_simple_values(PlannerInfo *root, Node *jtnode, RangeTblEntry *rte)
{
	Query	   *parse = root->parse;
	int			varno = ((RangeTblRef *) jtnode)->rtindex;
	List	   *values_list;
	List	   *tlist;
	AttrNumber	attrno;
	pullup_replace_vars_context rvcontext;
	ListCell   *lc;

	Assert(rte->rtekind == RTE_VALUES);
	Assert(list_length(rte->values_lists) == 1);

	/*
	 * 需要一份可修改的 VALUES 列表来改，以防它被
	 * 多处引用。
	 */
	values_list = copyObject(linitial(rte->values_lists));

	/*
	 * VALUES RTE 不可能含有层次为零的 Var，更不可能有
	 * 连接别名，因此不必展平连接别名 Var。
	 */
	Assert(!contain_vars_of_level((Node *) values_list, 0));

	/*
	 * 为 pullup_replace_vars 准备所需的上下文。特别是，
	 * 必须让 VALUES 列表看起来像子查询的目标列表。
	 */
	tlist = NIL;
	attrno = 1;
	foreach(lc, values_list)
	{
		tlist = lappend(tlist,
						makeTargetEntry((Expr *) lfirst(lc),
										attrno,
										NULL,
										false));
		attrno++;
	}
	rvcontext.root = root;
	rvcontext.targetlist = tlist;
	rvcontext.target_rte = rte;
	rvcontext.relids = NULL;	/* 这里不可能有 LATERAL 引用 */
	rvcontext.nullinfo = NULL;
	rvcontext.outer_hasSubLinks = &parse->hasSubLinks;
	rvcontext.varno = varno;
	rvcontext.wrap_non_vars = false;
	/* 用下标 0 .. length(tlist) 初始化缓存数组 */
	rvcontext.rv_cache = palloc0((list_length(tlist) + 1) *
								 sizeof(Node *));

	/*
	 * 把顶层查询中对该 RTE 输出的全部引用替换为
	 * 调整后的 VALUES 表达式的副本，小心不要替换
	 * 连接树结构的任何部分。可以假定包围 VALUES RTE 的哑 Query 中
	 * 没有外连接或 appendrel。
	 */
	perform_pullup_replace_vars(root, &rvcontext, NULL);

	/*
	 * 应当没有 appendrel 要修，也没有外连接，因此没有
	 * PlaceHolderVars。
	 */
	Assert(root->append_rel_list == NIL);
	Assert(root->join_info_list == NIL);
	Assert(root->placeholder_list == NIL);

	/*
	 * 用 RESULT RTE 替换 VALUES RTE。VALUES RTE 是
	 * 当前查询层唯一的 rtable 项，所以这很容易。
	 */
	Assert(list_length(parse->rtable) == 1);

	/* 创建合适的 RTE */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RESULT;
	rte->eref = makeAlias("*RESULT*", NIL);

	/* 替换范围表 */
	parse->rtable = list_make1(rte);

	/* 可以新造一个 RangeTblRef，但现有的这个就很好 */
	Assert(varno == 1);

	return jtnode;
}

/*
 * is_simple_values
 *	  检查范围表中的 VALUES RTE 是否简单到
 *	  可以上拉进父查询。
 *
 * rte 是要检查的 RTE_VALUES RangeTblEntry。
 */
static bool
is_simple_values(PlannerInfo *root, RangeTblEntry *rte)
{
	Assert(rte->rtekind == RTE_VALUES);

	/*
	 * 必须恰好有一份 VALUES 列表，否则在语义上
	 * 不能用 RESULT RTE 替换 VALUES RTE，也不会有
	 * 唯一的一组表达式可代入父查询。
	 */
	if (list_length(rte->values_lists) != 1)
		return false;

	/*
	 * 因为 VALUES 不能出现在外连接之下（至少，若出现我们不会
	 * 尝试上拉），所以不必担心 LATERAL，也不必
	 * 担心 VALUES 输出的 PHVs 是否有效。
	 */

	/*
	 * 不要上拉含有任何集合返回或易变
	 * 函数的 VALUES。这里的考虑与
	 * 可上拉子查询的目标列表限制基本相同。
	 */
	if (expression_returns_set((Node *) rte->values_lists) ||
		contain_volatile_functions((Node *) rte->values_lists))
		return false;

	/*
	 * 不要上拉在其父查询中不是唯一 RTE 的 VALUES。
	 * 这实际上是解析器目前会生成的唯一情况，
	 * 假定这一点大大简化了
	 * pull_up_simple_values()。
	 */
	if (list_length(root->parse->rtable) != 1 ||
		rte != (RangeTblEntry *) linitial(root->parse->rtable))
		return false;

	return true;
}

/*
 * pull_up_constant_function
 *		上拉已被简化为常量的 RTE_FUNCTION 表达式。
 *
 * jtnode 是已被 pull_up_subqueries 认定为 FUNCTION RTE 的
 * RangeTblRef。若其表达式只是一个 Const，就把该值提升
 * 到父查询，并用 RTE_RESULT 替换 RTE_FUNCTION。
 *
 * 原则上可以上拉任何不可变表达式，但我们不这么做。
 * 那可能导致表达式被多次求值，若它不只是一个 Const，
 * 代价可能很高。另外，这样做的主要价值是
 * 让常量参与进一步的常量折叠，而
 * 非 Const 当然不会发生折叠。
 *
 * 若该 RTE 位于外连接之下或属于 appendrel，上拉的值也许需要包进 PlaceHolderVar；
 * 额外的
 * 参数表明是否需要这样做。
 */
static Node *
pull_up_constant_function(PlannerInfo *root, Node *jtnode,
						  RangeTblEntry *rte,
						  AppendRelInfo *containing_appendrel)
{
	Query	   *parse = root->parse;
	RangeTblFunction *rtf;
	TypeFuncClass functypclass;
	Oid			funcrettype;
	TupleDesc	tupdesc;
	pullup_replace_vars_context rvcontext;

	/* 若 RTE 带 ORDINALITY 则失败——这里没有实现。 */
	if (rte->funcordinality)
		return jtnode;

	/* 若 RTE 不是单个简单的 Const 表达式则失败 */
	if (list_length(rte->functions) != 1)
		return jtnode;
	rtf = linitial_node(RangeTblFunction, rte->functions);
	if (!IsA(rtf->funcexpr, Const))
		return jtnode;

	/*
	 * 若函数结果不是标量，就放弃。原则上可以
	 * 把复合常量值拆成按列的
	 * 常量，但眼下似乎不值得做。
	 */
	if (rtf->funccolcount != 1)
		return jtnode;			/* 肯定是复合类型 */

	/* 若有 coldeflist，它一定返回 RECORD */
	if (rtf->funccolnames != NIL)
		return jtnode;			/* 必须是单列 RECORD 类型 */

	functypclass = get_expr_result_type(rtf->funcexpr,
										&funcrettype,
										&tupdesc);
	if (functypclass != TYPEFUNC_SCALAR)
		return jtnode;			/* 必须是单列复合类型 */

	/* 创建用于应用 pullup_replace_vars 的上下文 */
	rvcontext.root = root;
	rvcontext.targetlist = list_make1(makeTargetEntry((Expr *) rtf->funcexpr,
													  1,	/* resno（结果编号） */
													  NULL, /* resname（结果名） */
													  false));	/* resjunk（废列） */
	rvcontext.target_rte = rte;

	/*
	 * 由于该函数已简化为 Const，它不含任何
	 * LATERAL 引用，即使被标成 LATERAL。这意味着我们
	 * 不必填写 relids 或 nullinfo。
	 */
	rvcontext.relids = NULL;
	rvcontext.nullinfo = NULL;

	rvcontext.outer_hasSubLinks = &parse->hasSubLinks;
	rvcontext.varno = ((RangeTblRef *) jtnode)->rtindex;
	/* 若需要，下面会设置此标志 */
	rvcontext.wrap_non_vars = false;
	/* 用下标 0 .. length(tlist) 初始化缓存数组 */
	rvcontext.rv_cache = palloc0((list_length(rvcontext.targetlist) + 1) *
								 sizeof(Node *));

	/*
	 * 若处理的是 appendrel 成员，则任何不是
	 * 简单 Var 的东西都必须变成 PlaceHolderVar。（见
	 * pull_up_simple_subquery() 中的注释。）
	 */
	if (containing_appendrel != NULL)
		rvcontext.wrap_non_vars = true;

	/*
	 * 若父查询使用分组集，任何不是简单 Var 的东西
	 * 都需要 PlaceHolderVar。
	 */
	if (parse->groupingSets)
		rvcontext.wrap_non_vars = true;

	/*
	 * 把顶层查询中对该 RTE 输出的全部引用替换为
	 * funcexpr 的副本，小心不要替换
	 * 连接树结构的任何部分。
	 */
	perform_pullup_replace_vars(root, &rvcontext,
								containing_appendrel);

	/*
	 * 不必费心去改父查询中的 PlaceHolderVars。
	 * 它们对 RT 下标的引用目前仍然有效，并且
	 * 若以后能丢掉 RTE_RESULT，这些引用会被去掉。
	 */

	/*
	 * 把 RTE 转成 RTE_RESULT 类型，表示不再需要
	 * 扫描它，并把 RTE_FUNCTION 专用字段清零。同时
	 * 确保该 RTE 没有标成 LATERAL，因为别处不预期
	 * RTE_RESULTs 是 LATERAL。
	 */
	rte->rtekind = RTE_RESULT;
	rte->functions = NIL;
	rte->lateral = false;

	/*
	 * 可以复用这个 RangeTblRef 节点。
	 */
	return jtnode;
}

/*
 * is_simple_union_all
 *	  检查子查询是否为简单的 UNION ALL。
 *
 * 我们需要全部集合运算都是 UNION ALL（不能混合），并且不能有
 * 任何数据类型强制转换，也就是所有叶子查询必须产生
 * 相同的数据类型。
 */
static bool
is_simple_union_all(Query *subquery)
{
	SetOperationStmt *topop;

	/* 先确认它是有效的子查询 ... */
	if (!IsA(subquery, Query) ||
		subquery->commandType != CMD_SELECT)
		elog(ERROR, "subquery is bogus");

	/* 它到底是不是集合运算查询？ */
	topop = castNode(SetOperationStmt, subquery->setOperations);
	if (!topop)
		return false;

	/* 不能处理 ORDER BY、LIMIT/OFFSET、加锁或 WITH */
	if (subquery->sortClause ||
		subquery->limitOffset ||
		subquery->limitCount ||
		subquery->rowMarks ||
		subquery->cteList)
		return false;

	/* 递归检查集合运算树 */
	return is_simple_union_all_recurse((Node *) topop, subquery,
									   topop->colTypes);
}

static bool
is_simple_union_all_recurse(Node *setOp, Query *setOpQuery, List *colTypes)
{
	/* 由于本函数递归，可能被逼到栈溢出。 */
	check_stack_depth();

	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, setOpQuery->rtable);
		Query	   *subquery = rte->subquery;

		Assert(subquery != NULL);

		/* 叶子节点若与顶层列类型匹配就可以 */
		/* 这里不必比较 typmod 或排序规则 */
		return tlist_same_datatypes(subquery->targetList, colTypes, true);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/* 必须是 UNION ALL */
		if (op->op != SETOP_UNION || !op->all)
			return false;

		/* 递归检查输入 */
		return is_simple_union_all_recurse(op->larg, setOpQuery, colTypes) &&
			is_simple_union_all_recurse(op->rarg, setOpQuery, colTypes);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
		return false;			/* 消除编译器告警 */
	}
}

/*
 * is_safe_append_member
 *	  检查作为 UNION ALL appendrel 叶子的子查询，看它
 *	  上拉是否安全。
 */
static bool
is_safe_append_member(Query *subquery)
{
	FromExpr   *jtnode;

	/*
	 * 只有当子查询的连接树恰好含有
	 * 一个 RTE 时，上拉才安全，否则 AppendRelInfo 数据结构会坏。那一个基表 RTE
	 * 可以被埋在好几层 FromExpr 里。另外，若
	 * 子查询的连接树完全为空，也可以上拉，因为
	 * pull_up_simple_subquery 会改为插入单个 RTE_RESULT RTE。
	 *
	 * 另外，子查询不能有任何 WHERE 限定条件，因为 appendrel 里
	 * 没有地方放它们。（这有点烦人...）若不必
	 * 检查这一点，我们只需测试 get_relids_in_jointree()
	 * 是否得到单元素集合，以便与
	 * fix_append_rel_relids() 的写法更一致。
	 */
	jtnode = subquery->jointree;
	Assert(IsA(jtnode, FromExpr));
	/* 检查完全为空的情况 */
	if (jtnode->fromlist == NIL && jtnode->quals == NULL)
		return true;
	/* 检查更一般的情况 */
	while (IsA(jtnode, FromExpr))
	{
		if (jtnode->quals != NULL)
			return false;
		if (list_length(jtnode->fromlist) != 1)
			return false;
		jtnode = linitial(jtnode->fromlist);
	}
	if (!IsA(jtnode, RangeTblRef))
		return false;

	return true;
}

/*
 * jointree_contains_lateral_outer_refs
 *		检查连接树限定条件中是否有不允许的 LATERAL 引用
 *
 * 若 restricted 为假，所有层次 1 的 Var 都允许（但仍然必须
 * 搜索连接树，因为它可能含有外连接，其下会有
 * 限制）。若 restricted 为真，当连接树中任何限定条件
 * 含有来自 safe_upper_varnos 所列关系之外的层次 1 Var 时，
 * 返回真。
 */
static bool
jointree_contains_lateral_outer_refs(PlannerInfo *root, Node *jtnode,
									 bool restricted,
									 Relids safe_upper_varnos)
{
	if (jtnode == NULL)
		return false;
	if (IsA(jtnode, RangeTblRef))
		return false;
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		/* 先递归检查子连接 */
		foreach(l, f->fromlist)
		{
			if (jointree_contains_lateral_outer_refs(root,
													 lfirst(l),
													 restricted,
													 safe_upper_varnos))
				return true;
		}

		/* 然后检查顶层限定条件 */
		if (restricted &&
			!bms_is_subset(pull_varnos_of_level(root, f->quals, 1),
						   safe_upper_varnos))
			return true;
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/*
		 * 若这是外连接，不得允许任何上层 LATERAL
		 * 引用出现在它之中或之下。
		 */
		if (j->jointype != JOIN_INNER)
		{
			restricted = true;
			safe_upper_varnos = NULL;
		}

		/* 检查子连接 */
		if (jointree_contains_lateral_outer_refs(root,
												 j->larg,
												 restricted,
												 safe_upper_varnos))
			return true;
		if (jointree_contains_lateral_outer_refs(root,
												 j->rarg,
												 restricted,
												 safe_upper_varnos))
			return true;

		/* 检查 JOIN 的限定条件子句 */
		if (restricted &&
			!bms_is_subset(pull_varnos_of_level(root, j->quals, 1),
						   safe_upper_varnos))
			return true;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return false;
}

/*
 * 在查询树中每一处需要的地方执行 pullup_replace_vars。
 *
 * 调用者已经在 *rvcontext 中填好了描述用什么来
 * 替换引用目标子查询的 Var 的数据。此外
 * 若有包含它的 appendrel，还需要其身份。
 */
static void
perform_pullup_replace_vars(PlannerInfo *root,
							pullup_replace_vars_context *rvcontext,
							AppendRelInfo *containing_appendrel)
{
	Query	   *parse = root->parse;
	ListCell   *lc;

	/*
	 * 若考虑的是 appendrel 的子查询（也就是正在上拉的 UNION ALL
	 * 成员查询），则上层查询中目前还可能引用该子项的唯一部分是
	 * 相关联 AppendRelInfo 的 translated_vars 列表。
	 * 而且，我们不想在 AppendRelInfo 中强制使用
	 * PHVs——两者之间没有任何外连接。
	 */
	if (containing_appendrel)
	{
		bool		save_wrap_non_vars = rvcontext->wrap_non_vars;

		rvcontext->wrap_non_vars = false;
		containing_appendrel->translated_vars = (List *)
			pullup_replace_vars((Node *) containing_appendrel->translated_vars,
								rvcontext);
		rvcontext->wrap_non_vars = save_wrap_non_vars;
		return;
	}

	/*
	 * 把顶层查询中对子查询输出的全部引用替换为
	 * 调整后的 subtlist 项的副本，小心不要
	 * 替换连接树结构的任何部分。（若能使用
	 * query_tree_mutator 会干净得多。）必须在 targetList、
	 * returningList 和 havingQual 中使用 PHVs，因为它们肯定在任何
	 * 外连接之上。replace_vars_in_jointree 跟踪自己在
	 * 连接树中的位置，并适当地使用或不使用 PHVs。
	 */
	parse->targetList = (List *)
		pullup_replace_vars((Node *) parse->targetList, rvcontext);
	parse->returningList = (List *)
		pullup_replace_vars((Node *) parse->returningList, rvcontext);

	foreach(lc, parse->windowClause)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);

		if (wc->runCondition != NIL)
			wc->runCondition = (List *)
				pullup_replace_vars((Node *) wc->runCondition, rvcontext);
	}
	if (parse->onConflict)
	{
		parse->onConflict->onConflictSet = (List *)
			pullup_replace_vars((Node *) parse->onConflict->onConflictSet,
								rvcontext);
		parse->onConflict->onConflictWhere =
			pullup_replace_vars(parse->onConflict->onConflictWhere,
								rvcontext);

		/*
		 * 假定 ON CONFLICT 的 arbiterElems、arbiterWhere、exclRelTlist
		 * 不可能含有对子查询的任何引用。
		 */
	}
	if (parse->mergeActionList)
	{
		foreach(lc, parse->mergeActionList)
		{
			MergeAction *action = lfirst(lc);

			action->qual = pullup_replace_vars(action->qual, rvcontext);
			action->targetList = (List *)
				pullup_replace_vars((Node *) action->targetList, rvcontext);
		}
	}
	replace_vars_in_jointree((Node *) parse->jointree, rvcontext);
	Assert(parse->setOperations == NULL);
	parse->havingQual = pullup_replace_vars(parse->havingQual, rvcontext);

	/*
	 * 替换 appendrel 的 translated_vars 列表中的引用。
	 */
	foreach(lc, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(lc);

		appinfo->translated_vars = (List *)
			pullup_replace_vars((Node *) appinfo->translated_vars, rvcontext);
	}

	/*
	 * 替换连接 RTEs 的 joinaliasvars 列表中的引用。
	 */
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *otherrte = (RangeTblEntry *) lfirst(lc);

		if (otherrte->rtekind == RTE_JOIN)
			otherrte->joinaliasvars = (List *)
				pullup_replace_vars((Node *) otherrte->joinaliasvars,
									rvcontext);
	}
}

/*
 * perform_pullup_replace_vars 的辅助例程：对连接树中的
 * 每个表达式做 pullup_replace_vars，而不改变连接树结构
 * 本身。难看，但没有别的办法...
 */
static void
replace_vars_in_jointree(Node *jtnode,
						 pullup_replace_vars_context *context)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		/*
		 * 若 RangeTblRef 指向一个 LATERAL 子查询（不是
		 * 正在上拉的那个），它可能含有对
		 * 目标子查询的引用，必须替换。我们从
		 * 连接树扫描来驱动这件事，而不是扫描 rtable，以便
		 * 避免处理已不再被引用的 RTEs。
		 */
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		if (varno != context->varno)	/* 忽略目标子查询本身 */
		{
			RangeTblEntry *rte = rt_fetch(varno, context->root->parse->rtable);

			Assert(rte != context->target_rte);
			if (rte->lateral)
			{
				switch (rte->rtekind)
				{
					case RTE_RELATION:
						/* 除非是 tablesample，否则不应当标成 LATERAL */
						Assert(rte->tablesample);
						rte->tablesample = (TableSampleClause *)
							pullup_replace_vars((Node *) rte->tablesample,
												context);
						break;
					case RTE_SUBQUERY:
						rte->subquery =
							pullup_replace_vars_subquery(rte->subquery,
														 context);
						break;
					case RTE_FUNCTION:
						rte->functions = (List *)
							pullup_replace_vars((Node *) rte->functions,
												context);
						break;
					case RTE_TABLEFUNC:
						rte->tablefunc = (TableFunc *)
							pullup_replace_vars((Node *) rte->tablefunc,
												context);
						break;
					case RTE_VALUES:
						rte->values_lists = (List *)
							pullup_replace_vars((Node *) rte->values_lists,
												context);
						break;
					case RTE_JOIN:
					case RTE_CTE:
					case RTE_NAMEDTUPLESTORE:
					case RTE_RESULT:
						/* 这些不应当标成 LATERAL */
						Assert(false);
						break;
				}
			}
		}
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			replace_vars_in_jointree(lfirst(l), context);
		f->quals = pullup_replace_vars(f->quals, context);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		bool		save_wrap_non_vars = context->wrap_non_vars;

		replace_vars_in_jointree(j->larg, context);
		replace_vars_in_jointree(j->rarg, context);

		/*
		 * 在全连接的连接限定条件中使用 PHVs。否则，我们
		 * 无法辨认一个上拉后的无 var 表达式来自连接的哪一侧，
		 * 这可能导致根本做不出计划，
		 * 因为没有一条限定条件看起来是可归并或可哈希的
		 * 条件。
		 */
		if (j->jointype == JOIN_FULL)
			context->wrap_non_vars = true;

		j->quals = pullup_replace_vars(j->quals, context);

		context->wrap_non_vars = save_wrap_non_vars;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * 在整棵表达式树上应用上拉变量替换
 *
 * 返回修改后的树副本，因此在需要
 * 就地替换的地方不能用它。
 */
static Node *
pullup_replace_vars(Node *expr, pullup_replace_vars_context *context)
{
	return replace_rte_variables(expr,
								 context->varno, 0,
								 pullup_replace_vars_callback,
								 (void *) context,
								 context->outer_hasSubLinks);
}

static Node *
pullup_replace_vars_callback(Var *var,
							 replace_rte_variables_context *context)
{
	pullup_replace_vars_context *rcon = (pullup_replace_vars_context *) context->callback_arg;
	int			varattno = var->varattno;
	bool		need_phv;
	Node	   *newnode;

	/*
	 * 若待替换的 Var 有非空的
	 * varnullingrels，就需要 PlaceHolderVar（除非下面发现替换表达式是
	 * 一个 Var 或可以只把 nullingrels 加上去的 PlaceHolderVar）。若
	 * 调用者指示所有非 Var/PHV
	 * 替换都需要包装以便识别，也需要一个。
	 */
	need_phv = (var->varnullingrels != NULL) || rcon->wrap_non_vars;

	/*
	 * 若需要 PlaceHolderVars，我们把修改后的表达式缓存在
	 * rcon->rv_cache[] 中。这不是指望在
	 * 本函数内有什么实质性的加速，而是避免生成 ID 不同的相同 PHVs。
	 * 那会导致运行时重复求值，
	 * 并且可能妨碍依赖把对同一子查询输出的不同
	 * 引用认作 equal() 的优化。因此值得
	 * 多花一点力气来避免。
	 *
	 * 缓存项的 phlevelsup = 0 且 phnullingrels = NULL；我们会
	 * 复制它们，并在下面为这个引用点调整那些值。
	 */
	if (need_phv &&
		varattno >= InvalidAttrNumber &&
		varattno <= list_length(rcon->targetlist) &&
		rcon->rv_cache[varattno] != NULL)
	{
		/* Just copy the entry and fall through to adjust phlevelsup etc */
		newnode = copyObject(rcon->rv_cache[varattno]);
	}
	else if (varattno == InvalidAttrNumber)
	{
		/* 必须把整元组引用展开成 RowExpr */
		RowExpr    *rowexpr;
		List	   *colnames;
		List	   *fields;
		bool		save_wrap_non_vars = rcon->wrap_non_vars;
		int			save_sublevelsup = context->sublevels_up;

		/*
		 * 若为一个具名行类型的 var 生成展开式（即这
		 * 是普通关系 RTE），则必须为
		 * 已删除的列包含哑项。若该 var 是 RECORD（即这是 JOIN），则
		 * 省略已删除的列。后一种情况下，把列名附到
		 * RowExpr 上，供执行器和 ruleutils.c 使用。
		 *
		 * 为了能缓存结果，我们总是生成
		 * varlevelsup = 0 的展开式，然后在下面按需要调整。
		 */
		expandRTE(rcon->target_rte,
				  var->varno, 0 /* 不是 varlevelsup */ , var->location,
				  (var->vartype != RECORDOID),
				  &colnames, &fields);
		/* 展开生成的逐字段 Var，但不要在那里插入 PHVs */
		rcon->wrap_non_vars = false;
		context->sublevels_up = 0;	/* 以便与 expandRTE 的输出一致 */
		fields = (List *) replace_rte_variables_mutator((Node *) fields,
														context);
		rcon->wrap_non_vars = save_wrap_non_vars;
		context->sublevels_up = save_sublevelsup;

		rowexpr = makeNode(RowExpr);
		rowexpr->args = fields;
		rowexpr->row_typeid = var->vartype;
		rowexpr->row_format = COERCE_IMPLICIT_CAST;
		rowexpr->colnames = (var->vartype == RECORDOID) ? colnames : NIL;
		rowexpr->location = var->location;
		newnode = (Node *) rowexpr;

		/*
		 * 若需要则插入 PlaceHolderVar。注意我们是用一个
		 * PlaceHolderVar 包住整个 RowExpr，而不是给
		 * 行的每个元素各包一个。这是因为当外连接把它
		 * 强制为 null 时，表达式应当得出 NULL，
		 * 而不是 ROW(NULL,NULL,...)。
		 */
		if (need_phv)
		{
			newnode = (Node *)
				make_placeholder_expr(rcon->root,
									  (Expr *) newnode,
									  bms_make_singleton(rcon->varno));
			/* 连同 PHV 一起缓存，且 phlevelsup 等尚未设置 */
			rcon->rv_cache[InvalidAttrNumber] = copyObject(newnode);
		}
	}
	else
	{
		/* 引用一个目标列表元素的正常情况 */
		TargetEntry *tle = get_tle_by_resno(rcon->targetlist, varattno);

		if (tle == NULL)		/* 不应当发生 */
			elog(ERROR, "could not find attribute %d in subquery targetlist",
				 varattno);

		/* 复制一份 tlist 项以返回 */
		newnode = (Node *) copyObject(tle->expr);

		/* 若需要则插入 PlaceHolderVar */
		if (need_phv)
		{
			bool		wrap;

			if (newnode && IsA(newnode, Var) &&
				((Var *) newnode)->varlevelsup == 0)
			{
				/*
				 * 简单 Var 总是免于被包装，除非它们是
				 * 指向正在上拉的子查询之外某处的 LATERAL 引用。
				 * （即使那样，若被引用的关系处在同一个最低外连接之下，也可以省略 PlaceHolderVar，
				 * 但检查这一点
				 * 似乎不值得费事。）
				 */
				if (rcon->target_rte->lateral &&
					!bms_is_member(((Var *) newnode)->varno, rcon->relids))
					wrap = true;
				else
					wrap = false;
			}
			else if (newnode && IsA(newnode, PlaceHolderVar) &&
					 ((PlaceHolderVar *) newnode)->phlevelsup == 0)
			{
				/* PlaceHolderVar 适用同样的规则 */
				if (rcon->target_rte->lateral &&
					!bms_is_subset(((PlaceHolderVar *) newnode)->phrels,
								   rcon->relids))
					wrap = true;
				else
					wrap = false;
			}
			else if (rcon->wrap_non_vars)
			{
				/* 调用者要求把所有非 Var 都包进 PlaceHolderVar */
				wrap = true;
			}
			else
			{
				/*
				 * 若节点含有正在上拉的子查询的 Var 或 PlaceHolderVar，
				 * 并且不含任何
				 * 非严格构造，那么不必在上面再加一个 PHV，
				 * 可以把所需的 nullingrels 加到那些 Var/PHVs 上。
				 * （这本质上是上面那些情况的推广，
				 * 针对裸 Var 和 PHVs。）
				 *
				 * 这项检查有些贵，但可以避免在
				 * nullingrels 后来又被外连接简化
				 * 去掉的情况下把计划做差。
				 *
				 * 注意，只要表达式也含有子查询的 Var/PHVs，
				 * 我们就不强制包装含有 LATERAL 引用的表达式。
				 * 这样做没问题，是因为限制为
				 * 严格构造：若子查询的 Var/PHVs 已被
				 * 外连接强制为 NULL，则表达式的最终结果
				 * 也会是 NULL，与 LATERAL
				 * 引用无关。因此不必强制该表达式
				 * 在外连接之下求值。这可能是非常
				 * 有价值的优化，因为它也许让我们不必
				 * 用嵌套循环把 LATERAL 引用传下去。
				 *
				 * 这个分析还可以更紧：特别是，藏在更低层 PlaceHolderVar 里的非严格
				 * 构造并不是
				 * 再加一个 PHV 的理由。但眼下似乎
				 * 不值得为更精确而增加代码。
				 *
				 * 对于 LATERAL 子查询，必须检查节点实际的 var
				 * 成员；若它不是 LATERAL，则任何
				 * 层次为零的 var 必定属于该子查询。
				 */
				if ((rcon->target_rte->lateral ?
					 bms_overlap(pull_varnos(rcon->root, newnode),
								 rcon->relids) :
					 contain_vars_of_level(newnode, 0)) &&
					!contain_nonstrict_functions(newnode))
				{
					/* 不需要包装 */
					wrap = false;
				}
				else
				{
					/* 否则把它包进 PlaceHolderVar */
					wrap = true;
				}
			}

			if (wrap)
			{
				newnode = (Node *)
					make_placeholder_expr(rcon->root,
										  (Expr *) newnode,
										  bms_make_singleton(rcon->varno));

				/*
				 * 若可能则缓存（即 attno 在范围内，
				 * 这大概总是应当如此）。
				 */
				if (varattno > InvalidAttrNumber &&
					varattno <= list_length(rcon->targetlist))
					rcon->rv_cache[varattno] = copyObject(newnode);
			}
		}
	}

	/* 把任何 varnullingrels 传播进替换表达式 */
	if (var->varnullingrels != NULL)
	{
		if (IsA(newnode, Var))
		{
			Var		   *newvar = (Var *) newnode;

			Assert(newvar->varlevelsup == 0);
			newvar->varnullingrels = bms_add_members(newvar->varnullingrels,
													 var->varnullingrels);
		}
		else if (IsA(newnode, PlaceHolderVar))
		{
			PlaceHolderVar *newphv = (PlaceHolderVar *) newnode;

			Assert(newphv->phlevelsup == 0);
			newphv->phnullingrels = bms_add_members(newphv->phnullingrels,
													var->varnullingrels);
		}
		else
		{
			/*
			 * 表达式中应当有我们可以
			 * 修改的 Var/PHVs。子查询的 Var/PHVs 应当加上完整的
			 * var->varnullingrels，但若表达式中有 LATERAL
			 * 引用，那些引用只能标上
			 * 可能适用于它们的 nullingrels。（这
			 * 对应于表达式现在将在我们正在替换的那个 Var 的连接层
			 * 求值：
			 * LATERAL 引用向上冒泡时经过的外
			 * 连接可能比子查询的 Var 更少。按上面的讨论，
			 * 结果仍然是对的。）那组 relid 对
			 * 不同的 LATERAL 关系可能不同，因此必须对
			 * 每一个分别做这项工作。
			 *
			 * （目前 is_simple_subquery() 中的限制意味着
			 * 最多只需从 LATERAL 引用的 nullingrels 中去掉
			 * 最低外连接的 relid。不过将来也许会
			 * 放宽那些限制，所以还是做对。）
			 */
			if (rcon->target_rte->lateral)
			{
				nullingrel_info *nullinfo = rcon->nullinfo;
				Relids		lvarnos;
				int			lvarno;

				/*
				 * 识别 newnode 中使用的 LATERAL varno。必须在
				 * 把 var->varnullingrels 注入树之前做这件事。
				 */
				lvarnos = pull_varnos(rcon->root, newnode);
				lvarnos = bms_del_members(lvarnos, rcon->relids);
				/* 对每一个，若有相关的 nullingrels 就加上 */
				lvarno = -1;
				while ((lvarno = bms_next_member(lvarnos, lvarno)) >= 0)
				{
					Relids		lnullingrels;

					Assert(lvarno > 0 && lvarno <= nullinfo->rtlength);
					lnullingrels = bms_intersect(var->varnullingrels,
												 nullinfo->nullingrels[lvarno]);
					if (!bms_is_empty(lnullingrels))
						newnode = add_nulling_relids(newnode,
													 bms_make_singleton(lvarno),
													 lnullingrels);
				}
			}

			/* 最后处理子查询自身的 Var/PHVs */
			newnode = add_nulling_relids(newnode,
										 rcon->relids,
										 var->varnullingrels);
			/* 断言我们已经把 varnullingrels 放进了表达式 */
			Assert(bms_is_subset(var->varnullingrels,
								 pull_varnos(rcon->root, newnode)));
		}
	}

	/* 若被替换的 Var 位于子查询内，必须调整 varlevelsup */
	if (var->varlevelsup > 0)
		IncrementVarSublevelsUp(newnode, var->varlevelsup, 0);

	return newnode;
}

/*
 * 对子查询应用上拉变量替换
 *
 * 这必须与 pullup_replace_vars() 不同，因为
 * replace_rte_variables 会认为在进入 Query 之前不应当增加 sublevels_up；
 * 因此需要以 sublevels_up == 1 调用它。
 */
static Query *
pullup_replace_vars_subquery(Query *query,
							 pullup_replace_vars_context *context)
{
	Assert(IsA(query, Query));
	return (Query *) replace_rte_variables((Node *) query,
										   context->varno, 1,
										   pullup_replace_vars_callback,
										   (void *) context,
										   NULL);
}


/*
 * flatten_simple_union_all
 *		尝试把顶层 UNION ALL 结构优化成 appendrel
 *
 * 若查询的 setOperations 树完全由简单的 UNION ALL
 * 运算组成，把它展平成 append 关系，我们可以比
 * 一般集合运算情形更聪明地处理它。否则什么也不做。
 *
 * 多数情况下，这只对顶层查询能成功，因为对于
 * FROM 中的子查询，父查询调用 pull_up_subqueries 时
 * 已经通过 pull_up_simple_union_all 展平了 UNION。但有
 * 少数情形可以在这里支持、却不能在那条路径上支持，例如
 * 子查询还含有 ORDER BY 时。
 */
void
flatten_simple_union_all(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	SetOperationStmt *topop;
	Node	   *leftmostjtnode;
	int			leftmostRTI;
	RangeTblEntry *leftmostRTE;
	int			childRTI;
	RangeTblEntry *childRTE;
	RangeTblRef *rtr;

	/* 除非查询有集合运算，否则不应当调用 */
	topop = castNode(SetOperationStmt, parse->setOperations);
	Assert(topop);

	/* 无法优化掉递归 UNION */
	if (root->hasRecursion)
		return;

	/*
	 * 递归检查集合运算树。若并非全部是列类型相同的 UNION ALL，
	 * 就放弃。
	 */
	if (!is_simple_union_all_recurse((Node *) topop, parse, topop->colTypes))
		return;

	/*
	 * 定位集合运算树中最左边的叶子查询。上层查询的
	 * Var 都引用这个 RTE（见 transformSetOperationStmt）。
	 */
	leftmostjtnode = topop->larg;
	while (leftmostjtnode && IsA(leftmostjtnode, SetOperationStmt))
		leftmostjtnode = ((SetOperationStmt *) leftmostjtnode)->larg;
	Assert(leftmostjtnode && IsA(leftmostjtnode, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) leftmostjtnode)->rtindex;
	leftmostRTE = rt_fetch(leftmostRTI, parse->rtable);
	Assert(leftmostRTE->rtekind == RTE_SUBQUERY);

	/*
	 * 复制最左边的 RTE 并加入 rtable。这份副本
	 * 将代表最左边的叶子查询，身份是
	 * appendrel 的成员。原件将代表整个 appendrel。
	 * （必须这样做，因为上层查询的 Var 必须被
	 * 看作引用整个 appendrel。）
	 */
	childRTE = copyObject(leftmostRTE);
	parse->rtable = lappend(parse->rtable, childRTE);
	childRTI = list_length(parse->rtable);

	/* 修改集合运算树，使它引用子副本 */
	((RangeTblRef *) leftmostjtnode)->rtindex = childRTI;

	/* 修改原先最左边的 RTE，把它标成 appendrel 父项 */
	leftmostRTE->inh = true;

	/*
	 * 为 appendrel 形成一个 RangeTblRef，并插入 FROM。集合运算树的顶层
	 * Query 最初应当有一个空的 FromClause。
	 */
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = leftmostRTI;
	Assert(parse->jointree->fromlist == NIL);
	parse->jointree->fromlist = list_make1(rtr);

	/*
	 * 现在假装该查询没有集合运算。必须在尝试
	 * 上拉子查询之前这样做，因为 pull_up_simple_subquery 里有 Assert。
	 */
	parse->setOperations = NULL;

	/*
	 * 建立 AppendRelInfo 信息，并对 UNION ALL 的
	 * 叶子查询应用 pull_up_subqueries。（必须现在做，因为它们
	 * 以前没有被连接树引用，因此被
	 * pull_up_subqueries 的主调用漏掉了。）
	 */
	pull_up_union_leaf_queries((Node *) topop, root, leftmostRTI, parse, 0);
}


/*
 * reduce_outer_joins
 *		尝试把外连接简化为普通内连接。
 *
 * 这里的想法是，给定如下查询
 *		SELECT ... FROM a LEFT JOIN b ON (...) WHERE b.y = 42;
 * 若 WHERE 中的 "=" 操作符是严格的，就可以把 LEFT JOIN 简化为普通 JOIN。
 * 严格操作符总会返回 NULL，从而使外层
 * WHERE 失败，发生在 LEFT JOIN 为 b 的
 * 列填入 NULLs 的任何行上。因此连接根本不必产生空值扩展的
 * 行——这使它成为普通连接而不是外连接。
 * （手写查询里这种情形也许不太常见，但
 * 把限定条件下推进复杂视图时相当常见。）
 *
 * 更一般地，若限定条件树中在外连接之上有一条
 * 严格限定条件，把来自连接可空一侧的 Var
 * 约束为非空，就可以降低该外连接的强度。（对 FULL 连接，这
 * 分别适用于每一侧。）
 *
 * 这里还做另一种变换，用来识别如下情形
 *		SELECT ... FROM a LEFT JOIN b ON (a.x = b.y) WHERE b.y IS NULL;
 * 若连接子句对 b.y 严格，则只有空值扩展行能够
 * 通过上层 WHERE，于是可以断定查询真正
 * 指定的是反半连接。我们把连接类型从 JOIN_LEFT
 * 改为 JOIN_ANTI。于是 IS NULL 子句变得多余，必须
 * 去掉以防错误的选择率计算，但我们把它留给
 * distribute_qual_to_rels 来去掉这类子句。
 *
 * 另外，我们通过把 JOIN_RIGHT 翻过来变成
 * JOIN_LEFT 来消除这些情形。这省下这里和后面一些规划器例程中的代码；
 * 主要好处是减少可以出现在
 * SpecialJoinInfo 节点中的连接类型数目。注意仍然可以生成使用
 * JOIN_RIGHT（或 JOIN_RIGHT_ANTI）的 Path 和 Plan，办法是再次交换输入。
 *
 * 为便于识别严格限定条件子句，我们需要本例程在
 * 表达式预处理之后运行（即限定条件规范化以及 JOIN
 * 别名 var 展开之后）。
 */
void
reduce_outer_joins(PlannerInfo *root)
{
	reduce_outer_joins_pass1_state *state1;
	reduce_outer_joins_pass2_state state2;
	ListCell   *lc;

	/*
	 * 为避免对超出必要的限定条件做严格性检查，我们希望
	 * 一旦当前点之下不再有外连接，就停止沿连接树下降。
	 * 这一考虑迫使过程分成两遍。
	 * 第一遍收集每个连接子句每一侧之下出现哪些基关系，
	 * 以及每个连接子句每一侧之下是否有外
	 * 连接。第二遍检查
	 * 限定条件子句，并在下降树的过程中改变连接类型。
	 */
	state1 = reduce_outer_joins_pass1((Node *) root->parse->jointree);

	/* 没有外连接时，planner.c 不应当调用本函数 */
	if (state1 == NULL || !state1->contains_outer)
		elog(ERROR, "so where are the outer joins?");

	state2.inner_reduced = NULL;
	state2.partial_reduced = NIL;

	reduce_outer_joins_pass2((Node *) root->parse->jointree,
							 state1, &state2,
							 root, NULL, NIL);

	/*
	 * 若成功降低了任何外连接的强度，必须
	 * 去掉把这些连接当作置空关系的引用。这作为
	 * 额外的一遍来处理，既为简单，也因为可以在
	 * 对解析树的单次扫描中处理全部完全简化的连接。
	 */
	if (!bms_is_empty(state2.inner_reduced))
	{
		root->parse = (Query *)
			remove_nulling_relids((Node *) root->parse,
								  state2.inner_reduced,
								  NULL);
		/* append_rel_list 里也可能有引用 */
		root->append_rel_list = (List *)
			remove_nulling_relids((Node *) root->append_rel_list,
								  state2.inner_reduced,
								  NULL);
	}

	/*
	 * 部分简化的全连接必须一次做一个，因为
	 * 它们各自需要不同的 except_relids 设置。
	 */
	foreach(lc, state2.partial_reduced)
	{
		reduce_outer_joins_partial_state *statep = lfirst(lc);
		Relids		full_join_relids = bms_make_singleton(statep->full_join_rti);

		root->parse = (Query *)
			remove_nulling_relids((Node *) root->parse,
								  full_join_relids,
								  statep->unreduced_side);
		root->append_rel_list = (List *)
			remove_nulling_relids((Node *) root->append_rel_list,
								  full_join_relids,
								  statep->unreduced_side);
	}
}

/*
 * reduce_outer_joins_pass1 - 第一阶段数据收集
 *
 * 返回描述给定连接树节点的状态节点。
 */
static reduce_outer_joins_pass1_state *
reduce_outer_joins_pass1(Node *jtnode)
{
	reduce_outer_joins_pass1_state *result;

	result = (reduce_outer_joins_pass1_state *)
		palloc(sizeof(reduce_outer_joins_pass1_state));
	result->relids = NULL;
	result->contains_outer = false;
	result->sub_states = NIL;

	if (jtnode == NULL)
		return result;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		result->relids = bms_make_singleton(varno);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			reduce_outer_joins_pass1_state *sub_state;

			sub_state = reduce_outer_joins_pass1(lfirst(l));
			result->relids = bms_add_members(result->relids,
											 sub_state->relids);
			result->contains_outer |= sub_state->contains_outer;
			result->sub_states = lappend(result->sub_states, sub_state);
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		reduce_outer_joins_pass1_state *sub_state;

		/* 结果的 relids 中不需要连接自身的 RT 下标 */
		if (IS_OUTER_JOIN(j->jointype))
			result->contains_outer = true;

		sub_state = reduce_outer_joins_pass1(j->larg);
		result->relids = bms_add_members(result->relids,
										 sub_state->relids);
		result->contains_outer |= sub_state->contains_outer;
		result->sub_states = lappend(result->sub_states, sub_state);

		sub_state = reduce_outer_joins_pass1(j->rarg);
		result->relids = bms_add_members(result->relids,
										 sub_state->relids);
		result->contains_outer |= sub_state->contains_outer;
		result->sub_states = lappend(result->sub_states, sub_state);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return result;
}

/*
 * reduce_outer_joins_pass2 - 第二阶段处理
 *
 *	jtnode：当前连接树节点
 *	state1：第一阶段为该节点收集的状态数据
 *	state2：用来累积成功简化的连接的信息
 *	root：顶层规划器状态
 *	nonnullable_rels：被上层限定条件强制为非空的基表 relid 集合
 *	forced_null_vars：被上层限定条件强制为 null 的 Var 的 multibitmapset
 *
 * 在 state2 中返回成功简化的外连接信息。
 * 完全简化为内连接的连接都加入
 * state2->inner_reduced。若全连接被简化为左连接，
 * 它需要自己在 state2->partial_reduced 中的一项，因为那将
 * 需要专门处理，以便只去掉正确的 nullingrel 标记。
 */
static void
reduce_outer_joins_pass2(Node *jtnode,
						 reduce_outer_joins_pass1_state *state1,
						 reduce_outer_joins_pass2_state *state2,
						 PlannerInfo *root,
						 Relids nonnullable_rels,
						 List *forced_null_vars)
{
	/*
	 * 第二遍不应当下降到空子节点或基关系，
	 * 因为它只在标了 contains_outer 的子树上被调用。
	 */
	if (jtnode == NULL)
		elog(ERROR, "reached empty jointree");
	if (IsA(jtnode, RangeTblRef))
		elog(ERROR, "reached base rel");
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;
		ListCell   *s;
		Relids		pass_nonnullable_rels;
		List	   *pass_forced_null_vars;

		/* 扫描限定条件，看能否增加任何约束 */
		pass_nonnullable_rels = find_nonnullable_rels(f->quals);
		pass_nonnullable_rels = bms_add_members(pass_nonnullable_rels,
												nonnullable_rels);
		pass_forced_null_vars = find_forced_null_vars(f->quals);
		pass_forced_null_vars = mbms_add_members(pass_forced_null_vars,
												 forced_null_vars);
		/* 然后递归——但只进入有意思的子树 */
		Assert(list_length(f->fromlist) == list_length(state1->sub_states));
		forboth(l, f->fromlist, s, state1->sub_states)
		{
			reduce_outer_joins_pass1_state *sub_state = lfirst(s);

			if (sub_state->contains_outer)
				reduce_outer_joins_pass2(lfirst(l), sub_state,
										 state2, root,
										 pass_nonnullable_rels,
										 pass_forced_null_vars);
		}
		bms_free(pass_nonnullable_rels);
		/* 可惜没法这么容易地清理 var 列表 */
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		int			rtindex = j->rtindex;
		JoinType	jointype = j->jointype;
		reduce_outer_joins_pass1_state *left_state = linitial(state1->sub_states);
		reduce_outer_joins_pass1_state *right_state = lsecond(state1->sub_states);

		/* 能简化这个连接吗？ */
		switch (jointype)
		{
			case JOIN_INNER:
				break;
			case JOIN_LEFT:
				if (bms_overlap(nonnullable_rels, right_state->relids))
					jointype = JOIN_INNER;
				break;
			case JOIN_RIGHT:
				if (bms_overlap(nonnullable_rels, left_state->relids))
					jointype = JOIN_INNER;
				break;
			case JOIN_FULL:
				if (bms_overlap(nonnullable_rels, left_state->relids))
				{
					if (bms_overlap(nonnullable_rels, right_state->relids))
						jointype = JOIN_INNER;
					else
					{
						jointype = JOIN_LEFT;
						/* 也在 state2 中报告部分简化 */
						report_reduced_full_join(state2, rtindex,
												 right_state->relids);
					}
				}
				else
				{
					if (bms_overlap(nonnullable_rels, right_state->relids))
					{
						jointype = JOIN_RIGHT;
						/* 也在 state2 中报告部分简化 */
						report_reduced_full_join(state2, rtindex,
												 left_state->relids);
					}
				}
				break;
			case JOIN_SEMI:
			case JOIN_ANTI:

				/*
				 * 这些只可能由 pull_up_sublinks 引入，
				 * 所以上层限定条件不可能引用它们的
				 * 右侧，检查没有意义。我们不预期
				 * 此时会看到 JOIN_RIGHT_ANTI。
				 */
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) jointype);
				break;
		}

		/*
		 * 把 JOIN_RIGHT 转换成 JOIN_LEFT。注意在我们把
		 * JOIN_FULL 简化为 JOIN_RIGHT 的情况下，JoinExpr 将不再
		 * 匹配为表示合并后的连接变量而建立的任何 CoalesceExpr 的内部顺序。
		 * 目前我们不在乎这一点，
		 * 但要当心 ...
		 */
		if (jointype == JOIN_RIGHT)
		{
			Node	   *tmparg;

			tmparg = j->larg;
			j->larg = j->rarg;
			j->rarg = tmparg;
			jointype = JOIN_LEFT;
			right_state = linitial(state1->sub_states);
			left_state = lsecond(state1->sub_states);
		}

		/*
		 * 看能否把 JOIN_LEFT 简化为 JOIN_ANTI。若
		 * 连接自身的限定条件对任何被更高层限定条件强制为 null 的 var 是严格的，
		 * 就是这种情况。注意：还有别的办法可以
		 * 检测反连接，特别是若检查来自 RHS 的 Var
		 * 是否因表约束而必须非空。
		 * 不过那显得复杂而昂贵（特别是，必须
		 * 当心更低层的外连接）。眼下这
		 * 似乎足够了。
		 */
		if (jointype == JOIN_LEFT)
		{
			List	   *nonnullable_vars;
			Bitmapset  *overlap;

			/* 找出 j->quals 中在连接行里必须非空的 Var */
			nonnullable_vars = find_nonnullable_vars(j->quals);

			/*
			 * 只检查 nonnullable_vars 与
			 * forced_null_vars 是否重叠是不够的：需要知道重叠
			 * 是否包含任何 RHS 变量。
			 */
			overlap = mbms_overlap_sets(nonnullable_vars, forced_null_vars);
			if (bms_overlap(overlap, right_state->relids))
				jointype = JOIN_ANTI;
		}

		/*
		 * 若连接类型有变化，同时应用到连接树节点和 RTE。
		 * 另外，若把某个 RTE 改成了 INNER，把它的 RTI 加入 inner_reduced。
		 */
		if (rtindex && jointype != j->jointype)
		{
			RangeTblEntry *rte = rt_fetch(rtindex, root->parse->rtable);

			Assert(rte->rtekind == RTE_JOIN);
			Assert(rte->jointype == j->jointype);
			rte->jointype = jointype;
			if (jointype == JOIN_INNER)
				state2->inner_reduced = bms_add_member(state2->inner_reduced,
													   rtindex);
		}
		j->jointype = jointype;

		/* 只有下面还有事可做时才递归 */
		if (left_state->contains_outer || right_state->contains_outer)
		{
			Relids		local_nonnullable_rels;
			List	   *local_forced_null_vars;
			Relids		pass_nonnullable_rels;
			List	   *pass_forced_null_vars;

			/*
			 * 若此连接（现在）是内连接，可以把它的限定条件提供的任何约束
			 * 加到从上面得到的那些约束上。但若它是外连接，
			 * 只能把局部约束下传到可空
			 * 一侧，因为外连接从不消除其
			 * 不可空一侧的任何行。另外，把上层
			 * 约束传进可空一侧没有意义，因为若有的话
			 * 我们已经能简化该连接。（对于上层的
			 * 强制为 null 的约束，不得把它们传进
			 * 可空一侧——它们要么已在这里起作用，要么没有。）结论
			 * 是我们把局部约束或上层约束之一传给外连接的子节点，
			 * 绝不同时传两者。
			 *
			 * 注意 SEMI 连接在这里像内连接：可以
			 * 同时下传局部约束和上层约束。（不可能有
			 * 影响其内侧的上层约束，但
			 * 不值得为避免传递它们而另开一条代码路径。）
			 *
			 * 在 FULL 连接处我们直接放弃，什么也不下传——是否
			 * 有可能更聪明？
			 */
			if (jointype != JOIN_FULL)
			{
				local_nonnullable_rels = find_nonnullable_rels(j->quals);
				local_forced_null_vars = find_forced_null_vars(j->quals);
				if (jointype == JOIN_INNER || jointype == JOIN_SEMI)
				{
					/* 可以合并上层约束与局部约束 */
					local_nonnullable_rels = bms_add_members(local_nonnullable_rels,
															 nonnullable_rels);
					local_forced_null_vars = mbms_add_members(local_forced_null_vars,
															  forced_null_vars);
				}
			}
			else
			{
				/* 计算这些没有用 */
				local_nonnullable_rels = NULL;
				local_forced_null_vars = NIL;
			}

			if (left_state->contains_outer)
			{
				if (jointype == JOIN_INNER || jointype == JOIN_SEMI)
				{
					/* 传递局部约束与上层约束的并集 */
					pass_nonnullable_rels = local_nonnullable_rels;
					pass_forced_null_vars = local_forced_null_vars;
				}
				else if (jointype != JOIN_FULL) /* 即 LEFT 或 ANTI */
				{
					/* 不能把局部约束传到不可空一侧 */
					pass_nonnullable_rels = nonnullable_rels;
					pass_forced_null_vars = forced_null_vars;
				}
				else
				{
					/* 没有约束能穿过 JOIN_FULL */
					pass_nonnullable_rels = NULL;
					pass_forced_null_vars = NIL;
				}
				reduce_outer_joins_pass2(j->larg, left_state,
										 state2, root,
										 pass_nonnullable_rels,
										 pass_forced_null_vars);
			}

			if (right_state->contains_outer)
			{
				if (jointype != JOIN_FULL)	/* 即 INNER/LEFT/SEMI/ANTI */
				{
					/* 按上面的注释传递适当的约束 */
					pass_nonnullable_rels = local_nonnullable_rels;
					pass_forced_null_vars = local_forced_null_vars;
				}
				else
				{
					/* 没有约束能穿过 JOIN_FULL */
					pass_nonnullable_rels = NULL;
					pass_forced_null_vars = NIL;
				}
				reduce_outer_joins_pass2(j->rarg, right_state,
										 state2, root,
										 pass_nonnullable_rels,
										 pass_forced_null_vars);
			}
			bms_free(local_nonnullable_rels);
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/* reduce_outer_joins_pass2 的辅助函数 */
static void
report_reduced_full_join(reduce_outer_joins_pass2_state *state2,
						 int rtindex, Relids relids)
{
	reduce_outer_joins_partial_state *statep;

	statep = palloc(sizeof(reduce_outer_joins_partial_state));
	statep->full_join_rti = rtindex;
	statep->unreduced_side = relids;
	state2->partial_reduced = lappend(state2->partial_reduced, statep);
}


/*
 * remove_useless_result_rtes
 *		尝试从连接树中去掉 RTE_RESULT RTEs。
 *		另外，在可能时省去单子节点的 FromExprs。
 *
 * 可以利用如下知识从连接树去掉 RTE_RESULT 项：
 * RTE_RESULT 恰好返回一行，并且没有输出列。因此，
 * 若它与其他任何东西做内连接，就可以删掉它。对某些外连接情形
 * 也可以优化，详见下文。
 *
 * 这一遍还会在可能时用子节点替换单子节点的 FromExprs。
 * 放在这里而不是更早做是合适的，因为
 * 去掉 RTE_RESULT 可能把多子节点的 FromExpr 减成只有一个
 * 子节点。若这种 FromExpr 的限定条件为空，或者在语义上
 * 可以把限定条件并入父节点的限定条件，就可以去掉它。
 * 去掉不必要的连接树节点有一点微观效率价值，
 * 真正的原因是要消除这种情形：外连接节点的可空一侧
 * 是一个 FromExpr，其唯一子节点是另一个外连接。
 * 为了正确判断这两个外连接能否交换，
 * deconstruct_jointree() 必须把这种 FromExpr 的任何限定条件当作
 * 上层外连接的退化限定条件。最好的办法是
 * 让它们真的成为上层连接的限定条件：丢掉 FromExpr，
 * 并把限定条件提升到上层连接的限定条件中。（注意，
 * 中间的 FromExpr 有多个子节点时没有危险，因为那时
 * 它表示一个没法与上层外连接交换的内连接。）
 * 既然必须做这件事，不妨到处
 * 省去这种 FromExprs。
 *
 * 其中一些优化依赖于识别 FromExprs 和 JoinExprs 的空（恒真）
 * 限定条件。因此在表达式预处理之后再做这一遍是有用的，因为那会
 * 消掉恒真限定条件，使更多情形被识别为
 * 可优化。而且，出现 RTE_RESULT 的通常原因
 * 是我们上拉了子查询或 VALUES 子句，因而很有可能
 * 用常量替换了 Var，使限定条件更可能被
 * 归约为恒真。另外，因为有些优化依赖于
 * 外连接类型，最好先做完 reduce_outer_joins()。
 * 引用 RTE_RESULT RTE 的 PlaceHolderVar 会阻碍这个
 *
 * 过程：必须从 PHV 的 phrels 中去掉该 RTE_RESULT 的 relid，但
 * 不得把 phrels 集合减成空。若会发生这种情况，并且
 * 该 RTE_RESULT 是外连接的直接子节点，就只好放弃
 * 并且不去掉该 RTE_RESULT：没有别的地方可求值这个
 * PlaceHolderVar。（也就是说，在这类情况下 RTE_RESULT 确实有输出
 * 列。）但若 RTE_RESULT 是内连接的直接子节点，
 * 通常可以改 PlaceHolderVar 的 phrels，改为在
 * 内连接处求值它。这是可以的，因为我们真正在乎的只是 PHVs
 * 在正确的外连接之上或之下求值。不过，没法
 * 把 PHV 的求值推迟到使用它的位置之上；因此下面有
 * 一些检查，看输出 PHVs 是否在
 * 另一个连接输入关系中被 LATERAL 引用。
 * 我们以前试图作为 pull_up_subqueries() 的一部分来做这项工作，
 *
 * 潜在可优化的情形就是在那里引入的；但分开做
 * 要简单得多，
 * 也更有效。
 */
void
remove_useless_result_rtes(PlannerInfo *root)
{
	Relids		dropped_outer_joins = NULL;
	ListCell   *cell;

	/* 连接树顶层必须始终是 FromExpr */
	Assert(IsA(root->parse->jointree, FromExpr));
	/* 递归 ... */
	root->parse->jointree = (FromExpr *)
		remove_useless_results_recurse(root,
									   (Node *) root->parse->jointree,
									   NULL,
									   &dropped_outer_joins);
	/* 此时仍应有一个 FromExpr */
	Assert(IsA(root->parse->jointree, FromExpr));

	/*
	 * 若从连接树去掉了任何外连接节点，转一圈
	 * 去掉把这些连接当作置空关系的引用。（上拉出
	 * 被 RESULT 关系替换的原子查询的 PHVs 里
	 * 可能有这种引用。这是正当的，因为我们现在
	 * 知道这种外连接实际上不会把任何东西置空。）我们
	 * 不在主递归期间做这件事，既为简单，也因为可以在
	 * 对解析树的单次扫描中处理全部这种连接。
	 */
	if (!bms_is_empty(dropped_outer_joins))
	{
		root->parse = (Query *)
			remove_nulling_relids((Node *) root->parse,
								  dropped_outer_joins,
								  NULL);
		/* append_rel_list 里也可能有引用 */
		root->append_rel_list = (List *)
			remove_nulling_relids((Node *) root->append_rel_list,
								  dropped_outer_joins,
								  NULL);
	}

	/*
	 * 去掉任何引用 RTE_RESULT RTE 的 PlanRowMark。对于刚刚去掉的
	 * 任何 RTE_RESULT，显然必须这样做。但即使是我们没有去掉的
	 * RTE，对应的标记也可以丢掉：因为该 RTE
	 * 只有一种可能的输出行，EPQ 不必标记并
	 * 恢复那一行。
	 *
	 * 对幸存的 RTE_RESULT RTE 去掉 PlanRowMark 是必要的，不是可选的；
	 * 否则会为该
	 * RTE_RESULT 生成整行 Var，而执行器不支持。
	 */
	foreach(cell, root->rowMarks)
	{
		PlanRowMark *rc = (PlanRowMark *) lfirst(cell);

		if (rt_fetch(rc->rti, root->parse->rtable)->rtekind == RTE_RESULT)
			root->rowMarks = foreach_delete_current(root->rowMarks, cell);
	}
}

/*
 * remove_useless_results_recurse
 *		remove_useless_result_rtes 的递归主体。
 *
 * 本函数递归处理连接树并返回修改后的连接树。
 * 另外，任何被去掉的外连接节点的 RT 下标会加入
 * *dropped_outer_joins。
 *
 * jtnode 是当前连接树节点。若把它的限定条件并入
 * 父节点的限定条件是合法的，parent_quals 应当指向
 * 父节点的限定条件列表；否则为 parent_quals 传入 NULL。
 * （注意有些情况下，parent_quals 指向树上
 * 高出不止一层的父节点的限定条件。）
 */
static Node *
remove_useless_results_recurse(PlannerInfo *root, Node *jtnode,
							   Node **parent_quals,
							   Relids *dropped_outer_joins)
{
	Assert(jtnode != NULL);
	if (IsA(jtnode, RangeTblRef))
	{
		/* 对 RangeTblRef 不能立刻做任何事 */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		Relids		result_relids = NULL;
		ListCell   *cell;

		/*
		 * 只要至少还剩
		 * 一个子节点，就可以从 fromlist 丢掉 RTE_RESULT 关系，因为连接到单行表
		 * 什么也不改变。（但不能丢掉计算某些兄弟需要的 PHV 的 RTE_RESULT。
		 * 下面的清理变换会
		 * 把这些 PHVs 改派到在连接处计算，对
		 * 兄弟来说太晚了。）把这条规则机械化的最简单办法是
		 * 就地修改列表。
		 */
		foreach(cell, f->fromlist)
		{
			Node	   *child = (Node *) lfirst(cell);
			int			varno;

			/* 递归变换子节点，允许它上推限定条件 ... */
			child = remove_useless_results_recurse(root, child,
												   &f->quals,
												   dropped_outer_joins);
			/* ... 再把它塞回树中 */
			lfirst(cell) = child;

			/*
			 * 若它是至少有一个兄弟的 RTE_RESULT，且没有兄弟
			 * 引用依赖的 PHVs，就可以丢掉它。我们还不知道
			 * 内连接最终的 relid 集合会是什么，因此把
			 * PHVs 等的清理推迟到这个循环之后。
			 */
			if (list_length(f->fromlist) > 1 &&
				(varno = get_result_relid(root, child)) != 0 &&
				!find_dependent_phvs_in_jointree(root, (Node *) f, varno))
			{
				f->fromlist = foreach_delete_current(f->fromlist, cell);
				result_relids = bms_add_member(result_relids, varno);
			}
		}

		/*
		 * 若丢掉了任何 RTE_RESULT RTEs，做清理。多于一个时这有点
		 * 低效，但似乎更好的是
		 * 为单 relid 情形优化支持代码。
		 */
		if (result_relids)
		{
			int			varno = -1;

			while ((varno = bms_next_member(result_relids, varno)) >= 0)
				remove_result_refs(root, varno, (Node *) f);
		}

		/*
		 * 若 FromExpr 现在只有一个子节点，看能否省去它。
		 * 若没有限定条件，这总是合法的，但连接树
		 * 顶层除外（因为 Query.jointree 必须指向一个
		 * FromExpr）。否则，若能把限定条件上推到
		 * 父节点，就可以这样做。
		 *
		 * 注意：把这个变换推广成把多子节点的 FromExprs 并入其父
		 * FromExpr 并不太难，
		 * 但那有风险让父连接变得过于昂贵而难以规划。
		 * 我们留给后续处理启发式地决定
		 * 这是否好主意。不过，上拉单个子节点
		 * 总是可以的。
		 */
		if (list_length(f->fromlist) == 1 &&
			f != root->parse->jointree &&
			(f->quals == NULL || parent_quals != NULL))
		{
			/*
			 * 把任何限定条件合并到父节点。它们现在应当已是隐式 AND
			 * 形式，所以只需拼接列表。把
			 * 子限定条件放在前面，理由是它们名义上
			 * 应当更早求值。
			 */
			if (f->quals != NULL)
				*parent_quals = (Node *)
					list_concat(castNode(List, f->quals),
								castNode(List, *parent_quals));
			return (Node *) linitial(f->fromlist);
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		int			varno;

		/*
		 * 首先递归。若连接类型是 INNER，可以把从任一
		 * 子节点上推的 FromExpr 限定条件吸收进本节点，因为这时它
		 * 等价于 FromExpr。当连接类型是 LEFT 时，可以把
		 * 来自 RHS 子节点的限定条件吸收进当前节点，因为它们
		 * 本质上是外连接的退化限定条件。而且，若已经
		 * 被传入 parent_quals 指针，就可以允许 LHS 子节点的限定条件
		 * 被吸收进父节点。（这对确保去掉可交换左连接
		 * 紧下方的单子节点 FromExprs 很重要。）
		 * 对其余连接类型，不能把子限定条件上移，
		 * 或至少没有特别的理由这样做。
		 */
		j->larg = remove_useless_results_recurse(root, j->larg,
												 (j->jointype == JOIN_INNER) ?
												 &j->quals :
												 (j->jointype == JOIN_LEFT) ?
												 parent_quals : NULL,
												 dropped_outer_joins);
		j->rarg = remove_useless_results_recurse(root, j->rarg,
												 (j->jointype == JOIN_INNER ||
												  j->jointype == JOIN_LEFT) ?
												 &j->quals : NULL,
												 dropped_outer_joins);

		/* 应用特定于连接类型的优化规则 */
		switch (j->jointype)
		{
			case JOIN_INNER:

				/*
				 * 内连接等价于 FromExpr，因此若任
				 * 一侧被简化成 RTE_RESULT 关系，就可以用只含另一侧的
				 * FromExpr 替换该连接。
				 * 而且，可以按与上面相同的规则省去那个 FromExpr。
				 * 正如 FromExpr 的情况，若另一输入关系引用任何被标记为要在
				 *
				 * RTE_RESULT 关系处求值的 PHVs，就不能简化，
				 * 因为那种情况下没法
				 * 推迟它们的求值。但只在语法上允许
				 * 另一输入对该
				 * RTE_RESULT 关系有 LATERAL 引用时，
				 * 才需要检查这一点。只有内连接和左连接的
				 * RHSes
				 * 才允许有这种引用。
				 */
				if ((varno = get_result_relid(root, j->larg)) != 0 &&
					!find_dependent_phvs_in_jointree(root, j->rarg, varno))
				{
					remove_result_refs(root, varno, j->rarg);
					if (j->quals != NULL && parent_quals == NULL)
						jtnode = (Node *)
							makeFromExpr(list_make1(j->rarg), j->quals);
					else
					{
						/* 把任何限定条件合并到父节点 */
						if (j->quals != NULL)
							*parent_quals = (Node *)
								list_concat(castNode(List, j->quals),
											castNode(List, *parent_quals));
						jtnode = j->rarg;
					}
				}
				else if ((varno = get_result_relid(root, j->rarg)) != 0)
				{
					remove_result_refs(root, varno, j->larg);
					if (j->quals != NULL && parent_quals == NULL)
						jtnode = (Node *)
							makeFromExpr(list_make1(j->larg), j->quals);
					else
					{
						/* 把任何限定条件合并到父节点 */
						if (j->quals != NULL)
							*parent_quals = (Node *)
								list_concat(castNode(List, j->quals),
											castNode(List, *parent_quals));
						jtnode = j->larg;
					}
				}
				break;
			case JOIN_LEFT:

				/*
				 * 若 RHS 是 RTE_RESULT，可以简化这种情况，有
				 * 两种不同的可能：
				 *
				 * 若限定条件为空（JOIN ON TRUE），则该连接可以
				 * 降强度为普通内连接，因为每一 LHS 行
				 * 必然恰好有一个连接伙伴。因此总是可以
				 * 丢掉 RHS，很像上面 JOIN_INNER 的情况。
				 * （同样，LHS 无法含有指向
				 * RHS 的 LATERAL 引用。）
				 *
				 * 否则，每一 LHS 行仍应
				 * 恰好返回一次，并且由于 RHS 不返回列
				 * （除非有必须在那里求值的 PHVs），我们
				 * 不太在乎它是否被空值扩展。因此在这种
				 * 情况下也可以忽略限定条件并丢掉这个左
				 * 连接。
				 */
				if ((varno = get_result_relid(root, j->rarg)) != 0 &&
					(j->quals == NULL ||
					 !find_dependent_phvs(root, varno)))
				{
					remove_result_refs(root, varno, j->larg);
					*dropped_outer_joins = bms_add_member(*dropped_outer_joins,
														  j->rtindex);
					jtnode = j->larg;
				}
				break;
			case JOIN_SEMI:

				/*
				 * 若 RHS 是 RTE_RESULT，也许可以简化这种情况；
				 * 连接限定条件实际上变成只对
				 * LHS 的过滤条件，因为我们要么返回 LHS 行，要么不返回。
				 * 若无法把过滤子句上推
				 * 到父节点，它必须进入一个新的 FromExpr。
				 *
				 * 关于应当在 RHS 求值的 PHVs，有一个细微之处。
				 * 这种 PHVs 只能出现在
				 * 半连接的限定条件中，因为查询的其余部分无法
				 * 引用半连接 RHS 的任何输出。因此，
				 * 它们在被检查之前实际上无法变成 null，并且
				 * 直接去掉 PHV 包装也是可以的。我们没有
				 * 做这件事的基础设施，但 remove_result_refs() 会
				 * 把它们改标为在 LHS 求值，这没问题。
				 *
				 * 另外，不必担心去掉该
				 * 连接的 rtindex 痕迹，因为它本来就没有。
				 */
				if ((varno = get_result_relid(root, j->rarg)) != 0)
				{
					Assert(j->rtindex == 0);
					remove_result_refs(root, varno, j->larg);
					if (j->quals != NULL && parent_quals == NULL)
						jtnode = (Node *)
							makeFromExpr(list_make1(j->larg), j->quals);
					else
					{
						/* 把任何限定条件合并到父节点 */
						if (j->quals != NULL)
							*parent_quals = (Node *)
								list_concat(castNode(List, j->quals),
											castNode(List, *parent_quals));
						jtnode = j->larg;
					}
				}
				break;
			case JOIN_FULL:
			case JOIN_ANTI:
				/* 对这些情况我们没有特别的办法 */
				break;
			default:
				/* 注意：此时 JOIN_RIGHT 应当已经消失 */
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				break;
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return jtnode;
}

/*
 * get_result_relid
 *		若 jtnode 是 RTE_RESULT RTE 的 RangeTblRef，返回它的 relid；
 *		否则返回 0。
 */
static int
get_result_relid(PlannerInfo *root, Node *jtnode)
{
	int			varno;

	if (!IsA(jtnode, RangeTblRef))
		return 0;
	varno = ((RangeTblRef *) jtnode)->rtindex;
	if (rt_fetch(varno, root->parse->rtable)->rtekind != RTE_RESULT)
		return 0;
	return varno;
}

/*
 * remove_result_refs
 *		丢掉不需要的 RTE_RESULT RTE 的辅助例程。
 *
 * 这并不从连接树物理删除该 RTE，因为那
 * 在 remove_useless_results_recurse 里更容易处理。它做的是
 * 树其余部分的必要清理：必须调整任何可能引用该 RTE 的 PHVs。
 * 务必在连接树有效（没有断开的节点）时调用。
 * 注意不必处理 append_rel_list，因为直接在连接树中引用的 RTEs
 *
 * 不会是 appendrel 成员。
 * varno 是该 RTE_RESULT 的 relid。
 *
 * newjtloc 是连接树中任何引用该
 * RTE_RESULT 的 PHVs 应当改为求值的
 * 位置。
 */
static void
remove_result_refs(PlannerInfo *root, int varno, Node *newjtloc)
{
	/* 按需要修正 PlaceHolderVars */
	/* 若各处都没有 PHVs，可以跳过这一段 */
	if (root->glob->lastPHId != 0)
	{
		Relids		subrelids;

		subrelids = get_relids_in_jointree(newjtloc, true, false);
		Assert(!bms_is_empty(subrelids));
		substitute_phv_relids((Node *) root->parse, varno, subrelids);
		fix_append_rel_relids(root, varno, subrelids);
	}

	/*
	 * 还需要去掉任何引用该 RTE 的 PlanRowMark，但我们
	 * 把这项工作推迟到返回 remove_useless_result_rtes 时。
	 */
}


/*
 * find_dependent_phvs - 是否有 relid 恰好为
 * 给定 varno 的 PlaceHolderVars？
 *
 * 当想看 Query 中任何地方是否有
 * 这种 PHVs 时，应使用 find_dependent_phvs。另一种用途是看
 * 连接树的某棵子树是否含有这种 PHVs；但为此，不仅要看
 * 连接树节点本身，还要看
 * 被引用的 RTEs。那种情况用 find_dependent_phvs_in_jointree。
 */

typedef struct
{
	Relids		relids;
	int			sublevels_up;
} find_dependent_phvs_context;

static bool
find_dependent_phvs_walker(Node *node,
						   find_dependent_phvs_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up &&
			bms_equal(context->relids, phv->phrels))
			return true;
		/* fall through to examine children */
	}
	if (IsA(node, Query))
	{
		/* 递归进入子查询 */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   find_dependent_phvs_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	/* 这里应当不必处理大多数规划器辅助节点 */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	return expression_tree_walker(node, find_dependent_phvs_walker,
								  (void *) context);
}

static bool
find_dependent_phvs(PlannerInfo *root, int varno)
{
	find_dependent_phvs_context context;

	/* 若各处都没有 PHVs，不必费力 */
	if (root->glob->lastPHId == 0)
		return false;

	context.relids = bms_make_singleton(varno);
	context.sublevels_up = 0;

	if (query_tree_walker(root->parse,
						  find_dependent_phvs_walker,
						  (void *) &context,
						  0))
		return true;
	/* append_rel_list 可能已经填了，所以也要检查 */
	if (expression_tree_walker((Node *) root->append_rel_list,
							   find_dependent_phvs_walker,
							   (void *) &context))
		return true;
	return false;
}

static bool
find_dependent_phvs_in_jointree(PlannerInfo *root, Node *node, int varno)
{
	find_dependent_phvs_context context;
	Relids		subrelids;
	int			relid;

	/* 若各处都没有 PHVs，不必费力 */
	if (root->glob->lastPHId == 0)
		return false;

	context.relids = bms_make_singleton(varno);
	context.sublevels_up = 0;

	/*
	 * 看连接树片段本身是否含有引用（在连接限定条件中）
	 */
	if (find_dependent_phvs_walker(node, &context))
		return true;

	/*
	 * 否则，识别被引用的 RTEs 集合（可以忽略连接，
	 * 因为它们应当已经展平，所以它们的连接别名列表不再
	 * 重要），并逐个费力检查每个 RTE。不过可以忽略没有标成
	 * LATERAL 的 RTEs，因为它们不可能含有
	 * 对其他 RTEs 的任何交叉引用。
	 */
	subrelids = get_relids_in_jointree(node, false, false);
	relid = -1;
	while ((relid = bms_next_member(subrelids, relid)) >= 0)
	{
		RangeTblEntry *rte = rt_fetch(relid, root->parse->rtable);

		if (rte->lateral &&
			range_table_entry_walker(rte,
									 find_dependent_phvs_walker,
									 (void *) &context,
									 0))
			return true;
	}

	return false;
}

/*
 * substitute_phv_relids - 在上拉
 * 子查询或去掉 RTE_RESULT 连接树项之后，调整 PlaceHolderVar 的 relid 集合
 *
 * 在给定树中找出引用
 * 被上拉 relid 的任何 PlaceHolderVar 节点，改为引用替换后的 relid。
 *
 * 注意：虽然这具有遍历器的形式，我们取巧就地修改
 * 节点。这应当没问题，因为树先前已由
 * pullup_replace_vars 复制。不过要避免就地修改位图集合的原始值，
 * 因为 expression_tree_mutator 不复制它们。
 */

typedef struct
{
	int			varno;
	int			sublevels_up;
	Relids		subrelids;
} substitute_phv_relids_context;

static bool
substitute_phv_relids_walker(Node *node,
							 substitute_phv_relids_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up &&
			bms_is_member(context->varno, phv->phrels))
		{
			phv->phrels = bms_union(phv->phrels,
									context->subrelids);
			phv->phrels = bms_del_member(phv->phrels,
										 context->varno);
			/* 断言我们没有弄坏这个 PHV */
			Assert(!bms_is_empty(phv->phrels));
		}
		/* fall through to examine children */
	}
	if (IsA(node, Query))
	{
		/* 递归进入子查询 */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   substitute_phv_relids_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	/* 这里应当不必处理规划器辅助节点 */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, AppendRelInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	return expression_tree_walker(node, substitute_phv_relids_walker,
								  (void *) context);
}

static void
substitute_phv_relids(Node *node, int varno, Relids subrelids)
{
	substitute_phv_relids_context context;

	context.varno = varno;
	context.sublevels_up = 0;
	context.subrelids = subrelids;

	/*
	 * 必须准备好从 Query 或裸表达式树开始。
	 */
	query_or_expression_tree_walker(node,
									substitute_phv_relids_walker,
									(void *) &context,
									0);
}

/*
 * fix_append_rel_relids：更新 AppendRelInfo 节点的 RT 下标字段
 *
 * 上拉子查询时，任何 AppendRelInfo 对该子查询
 * RT 下标的引用都必须替换为代入的 relid（而且最好
 * 只有一个）。还需要对它们的
 * translated_vars 列表应用 substitute_phv_relids，因为其中可能含有 PlaceHolderVars。
 *
 * 假定可以就地修改 AppendRelInfo 节点。
 */
static void
fix_append_rel_relids(PlannerInfo *root, int varno, Relids subrelids)
{
	ListCell   *l;
	int			subvarno = -1;

	/*
	 * 只想提取一次成员 relid，但若有多个成员，不得立刻失败；
	 * 可能没有任何
	 * AppendRelInfo 节点引用它。因此在首次使用时再计算。注意
	 * 若集合不是单元素，bms_singleton_member 会报错。
	 */
	foreach(l, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);

		/* parent_relid 绝不应当是上拉目标 */
		Assert(appinfo->parent_relid != varno);

		if (appinfo->child_relid == varno)
		{
			if (subvarno < 0)
				subvarno = bms_singleton_member(subrelids);
			appinfo->child_relid = subvarno;
		}

		/* 也修正其翻译后的 var 中的任何 PHVs */
		if (root->glob->lastPHId != 0)
			substitute_phv_relids((Node *) appinfo->translated_vars,
								  varno, subrelids);
	}
}

/*
 * get_relids_in_jointree：取得连接树中出现的 RT 下标集合
 *
 * 基关系 relid 总是包含在结果中。
 * 若 include_outer_joins 为真，则包含外连接的 RT 下标。
 * 若 include_inner_joins 为真，则包含内连接的 RT 下标。
 *
 * 注意，对规划器中的大多数用途，外连接包含在
 * 标准 relid 集合中。只有在子查询展平期间的特殊用途，
 * 才适合把 include_inner_joins 设为真。
 */
Relids
get_relids_in_jointree(Node *jtnode, bool include_outer_joins,
					   bool include_inner_joins)
{
	Relids		result = NULL;

	if (jtnode == NULL)
		return result;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		result = bms_make_singleton(varno);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			result = bms_join(result,
							  get_relids_in_jointree(lfirst(l),
													 include_outer_joins,
													 include_inner_joins));
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		result = get_relids_in_jointree(j->larg,
										include_outer_joins,
										include_inner_joins);
		result = bms_join(result,
						  get_relids_in_jointree(j->rarg,
												 include_outer_joins,
												 include_inner_joins));
		if (j->rtindex)
		{
			if (j->jointype == JOIN_INNER)
			{
				if (include_inner_joins)
					result = bms_add_member(result, j->rtindex);
			}
			else
			{
				if (include_outer_joins)
					result = bms_add_member(result, j->rtindex);
			}
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return result;
}

/*
 * get_relids_for_join：取得构成一个连接的基表加外连接 RT 下标集合
 */
Relids
get_relids_for_join(Query *query, int joinrelid)
{
	Node	   *jtnode;

	jtnode = find_jointree_node_for_rel((Node *) query->jointree,
										joinrelid);
	if (!jtnode)
		elog(ERROR, "could not find join node %d", joinrelid);
	return get_relids_in_jointree(jtnode, true, false);
}

/*
 * find_jointree_node_for_rel：为基表或连接 RT 下标定位连接树节点
 *
 * 找不到则返回 NULL
 */
static Node *
find_jointree_node_for_rel(Node *jtnode, int relid)
{
	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		if (relid == varno)
			return jtnode;
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			jtnode = find_jointree_node_for_rel(lfirst(l), relid);
			if (jtnode)
				return jtnode;
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		if (relid == j->rtindex)
			return jtnode;
		jtnode = find_jointree_node_for_rel(j->larg, relid);
		if (jtnode)
			return jtnode;
		jtnode = find_jointree_node_for_rel(j->rarg, relid);
		if (jtnode)
			return jtnode;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return NULL;
}

/*
 * get_nullingrels：收集哪些外连接会把哪些关系置空
 *
 * 结果结构对查询中使用的每个叶子关系，包含
 * 可能把该关系置空的外连接的 relid 集合。
 */
static nullingrel_info *
get_nullingrels(Query *parse)
{
	nullingrel_info *result = palloc_object(nullingrel_info);

	result->rtlength = list_length(parse->rtable);
	result->nullingrels = palloc0_array(Relids, result->rtlength + 1);
	get_nullingrels_recurse((Node *) parse->jointree, NULL, result);
	return result;
}

/*
 * get_nullingrels() 的递归主体。
 *
 * 注意：在任何递归层，传下来的 upper_nullingrels 都必须
 * 当作常量，但若在叶子层，可以直接存进 *info。
 * 上层递归不释放它们变异后的
 * nullingrels 副本，因为那些副本很可能被
 * 至少一个叶子关系引用。
 */
static void
get_nullingrels_recurse(Node *jtnode, Relids upper_nullingrels,
						nullingrel_info *info)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		Assert(varno > 0 && varno <= info->rtlength);
		info->nullingrels[varno] = upper_nullingrels;
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			get_nullingrels_recurse(lfirst(l), upper_nullingrels, info);
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		Relids		local_nullingrels;

		switch (j->jointype)
		{
			case JOIN_INNER:
				get_nullingrels_recurse(j->larg, upper_nullingrels, info);
				get_nullingrels_recurse(j->rarg, upper_nullingrels, info);
				break;
			case JOIN_LEFT:
			case JOIN_SEMI:
			case JOIN_ANTI:
				local_nullingrels = bms_add_member(bms_copy(upper_nullingrels),
												   j->rtindex);
				get_nullingrels_recurse(j->larg, upper_nullingrels, info);
				get_nullingrels_recurse(j->rarg, local_nullingrels, info);
				break;
			case JOIN_FULL:
				local_nullingrels = bms_add_member(bms_copy(upper_nullingrels),
												   j->rtindex);
				get_nullingrels_recurse(j->larg, local_nullingrels, info);
				get_nullingrels_recurse(j->rarg, local_nullingrels, info);
				break;
			case JOIN_RIGHT:
				local_nullingrels = bms_add_member(bms_copy(upper_nullingrels),
												   j->rtindex);
				get_nullingrels_recurse(j->larg, local_nullingrels, info);
				get_nullingrels_recurse(j->rarg, upper_nullingrels, info);
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				break;
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}
