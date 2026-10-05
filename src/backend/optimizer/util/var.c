/*-------------------------------------------------------------------------
 *
 * var.c
 *	  Var node manipulation routines
 *
 * Note: for most purposes, PlaceHolderVar is considered a Var too,
 * even if its contained expression is variable-free.  Also, CurrentOfExpr
 * is treated as a Var for purposes of determining whether an expression
 * contains variables.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/var.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/placeholder.h"
#include "optimizer/prep.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"


typedef struct
{
	Relids		varnos;
	PlannerInfo *root;
	int			sublevels_up;
} pull_varnos_context;

typedef struct
{
	Bitmapset  *varattnos;
	Index		varno;
} pull_varattnos_context;

typedef struct
{
	List	   *vars;
	int			sublevels_up;
} pull_vars_context;

typedef struct
{
	int			var_location;
	int			sublevels_up;
} locate_var_of_level_context;

typedef struct
{
	List	   *varlist;
	int			flags;
} pull_var_clause_context;

typedef struct
{
	PlannerInfo *root;			/* 可能为 NULL！ */
	Query	   *query;			/* 外层 Query */
	int			sublevels_up;
	bool		possible_sublink;	/* 别名里会含有 SubLink 吗？ */
	bool		inserted_sublink;	/* 我们插入过 SubLink 吗？ */
} flatten_join_alias_vars_context;

static bool pull_varnos_walker(Node *node,
							   pull_varnos_context *context);
static bool pull_varattnos_walker(Node *node, pull_varattnos_context *context);
static bool pull_vars_walker(Node *node, pull_vars_context *context);
static bool contain_var_clause_walker(Node *node, void *context);
static bool contain_vars_of_level_walker(Node *node, int *sublevels_up);
static bool locate_var_of_level_walker(Node *node,
									   locate_var_of_level_context *context);
static bool pull_var_clause_walker(Node *node,
								   pull_var_clause_context *context);
static Node *flatten_join_alias_vars_mutator(Node *node,
											 flatten_join_alias_vars_context *context);
static Node *add_nullingrels_if_needed(PlannerInfo *root, Node *newnode,
									   Var *oldvar);
static bool is_standard_join_alias_expression(Node *newnode, Var *oldvar);
static void adjust_standard_join_alias_expression(Node *newnode, Var *oldvar);
static Relids alias_relid_set(Query *query, Relids relids);


/*
 * pull_varnos
 *		建立语法树中出现的全部不同 varno 的集合。
 *		只考虑引用第零层 rtable 项的 varno。
 *
 * 结果包含语法树里 Var.varnullingrels 和
 * PlaceHolderVar.phnullingrels 成员中提到的外连接 relid。
 *
 * 若不需要处理 PlaceHolderVars，可以把 root 传成 NULL。
 * 注意：这用于尚未规划的表达式。
 *
 * 因此它可能发现
 * 裸 SubLinks，若发现了，就需要递归进去寻找
 * 对目标 rtable 层的上层引用！但当我们发现已完成的
 * SubPlan 时，只需查看传给该子计划的参数。
 */
Relids
pull_varnos(PlannerInfo *root, Node *node)
{
	pull_varnos_context context;

	context.varnos = NULL;
	context.root = root;
	context.sublevels_up = 0;

	/*
	 * 必须准备好从 Query 或裸表达式树开始；若
	 * 它是 Query，我们不想增加 sublevels_up。
	 */
	query_or_expression_tree_walker(node,
									pull_varnos_walker,
									(void *) &context,
									0);

	return context.varnos;
}

/*
 * pull_varnos_of_level
 *		建立语法树中出现的全部不同 varno 的集合。
 *		只考虑指定层的 Var。
 */
Relids
pull_varnos_of_level(PlannerInfo *root, Node *node, int levelsup)
{
	pull_varnos_context context;

	context.varnos = NULL;
	context.root = root;
	context.sublevels_up = levelsup;

	/*
	 * 必须准备好从 Query 或裸表达式树开始；若
	 * 它是 Query，我们不想增加 sublevels_up。
	 */
	query_or_expression_tree_walker(node,
									pull_varnos_walker,
									(void *) &context,
									0);

	return context.varnos;
}

