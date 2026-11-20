/*-------------------------------------------------------------------------
 *
 * freespace.h
 *	  POSTGRES 关系空闲空间映射，供快速查找空闲空间。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/freespace.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FREESPACE_H_
#define FREESPACE_H_

#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/* freespace.c 中公开函数原型 */
extern Size GetRecordedFreeSpace(Relation rel, BlockNumber heapBlk);
extern BlockNumber GetPageWithFreeSpace(Relation rel, Size spaceNeeded);
extern BlockNumber RecordAndGetPageWithFreeSpace(Relation rel,
												 BlockNumber oldPage,
												 Size oldSpaceAvail,
												 Size spaceNeeded);
extern void RecordPageWithFreeSpace(Relation rel, BlockNumber heapBlk,
									Size spaceAvail);
extern void XLogRecordPageWithFreeSpace(RelFileLocator rlocator, BlockNumber heapBlk,
										Size spaceAvail);

extern BlockNumber FreeSpaceMapPrepareTruncateRel(Relation rel,
												  BlockNumber nblocks);
extern void FreeSpaceMapVacuum(Relation rel);
extern void FreeSpaceMapVacuumRange(Relation rel, BlockNumber start,
									BlockNumber end);

#endif							/* FREESPACE_H_ */
