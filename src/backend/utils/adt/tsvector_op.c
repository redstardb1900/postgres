/*-------------------------------------------------------------------------
 *
 * tsvector_op.c
 *	  tsvector 上的操作
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsvector_op.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "lib/qunique.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "tsearch/ts_utils.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"


typedef struct
{
	WordEntry  *arrb;
	WordEntry  *arre;
	char	   *values;
	char	   *operand;
} CHKVAL;


typedef struct StatEntry
{
	uint32		ndoc;			/* 0 表示我们在遍历树的过程中
								 * 已经到过此处 */
	uint32		nentry;
	struct StatEntry *left;
	struct StatEntry *right;
	uint32		lenlexeme;
	char		lexeme[FLEXIBLE_ARRAY_MEMBER];
} StatEntry;

#define STATENTRYHDRSZ	(offsetof(StatEntry, lexeme))

typedef struct
{
	int32		weight;

	uint32		maxdepth;

	StatEntry **stack;
	uint32		stackpos;

	StatEntry  *root;
} TSVectorStat;


static TSTernaryValue TS_execute_recurse(QueryItem *curitem, void *arg,
										 uint32 flags,
										 TSExecuteCallback chkcond);
static bool TS_execute_locations_recurse(QueryItem *curitem,
										 void *arg,
										 TSExecuteCallback chkcond,
										 List **locations);
static int	tsvector_bsearch(const TSVector tsv, char *lexeme, int lexeme_len);
static Datum tsvector_update_trigger(PG_FUNCTION_ARGS, bool config_column);


/*
 * 顺序：haspos, len, word 及所有位置 (pos, weight)
 */
static int
silly_cmp_tsvector(const TSVector a, const TSVector b)
{
	if (VARSIZE(a) < VARSIZE(b))
		return -1;
	else if (VARSIZE(a) > VARSIZE(b))
		return 1;
	else if (a->size < b->size)
		return -1;
	else if (a->size > b->size)
		return 1;
	else
	{
		WordEntry  *aptr = ARRPTR(a);
		WordEntry  *bptr = ARRPTR(b);
		int			i = 0;
		int			res;


		for (i = 0; i < a->size; i++)
		{
			if (aptr->haspos != bptr->haspos)
			{
				return (aptr->haspos > bptr->haspos) ? -1 : 1;
			}
			else if ((res = tsCompareString(STRPTR(a) + aptr->pos, aptr->len, STRPTR(b) + bptr->pos, bptr->len, false)) != 0)
			{
				return res;
			}
			else if (aptr->haspos)
			{
				WordEntryPos *ap = POSDATAPTR(a, aptr);
				WordEntryPos *bp = POSDATAPTR(b, bptr);
				int			j;

				if (POSDATALEN(a, aptr) != POSDATALEN(b, bptr))
					return (POSDATALEN(a, aptr) > POSDATALEN(b, bptr)) ? -1 : 1;

				for (j = 0; j < POSDATALEN(a, aptr); j++)
				{
					if (WEP_GETPOS(*ap) != WEP_GETPOS(*bp))
					{
						return (WEP_GETPOS(*ap) > WEP_GETPOS(*bp)) ? -1 : 1;
					}
					else if (WEP_GETWEIGHT(*ap) != WEP_GETWEIGHT(*bp))
					{
						return (WEP_GETWEIGHT(*ap) > WEP_GETWEIGHT(*bp)) ? -1 : 1;
					}
					ap++, bp++;
				}
			}

			aptr++;
			bptr++;
		}
	}

	return 0;
}

#define TSVECTORCMPFUNC( type, action, ret )			\
Datum													\
tsvector_##type(PG_FUNCTION_ARGS)						\
{														\
	TSVector	a = PG_GETARG_TSVECTOR(0);				\
	TSVector	b = PG_GETARG_TSVECTOR(1);				\
	int			res = silly_cmp_tsvector(a, b);			\
	PG_FREE_IF_COPY(a,0);								\
	PG_FREE_IF_COPY(b,1);								\
	PG_RETURN_##ret( res action 0 );					\
}	\
/* 让编译器安静 - 无多余 ; */					\
extern int no_such_variable

TSVECTORCMPFUNC(lt, <, BOOL);
TSVECTORCMPFUNC(le, <=, BOOL);
TSVECTORCMPFUNC(eq, ==, BOOL);
TSVECTORCMPFUNC(ge, >=, BOOL);
TSVECTORCMPFUNC(gt, >, BOOL);
TSVECTORCMPFUNC(ne, !=, BOOL);
TSVECTORCMPFUNC(cmp, +, INT32);

Datum
tsvector_strip(PG_FUNCTION_ARGS)
{
	TSVector	in = PG_GETARG_TSVECTOR(0);
	TSVector	out;
	int			i,
				len = 0;
	WordEntry  *arrin = ARRPTR(in),
			   *arrout;
	char	   *cur;

	for (i = 0; i < in->size; i++)
		len += arrin[i].len;

	len = CALCDATASIZE(in->size, len);
	out = (TSVector) palloc0(len);
	SET_VARSIZE(out, len);
	out->size = in->size;
	arrout = ARRPTR(out);
	cur = STRPTR(out);
	for (i = 0; i < in->size; i++)
	{
		memcpy(cur, STRPTR(in) + arrin[i].pos, arrin[i].len);
		arrout[i].haspos = 0;
		arrout[i].len = arrin[i].len;
		arrout[i].pos = cur - STRPTR(out);
		cur += arrout[i].len;
	}

	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_POINTER(out);
}

Datum
tsvector_length(PG_FUNCTION_ARGS)
{
	TSVector	in = PG_GETARG_TSVECTOR(0);
	int32		ret = in->size;

	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_INT32(ret);
}

Datum
tsvector_setweight(PG_FUNCTION_ARGS)
{
	TSVector	in = PG_GETARG_TSVECTOR(0);
	char		cw = PG_GETARG_CHAR(1);
	TSVector	out;
	int			i,
				j;
	WordEntry  *entry;
	WordEntryPos *p;
	int			w = 0;

	switch (cw)
	{
		case 'A':
		case 'a':
			w = 3;
			break;
		case 'B':
		case 'b':
			w = 2;
			break;
		case 'C':
		case 'c':
			w = 1;
			break;
		case 'D':
		case 'd':
			w = 0;
			break;
		default:
			/* 内部错误 */
			elog(ERROR, "unrecognized weight: %d", cw);
	}

	out = (TSVector) palloc(VARSIZE(in));
	memcpy(out, in, VARSIZE(in));
	entry = ARRPTR(out);
	i = out->size;
	while (i--)
	{
		if ((j = POSDATALEN(out, entry)) != 0)
		{
			p = POSDATAPTR(out, entry);
			while (j--)
			{
				WEP_SETWEIGHT(*p, w);
				p++;
			}
		}
		entry++;
	}

	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_POINTER(out);
}

/*
 * setweight(tsin tsvector, char_weight "char", lexemes "text"[])
 *
 * 给 tsin 中列于 lexemes 里的元素赋予权重 w。
 */
Datum
tsvector_setweight_by_filter(PG_FUNCTION_ARGS)
{
	TSVector	tsin = PG_GETARG_TSVECTOR(0);
	char		char_weight = PG_GETARG_CHAR(1);
	ArrayType  *lexemes = PG_GETARG_ARRAYTYPE_P(2);

	TSVector	tsout;
	int			i,
				j,
				nlexemes,
				weight;
	WordEntry  *entry;
	Datum	   *dlexemes;
	bool	   *nulls;

	switch (char_weight)
	{
		case 'A':
		case 'a':
			weight = 3;
			break;
		case 'B':
		case 'b':
			weight = 2;
			break;
		case 'C':
		case 'c':
			weight = 1;
			break;
		case 'D':
		case 'd':
			weight = 0;
			break;
		default:
			/* 内部错误 */
			elog(ERROR, "unrecognized weight: %c", char_weight);
	}

	tsout = (TSVector) palloc(VARSIZE(tsin));
	memcpy(tsout, tsin, VARSIZE(tsin));
	entry = ARRPTR(tsout);

	deconstruct_array_builtin(lexemes, TEXTOID, &dlexemes, &nulls, &nlexemes);

	/*
	 * 假定 lexemes 数组远短于 tsvector，我们就可以遍历
	 * lexemes，对其中每个词位在 tsvector 中
	 * 做二分查找。
	 */
	for (i = 0; i < nlexemes; i++)
	{
		char	   *lex;
		int			lex_len,
					lex_pos;

		/* 忽略 NULL 数组元素，它们必然不匹配 */
		if (nulls[i])
			continue;

		lex = VARDATA(dlexemes[i]);
		lex_len = VARSIZE(dlexemes[i]) - VARHDRSZ;
		lex_pos = tsvector_bsearch(tsout, lex, lex_len);

		if (lex_pos >= 0 && (j = POSDATALEN(tsout, entry + lex_pos)) != 0)
		{
			WordEntryPos *p = POSDATAPTR(tsout, entry + lex_pos);

			while (j--)
			{
				WEP_SETWEIGHT(*p, weight);
				p++;
			}
		}
	}

	PG_FREE_IF_COPY(tsin, 0);
	PG_FREE_IF_COPY(lexemes, 2);

	PG_RETURN_POINTER(tsout);
}

#define compareEntry(pa, a, pb, b) \
	tsCompareString((pa) + (a)->pos, (a)->len,	\
					(pb) + (b)->pos, (b)->len,	\
					false)

/*
 * 把 src 的位置偏移 maxpos 后加到 dest 上。
 * 返回加入的数量（因溢出可能少于预期）
 */
static int32
add_pos(TSVector src, WordEntry *srcptr,
		TSVector dest, WordEntry *destptr,
		int32 maxpos)
{
	uint16	   *clen = &_POSVECPTR(dest, destptr)->npos;
	int			i;
	uint16		slen = POSDATALEN(src, srcptr),
				startlen;
	WordEntryPos *spos = POSDATAPTR(src, srcptr),
			   *dpos = POSDATAPTR(dest, destptr);

	if (!destptr->haspos)
		*clen = 0;

	startlen = *clen;
	for (i = 0;
		 i < slen && *clen < MAXNUMPOS &&
		 (*clen == 0 || WEP_GETPOS(dpos[*clen - 1]) != MAXENTRYPOS - 1);
		 i++)
	{
		WEP_SETWEIGHT(dpos[*clen], WEP_GETWEIGHT(spos[i]));
		WEP_SETPOS(dpos[*clen], LIMITPOS(WEP_GETPOS(spos[i]) + maxpos));
		(*clen)++;
	}

	if (*clen != startlen)
		destptr->haspos = 1;
	return *clen - startlen;
}

