/*-------------------------------------------------------------------------
 *
 * mcv.c
 *	  POSTGRES multivariate MCV lists
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/statistics/mcv.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/*
 * 根据统计所定义的维度（列）个数，
 * 计算一条序列化 MCV 项的大小。datum 值
 * 存放在单独的数组里（去重，以尽量减小大小），
 * 因此序列化的项只存放指向该数组的 uint16 下标。
 *
 * 每个序列化项按此顺序存放：
 *
 * - 指向值的下标	  （ndim * sizeof(uint16)）
 * - 空标志			  （ndim * sizeof(bool)）
 * - 频率			  （sizeof(double)）
 * - base_frequency		  （sizeof(double)）。
 *
 * MCV 项内部没有对齐填充。
 * 因此每个 MCV 项总共需要这么多字节：
 *
 *	 ndim * (sizeof(uint16) + sizeof(bool)) + 2 * sizeof(double)
 */
#define ITEM_SIZE(ndims)	\
	((ndims) * (sizeof(uint16) + sizeof(bool)) + 2 * sizeof(double))

/*
 * 用来计算序列化 MCV 列表表示的大小。
 */
#define MinSizeOfMCVList		\
	(VARHDRSZ + sizeof(uint32) * 3 + sizeof(AttrNumber))

/*
 * 序列化 MCV 列表的大小，不含按维度
 * 去重的值所需空间。这个宏用于尚不能
 * 安全访问每一列数据量的
 * 序列化信息时。
 */
#define SizeOfMCVList(ndims,nitems)	\
	((MinSizeOfMCVList + sizeof(Oid) * (ndims)) + \
	 ((ndims) * sizeof(DimensionInfo)) + \
	 ((nitems) * ITEM_SIZE(ndims)))

static MultiSortSupport build_mss(StatsBuildData *data);

static SortItem *build_distinct_groups(int numrows, SortItem *items,
									   MultiSortSupport mss, int *ndistinct);

static SortItem **build_column_frequencies(SortItem *groups, int ngroups,
										   MultiSortSupport mss, int *ncounts);

static int	count_distinct_groups(int numrows, SortItem *items,
								  MultiSortSupport mss);

/*
 * 计算 bitmap 项的新值，要考虑它是用于
 * 用 AND/OR 连接的子句。
 */
#define RESULT_MERGE(value, is_or, match) \
	((is_or) ? ((value) || (match)) : ((value) && (match)))

/*
 * 处理子句列表时，bitmap 项可能被设成某个值，
 * 使得额外子句没法再改变它。例如在处理
 * 用 AND 连接的子句列表时，一旦该项被设为 false，
 * 它就会保持那样。用 OR 连接的子句和 true 也类似。
 *
 * 当 bitmap 里的值无论剩余子句如何求值都没法改变时，
 * 返回真。
 */
#define RESULT_IS_FINAL(value, is_or)	((is_or) ? (value) : (!(value)))

/*
 * get_mincount_for_mcv_list
 * 		确定一个值要被纳入 MCV 列表，在样本里
 * 		须出现的最少次数。
 *
 * 我们只想保留在样本里出现得足够频繁、
 * 因而有理由把其样本频率外推到
 * 整张表的那些值。做法是给样本频率的相对
 * 标准误差加一个上界，从而使规划器根据 MCV 统计信息
 * 产生的任何估计都可以预期相当
 * 准确。
 *
 * 由于我们是无放回抽样，某个特定值的样本频率
 * 服从超几何分布。在这种情形下估计误差时，
 * 一条常用的经验法则是：样本里该值须至少出现
 * 10 次，那种情况下该分布可以用正态分布近似，并且
 * 可以应用标准误差分析技术。给定样本大小
 * 为 n、总体大小为 N、样本频率为 p=cnt/n 时，比例 p 的
 * 标准误差由下式给出：
 * SE = sqrt(p*(1-p)/n) * sqrt((N-n)/(N-1))
 *		其中第二项是有限总体校正。为了得到
 * 相当准确的规划器估计，
 * 我们对相对标准误差施加 20% 的上界，即 SE/p < 0.2。这个 20%
 * 相对误差界相当任意，但经验表明它
 * 效果很好。整理该公式，就得到见到该值的
 * 实例数下界：
 * cnt > n*(N-n) / (N-n+0.04*n*(N-1))
 *		该下界最多为 25，并且当 n 趋近 0 或 N 时
 * 趋近于 0。n 趋近 0 的情形在实践中无法发生，因为样本
 * 大小至少为 300。n 趋近 N 的情形对应于
 * 对整张表抽样，那种情况下保留
 * 整个 MCV 列表（没有下界）是合理的，因此对所有输入
 * 应用该公式是有意义的，尽管上面的推导
 * 在严格意义上只在右端大约至少为 10 时
 * 才有效。
 *
 * 看待该公式的另一种方式如下：假定
 * 该值被见到的实例数按比例放大到整张
 * 表，从而总体计数为 K=N*cnt/n。于是样本中的分布
 * 是由 N、n 和 K 参数化的超几何分布，
 * 而上面的界在数学上等价于：该分布的标准差须
 * 小于其均值的 20%。因此，由 MCV 统计信息产生的
 * 任何规划器估计的相对误差
 * 不太可能太大。
 */
static double
get_mincount_for_mcv_list(int samplerows, double totalrows)
{
	double		n = samplerows;
	double		N = totalrows;
	double		numer,
				denom;

	numer = n * (N - n);
	denom = N - n + 0.04 * n * (N - 1);

	/* 防止除以零（若 n = N = 1 就可能发生） */
	if (denom == 0.0)
		return 0.0;

	return numer / denom;
}

/*
 * 根据采样行的集合构造 MCV 列表。
 *
 * 算法相当简单：
 *
 *	   (1) 对数据排序（默认排序规则，该数据类型的 '<'）。
 *
 *	   (2) 统计不同的组，决定保留多少。
 *
 *	   (3) 用 (2) 里确定的阈值构造 MCV 列表。
 *
 *	   (4) 从样本中去掉由 MCV 表示的行
 *
 */
MCVList *
statext_mcv_build(StatsBuildData *data, double totalrows, int stattarget)
{
	int			i,
				numattrs,
				numrows,
				ngroups,
				nitems;
	double		mincount;
	SortItem   *items;
	SortItem   *groups;
	MCVList    *mcvlist = NULL;
	MultiSortSupport mss;

	/* 所有列的比较器 */
	mss = build_mss(data);

	/* 对行排序 */
	items = build_sorted_items(data, &nitems, mss,
							   data->nattnums, data->attnums);

	if (!items)
		return NULL;

	/* 为方便起见 */
	numattrs = data->nattnums;
	numrows = data->numrows;

	/* 把排好序的行变成组（按频率排序） */
	groups = build_distinct_groups(nitems, items, mss, &ngroups);

	/*
	 * 要存储的 MCV 项最大个数，基于我们为该统计对象
	 * 算出的统计目标（来自对象本身、属性以及系统默认
	 * 设置的目标）。无论如何，我们
	 * 没法保留比现有组更多的组。
	 */
	nitems = stattarget;
	if (nitems > ngroups)
		nitems = ngroups;

	/*
	 * 决定 MCV 列表里保留多少项。我们没法使用与
	 * 按列 MCV 列表相同的算法，因为那个算法只考虑
	 * 实际的组频率，而我们主要关心实际频率与基频率
	 * （简单按列频率的乘积，仿佛这些列相互独立）
	 * 相差多少。
	 *
	 * 使用同一算法可能会排除接近样本
	 * 平均频率的项。但那并不能说明观察到的
	 * 频率是否接近基频率。我们还需要
	 * 考虑出乎意料地不常见的项（同样是相对基频率），
	 * 而单列算法不必考虑这一点。
	 *
	 * 我们只是用 get_mincount_for_mcv_list() 算出最小计数，
	 * 从而决定保留多少项，然后保留所有看起来比
	 * 那个计数更常见的项。
	 */
	mincount = get_mincount_for_mcv_list(numrows, totalrows);

	/*
	 * 沿着这些组走，直到找到第一个计数低于
	 * mincount 阈值的组（该组的下标就是我们
	 * 想保留的组数）。
	 */
	for (i = 0; i < nitems; i++)
	{
		if (groups[i].count < mincount)
		{
			nitems = i;
			break;
		}
	}

	/*
	 * 至此我们知道 MCV 列表的项数。可能
	 * 一个都没有（组很多的均匀分布），那种
	 * 情况下就不会有 MCV 列表。否则就构造 MCV 列表。
	 */
	if (nitems > 0)
	{
		int			j;
		SortItem	key;
		MultiSortSupport tmp;

		/* 每个属性里的值的频率 */
		SortItem  **freqs;
		int		   *nfreqs;

		/* 用于搜索值 */
		tmp = (MultiSortSupport) palloc(offsetof(MultiSortSupportData, ssup)
										+ sizeof(SortSupportData));

		/* 计算每一列里的值的频率 */
		nfreqs = (int *) palloc0(sizeof(int) * numattrs);
		freqs = build_column_frequencies(groups, ngroups, mss, nfreqs);

		/*
		 * 分配 MCV 列表结构，设置全局参数。
		 */
		mcvlist = (MCVList *) palloc0(offsetof(MCVList, items) +
									  sizeof(MCVItem) * nitems);

		mcvlist->magic = STATS_MCV_MAGIC;
		mcvlist->type = STATS_MCV_TYPE_BASIC;
		mcvlist->ndimensions = numattrs;
		mcvlist->nitems = nitems;

		/* 存储数据类型 OIDs 的信息 */
		for (i = 0; i < numattrs; i++)
			mcvlist->types[i] = data->stats[i]->attrtypid;

		/* 把第一段组复制进结果。 */
		for (i = 0; i < nitems; i++)
		{
			/* 只是指向列表里的正确位置 */
			MCVItem    *item = &mcvlist->items[i];

			item->values = (Datum *) palloc(sizeof(Datum) * numattrs);
			item->isnull = (bool *) palloc(sizeof(bool) * numattrs);

			/* 复制该组的值 */
			memcpy(item->values, groups[i].values, sizeof(Datum) * numattrs);
			memcpy(item->isnull, groups[i].isnull, sizeof(bool) * numattrs);

			/* 组应当按频率降序排列 */
			Assert((i == 0) || (groups[i - 1].count >= groups[i].count));

			/* 组频率 */
			item->frequency = (double) groups[i].count / numrows;

			/* 若这些属性相互独立时的基频率 */
			item->base_frequency = 1.0;
			for (j = 0; j < numattrs; j++)
			{
				SortItem   *freq;

				/* 单个维度 */
				tmp->ndims = 1;
				tmp->ssup[0] = mss->ssup[j];

				/* 填充查找键 */
				key.values = &groups[i].values[j];
				key.isnull = &groups[i].isnull[j];

				freq = (SortItem *) bsearch_arg(&key, freqs[j], nfreqs[j],
												sizeof(SortItem),
												multi_sort_compare, tmp);

				item->base_frequency *= ((double) freq->count) / numrows;
			}
		}

		pfree(nfreqs);
		pfree(freqs);
	}

	pfree(items);
	pfree(groups);

	return mcvlist;
}

