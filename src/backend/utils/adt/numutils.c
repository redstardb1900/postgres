/*-------------------------------------------------------------------------
 *
 * numutils.c
 *	  内置数值类型 I/O 的工具函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/numutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <limits.h>
#include <ctype.h>

#include "common/int.h"
#include "utils/builtins.h"
#include "port/pg_bitutils.h"

/*
 * 包含所有两位数字的表。用于把成对的数字位
 * 复制到最终输出，加速十进制数字位生成。
 */
static const char DIGIT_TABLE[200] =
"00" "01" "02" "03" "04" "05" "06" "07" "08" "09"
"10" "11" "12" "13" "14" "15" "16" "17" "18" "19"
"20" "21" "22" "23" "24" "25" "26" "27" "28" "29"
"30" "31" "32" "33" "34" "35" "36" "37" "38" "39"
"40" "41" "42" "43" "44" "45" "46" "47" "48" "49"
"50" "51" "52" "53" "54" "55" "56" "57" "58" "59"
"60" "61" "62" "63" "64" "65" "66" "67" "68" "69"
"70" "71" "72" "73" "74" "75" "76" "77" "78" "79"
"80" "81" "82" "83" "84" "85" "86" "87" "88" "89"
"90" "91" "92" "93" "94" "95" "96" "97" "98" "99";

/*
 * 改编自 http://graphics.stanford.edu/~seander/bithacks.html#IntegerLog10
 */
static inline int
decimalLength32(const uint32 v)
{
	int			t;
	static const uint32 PowersOfTen[] = {
		1, 10, 100,
		1000, 10000, 100000,
		1000000, 10000000, 100000000,
		1000000000
	};

	/*
	 * 把以 2 为底的对数除以 10 的以 2 为底对数
	 * 的足够好近似值，得到以 10 为底的对数。
	 */
	t = (pg_leftmost_one_pos32(v) + 1) * 1233 / 4096;
	return t + (v >= PowersOfTen[t]);
}

static inline int
decimalLength64(const uint64 v)
{
	int			t;
	static const uint64 PowersOfTen[] = {
		UINT64CONST(1), UINT64CONST(10),
		UINT64CONST(100), UINT64CONST(1000),
		UINT64CONST(10000), UINT64CONST(100000),
		UINT64CONST(1000000), UINT64CONST(10000000),
		UINT64CONST(100000000), UINT64CONST(1000000000),
		UINT64CONST(10000000000), UINT64CONST(100000000000),
		UINT64CONST(1000000000000), UINT64CONST(10000000000000),
		UINT64CONST(100000000000000), UINT64CONST(1000000000000000),
		UINT64CONST(10000000000000000), UINT64CONST(100000000000000000),
		UINT64CONST(1000000000000000000), UINT64CONST(10000000000000000000)
	};

	/*
	 * 把以 2 为底的对数除以 10 的以 2 为底对数
	 * 的足够好近似值，得到以 10 为底的对数。
	 */
	t = (pg_leftmost_one_pos64(v) + 1) * 1233 / 4096;
	return t + (v >= PowersOfTen[t]);
}

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

/*
 * 将输入字符串转为有符号 16 位整数。输入字符串可
 * 以十进制/十六进制/八进制/二进制格式表示，均可
 * 带有可选的符号字符前缀，'+'（默认）或负数用的
 * '-'。十六进制串以数字带 0x 或 0X 前缀来识别，
 * 而八进制串则以 0o 或 0O 前缀识别。二进制表示则
 * 以 0b 或 0B 前缀识别。
 *
 * 允许任意数量的前导或尾随空白字符。数字位之间
 * 可以用单个下划线字符分隔。这些下划线只能
 * 出现在数字位之间，不能位于其前或其后。下划线
 * 对返回值没有影响，支持它们只是为了帮助提高
 * 输入字符串的人类可读性。
 *
 * pg_strtoint16() 在输入格式错误或溢出时将抛出 ereport()；
 * 而 pg_strtoint16_safe() 则把此类问题返回到 *escontext，
 * 若为 ErrorSaveContext。
*
 * 注意：将输入累积为无符号数，以处理最小负数的补码
 * 表示，该数无法表示为有符号正数。
 *
 */
