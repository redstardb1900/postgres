/*-------------------------------------------------------------------------
 * oracle_compat.c
 *	Oracle 兼容函数。
 *
 * Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *	Author: Edmund Mergl <E.Mergl@bawue.de>
 *	Multibyte enhancement: Tatsuo Ishii <ishii@postgresql.org>
 *
 *
 * IDENTIFICATION
 *	src/backend/utils/adt/oracle_compat.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/int.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/memutils.h"
#include "varatt.h"


static text *dotrim(const char *string, int stringlen,
					const char *set, int setlen,
					bool doltrim, bool dortrim);
static bytea *dobyteatrim(bytea *string, bytea *set,
						  bool doltrim, bool dortrim);


/********************************************************************
 *
 * lower
 *
 * 语法:
 *
 *	 text lower(text string)
 *
 * 目的:
 *
 *	 返回 string，所有字母强制为小写。
 *
 ********************************************************************/

Datum
lower(PG_FUNCTION_ARGS)
{
	text	   *in_string = PG_GETARG_TEXT_PP(0);
	char	   *out_string;
	text	   *result;

	out_string = str_tolower(VARDATA_ANY(in_string),
							 VARSIZE_ANY_EXHDR(in_string),
							 PG_GET_COLLATION());
	result = cstring_to_text(out_string);
	pfree(out_string);

	PG_RETURN_TEXT_P(result);
}


/********************************************************************
 *
 * upper
 *
 * 语法:
 *
 *	 text upper(text string)
 *
 * 目的:
 *
 *	 返回 string，所有字母强制为大写。
 *
 ********************************************************************/

Datum
upper(PG_FUNCTION_ARGS)
{
	text	   *in_string = PG_GETARG_TEXT_PP(0);
	char	   *out_string;
	text	   *result;

	out_string = str_toupper(VARDATA_ANY(in_string),
							 VARSIZE_ANY_EXHDR(in_string),
							 PG_GET_COLLATION());
	result = cstring_to_text(out_string);
	pfree(out_string);

	PG_RETURN_TEXT_P(result);
}


/********************************************************************
 *
 * initcap
 *
 * 语法:
 *
 *	 text initcap(text string)
 *
 * 目的:
 *
 *	 返回 string，其中每个单词的首字母大写，
 *	 其余字母小写。单词的定义是由
 *	 字母数字字符构成、以非字母数字
 *	 分隔。
 *
 ********************************************************************/

Datum
initcap(PG_FUNCTION_ARGS)
{
	text	   *in_string = PG_GETARG_TEXT_PP(0);
	char	   *out_string;
	text	   *result;

	out_string = str_initcap(VARDATA_ANY(in_string),
							 VARSIZE_ANY_EXHDR(in_string),
							 PG_GET_COLLATION());
	result = cstring_to_text(out_string);
	pfree(out_string);

	PG_RETURN_TEXT_P(result);
}


/********************************************************************
 *
 * lpad
 *
 * 语法:
 *
 *	 text lpad(text string1, int4 len, text string2)
 *
 * 目的:
 *
 *	 返回 string1，用 string2 的字符序列将其向左
 *	 填充至 len 长度。若 len 小于 string1 的长度，
 *	 则改为（右侧）截断至 len。
 *
 ********************************************************************/

