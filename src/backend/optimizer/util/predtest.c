/*-------------------------------------------------------------------------
 *
 * predtest.c
 *	  Routines to attempt to prove logical implications between predicate
 *	  expressions.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/predtest.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 * 对 ScalarArrayOpExpr 节点里的大数组做证明，
 * 多半需要 O(N^2) 时间，而且往往还是失败。
 * 因此我们任意设了一个上限，限制
 * 允许当作 AND 或 OR 子句处理的数组元素个数。
 * XXX 值不值得把它做成 GUC 开关？
 */
#define MAX_SAOP_ARRAY_SIZE		100

/*
 * 为避免在 predicate_implied_by_recurse 和
 * predicate_refuted_by_recurse 里写重复代码，需要把
 * 遍历逻辑上是 AND
 * 或 OR 结构的表达式分量这一概念抽出来。有多种表达式节点可以
 * 当作 ANDs 或 OR 处理，我们不想为每一种单独写代码。
 * 因此有了这些类型和辅助例程。
 */
typedef enum
{
	CLASS_ATOM,					/* 既不是 AND 也不是 OR 的表达式 */
	CLASS_AND,					/* 具有 AND 语义的表达式 */
	CLASS_OR					/* 具有 OR 语义的表达式 */
} PredClass;

typedef struct PredIterInfoData *PredIterInfo;

typedef struct PredIterInfoData
{
	/* 随节点类型而异的迭代状态 */
	void	   *state;
	List	   *state_list;
	/* 初始化以便开始迭代 */
	void		(*startup_fn) (Node *clause, PredIterInfo info);
	/* 取下一个分量的迭代函数 */
	Node	   *(*next_fn) (PredIterInfo info);
	/* 迭代结束后释放资源 */
	void		(*cleanup_fn) (PredIterInfo info);
} PredIterInfoData;

#define iterate_begin(item, clause, info)	\
	do { \
		Node   *item; \
		(info).startup_fn((clause), &(info)); \
		while ((item = (info).next_fn(&(info))) != NULL)

#define iterate_end(info)	\
		(info).cleanup_fn(&(info)); \
	} while (0)


static bool predicate_implied_by_recurse(Node *clause, Node *predicate,
										 bool weak);
static bool predicate_refuted_by_recurse(Node *clause, Node *predicate,
										 bool weak);
static PredClass predicate_classify(Node *clause, PredIterInfo info);
static void list_startup_fn(Node *clause, PredIterInfo info);
static Node *list_next_fn(PredIterInfo info);
static void list_cleanup_fn(PredIterInfo info);
static void boolexpr_startup_fn(Node *clause, PredIterInfo info);
static void arrayconst_startup_fn(Node *clause, PredIterInfo info);
static Node *arrayconst_next_fn(PredIterInfo info);
static void arrayconst_cleanup_fn(PredIterInfo info);
static void arrayexpr_startup_fn(Node *clause, PredIterInfo info);
static Node *arrayexpr_next_fn(PredIterInfo info);
static void arrayexpr_cleanup_fn(PredIterInfo info);
static bool predicate_implied_by_simple_clause(Expr *predicate, Node *clause,
											   bool weak);
static bool predicate_refuted_by_simple_clause(Expr *predicate, Node *clause,
											   bool weak);
static Node *extract_not_arg(Node *clause);
static Node *extract_strong_not_arg(Node *clause);
static bool clause_is_strict_for(Node *clause, Node *subexpr, bool allow_false);
static bool operator_predicate_proof(Expr *predicate, Node *clause,
									 bool refute_it, bool weak);
static bool operator_same_subexprs_proof(Oid pred_op, Oid clause_op,
										 bool refute_it);
static bool operator_same_subexprs_lookup(Oid pred_op, Oid clause_op,
										  bool refute_it);
static Oid	get_btree_test_op(Oid pred_op, Oid clause_op, bool refute_it);
static void InvalidateOprProofCacheCallBack(Datum arg, int cacheid, uint32 hashvalue);


/*
 * predicate_implied_by
 *	  递归检查 clause_list 中的子句是否蕴含
 *	  给定谓词为真。
 *
 * 我们支持两种蕴含定义：
 *
 * 强蕴含：A 蕴含 B 表示 A 为真则 B 为真。
 * 我们用它证明：满足一条 WHERE 子句或索引谓词的行，
 * 必定满足另一条。
 *
 * 弱蕴含：A 蕴含 B 表示 A 非假则 B 非假。
 * 非假表示真或 NULL。我们用它证明：满足一条
 * CHECK 约束的行必定满足另一条。
 *
 * 强蕴含也可以用来证明 WHERE 子句蕴含一条 CHECK 约束，
 * 不过有少数我们本可安全断定蕴含成立的情形，它证不出来。
 * 不支持证明反过来的情形，
 * 因为只有少数几种 CHECK 约束
 * 能据以推出什么。
 *
 * 每个列表的顶层 List 结构对应一个 AND 列表。
 * 我们假定已经用过 eval_const_expressions()，因此没有未展平的
 * ANDs 或 OR（例如 AND 里紧挨着另一个 AND，
 * 包括顶层 List 结构正下方的 AND）。
 * 若并非如此，我们可能证不出一条本应成立的蕴含，
 * 但不会有更糟的后果。
 *
 * 我们假定谓词已经检查过，只含
 * 不可变函数和操作符。（许多当前用法里这是已知的，
 * 因为谓词是通过了 CheckPredicate() 的索引谓词的一部分；
 * 否则调用者必须自己检查。）我们不敢根据
 * 非不可变函数做推导，因为从做出计划到执行计划之间，
 * 它们的结果可能改变。
 * 必要时在这里检查 clause_list 中函数的不可变性。
 */
bool
predicate_implied_by(List *predicate_list, List *clause_list,
					 bool weak)
{
	Node	   *p,
			   *c;

	if (predicate_list == NIL)
		return true;			/* 没有谓词：蕴含是空泛的 */
	if (clause_list == NIL)
		return false;			/* 没有限制：蕴含必定失败 */

	/*
	 * 若任一输入是单元素列表，就换成它那唯一的
	 * 成员；这样省掉一层无用的 AND 递归。只需要
	 * 在顶层操心这一点，因为 eval_const_expressions 应当
	 * 已经去掉了更下层的平凡 ANDs 或 OR。
	 */
	if (list_length(predicate_list) == 1)
		p = (Node *) linitial(predicate_list);
	else
		p = (Node *) predicate_list;
	if (list_length(clause_list) == 1)
		c = (Node *) linitial(clause_list);
	else
		c = (Node *) clause_list;

	/* 然后就开始…… */
	return predicate_implied_by_recurse(c, p, weak);
}

/*
 * predicate_refuted_by
 *	  递归检查 clause_list 中的子句是否反驳给定
 *	  谓词（也就是证明它为假）。
 *
 * 这并不等于对 predicate_implied_by 取反，尽管代码的
 * 手法和结构与之相似。
 *
 * 我们支持两种反驳定义：
 *
 * 强反驳：A 反驳 B 表示 A 为真蕴含 B 为假。
 * 我们用它在给定 WHERE 子句时否定一条 CHECK 约束，也就是
 * 证明任何满足 WHERE 子句的行都会违反该 CHECK
 * 约束。（注意必须证明 B 得出假，而不只是非真。）
 *
 * 弱反驳：A 反驳 B 表示 A 为真蕴含 B 非真
 * （即 B 必须得出假或 NULL）。我们用它检测互相
 * 矛盾的 WHERE 子句。
 *
 * 有些强反驳不成立的情形仍能证明弱反驳，
 * 因此能用时就用它。目前不支持
 * 根据一条 CHECK 约束否定另一条，也不支持
 * 根据 CHECK 否定 WHERE。（和蕴含一样，后一种
 * 似乎不太实用。CHECK 对 CHECK 也许有用，但目前
 * 哪里都不需要。）
 *
 * 每个列表的顶层 List 结构对应一个 AND 列表。
 * 我们假定已经用过 eval_const_expressions()，因此
 * 没有未展平的 ANDs 或 OR（例如 AND 里紧挨着另一个 AND，
 * 包括顶层 List 结构正下方的 AND）。
 * 若并非如此，我们可能证不出一条本应
 * 成立的蕴含，但不会有更糟的后果。
 *
 * 我们假定谓词已经检查过，只含
 * 不可变函数和操作符。我们不敢根据
 * 非不可变函数做推导，因为从做出
 * 计划到执行计划之间，它们的结果可能改变。
 * 必要时在这里检查 clause_list 中函数的不可变性。
 */
bool
predicate_refuted_by(List *predicate_list, List *clause_list,
					 bool weak)
{
	Node	   *p,
			   *c;

	if (predicate_list == NIL)
		return false;			/* 没有谓词：不可能反驳 */
	if (clause_list == NIL)
		return false;			/* 没有限制：反驳必定失败 */

	/*
	 * 若任一输入是单元素列表，就换成它那唯一的
	 * 成员；这样省掉一层无用的 AND 递归。只需要
	 * 在顶层操心这一点，因为 eval_const_expressions 应当
	 * 已经去掉了更下层的平凡 ANDs 或 OR。
	 */
	if (list_length(predicate_list) == 1)
		p = (Node *) linitial(predicate_list);
	else
		p = (Node *) predicate_list;
	if (list_length(clause_list) == 1)
		c = (Node *) linitial(clause_list);
	else
		c = (Node *) clause_list;

	/* 然后就开始…… */
	return predicate_refuted_by_recurse(c, p, weak);
}