/*
 * build_mss
 *		为给定的 StatsBuildData 构造一个 MultiSortSupport。
 */
static MultiSortSupport
build_mss(StatsBuildData *data)
{
	int			i;
	int			numattrs = data->nattnums;

	/* 按多列排序（使用 SortSupport 数组） */
	MultiSortSupport mss = multi_sort_init(numattrs);

	/* 为所有属性准备排序函数 */
	for (i = 0; i < numattrs; i++)
	{
		VacAttrStats *colstat = data->stats[i];
		TypeCacheEntry *type;

		type = lookup_type_cache(colstat->attrtypid, TYPECACHE_LT_OPR);
		if (type->lt_opr == InvalidOid) /* 不应当发生 */
			elog(ERROR, "cache lookup failed for ordering operator for type %u",
				 colstat->attrtypid);

		multi_sort_add_dimension(mss, i, type->lt_opr, colstat->attrcollid);
	}

	return mss;
}

/*
 * count_distinct_groups
 *		统计数组里不同的 SortItems 组合个数。
 *
 * 假定该数组已按 MultiSortSupport 排序。
 */
static int
count_distinct_groups(int numrows, SortItem *items, MultiSortSupport mss)
{
	int			i;
	int			ndistinct;

	ndistinct = 1;
	for (i = 1; i < numrows; i++)
	{
		/* 确保该数组确实已排序 */
		Assert(multi_sort_compare(&items[i], &items[i - 1], mss) >= 0);

		if (multi_sort_compare(&items[i], &items[i - 1], mss) != 0)
			ndistinct += 1;
	}

	return ndistinct;
}

/*
 * compare_sort_item_count
 *		按计数（频率）降序
 *		对项排序的比较器。
 */
static int
compare_sort_item_count(const void *a, const void *b, void *arg)
{
	SortItem   *ia = (SortItem *) a;
	SortItem   *ib = (SortItem *) b;

	if (ia->count == ib->count)
		return 0;
	else if (ia->count > ib->count)
		return -1;

	return 1;
}

/*
 * build_distinct_groups
 *		为不同的组以及匹配项的计数构造 SortItems
 *		数组。
 *
 * 假定 items 数组已排序。
 */
static SortItem *
build_distinct_groups(int numrows, SortItem *items, MultiSortSupport mss,
					  int *ndistinct)
{
	int			i,
				j;
	int			ngroups = count_distinct_groups(numrows, items, mss);

	SortItem   *groups = (SortItem *) palloc(ngroups * sizeof(SortItem));

	j = 0;
	groups[0] = items[0];
	groups[0].count = 1;

	for (i = 1; i < numrows; i++)
	{
		/* 假定按升序排列。 */
		Assert(multi_sort_compare(&items[i], &items[i - 1], mss) >= 0);

		/* 检测到新的不同组。 */
		if (multi_sort_compare(&items[i], &items[i - 1], mss) != 0)
		{
			groups[++j] = items[i];
			groups[j].count = 0;
		}

		groups[j].count++;
	}

	/* 确保我们填入了预期个数的不同组 */
	Assert(j + 1 == ngroups);

	/* 按频率（降序）对不同的组排序。 */
	qsort_interruptible(groups, ngroups, sizeof(SortItem),
						compare_sort_item_count, NULL);

	*ndistinct = ngroups;
	return groups;
}

/* 比较排序项（单个维度） */
static int
sort_item_compare(const void *a, const void *b, void *arg)
{
	SortSupport ssup = (SortSupport) arg;
	SortItem   *ia = (SortItem *) a;
	SortItem   *ib = (SortItem *) b;

	return ApplySortComparator(ia->values[0], ia->isnull[0],
							   ib->values[0], ib->isnull[0],
							   ssup);
}

/*
 * build_column_frequencies
 *		计算每一列里的值的频率。
 *
 * 这为 MCV 所基于的每个属性返回一个 SortItems 数组，
 * 其中每个值都有一个频率（出现次数）。这随后
 * 用来计算 MCV 项的基频率。
 *
 * 所有内存都分配在单个块里，因此一次 pfree
 * 就足以释放。我们不为 SortItems 里的 values/isnull
 * 数组分配空间，因为可以直接指向输入
 * 组。
 */
static SortItem **
build_column_frequencies(SortItem *groups, int ngroups,
						 MultiSortSupport mss, int *ncounts)
{
	int			i,
				dim;
	SortItem  **result;
	char	   *ptr;

	Assert(groups);
	Assert(ncounts);

	/* 把所有列的数组作为单个块来分配 */
	ptr = palloc(MAXALIGN(sizeof(SortItem *) * mss->ndims) +
				 mss->ndims * MAXALIGN(sizeof(SortItem) * ngroups));

	/* 指针的初始数组 */
	result = (SortItem **) ptr;
	ptr += MAXALIGN(sizeof(SortItem *) * mss->ndims);

	for (dim = 0; dim < mss->ndims; dim++)
	{
		SortSupport ssup = &mss->ssup[dim];

		/* 单列的值数组 */
		result[dim] = (SortItem *) ptr;
		ptr += MAXALIGN(sizeof(SortItem) * ngroups);

		/* 提取该维度的数据 */
		for (i = 0; i < ngroups; i++)
		{
			/* 指向输入组 */
			result[dim][i].values = &groups[i].values[dim];
			result[dim][i].isnull = &groups[i].isnull[dim];
			result[dim][i].count = groups[i].count;
		}

		/* 对值排序并去重 */
		qsort_interruptible(result[dim], ngroups, sizeof(SortItem),
							sort_item_compare, ssup);

		/*
		 * 识别不同的值并计算频率（可能有
		 * 多个含该值的 MCV 项，因此需要把
		 * 它们的计数全部加起来。
		 */
		ncounts[dim] = 1;
		for (i = 1; i < ngroups; i++)
		{
			if (sort_item_compare(&result[dim][i - 1], &result[dim][i], ssup) == 0)
			{
				result[dim][ncounts[dim] - 1].count += result[dim][i].count;
				continue;
			}

			result[dim][ncounts[dim]] = result[dim][i];

			ncounts[dim]++;
		}
	}

	return result;
}

/*
 * statext_mcv_load
 *		为指定的 pg_statistic_ext_data 元组装载 MCV 列表。
 */
MCVList *
statext_mcv_load(Oid mvoid, bool inh)
{
	MCVList    *result;
	bool		isnull;
	Datum		mcvlist;
	HeapTuple	htup = SearchSysCache2(STATEXTDATASTXOID,
									   ObjectIdGetDatum(mvoid), BoolGetDatum(inh));

	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for statistics object %u", mvoid);

	mcvlist = SysCacheGetAttr(STATEXTDATASTXOID, htup,
							  Anum_pg_statistic_ext_data_stxdmcv, &isnull);

	if (isnull)
		elog(ERROR,
			 "requested statistics kind \"%c\" is not yet built for statistics object %u",
			 STATS_EXT_MCV, mvoid);

	result = statext_mcv_deserialize(DatumGetByteaP(mcvlist));

	ReleaseSysCache(htup);

	return result;
}


