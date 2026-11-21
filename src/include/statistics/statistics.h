/*-------------------------------------------------------------------------
 *
 * statistics.h
 *	  扩展统计与选择率估计函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/statistics/statistics.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STATISTICS_H
#define STATISTICS_H

#include "commands/vacuum.h"
#include "nodes/pathnodes.h"

#define STATS_MAX_DIMENSIONS	8	/*  最大属性数  */

/*  多变量 distinct 系数  */
#define STATS_NDISTINCT_MAGIC		0xA352BFA4	/*  结构标识  */
#define STATS_NDISTINCT_TYPE_BASIC	1	/*  结构版本  */

/*  MVNDistinctItem 表示列的单一组合  */
typedef struct MVNDistinctItem
{
	double		ndistinct;		/*  该组合的 ndistinct 值  */
	int			nattributes;	/*  属性个数  */
	AttrNumber *attributes;		/*  属性号  */
} MVNDistinctItem;

/*  含所有可能列组合的 MVNDistinct 对象  */
typedef struct MVNDistinct
{
	uint32		magic;			/*  魔数常量标记  */
	uint32		type;			/*  ndistinct 类型(BASIC)  */
	uint32		nitems;			/*  统计中的项数  */
	MVNDistinctItem items[FLEXIBLE_ARRAY_MEMBER];
} MVNDistinct;

/*  多变量函数依赖  */
#define STATS_DEPS_MAGIC		0xB4549A2C	/*  序列化标记  */
#define STATS_DEPS_TYPE_BASIC	1	/*  基本依赖类型  */

/*
 * 函数依赖，跟踪列级关系（一列的值
 * 决定另一列的值）。
 */
typedef struct MVDependency
{
	double		degree;			/*  有效程度（0-1）  */
	AttrNumber	nattributes;	/*  属性个数  */
	AttrNumber	attributes[FLEXIBLE_ARRAY_MEMBER];	/*  属性号  */
} MVDependency;

typedef struct MVDependencies
{
	uint32		magic;			/*  魔数常量标记  */
	uint32		type;			/*  MV 依赖类型（BASIC）  */
	uint32		ndeps;			/*  依赖个数  */
	MVDependency *deps[FLEXIBLE_ARRAY_MEMBER];	/*  依赖  */
} MVDependencies;

/*  标记序列化统计  */
#define STATS_MCV_MAGIC			0xE1A651C2	/*  序列化标记  */
#define STATS_MCV_TYPE_BASIC	1	/*  基本 MCV 类型  */

/*  MCV 最大项数（=default_statistics_target 上限）  */
#define STATS_MCVLIST_MAX_ITEMS        10000

/*
 * 多变量 MCV（最常见值）列表
 *
 * MCV 项的直接扩展——即属性值组合的
 * 列表（数组），附带频率与空值标志。
 */
typedef struct MCVItem
{
	double		frequency;		/*  该组合的频率  */
	double		base_frequency; /*  独立时的频率  */
	bool	   *isnull;			/*  空标  */
	Datum	   *values;			/*  项值  */
} MCVItem;

/*  多变量 MCV 列表——本质上是 MCV 项数组  */
typedef struct MCVList
{
	uint32		magic;			/*  魔数常量标记  */
	uint32		type;			/*  MCV 类型（BASIC）  */
	uint32		nitems;			/*  数组中的 MCV 项数  */
	AttrNumber	ndimensions;	/*  维数  */
	Oid			types[STATS_MAX_DIMENSIONS];	/*  各类型 OIDs  */
	MCVItem		items[FLEXIBLE_ARRAY_MEMBER];	/*  MCV 项数组  */
} MCVList;

extern MVNDistinct *statext_ndistinct_load(Oid mvoid, bool inh);
extern MVDependencies *statext_dependencies_load(Oid mvoid, bool inh);
extern MCVList *statext_mcv_load(Oid mvoid, bool inh);

extern void BuildRelationExtStatistics(Relation onerel, bool inh, double totalrows,
									   int numrows, HeapTuple *rows,
									   int natts, VacAttrStats **vacattrstats);
extern int	ComputeExtStatisticsRows(Relation onerel,
									 int natts, VacAttrStats **vacattrstats);
extern bool statext_is_kind_built(HeapTuple htup, char type);
extern Selectivity dependencies_clauselist_selectivity(PlannerInfo *root,
													   List *clauses,
													   int varRelid,
													   JoinType jointype,
													   SpecialJoinInfo *sjinfo,
													   RelOptInfo *rel,
													   Bitmapset **estimatedclauses);
extern Selectivity statext_clauselist_selectivity(PlannerInfo *root,
												  List *clauses,
												  int varRelid,
												  JoinType jointype,
												  SpecialJoinInfo *sjinfo,
												  RelOptInfo *rel,
												  Bitmapset **estimatedclauses,
												  bool is_or);
extern bool has_stats_of_kind(List *stats, char requiredkind);
extern StatisticExtInfo *choose_best_statistics(List *stats, char requiredkind,
												bool inh,
												Bitmapset **clause_attnums,
												List **clause_exprs,
												int nclauses);
extern HeapTuple statext_expressions_load(Oid stxoid, bool inh, int idx);

#endif							/* STATISTICS_H */
