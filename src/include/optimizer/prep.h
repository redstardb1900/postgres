/*-------------------------------------------------------------------------
 *
 * prep.h
 *	optimizer/prep/ 下各文件的原型
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/prep.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREP_H
#define PREP_H

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"


/*
 * prepjointree.c 的原型
 */
extern void transform_MERGE_to_join(Query *parse);
extern void replace_empty_jointree(Query *parse);
extern void pull_up_sublinks(PlannerInfo *root);
extern void preprocess_function_rtes(PlannerInfo *root);
extern void pull_up_subqueries(PlannerInfo *root);
extern void flatten_simple_union_all(PlannerInfo *root);
extern void reduce_outer_joins(PlannerInfo *root);
extern void remove_useless_result_rtes(PlannerInfo *root);
extern Relids get_relids_in_jointree(Node *jtnode, bool include_outer_joins,
									 bool include_inner_joins);
extern Relids get_relids_for_join(Query *query, int joinrelid);

/*
 * preptlist.c 的原型
 */
extern void preprocess_targetlist(PlannerInfo *root);

extern List *extract_update_targetlist_colnos(List *tlist);

extern PlanRowMark *get_plan_rowmark(List *rowmarks, Index rtindex);

/*
 * prepagg.c 的原型
 */
extern void get_agg_clause_costs(PlannerInfo *root, AggSplit aggsplit,
								 AggClauseCosts *costs);
extern void preprocess_aggrefs(PlannerInfo *root, Node *clause);

/*
 * prepunion.c 的原型
 */
extern RelOptInfo *plan_set_operations(PlannerInfo *root);


#endif							/* PREP_H */