/*
 * 在 TSVector 中对给定词位二分查找。
 * 返回该词位在 TSVector 条目数组中的位置，未找到则为
 * -1。
 */
static int
tsvector_bsearch(const TSVector tsv, char *lexeme, int lexeme_len)
{
	WordEntry  *arrin = ARRPTR(tsv);
	int			StopLow = 0,
				StopHigh = tsv->size,
				StopMiddle,
				cmp;

	while (StopLow < StopHigh)
	{
		StopMiddle = (StopLow + StopHigh) / 2;

		cmp = tsCompareString(lexeme, lexeme_len,
							  STRPTR(tsv) + arrin[StopMiddle].pos,
							  arrin[StopMiddle].len,
							  false);

		if (cmp < 0)
			StopHigh = StopMiddle;
		else if (cmp > 0)
			StopLow = StopMiddle + 1;
		else					/* 找到 */
			return StopMiddle;
	}

	return -1;
}

/*
 * qsort 比较函数
 */

static int
compare_int(const void *va, const void *vb)
{
	int			a = *((const int *) va);
	int			b = *((const int *) vb);

	if (a == b)
		return 0;
	return (a > b) ? 1 : -1;
}

static int
compare_text_lexemes(const void *va, const void *vb)
{
	Datum		a = *((const Datum *) va);
	Datum		b = *((const Datum *) vb);
	char	   *alex = VARDATA_ANY(a);
	int			alex_len = VARSIZE_ANY_EXHDR(a);
	char	   *blex = VARDATA_ANY(b);
	int			blex_len = VARSIZE_ANY_EXHDR(b);

	return tsCompareString(alex, alex_len, blex, blex_len, false);
}

/*
 * 按偏移量数组从 TSVector 中删除词位的内部例程。
 *
 * int *indices_to_delete -- 待删除词位偏移数组（此处会改！）
 * int indices_count -- 该数组的大小
 *
 * 返回去除了给定词位及其位置与权重的新
 * TSVector。
 */
static TSVector
tsvector_delete_by_indices(TSVector tsv, int *indices_to_delete,
						   int indices_count)
{
	TSVector	tsout;
	WordEntry  *arrin = ARRPTR(tsv),
			   *arrout;
	char	   *data = STRPTR(tsv),
			   *dataout;
	int			i,				/* arrin 下标 */
				j,				/* arrout 下标 */
				k,				/* indices_to_delete 下标 */
				curoff;			/* dataout 区下标 */

	/*
	 * 对过滤数组排序，以简化下面的成员检查。另外，
	 * 去掉其中重复条目，这样我们可假定 indices_count
	 * 恰好等于将被删除的词位个数。
	 */
	if (indices_count > 1)
	{
		qsort(indices_to_delete, indices_count, sizeof(int), compare_int);
		indices_count = qunique(indices_to_delete, indices_count, sizeof(int),
								compare_int);
	}

	/*
	 * 这里我们高估 tsout 的大小，因为我们不知道被删除
	 * 词位用掉多少空间。稍后设置精确大小。
	 */
	tsout = (TSVector) palloc0(VARSIZE(tsv));

	/* 这个计数必须正确，因为 STRPTR(tsout) 依赖它。 */
	tsout->size = tsv->size - indices_count;

	/*
	 * 把 tsv 复制到 tsout，跳过 indices_to_delete 所列项。
	 */
	arrout = ARRPTR(tsout);
	dataout = STRPTR(tsout);
	curoff = 0;
	for (i = j = k = 0; i < tsv->size; i++)
	{
		/*
		 * 若当前 i 在 indices_to_delete 中，则跳过该词位。
		 * 由于 indices_to_delete 已排序，我们只需检查当前
		 * 的第 k 个条目。
		 */
		if (k < indices_count && i == indices_to_delete[k])
		{
			k++;
			continue;
		}

		/* 复制词位及其位置和权重 */
		memcpy(dataout + curoff, data + arrin[i].pos, arrin[i].len);
		arrout[j].haspos = arrin[i].haspos;
		arrout[j].len = arrin[i].len;
		arrout[j].pos = curoff;
		curoff += arrin[i].len;
		if (arrin[i].haspos)
		{
			int			len = POSDATALEN(tsv, arrin + i) * sizeof(WordEntryPos)
				+ sizeof(uint16);

			curoff = SHORTALIGN(curoff);
			memcpy(dataout + curoff,
				   STRPTR(tsv) + SHORTALIGN(arrin[i].pos + arrin[i].len),
				   len);
			curoff += len;
		}

		j++;
	}

	/*
	 * 现在 k 应当恰好等于 indices_count。若不是这样，则
	 * 调用者给我们的下标超出了 [0, tsv->size) 范围，而
	 * 对 tsout 大小的估计有误。
	 */
	Assert(k == indices_count);

	SET_VARSIZE(tsout, CALCDATASIZE(tsout->size, curoff));
	return tsout;
}

/*
 * 从 tsvector 删除给定词位。
 * 用户级 ts_delete(tsvector, text) 的实现。
 */
Datum
tsvector_delete_str(PG_FUNCTION_ARGS)
{
	TSVector	tsin = PG_GETARG_TSVECTOR(0),
				tsout;
	text	   *tlexeme = PG_GETARG_TEXT_PP(1);
	char	   *lexeme = VARDATA_ANY(tlexeme);
	int			lexeme_len = VARSIZE_ANY_EXHDR(tlexeme),
				skip_index;

	if ((skip_index = tsvector_bsearch(tsin, lexeme, lexeme_len)) == -1)
		PG_RETURN_POINTER(tsin);

	tsout = tsvector_delete_by_indices(tsin, &skip_index, 1);

	PG_FREE_IF_COPY(tsin, 0);
	PG_FREE_IF_COPY(tlexeme, 1);
	PG_RETURN_POINTER(tsout);
}

/*
 * 从 tsvector 删除给定的词位数组。
 * 用户级 ts_delete(tsvector, text[]) 的实现。
 */
Datum
tsvector_delete_arr(PG_FUNCTION_ARGS)
{
	TSVector	tsin = PG_GETARG_TSVECTOR(0),
				tsout;
	ArrayType  *lexemes = PG_GETARG_ARRAYTYPE_P(1);
	int			i,
				nlex,
				skip_count,
			   *skip_indices;
	Datum	   *dlexemes;
	bool	   *nulls;

	deconstruct_array_builtin(lexemes, TEXTOID, &dlexemes, &nulls, &nlex);

	/*
	 * 在典型用例中，待删除的词位数组相对较小。因此
	 * 我们针对该场景做优化：遍历 lexarr，对 lexarr 中
	 * 的每个词位在 tsvector 中执行二分查找。
	 */
	skip_indices = palloc0(nlex * sizeof(int));
	for (i = skip_count = 0; i < nlex; i++)
	{
		char	   *lex;
		int			lex_len,
					lex_pos;

		/* 忽略 NULL 数组元素，它们必然不匹配 */
		if (nulls[i])
			continue;

		lex = VARDATA(dlexemes[i]);
		lex_len = VARSIZE(dlexemes[i]) - VARHDRSZ;
		lex_pos = tsvector_bsearch(tsin, lex, lex_len);

		if (lex_pos >= 0)
			skip_indices[skip_count++] = lex_pos;
	}

	tsout = tsvector_delete_by_indices(tsin, skip_indices, skip_count);

	pfree(skip_indices);
	PG_FREE_IF_COPY(tsin, 0);
	PG_FREE_IF_COPY(lexemes, 1);

	PG_RETURN_POINTER(tsout);
}

/*
 * 把 tsvector 展开为具有下列列的表：
 *	   lexeme:词位文本
 *	   positions：词位位置的整数数组
 *	   weights：与位置对应的权重字符数组
 */
Datum
tsvector_unnest(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	TSVector	tsin;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(3);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "lexeme",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "positions",
						   INT2ARRAYOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "weights",
						   TEXTARRAYOID, -1, 0);
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = tupdesc;

		funcctx->user_fctx = PG_GETARG_TSVECTOR_COPY(0);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	tsin = (TSVector) funcctx->user_fctx;

	if (funcctx->call_cntr < tsin->size)
	{
		WordEntry  *arrin = ARRPTR(tsin);
		char	   *data = STRPTR(tsin);
		HeapTuple	tuple;
		int			j,
					i = funcctx->call_cntr;
		bool		nulls[] = {false, false, false};
		Datum		values[3];

		values[0] = PointerGetDatum(cstring_to_text_with_len(data + arrin[i].pos, arrin[i].len));

		if (arrin[i].haspos)
		{
			WordEntryPosVector *posv;
			Datum	   *positions;
			Datum	   *weights;
			char		weight;

			/*
			 * tsvector 内部将位置与权重一并存放于
			 * 同一 uint16（权重 2 位，位置 14 位），把两者
			 * 拆分为两个数组。
			 */
			posv = _POSVECPTR(tsin, arrin + i);
			positions = palloc(posv->npos * sizeof(Datum));
			weights = palloc(posv->npos * sizeof(Datum));
			for (j = 0; j < posv->npos; j++)
			{
				positions[j] = Int16GetDatum(WEP_GETPOS(posv->pos[j]));
				weight = 'D' - WEP_GETWEIGHT(posv->pos[j]);
				weights[j] = PointerGetDatum(cstring_to_text_with_len(&weight,
																	  1));
			}

			values[1] = PointerGetDatum(construct_array_builtin(positions, posv->npos, INT2OID));
			values[2] = PointerGetDatum(construct_array_builtin(weights, posv->npos, TEXTOID));
		}
		else
		{
			nulls[1] = nulls[2] = true;
		}

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * 把 tsvector 转换成词位数组。
 */
Datum
tsvector_to_array(PG_FUNCTION_ARGS)
{
	TSVector	tsin = PG_GETARG_TSVECTOR(0);
	WordEntry  *arrin = ARRPTR(tsin);
	Datum	   *elements;
	int			i;
	ArrayType  *array;

	elements = palloc(tsin->size * sizeof(Datum));

	for (i = 0; i < tsin->size; i++)
	{
		elements[i] = PointerGetDatum(cstring_to_text_with_len(STRPTR(tsin) + arrin[i].pos,
															   arrin[i].len));
	}

	array = construct_array_builtin(elements, tsin->size, TEXTOID);

	pfree(elements);
	PG_FREE_IF_COPY(tsin, 0);
	PG_RETURN_POINTER(array);
}

/*
 * 从词位数组构建 tsvector。
 */
Datum
array_to_tsvector(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	TSVector	tsout;
	Datum	   *dlexemes;
	WordEntry  *arrout;
	bool	   *nulls;
	int			nitems,
				i,
				tslen,
				datalen = 0;
	char	   *cur;

	deconstruct_array_builtin(v, TEXTOID, &dlexemes, &nulls, &nitems);

	/*
	 * 拒绝 NULL 和零长度字符串（或许该直接忽略它们，
	 * 如何）
	 */
	for (i = 0; i < nitems; i++)
	{
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("lexeme array may not contain nulls")));

		if (VARSIZE(dlexemes[i]) - VARHDRSZ == 0)
			ereport(ERROR,
					(errcode(ERRCODE_ZERO_LENGTH_CHARACTER_STRING),
					 errmsg("lexeme array may not contain empty strings")));
	}

	/* 排序并去重，这是有效 tsvector 的必要条件。 */
	if (nitems > 1)
	{
		qsort(dlexemes, nitems, sizeof(Datum), compare_text_lexemes);
		nitems = qunique(dlexemes, nitems, sizeof(Datum),
						 compare_text_lexemes);
	}

	/* 计算存活词位所需的空间。 */
	for (i = 0; i < nitems; i++)
		datalen += VARSIZE(dlexemes[i]) - VARHDRSZ;
	tslen = CALCDATASIZE(nitems, datalen);

	/* 分配并填充 tsvector。 */
	tsout = (TSVector) palloc0(tslen);
	SET_VARSIZE(tsout, tslen);
	tsout->size = nitems;

	arrout = ARRPTR(tsout);
	cur = STRPTR(tsout);
	for (i = 0; i < nitems; i++)
	{
		char	   *lex = VARDATA(dlexemes[i]);
		int			lex_len = VARSIZE(dlexemes[i]) - VARHDRSZ;

		memcpy(cur, lex, lex_len);
		arrout[i].haspos = 0;
		arrout[i].len = lex_len;
		arrout[i].pos = cur - STRPTR(tsout);
		cur += lex_len;
	}

	PG_FREE_IF_COPY(v, 0);
	PG_RETURN_POINTER(tsout);
}

