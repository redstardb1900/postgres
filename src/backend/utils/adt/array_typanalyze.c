/*-------------------------------------------------------------------------
 *
 * array_typanalyze.c
 *	  从数组列收集统计信息的函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/array_typanalyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "commands/vacuum.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"


/*
 * 为避免在分析期间消耗过多内存、IO 与 CPU 负载，
 * 以及在生成的 pg_statistic 行里占过多空间，我们忽略
 * 宽度超过 ARRAY_WIDTH_THRESHOLD 的数组（去 toast 后！）。
 * 该数值远大于 analyze.c 标准统计收集代码中
 * 类似的 WIDTH_THRESHOLD 限制。
 */
#define ARRAY_WIDTH_THRESHOLD 0x10000

/* compute_array_stats 函数的额外数据 */
typedef struct
{
	/* 数组元素类型的相关信息 */
	Oid			type_id;		/* 元素类型 OID */
	Oid			eq_opr;			/* 默认相等算子的 OID */
	Oid			coll_id;		/* 排序规则 */
	bool		typbyval;		/* 元素类型的物理属性 */
	int16		typlen;
	char		typalign;

	/*
	 * 元素类型比较与哈希函数的查找数据（它们
	 * 位于该类型的 typcache 条目中，我们预期它们在
	 * ANALYZE 运行期有效）
	 */
	FmgrInfo   *cmp;
	FmgrInfo   *hash;

	/* std_typanalyze() 保存的状态 */
	AnalyzeAttrComputeStatsFunc std_compute_stats;
	void	   *std_extra_data;
} ArrayAnalyzeExtraData;

/*
 * 在 compute_array_stats 运行期间，我们把额外数据的
 * 指针存在此处，供各子例程使用。compute_array_stats
 * 目前无需支持重入，为避免它而付出
 * 额外记号累赘并不值得。
 */
static ArrayAnalyzeExtraData *array_extra_data;

/* 有损计数算法的哈希表条目 */
typedef struct
{
	Datum		key;			/* 这是 LC 算法中的 'e'。 */
	int			frequency;		/* 这是 'f' */
	int			delta;			/* 这是 'delta'。 */
	int			last_container; /* 用于数组元素去重。 */
} TrackItem;

/* 唯一元素计数的哈希表条目 */
typedef struct
{
	int			count;			/* 数组中唯一元素的计数 */
	int			frequency;		/* 具有该计数的数组数量 */
} DECountItem;

static void compute_array_stats(VacAttrStats *stats,
								AnalyzeAttrFetchFunc fetchfunc, int samplerows, double totalrows);
static void prune_element_hashtable(HTAB *elements_tab, int b_current);
static uint32 element_hash(const void *key, Size keysize);
static int	element_match(const void *key1, const void *key2, Size keysize);
static int	element_compare(const void *key1, const void *key2);
static int	trackitem_compare_frequencies_desc(const void *e1, const void *e2, void *arg);
static int	trackitem_compare_element(const void *e1, const void *e2, void *arg);
static int	countitem_compare_count(const void *e1, const void *e2, void *arg);


/*
 * array_typanalyze -- 数组列的统计收集函数
 */
