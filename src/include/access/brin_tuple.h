/*
 * brin_tuple.h
 *		处理 BRIN 专用元组的声明。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/access/brin_tuple.h
 */
#ifndef BRIN_TUPLE_H
#define BRIN_TUPLE_H

#include "access/brin_internal.h"
#include "access/tupdesc.h"

/*
 * BRIN 操作符类可注册序列化回调，若磁盘上
 * 与内存中的表示不同（例如出于性能原因）。
 */
typedef void (*brin_serialize_callback_type) (BrinDesc *bdesc, Datum src, Datum *dst);

/*
 * BRIN 索引为每页范围存一个索引元组。每个索引元组
 * 对每个被索引列有一个 BrinValues 结构；进而每个 BrinValues
 * 除空值标志外还有一个 Datum 数组，其大小由操作符类
 * 决定。
 */
typedef struct BrinValues
{
	AttrNumber	bv_attno;		/* 索引属性号 */
	bool		bv_hasnulls;	/* 页范围内是否有任何空值？ */
	bool		bv_allnulls;	/* 页范围内是否全部为空值？ */
	Datum	   *bv_values;		/* 当前累积值 */
	Datum		bv_mem_value;	/* 展开的累积值 */
	MemoryContext bv_context;
	brin_serialize_callback_type bv_serialize;
} BrinValues;

/*
 * 本结构用于表示内存中的索引元组。这些值只有
 * 借助合适的 BrinDesc 才有意义地解码。
 */
typedef struct BrinMemTuple
{
	bool		bt_placeholder; /* 这是占位元组 */
	bool		bt_empty_range; /* 范围表示无元组 */
	BlockNumber bt_blkno;		/* 该元组对应的堆块号 */
	MemoryContext bt_context;	/* 持有 bt_columns 值的 memcxt */
	/* brin_deform_tuple 输出数组： */
	Datum	   *bt_values;		/* values数组 */
	bool	   *bt_allnulls;	/* allnulls数组 */
	bool	   *bt_hasnulls;	/* hasnulls数组 */
	/* 不是输出数组，但须在最后 */
	BrinValues	bt_columns[FLEXIBLE_ARRAY_MEMBER];
} BrinMemTuple;

/*
 * 磁盘上的 BRIN 元组。其后可能跟有空值位掩码，
 * 可容 2 空值位（每被索引列两位）；后跟操作符类定义的
 * 每列若干 Datum 值。
 */
typedef struct BrinTuple
{
	/* 该元组对应的堆块号 */
	BlockNumber bt_blkno;

	/* ---------------
	 * bt_info 按如下方式布局：
	 *
	 * 第7(高)位:有空值
	 * 第6位:是占位元组
	 * 第5位:范围为空
	 * 4-0位:数据偏移
	 * ---------------
	 */
	uint8		bt_info;
} BrinTuple;

#define SizeOfBrinTuple (offsetof(BrinTuple, bt_info) + sizeof(uint8))

/*
 * bt_info 操纵宏
 */
#define BRIN_OFFSET_MASK		0x1F
#define BRIN_EMPTY_RANGE_MASK	0x20
#define BRIN_PLACEHOLDER_MASK	0x40
#define BRIN_NULLS_MASK			0x80

#define BrinTupleDataOffset(tup)	((Size) (((BrinTuple *) (tup))->bt_info & BRIN_OFFSET_MASK))
#define BrinTupleHasNulls(tup)	(((((BrinTuple *) (tup))->bt_info & BRIN_NULLS_MASK)) != 0)
#define BrinTupleIsPlaceholder(tup) (((((BrinTuple *) (tup))->bt_info & BRIN_PLACEHOLDER_MASK)) != 0)
#define BrinTupleIsEmptyRange(tup) (((((BrinTuple *) (tup))->bt_info & BRIN_EMPTY_RANGE_MASK)) != 0)


extern BrinTuple *brin_form_tuple(BrinDesc *brdesc, BlockNumber blkno,
								  BrinMemTuple *tuple, Size *size);
extern BrinTuple *brin_form_placeholder_tuple(BrinDesc *brdesc,
											  BlockNumber blkno, Size *size);
extern void brin_free_tuple(BrinTuple *tuple);
extern BrinTuple *brin_copy_tuple(BrinTuple *tuple, Size len,
								  BrinTuple *dest, Size *destsz);
extern bool brin_tuples_equal(const BrinTuple *a, Size alen,
							  const BrinTuple *b, Size blen);

extern BrinMemTuple *brin_new_memtuple(BrinDesc *brdesc);
extern BrinMemTuple *brin_memtuple_initialize(BrinMemTuple *dtuple,
											  BrinDesc *brdesc);
extern BrinMemTuple *brin_deform_tuple(BrinDesc *brdesc,
									   BrinTuple *tuple, BrinMemTuple *dMemtuple);

#endif							/* BRIN_TUPLE_H */
