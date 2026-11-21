/*-------------------------------------------------------------------------
 *
 * 前端代码的简单列表设施
 *
 * OIDs、字符串与指针的简单列表数据结构。相对
 * 后端 List 设施非常原始，但如 pg_dump 所需
 * 已够用。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/simple_list.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SIMPLE_LIST_H
#define SIMPLE_LIST_H

typedef struct SimpleOidListCell
{
	struct SimpleOidListCell *next;
	Oid			val;
} SimpleOidListCell;

typedef struct SimpleOidList
{
	SimpleOidListCell *head;
	SimpleOidListCell *tail;
} SimpleOidList;

typedef struct SimpleStringListCell
{
	struct SimpleStringListCell *next;
	bool		touched;		/*  真：此串已被搜索并
								 * 触及 */
	char		val[FLEXIBLE_ARRAY_MEMBER]; /* 略 */
} SimpleStringListCell;

typedef struct SimpleStringList
{
	SimpleStringListCell *head;
	SimpleStringListCell *tail;
} SimpleStringList;

typedef struct SimplePtrListCell
{
	struct SimplePtrListCell *next;
	void	   *ptr;
} SimplePtrListCell;

typedef struct SimplePtrList
{
	SimplePtrListCell *head;
	SimplePtrListCell *tail;
} SimplePtrList;

extern void simple_oid_list_append(SimpleOidList *list, Oid val);
extern bool simple_oid_list_member(SimpleOidList *list, Oid val);
extern void simple_oid_list_destroy(SimpleOidList *list);

extern void simple_string_list_append(SimpleStringList *list, const char *val);
extern bool simple_string_list_member(SimpleStringList *list, const char *val);
extern void simple_string_list_destroy(SimpleStringList *list);

extern const char *simple_string_list_not_touched(SimpleStringList *list);

extern void simple_ptr_list_append(SimplePtrList *list, void *ptr);

#endif							/* SIMPLE_LIST_H */