/*
 * statext_mcv_serialize
 *		把 MCV 列表序列化成 pg_mcv_list 值。
 *
 * MCV 项可能含各种数据类型的值，而且有理由
 * 预期同一属性的值会在多个
 * MCV 列表项里重复。因此我们把值去重放进数组，然后用
 * 指向那些数组的下标替换这些值。
 *
 * 序列化表示的整体结构如下：
 *
 * +---------------+----------------+---------------------+-------+
 * | 头成员 | 维度信息 | 去重后的值 | 项 |：
 * +---------------+----------------+---------------------+-------+
 *
 * 维度信息存放第 K 个
 * 属性的类型信息（例如 typlen、typbyval，以及去重后的值的长度）。
 * 去重后的值存放每个属性去重后的值。而
 * 项存放实际的 MCV 列表项，值被替换成指向
 * 这些数组的下标。
 *
 * 序列化这些项时，我们使用 uint16 下标。MCV 项的个数
 * 受统计目标限制（目前上限是 10k）。
 * 我们也许可以把上限提高到 65k，仍然放得进 uint16，所以还有一点
 * 余量。此外，这个限制是针对每列不同值的个数，
 * 而我们通常只有很少这样的值（以及它们在
 * 那些 MCV 列表里的各种组合）。因此目前 uint16 似乎够用。
 *
 * 我们并不真预期序列化能像直方图那样省下那么多空间，
 * 因为我们不做任何桶拆分（那是直方图里
 * 高度冗余的来源）。
 *
 * TODO：考虑把每个项的布尔标志（NULL）打包进单个 char
 * （或更长的类型），而不是使用 bool 项的数组。
 */
bytea *
statext_mcv_serialize(MCVList *mcvlist, VacAttrStats **stats)
{
	int			i;
	int			dim;
	int			ndims = mcvlist->ndimensions;

	SortSupport ssup;
	DimensionInfo *info;

	Size		total_length;

	/* 序列化的项（指向数组的下标等） */
	bytea	   *raw;
	char	   *ptr;
	char	   *endptr PG_USED_FOR_ASSERTS_ONLY;

	/* 每个维度的值（以及非 NULL 值的个数） */
	Datum	  **values = (Datum **) palloc0(sizeof(Datum *) * ndims);
	int		   *counts = (int *) palloc0(sizeof(int) * ndims);

	/*
	 * 我们会加入一些关于属性类型的粗略信息
	 * （长度、按值标志），这样反序列化 MCV 列表时就不必查找它们
	 * （头里已经有类型 OID）。这是安全的，因为改变属性类型时
	 * 统计信息会自动被删除。反正我们也需要
	 * 存储关于去重值
	 * 数组的信息。
	 */
	info = (DimensionInfo *) palloc0(sizeof(DimensionInfo) * ndims);

	/* MCV 列表所含全部属性的排序支持数据 */
	ssup = (SortSupport) palloc0(sizeof(SortSupportData) * ndims);

	/* 为每个维度（属性）收集并去重值 */
	for (dim = 0; dim < ndims; dim++)
	{
		int			ndistinct;
		TypeCacheEntry *typentry;

		/*
		 * 查找 LT 操作符（没法从统计的 extra_data 取得，因为我们
		 * 不知道如何解释它：标量还是数组等）。
		 */
		typentry = lookup_type_cache(stats[dim]->attrtypid, TYPECACHE_LT_OPR);

		/* 复制数据类型的重要信息（长度、是否按值） */
		info[dim].typlen = stats[dim]->attrtype->typlen;
		info[dim].typbyval = stats[dim]->attrtype->typbyval;

		/* 为该属性里的值分配空间并收集它们 */
		values[dim] = (Datum *) palloc0(sizeof(Datum) * mcvlist->nitems);

		for (i = 0; i < mcvlist->nitems; i++)
		{
			/* 跳过 NULL 值：那些不需要去重 */
			if (mcvlist->items[i].isnull[dim])
				continue;

			/* 把该值追加到末尾 */
			values[dim][counts[dim]] = mcvlist->items[i].values[dim];
			counts[dim] += 1;
		}

		/* 若该维度里全是 NULL 值，就完成了 */
		if (counts[dim] == 0)
			continue;

		/* 对数据排序并去重 */
		ssup[dim].ssup_cxt = CurrentMemoryContext;
		ssup[dim].ssup_collation = stats[dim]->attrcollid;
		ssup[dim].ssup_nulls_first = false;

		PrepareSortSupportFromOrderingOp(typentry->lt_opr, &ssup[dim]);

		qsort_interruptible(values[dim], counts[dim], sizeof(Datum),
							compare_scalars_simple, &ssup[dim]);

		/*
		 * 遍历数组并消除重复值，但保持
		 * 顺序（以便稍后做二分查找）。我们知道至少有
		 * 一项，因为 (counts[dim] != 0)，因此可以跳过第一个
		 * 元素。
		 */
		ndistinct = 1;			/* 不同值的个数 */
		for (i = 1; i < counts[dim]; i++)
		{
			/* 预期是已排序的数组 */
			Assert(compare_datums_simple(values[dim][i - 1], values[dim][i], &ssup[dim]) <= 0);

			/* 若该值与前一个相同，就可以跳过 */
			if (!compare_datums_simple(values[dim][i - 1], values[dim][i], &ssup[dim]))
				continue;

			values[dim][ndistinct] = values[dim][i];
			ndistinct += 1;
		}

		/* 我们不得超过 PG_UINT16_MAX，因为使用的是 uint16 下标 */
		Assert(ndistinct <= PG_UINT16_MAX);

		/*
		 * 存储关于该属性的额外信息：去重后的
		 * 值的个数，以及序列化数据的大小。对定长数据
		 * 类型这很容易计算，对变宽类型则需要
		 * 实际遍历数组并把大小加起来。
		 */
		info[dim].nvalues = ndistinct;

		if (info[dim].typbyval) /* 按值传递的数据类型 */
		{
			info[dim].nbytes = info[dim].nvalues * info[dim].typlen;

			/*
			 * 反序列化时我们把数据复制进 MCV 项，因此
			 * 不必分配任何额外空间。
			 */
			info[dim].nbytes_aligned = 0;
		}
		else if (info[dim].typlen > 0)	/* 定长的按引用传递 */
		{
			/*
			 * 我们不关心序列化数据里的对齐，因此尽可能
			 * 把数据打包。但我们也跟踪反序列化之后
			 * 需要多少数据，那种情况下
			 * 必须计入每一项的对齐。
			 *
			 * 注意：由于这些项是定长的，反序列化时很容易
			 * 计算这一点，但我们还是在这里做。
			 */
			info[dim].nbytes = info[dim].nvalues * info[dim].typlen;
			info[dim].nbytes_aligned = info[dim].nvalues * MAXALIGN(info[dim].typlen);
		}
		else if (info[dim].typlen == -1)	/* varlena（变长） */
		{
			info[dim].nbytes = 0;
			info[dim].nbytes_aligned = 0;
			for (i = 0; i < info[dim].nvalues; i++)
			{
				Size		len;

				/*
				 * 对 varlena 值，我们把值去 toast，并分别存储
				 * 长度和数据。这里不费心对齐，
				 * 这意味着反序列化时需要
				 * 把成员复制出来，并且只访问副本。
				 */
				values[dim][i] = PointerGetDatum(PG_DETOAST_DATUM(values[dim][i]));

				/* 序列化长度（uint32 长度 + 数据） */
				len = VARSIZE_ANY_EXHDR(values[dim][i]);
				info[dim].nbytes += sizeof(uint32); /* 长度 */
				info[dim].nbytes += len;	/* 值（没有头） */

				/*
				 * 反序列化时我们将构造带完整头的普通 varlena 值，
				 * 并且需要正确地对齐它们。
				 */
				info[dim].nbytes_aligned += MAXALIGN(VARHDRSZ + len);
			}
		}
		else if (info[dim].typlen == -2)	/* cstring（C 字符串） */
		{
			info[dim].nbytes = 0;
			info[dim].nbytes_aligned = 0;
			for (i = 0; i < info[dim].nvalues; i++)
			{
				Size		len;

				/*
				 * cstring 的处理与 varlena 类似：先把长度存成
				 * uint32，再存数据。我们不关心
				 * 对齐，这意味着反序列化时需要
				 * 把成员复制出来，并且只访问副本。
				 */

				/* c 字符串含终止符，因此 +1 字节 */
				len = strlen(DatumGetCString(values[dim][i])) + 1;
				info[dim].nbytes += sizeof(uint32); /* 长度 */
				info[dim].nbytes += len;	/* 值 */

				/* 正确对齐的反序列化副本所需的空间 */
				info[dim].nbytes_aligned += MAXALIGN(len);
			}
		}

		/* 我们知道 (count>0)，因此一定有一些数据 */
		Assert(info[dim].nbytes > 0);
	}

	/*
	 * 现在终于可以计算整个序列化 MCV 列表实际需要多少空间
	 * （varlena 头、MCV 头、每个属性的维度信息、
	 * 去重后的值和项）。
	 */
	total_length = (3 * sizeof(uint32)) /* magic + type + nitems（魔数、类型与项数） */
		+ sizeof(AttrNumber)	/* ndimensions（维数） */
		+ (ndims * sizeof(Oid));	/* 属性类型 */

	/* 维度信息 */
	total_length += ndims * sizeof(DimensionInfo);

	/* 加上去重值数组的空间 */
	for (i = 0; i < ndims; i++)
		total_length += info[i].nbytes;

	/*
	 * 最后计入这些项（由于把值替换成指向去重数组的
	 * uint16 下标，它们是定长的）。
	 */
	total_length += mcvlist->nitems * ITEM_SIZE(dim);

	/*
	 * 为整个序列化 MCV 列表分配空间（我们会跳过一些字节，因此
	 * 把它们设为零，使结果更容易压缩）。
	 */
	raw = (bytea *) palloc0(VARHDRSZ + total_length);
	SET_VARSIZE(raw, VARHDRSZ + total_length);

	ptr = VARDATA(raw);
	endptr = ptr + total_length;

	/* 逐个复制 MCV 列表的头成员 */
	memcpy(ptr, &mcvlist->magic, sizeof(uint32));
	ptr += sizeof(uint32);

	memcpy(ptr, &mcvlist->type, sizeof(uint32));
	ptr += sizeof(uint32);

	memcpy(ptr, &mcvlist->nitems, sizeof(uint32));
	ptr += sizeof(uint32);

	memcpy(ptr, &mcvlist->ndimensions, sizeof(AttrNumber));
	ptr += sizeof(AttrNumber);

	memcpy(ptr, mcvlist->types, sizeof(Oid) * ndims);
	ptr += (sizeof(Oid) * ndims);

	/* 存储关于属性的信息（数据量等） */
	memcpy(ptr, info, sizeof(DimensionInfo) * ndims);
	ptr += sizeof(DimensionInfo) * ndims;

	/* 把所有属性去重后的值复制到输出。 */
	for (dim = 0; dim < ndims; dim++)
	{
		/* 记住起点，供稍后的 Assert 使用 */
		char	   *start PG_USED_FOR_ASSERTS_ONLY = ptr;

		for (i = 0; i < info[dim].nvalues; i++)
		{
			Datum		value = values[dim][i];

			if (info[dim].typbyval) /* 按值传递 */
			{
				Datum		tmp;

				/*
				 * 对按值类型，我们只需复制有意义的字节。
				 * 不能直接用 memcpy，因为它假定
				 * 小端行为。store_att_byval 几乎做了我们
				 * 需要的事，但它需要正确对齐的缓冲区，而
				 * 输出缓冲区并不保证这一点。因此我们简单地使用一个
				 * 局部 Datum 变量（它保证正确对齐），
				 * 然后再从它复制该值。
				 */
				store_att_byval(&tmp, value, info[dim].typlen);

				memcpy(ptr, &tmp, info[dim].typlen);
				ptr += info[dim].typlen;
			}
			else if (info[dim].typlen > 0)	/* 按引用传递 */
			{
				/* 不需要特殊对齐，当作字符数组处理 */
				memcpy(ptr, DatumGetPointer(value), info[dim].typlen);
				ptr += info[dim].typlen;
			}
			else if (info[dim].typlen == -1)	/* varlena（变长） */
			{
				uint32		len = VARSIZE_ANY_EXHDR(DatumGetPointer(value));

				/* 复制长度 */
				memcpy(ptr, &len, sizeof(uint32));
				ptr += sizeof(uint32);

				/* 来自 varlena 值的数据（不含头） */
				memcpy(ptr, VARDATA_ANY(DatumGetPointer(value)), len);
				ptr += len;
			}
			else if (info[dim].typlen == -2)	/* cstring（C 字符串） */
			{
				uint32		len = (uint32) strlen(DatumGetCString(value)) + 1;

				/* 复制长度 */
				memcpy(ptr, &len, sizeof(uint32));
				ptr += sizeof(uint32);

				/* 值 */
				memcpy(ptr, DatumGetCString(value), len);
				ptr += len;
			}

			/* 没有下溢或上溢 */
			Assert((ptr > start) && ((ptr - start) <= info[dim].nbytes));
		}

		/* 该维度应当恰好得到 nbytes 的数据 */
		Assert((ptr - start) == info[dim].nbytes);
	}

	/* 序列化这些项，用 uint16 下标代替值。 */
	for (i = 0; i < mcvlist->nitems; i++)
	{
		MCVItem    *mcvitem = &mcvlist->items[i];

		/* 不要写到已分配空间之外 */
		Assert(ptr <= (endptr - ITEM_SIZE(dim)));

		/* 把 NULL 和频率标志复制进序列化的 MCV */
		memcpy(ptr, mcvitem->isnull, sizeof(bool) * ndims);
		ptr += sizeof(bool) * ndims;

		memcpy(ptr, &mcvitem->frequency, sizeof(double));
		ptr += sizeof(double);

		memcpy(ptr, &mcvitem->base_frequency, sizeof(double));
		ptr += sizeof(double);

		/* 最后存储下标 */
		for (dim = 0; dim < ndims; dim++)
		{
			uint16		index = 0;
			Datum	   *value;

			/* 只对非 NULL 值做查找 */
			if (!mcvitem->isnull[dim])
			{
				value = (Datum *) bsearch_arg(&mcvitem->values[dim], values[dim],
											  info[dim].nvalues, sizeof(Datum),
											  compare_scalars_simple, &ssup[dim]);

				Assert(value != NULL);	/* 序列化或去重
										 * 出错 */

				/* 计算在去重数组里的下标 */
				index = (uint16) (value - values[dim]);

				/* 检查下标是否在预期范围内 */
				Assert(index < info[dim].nvalues);
			}

			/* 把下标复制进序列化的 MCV */
			memcpy(ptr, &index, sizeof(uint16));
			ptr += sizeof(uint16);
		}

		/* 确保我们不超出已分配的值 */
		Assert(ptr <= endptr);
	}

	/* 此时我们预期恰好匹配 total_length */
	Assert(ptr == endptr);

	pfree(values);
	pfree(counts);

	return raw;
}

