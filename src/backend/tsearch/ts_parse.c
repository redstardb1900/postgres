/*-------------------------------------------------------------------------
 *
 * ts_parse.c
 *		main parse functions for tsearch
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/ts_parse.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "tsearch/ts_cache.h"
#include "tsearch/ts_utils.h"
#include "varatt.h"

#define IGNORE_LONGLEXEME	1

/*
 * 词元子系统
 */

typedef struct ParsedLex
{
	int			type;
	char	   *lemm;
	int			lenlemm;
	struct ParsedLex *next;
} ParsedLex;

typedef struct ListParsedLex
{
	ParsedLex  *head;
	ParsedLex  *tail;
} ListParsedLex;

typedef struct
{
	TSConfigCacheEntry *cfg;
	Oid			curDictId;
	int			posDict;
	DictSubState dictState;
	ParsedLex  *curSub;
	ListParsedLex towork;		/* 待处理列表 */
	ListParsedLex waste;		/* 已词元化的词元列表 */

	/*
	 * 存放上次待词元化变体的字段(基本为同义词典或
	 * 类似、需要多个词元者)
	 */

	ParsedLex  *lastRes;
	TSLexeme   *tmpRes;
} LexizeData;

static void
LexizeInit(LexizeData *ld, TSConfigCacheEntry *cfg)
{
	ld->cfg = cfg;
	ld->curDictId = InvalidOid;
	ld->posDict = 0;
	ld->towork.head = ld->towork.tail = ld->curSub = NULL;
	ld->waste.head = ld->waste.tail = NULL;
	ld->lastRes = NULL;
	ld->tmpRes = NULL;
}

static void
LPLAddTail(ListParsedLex *list, ParsedLex *newpl)
{
	if (list->tail)
	{
		list->tail->next = newpl;
		list->tail = newpl;
	}
	else
		list->head = list->tail = newpl;
	newpl->next = NULL;
}

static ParsedLex *
LPLRemoveHead(ListParsedLex *list)
{
	ParsedLex  *res = list->head;

	if (list->head)
		list->head = list->head->next;

	if (list->head == NULL)
		list->tail = NULL;

	return res;
}

static void
LexizeAddLemm(LexizeData *ld, int type, char *lemm, int lenlemm)
{
	ParsedLex  *newpl = (ParsedLex *) palloc(sizeof(ParsedLex));

	newpl->type = type;
	newpl->lemm = lemm;
	newpl->lenlemm = lenlemm;
	LPLAddTail(&ld->towork, newpl);
	ld->curSub = ld->towork.tail;
}

static void
RemoveHead(LexizeData *ld)
{
	LPLAddTail(&ld->waste, LPLRemoveHead(&ld->towork));

	ld->posDict = 0;
}

static void
setCorrLex(LexizeData *ld, ParsedLex **correspondLexem)
{
	if (correspondLexem)
	{
		*correspondLexem = ld->waste.head;
	}
	else
	{
		ParsedLex  *tmp,
				   *ptr = ld->waste.head;

		while (ptr)
		{
			tmp = ptr->next;
			pfree(ptr);
			ptr = tmp;
		}
	}
	ld->waste.head = ld->waste.tail = NULL;
}

static void
moveToWaste(LexizeData *ld, ParsedLex *stop)
{
	bool		go = true;

	while (ld->towork.head && go)
	{
		if (ld->towork.head == stop)
		{
			ld->curSub = stop->next;
			go = false;
		}
		RemoveHead(ld);
	}
}

static void
setNewTmpRes(LexizeData *ld, ParsedLex *lex, TSLexeme *res)
{
	if (ld->tmpRes)
	{
		TSLexeme   *ptr;

		for (ptr = ld->tmpRes; ptr->lexeme; ptr++)
			pfree(ptr->lexeme);
		pfree(ld->tmpRes);
	}
	ld->tmpRes = res;
	ld->lastRes = lex;
}

