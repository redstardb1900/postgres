/*-------------------------------------------------------------------------
 *
 * to_tsany.c
 *		to_ts* function definitions
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/to_tsany.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/jsonapi.h"
#include "tsearch/ts_cache.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/jsonfuncs.h"


/*
 * 不透明结构,由 parse_tsquery() 传给 pushval_morph()。
 */
typedef struct MorphOpaque
{
	Oid			cfg_id;

	/*
	 * 单个 tsquery 词形可解析为多词。这些词若处于相邻
	 * 位置,则用此运算符连接。通常即 OP_PHRASE,它要求
	 * 复合词形的词位置与 tsvector 精确
	 * 匹配。
	 */
	int			qoperator;
} MorphOpaque;

typedef struct TSVectorBuildState
{
	ParsedText *prs;
	Oid			cfgId;
} TSVectorBuildState;

static void add_to_tsvector(void *_state, char *elem_value, int elem_len);


Datum
get_current_ts_config(PG_FUNCTION_ARGS)
{
	PG_RETURN_OID(getTSCurrentConfig(true));
}

/*
 * to_tsvector
 */
static int
compareWORD(const void *a, const void *b)
{
	int			res;

	res = tsCompareString(((const ParsedWord *) a)->word, ((const ParsedWord *) a)->len,
						  ((const ParsedWord *) b)->word, ((const ParsedWord *) b)->len,
						  false);

	if (res == 0)
	{
		if (((const ParsedWord *) a)->pos.pos == ((const ParsedWord *) b)->pos.pos)
			return 0;

		res = (((const ParsedWord *) a)->pos.pos > ((const ParsedWord *) b)->pos.pos) ? 1 : -1;
	}

	return res;
}

static int
uniqueWORD(ParsedWord *a, int32 l)
{
	ParsedWord *ptr,
			   *res;
	int			tmppos;

	if (l == 1)
	{
		tmppos = LIMITPOS(a->pos.pos);
		a->alen = 2;
		a->pos.apos = (uint16 *) palloc(sizeof(uint16) * a->alen);
		a->pos.apos[0] = 1;
		a->pos.apos[1] = tmppos;
		return l;
	}

	res = a;
	ptr = a + 1;

	/*
	 * 按词及其位置排序
	 */
	qsort(a, l, sizeof(ParsedWord), compareWORD);

	/*
	 * 初始化首词及其首个位置
	 */
	tmppos = LIMITPOS(a->pos.pos);
	a->alen = 2;
	a->pos.apos = (uint16 *) palloc(sizeof(uint16) * a->alen);
	a->pos.apos[0] = 1;
	a->pos.apos[1] = tmppos;

	/*
	 * 汇总每个词的位置信息
	 */
	while (ptr - a < l)
	{
		if (!(ptr->len == res->len &&
			  strncmp(ptr->word, res->word, res->len) == 0))
		{
			/*
			 * 遇到新词,写入结果
			 */
			res++;
			res->len = ptr->len;
			res->word = ptr->word;
			tmppos = LIMITPOS(ptr->pos.pos);
			res->alen = 2;
			res->pos.apos = (uint16 *) palloc(sizeof(uint16) * res->alen);
			res->pos.apos[0] = 1;
			res->pos.apos[1] = tmppos;
		}
		else
		{
			/*
			 * 词已存在,调整位置信息。但先应检查
			 * 位置数组大小、位置允许最大值
			 * 以及位置唯一性
			 */
			pfree(ptr->word);
			if (res->pos.apos[0] < MAXNUMPOS - 1 && res->pos.apos[res->pos.apos[0]] != MAXENTRYPOS - 1 &&
				res->pos.apos[res->pos.apos[0]] != LIMITPOS(ptr->pos.pos))
			{
				if (res->pos.apos[0] + 1 >= res->alen)
				{
					res->alen *= 2;
					res->pos.apos = (uint16 *) repalloc(res->pos.apos, sizeof(uint16) * res->alen);
				}
				if (res->pos.apos[0] == 0 || res->pos.apos[res->pos.apos[0]] != LIMITPOS(ptr->pos.pos))
				{
					res->pos.apos[res->pos.apos[0] + 1] = LIMITPOS(ptr->pos.pos);
					res->pos.apos[0]++;
				}
			}
		}
		ptr++;
	}

	return res + 1 - a;
}

