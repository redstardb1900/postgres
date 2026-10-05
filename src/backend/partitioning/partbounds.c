/*-------------------------------------------------------------------------
 *
 * partbounds.c
 *		Support routines for manipulating partition bounds
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/backend/partitioning/partbounds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "commands/tablecmds.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "parser/parse_coerce.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "partitioning/partprune.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * 从系统目录读出后对分区边界做 qsort 时，每个边界
 * 用下列结构之一表示。
 */

/* 哈希分区的一个边界 */
typedef struct PartitionHashBound
{
	int			modulus;
	int			remainder;
	int			index;
} PartitionHashBound;

/* 来自某个（第 index 个）列表分区的一个值 */
typedef struct PartitionListValue
{
	int			index;
	Datum		value;
} PartitionListValue;

/* 区间分区的一个边界 */
typedef struct PartitionRangeBound
{
	int			index;
	Datum	   *datums;			/* 区间边界的 datum */
	PartitionRangeDatumKind *kind;	/* 每个 datum 的种类 */
	bool		lower;			/* 这是下界（相对上界而言） */
} PartitionRangeBound;

/*
 * 从被连接关系的分区到正在计算的连接
 * 关系的分区的映射（即合并后的分区）
 */
typedef struct PartitionMap
{
	int			nparts;			/* 分区个数 */
	int		   *merged_indexes; /* 合并后分区的下标 */
	bool	   *merged;			/* 表示分区是否
								 * 与非哑分区合并的标志 */
	bool		did_remapping;	/* 我们是否重新映射了分区？ */
	int		   *old_indexes;	/* 若 did_remapping，合并后分区的
								 * 旧下标 */
} PartitionMap;

/* 比较两个区间边界的宏 */
#define compare_range_bounds(partnatts, partsupfunc, partcollations, \
							 bound1, bound2) \
	(partition_rbound_cmp(partnatts, partsupfunc, partcollations, \
						  (bound1)->datums, (bound1)->kind, (bound1)->lower, \
						  bound2))

static int32 qsort_partition_hbound_cmp(const void *a, const void *b);
static int32 qsort_partition_list_value_cmp(const void *a, const void *b,
											void *arg);
static int32 qsort_partition_rbound_cmp(const void *a, const void *b,
										void *arg);
static PartitionBoundInfo create_hash_bounds(PartitionBoundSpec **boundspecs,
											 int nparts, PartitionKey key, int **mapping);
static PartitionBoundInfo create_list_bounds(PartitionBoundSpec **boundspecs,
											 int nparts, PartitionKey key, int **mapping);
static PartitionBoundInfo create_range_bounds(PartitionBoundSpec **boundspecs,
											  int nparts, PartitionKey key, int **mapping);
static PartitionBoundInfo merge_list_bounds(FmgrInfo *partsupfunc,
											Oid *partcollation,
											RelOptInfo *outer_rel,
											RelOptInfo *inner_rel,
											JoinType jointype,
											List **outer_parts,
											List **inner_parts);
static PartitionBoundInfo merge_range_bounds(int partnatts,
											 FmgrInfo *partsupfuncs,
											 Oid *partcollations,
											 RelOptInfo *outer_rel,
											 RelOptInfo *inner_rel,
											 JoinType jointype,
											 List **outer_parts,
											 List **inner_parts);
static void init_partition_map(RelOptInfo *rel, PartitionMap *map);
static void free_partition_map(PartitionMap *map);
static bool is_dummy_partition(RelOptInfo *rel, int part_index);
static int	merge_matching_partitions(PartitionMap *outer_map,
									  PartitionMap *inner_map,
									  int outer_index,
									  int inner_index,
									  int *next_index);
static int	process_outer_partition(PartitionMap *outer_map,
									PartitionMap *inner_map,
									bool outer_has_default,
									bool inner_has_default,
									int outer_index,
									int inner_default,
									JoinType jointype,
									int *next_index,
									int *default_index);
static int	process_inner_partition(PartitionMap *outer_map,
									PartitionMap *inner_map,
									bool outer_has_default,
									bool inner_has_default,
									int inner_index,
									int outer_default,
									JoinType jointype,
									int *next_index,
									int *default_index);
static void merge_null_partitions(PartitionMap *outer_map,
								  PartitionMap *inner_map,
								  bool outer_has_null,
								  bool inner_has_null,
								  int outer_null,
								  int inner_null,
								  JoinType jointype,
								  int *next_index,
								  int *null_index);
static void merge_default_partitions(PartitionMap *outer_map,
									 PartitionMap *inner_map,
									 bool outer_has_default,
									 bool inner_has_default,
									 int outer_default,
									 int inner_default,
									 JoinType jointype,
									 int *next_index,
									 int *default_index);
static int	merge_partition_with_dummy(PartitionMap *map, int index,
									   int *next_index);
static void fix_merged_indexes(PartitionMap *outer_map,
							   PartitionMap *inner_map,
							   int nmerged, List *merged_indexes);
static void generate_matching_part_pairs(RelOptInfo *outer_rel,
										 RelOptInfo *inner_rel,
										 PartitionMap *outer_map,
										 PartitionMap *inner_map,
										 int nmerged,
										 List **outer_parts,
										 List **inner_parts);
static PartitionBoundInfo build_merged_partition_bounds(char strategy,
														List *merged_datums,
														List *merged_kinds,
														List *merged_indexes,
														int null_index,
														int default_index);
static int	get_range_partition(RelOptInfo *rel,
								PartitionBoundInfo bi,
								int *lb_pos,
								PartitionRangeBound *lb,
								PartitionRangeBound *ub);
static int	get_range_partition_internal(PartitionBoundInfo bi,
										 int *lb_pos,
										 PartitionRangeBound *lb,
										 PartitionRangeBound *ub);
static bool compare_range_partitions(int partnatts, FmgrInfo *partsupfuncs,
									 Oid *partcollations,
									 PartitionRangeBound *outer_lb,
									 PartitionRangeBound *outer_ub,
									 PartitionRangeBound *inner_lb,
									 PartitionRangeBound *inner_ub,
									 int *lb_cmpval, int *ub_cmpval);
static void get_merged_range_bounds(int partnatts, FmgrInfo *partsupfuncs,
									Oid *partcollations, JoinType jointype,
									PartitionRangeBound *outer_lb,
									PartitionRangeBound *outer_ub,
									PartitionRangeBound *inner_lb,
									PartitionRangeBound *inner_ub,
									int lb_cmpval, int ub_cmpval,
									PartitionRangeBound *merged_lb,
									PartitionRangeBound *merged_ub);
static void add_merged_range_bounds(int partnatts, FmgrInfo *partsupfuncs,
									Oid *partcollations,
									PartitionRangeBound *merged_lb,
									PartitionRangeBound *merged_ub,
									int merged_index,
									List **merged_datums,
									List **merged_kinds,
									List **merged_indexes);
static PartitionRangeBound *make_one_partition_rbound(PartitionKey key, int index,
													  List *datums, bool lower);
static int32 partition_hbound_cmp(int modulus1, int remainder1, int modulus2,
								  int remainder2);
static int32 partition_rbound_cmp(int partnatts, FmgrInfo *partsupfunc,
								  Oid *partcollation, Datum *datums1,
								  PartitionRangeDatumKind *kind1, bool lower1,
								  PartitionRangeBound *b2);
static int	partition_range_bsearch(int partnatts, FmgrInfo *partsupfunc,
									Oid *partcollation,
									PartitionBoundInfo boundinfo,
									PartitionRangeBound *probe, int32 *cmpval);
static Expr *make_partition_op_expr(PartitionKey key, int keynum,
									uint16 strategy, Expr *arg1, Expr *arg2);
static Oid	get_partition_operator(PartitionKey key, int col,
								   StrategyNumber strategy, bool *need_relabel);
static List *get_qual_for_hash(Relation parent, PartitionBoundSpec *spec);
static List *get_qual_for_list(Relation parent, PartitionBoundSpec *spec);
static List *get_qual_for_range(Relation parent, PartitionBoundSpec *spec,
								bool for_default);
static void get_range_key_properties(PartitionKey key, int keynum,
									 PartitionRangeDatum *ldatum,
									 PartitionRangeDatum *udatum,
									 ListCell **partexprs_item,
									 Expr **keyCol,
									 Const **lower_val, Const **upper_val);
static List *get_range_nulltest(PartitionKey key);

/*
 * get_qual_from_partbound
 *		给定分区边界的解析器节点，返回作为分区约束的
 *		可执行表达式列表
 */
List *
get_qual_from_partbound(Relation parent, PartitionBoundSpec *spec)
{
	PartitionKey key = RelationGetPartitionKey(parent);
	List	   *my_qual = NIL;

	Assert(key != NULL);

	switch (key->strategy)
	{
		case PARTITION_STRATEGY_HASH:
			Assert(spec->strategy == PARTITION_STRATEGY_HASH);
			my_qual = get_qual_for_hash(parent, spec);
			break;

		case PARTITION_STRATEGY_LIST:
			Assert(spec->strategy == PARTITION_STRATEGY_LIST);
			my_qual = get_qual_for_list(parent, spec);
			break;

		case PARTITION_STRATEGY_RANGE:
			Assert(spec->strategy == PARTITION_STRATEGY_RANGE);
			my_qual = get_qual_for_range(parent, spec, false);
			break;
	}

	return my_qual;
}

/*
 *	partition_bounds_create
 *		从 PartitionBoundSpec 节点列表构造
 *		PartitionBoundInfo 结构。
 *
 * 本函数创建 PartitionBoundInfo，并按输入列表填写其
 * 各个成员。重要的是，datums 数组将包含
 * 各个边界的 Datum 表示（区间边界时可能经过
 * 去重），并按相应分区方法的 qsort_partition_
 * 函数所定义的规范次序排序。
 * indexes 数组的元素个数与边界个数相同（本函数体内
 * 列出了这条规则的具体例外），它们表示
 * 分区从 0 起计的规范位置。
 *
 * 本函数返回时，*mapping 被设为一个有
 * list_length(boundspecs) 个元素的数组，每个元素把分区的
 * 原始下标映射到其规范下标。
 *
 * 注意：本函数返回的对象全部在
 * 当前内存上下文中分配。
 */
PartitionBoundInfo
partition_bounds_create(PartitionBoundSpec **boundspecs, int nparts,
						PartitionKey key, int **mapping)
{
	int			i;

	Assert(nparts > 0);

	/*
	 * 对每种分区方法，我们先把分区边界
	 * 从解析器节点表示转换成内部表示，
	 * 并做任何额外预处理（例如对区间
	 * 边界去重）。得到的边界 datum 再加入
	 * PartitionBoundInfo 的 datums 数组。每加入一个 datum，就把表示
	 * 规范分区下标的整数加入 indexes 数组。
	 *
	 * 对每个边界，我们记住它的分区在
	 * 原始列表中的位置（从 0 起计），以便稍后映射到规范下标。
	 */

	/*
	 * 用无效值初始化映射数组，具体填充
	 * 由下面各子程序按边界类型完成。
	 */
	*mapping = (int *) palloc(sizeof(int) * nparts);
	for (i = 0; i < nparts; i++)
		(*mapping)[i] = -1;

	switch (key->strategy)
	{
		case PARTITION_STRATEGY_HASH:
			return create_hash_bounds(boundspecs, nparts, key, mapping);

		case PARTITION_STRATEGY_LIST:
			return create_list_bounds(boundspecs, nparts, key, mapping);

		case PARTITION_STRATEGY_RANGE:
			return create_range_bounds(boundspecs, nparts, key, mapping);
	}

	Assert(false);
	return NULL;				/* 消除编译器告警 */
}

/*
 * create_hash_bounds
 *		为哈希分区表创建 PartitionBoundInfo
 */
static PartitionBoundInfo
create_hash_bounds(PartitionBoundSpec **boundspecs, int nparts,
				   PartitionKey key, int **mapping)
{
	PartitionBoundInfo boundinfo;
	PartitionHashBound *hbounds;
	int			i;
	int			greatest_modulus;
	Datum	   *boundDatums;

	boundinfo = (PartitionBoundInfoData *)
		palloc0(sizeof(PartitionBoundInfoData));
	boundinfo->strategy = key->strategy;
	/* 没有特殊的哈希分区。 */
	boundinfo->null_index = -1;
	boundinfo->default_index = -1;

	hbounds = (PartitionHashBound *)
		palloc(nparts * sizeof(PartitionHashBound));

	/* 从节点转换成内部表示 */
	for (i = 0; i < nparts; i++)
	{
		PartitionBoundSpec *spec = boundspecs[i];

		if (spec->strategy != PARTITION_STRATEGY_HASH)
			elog(ERROR, "invalid strategy in partition bound spec");

		hbounds[i].modulus = spec->modulus;
		hbounds[i].remainder = spec->remainder;
		hbounds[i].index = i;
	}

	/* 把全部分区边界按升序排序 */
	qsort(hbounds, nparts, sizeof(PartitionHashBound),
		  qsort_partition_hbound_cmp);

	/* 排序之后，模数现在按升序存放。 */
	greatest_modulus = hbounds[nparts - 1].modulus;

	boundinfo->ndatums = nparts;
	boundinfo->datums = (Datum **) palloc0(nparts * sizeof(Datum *));
	boundinfo->kind = NULL;
	boundinfo->interleaved_parts = NULL;
	boundinfo->nindexes = greatest_modulus;
	boundinfo->indexes = (int *) palloc(greatest_modulus * sizeof(int));
	for (i = 0; i < greatest_modulus; i++)
		boundinfo->indexes[i] = -1;

	/*
	 * 为了避免在下面的循环里分配一串小的 datum
	 * 数组，这里只分配一个数组，下面再为每个分区
	 * 分配该数组的一段。
	 */
	boundDatums = (Datum *) palloc(nparts * 2 * sizeof(Datum));

	/*
	 * 对哈希分区，datum（模数与余数
	 * 对）的个数与分区个数相同。下标就是从
	 * 0 到 (nparts - 1) 的值。
	 */
	for (i = 0; i < nparts; i++)
	{
		int			modulus = hbounds[i].modulus;
		int			remainder = hbounds[i].remainder;

		boundinfo->datums[i] = &boundDatums[i * 2];
		boundinfo->datums[i][0] = Int32GetDatum(modulus);
		boundinfo->datums[i][1] = Int32GetDatum(remainder);

		while (remainder < greatest_modulus)
		{
			/* 重叠？ */
			Assert(boundinfo->indexes[remainder] == -1);
			boundinfo->indexes[remainder] = i;
			remainder += modulus;
		}

		(*mapping)[hbounds[i].index] = i;
	}
	pfree(hbounds);

	return boundinfo;
}

/*
 * get_non_null_list_datum_count
 * 		统计每个分区里非空 Datum 的个数。
 */
static int
get_non_null_list_datum_count(PartitionBoundSpec **boundspecs, int nparts)
{
	int			i;
	int			count = 0;

	for (i = 0; i < nparts; i++)
	{
		ListCell   *lc;

		foreach(lc, boundspecs[i]->listdatums)
		{
			Const	   *val = lfirst_node(Const, lc);

			if (!val->constisnull)
				count++;
		}
	}

	return count;
}

/*
 * create_list_bounds
 *		为列表分区表创建 PartitionBoundInfo
 */
static PartitionBoundInfo
create_list_bounds(PartitionBoundSpec **boundspecs, int nparts,
				   PartitionKey key, int **mapping)
{
	PartitionBoundInfo boundinfo;
	PartitionListValue *all_values;
	int			i;
	int			j;
	int			ndatums;
	int			next_index = 0;
	int			default_index = -1;
	int			null_index = -1;
	Datum	   *boundDatums;

	boundinfo = (PartitionBoundInfoData *)
		palloc0(sizeof(PartitionBoundInfoData));
	boundinfo->strategy = key->strategy;
	/* 下面会正确设置。 */
	boundinfo->null_index = -1;
	boundinfo->default_index = -1;

	ndatums = get_non_null_list_datum_count(boundspecs, nparts);
	all_values = (PartitionListValue *)
		palloc(ndatums * sizeof(PartitionListValue));

	/* 建立跨全部分区的非空值统一列表。 */
	for (j = 0, i = 0; i < nparts; i++)
	{
		PartitionBoundSpec *spec = boundspecs[i];
		ListCell   *c;

		if (spec->strategy != PARTITION_STRATEGY_LIST)
			elog(ERROR, "invalid strategy in partition bound spec");

		/*
		 * 记下默认分区的分区边界说明的下标。
		 * 这个分区没有要加入非空 datum
		 * 列表的 datum。
		 */
		if (spec->is_default)
		{
			default_index = i;
			continue;
		}

		foreach(c, spec->listdatums)
		{
			Const	   *val = lfirst_node(Const, c);

			if (!val->constisnull)
			{
				all_values[j].index = i;
				all_values[j].value = val->constvalue;
				j++;
			}
			else
			{
				/*
				 * 绝不把空值放进 values 数组；改为保存
				 * 存放空值的那个分区的下标。
				 */
				if (null_index != -1)
					elog(ERROR, "found null more than once");
				null_index = i;
			}
		}
	}

	/* 确保 all_values 数组的每个槽位都找到了 Datum */
	Assert(j == ndatums);

	qsort_arg(all_values, ndatums, sizeof(PartitionListValue),
			  qsort_partition_list_value_cmp, key);

	boundinfo->ndatums = ndatums;
	boundinfo->datums = (Datum **) palloc0(ndatums * sizeof(Datum *));
	boundinfo->kind = NULL;
	boundinfo->interleaved_parts = NULL;
	boundinfo->nindexes = ndatums;
	boundinfo->indexes = (int *) palloc(ndatums * sizeof(int));

	/*
	 * 为了避免在下面的循环里分配一串小的 datum
	 * 数组，这里只分配一个数组，下面再为每个 datum
	 * 分配该数组的一段。
	 */
	boundDatums = (Datum *) palloc(ndatums * sizeof(Datum));

	/*
	 * 复制值。规范下标是赋给每个分区的、从 0 到 (nparts -
	 * 1) 的值，使给定分区的全部 datum
	 * 得到同一个值。给定分区的这个值，是该分区最小 datum
	 * 在 all_values[] 数组中的下标。
	 */
	for (i = 0; i < ndatums; i++)
	{
		int			orig_index = all_values[i].index;

		boundinfo->datums[i] = &boundDatums[i];
		boundinfo->datums[i][0] = datumCopy(all_values[i].value,
											key->parttypbyval[0],
											key->parttyplen[0]);

		/* 若旧下标还没有映射，就分配一个 */
		if ((*mapping)[orig_index] == -1)
			(*mapping)[orig_index] = next_index++;

		boundinfo->indexes[i] = (*mapping)[orig_index];
	}

	pfree(all_values);

	/*
	 * 若有 null_index，设置它的规范值。
	 *
	 * 接受空值的分区可能还没有被分配
	 * 下标，若该分区只接受空值
	 * 就会这样，因为上面的循环只看了非空
	 * 值。
	 */
	if (null_index != -1)
	{
		Assert(null_index >= 0);
		if ((*mapping)[null_index] == -1)
			(*mapping)[null_index] = next_index++;
		boundinfo->null_index = (*mapping)[null_index];
	}

	/* 若有 default_index，设置它的规范值。 */
	if (default_index != -1)
	{
		/*
		 * 默认分区接受其他分区列表里没有指定的任何值，
		 * 因此在为非空 datum 分配映射下标时，
		 * 它不应当得到映射下标。
		 */
		Assert(default_index >= 0);
		Assert((*mapping)[default_index] == -1);
		(*mapping)[default_index] = next_index++;
		boundinfo->default_index = (*mapping)[default_index];
	}

	/*
	 * 计算交错分区。这里查找可能与其他分区
	 * 交错的分区，并对任何可能与
	 * 另一分区交错的分区，在
	 * interleaved_parts 里置位。
	 */

	/*
	 * 必须有多个分区才可能有交错分区，
	 * 否则没有可以交错的对象。
	 */
	if (nparts > 1)
	{
		/*
		 * 短路检查：是否每个分区只允许 1 个 Datum。
		 * 若是，就不必做更
		 * 贵的检查去找交错值。
		 */
		if (boundinfo->ndatums +
			partition_bound_accepts_nulls(boundinfo) +
			partition_bound_has_default(boundinfo) != nparts)
		{
			int			last_index = -1;

			/*
			 * 因为 indexes 数组按 Datum 次序排序，若有
			 * 分区交错，就会表现为
			 * 分区下标不是升序。这里检查
			 * 这一点，并记录所有次序不对的分区。
			 */
			for (i = 0; i < boundinfo->nindexes; i++)
			{
				int			index = boundinfo->indexes[i];

				if (index < last_index)
					boundinfo->interleaved_parts = bms_add_member(boundinfo->interleaved_parts,
																  index);

				/*
				 * 否则，若 null_index 出现在 indexes 数组里，
				 * 则 NULL 分区还必须允许某个其他 Datum，
				 * 因此它是交错的。
				 */
				else if (partition_bound_accepts_nulls(boundinfo) &&
						 index == boundinfo->null_index)
					boundinfo->interleaved_parts = bms_add_member(boundinfo->interleaved_parts,
																  index);

				last_index = index;
			}
		}

		/*
		 * DEFAULT 分区是可以包含
		 * 不属于任何其他分区的任何东西的兜底分区。若还有
		 * 任何其他分区，就必须把 DEFAULT 分区标成
		 * 交错的。
		 */
		if (partition_bound_has_default(boundinfo))
			boundinfo->interleaved_parts = bms_add_member(boundinfo->interleaved_parts,
														  boundinfo->default_index);
	}


	/* 现在全部分区都应当已分配规范下标。 */
	Assert(next_index == nparts);
	return boundinfo;
}

