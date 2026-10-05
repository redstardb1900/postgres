/*-------------------------------------------------------------------------
 *
 * rewriteManip.c
 *	  查询重写器的查询树操作子例程
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteManip.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"


typedef struct
{
	int			sublevels_up;
} contain_aggs_of_level_context;

typedef struct
{
	int			agg_location;
	int			sublevels_up;
} locate_agg_of_level_context;

typedef struct
{
	int			win_location;
} locate_windowfunc_context;

typedef struct
{
	const Bitmapset *target_relids;
	const Bitmapset *added_relids;
	int			sublevels_up;
} add_nulling_relids_context;

typedef struct
{
	const Bitmapset *removable_relids;
	const Bitmapset *except_relids;
	int			sublevels_up;
} remove_nulling_relids_context;

static bool contain_aggs_of_level_walker(Node *node,
										 contain_aggs_of_level_context *context);
static bool locate_agg_of_level_walker(Node *node,
									   locate_agg_of_level_context *context);
static bool contain_windowfuncs_walker(Node *node, void *context);
static bool locate_windowfunc_walker(Node *node,
									 locate_windowfunc_context *context);
static bool checkExprHasSubLink_walker(Node *node, void *context);
static Relids offset_relid_set(Relids relids, int offset);
static Relids adjust_relid_set(Relids relids, int oldrelid, int newrelid);
static Node *add_nulling_relids_mutator(Node *node,
										add_nulling_relids_context *context);
static Node *remove_nulling_relids_mutator(Node *node,
										   remove_nulling_relids_context *context);


/*
 * contain_aggs_of_level -
 *	检查表达式是否含有指定查询层次的
 *	聚合函数调用。
 *
 * 本例程的目的是检测是否有属于
 * 给定查询层次的聚合。
 * 属于子查询或外层查询的聚合不会导致返回真。我们必须递归进入
 * 子查询，以检测逻辑上属于
 * 指定查询层次的外层引用聚合。
 */
bool
contain_aggs_of_level(Node *node, int levelsup)
{
	contain_aggs_of_level_context context;

	context.sublevels_up = levelsup;

	/*
	 * 必须准备好从 Query 或裸表达式树开始；若
	 * 它是 Query，我们不想递增 sublevels_up。
	 */
	return query_or_expression_tree_walker(node,
										   contain_aggs_of_level_walker,
										   (void *) &context,
										   0);
}

static bool
contain_aggs_of_level_walker(Node *node,
							 contain_aggs_of_level_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->agglevelsup == context->sublevels_up)
			return true;		/* 中止树遍历并返回真 */
		/* else fall through to examine argument */
	}
	if (IsA(node, GroupingFunc))
	{
		if (((GroupingFunc *) node)->agglevelsup == context->sublevels_up)
			return true;
		/* else fall through to examine argument */
	}
	if (IsA(node, Query))
	{
		/* 递归进入子查询 */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   contain_aggs_of_level_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, contain_aggs_of_level_walker,
								  (void *) context);
}

/*
 * locate_agg_of_level -
 *	  找出指定查询层次的任何聚合的解析位置。
 *
 * 若查询树里没有这样的聚合，或它们的解析位置都
 * 未知，则返回 -1。（前一种情形大概是调用者错误，
 * 但我们不费心把它和后一种情形区分开。）
 *
 * 注意：把这项功能并进
 * contain_aggs_of_level 似乎合适，但那会使该函数的 API 变复杂。
 * 目前，本函数的唯一用途是报错，
 * 因此省下若干周期大概并不很重要。
 */
int
locate_agg_of_level(Node *node, int levelsup)
{
	locate_agg_of_level_context context;

	context.agg_location = -1;	/* 以防什么都找不到 */
	context.sublevels_up = levelsup;

	/*
	 * 必须准备好从 Query 或裸表达式树开始；若
	 * 它是 Query，我们不想递增 sublevels_up。
	 */
	(void) query_or_expression_tree_walker(node,
										   locate_agg_of_level_walker,
										   (void *) &context,
										   0);

	return context.agg_location;
}

static bool
locate_agg_of_level_walker(Node *node,
						   locate_agg_of_level_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->agglevelsup == context->sublevels_up &&
			((Aggref *) node)->location >= 0)
		{
			context->agg_location = ((Aggref *) node)->location;
			return true;		/* 中止树遍历并返回真 */
		}
		/* else fall through to examine argument */
	}
	if (IsA(node, GroupingFunc))
	{
		if (((GroupingFunc *) node)->agglevelsup == context->sublevels_up &&
			((GroupingFunc *) node)->location >= 0)
		{
			context->agg_location = ((GroupingFunc *) node)->location;
			return true;		/* 中止树遍历并返回真 */
		}
	}
	if (IsA(node, Query))
	{
		/* 递归进入子查询 */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   locate_agg_of_level_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, locate_agg_of_level_walker,
								  (void *) context);
}

/*
 * contain_windowfuncs -
 *	检查表达式是否含有当前查询层次的
 *	窗口函数调用。
 */
bool
contain_windowfuncs(Node *node)
{
	/*
	 * 必须准备好从 Query 或裸表达式树开始；若
	 * 它是 Query，我们不想递增 sublevels_up。
	 */
	return query_or_expression_tree_walker(node,
										   contain_windowfuncs_walker,
										   NULL,
										   0);
}

static bool
contain_windowfuncs_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, WindowFunc))
		return true;			/* 中止树遍历并返回真 */
	/* 不得递归进入子查询 */
	return expression_tree_walker(node, contain_windowfuncs_walker,
								  (void *) context);
}

/*
 * locate_windowfunc -
 *	  找出当前查询层次的任何窗口函数的解析位置。
 *
 * 若查询树里没有这样的窗口函数，或它们的解析位置都
 * 未知，则返回 -1。（前一种情形大概是调用者错误，
 * 但我们不费心把它和后一种情形区分开。）
 *
 * 注意：把这项功能并进
 * contain_windowfuncs 似乎合适，但那会使该函数的 API 变复杂。
 * 目前，本函数的唯一用途是报错，
 * 因此省下若干周期大概并不很重要。
 */
int
locate_windowfunc(Node *node)
{
	locate_windowfunc_context context;

	context.win_location = -1;	/* 以防什么都找不到 */

	/*
	 * 必须准备好从 Query 或裸表达式树开始；若
	 * 它是 Query，我们不想递增 sublevels_up。
	 */
	(void) query_or_expression_tree_walker(node,
										   locate_windowfunc_walker,
										   (void *) &context,
										   0);

	return context.win_location;
}

