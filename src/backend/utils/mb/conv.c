/*-------------------------------------------------------------------------
 *
 *	  转换过程用的工具函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/conv.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "mb/pg_wchar.h"


/*
 * local2local: 通用单字节字符集编码
 * 在两个 ASCII 超集编码之间的转换。
 *
 * l 指向长度为 len 的源串
 * p 为输出区（必须足够大！）
 * src_encoding 为源编码的 PG 标识符
 * dest_encoding 为目标编码的 PG 标识符
 * tab 保存源字符集的转换表项
 * 自 128 (0x80) 起。表中每项保存对应的
 * 目标字符集码点；若无等价码则为 0。
 *
 * 返回已消耗的输入字节数。若 noError 为真，该值可
 * 小于 'len'。
 */
int
local2local(const unsigned char *l,
			unsigned char *p,
			int len,
			int src_encoding,
			int dest_encoding,
			const unsigned char *tab,
			bool noError)
{
	const unsigned char *start = l;
	unsigned char c1,
				c2;

	while (len > 0)
	{
		c1 = *l;
		if (c1 == 0)
		{
			if (noError)
				break;
			report_invalid_encoding(src_encoding, (const char *) l, len);
		}
		if (!IS_HIGHBIT_SET(c1))
			*p++ = c1;
		else
		{
			c2 = tab[c1 - HIGHBIT];
			if (c2)
				*p++ = c2;
			else
			{
				if (noError)
					break;
				report_untranslatable_char(src_encoding, dest_encoding,
										   (const char *) l, len);
			}
		}
		l++;
		len--;
	}
	*p = '\0';

	return l - start;
}

/*
 * LATINn ---> MIC（本地码直接映射到 MIC 时）
 *
 * l 指向长度为 len 的源串
 * p 为输出区（必须足够大！）
 * lc 为本地编码的 mule 字符集 id
 * encoding 为本地编码的 PG 标识符
 *
 * 返回已消耗的输入字节数。若 noError 为真，该值可
 * 小于 'len'。
 */
int
latin2mic(const unsigned char *l, unsigned char *p, int len,
		  int lc, int encoding, bool noError)
{
	const unsigned char *start = l;
	int			c1;

	while (len > 0)
	{
		c1 = *l;
		if (c1 == 0)
		{
			if (noError)
				break;
			report_invalid_encoding(encoding, (const char *) l, len);
		}
		if (IS_HIGHBIT_SET(c1))
			*p++ = lc;
		*p++ = c1;
		l++;
		len--;
	}
	*p = '\0';

	return l - start;
}

/*
 * MIC ---> LATINn（本地码直接映射到 MIC 时）
 *
 * mic 指向长度为 len 的源串
 * p 为输出区（必须足够大！）
 * lc 为本地编码的 mule 字符集 id
 * encoding 为本地编码的 PG 标识符
 *
 * 返回已消耗的输入字节数。若 noError 为真，该值可
 * 小于 'len'。
 */
int
mic2latin(const unsigned char *mic, unsigned char *p, int len,
		  int lc, int encoding, bool noError)
{
	const unsigned char *start = mic;
	int			c1;

	while (len > 0)
	{
		c1 = *mic;
		if (c1 == 0)
		{
			if (noError)
				break;
			report_invalid_encoding(PG_MULE_INTERNAL, (const char *) mic, len);
		}
		if (!IS_HIGHBIT_SET(c1))
		{
			/* ASCII 简单 */
			*p++ = c1;
			mic++;
			len--;
		}
		else
		{
			int			l = pg_mule_mblen(mic);

			if (len < l)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_MULE_INTERNAL, (const char *) mic,
										len);
			}
			if (l != 2 || c1 != lc || !IS_HIGHBIT_SET(mic[1]))
			{
				if (noError)
					break;
				report_untranslatable_char(PG_MULE_INTERNAL, encoding,
										   (const char *) mic, len);
			}
			*p++ = mic[1];
			mic += 2;
			len -= 2;
		}
	}
	*p = '\0';

	return mic - start;
}


/*
 * latin2mic_with_table: 通用单字节字符集编码
 * 从本地字符集转换到 mule 内部码。
 *
 * l 指向长度为 len 的源串
 * p 为输出区（必须足够大！）
 * lc 为本地编码的 mule 字符集 id
 * encoding 为本地编码的 PG 标识符
 * tab 保存本地字符集的转换表项
 * 自 128 (0x80) 起。表中每项保存对应的
 * mule 编码码点；若无等价码则为 0。
 *
 * 返回已消耗的输入字节数。若 noError 为真，该值可
 * 小于 'len'。
 */