static bool
pull_varnos_walker(Node *node, pull_varnos_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up)
		{
			context->varnos = bms_add_member(context->varnos, var->varno);
			context->varnos = bms_add_members(context->varnos,
											  var->varnullingrels);
		}
		return false;
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) node;

		if (context->sublevels_up == 0)
			context->varnos = bms_add_member(context->varnos, cexpr->cvarno);
		return false;
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/*
		 * 若 PlaceHolderVar 不属于目标查询层，则忽略它，
		 * 改为递归进它的表达式，看里面是否含有
		 * 属于目标层的 var。调用者不传 root 指针时
		 * 我们也这么做。（这种情况下大概根本不该见到
		 * PlaceHolderVars，但若见到了，这样处理是
		 * 合理的。）
		 */
		if (phv->phlevelsup == context->sublevels_up &&
			context->root != NULL)
		{
			/*
			 * 理想情况下，PHV 对 context->varnos 的贡献是它的
			 * ph_eval_at 集合。不过这段代码可能在该集合算出之前就被调用。
			 * 若无法找到 PlaceHolderInfo，
			 * 就退回到保守假定：PHV 将在其
			 * 语法层（phv->phrels）求值。
			 *
			 * 另一个问题是，PlaceHolderVar 可能出现在已为
			 * 子 appendrel 翻译过的限定条件或目标列表中。
			 * 这种 PHV 通常是由某个其他关系提供的参数表达式，
			 * 因此从父 appendrel 到子 appendrel 的翻译
			 * 不会改变它的 phrels，我们仍应按字面采用
			 * ph_eval_at。但在边角情形下，PHV 原来的
			 * phrels 可以包含父 appendrel 本身，
			 * 这时翻译后的 PHV 会在 phrels 中含有子 appendrel，
			 * 我们必须把 ph_eval_at 翻译成与之匹配。
			 */
			PlaceHolderInfo *phinfo = NULL;

			if (phv->phlevelsup == 0)
			{
				if (phv->phid < context->root->placeholder_array_size)
					phinfo = context->root->placeholder_array[phv->phid];
			}
			if (phinfo == NULL)
			{
				/* 还没有 PlaceHolderInfo，就用 phrels */
				context->varnos = bms_add_members(context->varnos,
												  phv->phrels);
			}
			else if (bms_equal(phv->phrels, phinfo->ph_var->phrels))
			{
				/* 正常情形：使用 ph_eval_at */
				context->varnos = bms_add_members(context->varnos,
												  phinfo->ph_eval_at);
			}
			else
			{
				/* 已翻译的 PlaceHolderVar：把 ph_eval_at 翻译成与之匹配 */
				Relids		newevalat,
							delta;

				/* 去掉 phv->phrels 中被去掉的部分…… */
				delta = bms_difference(phinfo->ph_var->phrels, phv->phrels);
				newevalat = bms_difference(phinfo->ph_eval_at, delta);
				/* ……然后若那确实属于 ph_eval_at…… */
				if (!bms_equal(newevalat, phinfo->ph_eval_at))
				{
					/* ……再加上被加上的部分 */
					delta = bms_difference(phv->phrels, phinfo->ph_var->phrels);
					newevalat = bms_join(newevalat, delta);
				}
				context->varnos = bms_join(context->varnos,
										   newevalat);
			}

			/*
			 * 这三种情形都要把 phnullingrels 纳入结果。我们
			 * 不必担心可能需要翻译它，因为
			 * appendrel 只翻译基关系的 varno，不翻译外连接。
			 */
			context->varnos = bms_add_members(context->varnos,
											  phv->phnullingrels);
			return false;		/* 不要递归进表达式 */
		}
	}
	else if (IsA(node, Query))
	{
		/* 递归进 RTE 子查询或尚未规划的 sublink 子查询 */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, pull_varnos_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, pull_varnos_walker,
								  (void *) context);
}


/*
 * pull_varattnos
 *		找出表达式树中出现的全部不同属性号，
 *		并把它们加进 *varattnos 的初始内容。
 *		只考虑给定 varno 且 rtable 层为零的 Var。
 *
 * 属性号要加上 FirstLowInvalidHeapAttributeNumber 的偏移，以便
 * 位图表示能纳入系统属性（例如 OID）。
 *
 * 目前这不支持尚未规划的子查询；当前用途
 * 也不需要。但它会处理已经规划好的 SubPlan 节点，
 * 只查看 testexpr 和 args 列表。（子计划无法
 * 再含有对当前层 Var 的其他引用。）
 */
void
pull_varattnos(Node *node, Index varno, Bitmapset **varattnos)
{
	pull_varattnos_context context;

	context.varattnos = *varattnos;
	context.varno = varno;

	(void) pull_varattnos_walker(node, &context);

	*varattnos = context.varattnos;
}