/*
 * ts_filter()：仅保留 tsvector 中给定权重的词位。
 */
Datum
tsvector_filter(PG_FUNCTION_ARGS)
{
	TSVector	tsin = PG_GETARG_TSVECTOR(0),
				tsout;
	ArrayType  *weights = PG_GETARG_ARRAYTYPE_P(1);
	WordEntry  *arrin = ARRPTR(tsin),
			   *arrout;
	char	   *datain = STRPTR(tsin),
			   *dataout;
	Datum	   *dweights;
	bool	   *nulls;
	int			nweights;
	int			i,
				j;
	int			cur_pos = 0;
	char		mask = 0;

	deconstruct_array_builtin(weights, CHAROID, &dweights, &nulls, &nweights);

	for (i = 0; i < nweights; i++)
	{
		char		char_weight;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("weight array may not contain nulls")));

		char_weight = DatumGetChar(dweights[i]);
		switch (char_weight)
		{
			case 'A':
			case 'a':
				mask = mask | 8;
				break;
			case 'B':
			case 'b':
				mask = mask | 4;
				break;
			case 'C':
			case 'c':
				mask = mask | 2;
				break;
			case 'D':
			case 'd':
				mask = mask | 1;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized weight: \"%c\"", char_weight)));
		}
	}

	tsout = (TSVector) palloc0(VARSIZE(tsin));
	tsout->size = tsin->size;
	arrout = ARRPTR(tsout);
	dataout = STRPTR(tsout);

	for (i = j = 0; i < tsin->size; i++)
	{
		WordEntryPosVector *posvin,
				   *posvout;
		int			npos = 0;
		int			k;

		if (!arrin[i].haspos)
			continue;

		posvin = _POSVECPTR(tsin, arrin + i);
		posvout = (WordEntryPosVector *)
			(dataout + SHORTALIGN(cur_pos + arrin[i].len));

		for (k = 0; k < posvin->npos; k++)
		{
			if (mask & (1 << WEP_GETWEIGHT(posvin->pos[k])))
				posvout->pos[npos++] = posvin->pos[k];
		}

		/* 未找到合适位置，则跳过该词位 */
		if (!npos)
			continue;

		arrout[j].haspos = true;
		arrout[j].len = arrin[i].len;
		arrout[j].pos = cur_pos;

		memcpy(dataout + cur_pos, datain + arrin[i].pos, arrin[i].len);
		posvout->npos = npos;
		cur_pos += SHORTALIGN(arrin[i].len);
		cur_pos += POSDATALEN(tsout, arrout + j) * sizeof(WordEntryPos) +
			sizeof(uint16);
		j++;
	}

	tsout->size = j;
	if (dataout != STRPTR(tsout))
		memmove(STRPTR(tsout), dataout, cur_pos);

	SET_VARSIZE(tsout, CALCDATASIZE(tsout->size, cur_pos));

	PG_FREE_IF_COPY(tsin, 0);
	PG_RETURN_POINTER(tsout);
}

Datum
tsvector_concat(PG_FUNCTION_ARGS)
{
	TSVector	in1 = PG_GETARG_TSVECTOR(0);
	TSVector	in2 = PG_GETARG_TSVECTOR(1);
	TSVector	out;
	WordEntry  *ptr;
	WordEntry  *ptr1,
			   *ptr2;
	WordEntryPos *p;
	int			maxpos = 0,
				i,
				j,
				i1,
				i2,
				dataoff,
				output_bytes,
				output_size;
	char	   *data,
			   *data1,
			   *data2;

	/* 取 in1 的最大位置；需用它来偏移 in2 的位置 */
	ptr = ARRPTR(in1);
	i = in1->size;
	while (i--)
	{
		if ((j = POSDATALEN(in1, ptr)) != 0)
		{
			p = POSDATAPTR(in1, ptr);
			while (j--)
			{
				if (WEP_GETPOS(*p) > maxpos)
					maxpos = WEP_GETPOS(*p);
				p++;
			}
		}
		ptr++;
	}

	ptr1 = ARRPTR(in1);
	ptr2 = ARRPTR(in2);
	data1 = STRPTR(in1);
	data2 = STRPTR(in2);
	i1 = in1->size;
	i2 = in2->size;

	/*
	 * 对所需空间的保守估计。我们可能需要
	 * 两个输入的全部数据，且可能要在原本没有填充
	 * 字节处补上填充字节。
	 */
	output_bytes = VARSIZE(in1) + VARSIZE(in2) + i1 + i2;

	out = (TSVector) palloc0(output_bytes);
	SET_VARSIZE(out, output_bytes);

	/*
	 * 我们必须让 out->size 有效，以便 STRPTR(out) 有意义。
	 * 最后会收缩掉所有未用空间。
	 */
	out->size = in1->size + in2->size;

	ptr = ARRPTR(out);
	data = STRPTR(out);
	dataoff = 0;
	while (i1 && i2)
	{
		int			cmp = compareEntry(data1, ptr1, data2, ptr2);

		if (cmp < 0)
		{						/* 先 in1 */
			ptr->haspos = ptr1->haspos;
			ptr->len = ptr1->len;
			memcpy(data + dataoff, data1 + ptr1->pos, ptr1->len);
			ptr->pos = dataoff;
			dataoff += ptr1->len;
			if (ptr->haspos)
			{
				dataoff = SHORTALIGN(dataoff);
				memcpy(data + dataoff, _POSVECPTR(in1, ptr1), POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16));
				dataoff += POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16);
			}

			ptr++;
			ptr1++;
			i1--;
		}
		else if (cmp > 0)
		{						/* 先 in2 */
			ptr->haspos = ptr2->haspos;
			ptr->len = ptr2->len;
			memcpy(data + dataoff, data2 + ptr2->pos, ptr2->len);
			ptr->pos = dataoff;
			dataoff += ptr2->len;
			if (ptr->haspos)
			{
				int			addlen = add_pos(in2, ptr2, out, ptr, maxpos);

				if (addlen == 0)
					ptr->haspos = 0;
				else
				{
					dataoff = SHORTALIGN(dataoff);
					dataoff += addlen * sizeof(WordEntryPos) + sizeof(uint16);
				}
			}

			ptr++;
			ptr2++;
			i2--;
		}
		else
		{
			ptr->haspos = ptr1->haspos | ptr2->haspos;
			ptr->len = ptr1->len;
			memcpy(data + dataoff, data1 + ptr1->pos, ptr1->len);
			ptr->pos = dataoff;
			dataoff += ptr1->len;
			if (ptr->haspos)
			{
				if (ptr1->haspos)
				{
					dataoff = SHORTALIGN(dataoff);
					memcpy(data + dataoff, _POSVECPTR(in1, ptr1), POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16));
					dataoff += POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16);
					if (ptr2->haspos)
						dataoff += add_pos(in2, ptr2, out, ptr, maxpos) * sizeof(WordEntryPos);
				}
				else			/* 必然有 ptr2->haspos */
				{
					int			addlen = add_pos(in2, ptr2, out, ptr, maxpos);

					if (addlen == 0)
						ptr->haspos = 0;
					else
					{
						dataoff = SHORTALIGN(dataoff);
						dataoff += addlen * sizeof(WordEntryPos) + sizeof(uint16);
					}
				}
			}

			ptr++;
			ptr1++;
			ptr2++;
			i1--;
			i2--;
		}
	}

	while (i1)
	{
		ptr->haspos = ptr1->haspos;
		ptr->len = ptr1->len;
		memcpy(data + dataoff, data1 + ptr1->pos, ptr1->len);
		ptr->pos = dataoff;
		dataoff += ptr1->len;
		if (ptr->haspos)
		{
			dataoff = SHORTALIGN(dataoff);
			memcpy(data + dataoff, _POSVECPTR(in1, ptr1), POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16));
			dataoff += POSDATALEN(in1, ptr1) * sizeof(WordEntryPos) + sizeof(uint16);
		}

		ptr++;
		ptr1++;
		i1--;
	}

	while (i2)
	{
		ptr->haspos = ptr2->haspos;
		ptr->len = ptr2->len;
		memcpy(data + dataoff, data2 + ptr2->pos, ptr2->len);
		ptr->pos = dataoff;
		dataoff += ptr2->len;
		if (ptr->haspos)
		{
			int			addlen = add_pos(in2, ptr2, out, ptr, maxpos);

			if (addlen == 0)
				ptr->haspos = 0;
			else
			{
				dataoff = SHORTALIGN(dataoff);
				dataoff += addlen * sizeof(WordEntryPos) + sizeof(uint16);
			}
		}

		ptr++;
		ptr2++;
		i2--;
	}

	/*
	 * 我们不逐个检查各偏移量，而是统一
	 * 在末尾查 pos 溢出。
	 */
	if (dataoff > MAXSTRPOS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("string is too long for tsvector (%d bytes, max %d bytes)", dataoff, MAXSTRPOS)));

	/*
	 * 调整大小（断言我们没有超出最初的估计），
	 * 并收缩掉未使用的数组条目。
	 */
	output_size = ptr - ARRPTR(out);
	Assert(output_size <= out->size);
	out->size = output_size;
	if (data != STRPTR(out))
		memmove(STRPTR(out), data, dataoff);
	output_bytes = CALCDATASIZE(out->size, dataoff);
	Assert(output_bytes <= VARSIZE(out));
	SET_VARSIZE(out, output_bytes);

	PG_FREE_IF_COPY(in1, 0);
	PG_FREE_IF_COPY(in2, 1);
	PG_RETURN_POINTER(out);
}

