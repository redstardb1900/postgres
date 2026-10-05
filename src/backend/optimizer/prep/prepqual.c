/*-------------------------------------------------------------------------
 *
 * prepqual.c
 *	  预处理限定表达式的例程
 *
 *
 * 解析器会从简单的 AND 或 OR 连接的子句序列产生
 * 平坦的（N 元）AND/OR 树，但若输入的括号不寻常，
 * 仍可能有一个 AND 子句直接位于另一个 AND 之下，或 OR 位于 OR 之下。
 * 规则展开和子查询展平也可能产生这种语法分析树。规划器希望
 * 把所有这些情况都展平，
 * 以保证规划行为一致。
 *
 * 以前，本模块负责做最初的展平，
 * 但现在交给 eval_const_expressions 来做，因为它反正要对
 * 表达式树做完整的一遍扫描。现在我们只需要
 * 确保自己的操作保持 AND/OR 的平坦性。
 * pull_ands() 与 pull_ors() 用来在局部变换之后维持 AND/OR
 * 树的平坦性，这些变换可能会引入嵌套的 AND/OR。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepqual.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/prep.h"
#include "utils/lsyscache.h"


static List *pull_ands(List *andlist);
static List *pull_ors(List *orlist);
static Expr *find_duplicate_ors(Expr *qual, bool is_check);
static Expr *process_duplicate_ors(List *orlist);


/*
 * negate_clause
 *	    对一个布尔表达式取反。
 *
 * 输入是要被否定的子句（例如 NOT 子句的参数）。
 * 返回一个等价于给定子句之否定的新子句。
 *
 * 虽然这可以单独调用，它主要是作为
 * eval_const_expressions() 的辅助函数，那个上下文驱动了若干设计
 * 决定。尤其是，若输入已经是 AND/OR 平坦的，我们必须
 * 保持该性质。在可以假定更低层执行的 eval_const_expressions
 * 已经简化了输入的子子句的情况下，我们也不费心递归。
 * 这与简单的 make_notclause() 的区别是，这
 *
 * 试图用逻辑简化去掉 NOT 节点。若 NOT 节点能被整个去掉，
 * 那显然总是赚的。不过，我们
 * 使用 DeMorgan 定律可能导致 NOT 节点变多而不是
 * 变少。我们反正无条件这么做，因为在 WHERE 子句里，
 * 尽可能暴露顶层 AND/OR 结构很重要。
 * 另外，去掉一个中间的 NOT 也许能让我们把两层
 * AND 或 OR 展平到一起，否则做不到。
 * 最后，做这件事的动机之一
 * 是确保逻辑等价的
 * 表达式会被看成物理上 equal()，因此我们应当总是应用
 * 同样的变换。
 */
