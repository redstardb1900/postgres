/*-------------------------------------------------------------------------
 *
 * indexfsm.c
 *	  POSTGRES free space map for quickly finding free pages in relations
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/freespace/indexfsm.c
 *
 *
 * NOTES:
 *
 *	This is similar to the FSM used for heap, in freespace.c, but instead
 *	of tracking the amount of free space on pages, we only track whether
 *	pages are completely free or in-use. We use the same FSM implementation
 *	as for heaps, using 0 to denote used pages, and (BLCKSZ - 1) for unused.
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