static bool
pull_varattnos_walker(Node *node, pull_varattnos_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == context->varno && var->varlevelsup == 0)
			context->varattnos =
				bms_add_member(context->varattnos,
							   var->varattno - FirstLowInvalidHeapAttributeNumber);
		return false;
	}

	/* 不应当发现尚未规划的子查询 */
	Assert(!IsA(node, Query));

	return expression_tree_walker(node, pull_varattnos_walker,
								  (void *) context);
}


/*
 * pull_vars_of_level
 *		建立给定语法树中引用指定查询层的
 *		全部 Var（以及 PlaceHolderVars）的列表。
 *
 * 注意：这些 Var 并不复制，只是链进列表。
 */
List *
pull_vars_of_level(Node *node, int levelsup)
{
	pull_vars_context context;

	context.vars = NIL;
	context.sublevels_up = levelsup;

	/*
	 * 必须准备好从 Query 或裸表达式树开始；若
	 * 它是 Query，我们不想增加 sublevels_up。
	 */
	query_or_expression_tree_walker(node,
									pull_vars_walker,
									(void *) &context,
									0);

	return context.vars;
}

static bool
pull_vars_walker(Node *node, pull_vars_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up)
			context->vars = lappend(context->vars, var);
		return false;
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up)
			context->vars = lappend(context->vars, phv);
		/* 我们不想查看所含的表达式 */
		return false;
	}
	if (IsA(node, Query))
	{
		/* 递归进 RTE 子查询或尚未规划的 sublink 子查询 */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, pull_vars_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, pull_vars_walker,
								  (void *) context);
}


/*
 * contain_var_clause
 *	  递归扫描子句，看它是否含有任何 Var 节点
 *	  （属于当前查询层）。
 *
 *	  若发现任何 varnode 则返回真。
 *
 * 不检查子查询，因此只能在把
 * sublink 归约成子计划之后使用！
 */
bool
contain_var_clause(Node *node)
{
	return contain_var_clause_walker(node, NULL);
}

static bool
contain_var_clause_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup == 0)
			return true;		/* 中止树遍历并返回真 */
		return false;
	}
	if (IsA(node, CurrentOfExpr))
		return true;
	if (IsA(node, PlaceHolderVar))
	{
		if (((PlaceHolderVar *) node)->phlevelsup == 0)
			return true;		/* 中止树遍历并返回真 */
		/* else fall through to check the contained expr */
	}
	return expression_tree_walker(node, contain_var_clause_walker, context);
}


/*
 * contain_vars_of_level
 *	  递归扫描子句，看它是否含有指定查询层的
 *	  任何 Var 节点。
 *
 *	  若发现任何这样的 Var 则返回真。
 *
 * 会递归进 sublink。也可以直接对 Query 调用。
 */
bool
contain_vars_of_level(Node *node, int levelsup)
{
	int			sublevels_up = levelsup;

	return query_or_expression_tree_walker(node,
										   contain_vars_of_level_walker,
										   (void *) &sublevels_up,
										   0);
}

static bool
contain_vars_of_level_walker(Node *node, int *sublevels_up)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup == *sublevels_up)
			return true;		/* 中止树遍历并返回真 */
		return false;
	}
	if (IsA(node, CurrentOfExpr))
	{
		if (*sublevels_up == 0)
			return true;
		return false;
	}
	if (IsA(node, PlaceHolderVar))
	{
		if (((PlaceHolderVar *) node)->phlevelsup == *sublevels_up)
			return true;		/* 中止树遍历并返回真 */
		/* else fall through to check the contained expr */
	}
	if (IsA(node, Query))
	{
		/* 递归进子选择 */
		bool		result;

		(*sublevels_up)++;
		result = query_tree_walker((Query *) node,
								   contain_vars_of_level_walker,
								   (void *) sublevels_up,
								   0);
		(*sublevels_up)--;
		return result;
	}
	return expression_tree_walker(node,
								  contain_vars_of_level_walker,
								  (void *) sublevels_up);
}


/*
 * locate_var_of_level
 *	  找出指定查询层的任一 Var 的解析位置。
 *
 * 若查询树中没有这样的 Var，或它们的解析位置都
 * 未知，则返回 -1。（前者多半是调用者错误，
 * 但我们不费心把它和后者区分开。）
 *
 * 会递归进 sublink。也可以直接对 Query 调用。
 *
 * 注意：把这项功能并进
 * contain_vars_of_level 看似合适，但那会把该函数的 API 弄复杂。
 * 目前这个函数只用于报错，
 * 因此省几个周期大概并不重要。
 */
