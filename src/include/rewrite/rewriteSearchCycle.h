/*-------------------------------------------------------------------------
 *
 * rewriteSearchCycle.h
 *		改写 SEARCH 与 CYCLE 子句的支持。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/rewrite/rewriteSearchCycle.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITESEARCHCYCLE_H
#define REWRITESEARCHCYCLE_H

#include "nodes/parsenodes.h"

extern CommonTableExpr *rewriteSearchAndCycle(CommonTableExpr *cte);

#endif							/* REWRITESEARCHCYCLE_H */
