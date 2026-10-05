/*
 * brin_bloom.c
 *		BRIN 的 Bloom 操作符类实现
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * 把页范围汇总成布隆过滤器的 BRIN 操作符类。
 *
 * 布隆过滤器可以高效检测给定页范围是否包含
 * 某个特定值。因此，若把每个页范围汇总成一个小的
 * 布隆过滤器，就能轻松（且廉价地）检测它是否包含
 * 后来得到的值。
 *
 * 该索引只支持相等操作符，与哈希索引类似。
 * 不过布隆索引小得多，并且只支持位图扫描。
 *
 * 注意：不要把它与 contrib 模块里实现的 bloom 索引混淆。
 * 那个扩展实现的是全新的 AM，在单行的多列上构建布隆
 * 过滤器。本操作符类配合已有的
 * AM（BRIN），并在一列上构建布隆过滤器。
 *
 *
 * 值与哈希
 * -----------------
 *
 * 不直接使用原始列值，而是先做哈希，
 * 使用该类型常规的哈希函数，得到一个 uint32 哈希。
 * 然后把这个哈希值加入汇总，也就是再哈希一次，
 * 并加入布隆过滤器。
 *
 * 这样代码就能以同样方式处理所有数据类型（byval/byref/...），
 * 且空间需求很小，因为我们处理的是
 * 哈希而不是原始值。一切都是 uint32。
 *
 * 当然，这假定内建哈希函数足够好，
 * 碰撞不会过多，等等。就以往经验来看，
 * 情况似乎确实如此。毕竟同样的哈希函数也
 * 用于哈希索引、哈希分区等等。
 *
 *
 * 哈希方案
 * --------------
 *
 * 布隆过滤器需要若干相互独立的哈希函数。构造它们
 * 有不同方案，例如可以用
 * 带随机种子的 hash_uint32_extended，但那似乎相当昂贵。
 * 我们采用这篇论文所述、只需要两个函数的方案：
 *
 * Less Hashing, Same Performance:Building a Better Bloom Filter
 * Adam Kirsch, Michael Mitzenmacher, Harvard School of Engineering and
 * Applied Sciences, Cambridge, Massachusetts [DOI 10.1002/rsa.20208]
 *
 * 两个哈希函数 h1 和 h2 用硬编码种子计算，
 * 再用 (h1 + i * h2) 组合，生成各哈希函数。
 *
 *
 * 布隆过滤器的尺寸
 * -----------------------
 *
 * 布隆过滤器的大小取决于将要存入的不同值个数，
 * 以及期望的误报率。不同值个数越高，
 * 或误报率越低，布隆过滤器就
 * 越大。另一方面，我们希望索引尽可能小，
 * 这正是 BRIN 索引的基本优点之一。
 *
 * 尽管（一个页范围内的）不同元素个数取决于
 * 数据，我们可以把它视为固定。这就把权衡简化为
 * 仅仅是误报率与大小。
 *
 * 在页范围这一层，误报率是布隆
 * 过滤器命中一个随机值的概率。对整个索引（页范围
 * 足够多时），它表示与该随机值匹配的索引范围所占比例
 * （因而也是需要扫描的表所占的比例）。
 *
 * 此外，布隆过滤器的大小受实现
 * 限制：它必须放进单个索引页（默认 8kB）。由于
 * 位图本质上是随机的（"满" 时大约一半的位被
 * 随机置为 1），压缩起不了多大作用。
 *
 * 要缩小过滤器以便放进一页，要么
 * 接受更高的误报率（不可取），要么减少
 * 存入过滤器的不同项个数。我们当然无法改输入
 * 数据，但可以把 BRIN 页范围改小：不必用
 * 默认的 128 页（1MB），可以按 16 页的范围建索引，
 * 或类似做法。这应能减少页范围内
 * 不同值的个数，从而在误报率固定时
 * 让过滤器更小。即使对随机数据集也有帮助，因为每张堆页的
 * 行数有限（极窄的表约 290，实践中大约
 * 20）。
 *
 * 当然，好的尺寸决策取决于是否有必要的数据，
 * 即给定大小的页范围内的不同值个数，以及
 * 表的大小（用来估计误报率变化引起的代价变化，
 * 这种变化来自索引变大，相对于要扫描更大的索引）。我们可能
 * 没有这些数据，例如在空表上建索引时
 * 就做不到。对某些数据，我们只有整表的估计，
 * 只能估计每个范围的值（ndistinct）。
 *
 * 另一个困难是：布隆过滤器是按列的，但必须
 * 放进一页的是整个索引元组。对多列
 * 索引，其中可能有我们无法控制的部分（不一定是
 * 布隆过滤器，其他列可能用别的 BRIN 操作符类）。因此
 * 如何在这些列之间分配空间并不完全清楚。
 *
 * 当前逻辑在 brin_bloom_get_ndistinct 中实现，试图
 * 根据 BRIN 范围的大小，以及
 * 每个范围的最大行数，做一些基本的尺寸决策。
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_bloom.c
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/brin.h"
#include "access/brin_internal.h"
#include "access/brin_page.h"
#include "access/brin_tuple.h"
#include "access/hash.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "catalog/pg_amop.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include <math.h>

#define BloomEqualStrategyNumber	1

/*
 * 额外的 SQL 层支持函数。我们只有一个，用来
 * 计算输入值的哈希。
 *
 * 过程编号不得使用 BRIN 自身保留的值；参见
 * brin_internal.h。
 */