static bool
locate_windowfunc_walker(Node *node, locate_windowfunc_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, WindowFunc))
	{
		if (((WindowFunc *) node)->location >= 0)
		{
			context->win_location = ((WindowFunc *) node)->location;
			return true;		/* 中止树遍历并返回真 */
		}
		/* else fall through to examine argument */
	}
	/* 不得递归进入子查询 */
	return expression_tree_walker(node, locate_windowfunc_walker,
								  (void *) context);
}

/*
 * checkExprHasSubLink -
 *	检查表达式是否含有 SubLink。
 */
bool
checkExprHasSubLink(Node *node)
{
	/*
	 * 若传入的是 Query，就检查它，但我们不应当递归进入
	 * 它的范围表或 CTE 列表里的子 Query。
	 */
	return query_or_expression_tree_walker(node,
										   checkExprHasSubLink_walker,
										   NULL,
										   QTW_IGNORE_RC_SUBQUERIES);
}

static bool
checkExprHasSubLink_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
		return true;			/* 中止树遍历并返回真 */
	return expression_tree_walker(node, checkExprHasSubLink_walker, context);
}

/*
 * 检查表达式树里的 MULTIEXPR Param。
 *
 * 我们有意不下降进入 SubLinks：只有当前
 * 查询层次的 Param 才有意义。
 */
static bool
contains_multiexpr_param(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		if (((Param *) node)->paramkind == PARAM_MULTIEXPR)
			return true;		/* 中止树遍历并返回真 */
		return false;
	}
	return expression_tree_walker(node, contains_multiexpr_param, context);
}

/*
 * CombineRangeTables
 * 		把 src_rtable 的 RTEs 加入 dst_rtable。
 *
 * 这也把 src_perminfos 的 RTEPermissionInfos（属于
 * src_rtable 里的 RTEs）加入 *dst_perminfos，并更新 src_rtable 里
 * 那些 RTEs 的 perminfoindex，使它们现在指向
 * *dst_perminfos 里这些 perminfo 的下标。
 *
 * 注意这会破坏性地改动 dst_rtable 和 dst_perminfos，
 * 因此调用者最好传入可以安全修改的副本。
 */
void
CombineRangeTables(List **dst_rtable, List **dst_perminfos,
				   List *src_rtable, List *src_perminfos)
{
	ListCell   *l;
	int			offset = list_length(*dst_perminfos);

	if (offset > 0)
	{
		foreach(l, src_rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

			if (rte->perminfoindex > 0)
				rte->perminfoindex += offset;
		}
	}

	*dst_perminfos = list_concat(*dst_perminfos, src_perminfos);
	*dst_rtable = list_concat(*dst_rtable, src_rtable);
}

/*
 * OffsetVarNodes - 把一个查询的 RT 追加到另一个时调整 Vars。
 *
 * 找出给定树里 varlevelsup == sublevels_up 的全部 Var 节点，
 * 并把它们的 varno 成员（范围表下标）增加 offset。
 * varnosyn 成员也类似地调整。同时调整其他含有
 * 范围表下标的节点，例如 RangeTblRef 和 JoinExpr。
 *
 * 注意：虽然这具有遍历器的形式，我们取巧并就地修改
 * 这些节点。给定的表达式树应当事先已经复制过，
 * 以确保不会发生不想要的副作用！
 */

typedef struct
{
	int			offset;
	int			sublevels_up;
} OffsetVarNodes_context;

static bool
OffsetVarNodes_walker(Node *node, OffsetVarNodes_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up)
		{
			var->varno += context->offset;
			var->varnullingrels = offset_relid_set(var->varnullingrels,
												   context->offset);
			if (var->varnosyn > 0)
				var->varnosyn += context->offset;
		}
		return false;
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) node;

		if (context->sublevels_up == 0)
			cexpr->cvarno += context->offset;
		return false;
	}
	if (IsA(node, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) node;

		if (context->sublevels_up == 0)
			rtr->rtindex += context->offset;
		/* 子查询本身被单独访问 */
		return false;
	}
	if (IsA(node, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) node;

		if (j->rtindex && context->sublevels_up == 0)
			j->rtindex += context->offset;
		/* fall through to examine children */
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up)
		{
			phv->phrels = offset_relid_set(phv->phrels,
										   context->offset);
			phv->phnullingrels = offset_relid_set(phv->phnullingrels,
												  context->offset);
		}
		/* fall through to examine children */
	}
	if (IsA(node, AppendRelInfo))
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) node;

		if (context->sublevels_up == 0)
		{
			appinfo->parent_relid += context->offset;
			appinfo->child_relid += context->offset;
		}
		/* fall through to examine children */
	}
	/* 这里不应当需要处理其他规划器辅助节点 */
	Assert(!IsA(node, PlanRowMark));
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	if (IsA(node, Query))
	{
		/* 递归进入子查询 */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, OffsetVarNodes_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, OffsetVarNodes_walker,
								  (void *) context);
}

void
OffsetVarNodes(Node *node, int offset, int sublevels_up)
{
	OffsetVarNodes_context context;

	context.offset = offset;
	context.sublevels_up = sublevels_up;

	/*
	 * 必须准备好从 Query 或裸表达式树开始；若
	 * 它是 Query，就直接进入 query_tree_walker，以确保
	 * sublevels_up 不会被过早递增。
	 */
	if (node && IsA(node, Query))
	{
		Query	   *qry = (Query *) node;

		/*
		 * 若我们从 Query 开始，且 sublevels_up 为零，则我们
		 * 还必须修正 Query 自身里的范围表下标，即
		 * resultRelation、exclRelIndex 和 rowMarks 项。递归进入子查询时
		 * sublevels_up 无法为零，因此不必在
		 * OffsetVarNodes_walker 内部放同样的逻辑。
		 */
		if (sublevels_up == 0)
		{
			ListCell   *l;

			if (qry->resultRelation)
				qry->resultRelation += offset;

			if (qry->onConflict && qry->onConflict->exclRelIndex)
				qry->onConflict->exclRelIndex += offset;

			foreach(l, qry->rowMarks)
			{
				RowMarkClause *rc = (RowMarkClause *) lfirst(l);

				rc->rti += offset;
			}
		}
		query_tree_walker(qry, OffsetVarNodes_walker,
						  (void *) &context, 0);
	}
	else
		OffsetVarNodes_walker(node, &context);
}

