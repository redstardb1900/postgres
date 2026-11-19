/*--------------------------------------------------------------------------
 * gin.h
 *	  广义倒排索引访问方法的公开头文件。
 *
 *	Copyright (c) 2006-2023, PostgreSQL Global Development Group
 *
 *	src/include/access/gin.h
 *--------------------------------------------------------------------------
 */
#ifndef GIN_H
#define GIN_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/block.h"
#include "utils/relcache.h"


/*
 * 倒排索引的 amproc 编号。
 */
#define GIN_COMPARE_PROC			   1
#define GIN_EXTRACTVALUE_PROC		   2
#define GIN_EXTRACTQUERY_PROC		   3
#define GIN_CONSISTENT_PROC			   4
#define GIN_COMPARE_PARTIAL_PROC	   5
#define GIN_TRICONSISTENT_PROC		   6
#define GIN_OPTIONS_PROC	   7
#define GINNProcs					   7

/*
 * extractQueryFn 的 searchMode 设置。
 */
#define GIN_SEARCH_MODE_DEFAULT			0
#define GIN_SEARCH_MODE_INCLUDE_EMPTY	1
#define GIN_SEARCH_MODE_ALL				2
#define GIN_SEARCH_MODE_EVERYTHING		3	/* 仅供内部使用 */

/*
 * GinStatsData 表示规划器所用统计数据。
 */
typedef struct GinStatsData
{
	BlockNumber nPendingPages;
	BlockNumber nTotalPages;
	BlockNumber nEntryPages;
	BlockNumber nDataPages;
	int64		nEntries;
	int32		ginVersion;
} GinStatsData;

/*
 * 三值，供 tri-consistent 函数使用。
 *
 * 其大小必须与 bool 相同，因有些代码会把
 * 指向 bool 的指针转为指向 GinTernaryValue。
 */
typedef char GinTernaryValue;

StaticAssertDecl(sizeof(GinTernaryValue) == sizeof(bool),
				 "sizes of GinTernaryValue and bool are not equal");

#define GIN_FALSE		0		/* 项不存在 / 不匹配 */
#define GIN_TRUE		1		/* 项存在 / 匹配 */
#define GIN_MAYBE		2		/* 不知项是否存在/不知其是否
								 * 匹配*/

static inline GinTernaryValue
DatumGetGinTernaryValue(Datum X)
{
	return (GinTernaryValue) X;
}

static inline Datum
GinTernaryValueGetDatum(GinTernaryValue X)
{
	return (Datum) X;
}

#define PG_RETURN_GIN_TERNARY_VALUE(x) return GinTernaryValueGetDatum(x)

/* GUC 参数 */
extern PGDLLIMPORT int GinFuzzySearchLimit;
extern PGDLLIMPORT int gin_pending_list_limit;

/* ginutil.c */
extern void ginGetStats(Relation index, GinStatsData *stats);
extern void ginUpdateStats(Relation index, const GinStatsData *stats,
						   bool is_build);

#endif							/* GIN_H */
