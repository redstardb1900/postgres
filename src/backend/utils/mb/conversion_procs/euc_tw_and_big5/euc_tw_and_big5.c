/*-------------------------------------------------------------------------
 *
 *	  EUC_TW, BIG5 and MULE_INTERNAL
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/conversion_procs/euc_tw_and_big5/euc_tw_and_big5.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"

#define ENCODING_GROWTH_RATE 4

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(euc_tw_to_big5);
PG_FUNCTION_INFO_V1(big5_to_euc_tw);
PG_FUNCTION_INFO_V1(euc_tw_to_mic);
PG_FUNCTION_INFO_V1(mic_to_euc_tw);
PG_FUNCTION_INFO_V1(big5_to_mic);
PG_FUNCTION_INFO_V1(mic_to_big5);

/* ----------
 * conv_proc(
 *		INTEGER,	-- 源编码 id
 *		INTEGER,	-- 目标编码 id
 *		CSTRING,	-- 源串 (NUL 终止 C 串)
 *		CSTRING,	-- 目标串 (NUL 终止 C 串)
 *		INTEGER,	-- 源串长度
 *		BOOL		-- 为真时转换失败不抛错
 * ) returns INTEGER;
 *
 * 返回成功转换的字节数。
 * ----------
 */

static int	euc_tw2big5(const unsigned char *euc, unsigned char *p, int len, bool noError);
static int	big52euc_tw(const unsigned char *big5, unsigned char *p, int len, bool noError);
static int	big52mic(const unsigned char *big5, unsigned char *p, int len, bool noError);
static int	mic2big5(const unsigned char *mic, unsigned char *p, int len, bool noError);
static int	euc_tw2mic(const unsigned char *euc, unsigned char *p, int len, bool noError);
static int	mic2euc_tw(const unsigned char *mic, unsigned char *p, int len, bool noError);

Datum
euc_tw_to_big5(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_EUC_TW, PG_BIG5);

	converted = euc_tw2big5(src, dest, len, noError);

	PG_RETURN_INT32(converted);
}

Datum
big5_to_euc_tw(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_BIG5, PG_EUC_TW);

	converted = big52euc_tw(src, dest, len, noError);

	PG_RETURN_INT32(converted);
}

Datum
euc_tw_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_EUC_TW, PG_MULE_INTERNAL);

	converted = euc_tw2mic(src, dest, len, noError);

	PG_RETURN_INT32(converted);
}

Datum
mic_to_euc_tw(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_MULE_INTERNAL, PG_EUC_TW);

	converted = mic2euc_tw(src, dest, len, noError);

	PG_RETURN_INT32(converted);
}

Datum
big5_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_BIG5, PG_MULE_INTERNAL);

	converted = big52mic(src, dest, len, noError);

	PG_RETURN_INT32(converted);
}

Datum
mic_to_big5(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_MULE_INTERNAL, PG_BIG5);

	converted = mic2big5(src, dest, len, noError);

	PG_RETURN_INT32(converted);
}


/*
 * EUC_TW ---> Big5
 */
static int
euc_tw2big5(const unsigned char *euc, unsigned char *p, int len, bool noError)
{
	const unsigned char *start = euc;
	unsigned char c1;
	unsigned short big5buf,
				cnsBuf;
	unsigned char lc;
	int			l;

	while (len > 0)
	{
		c1 = *euc;
		if (IS_HIGHBIT_SET(c1))
		{
			/* 校验并解码下一 EUC_TW 输入字符 */
			l = pg_encoding_verifymbchar(PG_EUC_TW, (const char *) euc, len);
			if (l < 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_EUC_TW,
										(const char *) euc, len);
			}
			if (c1 == SS2)
			{
				c1 = euc[1];	/* 平面号 */
				if (c1 == 0xa1)
					lc = LC_CNS11643_1;
				else if (c1 == 0xa2)
					lc = LC_CNS11643_2;
				else
					lc = c1 - 0xa3 + LC_CNS11643_3;
				cnsBuf = (euc[2] << 8) | euc[3];
			}
			else
			{					/* CNS11643-1 */
				lc = LC_CNS11643_1;
				cnsBuf = (c1 << 8) | euc[1];
			}

			/* 写成 Big5 */
			big5buf = CNStoBIG5(cnsBuf, lc);
			if (big5buf == 0)
			{
				if (noError)
					break;
				report_untranslatable_char(PG_EUC_TW, PG_BIG5,
										   (const char *) euc, len);
			}
			*p++ = (big5buf >> 8) & 0x00ff;
			*p++ = big5buf & 0x00ff;

			euc += l;
			len -= l;
		}
		else
		{						/* 应为 ASCII */
			if (c1 == 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_EUC_TW,
										(const char *) euc, len);
			}
			*p++ = c1;
			euc++;
			len--;
		}
	}
	*p = '\0';

	return euc - start;
}