static Relids
offset_relid_set(Relids relids, int offset)
{
	Relids		result = NULL;
	int			rtindex;

	rtindex = -1;
	while ((rtindex = bms_next_member(relids, rtindex)) >= 0)
		result = bms_add_member(result, rtindex + offset);
	return result;
}

/*
 * ChangeVarNodes - 为 RT 下标的一次特定变更调整 Var 节点。
 *
 * 找出给定树里属于特定关系的全部 Var 节点
 * （由 sublevels_up 和 rt_index 标识），并把它们的 varno 成员
 * 改成 new_index。varnosyn 成员也改。同时调整其他
 * 含有范围表下标的节点，例如 RangeTblRef 和 JoinExpr。
 *
 * 注意：虽然这具有遍历器的形式，我们取巧并就地修改
 * 这些节点。给定的表达式树应当事先已经复制过，
 * 以确保不会发生不想要的副作用！
 */

typedef struct
{
	int			rt_index;
	int			new_index;
	int			sublevels_up;
} ChangeVarNodes_context;

static bool
ChangeVarNodes_walker(Node *node, ChangeVarNodes_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up)
		{
			if (var->varno == context->rt_index)
				var->varno = context->new_index;
			var->varnullingrels = adjust_relid_set(var->varnullingrels,
												   context->rt_index,
												   context->new_index);
			if (var->varnosyn == context->rt_index)
				var->varnosyn = context->new_index;
		}
		return false;
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) node;

		if (context->sublevels_up == 0 &&
			cexpr->cvarno == context->rt_index)
			cexpr->cvarno = context->new_index;
		return false;
	}
	if (IsA(node, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) node;

		if (context->sublevels_up == 0 &&
			rtr->rtindex == context->rt_index)
			rtr->rtindex = context->new_index;
		/* 子查询本身被单独访问 */
		return false;
	}
	if (IsA(node, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) node;

		if (context->sublevels_up == 0 &&
			j->rtindex == context->rt_index)
			j->rtindex = context->new_index;
		/* fall through to examine children */
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up)
		{
			phv->phrels = adjust_relid_set(phv->phrels,
										   context->rt_index,
										   context->new_index);
			phv->phnullingrels = adjust_relid_set(phv->phnullingrels,
												  context->rt_index,
												  context->new_index);
		}
		/* fall through to examine children */
	}
	if (IsA(node, PlanRowMark))
	{
		PlanRowMark *rowmark = (PlanRowMark *) node;

		if (context->sublevels_up == 0)
		{
			if (rowmark->rti == context->rt_index)
				rowmark->rti = context->new_index;
			if (rowmark->prti == context->rt_index)
				rowmark->prti = context->new_index;
		}
		return false;
	}
	if (IsA(node, AppendRelInfo))
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) node;

		if (context->sublevels_up == 0)
		{
			if (appinfo->parent_relid == context->rt_index)
				appinfo->parent_relid = context->new_index;
			if (appinfo->child_relid == context->rt_index)
				appinfo->child_relid = context->new_index;
		}
		/* fall through to examine children */
	}
	/* 这里不应当需要处理其他规划器辅助节点 */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	if (IsA(node, Query))
	{
		/* 递归进入子查询 */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, ChangeVarNodes_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, ChangeVarNodes_walker,
								  (void *) context);
}

void
ChangeVarNodes(Node *node, int rt_index, int new_index, int sublevels_up)
{
	ChangeVarNodes_context context;

	context.rt_index = rt_index;
	context.new_index = new_index;
	context.sublevels_up = sublevels_up;

	/*
	 * 必须准备好从 Query 或裸表达式树开始；若
	 * 它是 Query，就直接进入 query_tree_walker，以确保
	 * sublevels_up 不会被过早递增。
	 */
	if (node && IsA(node, Query))
	{
		Query	   *qry = (Query *) node;

		/*
		 * 若我们从 Query 开始，且 sublevels_up 为零，则我们
		 * 还必须修正 Query 自身里的范围表下标，即
		 * resultRelation 和 rowMarks 项。递归进入子查询时 sublevels_up
		 * 无法为零，因此不必在
		 * ChangeVarNodes_walker 内部放同样的逻辑。
		 */
		if (sublevels_up == 0)
		{
			ListCell   *l;

			if (qry->resultRelation == rt_index)
				qry->resultRelation = new_index;

			/* 这不大可能被用到，但是…… */
			if (qry->onConflict && qry->onConflict->exclRelIndex == rt_index)
				qry->onConflict->exclRelIndex = new_index;

			foreach(l, qry->rowMarks)
			{
				RowMarkClause *rc = (RowMarkClause *) lfirst(l);

				if (rc->rti == rt_index)
					rc->rti = new_index;
			}
		}
		query_tree_walker(qry, ChangeVarNodes_walker,
						  (void *) &context, 0);
	}
	else
		ChangeVarNodes_walker(node, &context);
}

/*
 * 在 Relid 集合里用 newrelid 替换 oldrelid。
 *
 * 注意：有些扩展可能把 INDEX_VAR 之类的特殊 varno 当作
 * oldrelid 传入。bms_is_member 不会喜欢那样，但我们应当容忍。
 * （也许 newrelid 也可以是特殊 varno，但最好
 * 没有理由把它注入 nullingrels 或 phrels 集合。）
 */
static Relids
adjust_relid_set(Relids relids, int oldrelid, int newrelid)
{
	if (!IS_SPECIAL_VARNO(oldrelid) && bms_is_member(oldrelid, relids))
	{
		/* 确保我们有一份可修改的副本 */
		relids = bms_copy(relids);
		/* 去掉旧的，加上新的 */
		relids = bms_del_member(relids, oldrelid);
		relids = bms_add_member(relids, newrelid);
	}
	return relids;
}

/*
 * IncrementVarSublevelsUp - 把 Var 节点在树中下推时调整它们。
 *
 * 找出给定树里 varlevelsup >= min_sublevels_up 的全部 Var 节点，
 * 并把 delta_sublevels_up 加到它们的 varlevelsup 上。当把
 * 对某个嵌套层次正确的表达式插入
 * 子查询时需要这样做。通常初次调用时 min_sublevels_up == 0，从而
 * 所有 Var 都受影响。min_sublevels_up 的意义在于，递归进入子链接时
 * 我们可以递增它，从而使该子链接里的局部变量
 * 不受影响，只有对外层的引用、属于
 * 该表达式原来的查询层次或其父层次的 var 才受影响。
 *
 * 对含有 levelsup 成员的其他节点也一样，例如 Aggref。
 *
 * 注意：虽然这具有遍历器的形式，我们取巧并就地修改
 * Var 节点。给定的表达式树应当事先已经复制过，
 * 以确保不会发生不想要的副作用！
 */

