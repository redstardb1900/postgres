/*-------------------------------------------------------------------------
 *
 * tsvector_parser.c
 *	  tsvector 解析器
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsvector_parser.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "tsearch/ts_locale.h"
#include "tsearch/ts_utils.h"


/*
 * tsvector 解析器的私有状态。注意 tsquery 也用这段
 * 代码解析自己的输入，因此有布尔标志。oprisdelim
 * 与 is_tsquery 在当前用法中总是同真或同假，
 * 为清晰而分开。
 *
 * 若设置了 oprisdelim，下列字符将被视为分隔符
 * （除空白符外）：! | & ( )
 *
 * is_tsquery 仅影响错误消息的内容。
 *
 * is_web 为 true 可进一步修改 tsquery 解析。
 *
 * 若 escontext 是 ErrorSaveContext 节点，则软错误可
 * 在其中捕获而不被抛出。
 */
struct TSVectorParseStateData
{
	char	   *prsbuf;			/* 下一输入字符 */
	char	   *bufstart;		/* 整个串（仅用于报错） */
	char	   *word;			/* 保存当前词的缓冲区 */
	int			len;			/* 'word' 已分配的字节大小 */
	int			eml;			/* 每字符字节上限 */
	bool		oprisdelim;		/* ! | * ( ) 视为分隔符？ */
	bool		is_tsquery;		/* 报错说 tsquery 而非 tsvector？ */
	bool		is_web;			/* 在 websearch_to_tsquery() 中 */
	Node	   *escontext;		/* 用于软错误报告 */
};


/*
 * 为给定输入串初始化一个解析器状态对象。
 * 还可提供标志位掩码（见 ts_utils.h）和错误
 * 上下文对象。
 */
TSVectorParseState
init_tsvector_parser(char *input, int flags, Node *escontext)
{
	TSVectorParseState state;

	state = (TSVectorParseState) palloc(sizeof(struct TSVectorParseStateData));
	state->prsbuf = input;
	state->bufstart = input;
	state->len = 32;
	state->word = (char *) palloc(state->len);
	state->eml = pg_database_encoding_max_length();
	state->oprisdelim = (flags & P_TSV_OPR_IS_DELIM) != 0;
	state->is_tsquery = (flags & P_TSV_IS_TSQUERY) != 0;
	state->is_web = (flags & P_TSV_IS_WEB) != 0;
	state->escontext = escontext;

	return state;
}

/*
 * 重新初始化解析器以解析 'input'，而非先
 *
 * 前的输入。注意 bufstart（报错中所报串）不变。
 */
void
reset_tsvector_parser(TSVectorParseState state, char *input)
{
	state->prsbuf = input;
}

/*
 * 关闭 tsvector 解析器。
 */
void
close_tsvector_parser(TSVectorParseState state)
{
	pfree(state->word);
	pfree(state);
}

/* 若需再容纳一个字符则扩大 'word' 的大小 */
#define RESIZEPRSBUF \
do { \
	int clen = curpos - state->word; \
	if ( clen + state->eml >= state->len ) \
	{ \
		state->len *= 2; \
		state->word = (char *) repalloc(state->word, state->len); \
		curpos = state->word + clen; \
	} \
} while (0)

/* 填充 gettoken_tsvector 的输出参数并返回 true */
#define RETURN_TOKEN \
do { \
	if (pos_ptr != NULL) \
	{ \
		*pos_ptr = pos; \
		*poslen = npos; \
	} \
	else if (pos != NULL) \
		pfree(pos); \
	\
	if (strval != NULL) \
		*strval = state->word; \
	if (lenval != NULL) \
		*lenval = curpos - state->word; \
	if (endptr != NULL) \
		*endptr = state->prsbuf; \
	return true; \
} while(0)


/* gettoken_tsvector 用的状态码 */
#define WAITWORD		1
#define WAITENDWORD		2
#define WAITNEXTCHAR	3
#define WAITENDCMPLX	4
#define WAITPOSINFO		5
#define INPOSINFO		6
#define WAITPOSDELIM	7
#define WAITCHARCMPLX	8

#define PRSSYNTAXERROR return prssyntaxerror(state)

