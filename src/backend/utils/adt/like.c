/*-------------------------------------------------------------------------
 *
 * like.c
 *	  like 表达式的处理。
 *
 *	 NOTES
 *		regexp.c 代码的大改！！贡献者：
 *		Keith Parks <emkxp01@mtcc.demon.co.uk> (7/95).
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	src/backend/utils/adt/like.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/pg_collation.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/pg_locale.h"
#include "varatt.h"


#define LIKE_TRUE						1
#define LIKE_FALSE						0
#define LIKE_ABORT						(-1)


static int	SB_MatchText(const char *t, int tlen, const char *p, int plen,
						 pg_locale_t locale, bool locale_is_c);
static text *SB_do_like_escape(text *pat, text *esc);

static int	MB_MatchText(const char *t, int tlen, const char *p, int plen,
						 pg_locale_t locale, bool locale_is_c);
static text *MB_do_like_escape(text *pat, text *esc);

static int	UTF8_MatchText(const char *t, int tlen, const char *p, int plen,
						   pg_locale_t locale, bool locale_is_c);

static int	SB_IMatchText(const char *t, int tlen, const char *p, int plen,
						  pg_locale_t locale, bool locale_is_c);

static int	GenericMatchText(const char *s, int slen, const char *p, int plen, Oid collation);
static int	Generic_Text_IC_like(text *str, text *pat, Oid collation);

/*--------------------
 * MatchText 的辅助例程。把给定的多字节流按宽字
 * 符进行比较。匹配则返回 1，否则返回 0。
 *--------------------
 */
static inline int
wchareq(const char *p1, const char *p2)
{
	int			p1_len;

	/* 优化：快速比较第一个字节。 */
	if (*p1 != *p2)
		return 0;

	p1_len = pg_mblen(p1);
	if (pg_mblen(p2) != p1_len)
		return 0;

	/* 两者长度相同 */
	while (p1_len--)
	{
		if (*p1++ != *p2++)
			return 0;
	}
	return 1;
}

/*
 * 以前我们这里有一个 iwchareq() 例程，它试图对多字
 * 节字符做大小写不敏感的比较。然而它完全没用，
 * 因为它依赖具有单字节 API 的 tolower() …… 而
 * towlower() 也不会好多少，因为我们没有足够廉价
 * 的办法把单个字符转换成系统 wchar_t 格式。所以
 * 现在我们只用 lower() 把串变小写，再应用常规的
 * LIKE 比较。待区域设置支持更好后再重新审视。
 */

/*
 * 不过对于单字节编码，我们确实用即时折叠处理
 * 来做大小写不敏感匹配。
 */
static char
SB_lower_char(unsigned char c, pg_locale_t locale, bool locale_is_c)
{
	if (locale_is_c)
		return pg_ascii_tolower(c);
#ifdef HAVE_LOCALE_T
	else if (locale)
		return tolower_l(c, locale->info.lt);
#endif
	else
		return pg_tolower(c);
}


#define NextByte(p, plen)	((p)++, (plen)--)

/* 设置 like_match.c 的多字节字符编译 */
#define CHAREQ(p1, p2) wchareq((p1), (p2))
#define NextChar(p, plen) \
	do { int __l = pg_mblen(p); (p) +=__l; (plen) -=__l; } while (0)
#define CopyAdvChar(dst, src, srclen) \
	do { int __l = pg_mblen(src); \
		 (srclen) -= __l; \
		 while (__l-- > 0) \
			 *(dst)++ = *(src)++; \
	   } while (0)

#define MatchText	MB_MatchText
#define do_like_escape	MB_do_like_escape

#include "like_match.c"

/* 设置 like_match.c 的单字节字符编译 */
#define CHAREQ(p1, p2) (*(p1) == *(p2))
#define NextChar(p, plen) NextByte((p), (plen))
#define CopyAdvChar(dst, src, srclen) (*(dst)++ = *(src)++, (srclen)--)

