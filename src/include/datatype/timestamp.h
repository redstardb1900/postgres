/*-------------------------------------------------------------------------
 *
 * timestamp.h
 *	  Timestamp 与 Interval 类型定义及相关宏。
 *
 * 注意：本文件须可在前端与后端上下文中包含。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/datatype/timestamp.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATATYPE_TIMESTAMP_H
#define DATATYPE_TIMESTAMP_H

/*
 * Timestamp 表示绝对时间。
 *
 * Interval 表示时间差。分别跟踪月（及年）、日
 * 与时/分/秒，因相对绝对时间实例化前，所跨
 * 流逝时间未知。
 *
 * 注意 Postgres 用"时间区间"指有界区间，含起止
 * 时间，而非时间跨度 - thomas 97/03/20
 *
 * 时间戳以及 Interval 的时/分/秒字段存为
 * 以微秒为单位的 int64。（曾几何时是以秒为
 * 单位的 double。）
 *
 * TimeOffset 与 fsec_t 是临时变量的便利 typedef。
 * 切勿在磁盘存储值中使用 fsec_t。
 * 另，fsec_t 仅用于*小数*秒；若需存可能达
 * 许多秒的值，当心溢出。
 */

typedef int64 Timestamp;
typedef int64 TimestampTz;
typedef int64 TimeOffset;
typedef int32 fsec_t;			/* 小数秒（微秒） */


/*
 * Interval 类型的存储格式。
 */
typedef struct
{
	TimeOffset	time;			/* 除日/月/年外全部
								 * years */
	int32		day;			/* 日，置于 time 后以对齐 */
	int32		month;			/* 月与年，置于 time 后以对齐 */
} Interval;

/*
 * 表示分解后 Interval 的数据结构。
 *
 * 因历史原因，按时间戳的 pg_tm 建模。
 * 与时间戳不同，月或年无魔法解释：只是零或
 * 非零。注意字段可为负；但因从 Interval
 * 转换时的除法，仅 tm_mday 可能为 INT_MIN。这很
 * 重要，因某些代码路径可能需对其取负。
 * 。
 */
struct pg_itm
{
	int			tm_usec;
	int			tm_sec;
	int			tm_min;
	int64		tm_hour;		/* 需要够宽 */
	int			tm_mday;
	int			tm_mon;
	int			tm_year;
};

/*
 * 解码 Interval 的数据结构。本可用 struct pg_itm，
 * 但那样会把 tm_usec 须 64 位的要求传到并不真正
 * 需要处。另，省略解码期间未用字段也是
 * 良好的防错措施。
 */
struct pg_itm_in
{
	int64		tm_usec;		/* 需要够宽 */
	int			tm_mday;
	int			tm_mon;
	int			tm_year;
};


/* 这些数据类型"精度"选项（typmod）的限制 */
#define MAX_TIMESTAMP_PRECISION 6
#define MAX_INTERVAL_PRECISION 6

/*
 *	舍入到 MAX_TIMESTAMP_PRECISION 位小数。
 *	注意：这也用于 Interval 的舍入。
 */
#define TS_PREC_INV 1000000.0
#define TSROUND(j) (rint(((double) (j)) * TS_PREC_INV) / TS_PREC_INV)


/*
 * 日期时间相关计算的各种常量
 */

#define DAYS_PER_YEAR	365.25	/* 假定每四年一闰 */
#define MONTHS_PER_YEAR 12
/*
 *	DAYS_PER_MONTH 很不精确。更准的值是
 *	365.2425/12 = 30.436875，或'30 days 10:29:06'。现仅
 *	返回整数日，但将来或许也应返回
 *	'time' 值一并使用。ISO 8601 建议
 *	30日。
 */
#define DAYS_PER_MONTH	30		/* 假定每月恰 30 日 */
#define HOURS_PER_DAY	24		/* 假定无夏令时变更 */

/*
 *	这不校正不均的夏令时区间或闰秒，且
 *	粗略估计闰年。更准的每年日数是
 *	365.2422。
 */
#define SECS_PER_YEAR	(36525 * 864)	/* 避免浮点计算 */
#define SECS_PER_DAY	86400
#define SECS_PER_HOUR	3600
#define SECS_PER_MINUTE 60
#define MINS_PER_HOUR	60

#define USECS_PER_DAY	INT64CONST(86400000000)
#define USECS_PER_HOUR	INT64CONST(3600000000)
#define USECS_PER_MINUTE INT64CONST(60000000)
#define USECS_PER_SEC	INT64CONST(1000000)

/*
 * 允许相对格林威治东西最多 15:59:59 的数值时区偏移。
 * 当前实际使用中最古怪偏移的纪录保持者是
 * Asia/Manila（至 1844 为 -15:56:08）与 America/Metlakatla
 * （至 1867 为 +15:13:42）。若拒绝此类值，将无法转储
 * 并恢复带这些时区设置的旧 timestamptz。
 */
#define MAX_TZDISP_HOUR		15	/* 最大小时部分 */
#define TZDISP_LIMIT		((MAX_TZDISP_HOUR + 1) * SECS_PER_HOUR)

