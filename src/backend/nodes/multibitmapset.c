/*-------------------------------------------------------------------------
 *
 * multibitmapset.c
 *	  Lists of Bitmapsets
 *
 * A multibitmapset is useful in situations where members of a set can
 * be identified by two small integers; for example, varno and varattno
 * of a group of Vars within a query.  The implementation is a List of
 * Bitmapsets, so that the empty set can be represented by NIL.  (But,
 * as with Bitmapsets, that's not the only allowed representation.)
 * The zero-based index of a List element is the first identifying value,
 * and the (also zero-based) index of a bit within that Bitmapset is
 * the second identifying value.  There is no expectation that the
 * Bitmapsets should all be the same size.
 *
 * The available operations on multibitmapsets are intended to parallel
 * those on bitmapsets, for example union and intersection.  So far only
 * a small fraction of that has been built out; we'll add more as needed.
 *
 *
 * Copyright (c) 2022-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/nodes/multibitmapset.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/multibitmapset.h"


/*
 * mbms_add_member
 *		向多位图集加入一个新成员。
 *
 * 新成员由 "listidx" 标识，即它应当进入的
 * List 元素的从零开始的下标，以及 "bitidx"，指定要在其中
 * 设置的位号。
 *
 * 这与 bms_add_member 类似，但是针对多位图集。
 */
List *
mbms_add_member(List *a, int listidx, int bitidx)
{
	Bitmapset  *bms;
	ListCell   *lc;

	if (listidx < 0 || bitidx < 0)
		elog(ERROR, "negative multibitmapset member index not allowed");
	/* 按需要加入空元素 */
	while (list_length(a) <= listidx)
		a = lappend(a, NULL);
	/* 更新目标元素 */
	lc = list_nth_cell(a, listidx);
	bms = lfirst_node(Bitmapset, lc);
	bms = bms_add_member(bms, bitidx);
	lfirst(lc) = bms;
	return a;
}

/*
 * mbms_add_members
 *		把集合 b 的全部成员加入集合 a。
 *
 * 这是 UNION 操作，但左输入被就地修改。
 *
 * 这与 bms_add_members 类似，但是针对多位图集。
 */
List *
mbms_add_members(List *a, const List *b)
{
	ListCell   *lca,
			   *lcb;

	/* 按需要给 a 加入空元素 */
	while (list_length(a) < list_length(b))
		a = lappend(a, NULL);
	/* forboth 会在较短列表的末尾停下，这没问题 */
	forboth(lca, a, lcb, b)
	{
		Bitmapset  *bmsa = lfirst_node(Bitmapset, lca);
		const Bitmapset *bmsb = lfirst_node(Bitmapset, lcb);

		bmsa = bms_add_members(bmsa, bmsb);
		lfirst(lca) = bmsa;
	}
	return a;
}

/*
 * mbms_int_members
 *		把集合 a 缩减为它与集合 b 的交集。
 *
 * 这是 INTERSECT 操作，但左输入被就地修改。
 *
 * 这与 bms_int_members 类似，但是针对多位图集。
 */
List *
mbms_int_members(List *a, const List *b)
{
	ListCell   *lca,
			   *lcb;

	/* 去掉 a 中不再有用的任何元素 */
	a = list_truncate(a, list_length(b));
	/* forboth 会在较短列表的末尾停下，这没问题 */
	forboth(lca, a, lcb, b)
	{
		Bitmapset  *bmsa = lfirst_node(Bitmapset, lca);
		const Bitmapset *bmsb = lfirst_node(Bitmapset, lcb);

		bmsa = bms_int_members(bmsa, bmsb);
		lfirst(lca) = bmsa;
	}
	return a;
}

/*
 * mbms_is_member
 *		listidx/bitidx 是 A 的成员吗？
 *
 * 这与 bms_is_member 类似，但是针对多位图集。
 */
bool
mbms_is_member(int listidx, int bitidx, const List *a)
{
	const Bitmapset *bms;

	/* XXX：对负下标直接返回 false 是否更好？ */
	if (listidx < 0 || bitidx < 0)
		elog(ERROR, "negative multibitmapset member index not allowed");
	if (listidx >= list_length(a))
		return false;
	bms = list_nth_node(Bitmapset, a, listidx);
	return bms_is_member(bitidx, bms);
}

/*
 * mbms_overlap_sets
 *		找出 a 和 b 中有共同成员的位图集。
 *
 * 结果是这些重叠位图集的列表下标组成的位图集。
 */
Bitmapset *
mbms_overlap_sets(const List *a, const List *b)
{
	Bitmapset  *result = NULL;
	ListCell   *lca,
			   *lcb;

	/* forboth 会在较短列表的末尾停下，这没问题 */
	forboth(lca, a, lcb, b)
	{
		const Bitmapset *bmsa = lfirst_node(Bitmapset, lca);
		const Bitmapset *bmsb = lfirst_node(Bitmapset, lcb);

		if (bms_overlap(bmsa, bmsb))
			result = bms_add_member(result, foreach_current_index(lca));
	}
	return result;
}
