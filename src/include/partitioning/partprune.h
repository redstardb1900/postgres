/*-------------------------------------------------------------------------
 *
 * partprune.h
 *	  partprune.c 的原型
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/partitioning/partprune.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARTPRUNE_H
#define PARTPRUNE_H

#include "nodes/execnodes.h"
#include "partitioning/partdefs.h"

struct PlannerInfo;				/*  避免在此包含 pathnodes.h  */
struct RelOptInfo;


/*
 * PartitionPruneContext
 *		保存运行时对单个分区表做裁剪计算
 *		所需的信息。
 *
 * strategy			分区策略，如 LIST、RANGE、HASH。
 * partnatts		分区键列数。
 * nparts			本分区表的分区数。
 * boundinfo		本分区表的分区边界信息。
 * partcollation	含 partnatts 个元素的数组，存分区键
 *					列排序规则。
 * partsupfunc		分区键比较或散列函数的 FmgrInfos
 *					数组（partnatts 个）。（通常指向
 *					partrel 的分区键。）
 * stepcmpfuncs		各裁剪步骤与分区键的比较或散列
 *					函数 FmgrInfos 数组。
 * ppccontext		保存本 PartitionPruneContext 的
 *					附属数据（如各 FmgrInfos）。
 * planstate		执行时指向父计划节点的 PlanState；
 *					规划器调用时为 NULL。
 * exprcontext		求值裁剪表达式时用的 ExprContext
 * exprstates		按 PruneCxtStateIdx 索引的 ExprStates
 *					数组；每个裁剪步骤的每个分区键一个。
 *					有 planstate 时分配。
 */
typedef struct PartitionPruneContext
{
	char		strategy;
	int			partnatts;
	int			nparts;
	PartitionBoundInfo boundinfo;
	Oid		   *partcollation;
	FmgrInfo   *partsupfunc;
	FmgrInfo   *stepcmpfuncs;
	MemoryContext ppccontext;
	PlanState  *planstate;
	ExprContext *exprcontext;
	ExprState **exprstates;
} PartitionPruneContext;

/*
 * PruneCxtStateIdx() 计算步骤 step_id、分区键列
 * keyno 在 stepcmpfuncs[] 与 exprstates[] 中的正确
 * 下标。（有代码假定同一步的项连续，故不可
 * 随意选取。）
 */
#define PruneCxtStateIdx(partnatts, step_id, keyno) \
	((partnatts) * (step_id) + (keyno))

extern PartitionPruneInfo *make_partition_pruneinfo(struct PlannerInfo *root,
													struct RelOptInfo *parentrel,
													List *subpaths,
													List *prunequal);
extern Bitmapset *prune_append_rel_partitions(struct RelOptInfo *rel);
extern Bitmapset *get_matching_partitions(PartitionPruneContext *context,
										  List *pruning_steps);

#endif							/* PARTPRUNE_H */