Node *
negate_clause(Node *node)
{
	if (node == NULL)			/* 不应当发生 */
		elog(ERROR, "can't negate an empty subexpression");
	switch (nodeTag(node))
	{
		case T_Const:
			{
				Const	   *c = (Const *) node;

				/* NOT NULL 仍然是 NULL */
				if (c->constisnull)
					return makeBoolConst(false, true);
				/* 否则相当容易 */
				return makeBoolConst(!DatumGetBool(c->constvalue), false);
			}
			break;
		case T_OpExpr:
			{
				/*
				 * 若可能，对操作符取反：(NOT (< A B)) => (>= A B)
				 */
				OpExpr	   *opexpr = (OpExpr *) node;
				Oid			negator = get_negator(opexpr->opno);

				if (negator)
				{
					OpExpr	   *newopexpr = makeNode(OpExpr);

					newopexpr->opno = negator;
					newopexpr->opfuncid = InvalidOid;
					newopexpr->opresulttype = opexpr->opresulttype;
					newopexpr->opretset = opexpr->opretset;
					newopexpr->opcollid = opexpr->opcollid;
					newopexpr->inputcollid = opexpr->inputcollid;
					newopexpr->args = opexpr->args;
					newopexpr->location = opexpr->location;
					return (Node *) newopexpr;
				}
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				/*
				 * 若 ScalarArrayOpExpr 的操作符有否定符，就对它取反；
				 * 例如 x = ANY (list) 变成 x <> ALL (list)
				 */
				ScalarArrayOpExpr *saopexpr = (ScalarArrayOpExpr *) node;
				Oid			negator = get_negator(saopexpr->opno);

				if (negator)
				{
					ScalarArrayOpExpr *newopexpr = makeNode(ScalarArrayOpExpr);

					newopexpr->opno = negator;
					newopexpr->opfuncid = InvalidOid;
					newopexpr->hashfuncid = InvalidOid;
					newopexpr->negfuncid = InvalidOid;
					newopexpr->useOr = !saopexpr->useOr;
					newopexpr->inputcollid = saopexpr->inputcollid;
					newopexpr->args = saopexpr->args;
					newopexpr->location = saopexpr->location;
					return (Node *) newopexpr;
				}
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				switch (expr->boolop)
				{
						/*--------------------
						 * Apply DeMorgan's Laws:
						 *		(NOT (AND A B)) => (OR (NOT A) (NOT B))
						 *		(NOT (OR A B))	=> (AND (NOT A) (NOT B))
						 * i.e., swap AND for OR and negate each subclause.
						 *
						 * If the input is already AND/OR flat and has no NOT
						 * directly above AND or OR, this transformation preserves
						 * those properties.  For example, if no direct child of
						 * the given AND clause is an AND or a NOT-above-OR, then
						 * the recursive calls of negate_clause() can't return any
						 * OR clauses.  So we needn't call pull_ors() before
						 * building a new OR clause.  Similarly for the OR case.
						 *--------------------
						 */
					case AND_EXPR:
						{
							List	   *nargs = NIL;
							ListCell   *lc;

							foreach(lc, expr->args)
							{
								nargs = lappend(nargs,
												negate_clause(lfirst(lc)));
							}
							return (Node *) make_orclause(nargs);
						}
						break;
					case OR_EXPR:
						{
							List	   *nargs = NIL;
							ListCell   *lc;

							foreach(lc, expr->args)
							{
								nargs = lappend(nargs,
												negate_clause(lfirst(lc)));
							}
							return (Node *) make_andclause(nargs);
						}
						break;
					case NOT_EXPR:

						/*
						 * NOT 下面还有 NOT：它们抵消。我们假定
						 * 输入已经简化过，因此不必递归。
						 */
						return (Node *) linitial(expr->args);
					default:
						elog(ERROR, "unrecognized boolop: %d",
							 (int) expr->boolop);
						break;
				}
			}
			break;
		case T_NullTest:
			{
				NullTest   *expr = (NullTest *) node;

				/*
				 * 在行类型情形下，两种 NullTest *并不是*
				 * 逻辑反面，因此没法简化。但对
				 * 标量数据类型是可以的。
				 */
				if (!expr->argisrow)
				{
					NullTest   *newexpr = makeNode(NullTest);

					newexpr->arg = expr->arg;
					newexpr->nulltesttype = (expr->nulltesttype == IS_NULL ?
											 IS_NOT_NULL : IS_NULL);
					newexpr->argisrow = expr->argisrow;
					newexpr->location = expr->location;
					return (Node *) newexpr;
				}
			}
			break;
		case T_BooleanTest:
			{
				BooleanTest *expr = (BooleanTest *) node;
				BooleanTest *newexpr = makeNode(BooleanTest);

				newexpr->arg = expr->arg;
				switch (expr->booltesttype)
				{
					case IS_TRUE:
						newexpr->booltesttype = IS_NOT_TRUE;
						break;
					case IS_NOT_TRUE:
						newexpr->booltesttype = IS_TRUE;
						break;
					case IS_FALSE:
						newexpr->booltesttype = IS_NOT_FALSE;
						break;
					case IS_NOT_FALSE:
						newexpr->booltesttype = IS_FALSE;
						break;
					case IS_UNKNOWN:
						newexpr->booltesttype = IS_NOT_UNKNOWN;
						break;
					case IS_NOT_UNKNOWN:
						newexpr->booltesttype = IS_UNKNOWN;
						break;
					default:
						elog(ERROR, "unrecognized booltesttype: %d",
							 (int) expr->booltesttype);
						break;
				}
				newexpr->location = expr->location;
				return (Node *) newexpr;
			}
			break;
		default:
			/* else fall through */
			break;
	}

	/*
	 * 否则我们不知道如何简化这个，因此就贴上一个
	 * 显式 NOT 节点。
	 */
	return (Node *) make_notclause((Expr *) node);
}


