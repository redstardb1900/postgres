/*-------------------------------------------------------------------------
 *
 * itemptr.h
 *	  POSTGRES 磁盘项指针定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/itemptr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITEMPTR_H
#define ITEMPTR_H

#include "storage/block.h"
#include "storage/off.h"

/*
 * ItemPointer:
 *
 * 这是指向已知文件某磁盘页内一项的指针（
 * 例如从索引到其父表的交叉链接）。
 * ip_blkid 指明哪一块，ip_posid 指明 linp（
 * ItemIdData）数组中要的项。
 *
 * 注意：因每个元组头和索引
 * 元组头在磁盘上都有项指针，切勿浪费空间于
 * 结构填充字节。本结构按设计为六字节长（
 * 含三个 int16 字段），但少数编译器会将其
 * 填充到八字节，除非加以约束。我们在能做的地方
 * 施加适当劝说。若编译器不肯配合，就会浪费大量
 * 空间。
 */
typedef struct ItemPointerData
{
	BlockIdData ip_blkid;
	OffsetNumber ip_posid;
}

/* 若编译器理解 packed 与 aligned 的 pragmas，则用之 */
#if defined(pg_attribute_packed) && defined(pg_attribute_aligned)
			pg_attribute_packed()
			pg_attribute_aligned(2)
#endif
ItemPointerData;

typedef ItemPointerData *ItemPointer;

/* ----------------
 *		堆元组（t_ctid）所用特殊值
 * ----------------
 */

/*
 * 若堆元组持有推测插入令牌而非真正
 * TID，ip_posid 设为 SpecTokenOffsetNumber，令牌存于
 * ip_blkid。SpecTokenOffsetNumber 必须高于 MaxOffsetNumber，
 * 以便与常规项指针中的有效偏移号区分
 * 开。
 */
#define SpecTokenOffsetNumber		0xfffe

/*
 * 当 UPDATE 把元组移到不同分区时，旧元组
 * 版本的 t_ctid 设为此魔法值。
 */
#define MovedPartitionsOffsetNumber 0xfffd
#define MovedPartitionsBlockNumber	InvalidBlockNumber


/* ----------------
 *		支持函数
 * ----------------
 */

/*
 * ItemPointerIsValid
 *		磁盘项指针非 NULL 则为真。
 */
static inline bool
ItemPointerIsValid(const ItemPointerData *pointer)
{
	return PointerIsValid(pointer) && pointer->ip_posid != 0;
}

/*
 * ItemPointerGetBlockNumberNoCheck
 *		返回磁盘项指针的块号。
 */
static inline BlockNumber
ItemPointerGetBlockNumberNoCheck(const ItemPointerData *pointer)
{
	return BlockIdGetBlockNumber(&pointer->ip_blkid);
}

/*
 * ItemPointerGetBlockNumber
 *		同上，但验证项指针看起来有效。
 */
static inline BlockNumber
ItemPointerGetBlockNumber(const ItemPointerData *pointer)
{
	Assert(ItemPointerIsValid(pointer));
	return ItemPointerGetBlockNumberNoCheck(pointer);
}

/*
 * ItemPointerGetOffsetNumberNoCheck
 *		返回磁盘项指针的偏移号。
 */
static inline OffsetNumber
ItemPointerGetOffsetNumberNoCheck(const ItemPointerData *pointer)
{
	return pointer->ip_posid;
}

/*
 * ItemPointerGetOffsetNumber
 *		同上，但验证项指针看起来有效。
 */
static inline OffsetNumber
ItemPointerGetOffsetNumber(const ItemPointerData *pointer)
{
	Assert(ItemPointerIsValid(pointer));
	return ItemPointerGetOffsetNumberNoCheck(pointer);
}

/*
 * ItemPointerSet
 *		把磁盘项指针设为指定块和偏移。
 */
static inline void
ItemPointerSet(ItemPointerData *pointer, BlockNumber blockNumber, OffsetNumber offNum)
{
	Assert(PointerIsValid(pointer));
	BlockIdSet(&pointer->ip_blkid, blockNumber);
	pointer->ip_posid = offNum;
}

/*
 * ItemPointerSetBlockNumber
 *		把磁盘项指针设为指定块。
 */
static inline void
ItemPointerSetBlockNumber(ItemPointerData *pointer, BlockNumber blockNumber)
{
	Assert(PointerIsValid(pointer));
	BlockIdSet(&pointer->ip_blkid, blockNumber);
}

/*
 * ItemPointerSetOffsetNumber
 *		把磁盘项指针设为指定偏移。
 */
static inline void
ItemPointerSetOffsetNumber(ItemPointerData *pointer, OffsetNumber offsetNumber)
{
	Assert(PointerIsValid(pointer));
	pointer->ip_posid = offsetNumber;
}

/*
 * ItemPointerCopy
 *		把一个磁盘项指针的内容复制到另一个。
 *
 * 若 ItemPointer 中出现填充，则须另行处理，
 * 因其用作哈希键。
 */
static inline void
ItemPointerCopy(const ItemPointerData *fromPointer, ItemPointerData *toPointer)
{
	Assert(PointerIsValid(toPointer));
	Assert(PointerIsValid(fromPointer));
	*toPointer = *fromPointer;
}

/*
 * ItemPointerSetInvalid
 *		把磁盘项指针设为无效。
 */
static inline void
ItemPointerSetInvalid(ItemPointerData *pointer)
{
	Assert(PointerIsValid(pointer));
	BlockIdSet(&pointer->ip_blkid, InvalidBlockNumber);
	pointer->ip_posid = InvalidOffsetNumber;
}

/*
 * ItemPointerIndicatesMovedPartitions
 *		当且仅当块号表明元组已移到另一
 *		分区。
 */
static inline bool
ItemPointerIndicatesMovedPartitions(const ItemPointerData *pointer)
{
	return
		ItemPointerGetOffsetNumber(pointer) == MovedPartitionsOffsetNumber &&
		ItemPointerGetBlockNumberNoCheck(pointer) == MovedPartitionsBlockNumber;
}

/*
 * ItemPointerSetMovedPartitions
 *		表明 itempointer 所引用的项已移入
 *		不同分区。
 */
static inline void
ItemPointerSetMovedPartitions(ItemPointerData *pointer)
{
	ItemPointerSet(pointer, MovedPartitionsBlockNumber, MovedPartitionsOffsetNumber);
}

/* ----------------
 *		externs
 * ----------------
 */

extern bool ItemPointerEquals(ItemPointer pointer1, ItemPointer pointer2);
extern int32 ItemPointerCompare(ItemPointer arg1, ItemPointer arg2);
extern void ItemPointerInc(ItemPointer pointer);
extern void ItemPointerDec(ItemPointer pointer);

/* ----------------
 *		Datum 转换函数
 * ----------------
 */

static inline ItemPointer
DatumGetItemPointer(Datum X)
{
	return (ItemPointer) DatumGetPointer(X);
}

static inline Datum
ItemPointerGetDatum(const ItemPointerData *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_ITEMPOINTER(n) DatumGetItemPointer(PG_GETARG_DATUM(n))
#define PG_RETURN_ITEMPOINTER(x) return ItemPointerGetDatum(x)

#endif							/* ITEMPTR_H */