/*
 * create_range_bounds
 *		为区间分区表创建 PartitionBoundInfo
 */
static PartitionBoundInfo
create_range_bounds(PartitionBoundSpec **boundspecs, int nparts,
					PartitionKey key, int **mapping)
{
	PartitionBoundInfo boundinfo;
	PartitionRangeBound **rbounds = NULL;
	PartitionRangeBound **all_bounds,
			   *prev;
	int			i,
				k,
				partnatts;
	int			ndatums = 0;
	int			default_index = -1;
	int			next_index = 0;
	Datum	   *boundDatums;
	PartitionRangeDatumKind *boundKinds;

	boundinfo = (PartitionBoundInfoData *)
		palloc0(sizeof(PartitionBoundInfoData));
	boundinfo->strategy = key->strategy;
	/* 没有专门接受空值的区间分区。 */
	boundinfo->null_index = -1;
	/* 下面会正确设置。 */
	boundinfo->default_index = -1;

	all_bounds = (PartitionRangeBound **)
		palloc0(2 * nparts * sizeof(PartitionRangeBound *));

	/* 建立跨全部分区的区间边界统一列表。 */
	ndatums = 0;
	for (i = 0; i < nparts; i++)
	{
		PartitionBoundSpec *spec = boundspecs[i];
		PartitionRangeBound *lower,
				   *upper;

		if (spec->strategy != PARTITION_STRATEGY_RANGE)
			elog(ERROR, "invalid strategy in partition bound spec");

		/*
		 * 记下默认分区的分区边界说明的下标。
		 * 这个分区没有要加入 all_bounds 数组的
		 * datum。
		 */
		if (spec->is_default)
		{
			default_index = i;
			continue;
		}

		lower = make_one_partition_rbound(key, i, spec->lowerdatums, true);
		upper = make_one_partition_rbound(key, i, spec->upperdatums, false);
		all_bounds[ndatums++] = lower;
		all_bounds[ndatums++] = upper;
	}

	Assert(ndatums == nparts * 2 ||
		   (default_index != -1 && ndatums == (nparts - 1) * 2));

	/* 把全部分区边界按升序排序 */
	qsort_arg(all_bounds, ndatums,
			  sizeof(PartitionRangeBound *),
			  qsort_partition_rbound_cmp,
			  key);

	/* 把 all_bounds 里互异的边界存进 rbounds。 */
	rbounds = (PartitionRangeBound **)
		palloc(ndatums * sizeof(PartitionRangeBound *));
	k = 0;
	prev = NULL;
	for (i = 0; i < ndatums; i++)
	{
		PartitionRangeBound *cur = all_bounds[i];
		bool		is_distinct = false;
		int			j;

		/* 当前边界与前一个不同吗？ */
		for (j = 0; j < key->partnatts; j++)
		{
			Datum		cmpval;

			if (prev == NULL || cur->kind[j] != prev->kind[j])
			{
				is_distinct = true;
				break;
			}

			/*
			 * 若两个边界都是 MINVALUE 或都是 MAXVALUE，现在就停止并当作
			 * 它们相等，因为这一点之后的任何值都必须
			 * 忽略。
			 */
			if (cur->kind[j] != PARTITION_RANGE_DATUM_VALUE)
				break;

			cmpval = FunctionCall2Coll(&key->partsupfunc[j],
									   key->partcollation[j],
									   cur->datums[j],
									   prev->datums[j]);
			if (DatumGetInt32(cmpval) != 0)
			{
				is_distinct = true;
				break;
			}
		}

		/*
		 * 只有边界互异时才把它存进临时数组，即
		 * 稍后复制进 boundinfo datum 数组的 rbounds。
		 */
		if (is_distinct)
			rbounds[k++] = all_bounds[i];

		prev = cur;
	}

	pfree(all_bounds);

	/* 把 ndatums 更新为互异 datum 的个数。 */
	ndatums = k;

	/*
	 * 把 datum 加入 boundinfo。规范下标是从 0
	 * 到 nparts - 1 的值，按该次序赋给每个分区的上界。
	 * 对作为下界的 datums 元素，indexes 数组里放 -1，
	 * 表示小于该边界、且大于等于上一上界的那些值
	 * 不属于任何
	 * 分区。
	 */
	boundinfo->ndatums = ndatums;
	boundinfo->datums = (Datum **) palloc0(ndatums * sizeof(Datum *));
	boundinfo->kind = (PartitionRangeDatumKind **)
		palloc(ndatums *
			   sizeof(PartitionRangeDatumKind *));
	boundinfo->interleaved_parts = NULL;

	/*
	 * 对区间分区，indexes[] 数组的最后一个元素
	 * 额外存放一个 -1。
	 */
	boundinfo->nindexes = ndatums + 1;
	boundinfo->indexes = (int *) palloc((ndatums + 1) * sizeof(int));

	/*
	 * 为了避免在下面的循环里分配一串小数组，
	 * 这里只为 Datum 分配一个数组，再为
	 * PartitionRangeDatumKinds 分配另一个，下面再在每次循环里
	 * 分配这些数组的一段。
	 */
	partnatts = key->partnatts;
	boundDatums = (Datum *) palloc(ndatums * partnatts * sizeof(Datum));
	boundKinds = (PartitionRangeDatumKind *) palloc(ndatums * partnatts *
													sizeof(PartitionRangeDatumKind));

	for (i = 0; i < ndatums; i++)
	{
		int			j;

		boundinfo->datums[i] = &boundDatums[i * partnatts];
		boundinfo->kind[i] = &boundKinds[i * partnatts];
		for (j = 0; j < partnatts; j++)
		{
			if (rbounds[i]->kind[j] == PARTITION_RANGE_DATUM_VALUE)
				boundinfo->datums[i][j] =
					datumCopy(rbounds[i]->datums[j],
							  key->parttypbyval[j],
							  key->parttyplen[j]);
			boundinfo->kind[i][j] = rbounds[i]->kind[j];
		}

		/*
		 * 无效下标没有映射。
		 *
		 * rbounds 数组里的任何下界都被赋了无效下标，
		 * 因为上一边界（若有）与这个（下）边界之间的值
		 * 不属于任何
		 * 现有分区的区间。
		 */
		if (rbounds[i]->lower)
			boundinfo->indexes[i] = -1;
		else
		{
			int			orig_index = rbounds[i]->index;

			/* 若旧下标还没有映射，就分配一个 */
			if ((*mapping)[orig_index] == -1)
				(*mapping)[orig_index] = next_index++;

			boundinfo->indexes[i] = (*mapping)[orig_index];
		}
	}

	pfree(rbounds);

	/* 若有 default_index，设置它的规范值。 */
	if (default_index != -1)
	{
		Assert(default_index >= 0 && (*mapping)[default_index] == -1);
		(*mapping)[default_index] = next_index++;
		boundinfo->default_index = (*mapping)[default_index];
	}

	/* 额外的那个 -1 元素。 */
	Assert(i == ndatums);
	boundinfo->indexes[i] = -1;

	/* 现在全部分区都应当已分配规范下标。 */
	Assert(next_index == nparts);
	return boundinfo;
}

/*
 * 两套分区边界集合在逻辑上相等吗？
 *
 * 用于 relcache.c 的保留逻辑（即 RelationClearRelation()）。
 * 当 b1 和 b2 分别是两个不同
 * 关系的边界集合时这也有用，因为 PartitionBoundInfo 是分区边界的
 * 规范表示。
 */
bool
partition_bounds_equal(int partnatts, int16 *parttyplen, bool *parttypbyval,
					   PartitionBoundInfo b1, PartitionBoundInfo b2)
{
	int			i;

	if (b1->strategy != b2->strategy)
		return false;

	if (b1->ndatums != b2->ndatums)
		return false;

	if (b1->nindexes != b2->nindexes)
		return false;

	if (b1->null_index != b2->null_index)
		return false;

	if (b1->default_index != b2->default_index)
		return false;

	/* 对所有分区策略，indexes[] 数组都必须匹配 */
	for (i = 0; i < b1->nindexes; i++)
	{
		if (b1->indexes[i] != b2->indexes[i])
			return false;
	}

	/* 最后比较 datums[] 数组 */
	if (b1->strategy == PARTITION_STRATEGY_HASH)
	{
		/*
		 * 我们按模数和余数的升序安排分区。
		 * 而且每个模数都是下一个更大
		 * 模数的因子。因此可以安全地把给定分区的下标
		 * 存在 indexes 数组里该分区余数的位置。并且对任何分区的
		 * (模数, 余数) 说明，indexes 数组里
		 * (余数 + N * 模数) 位置的项全都相同。因此给定边界的
		 * datums 数组相同，当且仅当
		 * 它们的 indexes 数组相同。所以比较
		 * indexes 数组就够了。
		 *
		 * 尽管如此，在下标匹配时仍要确认边界确实相同。
		 * 哈希分区边界把模数和余数
		 * 分别存在 b1->datums[i][0] 和 b1->datums[i][1]。
		 */
#ifdef USE_ASSERT_CHECKING
		for (i = 0; i < b1->ndatums; i++)
			Assert((b1->datums[i][0] == b2->datums[i][0] &&
					b1->datums[i][1] == b2->datums[i][1]));
#endif
	}
	else
	{
		for (i = 0; i < b1->ndatums; i++)
		{
			int			j;

			for (j = 0; j < partnatts; j++)
			{
				/* 对区间分区，边界可能不是有限的。 */
				if (b1->kind != NULL)
				{
					/* 不同种类的边界彼此都不同 */
					if (b1->kind[i][j] != b2->kind[i][j])
						return false;

					/*
					 * 非有限边界无需进一步
					 * 检查即相等。
					 */
					if (b1->kind[i][j] != PARTITION_RANGE_DATUM_VALUE)
						continue;
				}

				/*
				 * 比较实际的值。注意，在这里调用
				 * 由分区说明导出的比较操作符既
				 * 不正确也不安全。不正确是因为我们希望关系缓存项对分区边界的
				 * 任何变化都更新，而不只是
				 * 分区操作符认为
				 * 有意义的那些。不安全是因为我们可能在
				 * 已中止事务的上下文里到达
				 * 这段代码，任意的分区操作符在该
				 * 上下文里可能不安全。datumIsEqual() 应当
				 * 简单到足够
				 * 安全。
				 */
				if (!datumIsEqual(b1->datums[i][j], b2->datums[i][j],
								  parttypbyval[j], parttyplen[j]))
					return false;
			}
		}
	}
	return true;
}

/*
 * 返回给定 PartitionBoundInfo 结构的副本。边界的数据类型
 * 由给定的分区键说明描述。
 *
 * 注意：本函数及其被调用者不要做任何系统目录
 * 访问，也不要做任何会导致在返回的数据结构之外分配内存的事。
 * 因为这是在长寿命上下文里调用的，
 * 否则会造成不想要的内存泄漏。
 */
PartitionBoundInfo
partition_bounds_copy(PartitionBoundInfo src,
					  PartitionKey key)
{
	PartitionBoundInfo dest;
	int			i;
	int			ndatums;
	int			nindexes;
	int			partnatts;
	bool		hash_part;
	int			natts;
	Datum	   *boundDatums;

	dest = (PartitionBoundInfo) palloc(sizeof(PartitionBoundInfoData));

	dest->strategy = src->strategy;
	ndatums = dest->ndatums = src->ndatums;
	nindexes = dest->nindexes = src->nindexes;
	partnatts = key->partnatts;

	/* 列表分区表只有单个分区键。 */
	Assert(key->strategy != PARTITION_STRATEGY_LIST || partnatts == 1);

	dest->datums = (Datum **) palloc(sizeof(Datum *) * ndatums);

	if (src->kind != NULL)
	{
		PartitionRangeDatumKind *boundKinds;

		/* 只有 RANGE 分区才应有非 NULL 的 kind */
		Assert(key->strategy == PARTITION_STRATEGY_RANGE);

		dest->kind = (PartitionRangeDatumKind **) palloc(ndatums *
														 sizeof(PartitionRangeDatumKind *));

		/*
		 * 为了避免在下面的循环里为存放 PartitionRangeDatumKind 分配一串小数组，
		 * 这里分配一整块，
		 * 再为每个 datum 使用其中较小的一段。
		 */
		boundKinds = (PartitionRangeDatumKind *) palloc(ndatums * partnatts *
														sizeof(PartitionRangeDatumKind));

		for (i = 0; i < ndatums; i++)
		{
			dest->kind[i] = &boundKinds[i * partnatts];
			memcpy(dest->kind[i], src->kind[i],
				   sizeof(PartitionRangeDatumKind) * partnatts);
		}
	}
	else
		dest->kind = NULL;

	/* 为 LIST 分区表复制交错分区 */
	dest->interleaved_parts = bms_copy(src->interleaved_parts);

	/*
	 * 对哈希分区，datums 数组有两个元素：模数
	 * 和余数。
	 */
	hash_part = (key->strategy == PARTITION_STRATEGY_HASH);
	natts = hash_part ? 2 : partnatts;
	boundDatums = palloc(ndatums * natts * sizeof(Datum));

	for (i = 0; i < ndatums; i++)
	{
		int			j;

		dest->datums[i] = &boundDatums[i * natts];

		for (j = 0; j < natts; j++)
		{
			bool		byval;
			int			typlen;

			if (hash_part)
			{
				typlen = sizeof(int32); /* 始终是 int4 */
				byval = true;	/* int4 按值传递 */
			}
			else
			{
				byval = key->parttypbyval[j];
				typlen = key->parttyplen[j];
			}

			if (dest->kind == NULL ||
				dest->kind[i][j] == PARTITION_RANGE_DATUM_VALUE)
				dest->datums[i][j] = datumCopy(src->datums[i][j],
											   byval, typlen);
		}
	}

	dest->indexes = (int *) palloc(sizeof(int) * nindexes);
	memcpy(dest->indexes, src->indexes, sizeof(int) * nindexes);

	dest->null_index = src->null_index;
	dest->default_index = src->default_index;

	return dest;
}

/*
 * partition_bounds_merge
 *		检查 outer_rel 的每个分区是否最多匹配/重叠
 *		inner_rel 的一个分区，反之亦然；若是，就构造
 *		并返回这两个关系之间连接关系的分区边界，
 *		生成两份匹配/重叠分区的列表，分别
 *		返回到 *outer_parts 和 *inner_parts。
 *
 * 这两份列表含有相同个数的分区，相同位置上的分区
 * 表示用于分区连接的连接对。
 * 若一侧的一个分区匹配/重叠另一侧的多个分区，
 * 本函数返回 NULL，并把 *outer_parts 和 *inner_parts 设为
 * NIL。
 */
PartitionBoundInfo
partition_bounds_merge(int partnatts,
					   FmgrInfo *partsupfunc, Oid *partcollation,
					   RelOptInfo *outer_rel, RelOptInfo *inner_rel,
					   JoinType jointype,
					   List **outer_parts, List **inner_parts)
{
	/*
	 * 目前本函数只从 try_partitionwise_join() 调用，
	 * 因此连接类型应当是 INNER、LEFT、FULL、SEMI 或 ANTI。
	 */
	Assert(jointype == JOIN_INNER || jointype == JOIN_LEFT ||
		   jointype == JOIN_FULL || jointype == JOIN_SEMI ||
		   jointype == JOIN_ANTI);

	/* 分区策略应当相同。 */
	Assert(outer_rel->boundinfo->strategy == inner_rel->boundinfo->strategy);

	*outer_parts = *inner_parts = NIL;
	switch (outer_rel->boundinfo->strategy)
	{
		case PARTITION_STRATEGY_HASH:

			/*
			 * 对哈希分区表，目前只在它们的分区边界
			 * 完全相同时才支持分区连接。
			 *
			 * XXX：也许可以放宽限制，以支持
			 * 哈希分区表缺少分区
			 * 和/或模数不同的情形，但不清楚
			 * 支持前一种情形是否有用，因为
			 * 缺少分区并不常见。另一方面，
			 * 支持后一种情形会有用，但那种情况下，
			 * 一侧的一个分区很有可能匹配
			 * 另一侧的多个分区，而这正是当前
			 * 分区连接实现没法处理的情形。
			 */
			return NULL;

		case PARTITION_STRATEGY_LIST:
			return merge_list_bounds(partsupfunc,
									 partcollation,
									 outer_rel,
									 inner_rel,
									 jointype,
									 outer_parts,
									 inner_parts);

		case PARTITION_STRATEGY_RANGE:
			return merge_range_bounds(partnatts,
									  partsupfunc,
									  partcollation,
									  outer_rel,
									  inner_rel,
									  jointype,
									  outer_parts,
									  inner_parts);
	}

	return NULL;
}

/*
 * merge_list_bounds
 *		若可能，为列表
 *		分区表之间的连接关系创建分区边界。
 *
 * 本函数试图通过比较分区边界里存放的列表值，
 * 找出两侧匹配的分区集合。因为列表
 * 值按升序出现，这里用类似归并连接的
 * 算法。
 * 若一侧的分区在另一侧没有匹配
 * 分区，算法就试着把它配上另一侧的
 * 默认分区（若有）；若没有，且它在
 * 外连接的非空一侧，算法就试着把它配上
 * 另一侧的哑分区。另外，若两侧都有默认
 * 分区，算法就试着让它们彼此匹配。若算法发现
 * 一个分区匹配另一侧的多个分区，我们就放弃，
 * 这是当前分区连接实现没法处理的情形。
 */