/*
 * canonicalize_qual
 *	    把限定表达式转换成最有用的形式。
 *
 * 这主要是为了用在顶层 WHERE（或 JOIN/ON）
 * 子句上。也可以用在顶层 CHECK 约束上，那种情况
 * 传入 is_check = true。不要对任何并不知道
 * 是这两者之一的表达式调用它，因为它也许会应用不适当的简化。
 *
 * 这个例程的名字是从它曾试图
 * 把表达式强制成规范的 AND 套 OR 或 OR 套 ANDs 形式的时代留下来的。
 * 后来我们认识到那的理论纯粹性多于
 * 实际用处，因此现在的变换不涉及任何
 * 达到规范形式的概念。
 *
 * 注意：我们假定输入已经过 eval_const_expressions，
 * 因此具有 AND/OR 平坦性。以前这个函数包含
 * 自己的展平逻辑，但那需要多做一趟无用的
 * 树遍历。
 *
 * 返回修改后的限定条件。
 */
Expr *
canonicalize_qual(Expr *qual, bool is_check)
{
	Expr	   *newqual;

	/* 空限定条件时快速退出 */
	if (qual == NULL)
		return NULL;

	/* 不应当对隐式 AND 格式的限定条件调用这个 */
	Assert(!IsA(qual, List));

	/*
	 * 上拉 OR 套 AND 树里的冗余子子句。我们只在
	 * 顶层 AND/OR 结构内做这件事；再往
	 * 深处看没有意义。也去掉顶层结构里的任何 NULL 常量。
	 */
	newqual = find_duplicate_ors(qual, is_check);

	return newqual;
}


/*
 * pull_ands
 *	    递归把嵌套的 AND 子句展平成单个 and 子句列表。
 *
 * 输入是一个 AND 子句的参数列表。
 * 返回重建的参数列表（注意原来的列表结构不被触碰）。
 */
static List *
pull_ands(List *andlist)
{
	List	   *out_list = NIL;
	ListCell   *arg;

	foreach(arg, andlist)
	{
		Node	   *subexpr = (Node *) lfirst(arg);

		if (is_andclause(subexpr))
			out_list = list_concat(out_list,
								   pull_ands(((BoolExpr *) subexpr)->args));
		else
			out_list = lappend(out_list, subexpr);
	}
	return out_list;
}

/*
 * pull_ors
 *	    递归把嵌套的 OR 子句展平成单个 or 子句列表。
 *
 * 输入是一个 OR 子句的参数列表。
 * 返回重建的参数列表（注意原来的列表结构不被触碰）。
 */
static List *
pull_ors(List *orlist)
{
	List	   *out_list = NIL;
	ListCell   *arg;

	foreach(arg, orlist)
	{
		Node	   *subexpr = (Node *) lfirst(arg);

		if (is_orclause(subexpr))
			out_list = list_concat(out_list,
								   pull_ors(((BoolExpr *) subexpr)->args));
		else
			out_list = lappend(out_list, subexpr);
	}
	return out_list;
}


/*--------------------
 * The following code attempts to apply the inverse OR distributive law:
 *		((A AND B) OR (A AND C))  =>  (A AND (B OR C))
 * That is, locate OR clauses in which every subclause contains an
 * identical term, and pull out the duplicated terms.
 *
 * This may seem like a fairly useless activity, but it turns out to be
 * applicable to many machine-generated queries, and there are also queries
 * in some of the TPC benchmarks that need it.  This was in fact almost the
 * sole useful side-effect of the old prepqual code that tried to force
 * the query into canonical AND-of-ORs form: the canonical equivalent of
 *		((A AND B) OR (A AND C))
 * is
 *		((A OR A) AND (A OR C) AND (B OR A) AND (B OR C))
 * which the code was able to simplify to
 *		(A AND (A OR C) AND (B OR A) AND (B OR C))
 * thus successfully extracting the common condition A --- but at the cost
 * of cluttering the qual with many redundant clauses.
 *--------------------
 */

