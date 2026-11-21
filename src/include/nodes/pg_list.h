/*-------------------------------------------------------------------------
 *
 * pg_list.h
 *	PostgreSQL 通用列表包的接口
 *
 * 曾几何时 Postgres 部分用 Lisp 写,主要数据结构用真
 * cons 单元列表。改写为 C 后,起初忠实模拟 cons 列表,
 * 不出所料成性能瓶颈。几经大改后,这些结构其实是
 * 简单可扩展数组;但 "List" 之名与大量记法
 * 仍保留。
 * 见上。
 *
 * 对原实现的重要让步是:空列表总用空指针表示
 * (优先写作 NIL)。非空列表有一头部,只要列表保持非空
 * 就不会被搬迁,另有可扩展
 * 数据数组。
 *
 * 我们支持四种列表类型:
 *
 *	T_List: 指针列表
 *		(实践中多为指向 Node 的指针,但并非总是;
 *		声明为 "void *" 以减少强制转换麻烦)
 *	T_IntList: 整数列表
 *	T_OidList: Oid 列表
 *	T_XidList: TransactionId 列表
 *		(XidList 设施不如其他情形完备)
 *
 * (目前 int、Oid 与 XID 同宽,但未必永远如此;
 * 请按数据选用合适的列表类型。)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/pg_list.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LIST_H
#define PG_LIST_H

#include "nodes/nodes.h"


typedef union ListCell
{
	void	   *ptr_value;
	int			int_value;
	Oid			oid_value;
	TransactionId xid_value;
} ListCell;

typedef struct List
{
	NodeTag		type;			/* T_List/T_IntList/T_OidList/T_XidList */
	int			length;			/* 当前元素个数 */
	int			max_length;		/* elements[] 的已分配长度 */
	ListCell   *elements;		/* 可再分配的单元数组 */
	/* 可与 List 头一并分配若干单元: */
	ListCell	initial_elements[FLEXIBLE_ARRAY_MEMBER];
	/* 若 elements == initial_elements,则非单独分配 */
} List;

/*
 * 空列表*唯一*合法表示是 NIL;换言之,
 * 非 NIL 列表保证长度 >= 1。
 */
#define NIL						((List *) NULL)

/*
 * 下列循环宏所用的状态结构。
 */
typedef struct ForEachState
{
	const List *l;				/* 正在遍历的列表 */
	int			i;				/* 当前元素下标 */
} ForEachState;

typedef struct ForBothState
{
	const List *l1;				/* 正在遍历的列表 */
	const List *l2;
	int			i;				/* 公共元素下标 */
} ForBothState;

typedef struct ForBothCellState
{
	const List *l1;				/* 正在遍历的列表 */
	const List *l2;
	int			i1;				/* 当前元素下标 */
	int			i2;
} ForBothCellState;

typedef struct ForThreeState
{
	const List *l1;				/* 正在遍历的列表 */
	const List *l2;
	const List *l3;
	int			i;				/* 公共元素下标 */
} ForThreeState;

typedef struct ForFourState
{
	const List *l1;				/* 正在遍历的列表 */
	const List *l2;
	const List *l3;
	const List *l4;
	int			i;				/* 公共元素下标 */
} ForFourState;

typedef struct ForFiveState
{
	const List *l1;				/* 正在遍历的列表 */
	const List *l2;
	const List *l3;
	const List *l4;
	const List *l5;
	int			i;				/* 公共元素下标 */
} ForFiveState;

/*
 * 这些例程够小且常用,值得做成
 * 内联.
 */

/* 取列表首单元地址;空列表则为 NULL */
static inline ListCell *
list_head(const List *l)
{
	return l ? &l->elements[0] : NULL;
}

/* 取列表末单元地址;空列表则为 NULL */
static inline ListCell *
list_tail(const List *l)
{
	return l ? &l->elements[l->length - 1] : NULL;
}

/* 取列表第二单元地址(若有),否则 NULL */
static inline ListCell *
list_second_cell(const List *l)
{
	if (l && l->length >= 2)
		return &l->elements[1];
	else
		return NULL;
}

/* 取列表长度 */
static inline int
list_length(const List *l)
{
	return l ? l->length : 0;
}