/*
 * statext_mcv_deserialize
 *		把序列化的 MCV 列表读入 MCVList 结构。
 *
 * MCV 列表所需的全部内存都作为单个块分配，因此
 * 可以一次 pfree() 掉。
 */
MCVList *
statext_mcv_deserialize(bytea *data)
{
	int			dim,
				i;
	Size		expected_size;
	MCVList    *mcvlist;
	char	   *raw;
	char	   *ptr;
	char	   *endptr PG_USED_FOR_ASSERTS_ONLY;

	int			ndims,
				nitems;
	DimensionInfo *info = NULL;

	/* 本地分配缓冲区（只用于反序列化） */
	Datum	  **map = NULL;

	/* MCV 列表 */
	Size		mcvlen;

	/* 用于结果的缓冲区 */
	Size		datalen;
	char	   *dataptr;
	char	   *valuesptr;
	char	   *isnullptr;

	if (data == NULL)
		return NULL;

	/*
	 * 若连完整的头都没有，就不可能反序列化 MCV 列表。
	 * 这里需要一个显式公式，因为我们是逐个序列化
	 * 头成员的，因此必须忽略结构对齐。
	 */
	if (VARSIZE_ANY(data) < MinSizeOfMCVList)
		elog(ERROR, "invalid MCV size %zu (expected at least %zu)",
			 VARSIZE_ANY(data), MinSizeOfMCVList);

	/* 读取 MCV 列表头 */
	mcvlist = (MCVList *) palloc0(offsetof(MCVList, items));

	/* 指向数据部分的指针（跳过 varlena 头） */
	raw = (char *) data;
	ptr = VARDATA_ANY(raw);
	endptr = (char *) raw + VARSIZE_ANY(data);

	/* 取得头并做进一步的健全性检查 */
	memcpy(&mcvlist->magic, ptr, sizeof(uint32));
	ptr += sizeof(uint32);

	memcpy(&mcvlist->type, ptr, sizeof(uint32));
	ptr += sizeof(uint32);

	memcpy(&mcvlist->nitems, ptr, sizeof(uint32));
	ptr += sizeof(uint32);

	memcpy(&mcvlist->ndimensions, ptr, sizeof(AttrNumber));
	ptr += sizeof(AttrNumber);

	if (mcvlist->magic != STATS_MCV_MAGIC)
		elog(ERROR, "invalid MCV magic %u (expected %u)",
			 mcvlist->magic, STATS_MCV_MAGIC);

	if (mcvlist->type != STATS_MCV_TYPE_BASIC)
		elog(ERROR, "invalid MCV type %u (expected %u)",
			 mcvlist->type, STATS_MCV_TYPE_BASIC);

	if (mcvlist->ndimensions == 0)
		elog(ERROR, "invalid zero-length dimension array in MCVList");
	else if ((mcvlist->ndimensions > STATS_MAX_DIMENSIONS) ||
			 (mcvlist->ndimensions < 0))
		elog(ERROR, "invalid length (%d) dimension array in MCVList",
			 mcvlist->ndimensions);

	if (mcvlist->nitems == 0)
		elog(ERROR, "invalid zero-length item array in MCVList");
	else if (mcvlist->nitems > STATS_MCVLIST_MAX_ITEMS)
		elog(ERROR, "invalid length (%u) item array in MCVList",
			 mcvlist->nitems);

	nitems = mcvlist->nitems;
	ndims = mcvlist->ndimensions;

	/*
	 * 检查数据量，包括所有维度的 DimensionInfo，以及
	 * 序列化的项（含 uint16 下标）。同时遍历
	 * 维度信息并把它加进总和。
	 */
	expected_size = SizeOfMCVList(ndims, nitems);

	/*
	 * 检查至少有维度和信息记录，以及
	 * 这些项。我们还不知道序列化值的大小。需要
	 * 先做这项检查，然后再访问维度信息。
	 */
	if (VARSIZE_ANY(data) < expected_size)
		elog(ERROR, "invalid MCV size %zu (expected %zu)",
			 VARSIZE_ANY(data), expected_size);

	/* 现在复制类型 Oid 的数组。 */
	memcpy(mcvlist->types, ptr, sizeof(Oid) * ndims);
	ptr += (sizeof(Oid) * ndims);

	/* 现在可以安全访问维度信息。 */
	info = palloc(ndims * sizeof(DimensionInfo));

	memcpy(info, ptr, ndims * sizeof(DimensionInfo));
	ptr += (ndims * sizeof(DimensionInfo));

	/* 计入值数组 */
	for (dim = 0; dim < ndims; dim++)
	{
		/*
		 * XXX 我不确定这里能否依赖断言。也许这些
		 * 检查每次都该做？
		 */
		Assert(info[dim].nvalues >= 0);
		Assert(info[dim].nbytes >= 0);

		expected_size += info[dim].nbytes;
	}

	/*
	 * 现在我们知道预期的 MCV 总大小，包括所有部分
	 * （头、维度信息、项和去重后的数据）。于是做最后的
	 * 大小检查。
	 */
	if (VARSIZE_ANY(data) != expected_size)
		elog(ERROR, "invalid MCV size %zu (expected %zu)",
			 VARSIZE_ANY(data), expected_size);

	/*
	 * 每个维度都需要一个 Datum 值数组，以便稍后
	 * 方便地把 uint16 下标翻译过来。还需要一个顶层
	 * 指向这些按维度数组的指针数组。
	 *
	 * 分配各维度的数组时，计算按引用数据的副本
	 * 需要多少空间，因为没法直接指向
	 * 原始值（它可能会消失）。
	 */
	datalen = 0;				/* 按引用数据的空间 */
	map = (Datum **) palloc(ndims * sizeof(Datum *));

	for (dim = 0; dim < ndims; dim++)
	{
		map[dim] = (Datum *) palloc(sizeof(Datum) * info[dim].nvalues);

		/* 按引用类型的数据副本所需空间 */
		datalen += info[dim].nbytes_aligned;
	}

	/*
	 * 现在调整 MCV 列表的大小，使分配包含全部数据。
	 *
	 * 为数据副本分配空间，因为没法直接引用
	 * 序列化数据：它没有正确对齐，而且在我们仍在使用
	 * MCV 列表时它可能消失，例如因为 catcache 释放。
	 *
	 * 这里我们确实关心对齐，因为会一次性分配所有
	 * 部分，然后用指针指向不同的部分。
	 */
	mcvlen = MAXALIGN(offsetof(MCVList, items) + (sizeof(MCVItem) * nitems));

	/* 所有 MCV 项的值和 isnull 标志数组 */
	mcvlen += nitems * MAXALIGN(sizeof(Datum) * ndims);
	mcvlen += nitems * MAXALIGN(sizeof(bool) * ndims);

	/* 并不真需要对齐它，但这样让某些断言更容易 */
	mcvlen += MAXALIGN(datalen);

	/* 现在调整反序列化后的 MCV 列表大小，并计算各部分的指针 */
	mcvlist = repalloc(mcvlist, mcvlen);

	/* 指向 values/isnull 数组开头的指针 */
	valuesptr = (char *) mcvlist
		+ MAXALIGN(offsetof(MCVList, items) + (sizeof(MCVItem) * nitems));

	isnullptr = valuesptr + (nitems * MAXALIGN(sizeof(Datum) * ndims));

	dataptr = isnullptr + (nitems * MAXALIGN(sizeof(bool) * ndims));

	/*
	 * 建立映射（下标 => 值），把序列化数据翻译成
	 * 内存中的表示。
	 */
	for (dim = 0; dim < ndims; dim++)
	{
		/* 记住输入数组里的起始位置 */
		char	   *start PG_USED_FOR_ASSERTS_ONLY = ptr;

		if (info[dim].typbyval)
		{
			/* 对按值类型，我们只是把数据复制进映射 */
			for (i = 0; i < info[dim].nvalues; i++)
			{
				Datum		v = 0;

				memcpy(&v, ptr, info[dim].typlen);
				ptr += info[dim].typlen;

				map[dim][i] = fetch_att(&v, true, info[dim].typlen);

				/* 输入数组没有下溢或上溢 */
				Assert(ptr <= (start + info[dim].nbytes));
			}
		}
		else
		{
			/* 对按引用类型，还需要复制一份数据 */

			/* 按引用传递，但是定长（name、tid 等） */
			if (info[dim].typlen > 0)
			{
				for (i = 0; i < info[dim].nvalues; i++)
				{
					memcpy(dataptr, ptr, info[dim].typlen);
					ptr += info[dim].typlen;

					/* 只是指向数组内部 */
					map[dim][i] = PointerGetDatum(dataptr);
					dataptr += MAXALIGN(info[dim].typlen);
				}
			}
			else if (info[dim].typlen == -1)
			{
				/* varlena（变长） */
				for (i = 0; i < info[dim].nvalues; i++)
				{
					uint32		len;

					/* 读取 uint32 长度 */
					memcpy(&len, ptr, sizeof(uint32));
					ptr += sizeof(uint32);

					/* 该长度只含数据 */
					SET_VARSIZE(dataptr, len + VARHDRSZ);
					memcpy(VARDATA(dataptr), ptr, len);
					ptr += len;

					/* 只是指向数组内部 */
					map[dim][i] = PointerGetDatum(dataptr);

					/* 跳到下一个反序列化值的位置 */
					dataptr += MAXALIGN(len + VARHDRSZ);
				}
			}
			else if (info[dim].typlen == -2)
			{
				/* cstring（C 字符串） */
				for (i = 0; i < info[dim].nvalues; i++)
				{
					uint32		len;

					memcpy(&len, ptr, sizeof(uint32));
					ptr += sizeof(uint32);

					memcpy(dataptr, ptr, len);
					ptr += len;

					/* 只是指向数组内部 */
					map[dim][i] = PointerGetDatum(dataptr);
					dataptr += MAXALIGN(len);
				}
			}

			/* 输入数组没有下溢或上溢 */
			Assert(ptr <= (start + info[dim].nbytes));

			/* 输出 mcv 值没有上溢 */
			Assert(dataptr <= ((char *) mcvlist + mcvlen));
		}

		/* 检查恰好消耗了该维度的输入数据 */
		Assert(ptr == (start + info[dim].nbytes));
	}

	/* 也应当恰好填满 MCV 列表 */
	Assert(dataptr == ((char *) mcvlist + mcvlen));

	/* 反序列化 MCV 项，并把下标翻译成 Datum */
	for (i = 0; i < nitems; i++)
	{
		MCVItem    *item = &mcvlist->items[i];

		item->values = (Datum *) valuesptr;
		valuesptr += MAXALIGN(sizeof(Datum) * ndims);

		item->isnull = (bool *) isnullptr;
		isnullptr += MAXALIGN(sizeof(bool) * ndims);

		memcpy(item->isnull, ptr, sizeof(bool) * ndims);
		ptr += sizeof(bool) * ndims;

		memcpy(&item->frequency, ptr, sizeof(double));
		ptr += sizeof(double);

		memcpy(&item->base_frequency, ptr, sizeof(double));
		ptr += sizeof(double);

		/* 最后翻译下标（仅非 NULL） */
		for (dim = 0; dim < ndims; dim++)
		{
			uint16		index;

			memcpy(&index, ptr, sizeof(uint16));
			ptr += sizeof(uint16);

			if (item->isnull[dim])
				continue;

			item->values[dim] = map[dim][index];
		}

		/* 检查没有溢出输入 */
		Assert(ptr <= endptr);
	}

	/* 检查已处理全部数据 */
	Assert(ptr == endptr);

	/* 释放用于映射的缓冲区 */
	for (dim = 0; dim < ndims; dim++)
		pfree(map[dim]);

	pfree(map);

	return mcvlist;
}

