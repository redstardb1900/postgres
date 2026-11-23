/*-------------------------------------------------------------------------
 *
 * encode.c
 *	  各类数据编码/解码。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/encode.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "varatt.h"


/*
 * 编码转换 API。
 * encode_len() 与 decode_len() 计算所需空间,而
 * encode() 与 decode() 执行实际转换。_len 函数可
 * 高估但不可低估。(虽如此,过大高估可能
 * 引发不必要错误,故最好估准。) 转换例程
 * 写入 *res 所指缓冲并返回其输出的
 * 真实长度。
 */
struct pg_encoding
{
	uint64		(*encode_len) (const char *data, size_t dlen);
	uint64		(*decode_len) (const char *data, size_t dlen);
	uint64		(*encode) (const char *data, size_t dlen, char *res);
	uint64		(*decode) (const char *data, size_t dlen, char *res);
};

static const struct pg_encoding *pg_find_encoding(const char *name);

/*
 * SQL 函数。
 */

Datum
binary_encode(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	Datum		name = PG_GETARG_DATUM(1);
	text	   *result;
	char	   *namebuf;
	char	   *dataptr;
	size_t		datalen;
	uint64		resultlen;
	uint64		res;
	const struct pg_encoding *enc;

	namebuf = TextDatumGetCString(name);

	enc = pg_find_encoding(namebuf);
	if (enc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized encoding: \"%s\"", namebuf)));

	dataptr = VARDATA_ANY(data);
	datalen = VARSIZE_ANY_EXHDR(data);

	resultlen = enc->encode_len(dataptr, datalen);

	/*
	 * resultlen 可能溢出 uint32,故在 32 位机上
	 * 不可依赖 palloc 的内部检查。
	 */
	if (resultlen > MaxAllocSize - VARHDRSZ)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("result of encoding conversion is too large")));

	result = palloc(VARHDRSZ + resultlen);

	res = enc->encode(dataptr, datalen, VARDATA(result));

	/* 用 FATAL,因已踩坏内存 ... */
	if (res > resultlen)
		elog(FATAL, "overflow - encode estimate too small");

	SET_VARSIZE(result, VARHDRSZ + res);

	PG_RETURN_TEXT_P(result);
}

Datum
binary_decode(PG_FUNCTION_ARGS)
{
	text	   *data = PG_GETARG_TEXT_PP(0);
	Datum		name = PG_GETARG_DATUM(1);
	bytea	   *result;
	char	   *namebuf;
	char	   *dataptr;
	size_t		datalen;
	uint64		resultlen;
	uint64		res;
	const struct pg_encoding *enc;

	namebuf = TextDatumGetCString(name);

	enc = pg_find_encoding(namebuf);
	if (enc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized encoding: \"%s\"", namebuf)));

	dataptr = VARDATA_ANY(data);
	datalen = VARSIZE_ANY_EXHDR(data);

	resultlen = enc->decode_len(dataptr, datalen);

	/*
	 * resultlen 可能溢出 uint32,故在 32 位机上
	 * 不可依赖 palloc 的内部检查。
	 */
	if (resultlen > MaxAllocSize - VARHDRSZ)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("result of decoding conversion is too large")));

	result = palloc(VARHDRSZ + resultlen);

	res = enc->decode(dataptr, datalen, VARDATA(result));

	/* 用 FATAL,因已踩坏内存 ... */
	if (res > resultlen)
		elog(FATAL, "overflow - decode estimate too small");

	SET_VARSIZE(result, VARHDRSZ + res);

	PG_RETURN_BYTEA_P(result);
}


/*
 * HEX
 */

static const char hextbl[] = "0123456789abcdef";

static const int8 hexlookup[128] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

uint64
hex_encode(const char *src, size_t len, char *dst)
{
	const char *end = src + len;

	while (src < end)
	{
		*dst++ = hextbl[(*src >> 4) & 0xF];
		*dst++ = hextbl[*src & 0xF];
		src++;
	}
	return (uint64) len * 2;
}

static inline bool
get_hex(const char *cp, char *out)
{
	unsigned char c = (unsigned char) *cp;
	int			res = -1;

	if (c < 127)
		res = hexlookup[c];

	*out = (char) res;

	return (res >= 0);
}

uint64
hex_decode(const char *src, size_t len, char *dst)
{
	return hex_decode_safe(src, len, dst, NULL);
}

