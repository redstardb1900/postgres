/*-------------------------------------------------------------------------
 *
 * ts_locale.c
 *		locale compatibility layer for tsearch
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/ts_locale.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "common/string.h"
#include "storage/fd.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_public.h"

static void tsearch_readline_callback(void *arg);


/*
 * 这些函数用 3 个 wchar_t 输出缓冲而非直觉上的 2,因
 * Windows 上 wchar_t 为 16 位,且 char2wchar() 得到
 * UTF16 而非 UTF32。单输入字符因此可能产生代理对,
 * 而非仅一个 wchar_t;须留尾空字符位。若确为代理对,
 * 我们只把首码点传给 iswdigit() 等,故对基本多文种
 * 平面以外的字符,这些函数
 * 恒返回假。
 */
#define WC_BUF_LEN  3

int
t_isdigit(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[WC_BUF_LEN];
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || database_ctype_is_c)
		return isdigit(TOUCHAR(ptr));

	char2wchar(character, WC_BUF_LEN, ptr, clen, mylocale);

	return iswdigit((wint_t) character[0]);
}

int
t_isspace(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[WC_BUF_LEN];
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || database_ctype_is_c)
		return isspace(TOUCHAR(ptr));

	char2wchar(character, WC_BUF_LEN, ptr, clen, mylocale);

	return iswspace((wint_t) character[0]);
}

int
t_isalpha(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[WC_BUF_LEN];
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || database_ctype_is_c)
		return isalpha(TOUCHAR(ptr));

	char2wchar(character, WC_BUF_LEN, ptr, clen, mylocale);

	return iswalpha((wint_t) character[0]);
}

int
t_isalnum(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[WC_BUF_LEN];
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || database_ctype_is_c)
		return isalnum(TOUCHAR(ptr));

	char2wchar(character, WC_BUF_LEN, ptr, clen, mylocale);

	return iswalnum((wint_t) character[0]);
}

int
t_isprint(const char *ptr)
{
	int			clen = pg_mblen(ptr);
	wchar_t		character[WC_BUF_LEN];
	pg_locale_t mylocale = 0;	/* TODO */

	if (clen == 1 || database_ctype_is_c)
		return isprint(TOUCHAR(ptr));

	char2wchar(character, WC_BUF_LEN, ptr, clen, mylocale);

	return iswprint((wint_t) character[0]);
}


/*
 * Set up to read a file using tsearch_readline().  This facility is
 * better than just reading the file directly because it provides error
 * context pointing to the specific line where a problem is detected.
 *
 * Expected usage is:
 *
 *		tsearch_readline_state trst;
 *
 *		if (!tsearch_readline_begin(&trst, filename))
 *			ereport(ERROR,
 *					(errcode(ERRCODE_CONFIG_FILE_ERROR),
 *					 errmsg("could not open stop-word file \"%s\": %m",
 *							filename)));
 *		while ((line = tsearch_readline(&trst)) != NULL)
 *			process line;
 *		tsearch_readline_end(&trst);
 *
 * Note that the caller supplies the ereport() for file open failure;
 * this is so that a custom message can be provided.  The filename string
 * passed to tsearch_readline_begin() must remain valid through
 * tsearch_readline_end().
 */
bool
tsearch_readline_begin(tsearch_readline_state *stp,
					   const char *filename)
{
	if ((stp->fp = AllocateFile(filename, "r")) == NULL)
		return false;
	stp->filename = filename;
	stp->lineno = 0;
	initStringInfo(&stp->buf);
	stp->curline = NULL;
	/* 为 ereport() 设置错误回溯支持 */
	stp->cb.callback = tsearch_readline_callback;
	stp->cb.arg = (void *) stp;
	stp->cb.previous = error_context_stack;
	error_context_stack = &stp->cb;
	return true;
}

/*
 * 从 tsearch 数据文件读下一行(预期 UTF-8),需要时
 * 转为数据库编码。返回串经 palloc。返回 NULL
 * 表示 EOF。
 */