/*
 * 关于直方图桶细节的 SRF：
 *
 * - 项 ID（0...nitems）
 * - 值（字符串数组）
 * - 仅空（布尔数组）
 * - 频率（double precision）
 * - base_frequency（double precision）。
 *
 * 输入是统计信息的 OID，若统计信息不含直方图，
 * 就不返回任何行。
 */
Datum
pg_stats_ext_mcvlist_items(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	/* 只在函数第一次调用时做的事 */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		MCVList    *mcvlist;
		TupleDesc	tupdesc;

		/* 创建跨调用持久的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		/* 切换到适合多次函数调用的内存上下文 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		mcvlist = statext_mcv_deserialize(PG_GETARG_BYTEA_P(0));

		funcctx->user_fctx = mcvlist;

		/* 要返回的元组总数 */
		funcctx->max_calls = 0;
		if (funcctx->user_fctx != NULL)
			funcctx->max_calls = mcvlist->nitems;

		/* 为我们的结果类型构造一个元组描述符 */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
		tupdesc = BlessTupleDesc(tupdesc);

		/*
		 * 生成稍后从原始 C 字符串产生元组所需的
		 * 属性元数据
		 */
		funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);

		MemoryContextSwitchTo(oldcontext);
	}

	/* 每次调用函数时做的事 */
	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < funcctx->max_calls)	/* 还有更多要发送时
													 * 执行 */
	{
		Datum		values[5];
		bool		nulls[5];
		HeapTuple	tuple;
		Datum		result;
		ArrayBuildState *astate_values = NULL;
		ArrayBuildState *astate_nulls = NULL;

		int			i;
		MCVList    *mcvlist;
		MCVItem    *item;

		mcvlist = (MCVList *) funcctx->user_fctx;

		Assert(funcctx->call_cntr < mcvlist->nitems);

		item = &mcvlist->items[funcctx->call_cntr];

		for (i = 0; i < mcvlist->ndimensions; i++)
		{

			astate_nulls = accumArrayResult(astate_nulls,
											BoolGetDatum(item->isnull[i]),
											false,
											BOOLOID,
											CurrentMemoryContext);

			if (!item->isnull[i])
			{
				bool		isvarlena;
				Oid			outfunc;
				FmgrInfo	fmgrinfo;
				Datum		val;
				text	   *txt;

				/* 查找该类型的输出函数 */
				getTypeOutputInfo(mcvlist->types[i], &outfunc, &isvarlena);
				fmgr_info(outfunc, &fmgrinfo);

				val = FunctionCall1(&fmgrinfo, item->values[i]);
				txt = cstring_to_text(DatumGetPointer(val));

				astate_values = accumArrayResult(astate_values,
												 PointerGetDatum(txt),
												 false,
												 TEXTOID,
												 CurrentMemoryContext);
			}
			else
				astate_values = accumArrayResult(astate_values,
												 (Datum) 0,
												 true,
												 TEXTOID,
												 CurrentMemoryContext);
		}

		values[0] = Int32GetDatum(funcctx->call_cntr);
		values[1] = makeArrayResult(astate_values, CurrentMemoryContext);
		values[2] = makeArrayResult(astate_nulls, CurrentMemoryContext);
		values[3] = Float8GetDatum(item->frequency);
		values[4] = Float8GetDatum(item->base_frequency);

		/* 元组里没有 NULLs */
		memset(nulls, 0, sizeof(nulls));

		/* 构造一个元组 */
		tuple = heap_form_tuple(funcctx->attinmeta->tupdesc, values, nulls);

		/* 把元组做成 datum */
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else						/* 没有更多要发送时执行 */
	{
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * pg_mcv_list_in		- 类型 pg_mcv_list 的输入例程。
 *
 * pg_mcv_list 真实到足以作为表列，但它没有自己的操作，
 * 也不允许输入。
 */
Datum
pg_mcv_list_in(PG_FUNCTION_ARGS)
{
	/*
	 * pg_mcv_list 以二进制形式存放数据，不需要解析文本输入，
	 * 因此不允许这样做。
	 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_mcv_list")));

	PG_RETURN_VOID();			/* 消除编译器告警 */
}


/*
 * pg_mcv_list_out		- 类型 pg_mcv_list 的输出例程。
 *
 * MCV 列表被序列化成 bytea 值，因此我们直接调用 byteaout()
 * 把它序列化成文本。但最好能把它序列化成
 * 一种有意义的表示（例如供人查看）。
 *
 * XXX 这里也许应当返回某种有意义的内容，类似于
 * pg_dependencies_out 所做的。不过不确定如何处理去重后的
 * 值：要不要展开它们？
 */
Datum
pg_mcv_list_out(PG_FUNCTION_ARGS)
{
	return byteaout(fcinfo);
}

/*
 * pg_mcv_list_recv		- 类型 pg_mcv_list 的二进制输入例程。
 */
Datum
pg_mcv_list_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_mcv_list")));

	PG_RETURN_VOID();			/* 消除编译器告警 */
}

/*
 * pg_mcv_list_send		- 类型 pg_mcv_list 的二进制输出例程。
 *
 * MCV 列表被序列化在 bytea 值里（尽管类型名
 * 不同），因此直接发送它即可。
 */
Datum
pg_mcv_list_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}

