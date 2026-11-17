/*-------------------------------------------------------------------------
 *
 * float.c
 *	  内置浮点数类型的相关函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/float.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <limits.h>

#include "catalog/pg_type.h"
#include "common/int.h"
#include "common/pg_prng.h"
#include "common/shortest_dec.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/sortsupport.h"
#include "utils/timestamp.h"


/*
 * 可配置的 GUC 参数
 *
 * 若 >0，输出采用最短十进制格式；这既是默认行为，也
 * 使显式在此处设置值的客户端保持兼容，获得往返
 * 精确的结果。若为 0 或更小，则使用旧的、缓慢的
 * 十进制舍入方法。
 */
int			extra_float_digits = 1;

/* 基于角度的三角函数的缓存常量 */
static bool degree_consts_set = false;
static float8 sin_30 = 0;
static float8 one_minus_cos_60 = 0;
static float8 asin_0_5 = 0;
static float8 acos_0_5 = 0;
static float8 atan_1_0 = 0;
static float8 tan_45 = 0;
static float8 cot_45 = 0;

/*
 * 这些变量刻意不用 static；不要"修复"它们。它们永远
 * 不会被其他文件引用，更不会被改动；但我们不想让
 * 编译器知道，否则它可能预计算涉及它们的表达
 * 式。参见 init_degree_constants() 的注释。
 */
float8		degree_c_thirty = 30.0;
float8		degree_c_forty_five = 45.0;
float8		degree_c_sixty = 60.0;
float8		degree_c_one_half = 0.5;
float8		degree_c_one = 1.0;

/* drandom()/setseed() 的状态 */
static bool drandom_seed_set = false;
static pg_prng_state drandom_seed;

/* 局部函数声明 */
static double sind_q1(double x);
static double cosd_q1(double x);
static void init_degree_constants(void);


/*
 * 我们用这些行外 ereport() 调用报告浮点溢出、下溢
 * 和除零，因为按惯例在每个调用点重复它们会产生
 * 大量代码膨胀。
 *
 * 这确实意味着你得不到有用的错误位置指示。
 */
pg_noinline void
float_overflow_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value out of range: overflow")));
}

pg_noinline void
float_underflow_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value out of range: underflow")));
}

pg_noinline void
float_zero_divide_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_DIVISION_BY_ZERO),
			 errmsg("division by zero")));
}


/*
 * 若 'val' 表示负无穷，返回 -1；若 'val' 表示
 * （正）无穷，则返回 1；否则返回 0。有些平台上
 * 它等价于 isinf() 宏，但并非处处如此：C99 并未
 * 规定 isinf() 必须区分正无穷与
 * 负无穷。
 */
int
is_infinite(double val)
{
	int			inf = isinf(val);

	if (inf == 0)
		return 0;
	else if (val > 0)
		return 1;
	else
		return -1;
}


/* ========== 用户 I/O 例程 ========== */


/*
 *		float4in		- 把 "num" 转成 float4
 *
 * 注意此代码现在使用 strtof()，以前用的是 strtod()。
 *
 * 使用 strtof() 的动机是避免二次舍入问题：
 * 对某些十进制输入，若先把输入正确舍入为 double，
 * 再将该 double 舍入为 float，其结果就不正确——与把
 * 十进制值直接舍入为 float 的结果不相符。
 *
 * 最佳例证之一是 7.038531e-26：
 *
 * 0xAE43FDp-107 = 7.03853069185120912085...e-26
 *      midpoint   7.03853100000000022281...e-26
 * 0xAE43FEp-107 = 7.03853130814879132477...e-26
 *
 * 这使得 0xAE43FDp-107 成为正确的 float 结果；但若经由 double
 * 转换则得到
 *
 * 0xAE43FD.7FFFFFF8p-107 = 7.03853099999999907487...e-26
 *               midpoint   7.03853099999999964884...e-26
 * 0xAE43FD.80000000p-107 = 7.03853100000000022281...e-26
 * 0xAE43FD.80000008p-107 = 7.03853100000000137076...e-26
 *
 * 因此该值恰好舍入到两个相邻 float 的中点上（以 double
 * 表示），然后再舍入为 float 就会得到错误的结果
 * 0xAE43FEp-107。
 *
 */
Datum
float4in(PG_FUNCTION_ARGS)
{
	char	   *num = PG_GETARG_CSTRING(0);

	PG_RETURN_FLOAT4(float4in_internal(num, NULL, "real", num,
									   fcinfo->context));
}

/*
 * float4in_internal - float4in() 内部
 *
 * 对外暴露本函数，供希望以较独立于平台
 * 的方式输入浮点数的函数使用。其行为本质上
 * 如同 strtof + 出错时 ereturn。
 *
 * 它使用与下方 float8in_internal 相同的 API，其注
 * 释也适用于此处，但涉及几何类型的使用除外。
 */
float4
float4in_internal(char *num, char **endptr_p,
				  const char *type_name, const char *orig_string,
				  struct Node *escontext)
{
	float		val;
	char	   *endptr;

	/*
	 * endptr 指向被我们识别为有效浮点数的序列之后的第
	 * 一个字符。orig_string 指向最初的输入
	 * 字符串。
	 */

	/* 跳过前导空白 */
	while (*num != '\0' && isspace((unsigned char) *num))
		num++;

	/*
	 * 首先检查空字符串输入，以避开 strtod() 在不同平台
	 * 的怪异行为。
	 */
	if (*num == '\0')
		ereturn(escontext, 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						type_name, orig_string)));

	errno = 0;
	val = strtof(num, &endptr);

	/* 是否没看到任何形似 double 的内容? */
	if (endptr == num || errno != 0)
	{
		int			save_errno = errno;

		/*
		 * C99 要求 strtof() 接受 NaN、[+-]Infinity、[+-]Inf，
		 * 但并非所有平台都支持它们（有的接受后却仍设
		 * 置 ERANGE……）。因此若 strtof() 失败，我们会
		 * 自行检查这些输入。
		 *
		 * 注意：C99 还要求十六进制输入以及一些扩展形式
		 * 的 NaN，但我们认为这些形式不可移植，不打算支
		 * 持它们。若你的 strtof() 接受，那尽管用好了。
		 */
		if (pg_strncasecmp(num, "NaN", 3) == 0)
		{
			val = get_float4_nan();
			endptr = num + 3;
		}
		else if (pg_strncasecmp(num, "Infinity", 8) == 0)
		{
			val = get_float4_infinity();
			endptr = num + 8;
		}
		else if (pg_strncasecmp(num, "+Infinity", 9) == 0)
		{
			val = get_float4_infinity();
			endptr = num + 9;
		}
		else if (pg_strncasecmp(num, "-Infinity", 9) == 0)
		{
			val = -get_float4_infinity();
			endptr = num + 9;
		}
		else if (pg_strncasecmp(num, "inf", 3) == 0)
		{
			val = get_float4_infinity();
			endptr = num + 3;
		}
		else if (pg_strncasecmp(num, "+inf", 4) == 0)
		{
			val = get_float4_infinity();
			endptr = num + 4;
		}
		else if (pg_strncasecmp(num, "-inf", 4) == 0)
		{
			val = -get_float4_infinity();
			endptr = num + 4;
		}
		else if (save_errno == ERANGE)
		{
			/*
			 * 有些平台会对非规范化数（非零、但太接近零
			 * 而不具备完整精度）返回 ERANGE。我们不愿
			 * 为此抛出错误，因此尝试通过检查结果来判断：
			 * 它是否是"真实"的越界情形，也就是结果是否
			 * 为零或巨大。
			 */
			if (val == 0.0 ||
#if !defined(HUGE_VALF)
				isinf(val)
#else
				(val >= HUGE_VALF || val <= -HUGE_VALF)
#endif
				)
			{
				/* 理由见 float8in_internal 中注释 */
				char	   *errnumber = pstrdup(num);

				errnumber[endptr - num] = '\0';

				ereturn(escontext, 0,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("\"%s\" is out of range for type real",
								errnumber)));
			}
		}
		else
			ereturn(escontext, 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							type_name, orig_string)));
	}

	/* 跳过尾部空白 */
	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;

	/* 需要时报告停止点，否则若未到末尾则报错 */
	if (endptr_p)
		*endptr_p = endptr;
	else if (*endptr != '\0')
		ereturn(escontext, 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						type_name, orig_string)));

	return val;
}

/*
 *		float4out		- 把 float4 数转成字符串，
 *						  采用标准输出格式
 */
Datum
float4out(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);
	char	   *ascii = (char *) palloc(32);
	int			ndig = FLT_DIG + extra_float_digits;

	if (extra_float_digits > 0)
	{
		float_to_shortest_decimal_buf(num, ascii);
		PG_RETURN_CSTRING(ascii);
	}

	(void) pg_strfromd(ascii, 32, ndig, num);
	PG_RETURN_CSTRING(ascii);
}

/*
 *		float4recv			- 将外部二进制格式转成 float4
 */
Datum
float4recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_FLOAT4(pq_getmsgfloat4(buf));
}

/*
 *		float4send			- 把 float4 转成二进制格式
 */
Datum
float4send(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendfloat4(&buf, num);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 *		float8in		- 把 "num" 转成 float8
 */
Datum
float8in(PG_FUNCTION_ARGS)
{
	char	   *num = PG_GETARG_CSTRING(0);

	PG_RETURN_FLOAT8(float8in_internal(num, NULL, "double precision", num,
									   fcinfo->context));
}

/*
 * float8in_internal - float8in() 内部
 *
 * 对外暴露本函数，供希望以较独立于平台
 * 方式输入 double。其行为本质上类似于
 * strtod + 出错时 ereturn，但请注意以
 * 下差异：
 * 1. 前导与尾部空白均会被跳过。
 * 2. 若 endptr_p 为 NULL，则存在尾部垃圾时报错；
 * 否则由调用者负责处理尾部垃圾的投诉。
 * 3. 出现语法错误时，报告会提及给定的 type_name，
 * 并把 orig_string 作为输入打印；这是为了支持把本函
 * 数用于 "box"、"point" 这类类型——此时我们在此解析
 * 的只是 orig_string 的一个子串。
 *
 * 如果 escontext 指向 ErrorSaveContext 节点，则改为填
 * 充它而不抛错；调用者需检查 SOFT_ERROR_OCCURRED()
 * 检测错误。
 *
 * "num" 本可声明为 "const char *"，但这会在本函数
 * 和调用者中带来不合理的大量额外转换，故没这么做。
 */
float8
float8in_internal(char *num, char **endptr_p,
				  const char *type_name, const char *orig_string,
				  struct Node *escontext)
{
	double		val;
	char	   *endptr;

	/* 跳过前导空白 */
	while (*num != '\0' && isspace((unsigned char) *num))
		num++;

	/*
	 * 首先检查空字符串输入，以避开 strtod() 在不同平台
	 * 的怪异行为。
	 */
	if (*num == '\0')
		ereturn(escontext, 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						type_name, orig_string)));

	errno = 0;
	val = strtod(num, &endptr);

	/* 是否没看到任何形似 double 的内容? */
	if (endptr == num || errno != 0)
	{
		int			save_errno = errno;

		/*
		 * C99 要求 strtod() 接受 NaN、[+-]Infinity、[+-]Inf，
		 * 但并非所有平台都支持它们（有的接受后却仍设
		 * 置 ERANGE……）。因此若 strtod() 失败，我们会
		 * 自行检查这些输入。
		 *
		 * 注意：C99 还要求十六进制输入以及一些扩展形式
		 * 的 NaN，但我们认为这些形式不可移植，不打算支
		 * 持它们。若你的 strtod() 接受，那尽管用好了。
		 */
		if (pg_strncasecmp(num, "NaN", 3) == 0)
		{
			val = get_float8_nan();
			endptr = num + 3;
		}
		else if (pg_strncasecmp(num, "Infinity", 8) == 0)
		{
			val = get_float8_infinity();
			endptr = num + 8;
		}
		else if (pg_strncasecmp(num, "+Infinity", 9) == 0)
		{
			val = get_float8_infinity();
			endptr = num + 9;
		}
		else if (pg_strncasecmp(num, "-Infinity", 9) == 0)
		{
			val = -get_float8_infinity();
			endptr = num + 9;
		}
		else if (pg_strncasecmp(num, "inf", 3) == 0)
		{
			val = get_float8_infinity();
			endptr = num + 3;
		}
		else if (pg_strncasecmp(num, "+inf", 4) == 0)
		{
			val = get_float8_infinity();
			endptr = num + 4;
		}
		else if (pg_strncasecmp(num, "-inf", 4) == 0)
		{
			val = -get_float8_infinity();
			endptr = num + 4;
		}
		else if (save_errno == ERANGE)
		{
			/*
			 * 有些平台会对非规范化数（非零、但太接近零
			 * 而不具备完整精度）返回 ERANGE。我们不愿
			 * 为此抛出错误，因此尝试通过检查结果来判断：
			 * 它是否是"真实"的越界情形，也就是结果是否
			 * 为零或巨大。
			 *
			 * 出错时，我们刻意报告 double 精度而非给定的
			 * 类型名，且只打印字符串中作当前数字的那一
			 * 部分。
			 */
			if (val == 0.0 || val >= HUGE_VAL || val <= -HUGE_VAL)
			{
				char	   *errnumber = pstrdup(num);

				errnumber[endptr - num] = '\0';
				ereturn(escontext, 0,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("\"%s\" is out of range for type double precision",
								errnumber)));
			}
		}
		else
			ereturn(escontext, 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							type_name, orig_string)));
	}

	/* 跳过尾部空白 */
	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;

	/* 需要时报告停止点，否则若未到末尾则报错 */
	if (endptr_p)
		*endptr_p = endptr;
	else if (*endptr != '\0')
		ereturn(escontext, 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						type_name, orig_string)));

	return val;
}


