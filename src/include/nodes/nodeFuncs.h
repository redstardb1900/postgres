/*-------------------------------------------------------------------------
 *
 * nodeFuncs.h
 *	Node 树的各类通用操作
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/nodeFuncs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEFUNCS_H
#define NODEFUNCS_H

#include "nodes/parsenodes.h"

struct PlanState;				/* 避免也包含 execnodes.h */


/* query_tree_walker 与 query_tree_mutator 的标志位 */
#define QTW_IGNORE_RT_SUBQUERIES	0x01	/* rtable 中子查询 */
#define QTW_IGNORE_CTE_SUBQUERIES	0x02	/* cteList 中子查询 */
#define QTW_IGNORE_RC_SUBQUERIES	0x03	/* 以上两者 */
#define QTW_IGNORE_JOINALIASES		0x04	/* JOIN 别名 var 表 */
#define QTW_IGNORE_RANGE_TABLE		0x08	/* 完全跳过范围表 */
#define QTW_EXAMINE_RTES_BEFORE		0x10	/* 先于内容检查 RTE
											 * 节点 */
#define QTW_EXAMINE_RTES_AFTER		0x20	/* 后于内容检查 RTE
											 * 节点 */
#define QTW_DONT_COPY_QUERY			0x40	/* 不复制顶层 Query */
#define QTW_EXAMINE_SORTGROUP		0x80	/* 包含 SortGroupClause 列表 */

/* check_functions_in_node 的回调 */
typedef bool (*check_function_callback) (Oid func_id, void *context);

/* 树 walker 的回调函数 */
typedef bool (*tree_walker_callback) (Node *node, void *context);
typedef bool (*planstate_tree_walker_callback) (struct PlanState *planstate,
												void *context);

/* 树 mutator 的回调函数 */
typedef Node *(*tree_mutator_callback) (Node *node, void *context);


extern Oid	exprType(const Node *expr);
extern int32 exprTypmod(const Node *expr);
extern bool exprIsLengthCoercion(const Node *expr, int32 *coercedTypmod);
extern Node *applyRelabelType(Node *arg, Oid rtype, int32 rtypmod, Oid rcollid,
							  CoercionForm rformat, int rlocation,
							  bool overwrite_ok);
extern Node *relabel_to_typmod(Node *expr, int32 typmod);
extern Node *strip_implicit_coercions(Node *node);
extern bool expression_returns_set(Node *clause);

extern Oid	exprCollation(const Node *expr);
extern Oid	exprInputCollation(const Node *expr);
extern void exprSetCollation(Node *expr, Oid collation);
extern void exprSetInputCollation(Node *expr, Oid inputcollation);

extern int	exprLocation(const Node *expr);

extern void fix_opfuncids(Node *node);
extern void set_opfuncid(OpExpr *opexpr);
extern void set_sa_opfuncid(ScalarArrayOpExpr *opexpr);

/* 子句是否为 FuncExpr？ */
static inline bool
is_funcclause(const void *clause)
{
	return clause != NULL && IsA(clause, FuncExpr);
}

/* 子句是否为 OpExpr？ */
static inline bool
is_opclause(const void *clause)
{
	return clause != NULL && IsA(clause, OpExpr);
}

/* 取二元 opclause 左参,或一元 opclause 的唯一参 */
static inline Node *
get_leftop(const void *clause)
{
	const OpExpr *expr = (const OpExpr *) clause;

	if (expr->args != NIL)
		return (Node *) linitial(expr->args);
	else
		return NULL;
}

/* 取二元 opclause 右参(一元则为 NULL) */
static inline Node *
get_rightop(const void *clause)
{
	const OpExpr *expr = (const OpExpr *) clause;

	if (list_length(expr->args) >= 2)
		return (Node *) lsecond(expr->args);
	else
		return NULL;
}

/* 子句是否为 AND？ */
static inline bool
is_andclause(const void *clause)
{
	return (clause != NULL &&
			IsA(clause, BoolExpr) &&
			((const BoolExpr *) clause)->boolop == AND_EXPR);
}

