/*-------------------------------------------------------------------------
 *
 * int.h
 *	  做整数运算并检查溢出的例程。
 *
 * 本文件例程意在为良定义的 C，不依赖
 * 如 -fwrapv 的编译器标志。
 *
 * 为降低开销，尽量用编译器内建（若有）。
 * 对 16/32 位不那么重要，但 64 位用内建可
 * 明显更快。若无内建则在可用处用 128 位运算。
 * 略
 *
 * Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * src/include/common/int.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMMON_INT_H
#define COMMON_INT_H


/*---------
 * 下列准则适用于全部例程：
 * - 若 a+b 溢出返回 true，否则把 a+b 结果存入
 * *result。溢出时 *result 内容为实现定义。
 * 略
 * - 若 a-b 溢出返回 true，否则把 a-b 结果存入
 * *result。溢出时 *result 内容为实现定义。
 * 略
 * - 若 a*b 溢出返回 true，否则把 a*b 结果存入
 * *result。溢出时 *result 内容为实现定义。
 * 略
 *---------
 */

/*------------------------------------------------------------------------
 * 有符号整数的溢出例程
 *------------------------------------------------------------------------
 */

/*
 * INT16
 */
static inline bool
pg_add_s16_overflow(int16 a, int16 b, int16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_add_overflow(a, b, result);
#else
	int32		res = (int32) a + (int32) b;

	if (res > PG_INT16_MAX || res < PG_INT16_MIN)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = (int16) res;
	return false;
#endif
}

static inline bool
pg_sub_s16_overflow(int16 a, int16 b, int16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(a, b, result);
#else
	int32		res = (int32) a - (int32) b;

	if (res > PG_INT16_MAX || res < PG_INT16_MIN)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = (int16) res;
	return false;
#endif
}

static inline bool
pg_mul_s16_overflow(int16 a, int16 b, int16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_mul_overflow(a, b, result);
#else
	int32		res = (int32) a * (int32) b;

	if (res > PG_INT16_MAX || res < PG_INT16_MIN)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = (int16) res;
	return false;
#endif
}

/*
 * INT32
 */
static inline bool
pg_add_s32_overflow(int32 a, int32 b, int32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_add_overflow(a, b, result);
#else
	int64		res = (int64) a + (int64) b;

	if (res > PG_INT32_MAX || res < PG_INT32_MIN)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = (int32) res;
	return false;
#endif
}

static inline bool
pg_sub_s32_overflow(int32 a, int32 b, int32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(a, b, result);
#else
	int64		res = (int64) a - (int64) b;

	if (res > PG_INT32_MAX || res < PG_INT32_MIN)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = (int32) res;
	return false;
#endif
}

static inline bool
pg_mul_s32_overflow(int32 a, int32 b, int32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_mul_overflow(a, b, result);
#else
	int64		res = (int64) a * (int64) b;

	if (res > PG_INT32_MAX || res < PG_INT32_MIN)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = (int32) res;
	return false;
#endif
}

/*
 * INT64
 */
static inline bool
pg_add_s64_overflow(int64 a, int64 b, int64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_add_overflow(a, b, result);
#elif defined(HAVE_INT128)
	int128		res = (int128) a + (int128) b;

	if (res > PG_INT64_MAX || res < PG_INT64_MIN)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = (int64) res;
	return false;
#else
	if ((a > 0 && b > 0 && a > PG_INT64_MAX - b) ||
		(a < 0 && b < 0 && a < PG_INT64_MIN - b))
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = a + b;
	return false;
#endif
}

static inline bool
pg_sub_s64_overflow(int64 a, int64 b, int64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(a, b, result);
#elif defined(HAVE_INT128)
	int128		res = (int128) a - (int128) b;

	if (res > PG_INT64_MAX || res < PG_INT64_MIN)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = (int64) res;
	return false;
#else
	/*
	 * 注意：a==0 且 b<0 时也可能溢出（具体当
	 * b==PG_INT64_MIN）。
	 */
	if ((a < 0 && b > 0 && a < PG_INT64_MIN + b) ||
		(a >= 0 && b < 0 && a > PG_INT64_MAX + b))
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = a - b;
	return false;
#endif
}

