/*-------------------------------------------------------------------------
 *
 * ts_typanalyze.c
 *	  functions for gathering statistics from tsvector columns
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/ts_typanalyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "commands/vacuum.h"
#include "common/hashfn.h"
#include "tsearch/ts_type.h"
#include "utils/builtins.h"
#include "varatt.h"


/* 词元的哈希键 */
typedef struct
{
	char	   *lexeme;			/* 词元(非空终止!) */
	int			length;			/* 其字节长度 */
} LexemeHashKey;

/* 有损计数算法的哈希表条目 */
typedef struct
{
	LexemeHashKey key;			/* 此即 LC 算法中的 'e'。 */
	int			frequency;		/* 即 'f'。 */
	int			delta;			/* 此即 'delta'。 */
} TrackItem;

static void compute_tsvector_stats(VacAttrStats *stats,
								   AnalyzeAttrFetchFunc fetchfunc,
								   int samplerows,
								   double totalrows);
static void prune_lexemes_hashtable(HTAB *lexemes_tab, int b_current);
static uint32 lexeme_hash(const void *key, Size keysize);
static int	lexeme_match(const void *key1, const void *key2, Size keysize);
static int	lexeme_compare(const void *key1, const void *key2);
static int	trackitem_compare_frequencies_desc(const void *e1, const void *e2,
											   void *arg);
static int	trackitem_compare_lexemes(const void *e1, const void *e2,
									  void *arg);


/*
 *	ts_typanalyze -- tsvector 列的定制 typanalyze 函数
 */
Datum
ts_typanalyze(PG_FUNCTION_ARGS)
{
	VacAttrStats *stats = (VacAttrStats *) PG_GETARG_POINTER(0);
	Form_pg_attribute attr = stats->attr;

	/* 若 attstattarget 列为负,则用默认值 */
	/* 注:可涂改 stats->attr,因其为副本 */
	if (attr->attstattarget < 0)
		attr->attstattarget = default_statistics_target;

	stats->compute_stats = compute_tsvector_stats;
	/* 关于 minrows 取值见 commands/analyze.c 注释 */
	stats->minrows = 300 * attr->attstattarget;

	PG_RETURN_BOOL(true);
}

/*
 *	compute_tsvector_stats() -- compute statistics for a tsvector column
 *
 *	This functions computes statistics that are useful for determining @@
 *	operations' selectivity, along with the fraction of non-null rows and
 *	average width.
 *
 *	Instead of finding the most common values, as we do for most datatypes,
 *	we're looking for the most common lexemes. This is more useful, because
 *	there most probably won't be any two rows with the same tsvector and thus
 *	the notion of a MCV is a bit bogus with this datatype. With a list of the
 *	most common lexemes we can do a better job at figuring out @@ selectivity.
 *
 *	For the same reasons we assume that tsvector columns are unique when
 *	determining the number of distinct values.
 *
 *	The algorithm used is Lossy Counting, as proposed in the paper "Approximate
 *	frequency counts over data streams" by G. S. Manku and R. Motwani, in
 *	Proceedings of the 28th International Conference on Very Large Data Bases,
 *	Hong Kong, China, August 2002, section 4.2. The paper is available at
 *	http://www.vldb.org/conf/2002/S10P03.pdf
 *
 *	The Lossy Counting (aka LC) algorithm goes like this:
 *	Let s be the threshold frequency for an item (the minimum frequency we
 *	are interested in) and epsilon the error margin for the frequency. Let D
 *	be a set of triples (e, f, delta), where e is an element value, f is that
 *	element's frequency (actually, its current occurrence count) and delta is
 *	the maximum error in f. We start with D empty and process the elements in
 *	batches of size w. (The batch size is also known as "bucket size" and is
 *	equal to 1/epsilon.) Let the current batch number be b_current, starting
 *	with 1. For each element e we either increment its f count, if it's
 *	already in D, or insert a new triple into D with values (e, 1, b_current
 *	- 1). After processing each batch we prune D, by removing from it all
 *	elements with f + delta <= b_current.  After the algorithm finishes we
 *	suppress all elements from D that do not satisfy f >= (s - epsilon) * N,
 *	where N is the total number of elements in the input.  We emit the
 *	remaining elements with estimated frequency f/N.  The LC paper proves
 *	that this algorithm finds all elements with true frequency at least s,
 *	and that no frequency is overestimated or is underestimated by more than
 *	epsilon.  Furthermore, given reasonable assumptions about the input
 *	distribution, the required table size is no more than about 7 times w.
 *
 *	We set s to be the estimated frequency of the K'th word in a natural
 *	language's frequency table, where K is the target number of entries in
 *	the MCELEM array plus an arbitrary constant, meant to reflect the fact
 *	that the most common words in any language would usually be stopwords
 *	so we will not actually see them in the input.  We assume that the
 *	distribution of word frequencies (including the stopwords) follows Zipf's
 *	law with an exponent of 1.
 *
 *	Assuming Zipfian distribution, the frequency of the K'th word is equal
 *	to 1/(K * H(W)) where H(n) is 1/2 + 1/3 + ... + 1/n and W is the number of
 *	words in the language.  Putting W as one million, we get roughly 0.07/K.
 *	Assuming top 10 words are stopwords gives s = 0.07/(K + 10).  We set
 *	epsilon = s/10, which gives bucket width w = (K + 10)/0.007 and
 *	maximum expected hashtable size of about 1000 * (K + 10).
 *
 *	Note: in the above discussion, s, epsilon, and f/N are in terms of a
 *	lexeme's frequency as a fraction of all lexemes seen in the input.
 *	However, what we actually want to store in the finished pg_statistic
 *	entry is each lexeme's frequency as a fraction of all rows that it occurs
 *	in.  Assuming that the input tsvectors are correctly constructed, no
 *	lexeme occurs more than once per tsvector, so the final count f is a
 *	correct estimate of the number of input tsvectors it occurs in, and we
 *	need only change the divisor from N to nonnull_cnt to get the number we
 *	want.
 */
