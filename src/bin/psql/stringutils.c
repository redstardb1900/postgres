/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/bin/psql/stringutils.c
 */
#include "postgres_fe.h"

#include <ctype.h>

#include "common.h"
#include "stringutils.h"


/*
 * strtok() 的替代品（又名穷人的 flex）
 *
 * 将字符串拆分为 token，每次调用返回一个 token，
 * 当给定字符串中没有更多 token 时返回 NULL。
 *
 * 调用约定类似于 strtok，但有更多特性。
 *
 * s -			要解析的字符串，如果为 NULL 则继续解析上一个字符串
 * whitespace - 分隔 token 的空白字符集合
 * delim -		非空白分隔符字符集合（或 NULL）
 * quote -		可以引用 token 的字符集合（如果无则为 NULL）
 * escape -		可以转义引号的字符（如果无则为 0）
 * e_strings -	如果为 true，则将 E'...' 语法视为有效 token
 * del_quotes - 如果为 true，从返回的 token 中去除引号，否则
 *				按字符串中的原样返回
 * encoding -	活动的字符集编码
 *
 * 'delim' 中的字符（如果有）将作为单字符 token 返回，
 * 除非是引用 token 的一部分。
 *
 * 引号字符的双次出现始终被视为表示数据中的单个引号字符。
 * 如果 escape 不为 0，则 escape 后跟任何字符（\0 除外）也是数据字符。
 *
 * e_strings 和 del_quotes 都为 true 的组合目前尚未处理。
 * 这可以修复，但目前不需要。
 *
 * 注意：在此实现中，字符串 s _不会_被覆盖。
 *
 * 注意：可以在单个源字符串的连续调用中变化 delim、quote 和 escape，
 * 但更改 whitespace 是个坏主意，因为你可能会丢失数据。
 *
 *
 *
 */
char *
strtokx(const char *s,
		const char *whitespace,
		const char *delim,
		const char *quote,
		char escape,
		bool e_strings,
		bool del_quotes,
		int encoding)
{
	static char *storage = NULL;	/* store the local copy of the users
									 * 字符串本地副本 */
	static char *string = NULL; /* pointer into storage where to continue on
								 * next call */

	/* 被滥用的变量： */
	unsigned int offset;
	char	   *start;
	char	   *p;

	if (s)
	{
		free(storage);

		/*
		 * 我们可能需要额外空间来为相邻 token 插入分隔符空字符。
		 * 2 倍空间是一个较大的高估，但无论如何此代码不太可能
		 * 用于巨大的字符串。
		 */
		storage = pg_malloc(2 * strlen(s) + 1);
		strcpy(storage, s);
		string = storage;
	}

	if (!storage)
		return NULL;

	/* 跳过前导空白 */
	offset = strspn(string, whitespace);
	start = &string[offset];

	/* 到达字符串末尾？ */
	if (*start == '\0')
	{
		/* 技术上此处不需要释放，但我们比较友好 */
		free(storage);
		storage = NULL;
		string = NULL;
		return NULL;
	}

	/* 测试是否为分隔符字符 */
	if (delim && strchr(delim, *start))
	{
		/*
		 * 如果不在字符串末尾，我们需要插入空字符来终止返回的 token。
		 * 如果下一个字符恰好属于空白集，我们可以直接覆盖它……
		 * 否则移动字符串的其余部分以腾出空间。
		 * （这就是我们在上面分配额外空间的原因。）
		 *
		 */
		p = start + 1;
		if (*p != '\0')
		{
			if (!strchr(whitespace, *p))
				memmove(p + 1, p, strlen(p) + 1);
			*p = '\0';
			string = p + 1;
		}
		else
		{
			/* 在字符串末尾，无需额外工作 */
			string = p;
		}

		return start;
	}

	/* 检查 E 字符串 */
	p = start;
	if (e_strings &&
		(*p == 'E' || *p == 'e') &&
		p[1] == '\'')
	{
		quote = "'";
		escape = '\\';			/* 如果之前是标准字符串，不再是了 */
		p++;
	}

	/* 测试是否为引号字符 */
	if (quote && strchr(quote, *p))
	{
		/* 好的，我们有一个被引用的 token，现在扫描结束引号 */
		char		thisquote = *p++;

		for (; *p; p += PQmblenBounded(p, encoding))
		{
			if (*p == escape && p[1] != '\0')
				p++;			/* 处理转义的任何内容 */
			else if (*p == thisquote && p[1] == thisquote)
				p++;			/* 处理双引号 */
			else if (*p == thisquote)
			{
				p++;			/* 跳过尾随引号 */
				break;
			}
		}

		/*
		 * 如果不在字符串末尾，我们需要插入空字符来终止返回的 token。
		 * 请参阅上面的说明。
		 */
		if (*p != '\0')
		{
			if (!strchr(whitespace, *p))
				memmove(p + 1, p, strlen(p) + 1);
			*p = '\0';
			string = p + 1;
		}
		else
		{
			/* 字符串末尾，无需额外工作 */
			string = p;
		}

		/* 如果调用者需要，清理 token */
		if (del_quotes)
			strip_quotes(start, thisquote, escape, encoding);

		return start;
	}

	/*
	 * 否则没有引号字符。扫描直到下一个空白、分隔符或引号。
	 * 注意：此时已知 *start 不是 '\0'、空白、分隔符或引号，
	 * 因此我们将消耗至少一个字符。
	 */
	offset = strcspn(start, whitespace);

	if (delim)
	{
		unsigned int offset2 = strcspn(start, delim);

		if (offset > offset2)
			offset = offset2;
	}

	if (quote)
	{
		unsigned int offset2 = strcspn(start, quote);

		if (offset > offset2)
			offset = offset2;
	}

	p = start + offset;

	/*
	 * 如果不在字符串末尾，我们需要插入空字符来终止返回的 token。
	 * 请参阅上面的说明。
	 */
	if (*p != '\0')
	{
		if (!strchr(whitespace, *p))
			memmove(p + 1, p, strlen(p) + 1);
		*p = '\0';
		string = p + 1;
	}
	else
	{
		/* 字符串末尾，无需额外工作 */
		string = p;
	}

	return start;
}