typedef struct
{
	int			delta_sublevels_up;
	int			min_sublevels_up;
} IncrementVarSublevelsUp_context;

static bool
IncrementVarSublevelsUp_walker(Node *node,
							   IncrementVarSublevelsUp_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup >= context->min_sublevels_up)
			var->varlevelsup += context->delta_sublevels_up;
		return false;			/* 到此完成 */
	}
	if (IsA(node, CurrentOfExpr))
	{
		/* 这不应当发生 */
		if (context->min_sublevels_up == 0)
			elog(ERROR, "cannot push down CurrentOfExpr");
		return false;
	}
	if (IsA(node, Aggref))
	{
		Aggref	   *agg = (Aggref *) node;

		if (agg->agglevelsup >= context->min_sublevels_up)
			agg->agglevelsup += context->delta_sublevels_up;
		/* fall through to recurse into argument */
	}
	if (IsA(node, GroupingFunc))
	{
		GroupingFunc *grp = (GroupingFunc *) node;

		if (grp->agglevelsup >= context->min_sublevels_up)
			grp->agglevelsup += context->delta_sublevels_up;
		/* fall through to recurse into argument */
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup >= context->min_sublevels_up)
			phv->phlevelsup += context->delta_sublevels_up;
		/* fall through to recurse into argument */
	}
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		if (rte->rtekind == RTE_CTE)
		{
			if (rte->ctelevelsup >= context->min_sublevels_up)
				rte->ctelevelsup += context->delta_sublevels_up;
		}
		return false;			/* 允许 range_table_walker 继续 */
	}
	if (IsA(node, Query))
	{
		/* 递归进入子查询 */
		bool		result;

		context->min_sublevels_up++;
		result = query_tree_walker((Query *) node,
								   IncrementVarSublevelsUp_walker,
								   (void *) context,
								   QTW_EXAMINE_RTES_BEFORE);
		context->min_sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, IncrementVarSublevelsUp_walker,
								  (void *) context);
}

void
IncrementVarSublevelsUp(Node *node, int delta_sublevels_up,
						int min_sublevels_up)
{
	IncrementVarSublevelsUp_context context;

	context.delta_sublevels_up = delta_sublevels_up;
	context.min_sublevels_up = min_sublevels_up;

	/*
	 * 必须准备好从 Query 或裸表达式树开始；若
	 * 它是 Query，我们不想递增 sublevels_up。
	 */
	query_or_expression_tree_walker(node,
									IncrementVarSublevelsUp_walker,
									(void *) &context,
									QTW_EXAMINE_RTES_BEFORE);
}

/*
 * IncrementVarSublevelsUp_rtable -
 *	与 IncrementVarSublevelsUp 相同，但要在范围表上调用。
 */
void
IncrementVarSublevelsUp_rtable(List *rtable, int delta_sublevels_up,
							   int min_sublevels_up)
{
	IncrementVarSublevelsUp_context context;

	context.delta_sublevels_up = delta_sublevels_up;
	context.min_sublevels_up = min_sublevels_up;

	range_table_walker(rtable,
					   IncrementVarSublevelsUp_walker,
					   (void *) &context,
					   QTW_EXAMINE_RTES_BEFORE);
}


/*
 * rangeTableEntry_used - 检测某个 RTE 是否在查询或表达式的
 *	var 节点、连接或集合操作树里某处被引用。
 */

typedef struct
{
	int			rt_index;
	int			sublevels_up;
} rangeTableEntry_used_context;

static bool
rangeTableEntry_used_walker(Node *node,
							rangeTableEntry_used_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up &&
			(var->varno == context->rt_index ||
			 bms_is_member(context->rt_index, var->varnullingrels)))
			return true;
		return false;
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) node;

		if (context->sublevels_up == 0 &&
			cexpr->cvarno == context->rt_index)
			return true;
		return false;
	}
	if (IsA(node, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) node;

		if (rtr->rtindex == context->rt_index &&
			context->sublevels_up == 0)
			return true;
		/* 子查询本身被单独访问 */
		return false;
	}
	if (IsA(node, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) node;

		if (j->rtindex == context->rt_index &&
			context->sublevels_up == 0)
			return true;
		/* fall through to examine children */
	}
	/* 这里不应当需要处理规划器辅助节点 */
	Assert(!IsA(node, PlaceHolderVar));
	Assert(!IsA(node, PlanRowMark));
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, AppendRelInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	if (IsA(node, Query))
	{
		/* 递归进入子查询 */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, rangeTableEntry_used_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, rangeTableEntry_used_walker,
								  (void *) context);
}

bool
rangeTableEntry_used(Node *node, int rt_index, int sublevels_up)
{
	rangeTableEntry_used_context context;

	context.rt_index = rt_index;
	context.sublevels_up = sublevels_up;

	/*
	 * 必须准备好从 Query 或裸表达式树开始；若
	 * 它是 Query，我们不想递增 sublevels_up。
	 */
	return query_or_expression_tree_walker(node,
										   rangeTableEntry_used_walker,
										   (void *) &context,
										   0);
}


/*
 * 若给定 Query 是 INSERT ... SELECT 构造，就取出并
 * 返回表示 SELECT 部分的子 Query 节点。否则
 * 返回给定的 Query。
 *
 * 若 subquery_ptr 非 NULL，则把 *subquery_ptr 设为
 * parsetree 内部指向 SELECT 子查询的链接位置，若不是
 * INSERT ... SELECT 则设为 NULL。
 *
 * 这是权宜做法，因为出现在规则动作里的 INSERT ... SELECTs 上的变换
 * 应当作用于源 SELECT，而不是
 * INSERT 部分。也许可以用重新设计的查询树把这清理掉。
 */
