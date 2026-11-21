/*-------------------------------------------------------------------------
 *
 * int128.h
 *	  自研 128 位整数运算。
 *
 * 若有原生 int128 则用之，否则基于两个
 * int64 半部硬实现。
 *
 * See src/tools/testint128.c for a simple test harness for this file.
 *
 * Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * src/include/common/int128.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INT128_H
#define INT128_H

/*
 * 测试时可预定义 USE_NATIVE_INT128 开关原生
 * int128。
 */
#ifndef USE_NATIVE_INT128
#ifdef HAVE_INT128
#define USE_NATIVE_INT128 1
#else
#define USE_NATIVE_INT128 0
#endif
#endif


#if USE_NATIVE_INT128

typedef int128 INT128;

/*
 * 把无符号 int64 加到 INT128 变量。
 */
static inline void
int128_add_uint64(INT128 *i128, uint64 v)
{
	*i128 += v;
}

/*
 * 把有符号 int64 加到 INT128 变量。
 */
static inline void
int128_add_int64(INT128 *i128, int64 v)
{
	*i128 += v;
}

/*
 * 把两个 int64 的 128 位积加到 INT128 变量。
 *
 * XXX 笨编译器下可能比另一实现更慢；或许应
 * 总是手写？
 */
static inline void
int128_add_int64_mul_int64(INT128 *i128, int64 x, int64 y)
{
	*i128 += (int128) x * (int128) y;
}

/*
 * 比较两 INT128，返回 -1、0 或 +1。
 */
static inline int
int128_compare(INT128 x, INT128 y)
{
	if (x < y)
		return -1;
	if (x > y)
		return 1;
	return 0;
}

/*
 * int64→INT128。
 */
static inline INT128
int64_to_int128(int64 v)
{
	return (INT128) v;
}

/*
 * INT128 转为 int64（丢掉高位）。
 * 转 uint64 亦可。
 */
static inline int64
int128_to_int64(INT128 val)
{
	return (int64) val;
}

#else							/* !USE_NATIVE_INT128 */

/*
 * INT128 布局与原生 int128（大概）相同内容与
 * 字节序。普通使用无差别，但可与 int128 联合
 * 以便测试。
 * 略
 */
typedef struct
{
#ifdef WORDS_BIGENDIAN
	int64		hi;				/*  最高 64 位，含符号  */
	uint64		lo;				/*  最低 64 位，无符号  */
#else
	uint64		lo;				/*  最低 64 位，无符号  */
	int64		hi;				/*  最高 64 位，含符号  */
#endif
} INT128;

/*
 * 把无符号 int64 加到 INT128 变量。
 */
static inline void
int128_add_uint64(INT128 *i128, uint64 v)
{
	/*
	 * 先加到 .lo，再看是否需向 .hi 进位。两输入皆
	 * 高位置位，或仅一输入高位置位而新 .lo 未置，
	 * 则需进位。.lo 无符号；这里转为有符号只为
	 * 便宜地查高位。
	 * 略
	 */
	uint64		oldlo = i128->lo;

	i128->lo += v;
	if (((int64) v < 0 && (int64) oldlo < 0) ||
		(((int64) v < 0 || (int64) oldlo < 0) && (int64) i128->lo >= 0))
		i128->hi++;
}

/*
 * 把有符号 int64 加到 INT128 变量。
 */
static inline void
int128_add_int64(INT128 *i128, int64 v)
{
	/*
	 * 与上类似，但负 v 时进位逻辑不同。通常须从
	 * .hi 减 1（对应把 v 符号扩展位加上）；若 .lo
	 * 有进位则抵消，什么也不做。
	 * 略
	 */
	uint64		oldlo = i128->lo;

	i128->lo += v;
	if (v >= 0)
	{
		if ((int64) oldlo < 0 && (int64) i128->lo >= 0)
			i128->hi++;
	}
	else
	{
		if (!((int64) oldlo < 0 || (int64) i128->lo >= 0))
			i128->hi--;
	}
}

/*
 * INT64_AU32 取 int64 最高 32 位为 int64，
 * INT64_AL32 取最低 32 位为 uint64。
 */
#define INT64_AU32(i64) ((i64) >> 32)
#define INT64_AL32(i64) ((i64) & UINT64CONST(0xFFFFFFFF))

/*
 * 把两个 int64 的 128 位积加到 INT128 变量。
 */
static inline void
int128_add_int64_mul_int64(INT128 *i128, int64 x, int64 y)
{
	/*  INT64_AU32 须用算术右移  */
	StaticAssertDecl(((int64) -1 >> 1) == (int64) -1,
					 "arithmetic right shift is needed");

	/*----------
	 * 用 64 位运算形成 128 位积 x*y。
	 * 把各 64 位输入视为 32 位高/低部，可算：
	 * 略
	 *
	 *	 x * y = ((x.hi << 32) + x.lo) * (((y.hi << 32) + y.lo)
	 *		   = (x.hi * y.hi) << 64 +
	 *			 (x.hi * y.lo) << 32 +
	 *			 (x.lo * y.hi) << 32 +
	 *			 x.lo * y.lo
	 *
	 * 各积为 32 位项，64 位运算下不会溢出。再移位
	 * 到正确位置加进 128 位结果。还须把 "lo" 部
	 * 当作无符号。
	 * 略
	 *----------
	 */

	/*  积必为零则不必费力  */
	if (x != 0 && y != 0)
	{
		int64		x_u32 = INT64_AU32(x);
		uint64		x_l32 = INT64_AL32(x);
		int64		y_u32 = INT64_AU32(y);
		uint64		y_l32 = INT64_AL32(y);
		int64		tmp;

		/*  第一项  */
		i128->hi += x_u32 * y_u32;

		/*  第二项：仅当 x 为负时符号扩展  */
		tmp = x_u32 * y_l32;
		if (x < 0)
			i128->hi += INT64_AU32(tmp);
		else
			i128->hi += ((uint64) tmp) >> 32;
		int128_add_uint64(i128, ((uint64) INT64_AL32(tmp)) << 32);

		/*  第三项：仅当 y 为负时符号扩展  */
		tmp = x_l32 * y_u32;
		if (y < 0)
			i128->hi += INT64_AU32(tmp);
		else
			i128->hi += ((uint64) tmp) >> 32;
		int128_add_uint64(i128, ((uint64) INT64_AL32(tmp)) << 32);

		/*  第四项：总是无符号  */
		int128_add_uint64(i128, x_l32 * y_l32);
	}
}

/*
 * 比较两 INT128，返回 -1、0 或 +1。
 */
static inline int
int128_compare(INT128 x, INT128 y)
{
	if (x.hi < y.hi)
		return -1;
	if (x.hi > y.hi)
		return 1;
	if (x.lo < y.lo)
		return -1;
	if (x.lo > y.lo)
		return 1;
	return 0;
}

/*
 * int64→INT128。
 */
static inline INT128
int64_to_int128(int64 v)
{
	INT128		val;

	val.lo = (uint64) v;
	val.hi = (v < 0) ? -INT64CONST(1) : INT64CONST(0);
	return val;
}

/*
 * INT128 转为 int64（丢掉高位）。
 * 转 uint64 亦可。
 */
static inline int64
int128_to_int64(INT128 val)
{
	return (int64) val.lo;
}

#endif							/* USE_NATIVE_INT128 */

#endif							/* INT128_H */
