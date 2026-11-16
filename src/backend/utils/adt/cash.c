/*
 * cash.c
 * Written by D'Arcy J.M. Cain
 * darcy@druid.net
 * http://www.druid.net/darcy/
 *
 * 提供按常规方式输入与输出金额、但以 64 位
 * 整数来存储和处理
 *
 * 本文件的一个略微修改版本，以及对工作原理
 * 的探讨可见于 "Software Solutions in C" 一
 * 书，作者 Dale Schumacher，Academic Press，区别是
 * 本版采用 64 位数字，故可保存的最高值达
 * $92,233,720,368,547,758.07.
 *
 * src/backend/utils/adt/cash.c
 */

#include "postgres.h"

#include <limits.h>
#include <ctype.h>
#include <math.h>

#include "common/int.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/cash.h"
#include "utils/float.h"
#include "utils/numeric.h"
#include "utils/pg_locale.h"


/*************************************************************************
 * 内部例程
 ************************************************************************/

static const char *
num_word(Cash value)
{
	static char buf[128];
	static const char *const small[] = {
		"zero", "one", "two", "three", "four", "five", "six", "seven",
		"eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
		"fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty",
		"thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"
	};
	const char *const *big = small + 18;
	int			tu = value % 100;

	/* 先处理几种简单情形 */
	if (value <= 20)
		return small[value];

	/* 是否为 100 的整数倍？ */
	if (!tu)
	{
		sprintf(buf, "%s hundred", small[value / 100]);
		return buf;
	}

	/* 超过 99？ */
	if (value > 99)
	{
		/* 是否为 10 的其他整数倍？ */
		if (value % 10 == 0 && tu > 10)
			sprintf(buf, "%s hundred %s",
					small[value / 100], big[tu / 10]);
		else if (tu < 20)
			sprintf(buf, "%s hundred and %s",
					small[value / 100], small[tu]);
		else
			sprintf(buf, "%s hundred %s %s",
					small[value / 100], big[tu / 10], small[tu % 10]);
	}
	else
	{
		/* 是否为 10 的其他整数倍？ */
		if (value % 10 == 0 && tu > 10)
			sprintf(buf, "%s", big[tu / 10]);
		else if (tu < 20)
			sprintf(buf, "%s", small[tu]);
		else
			sprintf(buf, "%s %s", big[tu / 10], small[tu % 10]);
	}

	return buf;
}								/* num_word() */

static inline Cash
cash_pl_cash(Cash c1, Cash c2)
{
	Cash		res;

	if (unlikely(pg_add_s64_overflow(c1, c2, &res)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("money out of range")));

	return res;
}

static inline Cash
cash_mi_cash(Cash c1, Cash c2)
{
	Cash		res;

	if (unlikely(pg_sub_s64_overflow(c1, c2, &res)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("money out of range")));

	return res;
}

static inline Cash
cash_mul_float8(Cash c, float8 f)
{
	float8		res = rint(float8_mul((float8) c, f));

	if (unlikely(isnan(res) || !FLOAT8_FITS_IN_INT64(res)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("money out of range")));

	return (Cash) res;
}

static inline Cash
cash_div_float8(Cash c, float8 f)
{
	float8		res = rint(float8_div((float8) c, f));

	if (unlikely(isnan(res) || !FLOAT8_FITS_IN_INT64(res)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("money out of range")));

	return (Cash) res;
}

static inline Cash
cash_mul_int64(Cash c, int64 i)
{
	Cash		res;

	if (unlikely(pg_mul_s64_overflow(c, i, &res)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("money out of range")));

	return res;
}

static inline Cash
cash_div_int64(Cash c, int64 i)
{
	if (unlikely(i == 0))
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	return c / i;
}

/* cash_in()
 * 把字符串转换为 cash 类型。
 * 格式：[$]###[,]###[.##]
 * 示例：123.45 $123.45 $123,456.78
 *
 */
