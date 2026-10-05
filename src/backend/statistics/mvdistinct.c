/*-------------------------------------------------------------------------
 *
 * mvdistinct.c
 *	  POSTGRES multivariate ndistinct coefficients
 *
 * Estimating number of groups in a combination of columns (e.g. for GROUP BY)
 * is tricky, and the estimation error is often significant.

 * The multivariate ndistinct coefficients address this by storing ndistinct
 * estimates for combinations of the user-specified columns.  So for example
 * given a statistics object on three columns (a,b,c), this module estimates
 * and stores n-distinct for (a,b), (a,c), (b,c) and (a,b,c).  The per-column
 * estimates are already available in pg_statistic.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/statistics/mvdistinct.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "lib/stringinfo.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

static double ndistinct_for_combination(double totalrows, StatsBuildData *data,
										int k, int *combination);
static double estimate_ndistinct(double totalrows, int numrows, int d, int f1);
static int	n_choose_k(int n, int k);
static int	num_combinations(int n);

/* 结构头成员的大小（magic、type、nitems） */
#define SizeOfHeader		(3 * sizeof(uint32))

/* 一条序列化 ndistinct 项的大小（系数、natts、atts） */
#define SizeOfItem(natts) \
	(sizeof(double) + sizeof(int) + (natts) * sizeof(AttrNumber))

/* 一条 ndistinct 项的最小大小（含两个属性） */
#define MinSizeOfItem	SizeOfItem(2)

/* 当所有项都取最小大小时，mvndistinct 的最小大小 */
#define MinSizeOfItems(nitems)	\
	(SizeOfHeader + (nitems) * MinSizeOfItem)

/* 组合生成器 API */

/* 从 n 个元素中生成 k 组合的内部状态 */
typedef struct CombinationGenerator
{
	int			k;				/* 组合的大小 */
	int			n;				/* 元素总数 */
	int			current;		/* 下一个要返回的组合的下标 */
	int			ncombinations;	/* 组合的个数（数组大小） */
	int		   *combinations;	/* 预先构造的组合数组 */
} CombinationGenerator;

static CombinationGenerator *generator_init(int n, int k);
static void generator_free(CombinationGenerator *state);
static int *generator_next(CombinationGenerator *state);
static void generate_combinations(CombinationGenerator *state);


/*
 * statext_ndistinct_build
 *		计算属性组合的 ndistinct 系数。
 *
 * 这使用与 analyze.c 相同的估计器计算 ndistinct 估计，
 * 然后计算该系数。
 *
 * 为了方便处理表达式，我们把它们当作带负 attnums 的
 * 系统属性，并把一切按表达式个数偏移，
 * 以便使用 Bitmapsets。
 */
MVNDistinct *
statext_ndistinct_build(double totalrows, StatsBuildData *data)
{
	MVNDistinct *result;
	int			k;
	int			itemcnt;
	int			numattrs = data->nattnums;
	int			numcombs = num_combinations(numattrs);

	result = palloc(offsetof(MVNDistinct, items) +
					numcombs * sizeof(MVNDistinctItem));
	result->magic = STATS_NDISTINCT_MAGIC;
	result->type = STATS_NDISTINCT_TYPE_BASIC;
	result->nitems = numcombs;

	itemcnt = 0;
	for (k = 2; k <= numattrs; k++)
	{
		int		   *combination;
		CombinationGenerator *generator;

		/* 生成从 N 个元素中取 K 个的组合 */
		generator = generator_init(numattrs, k);

		while ((combination = generator_next(generator)))
		{
			MVNDistinctItem *item = &result->items[itemcnt];
			int			j;

			item->attributes = palloc(sizeof(AttrNumber) * k);
			item->nattributes = k;

			/* 把下标翻译成 attnums */
			for (j = 0; j < k; j++)
			{
				item->attributes[j] = data->attnums[combination[j]];

				Assert(AttributeNumberIsValid(item->attributes[j]));
			}

			item->ndistinct =
				ndistinct_for_combination(totalrows, data, k, combination);

			itemcnt++;
			Assert(itemcnt <= result->nitems);
		}

		generator_free(generator);
	}

	/* 必须恰好消耗整个输出数组 */
	Assert(itemcnt == result->nitems);

	return result;
}

