/*-------------------------------------------------------------------------
 *
 * kwlookup.h
 *	  PostgreSQL 的关键字查找
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/kwlookup.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef KWLOOKUP_H
#define KWLOOKUP_H

/*  ScanKeywordLookup 所用散列函数  */
typedef int (*ScanKeywordHashFunc) (const void *key, size_t keylen);

/*
 * 本结构含 ScanKeywordLookup 在关键字集中搜索所需
 * 数据。内容通常由 src/tools/gen_keywordlist.pl 从
 * 含 PG_KEYWORD 宏的头生成。
 */
typedef struct ScanKeywordList
{
	const char *kw_string;		/* 略 */
	const uint16 *kw_offsets;	/*  各关键字起点的偏移  */
	ScanKeywordHashFunc hash;	/*  关键字的完美散列函数  */
	int			num_keywords;	/*  关键字个数  */
	int			max_kw_len;		/* 略 */
} ScanKeywordList;


extern int	ScanKeywordLookup(const char *str, const ScanKeywordList *keywords);

/*  取第 N 个关键字文本应使用此项。  */
static inline const char *
GetScanKeyword(int n, const ScanKeywordList *keywords)
{
	return keywords->kw_string + keywords->kw_offsets[n];
}

#endif							/* KWLOOKUP_H */