/*
 * 按 tsvector 规则比较字符串。
 *
 * 若 prefix 为 true，则当且仅当 b 有前缀 a 时得零
 */
int32
tsCompareString(char *a, int lena, char *b, int lenb, bool prefix)
{
	int			cmp;

	if (lena == 0)
	{
		if (prefix)
			cmp = 0;			/* 空字符串是任意串的前缀 */
		else
			cmp = (lenb > 0) ? -1 : 0;
	}
	else if (lenb == 0)
	{
		cmp = (lena > 0) ? 1 : 0;
	}
	else
	{
		cmp = memcmp(a, b, Min((unsigned int) lena, (unsigned int) lenb));

		if (prefix)
		{
			if (cmp == 0 && lena > lenb)
				cmp = 1;		/* a 更长，故不是 b 的前缀 */
		}
		else if (cmp == 0 && lena != lenb)
		{
			cmp = (lena < lenb) ? -1 : 1;
		}
	}

	return cmp;
}

/*
 * 检查权重信息和/或用所需位置填充 'data'
 */
static TSTernaryValue
checkclass_str(CHKVAL *chkval, WordEntry *entry, QueryOperand *val,
			   ExecPhraseData *data)
{
	TSTernaryValue result = TS_NO;

	Assert(data == NULL || data->npos == 0);

	if (entry->haspos)
	{
		WordEntryPosVector *posvec;

		/*
		 * 这里我们不能使用 _POSVECPTR 宏，因为指向
		 * tsvector 词位存储的指针已包含在 chkval->values 中。
		 */
		posvec = (WordEntryPosVector *)
			(chkval->values + SHORTALIGN(entry->pos + entry->len));

		if (val->weight && data)
		{
			WordEntryPos *posvec_iter = posvec->pos;
			WordEntryPos *dptr;

			/*
			 * 按权重过滤位置信息
			 */
			dptr = data->pos = palloc(sizeof(WordEntryPos) * posvec->npos);
			data->allocated = true;

			/* 是否存在权重匹配的位置？ */
			while (posvec_iter < posvec->pos + posvec->npos)
			{
				/* 若为真，则把该位置追加到 data->pos */
				if (val->weight & (1 << WEP_GETWEIGHT(*posvec_iter)))
				{
					*dptr = WEP_GETPOS(*posvec_iter);
					dptr++;
				}

				posvec_iter++;
			}

			data->npos = dptr - data->pos;

			if (data->npos > 0)
				result = TS_YES;
			else
			{
				pfree(data->pos);
				data->pos = NULL;
				data->allocated = false;
			}
		}
		else if (val->weight)
		{
			WordEntryPos *posvec_iter = posvec->pos;

			/* 是否存在权重匹配的位置？ */
			while (posvec_iter < posvec->pos + posvec->npos)
			{
				if (val->weight & (1 << WEP_GETWEIGHT(*posvec_iter)))
				{
					result = TS_YES;
					break;		/* 无需再继续 */
				}

				posvec_iter++;
			}
		}
		else if (data)
		{
			data->npos = posvec->npos;
			data->pos = posvec->pos;
			data->allocated = false;
			result = TS_YES;
		}
		else
		{
			/* 最简单情形：无权重检查，不需要位置 */
			result = TS_YES;
		}
	}
	else
	{
		/*
		 * 位置信息缺失，因此若调用者需要位置信息，
		 * 只能说也许存在匹配。
		 *
		 * 但请注意，这里我们*不*检查 val->weight。
		 * 按照惯例，去除位置的 tsvector 被视为可以匹配
		 * 查询，无论查询是否带权重限制；这有点可疑，
		 * 但我们会保留这种行为。
		 */
		if (data)
			result = TS_MAYBE;
		else
			result = TS_YES;
	}

	return result;
}

/*
 * 把 tsquery 操作数匹配到普通 tsvector 数据的 TS_execute 回调
 */
static TSTernaryValue
checkcondition_str(void *checkval, QueryOperand *val, ExecPhraseData *data)
{
	CHKVAL	   *chkval = (CHKVAL *) checkval;
	WordEntry  *StopLow = chkval->arrb;
	WordEntry  *StopHigh = chkval->arre;
	WordEntry  *StopMiddle = StopHigh;
	TSTernaryValue res = TS_NO;

	/* 循环不变式:StopLow <= val < StopHigh */
	while (StopLow < StopHigh)
	{
		int			difference;

		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		difference = tsCompareString(chkval->operand + val->distance,
									 val->length,
									 chkval->values + StopMiddle->pos,
									 StopMiddle->len,
									 false);

		if (difference == 0)
		{
			/* 检查权重信息并用位置填充 'data' */
			res = checkclass_str(chkval, StopMiddle, val, data);
			break;
		}
		else if (difference > 0)
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle;
	}

	/*
	 * 如果是前缀搜索，我们还应考虑那些以搜索
	 * 词为前缀的词位（它们必然紧跟在上面循环
	 * 中找到的位置之后）。但如果对精确词已
	 * 确定匹配，且调用者不需要位置信息，则
	 * 可跳过。
	 */
	if (val->prefix && (res != TS_YES || data))
	{
		WordEntryPos *allpos = NULL;
		int			npos = 0,
					totalpos = 0;

		/* 针对边界情形调整起始位置 */
		if (StopLow >= StopHigh)
			StopMiddle = StopHigh;

		/* 我们不尝试重用初始匹配的任何数据 */
		if (data)
		{
			if (data->allocated)
				pfree(data->pos);
			data->pos = NULL;
			data->allocated = false;
			data->npos = 0;
		}
		res = TS_NO;

		while ((res != TS_YES || data) &&
			   StopMiddle < chkval->arre &&
			   tsCompareString(chkval->operand + val->distance,
							   val->length,
							   chkval->values + StopMiddle->pos,
							   StopMiddle->len,
							   true) == 0)
		{
			TSTernaryValue subres;

			subres = checkclass_str(chkval, StopMiddle, val, data);

			if (subres != TS_NO)
			{
				if (data)
				{
					/*
					 * 我们需要合并位置信息
					 */
					if (subres == TS_MAYBE)
					{
						/*
						 * 此匹配无位置信息，故整体必须报告
						 *  MAYBE。
						 */
						res = TS_MAYBE;
						/* 忘掉之前所有的位置 */
						npos = 0;
						/* 不要泄漏存储 */
						if (allpos)
							pfree(allpos);
						break;
					}

					while (npos + data->npos > totalpos)
					{
						if (totalpos == 0)
						{
							totalpos = 256;
							allpos = palloc(sizeof(WordEntryPos) * totalpos);
						}
						else
						{
							totalpos *= 2;
							allpos = repalloc(allpos, sizeof(WordEntryPos) * totalpos);
						}
					}

					memcpy(allpos + npos, data->pos, sizeof(WordEntryPos) * data->npos);
					npos += data->npos;

					/* 不要从各个单独匹配中泄漏存储 */
					if (data->allocated)
						pfree(data->pos);
					data->pos = NULL;
					data->allocated = false;
					/* 在下一轮循环前重置 data->npos 很重要 */
					data->npos = 0;
				}
				else
				{
					/* 不需要位置，只需处理 YES/MAYBE */
					if (subres == TS_YES || res == TS_NO)
						res = subres;
				}
			}

			StopMiddle++;
		}

		if (data && npos > 0)
		{
			/* 对找到的位置排序并去重 */
			data->pos = allpos;
			qsort(data->pos, npos, sizeof(WordEntryPos), compareWordEntryPos);
			data->npos = qunique(data->pos, npos, sizeof(WordEntryPos),
								 compareWordEntryPos);
			data->allocated = true;
			res = TS_YES;
		}
	}

	return res;
}

/*
 * 为短语模式下的 tsquery 算子计算输出位置列表。
 *
 * 按 "emit" 的指定合并 Ldata 与 Rdata 中的位置列表，
 * 并把结果列表写入 *data。输入的位置列表必须
 * 已排序且无重复，输出也将如此。
 *
 * data：指向初始全零输出结构的指针（或 NULL）
 * Ldata, Rdata：输入位置列表
 * emit：TSPO_XXX 位掩码
 * Loffset：比较或输出前加到 Ldata 位置的偏移量
 * Roffset：比较或输出前加至 Rdata 位置的偏移量
 * max_npos：输出位置数组可能需要的最大大小
 *
 * Loffset 与 Roffset 不应为负，否则我们可能试图
 * 输出负位置值，其无法装入 WordEntryPos。
 *
 * 结果是布尔值（TS_YES 或 TS_NO），但为调用者方便，
 * 以 TSTernaryValue 返回。
 *
 * 若有任何位置被输出到 *data 则返回 TS_YES；若 data
 * 为 NULL，则应输出任何位置时返回 TS_YES。
 */
#define TSPO_L_ONLY		0x01	/* 输出仅出现在 L 中的位置 */
#define TSPO_R_ONLY		0x02	/* 输出仅在 R 中出现的位置 */
#define TSPO_BOTH		0x04	/* 输出 L、R 共有的位置 */

