/*
 * brin_minmax_multi.c
 *		BRIN 的 Multi Min/Max 操作符类实现
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * 实现 minmax 操作符类的一种变体，其汇总由
 * 多个较小的区间组成。这样就能处理离群值；离群值
 * 通常会让简单的 minmax 操作符类效率低下。
 *
 * 例如考虑一个页范围，简单 minmax 区间为 [1000,2000]，
 * 并假设向该范围插入一行，值为 1000000。
 * 于是区间变成 [1000,1000000]。也就是说 minmax 区间
 * 变宽了 1000 倍，对排除介于 2001
 * 与 1000000 之间的扫描键不再有用。
 *
 * 使用 minmax-multi 操作符类时，起初可能只有 [1000,2000] 这一区间，
 * 但加入新行之后，我们开始把它跟踪为两个区间：
 *
 *   [1000,2000] 和 [1000000,1000000]
 *
 * 这样，当扫描键落在 2000 与 1000000 之间的间隙时，
 * 我们仍能排除该页范围，因而在简单 minmax
 * 操作符类变得低效的情形下仍然有用。
 *
 * 每个页范围跟踪的区间个数有一定弹性。
 * 受到限制的是每个页范围的值个数，目前的上限
 * 是 32（见 values_per_range 关系选项）。折叠区间
 * （最小值与最大值相等）存成单个值，
 * 而普通区间需要两个值。
 *
 * 当值的个数变得过高（因为向
 * 汇总加入了新值）时，我们合并一些区间，以便为更多值腾出空间。
 * 这是贪心做法：直接选出两个最接近的区间，
 * 合并它们，并重复，直到要存储的值个数
 * 足够低（低于最大值的 50%），不过这大多是
 * 任意阈值，可以轻易改掉）。
 *
 * 为选出最接近的区间，我们使用 "distance" 支持过程，
 * 它度量两个区间之间的空间（即一个区间的长度）。
 * 算出的值可以是近似值：最坏情况下我们会
 * 在该步合并两个略逊于最优的区间，但
 * 索引仍应产生正确结果。
 *
 * 压实（减少值的个数）相当昂贵，因为
 * 它需要调用距离函数、排序等等。因此在构建
 * 汇总时，我们使用大得多的缓冲区，只在最后
 * 才强制精确上限。这能提高性能，也有助于
 * 构建更好的区间（由于贪心方法）。
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_minmax_multi.c
 */
#include "postgres.h"

/* PGSQL_AF_INET 需要此头文件 */
#include <sys/socket.h>

#include "access/genam.h"
#include "access/brin.h"
#include "access/brin_internal.h"
#include "access/brin_tuple.h"
#include "access/reloptions.h"
#include "access/stratnum.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/float.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

/*
 * 额外的 SQL 层支持函数
 *
 * 过程号不得使用 BRIN 自身保留的值；见
 * brin_internal.h。
 */
#define		MINMAX_MAX_PROCNUMS		1	/* 我们需要的支持过程个数上限 */
#define		PROCNUM_DISTANCE		11	/* 必需，值之间的距离 */

/*
 * 从 procnum 中减去它，得到 MinmaxMultiOpaque 数组中的下标
 * （必须等于私有过程号的最小值）。
 */
#define		PROCNUM_BASE			11

/*
 * 确定插入缓冲区大小：使用关系选项所指定值个数的 10 倍，
 * 但封顶为 8192，以免过大。当
 * 缓冲区装满时，把值的个数减半。
 */
#define		MINMAX_BUFFER_FACTOR			10
#define		MINMAX_BUFFER_MIN				256
#define		MINMAX_BUFFER_MAX				8192
#define		MINMAX_BUFFER_LOAD_FACTOR		0.5

typedef struct MinmaxMultiOpaque
{
	FmgrInfo	extra_procinfos[MINMAX_MAX_PROCNUMS];
	Oid			cached_subtype;
	FmgrInfo	strategy_procinfos[BTMaxStrategyNumber];
} MinmaxMultiOpaque;

/*
 * BRIN minmax 关系选项的存储类型
 */
typedef struct MinMaxMultiOptions
{
	int32		vl_len_;		/* varlena 头部（不要直接改动！） */
	int			valuesPerRange; /* 每个页范围的值个数 */
} MinMaxMultiOptions;

#define MINMAX_MULTI_DEFAULT_VALUES_PER_PAGE		32

#define MinMaxMultiGetValuesPerRange(opts) \
		((opts) && (((MinMaxMultiOptions *) (opts))->valuesPerRange != 0) ? \
		 ((MinMaxMultiOptions *) (opts))->valuesPerRange : \
		 MINMAX_MULTI_DEFAULT_VALUES_PER_PAGE)

#define SAMESIGN(a,b) (((a) < 0) == ((b) < 0))

/*
 * minmax-multi 索引的汇总有两种表示：便于处理的 Ranges，
 * 以及存入 bytea 值的 SerializedRanges。
 *
 * Ranges 结构把边界值存在同一个数组里，但为了省空间，
 * 普通区间和单点区间的处理不同。对于
 * 普通区间（两端边界值不同），必须同时存储
 * 区间的下界和上界；而对于 "单点区间"
 * 只需要存一个值。
 *
 * values 数组先存普通区间的边界值（要存
 * 2*nranges 个值），然后再存单点区间的 nvalues 个边界值。
 * 也就是说，数组里共有 (2*nranges + nvalues) 个
 * 边界值。
 *
 * +-------------------------+----------------------------------+
 * | 区间（2 * nranges 个） | 单点值（nvalues 个） |
 * +-------------------------+----------------------------------+
 *
 * 这样就能快速加入新值，并存下离群值，而不必
 * 放宽任何已有区间的值。
 *
 * nsorted 表示 values[] 数组里的 nvalues 中有多少已经排好序。
 * 当 nsorted == nvalues 时，全部单点值都已排序。
 *
 * 存储的值绝不超过 maxvalues 个（由 values_per_range
 * 关系选项设定）。必要时我们合并一些区间。
 *
 * 为减少 palloc 开销，我们总是按 maxvalues 个元素的
 * 容量分配整个数组。只要
 * maxvalues 相当小（64 看起来可以），这就没问题，而
 * values_per_range 关系选项上限为 256，正好如此。
 */
typedef struct Ranges
{
	/* 缓存我们经常需要的信息。 */
	Oid			typid;
	Oid			colloid;
	AttrNumber	attno;
	FmgrInfo   *cmp;

	/* 满足 (2*nranges + nvalues) <= maxvalues */
	int			nranges;		/* values[] 数组中的区间个数 */
	int			nsorted;		/* nvalues 中已排序的个数 */
	int			nvalues;		/* values[] 数组中的点值个数 */
	int			maxvalues;		/* values[] 数组的元素个数 */

	/*
	 * 我们只是把值放进一个大缓冲区，不做任何昂贵
	 * 步骤（排序、去重等等）。缓冲区是目标值个数的
	 * 若干倍，因此压实不那么频繁，
	 * 从而摊销开销。我们保留实际目标，直到最后、在序列化成
	 * 磁盘表示之前，才压实到
	 * 所请求的值个数。
	 */
	/* 所请求的值个数 */
	int			target_maxvalues;

	/* 为本范围存储的值：原始值或区间 */
	Datum		values[FLEXIBLE_ARRAY_MEMBER];
} Ranges;

/*
 * 在磁盘上，汇总存为一个 bytea 值：一个带基本元数据的简单头部，
 * 后面跟着边界值。它带有 varlena
 * 头部，因此可以直接当作 varlena。
 *
 * 序列化细节见 brin_range_serialize/brin_range_deserialize。
 */
typedef struct SerializedRanges
{
	/* varlena 头部（不要直接改动！） */
	int32		vl_len_;

	/* data 数组中所存值的类型 */
	Oid			typid;

	/* 满足 (2*nranges + nvalues) <= maxvalues */
	int			nranges;		/* 数组中的区间个数（已存储） */
	int			nvalues;		/* data 数组中的值个数（全部） */
	int			maxvalues;		/* 值的最大个数（关系选项） */

	/* 包含实际数据 */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} SerializedRanges;

static SerializedRanges *brin_range_serialize(Ranges *range);

static Ranges *brin_range_deserialize(int maxvalues,
									  SerializedRanges *serialized);


/*
 * 用来表示展开后的区间，以便更容易合并与组合。
 *
 * 每个展开区间本质上是一个区间，由最小/最大
 * 值表示，并带一个标志说明它是否为折叠区间（此时
 * 最小值与最大值相等）。设置该标志是为了处理按引用
 * 数据类型：不能简单比较 datum，这样也能省去一些
 * 对该类型比较函数的调用。
 */
typedef struct ExpandedRange
{
	Datum		minval;			/* 下界 */
	Datum		maxval;			/* 上界 */
	bool		collapsed;		/* 若 minval==maxval 则为真 */
} ExpandedRange;

/*
 * 表示两个区间之间的距离（由扩展区间数组中的
 * 下标标识）。
 */
typedef struct DistanceValue
{
	int			index;
	double		value;
} DistanceValue;


/* 支持过程与策略过程的缓存。 */

static FmgrInfo *minmax_multi_get_procinfo(BrinDesc *bdesc, uint16 attno,
										   uint16 procnum);

static FmgrInfo *minmax_multi_get_strategy_procinfo(BrinDesc *bdesc,
													uint16 attno, Oid subtype,
													uint16 strategynum);

typedef struct compare_context
{
	FmgrInfo   *cmpFn;
	Oid			colloid;
} compare_context;

static int	compare_values(const void *a, const void *b, void *arg);


#ifdef USE_ASSERT_CHECKING
/*
 * 用 cmp 函数检查数组值的顺序是否正确
 * （该函数应为 BTLessStrategyNumber）。
 */
static void
AssertArrayOrder(FmgrInfo *cmp, Oid colloid, Datum *values, int nvalues)
{
	int			i;
	Datum		lt;

	for (i = 0; i < (nvalues - 1); i++)
	{
		lt = FunctionCall2Coll(cmp, colloid, values[i], values[i + 1]);
		Assert(DatumGetBool(lt));
	}
}
#endif

/*
 * 对 Ranges 结构做全面检查。
 */
static void
AssertCheckRanges(Ranges *ranges, FmgrInfo *cmpFn, Oid colloid)
{
#ifdef USE_ASSERT_CHECKING
	int			i;

	/* 一些基本的健全性检查 */
	Assert(ranges->nranges >= 0);
	Assert(ranges->nsorted >= 0);
	Assert(ranges->nvalues >= ranges->nsorted);
	Assert(ranges->maxvalues >= 2 * ranges->nranges + ranges->nvalues);
	Assert(ranges->typid != InvalidOid);

	/*
	 * 先检查区间：共有 2*nranges 个边界值，这些值
	 * 必须严格有序（相等意味着该区间已
	 * 折叠，应存为点）。这也保证
	 * 各区间互不重叠。
	 */
	AssertArrayOrder(cmpFn, colloid, ranges->values, 2 * ranges->nranges);

	/* 然后是单点区间（有 nvalues 个边界值） */
	AssertArrayOrder(cmpFn, colloid, &ranges->values[2 * ranges->nranges],
					 ranges->nsorted);

	/*
	 * 检查这些值都没有被区间覆盖（已排序
	 * 和未排序的都要查）
	 */
	if (ranges->nranges > 0)
	{
		for (i = 0; i < ranges->nvalues; i++)
		{
			Datum		compar;
			int			start,
						end;
			Datum		minvalue = ranges->values[0];
			Datum		maxvalue = ranges->values[2 * ranges->nranges - 1];
			Datum		value = ranges->values[2 * ranges->nranges + i];

			compar = FunctionCall2Coll(cmpFn, colloid, value, minvalue);

			/*
			 * 若该值小于第一个区间的下界，
			 * 则它不可能落在任何一个区间里。
			 */
			if (DatumGetBool(compar))
				continue;

			compar = FunctionCall2Coll(cmpFn, colloid, maxvalue, value);

			/*
			 * 同样，若该值大于最后一个区间的上界，
			 * 则它不可能落在任何一个
			 * 区间里。
			 */
			if (DatumGetBool(compar))
				continue;

			/* 二分查找各区间，看 value 是否落在其中 */
			start = 0;			/* 第一个区间 */
			end = ranges->nranges - 1;	/* 最后一个区间 */
			while (true)
			{
				int			midpoint = (start + end) / 2;

				/* 这意味着上一步已经没有区间可看了 */
				if (start > end)
					break;

				/* 从各区间复制最小/最大值 */
				minvalue = ranges->values[2 * midpoint];
				maxvalue = ranges->values[2 * midpoint + 1];

				/*
				 * 该值是否小于 minval？若是，则递归
				 * 到区间数组的左侧。
				 */
				compar = FunctionCall2Coll(cmpFn, colloid, value, minvalue);

				/* 小于本区间中的最小值 */
				if (DatumGetBool(compar))
				{
					end = (midpoint - 1);
					continue;
				}

				/*
				 * 该值是否大于 minval？若是，则递归
				 * 到区间数组的右侧。
				 */
				compar = FunctionCall2Coll(cmpFn, colloid, maxvalue, value);

				/* 大于本区间中的最大值 */
				if (DatumGetBool(compar))
				{
					start = (midpoint + 1);
					continue;
				}

				/* 嘿，找到了匹配的区间 */
				Assert(false);
			}
		}
	}

	/* 未排序部分中的值不得出现在已排序部分 */
	if (ranges->nsorted > 0)
	{
		compare_context cxt;

		cxt.colloid = ranges->colloid;
		cxt.cmpFn = ranges->cmp;

		for (i = ranges->nsorted; i < ranges->nvalues; i++)
		{
			Datum		value = ranges->values[2 * ranges->nranges + i];

			Assert(bsearch_arg(&value, &ranges->values[2 * ranges->nranges],
							   ranges->nsorted, sizeof(Datum),
							   compare_values, (void *) &cxt) == NULL);
		}
	}
#endif
}

