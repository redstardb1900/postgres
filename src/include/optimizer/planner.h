/*-------------------------------------------------------------------------
 *
 * planner.h
 *	planner.c 的原型。
 *
 * 注意 planner.c 主入口声明于
 * optimizer/optimizer.h，因其拟由非规划器
 * 代码调用。此处声明供其他规划器模块
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/planner.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"


/* 插件在 planner() 取得控制的钩子 */
typedef PlannedStmt *(*planner_hook_type) (Query *parse,
										   const char *query_string,
										   int cursorOptions,
										   ParamListInfo boundParams);
extern PGDLLIMPORT planner_hook_type planner_hook;

/* grouping_planner() 规划上层关系时插件取得控制的钩子 */
typedef void (*create_upper_paths_hook_type) (PlannerInfo *root,
											  UpperRelationKind stage,
											  RelOptInfo *input_rel,
											  RelOptInfo *output_rel,
											  void *extra);
extern PGDLLIMPORT create_upper_paths_hook_type create_upper_paths_hook;


extern PlannedStmt *standard_planner(Query *parse, const char *query_string,
									 int cursorOptions,
									 ParamListInfo boundParams);

extern PlannerInfo *subquery_planner(PlannerGlobal *glob, Query *parse,
									 PlannerInfo *parent_root,
									 bool hasRecursion, double tuple_fraction);

extern RowMarkType select_rowmark_type(RangeTblEntry *rte,
									   LockClauseStrength strength);

extern bool limit_needed(Query *parse);

extern void mark_partial_aggref(Aggref *agg, AggSplit aggsplit);

extern Path *get_cheapest_fractional_path(RelOptInfo *rel,
										  double tuple_fraction);

extern Expr *preprocess_phv_expression(PlannerInfo *root, Expr *expr);

#endif							/* PLANNER_H */