int
locate_var_of_level(Node *node, int levelsup)
{
	locate_var_of_level_context context;

	context.var_location = -1;	/* 以防什么都找不到 */
	context.sublevels_up = levelsup;

	(void) query_or_expression_tree_walker(node,
										   locate_var_of_level_walker,
										   (void *) &context,
										   0);

	return context.var_location;
}

static bool
locate_var_of_level_walker(Node *node,
						   locate_var_of_level_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up &&
			var->location >= 0)
		{
			context->var_location = var->location;
			return true;		/* 中止树遍历并返回真 */
		}
		return false;
	}
	if (IsA(node, CurrentOfExpr))
	{
		/* 既然 CurrentOfExpr 不携带位置，我们也无能为力 */
		return false;
	}
	/* PlaceHolderVar 不需要额外代码；只查看所含表达式 */
	if (IsA(node, Query))
	{
		/* 递归进子选择 */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   locate_var_of_level_walker,
								   (void *) context,
								   0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node,
								  locate_var_of_level_walker,
								  (void *) context);
}


/*
 * pull_var_clause
 *	  从表达式子句中递归抽出全部 Var 节点。
 *
 *	  Aggref 按 flags 中的这些位处理：
 *		PVC_INCLUDE_AGGREGATES		把 Aggref 纳入输出列表
 *		PVC_RECURSE_AGGREGATES		递归进 Aggref 参数
 *		两个标志都没有				发现 Aggref 就报错
 *	  只有指定了 PVC_RECURSE_AGGREGATES 时，
 *	  Aggref 表达式里的 Var 才会纳入结果。
 *
 *	  WindowFuncs 按 flags 中的这些位处理：
 *		PVC_INCLUDE_WINDOWFUNCS		把 WindowFuncs 纳入输出列表
 *		PVC_RECURSE_WINDOWFUNCS		递归进 WindowFunc 参数
 *		两个标志都没有				发现 WindowFunc 就报错
 *	  只有指定了 PVC_RECURSE_WINDOWFUNCS 时，
 *	  WindowFunc 表达式里的 Var 才会纳入结果。
 *
 *	  PlaceHolderVars 按 flags 中的这些位处理：
 *		PVC_INCLUDE_PLACEHOLDERS	把 PlaceHolderVars 纳入输出列表
 *		PVC_RECURSE_PLACEHOLDERS	递归进 PlaceHolderVar 参数
 *		两个标志都没有				发现 PlaceHolderVar 就报错
 *	  只有指定了 PVC_RECURSE_PLACEHOLDERS 时，
 *	  PHV 表达式里的 Var 才会纳入结果。
 *
 *	  GroupingFuncs 的处理与 Aggref 完全相同，因此不需要
 *	  自己的标志位。
 *
 *	  任何情况下都忽略 CurrentOfExpr 节点。
 *
 *	  这里不应当见到上层 var（varlevelsup > 0），
 *	  上层 Aggref 和 PlaceHolderVars 也同样不应当见到。
 *
 *	  返回找到的节点列表。注意节点本身并不
 *	  复制，只是被引用。
 *
 * 不检查子查询，因此只能在把
 * sublink 归约成子计划之后使用！
 */
List *
pull_var_clause(Node *node, int flags)
{
	pull_var_clause_context context;

	/* 断言调用者没有指定互相矛盾的标志 */
	Assert((flags & (PVC_INCLUDE_AGGREGATES | PVC_RECURSE_AGGREGATES))
		   != (PVC_INCLUDE_AGGREGATES | PVC_RECURSE_AGGREGATES));
	Assert((flags & (PVC_INCLUDE_WINDOWFUNCS | PVC_RECURSE_WINDOWFUNCS))
		   != (PVC_INCLUDE_WINDOWFUNCS | PVC_RECURSE_WINDOWFUNCS));
	Assert((flags & (PVC_INCLUDE_PLACEHOLDERS | PVC_RECURSE_PLACEHOLDERS))
		   != (PVC_INCLUDE_PLACEHOLDERS | PVC_RECURSE_PLACEHOLDERS));

	context.varlist = NIL;
	context.flags = flags;

	pull_var_clause_walker(node, &context);
	return context.varlist;
}