#define		BLOOM_MAX_PROCNUMS		1	/* 我们需要的支持过程上限 */
#define		PROCNUM_HASH			11	/* 必需 */

/*
 * 从 procnum 减去此值，得到 BloomOpaque 数组中的下标
 * （必须等于私有过程编号的最小值）。
 */
#define		PROCNUM_BASE			11

/*
 * BRIN reloptions 的存储类型。
 */
typedef struct BloomOptions
{
	int32		vl_len_;		/* varlena 头部（不要直接碰！） */
	double		nDistinctPerRange;	/* 每个范围的不同值个数 */
	double		falsePositiveRate;	/* 布隆过滤器的误报率 */
} BloomOptions;

/*
 * 当前最小值（16）有些随意，但依据是
 * 过滤器头部本身约 20B，大约
 * 相当于 16 个不同项、1% 误报
 * 率时的过滤器位图。因此允许更低的值收益不大。总之
 * 最小值不应大于 MaxHeapTuplesPerPage
 * （约 290），那是单页范围的理论上限。
 */
#define		BLOOM_MIN_NDISTINCT_PER_RANGE		16

/*
 * 用来按页范围内的行数确定不同项个数。
 * 这里的 10% 与 estimate_num_groups
 * 的做法有些相似，因此用同一个系数。
 */
#define		BLOOM_DEFAULT_NDISTINCT_PER_RANGE	-0.1	/* 值的 10% */

/*
 * 误报率的允许范围与默认值。具体
 * 数值有些随意，但是综合考虑各项
 * 参数（过滤器大小与页大小等）后选定的。
 *
 * 误报率越低，过滤器越准确，但
 * 它也会变大，到某一点就会抵消主要优点，
 * 即 BRIN 索引体积很小。在 0.01% 时索引大约是
 * 表的 10%（假定每 8kB 页有 290 个不同值）。
 *
 * 另一方面，误报率升高时，由于误报，表中更大的
 * 部分必须被扫描。到 25% 时，我们大概
 * 已经接近顺序扫描代价更低的程度。
 */
#define		BLOOM_MIN_FALSE_POSITIVE_RATE	0.0001	/* 0.01% 误报率 */
#define		BLOOM_MAX_FALSE_POSITIVE_RATE	0.25	/* 25% 误报率 */
#define		BLOOM_DEFAULT_FALSE_POSITIVE_RATE	0.01	/* 1% 误报率 */

#define BloomGetNDistinctPerRange(opts) \
	((opts) && (((BloomOptions *) (opts))->nDistinctPerRange != 0) ? \
	 (((BloomOptions *) (opts))->nDistinctPerRange) : \
	 BLOOM_DEFAULT_NDISTINCT_PER_RANGE)

