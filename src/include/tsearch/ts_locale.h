/*-------------------------------------------------------------------------
 *
 * ts_locale.h
 *		全文检索的区域设置兼容层
 *
 * Copyright (c) 1998-2023, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_locale.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef __TSLOCALE_H__
#define __TSLOCALE_H__

#include <ctype.h>
#include <limits.h>
#include <wctype.h>

#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "utils/pg_locale.h"

/*  tsearch_readline 的工作状态（应为调用方局部变量）  */
typedef struct
{
	FILE	   *fp;
	const char *filename;
	int			lineno;
	StringInfoData buf;			/*  当前输入行，UTF-8  */
	char	   *curline;		/*  当前输入行，数据库编码  */
	/*  curline 可为 NULL、等于 buf.data，或 palloc 串  */
	ErrorContextCallback cb;
} tsearch_readline_state;

#define TOUCHAR(x)	(*((const unsigned char *) (x)))

/*  t_iseq() 第二参数须为普通 ASCII 字符  */
#define t_iseq(x,c)		(TOUCHAR(x) == (unsigned char) (c))

#define COPYCHAR(d,s)	memcpy(d, s, pg_mblen(s))

extern int	t_isdigit(const char *ptr);
extern int	t_isspace(const char *ptr);
extern int	t_isalpha(const char *ptr);
extern int	t_isalnum(const char *ptr);
extern int	t_isprint(const char *ptr);

extern char *lowerstr(const char *str);
extern char *lowerstr_with_len(const char *str, int len);

extern bool tsearch_readline_begin(tsearch_readline_state *stp,
								   const char *filename);
extern char *tsearch_readline(tsearch_readline_state *stp);
extern void tsearch_readline_end(tsearch_readline_state *stp);

#endif							/* __TSLOCALE_H__ */