Datum
lpad(PG_FUNCTION_ARGS)
{
	text	   *string1 = PG_GETARG_TEXT_PP(0);
	int32		len = PG_GETARG_INT32(1);
	text	   *string2 = PG_GETARG_TEXT_PP(2);
	text	   *ret;
	char	   *ptr1,
			   *ptr2,
			   *ptr2start,
			   *ptr2end,
			   *ptr_ret;
	int			m,
				s1len,
				s2len;
	int			bytelen;

	/* len 为负时静默当作零 */
	if (len < 0)
		len = 0;

	s1len = VARSIZE_ANY_EXHDR(string1);
	if (s1len < 0)
		s1len = 0;				/* 不应发生 */

	s2len = VARSIZE_ANY_EXHDR(string2);
	if (s2len < 0)
		s2len = 0;				/* 不应发生 */

	s1len = pg_mbstrlen_with_len(VARDATA_ANY(string1), s1len);

	if (s1len > len)
		s1len = len;			/* 把 string1 截到 len 字符 */

	if (s2len <= 0)
		len = s1len;			/* 没有填充串，故不填充 */

	/* 计算最坏情况输出长度 */
	if (unlikely(pg_mul_s32_overflow(pg_database_encoding_max_length(), len,
									 &bytelen)) ||
		unlikely(pg_add_s32_overflow(bytelen, VARHDRSZ, &bytelen)) ||
		unlikely(!AllocSizeIsValid(bytelen)))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested length too large")));

	ret = (text *) palloc(bytelen);

	m = len - s1len;

	ptr2 = ptr2start = VARDATA_ANY(string2);
	ptr2end = ptr2 + s2len;
	ptr_ret = VARDATA(ret);

	while (m--)
	{
		int			mlen = pg_mblen(ptr2);

		memcpy(ptr_ret, ptr2, mlen);
		ptr_ret += mlen;
		ptr2 += mlen;
		if (ptr2 == ptr2end)	/* 到 s2 末尾时回绕 */
			ptr2 = ptr2start;
	}

	ptr1 = VARDATA_ANY(string1);

	while (s1len--)
	{
		int			mlen = pg_mblen(ptr1);

		memcpy(ptr_ret, ptr1, mlen);
		ptr_ret += mlen;
		ptr1 += mlen;
	}

	SET_VARSIZE(ret, ptr_ret - (char *) ret);

	PG_RETURN_TEXT_P(ret);
}


/********************************************************************
 *
 * rpad
 *
 * 语法:
 *
 *	 text rpad(text string1, int4 len, text string2)
 *
 * 目的:
 *
 *	 返回 string1，用 string2 的字符序列将其向右
 *	 填充至 len 长度。若 len 小于 string1 的长度，
 *	 则改为（右侧）截断至 len。
 *
 ********************************************************************/

Datum
rpad(PG_FUNCTION_ARGS)
{
	text	   *string1 = PG_GETARG_TEXT_PP(0);
	int32		len = PG_GETARG_INT32(1);
	text	   *string2 = PG_GETARG_TEXT_PP(2);
	text	   *ret;
	char	   *ptr1,
			   *ptr2,
			   *ptr2start,
			   *ptr2end,
			   *ptr_ret;
	int			m,
				s1len,
				s2len;
	int			bytelen;

	/* len 为负时静默当作零 */
	if (len < 0)
		len = 0;

	s1len = VARSIZE_ANY_EXHDR(string1);
	if (s1len < 0)
		s1len = 0;				/* 不应发生 */

	s2len = VARSIZE_ANY_EXHDR(string2);
	if (s2len < 0)
		s2len = 0;				/* 不应发生 */

	s1len = pg_mbstrlen_with_len(VARDATA_ANY(string1), s1len);

	if (s1len > len)
		s1len = len;			/* 把 string1 截到 len 字符 */

	if (s2len <= 0)
		len = s1len;			/* 没有填充串，故不填充 */

	/* 计算最坏情况输出长度 */
	if (unlikely(pg_mul_s32_overflow(pg_database_encoding_max_length(), len,
									 &bytelen)) ||
		unlikely(pg_add_s32_overflow(bytelen, VARHDRSZ, &bytelen)) ||
		unlikely(!AllocSizeIsValid(bytelen)))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested length too large")));

	ret = (text *) palloc(bytelen);

	m = len - s1len;

	ptr1 = VARDATA_ANY(string1);
	ptr_ret = VARDATA(ret);

	while (s1len--)
	{
		int			mlen = pg_mblen(ptr1);

		memcpy(ptr_ret, ptr1, mlen);
		ptr_ret += mlen;
		ptr1 += mlen;
	}

	ptr2 = ptr2start = VARDATA_ANY(string2);
	ptr2end = ptr2 + s2len;

	while (m--)
	{
		int			mlen = pg_mblen(ptr2);

		memcpy(ptr_ret, ptr2, mlen);
		ptr_ret += mlen;
		ptr2 += mlen;
		if (ptr2 == ptr2end)	/* 到 s2 末尾时回绕 */
			ptr2 = ptr2start;
	}

	SET_VARSIZE(ret, ptr_ret - (char *) ret);

	PG_RETURN_TEXT_P(ret);
}