/*
 * 保留整数最小/最大值以表示 timestamp
 * （或 timestamptz）的 -infinity 与 +infinity。
 */
#define TIMESTAMP_MINUS_INFINITY	PG_INT64_MIN
#define TIMESTAMP_INFINITY	PG_INT64_MAX

/*
 * 历史上曾用这些 infinity 别名。
 */
#define DT_NOBEGIN		TIMESTAMP_MINUS_INFINITY
#define DT_NOEND		TIMESTAMP_INFINITY

#define TIMESTAMP_NOBEGIN(j)	\
	do {(j) = DT_NOBEGIN;} while (0)

#define TIMESTAMP_IS_NOBEGIN(j) ((j) == DT_NOBEGIN)

#define TIMESTAMP_NOEND(j)		\
	do {(j) = DT_NOEND;} while (0)

#define TIMESTAMP_IS_NOEND(j)	((j) == DT_NOEND)

#define TIMESTAMP_NOT_FINITE(j) (TIMESTAMP_IS_NOBEGIN(j) || TIMESTAMP_IS_NOEND(j))


/*
 * 儒略日支持。
 *
 * date2j() 与 j2date() 名义上处理儒略日范围 0..INT_MAX，
 * 即公元前 4714-11-24 至公元 5874898-06-03。实践中 date2j()
 * 对更早日期也能工作并给出正确负儒略日。
 * 我们依赖它回溯到公元前 4714-11-01。至少留一日
 * 余量是必要的，以免时间戳时区旋转产生输入会
 * 拒绝的日期。例如 '4714-11-24 00:00 GMT BC' 是合法
 * timestamptz，但在格林威治以东时区会打印成
 * 公元前 4714-11-23 下午某时；若无法处理该日期
 * 就会转储/重载失败。故 IS_VALID_JULIAN 的想法是
 * 接受略宽于实际支持的日期范围，然后在
 * IS_VALID_DATE 或 IS_VALID_TIMESTAMP 中做精确检查
 * （若有时区旋转则在其后）。为省几轮，
 * IS_VALID_JULIAN 可只检查到月界，因其精确截止
 * 在本方案中并不十分关键。
 *
 * JULIAN_MINYEAR 为 -4713 而非 -4714 是正确的；它定义为
 * 便于与 tm_year 比较，其中我们遵循约定
 * tm_year <= 0 表示 abs(tm_year)+1 BC。
 */

#define JULIAN_MINYEAR (-4713)
#define JULIAN_MINMONTH (11)
#define JULIAN_MINDAY (24)
#define JULIAN_MAXYEAR (5874898)
#define JULIAN_MAXMONTH (6)
#define JULIAN_MAXDAY (3)

#define IS_VALID_JULIAN(y,m,d) \
	(((y) > JULIAN_MINYEAR || \
	  ((y) == JULIAN_MINYEAR && ((m) >= JULIAN_MINMONTH))) && \
	 ((y) < JULIAN_MAXYEAR || \
	  ((y) == JULIAN_MAXYEAR && ((m) < JULIAN_MAXMONTH))))

/* Julian-date equivalents of Day 0 in Unix and Postgres reckoning */
#define UNIX_EPOCH_JDATE		2440588 /* == date2j(1970, 1, 1) */
#define POSTGRES_EPOCH_JDATE	2451545 /* == date2j(2000, 1, 1) */

/*
 * 日期与时间戳的范围限制。
 *
 * 传统上允许儒略日零为有效日期时间值，
 * 故它是日期与时间戳的下界。
 *
 * 日期上界是 5874897-12-31，略小于儒略日代码
 * 所能允许。时间戳上界是
 * 294276-12-31。int64 溢出限还要晚几天；同样，
 * 留余量可免边角溢出之忧，并提供更简单的
 * 用户可见定义。
 */

/* First allowed date, and first disallowed date, in Julian-date form */
#define DATETIME_MIN_JULIAN (0)
#define DATE_END_JULIAN (2147483494)	/* == date2j(JULIAN_MAXYEAR, 1, 1) */
#define TIMESTAMP_END_JULIAN (109203528)	/* == date2j(294277, 1, 1) */

/* Timestamp limits */
#define MIN_TIMESTAMP	INT64CONST(-211813488000000000)
/* == (DATETIME_MIN_JULIAN - POSTGRES_EPOCH_JDATE) * USECS_PER_DAY */
#define END_TIMESTAMP	INT64CONST(9223371331200000000)
/* == (TIMESTAMP_END_JULIAN - POSTGRES_EPOCH_JDATE) * USECS_PER_DAY */

/* Range-check a date (given in Postgres, not Julian, numbering) */
#define IS_VALID_DATE(d) \
	((DATETIME_MIN_JULIAN - POSTGRES_EPOCH_JDATE) <= (d) && \
	 (d) < (DATE_END_JULIAN - POSTGRES_EPOCH_JDATE))

/* Range-check a timestamp */
#define IS_VALID_TIMESTAMP(t)  (MIN_TIMESTAMP <= (t) && (t) < END_TIMESTAMP)

#endif							/* DATATYPE_TIMESTAMP_H */
