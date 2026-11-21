/*-------------------------------------------------------------------------
 *
 * keywords.h
 *	略
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/keywords.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef KEYWORDS_H
#define KEYWORDS_H

#include "common/kwlookup.h"

/* 略 */
#define UNRESERVED_KEYWORD		0
#define COL_NAME_KEYWORD		1
#define TYPE_FUNC_NAME_KEYWORD	2
#define RESERVED_KEYWORD		3

extern PGDLLIMPORT const ScanKeywordList ScanKeywords;
extern PGDLLIMPORT const uint8 ScanKeywordCategories[];
extern PGDLLIMPORT const bool ScanKeywordBareLabel[];

#endif							/* KEYWORDS_H */
