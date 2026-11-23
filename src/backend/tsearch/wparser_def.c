/*-------------------------------------------------------------------------
 *
 * wparser_def.c
 *		Default text search parser
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/wparser_def.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_public.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"


/* 定义本宏以启用解析器行为跟踪 */
/* #define WPARSER_TRACE */


/* 输出词元类别 */

#define ASCIIWORD		1
#define WORD_T			2
#define NUMWORD			3
#define EMAIL			4
#define URL_T			5
#define HOST			6
#define SCIENTIFIC		7
#define VERSIONNUMBER	8
#define NUMPARTHWORD	9
#define PARTHWORD		10
#define ASCIIPARTHWORD	11
#define SPACE			12
#define TAG_T			13
#define PROTOCOL		14
#define NUMHWORD		15
#define ASCIIHWORD		16
#define HWORD			17
#define URLPATH			18
#define FILEPATH		19
#define DECIMAL_T		20
#define SIGNEDINT		21
#define UNSIGNEDINT		22
#define XMLENTITY		23

#define LASTNUM			23

static const char *const tok_alias[] = {
	"",
	"asciiword",
	"word",
	"numword",
	"email",
	"url",
	"host",
	"sfloat",
	"version",
	"hword_numpart",
	"hword_part",
	"hword_asciipart",
	"blank",
	"tag",
	"protocol",
	"numhword",
	"asciihword",
	"hword",
	"url_path",
	"file",
	"float",
	"int",
	"uint",
	"entity"
};

static const char *const lex_descr[] = {
	"",
	"Word, all ASCII",
	"Word, all letters",
	"Word, letters and digits",
	"Email address",
	"URL",
	"Host",
	"Scientific notation",
	"Version number",
	"Hyphenated word part, letters and digits",
	"Hyphenated word part, all letters",
	"Hyphenated word part, all ASCII",
	"Space symbols",
	"XML tag",
	"Protocol head",
	"Hyphenated word, letters and digits",
	"Hyphenated word, all ASCII",
	"Hyphenated word, all letters",
	"URL path",
	"File or path name",
	"Decimal notation",
	"Signed integer",
	"Unsigned integer",
	"XML entity"
};


/* 解析状态 */

typedef enum
{
	TPS_Base = 0,
	TPS_InNumWord,
	TPS_InAsciiWord,
	TPS_InWord,
	TPS_InUnsignedInt,
	TPS_InSignedIntFirst,
	TPS_InSignedInt,
	TPS_InSpace,
	TPS_InUDecimalFirst,
	TPS_InUDecimal,
	TPS_InDecimalFirst,
	TPS_InDecimal,
	TPS_InVerVersion,
	TPS_InSVerVersion,
	TPS_InVersionFirst,
	TPS_InVersion,
	TPS_InMantissaFirst,
	TPS_InMantissaSign,
	TPS_InMantissa,
	TPS_InXMLEntityFirst,
	TPS_InXMLEntity,
	TPS_InXMLEntityNumFirst,
	TPS_InXMLEntityNum,
	TPS_InXMLEntityHexNumFirst,
	TPS_InXMLEntityHexNum,
	TPS_InXMLEntityEnd,
	TPS_InTagFirst,
	TPS_InXMLBegin,
	TPS_InTagCloseFirst,
	TPS_InTagName,
	TPS_InTagBeginEnd,
	TPS_InTag,
	TPS_InTagEscapeK,
	TPS_InTagEscapeKK,
	TPS_InTagBackSleshed,
	TPS_InTagEnd,
	TPS_InCommentFirst,
	TPS_InCommentLast,
	TPS_InComment,
	TPS_InCloseCommentFirst,
	TPS_InCloseCommentLast,
	TPS_InCommentEnd,
	TPS_InHostFirstDomain,
	TPS_InHostDomainSecond,
	TPS_InHostDomain,
	TPS_InPortFirst,
	TPS_InPort,
	TPS_InHostFirstAN,
	TPS_InHost,
	TPS_InEmail,
	TPS_InFileFirst,
	TPS_InFileTwiddle,
	TPS_InPathFirst,
	TPS_InPathFirstFirst,
	TPS_InPathSecond,
	TPS_InFile,
	TPS_InFileNext,
	TPS_InURLPathFirst,
	TPS_InURLPathStart,
	TPS_InURLPath,
	TPS_InFURL,
	TPS_InProtocolFirst,
	TPS_InProtocolSecond,
	TPS_InProtocolEnd,
	TPS_InHyphenAsciiWordFirst,
	TPS_InHyphenAsciiWord,
	TPS_InHyphenWordFirst,
	TPS_InHyphenWord,
	TPS_InHyphenNumWordFirst,
	TPS_InHyphenNumWord,
	TPS_InHyphenDigitLookahead,
	TPS_InParseHyphen,
	TPS_InParseHyphenHyphen,
	TPS_InHyphenWordPart,
	TPS_InHyphenAsciiWordPart,
	TPS_InHyphenNumWordPart,
	TPS_InHyphenUnsignedInt,
	TPS_Null					/* 末状态(伪值) */
} TParserState;

/* 前置声明 */
struct TParser;

typedef int (*TParserCharTest) (struct TParser *);	/* 任一 p_is* 函数
													 * 除 p_iseq */
typedef void (*TParserSpecial) (struct TParser *);	/* 特殊情形的
													 * 特殊处理... */

typedef struct
{
	TParserCharTest isclass;
	char		c;
	uint16		flags;
	TParserState tostate;
	int			type;
	TParserSpecial special;
} TParserStateActionItem;

/* TParserStateActionItem.flags 的标志位 */
#define A_NEXT		0x0000
#define A_BINGO		0x0001
#define A_POP		0x0002
#define A_PUSH		0x0004
#define A_RERUN		0x0008
#define A_CLEAR		0x0010
#define A_MERGE		0x0020
#define A_CLRALL	0x0040

typedef struct TParserPosition
{
	int			posbyte;		/* 解析器按字节的位置 */
	int			poschar;		/* 解析器按字符的位置 */
	int			charlen;		/* 当前字符长度 */
	int			lenbytetoken;	/* 迄今词元的字节长度 */
	int			lenchartoken;	/* 及字符数 */
	TParserState state;
	struct TParserPosition *prev;
	const TParserStateActionItem *pushedAtAction;
} TParserPosition;

typedef struct TParser
{
	/* 串与位置信息 */
	char	   *str;			/* 多字节串 */
	int			lenstr;			/* mbstring 长度 */
	wchar_t    *wstr;			/* 宽字符串 */
	pg_wchar   *pgwstr;			/* C 区域设置用宽字符串 */
	bool		usewide;

	/* 解析状态 */
	int			charmaxlen;
	TParserPosition *state;
	bool		ignore;
	bool		wanthost;

	/* 杂字符 */
	char		c;

	/* 出 */
	char	   *token;
	int			lenbytetoken;
	int			lenchartoken;
	int			type;
} TParser;


/* 此处前置声明 */
static bool TParserGet(TParser *prs);


static TParserPosition *
newTParserPosition(TParserPosition *prev)
{
	TParserPosition *res = (TParserPosition *) palloc(sizeof(TParserPosition));

	if (prev)
		memcpy(res, prev, sizeof(TParserPosition));
	else
		memset(res, 0, sizeof(TParserPosition));

	res->prev = prev;

	res->pushedAtAction = NULL;

	return res;
}

static TParser *
TParserInit(char *str, int len)
{
	TParser    *prs = (TParser *) palloc0(sizeof(TParser));

	prs->charmaxlen = pg_database_encoding_max_length();
	prs->str = str;
	prs->lenstr = len;

	/*
	 * 仅当最大编码长度 > 1 时用宽字符代码。
	 */
	if (prs->charmaxlen > 1)
	{
		pg_locale_t mylocale = 0;	/* TODO */

		prs->usewide = true;
		if (database_ctype_is_c)
		{
			/*
			 * char2wchar 对 C 区域设置无效,且 sizeof(pg_wchar)
			 * 可能异于 sizeof(wchar_t)
			 */
			prs->pgwstr = (pg_wchar *) palloc(sizeof(pg_wchar) * (prs->lenstr + 1));
			pg_mb2wchar_with_len(prs->str, prs->pgwstr, prs->lenstr);
		}
		else
		{
			prs->wstr = (wchar_t *) palloc(sizeof(wchar_t) * (prs->lenstr + 1));
			char2wchar(prs->wstr, prs->lenstr + 1, prs->str, prs->lenstr,
					   mylocale);
		}
	}
	else
		prs->usewide = false;

	prs->state = newTParserPosition(NULL);
	prs->state->state = TPS_Base;

#ifdef WPARSER_TRACE
	fprintf(stderr, "parsing \"%.*s\"\n", len, str);
#endif

	return prs;
}

/*
 * 相对完整 TParserInit,也可造
 * TParserCopy:本质是普通 TParser,但不
 * 私有拷贝串,而用另一 TParser 的串。
 * 递归创建 TParsers 时很有用,避免长源
 * 串反复拷贝造成严重低效。
 * 新解析器从原解析器当前位置开始
 * 解析。
 *
 * 显然关闭副本前不得关闭原 TParser。
 */
static TParser *
TParserCopyInit(const TParser *orig)
{
	TParser    *prs = (TParser *) palloc0(sizeof(TParser));

	prs->charmaxlen = orig->charmaxlen;
	prs->str = orig->str + orig->state->posbyte;
	prs->lenstr = orig->lenstr - orig->state->posbyte;
	prs->usewide = orig->usewide;

	if (orig->pgwstr)
		prs->pgwstr = orig->pgwstr + orig->state->poschar;
	if (orig->wstr)
		prs->wstr = orig->wstr + orig->state->poschar;

	prs->state = newTParserPosition(NULL);
	prs->state->state = TPS_Base;

#ifdef WPARSER_TRACE
	fprintf(stderr, "parsing copy of \"%.*s\"\n", prs->lenstr, prs->str);
#endif

	return prs;
}


static void
TParserClose(TParser *prs)
{
	while (prs->state)
	{
		TParserPosition *ptr = prs->state->prev;

		pfree(prs->state);
		prs->state = ptr;
	}

	if (prs->wstr)
		pfree(prs->wstr);
	if (prs->pgwstr)
		pfree(prs->pgwstr);

#ifdef WPARSER_TRACE
	fprintf(stderr, "closing parser\n");
#endif
	pfree(prs);
}

/*
 * 关闭 TParserCopyInit 创建的解析器
 */
static void
TParserCopyClose(TParser *prs)
{
	while (prs->state)
	{
		TParserPosition *ptr = prs->state->prev;

		pfree(prs->state);
		prs->state = ptr;
	}

#ifdef WPARSER_TRACE
	fprintf(stderr, "closing parser copy\n");
#endif
	pfree(prs);
}


