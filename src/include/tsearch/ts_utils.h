/*-------------------------------------------------------------------------
 *
 * ts_utils.h
 *	  全文检索的辅助工具
 *
 * Copyright (c) 1998-2023, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_TS_UTILS_H_
#define _PG_TS_UTILS_H_

#include "nodes/pg_list.h"
#include "tsearch/ts_public.h"
#include "tsearch/ts_type.h"

/*
 * tsvector 与 tsquery 的公共解析定义
 */

/*  tsvector 解析。  */

struct TSVectorParseStateData;	/*  parser 中不透明结构  */
typedef struct TSVectorParseStateData *TSVectorParseState;

/*  可传给 init_tsvector_parser 的标志位：  */
#define P_TSV_OPR_IS_DELIM	(1 << 0)
#define P_TSV_IS_TSQUERY	(1 << 1)
#define P_TSV_IS_WEB		(1 << 2)

extern TSVectorParseState init_tsvector_parser(char *input, int flags,
											   Node *escontext);
extern void reset_tsvector_parser(TSVectorParseState state, char *input);
extern bool gettoken_tsvector(TSVectorParseState state,
							  char **strval, int *lenval,
							  WordEntryPos **pos_ptr, int *poslen,
							  char **endptr);
extern void close_tsvector_parser(TSVectorParseState state);

/*  短语运算符以 '<' 开头  */
#define ISOPERATOR(x) \
	( pg_mblen(x) == 1 && ( *(x) == '!' ||	\
							*(x) == '&' ||	\
							*(x) == '|' ||	\
							*(x) == '(' ||	\
							*(x) == ')' ||	\
							*(x) == '<'		\
						  ) )

/* 。 */

struct TSQueryParserStateData;	/*  tsquery.c 中私有  */
typedef struct TSQueryParserStateData *TSQueryParserState;

typedef void (*PushFunction) (Datum opaque, TSQueryParserState state,
							  char *token, int tokenlen,
							  int16 tokenweights,	/*  QueryOperand 所述
													 * 位图 */
							  bool prefix);

/*  可传给 parse_tsquery 的标志位：  */
#define P_TSQ_PLAIN		(1 << 0)
#define P_TSQ_WEB		(1 << 1)

extern TSQuery parse_tsquery(char *buf,
							 PushFunction pushval,
							 Datum opaque,
							 int flags,
							 Node *escontext);

/*  供 PushFunction 实现使用的函数  */
extern void pushValue(TSQueryParserState state,
					  char *strval, int lenval, int16 weight, bool prefix);
extern void pushStop(TSQueryParserState state);
extern void pushOperator(TSQueryParserState state, int8 oper, int16 distance);

/*
 * 解析纯文本并词元化
 */
typedef struct
{
	uint16		len;
	uint16		nvariant;
	union
	{
		uint16		pos;

		/*
		 * 使用 apos 数组时，apos[0] 为数组元素个数
		 * （不含 apos[0]），alen 为数组已分配
		 * 。
		 */
		uint16	   *apos;
	}			pos;
	uint16		flags;			/*  目前仅 TSL_PREFIX  */
	char	   *word;
	uint32		alen;
} ParsedWord;

typedef struct
{
	ParsedWord *words;
	int32		lenwords;
	int32		curwords;
	int32		pos;
} ParsedText;

extern void parsetext(Oid cfgId, ParsedText *prs, char *buf, int32 buflen);

/*
 * 标题框架，生成流程通常为：
 *	。
 *	2 解析器专用函数找片段
 *	3 generateHeadline 生成结果文本
 */

extern void hlparsetext(Oid cfgId, HeadlineParsedText *prs, TSQuery query,
						char *buf, int32 buflen);
extern text *generateHeadline(HeadlineParsedText *prs);

/*
 * TSQuery 执行支持
 *
 * TS_execute() 对可多种形式表示的数据执行
 * tsquery。调用 TSExecuteCallback 检查给定
 * 原始 tsquery 值是否在数据中匹配。
 */

/*  TS_execute 需三值逻辑以处理带短语匹配的 NOT  */
typedef enum
{
	TS_NO,						/*  确定不匹配  */
	TS_YES,						/*  确定匹配  */
	TS_MAYBE					/*  缺位置数据无法验证匹配  */
} TSTernaryValue;

/*
 * 若因 tsquery 中短语匹配运算符而需要词元位置
 * 数据，则向 TSExecuteCallback 传入 ExecPhraseData。
 * 回调在返回 TS_YES（成功）时应填写位置数据。
 * 若无法返回位置数据，应保持 data 不变并返回
 * TS_MAYBE。TS_execute() 调用方须安排稍后在
 * 有位置数据时重检。
 *
 * 报告的词元位置须已排序且唯一。调用方只应查阅
 * pos 数组的位置位，即 WEP_GETPOS(data->pos[i])。
 * 这样返回的 pos 可直接指向 tsvector 的
 * WordEntryPos 部分。若 allocated 为真，则 pos
 * 为 palloc 工作区，用毕可由调用方释放。
 *
 * "negate" 表示 pos 含查询不匹配的位置，而非
 * 匹配位置。"width" 在匹配宽于一词元时为正。
 * 这两字段通常无须 TSExecuteCallback 触碰；供
 * TS_execute 内短语搜索处理使用。
 * 。
 *
 * ExecPhraseData 各字段由调用方初值为零。
 */