/*
 * 访问 List 单元内数据值的宏。
 *
 * 除 "xxx_node" 宏外,这些都是左值,
 * 可赋值。
 *
 * 注意:List API 前身留下一桩不幸遗产:
 * 宏 lfirst() 曾表示"此 cons 单元中的数据"。
 * 为免改遍所有 lfirst() 用法,该含义保留。
 * 故 lfirst() 接受 ListCell 并返回其中数据;
 * 要取 List 首单元数据请用 linitial()。更糟的是
 * lsecond() 更接近 linitial() 而非 lfirst():给定
 * List,lsecond() 返回第二列表单元中的
 * 数据。
 */
#define lfirst(lc)				((lc)->ptr_value)
#define lfirst_int(lc)			((lc)->int_value)
#define lfirst_oid(lc)			((lc)->oid_value)
#define lfirst_xid(lc)			((lc)->xid_value)
#define lfirst_node(type,lc)	castNode(type, lfirst(lc))

#define linitial(l)				lfirst(list_nth_cell(l, 0))
#define linitial_int(l)			lfirst_int(list_nth_cell(l, 0))
#define linitial_oid(l)			lfirst_oid(list_nth_cell(l, 0))
#define linitial_node(type,l)	castNode(type, linitial(l))

#define lsecond(l)				lfirst(list_nth_cell(l, 1))
#define lsecond_int(l)			lfirst_int(list_nth_cell(l, 1))
#define lsecond_oid(l)			lfirst_oid(list_nth_cell(l, 1))
#define lsecond_node(type,l)	castNode(type, lsecond(l))

#define lthird(l)				lfirst(list_nth_cell(l, 2))
#define lthird_int(l)			lfirst_int(list_nth_cell(l, 2))
#define lthird_oid(l)			lfirst_oid(list_nth_cell(l, 2))
#define lthird_node(type,l)		castNode(type, lthird(l))

#define lfourth(l)				lfirst(list_nth_cell(l, 3))
#define lfourth_int(l)			lfirst_int(list_nth_cell(l, 3))
#define lfourth_oid(l)			lfirst_oid(list_nth_cell(l, 3))
#define lfourth_node(type,l)	castNode(type, lfourth(l))

#define llast(l)				lfirst(list_last_cell(l))
#define llast_int(l)			lfirst_int(list_last_cell(l))
#define llast_oid(l)			lfirst_oid(list_last_cell(l))
#define llast_xid(l)			lfirst_xid(list_last_cell(l))
#define llast_node(type,l)		castNode(type, llast(l))

/*
 * 构造定长列表的便利宏
 */
#define list_make_ptr_cell(v)	((ListCell) {.ptr_value = (v)})
#define list_make_int_cell(v)	((ListCell) {.int_value = (v)})
#define list_make_oid_cell(v)	((ListCell) {.oid_value = (v)})
#define list_make_xid_cell(v)	((ListCell) {.xid_value = (v)})

#define list_make1(x1) \
	list_make1_impl(T_List, list_make_ptr_cell(x1))
#define list_make2(x1,x2) \
	list_make2_impl(T_List, list_make_ptr_cell(x1), list_make_ptr_cell(x2))
#define list_make3(x1,x2,x3) \
	list_make3_impl(T_List, list_make_ptr_cell(x1), list_make_ptr_cell(x2), \
					list_make_ptr_cell(x3))
#define list_make4(x1,x2,x3,x4) \
	list_make4_impl(T_List, list_make_ptr_cell(x1), list_make_ptr_cell(x2), \
					list_make_ptr_cell(x3), list_make_ptr_cell(x4))
#define list_make5(x1,x2,x3,x4,x5) \
	list_make5_impl(T_List, list_make_ptr_cell(x1), list_make_ptr_cell(x2), \
					list_make_ptr_cell(x3), list_make_ptr_cell(x4), \
					list_make_ptr_cell(x5))

#define list_make1_int(x1) \
	list_make1_impl(T_IntList, list_make_int_cell(x1))
#define list_make2_int(x1,x2) \
	list_make2_impl(T_IntList, list_make_int_cell(x1), list_make_int_cell(x2))
#define list_make3_int(x1,x2,x3) \
	list_make3_impl(T_IntList, list_make_int_cell(x1), list_make_int_cell(x2), \
					list_make_int_cell(x3))