Query *
getInsertSelectQuery(Query *parsetree, Query ***subquery_ptr)
{
	Query	   *selectquery;
	RangeTblEntry *selectrte;
	RangeTblRef *rtr;

	if (subquery_ptr)
		*subquery_ptr = NULL;

	if (parsetree == NULL)
		return parsetree;
	if (parsetree->commandType != CMD_INSERT)
		return parsetree;

	/*
	 * 目前，这只应用于规则动作查询，因此我们
	 * 预期在给定查询里找到 OLD 和 NEW 占位项。
	 * 若它们不在那里，那必定是 INSERT/SELECT，其中它们已被
	 * 下推到 SELECT。
	 */
	if (list_length(parsetree->rtable) >= 2 &&
		strcmp(rt_fetch(PRS2_OLD_VARNO, parsetree->rtable)->eref->aliasname,
			   "old") == 0 &&
		strcmp(rt_fetch(PRS2_NEW_VARNO, parsetree->rtable)->eref->aliasname,
			   "new") == 0)
		return parsetree;
	Assert(parsetree->jointree && IsA(parsetree->jointree, FromExpr));
	if (list_length(parsetree->jointree->fromlist) != 1)
		elog(ERROR, "expected to find SELECT subquery");
	rtr = (RangeTblRef *) linitial(parsetree->jointree->fromlist);
	if (!IsA(rtr, RangeTblRef))
		elog(ERROR, "expected to find SELECT subquery");
	selectrte = rt_fetch(rtr->rtindex, parsetree->rtable);
	if (!(selectrte->rtekind == RTE_SUBQUERY &&
		  selectrte->subquery &&
		  IsA(selectrte->subquery, Query) &&
		  selectrte->subquery->commandType == CMD_SELECT))
		elog(ERROR, "expected to find SELECT subquery");
	selectquery = selectrte->subquery;
	if (list_length(selectquery->rtable) >= 2 &&
		strcmp(rt_fetch(PRS2_OLD_VARNO, selectquery->rtable)->eref->aliasname,
			   "old") == 0 &&
		strcmp(rt_fetch(PRS2_NEW_VARNO, selectquery->rtable)->eref->aliasname,
			   "new") == 0)
	{
		if (subquery_ptr)
			*subquery_ptr = &(selectrte->subquery);
		return selectquery;
	}
	elog(ERROR, "could not find rule placeholders");
	return NULL;				/* 不会到达 */
}


/*
 * 把给定的限定条件加到查询的 WHERE 子句
 */
void
AddQual(Query *parsetree, Node *qual)
{
	Node	   *copy;

	if (qual == NULL)
		return;

	if (parsetree->commandType == CMD_UTILITY)
	{
		/*
		 * 实用语句上没有地方放这个限定条件。
		 *
		 * 若它是 NOTIFY，就静默忽略该限定条件；这意味着
		 * NOTIFY 会执行，不论是否有任何符合条件的行。
		 * 这显然是错的，但比完全拒绝
		 * 执行该规则有用得多，而且额外的 NOTIFY 事件对
		 * NOTIFY 的典型用途是无害的。
		 *
		 * 若它不是 NOTIFY，就报错，因为无条件执行
		 * 其他实用语句多半不是想要的。（这种情形目前
		 * 反正不被允许，但为安全起见保留这项测试。）
		 */
		if (parsetree->utilityStmt && IsA(parsetree->utilityStmt, NotifyStmt))
			return;
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("conditional utility statements are not implemented")));
	}

	if (parsetree->setOperations != NULL)
	{
		/*
		 * 集合操作语句上也没有地方放这个限定条件。（这
		 * 可以修好，但目前规划器会直接忽略集合操作查询上的
		 * 任何限定条件。）
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));
	}

	/* INTERSECT 想要原来的，但我们需要复制。Jan */
	copy = copyObject(qual);

	parsetree->jointree->quals = make_and_qual(parsetree->jointree->quals,
											   copy);

	/*
	 * 我们最好不要把聚合塞进 WHERE 子句。
	 */
	Assert(!contain_aggs_of_level(copy, 0));

	/*
	 * 若加入的限定条件含有子链接，确保查询被正确标记。查询已经
	 * 标过时不必搜索该限定条件。
	 */
	if (!parsetree->hasSubLinks)
		parsetree->hasSubLinks = checkExprHasSubLink(copy);
}


/*
 * 把给定子句取反并加到给定查询树的 WHERE 限定条件上。
 * 取反的意思是 x IS NOT TRUE，而不只是 NOT x，
 * 否则当 x 求值为 NULL 时我们会做错事。
 */
void
AddInvertedQual(Query *parsetree, Node *qual)
{
	BooleanTest *invqual;

	if (qual == NULL)
		return;

	/* 不必复制输入限定条件，因为 AddQual 会…… */
	invqual = makeNode(BooleanTest);
	invqual->arg = (Expr *) qual;
	invqual->booltesttype = IS_NOT_TRUE;
	invqual->location = -1;

	AddQual(parsetree, (Node *) invqual);
}


/*
 * add_nulling_relids() 找出属于任何一个
 * target_relids 的 Var 和 PlaceHolderVars，并把 added_relids 加到它们的 varnullingrels
 * 和 phnullingrels 成员上。若 target_relids 为 NULL，则修改全部零层次
 * Var 和 PHVs。
 */
Node *
add_nulling_relids(Node *node,
				   const Bitmapset *target_relids,
				   const Bitmapset *added_relids)
{
	add_nulling_relids_context context;

	context.target_relids = target_relids;
	context.added_relids = added_relids;
	context.sublevels_up = 0;
	return query_or_expression_tree_mutator(node,
											add_nulling_relids_mutator,
											&context,
											0);
}

static Node *
add_nulling_relids_mutator(Node *node,
						   add_nulling_relids_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up &&
			(context->target_relids == NULL ||
			 bms_is_member(var->varno, context->target_relids)))
		{
			Relids		newnullingrels = bms_union(var->varnullingrels,
												   context->added_relids);

			/* 复制该 Var…… */
			var = copyObject(var);
			/* ……并替换副本的 varnullingrels 成员 */
			var->varnullingrels = newnullingrels;
			return (Node *) var;
		}
		/* Otherwise fall through to copy the Var normally */
	}
	else if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up &&
			(context->target_relids == NULL ||
			 bms_overlap(phv->phrels, context->target_relids)))
		{
			Relids		newnullingrels = bms_union(phv->phnullingrels,
												   context->added_relids);

			/*
			 * 我们不修改 PHV 表达式的内容，只往
			 * phnullingrels 上加。这对应于假定该 PHV
			 * 仍在原来的层次求值，然后也许在上浮时被
			 * 置空。因此，只平铺复制该节点……
			 */
			phv = makeNode(PlaceHolderVar);
			memcpy(phv, node, sizeof(PlaceHolderVar));
			/* ……并替换副本的 phnullingrels 成员 */
			phv->phnullingrels = newnullingrels;
			return (Node *) phv;
		}
		/* Otherwise fall through to copy the PlaceHolderVar normally */
	}
	else if (IsA(node, Query))
	{
		/* 递归进入 RTE 或子链接子查询 */
		Query	   *newnode;

		context->sublevels_up++;
		newnode = query_tree_mutator((Query *) node,
									 add_nulling_relids_mutator,
									 (void *) context,
									 0);
		context->sublevels_up--;
		return (Node *) newnode;
	}
	return expression_tree_mutator(node, add_nulling_relids_mutator,
								   (void *) context);
}

