/*
 * AM 可调用的 BRIN 索引函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/brin.h
 */
#ifndef BRIN_H
#define BRIN_H

#include "nodes/execnodes.h"
#include "utils/relcache.h"


/*
 * BRIN 关系选项的存储类型
 */
typedef struct BrinOptions
{
	int32		vl_len_;		/* varlena 头(勿直接碰!) */
	BlockNumber pagesPerRange;
	bool		autosummarize;
} BrinOptions;


/*
 * BrinStatsData 表示规划器所用统计数据。
 */
typedef struct BrinStatsData
{
	BlockNumber pagesPerRange;
	BlockNumber revmapNumPages;
} BrinStatsData;


#define BRIN_DEFAULT_PAGES_PER_RANGE	128
#define BrinGetPagesPerRange(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == BRIN_AM_OID), \
	 (relation)->rd_options ? \
	 ((BrinOptions *) (relation)->rd_options)->pagesPerRange : \
	  BRIN_DEFAULT_PAGES_PER_RANGE)
#define BrinGetAutoSummarize(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == BRIN_AM_OID), \
	 (relation)->rd_options ? \
	 ((BrinOptions *) (relation)->rd_options)->autosummarize : \
	  false)


extern void brinGetStats(Relation index, BrinStatsData *stats);

#endif							/* BRIN_H */