/********************************************************************
 *
 * btrim
 *
 * 语法:
 *
 *	 text btrim(text string, text set)
 *
 * 目的:
 *
 *	 返回 string，将其从头、尾两端移除字符，
 *	 直到不在 set 中的首字符。
 *
 ********************************************************************/

Datum
btrim(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *set = PG_GETARG_TEXT_PP(1);
	text	   *ret;

	ret = dotrim(VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string),
				 VARDATA_ANY(set), VARSIZE_ANY_EXHDR(set),
				 true, true);

	PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * btrim1 --- set 固定为 ' ' 的 btrim
 *
 ********************************************************************/

Datum
btrim1(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *ret;

	ret = dotrim(VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string),
				 " ", 1,
				 true, true);

	PG_RETURN_TEXT_P(ret);
}

/*
 * btrim、ltrim、rtrim 的公共实现
 */
static text *
dotrim(const char *string, int stringlen,
	   const char *set, int setlen,
	   bool doltrim, bool dortrim)
{
	int			i;

	/* string 或 set 为空时无事可做 */
	if (stringlen > 0 && setlen > 0)
	{
		if (pg_database_encoding_max_length() > 1)
		{
			/*
			 * 多字节编码情形下，构建指向各字符起始
			 * 位置的指针数组，从而避免内层循环里的
			 * 低效检查。
			 */
			const char **stringchars;
			const char **setchars;
			int		   *stringmblen;
			int		   *setmblen;
			int			stringnchars;
			int			setnchars;
			int			resultndx;
			int			resultnchars;
			const char *p;
			int			len;
			int			mblen;
			const char *str_pos;
			int			str_len;

			stringchars = (const char **) palloc(stringlen * sizeof(char *));
			stringmblen = (int *) palloc(stringlen * sizeof(int));
			stringnchars = 0;
			p = string;
			len = stringlen;
			while (len > 0)
			{
				stringchars[stringnchars] = p;
				stringmblen[stringnchars] = mblen = pg_mblen(p);
				stringnchars++;
				p += mblen;
				len -= mblen;
			}

			setchars = (const char **) palloc(setlen * sizeof(char *));
			setmblen = (int *) palloc(setlen * sizeof(int));
			setnchars = 0;
			p = set;
			len = setlen;
			while (len > 0)
			{
				setchars[setnchars] = p;
				setmblen[setnchars] = mblen = pg_mblen(p);
				setnchars++;
				p += mblen;
				len -= mblen;
			}

			resultndx = 0;		/* stringchars[] 下标 */
			resultnchars = stringnchars;

			if (doltrim)
			{
				while (resultnchars > 0)
				{
					str_pos = stringchars[resultndx];
					str_len = stringmblen[resultndx];
					for (i = 0; i < setnchars; i++)
					{
						if (str_len == setmblen[i] &&
							memcmp(str_pos, setchars[i], str_len) == 0)
							break;
					}
					if (i >= setnchars)
						break;	/* 无匹配 */
					string += str_len;
					stringlen -= str_len;
					resultndx++;
					resultnchars--;
				}
			}

			if (dortrim)
			{
				while (resultnchars > 0)
				{
					str_pos = stringchars[resultndx + resultnchars - 1];
					str_len = stringmblen[resultndx + resultnchars - 1];
					for (i = 0; i < setnchars; i++)
					{
						if (str_len == setmblen[i] &&
							memcmp(str_pos, setchars[i], str_len) == 0)
							break;
					}
					if (i >= setnchars)
						break;	/* 无匹配 */
					stringlen -= str_len;
					resultnchars--;
				}
			}

			pfree(stringchars);
			pfree(stringmblen);
			pfree(setchars);
			pfree(setmblen);
		}
		else
		{
			/*
			 * 单字节编码情形下无需这种额外开销。
			 */
			if (doltrim)
			{
				while (stringlen > 0)
				{
					char		str_ch = *string;

					for (i = 0; i < setlen; i++)
					{
						if (str_ch == set[i])
							break;
					}
					if (i >= setlen)
						break;	/* 无匹配 */
					string++;
					stringlen--;
				}
			}

			if (dortrim)
			{
				while (stringlen > 0)
				{
					char		str_ch = string[stringlen - 1];

					for (i = 0; i < setlen; i++)
					{
						if (str_ch == set[i])
							break;
					}
					if (i >= setlen)
						break;	/* 无匹配 */
					stringlen--;
				}
			}
		}
	}

	/* 返回选中的字符串片段 */
	return cstring_to_text_with_len(string, stringlen);
}