static TSLexeme *
LexizeExec(LexizeData *ld, ParsedLex **correspondLexem)
{
	int			i;
	ListDictionary *map;
	TSDictionaryCacheEntry *dict;
	TSLexeme   *res;

	if (ld->curDictId == InvalidOid)
	{
		/*
		 * 常规模式:词典只要一词,但须记住应遍历
		 * 整个栈
		 */

		while (ld->towork.head)
		{
			ParsedLex  *curVal = ld->towork.head;
			char	   *curValLemm = curVal->lemm;
			int			curValLenLemm = curVal->lenlemm;

			map = ld->cfg->map + curVal->type;

			if (curVal->type == 0 || curVal->type >= ld->cfg->lenmap || map->len == 0)
			{
				/* 跳过此类词元 */
				RemoveHead(ld);
				continue;
			}

			for (i = ld->posDict; i < map->len; i++)
			{
				dict = lookup_ts_dictionary_cache(map->dictIds[i]);

				ld->dictState.isend = ld->dictState.getnext = false;
				ld->dictState.private_state = NULL;
				res = (TSLexeme *) DatumGetPointer(FunctionCall4(&(dict->lexize),
																 PointerGetDatum(dict->dictData),
																 PointerGetDatum(curValLemm),
																 Int32GetDatum(curValLenLemm),
																 PointerGetDatum(&ld->dictState)));

				if (ld->dictState.getnext)
				{
					/*
					 * 词典要下一词,故建立并保存当前位置
					 * 后转入多词模式
					 */

					ld->curDictId = DatumGetObjectId(map->dictIds[i]);
					ld->posDict = i + 1;
					ld->curSub = curVal->next;
					if (res)
						setNewTmpRes(ld, curVal, res);
					return LexizeExec(ld, correspondLexem);
				}

				if (!res)		/* 词典不识此词元 */
					continue;

				if (res->flags & TSL_FILTER)
				{
					curValLemm = res->lexeme;
					curValLenLemm = strlen(res->lexeme);
					continue;
				}

				RemoveHead(ld);
				setCorrLex(ld, correspondLexem);
				return res;
			}

			RemoveHead(ld);
		}
	}
	else
	{							/* curDictId 有效 */
		dict = lookup_ts_dictionary_cache(ld->curDictId);

		/*
		 * 词典 ld->curDictId 询问后续词
		 */

		while (ld->curSub)
		{
			ParsedLex  *curVal = ld->curSub;

			map = ld->cfg->map + curVal->type;

			if (curVal->type != 0)
			{
				bool		dictExists = false;

				if (curVal->type >= ld->cfg->lenmap || map->len == 0)
				{
					/* 跳过此类词元 */
					ld->curSub = curVal->next;
					continue;
				}

				/*
				 * 须确认当前词元类型为本词典所识:只需检查
				 * 它是否存在于词典
				 * 列表中?
				 */
				for (i = 0; i < map->len && !dictExists; i++)
					if (ld->curDictId == DatumGetObjectId(map->dictIds[i]))
						dictExists = true;

				if (!dictExists)
				{
					/*
					 * 词典不能处理当前词元类型,
					 * 回到基本模式并重做所有已存词元
					 */
					ld->curDictId = InvalidOid;
					return LexizeExec(ld, correspondLexem);
				}
			}

			ld->dictState.isend = (curVal->type == 0);
			ld->dictState.getnext = false;

			res = (TSLexeme *) DatumGetPointer(FunctionCall4(&(dict->lexize),
															 PointerGetDatum(dict->dictData),
															 PointerGetDatum(curVal->lemm),
															 Int32GetDatum(curVal->lenlemm),
															 PointerGetDatum(&ld->dictState)));

			if (ld->dictState.getnext)
			{
				/* 词典还要一个 */
				ld->curSub = curVal->next;
				if (res)
					setNewTmpRes(ld, curVal, res);
				continue;
			}

			if (res || ld->tmpRes)
			{
				/*
				 * 词典已规范化词元,故从栈移除已用词元,
				 * 回到基本模式并重做栈尾
				 * (若仍有)
				 */
				if (res)
				{
					moveToWaste(ld, ld->curSub);
				}
				else
				{
					res = ld->tmpRes;
					moveToWaste(ld, ld->lastRes);
				}

				/* 复位到初始状态 */
				ld->curDictId = InvalidOid;
				ld->posDict = 0;
				ld->lastRes = NULL;
				ld->tmpRes = NULL;
				setCorrLex(ld, correspondLexem);
				return res;
			}

			/*
			 * 词典不要下一词元且未识别,自 ld->towork.head 
			 * 重做
			 */
			ld->curDictId = InvalidOid;
			return LexizeExec(ld, correspondLexem);
		}
	}

	setCorrLex(ld, correspondLexem);
	return NULL;
}

/*
 * 解析字符串并词元化。
 *
 * 结果填入 prs。
 */