static void
compute_tsvector_stats(VacAttrStats *stats,
					   AnalyzeAttrFetchFunc fetchfunc,
					   int samplerows,
					   double totalrows)
{
	int			num_mcelem;
	int			null_cnt = 0;
	double		total_width = 0;

	/* 此即 LC 算法中的 D。 */
	HTAB	   *lexemes_tab;
	HASHCTL		hash_ctl;
	HASH_SEQ_STATUS scan_status;

	/* 此即 LC 算法的当前桶号 */
	int			b_current;

	/* 此即 LC 算法中的 'w' */
	int			bucket_width;
	int			vector_no,
				lexeme_no;
	LexemeHashKey hash_key;

	/*
	 * 希望 MCELEM 数组含 statistics_target * 10 个词元。
	 * 该乘数颇任意,意在反映:pg_statistic 中跟踪的
	 * 个别词元值数目,应多于简单标量列的
	 * 取值数目。
	 */
	num_mcelem = stats->attr->attstattarget * 10;

	/*
	 * 按上文注释,桶宽取 (num_mcelem + 10) / 0.007
	 * (见上文)。
	 */
	bucket_width = (num_mcelem + 10) * 1000 / 7;

	/*
	 * 创建哈希表。它在本地内存,故不必担心初始大小
	 * 溢出。也不必操心加锁与
	 * 内存管理。
	 */
	hash_ctl.keysize = sizeof(LexemeHashKey);
	hash_ctl.entrysize = sizeof(TrackItem);
	hash_ctl.hash = lexeme_hash;
	hash_ctl.match = lexeme_match;
	hash_ctl.hcxt = CurrentMemoryContext;
	lexemes_tab = hash_create("Analyzed lexemes table",
							  num_mcelem,
							  &hash_ctl,
							  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

	/* 初始化计数器 */
	b_current = 1;
	lexeme_no = 0;

	/* 遍历各 tsvector。 */
	for (vector_no = 0; vector_no < samplerows; vector_no++)
	{
		Datum		value;
		bool		isnull;
		TSVector	vector;
		WordEntry  *curentryptr;
		char	   *lexemesptr;
		int			j;

		vacuum_delay_point();

		value = fetchfunc(stats, vector_no, &isnull);

		/*
		 * 检查空/非空。
		 */
		if (isnull)
		{
			null_cnt++;
			continue;
		}

		/*
		 * 累加宽度以算平均宽度。既是 tsvector,必为
		 * varlena。与常规 compute_minimal_stats
		 * 一样,此处用已 toast 的宽度
		 * 做计算。
		 */
		total_width += VARSIZE_ANY(DatumGetPointer(value));

		/*
		 * 现按需 detoast 该 tsvector。
		 */
		vector = DatumGetTSVector(value);

		/*
		 * 遍历 tsvector 中的词元,加入跟踪
		 * 哈希表。
		 */
		lexemesptr = STRPTR(vector);
		curentryptr = ARRPTR(vector);
		for (j = 0; j < vector->size; j++)
		{
			TrackItem  *item;
			bool		found;

			/*
			 * 构造哈希键。此刻键指向(已 detoast 的)
			 * tsvector 值;若创建新项则再拷贝。
			 * 这样处理完其全部词元后即可释放
			 * 该 tsvector。
			 */
			hash_key.lexeme = lexemesptr + curentryptr->pos;
			hash_key.length = curentryptr->len;

			/* 在哈希表中查找当前词元,新则加入 */
			item = (TrackItem *) hash_search(lexemes_tab,
											 &hash_key,
											 HASH_ENTER, &found);

			if (found)
			{
				/* 该词元已在跟踪列表上 */
				item->frequency++;
			}
			else
			{
				/* 初始化新的跟踪列表元素 */
				item->frequency = 1;
				item->delta = b_current - 1;

				item->key.lexeme = palloc(hash_key.length);
				memcpy(item->key.lexeme, hash_key.lexeme, hash_key.length);
			}

			/* lexeme_no 为已处理元素数(即 N) */
			lexeme_no++;

			/* 每处理完一桶后修剪 D 结构 */
			if (lexeme_no % bucket_width == 0)
			{
				prune_lexemes_hashtable(lexemes_tab, b_current);
				b_current++;
			}

			/* 前进到 tsvector 中下一 WordEntry */
			curentryptr++;
		}

		/* 若向量曾被 toast,释放 detoast 副本。 */
		if (TSVectorGetDatum(vector) != value)
			pfree(vector);
	}

	/* 仅当发现若干非空值时才能算真实统计。 */
	if (null_cnt < samplerows)
	{
		int			nonnull_cnt = samplerows - null_cnt;
		int			i;
		TrackItem **sort_table;
		TrackItem  *item;
		int			track_len;
		int			cutoff_freq;
		int			minfreq,
					maxfreq;

		stats->stats_valid = true;
		/* 做简单的空值比例与平均宽度统计 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		stats->stawidth = total_width / (double) nonnull_cnt;

		/* 假定为唯一列(见上文说明) */
		stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);

		/*
		 * 构造有趣哈希表条目的数组,即满足截止频率
		 * (s - epsilon)*N 者。并识别这些项中的
		 * 最小与最大频率。
		 *
		 * 因 epsilon = s/10 且 bucket_width = 1/epsilon,截止
		 * 频率为 9*N / bucket_width。
		 */
		cutoff_freq = 9 * lexeme_no / bucket_width;

		i = hash_get_num_entries(lexemes_tab);	/* 空间肯定够 */
		sort_table = (TrackItem **) palloc(sizeof(TrackItem *) * i);

		hash_seq_init(&scan_status, lexemes_tab);
		track_len = 0;
		minfreq = lexeme_no;
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

		/* 输出一些调试用统计 */
		elog(DEBUG3, "tsvector_stats: target # mces = %d, bucket width = %d, "
			 "# lexemes = %d, hashtable size = %d, usable entries = %d",
			 num_mcelem, bucket_width, lexeme_no, i, track_len);

		/*
		 * 若得到的词元多于真正想要的,去掉频率最低者。
		 * 最简办法是按频率降序 qsort 数组并
		 * 截断。
		 */
		if (num_mcelem < track_len)
		{
			qsort_interruptible(sort_table, track_len, sizeof(TrackItem *),
								trackitem_compare_frequencies_desc, NULL);
			/* 把 minfreq 重置为所保留的最小频率 */
			minfreq = sort_table[num_mcelem - 1]->frequency;
		}
		else
			num_mcelem = track_len;

		/* 生成 MCELEM 槽项 */
		if (num_mcelem > 0)
		{
			MemoryContext old_context;
			Datum	   *mcelem_values;
			float4	   *mcelem_freqs;

			/*
			 * 希望按词元值排序存统计:先长度,再逐字节。
			 * 先比长度因只要一致即可,不在乎顺序,
			 * 且先比长度或可避免一次 strncmp()
			 * 调用。
			 *
			 *
			 * 这与标量统计不同——后者按频率排序。因
			 * 我们常在最常见元素中查特定值以取其
			 * 频率。值预排序时可用二分查找。见
			 * ts_selfuncs.c。
			 *
			 *
			 */
			qsort_interruptible(sort_table, num_mcelem, sizeof(TrackItem *),
								trackitem_compare_lexemes, NULL);

			/* 必须把目标值拷入 anl_context */
			old_context = MemoryContextSwitchTo(stats->anl_context);

			/*
			 * 统计已按词元值排序,但希望不必遍历全部值
			 * 就能得知最小与最大频率。把这两个额外
			 * 频率放在 mcelem_freqs 的两格额外
			 * 单元中。
			 *
			 * (注:MCELEM 统计槽定义允许第三项额外数字
			 * 表示空值频率,但我们不为 tsvector 列创建
			 * 它,因不可能有空元素。)
			 *
			 */
			mcelem_values = (Datum *) palloc(num_mcelem * sizeof(Datum));
			mcelem_freqs = (float4 *) palloc((num_mcelem + 2) * sizeof(float4));

			/*
			 * 关于用 nonnull_cnt 作最终频率估计除数,见
			 * 上文注释。
			 */
			for (i = 0; i < num_mcelem; i++)
			{
				TrackItem  *titem = sort_table[i];

				mcelem_values[i] =
					PointerGetDatum(cstring_to_text_with_len(titem->key.lexeme,
															 titem->key.length));
				mcelem_freqs[i] = (double) titem->frequency / (double) nonnull_cnt;
			}
			mcelem_freqs[i++] = (double) minfreq / (double) nonnull_cnt;
			mcelem_freqs[i] = (double) maxfreq / (double) nonnull_cnt;
			MemoryContextSwitchTo(old_context);

			stats->stakind[0] = STATISTIC_KIND_MCELEM;
			stats->staop[0] = TextEqualOperator;
			stats->stacoll[0] = DEFAULT_COLLATION_OID;
			stats->stanumbers[0] = mcelem_freqs;
			/* 关于两个额外频率字段见上文注释 */
			stats->numnumbers[0] = num_mcelem + 2;
			stats->stavalues[0] = mcelem_values;
			stats->numvalues[0] = num_mcelem;
			/* 存储的是 text 值 */
			stats->statypid[0] = TEXTOID;
			stats->statyplen[0] = -1;	/* typlen,varlena 为 -1 */
			stats->statypbyval[0] = false;
			stats->statypalign[0] = 'i';
		}
	}
	else
	{
		/* 只见到空值;假定整列皆空 */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		stats->stawidth = 0;	/* "未知" */
		stats->stadistinct = 0.0;	/* "未知" */
	}

	/*
	 * 不必费心清理临时 palloc。哈希表也应随之消失,
	 * 因其使用了子内存上下文。
	 */
}