static TSTernaryValue
TS_phrase_output(ExecPhraseData *data,
				 ExecPhraseData *Ldata,
				 ExecPhraseData *Rdata,
				 int emit,
				 int Loffset,
				 int Roffset,
				 int max_npos)
{
	int			Lindex,
				Rindex;

	/* 循环直到两个输入都耗尽 */
	Lindex = Rindex = 0;
	while (Lindex < Ldata->npos || Rindex < Rdata->npos)
	{
		int			Lpos,
					Rpos;
		int			output_pos = 0;

		/*
		 * 取出要比较的当前值。需要 WEP_GETPOS()，因为
		 * ExecPhraseData->data 可能指向 tsvector WordEntryPosVector。
		 */
		if (Lindex < Ldata->npos)
			Lpos = WEP_GETPOS(Ldata->pos[Lindex]) + Loffset;
		else
		{
			/* L 数组耗尽，因此若 R_ONLY 未置位则结束 */
			if (!(emit & TSPO_R_ONLY))
				break;
			Lpos = INT_MAX;
		}
		if (Rindex < Rdata->npos)
			Rpos = WEP_GETPOS(Rdata->pos[Rindex]) + Roffset;
		else
		{
			/* R 数组耗尽，因此若 L_ONLY 未置位则结束 */
			if (!(emit & TSPO_L_ONLY))
				break;
			Rpos = INT_MAX;
		}

		/* 归并连接两个输入列表 */
		if (Lpos < Rpos)
		{
			/* Lpos 在 Rdata 中无匹配，是否输出它？ */
			if (emit & TSPO_L_ONLY)
				output_pos = Lpos;
			Lindex++;
		}
		else if (Lpos == Rpos)
		{
			/* Lpos 与 Rpos 匹配……是否输出它？ */
			if (emit & TSPO_BOTH)
				output_pos = Rpos;
			Lindex++;
			Rindex++;
		}
		else					/* Lpos > Rpos */
		{
			/* Rpos 在 Ldata 中无匹配，是否输出它？ */
			if (emit & TSPO_R_ONLY)
				output_pos = Rpos;
			Rindex++;
		}

		if (output_pos > 0)
		{
			if (data)
			{
				/* 存储位置，如有需要先分配输出数组 */
				if (data->pos == NULL)
				{
					data->pos = (WordEntryPos *)
						palloc(max_npos * sizeof(WordEntryPos));
					data->allocated = true;
				}
				data->pos[data->npos++] = output_pos;
			}
			else
			{
				/*
				 * 不需要精确位置，因此一旦知道至少有一个
				 * 即返回 TS_YES。
				 */
				return TS_YES;
			}
		}
	}

	if (data && data->npos > 0)
	{
		/* 让我们断言没有越出该数组 */
		Assert(data->npos <= max_npos);
		return TS_YES;
	}
	return TS_NO;
}

/*
 * 在 OP_PHRASE 算子处或以下执行 tsquery。
 *
 * 本函数处理需要关心匹配位置的递归层级上的
 * tsquery 执行。
 *
 * 除了 TS_execute 所用的相同参数外，调用者还可以
 * 传入一个预初始化为零的 ExecPhraseData 结构，成功时
 * 其中填入词位匹配位置信息；若无需位置数据则为
 * NULL。
 * 注意：OP_PHRASE 以外的算子，函数假定 data != NULL。
 * 这是可行的，因为外部调用总是从 OP_PHRASE 节点开始，
 * 而所有内部递归都传入 data != NULL。
 *
 * 在函数返回 TS_YES（成功匹配）的前提下，匹配数据
 * 的详细语义如下：
 *
 * npos > 0, negate = false:
 *	 查询在指定位置（且仅在这些位置）匹配
 * npos > 0, negate = true:
 *	 查询在除指定位置外的所有位置匹配
 * npos = 0, negate = true:
 *	 查询在所有位置匹配
 * npos = 0, negate = false:
 *	 不允许（此时应视情况得到 TS_NO 或 TS_MAYBE）
 *
 * 成功的匹配还会返回一个 "width" 值，即以词位计的
 * 匹配宽度减一。因此对于简单的单词位匹配，"width"
 * 为零；对于短语匹配，则是短语算子距离之和。注意
 * 当 width > 0 时，所列位置表示匹配的末端而非起点。
 * （这条不直观的规则是为避免可能生成负的位置值，
 * 负值无法装入 WordEntryPos 数组。）
 *
 * 若 TSExecuteCallback 函数报告某个操作数存在，
 * 但未能提供它的位置，则当查询有可能匹配但
 * 不确定时，我们将返回 TS_MAYBE。
 *
 * 当函数返回 TS_NO 或 TS_MAYBE 时，它必须返回
 * npos = 0, negate = false（即调用者初始化的状态）；
 * 此时 "width" 输出未定义。
 */
static TSTernaryValue
TS_phrase_execute(QueryItem *curitem, void *arg, uint32 flags,
				  TSExecuteCallback chkcond,
				  ExecPhraseData *data)
{
	ExecPhraseData Ldata,
				Rdata;
	TSTernaryValue lmatch,
				rmatch;
	int			Loffset,
				Roffset,
				maxwidth;

	/* 本函数会递归，因此可能被驱动到栈溢出 */
	check_stack_depth();

	/* 另外，顺便检查一下查询取消 */
	CHECK_FOR_INTERRUPTS();

	if (curitem->type == QI_VAL)
		return chkcond(arg, (QueryOperand *) curitem, data);

	switch (curitem->qoperator.oper)
	{
		case OP_NOT:

			/*
			 * 我们无需改动 data->width，因为 NOT 运算并不
			 * 改变匹配宽度。
			 */
			if (flags & TS_EXEC_SKIP_NOT)
			{
				/* 有 SKIP_NOT 时 NOT 报告为 "处处匹配" */
				Assert(data->npos == 0 && !data->negate);
				data->negate = true;
				return TS_YES;
			}
			switch (TS_phrase_execute(curitem + 1, arg, flags, chkcond, data))
			{
				case TS_NO:
					/* 把 "无处匹配" 改成 "处处匹配" */
					Assert(data->npos == 0 && !data->negate);
					data->negate = true;
					return TS_YES;
				case TS_YES:
					if (data->npos > 0)
					{
						/* 若有位置，则反转 negate 标志 */
						data->negate = !data->negate;
						return TS_YES;
					}
					else if (data->negate)
					{
						/* 把 "处处匹配" 改成 "无处匹配" */
						data->negate = false;
						return TS_NO;
					}
					/* 若结果为 TS_YES 不应到达此处 */
					Assert(false);
					break;
				case TS_MAYBE:
					/* 匹配位置不确定，且保持不确定 */
					return TS_MAYBE;
			}
			break;

		case OP_PHRASE:
		case OP_AND:
			memset(&Ldata, 0, sizeof(Ldata));
			memset(&Rdata, 0, sizeof(Rdata));

			lmatch = TS_phrase_execute(curitem + curitem->qoperator.left,
									   arg, flags, chkcond, &Ldata);
			if (lmatch == TS_NO)
				return TS_NO;

			rmatch = TS_phrase_execute(curitem + 1,
									   arg, flags, chkcond, &Rdata);
			if (rmatch == TS_NO)
				return TS_NO;

			/*
			 * 若任一操作数没有位置信息，我们就无法
			 * 返回可靠位置数据，只能返回 MAYBE。
			 */
			if (lmatch == TS_MAYBE || rmatch == TS_MAYBE)
				return TS_MAYBE;

			if (curitem->qoperator.oper == OP_PHRASE)
			{
				/*
				 * 计算适用于短语匹配的 Loffset 和 Roffset，
				 * 并计算整个短语匹配的总宽度。
				 */
				Loffset = curitem->qoperator.distance + Rdata.width;
				Roffset = 0;
				if (data)
					data->width = curitem->qoperator.distance +
						Ldata.width + Rdata.width;
			}
			else
			{
				/*
				 * 对于 OP_AND，同 OP_OR 一样设置宽度与对齐
				 * （见下）
				 */
				maxwidth = Max(Ldata.width, Rdata.width);
				Loffset = maxwidth - Ldata.width;
				Roffset = maxwidth - Rdata.width;
				if (data)
					data->width = maxwidth;
			}

			if (Ldata.negate && Rdata.negate)
			{
				/* !L & !R: 视为 !(L | R) */
				(void) TS_phrase_output(data, &Ldata, &Rdata,
										TSPO_BOTH | TSPO_L_ONLY | TSPO_R_ONLY,
										Loffset, Roffset,
										Ldata.npos + Rdata.npos);
				if (data)
					data->negate = true;
				return TS_YES;
			}
			else if (Ldata.negate)
			{
				/* !L & R */
				return TS_phrase_output(data, &Ldata, &Rdata,
										TSPO_R_ONLY,
										Loffset, Roffset,
										Rdata.npos);
			}
			else if (Rdata.negate)
			{
				/* L & !R */
				return TS_phrase_output(data, &Ldata, &Rdata,
										TSPO_L_ONLY,
										Loffset, Roffset,
										Ldata.npos);
			}
			else
			{
				/* 直接 AND */
				return TS_phrase_output(data, &Ldata, &Rdata,
										TSPO_BOTH,
										Loffset, Roffset,
										Min(Ldata.npos, Rdata.npos));
			}

		case OP_OR:
			memset(&Ldata, 0, sizeof(Ldata));
			memset(&Rdata, 0, sizeof(Rdata));

			lmatch = TS_phrase_execute(curitem + curitem->qoperator.left,
									   arg, flags, chkcond, &Ldata);
			rmatch = TS_phrase_execute(curitem + 1,
									   arg, flags, chkcond, &Rdata);

			if (lmatch == TS_NO && rmatch == TS_NO)
				return TS_NO;

			/*
			 * 若任一操作数没有位置信息，我们就无法
			 * 返回可靠位置数据，只能返回 MAYBE。
			 */
			if (lmatch == TS_MAYBE || rmatch == TS_MAYBE)
				return TS_MAYBE;

			/*
			 * 应对来自失败子匹配的未定义输出宽度。（这
			 * 比设法保证所有失败返回都将 data->width 置零
			 * 代码更少。）
			 */
			if (lmatch == TS_NO)
				Ldata.width = 0;
			if (rmatch == TS_NO)
				Rdata.width = 0;

			/*
			 * 对于 OP_AND 与 OP_OR，报告两者中较宽输入的
			 * 宽度，并把较窄输入的位置对齐到该宽度的
			 * 右端。这条规则至少能还算合理地应对
			 * "x <-> (y | z <-> q)" 这类情形。
			 */
			maxwidth = Max(Ldata.width, Rdata.width);
			Loffset = maxwidth - Ldata.width;
			Roffset = maxwidth - Rdata.width;
			data->width = maxwidth;

			if (Ldata.negate && Rdata.negate)
			{
				/* !L | !R: 视为 !(L & R) */
				(void) TS_phrase_output(data, &Ldata, &Rdata,
										TSPO_BOTH,
										Loffset, Roffset,
										Min(Ldata.npos, Rdata.npos));
				data->negate = true;
				return TS_YES;
			}
			else if (Ldata.negate)
			{
				/* !L | R: 视为 !(L & !R) */
				(void) TS_phrase_output(data, &Ldata, &Rdata,
										TSPO_L_ONLY,
										Loffset, Roffset,
										Ldata.npos);
				data->negate = true;
				return TS_YES;
			}
			else if (Rdata.negate)
			{
				/* L | !R: 视为 !(!L & R) */
				(void) TS_phrase_output(data, &Ldata, &Rdata,
										TSPO_R_ONLY,
										Loffset, Roffset,
										Rdata.npos);
				data->negate = true;
				return TS_YES;
			}
			else
			{
				/* 直接 OR */
				return TS_phrase_output(data, &Ldata, &Rdata,
										TSPO_BOTH | TSPO_L_ONLY | TSPO_R_ONLY,
										Loffset, Roffset,
										Ldata.npos + Rdata.npos);
			}

		default:
			elog(ERROR, "unrecognized operator: %d", curitem->qoperator.oper);
	}

	/* 不可达，但让编译器安静 */
	return TS_NO;
}