/*
 *		float8out		- 把 float8 数转成字符串，
 *						  采用标准输出格式
 */
Datum
float8out(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);

	PG_RETURN_CSTRING(float8out_internal(num));
}

/*
 * float8out_internal - float8out() 内部
 *
 * 对外暴露本函数，供希望以较独立于平台
 * 方式输出 double。
 * 结果总是 palloc 的。
 */
char *
float8out_internal(double num)
{
	char	   *ascii = (char *) palloc(32);
	int			ndig = DBL_DIG + extra_float_digits;

	if (extra_float_digits > 0)
	{
		double_to_shortest_decimal_buf(num, ascii);
		return ascii;
	}

	(void) pg_strfromd(ascii, 32, ndig, num);
	return ascii;
}

/*
 *		float8recv			- 将外部二进制格式转成 float8
 */
Datum
float8recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_FLOAT8(pq_getmsgfloat8(buf));
}

/*
 *		float8send			- 把 float8 转成二进制格式
 */
Datum
float8send(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendfloat8(&buf, num);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/* ========== 公开例程 ========== */


/*
 *		======================
 *		FLOAT4 基本运算
 *		======================
 */

/*
 *		float4abs		- 返回 |arg1| (绝对值)
 */
Datum
float4abs(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);

	PG_RETURN_FLOAT4(fabsf(arg1));
}

/*
 *		float4um		- 返回 -arg1 (一元负号)
 */
Datum
float4um(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		result;

	result = -arg1;
	PG_RETURN_FLOAT4(result);
}

Datum
float4up(PG_FUNCTION_ARGS)
{
	float4		arg = PG_GETARG_FLOAT4(0);

	PG_RETURN_FLOAT4(arg);
}

Datum
float4larger(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);
	float4		result;

	if (float4_gt(arg1, arg2))
		result = arg1;
	else
		result = arg2;
	PG_RETURN_FLOAT4(result);
}

Datum
float4smaller(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);
	float4		result;

	if (float4_lt(arg1, arg2))
		result = arg1;
	else
		result = arg2;
	PG_RETURN_FLOAT4(result);
}

/*
 *		======================
 *		FLOAT8 基本运算
 *		======================
 */

/*
 *		float8abs		- 返回 |arg1| (绝对值)
 */
Datum
float8abs(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(fabs(arg1));
}


/*
 *		float8um		- 返回 -arg1 (一元负号)
 */
Datum
float8um(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	result = -arg1;
	PG_RETURN_FLOAT8(result);
}

Datum
float8up(PG_FUNCTION_ARGS)
{
	float8		arg = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(arg);
}

Datum
float8larger(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	if (float8_gt(arg1, arg2))
		result = arg1;
	else
		result = arg2;
	PG_RETURN_FLOAT8(result);
}

Datum
float8smaller(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	if (float8_lt(arg1, arg2))
		result = arg1;
	else
		result = arg2;
	PG_RETURN_FLOAT8(result);
}


/*
 *		====================
 *		算术算子
 *		====================
 */

/*
 *		float4pl		- 返回 arg1 + arg2
 *		float4mi		- 返回 arg1 - arg2
 *		float4mul		- 返回 arg1 * arg2
 *		float4div		- 返回 arg1 / arg2
 */
Datum
float4pl(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT4(float4_pl(arg1, arg2));
}

Datum
float4mi(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT4(float4_mi(arg1, arg2));
}

Datum
float4mul(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT4(float4_mul(arg1, arg2));
}

Datum
float4div(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT4(float4_div(arg1, arg2));
}

/*
 *		float8pl		- 返回 arg1 + arg2
 *		float8mi		- 返回 arg1 - arg2
 *		float8mul		- 返回 arg1 * arg2
 *		float8div		- 返回 arg1 / arg2
 */
Datum
float8pl(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_pl(arg1, arg2));
}

Datum
float8mi(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_mi(arg1, arg2));
}

Datum
float8mul(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_mul(arg1, arg2));
}

Datum
float8div(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_div(arg1, arg2));
}


/*
 *		====================
 *		比较算子
 *		====================
 */

/*
 *		float4{eq,ne,lt,le,gt,ge}		- float4/float4 比较运算
 */
int
float4_cmp_internal(float4 a, float4 b)
{
	if (float4_gt(a, b))
		return 1;
	if (float4_lt(a, b))
		return -1;
	return 0;
}

Datum
float4eq(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_eq(arg1, arg2));
}

Datum
float4ne(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_ne(arg1, arg2));
}

Datum
float4lt(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_lt(arg1, arg2));
}

Datum
float4le(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_le(arg1, arg2));
}

Datum
float4gt(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_gt(arg1, arg2));
}

Datum
float4ge(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float4_ge(arg1, arg2));
}

Datum
btfloat4cmp(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_INT32(float4_cmp_internal(arg1, arg2));
}

static int
btfloat4fastcmp(Datum x, Datum y, SortSupport ssup)
{
	float4		arg1 = DatumGetFloat4(x);
	float4		arg2 = DatumGetFloat4(y);

	return float4_cmp_internal(arg1, arg2);
}

Datum
btfloat4sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = btfloat4fastcmp;
	PG_RETURN_VOID();
}

/*
 *		float8{eq,ne,lt,le,gt,ge}		- float8/float8 比较运算
 */
int
float8_cmp_internal(float8 a, float8 b)
{
	if (float8_gt(a, b))
		return 1;
	if (float8_lt(a, b))
		return -1;
	return 0;
}

Datum
float8eq(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_eq(arg1, arg2));
}

Datum
float8ne(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_ne(arg1, arg2));
}

Datum
float8lt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_lt(arg1, arg2));
}

Datum
float8le(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_le(arg1, arg2));
}

Datum
float8gt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_gt(arg1, arg2));
}

Datum
float8ge(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_ge(arg1, arg2));
}

Datum
btfloat8cmp(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_INT32(float8_cmp_internal(arg1, arg2));
}

static int
btfloat8fastcmp(Datum x, Datum y, SortSupport ssup)
{
	float8		arg1 = DatumGetFloat8(x);
	float8		arg2 = DatumGetFloat8(y);

	return float8_cmp_internal(arg1, arg2);
}

Datum
btfloat8sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = btfloat8fastcmp;
	PG_RETURN_VOID();
}

Datum
btfloat48cmp(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	/* 先展宽 float4 为 float8 再比较 */
	PG_RETURN_INT32(float8_cmp_internal(arg1, arg2));
}

Datum
btfloat84cmp(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	/* 先展宽 float4 为 float8 再比较 */
	PG_RETURN_INT32(float8_cmp_internal(arg1, arg2));
}

/*
 * float8 的 in_range 支持函数。
 *
 * 注意：我们无需提供 float8_float4 变体，因为偏移值的
 * 隐式类型转换同样能处理该场景。
 */