#define BloomGetFalsePositiveRate(opts) \
	((opts) && (((BloomOptions *) (opts))->falsePositiveRate != 0.0) ? \
	 (((BloomOptions *) (opts))->falsePositiveRate) : \
	 BLOOM_DEFAULT_FALSE_POSITIVE_RATE)

/*
 * 能放进一页的最大布隆过滤器的估计。这并不是
 * 完美保证，原因有几个。例如，行可能
 * 因为索引有多列而更大。
 */
#define BloomMaxFilterSize \
	MAXALIGN_DOWN(BLCKSZ - \
				  (MAXALIGN(SizeOfPageHeaderData + \
							sizeof(ItemIdData)) + \
				   MAXALIGN(sizeof(BrinSpecialSpace)) + \
				   SizeOfBrinTuple))

/*
 * 用来计算两个哈希函数 h1 和 h2 的种子，二者再用来
 * 按 (h1 + i * h2) 方案生成 k 个哈希。
 */
#define BLOOM_SEED_1	0x71d924af
#define BLOOM_SEED_2	0xba48b314

/*
 * 布隆过滤器
 *
 * 表示建在被索引值的哈希之上的布隆过滤器。也就是
 * 先计算值的 uint32 哈希，再把这个哈希存进
 * 布隆过滤器（并在其上再计算更多哈希）。
 *
 * XXX 我们可以实现 "稀疏" 布隆过滤器，只保留并非
 * 全为 0 的字节。不过索引虽然不支持 TOAST，varlena 仍
 * 可以压缩。因此这似乎没有必要，因为压缩
 * 应当能做同样的事。
 *
 * XXX 我们也可以观察布隆过滤器中已置位的个数，然后
 * 在误报率过高时停止使用它（并且不存位图，以省空间）。
 * 但即使误报率超过
 * 期望值，它仍然能排除一些页范围。
 */
typedef struct BloomFilter
{
	/* varlena 头部（不要直接碰！） */
	int32		vl_len_;

	/* 供各种标志使用的空间（目前未用） */
	uint16		flags;

	/* HASHED 阶段的字段 */
	uint8		nhashes;		/* 哈希函数个数 */
	uint32		nbits;			/* 位图中的位数（大小） */
	uint32		nbits_set;		/* 被置为 1 的位数 */

	/* 布隆过滤器的数据 */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} BloomFilter;


/*
 * bloom_init
 * 		初始化布隆过滤器，分配全部内存。
 *
 * 按 ndistinct 个预期值和
 * 给定的误报率，把过滤器初始化为最优大小。过滤器以 varlena 存储。
 */
static BloomFilter *
bloom_init(int ndistinct, double false_positive_rate)
{
	Size		len;
	BloomFilter *filter;

	int			nbits;			/* 过滤器大小 / 位数 */
	int			nbytes;			/* 过滤器大小 / 字节数 */

	double		k;				/* 哈希函数个数 */

	Assert(ndistinct > 0);
	Assert(false_positive_rate > 0 && false_positive_rate < 1);

	/* 布隆过滤器定尺寸：-(n * ln(p)) / (ln(2))^2 */
	nbits = ceil(-(ndistinct * log(false_positive_rate)) / pow(log(2.0), 2));

	/* 把 m 舍入到整字节 */
	nbytes = ((nbits + 7) / 8);
	nbits = nbytes * 8;

	/*
	 * 拒绝显然大到无法存进一页的过滤器。
	 *
	 * 起初布隆过滤器全是零，因此很容易压缩，但
	 * 随着加入值，它越来越随机，也就越来越
	 * 难压缩。所以一开始什么都放得进页，但以后可能
	 * 突然失败。我们希望避免这种情况，因此拒绝
	 * 显然过大的布隆过滤器。
	 *
	 * XXX 把布隆过滤器定得稍大一些并不少见，用来防御
	 * 意外的数据异常（表的某些部分每个范围的不同
	 * 值更多，等等）。但若有这种需要，我们仍必须确保即使
	 * 偏大的过滤器也能放进页。
	 *
	 * XXX 这项检查并不完美，因为索引可能有多个
	 * 过滤器，单个都小，合在一起却太大。
	 */
	if (nbytes > BloomMaxFilterSize)
		elog(ERROR, "the bloom filter is too large (%d > %zu)", nbytes,
			 BloomMaxFilterSize);

	/*
	 * round(log(2.0) * m / ndistinct)，但假定 Windows 上可能没有
	 * round()
	 */
	k = log(2.0) * nbits / ndistinct;
	k = (k - floor(k) >= 0.5) ? ceil(k) : floor(k);

	/*
	 * 我们分配整个过滤器。其中大部分将是 0 位，因此
	 * varlena 很容易压缩。
	 */
	len = offsetof(BloomFilter, data) + nbytes;

	filter = (BloomFilter *) palloc0(len);

	filter->flags = 0;
	filter->nhashes = (int) k;
	filter->nbits = nbits;

	SET_VARSIZE(filter, len);

	return filter;
}


