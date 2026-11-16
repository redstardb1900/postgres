/*-------------------------------------------------------------------------
 *
 * quote.c
 *	  用于引用标识符与字面量的函数
 *
 * Portions Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/quote.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/builtins.h"
#include "varatt.h"


/*
 * quote_ident -
 *	  返回正确引用的标识符
 */
Datum
quote_ident(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);
	const char *qstr;
	char	   *str;

	str = text_to_cstring(t);
	qstr = quote_identifier(str);
	PG_RETURN_TEXT_P(cstring_to_text(qstr));
}

/*
 * quote_literal_internal -
 *	  quote_literal 与 quote_literal_cstr 的辅助函数
 *
 * 注意：不要想让本函数的行为随
 * standard_conforming_strings 而变。我们不知道结果
 * 字面量用于何处，故必须生成在两种设置下
 * 都可用的结果。在自以为更高明前，先看看
 * dblink 把它用在了什么地方。
 */
static size_t
quote_literal_internal(char *dst, const char *src, size_t len)
{
	const char *s;
	char	   *savedst = dst;

	for (s = src; s < src + len; s++)
	{
		if (*s == '\\')
		{
			*dst++ = ESCAPE_STRING_SYNTAX;
			break;
		}
	}

	*dst++ = '\'';
	while (len-- > 0)
	{
		if (SQL_STR_DOUBLE(*src, true))
			*dst++ = *src;
		*dst++ = *src++;
	}
	*dst++ = '\'';

	return dst - savedst;
}

/*
 * quote_literal -
 *	  返回正确引用的字面量
 */
Datum
quote_literal(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);
	text	   *result;
	char	   *cp1;
	char	   *cp2;
	int			len;

	len = VARSIZE_ANY_EXHDR(t);
	/* 按最坏情况预留结果区；浪费一点空间无妨 */
	result = (text *) palloc(len * 2 + 3 + VARHDRSZ);

	cp1 = VARDATA_ANY(t);
	cp2 = VARDATA(result);

	SET_VARSIZE(result, VARHDRSZ + quote_literal_internal(cp2, cp1, len));

	PG_RETURN_TEXT_P(result);
}

/*
 * quote_literal_cstr -
 *	  返回正确引用的字面量
 */
char *
quote_literal_cstr(const char *rawstr)
{
	char	   *result;
	int			len;
	int			newlen;

	len = strlen(rawstr);
	/* 按最坏情况预留结果区；浪费一点空间无妨 */
	result = palloc(len * 2 + 3 + 1);

	newlen = quote_literal_internal(result, rawstr, len);
	result[newlen] = '\0';

	return result;
}

/*
 * quote_nullable -
 *	  返回正确引用的字面量，空值则返回
 *	  为 'NULL' 文本串。
 */
Datum
quote_nullable(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_TEXT_P(cstring_to_text("NULL"));
	else
		PG_RETURN_DATUM(DirectFunctionCall1(quote_literal,
											PG_GETARG_DATUM(0)));
}