/*
 * strip_quotes
 *
 * 从 *source 处的字符串中移除引号。前导和尾随的 'quote' 出现
 * 被移除；嵌入的 'quote' 双次出现被缩减为单次出现；
 * 如果 'escape' 不为 0，则 'escape' 移除下一个字符的特殊意义。
 *
 * 注意源字符串会被原地覆盖。
 *
 */
void
strip_quotes(char *source, char quote, char escape, int encoding)
{
	char	   *src;
	char	   *dst;

	Assert(source != NULL);
	Assert(quote != '\0');

	src = dst = source;

	if (*src && *src == quote)
		src++;					/* 跳过前导引号 */

	while (*src)
	{
		char		c = *src;
		int			i;

		if (c == quote && src[1] == '\0')
			break;				/* 跳过尾随引号 */
		else if (c == quote && src[1] == quote)
			src++;				/* 处理双引号 */
		else if (c == escape && src[1] != '\0')
			src++;				/* 处理转义字符 */

		i = PQmblenBounded(src, encoding);
		while (i--)
			*dst++ = *src++;
	}

	*dst = '\0';
}


/*
 * quote_if_needed
 *
 * strip_quotes() 的反操作。如果 "source" 字面上表示自身
 * 而无需引用或转义，则返回 NULL。否则，返回一个 malloc'd 的副本，
 * 应用了引用和转义：
 *
 * source -			要解析的字符串
 * entails_quote -	这些字符中任何一个存在？需要外部引号
 * quote -			在字符串内双写，附加到两端
 * escape -			在字符串内双写
 * force_quote -	如果为 true，即使输出不"需要"也引用它
 * encoding -		活动的字符集编码
 *
 * 不要将此用作 PQescapeStringConn() 的替代品。将其用于
 * 将由 strtokx() 或 psql_scan_slash_option() 解析的字符串。
 */
char *
quote_if_needed(const char *source, const char *entails_quote,
				char quote, char escape, bool force_quote,
				int encoding)
{
	const char *src;
	char	   *ret;
	char	   *dst;
	bool		need_quotes = force_quote;

	Assert(source != NULL);
	Assert(quote != '\0');

	src = source;
	dst = ret = pg_malloc(2 * strlen(src) + 3); /* 余量 */

	*dst++ = quote;

	while (*src)
	{
		char		c = *src;
		int			i;

		if (c == quote)
		{
			need_quotes = true;
			*dst++ = quote;
		}
		else if (c == escape)
		{
			need_quotes = true;
			*dst++ = escape;
		}
		else if (strchr(entails_quote, c))
			need_quotes = true;

		i = PQmblenBounded(src, encoding);
		while (i--)
			*dst++ = *src++;
	}

	*dst++ = quote;
	*dst = '\0';

	if (!need_quotes)
	{
		free(ret);
		ret = NULL;
	}

	return ret;
}
