/*-------------------------------------------------------------------------
 *
 * ts_selfuncs.c
 *	  Selectivity estimation functions for text search operators.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/ts_selfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "tsearch/ts_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"


/*
 * 默认全文检索选择率取得足够小,以在典型表密度下
 * 鼓励索引扫描。细节见 selfuncs.h 与
 * DEFAULT_EQ_SEL。
 */
#define DEFAULT_TS_MATCH_SEL 0.005

/* 在 MCELEMs 中二分查找用的查找表类型 */
typedef struct
{
	text	   *element;
	float4		frequency;
} TextFreq;

/* 在 TextFreqs 数组中 bsearch 的键类型 */
typedef struct
{
	char	   *lexeme;
	int			length;
} LexemeKey;

static Selectivity tsquerysel(VariableStatData *vardata, Datum constval);
static Selectivity mcelem_tsquery_selec(TSQuery query,
										Datum *mcelem, int nmcelem,
										float4 *numbers, int nnumbers);
static Selectivity tsquery_opr_selec(QueryItem *item, char *operand,
									 TextFreq *lookup, int length, float4 minfreq);
static int	compare_lexeme_textfreq(const void *e1, const void *e2);

#define tsquery_opr_selec_no_stats(query) \
	tsquery_opr_selec(GETQUERY(query), GETOPERAND(query), NULL, 0, 0)


/*
 *	tsmatchsel -- "@@" 的选择率
 *
 * 限制选择率:tsvector @@ tsquery 与
 * tsquery @@ tsvector
 */
Datum
tsmatchsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			operator = PG_GETARG_OID(1);
#endif
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Selectivity selec;

	/*
	 * 若表达式不是变量 = 某物或某物 = 变量,则
	 * 放弃并返回默认估计。
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_TS_MATCH_SEL);

	/*
	 * 若那一侧也非常量,同样做不成有用估计。
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_TS_MATCH_SEL);
	}

	/*
	 * "@@" 运算符是严格的,故可立即处理 NULL
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}

	/*
	 * 此处处理的是变量与常量。常量须为 TSQuery,
	 * 否则做不成有用估计。必须检查,因变量也可能
	 * 是 TSQuery 而非 TSVector。
	 */
	if (((Const *) other)->consttype == TSQUERYOID)
	{
		/* tsvector @@ tsquery 或反过来 */
		Assert(vardata.vartype == TSVECTOROID);

		selec = tsquerysel(&vardata, ((Const *) other)->constvalue);
	}
	else
	{
		/* 若看不到查询结构,只好放弃 */
		selec = DEFAULT_TS_MATCH_SEL;
	}

	ReleaseVariableStats(vardata);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}


/*
 *	tsmatchjoinsel -- "@@" 的连接选择率
 *
 * tsvector @@ tsquery 与 tsquery @@ tsvector 的连接选择率函数
 */
Datum
tsmatchjoinsel(PG_FUNCTION_ARGS)
{
	/* 眼下只好放弃 */
	PG_RETURN_FLOAT8(DEFAULT_TS_MATCH_SEL);
}


/*
 * tsvector 变量对 tsquery 常量的 @@ 选择率
 */