/*
 * 检查展开区间（在通过合并来减少区间个数时构造）
 * 是否正确排序且互不重叠。
 */
static void
AssertCheckExpandedRanges(BrinDesc *bdesc, Oid colloid, AttrNumber attno,
						  Form_pg_attribute attr, ExpandedRange *ranges,
						  int nranges)
{
#ifdef USE_ASSERT_CHECKING
	int			i;
	FmgrInfo   *eq;
	FmgrInfo   *lt;

	eq = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
											BTEqualStrategyNumber);

	lt = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
											BTLessStrategyNumber);

	/*
	 * 每个区间本身都应有效，即边界值满足
	 * （下界 <= 上界）。
	 */
	for (i = 0; i < nranges; i++)
	{
		Datum		r;
		Datum		minval = ranges[i].minval;
		Datum		maxval = ranges[i].maxval;

		if (ranges[i].collapsed)	/* 折叠：minval == maxval */
			r = FunctionCall2Coll(eq, colloid, minval, maxval);
		else					/* 未折叠：minval < maxval */
			r = FunctionCall2Coll(lt, colloid, minval, maxval);

		Assert(DatumGetBool(r));
	}

	/*
	 * 并且各区间应有序且不得重叠，即相邻区间的边界满足上界 <
	 * 下界。
	 */
	for (i = 0; i < nranges - 1; i++)
	{
		Datum		r;
		Datum		maxval = ranges[i].maxval;
		Datum		minval = ranges[i + 1].minval;

		r = FunctionCall2Coll(lt, colloid, maxval, minval);

		Assert(DatumGetBool(r));
	}
#endif
}


/*
 * minmax_multi_init
 * 		初始化反序列化后的区间列表，分配全部内存。
 *
 * 这只是区间的内存表示，因此按最大取值个数
 * 分配足够空间（以免区间增长时还要
 * 做 repalloc）。
 */
static Ranges *
minmax_multi_init(int maxvalues)
{
	Size		len;
	Ranges	   *ranges;

	Assert(maxvalues > 0);

	len = offsetof(Ranges, values); /* 固定头部 */
	len += maxvalues * sizeof(Datum);	/* Datum 值 */

	ranges = (Ranges *) palloc0(len);

	ranges->maxvalues = maxvalues;

	return ranges;
}


/*
 * range_deduplicate_values
 *		对简单点那一部分的值去重。
 *
 * 这是一种更便宜的缩小区间规模的办法。它
 * 不触碰区间，只对其余值排序，并不
 * 调用可能相当昂贵的距离函数，等等。
 *
 * 我们知道这些值与区间不重复，因为在加入新值之前已经检查过。
 * 值的已排序部分也是如此。
 */
static void
range_deduplicate_values(Ranges *range)
{
	int			i,
				n;
	int			start;
	compare_context cxt;

	/*
	 * 若没有未排序的值，则已完成（这大概不会
	 * 发生，因为我们是在往未排序部分加值）。
	 */
	if (range->nsorted == range->nvalues)
		return;

	/* 对值排序 */
	cxt.colloid = range->colloid;
	cxt.cmpFn = range->cmp;

	/* 这些值紧接在区间之后开始（区间始终有序） */
	start = 2 * range->nranges;

	/*
	 * XXX 这里也许可以做归并排序，以利用数组前一部分
	 * 已经有序这一事实。若已排序部分很大，可能会
	 * 快上不少。
	 */
	qsort_arg(&range->values[start],
			  range->nvalues, sizeof(Datum),
			  compare_values, &cxt);

	n = 1;
	for (i = 1; i < range->nvalues; i++)
	{
		/* 与前一个值相同，因此存下它 */
		if (compare_values(&range->values[start + i - 1],
						   &range->values[start + i],
						   (void *) &cxt) == 0)
			continue;

		range->values[start + n] = range->values[start + i];

		n++;
	}

	/* 现在全部值都已排序 */
	range->nvalues = n;
	range->nsorted = n;

	AssertCheckRanges(range, range->cmp, range->colloid);
}


/*
 * brin_range_serialize
 *	  把内存中的表示序列化成紧凑的 varlena 值。
 *
 * 只需复制头部，然后再复制内存值数组中
 * 所存的各个值。
 */
static SerializedRanges *
brin_range_serialize(Ranges *range)
{
	Size		len;
	int			nvalues;
	SerializedRanges *serialized;
	Oid			typid;
	int			typlen;
	bool		typbyval;

	char	   *ptr;

	/* 简单的健全性检查 */
	Assert(range->nranges >= 0);
	Assert(range->nsorted >= 0);
	Assert(range->nvalues >= 0);
	Assert(range->maxvalues > 0);
	Assert(range->target_maxvalues > 0);

	/* 此时区间应已压实到目标大小 */
	Assert(2 * range->nranges + range->nvalues <= range->target_maxvalues);

	Assert(range->target_maxvalues <= range->maxvalues);

	/* 区间边界始终有序 */
	Assert(range->nvalues >= range->nsorted);

	/* 若有未排序部分，则对值去重 */
	range_deduplicate_values(range);

	/* 看看实际有多少个 Datum 值 */
	nvalues = 2 * range->nranges + range->nvalues;

	typid = range->typid;
	typbyval = get_typbyval(typid);
	typlen = get_typlen(typid);

	/* 头部总是需要的 */
	len = offsetof(SerializedRanges, data);

	/*
	 * 所需空间取决于数据类型：对定长数据类型
	 * （按值，以及部分按引用）很简单，只要乘上
	 * (attlen * nvalues) 即可。对变长按引用
	 * 类型，则必须遍历全部值并累加长度。
	 */
	if (typlen == -1)			/* 变长（varlena） */
	{
		int			i;

		for (i = 0; i < nvalues; i++)
		{
			len += VARSIZE_ANY(range->values[i]);
		}
	}
	else if (typlen == -2)		/* 字符串（cstring） */
	{
		int			i;

		for (i = 0; i < nvalues; i++)
		{
			/* 别忘了计入结尾的空字符 ;-) */
			len += strlen(DatumGetCString(range->values[i])) + 1;
		}
	}
	else						/* 定长类型（即使是按引用） */
	{
		Assert(typlen > 0);
		len += nvalues * typlen;
	}

	/*
	 * 分配序列化对象，并复制基本信息。
	 * 序列化对象是 varlena，因此要更新头部。
	 */
	serialized = (SerializedRanges *) palloc0(len);
	SET_VARSIZE(serialized, len);

	serialized->typid = typid;
	serialized->nranges = range->nranges;
	serialized->nvalues = range->nvalues;
	serialized->maxvalues = range->target_maxvalues;

	/*
	 * 现在也复制边界值（与长度计算一样，这
	 * 取决于具体的数据类型）。
	 */
	ptr = serialized->data;		/* 序列化数据的起点 */

	for (int i = 0; i < nvalues; i++)
	{
		if (typbyval)			/* 简单的按值数据类型 */
		{
			Datum		tmp;

			/*
			 * 对按值类型，只需复制有效字节。
			 * 不能直接用 memcpy，因为它假定小端
			 * 行为。store_att_byval 几乎满足需要，但它
			 * 需要正确对齐的缓冲区，而输出缓冲区并不
			 * 保证这一点。因此我们使用一个局部 Datum 变量（它
			 * 保证正确对齐），再从中复制该值。
			 */
			store_att_byval(&tmp, range->values[i], typlen);

			memcpy(ptr, &tmp, typlen);
			ptr += typlen;
		}
		else if (typlen > 0)	/* 定长按引用类型 */
		{
			memcpy(ptr, DatumGetPointer(range->values[i]), typlen);
			ptr += typlen;
		}
		else if (typlen == -1)	/* 变长（varlena） */
		{
			int			tmp = VARSIZE_ANY(DatumGetPointer(range->values[i]));

			memcpy(ptr, DatumGetPointer(range->values[i]), tmp);
			ptr += tmp;
		}
		else if (typlen == -2)	/* 字符串（cstring） */
		{
			int			tmp = strlen(DatumGetCString(range->values[i])) + 1;

			memcpy(ptr, DatumGetCString(range->values[i]), tmp);
			ptr += tmp;
		}

		/* 确保没有越过缓冲区末尾 */
		Assert(ptr <= ((char *) serialized + len));
	}

	/* 精确大小 */
	Assert(ptr == ((char *) serialized + len));

	return serialized;
}

/*
 * brin_range_deserialize
 *	  把内存中的表示序列化成紧凑的 varlena 值。
 *
 * 只需复制头部，然后再复制内存值数组中
 * 所存的各个值。
 */