/*
 * 求值 tsquery 布尔表达式。
 *
 * curitem：当前 tsquery 条目（初始为第一个）
 * arg：传给回调函数的不透明值
 * flags：ts_utils.h 中标志位的位掩码
 * chkcond：检查某个基本值是否存在的回调函数
 */
bool
TS_execute(QueryItem *curitem, void *arg, uint32 flags,
		   TSExecuteCallback chkcond)
{
	/*
	 * 如果递归返回 TS_MAYBE，则返回真。只有当调用者
	 * 传入 TS_EXEC_PHRASE_NO_POS 时才会出现该结果，
	 * 无需再查。
	 */
	return TS_execute_recurse(curitem, arg, flags, chkcond) != TS_NO;
}

/*
 * 求值 tsquery 布尔表达式。
 *
 * 与 TS_execute 相同，只是 TS_MAYBE 原样返回。
 */
TSTernaryValue
TS_execute_ternary(QueryItem *curitem, void *arg, uint32 flags,
				   TSExecuteCallback chkcond)
{
	return TS_execute_recurse(curitem, arg, flags, chkcond);
}

/*
 * 用于任何短语算子之上算子的 TS_execute 递归。这里
 * 我们无需关心词位位置。一旦遇到 OP_PHRASE 算子，
 * 就把它交给会关心此事的 TS_phrase_execute。
 */
static TSTernaryValue
TS_execute_recurse(QueryItem *curitem, void *arg, uint32 flags,
				   TSExecuteCallback chkcond)
{
	TSTernaryValue lmatch;

	/* 本函数会递归，因此可能被驱动到栈溢出 */
	check_stack_depth();

	/* 另外，顺便检查一下查询取消 */
	CHECK_FOR_INTERRUPTS();

	if (curitem->type == QI_VAL)
		return chkcond(arg, (QueryOperand *) curitem,
					   NULL /* 不需要位置信息 */ );

	switch (curitem->qoperator.oper)
	{
		case OP_NOT:
			if (flags & TS_EXEC_SKIP_NOT)
				return TS_YES;
			switch (TS_execute_recurse(curitem + 1, arg, flags, chkcond))
			{
				case TS_NO:
					return TS_YES;
				case TS_YES:
					return TS_NO;
				case TS_MAYBE:
					return TS_MAYBE;
			}
			break;

		case OP_AND:
			lmatch = TS_execute_recurse(curitem + curitem->qoperator.left, arg,
										flags, chkcond);
			if (lmatch == TS_NO)
				return TS_NO;
			switch (TS_execute_recurse(curitem + 1, arg, flags, chkcond))
			{
				case TS_NO:
					return TS_NO;
				case TS_YES:
					return lmatch;
				case TS_MAYBE:
					return TS_MAYBE;
			}
			break;

		case OP_OR:
			lmatch = TS_execute_recurse(curitem + curitem->qoperator.left, arg,
										flags, chkcond);
			if (lmatch == TS_YES)
				return TS_YES;
			switch (TS_execute_recurse(curitem + 1, arg, flags, chkcond))
			{
				case TS_NO:
					return lmatch;
				case TS_YES:
					return TS_YES;
				case TS_MAYBE:
					return TS_MAYBE;
			}
			break;

		case OP_PHRASE:

			/*
			 * 若得到 MAYBE 结果而调用者不想要，
			 * 就把它转换为 NO。更一致的做法或许是原样
			 * 返回 TS_phrase_execute() 的结果，然后
			 * 在递归顶部转换 MAYBE 结果。但在最顶层
			 * 的短语算子处进行转换，所得结果与旧
			 * 实现 bug 兼容，所以目前就
			 * 这样做。
			 */
			switch (TS_phrase_execute(curitem, arg, flags, chkcond, NULL))
			{
				case TS_NO:
					return TS_NO;
				case TS_YES:
					return TS_YES;
				case TS_MAYBE:
					return (flags & TS_EXEC_PHRASE_NO_POS) ? TS_MAYBE : TS_NO;
			}
			break;

		default:
			elog(ERROR, "unrecognized operator: %d", curitem->qoperator.oper);
	}

	/* 不可达，但让编译器安静 */
	return TS_NO;
}

/*
 * 求值 tsquery 并报告匹配词的位置。
 *
 * 这与 TS_execute 类似，只是它返回匹配位置而不只是
 * 成功/失败状态。回调函数必须提供位置数据
 * （若不提供，我们将报告失败）。
 *
 * 匹配成功时，结果是一个 ExecPhraseData 结构列表，
 * 查询中每个 AND 连接的词项或短语算子各对应一个。
 * 每个结构含一个已排序的、匹配该词项的词位
 * 的位置数组。（回顾：短语算子的匹配含 width+1
 * 词位，记录的是最右词位。）
 *
 * OR 子表达式通过把它们的匹配位置并成一个列表
 * 元素来处理，这是可行的，因为其中任一位置都
 * 含有匹配。但当某些 OR 连接的词项是短语算子时，
 * 我们报告这些词项中的最大宽度，这使得此类情形
 * 在保守方向上略有失真。（例如，若 tsquery 为
 * "(A <-> B) | C"，数据中 C 的一次出现将被报告成
 * 仿佛它包含 C 左侧的词位。）
 *
 * NOT 子表达式的位置不予报告。（显然，顶层不可能
 * 存在成功的 NOT 匹配，否则整个匹配早已失败。
 * 所以这相当于忽略 OR 下方的 NOT。）
 *
 * 若无匹配，或未返回位置数据，则结果为 NIL。
 *
 * 参数与 TS_execute 相同，不过 flags 目前只是残留，
 * 因为已定义的各位在此处都无意义。
 */
List *
TS_execute_locations(QueryItem *curitem, void *arg,
					 uint32 flags,
					 TSExecuteCallback chkcond)
{
	List	   *result;

	/* 尚不支持任何标志 */
	Assert(flags == TS_EXEC_EMPTY);
	if (TS_execute_locations_recurse(curitem, arg, chkcond, &result))
		return result;
	return NIL;
}

/*
 * 用于短语算子之上算子的 TS_execute_locations 递归。
 * OP_PHRASE 子表达式可以交给 TS_phrase_execute 处理。
 */
static bool
TS_execute_locations_recurse(QueryItem *curitem, void *arg,
							 TSExecuteCallback chkcond,
							 List **locations)
{
	bool		lmatch,
				rmatch;
	List	   *llocations,
			   *rlocations;
	ExecPhraseData *data;

	/* 本函数会递归，因此可能被驱动到栈溢出 */
	check_stack_depth();

	/* 另外，顺便检查一下查询取消 */
	CHECK_FOR_INTERRUPTS();

	/* 默认位置结果为空 */
	*locations = NIL;

	if (curitem->type == QI_VAL)
	{
		data = palloc0_object(ExecPhraseData);
		if (chkcond(arg, (QueryOperand *) curitem, data) == TS_YES)
		{
			*locations = list_make1(data);
			return true;
		}
		pfree(data);
		return false;
	}

	switch (curitem->qoperator.oper)
	{
		case OP_NOT:
			if (!TS_execute_locations_recurse(curitem + 1, arg, chkcond,
											  &llocations))
				return true;	/* 我们不回传任何位置 */
			return false;

		case OP_AND:
			if (!TS_execute_locations_recurse(curitem + curitem->qoperator.left,
											  arg, chkcond,
											  &llocations))
				return false;
			if (!TS_execute_locations_recurse(curitem + 1,
											  arg, chkcond,
											  &rlocations))
				return false;
			*locations = list_concat(llocations, rlocations);
			return true;

		case OP_OR:
			lmatch = TS_execute_locations_recurse(curitem + curitem->qoperator.left,
												  arg, chkcond,
												  &llocations);
			rmatch = TS_execute_locations_recurse(curitem + 1,
												  arg, chkcond,
												  &rlocations);
			if (lmatch || rmatch)
			{
				/*
				 * 我们从子匹配的每种组合生成一个可
				 * 与之 AND 的位置结构，遵循析取律
				 * (A&B)|(C&D)=(A|C)&(A|D)&(B|C)&(B|D)。
				 *
				 * 不过，若任一输入未产生位置（即其失败
				 * 或为 NOT），就必须直接返回另一列表。
				 */
				if (llocations == NIL)
					*locations = rlocations;
				else if (rlocations == NIL)
					*locations = llocations;
				else
				{
					ListCell   *ll;

					foreach(ll, llocations)
					{
						ExecPhraseData *ldata = (ExecPhraseData *) lfirst(ll);
						ListCell   *lr;

						foreach(lr, rlocations)
						{
							ExecPhraseData *rdata = (ExecPhraseData *) lfirst(lr);

							data = palloc0_object(ExecPhraseData);
							(void) TS_phrase_output(data, ldata, rdata,
													TSPO_BOTH | TSPO_L_ONLY | TSPO_R_ONLY,
													0, 0,
													ldata->npos + rdata->npos);
							/* 如上所述，报告较大的宽度。 */
							data->width = Max(ldata->width, rdata->width);
							*locations = lappend(*locations, data);
						}
					}
				}

				return true;
			}
			return false;

		case OP_PHRASE:
			/* 可交给 TS_phrase_execute 处理 */
			data = palloc0_object(ExecPhraseData);
			if (TS_phrase_execute(curitem, arg, TS_EXEC_EMPTY, chkcond,
								  data) == TS_YES)
			{
				if (!data->negate)
					*locations = list_make1(data);
				return true;
			}
			pfree(data);
			return false;

		default:
			elog(ERROR, "unrecognized operator: %d", curitem->qoperator.oper);
	}

	/* 不可达，但让编译器安静 */
	return false;
}