/*
 * 字符类型支持函数,等价于 is* 宏,但
 * 可处理任意编码与区域设置。注意:
 *	- 多字节编码与 C 区域设置下 isw* 可能
 *	  失败或结果错误
 *	- 多字节编码与 C 区域设置常用于
 *	  亚洲语言。
 *	- 若区域设置为 C 则用 pgwstr 而非 wstr。
 */

#define p_iswhat(type, nonascii)											\
																			\
static int																	\
p_is##type(TParser *prs)													\
{																			\
	Assert(prs->state);														\
	if (prs->usewide)														\
	{																		\
		if (prs->pgwstr)													\
		{																	\
			unsigned int c = *(prs->pgwstr + prs->state->poschar);			\
			if (c > 0x7f)													\
				return nonascii;											\
			return is##type(c);												\
		}																	\
		return isw##type(*(prs->wstr + prs->state->poschar));				\
	}																		\
	return is##type(*(unsigned char *) (prs->str + prs->state->posbyte));	\
}																			\
																			\
static int																	\
p_isnot##type(TParser *prs)													\
{																			\
	return !p_is##type(prs);												\
}

/*
 * 在 C 区域设置且多字节编码下,任何非 ASCII
 * 符号视为 alpha,但不属其他字符类。
 */
p_iswhat(alnum, 1)
p_iswhat(alpha, 1)
p_iswhat(digit, 0)
p_iswhat(lower, 0)
p_iswhat(print, 0)
p_iswhat(punct, 0)
p_iswhat(space, 0)
p_iswhat(upper, 0)
p_iswhat(xdigit, 0)

/* p_iseq 应只用于 ascii 符号 */

static int
p_iseq(TParser *prs, char c)
{
	Assert(prs->state);
	return ((prs->state->charlen == 1 && *(prs->str + prs->state->posbyte) == c)) ? 1 : 0;
}

static int
p_isEOF(TParser *prs)
{
	Assert(prs->state);
	return (prs->state->posbyte == prs->lenstr || prs->state->charlen == 0) ? 1 : 0;
}

static int
p_iseqC(TParser *prs)
{
	return p_iseq(prs, prs->c);
}

static int
p_isneC(TParser *prs)
{
	return !p_iseq(prs, prs->c);
}

static int
p_isascii(TParser *prs)
{
	return (prs->state->charlen == 1 && isascii((unsigned char) *(prs->str + prs->state->posbyte))) ? 1 : 0;
}

static int
p_isasclet(TParser *prs)
{
	return (p_isascii(prs) && p_isalpha(prs)) ? 1 : 0;
}

static int
p_isurlchar(TParser *prs)
{
	char		ch;

	/* 非 ASCII 无需适用 */
	if (prs->state->charlen != 1)
		return 0;
	ch = *(prs->str + prs->state->posbyte);
	/* 不要空白或控制字符 */
	if (ch <= 0x20 || ch >= 0x7F)
		return 0;
	/* 拒绝 RFC 3986 禁止的字符 */
	switch (ch)
	{
		case '"':
		case '<':
		case '>':
		case '\\':
		case '^':
		case '`':
		case '{':
		case '|':
		case '}':
			return 0;
	}
	return 1;
}


/* 有意压制上述未用函数的告警 */
void		_make_compiler_happy(void);
void
_make_compiler_happy(void)
{
	p_isalnum(NULL);
	p_isnotalnum(NULL);
	p_isalpha(NULL);
	p_isnotalpha(NULL);
	p_isdigit(NULL);
	p_isnotdigit(NULL);
	p_islower(NULL);
	p_isnotlower(NULL);
	p_isprint(NULL);
	p_isnotprint(NULL);
	p_ispunct(NULL);
	p_isnotpunct(NULL);
	p_isspace(NULL);
	p_isnotspace(NULL);
	p_isupper(NULL);
	p_isnotupper(NULL);
	p_isxdigit(NULL);
	p_isnotxdigit(NULL);
	p_isEOF(NULL);
	p_iseqC(NULL);
	p_isneC(NULL);
}


static void
SpecialTags(TParser *prs)
{
	switch (prs->state->lenchartoken)
	{
		case 8:					/* </script */
			if (pg_strncasecmp(prs->token, "</script", 8) == 0)
				prs->ignore = false;
			break;
		case 7:					/* <script || </style */
			if (pg_strncasecmp(prs->token, "</style", 7) == 0)
				prs->ignore = false;
			else if (pg_strncasecmp(prs->token, "<script", 7) == 0)
				prs->ignore = true;
			break;
		case 6:					/* <style */
			if (pg_strncasecmp(prs->token, "<style", 6) == 0)
				prs->ignore = true;
			break;
		default:
			break;
	}
}

static void
SpecialFURL(TParser *prs)
{
	prs->wanthost = true;
	prs->state->posbyte -= prs->state->lenbytetoken;
	prs->state->poschar -= prs->state->lenchartoken;
}

static void
SpecialHyphen(TParser *prs)
{
	prs->state->posbyte -= prs->state->lenbytetoken;
	prs->state->poschar -= prs->state->lenchartoken;
}

static void
SpecialVerVersion(TParser *prs)
{
	prs->state->posbyte -= prs->state->lenbytetoken;
	prs->state->poschar -= prs->state->lenchartoken;
	prs->state->lenbytetoken = 0;
	prs->state->lenchartoken = 0;
}

static int
p_isstophost(TParser *prs)
{
	if (prs->wanthost)
	{
		prs->wanthost = false;
		return 1;
	}
	return 0;
}

static int
p_isignore(TParser *prs)
{
	return (prs->ignore) ? 1 : 0;
}

static int
p_ishost(TParser *prs)
{
	TParser    *tmpprs = TParserCopyInit(prs);
	int			res = 0;

	tmpprs->wanthost = true;

	/*
	 * 递归前检查栈深。(TParserGet() 通常
	 * 不递归,故把检查开销放在此处。)
	 */
	check_stack_depth();

	if (TParserGet(tmpprs) && tmpprs->type == HOST)
	{
		prs->state->posbyte += tmpprs->lenbytetoken;
		prs->state->poschar += tmpprs->lenchartoken;
		prs->state->lenbytetoken += tmpprs->lenbytetoken;
		prs->state->lenchartoken += tmpprs->lenchartoken;
		prs->state->charlen = tmpprs->state->charlen;
		res = 1;
	}
	TParserCopyClose(tmpprs);

	return res;
}

static int
p_isURLPath(TParser *prs)
{
	TParser    *tmpprs = TParserCopyInit(prs);
	int			res = 0;

	tmpprs->state = newTParserPosition(tmpprs->state);
	tmpprs->state->state = TPS_InURLPathFirst;

	/*
	 * 递归前检查栈深。(TParserGet() 通常
	 * 不递归,故把检查开销放在此处。)
	 */
	check_stack_depth();

	if (TParserGet(tmpprs) && tmpprs->type == URLPATH)
	{
		prs->state->posbyte += tmpprs->lenbytetoken;
		prs->state->poschar += tmpprs->lenchartoken;
		prs->state->lenbytetoken += tmpprs->lenbytetoken;
		prs->state->lenchartoken += tmpprs->lenchartoken;
		prs->state->charlen = tmpprs->state->charlen;
		res = 1;
	}
	TParserCopyClose(tmpprs);

	return res;
}

/*
 * 若当前字符显示长度为 0,或为若干
 * 语言中的特殊记号,则返回真。此类字符
 * 虽非 isalpha,但不是断词符。
 * 在词首时它们不构成词的一部分。
 */