/*----------
 * predicate_implied_by_recurse
 *	  Does the predicate implication test for non-NULL restriction and
 *	  predicate clauses.
 *
 * The logic followed here is ("=>" means "implies"):
 *	atom A => atom B iff:			predicate_implied_by_simple_clause says so
 *	atom A => AND-expr B iff:		A => each of B's components
 *	atom A => OR-expr B iff:		A => any of B's components
 *	AND-expr A => atom B iff:		any of A's components => B
 *	AND-expr A => AND-expr B iff:	A => each of B's components
 *	AND-expr A => OR-expr B iff:	A => any of B's components,
 *									*or* any of A's components => B
 *	OR-expr A => atom B iff:		each of A's components => B
 *	OR-expr A => AND-expr B iff:	A => each of B's components
 *	OR-expr A => OR-expr B iff:		each of A's components => any of B's
 *
 * An "atom" is anything other than an AND or OR node.  Notice that we don't
 * have any special logic to handle NOT nodes; these should have been pushed
 * down or eliminated where feasible during eval_const_expressions().
 *
 * All of these rules apply equally to strong or weak implication.
 *
 * We can't recursively expand either side first, but have to interleave
 * the expansions per the above rules, to be sure we handle all of these
 * examples:
 *		(x OR y) => (x OR y OR z)
 *		(x AND y AND z) => (x AND y)
 *		(x AND y) => ((x AND y) OR z)
 *		((x OR y) AND z) => (x OR y)
 * This is still not an exhaustive test, but it handles most normal cases
 * under the assumption that both inputs have been AND/OR flattened.
 *
 * We have to be prepared to handle RestrictInfo nodes in the restrictinfo
 * tree, though not in the predicate tree.
 *----------
 */