void
parsetext(Oid cfgId, ParsedText *prs, char *buf, int buflen)
{
	int			type,
				lenlemm = 0;	/* 消编译器告警 */
	char	   *lemm = NULL;
	LexizeData	ldata;
	TSLexeme   *norms;
	TSConfigCacheEntry *cfg;
	TSParserCacheEntry *prsobj;
	void	   *prsdata;

	cfg = lookup_ts_config_cache(cfgId);
	prsobj = lookup_ts_parser_cache(cfg->prsId);

	prsdata = (void *) DatumGetPointer(FunctionCall2(&prsobj->prsstart,
													 PointerGetDatum(buf),
													 Int32GetDatum(buflen)));

	LexizeInit(&ldata, cfg);

	do
	{
		type = DatumGetInt32(FunctionCall3(&(prsobj->prstoken),
										   PointerGetDatum(prsdata),
										   PointerGetDatum(&lemm),
										   PointerGetDatum(&lenlemm)));

		if (type > 0 && lenlemm >= MAXSTRLEN)
		{
#ifdef IGNORE_LONGLEXEME
			ereport(NOTICE,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("word is too long to be indexed"),
					 errdetail("Words longer than %d characters are ignored.",
							   MAXSTRLEN)));
			continue;
#else
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("word is too long to be indexed"),
					 errdetail("Words longer than %d characters are ignored.",
							   MAXSTRLEN)));
#endif
		}

		LexizeAddLemm(&ldata, type, lemm, lenlemm);

		while ((norms = LexizeExec(&ldata, NULL)) != NULL)
		{
			TSLexeme   *ptr = norms;

			prs->pos++;			/* 置 pos */

			while (ptr->lexeme)
			{
				if (prs->curwords == prs->lenwords)
				{
					prs->lenwords *= 2;
					prs->words = (ParsedWord *) repalloc(prs->words, prs->lenwords * sizeof(ParsedWord));
				}

				if (ptr->flags & TSL_ADDPOS)
					prs->pos++;
				prs->words[prs->curwords].len = strlen(ptr->lexeme);
				prs->words[prs->curwords].word = ptr->lexeme;
				prs->words[prs->curwords].nvariant = ptr->nvariant;
				prs->words[prs->curwords].flags = ptr->flags & TSL_PREFIX;
				prs->words[prs->curwords].alen = 0;
				prs->words[prs->curwords].pos.pos = LIMITPOS(prs->pos);
				ptr++;
				prs->curwords++;
			}
			pfree(norms);
		}
	} while (type > 0);

	FunctionCall1(&(prsobj->prsend), PointerGetDatum(prsdata));
}

/*
 * 标题框架
 */

/* 向 prs->words[] 加一词 */
static void
hladdword(HeadlineParsedText *prs, char *buf, int buflen, int type)
{
	if (prs->curwords >= prs->lenwords)
	{
		prs->lenwords *= 2;
		prs->words = (HeadlineWordEntry *) repalloc(prs->words, prs->lenwords * sizeof(HeadlineWordEntry));
	}
	memset(&(prs->words[prs->curwords]), 0, sizeof(HeadlineWordEntry));
	prs->words[prs->curwords].type = (uint8) type;
	prs->words[prs->curwords].len = buflen;
	prs->words[prs->curwords].word = palloc(buflen);
	memcpy(prs->words[prs->curwords].word, buf, buflen);
	prs->curwords++;
}

/*
 * 给刚加入的词附加 pos 与匹配查询项数据。
 * 此处 buf/buflen 表示已处理词元,非原始词元文本。
 *
 * 若查询含多个匹配项,则复制刚加入的词,使
 * 每项可被指向。副本以
 * repeated = 1 标记。
 */
static void
hlfinditem(HeadlineParsedText *prs, TSQuery query, int32 pos, char *buf, int buflen)
{
	int			i;
	QueryItem  *item = GETQUERY(query);
	HeadlineWordEntry *word;

	while (prs->curwords + query->size >= prs->lenwords)
	{
		prs->lenwords *= 2;
		prs->words = (HeadlineWordEntry *) repalloc(prs->words, prs->lenwords * sizeof(HeadlineWordEntry));
	}

	word = &(prs->words[prs->curwords - 1]);
	word->pos = LIMITPOS(pos);
	for (i = 0; i < query->size; i++)
	{
		if (item->type == QI_VAL &&
			tsCompareString(GETOPERAND(query) + item->qoperand.distance, item->qoperand.length,
							buf, buflen, item->qoperand.prefix) == 0)
		{
			if (word->item)
			{
				memcpy(&(prs->words[prs->curwords]), word, sizeof(HeadlineWordEntry));
				prs->words[prs->curwords].item = &item->qoperand;
				prs->words[prs->curwords].repeated = 1;
				prs->curwords++;
			}
			else
				word->item = &item->qoperand;
		}
		item++;
	}
}