Datum
in_range_float8_float8(PG_FUNCTION_ARGS)
{
	float8		val = PG_GETARG_FLOAT8(0);
	float8		base = PG_GETARG_FLOAT8(1);
	float8		offset = PG_GETARG_FLOAT8(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	float8		sum;

	/*
	 * 拒绝负数或 NaN 偏移。负数是规范要求，而 NaN 则
	 * 是因为针对它的合适语义似乎并不明显。
	 */
	if (isnan(offset) || offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	/*
	 * 处理 val 和/或 base 为 NaN 的情形，遵循 NaN 排在非
	 * NaN 之后的规则（参见 float8_cmp_internal）。偏移量
	 * 无法影响结论。
	 */
	if (isnan(val))
	{
		if (isnan(base))
			PG_RETURN_BOOL(true);	/* NAN = NAN */
		else
			PG_RETURN_BOOL(!less);	/* NAN > non-NAN */
	}
	else if (isnan(base))
	{
		PG_RETURN_BOOL(less);	/* non-NAN < NAN */
	}

	/*
	 * 处理 base 与 offset 均为无穷，且计算 base +/- offset 会产
	 * 生 NaN 的情形。这对应于边界无限前于 +inf 或无限
	 * 后于 -inf 的窗口帧，这并非良定义。为与其他涉及
	 * 无穷的情形保持一致——例如 +inf 无限后于 +inf，我
	 * 们选择假定 +inf 无限前于 +inf，且 -inf 无限后于
	 * -inf，从而所有有限值与无穷值都处于这样
	 * 的窗口
	 * 帧中。
	 *
	 * offset 已知为正，因此在该测试中只需检查 base 的
	 * 符号。
	 */
	if (isinf(offset) && isinf(base) &&
		(sub ? base > 0 : base < 0))
		PG_RETURN_BOOL(true);

	/*
	 * 否则计算 base +/- offset 应当是安全的。我们相信
	 * FPU 能应付输入为 +/-inf 或真实和会溢出的情况，并
	 * 产生带有适当符号的无穷大，无论 val 是否为无穷，
	 * 它都能正确参与比较。
	 */
	if (sub)
		sum = base - offset;
	else
		sum = base + offset;

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}

/*
 * float4 的 in_range 支持函数。
 *
 * 无论如何我们都需要一个 float4_float8 变体，因此提
 * 供它，让隐式转换处理 float4_float4 的情形。
 */
Datum
in_range_float4_float8(PG_FUNCTION_ARGS)
{
	float4		val = PG_GETARG_FLOAT4(0);
	float4		base = PG_GETARG_FLOAT4(1);
	float8		offset = PG_GETARG_FLOAT8(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	float8		sum;

	/*
	 * 拒绝负数或 NaN 偏移。负数是规范要求，而 NaN 则
	 * 是因为针对它的合适语义似乎并不明显。
	 */
	if (isnan(offset) || offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	/*
	 * 处理 val 和/或 base 为 NaN 的情形，遵循 NaN 排在非
	 * NaN 之后的规则（参见 float8_cmp_internal）。偏移量
	 * 无法影响结论。
	 */
	if (isnan(val))
	{
		if (isnan(base))
			PG_RETURN_BOOL(true);	/* NAN = NAN */
		else
			PG_RETURN_BOOL(!less);	/* NAN > non-NAN */
	}
	else if (isnan(base))
	{
		PG_RETURN_BOOL(less);	/* non-NAN < NAN */
	}

	/*
	 * 处理 base 与 offset 均为无穷，且计算 base +/- offset 会产
	 * 生 NaN 的情形。这对应于边界无限前于 +inf 或无限
	 * 后于 -inf 的窗口帧，这并非良定义。为与其他涉及
	 * 无穷的情形保持一致——例如 +inf 无限后于 +inf，我
	 * 们选择假定 +inf 无限前于 +inf，且 -inf 无限后于
	 * -inf，从而所有有限值与无穷值都处于这样
	 * 的窗口
	 * 帧中。
	 *
	 * offset 已知为正，因此在该测试中只需检查 base 的
	 * 符号。
	 */
	if (isinf(offset) && isinf(base) &&
		(sub ? base > 0 : base < 0))
		PG_RETURN_BOOL(true);

	/*
	 * 否则计算 base +/- offset 应当是安全的。我们相信
	 * FPU 能应付输入为 +/-inf 或真实和会溢出的情况，并
	 * 产生带有适当符号的无穷大，无论 val 是否为无穷，
	 * 它都能正确参与比较。
	 */
	if (sub)
		sum = base - offset;
	else
		sum = base + offset;

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}


/*
 *		===================
 *		转换例程
 *		===================
 */

/*
 *		ftod			- 把 float4 数转换为 float8 数
 */
Datum
ftod(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);

	PG_RETURN_FLOAT8((float8) num);
}


/*
 *		dtof			- 把 float8 数转换为 float4 数
 */
Datum
dtof(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);
	float4		result;

	result = (float4) num;
	if (unlikely(isinf(result)) && !isinf(num))
		float_overflow_error();
	if (unlikely(result == 0.0f) && num != 0.0)
		float_underflow_error();

	PG_RETURN_FLOAT4(result);
}


/*
 *		dtoi4			- 把 float8 数转换为 int4 数
 */
Datum
dtoi4(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);

	/*
	 * 去掉输入中的所有小数部分。这样我们就不会因
	 * 那些恰好略微超界但舍入后落入范围内的值
	 * 而失败。注意假定 rint() 会原样通过 NaN 或 Inf。
	 */
	num = rint(num);

	/* 范围检查*/
	if (unlikely(isnan(num) || !FLOAT8_FITS_IN_INT32(num)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32((int32) num);
}


/*
 *		dtoi2			- 把 float8 数转换为 int2 数
 */
Datum
dtoi2(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);

	/*
	 * 去掉输入中的所有小数部分。这样我们就不会因
	 * 那些恰好略微超界但舍入后落入范围内的值
	 * 而失败。注意假定 rint() 会原样通过 NaN 或 Inf。
	 */
	num = rint(num);

	/* 范围检查*/
	if (unlikely(isnan(num) || !FLOAT8_FITS_IN_INT16(num)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16((int16) num);
}


/*
 *		i4tod			- 把 int4 数转换为 float8 数
 */
Datum
i4tod(PG_FUNCTION_ARGS)
{
	int32		num = PG_GETARG_INT32(0);

	PG_RETURN_FLOAT8((float8) num);
}


/*
 *		i2tod			- 把 int2 数转换为 float8 数
 */
Datum
i2tod(PG_FUNCTION_ARGS)
{
	int16		num = PG_GETARG_INT16(0);

	PG_RETURN_FLOAT8((float8) num);
}


/*
 *		ftoi4			- 把 float4 数转换为 int4 数
 */
Datum
ftoi4(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);

	/*
	 * 去掉输入中的所有小数部分。这样我们就不会因
	 * 那些恰好略微超界但舍入后落入范围内的值
	 * 而失败。注意假定 rint() 会原样通过 NaN 或 Inf。
	 */
	num = rint(num);

	/* 范围检查*/
	if (unlikely(isnan(num) || !FLOAT4_FITS_IN_INT32(num)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32((int32) num);
}


/*
 *		ftoi2			- 把 float4 数转换为 int2 数
 */
Datum
ftoi2(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);

	/*
	 * 去掉输入中的所有小数部分。这样我们就不会因
	 * 那些恰好略微超界但舍入后落入范围内的值
	 * 而失败。注意假定 rint() 会原样通过 NaN 或 Inf。
	 */
	num = rint(num);

	/* 范围检查*/
	if (unlikely(isnan(num) || !FLOAT4_FITS_IN_INT16(num)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16((int16) num);
}


/*
 *		i4tof			- 把 int4 数转换为 float4 数
 */
Datum
i4tof(PG_FUNCTION_ARGS)
{
	int32		num = PG_GETARG_INT32(0);

	PG_RETURN_FLOAT4((float4) num);
}


/*
 *		i2tof			- 把 int2 数转换为 float4 数
 */
Datum
i2tof(PG_FUNCTION_ARGS)
{
	int16		num = PG_GETARG_INT16(0);

	PG_RETURN_FLOAT4((float4) num);
}


/*
 *		=======================
 *		float8 随机算子
 *		=======================
 */

/*
 *		dround			- 返回 ROUND(arg1)
 */
Datum
dround(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(rint(arg1));
}

/*
 *		dceil			- 返回大于或等于给定 float 的
 *						  最小整数
 */
Datum
dceil(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(ceil(arg1));
}

/*
 *		dfloor			- 返回小于或等于给定 float 的
 *						  最大整数
 */
Datum
dfloor(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(floor(arg1));
}

/*
 *		dsign			- 参数小于 0 时返回 -1，参数
 *						  等于 0 时返回 0，大于零时返回
 *						  1。
 */
Datum
dsign(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	if (arg1 > 0)
		result = 1.0;
	else if (arg1 < 0)
		result = -1.0;
	else
		result = 0.0;

	PG_RETURN_FLOAT8(result);
}

/*
 *		dtrunc			- 返回 arg1 朝零截断的结果，
 *						  arg1>=0 …… 不大于 arg1 最大
 *										整数。
 *						  arg1<0 …… 不小于 arg1 最小
 *										整数。
 */
Datum
dtrunc(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	if (arg1 >= 0)
		result = floor(arg1);
	else
		result = -floor(-arg1);

	PG_RETURN_FLOAT8(result);
}


/*
 *		dsqrt			- 返回 arg1 的平方根
 */
Datum
dsqrt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	if (arg1 < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
				 errmsg("cannot take square root of a negative number")));

	result = sqrt(arg1);
	if (unlikely(isinf(result)) && !isinf(arg1))
		float_overflow_error();
	if (unlikely(result == 0.0) && arg1 != 0.0)
		float_underflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dcbrt			- 返回 arg1 的立方根
 */
Datum
dcbrt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	result = cbrt(arg1);
	if (unlikely(isinf(result)) && !isinf(arg1))
		float_overflow_error();
	if (unlikely(result == 0.0) && arg1 != 0.0)
		float_underflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dpow			- 返回 pow(arg1,arg2)
 */
Datum
dpow(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	/*
	 * POSIX 规范规定 NaN ^ 0 = 1 且 1 ^ NaN = 1，而其他所
	 * 有含 NaN 输入的情形都得到 NaN（不报错）。许多较旧
	 * 的平台会弄错其中一种或多种情形，所以要用显式
	 * 逻辑处理而非信任 pow(3)。
	 */
	if (isnan(arg1))
	{
		if (isnan(arg2) || arg2 != 0.0)
			PG_RETURN_FLOAT8(get_float8_nan());
		PG_RETURN_FLOAT8(1.0);
	}
	if (isnan(arg2))
	{
		if (arg1 != 1.0)
			PG_RETURN_FLOAT8(get_float8_nan());
		PG_RETURN_FLOAT8(1.0);
	}

	/*
	 * SQL 规范要求我们对某些错误情况发出特定的
	 * SQLSTATE 错误码。具体来说，对 0 ^ -1 我们不
	 * 返回除以零的错误码。
	 */
	if (arg1 == 0 && arg2 < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
				 errmsg("zero raised to a negative power is undefined")));
	if (arg1 < 0 && floor(arg2) != arg2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
				 errmsg("a negative number raised to a non-integer power yields a complex result")));

	/*
	 * 我们也不信任平台 pow() 按 POSIX 规范处理无穷
	 * 情形，所以这些情形也显式处理。先处理 y 为无穷
	 * 比较容易，这样 x 是否也为无穷就无关紧要。
	 */
	if (isinf(arg2))
	{
		float8		absx = fabs(arg1);

		if (absx == 1.0)
			result = 1.0;
		else if (arg2 > 0.0)	/* y = +Inf */
		{
			if (absx > 1.0)
				result = arg2;
			else
				result = 0.0;
		}
		else					/* y = -Inf */
		{
			if (absx > 1.0)
				result = 0.0;
			else
				result = -arg2;
		}
	}
	else if (isinf(arg1))
	{
		if (arg2 == 0.0)
			result = 1.0;
		else if (arg1 > 0.0)	/* x = +Inf */
		{
			if (arg2 > 0.0)
				result = arg1;
			else
				result = 0.0;
		}
		else					/* x = -Inf */
		{
			/*
			 * 按 POSIX，结果的符号取决于 y 是否为奇整
			 * 数。由于 x < 0，从前面的定义域检查已得知 y
			 * 是整数。若 y/2 不是整数，则 y 就是奇
			 * 数。
			 */
			float8		halfy = arg2 / 2;	/* 必须精确计算 */
			bool		yisoddinteger = (floor(halfy) != halfy);

			if (arg2 > 0.0)
				result = yisoddinteger ? arg1 : -arg1;
			else
				result = yisoddinteger ? -0.0 : 0.0;
		}
	}
	else
	{
		/*
		 * pow() 只在部分平台上设置 errno，取决于它遵循
		 * _IEEE_、_POSIX_、_XOPEN_ 还是 _SVID_，所以我们必须
		 * 同时检查 errno 与无效的输出值。（仅凭
		 * 后者也不可靠；一些旧平台报告溢出时会返回
		 * 很大但有限的 HUGE_VAL。）
		 */
		errno = 0;
		result = pow(arg1, arg2);
		if (errno == EDOM || isnan(result))
		{
			/*
			 * 上面已处理完所有可能的定义域错误，所以这
			 * 种情况本不该发生。但 x86 上的旧版 glibc 有一
			 * 个缺陷，当 abs(y) 大于 2^63 时会这样失败：
			 *
			 * https://sourceware.org/bugzilla/show_bug.cgi?id=3866
			 *
			 * 若走到这里，就假定 y 有限但很大（大到必
			 * 定为偶数）。结果应为：若 x == 0 则为 0，若
			 * abs(x) == 1.0 则为 1.0，否则为溢出或下溢错误。
			 */
			if (arg1 == 0.0)
				result = 0.0;	/* 前面已验证 y 为正 */
			else
			{
				float8		absx = fabs(arg1);

				if (absx == 1.0)
					result = 1.0;
				else if (arg2 >= 0.0 ? (absx > 1.0) : (absx < 1.0))
					float_overflow_error();
				else
					float_underflow_error();
			}
		}
		else if (errno == ERANGE)
		{
			if (result != 0.0)
				float_overflow_error();
			else
				float_underflow_error();
		}
		else
		{
			if (unlikely(isinf(result)))
				float_overflow_error();
			if (unlikely(result == 0.0) && arg1 != 0.0)
				float_underflow_error();
		}
	}

	PG_RETURN_FLOAT8(result);
}


/*
 *		dexp			- 返回 arg1 的指数函数值
 */
Datum
dexp(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * 显式处理 NaN 与 Inf 情形。这避免了必须假定平
	 * 台的 exp() 在这些情形下符合 POSIX，并且它消掉
	 * 了下面溢出检查的一些边界情形。
	 */
	if (isnan(arg1))
		result = arg1;
	else if (isinf(arg1))
	{
		/* 按 POSIX，exp(-Inf)=0 */
		result = (arg1 > 0.0) ? arg1 : 0;
	}
	else
	{
		/*
		 * 在一些平台上，exp() 不会设置 errno，而只是返回
		 * Inf 或零来报告溢出/下溢；因此两者都要测试。
		 */
		errno = 0;
		result = exp(arg1);
		if (unlikely(errno == ERANGE))
		{
			if (result != 0.0)
				float_overflow_error();
			else
				float_underflow_error();
		}
		else if (unlikely(isinf(result)))
			float_overflow_error();
		else if (unlikely(result == 0.0))
			float_underflow_error();
	}

	PG_RETURN_FLOAT8(result);
}


/*
 *		dlog1			- 返回 arg1 的自然对数
 */
Datum
dlog1(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * 为 ln() 发出特定的 SQLSTATE 错误码。这是 SQL 标准
	 * 的要求。
	 */
	if (arg1 == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
				 errmsg("cannot take logarithm of zero")));
	if (arg1 < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
				 errmsg("cannot take logarithm of a negative number")));

	result = log(arg1);
	if (unlikely(isinf(result)) && !isinf(arg1))
		float_overflow_error();
	if (unlikely(result == 0.0) && arg1 != 1.0)
		float_underflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dlog10			- 返回 arg1 以 10 为底的对数
 */
Datum
dlog10(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * 为 log() 发出特定的 SQLSTATE 错误码。SQL 规范并
	 * 未定义 log()，但定义了 ln()，因此对类似的错误情
	 * 况发出相同的错误码是有意义的。
	 */
	if (arg1 == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
				 errmsg("cannot take logarithm of zero")));
	if (arg1 < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
				 errmsg("cannot take logarithm of a negative number")));

	result = log10(arg1);
	if (unlikely(isinf(result)) && !isinf(arg1))
		float_overflow_error();
	if (unlikely(result == 0.0) && arg1 != 1.0)
		float_underflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dacos			- 返回 arg1 的反余弦 (弧度)
 */
Datum
dacos(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* 按 POSIX 规范，输入为 NaN 时返回 NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/*
	 * 反余弦函数的主支把 [-1, 1] 范围内的值映射到
	 * [0, Pi] 范围，所以我们应当拒绝该范围之外的任
	 * 何输入，而结果将总是有限的。
	 */
	if (arg1 < -1.0 || arg1 > 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	result = acos(arg1);
	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dasin			- 返回 arg1 的反正弦 (弧度)
 */
Datum
dasin(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* 按 POSIX 规范，输入为 NaN 时返回 NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/*
	 * 反正弦函数的主支把 [-1, 1] 范围内的值映射到
	 * [-Pi/2, Pi/2] 范围，所以我们应当拒绝该范围之外
	 * 的任何输入，而结果将总是有限的。
	 */
	if (arg1 < -1.0 || arg1 > 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	result = asin(arg1);
	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		datan			- 返回 arg1 的反正切 (弧度)
 */
Datum
datan(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* 按 POSIX 规范，输入为 NaN 时返回 NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/*
	 * 反正切函数的主支把所有输入映射到 [-Pi/2,
	 * Pi/2] 范围内的值，所以结果应总是有限的，即便
	 * 输入是无穷。
	 */
	result = atan(arg1);
	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		atan2			- 返回 arg1/arg2 的反正切 (弧度)
 */
Datum
datan2(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;

	/* 按 POSIX 规范，任一输入为 NaN 时返回 NaN */
	if (isnan(arg1) || isnan(arg2))
		PG_RETURN_FLOAT8(get_float8_nan());

	/*
	 * atan2 把所有输入映射到 [-Pi, Pi] 范围，所以结
	 * 果应总是有限的，即便输入是无穷。
	 */
	result = atan2(arg1, arg2);
	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dcos			- 返回 arg1 的余弦 (弧度)
 */
Datum
dcos(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* 按 POSIX 规范，输入为 NaN 时返回 NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/*
	 * cos() 是周期性的，因此理论上可对所有有限输入
	 * 起作用，但某些实现可能选择在输入大到结果中
	 * 没有任何有效数字时报错。所以我们应当检查错
	 * 误。POSIX 允许经由 errno 或 fetestexcept() 报错，
	 * 但目前我们只支持检查 errno。（传闻 fetestexcept()
	 * 会在某些平台上不合理地过早报告下溢，所以
	 * 无论如何，听信它是否是净改进也不
	 * 明确。）
	 *
	 * 对于无穷输入，POSIX 规定三角函数应返回定义域
	 * 错误；但除非平台经由 errno 报告，我们不会注意
	 * 到这一点，所以也要显式地测试无穷输入情
	 * 况。
	 */
	errno = 0;
	result = cos(arg1);
	if (errno != 0 || isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));
	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dcot			- 返回 arg1 的余切 (弧度)
 */
Datum
dcot(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* 按 POSIX 规范，输入为 NaN 时返回 NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/* 若输入为无穷务必抛出错误 --- 参见 dcos() */
	errno = 0;
	result = tan(arg1);
	if (errno != 0 || isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	result = 1.0 / result;
	/* 不检查溢出，因为 cot(0) == Inf */

	PG_RETURN_FLOAT8(result);
}


/*
 *		dsin			- 返回 arg1 的正弦 (弧度)
 */
Datum
dsin(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* 按 POSIX 规范，输入为 NaN 时返回 NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/* 若输入为无穷务必抛出错误 --- 参见 dcos() */
	errno = 0;
	result = sin(arg1);
	if (errno != 0 || isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));
	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dtan			- 返回 arg1 的正切 (弧度)
 */
Datum
dtan(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* 按 POSIX 规范，输入为 NaN 时返回 NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	/* 若输入为无穷务必抛出错误 --- 参见 dcos() */
	errno = 0;
	result = tan(arg1);
	if (errno != 0 || isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));
	/* 不检查溢出，因为 tan(pi/2) == Inf */

	PG_RETURN_FLOAT8(result);
}


/* ========== 基于角度的三角函数 ========== */


/*
 * 初始化本文件头部声明的缓存常量（sin_30 等）。
 * 我们之所以根本需要这些常量，更别说需要这种鲁
 * 布-戈德堡式的初始化方法，是因为现有的编译器
 * 中会有人预计算诸如 sin(constant) 这样的表达式，
 * 而其所用的 sin() 函数与运行时实际使用的不同。
 * 如果我们想要精确的结果，就必须确保基于角度的
 * 三角函数所用的缩放常量没有一个是以那种方式计
 * 算的。为此，我们用 degree_c_thirty 等变量来算；它
 * 们虽是常量，但编译器无法假定。
 *
 * 我们想用这个补丁式手法预防的其他风险还包括：
 * 编译器可能重排表达式，或者在比标准 double 更
 * 宽的寄存器中计算某些中间结果。
 *
 * 在使用这些常量的地方，典型模式如下：
 *		volatile float8 sin_x = sin(x * RADIANS_PER_DEGREE);
 *		return (sin_x / sin_30);
 * 我们希望当 x = 30 时除法恰好得到 1.0。
 * 在具有宽浮点寄存器的机器上，需要 volatile 的临
 * 时变量，以确保 sin(x) 的结果像 sin_30 的值一样被
 * 舍入到 double 宽度。用 gcc 实验表明，把临时变量
 * 标记为 volatile 是让存储和重载真正发生的必要条
 * 件；希望同样的技巧对其他编译器也有效。
 * （gcc 的文档建议使用 -ffloat-store 编译器开关来
 * 确保这一点，但那是编译器特定的，而且在很多我
 * 们不在乎的地方会劣化代码。）
 */
static void
init_degree_constants(void)
{
	sin_30 = sin(degree_c_thirty * RADIANS_PER_DEGREE);
	one_minus_cos_60 = 1.0 - cos(degree_c_sixty * RADIANS_PER_DEGREE);
	asin_0_5 = asin(degree_c_one_half);
	acos_0_5 = acos(degree_c_one_half);
	atan_1_0 = atan(degree_c_one);
	tan_45 = sind_q1(degree_c_forty_five) / cosd_q1(degree_c_forty_five);
	cot_45 = cosd_q1(degree_c_forty_five) / sind_q1(degree_c_forty_five);
	degree_consts_set = true;
}

#define INIT_DEGREE_CONSTANTS() \
do { \
	if (!degree_consts_set) \
		init_degree_constants(); \
} while(0)


/*
 *		asind_q1		- 返回 x 的反正弦值（以度为单位），
 *						  x 在 [0, 1] 范围内。结果是第一象限
 *						  内的角度 --- [0, 90] 度。
 *
 *						  对 0、0.5 和 1 这 3 个特殊输入，它会
 *						  分别返回精确值（分别为 0、30 和
 *						  90 度）。
 */
static double
asind_q1(double x)
{
	/*
	 * 拼接 [0, 0.5] 与 (0.5, 1] 范围的反正弦和反余弦
	 * 函数。下面的每个表达式都保证对 x=0.5 精确返
	 * 回 30，因此结果在整个范围上是连续的单调
	 * 函数。
	 */
	if (x <= 0.5)
	{
		volatile float8 asin_x = asin(x);

		return (asin_x / asin_0_5) * 30.0;
	}
	else
	{
		volatile float8 acos_x = acos(x);

		return 90.0 - (acos_x / acos_0_5) * 60.0;
	}
}


/*
 *		acosd_q1		- 返回 x 的反余弦值（以度为单位），
 *						  x 在 [0, 1] 范围内。结果是第一象限
 *						  内的角度 --- [0, 90] 度。
 *
 *						  对 0、0.5 和 1 这 3 个特殊输入，它会
 *						  分别返回精确值（分别为 0、60 和
 *						  90 度）。
 */
static double
acosd_q1(double x)
{
	/*
	 * 拼接 [0, 0.5] 与 (0.5, 1] 范围的反正弦和反余弦
	 * 函数。下面的每个表达式都保证对 x=0.5 精确返
	 * 回 60，因此结果在整个范围上是连续的单调
	 * 函数。
	 */
	if (x <= 0.5)
	{
		volatile float8 asin_x = asin(x);

		return 90.0 - (asin_x / asin_0_5) * 30.0;
	}
	else
	{
		volatile float8 acos_x = acos(x);

		return (acos_x / acos_0_5) * 60.0;
	}
}


/*
 *		dacosd			- 返回 arg1 的反余弦 (度)
 */
Datum
dacosd(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* 按 POSIX 规范，输入为 NaN 时返回 NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	INIT_DEGREE_CONSTANTS();

	/*
	 * 反余弦函数的主支把 [-1, 1] 范围内的值映射到
	 * [0, 180] 范围，所以我们应当拒绝该范围之外的
	 * 任何输入，而结果将总是有限的。
	 */
	if (arg1 < -1.0 || arg1 > 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	if (arg1 >= 0.0)
		result = acosd_q1(arg1);
	else
		result = 90.0 + asind_q1(-arg1);

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dasind			- 返回 arg1 的反正弦 (度)
 */
Datum
dasind(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/* 按 POSIX 规范，输入为 NaN 时返回 NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	INIT_DEGREE_CONSTANTS();

	/*
	 * 反正弦函数的主支把 [-1, 1] 范围内的值映射到
	 * [-90, 90] 范围，所以我们应当拒绝该范围之外的
	 * 任何输入，而结果将总是有限的。
	 */
	if (arg1 < -1.0 || arg1 > 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	if (arg1 >= 0.0)
		result = asind_q1(arg1);
	else
		result = -asind_q1(-arg1);

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		datand			- 返回 arg1 的反正切 (度)
 */
Datum
datand(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;
	volatile float8 atan_arg1;

	/* 按 POSIX 规范，输入为 NaN 时返回 NaN */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	INIT_DEGREE_CONSTANTS();

	/*
	 * 反正切函数的主支把所有输入映射到 [-90, 90]
	 * 范围内的值，所以结果应总是有限的，即便输入
	 * 是无穷。此外，我们注意确保当 arg1 为 1 时，
	 * 结果恰好是 45。
	 */
	atan_arg1 = atan(arg1);
	result = (atan_arg1 / atan_1_0) * 45.0;

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		atan2d			- 返回 arg1/arg2 的反正切 (度)
 */
Datum
datan2d(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);
	float8		result;
	volatile float8 atan2_arg1_arg2;

	/* 按 POSIX 规范，任一输入为 NaN 时返回 NaN */
	if (isnan(arg1) || isnan(arg2))
		PG_RETURN_FLOAT8(get_float8_nan());

	INIT_DEGREE_CONSTANTS();

	/*
	 * atan2d 把所有输入映射到 [-180, 180] 范围，所以
	 * 结果应总是有限的，即便输入是无穷。
	 *
	 * 注意：这段代码假定 atan(1.0) 是一个合适的缩放
	 * 常量，能让 atan2() 得到精确结果。这一点在某天
	 * 可能会让我们失望，届时需要我们确切决定要为哪
	 * 些输入保证精确结果。
	 */
	atan2_arg1_arg2 = atan2(arg1, arg2);
	result = (atan2_arg1_arg2 / atan_1_0) * 45.0;

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		sind_0_to_30	- 返回 0 到 30 度之间的角的正弦。
 *						  x 为 0 时精确返回 0；x 为 30 度时精确
 *						  返回 0.5。
 */
static double
sind_0_to_30(double x)
{
	volatile float8 sin_x = sin(x * RADIANS_PER_DEGREE);

	return (sin_x / sin_30) / 2.0;
}


/*
 *		cosd_0_to_60	- 返回 0 到 60 度之间的角的余弦。
 *						  x 为 0 时精确返回 1；x 为 60 度时精确
 *						  返回 0.5。
 */
static double
cosd_0_to_60(double x)
{
	volatile float8 one_minus_cos_x = 1.0 - cos(x * RADIANS_PER_DEGREE);

	return 1.0 - (one_minus_cos_x / one_minus_cos_60) / 2.0;
}


/*
 *		sind_q1			- 返回第一象限（0 到 90 度）内的
 *						  角的正弦。
 */
static double
sind_q1(double x)
{
	/*
	 * 拼接 [0, 30] 与 (30, 90] 的正弦和余弦函数
	 * 它们保证在端点处返回精确答案，因此整体结果
	 * 是一个连续的单调函数，在 x = 0、30、90 度时
	 * 给出精确结果。
	 */
	if (x <= 30.0)
		return sind_0_to_30(x);
	else
		return cosd_0_to_60(90.0 - x);
}


/*
 *		cosd_q1			- 返回第一象限（0 到 90 度）内的
 *						  角的余弦。
 */
static double
cosd_q1(double x)
{
	/*
	 * 拼接 [0, 60] 与 (60, 90] 的正弦和余弦函数
	 * 它们保证在端点处返回精确答案，因此整体结果
	 * 是一个连续的单调函数，在 x = 0、60、90 度时
	 * 给出精确结果。
	 */
	if (x <= 60.0)
		return cosd_0_to_60(x);
	else
		return sind_0_to_30(90.0 - x);
}


/*
 *		dcosd			- 返回 arg1 的余弦 (度)
 */
Datum
dcosd(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;
	int			sign = 1;

	/*
	 * 按 POSIX 规范，输入为 NaN 时返回 NaN，输入为无
	 * 穷时抛出错误。
	 */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	if (isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	INIT_DEGREE_CONSTANTS();

	/* 把输入的范围约减到 [0,90] 度 */
	arg1 = fmod(arg1, 360.0);

	if (arg1 < 0.0)
	{
		/* cosd(-x) = cosd(x) */
		arg1 = -arg1;
	}

	if (arg1 > 180.0)
	{
		/* cosd(360-x) = cosd(x) */
		arg1 = 360.0 - arg1;
	}

	if (arg1 > 90.0)
	{
		/* cosd(180-x) = -cosd(x) */
		arg1 = 180.0 - arg1;
		sign = -sign;
	}

	result = sign * cosd_q1(arg1);

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dcotd			- 返回 arg1 的余切 (度)
 */
Datum
dcotd(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;
	volatile float8 cot_arg1;
	int			sign = 1;

	/*
	 * 按 POSIX 规范，输入为 NaN 时返回 NaN，输入为无
	 * 穷时抛出错误。
	 */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	if (isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	INIT_DEGREE_CONSTANTS();

	/* 把输入的范围约减到 [0,90] 度 */
	arg1 = fmod(arg1, 360.0);

	if (arg1 < 0.0)
	{
		/* cotd(-x) = -cotd(x) */
		arg1 = -arg1;
		sign = -sign;
	}

	if (arg1 > 180.0)
	{
		/* cotd(360-x) = -cotd(x) */
		arg1 = 360.0 - arg1;
		sign = -sign;
	}

	if (arg1 > 90.0)
	{
		/* cotd(180-x) = -cotd(x) */
		arg1 = 180.0 - arg1;
		sign = -sign;
	}

	cot_arg1 = cosd_q1(arg1) / sind_q1(arg1);
	result = sign * (cot_arg1 / cot_45);

	/*
	 * 在某些机器上我们会得到 cotd(270) = 负零，但这并
	 * 非总是如此。为了可移植性，也因为这个函数的用
	 * 户群体多半不想要负零，我们强制其为普通零。
	 */
	if (result == 0.0)
		result = 0.0;

	/* 不检查溢出，因为 cotd(0) == Inf */

	PG_RETURN_FLOAT8(result);
}


/*
 *		dsind			- 返回 arg1 的正弦 (度)
 */
Datum
dsind(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;
	int			sign = 1;

	/*
	 * 按 POSIX 规范，输入为 NaN 时返回 NaN，输入为无
	 * 穷时抛出错误。
	 */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	if (isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	INIT_DEGREE_CONSTANTS();

	/* 把输入的范围约减到 [0,90] 度 */
	arg1 = fmod(arg1, 360.0);

	if (arg1 < 0.0)
	{
		/* sind(-x) = -sind(x) */
		arg1 = -arg1;
		sign = -sign;
	}

	if (arg1 > 180.0)
	{
		/* sind(360-x) = -sind(x) */
		arg1 = 360.0 - arg1;
		sign = -sign;
	}

	if (arg1 > 90.0)
	{
		/* sind(180-x) = sind(x) */
		arg1 = 180.0 - arg1;
	}

	result = sign * sind_q1(arg1);

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/*
 *		dtand			- 返回 arg1 的正切 (度)
 */
Datum
dtand(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;
	volatile float8 tan_arg1;
	int			sign = 1;

	/*
	 * 按 POSIX 规范，输入为 NaN 时返回 NaN，输入为无
	 * 穷时抛出错误。
	 */
	if (isnan(arg1))
		PG_RETURN_FLOAT8(get_float8_nan());

	if (isinf(arg1))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	INIT_DEGREE_CONSTANTS();

	/* 把输入的范围约减到 [0,90] 度 */
	arg1 = fmod(arg1, 360.0);

	if (arg1 < 0.0)
	{
		/* tand(-x) = -tand(x) */
		arg1 = -arg1;
		sign = -sign;
	}

	if (arg1 > 180.0)
	{
		/* tand(360-x) = -tand(x) */
		arg1 = 360.0 - arg1;
		sign = -sign;
	}

	if (arg1 > 90.0)
	{
		/* tand(180-x) = -tand(x) */
		arg1 = 180.0 - arg1;
		sign = -sign;
	}

	tan_arg1 = sind_q1(arg1) / cosd_q1(arg1);
	result = sign * (tan_arg1 / tan_45);

	/*
	 * 在某些机器上我们会得到 tand(180) = 负零，但这并
	 * 非总是如此。为了可移植性，也因为这个函数的用
	 * 户群体多半不想要负零，我们强制其为普通零。
	 */
	if (result == 0.0)
		result = 0.0;

	/* 不检查溢出，因为 tand(90) == Inf */

	PG_RETURN_FLOAT8(result);
}


/*
 *		degrees		- 返回由弧度转换而来的角度
 */
Datum
degrees(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(float8_div(arg1, RADIANS_PER_DEGREE));
}


/*
 *		dpi				- 返回常量 PI
 */
Datum
dpi(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(M_PI);
}


/*
 *		radians		- 返回由角度转换而来的弧度
 */
Datum
radians(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(float8_mul(arg1, RADIANS_PER_DEGREE));
}


/* ========== 双曲函数 ========== */


/*
 *		dsinh			- 返回 arg1 的双曲正弦
 */
Datum
dsinh(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	errno = 0;
	result = sinh(arg1);

	/*
	 * 若发生 ERANGE 错误，就意味着溢出。对 sinh 而
	 * 言，结果应为 -Inf 或 Inf，取决于 arg1 的符
	 * 号。
	 */
	if (errno == ERANGE)
	{
		if (arg1 < 0)
			result = -get_float8_infinity();
		else
			result = get_float8_infinity();
	}

	PG_RETURN_FLOAT8(result);
}


/*
 *		dcosh			- 返回 arg1 的双曲余弦
 */
Datum
dcosh(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	errno = 0;
	result = cosh(arg1);

	/*
	 * 若发生 ERANGE 错误，就意味着溢出。由于 cosh 总
	 * 为正，因此这总是意味着结果为正无穷。
	 */
	if (errno == ERANGE)
		result = get_float8_infinity();

	if (unlikely(result == 0.0))
		float_underflow_error();

	PG_RETURN_FLOAT8(result);
}

/*
 *		dtanh			- 返回 arg1 的双曲正切
 */
Datum
dtanh(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * 对 tanh 而言，无需 errno 检查，因为它从不溢出。
	 */
	result = tanh(arg1);

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}

/*
 *		dasinh			- 返回 arg1 的反双曲正弦
 */
Datum
dasinh(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * 对 asinh 而言，无需 errno 检查，因为它从不溢出。
	 */
	result = asinh(arg1);

	PG_RETURN_FLOAT8(result);
}

/*
 *		dacosh			- 返回 arg1 的反双曲余弦
 */
Datum
dacosh(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * acosh 仅对 >= 1.0 的输入有定义。我们自己检查这
	 * 一点后，就无需操心 EDOM 错误的检查，这是一件
	 * 好事，因为某些实现对 NaN 也会报告该错误。除此之
	 * 外不可能出错。
	 */
	if (arg1 < 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	result = acosh(arg1);

	PG_RETURN_FLOAT8(result);
}

/*
 *		datanh			- 返回 arg1 的反双曲正切
 */
Datum
datanh(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * atanh 仅对 -1 与 1 之间的输入有定义。我们自己
	 * 检查这一点后，就无需操心 EDOM 错误的检查，这
	 * 是一件好事，因为某些实现对 NaN 也报告该错误。
	 */
	if (arg1 < -1.0 || arg1 > 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("input is out of range")));

	/*
	 * 我们也自行处理无穷的情形；这很有帮助，因为旧
	 * 版 glibc 可能为此产生错误的 errno。所有其他输入
	 * 都不可能出错。
	 */
	if (arg1 == -1.0)
		result = -get_float8_infinity();
	else if (arg1 == 1.0)
		result = get_float8_infinity();
	else
		result = atanh(arg1);

	PG_RETURN_FLOAT8(result);
}


/* ========== 误差函数 ========== */


/*
 *		derf			- 返回误差函数：erf(arg1)
 */
Datum
derf(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * 对 erf 而言，无需 errno 检查，因为它从不溢出。
	 */
	result = erf(arg1);

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}

/*
 *		derfc			- 返回互补误差函数：1 - erf(arg1)
 */
Datum
derfc(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float8		result;

	/*
	 * 对 erfc 而言，无需 errno 检查，因为它从不溢出。
	 */
	result = erfc(arg1);

	if (unlikely(isinf(result)))
		float_overflow_error();

	PG_RETURN_FLOAT8(result);
}


/* ========== 随机函数 ========== */


/*
 * initialize_drandom_seed - 尚未完成则初始化 drandom_seed
 */
static void
initialize_drandom_seed(void)
{
	/* 初始化随机种子，若本进程中尚未完成 */
	if (unlikely(!drandom_seed_set))
	{
		/*
		 * 如有可能，用高质量随机比特来初始化种子。
		 * 若因故失败，我们退而使用基于当前时间和 PID 的
		 * 较低质量种子。
		 */
		if (unlikely(!pg_prng_strong_seed(&drandom_seed)))
		{
			TimestampTz now = GetCurrentTimestamp();
			uint64		iseed;

			/* 把 PID 与时间戳中最可预测的比特混合 */
			iseed = (uint64) now ^ ((uint64) MyProcPid << 32);
			pg_prng_seed(&drandom_seed, iseed);
		}
		drandom_seed_set = true;
	}
}

/*
 *		drandom		- 返回一个随机数
 */
Datum
drandom(PG_FUNCTION_ARGS)
{
	float8		result;

	initialize_drandom_seed();

	/* pg_prng_double 产生期望的结果范围 [0.0 - 1.0) */
	result = pg_prng_double(&drandom_seed);

	PG_RETURN_FLOAT8(result);
}

/*
 *		drandom_normal	- 返回来自正态分布的随机数
 */
Datum
drandom_normal(PG_FUNCTION_ARGS)
{
	float8		mean = PG_GETARG_FLOAT8(0);
	float8		stddev = PG_GETARG_FLOAT8(1);
	float8		result,
				z;

	initialize_drandom_seed();

	/* 从标准正态（均值 0.0，标准差 1.0）取随机值 */
	z = pg_prng_double_normal(&drandom_seed);
	/* 变换正态标准变量 (z) */
	/* 使用目标正态分布的参数 */
	result = (stddev * z) + mean;

	PG_RETURN_FLOAT8(result);
}

/*
 *		setseed		- 为随机数发生器设置种子
 */
Datum
setseed(PG_FUNCTION_ARGS)
{
	float8		seed = PG_GETARG_FLOAT8(0);

	if (seed < -1 || seed > 1 || isnan(seed))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("setseed parameter %g is out of allowed range [-1,1]",
						seed)));

	pg_prng_fseed(&drandom_seed, seed);
	drandom_seed_set = true;

	PG_RETURN_VOID();
}



/*
 *		=========================
 *		浮点聚合算子
 *		=========================
 *
 *		float8_accum		- 为 AVG()、方差聚合等累加
 *		float4_accum		- 同上，但输入是 float4
 *		float8_avg			- 为 float AVG() 产出最终结果
 *		float8_var_samp		- 为 float VAR_SAMP() 产出最终结果
 *		float8_var_pop		- 为 float VAR_POP() 产出最终结果
 *		float8_stddev_samp	- 为 float STDDEV_SAMP() 产出最终结果
 *		float8_stddev_pop	- 为 float STDDEV_POP() 产出最终结果
 *
 * 这些聚合的朴素书本式实现通过累加 sum(X) 与
 * sum(X^2) 来工作。然而这种方法在最终计算诸如总
 * 体方差 (N*sum(X^2) - sum(X)^2) / N^2 这类量时会
 * 产生很大的舍入误差，因为每个中间项都可能非常
 * 大，而差值往往
 * 很小。
 *
 * 因此我们改用 Youngs-Cramer 算法[1]，它通过累加
 * Sx=sum(X) 与 Sxx=sum((X-Sx/N)^2)，并用数值稳定的
 * 算法来增量更新这些量。此后各聚合值的最终计算
 * 都很简单，且能给出更精确的结果（例如总体方差
 * 就是 Sxx/N）。该算法还相当容易推广为可并行执行
 * 而不损失精度（例如参见 [2]）。更多细节以及其他
 * 算法的比较
 * 参见 [3]。
 *
 * 所有这些聚合的过渡数据类型都是一个 3 元素的
 * float8 数组，按顺序存放 N、Sx、Sxx。
 *
 * 注意我们把 N 表示为 float，以避免构建一种特殊
 * 的数据类型。只要浮点实现合理，除非 N 超过 2 ^ 52
 * 左右，否则不应有精度损失（到那时用户恐怕早已
 * 失去兴趣了……）
 *
 * [1] Some Results Relevant to Choice of Sum and Sum-of-Product Algorithms,
 * E. A. Youngs and E. M. Cramer, Technometrics Vol 13, No 3, August 1971.
 *
 * [2] Updating Formulae and a Pairwise Algorithm for Computing Sample
 * Variances, T. F. Chan, G. H. Golub & R. J. LeVeque, COMPSTAT 1982.
 *
 * [3] Numerically Stable Parallel Computation of (Co-)Variance, Erich
 * Schubert and Michael Gertz, Proceedings of the 30th International
 * Conference on Scientific and Statistical Database Management, 2018.
 */

static float8 *
check_float8_array(ArrayType *transarray, const char *caller, int n)
{
	/*
	 * 我们期望输入是一个 N 元素的 float 数组；请验证
	 * 这一点。无需使用 deconstruct_array()，因为数组数据
	 * 只是 N 个 float8 值构成的 C 数组。
	 */
	if (ARR_NDIM(transarray) != 1 ||
		ARR_DIMS(transarray)[0] != n ||
		ARR_HASNULL(transarray) ||
		ARR_ELEMTYPE(transarray) != FLOAT8OID)
		elog(ERROR, "%s: expected %d-element float8 array", caller, n);
	return (float8 *) ARR_DATA_PTR(transarray);
}

/*
 * float8_combine
 *
 * 合并函数，把两份 3 字段聚合过渡数据合并
 * 成单一过渡数据。本函数仅用于两阶段聚
 * 合中，不应在聚合上下文之外
 * 调用。
 */
Datum
float8_combine(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *transarray2 = PG_GETARG_ARRAYTYPE_P(1);
	float8	   *transvalues1;
	float8	   *transvalues2;
	float8		N1,
				Sx1,
				Sxx1,
				N2,
				Sx2,
				Sxx2,
				tmp,
				N,
				Sx,
				Sxx;

	transvalues1 = check_float8_array(transarray1, "float8_combine", 3);
	transvalues2 = check_float8_array(transarray2, "float8_combine", 3);

	N1 = transvalues1[0];
	Sx1 = transvalues1[1];
	Sxx1 = transvalues1[2];

	N2 = transvalues2[0];
	Sx2 = transvalues2[1];
	Sxx2 = transvalues2[2];

	/*--------------------
	 * 过渡值按下述推广的 Youngs-Cramer 算法合并：
	 * 具体如下：
	 *
	 *	N = N1 + N2
	 *	Sx = Sx1 + Sx2
	 *	Sxx = Sxx1 + Sxx2 + N1 * N2 * (Sx1/N1 - Sx2/N2)^2 / N;
	 *
	 * 值得单独处理 N1 = 0 与 N2 = 0 这两种特殊情形，
	 * 因为这两种情形很平凡，而且此后在一般情形中我
	 * 们无需担心除以零的错误。
	 *--------------------
	 */
	if (N1 == 0.0)
	{
		N = N2;
		Sx = Sx2;
		Sxx = Sxx2;
	}
	else if (N2 == 0.0)
	{
		N = N1;
		Sx = Sx1;
		Sxx = Sxx1;
	}
	else
	{
		N = N1 + N2;
		Sx = float8_pl(Sx1, Sx2);
		tmp = Sx1 / N1 - Sx2 / N2;
		Sxx = Sxx1 + Sxx2 + N1 * N2 * tmp * tmp / N;
		if (unlikely(isinf(Sxx)) && !isinf(Sxx1) && !isinf(Sxx2))
			float_overflow_error();
	}

	/*
	 * 如果我们作为聚合被调用，可以作弊并就地修改第
	 * 一个参数，以减少 palloc 开销。否则我们构造含
	 * 有更新后过渡数据的新数组并返回它。
	 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues1[0] = N;
		transvalues1[1] = Sx;
		transvalues1[2] = Sxx;

		PG_RETURN_ARRAYTYPE_P(transarray1);
	}
	else
	{
		Datum		transdatums[3];
		ArrayType  *result;

		transdatums[0] = Float8GetDatumFast(N);
		transdatums[1] = Float8GetDatumFast(Sx);
		transdatums[2] = Float8GetDatumFast(Sxx);

		result = construct_array(transdatums, 3,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

		PG_RETURN_ARRAYTYPE_P(result);
	}
}

Datum
float8_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8		newval = PG_GETARG_FLOAT8(1);
	float8	   *transvalues;
	float8		N,
				Sx,
				Sxx,
				tmp;

	transvalues = check_float8_array(transarray, "float8_accum", 3);
	N = transvalues[0];
	Sx = transvalues[1];
	Sxx = transvalues[2];

	/*
	 * 用 Youngs-Cramer 算法把新值合并进
	 * 过渡值中。
	 */
	N += 1.0;
	Sx += newval;
	if (transvalues[0] > 0.0)
	{
		tmp = newval * N - Sx;
		Sxx += tmp * tmp / (N * transvalues[0]);

		/*
		 * 溢出检查。有限输入导致无穷结果时，我们
		 * 才报告溢出错误。还要注意，若任何输入为无
		 * 穷，Sxx 就应为 NaN，所以我们有意阻止 Sxx
		 * 变成无穷。
		 */
		if (isinf(Sx) || isinf(Sxx))
		{
			if (!isinf(transvalues[1]) && !isinf(newval))
				float_overflow_error();

			Sxx = get_float8_nan();
		}
	}
	else
	{
		/*
		 * 在第一个输入时，我们通常可以让 Sxx 保持为 0。
		 * 但如果第一个输入是 Inf 或 NaN，最好把 Sxx 强
		 * 制为 NaN；否则当没有更多输入时我们会误报方差
		 * 为零。
		 */
		if (isnan(newval) || isinf(newval))
			Sxx = get_float8_nan();
	}

	/*
	 * 如果我们作为聚合被调用，可以作弊并就地修改第
	 * 一个参数，以减少 palloc 开销。否则我们构造含
	 * 有更新后过渡数据的新数组并返回它。
	 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues[0] = N;
		transvalues[1] = Sx;
		transvalues[2] = Sxx;

		PG_RETURN_ARRAYTYPE_P(transarray);
	}
	else
	{
		Datum		transdatums[3];
		ArrayType  *result;

		transdatums[0] = Float8GetDatumFast(N);
		transdatums[1] = Float8GetDatumFast(Sx);
		transdatums[2] = Float8GetDatumFast(Sxx);

		result = construct_array(transdatums, 3,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

		PG_RETURN_ARRAYTYPE_P(result);
	}
}

Datum
float4_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);

	/* 以 float8 进行计算 */
	float8		newval = PG_GETARG_FLOAT4(1);
	float8	   *transvalues;
	float8		N,
				Sx,
				Sxx,
				tmp;

	transvalues = check_float8_array(transarray, "float4_accum", 3);
	N = transvalues[0];
	Sx = transvalues[1];
	Sxx = transvalues[2];

	/*
	 * 用 Youngs-Cramer 算法把新值合并进
	 * 过渡值中。
	 */
	N += 1.0;
	Sx += newval;
	if (transvalues[0] > 0.0)
	{
		tmp = newval * N - Sx;
		Sxx += tmp * tmp / (N * transvalues[0]);

		/*
		 * 溢出检查。有限输入导致无穷结果时，我们
		 * 才报告溢出错误。还要注意，若任何输入为无
		 * 穷，Sxx 就应为 NaN，所以我们有意阻止 Sxx
		 * 变成无穷。
		 */
		if (isinf(Sx) || isinf(Sxx))
		{
			if (!isinf(transvalues[1]) && !isinf(newval))
				float_overflow_error();

			Sxx = get_float8_nan();
		}
	}
	else
	{
		/*
		 * 在第一个输入时，我们通常可以让 Sxx 保持为 0。
		 * 但如果第一个输入是 Inf 或 NaN，最好把 Sxx 强
		 * 制为 NaN；否则当没有更多输入时我们会误报方差
		 * 为零。
		 */
		if (isnan(newval) || isinf(newval))
			Sxx = get_float8_nan();
	}

	/*
	 * 如果我们作为聚合被调用，可以作弊并就地修改第
	 * 一个参数，以减少 palloc 开销。否则我们构造含
	 * 有更新后过渡数据的新数组并返回它。
	 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues[0] = N;
		transvalues[1] = Sx;
		transvalues[2] = Sxx;

		PG_RETURN_ARRAYTYPE_P(transarray);
	}
	else
	{
		Datum		transdatums[3];
		ArrayType  *result;

		transdatums[0] = Float8GetDatumFast(N);
		transdatums[1] = Float8GetDatumFast(Sx);
		transdatums[2] = Float8GetDatumFast(Sxx);

		result = construct_array(transdatums, 3,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

		PG_RETURN_ARRAYTYPE_P(result);
	}
}

Datum
float8_avg(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sx;

	transvalues = check_float8_array(transarray, "float8_avg", 3);
	N = transvalues[0];
	Sx = transvalues[1];
	/* 忽略 Sxx */

	/* SQL 定义无值时 AVG 为 NULL */
	if (N == 0.0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sx / N);
}

Datum
float8_var_pop(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx;

	transvalues = check_float8_array(transarray, "float8_var_pop", 3);
	N = transvalues[0];
	/* 忽略 Sx */
	Sxx = transvalues[2];

	/* N 为 0 时总体方差无定义，因此返回 NULL */
	if (N == 0.0)
		PG_RETURN_NULL();

	/* 注意 Sxx 保证非负 */

	PG_RETURN_FLOAT8(Sxx / N);
}

Datum
float8_var_samp(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx;

	transvalues = check_float8_array(transarray, "float8_var_samp", 3);
	N = transvalues[0];
	/* 忽略 Sx */
	Sxx = transvalues[2];

	/* N 为 0 或 1 时样本方差无定义，因此返回 NULL */
	if (N <= 1.0)
		PG_RETURN_NULL();

	/* 注意 Sxx 保证非负 */

	PG_RETURN_FLOAT8(Sxx / (N - 1.0));
}

Datum
float8_stddev_pop(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx;

	transvalues = check_float8_array(transarray, "float8_stddev_pop", 3);
	N = transvalues[0];
	/* 忽略 Sx */
	Sxx = transvalues[2];

	/* N 为 0 时总体标准差无定义，因此返回 NULL */
	if (N == 0.0)
		PG_RETURN_NULL();

	/* 注意 Sxx 保证非负 */

	PG_RETURN_FLOAT8(sqrt(Sxx / N));
}

Datum
float8_stddev_samp(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx;

	transvalues = check_float8_array(transarray, "float8_stddev_samp", 3);
	N = transvalues[0];
	/* 忽略 Sx */
	Sxx = transvalues[2];

	/* N 为 0 或 1 时样本标准差无定义故返回 NULL */
	if (N <= 1.0)
		PG_RETURN_NULL();

	/* 注意 Sxx 保证非负 */

	PG_RETURN_FLOAT8(sqrt(Sxx / (N - 1.0)));
}

/*
 *		=========================
 *		SQL2003 二元聚合
 *		=========================
 *
 * 与前面的聚合一样，我们使用 Youngs-Cramer 算法来
 * 降低聚合最终函数中的舍入误差。
 *
 * 所有这些聚合的过渡数据类型都是一个 6 元素的
 * float8 数组，按顺序存放 N、Sx=sum(X)、Sxx=sum((X-Sx/N)^2)、
 * Sy=sum(Y), Syy=sum((Y-Sy/N)^2), Sxy=sum((X-Sx/N)*(Y-Sy/N))。
 *
 * 注意 Y 是所有这些聚合的第一个参数！
 *
 * 通过设置多个只计算实际需要的和的累加器函数来
 * 优化，看起来挺有吸引力。但在大多数现代机器
 * 上，额外几次浮点乘法与其他每元组开销相比无足
 * 轻重，所以我选择把代码空间减到
 * 最小。
 */

Datum
float8_regr_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8		newvalY = PG_GETARG_FLOAT8(1);
	float8		newvalX = PG_GETARG_FLOAT8(2);
	float8	   *transvalues;
	float8		N,
				Sx,
				Sxx,
				Sy,
				Syy,
				Sxy,
				tmpX,
				tmpY,
				scale;

	transvalues = check_float8_array(transarray, "float8_regr_accum", 6);
	N = transvalues[0];
	Sx = transvalues[1];
	Sxx = transvalues[2];
	Sy = transvalues[3];
	Syy = transvalues[4];
	Sxy = transvalues[5];

	/*
	 * 用 Youngs-Cramer 算法把新值合并进
	 * 过渡值中。
	 */
	N += 1.0;
	Sx += newvalX;
	Sy += newvalY;
	if (transvalues[0] > 0.0)
	{
		tmpX = newvalX * N - Sx;
		tmpY = newvalY * N - Sy;
		scale = 1.0 / (N * transvalues[0]);
		Sxx += tmpX * tmpX * scale;
		Syy += tmpY * tmpY * scale;
		Sxy += tmpX * tmpY * scale;

		/*
		 * 溢出检查。有限输入导致无穷结果时，我们
		 * 才报告溢出错误。还须注意，若有任何相关输
		 * 入为无穷，则 Sxx、Syy 与 Sxy 都应为 NaN，
		 * 所以我们有意阻止它们变成无穷。
		 */
		if (isinf(Sx) || isinf(Sxx) || isinf(Sy) || isinf(Syy) || isinf(Sxy))
		{
			if (((isinf(Sx) || isinf(Sxx)) &&
				 !isinf(transvalues[1]) && !isinf(newvalX)) ||
				((isinf(Sy) || isinf(Syy)) &&
				 !isinf(transvalues[3]) && !isinf(newvalY)) ||
				(isinf(Sxy) &&
				 !isinf(transvalues[1]) && !isinf(newvalX) &&
				 !isinf(transvalues[3]) && !isinf(newvalY)))
				float_overflow_error();

			if (isinf(Sxx))
				Sxx = get_float8_nan();
			if (isinf(Syy))
				Syy = get_float8_nan();
			if (isinf(Sxy))
				Sxy = get_float8_nan();
		}
	}
	else
	{
		/*
		 * 在第一个输入时，我们通常可以让 Sxx 等保持为
		 * 0。但如果第一个输入是 Inf 或 NaN，最好把依赖
		 * 的和强制为 NaN；否则当没有更多输入时我们会误
		 * 报方差为零。
		 */
		if (isnan(newvalX) || isinf(newvalX))
			Sxx = Sxy = get_float8_nan();
		if (isnan(newvalY) || isinf(newvalY))
			Syy = Sxy = get_float8_nan();
	}

	/*
	 * 如果我们作为聚合被调用，可以作弊并就地修改第
	 * 一个参数，以减少 palloc 开销。否则我们构造含
	 * 有更新后过渡数据的新数组并返回它。
	 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues[0] = N;
		transvalues[1] = Sx;
		transvalues[2] = Sxx;
		transvalues[3] = Sy;
		transvalues[4] = Syy;
		transvalues[5] = Sxy;

		PG_RETURN_ARRAYTYPE_P(transarray);
	}
	else
	{
		Datum		transdatums[6];
		ArrayType  *result;

		transdatums[0] = Float8GetDatumFast(N);
		transdatums[1] = Float8GetDatumFast(Sx);
		transdatums[2] = Float8GetDatumFast(Sxx);
		transdatums[3] = Float8GetDatumFast(Sy);
		transdatums[4] = Float8GetDatumFast(Syy);
		transdatums[5] = Float8GetDatumFast(Sxy);

		result = construct_array(transdatums, 6,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

		PG_RETURN_ARRAYTYPE_P(result);
	}
}

/*
 * float8_regr_combine
 *
 * 一种合并函数，把两份 6 字段聚合过渡数据
 * 合并成单一过渡数据。只用于两阶段聚
 * 合中，不应在聚合上下文之外
 * 调用。
 */
Datum
float8_regr_combine(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *transarray2 = PG_GETARG_ARRAYTYPE_P(1);
	float8	   *transvalues1;
	float8	   *transvalues2;
	float8		N1,
				Sx1,
				Sxx1,
				Sy1,
				Syy1,
				Sxy1,
				N2,
				Sx2,
				Sxx2,
				Sy2,
				Syy2,
				Sxy2,
				tmp1,
				tmp2,
				N,
				Sx,
				Sxx,
				Sy,
				Syy,
				Sxy;

	transvalues1 = check_float8_array(transarray1, "float8_regr_combine", 6);
	transvalues2 = check_float8_array(transarray2, "float8_regr_combine", 6);

	N1 = transvalues1[0];
	Sx1 = transvalues1[1];
	Sxx1 = transvalues1[2];
	Sy1 = transvalues1[3];
	Syy1 = transvalues1[4];
	Sxy1 = transvalues1[5];

	N2 = transvalues2[0];
	Sx2 = transvalues2[1];
	Sxx2 = transvalues2[2];
	Sy2 = transvalues2[3];
	Syy2 = transvalues2[4];
	Sxy2 = transvalues2[5];

	/*--------------------
	 * 过渡值按下述推广的 Youngs-Cramer 算法合并：
	 * 具体如下：
	 *
	 *	N = N1 + N2
	 *	Sx = Sx1 + Sx2
	 *	Sxx = Sxx1 + Sxx2 + N1 * N2 * (Sx1/N1 - Sx2/N2)^2 / N
	 *	Sy = Sy1 + Sy2
	 *	Syy = Syy1 + Syy2 + N1 * N2 * (Sy1/N1 - Sy2/N2)^2 / N
	 *	Sxy = Sxy1 + Sxy2 + N1 * N2 * (Sx1/N1 - Sx2/N2) * (Sy1/N1 - Sy2/N2) / N
	 *
	 * 值得单独处理 N1 = 0 与 N2 = 0 这两种特殊情形，
	 * 因为这两种情形很平凡，而且此后在一般情形中我
	 * 们无需担心除以零的错误。
	 *--------------------
	 */
	if (N1 == 0.0)
	{
		N = N2;
		Sx = Sx2;
		Sxx = Sxx2;
		Sy = Sy2;
		Syy = Syy2;
		Sxy = Sxy2;
	}
	else if (N2 == 0.0)
	{
		N = N1;
		Sx = Sx1;
		Sxx = Sxx1;
		Sy = Sy1;
		Syy = Syy1;
		Sxy = Sxy1;
	}
	else
	{
		N = N1 + N2;
		Sx = float8_pl(Sx1, Sx2);
		tmp1 = Sx1 / N1 - Sx2 / N2;
		Sxx = Sxx1 + Sxx2 + N1 * N2 * tmp1 * tmp1 / N;
		if (unlikely(isinf(Sxx)) && !isinf(Sxx1) && !isinf(Sxx2))
			float_overflow_error();
		Sy = float8_pl(Sy1, Sy2);
		tmp2 = Sy1 / N1 - Sy2 / N2;
		Syy = Syy1 + Syy2 + N1 * N2 * tmp2 * tmp2 / N;
		if (unlikely(isinf(Syy)) && !isinf(Syy1) && !isinf(Syy2))
			float_overflow_error();
		Sxy = Sxy1 + Sxy2 + N1 * N2 * tmp1 * tmp2 / N;
		if (unlikely(isinf(Sxy)) && !isinf(Sxy1) && !isinf(Sxy2))
			float_overflow_error();
	}

	/*
	 * 如果我们作为聚合被调用，可以作弊并就地修改第
	 * 一个参数，以减少 palloc 开销。否则我们构造含
	 * 有更新后过渡数据的新数组并返回它。
	 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues1[0] = N;
		transvalues1[1] = Sx;
		transvalues1[2] = Sxx;
		transvalues1[3] = Sy;
		transvalues1[4] = Syy;
		transvalues1[5] = Sxy;

		PG_RETURN_ARRAYTYPE_P(transarray1);
	}
	else
	{
		Datum		transdatums[6];
		ArrayType  *result;

		transdatums[0] = Float8GetDatumFast(N);
		transdatums[1] = Float8GetDatumFast(Sx);
		transdatums[2] = Float8GetDatumFast(Sxx);
		transdatums[3] = Float8GetDatumFast(Sy);
		transdatums[4] = Float8GetDatumFast(Syy);
		transdatums[5] = Float8GetDatumFast(Sxy);

		result = construct_array(transdatums, 6,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

		PG_RETURN_ARRAYTYPE_P(result);
	}
}


Datum
float8_regr_sxx(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx;

	transvalues = check_float8_array(transarray, "float8_regr_sxx", 6);
	N = transvalues[0];
	Sxx = transvalues[2];

	/* 若 N 为 0 应返回 NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* 注意 Sxx 保证非负 */

	PG_RETURN_FLOAT8(Sxx);
}

Datum
float8_regr_syy(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Syy;

	transvalues = check_float8_array(transarray, "float8_regr_syy", 6);
	N = transvalues[0];
	Syy = transvalues[4];

	/* 若 N 为 0 应返回 NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* 注意 Syy 保证非负 */

	PG_RETURN_FLOAT8(Syy);
}

Datum
float8_regr_sxy(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_regr_sxy", 6);
	N = transvalues[0];
	Sxy = transvalues[5];

	/* 若 N 为 0 应返回 NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* 此处结果为负是合法的 */

	PG_RETURN_FLOAT8(Sxy);
}

Datum
float8_regr_avgx(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sx;

	transvalues = check_float8_array(transarray, "float8_regr_avgx", 6);
	N = transvalues[0];
	Sx = transvalues[1];

	/* 若 N 为 0 应返回 NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sx / N);
}

Datum
float8_regr_avgy(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sy;

	transvalues = check_float8_array(transarray, "float8_regr_avgy", 6);
	N = transvalues[0];
	Sy = transvalues[3];

	/* 若 N 为 0 应返回 NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sy / N);
}

Datum
float8_covar_pop(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_covar_pop", 6);
	N = transvalues[0];
	Sxy = transvalues[5];

	/* 若 N 为 0 应返回 NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sxy / N);
}

Datum
float8_covar_samp(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_covar_samp", 6);
	N = transvalues[0];
	Sxy = transvalues[5];

	/* 若 N <= 1 应返回 NULL */
	if (N < 2.0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sxy / (N - 1.0));
}

Datum
float8_corr(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx,
				Syy,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_corr", 6);
	N = transvalues[0];
	Sxx = transvalues[2];
	Syy = transvalues[4];
	Sxy = transvalues[5];

	/* 若 N 为 0 应返回 NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* 注意 Sxx 与 Syy 保证非负 */

	/* 按规范，对水平和垂直线返回 NULL */
	if (Sxx == 0 || Syy == 0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sxy / sqrt(Sxx * Syy));
}

Datum
float8_regr_r2(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx,
				Syy,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_regr_r2", 6);
	N = transvalues[0];
	Sxx = transvalues[2];
	Syy = transvalues[4];
	Sxy = transvalues[5];

	/* 若 N 为 0 应返回 NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* 注意 Sxx 与 Syy 保证非负 */

	/* 按规范，对垂直线返回 NULL */
	if (Sxx == 0)
		PG_RETURN_NULL();

	/* 按规范，对水平线返回 1.0 */
	if (Syy == 0)
		PG_RETURN_FLOAT8(1.0);

	PG_RETURN_FLOAT8((Sxy * Sxy) / (Sxx * Syy));
}

Datum
float8_regr_slope(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sxx,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_regr_slope", 6);
	N = transvalues[0];
	Sxx = transvalues[2];
	Sxy = transvalues[5];

	/* 若 N 为 0 应返回 NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* 注意 Sxx 保证非负 */

	/* 按规范，对垂直线返回 NULL */
	if (Sxx == 0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(Sxy / Sxx);
}

Datum
float8_regr_intercept(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *transvalues;
	float8		N,
				Sx,
				Sxx,
				Sy,
				Sxy;

	transvalues = check_float8_array(transarray, "float8_regr_intercept", 6);
	N = transvalues[0];
	Sx = transvalues[1];
	Sxx = transvalues[2];
	Sy = transvalues[3];
	Sxy = transvalues[5];

	/* 若 N 为 0 应返回 NULL */
	if (N < 1.0)
		PG_RETURN_NULL();

	/* 注意 Sxx 保证非负 */

	/* 按规范，对垂直线返回 NULL */
	if (Sxx == 0)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8((Sy - Sx * Sxy / Sxx) / N);
}


/*
 *		====================================
 *		混合精度算术算子
 *		====================================
 */

/*
 *		float48pl		- 返回 arg1 + arg2
 *		float48mi		- 返回 arg1 - arg2
 *		float48mul		- 返回 arg1 * arg2
 *		float48div		- 返回 arg1 / arg2
 */
Datum
float48pl(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_pl((float8) arg1, arg2));
}

Datum
float48mi(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_mi((float8) arg1, arg2));
}

Datum
float48mul(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_mul((float8) arg1, arg2));
}

Datum
float48div(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_FLOAT8(float8_div((float8) arg1, arg2));
}

/*
 *		float84pl		- 返回 arg1 + arg2
 *		float84mi		- 返回 arg1 - arg2
 *		float84mul		- 返回 arg1 * arg2
 *		float84div		- 返回 arg1 / arg2
 */
Datum
float84pl(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT8(float8_pl(arg1, (float8) arg2));
}

Datum
float84mi(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT8(float8_mi(arg1, (float8) arg2));
}

Datum
float84mul(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT8(float8_mul(arg1, (float8) arg2));
}

Datum
float84div(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_FLOAT8(float8_div(arg1, (float8) arg2));
}

/*
 *		====================
 *		比较算子
 *		====================
 */

/*
 *		float48{eq,ne,lt,le,gt,ge}		- float4/float8 比较运算
 */
Datum
float48eq(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_eq((float8) arg1, arg2));
}

Datum
float48ne(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_ne((float8) arg1, arg2));
}

Datum
float48lt(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_lt((float8) arg1, arg2));
}

Datum
float48le(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_le((float8) arg1, arg2));
}

Datum
float48gt(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_gt((float8) arg1, arg2));
}

Datum
float48ge(PG_FUNCTION_ARGS)
{
	float4		arg1 = PG_GETARG_FLOAT4(0);
	float8		arg2 = PG_GETARG_FLOAT8(1);

	PG_RETURN_BOOL(float8_ge((float8) arg1, arg2));
}

/*
 *		float84{eq,ne,lt,le,gt,ge}		- float8/float4 比较运算
 */
Datum
float84eq(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_eq(arg1, (float8) arg2));
}

Datum
float84ne(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_ne(arg1, (float8) arg2));
}

Datum
float84lt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_lt(arg1, (float8) arg2));
}

Datum
float84le(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_le(arg1, (float8) arg2));
}

Datum
float84gt(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_gt(arg1, (float8) arg2));
}

Datum
float84ge(PG_FUNCTION_ARGS)
{
	float8		arg1 = PG_GETARG_FLOAT8(0);
	float4		arg2 = PG_GETARG_FLOAT4(1);

	PG_RETURN_BOOL(float8_ge(arg1, (float8) arg2));
}

/*
 * 实现 SQL2003 定义的 width_bucket() 函数的 float8
 * 版本。另参见 width_bucket_numeric()。
 *
 * 'bound1' 和 'bound2' 分别是直方图的下界与
 * 上界。'count' 为直方图中的桶数。width_bucket()
 * 返回一个整数，指明 'operand' 在具有上述特征的
 * 等宽直方图所属的桶号。小于下界的操作数被分
 * 配到桶 0。大于等于上界的操作数被分到一个附
 * 加的桶（桶号为 count+1）。我们不
 * 允许任何 float8 输入为 "NaN"，也不允许
 * 直方图的两个边界中的任何一个为
 * +/- 无穷。
 */
Datum
width_bucket_float8(PG_FUNCTION_ARGS)
{
	float8		operand = PG_GETARG_FLOAT8(0);
	float8		bound1 = PG_GETARG_FLOAT8(1);
	float8		bound2 = PG_GETARG_FLOAT8(2);
	int32		count = PG_GETARG_INT32(3);
	int32		result;

	if (count <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
				 errmsg("count must be greater than zero")));

	if (isnan(operand) || isnan(bound1) || isnan(bound2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
				 errmsg("operand, lower bound, and upper bound cannot be NaN")));

	/* 注意我们允许 "operand" 为无穷 */
	if (isinf(bound1) || isinf(bound2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
				 errmsg("lower and upper bounds must be finite")));

	if (bound1 < bound2)
	{
		if (operand < bound1)
			result = 0;
		else if (operand >= bound2)
		{
			if (pg_add_s32_overflow(count, 1, &result))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("integer out of range")));
		}
		else
		{
			if (!isinf(bound2 - bound1))
			{
				/* 商必然在 [0,1] 内，因此不会溢出 */
				result = count * ((operand - bound1) / (bound2 - bound1));
			}
			else
			{
				/*
				 * 若 bound2 - bound1 溢出 DBL_MAX 就会走到这里。
				 * 两边界均有限，其差不会超过两倍的
				 * DBL_MAX；故把全部输入除以 2 即可无溢出地
				 * 完成计算。这亦应精确，除非极小的操作数
				 * 下溢为零。
				 * 这对结果的影响可以忽略不计——鉴于
				 * 边界如此之大。
				 */
				result = count * ((operand / 2 - bound1 / 2) / (bound2 / 2 - bound1 / 2));
			}
			/* 商可能舍入为 1.0，那就是撒谎了 */
			if (result >= count)
				result = count - 1;
			/* 此后即可放心加 1 而不必担心溢出 */
			result++;
		}
	}
	else if (bound1 > bound2)
	{
		if (operand > bound1)
			result = 0;
		else if (operand <= bound2)
		{
			if (pg_add_s32_overflow(count, 1, &result))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("integer out of range")));
		}
		else
		{
			if (!isinf(bound1 - bound2))
				result = count * ((bound1 - operand) / (bound1 - bound2));
			else
				result = count * ((bound1 / 2 - operand / 2) / (bound1 / 2 - bound2 / 2));
			if (result >= count)
				result = count - 1;
			result++;
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
				 errmsg("lower bound cannot equal upper bound")));
		result = 0;				/* 让编译器安静 */
	}

	PG_RETURN_INT32(result);
}
