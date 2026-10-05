/*-------------------------------------------------------------------------
 *
 * indexfsm.c
 *	  供快速查找关系中空闲页的 POSTGRES 空闲空间映射
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/freespace/indexfsm.c
 *
 *
 * 说明：
 *
 *	这与 freespace.c 里用于堆的 FSM 相似，但我们并不
 *	跟踪页上的空闲空间量，而只跟踪
 *	页是完全空闲还是正在使用。我们使用与堆相同的 FSM 实现，
 *	用 0 表示已使用的页，用 (BLCKSZ - 1) 表示未使用的页。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/freespace.h"
#include "storage/indexfsm.h"

/*
 * 导出的例程
 */

/*
 * GetFreeIndexPage：从空闲空间映射（FSM）返回一个空闲页。
 *
 * 副作用是，该页在 FSM 中被标成已用。
 */
BlockNumber
GetFreeIndexPage(Relation rel)
{
	BlockNumber blkno = GetPageWithFreeSpace(rel, BLCKSZ / 2);

	if (blkno != InvalidBlockNumber)
		RecordUsedIndexPage(rel, blkno);

	return blkno;
}

/*
 * RecordFreeIndexPage：在空闲空间映射（FSM）里把一页标成空闲。
 */
void
RecordFreeIndexPage(Relation rel, BlockNumber freeBlock)
{
	RecordPageWithFreeSpace(rel, freeBlock, BLCKSZ - 1);
}


/*
 * RecordUsedIndexPage：在空闲空间映射（FSM）里把一页标成已用。
 */
void
RecordUsedIndexPage(Relation rel, BlockNumber usedBlock)
{
	RecordPageWithFreeSpace(rel, usedBlock, 0);
}

/*
 * IndexFreeSpaceMapVacuum：扫描并修正空闲空间映射（FSM）中的不一致。
 */
void
IndexFreeSpaceMapVacuum(Relation rel)
{
	FreeSpaceMapVacuum(rel);
}