/*
 * remove_nulling_relids() 去掉给定表达式里
 * Var.varnullingrels 和 PlaceHolderVar.phnullingrels 成员中
 * 提到的指定 RT 下标，
 * 属于 except_relids 所列关系的节点除外。
 */
Node *
remove_nulling_relids(Node *node,
					  const Bitmapset *removable_relids,
					  const Bitmapset *except_relids)
{
	remove_nulling_relids_context context;

	context.removable_relids = removable_relids;
	context.except_relids = except_relids;
	context.sublevels_up = 0;
	return query_or_expression_tree_mutator(node,
											remove_nulling_relids_mutator,
											&context,
											0);
}

static Node *
remove_nulling_relids_mutator(Node *node,
							  remove_nulling_relids_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up &&
			!bms_is_member(var->varno, context->except_relids) &&
			bms_overlap(var->varnullingrels, context->removable_relids))
		{
			/* 复制该 Var…… */
			var = copyObject(var);
			/* ……并替换副本的 varnullingrels 成员 */
			var->varnullingrels = bms_difference(var->varnullingrels,
												 context->removable_relids);
			return (Node *) var;
		}
		/* Otherwise fall through to copy the Var normally */
	}
	else if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up &&
			!bms_overlap(phv->phrels, context->except_relids))
		{
			/*
			 * 注意：若 phnullingrels 变成空，把 PHV 整个去掉似乎
			 * 可取。目前我们不敢那样做，
			 * 因为有些情形我们用 PHVs 来强制子表达式的
			 * 各自身份；见 prepjointree.c 里 wrap_non_vars 的用法。
			 */
			/* 复制 PlaceHolderVar 并修改其下的内容…… */
			phv = (PlaceHolderVar *)
				expression_tree_mutator(node,
										remove_nulling_relids_mutator,
										(void *) context);
			/* ……并替换副本的 phnullingrels 成员 */
			phv->phnullingrels = bms_difference(phv->phnullingrels,
												context->removable_relids);
			/* 若 phrels 含有可去掉的 RTI，我们也必须更新它 */
			phv->phrels = bms_difference(phv->phrels,
										 context->removable_relids);
			Assert(!bms_is_empty(phv->phrels));
			return (Node *) phv;
		}
		/* Otherwise fall through to copy the PlaceHolderVar normally */
	}
	else if (IsA(node, Query))
	{
		/* 递归进入 RTE 或子链接子查询 */
		Query	   *newnode;

		context->sublevels_up++;
		newnode = query_tree_mutator((Query *) node,
									 remove_nulling_relids_mutator,
									 (void *) context,
									 0);
		context->sublevels_up--;
		return (Node *) newnode;
	}
	return expression_tree_mutator(node, remove_nulling_relids_mutator,
								   (void *) context);
}


/*
 * replace_rte_variables() 找出表达式树里
 * 引用特定 RTE 的全部 Var，并用调用者提供的回调函数
 * 取得的替换表达式替换它们。
 *
 * 对 Query 的一部分调用 replace_rte_variables 时，把
 * 包含它的 Query 的 hasSubLinks 成员地址作为 outer_hasSubLinks 传入。
 * 否则传入 NULL，但那时把 SubLink 插入非 Query 表达式
 * 就会导致错误。
 *
 * 注意：inserted_sublink 这套做法是为了在替换把子查询加进子查询内部时
 * 更新子查询里的 hasSubLinks。
 * 很乱，对吧？我们不必对 hasAggs 做类似的折腾，
 * 因为这种变换不可能把零层次的
 * 聚合引用插入子查询，它只能插入外层聚合。
 * 对 hasWindowFuncs 也一样。
 *
 * 注意：对这样的函数，我们通常不会暴露修改器函数或上下文结构。
 * 我们这样做，是因为回调常常发现
 * 直接对它们将要返回的子表达式递归到修改器
 * 很方便。
 */
Node *
replace_rte_variables(Node *node, int target_varno, int sublevels_up,
					  replace_rte_variables_callback callback,
					  void *callback_arg,
					  bool *outer_hasSubLinks)
{
	Node	   *result;
	replace_rte_variables_context context;

	context.callback = callback;
	context.callback_arg = callback_arg;
	context.target_varno = target_varno;
	context.sublevels_up = sublevels_up;

	/*
	 * 若不必检测新子链接，因为查询已经有一些，
	 * 我们就尝试把 inserted_sublink 初始化为真。
	 */
	if (node && IsA(node, Query))
		context.inserted_sublink = ((Query *) node)->hasSubLinks;
	else if (outer_hasSubLinks)
		context.inserted_sublink = *outer_hasSubLinks;
	else
		context.inserted_sublink = false;

	/*
	 * 必须准备好从 Query 或裸表达式树开始；若
	 * 它是 Query，我们不想递增 sublevels_up。
	 */
	result = query_or_expression_tree_mutator(node,
											  replace_rte_variables_mutator,
											  (void *) &context,
											  0);

	if (context.inserted_sublink)
	{
		if (result && IsA(result, Query))
			((Query *) result)->hasSubLinks = true;
		else if (outer_hasSubLinks)
			*outer_hasSubLinks = true;
		else
			elog(ERROR, "replace_rte_variables inserted a SubLink, but has noplace to record it");
	}

	return result;
}