Datum
cash_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	Cash		result;
	Cash		value = 0;
	Cash		dec = 0;
	Cash		sgn = 1;
	bool		seen_dot = false;
	const char *s = str;
	int			fpoint;
	char		dsymbol;
	const char *ssymbol,
			   *psymbol,
			   *nsymbol,
			   *csymbol;
	struct lconv *lconvert = PGLC_localeconv();

	/*
	 * 在某些区域设置（尤其 C）中 frac_digits 会是 CHAR_MAX。
	 * 然而，仅凭 == CHAR_MAX 判断有风险，因为 gcc 这
	 * 类编译器会"好心"允许改动 char 符号性的标准定义。
	 * 如果不幸以非标准的 -fsigned-char 或 -funsigned-char
	 * 开关编译，我们认定的 CHAR_MAX 就会与 libc 的
	 * 理解不一致。最安全的做法是根本不去测试
	 * CHAR_MAX，而是对 frac_digits 的合理取值设范围检查
	 *
	 */
	fpoint = lconvert->frac_digits;
	if (fpoint < 0 || fpoint > 10)
		fpoint = 2;				/* 此情况下算最佳猜测 */

	/* dsymbol 被限定为单字节，其他符号则无此限制 */
	if (*lconvert->mon_decimal_point != '\0' &&
		lconvert->mon_decimal_point[1] == '\0')
		dsymbol = *lconvert->mon_decimal_point;
	else
		dsymbol = '.';
	if (*lconvert->mon_thousands_sep != '\0')
		ssymbol = lconvert->mon_thousands_sep;
	else						/* ssymbol 不应等于 dsymbol */
		ssymbol = (dsymbol != ',') ? "," : ".";
	csymbol = (*lconvert->currency_symbol != '\0') ? lconvert->currency_symbol : "$";
	psymbol = (*lconvert->positive_sign != '\0') ? lconvert->positive_sign : "+";
	nsymbol = (*lconvert->negative_sign != '\0') ? lconvert->negative_sign : "-";

#ifdef CASHDEBUG
	printf("cashin- precision '%d'; decimal '%c'; thousands '%s'; currency '%s'; positive '%s'; negative '%s'\n",
		   fpoint, dsymbol, ssymbol, csymbol, psymbol, nsymbol);
#endif

	/* 这里本应加上各种各样的检查。目前只是 */
	/* 去掉所有前导空白以及前导货币符号 */
	while (isspace((unsigned char) *s))
		s++;
	if (strncmp(s, csymbol, strlen(csymbol)) == 0)
		s += strlen(csymbol);
	while (isspace((unsigned char) *s))
		s++;

#ifdef CASHDEBUG
	printf("cashin- string is '%s'\n", s);
#endif

	/* 前导负号或括号表示负数 */
	/* 同样，需要更佳启发式 */
	/* XXX - 未能正确检查括号是否配对 - djmc */
	if (strncmp(s, nsymbol, strlen(nsymbol)) == 0)
	{
		sgn = -1;
		s += strlen(nsymbol);
	}
	else if (*s == '(')
	{
		sgn = -1;
		s++;
	}
	else if (strncmp(s, psymbol, strlen(psymbol)) == 0)
		s += strlen(psymbol);

#ifdef CASHDEBUG
	printf("cashin- string is '%s'\n", s);
#endif

	/* 正负号之后也允许空白和货币符号 */
	while (isspace((unsigned char) *s))
		s++;
	if (strncmp(s, csymbol, strlen(csymbol)) == 0)
		s += strlen(csymbol);
	while (isspace((unsigned char) *s))
		s++;

#ifdef CASHDEBUG
	printf("cashin- string is '%s'\n", s);
