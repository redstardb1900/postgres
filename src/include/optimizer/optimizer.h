/*-------------------------------------------------------------------------
 *
 * optimizer.h
 *	Postgres 规划器的外部 API。
 *
 * 本头意在定义核心规划器向非规划器模块
 * 暴露以供使用的一切。
 *
 * 注意 src/backend/optimizer/ 之外亦有文件被视为
 * 规划器模块，因其与规划器操作缠得太紧，
 * 难以另当别论。FDW 规划代码即为一例。
 * 但大体上，核心规划器之外的代码
 * 除本头外不必包含任何 optimizer/ 头。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/optimizer.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "nodes/parsenodes.h"

/*
 * 不想在此含 nodes/pathnodes.h，因非规划器
 * 代码一般应将 PlannerInfo 视为不透明 typedef。
 * 但希望此类代码用该 typedef 名，故在
 * 此处或 pathnodes.h（先读到者）定义之。
 */
#ifndef HAVE_PLANNERINFO_TYPEDEF
typedef struct PlannerInfo PlannerInfo;
#define HAVE_PLANNERINFO_TYPEDEF 1
#endif

/* IndexOptInfo 与 SpecialJoinInfo 同理。 */
#ifndef HAVE_INDEXOPTINFO_TYPEDEF
typedef struct IndexOptInfo IndexOptInfo;
#define HAVE_INDEXOPTINFO_TYPEDEF 1
#endif
#ifndef HAVE_SPECIALJOININFO_TYPEDEF
typedef struct SpecialJoinInfo SpecialJoinInfo;
#define HAVE_SPECIALJOININFO_TYPEDEF 1
#endif

/* 亦最好不在此含 plannodes.h、params.h 或 htup.h */
struct PlannedStmt;
struct ParamListInfoData;
struct HeapTupleData;


/* path/clausesel.c： */

extern Selectivity clause_selectivity(PlannerInfo *root,
									  Node *clause,
									  int varRelid,
									  JoinType jointype,
									  SpecialJoinInfo *sjinfo);
extern Selectivity clause_selectivity_ext(PlannerInfo *root,
										  Node *clause,
										  int varRelid,
										  JoinType jointype,
										  SpecialJoinInfo *sjinfo,
										  bool use_extended_stats);
extern Selectivity clauselist_selectivity(PlannerInfo *root,
										  List *clauses,
										  int varRelid,
										  JoinType jointype,
										  SpecialJoinInfo *sjinfo);
extern Selectivity clauselist_selectivity_ext(PlannerInfo *root,
											  List *clauses,
											  int varRelid,
											  JoinType jointype,
											  SpecialJoinInfo *sjinfo,
											  bool use_extended_stats);

/* path/costsize.c： */

/* 广泛使用的代价参数 */
extern PGDLLIMPORT double seq_page_cost;
extern PGDLLIMPORT double random_page_cost;
extern PGDLLIMPORT double cpu_tuple_cost;
extern PGDLLIMPORT double cpu_index_tuple_cost;
extern PGDLLIMPORT double cpu_operator_cost;
extern PGDLLIMPORT double parallel_tuple_cost;
extern PGDLLIMPORT double parallel_setup_cost;
extern PGDLLIMPORT double recursive_worktable_factor;
extern PGDLLIMPORT int effective_cache_size;

extern double clamp_row_est(double nrows);
extern long clamp_cardinality_to_long(Cardinality x);

/* path/indxpath.c： */

extern bool is_pseudo_constant_for_index(PlannerInfo *root, Node *expr,
										 IndexOptInfo *index);

/* plan/planner.c： */

/* debug_parallel_query 的可能取值 */
typedef enum
{
	DEBUG_PARALLEL_OFF,
	DEBUG_PARALLEL_ON,
	DEBUG_PARALLEL_REGRESS
}			DebugParallelMode;

/* GUC 参数 */
extern PGDLLIMPORT int debug_parallel_query;
extern PGDLLIMPORT bool parallel_leader_participation;