static bool
prssyntaxerror(TSVectorParseState state)
{
	errsave(state->escontext,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 state->is_tsquery ?
			 errmsg("syntax error in tsquery: \"%s\"", state->bufstart) :
			 errmsg("syntax error in tsvector: \"%s\"", state->bufstart)));
	/* 软错误情形下为方便调用方返回 false */
	return false;
}


/*
 * 从正在解析的串中取下一个词位。成功时返回 true，
 * 输入串结束或软错误时返回 false。
 *
 * 成功时，下列输出参数会被填充：
 *
 * *strval		词位指针
 * *lenval		*strval 的长度
 * *pos_ptr		指向 palloc 的位置与权重数组的指针
 *				与该词位关联。若调用方不感兴趣，
 *				则可传入 NULL，否则
 *				由调用方负责 pfree 该数组。
 * *poslen		*pos_ptr 中的元素个数
 * *endptr		扫描恢复点
 *
 * 不需要的输出参数传 NULL 即可。
 *
 * 若 state->escontext 是 ErrorSaveContext，则调用方须检查
 * SOFT_ERROR_OCCURRED() 来判断 "false" 结果意味着出错
 * 还是串正常结束。
 */
bool
gettoken_tsvector(TSVectorParseState state,
				  char **strval, int *lenval,
				  WordEntryPos **pos_ptr, int *poslen,
				  char **endptr)
{
	int			oldstate = 0;
	char	   *curpos = state->word;
	int			statecode = WAITWORD;

	/*
	 * pos 用于收集逗号分隔的位置列表，其后跟着
	 * 实际词位。
	 */
	WordEntryPos *pos = NULL;
	int			npos = 0;		/* pos 的已用元素 */
	int			posalen = 0;	/* pos 的分配大小 */

	while (1)
	{
		if (statecode == WAITWORD)
		{
			if (*(state->prsbuf) == '\0')
				return false;
			else if (!state->is_web && t_iseq(state->prsbuf, '\''))
				statecode = WAITENDCMPLX;
			else if (!state->is_web && t_iseq(state->prsbuf, '\\'))
			{
				statecode = WAITNEXTCHAR;
				oldstate = WAITENDWORD;
			}
			else if ((state->oprisdelim && ISOPERATOR(state->prsbuf)) ||
					 (state->is_web && t_iseq(state->prsbuf, '"')))
				PRSSYNTAXERROR;
			else if (!t_isspace(state->prsbuf))
			{
				COPYCHAR(curpos, state->prsbuf);
				curpos += pg_mblen(state->prsbuf);
				statecode = WAITENDWORD;
			}
		}
		else if (statecode == WAITNEXTCHAR)
		{
			if (*(state->prsbuf) == '\0')
				ereturn(state->escontext, false,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("there is no escaped character: \"%s\"",
								state->bufstart)));
			else
			{
				RESIZEPRSBUF;
				COPYCHAR(curpos, state->prsbuf);
				curpos += pg_mblen(state->prsbuf);
				Assert(oldstate != 0);
				statecode = oldstate;
			}
		}
		else if (statecode == WAITENDWORD)
		{
			if (!state->is_web && t_iseq(state->prsbuf, '\\'))
			{
				statecode = WAITNEXTCHAR;
				oldstate = WAITENDWORD;
			}
			else if (t_isspace(state->prsbuf) || *(state->prsbuf) == '\0' ||
					 (state->oprisdelim && ISOPERATOR(state->prsbuf)) ||
					 (state->is_web && t_iseq(state->prsbuf, '"')))
			{
				RESIZEPRSBUF;
				if (curpos == state->word)
					PRSSYNTAXERROR;
				*(curpos) = '\0';
				RETURN_TOKEN;
			}
			else if (t_iseq(state->prsbuf, ':'))
			{
				if (curpos == state->word)
					PRSSYNTAXERROR;
				*(curpos) = '\0';
				if (state->oprisdelim)
					RETURN_TOKEN;
				else
					statecode = INPOSINFO;
			}
			else
			{
				RESIZEPRSBUF;
				COPYCHAR(curpos, state->prsbuf);
				curpos += pg_mblen(state->prsbuf);
			}
		}
		else if (statecode == WAITENDCMPLX)
		{
			if (!state->is_web && t_iseq(state->prsbuf, '\''))
			{
				statecode = WAITCHARCMPLX;
			}
			else if (!state->is_web && t_iseq(state->prsbuf, '\\'))
			{
				statecode = WAITNEXTCHAR;
				oldstate = WAITENDCMPLX;
			}
			else if (*(state->prsbuf) == '\0')
				PRSSYNTAXERROR;
			else
			{
				RESIZEPRSBUF;
				COPYCHAR(curpos, state->prsbuf);
				curpos += pg_mblen(state->prsbuf);
			}
		}
		else if (statecode == WAITCHARCMPLX)
		{
			if (!state->is_web && t_iseq(state->prsbuf, '\''))
			{
				RESIZEPRSBUF;
				COPYCHAR(curpos, state->prsbuf);
				curpos += pg_mblen(state->prsbuf);
				statecode = WAITENDCMPLX;
			}
			else
			{
				RESIZEPRSBUF;
				*(curpos) = '\0';
				if (curpos == state->word)
					PRSSYNTAXERROR;
				if (state->oprisdelim)
				{
					/* state->prsbuf+=pg_mblen(state->prsbuf); */
					RETURN_TOKEN;
				}
				else
					statecode = WAITPOSINFO;
				continue;		/* 复查当前字符 */
			}
		}
		else if (statecode == WAITPOSINFO)
		{
			if (t_iseq(state->prsbuf, ':'))
				statecode = INPOSINFO;
			else
				RETURN_TOKEN;
		}
		else if (statecode == INPOSINFO)
		{
			if (t_isdigit(state->prsbuf))
			{
				if (posalen == 0)
				{
					posalen = 4;
					pos = (WordEntryPos *) palloc(sizeof(WordEntryPos) * posalen);
					npos = 0;
				}
				else if (npos + 1 >= posalen)
				{
					posalen *= 2;
					pos = (WordEntryPos *) repalloc(pos, sizeof(WordEntryPos) * posalen);
				}
				npos++;
				WEP_SETPOS(pos[npos - 1], LIMITPOS(atoi(state->prsbuf)));
				/* tsquery 中不会走到这里，无需两条 errmsg */
				if (WEP_GETPOS(pos[npos - 1]) == 0)
					ereturn(state->escontext, false,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("wrong position info in tsvector: \"%s\"",
									state->bufstart)));
				WEP_SETWEIGHT(pos[npos - 1], 0);
				statecode = WAITPOSDELIM;
			}
			else
				PRSSYNTAXERROR;
		}
		else if (statecode == WAITPOSDELIM)
		{
			if (t_iseq(state->prsbuf, ','))
				statecode = INPOSINFO;
			else if (t_iseq(state->prsbuf, 'a') || t_iseq(state->prsbuf, 'A') || t_iseq(state->prsbuf, '*'))
			{
				if (WEP_GETWEIGHT(pos[npos - 1]))
					PRSSYNTAXERROR;
				WEP_SETWEIGHT(pos[npos - 1], 3);
			}
			else if (t_iseq(state->prsbuf, 'b') || t_iseq(state->prsbuf, 'B'))
			{
				if (WEP_GETWEIGHT(pos[npos - 1]))
					PRSSYNTAXERROR;
				WEP_SETWEIGHT(pos[npos - 1], 2);
			}
			else if (t_iseq(state->prsbuf, 'c') || t_iseq(state->prsbuf, 'C'))
			{
				if (WEP_GETWEIGHT(pos[npos - 1]))
					PRSSYNTAXERROR;
				WEP_SETWEIGHT(pos[npos - 1], 1);
			}
			else if (t_iseq(state->prsbuf, 'd') || t_iseq(state->prsbuf, 'D'))
			{
				if (WEP_GETWEIGHT(pos[npos - 1]))
					PRSSYNTAXERROR;
				WEP_SETWEIGHT(pos[npos - 1], 0);
			}
			else if (t_isspace(state->prsbuf) ||
					 *(state->prsbuf) == '\0')
				RETURN_TOKEN;
			else if (!t_isdigit(state->prsbuf))
				PRSSYNTAXERROR;
		}
		else					/* 内部错误 */
			elog(ERROR, "unrecognized state in gettoken_tsvector: %d",
				 statecode);

		/* 下一字符 */
		state->prsbuf += pg_mblen(state->prsbuf);
	}
}