/*
 * btrim、ltrim、rtrim 的 bytea 版本公共实现
 */
bytea *
dobyteatrim(bytea *string, bytea *set, bool doltrim, bool dortrim)
{
	bytea	   *ret;
	char	   *ptr,
			   *end,
			   *ptr2,
			   *ptr2start,
			   *end2;
	int			m,
				stringlen,
				setlen;

	stringlen = VARSIZE_ANY_EXHDR(string);
	setlen = VARSIZE_ANY_EXHDR(set);

	if (stringlen <= 0 || setlen <= 0)
		return string;

	m = stringlen;
	ptr = VARDATA_ANY(string);
	end = ptr + stringlen - 1;
	ptr2start = VARDATA_ANY(set);
	end2 = ptr2start + setlen - 1;

	if (doltrim)
	{
		while (m > 0)
		{
			ptr2 = ptr2start;
			while (ptr2 <= end2)
			{
				if (*ptr == *ptr2)
					break;
				++ptr2;
			}
			if (ptr2 > end2)
				break;
			ptr++;
			m--;
		}
	}

	if (dortrim)
	{
		while (m > 0)
		{
			ptr2 = ptr2start;
			while (ptr2 <= end2)
			{
				if (*end == *ptr2)
					break;
				++ptr2;
			}
			if (ptr2 > end2)
				break;
			end--;
			m--;
		}
	}

	ret = (bytea *) palloc(VARHDRSZ + m);
	SET_VARSIZE(ret, VARHDRSZ + m);
	memcpy(VARDATA(ret), ptr, m);
	return ret;
}

/********************************************************************
 *
 * byteatrim
 *
 * 语法:
 *
 *	 bytea byteatrim(bytea string, bytea set)
 *
 * 目的:
 *
 *	 返回 string，将其从头、尾两端移除字符，
 *	 直到不在 set 中的首字符。
 *
 * 从 btrim 克隆并按需修改。
 ********************************************************************/

Datum
byteatrim(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_PP(0);
	bytea	   *set = PG_GETARG_BYTEA_PP(1);
	bytea	   *ret;

	ret = dobyteatrim(string, set, true, true);

	PG_RETURN_BYTEA_P(ret);
}

/********************************************************************
 *
 * bytealtrim
 *
 * 语法:
 *
 *	 bytea bytealtrim(bytea string, bytea set)
 *
 * 目的:
 *
 *	 返回 string，移除起始字符直到不在 set 中的
 *	 首个字符为止。
 *
 ********************************************************************/

Datum
bytealtrim(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_PP(0);
	bytea	   *set = PG_GETARG_BYTEA_PP(1);
	bytea	   *ret;

	ret = dobyteatrim(string, set, true, false);

	PG_RETURN_BYTEA_P(ret);
}