Node *
replace_rte_variables_mutator(Node *node,
							  replace_rte_variables_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == context->target_varno &&
			var->varlevelsup == context->sublevels_up)
		{
			/* 找到匹配的变量，做替换 */
			Node	   *newnode;

			newnode = context->callback(var, context);
			/* 检测我们是否在给查询加子链接 */
			if (!context->inserted_sublink)
				context->inserted_sublink = checkExprHasSubLink(newnode);
			return newnode;
		}
		/* otherwise fall through to copy the var normally */
	}
	else if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) node;

		if (cexpr->cvarno == context->target_varno &&
			context->sublevels_up == 0)
		{
			/*
			 * 若 WHERE CURRENT OF 表达式结果作用于
			 * 一个视图，我们就到这里。将来也许能把该
			 * 表达式翻译成作用于视图的底层表，但
			 * 目前尚未实现。
			 */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("WHERE CURRENT OF on a view is not implemented")));
		}
		/* otherwise fall through to copy the expr normally */
	}
	else if (IsA(node, Query))
	{
		/* 递归进入 RTE 子查询或尚未规划的子链接子查询 */
		Query	   *newnode;
		bool		save_inserted_sublink;

		context->sublevels_up++;
		save_inserted_sublink = context->inserted_sublink;
		context->inserted_sublink = ((Query *) node)->hasSubLinks;
		newnode = query_tree_mutator((Query *) node,
									 replace_rte_variables_mutator,
									 (void *) context,
									 0);
		newnode->hasSubLinks |= context->inserted_sublink;
		context->inserted_sublink = save_inserted_sublink;
		context->sublevels_up--;
		return (Node *) newnode;
	}
	return expression_tree_mutator(node, replace_rte_variables_mutator,
								   (void *) context);
}


/*
 * map_variable_attnos() 找出表达式树里引用特定 RTE 的
 * 全部用户列 Var，并按给定的映射数组调整它们的 varattno
 * （varattno n 被替换成 attno_map[n-1]）。
 * 系统列的 Var 不被修改。
 *
 * 映射数组里的零表示已删除的列，它不应当
 * 出现在表达式里。
 *
 * 若表达式树含有目标 RTE 的整行 Var，
 * 就把 *found_whole_row 设为真。此外，若 to_rowtype
 * 不是 InvalidOid，我们把该 Var 替换成该 vartype 的 Var，并插入
 * 一个 ConvertRowtypeExpr，映射回表达式所预期的行类型。
 * （因此，to_rowtype 最好是我们正在改引用的那个
 * RTE 的行类型的子行类型。）不提供 to_rowtype 的调用者
 * 应当在 *found_whole_row 为真时报错；我们不在这里做，
 * 因为我们不知道错误信息用什么措辞
 * 最合适。调用者会了解上下文。
 *
 * 这可以用 replace_rte_variables 和一个回调函数来构造，
 * 但因为我们从不会插入子链接，replace_rte_variables
 * 过于复杂。
 */

typedef struct
{
	int			target_varno;	/* 要搜索的 RTE 下标 */
	int			sublevels_up;	/* （当前）嵌套深度 */
	const AttrMap *attno_map;	/* 用户属性号的映射数组 */
	Oid			to_rowtype;		/* 把整行 Var 改成这个类型 */
	bool	   *found_whole_row;	/* 输出标志 */
} map_variable_attnos_context;

static Node *
map_variable_attnos_mutator(Node *node,
							map_variable_attnos_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == context->target_varno &&
			var->varlevelsup == context->sublevels_up)
		{
			/* 找到匹配的变量，做替换 */
			Var		   *newvar = (Var *) palloc(sizeof(Var));
			int			attno = var->varattno;

			*newvar = *var;		/* 先复制该 Var 的全部成员 */

			if (attno > 0)
			{
				/* 用户定义的列，替换属性号 */
				if (attno > context->attno_map->maplen ||
					context->attno_map->attnums[attno - 1] == 0)
					elog(ERROR, "unexpected varattno %d in expression to be mapped",
						 attno);
				newvar->varattno = context->attno_map->attnums[attno - 1];
				/* 若语法引用对象是同一个 RTE，也修正它 */
				if (newvar->varnosyn == context->target_varno)
					newvar->varattnosyn = newvar->varattno;
			}
			else if (attno == 0)
			{
				/* 整行变量，警告调用者 */
				*(context->found_whole_row) = true;

				/* 若调用者预期我们转换该 Var，就这样做。 */
				if (OidIsValid(context->to_rowtype) &&
					context->to_rowtype != var->vartype)
				{
					ConvertRowtypeExpr *r;

					/* 这对 RECORD 变量肯定行不通。 */
					Assert(var->vartype != RECORDOID);

					/* Var 本身被改成所请求的类型。 */
					newvar->vartype = context->to_rowtype;

					/*
					 * 在顶上加一个转换节点，转换回表达式
					 * 所预期的原始类型。
					 */
					r = makeNode(ConvertRowtypeExpr);
					r->arg = (Expr *) newvar;
					r->resulttype = var->vartype;
					r->convertformat = COERCE_IMPLICIT_CAST;
					r->location = -1;

					return (Node *) r;
				}
			}
			return (Node *) newvar;
		}
		/* otherwise fall through to copy the var normally */
	}
	else if (IsA(node, ConvertRowtypeExpr))
	{
		ConvertRowtypeExpr *r = (ConvertRowtypeExpr *) node;
		Var		   *var = (Var *) r->arg;

		/*
		 * 若这是在强制转换我们需要转换的整行 Var，则
		 * 只转换该 Var，不再额外加 ConvertRowtypeExpr。
		 * 效果上是把 var::parenttype::grandparenttype 简化成
		 * 只剩 var::grandparenttype。这样若本函数被反复应用，
		 * 就可以避免堆起一叠 CREs。
		 */
		if (IsA(var, Var) &&
			var->varno == context->target_varno &&
			var->varlevelsup == context->sublevels_up &&
			var->varattno == 0 &&
			OidIsValid(context->to_rowtype) &&
			context->to_rowtype != var->vartype)
		{
			ConvertRowtypeExpr *newnode;
			Var		   *newvar = (Var *) palloc(sizeof(Var));

			/* 整行变量，警告调用者 */
			*(context->found_whole_row) = true;

			*newvar = *var;		/* 先复制该 Var 的全部成员 */

			/* 这对 RECORD 变量肯定行不通。 */
			Assert(var->vartype != RECORDOID);

			/* Var 本身被改成所请求的类型。 */
			newvar->vartype = context->to_rowtype;

			newnode = (ConvertRowtypeExpr *) palloc(sizeof(ConvertRowtypeExpr));
			*newnode = *r;		/* 先复制该 CRE 的全部成员 */
			newnode->arg = (Expr *) newvar;

			return (Node *) newnode;
		}
		/* otherwise fall through to process the expression normally */
	}
	else if (IsA(node, Query))
	{
		/* 递归进入 RTE 子查询或尚未规划的子链接子查询 */
		Query	   *newnode;

		context->sublevels_up++;
		newnode = query_tree_mutator((Query *) node,
									 map_variable_attnos_mutator,
									 (void *) context,
									 0);
		context->sublevels_up--;
		return (Node *) newnode;
	}
	return expression_tree_mutator(node, map_variable_attnos_mutator,
								   (void *) context);
}