int16
pg_strtoint16(const char *s)
{
	return pg_strtoint16_safe(s, NULL);
}

int16
pg_strtoint16_safe(const char *s, Node *escontext)
{
	const char *ptr = s;
	const char *firstdigit;
	uint16		tmp = 0;
	bool		neg = false;
	unsigned char digit;

	/*
	 * 多数情况很可能是不带下划线分隔符的十进制
	 * 数字位。我们先按此假设尝试解析字符串，
	 * 仅当快速路径版本无法解析时，才回退到能处理
	 * 十六、八、二进制及下划线实现较慢。
	 *
	 */

	/* 前导空格留给慢速路径去检查 */

	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}

	/* 前导 '+' 不常见，故留给慢速路径处理 */

	/* 处理首个数字位 */
	digit = (*ptr - '0');

	/*
	 * 利用无符号算术，省去对数字位上、下边界的
	 * 双重检查。
	 */
	if (likely(digit < 10))
	{
		ptr++;
		tmp = digit;
	}
	else
	{
		/* 至少一个数字位 */
		goto slow;
	}

	/* 处理其余数字位 */
	for (;;)
	{
		digit = (*ptr - '0');

		if (digit >= 10)
			break;

		ptr++;

		if (unlikely(tmp > -(PG_INT16_MIN / 10)))
			goto out_of_range;

		tmp = tmp * 10 + digit;
	}

	/* 字符串未以数字位结尾时，交给慢速路径处理 */
	if (unlikely(*ptr != '\0'))
		goto slow;

	if (neg)
	{
		/* 检查负数等价值能否在不溢出的情况下容纳 */
		if (unlikely(tmp > (uint16) (-(PG_INT16_MIN + 1)) + 1))
			goto out_of_range;
		return -((int16) tmp);
	}

	if (unlikely(tmp > PG_INT16_MAX))
		goto out_of_range;

	return (int16) tmp;

slow:
	tmp = 0;
	ptr = s;
	/* 无需重置 neg */

	/* 跳过前导空格 */
	while (isspace((unsigned char) *ptr))
		ptr++;

	/* 处理符号 */
	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}
	else if (*ptr == '+')
		ptr++;

	/* 处理数字 */
	if (ptr[0] == '0' && (ptr[1] == 'x' || ptr[1] == 'X'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (isxdigit((unsigned char) *ptr))
			{
				if (unlikely(tmp > -(PG_INT16_MIN / 16)))
					goto out_of_range;

				tmp = tmp * 16 + hexlookup[(unsigned char) *ptr++];
			}
			else if (*ptr == '_')
			{
				/* 下划线后必须还有更多数字位 */
				ptr++;
				if (*ptr == '\0' || !isxdigit((unsigned char) *ptr))
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (ptr[0] == '0' && (ptr[1] == 'o' || ptr[1] == 'O'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '7')
			{
				if (unlikely(tmp > -(PG_INT16_MIN / 8)))
					goto out_of_range;

				tmp = tmp * 8 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线后必须还有更多数字位 */
				ptr++;
				if (*ptr == '\0' || *ptr < '0' || *ptr > '7')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (ptr[0] == '0' && (ptr[1] == 'b' || ptr[1] == 'B'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '1')
			{
				if (unlikely(tmp > -(PG_INT16_MIN / 2)))
					goto out_of_range;

				tmp = tmp * 2 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线后必须还有更多数字位 */
				ptr++;
				if (*ptr == '\0' || *ptr < '0' || *ptr > '1')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else
	{
		firstdigit = ptr;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '9')
			{
				if (unlikely(tmp > -(PG_INT16_MIN / 10)))
					goto out_of_range;

				tmp = tmp * 10 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线不得在开头 */
				if (unlikely(ptr == firstdigit))
					goto invalid_syntax;
				/* 且其后必须还有更多数字位 */
				ptr++;
				if (*ptr == '\0' || !isdigit((unsigned char) *ptr))
					goto invalid_syntax;
			}
			else
				break;
		}
	}

	/* 至少一个数字位 */
	if (unlikely(ptr == firstdigit))
		goto invalid_syntax;

	/* 允许尾随空白，但不允许其他尾随字符 */
	while (isspace((unsigned char) *ptr))
		ptr++;

	if (unlikely(*ptr != '\0'))
		goto invalid_syntax;

	if (neg)
	{
		/* 检查负数等价值能否在不溢出的情况下容纳 */
		if (tmp > (uint16) (-(PG_INT16_MIN + 1)) + 1)
			goto out_of_range;
		return -((int16) tmp);
	}

	if (tmp > PG_INT16_MAX)
		goto out_of_range;

	return (int16) tmp;

out_of_range:
	ereturn(escontext, 0,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value \"%s\" is out of range for type %s",
					s, "smallint")));

invalid_syntax:
	ereturn(escontext, 0,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"smallint", s)));
}

