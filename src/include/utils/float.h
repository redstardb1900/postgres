/*-------------------------------------------------------------------------
 *
 * float.h
 *	  内建浮点类型的定义
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/include/utils/float.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FLOAT_H
#define FLOAT_H

#include <math.h>

/*  X/Open (XSI) 要求 <math.h> 提供 M_PI，但核心 POSIX 不要求  */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*  每度弧度数，即 PI / 180  */
#define RADIANS_PER_DEGREE 0.0174532925199432957692

/*  Visual C++ 等缺 NAN，且不接受 0.0/0.0。  */
#if defined(WIN32) && !defined(NAN)
static const uint32 nan[2] = {0xffffffff, 0x7fffffff};

#define NAN (*(const float8 *) nan)
#endif

extern PGDLLIMPORT int extra_float_digits;

/*
 * float.c 中的实用函数
 */
extern void float_overflow_error(void) pg_attribute_noreturn();
extern void float_underflow_error(void) pg_attribute_noreturn();
extern void float_zero_divide_error(void) pg_attribute_noreturn();
extern int	is_infinite(float8 val);
extern float8 float8in_internal(char *num, char **endptr_p,
								const char *type_name, const char *orig_string,
								struct Node *escontext);
extern float4 float4in_internal(char *num, char **endptr_p,
								const char *type_name, const char *orig_string,
								struct Node *escontext);
extern char *float8out_internal(float8 num);
extern int	float4_cmp_internal(float4 a, float4 b);
extern int	float8_cmp_internal(float8 a, float8 b);

/*
 * 提供相对平台无关的无穷与 NaN 处理例程
 * 见上。
 *
 * 假定 isinf() 与 isnan() 可用且按规范工作。
 * （某些平台须自备；见 src/port。）但首先生成
 * Infinity 或 NaN 的标准化较差；C99 前系统往往
 * 没有 C99 的 INFINITY 与 NaN 宏。变通集中于此。
 * 见上。
 */

/*
 * 两处 #pragma 的奇特放置是因微软编译器的长期
 * 缺陷。详见
 * http://support.microsoft.com/kb/120968/en-us
 */
#ifdef _MSC_VER
#pragma warning(disable:4756)
#endif
static inline float4
get_float4_infinity(void)
{
#ifdef INFINITY
	/*  C99 标准法  */
	return (float4) INFINITY;
#else
#ifdef _MSC_VER
#pragma warning(default:4756)
#endif

	/*
	 * 某些平台上 HUGE_VAL 是无穷，别处只是最大正规
	 * float8。假定强制溢出可得真正无穷。
	 * 见上。
	 */
	return (float4) (HUGE_VAL * HUGE_VAL);
#endif
}

static inline float8
get_float8_infinity(void)
{
#ifdef INFINITY
	/*  C99 标准法  */
	return (float8) INFINITY;
#else

	/*
	 * 某些平台上 HUGE_VAL 是无穷，别处只是最大正规
	 * float8。假定强制溢出可得真正无穷。
	 * 见上。
	 */
	return (float8) (HUGE_VAL * HUGE_VAL);
#endif
}

static inline float4
get_float4_nan(void)
{
#ifdef NAN
	/*  C99 标准法  */
	return (float4) NAN;
#else
	/*  假定可通过零除得到 NAN  */
	return (float4) (0.0 / 0.0);
#endif
}

static inline float8
get_float8_nan(void)
{
	/*  (float8) NAN 在某些 NetBSD/MIPS 上不工作  */
#if defined(NAN) && !(defined(__NetBSD__) && defined(__mips__))
	/*  C99 标准法  */
	return (float8) NAN;
#else
	/*  假定可通过零除得到 NaN  */
	return (float8) (0.0 / 0.0);
#endif
}

/*
 * 把溢出/下溢报告为错误的浮点运算
 *
 * 无法检查加/减的下溢，因为接近下溢值的数已
 * 被舍入到无法检测两值原先不同，例如在 x86 上
 * '1e-45'::float4 == '2e-45'::float4 ==
 * 1.4013e-45。
 * 见上。
 */

static inline float4
float4_pl(const float4 val1, const float4 val2)
{
	float4		result;

	result = val1 + val2;
	if (unlikely(isinf(result)) && !isinf(val1) && !isinf(val2))
		float_overflow_error();

	return result;
}

static inline float8
float8_pl(const float8 val1, const float8 val2)
{
	float8		result;

	result = val1 + val2;
	if (unlikely(isinf(result)) && !isinf(val1) && !isinf(val2))
		float_overflow_error();

	return result;
}