/*
 * 把属性或表达式匹配到统计的某个维度。
 *
 * 返回匹配的统计维度的从零开始的下标。
 * 可选地确定排序规则。
 */
static int
mcv_match_expression(Node *expr, Bitmapset *keys, List *exprs, Oid *collid)
{
	int			idx;

	if (IsA(expr, Var))
	{
		/* 简单 Var，因此直接用 varattno 查找 */
		Var		   *var = (Var *) expr;

		if (collid)
			*collid = var->varcollid;

		idx = bms_member_index(keys, var->varattno);

		if (idx < 0)
			elog(ERROR, "variable not found in statistics object");
	}
	else
	{
		/* 表达式：在统计表达式里查找 */
		ListCell   *lc;

		if (collid)
			*collid = exprCollation(expr);

		/* 表达式存放在简单列之后 */
		idx = bms_num_members(keys);
		foreach(lc, exprs)
		{
			Node	   *stat_expr = (Node *) lfirst(lc);

			if (equal(expr, stat_expr))
				break;

			idx++;
		}

		if (lc == NULL)
			elog(ERROR, "expression not found in statistics object");
	}

	return idx;
}

/*
 * mcv_get_match_bitmap
 *	用 MCV 列表对子句求值，并更新匹配 bitmap。
 *
 * 匹配 bitmap 为每个 MCV 项保存匹配/不匹配状态，我们
 * 根据额外的子句更新它。我们也用它跳过
 * 不可能匹配的项（例如标记为不匹配的项，在求值 AND 子句列表时
 * 没法变成匹配）。
 *
 * 本函数还返回一个标志，表明是否对所有属性都有
 * 相等条件、MCV 列表里的最小频率，
 * 以及总的 MCV 频率（所有项的频率之和）。
 *
 * XXX 目前匹配 bitmap 对每个 MCV 项用一个 bool，这有些
 * 浪费，因为我们只用一位就够了，从而把
 * 大小减到大约 1/8。这也让我们能简单地用
 * & 和 | 组合 bitmap，这应当比 min/max 更快。不过这些 bitmap
 * 相当小（这要归功于 MCV 列表大小的上限）。
 */
