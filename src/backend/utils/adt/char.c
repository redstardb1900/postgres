/*-------------------------------------------------------------------------
 *
 * char.c
 *	  内置类型 "char" 的函数（不要与
 *	  SQL CHAR(n) 类型 bpchar 混淆）。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/char.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "varatt.h"

#define ISOCTAL(c)   (((c) >= '0') && ((c) <= '7'))
#define TOOCTAL(c)   ((c) + '0')
#define FROMOCTAL(c) ((unsigned char) (c) - '0')


/*****************************************************************************
 *	 用户 I/O 例程						 *
 *****************************************************************************/

/*
 *		charin			- 将 "x" 转为 'x'
 *
 * 它接受 charout 产生的格式。如果有多字节输入
 * 不是 '\ooo' 形式，则以第一个字节作为值，并
 * 静默丢弃其余部分；这是一项向后兼容措施。
 */
Datum
charin(PG_FUNCTION_ARGS)
{
	char	   *ch = PG_GETARG_CSTRING(0);

	if (strlen(ch) == 4 && ch[0] == '\\' &&
		ISOCTAL(ch[1]) && ISOCTAL(ch[2]) && ISOCTAL(ch[3]))
		PG_RETURN_CHAR((FROMOCTAL(ch[1]) << 6) +
					   (FROMOCTAL(ch[2]) << 3) +
					   FROMOCTAL(ch[3]));
	/* 对零长输入字符串也能得到正确结果 */
	PG_RETURN_CHAR(ch[0]);
}

/*
 *		charout			- 将 'x' 转为 "x"
 *
 * 可能的输出格式有：
 * 1. 0x00 表示为空字符串。
 * 2. 0x01..0x7F 表示为单个 ASCII 字节。
 * 3. 0x80..0xFF 表示为 \ooo（反斜杠和3位八进制数字）。
 * 情形 3 旨在匹配 bytea 传统的 "escape" 格式。
 */
Datum
charout(PG_FUNCTION_ARGS)
{
	char		ch = PG_GETARG_CHAR(0);
	char	   *result = (char *) palloc(5);

	if (IS_HIGHBIT_SET(ch))
	{
		result[0] = '\\';
		result[1] = TOOCTAL(((unsigned char) ch) >> 6);
		result[2] = TOOCTAL((((unsigned char) ch) >> 3) & 07);
		result[3] = TOOCTAL(((unsigned char) ch) & 07);
		result[4] = '\0';
	}
	else
	{
		/* 这对 0x00 也能产生可接受的结果 */
		result[0] = ch;
		result[1] = '\0';
	}
	PG_RETURN_CSTRING(result);
}

/*
 *		charrecv			- 将外部二进制格式转为 char
 *
 * 外部表示为一个字节，且无字符集转换。这在
 * 某种程度上值得商榷，但许多场合人们将
 * char 用作 1 字节二进制类型。
 */
Datum
charrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_CHAR(pq_getmsgbyte(buf));
}

/*
 *		charsend			- 将 char 转为二进制格式
 */
Datum
charsend(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*****************************************************************************
 *	 公共例程						 *
 *****************************************************************************/

/*
 * 注意：比较时将 char 视为无符号 (uint8) 进行。
 * 与整数之间的转换则将 char 视为有符号 (int8) 进行。
 *
 * 你想要一致性？
 */

Datum
chareq(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
charne(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
charlt(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL((uint8) arg1 < (uint8) arg2);
}

Datum
charle(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL((uint8) arg1 <= (uint8) arg2);
}

Datum
chargt(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL((uint8) arg1 > (uint8) arg2);
}

Datum
charge(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL((uint8) arg1 >= (uint8) arg2);
}


Datum
chartoi4(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);

	PG_RETURN_INT32((int32) ((int8) arg1));
}

Datum
i4tochar(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);

	if (arg1 < SCHAR_MIN || arg1 > SCHAR_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("\"char\" out of range")));

	PG_RETURN_CHAR((int8) arg1);
}


Datum
text_char(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	char	   *ch = VARDATA_ANY(arg1);
	char		result;

	/*
	 * 转换规则与 charin() 相同，但这里需要如实
	 * 处理空字符串情形。
	 */
	if (VARSIZE_ANY_EXHDR(arg1) == 4 && ch[0] == '\\' &&
		ISOCTAL(ch[1]) && ISOCTAL(ch[2]) && ISOCTAL(ch[3]))
		result = (FROMOCTAL(ch[1]) << 6) +
			(FROMOCTAL(ch[2]) << 3) +
			FROMOCTAL(ch[3]);
	else if (VARSIZE_ANY_EXHDR(arg1) > 0)
		result = ch[0];
	else
		result = '\0';

	PG_RETURN_CHAR(result);
}

Datum
char_text(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	text	   *result = palloc(VARHDRSZ + 4);

	/*
	 * 转换规则与 charout() 相同，但这里需要如
	 * 实处理 0x00 到空字符串的转换。
	 */
	if (IS_HIGHBIT_SET(arg1))
	{
		SET_VARSIZE(result, VARHDRSZ + 4);
		(VARDATA(result))[0] = '\\';
		(VARDATA(result))[1] = TOOCTAL(((unsigned char) arg1) >> 6);
		(VARDATA(result))[2] = TOOCTAL((((unsigned char) arg1) >> 3) & 07);
		(VARDATA(result))[3] = TOOCTAL(((unsigned char) arg1) & 07);
	}
	else if (arg1 != '\0')
	{
		SET_VARSIZE(result, VARHDRSZ + 1);
		*(VARDATA(result)) = arg1;
	}
	else
		SET_VARSIZE(result, VARHDRSZ);

	PG_RETURN_TEXT_P(result);
}