#endif

	/*
	 * 绝对值累积在 "value" 中，最后施加符号。（符号可前
	 * 可后，否则会更复杂。）由于负整数范围
	 * 更大，我们以负数构建 "value"，再于最后翻转
	 * 符号，必要时可借此捕获最负数的溢出。
	 *
	 *
	 */

	for (; *s; s++)
	{
		/*
		 * 找满所需的小数位数之前，我们会持续寻找数字。
		 *
		 */
		if (isdigit((unsigned char) *s) && (!seen_dot || dec < fpoint))
		{
			int8		digit = *s - '0';

			if (pg_mul_s64_overflow(value, 10, &value) ||
				pg_sub_s64_overflow(value, digit, &value))
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("value \"%s\" is out of range for type %s",
								str, "money")));

			if (seen_dot)
				dec++;
		}
		/* 小数点？那就开始统计小数…… */
		else if (*s == dsymbol && !seen_dot)
		{
			seen_dot = true;
		}
		/* 为"千位"分隔符则忽略，否则结束 */
		else if (strncmp(s, ssymbol, strlen(ssymbol)) == 0)
			s += strlen(ssymbol) - 1;
		else
			break;
	}

	/* 若还有数字则进行舍入 */
	if (isdigit((unsigned char) *s) && *s >= '5')
	{
		/* 记住我们是以负数形式构建该值 */
		if (pg_sub_s64_overflow(value, 1, &value))
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value \"%s\" is out of range for type %s",
							str, "money")));
	}

	/* 为不足所需的小数位数作调整 */
	for (; dec < fpoint; dec++)
	{
		if (pg_mul_s64_overflow(value, 10, &value))
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value \"%s\" is out of range for type %s",
							str, "money")));
	}

	/*
	 * 其后应只有数字、空白、右括号、尾随符号以及
	 * 尾随货币符号
	 */
	while (isdigit((unsigned char) *s))
		s++;

	while (*s)
	{
		if (isspace((unsigned char) *s) || *s == ')')
			s++;
		else if (strncmp(s, nsymbol, strlen(nsymbol)) == 0)
		{
			sgn = -1;
			s += strlen(nsymbol);
		}
		else if (strncmp(s, psymbol, strlen(psymbol)) == 0)
			s += strlen(psymbol);
		else if (strncmp(s, csymbol, strlen(csymbol)) == 0)
			s += strlen(csymbol);
		else
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							"money", str)));
	}

	/*
	 * 如果该值本应为正数，则翻转符号，但要检查是
	 * 否为最负数。
	 */
	if (sgn > 0)
	{
		if (value == PG_INT64_MIN)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value \"%s\" is out of range for type %s",
							str, "money")));
		result = -value;
	}
	else
		result = value;

#ifdef CASHDEBUG
	printf("cashin- result is " INT64_FORMAT "\n", result);
#endif

	PG_RETURN_CASH(result);
}


/* cash_out()
 * 把 cash 转换为"元和分"表示形式的函数，采用
 * lc_monetary 区域设置格式。
 */