/*
 * find_duplicate_ors
 *	    给定一棵 NOTs 已下推的限定条件树，搜索
 *	    逆 OR 分配律也许适用的 OR 子句。
 *	    只搜索顶层 AND/OR 结构。
 *
 * 顺便去掉顶层 AND/OR
 * 结构里的任何 NULL 常量，例如在 WHERE 子句里，"x OR NULL::boolean" 收成 "x"。
 * 一般而言那会改变结果，因此 eval_const_expressions 不能
 * 这么做；但在 WHERE 的顶层，我们不必区分
 * FALSE 和 NULL 结果，因此把 NULL::boolean 当成
 * FALSE 再相应地简化 AND/OR 是有效的。反过来，在顶层
 * CHECK 约束里，我们可以把 NULL 当成 TRUE。
 *
 * 返回修改后的限定条件。AND/OR 平坦性得以保持。
 */
static Expr *
find_duplicate_ors(Expr *qual, bool is_check)
{
	if (is_orclause(qual))
	{
		List	   *orlist = NIL;
		ListCell   *temp;

		/* 递归 */
		foreach(temp, ((BoolExpr *) qual)->args)
		{
			Expr	   *arg = (Expr *) lfirst(temp);

			arg = find_duplicate_ors(arg, is_check);

			/* 去掉任何常量输入 */
			if (arg && IsA(arg, Const))
			{
				Const	   *carg = (Const *) arg;

				if (is_check)
				{
					/* 在 CHECK 里的 OR 中，丢掉常量 FALSE */
					if (!carg->constisnull && !DatumGetBool(carg->constvalue))
						continue;
					/* 常量 TRUE 或 NULL，因此 OR 收成 TRUE */
					return (Expr *) makeBoolConst(true, false);
				}
				else
				{
					/* 在 WHERE 里的 OR 中，丢掉常量 FALSE 或 NULL */
					if (carg->constisnull || !DatumGetBool(carg->constvalue))
						continue;
					/* 常量 TRUE，因此 OR 收成 TRUE */
					return arg;
				}
			}

			orlist = lappend(orlist, arg);
		}

		/* 展平任何被上拉到紧挨此处之下的 OR */
		orlist = pull_ors(orlist);

		/* 现在可以寻找重复的 OR */
		return process_duplicate_ors(orlist);
	}
	else if (is_andclause(qual))
	{
		List	   *andlist = NIL;
		ListCell   *temp;

		/* 递归 */
		foreach(temp, ((BoolExpr *) qual)->args)
		{
			Expr	   *arg = (Expr *) lfirst(temp);

			arg = find_duplicate_ors(arg, is_check);

			/* 去掉任何常量输入 */
			if (arg && IsA(arg, Const))
			{
				Const	   *carg = (Const *) arg;

				if (is_check)
				{
					/* 在 CHECK 里的 AND 中，丢掉常量 TRUE 或 NULL */
					if (carg->constisnull || DatumGetBool(carg->constvalue))
						continue;
					/* 常量 FALSE，因此 AND 收成 FALSE */
					return arg;
				}
				else
				{
					/* 在 WHERE 里的 AND 中，丢掉常量 TRUE */
					if (!carg->constisnull && DatumGetBool(carg->constvalue))
						continue;
					/* 常量 FALSE 或 NULL，因此 AND 收成 FALSE */
					return (Expr *) makeBoolConst(false, false);
				}
			}

			andlist = lappend(andlist, arg);
		}

		/* 展平任何刚在此处之下引入的 ANDs */
		andlist = pull_ands(andlist);

		/* 没有输入的 AND 收成 TRUE */
		if (andlist == NIL)
			return (Expr *) makeBoolConst(true, false);

		/* 单表达式的 AND 就收成那个表达式 */
		if (list_length(andlist) == 1)
			return (Expr *) linitial(andlist);

		/* 否则仍需要一个 AND 节点 */
		return make_andclause(andlist);
	}
	else
		return qual;
}

/*
 * process_duplicate_ors
 *	    给定一份被 ORed 在一起的表达式列表，试着应用
 *	    逆 OR 分配律。
 *
 * 返回结果表达式（可能是 AND 子句、OR
 * 子句，甚至也许是单个子表达式）。
 */