static int
p_isspecial(TParser *prs)
{
	/*
	 * pg_dsplen 可能返回 -1,表示出错或控制字符
	 */
	if (pg_dsplen(prs->str + prs->state->posbyte) == 0)
		return 1;

	/*
	 * Unicode Characters in the 'Mark, Spacing Combining' Category That
	 * characters are not alpha although they are not breakers of word too.
	 * Check that only in utf encoding, because other encodings aren't
	 * supported by postgres or even exists.
	 */
	if (GetDatabaseEncoding() == PG_UTF8 && prs->usewide)
	{
		static const pg_wchar strange_letter[] = {
			/*
			 * 用二分查找,故元素须有序
			 */
			0x0903,				/* DEVANAGARI SIGN VISARGA */
			0x093E,				/* DEVANAGARI VOWEL SIGN AA */
			0x093F,				/* DEVANAGARI VOWEL SIGN I */
			0x0940,				/* DEVANAGARI VOWEL SIGN II */
			0x0949,				/* DEVANAGARI VOWEL SIGN CANDRA O */
			0x094A,				/* DEVANAGARI VOWEL SIGN SHORT O */
			0x094B,				/* DEVANAGARI VOWEL SIGN O */
			0x094C,				/* DEVANAGARI VOWEL SIGN AU */
			0x0982,				/* BENGALI SIGN ANUSVARA */
			0x0983,				/* BENGALI SIGN VISARGA */
			0x09BE,				/* BENGALI VOWEL SIGN AA */
			0x09BF,				/* BENGALI VOWEL SIGN I */
			0x09C0,				/* BENGALI VOWEL SIGN II */
			0x09C7,				/* BENGALI VOWEL SIGN E */
			0x09C8,				/* BENGALI VOWEL SIGN AI */
			0x09CB,				/* BENGALI VOWEL SIGN O */
			0x09CC,				/* BENGALI VOWEL SIGN AU */
			0x09D7,				/* BENGALI AU LENGTH MARK */
			0x0A03,				/* GURMUKHI SIGN VISARGA */
			0x0A3E,				/* GURMUKHI VOWEL SIGN AA */
			0x0A3F,				/* GURMUKHI VOWEL SIGN I */
			0x0A40,				/* GURMUKHI VOWEL SIGN II */
			0x0A83,				/* GUJARATI SIGN VISARGA */
			0x0ABE,				/* GUJARATI VOWEL SIGN AA */
			0x0ABF,				/* GUJARATI VOWEL SIGN I */
			0x0AC0,				/* GUJARATI VOWEL SIGN II */
			0x0AC9,				/* GUJARATI VOWEL SIGN CANDRA O */
			0x0ACB,				/* GUJARATI VOWEL SIGN O */
			0x0ACC,				/* GUJARATI VOWEL SIGN AU */
			0x0B02,				/* ORIYA SIGN ANUSVARA */
			0x0B03,				/* ORIYA SIGN VISARGA */
			0x0B3E,				/* ORIYA VOWEL SIGN AA */
			0x0B40,				/* ORIYA VOWEL SIGN II */
			0x0B47,				/* ORIYA VOWEL SIGN E */
			0x0B48,				/* ORIYA VOWEL SIGN AI */
			0x0B4B,				/* ORIYA VOWEL SIGN O */
			0x0B4C,				/* ORIYA VOWEL SIGN AU */
			0x0B57,				/* ORIYA AU LENGTH MARK */
			0x0BBE,				/* TAMIL VOWEL SIGN AA */
			0x0BBF,				/* TAMIL VOWEL SIGN I */
			0x0BC1,				/* TAMIL VOWEL SIGN U */
			0x0BC2,				/* TAMIL VOWEL SIGN UU */
			0x0BC6,				/* TAMIL VOWEL SIGN E */
			0x0BC7,				/* TAMIL VOWEL SIGN EE */
			0x0BC8,				/* TAMIL VOWEL SIGN AI */
			0x0BCA,				/* TAMIL VOWEL SIGN O */
			0x0BCB,				/* TAMIL VOWEL SIGN OO */
			0x0BCC,				/* TAMIL VOWEL SIGN AU */
			0x0BD7,				/* TAMIL AU LENGTH MARK */
			0x0C01,				/* TELUGU SIGN CANDRABINDU */
			0x0C02,				/* TELUGU SIGN ANUSVARA */
			0x0C03,				/* TELUGU SIGN VISARGA */
			0x0C41,				/* TELUGU VOWEL SIGN U */
			0x0C42,				/* TELUGU VOWEL SIGN UU */
			0x0C43,				/* TELUGU VOWEL SIGN VOCALIC R */
			0x0C44,				/* TELUGU VOWEL SIGN VOCALIC RR */
			0x0C82,				/* KANNADA SIGN ANUSVARA */
			0x0C83,				/* KANNADA SIGN VISARGA */
			0x0CBE,				/* KANNADA VOWEL SIGN AA */
			0x0CC0,				/* KANNADA VOWEL SIGN II */
			0x0CC1,				/* KANNADA VOWEL SIGN U */
			0x0CC2,				/* KANNADA VOWEL SIGN UU */
			0x0CC3,				/* KANNADA VOWEL SIGN VOCALIC R */
			0x0CC4,				/* KANNADA VOWEL SIGN VOCALIC RR */
			0x0CC7,				/* KANNADA VOWEL SIGN EE */
			0x0CC8,				/* KANNADA VOWEL SIGN AI */
			0x0CCA,				/* KANNADA VOWEL SIGN O */
			0x0CCB,				/* KANNADA VOWEL SIGN OO */
			0x0CD5,				/* KANNADA LENGTH MARK */
			0x0CD6,				/* KANNADA AI LENGTH MARK */
			0x0D02,				/* MALAYALAM SIGN ANUSVARA */
			0x0D03,				/* MALAYALAM SIGN VISARGA */
			0x0D3E,				/* MALAYALAM VOWEL SIGN AA */
			0x0D3F,				/* MALAYALAM VOWEL SIGN I */
			0x0D40,				/* MALAYALAM VOWEL SIGN II */
			0x0D46,				/* MALAYALAM VOWEL SIGN E */
			0x0D47,				/* MALAYALAM VOWEL SIGN EE */
			0x0D48,				/* MALAYALAM VOWEL SIGN AI */
			0x0D4A,				/* MALAYALAM VOWEL SIGN O */
			0x0D4B,				/* MALAYALAM VOWEL SIGN OO */
			0x0D4C,				/* MALAYALAM VOWEL SIGN AU */
			0x0D57,				/* MALAYALAM AU LENGTH MARK */
			0x0D82,				/* SINHALA SIGN ANUSVARAYA */
			0x0D83,				/* SINHALA SIGN VISARGAYA */
			0x0DCF,				/* SINHALA VOWEL SIGN AELA-PILLA */
			0x0DD0,				/* SINHALA VOWEL SIGN KETTI AEDA-PILLA */
			0x0DD1,				/* SINHALA VOWEL SIGN DIGA AEDA-PILLA */
			0x0DD8,				/* SINHALA VOWEL SIGN GAETTA-PILLA */
			0x0DD9,				/* SINHALA VOWEL SIGN KOMBUVA */
			0x0DDA,				/* SINHALA VOWEL SIGN DIGA KOMBUVA */
			0x0DDB,				/* SINHALA VOWEL SIGN KOMBU DEKA */
			0x0DDC,				/* SINHALA VOWEL SIGN KOMBUVA HAA AELA-PILLA */
			0x0DDD,				/* SINHALA VOWEL SIGN KOMBUVA HAA DIGA
								 * AELA-PILLA */
			0x0DDE,				/* SINHALA VOWEL SIGN KOMBUVA HAA GAYANUKITTA */
			0x0DDF,				/* SINHALA VOWEL SIGN GAYANUKITTA */
			0x0DF2,				/* SINHALA VOWEL SIGN DIGA GAETTA-PILLA */
			0x0DF3,				/* SINHALA VOWEL SIGN DIGA GAYANUKITTA */
			0x0F3E,				/* TIBETAN SIGN YAR TSHES */
			0x0F3F,				/* TIBETAN SIGN MAR TSHES */
			0x0F7F,				/* TIBETAN SIGN RNAM BCAD */
			0x102B,				/* MYANMAR VOWEL SIGN TALL AA */
			0x102C,				/* MYANMAR VOWEL SIGN AA */
			0x1031,				/* MYANMAR VOWEL SIGN E */
			0x1038,				/* MYANMAR SIGN VISARGA */
			0x103B,				/* MYANMAR CONSONANT SIGN MEDIAL YA */
			0x103C,				/* MYANMAR CONSONANT SIGN MEDIAL RA */
			0x1056,				/* MYANMAR VOWEL SIGN VOCALIC R */
			0x1057,				/* MYANMAR VOWEL SIGN VOCALIC RR */
			0x1062,				/* MYANMAR VOWEL SIGN SGAW KAREN EU */
			0x1063,				/* MYANMAR TONE MARK SGAW KAREN HATHI */
			0x1064,				/* MYANMAR TONE MARK SGAW KAREN KE PHO */
			0x1067,				/* MYANMAR VOWEL SIGN WESTERN PWO KAREN EU */
			0x1068,				/* MYANMAR VOWEL SIGN WESTERN PWO KAREN UE */
			0x1069,				/* MYANMAR SIGN WESTERN PWO KAREN TONE-1 */
			0x106A,				/* MYANMAR SIGN WESTERN PWO KAREN TONE-2 */
			0x106B,				/* MYANMAR SIGN WESTERN PWO KAREN TONE-3 */
			0x106C,				/* MYANMAR SIGN WESTERN PWO KAREN TONE-4 */
			0x106D,				/* MYANMAR SIGN WESTERN PWO KAREN TONE-5 */
			0x1083,				/* MYANMAR VOWEL SIGN SHAN AA */
			0x1084,				/* MYANMAR VOWEL SIGN SHAN E */
			0x1087,				/* MYANMAR SIGN SHAN TONE-2 */
			0x1088,				/* MYANMAR SIGN SHAN TONE-3 */
			0x1089,				/* MYANMAR SIGN SHAN TONE-5 */
			0x108A,				/* MYANMAR SIGN SHAN TONE-6 */
			0x108B,				/* MYANMAR SIGN SHAN COUNCIL TONE-2 */
			0x108C,				/* MYANMAR SIGN SHAN COUNCIL TONE-3 */
			0x108F,				/* MYANMAR SIGN RUMAI PALAUNG TONE-5 */
			0x17B6,				/* KHMER VOWEL SIGN AA */
			0x17BE,				/* KHMER VOWEL SIGN OE */
			0x17BF,				/* KHMER VOWEL SIGN YA */
			0x17C0,				/* KHMER VOWEL SIGN IE */
			0x17C1,				/* KHMER VOWEL SIGN E */
			0x17C2,				/* KHMER VOWEL SIGN AE */
			0x17C3,				/* KHMER VOWEL SIGN AI */
			0x17C4,				/* KHMER VOWEL SIGN OO */
			0x17C5,				/* KHMER VOWEL SIGN AU */
			0x17C7,				/* KHMER SIGN REAHMUK */
			0x17C8,				/* KHMER SIGN YUUKALEAPINTU */
			0x1923,				/* LIMBU VOWEL SIGN EE */
			0x1924,				/* LIMBU VOWEL SIGN AI */
			0x1925,				/* LIMBU VOWEL SIGN OO */
			0x1926,				/* LIMBU VOWEL SIGN AU */
			0x1929,				/* LIMBU SUBJOINED LETTER YA */
			0x192A,				/* LIMBU SUBJOINED LETTER RA */
			0x192B,				/* LIMBU SUBJOINED LETTER WA */
			0x1930,				/* LIMBU SMALL LETTER KA */
			0x1931,				/* LIMBU SMALL LETTER NGA */
			0x1933,				/* LIMBU SMALL LETTER TA */
			0x1934,				/* LIMBU SMALL LETTER NA */
			0x1935,				/* LIMBU SMALL LETTER PA */
			0x1936,				/* LIMBU SMALL LETTER MA */
			0x1937,				/* LIMBU SMALL LETTER RA */
			0x1938,				/* LIMBU SMALL LETTER LA */
			0x19B0,				/* NEW TAI LUE VOWEL SIGN VOWEL SHORTENER */
			0x19B1,				/* NEW TAI LUE VOWEL SIGN AA */
			0x19B2,				/* NEW TAI LUE VOWEL SIGN II */
			0x19B3,				/* NEW TAI LUE VOWEL SIGN U */
			0x19B4,				/* NEW TAI LUE VOWEL SIGN UU */
			0x19B5,				/* NEW TAI LUE VOWEL SIGN E */
			0x19B6,				/* NEW TAI LUE VOWEL SIGN AE */
			0x19B7,				/* NEW TAI LUE VOWEL SIGN O */
			0x19B8,				/* NEW TAI LUE VOWEL SIGN OA */
			0x19B9,				/* NEW TAI LUE VOWEL SIGN UE */
			0x19BA,				/* NEW TAI LUE VOWEL SIGN AY */
			0x19BB,				/* NEW TAI LUE VOWEL SIGN AAY */
			0x19BC,				/* NEW TAI LUE VOWEL SIGN UY */
			0x19BD,				/* NEW TAI LUE VOWEL SIGN OY */
			0x19BE,				/* NEW TAI LUE VOWEL SIGN OAY */
			0x19BF,				/* NEW TAI LUE VOWEL SIGN UEY */
			0x19C0,				/* NEW TAI LUE VOWEL SIGN IY */
			0x19C8,				/* NEW TAI LUE TONE MARK-1 */
			0x19C9,				/* NEW TAI LUE TONE MARK-2 */
			0x1A19,				/* BUGINESE VOWEL SIGN E */
			0x1A1A,				/* BUGINESE VOWEL SIGN O */
			0x1A1B,				/* BUGINESE VOWEL SIGN AE */
			0x1B04,				/* BALINESE SIGN BISAH */
			0x1B35,				/* BALINESE VOWEL SIGN TEDUNG */
			0x1B3B,				/* BALINESE VOWEL SIGN RA REPA TEDUNG */
			0x1B3D,				/* BALINESE VOWEL SIGN LA LENGA TEDUNG */
			0x1B3E,				/* BALINESE VOWEL SIGN TALING */
			0x1B3F,				/* BALINESE VOWEL SIGN TALING REPA */
			0x1B40,				/* BALINESE VOWEL SIGN TALING TEDUNG */
			0x1B41,				/* BALINESE VOWEL SIGN TALING REPA TEDUNG */
			0x1B43,				/* BALINESE VOWEL SIGN PEPET TEDUNG */
			0x1B44,				/* BALINESE ADEG ADEG */
			0x1B82,				/* SUNDANESE SIGN PANGWISAD */
			0x1BA1,				/* SUNDANESE CONSONANT SIGN PAMINGKAL */
			0x1BA6,				/* SUNDANESE VOWEL SIGN PANAELAENG */
			0x1BA7,				/* SUNDANESE VOWEL SIGN PANOLONG */
			0x1BAA,				/* SUNDANESE SIGN PAMAAEH */
			0x1C24,				/* LEPCHA SUBJOINED LETTER YA */
			0x1C25,				/* LEPCHA SUBJOINED LETTER RA */
			0x1C26,				/* LEPCHA VOWEL SIGN AA */
			0x1C27,				/* LEPCHA VOWEL SIGN I */
			0x1C28,				/* LEPCHA VOWEL SIGN O */
			0x1C29,				/* LEPCHA VOWEL SIGN OO */
			0x1C2A,				/* LEPCHA VOWEL SIGN U */
			0x1C2B,				/* LEPCHA VOWEL SIGN UU */
			0x1C34,				/* LEPCHA CONSONANT SIGN NYIN-DO */
			0x1C35,				/* LEPCHA CONSONANT SIGN KANG */
			0xA823,				/* SYLOTI NAGRI VOWEL SIGN A */
			0xA824,				/* SYLOTI NAGRI VOWEL SIGN I */
			0xA827,				/* SYLOTI NAGRI VOWEL SIGN OO */
			0xA880,				/* SAURASHTRA SIGN ANUSVARA */
			0xA881,				/* SAURASHTRA SIGN VISARGA */
			0xA8B4,				/* SAURASHTRA CONSONANT SIGN HAARU */
			0xA8B5,				/* SAURASHTRA VOWEL SIGN AA */
			0xA8B6,				/* SAURASHTRA VOWEL SIGN I */
			0xA8B7,				/* SAURASHTRA VOWEL SIGN II */
			0xA8B8,				/* SAURASHTRA VOWEL SIGN U */
			0xA8B9,				/* SAURASHTRA VOWEL SIGN UU */
			0xA8BA,				/* SAURASHTRA VOWEL SIGN VOCALIC R */
			0xA8BB,				/* SAURASHTRA VOWEL SIGN VOCALIC RR */
			0xA8BC,				/* SAURASHTRA VOWEL SIGN VOCALIC L */
			0xA8BD,				/* SAURASHTRA VOWEL SIGN VOCALIC LL */
			0xA8BE,				/* SAURASHTRA VOWEL SIGN E */
			0xA8BF,				/* SAURASHTRA VOWEL SIGN EE */
			0xA8C0,				/* SAURASHTRA VOWEL SIGN AI */
			0xA8C1,				/* SAURASHTRA VOWEL SIGN O */
			0xA8C2,				/* SAURASHTRA VOWEL SIGN OO */
			0xA8C3,				/* SAURASHTRA VOWEL SIGN AU */
			0xA952,				/* REJANG CONSONANT SIGN H */
			0xA953,				/* REJANG VIRAMA */
			0xAA2F,				/* CHAM VOWEL SIGN O */
			0xAA30,				/* CHAM VOWEL SIGN AI */
			0xAA33,				/* CHAM CONSONANT SIGN YA */
			0xAA34,				/* CHAM CONSONANT SIGN RA */
			0xAA4D				/* CHAM CONSONANT SIGN FINAL H */
		};
		const pg_wchar *StopLow = strange_letter,
				   *StopHigh = strange_letter + lengthof(strange_letter),
				   *StopMiddle;
		pg_wchar	c;

		if (prs->pgwstr)
			c = *(prs->pgwstr + prs->state->poschar);
		else
			c = (pg_wchar) *(prs->wstr + prs->state->poschar);

		while (StopLow < StopHigh)
		{
			StopMiddle = StopLow + ((StopHigh - StopLow) >> 1);
			if (*StopMiddle == c)
				return 1;
			else if (*StopMiddle < c)
				StopLow = StopMiddle + 1;
			else
				StopHigh = StopMiddle;
		}
	}

	return 0;
}