/********************************************************************
 *
 * byteartrim
 *
 * 语法:
 *
 *	 bytea byteartrim(bytea string, bytea set)
 *
 * 目的:
 *
 *	 返回 string，删去末尾字符，直到最后一个
 *	 不在 set 即止。
 *
 ********************************************************************/

Datum
byteartrim(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_PP(0);
	bytea	   *set = PG_GETARG_BYTEA_PP(1);
	bytea	   *ret;

	ret = dobyteatrim(string, set, false, true);

	PG_RETURN_BYTEA_P(ret);
}

/********************************************************************
 *
 * ltrim
 *
 * 语法:
 *
 *	 text ltrim(text string, text set)
 *
 * 目的:
 *
 *	 返回 string，移除起始字符直到不在 set 中的
 *	 首个字符为止。
 *
 ********************************************************************/

Datum
ltrim(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *set = PG_GETARG_TEXT_PP(1);
	text	   *ret;

	ret = dotrim(VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string),
				 VARDATA_ANY(set), VARSIZE_ANY_EXHDR(set),
				 true, false);

	PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * ltrim1 --- set 固定为 ' ' 的 ltrim
 *
 ********************************************************************/

Datum
ltrim1(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *ret;

	ret = dotrim(VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string),
				 " ", 1,
				 true, false);

	PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * rtrim
 *
 * 语法:
 *
 *	 text rtrim(text string, text set)
 *
 * 目的:
 *
 *	 返回 string，删去末尾字符，直到最后一个
 *	 不在 set 即止。
 *
 ********************************************************************/

Datum
rtrim(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *set = PG_GETARG_TEXT_PP(1);
	text	   *ret;

	ret = dotrim(VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string),
				 VARDATA_ANY(set), VARSIZE_ANY_EXHDR(set),
				 false, true);

	PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * rtrim1 --- set 固定为 ' ' 的 rtrim
 *
 ********************************************************************/

Datum
rtrim1(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *ret;

	ret = dotrim(VARDATA_ANY(string), VARSIZE_ANY_EXHDR(string),
				 " ", 1,
				 false, true);

	PG_RETURN_TEXT_P(ret);
}


/********************************************************************
 *
 * translate
 *
 * 语法:
 *
 *	 text translate(text string, text from, text to)
 *
 * 目的:
 *
 *	 返回 string，将 from 中出现的所有字符替换为
 *	 to 中对应的字符。若 from 比 to 长，
 *	 则 from 中多出字符的出现处会被删除。
 *	 改进：Edwin Ramirez <ramirez@doc.mssm.edu>。
 *
 ********************************************************************/