uint64
hex_decode_safe(const char *src, size_t len, char *dst, Node *escontext)
{
	const char *s,
			   *srcend;
	char		v1,
				v2,
			   *p;

	srcend = src + len;
	s = src;
	p = dst;
	while (s < srcend)
	{
		if (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r')
		{
			s++;
			continue;
		}
		if (!get_hex(s, &v1))
			ereturn(escontext, 0,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid hexadecimal digit: \"%.*s\"",
							pg_mblen(s), s)));
		s++;
		if (s >= srcend)
			ereturn(escontext, 0,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid hexadecimal data: odd number of digits")));
		if (!get_hex(s, &v2))
			ereturn(escontext, 0,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid hexadecimal digit: \"%.*s\"",
							pg_mblen(s), s)));
		s++;
		*p++ = (v1 << 4) | v2;
	}

	return p - dst;
}

static uint64
hex_enc_len(const char *src, size_t srclen)
{
	return (uint64) srclen << 1;
}

static uint64
hex_dec_len(const char *src, size_t srclen)
{
	return (uint64) srclen >> 1;
}

/*
 * BASE64
 */

static const char _base64[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const int8 b64lookup[128] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
	52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
	-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
	15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
	-1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
	41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
};

static uint64
pg_base64_encode(const char *src, size_t len, char *dst)
{
	char	   *p,
			   *lend = dst + 76;
	const char *s,
			   *end = src + len;
	int			pos = 2;
	uint32		buf = 0;

	s = src;
	p = dst;

	while (s < end)
	{
		buf |= (unsigned char) *s << (pos << 3);
		pos--;
		s++;

		/* 写出 */
		if (pos < 0)
		{
			*p++ = _base64[(buf >> 18) & 0x3f];
			*p++ = _base64[(buf >> 12) & 0x3f];
			*p++ = _base64[(buf >> 6) & 0x3f];
			*p++ = _base64[buf & 0x3f];

			pos = 2;
			buf = 0;
		}
		if (p >= lend)
		{
			*p++ = '\n';
			lend = p + 76;
		}
	}
	if (pos != 2)
	{
		*p++ = _base64[(buf >> 18) & 0x3f];
		*p++ = _base64[(buf >> 12) & 0x3f];
		*p++ = (pos == 0) ? _base64[(buf >> 6) & 0x3f] : '=';
		*p++ = '=';
	}

	return p - dst;
}

static uint64
pg_base64_decode(const char *src, size_t len, char *dst)
{
	const char *srcend = src + len,
			   *s = src;
	char	   *p = dst;
	char		c;
	int			b = 0;
	uint32		buf = 0;
	int			pos = 0,
				end = 0;

	while (s < srcend)
	{
		c = *s++;

		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			continue;

		if (c == '=')
		{
			/* 结束序列 */
			if (!end)
			{
				if (pos == 2)
					end = 1;
				else if (pos == 3)
					end = 2;
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("unexpected \"=\" while decoding base64 sequence")));
			}
			b = 0;
		}
		else
		{
			b = -1;
			if (c > 0 && c < 127)
				b = b64lookup[(unsigned char) c];
			if (b < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid symbol \"%.*s\" found while decoding base64 sequence",
								pg_mblen(s - 1), s - 1)));
		}
		/* 加入缓冲 */
		buf = (buf << 6) + b;
		pos++;
		if (pos == 4)
		{
			*p++ = (buf >> 16) & 255;
			if (end == 0 || end > 1)
				*p++ = (buf >> 8) & 255;
			if (end == 0 || end > 2)
				*p++ = buf & 255;
			buf = 0;
			pos = 0;
		}
	}

	if (pos != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid base64 end sequence"),
				 errhint("Input data is missing padding, is truncated, or is otherwise corrupted.")));

	return p - dst;
}


static uint64
pg_base64_enc_len(const char *src, size_t srclen)
{
	/* 3 字节转 4,每 76 字符后换行 */
	return ((uint64) srclen + 2) / 3 * 4 + (uint64) srclen / (76 * 3 / 4);
}

static uint64
pg_base64_dec_len(const char *src, size_t srclen)
{
	return ((uint64) srclen * 3) >> 2;
}

