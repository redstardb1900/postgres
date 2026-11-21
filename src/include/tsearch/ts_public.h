/*-------------------------------------------------------------------------
 *
 * ts_public.h
 *	  各全文检索模块（如解析器与词典）的
 *	  公共接口。
 *
 * Copyright (c) 1998-2023, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_public.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_TS_PUBLIC_H_
#define _PG_TS_PUBLIC_H_

#include "tsearch/ts_type.h"

/*
 * 解析器框架
 */

/*
 * 解析器 prslextype 方法的返回类型
 */
typedef struct
{
	int			lexid;
	char	   *alias;
	char	   *descr;
} LexDescr;

/*
 * 与标题生成器的接口（tsparser 的 prsheadline）
 *
 * HeadlineParsedText 描述待高亮的文本。
 * 部分字段由核心传给 prsheadline，其余由
 * prsheadline 输出。
 *
 * 主要数据是 words[]：HeadlineWordEntry 数组，
 * 每词元一项，长度为 curwords。
 * HeadlineWordEntry 字段为：
 *
 * in、selected、replace、skip：初值为零，
 * 可由 prsheadline 置位。标为 "in" 的连续词元
 * 构成待输出的"片段"。此类词元还可标
 * selected、replace 或 skip 以调整显示。
 * （若置多位，行为为其中未指定的一种。）
 * 。
 *
 * type、len、pos、word：由核心填写以描述词元。
 *
 * item：若词元匹配感兴趣 tsquery 的任一操作数，
 * 则为指向该操作数的指针。（多匹配时生成
 * 额外 HeadlineWordEntry 副本持有全部指针。
 * 额外项标 repeated=1，除检查 item 指针外
 * 应忽略。）
 */
typedef struct
{
	uint32		selected:1,		/*  词元要高亮  */
				in:1,			/*  词元属于标题  */
				replace:1,		/*  词元要以空格替换  */
				repeated:1,		/*  持 item 指针的重复项  */
				skip:1,			/*  词元要跳过（不输出）  */
				unused:3,		/*  可用位  */
				type:8,			/*  解析器词元类别  */
				len:16;			/*  词元长度  */
	WordEntryPos pos;			/*  词元位置  */
	char	   *word;			/*  词元文本（非空终止）  */
	QueryOperand *item;			/*  匹配的查询操作数，或 NULL  */
} HeadlineWordEntry;

typedef struct
{
	/*  调用 prsheadline 前由核心填写的字段：  */
	HeadlineWordEntry *words;
	int32		lenwords;		/*  words[] 已分配长度  */
	int32		curwords;		/*  当前有效项数  */
	int32		vectorpos;		/*  ts_parse.c 填写 pos 字段时使用  */

	/*  prsheadline 须填写这些字段：  */
	/*  标记选中词元与分隔片段的字符串：  */
	char	   *startsel;		/*  palloc 串  */
	char	   *stopsel;
	char	   *fragdelim;
	int16		startsellen;	/*  字符串长度  */
	int16		stopsellen;
	int16		fragdelimlen;
} HeadlineParsedText;

/*
 * 全文检索子系统常用事项
 */
extern char *get_tsearch_config_filename(const char *basename,
										 const char *extension);

/*
 * 常用停用词表管理
 */
typedef struct
{
	int			len;
	char	  **stop;
} StopList;

extern void readstoplist(const char *fname, StopList *s,
						 char *(*wordop) (const char *));
extern bool searchstoplist(StopList *s, char *key);

/*
 * 与词典的接口
 */

/*  任意 lexize 函数的返回结构  */
typedef struct
{
	/*----------
	 * 当前切分变体编号。例如挪威语词
	 * 'fotballklubber' 有两种切分：(fotball/klubb)
	 * 与 (fot/ball/klubb)。故词典应返回：
	 *
	 * 变体号		词元
	 *	   1	   fotball
	 *	   1	   klubb
	 *	   2	   fot
	 *	   2	   ball
	 *	   2	   klubb
	 *
	 * 一般地，若 nvariant 与前一相同，则该
	 * TSLexeme 视为属同一切分变体。
	 * 确切取值无关，只看相邻词元间是否变化。
	 *----------
	 */
	uint16		nvariant;

	uint16		flags;			/*  见下方标志  */

	char	   *lexeme;			/*  C 串  */
} TSLexeme;

/*  TSLexeme.flags 中的标志位  */
#define TSL_ADDPOS		0x01
#define TSL_PREFIX		0x02
#define TSL_FILTER		0x04

/*
 * 支持复杂词典（如同义词库）的结构。
 * dictlexize 方法的第 4 参数指向本结构
 */
typedef struct
{
	bool		isend;			/*  in：告知 lexize_info 文本末已
								 * 到达 */
	bool		getnext;		/* 。 */
	void	   *private_state;	/*  getnext==true 时调用间的
								 * 。 */
} DictSubState;

#endif							/* _PG_TS_PUBLIC_H_ */