int
latin2mic_with_table(const unsigned char *l,
					 unsigned char *p,
					 int len,
					 int lc,
					 int encoding,
					 const unsigned char *tab,
					 bool noError)
{
	const unsigned char *start = l;
	unsigned char c1,
				c2;

	while (len > 0)
	{
		c1 = *l;
		if (c1 == 0)
		{
			if (noError)
				break;
			report_invalid_encoding(encoding, (const char *) l, len);
		}
		if (!IS_HIGHBIT_SET(c1))
			*p++ = c1;
		else
		{
			c2 = tab[c1 - HIGHBIT];
			if (c2)
			{
				*p++ = lc;
				*p++ = c2;
			}
			else
			{
				if (noError)
					break;
				report_untranslatable_char(encoding, PG_MULE_INTERNAL,
										   (const char *) l, len);
			}
		}
		l++;
		len--;
	}
	*p = '\0';

	return l - start;
}

/*
 * mic2latin_with_table: 通用单字节字符集编码
 * 从 mule 内部码转换到本地字符集。
 *
 * mic 指向长度为 len 的源串
 * p 为输出区（必须足够大！）
 * lc 为本地编码的 mule 字符集 id
 * encoding 为本地编码的 PG 标识符
 * tab 保存 mule 内部码第二字节的转换表项，
 * 自 128 (0x80) 起。表中每项保存对应的
 * 本地字符集码点；若无等价码则为 0。
 *
 * 返回已消耗的输入字节数。若 noError 为真，该值可
 * 小于 'len'。
 */
int
mic2latin_with_table(const unsigned char *mic,
					 unsigned char *p,
					 int len,
					 int lc,
					 int encoding,
					 const unsigned char *tab,
					 bool noError)
{
	const unsigned char *start = mic;
	unsigned char c1,
				c2;

	while (len > 0)
	{
		c1 = *mic;
		if (c1 == 0)
		{
			if (noError)
				break;
			report_invalid_encoding(PG_MULE_INTERNAL, (const char *) mic, len);
		}
		if (!IS_HIGHBIT_SET(c1))
		{
			/* ASCII 简单 */
			*p++ = c1;
			mic++;
			len--;
		}
		else
		{
			int			l = pg_mule_mblen(mic);

			if (len < l)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_MULE_INTERNAL, (const char *) mic,
										len);
			}
			if (l != 2 || c1 != lc || !IS_HIGHBIT_SET(mic[1]) ||
				(c2 = tab[mic[1] - HIGHBIT]) == 0)
			{
				if (noError)
					break;
				report_untranslatable_char(PG_MULE_INTERNAL, encoding,
										   (const char *) mic, len);
				break;			/* 让编译器安静 */
			}
			*p++ = c2;
			mic += 2;
			len -= 2;
		}
	}
	*p = '\0';

	return mic - start;
}

/*
 * 供 bsearch() 用的比较例程
 * 本例程用于组合 UTF8 -> 本地码
 */
static int
compare3(const void *p1, const void *p2)
{
	uint32		s1,
				s2,
				d1,
				d2;

	s1 = *(const uint32 *) p1;
	s2 = *((const uint32 *) p1 + 1);
	d1 = ((const pg_utf_to_local_combined *) p2)->utf1;
	d2 = ((const pg_utf_to_local_combined *) p2)->utf2;
	return (s1 > d1 || (s1 == d1 && s2 > d2)) ? 1 : ((s1 == d1 && s2 == d2) ? 0 : -1);
}

/*
 * 供 bsearch() 用的比较例程
 * 本例程用于本地码 -> 组合 UTF8
 */
static int
compare4(const void *p1, const void *p2)
{
	uint32		v1,
				v2;

	v1 = *(const uint32 *) p1;
	v2 = ((const pg_local_to_utf_combined *) p2)->code;
	return (v1 > v2) ? 1 : ((v1 == v2) ? 0 : -1);
}

/*
 * 把 32 位字符表示存入多字节流
 */
static inline unsigned char *
store_coded_char(unsigned char *dest, uint32 code)
{
	if (code & 0xff000000)
		*dest++ = code >> 24;
	if (code & 0x00ff0000)
		*dest++ = code >> 16;
	if (code & 0x0000ff00)
		*dest++ = code >> 8;
	if (code & 0x000000ff)
		*dest++ = code;
	return dest;
}