/*
 * Escape
 * bytea 最小转义为 text。
 * text 反转义→bytea
 *
 * 须转义零字节与高位置位字节,以免生成
 * 在当前编码下可能无效的文本,或经编码转换
 * 后变成别的内容(导致无法反转义回原
 * bytea)。
 * 当然反斜杠本身也须转义。
 *
 * 反转义处理 \\ 与任何 \### 八进制
 */

#define VAL(CH)			((CH) - '0')
#define DIG(VAL)		((VAL) + '0')

static uint64
esc_encode(const char *src, size_t srclen, char *dst)
{
	const char *end = src + srclen;
	char	   *rp = dst;
	uint64		len = 0;

	while (src < end)
	{
		unsigned char c = (unsigned char) *src;

		if (c == '\0' || IS_HIGHBIT_SET(c))
		{
			rp[0] = '\\';
			rp[1] = DIG(c >> 6);
			rp[2] = DIG((c >> 3) & 7);
			rp[3] = DIG(c & 7);
			rp += 4;
			len += 4;
		}
		else if (c == '\\')
		{
			rp[0] = '\\';
			rp[1] = '\\';
			rp += 2;
			len += 2;
		}
		else
		{
			*rp++ = c;
			len++;
		}

		src++;
	}

	return len;
}

static uint64
esc_decode(const char *src, size_t srclen, char *dst)
{
	const char *end = src + srclen;
	char	   *rp = dst;
	uint64		len = 0;

	while (src < end)
	{
		if (src[0] != '\\')
			*rp++ = *src++;
		else if (src + 3 < end &&
				 (src[1] >= '0' && src[1] <= '3') &&
				 (src[2] >= '0' && src[2] <= '7') &&
				 (src[3] >= '0' && src[3] <= '7'))
		{
			int			val;

			val = VAL(src[1]);
			val <<= 3;
			val += VAL(src[2]);
			val <<= 3;
			*rp++ = val + VAL(src[3]);
			src += 4;
		}
		else if (src + 1 < end &&
				 (src[1] == '\\'))
		{
			*rp++ = '\\';
			src += 2;
		}
		else
		{
			/*
			 * 一个反斜杠,后非合法 ### 八进制。不应
			 * 到此,因 esc_dec_len 做同样检查。
			 */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s", "bytea")));
		}

		len++;
	}

	return len;
}

static uint64
esc_enc_len(const char *src, size_t srclen)
{
	const char *end = src + srclen;
	uint64		len = 0;

	while (src < end)
	{
		if (*src == '\0' || IS_HIGHBIT_SET(*src))
			len += 4;
		else if (*src == '\\')
			len += 2;
		else
			len++;

		src++;
	}

	return len;
}

static uint64
esc_dec_len(const char *src, size_t srclen)
{
	const char *end = src + srclen;
	uint64		len = 0;

	while (src < end)
	{
		if (src[0] != '\\')
			src++;
		else if (src + 3 < end &&
				 (src[1] >= '0' && src[1] <= '3') &&
				 (src[2] >= '0' && src[2] <= '7') &&
				 (src[3] >= '0' && src[3] <= '7'))
		{
			/*
			 * \ + 合法八进制
			 */
			src += 4;
		}
		else if (src + 1 < end &&
				 (src[1] == '\\'))
		{
			/*
			 * 两反斜杠 = 反斜杠
			 */
			src += 2;
		}
		else
		{
			/*
			 * 一反斜杠,后非合法 ### 八进制
			 */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s", "bytea")));
		}

		len++;
	}
	return len;
}

/*
 * Common
 */

static const struct
{
	const char *name;
	struct pg_encoding enc;
}			enclist[] =

{
	{
		"hex",
		{
			hex_enc_len, hex_dec_len, hex_encode, hex_decode
		}
	},
	{
		"base64",
		{
			pg_base64_enc_len, pg_base64_dec_len, pg_base64_encode, pg_base64_decode
		}
	},
	{
		"escape",
		{
			esc_enc_len, esc_dec_len, esc_encode, esc_decode
		}
	},
	{
		NULL,
		{
			NULL, NULL, NULL, NULL
		}
	}
};

static const struct pg_encoding *
pg_find_encoding(const char *name)
{
	int			i;

	for (i = 0; enclist[i].name; i++)
		if (pg_strcasecmp(enclist[i].name, name) == 0)
			return &enclist[i].enc;

	return NULL;
}