static bool *
mcv_get_match_bitmap(PlannerInfo *root, List *clauses,
					 Bitmapset *keys, List *exprs,
					 MCVList *mcvlist, bool is_or)
{
	ListCell   *l;
	bool	   *matches;

	/* bitmap 可能只构造了一部分。 */
	Assert(clauses != NIL);
	Assert(mcvlist != NULL);
	Assert(mcvlist->nitems > 0);
	Assert(mcvlist->nitems <= STATS_MCVLIST_MAX_ITEMS);

	matches = palloc(sizeof(bool) * mcvlist->nitems);
	memset(matches, !is_or, sizeof(bool) * mcvlist->nitems);

	/*
	 * 遍历子句列表，对其中每条子句求值所有尚未被
	 * 前面的子句排除的 MCV 项。
	 */
	foreach(l, clauses)
	{
		Node	   *clause = (Node *) lfirst(l);

		/* 若它是 RestrictInfo，则取出子句 */
		if (IsA(clause, RestrictInfo))
			clause = (Node *) ((RestrictInfo *) clause)->clause;

		/*
		 * 处理各类子句：OpClause、NullTest 以及
		 * AND/OR/NOT
		 */
		if (is_opclause(clause))
		{
			OpExpr	   *expr = (OpExpr *) clause;
			FmgrInfo	opproc;

			/* 仅在 examine_opclause_args 返回真之后有效 */
			Node	   *clause_expr;
			Const	   *cst;
			bool		expronleft;
			int			idx;
			Oid			collid;

			fmgr_info(get_opcode(expr->opno), &opproc);

			/* 从表达式中取出 var/expr 和 const */
			if (!examine_opclause_args(expr->args, &clause_expr, &cst, &expronleft))
				elog(ERROR, "incompatible clause");

			/* 把属性或表达式匹配到统计的某个维度 */
			idx = mcv_match_expression(clause_expr, keys, exprs, &collid);

			/*
			 * 遍历 MCV 项并对当前子句求值。可以跳过
			 * 已经被排除的项，若不再有可能匹配的
			 * 剩余 MCV 项则终止。
			 */
			for (int i = 0; i < mcvlist->nitems; i++)
			{
				bool		match = true;
				MCVItem    *item = &mcvlist->items[i];

				Assert(idx >= 0);

				/*
				 * 当 MCV 项或 Const 值为 NULL 时，可以把它
				 * 当作不匹配。由于严格性，我们不得调用
				 * 该操作符。
				 */
				if (item->isnull[idx] || cst->constisnull)
				{
					matches[i] = RESULT_MERGE(matches[i], is_or, false);
					continue;
				}

				/*
				 * 跳过 bitmap 里结果不会再变的 MCV 项。一旦
				 * AND 列表里该值变成 false，或 OR 列表里变成 true，
				 * 就不需要再看更多子句。
				 */
				if (RESULT_IS_FINAL(matches[i], is_or))
					continue;

				/*
				 * 首先检查常量是否低于下界
				 * （那种情况下可以跳过该桶，因为
				 * 没有重叠）。
				 *
				 * 我们不存储构造统计信息时用的排序规则，但
				 * 可以用属性本身的排序规则，即
				 * 存放在 varcollid 里的那个。类型变更（包括排序规则
				 * 变更）之后我们会重置统计信息，因此这样做没问题。
				 * 对表达式，我们使用从表达式本身取出的
				 * 排序规则。
				 */
				if (expronleft)
					match = DatumGetBool(FunctionCall2Coll(&opproc,
														   collid,
														   item->values[idx],
														   cst->constvalue));
				else
					match = DatumGetBool(FunctionCall2Coll(&opproc,
														   collid,
														   cst->constvalue,
														   item->values[idx]));

				/* 用结果更新匹配 bitmap */
				matches[i] = RESULT_MERGE(matches[i], is_or, match);
			}
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) clause;
			FmgrInfo	opproc;

			/* 仅在 examine_opclause_args 返回真之后有效 */
			Node	   *clause_expr;
			Const	   *cst;
			bool		expronleft;
			Oid			collid;
			int			idx;

			/* 数组求值 */
			ArrayType  *arrayval;
			int16		elmlen;
			bool		elmbyval;
			char		elmalign;
			int			num_elems;
			Datum	   *elem_values;
			bool	   *elem_nulls;

			fmgr_info(get_opcode(expr->opno), &opproc);

			/* 从表达式中取出 var/expr 和 const */
			if (!examine_opclause_args(expr->args, &clause_expr, &cst, &expronleft))
				elog(ERROR, "incompatible clause");

			/* 我们预期左边是 Var */
			if (!expronleft)
				elog(ERROR, "incompatible clause");

			/*
			 * 拆开数组常量，除非它是 NULL（那种情况
			 * 下面会处理）
			 */
			if (!cst->constisnull)
			{
				arrayval = DatumGetArrayTypeP(cst->constvalue);
				get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
									 &elmlen, &elmbyval, &elmalign);
				deconstruct_array(arrayval,
								  ARR_ELEMTYPE(arrayval),
								  elmlen, elmbyval, elmalign,
								  &elem_values, &elem_nulls, &num_elems);
			}

			/* 把属性或表达式匹配到统计的某个维度 */
			idx = mcv_match_expression(clause_expr, keys, exprs, &collid);

			/*
			 * 遍历 MCV 项并对当前子句求值。可以跳过
			 * 已经被排除的项，若不再有可能匹配的
			 * 剩余 MCV 项则终止。
			 */
			for (int i = 0; i < mcvlist->nitems; i++)
			{
				int			j;
				bool		match = !expr->useOr;
				MCVItem    *item = &mcvlist->items[i];

				/*
				 * 当 MCV 项或 Const 值为 NULL 时，可以把它
				 * 当作不匹配。由于严格性，我们不得调用
				 * 该操作符。
				 */
				if (item->isnull[idx] || cst->constisnull)
				{
					matches[i] = RESULT_MERGE(matches[i], is_or, false);
					continue;
				}

				/*
				 * 跳过 bitmap 里结果不会再变的 MCV 项。一旦
				 * AND 列表里该值变成 false，或 OR 列表里变成 true，
				 * 就不需要再看更多子句。
				 */
				if (RESULT_IS_FINAL(matches[i], is_or))
					continue;

				for (j = 0; j < num_elems; j++)
				{
					Datum		elem_value = elem_values[j];
					bool		elem_isnull = elem_nulls[j];
					bool		elem_match;

					/* NULL 值求值结果总是不匹配。 */
					if (elem_isnull)
					{
						match = RESULT_MERGE(match, expr->useOr, false);
						continue;
					}

					/*
					 * 一旦到达一个不会再改变结果的匹配值，就停止
					 * 对数组元素求值：ALL() 与 AND 列表相同，
					 * ANY() 与 OR 列表相同。
					 */
					if (RESULT_IS_FINAL(match, expr->useOr))
						break;

					elem_match = DatumGetBool(FunctionCall2Coll(&opproc,
																collid,
																item->values[idx],
																elem_value));

					match = RESULT_MERGE(match, expr->useOr, elem_match);
				}

				/* 用结果更新匹配 bitmap */
				matches[i] = RESULT_MERGE(matches[i], is_or, match);
			}
		}
		else if (IsA(clause, NullTest))
		{
			NullTest   *expr = (NullTest *) clause;
			Node	   *clause_expr = (Node *) (expr->arg);

			/* 把属性或表达式匹配到统计的某个维度 */
			int			idx = mcv_match_expression(clause_expr, keys, exprs, NULL);

			/*
			 * 遍历 MCV 项并对当前子句求值。可以跳过
			 * 已经被排除的项，若不再有可能匹配的
			 * 剩余 MCV 项则终止。
			 */
			for (int i = 0; i < mcvlist->nitems; i++)
			{
				bool		match = false;	/* 假定不匹配 */
				MCVItem    *item = &mcvlist->items[i];

				/* 若该子句与 MCV 项不匹配，则更新 bitmap */
				switch (expr->nulltesttype)
				{
					case IS_NULL:
						match = (item->isnull[idx]) ? true : match;
						break;

					case IS_NOT_NULL:
						match = (!item->isnull[idx]) ? true : match;
						break;
				}

				/* 现在根据 OR/AND 类型更新匹配 bitmap */
				matches[i] = RESULT_MERGE(matches[i], is_or, match);
			}
		}
		else if (is_orclause(clause) || is_andclause(clause))
		{
			/* AND/OR 子句，所有子子句都兼容 */

			int			i;
			BoolExpr   *bool_clause = ((BoolExpr *) clause);
			List	   *bool_clauses = bool_clause->args;

			/* 每个 MCV 项的匹配/不匹配 bitmap */
			bool	   *bool_matches = NULL;

			Assert(bool_clauses != NIL);
			Assert(list_length(bool_clauses) >= 2);

			/* 为 OR 子句构造匹配 bitmap */
			bool_matches = mcv_get_match_bitmap(root, bool_clauses, keys, exprs,
												mcvlist, is_orclause(clause));

			/*
			 * 把 mcv_get_match_bitmap 产生的 bitmap 合并进
			 * 当前的 bitmap。合并结果时需要考虑正在求值的是
			 * AND 还是 OR 条件。
			 */
			for (i = 0; i < mcvlist->nitems; i++)
				matches[i] = RESULT_MERGE(matches[i], is_or, bool_matches[i]);

			pfree(bool_matches);
		}
		else if (is_notclause(clause))
		{
			/* NOT 子句，所有子子句都兼容 */

			int			i;
			BoolExpr   *not_clause = ((BoolExpr *) clause);
			List	   *not_args = not_clause->args;

			/* 每个 MCV 项的匹配/不匹配 bitmap */
			bool	   *not_matches = NULL;

			Assert(not_args != NIL);
			Assert(list_length(not_args) == 1);

			/* 为 NOT 子句构造匹配 bitmap */
			not_matches = mcv_get_match_bitmap(root, not_args, keys, exprs,
											   mcvlist, false);

			/*
			 * 把 mcv_get_match_bitmap 产生的 bitmap 合并进
			 * 当前的 bitmap。我们在处理 NOT 子句，因此在合并进
			 * 全局 bitmap 之前先把结果取反。
			 */
			for (i = 0; i < mcvlist->nitems; i++)
				matches[i] = RESULT_MERGE(matches[i], is_or, !not_matches[i]);

			pfree(not_matches);
		}
		else if (IsA(clause, Var))
		{
			/* Var（必须是布尔 Var，可能来自下面的 NOT） */

			Var		   *var = (Var *) (clause);

			/* 把属性匹配到统计的某个维度 */
			int			idx = bms_member_index(keys, var->varattno);

			Assert(var->vartype == BOOLOID);

			/*
			 * 遍历 MCV 项并对当前子句求值。可以跳过
			 * 已经被排除的项，若不再有可能匹配的
			 * 剩余 MCV 项则终止。
			 */
			for (int i = 0; i < mcvlist->nitems; i++)
			{
				MCVItem    *item = &mcvlist->items[i];
				bool		match = false;

				/* 若该项为 NULL，就是不匹配 */
				if (!item->isnull[idx] && DatumGetBool(item->values[idx]))
					match = true;

				/* 更新结果 bitmap */
				matches[i] = RESULT_MERGE(matches[i], is_or, match);
			}
		}
		else
		{
			/* 否则，它必须是一个返回布尔值的裸表达式 */
			int			idx;

			/* 把表达式匹配到统计的某个维度 */
			idx = mcv_match_expression(clause, keys, exprs, NULL);

			/*
			 * 遍历 MCV 项并对当前子句求值。可以跳过
			 * 已经被排除的项，若不再有可能匹配的
			 * 剩余 MCV 项则终止。
			 */
			for (int i = 0; i < mcvlist->nitems; i++)
			{
				bool		match;
				MCVItem    *item = &mcvlist->items[i];

				/* "匹配"只表示它是布尔 TRUE */
				match = !item->isnull[idx] && DatumGetBool(item->values[idx]);

				/* 现在根据 OR/AND 类型更新匹配 bitmap */
				matches[i] = RESULT_MERGE(matches[i], is_or, match);
			}
		}
	}

	return matches;
}


