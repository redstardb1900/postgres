/*-------------------------------------------------------------------------
 *
 * index_selfuncs.h
 *	  标准索引访问方法的索引代价估计函数。
 *
 *
 * 注意：自 selfuncs.h 拆出，主要为避免把规划器的全部
 * 数据结构引入索引 AM 的非规划器部分。
 * 若再依赖 access/amapi.h 以外之物，多半是
 * 弄错。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/index_selfuncs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEX_SELFUNCS_H
#define INDEX_SELFUNCS_H

#include "access/amapi.h"

/*  selfuncs.c 中函数  */
extern void brincostestimate(struct PlannerInfo *root,
							 struct IndexPath *path,
							 double loop_count,
							 Cost *indexStartupCost,
							 Cost *indexTotalCost,
							 Selectivity *indexSelectivity,
							 double *indexCorrelation,
							 double *indexPages);
extern void btcostestimate(struct PlannerInfo *root,
						   struct IndexPath *path,
						   double loop_count,
						   Cost *indexStartupCost,
						   Cost *indexTotalCost,
						   Selectivity *indexSelectivity,
						   double *indexCorrelation,
						   double *indexPages);
extern void hashcostestimate(struct PlannerInfo *root,
							 struct IndexPath *path,
							 double loop_count,
							 Cost *indexStartupCost,
							 Cost *indexTotalCost,
							 Selectivity *indexSelectivity,
							 double *indexCorrelation,
							 double *indexPages);
extern void gistcostestimate(struct PlannerInfo *root,
							 struct IndexPath *path,
							 double loop_count,
							 Cost *indexStartupCost,
							 Cost *indexTotalCost,
							 Selectivity *indexSelectivity,
							 double *indexCorrelation,
							 double *indexPages);
extern void spgcostestimate(struct PlannerInfo *root,
							struct IndexPath *path,
							double loop_count,
							Cost *indexStartupCost,
							Cost *indexTotalCost,
							Selectivity *indexSelectivity,
							double *indexCorrelation,
							double *indexPages);
extern void gincostestimate(struct PlannerInfo *root,
							struct IndexPath *path,
							double loop_count,
							Cost *indexStartupCost,
							Cost *indexTotalCost,
							Selectivity *indexSelectivity,
							double *indexCorrelation,
							double *indexPages);

#endif							/* INDEX_SELFUNCS_H */