/*
 * Big5 ---> EUC_TW
 */
static int
big52euc_tw(const unsigned char *big5, unsigned char *p, int len, bool noError)
{
	const unsigned char *start = big5;
	unsigned short c1;
	unsigned short big5buf,
				cnsBuf;
	unsigned char lc;
	int			l;

	while (len > 0)
	{
		/* 校验并解码下一 Big5 输入字符 */
		c1 = *big5;
		if (IS_HIGHBIT_SET(c1))
		{
			l = pg_encoding_verifymbchar(PG_BIG5, (const char *) big5, len);
			if (l < 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_BIG5,
										(const char *) big5, len);
			}
			big5buf = (c1 << 8) | big5[1];
			cnsBuf = BIG5toCNS(big5buf, &lc);

			if (lc == LC_CNS11643_1)
			{
				*p++ = (cnsBuf >> 8) & 0x00ff;
				*p++ = cnsBuf & 0x00ff;
			}
			else if (lc == LC_CNS11643_2)
			{
				*p++ = SS2;
				*p++ = 0xa2;
				*p++ = (cnsBuf >> 8) & 0x00ff;
				*p++ = cnsBuf & 0x00ff;
			}
			else if (lc >= LC_CNS11643_3 && lc <= LC_CNS11643_7)
			{
				*p++ = SS2;
				*p++ = lc - LC_CNS11643_3 + 0xa3;
				*p++ = (cnsBuf >> 8) & 0x00ff;
				*p++ = cnsBuf & 0x00ff;
			}
			else
			{
				if (noError)
					break;
				report_untranslatable_char(PG_BIG5, PG_EUC_TW,
										   (const char *) big5, len);
			}

			big5 += l;
			len -= l;
		}
		else
		{
			/* ASCII */
			if (c1 == 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_BIG5,
										(const char *) big5, len);
			}
			*p++ = c1;
			big5++;
			len--;
			continue;
		}
	}
	*p = '\0';

	return big5 - start;
}

/*
 * EUC_TW ---> MIC
 */
static int
euc_tw2mic(const unsigned char *euc, unsigned char *p, int len, bool noError)
{
	const unsigned char *start = euc;
	int			c1;
	int			l;

	while (len > 0)
	{
		c1 = *euc;
		if (IS_HIGHBIT_SET(c1))
		{
			l = pg_encoding_verifymbchar(PG_EUC_TW, (const char *) euc, len);
			if (l < 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_EUC_TW,
										(const char *) euc, len);
			}
			if (c1 == SS2)
			{
				c1 = euc[1];	/* 平面号 */
				if (c1 == 0xa1)
					*p++ = LC_CNS11643_1;
				else if (c1 == 0xa2)
					*p++ = LC_CNS11643_2;
				else
				{
					/* 其它平面为 MULE 私有字符集 */
					*p++ = LCPRV2_B;
					*p++ = c1 - 0xa3 + LC_CNS11643_3;
				}
				*p++ = euc[2];
				*p++ = euc[3];
			}
			else
			{					/* CNS11643-1 */
				*p++ = LC_CNS11643_1;
				*p++ = c1;
				*p++ = euc[1];
			}
			euc += l;
			len -= l;
		}
		else
		{						/* 应为 ASCII */
			if (c1 == 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_EUC_TW,
										(const char *) euc, len);
			}
			*p++ = c1;
			euc++;
			len--;
		}
	}
	*p = '\0';

	return euc - start;
}

/*
 * MIC ---> EUC_TW
 */