static bool
pull_var_clause_walker(Node *node, pull_var_clause_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup != 0)
			elog(ERROR, "Upper-level Var found where not expected");
		context->varlist = lappend(context->varlist, node);
		return false;
	}
	else if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->agglevelsup != 0)
			elog(ERROR, "Upper-level Aggref found where not expected");
		if (context->flags & PVC_INCLUDE_AGGREGATES)
		{
			context->varlist = lappend(context->varlist, node);
			/* 我们不下降到所含的表达式里 */
			return false;
		}
		else if (context->flags & PVC_RECURSE_AGGREGATES)
		{
			/* fall through to recurse into the aggregate's arguments */
		}
		else
			elog(ERROR, "Aggref found where not expected");
	}
	else if (IsA(node, GroupingFunc))
	{
		if (((GroupingFunc *) node)->agglevelsup != 0)
			elog(ERROR, "Upper-level GROUPING found where not expected");
		if (context->flags & PVC_INCLUDE_AGGREGATES)
		{
			context->varlist = lappend(context->varlist, node);
			/* 我们不下降到所含的表达式里 */
			return false;
		}
		else if (context->flags & PVC_RECURSE_AGGREGATES)
		{
			/* fall through to recurse into the GroupingFunc's arguments */
		}
		else
			elog(ERROR, "GROUPING found where not expected");
	}
	else if (IsA(node, WindowFunc))
	{
		/* WindowFuncs 没有 levelsup 成员可检查…… */
		if (context->flags & PVC_INCLUDE_WINDOWFUNCS)
		{
			context->varlist = lappend(context->varlist, node);
			/* 我们不下降到所含的表达式里 */
			return false;
		}
		else if (context->flags & PVC_RECURSE_WINDOWFUNCS)
		{
			/* fall through to recurse into the windowfunc's arguments */
		}
		else
			elog(ERROR, "WindowFunc found where not expected");
	}
	else if (IsA(node, PlaceHolderVar))
	{
		if (((PlaceHolderVar *) node)->phlevelsup != 0)
			elog(ERROR, "Upper-level PlaceHolderVar found where not expected");
		if (context->flags & PVC_INCLUDE_PLACEHOLDERS)
		{
			context->varlist = lappend(context->varlist, node);
			/* 我们不下降到所含的表达式里 */
			return false;
		}
		else if (context->flags & PVC_RECURSE_PLACEHOLDERS)
		{
			/* fall through to recurse into the placeholder's expression */
		}
		else
			elog(ERROR, "PlaceHolderVar found where not expected");
	}
	return expression_tree_walker(node, pull_var_clause_walker,
								  (void *) context);
}


/*
 * flatten_join_alias_vars
 *	  把引用 JOIN 输出的 Var 换成对原来
 *	  关系变量的引用。这样涉及这些 var 的限定条件就可以
 *	  下推。引用 JOIN 关系的整行 Var 会被展开成
 *	  点名各个输出 Var 的 RowExpr 构造。这是
 *	  必要的，因为我们不会把 JOIN 当作基关系来扫描，而那
 *	  是执行器能直接处理整行 Var 的唯一方式。
 *
 * 这也会调整某些表达式节点类型中的 relid 集合，
 * 用所含的基关系加外连接关系替换任何连接 relid。
 *
 * 若 JOIN 含有已被展平的子选择，它的连接别名
 * 项现在可能是任意表达式，而不只是 Var。这从两个
 * 重要方面影响本函数。第一，我们可能发现自己把
 * SubLink 表达式插进子查询，若如此必须确保
 * 它们的 Query.hasSubLinks 成员被设为真。若连接别名列表里
 * 已有任何 SubLinks，外层 Query 应当已经
 * hasSubLinks = true，因此这只与尚未展平的子查询有关。
 * 第二，我们必须保留挂在被替换的
 * 别名 Var 上的任何 varnullingrels 信息。若替换表达式是 Var 或
 * PlaceHolderVar，或由它们构造而成，我们可以把
 * varnullingrels 位加到已有的 nullingrels 成员上；否则
 * 就必须加上一层 PlaceHolderVar 包装。
 *
 * 注意：解析器也会用它，在检查 GROUP BY 有效性之前
 * 展开连接别名 Var。那种用法下 root 将为 NULL，这就是
 * 为什么必须把 Query 单独传进来。我们只在
 * 构造 PlaceHolderVars 时才需要 root 本身。解析器的用法里
 * 可以避免构造 PlaceHolderVars，因为它不会处理任意表达式：
 * 只要 adjust_standard_join_alias_expression 能处理解析器
 * 会做成连接别名表达式的一切，就没问题。
 */