/*
 * 解析器状态/动作表
 */

static const TParserStateActionItem actionTPS_Base[] = {
	{p_isEOF, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '<', A_PUSH, TPS_InTagFirst, 0, NULL},
	{p_isignore, 0, A_NEXT, TPS_InSpace, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InAsciiWord, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InUnsignedInt, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InSignedIntFirst, 0, NULL},
	{p_iseqC, '+', A_PUSH, TPS_InSignedIntFirst, 0, NULL},
	{p_iseqC, '&', A_PUSH, TPS_InXMLEntityFirst, 0, NULL},
	{p_iseqC, '~', A_PUSH, TPS_InFileTwiddle, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InPathFirstFirst, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InSpace, 0, NULL}
};


static const TParserStateActionItem actionTPS_InNumWord[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, NUMWORD, NULL},
	{p_isalnum, 0, A_NEXT, TPS_InNumWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InNumWord, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InFileNext, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenNumWordFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, NUMWORD, NULL}
};

static const TParserStateActionItem actionTPS_InAsciiWord[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, ASCIIWORD, NULL},
	{p_isasclet, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InFileNext, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenAsciiWordFirst, 0, NULL},
	{p_iseqC, '_', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_iseqC, ':', A_PUSH, TPS_InProtocolFirst, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{p_isdigit, 0, A_PUSH, TPS_InHost, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InNumWord, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InWord, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, ASCIIWORD, NULL}
};

static const TParserStateActionItem actionTPS_InWord[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, WORD_T, NULL},
	{p_isalpha, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InNumWord, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenWordFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, WORD_T, NULL}
};

static const TParserStateActionItem actionTPS_InUnsignedInt[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, UNSIGNEDINT, NULL},
	{p_isdigit, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InUDecimalFirst, 0, NULL},
	{p_iseqC, 'e', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, 'E', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '_', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_isasclet, 0, A_PUSH, TPS_InHost, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InNumWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InNumWord, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, UNSIGNEDINT, NULL}
};

static const TParserStateActionItem actionTPS_InSignedIntFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT | A_CLEAR, TPS_InSignedInt, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InSignedInt[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, SIGNEDINT, NULL},
	{p_isdigit, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InDecimalFirst, 0, NULL},
	{p_iseqC, 'e', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, 'E', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, SIGNEDINT, NULL}
};

static const TParserStateActionItem actionTPS_InSpace[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, SPACE, NULL},
	{p_iseqC, '<', A_BINGO, TPS_Base, SPACE, NULL},
	{p_isignore, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_BINGO, TPS_Base, SPACE, NULL},
	{p_iseqC, '+', A_BINGO, TPS_Base, SPACE, NULL},
	{p_iseqC, '&', A_BINGO, TPS_Base, SPACE, NULL},
	{p_iseqC, '/', A_BINGO, TPS_Base, SPACE, NULL},
	{p_isnotalnum, 0, A_NEXT, TPS_InSpace, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, SPACE, NULL}
};

static const TParserStateActionItem actionTPS_InUDecimalFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InUDecimal, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InUDecimal[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, DECIMAL_T, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InUDecimal, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InVersionFirst, 0, NULL},
	{p_iseqC, 'e', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, 'E', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, DECIMAL_T, NULL}
};

static const TParserStateActionItem actionTPS_InDecimalFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InDecimal, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InDecimal[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, DECIMAL_T, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InDecimal, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InVerVersion, 0, NULL},
	{p_iseqC, 'e', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, 'E', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, DECIMAL_T, NULL}
};

static const TParserStateActionItem actionTPS_InVerVersion[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_RERUN, TPS_InSVerVersion, 0, SpecialVerVersion},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InSVerVersion[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_BINGO | A_CLRALL, TPS_InUnsignedInt, SPACE, NULL},
	{NULL, 0, A_NEXT, TPS_Null, 0, NULL}
};


static const TParserStateActionItem actionTPS_InVersionFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InVersion, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InVersion[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, VERSIONNUMBER, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InVersion, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InVersionFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, VERSIONNUMBER, NULL}
};