Datum
array_typanalyze(PG_FUNCTION_ARGS)
{
	VacAttrStats *stats = (VacAttrStats *) PG_GETARG_POINTER(0);
	Oid			element_typeid;
	TypeCacheEntry *typentry;
	ArrayAnalyzeExtraData *extra_data;

	/*
	 * 调用标准统计收集函数。它可能找不到所需的
	 * 算子，那时我们也无能为力，因此直接失败。
	 */
	if (!std_typanalyze(stats))
		PG_RETURN_BOOL(false);

	/*
	 * 检查属性数据类型是否为 varlena 数组（或其域）。
	 */
	element_typeid = get_base_element_type(stats->attrtypid);
	if (!OidIsValid(element_typeid))
		elog(ERROR, "array_typanalyze was invoked for non-array type %u",
			 stats->attrtypid);

	/*
	 * 收集元素类型的相关信息。若找不到
	 * 所需内容，则保留 std_typanalyze() 的状态直接返回。
	 */
	typentry = lookup_type_cache(element_typeid,
								 TYPECACHE_EQ_OPR |
								 TYPECACHE_CMP_PROC_FINFO |
								 TYPECACHE_HASH_PROC_FINFO);

	if (!OidIsValid(typentry->eq_opr) ||
		!OidIsValid(typentry->cmp_proc_finfo.fn_oid) ||
		!OidIsValid(typentry->hash_proc_finfo.fn_oid))
		PG_RETURN_BOOL(true);

	/* 保存所得信息供 compute_array_stats() 使用 */
	extra_data = (ArrayAnalyzeExtraData *) palloc(sizeof(ArrayAnalyzeExtraData));
	extra_data->type_id = typentry->type_id;
	extra_data->eq_opr = typentry->eq_opr;
	extra_data->coll_id = stats->attrcollid;	/* 要用的排序规则 */
	extra_data->typbyval = typentry->typbyval;
	extra_data->typlen = typentry->typlen;
	extra_data->typalign = typentry->typalign;
	extra_data->cmp = &typentry->cmp_proc_finfo;
	extra_data->hash = &typentry->hash_proc_finfo;

	/* 保存标量统计原有的 compute_stats 与 extra_data ... */
	extra_data->std_compute_stats = stats->compute_stats;
	extra_data->std_extra_data = stats->extra_data;

	/* ... 并换上我们的信息 */
	stats->compute_stats = compute_array_stats;
	stats->extra_data = extra_data;

	/*
	 * 注意我们让 stats->minrows 保持 std_typanalyze 所设
	 * 的值。数组分析要增大吗？
	 */

	PG_RETURN_BOOL(true);
}

/*
 * compute_array_stats() -- 计算数组列的统计信息
 *
 * 本函数计算用于判定数组算子 <@、&&、@> 选择率的
 * 统计信息。它由 ANALYZE 在采样行收集完之后通过
 * compute_stats 钩子调用。
 *
 * 我们还会调用标准 compute_stats 函数，它将计算与
 * btree 风格数组比较算子相关的"标量"统计。
 * 然而，即便许多数组共享个别元素，整个数组
 * 的完全重复仍可能罕见。这一点对长数组影响尤甚，
 * 它们还可能因 analyze.c 中过低的
 * WIDTH_THRESHOLD 而缺标量统计。所以除标准统计外，
 * 我们还会找出最常见数组元素，并计算唯一元素计数
 * 直方图。
 *
 * 所用算法是有损计数（Lossy Counting），出自论文"Approximate
 *  frequency counts over data streams"，作者 G. S. Manku 和
 * R. Motwani，发表于第 28 届超大型数据库国际会议
 * （中国香港，2002 年 8 月）4.2 节。论文见
 * http://www.vldb.org/conf/2002/S10P03.pdf
 *
 * 有损计数（即 LC）算法的过程如下：
 * 设 s 为某项的阈值频率（我们感兴趣的
 * 最小频率），epsilon 为频率的误差容限。设 D
 * 为三元组 (e, f, delta) 的集合，其中 e 是元素值，f 是该
 * 元素的频率（实际上是当前出现次数），delta 是
 * f 的最大误差。从空的 D 开始，按大小为 w 的批次
 * 处理元素。（批大小也称为"桶大小"，等于
 * 1/epsilon。）设当前批号为 b_current，从 1 开始。
 * 对每个元素 e，若它已在 D 中则递增其 f 计数，
 * 否则向 D 插入值为 (e, 1, b_current
 *  - 1) 的新三元组。每处理完一批后我们就修剪 D，
 * 从中移除所有满足 f + delta <= b_current 的元素。
 * 算法结束后，我们从 D 中去掉所有不满足
 * f >= (s - epsilon) * N 的元素，其中 N 是输入中
 * 元素总数。我们以估计频率 f/N 输出剩余元素。
 * LC 论文证明该算法能找出真实频率至少为 s 的
 * 全部元素，且任何频率都不会被高估，也不会被
 * 低估超过 epsilon。在输入分布的合理假设下，
 * 所需表大小至多约为 w 的 7 倍。
 *
 * 由于缺乏选取其他特定值的原则性依据，我们
 * 沿用 ts_typanalyze()，采用参数 s = 0.07/K、
 * epsilon = s/10。但省去了停用词修正，停用词并不
 * 适用于数组。这些参数给出桶宽 w = K/0.007 与
 * 最大预期哈希表大小约 1000 * K。
 *
 * 元素可能在数组内重复。由于重复项不会改变
 * <@、&& 或 @> 的行为，我们希望每个元素在每个
 * 数组中只计一次。因此在最终 pg_statistic 条目
 * 中把每个元素的频率存为包含它的全部非空行的
 * 比例。原始计数除以 nonnull_cnt 即得。
 */
