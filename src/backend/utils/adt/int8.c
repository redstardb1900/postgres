/*-------------------------------------------------------------------------
 *
 * int8.c
 *	  64 位整数的内部运算。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/int8.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>

#include "common/int.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


typedef struct
{
	int64		current;
	int64		finish;
	int64		step;
} generate_series_fctx;


/***********************************************************************
 **
 **		64 位整数的例程。
 **
 ***********************************************************************/

/*----------------------------------------------------------
 * 格式化与转换例程。
 *---------------------------------------------------------*/

/* int8in()
 */
Datum
int8in(PG_FUNCTION_ARGS)
{
	char	   *num = PG_GETARG_CSTRING(0);

	PG_RETURN_INT64(pg_strtoint64_safe(num, fcinfo->context));
}


/* int8out()
 */
Datum
int8out(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	char		buf[MAXINT8LEN + 1];
	char	   *result;
	int			len;

	len = pg_lltoa(val, buf) + 1;

	/*
	 * 由于长度已知，我们手动 palloc() 与 memcpy()，
	 * 以避免 pstrdup() 中本会发生的 strlen() 调用。
	 */
	result = palloc(len);
	memcpy(result, buf, len);
	PG_RETURN_CSTRING(result);
}

/*
 *		int8recv - 把外部二进制格式转换为 int8
 */
Datum
int8recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INT64(pq_getmsgint64(buf));
}

/*
 *		int8send - 将 int8 转为二进制格式
 */