extern struct PlannedStmt *planner(Query *parse, const char *query_string,
								   int cursorOptions,
								   struct ParamListInfoData *boundParams);

extern Expr *expression_planner(Expr *expr);
extern Expr *expression_planner_with_deps(Expr *expr,
										  List **relationOids,
										  List **invalItems);

extern bool plan_cluster_use_sort(Oid tableOid, Oid indexOid);
extern int	plan_create_index_workers(Oid tableOid, Oid indexOid);

/* plan/setrefs.c： */

extern void extract_query_dependencies(Node *query,
									   List **relationOids,
									   List **invalItems,
									   bool *hasRowSecurity);

/* prep/prepqual.c： */

extern Node *negate_clause(Node *node);
extern Expr *canonicalize_qual(Expr *qual, bool is_check);

/* util/clauses.c： */

extern bool contain_mutable_functions(Node *clause);
extern bool contain_mutable_functions_after_planning(Expr *expr);
extern bool contain_volatile_functions(Node *clause);
extern bool contain_volatile_functions_after_planning(Expr *expr);
extern bool contain_volatile_functions_not_nextval(Node *clause);

extern Node *eval_const_expressions(PlannerInfo *root, Node *node);

extern void convert_saop_to_hashed_saop(Node *node);

extern Node *estimate_expression_value(PlannerInfo *root, Node *node);

extern Expr *evaluate_expr(Expr *expr, Oid result_type, int32 result_typmod,
						   Oid result_collation);

extern List *expand_function_arguments(List *args, bool include_out_arguments,
									   Oid result_type,
									   struct HeapTupleData *func_tuple);

/* util/predtest.c： */

extern bool predicate_implied_by(List *predicate_list, List *clause_list,
								 bool weak);
extern bool predicate_refuted_by(List *predicate_list, List *clause_list,
								 bool weak);

/* util/tlist.c： */

extern int	count_nonjunk_tlist_entries(List *tlist);
extern TargetEntry *get_sortgroupref_tle(Index sortref,
										 List *targetList);
extern TargetEntry *get_sortgroupclause_tle(SortGroupClause *sgClause,
											List *targetList);
extern Node *get_sortgroupclause_expr(SortGroupClause *sgClause,
									  List *targetList);
extern List *get_sortgrouplist_exprs(List *sgClauses,
									 List *targetList);
extern SortGroupClause *get_sortgroupref_clause(Index sortref,
												List *clauses);
extern SortGroupClause *get_sortgroupref_clause_noerr(Index sortref,
													  List *clauses);

/* util/var.c： */

/* 可 OR 进 pull_var_clause() flags 参数的位 */
#define PVC_INCLUDE_AGGREGATES	0x0001	/* 在输出列表中含 Aggrefs */
#define PVC_RECURSE_AGGREGATES	0x0002	/* 递归进入 Aggref 参数 */
#define PVC_INCLUDE_WINDOWFUNCS 0x0004	/* 在输出列表中含 WindowFuncs */
#define PVC_RECURSE_WINDOWFUNCS 0x0008	/* 递归进入 WindowFunc 参数 */
#define PVC_INCLUDE_PLACEHOLDERS	0x0010	/* include PlaceHolderVars in
											 * output list */
#define PVC_RECURSE_PLACEHOLDERS	0x0020	/* 递归进入 PlaceHolderVar
											 * 参数 */

extern Bitmapset *pull_varnos(PlannerInfo *root, Node *node);
extern Bitmapset *pull_varnos_of_level(PlannerInfo *root, Node *node, int levelsup);
extern void pull_varattnos(Node *node, Index varno, Bitmapset **varattnos);
extern List *pull_vars_of_level(Node *node, int levelsup);
extern bool contain_var_clause(Node *node);
extern bool contain_vars_of_level(Node *node, int levelsup);
extern int	locate_var_of_level(Node *node, int levelsup);
extern List *pull_var_clause(Node *node, int flags);
extern Node *flatten_join_alias_vars(PlannerInfo *root, Query *query, Node *node);

#endif							/* OPTIMIZER_H */