/*
 * 用转换基数树转换一个字符。
 *
 * 'l' 为输入字符的字节长度，b1-b4 为
 * 输入字符的各字节。
 */
static inline uint32
pg_mb_radix_conv(const pg_mb_radix_tree *rt,
				 int l,
				 unsigned char b1,
				 unsigned char b2,
				 unsigned char b3,
				 unsigned char b4)
{
	if (l == 4)
	{
		/* 4字节码 */

		/* 检查码有效性 */
		if (b1 < rt->b4_1_lower || b1 > rt->b4_1_upper ||
			b2 < rt->b4_2_lower || b2 > rt->b4_2_upper ||
			b3 < rt->b4_3_lower || b3 > rt->b4_3_upper ||
			b4 < rt->b4_4_lower || b4 > rt->b4_4_upper)
			return 0;

		/* 执行查找 */
		if (rt->chars32)
		{
			uint32		idx = rt->b4root;

			idx = rt->chars32[b1 + idx - rt->b4_1_lower];
			idx = rt->chars32[b2 + idx - rt->b4_2_lower];
			idx = rt->chars32[b3 + idx - rt->b4_3_lower];
			return rt->chars32[b4 + idx - rt->b4_4_lower];
		}
		else
		{
			uint16		idx = rt->b4root;

			idx = rt->chars16[b1 + idx - rt->b4_1_lower];
			idx = rt->chars16[b2 + idx - rt->b4_2_lower];
			idx = rt->chars16[b3 + idx - rt->b4_3_lower];
			return rt->chars16[b4 + idx - rt->b4_4_lower];
		}
	}
	else if (l == 3)
	{
		/* 3字节码 */

		/* 检查码有效性 */
		if (b2 < rt->b3_1_lower || b2 > rt->b3_1_upper ||
			b3 < rt->b3_2_lower || b3 > rt->b3_2_upper ||
			b4 < rt->b3_3_lower || b4 > rt->b3_3_upper)
			return 0;

		/* 执行查找 */
		if (rt->chars32)
		{
			uint32		idx = rt->b3root;

			idx = rt->chars32[b2 + idx - rt->b3_1_lower];
			idx = rt->chars32[b3 + idx - rt->b3_2_lower];
			return rt->chars32[b4 + idx - rt->b3_3_lower];
		}
		else
		{
			uint16		idx = rt->b3root;

			idx = rt->chars16[b2 + idx - rt->b3_1_lower];
			idx = rt->chars16[b3 + idx - rt->b3_2_lower];
			return rt->chars16[b4 + idx - rt->b3_3_lower];
		}
	}
	else if (l == 2)
	{
		/* 2字节码 */

		/* 检查码有效性 - 首字节 */
		if (b3 < rt->b2_1_lower || b3 > rt->b2_1_upper ||
			b4 < rt->b2_2_lower || b4 > rt->b2_2_upper)
			return 0;

		/* 执行查找 */
		if (rt->chars32)
		{
			uint32		idx = rt->b2root;

			idx = rt->chars32[b3 + idx - rt->b2_1_lower];
			return rt->chars32[b4 + idx - rt->b2_2_lower];
		}
		else
		{
			uint16		idx = rt->b2root;

			idx = rt->chars16[b3 + idx - rt->b2_1_lower];
			return rt->chars16[b4 + idx - rt->b2_2_lower];
		}
	}
	else if (l == 1)
	{
		/* 1字节码 */

		/* 检查码有效性 - 首字节 */
		if (b4 < rt->b1_lower || b4 > rt->b1_upper)
			return 0;

		/* 执行查找 */
		if (rt->chars32)
			return rt->chars32[b4 + rt->b1root - rt->b1_lower];
		else
			return rt->chars16[b4 + rt->b1root - rt->b1_lower];
	}
	return 0;					/* 不应发生 */
}