Datum
int8send(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*----------------------------------------------------------
 *	int8 的关系算子，包括跨数据类型比较。
 *---------------------------------------------------------*/

/* int8relop()
 * val1 relop val2？
 */
Datum
int8eq(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int8ne(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int8lt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int8gt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int8le(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int8ge(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/* int84relop()
 * 64 位 val1 relop 32 位 val2？
 */
Datum
int84eq(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int84ne(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int84lt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int84gt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int84le(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int84ge(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/* int48relop()
 * 32 位 val1 relop 64 位 val2？
 */
Datum
int48eq(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int48ne(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int48lt(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int48gt(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int48le(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int48ge(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/* int82relop()
 * 64 位 val1 relop 16 位 val2？
 */
Datum
int82eq(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int82ne(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int82lt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int82gt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int82le(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int82ge(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/* int28relop()
 * 16 位 val1 relop 64 位 val2？
 */
Datum
int28eq(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int28ne(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int28lt(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int28gt(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int28le(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int28ge(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/*
 * int8 的 in_range 支持函数。
 *
 * 注意：无须提供 int8_int4 或 int8_int2 变体，
 * 因为偏移值的隐式转换同样能应对那些场景。
 */
Datum
in_range_int8_int8(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	int64		base = PG_GETARG_INT64(1);
	int64		offset = PG_GETARG_INT64(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	int64		sum;

	if (offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	if (sub)
		offset = -offset;		/* 不会溢出 */

	if (unlikely(pg_add_s64_overflow(base, offset, &sum)))
	{
		/*
		 * 若 sub 为假，真实和必然大于 val，因此正确
		 * 答案与 "less" 相同。若 sub 为真，真实和必
		 * 然小于 val，因此答案为 "!less"。
		 */
		PG_RETURN_BOOL(sub ? !less : less);
	}

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}


/*----------------------------------------------------------
 *	64 位整数的算术算子。
 *---------------------------------------------------------*/

Datum
int8um(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);
	int64		result;

	if (unlikely(arg == PG_INT64_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	result = -arg;
	PG_RETURN_INT64(result);
}

Datum
int8up(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);

	PG_RETURN_INT64(arg);
}

Datum
int8pl(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_add_s64_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int8mi(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_sub_s64_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int8mul(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_mul_s64_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int8div(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (arg2 == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器意识到我们绝不能到达除法（gcc bug） */
		PG_RETURN_NULL();
	}

	/*
	 * INT64_MIN / -1 有问题，因为结果无法在
	 * 二进制补码机器上表示。有的机器产生
	 * INT64_MIN，有的产生零，有的抛出异常。
	 * 用取负代替除以 -1 即可规避此问题。
	 */
	if (arg2 == -1)
	{
		if (unlikely(arg1 == PG_INT64_MIN))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));
		result = -arg1;
		PG_RETURN_INT64(result);
	}

	/* 不可能溢出 */

	result = arg1 / arg2;

	PG_RETURN_INT64(result);
}

/* int8abs()
 * 绝对值
 */
Datum
int8abs(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		result;

	if (unlikely(arg1 == PG_INT64_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	result = (arg1 < 0) ? -arg1 : arg1;
	PG_RETURN_INT64(result);
}

/* int8mod()
 * 取模运算。
 */
Datum
int8mod(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器意识到我们绝不能到达除法（gcc bug） */
		PG_RETURN_NULL();
	}

	/*
	 * 有的机器对 INT64_MIN % -1 抛出浮点异常，
	 * 这有点滑稽，因为正确答案其实是完全
	 * 确定的，即零。
	 */
	if (arg2 == -1)
		PG_RETURN_INT64(0);

	/* 不可能溢出 */

	PG_RETURN_INT64(arg1 % arg2);
}

/*
 * 最大公约数
 *
 * 返回能整除两个输入的最大正整数。
 * 特例：
 *   - gcd(x, 0) = gcd(0, x) = abs(x)
 *   		因为 0 能被任何数整除
 *   - gcd(0, 0) = 0
 *   		符合前一定义，也是通行惯例
 *
 * 若任一输入为 INT64_MIN 则须特别当心 ---
 * gcd(0, INT64_MIN)、gcd(INT64_MIN, 0) 与
 * gcd(INT64_MIN, INT64_MIN) 都等于 abs(INT64_MIN)，无法表示成 64 位
 * 整数
 */
static int64
int8gcd_internal(int64 arg1, int64 arg2)
{
	int64		swap;
	int64		a1,
				a2;

	/*
	 * 把较大的绝对值放入 arg1。
	 *
	 * 下面的循环本来会自动做到这一点，但这样可以
	 * 避免昂贵的取模运算，并简化下方对 INT64_MIN
	 * 的特例处理。
	 *
	 * 为了处理 INT64_MIN，我们在负数空间做此事。
	 */
	a1 = (arg1 < 0) ? arg1 : -arg1;
	a2 = (arg2 < 0) ? arg2 : -arg2;
	if (a1 > a2)
	{
		swap = arg1;
		arg1 = arg2;
		arg2 = swap;
	}

	/* INT64_MIN 需要特别当心。见上方注释。 */
	if (arg1 == PG_INT64_MIN)
	{
		if (arg2 == 0 || arg2 == PG_INT64_MIN)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));

		/*
		 * 有的机器对 INT64_MIN % -1 抛出浮点异常，
		 * 这有点滑稽，因为正确答案其实是完全
		 * 确定的，即零。防范这种情况，直接返回结果
		 * gcd(INT64_MIN, -1) = 1。
		 */
		if (arg2 == -1)
			return 1;
	}

	/* 用欧几里得算法求 GCD */
	while (arg2 != 0)
	{
		swap = arg2;
		arg2 = arg1 % arg2;
		arg1 = swap;
	}

	/*
	 * 确保结果为正。（我们知道已不是 INT64_MIN
	 * 了）。
	 */
	if (arg1 < 0)
		arg1 = -arg1;

	return arg1;
}

Datum
int8gcd(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = int8gcd_internal(arg1, arg2);

	PG_RETURN_INT64(result);
}

/*
 * 最小公倍数
 */
Datum
int8lcm(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		gcd;
	int64		result;

	/*
	 * 把 lcm(x, 0) = lcm(0, x) = 0 作为特例
	 * 处理。这防止了 x 为零时下方出现除零错误，以及
	 * x = INT64_MIN 时 GCD 溢出错误。
	 */
	if (arg1 == 0 || arg2 == 0)
		PG_RETURN_INT64(0);

	/* lcm(x, y) = abs(x / gcd(x, y) * y) */
	gcd = int8gcd_internal(arg1, arg2);
	arg1 = arg1 / gcd;

	if (unlikely(pg_mul_s64_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	/* 若结果为 INT64_MIN，则无法表示。 */
	if (unlikely(result == PG_INT64_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	if (result < 0)
		result = -result;

	PG_RETURN_INT64(result);
}

Datum
int8inc(PG_FUNCTION_ARGS)
{
	/*
	 * 当 int8 为按引用传递时，我们提供此特例来避免
	 * COUNT() 的 palloc 开销：作为聚合被调用时，
	 * 我们知道参数是可修改的本地存储，因此
	 * 直接原地更新即可。（若 int8 按值传递则
	 * 错误且无用，不编译。）
	 */
#ifndef USE_FLOAT8_BYVAL		/* 兼控 int8 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		int64	   *arg = (int64 *) PG_GETARG_POINTER(0);

		if (unlikely(pg_add_s64_overflow(*arg, 1, arg)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));

		PG_RETURN_POINTER(arg);
	}
	else
#endif
	{
		/* 不是作为聚合调用，因此直接用笨办法 */
		int64		arg = PG_GETARG_INT64(0);
		int64		result;

		if (unlikely(pg_add_s64_overflow(arg, 1, &result)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));

		PG_RETURN_INT64(result);
	}
}

Datum
int8dec(PG_FUNCTION_ARGS)
{
	/*
	 * 当 int8 为按引用传递时，我们提供此特例来避免
	 * COUNT() 的 palloc 开销：作为聚合被调用时，
	 * 我们知道参数是可修改的本地存储，因此
	 * 直接原地更新即可。（若 int8 按值传递则
	 * 错误且无用，不编译。）
	 */
#ifndef USE_FLOAT8_BYVAL		/* 兼控 int8 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		int64	   *arg = (int64 *) PG_GETARG_POINTER(0);

		if (unlikely(pg_sub_s64_overflow(*arg, 1, arg)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));
		PG_RETURN_POINTER(arg);
	}
	else
#endif
	{
		/* 不是作为聚合调用，因此直接用笨办法 */
		int64		arg = PG_GETARG_INT64(0);
		int64		result;

		if (unlikely(pg_sub_s64_overflow(arg, 1, &result)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));

		PG_RETURN_INT64(result);
	}
}


/*
 * 这些函数与 int8inc/int8dec 完全相同，但
 * 用于只统计非 NULL 值的聚合。这些函数声明为
 * strict，所以 NULL 检查在我们到达之前就已
 * 发生，要做的只是递增状态值。本来就能把
 * 这些 pg_proc 条目直接指向 int8inc/int8dec，
 * 但 opr_sanity 回归测试会抱怨内建函数条目不匹配。
 */

Datum
int8inc_any(PG_FUNCTION_ARGS)
{
	return int8inc(fcinfo);
}

Datum
int8inc_float8_float8(PG_FUNCTION_ARGS)
{
	return int8inc(fcinfo);
}

Datum
int8dec_any(PG_FUNCTION_ARGS)
{
	return int8dec(fcinfo);
}

/*
 * int8inc_support
 *		int8inc() 与 int8inc_any() 的 prosupport 函数
 */
Datum
int8inc_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestWFuncMonotonic))
	{
		SupportRequestWFuncMonotonic *req = (SupportRequestWFuncMonotonic *) rawreq;
		MonotonicFunction monotonic = MONOTONICFUNC_NONE;
		int			frameOptions = req->window_clause->frameOptions;
		WindowFunc *wfunc = req->window_func;

		if (list_length(wfunc->args) == 1)
		{
			Node *expr = eval_const_expressions(NULL, linitial(wfunc->args));

			/*
			 * 由于 v17 之前版本中 WindowClause runCondition
			 * 的 Node 表示形式，我们必须坚持 count 参数
			 * 为 Const 才能安全地应用 runCondition
			 * 优化。
			 */
			if (!IsA(expr, Const))
				PG_RETURN_POINTER(NULL);
		}

		/* 无 ORDER BY 则所有行为同伴 */
		if (req->window_clause->orderClause == NIL)
			monotonic = MONOTONICFUNC_BOTH;
		else
		{
			/*
			 * 否则要把帧选项考虑在内。当帧边界为
			 * 窗口起点时，结果值就绝不会减小，因此
			 * 是单调递增的
			 */
			if (frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING)
				monotonic |= MONOTONICFUNC_INCREASING;

			/*
			 * 同样，如果帧边界为窗口终点，那么结果
			 * 值也绝不会减小。
			 */
			if (frameOptions & FRAMEOPTION_END_UNBOUNDED_FOLLOWING)
				monotonic |= MONOTONICFUNC_DECREASING;
		}

		req->monotonic = monotonic;
		PG_RETURN_POINTER(req);
	}

	PG_RETURN_POINTER(NULL);
}


Datum
int8larger(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = ((arg1 > arg2) ? arg1 : arg2);

	PG_RETURN_INT64(result);
}

Datum
int8smaller(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = ((arg1 < arg2) ? arg1 : arg2);

	PG_RETURN_INT64(result);
}

Datum
int84pl(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int64		result;

	if (unlikely(pg_add_s64_overflow(arg1, (int64) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int84mi(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int64		result;

	if (unlikely(pg_sub_s64_overflow(arg1, (int64) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int84mul(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int64		result;

	if (unlikely(pg_mul_s64_overflow(arg1, (int64) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int84div(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int64		result;

	if (arg2 == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器意识到我们绝不能到达除法（gcc bug） */
		PG_RETURN_NULL();
	}

	/*
	 * INT64_MIN / -1 有问题，因为结果无法在
	 * 二进制补码机器上表示。有的机器产生
	 * INT64_MIN，有的产生零，有的抛出异常。
	 * 用取负代替除以 -1 即可规避此问题。
	 */
	if (arg2 == -1)
	{
		if (unlikely(arg1 == PG_INT64_MIN))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));
		result = -arg1;
		PG_RETURN_INT64(result);
	}

	/* 不可能溢出 */

	result = arg1 / arg2;

	PG_RETURN_INT64(result);
}

Datum
int48pl(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_add_s64_overflow((int64) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int48mi(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_sub_s64_overflow((int64) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int48mul(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_mul_s64_overflow((int64) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int48div(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int64		arg2 = PG_GETARG_INT64(1);

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器意识到我们绝不能到达除法（gcc bug） */
		PG_RETURN_NULL();
	}

	/* 不可能溢出 */
	PG_RETURN_INT64((int64) arg1 / arg2);
}

Datum
int82pl(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int64		result;

	if (unlikely(pg_add_s64_overflow(arg1, (int64) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int82mi(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int64		result;

	if (unlikely(pg_sub_s64_overflow(arg1, (int64) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int82mul(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int64		result;

	if (unlikely(pg_mul_s64_overflow(arg1, (int64) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int82div(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int64		result;

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器意识到我们绝不能到达除法（gcc bug） */
		PG_RETURN_NULL();
	}

	/*
	 * INT64_MIN / -1 有问题，因为结果无法在
	 * 二进制补码机器上表示。有的机器产生
	 * INT64_MIN，有的产生零，有的抛出异常。
	 * 用取负代替除以 -1 即可规避此问题。
	 */
	if (arg2 == -1)
	{
		if (unlikely(arg1 == PG_INT64_MIN))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));
		result = -arg1;
		PG_RETURN_INT64(result);
	}

	/* 不可能溢出 */

	result = arg1 / arg2;

	PG_RETURN_INT64(result);
}

Datum
int28pl(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_add_s64_overflow((int64) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int28mi(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_sub_s64_overflow((int64) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int28mul(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_mul_s64_overflow((int64) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int28div(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int64		arg2 = PG_GETARG_INT64(1);

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器意识到我们绝不能到达除法（gcc bug） */
		PG_RETURN_NULL();
	}

	/* 不可能溢出 */
	PG_RETURN_INT64((int64) arg1 / arg2);
}

/* 二进制算术
 *
 *		int8and		- 返回 arg1 & arg2
 *		int8or		- 返回 arg1 | arg2
 *		int8xor		- 返回 arg1 # arg2
 *		int8not		- 返回 ~arg1
 *		int8shl		- 返回 arg1 << arg2
 *		int8shr		- 返回 arg1 >> arg2
 */

Datum
int8and(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(arg1 & arg2);
}

Datum
int8or(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(arg1 | arg2);
}

Datum
int8xor(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(arg1 ^ arg2);
}

Datum
int8not(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);

	PG_RETURN_INT64(~arg1);
}

Datum
int8shl(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT64(arg1 << arg2);
}

Datum
int8shr(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT64(arg1 >> arg2);
}

/*----------------------------------------------------------
 *	转换算子。
 *---------------------------------------------------------*/

Datum
int48(PG_FUNCTION_ARGS)
{
	int32		arg = PG_GETARG_INT32(0);

	PG_RETURN_INT64((int64) arg);
}

Datum
int84(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);

	if (unlikely(arg < PG_INT32_MIN) || unlikely(arg > PG_INT32_MAX))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32((int32) arg);
}

Datum
int28(PG_FUNCTION_ARGS)
{
	int16		arg = PG_GETARG_INT16(0);

	PG_RETURN_INT64((int64) arg);
}

Datum
int82(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);

	if (unlikely(arg < PG_INT16_MIN) || unlikely(arg > PG_INT16_MAX))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16((int16) arg);
}

Datum
i8tod(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);
	float8		result;

	result = arg;

	PG_RETURN_FLOAT8(result);
}

/* dtoi8()
 * 把 float8 转为 8 字节整数
 */
Datum
dtoi8(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);

	/*
	 * 去除输入中的所有小数部分。这样做是为了
	 * 不因略微超范围、舍入后却能落回范围内的
	 * 值上失败。注意假定 NaN/Inf 经 rint() 透传。
	 */
	num = rint(num);

	/*范围检查*/
	if (unlikely(isnan(num) || !FLOAT8_FITS_IN_INT64(num)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	PG_RETURN_INT64((int64) num);
}

Datum
i8tof(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);
	float4		result;

	result = arg;

	PG_RETURN_FLOAT4(result);
}

/* ftoi8()
 * 把 float4 转为 8 字节整数
 */
Datum
ftoi8(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);

	/*
	 * 去除输入中的所有小数部分。这样做是为了
	 * 不因略微超范围、舍入后却能落回范围内的
	 * 值上失败。注意假定 NaN/Inf 经 rint() 透传。
	 */
	num = rint(num);

	/*范围检查*/
	if (unlikely(isnan(num) || !FLOAT4_FITS_IN_INT64(num)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	PG_RETURN_INT64((int64) num);
}

Datum
i8tooid(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);

	if (unlikely(arg < 0) || unlikely(arg > PG_UINT32_MAX))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("OID out of range")));

	PG_RETURN_OID((Oid) arg);
}

Datum
oidtoi8(PG_FUNCTION_ARGS)
{
	Oid			arg = PG_GETARG_OID(0);

	PG_RETURN_INT64((int64) arg);
}

/*
 * 非持久数值序列生成器
 */
Datum
generate_series_int8(PG_FUNCTION_ARGS)
{
	return generate_series_step_int8(fcinfo);
}

Datum
generate_series_step_int8(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	generate_series_fctx *fctx;
	int64		result;
	MemoryContext oldcontext;

	/* 只在函数首次调用时做的事 */
	if (SRF_IS_FIRSTCALL())
	{
		int64		start = PG_GETARG_INT64(0);
		int64		finish = PG_GETARG_INT64(1);
		int64		step = 1;

		/* 看看是否显式给了步长 */
		if (PG_NARGS() == 3)
			step = PG_GETARG_INT64(2);
		if (step == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot equal zero")));

		/* 创建跨调用持久化的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * 切换到适合多次函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* 为用户上下文分配内存 */
		fctx = (generate_series_fctx *) palloc(sizeof(generate_series_fctx));

		/*
		 * 用 fctx 在多次调用间保存状态。用原始
		 * 值初始化 current
		 */
		fctx->current = start;
		fctx->finish = finish;
		fctx->step = step;

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* 每次调用函数时都要做的事 */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * 取回保存的状态，current 即本轮迭代的结果
	 */
	fctx = funcctx->user_fctx;
	result = fctx->current;

	if ((fctx->step > 0 && fctx->current <= fctx->finish) ||
		(fctx->step < 0 && fctx->current >= fctx->finish))
	{
		/*
		 * 递增 current 为下一轮迭代做准备。若下值
		 * 计算溢出，则这就是最终结果。
		 */
		if (pg_add_s64_overflow(fctx->current, fctx->step, &fctx->current))
			fctx->step = 0;

		/* 还有剩余要发送时执行 */
		SRF_RETURN_NEXT(funcctx, Int64GetDatum(result));
	}
	else
		/* 没有剩余时执行 */
		SRF_RETURN_DONE(funcctx);
}

/*
 * generate_series(int8, int8 [, int8]) 的规划器支持函数
 */
Datum
generate_series_int8_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestRows))
	{
		/* 尝试估计将返回的行数 */
		SupportRequestRows *req = (SupportRequestRows *) rawreq;

		if (is_funcclause(req->node))	/* 稳妥 */
		{
			List	   *args = ((FuncExpr *) req->node)->args;
			Node	   *arg1,
					   *arg2,
					   *arg3;

			/* 这里可以使用估算的参数值 */
			arg1 = estimate_expression_value(req->root, linitial(args));
			arg2 = estimate_expression_value(req->root, lsecond(args));
			if (list_length(args) >= 3)
				arg3 = estimate_expression_value(req->root, lthird(args));
			else
				arg3 = NULL;

			/*
			 * 如果任一参数是常量 NULL，则可以放心地
			 * 假定返回零行。否则，如果它们都是非 NULL
			 * 常量，就能算出将返回的行数。使用
			 * double 运算以避免溢出风险。
			 */
			if ((IsA(arg1, Const) &&
				 ((Const *) arg1)->constisnull) ||
				(IsA(arg2, Const) &&
				 ((Const *) arg2)->constisnull) ||
				(arg3 != NULL && IsA(arg3, Const) &&
				 ((Const *) arg3)->constisnull))
			{
				req->rows = 0;
				ret = (Node *) req;
			}
			else if (IsA(arg1, Const) &&
					 IsA(arg2, Const) &&
					 (arg3 == NULL || IsA(arg3, Const)))
			{
				double		start,
							finish,
							step;

				start = DatumGetInt64(((Const *) arg1)->constvalue);
				finish = DatumGetInt64(((Const *) arg2)->constvalue);
				step = arg3 ? DatumGetInt64(((Const *) arg3)->constvalue) : 1;

				/* 该等式对步长的任一符号都适用 */
				if (step != 0)
				{
					req->rows = floor((finish - start + step) / step);
					ret = (Node *) req;
				}
			}
		}
	}

	PG_RETURN_POINTER(ret);
}