static Ranges *
brin_range_deserialize(int maxvalues, SerializedRanges *serialized)
{
	int			i,
				nvalues;
	char	   *ptr,
			   *dataptr;
	bool		typbyval;
	int			typlen;
	Size		datalen;

	Ranges	   *range;

	Assert(serialized->nranges >= 0);
	Assert(serialized->nvalues >= 0);
	Assert(serialized->maxvalues > 0);

	nvalues = 2 * serialized->nranges + serialized->nvalues;

	Assert(nvalues <= serialized->maxvalues);
	Assert(serialized->maxvalues <= maxvalues);

	range = minmax_multi_init(maxvalues);

	/* 复制头部信息 */
	range->nranges = serialized->nranges;
	range->nvalues = serialized->nvalues;
	range->nsorted = serialized->nvalues;
	range->maxvalues = maxvalues;
	range->target_maxvalues = serialized->maxvalues;

	range->typid = serialized->typid;

	typbyval = get_typbyval(serialized->typid);
	typlen = get_typlen(serialized->typid);

	/*
	 * 现在把这些值拆回 Datum 数组。必须复制
	 * 数据，因为序列化表示忽略对齐，而且我们
	 * 也不想依赖它一直留在原地。
	 */
	ptr = serialized->data;

	/*
	 * 不想分配很多小块，因此一次性把全部内容
	 * 分配在一块里。需要多少空间？
	 *
	 * XXX 简单的按值数据类型其实不必复制。
	 */
	datalen = 0;
	dataptr = NULL;
	for (i = 0; (i < nvalues) && (!typbyval); i++)
	{
		if (typlen > 0)			/* 定长按引用类型 */
			datalen += MAXALIGN(typlen);
		else if (typlen == -1)	/* 变长（varlena） */
		{
			datalen += MAXALIGN(VARSIZE_ANY(ptr));
			ptr += VARSIZE_ANY(ptr);
		}
		else if (typlen == -2)	/* 字符串（cstring） */
		{
			Size		slen = strlen(ptr) + 1;

			datalen += MAXALIGN(slen);
			ptr += slen;
		}
	}

	if (datalen > 0)
		dataptr = palloc(datalen);

	/*
	 * 恢复源指针（计算需要分配的空间时
	 * 它可能已被改动）。
	 */
	ptr = serialized->data;

	for (i = 0; i < nvalues; i++)
	{
		if (typbyval)			/* 简单的按值数据类型 */
		{
			Datum		v = 0;

			memcpy(&v, ptr, typlen);

			range->values[i] = fetch_att(&v, true, typlen);
			ptr += typlen;
		}
		else if (typlen > 0)	/* 定长按引用类型 */
		{
			range->values[i] = PointerGetDatum(dataptr);

			memcpy(dataptr, ptr, typlen);
			dataptr += MAXALIGN(typlen);

			ptr += typlen;
		}
		else if (typlen == -1)	/* 变长（varlena） */
		{
			range->values[i] = PointerGetDatum(dataptr);

			memcpy(dataptr, ptr, VARSIZE_ANY(ptr));
			dataptr += MAXALIGN(VARSIZE_ANY(ptr));
			ptr += VARSIZE_ANY(ptr);
		}
		else if (typlen == -2)	/* 字符串（cstring） */
		{
			Size		slen = strlen(ptr) + 1;

			range->values[i] = PointerGetDatum(dataptr);

			memcpy(dataptr, ptr, slen);
			dataptr += MAXALIGN(slen);
			ptr += slen;
		}

		/* 确保没有越过缓冲区末尾 */
		Assert(ptr <= ((char *) serialized + VARSIZE_ANY(serialized)));
	}

	/* 应当正好消耗完整个输入值 */
	Assert(ptr == ((char *) serialized + VARSIZE_ANY(serialized)));

	/* 返回反序列化后的值 */
	return range;
}

/*
 * compare_expanded_ranges
 *	  比较展开区间：先比最小值，再比最大值。
 *
 * 我们保证单个 Ranges 对象中的区间互不重叠，因此
 * 只按最小值排序似乎就够了。但合并两个
 * Ranges（发生在 union 函数中）时，区间实际上可能
 * 重叠。所以两端都要比较。
 */
static int
compare_expanded_ranges(const void *a, const void *b, void *arg)
{
	ExpandedRange *ra = (ExpandedRange *) a;
	ExpandedRange *rb = (ExpandedRange *) b;
	Datum		r;

	compare_context *cxt = (compare_context *) arg;

	/* 先比较 minval */
	r = FunctionCall2Coll(cxt->cmpFn, cxt->colloid, ra->minval, rb->minval);

	if (DatumGetBool(r))
		return -1;

	r = FunctionCall2Coll(cxt->cmpFn, cxt->colloid, rb->minval, ra->minval);

	if (DatumGetBool(r))
		return 1;

	/* 再比较 maxval */
	r = FunctionCall2Coll(cxt->cmpFn, cxt->colloid, ra->maxval, rb->maxval);

	if (DatumGetBool(r))
		return -1;

	r = FunctionCall2Coll(cxt->cmpFn, cxt->colloid, rb->maxval, ra->maxval);

	if (DatumGetBool(r))
		return 1;

	return 0;
}

/*
 * compare_values
 *	  比较这些值。
 */
static int
compare_values(const void *a, const void *b, void *arg)
{
	Datum	   *da = (Datum *) a;
	Datum	   *db = (Datum *) b;
	Datum		r;

	compare_context *cxt = (compare_context *) arg;

	r = FunctionCall2Coll(cxt->cmpFn, cxt->colloid, *da, *db);

	if (DatumGetBool(r))
		return -1;

	r = FunctionCall2Coll(cxt->cmpFn, cxt->colloid, *db, *da);

	if (DatumGetBool(r))
		return 1;

	return 0;
}

/*
 * 检查新值是否匹配某个已有区间。
 */
static bool
has_matching_range(BrinDesc *bdesc, Oid colloid, Ranges *ranges,
				   Datum newval, AttrNumber attno, Oid typid)
{
	Datum		compar;

	Datum		minvalue;
	Datum		maxvalue;

	FmgrInfo   *cmpLessFn;
	FmgrInfo   *cmpGreaterFn;

	/* 在区间上做二分查找 */
	int			start,
				end;

	if (ranges->nranges == 0)
		return false;

	minvalue = ranges->values[0];
	maxvalue = ranges->values[2 * ranges->nranges - 1];

	/*
	 * 否则，需要把新值与所有区间的边界比较。
	 * 先检查它是否小于绝对最小值，也就是
	 * 数组中的第一个值。
	 */
	cmpLessFn = minmax_multi_get_strategy_procinfo(bdesc, attno, typid,
												   BTLessStrategyNumber);
	compar = FunctionCall2Coll(cmpLessFn, colloid, newval, minvalue);

	/* 小于区间列表中的最小值 */
	if (DatumGetBool(compar))
		return false;

	/*
	 * 现在把它与现有最大值比较（数据数组中的最后一个值）。
	 * 但仅当最小值检查尚未排除可能的匹配时
	 * 才做这一步。
	 */
	cmpGreaterFn = minmax_multi_get_strategy_procinfo(bdesc, attno, typid,
													  BTGreaterStrategyNumber);
	compar = FunctionCall2Coll(cmpGreaterFn, colloid, newval, maxvalue);

	if (DatumGetBool(compar))
		return false;

	/*
	 * 因此我们知道它落在整体最小/最大之间，问题是它
	 * 落在某个区间里还是间隙里。我们对各个区间做二分查找：
	 * 对每个区间检查是否相等（值落入
	 * 该区间），然后检查当前区间之上或之下的
	 * 区间。
	 */
	start = 0;					/* 第一个区间 */
	end = (ranges->nranges - 1);	/* 最后一个区间 */
	while (true)
	{
		int			midpoint = (start + end) / 2;

		/* 这意味着上一步已经没有区间可看了 */
		if (start > end)
			return false;

		/* 从各区间复制最小/最大值 */
		minvalue = ranges->values[2 * midpoint];
		maxvalue = ranges->values[2 * midpoint + 1];

		/*
		 * 该值是否小于 minval？若是，则递归
		 * 到区间数组的左侧。
		 */
		compar = FunctionCall2Coll(cmpLessFn, colloid, newval, minvalue);

		/* 小于本区间中的最小值 */
		if (DatumGetBool(compar))
		{
			end = (midpoint - 1);
			continue;
		}

		/*
		 * 该值是否大于 minval？若是，则递归
		 * 到区间数组的右侧。
		 */
		compar = FunctionCall2Coll(cmpGreaterFn, colloid, newval, maxvalue);

		/* 大于本区间中的最大值 */
		if (DatumGetBool(compar))
		{
			start = (midpoint + 1);
			continue;
		}

		/* 嘿，找到了匹配的区间 */
		return true;
	}

	return false;
}


/*
 * range_contains_value
 * 		看新值是否已经包含在区间列表中。
 *
 * 先检查区间列表。这里用一个小技巧：先把
 * 该值与整个范围的最小/最大比较（第一个区间的最小，
 * 最后一个区间的最大），只有通过之后才检查
 * 各个区间。
 *
 * 若该值不匹配任何区间，再检查精确值。
 * 只需遍历它们并调用相等操作符。
 *
 * 最后一个参数（full）决定是否需要搜索全部
 * 值，包括未排序部分。full=false 时不搜索未排序部分，
 * 这可能产生假阴性，并在未排序部分出现重复值
 * （仅限未排序部分），但在构建区间时这
 * 没问题：序列化之前会去重，而且只有
 * 已经存在未排序值时才会发生（因此它已经被改过）。
 *
 * 序列化后的区间没有任何未排序值，因此查询时不会因此
 * 产生假阴性。
 */
static bool
range_contains_value(BrinDesc *bdesc, Oid colloid,
					 AttrNumber attno, Form_pg_attribute attr,
					 Ranges *ranges, Datum newval, bool full)
{
	int			i;
	FmgrInfo   *cmpEqualFn;
	Oid			typid = attr->atttypid;

	/*
	 * 若有区间，先检查它们。先检查整个
	 * 范围，只有仍有可能匹配时才
	 * 检查各个区间。
	 */
	if (has_matching_range(bdesc, colloid, ranges, newval, attno, typid))
		return true;

	cmpEqualFn = minmax_multi_get_strategy_procinfo(bdesc, attno, typid,
													BTEqualStrategyNumber);

	/*
	 * 没有匹配的区间，因此检查已排序的值。
	 *
	 * 值很少时做顺序查找，个数较多时做二分
	 * 查找（超过 16 个值）。这个阈值有些任意，
	 * 取决于比较函数有多贵。
	 *
	 * XXX 若这里使用该阈值，也许 has_matching_range 里也应同样处理？
	 * 或者也许应该一直做二分查找？
	 *
	 * XXX 也可以用与区间相同的优化，检查该值是否
	 * 介于最小/最大之间，从而也许不必查看全部已排序值
	 * 就能把它们全部排除。
	 */
	if (ranges->nsorted >= 16)
	{
		compare_context cxt;

		cxt.colloid = ranges->colloid;
		cxt.cmpFn = ranges->cmp;

		if (bsearch_arg(&newval, &ranges->values[2 * ranges->nranges],
						ranges->nsorted, sizeof(Datum),
						compare_values, (void *) &cxt) != NULL)
			return true;
	}
	else
	{
		for (i = 2 * ranges->nranges; i < 2 * ranges->nranges + ranges->nsorted; i++)
		{
			Datum		compar;

			compar = FunctionCall2Coll(cmpEqualFn, colloid, newval, ranges->values[i]);

			/* 找到了精确匹配 */
			if (DatumGetBool(compar))
				return true;
		}
	}

	/* 若未让我们检查未排序部分，则已完成。 */
	if (!full)
		return false;

	/* 检查未排序部分。 */
	for (i = 2 * ranges->nranges + ranges->nsorted; i < 2 * ranges->nranges + ranges->nvalues; i++)
	{
		Datum		compar;

		compar = FunctionCall2Coll(cmpEqualFn, colloid, newval, ranges->values[i]);

		/* 找到了精确匹配 */
		if (DatumGetBool(compar))
			return true;
	}

	/* 该值不被这个 BRIN 元组覆盖 */
	return false;
}

/*
 * 把 Ranges 中的区间展开到 ExpandedRange 数组。这里假定
 * eranges 已预先分配且大小正确：需要有
 * (nranges + nvalues) 个元素。
 *
 * 展开区间的顺序是任意的。我们先展开区间，
 * 这一部分是有序的。然后再展开值，这一部分可能
 * 无序。
 */
static void
fill_expanded_ranges(ExpandedRange *eranges, int neranges, Ranges *ranges)
{
	int			idx;
	int			i;

	/* 检查输出数组的大小是否正确。 */
	Assert(neranges == (ranges->nranges + ranges->nvalues));

	idx = 0;
	for (i = 0; i < ranges->nranges; i++)
	{
		eranges[idx].minval = ranges->values[2 * i];
		eranges[idx].maxval = ranges->values[2 * i + 1];
		eranges[idx].collapsed = false;
		idx++;

		Assert(idx <= neranges);
	}

	for (i = 0; i < ranges->nvalues; i++)
	{
		eranges[idx].minval = ranges->values[2 * ranges->nranges + i];
		eranges[idx].maxval = ranges->values[2 * ranges->nranges + i];
		eranges[idx].collapsed = true;
		idx++;

		Assert(idx <= neranges);
	}

	/* 产生的元素个数是否符合预期？ */
	Assert(idx == neranges);

	return;
}

/*
 * 对展开区间排序并去重。
 *
 * 这些区间可能有重复：我们只是追加值，并不
 * 检查重复等等。因此去重也许就能把
 * 区间个数减得够多，从而不必再计算距离等等。
 *
 * 返回展开区间的个数。
 */