#define list_make4_int(x1,x2,x3,x4) \
	list_make4_impl(T_IntList, list_make_int_cell(x1), list_make_int_cell(x2), \
					list_make_int_cell(x3), list_make_int_cell(x4))
#define list_make5_int(x1,x2,x3,x4,x5) \
	list_make5_impl(T_IntList, list_make_int_cell(x1), list_make_int_cell(x2), \
					list_make_int_cell(x3), list_make_int_cell(x4), \
					list_make_int_cell(x5))

#define list_make1_oid(x1) \
	list_make1_impl(T_OidList, list_make_oid_cell(x1))
#define list_make2_oid(x1,x2) \
	list_make2_impl(T_OidList, list_make_oid_cell(x1), list_make_oid_cell(x2))
#define list_make3_oid(x1,x2,x3) \
	list_make3_impl(T_OidList, list_make_oid_cell(x1), list_make_oid_cell(x2), \
					list_make_oid_cell(x3))
#define list_make4_oid(x1,x2,x3,x4) \
	list_make4_impl(T_OidList, list_make_oid_cell(x1), list_make_oid_cell(x2), \
					list_make_oid_cell(x3), list_make_oid_cell(x4))
#define list_make5_oid(x1,x2,x3,x4,x5) \
	list_make5_impl(T_OidList, list_make_oid_cell(x1), list_make_oid_cell(x2), \
					list_make_oid_cell(x3), list_make_oid_cell(x4), \
					list_make_oid_cell(x5))

#define list_make1_xid(x1) \
	list_make1_impl(T_XidList, list_make_xid_cell(x1))
#define list_make2_xid(x1,x2) \
	list_make2_impl(T_XidList, list_make_xid_cell(x1), list_make_xid_cell(x2))
#define list_make3_xid(x1,x2,x3) \
	list_make3_impl(T_XidList, list_make_xid_cell(x1), list_make_xid_cell(x2), \
					list_make_xid_cell(x3))
#define list_make4_xid(x1,x2,x3,x4) \
	list_make4_impl(T_XidList, list_make_xid_cell(x1), list_make_xid_cell(x2), \
					list_make_xid_cell(x3), list_make_xid_cell(x4))
#define list_make5_xid(x1,x2,x3,x4,x5) \
	list_make5_impl(T_XidList, list_make_xid_cell(x1), list_make_xid_cell(x2), \
					list_make_xid_cell(x3), list_make_xid_cell(x4), \
					list_make_xid_cell(x5))

/*
 * 定位列表第 n 个单元(从 0 计)。
 * 若无此单元则断言失败。
 */
static inline ListCell *
list_nth_cell(const List *list, int n)
{
	Assert(list != NIL);
	Assert(n >= 0 && n < list->length);
	return &list->elements[n];
}

/*
 * 返回非 NIL List 的末单元。
 */
static inline ListCell *
list_last_cell(const List *list)
{
	Assert(list != NIL);
	return &list->elements[list->length - 1];
}

/*
 * 返回指定列表第 n 个元素中的指针值。
 * (列表元素从 0 起。)
 */
static inline void *
list_nth(const List *list, int n)
{
	Assert(IsA(list, List));
	return lfirst(list_nth_cell(list, n));
}

/*
 * 返回指定列表第 n 个元素中的整数值。
 * 见上。
 */
static inline int
list_nth_int(const List *list, int n)
{
	Assert(IsA(list, IntList));
	return lfirst_int(list_nth_cell(list, n));
}

/*
 * Return the OID value contained in the n'th element of the specified
 * list.
 */
static inline Oid
list_nth_oid(const List *list, int n)
{
	Assert(IsA(list, OidList));
	return lfirst_oid(list_nth_cell(list, n));
}

#define list_nth_node(type,list,n)	castNode(type, list_nth(list, n))

/*
 * 取给定 ListCell 在给定 List 中下标(从 0 起)。
 */
static inline int
list_cell_number(const List *l, const ListCell *c)
{
	Assert(c >= &l->elements[0] && c < &l->elements[l->length]);
	return c - l->elements;
}

/*
 * 取列表 l 中单元 c 之后下一单元地址,无则 NULL。
 */