/*
 * UTF8 ---> 本地码
 *
 * utf: UTF8 编码的输入串（不必以空字节结尾）
 * len: 输入串长度（字节）
 * iso: 指向输出区的指针（必须足够大！）
		  （输出串将空终止）
 * map: 单字符转换映射
 * cmap: 组合字符转换映射
 *		  (可选，无则传 NULL)
 * cmapsize: 组合字符转换映射的表项数
 *		  (可选，无则传 0)
 * conv_func: 算法型编码转换函数
 *		  (可选，无则传 NULL)
 * encoding: 本地编码的 PG 标识符
 *
 * 对每个字符，先查 cmap（若已提供）；无匹配则
 * 再查 map；仍无匹配则应用 conv_func（若已提供）。
 * 若仍无匹配则报错。
 *
 * 数据结构细节见 pg_wchar.h。
 *
 * 返回已消耗的输入字节数。若 noError 为真，该值可
 * 小于 'len'。
 */
int
UtfToLocal(const unsigned char *utf, int len,
		   unsigned char *iso,
		   const pg_mb_radix_tree *map,
		   const pg_utf_to_local_combined *cmap, int cmapsize,
		   utf_local_conversion_func conv_func,
		   int encoding, bool noError)
{
	uint32		iutf;
	int			l;
	const pg_utf_to_local_combined *cp;
	const unsigned char *start = utf;

	if (!PG_VALID_ENCODING(encoding))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid encoding number: %d", encoding)));

	for (; len > 0; len -= l)
	{
		unsigned char b1 = 0;
		unsigned char b2 = 0;
		unsigned char b3 = 0;
		unsigned char b4 = 0;

		/* "break" 情形均表示错误 */
		if (*utf == '\0')
			break;

		l = pg_utf_mblen(utf);
		if (len < l)
			break;

		if (!pg_utf8_islegal(utf, l))
			break;

		if (l == 1)
		{
			/* ASCII 情形简单，假定为一对一转换 */
			*iso++ = *utf++;
			continue;
		}

		/* 收集长为 l 的编码字符 */
		if (l == 2)
		{
			b3 = *utf++;
			b4 = *utf++;
		}
		else if (l == 3)
		{
			b2 = *utf++;
			b3 = *utf++;
			b4 = *utf++;
		}
		else if (l == 4)
		{
			b1 = *utf++;
			b2 = *utf++;
			b3 = *utf++;
			b4 = *utf++;
		}
		else
		{
			elog(ERROR, "unsupported character length %d", l);
			iutf = 0;			/* 让编译器安静 */
		}
		iutf = (b1 << 24 | b2 << 16 | b3 << 8 | b4);

		/* 首先，若可能则试用组合映射 */
		if (cmap && len > l)
		{
			const unsigned char *utf_save = utf;
			int			len_save = len;
			int			l_save = l;

			/* 收集下一字符，同上 */
			len -= l;

			l = pg_utf_mblen(utf);
			if (len < l)
			{
				/* 需更多数据才能判定是否为组合字符 */
				utf -= l_save;
				break;
			}

			if (!pg_utf8_islegal(utf, l))
			{
				if (!noError)
					report_invalid_encoding(PG_UTF8, (const char *) utf, len);
				utf -= l_save;
				break;
			}

			/* 假定 ASCII 字符不会出现在组合映射中 */
			if (l > 1)
			{
				uint32		iutf2;
				uint32		cutf[2];

				if (l == 2)
				{
					iutf2 = *utf++ << 8;
					iutf2 |= *utf++;
				}
				else if (l == 3)
				{
					iutf2 = *utf++ << 16;
					iutf2 |= *utf++ << 8;
					iutf2 |= *utf++;
				}
				else if (l == 4)
				{
					iutf2 = *utf++ << 24;
					iutf2 |= *utf++ << 16;
					iutf2 |= *utf++ << 8;
					iutf2 |= *utf++;
				}
				else
				{
					elog(ERROR, "unsupported character length %d", l);
					iutf2 = 0;	/* 让编译器安静 */
				}

				cutf[0] = iutf;
				cutf[1] = iutf2;

				cp = bsearch(cutf, cmap, cmapsize,
							 sizeof(pg_utf_to_local_combined), compare3);

				if (cp)
				{
					iso = store_coded_char(iso, cp->code);
					continue;
				}
			}

			/* 失败，回退以便下次再处理第二字符 */
			utf = utf_save;
			len = len_save;
			l = l_save;
		}

		/* 现检查普通映射 */
		if (map)
		{
			uint32		converted = pg_mb_radix_conv(map, l, b1, b2, b3, b4);

			if (converted)
			{
				iso = store_coded_char(iso, converted);
				continue;
			}
		}

		/* 若有转换函数，则试用 */
		if (conv_func)
		{
			uint32		converted = (*conv_func) (iutf);

			if (converted)
			{
				iso = store_coded_char(iso, converted);
				continue;
			}
		}

		/* 未能翻译此字符 */
		utf -= l;
		if (noError)
			break;
		report_untranslatable_char(PG_UTF8, encoding,
								   (const char *) utf, len);
	}

	/* 若提前跳出循环，则输入必无效 */
	if (len > 0 && !noError)
		report_invalid_encoding(PG_UTF8, (const char *) utf, len);

	*iso = '\0';

	return utf - start;
}

