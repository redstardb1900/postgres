/*-------------------------------------------------------------------------
 *
 * tsginidx.c
 *	 tsvector_ops 的 GIN 支持函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsginidx.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin.h"
#include "access/stratnum.h"
#include "miscadmin.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "varatt.h"


Datum
gin_cmp_tslexeme(PG_FUNCTION_ARGS)
{
	text	   *a = PG_GETARG_TEXT_PP(0);
	text	   *b = PG_GETARG_TEXT_PP(1);
	int			cmp;

	cmp = tsCompareString(VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a),
						  VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b),
						  false);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_INT32(cmp);
}

Datum
gin_cmp_prefix(PG_FUNCTION_ARGS)
{
	text	   *a = PG_GETARG_TEXT_PP(0);
	text	   *b = PG_GETARG_TEXT_PP(1);

#ifdef NOT_USED
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	Pointer		extra_data = PG_GETARG_POINTER(3);
#endif
	int			cmp;

	cmp = tsCompareString(VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a),
						  VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b),
						  true);

	if (cmp < 0)
		cmp = 1;				/* 阻止继续扫描 */

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_INT32(cmp);
}

Datum
gin_extract_tsvector(PG_FUNCTION_ARGS)
{
	TSVector	vector = PG_GETARG_TSVECTOR(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = NULL;

	*nentries = vector->size;
	if (vector->size > 0)
	{
		int			i;
		WordEntry  *we = ARRPTR(vector);

		entries = (Datum *) palloc(sizeof(Datum) * vector->size);

		for (i = 0; i < vector->size; i++)
		{
			text	   *txt;

			txt = cstring_to_text_with_len(STRPTR(vector) + we->pos, we->len);
			entries[i] = PointerGetDatum(txt);

			we++;
		}
	}

	PG_FREE_IF_COPY(vector, 0);
	PG_RETURN_POINTER(entries);
}

Datum
gin_extract_tsquery(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);

	/* StrategyNumber strategy = PG_GETARG_UINT16(2); */
	bool	  **ptr_partialmatch = (bool **) PG_GETARG_POINTER(3);
	Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);

	/* bool   **nullFlags = (bool **) PG_GETARG_POINTER(5); */
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *entries = NULL;

	*nentries = 0;

	if (query->size > 0)
	{
		QueryItem  *item = GETQUERY(query);
		int32		i,
					j;
		bool	   *partialmatch;
		int		   *map_item_operand;

		/*
		 * 若查询没有任何必需的正向匹配
		 * （例如 '! foo'），就必须做整个索引
		 *扫描
		 */
		if (tsquery_requires_match(item))
			*searchMode = GIN_SEARCH_MODE_DEFAULT;
		else
			*searchMode = GIN_SEARCH_MODE_ALL;

		/* 统计 VAL 项的个数 */
		j = 0;
		for (i = 0; i < query->size; i++)
		{
			if (item[i].type == QI_VAL)
				j++;
		}
		*nentries = j;

		entries = (Datum *) palloc(sizeof(Datum) * j);
		partialmatch = *ptr_partialmatch = (bool *) palloc(sizeof(bool) * j);

		/*
		 * 建立映射，把项编号转换为对应操作数（即
		 * 条目）的编号。条目编号用于 consistent
		 * 方法中的检查数组，各条目同一映射。
		 */
		*extra_data = (Pointer *) palloc(sizeof(Pointer) * j);
		map_item_operand = (int *) palloc0(sizeof(int) * query->size);

		/* 现在重新扫描各 VAL 项并填数组 */
		j = 0;
		for (i = 0; i < query->size; i++)
		{
			if (item[i].type == QI_VAL)
			{
				QueryOperand *val = &item[i].qoperand;
				text	   *txt;

				txt = cstring_to_text_with_len(GETOPERAND(query) + val->distance,
											   val->length);
				entries[j] = PointerGetDatum(txt);
				partialmatch[j] = val->prefix;
				(*extra_data)[j] = (Pointer) map_item_operand;
				map_item_operand[i] = j;
				j++;
			}
		}
	}

	PG_FREE_IF_COPY(query, 0);

	PG_RETURN_POINTER(entries);
}

typedef struct
{
	QueryItem  *first_item;
	GinTernaryValue *check;
	int		   *map_item_operand;
} GinChkVal;

/*
 * TS_execute 回调：将 tsquery 操作数匹配到 GIN 索引数据
 */