static inline float4
float4_mi(const float4 val1, const float4 val2)
{
	float4		result;

	result = val1 - val2;
	if (unlikely(isinf(result)) && !isinf(val1) && !isinf(val2))
		float_overflow_error();

	return result;
}

static inline float8
float8_mi(const float8 val1, const float8 val2)
{
	float8		result;

	result = val1 - val2;
	if (unlikely(isinf(result)) && !isinf(val1) && !isinf(val2))
		float_overflow_error();

	return result;
}

static inline float4
float4_mul(const float4 val1, const float4 val2)
{
	float4		result;

	result = val1 * val2;
	if (unlikely(isinf(result)) && !isinf(val1) && !isinf(val2))
		float_overflow_error();
	if (unlikely(result == 0.0f) && val1 != 0.0f && val2 != 0.0f)
		float_underflow_error();

	return result;
}

static inline float8
float8_mul(const float8 val1, const float8 val2)
{
	float8		result;

	result = val1 * val2;
	if (unlikely(isinf(result)) && !isinf(val1) && !isinf(val2))
		float_overflow_error();
	if (unlikely(result == 0.0) && val1 != 0.0 && val2 != 0.0)
		float_underflow_error();

	return result;
}

static inline float4
float4_div(const float4 val1, const float4 val2)
{
	float4		result;

	if (unlikely(val2 == 0.0f) && !isnan(val1))
		float_zero_divide_error();
	result = val1 / val2;
	if (unlikely(isinf(result)) && !isinf(val1))
		float_overflow_error();
	if (unlikely(result == 0.0f) && val1 != 0.0f && !isinf(val2))
		float_underflow_error();

	return result;
}

static inline float8
float8_div(const float8 val1, const float8 val2)
{
	float8		result;

	if (unlikely(val2 == 0.0) && !isnan(val1))
		float_zero_divide_error();
	result = val1 / val2;
	if (unlikely(isinf(result)) && !isinf(val1))
		float_overflow_error();
	if (unlikely(result == 0.0) && val1 != 0.0 && !isinf(val2))
		float_underflow_error();

	return result;
}

/*
 * 感知 NaN 的比较例程
 *
 * 我们认为所有 NaNs 相等且大于任何非 NaN。这略任意,
 * 重要的是有一致的排序。
 * 见上
 */

static inline bool
float4_eq(const float4 val1, const float4 val2)
{
	return isnan(val1) ? isnan(val2) : !isnan(val2) && val1 == val2;
}

static inline bool
float8_eq(const float8 val1, const float8 val2)
{
	return isnan(val1) ? isnan(val2) : !isnan(val2) && val1 == val2;
}

static inline bool
float4_ne(const float4 val1, const float4 val2)
{
	return isnan(val1) ? !isnan(val2) : isnan(val2) || val1 != val2;
}

static inline bool
float8_ne(const float8 val1, const float8 val2)
{
	return isnan(val1) ? !isnan(val2) : isnan(val2) || val1 != val2;
}

static inline bool
float4_lt(const float4 val1, const float4 val2)
{
	return !isnan(val1) && (isnan(val2) || val1 < val2);
}

static inline bool
float8_lt(const float8 val1, const float8 val2)
{
	return !isnan(val1) && (isnan(val2) || val1 < val2);
}

static inline bool
float4_le(const float4 val1, const float4 val2)
{
	return isnan(val2) || (!isnan(val1) && val1 <= val2);
}

static inline bool
float8_le(const float8 val1, const float8 val2)
{
	return isnan(val2) || (!isnan(val1) && val1 <= val2);
}

static inline bool
float4_gt(const float4 val1, const float4 val2)
{
	return !isnan(val2) && (isnan(val1) || val1 > val2);
}

static inline bool
float8_gt(const float8 val1, const float8 val2)
{
	return !isnan(val2) && (isnan(val1) || val1 > val2);
}

static inline bool
float4_ge(const float4 val1, const float4 val2)
{
	return isnan(val1) || (!isnan(val2) && val1 >= val2);
}

static inline bool
float8_ge(const float8 val1, const float8 val2)
{
	return isnan(val1) || (!isnan(val2) && val1 >= val2);
}

static inline float4
float4_min(const float4 val1, const float4 val2)
{
	return float4_lt(val1, val2) ? val1 : val2;
}

static inline float8
float8_min(const float8 val1, const float8 val2)
{
	return float8_lt(val1, val2) ? val1 : val2;
}

static inline float4
float4_max(const float4 val1, const float4 val2)
{
	return float4_gt(val1, val2) ? val1 : val2;
}

static inline float8
float8_max(const float8 val1, const float8 val2)
{
	return float8_gt(val1, val2) ? val1 : val2;
}

#endif							/* FLOAT_H */