/*
 * 将输入字符串转为有符号 32 位整数。输入字符串可
 * 以十进制/十六进制/八进制/二进制格式表示，均可
 * 带有可选的符号字符前缀，'+'（默认）或负数用的
 * '-'。十六进制串以数字带 0x 或 0X 前缀来识别，
 * 而八进制串则以 0o 或 0O 前缀识别。二进制表示则
 * 以 0b 或 0B 前缀识别。
 *
 * 允许任意数量的前导或尾随空白字符。数字位之间
 * 可以用单个下划线字符分隔。这些下划线只能
 * 出现在数字位之间，不能位于其前或其后。下划线
 * 对返回值没有影响，支持它们只是为了帮助提高
 * 输入字符串的人类可读性。
 *
 * pg_strtoint32() 在输入格式错误或溢出时将抛出 ereport()；
 * 而 pg_strtoint32_safe() 则把此类问题返回到 *escontext，
 * 若为 ErrorSaveContext。
 *
 * 注意：将输入累积为无符号数，以处理最小负数的补码
 * 表示，该数无法表示为有符号正数。
 *
 */
int32
pg_strtoint32(const char *s)
{
	return pg_strtoint32_safe(s, NULL);
}

int32
pg_strtoint32_safe(const char *s, Node *escontext)
{
	const char *ptr = s;
	const char *firstdigit;
	uint32		tmp = 0;
	bool		neg = false;
	unsigned char digit;

	/*
	 * 多数情况很可能是不带下划线分隔符的十进制
	 * 数字位。我们先按此假设尝试解析字符串，
	 * 仅当快速路径版本无法解析时，才回退到能处理
	 * 十六、八、二进制及下划线实现较慢。
	 *
	 */

	/* 前导空格留给慢速路径去检查 */

	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}

	/* 前导 '+' 不常见，故留给慢速路径处理 */

	/* 处理首个数字位 */
	digit = (*ptr - '0');

	/*
	 * 利用无符号算术，省去对数字位上、下边界的
	 * 双重检查。
	 */
	if (likely(digit < 10))
	{
		ptr++;
		tmp = digit;
	}
	else
	{
		/* 至少一个数字位 */
		goto slow;
	}

	/* 处理其余数字位 */
	for (;;)
	{
		digit = (*ptr - '0');

		if (digit >= 10)
			break;

		ptr++;

		if (unlikely(tmp > -(PG_INT32_MIN / 10)))
			goto out_of_range;

		tmp = tmp * 10 + digit;
	}

	/* 字符串未以数字位结尾时，交给慢速路径处理 */
	if (unlikely(*ptr != '\0'))
		goto slow;

	if (neg)
	{
		/* 检查负数等价值能否在不溢出的情况下容纳 */
		if (unlikely(tmp > (uint32) (-(PG_INT32_MIN + 1)) + 1))
			goto out_of_range;
		return -((int32) tmp);
	}

	if (unlikely(tmp > PG_INT32_MAX))
		goto out_of_range;

	return (int32) tmp;