/*
 * 由已解析文本构造 tsvector 值
 *
 * 注意:释放 prs->words 及附属数据。
 */
TSVector
make_tsvector(ParsedText *prs)
{
	int			i,
				j,
				lenstr = 0,
				totallen;
	TSVector	in;
	WordEntry  *ptr;
	char	   *str;
	int			stroff;

	/* 合并重复词 */
	if (prs->curwords > 0)
		prs->curwords = uniqueWORD(prs->words, prs->curwords);

	/* 确定所需空间 */
	for (i = 0; i < prs->curwords; i++)
	{
		lenstr += prs->words[i].len;
		if (prs->words[i].alen)
		{
			lenstr = SHORTALIGN(lenstr);
			lenstr += sizeof(uint16) + prs->words[i].pos.apos[0] * sizeof(WordEntryPos);
		}
	}

	if (lenstr > MAXSTRPOS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("string is too long for tsvector (%d bytes, max %d bytes)", lenstr, MAXSTRPOS)));

	totallen = CALCDATASIZE(prs->curwords, lenstr);
	in = (TSVector) palloc0(totallen);
	SET_VARSIZE(in, totallen);
	in->size = prs->curwords;

	ptr = ARRPTR(in);
	str = STRPTR(in);
	stroff = 0;
	for (i = 0; i < prs->curwords; i++)
	{
		ptr->len = prs->words[i].len;
		ptr->pos = stroff;
		memcpy(str + stroff, prs->words[i].word, prs->words[i].len);
		stroff += prs->words[i].len;
		pfree(prs->words[i].word);
		if (prs->words[i].alen)
		{
			int			k = prs->words[i].pos.apos[0];
			WordEntryPos *wptr;

			if (k > 0xFFFF)
				elog(ERROR, "positions array too long");

			ptr->haspos = 1;
			stroff = SHORTALIGN(stroff);
			*(uint16 *) (str + stroff) = (uint16) k;
			wptr = POSDATAPTR(in, ptr);
			for (j = 0; j < k; j++)
			{
				WEP_SETWEIGHT(wptr[j], 0);
				WEP_SETPOS(wptr[j], prs->words[i].pos.apos[j + 1]);
			}
			stroff += sizeof(uint16) + k * sizeof(WordEntryPos);
			pfree(prs->words[i].pos.apos);
		}
		else
			ptr->haspos = 0;
		ptr++;
	}

	if (prs->words)
		pfree(prs->words);

	return in;
}

Datum
to_tsvector_byid(PG_FUNCTION_ARGS)
{
	Oid			cfgId = PG_GETARG_OID(0);
	text	   *in = PG_GETARG_TEXT_PP(1);
	ParsedText	prs;
	TSVector	out;

	prs.lenwords = VARSIZE_ANY_EXHDR(in) / 6;	/* 仅粗估
												 * 词数 */
	if (prs.lenwords < 2)
		prs.lenwords = 2;
	else if (prs.lenwords > MaxAllocSize / sizeof(ParsedWord))
		prs.lenwords = MaxAllocSize / sizeof(ParsedWord);
	prs.curwords = 0;
	prs.pos = 0;
	prs.words = (ParsedWord *) palloc(sizeof(ParsedWord) * prs.lenwords);

	parsetext(cfgId, &prs, VARDATA_ANY(in), VARSIZE_ANY_EXHDR(in));

	PG_FREE_IF_COPY(in, 1);

	out = make_tsvector(&prs);

	PG_RETURN_TSVECTOR(out);
}

Datum
to_tsvector(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_PP(0);
	Oid			cfgId;

	cfgId = getTSCurrentConfig(true);
	PG_RETURN_DATUM(DirectFunctionCall2(to_tsvector_byid,
										ObjectIdGetDatum(cfgId),
										PointerGetDatum(in)));
}

/*
 * jsonb(_string)_to_tsvector(_byid) 的工作函数
 */
