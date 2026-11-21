/*-------------------------------------------------------------------------
 *
 * parsetree.h
 *	  访问解析树各成分与子成分的
 *	  例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parsetree.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSETREE_H
#define PARSETREE_H

#include "nodes/parsenodes.h"


/* ----------------
 *		范围表操作
 * ----------------
 */

/*
 *		rt_fetch
 *
 * 注意：若 RT 下标越界将直接崩溃
 */
#define rt_fetch(rangetable_index, rangetable) \
	((RangeTblEntry *) list_nth(rangetable, (rangetable_index)-1))

/*
 * 给定 RTE 与属性号，返回该 RTE
 * 该属性的合适变量名或别名。
 */
extern char *get_rte_attribute_name(RangeTblEntry *rte, AttrNumber attnum);

/*
 * 检查 RTE 的某属性是否已被删除
 */
extern bool get_rte_attribute_is_dropped(RangeTblEntry *rte,
										 AttrNumber attnum);


/* ----------------
 *		目标列表操作
 * ----------------
 */

extern TargetEntry *get_tle_by_resno(List *tlist, AttrNumber resno);

/* ----------------
 *		FOR UPDATE/SHARE
 * ----------------
 */

extern RowMarkClause *get_parse_rowmark(Query *qry, Index rtindex);

#endif							/* PARSETREE_H */