static inline bool
pg_mul_s64_overflow(int64 a, int64 b, int64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_mul_overflow(a, b, result);
#elif defined(HAVE_INT128)
	int128		res = (int128) a * (int128) b;

	if (res > PG_INT64_MAX || res < PG_INT64_MIN)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = (int64) res;
	return false;
#else
	/*
	 * 仅当至少一值落在 sqrt(min)..sqrt(max) 外才可能
	 * 溢出，故先检查；除法可比乘法贵不少。
	 * 略
	 *
	 * 乘 0 或 1 当然不会溢出；单独检查 0 可避免
	 * 除以 0。也当心 INT_MIN/-1；注意交换 a、b
	 * 以确保总是除以正数。
	 * 略
	 *
	 */
	if ((a > PG_INT32_MAX || a < PG_INT32_MIN ||
		 b > PG_INT32_MAX || b < PG_INT32_MIN) &&
		a != 0 && a != 1 && b != 0 && b != 1 &&
		((a > 0 && b > 0 && a > PG_INT64_MAX / b) ||
		 (a > 0 && b < 0 && b < PG_INT64_MIN / a) ||
		 (a < 0 && b > 0 && a < PG_INT64_MIN / b) ||
		 (a < 0 && b < 0 && a < PG_INT64_MAX / b)))
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = a * b;
	return false;
#endif
}

/*------------------------------------------------------------------------
 * 无符号整数的溢出例程
 *------------------------------------------------------------------------
 */

/*
 * UINT16
 */
static inline bool
pg_add_u16_overflow(uint16 a, uint16 b, uint16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_add_overflow(a, b, result);
#else
	uint16		res = a + b;

	if (res < a)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = res;
	return false;
#endif
}

static inline bool
pg_sub_u16_overflow(uint16 a, uint16 b, uint16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(a, b, result);
#else
	if (b > a)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = a - b;
	return false;
#endif
}

static inline bool
pg_mul_u16_overflow(uint16 a, uint16 b, uint16 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_mul_overflow(a, b, result);
#else
	uint32		res = (uint32) a * (uint32) b;

	if (res > PG_UINT16_MAX)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = (uint16) res;
	return false;
#endif
}

/*
 * INT32
 */
static inline bool
pg_add_u32_overflow(uint32 a, uint32 b, uint32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_add_overflow(a, b, result);
#else
	uint32		res = a + b;

	if (res < a)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = res;
	return false;
#endif
}

static inline bool
pg_sub_u32_overflow(uint32 a, uint32 b, uint32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(a, b, result);
#else
	if (b > a)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = a - b;
	return false;
#endif
}

static inline bool
pg_mul_u32_overflow(uint32 a, uint32 b, uint32 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_mul_overflow(a, b, result);
#else
	uint64		res = (uint64) a * (uint64) b;

	if (res > PG_UINT32_MAX)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = (uint32) res;
	return false;
#endif
}

/*
 * UINT64
 */
static inline bool
pg_add_u64_overflow(uint64 a, uint64 b, uint64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_add_overflow(a, b, result);
#else
	uint64		res = a + b;

	if (res < a)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = res;
	return false;
#endif
}

static inline bool
pg_sub_u64_overflow(uint64 a, uint64 b, uint64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_sub_overflow(a, b, result);
#else
	if (b > a)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = a - b;
	return false;
#endif
}

static inline bool
pg_mul_u64_overflow(uint64 a, uint64 b, uint64 *result)
{
#if defined(HAVE__BUILTIN_OP_OVERFLOW)
	return __builtin_mul_overflow(a, b, result);
#elif defined(HAVE_INT128)
	uint128		res = (uint128) a * (uint128) b;

	if (res > PG_UINT64_MAX)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = (uint64) res;
	return false;
#else
	uint64		res = a * b;

	if (a != 0 && b != res / a)
	{
		*result = 0x5EED;		/*  避免伪警告  */
		return true;
	}
	*result = res;
	return false;
#endif
}

#endif							/* COMMON_INT_H */