static PartitionBoundInfo
merge_list_bounds(FmgrInfo *partsupfunc, Oid *partcollation,
				  RelOptInfo *outer_rel, RelOptInfo *inner_rel,
				  JoinType jointype,
				  List **outer_parts, List **inner_parts)
{
	PartitionBoundInfo merged_bounds = NULL;
	PartitionBoundInfo outer_bi = outer_rel->boundinfo;
	PartitionBoundInfo inner_bi = inner_rel->boundinfo;
	bool		outer_has_default = partition_bound_has_default(outer_bi);
	bool		inner_has_default = partition_bound_has_default(inner_bi);
	int			outer_default = outer_bi->default_index;
	int			inner_default = inner_bi->default_index;
	bool		outer_has_null = partition_bound_accepts_nulls(outer_bi);
	bool		inner_has_null = partition_bound_accepts_nulls(inner_bi);
	PartitionMap outer_map;
	PartitionMap inner_map;
	int			outer_pos;
	int			inner_pos;
	int			next_index = 0;
	int			null_index = -1;
	int			default_index = -1;
	List	   *merged_datums = NIL;
	List	   *merged_indexes = NIL;

	Assert(*outer_parts == NIL);
	Assert(*inner_parts == NIL);
	Assert(outer_bi->strategy == inner_bi->strategy &&
		   outer_bi->strategy == PARTITION_STRATEGY_LIST);
	/* 列表分区不需要 kinds。 */
	Assert(!outer_bi->kind && !inner_bi->kind);

	init_partition_map(outer_rel, &outer_map);
	init_partition_map(inner_rel, &inner_map);

	/*
	 * 若默认分区（若有）已被证明为空，就当作它们
	 * 不存在。
	 */
	if (outer_has_default && is_dummy_partition(outer_rel, outer_default))
		outer_has_default = false;
	if (inner_has_default && is_dummy_partition(inner_rel, inner_default))
		inner_has_default = false;

	/*
	 * 合并两侧的分区。每次迭代比较一对
	 * 列表值，每侧一个，并决定
	 * 对应分区是否匹配。若两个值
	 * 完全匹配，就移到下一对列表值，否则移到
	 * 列表值较小那一侧的下一个列表值。
	 */
	outer_pos = inner_pos = 0;
	while (outer_pos < outer_bi->ndatums || inner_pos < inner_bi->ndatums)
	{
		int			outer_index = -1;
		int			inner_index = -1;
		Datum	   *outer_datums;
		Datum	   *inner_datums;
		int			cmpval;
		Datum	   *merged_datum = NULL;
		int			merged_index = -1;

		if (outer_pos < outer_bi->ndatums)
		{
			/*
			 * 若外侧分区已被证明为空，
			 * 就忽略它并移到外侧的下一个 datum。
			 */
			outer_index = outer_bi->indexes[outer_pos];
			if (is_dummy_partition(outer_rel, outer_index))
			{
				outer_pos++;
				continue;
			}
		}
		if (inner_pos < inner_bi->ndatums)
		{
			/*
			 * 若内侧分区已被证明为空，
			 * 就忽略它并移到内侧的下一个 datum。
			 */
			inner_index = inner_bi->indexes[inner_pos];
			if (is_dummy_partition(inner_rel, inner_index))
			{
				inner_pos++;
				continue;
			}
		}

		/* 取得列表值。 */
		outer_datums = outer_pos < outer_bi->ndatums ?
			outer_bi->datums[outer_pos] : NULL;
		inner_datums = inner_pos < inner_bi->ndatums ?
			inner_bi->datums[inner_pos] : NULL;

		/*
		 * 这个循环一直跑到两侧都结束。这样可以避免
		 * 为较晚结束那一侧剩余的值重复写代码。
		 * 为此我们把比较参数 cmpval 设成看起来像
		 * 较早结束的那一侧还有一个额外值，
		 * 比未结束一侧的任何值都高。这样我们就推进
		 * 未结束一侧的值，直到它的
		 * 全部值都用完。
		 */
		if (outer_pos >= outer_bi->ndatums)
			cmpval = 1;
		else if (inner_pos >= inner_bi->ndatums)
			cmpval = -1;
		else
		{
			Assert(outer_datums != NULL && inner_datums != NULL);
			cmpval = DatumGetInt32(FunctionCall2Coll(&partsupfunc[0],
													 partcollation[0],
													 outer_datums[0],
													 inner_datums[0]));
		}

		if (cmpval == 0)
		{
			/* 两个列表值完全匹配。 */
			Assert(outer_pos < outer_bi->ndatums);
			Assert(inner_pos < inner_bi->ndatums);
			Assert(outer_index >= 0);
			Assert(inner_index >= 0);

			/*
			 * 尝试合并两个分区。若成功，就在下面加入列表值
			 * 和合并后分区的下标。
			 */
			merged_index = merge_matching_partitions(&outer_map, &inner_map,
													 outer_index, inner_index,
													 &next_index);
			if (merged_index == -1)
				goto cleanup;

			merged_datum = outer_datums;

			/* 移到下一对列表值。 */
			outer_pos++;
			inner_pos++;
		}
		else if (cmpval < 0)
		{
			/* 内层缺少的一个列表值。 */
			Assert(outer_pos < outer_bi->ndatums);

			/*
			 * 若内侧有默认分区，或这是
			 * 外连接，就试着给外侧分区分配一个合并后的分区
			 * （见 process_outer_partition()）。否则，外侧
			 * 分区不会对结果有贡献。
			 */
			if (inner_has_default || IS_OUTER_JOIN(jointype))
			{
				/* 取得外侧分区。 */
				outer_index = outer_bi->indexes[outer_pos];
				Assert(outer_index >= 0);
				merged_index = process_outer_partition(&outer_map,
													   &inner_map,
													   outer_has_default,
													   inner_has_default,
													   outer_index,
													   inner_default,
													   jointype,
													   &next_index,
													   &default_index);
				if (merged_index == -1)
					goto cleanup;
				merged_datum = outer_datums;
			}

			/* 移到外侧的下一个列表值。 */
			outer_pos++;
		}
		else
		{
			/* 外层缺少的一个列表值。 */
			Assert(cmpval > 0);
			Assert(inner_pos < inner_bi->ndatums);

			/*
			 * 若外侧有默认分区，或这是 FULL
			 * 连接，就试着给内侧分区分配一个合并后的分区
			 * （见 process_inner_partition()）。否则，内侧
			 * 分区不会对结果有贡献。
			 */
			if (outer_has_default || jointype == JOIN_FULL)
			{
				/* 取得内侧分区。 */
				inner_index = inner_bi->indexes[inner_pos];
				Assert(inner_index >= 0);
				merged_index = process_inner_partition(&outer_map,
													   &inner_map,
													   outer_has_default,
													   inner_has_default,
													   inner_index,
													   outer_default,
													   jointype,
													   &next_index,
													   &default_index);
				if (merged_index == -1)
					goto cleanup;
				merged_datum = inner_datums;
			}

			/* 移到内侧的下一个列表值。 */
			inner_pos++;
		}

		/*
		 * 若已分配合并后的分区，就在适当时候加入列表值以及
		 * 该合并后分区的下标。
		 */
		if (merged_index >= 0 && merged_index != default_index)
		{
			merged_datums = lappend(merged_datums, merged_datum);
			merged_indexes = lappend_int(merged_indexes, merged_index);
		}
	}

	/*
	 * 若 NULL 分区（若有）已被证明为空，就当作它们
	 * 不存在。
	 */
	if (outer_has_null &&
		is_dummy_partition(outer_rel, outer_bi->null_index))
		outer_has_null = false;
	if (inner_has_null &&
		is_dummy_partition(inner_rel, inner_bi->null_index))
		inner_has_null = false;

	/* 若有 NULL 分区，就合并它们。 */
	if (outer_has_null || inner_has_null)
		merge_null_partitions(&outer_map, &inner_map,
							  outer_has_null, inner_has_null,
							  outer_bi->null_index, inner_bi->null_index,
							  jointype, &next_index, &null_index);
	else
		Assert(null_index == -1);

	/* 若有默认分区，就合并它们。 */
	if (outer_has_default || inner_has_default)
		merge_default_partitions(&outer_map, &inner_map,
								 outer_has_default, inner_has_default,
								 outer_default, inner_default,
								 jointype, &next_index, &default_index);
	else
		Assert(default_index == -1);

	/* 若已有合并后的分区，就创建分区边界。 */
	if (next_index > 0)
	{
		/* 必要时修正 merged_indexes 列表。 */
		if (outer_map.did_remapping || inner_map.did_remapping)
		{
			Assert(jointype == JOIN_FULL);
			fix_merged_indexes(&outer_map, &inner_map,
							   next_index, merged_indexes);
		}

		/* 用映射去匹配输入的分区。 */
		generate_matching_part_pairs(outer_rel, inner_rel,
									 &outer_map, &inner_map,
									 next_index,
									 outer_parts, inner_parts);
		Assert(*outer_parts != NIL);
		Assert(*inner_parts != NIL);
		Assert(list_length(*outer_parts) == list_length(*inner_parts));
		Assert(list_length(*outer_parts) <= next_index);

		/* 构造一个 PartitionBoundInfo 结构以便返回。 */
		merged_bounds = build_merged_partition_bounds(outer_bi->strategy,
													  merged_datums,
													  NIL,
													  merged_indexes,
													  null_index,
													  default_index);
		Assert(merged_bounds);
	}

cleanup:
	/* 返回前释放局部内存。 */
	list_free(merged_datums);
	list_free(merged_indexes);
	free_partition_map(&outer_map);
	free_partition_map(&inner_map);

	return merged_bounds;
}

/*
 * merge_range_bounds
 *		若可能，为区间
 *		分区表之间的连接关系创建分区边界。
 *
 * 本函数试图通过比较分区边界里存放的区间，
 * 找出两侧重叠的分区集合。因为
 * 区间按升序出现，这里用类似归并连接的
 * 算法。
 * 若一侧的分区在另一侧没有重叠
 * 分区，算法就试着把它配上另一侧的
 * 默认分区（若有）；若没有，且它在
 * 外连接的非空一侧，算法就试着把它配上
 * 另一侧的哑分区。另外，若两侧都有默认
 * 分区，算法就试着让它们彼此匹配。若算法发现
 * 一个分区重叠另一侧的多个分区，我们就放弃，
 * 这是当前分区连接实现没法处理的情形。
 */
static PartitionBoundInfo
merge_range_bounds(int partnatts, FmgrInfo *partsupfuncs,
				   Oid *partcollations,
				   RelOptInfo *outer_rel, RelOptInfo *inner_rel,
				   JoinType jointype,
				   List **outer_parts, List **inner_parts)
{
	PartitionBoundInfo merged_bounds = NULL;
	PartitionBoundInfo outer_bi = outer_rel->boundinfo;
	PartitionBoundInfo inner_bi = inner_rel->boundinfo;
	bool		outer_has_default = partition_bound_has_default(outer_bi);
	bool		inner_has_default = partition_bound_has_default(inner_bi);
	int			outer_default = outer_bi->default_index;
	int			inner_default = inner_bi->default_index;
	PartitionMap outer_map;
	PartitionMap inner_map;
	int			outer_index;
	int			inner_index;
	int			outer_lb_pos;
	int			inner_lb_pos;
	PartitionRangeBound outer_lb;
	PartitionRangeBound outer_ub;
	PartitionRangeBound inner_lb;
	PartitionRangeBound inner_ub;
	int			next_index = 0;
	int			default_index = -1;
	List	   *merged_datums = NIL;
	List	   *merged_kinds = NIL;
	List	   *merged_indexes = NIL;

	Assert(*outer_parts == NIL);
	Assert(*inner_parts == NIL);
	Assert(outer_bi->strategy == inner_bi->strategy &&
		   outer_bi->strategy == PARTITION_STRATEGY_RANGE);

	init_partition_map(outer_rel, &outer_map);
	init_partition_map(inner_rel, &inner_map);

	/*
	 * 若默认分区（若有）已被证明为空，就当作它们
	 * 不存在。
	 */
	if (outer_has_default && is_dummy_partition(outer_rel, outer_default))
		outer_has_default = false;
	if (inner_has_default && is_dummy_partition(inner_rel, inner_default))
		inner_has_default = false;

	/*
	 * 合并两侧的分区。每次迭代比较一对
	 * 区间，每侧一个，并决定对应
	 * 分区是否匹配。若两个区间重叠，就移到下一
	 * 对区间，否则移到区间更低那一侧的下一个区间。
	 * outer_lb_pos/inner_lb_pos 跟踪外/内
	 * PartitionBoundInfos 的 datums 数组里
	 * 下界的位置。
	 */
	outer_lb_pos = inner_lb_pos = 0;
	outer_index = get_range_partition(outer_rel, outer_bi, &outer_lb_pos,
									  &outer_lb, &outer_ub);
	inner_index = get_range_partition(inner_rel, inner_bi, &inner_lb_pos,
									  &inner_lb, &inner_ub);
	while (outer_index >= 0 || inner_index >= 0)
	{
		bool		overlap;
		int			ub_cmpval;
		int			lb_cmpval;
		PartitionRangeBound merged_lb = {-1, NULL, NULL, true};
		PartitionRangeBound merged_ub = {-1, NULL, NULL, false};
		int			merged_index = -1;

		/*
		 * 这个循环一直跑到两侧都结束。这样可以避免
		 * 为较晚结束那一侧剩余的区间重复写代码。
		 * 为此我们把比较参数 cmpval 设成看起来像
		 * 较早结束的那一侧还有一个额外区间，
		 * 比未结束一侧的任何区间都高。这样我们就推进
		 * 未结束一侧的区间，直到它的
		 * 全部区间都用完。
		 */
		if (outer_index == -1)
		{
			overlap = false;
			lb_cmpval = 1;
			ub_cmpval = 1;
		}
		else if (inner_index == -1)
		{
			overlap = false;
			lb_cmpval = -1;
			ub_cmpval = -1;
		}
		else
			overlap = compare_range_partitions(partnatts, partsupfuncs,
											   partcollations,
											   &outer_lb, &outer_ub,
											   &inner_lb, &inner_ub,
											   &lb_cmpval, &ub_cmpval);

		if (overlap)
		{
			/* 两个区间重叠；组成一个连接对。 */

			PartitionRangeBound save_outer_ub;
			PartitionRangeBound save_inner_ub;

			/* 两个分区都应当还没被合并。 */
			Assert(outer_index >= 0);
			Assert(outer_map.merged_indexes[outer_index] == -1 &&
				   outer_map.merged[outer_index] == false);
			Assert(inner_index >= 0);
			Assert(inner_map.merged_indexes[inner_index] == -1 &&
				   inner_map.merged[inner_index] == false);

			/*
			 * 取得合并后分区的下标。两个分区都还
			 * 没合并，因此应当能成功合并。
			 */
			merged_index = merge_matching_partitions(&outer_map, &inner_map,
													 outer_index, inner_index,
													 &next_index);
			Assert(merged_index >= 0);

			/* 取得合并后分区的区间边界。 */
			get_merged_range_bounds(partnatts, partsupfuncs,
									partcollations, jointype,
									&outer_lb, &outer_ub,
									&inner_lb, &inner_ub,
									lb_cmpval, ub_cmpval,
									&merged_lb, &merged_ub);

			/* 保存两个分区的上界，供下面使用。 */
			save_outer_ub = outer_ub;
			save_inner_ub = inner_ub;

			/* 移到下一对区间。 */
			outer_index = get_range_partition(outer_rel, outer_bi, &outer_lb_pos,
											  &outer_lb, &outer_ub);
			inner_index = get_range_partition(inner_rel, inner_bi, &inner_lb_pos,
											  &inner_lb, &inner_ub);

			/*
			 * 若一侧某分区的区间重叠另一侧
			 * 下一个分区的区间，就会使
			 * 一侧的分区至少匹配另一侧的两个分区，
			 * 这是目前不支持分区连接的情形；
			 * 放弃。
			 */
			if (ub_cmpval > 0 && inner_index >= 0 &&
				compare_range_bounds(partnatts, partsupfuncs, partcollations,
									 &save_outer_ub, &inner_lb) > 0)
				goto cleanup;
			if (ub_cmpval < 0 && outer_index >= 0 &&
				compare_range_bounds(partnatts, partsupfuncs, partcollations,
									 &outer_lb, &save_inner_ub) < 0)
				goto cleanup;

			/*
			 * 来自一侧分区不重叠部分（若有）的行
			 * 可能在另一侧的默认分区
			 * （若有）里找到连接伙伴，造成与上面相同的局面；
			 * 那种情况下放弃。
			 */
			if ((outer_has_default && (lb_cmpval > 0 || ub_cmpval < 0)) ||
				(inner_has_default && (lb_cmpval < 0 || ub_cmpval > 0)))
				goto cleanup;
		}
		else if (ub_cmpval < 0)
		{
			/* 一个不重叠的外侧区间。 */

			/* 外侧分区应当还没被合并。 */
			Assert(outer_index >= 0);
			Assert(outer_map.merged_indexes[outer_index] == -1 &&
				   outer_map.merged[outer_index] == false);

			/*
			 * 若内侧有默认分区，或这是
			 * 外连接，就试着给外侧分区分配一个合并后的分区
			 * （见 process_outer_partition()）。否则，外侧
			 * 分区不会对结果有贡献。
			 */
			if (inner_has_default || IS_OUTER_JOIN(jointype))
			{
				merged_index = process_outer_partition(&outer_map,
													   &inner_map,
													   outer_has_default,
													   inner_has_default,
													   outer_index,
													   inner_default,
													   jointype,
													   &next_index,
													   &default_index);
				if (merged_index == -1)
					goto cleanup;
				merged_lb = outer_lb;
				merged_ub = outer_ub;
			}

			/* 移到外侧的下一个区间。 */
			outer_index = get_range_partition(outer_rel, outer_bi, &outer_lb_pos,
											  &outer_lb, &outer_ub);
		}
		else
		{
			/* 一个不重叠的内侧区间。 */
			Assert(ub_cmpval > 0);

			/* 内侧分区应当还没被合并。 */
			Assert(inner_index >= 0);
			Assert(inner_map.merged_indexes[inner_index] == -1 &&
				   inner_map.merged[inner_index] == false);

			/*
			 * 若外侧有默认分区，或这是 FULL
			 * 连接，就试着给内侧分区分配一个合并后的分区
			 * （见 process_inner_partition()）。否则，内侧
			 * 分区不会对结果有贡献。
			 */
			if (outer_has_default || jointype == JOIN_FULL)
			{
				merged_index = process_inner_partition(&outer_map,
													   &inner_map,
													   outer_has_default,
													   inner_has_default,
													   inner_index,
													   outer_default,
													   jointype,
													   &next_index,
													   &default_index);
				if (merged_index == -1)
					goto cleanup;
				merged_lb = inner_lb;
				merged_ub = inner_ub;
			}

			/* 移到内侧的下一个区间。 */
			inner_index = get_range_partition(inner_rel, inner_bi, &inner_lb_pos,
											  &inner_lb, &inner_ub);
		}

		/*
		 * 若已分配合并后的分区，就在适当时候加入区间边界以及
		 * 该合并后分区的下标。
		 */
		if (merged_index >= 0 && merged_index != default_index)
			add_merged_range_bounds(partnatts, partsupfuncs, partcollations,
									&merged_lb, &merged_ub, merged_index,
									&merged_datums, &merged_kinds,
									&merged_indexes);
	}

	/* 若有默认分区，就合并它们。 */
	if (outer_has_default || inner_has_default)
		merge_default_partitions(&outer_map, &inner_map,
								 outer_has_default, inner_has_default,
								 outer_default, inner_default,
								 jointype, &next_index, &default_index);
	else
		Assert(default_index == -1);

	/* 若已有合并后的分区，就创建分区边界。 */
	if (next_index > 0)
	{
		/*
		 * 与列表分区不同，我们不会重新合并
		 * 分区，因此 did_remapping 应当保持不动。
		 */
		Assert(!outer_map.did_remapping);
		Assert(!inner_map.did_remapping);

		/* 用映射去匹配输入的分区。 */
		generate_matching_part_pairs(outer_rel, inner_rel,
									 &outer_map, &inner_map,
									 next_index,
									 outer_parts, inner_parts);
		Assert(*outer_parts != NIL);
		Assert(*inner_parts != NIL);
		Assert(list_length(*outer_parts) == list_length(*inner_parts));
		Assert(list_length(*outer_parts) == next_index);

		/* 构造一个 PartitionBoundInfo 结构以便返回。 */
		merged_bounds = build_merged_partition_bounds(outer_bi->strategy,
													  merged_datums,
													  merged_kinds,
													  merged_indexes,
													  -1,
													  default_index);
		Assert(merged_bounds);
	}

cleanup:
	/* 返回前释放局部内存。 */
	list_free(merged_datums);
	list_free(merged_kinds);
	list_free(merged_indexes);
	free_partition_map(&outer_map);
	free_partition_map(&inner_map);

	return merged_bounds;
}