Datum
cash_out(PG_FUNCTION_ARGS)
{
	Cash		value = PG_GETARG_CASH(0);
	char	   *result;
	char		buf[128];
	char	   *bufptr;
	int			digit_pos;
	int			points,
				mon_group;
	char		dsymbol;
	const char *ssymbol,
			   *csymbol,
			   *signsymbol;
	char		sign_posn,
				cs_precedes,
				sep_by_space;
	struct lconv *lconvert = PGLC_localeconv();

	/* frac_digits 见 cash_in() 中的说明 */
	points = lconvert->frac_digits;
	if (points < 0 || points > 10)
		points = 2;				/* 此情况下算最佳猜测 */

	/*
	 * 与 frac_digits 一样，必须对 mon_grouping 施加范围检
	 * 查，以免被 CHAR_MAX 值愚弄。
	 */
	mon_group = *lconvert->mon_grouping;
	if (mon_group <= 0 || mon_group > 6)
		mon_group = 3;

	/* dsymbol 被限定为单字节，其他符号则无此限制 */
	if (*lconvert->mon_decimal_point != '\0' &&
		lconvert->mon_decimal_point[1] == '\0')
		dsymbol = *lconvert->mon_decimal_point;
	else
		dsymbol = '.';
	if (*lconvert->mon_thousands_sep != '\0')
		ssymbol = lconvert->mon_thousands_sep;
	else						/* ssymbol 不应等于 dsymbol */
		ssymbol = (dsymbol != ',') ? "," : ".";
	csymbol = (*lconvert->currency_symbol != '\0') ? lconvert->currency_symbol : "$";

	if (value < 0)
	{
		/* 把金额变为正数，供数字重构循环使用 */
		value = -value;
		/* 准备格式数据 */
		signsymbol = (*lconvert->negative_sign != '\0') ? lconvert->negative_sign : "-";
		sign_posn = lconvert->n_sign_posn;
		cs_precedes = lconvert->n_cs_precedes;
		sep_by_space = lconvert->n_sep_by_space;
	}
	else
	{
		signsymbol = lconvert->positive_sign;
		sign_posn = lconvert->p_sign_posn;
		cs_precedes = lconvert->p_cs_precedes;
		sep_by_space = lconvert->p_sep_by_space;
	}

	/* 在 buf[] 里自右向左构建数字、小数点与分隔符串 */
	bufptr = buf + sizeof(buf) - 1;
	*bufptr = '\0';

	/*
	 * 持续生成数字，直到无非零数字剩余、且已在小数
	 * 点左侧输出至少一位。
	 * digit_pos 为当前数字位置，0 表示紧邻小数点左侧的那
	 * 一位，向右递增。
	 */
	digit_pos = points;
	do
	{
		if (points && digit_pos == 0)
		{
			/* 插入小数点，但值不可能有小数时除外 */
			*(--bufptr) = dsymbol;
		}
		else if (digit_pos < 0 && (digit_pos % mon_group) == 0)
		{
			/* 插入千位分隔符，但仅限于小数点左侧 */
			bufptr -= strlen(ssymbol);
			memcpy(bufptr, ssymbol, strlen(ssymbol));
		}

		*(--bufptr) = ((uint64) value % 10) + '0';
		value = ((uint64) value) / 10;
		digit_pos--;
	} while (value || digit_pos >= 0);

	/*----------
	 * 现在，按正确的顺序附加货币符号与正负符号。
	 *
	 * POSIX 规范定义了控制这段代码的下列值：
	 *
	 * p/n_sign_posn:
	 *	0	用圆括号把数额与 currency_symbol 括起来。
	 *	1	正负号串位于数额与 currency_symbol 之前。
	 *	2	正负号串排在数额与 currency_symbol 之后。
	 *	3	正负号串位于 currency_symbol 之前。
	 *	4	正负号串位于 currency_symbol 之后。
	 *
	 * p/n_cs_precedes: 0 表示货币符号在数值后，否则在前。
	 *
	 * p/n_sep_by_space:
	 *	0	货币符号与数值之间没有 <space>。
	 *	1	货币符号若与正负号串相邻，<space> 将其与
	 *		数值隔开；否则 <space> 分隔货币符号与数值。
	 *
	 *	2	货币符号若紧邻正负号串，则 <space> 隔开
	 *		二者；否则 <space> 隔开正负号串与数值。
	 *
	 *----------
	 */
	switch (sign_posn)
	{
		case 0:
			if (cs_precedes)
				result = psprintf("(%s%s%s)",
								  csymbol,
								  (sep_by_space == 1) ? " " : "",
								  bufptr);
			else
				result = psprintf("(%s%s%s)",
								  bufptr,
								  (sep_by_space == 1) ? " " : "",
								  csymbol);
			break;
		case 1:
		default:
			if (cs_precedes)
				result = psprintf("%s%s%s%s%s",
								  signsymbol,
								  (sep_by_space == 2) ? " " : "",
								  csymbol,
								  (sep_by_space == 1) ? " " : "",
								  bufptr);
			else
				result = psprintf("%s%s%s%s%s",
								  signsymbol,
								  (sep_by_space == 2) ? " " : "",
								  bufptr,
								  (sep_by_space == 1) ? " " : "",
								  csymbol);
			break;
		case 2:
			if (cs_precedes)
				result = psprintf("%s%s%s%s%s",
								  csymbol,
								  (sep_by_space == 1) ? " " : "",
								  bufptr,
								  (sep_by_space == 2) ? " " : "",
								  signsymbol);
			else
				result = psprintf("%s%s%s%s%s",
								  bufptr,
								  (sep_by_space == 1) ? " " : "",
								  csymbol,
								  (sep_by_space == 2) ? " " : "",
								  signsymbol);
			break;
		case 3:
			if (cs_precedes)
				result = psprintf("%s%s%s%s%s",
								  signsymbol,
								  (sep_by_space == 2) ? " " : "",
								  csymbol,
								  (sep_by_space == 1) ? " " : "",
								  bufptr);
			else
				result = psprintf("%s%s%s%s%s",
								  bufptr,
								  (sep_by_space == 1) ? " " : "",
								  signsymbol,
								  (sep_by_space == 2) ? " " : "",
								  csymbol);
			break;
		case 4:
			if (cs_precedes)
				result = psprintf("%s%s%s%s%s",
								  csymbol,
								  (sep_by_space == 2) ? " " : "",
								  signsymbol,
								  (sep_by_space == 1) ? " " : "",
								  bufptr);
			else
				result = psprintf("%s%s%s%s%s",
								  bufptr,
								  (sep_by_space == 1) ? " " : "",
								  csymbol,
								  (sep_by_space == 2) ? " " : "",
								  signsymbol);
			break;
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		cash_recv		- 把外部二进制格式转换为 cash
 */
Datum
cash_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_CASH((Cash) pq_getmsgint64(buf));
}

/*
 *		cash_send		- 把 cash 转为二进制格式
 */
Datum
cash_send(PG_FUNCTION_ARGS)
{
	Cash		arg1 = PG_GETARG_CASH(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * 比较函数
 */

Datum
cash_eq(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_BOOL(c1 == c2);
}

Datum
cash_ne(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_BOOL(c1 != c2);
}

Datum
cash_lt(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_BOOL(c1 < c2);
}

Datum
cash_le(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_BOOL(c1 <= c2);
}

Datum
cash_gt(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_BOOL(c1 > c2);
}

Datum
cash_ge(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_BOOL(c1 >= c2);
}

Datum
cash_cmp(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	if (c1 > c2)
		PG_RETURN_INT32(1);
	else if (c1 == c2)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}


/* cash_pl()
 * cash 值相加。
 */
Datum
cash_pl(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_CASH(cash_pl_cash(c1, c2));
}


/* cash_mi()
 * cash 值相减。
 */
Datum
cash_mi(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_CASH(cash_mi_cash(c1, c2));
}


/* cash_div_cash()
 * cash 除以 cash，返回 float8。
 */
Datum
cash_div_cash(PG_FUNCTION_ARGS)
{
	Cash		dividend = PG_GETARG_CASH(0);
	Cash		divisor = PG_GETARG_CASH(1);
	float8		quotient;

	if (divisor == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	quotient = (float8) dividend / (float8) divisor;
	PG_RETURN_FLOAT8(quotient);
}


/* cash_mul_flt8()
 * cash 乘以 float8。
 */
Datum
cash_mul_flt8(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	float8		f = PG_GETARG_FLOAT8(1);

	PG_RETURN_CASH(cash_mul_float8(c, f));
}


/* flt8_mul_cash()
 * float8 乘以 cash。
 */
Datum
flt8_mul_cash(PG_FUNCTION_ARGS)
{
	float8		f = PG_GETARG_FLOAT8(0);
	Cash		c = PG_GETARG_CASH(1);

	PG_RETURN_CASH(cash_mul_float8(c, f));
}


/* cash_div_flt8()
 * cash 除以 float8。
 */
Datum
cash_div_flt8(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	float8		f = PG_GETARG_FLOAT8(1);

	PG_RETURN_CASH(cash_div_float8(c, f));
}


/* cash_mul_flt4()
 * cash 乘以 float4。
 */
Datum
cash_mul_flt4(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	float4		f = PG_GETARG_FLOAT4(1);

	PG_RETURN_CASH(cash_mul_float8(c, (float8) f));
}


/* flt4_mul_cash()
 * float4 乘以 cash。
 */
Datum
flt4_mul_cash(PG_FUNCTION_ARGS)
{
	float4		f = PG_GETARG_FLOAT4(0);
	Cash		c = PG_GETARG_CASH(1);

	PG_RETURN_CASH(cash_mul_float8(c, (float8) f));
}


/* cash_div_flt4()
 * cash 除以 float4。
 *
 */
Datum
cash_div_flt4(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	float4		f = PG_GETARG_FLOAT4(1);

	PG_RETURN_CASH(cash_div_float8(c, (float8) f));
}


/* cash_mul_int8()
 * cash 乘以 int8。
 */
Datum
cash_mul_int8(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	int64		i = PG_GETARG_INT64(1);

	PG_RETURN_CASH(cash_mul_int64(c, i));
}


/* int8_mul_cash()
 * int8 乘以 cash。
 */
Datum
int8_mul_cash(PG_FUNCTION_ARGS)
{
	int64		i = PG_GETARG_INT64(0);
	Cash		c = PG_GETARG_CASH(1);

	PG_RETURN_CASH(cash_mul_int64(c, i));
}

/* cash_div_int8()
 * cash 除以 8 字节整数。
 */
Datum
cash_div_int8(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	int64		i = PG_GETARG_INT64(1);

	PG_RETURN_CASH(cash_div_int64(c, i));
}


/* cash_mul_int4()
 * cash 乘以 int4。
 */
Datum
cash_mul_int4(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	int32		i = PG_GETARG_INT32(1);

	PG_RETURN_CASH(cash_mul_int64(c, (int64) i));
}


/* int4_mul_cash()
 * int4 乘以 cash。
 */
Datum
int4_mul_cash(PG_FUNCTION_ARGS)
{
	int32		i = PG_GETARG_INT32(0);
	Cash		c = PG_GETARG_CASH(1);

	PG_RETURN_CASH(cash_mul_int64(c, (int64) i));
}


/* cash_div_int4()
 * cash 除以 4 字节整数。
 *
 */
Datum
cash_div_int4(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	int32		i = PG_GETARG_INT32(1);

	PG_RETURN_CASH(cash_div_int64(c, (int64) i));
}


/* cash_mul_int2()
 * cash 乘以 int2。
 */
Datum
cash_mul_int2(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	int16		s = PG_GETARG_INT16(1);

	PG_RETURN_CASH(cash_mul_int64(c, (int64) s));
}

/* int2_mul_cash()
 * int2 乘以 cash。
 */
Datum
int2_mul_cash(PG_FUNCTION_ARGS)
{
	int16		s = PG_GETARG_INT16(0);
	Cash		c = PG_GETARG_CASH(1);

	PG_RETURN_CASH(cash_mul_int64(c, (int64) s));
}

/* cash_div_int2()
 * cash 除以 int2。
 *
 */
Datum
cash_div_int2(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	int16		s = PG_GETARG_INT16(1);

	PG_RETURN_CASH(cash_div_int64(c, (int64) s));
}

/* cashlarger()
 * 返回两 cash 值中较大者。
 */
Datum
cashlarger(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);
	Cash		result;

	result = (c1 > c2) ? c1 : c2;

	PG_RETURN_CASH(result);
}

/* cashsmaller()
 * 返回两 cash 值中较小者。
 */
Datum
cashsmaller(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);
	Cash		result;

	result = (c1 < c2) ? c1 : c2;

	PG_RETURN_CASH(result);
}

/* cash_words()
 * 这也会把 int4 转换为用单词表示的形式。
 * 显然非常偏重北美习惯——抱歉
 */
Datum
cash_words(PG_FUNCTION_ARGS)
{
	Cash		value = PG_GETARG_CASH(0);
	uint64		val;
	char		buf[256];
	char	   *p = buf;
	Cash		m0;
	Cash		m1;
	Cash		m2;
	Cash		m3;
	Cash		m4;
	Cash		m5;
	Cash		m6;

	/* 处理正数 */
	if (value < 0)
	{
		value = -value;
		strcpy(buf, "minus ");
		p += 6;
	}
	else
		buf[0] = '\0';

	/* 按无符号处理，避免 INT_MIN 处出问题 */
	val = (uint64) value;

	m0 = val % INT64CONST(100); /* 分 */
	m1 = (val / INT64CONST(100)) % 1000;	/* 百位 */
	m2 = (val / INT64CONST(100000)) % 1000; /* 千位 */
	m3 = (val / INT64CONST(100000000)) % 1000;	/* 百万 */
	m4 = (val / INT64CONST(100000000000)) % 1000;	/* 十亿 */
	m5 = (val / INT64CONST(100000000000000)) % 1000;	/* 万亿 */
	m6 = (val / INT64CONST(100000000000000000)) % 1000; /* 千万亿 */

	if (m6)
	{
		strcat(buf, num_word(m6));
		strcat(buf, " quadrillion ");
	}

	if (m5)
	{
		strcat(buf, num_word(m5));
		strcat(buf, " trillion ");
	}

	if (m4)
	{
		strcat(buf, num_word(m4));
		strcat(buf, " billion ");
	}

	if (m3)
	{
		strcat(buf, num_word(m3));
		strcat(buf, " million ");
	}

	if (m2)
	{
		strcat(buf, num_word(m2));
		strcat(buf, " thousand ");
	}

	if (m1)
		strcat(buf, num_word(m1));

	if (!*p)
		strcat(buf, "zero");

	strcat(buf, (val / 100) == 1 ? " dollar and " : " dollars and ");
	strcat(buf, num_word(m0));
	strcat(buf, m0 == 1 ? " cent" : " cents");

	/* 首字母大写 */
	buf[0] = pg_toupper((unsigned char) buf[0]);

	/* 返回 text datum */
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}


/* cash_numeric()
 * cash 转为 numeric。
 */
Datum
cash_numeric(PG_FUNCTION_ARGS)
{
	Cash		money = PG_GETARG_CASH(0);
	Datum		result;
	int			fpoint;
	struct lconv *lconvert = PGLC_localeconv();

	/* frac_digits 见 cash_in() 中的说明 */
	fpoint = lconvert->frac_digits;
	if (fpoint < 0 || fpoint > 10)
		fpoint = 2;

	/* 把整数的货币值转换为 numeric */
	result = NumericGetDatum(int64_to_numeric(money));

	/* 必要时进行适当的缩放 */
	if (fpoint > 0)
	{
		int64		scale;
		int			i;
		Datum		numeric_scale;
		Datum		quotient;

		/* 计算所需的缩放因子 */
		scale = 1;
		for (i = 0; i < fpoint; i++)
			scale *= 10;
		numeric_scale = NumericGetDatum(int64_to_numeric(scale));

		/*
		 * 当整数输入接近 INT64_MAX 时，select_div_scale() 可
		 * 能选零刻度，使商丢小数位。要结果精确，
		 * 可将任一输入的 dscale 设为不小于期望的结果刻
		 * 度。numeric_round() 会替我们做好这件事。
		 *
		 */
		numeric_scale = DirectFunctionCall2(numeric_round,
											numeric_scale,
											Int32GetDatum(fpoint));

		/* 现在可安全相除... */
		quotient = DirectFunctionCall2(numeric_div, result, numeric_scale);

		/* 然后强制舍入到精确的目标位数 */
		result = DirectFunctionCall2(numeric_round,
									 quotient,
									 Int32GetDatum(fpoint));
	}

	PG_RETURN_DATUM(result);
}

/* numeric_cash()
 * numeric 转为 cash。
 */
Datum
numeric_cash(PG_FUNCTION_ARGS)
{
	Datum		amount = PG_GETARG_DATUM(0);
	Cash		result;
	int			fpoint;
	int64		scale;
	int			i;
	Datum		numeric_scale;
	struct lconv *lconvert = PGLC_localeconv();

	/* frac_digits 见 cash_in() 中的说明 */
	fpoint = lconvert->frac_digits;
	if (fpoint < 0 || fpoint > 10)
		fpoint = 2;

	/* 计算所需的缩放因子 */
	scale = 1;
	for (i = 0; i < fpoint; i++)
		scale *= 10;

	/* 把输入金额乘以缩放因子 */
	numeric_scale = NumericGetDatum(int64_to_numeric(scale));
	amount = DirectFunctionCall2(numeric_mul, amount, numeric_scale);

	/* 注意 numeric_int8 会为我们舍入到最接近的整数 */
	result = DatumGetInt64(DirectFunctionCall1(numeric_int8, amount));

	PG_RETURN_CASH(result);
}

/* int4_cash()
 * int4 (int) 转为 cash
 */
Datum
int4_cash(PG_FUNCTION_ARGS)
{
	int32		amount = PG_GETARG_INT32(0);
	Cash		result;
	int			fpoint;
	int64		scale;
	int			i;
	struct lconv *lconvert = PGLC_localeconv();

	/* frac_digits 见 cash_in() 中的说明 */
	fpoint = lconvert->frac_digits;
	if (fpoint < 0 || fpoint > 10)
		fpoint = 2;

	/* 计算所需的缩放因子 */
	scale = 1;
	for (i = 0; i < fpoint; i++)
		scale *= 10;

	/* 计算 amount * scale，并检查溢出 */
	result = DatumGetInt64(DirectFunctionCall2(int8mul, Int64GetDatum(amount),
											   Int64GetDatum(scale)));

	PG_RETURN_CASH(result);
}

/* int8_cash()
 * int8 (bigint) 转为 cash
 */
Datum
int8_cash(PG_FUNCTION_ARGS)
{
	int64		amount = PG_GETARG_INT64(0);
	Cash		result;
	int			fpoint;
	int64		scale;
	int			i;
	struct lconv *lconvert = PGLC_localeconv();

	/* frac_digits 见 cash_in() 中的说明 */
	fpoint = lconvert->frac_digits;
	if (fpoint < 0 || fpoint > 10)
		fpoint = 2;

	/* 计算所需的缩放因子 */
	scale = 1;
	for (i = 0; i < fpoint; i++)
		scale *= 10;

	/* 计算 amount * scale，并检查溢出 */
	result = DatumGetInt64(DirectFunctionCall2(int8mul, Int64GetDatum(amount),
											   Int64GetDatum(scale)));

	PG_RETURN_CASH(result);
}
