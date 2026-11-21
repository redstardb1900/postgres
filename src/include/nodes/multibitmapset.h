/*-------------------------------------------------------------------------
 *
 * multibitmapset.h
 *	Bitmapset 的列表
 *
 * 集合元可用两小整数标识时,multibitmapset 有用；
 * 例如查询中一组 Var 的 varno 与 varattno。实现为
 * Bitmapset 的 List，故空集可用 NIL 表示。（但与
 * Bitmapset 一样，这不是唯一允许的表示。）
 * as with Bitmapsets, that's not the only allowed representation.)
 * List 元素的从零起下标是第一标识值，
 * 该 Bitmapset 内位的（也从零起）下标是
 * 第二标识值。不要求各
 * Bitmapset 大小相同。
 *
 * 对 multibitmapset 的可用操作意在与 bitmapset 并行，
 * 例如并与交。目前只实现了一小部分；
 * 需要时再加。
 *
 *
 * Copyright (c) 2022-2023, PostgreSQL Global Development Group
 *
 * src/include/nodes/multibitmapset.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MULTIBITMAPSET_H
#define MULTIBITMAPSET_H

#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"

extern List *mbms_add_member(List *a, int listidx, int bitidx);
extern List *mbms_add_members(List *a, const List *b);
extern List *mbms_int_members(List *a, const List *b);
extern bool mbms_is_member(int listidx, int bitidx, const List *a);
extern Bitmapset *mbms_overlap_sets(const List *a, const List *b);

#endif							/* MULTIBITMAPSET_H */