#define MatchText	SB_MatchText
#define do_like_escape	SB_do_like_escape

#include "like_match.c"

/* 设置 like_match.c 的单字节大小写不敏感匹配编译 */
#define MATCH_LOWER(t) SB_lower_char((unsigned char) (t), locale, locale_is_c)
#define NextChar(p, plen) NextByte((p), (plen))
#define MatchText SB_IMatchText

#include "like_match.c"

/* 设置 like_match.c 的 UTF8 编码编译，使用快速 NextChar */

#define NextChar(p, plen) \
	do { (p)++; (plen)--; } while ((plen) > 0 && (*(p) & 0xC0) == 0x80 )
#define MatchText	UTF8_MatchText

#include "like_match.c"

/* 所有无需内联大小写折叠情形通用 */
static inline int
GenericMatchText(const char *s, int slen, const char *p, int plen, Oid collation)
{
	if (collation && !lc_ctype_is_c(collation))
	{
		pg_locale_t locale = pg_newlocale_from_collation(collation);

		if (!pg_locale_deterministic(locale))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("nondeterministic collations are not supported for LIKE")));
	}

	if (pg_database_encoding_max_length() == 1)
		return SB_MatchText(s, slen, p, plen, 0, true);
	else if (GetDatabaseEncoding() == PG_UTF8)
		return UTF8_MatchText(s, slen, p, plen, 0, true);
	else
		return MB_MatchText(s, slen, p, plen, 0, true);
}