static const TParserStateActionItem actionTPS_InMantissaFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InMantissa, 0, NULL},
	{p_iseqC, '+', A_NEXT, TPS_InMantissaSign, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InMantissaSign, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InMantissaSign[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InMantissa, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InMantissa[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, SCIENTIFIC, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InMantissa, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, SCIENTIFIC, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntityFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '#', A_NEXT, TPS_InXMLEntityNumFirst, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, ':', A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InXMLEntity, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntity[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isalnum, 0, A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, ':', A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, ';', A_NEXT, TPS_InXMLEntityEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntityNumFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, 'x', A_NEXT, TPS_InXMLEntityHexNumFirst, 0, NULL},
	{p_iseqC, 'X', A_NEXT, TPS_InXMLEntityHexNumFirst, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InXMLEntityNum, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntityHexNumFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isxdigit, 0, A_NEXT, TPS_InXMLEntityHexNum, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntityNum[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InXMLEntityNum, 0, NULL},
	{p_iseqC, ';', A_NEXT, TPS_InXMLEntityEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntityHexNum[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isxdigit, 0, A_NEXT, TPS_InXMLEntityHexNum, 0, NULL},
	{p_iseqC, ';', A_NEXT, TPS_InXMLEntityEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntityEnd[] = {
	{NULL, 0, A_BINGO | A_CLEAR, TPS_Base, XMLENTITY, NULL}
};

static const TParserStateActionItem actionTPS_InTagFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InTagCloseFirst, 0, NULL},
	{p_iseqC, '!', A_PUSH, TPS_InCommentFirst, 0, NULL},
	{p_iseqC, '?', A_PUSH, TPS_InXMLBegin, 0, NULL},
	{p_isasclet, 0, A_PUSH, TPS_InTagName, 0, NULL},
	{p_iseqC, ':', A_PUSH, TPS_InTagName, 0, NULL},
	{p_iseqC, '_', A_PUSH, TPS_InTagName, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLBegin[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	/* <?xml ... */
	/* XXX 要不要为 m、l 设状态?现接受 <?xZ */
	{p_iseqC, 'x', A_NEXT, TPS_InTag, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTagCloseFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InTagName, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTagName[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	/* <br/> case */
	{p_iseqC, '/', A_NEXT, TPS_InTagBeginEnd, 0, NULL},
	{p_iseqC, '>', A_NEXT, TPS_InTagEnd, 0, SpecialTags},
	{p_isspace, 0, A_NEXT, TPS_InTag, 0, SpecialTags},
	{p_isalnum, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, ':', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_Null, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTagBeginEnd[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '>', A_NEXT, TPS_InTagEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTag[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '>', A_NEXT, TPS_InTagEnd, 0, SpecialTags},
	{p_iseqC, '\'', A_NEXT, TPS_InTagEscapeK, 0, NULL},
	{p_iseqC, '"', A_NEXT, TPS_InTagEscapeKK, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '=', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '#', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, ':', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '&', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '?', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '%', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '~', A_NEXT, TPS_Null, 0, NULL},
	{p_isspace, 0, A_NEXT, TPS_Null, 0, SpecialTags},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTagEscapeK[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '\\', A_PUSH, TPS_InTagBackSleshed, 0, NULL},
	{p_iseqC, '\'', A_NEXT, TPS_InTag, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InTagEscapeK, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTagEscapeKK[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '\\', A_PUSH, TPS_InTagBackSleshed, 0, NULL},
	{p_iseqC, '"', A_NEXT, TPS_InTag, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InTagEscapeKK, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTagBackSleshed[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{NULL, 0, A_MERGE, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTagEnd[] = {
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, TAG_T, NULL}
};

static const TParserStateActionItem actionTPS_InCommentFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InCommentLast, 0, NULL},
	/* <!DOCTYPE ...> */
	{p_iseqC, 'D', A_NEXT, TPS_InTag, 0, NULL},
	{p_iseqC, 'd', A_NEXT, TPS_InTag, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InCommentLast[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InComment, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InComment[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InCloseCommentFirst, 0, NULL},
	{NULL, 0, A_NEXT, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InCloseCommentFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InCloseCommentLast, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InComment, 0, NULL}
};

static const TParserStateActionItem actionTPS_InCloseCommentLast[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '>', A_NEXT, TPS_InCommentEnd, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InComment, 0, NULL}
};

static const TParserStateActionItem actionTPS_InCommentEnd[] = {
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, TAG_T, NULL}
};

static const TParserStateActionItem actionTPS_InHostFirstDomain[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHostDomainSecond, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHost, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHostDomainSecond[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHostDomain, 0, NULL},
	{p_isdigit, 0, A_PUSH, TPS_InHost, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '_', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHostDomain[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_Base, HOST, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHostDomain, 0, NULL},
	{p_isdigit, 0, A_PUSH, TPS_InHost, 0, NULL},
	{p_iseqC, ':', A_PUSH, TPS_InPortFirst, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '_', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_isdigit, 0, A_POP, TPS_Null, 0, NULL},
	{p_isstophost, 0, A_BINGO | A_CLRALL, TPS_InURLPathStart, HOST, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFURL, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, HOST, NULL}
};

static const TParserStateActionItem actionTPS_InPortFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InPort, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InPort[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_Base, HOST, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InPort, 0, NULL},
	{p_isstophost, 0, A_BINGO | A_CLRALL, TPS_InURLPathStart, HOST, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFURL, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, HOST, NULL}
};

static const TParserStateActionItem actionTPS_InHostFirstAN[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHost, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHost, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHost[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHost, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHost, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '_', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InEmail[] = {
	{p_isstophost, 0, A_POP, TPS_Null, 0, NULL},
	{p_ishost, 0, A_BINGO | A_CLRALL, TPS_Base, EMAIL, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InFileFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_InPathFirst, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '~', A_PUSH, TPS_InFileTwiddle, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InFileTwiddle[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InPathFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_InPathSecond, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InPathFirstFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_InPathSecond, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InPathSecond[] = {
	{p_isEOF, 0, A_BINGO | A_CLEAR, TPS_Base, FILEPATH, NULL},
	{p_iseqC, '/', A_NEXT | A_PUSH, TPS_InFileFirst, 0, NULL},
	{p_iseqC, '/', A_BINGO | A_CLEAR, TPS_Base, FILEPATH, NULL},
	{p_isspace, 0, A_BINGO | A_CLEAR, TPS_Base, FILEPATH, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InFile[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, FILEPATH, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InFileNext, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, FILEPATH, NULL}
};

static const TParserStateActionItem actionTPS_InFileNext[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_CLEAR, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InFile, 0, NULL},
	{p_iseqC, '_', A_CLEAR, TPS_InFile, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InURLPathFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isurlchar, 0, A_NEXT, TPS_InURLPath, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL},
};

static const TParserStateActionItem actionTPS_InURLPathStart[] = {
	{NULL, 0, A_NEXT, TPS_InURLPath, 0, NULL}
};

static const TParserStateActionItem actionTPS_InURLPath[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, URLPATH, NULL},
	{p_isurlchar, 0, A_NEXT, TPS_InURLPath, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, URLPATH, NULL}
};

static const TParserStateActionItem actionTPS_InFURL[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isURLPath, 0, A_BINGO | A_CLRALL, TPS_Base, URL_T, SpecialFURL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InProtocolFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InProtocolSecond, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InProtocolSecond[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InProtocolEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InProtocolEnd[] = {
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, PROTOCOL, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenAsciiWordFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHyphenAsciiWord, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenDigitLookahead, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenAsciiWord[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, ASCIIHWORD, SpecialHyphen},
	{p_isasclet, 0, A_NEXT, TPS_InHyphenAsciiWord, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenAsciiWordFirst, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, ASCIIHWORD, SpecialHyphen}
};

static const TParserStateActionItem actionTPS_InHyphenWordFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenDigitLookahead, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenWord[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, HWORD, SpecialHyphen},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenWordFirst, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, HWORD, SpecialHyphen}
};

static const TParserStateActionItem actionTPS_InHyphenNumWordFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenDigitLookahead, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenNumWord[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, NUMHWORD, SpecialHyphen},
	{p_isalnum, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenNumWordFirst, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, NUMHWORD, SpecialHyphen}
};

static const TParserStateActionItem actionTPS_InHyphenDigitLookahead[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenDigitLookahead, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InParseHyphen[] = {
	{p_isEOF, 0, A_RERUN, TPS_Base, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHyphenAsciiWordPart, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWordPart, 0, NULL},
	{p_isdigit, 0, A_PUSH, TPS_InHyphenUnsignedInt, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InParseHyphenHyphen, 0, NULL},
	{NULL, 0, A_RERUN, TPS_Base, 0, NULL}
};

static const TParserStateActionItem actionTPS_InParseHyphenHyphen[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isalnum, 0, A_BINGO | A_CLEAR, TPS_InParseHyphen, SPACE, NULL},
	{p_isspecial, 0, A_BINGO | A_CLEAR, TPS_InParseHyphen, SPACE, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenWordPart[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, PARTHWORD, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWordPart, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenWordPart, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenNumWordPart, 0, NULL},
	{NULL, 0, A_BINGO, TPS_InParseHyphen, PARTHWORD, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenAsciiWordPart[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, ASCIIPARTHWORD, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHyphenAsciiWordPart, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWordPart, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenWordPart, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenNumWordPart, 0, NULL},
	{NULL, 0, A_BINGO, TPS_InParseHyphen, ASCIIPARTHWORD, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenNumWordPart[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, NUMPARTHWORD, NULL},
	{p_isalnum, 0, A_NEXT, TPS_InHyphenNumWordPart, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenNumWordPart, 0, NULL},
	{NULL, 0, A_BINGO, TPS_InParseHyphen, NUMPARTHWORD, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenUnsignedInt[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_isalpha, 0, A_CLEAR, TPS_InHyphenNumWordPart, 0, NULL},
	{p_isspecial, 0, A_CLEAR, TPS_InHyphenNumWordPart, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};


/*
 * 每状态解析器动作主表
 */
typedef struct
{
	const TParserStateActionItem *action;	/* 实际状态信息 */
	TParserState state;			/* 仅供 Assert 交叉校验 */
#ifdef WPARSER_TRACE
	const char *state_name;		/* 仅供调试打印 */
#endif
} TParserStateAction;

#ifdef WPARSER_TRACE
#define TPARSERSTATEACTION(state) \
	{ CppConcat(action,state), state, CppAsString(state) }
#else
#define TPARSERSTATEACTION(state) \
	{ CppConcat(action,state), state }
#endif

/*
 * 次序须与 TParserState 枚举定义相同!!
 */

static const TParserStateAction Actions[] = {
	TPARSERSTATEACTION(TPS_Base),
	TPARSERSTATEACTION(TPS_InNumWord),
	TPARSERSTATEACTION(TPS_InAsciiWord),
	TPARSERSTATEACTION(TPS_InWord),
	TPARSERSTATEACTION(TPS_InUnsignedInt),
	TPARSERSTATEACTION(TPS_InSignedIntFirst),
	TPARSERSTATEACTION(TPS_InSignedInt),
	TPARSERSTATEACTION(TPS_InSpace),
	TPARSERSTATEACTION(TPS_InUDecimalFirst),
	TPARSERSTATEACTION(TPS_InUDecimal),
	TPARSERSTATEACTION(TPS_InDecimalFirst),
	TPARSERSTATEACTION(TPS_InDecimal),
	TPARSERSTATEACTION(TPS_InVerVersion),
	TPARSERSTATEACTION(TPS_InSVerVersion),
	TPARSERSTATEACTION(TPS_InVersionFirst),
	TPARSERSTATEACTION(TPS_InVersion),
	TPARSERSTATEACTION(TPS_InMantissaFirst),
	TPARSERSTATEACTION(TPS_InMantissaSign),
	TPARSERSTATEACTION(TPS_InMantissa),
	TPARSERSTATEACTION(TPS_InXMLEntityFirst),
	TPARSERSTATEACTION(TPS_InXMLEntity),
	TPARSERSTATEACTION(TPS_InXMLEntityNumFirst),
	TPARSERSTATEACTION(TPS_InXMLEntityNum),
	TPARSERSTATEACTION(TPS_InXMLEntityHexNumFirst),
	TPARSERSTATEACTION(TPS_InXMLEntityHexNum),
	TPARSERSTATEACTION(TPS_InXMLEntityEnd),
	TPARSERSTATEACTION(TPS_InTagFirst),
	TPARSERSTATEACTION(TPS_InXMLBegin),
	TPARSERSTATEACTION(TPS_InTagCloseFirst),
	TPARSERSTATEACTION(TPS_InTagName),
	TPARSERSTATEACTION(TPS_InTagBeginEnd),
	TPARSERSTATEACTION(TPS_InTag),
	TPARSERSTATEACTION(TPS_InTagEscapeK),
	TPARSERSTATEACTION(TPS_InTagEscapeKK),
	TPARSERSTATEACTION(TPS_InTagBackSleshed),
	TPARSERSTATEACTION(TPS_InTagEnd),
	TPARSERSTATEACTION(TPS_InCommentFirst),
	TPARSERSTATEACTION(TPS_InCommentLast),
	TPARSERSTATEACTION(TPS_InComment),
	TPARSERSTATEACTION(TPS_InCloseCommentFirst),
	TPARSERSTATEACTION(TPS_InCloseCommentLast),
	TPARSERSTATEACTION(TPS_InCommentEnd),
	TPARSERSTATEACTION(TPS_InHostFirstDomain),
	TPARSERSTATEACTION(TPS_InHostDomainSecond),
	TPARSERSTATEACTION(TPS_InHostDomain),
	TPARSERSTATEACTION(TPS_InPortFirst),
	TPARSERSTATEACTION(TPS_InPort),
	TPARSERSTATEACTION(TPS_InHostFirstAN),
	TPARSERSTATEACTION(TPS_InHost),
	TPARSERSTATEACTION(TPS_InEmail),
	TPARSERSTATEACTION(TPS_InFileFirst),
	TPARSERSTATEACTION(TPS_InFileTwiddle),
	TPARSERSTATEACTION(TPS_InPathFirst),
	TPARSERSTATEACTION(TPS_InPathFirstFirst),
	TPARSERSTATEACTION(TPS_InPathSecond),
	TPARSERSTATEACTION(TPS_InFile),
	TPARSERSTATEACTION(TPS_InFileNext),
	TPARSERSTATEACTION(TPS_InURLPathFirst),
	TPARSERSTATEACTION(TPS_InURLPathStart),
	TPARSERSTATEACTION(TPS_InURLPath),
	TPARSERSTATEACTION(TPS_InFURL),
	TPARSERSTATEACTION(TPS_InProtocolFirst),
	TPARSERSTATEACTION(TPS_InProtocolSecond),
	TPARSERSTATEACTION(TPS_InProtocolEnd),
	TPARSERSTATEACTION(TPS_InHyphenAsciiWordFirst),
	TPARSERSTATEACTION(TPS_InHyphenAsciiWord),
	TPARSERSTATEACTION(TPS_InHyphenWordFirst),
	TPARSERSTATEACTION(TPS_InHyphenWord),
	TPARSERSTATEACTION(TPS_InHyphenNumWordFirst),
	TPARSERSTATEACTION(TPS_InHyphenNumWord),
	TPARSERSTATEACTION(TPS_InHyphenDigitLookahead),
	TPARSERSTATEACTION(TPS_InParseHyphen),
	TPARSERSTATEACTION(TPS_InParseHyphenHyphen),
	TPARSERSTATEACTION(TPS_InHyphenWordPart),
	TPARSERSTATEACTION(TPS_InHyphenAsciiWordPart),
	TPARSERSTATEACTION(TPS_InHyphenNumWordPart),
	TPARSERSTATEACTION(TPS_InHyphenUnsignedInt)
};


static bool
TParserGet(TParser *prs)
{
	const TParserStateActionItem *item = NULL;

	CHECK_FOR_INTERRUPTS();

	Assert(prs->state);

	if (prs->state->posbyte >= prs->lenstr)
		return false;

	prs->token = prs->str + prs->state->posbyte;
	prs->state->pushedAtAction = NULL;

	/* 查看串 */
	while (prs->state->posbyte <= prs->lenstr)
	{
		if (prs->state->posbyte == prs->lenstr)
			prs->state->charlen = 0;
		else
			prs->state->charlen = (prs->charmaxlen == 1) ? prs->charmaxlen :
				pg_mblen(prs->str + prs->state->posbyte);

		Assert(prs->state->posbyte + prs->state->charlen <= prs->lenstr);
		Assert(prs->state->state >= TPS_Base && prs->state->state < TPS_Null);
		Assert(Actions[prs->state->state].state == prs->state->state);

		if (prs->state->pushedAtAction)
		{
			/* POP 之后从下一测试继续 */
			item = prs->state->pushedAtAction + 1;
			prs->state->pushedAtAction = NULL;
		}
		else
		{
			item = Actions[prs->state->state].action;
			Assert(item != NULL);
		}

		/* 按字符类查找动作 */
		while (item->isclass)
		{
			prs->c = item->c;
			if (item->isclass(prs) != 0)
				break;
			item++;
		}

#ifdef WPARSER_TRACE
		{
			TParserPosition *ptr;

			fprintf(stderr, "state ");
			/* 按栈深缩进 */
			for (ptr = prs->state->prev; ptr; ptr = ptr->prev)
				fprintf(stderr, "  ");
			fprintf(stderr, "%s ", Actions[prs->state->state].state_name);
			if (prs->state->posbyte < prs->lenstr)
				fprintf(stderr, "at %c", *(prs->str + prs->state->posbyte));
			else
				fprintf(stderr, "at EOF");
			fprintf(stderr, " matched rule %d flags%s%s%s%s%s%s%s%s%s%s%s\n",
					(int) (item - Actions[prs->state->state].action),
					(item->flags & A_BINGO) ? " BINGO" : "",
					(item->flags & A_POP) ? " POP" : "",
					(item->flags & A_PUSH) ? " PUSH" : "",
					(item->flags & A_RERUN) ? " RERUN" : "",
					(item->flags & A_CLEAR) ? " CLEAR" : "",
					(item->flags & A_MERGE) ? " MERGE" : "",
					(item->flags & A_CLRALL) ? " CLRALL" : "",
					(item->tostate != TPS_Null) ? " tostate " : "",
					(item->tostate != TPS_Null) ? Actions[item->tostate].state_name : "",
					(item->type > 0) ? " type " : "",
					tok_alias[item->type]);
		}
#endif

		/* 若有特殊处理则调用 */
		if (item->special)
			item->special(prs);

		/* BINGO,找到词元 */
		if (item->flags & A_BINGO)
		{
			Assert(item->type > 0);
			prs->lenbytetoken = prs->state->lenbytetoken;
			prs->lenchartoken = prs->state->lenchartoken;
			prs->state->lenbytetoken = prs->state->lenchartoken = 0;
			prs->type = item->type;
		}

		/* 按标志做各种动作 */
		if (item->flags & A_POP)
		{						/* 弹出栈内保存状态 */
			TParserPosition *ptr = prs->state->prev;

			pfree(prs->state);
			prs->state = ptr;
			Assert(prs->state);
		}
		else if (item->flags & A_PUSH)
		{						/* 压入(保存)状态到栈 */
			prs->state->pushedAtAction = item;	/* 记住压入位置 */
			prs->state = newTParserPosition(prs->state);
		}
		else if (item->flags & A_CLEAR)
		{						/* 清除先前压入的状态 */
			TParserPosition *ptr;

			Assert(prs->state->prev);
			ptr = prs->state->prev->prev;
			pfree(prs->state->prev);
			prs->state->prev = ptr;
		}
		else if (item->flags & A_CLRALL)
		{						/* 清除全部先前压入状态 */
			TParserPosition *ptr;

			while (prs->state->prev)
			{
				ptr = prs->state->prev->prev;
				pfree(prs->state->prev);
				prs->state->prev = ptr;
			}
		}
		else if (item->flags & A_MERGE)
		{						/* 合并当前与压入状态位置信息 */
			TParserPosition *ptr = prs->state;

			Assert(prs->state->prev);
			prs->state = prs->state->prev;

			prs->state->posbyte = ptr->posbyte;
			prs->state->poschar = ptr->poschar;
			prs->state->charlen = ptr->charlen;
			prs->state->lenbytetoken = ptr->lenbytetoken;
			prs->state->lenchartoken = ptr->lenchartoken;
			pfree(ptr);
		}

		/* 有指向则设新状态 */
		if (item->tostate != TPS_Null)
			prs->state->state = item->tostate;

		/* 查是否离开 */
		if ((item->flags & A_BINGO) ||
			(prs->state->posbyte >= prs->lenstr &&
			 (item->flags & A_RERUN) == 0))
			break;

		/* 应重跑或刚恢复状态则回到循环开头 */
		if (item->flags & (A_RERUN | A_POP))
			continue;

		/* 向前推进 */
		if (prs->state->charlen)
		{
			prs->state->posbyte += prs->state->charlen;
			prs->state->lenbytetoken += prs->state->charlen;
			prs->state->poschar++;
			prs->state->lenchartoken++;
		}
	}

	return (item && (item->flags & A_BINGO));
}

Datum
prsd_lextype(PG_FUNCTION_ARGS)
{
	LexDescr   *descr = (LexDescr *) palloc(sizeof(LexDescr) * (LASTNUM + 1));
	int			i;

	for (i = 1; i <= LASTNUM; i++)
	{
		descr[i - 1].lexid = i;
		descr[i - 1].alias = pstrdup(tok_alias[i]);
		descr[i - 1].descr = pstrdup(lex_descr[i]);
	}

	descr[LASTNUM].lexid = 0;

	PG_RETURN_POINTER(descr);
}

Datum
prsd_start(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(TParserInit((char *) PG_GETARG_POINTER(0), PG_GETARG_INT32(1)));
}

Datum
prsd_nexttoken(PG_FUNCTION_ARGS)
{
	TParser    *p = (TParser *) PG_GETARG_POINTER(0);
	char	  **t = (char **) PG_GETARG_POINTER(1);
	int		   *tlen = (int *) PG_GETARG_POINTER(2);

	if (!TParserGet(p))
		PG_RETURN_INT32(0);

	*t = p->token;
	*tlen = p->lenbytetoken;

	PG_RETURN_INT32(p->type);
}

Datum
prsd_end(PG_FUNCTION_ARGS)
{
	TParser    *p = (TParser *) PG_GETARG_POINTER(0);

	TParserClose(p);
	PG_RETURN_VOID();
}


/*
 * 以下开始 ts_headline 支持
 */

/* 词元类型分类宏 */
#define TS_IDIGNORE(x)	( (x)==TAG_T || (x)==PROTOCOL || (x)==SPACE || (x)==XMLENTITY )
#define HLIDREPLACE(x)	( (x)==TAG_T )
#define HLIDSKIP(x)		( (x)==URL_T || (x)==NUMHWORD || (x)==ASCIIHWORD || (x)==HWORD )
#define XMLHLIDSKIP(x)	( (x)==URL_T || (x)==NUMHWORD || (x)==ASCIIHWORD || (x)==HWORD )
#define NONWORDTOKEN(x) ( (x)==SPACE || HLIDREPLACE(x) || HLIDSKIP(x) )
#define NOENDTOKEN(x)	( NONWORDTOKEN(x) || (x)==SCIENTIFIC || (x)==VERSIONNUMBER || (x)==DECIMAL_T || (x)==SIGNEDINT || (x)==UNSIGNEDINT || TS_IDIGNORE(x) )

/*
 * Macros useful in headline selection.  These rely on availability of
 * "HeadlineParsedText *prs" describing some text, and "int shortword"
 * describing the "short word" length parameter.
 */

/* 有趣词是非重复的检索词 */
#define INTERESTINGWORD(j) \
	(prs->words[j].item && !prs->words[j].repeated)

/* 不要停在非词或短词上,除非该词有趣 */
#define BADENDPOINT(j) \
	((NOENDTOKEN(prs->words[j].type) || prs->words[j].len <= shortword) && \
	 !INTERESTINGWORD(j))

typedef struct
{
	/* 一个覆盖(实为片段),供 mark_hl_fragments */
	int32		startpos;		/* 片段起始词下标 */
	int32		endpos;			/* 结束词下标(含) */
	int32		poslen;			/* 有趣词个数 */
	int32		curlen;			/* 词总数 */
	bool		chosen;			/* 已选? */
	bool		excluded;		/* 排除? */
} CoverPos;

typedef struct
{
	/* checkcondition_HL 的回调数据 */
	HeadlineWordEntry *words;
	int			len;
} hlCheck;


/*
 * TS_execute callback for matching a tsquery operand to headline words
 *
 * Note: it's tempting to report words[] indexes as pos values to save
 * searching in hlCover; but that would screw up phrase matching, which
 * expects to measure distances in lexemes not tokens.
 */
static TSTernaryValue
checkcondition_HL(void *opaque, QueryOperand *val, ExecPhraseData *data)
{
	hlCheck    *checkval = (hlCheck *) opaque;
	int			i;

	/* 扫描 words 数组找匹配项 */
	for (i = 0; i < checkval->len; i++)
	{
		if (checkval->words[i].item == val)
		{
			/* data == NULL 则无需报告位置 */
			if (!data)
				return TS_YES;

			if (!data->pos)
			{
				data->pos = palloc(sizeof(WordEntryPos) * checkval->len);
				data->allocated = true;
				data->npos = 1;
				data->pos[0] = checkval->words[i].pos;
			}
			else if (data->pos[data->npos - 1] < checkval->words[i].pos)
			{
				data->pos[data->npos++] = checkval->words[i].pos;
			}
		}
	}

	if (data && data->npos > 0)
		return TS_YES;

	return TS_NO;
}

/*
 * hlCover:在 prs 词表中找满足查询的子串
 *
 * locations 是查询的 TS_execute_locations() 结果。
 * 用来识别查询的可行子范围。
 *
 * *nextpos 是开始搜索的词元位置(非词下标)。
 * 调用方应初始化为 0。成功则推进到下一
 * 搜索起点。
 *
 * 成功时置 *p 与 *q 为覆盖子串首/末词下标,
 * 并返回真。
 *
 * 结果是最小覆盖:*p 与 *q 都会是
 * 查询所用之词。
 */
static bool
hlCover(HeadlineParsedText *prs, TSQuery query, List *locations,
		int *nextpos, int *p, int *q)
{
	int			pos = *nextpos;

	/* 所选词范围未满足查询时本循环会重来 */
	for (;;)
	{
		int			posb,
					pose;
		ListCell   *lc;

		/*
		 * 对每个 AND 查询词或短语,找其在 pos 及之后
		 * 的首次出现;pose 取这些位置的最大。
		 *
		 * 此处不必考虑 OR/NOT;见
		 * TS_execute_locations() 注释。下文再用
		 * TS_execute() 复核,处理随之不精确。
		 */
		pose = -1;
		foreach(lc, locations)
		{
			ExecPhraseData *pdata = (ExecPhraseData *) lfirst(lc);
			int			first = -1;

			for (int i = 0; i < pdata->npos; i++)
			{
				/* 短语匹配用末词元 */
				int			endp = pdata->pos[i];

				if (endp >= pos)
				{
					first = endp;
					break;
				}
			}
			if (first < 0)
				return false;	/* 本词再无匹配 */
			if (first > pose)
				pose = first;
		}

		if (pose < 0)
			return false;		/* 仅空列表才到此 */

		/*
		 * 再对每个 AND 查询词或短语,找其在 pose 及
		 * 之前的末次出现;posb 取这些位置的最小。
		 *
		 * posb 初值 INT_MAX-1,保证下面算
		 * posb+1 不溢
		 */
		posb = INT_MAX - 1;
		foreach(lc, locations)
		{
			ExecPhraseData *pdata = (ExecPhraseData *) lfirst(lc);
			int			last = -1;

			for (int i = pdata->npos - 1; i >= 0; i--)
			{
				/* 短语匹配用首词元 */
				int			startp = pdata->pos[i] - pdata->width;

				if (startp <= pose)
				{
					last = startp;
					break;
				}
			}
			if (last < posb)
				posb = last;
		}

		/*
		 * 若某短语匹配跨过 pos,可能得到 posb 在 pos
		 * 左侧。仍从 pos 试匹配,因短语与普通匹配
		 * OR 时 TS_execute_locations 结果不精确;即
		 * 若查询为 "(A <-> B) | C",则 C 可在 pos
		 * 匹配,即使短语匹配须伸到
		 * pos 左侧。
		 */
		posb = Max(posb, pos);

		/* 此测试多半总成功,但仍要防万一 */
		if (posb <= pose)
		{
			/*
			 * posb..pose 现为 pos 之后最早且最短、含全部
			 * 查询词的词元位置范围。除上文边角外
			 * 也含全部短语匹配。
			 *
			 *
			 * 现把这些词元位置转为 prs->words[] 下标。
			 */
			int			idxb = -1;
			int			idxe = -1;

			for (int i = 0; i < prs->curwords; i++)
			{
				if (prs->words[i].item == NULL)
					continue;
				if (idxb < 0 && prs->words[i].pos >= posb)
					idxb = i;
				if (prs->words[i].pos <= pose)
					idxe = i;
				else
					break;
			}

			/* 此测试多半总成功,但仍要防万一 */
			if (idxb >= 0 && idxe >= idxb)
			{
				/*
				 * 最后检查所选范围是否满足查询。简单
				 * 情形应成功;但非顶层 NOT 或短语与他物
				 * OR 等怪例可能失败,因
				 * TS_execute_locations 结果不能完整表示
				 * 这类情况。
				 */
				hlCheck		ch;

				ch.words = &(prs->words[idxb]);
				ch.len = idxe - idxb + 1;
				if (TS_execute(GETQUERY(query), &ch,
							   TS_EXEC_EMPTY, checkcondition_HL))
				{
					/* 匹配!推进 *nextpos 并返回词范围。 */
					*nextpos = posb + 1;
					*p = idxb;
					*q = idxe;
					return true;
				}
			}
		}

		/*
		 * 推进 pos 重试。之后可行匹配必始于
		 * posb 外。
		 */
		pos = posb + 1;
	}
	/* 不可达,但傻编译器缺了会抱怨 */
	return false;
}

/*
 * 对标题选择器选出的词施加合适高亮标记
 *
 * 自 startpos 至 endpos(含)按 highlightall 标记
 */
static void
mark_fragment(HeadlineParsedText *prs, bool highlightall,
			  int startpos, int endpos)
{
	int			i;

	for (i = startpos; i <= endpos; i++)
	{
		if (prs->words[i].item)
			prs->words[i].selected = 1;
		if (!highlightall)
		{
			if (HLIDREPLACE(prs->words[i].type))
				prs->words[i].replace = 1;
			else if (HLIDSKIP(prs->words[i].type))
				prs->words[i].skip = 1;
		}
		else
		{
			if (XMLHLIDSKIP(prs->words[i].type))
				prs->words[i].skip = 1;
		}

		prs->words[i].in = (prs->words[i].repeated) ? 0 : 1;
	}
}

/*
 * 把覆盖子串拆成不长于 max_words 的片段
 *
 * 入口时 *startpos 与 *endpos 是覆盖子串(剩余)
 * 边界。更新为下一片段的边界。
 *
 * *curlen 与 *poslen 分别设为片段词数与
 * 有趣词数。
 */
static void
get_next_fragment(HeadlineParsedText *prs, int *startpos, int *endpos,
				  int *curlen, int *poslen, int max_words)
{
	int			i;

	/*
	 * 目标:在 startpos 与 endpos 间选词片段,使
	 * 其至多 max_words 且两端皆为查询词。若
	 * startpos/endpos 是覆盖端点且词数少于
	 * max_words,则本函数应直接返回该覆盖
	 *
	 */
	/* 先把 startpos 移到一项 */
	for (i = *startpos; i <= *endpos; i++)
	{
		*startpos = i;
		if (INTERESTINGWORD(i))
			break;
	}
	/* 裁 endpos 使仅含 max_words */
	*curlen = 0;
	*poslen = 0;
	for (i = *startpos; i <= *endpos && *curlen < max_words; i++)
	{
		if (!NONWORDTOKEN(prs->words[i].type))
			*curlen += 1;
		if (INTERESTINGWORD(i))
			*poslen += 1;
	}
	/* 若覆盖被裁则把 endpos 移回查询项 */
	if (*endpos > i)
	{
		*endpos = i;
		for (i = *endpos; i >= *startpos; i--)
		{
			*endpos = i;
			if (INTERESTINGWORD(i))
				break;
			if (!NONWORDTOKEN(prs->words[i].type))
				*curlen -= 1;
		}
	}
}

/*
 * MaxFragments > 0 时用的标题选择器
 *
 * 注意:此模式下短语选取忽略 highlightall;
 * 它只控制呈现细节。
 */
static void
mark_hl_fragments(HeadlineParsedText *prs, TSQuery query, List *locations,
				  bool highlightall,
				  int shortword, int min_words,
				  int max_words, int max_fragments)
{
	int32		poslen,
				curlen,
				i,
				f,
				num_f = 0;
	int32		stretch,
				maxstretch,
				posmarker;

	int32		startpos = 0,
				endpos = 0,
				nextpos = 0,
				p = 0,
				q = 0;

	int32		numcovers = 0,
				maxcovers = 32;

	int32		minI,
				minwords,
				maxitems;
	CoverPos   *covers;

	covers = palloc(maxcovers * sizeof(CoverPos));

	/* 取各覆盖 */
	while (hlCover(prs, query, locations, &nextpos, &p, &q))
	{
		startpos = p;
		endpos = q;

		/*
		 * 把覆盖拆成更小片段,使每段至多
		 * max_words。并确保每段两端皆为
		 * 查询词。这样可向两侧拉伸
		 * 片段
		 */

		while (startpos <= endpos)
		{
			get_next_fragment(prs, &startpos, &endpos, &curlen, &poslen, max_words);
			if (numcovers >= maxcovers)
			{
				maxcovers *= 2;
				covers = repalloc(covers, sizeof(CoverPos) * maxcovers);
			}
			covers[numcovers].startpos = startpos;
			covers[numcovers].endpos = endpos;
			covers[numcovers].curlen = curlen;
			covers[numcovers].poslen = poslen;
			covers[numcovers].chosen = false;
			covers[numcovers].excluded = false;
			numcovers++;
			startpos = endpos + 1;
			endpos = q;
		}
	}

	/* 选最佳覆盖 */
	for (f = 0; f < max_fragments; f++)
	{
		maxitems = 0;
		minwords = PG_INT32_MAX;
		minI = -1;

		/*
		 * 选含项最多的覆盖。并列则选
		 * 词数较少者。
		 */
		for (i = 0; i < numcovers; i++)
		{
			if (!covers[i].chosen && !covers[i].excluded &&
				(maxitems < covers[i].poslen ||
				 (maxitems == covers[i].poslen &&
				  minwords > covers[i].curlen)))
			{
				maxitems = covers[i].poslen;
				minwords = covers[i].curlen;
				minI = i;
			}
		}
		/* 若找到覆盖则标记 */
		if (minI >= 0)
		{
			covers[minI].chosen = true;
			/* 调整覆盖大小 */
			startpos = covers[minI].startpos;
			endpos = covers[minI].endpos;
			curlen = covers[minI].curlen;
			/* 覆盖小于 max_words 则拉伸 */
			if (curlen < max_words)
			{
				/* 把拉伸分到覆盖两侧 */
				maxstretch = (max_words - curlen) / 2;

				/*
				 * 先拉伸 startpos,遇以下则停:1.到文档
				 * 开头 2.超过 maxstretch 3.碰到已
				 * 标记片段
				 */
				stretch = 0;
				posmarker = startpos;
				for (i = startpos - 1; i >= 0 && stretch < maxstretch && !prs->words[i].in; i--)
				{
					if (!NONWORDTOKEN(prs->words[i].type))
					{
						curlen++;
						stretch++;
					}
					posmarker = i;
				}
				/* 回缩 startpos 直到好的端点 */
				for (i = posmarker; i < startpos && BADENDPOINT(i); i++)
				{
					if (!NONWORDTOKEN(prs->words[i].type))
						curlen--;
				}
				startpos = i;
				/* 再尽量拉伸 endpos */
				posmarker = endpos;
				for (i = endpos + 1; i < prs->curwords && curlen < max_words && !prs->words[i].in; i++)
				{
					if (!NONWORDTOKEN(prs->words[i].type))
						curlen++;
					posmarker = i;
				}
				/* 回缩 endpos 直到好的端点 */
				for (i = posmarker; i > endpos && BADENDPOINT(i); i--)
				{
					if (!NONWORDTOKEN(prs->words[i].type))
						curlen--;
				}
				endpos = i;
			}
			covers[minI].startpos = startpos;
			covers[minI].endpos = endpos;
			covers[minI].curlen = curlen;
			/* 标记所选片段(覆盖) */
			mark_fragment(prs, highlightall, startpos, endpos);
			num_f++;
			/* 排除与本覆盖重叠者,后续不再考虑 */
			for (i = 0; i < numcovers; i++)
			{
				if (i != minI &&
					((covers[i].startpos >= startpos &&
					  covers[i].startpos <= endpos) ||
					 (covers[i].endpos >= startpos &&
					  covers[i].endpos <= endpos) ||
					 (covers[i].startpos < startpos &&
					  covers[i].endpos > endpos)))
					covers[i].excluded = true;
			}
		}
		else
			break;				/* 无可选覆盖则止 */
	}

	/* 若尚未标记任何内容则显示前 min_words 词 */
	if (num_f <= 0)
	{
		startpos = curlen = 0;
		endpos = -1;
		for (i = 0; i < prs->curwords && curlen < min_words; i++)
		{
			if (!NONWORDTOKEN(prs->words[i].type))
				curlen++;
			endpos = i;
		}
		mark_fragment(prs, highlightall, startpos, endpos);
	}

	pfree(covers);
}

/*
 * MaxFragments == 0 时用的标题选择器
 */
static void
mark_hl_words(HeadlineParsedText *prs, TSQuery query, List *locations,
			  bool highlightall,
			  int shortword, int min_words, int max_words)
{
	int			nextpos = 0,
				p = 0,
				q = 0;
	int			bestb = -1,
				beste = -1;
	int			bestlen = -1;
	bool		bestcover = false;
	int			pose,
				posb,
				poslen,
				curlen;
	bool		poscover;
	int			i;

	if (!highlightall)
	{
		/* 考察全部覆盖,用最佳者选标题 */
		while (hlCover(prs, query, locations, &nextpos, &p, &q))
		{
			/*
			 * 统计覆盖内词数(curlen)与有趣词
			 * (poslen),达 max_words 即停。本步不管
			 * 是否好停点。posb/pose 设为可能标题
			 * 的起止下标。
			 */
			curlen = 0;
			poslen = 0;
			posb = pose = p;
			for (i = p; i <= q && curlen < max_words; i++)
			{
				if (!NONWORDTOKEN(prs->words[i].type))
					curlen++;
				if (INTERESTINGWORD(i))
					poslen++;
				pose = i;
			}

			if (curlen < max_words)
			{
				/*
				 * 标题还有加长空间,则向前搜至满
				 * 或遇好停点。会重看 "q" 处
				 * 词再前进。
				 */
				for (i = i - 1; i < prs->curwords && curlen < max_words; i++)
				{
					if (i > q)
					{
						if (!NONWORDTOKEN(prs->words[i].type))
							curlen++;
						if (INTERESTINGWORD(i))
							poslen++;
					}
					pose = i;
					if (BADENDPOINT(i))
						continue;
					if (curlen >= min_words)
						break;
				}
				if (curlen < min_words)
				{
					/*
					 * 已到文末且标题仍短于 min_words,
					 * 故试向左扩展。
					 */
					for (i = p - 1; i >= 0; i--)
					{
						if (!NONWORDTOKEN(prs->words[i].type))
							curlen++;
						if (INTERESTINGWORD(i))
							poslen++;
						if (curlen >= max_words)
							break;
						if (BADENDPOINT(i))
							continue;
						if (curlen >= min_words)
							break;
					}
					posb = (i >= 0) ? i : 0;
				}
			}
			else
			{
				/*
				 * 无法再加长标题,故必要时考虑缩短
				 * 以避免坏端点。
				 */
				if (i > q)
					i = q;
				for (; curlen > min_words; i--)
				{
					if (!BADENDPOINT(i))
						break;
					if (!NONWORDTOKEN(prs->words[i].type))
						curlen--;
					if (INTERESTINGWORD(i))
						poslen--;
					pose = i - 1;
				}
			}

			/*
			 * 检查拟用标题是否包含原覆盖;若因
			 * max_words 裁过则可能不含。
			 */
			poscover = (posb <= p && pose >= q);

			/*
			 * 若本标题优于上一个则采纳:优先含覆盖
			 * 者,其次有趣词更多者,再次停点好者。
			 * (bestlen 初为 -1,故必采纳
			 * 首个标题。)
			 *
			 */
			if (poscover > bestcover ||
				(poscover == bestcover && poslen > bestlen) ||
				(poscover == bestcover && poslen == bestlen &&
				 !BADENDPOINT(pose) && BADENDPOINT(beste)))
			{
				bestb = posb;
				beste = pose;
				bestlen = poslen;
				bestcover = poscover;
			}
		}

		/*
		 * 若无一可接受,则自开头选取
		 * min_words 词
		 */
		if (bestlen < 0)
		{
			curlen = 0;
			pose = -1;
			for (i = 0; i < prs->curwords && curlen < min_words; i++)
			{
				if (!NONWORDTOKEN(prs->words[i].type))
					curlen++;
				pose = i;
			}
			bestb = 0;
			beste = pose;
		}
	}
	else
	{
		/* highlightall 模式:标题为全文 */
		bestb = 0;
		beste = prs->curwords - 1;
	}

	mark_fragment(prs, highlightall, bestb, beste);
}

/*
 * 默认解析器的 prsheadline 函数
 */
Datum
prsd_headline(PG_FUNCTION_ARGS)
{
	HeadlineParsedText *prs = (HeadlineParsedText *) PG_GETARG_POINTER(0);
	List	   *prsoptions = (List *) PG_GETARG_POINTER(1);
	TSQuery		query = PG_GETARG_TSQUERY(2);
	List	   *locations;

	/* 默认选项值: */
	int			min_words = 15;
	int			max_words = 35;
	int			shortword = 3;
	int			max_fragments = 0;
	bool		highlightall = false;
	ListCell   *l;

	/* 提取配置选项值 */
	prs->startsel = NULL;
	prs->stopsel = NULL;
	prs->fragdelim = NULL;
	foreach(l, prsoptions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);
		char	   *val = defGetString(defel);

		if (pg_strcasecmp(defel->defname, "MaxWords") == 0)
			max_words = pg_strtoint32(val);
		else if (pg_strcasecmp(defel->defname, "MinWords") == 0)
			min_words = pg_strtoint32(val);
		else if (pg_strcasecmp(defel->defname, "ShortWord") == 0)
			shortword = pg_strtoint32(val);
		else if (pg_strcasecmp(defel->defname, "MaxFragments") == 0)
			max_fragments = pg_strtoint32(val);
		else if (pg_strcasecmp(defel->defname, "StartSel") == 0)
			prs->startsel = pstrdup(val);
		else if (pg_strcasecmp(defel->defname, "StopSel") == 0)
			prs->stopsel = pstrdup(val);
		else if (pg_strcasecmp(defel->defname, "FragmentDelimiter") == 0)
			prs->fragdelim = pstrdup(val);
		else if (pg_strcasecmp(defel->defname, "HighlightAll") == 0)
			highlightall = (pg_strcasecmp(val, "1") == 0 ||
							pg_strcasecmp(val, "on") == 0 ||
							pg_strcasecmp(val, "true") == 0 ||
							pg_strcasecmp(val, "t") == 0 ||
							pg_strcasecmp(val, "y") == 0 ||
							pg_strcasecmp(val, "yes") == 0);
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized headline parameter: \"%s\"",
							defel->defname)));
	}

	/* HighlightAll 模式下忽略这些参数 */
	if (!highlightall)
	{
		if (min_words >= max_words)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("MinWords should be less than MaxWords")));
		if (min_words <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("MinWords should be positive")));
		if (shortword < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ShortWord should be >= 0")));
		if (max_fragments < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("MaxFragments should be >= 0")));
	}

	/* 定位匹配查询的词与短语 */
	if (query->size > 0)
	{
		hlCheck		ch;

		ch.words = prs->words;
		ch.len = prs->curwords;
		locations = TS_execute_locations(GETQUERY(query), &ch, TS_EXEC_EMPTY,
										 checkcondition_HL);
	}
	else
		locations = NIL;		/* 空查询匹配空 */

	/* 应用合适的标题选择器 */
	if (max_fragments == 0)
		mark_hl_words(prs, query, locations, highlightall, shortword,
					  min_words, max_words);
	else
		mark_hl_fragments(prs, query, locations, highlightall, shortword,
						  min_words, max_words, max_fragments);

	/* 为串选项填默认值 */
	if (!prs->startsel)
		prs->startsel = pstrdup("<b>");
	if (!prs->stopsel)
		prs->stopsel = pstrdup("</b>");
	if (!prs->fragdelim)
		prs->fragdelim = pstrdup(" ... ");

	/* 调用方也需要这些长度 */
	prs->startsellen = strlen(prs->startsel);
	prs->stopsellen = strlen(prs->stopsel);
	prs->fragdelimlen = strlen(prs->fragdelim);

	PG_RETURN_POINTER(prs);
}