/*
 * init_partition_map
 *		为给定关系初始化 PartitionMap 结构
 */
static void
init_partition_map(RelOptInfo *rel, PartitionMap *map)
{
	int			nparts = rel->nparts;
	int			i;

	map->nparts = nparts;
	map->merged_indexes = (int *) palloc(sizeof(int) * nparts);
	map->merged = (bool *) palloc(sizeof(bool) * nparts);
	map->did_remapping = false;
	map->old_indexes = (int *) palloc(sizeof(int) * nparts);
	for (i = 0; i < nparts; i++)
	{
		map->merged_indexes[i] = map->old_indexes[i] = -1;
		map->merged[i] = false;
	}
}

/*
 * free_partition_map：释放分区映射
 */
static void
free_partition_map(PartitionMap *map)
{
	pfree(map->merged_indexes);
	pfree(map->merged);
	pfree(map->old_indexes);
}

/*
 * is_dummy_partition：分区是否已被证明为空？
 */
static bool
is_dummy_partition(RelOptInfo *rel, int part_index)
{
	RelOptInfo *part_rel;

	Assert(part_index >= 0);
	part_rel = rel->part_rels[part_index];
	if (part_rel == NULL || IS_DUMMY_REL(part_rel))
		return true;
	return false;
}

/*
 * merge_matching_partitions
 *		尝试合并给定的外/内分区，成功时返回由它们产生的
 *		合并后分区的下标，否则返回 -1。
 *
 * 若合并后的分区是新创建的，*next_index 就递增。
 */
static int
merge_matching_partitions(PartitionMap *outer_map, PartitionMap *inner_map,
						  int outer_index, int inner_index, int *next_index)
{
	int			outer_merged_index;
	int			inner_merged_index;
	bool		outer_merged;
	bool		inner_merged;

	Assert(outer_index >= 0 && outer_index < outer_map->nparts);
	outer_merged_index = outer_map->merged_indexes[outer_index];
	outer_merged = outer_map->merged[outer_index];
	Assert(inner_index >= 0 && inner_index < inner_map->nparts);
	inner_merged_index = inner_map->merged_indexes[inner_index];
	inner_merged = inner_map->merged[inner_index];

	/*
	 * 处理已经给每个
	 * 给定分区分配了合并后分区的情形。
	 */
	if (outer_merged_index >= 0 && inner_merged_index >= 0)
	{
		/*
		 * 若合并后的分区相同，就不必做任何事；
		 * 返回这些合并后分区的下标。否则，若每个
		 * 给定分区都已与另一侧的哑分区合并，
		 * 就把它们重新映射到这两个合并后分区之一。
		 * 否则它们没法合并，因此返回 -1。
		 */
		if (outer_merged_index == inner_merged_index)
		{
			Assert(outer_merged);
			Assert(inner_merged);
			return outer_merged_index;
		}
		if (!outer_merged && !inner_merged)
		{
			/*
			 * 这只可能发生在列表分区的情形。我们把它们
			 * 重新映射到两个合并下标中较小的那个合并后分区，
			 * 以保持这条性质：列表分区的规范次序
			 * 由赋给每个分区
			 * 最小列表值的下标决定。
			 */
			if (outer_merged_index < inner_merged_index)
			{
				outer_map->merged[outer_index] = true;
				inner_map->merged_indexes[inner_index] = outer_merged_index;
				inner_map->merged[inner_index] = true;
				inner_map->did_remapping = true;
				inner_map->old_indexes[inner_index] = inner_merged_index;
				return outer_merged_index;
			}
			else
			{
				inner_map->merged[inner_index] = true;
				outer_map->merged_indexes[outer_index] = inner_merged_index;
				outer_map->merged[outer_index] = true;
				outer_map->did_remapping = true;
				outer_map->old_indexes[outer_index] = outer_merged_index;
				return inner_merged_index;
			}
		}
		return -1;
	}

	/* 给定分区中至少有一个应当还没被合并。 */
	Assert(outer_merged_index == -1 || inner_merged_index == -1);

	/*
	 * 若两者都还没合并，就合并它们。否则，若其中一个已
	 * 与另一侧的哑分区合并（而另一个
	 * 还没与任何东西合并），就重新合并它们。否则它们
	 * 没法合并，因此返回 -1。
	 */
	if (outer_merged_index == -1 && inner_merged_index == -1)
	{
		int			merged_index = *next_index;

		Assert(!outer_merged);
		Assert(!inner_merged);
		outer_map->merged_indexes[outer_index] = merged_index;
		outer_map->merged[outer_index] = true;
		inner_map->merged_indexes[inner_index] = merged_index;
		inner_map->merged[inner_index] = true;
		*next_index = *next_index + 1;
		return merged_index;
	}
	if (outer_merged_index >= 0 && !outer_map->merged[outer_index])
	{
		Assert(inner_merged_index == -1);
		Assert(!inner_merged);
		inner_map->merged_indexes[inner_index] = outer_merged_index;
		inner_map->merged[inner_index] = true;
		outer_map->merged[outer_index] = true;
		return outer_merged_index;
	}
	if (inner_merged_index >= 0 && !inner_map->merged[inner_index])
	{
		Assert(outer_merged_index == -1);
		Assert(!outer_merged);
		outer_map->merged_indexes[outer_index] = inner_merged_index;
		outer_map->merged[outer_index] = true;
		inner_map->merged[inner_index] = true;
		return inner_merged_index;
	}
	return -1;
}

/*
 * process_outer_partition
 *		试着给给定的外侧分区分配一个合并后的分区，成功时返回
 *		该合并后分区的下标，否则返回 -1。
 *
 * 若该分区是新创建的，*next_index 就递增。另外，若它
 * 是连接关系的默认分区，且尚未设置，就把 *default_index
 * 设为该下标。
 */
static int
process_outer_partition(PartitionMap *outer_map,
						PartitionMap *inner_map,
						bool outer_has_default,
						bool inner_has_default,
						int outer_index,
						int inner_default,
						JoinType jointype,
						int *next_index,
						int *default_index)
{
	int			merged_index = -1;

	Assert(outer_index >= 0);

	/*
	 * 若内侧有默认分区，来自外侧
	 * 分区的行可能在默认分区里找到连接伙伴；试着
	 * 把外侧分区与默认分区合并。否则，
	 * 这应当是外连接，那种情况下外侧分区反正
	 * 必须全程扫描；把外侧分区与另一侧的哑
	 * 分区合并。
	 */
	if (inner_has_default)
	{
		Assert(inner_default >= 0);

		/*
		 * 若外侧也有默认分区，内侧的默认
		 * 分区在另一侧就会有两个匹配分区：
		 * 外侧分区，以及外侧的
		 * 默认分区。分区连接还不能处理
		 * 这种情形。
		 */
		if (outer_has_default)
			return -1;

		merged_index = merge_matching_partitions(outer_map, inner_map,
												 outer_index, inner_default,
												 next_index);
		if (merged_index == -1)
			return -1;

		/*
		 * 若这是 FULL 连接，内侧的默认分区反正
		 * 必须全程扫描，因此得到的分区将
		 * 包含默认分区的全部键值，而连接关系的任何其他
		 * 分区都不会包含这些值。因此
		 * 得到的分区将充当连接
		 * 关系的默认分区；若尚未记录，就把下标记在 *default_index 里。
		 */
		if (jointype == JOIN_FULL)
		{
			if (*default_index == -1)
				*default_index = merged_index;
			else
				Assert(*default_index == merged_index);
		}
	}
	else
	{
		Assert(IS_OUTER_JOIN(jointype));
		Assert(jointype != JOIN_RIGHT);

		/* 若已经分配了分区，就不必做任何事。 */
		merged_index = outer_map->merged_indexes[outer_index];
		if (merged_index == -1)
			merged_index = merge_partition_with_dummy(outer_map, outer_index,
													  next_index);
	}
	return merged_index;
}

/*
 * process_inner_partition
 *		试着给给定的内侧分区分配一个合并后的分区，成功时返回
 *		该合并后分区的下标，否则返回 -1。
 *
 * 若该分区是新创建的，*next_index 就递增。另外，若它
 * 是连接关系的默认分区，且尚未设置，就把 *default_index
 * 设为该下标。
 */
static int
process_inner_partition(PartitionMap *outer_map,
						PartitionMap *inner_map,
						bool outer_has_default,
						bool inner_has_default,
						int inner_index,
						int outer_default,
						JoinType jointype,
						int *next_index,
						int *default_index)
{
	int			merged_index = -1;

	Assert(inner_index >= 0);

	/*
	 * 若外侧有默认分区，来自内侧
	 * 分区的行可能在默认分区里找到连接伙伴；试着
	 * 把内侧分区与默认分区合并。否则，
	 * 这应当是 FULL 连接，那种情况下内侧分区反正必须
	 * 全程扫描；把内侧分区与另一侧的哑
	 * 分区合并。
	 */
	if (outer_has_default)
	{
		Assert(outer_default >= 0);

		/*
		 * 若内侧也有默认分区，外侧的默认
		 * 分区在另一侧就会有两个匹配分区：
		 * 内侧分区，以及内侧的
		 * 默认分区。分区连接还不能处理
		 * 这种情形。
		 */
		if (inner_has_default)
			return -1;

		merged_index = merge_matching_partitions(outer_map, inner_map,
												 outer_default, inner_index,
												 next_index);
		if (merged_index == -1)
			return -1;

		/*
		 * 若这是外连接，外侧的默认分区
		 * 反正必须全程扫描，因此得到的分区
		 * 将包含默认分区的全部键值，而连接关系的任何
		 * 其他分区都不会包含这些值。因此
		 * 得到的分区将充当连接
		 * 关系的默认分区；若尚未记录，就把下标记在 *default_index 里。
		 */
		if (IS_OUTER_JOIN(jointype))
		{
			Assert(jointype != JOIN_RIGHT);
			if (*default_index == -1)
				*default_index = merged_index;
			else
				Assert(*default_index == merged_index);
		}
	}
	else
	{
		Assert(jointype == JOIN_FULL);

		/* 若已经分配了分区，就不必做任何事。 */
		merged_index = inner_map->merged_indexes[inner_index];
		if (merged_index == -1)
			merged_index = merge_partition_with_dummy(inner_map, inner_index,
													  next_index);
	}
	return merged_index;
}

/*
 * merge_null_partitions
 *		合并连接外侧和内侧的 NULL 分区。
 *
 * 若由它们产生的合并分区是连接
 * 关系的 NULL 分区，就把 *null_index 设为该合并分区的下标。
 *
 * 注意：这里假定分区连接的连接子句是严格的，
 * 因为 have_partkey_equi_join() 需要对应操作符
 * 可归并连接，而我们目前假定可归并连接的操作符是
 * 严格的（见 MJEvalOuterValues()/MJEvalInnerValues()）。
 */
static void
merge_null_partitions(PartitionMap *outer_map,
					  PartitionMap *inner_map,
					  bool outer_has_null,
					  bool inner_has_null,
					  int outer_null,
					  int inner_null,
					  JoinType jointype,
					  int *next_index,
					  int *null_index)
{
	bool		consider_outer_null = false;
	bool		consider_inner_null = false;

	Assert(outer_has_null || inner_has_null);
	Assert(*null_index == -1);

	/*
	 * 检查 NULL 分区是否已经合并，若是，
	 * 就设置 consider_outer_null/consider_inner_null 标志。
	 */
	if (outer_has_null)
	{
		Assert(outer_null >= 0 && outer_null < outer_map->nparts);
		if (outer_map->merged_indexes[outer_null] == -1)
			consider_outer_null = true;
	}
	if (inner_has_null)
	{
		Assert(inner_null >= 0 && inner_null < inner_map->nparts);
		if (inner_map->merged_indexes[inner_null] == -1)
			consider_inner_null = true;
	}

	/* 若两个标志都设为假，就不必做任何事。 */
	if (!consider_outer_null && !consider_inner_null)
		return;

	if (consider_outer_null && !consider_inner_null)
	{
		Assert(outer_has_null);

		/*
		 * 若这是外连接，外侧的 NULL 分区反正
		 * 必须全程扫描；把该 NULL 分区与另一侧的
		 * 哑分区合并。那种情况下
		 * consider_outer_null 表示该 NULL 分区的键值只含
		 * NULL，因此合并后的分区也会如此；
		 * 把它当作连接关系的 NULL 分区。
		 */
		if (IS_OUTER_JOIN(jointype))
		{
			Assert(jointype != JOIN_RIGHT);
			*null_index = merge_partition_with_dummy(outer_map, outer_null,
													 next_index);
		}
	}
	else if (!consider_outer_null && consider_inner_null)
	{
		Assert(inner_has_null);

		/*
		 * 若这是 FULL 连接，内侧的 NULL 分区反正必须
		 * 全程扫描；把该 NULL 分区与另一侧的
		 * 哑分区合并。那种情况下
		 * consider_inner_null 表示该 NULL 分区的键值只含
		 * NULL，因此合并后的分区也会如此；
		 * 把它当作连接关系的 NULL 分区。
		 */
		if (jointype == JOIN_FULL)
			*null_index = merge_partition_with_dummy(inner_map, inner_null,
													 next_index);
	}
	else
	{
		Assert(consider_outer_null && consider_inner_null);
		Assert(outer_has_null);
		Assert(inner_has_null);

		/*
		 * 若这是外连接，外侧的 NULL 分区（以及
		 * 若这是 FULL 连接时内侧的那个）反正必须
		 * 全程扫描，因此把它们合并。注意每个 NULL
		 * 分区都还没合并，所以应当能成功合并。
		 * 同上，每个 NULL 分区的键值只含 NULL
		 * 值，因此合并后的分区也会如此；把它
		 * 当作连接关系的 NULL 分区。
		 *
		 * 注意：若这是 INNER/SEMI 连接，连接子句永远不会被
		 * 两个 NULL 值满足（见上面的注释），因此两个 NULL
		 * 分区都可以去掉。
		 */
		if (IS_OUTER_JOIN(jointype))
		{
			Assert(jointype != JOIN_RIGHT);
			*null_index = merge_matching_partitions(outer_map, inner_map,
													outer_null, inner_null,
													next_index);
			Assert(*null_index >= 0);
		}
	}
}

/*
 * merge_default_partitions
 *		合并连接外侧和内侧的默认分区。
 *
 * 若由它们产生的合并分区是连接
 * 关系的默认分区，就把 *default_index 设为该合并分区的下标。
 */
static void
merge_default_partitions(PartitionMap *outer_map,
						 PartitionMap *inner_map,
						 bool outer_has_default,
						 bool inner_has_default,
						 int outer_default,
						 int inner_default,
						 JoinType jointype,
						 int *next_index,
						 int *default_index)
{
	int			outer_merged_index = -1;
	int			inner_merged_index = -1;

	Assert(outer_has_default || inner_has_default);

	/* 取得默认分区的合并后分区下标。 */
	if (outer_has_default)
	{
		Assert(outer_default >= 0 && outer_default < outer_map->nparts);
		outer_merged_index = outer_map->merged_indexes[outer_default];
	}
	if (inner_has_default)
	{
		Assert(inner_default >= 0 && inner_default < inner_map->nparts);
		inner_merged_index = inner_map->merged_indexes[inner_default];
	}

	if (outer_has_default && !inner_has_default)
	{
		/*
		 * 若这是外连接，外侧的默认分区
		 * 反正必须全程扫描；若还没有分配
		 * 分区，就把默认分区与另一侧的哑分区
		 * 合并。合并后的分区将充当默认
		 * 分区（见
		 * process_inner_partition() 里的注释）。
		 */
		if (IS_OUTER_JOIN(jointype))
		{
			Assert(jointype != JOIN_RIGHT);
			if (outer_merged_index == -1)
			{
				Assert(*default_index == -1);
				*default_index = merge_partition_with_dummy(outer_map,
															outer_default,
															next_index);
			}
			else
				Assert(*default_index == outer_merged_index);
		}
		else
			Assert(*default_index == -1);
	}
	else if (!outer_has_default && inner_has_default)
	{
		/*
		 * 若这是 FULL 连接，内侧的默认分区反正
		 * 必须全程扫描；若还没有分配
		 * 分区，就把默认分区与另一侧的哑分区
		 * 合并。合并后的分区将充当默认
		 * 分区（见
		 * process_outer_partition() 里的注释）。
		 */
		if (jointype == JOIN_FULL)
		{
			if (inner_merged_index == -1)
			{
				Assert(*default_index == -1);
				*default_index = merge_partition_with_dummy(inner_map,
															inner_default,
															next_index);
			}
			else
				Assert(*default_index == inner_merged_index);
		}
		else
			Assert(*default_index == -1);
	}
	else
	{
		Assert(outer_has_default && inner_has_default);

		/*
		 * 默认分区必须彼此连接，因此把它们
		 * 合并。注意每个默认分区都还没合并
		 * （见 process_outer_partition()/process_inner_partition()），所以它们
		 * 应当能成功合并。合并后的分区将充当
		 * 连接关系的默认分区。
		 */
		Assert(outer_merged_index == -1);
		Assert(inner_merged_index == -1);
		Assert(*default_index == -1);
		*default_index = merge_matching_partitions(outer_map,
												   inner_map,
												   outer_default,
												   inner_default,
												   next_index);
		Assert(*default_index >= 0);
	}
}

/*
 * merge_partition_with_dummy
 *		给给定分区分配连接关系的一个新分区。
 *
 * 注意：调用方假定给定分区在另一侧没有非哑的
 * 匹配分区，但若该分区后来找到了
 * 匹配分区，我们会调整这次分配。
 */
static int
merge_partition_with_dummy(PartitionMap *map, int index, int *next_index)
{
	int			merged_index = *next_index;

	Assert(index >= 0 && index < map->nparts);
	Assert(map->merged_indexes[index] == -1);
	Assert(!map->merged[index]);
	map->merged_indexes[index] = merged_index;
	/* 别动合并标志！ */
	*next_index = *next_index + 1;
	return merged_index;
}

/*
 * fix_merged_indexes
 *		调整重新合并的分区的合并下标
 */
static void
fix_merged_indexes(PartitionMap *outer_map, PartitionMap *inner_map,
				   int nmerged, List *merged_indexes)
{
	int		   *new_indexes;
	int			merged_index;
	int			i;
	ListCell   *lc;

	Assert(nmerged > 0);

	new_indexes = (int *) palloc(sizeof(int) * nmerged);
	for (i = 0; i < nmerged; i++)
		new_indexes[i] = -1;

	/* 建立旧合并下标到新合并下标的映射。 */
	if (outer_map->did_remapping)
	{
		for (i = 0; i < outer_map->nparts; i++)
		{
			merged_index = outer_map->old_indexes[i];
			if (merged_index >= 0)
				new_indexes[merged_index] = outer_map->merged_indexes[i];
		}
	}
	if (inner_map->did_remapping)
	{
		for (i = 0; i < inner_map->nparts; i++)
		{
			merged_index = inner_map->old_indexes[i];
			if (merged_index >= 0)
				new_indexes[merged_index] = inner_map->merged_indexes[i];
		}
	}

	/* 用该映射修正 merged_indexes 列表。 */
	foreach(lc, merged_indexes)
	{
		merged_index = lfirst_int(lc);
		Assert(merged_index >= 0);
		if (new_indexes[merged_index] >= 0)
			lfirst_int(lc) = new_indexes[merged_index];
	}

	pfree(new_indexes);
}

