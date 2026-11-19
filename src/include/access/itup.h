/*-------------------------------------------------------------------------
 *
 * itup.h
 *	  POSTGRES 索引元组定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/itup.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITUP_H
#define ITUP_H

#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"

/*
 * 索引元组头结构
 *
 * 所有索引元组以 IndexTupleData 开头。若设置 HasNulls 位，
 * 其后紧跟 IndexAttributeBitMapData。索引属性值
 * 自 MAXALIGN 边界起依次存放。
 *
 * 注意，为位图分配的空间不随属性个数而变；
 * 这是因为头部没有空间存放属性个数。
 * 鉴于 MAXALIGN 约束，无论如何都没有可节省的空间，
 * INDEX_MAX_KEYS 取常见值时亦然。
 */

typedef struct IndexTupleData
{
	ItemPointerData t_tid;		/* 指向堆元组的 TID */

	/* ---------------
	 * t_info 布局如下：
	 *
	 * 位15(高位)：有空值
	 * 位14：含变长属性
	 * 位13：AM 定义的含义
	 * 位12-0：元组大小
	 * ---------------
	 */

	unsigned short t_info;		/* 元组的各类信息 */

} IndexTupleData;				/* 更多数据紧随结构体末尾 */

typedef IndexTupleData *IndexTuple;

typedef struct IndexAttributeBitMapData
{
	bits8		bits[(INDEX_MAX_KEYS + 8 - 1) / 8];
}			IndexAttributeBitMapData;

typedef IndexAttributeBitMapData * IndexAttributeBitMap;

/*
 * t_info 操作宏
 */
#define INDEX_SIZE_MASK 0x1FFF
#define INDEX_AM_RESERVED_BIT 0x2000	/* 预留给索引 AM 特定
										 * 用途*/
#define INDEX_VAR_MASK	0x4000
#define INDEX_NULL_MASK 0x8000

#define IndexTupleSize(itup)		((Size) ((itup)->t_info & INDEX_SIZE_MASK))
#define IndexTupleHasNulls(itup)	((((IndexTuple) (itup))->t_info & INDEX_NULL_MASK))
#define IndexTupleHasVarwidths(itup) ((((IndexTuple) (itup))->t_info & INDEX_VAR_MASK))


/* indextuple.c 内例程 */
extern IndexTuple index_form_tuple(TupleDesc tupleDescriptor,
								   Datum *values, bool *isnull);
extern IndexTuple index_form_tuple_context(TupleDesc tupleDescriptor,
										   Datum *values, bool *isnull,
										   MemoryContext context);
extern Datum nocache_index_getattr(IndexTuple tup, int attnum,
								   TupleDesc tupleDesc);
extern void index_deform_tuple(IndexTuple tup, TupleDesc tupleDescriptor,
							   Datum *values, bool *isnull);
extern void index_deform_tuple_internal(TupleDesc tupleDescriptor,
										Datum *values, bool *isnull,
										char *tp, bits8 *bp, int hasnulls);
extern IndexTuple CopyIndexTuple(IndexTuple source);
extern IndexTuple index_truncate_tuple(TupleDesc sourceDescriptor,
									   IndexTuple source, int leavenatts);


/*
 * 入参为 infomask(主要因为它须能在 index_form_tuple
 * 时使用，以便分配足够的空间)。
 */
static inline Size
IndexInfoFindDataOffset(unsigned short t_info)
{
	if (!(t_info & INDEX_NULL_MASK))
		return MAXALIGN(sizeof(IndexTupleData));
	else
		return MAXALIGN(sizeof(IndexTupleData) + sizeof(IndexAttributeBitMapData));
}

#ifndef FRONTEND

/* ----------------
 *		index_getattr
 *
 *		此函数常被调用，故对可缓存属性与 NULL 的
 *		查找用宏，其余调用 nocache_index_getattr()。
 *
 * ----------------
 */
static inline Datum
index_getattr(IndexTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	Assert(PointerIsValid(isnull));
	Assert(attnum > 0);

	*isnull = false;

	if (!IndexTupleHasNulls(tup))
	{
		if (TupleDescAttr(tupleDesc, attnum - 1)->attcacheoff >= 0)
		{
			return fetchatt(TupleDescAttr(tupleDesc, attnum - 1),
							(char *) tup + IndexInfoFindDataOffset(tup->t_info)
							+ TupleDescAttr(tupleDesc, attnum - 1)->attcacheoff);
		}
		else
			return nocache_index_getattr(tup, attnum, tupleDesc);
	}
	else
	{
		if (att_isnull(attnum - 1, (bits8 *) tup + sizeof(IndexTupleData)))
		{
			*isnull = true;
			return (Datum) NULL;
		}
		else
			return nocache_index_getattr(tup, attnum, tupleDesc);
	}
}

#endif

/*
 * MaxIndexTuplesPerPage 是单个索引页可容纳元组数的
 * 上限。索引元组必须带有数据或空值位图，
 * 故可放心假定，它比裸 IndexTupleData 结构体
 * 至少大 1 字节。该除数的由来是：每个元组
 * 必须按 MAXALIGN 对齐，且必须有关联的行指针。
 *
 * 为与索引类型无关，本值不计页面上的任何特殊空间，
 * 因此偏保守。
 *
 * 注意：btree 非叶页中，首个元组没有键(其隐含为
 * 负无穷)，因而打破了"至少大 1 字节"这一假定。
 * 在此类页面上，N 个元组所需空间可比估算值少一个
 * MAXALIGN 单位，似乎可比估算值多容纳一个元组。
 * 但此类页面始终至少有 MAXALIGN 的特殊空间，故无妨。
 */
#define MaxIndexTuplesPerPage	\
	((int) ((BLCKSZ - SizeOfPageHeaderData) / \
			(MAXALIGN(sizeof(IndexTupleData) + 1) + sizeof(ItemIdData))))

#endif							/* ITUP_H */
