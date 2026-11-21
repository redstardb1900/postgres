/*-------------------------------------------------------------------------
 *
 * instr_time.h
 *	  可移植高精度区间计时
 *
 * 本文件提供抽象层，隐藏区间计时的
 * 可移植问题。Unix 用 clock_gettime()，Windows 用
 * QueryPerformanceCounter()。这些宏也为改用其他
 * 高精度计时 API 留出余地。
 *
 * 基本数据类型为 instr_time，调用者应视其为
 * 不透明 typedef。instr_time 可存绝对时间（参考点
 * 未指定）或区间。为其提供的操作
 * 有：
 *
 * INSTR_TIME_IS_ZERO(t)			t 是否为零？
 *
 * INSTR_TIME_SET_ZERO(t)			将 t 置零（也可用 memset）
 *
 * INSTR_TIME_SET_CURRENT(t)		设 t 为当前时间
 *
 * INSTR_TIME_SET_CURRENT_LAZY(t)	若 t 为零则取当前时间，
 *									求值结果为 t 是否改变
 *
 * INSTR_TIME_ADD(x, y)				x += y
 *
 * INSTR_TIME_SUBTRACT(x, y)		x -= y
 *
 * INSTR_TIME_ACCUM_DIFF(x, y, z)	x += (y - z)
 *
 * INSTR_TIME_GET_DOUBLE(t)			将 t 转为 double（秒）
 *
 * INSTR_TIME_GET_MILLISEC(t)		将 t 转为 double（毫秒）
 *
 * INSTR_TIME_GET_MICROSEC(t)		将 t 转为 uint64（微秒）
 *
 * INSTR_TIME_GET_NANOSEC(t)		将 t 转为 uint64（纳秒）
 *
 * 注意 INSTR_TIME_SUBTRACT 与 INSTR_TIME_ACCUM_DIFF 会把
 * 绝对时间转为区间。INSTR_TIME_GET_xxx 操作
 * 仅对区间有用。
 *
 * 累加多次测量时，建议将运行总和
 * 保持为 instr_time 形式（即用 INSTR_TIME_ADD 或
 * INSTR_TIME_ACCUM_DIFF），仅在最后转为结果格式。
 *
 * 当心宏参数被多次求值。
 *
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * src/include/portability/instr_time.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INSTR_TIME_H
#define INSTR_TIME_H


/*
 * 所有平台上将区间时间存为 int64，因 int64 的
 * 加/减便宜，而这是 instr_time 最常见操作。取时
 * 及换算到具体时间单位则依平台
 * 而定。
 *
 * 为免 API 用户依赖整数表示，我们将
 * 64 位整数包进结构。
 */
typedef struct instr_time
{
	int64		ticks;			/*  以平台相关单位  */
} instr_time;


/*  下列平台相关代码所用的辅助宏  */

#define NS_PER_S	INT64CONST(1000000000)
#define NS_PER_MS	INT64CONST(1000000)
#define NS_PER_US	INT64CONST(1000)


#ifndef WIN32


/*  clock_gettime  */

#include <time.h>

/*
 * 按 POSIX 规范，最佳 clockid 是 CLOCK_MONOTONIC，
 * 因其即使系统时钟改变也能给出可靠区间计时。
 * 但 POSIX 不要求实现提供 CLOCK_MONOTONIC，
 * 除 CLOCK_REALTIME 外皆可选，故找不到时回退到
 * CLOCK_REALTIME。
 *
 * 另有些实现有性质更好的非标准 clockid。
 * 尤其自 macOS 10.12 起，Apple 提供
 * CLOCK_MONOTONIC_RAW，比其 CLOCK_MONOTONIC 读更快、
 * 分辨率更高。
 */
#if defined(__darwin__) && defined(CLOCK_MONOTONIC_RAW)
#define PG_INSTR_CLOCK	CLOCK_MONOTONIC_RAW
#elif defined(CLOCK_MONOTONIC)
#define PG_INSTR_CLOCK	CLOCK_MONOTONIC
#else
#define PG_INSTR_CLOCK	CLOCK_REALTIME
#endif

/*  INSTR_TIME_SET_CURRENT 辅助  */
static inline instr_time
pg_clock_gettime_ns(void)
{
	instr_time	now;
	struct timespec tmp;

	clock_gettime(PG_INSTR_CLOCK, &tmp);
	now.ticks = tmp.tv_sec * NS_PER_S + tmp.tv_nsec;

	return now;
}

#define INSTR_TIME_SET_CURRENT(t) \
	((t) = pg_clock_gettime_ns())

#define INSTR_TIME_GET_NANOSEC(t) \
	((int64) (t).ticks)


#else							/* WIN32 */


/*  QueryPerformanceCounter  */

/*  INSTR_TIME_SET_CURRENT 辅助  */
static inline instr_time
pg_query_performance_counter(void)
{
	instr_time	now;
	LARGE_INTEGER tmp;

	QueryPerformanceCounter(&tmp);
	now.ticks = tmp.QuadPart;

	return now;
}

static inline double
GetTimerFrequency(void)
{
	LARGE_INTEGER f;

	QueryPerformanceFrequency(&f);
	return (double) f.QuadPart;
}

#define INSTR_TIME_SET_CURRENT(t) \
	((t) = pg_query_performance_counter())

#define INSTR_TIME_GET_NANOSEC(t) \
	((int64) ((t).ticks * ((double) NS_PER_S / GetTimerFrequency())))

#endif							/* WIN32 */


/*
 * 公共宏
 */

#define INSTR_TIME_IS_ZERO(t)	((t).ticks == 0)


#define INSTR_TIME_SET_ZERO(t)	((t).ticks = 0)

#define INSTR_TIME_SET_CURRENT_LAZY(t) \
	(INSTR_TIME_IS_ZERO(t) ? INSTR_TIME_SET_CURRENT(t), true : false)


#define INSTR_TIME_ADD(x,y) \
	((x).ticks += (y).ticks)

#define INSTR_TIME_SUBTRACT(x,y) \
	((x).ticks -= (y).ticks)

#define INSTR_TIME_ACCUM_DIFF(x,y,z) \
	((x).ticks += (y).ticks - (z).ticks)


#define INSTR_TIME_GET_DOUBLE(t) \
	((double) INSTR_TIME_GET_NANOSEC(t) / NS_PER_S)

#define INSTR_TIME_GET_MILLISEC(t) \
	((double) INSTR_TIME_GET_NANOSEC(t) / NS_PER_MS)

#define INSTR_TIME_GET_MICROSEC(t) \
	(INSTR_TIME_GET_NANOSEC(t) / NS_PER_US)

#endif							/* INSTR_TIME_H */