/*
 * generate_matching_part_pairs
 *		生成产生合并后分区的一对分区列表。
 *
 * 分区列表按合并后分区下标的次序建立，
 * 并返回到 *outer_parts 和 *inner_parts。
 */
static void
generate_matching_part_pairs(RelOptInfo *outer_rel, RelOptInfo *inner_rel,
							 PartitionMap *outer_map, PartitionMap *inner_map,
							 int nmerged,
							 List **outer_parts, List **inner_parts)
{
	int			outer_nparts = outer_map->nparts;
	int			inner_nparts = inner_map->nparts;
	int		   *outer_indexes;
	int		   *inner_indexes;
	int			max_nparts;
	int			i;

	Assert(nmerged > 0);
	Assert(*outer_parts == NIL);
	Assert(*inner_parts == NIL);

	outer_indexes = (int *) palloc(sizeof(int) * nmerged);
	inner_indexes = (int *) palloc(sizeof(int) * nmerged);
	for (i = 0; i < nmerged; i++)
		outer_indexes[i] = inner_indexes[i] = -1;

	/* 设置匹配分区对。 */
	Assert(outer_nparts == outer_rel->nparts);
	Assert(inner_nparts == inner_rel->nparts);
	max_nparts = Max(outer_nparts, inner_nparts);
	for (i = 0; i < max_nparts; i++)
	{
		if (i < outer_nparts)
		{
			int			merged_index = outer_map->merged_indexes[i];

			if (merged_index >= 0)
			{
				Assert(merged_index < nmerged);
				outer_indexes[merged_index] = i;
			}
		}
		if (i < inner_nparts)
		{
			int			merged_index = inner_map->merged_indexes[i];

			if (merged_index >= 0)
			{
				Assert(merged_index < nmerged);
				inner_indexes[merged_index] = i;
			}
		}
	}

	/* 建立列表对。 */
	for (i = 0; i < nmerged; i++)
	{
		int			outer_index = outer_indexes[i];
		int			inner_index = inner_indexes[i];

		/*
		 * 若两个分区都是哑的，就表示曾经分配给外/内分区的
		 * 合并后分区，在
		 * merge_matching_partitions() 里重新合并外/内分区时被去掉了；
		 * 忽略该合并后分区。
		 */
		if (outer_index == -1 && inner_index == -1)
			continue;

		*outer_parts = lappend(*outer_parts, outer_index >= 0 ?
							   outer_rel->part_rels[outer_index] : NULL);
		*inner_parts = lappend(*inner_parts, inner_index >= 0 ?
							   inner_rel->part_rels[inner_index] : NULL);
	}

	pfree(outer_indexes);
	pfree(inner_indexes);
}

/*
 * build_merged_partition_bounds
 *		从合并后的分区边界创建 PartitionBoundInfo 结构
 */
static PartitionBoundInfo
build_merged_partition_bounds(char strategy, List *merged_datums,
							  List *merged_kinds, List *merged_indexes,
							  int null_index, int default_index)
{
	PartitionBoundInfo merged_bounds;
	int			ndatums = list_length(merged_datums);
	int			pos;
	ListCell   *lc;

	merged_bounds = (PartitionBoundInfo) palloc(sizeof(PartitionBoundInfoData));
	merged_bounds->strategy = strategy;
	merged_bounds->ndatums = ndatums;

	merged_bounds->datums = (Datum **) palloc(sizeof(Datum *) * ndatums);
	pos = 0;
	foreach(lc, merged_datums)
		merged_bounds->datums[pos++] = (Datum *) lfirst(lc);

	if (strategy == PARTITION_STRATEGY_RANGE)
	{
		Assert(list_length(merged_kinds) == ndatums);
		merged_bounds->kind = (PartitionRangeDatumKind **)
			palloc(sizeof(PartitionRangeDatumKind *) * ndatums);
		pos = 0;
		foreach(lc, merged_kinds)
			merged_bounds->kind[pos++] = (PartitionRangeDatumKind *) lfirst(lc);

		/* 区间分区时有 ndatums+1 个下标。 */
		merged_indexes = lappend_int(merged_indexes, -1);
		ndatums++;
	}
	else
	{
		Assert(strategy == PARTITION_STRATEGY_LIST);
		Assert(merged_kinds == NIL);
		merged_bounds->kind = NULL;
	}

	/* 对连接关系，interleaved_parts 始终为 NULL。 */
	merged_bounds->interleaved_parts = NULL;

	Assert(list_length(merged_indexes) == ndatums);
	merged_bounds->nindexes = ndatums;
	merged_bounds->indexes = (int *) palloc(sizeof(int) * ndatums);
	pos = 0;
	foreach(lc, merged_indexes)
		merged_bounds->indexes[pos++] = lfirst_int(lc);

	merged_bounds->null_index = null_index;
	merged_bounds->default_index = default_index;

	return merged_bounds;
}

/*
 * get_range_partition
 *		取得区间分区关系的下一个非哑分区，
 *		返回该分区的下标。
 *
 * *lb 和 *ub 分别设为该分区的下界和上界，
 * 若还有下一个下界，*lb_pos 就前进到它。
 */
static int
get_range_partition(RelOptInfo *rel,
					PartitionBoundInfo bi,
					int *lb_pos,
					PartitionRangeBound *lb,
					PartitionRangeBound *ub)
{
	int			part_index;

	Assert(bi->strategy == PARTITION_STRATEGY_RANGE);

	do
	{
		part_index = get_range_partition_internal(bi, lb_pos, lb, ub);
		if (part_index == -1)
			return -1;
	} while (is_dummy_partition(rel, part_index));

	return part_index;
}

static int
get_range_partition_internal(PartitionBoundInfo bi,
							 int *lb_pos,
							 PartitionRangeBound *lb,
							 PartitionRangeBound *ub)
{
	/* 若下界已用完，就把下标作为 -1 返回。 */
	if (*lb_pos >= bi->ndatums)
		return -1;

	/* 下界之后至少还应有一个边界。 */
	Assert(*lb_pos + 1 < bi->ndatums);

	/* 设置下界。 */
	lb->index = bi->indexes[*lb_pos];
	lb->datums = bi->datums[*lb_pos];
	lb->kind = bi->kind[*lb_pos];
	lb->lower = true;
	/* 设置上界。 */
	ub->index = bi->indexes[*lb_pos + 1];
	ub->datums = bi->datums[*lb_pos + 1];
	ub->kind = bi->kind[*lb_pos + 1];
	ub->lower = false;

	/* 赋给上界的下标应当有效。 */
	Assert(ub->index >= 0);

	/*
	 * 把位置前进到下一个下界。若上界之外已经没有边界，
	 * 我们就到达了最后一个下界。
	 */
	if (*lb_pos + 2 >= bi->ndatums)
		*lb_pos = bi->ndatums;
	else
	{
		/*
		 * 若紧挨上界的下一个边界所赋的下标
		 * 无效，那就是下一个下界；否则，该上界同时也是
		 * 下一个区间分区的下界。
		 */
		if (bi->indexes[*lb_pos + 2] < 0)
			*lb_pos = *lb_pos + 2;
		else
			*lb_pos = *lb_pos + 1;
	}

	return ub->index;
}

/*
 * compare_range_partitions
 *		比较两个区间分区的边界，若两个分区
 *		重叠就返回真，否则返回假。
 *
 * *lb_cmpval 在外侧分区的下界
 * 低于、等于或高于内侧分区的下界时分别设为 -1、0 或 1。
 * 同样，*ub_cmpval 在外侧
 * 分区的上界低于、等于或高于内侧
 * 分区的上界时分别设为 -1、0 或 1。
 */
static bool
compare_range_partitions(int partnatts, FmgrInfo *partsupfuncs,
						 Oid *partcollations,
						 PartitionRangeBound *outer_lb,
						 PartitionRangeBound *outer_ub,
						 PartitionRangeBound *inner_lb,
						 PartitionRangeBound *inner_ub,
						 int *lb_cmpval, int *ub_cmpval)
{
	/*
	 * 检查外侧分区的上界是否低于内侧
	 * 分区的下界；若是，两个分区就不重叠。
	 */
	if (compare_range_bounds(partnatts, partsupfuncs, partcollations,
							 outer_ub, inner_lb) < 0)
	{
		*lb_cmpval = -1;
		*ub_cmpval = -1;
		return false;
	}

	/*
	 * 检查外侧分区的下界是否高于内侧
	 * 分区的上界；若是，两个分区就不重叠。
	 */
	if (compare_range_bounds(partnatts, partsupfuncs, partcollations,
							 outer_lb, inner_ub) > 0)
	{
		*lb_cmpval = 1;
		*ub_cmpval = 1;
		return false;
	}

	/* 其他情形都表示分区重叠。 */
	*lb_cmpval = compare_range_bounds(partnatts, partsupfuncs, partcollations,
									  outer_lb, inner_lb);
	*ub_cmpval = compare_range_bounds(partnatts, partsupfuncs, partcollations,
									  outer_ub, inner_ub);
	return true;
}

/*
 * get_merged_range_bounds
 *		给定要连接的区间分区的边界，确定由这些区间分区产生的
 *		合并分区的边界。
 *
 * *merged_lb 和 *merged_ub 分别设为该
 * 合并分区的下界和上界。
 */
static void
get_merged_range_bounds(int partnatts, FmgrInfo *partsupfuncs,
						Oid *partcollations, JoinType jointype,
						PartitionRangeBound *outer_lb,
						PartitionRangeBound *outer_ub,
						PartitionRangeBound *inner_lb,
						PartitionRangeBound *inner_ub,
						int lb_cmpval, int ub_cmpval,
						PartitionRangeBound *merged_lb,
						PartitionRangeBound *merged_ub)
{
	Assert(compare_range_bounds(partnatts, partsupfuncs, partcollations,
								outer_lb, inner_lb) == lb_cmpval);
	Assert(compare_range_bounds(partnatts, partsupfuncs, partcollations,
								outer_ub, inner_ub) == ub_cmpval);

	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_SEMI:

			/*
			 * INNER/SEMI 连接会有同时适合两侧的行，因此
			 * 合并分区的下界将是两个下界中较高的那个，
			 * 合并
			 * 分区的上界将是两个上界中较低的那个。
			 */
			*merged_lb = (lb_cmpval > 0) ? *outer_lb : *inner_lb;
			*merged_ub = (ub_cmpval < 0) ? *outer_ub : *inner_ub;
			break;

		case JOIN_LEFT:
		case JOIN_ANTI:

			/*
			 * LEFT/ANTI 连接会有来自外侧的全部行，因此
			 * 合并分区的边界将与
			 * 外侧边界相同。
			 */
			*merged_lb = *outer_lb;
			*merged_ub = *outer_ub;
			break;

		case JOIN_FULL:

			/*
			 * FULL 连接会有来自两侧的全部行，因此
			 * 合并分区的下界将是两个
			 * 下界中较低的那个，合并分区的上界
			 * 将是两个上界中较高的那个。
			 */
			*merged_lb = (lb_cmpval < 0) ? *outer_lb : *inner_lb;
			*merged_ub = (ub_cmpval > 0) ? *outer_ub : *inner_ub;
			break;

		default:
			elog(ERROR, "unrecognized join type: %d", (int) jointype);
	}
}

/*
 * add_merged_range_bounds
 *		把合并分区的边界加入区间边界列表
 */
static void
add_merged_range_bounds(int partnatts, FmgrInfo *partsupfuncs,
						Oid *partcollations,
						PartitionRangeBound *merged_lb,
						PartitionRangeBound *merged_ub,
						int merged_index,
						List **merged_datums,
						List **merged_kinds,
						List **merged_indexes)
{
	int			cmpval;

	if (!*merged_datums)
	{
		/* 第一个合并分区 */
		Assert(!*merged_kinds);
		Assert(!*merged_indexes);
		cmpval = 1;
	}
	else
	{
		PartitionRangeBound prev_ub;

		Assert(*merged_datums);
		Assert(*merged_kinds);
		Assert(*merged_indexes);

		/* 取得最后一个上界。 */
		prev_ub.index = llast_int(*merged_indexes);
		prev_ub.datums = (Datum *) llast(*merged_datums);
		prev_ub.kind = (PartitionRangeDatumKind *) llast(*merged_kinds);
		prev_ub.lower = false;

		/*
		 * 我们向 partition_rbound_cmp() 传入 lower1 = false，
		 * 以免在两个区间边界的值比较相等时，
		 * 它把最后一个上界看成小于合并分区的
		 * 下界。
		 */
		cmpval = partition_rbound_cmp(partnatts, partsupfuncs, partcollations,
									  merged_lb->datums, merged_lb->kind,
									  false, &prev_ub);
		Assert(cmpval >= 0);
	}

	/*
	 * 若下界高于最后一个上界，就把下界
	 * 连同下标 -1 加入，表示那是下界；否则，
	 * 最后一个上界会被复用为合并
	 * 分区的下界，因此跳过这一步。
	 */
	if (cmpval > 0)
	{
		*merged_datums = lappend(*merged_datums, merged_lb->datums);
		*merged_kinds = lappend(*merged_kinds, merged_lb->kind);
		*merged_indexes = lappend_int(*merged_indexes, -1);
	}

	/* 加入合并分区的上界和下标。 */
	*merged_datums = lappend(*merged_datums, merged_ub->datums);
	*merged_kinds = lappend(*merged_kinds, merged_ub->kind);
	*merged_indexes = lappend_int(*merged_indexes, merged_index);
}

/*
 * partitions_are_ordered
 *		判断 boundinfo 所描述的分区是否有序，
 *		也就是 PartitionDesc 序列里较早出现的分区
 *		所含的分区键严格小于较晚出现的那些。
 *		另外，若可能有 NULL 值，它们必须出现在
 *		PartitionDesc 里定义的最后一个分区中。live_parts 标出检查次序时
 *		应当包含哪些分区。不出现在 live_parts 里的分区
 *		被忽略。
 *
 * 若次序不对，或信息不足以知道次序，
 * 就返回假。
 */
bool
partitions_are_ordered(PartitionBoundInfo boundinfo, Bitmapset *live_parts)
{
	Assert(boundinfo != NULL);

	switch (boundinfo->strategy)
	{
		case PARTITION_STRATEGY_RANGE:

			/*
			 * RANGE 型分区保证可以按分区在 PartitionDesc
			 * 里定义的次序扫描，
			 * 以提供顺序、互不重叠的元组区间。
			 * 不过，若存在 DEFAULT 分区且它包含在
			 * live_parts 里，则这些分区不是有序的。
			 */
			if (!partition_bound_has_default(boundinfo) ||
				!bms_is_member(boundinfo->default_index, live_parts))
				return true;
			break;

		case PARTITION_STRATEGY_LIST:

			/*
			 * 只要 live_parts 都不与分区表的交错分区重叠，
			 * LIST 分区就是有序的。
			 */
			if (!bms_overlap(live_parts, boundinfo->interleaved_parts))
				return true;

			break;
		case PARTITION_STRATEGY_HASH:
			break;
	}

	return false;
}

/*
 * check_new_partition_bound
 *
 * 检查新分区的边界是否与父表任何现有分区重叠。
 * 并按策略做必要的额外检查。
 */