Node *
flatten_join_alias_vars(PlannerInfo *root, Query *query, Node *node)
{
	flatten_join_alias_vars_context context;

	/*
	 * 我们不预期把它用到整个 Query 上，只用到
	 * 表达式或 LATERAL 子查询上。因此若顶层节点是 Query，
	 * 可以立刻增加 sublevels_up。
	 */
	Assert(node != (Node *) query);

	context.root = root;
	context.query = query;
	context.sublevels_up = 0;
	/* 标记连接别名是否可能含有 SubLinks */
	context.possible_sublink = query->hasSubLinks;
	/* 若 hasSubLinks 已经为真，就不必费力 */
	context.inserted_sublink = query->hasSubLinks;

	return flatten_join_alias_vars_mutator(node, &context);
}

static Node *
flatten_join_alias_vars_mutator(Node *node,
								flatten_join_alias_vars_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		RangeTblEntry *rte;
		Node	   *newvar;

		/* 除非 Var 属于目标层的 JOIN，否则不变 */
		if (var->varlevelsup != context->sublevels_up)
			return node;		/* 其实不必复制 */
		rte = rt_fetch(var->varno, context->query->rtable);
		if (rte->rtekind != RTE_JOIN)
			return node;
		if (var->varattno == InvalidAttrNumber)
		{
			/* 必须展开整行引用 */
			RowExpr    *rowexpr;
			List	   *fields = NIL;
			List	   *colnames = NIL;
			ListCell   *lv;
			ListCell   *ln;

			Assert(list_length(rte->joinaliasvars) == list_length(rte->eref->colnames));
			forboth(lv, rte->joinaliasvars, ln, rte->eref->colnames)
			{
				newvar = (Node *) lfirst(lv);
				/* 忽略已删除的列 */
				if (newvar == NULL)
					continue;
				newvar = copyObject(newvar);

				/*
				 * 若正在展开从上层查询传下来的别名，
				 * 必须调整它的 varlevelsup 成员。
				 */
				if (context->sublevels_up != 0)
					IncrementVarSublevelsUp(newvar, context->sublevels_up, 0);
				/* 若可能，保留原来 Var 的位置 */
				if (IsA(newvar, Var))
					((Var *) newvar)->location = var->location;
				/* 递归，以防连接输入本身也是连接 */
				/* （若需要，也负责设置 inserted_sublink） */
				newvar = flatten_join_alias_vars_mutator(newvar, context);
				fields = lappend(fields, newvar);
				/* 我们也需要未删除列的名字 */
				colnames = lappend(colnames, copyObject((Node *) lfirst(ln)));
			}
			rowexpr = makeNode(RowExpr);
			rowexpr->args = fields;
			rowexpr->row_typeid = var->vartype;
			rowexpr->row_format = COERCE_IMPLICIT_CAST;
			/* vartype 总是 RECORDOID，因此总是需要列名 */
			rowexpr->colnames = colnames;
			rowexpr->location = var->location;

			/* 最后，把任何 varnullingrels 加到替换表达式上 */
			return add_nullingrels_if_needed(context->root, (Node *) rowexpr,
											 var);
		}

		/* 展开连接别名引用 */
		Assert(var->varattno > 0);
		newvar = (Node *) list_nth(rte->joinaliasvars, var->varattno - 1);
		Assert(newvar != NULL);
		newvar = copyObject(newvar);

		/*
		 * 若正在展开从上层查询传下来的别名，必须
		 * 调整它的 varlevelsup 成员。
		 */
		if (context->sublevels_up != 0)
			IncrementVarSublevelsUp(newvar, context->sublevels_up, 0);

		/* 若可能，保留原来 Var 的位置 */
		if (IsA(newvar, Var))
			((Var *) newvar)->location = var->location;

		/* 递归，以防连接输入本身也是连接 */
		newvar = flatten_join_alias_vars_mutator(newvar, context);

		/* 检测是否在往查询里加 sublink */
		if (context->possible_sublink && !context->inserted_sublink)
			context->inserted_sublink = checkExprHasSubLink(newvar);

		/* 最后，把任何 varnullingrels 加到替换表达式上 */
		return add_nullingrels_if_needed(context->root, newvar, var);
	}
	if (IsA(node, PlaceHolderVar))
	{
		/* 复制 PlaceHolderVar 节点，并正确改写子节点 */
		PlaceHolderVar *phv;

		phv = (PlaceHolderVar *) expression_tree_mutator(node,
														 flatten_join_alias_vars_mutator,
														 (void *) context);
		/* 现在修正 PlaceHolderVar 的 relid 集合 */
		if (phv->phlevelsup == context->sublevels_up)
		{
			phv->phrels = alias_relid_set(context->query,
										  phv->phrels);
			/* 我们不改 phnullingrels */
		}
		return (Node *) phv;
	}

	if (IsA(node, Query))
	{
		/* 递归进 RTE 子查询或尚未规划的 sublink 子查询 */
		Query	   *newnode;
		bool		save_inserted_sublink;

		context->sublevels_up++;
		save_inserted_sublink = context->inserted_sublink;
		context->inserted_sublink = ((Query *) node)->hasSubLinks;
		newnode = query_tree_mutator((Query *) node,
									 flatten_join_alias_vars_mutator,
									 (void *) context,
									 QTW_IGNORE_JOINALIASES);
		newnode->hasSubLinks |= context->inserted_sublink;
		context->inserted_sublink = save_inserted_sublink;
		context->sublevels_up--;
		return (Node *) newnode;
	}
	/* 不支持已经规划好的树 */
	Assert(!IsA(node, SubPlan));
	/* 这里应当不必处理这些规划器辅助节点 */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	return expression_tree_mutator(node, flatten_join_alias_vars_mutator,
								   (void *) context);
}