static int
sort_expanded_ranges(FmgrInfo *cmp, Oid colloid,
					 ExpandedRange *eranges, int neranges)
{
	int			n;
	int			i;
	compare_context cxt;

	Assert(neranges > 0);

	/* 对值排序 */
	cxt.colloid = colloid;
	cxt.cmpFn = cmp;

	/*
	 * XXX 我们对全部值做 qsort，但也可以利用
	 * 部分输入已经有序这一事实（全部区间，也许还有
	 * 部分点）来做归并排序。
	 */
	qsort_arg(eranges, neranges, sizeof(ExpandedRange),
			  compare_expanded_ranges, &cxt);

	/*
	 * 对区间去重：把每个区间与前一个比较，
	 * 并跳过重复的。
	 */
	n = 1;
	for (i = 1; i < neranges; i++)
	{
		/* 若当前区间与前一个相等，则什么也不做 */
		if (!compare_expanded_ranges(&eranges[i - 1], &eranges[i], (void *) &cxt))
			continue;

		/* 否则，把它复制到第 n 个位置（若还不在那里） */
		if (i != n)
			memcpy(&eranges[n], &eranges[i], sizeof(ExpandedRange));

		n++;
	}

	Assert((n > 0) && (n <= neranges));

	return n;
}

/*
 * 在组合多个 Range 值（union 函数中）时，有些
 * 区间可能重叠。我们直接合并重叠区间来修正。
 *
 * XXX 这里假定展开区间先前已排序（先按 minval，
 * 再按 maxval）。检测重叠时利用这一点。
 */
static int
merge_overlapping_ranges(FmgrInfo *cmp, Oid colloid,
						 ExpandedRange *eranges, int neranges)
{
	int			idx;

	/* 若区间 (idx) 与 (idx+1) 重叠，则合并它们。 */
	idx = 0;
	while (idx < (neranges - 1))
	{
		Datum		r;

		/*
		 * 比较 [?,maxval] 与 [minval,?]：若 (minval
		 * < maxval)，则区间重叠
		 */
		r = FunctionCall2Coll(cmp, colloid,
							  eranges[idx].maxval,
							  eranges[idx + 1].minval);

		/*
		 * 不重叠：maxval < minval。而且我们知道区间
		 * 有序，因此不会再有重叠，因为其余
		 * 区间的 minval 都更大或相等。
		 */
		if (DatumGetBool(r))
		{
			/* 继续下一个区间 */
			idx += 1;
			continue;
		}

		/*
		 * 因此区间 idx 与 idx+1 确实重叠，但不知道
		 * idx+1 是否包含在 idx 中，还是只部分重叠。
		 * 所以比较上界并保留较大的那个。
		 */
		r = FunctionCall2Coll(cmp, colloid,
							  eranges[idx].maxval,
							  eranges[idx + 1].maxval);

		if (DatumGetBool(r))
			eranges[idx].maxval = eranges[idx + 1].maxval;

		/*
		 * 该区间肯定不再是折叠的（不论
		 * 先前状态如何）。
		 */
		eranges[idx].collapsed = false;

		/*
		 * 现在把 (idx+1) 区间整个去掉：把剩余
		 * 区间前移 1。共有 neranges 个元素，需要移动
		 * 从 (idx+2) 开始的元素。因此要移动的元素个数是
		 * [ncranges - (idx+2)]。
		 */
		memmove(&eranges[idx + 1], &eranges[idx + 2],
				(neranges - (idx + 2)) * sizeof(ExpandedRange));

		/*
		 * 减少区间个数，并重复（仍用同一个区间，因为
		 * 合并之后它可能还会与更多区间重叠）。
		 */
		neranges--;
	}

	return neranges;
}

/*
 * 距离值的简单比较器，比较 double 值。
 * 这里有意按降序排列距离，也就是
 * 较长的间隙排在前面。
 */
static int
compare_distances(const void *a, const void *b)
{
	DistanceValue *da = (DistanceValue *) a;
	DistanceValue *db = (DistanceValue *) b;

	if (da->value < db->value)
		return 1;
	else if (da->value > db->value)
		return -1;

	return 0;
}

/*
 * 给定展开区间数组，计算每对区间之间的间隙大小。
 * neranges 个区间有 (neranges-1) 个间隙。
 *
 * 只需调用 "distance" 函数，计算连续区间对的 (max-min)。
 * 该函数可能相当昂贵，因此只做
 * 一次（然后用它尽可能多地挑选要合并的区间）。
 *
 * 细节见 reduce_expanded_ranges。
 */
static DistanceValue *
build_distances(FmgrInfo *distanceFn, Oid colloid,
				ExpandedRange *eranges, int neranges)
{
	int			i;
	int			ndistances;
	DistanceValue *distances;

	Assert(neranges > 0);

	/* 若只有一个区间，就没有距离可算。 */
	if (neranges == 1)
		return NULL;

	ndistances = (neranges - 1);
	distances = (DistanceValue *) palloc0(sizeof(DistanceValue) * ndistances);

	/*
	 * 把区间走一遍，计算区间之间的距离，
	 * 以便只排序一次。
	 */
	for (i = 0; i < ndistances; i++)
	{
		Datum		a1,
					a2,
					r;

		a1 = eranges[i].maxval;
		a2 = eranges[i + 1].minval;

		/* 计算间隙长度（介于 max/min 之间） */
		r = FunctionCall2Coll(distanceFn, colloid, a1, a2);

		/* 记住该距离对应的间隙下标 */
		distances[i].index = i;
		distances[i].value = DatumGetFloat8(r);
	}

	/*
	 * 按降序排列距离，使最长的间隙位于
	 * 前面。
	 */
	pg_qsort(distances, ndistances, sizeof(DistanceValue), compare_distances);

	return distances;
}

/*
 * 为已有区间（以及单点区间）构造展开区间，
 * 也包括未能放入数组的新值。这种展开
 * 表示让处理更容易一些，因为区间和点可以
 * 用同样的方式处理。
 *
 * 我们对展开区间排序并去重。这是必要的，因为
 * 点可能无序。而且两部分（区间和
 * 点）各自是排好序的。
 */
static ExpandedRange *
build_expanded_ranges(FmgrInfo *cmp, Oid colloid, Ranges *ranges,
					  int *nranges)
{
	int			neranges;
	ExpandedRange *eranges;

	/* 区间和点都各自展开成单独的元素 */
	neranges = ranges->nranges + ranges->nvalues;

	eranges = (ExpandedRange *) palloc0(neranges * sizeof(ExpandedRange));

	/* 填充展开区间 */
	fill_expanded_ranges(eranges, neranges, ranges);

	/* 对展开区间排序并去重 */
	neranges = sort_expanded_ranges(cmp, colloid, eranges, neranges);

	/* 记住我们构造了多少个区间 */
	*nranges = neranges;

	return eranges;
}

#ifdef USE_ASSERT_CHECKING
/*
 * 计算存储这些区间所需的边界值个数。每个单点
 * 区间用一个值存储，每个普通区间需要两个。
 */
static int
count_values(ExpandedRange *cranges, int ncranges)
{
	int			i;
	int			count;

	count = 0;
	for (i = 0; i < ncranges; i++)
	{
		if (cranges[i].collapsed)
			count += 1;
		else
			count += 2;
	}

	return count;
}
#endif

/*
 * reduce_expanded_ranges
 *		减少区间，直到值的个数足够低
 *
 * 不断组合区间，直到边界值个数降到
 * max_values 所指定的阈值以下。做法是按区间之间的距离
 * 合并足够多的区间。
 *
 * 返回结果区间的个数。
 *
 * 我们只用全局最小/最大，再为足够多的
 * 最大间隙加上边界。每个间隙增加 2 个值，因此只用 (target/2-1)
 * 个距离。然后把全部值排序：每两个值就是
 * 一个区间的边界（可能是折叠的）。
 *
 * XXX 有些区间可能已折叠（即最小/最大值
 * 相等），但目前先忽略。我们可以重复该过程，
 * 再递归地多加几个间隙。
 *
 * XXX 要合并的区间完全按距离选取。但这
 * 未必是最好的策略，例如当多个间隙
 * 长度相等（或非常接近）时。
 *
 * 例如考虑点 1, 2, 3, .., 64，它们的间隙
 * 当然都是长度 1。这种情况下，我们倾向于选第一个
 * 该长度的间隙，于是得到：
 *
 *    第 1 步：  [1, 2], 3, 4, 5, .., 64
 *    第 2 步：  [1, 3], 4, 5,    .., 64
 *    第 3 步：  [1, 4], 5,       .., 64
 *    ...
 *
 * 最终会得到一个 "大" 区间和多个小点。
 * 这也许可以，但看起来有点奇怪，也不是最优。也许
 * 挑选要合并的区间时还应考虑别的因素，例如
 * 区间的长度？或者随机化区间的选择，
 * 概率与距离成反比（间隙长度
 * 可能非常接近，但并不完全相同）。
 *
 * XXX 或者也许只需用随机值作为
 * 决胜项，或给实际距离加上随机噪声。
 */
static int
reduce_expanded_ranges(ExpandedRange *eranges, int neranges,
					   DistanceValue *distances, int max_values,
					   FmgrInfo *cmp, Oid colloid)
{
	int			i;
	int			nvalues;
	Datum	   *values;

	compare_context cxt;

	/* 区间之间的间隙总数 */
	int			ndistances = (neranges - 1);

	/* 要保留的间隙个数 */
	int			keep = (max_values / 2 - 1);

	/*
	 * 也许区间个数已经足够低了？
	 *
	 * XXX 这应当发生在真正做排序之类的昂贵操作之前，
	 * 所以也许这里只该是一个断言。
	 */
	if (keep >= ndistances)
		return neranges;

	/* 对值排序 */
	cxt.colloid = colloid;
	cxt.cmpFn = cmp;

	/* 为边界值分配空间 */
	nvalues = 0;
	values = (Datum *) palloc(sizeof(Datum) * max_values);

	/* 加入全局最小/最大值，取自第一个/最后一个区间 */
	values[nvalues++] = eranges[0].minval;
	values[nvalues++] = eranges[neranges - 1].maxval;

	/* 为足够多的间隙加入边界值 */
	for (i = 0; i < keep; i++)
	{
		/* (index) 与 (index+1) 区间之间的间隙下标 */
		int			index = distances[i].index;

		Assert((index >= 0) && ((index + 1) < neranges));

		/* 加入前一区间的 max，以及下一区间的 minval */
		values[nvalues++] = eranges[index].maxval;
		values[nvalues++] = eranges[index + 1].minval;

		Assert(nvalues <= max_values);
	}

	/* 区间值的个数应为偶数。 */
	Assert(nvalues % 2 == 0);

	/*
	 * 用比较函数对值排序，并由排序结果
	 * 构成区间。
	 */
	qsort_arg(values, nvalues, sizeof(Datum),
			  compare_values, &cxt);

	/* 有 nvalues 个边界值，即 nvalues/2 个区间。 */
	for (i = 0; i < (nvalues / 2); i++)
	{
		eranges[i].minval = values[2 * i];
		eranges[i].maxval = values[2 * i + 1];

		/* 若两端边界值相同，则是折叠区间 */
		eranges[i].collapsed = (compare_values(&values[2 * i],
											   &values[2 * i + 1],
											   &cxt) == 0);
	}

	return (nvalues / 2);
}

/*
 * 把 ExpandedRanges 中的边界值存回 ranges（只使用
 * 所需的最少个数）。
 */
static void
store_expanded_ranges(Ranges *ranges, ExpandedRange *eranges, int neranges)
{
	int			i;
	int			idx = 0;

	/* 先复制普通区间 */
	ranges->nranges = 0;
	for (i = 0; i < neranges; i++)
	{
		if (!eranges[i].collapsed)
		{
			ranges->values[idx++] = eranges[i].minval;
			ranges->values[idx++] = eranges[i].maxval;
			ranges->nranges++;
		}
	}

	/* 再复制折叠的那些 */
	ranges->nvalues = 0;
	for (i = 0; i < neranges; i++)
	{
		if (eranges[i].collapsed)
		{
			ranges->values[idx++] = eranges[i].minval;
			ranges->nvalues++;
		}
	}

	/* 全部值都已排序 */
	ranges->nsorted = ranges->nvalues;

	Assert(count_values(eranges, neranges) == 2 * ranges->nranges + ranges->nvalues);
	Assert(2 * ranges->nranges + ranges->nvalues <= ranges->maxvalues);
}