static Expr *
process_duplicate_ors(List *orlist)
{
	List	   *reference = NIL;
	int			num_subclauses = 0;
	List	   *winners;
	List	   *neworlist;
	ListCell   *temp;

	/* 没有输入的 OR 收成 FALSE */
	if (orlist == NIL)
		return (Expr *) makeBoolConst(false, false);

	/* 单表达式的 OR 就收成那个表达式 */
	if (list_length(orlist) == 1)
		return (Expr *) linitial(orlist);

	/*
	 * 选最短的 AND 子句作为参考列表——显然，任何
	 * 不在这个子句里的子子句就不会在全部分句里。若找到一个
	 * 不是 AND 的子句，可以把它当作单元素 AND 子句，
	 * 它必然作为最短者胜出。
	 */
	foreach(temp, orlist)
	{
		Expr	   *clause = (Expr *) lfirst(temp);

		if (is_andclause(clause))
		{
			List	   *subclauses = ((BoolExpr *) clause)->args;
			int			nclauses = list_length(subclauses);

			if (reference == NIL || nclauses < num_subclauses)
			{
				reference = subclauses;
				num_subclauses = nclauses;
			}
		}
		else
		{
			reference = list_make1(clause);
			break;
		}
	}

	/*
	 * 以防万一，去掉参考列表里的任何重复。
	 */
	reference = list_union(NIL, reference);

	/*
	 * 检查参考列表的每个元素，看它是否在全部 OR
	 * 子句里。建立一份获胜子句的新列表。
	 */
	winners = NIL;
	foreach(temp, reference)
	{
		Expr	   *refclause = (Expr *) lfirst(temp);
		bool		win = true;
		ListCell   *temp2;

		foreach(temp2, orlist)
		{
			Expr	   *clause = (Expr *) lfirst(temp2);

			if (is_andclause(clause))
			{
				if (!list_member(((BoolExpr *) clause)->args, refclause))
				{
					win = false;
					break;
				}
			}
			else
			{
				if (!equal(refclause, clause))
				{
					win = false;
					break;
				}
			}
		}

		if (win)
			winners = lappend(winners, refclause);
	}

	/*
	 * 若没有获胜者，就不能变换这个 OR
	 */
	if (winners == NIL)
		return make_orclause(orlist);

	/*
	 * 生成由剩余子子句组成的新 OR 列表。
	 *
	 * 若任何子句退化成空，则我们有 (A
	 * AND B) OR (A) 这样的情形，可以收成只剩 A——也就是说，
	 * OR 其他分支里的附加条件无关紧要。
	 *
	 * 注意因为我们用 list_difference，AND 子子句里获胜子句的任何多次出现
	 * 都会被自动去掉。
	 */
	neworlist = NIL;
	foreach(temp, orlist)
	{
		Expr	   *clause = (Expr *) lfirst(temp);

		if (is_andclause(clause))
		{
			List	   *subclauses = ((BoolExpr *) clause)->args;

			subclauses = list_difference(subclauses, winners);
			if (subclauses != NIL)
			{
				if (list_length(subclauses) == 1)
					neworlist = lappend(neworlist, linitial(subclauses));
				else
					neworlist = lappend(neworlist, make_andclause(subclauses));
			}
			else
			{
				neworlist = NIL;	/* 退化情形，见上 */
				break;
			}
		}
		else
		{
			if (!list_member(winners, clause))
				neworlist = lappend(neworlist, clause);
			else
			{
				neworlist = NIL;	/* 退化情形，见上 */
				break;
			}
		}
	}

	/*
	 * 若归约后的 OR 没有退化，就把它追加到获胜者列表，并正确处理
	 * 单元素的特殊情形（那真的会发生吗？）。
	 * 若上拉了一个
	 * 子子 OR 子句，也要小心保持 AND/OR 平坦。
	 */
	if (neworlist != NIL)
	{
		if (list_length(neworlist) == 1)
			winners = lappend(winners, linitial(neworlist));
		else
			winners = lappend(winners, make_orclause(pull_ors(neworlist)));
	}

	/*
	 * 并返回构造出的 AND 子句，同样要当心单个
	 * 元素和 AND/OR 平坦性。
	 */
	if (list_length(winners) == 1)
		return (Expr *) linitial(winners);
	else
		return make_andclause(pull_ands(winners));
}