/*
 * 检测 tsquery 布尔表达式是否需要对 tsquery 中所示
 * 的值有任何肯定匹配。
 *
 * 需要以此来判断 GIN 索引搜索是否需要全索引扫描。
 * 例如，'x & !y' 要求匹配 x，因此只需扫描 x 的
 * 条目；但 'x | !y' 可能匹配既不含 x 也不含 y 的行。
 */
bool
tsquery_requires_match(QueryItem *curitem)
{
	/* 本函数会递归，因此可能被驱动到栈溢出 */
	check_stack_depth();

	if (curitem->type == QI_VAL)
		return true;

	switch (curitem->qoperator.oper)
	{
		case OP_NOT:

			/*
			 * 假定 NOT 之下没有必需的匹配。对于某些
			 * 嵌套 NOT 的情形，我们本可以证明存在必需
			 * 匹配，但这似乎不值得费那个麻烦。
			 */
			return false;

		case OP_PHRASE:

			/*
			 * 此处视 OP_PHRASE 为 OP_AND
			 */
		case OP_AND:
			/* 若任一侧要求匹配，即为满足 */
			if (tsquery_requires_match(curitem + curitem->qoperator.left))
				return true;
			else
				return tsquery_requires_match(curitem + 1);

		case OP_OR:
			/* 两侧都必须要求匹配 */
			if (tsquery_requires_match(curitem + curitem->qoperator.left))
				return tsquery_requires_match(curitem + 1);
			else
				return false;

		default:
			elog(ERROR, "unrecognized operator: %d", curitem->qoperator.oper);
	}

	/* 不可达，但让编译器安静 */
	return false;
}

/*
 * 布尔运算
 */
Datum
ts_match_qv(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall2(ts_match_vq,
										PG_GETARG_DATUM(1),
										PG_GETARG_DATUM(0)));
}

Datum
ts_match_vq(PG_FUNCTION_ARGS)
{
	TSVector	val = PG_GETARG_TSVECTOR(0);
	TSQuery		query = PG_GETARG_TSQUERY(1);
	CHKVAL		chkval;
	bool		result;

	/* 空查询不匹配任何项 */
	if (!query->size)
	{
		PG_FREE_IF_COPY(val, 0);
		PG_FREE_IF_COPY(query, 1);
		PG_RETURN_BOOL(false);
	}

	chkval.arrb = ARRPTR(val);
	chkval.arre = chkval.arrb + val->size;
	chkval.values = STRPTR(val);
	chkval.operand = GETOPERAND(query);
	result = TS_execute(GETQUERY(query),
						&chkval,
						TS_EXEC_EMPTY,
						checkcondition_str);

	PG_FREE_IF_COPY(val, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(result);
}

Datum
ts_match_tt(PG_FUNCTION_ARGS)
{
	TSVector	vector;
	TSQuery		query;
	bool		res;

	vector = DatumGetTSVector(DirectFunctionCall1(to_tsvector,
												  PG_GETARG_DATUM(0)));
	query = DatumGetTSQuery(DirectFunctionCall1(plainto_tsquery,
												PG_GETARG_DATUM(1)));

	res = DatumGetBool(DirectFunctionCall2(ts_match_vq,
										   TSVectorGetDatum(vector),
										   TSQueryGetDatum(query)));

	pfree(vector);
	pfree(query);

	PG_RETURN_BOOL(res);
}

Datum
ts_match_tq(PG_FUNCTION_ARGS)
{
	TSVector	vector;
	TSQuery		query = PG_GETARG_TSQUERY(1);
	bool		res;

	vector = DatumGetTSVector(DirectFunctionCall1(to_tsvector,
												  PG_GETARG_DATUM(0)));

	res = DatumGetBool(DirectFunctionCall2(ts_match_vq,
										   TSVectorGetDatum(vector),
										   TSQueryGetDatum(query)));

	pfree(vector);
	PG_FREE_IF_COPY(query, 1);

	PG_RETURN_BOOL(res);
}

/*
 * ts_stat 统计函数支持
 */


/*
 * 返回 tsvector 'txt' 中值 'wptr' 的位置里，权重等于
 * 'weight' 位掩码中某个权重的位置个数。
 */
static int
check_weight(TSVector txt, WordEntry *wptr, int8 weight)
{
	int			len = POSDATALEN(txt, wptr);
	int			num = 0;
	WordEntryPos *ptr = POSDATAPTR(txt, wptr);

	while (len--)
	{
		if (weight & (1 << WEP_GETWEIGHT(*ptr)))
			num++;
		ptr++;
	}
	return num;
}

#define compareStatWord(a,e,t)							\
	tsCompareString((a)->lexeme, (a)->lenlexeme,		\
					STRPTR(t) + (e)->pos, (e)->len,		\
					false)

static void
insertStatEntry(MemoryContext persistentContext, TSVectorStat *stat, TSVector txt, uint32 off)
{
	WordEntry  *we = ARRPTR(txt) + off;
	StatEntry  *node = stat->root,
			   *pnode = NULL;
	int			n,
				res = 0;
	uint32		depth = 1;

	if (stat->weight == 0)
		n = (we->haspos) ? POSDATALEN(txt, we) : 1;
	else
		n = (we->haspos) ? check_weight(txt, we, stat->weight) : 0;

	if (n == 0)
		return;					/* 无可插入项 */

	while (node)
	{
		res = compareStatWord(node, we, txt);

		if (res == 0)
		{
			break;
		}
		else
		{
			pnode = node;
			node = (res < 0) ? node->left : node->right;
		}
		depth++;
	}

	if (depth > stat->maxdepth)
		stat->maxdepth = depth;

	if (node == NULL)
	{
		node = MemoryContextAlloc(persistentContext, STATENTRYHDRSZ + we->len);
		node->left = node->right = NULL;
		node->ndoc = 1;
		node->nentry = n;
		node->lenlexeme = we->len;
		memcpy(node->lexeme, STRPTR(txt) + we->pos, node->lenlexeme);

		if (pnode == NULL)
		{
			stat->root = node;
		}
		else
		{
			if (res < 0)
				pnode->left = node;
			else
				pnode->right = node;
		}
	}
	else
	{
		node->ndoc++;
		node->nentry += n;
	}
}

static void
chooseNextStatEntry(MemoryContext persistentContext, TSVectorStat *stat, TSVector txt,
					uint32 low, uint32 high, uint32 offset)
{
	uint32		pos;
	uint32		middle = (low + high) >> 1;

	pos = (low + middle) >> 1;
	if (low != middle && pos >= offset && pos - offset < txt->size)
		insertStatEntry(persistentContext, stat, txt, pos - offset);
	pos = (high + middle + 1) >> 1;
	if (middle + 1 != high && pos >= offset && pos - offset < txt->size)
		insertStatEntry(persistentContext, stat, txt, pos - offset);

	if (low != middle)
		chooseNextStatEntry(persistentContext, stat, txt, low, middle, offset);
	if (high != middle + 1)
		chooseNextStatEntry(persistentContext, stat, txt, middle + 1, high, offset);
}

/*
 * 本函数写成类似自定义聚集函数，因为最初
 * 的计划正是那样做。遗憾的是，聚集函数无法
 * 返回集合，所以那个计划被放弃了。如果将来
 * 这一限制被解除，ts_stat 可以成为真正的聚集函数，
 * 届时可这样使用：
 *
 *	 SELECT ts_stat(vector_column) FROM vector_table;
 *
 *	其中 vector_column 为 vector_table 的 tsvector 列。
 */

static TSVectorStat *
ts_accum(MemoryContext persistentContext, TSVectorStat *stat, Datum data)
{
	TSVector	txt = DatumGetTSVector(data);
	uint32		i,
				nbit = 0,
				offset;

	if (stat == NULL)
	{							/* 初始 */
		stat = MemoryContextAllocZero(persistentContext, sizeof(TSVectorStat));
		stat->maxdepth = 1;
	}

	/* 简单的正确性检查 */
	if (txt == NULL || txt->size == 0)
	{
		if (txt && txt != (TSVector) DatumGetPointer(data))
			pfree(txt);
		return stat;
	}

	i = txt->size - 1;
	for (; i > 0; i >>= 1)
		nbit++;

	nbit = 1 << nbit;
	offset = (nbit - txt->size) / 2;

	insertStatEntry(persistentContext, stat, txt, (nbit >> 1) - offset);
	chooseNextStatEntry(persistentContext, stat, txt, 0, nbit, offset);

	return stat;
}

static void
ts_setup_firstcall(FunctionCallInfo fcinfo, FuncCallContext *funcctx,
				   TSVectorStat *stat)
{
	TupleDesc	tupdesc;
	MemoryContext oldcontext;
	StatEntry  *node;

	funcctx->user_fctx = (void *) stat;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

	stat->stack = palloc0(sizeof(StatEntry *) * (stat->maxdepth + 1));
	stat->stackpos = 0;

	node = stat->root;
	/* 找最左边的值 */
	if (node == NULL)
		stat->stack[stat->stackpos] = NULL;
	else
		for (;;)
		{
			stat->stack[stat->stackpos] = node;
			if (node->left)
			{
				stat->stackpos++;
				node = node->left;
			}
			else
				break;
		}
	Assert(stat->stackpos <= stat->maxdepth);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	funcctx->tuple_desc = tupdesc;
	funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);

	MemoryContextSwitchTo(oldcontext);
}

static StatEntry *
walkStatEntryTree(TSVectorStat *stat)
{
	StatEntry  *node = stat->stack[stat->stackpos];

	if (node == NULL)
		return NULL;

	if (node->ndoc != 0)
	{
		/* 直接返回该条目：已位于左子链接 */
		return node;
	}
	else if (node->right && node->right != stat->stack[stat->stackpos + 1])
	{
		/* 转向右子链接 */
		stat->stackpos++;
		node = node->right;

		/* 找最左边的值 */
		for (;;)
		{
			stat->stack[stat->stackpos] = node;
			if (node->left)
			{
				stat->stackpos++;
				node = node->left;
			}
			else
				break;
		}
		Assert(stat->stackpos <= stat->maxdepth);
	}
	else
	{
		/* 我们已返回全部左子树、自身和右子树 */
		if (stat->stackpos == 0)
			return NULL;

		stat->stackpos--;
		return walkStatEntryTree(stat);
	}

	return node;
}

