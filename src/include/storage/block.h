/*-------------------------------------------------------------------------
 *
 * block.h
 *	  POSTGRES 磁盘块定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/block.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BLOCK_H
#define BLOCK_H

/*
 * BlockNumber:
 *
 * 每个数据文件（堆或索引）被分成 postgres 磁盘块（
 * 可视为 I/O 单位——一个 postgres 缓冲区
 * 恰好含一个磁盘块）。块按顺序编号，
 * 从 0 到 0xFFFFFFFE。
 *
 * InvalidBlockNumber 与 bufmgr.h 中的 P_NEW 相同。
 *
 * 访问方法、缓冲区管理器和存储管理器
 * 几乎是唯一应直接访问磁盘
 * 块的代码。
 */
typedef uint32 BlockNumber;

#define InvalidBlockNumber		((BlockNumber) 0xFFFFFFFF)

#define MaxBlockNumber			((BlockNumber) 0xFFFFFFFE)

/*
 * BlockId:
 *
 * 这是 BlockNumber 的存储类型。换言之，本类型
 * 用于磁盘上结构（如 HeapTupleData），而
 * BlockNumber 是做计算的类型（如在
 * 访问方法中）。
 *
 * 分开两种类型似乎并无别的理由，只因
 * BlockIds 可 SHORTALIGN（因而任何含它的
 * 结构，如 ItemPointerData，也可
 * SHORTALIGN）。这对减小每页行指针（
 * ItemIdData）数组以及每个堆或索引元组头的
 * 空间需求很重要，因此没有充分理由
 * 似乎不宜改动。
 */
typedef struct BlockIdData
{
	uint16		bi_hi;
	uint16		bi_lo;
} BlockIdData;

typedef BlockIdData *BlockId;	/* 块标识 */

/* ----------------
 *		支持函数
 * ----------------
 */

/*
 * BlockNumberIsValid
 *		blockNumber 有效则真。
 */
static inline bool
BlockNumberIsValid(BlockNumber blockNumber)
{
	return blockNumber != InvalidBlockNumber;
}

/*
 * BlockIdSet
 *		把块标识设为指定值。
 */
static inline void
BlockIdSet(BlockIdData *blockId, BlockNumber blockNumber)
{
	blockId->bi_hi = blockNumber >> 16;
	blockId->bi_lo = blockNumber & 0xffff;
}

/*
 * BlockIdEquals
 *		检查块号是否相等。
 */
static inline bool
BlockIdEquals(const BlockIdData *blockId1, const BlockIdData *blockId2)
{
	return (blockId1->bi_hi == blockId2->bi_hi &&
			blockId1->bi_lo == blockId2->bi_lo);
}

/*
 * BlockIdGetBlockNumber
 *		从块标识取出块号。
 */
static inline BlockNumber
BlockIdGetBlockNumber(const BlockIdData *blockId)
{
	return (((BlockNumber) blockId->bi_hi) << 16) | ((BlockNumber) blockId->bi_lo);
}

#endif							/* BLOCK_H */