char *
tsearch_readline(tsearch_readline_state *stp)
{
	char	   *recoded;

	/* 推进行号以用于错误报告 */
	stp->lineno++;

	/* 清除 curline,已不再相关 */
	if (stp->curline)
	{
		if (stp->curline != stp->buf.data)
			pfree(stp->curline);
		stp->curline = NULL;
	}

	/* 收集下一行(若有) */
	if (!pg_get_line_buf(stp->fp, &stp->buf))
		return NULL;

	/* 校验输入为 UTF-8,需要时再转为 DB 编码 */
	recoded = pg_any_to_server(stp->buf.data, stp->buf.len, PG_UTF8);

	/* 保存正确编码的串以备错误报告 */
	stp->curline = recoded;		/* 可能等于 buf.data */

	/*
	 * 我们总返回新 pstrdup 的串。若 pg_any_to_server() 返回
	 * 的是 buf.data,这显然必要;即便发生了编码转换也需要
	 * 第二份拷贝。调用方有权随时 pfree 返回串,那之后若
	 * 再出错,curline 会指向已回收存储而惹祸。
	 * (更愿返回 pstrdup 结果而非 pg_any_to_server 的输出,
	 * 因转换结果往往过量分配。调用方可能把结果串直接
	 * 存入长寿命词典结构,故不想 palloc 块偏大。
	 * 转换结果将在下次调用时回收。)
	 *
	 *
	 *
	 */
	return pstrdup(recoded);
}

/*
 * 用 tsearch_readline() 读完文件后收尾
 */
void
tsearch_readline_end(tsearch_readline_state *stp)
{
	/* 禁止下方错误报告使用 curline */
	if (stp->curline)
	{
		if (stp->curline != stp->buf.data)
			pfree(stp->curline);
		stp->curline = NULL;
	}

	/* 释放其他资源 */
	pfree(stp->buf.data);
	FreeFile(stp->fp);

	/* 弹出错误上下文栈 */
	error_context_stack = stp->cb.previous;
}

/*
 * 读 tsearch 配置文件时出错的错误上下文
 * 回调。
 */
static void
tsearch_readline_callback(void *arg)
{
	tsearch_readline_state *stp = (tsearch_readline_state *) arg;

	/*
	 * tsearch_readline() 自身出错时不能纳入配置行文本。
	 * 此类错误主因是编码违规,对含错误编码数据的
	 * 消息我们不敢尝试打印。
	 *
	 */
	if (stp->curline)
		errcontext("line %d of configuration file \"%s\": \"%s\"",
				   stp->lineno,
				   stp->filename,
				   stp->curline);
	else
		errcontext("line %d of configuration file \"%s\"",
				   stp->lineno,
				   stp->filename);
}


/*
 * lowerstr --- 将空终止串折为小写
 *
 * 返回串经 palloc
 */
char *
lowerstr(const char *str)
{
	return lowerstr_with_len(str, strlen(str));
}

/*
 * lowerstr_with_len --- 将串折为小写
 *
 * 输入串不必空终止。
 *
 * 返回串经 palloc
 */
char *
lowerstr_with_len(const char *str, int len)
{
	char	   *out;
	pg_locale_t mylocale = 0;	/* TODO */

	if (len == 0)
		return pstrdup("");

	/*
	 * 仅当最大编码长度 > 1 且 ctype != C 时用宽字符代码。
	 * 某些操作系统在多字节编码与 C 区域设置下会失败。
	 * 对 C 区域设置也无需按多字节处理。源自
	 * backend/utils/adt/oracle_compat.c Teodor
	 */
	if (pg_database_encoding_max_length() > 1 && !database_ctype_is_c)
	{
		wchar_t    *wstr,
				   *wptr;
		int			wlen;

		/*
		 * 按最坏情况分配 wchar_t 个数,len 为字节数且
		 * >= 字符数,再为 0 分配 1 个 wchar_t,因
		 * wchar2char 要零终止串
		 */
		wptr = wstr = (wchar_t *) palloc(sizeof(wchar_t) * (len + 1));

		wlen = char2wchar(wstr, len + 1, str, len, mylocale);
		Assert(wlen <= len);

		while (*wptr)
		{
			*wptr = towlower((wint_t) *wptr);
			wptr++;
		}

		/*
		 * 按最坏情况分配结果串 + '\0'
		 */
		len = pg_database_encoding_max_length() * wlen + 1;
		out = (char *) palloc(len);

		wlen = wchar2char(out, wstr, len, mylocale);

		pfree(wstr);

		if (wlen < 0)
			ereport(ERROR,
					(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
					 errmsg("conversion from wchar_t to server encoding failed: %m")));
		Assert(wlen < len);
	}
	else
	{
		const char *ptr = str;
		char	   *outptr;

		outptr = out = (char *) palloc(sizeof(char) * (len + 1));
		while ((ptr - str) < len && *ptr)
		{
			*outptr++ = tolower(TOUCHAR(ptr));
			ptr++;
		}
		*outptr = '\0';
	}

	return out;
}
