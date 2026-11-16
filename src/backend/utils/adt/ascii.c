/*-----------------------------------------------------------------------
 * ascii.c
 *	 字符串到 ASCII 转换的 PostgreSQL 例程。
 *
 *	 Portions Copyright (c) 1999-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/ascii.c
 *
 *-----------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "utils/ascii.h"
#include "utils/builtins.h"
#include "varatt.h"

static void pg_to_ascii(unsigned char *src, unsigned char *src_end,
						unsigned char *dest, int enc);
static text *encode_to_ascii(text *data, int enc);


/* ----------
 * to_ascii
 * ----------
 */
static void
pg_to_ascii(unsigned char *src, unsigned char *src_end, unsigned char *dest, int enc)
{
	unsigned char *x;
	const unsigned char *ascii;
	int			range;

	/*
	 * 与编码相关的起始点
	 */
#define RANGE_128	128
#define RANGE_160	160

	if (enc == PG_LATIN1)
	{
		/*
		 * ISO-8859-1 <范围:160 -- 255>
		 */
		ascii = (const unsigned char *) "  cL Y  \"Ca  -R     'u .,      ?AAAAAAACEEEEIIII NOOOOOxOUUUUYTBaaaaaaaceeeeiiii nooooo/ouuuuyty";
		range = RANGE_160;
	}
	else if (enc == PG_LATIN2)
	{
		/*
		 * ISO-8859-2 <范围:160 -- 255>
		 */
		ascii = (const unsigned char *) " A L LS \"SSTZ-ZZ a,l'ls ,sstz\"zzRAAAALCCCEEEEIIDDNNOOOOxRUUUUYTBraaaalccceeeeiiddnnoooo/ruuuuyt.";
		range = RANGE_160;
	}
	else if (enc == PG_LATIN9)
	{
		/*
		 * ISO-8859-15 <范围:160 -- 255>
		 */
		ascii = (const unsigned char *) "  cL YS sCa  -R     Zu .z   EeY?AAAAAAACEEEEIIII NOOOOOxOUUUUYTBaaaaaaaceeeeiiii nooooo/ouuuuyty";
		range = RANGE_160;
	}
	else if (enc == PG_WIN1250)
	{
		/*
		 * Window CP1250 <范围:128 -- 255>
		 */
		ascii = (const unsigned char *) "  ' \"    %S<STZZ `'\"\".--  s>stzz   L A  \"CS  -RZ  ,l'u .,as L\"lzRAAAALCCCEEEEIIDDNNOOOOxRUUUUYTBraaaalccceeeeiiddnnoooo/ruuuuyt ";
		range = RANGE_128;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("encoding conversion from %s to ASCII not supported",
						pg_encoding_to_char(enc))));
		return;					/* 安抚编译器 */
	}

	/*
	 * 编码
	 */
	for (x = src; x < src_end; x++)
	{
		if (*x < 128)
			*dest++ = *x;
		else if (*x < range)
			*dest++ = ' ';		/* 128 伪作范围 */
		else
			*dest++ = ascii[*x - range];
	}
}

/* ----------
 * 编码 text
 *
 * 文本 datum 会被就地覆写，因此该编码方法无法
 * 支持会改变字符串长度的转换！
 * ----------
 */
static text *
encode_to_ascii(text *data, int enc)
{
	pg_to_ascii((unsigned char *) VARDATA(data),	/* src */
				(unsigned char *) (data) + VARSIZE(data),	/* 源尾 */
				(unsigned char *) VARDATA(data),	/* dest */
				enc);			/* 编码 */

	return data;
}

/* ----------
 * 转换为 ASCII，enc 以 'name' 传入。
 * ----------
 */
Datum
to_ascii_encname(PG_FUNCTION_ARGS)
{
	text	   *data = PG_GETARG_TEXT_P_COPY(0);
	char	   *encname = NameStr(*PG_GETARG_NAME(1));
	int			enc = pg_char_to_encoding(encname);

	if (enc < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("%s is not a valid encoding name", encname)));

	PG_RETURN_TEXT_P(encode_to_ascii(data, enc));
}

/* ----------
 * 转换为 ASCII，enc 以 int4 给定
 * ----------
 */
Datum
to_ascii_enc(PG_FUNCTION_ARGS)
{
	text	   *data = PG_GETARG_TEXT_P_COPY(0);
	int			enc = PG_GETARG_INT32(1);

	if (!PG_VALID_ENCODING(enc))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("%d is not a valid encoding code", enc)));

	PG_RETURN_TEXT_P(encode_to_ascii(data, enc));
}

/* ----------
 * 转换为 ASCII，当前编码为 DatabaseEncoding
 * ----------
 */
Datum
to_ascii_default(PG_FUNCTION_ARGS)
{
	text	   *data = PG_GETARG_TEXT_P_COPY(0);
	int			enc = GetDatabaseEncoding();

	PG_RETURN_TEXT_P(encode_to_ascii(data, enc));
}

/* ----------
 * 复制任意后端安全编码中的字符串，将其转换为
 * 有效 ASCII 串，非 ASCII 字节以 '?' 替换。其余行为
 * 与 strlcpy() 完全相同，只是不费心提供
 * 返回值。
 *
 * 绝不能触发 ereport(ERROR)，因其在 postmaster 中调用。
 * ----------
 */
void
ascii_safe_strlcpy(char *dest, const char *src, size_t destsiz)
{
	if (destsiz == 0)			/* 边界情况：末尾 nul 无空间 */
		return;

	while (--destsiz > 0)
	{
		/* 用 unsigned char 避免编译器警告 */
		unsigned char ch = *src++;

		if (ch == '\0')
			break;
		/* 保留可打印 ASCII 字符 */
		if (32 <= ch && ch <= 127)
			*dest = ch;
		/* 空白字符也可以 */
		else if (ch == '\n' || ch == '\r' || ch == '\t')
			*dest = ch;
		/* 其余内容均替换为 '?' */
		else
			*dest = '?';
		dest++;
	}

	*dest = '\0';
}