void
check_new_partition_bound(char *relname, Relation parent,
						  PartitionBoundSpec *spec, ParseState *pstate)
{
	PartitionKey key = RelationGetPartitionKey(parent);
	PartitionDesc partdesc = RelationGetPartitionDesc(parent, false);
	PartitionBoundInfo boundinfo = partdesc->boundinfo;
	int			with = -1;
	bool		overlap = false;
	int			overlap_location = -1;

	if (spec->is_default)
	{
		/*
		 * 默认分区边界从不与任何其他
		 * 分区冲突；若我们挂接的就是它，唯一可能的
		 * 问题是已存在一个，因此检查这一点后就
		 * 完成了。
		 */
		if (boundinfo == NULL || !partition_bound_has_default(boundinfo))
			return;

		/* 默认分区已存在，报错退出。 */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("partition \"%s\" conflicts with existing default partition \"%s\"",
						relname, get_rel_name(partdesc->oids[boundinfo->default_index])),
				 parser_errposition(pstate, spec->location)));
	}

	switch (key->strategy)
	{
		case PARTITION_STRATEGY_HASH:
			{
				Assert(spec->strategy == PARTITION_STRATEGY_HASH);
				Assert(spec->remainder >= 0 && spec->remainder < spec->modulus);

				if (partdesc->nparts > 0)
				{
					int			greatest_modulus;
					int			remainder;
					int			offset;

					/*
					 * 检查这条规则：每个模数都必须是
					 * 下一个更大模数的因子。（例如，若有一批
					 * 分区的模数都是 5，可以添加
					 * 一个模数为 10 的新分区，或一个模数为
					 * 15 的新分区，但无法同时添加模数为 10
					 * 的分区和模数为 15 的分区，因为 10
					 * 不是 15 的因子。）我们只需检查下一个
					 * 更小和下一个更大的现有模数，依靠先前
					 * 对这条规则的强制执行，即可确信
					 * 其余的都符合。
					 */

					/*
					 * 取得 boundinfo->datums 里小于等于
					 * (spec->modulus, spec->remainder) 对的、最大的
					 * (模数, 余数) 对。
					 */
					offset = partition_hash_bsearch(boundinfo,
													spec->modulus,
													spec->remainder);
					if (offset < 0)
					{
						int			next_modulus;

						/*
						 * 现有模数都大于或等于新模数，因此
						 * 新模数必须是最小那个的因子，而最小的
						 * 是 boundinfo 里的第一个。
						 */
						next_modulus = DatumGetInt32(boundinfo->datums[0][0]);
						if (next_modulus % spec->modulus != 0)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
									 errmsg("every hash partition modulus must be a factor of the next larger modulus"),
									 errdetail("The new modulus %d is not a factor of %d, the modulus of existing partition \"%s\".",
											   spec->modulus, next_modulus,
											   get_rel_name(partdesc->oids[0]))));
					}
					else
					{
						int			prev_modulus;

						/*
						 * 我们找到了小于等于新对的最大
						 * (模数, 余数) 对。该模数必须能整除
						 * 新模数，或与它相等。
						 */
						prev_modulus = DatumGetInt32(boundinfo->datums[offset][0]);

						if (spec->modulus % prev_modulus != 0)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
									 errmsg("every hash partition modulus must be a factor of the next larger modulus"),
									 errdetail("The new modulus %d is not divisible by %d, the modulus of existing partition \"%s\".",
											   spec->modulus,
											   prev_modulus,
											   get_rel_name(partdesc->oids[offset]))));

						if (offset + 1 < boundinfo->ndatums)
						{
							int			next_modulus;

							/*
							 * 看下一个更高的 (模数, 余数)
							 * 对。它可能模数相同而余数
							 * 比新对更大，那种情况下就没问题。
							 * 若它的模数更大，新模数必须能
							 * 整除那个模数。
							 */
							next_modulus = DatumGetInt32(boundinfo->datums[offset + 1][0]);

							if (next_modulus % spec->modulus != 0)
								ereport(ERROR,
										(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
										 errmsg("every hash partition modulus must be a factor of the next larger modulus"),
										 errdetail("The new modulus %d is not a factor of %d, the modulus of existing partition \"%s\".",
												   spec->modulus, next_modulus,
												   get_rel_name(partdesc->oids[offset + 1]))));
						}
					}

					greatest_modulus = boundinfo->nindexes;
					remainder = spec->remainder;

					/*
					 * 通常，可能与新分区冲突的最低余数
					 * 等于为新分区指定的余数，但当新分区的
					 * 模数高于迄今用过的任何模数时，
					 * 我们需要调整。
					 */
					if (remainder >= greatest_modulus)
						remainder = remainder % greatest_modulus;

					/* 检查每一个可能冲突的余数。 */
					do
					{
						if (boundinfo->indexes[remainder] != -1)
						{
							overlap = true;
							overlap_location = spec->location;
							with = boundinfo->indexes[remainder];
							break;
						}
						remainder += spec->modulus;
					} while (remainder < greatest_modulus);
				}

				break;
			}

		case PARTITION_STRATEGY_LIST:
			{
				Assert(spec->strategy == PARTITION_STRATEGY_LIST);

				if (partdesc->nparts > 0)
				{
					ListCell   *cell;

					Assert(boundinfo &&
						   boundinfo->strategy == PARTITION_STRATEGY_LIST &&
						   (boundinfo->ndatums > 0 ||
							partition_bound_accepts_nulls(boundinfo) ||
							partition_bound_has_default(boundinfo)));

					foreach(cell, spec->listdatums)
					{
						Const	   *val = lfirst_node(Const, cell);

						overlap_location = val->location;
						if (!val->constisnull)
						{
							int			offset;
							bool		equal;

							offset = partition_list_bsearch(&key->partsupfunc[0],
															key->partcollation,
															boundinfo,
															val->constvalue,
															&equal);
							if (offset >= 0 && equal)
							{
								overlap = true;
								with = boundinfo->indexes[offset];
								break;
							}
						}
						else if (partition_bound_accepts_nulls(boundinfo))
						{
							overlap = true;
							with = boundinfo->null_index;
							break;
						}
					}
				}

				break;
			}

		case PARTITION_STRATEGY_RANGE:
			{
				PartitionRangeBound *lower,
						   *upper;
				int			cmpval;

				Assert(spec->strategy == PARTITION_STRATEGY_RANGE);
				lower = make_one_partition_rbound(key, -1, spec->lowerdatums, true);
				upper = make_one_partition_rbound(key, -1, spec->upperdatums, false);

				/*
				 * 首先检查用指定的下界和上界得到的区间是否为空。
				 * partition_rbound_cmp
				 * 在这里无法返回零，因为下界标志
				 * 不同。
				 */
				cmpval = partition_rbound_cmp(key->partnatts,
											  key->partsupfunc,
											  key->partcollation,
											  lower->datums, lower->kind,
											  true, upper);
				Assert(cmpval != 0);
				if (cmpval > 0)
				{
					/* 指向下界 datum 列表里出问题的键。 */
					PartitionRangeDatum *datum = list_nth(spec->lowerdatums,
														  cmpval - 1);

					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("empty range bound specified for partition \"%s\"",
									relname),
							 errdetail("Specified lower bound %s is greater than or equal to upper bound %s.",
									   get_range_partbound_string(spec->lowerdatums),
									   get_range_partbound_string(spec->upperdatums)),
							 parser_errposition(pstate, datum->location)));
				}

				if (partdesc->nparts > 0)
				{
					int			offset;

					Assert(boundinfo &&
						   boundinfo->strategy == PARTITION_STRATEGY_RANGE &&
						   (boundinfo->ndatums > 0 ||
							partition_bound_has_default(boundinfo)));

					/*
					 * 测试新下界（它被当作
					 * 新分区的一部分，含端点）是落在
					 * 现有分区内部，还是落在间隙里。
					 *
					 * 若它在现有分区内部，偏移 + 1 处的边界
					 * 就是该分区的上界，
					 * 其下标将 >= 0。
					 *
					 * 若它在间隙里，偏移 + 1 处的边界将是
					 * 下一个分区的下界，其下标将
					 * 是 -1。若没有下一个分区也是如此，
					 * 因为下标数组末尾
					 * 额外初始化了一个 -1。
					 */
					offset = partition_range_bsearch(key->partnatts,
													 key->partsupfunc,
													 key->partcollation,
													 boundinfo, lower,
													 &cmpval);

					if (boundinfo->indexes[offset + 1] < 0)
					{
						/*
						 * 检查新分区能否放进这个间隙。
						 * 要放得下，新上界必须小于
						 * 或等于下一个分区的下界，
						 * 若存在下一个分区的话。
						 */
						if (offset + 1 < boundinfo->ndatums)
						{
							Datum	   *datums;
							PartitionRangeDatumKind *kind;
							bool		is_lower;

							datums = boundinfo->datums[offset + 1];
							kind = boundinfo->kind[offset + 1];
							is_lower = (boundinfo->indexes[offset + 1] == -1);

							cmpval = partition_rbound_cmp(key->partnatts,
														  key->partsupfunc,
														  key->partcollation,
														  datums, kind,
														  is_lower, upper);
							if (cmpval < 0)
							{
								/*
								 * 指向上界
								 * datum 列表里出问题的键。
								 */
								PartitionRangeDatum *datum =
									list_nth(spec->upperdatums, abs(cmpval) - 1);

								/*
								 * 新分区与
								 * 偏移 + 1 和
								 * 偏移 + 2 之间的现有分区重叠。
								 */
								overlap = true;
								overlap_location = datum->location;
								with = boundinfo->indexes[offset + 2];
							}
						}
					}
					else
					{
						/*
						 * 新分区与偏移和偏移 + 1 之间的现有
						 * 分区重叠。
						 */
						PartitionRangeDatum *datum;

						/*
						 * 指向下界 datum 列表里出问题的键；
						 * 若相等，就指向第一个。
						 */
						datum = cmpval == 0 ? linitial(spec->lowerdatums) :
							list_nth(spec->lowerdatums, abs(cmpval) - 1);
						overlap = true;
						overlap_location = datum->location;
						with = boundinfo->indexes[offset + 1];
					}
				}

				break;
			}
	}

	if (overlap)
	{
		Assert(with >= 0);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("partition \"%s\" would overlap partition \"%s\"",
						relname, get_rel_name(partdesc->oids[with])),
				 parser_errposition(pstate, overlap_location)));
	}
}

/*
 * check_default_partition_contents
 *
 * 本函数检查默认分区里是否存在一行，
 * 它本应属于正在添加的新分区。若找到一行，
 * 就抛出错误。
 */
void
check_default_partition_contents(Relation parent, Relation default_rel,
								 PartitionBoundSpec *new_spec)
{
	List	   *new_part_constraints;
	List	   *def_part_constraints;
	List	   *all_parts;
	ListCell   *lc;

	new_part_constraints = (new_spec->strategy == PARTITION_STRATEGY_LIST)
		? get_qual_for_list(parent, new_spec)
		: get_qual_for_range(parent, new_spec, false);
	def_part_constraints =
		get_proposed_default_constraint(new_part_constraints);

	/*
	 * 把约束表达式里的 Var 从父表的属性号映射到
	 * default_rel 的属性号。
	 */
	def_part_constraints =
		map_partition_varattnos(def_part_constraints, 1, default_rel,
								parent);

	/*
	 * 若默认分区上的现有约束蕴含它不会
	 * 包含任何属于新分区的行，就可以
	 * 避免扫描默认分区。
	 */
	if (PartConstraintImpliedByRelConstraint(default_rel, def_part_constraints))
	{
		ereport(DEBUG1,
				(errmsg_internal("updated partition constraint for default partition \"%s\" is implied by existing constraints",
								 RelationGetRelationName(default_rel))));
		return;
	}

	/*
	 * 扫描默认分区及其子分区，并检查
	 * 不满足修订后分区约束的行。
	 */
	if (default_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		all_parts = find_all_inheritors(RelationGetRelid(default_rel),
										AccessExclusiveLock, NULL);
	else
		all_parts = list_make1_oid(RelationGetRelid(default_rel));

	foreach(lc, all_parts)
	{
		Oid			part_relid = lfirst_oid(lc);
		Relation	part_rel;
		Expr	   *partition_constraint;
		EState	   *estate;
		ExprState  *partqualstate = NULL;
		Snapshot	snapshot;
		ExprContext *econtext;
		TableScanDesc scan;
		MemoryContext oldCxt;
		TupleTableSlot *tupslot;

		/* 锁已经在上面取得。 */
		if (part_relid != RelationGetRelid(default_rel))
		{
			part_rel = table_open(part_relid, NoLock);

			/*
			 * 把约束表达式里的 Var 从 default_rel 的
			 * 映射到子分区的。
			 */
			partition_constraint = make_ands_explicit(def_part_constraints);
			partition_constraint = (Expr *)
				map_partition_varattnos((List *) partition_constraint, 1,
										part_rel, default_rel);

			/*
			 * 若默认分区子表上的分区约束蕴含
			 * 它不会包含任何属于新
			 * 分区的行，就可以避免扫描该子表。
			 */
			if (PartConstraintImpliedByRelConstraint(part_rel,
													 def_part_constraints))
			{
				ereport(DEBUG1,
						(errmsg_internal("updated partition constraint for default partition \"%s\" is implied by existing constraints",
										 RelationGetRelationName(part_rel))));

				table_close(part_rel, NoLock);
				continue;
			}
		}
		else
		{
			part_rel = default_rel;
			partition_constraint = make_ands_explicit(def_part_constraints);
		}

		/*
		 * 只有 RELKIND_RELATION 关系（即叶子分区）需要被
		 * 扫描。
		 */
		if (part_rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (part_rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
				ereport(WARNING,
						(errcode(ERRCODE_CHECK_VIOLATION),
						 errmsg("skipped scanning foreign table \"%s\" which is a partition of default partition \"%s\"",
								RelationGetRelationName(part_rel),
								RelationGetRelationName(default_rel))));

			if (RelationGetRelid(default_rel) != RelationGetRelid(part_rel))
				table_close(part_rel, NoLock);

			continue;
		}

		estate = CreateExecutorState();

		/* 为分区检查限定条件建立表达式执行状态 */
		partqualstate = ExecPrepareExpr(partition_constraint, estate);

		econtext = GetPerTupleExprContext(estate);
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		tupslot = table_slot_create(part_rel, &estate->es_tupleTable);
		scan = table_beginscan(part_rel, snapshot, 0, NULL);

		/*
		 * 切换到每元组内存上下文，并为产生的每个元组重置它，
		 * 以免泄漏内存。
		 */
		oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		while (table_scan_getnextslot(scan, ForwardScanDirection, tupslot))
		{
			econtext->ecxt_scantuple = tupslot;

			if (!ExecCheck(partqualstate, econtext))
				ereport(ERROR,
						(errcode(ERRCODE_CHECK_VIOLATION),
						 errmsg("updated partition constraint for default partition \"%s\" would be violated by some row",
								RelationGetRelationName(default_rel)),
						 errtable(default_rel)));

			ResetExprContext(econtext);
			CHECK_FOR_INTERRUPTS();
		}

		MemoryContextSwitchTo(oldCxt);
		table_endscan(scan);
		UnregisterSnapshot(snapshot);
		ExecDropSingleTupleTableSlot(tupslot);
		FreeExecutorState(estate);

		if (RelationGetRelid(default_rel) != RelationGetRelid(part_rel))
			table_close(part_rel, NoLock);	/* 把锁保持到提交 */
	}
}

/*
 * get_hash_partition_greatest_modulus
 *
 * 返回哈希分区边界的最大模数。
 * 核心代码里已经不再使用它，但我们留着它，
 * 以防外部模块还在用。
 */
int
get_hash_partition_greatest_modulus(PartitionBoundInfo bound)
{
	Assert(bound && bound->strategy == PARTITION_STRATEGY_HASH);
	return bound->nindexes;
}

/*
 * make_one_partition_rbound
 *
 * 给定 PartitionRangeDatum 元素列表
 * 以及说明该边界是否为下界的标志，返回 PartitionRangeBound。做成函数
 * 是因为有多处想用这个功能。
 */
static PartitionRangeBound *
make_one_partition_rbound(PartitionKey key, int index, List *datums, bool lower)
{
	PartitionRangeBound *bound;
	ListCell   *lc;
	int			i;

	Assert(datums != NIL);

	bound = (PartitionRangeBound *) palloc0(sizeof(PartitionRangeBound));
	bound->index = index;
	bound->datums = (Datum *) palloc0(key->partnatts * sizeof(Datum));
	bound->kind = (PartitionRangeDatumKind *) palloc0(key->partnatts *
													  sizeof(PartitionRangeDatumKind));
	bound->lower = lower;

	i = 0;
	foreach(lc, datums)
	{
		PartitionRangeDatum *datum = lfirst_node(PartitionRangeDatum, lc);

		/* 这个区间 datum 里装的是什么？ */
		bound->kind[i] = datum->kind;

		if (datum->kind == PARTITION_RANGE_DATUM_VALUE)
		{
			Const	   *val = castNode(Const, datum->value);

			if (val->constisnull)
				elog(ERROR, "invalid range bound datum");
			bound->datums[i] = val->constvalue;
		}

		i++;
	}

	return bound;
}

/*
 * partition_rbound_cmp
 *
 * 对两个区间边界，判断第 1 个（由
 * datums1、kind1 和 lower1 指定）是 <、= 还是 > *b2 里的边界。
 *
 * 相等时返回 0，否则返回非零整数，其符号
 * 表示次序，其绝对值给出第一个不匹配列的
 * 从 1 起计的分区键号。
 *
 * partnatts、partsupfunc 和 partcollation 分别给出要比较的
 * 边界里的属性个数、要用的比较函数，以及
 * 属性的排序规则。
 *
 * 注意：若两个区间边界的值比较相等，就要考虑
 * 它们是上界还是下界，上界被认为小于下界。
 * 这对 RelationBuildPartitionDesc() 构造 PartitionBoundInfoData
 * 结构的方式很重要。该结构对两个相邻分区
 * 的公共边界
 * 只保存上界。
 */
static int32
partition_rbound_cmp(int partnatts, FmgrInfo *partsupfunc,
					 Oid *partcollation,
					 Datum *datums1, PartitionRangeDatumKind *kind1,
					 bool lower1, PartitionRangeBound *b2)
{
	int32		colnum = 0;
	int32		cmpval = 0;		/* 消除编译器告警 */
	int			i;
	Datum	   *datums2 = b2->datums;
	PartitionRangeDatumKind *kind2 = b2->kind;
	bool		lower2 = b2->lower;

	for (i = 0; i < partnatts; i++)
	{
		/* 跟踪列号，以防结果需要它 */
		colnum++;

		/*
		 * 首先处理该列无界的情形，这时不应当
		 * 调用比较过程，也不应当考虑任何后面的
		 * 列。注意 PartitionRangeDatumKind 枚举元素
		 * 的比较方式与它们所表示的值相同。
		 */
		if (kind1[i] < kind2[i])
			return -colnum;
		else if (kind1[i] > kind2[i])
			return colnum;
		else if (kind1[i] != PARTITION_RANGE_DATUM_VALUE)
		{
			/*
			 * 列边界都是 MINVALUE 或都是 MAXVALUE。不应当再考虑后面的
			 * 列，但仍然需要比较
			 * 它们是上界还是下界。
			 */
			break;
		}

		cmpval = DatumGetInt32(FunctionCall2Coll(&partsupfunc[i],
												 partcollation[i],
												 datums1[i],
												 datums2[i]));
		if (cmpval != 0)
			break;
	}

	/*
	 * 若比较结果不是相等，就完成了。若它们
	 * 比较相等，仍然必须考虑边界
	 * 是包含端点还是排除端点。排除端点的那个被认为是两者中
	 * 较小的。
	 */
	if (cmpval == 0 && lower1 != lower2)
		cmpval = lower1 ? 1 : -1;

	return cmpval == 0 ? 0 : (cmpval < 0 ? -colnum : colnum);
}

/*
 * partition_rbound_datum_cmp
 *
 * 返回区间边界（由 rb_datums 和 rb_kind 指定）
 * 是 <、= 还是 > 元组的分区键（tuple_datums）
 *
 * n_tuple_datums、partsupfunc 和 partcollation 分别给出要比较的
 * 边界里的属性个数、要用的比较函数，以及
 * 属性的排序规则。
 */
int32
partition_rbound_datum_cmp(FmgrInfo *partsupfunc, Oid *partcollation,
						   Datum *rb_datums, PartitionRangeDatumKind *rb_kind,
						   Datum *tuple_datums, int n_tuple_datums)
{
	int			i;
	int32		cmpval = -1;

	for (i = 0; i < n_tuple_datums; i++)
	{
		if (rb_kind[i] == PARTITION_RANGE_DATUM_MINVALUE)
			return -1;
		else if (rb_kind[i] == PARTITION_RANGE_DATUM_MAXVALUE)
			return 1;

		cmpval = DatumGetInt32(FunctionCall2Coll(&partsupfunc[i],
												 partcollation[i],
												 rb_datums[i],
												 tuple_datums[i]));
		if (cmpval != 0)
			break;
	}

	return cmpval;
}

/*
 * partition_hbound_cmp
 *
 * 先比较模数，模数相等再比较余数。
 */
static int32
partition_hbound_cmp(int modulus1, int remainder1, int modulus2, int remainder2)
{
	if (modulus1 < modulus2)
		return -1;
	if (modulus1 > modulus2)
		return 1;
	if (modulus1 == modulus2 && remainder1 != remainder2)
		return (remainder1 > remainder2) ? 1 : -1;
	return 0;
}

/*
 * partition_list_bsearch
 *		返回小于等于给定值的最大边界 datum 的下标，
 * 		若全部边界 datum 都更大则返回 -1。
 *
 * 若返回下标处的边界 datum 等于
 * 输入值，就把 *is_equal 设为真。
 */
int
partition_list_bsearch(FmgrInfo *partsupfunc, Oid *partcollation,
					   PartitionBoundInfo boundinfo,
					   Datum value, bool *is_equal)
{
	int			lo,
				hi,
				mid;

	lo = -1;
	hi = boundinfo->ndatums - 1;
	while (lo < hi)
	{
		int32		cmpval;

		mid = (lo + hi + 1) / 2;
		cmpval = DatumGetInt32(FunctionCall2Coll(&partsupfunc[0],
												 partcollation[0],
												 boundinfo->datums[mid][0],
												 value));
		if (cmpval <= 0)
		{
			lo = mid;
			*is_equal = (cmpval == 0);
			if (*is_equal)
				break;
		}
		else
			hi = mid - 1;
	}

	return lo;
}

/*
 * partition_range_bsearch
 *		返回小于或等于给定区间边界的最大区间边界的下标，
 *		若全部区间边界都更大
 *		则返回 -1。
 *
 * 本函数返回时，若返回下标处的边界与输入区间边界
 * 完全匹配，*cmpval 设为 0，否则是一个
 * 非零整数，其符号表示次序，其绝对值
 * 给出第一个不匹配列的
 * 从 1 起计的分区键号。
 */
static int
partition_range_bsearch(int partnatts, FmgrInfo *partsupfunc,
						Oid *partcollation,
						PartitionBoundInfo boundinfo,
						PartitionRangeBound *probe, int32 *cmpval)
{
	int			lo,
				hi,
				mid;

	lo = -1;
	hi = boundinfo->ndatums - 1;
	while (lo < hi)
	{
		mid = (lo + hi + 1) / 2;
		*cmpval = partition_rbound_cmp(partnatts, partsupfunc,
									   partcollation,
									   boundinfo->datums[mid],
									   boundinfo->kind[mid],
									   (boundinfo->indexes[mid] == -1),
									   probe);
		if (*cmpval <= 0)
		{
			lo = mid;
			if (*cmpval == 0)
				break;
		}
		else
			hi = mid - 1;
	}

	return lo;
}

/*
 * partition_range_datum_bsearch
 *		返回小于或等于给定元组的最大区间边界的下标，
 *		若全部区间边界都更大则返回 -1。
 *
 * 若返回下标处的区间边界等于
 * 输入元组，就把 *is_equal 设为真。
 */
int
partition_range_datum_bsearch(FmgrInfo *partsupfunc, Oid *partcollation,
							  PartitionBoundInfo boundinfo,
							  int nvalues, Datum *values, bool *is_equal)
{
	int			lo,
				hi,
				mid;

	lo = -1;
	hi = boundinfo->ndatums - 1;
	while (lo < hi)
	{
		int32		cmpval;

		mid = (lo + hi + 1) / 2;
		cmpval = partition_rbound_datum_cmp(partsupfunc,
											partcollation,
											boundinfo->datums[mid],
											boundinfo->kind[mid],
											values,
											nvalues);
		if (cmpval <= 0)
		{
			lo = mid;
			*is_equal = (cmpval == 0);

			if (*is_equal)
				break;
		}
		else
			hi = mid - 1;
	}

	return lo;
}

/*
 * partition_hash_bsearch
 *		返回小于或等于给定 (模数, 余数) 对的
 *		最大 (模数, 余数) 对的下标，若全部都更大
 *		则返回 -1。
 */
int
partition_hash_bsearch(PartitionBoundInfo boundinfo,
					   int modulus, int remainder)
{
	int			lo,
				hi,
				mid;

	lo = -1;
	hi = boundinfo->ndatums - 1;
	while (lo < hi)
	{
		int32		cmpval,
					bound_modulus,
					bound_remainder;

		mid = (lo + hi + 1) / 2;
		bound_modulus = DatumGetInt32(boundinfo->datums[mid][0]);
		bound_remainder = DatumGetInt32(boundinfo->datums[mid][1]);
		cmpval = partition_hbound_cmp(bound_modulus, bound_remainder,
									  modulus, remainder);
		if (cmpval <= 0)
		{
			lo = mid;

			if (cmpval == 0)
				break;
		}
		else
			hi = mid - 1;
	}

	return lo;
}

/*
 * qsort_partition_hbound_cmp
 *
 * 哈希边界先按模数排序，再按余数排序。
 */
static int32
qsort_partition_hbound_cmp(const void *a, const void *b)
{
	const PartitionHashBound *h1 = (const PartitionHashBound *) a;
	const PartitionHashBound *h2 = (const PartitionHashBound *) b;

	return partition_hbound_cmp(h1->modulus, h1->remainder,
								h2->modulus, h2->remainder);
}

/*
 * qsort_partition_list_value_cmp
 *
 * 比较两个列表分区边界 datum。
 */
static int32
qsort_partition_list_value_cmp(const void *a, const void *b, void *arg)
{
	Datum		val1 = ((const PartitionListValue *) a)->value,
				val2 = ((const PartitionListValue *) b)->value;
	PartitionKey key = (PartitionKey) arg;

	return DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[0],
										   key->partcollation[0],
										   val1, val2));
}

