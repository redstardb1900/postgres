/*-------------------------------------------------------------------------
 *
 * off.h
 *	  POSTGRES 磁盘"偏移"定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/off.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef OFF_H
#define OFF_H

#include "storage/itemid.h"
/*
 * OffsetNumber:
 *
 * 这是每页头中 linp（ItemIdData）数组的
 * 从 1 起的下标。
 */
typedef uint16 OffsetNumber;

#define InvalidOffsetNumber		((OffsetNumber) 0)
#define FirstOffsetNumber		((OffsetNumber) 1)
#define MaxOffsetNumber			((OffsetNumber) (BLCKSZ / sizeof(ItemIdData)))

/* ----------------
 *		支持宏
 * ----------------
 */

/*
 * OffsetNumberIsValid
 *		当且仅当偏移号有效为真。
 */
#define OffsetNumberIsValid(offsetNumber) \
	((bool) ((offsetNumber != InvalidOffsetNumber) && \
			 (offsetNumber <= MaxOffsetNumber)))

/*
 * OffsetNumberNext
 * OffsetNumberPrev
 *		把参数加一/减一。这些宏看似多余，
 *		但有助于区分对 OffsetNumbers 的不同操作（
 *		例如有时减一是为了后退，有时是为了
 *		把 OffsetNumber 变成真正 C 数组
 *		下标）。
 */
#define OffsetNumberNext(offsetNumber) \
	((OffsetNumber) (1 + (offsetNumber)))
#define OffsetNumberPrev(offsetNumber) \
	((OffsetNumber) (-1 + (offsetNumber)))

#endif							/* OFF_H */