/*
 * 考虑在区间中腾出空间。检查是否至少还有一个新值的
 * 空位，必要时执行压实。
 *
 * 若该值确实被修改则返回 true。
 */
static bool
ensure_free_space_in_buffer(BrinDesc *bdesc, Oid colloid,
							AttrNumber attno, Form_pg_attribute attr,
							Ranges *range)
{
	MemoryContext ctx;
	MemoryContext oldctx;

	FmgrInfo   *cmpFn,
			   *distanceFn;

	/* 展开区间 */
	ExpandedRange *eranges;
	int			neranges;
	DistanceValue *distances;

	/*
	 * 若缓冲区还有空位，则不必修改任何东西，
	 * 就此完成。
	 */
	if (2 * range->nranges + range->nvalues < range->maxvalues)
		return false;

	/* 肯定需要比较函数，现在就查出来 */
	cmpFn = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
											   BTLessStrategyNumber);

	/* 若有未排序部分，则对值去重 */
	range_deduplicate_values(range);

	/*
	 * 仅仅去重是否已经腾出足够的空位？
	 *
	 * 我们并不只是再对照 range->maxvalues 检查一次。去重
	 * 也许只腾出很少空间（例如只一个值），迫使我们
	 * 非常频繁地去重。这种情况下，最好做
	 * 压实，多腾出一些空间。
	 */
	if (2 * range->nranges + range->nvalues <= range->maxvalues * MINMAX_BUFFER_LOAD_FACTOR)
		return true;

	/*
	 * 需要组合一些已有区间，以减少必须
	 * 存储的值个数。
	 *
	 * distanceFn 调用（内部可能调用例如 numeric_le）可能
	 * 分配不少内存，绝不能泄漏（即使对单个 BRIN 页范围，
	 * 也可能要反复做这件事）。否则
	 * 在例如构建新索引时会出问题。因此我们使用内存
	 * 上下文，并确保最后释放内存（因此若多次调用
	 * 距离函数，也许会有问题，不过也无妨）。
	 */
	ctx = AllocSetContextCreate(CurrentMemoryContext,
								"minmax-multi context",
								ALLOCSET_DEFAULT_SIZES);

	oldctx = MemoryContextSwitchTo(ctx);

	/* 构造展开区间 */
	eranges = build_expanded_ranges(cmpFn, colloid, range, &neranges);

	/* 还需要 "distance" 过程 */
	distanceFn = minmax_multi_get_procinfo(bdesc, attno, PROCNUM_DISTANCE);

	/* 构造间隙距离数组，并按升序排序 */
	distances = build_distances(distanceFn, colloid, eranges, neranges);

	/*
	 * 不断组合区间，直到至少释放 50% 的空间。这个
	 * 阈值有些任意，也许需要调优。不得使用
	 * 过低或过高的值。
	 */
	neranges = reduce_expanded_ranges(eranges, neranges, distances,
									  range->maxvalues * MINMAX_BUFFER_LOAD_FACTOR,
									  cmpFn, colloid);

	/* 确保区间个数已经减得足够多。 */
	Assert(count_values(eranges, neranges) <= range->maxvalues * MINMAX_BUFFER_LOAD_FACTOR);

	/* 把展开区间分解成普通区间和单值 */
	store_expanded_ranges(range, eranges, neranges);

	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(ctx);

	/* 区间是不是被我们弄坏了？ */
	AssertCheckRanges(range, cmpFn, colloid);

	return true;
}

/*
 * range_add_value
 * 		把新值加入 minmax-multi 区间。
 */
static bool
range_add_value(BrinDesc *bdesc, Oid colloid,
				AttrNumber attno, Form_pg_attribute attr,
				Ranges *ranges, Datum newval)
{
	FmgrInfo   *cmpFn;
	bool		modified = false;

	/* 肯定需要比较函数，现在就查出来 */
	cmpFn = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
											   BTLessStrategyNumber);

	/* 对输入区间做全面检查 */
	AssertCheckRanges(ranges, cmpFn, colloid);

	/*
	 * 确保缓冲区有足够空位。只有缓冲区已满时才触发，
	 * 这意味着它一定被改过，因为我们把它的尺寸
	 * 定得比磁盘上所存的更大。
	 *
	 * 这必须发生在检查该值是否已包含在
	 * 区间中之前，因为该值可能在未排序部分，而
	 * range_contains_value 并不检查那里。去重随后会把它移到
	 * 已排序部分，我们又会再加一次该值，这就违反了
	 * 与区间或已排序值永不重复的规则。
	 *
	 * 也可以先去重再重新检查该值是否已包含，但
	 * 那似乎不值得。反正都要去重，何不
	 * 现在就做。
	 */
	modified = ensure_free_space_in_buffer(bdesc, colloid,
										   attno, attr, ranges);

	/*
	 * 若该值已经被区间覆盖，则直接返回。
	 *
	 * 也可以一直加值直到达到 values_per_range，然后成批
	 * 去重，指望效率更好。但那样
	 * 每次都会真正修改区间，也就必须
	 * 序列化该值，而这会 palloc、遍历值、复制它们，
	 * 等等。并不便宜。
	 *
	 * 因此我们改为做检查，这应当相当便宜，只要
	 * 比较函数不是很贵。
	 *
	 * 这也意味着 values 数组不能含有重复值。
	 */
	if (range_contains_value(bdesc, colloid, attno, attr, ranges, newval, false))
		return modified;

	/* 若需要，复制该值。 */
	newval = datumCopy(newval, attr->attbyval, attr->attlen);

	/*
	 * 若 values 数组还有空位，复制进去即完成。
	 *
	 * 我们希望保持值有序（以加快查找），因此做一次
	 * 简单的插入排序。也可以做得更精细，例如
	 * 只是偶尔才排序，但对较小的个数（例如
	 * maxvalues 为 64）这样就可以了。
	 */
	ranges->values[2 * ranges->nranges + ranges->nvalues] = newval;
	ranges->nvalues++;

	/* 若加入的是第一个值，可以把它视为已排序。 */
	if (ranges->nvalues == 1)
		ranges->nsorted = 1;

	/*
	 * 检查边界值的顺序没有被破坏（两部分都会查，
	 * 但没有坏处）。
	 */
	AssertCheckRanges(ranges, cmpFn, colloid);

	/* 检查区间包含我们刚加入的值。 */
	Assert(range_contains_value(bdesc, colloid, attno, attr, ranges, newval, true));

	/* 是的，我们已经修改了该区间 */
	return true;
}

/*
 * 生成 "批模式" 期间收集的数据的区间表示。
 * 这与 reduce_expanded_ranges 类似，只是不能假定
 * 值已排序，而且可能有重复值。
 */
static void
compactify_ranges(BrinDesc *bdesc, Ranges *ranges, int max_values)
{
	FmgrInfo   *cmpFn,
			   *distanceFn;

	/* 展开区间 */
	ExpandedRange *eranges;
	int			neranges;
	DistanceValue *distances;

	MemoryContext ctx;
	MemoryContext oldctx;

	/*
	 * 是否真的需要压实？
	 *
	 * 需要压实有两个原因：首先，值可能
	 * 过多，或者有些值可能尚未排序。
	 */
	if ((ranges->nranges * 2 + ranges->nvalues <= max_values) &&
		(ranges->nsorted == ranges->nvalues))
		return;

	/* 肯定需要比较函数，现在就查出来 */
	cmpFn = minmax_multi_get_strategy_procinfo(bdesc, ranges->attno, ranges->typid,
											   BTLessStrategyNumber);

	/* 还需要 "distance" 过程 */
	distanceFn = minmax_multi_get_procinfo(bdesc, ranges->attno, PROCNUM_DISTANCE);

	/*
	 * distanceFn 调用（内部可能调用例如 numeric_le）可能
	 * 分配不少内存，绝不能泄漏。否则，
	 * 在例如构建索引时会出问题。因此我们创建局部
	 * 内存上下文，并确保在离开本函数之前释放内存
	 * （而不是每次调用之后）。
	 */
	ctx = AllocSetContextCreate(CurrentMemoryContext,
								"minmax-multi context",
								ALLOCSET_DEFAULT_SIZES);

	oldctx = MemoryContextSwitchTo(ctx);

	/* 构造展开区间 */
	eranges = build_expanded_ranges(cmpFn, ranges->colloid, ranges, &neranges);

	/* 构造间隙距离数组，并按升序排序 */
	distances = build_distances(distanceFn, ranges->colloid,
								eranges, neranges);

	/*
	 * 不断组合区间，直到低于 max_values。这里不用任何缩放
	 * 因子，因为这用在序列化期间，而且我们不预期
	 * 很快还会插入更多元组。
	 */
	neranges = reduce_expanded_ranges(eranges, neranges, distances,
									  max_values, cmpFn, ranges->colloid);

	Assert(count_values(eranges, neranges) <= max_values);

	/* 转换回普通区间和单值 */
	store_expanded_ranges(ranges, eranges, neranges);

	/* 检查全部区间不变量 */
	AssertCheckRanges(ranges, cmpFn, ranges->colloid);

	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(ctx);
}

Datum
brin_minmax_multi_opcinfo(PG_FUNCTION_ARGS)
{
	BrinOpcInfo *result;

	/*
	 * opaque->strategy_procinfos 惰性初始化；这里由 palloc0 置为
	 * 全部未初始化，它会把 fn_oid 设为 InvalidOid。
	 */

	result = palloc0(MAXALIGN(SizeofBrinOpcInfo(1)) +
					 sizeof(MinmaxMultiOpaque));
	result->oi_nstored = 1;
	result->oi_regular_nulls = true;
	result->oi_opaque = (MinmaxMultiOpaque *)
		MAXALIGN((char *) result + SizeofBrinOpcInfo(1));
	result->oi_typcache[0] = lookup_type_cache(PG_BRIN_MINMAX_MULTI_SUMMARYOID, 0);

	PG_RETURN_POINTER(result);
}

/*
 * 计算两个 float4 值之间的距离（直接相减）。
 */
Datum
brin_minmax_multi_distance_float4(PG_FUNCTION_ARGS)
{
	float		a1 = PG_GETARG_FLOAT4(0);
	float		a2 = PG_GETARG_FLOAT4(1);

	/* 若两个值都是 NaN，则视为相同 */
	if (isnan(a1) && isnan(a2))
		PG_RETURN_FLOAT8(0.0);

	/* 若其中一个值是 NaN，则使用无限距离 */
	if (isnan(a1) || isnan(a2))
		PG_RETURN_FLOAT8(get_float8_infinity());

	/*
	 * 我们知道这些值是区间边界，但区间可能已折叠
	 * （即单点），两端的值相等。
	 */
	Assert(a1 <= a2);

	PG_RETURN_FLOAT8((double) a2 - (double) a1);
}

/*
 * 计算两个 float8 值之间的距离（直接相减）。
 */
Datum
brin_minmax_multi_distance_float8(PG_FUNCTION_ARGS)
{
	double		a1 = PG_GETARG_FLOAT8(0);
	double		a2 = PG_GETARG_FLOAT8(1);

	/* 若两个值都是 NaN，则视为相同 */
	if (isnan(a1) && isnan(a2))
		PG_RETURN_FLOAT8(0.0);

	/* 若其中一个值是 NaN，则使用无限距离 */
	if (isnan(a1) || isnan(a2))
		PG_RETURN_FLOAT8(get_float8_infinity());

	/*
	 * 我们知道这些值是区间边界，但区间可能已折叠
	 * （即单点），两端的值相等。
	 */
	Assert(a1 <= a2);

	PG_RETURN_FLOAT8(a2 - a1);
}

/*
 * 计算两个 int2 值之间的距离（直接相减）。
 */
Datum
brin_minmax_multi_distance_int2(PG_FUNCTION_ARGS)
{
	int16		a1 = PG_GETARG_INT16(0);
	int16		a2 = PG_GETARG_INT16(1);

	/*
	 * 我们知道这些值是区间边界，但区间可能已折叠
	 * （即单点），两端的值相等。
	 */
	Assert(a1 <= a2);

	PG_RETURN_FLOAT8((double) a2 - (double) a1);
}