Datum
translate(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	text	   *from = PG_GETARG_TEXT_PP(1);
	text	   *to = PG_GETARG_TEXT_PP(2);
	text	   *result;
	char	   *from_ptr,
			   *to_ptr,
			   *to_end;
	char	   *source,
			   *target;
	int			m,
				fromlen,
				tolen,
				retlen,
				i;
	int			bytelen;
	int			len;
	int			source_len;
	int			from_index;

	m = VARSIZE_ANY_EXHDR(string);
	if (m <= 0)
		PG_RETURN_TEXT_P(string);
	source = VARDATA_ANY(string);

	fromlen = VARSIZE_ANY_EXHDR(from);
	from_ptr = VARDATA_ANY(from);
	tolen = VARSIZE_ANY_EXHDR(to);
	to_ptr = VARDATA_ANY(to);
	to_end = to_ptr + tolen;

	/*
	 * 最坏情况的膨胀是：在字符串的每个位置
	 * 用一个最长字符替换一个单字节字符。
	 */
	if (unlikely(pg_mul_s32_overflow(pg_database_encoding_max_length(), m,
									 &bytelen)) ||
		unlikely(pg_add_s32_overflow(bytelen, VARHDRSZ, &bytelen)) ||
		unlikely(!AllocSizeIsValid(bytelen)))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested length too large")));

	result = (text *) palloc(bytelen);

	target = VARDATA(result);
	retlen = 0;

	while (m > 0)
	{
		source_len = pg_mblen(source);
		from_index = 0;

		for (i = 0; i < fromlen; i += len)
		{
			len = pg_mblen(&from_ptr[i]);
			if (len == source_len &&
				memcmp(source, &from_ptr[i], len) == 0)
				break;

			from_index++;
		}
		if (i < fromlen)
		{
			/* 替换，或在没有对应 "to" 字符时删除 */
			char	   *p = to_ptr;

			for (i = 0; i < from_index; i++)
			{
				if (p >= to_end)
					break;
				p += pg_mblen(p);
			}
			if (p < to_end)
			{
				len = pg_mblen(p);
				memcpy(target, p, len);
				target += len;
				retlen += len;
			}
		}
		else
		{
			/* 无匹配, 复制 */
			memcpy(target, source, source_len);
			target += source_len;
			retlen += source_len;
		}

		source += source_len;
		m -= source_len;
	}

	SET_VARSIZE(result, retlen + VARHDRSZ);

	/*
	 * 若使用多字节编码，函数结果的长度很可能远大于
	 * 实际所需，但不值得为此重新分配内存；反正结果
	 * 大概也存不了多久。
	 */

	PG_RETURN_TEXT_P(result);
}

/********************************************************************
 *
 * ascii
 *
 * 语法:
 *
 *	 int ascii(text string)
 *
 * 目的:
 *
 *	 返回 string 首字符的十进制表示。
 *
 *	 若 string 为空则返回 0。
 *	 若数据库编码为 UTF8，则返回 Unicode 码点。
 *	 若数据库编码为其他多字节编码，当首字节
 *	 为 ASCII 字符（范围 1..127）时返回其值，
 *	 否则报错。
 *	 对于所有其他编码，返回首字节的值在范围
 *	 1..255 内。
 *
 ********************************************************************/

Datum
ascii(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	int			encoding = GetDatabaseEncoding();
	unsigned char *data;

	if (VARSIZE_ANY_EXHDR(string) <= 0)
		PG_RETURN_INT32(0);

	data = (unsigned char *) VARDATA_ANY(string);

	if (encoding == PG_UTF8 && *data > 127)
	{
		/* 返回 Unicode 的码点 */

		int			result = 0,
					tbytes = 0,
					i;

		if (*data >= 0xF0)
		{
			result = *data & 0x07;
			tbytes = 3;
		}
		else if (*data >= 0xE0)
		{
			result = *data & 0x0F;
			tbytes = 2;
		}
		else
		{
			Assert(*data > 0xC0);
			result = *data & 0x1f;
			tbytes = 1;
		}

		Assert(tbytes > 0);

		for (i = 1; i <= tbytes; i++)
		{
			Assert((data[i] & 0xC0) == 0x80);
			result = (result << 6) + (data[i] & 0x3f);
		}

		PG_RETURN_INT32(result);
	}
	else
	{
		if (pg_encoding_max_length(encoding) > 1 && *data > 127)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("requested character too large")));


		PG_RETURN_INT32((int32) *data);
	}
}

/********************************************************************
 *
 * chr
 *
 * 语法:
 *
 *	 text chr(int val)
 *
 * 目的:
 *
 *	返回与 val 二进制等值的字符。
 *
 * 对 UTF8，参数视为 Unicode 码点。
 * 对其他多字节编码，参数超出严格 ASCII 范围
 * （1..127）时会报错。
 *
 * 关键在于我们绝不能返回在数据库编码中
 * 无效的值，以免它成为无效数据进入数据库
 * 的途径。
 *
 ********************************************************************/