/*
 * statext_ndistinct_load
 *		装载所指 pg_statistic_ext 元组的 ndistinct 值。
 */
MVNDistinct *
statext_ndistinct_load(Oid mvoid, bool inh)
{
	MVNDistinct *result;
	bool		isnull;
	Datum		ndist;
	HeapTuple	htup;

	htup = SearchSysCache2(STATEXTDATASTXOID,
						   ObjectIdGetDatum(mvoid), BoolGetDatum(inh));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for statistics object %u", mvoid);

	ndist = SysCacheGetAttr(STATEXTDATASTXOID, htup,
							Anum_pg_statistic_ext_data_stxdndistinct, &isnull);
	if (isnull)
		elog(ERROR,
			 "requested statistics kind \"%c\" is not yet built for statistics object %u",
			 STATS_EXT_NDISTINCT, mvoid);

	result = statext_ndistinct_deserialize(DatumGetByteaPP(ndist));

	ReleaseSysCache(htup);

	return result;
}

/*
 * statext_ndistinct_serialize
 *		把 ndistinct 序列化成磁盘上的 bytea 格式。
 */
bytea *
statext_ndistinct_serialize(MVNDistinct *ndistinct)
{
	int			i;
	bytea	   *output;
	char	   *tmp;
	Size		len;

	Assert(ndistinct->magic == STATS_NDISTINCT_MAGIC);
	Assert(ndistinct->type == STATS_NDISTINCT_TYPE_BASIC);

	/*
	 * 基本大小是结构里标量成员的大小，再加上每个项的
	 * 一个基本结构，其中包括每一项的个数。
	 */
	len = VARHDRSZ + SizeOfHeader;

	/* 并且也计入实际属性号所需的空间 */
	for (i = 0; i < ndistinct->nitems; i++)
	{
		int			nmembers;

		nmembers = ndistinct->items[i].nattributes;
		Assert(nmembers >= 2);

		len += SizeOfItem(nmembers);
	}

	output = (bytea *) palloc(len);
	SET_VARSIZE(output, len);

	tmp = VARDATA(output);

	/* 存放基本结构的值（magic、type、nitems） */
	memcpy(tmp, &ndistinct->magic, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(tmp, &ndistinct->type, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(tmp, &ndistinct->nitems, sizeof(uint32));
	tmp += sizeof(uint32);

	/*
	 * 为每个条目存放属性个数和属性号
	 */
	for (i = 0; i < ndistinct->nitems; i++)
	{
		MVNDistinctItem item = ndistinct->items[i];
		int			nmembers = item.nattributes;

		memcpy(tmp, &item.ndistinct, sizeof(double));
		tmp += sizeof(double);
		memcpy(tmp, &nmembers, sizeof(int));
		tmp += sizeof(int);

		memcpy(tmp, item.attributes, sizeof(AttrNumber) * nmembers);
		tmp += nmembers * sizeof(AttrNumber);

		/* 防止溢出 */
		Assert(tmp <= ((char *) output + len));
	}

	/* 检查我们恰好用掉了预期的空间 */
	Assert(tmp == ((char *) output + len));

	return output;
}

/*
 * statext_ndistinct_deserialize
 *		把磁盘上的 bytea 格式 MVNDistinct 读入内存格式。
 */
MVNDistinct *
statext_ndistinct_deserialize(bytea *data)
{
	int			i;
	Size		minimum_size;
	MVNDistinct ndist;
	MVNDistinct *ndistinct;
	char	   *tmp;

	if (data == NULL)
		return NULL;

	/* 我们预期至少有 MVNDistinct 结构的基本成员 */
	if (VARSIZE_ANY_EXHDR(data) < SizeOfHeader)
		elog(ERROR, "invalid MVNDistinct size %zu (expected at least %zu)",
			 VARSIZE_ANY_EXHDR(data), SizeOfHeader);

	/* 把指针初始化到数据部分（跳过 varlena 头） */
	tmp = VARDATA_ANY(data);

	/* 读取头成员并做基本的健全性检查 */
	memcpy(&ndist.magic, tmp, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(&ndist.type, tmp, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(&ndist.nitems, tmp, sizeof(uint32));
	tmp += sizeof(uint32);

	if (ndist.magic != STATS_NDISTINCT_MAGIC)
		elog(ERROR, "invalid ndistinct magic %08x (expected %08x)",
			 ndist.magic, STATS_NDISTINCT_MAGIC);
	if (ndist.type != STATS_NDISTINCT_TYPE_BASIC)
		elog(ERROR, "invalid ndistinct type %d (expected %d)",
			 ndist.type, STATS_NDISTINCT_TYPE_BASIC);
	if (ndist.nitems == 0)
		elog(ERROR, "invalid zero-length item array in MVNDistinct");

	/* 这些参数下我们预期的最小 bytea 大小是多少 */
	minimum_size = MinSizeOfItems(ndist.nitems);
	if (VARSIZE_ANY_EXHDR(data) < minimum_size)
		elog(ERROR, "invalid MVNDistinct size %zu (expected at least %zu)",
			 VARSIZE_ANY_EXHDR(data), minimum_size);

	/*
	 * 为 ndistinct 项分配空间（不为每个项的 attnos
	 * 分配空间：那些住在单独分配的 bitmapset 里）。
	 */
	ndistinct = palloc0(MAXALIGN(offsetof(MVNDistinct, items)) +
						(ndist.nitems * sizeof(MVNDistinctItem)));
	ndistinct->magic = ndist.magic;
	ndistinct->type = ndist.type;
	ndistinct->nitems = ndist.nitems;

	for (i = 0; i < ndistinct->nitems; i++)
	{
		MVNDistinctItem *item = &ndistinct->items[i];

		/* ndistinct 值 */
		memcpy(&item->ndistinct, tmp, sizeof(double));
		tmp += sizeof(double);

		/* 属性个数 */
		memcpy(&item->nattributes, tmp, sizeof(int));
		tmp += sizeof(int);
		Assert((item->nattributes >= 2) && (item->nattributes <= STATS_MAX_DIMENSIONS));

		item->attributes
			= (AttrNumber *) palloc(item->nattributes * sizeof(AttrNumber));

		memcpy(item->attributes, tmp, sizeof(AttrNumber) * item->nattributes);
		tmp += sizeof(AttrNumber) * item->nattributes;

		/* 仍在 bytea 之内 */
		Assert(tmp <= ((char *) data + VARSIZE_ANY(data)));
	}

	/* 应当恰好消耗整个 bytea */
	Assert(tmp == ((char *) data + VARSIZE_ANY(data)));

	return ndistinct;
}

/*
 * pg_ndistinct_in
 *		类型 pg_ndistinct 的输入例程。
 *
 * pg_ndistinct 真实到足以作为表列，但它没有
 * 自己的操作，并且不允许输入（就像 pg_node_tree 一样）。
 */
Datum
pg_ndistinct_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_ndistinct")));

	PG_RETURN_VOID();			/* 消除编译器告警 */
}

/*
 * pg_ndistinct
 *		类型 pg_ndistinct 的输出例程。
 *
 * 产生该值的人类可读表示。
 */
Datum
pg_ndistinct_out(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	MVNDistinct *ndist = statext_ndistinct_deserialize(data);
	int			i;
	StringInfoData str;

	initStringInfo(&str);
	appendStringInfoChar(&str, '{');

	for (i = 0; i < ndist->nitems; i++)
	{
		int			j;
		MVNDistinctItem item = ndist->items[i];

		if (i > 0)
			appendStringInfoString(&str, ", ");

		for (j = 0; j < item.nattributes; j++)
		{
			AttrNumber	attnum = item.attributes[j];

			appendStringInfo(&str, "%s%d", (j == 0) ? "\"" : ", ", attnum);
		}
		appendStringInfo(&str, "\": %d", (int) item.ndistinct);
	}

	appendStringInfoChar(&str, '}');

	PG_RETURN_CSTRING(str.data);
}

/*
 * pg_ndistinct_recv
 *		类型 pg_ndistinct 的二进制输入例程。
 */
Datum
pg_ndistinct_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_ndistinct")));

	PG_RETURN_VOID();			/* 消除编译器告警 */
}

/*
 * pg_ndistinct_send
 *		类型 pg_ndistinct 的二进制输出例程。
 *
 * n-distinct 被序列化成 bytea 值，因此直接发送它即可。
 */
Datum
pg_ndistinct_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}

/*
 * ndistinct_for_combination
 *		估计一组列组合里的不同值个数。
 *
 * 这使用与 ANALYZE 里 compute_scalar_stats() 相同的
 * ndistinct 估计器，即：
 *		n*d / (n - f1 + f1*n/N)。
 *
 * 区别在于，我们处理的不是单列里的值，而是
 * 多列的组合。
 */
static double
ndistinct_for_combination(double totalrows, StatsBuildData *data,
						  int k, int *combination)
{
	int			i,
				j;
	int			f1,
				cnt,
				d;
	bool	   *isnull;
	Datum	   *values;
	SortItem   *items;
	MultiSortSupport mss;
	int			numrows = data->numrows;

	mss = multi_sort_init(k);

	/*
	 * 为了确定不同元素的个数，我们建立单独的
	 * values[]/isnull[] 数组，放入已有的全部数据，然后按
	 * 指定的列组合作为维度对它们排序。也可以尝试
	 * 就地排序，但那可能会更复杂、更容易出错。
	 */
	items = (SortItem *) palloc(numrows * sizeof(SortItem));
	values = (Datum *) palloc0(sizeof(Datum) * numrows * k);
	isnull = (bool *) palloc0(sizeof(bool) * numrows * k);

	for (i = 0; i < numrows; i++)
	{
		items[i].values = &values[i * k];
		items[i].isnull = &isnull[i * k];
	}

	/*
	 * 对每个维度，设置排序支持，并从样本数据填入
	 * 这些值。
	 *
	 * 我们使用列数据类型的默认排序操作符和排序规则；
	 * 也许将来值得使用列专属的排序规则？
	 */
	for (i = 0; i < k; i++)
	{
		Oid			typid;
		TypeCacheEntry *type;
		Oid			collid = InvalidOid;
		VacAttrStats *colstat = data->stats[combination[i]];

		typid = colstat->attrtypid;
		collid = colstat->attrcollid;

		type = lookup_type_cache(typid, TYPECACHE_LT_OPR);
		if (type->lt_opr == InvalidOid) /* 不应当发生 */
			elog(ERROR, "cache lookup failed for ordering operator for type %u",
				 typid);

		/* 为这个维度准备排序函数 */
		multi_sort_add_dimension(mss, i, type->lt_opr, collid);

		/* 把这个维度的全部数据累积进数组 */
		for (j = 0; j < numrows; j++)
		{
			items[j].values[i] = data->values[combination[i]][j];
			items[j].isnull[i] = data->nulls[combination[i]][j];
		}
	}

	/* 现在可以对数组排序…… */
	qsort_interruptible(items, numrows, sizeof(SortItem),
						multi_sort_compare, mss);

	/* ……并统计不同组合的个数 */

	f1 = 0;
	cnt = 1;
	d = 1;
	for (i = 1; i < numrows; i++)
	{
		if (multi_sort_compare(&items[i], &items[i - 1], mss) != 0)
		{
			if (cnt == 1)
				f1 += 1;

			d++;
			cnt = 0;
		}

		cnt += 1;
	}

	if (cnt == 1)
		f1 += 1;

	return estimate_ndistinct(totalrows, numrows, d, f1);
}

/* Duj1 估计器（analyze.c 里已经在用）。 */
static double
estimate_ndistinct(double totalrows, int numrows, int d, int f1)
{
	double		numer,
				denom,
				ndistinct;

	numer = (double) numrows * (double) d;

	denom = (double) (numrows - f1) +
		(double) f1 * (double) numrows / totalrows;

	ndistinct = numer / denom;

	/* 若有舍入误差，则钳制到合理范围 */
	if (ndistinct < (double) d)
		ndistinct = (double) d;

	if (ndistinct > totalrows)
		ndistinct = totalrows;

	return floor(ndistinct + 0.5);
}

/*
 * n_choose_k
 *		用一种既高效又能防止溢出的算法
 *		计算二项式系数。
 */
static int
n_choose_k(int n, int k)
{
	int			d,
				r;

	Assert((k > 0) && (n >= k));

	/* 利用二项式系数的对称性 */
	k = Min(k, n - k);

	r = 1;
	for (d = 1; d <= k; ++d)
	{
		r *= n--;
		r /= d;
	}

	return r;
}

/*
 * num_combinations
 *		组合的个数，不含单值组合。
 */
static int
num_combinations(int n)
{
	return (1 << n) - (n + 1);
}

/*
 * generator_init
 *		初始化组合生成器。
 *
 * 该生成器产生区间 (0..N) 内 K 个元素的组合。
 * 我们在这个方法里预先构造全部组合，这比
 * 即时生成它们更简单。
 */
static CombinationGenerator *
generator_init(int n, int k)
{
	CombinationGenerator *state;

	Assert((n >= k) && (k > 0));

	/* 把生成器状态作为一整块内存来分配 */
	state = (CombinationGenerator *) palloc(sizeof(CombinationGenerator));

	state->ncombinations = n_choose_k(n, k);

	/* 为全部组合预先分配空间 */
	state->combinations = (int *) palloc(sizeof(int) * k * state->ncombinations);

	state->current = 0;
	state->k = k;
	state->n = n;

	/* 现在实际预先生成 K 个元素的全部组合 */
	generate_combinations(state);

	/* 确保得到了预期个数的组合 */
	Assert(state->current == state->ncombinations);

	/* 把序号重置，以便从第一个开始 */
	state->current = 0;

	return state;
}

/*
 * generator_next
 *		返回预构造列表中的下一个组合。
 *
 * 返回 K 个数组下标的组合（0 .. N），如 generator_init
 * 所指定的那样；没有更多组合时返回 NULL。
 */
static int *
generator_next(CombinationGenerator *state)
{
	if (state->current == state->ncombinations)
		return NULL;

	return &state->combinations[state->k * state->current++];
}

/*
 * generator_free
 *		释放生成器的内部状态。
 *
 * 释放生成器的内部状态（预先构造的组合）。
 */
static void
generator_free(CombinationGenerator *state)
{
	pfree(state->combinations);
	pfree(state);
}

/*
 * generate_combinations_recurse
 *		给定前缀，生成所有可能的组合。
 *
 * 给定前缀（组合的前几个元素），递归生成后面的
 * 元素。我们按字典序生成这些组合，
 * 从而消除同一组合的排列。
 */
static void
generate_combinations_recurse(CombinationGenerator *state,
							  int index, int start, int *current)
{
	/* 若尚未填满全部元素，就直接递归。 */
	if (index < state->k)
	{
		int			i;

		/*
		 * 这些值必须按升序排列，因此确保我们从
		 * 参数传入的值开始。
		 */

		for (i = start; i < state->n; i++)
		{
			current[index] = i;
			generate_combinations_recurse(state, (index + 1), (i + 1), current);
		}

		return;
	}
	else
	{
		/* 得到一个有效组合，把它加入数组 */
		memcpy(&state->combinations[(state->k * state->current)],
			   current, state->k * sizeof(int));
		state->current++;
	}
}

/*
 * generate_combinations
 *		生成 N 个元素的全部 k 组合。
 */
static void
generate_combinations(CombinationGenerator *state)
{
	int		   *current = (int *) palloc0(sizeof(int) * state->k);

	generate_combinations_recurse(state, 0, 0, current);

	pfree(current);
}