/*
 * bloom_add_value
 * 		把值加入布隆过滤器。
 */
static BloomFilter *
bloom_add_value(BloomFilter *filter, uint32 value, bool *updated)
{
	int			i;
	uint64		h1,
				h2;

	/* 计算用于布隆过滤器的哈希 */
	h1 = hash_bytes_uint32_extended(value, BLOOM_SEED_1) % filter->nbits;
	h2 = hash_bytes_uint32_extended(value, BLOOM_SEED_2) % filter->nbits;

	/* 计算所需个数的哈希 */
	for (i = 0; i < filter->nhashes; i++)
	{
		/* 即 h1 + h2 + f(i) */
		uint32		h = (h1 + i * h2) % filter->nbits;
		uint32		byte = (h / 8);
		uint32		bit = (h % 8);

		/* 若该位未置位，则置位并记住我们这样做了 */
		if (!(filter->data[byte] & (0x01 << bit)))
		{
			filter->data[byte] |= (0x01 << bit);
			filter->nbits_set++;
			if (updated)
				*updated = true;
		}
	}

	return filter;
}


/*
 * bloom_contains_value
 * 		检查布隆过滤器是否包含某个特定值。
 */
static bool
bloom_contains_value(BloomFilter *filter, uint32 value)
{
	int			i;
	uint64		h1,
				h2;

	/* 计算这两个哈希 */
	h1 = hash_bytes_uint32_extended(value, BLOOM_SEED_1) % filter->nbits;
	h2 = hash_bytes_uint32_extended(value, BLOOM_SEED_2) % filter->nbits;

	/* 计算所需个数的哈希 */
	for (i = 0; i < filter->nhashes; i++)
	{
		/* 即 h1 + h2 + f(i) */
		uint32		h = (h1 + i * h2) % filter->nbits;
		uint32		byte = (h / 8);
		uint32		bit = (h % 8);

		/* 若该位未置位，则该值不在其中 */
		if (!(filter->data[byte] & (0x01 << bit)))
			return false;
	}

	/* 在布隆过滤器中找到了全部哈希 */
	return true;
}

typedef struct BloomOpaque
{
	/*
	 * XXX 目前只需要一个过程（用来计算哈希），但
	 * 为了一致，仍像 inclusion 和 minmax 操作符类那样保留数组。
	 * 将来可能需要更多过程。
	 */
	FmgrInfo	extra_procinfos[BLOOM_MAX_PROCNUMS];
} BloomOpaque;

static FmgrInfo *bloom_get_procinfo(BrinDesc *bdesc, uint16 attno,
									uint16 procnum);


Datum
brin_bloom_opcinfo(PG_FUNCTION_ARGS)
{
	BrinOpcInfo *result;

	/*
	 * opaque->strategy_procinfos 惰性初始化；这里由 palloc0 置为
	 * 全部未初始化，它会把 fn_oid 设为 InvalidOid。
	 *
	 * bloom 索引只把过滤器存成单独一列 BYTEA。
	 */

	result = palloc0(MAXALIGN(SizeofBrinOpcInfo(1)) +
					 sizeof(BloomOpaque));
	result->oi_nstored = 1;
	result->oi_regular_nulls = true;
	result->oi_opaque = (BloomOpaque *)
		MAXALIGN((char *) result + SizeofBrinOpcInfo(1));
	result->oi_typcache[0] = lookup_type_cache(PG_BRIN_BLOOM_SUMMARYOID, 0);

	PG_RETURN_POINTER(result);
}