/*
 * 本地码 ---> UTF8
 *
 * iso: 本地编码的输入串（不必以空字节结尾）
 * len: 输入串长度（字节）
 * utf: 指向输出区的指针（必须足够大！）
		  （输出串将空终止）
 * map: 单字符转换映射
 * cmap: 组合字符转换映射
 *		  (可选，无则传 NULL)
 * cmapsize: 组合字符转换映射的表项数
 *		  (可选，无则传 0)
 * conv_func: 算法型编码转换函数
 *		  (可选，无则传 NULL)
 * encoding: 本地编码的 PG 标识符
 *
 * 对每个字符先查 map；无匹配则再查 cmap（若
 * 已提供）；仍无匹配则应用 conv_func（若已
 * 提供）。若仍无匹配则报错。
 *
 * 数据结构细节见 pg_wchar.h。
 *
 * 返回已消耗的输入字节数。若 noError 为真，该值可
 * 小于 'len'。
 */
int
LocalToUtf(const unsigned char *iso, int len,
		   unsigned char *utf,
		   const pg_mb_radix_tree *map,
		   const pg_local_to_utf_combined *cmap, int cmapsize,
		   utf_local_conversion_func conv_func,
		   int encoding,
		   bool noError)
{
	uint32		iiso;
	int			l;
	const pg_local_to_utf_combined *cp;
	const unsigned char *start = iso;

	if (!PG_VALID_ENCODING(encoding))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid encoding number: %d", encoding)));

	for (; len > 0; len -= l)
	{
		unsigned char b1 = 0;
		unsigned char b2 = 0;
		unsigned char b3 = 0;
		unsigned char b4 = 0;

		/* "break" 情形均表示错误 */
		if (*iso == '\0')
			break;

		if (!IS_HIGHBIT_SET(*iso))
		{
			/* ASCII 情形简单，假定为一对一转换 */
			*utf++ = *iso++;
			l = 1;
			continue;
		}

		l = pg_encoding_verifymbchar(encoding, (const char *) iso, len);
		if (l < 0)
			break;

		/* 收集长为 l 的编码字符 */
		if (l == 1)
			b4 = *iso++;
		else if (l == 2)
		{
			b3 = *iso++;
			b4 = *iso++;
		}
		else if (l == 3)
		{
			b2 = *iso++;
			b3 = *iso++;
			b4 = *iso++;
		}
		else if (l == 4)
		{
			b1 = *iso++;
			b2 = *iso++;
			b3 = *iso++;
			b4 = *iso++;
		}
		else
		{
			elog(ERROR, "unsupported character length %d", l);
			iiso = 0;			/* 让编译器安静 */
		}
		iiso = (b1 << 24 | b2 << 16 | b3 << 8 | b4);

		if (map)
		{
			uint32		converted = pg_mb_radix_conv(map, l, b1, b2, b3, b4);

			if (converted)
			{
				utf = store_coded_char(utf, converted);
				continue;
			}

			/* 若有组合字符映射，则试用 */
			if (cmap)
			{
				cp = bsearch(&iiso, cmap, cmapsize,
							 sizeof(pg_local_to_utf_combined), compare4);

				if (cp)
				{
					utf = store_coded_char(utf, cp->utf1);
					utf = store_coded_char(utf, cp->utf2);
					continue;
				}
			}
		}

		/* 若有转换函数，则试用 */
		if (conv_func)
		{
			uint32		converted = (*conv_func) (iiso);

			if (converted)
			{
				utf = store_coded_char(utf, converted);
				continue;
			}
		}

		/* 未能翻译此字符 */
		iso -= l;
		if (noError)
			break;
		report_untranslatable_char(encoding, PG_UTF8,
								   (const char *) iso, len);
	}

	/* 若提前跳出循环，则输入必无效 */
	if (len > 0 && !noError)
		report_invalid_encoding(encoding, (const char *) iso, len);

	*utf = '\0';

	return iso - start;
}