/*
 * 把 oldvar 的 varnullingrels（若有）加到展平后的连接别名表达式上。
 * newnode 已经复制过，因此可以随意修改。
 */
static Node *
add_nullingrels_if_needed(PlannerInfo *root, Node *newnode, Var *oldvar)
{
	if (oldvar->varnullingrels == NULL)
		return newnode;			/* 无事可做 */
	/* 若可能，就加到已有的 nullingrel 成员上 */
	if (is_standard_join_alias_expression(newnode, oldvar))
		adjust_standard_join_alias_expression(newnode, oldvar);
	else if (root)
	{
		/*
		 * 我们可以插入一个 PlaceHolderVar 来携带 nullingrels。不过
		 * 决定在何处求值这个 PHV 略有些棘手。我们先
		 * 试着在新表达式的自然语义层求值它；
		 * 但若该表达式不含变量，就退回到
		 * 在 oldvar 作为别名 Var 所对应的那个连接上求值。
		 */
		PlaceHolderVar *newphv;
		Index		levelsup = oldvar->varlevelsup;
		Relids		phrels = pull_varnos_of_level(root, newnode, levelsup);

		if (bms_is_empty(phrels))	/* 不含变量？ */
		{
			if (levelsup != 0)	/* 否则这样行不通 */
				elog(ERROR, "unsupported join alias expression");
			phrels = get_relids_for_join(root->parse, oldvar->varno);
			/* 若它是外连接，就在连接之下而不是之上求值 */
			phrels = bms_del_member(phrels, oldvar->varno);
			Assert(!bms_is_empty(phrels));
		}
		newphv = make_placeholder_expr(root, (Expr *) newnode, phrels);
		/* newphv 的 phlevelsup 为零且 phnullingrels 为 NULL；修正它 */
		newphv->phlevelsup = levelsup;
		newphv->phnullingrels = bms_copy(oldvar->varnullingrels);
		newnode = (Node *) newphv;
	}
	else
	{
		/* 糟糕，解析器能构造的某种东西我们还缺少支持 */
		elog(ERROR, "unsupported join alias expression");
	}
	return newnode;
}

/*
 * 检查能否不靠单独的 PlaceHolderVar，就把 nullingrels
 * 插进这个连接别名表达式。
 *
 * 这能处理 Var、PlaceHolderVars，以及由它们构成的隐式强制转换和 COALESCE
 * 表达式。覆盖范围需要能处理解析器
 * 会放进 joinaliasvars 的任何东西。
 */