static void
compute_array_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
					int samplerows, double totalrows)
{
	ArrayAnalyzeExtraData *extra_data;
	int			num_mcelem;
	int			null_elem_cnt = 0;
	int			analyzed_rows = 0;

	/* 这是 LC 算法中的 D。 */
	HTAB	   *elements_tab;
	HASHCTL		elem_hash_ctl;
	HASH_SEQ_STATUS scan_status;

	/* 这是 LC 算法中的当前桶编号 */
	int			b_current;

	/* 这是 LC 算法中的 'w' */
	int			bucket_width;
	int			array_no;
	int64		element_no;
	TrackItem  *item;
	int			slot_idx;
	HTAB	   *count_tab;
	HASHCTL		count_hash_ctl;
	DECountItem *count_item;

	extra_data = (ArrayAnalyzeExtraData *) stats->extra_data;

	/*
	 * 调用 analyze.c 的标准分析函数来创建标量型
	 * 列统计。它会期望自己的 extra_data 指针，因此
	 * 需暂时安装它。
	 */
	stats->extra_data = extra_data->std_extra_data;
	extra_data->std_compute_stats(stats, fetchfunc, samplerows, totalrows);
	stats->extra_data = extra_data;

	/*
	 * 为子例程设置静态指针。我们等到这里才设置，
	 * 以防 std_compute_stats 以某种方式递归调用我们（
	 * 多半不可能）
	 */
	array_extra_data = extra_data;

	/*
	 * 我们想要 MCELEM 数组中有 statistics_target * 10 个
	 * 元素。该倍数相当随意，但意在反映：
	 * pg_statistic 中跟踪的个别元素数量应多于
	 * 简单标量列的值数量。
	 */
	num_mcelem = stats->attr->attstattarget * 10;

	/*
	 * 桶宽设为 num_mcelem / 0.007，此即上文注释
	 * 所述
	 */
	bucket_width = num_mcelem * 1000 / 7;

	/*
	 * 创建哈希表。它位于本地内存中，因此无需担心
	 * 初始大小溢出问题。也无需再费心
	 * 加锁与内存管理。
	 */
	elem_hash_ctl.keysize = sizeof(Datum);
	elem_hash_ctl.entrysize = sizeof(TrackItem);
	elem_hash_ctl.hash = element_hash;
	elem_hash_ctl.match = element_match;
	elem_hash_ctl.hcxt = CurrentMemoryContext;
	elements_tab = hash_create("Analyzed elements table",
							   num_mcelem,
							   &elem_hash_ctl,
							   HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

	/* 数组唯一元素计数的哈希表 */
	count_hash_ctl.keysize = sizeof(int);
	count_hash_ctl.entrysize = sizeof(DECountItem);
	count_hash_ctl.hcxt = CurrentMemoryContext;
	count_tab = hash_create("Array distinct element count table",
							64,
							&count_hash_ctl,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* 初始化计数器 */
	b_current = 1;
	element_no = 0;

	/* 遍历各个数组。 */
	for (array_no = 0; array_no < samplerows; array_no++)
	{
		Datum		value;
		bool		isnull;
		ArrayType  *array;
		int			num_elems;
		Datum	   *elem_values;
		bool	   *elem_nulls;
		bool		null_present;
		int			j;
		int64		prev_element_no = element_no;
		int			distinct_count;
		bool		count_item_found;

		vacuum_delay_point();

		value = fetchfunc(stats, array_no, &isnull);
		if (isnull)
		{
			/* 忽略整行为 null 的数组 */
			continue;
		}

		/* 跳过过大的值。 */
		if (toast_raw_datum_size(value) > ARRAY_WIDTH_THRESHOLD)
			continue;
		else
			analyzed_rows++;

		/*
		 * 现在按需对数组去 toast，并解构为 datum。
		 */
		array = DatumGetArrayTypeP(value);

		Assert(ARR_ELEMTYPE(array) == extra_data->type_id);
		deconstruct_array(array,
						  extra_data->type_id,
						  extra_data->typlen,
						  extra_data->typbyval,
						  extra_data->typalign,
						  &elem_values, &elem_nulls, &num_elems);

		/*
		 * 我们遍历数组中的元素并把它们加入我们的
		 * 跟踪哈希表。
		 */
		null_present = false;
		for (j = 0; j < num_elems; j++)
		{
			Datum		elem_value;
			bool		found;

			/* 这里除设置标志外不对 null 元素做处理 */
			if (elem_nulls[j])
			{
				null_present = true;
				continue;
			}

			/* 哈希表中查找当前元素，若为新则加入 */
			elem_value = elem_values[j];
			item = (TrackItem *) hash_search(elements_tab,
											 &elem_value,
											 HASH_ENTER, &found);

			if (found)
			{
				/* 该元素值已在跟踪列表上 */

				/*
				 * 我们辅助的算子忽略重复数组元素，因此
				 * 给定唯一元素在每个数组中只计一次。
				 */
				if (item->last_container == array_no)
					continue;

				item->frequency++;
				item->last_container = array_no;
			}
			else
			{
				/* 初始化新的跟踪列表元素 */

				/*
				 * 若元素类型为按引用传递，必须把它复制到
				 * palloc 的内存中，以便随后释放数组。（我们
				 * 这样做是为了让元素值所需空间受
				 * 哈希表大小的限制；若把所有数组值
				 * 都保留，空间可能大得多。）
				 */
				item->key = datumCopy(elem_value,
									  extra_data->typbyval,
									  extra_data->typlen);

				item->frequency = 1;
				item->delta = b_current - 1;
				item->last_container = array_no;
			}

			/* element_no 是已处理元素数（即 N） */
			element_no++;

			/* 每处理完一个桶后就修剪 D 结构 */
			if (element_no % bucket_width == 0)
			{
				prune_element_hashtable(elements_tab, b_current);
				b_current++;
			}
		}

		/* 每数组只计一次 null 元素存在。 */
		if (null_present)
			null_elem_cnt++;

		/* 更新该数组唯一元素计数的频率。 */
		distinct_count = (int) (element_no - prev_element_no);
		count_item = (DECountItem *) hash_search(count_tab, &distinct_count,
												 HASH_ENTER,
												 &count_item_found);

		if (count_item_found)
			count_item->frequency++;
		else
			count_item->frequency = 1;

		/* 释放去 toast 时分配的内存。 */
		if (PointerGetDatum(array) != value)
			pfree(array);
		pfree(elem_values);
		pfree(elem_nulls);
	}

	/* 跳过被标准统计占用的 pg_statistic 槽位 */
	slot_idx = 0;
	while (slot_idx < STATISTIC_NUM_SLOTS && stats->stakind[slot_idx] != 0)
		slot_idx++;
	if (slot_idx > STATISTIC_NUM_SLOTS - 2)
		elog(ERROR, "insufficient pg_statistic slots for array stats");

	/* 只有找到一些非空值才能计算真实统计。 */
	if (analyzed_rows > 0)
	{
		int			nonnull_cnt = analyzed_rows;
		int			count_items_count;
		int			i;
		TrackItem **sort_table;
		int			track_len;
		int64		cutoff_freq;
		int64		minfreq,
					maxfreq;

		/*
		 * 我们假定标准统计代码已设置好
		 * stats_valid、stanullfrac、stawidth、stadistinct。
		 * 若不想存储标准统计，就得重新计算
		 * 其值
		 */

		/*
		 * 由值得关注的哈希表条目构建一个数组，也就
		 * 是满足截止频率 (s - epsilon)*N 的那些。同时找出
		 * 这些条目中的最小与最大频率。
		 *
		 * 由于 epsilon = s/10 且 bucket_width = 1/epsilon，
		 * 截止频率为 9*N/bucket_width
		 */
		cutoff_freq = 9 * element_no / bucket_width;

		i = hash_get_num_entries(elements_tab); /* 空间必定够用 */
		sort_table = (TrackItem **) palloc(sizeof(TrackItem *) * i);

		hash_seq_init(&scan_status, elements_tab);
		track_len = 0;
		minfreq = element_no;
		maxfreq = 0;
		while ((item = (TrackItem *) hash_seq_search(&scan_status)) != NULL)
		{
			if (item->frequency > cutoff_freq)
			{
				sort_table[track_len++] = item;
				minfreq = Min(minfreq, item->frequency);
				maxfreq = Max(maxfreq, item->frequency);
			}
		}
		Assert(track_len <= i);

		/* 输出一些用于调试的统计 */
		elog(DEBUG3, "compute_array_stats: target # mces = %d, "
			 "bucket width = %d, "
			 "# elements = " INT64_FORMAT ", hashtable size = %d, "
			 "usable entries = %d",
			 num_mcelem, bucket_width, element_no, i, track_len);

		/*
		 * 若获得的元素多于真正想要的，就剔除那些
		 * 频率最低的元素。最简单的办法是把数组按
		 * 频率降序 qsort 排序然后截断数组。
		 */
		if (num_mcelem < track_len)
		{
			qsort_interruptible(sort_table, track_len, sizeof(TrackItem *),
								trackitem_compare_frequencies_desc, NULL);
			/* 把 minfreq 重置为我们保留的最小频率 */
			minfreq = sort_table[num_mcelem - 1]->frequency;
		}
		else
			num_mcelem = track_len;

		/* 生成 MCELEM 槽位条目 */
		if (num_mcelem > 0)
		{
			MemoryContext old_context;
			Datum	   *mcelem_values;
			float4	   *mcelem_freqs;

			/*
			 * 我们想存储按元素值排序的统计，排序使用
			 * 元素类型的默认比较函数。这样可以
			 * 在选择率估算函数中进行快速二分查找。
			 */
			qsort_interruptible(sort_table, num_mcelem, sizeof(TrackItem *),
								trackitem_compare_element, NULL);

			/* 必须把目标值复制到 anl_context */
			old_context = MemoryContextSwitchTo(stats->anl_context);

			/*
			 * 统计已按元素值排序，但我们希望不必遍历
			 * 全部值就能找出最小与最大频率。我们还
			 * 想要 null 元素的频率。把这三个值存到
			 * mcelem_freqs 的末尾。
			 */
			mcelem_values = (Datum *) palloc(num_mcelem * sizeof(Datum));
			mcelem_freqs = (float4 *) palloc((num_mcelem + 3) * sizeof(float4));

			/*
			 * 关于用 nonnull_cnt 作最终频率估算的除数，
			 * 参见上面的注释。
			 */
			for (i = 0; i < num_mcelem; i++)
			{
				TrackItem  *titem = sort_table[i];

				mcelem_values[i] = datumCopy(titem->key,
											 extra_data->typbyval,
											 extra_data->typlen);
				mcelem_freqs[i] = (double) titem->frequency /
					(double) nonnull_cnt;
			}
			mcelem_freqs[i++] = (double) minfreq / (double) nonnull_cnt;
			mcelem_freqs[i++] = (double) maxfreq / (double) nonnull_cnt;
			mcelem_freqs[i++] = (double) null_elem_cnt / (double) nonnull_cnt;

			MemoryContextSwitchTo(old_context);

			stats->stakind[slot_idx] = STATISTIC_KIND_MCELEM;
			stats->staop[slot_idx] = extra_data->eq_opr;
			stats->stacoll[slot_idx] = extra_data->coll_id;
			stats->stanumbers[slot_idx] = mcelem_freqs;
			/* 关于额外 stanumber 条目见上文注释 */
			stats->numnumbers[slot_idx] = num_mcelem + 3;
			stats->stavalues[slot_idx] = mcelem_values;
			stats->numvalues[slot_idx] = num_mcelem;
			/* 我们存储的是元素类型的值 */
			stats->statypid[slot_idx] = extra_data->type_id;
			stats->statyplen[slot_idx] = extra_data->typlen;
			stats->statypbyval[slot_idx] = extra_data->typbyval;
			stats->statypalign[slot_idx] = extra_data->typalign;
			slot_idx++;
		}

		/* 生成 DECHIST 槽位条目 */
		count_items_count = hash_get_num_entries(count_tab);
		if (count_items_count > 0)
		{
			int			num_hist = stats->attr->attstattarget;
			DECountItem **sorted_count_items;
			int			j;
			int			delta;
			int64		frac;
			float4	   *hist;

			/* 下面的循环要工作 num_hist 至少必须为 2 */
			num_hist = Max(num_hist, 2);

			/*
			 * 创建一个 DECountItem 指针数组，并把它们排成
			 * 计数递增序。
			 */
			sorted_count_items = (DECountItem **)
				palloc(sizeof(DECountItem *) * count_items_count);
			hash_seq_init(&scan_status, count_tab);
			j = 0;
			while ((count_item = (DECountItem *) hash_seq_search(&scan_status)) != NULL)
			{
				sorted_count_items[j++] = count_item;
			}
			qsort_interruptible(sorted_count_items, count_items_count,
								sizeof(DECountItem *),
								countitem_compare_count, NULL);

			/*
			 * 准备用直方图填充 stanumbers，其后跟
			 * 平均计数。此数组必须存于 anl_context。
			 */
			hist = (float4 *)
				MemoryContextAlloc(stats->anl_context,
								   sizeof(float4) * (num_hist + 1));
			hist[num_hist] = (double) element_no / (double) nonnull_cnt;

			/*----------
			 * 构建唯一元素计数（DEC）的直方图。
			 *
			 * 本循环的目的是把最小与最大的 DEC 复制到
			 * hist[0] 与 hist[num_hist - 1]，以及介于其间的
			 * 等距 DEC（其中"等距"是相对于
			 * 全体输入数组总体）。若我们有一份完整
			 * 已排序 DEC 数组（每分析行一个），第 i 个 hist
			 * 值取自 DECs[i * (analyzed_rows - 1) / (num_hist - 1)]
			 * （参照 compute_scalar_stats() 建直方图循环）。
			 * 但我们所有的是 sorted_count_items[] 数组，
			 * 它保存唯一 DEC 值及其频率（也就是
			 * 完整数组的行程压缩版）。于是我们
			 * 用变量 "frac" 来控制在 sorted_count_items[]
			 * 中的前进，该变量定义为 (x - y) * (num_hist - 1)，
			 * 其中 x 是名义 DEC 数组中对应
			 * 下一个 sorted_count_items[] 元素行程开始的
			 * 索引，而 y 是 DEC 中应取下一个直方图值的
			 * 索引。只要 x <= y（即 frac <= 0）就必须前进。
			 * x 分量是到目前为止看到的频率之和
			 * （算到当前 sorted_count_items[] 元素为止），
			 * 当然 y * (num_hist - 1) = i * (analyzed_rows - 1)，
			 * 与上面的下标计算一致。（该下标计算
			 * 意味着舍弃 y 的小数部分；在此表述中
			 * 这体现为 frac 达到 1 前不前进。）
			 *
			 * 尽管 frac 取值范围有限，在非常大的统计目标
			 * 下仍可能溢出 int32，因此我们在 int64 中
			 * 做该计算
			 *----------
			 */
			delta = analyzed_rows - 1;
			j = 0;				/* sorted_count_items 当前下标 */
			/* 为 sorted_count_items[0] 初始化 frac；y 初始为 0 */
			frac = (int64) sorted_count_items[0]->frequency * (num_hist - 1);
			for (i = 0; i < num_hist; i++)
			{
				while (frac <= 0)
				{
					/* 前进，并更新 frac 的 x 分量 */
					j++;
					frac += (int64) sorted_count_items[j]->frequency * (num_hist - 1);
				}
				hist[i] = sorted_count_items[j]->count;
				frac -= delta;	/* 为接下来的 i 递增更新 y */
			}
			Assert(j == count_items_count - 1);

			stats->stakind[slot_idx] = STATISTIC_KIND_DECHIST;
			stats->staop[slot_idx] = extra_data->eq_opr;
			stats->stacoll[slot_idx] = extra_data->coll_id;
			stats->stanumbers[slot_idx] = hist;
			stats->numnumbers[slot_idx] = num_hist + 1;
			slot_idx++;
		}
	}

	/*
	 * 我们不必费心清理任何临时 palloc 的内存。哈希表
	 * 也应随之消失，因为它使用的是子内存上下文。
	 */
}

/*
 * 一个用于修剪有损计数算法 D 结构的函数。
 * 更详细的说明见 compute_tsvector_stats()。
 */
static void
prune_element_hashtable(HTAB *elements_tab, int b_current)
{
	HASH_SEQ_STATUS scan_status;
	TrackItem  *item;

	hash_seq_init(&scan_status, elements_tab);
	while ((item = (TrackItem *) hash_seq_search(&scan_status)) != NULL)
	{
		if (item->frequency + item->delta <= b_current)
		{
			Datum		value = item->key;

			if (hash_search(elements_tab, &item->key,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "hash table corrupted");
			/* 若元素非按值传递则应释放内存 */
			if (!array_extra_data->typbyval)
				pfree(DatumGetPointer(value));
		}
	}
}

/*
 * 元素的哈希函数。
 *
 * 我们使用元素类型的默认哈希算子类，以及列排序
 * 规则（若该类型敏感）。
 */
static uint32
element_hash(const void *key, Size keysize)
{
	Datum		d = *((const Datum *) key);
	Datum		h;

	h = FunctionCall1Coll(array_extra_data->hash,
						  array_extra_data->coll_id,
						  d);
	return DatumGetUInt32(h);
}

/*
 * 元素的匹配函数，用于哈希表查找。
 */
static int
element_match(const void *key1, const void *key2, Size keysize)
{
	/* keysize 参数在此是多余的 */
	return element_compare(key1, key2);
}

/*
 * 元素的比较函数。
 *
 * 我们使用元素类型的默认 btree 算子类，以及列排
 * 规则（若该类型敏感）。
 *
 * XXX 考虑使用 SortSupport 基础设施
 */
static int
element_compare(const void *key1, const void *key2)
{
	Datum		d1 = *((const Datum *) key1);
	Datum		d2 = *((const Datum *) key2);
	Datum		c;

	c = FunctionCall2Coll(array_extra_data->cmp,
						  array_extra_data->coll_id,
						  d1, d2);
	return DatumGetInt32(c);
}

/*
 * 按频率排序 TrackItems 的比较器（降序排序）
 */
static int
trackitem_compare_frequencies_desc(const void *e1, const void *e2, void *arg)
{
	const TrackItem *const *t1 = (const TrackItem *const *) e1;
	const TrackItem *const *t2 = (const TrackItem *const *) e2;

	return (*t2)->frequency - (*t1)->frequency;
}

/*
 * 按元素值排序 TrackItems 的比较器
 */
static int
trackitem_compare_element(const void *e1, const void *e2, void *arg)
{
	const TrackItem *const *t1 = (const TrackItem *const *) e1;
	const TrackItem *const *t2 = (const TrackItem *const *) e2;

	return element_compare(&(*t1)->key, &(*t2)->key);
}

/*
 * 按计数排序 DECountItems 的比较器
 */
static int
countitem_compare_count(const void *e1, const void *e2, void *arg)
{
	const DECountItem *const *t1 = (const DECountItem *const *) e1;
	const DECountItem *const *t2 = (const DECountItem *const *) e2;

	if ((*t1)->count < (*t2)->count)
		return -1;
	else if ((*t1)->count == (*t2)->count)
		return 0;
	else
		return 1;
}
