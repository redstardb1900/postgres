/*-------------------------------------------------------------------------
 *
 * combocid.h
 *	  组合命令 ID 支持例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/combocid.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMBOCID_H
#define COMBOCID_H

/*
 * HeapTupleHeaderGetCmin 与 HeapTupleHeaderGetCmax 的函数原型
 * 在 access/htup.h，因被其取代的宏定义原先就在
 * 那里。
 */

extern void AtEOXact_ComboCid(void);
extern void RestoreComboCIDState(char *comboCIDstate);
extern void SerializeComboCIDState(Size maxsize, char *start_address);
extern Size EstimateComboCIDStateSpace(void);

#endif							/* COMBOCID_H */