slow:
	tmp = 0;
	ptr = s;
	/* 无需重置 neg */

	/* 跳过前导空格 */
	while (isspace((unsigned char) *ptr))
		ptr++;

	/* 处理符号 */
	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}
	else if (*ptr == '+')
		ptr++;

	/* 处理数字 */
	if (ptr[0] == '0' && (ptr[1] == 'x' || ptr[1] == 'X'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (isxdigit((unsigned char) *ptr))
			{
				if (unlikely(tmp > -(PG_INT32_MIN / 16)))
					goto out_of_range;

				tmp = tmp * 16 + hexlookup[(unsigned char) *ptr++];
			}
			else if (*ptr == '_')
			{
				/* 下划线后必须还有更多数字位 */
				ptr++;
				if (*ptr == '\0' || !isxdigit((unsigned char) *ptr))
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (ptr[0] == '0' && (ptr[1] == 'o' || ptr[1] == 'O'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '7')
			{
				if (unlikely(tmp > -(PG_INT32_MIN / 8)))
					goto out_of_range;

				tmp = tmp * 8 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线后必须还有更多数字位 */
				ptr++;
				if (*ptr == '\0' || *ptr < '0' || *ptr > '7')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (ptr[0] == '0' && (ptr[1] == 'b' || ptr[1] == 'B'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '1')
			{
				if (unlikely(tmp > -(PG_INT32_MIN / 2)))
					goto out_of_range;

				tmp = tmp * 2 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线后必须还有更多数字位 */
				ptr++;
				if (*ptr == '\0' || *ptr < '0' || *ptr > '1')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else
	{
		firstdigit = ptr;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '9')
			{
				if (unlikely(tmp > -(PG_INT32_MIN / 10)))
					goto out_of_range;

				tmp = tmp * 10 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线不得在开头 */
				if (unlikely(ptr == firstdigit))
					goto invalid_syntax;
				/* 且其后必须还有更多数字位 */
				ptr++;
				if (*ptr == '\0' || !isdigit((unsigned char) *ptr))
					goto invalid_syntax;
			}
			else
				break;
		}
	}

	/* 至少一个数字位 */
	if (unlikely(ptr == firstdigit))
		goto invalid_syntax;

	/* 允许尾随空白，但不允许其他尾随字符 */
	while (isspace((unsigned char) *ptr))
		ptr++;

	if (unlikely(*ptr != '\0'))
		goto invalid_syntax;

	if (neg)
	{
		/* 检查负数等价值能否在不溢出的情况下容纳 */
		if (tmp > (uint32) (-(PG_INT32_MIN + 1)) + 1)
			goto out_of_range;
		return -((int32) tmp);
	}

	if (tmp > PG_INT32_MAX)
		goto out_of_range;

	return (int32) tmp;

out_of_range:
	ereturn(escontext, 0,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value \"%s\" is out of range for type %s",
					s, "integer")));

invalid_syntax:
	ereturn(escontext, 0,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"integer", s)));
}

/*
 * 将输入字符串转为有符号 64 位整数。输入字符串可
 * 以十进制/十六进制/八进制/二进制格式表示，均可
 * 带有可选的符号字符前缀，'+'（默认）或负数用的
 * '-'。十六进制串以数字带 0x 或 0X 前缀来识别，
 * 而八进制串则以 0o 或 0O 前缀识别。二进制表示则
 * 以 0b 或 0B 前缀识别。
 *
 * 允许任意数量的前导或尾随空白字符。数字位之间
 * 可以用单个下划线字符分隔。这些下划线只能
 * 出现在数字位之间，不能位于其前或其后。下划线
 * 对返回值没有影响，支持它们只是为了帮助提高
 * 输入字符串的人类可读性。
 *
 * pg_strtoint64() 在输入格式错误或溢出时将抛出 ereport()；
 * 而 pg_strtoint64_safe() 则把此类问题返回到 *escontext，
 * 若为 ErrorSaveContext。
 *
 * 注意：将输入累积为无符号数，以处理最小负数的补码
 * 表示，该数无法表示为有符号正数。
 *
 */
int64
pg_strtoint64(const char *s)
{
	return pg_strtoint64_safe(s, NULL);
}