static TSVector
jsonb_to_tsvector_worker(Oid cfgId, Jsonb *jb, uint32 flags)
{
	TSVectorBuildState state;
	ParsedText	prs;

	prs.words = NULL;
	prs.curwords = 0;
	state.prs = &prs;
	state.cfgId = cfgId;

	iterate_jsonb_values(jb, flags, &state, add_to_tsvector);

	return make_tsvector(&prs);
}

Datum
jsonb_string_to_tsvector_byid(PG_FUNCTION_ARGS)
{
	Oid			cfgId = PG_GETARG_OID(0);
	Jsonb	   *jb = PG_GETARG_JSONB_P(1);
	TSVector	result;

	result = jsonb_to_tsvector_worker(cfgId, jb, jtiString);
	PG_FREE_IF_COPY(jb, 1);

	PG_RETURN_TSVECTOR(result);
}

Datum
jsonb_string_to_tsvector(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	Oid			cfgId;
	TSVector	result;

	cfgId = getTSCurrentConfig(true);
	result = jsonb_to_tsvector_worker(cfgId, jb, jtiString);
	PG_FREE_IF_COPY(jb, 0);

	PG_RETURN_TSVECTOR(result);
}

Datum
jsonb_to_tsvector_byid(PG_FUNCTION_ARGS)
{
	Oid			cfgId = PG_GETARG_OID(0);
	Jsonb	   *jb = PG_GETARG_JSONB_P(1);
	Jsonb	   *jbFlags = PG_GETARG_JSONB_P(2);
	TSVector	result;
	uint32		flags = parse_jsonb_index_flags(jbFlags);

	result = jsonb_to_tsvector_worker(cfgId, jb, flags);
	PG_FREE_IF_COPY(jb, 1);
	PG_FREE_IF_COPY(jbFlags, 2);

	PG_RETURN_TSVECTOR(result);
}

Datum
jsonb_to_tsvector(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbFlags = PG_GETARG_JSONB_P(1);
	Oid			cfgId;
	TSVector	result;
	uint32		flags = parse_jsonb_index_flags(jbFlags);

	cfgId = getTSCurrentConfig(true);
	result = jsonb_to_tsvector_worker(cfgId, jb, flags);
	PG_FREE_IF_COPY(jb, 0);
	PG_FREE_IF_COPY(jbFlags, 1);

	PG_RETURN_TSVECTOR(result);
}

/*
 * json(_string)_to_tsvector(_byid) 的工作函数
 */
static TSVector
json_to_tsvector_worker(Oid cfgId, text *json, uint32 flags)
{
	TSVectorBuildState state;
	ParsedText	prs;

	prs.words = NULL;
	prs.curwords = 0;
	state.prs = &prs;
	state.cfgId = cfgId;

	iterate_json_values(json, flags, &state, add_to_tsvector);

	return make_tsvector(&prs);
}

Datum
json_string_to_tsvector_byid(PG_FUNCTION_ARGS)
{
	Oid			cfgId = PG_GETARG_OID(0);
	text	   *json = PG_GETARG_TEXT_P(1);
	TSVector	result;

	result = json_to_tsvector_worker(cfgId, json, jtiString);
	PG_FREE_IF_COPY(json, 1);

	PG_RETURN_TSVECTOR(result);
}

Datum
json_string_to_tsvector(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	Oid			cfgId;
	TSVector	result;

	cfgId = getTSCurrentConfig(true);
	result = json_to_tsvector_worker(cfgId, json, jtiString);
	PG_FREE_IF_COPY(json, 0);

	PG_RETURN_TSVECTOR(result);
}

Datum
json_to_tsvector_byid(PG_FUNCTION_ARGS)
{
	Oid			cfgId = PG_GETARG_OID(0);
	text	   *json = PG_GETARG_TEXT_P(1);
	Jsonb	   *jbFlags = PG_GETARG_JSONB_P(2);
	TSVector	result;
	uint32		flags = parse_jsonb_index_flags(jbFlags);

	result = json_to_tsvector_worker(cfgId, json, flags);
	PG_FREE_IF_COPY(json, 1);
	PG_FREE_IF_COPY(jbFlags, 2);

	PG_RETURN_TSVECTOR(result);
}