static inline int
Generic_Text_IC_like(text *str, text *pat, Oid collation)
{
	char	   *s,
			   *p;
	int			slen,
				plen;
	pg_locale_t locale = 0;
	bool		locale_is_c = false;

	if (!OidIsValid(collation))
	{
		/*
		 * 这通常意味着解析器无法消解隐式排序规则
		 * 的冲突，因此就这样报告。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for ILIKE"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}

	if (lc_ctype_is_c(collation))
		locale_is_c = true;
	else
		locale = pg_newlocale_from_collation(collation);

	if (!pg_locale_deterministic(locale))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nondeterministic collations are not supported for ILIKE")));

	/*
	 * 出于效率考虑，单字节情形我们不对模式与文本
	 * 调 lower()，而是逐字符调 SB_lower_char。多
	 * 字节情形下只好 :-(。另外 ICU 也不支持单字
	 * 符折叠，因此我们只能绕远路而
	 * 行
	 */

	if (pg_database_encoding_max_length() > 1 || (locale && locale->provider == COLLPROVIDER_ICU))
	{
		pat = DatumGetTextPP(DirectFunctionCall1Coll(lower, collation,
													 PointerGetDatum(pat)));
		p = VARDATA_ANY(pat);
		plen = VARSIZE_ANY_EXHDR(pat);
		str = DatumGetTextPP(DirectFunctionCall1Coll(lower, collation,
													 PointerGetDatum(str)));
		s = VARDATA_ANY(str);
		slen = VARSIZE_ANY_EXHDR(str);
		if (GetDatabaseEncoding() == PG_UTF8)
			return UTF8_MatchText(s, slen, p, plen, 0, true);
		else
			return MB_MatchText(s, slen, p, plen, 0, true);
	}
	else
	{
		p = VARDATA_ANY(pat);
		plen = VARSIZE_ANY_EXHDR(pat);
		s = VARDATA_ANY(str);
		slen = VARSIZE_ANY_EXHDR(str);
		return SB_IMatchText(s, slen, p, plen, locale, locale_is_c);
	}
}

/*
 *	由函数管理器调用的接口例程
 */

Datum
namelike(PG_FUNCTION_ARGS)
{
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_PP(1);
	bool		result;
	char	   *s,
			   *p;
	int			slen,
				plen;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);

	result = (GenericMatchText(s, slen, p, plen, PG_GET_COLLATION()) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
namenlike(PG_FUNCTION_ARGS)
{
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_PP(1);
	bool		result;
	char	   *s,
			   *p;
	int			slen,
				plen;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);

	result = (GenericMatchText(s, slen, p, plen, PG_GET_COLLATION()) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
textlike(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	text	   *pat = PG_GETARG_TEXT_PP(1);
	bool		result;
	char	   *s,
			   *p;
	int			slen,
				plen;

	s = VARDATA_ANY(str);
	slen = VARSIZE_ANY_EXHDR(str);
	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);

	result = (GenericMatchText(s, slen, p, plen, PG_GET_COLLATION()) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
textnlike(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	text	   *pat = PG_GETARG_TEXT_PP(1);
	bool		result;
	char	   *s,
			   *p;
	int			slen,
				plen;

	s = VARDATA_ANY(str);
	slen = VARSIZE_ANY_EXHDR(str);
	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);

	result = (GenericMatchText(s, slen, p, plen, PG_GET_COLLATION()) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
bytealike(PG_FUNCTION_ARGS)
{
	bytea	   *str = PG_GETARG_BYTEA_PP(0);
	bytea	   *pat = PG_GETARG_BYTEA_PP(1);
	bool		result;
	char	   *s,
			   *p;
	int			slen,
				plen;

	s = VARDATA_ANY(str);
	slen = VARSIZE_ANY_EXHDR(str);
	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);

	result = (SB_MatchText(s, slen, p, plen, 0, true) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
byteanlike(PG_FUNCTION_ARGS)
{
	bytea	   *str = PG_GETARG_BYTEA_PP(0);
	bytea	   *pat = PG_GETARG_BYTEA_PP(1);
	bool		result;
	char	   *s,
			   *p;
	int			slen,
				plen;

	s = VARDATA_ANY(str);
	slen = VARSIZE_ANY_EXHDR(str);
	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);

	result = (SB_MatchText(s, slen, p, plen, 0, true) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

/*
 * 大小写不敏感版本
 */

Datum
nameiclike(PG_FUNCTION_ARGS)
{
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_PP(1);
	bool		result;
	text	   *strtext;

	strtext = DatumGetTextPP(DirectFunctionCall1(name_text,
												 NameGetDatum(str)));
	result = (Generic_Text_IC_like(strtext, pat, PG_GET_COLLATION()) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
nameicnlike(PG_FUNCTION_ARGS)
{
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_PP(1);
	bool		result;
	text	   *strtext;

	strtext = DatumGetTextPP(DirectFunctionCall1(name_text,
												 NameGetDatum(str)));
	result = (Generic_Text_IC_like(strtext, pat, PG_GET_COLLATION()) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
texticlike(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	text	   *pat = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = (Generic_Text_IC_like(str, pat, PG_GET_COLLATION()) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
texticnlike(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	text	   *pat = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = (Generic_Text_IC_like(str, pat, PG_GET_COLLATION()) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

/*
 * like_escape() --- 给定模式和 ESCAPE 串，
 * 把模式转换为使用 Postgres 的标准反斜杠转义约定。
 */
Datum
like_escape(PG_FUNCTION_ARGS)
{
	text	   *pat = PG_GETARG_TEXT_PP(0);
	text	   *esc = PG_GETARG_TEXT_PP(1);
	text	   *result;

	if (pg_database_encoding_max_length() == 1)
		result = SB_do_like_escape(pat, esc);
	else
		result = MB_do_like_escape(pat, esc);

	PG_RETURN_TEXT_P(result);
}

/*
 * like_escape_bytea() --- 给定模式和 ESCAPE 串，
 * 把模式转换为使用 Postgres 的标准反斜杠转义约定。
 */
Datum
like_escape_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *pat = PG_GETARG_BYTEA_PP(0);
	bytea	   *esc = PG_GETARG_BYTEA_PP(1);
	bytea	   *result = SB_do_like_escape((text *) pat, (text *) esc);

	PG_RETURN_BYTEA_P((bytea *) result);
}