static inline ListCell *
lnext(const List *l, const ListCell *c)
{
	Assert(c >= &l->elements[0] && c < &l->elements[l->length]);
	c++;
	if (c < &l->elements[l->length])
		return (ListCell *) c;
	else
		return NULL;
}

/*
 * foreach -
 *	  遍历列表的便利宏
 *
 * "cell" 须为 "ListCell *" 变量名;会依次指向
 * 每个 List 元素。正常退出循环后 cell 为 NULL,
 * 但提前 break 会让它停在当前 List
 * 元素上。
 *
 * 迭代中途改动 List 对象须谨慎。
 * 当前语义是每轮检查相继的列表下标,故插入或
 * 删除元素可能导致元素被重访或意外跳过。
 * 早先 foreach() 实现行为不同。但向 List
 * 追加元素(或一般地插在当前元素之后)是安全的;
 * 这些新元素保证会被访问。
 * 也可用 foreach_delete_current() 删除当前元素。
 * 但:这两种动作都会使本轮余下时间里
 * "cell" 指针失效。
 * 见上。
 */
#define foreach(cell, lst)	\
	for (ForEachState cell##__state = {(lst), 0}; \
		 (cell##__state.l != NIL && \
		  cell##__state.i < cell##__state.l->length) ? \
		 (cell = &cell##__state.l->elements[cell##__state.i], true) : \
		 (cell = NULL, false); \
		 cell##__state.i++)

/*
 * foreach_delete_current -
 *	  从包围的 foreach() 循环关联的 List 中删除
 *	  当前列表元素,返回新 List 指针。
 *
 * 等价于 list_delete_cell(),但还会调整 foreach
 * 循环状态以免漏元素。勿以其他方式从活动
 * foreach 循环的列表中删元素!
 */
#define foreach_delete_current(lst, cell)	\
	(cell##__state.i--, \
	 (List *) (cell##__state.l = list_delete_cell(lst, cell)))

/*
 * foreach_current_index -
 *	  取包围的 foreach() 循环当前元素的从零下标;
 *	  传入 "ListCell *" 迭代变量名。
 *
 * 在 foreach_delete_current() 之后慎用;本轮余下
 * 时间值会不同步。而且刚删了当前元素,
 * 该值也几乎无意义。
 */
#define foreach_current_index(cell)  (cell##__state.i)

/*
 * for_each_from -
 *	  类似 foreach(),但从第 N 个(从零计)列表元素
 *	  开始,不必是第一个。
 *
 * N 可超过列表长度,但不可为负。
 *
 * foreach() 的注意事项同样适用。
 */
#define for_each_from(cell, lst, N)	\
	for (ForEachState cell##__state = for_each_from_setup(lst, N); \
		 (cell##__state.l != NIL && \
		  cell##__state.i < cell##__state.l->length) ? \
		 (cell = &cell##__state.l->elements[cell##__state.i], true) : \
		 (cell = NULL, false); \
		 cell##__state.i++)

static inline ForEachState
for_each_from_setup(const List *lst, int N)
{
	ForEachState r = {lst, N};

	Assert(N >= 0);
	return r;
}

/*
 * for_each_cell -
 *	  从指定单元起遍历列表的便利宏
 *	见上。
 *
 * foreach() 的注意事项同样适用。
 */
#define for_each_cell(cell, lst, initcell)	\
	for (ForEachState cell##__state = for_each_cell_setup(lst, initcell); \
		 (cell##__state.l != NIL && \
		  cell##__state.i < cell##__state.l->length) ? \
		 (cell = &cell##__state.l->elements[cell##__state.i], true) : \
		 (cell = NULL, false); \
		 cell##__state.i++)

static inline ForEachState
for_each_cell_setup(const List *lst, const ListCell *initcell)
{
	ForEachState r = {lst,
	initcell ? list_cell_number(lst, initcell) : list_length(lst)};

	return r;
}

/*
 * forboth -
 *	  同时推进两个链表的便利宏。
 *	  本宏同步遍历两列表,任一耗尽即停。
 *	  视调用点要求,也可断言两列表长度相等。
 *	  (但若不等,有些调用方依赖此处定义的结束
 *	  单元值分别是 NULL 或非 NULL;勿试图优化。)
 *	见上。
 *	见上。
 *
 * foreach() 的注意事项同样适用。
 */
#define forboth(cell1, list1, cell2, list2)							\
	for (ForBothState cell1##__state = {(list1), (list2), 0}; \
		 multi_for_advance_cell(cell1, cell1##__state, l1, i), \
		 multi_for_advance_cell(cell2, cell1##__state, l2, i), \
		 (cell1 != NULL && cell2 != NULL); \
		 cell1##__state.i++)

#define multi_for_advance_cell(cell, state, l, i) \
	(cell = (state.l != NIL && state.i < state.l->length) ? \
	 &state.l->elements[state.i] : NULL)

/*
 * for_both_cell -
 *	  从各列表指定单元起同步遍历两列表的便利宏。
 *	  任一列表耗尽即停。视调用点要求,也可断言
 *	  两列表等长,且 initcell1 与 initcell2 在各自
 *	  列表中处于相同位置。
 *	见上。
 *	见上。
 *
 * foreach() 的注意事项同样适用。
 */
#define for_both_cell(cell1, list1, initcell1, cell2, list2, initcell2)	\
	for (ForBothCellState cell1##__state = \
			 for_both_cell_setup(list1, initcell1, list2, initcell2); \
		 multi_for_advance_cell(cell1, cell1##__state, l1, i1), \
		 multi_for_advance_cell(cell2, cell1##__state, l2, i2), \
		 (cell1 != NULL && cell2 != NULL); \
		 cell1##__state.i1++, cell1##__state.i2++)

static inline ForBothCellState
for_both_cell_setup(const List *list1, const ListCell *initcell1,
					const List *list2, const ListCell *initcell2)
{
	ForBothCellState r = {list1, list2,
		initcell1 ? list_cell_number(list1, initcell1) : list_length(list1),
	initcell2 ? list_cell_number(list2, initcell2) : list_length(list2)};

	return r;
}

/*
 * forthree -
 *	  对三个列表同上
 */
#define forthree(cell1, list1, cell2, list2, cell3, list3) \
	for (ForThreeState cell1##__state = {(list1), (list2), (list3), 0}; \
		 multi_for_advance_cell(cell1, cell1##__state, l1, i), \
		 multi_for_advance_cell(cell2, cell1##__state, l2, i), \
		 multi_for_advance_cell(cell3, cell1##__state, l3, i), \
		 (cell1 != NULL && cell2 != NULL && cell3 != NULL); \
		 cell1##__state.i++)

/*
 * forfour -
 *	  对四个列表同上
 */
#define forfour(cell1, list1, cell2, list2, cell3, list3, cell4, list4) \
	for (ForFourState cell1##__state = {(list1), (list2), (list3), (list4), 0}; \
		 multi_for_advance_cell(cell1, cell1##__state, l1, i), \
		 multi_for_advance_cell(cell2, cell1##__state, l2, i), \
		 multi_for_advance_cell(cell3, cell1##__state, l3, i), \
		 multi_for_advance_cell(cell4, cell1##__state, l4, i), \
		 (cell1 != NULL && cell2 != NULL && cell3 != NULL && cell4 != NULL); \
		 cell1##__state.i++)

/*
 * forfive -
 *	  对五个列表同上
 */
#define forfive(cell1, list1, cell2, list2, cell3, list3, cell4, list4, cell5, list5) \
	for (ForFiveState cell1##__state = {(list1), (list2), (list3), (list4), (list5), 0}; \
		 multi_for_advance_cell(cell1, cell1##__state, l1, i), \
		 multi_for_advance_cell(cell2, cell1##__state, l2, i), \
		 multi_for_advance_cell(cell3, cell1##__state, l3, i), \
		 multi_for_advance_cell(cell4, cell1##__state, l4, i), \
		 multi_for_advance_cell(cell5, cell1##__state, l5, i), \
		 (cell1 != NULL && cell2 != NULL && cell3 != NULL && \
		  cell4 != NULL && cell5 != NULL); \
		 cell1##__state.i++)

/* src/backend/nodes/list.c 中的函数 */

extern List *list_make1_impl(NodeTag t, ListCell datum1);
extern List *list_make2_impl(NodeTag t, ListCell datum1, ListCell datum2);
extern List *list_make3_impl(NodeTag t, ListCell datum1, ListCell datum2,
							 ListCell datum3);
extern List *list_make4_impl(NodeTag t, ListCell datum1, ListCell datum2,
							 ListCell datum3, ListCell datum4);
extern List *list_make5_impl(NodeTag t, ListCell datum1, ListCell datum2,
							 ListCell datum3, ListCell datum4,
							 ListCell datum5);

extern pg_nodiscard List *lappend(List *list, void *datum);
extern pg_nodiscard List *lappend_int(List *list, int datum);
extern pg_nodiscard List *lappend_oid(List *list, Oid datum);
extern pg_nodiscard List *lappend_xid(List *list, TransactionId datum);

extern pg_nodiscard List *list_insert_nth(List *list, int pos, void *datum);
extern pg_nodiscard List *list_insert_nth_int(List *list, int pos, int datum);
extern pg_nodiscard List *list_insert_nth_oid(List *list, int pos, Oid datum);

extern pg_nodiscard List *lcons(void *datum, List *list);
extern pg_nodiscard List *lcons_int(int datum, List *list);
extern pg_nodiscard List *lcons_oid(Oid datum, List *list);

extern pg_nodiscard List *list_concat(List *list1, const List *list2);
extern pg_nodiscard List *list_concat_copy(const List *list1, const List *list2);

extern pg_nodiscard List *list_truncate(List *list, int new_size);

extern bool list_member(const List *list, const void *datum);
extern bool list_member_ptr(const List *list, const void *datum);
extern bool list_member_int(const List *list, int datum);
extern bool list_member_oid(const List *list, Oid datum);
extern bool list_member_xid(const List *list, TransactionId datum);

extern pg_nodiscard List *list_delete(List *list, void *datum);
extern pg_nodiscard List *list_delete_ptr(List *list, void *datum);
extern pg_nodiscard List *list_delete_int(List *list, int datum);
extern pg_nodiscard List *list_delete_oid(List *list, Oid datum);
extern pg_nodiscard List *list_delete_first(List *list);
extern pg_nodiscard List *list_delete_last(List *list);
extern pg_nodiscard List *list_delete_first_n(List *list, int n);
extern pg_nodiscard List *list_delete_nth_cell(List *list, int n);
extern pg_nodiscard List *list_delete_cell(List *list, ListCell *cell);

extern List *list_union(const List *list1, const List *list2);
extern List *list_union_ptr(const List *list1, const List *list2);
extern List *list_union_int(const List *list1, const List *list2);
extern List *list_union_oid(const List *list1, const List *list2);

extern List *list_intersection(const List *list1, const List *list2);
extern List *list_intersection_int(const List *list1, const List *list2);

/* 目前无需 list_intersection_ptr 等 */

extern List *list_difference(const List *list1, const List *list2);
extern List *list_difference_ptr(const List *list1, const List *list2);
extern List *list_difference_int(const List *list1, const List *list2);
extern List *list_difference_oid(const List *list1, const List *list2);

extern pg_nodiscard List *list_append_unique(List *list, void *datum);
extern pg_nodiscard List *list_append_unique_ptr(List *list, void *datum);
extern pg_nodiscard List *list_append_unique_int(List *list, int datum);
extern pg_nodiscard List *list_append_unique_oid(List *list, Oid datum);

extern pg_nodiscard List *list_concat_unique(List *list1, const List *list2);
extern pg_nodiscard List *list_concat_unique_ptr(List *list1, const List *list2);
extern pg_nodiscard List *list_concat_unique_int(List *list1, const List *list2);
extern pg_nodiscard List *list_concat_unique_oid(List *list1, const List *list2);

extern void list_deduplicate_oid(List *list);

extern void list_free(List *list);
extern void list_free_deep(List *list);

extern pg_nodiscard List *list_copy(const List *oldlist);
extern pg_nodiscard List *list_copy_head(const List *oldlist, int len);
extern pg_nodiscard List *list_copy_tail(const List *oldlist, int nskip);
extern pg_nodiscard List *list_copy_deep(const List *oldlist);

typedef int (*list_sort_comparator) (const ListCell *a, const ListCell *b);
extern void list_sort(List *list, list_sort_comparator cmp);

extern int	list_int_cmp(const ListCell *p1, const ListCell *p2);
extern int	list_oid_cmp(const ListCell *p1, const ListCell *p2);

#endif							/* PG_LIST_H */
