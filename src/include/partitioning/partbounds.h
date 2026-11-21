/*-------------------------------------------------------------------------
 *
 * partbounds.h
 *
 * Copyright (c) 2007-2023, PostgreSQL Global Development Group
 *
 * src/include/partitioning/partbounds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARTBOUNDS_H
#define PARTBOUNDS_H

#include "fmgr.h"
#include "parser/parse_node.h"
#include "partitioning/partdefs.h"

struct RelOptInfo;				/*  避免在此包含 pathnodes.h  */


/*
 * PartitionBoundInfoData 封装一组分区边界。通常
 * 作为分区描述符的一部分关联到分区表，也可
 * 表示规划器中的虚拟分区表，如分区
 * 连接关系。
 *
 * 已知为 NULL 的列表分区 datum 从不放入
 * datums 数组，而用 null_index 字段跟踪。
 *
 * 范围分区时 ndatums 通常远小于 2*nparts，因
 * 一分区上界与下一分区下界在常见情形相同，
 * 我们只存其一（上界）。散列分区时 ndatums
 * 等于分区数。
 * 。
 *
 * 对范围与列表分区表，datums 是每元组含
 * key->partnatts 个 datum 的数组。对散列分区表，
 * 是每元组 2 个 datum（模与余数）的数组，对应
 * 给定分区。
 *
 * datums 数组按递增序排列，分别由
 * qsort_partition_rbound_cmp()、qsort_partition_list_value_cmp() 与
 * qsort_partition_hbound_cmp() 为范围、列表与散列
 * 分区表定义。对范围与列表分区，这简单意味着
 * 按分区键运算符类与排序规则定义的递增序
 * 排列。
 *
 * 列表分区时，indexes 为每个 datum 数组项存一条
 * 记录：接受匹配该 datum 之行的分区下标。故
 * nindexes == ndatums。
 *
 * 范围分区时，indexes 为每个相异范围 datum 存
 * 一条：该 datum 为其上界的分区下标（无分区的
 * "间隙" 为 -1）。另有额外 -1 表示高于最后
 * 范围 datum 的值，故 nindexes == ndatums + 1。
 * 。
 *
 * 散列分区时，indexes 项数等于所有分区中最大
 * 模（且为各分区模的倍数），故 nindexes 等于
 * 最大模。indexes 按下标=散列键余数对最大模
 * 取模索引，内容为接受该余数的分区下标，或
 * 无对应分区时为 -1。
 * 。
 *
 * 对 LIST 分区表，跟踪可能"交错"分区的分区
 * 下标。若某分区允许多值且存在另一分区可含
 * 介于其间的值，则视为交错。例如存在分区
 * FOR VALUES IN(3,5) 与另一 FOR VALUES IN(4)，则
 * IN(3,5) 分区为交错分区。DEFAULT 分区亦然，
 * 因其可含不属于其他分区的任何值。本字段仅
 * 证明某分区非交错，不能证明其交错；不确定时
 * 标为交错。interleaved_parts 仅对
 * RELOPT_BASEREL 与 RELOPT_OTHER_MEMBER_REL 设置，
 * 连接关系上始终为 NULL。
 * 。
 * 。
 * 。
 */
typedef struct PartitionBoundInfoData
{
	PartitionStrategy strategy; /*  分区策略  */
	int			ndatums;		/*  datums[] 数组长度  */
	Datum	  **datums;
	PartitionRangeDatumKind **kind; /*  各范围边界 datum 的种类；
									 * 散列与列表分区表为
									 * NULL */
	Bitmapset  *interleaved_parts;	/*  可能交错分区的分区下标。
									 * 见上文。仅对 LIST
									 * 分区表设置 */
	int			nindexes;		/*  indexes[] 数组长度  */
	int		   *indexes;		/*  分区下标  */
	int			null_index;		/*  接受空值的分区下标；无则为
								 * -1 */
	int			default_index;	/*  默认分区下标；无则为
								 * -1 */
} PartitionBoundInfoData;

#define partition_bound_accepts_nulls(bi) ((bi)->null_index != -1)
#define partition_bound_has_default(bi) ((bi)->default_index != -1)

extern int	get_hash_partition_greatest_modulus(PartitionBoundInfo bound);
extern uint64 compute_partition_hash_value(int partnatts, FmgrInfo *partsupfunc,
										   Oid *partcollation,
										   Datum *values, bool *isnull);
extern List *get_qual_from_partbound(Relation parent,
									 PartitionBoundSpec *spec);
extern PartitionBoundInfo partition_bounds_create(PartitionBoundSpec **boundspecs,
												  int nparts, PartitionKey key, int **mapping);
extern bool partition_bounds_equal(int partnatts, int16 *parttyplen,
								   bool *parttypbyval, PartitionBoundInfo b1,
								   PartitionBoundInfo b2);
extern PartitionBoundInfo partition_bounds_copy(PartitionBoundInfo src,
												PartitionKey key);
extern PartitionBoundInfo partition_bounds_merge(int partnatts,
												 FmgrInfo *partsupfunc,
												 Oid *partcollation,
												 struct RelOptInfo *outer_rel,
												 struct RelOptInfo *inner_rel,
												 JoinType jointype,
												 List **outer_parts,
												 List **inner_parts);
extern bool partitions_are_ordered(PartitionBoundInfo boundinfo,
								   Bitmapset *live_parts);
extern void check_new_partition_bound(char *relname, Relation parent,
									  PartitionBoundSpec *spec,
									  ParseState *pstate);
extern void check_default_partition_contents(Relation parent,
											 Relation default_rel,
											 PartitionBoundSpec *new_spec);

extern int32 partition_rbound_datum_cmp(FmgrInfo *partsupfunc,
										Oid *partcollation,
										Datum *rb_datums, PartitionRangeDatumKind *rb_kind,
										Datum *tuple_datums, int n_tuple_datums);
extern int	partition_list_bsearch(FmgrInfo *partsupfunc,
								   Oid *partcollation,
								   PartitionBoundInfo boundinfo,
								   Datum value, bool *is_equal);
extern int	partition_range_datum_bsearch(FmgrInfo *partsupfunc,
										  Oid *partcollation,
										  PartitionBoundInfo boundinfo,
										  int nvalues, Datum *values, bool *is_equal);
extern int	partition_hash_bsearch(PartitionBoundInfo boundinfo,
								   int modulus, int remainder);

#endif							/* PARTBOUNDS_H */
