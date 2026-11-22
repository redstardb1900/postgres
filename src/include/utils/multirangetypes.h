/*-------------------------------------------------------------------------
 *
 * multirangetypes.h
 *	  Postgres 多重范围类型的声明。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/multirangetypes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MULTIRANGETYPES_H
#define MULTIRANGETYPES_H

#include "utils/rangetypes.h"
#include "utils/typcache.h"


/*
 * 多重范围是 varlena 对象，须遵守 varlena 约定：
 * 对象首个 int32 含总字节大小。访问时务必用
 * VARSIZE() 与 SET_VARSIZE()！
 */
typedef struct
{
	int32		vl_len_;		/* varlena 头（勿直接触碰！） */
	Oid			multirangetypid;	/* 多重范围类型的 OID */
	uint32		rangeCount;		/* 范围个数 */

	/*
	 * 计数之后是范围对象本身，为 ShortRangeType
	 * 结构。注意范围也是 varlena，取决于是否有上/
	 * 下界，且其基类型甚至也可是 varlena。故无法
	 * 真正索引进此列表。
	 */
} MultirangeType;

/* 优先用这些宏，勿直接访问这些字段 */
#define MultirangeTypeGetOid(mr)	((mr)->multirangetypid)
#define MultirangeIsEmpty(mr)  ((mr)->rangeCount == 0)

/*
 * 多重范围类型对象的 fmgr 函数
 */
static inline MultirangeType *
DatumGetMultirangeTypeP(Datum X)
{
	return (MultirangeType *) PG_DETOAST_DATUM(X);
}

static inline MultirangeType *
DatumGetMultirangeTypePCopy(Datum X)
{
	return (MultirangeType *) PG_DETOAST_DATUM_COPY(X);
}

static inline Datum
MultirangeTypePGetDatum(const MultirangeType *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_MULTIRANGE_P(n)		DatumGetMultirangeTypeP(PG_GETARG_DATUM(n))
#define PG_GETARG_MULTIRANGE_P_COPY(n)	DatumGetMultirangeTypePCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_MULTIRANGE_P(x)		return MultirangeTypePGetDatum(x)

/*
 * multirangetypes.c 中函数的原型
 */

/* 上述的内部版本 */
extern bool multirange_eq_internal(TypeCacheEntry *rangetyp,
								   const MultirangeType *mr1,
								   const MultirangeType *mr2);
extern bool multirange_ne_internal(TypeCacheEntry *rangetyp,
								   const MultirangeType *mr1,
								   const MultirangeType *mr2);
extern bool multirange_contains_elem_internal(TypeCacheEntry *rangetyp,
											  const MultirangeType *mr,
											  Datum val);
extern bool multirange_contains_range_internal(TypeCacheEntry *rangetyp,
											   const MultirangeType *mr,
											   const RangeType *r);
extern bool range_contains_multirange_internal(TypeCacheEntry *rangetyp,
											   const RangeType *r,
											   const MultirangeType *mr);
extern bool multirange_contains_multirange_internal(TypeCacheEntry *rangetyp,
													const MultirangeType *mr1,
													const MultirangeType *mr2);
extern bool range_overlaps_multirange_internal(TypeCacheEntry *rangetyp,
											   const RangeType *r,
											   const MultirangeType *mr);
extern bool multirange_overlaps_multirange_internal(TypeCacheEntry *rangetyp,
													const MultirangeType *mr1,
													const MultirangeType *mr2);
extern bool range_overleft_multirange_internal(TypeCacheEntry *rangetyp,
											   const RangeType *r,
											   const MultirangeType *mr);
extern bool range_overright_multirange_internal(TypeCacheEntry *rangetyp,
												const RangeType *r,
												const MultirangeType *mr);
extern bool range_before_multirange_internal(TypeCacheEntry *rangetyp,
											 const RangeType *r,
											 const MultirangeType *mr);
extern bool range_after_multirange_internal(TypeCacheEntry *rangetyp,
											const RangeType *r,
											const MultirangeType *mr);
extern bool range_adjacent_multirange_internal(TypeCacheEntry *rangetyp,
											   const RangeType *r,
											   const MultirangeType *mr);
extern bool multirange_before_multirange_internal(TypeCacheEntry *rangetyp,
												  const MultirangeType *mr1,
												  const MultirangeType *mr2);
extern MultirangeType *multirange_minus_internal(Oid mltrngtypoid,
												 TypeCacheEntry *rangetyp,
												 int32 range_count1,
												 RangeType **ranges1,
												 int32 range_count2,
												 RangeType **ranges2);
extern MultirangeType *multirange_intersect_internal(Oid mltrngtypoid,
													 TypeCacheEntry *rangetyp,
													 int32 range_count1,
													 RangeType **ranges1,
													 int32 range_count2,
													 RangeType **ranges2);

/* 各类支持函数 */
extern TypeCacheEntry *multirange_get_typcache(FunctionCallInfo fcinfo,
											   Oid mltrngtypid);
extern void multirange_deserialize(TypeCacheEntry *rangetyp,
								   const MultirangeType *multirange,
								   int32 *range_count,
								   RangeType ***ranges);
extern MultirangeType *make_multirange(Oid mltrngtypoid,
									   TypeCacheEntry *rangetyp,
									   int32 range_count, RangeType **ranges);
extern MultirangeType *make_empty_multirange(Oid mltrngtypoid,
											 TypeCacheEntry *rangetyp);
extern void multirange_get_bounds(TypeCacheEntry *rangetyp,
								  const MultirangeType *multirange,
								  uint32 i,
								  RangeBound *lower, RangeBound *upper);
extern RangeType *multirange_get_range(TypeCacheEntry *rangetyp,
									   const MultirangeType *multirange, int i);
extern RangeType *multirange_get_union_range(TypeCacheEntry *rangetyp,
											 const MultirangeType *mr);

#endif							/* MULTIRANGETYPES_H */
