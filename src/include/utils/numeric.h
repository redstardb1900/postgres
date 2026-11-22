/*-------------------------------------------------------------------------
 *
 * numeric.h
 *	  Postgres 精确数值数据类型的定义
 *
 * 最初编码 1998，Jan Wieck。2003 由 Tom Lane 大幅修订。
 *
 * Copyright (c) 1998-2023, PostgreSQL Global Development Group
 *
 * src/include/utils/numeric.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_NUMERIC_H_
#define _PG_NUMERIC_H_

#include "fmgr.h"

/*
 * NUMERIC typmod 中可指定的精度与标度上限。精度
 * 须严格为正，标度可为正或负。负标度意味在小数点
 * 前舍入。
 *
 * 注意下方最小显示标度为零——即使标度为负，
 * 我们也总显示小数点前的全部数字。
 * 见上。
 *
 * 注意数值的精度与显示标度的实现上限要大得多——
 * 使用这些常量时当心！
 */
#define NUMERIC_MAX_PRECISION		1000

#define NUMERIC_MIN_SCALE			(-1000)
#define NUMERIC_MAX_SCALE			1000

/*
 * 为计算结果所选标度的内部限制
 */
#define NUMERIC_MAX_DISPLAY_SCALE	NUMERIC_MAX_PRECISION
#define NUMERIC_MIN_DISPLAY_SCALE	0

#define NUMERIC_MAX_RESULT_SCALE	(NUMERIC_MAX_PRECISION * 2)

/*
 * 对除法与平方根等固有不精确计算，我们尽量至少
 * 得到这么多有效数字；意图是结果不差于 float8。
 * 见上。
 */
#define NUMERIC_MIN_SIG_DIGITS		16

/* Numeric 的实际内容对 numeric.c 私有 */
struct NumericData;
typedef struct NumericData *Numeric;

/*
 * fmgr 接口宏
 */

static inline Numeric
DatumGetNumeric(Datum X)
{
	return (Numeric) PG_DETOAST_DATUM(X);
}

static inline Numeric
DatumGetNumericCopy(Datum X)
{
	return (Numeric) PG_DETOAST_DATUM_COPY(X);
}

static inline Datum
NumericGetDatum(Numeric X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_NUMERIC(n)	  DatumGetNumeric(PG_GETARG_DATUM(n))
#define PG_GETARG_NUMERIC_COPY(n) DatumGetNumericCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_NUMERIC(x)	  return NumericGetDatum(x)

/*
 * numeric.c 中的实用函数
 */
extern bool numeric_is_nan(Numeric num);
extern bool numeric_is_inf(Numeric num);
extern int32 numeric_maximum_size(int32 typmod);
extern char *numeric_out_sci(Numeric num, int scale);
extern char *numeric_normalize(Numeric num);

extern Numeric int64_to_numeric(int64 val);
extern Numeric int64_div_fast_to_numeric(int64 val1, int log10val2);

extern Numeric numeric_add_opt_error(Numeric num1, Numeric num2,
									 bool *have_error);
extern Numeric numeric_sub_opt_error(Numeric num1, Numeric num2,
									 bool *have_error);
extern Numeric numeric_mul_opt_error(Numeric num1, Numeric num2,
									 bool *have_error);
extern Numeric numeric_div_opt_error(Numeric num1, Numeric num2,
									 bool *have_error);
extern Numeric numeric_mod_opt_error(Numeric num1, Numeric num2,
									 bool *have_error);
extern int32 numeric_int4_opt_error(Numeric num, bool *have_error);

#endif							/* _PG_NUMERIC_H_ */