/*
 * 计算两个 int4 值之间的距离（直接相减）。
 */
Datum
brin_minmax_multi_distance_int4(PG_FUNCTION_ARGS)
{
	int32		a1 = PG_GETARG_INT32(0);
	int32		a2 = PG_GETARG_INT32(1);

	/*
	 * 我们知道这些值是区间边界，但区间可能已折叠
	 * （即单点），两端的值相等。
	 */
	Assert(a1 <= a2);

	PG_RETURN_FLOAT8((double) a2 - (double) a1);
}

/*
 * 计算两个 int8 值之间的距离（直接相减）。
 */
Datum
brin_minmax_multi_distance_int8(PG_FUNCTION_ARGS)
{
	int64		a1 = PG_GETARG_INT64(0);
	int64		a2 = PG_GETARG_INT64(1);

	/*
	 * 我们知道这些值是区间边界，但区间可能已折叠
	 * （即单点），两端的值相等。
	 */
	Assert(a1 <= a2);

	PG_RETURN_FLOAT8((double) a2 - (double) a1);
}

/*
 * 计算两个 tid 值之间的距离（先映射成 float8，
 * 再相减）。
 */
Datum
brin_minmax_multi_distance_tid(PG_FUNCTION_ARGS)
{
	double		da1,
				da2;

	ItemPointer pa1 = (ItemPointer) PG_GETARG_DATUM(0);
	ItemPointer pa2 = (ItemPointer) PG_GETARG_DATUM(1);

	/*
	 * 我们知道这些值是区间边界，但区间可能已折叠
	 * （即单点），两端的值相等。
	 */
	Assert(ItemPointerCompare(pa1, pa2) <= 0);

	/*
	 * 这里使用不做检查的变体，因为用户提供的值可能
	 * 有 (ip_posid == 0)。见 ItemPointerCompare。
	 */
	da1 = ItemPointerGetBlockNumberNoCheck(pa1) * MaxHeapTuplesPerPage +
		ItemPointerGetOffsetNumberNoCheck(pa1);

	da2 = ItemPointerGetBlockNumberNoCheck(pa2) * MaxHeapTuplesPerPage +
		ItemPointerGetOffsetNumberNoCheck(pa2);

	PG_RETURN_FLOAT8(da2 - da1);
}

/*
 * 计算两个 numeric 值之间的距离（直接相减）。
 */
Datum
brin_minmax_multi_distance_numeric(PG_FUNCTION_ARGS)
{
	Datum		d;
	Datum		a1 = PG_GETARG_DATUM(0);
	Datum		a2 = PG_GETARG_DATUM(1);

	/*
	 * 我们知道这些值是区间边界，但区间可能已折叠
	 * （即单点），两端的值相等。
	 */
	Assert(DatumGetBool(DirectFunctionCall2(numeric_le, a1, a2)));

	d = DirectFunctionCall2(numeric_sub, a2, a1);	/* a2 减 a1 */

	PG_RETURN_DATUM(DirectFunctionCall1(numeric_float8, d));
}

/*
 * 计算两个 UUID 值之间的近似距离。
 *
 * XXX 我们不需要完全精确的值，因此用 64 位浮点来近似
 * 差值（否则得用 128 位整数）。
 * 实践中这点误差无关紧要，最坏情况下
 * 我们会决定合并并非最接近的区间。
 */