static bool
is_standard_join_alias_expression(Node *newnode, Var *oldvar)
{
	if (newnode == NULL)
		return false;
	if (IsA(newnode, Var) &&
		((Var *) newnode)->varlevelsup == oldvar->varlevelsup)
		return true;
	else if (IsA(newnode, PlaceHolderVar) &&
			 ((PlaceHolderVar *) newnode)->phlevelsup == oldvar->varlevelsup)
		return true;
	else if (IsA(newnode, FuncExpr))
	{
		FuncExpr   *fexpr = (FuncExpr *) newnode;

		/*
		 * 我们需要假定该函数不会从
		 * NULL 产生非 NULL，这对隐式强制转换是合理的，对其他情形
		 * 就没那么合理。（查看它是否严格多半是杀鸡用牛刀，而且
		 * 若有人忘了把隐式
		 * 强制转换标成严格，还会导致我们失败。）
		 */
		if (fexpr->funcformat != COERCE_IMPLICIT_CAST ||
			fexpr->args == NIL)
			return false;

		/*
		 * 只检查第一个参数——强制转换可能还有额外的
		 * 常量参数。
		 */
		return is_standard_join_alias_expression(linitial(fexpr->args), oldvar);
	}
	else if (IsA(newnode, RelabelType))
	{
		RelabelType *relabel = (RelabelType *) newnode;

		/* 这肯定不会从 NULL 产生非 NULL */
		return is_standard_join_alias_expression((Node *) relabel->arg, oldvar);
	}
	else if (IsA(newnode, CoerceViaIO))
	{
		CoerceViaIO *iocoerce = (CoerceViaIO *) newnode;

		/* 这肯定不会从 NULL 产生非 NULL */
		return is_standard_join_alias_expression((Node *) iocoerce->arg, oldvar);
	}
	else if (IsA(newnode, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) newnode;

		/* 这肯定不会从 NULL 产生非 NULL（在数组这一层） */
		return is_standard_join_alias_expression((Node *) acoerce->arg, oldvar);
	}
	else if (IsA(newnode, CoalesceExpr))
	{
		CoalesceExpr *cexpr = (CoalesceExpr *) newnode;
		ListCell   *lc;

		Assert(cexpr->args != NIL);
		foreach(lc, cexpr->args)
		{
			if (!is_standard_join_alias_expression(lfirst(lc), oldvar))
				return false;
		}
		return true;
	}
	else
		return false;
}

/*
 * 把 nullingrels 插进
 * is_standard_join_alias_expression 所接受的表达式。
 */
static void
adjust_standard_join_alias_expression(Node *newnode, Var *oldvar)
{
	if (IsA(newnode, Var) &&
		((Var *) newnode)->varlevelsup == oldvar->varlevelsup)
	{
		Var		   *newvar = (Var *) newnode;

		newvar->varnullingrels = bms_add_members(newvar->varnullingrels,
												 oldvar->varnullingrels);
	}
	else if (IsA(newnode, PlaceHolderVar) &&
			 ((PlaceHolderVar *) newnode)->phlevelsup == oldvar->varlevelsup)
	{
		PlaceHolderVar *newphv = (PlaceHolderVar *) newnode;

		newphv->phnullingrels = bms_add_members(newphv->phnullingrels,
												oldvar->varnullingrels);
	}
	else if (IsA(newnode, FuncExpr))
	{
		FuncExpr   *fexpr = (FuncExpr *) newnode;

		adjust_standard_join_alias_expression(linitial(fexpr->args), oldvar);
	}
	else if (IsA(newnode, RelabelType))
	{
		RelabelType *relabel = (RelabelType *) newnode;

		adjust_standard_join_alias_expression((Node *) relabel->arg, oldvar);
	}
	else if (IsA(newnode, CoerceViaIO))
	{
		CoerceViaIO *iocoerce = (CoerceViaIO *) newnode;

		adjust_standard_join_alias_expression((Node *) iocoerce->arg, oldvar);
	}
	else if (IsA(newnode, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) newnode;

		adjust_standard_join_alias_expression((Node *) acoerce->arg, oldvar);
	}
	else if (IsA(newnode, CoalesceExpr))
	{
		CoalesceExpr *cexpr = (CoalesceExpr *) newnode;
		ListCell   *lc;

		Assert(cexpr->args != NIL);
		foreach(lc, cexpr->args)
		{
			adjust_standard_join_alias_expression(lfirst(lc), oldvar);
		}
	}
	else
		Assert(false);
}

/*
 * alias_relid_set：在一组 RT 下标中，把连接替换成它们
 * 底层的基关系加外连接 relid
 */
static Relids
alias_relid_set(Query *query, Relids relids)
{
	Relids		result = NULL;
	int			rtindex;

	rtindex = -1;
	while ((rtindex = bms_next_member(relids, rtindex)) >= 0)
	{
		RangeTblEntry *rte = rt_fetch(rtindex, query->rtable);

		if (rte->rtekind == RTE_JOIN)
			result = bms_join(result, get_relids_for_join(query, rtindex));
		else
			result = bms_add_member(result, rtindex);
	}
	return result;
}