static TSTernaryValue
checkcondition_gin(void *checkval, QueryOperand *val, ExecPhraseData *data)
{
	GinChkVal  *gcv = (GinChkVal *) checkval;
	int			j;
	GinTernaryValue result;

	/* 把项编号转换为对应条目（操作数）的编号 */
	j = gcv->map_item_operand[((QueryItem *) val) - gcv->first_item];

	/* 判断当前条目是否存在于被索引值中 */
	result = gcv->check[j];

	/*
	 * 若用到任何需要权重的值，或调用者需要位置
	 * 信息，就必须重查，故将 TRUE 换成 MAYBE。
	 */
	if (result == GIN_TRUE)
	{
		if (val->weight != 0 || data != NULL)
			result = GIN_MAYBE;
	}

	/*
	 * 我们依赖 GinTernaryValue 与 TSTernaryValue 采用
	 * 等价的取值方式。若这一点改变，可以用 switch 语句
	 * 来映射这些值，但看起来不太可能发生。
	 */
	return (TSTernaryValue) result;
}

Datum
gin_tsquery_consistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);

	/* StrategyNumber strategy = PG_GETARG_UINT16(1); */
	TSQuery		query = PG_GETARG_TSQUERY(2);

	/* int32	nkeys = PG_GETARG_INT32(3); */
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res = false;

	/* 初始假定查询无需重查 */
	*recheck = false;

	if (query->size > 0)
	{
		GinChkVal	gcv;

		/*
		 * check 参数数组对查询中每个值（操作数）都有对应
		 * 项。
		 */
		gcv.first_item = GETQUERY(query);
		gcv.check = (GinTernaryValue *) check;
		gcv.map_item_operand = (int *) (extra_data[0]);

		switch (TS_execute_ternary(GETQUERY(query),
								   &gcv,
								   TS_EXEC_PHRASE_NO_POS,
								   checkcondition_gin))
		{
			case TS_NO:
				res = false;
				break;
			case TS_YES:
				res = true;
				break;
			case TS_MAYBE:
				res = true;
				*recheck = true;
				break;
		}
	}

	PG_RETURN_BOOL(res);
}

Datum
gin_tsquery_triconsistent(PG_FUNCTION_ARGS)
{
	GinTernaryValue *check = (GinTernaryValue *) PG_GETARG_POINTER(0);

	/* StrategyNumber strategy = PG_GETARG_UINT16(1); */
	TSQuery		query = PG_GETARG_TSQUERY(2);

	/* int32	nkeys = PG_GETARG_INT32(3); */
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	GinTernaryValue res = GIN_FALSE;

	if (query->size > 0)
	{
		GinChkVal	gcv;

		/*
		 * check 参数数组对查询中每个值（操作数）都有对应
		 * 项。
		 */
		gcv.first_item = GETQUERY(query);
		gcv.check = check;
		gcv.map_item_operand = (int *) (extra_data[0]);

		res = TS_execute_ternary(GETQUERY(query),
								 &gcv,
								 TS_EXEC_PHRASE_NO_POS,
								 checkcondition_gin);
	}

	PG_RETURN_GIN_TERNARY_VALUE(res);
}

/*
 * 从前 gin_extract_tsvector 只有两个参数，现在有三个，
 * 但仍需要一个双参数的 pg_proc 条目，以支持重载
 * 9.1 之前 contrib/tsearch2 的算子类声明。这个兼容
 * 函数终究应被移除。（注：你可能会说"嘿，但上面
 * 的代码只是在使用两个参数，直接那样声明好了"。
 * 如果你那样尝试，会发现 opr_sanity 回归测试报警。）
 */
Datum
gin_extract_tsvector_2args(PG_FUNCTION_ARGS)
{
	if (PG_NARGS() < 3)			/* 不应发生 */
		elog(ERROR, "gin_extract_tsvector requires three arguments");
	return gin_extract_tsvector(fcinfo);
}

/*
 * 同样，我们需要一个 gin_extract_tsquery 存根版本，
 * 仅五个参数。
 */
Datum
gin_extract_tsquery_5args(PG_FUNCTION_ARGS)
{
	if (PG_NARGS() < 7)			/* 不应发生 */
		elog(ERROR, "gin_extract_tsquery requires seven arguments");
	return gin_extract_tsquery(fcinfo);
}

/*
 * 同样，我们需要一个 gin_tsquery_consistent 存根版本，
 * 仅六个参数。
 */
Datum
gin_tsquery_consistent_6args(PG_FUNCTION_ARGS)
{
	if (PG_NARGS() < 8)			/* 不应发生 */
		elog(ERROR, "gin_tsquery_consistent requires eight arguments");
	return gin_tsquery_consistent(fcinfo);
}

/*
 * 同样，还有一个 gin_extract_tsquery 存根版本，其参数
 * 类型已不再被视为合适。
 */
Datum
gin_extract_tsquery_oldsig(PG_FUNCTION_ARGS)
{
	return gin_extract_tsquery(fcinfo);
}

/*
 * 同样，还有一个 gin_tsquery_consistent 存根版本，其参数
 * 类型已不再被视为合适。
 */
Datum
gin_tsquery_consistent_oldsig(PG_FUNCTION_ARGS)
{
	return gin_tsquery_consistent(fcinfo);
}