static bool
predicate_implied_by_recurse(Node *clause, Node *predicate,
							 bool weak)
{
	PredIterInfoData clause_info;
	PredIterInfoData pred_info;
	PredClass	pclass;
	bool		result;

	/* 跳过 RestrictInfo */
	Assert(clause != NULL);
	if (IsA(clause, RestrictInfo))
		clause = (Node *) ((RestrictInfo *) clause)->clause;

	pclass = predicate_classify(predicate, &pred_info);

	switch (predicate_classify(clause, &clause_info))
	{
		case CLASS_AND:
			switch (pclass)
			{
				case CLASS_AND:

					/*
					 * AND 子句 => AND 子句，若 A 蕴含 B 的每一项
					 */
					result = true;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (!predicate_implied_by_recurse(clause, pitem,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_OR:

					/*
					 * AND 子句 => OR 子句，若 A 蕴含 B 的任一项。
					 *
					 * 为处理 (x AND y) => ((x AND y) OR z) 所需要。
					 */
					result = false;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (predicate_implied_by_recurse(clause, pitem,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(pred_info);
					if (result)
						return result;

					/*
					 * 也检查 A 的任一项是否蕴含 B。
					 *
					 * 为处理 ((x OR y) AND z) => (x OR y) 所需要。
					 */
					iterate_begin(citem, clause, clause_info)
					{
						if (predicate_implied_by_recurse(citem, predicate,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(clause_info);
					return result;

				case CLASS_ATOM:

					/*
					 * AND 子句 => 原子，若 A 的任一项蕴含 B
					 */
					result = false;
					iterate_begin(citem, clause, clause_info)
					{
						if (predicate_implied_by_recurse(citem, predicate,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(clause_info);
					return result;
			}
			break;

		case CLASS_OR:
			switch (pclass)
			{
				case CLASS_OR:

					/*
					 * OR 子句 => OR 子句，若 A 的每一项蕴含 B 的
					 * 任一项。很乱，但没法更简单。
					 */
					result = true;
					iterate_begin(citem, clause, clause_info)
					{
						bool		presult = false;

						iterate_begin(pitem, predicate, pred_info)
						{
							if (predicate_implied_by_recurse(citem, pitem,
															 weak))
							{
								presult = true;
								break;
							}
						}
						iterate_end(pred_info);
						if (!presult)
						{
							result = false; /* 不蕴含 B 的任何一项 */
							break;
						}
					}
					iterate_end(clause_info);
					return result;

				case CLASS_AND:
				case CLASS_ATOM:

					/*
					 * OR 子句 => AND 子句，若 A 的每一项都蕴含 B。
					 *
					 * OR 子句 => 原子，若 A 的每一项都蕴含 B。
					 */
					result = true;
					iterate_begin(citem, clause, clause_info)
					{
						if (!predicate_implied_by_recurse(citem, predicate,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(clause_info);
					return result;
			}
			break;

		case CLASS_ATOM:
			switch (pclass)
			{
				case CLASS_AND:

					/*
					 * 原子 => AND 子句，若 A 蕴含 B 的每一项
					 */
					result = true;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (!predicate_implied_by_recurse(clause, pitem,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_OR:

					/*
					 * 原子 => OR 子句，若 A 蕴含 B 的任一项
					 */
					result = false;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (predicate_implied_by_recurse(clause, pitem,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_ATOM:

					/*
					 * 原子 => 原子是基本情况
					 */
					return
						predicate_implied_by_simple_clause((Expr *) predicate,
														   clause,
														   weak);
			}
			break;
	}

	/* 到不了这里 */
	elog(ERROR, "predicate_classify returned a bogus value");
	return false;
}

/*----------
 * predicate_refuted_by_recurse
 *	  Does the predicate refutation test for non-NULL restriction and
 *	  predicate clauses.
 *
 * The logic followed here is ("R=>" means "refutes"):
 *	atom A R=> atom B iff:			predicate_refuted_by_simple_clause says so
 *	atom A R=> AND-expr B iff:		A R=> any of B's components
 *	atom A R=> OR-expr B iff:		A R=> each of B's components
 *	AND-expr A R=> atom B iff:		any of A's components R=> B
 *	AND-expr A R=> AND-expr B iff:	A R=> any of B's components,
 *									*or* any of A's components R=> B
 *	AND-expr A R=> OR-expr B iff:	A R=> each of B's components
 *	OR-expr A R=> atom B iff:		each of A's components R=> B
 *	OR-expr A R=> AND-expr B iff:	each of A's components R=> any of B's
 *	OR-expr A R=> OR-expr B iff:	A R=> each of B's components
 *
 * All of the above rules apply equally to strong or weak refutation.
 *
 * In addition, if the predicate is a NOT-clause then we can use
 *	A R=> NOT B if:					A => B
 * This works for several different SQL constructs that assert the non-truth
 * of their argument, ie NOT, IS FALSE, IS NOT TRUE, IS UNKNOWN, although some
 * of them require that we prove strong implication.  Likewise, we can use
 *	NOT A R=> B if:					B => A
 * but here we must be careful about strong vs. weak refutation and make
 * the appropriate type of implication proof (weak or strong respectively).
 *
 * Other comments are as for predicate_implied_by_recurse().
 *----------
 */
static bool
predicate_refuted_by_recurse(Node *clause, Node *predicate,
							 bool weak)
{
	PredIterInfoData clause_info;
	PredIterInfoData pred_info;
	PredClass	pclass;
	Node	   *not_arg;
	bool		result;

	/* 跳过 RestrictInfo */
	Assert(clause != NULL);
	if (IsA(clause, RestrictInfo))
		clause = (Node *) ((RestrictInfo *) clause)->clause;

	pclass = predicate_classify(predicate, &pred_info);

	switch (predicate_classify(clause, &clause_info))
	{
		case CLASS_AND:
			switch (pclass)
			{
				case CLASS_AND:

					/*
					 * AND 子句 R=> AND 子句，若 A 反驳 B 的任一项。
					 *
					 * 为处理 (x AND y) R=> ((!x OR !y) AND z) 所需要。
					 */
					result = false;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (predicate_refuted_by_recurse(clause, pitem,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(pred_info);
					if (result)
						return result;

					/*
					 * 也检查 A 的任一项是否反驳 B。
					 *
					 * 为处理 ((x OR y) AND z) R=> (!x AND !y) 所需要。
					 */
					iterate_begin(citem, clause, clause_info)
					{
						if (predicate_refuted_by_recurse(citem, predicate,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(clause_info);
					return result;

				case CLASS_OR:

					/*
					 * AND 子句 R=> OR 子句，若 A 反驳 B 的每一项
					 */
					result = true;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (!predicate_refuted_by_recurse(clause, pitem,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_ATOM:

					/*
					 * 若 B 是 NOT 型子句，则当 A => B 的参数时 A R=> B。
					 *
					 * 因为无论哪种反驳，出发点都是 A 为真，
					 * 所以任何情况下都可以用强蕴含检验。
					 * 那会证明 B 的参数为真。对弱反驳来说，
					 * 若 B 只是简单的 NOT，这比所需更强，
					 * 但它让我们不必操心
					 * 到底是哪一种否定子句。
					 */
					not_arg = extract_not_arg(predicate);
					if (not_arg &&
						predicate_implied_by_recurse(clause, not_arg,
													 false))
						return true;

					/*
					 * AND 子句 R=> 原子，若 A 的任一项反驳 B
					 */
					result = false;
					iterate_begin(citem, clause, clause_info)
					{
						if (predicate_refuted_by_recurse(citem, predicate,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(clause_info);
					return result;
			}
			break;

		case CLASS_OR:
			switch (pclass)
			{
				case CLASS_OR:

					/*
					 * OR 子句 R=> OR 子句，若 A 反驳 B 的每一项
					 */
					result = true;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (!predicate_refuted_by_recurse(clause, pitem,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_AND:

					/*
					 * OR 子句 R=> AND 子句，若 A 的每一项反驳
					 * B 的任一项。
					 */
					result = true;
					iterate_begin(citem, clause, clause_info)
					{
						bool		presult = false;

						iterate_begin(pitem, predicate, pred_info)
						{
							if (predicate_refuted_by_recurse(citem, pitem,
															 weak))
							{
								presult = true;
								break;
							}
						}
						iterate_end(pred_info);
						if (!presult)
						{
							result = false; /* citem 什么都不反驳 */
							break;
						}
					}
					iterate_end(clause_info);
					return result;

				case CLASS_ATOM:

					/*
					 * 若 B 是 NOT 型子句，则当 A => B 的参数时 A R=> B。
					 *
					 * 逻辑与上面 AND 子句的情形相同。
					 */
					not_arg = extract_not_arg(predicate);
					if (not_arg &&
						predicate_implied_by_recurse(clause, not_arg,
													 false))
						return true;

					/*
					 * OR 子句 R=> 原子，若 A 的每一项反驳 B
					 */
					result = true;
					iterate_begin(citem, clause, clause_info)
					{
						if (!predicate_refuted_by_recurse(citem, predicate,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(clause_info);
					return result;
			}
			break;

		case CLASS_ATOM:

			/*
			 * 若 A 是强 NOT 子句，则当 B 蕴含 A 的参数时 A R=> B。
			 *
			 * 因为 A 是强的，可以假定 A 的参数为假，而不只是非真。
			 * 若 B 弱蕴含 A 的参数，则 B 既无法为真也无法为 null，
			 * 从而证明了强反驳。
			 * 若 B 强蕴含 A 的参数，则 B 无法为真，
			 * 从而证明了弱反驳。
			 */
			not_arg = extract_strong_not_arg(clause);
			if (not_arg &&
				predicate_implied_by_recurse(predicate, not_arg,
											 !weak))
				return true;

			switch (pclass)
			{
				case CLASS_AND:

					/*
					 * 原子 R=> AND 子句，若 A 反驳 B 的任一项
					 */
					result = false;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (predicate_refuted_by_recurse(clause, pitem,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_OR:

					/*
					 * 原子 R=> OR 子句，若 A 反驳 B 的每一项
					 */
					result = true;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (!predicate_refuted_by_recurse(clause, pitem,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_ATOM:

					/*
					 * 若 B 是 NOT 型子句，则当 A => B 的参数时 A R=> B。
					 *
					 * 逻辑与上面 AND 子句的情形相同。
					 */
					not_arg = extract_not_arg(predicate);
					if (not_arg &&
						predicate_implied_by_recurse(clause, not_arg,
													 false))
						return true;

					/*
					 * 原子 R=> 原子是基本情况
					 */
					return
						predicate_refuted_by_simple_clause((Expr *) predicate,
														   clause,
														   weak);
			}
			break;
	}

	/* 到不了这里 */
	elog(ERROR, "predicate_classify returned a bogus value");
	return false;
}


/*
 * predicate_classify
 *	  把表达式节点分成 AND 型、OR 型，或两者都不是（原子）。
 *
 * 若分成 AND 型或 OR 型，则 *info 里填上
 * 遍历其分量所需的函数。
 *
 * 本函数也强制执行 MAX_SAOP_ARRAY_SIZE：若
 * ScalarArrayOpExpr 的数组元素过多，就只把它分成
 * 原子。（结果是它会原样传给 simple_clause
 * 函数，其中许多证不出任何东西。）注意我们
 * 无法只考虑到 MAX_SAOP_ARRAY_SIZE 个元素就停下；一般说来
 * 那样会得出错误的证明，而不是证不出任何东西。
 */
static PredClass
predicate_classify(Node *clause, PredIterInfo info)
{
	/* 调用者不应当传 NULL，也不应当传 RestrictInfo 子句 */
	Assert(clause != NULL);
	Assert(!IsA(clause, RestrictInfo));

	/*
	 * 若看到 List，就假定它是隐式 AND 列表；对
	 * RestrictInfo 节点列表，这是正确语义。
	 */
	if (IsA(clause, List))
	{
		info->startup_fn = list_startup_fn;
		info->next_fn = list_next_fn;
		info->cleanup_fn = list_cleanup_fn;
		return CLASS_AND;
	}

	/* 处理普通的 AND 和 OR 布尔子句 */
	if (is_andclause(clause))
	{
		info->startup_fn = boolexpr_startup_fn;
		info->next_fn = list_next_fn;
		info->cleanup_fn = list_cleanup_fn;
		return CLASS_AND;
	}
	if (is_orclause(clause))
	{
		info->startup_fn = boolexpr_startup_fn;
		info->next_fn = list_next_fn;
		info->cleanup_fn = list_cleanup_fn;
		return CLASS_OR;
	}

	/* 处理 ScalarArrayOpExpr */
	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Node	   *arraynode = (Node *) lsecond(saop->args);

		/*
		 * 我们可以把它拆成 AND 或 OR 结构，但前提是知道
		 * 如何遍历数组元素的表达式。
		 * 若数组操作数是非空常量，或是
		 * 简单的 ArrayExpr，就可以这么做。
		 */
		if (arraynode && IsA(arraynode, Const) &&
			!((Const *) arraynode)->constisnull)
		{
			ArrayType  *arrayval;
			int			nelems;

			arrayval = DatumGetArrayTypeP(((Const *) arraynode)->constvalue);
			nelems = ArrayGetNItems(ARR_NDIM(arrayval), ARR_DIMS(arrayval));
			if (nelems <= MAX_SAOP_ARRAY_SIZE)
			{
				info->startup_fn = arrayconst_startup_fn;
				info->next_fn = arrayconst_next_fn;
				info->cleanup_fn = arrayconst_cleanup_fn;
				return saop->useOr ? CLASS_OR : CLASS_AND;
			}
		}
		else if (arraynode && IsA(arraynode, ArrayExpr) &&
				 !((ArrayExpr *) arraynode)->multidims &&
				 list_length(((ArrayExpr *) arraynode)->elements) <= MAX_SAOP_ARRAY_SIZE)
		{
			info->startup_fn = arrayexpr_startup_fn;
			info->next_fn = arrayexpr_next_fn;
			info->cleanup_fn = arrayexpr_cleanup_fn;
			return saop->useOr ? CLASS_OR : CLASS_AND;
		}
	}

	/* 以上都不是，因此是原子 */
	return CLASS_ATOM;
}

/*
 * 遍历普通 List 的 PredIterInfo 例程。迭代
 * 状态变量是下一个要访问的 ListCell。
 */
static void
list_startup_fn(Node *clause, PredIterInfo info)
{
	info->state_list = (List *) clause;
	info->state = (void *) list_head(info->state_list);
}

static Node *
list_next_fn(PredIterInfo info)
{
	ListCell   *l = (ListCell *) info->state;
	Node	   *n;

	if (l == NULL)
		return NULL;
	n = lfirst(l);
	info->state = (void *) lnext(info->state_list, l);
	return n;
}

static void
list_cleanup_fn(PredIterInfo info)
{
	/* 没有什么要清理的 */
}

/*
 * BoolExpr 需要自己的启动函数，但可以使用 list_next_fn 和
 * list_cleanup_fn。
 */
static void
boolexpr_startup_fn(Node *clause, PredIterInfo info)
{
	info->state_list = ((BoolExpr *) clause)->args;
	info->state = (void *) list_head(info->state_list);
}

/*
 * 遍历数组操作数为常量的 ScalarArrayOpExpr 的 PredIterInfo 例程。
 * 数组操作数是常量。
 */
typedef struct
{
	OpExpr		opexpr;
	Const		const_expr;
	int			next_elem;
	int			num_elems;
	Datum	   *elem_values;
	bool	   *elem_nulls;
} ArrayConstIterState;

static void
arrayconst_startup_fn(Node *clause, PredIterInfo info)
{
	ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
	ArrayConstIterState *state;
	Const	   *arrayconst;
	ArrayType  *arrayval;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;

	/* 建立工作状态结构 */
	state = (ArrayConstIterState *) palloc(sizeof(ArrayConstIterState));
	info->state = (void *) state;

	/* 拆开数组字面量 */
	arrayconst = (Const *) lsecond(saop->args);
	arrayval = DatumGetArrayTypeP(arrayconst->constvalue);
	get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
						 &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arrayval,
					  ARR_ELEMTYPE(arrayval),
					  elmlen, elmbyval, elmalign,
					  &state->elem_values, &state->elem_nulls,
					  &state->num_elems);

	/* 建一个哑 OpExpr，作为逐项返回的节点 */
	state->opexpr.xpr.type = T_OpExpr;
	state->opexpr.opno = saop->opno;
	state->opexpr.opfuncid = saop->opfuncid;
	state->opexpr.opresulttype = BOOLOID;
	state->opexpr.opretset = false;
	state->opexpr.opcollid = InvalidOid;
	state->opexpr.inputcollid = saop->inputcollid;
	state->opexpr.args = list_copy(saop->args);

	/* 建一个哑 Const 节点，用来存放每个元素的值 */
	state->const_expr.xpr.type = T_Const;
	state->const_expr.consttype = ARR_ELEMTYPE(arrayval);
	state->const_expr.consttypmod = -1;
	state->const_expr.constcollid = arrayconst->constcollid;
	state->const_expr.constlen = elmlen;
	state->const_expr.constbyval = elmbyval;
	lsecond(state->opexpr.args) = &state->const_expr;

	/* 初始化迭代状态 */
	state->next_elem = 0;
}

static Node *
arrayconst_next_fn(PredIterInfo info)
{
	ArrayConstIterState *state = (ArrayConstIterState *) info->state;

	if (state->next_elem >= state->num_elems)
		return NULL;
	state->const_expr.constvalue = state->elem_values[state->next_elem];
	state->const_expr.constisnull = state->elem_nulls[state->next_elem];
	state->next_elem++;
	return (Node *) &(state->opexpr);
}

static void
arrayconst_cleanup_fn(PredIterInfo info)
{
	ArrayConstIterState *state = (ArrayConstIterState *) info->state;

	pfree(state->elem_values);
	pfree(state->elem_nulls);
	list_free(state->opexpr.args);
	pfree(state);
}

/*
 * 遍历数组操作数为一维 ArrayExpr 的 ScalarArrayOpExpr 的 PredIterInfo 例程。
 * 数组操作数是一维 ArrayExpr。
 */
typedef struct
{
	OpExpr		opexpr;
	ListCell   *next;
} ArrayExprIterState;

static void
arrayexpr_startup_fn(Node *clause, PredIterInfo info)
{
	ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
	ArrayExprIterState *state;
	ArrayExpr  *arrayexpr;

	/* 建立工作状态结构 */
	state = (ArrayExprIterState *) palloc(sizeof(ArrayExprIterState));
	info->state = (void *) state;

	/* 建一个哑 OpExpr，作为逐项返回的节点 */
	state->opexpr.xpr.type = T_OpExpr;
	state->opexpr.opno = saop->opno;
	state->opexpr.opfuncid = saop->opfuncid;
	state->opexpr.opresulttype = BOOLOID;
	state->opexpr.opretset = false;
	state->opexpr.opcollid = InvalidOid;
	state->opexpr.inputcollid = saop->inputcollid;
	state->opexpr.args = list_copy(saop->args);

	/* 把迭代变量初始化为 ArrayExpr 的第一个成员 */
	arrayexpr = (ArrayExpr *) lsecond(saop->args);
	info->state_list = arrayexpr->elements;
	state->next = list_head(arrayexpr->elements);
}

static Node *
arrayexpr_next_fn(PredIterInfo info)
{
	ArrayExprIterState *state = (ArrayExprIterState *) info->state;

	if (state->next == NULL)
		return NULL;
	lsecond(state->opexpr.args) = lfirst(state->next);
	state->next = lnext(info->state_list, state->next);
	return (Node *) &(state->opexpr);
}

static void
arrayexpr_cleanup_fn(PredIterInfo info)
{
	ArrayExprIterState *state = (ArrayExprIterState *) info->state;

	list_free(state->opexpr.args);
	pfree(state);
}


/*----------
 * predicate_implied_by_simple_clause
 *	  Does the predicate implication test for a "simple clause" predicate
 *	  and a "simple clause" restriction.
 *
 * We return true if able to prove the implication, false if not.
 *
 * We have several strategies for determining whether one simple clause
 * implies another:
 *
 * A simple and general way is to see if they are equal(); this works for any
 * kind of expression, and for either implication definition.  (Actually,
 * there is an implied assumption that the functions in the expression are
 * immutable --- but this was checked for the predicate by the caller.)
 *
 * Another way that always works is that for boolean x, "x = TRUE" is
 * equivalent to "x", likewise "x = FALSE" is equivalent to "NOT x".
 * These can be worth checking because, while we preferentially simplify
 * boolean comparisons down to "x" and "NOT x", the other form has to be
 * dealt with anyway in the context of index conditions.
 *
 * If the predicate is of the form "foo IS NOT NULL", and we are considering
 * strong implication, we can conclude that the predicate is implied if the
 * clause is strict for "foo", i.e., it must yield false or NULL when "foo"
 * is NULL.  In that case truth of the clause ensures that "foo" isn't NULL.
 * (Again, this is a safe conclusion because "foo" must be immutable.)
 * This doesn't work for weak implication, though.
 *
 * Finally, if both clauses are binary operator expressions, we may be able
 * to prove something using the system's knowledge about operators; those
 * proof rules are encapsulated in operator_predicate_proof().
 *----------
 */
static bool
predicate_implied_by_simple_clause(Expr *predicate, Node *clause,
								   bool weak)
{
	/* 允许打断长时间的证明尝试 */
	CHECK_FOR_INTERRUPTS();

	/* 先试 equal() 检验 */
	if (equal((Node *) predicate, clause))
		return true;

	/* 接着看子句是否为与常量的布尔等值 */
	if (is_opclause(clause) &&
		((OpExpr *) clause)->opno == BooleanEqualOperator)
	{
		OpExpr	   *op = (OpExpr *) clause;
		Node	   *rightop;

		Assert(list_length(op->args) == 2);
		rightop = lsecond(op->args);
		/* 这里也许永远见不到空 Const，但最好还是检查 */
		if (rightop && IsA(rightop, Const) &&
			!((Const *) rightop)->constisnull)
		{
			Node	   *leftop = linitial(op->args);

			if (DatumGetBool(((Const *) rightop)->constvalue))
			{
				/* X = true 蕴含 X */
				if (equal(predicate, leftop))
					return true;
			}
			else
			{
				/* X = false 蕴含 NOT X */
				if (is_notclause(predicate) &&
					equal(get_notclausearg(predicate), leftop))
					return true;
			}
		}
	}

	/*
	 * 同样可以检查谓词是否为与常量的布尔等值；
	 * 但假定谓词已经过常量折叠，
	 * 目前没有已知的使用情形。
	 */

	/* 接着试 IS NOT NULL 情形 */
	if (!weak &&
		predicate && IsA(predicate, NullTest))
	{
		NullTest   *ntest = (NullTest *) predicate;

		/* 行 IS NOT NULL 并不按我们心里那种简单方式起作用 */
		if (ntest->nulltesttype == IS_NOT_NULL &&
			!ntest->argisrow)
		{
			/* 子句对 foo 严格，蕴含 foo IS NOT NULL */
			if (clause_is_strict_for(clause, (Node *) ntest->arg, true))
				return true;
		}
		return false;			/* 下面没法成功…… */
	}

	/* 否则试与操作符有关的知识 */
	return operator_predicate_proof(predicate, clause, false, weak);
}

/*----------
 * predicate_refuted_by_simple_clause
 *	  Does the predicate refutation test for a "simple clause" predicate
 *	  and a "simple clause" restriction.
 *
 * We return true if able to prove the refutation, false if not.
 *
 * Unlike the implication case, checking for equal() clauses isn't helpful.
 * But relation_excluded_by_constraints() checks for self-contradictions in a
 * list of clauses, so that we may get here with predicate and clause being
 * actually pointer-equal, and that is worth eliminating quickly.
 *
 * When the predicate is of the form "foo IS NULL", we can conclude that
 * the predicate is refuted if the clause is strict for "foo" (see notes for
 * implication case), or is "foo IS NOT NULL".  That works for either strong
 * or weak refutation.
 *
 * A clause "foo IS NULL" refutes a predicate "foo IS NOT NULL" in all cases.
 * If we are considering weak refutation, it also refutes a predicate that
 * is strict for "foo", since then the predicate must yield false or NULL
 * (and since "foo" appears in the predicate, it's known immutable).
 *
 * (The main motivation for covering these IS [NOT] NULL cases is to support
 * using IS NULL/IS NOT NULL as partition-defining constraints.)
 *
 * Finally, if both clauses are binary operator expressions, we may be able
 * to prove something using the system's knowledge about operators; those
 * proof rules are encapsulated in operator_predicate_proof().
 *----------
 */
static bool
predicate_refuted_by_simple_clause(Expr *predicate, Node *clause,
								   bool weak)
{
	/* 允许打断长时间的证明尝试 */
	CHECK_FOR_INTERRUPTS();

	/* 简单子句无法反驳它自己 */
	/* 值得检查，因为有 relation_excluded_by_constraints() */
	if ((Node *) predicate == clause)
		return false;

	/* 试谓词为 IS NULL 的情形 */
	if (predicate && IsA(predicate, NullTest) &&
		((NullTest *) predicate)->nulltesttype == IS_NULL)
	{
		Expr	   *isnullarg = ((NullTest *) predicate)->arg;

		/* 行 IS NULL 并不按我们心里那种简单方式起作用 */
		if (((NullTest *) predicate)->argisrow)
			return false;

		/* 子句对 foo 严格，反驳 foo IS NULL */
		if (clause_is_strict_for(clause, (Node *) isnullarg, true))
			return true;

		/* foo IS NOT NULL 反驳 foo IS NULL */
		if (clause && IsA(clause, NullTest) &&
			((NullTest *) clause)->nulltesttype == IS_NOT_NULL &&
			!((NullTest *) clause)->argisrow &&
			equal(((NullTest *) clause)->arg, isnullarg))
			return true;

		return false;			/* 下面没法成功…… */
	}

	/* 试子句为 IS NULL 的情形 */
	if (clause && IsA(clause, NullTest) &&
		((NullTest *) clause)->nulltesttype == IS_NULL)
	{
		Expr	   *isnullarg = ((NullTest *) clause)->arg;

		/* 行 IS NULL 并不按我们心里那种简单方式起作用 */
		if (((NullTest *) clause)->argisrow)
			return false;

		/* foo IS NULL 反驳 foo IS NOT NULL */
		if (predicate && IsA(predicate, NullTest) &&
			((NullTest *) predicate)->nulltesttype == IS_NOT_NULL &&
			!((NullTest *) predicate)->argisrow &&
			equal(((NullTest *) predicate)->arg, isnullarg))
			return true;

		/* foo IS NULL 弱反驳任何对 foo 严格的谓词 */
		if (weak &&
			clause_is_strict_for((Node *) predicate, (Node *) isnullarg, true))
			return true;

		return false;			/* 下面没法成功…… */
	}

	/* 否则试与操作符有关的知识 */
	return operator_predicate_proof(predicate, clause, true, weak);
}


/*
 * 若子句断言某个子子句非真，就返回该子子句；
 * 否则返回 NULL。
 */
static Node *
extract_not_arg(Node *clause)
{
	if (clause == NULL)
		return NULL;
	if (IsA(clause, BoolExpr))
	{
		BoolExpr   *bexpr = (BoolExpr *) clause;

		if (bexpr->boolop == NOT_EXPR)
			return (Node *) linitial(bexpr->args);
	}
	else if (IsA(clause, BooleanTest))
	{
		BooleanTest *btest = (BooleanTest *) clause;

		if (btest->booltesttype == IS_NOT_TRUE ||
			btest->booltesttype == IS_FALSE ||
			btest->booltesttype == IS_UNKNOWN)
			return (Node *) btest->arg;
	}
	return NULL;
}

/*
 * 若子句断言某个子子句为假，就返回该子子句；
 * 否则返回 NULL。
 */
static Node *
extract_strong_not_arg(Node *clause)
{
	if (clause == NULL)
		return NULL;
	if (IsA(clause, BoolExpr))
	{
		BoolExpr   *bexpr = (BoolExpr *) clause;

		if (bexpr->boolop == NOT_EXPR)
			return (Node *) linitial(bexpr->args);
	}
	else if (IsA(clause, BooleanTest))
	{
		BooleanTest *btest = (BooleanTest *) clause;

		if (btest->booltesttype == IS_FALSE)
			return (Node *) btest->arg;
	}
	return NULL;
}


/*
 * 能否证明：若假定 subexpr 得出 NULL，
 * 则 clause 返回 NULL（或 FALSE）？
 *
 * 规划器里多数地方的严格性，是保证
 * 表达式在输入为 NULL 时输出 NULL，这里大体上也是
 * 在找这个。不过在顶层，已知子句
 * 得出布尔值时，证明它在 subexpr 为 NULL 时无法返回 TRUE
 * 也许就够了。这种较弱性质可接受时，调用者应传 allow_false = true。
 * （本函数内部递归时，
 * 我们向下传 allow_false = false，因为需要证明
 * 子表达式确实为 null。）
 *
 * 我们假定调用者已检查过，至少有一个输入表达式
 * 是不可变的。这里所有证明规则都是把 subexpr 匹配到
 * clause 的某一部分，因此不必再单独检查就可以假定 subexpr
 * 不可变。
 *
 * 基本情况是 clause 与 subexpr equal()。
 *
 * 若 subexpr 作为子表达式出现在
 * clause 中某个会迫使整体结果为 null 的位置，也可以报告成功。
 */
static bool
clause_is_strict_for(Node *clause, Node *subexpr, bool allow_false)
{
	ListCell   *lc;

	/* 安全检查 */
	if (clause == NULL || subexpr == NULL)
		return false;

	/*
	 * 看穿任何 RelabelType 节点，以便能匹配例如
	 * varcharcol 与 lower(varcharcol::text)。（一般说来可以递归
	 * 穿过任何保持空值性的不可变操作。）这里不应当
	 * 见到叠在一起的 RelabelTypes。
	 */
	if (IsA(clause, RelabelType))
		clause = (Node *) ((RelabelType *) clause)->arg;
	if (IsA(subexpr, RelabelType))
		subexpr = (Node *) ((RelabelType *) subexpr)->arg;

	/* 基本情况 */
	if (equal(clause, subexpr))
		return true;

	/*
	 * 若操作符或函数严格，则只要任一输入被 subexpr
	 * 迫使为 NULL，就保证结果为 NULL。即使该操作符或
	 * 函数不可变也没关系，因为 NULL 输入上根本不会调用它。
	 */
	if (is_opclause(clause) &&
		op_strict(((OpExpr *) clause)->opno))
	{
		foreach(lc, ((OpExpr *) clause)->args)
		{
			if (clause_is_strict_for((Node *) lfirst(lc), subexpr, false))
				return true;
		}
		return false;
	}
	if (is_funcclause(clause) &&
		func_strict(((FuncExpr *) clause)->funcid))
	{
		foreach(lc, ((FuncExpr *) clause)->args)
		{
			if (clause_is_strict_for((Node *) lfirst(lc), subexpr, false))
				return true;
		}
		return false;
	}

	/*
	 * CoerceViaIO 是严格的（不论它调用的 I/O 函数是否严格）。
	 * 同样，ArrayCoerceExpr 对其数组参数严格（不论
	 * 逐元素表达式是什么），ConvertRowtypeExpr 在
	 * 行这一层严格，CoerceToDomain 也严格。值得
	 * 检查，主要是免得向用户解释为什么有些
	 * 类型强制已知严格，有些则不是。
	 */
	if (IsA(clause, CoerceViaIO))
		return clause_is_strict_for((Node *) ((CoerceViaIO *) clause)->arg,
									subexpr, false);
	if (IsA(clause, ArrayCoerceExpr))
		return clause_is_strict_for((Node *) ((ArrayCoerceExpr *) clause)->arg,
									subexpr, false);
	if (IsA(clause, ConvertRowtypeExpr))
		return clause_is_strict_for((Node *) ((ConvertRowtypeExpr *) clause)->arg,
									subexpr, false);
	if (IsA(clause, CoerceToDomain))
		return clause_is_strict_for((Node *) ((CoerceToDomain *) clause)->arg,
									subexpr, false);

	/*
	 * ScalarArrayOpExpr 是特例。注意只有在没能把它拆成
	 * AND 或 OR 树时，才会带着 ScalarArrayOpExpr 子句走到这里，
	 * 例如数组元素过多的时候。
	 */
	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Node	   *scalarnode = (Node *) linitial(saop->args);
		Node	   *arraynode = (Node *) lsecond(saop->args);

		/*
		 * 若能证明标量输入为 null，且操作符
		 * 严格，则 SAOP 结果必须为 null——除非数组
		 * 为空。空数组会得到假（ANY）或真
		 * （ALL）。因此若 allow_false = true，则 ANY 情形下证明
		 * 反正会成功；否则只有能
		 * 证明数组非空时，才能做出证明。
		 */
		if (clause_is_strict_for(scalarnode, subexpr, false) &&
			op_strict(saop->opno))
		{
			int			nelems = 0;

			if (allow_false && saop->useOr)
				return true;	/* 即使数组为空也能成功 */

			if (arraynode && IsA(arraynode, Const))
			{
				Const	   *arrayconst = (Const *) arraynode;
				ArrayType  *arrval;

				/*
				 * 若数组是常量 NULL，则可以成功，如下面
				 * 那种情形。
				 */
				if (arrayconst->constisnull)
					return true;

				/* 否则可以算出元素个数。 */
				arrval = DatumGetArrayTypeP(arrayconst->constvalue);
				nelems = ArrayGetNItems(ARR_NDIM(arrval), ARR_DIMS(arrval));
			}
			else if (arraynode && IsA(arraynode, ArrayExpr) &&
					 !((ArrayExpr *) arraynode)->multidims)
			{
				/*
				 * 若输入是非多维的 ARRAY[] 表达式，
				 * 也可以可靠地数出数组元素个数。
				 */
				nelems = list_length(((ArrayExpr *) arraynode)->elements);
			}

			/* 若数组确定非空，则证明成功 */
			if (nelems > 0)
				return true;
		}

		/*
		 * 若能证明数组输入为 null，则任何情况下证明都成功，
		 * 因为对 NULL 数组，ScalarArrayOpExpr 总是返回 NULL。
		 * 否则这里就结束了。
		 */
		return clause_is_strict_for(arraynode, subexpr, false);
	}

	/*
	 * 递归进表达式时，可能发现 NULL 常量。
	 * 那当然是 NULL，不论它是否匹配 subexpr。
	 */
	if (IsA(clause, Const))
		return ((Const *) clause)->constisnull;

	return false;
}


/*
 * 为 btree 操作符（策略）定义操作符蕴含表，
 * 以及类似的反驳表。
 *
 * btree 索引定义的策略号（见 access/stratnum.h）如下：
 *		1 <		2 <=	3 =		4 >=	5 >
 * 另外用 6 表示 <>。<> 不是可建 btree 索引的操作符，
 * 但这里假定：若某 btree 操作符族的等值操作符有否定操作符，
 * 该否定操作符对该操作符族的行为如同 <>。
 * （get_op_btree_interpretation() 也采用这一约定。）
 *
 * 当两个子表达式相同时，使用 BT_implies_table[] 与 BT_refutes_table[]，
 * 用来判断一个操作符表达式是否蕴含或反驳另一个。
 * 若子句是 EXPR1 clause_op EXPR2，
 * 谓词是同一对不可变子表达式上的 EXPR1 pred_op EXPR2，则：
 * BT_implies_table[clause_op-1][pred_op-1]
 *		在子句蕴含谓词时为真。
 *			BT_refutes_table[clause_op-1][pred_op-1]
 *		在子句反驳谓词时为真。
 *			其中 clause_op 与 pred_op 是同一 btree 操作符族里
 * 从 1 到 6 的策略号。
 * 例如 x < y 蕴含 x <= y，
 * 并反驳 x > y。
 *
 * 有两个需要比较的常量时，使用 BT_implic_table[] 和 BT_refute_table[]。
 * 其含义是：
 *
 *		test_op = BT_implic_table[clause_op-1][pred_op-1]
 *
 * 其中 test_op、clause_op 和 pred_op 是从 1 到 6 的策略号，
 * 含义如下：
 *
 *	 若对某个 EXPR 已知 EXPR clause_op CONST1 为真，
 *	 并且想判断 EXPR pred_op CONST2 是否也必定为真，
 *	 可以用 CONST2 test_op CONST1 做检验。若检验返回真，
 *	 则谓词表达式必定为真；若检验返回假，
 *	 则谓词表达式可能为假。
 *
 * 例如，若子句是 Quantity > 10，谓词是 Quantity > 5，
 * 则检验 5 <= 10，求值为真，因此子句蕴含谓词。
 *
 * 类似地，BT_refute_table 一项的含义是：
 *
 *	 若对某个 EXPR 已知 EXPR clause_op CONST1 为真，
 *	 并且想判断 EXPR pred_op CONST2 是否必定为假，
 *	 可以用 CONST2 test_op CONST1 做检验。若检验返回真，
 *	 则谓词表达式必定为假；若检验返回假，
 *	 则谓词表达式可能为真。
 *
 * 例如，若子句是 Quantity > 10，谓词是 Quantity < 5，
 * 则检验 5 <= 10，求值为真，因此子句反驳谓词。
 *
 * test_op == 0 的项表示该蕴含无法判定。
 */

#define BTLT BTLessStrategyNumber
#define BTLE BTLessEqualStrategyNumber
#define BTEQ BTEqualStrategyNumber
#define BTGE BTGreaterEqualStrategyNumber
#define BTGT BTGreaterStrategyNumber
#define BTNE ROWCOMPARE_NE

/* 用 none 表示 0 或假，以便表格对齐 */
#define none 0

static const bool BT_implies_table[6][6] = {
/*
 *			谓词操作符：
 *	 LT（小于） LE（小于等于） EQ（等于） GE（大于等于） GT（大于） NE（不等于）
 */
	{true, true, none, none, none, true},	/* LT（小于） */
	{none, true, none, none, none, none},	/* LE（小于等于） */
	{none, true, true, true, none, none},	/* EQ（等于） */
	{none, none, none, true, none, none},	/* GE（大于等于） */
	{none, none, none, true, true, true},	/* GT（大于） */
	{none, none, none, none, none, true}	/* NE（不等于） */
};

static const bool BT_refutes_table[6][6] = {
/*
 *			谓词操作符：
 *	 LT（小于） LE（小于等于） EQ（等于） GE（大于等于） GT（大于） NE（不等于）
 */
	{none, none, true, true, true, none},	/* LT（小于） */
	{none, none, none, none, true, none},	/* LE（小于等于） */
	{true, none, none, none, true, true},	/* EQ（等于） */
	{true, none, none, none, none, none},	/* GE（大于等于） */
	{true, true, true, none, none, none},	/* GT（大于） */
	{none, none, true, none, none, none}	/* NE（不等于） */
};

static const StrategyNumber BT_implic_table[6][6] = {
/*
 *			谓词操作符：
 *	 LT（小于） LE（小于等于） EQ（等于） GE（大于等于） GT（大于） NE（不等于）
 */
	{BTGE, BTGE, none, none, none, BTGE},	/* LT（小于） */
	{BTGT, BTGE, none, none, none, BTGT},	/* LE（小于等于） */
	{BTGT, BTGE, BTEQ, BTLE, BTLT, BTNE},	/* EQ（等于） */
	{none, none, none, BTLE, BTLT, BTLT},	/* GE（大于等于） */
	{none, none, none, BTLE, BTLE, BTLE},	/* GT（大于） */
	{none, none, none, none, none, BTEQ}	/* NE（不等于） */
};

static const StrategyNumber BT_refute_table[6][6] = {
/*
 *			谓词操作符：
 *	 LT（小于） LE（小于等于） EQ（等于） GE（大于等于） GT（大于） NE（不等于）
 */
	{none, none, BTGE, BTGE, BTGE, none},	/* LT（小于） */
	{none, none, BTGT, BTGT, BTGE, none},	/* LE（小于等于） */
	{BTLE, BTLT, BTNE, BTGT, BTGE, BTEQ},	/* EQ（等于） */
	{BTLE, BTLT, BTLT, none, none, none},	/* GE（大于等于） */
	{BTLE, BTLE, BTLE, none, none, none},	/* GT（大于） */
	{none, none, BTEQ, none, none, none}	/* NE（不等于） */
};


/*
 * operator_predicate_proof
 *	  当两者都是使用相关操作符、且输入表达式相同的
 *	  操作符子句时，对简单子句谓词和简单子句限制
 *	  做蕴含或反驳检验。
 *
 * refute_it == false 时，要证明谓词为真；
 * refute_it == true 时，要证明谓词为假。
 * 两边共同代码很多，值得放在同一个例程里。
 * 若能做出证明则返回真，
 * 若证不出则返回假。
 *
 * 这里多半不必区分强蕴含/反驳与弱蕴含/反驳。
 * 这依赖于如下假定：一对相关操作符（交换子、否定子，
 * 或 btree 操作符族中的兄弟）对相同输入不会一个返回 NULL、
 * 另一个返回非 NULL。于是，对那些从假定子句为真出发的
 * 证明类型，谓词操作符也无法返回 NULL，
 * 因此做强证明还是弱证明并不重要。
 * 对弱蕴含，子句操作符可能返回了 NULL，
 * 但那时谓词操作符也会如此，弱蕴含仍然成立。
 * 考虑两个不同常量时这一论证不适用，
 * 因为操作符拿到的输入并不相同。
 * 但我们只对 btree 操作符支持那种情形，
 * 可以假定非空输入都得到非空输出，
 * 因此考虑哪两个非空常量都没关系。
 * 若任一常量为 NULL，就得想得更仔细，
 * 但有时证明仍然成立，如下所述。
 *
 * 可以对几种表达式形式做证明。这里 foo 和 bar
 * 表示按 equal() 来说相同的子表达式：
 *	若 op1 是 op2 的否定子，则 foo op1 bar 反驳 foo op2 bar。
 *	若 op1 是 op2 的交换子，则 foo op1 bar 蕴含 bar op2 foo。
 *	若 op1 是 op2 交换子的否定子，则 foo op1 bar 反驳 bar op2 foo。
 *	基于 btree 语义，foo op1 bar 可以蕴含或反驳 foo op2 bar。
 *	基于 btree 语义，foo op1 bar 可以蕴含或反驳 bar op2 foo。
 *	基于 btree 语义，foo op1 const1 可以蕴含或反驳 foo op2 const2。
 *
 * 最后三种情形里，op1 和 op2 必须属于同一个 btree 操作符族。
 * 当两个子表达式相同时，想法是：例如 x < y 蕴含 x <= y，
 * 与 x 和 y 究竟是什么无关。
 * 若两个不同常量与同一个表达式 foo 比较，
 * 就必须执行一次常量比较才能决定结果。
 * 例如，若 c1 <= c2，则 foo < c1 蕴含 foo < c2。
 * 若比较操作符不可变，
 * 我们假定在计划时比较这些常量是安全的。
 *
 * 注意：所有操作符和子表达式都必须不可变，证明才安全。
 * 我们假定谓词表达式整体不可变，
 * 因此这里不必显式检查子表达式，但有些情形
 * 需要额外检查操作符的不可变性。尤其是
 * btree 操作符族可以含有仅仅稳定的跨类型操作符，
 * 我们不敢用那些做推导。
 */
static bool
operator_predicate_proof(Expr *predicate, Node *clause,
						 bool refute_it, bool weak)
{
	OpExpr	   *pred_opexpr,
			   *clause_opexpr;
	Oid			pred_collation,
				clause_collation;
	Oid			pred_op,
				clause_op,
				test_op;
	Node	   *pred_leftop,
			   *pred_rightop,
			   *clause_leftop,
			   *clause_rightop;
	Const	   *pred_const,
			   *clause_const;
	Expr	   *test_expr;
	ExprState  *test_exprstate;
	Datum		test_result;
	bool		isNull;
	EState	   *estate;
	MemoryContext oldcontext;

	/*
	 * 两个表达式都必须是二元操作子句，否则我们也没法做什么。
	 *
	 * 注意：将来也许把这一逻辑扩展到其他基于操作符的
	 * 构造，例如 DistinctExpr。但规划器对
	 * DistinctExpr 总体上并不聪明，若想改进这一点，这里大概不是
	 * 最先该修的地方。
	 */
	if (!is_opclause(predicate))
		return false;
	pred_opexpr = (OpExpr *) predicate;
	if (list_length(pred_opexpr->args) != 2)
		return false;
	if (!is_opclause(clause))
		return false;
	clause_opexpr = (OpExpr *) clause;
	if (list_length(clause_opexpr->args) != 2)
		return false;

	/*
	 * 若它们标了不同的排序规则，我们也没法做什么。
	 * 这个检查很便宜，所以先做掉。
	 */
	pred_collation = pred_opexpr->inputcollid;
	clause_collation = clause_opexpr->inputcollid;
	if (pred_collation != clause_collation)
		return false;

	/* 现在也取出操作符 OIDs。下面也许会交换它们。 */
	pred_op = pred_opexpr->opno;
	clause_op = clause_opexpr->opno;

	/*
	 * 至少要配上一对输入表达式。
	 */
	pred_leftop = (Node *) linitial(pred_opexpr->args);
	pred_rightop = (Node *) lsecond(pred_opexpr->args);
	clause_leftop = (Node *) linitial(clause_opexpr->args);
	clause_rightop = (Node *) lsecond(clause_opexpr->args);

	if (equal(pred_leftop, clause_leftop))
	{
		if (equal(pred_rightop, clause_rightop))
		{
			/* 我们有 x op1 y 和 x op2 y */
			return operator_same_subexprs_proof(pred_op, clause_op, refute_it);
		}
		else
		{
			/* 除非两边的右操作数都是 Const，否则失败 */
			if (pred_rightop == NULL || !IsA(pred_rightop, Const))
				return false;
			pred_const = (Const *) pred_rightop;
			if (clause_rightop == NULL || !IsA(clause_rightop, Const))
				return false;
			clause_const = (Const *) clause_rightop;
		}
	}
	else if (equal(pred_rightop, clause_rightop))
	{
		/* 除非两边的左操作数都是 Const，否则失败 */
		if (pred_leftop == NULL || !IsA(pred_leftop, Const))
			return false;
		pred_const = (Const *) pred_leftop;
		if (clause_leftop == NULL || !IsA(clause_leftop, Const))
			return false;
		clause_const = (Const *) clause_leftop;
		/* 交换两个操作符，以便假定 Const 在右边 */
		pred_op = get_commutator(pred_op);
		if (!OidIsValid(pred_op))
			return false;
		clause_op = get_commutator(clause_op);
		if (!OidIsValid(clause_op))
			return false;
	}
	else if (equal(pred_leftop, clause_rightop))
	{
		if (equal(pred_rightop, clause_leftop))
		{
			/* 我们有 x op1 y 和 y op2 x */
			/* 交换 pred_op，以便把它当成直接匹配 */
			pred_op = get_commutator(pred_op);
			if (!OidIsValid(pred_op))
				return false;
			return operator_same_subexprs_proof(pred_op, clause_op, refute_it);
		}
		else
		{
			/* 除非 pred_rightop 和 clause_leftop 都是 Const，否则失败 */
			if (pred_rightop == NULL || !IsA(pred_rightop, Const))
				return false;
			pred_const = (Const *) pred_rightop;
			if (clause_leftop == NULL || !IsA(clause_leftop, Const))
				return false;
			clause_const = (Const *) clause_leftop;
			/* 交换 clause_op，以便假定 Const 在右边 */
			clause_op = get_commutator(clause_op);
			if (!OidIsValid(clause_op))
				return false;
		}
	}
	else if (equal(pred_rightop, clause_leftop))
	{
		/* 除非 pred_leftop 和 clause_rightop 都是 Const，否则失败 */
		if (pred_leftop == NULL || !IsA(pred_leftop, Const))
			return false;
		pred_const = (Const *) pred_leftop;
		if (clause_rightop == NULL || !IsA(clause_rightop, Const))
			return false;
		clause_const = (Const *) clause_rightop;
		/* 交换 pred_op，以便假定 Const 在右边 */
		pred_op = get_commutator(pred_op);
		if (!OidIsValid(pred_op))
			return false;
	}
	else
	{
		/* 任何子表达式都配不上，因此失败 */
		return false;
	}

	/*
	 * 我们有两个相同的子表达式，以及另外两个并不相同
	 * 但都是 Const 的子表达式；并且已在必要时交换了
	 * 操作符，使 Const 在右边。我们需要
	 * 比较这些 Const 的值。若任一为 NULL，就没法比较，因此
	 * 证明通常失败……但有些情形可以声称成功。
	 */
	if (clause_const->constisnull)
	{
		/* 若 clause_op 不严格，就证不出任何东西 */
		if (!op_strict(clause_op))
			return false;

		/*
		 * 至此我们知道子句返回 NULL。对那些
		 * 假定子句为真的证明类型，这意味着证明
		 * 空泛地为真（亦即假蕴含一切）。除了弱蕴含，
		 * 所有证明类型都是如此。
		 */
		if (!(weak && !refute_it))
			return true;

		/*
		 * 对弱蕴含，证明仍有可能成功，
		 * 若谓词也能被证明为 NULL。那时我们就有
		 * NULL => NULL，对这种证明类型是有效的。
		 */
		if (pred_const->constisnull && op_strict(pred_op))
			return true;
		/* 否则证明失败 */
		return false;
	}
	if (pred_const->constisnull)
	{
		/*
		 * 若 pred_op 严格，我们知道谓词得出 NULL，这
		 * 意味着对弱蕴含或弱
		 * 反驳，证明都成功。
		 */
		if (weak && op_strict(pred_op))
			return true;
		/* 否则证明失败 */
		return false;
	}

	/*
	 * 用系统目录和
	 * 操作符蕴含表查找常量比较操作符。
	 */
	test_op = get_btree_test_op(pred_op, clause_op, refute_it);

	if (!OidIsValid(test_op))
	{
		/* 找不到合适的比较操作符 */
		return false;
	}

	/*
	 * 求值该检验。为此需要一个 EState。
	 */
	estate = CreateExecutorState();

	/* 可以用 estate 的工作上下文来避免内存泄漏。 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* 建立表达式树 */
	test_expr = make_opclause(test_op,
							  BOOLOID,
							  false,
							  (Expr *) pred_const,
							  (Expr *) clause_const,
							  InvalidOid,
							  pred_collation);

	/* 填上 opfuncids */
	fix_opfuncids((Node *) test_expr);

	/* 准备执行它 */
	test_exprstate = ExecInitExpr(test_expr, NULL);

	/* 然后执行它。 */
	test_result = ExecEvalExprSwitchContext(test_exprstate,
											GetPerTupleExprContext(estate),
											&isNull);

	/* 回到外层内存上下文 */
	MemoryContextSwitchTo(oldcontext);

	/* 释放刚才建出来的那些杂物 */
	FreeExecutorState(estate);

	if (isNull)
	{
		/* 把空结果当成未证明……不过这有点可疑…… */
		elog(DEBUG2, "null predicate test result");
		return false;
	}
	return DatumGetBool(test_result);
}


/*
 * operator_same_subexprs_proof
 *	  假定 EXPR1 clause_op EXPR2 为真，尝试证明或反驳
 *	  EXPR1 pred_op EXPR2。
 *
 * 若能做出证明则返回真，若证不出则返回假。
 */
static bool
operator_same_subexprs_proof(Oid pred_op, Oid clause_op, bool refute_it)
{
	/*
	 * 一条简单而通用的规则是：若 clause_op
	 * 与 pred_op 相同则谓词得证；若二者互为否定子，则被反驳。
	 * 不必检查不可变性，因为 pred_op 已知
	 * 不可变。（实际上到这里我们拿到的可能是已知不可变的
	 * pred_op 的交换子，但那当然也应当不可变。
	 * 同样我们不担心 pred_op 的否定子是否不可变。）
	 *
	 * 注意：若我们实际上有 EXPR1 clause_op
	 * EXPR2 和 EXPR1 pred_op EXPR2，相同这一情形到不了这里，因为
	 * predicate_implied_by_simple_clause 里的整体表达式相等
	 * 检验会抓住它。但交换 pred_op 之后，仍可能见到同一个操作符。
	 */
	if (refute_it)
	{
		if (get_negator(pred_op) == clause_op)
			return true;
	}
	else
	{
		if (pred_op == clause_op)
			return true;
	}

	/*
	 * 否则，看能否通过某个 btree 操作符族找出
	 * 操作符之间的关系，从而判定蕴含。
	 */
	return operator_same_subexprs_lookup(pred_op, clause_op, refute_it);
}


/*
 * 我们用一张旁路表缓存 btree 证明操作符
 * 查找的结果，因为实际查找相当贵，而且对
 * 任何给定的一对操作符都不变（至少在 pg_amop 不
 * 变的时候）。单个散列项同时存放给定一对操作符的蕴含和反驳
 * 结果；但注意我们眼下也许只
 * 确定了其中一组结果。
 */
typedef struct OprProofCacheKey
{
	Oid			pred_op;		/* 谓词操作符 */
	Oid			clause_op;		/* 子句操作符 */
} OprProofCacheKey;

typedef struct OprProofCacheEntry
{
	/* 散列查找键必须放在 FIRST */
	OprProofCacheKey key;

	bool		have_implic;	/* 我们知道蕴含结果吗？ */
	bool		have_refute;	/* 我们知道反驳结果吗？ */
	bool		same_subexprs_implies;	/* X clause_op Y 是否蕴含 X pred_op Y？ */
	bool		same_subexprs_refutes;	/* X clause_op Y 是否反驳 X pred_op Y？ */
	Oid			implic_test_op; /* 检验操作符的 OID，没有则为 0 */
	Oid			refute_test_op; /* 检验操作符的 OID，没有则为 0 */
} OprProofCacheEntry;

static HTAB *OprProofCacheHash = NULL;


/*
 * lookup_proof_cache
 *	  取得适当的缓存项，必要时填上。
 */
static OprProofCacheEntry *
lookup_proof_cache(Oid pred_op, Oid clause_op, bool refute_it)
{
	OprProofCacheKey key;
	OprProofCacheEntry *cache_entry;
	bool		cfound;
	bool		same_subexprs = false;
	Oid			test_op = InvalidOid;
	bool		found = false;
	List	   *pred_op_infos,
			   *clause_op_infos;
	ListCell   *lcp,
			   *lcc;

	/*
	 * 为这一对操作符查找或建立缓存项。
	 */
	if (OprProofCacheHash == NULL)
	{
		/* 第一次经过：初始化散列表 */
		HASHCTL		ctl;

		ctl.keysize = sizeof(OprProofCacheKey);
		ctl.entrysize = sizeof(OprProofCacheEntry);
		OprProofCacheHash = hash_create("Btree proof lookup cache", 256,
										&ctl, HASH_ELEM | HASH_BLOBS);

		/* 安排在 pg_amop 变化时清空缓存 */
		CacheRegisterSyscacheCallback(AMOPOPID,
									  InvalidateOprProofCacheCallBack,
									  (Datum) 0);
	}

	key.pred_op = pred_op;
	key.clause_op = clause_op;
	cache_entry = (OprProofCacheEntry *) hash_search(OprProofCacheHash,
													 &key,
													 HASH_ENTER, &cfound);
	if (!cfound)
	{
		/* 新缓存项，把它设成无效 */
		cache_entry->have_implic = false;
		cache_entry->have_refute = false;
	}
	else
	{
		/* 已有的缓存项，看是否已经知道答案 */
		if (refute_it ? cache_entry->have_refute : cache_entry->have_implic)
			return cache_entry;
	}

	/*
	 * 试着找一个含有给定操作符的 btree 操作符族。
	 *
	 * 必须找到同时含有两个操作符的 btree 操作符族，否则
	 * 蕴含没法判定。另外，该操作符族必须含有
	 * 一个接受这些操作符右端数据类型的合适检验操作符。
	 *
	 * 若有多个匹配的操作符族，假定可以用其中任何一个来
	 * 判定两个操作符的逻辑关系以及正确的
	 * 对应检验操作符。对任何逻辑上
	 * 一致的操作符族，这都应当可行。
	 *
	 * 注意，即使某个操作符族缺少可用的检验
	 * 操作符，我们仍能判定相同子表达式情形下
	 * 操作符之间的关系。不完整的跨类型
	 * 比较操作符集合就会出现这种情况。
	 */
	clause_op_infos = get_op_btree_interpretation(clause_op);
	if (clause_op_infos)
		pred_op_infos = get_op_btree_interpretation(pred_op);
	else						/* 没有查找的必要 */
		pred_op_infos = NIL;

	foreach(lcp, pred_op_infos)
	{
		OpBtreeInterpretation *pred_op_info = lfirst(lcp);
		Oid			opfamily_id = pred_op_info->opfamily_id;

		foreach(lcc, clause_op_infos)
		{
			OpBtreeInterpretation *clause_op_info = lfirst(lcc);
			StrategyNumber pred_strategy,
						clause_strategy,
						test_strategy;

			/* 必须在同一个操作符族里找到它们 */
			if (opfamily_id != clause_op_info->opfamily_id)
				continue;
			/* 左端类型应当匹配 */
			Assert(clause_op_info->oplefttype == pred_op_info->oplefttype);

			pred_strategy = pred_op_info->strategy;
			clause_strategy = clause_op_info->strategy;

			/*
			 * 检查能否根据这一操作符族中操作符的关系，
			 * 为相同子表达式的情形做出证明。
			 */
			if (refute_it)
				same_subexprs |= BT_refutes_table[clause_strategy - 1][pred_strategy - 1];
			else
				same_subexprs |= BT_implies_table[clause_strategy - 1][pred_strategy - 1];

			/*
			 * 在蕴含表里查找检验策略号
			 */
			if (refute_it)
				test_strategy = BT_refute_table[clause_strategy - 1][pred_strategy - 1];
			else
				test_strategy = BT_implic_table[clause_strategy - 1][pred_strategy - 1];

			if (test_strategy == 0)
			{
				/* 用这种解释无法判定蕴含 */
				continue;
			}

			/*
			 * 看操作符族是否有针对该检验策略和
			 * 这些数据类型的操作符。
			 */
			if (test_strategy == BTNE)
			{
				test_op = get_opfamily_member(opfamily_id,
											  pred_op_info->oprighttype,
											  clause_op_info->oprighttype,
											  BTEqualStrategyNumber);
				if (OidIsValid(test_op))
					test_op = get_negator(test_op);
			}
			else
			{
				test_op = get_opfamily_member(opfamily_id,
											  pred_op_info->oprighttype,
											  clause_op_info->oprighttype,
											  test_strategy);
			}

			if (!OidIsValid(test_op))
				continue;

			/*
			 * 最后一项检查：test_op 必须不可变。
			 *
			 * 注意我们只需要 test_op 不可变，原来的
			 * clause_op 则不必。（假定调用者已经检查
			 * pred_op 不可变。）本质上我们假定操作符族
			 * 即使含有仅仅稳定的操作符，
			 * 仍然是一致的。
			 */
			if (op_volatile(test_op) == PROVOLATILE_IMMUTABLE)
			{
				found = true;
				break;
			}
		}

		if (found)
			break;
	}

	list_free_deep(pred_op_infos);
	list_free_deep(clause_op_infos);

	if (!found)
	{
		/* 找不到合适的比较操作符 */
		test_op = InvalidOid;
	}

	/*
	 * 若我们认为已经能对相同子表达式
	 * 情形证明些什么，在完全采信之前要检查 clause_op 是否不可变。
	 * （通常若 pred_op 不可变，clause_op 也会不可变，但
	 * 并不完全清楚这在所有
	 * 情形下都必定为真，所以还是检查一下。）
	 */
	if (same_subexprs &&
		op_volatile(clause_op) != PROVOLATILE_IMMUTABLE)
		same_subexprs = false;

	/* 把结果放进缓存，不论是肯定还是否定 */
	if (refute_it)
	{
		cache_entry->refute_test_op = test_op;
		cache_entry->same_subexprs_refutes = same_subexprs;
		cache_entry->have_refute = true;
	}
	else
	{
		cache_entry->implic_test_op = test_op;
		cache_entry->same_subexprs_implies = same_subexprs;
		cache_entry->have_implic = true;
	}

	return cache_entry;
}

/*
 * operator_same_subexprs_lookup
 *	  查找相同子表达式情形下缓存答案的
 *	  方便子程序。
 */
static bool
operator_same_subexprs_lookup(Oid pred_op, Oid clause_op, bool refute_it)
{
	OprProofCacheEntry *cache_entry;

	cache_entry = lookup_proof_cache(pred_op, clause_op, refute_it);
	if (refute_it)
		return cache_entry->same_subexprs_refutes;
	else
		return cache_entry->same_subexprs_implies;
}

/*
 * get_btree_test_op
 *	  找出在涉及常量比较的证明或反驳中，
 *	  btree 操作符所需的比较操作符。
 *
 * 给定子句 var clause_op const1 为真，我们试图
 * 证明或反驳谓词 var pred_op const2。两个操作符的身份
 * 足以决定用哪个操作符（若有的话）来比较
 * const2 和 const1。
 *
 * 返回要使用的操作符 OID，
 * 若无法证明则返回 InvalidOid。
 */
static Oid
get_btree_test_op(Oid pred_op, Oid clause_op, bool refute_it)
{
	OprProofCacheEntry *cache_entry;

	cache_entry = lookup_proof_cache(pred_op, clause_op, refute_it);
	if (refute_it)
		return cache_entry->refute_test_op;
	else
		return cache_entry->implic_test_op;
}


/*
 * pg_amop 失效事件的回调
 */
static void
InvalidateOprProofCacheCallBack(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	OprProofCacheEntry *hentry;

	Assert(OprProofCacheHash != NULL);

	/* 目前只是重置全部项；很难更聪明…… */
	hash_seq_init(&status, OprProofCacheHash);

	while ((hentry = (OprProofCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		hentry->have_implic = false;
		hentry->have_refute = false;
	}
}
