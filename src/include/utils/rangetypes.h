/*-------------------------------------------------------------------------
 *
 * rangetypes.h
 *	  Postgres 范围类型的声明。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/rangetypes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RANGETYPES_H
#define RANGETYPES_H

#include "utils/typcache.h"


/*
 * 范围是 varlena 对象，须遵守 varlena 约定：对象
 * 首个 int32 含总字节大小。访问时务必用 VARSIZE()
 * 与 SET_VARSIZE()！
 */
typedef struct
{
	int32		vl_len_;		/* varlena 头（勿直接触碰！） */
	Oid			rangetypid;		/* 范围类型的 OID */
	/* OID 之后是零到两个边界值，再跟标志字节 */
} RangeType;

#define RANGE_EMPTY_LITERAL "empty"

/* 优先用本宏，勿直接取 rangetypid 字段 */
#define RangeTypeGetOid(r)	((r)->rangetypid)

/* 范围的标志字节含这些位： */
#define RANGE_EMPTY			0x01	/* 范围为空 */
#define RANGE_LB_INC		0x02	/* 下界含端点 */
#define RANGE_UB_INC		0x04	/* 上界含端点 */
#define RANGE_LB_INF		0x08	/* 下界为 -infinity */
#define RANGE_UB_INF		0x10	/* 上界为 +infinity */
#define RANGE_LB_NULL		0x20	/* 下界为 null（未用） */
#define RANGE_UB_NULL		0x40	/* 上界为 null（未用） */
#define RANGE_CONTAIN_EMPTY 0x80	/* 标记其子树含某些空范围的
									 * GiST 内页条目 */

#define RANGE_HAS_LBOUND(flags) (!((flags) & (RANGE_EMPTY | \
											  RANGE_LB_NULL | \
											  RANGE_LB_INF)))

#define RANGE_HAS_UBOUND(flags) (!((flags) & (RANGE_EMPTY | \
											  RANGE_UB_NULL | \
											  RANGE_UB_INF)))

#define RangeIsEmpty(r)  ((range_get_flags(r) & RANGE_EMPTY) != 0)
#define RangeIsOrContainsEmpty(r)  \
	((range_get_flags(r) & (RANGE_EMPTY | RANGE_CONTAIN_EMPTY)) != 0)


/* 范围任一侧边界的内部表示（非盘上格式） */
typedef struct
{
	Datum		val;			/* 边界值（若有） */
	bool		infinite;		/* 边界为 ±infinity */
	bool		inclusive;		/* 边界含端点（相对开） */
	bool		lower;			/* 这是下界（相对上界） */
} RangeBound;

/*
 * 范围类型对象的 fmgr 函数
 */
static inline RangeType *
DatumGetRangeTypeP(Datum X)
{
	return (RangeType *) PG_DETOAST_DATUM(X);
}

static inline RangeType *
DatumGetRangeTypePCopy(Datum X)
{
	return (RangeType *) PG_DETOAST_DATUM_COPY(X);
}

static inline Datum
RangeTypePGetDatum(const RangeType *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_RANGE_P(n)		DatumGetRangeTypeP(PG_GETARG_DATUM(n))
#define PG_GETARG_RANGE_P_COPY(n)	DatumGetRangeTypePCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_RANGE_P(x)		return RangeTypePGetDatum(x)

/* GiST 与 SP-GiST 范围操作符类所用的策略号 */
/* 编号选为与既有用法中的运算符名对应 */
#define RANGESTRAT_BEFORE				RTLeftStrategyNumber
#define RANGESTRAT_OVERLEFT				RTOverLeftStrategyNumber
#define RANGESTRAT_OVERLAPS				RTOverlapStrategyNumber
#define RANGESTRAT_OVERRIGHT			RTOverRightStrategyNumber
#define RANGESTRAT_AFTER				RTRightStrategyNumber
#define RANGESTRAT_ADJACENT				RTSameStrategyNumber
#define RANGESTRAT_CONTAINS				RTContainsStrategyNumber
#define RANGESTRAT_CONTAINED_BY			RTContainedByStrategyNumber
#define RANGESTRAT_CONTAINS_ELEM		RTContainsElemStrategyNumber
#define RANGESTRAT_EQ					RTEqualStrategyNumber

/*
 * rangetypes.c 中函数的原型
 */

extern bool range_contains_elem_internal(TypeCacheEntry *typcache, const RangeType *r, Datum val);

/* 上述的内部版本 */
extern bool range_eq_internal(TypeCacheEntry *typcache, const RangeType *r1,
							  const RangeType *r2);
extern bool range_ne_internal(TypeCacheEntry *typcache, const RangeType *r1,
							  const RangeType *r2);
extern bool range_contains_internal(TypeCacheEntry *typcache, const RangeType *r1,
									const RangeType *r2);
extern bool range_contained_by_internal(TypeCacheEntry *typcache, const RangeType *r1,
										const RangeType *r2);
extern bool range_before_internal(TypeCacheEntry *typcache, const RangeType *r1,
								  const RangeType *r2);
extern bool range_after_internal(TypeCacheEntry *typcache, const RangeType *r1,
								 const RangeType *r2);
extern bool range_adjacent_internal(TypeCacheEntry *typcache, const RangeType *r1,
									const RangeType *r2);
extern bool range_overlaps_internal(TypeCacheEntry *typcache, const RangeType *r1,
									const RangeType *r2);
extern bool range_overleft_internal(TypeCacheEntry *typcache, const RangeType *r1,
									const RangeType *r2);
extern bool range_overright_internal(TypeCacheEntry *typcache, const RangeType *r1,
									 const RangeType *r2);
extern RangeType *range_union_internal(TypeCacheEntry *typcache, RangeType *r1,
									   RangeType *r2, bool strict);
extern RangeType *range_minus_internal(TypeCacheEntry *typcache, RangeType *r1,
									   RangeType *r2);
extern RangeType *range_intersect_internal(TypeCacheEntry *typcache, const RangeType *r1,
										   const RangeType *r2);

/* 各类支持函数 */
extern TypeCacheEntry *range_get_typcache(FunctionCallInfo fcinfo,
										  Oid rngtypid);
extern RangeType *range_serialize(TypeCacheEntry *typcache, RangeBound *lower,
								  RangeBound *upper, bool empty,
								  struct Node *escontext);
extern void range_deserialize(TypeCacheEntry *typcache, const RangeType *range,
							  RangeBound *lower, RangeBound *upper,
							  bool *empty);
extern char range_get_flags(const RangeType *range);
extern void range_set_contain_empty(RangeType *range);
extern RangeType *make_range(TypeCacheEntry *typcache, RangeBound *lower,
							 RangeBound *upper, bool empty,
							 struct Node *escontext);
extern int	range_cmp_bounds(TypeCacheEntry *typcache, const RangeBound *b1,
							 const RangeBound *b2);
extern int	range_cmp_bound_values(TypeCacheEntry *typcache, const RangeBound *b1,
								   const RangeBound *b2);
extern int	range_compare(const void *key1, const void *key2, void *arg);
extern bool bounds_adjacent(TypeCacheEntry *typcache, RangeBound boundA,
							RangeBound boundB);
extern RangeType *make_empty_range(TypeCacheEntry *typcache);
extern bool range_split_internal(TypeCacheEntry *typcache, const RangeType *r1,
								 const RangeType *r2, RangeType **output1,
								 RangeType **output2);

#endif							/* RANGETYPES_H */