static Selectivity
tsquerysel(VariableStatData *vardata, Datum constval)
{
	Selectivity selec;
	TSQuery		query;

	/* 调用方已确保常量是 TSQuery,现取出 */
	query = DatumGetTSQuery(constval);

	/* 空查询不匹配任何行 */
	if (query->size == 0)
		return (Selectivity) 0.0;

	if (HeapTupleIsValid(vardata->statsTuple))
	{
		Form_pg_statistic stats;
		AttStatsSlot sslot;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);

		/* 对 tsvector 列,MCELEM 将是 TEXT 元素数组 */
		if (get_attstatsslot(&sslot, vardata->statsTuple,
							 STATISTIC_KIND_MCELEM, InvalidOid,
							 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
		{
			/*
			 * tsvector 变量有最常见元素槽,
			 * 用之。
			 */
			selec = mcelem_tsquery_selec(query, sslot.values, sslot.nvalues,
										 sslot.numbers, sslot.nnumbers);
			free_attstatsslot(&sslot);
		}
		else
		{
			/* 无最常见元素信息,只好不用 */
			selec = tsquery_opr_selec_no_stats(query);
		}

		/*
		 * MCE 统计只计非空行,故按空行调整。
		 */
		selec *= (1.0 - stats->stanullfrac);
	}
	else
	{
		/* 全无统计,只好不用 */
		selec = tsquery_opr_selec_no_stats(query);
		/* 此处假定无空值,故不做 stanullfrac 校正 */
	}

	return selec;
}

/*
 * 把 pg_statistic 数组提取为有用格式。
 */
static Selectivity
mcelem_tsquery_selec(TSQuery query, Datum *mcelem, int nmcelem,
					 float4 *numbers, int nnumbers)
{
	float4		minfreq;
	TextFreq   *lookup;
	Selectivity selec;
	int			i;

	/*
	 * Numbers 应比 Values 多两项,因末两格用于
	 * 最小与最大频率。否则放弃。
	 *
	 * (注:MCELEM 统计槽定义允许第三项额外数字表示
	 * 空值频率,但我们不预期它出现在
	 * tsvector 列上。)
	 */
	if (nnumbers != nmcelem + 2)
		return tsquery_opr_selec_no_stats(query);

	/*
	 * 转置为单一数组以便使用 bsearch()。
	 */
	lookup = (TextFreq *) palloc(sizeof(TextFreq) * nmcelem);
	for (i = 0; i < nmcelem; i++)
	{
		/*
		 * 这些 text 来自数组,故不可能压缩或
		 * 行外存储 -- 可安全使用 VARSIZE_ANY*。
		 */
		Assert(!VARATT_IS_COMPRESSED(mcelem[i]) && !VARATT_IS_EXTERNAL(mcelem[i]));
		lookup[i].element = (text *) DatumGetPointer(mcelem[i]);
		lookup[i].frequency = numbers[i];
	}

	/*
	 * 取最低频率。compute_tsvector_stats() 把它存于
	 * Numbers 数组倒数第二格。见 ts_typanalyze.c
	 */
	minfreq = numbers[nnumbers - 2];

	selec = tsquery_opr_selec(GETQUERY(query), GETOPERAND(query), lookup,
							  nmcelem, minfreq);

	pfree(lookup);

	return selec;
}

/*
 * 前序遍历 tsquery,按如下计算选择率:
 *
 *	 selec(left_oper) * selec(right_oper) 于 AND & PHRASE,
 *
 *	 selec(left_oper) + selec(right_oper) -
 *		selec(left_oper) * selec(right_oper) 于 OR,
 *
 *	 1 - select(oper) 于 NOT
 *
 *	 前缀 VAL 节点:基于直方图估计
 *
 *	 精确 VAL 且在 MCELEM:freq[val]
 *	 否则:min(freq[MCELEM]) / 2
 *
 * MCELEM 数组已排序(见 ts_typanalyze.c),故可用
 * 二分查找确定 freq[MCELEM]。
 *
 * 即使没有 tsvector 统计仍用此逻辑,
 * 只是 VAL 用默认估计。由 lookup == NULL
 * 表示。
 */
static Selectivity
tsquery_opr_selec(QueryItem *item, char *operand,
				  TextFreq *lookup, int length, float4 minfreq)
{
	Selectivity selec;

	/* 本函数递归,可能把栈撑爆 */
	check_stack_depth();

	if (item->type == QI_VAL)
	{
		QueryOperand *oper = (QueryOperand *) item;
		LexemeKey	key;

		/*
		 * 为 bsearch() 准备键。
		 */
		key.lexeme = operand + oper->distance;
		key.length = oper->length;

		if (oper->prefix)
		{
			/* 前缀匹配,即查询项为 lexeme:* */
			Selectivity matched,
						allmces;
			int			i,
						n_matched;

			/*
			 * 策略是扫描 MCELEM 列表并合并匹配该前缀者
			 * 的频率。再把匹配比例外推到其余行,假定
			 * 在此方面 MCELEM 可代表整个词元人群。(对照
			 * histogram_selectivity()。)注意这些是最常
			 * 见元素而非最常见值,故不互斥。
			 * 我们把出现当作独立事件。
			 *
			 * 仅当 MCELEM 相当多时此计划才好。
			 *
			 * 阈值定为 100。无统计或统计不足时,任意采用
			 * DEFAULT_TS_MATCH_SEL*4。
			 *
			 */
			if (lookup == NULL || length < 100)
				return (Selectivity) (DEFAULT_TS_MATCH_SEL * 4);

			matched = allmces = 0;
			n_matched = 0;
			for (i = 0; i < length; i++)
			{
				TextFreq   *t = lookup + i;
				int			tlen = VARSIZE_ANY_EXHDR(t->element);

				if (tlen >= key.length &&
					strncmp(key.lexeme, VARDATA_ANY(t->element),
							key.length) == 0)
				{
					matched += t->frequency - matched * t->frequency;
					n_matched++;
				}
				allmces += t->frequency - allmces * t->frequency;
			}

			/* 钳制以确保舍入误差下仍合理 */
			CLAMP_PROBABILITY(matched);
			CLAMP_PROBABILITY(allmces);

			selec = matched + (1.0 - allmces) * ((double) n_matched / length);

			/*
			 * 无论如何,切勿相信前缀匹配的选择率会低于
			 * 非 MCELEM 词元的赋值。这保持了 "word:*"
			 * 估计匹配行数至少应与 "word"
			 * 一样多的性质。
			 */
			selec = Max(Min(DEFAULT_TS_MATCH_SEL, minfreq / 2), selec);
		}
		else
		{
			/* 常规精确词元匹配 */
			TextFreq   *searchres;

			/* 变量无统计时用 DEFAULT_TS_MATCH_SEL */
			if (lookup == NULL)
				return (Selectivity) DEFAULT_TS_MATCH_SEL;

			searchres = (TextFreq *) bsearch(&key, lookup, length,
											 sizeof(TextFreq),
											 compare_lexeme_textfreq);

			if (searchres)
			{
				/*
				 * 元素在 MCELEM 中。返回精确选择率(或至少
				 * 与 ANALYZE 能弄清的一样精确)。
				 */
				selec = searchres->frequency;
			}
			else
			{
				/*
				 * 元素不在 MCELEM 中。放弃,但假定选择率
				 * 不会高于 minfreq / 2。
				 */
				selec = Min(DEFAULT_TS_MATCH_SEL, minfreq / 2);
			}
		}
	}
	else
	{
		/* 当前 TSQuery 节点是运算符 */
		Selectivity s1,
					s2;

		switch (item->qoperator.oper)
		{
			case OP_NOT:
				selec = 1.0 - tsquery_opr_selec(item + 1, operand,
												lookup, length, minfreq);
				break;

			case OP_PHRASE:
			case OP_AND:
				s1 = tsquery_opr_selec(item + 1, operand,
									   lookup, length, minfreq);
				s2 = tsquery_opr_selec(item + item->qoperator.left, operand,
									   lookup, length, minfreq);
				selec = s1 * s2;
				break;

			case OP_OR:
				s1 = tsquery_opr_selec(item + 1, operand,
									   lookup, length, minfreq);
				s2 = tsquery_opr_selec(item + item->qoperator.left, operand,
									   lookup, length, minfreq);
				selec = s1 + s2 - s1 * s2;
				break;

			default:
				elog(ERROR, "unrecognized operator: %d", item->qoperator.oper);
				selec = 0;		/* 让编译器安静 */
				break;
		}
	}

	/* 钳制中间结果,以免舍入误差导致离谱 */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * bsearch() 比较器:词元(带长度的非空终止串)
 * 与 TextFreq。先比长度再逐字节,因 ANALYZE
 * 存入统计元组前即如此排序。
 * 细节见 ts_typanalyze.c。
 */
static int
compare_lexeme_textfreq(const void *e1, const void *e2)
{
	const LexemeKey *key = (const LexemeKey *) e1;
	const TextFreq *t = (const TextFreq *) e2;
	int			len1,
				len2;

	len1 = key->length;
	len2 = VARSIZE_ANY_EXHDR(t->element);

	/* 先比长度,或可避免一次 strncmp */
	if (len1 > len2)
		return 1;
	else if (len1 < len2)
		return -1;

	/* 退回逐字节比较 */
	return strncmp(key->lexeme, VARDATA_ANY(t->element), len1);
}