int64
pg_strtoint64_safe(const char *s, Node *escontext)
{
	const char *ptr = s;
	const char *firstdigit;
	uint64		tmp = 0;
	bool		neg = false;
	unsigned char digit;

	/*
	 * 多数情况很可能是不带下划线分隔符的十进制
	 * 数字位。我们先按此假设尝试解析字符串，
	 * 仅当快速路径版本无法解析时，才回退到能处理
	 * 十六、八、二进制及下划线实现较慢。
	 *
	 */

	/* 前导空格留给慢速路径去检查 */

	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}

	/* 前导 '+' 不常见，故留给慢速路径处理 */

	/* 处理首个数字位 */
	digit = (*ptr - '0');

	/*
	 * 利用无符号算术，省去对数字位上、下边界的
	 * 双重检查。
	 */
	if (likely(digit < 10))
	{
		ptr++;
		tmp = digit;
	}
	else
	{
		/* 至少一个数字位 */
		goto slow;
	}

	/* 处理其余数字位 */
	for (;;)
	{
		digit = (*ptr - '0');

		if (digit >= 10)
			break;

		ptr++;

		if (unlikely(tmp > -(PG_INT64_MIN / 10)))
			goto out_of_range;

		tmp = tmp * 10 + digit;
	}

	/* 字符串未以数字位结尾时，交给慢速路径处理 */
	if (unlikely(*ptr != '\0'))
		goto slow;

	if (neg)
	{
		/* 检查负数等价值能否在不溢出的情况下容纳 */
		if (unlikely(tmp > (uint64) (-(PG_INT64_MIN + 1)) + 1))
			goto out_of_range;
		return -((int64) tmp);
	}

	if (unlikely(tmp > PG_INT64_MAX))
		goto out_of_range;

	return (int64) tmp;

slow:
	tmp = 0;
	ptr = s;
	/* 无需重置 neg */

	/* 跳过前导空格 */
	while (isspace((unsigned char) *ptr))
		ptr++;

	/* 处理符号 */
	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}
	else if (*ptr == '+')
		ptr++;

	/* 处理数字 */
	if (ptr[0] == '0' && (ptr[1] == 'x' || ptr[1] == 'X'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (isxdigit((unsigned char) *ptr))
			{
				if (unlikely(tmp > -(PG_INT64_MIN / 16)))
					goto out_of_range;

				tmp = tmp * 16 + hexlookup[(unsigned char) *ptr++];
			}
			else if (*ptr == '_')
			{
				/* 下划线后必须还有更多数字位 */
				ptr++;
				if (*ptr == '\0' || !isxdigit((unsigned char) *ptr))
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (ptr[0] == '0' && (ptr[1] == 'o' || ptr[1] == 'O'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '7')
			{
				if (unlikely(tmp > -(PG_INT64_MIN / 8)))
					goto out_of_range;

				tmp = tmp * 8 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线后必须还有更多数字位 */
				ptr++;
				if (*ptr == '\0' || *ptr < '0' || *ptr > '7')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (ptr[0] == '0' && (ptr[1] == 'b' || ptr[1] == 'B'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '1')
			{
				if (unlikely(tmp > -(PG_INT64_MIN / 2)))
					goto out_of_range;

				tmp = tmp * 2 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线后必须还有更多数字位 */
				ptr++;
				if (*ptr == '\0' || *ptr < '0' || *ptr > '1')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else
	{
		firstdigit = ptr;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '9')
			{
				if (unlikely(tmp > -(PG_INT64_MIN / 10)))
					goto out_of_range;

				tmp = tmp * 10 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线不得在开头 */
				if (unlikely(ptr == firstdigit))
					goto invalid_syntax;
				/* 且其后必须还有更多数字位 */
				ptr++;
				if (*ptr == '\0' || !isdigit((unsigned char) *ptr))
					goto invalid_syntax;
			}
			else
				break;
		}
	}

	/* 至少一个数字位 */
	if (unlikely(ptr == firstdigit))
		goto invalid_syntax;

	/* 允许尾随空白，但不允许其他尾随字符 */
	while (isspace((unsigned char) *ptr))
		ptr++;

	if (unlikely(*ptr != '\0'))
		goto invalid_syntax;

	if (neg)
	{
		/* 检查负数等价值能否在不溢出的情况下容纳 */
		if (tmp > (uint64) (-(PG_INT64_MIN + 1)) + 1)
			goto out_of_range;
		return -((int64) tmp);
	}

	if (tmp > PG_INT64_MAX)
		goto out_of_range;

	return (int64) tmp;

out_of_range:
	ereturn(escontext, 0,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value \"%s\" is out of range for type %s",
					s, "bigint")));

invalid_syntax:
	ereturn(escontext, 0,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"bigint", s)));
}