/*
 * qsort_partition_rbound_cmp
 *
 * 在对所有区间分区的区间边界排序时使用。
 */
static int32
qsort_partition_rbound_cmp(const void *a, const void *b, void *arg)
{
	PartitionRangeBound *b1 = (*(PartitionRangeBound *const *) a);
	PartitionRangeBound *b2 = (*(PartitionRangeBound *const *) b);
	PartitionKey key = (PartitionKey) arg;

	return compare_range_bounds(key->partnatts, key->partsupfunc,
								key->partcollation,
								b1, b2);
}

/*
 * get_partition_operator
 *
 * 返回给定分区
 * 键列、给定策略的操作符 oid。假定分区键与
 * 所选分区操作符类类型相同，或至少二进制兼容。在
 * 后一种情况下，若操作符类不是
 * 多态类型（表示上面需要一个 RelabelType 节点），就把 *need_relabel 设为真，否则
 * 设为假。
 */
static Oid
get_partition_operator(PartitionKey key, int col, StrategyNumber strategy,
					   bool *need_relabel)
{
	Oid			operoid;

	/*
	 * 用操作符类声明的输入类型同时作为左类型和右类型，
	 * 在分区操作符族里取得操作符。
	 */
	operoid = get_opfamily_member(key->partopfamily[col],
								  key->partopcintype[col],
								  key->partopcintype[col],
								  strategy);
	if (!OidIsValid(operoid))
		elog(ERROR, "missing operator %d(%u,%u) in partition opfamily %u",
			 strategy, key->partopcintype[col], key->partopcintype[col],
			 key->partopfamily[col]);

	/*
	 * 若分区键列与操作符
	 * 类类型不同且不是多态的，就告诉调用方把非 Const 表达式
	 * 包进 RelabelType。这与 parse_coerce.c 的做法一致。
	 */
	*need_relabel = (key->parttypid[col] != key->partopcintype[col] &&
					 key->partopcintype[col] != RECORDOID &&
					 !IsPolymorphicType(key->partopcintype[col]));

	return operoid;
}

/*
 * make_partition_op_expr
 *		返回给定分区键列的 Expr，arg1 和
 *		arg2 分别作为它的左操作数和右操作数
 */
static Expr *
make_partition_op_expr(PartitionKey key, int keynum,
					   uint16 strategy, Expr *arg1, Expr *arg2)
{
	Oid			operoid;
	bool		need_relabel = false;
	Expr	   *result = NULL;

	/* 取得这个分区列的正确 btree 操作符 */
	operoid = get_partition_operator(key, keynum, strategy, &need_relabel);

	/*
	 * 所选操作符可能使非 Const 操作数需要被
	 * 转换，因此照做；见
	 * get_partition_operator() 里的注释。
	 */
	if (!IsA(arg1, Const) &&
		(need_relabel ||
		 key->partcollation[keynum] != key->parttypcoll[keynum]))
		arg1 = (Expr *) makeRelabelType(arg1,
										key->partopcintype[keynum],
										-1,
										key->partcollation[keynum],
										COERCE_EXPLICIT_CAST);

	/* 生成实际的表达式 */
	switch (key->strategy)
	{
		case PARTITION_STRATEGY_LIST:
			{
				List	   *elems = (List *) arg2;
				int			nelems = list_length(elems);

				Assert(nelems >= 1);
				Assert(keynum == 0);

				if (nelems > 1 &&
					!type_is_array(key->parttypid[keynum]))
				{
					ArrayExpr  *arrexpr;
					ScalarArrayOpExpr *saopexpr;

					/* 为右侧输入构造 ArrayExpr */
					arrexpr = makeNode(ArrayExpr);
					arrexpr->array_typeid =
						get_array_type(key->parttypid[keynum]);
					arrexpr->array_collid = key->parttypcoll[keynum];
					arrexpr->element_typeid = key->parttypid[keynum];
					arrexpr->elements = elems;
					arrexpr->multidims = false;
					arrexpr->location = -1;

					/* 构造 leftop = ANY (rightop) */
					saopexpr = makeNode(ScalarArrayOpExpr);
					saopexpr->opno = operoid;
					saopexpr->opfuncid = get_opcode(operoid);
					saopexpr->hashfuncid = InvalidOid;
					saopexpr->negfuncid = InvalidOid;
					saopexpr->useOr = true;
					saopexpr->inputcollid = key->partcollation[keynum];
					saopexpr->args = list_make2(arg1, arrexpr);
					saopexpr->location = -1;

					result = (Expr *) saopexpr;
				}
				else
				{
					List	   *elemops = NIL;
					ListCell   *lc;

					foreach(lc, elems)
					{
						Expr	   *elem = lfirst(lc),
								   *elemop;

						elemop = make_opclause(operoid,
											   BOOLOID,
											   false,
											   arg1, elem,
											   InvalidOid,
											   key->partcollation[keynum]);
						elemops = lappend(elemops, elemop);
					}

					result = nelems > 1 ? makeBoolExpr(OR_EXPR, elemops, -1) : linitial(elemops);
				}
				break;
			}

		case PARTITION_STRATEGY_RANGE:
			result = make_opclause(operoid,
								   BOOLOID,
								   false,
								   arg1, arg2,
								   InvalidOid,
								   key->partcollation[keynum]);
			break;

		case PARTITION_STRATEGY_HASH:
			Assert(false);
			break;
	}

	return result;
}

/*
 * get_qual_for_hash
 *
 * 给定父关系和分区边界结构，返回用作哈希分区
 * 约束的 CHECK 约束表达式。
 *
 * 哈希分区的分区约束始终是对
 * 内建函数 satisfies_hash_partition() 的调用。
 */
static List *
get_qual_for_hash(Relation parent, PartitionBoundSpec *spec)
{
	PartitionKey key = RelationGetPartitionKey(parent);
	FuncExpr   *fexpr;
	Node	   *relidConst;
	Node	   *modulusConst;
	Node	   *remainderConst;
	List	   *args;
	ListCell   *partexprs_item;
	int			i;

	/* 固定参数。 */
	relidConst = (Node *) makeConst(OIDOID,
									-1,
									InvalidOid,
									sizeof(Oid),
									ObjectIdGetDatum(RelationGetRelid(parent)),
									false,
									true);

	modulusConst = (Node *) makeConst(INT4OID,
									  -1,
									  InvalidOid,
									  sizeof(int32),
									  Int32GetDatum(spec->modulus),
									  false,
									  true);

	remainderConst = (Node *) makeConst(INT4OID,
										-1,
										InvalidOid,
										sizeof(int32),
										Int32GetDatum(spec->remainder),
										false,
										true);

	args = list_make3(relidConst, modulusConst, remainderConst);
	partexprs_item = list_head(key->partexprs);

	/* 为每个键列加一个参数。 */
	for (i = 0; i < key->partnatts; i++)
	{
		Node	   *keyCol;

		/* 左操作数 */
		if (key->partattrs[i] != 0)
		{
			keyCol = (Node *) makeVar(1,
									  key->partattrs[i],
									  key->parttypid[i],
									  key->parttypmod[i],
									  key->parttypcoll[i],
									  0);
		}
		else
		{
			keyCol = (Node *) copyObject(lfirst(partexprs_item));
			partexprs_item = lnext(key->partexprs, partexprs_item);
		}

		args = lappend(args, keyCol);
	}

	fexpr = makeFuncExpr(F_SATISFIES_HASH_PARTITION,
						 BOOLOID,
						 args,
						 InvalidOid,
						 InvalidOid,
						 COERCE_EXPLICIT_CALL);

	return list_make1(fexpr);
}

/*
 * get_qual_for_list
 *
 * 给定父关系和分区边界结构，返回用作列表分区
 * 约束的隐式 AND 表达式列表。
 *
 * 当默认分区是唯一
 * 分区时函数返回 NIL，因为那种情况下没有约束。
 */
static List *
get_qual_for_list(Relation parent, PartitionBoundSpec *spec)
{
	PartitionKey key = RelationGetPartitionKey(parent);
	List	   *result;
	Expr	   *keyCol;
	Expr	   *opexpr;
	NullTest   *nulltest;
	ListCell   *cell;
	List	   *elems = NIL;
	bool		list_has_null = false;

	/*
	 * 只支持单列列表分区，因此我们只关心
	 * 下标为 0 的分区键。
	 */
	Assert(key->partnatts == 1);

	/* 构造表示分区列的 Var 或表达式 */
	if (key->partattrs[0] != 0)
		keyCol = (Expr *) makeVar(1,
								  key->partattrs[0],
								  key->parttypid[0],
								  key->parttypmod[0],
								  key->parttypcoll[0],
								  0);
	else
		keyCol = (Expr *) copyObject(linitial(key->partexprs));

	/*
	 * 对默认列表分区，收集全部分区的 datum。默认
	 * 分区约束应当检查分区键
	 * 不等于其中任何一个。
	 */
	if (spec->is_default)
	{
		int			i;
		int			ndatums = 0;
		PartitionDesc pdesc = RelationGetPartitionDesc(parent, false);
		PartitionBoundInfo boundinfo = pdesc->boundinfo;

		if (boundinfo)
		{
			ndatums = boundinfo->ndatums;

			if (partition_bound_accepts_nulls(boundinfo))
				list_has_null = true;
		}

		/*
		 * 若默认分区是唯一的分区，它上面不必有任何分区
		 * 约束。
		 */
		if (ndatums == 0 && !list_has_null)
			return NIL;

		for (i = 0; i < ndatums; i++)
		{
			Const	   *val;

			/*
			 * 从已知非空的 datum 构造 Const。必须小心地
			 * 复制该值，因为我们的结果必须能够活得比
			 * 正在复制的关系缓存项更久。
			 */
			val = makeConst(key->parttypid[0],
							key->parttypmod[0],
							key->parttypcoll[0],
							key->parttyplen[0],
							datumCopy(*boundinfo->datums[i],
									  key->parttypbyval[0],
									  key->parttyplen[0]),
							false,	/* isnull（是否为空） */
							key->parttypbyval[0]);

			elems = lappend(elems, val);
		}
	}
	else
	{
		/*
		 * 为允许的值建立 Const 列表，排除任何空值。
		 */
		foreach(cell, spec->listdatums)
		{
			Const	   *val = lfirst_node(Const, cell);

			if (val->constisnull)
				list_has_null = true;
			else
				elems = lappend(elems, copyObject(val));
		}
	}

	if (elems)
	{
		/*
		 * 从非空分区
		 * 值生成操作符表达式。
		 */
		opexpr = make_partition_op_expr(key, 0, BTEqualStrategyNumber,
										keyCol, (Expr *) elems);
	}
	else
	{
		/*
		 * 若没有分区值，就不需要操作符
		 * 表达式。
		 */
		opexpr = NULL;
	}

	if (!list_has_null)
	{
		/*
		 * 造一个将与主表达式 ANDed 的 col IS NOT NULL 测试。
		 * 这看起来可能多余，但分区路由
		 * 机制需要它。
		 */
		nulltest = makeNode(NullTest);
		nulltest->arg = keyCol;
		nulltest->nulltesttype = IS_NOT_NULL;
		nulltest->argisrow = false;
		nulltest->location = -1;

		result = opexpr ? list_make2(nulltest, opexpr) : list_make1(nulltest);
	}
	else
	{
		/*
		 * 造一个将与主表达式做 OR 的
		 * col IS NULL 测试。
		 */
		nulltest = makeNode(NullTest);
		nulltest->arg = keyCol;
		nulltest->nulltesttype = IS_NULL;
		nulltest->argisrow = false;
		nulltest->location = -1;

		if (opexpr)
		{
			Expr	   *or;

			or = makeBoolExpr(OR_EXPR, list_make2(nulltest, opexpr), -1);
			result = list_make1(or);
		}
		else
			result = list_make1(nulltest);
	}

	/*
	 * 注意，一般而言，对约束表达式加 NOT 并不
	 * 必然反转它所接受的行集合，因为 NOT (NULL) 是
	 * NULL。不过，我们在这里构造的分区约束从不
	 * 求值为 NULL，因此加 NOT 能按预期工作。
	 */
	if (spec->is_default)
	{
		result = list_make1(make_ands_explicit(result));
		result = list_make1(makeBoolExpr(NOT_EXPR, result, -1));
	}

	return result;
}

/*
 * get_qual_for_range
 *
 * 给定父关系和分区边界结构，返回用作区间分区
 * 约束的隐式 AND 表达式列表。
 *
 * 对多列区间分区键，例如 (a, b, c)，以下界元组 (al, bl, cl)
 * 和上界元组 (au, bu, cu)，我们
 * 生成如下形式的表达式树：
 *
 *	(a IS NOT NULL) and (b IS NOT NULL) and (c IS NOT NULL)
 *		AND
 *	(a > al OR (a = al AND b > bl) OR (a = al AND b = bl AND c >= cl))
 *		AND
 *	(a < au OR (a = au AND b < bu) OR (a = au AND b = bu AND c < cu))
 *
 * 下界元组和上界元组的前缀常常含有
 * 相同的值，例如 (al = au)，那种情况下我们会发出
 * 如下形式的表达式树：
 *
 *	(a IS NOT NULL) and (b IS NOT NULL) and (c IS NOT NULL)
 *		AND
 *	(a = al)
 *		AND
 *	(b > bl OR (b = bl AND c >= cl))
 *		AND
 *	(b < bu OR (b = bu AND c < cu))
 *
 * 若某个边界 datum 是 MINVALUE 或 MAXVALUE，这些表达式就利用
 * 任何值都大于 MINVALUE 且小于
 * MAXVALUE 这一事实来简化。例如，若 cu = MAXVALUE，c < cu 自动
 * 为真，就不必为它发出任何表达式，最后一行变成：
 *
 *	(b < bu) OR (b = bu)，这被简化为 (b <= bu)
 *
 * 最常见的只有一个分区列的情形，例如 a，会生成如下
 * 表达式树：a IS NOT NULL AND a >= al AND a < au。
 *
 * 对默认分区，它返回所有
 * 其他分区约束的否定。
 *
 * 外部调用方应当把 for_default 传为假；只有
 * 递归时我们才把它设为真。
 */