/*
 *	修剪有损计数算法中 D 结构的函数。
 *	更广说明见 compute_tsvector_stats()。
 */
static void
prune_lexemes_hashtable(HTAB *lexemes_tab, int b_current)
{
	HASH_SEQ_STATUS scan_status;
	TrackItem  *item;

	hash_seq_init(&scan_status, lexemes_tab);
	while ((item = (TrackItem *) hash_seq_search(&scan_status)) != NULL)
	{
		if (item->frequency + item->delta <= b_current)
		{
			char	   *lexeme = item->key.lexeme;

			if (hash_search(lexemes_tab, &item->key,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "hash table corrupted");
			pfree(lexeme);
		}
	}
}

/*
 * 词元的哈希函数。它们是串但非空终止,
 * 故需要专用哈希函数。
 */
static uint32
lexeme_hash(const void *key, Size keysize)
{
	const LexemeHashKey *l = (const LexemeHashKey *) key;

	return DatumGetUInt32(hash_any((const unsigned char *) l->lexeme,
								   l->length));
}

/*
 *	词元匹配函数,用于哈希表查找。
 */
static int
lexeme_match(const void *key1, const void *key2, Size keysize)
{
	/* keysize 参数多余,键自带长度 */
	return lexeme_compare(key1, key2);
}

/*
 *	词元比较函数。
 */
static int
lexeme_compare(const void *key1, const void *key2)
{
	const LexemeHashKey *d1 = (const LexemeHashKey *) key1;
	const LexemeHashKey *d2 = (const LexemeHashKey *) key2;

	/* 先按长度比较 */
	if (d1->length > d2->length)
		return 1;
	else if (d1->length < d2->length)
		return -1;
	/* 长度相等,再逐字节比较 */
	return strncmp(d1->lexeme, d2->lexeme, d1->length);
}

/*
 *	按频率排序 TrackItems 的比较器(降序)
 */
static int
trackitem_compare_frequencies_desc(const void *e1, const void *e2, void *arg)
{
	const TrackItem *const *t1 = (const TrackItem *const *) e1;
	const TrackItem *const *t2 = (const TrackItem *const *) e2;

	return (*t2)->frequency - (*t1)->frequency;
}

/*
 *	按词元排序 TrackItems 的比较器
 */
static int
trackitem_compare_lexemes(const void *e1, const void *e2, void *arg)
{
	const TrackItem *const *t1 = (const TrackItem *const *) e1;
	const TrackItem *const *t2 = (const TrackItem *const *) e2;

	return lexeme_compare(&(*t1)->key, &(*t2)->key);
}