/*
 * 将输入字符串转为无符号 32 位整数。
 *
 * 允许任意数量的前导或尾随空白字符。
 *
 * 若 endloc 非 NULL，则把指向字符串剩余部分的指针
 * 存到那里，以便调用者解析剩余部分。否则其后出现
 * 非空白即错误。
 *
 * typname 是错误消息中报告的名称。
 *
 * 若 escontext 指向 ErrorSaveContext 节点，则填充该节点
 * 而非抛出错误；调用者须检查以 SOFT_ERROR_OCCURRED()
 * 检测错误。
 */
uint32
uint32in_subr(const char *s, char **endloc,
			  const char *typname, Node *escontext)
{
	uint32		result;
	unsigned long cvt;
	char	   *endptr;

	errno = 0;
	cvt = strtoul(s, &endptr, 0);

	/*
	 * strtoul() 通常只置 ERANGE。在某些系统上它还可能置
	 * EINVAL，这只意味着它无法解析输入字符串。务必
	 * 以与标准错误指示（即 endptr == s）相同的方式
	 * 报告它。
	 */
	if ((errno && errno != ERANGE) || endptr == s)
		ereturn(escontext, 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						typname, s)));

	if (errno == ERANGE)
		ereturn(escontext, 0,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value \"%s\" is out of range for type %s",
						s, typname)));

	if (endloc)
	{
		/* 调用者要处理字符串剩余部分 */
		*endloc = endptr;
	}
	else
	{
		/* 数字之后只允许空白 */
		while (*endptr && isspace((unsigned char) *endptr))
			endptr++;
		if (*endptr)
			ereturn(escontext, 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							typname, s)));
	}

	result = (uint32) cvt;

	/*
	 * 应对无符号 long 宽于 uint32 的可能性，此时对于某些
	 * 超出 uint32 范围的值，strtoul 将不会报错。
	 *
	 *
	 * 出于向后兼容的考虑，要接受带负号给出的输入，
	 * 即若该值经有符号或无符号任一方式扩展到 long
	 * 后仍匹配则接受。
	 *
	 * 为保证 32 位与 64 位平台上的结果一致，务必确保
	 * 错误消息与 strtoul() 返回 ERANGE 时相同。
	 */
#if PG_UINT32_MAX != ULONG_MAX
	if (cvt != (unsigned long) result &&
		cvt != (unsigned long) ((int) result))
		ereturn(escontext, 0,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value \"%s\" is out of range for type %s",
						s, typname)));
#endif

	return result;
}

/*
 * 将输入字符串转为无符号 64 位整数。
 *
 * 允许任意数量的前导或尾随空白字符。
 *
 * 若 endloc 非 NULL，则把指向字符串剩余部分的指针
 * 存到那里，以便调用者解析剩余部分。否则其后出现
 * 非空白即错误。
 *
 * typname 是错误消息中报告的名称。
 *
 * 若 escontext 指向 ErrorSaveContext 节点，则填充该节点
 * 而非抛出错误；调用者须检查以 SOFT_ERROR_OCCURRED()
 * 检测错误。
 */