static void
addHLParsedLex(HeadlineParsedText *prs, TSQuery query, ParsedLex *lexs, TSLexeme *norms)
{
	ParsedLex  *tmplexs;
	TSLexeme   *ptr;
	int32		savedpos;

	while (lexs)
	{
		if (lexs->type > 0)
			hladdword(prs, lexs->lemm, lexs->lenlemm, lexs->type);

		ptr = norms;
		savedpos = prs->vectorpos;
		while (ptr && ptr->lexeme)
		{
			if (ptr->flags & TSL_ADDPOS)
				savedpos++;
			hlfinditem(prs, query, savedpos, ptr->lexeme, strlen(ptr->lexeme));
			ptr++;
		}

		tmplexs = lexs->next;
		pfree(lexs);
		lexs = tmplexs;
	}

	if (norms)
	{
		ptr = norms;
		while (ptr->lexeme)
		{
			if (ptr->flags & TSL_ADDPOS)
				prs->vectorpos++;
			pfree(ptr->lexeme);
			ptr++;
		}
		pfree(norms);
	}
}

void
hlparsetext(Oid cfgId, HeadlineParsedText *prs, TSQuery query, char *buf, int buflen)
{
	int			type,
				lenlemm = 0;	/* 消编译器告警 */
	char	   *lemm = NULL;
	LexizeData	ldata;
	TSLexeme   *norms;
	ParsedLex  *lexs;
	TSConfigCacheEntry *cfg;
	TSParserCacheEntry *prsobj;
	void	   *prsdata;

	cfg = lookup_ts_config_cache(cfgId);
	prsobj = lookup_ts_parser_cache(cfg->prsId);

	prsdata = (void *) DatumGetPointer(FunctionCall2(&(prsobj->prsstart),
													 PointerGetDatum(buf),
													 Int32GetDatum(buflen)));

	LexizeInit(&ldata, cfg);

	do
	{
		type = DatumGetInt32(FunctionCall3(&(prsobj->prstoken),
										   PointerGetDatum(prsdata),
										   PointerGetDatum(&lemm),
										   PointerGetDatum(&lenlemm)));

		if (type > 0 && lenlemm >= MAXSTRLEN)
		{
#ifdef IGNORE_LONGLEXEME
			ereport(NOTICE,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("word is too long to be indexed"),
					 errdetail("Words longer than %d characters are ignored.",
							   MAXSTRLEN)));
			continue;
#else
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("word is too long to be indexed"),
					 errdetail("Words longer than %d characters are ignored.",
							   MAXSTRLEN)));
#endif
		}

		LexizeAddLemm(&ldata, type, lemm, lenlemm);

		do
		{
			if ((norms = LexizeExec(&ldata, &lexs)) != NULL)
			{
				prs->vectorpos++;
				addHLParsedLex(prs, query, lexs, norms);
			}
			else
				addHLParsedLex(prs, query, lexs, NULL);
		} while (norms);
	} while (type > 0);

	FunctionCall1(&(prsobj->prsend), PointerGetDatum(prsdata));
}

/*
 * 由 HeadlineParsedText 生成标题,作为 text 对象。
 */
text *
generateHeadline(HeadlineParsedText *prs)
{
	text	   *out;
	char	   *ptr;
	int			len = 128;
	int			numfragments = 0;
	int16		infrag = 0;

	HeadlineWordEntry *wrd = prs->words;

	out = (text *) palloc(len);
	ptr = ((char *) out) + VARHDRSZ;

	while (wrd - prs->words < prs->curwords)
	{
		while (wrd->len + prs->stopsellen + prs->startsellen + prs->fragdelimlen + (ptr - ((char *) out)) >= len)
		{
			int			dist = ptr - ((char *) out);

			len *= 2;
			out = (text *) repalloc(out, len);
			ptr = ((char *) out) + dist;
		}

		if (wrd->in && !wrd->repeated)
		{
			if (!infrag)
			{

				/* 新片段起点 */
				infrag = 1;
				numfragments++;
				/* 非首片段则加片段分隔符 */
				if (numfragments > 1)
				{
					memcpy(ptr, prs->fragdelim, prs->fragdelimlen);
					ptr += prs->fragdelimlen;
				}
			}
			if (wrd->replace)
			{
				*ptr = ' ';
				ptr++;
			}
			else if (!wrd->skip)
			{
				if (wrd->selected)
				{
					memcpy(ptr, prs->startsel, prs->startsellen);
					ptr += prs->startsellen;
				}
				memcpy(ptr, wrd->word, wrd->len);
				ptr += wrd->len;
				if (wrd->selected)
				{
					memcpy(ptr, prs->stopsel, prs->stopsellen);
					ptr += prs->stopsellen;
				}
			}
		}
		else if (!wrd->repeated)
		{
			if (infrag)
				infrag = 0;
			pfree(wrd->word);
		}

		wrd++;
	}

	SET_VARSIZE(out, ptr - ((char *) out));
	return out;
}