/*
 * brin_bloom_get_ndistinct
 *		确定用于为布隆过滤器定尺寸的 ndistinct 值。
 *
 * 按 pagesPerRange 的值调整 ndistinct。首先，
 * 若它为负，就视为相对于范围内最大
 * 元组数的比例（假定每页有 MaxHeapTuplesPerPage
 * 个元组，这很可能严重高估）。我们还会钳制
 * 该值，以免不必要地把布隆过滤器定得过大。
 *
 * XXX 只有在提供了 pagesPerRange 值时才能这样做。
 * 若没有提供，那只能是对索引的只读访问，此时
 * 我们其实并不在意。但也许应当回退到
 * 默认的 pagesPerRange 值？
 *
 * XXX 我们也可以取该列的 ndistinct 估计，
 * 并计算一个范围内预期的不同值个数。但
 * 数据可能以各种方式排序，这会很难办，因此
 * 依靠上界估计似乎更好。
 *
 * XXX 我们也可以更好地估计每个 BRIN 范围的行数，
 * 而不是用 MaxHeapTuplesPerPage（它得出的值
 * 很可能远高于实际）。
 */
static int
brin_bloom_get_ndistinct(BrinDesc *bdesc, BloomOptions *opts)
{
	double		ndistinct;
	double		maxtuples;
	BlockNumber pagesPerRange;

	pagesPerRange = BrinGetPagesPerRange(bdesc->bd_index);
	ndistinct = BloomGetNDistinctPerRange(opts);

	Assert(BlockNumberIsValid(pagesPerRange));

	maxtuples = MaxHeapTuplesPerPage * pagesPerRange;

	/*
	 * 与 n_distinct 类似，负值是相对的，这里相对于
	 * 页范围内的最大元组数（maxtuples）。
	 */
	if (ndistinct < 0)
		ndistinct = (-ndistinct) * maxtuples;

	/*
	 * 正值直接使用，但我们仍施加几项
	 * 保险限制，以免使用小得不合理的布隆过滤器。
	 */
	ndistinct = Max(ndistinct, BLOOM_MIN_NDISTINCT_PER_RANGE);

	/*
	 * 并且不要超过该范围内可能的最大元组数，
	 * 否则完全是浪费。
	 */
	ndistinct = Min(ndistinct, maxtuples);

	return (int) ndistinct;
}

/*
 * 检查给定索引元组（它含有某个
 * 页范围的部分状态）：把它与来自另一个堆
 * 元组的给定值相比较。若新值落在现有元组值
 * 所指定的布隆过滤器之外，则更新索引元组并返回 true。否则，
 * 返回 false，且在这种情况下不做修改。
 */
Datum
brin_bloom_add_value(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	Datum		newval = PG_GETARG_DATUM(2);
	bool		isnull PG_USED_FOR_ASSERTS_ONLY = PG_GETARG_DATUM(3);
	BloomOptions *opts = (BloomOptions *) PG_GET_OPCLASS_OPTIONS();
	Oid			colloid = PG_GET_COLLATION();
	FmgrInfo   *hashFn;
	uint32		hashValue;
	bool		updated = false;
	AttrNumber	attno;
	BloomFilter *filter;

	Assert(!isnull);

	attno = column->bv_attno;

	/*
	 * 若这是第一个非空值，就需要初始化布隆
	 * 过滤器。否则只从
	 * BrinValues 中取出已有的布隆过滤器。
	 */
	if (column->bv_allnulls)
	{
		filter = bloom_init(brin_bloom_get_ndistinct(bdesc, opts),
							BloomGetFalsePositiveRate(opts));
		column->bv_values[0] = PointerGetDatum(filter);
		column->bv_allnulls = false;
		updated = true;
	}
	else
		filter = (BloomFilter *) PG_DETOAST_DATUM(column->bv_values[0]);

	/*
	 * 用提供的哈希函数计算新值的哈希，
	 * 再把哈希值加入布隆过滤器。
	 */
	hashFn = bloom_get_procinfo(bdesc, attno, PROCNUM_HASH);

	hashValue = DatumGetUInt32(FunctionCall1Coll(hashFn, colloid, newval));

	filter = bloom_add_value(filter, hashValue, &updated);

	column->bv_values[0] = PointerGetDatum(filter);

	PG_RETURN_BOOL(updated);
}