static int
mic2euc_tw(const unsigned char *mic, unsigned char *p, int len, bool noError)
{
	const unsigned char *start = mic;
	int			c1;
	int			l;

	while (len > 0)
	{
		c1 = *mic;
		if (!IS_HIGHBIT_SET(c1))
		{
			/* ASCII */
			if (c1 == 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_MULE_INTERNAL,
										(const char *) mic, len);
			}
			*p++ = c1;
			mic++;
			len--;
			continue;
		}
		l = pg_encoding_verifymbchar(PG_MULE_INTERNAL, (const char *) mic, len);
		if (l < 0)
		{
			if (noError)
				break;
			report_invalid_encoding(PG_MULE_INTERNAL,
									(const char *) mic, len);
		}
		if (c1 == LC_CNS11643_1)
		{
			*p++ = mic[1];
			*p++ = mic[2];
		}
		else if (c1 == LC_CNS11643_2)
		{
			*p++ = SS2;
			*p++ = 0xa2;
			*p++ = mic[1];
			*p++ = mic[2];
		}
		else if (c1 == LCPRV2_B &&
				 mic[1] >= LC_CNS11643_3 && mic[1] <= LC_CNS11643_7)
		{
			*p++ = SS2;
			*p++ = mic[1] - LC_CNS11643_3 + 0xa3;
			*p++ = mic[2];
			*p++ = mic[3];
		}
		else
		{
			if (noError)
				break;
			report_untranslatable_char(PG_MULE_INTERNAL, PG_EUC_TW,
									   (const char *) mic, len);
		}
		mic += l;
		len -= l;
	}
	*p = '\0';

	return mic - start;
}

/*
 * Big5 ---> MIC
 */
static int
big52mic(const unsigned char *big5, unsigned char *p, int len, bool noError)
{
	const unsigned char *start = big5;
	unsigned short c1;
	unsigned short big5buf,
				cnsBuf;
	unsigned char lc;
	int			l;

	while (len > 0)
	{
		c1 = *big5;
		if (!IS_HIGHBIT_SET(c1))
		{
			/* ASCII */
			if (c1 == 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_BIG5,
										(const char *) big5, len);
			}
			*p++ = c1;
			big5++;
			len--;
			continue;
		}
		l = pg_encoding_verifymbchar(PG_BIG5, (const char *) big5, len);
		if (l < 0)
		{
			if (noError)
				break;
			report_invalid_encoding(PG_BIG5,
									(const char *) big5, len);
		}
		big5buf = (c1 << 8) | big5[1];
		cnsBuf = BIG5toCNS(big5buf, &lc);
		if (lc != 0)
		{
			/* 平面 3/4 为 MULE 私有字符集 */
			if (lc == LC_CNS11643_3 || lc == LC_CNS11643_4)
				*p++ = LCPRV2_B;
			*p++ = lc;			/* 平面号 */
			*p++ = (cnsBuf >> 8) & 0x00ff;
			*p++ = cnsBuf & 0x00ff;
		}
		else
		{
			if (noError)
				break;
			report_untranslatable_char(PG_BIG5, PG_MULE_INTERNAL,
									   (const char *) big5, len);
		}
		big5 += l;
		len -= l;
	}
	*p = '\0';

	return big5 - start;
}

/*
 * MIC ---> Big5
 */
static int
mic2big5(const unsigned char *mic, unsigned char *p, int len, bool noError)
{
	const unsigned char *start = mic;
	unsigned short c1;
	unsigned short big5buf,
				cnsBuf;
	int			l;

	while (len > 0)
	{
		c1 = *mic;
		if (!IS_HIGHBIT_SET(c1))
		{
			/* ASCII */
			if (c1 == 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_MULE_INTERNAL,
										(const char *) mic, len);
			}
			*p++ = c1;
			mic++;
			len--;
			continue;
		}
		l = pg_encoding_verifymbchar(PG_MULE_INTERNAL, (const char *) mic, len);
		if (l < 0)
		{
			if (noError)
				break;
			report_invalid_encoding(PG_MULE_INTERNAL,
									(const char *) mic, len);
		}
		if (c1 == LC_CNS11643_1 || c1 == LC_CNS11643_2 || c1 == LCPRV2_B)
		{
			if (c1 == LCPRV2_B)
			{
				c1 = mic[1];	/* 取平面号 */
				cnsBuf = (mic[2] << 8) | mic[3];
			}
			else
			{
				cnsBuf = (mic[1] << 8) | mic[2];
			}
			big5buf = CNStoBIG5(cnsBuf, c1);
			if (big5buf == 0)
			{
				if (noError)
					break;
				report_untranslatable_char(PG_MULE_INTERNAL, PG_BIG5,
										   (const char *) mic, len);
			}
			*p++ = (big5buf >> 8) & 0x00ff;
			*p++ = big5buf & 0x00ff;
		}
		else
		{
			if (noError)
				break;
			report_untranslatable_char(PG_MULE_INTERNAL, PG_BIG5,
									   (const char *) mic, len);
		}
		mic += l;
		len -= l;
	}
	*p = '\0';

	return mic - start;
}