Datum
json_to_tsvector(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	Jsonb	   *jbFlags = PG_GETARG_JSONB_P(1);
	Oid			cfgId;
	TSVector	result;
	uint32		flags = parse_jsonb_index_flags(jbFlags);

	cfgId = getTSCurrentConfig(true);
	result = json_to_tsvector_worker(cfgId, json, flags);
	PG_FREE_IF_COPY(json, 0);
	PG_FREE_IF_COPY(jbFlags, 1);

	PG_RETURN_TSVECTOR(result);
}

/*
 * 解析 json(b) 元素中的词元,加入 TSVectorBuildState。
 */
static void
add_to_tsvector(void *_state, char *elem_value, int elem_len)
{
	TSVectorBuildState *state = (TSVectorBuildState *) _state;
	ParsedText *prs = state->prs;
	int32		prevwords;

	if (prs->words == NULL)
	{
		/*
		 * 首次进入:把 words 数组初始化为合理大小。
		 * (parsetext() 会按需再 realloc 放大。)
		 */
		prs->lenwords = 16;
		prs->words = (ParsedWord *) palloc(sizeof(ParsedWord) * prs->lenwords);
		prs->curwords = 0;
		prs->pos = 0;
	}

	prevwords = prs->curwords;

	parsetext(state->cfgId, prs, elem_value, elem_len);

	/*
	 * 若本 JSON 元素抽出了词,则推进 pos,在元素间
	 * 制造人为间断。因不想让短语搜索以为本元素
	 * 末词与下一元素首词
	 * 相邻。
	 */
	if (prs->curwords > prevwords)
		prs->pos += 1;
}


/*
 * to_tsquery
 */


/*
 * 本函数用于词形解析。
 *
 * 值交给 parsetext,由其调用合适词典做
 * 词元化。若为停用词,则把 QI_VALSTOP
 * 压栈。
 *
 * 同一切分变体的词以 AND 列表压栈,
 * 不同变体之间 OR 连接。
 */
static void
pushval_morph(Datum opaque, TSQueryParserState state, char *strval, int lenval, int16 weight, bool prefix)
{
	int32		count = 0;
	ParsedText	prs;
	uint32		variant,
				pos = 0,
				cntvar = 0,
				cntpos = 0,
				cnt = 0;
	MorphOpaque *data = (MorphOpaque *) DatumGetPointer(opaque);

	prs.lenwords = 4;
	prs.curwords = 0;
	prs.pos = 0;
	prs.words = (ParsedWord *) palloc(sizeof(ParsedWord) * prs.lenwords);

	parsetext(data->cfg_id, &prs, strval, lenval);

	if (prs.curwords > 0)
	{
		while (count < prs.curwords)
		{
			/*
			 * 是否去掉了停用词?若是,用适当运算符
			 * 连接的占位符填补空位。
			 */
			if (pos > 0 && pos + 1 < prs.words[count].pos.pos)
			{
				while (pos + 1 < prs.words[count].pos.pos)
				{
					/* 为每个缺失停用词放占位符 */
					pushStop(state);
					if (cntpos)
						pushOperator(state, data->qoperator, 1);
					cntpos++;
					pos++;
				}
			}

			/* 保存当前词位置 */
			pos = prs.words[count].pos.pos;

			/* 遍历本词元得到的所有变体 */
			cntvar = 0;
			while (count < prs.curwords && pos == prs.words[count].pos.pos)
			{
				variant = prs.words[count].nvariant;

				/* 压入同一切分变体的全部词 */
				cnt = 0;
				while (count < prs.curwords &&
					   pos == prs.words[count].pos.pos &&
					   variant == prs.words[count].nvariant)
				{
					pushValue(state,
							  prs.words[count].word,
							  prs.words[count].len,
							  weight,
							  ((prs.words[count].flags & TSL_PREFIX) || prefix));
					pfree(prs.words[count].word);
					if (cnt)
						pushOperator(state, OP_AND, 0);
					cnt++;
					count++;
				}

				if (cntvar)
					pushOperator(state, OP_OR, 0);
				cntvar++;
			}

			if (cntpos)
			{
				/* 距离或有用 */
				pushOperator(state, data->qoperator, 1);
			}

			cntpos++;
		}

		pfree(prs.words);
	}
	else
		pushStop(state);
}