Datum
brin_minmax_multi_distance_uuid(PG_FUNCTION_ARGS)
{
	int			i;
	float8		delta = 0;

	Datum		a1 = PG_GETARG_DATUM(0);
	Datum		a2 = PG_GETARG_DATUM(1);

	pg_uuid_t  *u1 = DatumGetUUIDP(a1);
	pg_uuid_t  *u2 = DatumGetUUIDP(a2);

	/*
	 * 我们知道这些值是区间边界，但区间可能已折叠
	 * （即单点），两端的值相等。
	 */
	Assert(DatumGetBool(DirectFunctionCall2(uuid_le, a1, a2)));

	/* 把近似差值算成双精度值 */
	for (i = UUID_LEN - 1; i >= 0; i--)
	{
		delta += (int) u2->data[i] - (int) u1->data[i];
		delta /= 256;
	}

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * 计算两个 date 之间的近似距离。
 */
Datum
brin_minmax_multi_distance_date(PG_FUNCTION_ARGS)
{
	float8		delta = 0;
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	delta = (float8) dateVal2 - (float8) dateVal1;

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * 计算两个 time（不带时区）值之间的近似距离。
 *
 * TimeADT 就是 int64，因此直接把两个值相减。
 */
Datum
brin_minmax_multi_distance_time(PG_FUNCTION_ARGS)
{
	float8		delta = 0;

	TimeADT		ta = PG_GETARG_TIMEADT(0);
	TimeADT		tb = PG_GETARG_TIMEADT(1);

	delta = (tb - ta);

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * 计算两个 timetz 值之间的近似距离。
 *
 * 直接减去嵌在 TimeTzADT 里的 TimeADT（int64）值。
 */
Datum
brin_minmax_multi_distance_timetz(PG_FUNCTION_ARGS)
{
	float8		delta = 0;

	TimeTzADT  *ta = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *tb = PG_GETARG_TIMETZADT_P(1);

	delta = (tb->time - ta->time) + (tb->zone - ta->zone) * USECS_PER_SEC;

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * 计算两个 timestamp 值之间的距离。
 */
Datum
brin_minmax_multi_distance_timestamp(PG_FUNCTION_ARGS)
{
	float8		delta = 0;

	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	delta = (float8) dt2 - (float8) dt1;

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * 计算两个 interval 值之间的距离。
 */
Datum
brin_minmax_multi_distance_interval(PG_FUNCTION_ARGS)
{
	float8		delta = 0;

	Interval   *ia = PG_GETARG_INTERVAL_P(0);
	Interval   *ib = PG_GETARG_INTERVAL_P(1);

	int64		dayfraction;
	int64		days;

	/*
	 * 差值是两个 interval 之间的（小数）天数。为与
	 * interval_cmp_internal 一致，假定每月 30 天。我们
	 * 不必精确，最坏情况下只会构建稍差一些的
	 * 区间。但不应当与 interval_cmp 矛盾。
	 */
	dayfraction = (ib->time % USECS_PER_DAY) - (ia->time % USECS_PER_DAY);
	days = (ib->time / USECS_PER_DAY) - (ia->time / USECS_PER_DAY);
	days += (int64) ib->day - (int64) ia->day;
	days += ((int64) ib->month - (int64) ia->month) * INT64CONST(30);

	/* 转换成双精度 */
	delta = (double) days + dayfraction / (double) USECS_PER_DAY;

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * 计算两个 pg_lsn 值之间的距离。
 *
 * LSN 只是编码流中位置的 int64，因此直接把
 * 这些 int64 值相减。
 */
Datum
brin_minmax_multi_distance_pg_lsn(PG_FUNCTION_ARGS)
{
	float8		delta = 0;

	XLogRecPtr	lsna = PG_GETARG_LSN(0);
	XLogRecPtr	lsnb = PG_GETARG_LSN(1);

	delta = (lsnb - lsna);

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * 计算两个 macaddr 值之间的距离。
 *
 * MAC 地址按 6 个无符号字符处理，因此做法与
 * 已经用于 UUID 值的相同。
 */
Datum
brin_minmax_multi_distance_macaddr(PG_FUNCTION_ARGS)
{
	float8		delta;

	macaddr    *a = PG_GETARG_MACADDR_P(0);
	macaddr    *b = PG_GETARG_MACADDR_P(1);

	delta = ((float8) b->f - (float8) a->f);
	delta /= 256;

	delta += ((float8) b->e - (float8) a->e);
	delta /= 256;

	delta += ((float8) b->d - (float8) a->d);
	delta /= 256;

	delta += ((float8) b->c - (float8) a->c);
	delta /= 256;

	delta += ((float8) b->b - (float8) a->b);
	delta /= 256;

	delta += ((float8) b->a - (float8) a->a);
	delta /= 256;

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * 计算两个 macaddr8 值之间的距离。
 *
 * macaddr8 地址是 8 个无符号字符，因此做法与
 * 已经用于 UUID 值的相同。
 */
Datum
brin_minmax_multi_distance_macaddr8(PG_FUNCTION_ARGS)
{
	float8		delta;

	macaddr8   *a = PG_GETARG_MACADDR8_P(0);
	macaddr8   *b = PG_GETARG_MACADDR8_P(1);

	delta = ((float8) b->h - (float8) a->h);
	delta /= 256;

	delta += ((float8) b->g - (float8) a->g);
	delta /= 256;

	delta += ((float8) b->f - (float8) a->f);
	delta /= 256;

	delta += ((float8) b->e - (float8) a->e);
	delta /= 256;

	delta += ((float8) b->d - (float8) a->d);
	delta /= 256;

	delta += ((float8) b->c - (float8) a->c);
	delta /= 256;

	delta += ((float8) b->b - (float8) a->b);
	delta /= 256;

	delta += ((float8) b->a - (float8) a->a);
	delta /= 256;

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * 计算两个 inet 值之间的距离。
 *
 * 距离定义为 32 位/128 位值之差，
 * 取决于 IP 版本。距离的算法是把
 * 字节相减，并对每个 IP 族归一化到 [0,1]。
 * 不同族的地址视为处于最大
 * 距离，即 1.0。
 *
 * XXX 是否需要考虑掩码（bits）？目前先忽略。
 */
Datum
brin_minmax_multi_distance_inet(PG_FUNCTION_ARGS)
{
	float8		delta;
	int			i;
	int			len;
	unsigned char *addra,
			   *addrb;

	inet	   *ipa = PG_GETARG_INET_PP(0);
	inet	   *ipb = PG_GETARG_INET_PP(1);

	int			lena,
				lenb;

	/*
	 * 若地址来自不同的族，则视为处于
	 * 可能的最大距离（即 1.0）。
	 */
	if (ip_family(ipa) != ip_family(ipb))
		PG_RETURN_FLOAT8(1.0);

	addra = (unsigned char *) palloc(ip_addrsize(ipa));
	memcpy(addra, ip_addr(ipa), ip_addrsize(ipa));

	addrb = (unsigned char *) palloc(ip_addrsize(ipb));
	memcpy(addrb, ip_addr(ipb), ip_addrsize(ipb));

	/*
	 * 长度按掩码长度计算，因为我们按范围中的第一个地址
	 * 排序，因此 A.B.C.D/24 < A.B.C.1（
	 * 前一个范围从 A.B.C.0 开始，它在 A.B.C.1 之前）。这种情况下我们不想
	 * 产生负的差值，因此直接截掉多余的
	 * 字节。
	 *
	 * XXX 也许应当更仔细一些，截掉的是位，而不只是
	 * 整个字节。
	 */
	lena = ip_bits(ipa);
	lenb = ip_bits(ipb);

	len = ip_addrsize(ipa);

	/* 对两个地址都施加网络掩码 */
	for (i = 0; i < len; i++)
	{
		unsigned char mask;
		int			nbits;

		nbits = Max(0, lena - (i * 8));
		if (nbits < 8)
		{
			mask = (0xFF << (8 - nbits));
			addra[i] = (addra[i] & mask);
		}

		nbits = Max(0, lenb - (i * 8));
		if (nbits < 8)
		{
			mask = (0xFF << (8 - nbits));
			addrb[i] = (addrb[i] & mask);
		}
	}

	/* 计算两个地址之差。 */
	delta = 0;
	for (i = len - 1; i >= 0; i--)
	{
		unsigned char a = addra[i];
		unsigned char b = addrb[i];

		delta += (float8) b - (float8) a;
		delta /= 256;
	}

	Assert((delta >= 0) && (delta <= 1));

	pfree(addra);
	pfree(addrb);

	PG_RETURN_FLOAT8(delta);
}

static void
brin_minmax_multi_serialize(BrinDesc *bdesc, Datum src, Datum *dst)
{
	Ranges	   *ranges = (Ranges *) DatumGetPointer(src);
	SerializedRanges *s;

	/*
	 * 在批模式下，需要把累积的值压缩到
	 * 实际请求的值/区间个数。
	 */
	compactify_ranges(bdesc, ranges, ranges->target_maxvalues);

	/* 此时一切都必须已完全排序。 */
	Assert(ranges->nsorted == ranges->nvalues);

	s = brin_range_serialize(ranges);
	dst[0] = PointerGetDatum(s);
}

static int
brin_minmax_multi_get_values(BrinDesc *bdesc, MinMaxMultiOptions *opts)
{
	return MinMaxMultiGetValuesPerRange(opts);
}

/*
 * 检查给定索引元组（它含有某个
 * 页范围的部分状态）：把它与来自另一个堆元组的给定值相比较。
 * 若新值超出现有元组值所指定的最小/最大范围，
 * 则更新该索引元组并返回
 * true。否则在这种情况下返回 false，且不做修改。
 */
Datum
brin_minmax_multi_add_value(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	Datum		newval = PG_GETARG_DATUM(2);
	bool		isnull PG_USED_FOR_ASSERTS_ONLY = PG_GETARG_DATUM(3);
	MinMaxMultiOptions *opts = (MinMaxMultiOptions *) PG_GET_OPCLASS_OPTIONS();
	Oid			colloid = PG_GET_COLLATION();
	bool		modified = false;
	Form_pg_attribute attr;
	AttrNumber	attno;
	Ranges	   *ranges;
	SerializedRanges *serialized = NULL;

	Assert(!isnull);

	attno = column->bv_attno;
	attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);

	/* 若可能，使用已经反序列化的值 */
	ranges = (Ranges *) DatumGetPointer(column->bv_mem_value);

	/*
	 * 若这是第一个非空值，需要初始化区间
	 * 列表。否则，只从 BrinValues 中取出已有的区间列表。
	 *
	 * 从空区间开始时，我们假定这是批模式，并
	 * 使用更大的缓冲区。缓冲区大小由 BRIN 范围
	 * 大小、每页行数推导，并带一些合理的最小/最大值。
	 * 缓冲区太小对性能不好，但太大则可能
	 * 需要很多内存（因为要保留全部值）。
	 */
	if (column->bv_allnulls)
	{
		MemoryContext oldctx;

		int			target_maxvalues;
		int			maxvalues;
		BlockNumber pagesPerRange = BrinGetPagesPerRange(bdesc->bd_index);

		/* 关系选项里指定的是什么？ */
		target_maxvalues = brin_minmax_multi_get_values(bdesc, opts);

		/*
		 * 确定插入缓冲区大小：取目标值的 10 倍，并封顶为
		 * 堆页范围中的最大取值个数。这已经绰绰有余，
		 * 因为每页的实际行数很可能
		 * 低得多，不过也无妨。
		 */
		maxvalues = Min(target_maxvalues * MINMAX_BUFFER_FACTOR,
						MaxHeapTuplesPerPage * pagesPerRange);

		/* 但始终至少保留原来的值 */
		maxvalues = Max(maxvalues, target_maxvalues);

		/* 始终用 MIN/MAX 封顶 */
		maxvalues = Max(maxvalues, MINMAX_BUFFER_MIN);
		maxvalues = Min(maxvalues, MINMAX_BUFFER_MAX);

		oldctx = MemoryContextSwitchTo(column->bv_context);
		ranges = minmax_multi_init(maxvalues);
		ranges->attno = attno;
		ranges->colloid = colloid;
		ranges->typid = attr->atttypid;
		ranges->target_maxvalues = target_maxvalues;

		/* 肯定需要比较函数，现在就查出来 */
		ranges->cmp = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
														 BTLessStrategyNumber);

		MemoryContextSwitchTo(oldctx);

		column->bv_allnulls = false;
		modified = true;

		column->bv_mem_value = PointerGetDatum(ranges);
		column->bv_serialize = brin_minmax_multi_serialize;
	}
	else if (!ranges)
	{
		MemoryContext oldctx;

		int			maxvalues;
		BlockNumber pagesPerRange = BrinGetPagesPerRange(bdesc->bd_index);

		oldctx = MemoryContextSwitchTo(column->bv_context);

		serialized = (SerializedRanges *) PG_DETOAST_DATUM(column->bv_values[0]);

		/*
		 * 确定插入缓冲区大小：取目标值的 10 倍，并封顶为
		 * 堆页范围中的最大取值个数。这已经绰绰有余，
		 * 因为每页的实际行数很可能
		 * 低得多，不过也无妨。
		 */
		maxvalues = Min(serialized->maxvalues * MINMAX_BUFFER_FACTOR,
						MaxHeapTuplesPerPage * pagesPerRange);

		/* 但始终至少保留原来的值 */
		maxvalues = Max(maxvalues, serialized->maxvalues);

		/* 始终用 MIN/MAX 封顶 */
		maxvalues = Max(maxvalues, MINMAX_BUFFER_MIN);
		maxvalues = Min(maxvalues, MINMAX_BUFFER_MAX);

		ranges = brin_range_deserialize(maxvalues, serialized);

		ranges->attno = attno;
		ranges->colloid = colloid;
		ranges->typid = attr->atttypid;

		/* 肯定需要比较函数，现在就查出来 */
		ranges->cmp = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
														 BTLessStrategyNumber);

		column->bv_mem_value = PointerGetDatum(ranges);
		column->bv_serialize = brin_minmax_multi_serialize;

		MemoryContextSwitchTo(oldctx);
	}

	/*
	 * 尝试把新值加入区间。需要更新 modified
	 * 标志，以便稍后序列化更新后的汇总。
	 */
	modified |= range_add_value(bdesc, colloid, attno, attr, ranges, newval);


	PG_RETURN_BOOL(modified);
}

/*
 * 给定对应于某个页范围的索引元组和一个扫描键，
 * 返回该扫描键是否与索引元组的最小/最大值一致。
 * 若一致则返回 true，否则返回 false。
 */
Datum
brin_minmax_multi_consistent(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	ScanKey    *keys = (ScanKey *) PG_GETARG_POINTER(2);
	int			nkeys = PG_GETARG_INT32(3);

	Oid			colloid = PG_GET_COLLATION(),
				subtype;
	AttrNumber	attno;
	Datum		value;
	FmgrInfo   *finfo;
	SerializedRanges *serialized;
	Ranges	   *ranges;
	int			keyno;
	int			rangeno;
	int			i;

	attno = column->bv_attno;

	serialized = (SerializedRanges *) PG_DETOAST_DATUM(column->bv_values[0]);
	ranges = brin_range_deserialize(serialized->maxvalues, serialized);

	/* 检查各区间，并对每个区间评估扫描键 */
	for (rangeno = 0; rangeno < ranges->nranges; rangeno++)
	{
		Datum		minval = ranges->values[2 * rangeno];
		Datum		maxval = ranges->values[2 * rangeno + 1];

		/* 假定该区间匹配，再设法证明并非如此 */
		bool		matching = true;

		for (keyno = 0; keyno < nkeys; keyno++)
		{
			Datum		matches;
			ScanKey		key = keys[keyno];

			/* NULL 键在 bringetbitmap 中处理并过滤掉 */
			Assert(!(key->sk_flags & SK_ISNULL));

			attno = key->sk_attno;
			subtype = key->sk_subtype;
			value = key->sk_argument;
			switch (key->sk_strategy)
			{
				case BTLessStrategyNumber:
				case BTLessEqualStrategyNumber:
					finfo = minmax_multi_get_strategy_procinfo(bdesc, attno, subtype,
															   key->sk_strategy);
					/* 数组中的第一个值 */
					matches = FunctionCall2Coll(finfo, colloid, minval, value);
					break;

				case BTEqualStrategyNumber:
					{
						Datum		compar;
						FmgrInfo   *cmpFn;

						/* 默认该区间不匹配 */
						matches = false;

						/*
						 * 否则，需要把新值与
						 * 所有区间的边界比较。先检查它是否
						 * 小于绝对最小值，也就是
						 * 数组中的第一个值。
						 */
						cmpFn = minmax_multi_get_strategy_procinfo(bdesc, attno, subtype,
																   BTGreaterStrategyNumber);
						compar = FunctionCall2Coll(cmpFn, colloid, minval, value);

						/* 小于本区间中的最小值 */
						if (DatumGetBool(compar))
							break;

						cmpFn = minmax_multi_get_strategy_procinfo(bdesc, attno, subtype,
																   BTLessStrategyNumber);
						compar = FunctionCall2Coll(cmpFn, colloid, maxval, value);

						/* 大于本区间中的最大值 */
						if (DatumGetBool(compar))
							break;

						/*
						 * 没能排除这个区间，因此
						 * 把它视为匹配。
						 */
						matches = true;

						break;
					}
				case BTGreaterEqualStrategyNumber:
				case BTGreaterStrategyNumber:
					finfo = minmax_multi_get_strategy_procinfo(bdesc, attno, subtype,
															   key->sk_strategy);
					/* 数组中的最后一个值 */
					matches = FunctionCall2Coll(finfo, colloid, maxval, value);
					break;

				default:
					/* 不应发生 */
					elog(ERROR, "invalid strategy number %d", key->sk_strategy);
					matches = 0;
					break;
			}

			/* 该区间必须匹配全部扫描键 */
			matching &= DatumGetBool(matches);

			/* 一旦发现不匹配的键，就此结束 */
			if (!matching)
				break;
		}

		/*
		 * 是否找到了匹配全部扫描键的区间？若是，则结束
		 */
		if (matching)
			PG_RETURN_DATUM(BoolGetDatum(true));
	}

	/*
	 * 现在检查这些值。这里不费心做二分查找，
	 * 因为处理的是已序列化 / 完全压实的区间，
	 * 所以值应当很少。
	 */
	for (i = 0; i < ranges->nvalues; i++)
	{
		Datum		val = ranges->values[2 * ranges->nranges + i];

		/* 假定该区间匹配，再设法证明并非如此 */
		bool		matching = true;

		for (keyno = 0; keyno < nkeys; keyno++)
		{
			Datum		matches;
			ScanKey		key = keys[keyno];

			/* 开头已经处理过 NULL 键 */
			if (key->sk_flags & SK_ISNULL)
				continue;

			attno = key->sk_attno;
			subtype = key->sk_subtype;
			value = key->sk_argument;
			switch (key->sk_strategy)
			{
				case BTLessStrategyNumber:
				case BTLessEqualStrategyNumber:
				case BTEqualStrategyNumber:
				case BTGreaterEqualStrategyNumber:
				case BTGreaterStrategyNumber:

					finfo = minmax_multi_get_strategy_procinfo(bdesc, attno, subtype,
															   key->sk_strategy);
					matches = FunctionCall2Coll(finfo, colloid, val, value);
					break;

				default:
					/* 不应发生 */
					elog(ERROR, "invalid strategy number %d", key->sk_strategy);
					matches = 0;
					break;
			}

			/* 该区间必须匹配全部扫描键 */
			matching &= DatumGetBool(matches);

			/* 一旦发现不匹配的键，就此结束 */
			if (!matching)
				break;
		}

		/* 是否找到了匹配全部扫描键的区间？若是，则结束 */
		if (matching)
			PG_RETURN_DATUM(BoolGetDatum(true));
	}

	PG_RETURN_DATUM(BoolGetDatum(false));
}

/*
 * 给定两个 BrinValues，把第一个更新为二者所含汇总
 * 值的并。第二个保持不动。
 */
Datum
brin_minmax_multi_union(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *col_a = (BrinValues *) PG_GETARG_POINTER(1);
	BrinValues *col_b = (BrinValues *) PG_GETARG_POINTER(2);

	Oid			colloid = PG_GET_COLLATION();
	SerializedRanges *serialized_a;
	SerializedRanges *serialized_b;
	Ranges	   *ranges_a;
	Ranges	   *ranges_b;
	AttrNumber	attno;
	Form_pg_attribute attr;
	ExpandedRange *eranges;
	int			neranges;
	FmgrInfo   *cmpFn,
			   *distanceFn;
	DistanceValue *distances;
	MemoryContext ctx;
	MemoryContext oldctx;

	Assert(col_a->bv_attno == col_b->bv_attno);
	Assert(!col_a->bv_allnulls && !col_b->bv_allnulls);

	attno = col_a->bv_attno;
	attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);

	serialized_a = (SerializedRanges *) PG_DETOAST_DATUM(col_a->bv_values[0]);
	serialized_b = (SerializedRanges *) PG_DETOAST_DATUM(col_b->bv_values[0]);

	ranges_a = brin_range_deserialize(serialized_a->maxvalues, serialized_a);
	ranges_b = brin_range_deserialize(serialized_b->maxvalues, serialized_b);

	/* 确保两个区间都不是 NULL */
	Assert(ranges_a && ranges_b);

	neranges = (ranges_a->nranges + ranges_a->nvalues) +
		(ranges_b->nranges + ranges_b->nvalues);

	/*
	 * distanceFn 调用（内部可能调用例如 numeric_le）可能
	 * 分配不少内存，绝不能泄漏。否则，
	 * 在例如构建索引时会出问题。因此我们创建局部
	 * 内存上下文，并确保在离开本函数之前释放内存
	 * （而不是每次调用之后）。
	 */
	ctx = AllocSetContextCreate(CurrentMemoryContext,
								"minmax-multi context",
								ALLOCSET_DEFAULT_SIZES);

	oldctx = MemoryContextSwitchTo(ctx);

	/* 分配并填充 */
	eranges = (ExpandedRange *) palloc0(neranges * sizeof(ExpandedRange));

	/* 用第一个区间的条目填充展开区间 */
	fill_expanded_ranges(eranges, ranges_a->nranges + ranges_a->nvalues,
						 ranges_a);

	/* 现在为第二个区间加入组合区间 */
	fill_expanded_ranges(&eranges[ranges_a->nranges + ranges_a->nvalues],
						 ranges_b->nranges + ranges_b->nvalues,
						 ranges_b);

	cmpFn = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
											   BTLessStrategyNumber);

	/* 对展开区间排序 */
	neranges = sort_expanded_ranges(cmpFn, colloid, eranges, neranges);

	/*
	 * 我们装入了两份不同的展开区间列表，因此其中有些
	 * 可能重叠。遍历它们并合并。
	 */
	neranges = merge_overlapping_ranges(cmpFn, colloid, eranges, neranges);

	/* 检查组合区间是否正确（无重叠、有序） */
	AssertCheckExpandedRanges(bdesc, colloid, attno, attr, eranges, neranges);

	/*
	 * 若需要，减少一些区间。
	 *
	 * XXX 这可能相当昂贵，所以也许只应在
	 * 真正需要时才做（区间过多时）。
	 */

	/* 构造间隙距离数组，并按升序排序 */
	distanceFn = minmax_multi_get_procinfo(bdesc, attno, PROCNUM_DISTANCE);
	distances = build_distances(distanceFn, colloid, eranges, neranges);

	/*
	 * 看看存储当前区间需要多少个值，若
	 * 需要则组合足够多的区间，使个数降到阈值以下。
	 * 折叠区间将存成单个值。
	 *
	 * XXX 这里不应用装载因子，因为我们不预期再向
	 * 区间加入更多值，因此倾向于尽可能多地保留区间。
	 *
	 * XXX 两个区间的 maxvalues 会不会不同？也许应当
	 * 取二者中较大的那个？
	 */
	neranges = reduce_expanded_ranges(eranges, neranges, distances,
									  ranges_a->maxvalues,
									  cmpFn, colloid);

	/* 更新第一个区间汇总 */
	store_expanded_ranges(ranges_a, eranges, neranges);

	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(ctx);

	/* 清理并更新序列化后的值 */
	pfree(serialized_a);
	col_a->bv_values[0] = PointerGetDatum(brin_range_serialize(ranges_a));

	PG_RETURN_VOID();
}

/*
 * 缓存并返回 minmax multi 操作符类的支持过程
 *
 * 返回与给定函数支持号对应的过程，
 * 若不存在则返回 null。
 */
static FmgrInfo *
minmax_multi_get_procinfo(BrinDesc *bdesc, uint16 attno, uint16 procnum)
{
	MinmaxMultiOpaque *opaque;
	uint16		basenum = procnum - PROCNUM_BASE;

	/*
	 * 把它们缓存在 opaque 结构里，以避免反复的 syscache
	 * 查找。
	 */
	opaque = (MinmaxMultiOpaque *) bdesc->bd_info[attno - 1]->oi_opaque;

	if (opaque->extra_procinfos[basenum].fn_oid == InvalidOid)
	{
		if (RegProcedureIsValid(index_getprocid(bdesc->bd_index, attno,
												procnum)))
			fmgr_info_copy(&opaque->extra_procinfos[basenum],
						   index_getprocinfo(bdesc->bd_index, attno, procnum),
						   bdesc->bd_context);
		else
			ereport(ERROR,
					errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					errmsg_internal("invalid opclass definition"),
					errdetail_internal("The operator class is missing support function %d for column %d.",
									   procnum, attno));
	}

	return &opaque->extra_procinfos[basenum];
}

/*
 * 缓存并返回给定策略对应的过程。
 *
 * 注意：本函数与 minmax_multi_get_strategy_procinfo 结构相同；参见
 * 那里的注释。若此处有改动，也要看那个函数。
 */
static FmgrInfo *
minmax_multi_get_strategy_procinfo(BrinDesc *bdesc, uint16 attno, Oid subtype,
								   uint16 strategynum)
{
	MinmaxMultiOpaque *opaque;

	Assert(strategynum >= 1 &&
		   strategynum <= BTMaxStrategyNumber);

	opaque = (MinmaxMultiOpaque *) bdesc->bd_info[attno - 1]->oi_opaque;

	/*
	 * 把上一子类型的过程缓存在 opaque 结构里，
	 * 以避免反复的 syscache 查找。若子类型变了，
	 * 则使全部缓存条目失效。
	 */
	if (opaque->cached_subtype != subtype)
	{
		uint16		i;

		for (i = 1; i <= BTMaxStrategyNumber; i++)
			opaque->strategy_procinfos[i - 1].fn_oid = InvalidOid;
		opaque->cached_subtype = subtype;
	}

	if (opaque->strategy_procinfos[strategynum - 1].fn_oid == InvalidOid)
	{
		Form_pg_attribute attr;
		HeapTuple	tuple;
		Oid			opfamily,
					oprid;

		opfamily = bdesc->bd_index->rd_opfamily[attno - 1];
		attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);
		tuple = SearchSysCache4(AMOPSTRATEGY, ObjectIdGetDatum(opfamily),
								ObjectIdGetDatum(attr->atttypid),
								ObjectIdGetDatum(subtype),
								Int16GetDatum(strategynum));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 strategynum, attr->atttypid, subtype, opfamily);

		oprid = DatumGetObjectId(SysCacheGetAttrNotNull(AMOPSTRATEGY, tuple,
														Anum_pg_amop_amopopr));
		ReleaseSysCache(tuple);
		Assert(RegProcedureIsValid(oprid));

		fmgr_info_cxt(get_opcode(oprid),
					  &opaque->strategy_procinfos[strategynum - 1],
					  bdesc->bd_context);
	}

	return &opaque->strategy_procinfos[strategynum - 1];
}

Datum
brin_minmax_multi_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(MinMaxMultiOptions));

	add_local_int_reloption(relopts, "values_per_range", "desc",
							MINMAX_MULTI_DEFAULT_VALUES_PER_PAGE, 8, 256,
							offsetof(MinMaxMultiOptions, valuesPerRange));

	PG_RETURN_VOID();
}