typedef struct ExecPhraseData
{
	int			npos;			/*  报告的位置个数  */
	bool		allocated;		/* 。 */
	bool		negate;			/*  位置为查询不匹配处  */
	WordEntryPos *pos;			/*  有序、无重复的词元位置  */
	int			width;			/* 。 */
} ExecPhraseData;

/*
 * TSQuery 词元检查函数的签名
 *
 * arg：从 TS_execute 调用方透传的不透明值
 * val：待测是否存在的词元
 * data：填写词元位置；不需要位置数据时为 NULL
 *
 * 若词元在数据中返回 TS_YES，可能在则 TS_MAYBE，
 * 确定不在则 TS_NO。若 data 非 NULL，有位置数据时
 * 须填写；无位置数据则保持 *data 为零并返回
 * TS_MAYBE，切勿返回 TS_YES。
 */
typedef TSTernaryValue (*TSExecuteCallback) (void *arg, QueryOperand *val,
											 ExecPhraseData *data);

/*
 * TS_execute 的标志位
 */
#define TS_EXEC_EMPTY			(0x00)
/*
 * 若置 TS_EXEC_SKIP_NOT，则 NOT 子表达式自动
 * 求值为真。此为旧默认行为，现已不推荐（常给出
 * 愚蠢结果），但某些应用可能仍有用。
 * 。
 */
#define TS_EXEC_SKIP_NOT		(0x01)
/*
 * 若置 TS_EXEC_PHRASE_NO_POS，允许在无位置信息时
 * 有损执行 OP_PHRASE：真结果表示短语可能存在。
 * 无此标志时，无词元位置信息则 OP_PHRASE 恒返回
 * 假。
 */
#define TS_EXEC_PHRASE_NO_POS	(0x02)

extern bool TS_execute(QueryItem *curitem, void *arg, uint32 flags,
					   TSExecuteCallback chkcond);
extern TSTernaryValue TS_execute_ternary(QueryItem *curitem, void *arg,
										 uint32 flags,
										 TSExecuteCallback chkcond);
extern List *TS_execute_locations(QueryItem *curitem, void *arg,
								  uint32 flags,
								  TSExecuteCallback chkcond);
extern bool tsquery_requires_match(QueryItem *curitem);

/*
 * to_ts* - 文本到 tsvector、tsquery 的变换
 */
extern TSVector make_tsvector(ParsedText *prs);
extern int32 tsCompareString(char *a, int lena, char *b, int lenb, bool prefix);

/*
 * 索引可能的策略号
 *	  TSearchStrategyNumber  - (tsvector|text) @@ tsquery
 *	  TSearchWithClassStrategyNumber  - tsvector @@@ tsquery
 */
#define TSearchStrategyNumber			1
#define TSearchWithClassStrategyNumber	2

/*
 * TSQuery 工具
 */
extern QueryItem *clean_NOT(QueryItem *ptr, int32 *len);
extern TSQuery cleanup_tsquery_stopwords(TSQuery in, bool noisy);

typedef struct QTNode
{
	QueryItem  *valnode;
	uint32		flags;
	int32		nchild;
	char	   *word;
	uint32		sign;
	struct QTNode **child;
} QTNode;

/* 。 */
#define QTN_NEEDFREE	0x01
#define QTN_NOCHANGE	0x02
#define QTN_WORDFREE	0x04

typedef uint64 TSQuerySign;

#define TSQS_SIGLEN  (sizeof(TSQuerySign)*BITS_PER_BYTE)

static inline Datum
TSQuerySignGetDatum(TSQuerySign X)
{
	return Int64GetDatum((int64) X);
}

static inline TSQuerySign
DatumGetTSQuerySign(Datum X)
{
	return (TSQuerySign) DatumGetInt64(X);
}

#define PG_RETURN_TSQUERYSIGN(X)	return TSQuerySignGetDatum(X)
#define PG_GETARG_TSQUERYSIGN(n)	DatumGetTSQuerySign(PG_GETARG_DATUM(n))


extern QTNode *QT2QTN(QueryItem *in, char *operand);
extern TSQuery QTN2QT(QTNode *in);
extern void QTNFree(QTNode *in);
extern void QTNSort(QTNode *in);
extern void QTNTernary(QTNode *in);
extern void QTNBinary(QTNode *in);
extern int	QTNodeCompare(QTNode *an, QTNode *bn);
extern QTNode *QTNCopy(QTNode *in);
extern void QTNClearFlags(QTNode *in, uint32 flags);
extern bool QTNEq(QTNode *a, QTNode *b);
extern TSQuerySign makeTSQuerySign(TSQuery a);
extern QTNode *findsubquery(QTNode *root, QTNode *ex, QTNode *subs,
							bool *isfind);

#endif							/* _PG_TS_UTILS_H_ */