uint64
uint64in_subr(const char *s, char **endloc,
			  const char *typname, Node *escontext)
{
	uint64		result;
	char	   *endptr;

	errno = 0;
	result = strtou64(s, &endptr, 0);

	/*
	 * strtoul[l] 通常只置 ERANGE。在某些系统上还可能置
	 * EINVAL，这只意味着它无法解析输入字符串。务必
	 * 以与标准错误指示（即 endptr == s）相同的方式
	 * 报告它。
	 */
	if ((errno && errno != ERANGE) || endptr == s)
		ereturn(escontext, 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						typname, s)));

	if (errno == ERANGE)
		ereturn(escontext, 0,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value \"%s\" is out of range for type %s",
						s, typname)));

	if (endloc)
	{
		/* 调用者要处理字符串剩余部分 */
		*endloc = endptr;
	}
	else
	{
		/* 数字之后只允许空白 */
		while (*endptr && isspace((unsigned char) *endptr))
			endptr++;
		if (*endptr)
			ereturn(escontext, 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							typname, s)));
	}

	return result;
}

/*
 * pg_itoa：将有符号 16 位整数转换为它的字符串表示，
 * 并返回 strlen(a)。
 *
 * 调用者必须确保 'a' 指向足以容纳结果的内存
 * （至少 7 字节，含前导符号与结尾 NUL）。
 *
 * 似乎不值得单独实现这个函数。
 */
int
pg_itoa(int16 i, char *a)
{
	return pg_ltoa((int32) i, a);
}

/*
 * pg_ultoa_n：将无符号 32 位整数转换为它的字符串表示，
 * 不以 NUL 结尾，并返回该字符串表示的长度
 *
 * 调用者必须确保 'a' 指向足以容纳结果的内存，至少
 * 10 字节。
 */
int
pg_ultoa_n(uint32 value, char *a)
{
	int			olength,
				i = 0;

	/* 退化情形 */
	if (value == 0)
	{
		*a = '0';
		return 1;
	}

	olength = decimalLength32(value);

	/* 计算结果字符串。 */
	while (value >= 10000)
	{
		const uint32 c = value - 10000 * (value / 10000);
		const uint32 c0 = (c % 100) << 1;
		const uint32 c1 = (c / 100) << 1;

		char	   *pos = a + olength - i;

		value /= 10000;

		memcpy(pos - 2, DIGIT_TABLE + c0, 2);
		memcpy(pos - 4, DIGIT_TABLE + c1, 2);
		i += 4;
	}
	if (value >= 100)
	{
		const uint32 c = (value % 100) << 1;

		char	   *pos = a + olength - i;

		value /= 100;

		memcpy(pos - 2, DIGIT_TABLE + c, 2);
		i += 2;
	}
	if (value >= 10)
	{
		const uint32 c = value << 1;

		char	   *pos = a + olength - i;

		memcpy(pos - 2, DIGIT_TABLE + c, 2);
	}
	else
	{
		*a = (char) ('0' + value);
	}

	return olength;
}

/*
 * pg_ltoa：将有符号 32 位整数转为字符串表示并返回
 * strlen(a)。
 *
 * 调用者有责任确保 a 的长度至少为 12 字节，
 * 这一空间足以容纳一个负号、一个最长 int32 以及上述
 * 的结尾 NUL。
 */
int
pg_ltoa(int32 value, char *a)
{
	uint32		uvalue = (uint32) value;
	int			len = 0;

	if (value < 0)
	{
		uvalue = (uint32) 0 - uvalue;
		a[len++] = '-';
	}
	len += pg_ultoa_n(uvalue, a + len);
	a[len] = '\0';
	return len;
}

/*
 * 取得十进制表示（不以 NUL 结尾），并返回其长度。
 * 调用者必须确保 a 指向至少 MAXINT8LEN 字节。
 */