/*
 * 给定对应于某个页范围的索引元组和一个扫描键，
 * 返回该扫描键是否与索引元组的布隆
 * 过滤器一致。若一致则返回 true，否则返回 false。
 */
Datum
brin_bloom_consistent(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	ScanKey    *keys = (ScanKey *) PG_GETARG_POINTER(2);
	int			nkeys = PG_GETARG_INT32(3);
	Oid			colloid = PG_GET_COLLATION();
	AttrNumber	attno;
	Datum		value;
	Datum		matches;
	FmgrInfo   *finfo;
	uint32		hashValue;
	BloomFilter *filter;
	int			keyno;

	filter = (BloomFilter *) PG_DETOAST_DATUM(column->bv_values[0]);

	Assert(filter);

	matches = true;

	for (keyno = 0; keyno < nkeys; keyno++)
	{
		ScanKey		key = keys[keyno];

		/* NULL 键在 bringetbitmap 中处理并被滤掉 */
		Assert(!(key->sk_flags & SK_ISNULL));

		attno = key->sk_attno;
		value = key->sk_argument;

		switch (key->sk_strategy)
		{
			case BloomEqualStrategyNumber:

				/*
				 * 在相等情形（WHERE col = someval）下，若想
				 * 返回当前页范围，需要范围内的最小值
				 * <= 扫描键，且最大值 >= 扫描键。
				 */
				finfo = bloom_get_procinfo(bdesc, attno, PROCNUM_HASH);

				hashValue = DatumGetUInt32(FunctionCall1Coll(finfo, colloid, value));
				matches &= bloom_contains_value(filter, hashValue);

				break;
			default:
				/* 不应发生 */
				elog(ERROR, "invalid strategy number %d", key->sk_strategy);
				matches = 0;
				break;
		}

		if (!matches)
			break;
	}

	PG_RETURN_DATUM(matches);
}

/*
 * 给定两个 BrinValues，把第一个更新为二者所含汇总
 * 值的并。第二个保持不动。
 *
 * XXX 目前假定这些布隆过滤器参数相同。将来
 * 应当有 'can union' 函数，用来判断能否合并
 * 两个特定的布隆过滤器。
 */
Datum
brin_bloom_union(PG_FUNCTION_ARGS)
{
	int			i;
	int			nbytes;
	BrinValues *col_a = (BrinValues *) PG_GETARG_POINTER(1);
	BrinValues *col_b = (BrinValues *) PG_GETARG_POINTER(2);
	BloomFilter *filter_a;
	BloomFilter *filter_b;

	Assert(col_a->bv_attno == col_b->bv_attno);
	Assert(!col_a->bv_allnulls && !col_b->bv_allnulls);

	filter_a = (BloomFilter *) PG_DETOAST_DATUM(col_a->bv_values[0]);
	filter_b = (BloomFilter *) PG_DETOAST_DATUM(col_b->bv_values[0]);

	/* 确保这些过滤器使用相同的参数 */
	Assert(filter_a && filter_b);
	Assert(filter_a->nbits == filter_b->nbits);
	Assert(filter_a->nhashes == filter_b->nhashes);
	Assert((filter_a->nbits > 0) && (filter_a->nbits % 8 == 0));

	nbytes = (filter_a->nbits) / 8;

	/* 直接对位图做 OR */
	for (i = 0; i < nbytes; i++)
		filter_a->data[i] |= filter_b->data[i];

	/* 更新过滤器中已置位的个数 */
	filter_a->nbits_set = pg_popcount((const char *) filter_a->data, nbytes);

	/* 若 filter_a 曾被解压，则更新汇总 */
	if (PointerGetDatum(filter_a) != col_a->bv_values[0])
	{
		pfree(DatumGetPointer(col_a->bv_values[0]));
		col_a->bv_values[0] = PointerGetDatum(filter_a);
	}

	/* 若 filter_b 曾被解压，也释放它 */
	if (PointerGetDatum(filter_b) != col_b->bv_values[0])
		pfree(filter_b);

	PG_RETURN_VOID();
}