Datum
chr			(PG_FUNCTION_ARGS)
{
	int32		arg = PG_GETARG_INT32(0);
	uint32		cvalue;
	text	   *result;
	int			encoding = GetDatabaseEncoding();

	/*
	 * 对无意义或无法在当前编码中有效表示的
	 * 参数报错。
	 */
	if (arg < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("character number must be positive")));
	else if (arg == 0)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("null character not permitted")));

	cvalue = arg;

	if (encoding == PG_UTF8 && cvalue > 127)
	{
		/* 对 Unicode，参数被视为码点 */
		int			bytes;
		unsigned char *wch;

		/*
		 * 我们只允许有效的 Unicode 码点；按 RFC3629，上界
		 * 为 U+10FFFF，尽管 4 字节 UTF8 序列可容纳到
		 * U+1FFFFF.
		 */
		if (cvalue > 0x0010ffff)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("requested character too large for encoding: %u",
							cvalue)));

		if (cvalue > 0xffff)
			bytes = 4;
		else if (cvalue > 0x07ff)
			bytes = 3;
		else
			bytes = 2;

		result = (text *) palloc(VARHDRSZ + bytes);
		SET_VARSIZE(result, VARHDRSZ + bytes);
		wch = (unsigned char *) VARDATA(result);

		if (bytes == 2)
		{
			wch[0] = 0xC0 | ((cvalue >> 6) & 0x1F);
			wch[1] = 0x80 | (cvalue & 0x3F);
		}
		else if (bytes == 3)
		{
			wch[0] = 0xE0 | ((cvalue >> 12) & 0x0F);
			wch[1] = 0x80 | ((cvalue >> 6) & 0x3F);
			wch[2] = 0x80 | (cvalue & 0x3F);
		}
		else
		{
			wch[0] = 0xF0 | ((cvalue >> 18) & 0x07);
			wch[1] = 0x80 | ((cvalue >> 12) & 0x3F);
			wch[2] = 0x80 | ((cvalue >> 6) & 0x3F);
			wch[3] = 0x80 | (cvalue & 0x3F);
		}

		/*
		 * 前面的范围检查并不充分，因为 UTF8 排除
		 * Unicode "代理对"码。需确保所构造的结果为有效
		 * UTF8.
		 */
		if (!pg_utf8_islegal(wch, bytes))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("requested character not valid for encoding: %u",
							cvalue)));
	}
	else
	{
		bool		is_mb;

		is_mb = pg_encoding_max_length(encoding) > 1;

		if ((is_mb && (cvalue > 127)) || (!is_mb && (cvalue > 255)))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("requested character too large for encoding: %u",
							cvalue)));

		result = (text *) palloc(VARHDRSZ + 1);
		SET_VARSIZE(result, VARHDRSZ + 1);
		*VARDATA(result) = (char) cvalue;
	}

	PG_RETURN_TEXT_P(result);
}

/********************************************************************
 *
 * repeat
 *
 * 语法:
 *
 *	 text repeat(text string, int val)
 *
 * 目的:
 *
 *	按 val 重复。
 *
 ********************************************************************/

Datum
repeat(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_PP(0);
	int32		count = PG_GETARG_INT32(1);
	text	   *result;
	int			slen,
				tlen;
	int			i;
	char	   *cp,
			   *sp;

	if (count < 0)
		count = 0;

	slen = VARSIZE_ANY_EXHDR(string);

	if (unlikely(pg_mul_s32_overflow(count, slen, &tlen)) ||
		unlikely(pg_add_s32_overflow(tlen, VARHDRSZ, &tlen)) ||
		unlikely(!AllocSizeIsValid(tlen)))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested length too large")));

	result = (text *) palloc(tlen);

	SET_VARSIZE(result, tlen);
	cp = VARDATA(result);
	sp = VARDATA_ANY(string);
	for (i = 0; i < count; i++)
	{
		memcpy(cp, sp, slen);
		cp += slen;
		CHECK_FOR_INTERRUPTS();
	}

	PG_RETURN_TEXT_P(result);
}