int
pg_ulltoa_n(uint64 value, char *a)
{
	int			olength,
				i = 0;
	uint32		value2;

	/* 退化情形 */
	if (value == 0)
	{
		*a = '0';
		return 1;
	}

	olength = decimalLength64(value);

	/* 计算结果字符串。 */
	while (value >= 100000000)
	{
		const uint64 q = value / 100000000;
		uint32		value3 = (uint32) (value - 100000000 * q);

		const uint32 c = value3 % 10000;
		const uint32 d = value3 / 10000;
		const uint32 c0 = (c % 100) << 1;
		const uint32 c1 = (c / 100) << 1;
		const uint32 d0 = (d % 100) << 1;
		const uint32 d1 = (d / 100) << 1;

		char	   *pos = a + olength - i;

		value = q;

		memcpy(pos - 2, DIGIT_TABLE + c0, 2);
		memcpy(pos - 4, DIGIT_TABLE + c1, 2);
		memcpy(pos - 6, DIGIT_TABLE + d0, 2);
		memcpy(pos - 8, DIGIT_TABLE + d1, 2);
		i += 8;
	}

	/* 切换 32 位以提速 */
	value2 = (uint32) value;

	if (value2 >= 10000)
	{
		const uint32 c = value2 - 10000 * (value2 / 10000);
		const uint32 c0 = (c % 100) << 1;
		const uint32 c1 = (c / 100) << 1;

		char	   *pos = a + olength - i;

		value2 /= 10000;

		memcpy(pos - 2, DIGIT_TABLE + c0, 2);
		memcpy(pos - 4, DIGIT_TABLE + c1, 2);
		i += 4;
	}
	if (value2 >= 100)
	{
		const uint32 c = (value2 % 100) << 1;
		char	   *pos = a + olength - i;

		value2 /= 100;

		memcpy(pos - 2, DIGIT_TABLE + c, 2);
		i += 2;
	}
	if (value2 >= 10)
	{
		const uint32 c = value2 << 1;
		char	   *pos = a + olength - i;

		memcpy(pos - 2, DIGIT_TABLE + c, 2);
	}
	else
		*a = (char) ('0' + value2);

	return olength;
}

/*
 * pg_lltoa：将有符号 64 位整数转为字符串表示并返回
 * strlen(a)。
 *
 * 调用者必须确保 'a' 指向足以容纳结果的内存
 * （至少 MAXINT8LEN + 1 字节，含前导符号与结尾 NUL）。
 */
int
pg_lltoa(int64 value, char *a)
{
	uint64		uvalue = value;
	int			len = 0;

	if (value < 0)
	{
		uvalue = (uint64) 0 - uvalue;
		a[len++] = '-';
	}

	len += pg_ulltoa_n(uvalue, a + len);
	a[len] = '\0';
	return len;
}


/*
 * pg_ultostr_zeropad
 *		将 'value' 转换为存储在 'str' 处的十进制字符串表示。
 *		'minwidth' 指定结果的最小宽度；多余的空间
 *		以数字前导零填充。
 *
 * 返回字符串结果的结束地址，即最后写入的字符
 * 加 1。注意不会写入 NUL 终止符。
 *
 * 本函数的预期用例是构建包含多个独立数字的字符串，
 * 例如：
 *
 *	str = pg_ultostr_zeropad(str, hours, 2);
 *	*str++ = ':';
 *	str = pg_ultostr_zeropad(str, mins, 2);
 *	*str++ = ':';
 *	str = pg_ultostr_zeropad(str, secs, 2);
 *	*str = '\0';
 *
 * 注意：调用者须确保 'str' 指向足以容纳结果的内存。
 *
 */
char *
pg_ultostr_zeropad(char *str, uint32 value, int32 minwidth)
{
	int			len;

	Assert(minwidth > 0);

	if (value < 100 && minwidth == 2)	/* 常见情形的捷径 */
	{
		memcpy(str, DIGIT_TABLE + value * 2, 2);
		return str + 2;
	}

	len = pg_ultoa_n(value, str);
	if (len >= minwidth)
		return str + len;

	memmove(str + minwidth - len, str, len);
	memset(str, '0', minwidth - len);
	return str + minwidth;
}

/*
 * pg_ultostr
 *		将 'value' 转换为存储在 'str' 处的十进制字符串表示。
 *
 * 返回字符串结果的结束地址，即最后写入的字符
 * 加 1。注意不会写入 NUL 终止符。
 *
 * 本函数的预期用例是构建包含多个独立数字的字符串，
 * 例如：
 *
 *	str = pg_ultostr(str, a);
 *	*str++ = ' ';
 *	str = pg_ultostr(str, b);
 *	*str = '\0';
 *
 * 注意：调用者须确保 'str' 指向足以容纳结果的内存。
 *
 */
char *
pg_ultostr(char *str, uint32 value)
{
	int			len = pg_ultoa_n(value, str);

	return str + len;
}