static List *
get_qual_for_range(Relation parent, PartitionBoundSpec *spec,
				   bool for_default)
{
	List	   *result = NIL;
	ListCell   *cell1,
			   *cell2,
			   *partexprs_item,
			   *partexprs_item_saved;
	int			i,
				j;
	PartitionRangeDatum *ldatum,
			   *udatum;
	PartitionKey key = RelationGetPartitionKey(parent);
	Expr	   *keyCol;
	Const	   *lower_val,
			   *upper_val;
	List	   *lower_or_arms,
			   *upper_or_arms;
	int			num_or_arms,
				current_or_arm;
	ListCell   *lower_or_start_datum,
			   *upper_or_start_datum;
	bool		need_next_lower_arm,
				need_next_upper_arm;

	if (spec->is_default)
	{
		List	   *or_expr_args = NIL;
		PartitionDesc pdesc = RelationGetPartitionDesc(parent, false);
		Oid		   *inhoids = pdesc->oids;
		int			nparts = pdesc->nparts,
					k;

		for (k = 0; k < nparts; k++)
		{
			Oid			inhrelid = inhoids[k];
			HeapTuple	tuple;
			Datum		datum;
			PartitionBoundSpec *bspec;

			tuple = SearchSysCache1(RELOID, inhrelid);
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for relation %u", inhrelid);

			datum = SysCacheGetAttrNotNull(RELOID, tuple,
										   Anum_pg_class_relpartbound);
			bspec = (PartitionBoundSpec *)
				stringToNode(TextDatumGetCString(datum));
			if (!IsA(bspec, PartitionBoundSpec))
				elog(ERROR, "expected PartitionBoundSpec");

			if (!bspec->is_default)
			{
				List	   *part_qual;

				part_qual = get_qual_for_range(parent, bspec, true);

				/*
				 * 把该分区的约束 AND 起来并加入
				 * or_expr_args
				 */
				or_expr_args = lappend(or_expr_args, list_length(part_qual) > 1
									   ? makeBoolExpr(AND_EXPR, part_qual, -1)
									   : linitial(part_qual));
			}
			ReleaseSysCache(tuple);
		}

		if (or_expr_args != NIL)
		{
			Expr	   *other_parts_constr;

			/*
			 * 用 OR 组合为非默认分区得到的约束。
			 * 按要求，每个 OR 参数都不包含
			 * 分区键的 NOT NULL 测试（以免
			 * 无用地重复）。现在把同样的测试加上。
			 */
			other_parts_constr =
				makeBoolExpr(AND_EXPR,
							 lappend(get_range_nulltest(key),
									 list_length(or_expr_args) > 1
									 ? makeBoolExpr(OR_EXPR, or_expr_args,
													-1)
									 : linitial(or_expr_args)),
							 -1);

			/*
			 * 最后，默认分区包含非默认分区里所含一切的
			 * *NOT*。
			 */
			result = list_make1(makeBoolExpr(NOT_EXPR,
											 list_make1(other_parts_constr), -1));
		}

		return result;
	}

	/*
	 * 若这是为默认分区做的递归调用，就跳过 get_range_nulltest，
	 * 以免对每个分区在相同键上累积 NullTest。
	 */
	if (!for_default)
		result = get_range_nulltest(key);

	/*
	 * 遍历键列，用该列类型的 btree 相等操作符
	 * 检查对应的下界和上界 datum 是否相等。若相等，
	 * 就发出单个 keyCol = common_value 表达式。
	 * 从对应下界和上界 datum 不相等的第一列开始，
	 * 我们生成函数头注释里
	 * 所示的 OR 表达式。
	 */
	i = 0;
	partexprs_item = list_head(key->partexprs);
	partexprs_item_saved = partexprs_item;	/* 消除编译器告警 */
	forboth(cell1, spec->lowerdatums, cell2, spec->upperdatums)
	{
		EState	   *estate;
		MemoryContext oldcxt;
		Expr	   *test_expr;
		ExprState  *test_exprstate;
		Datum		test_result;
		bool		isNull;

		ldatum = lfirst_node(PartitionRangeDatum, cell1);
		udatum = lfirst_node(PartitionRangeDatum, cell2);

		/*
		 * 因为 get_range_key_properties() 会修改 partexprs_item，而本函数后面
		 * 可能需要从前一个表达式重新开始，
		 * 所以把当前值存起来。
		 */
		partexprs_item_saved = partexprs_item;

		get_range_key_properties(key, i, ldatum, udatum,
								 &partexprs_item,
								 &keyCol,
								 &lower_val, &upper_val);

		/*
		 * 若任一值为 NULL，对应的分区边界就是
		 * MINVALUE 或 MAXVALUE，我们把它们当作不相等，因为
		 * 即使它们相同，也没有可以把键列等同起来的
		 * 公共值。
		 */
		if (!lower_val || !upper_val)
			break;

		/* 创建测试表达式 */
		estate = CreateExecutorState();
		oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
		test_expr = make_partition_op_expr(key, i, BTEqualStrategyNumber,
										   (Expr *) lower_val,
										   (Expr *) upper_val);
		fix_opfuncids((Node *) test_expr);
		test_exprstate = ExecInitExpr(test_expr, NULL);
		test_result = ExecEvalExprSwitchContext(test_exprstate,
												GetPerTupleExprContext(estate),
												&isNull);
		MemoryContextSwitchTo(oldcxt);
		FreeExecutorState(estate);

		/* 若不相等，就去生成 OR 表达式 */
		if (!DatumGetBool(test_result))
			break;

		/*
		 * 最后一列键的边界不能相等，因为这样的
		 * 区间分区永远不会被允许定义（否则它会有
		 * 空区间）。
		 */
		if (i == key->partnatts - 1)
			elog(ERROR, "invalid range bound specification");

		/* 相等，因此生成 keyCol = lower_val 表达式 */
		result = lappend(result,
						 make_partition_op_expr(key, i, BTEqualStrategyNumber,
												keyCol, (Expr *) lower_val));

		i++;
	}

	/* 第一对不相等的 lower_val 和 upper_val。 */
	lower_or_start_datum = cell1;
	upper_or_start_datum = cell2;

	/* OR 的分支数等于剩下的键列数。 */
	num_or_arms = key->partnatts - i;
	current_or_arm = 0;
	lower_or_arms = upper_or_arms = NIL;
	need_next_lower_arm = need_next_upper_arm = true;
	while (current_or_arm < num_or_arms)
	{
		List	   *lower_or_arm_args = NIL,
				   *upper_or_arm_args = NIL;

		/* 从第 i 列重新开始扫描列 */
		j = i;
		partexprs_item = partexprs_item_saved;

		for_both_cell(cell1, spec->lowerdatums, lower_or_start_datum,
					  cell2, spec->upperdatums, upper_or_start_datum)
		{
			PartitionRangeDatum *ldatum_next = NULL,
					   *udatum_next = NULL;

			ldatum = lfirst_node(PartitionRangeDatum, cell1);
			if (lnext(spec->lowerdatums, cell1))
				ldatum_next = castNode(PartitionRangeDatum,
									   lfirst(lnext(spec->lowerdatums, cell1)));
			udatum = lfirst_node(PartitionRangeDatum, cell2);
			if (lnext(spec->upperdatums, cell2))
				udatum_next = castNode(PartitionRangeDatum,
									   lfirst(lnext(spec->upperdatums, cell2)));
			get_range_key_properties(key, j, ldatum, udatum,
									 &partexprs_item,
									 &keyCol,
									 &lower_val, &upper_val);

			if (need_next_lower_arm && lower_val)
			{
				uint16		strategy;

				/*
				 * 对本分支的非最后几列，使用 EQ 操作符。
				 * 对本分支的最后一列，使用 GT，除非这是
				 * 整个边界检查的最后一列，或下一个边界
				 * datum 是 MINVALUE，那种情况下使用 GE。
				 */
				if (j - i < current_or_arm)
					strategy = BTEqualStrategyNumber;
				else if (j == key->partnatts - 1 ||
						 (ldatum_next &&
						  ldatum_next->kind == PARTITION_RANGE_DATUM_MINVALUE))
					strategy = BTGreaterEqualStrategyNumber;
				else
					strategy = BTGreaterStrategyNumber;

				lower_or_arm_args = lappend(lower_or_arm_args,
											make_partition_op_expr(key, j,
																   strategy,
																   keyCol,
																   (Expr *) lower_val));
			}

			if (need_next_upper_arm && upper_val)
			{
				uint16		strategy;

				/*
				 * 对本分支的非最后几列，使用 EQ 操作符。
				 * 对本分支的最后一列，使用 LT，除非下一个
				 * 边界 datum 是 MAXVALUE，那种情况下使用 LE。
				 */
				if (j - i < current_or_arm)
					strategy = BTEqualStrategyNumber;
				else if (udatum_next &&
						 udatum_next->kind == PARTITION_RANGE_DATUM_MAXVALUE)
					strategy = BTLessEqualStrategyNumber;
				else
					strategy = BTLessStrategyNumber;

				upper_or_arm_args = lappend(upper_or_arm_args,
											make_partition_op_expr(key, j,
																   strategy,
																   keyCol,
																   (Expr *) upper_val));
			}

			/*
			 * OR 的参数够了吗？第一分支考虑
			 * 剩余列中的第一列，第二分支考虑剩余列中的前
			 * 两列，依此类推。
			 */
			++j;
			if (j - i > current_or_arm)
			{
				/*
				 * 若将要考虑的新列无界，或这一列已经无界，
				 * 就不得再发出更多分支。
				 */
				if (!lower_val || !ldatum_next ||
					ldatum_next->kind != PARTITION_RANGE_DATUM_VALUE)
					need_next_lower_arm = false;
				if (!upper_val || !udatum_next ||
					udatum_next->kind != PARTITION_RANGE_DATUM_VALUE)
					need_next_upper_arm = false;
				break;
			}
		}

		if (lower_or_arm_args != NIL)
			lower_or_arms = lappend(lower_or_arms,
									list_length(lower_or_arm_args) > 1
									? makeBoolExpr(AND_EXPR, lower_or_arm_args, -1)
									: linitial(lower_or_arm_args));

		if (upper_or_arm_args != NIL)
			upper_or_arms = lappend(upper_or_arms,
									list_length(upper_or_arm_args) > 1
									? makeBoolExpr(AND_EXPR, upper_or_arm_args, -1)
									: linitial(upper_or_arm_args));

		/* 若下一次迭代无事可做，就跳出。 */
		if (!need_next_lower_arm && !need_next_upper_arm)
			break;

		++current_or_arm;
	}

	/*
	 * 为下界和上界（若
	 * 需要）各自生成 OR 表达式，并追加到隐式 ANDed 的
	 * 表达式列表。
	 */
	if (lower_or_arms != NIL)
		result = lappend(result,
						 list_length(lower_or_arms) > 1
						 ? makeBoolExpr(OR_EXPR, lower_or_arms, -1)
						 : linitial(lower_or_arms));
	if (upper_or_arms != NIL)
		result = lappend(result,
						 list_length(upper_or_arms) > 1
						 ? makeBoolExpr(OR_EXPR, upper_or_arms, -1)
						 : linitial(upper_or_arms));

	/*
	 * 如上所述，对非默认分区，我们返回含常量 TRUE 的列表。若
	 * 为默认分区做递归调用时结果是 NIL，就意味着
	 * 这是唯一另一个能容纳该键除 NULL 外每个值的分区。
	 * 因此返回先前跳过的 NullTest 结果。
	 */
	if (result == NIL)
		result = for_default
			? get_range_nulltest(key)
			: list_make1(makeBoolConst(true, false));

	return result;
}

/*
 * get_range_key_properties
 *		返回给定列的区间分区键信息。
 *
 * 这是 get_qual_for_range 的子程序，其 API 相当
 * 专门服务于该调用方。
 *
 * 为键列构造 Expr（经 *keyCol 返回），并为下界和上界
 * 构造 Const（经 *lower_val 和
 * *upper_val 返回）。对 MINVALUE/MAXVALUE 界限，返回 NULL 而不是
 * Const。这些结构都是新 palloc 的。
 *
 * *partexprs_item 指向 key->partexprs 列表里下一个表达式所在的单元，
 * 或为 NULL。返回时它可能前进一步。
 */
static void
get_range_key_properties(PartitionKey key, int keynum,
						 PartitionRangeDatum *ldatum,
						 PartitionRangeDatum *udatum,
						 ListCell **partexprs_item,
						 Expr **keyCol,
						 Const **lower_val, Const **upper_val)
{
	/* 取得这一列的分区键表达式 */
	if (key->partattrs[keynum] != 0)
	{
		*keyCol = (Expr *) makeVar(1,
								   key->partattrs[keynum],
								   key->parttypid[keynum],
								   key->parttypmod[keynum],
								   key->parttypcoll[keynum],
								   0);
	}
	else
	{
		if (*partexprs_item == NULL)
			elog(ERROR, "wrong number of partition key expressions");
		*keyCol = copyObject(lfirst(*partexprs_item));
		*partexprs_item = lnext(key->partexprs, *partexprs_item);
	}

	/* 为边界取得适当的 Const 节点 */
	if (ldatum->kind == PARTITION_RANGE_DATUM_VALUE)
		*lower_val = castNode(Const, copyObject(ldatum->value));
	else
		*lower_val = NULL;

	if (udatum->kind == PARTITION_RANGE_DATUM_VALUE)
		*upper_val = castNode(Const, copyObject(udatum->value));
	else
		*upper_val = NULL;
}

/*
 * get_range_nulltest
 *
 * 非默认区间分区表目前不允许分区
 * 键为 null，因此为每个键列发出 IS NOT NULL 表达式。
 */
static List *
get_range_nulltest(PartitionKey key)
{
	List	   *result = NIL;
	NullTest   *nulltest;
	ListCell   *partexprs_item;
	int			i;

	partexprs_item = list_head(key->partexprs);
	for (i = 0; i < key->partnatts; i++)
	{
		Expr	   *keyCol;

		if (key->partattrs[i] != 0)
		{
			keyCol = (Expr *) makeVar(1,
									  key->partattrs[i],
									  key->parttypid[i],
									  key->parttypmod[i],
									  key->parttypcoll[i],
									  0);
		}
		else
		{
			if (partexprs_item == NULL)
				elog(ERROR, "wrong number of partition key expressions");
			keyCol = copyObject(lfirst(partexprs_item));
			partexprs_item = lnext(key->partexprs, partexprs_item);
		}

		nulltest = makeNode(NullTest);
		nulltest->arg = keyCol;
		nulltest->nulltesttype = IS_NOT_NULL;
		nulltest->argisrow = false;
		nulltest->location = -1;
		result = lappend(result, nulltest);
	}

	return result;
}

/*
 * compute_partition_hash_value
 *
 * 计算给定分区键值的哈希值。
 */
uint64
compute_partition_hash_value(int partnatts, FmgrInfo *partsupfunc, Oid *partcollation,
							 Datum *values, bool *isnull)
{
	int			i;
	uint64		rowHash = 0;
	Datum		seed = UInt64GetDatum(HASH_PARTITION_SEED);

	for (i = 0; i < partnatts; i++)
	{
		/* 空值直接忽略 */
		if (!isnull[i])
		{
			Datum		hash;

			Assert(OidIsValid(partsupfunc[i].fn_oid));

			/*
			 * 调用每个分区键
			 * 属性各自的数据类型专用哈希函数，
			 * 计算每个 datum 值的哈希。
			 */
			hash = FunctionCall2Coll(&partsupfunc[i], partcollation[i],
									 values[i], seed);

			/* 合成一个 64 位哈希值 */
			rowHash = hash_combine64(rowHash, DatumGetUInt64(hash));
		}
	}

	return rowHash;
}

/*
 * satisfies_hash_partition
 *
 * 这是供哈希分区约束使用的 SQL 可调用函数。
 * 前三个参数是父表 OID、模数和余数。
 * 其余参数是分区列的值（或
 * 表达式）；它们被哈希，结果通过调用 hash_combine64
 * 合成一个哈希值。
 *
 * 若这个计算出的单个哈希值除以给定模数得到的余数
 * 等于给定余数，就返回真，否则返回假。
 * 注意：它绝不能返回 null，因为约束机制
 * 会把那当成通过。
 *
 * 用法见 get_qual_for_hash()。
 */
Datum
satisfies_hash_partition(PG_FUNCTION_ARGS)
{
	typedef struct ColumnsHashData
	{
		Oid			relid;
		int			nkeys;
		Oid			variadic_type;
		int16		variadic_typlen;
		bool		variadic_typbyval;
		char		variadic_typalign;
		Oid			partcollid[PARTITION_MAX_KEYS];
		FmgrInfo	partsupfunc[FLEXIBLE_ARRAY_MEMBER];
	} ColumnsHashData;
	Oid			parentId;
	int			modulus;
	int			remainder;
	Datum		seed = UInt64GetDatum(HASH_PARTITION_SEED);
	ColumnsHashData *my_extra;
	uint64		rowHash = 0;

	/* 若父 OID、模数或余数为 NULL，就返回假。 */
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		PG_RETURN_BOOL(false);
	parentId = PG_GETARG_OID(0);
	modulus = PG_GETARG_INT32(1);
	remainder = PG_GETARG_INT32(2);

	/* 对模数和余数做健全性检查。 */
	if (modulus <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("modulus for hash partition must be an integer value greater than zero")));
	if (remainder < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("remainder for hash partition must be an integer value greater than or equal to zero")));
	if (remainder >= modulus)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("remainder for hash partition must be less than modulus")));

	/*
	 * 缓存哈希函数信息。
	 */
	my_extra = (ColumnsHashData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL || my_extra->relid != parentId)
	{
		Relation	parent;
		PartitionKey key;
		int			j;

		/* 打开父关系并取出分区键信息 */
		parent = relation_open(parentId, AccessShareLock);
		key = RelationGetPartitionKey(parent);

		/* 拒绝不是哈希分区的父表。 */
		if (key == NULL || key->strategy != PARTITION_STRATEGY_HASH)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"%s\" is not a hash partitioned table",
							get_rel_name(parentId))));

		if (!get_fn_expr_variadic(fcinfo->flinfo))
		{
			int			nargs = PG_NARGS() - 3;

			/* 若列值个数不对就报错 */
			if (key->partnatts != nargs)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("number of partitioning columns (%d) does not match number of partition keys provided (%d)",
								key->partnatts, nargs)));

			/* 为我们的缓存分配空间 */
			fcinfo->flinfo->fn_extra =
				MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
									   offsetof(ColumnsHashData, partsupfunc) +
									   sizeof(FmgrInfo) * nargs);
			my_extra = (ColumnsHashData *) fcinfo->flinfo->fn_extra;
			my_extra->relid = parentId;
			my_extra->nkeys = key->partnatts;
			memcpy(my_extra->partcollid, key->partcollation,
				   key->partnatts * sizeof(Oid));

			/* 检查参数类型并保存 fmgr_infos */
			for (j = 0; j < key->partnatts; ++j)
			{
				Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, j + 3);

				if (argtype != key->parttypid[j] && !IsBinaryCoercible(argtype, key->parttypid[j]))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("column %d of the partition key has type %s, but supplied value is of type %s",
									j + 1, format_type_be(key->parttypid[j]), format_type_be(argtype))));

				fmgr_info_copy(&my_extra->partsupfunc[j],
							   &key->partsupfunc[j],
							   fcinfo->flinfo->fn_mcxt);
			}
		}
		else
		{
			ArrayType  *variadic_array = PG_GETARG_ARRAYTYPE_P(3);

			/* 为我们的缓存分配空间，这里只有一个 FmgrInfo */
			fcinfo->flinfo->fn_extra =
				MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
									   offsetof(ColumnsHashData, partsupfunc) +
									   sizeof(FmgrInfo));
			my_extra = (ColumnsHashData *) fcinfo->flinfo->fn_extra;
			my_extra->relid = parentId;
			my_extra->nkeys = key->partnatts;
			my_extra->variadic_type = ARR_ELEMTYPE(variadic_array);
			get_typlenbyvalalign(my_extra->variadic_type,
								 &my_extra->variadic_typlen,
								 &my_extra->variadic_typbyval,
								 &my_extra->variadic_typalign);
			my_extra->partcollid[0] = key->partcollation[0];

			/* 检查参数类型 */
			for (j = 0; j < key->partnatts; ++j)
				if (key->parttypid[j] != my_extra->variadic_type)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("column %d of the partition key has type \"%s\", but supplied value is of type \"%s\"",
									j + 1,
									format_type_be(key->parttypid[j]),
									format_type_be(my_extra->variadic_type))));

			fmgr_info_copy(&my_extra->partsupfunc[0],
						   &key->partsupfunc[0],
						   fcinfo->flinfo->fn_mcxt);
		}

		/* 把锁保持到提交 */
		relation_close(parent, NoLock);
	}

	if (!OidIsValid(my_extra->variadic_type))
	{
		int			nkeys = my_extra->nkeys;
		int			i;

		/*
		 * 对非可变参数调用，参数个数和类型
		 * 都不会在各次调用之间改变，因此避免在这里
		 * 再花检查的开销。
		 */

		for (i = 0; i < nkeys; i++)
		{
			Datum		hash;

			/* 键从函数的第四个参数开始。 */
			int			argno = i + 3;

			if (PG_ARGISNULL(argno))
				continue;

			hash = FunctionCall2Coll(&my_extra->partsupfunc[i],
									 my_extra->partcollid[i],
									 PG_GETARG_DATUM(argno),
									 seed);

			/* 合成一个 64 位哈希值 */
			rowHash = hash_combine64(rowHash, DatumGetUInt64(hash));
		}
	}
	else
	{
		ArrayType  *variadic_array = PG_GETARG_ARRAYTYPE_P(3);
		int			i;
		int			nelems;
		Datum	   *datum;
		bool	   *isnull;

		deconstruct_array(variadic_array,
						  my_extra->variadic_type,
						  my_extra->variadic_typlen,
						  my_extra->variadic_typbyval,
						  my_extra->variadic_typalign,
						  &datum, &isnull, &nelems);

		/* 若列值个数不对就报错 */
		if (nelems != my_extra->nkeys)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("number of partitioning columns (%d) does not match number of partition keys provided (%d)",
							my_extra->nkeys, nelems)));

		for (i = 0; i < nelems; i++)
		{
			Datum		hash;

			if (isnull[i])
				continue;

			hash = FunctionCall2Coll(&my_extra->partsupfunc[0],
									 my_extra->partcollid[0],
									 datum[i],
									 seed);

			/* 合成一个 64 位哈希值 */
			rowHash = hash_combine64(rowHash, DatumGetUInt64(hash));
		}
	}

	PG_RETURN_BOOL(rowHash % modulus == remainder);
}