/*
 * brin_minmax_multi_summary_in
 *		- 类型 brin_minmax_multi_summary 的输入例程。
 *
 * brin_minmax_multi_summary 只在内部用来表示
 * BRIN minmax-multi 索引中的汇总，因此它自己没有操作，我们
 * 也不允许输入。
 */
Datum
brin_minmax_multi_summary_in(PG_FUNCTION_ARGS)
{
	/*
	 * brin_minmax_multi_summary 以二进制形式存储数据，不需要解析
	 * 文本输入，因此不允许这样做。
	 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "brin_minmax_multi_summary")));

	PG_RETURN_VOID();			/* 让编译器保持安静 */
}


/*
 * brin_minmax_multi_summary_out
 *		- 类型 brin_minmax_multi_summary 的输出例程。
 *
 * BRIN minmax-multi 汇总被序列化成 bytea 值，但我们
 * 希望输出人能看懂的更友好形式。
 */
Datum
brin_minmax_multi_summary_out(PG_FUNCTION_ARGS)
{
	int			i;
	int			idx;
	SerializedRanges *ranges;
	Ranges	   *ranges_deserialized;
	StringInfoData str;
	bool		isvarlena;
	Oid			outfunc;
	FmgrInfo	fmgrinfo;
	ArrayBuildState *astate_values = NULL;

	initStringInfo(&str);
	appendStringInfoChar(&str, '{');

	/*
	 * 解除 TOAST，得到带完整 4 字节头部的值（不能存进 TOAST
	 * 表，但可以使用 1 字节头部）。
	 */
	ranges = (SerializedRanges *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));

	/* 查找该类型的输出函数 */
	getTypeOutputInfo(ranges->typid, &outfunc, &isvarlena);
	fmgr_info(outfunc, &fmgrinfo);

	/* 把区间信息反序列化成易于处理的片段 */
	ranges_deserialized = brin_range_deserialize(ranges->maxvalues, ranges);

	appendStringInfo(&str, "nranges: %d  nvalues: %d  maxvalues: %d",
					 ranges_deserialized->nranges,
					 ranges_deserialized->nvalues,
					 ranges_deserialized->maxvalues);

	/* 序列化区间 */
	idx = 0;
	for (i = 0; i < ranges_deserialized->nranges; i++)
	{
		char	   *a,
				   *b;
		text	   *c;
		StringInfoData buf;

		initStringInfo(&buf);

		a = OutputFunctionCall(&fmgrinfo, ranges_deserialized->values[idx++]);
		b = OutputFunctionCall(&fmgrinfo, ranges_deserialized->values[idx++]);

		appendStringInfo(&buf, "%s ... %s", a, b);

		c = cstring_to_text_with_len(buf.data, buf.len);

		astate_values = accumArrayResult(astate_values,
										 PointerGetDatum(c),
										 false,
										 TEXTOID,
										 CurrentMemoryContext);
	}

	if (ranges_deserialized->nranges > 0)
	{
		Oid			typoutput;
		bool		typIsVarlena;
		Datum		val;
		char	   *extval;

		getTypeOutputInfo(ANYARRAYOID, &typoutput, &typIsVarlena);

		val = makeArrayResult(astate_values, CurrentMemoryContext);

		extval = OidOutputFunctionCall(typoutput, val);

		appendStringInfo(&str, " ranges: %s", extval);
	}

	/* 序列化各个值 */
	astate_values = NULL;

	for (i = 0; i < ranges_deserialized->nvalues; i++)
	{
		Datum		a;
		text	   *b;

		a = FunctionCall1(&fmgrinfo, ranges_deserialized->values[idx++]);
		b = cstring_to_text(DatumGetCString(a));

		astate_values = accumArrayResult(astate_values,
										 PointerGetDatum(b),
										 false,
										 TEXTOID,
										 CurrentMemoryContext);
	}

	if (ranges_deserialized->nvalues > 0)
	{
		Oid			typoutput;
		bool		typIsVarlena;
		Datum		val;
		char	   *extval;

		getTypeOutputInfo(ANYARRAYOID, &typoutput, &typIsVarlena);

		val = makeArrayResult(astate_values, CurrentMemoryContext);

		extval = OidOutputFunctionCall(typoutput, val);

		appendStringInfo(&str, " values: %s", extval);
	}


	appendStringInfoChar(&str, '}');

	PG_RETURN_CSTRING(str.data);
}

/*
 * brin_minmax_multi_summary_recv
 *		- 类型 brin_minmax_multi_summary 的二进制输入例程。
 */
Datum
brin_minmax_multi_summary_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "brin_minmax_multi_summary")));

	PG_RETURN_VOID();			/* 让编译器保持安静 */
}

/*
 * brin_minmax_multi_summary_send
 *		- 类型 brin_minmax_multi_summary 的二进制输出例程。
 *
 * BRIN minmax-multi 汇总序列化在一个 bytea 值里（尽管
 * 类型的名字不同），因此直接把它发送出去。
 */
Datum
brin_minmax_multi_summary_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}