Node *
map_variable_attnos(Node *node,
					int target_varno, int sublevels_up,
					const AttrMap *attno_map,
					Oid to_rowtype, bool *found_whole_row)
{
	map_variable_attnos_context context;

	context.target_varno = target_varno;
	context.sublevels_up = sublevels_up;
	context.attno_map = attno_map;
	context.to_rowtype = to_rowtype;
	context.found_whole_row = found_whole_row;

	*found_whole_row = false;

	/*
	 * 必须准备好从 Query 或裸表达式树开始；若
	 * 它是 Query，我们不想递增 sublevels_up。
	 */
	return query_or_expression_tree_mutator(node,
											map_variable_attnos_mutator,
											(void *) &context,
											0);
}


/*
 * ReplaceVarsFromTargetList - 用目标列表里的项替换 Vars。
 *
 * 匹配 target_varno 和 sublevels_up 的 Var 被替换成
 * 目标列表里 resno 匹配的项，若有的话。
 *
 * 若这样的 Var 没有匹配的 resno，动作取决于
 * nomatch_option：
 *	REPLACEVARS_REPORT_ERROR：报错
 *	REPLACEVARS_CHANGE_VARNO：把 Var 的 varno 改成 nomatch_varno
 *	REPLACEVARS_SUBSTITUTE_NULL：把 Var 替换成同类型的 NULL Const。
 *
 * 调用者还必须提供 target_rte，即描述目标
 * 关系的 RTE。这是为了处理引用目标的整行 Var。
 * 我们把这样的 Var 展开成 RowExpr 构造。
 *
 * outer_hasSubLinks 的工作方式与 replace_rte_variables() 相同。
 */

typedef struct
{
	RangeTblEntry *target_rte;
	List	   *targetlist;
	ReplaceVarsNoMatchOption nomatch_option;
	int			nomatch_varno;
} ReplaceVarsFromTargetList_context;

static Node *
ReplaceVarsFromTargetList_callback(Var *var,
								   replace_rte_variables_context *context)
{
	ReplaceVarsFromTargetList_context *rcon = (ReplaceVarsFromTargetList_context *) context->callback_arg;
	TargetEntry *tle;

	if (var->varattno == InvalidAttrNumber)
	{
		/* 必须把整元组引用展开成 RowExpr */
		RowExpr    *rowexpr;
		List	   *colnames;
		List	   *fields;

		/*
		 * 若要为具名行类型的 var 生成展开（即这
		 * 是普通关系 RTE），则必须为
		 * 已删除的列包括哑项。若该 var 是 RECORD（即这是 JOIN），则
		 * 略去已删除的列。后一种情形下，把列名挂到
		 * RowExpr 上，供执行器和 ruleutils.c 使用。
		 */
		expandRTE(rcon->target_rte,
				  var->varno, var->varlevelsup, var->location,
				  (var->vartype != RECORDOID),
				  &colnames, &fields);
		/* 调整生成的按成员 Var…… */
		fields = (List *) replace_rte_variables_mutator((Node *) fields,
														context);
		rowexpr = makeNode(RowExpr);
		rowexpr->args = fields;
		rowexpr->row_typeid = var->vartype;
		rowexpr->row_format = COERCE_IMPLICIT_CAST;
		rowexpr->colnames = (var->vartype == RECORDOID) ? colnames : NIL;
		rowexpr->location = var->location;

		return (Node *) rowexpr;
	}

	/* 引用一个目标列表元素的普通情形 */
	tle = get_tle_by_resno(rcon->targetlist, var->varattno);

	if (tle == NULL || tle->resjunk)
	{
		/* 未能在目标列表里找到该列 */
		switch (rcon->nomatch_option)
		{
			case REPLACEVARS_REPORT_ERROR:
				/* fall through, throw error below */
				break;

			case REPLACEVARS_CHANGE_VARNO:
				var = (Var *) copyObject(var);
				var->varno = rcon->nomatch_varno;
				/* 我们不改动语法引用对象 */
				return (Node *) var;

			case REPLACEVARS_SUBSTITUTE_NULL:
				{
					/*
					 * 若 Var 是域类型，我们必须加一个 CoerceToDomain
					 * 节点，以防有 NOT NULL 域约束。
					 */
					int16		vartyplen;
					bool		vartypbyval;

					get_typlenbyval(var->vartype, &vartyplen, &vartypbyval);
					return coerce_null_to_domain(var->vartype,
												 var->vartypmod,
												 var->varcollid,
												 vartyplen,
												 vartypbyval);
				}
		}
		elog(ERROR, "could not find replacement targetlist entry for attno %d",
			 var->varattno);
		return NULL;			/* 消除编译器告警 */
	}
	else
	{
		/* 复制目标列表项再返回 */
		Expr	   *newnode = copyObject(tle->expr);

		/* 若目标列表项来自更高的查询，必须调整 varlevelsup */
		if (var->varlevelsup > 0)
			IncrementVarSublevelsUp((Node *) newnode, var->varlevelsup, 0);

		/*
		 * 检查目标列表项是否含有 PARAM_MULTIEXPR Param，
		 * 若有则报错。这种情形只可能发生在展开
		 * ON UPDATE 规则的 NEW 变量时，且被引用的目标列表项
		 * 位于原始 UPDATE 命令里，并且是多重赋值的一部分。
		 * 似乎没有实用的办法处理这类情形，除非对
		 * 多重赋值的子查询做多次求值，那会造成
		 * 规则系统使用者大概不愿应对的语义怪异。
		 * 因此把它当作尚未实现的特性。
		 */
		if (contains_multiexpr_param((Node *) newnode, NULL))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("NEW variables in ON UPDATE rules cannot reference columns that are part of a multiple assignment in the subject UPDATE command")));

		return (Node *) newnode;
	}
}

Node *
ReplaceVarsFromTargetList(Node *node,
						  int target_varno, int sublevels_up,
						  RangeTblEntry *target_rte,
						  List *targetlist,
						  ReplaceVarsNoMatchOption nomatch_option,
						  int nomatch_varno,
						  bool *outer_hasSubLinks)
{
	ReplaceVarsFromTargetList_context context;

	context.target_rte = target_rte;
	context.targetlist = targetlist;
	context.nomatch_option = nomatch_option;
	context.nomatch_varno = nomatch_varno;

	return replace_rte_variables(node, target_varno, sublevels_up,
								 ReplaceVarsFromTargetList_callback,
								 (void *) &context,
								 outer_hasSubLinks);
}