static Datum
ts_process_call(FuncCallContext *funcctx)
{
	TSVectorStat *st;
	StatEntry  *entry;

	st = (TSVectorStat *) funcctx->user_fctx;

	entry = walkStatEntryTree(st);

	if (entry != NULL)
	{
		Datum		result;
		char	   *values[3];
		char		ndoc[16];
		char		nentry[16];
		HeapTuple	tuple;

		values[0] = palloc(entry->lenlexeme + 1);
		memcpy(values[0], entry->lexeme, entry->lenlexeme);
		(values[0])[entry->lenlexeme] = '\0';
		sprintf(ndoc, "%d", entry->ndoc);
		values[1] = ndoc;
		sprintf(nentry, "%d", entry->nentry);
		values[2] = nentry;

		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);
		result = HeapTupleGetDatum(tuple);

		pfree(values[0]);

		/* 标记该条目已访问 */
		entry->ndoc = 0;

		return result;
	}

	return (Datum) 0;
}

static TSVectorStat *
ts_stat_sql(MemoryContext persistentContext, text *txt, text *ws)
{
	char	   *query = text_to_cstring(txt);
	TSVectorStat *stat;
	bool		isnull;
	Portal		portal;
	SPIPlanPtr	plan;

	if ((plan = SPI_prepare(query, 0, NULL)) == NULL)
		/* 内部错误 */
		elog(ERROR, "SPI_prepare(\"%s\") failed", query);

	if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, true)) == NULL)
		/* 内部错误 */
		elog(ERROR, "SPI_cursor_open(\"%s\") failed", query);

	SPI_cursor_fetch(portal, true, 100);

	if (SPI_tuptable == NULL ||
		SPI_tuptable->tupdesc->natts != 1 ||
		!IsBinaryCoercible(SPI_gettypeid(SPI_tuptable->tupdesc, 1),
						   TSVECTOROID))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ts_stat query must return one tsvector column")));

	stat = MemoryContextAllocZero(persistentContext, sizeof(TSVectorStat));
	stat->maxdepth = 1;

	if (ws)
	{
		char	   *buf;

		buf = VARDATA_ANY(ws);
		while (buf - VARDATA_ANY(ws) < VARSIZE_ANY_EXHDR(ws))
		{
			if (pg_mblen(buf) == 1)
			{
				switch (*buf)
				{
					case 'A':
					case 'a':
						stat->weight |= 1 << 3;
						break;
					case 'B':
					case 'b':
						stat->weight |= 1 << 2;
						break;
					case 'C':
					case 'c':
						stat->weight |= 1 << 1;
						break;
					case 'D':
					case 'd':
						stat->weight |= 1;
						break;
					default:
						stat->weight |= 0;
				}
			}
			buf += pg_mblen(buf);
		}
	}

	while (SPI_processed > 0)
	{
		uint64		i;

		for (i = 0; i < SPI_processed; i++)
		{
			Datum		data = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);

			if (!isnull)
				stat = ts_accum(persistentContext, stat, data);
		}

		SPI_freetuptable(SPI_tuptable);
		SPI_cursor_fetch(portal, true, 100);
	}

	SPI_freetuptable(SPI_tuptable);
	SPI_cursor_close(portal);
	SPI_freeplan(plan);
	pfree(query);

	return stat;
}

Datum
ts_stat1(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Datum		result;

	if (SRF_IS_FIRSTCALL())
	{
		TSVectorStat *stat;
		text	   *txt = PG_GETARG_TEXT_PP(0);

		funcctx = SRF_FIRSTCALL_INIT();
		SPI_connect();
		stat = ts_stat_sql(funcctx->multi_call_memory_ctx, txt, NULL);
		PG_FREE_IF_COPY(txt, 0);
		ts_setup_firstcall(fcinfo, funcctx, stat);
		SPI_finish();
	}

	funcctx = SRF_PERCALL_SETUP();
	if ((result = ts_process_call(funcctx)) != (Datum) 0)
		SRF_RETURN_NEXT(funcctx, result);
	SRF_RETURN_DONE(funcctx);
}

Datum
ts_stat2(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Datum		result;

	if (SRF_IS_FIRSTCALL())
	{
		TSVectorStat *stat;
		text	   *txt = PG_GETARG_TEXT_PP(0);
		text	   *ws = PG_GETARG_TEXT_PP(1);

		funcctx = SRF_FIRSTCALL_INIT();
		SPI_connect();
		stat = ts_stat_sql(funcctx->multi_call_memory_ctx, txt, ws);
		PG_FREE_IF_COPY(txt, 0);
		PG_FREE_IF_COPY(ws, 1);
		ts_setup_firstcall(fcinfo, funcctx, stat);
		SPI_finish();
	}

	funcctx = SRF_PERCALL_SETUP();
	if ((result = ts_process_call(funcctx)) != (Datum) 0)
		SRF_RETURN_NEXT(funcctx, result);
	SRF_RETURN_DONE(funcctx);
}


/*
 * 用文本列自动更新 tsvector 列的触发器
 *
 * 参数为以下两种之一
 *		tsvector 列名、所用 tsconfig 的名字、文本列的名字
 *		tsvector 列名、regconfig 列名、文本列的名字
 * 即 tsconfig 既可以通过名字指定，也可以间接地以
 * 行中 regconfig 字段的内容指定。若使用名字，
 * 则必须显式限定模式。
 */
Datum
tsvector_update_trigger_byid(PG_FUNCTION_ARGS)
{
	return tsvector_update_trigger(fcinfo, false);
}

Datum
tsvector_update_trigger_bycolumn(PG_FUNCTION_ARGS)
{
	return tsvector_update_trigger(fcinfo, true);
}

static Datum
tsvector_update_trigger(PG_FUNCTION_ARGS, bool config_column)
{
	TriggerData *trigdata;
	Trigger    *trigger;
	Relation	rel;
	HeapTuple	rettuple = NULL;
	int			tsvector_attr_num,
				i;
	ParsedText	prs;
	Datum		datum;
	bool		isnull;
	text	   *txt;
	Oid			cfgId;
	bool		update_needed;

	/* 查调用上下文 */
	if (!CALLED_AS_TRIGGER(fcinfo)) /* 内部错误 */
		elog(ERROR, "tsvector_update_trigger: not fired by trigger manager");

	trigdata = (TriggerData *) fcinfo->context;
	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		elog(ERROR, "tsvector_update_trigger: must be fired for row");
	if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		elog(ERROR, "tsvector_update_trigger: must be fired BEFORE event");

	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
	{
		rettuple = trigdata->tg_trigtuple;
		update_needed = true;
	}
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
	{
		rettuple = trigdata->tg_newtuple;
		update_needed = false;	/* 下方计算 */
	}
	else
		elog(ERROR, "tsvector_update_trigger: must be fired for INSERT or UPDATE");

	trigger = trigdata->tg_trigger;
	rel = trigdata->tg_relation;

	if (trigger->tgnargs < 3)
		elog(ERROR, "tsvector_update_trigger: arguments must be tsvector_field, ts_config, text_field1, ...)");

	/* 找出目标 tsvector 列 */
	tsvector_attr_num = SPI_fnumber(rel->rd_att, trigger->tgargs[0]);
	if (tsvector_attr_num == SPI_ERROR_NOATTRIBUTE)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("tsvector column \"%s\" does not exist",
						trigger->tgargs[0])));
	/* 这实际上会拒绝系统列，因此无需单独测试： */
	if (!IsBinaryCoercible(SPI_gettypeid(rel->rd_att, tsvector_attr_num),
						   TSVECTOROID))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" is not of tsvector type",
						trigger->tgargs[0])));

	/* 找出要使用的配置 */
	if (config_column)
	{
		int			config_attr_num;

		config_attr_num = SPI_fnumber(rel->rd_att, trigger->tgargs[1]);
		if (config_attr_num == SPI_ERROR_NOATTRIBUTE)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("configuration column \"%s\" does not exist",
							trigger->tgargs[1])));
		if (!IsBinaryCoercible(SPI_gettypeid(rel->rd_att, config_attr_num),
							   REGCONFIGOID))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" is not of regconfig type",
							trigger->tgargs[1])));

		datum = SPI_getbinval(rettuple, rel->rd_att, config_attr_num, &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("configuration column \"%s\" must not be null",
							trigger->tgargs[1])));
		cfgId = DatumGetObjectId(datum);
	}
	else
	{
		List	   *names;

		names = stringToQualifiedNameList(trigger->tgargs[1], NULL);
		/* 需要带模式，使结果不依赖于搜索路径 */
		if (list_length(names) < 2)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("text search configuration name \"%s\" must be schema-qualified",
							trigger->tgargs[1])));
		cfgId = get_ts_config_oid(names, false);
	}

	/* 初始化解析状态 */
	prs.lenwords = 32;
	prs.curwords = 0;
	prs.pos = 0;
	prs.words = (ParsedWord *) palloc(sizeof(ParsedWord) * prs.lenwords);

	/* 在可索引列中找出所有词 */
	for (i = 2; i < trigger->tgnargs; i++)
	{
		int			numattr;

		numattr = SPI_fnumber(rel->rd_att, trigger->tgargs[i]);
		if (numattr == SPI_ERROR_NOATTRIBUTE)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist",
							trigger->tgargs[i])));
		if (!IsBinaryCoercible(SPI_gettypeid(rel->rd_att, numattr), TEXTOID))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" is not of a character type",
							trigger->tgargs[i])));

		if (bms_is_member(numattr - FirstLowInvalidHeapAttributeNumber, trigdata->tg_updatedcols))
			update_needed = true;

		datum = SPI_getbinval(rettuple, rel->rd_att, numattr, &isnull);
		if (isnull)
			continue;

		txt = DatumGetTextPP(datum);

		parsetext(cfgId, &prs, VARDATA_ANY(txt), VARSIZE_ANY_EXHDR(txt));

		if (txt != (text *) DatumGetPointer(datum))
			pfree(txt);
	}

	if (update_needed)
	{
		/* 构造 tsvector 值 */
		datum = TSVectorGetDatum(make_tsvector(&prs));
		isnull = false;

		/* 并把它插入元组 */
		rettuple = heap_modify_tuple_by_cols(rettuple, rel->rd_att,
											 1, &tsvector_attr_num,
											 &datum, &isnull);

		pfree(DatumGetPointer(datum));
	}

	return PointerGetDatum(rettuple);
}