/* 子句是否为 OR？ */
static inline bool
is_orclause(const void *clause)
{
	return (clause != NULL &&
			IsA(clause, BoolExpr) &&
			((const BoolExpr *) clause)->boolop == OR_EXPR);
}

/* 子句是否为 NOT？ */
static inline bool
is_notclause(const void *clause)
{
	return (clause != NULL &&
			IsA(clause, BoolExpr) &&
			((const BoolExpr *) clause)->boolop == NOT_EXPR);
}

/* 从已知为 NOT 的子句取出参数 */
static inline Expr *
get_notclausearg(const void *notclause)
{
	return (Expr *) linitial(((const BoolExpr *) notclause)->args);
}

extern bool check_functions_in_node(Node *node, check_function_callback checker,
									void *context);

/*
 * The following functions are usually passed walker or mutator callbacks
 * that are declared like "bool walker(Node *node, my_struct *context)"
 * rather than "bool walker(Node *node, void *context)" as a strict reading
 * of the C standard would require.  Changing the callbacks' declarations
 * to "void *" would create serious hazards of passing them the wrong context
 * struct type, so we respectfully decline to support the standard's position
 * that a pointer to struct is incompatible with "void *".  Instead, silence
 * related compiler warnings by inserting casts into these macro wrappers.
 */

#define expression_tree_walker(n, w, c) \
	expression_tree_walker_impl(n, (tree_walker_callback) (w), c)
#define expression_tree_mutator(n, m, c) \
	expression_tree_mutator_impl(n, (tree_mutator_callback) (m), c)

#define query_tree_walker(q, w, c, f) \
	query_tree_walker_impl(q, (tree_walker_callback) (w), c, f)
#define query_tree_mutator(q, m, c, f) \
	query_tree_mutator_impl(q, (tree_mutator_callback) (m), c, f)

#define range_table_walker(rt, w, c, f) \
	range_table_walker_impl(rt, (tree_walker_callback) (w), c, f)
#define range_table_mutator(rt, m, c, f) \
	range_table_mutator_impl(rt, (tree_mutator_callback) (m), c, f)

#define range_table_entry_walker(r, w, c, f) \
	range_table_entry_walker_impl(r, (tree_walker_callback) (w), c, f)

#define query_or_expression_tree_walker(n, w, c, f) \
	query_or_expression_tree_walker_impl(n, (tree_walker_callback) (w), c, f)
#define query_or_expression_tree_mutator(n, m, c, f) \
	query_or_expression_tree_mutator_impl(n, (tree_mutator_callback) (m), c, f)

#define raw_expression_tree_walker(n, w, c) \
	raw_expression_tree_walker_impl(n, (tree_walker_callback) (w), c)

#define planstate_tree_walker(ps, w, c) \
	planstate_tree_walker_impl(ps, (planstate_tree_walker_callback) (w), c)

extern bool expression_tree_walker_impl(Node *node,
										tree_walker_callback walker,
										void *context);
extern Node *expression_tree_mutator_impl(Node *node,
										  tree_mutator_callback mutator,
										  void *context);

extern bool query_tree_walker_impl(Query *query,
								   tree_walker_callback walker,
								   void *context, int flags);
extern Query *query_tree_mutator_impl(Query *query,
									  tree_mutator_callback mutator,
									  void *context, int flags);

extern bool range_table_walker_impl(List *rtable,
									tree_walker_callback walker,
									void *context, int flags);
extern List *range_table_mutator_impl(List *rtable,
									  tree_mutator_callback mutator,
									  void *context, int flags);

extern bool range_table_entry_walker_impl(RangeTblEntry *rte,
										  tree_walker_callback walker,
										  void *context, int flags);

extern bool query_or_expression_tree_walker_impl(Node *node,
												 tree_walker_callback walker,
												 void *context, int flags);
extern Node *query_or_expression_tree_mutator_impl(Node *node,
												   tree_mutator_callback mutator,
												   void *context, int flags);

extern bool raw_expression_tree_walker_impl(Node *node,
											tree_walker_callback walker,
											void *context);

extern bool planstate_tree_walker_impl(struct PlanState *planstate,
									   planstate_tree_walker_callback walker,
									   void *context);

#endif							/* NODEFUNCS_H */
