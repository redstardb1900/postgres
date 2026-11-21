/*-------------------------------------------------------------------------
 *
 * bitmapset.h
 *	PostgreSQL 通用位图集合包
 *
 * 位图集可表示任意非负整数集,主要面向最大值不大
 * （大约至多数百）的集合。按约定空集总是用
 * NULL 指针
 * 表示。
 *
 *
 * Copyright (c) 2003-2023, PostgreSQL Global Development Group
 *
 * src/include/nodes/bitmapset.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BITMAPSET_H
#define BITMAPSET_H

#include "nodes/nodes.h"

/*
 * 前向声明，以免包含 pg_list.h
 */
struct List;

/*
 * 数据表示
 *
 * 更大位图字一般性能更好，只要不宽过处理器能高效
 * 处理的宽度。指针够大则用 64 位字，否则用
 * 32 位字。
 */
#if SIZEOF_VOID_P >= 8

#define BITS_PER_BITMAPWORD 64
typedef uint64 bitmapword;		/* 须为无符号类型 */
typedef int64 signedbitmapword; /* 须为匹配的有符号类型 */

#else

#define BITS_PER_BITMAPWORD 32
typedef uint32 bitmapword;		/* 须为无符号类型 */
typedef int32 signedbitmapword; /* 须为匹配的有符号类型 */

#endif

typedef struct Bitmapset
{
	pg_node_attr(custom_copy_equal, special_read_write, no_query_jumble)

	NodeTag		type;
	int			nwords;			/* 数组中的字数 */
	bitmapword	words[FLEXIBLE_ARRAY_MEMBER];	/* 实为 [nwords] */
} Bitmapset;


/* bms_subset_compare 的结果 */
typedef enum
{
	BMS_EQUAL,					/* 两集相等 */
	BMS_SUBSET1,				/* 第一集是第二集的子集 */
	BMS_SUBSET2,				/* 第二集是第一集的子集 */
	BMS_DIFFERENT				/* 互不为子集 */
} BMS_Comparison;

/* bms_membership 的结果 */
typedef enum
{
	BMS_EMPTY_SET,				/* 0 成员 */
	BMS_SINGLETON,				/* 1 成员 */
	BMS_MULTIPLE				/* >1 成员 */
} BMS_Membership;


/*
 * nodes/bitmapset.c 中的函数原型
 */

extern Bitmapset *bms_copy(const Bitmapset *a);
extern bool bms_equal(const Bitmapset *a, const Bitmapset *b);
extern int	bms_compare(const Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_make_singleton(int x);
extern void bms_free(Bitmapset *a);

extern Bitmapset *bms_union(const Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_intersect(const Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_difference(const Bitmapset *a, const Bitmapset *b);
extern bool bms_is_subset(const Bitmapset *a, const Bitmapset *b);
extern BMS_Comparison bms_subset_compare(const Bitmapset *a, const Bitmapset *b);
extern bool bms_is_member(int x, const Bitmapset *a);
extern int	bms_member_index(Bitmapset *a, int x);
extern bool bms_overlap(const Bitmapset *a, const Bitmapset *b);
extern bool bms_overlap_list(const Bitmapset *a, const struct List *b);
extern bool bms_nonempty_difference(const Bitmapset *a, const Bitmapset *b);
extern int	bms_singleton_member(const Bitmapset *a);
extern bool bms_get_singleton_member(const Bitmapset *a, int *member);
extern int	bms_num_members(const Bitmapset *a);

/* 不需确切成员数时的优化测试: */
extern BMS_Membership bms_membership(const Bitmapset *a);

/* 现仅允许用 NULL 表示空 bitmapset */
#define bms_is_empty(a)  ((a) == NULL)

/* 这些例程回收(修改或释放)其非常量输入: */

extern Bitmapset *bms_add_member(Bitmapset *a, int x);
extern Bitmapset *bms_del_member(Bitmapset *a, int x);
extern Bitmapset *bms_add_members(Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_add_range(Bitmapset *a, int lower, int upper);
extern Bitmapset *bms_int_members(Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_del_members(Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_join(Bitmapset *a, Bitmapset *b);

/* 支持遍历集合中的整数元素: */
extern int	bms_next_member(const Bitmapset *a, int prevbit);
extern int	bms_prev_member(const Bitmapset *a, int prevbit);

/* 支持以 Bitmapset 为键的哈希表: */
extern uint32 bms_hash_value(const Bitmapset *a);
extern uint32 bitmap_hash(const void *key, Size keysize);
extern int	bitmap_match(const void *key1, const void *key2, Size keysize);

#endif							/* BITMAPSET_H */