/*
 * mcv_combine_selectivities
 * 		组合按列和多列 MCV 选择率估计。
 *
 * simple_sel 是简单选择率估计（不使用任何
 * 扩展统计信息，本质上假定列和子句相互独立）。
 *
 * mcv_sel 和 mcv_basesel 是所有匹配 MCV 项的频率与基频率之和。
 * 差值（mcv_sel - mcv_basesel）随后
 * 本质上被解释为要加到 simple_sel 上的修正，如下面
 * 所述。
 *
 * mcv_totalsel 是所有 MCV 项（不只是
 * 匹配的那些）的频率之和。它被用作 MCV 统计信息未覆盖的
 * 那部分选择率估计的上界。
 *
 * 注意：虽然简单选择率和基选择率的定义方式相当相似，
 * 但数值的计算方式不同，因此并不相等。
 * 简单选择率是按子句估计的乘积，而
 * 基选择率是把多列 MCV 列表中匹配
 * 项的基频率加起来。因此这两个值可能因两个主要
 * 原因而不同：（a）MCV 列表可能没有覆盖 100% 的数据，（b）有些
 * MCV 项没有匹配被估计的子句。
 *
 * 由于 (a) 和 (b) 都会降低基选择率的值，一般有
 * (simple_sel >= mcv_basesel)。若 MCV 列表覆盖全部数据，
 * 这两个值可能相等。
 *
 * 因此，other_sel = (simple_sel - mcv_basesel) 是对 MCV 列表未
 * 覆盖部分的估计，而 (mcv_sel - mcv_basesel) 可以看成对
 * MCV 列表所覆盖部分的修正。这两个说法
 * 实际上是等价的。
 */
Selectivity
mcv_combine_selectivities(Selectivity simple_sel,
						  Selectivity mcv_sel,
						  Selectivity mcv_basesel,
						  Selectivity mcv_totalsel)
{
	Selectivity other_sel;
	Selectivity sel;

	/* MCV 匹配未覆盖的值的估计选择率 */
	other_sel = simple_sel - mcv_basesel;
	CLAMP_PROBABILITY(other_sel);

	/* 这份非 MCV 选择率无法超过 1 - mcv_totalsel */
	if (other_sel > 1.0 - mcv_totalsel)
		other_sel = 1.0 - mcv_totalsel;

	/* 总选择率是 MCV 部分与非 MCV 部分之和 */
	sel = mcv_sel + other_sel;
	CLAMP_PROBABILITY(sel);

	return sel;
}


/*
 * mcv_clauselist_selectivity
 *		用 MCV 统计信息估计隐式 ANDed
 *		子句列表的选择率。
 *
 * 这确定哪些 MCV 项匹配列表里的每个子句，并返回
 * 那些项的频率之和。
 *
 * 此外，它返回那些项各自的
 * 基频率之和（也就是若各列彼此独立，
 * 每个项本会有的选择率之和），以及所有
 * MCV 项（不只是匹配的那些）的总选择率。
 * 这些预期与一个简单选择率估计一起使用
 * （只基于按列统计信息的那个），以产生同时利用
 * 按列和多列统计信息的总选择率估计。见
 * mcv_combine_selectivities()。
 */
Selectivity
mcv_clauselist_selectivity(PlannerInfo *root, StatisticExtInfo *stat,
						   List *clauses, int varRelid,
						   JoinType jointype, SpecialJoinInfo *sjinfo,
						   RelOptInfo *rel,
						   Selectivity *basesel, Selectivity *totalsel)
{
	int			i;
	MCVList    *mcv;
	Selectivity s = 0.0;
	RangeTblEntry *rte = root->simple_rte_array[rel->relid];

	/* 每个 MCV 项的匹配/不匹配 bitmap */
	bool	   *matches = NULL;

	/* 装载存放在统计对象里的 MCV 列表 */
	mcv = statext_mcv_load(stat->statOid, rte->inh);

	/* 为这些子句构造匹配 bitmap */
	matches = mcv_get_match_bitmap(root, clauses, stat->keys, stat->exprs,
								   mcv, false);

	/* 把所有匹配的 MCV 项的频率加总 */
	*basesel = 0.0;
	*totalsel = 0.0;
	for (i = 0; i < mcv->nitems; i++)
	{
		*totalsel += mcv->items[i].frequency;

		if (matches[i] != false)
		{
			*basesel += mcv->items[i].base_frequency;
			s += mcv->items[i].frequency;
		}
	}

	return s;
}


/*
 * mcv_clause_selectivity_or
 *		用 MCV 统计信息估计出现在 ORed
 *		子句列表里的一条子句的选择率。
 *
 * 与 mcv_clauselist_selectivity() 一样，这确定哪些 MCV 项匹配
 * 该子句，并返回那些项的频率之和与
 * 基频率之和，以及所有 MCV 项（不只是匹配的那些）的
 * 频率之和，以便 mcv_combine_selectivities() 能用这些信息
 * 产生同时利用按列和多列统计信息的
 * 选择率估计。
 *
 * 此外，我们返回有助于计算假定包含该子句的 ORed 子句列表
 * 总选择率的信息。本函数
 * 打算对 ORed 子句列表里的每个子句调用，
 * 从而可以用下面的
 * 算法计算总选择率：
 *
 * 设 P[n] = P(C[1] OR C[2] OR ... OR C[n]) 是列表中前 n 条
 * 子句的组合选择率。那么计入下一条子句 C[n+1] 之后的
 * 组合选择率可以写成：
 *
 *		P[n+1] = P[n] + P(C[n+1]) - P((C[1] OR ... OR C[n]) AND C[n+1])。
 *
 * 上面最后一项表示迄今检查过的子句与
 * 第 (n+1) 条子句之间的重叠。为了估计它的选择率，我们跟踪
 * 迄今检查过的 ORed 子句列表的匹配 bitmap，并检查它与
 * 第 (n+1) 条子句的匹配 bitmap 的交集。
 *
 * 然后我们还返回对应于上面那个重叠项的匹配 bitmap 交集的
 * MCV 项频率之和与基
 * 频率之和，以便它们能与该重叠项的简单选择率估计
 * 组合起来。
 *
 * 参数 or_matches 是跟踪迄今检查过的子句的
 * 匹配 bitmap 的输入/输出参数。调用者预期在
 * 第一次调用本函数时把它设为 NULL。
 */
Selectivity
mcv_clause_selectivity_or(PlannerInfo *root, StatisticExtInfo *stat,
						  MCVList *mcv, Node *clause, bool **or_matches,
						  Selectivity *basesel, Selectivity *overlap_mcvsel,
						  Selectivity *overlap_basesel, Selectivity *totalsel)
{
	Selectivity s = 0.0;
	bool	   *new_matches;
	int			i;

	/* 若尚未构造，则构造 OR 匹配 bitmap */
	if (*or_matches == NULL)
		*or_matches = palloc0(sizeof(bool) * mcv->nitems);

	/* 为新子句构造匹配 bitmap */
	new_matches = mcv_get_match_bitmap(root, list_make1(clause), stat->keys,
									   stat->exprs, mcv, false);

	/*
	 * 把匹配本子句的所有 MCV 项的频率加总，也把
	 * 匹配本子句与前面任一子句之间重叠的那些项
	 * 按上面所述加总。
	 */
	*basesel = 0.0;
	*overlap_mcvsel = 0.0;
	*overlap_basesel = 0.0;
	*totalsel = 0.0;
	for (i = 0; i < mcv->nitems; i++)
	{
		*totalsel += mcv->items[i].frequency;

		if (new_matches[i])
		{
			s += mcv->items[i].frequency;
			*basesel += mcv->items[i].base_frequency;

			if ((*or_matches)[i])
			{
				*overlap_mcvsel += mcv->items[i].frequency;
				*overlap_basesel += mcv->items[i].base_frequency;
			}
		}

		/* 为下一条子句更新 OR 匹配 bitmap */
		(*or_matches)[i] = (*or_matches)[i] || new_matches[i];
	}

	pfree(new_matches);

	return s;
}