/*
 * 缓存并返回 inclusion 操作符类的支持过程
 *
 * 返回给定支持函数编号对应的过程；
 * 若不存在则返回 null。
 */
static FmgrInfo *
bloom_get_procinfo(BrinDesc *bdesc, uint16 attno, uint16 procnum)
{
	BloomOpaque *opaque;
	uint16		basenum = procnum - PROCNUM_BASE;

	/*
	 * 我们把这些缓存在 opaque 结构中，以避免反复的系统缓存
	 * 查找。
	 */
	opaque = (BloomOpaque *) bdesc->bd_info[attno - 1]->oi_opaque;

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

Datum
brin_bloom_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(BloomOptions));

	add_local_real_reloption(relopts, "n_distinct_per_range",
							 "number of distinct items expected in a BRIN page range",
							 BLOOM_DEFAULT_NDISTINCT_PER_RANGE,
							 -1.0, INT_MAX, offsetof(BloomOptions, nDistinctPerRange));

	add_local_real_reloption(relopts, "false_positive_rate",
							 "desired false-positive rate for the bloom filters",
							 BLOOM_DEFAULT_FALSE_POSITIVE_RATE,
							 BLOOM_MIN_FALSE_POSITIVE_RATE,
							 BLOOM_MAX_FALSE_POSITIVE_RATE,
							 offsetof(BloomOptions, falsePositiveRate));

	PG_RETURN_VOID();
}

/*
 * brin_bloom_summary_in
 *		- brin_bloom_summary 类型的输入例程。
 *
 * brin_bloom_summary 只在内部用来表示
 * BRIN bloom 索引中的汇总，因此它自己没有操作，我们
 * 也不允许输入。
 */
Datum
brin_bloom_summary_in(PG_FUNCTION_ARGS)
{
	/*
	 * brin_bloom_summary 以二进制形式存储数据，不需要解析文本
	 * 输入，因此不允许这样做。
	 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_brin_bloom_summary")));

	PG_RETURN_VOID();			/* 让编译器保持安静 */
}


/*
 * brin_bloom_summary_out
 *		- brin_bloom_summary 类型的输出例程。
 *
 * BRIN bloom 汇总被序列化成 bytea 值，但我们希望
 * 输出人类能看懂的更清楚的形式。
 */
Datum
brin_bloom_summary_out(PG_FUNCTION_ARGS)
{
	BloomFilter *filter;
	StringInfoData str;

	/* 把数据 detoast，得到带完整 4B 头部的值 */
	filter = (BloomFilter *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));

	initStringInfo(&str);
	appendStringInfoChar(&str, '{');

	appendStringInfo(&str, "mode: hashed  nhashes: %u  nbits: %u  nbits_set: %u",
					 filter->nhashes, filter->nbits, filter->nbits_set);

	appendStringInfoChar(&str, '}');

	PG_RETURN_CSTRING(str.data);
}

/*
 * brin_bloom_summary_recv
 *		- brin_bloom_summary 类型的二进制输入例程。
 */
Datum
brin_bloom_summary_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_brin_bloom_summary")));

	PG_RETURN_VOID();			/* 让编译器保持安静 */
}

/*
 * brin_bloom_summary_send
 *		- brin_bloom_summary 类型的二进制输出例程。
 *
 * BRIN bloom 汇总被序列化在 bytea 值中（尽管
 * 类型名不同），因此直接把它发送出去即可。
 */
Datum
brin_bloom_summary_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}