Datum
to_tsquery_byid(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_PP(1);
	TSQuery		query;
	MorphOpaque data;

	data.cfg_id = PG_GETARG_OID(0);

	/*
	 * 将 OP_PHRASE 作 qoperator 时,tsquery 要求复合词形
	 * 的词位置与 tsvector 精确匹配。另当复合词形经
	 * OP_PHRASE 相连时,把其词全部连入 OP_PHRASE
	 * 序列。
	 */
	data.qoperator = OP_PHRASE;

	query = parse_tsquery(text_to_cstring(in),
						  pushval_morph,
						  PointerGetDatum(&data),
						  0,
						  NULL);

	PG_RETURN_TSQUERY(query);
}

Datum
to_tsquery(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_PP(0);
	Oid			cfgId;

	cfgId = getTSCurrentConfig(true);
	PG_RETURN_DATUM(DirectFunctionCall2(to_tsquery_byid,
										ObjectIdGetDatum(cfgId),
										PointerGetDatum(in)));
}

Datum
plainto_tsquery_byid(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_PP(1);
	TSQuery		query;
	MorphOpaque data;

	data.cfg_id = PG_GETARG_OID(0);

	/*
	 * parse_tsquery() 带 P_TSQ_PLAIN 时把整段输入视作
	 * 单个词形。将 OP_PHRASE 作 qoperator 时,tsquery
	 * 要求匹配全部词,而不论其位置。
	 */
	data.qoperator = OP_AND;

	query = parse_tsquery(text_to_cstring(in),
						  pushval_morph,
						  PointerGetDatum(&data),
						  P_TSQ_PLAIN,
						  NULL);

	PG_RETURN_POINTER(query);
}

Datum
plainto_tsquery(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_PP(0);
	Oid			cfgId;

	cfgId = getTSCurrentConfig(true);
	PG_RETURN_DATUM(DirectFunctionCall2(plainto_tsquery_byid,
										ObjectIdGetDatum(cfgId),
										PointerGetDatum(in)));
}


Datum
phraseto_tsquery_byid(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_PP(1);
	TSQuery		query;
	MorphOpaque data;

	data.cfg_id = PG_GETARG_OID(0);

	/*
	 * parse_tsquery() 带 P_TSQ_PLAIN 时把整段输入视作
	 * 单个词形。将 OP_PHRASE 作 qoperator 时,tsquery
	 * 要求匹配词位置。
	 */
	data.qoperator = OP_PHRASE;

	query = parse_tsquery(text_to_cstring(in),
						  pushval_morph,
						  PointerGetDatum(&data),
						  P_TSQ_PLAIN,
						  NULL);

	PG_RETURN_TSQUERY(query);
}

Datum
phraseto_tsquery(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_PP(0);
	Oid			cfgId;

	cfgId = getTSCurrentConfig(true);
	PG_RETURN_DATUM(DirectFunctionCall2(phraseto_tsquery_byid,
										ObjectIdGetDatum(cfgId),
										PointerGetDatum(in)));
}

Datum
websearch_to_tsquery_byid(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_PP(1);
	MorphOpaque data;
	TSQuery		query = NULL;

	data.cfg_id = PG_GETARG_OID(0);

	/*
	 * 将 OP_PHRASE 作 qoperator 时,tsquery 要求复合词形
	 * 的词位置与 tsvector 精确匹配。另当复合词形写在
	 * 引号内时,把其词全部连入 OP_PHRASE
	 * 序列。
	 */
	data.qoperator = OP_PHRASE;

	query = parse_tsquery(text_to_cstring(in),
						  pushval_morph,
						  PointerGetDatum(&data),
						  P_TSQ_WEB,
						  NULL);

	PG_RETURN_TSQUERY(query);
}

Datum
websearch_to_tsquery(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_PP(0);
	Oid			cfgId;

	cfgId = getTSCurrentConfig(true);
	PG_RETURN_DATUM(DirectFunctionCall2(websearch_to_tsquery_byid,
										ObjectIdGetDatum(cfgId),
										PointerGetDatum(in)));
}
