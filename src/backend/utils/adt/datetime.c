/*-------------------------------------------------------------------------
 *
 * datetime.c
 *	  日期/时间类型的支持函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/datetime.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "common/string.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/tzparser.h"

static int	DecodeNumber(int flen, char *str, bool haveTextMonth,
						 int fmask, int *tmask,
						 struct pg_tm *tm, fsec_t *fsec, bool *is2digits);
static int	DecodeNumberField(int len, char *str,
							  int fmask, int *tmask,
							  struct pg_tm *tm, fsec_t *fsec, bool *is2digits);
static int	DecodeTimeCommon(char *str, int fmask, int range,
							 int *tmask, struct pg_itm *itm);
static int	DecodeTime(char *str, int fmask, int range,
					   int *tmask, struct pg_tm *tm, fsec_t *fsec);
static int	DecodeTimeForInterval(char *str, int fmask, int range,
								  int *tmask, struct pg_itm_in *itm_in);
static const datetkn *datebsearch(const char *key, const datetkn *base, int nel);
static int	DecodeDate(char *str, int fmask, int *tmask, bool *is2digits,
					   struct pg_tm *tm);
static char *AppendSeconds(char *cp, int sec, fsec_t fsec,
						   int precision, bool fillzeros);
static bool int64_multiply_add(int64 val, int64 multiplier, int64 *sum);
static bool AdjustFractMicroseconds(double frac, int64 scale,
									struct pg_itm_in *itm_in);
static bool AdjustFractDays(double frac, int scale,
							struct pg_itm_in *itm_in);
static bool AdjustFractYears(double frac, int scale,
							 struct pg_itm_in *itm_in);
static bool AdjustMicroseconds(int64 val, double fval, int64 scale,
							   struct pg_itm_in *itm_in);
static bool AdjustDays(int64 val, int scale,
					   struct pg_itm_in *itm_in);
static bool AdjustMonths(int64 val, struct pg_itm_in *itm_in);
static bool AdjustYears(int64 val, int scale,
						struct pg_itm_in *itm_in);
static int	DetermineTimeZoneOffsetInternal(struct pg_tm *tm, pg_tz *tzp,
											pg_time_t *tp);
static bool DetermineTimeZoneAbbrevOffsetInternal(pg_time_t t,
												  const char *abbr, pg_tz *tzp,
												  int *offset, int *isdst);
static pg_tz *FetchDynamicTimeZone(TimeZoneAbbrevTable *tbl, const datetkn *tp,
								   DateTimeErrorExtra *extra);


const int	day_tab[2][13] =
{
	{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0},
	{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0}
};

const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
"Jul", "Aug", "Sep", "Oct", "Nov", "Dec", NULL};

const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
"Thursday", "Friday", "Saturday", NULL};


/*****************************************************************************
 *	 私有例程                   *
 *****************************************************************************/

/*
 * datetktbl 存日期/时间关键字
 *
 * 注意本表必须严格按字母顺序排列，这样才能使用
 *  O(ln(N)) 搜索算法。
 *
 * token 字段必须以 NUL 终止；我们把条目截断到 TOKMAXLEN
 * 个字符。
 *
 * 静态表不含 TZ、DTZ 或 DYNTZ 条目；这些条目改为
 * 从配置文件装载并存入 zoneabbrevtbl，其
 * abbrevs[] 字段与静态 datetktbl 格式相同。
 */
static const datetkn datetktbl[] = {
	/* token/type/value */
	{"+infinity", RESERV, DTK_LATE},	/* 同"infinity" */
	{EARLY, RESERV, DTK_EARLY}, /* "-infinity" 保留给 "early time" */
	{DA_D, ADBC, AD},			/* 年份>0 用 "ad" */
	{"allballs", RESERV, DTK_ZULU}, /* 00:00:00 */
	{"am", AMPM, AM},
	{"apr", MONTH, 4},
	{"april", MONTH, 4},
	{"at", IGNORE_DTF, 0},		/* "at" (弃词) */
	{"aug", MONTH, 8},
	{"august", MONTH, 8},
	{DB_C, ADBC, BC},			/* 年份<=0 用 "bc" */
	{"d", UNITS, DTK_DAY},		/* ISO 输入的 "月内日" */
	{"dec", MONTH, 12},
	{"december", MONTH, 12},
	{"dow", UNITS, DTK_DOW},	/* 周内日 */
	{"doy", UNITS, DTK_DOY},	/* 年积日 */
	{"dst", DTZMOD, SECS_PER_HOUR},
	{EPOCH, RESERV, DTK_EPOCH}, /* "epoch" 保留给系统纪元时间 */
	{"feb", MONTH, 2},
	{"february", MONTH, 2},
	{"fri", DOW, 5},
	{"friday", DOW, 5},
	{"h", UNITS, DTK_HOUR},		/* "时" */
	{LATE, RESERV, DTK_LATE},	/* "infinity" 保留给 "late time" */
	{"isodow", UNITS, DTK_ISODOW},	/* ISO 周内日，周日 == 7 */
	{"isoyear", UNITS, DTK_ISOYEAR},	/* 按 ISO 周日期计的年份 */
	{"j", UNITS, DTK_JULIAN},
	{"jan", MONTH, 1},
	{"january", MONTH, 1},
	{"jd", UNITS, DTK_JULIAN},
	{"jul", MONTH, 7},
	{"julian", UNITS, DTK_JULIAN},
	{"july", MONTH, 7},
	{"jun", MONTH, 6},
	{"june", MONTH, 6},
	{"m", UNITS, DTK_MONTH},	/* ISO 输入的 "月" */
	{"mar", MONTH, 3},
	{"march", MONTH, 3},
	{"may", MONTH, 5},
	{"mm", UNITS, DTK_MINUTE},	/* ISO 输入的 "分" */
	{"mon", DOW, 1},
	{"monday", DOW, 1},
	{"nov", MONTH, 11},
	{"november", MONTH, 11},
	{NOW, RESERV, DTK_NOW},		/* 当前事务时间 */
	{"oct", MONTH, 10},
	{"october", MONTH, 10},
	{"on", IGNORE_DTF, 0},		/* "on" (弃词) */
	{"pm", AMPM, PM},
	{"s", UNITS, DTK_SECOND},	/* ISO 输入的 "秒" */
	{"sat", DOW, 6},
	{"saturday", DOW, 6},
	{"sep", MONTH, 9},
	{"sept", MONTH, 9},
	{"september", MONTH, 9},
	{"sun", DOW, 0},
	{"sunday", DOW, 0},
	{"t", ISOTIME, DTK_TIME},	/* ISO 时间字段占位符 */
	{"thu", DOW, 4},
	{"thur", DOW, 4},
	{"thurs", DOW, 4},
	{"thursday", DOW, 4},
	{TODAY, RESERV, DTK_TODAY}, /* 午夜 */
	{TOMORROW, RESERV, DTK_TOMORROW},	/* 明日午夜 */
	{"tue", DOW, 2},
	{"tues", DOW, 2},
	{"tuesday", DOW, 2},
	{"wed", DOW, 3},
	{"wednesday", DOW, 3},
	{"weds", DOW, 3},
	{"y", UNITS, DTK_YEAR},		/* ISO 输入的 "年" */
	{YESTERDAY, RESERV, DTK_YESTERDAY}	/* 昨日午夜 */
};

static const int szdatetktbl = sizeof datetktbl / sizeof datetktbl[0];

/*
 * deltatktbl：与 datetktbl 格式相同，但保存用于表示
 * 时间单位（如 interval 与 EXTRACT）。
 */
static const datetkn deltatktbl[] = {
	/* token/type/value */
	{"@", IGNORE_DTF, 0},		/* postgres 相对前缀 */
	{DAGO, AGO, 0},				/* "ago" 表示负的时间偏移 */
	{"c", UNITS, DTK_CENTURY},	/* "世纪"(相对) */
	{"cent", UNITS, DTK_CENTURY},	/* "世纪"(相对) */
	{"centuries", UNITS, DTK_CENTURY},	/* "世纪"(相对) */
	{DCENTURY, UNITS, DTK_CENTURY}, /* "世纪"(相对) */
	{"d", UNITS, DTK_DAY},		/* "日"(相对) */
	{DDAY, UNITS, DTK_DAY},		/* "日"(相对) */
	{"days", UNITS, DTK_DAY},	/* "日"(相对) */
	{"dec", UNITS, DTK_DECADE}, /* "十年"(相对) */
	{DDECADE, UNITS, DTK_DECADE},	/* "十年"(相对) */
	{"decades", UNITS, DTK_DECADE}, /* "十年"(相对) */
	{"decs", UNITS, DTK_DECADE},	/* "十年"(相对) */
	{"h", UNITS, DTK_HOUR},		/* "时"(相对) */
	{DHOUR, UNITS, DTK_HOUR},	/* "时"(相对) */
	{"hours", UNITS, DTK_HOUR}, /* "小时"(相对) */
	{"hr", UNITS, DTK_HOUR},	/* "时"(相对) */
	{"hrs", UNITS, DTK_HOUR},	/* "小时"(相对) */
	{"m", UNITS, DTK_MINUTE},	/* "分"(相对) */
	{"microsecon", UNITS, DTK_MICROSEC},	/* "微秒"(相对) */
	{"mil", UNITS, DTK_MILLENNIUM}, /* "千年"(相对) */
	{"millennia", UNITS, DTK_MILLENNIUM},	/* "千年"(相对) */
	{DMILLENNIUM, UNITS, DTK_MILLENNIUM},	/* "千年"(相对) */
	{"millisecon", UNITS, DTK_MILLISEC},	/* 相对 */
	{"mils", UNITS, DTK_MILLENNIUM},	/* "千年"(相对) */
	{"min", UNITS, DTK_MINUTE}, /* "分"(相对) */
	{"mins", UNITS, DTK_MINUTE},	/* "分"(相对) */
	{DMINUTE, UNITS, DTK_MINUTE},	/* "分"(相对) */
	{"minutes", UNITS, DTK_MINUTE}, /* "分"(相对) */
	{"mon", UNITS, DTK_MONTH},	/* "月"(相对) */
	{"mons", UNITS, DTK_MONTH}, /* "月"(相对) */
	{DMONTH, UNITS, DTK_MONTH}, /* "月"(相对) */
	{"months", UNITS, DTK_MONTH},
	{"ms", UNITS, DTK_MILLISEC},
	{"msec", UNITS, DTK_MILLISEC},
	{DMILLISEC, UNITS, DTK_MILLISEC},
	{"mseconds", UNITS, DTK_MILLISEC},
	{"msecs", UNITS, DTK_MILLISEC},
	{"qtr", UNITS, DTK_QUARTER},	/* "季度"(相对) */
	{DQUARTER, UNITS, DTK_QUARTER}, /* "季度"(相对) */
	{"s", UNITS, DTK_SECOND},
	{"sec", UNITS, DTK_SECOND},
	{DSECOND, UNITS, DTK_SECOND},
	{"seconds", UNITS, DTK_SECOND},
	{"secs", UNITS, DTK_SECOND},
	{DTIMEZONE, UNITS, DTK_TZ}, /* "时区"时间偏移 */
	{"timezone_h", UNITS, DTK_TZ_HOUR}, /* 时区小时单位 */
	{"timezone_m", UNITS, DTK_TZ_MINUTE},	/* 时区分钟单位 */
	{"us", UNITS, DTK_MICROSEC},	/* "微秒"(相对) */
	{"usec", UNITS, DTK_MICROSEC},	/* "微秒"(相对) */
	{DMICROSEC, UNITS, DTK_MICROSEC},	/* "微秒"(相对) */
	{"useconds", UNITS, DTK_MICROSEC},	/* "微秒"(相对) */
	{"usecs", UNITS, DTK_MICROSEC}, /* "微秒"(相对) */
	{"w", UNITS, DTK_WEEK},		/* "周"(相对) */
	{DWEEK, UNITS, DTK_WEEK},	/* "周"(相对) */
	{"weeks", UNITS, DTK_WEEK}, /* "周"(相对) */
	{"y", UNITS, DTK_YEAR},		/* "年"(相对) */
	{DYEAR, UNITS, DTK_YEAR},	/* "年"(相对) */
	{"years", UNITS, DTK_YEAR}, /* "年"(相对) */
	{"yr", UNITS, DTK_YEAR},	/* "年"(相对) */
	{"yrs", UNITS, DTK_YEAR}	/* "年"(相对) */
};

static const int szdeltatktbl = sizeof deltatktbl / sizeof deltatktbl[0];

static TimeZoneAbbrevTable *zoneabbrevtbl = NULL;

/* 上面各表近期查找结果的缓存 */

static const datetkn *datecache[MAXDATEFIELDS] = {NULL};

static const datetkn *deltacache[MAXDATEFIELDS] = {NULL};

static const datetkn *abbrevcache[MAXDATEFIELDS] = {NULL};


/*
 * 日历时间与儒略日期的换算。
 * 儒略日期常用于天文学应用中，
 *	因为它数值精确且计算简单。
 * 这里的算法能在儒略日与
 *	日历日期之间对所有非负儒略日换算
 *	（自 -4713-11-24 起）。
 *
 * 重写以消除溢出问题。现在可让这些
 * 例程对从 0 到 2147483647 的儒略日计数
 * （-4713-11-24 到 5874898-06-03）在假设
 * 为 32 位整数时都正确。更长类型也应在其
 * 精度内可行。
 *
 * 实际上，date2j() 能以产生有效负儒略日期的
 * 合理方式工作，且远在 -4713-11-24 之前即如此。
 * 我们依赖它追溯到 -4713-11-01；见 IS_VALID_JULIAN()
 * 及 timestamp.h 中的相关说明。
 */

int
date2j(int year, int month, int day)
{
	int			julian;
	int			century;

	if (month > 2)
	{
		month += 1;
		year += 4800;
	}
	else
	{
		month += 13;
		year += 4799;
	}

	century = year / 100;
	julian = year * 365 - 32167;
	julian += year / 4 - century + century / 4;
	julian += 7834 * month / 256 + day;

	return julian;
}								/* date2j() */

void
j2date(int jd, int *year, int *month, int *day)
{
	unsigned int julian;
	unsigned int quad;
	unsigned int extra;
	int			y;

	julian = jd;
	julian += 32044;
	quad = julian / 146097;
	extra = (julian - quad * 146097) * 4 + 3;
	julian += 60 + quad * 3 + extra / 146097;
	quad = julian / 1461;
	julian -= quad * 1461;
	y = julian * 4 / 1461;
	julian = ((y != 0) ? ((julian + 305) % 365) : ((julian + 306) % 366))
		+ 123;
	y += quad * 4;
	*year = y - 4800;
	quad = julian * 2141 / 65536;
	*day = julian - 7834 * quad / 256;
	*month = (quad + 10) % MONTHS_PER_YEAR + 1;
}								/* j2date() */


/*
 * j2day - 儒略日期转周内日 (0..6 == 周日..周六)
 *
 * 注意：多处使用 j2day(date - 1) 这一写法来产生
 * 符合 0..6 = 周一..周日 约定的结果。这有点
 * 不严谨，但只要这里的计算只是取模，它就能工作。
 */
int
j2day(int date)
{
	date += 1;
	date %= 7;
	/* 若除法向零截断则作补偿，它很可能如此 */
	if (date < 0)
		date += 7;

	return date;
}								/* j2day() */


/*
 * GetCurrentDateTime()
 *
 * 取得事务开始时间（"now()"）并分解为 struct pg_tm，
 * 并按会话时区设置加以转换。
 *
 * 这只是 GetCurrentTimeUsec 的便捷包装，用于覆盖
 * 调用方既不需要小数秒也不需要时区偏移的情形。
 */
void
GetCurrentDateTime(struct pg_tm *tm)
{
	fsec_t		fsec;

	GetCurrentTimeUsec(tm, &fsec, NULL);
}

/*
 * GetCurrentTimeUsec()
 *
 * 取得事务开始时间（"now()"）并分解为 struct pg_tm，
 * 包括小数秒与时区偏移。时间按会话时区设置
 * 加以转换。
 *
 * 调用方若不需要偏移可传 tzp = NULL，但这并不
 * 影响转换行为（不同于 timestamp2tm()）。
 *
 * 内部我们会缓存结果，因为它可能在一个事务中
 * 被调用多次，其间 now() 不会改变。
 */
void
GetCurrentTimeUsec(struct pg_tm *tm, fsec_t *fsec, int *tzp)
{
	TimestampTz cur_ts = GetCurrentTransactionStartTimestamp();

	/*
	 * 缓存键必须同时包含当前时间与当前时区。由于
	 * 仅用指针表示时区，我们是在假设
	 * 不同的时区设置绝不会有相同的指针值。
	 * 这得益于 pg_tzset() 内部使用的哈希表；
	 * 不过，如果将来允许该哈希中的条目被回收，
	 * 则需再看。
	 */
	static TimestampTz cache_ts = 0;
	static pg_tz *cache_timezone = NULL;
	static struct pg_tm cache_tm;
	static fsec_t cache_fsec;
	static int	cache_tz;

	if (cur_ts != cache_ts || session_timezone != cache_timezone)
	{
		/*
		 * 若 timestamp2tm 内发生部分更新之后出错，要
		 * 确保缓存无效。
		 */
		cache_timezone = NULL;

		/*
		 * 执行计算并把结果存入缓存。我们并不
		 * 真的预期这里会出错，因为当前时间理应
		 * 在范围内，但为稳妥仍检查。
		 */
		if (timestamp2tm(cur_ts, &cache_tz, &cache_tm, &cache_fsec,
						 NULL, session_timezone) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		/* 好，标记缓存有效。 */
		cache_ts = cur_ts;
		cache_timezone = session_timezone;
	}

	*tm = cache_tm;
	*fsec = cache_fsec;
	if (tzp != NULL)
		*tzp = cache_tz;
}


/*
 * 在 *cp 处追加秒与小数秒（如有）。
 *
 * precision 是小数位数的上限，fillzeros 表示要
 * 补足两位整数秒数字。
 *
 * 返回指向字符串新末尾的指针。不在那里放
 * NUL 终止符；调用方需自行负责 str 的 NUL 终止。
 *
 * 注意 sec 与 fsec 输入值的任何符号都会被剥除。
 */
static char *
AppendSeconds(char *cp, int sec, fsec_t fsec, int precision, bool fillzeros)
{
	Assert(precision >= 0);

	if (fillzeros)
		cp = pg_ultostr_zeropad(cp, abs(sec), 2);
	else
		cp = pg_ultostr(cp, abs(sec));

	/* fsec_t 只是 int32 */
	if (fsec != 0)
	{
		int32		value = abs(fsec);
		char	   *end = &cp[precision + 1];
		bool		gotnonzero = false;

		*cp++ = '.';

		/*
		 * 追加小数秒部分。注意我们不想要任何
		 * 尾部零，所以由于我们是反向构建数字，
		 * 在输出非零数字前会跳过追加零。
		 */
		while (precision--)
		{
			int32		oldval = value;
			int32		remainder;

			value /= 10;
			remainder = oldval - value * 10;

			/* 查是否得到非零 */
			if (remainder)
				gotnonzero = true;

			if (gotnonzero)
				cp[precision] = '0' + remainder;
			else
				end = &cp[precision];
		}

		/*
		 * 如果我们仍有非零值，那么 precision 必然
		 * 不足以打印该数字。我们把问题交给 pg_ultostr()，
		 * 它会以最小有效宽度生成正确答案。
		 */
		if (value)
			return pg_ultostr(cp, abs(fsec));

		return end;
	}
	else
		return cp;
}


/*
 * 上面的变体，专用于 timestamp 情形。
 *
 * 返回指向字符串新末尾的指针。不在那里放
 * NUL 终止符；调用方需自行负责 str 的 NUL 终止。
 */
static char *
AppendTimestampSeconds(char *cp, struct pg_tm *tm, fsec_t fsec)
{
	return AppendSeconds(cp, tm->tm_sec, fsec, MAX_TIMESTAMP_PRECISION, true);
}


/*
 * val*multiplier 加到*sum。
 * 成功返回 true，溢出则返回 false。
 */
static bool
int64_multiply_add(int64 val, int64 multiplier, int64 *sum)
{
	int64		product;

	if (pg_mul_s64_overflow(val, multiplier, &product) ||
		pg_add_s64_overflow(*sum, product, sum))
		return false;
	return true;
}

/*
 * 把 frac 乘以 scale（得到微秒）并加到 itm_in->tm_usec。
 * 成功返回 true，itm_in 溢出则返回 false。
 */
static bool
AdjustFractMicroseconds(double frac, int64 scale,
						struct pg_itm_in *itm_in)
{
	int64		usec;

	/* 常用快速路径 */
	if (frac == 0)
		return true;

	/*
	 * 我们假设输入 frac 的绝对值小于 1，因此 frac 或
	 * usec 不会溢出（对有意义的 scale 值）。
	 */
	frac *= scale;
	usec = (int64) frac;

	/* 对任何小数微秒做舍入 */
	frac -= usec;
	if (frac > 0.5)
		usec++;
	else if (frac < -0.5)
		usec--;

	return !pg_add_s64_overflow(itm_in->tm_usec, usec, &itm_in->tm_usec);
}

/*
 * 把 frac 乘以 scale（得到天）。把结果的整数部分加
 * 到 itm_in->tm_mday，小数部分加到 itm_in->tm_usec。
 * 成功返回 true，itm_in 溢出则返回 false。
 */
static bool
AdjustFractDays(double frac, int scale,
				struct pg_itm_in *itm_in)
{
	int			extra_days;

	/* 常用快速路径 */
	if (frac == 0)
		return true;

	/*
	 * 我们假设输入 frac 的绝对值小于 1，因此 frac 或
	 * extra_days 不构成问题。
	 */
	frac *= scale;
	extra_days = (int) frac;

	/* 但若 tm_mday 已非零，此处可能溢出 */
	if (pg_add_s32_overflow(itm_in->tm_mday, extra_days, &itm_in->tm_mday))
		return false;

	/* 处理任何小数天 */
	frac -= extra_days;
	return AdjustFractMicroseconds(frac, USECS_PER_DAY, itm_in);
}

/*
 * 把 frac 乘以 scale（得到年），再进一步放大到月。
 * 把结果的整数部分加到 itm_in->tm_mon，丢弃任何
 * 小数部分。
 * 成功返回 true，itm_in 溢出则返回 false。
 */
static bool
AdjustFractYears(double frac, int scale,
				 struct pg_itm_in *itm_in)
{
	/*
	 * 同上，我们假设 abs(frac) < 1，因此只要 scale
	 * 有意义就不溢出。
	 */
	int			extra_months = (int) rint(frac * scale * MONTHS_PER_YEAR);

	return !pg_add_s32_overflow(itm_in->tm_mon, extra_months, &itm_in->tm_mon);
}

/*
 * (val+fval)*scale 加到 itm_in->tm_usec。
 * 成功返回 true，itm_in 溢出则返回 false。
 */
static bool
AdjustMicroseconds(int64 val, double fval, int64 scale,
				   struct pg_itm_in *itm_in)
{
	/* 处理整数部分 */
	if (!int64_multiply_add(val, scale, &itm_in->tm_usec))
		return false;
	/* 处理浮点部分 */
	return AdjustFractMicroseconds(fval, scale, itm_in);
}

/*
 * 把 val 乘以 scale（得到天）并加到 itm_in->tm_mday。
 * 成功返回 true，itm_in 溢出则返回 false。
 */
static bool
AdjustDays(int64 val, int scale, struct pg_itm_in *itm_in)
{
	int			days;

	if (val < INT_MIN || val > INT_MAX)
		return false;
	return !pg_mul_s32_overflow((int32) val, scale, &days) &&
		!pg_add_s32_overflow(itm_in->tm_mday, days, &itm_in->tm_mday);
}

/*
 * 把 val 加到 itm_in->tm_mon（此处无需 scale，
 * 已以月计）。
 * 成功返回 true，itm_in 溢出则返回 false。
 */
static bool
AdjustMonths(int64 val, struct pg_itm_in *itm_in)
{
	if (val < INT_MIN || val > INT_MAX)
		return false;
	return !pg_add_s32_overflow(itm_in->tm_mon, (int32) val, &itm_in->tm_mon);
}

/*
 * 把 val 乘以 scale（得到年）并加到 itm_in->tm_year。
 * 成功返回 true，itm_in 溢出则返回 false。
 */
static bool
AdjustYears(int64 val, int scale,
			struct pg_itm_in *itm_in)
{
	int			years;

	if (val < INT_MIN || val > INT_MAX)
		return false;
	return !pg_mul_s32_overflow((int32) val, scale, &years) &&
		!pg_add_s32_overflow(itm_in->tm_year, years, &itm_in->tm_year);
}


/*
 * 解析数字的小数部分（小数点与可选数字，
 * 后接字符串结尾）。把小数值返回到 *frac 中。
 *
 * 成功返回 0，检测到伪输入则返回 DTERR 码。
 */
static int
ParseFraction(char *cp, double *frac)
{
	/* 调用方应始终传入小数部分的开头 */
	Assert(*cp == '.');

	/*
	 * 我们想允许不带数字的单独 "."，但某些版本的 strtod
	 * 会为此报告 EINVAL，所以特判它。
	 */
	if (cp[1] == '\0')
	{
		*frac = 0;
	}
	else
	{
		errno = 0;
		*frac = strtod(cp, &cp);
		/* 查解析是否失败 */
		if (*cp != '\0' || errno != 0)
			return DTERR_BAD_FORMAT;
	}
	return 0;
}

/*
 * 在适当错误检查下取小数秒值。
 * 同 ParseFraction，但会把结果转换为整数微秒。
 */
static int
ParseFractionalSecond(char *cp, fsec_t *fsec)
{
	double		frac;
	int			dterr;

	dterr = ParseFraction(cp, &frac);
	if (dterr)
		return dterr;
	*fsec = rint(frac * 1000000);
	return 0;
}


/* ParseDateTime()
 *	按日期/时间语境切分字符串为记号。
 *	成功返回 0，检测到伪输入则返回 DTERR 码。
 *
 * timestr - 输入字符串
 * workbuf - 存储字段字符串的工作区。它必须
 *	 大于该日期时间类型最大合法输入 --
 *	 因为 NUL 终止字段还需要一些额外空间。
 * buflen - workbuf 的大小
 * field[] - 字段字符串的指针返回到此数组中
 * ftype[] - 字段类型指示符返回到此数组中
 * maxfields - 上面两个数组的维度
 * *numfields - 置为检测到的实际字段数
 *
 * 从输入提取的字段作为独立的、
 * 以 NUL 终止的字符串存入 workbuf 工作区。
 * 文本都转为小写。
 *
 * 会指定几种字段类型：
 *	DTK_NUMBER - 数字和（可能的）小数点
 *	DTK_DATE - 数字与两个定界符，或数字与文本
 *	DTK_TIME - 数字、冒号定界符，可能有小数点
 *	DTK_STRING - 文本（无数字或标点）
 *	DTK_SPECIAL - 前导 "+" 或 "-" 后接文本
 *	DTK_TZ - 前导 "+" 或 "-" 后接数字（也吞入 ':'、'.'、'-'）
 *
 * 注意某些字段类型可容纳意外条目：
 *	DTK_NUMBER 容纳日期字段 (yy.ddd)
 *	DTK_STRING 可容纳月份 (January) 与时区 (PST)
 *	DTK_DATE 可容纳时区名 (America/New_York, GMT-8)
 */
int
ParseDateTime(const char *timestr, char *workbuf, size_t buflen,
			  char **field, int *ftype, int maxfields, int *numfields)
{
	int			nf = 0;
	const char *cp = timestr;
	char	   *bufp = workbuf;
	const char *bufend = workbuf + buflen;

	/*
	 * 把 "bufptr" 所指字符置为 "newchar"，并递增
	 * "bufptr"。"end" 给出缓冲区末尾 -- 若
	 * 缓冲区已无空间追加字符就返回错误。注意
	 * "bufptr" 被求值两次。
	 */
#define APPEND_CHAR(bufptr, end, newchar)		\
	do											\
	{											\
		if (((bufptr) + 1) >= (end))			\
			return DTERR_BAD_FORMAT;			\
		*(bufptr)++ = newchar;					\
	} while (0)

	/* 外层循环遍历字段 */
	while (*cp != '\0')
	{
		/* 忽略字段之间的空白 */
		if (isspace((unsigned char) *cp))
		{
			cp++;
			continue;
		}

		/* 记录当前字段的起点 */
		if (nf >= maxfields)
			return DTERR_BAD_FORMAT;
		field[nf] = bufp;

		/* 前导数字?为日期或时间 */
		if (isdigit((unsigned char) *cp))
		{
			APPEND_CHAR(bufp, bufend, *cp++);
			while (isdigit((unsigned char) *cp))
				APPEND_CHAR(bufp, bufend, *cp++);

			/* 时间? */
			if (*cp == ':')
			{
				ftype[nf] = DTK_TIME;
				APPEND_CHAR(bufp, bufend, *cp++);
				while (isdigit((unsigned char) *cp) ||
					   (*cp == ':') || (*cp == '.'))
					APPEND_CHAR(bufp, bufend, *cp++);
			}
			/* 日期字段?允许内嵌文本月份 */
			else if (*cp == '-' || *cp == '/' || *cp == '.')
			{
				/* 保存定界字符供稍后使用 */
				char		delim = *cp;

				APPEND_CHAR(bufp, bufend, *cp++);
				/* 第二字段全为数字？则无内嵌文本月份 */
				if (isdigit((unsigned char) *cp))
				{
					ftype[nf] = ((delim == '.') ? DTK_NUMBER : DTK_DATE);
					while (isdigit((unsigned char) *cp))
						APPEND_CHAR(bufp, bufend, *cp++);

					/*
					 * 坚持定界符必须匹配才能得到三字段日
					 * 期
					 */
					if (*cp == delim)
					{
						ftype[nf] = DTK_DATE;
						APPEND_CHAR(bufp, bufend, *cp++);
						while (isdigit((unsigned char) *cp) || *cp == delim)
							APPEND_CHAR(bufp, bufend, *cp++);
					}
				}
				else
				{
					ftype[nf] = DTK_DATE;
					while (isalnum((unsigned char) *cp) || *cp == delim)
						APPEND_CHAR(bufp, bufend, pg_tolower((unsigned char) *cp++));
				}
			}

			/*
			 * 否则为纯数字，稍后将判定年、月、日或
			 * 连接字段……
			 */
			else
				ftype[nf] = DTK_NUMBER;
		}
		/* 前导小数点？那么是小数秒…… */
		else if (*cp == '.')
		{
			APPEND_CHAR(bufp, bufend, *cp++);
			while (isdigit((unsigned char) *cp))
				APPEND_CHAR(bufp, bufend, *cp++);

			ftype[nf] = DTK_NUMBER;
		}

		/*
		 * 文本？则为日期串、月份、周内日、特殊或时区
		 */
		else if (isalpha((unsigned char) *cp))
		{
			bool		is_date;

			ftype[nf] = DTK_STRING;
			APPEND_CHAR(bufp, bufend, pg_tolower((unsigned char) *cp++));
			while (isalpha((unsigned char) *cp))
				APPEND_CHAR(bufp, bufend, pg_tolower((unsigned char) *cp++));

			/*
			 * 日期可内嵌 '-'、'/' 或 '.' 分隔符。它也可能
			 * 是内嵌 '/'、'+'、'-'、'_'、
			 * ':'（但 '_' 或 ':' 不能是首个标点）。如果
			 * 下一字符是数字或 '+'，我们需要检查目前
			 * 内容是否为已识别的非时区关键字---若是，
			 * 则不要认为这是时区的开头。
			 */
			is_date = false;
			if (*cp == '-' || *cp == '/' || *cp == '.')
				is_date = true;
			else if (*cp == '+' || isdigit((unsigned char) *cp))
			{
				*bufp = '\0';	/* 以 NUL 终止当前字段值 */
				/* 只需搜索核心记号表，无需搜 TZ 名 */
				if (datebsearch(field[nf], datetktbl, szdatetktbl) == NULL)
					is_date = true;
			}
			if (is_date)
			{
				ftype[nf] = DTK_DATE;
				do
				{
					APPEND_CHAR(bufp, bufend, pg_tolower((unsigned char) *cp++));
				} while (*cp == '+' || *cp == '-' ||
						 *cp == '/' || *cp == '_' ||
						 *cp == '.' || *cp == ':' ||
						 isalnum((unsigned char) *cp));
			}
		}
		/* 符号?则为特殊词或数字时区 */
		else if (*cp == '+' || *cp == '-')
		{
			APPEND_CHAR(bufp, bufend, *cp++);
			/* 吞掉前导空白 */
			while (isspace((unsigned char) *cp))
				cp++;
			/* 数字时区？ */
			/* 注意 "DTK_TZ" 也可能是带符号浮点数或 yyyy-mm */
			if (isdigit((unsigned char) *cp))
			{
				ftype[nf] = DTK_TZ;
				APPEND_CHAR(bufp, bufend, *cp++);
				while (isdigit((unsigned char) *cp) ||
					   *cp == ':' || *cp == '.' || *cp == '-')
					APPEND_CHAR(bufp, bufend, *cp++);
			}
			/* 特殊? */
			else if (isalpha((unsigned char) *cp))
			{
				ftype[nf] = DTK_SPECIAL;
				APPEND_CHAR(bufp, bufend, pg_tolower((unsigned char) *cp++));
				while (isalpha((unsigned char) *cp))
					APPEND_CHAR(bufp, bufend, pg_tolower((unsigned char) *cp++));
			}
			/* 否则哪里不对…… */
			else
				return DTERR_BAD_FORMAT;
		}
		/* 忽略其他标点但用作定界符 */
		else if (ispunct((unsigned char) *cp))
		{
			cp++;
			continue;
		}
		/* 否则，某些地方不对…… */
		else
			return DTERR_BAD_FORMAT;

		/* 每字段后强制加一定界符 */
		*bufp++ = '\0';
		nf++;
	}

	*numfields = nf;

	return 0;
}


/* DecodeDateTime()
 * 解释先前解析的字段，用于一般日期与时间。
 * 完整日期返回 0，仅时间返回 1，有问题返回负 DTERR 码。
 * （目前所有调用方把 1 当作错误返回。）
 *
 * 输入是 field[]/ftype[] 数组，长度为 nf。
 * 其他参数都是输出。
 *
 *		外部格式：
 *				"<weekday> <month>-<day>-<year> <hour>:<minute>:<second>"
 *				"Fri Feb-7-1997 15:23:27"
 *				"Feb-7-1997 15:23:27"
 *				"2-7-1997 15:23:27"
 *				"1997-2-7 15:23:27"
 *				 "1997.038 15:23:27"		(年积日 1-366)
 *		也支持紧凑时间输入：
 *				"970207 152327"
 *				"97038 152327"
 *				"20011225T040506.789-07"
 *
 * 若输入串未指定时区，则使用系统提供的函数
 * 取得当前时区。
 *
 * 若日期超出 pg_time_t 的范围（实际上这只有在
 * pg_time_t 仅 32 位时才可能发生），则假定 UTC 时区 -thomas
 * 1997-05-27
 */
int
DecodeDateTime(char **field, int *ftype, int nf,
			   int *dtype, struct pg_tm *tm, fsec_t *fsec, int *tzp,
			   DateTimeErrorExtra *extra)
{
	int			fmask = 0,
				tmask,
				type;
	int			ptype = 0;		/* ISO 与儒略格式的 "前缀类型" */
	int			i;
	int			val;
	int			dterr;
	int			mer = HR24;
	bool		haveTextMonth = false;
	bool		isjulian = false;
	bool		is2digits = false;
	bool		bc = false;
	pg_tz	   *namedTz = NULL;
	pg_tz	   *abbrevTz = NULL;
	pg_tz	   *valtz;
	char	   *abbrev = NULL;
	struct pg_tm cur_tm;

	/*
	 * 我们会坚持至少要有全部日期字段，但会初始化
	 * 其余字段以防稍后未设置……
	 */
	*dtype = DTK_DATE;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	*fsec = 0;
	/* 事先不知道夏令时状态 */
	tm->tm_isdst = -1;
	if (tzp != NULL)
		*tzp = 0;

	for (i = 0; i < nf; i++)
	{
		switch (ftype[i])
		{
			case DTK_DATE:

				/*
				 * 带附加时区的整数儒略日？所有其他
				 * 带 JD 的形式都会被分隔成独立字段，所以
				 * 只处理这一情形。
				 */
				if (ptype == DTK_JULIAN)
				{
					char	   *cp;
					int			jday;

					if (tzp == NULL)
						return DTERR_BAD_FORMAT;

					errno = 0;
					jday = strtoint(field[i], &cp, 10);
					if (errno == ERANGE || jday < 0)
						return DTERR_FIELD_OVERFLOW;

					j2date(jday, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
					isjulian = true;

					/* 从字符串末尾取得时区 */
					dterr = DecodeTimezone(cp, tzp);
					if (dterr)
						return dterr;

					tmask = DTK_DATE_M | DTK_TIME_M | DTK_M(TZ);
					ptype = 0;
					break;
				}

				/*
				 * 已有日期？那么这可能是带内嵌标点的
				 * 时区名（如 "America/New_York"），或是
				 * 带尾随时区的连写时间（如 hhmmss-zz）。
				 * - thomas 2001-12-25
				 *
				 * 若我们已有月和日，就把它视为时区。
				 * 这是为了允许 "mmm dd hhmmss tz year"，
				 * 历史上一直接受。
				 */
				else if (ptype != 0 ||
						 ((fmask & (DTK_M(MONTH) | DTK_M(DAY))) ==
						  (DTK_M(MONTH) | DTK_M(DAY))))
				{
					/* 不接受时区?那么退出…… */
					if (tzp == NULL)
						return DTERR_BAD_FORMAT;

					if (isdigit((unsigned char) *field[i]) || ptype != 0)
					{
						char	   *cp;

						/*
						 * 允许前导 "t" 字段，不许其他单位。
						 */
						if (ptype != 0)
						{
							/* 健全性检查；本测试不应失败 */
							if (ptype != DTK_TIME)
								return DTERR_BAD_FORMAT;
							ptype = 0;
						}

						/*
						 * 以数字开头但我们已有时间
						 * 字段?那么日期和时间都已存在，情况
						 * 不妙…
						 */
						if ((fmask & DTK_TIME_M) == DTK_TIME_M)
							return DTERR_BAD_FORMAT;

						if ((cp = strchr(field[i], '-')) == NULL)
							return DTERR_BAD_FORMAT;

						/* 从字符串末尾取得时区 */
						dterr = DecodeTimezone(cp, tzp);
						if (dterr)
							return dterr;
						*cp = '\0';

						/*
						 * 然后把字段其余部分读作连写
						 * 间
						 */
						dterr = DecodeNumberField(strlen(field[i]), field[i],
												  fmask,
												  &tmask, tm,
												  fsec, &is2digits);
						if (dterr < 0)
							return dterr;

						/*
						 * tmask 在 DecodeNumberField()
						 * 返回后再修改
						 */
						tmask |= DTK_M(TZ);
					}
					else
					{
						namedTz = pg_tzset(field[i]);
						if (!namedTz)
						{
							extra->dtee_timezone = field[i];
							return DTERR_BAD_TIMEZONE;
						}
						/* 时区设置稍后应用 */
						tmask = DTK_M(TZ);
					}
				}
				else
				{
					dterr = DecodeDate(field[i], fmask,
									   &tmask, &is2digits, tm);
					if (dterr)
						return dterr;
				}
				break;

			case DTK_TIME:

				/*
				 * 这可能是 "t" 字段后的 ISO 时间。
				 */
				if (ptype != 0)
				{
					/* 健全性检查；本测试不应失败 */
					if (ptype != DTK_TIME)
						return DTERR_BAD_FORMAT;
					ptype = 0;
				}
				dterr = DecodeTime(field[i], fmask, INTERVAL_FULL_RANGE,
								   &tmask, tm, fsec);
				if (dterr)
					return dterr;

				/* 检查时间溢出 */
				if (time_overflows(tm->tm_hour, tm->tm_min, tm->tm_sec,
								   *fsec))
					return DTERR_FIELD_OVERFLOW;
				break;

			case DTK_TZ:
				{
					int			tz;

					if (tzp == NULL)
						return DTERR_BAD_FORMAT;

					dterr = DecodeTimezone(field[i], &tz);
					if (dterr)
						return dterr;
					*tzp = tz;
					tmask = DTK_M(TZ);
				}
				break;

			case DTK_NUMBER:

				/*
				 * 处理前一字段为本字段作标记的情形
				 */
				if (ptype != 0)
				{
					char	   *cp;
					int			value;

					errno = 0;
					value = strtoint(field[i], &cp, 10);
					if (errno == ERANGE)
						return DTERR_FIELD_OVERFLOW;
					if (*cp != '.' && *cp != '\0')
						return DTERR_BAD_FORMAT;

					switch (ptype)
					{
						case DTK_JULIAN:
							/* 前一字段为儒略日期的标记 */
							if (value < 0)
								return DTERR_FIELD_OVERFLOW;
							tmask = DTK_DATE_M;
							j2date(value, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
							isjulian = true;

							/* 小数儒略日？ */
							if (*cp == '.')
							{
								double		time;

								dterr = ParseFraction(cp, &time);
								if (dterr)
									return dterr;
								time *= USECS_PER_DAY;
								dt2time(time,
										&tm->tm_hour, &tm->tm_min,
										&tm->tm_sec, fsec);
								tmask |= DTK_TIME_M;
							}
							break;

						case DTK_TIME:
							/* 前字段是 ISO 时间 "t" */
							dterr = DecodeNumberField(strlen(field[i]), field[i],
													  (fmask | DTK_DATE_M),
													  &tmask, tm,
													  fsec, &is2digits);
							if (dterr < 0)
								return dterr;
							if (tmask != DTK_TIME_M)
								return DTERR_BAD_FORMAT;
							break;

						default:
							return DTERR_BAD_FORMAT;
							break;
					}

					ptype = 0;
					*dtype = DTK_DATE;
				}
				else
				{
					char	   *cp;
					int			flen;

					flen = strlen(field[i]);
					cp = strchr(field[i], '.');

					/* 内嵌小数点且无日期？ */
					if (cp != NULL && !(fmask & DTK_DATE_M))
					{
						dterr = DecodeDate(field[i], fmask,
										   &tmask, &is2digits, tm);
						if (dterr)
							return dterr;
					}
					/* 小数点前有多位数字？ */
					else if (cp != NULL && flen - strlen(cp) > 2)
					{
						/*
						 * 解释为连写的日期或时间。设置
						 * 类型字段以便稍后解码其他字段。
						 * 例：20011223 或 040506
						 */
						dterr = DecodeNumberField(flen, field[i], fmask,
												  &tmask, tm,
												  fsec, &is2digits);
						if (dterr < 0)
							return dterr;
					}

					/*
					 * 这是 YMD 或 HMS 规格还是年份数？
					 * YMD 与 HMS 要求六位或更多数字，所以
					 * 若是 5 位数即年份。六位或更多，
					 * 我们假定它是 YMD 或 HMS，除非日期与
					 * 时间值均未指定。这迫使 6+
					 * 位年份必须位于字符串末尾，或使用
					 * ISO 日期规格。
					 */
					else if (flen >= 6 && (!(fmask & DTK_DATE_M) ||
										   !(fmask & DTK_TIME_M)))
					{
						dterr = DecodeNumberField(flen, field[i], fmask,
												  &tmask, tm,
												  fsec, &is2digits);
						if (dterr < 0)
							return dterr;
					}
					/* 否则为单日期时间字段 */
					else
					{
						dterr = DecodeNumber(flen, field[i],
											 haveTextMonth, fmask,
											 &tmask, tm,
											 fsec, &is2digits);
						if (dterr)
							return dterr;
					}
				}
				break;

			case DTK_STRING:
			case DTK_SPECIAL:
				/* 时区缩写优先于内建记号 */
				dterr = DecodeTimezoneAbbrev(i, field[i],
											 &type, &val, &valtz, extra);
				if (dterr)
					return dterr;
				if (type == UNKNOWN_FIELD)
					type = DecodeSpecial(i, field[i], &val);
				if (type == IGNORE_DTF)
					continue;

				tmask = DTK_M(type);
				switch (type)
				{
					case RESERV:
						switch (val)
						{
							case DTK_NOW:
								tmask = (DTK_DATE_M | DTK_TIME_M | DTK_M(TZ));
								*dtype = DTK_DATE;
								GetCurrentTimeUsec(tm, fsec, tzp);
								break;

							case DTK_YESTERDAY:
								tmask = DTK_DATE_M;
								*dtype = DTK_DATE;
								GetCurrentDateTime(&cur_tm);
								j2date(date2j(cur_tm.tm_year, cur_tm.tm_mon, cur_tm.tm_mday) - 1,
									   &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
								break;

							case DTK_TODAY:
								tmask = DTK_DATE_M;
								*dtype = DTK_DATE;
								GetCurrentDateTime(&cur_tm);
								tm->tm_year = cur_tm.tm_year;
								tm->tm_mon = cur_tm.tm_mon;
								tm->tm_mday = cur_tm.tm_mday;
								break;

							case DTK_TOMORROW:
								tmask = DTK_DATE_M;
								*dtype = DTK_DATE;
								GetCurrentDateTime(&cur_tm);
								j2date(date2j(cur_tm.tm_year, cur_tm.tm_mon, cur_tm.tm_mday) + 1,
									   &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
								break;

							case DTK_ZULU:
								tmask = (DTK_TIME_M | DTK_M(TZ));
								*dtype = DTK_DATE;
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								if (tzp != NULL)
									*tzp = 0;
								break;

							case DTK_EPOCH:
							case DTK_LATE:
							case DTK_EARLY:
								tmask = (DTK_DATE_M | DTK_TIME_M | DTK_M(TZ));
								*dtype = val;
								/* 调用方对这些码忽略 tm */
								break;

							default:
								elog(ERROR, "unrecognized RESERV datetime token: %d",
									 val);
						}

						break;

					case MONTH:

						/*
						 * 已有（数字）月份？那么看看能否
						 * 替换……
						 */
						if ((fmask & DTK_M(MONTH)) && !haveTextMonth &&
							!(fmask & DTK_M(DAY)) && tm->tm_mon >= 1 &&
							tm->tm_mon <= 31)
						{
							tm->tm_mday = tm->tm_mon;
							tmask = DTK_M(DAY);
						}
						haveTextMonth = true;
						tm->tm_mon = val;
						break;

					case DTZMOD:

						/*
						 * 夏令时修正 (解决 "MET DST"
						 * 语法)
						 */
						tmask |= DTK_M(DTZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return DTERR_BAD_FORMAT;
						*tzp -= val;
						break;

					case DTZ:

						/*
						 * 在此处为 TZ 设掩码 _or_ 稍后取默认
						 * 时区时再检查 DTZ
						 */
						tmask |= DTK_M(TZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return DTERR_BAD_FORMAT;
						*tzp = -val;
						break;

					case TZ:
						tm->tm_isdst = 0;
						if (tzp == NULL)
							return DTERR_BAD_FORMAT;
						*tzp = -val;
						break;

					case DYNTZ:
						tmask |= DTK_M(TZ);
						if (tzp == NULL)
							return DTERR_BAD_FORMAT;
						/* 稍后我们再确定实际偏移 */
						abbrevTz = valtz;
						abbrev = field[i];
						break;

					case AMPM:
						mer = val;
						break;

					case ADBC:
						bc = (val == BC);
						break;

					case DOW:
						tm->tm_wday = val;
						break;

					case UNITS:
						tmask = 0;
						/* 拒连续未处理单位 */
						if (ptype != 0)
							return DTERR_BAD_FORMAT;
						ptype = val;
						break;

					case ISOTIME:

						/*
						 * 这是占位字段 "t"，表示下一
						 * 字段是时间。尝试验证这是否合理。
						 */
						tmask = 0;

						/* 前无日期？则退出 */
						if ((fmask & DTK_DATE_M) != DTK_DATE_M)
							return DTERR_BAD_FORMAT;

						/* 拒连续未处理单位 */
						if (ptype != 0)
							return DTERR_BAD_FORMAT;
						ptype = val;
						break;

					case UNKNOWN_FIELD:

						/*
						 * 在放弃并宣告错误之前，检查它
						 * 是否为全字母的时区名。
						 */
						namedTz = pg_tzset(field[i]);
						if (!namedTz)
							return DTERR_BAD_FORMAT;
						/* 时区设置稍后应用 */
						tmask = DTK_M(TZ);
						break;

					default:
						return DTERR_BAD_FORMAT;
				}
				break;

			default:
				return DTERR_BAD_FORMAT;
		}

		if (tmask & fmask)
			return DTERR_BAD_FORMAT;
		fmask |= tmask;
	}							/* 字段循环结束 */

	/* 前缀类型未获处理则拒绝 */
	if (ptype != 0)
		return DTERR_BAD_FORMAT;

	/* 普通日期规格另加检查（"infinity" 除外） */
	if (*dtype == DTK_DATE)
	{
		/* Y/M/D 字段终检与调整 */
		dterr = ValidateDate(fmask, isjulian, is2digits, bc, tm);
		if (dterr)
			return dterr;

		/* 处理 AM/PM */
		if (mer != HR24 && tm->tm_hour > HOURS_PER_DAY / 2)
			return DTERR_FIELD_OVERFLOW;
		if (mer == AM && tm->tm_hour == HOURS_PER_DAY / 2)
			tm->tm_hour = 0;
		else if (mer == PM && tm->tm_hour != HOURS_PER_DAY / 2)
			tm->tm_hour += HOURS_PER_DAY / 2;

		/* 检查不完整输入 */
		if ((fmask & DTK_DATE_M) != DTK_DATE_M)
		{
			if ((fmask & DTK_TIME_M) == DTK_TIME_M)
				return 1;
			return DTERR_BAD_FORMAT;
		}

		/*
		 * 若已有完整时区规格，则计算偏移（之前无法
		 * 做，因需日期判定夏令时状态）。
		 */
		if (namedTz != NULL)
		{
			/* 完整时区禁用夏令时修饰符 */
			if (fmask & DTK_M(DTZMOD))
				return DTERR_BAD_FORMAT;

			*tzp = DetermineTimeZoneOffset(tm, namedTz);
		}

		/*
		 * 同样，若我们已有动态时区缩写，现在就解析
		 * 它
		 */
		if (abbrevTz != NULL)
		{
			/* 动态时区下不允许夏令时修饰符 */
			if (fmask & DTK_M(DTZMOD))
				return DTERR_BAD_FORMAT;

			*tzp = DetermineTimeZoneAbbrevOffset(tm, abbrev, abbrevTz);
		}

		/* 未指定时区？则用会话时区 */
		if (tzp != NULL && !(fmask & DTK_M(TZ)))
		{
			/*
			 * 夏令时修饰符但无标准时区？那么报
			 * 错
			 */
			if (fmask & DTK_M(DTZMOD))
				return DTERR_BAD_FORMAT;

			*tzp = DetermineTimeZoneOffset(tm, session_timezone);
		}
	}

	return 0;
}


/* DetermineTimeZoneOffset()
 *
 * 给定 struct pg_tm，其 tm_year、tm_mon、tm_mday、tm_hour、tm_min
 * tm_sec 字段已设置，并给定 zic 风格时区定义，确定
 * 该时刻适用的 GMT 偏移与夏令时状态。
 * 相应设置 struct pg_tm 的 tm_isdst 字段，并返回
 * GMT 偏移为函数结果。
 *
 * 注意：若日期超出我们能处理的范围，则返回
 * 零作为 GMT 偏移并置 tm_isdst = 0。此处我们
 * 不抛错，不过上层代码可能会。
 */
int
DetermineTimeZoneOffset(struct pg_tm *tm, pg_tz *tzp)
{
	pg_time_t	t;

	return DetermineTimeZoneOffsetInternal(tm, tzp, &t);
}


/* DetermineTimeZoneOffsetInternal()
 *
 * 同上，但还会把归于该日期/时间的实际 UTC 时间写入
 * *tp。
 *
 * 日期越界时，我们以向 *tp 返回零来搪塞。
 * 这对直接调用方没问题，但也是不把
 * 此工作函数全局暴露的理由。
 *
 * 注意：看起来我们应该为此使用 mktime()，但
 * 惨痛经验的教训并非如此。这段代码远快于多数版本
 * 的 mktime()。
 */
static int
DetermineTimeZoneOffsetInternal(struct pg_tm *tm, pg_tz *tzp, pg_time_t *tp)
{
	int			date,
				sec;
	pg_time_t	day,
				mytime,
				prevtime,
				boundary,
				beforetime,
				aftertime;
	long int	before_gmtoff,
				after_gmtoff;
	int			before_isdst,
				after_isdst;
	int			res;

	/*
	 * 首先，按 GMT 时间解释给定的
	 * y/m/d/h/m/s 生成 pg_time_t 值。若溢出，就搪塞并
	 * 认定时区为 GMT。（对有效儒略日，整数溢出
	 * 64 位 pg_time_t 下本不可能，但仍为安全检查。）
	 */
	if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday))
		goto overflow;
	date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - UNIX_EPOCH_JDATE;

	day = ((pg_time_t) date) * SECS_PER_DAY;
	if (day / SECS_PER_DAY != date)
		goto overflow;
	sec = tm->tm_sec + (tm->tm_min + tm->tm_hour * MINS_PER_HOUR) * SECS_PER_MINUTE;
	mytime = day + sec;
	/* sec >= 0 故溢出仅在 +day 到 -mytime 间 */
	if (mytime < 0 && day > 0)
		goto overflow;

	/*
	 * 找到紧邻目标时间之前或之后的夏令时边界。我们
	 * 假设所有时区的 GMT 偏移都小于 24 小时，且
	 * 夏令时边界相距不会小于 48 小时，所以
	 * 回退 24 小时找 "下一" 边界即可。
	 */
	prevtime = mytime - SECS_PER_DAY;
	if (mytime < 0 && prevtime > 0)
		goto overflow;

	res = pg_next_dst_boundary(&prevtime,
							   &before_gmtoff, &before_isdst,
							   &boundary,
							   &after_gmtoff, &after_isdst,
							   tzp);
	if (res < 0)
		goto overflow;			/* 失败? */

	if (res == 0)
	{
		/* 非夏令时区则简单 */
		tm->tm_isdst = before_isdst;
		*tp = mytime - before_gmtoff;
		return -(int) before_gmtoff;
	}

	/*
	 * 用本地时间调整构造候选 pg_time_t 值
	 */
	beforetime = mytime - before_gmtoff;
	if ((before_gmtoff > 0 &&
		 mytime < 0 && beforetime > 0) ||
		(before_gmtoff <= 0 &&
		 mytime > 0 && beforetime < 0))
		goto overflow;
	aftertime = mytime - after_gmtoff;
	if ((after_gmtoff > 0 &&
		 mytime < 0 && aftertime > 0) ||
		(after_gmtoff <= 0 &&
		 mytime > 0 && aftertime < 0))
		goto overflow;

	/*
	 * 若都在边界时间之前或都在其后，我们
	 * 知道该怎么处理。边界时间本身算作过渡
	 * 之后情形，故可接受 aftertime == boundary。
	 */
	if (beforetime < boundary && aftertime < boundary)
	{
		tm->tm_isdst = before_isdst;
		*tp = beforetime;
		return -(int) before_gmtoff;
	}
	if (beforetime > boundary && aftertime >= boundary)
	{
		tm->tm_isdst = after_isdst;
		*tp = aftertime;
		return -(int) after_gmtoff;
	}

	/*
	 * 因时区过渡导致时间无效或含糊。在
	 * 春季前跳过渡取 "之前" 解释；在
	 * 秋后回过渡中取 "之后"。（我们曾把此检验
	 * 定义并实现为 "偏向标准时间解释"，但该规则
	 * 无助于分辨两个时间均为标准时的情形；
	 * 这确实会发生，如 2014 年 10 月的 Europe/Moscow。
	 * 另外，在某些时区如 Europe/Dublin，
	 * 哪个偏移是 "标准" 时间普遍混淆，
	 * 幸好行为不靠它。）
	 */
	if (beforetime > aftertime)
	{
		tm->tm_isdst = before_isdst;
		*tp = beforetime;
		return -(int) before_gmtoff;
	}
	tm->tm_isdst = after_isdst;
	*tp = aftertime;
	return -(int) after_gmtoff;

overflow:
	/* 日期越界故取 UTC */
	tm->tm_isdst = 0;
	*tp = 0;
	return 0;
}


/* DetermineTimeZoneAbbrevOffset()
 *
 * 确定动态时区缩写（即含义随时间变化的缩写）
 * 应归于的 GMT 偏移与夏令时标志。*tm 含有
 * 应在其处确定含义的本地时间，tm->tm_isdst
 * 接收夏令时标志。
 *
 * 本函数与 DetermineTimeZoneOffset() 的行为不同之处在于：
 * 标准时间或夏令时缩写会强制使用对应的
 * GMT 偏移，即使当时该时区分别处于夏令时
 * 或标准时间。（不过，这仅当我们能把给定缩写
 * 匹配到 IANA 时区数据中出现的某个缩写。否则，
 * 退回 DetermineTimeZoneOffset()。）
 */
int
DetermineTimeZoneAbbrevOffset(struct pg_tm *tm, const char *abbr, pg_tz *tzp)
{
	pg_time_t	t;
	int			zone_offset;
	int			abbr_offset;
	int			abbr_isdst;

	/*
	 * 计算我们想要探测的 UTC 时间。（若溢出，
	 * 就在纪元处探测，这有点随意但大概无关紧要。）
	 */
	zone_offset = DetermineTimeZoneOffsetInternal(tm, tzp, &t);

	/*
	 * 尝试把缩写匹配到时区定义中的某项。
	 */
	if (DetermineTimeZoneAbbrevOffsetInternal(t, abbr, tzp,
											  &abbr_offset, &abbr_isdst))
	{
		/* 成功，故使用缩写专用的答案。 */
		tm->tm_isdst = abbr_isdst;
		return abbr_offset;
	}

	/*
	 * 无匹配，用 DetermineTimeZoneOffsetInternal
	 * 已经得到的答案。
	 */
	return zone_offset;
}


/* DetermineTimeZoneAbbrevOffsetTS()
 *
 * 同上，但探测时间以 TimestampTz 指定（即 UTC 时间），
 * 且夏令时状态返回到 *isdst 而非 tm->tm_isdst。
 */
int
DetermineTimeZoneAbbrevOffsetTS(TimestampTz ts, const char *abbr,
								pg_tz *tzp, int *isdst)
{
	pg_time_t	t = timestamptz_to_time_t(ts);
	int			zone_offset;
	int			abbr_offset;
	int			tz;
	struct pg_tm tm;
	fsec_t		fsec;

	/*
	 * 若缩写匹配到时区数据中任何项，这相当简单。
	 */
	if (DetermineTimeZoneAbbrevOffsetInternal(t, abbr, tzp,
											  &abbr_offset, isdst))
		return abbr_offset;

	/*
	 * 否则，分解 timestamp 以便使用 DetermineTimeZoneOffset。
	 */
	if (timestamp2tm(ts, &tz, &tm, &fsec, NULL, tzp) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	zone_offset = DetermineTimeZoneOffset(&tm, tzp);
	*isdst = tm.tm_isdst;
	return zone_offset;
}


/* DetermineTimeZoneAbbrevOffsetInternal()
 *
 * 上述两函数的主力：从 pg_time_t 探测时刻出发。
 * 成功时 GMT 偏移与夏令时状态返回到 *offset 与 *isdst。
 */
static bool
DetermineTimeZoneAbbrevOffsetInternal(pg_time_t t, const char *abbr, pg_tz *tzp,
									  int *offset, int *isdst)
{
	char		upabbr[TZ_STRLEN_MAX + 1];
	unsigned char *p;
	long int	gmtoff;

	/* 我们需要把缩写强制转为大写 */
	strlcpy(upabbr, abbr, sizeof(upabbr));
	for (p = (unsigned char *) upabbr; *p; p++)
		*p = pg_toupper(*p);

	/* 在此时区中查找缩写的含义 */
	if (pg_interpret_timezone_abbrev(upabbr,
									 &t,
									 &gmtoff,
									 isdst,
									 tzp))
	{
		/* 改变符号以与 DetermineTimeZoneOffset() 一致 */
		*offset = (int) -gmtoff;
		return true;
	}
	return false;
}


/* DecodeTimeOnly()
 * 把解析串解释为仅时间字段。
 * 成功返回 0，检测到伪输入则返回 DTERR 码。
 *
 * 输入是 field[]/ftype[] 数组，长度为 nf。
 * 其他参数都是输出。
 *
 * 注意这里支持时区是为了
 * SQL TIME WITH TIME ZONE，但它暴露
 * SQL 日期/时间标准荒谬之处，因为
 * 我们必须从当前时间推断时区。
 * - thomas 2000-03-10
 * 允许指定日期以获得更好的时区，
 * 如果允许时区。- thomas 2001-12-26
 */
int
DecodeTimeOnly(char **field, int *ftype, int nf,
			   int *dtype, struct pg_tm *tm, fsec_t *fsec, int *tzp,
			   DateTimeErrorExtra *extra)
{
	int			fmask = 0,
				tmask,
				type;
	int			ptype = 0;		/* ISO 与儒略格式的 "前缀类型" */
	int			i;
	int			val;
	int			dterr;
	bool		isjulian = false;
	bool		is2digits = false;
	bool		bc = false;
	int			mer = HR24;
	pg_tz	   *namedTz = NULL;
	pg_tz	   *abbrevTz = NULL;
	char	   *abbrev = NULL;
	pg_tz	   *valtz;

	*dtype = DTK_TIME;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	*fsec = 0;
	/* 事先不知道夏令时状态 */
	tm->tm_isdst = -1;

	if (tzp != NULL)
		*tzp = 0;

	for (i = 0; i < nf; i++)
	{
		switch (ftype[i])
		{
			case DTK_DATE:

				/*
				 * 不允许时区？那么无论如何都不应接受
				 * 日期或时区！
				 */
				if (tzp == NULL)
					return DTERR_BAD_FORMAT;

				/* 在有限情形下我们会接受日期…… */
				if (i == 0 && nf >= 2 &&
					(ftype[nf - 1] == DTK_DATE || ftype[1] == DTK_TIME))
				{
					dterr = DecodeDate(field[i], fmask,
									   &tmask, &is2digits, tm);
					if (dterr)
						return dterr;
				}
				/* 否则这是时间和/或时区 */
				else
				{
					if (isdigit((unsigned char) *field[i]))
					{
						char	   *cp;

						/*
						 * 以数字开头但我们已有时间
						 * 字段？那么时间已存在，麻烦了…
						 */
						if ((fmask & DTK_TIME_M) == DTK_TIME_M)
							return DTERR_BAD_FORMAT;

						/*
						 * 不应到这里并失败，仅健全性检查…
						 */
						if ((cp = strchr(field[i], '-')) == NULL)
							return DTERR_BAD_FORMAT;

						/* 从字符串末尾取得时区 */
						dterr = DecodeTimezone(cp, tzp);
						if (dterr)
							return dterr;
						*cp = '\0';

						/*
						 * 然后把字段其余部分读作连写
						 * 间
						 */
						dterr = DecodeNumberField(strlen(field[i]), field[i],
												  (fmask | DTK_DATE_M),
												  &tmask, tm,
												  fsec, &is2digits);
						if (dterr < 0)
							return dterr;
						ftype[i] = dterr;

						tmask |= DTK_M(TZ);
					}
					else
					{
						namedTz = pg_tzset(field[i]);
						if (!namedTz)
						{
							extra->dtee_timezone = field[i];
							return DTERR_BAD_TIMEZONE;
						}
						/* 时区设置稍后应用 */
						ftype[i] = DTK_TZ;
						tmask = DTK_M(TZ);
					}
				}
				break;

			case DTK_TIME:

				/*
				 * 这可能是 "t" 字段后的 ISO 时间。
				 */
				if (ptype != 0)
				{
					if (ptype != DTK_TIME)
						return DTERR_BAD_FORMAT;
					ptype = 0;
				}

				dterr = DecodeTime(field[i], (fmask | DTK_DATE_M),
								   INTERVAL_FULL_RANGE,
								   &tmask, tm, fsec);
				if (dterr)
					return dterr;
				break;

			case DTK_TZ:
				{
					int			tz;

					if (tzp == NULL)
						return DTERR_BAD_FORMAT;

					dterr = DecodeTimezone(field[i], &tz);
					if (dterr)
						return dterr;
					*tzp = tz;
					tmask = DTK_M(TZ);
				}
				break;

			case DTK_NUMBER:

				/*
				 * 处理前一字段为本字段作标记的情形
				 */
				if (ptype != 0)
				{
					char	   *cp;
					int			value;

					errno = 0;
					value = strtoint(field[i], &cp, 10);
					if (errno == ERANGE)
						return DTERR_FIELD_OVERFLOW;
					if (*cp != '.' && *cp != '\0')
						return DTERR_BAD_FORMAT;

					switch (ptype)
					{
						case DTK_JULIAN:
							/* 前一字段为儒略日期的标记 */
							if (tzp == NULL)
								return DTERR_BAD_FORMAT;
							if (value < 0)
								return DTERR_FIELD_OVERFLOW;
							tmask = DTK_DATE_M;
							j2date(value, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
							isjulian = true;

							if (*cp == '.')
							{
								double		time;

								dterr = ParseFraction(cp, &time);
								if (dterr)
									return dterr;
								time *= USECS_PER_DAY;
								dt2time(time,
										&tm->tm_hour, &tm->tm_min,
										&tm->tm_sec, fsec);
								tmask |= DTK_TIME_M;
							}
							break;

						case DTK_TIME:
							/* 前字段是 ISO 时间 "t" */
							dterr = DecodeNumberField(strlen(field[i]), field[i],
													  (fmask | DTK_DATE_M),
													  &tmask, tm,
													  fsec, &is2digits);
							if (dterr < 0)
								return dterr;
							ftype[i] = dterr;

							if (tmask != DTK_TIME_M)
								return DTERR_BAD_FORMAT;
							break;

						default:
							return DTERR_BAD_FORMAT;
							break;
					}

					ptype = 0;
					*dtype = DTK_DATE;
				}
				else
				{
					char	   *cp;
					int			flen;

					flen = strlen(field[i]);
					cp = strchr(field[i], '.');

					/* 内嵌小数点? */
					if (cp != NULL)
					{
						/*
						 * 在有限情形下我们会接受日
						 * 期…
						 */
						if (i == 0 && nf >= 2 && ftype[nf - 1] == DTK_DATE)
						{
							dterr = DecodeDate(field[i], fmask,
											   &tmask, &is2digits, tm);
							if (dterr)
								return dterr;
						}
						/* 小数点前有多位数字？ */
						else if (flen - strlen(cp) > 2)
						{
							/*
							 * 解释为连写的日期或时间。设置
							 * 类型字段以便稍后解码其他字段。
							 * 例：20011223 或 040506
							 */
							dterr = DecodeNumberField(flen, field[i],
													  (fmask | DTK_DATE_M),
													  &tmask, tm,
													  fsec, &is2digits);
							if (dterr < 0)
								return dterr;
							ftype[i] = dterr;
						}
						else
							return DTERR_BAD_FORMAT;
					}
					else if (flen > 4)
					{
						dterr = DecodeNumberField(flen, field[i],
												  (fmask | DTK_DATE_M),
												  &tmask, tm,
												  fsec, &is2digits);
						if (dterr < 0)
							return dterr;
						ftype[i] = dterr;
					}
					/* 否则为单日期时间字段 */
					else
					{
						dterr = DecodeNumber(flen, field[i],
											 false,
											 (fmask | DTK_DATE_M),
											 &tmask, tm,
											 fsec, &is2digits);
						if (dterr)
							return dterr;
					}
				}
				break;

			case DTK_STRING:
			case DTK_SPECIAL:
				/* 时区缩写优先于内建记号 */
				dterr = DecodeTimezoneAbbrev(i, field[i],
											 &type, &val, &valtz, extra);
				if (dterr)
					return dterr;
				if (type == UNKNOWN_FIELD)
					type = DecodeSpecial(i, field[i], &val);
				if (type == IGNORE_DTF)
					continue;

				tmask = DTK_M(type);
				switch (type)
				{
					case RESERV:
						switch (val)
						{
							case DTK_NOW:
								tmask = DTK_TIME_M;
								*dtype = DTK_TIME;
								GetCurrentTimeUsec(tm, fsec, NULL);
								break;

							case DTK_ZULU:
								tmask = (DTK_TIME_M | DTK_M(TZ));
								*dtype = DTK_TIME;
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								tm->tm_isdst = 0;
								break;

							default:
								return DTERR_BAD_FORMAT;
						}

						break;

					case DTZMOD:

						/*
						 * 夏令时修正 (解决 "MET DST"
						 * 语法)
						 */
						tmask |= DTK_M(DTZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return DTERR_BAD_FORMAT;
						*tzp -= val;
						break;

					case DTZ:

						/*
						 * 在此处为 TZ 设掩码 _or_ 稍后取默认
						 * 时区时再检查 DTZ
						 */
						tmask |= DTK_M(TZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return DTERR_BAD_FORMAT;
						*tzp = -val;
						ftype[i] = DTK_TZ;
						break;

					case TZ:
						tm->tm_isdst = 0;
						if (tzp == NULL)
							return DTERR_BAD_FORMAT;
						*tzp = -val;
						ftype[i] = DTK_TZ;
						break;

					case DYNTZ:
						tmask |= DTK_M(TZ);
						if (tzp == NULL)
							return DTERR_BAD_FORMAT;
						/* 稍后我们再确定实际偏移 */
						abbrevTz = valtz;
						abbrev = field[i];
						ftype[i] = DTK_TZ;
						break;

					case AMPM:
						mer = val;
						break;

					case ADBC:
						bc = (val == BC);
						break;

					case UNITS:
						tmask = 0;
						/* 拒连续未处理单位 */
						if (ptype != 0)
							return DTERR_BAD_FORMAT;
						ptype = val;
						break;

					case ISOTIME:
						tmask = 0;
						/* 拒连续未处理单位 */
						if (ptype != 0)
							return DTERR_BAD_FORMAT;
						ptype = val;
						break;

					case UNKNOWN_FIELD:

						/*
						 * 在放弃并宣告错误之前，检查它
						 * 是否为全字母的时区名。
						 */
						namedTz = pg_tzset(field[i]);
						if (!namedTz)
							return DTERR_BAD_FORMAT;
						/* 时区设置稍后应用 */
						tmask = DTK_M(TZ);
						break;

					default:
						return DTERR_BAD_FORMAT;
				}
				break;

			default:
				return DTERR_BAD_FORMAT;
		}

		if (tmask & fmask)
			return DTERR_BAD_FORMAT;
		fmask |= tmask;
	}							/* 字段循环结束 */

	/* 前缀类型未获处理则拒绝 */
	if (ptype != 0)
		return DTERR_BAD_FORMAT;

	/* Y/M/D 字段终检与调整 */
	dterr = ValidateDate(fmask, isjulian, is2digits, bc, tm);
	if (dterr)
		return dterr;

	/* 处理 AM/PM */
	if (mer != HR24 && tm->tm_hour > HOURS_PER_DAY / 2)
		return DTERR_FIELD_OVERFLOW;
	if (mer == AM && tm->tm_hour == HOURS_PER_DAY / 2)
		tm->tm_hour = 0;
	else if (mer == PM && tm->tm_hour != HOURS_PER_DAY / 2)
		tm->tm_hour += HOURS_PER_DAY / 2;

	/* 检查时间溢出 */
	if (time_overflows(tm->tm_hour, tm->tm_min, tm->tm_sec, *fsec))
		return DTERR_FIELD_OVERFLOW;

	if ((fmask & DTK_TIME_M) != DTK_TIME_M)
		return DTERR_BAD_FORMAT;

	/*
	 * 若已有完整时区规格，则计算偏移（此前无法
	 * 做，因可能需日期判定夏令时状态）。
	 */
	if (namedTz != NULL)
	{
		long int	gmtoff;

		/* 完整时区禁用夏令时修饰符 */
		if (fmask & DTK_M(DTZMOD))
			return DTERR_BAD_FORMAT;

		/* 若非夏令时区则无需知道日期 */
		if (pg_get_timezone_offset(namedTz, &gmtoff))
		{
			*tzp = -(int) gmtoff;
		}
		else
		{
			/* 须有日期 */
			if ((fmask & DTK_DATE_M) != DTK_DATE_M)
				return DTERR_BAD_FORMAT;
			*tzp = DetermineTimeZoneOffset(tm, namedTz);
		}
	}

	/*
	 * 同样，若我们已有动态时区缩写，现在就解析它。
	 */
	if (abbrevTz != NULL)
	{
		struct pg_tm tt,
				   *tmp = &tt;

		/*
		 * 夏令时修饰符但无标准时区？那么报错。
		 */
		if (fmask & DTK_M(DTZMOD))
			return DTERR_BAD_FORMAT;

		if ((fmask & DTK_DATE_M) == 0)
			GetCurrentDateTime(tmp);
		else
		{
			/* 须有日期 */
			if ((fmask & DTK_DATE_M) != DTK_DATE_M)
				return DTERR_BAD_FORMAT;
			tmp->tm_year = tm->tm_year;
			tmp->tm_mon = tm->tm_mon;
			tmp->tm_mday = tm->tm_mday;
		}
		tmp->tm_hour = tm->tm_hour;
		tmp->tm_min = tm->tm_min;
		tmp->tm_sec = tm->tm_sec;
		*tzp = DetermineTimeZoneAbbrevOffset(tmp, abbrev, abbrevTz);
		tm->tm_isdst = tmp->tm_isdst;
	}

	/* 未指定时区？则用会话时区 */
	if (tzp != NULL && !(fmask & DTK_M(TZ)))
	{
		struct pg_tm tt,
				   *tmp = &tt;

		/*
		 * 夏令时修饰符但无标准时区？那么报错。
		 */
		if (fmask & DTK_M(DTZMOD))
			return DTERR_BAD_FORMAT;

		if ((fmask & DTK_DATE_M) == 0)
			GetCurrentDateTime(tmp);
		else
		{
			/* 须有日期 */
			if ((fmask & DTK_DATE_M) != DTK_DATE_M)
				return DTERR_BAD_FORMAT;
			tmp->tm_year = tm->tm_year;
			tmp->tm_mon = tm->tm_mon;
			tmp->tm_mday = tm->tm_mday;
		}
		tmp->tm_hour = tm->tm_hour;
		tmp->tm_min = tm->tm_min;
		tmp->tm_sec = tm->tm_sec;
		*tzp = DetermineTimeZoneOffset(tmp, session_timezone);
		tm->tm_isdst = tmp->tm_isdst;
	}

	return 0;
}

/* DecodeDate()
 * 解码含定界符的日期字符串。
 * 正常返回 0，否则 DTERR 码。
 *
 *	str: 待解析字段
 *	fmask: 已见字段类型的位掩码
 *	*tmask: 接收此处发现字段的位掩码
 *	*is2digits: 若发现两位年份则置为 true
 *	*tm: 字段值存入本结构的相应成员中
 */
static int
DecodeDate(char *str, int fmask, int *tmask, bool *is2digits,
		   struct pg_tm *tm)
{
	fsec_t		fsec;
	int			nf = 0;
	int			i,
				len;
	int			dterr;
	bool		haveTextMonth = false;
	int			type,
				val,
				dmask = 0;
	char	   *field[MAXDATEFIELDS];

	*tmask = 0;

	/* 解析此字符串 */
	while (*str != '\0' && nf < MAXDATEFIELDS)
	{
		/* 跳过字段分隔符 */
		while (*str != '\0' && !isalnum((unsigned char) *str))
			str++;

		if (*str == '\0')
			return DTERR_BAD_FORMAT;	/* 分隔符后是串结尾 */

		field[nf] = str;
		if (isdigit((unsigned char) *str))
		{
			while (isdigit((unsigned char) *str))
				str++;
		}
		else if (isalpha((unsigned char) *str))
		{
			while (isalpha((unsigned char) *str))
				str++;
		}

		/* 去掉任何非数字、非字母字符…… */
		if (*str != '\0')
			*str++ = '\0';
		nf++;
	}

	/* 先查找文本字段，因为它将是无歧义的月份 */
	for (i = 0; i < nf; i++)
	{
		if (isalpha((unsigned char) *field[i]))
		{
			type = DecodeSpecial(i, field[i], &val);
			if (type == IGNORE_DTF)
				continue;

			dmask = DTK_M(type);
			switch (type)
			{
				case MONTH:
					tm->tm_mon = val;
					haveTextMonth = true;
					break;

				default:
					return DTERR_BAD_FORMAT;
			}
			if (fmask & dmask)
				return DTERR_BAD_FORMAT;

			fmask |= dmask;
			*tmask |= dmask;

			/* 标记此字段完成 */
			field[i] = NULL;
		}
	}

	/* 取其余数字字段 */
	for (i = 0; i < nf; i++)
	{
		if (field[i] == NULL)
			continue;

		if ((len = strlen(field[i])) <= 0)
			return DTERR_BAD_FORMAT;

		dterr = DecodeNumber(len, field[i], haveTextMonth, fmask,
							 &dmask, tm,
							 &fsec, is2digits);
		if (dterr)
			return dterr;

		if (fmask & dmask)
			return DTERR_BAD_FORMAT;

		fmask |= dmask;
		*tmask |= dmask;
	}

	if ((fmask & ~(DTK_M(DOY) | DTK_M(TZ))) != DTK_DATE_M)
		return DTERR_BAD_FORMAT;

	/* 字段值验证须等到 ValidateDate() */

	return 0;
}

/* ValidateDate()
 * 检查有效年/月/日值，处理 BC 与年积日情形
 * 正常返回 0，否则 DTERR 码。
 */
int
ValidateDate(int fmask, bool isjulian, bool is2digits, bool bc,
			 struct pg_tm *tm)
{
	if (fmask & DTK_M(YEAR))
	{
		if (isjulian)
		{
			/* tm_year 正确且不应被改动 */
		}
		else if (bc)
		{
			/* 公元/公元前无零年 */
			if (tm->tm_year <= 0)
				return DTERR_FIELD_OVERFLOW;
			/* 内部公元前 1 年为零年，2 为 -1 等 */
			tm->tm_year = -(tm->tm_year - 1);
		}
		else if (is2digits)
		{
			/* 1 或 2 位输入按公元 1970-2069 处理，允许 '0'/'00' */
			if (tm->tm_year < 0)	/* 仅是防呆 */
				return DTERR_FIELD_OVERFLOW;
			if (tm->tm_year < 70)
				tm->tm_year += 2000;
			else if (tm->tm_year < 100)
				tm->tm_year += 1900;
		}
		else
		{
			/* 公元/公元前无零年 */
			if (tm->tm_year <= 0)
				return DTERR_FIELD_OVERFLOW;
		}
	}

	/* 既有正确年份，解码年积日 */
	if (fmask & DTK_M(DOY))
	{
		j2date(date2j(tm->tm_year, 1, 1) + tm->tm_yday - 1,
			   &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
	}

	/* 检查有效月份 */
	if (fmask & DTK_M(MONTH))
	{
		if (tm->tm_mon < 1 || tm->tm_mon > MONTHS_PER_YEAR)
			return DTERR_MD_FIELD_OVERFLOW;
	}

	/* 最小检查有效日 */
	if (fmask & DTK_M(DAY))
	{
		if (tm->tm_mday < 1 || tm->tm_mday > 31)
			return DTERR_MD_FIELD_OVERFLOW;
	}

	if ((fmask & DTK_DATE_M) == DTK_DATE_M)
	{
		/*
		 * 既然已确知月份和年份，现在检查月内
		 * 日是否有效。注意我们这里不用 MD_FIELD_OVERFLOW，
		 * 因 "Feb 29" 不像 YMD 顺序错误。
		 */
		if (tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1])
			return DTERR_FIELD_OVERFLOW;
	}

	return 0;
}


/* DecodeTimeCommon()
 * 解码含定界符的时间字符串。
 * 正常返回 0，否则 DTERR 码。
 * tmask 与 itm 是输出参数。
 *
 * 此代码由 timestamp 与 interval 两种情形共享。
 * 我们返回 struct pg_itm（其中仅 tm_usec、tm_sec、tm_min、
 * tm_hour 字段被使用），再让下面的包装函数
 * 按需做转换与范围检查。
 */
static int
DecodeTimeCommon(char *str, int fmask, int range,
				 int *tmask, struct pg_itm *itm)
{
	char	   *cp;
	int			dterr;
	fsec_t		fsec = 0;

	*tmask = DTK_TIME_M;

	errno = 0;
	itm->tm_hour = strtoi64(str, &cp, 10);
	if (errno == ERANGE)
		return DTERR_FIELD_OVERFLOW;
	if (*cp != ':')
		return DTERR_BAD_FORMAT;
	errno = 0;
	itm->tm_min = strtoint(cp + 1, &cp, 10);
	if (errno == ERANGE)
		return DTERR_FIELD_OVERFLOW;
	if (*cp == '\0')
	{
		itm->tm_sec = 0;
		/* 是 MINUTE TO SECOND 则两字段作 mm:ss */
		if (range == (INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND)))
		{
			if (itm->tm_hour > INT_MAX || itm->tm_hour < INT_MIN)
				return DTERR_FIELD_OVERFLOW;
			itm->tm_sec = itm->tm_min;
			itm->tm_min = (int) itm->tm_hour;
			itm->tm_hour = 0;
		}
	}
	else if (*cp == '.')
	{
		/* mm:ss.sss 即 MINUTE TO SECOND */
		dterr = ParseFractionalSecond(cp, &fsec);
		if (dterr)
			return dterr;
		if (itm->tm_hour > INT_MAX || itm->tm_hour < INT_MIN)
			return DTERR_FIELD_OVERFLOW;
		itm->tm_sec = itm->tm_min;
		itm->tm_min = (int) itm->tm_hour;
		itm->tm_hour = 0;
	}
	else if (*cp == ':')
	{
		errno = 0;
		itm->tm_sec = strtoint(cp + 1, &cp, 10);
		if (errno == ERANGE)
			return DTERR_FIELD_OVERFLOW;
		if (*cp == '.')
		{
			dterr = ParseFractionalSecond(cp, &fsec);
			if (dterr)
				return dterr;
		}
		else if (*cp != '\0')
			return DTERR_BAD_FORMAT;
	}
	else
		return DTERR_BAD_FORMAT;

	/* 健全性检查；tm_hour 范围由调用方检查 */
	if (itm->tm_hour < 0 ||
		itm->tm_min < 0 || itm->tm_min > MINS_PER_HOUR - 1 ||
		itm->tm_sec < 0 || itm->tm_sec > SECS_PER_MINUTE ||
		fsec < 0 || fsec > USECS_PER_SEC)
		return DTERR_FIELD_OVERFLOW;

	itm->tm_usec = (int) fsec;

	return 0;
}

/* DecodeTime()
 * 解码含定界符的时间字符串。
 * 正常返回 0，否则 DTERR 码。
 *
 * 此版本用于 timestamp。结果返回到 *tm 的
 * tm_hour/tm_min/tm_sec 字段，微秒返回到 *fsec。
 */
static int
DecodeTime(char *str, int fmask, int range,
		   int *tmask, struct pg_tm *tm, fsec_t *fsec)
{
	struct pg_itm itm;
	int			dterr;

	dterr = DecodeTimeCommon(str, fmask, range,
							 tmask, &itm);
	if (dterr)
		return dterr;

	if (itm.tm_hour > INT_MAX)
		return DTERR_FIELD_OVERFLOW;
	tm->tm_hour = (int) itm.tm_hour;
	tm->tm_min = itm.tm_min;
	tm->tm_sec = itm.tm_sec;
	*fsec = itm.tm_usec;

	return 0;
}

/* DecodeTimeForInterval()
 * 解码含定界符的时间字符串。
 * 正常返回 0，否则 DTERR 码。
 *
 * 此版本用于 interval。结果返回到 itm_in->
 * tm_usec。
 */
static int
DecodeTimeForInterval(char *str, int fmask, int range,
					  int *tmask, struct pg_itm_in *itm_in)
{
	struct pg_itm itm;
	int			dterr;

	dterr = DecodeTimeCommon(str, fmask, range,
							 tmask, &itm);
	if (dterr)
		return dterr;

	itm_in->tm_usec = itm.tm_usec;
	if (!int64_multiply_add(itm.tm_hour, USECS_PER_HOUR, &itm_in->tm_usec) ||
		!int64_multiply_add(itm.tm_min, USECS_PER_MINUTE, &itm_in->tm_usec) ||
		!int64_multiply_add(itm.tm_sec, USECS_PER_SEC, &itm_in->tm_usec))
		return DTERR_FIELD_OVERFLOW;

	return 0;
}


/* DecodeNumber()
 * 按上下文把纯数字字段解释为日期值。
 * 正常返回 0，否则 DTERR 码。
 */
static int
DecodeNumber(int flen, char *str, bool haveTextMonth, int fmask,
			 int *tmask, struct pg_tm *tm, fsec_t *fsec, bool *is2digits)
{
	int			val;
	char	   *cp;
	int			dterr;

	*tmask = 0;

	errno = 0;
	val = strtoint(str, &cp, 10);
	if (errno == ERANGE)
		return DTERR_FIELD_OVERFLOW;
	if (cp == str)
		return DTERR_BAD_FORMAT;

	if (*cp == '.')
	{
		/*
		 * 小数点前超过两位数字？则可能是日期或
		 * 连写时间：2001.360 20011225 040506.789
		 */
		if (cp - str > 2)
		{
			dterr = DecodeNumberField(flen, str,
									  (fmask | DTK_DATE_M),
									  tmask, tm,
									  fsec, is2digits);
			if (dterr < 0)
				return dterr;
			return 0;
		}

		dterr = ParseFractionalSecond(cp, fsec);
		if (dterr)
			return dterr;
	}
	else if (*cp != '\0')
		return DTERR_BAD_FORMAT;

	/* 年积日特例 */
	if (flen == 3 && (fmask & DTK_DATE_M) == DTK_M(YEAR) && val >= 1 &&
		val <= 366)
	{
		*tmask = (DTK_M(DOY) | DTK_M(MONTH) | DTK_M(DAY));
		tm->tm_yday = val;
		/* tm_mon/tm_mday 实际尚未设置 */
		return 0;
	}

	/* 按目前已有内容分支 */
	switch (fmask & DTK_DATE_M)
	{
		case 0:

			/*
			 * 一无所有；对我们认为输入是什么做判定。
			 * 这里曾有很多启发式规则，但现在
			 * 的共识是保持警惕。它 *必须* 要么是
			 * YYYY-MM-DD（年份字段多于两位），或
			 * DateOrder 的字段顺序。
			 */
			if (flen >= 3 || DateOrder == DATEORDER_YMD)
			{
				*tmask = DTK_M(YEAR);
				tm->tm_year = val;
			}
			else if (DateOrder == DATEORDER_DMY)
			{
				*tmask = DTK_M(DAY);
				tm->tm_mday = val;
			}
			else
			{
				*tmask = DTK_M(MONTH);
				tm->tm_mon = val;
			}
			break;

		case (DTK_M(YEAR)):
			/* 必为 YY-MM-DD 第二字段 */
			*tmask = DTK_M(MONTH);
			tm->tm_mon = val;
			break;

		case (DTK_M(MONTH)):
			if (haveTextMonth)
			{
				/*
				 * 我们处于含文本月份名的日期的第一个
				 * 数字字段。想支持这些无歧义变体：
				 * MON-DD-YYYY、DD-MON-YYYY、YYYY-MON-DD。
				 * 也会在 DMY 或 MDY 模式下接受 MON-DD-YY 或
				 * DD-MON-YY，YMD 模式下也接受 YY-MON-DD。
				 */
				if (flen >= 3 || DateOrder == DATEORDER_YMD)
				{
					*tmask = DTK_M(YEAR);
					tm->tm_year = val;
				}
				else
				{
					*tmask = DTK_M(DAY);
					tm->tm_mday = val;
				}
			}
			else
			{
				/* 必为 MM-DD-YY 第二字段 */
				*tmask = DTK_M(DAY);
				tm->tm_mday = val;
			}
			break;

		case (DTK_M(YEAR) | DTK_M(MONTH)):
			if (haveTextMonth)
			{
				/* YMD 模式下也须接受 DD-MON-YYYY */
				if (flen >= 3 && *is2digits)
				{
					/* 猜首字段为日是错的 */
					*tmask = DTK_M(DAY);	/* YEAR 已被设置 */
					tm->tm_mday = tm->tm_year;
					tm->tm_year = val;
					*is2digits = false;
				}
				else
				{
					*tmask = DTK_M(DAY);
					tm->tm_mday = val;
				}
			}
			else
			{
				/* 必为 YY-MM-DD 第三字段 */
				*tmask = DTK_M(DAY);
				tm->tm_mday = val;
			}
			break;

		case (DTK_M(DAY)):
			/* 必为 DD-MM-YY 第二字段 */
			*tmask = DTK_M(MONTH);
			tm->tm_mon = val;
			break;

		case (DTK_M(MONTH) | DTK_M(DAY)):
			/* 必为 DD-MM-YY/MM-DD-YY 第三字段 */
			*tmask = DTK_M(YEAR);
			tm->tm_year = val;
			break;

		case (DTK_M(YEAR) | DTK_M(MONTH) | DTK_M(DAY)):
			/* 日期已齐，必为时间字段 */
			dterr = DecodeNumberField(flen, str, fmask,
									  tmask, tm,
									  fsec, is2digits);
			if (dterr < 0)
				return dterr;
			return 0;

		default:
			/* 余皆伪输入 */
			return DTERR_BAD_FORMAT;
	}

	/*
	 * 处理年份字段时，若仅有一或两位数字则标记
	 * 待调整
	 */
	if (*tmask == DTK_M(YEAR))
		*is2digits = (flen <= 2);

	return 0;
}


/* DecodeNumberField()
 * 把数字字符串解释为连写的日期或时间字段。
 * 成功返回 DTK 记号 (>= 0)，否则返回 DTERR 码 (< 0)。
 *
 * 利用先前已解码字段的上下文帮助
 * 进行解释。
 */
static int
DecodeNumberField(int len, char *str, int fmask,
				  int *tmask, struct pg_tm *tm, fsec_t *fsec, bool *is2digits)
{
	char	   *cp;

	/*
	 * 有小数点？那么这是日期或带秒字段的东
	 * 西
	 */
	if ((cp = strchr(str, '.')) != NULL)
	{
		/*
		 * 我们能否在这里用 ParseFractionalSecond？尚不清楚
		 * 尾部垃圾是否应拒…
		 */
		if (cp[1] == '\0')
		{
			/* 避免假定 strtod 会接受 "." */
			*fsec = 0;
		}
		else
		{
			double		frac;

			errno = 0;
			frac = strtod(cp, NULL);
			if (errno != 0)
				return DTERR_BAD_FORMAT;
			*fsec = rint(frac * 1000000);
		}
		/* 现在截去小数部分以便后续处理 */
		*cp = '\0';
		len = strlen(str);
	}
	/* 无小数点且无完整日期？ */
	else if ((fmask & DTK_DATE_M) != DTK_DATE_M)
	{
		if (len >= 6)
		{
			*tmask = DTK_DATE_M;

			/*
			 * 从末尾起，头 2 位看作日，次 2 位看作月，
			 * 其余看作年份。
			 */
			tm->tm_mday = atoi(str + (len - 2));
			*(str + (len - 2)) = '\0';
			tm->tm_mon = atoi(str + (len - 4));
			*(str + (len - 4)) = '\0';
			tm->tm_year = atoi(str);
			if ((len - 4) == 2)
				*is2digits = true;

			return DTK_DATE;
		}
	}

	/* 字段未全指定 */
	if ((fmask & DTK_TIME_M) != DTK_TIME_M)
	{
		/* hhmmss */
		if (len == 6)
		{
			*tmask = DTK_TIME_M;
			tm->tm_sec = atoi(str + 4);
			*(str + 4) = '\0';
			tm->tm_min = atoi(str + 2);
			*(str + 2) = '\0';
			tm->tm_hour = atoi(str);

			return DTK_TIME;
		}
		/* hhmm? */
		else if (len == 4)
		{
			*tmask = DTK_TIME_M;
			tm->tm_sec = 0;
			tm->tm_min = atoi(str + 2);
			*(str + 2) = '\0';
			tm->tm_hour = atoi(str);

			return DTK_TIME;
		}
	}

	return DTERR_BAD_FORMAT;
}


/* DecodeTimezone()
 * 把字符串解释为数字时区。
 *
 * 正常返回 0（设 *tzp），否则 DTERR 码。
 */
int
DecodeTimezone(const char *str, int *tzp)
{
	int			tz;
	int			hr,
				min,
				sec = 0;
	char	   *cp;

	/* 首字符须为 "+" 或 "-" */
	if (*str != '+' && *str != '-')
		return DTERR_BAD_FORMAT;

	errno = 0;
	hr = strtoint(str + 1, &cp, 10);
	if (errno == ERANGE)
		return DTERR_TZDISP_OVERFLOW;

	/* 定界符? */
	if (*cp == ':')
	{
		errno = 0;
		min = strtoint(cp + 1, &cp, 10);
		if (errno == ERANGE)
			return DTERR_TZDISP_OVERFLOW;
		if (*cp == ':')
		{
			errno = 0;
			sec = strtoint(cp + 1, &cp, 10);
			if (errno == ERANGE)
				return DTERR_TZDISP_OVERFLOW;
		}
	}
	/* 否则可能连写在一起…… */
	else if (*cp == '\0' && strlen(str) > 3)
	{
		min = hr % 100;
		hr = hr / 100;
		/* 我们本可以但不支持连写的 hhmmss 格式 */
	}
	else
		min = 0;

	/* 范围检查；详见 datatype/timestamp.h */
	if (hr < 0 || hr > MAX_TZDISP_HOUR)
		return DTERR_TZDISP_OVERFLOW;
	if (min < 0 || min >= MINS_PER_HOUR)
		return DTERR_TZDISP_OVERFLOW;
	if (sec < 0 || sec >= SECS_PER_MINUTE)
		return DTERR_TZDISP_OVERFLOW;

	tz = (hr * MINS_PER_HOUR + min) * SECS_PER_MINUTE + sec;
	if (*str == '-')
		tz = -tz;

	*tzp = -tz;

	if (*cp != '\0')
		return DTERR_BAD_FORMAT;

	return 0;
}


/* DecodeTimezoneAbbrev()
 * 如可能，把字符串解释为时区缩写。
 *
 * 把 *ftype 置为缩写类型（TZ、DTZ 或 DYNTZ），或当
 * 字符串非已知缩写时置 UNKNOWN_FIELD。成功时把 *offset
 * 与 *tz 置为 UTC 偏移（TZ 或 DTZ）或底层时区（DYNTZ）。
 * 注意完整时区名（如 America/New_York）不在此处理，
 * 多半出于历史原因。
 *
 * 函数结果为 0 或 DTERR 码；后者情形下按需填 *extra。
 * 未知缩写不算错误情形。还要注意许多调用方假定
 * DTERR 码是
 * DateTimeParseError 无需 "str"/"datatype" 字
 * 符串
 *
 * 给定字符串必须已为小写。
 *
 * 实现缓存查找，因为各日期很可能
 *	在格式上相关联。
 */
int
DecodeTimezoneAbbrev(int field, const char *lowtoken,
					 int *ftype, int *offset, pg_tz **tz,
					 DateTimeErrorExtra *extra)
{
	const datetkn *tp;

	tp = abbrevcache[field];
	/* 用 strncmp 以匹配被截断的记号 */
	if (tp == NULL || strncmp(lowtoken, tp->token, TOKMAXLEN) != 0)
	{
		if (zoneabbrevtbl)
			tp = datebsearch(lowtoken, zoneabbrevtbl->abbrevs,
							 zoneabbrevtbl->numabbrevs);
		else
			tp = NULL;
	}
	if (tp == NULL)
	{
		*ftype = UNKNOWN_FIELD;
		*offset = 0;
		*tz = NULL;
	}
	else
	{
		abbrevcache[field] = tp;
		*ftype = tp->type;
		if (tp->type == DYNTZ)
		{
			*offset = 0;
			*tz = FetchDynamicTimeZone(zoneabbrevtbl, tp, extra);
			if (*tz == NULL)
				return DTERR_BAD_ZONE_ABBREV;
		}
		else
		{
			*offset = tp->value;
			*tz = NULL;
		}
	}

	return 0;
}


/* DecodeSpecial()
 * 用查找表解码文本字符串。
 *
 * 识别 datetktbl 中列出的关键字。
 * 注意：本函数曾也能识别时区缩写，
 * 但已不行；用 DecodeTimezoneAbbrev。
 *
 * 给定字符串必须已为小写。
 *
 * 实现缓存查找，因为各日期很可能
 *	在格式上相关联。
 */
int
DecodeSpecial(int field, const char *lowtoken, int *val)
{
	int			type;
	const datetkn *tp;

	tp = datecache[field];
	/* 用 strncmp 以匹配被截断的记号 */
	if (tp == NULL || strncmp(lowtoken, tp->token, TOKMAXLEN) != 0)
	{
		tp = datebsearch(lowtoken, datetktbl, szdatetktbl);
	}
	if (tp == NULL)
	{
		type = UNKNOWN_FIELD;
		*val = 0;
	}
	else
	{
		datecache[field] = tp;
		type = tp->type;
		*val = tp->value;
	}

	return type;
}


/* DecodeTimezoneName()
 * 把字符串解释为时区缩写或名称。
 * 若名称不被识别则抛错。
 *
 * 返回值指示时区标识符是哪一类：
 *	TZNAME_FIXED_OFFSET: 相对 UTC 偏移
 *	TZNAME_DYNTZ: 动态时区缩写
 *	TZNAME_ZONE: tzdb 全名
 *
 * TZNAME_FIXED_OFFSET 时 *offset 接收 UTC 偏移（以秒计，
 * ISO 符号约定：正值在格林威治以东）。
 * 对于另外两种情形，*tz 接收代表时区名或缩写
 * 底层时区的 timezone 结构。
 */
int
DecodeTimezoneName(const char *tzname, int *offset, pg_tz **tz)
{
	char	   *lowzone;
	int			dterr,
				type;
	DateTimeErrorExtra extra;

	/*
	 * 首先查时区缩写表（处理 "EST" 之类情形），
	 * 若失败则查时区数据库（处理 "America/New_York"
	 * 之类情形）。这与 timestamp 输入检查这些情形的
	 * 顺序一致；这很重要，因为时区数据库不明智地
	 * 把少数几个与偏移缩写同名的时区名用作
	 * 时区名。
	 */

	/* DecodeTimezoneAbbrev 须小写 */
	lowzone = downcase_truncate_identifier(tzname,
										   strlen(tzname),
										   false);

	dterr = DecodeTimezoneAbbrev(0, lowzone, &type, offset, tz, &extra);
	if (dterr)
		DateTimeParseError(dterr, &extra, NULL, NULL, NULL);

	if (type == TZ || type == DTZ)
	{
		/* 固定偏移缩写，返回该偏移 */
		return TZNAME_FIXED_OFFSET;
	}
	else if (type == DYNTZ)
	{
		/* 动态偏移缩写，返回其引用的时区 */
		return TZNAME_DYNTZ;
	}
	else
	{
		/* 试作完整名 */
		*tz = pg_tzset(tzname);
		if (*tz == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone \"%s\" not recognized", tzname)));
		return TZNAME_ZONE;
	}
}

/* DecodeTimezoneNameToTz()
 * 把字符串解释为时区缩写或名称。
 * 若名称不被识别则抛错。
 *
 * 这是 DecodeTimezoneName 的简单包装，总产出
 * pg_tz * 结果。
 */
pg_tz *
DecodeTimezoneNameToTz(const char *tzname)
{
	pg_tz	   *result;
	int			offset;

	if (DecodeTimezoneName(tzname, &offset, &result) == TZNAME_FIXED_OFFSET)
	{
		/* 固定偏移缩写，为它取 pg_tz 描述符 */
		result = pg_tzset_offset(-offset);	/* 翻转到 POSIX 符号约定 */
	}
	return result;
}


/* ClearPgItmIn
 *
 * 把 pg_itm_in 清零
 */
static inline void
ClearPgItmIn(struct pg_itm_in *itm_in)
{
	itm_in->tm_usec = 0;
	itm_in->tm_mday = 0;
	itm_in->tm_mon = 0;
	itm_in->tm_year = 0;
}


/* DecodeInterval()
 * 解释先前解析的字段，用于一般时间间隔。
 * 成功返回 0，检测到伪输入则返回 DTERR 码。
 * dtype 与 itm_in 是输出参数。
 *
 * 允许 "date" 字段为 DTK_DATE，因可能只是
 *	一个无符号浮点数。- thomas 1997-11-16
 *
 * 允许 ISO 风格的时间跨度，天数带隐含单位，
 *	其前为 hh:mm:ss 字段。- thomas 1998-04-30
 */
int
DecodeInterval(char **field, int *ftype, int nf, int range,
			   int *dtype, struct pg_itm_in *itm_in)
{
	bool		force_negative = false;
	bool		is_before = false;
	char	   *cp;
	int			fmask = 0,
				tmask,
				type,
				uval;
	int			i;
	int			dterr;
	int64		val;
	double		fval;

	*dtype = DTK_DELTA;
	type = IGNORE_DTF;
	ClearPgItmIn(itm_in);

	/*----------
	 * SQL 标准定义 interval 字面值
	 *	 '-1 1:00:00'
	 * 表示 "负 1 天且负 1 小时"，而 Postgres 传统上
	 * 把它当作 "负 1 天且正 1 小时" 的意思。在
	 * SQL_STANDARD intervalstyle 中，若无其他显式符号，
	 * 我们会把前导符号应用于所有字段。
	 *
	 * 若另有其他显式符号，我们就不管这些符号。
	 * 这保护我们免于误读 postgres 风格的转储输出，
	 * 因为 postgres 风格的输出代码总是给负
	 * 字段之后的所有字段放显式符号。但注意 SQL
	 * 规格输出有歧义且载入时可能被误读！（所以
	 * 最好用 postgres 而非 SQL 转储）
	 *----------
	 */
	if (IntervalStyle == INTSTYLE_SQL_STANDARD && nf > 0 && *field[0] == '-')
	{
		force_negative = true;
		/* 查其他显式符号 */
		for (i = 1; i < nf; i++)
		{
			if (*field[i] == '-' || *field[i] == '+')
			{
				force_negative = false;
				break;
			}
		}
	}

	/* 反向读列表，在数值前拾取单位 */
	for (i = nf - 1; i >= 0; i--)
	{
		switch (ftype[i])
		{
			case DTK_TIME:
				dterr = DecodeTimeForInterval(field[i], fmask, range,
											  &tmask, itm_in);
				if (dterr)
					return dterr;
				if (force_negative &&
					itm_in->tm_usec > 0)
					itm_in->tm_usec = -itm_in->tm_usec;
				type = DTK_DAY;
				break;

			case DTK_TZ:

				/*
				 * 时区指带前导符号字符且至少
				 * 有一位数字的记号；其中内嵌 ':'、'.',
				 * '-' 等。
				 */
				Assert(*field[i] == '-' || *field[i] == '+');

				/*
				 * 检查带符号的 hh:mm 或 hh:mm:ss。若是，
				 * 则如上 DTK_TIME 处理，另加符号。
				 */
				if (strchr(field[i] + 1, ':') != NULL &&
					DecodeTimeForInterval(field[i] + 1, fmask, range,
										  &tmask, itm_in) == 0)
				{
					if (*field[i] == '-')
					{
						/* 翻转时间符号 */
						if (itm_in->tm_usec == PG_INT64_MIN)
							return DTERR_FIELD_OVERFLOW;
						itm_in->tm_usec = -itm_in->tm_usec;
					}

					if (force_negative &&
						itm_in->tm_usec > 0)
						itm_in->tm_usec = -itm_in->tm_usec;

					/*
					 * 未指定单位则把下一类型设为日。
					 * 这处理 '1 +02:03' 情形，因为我们是
					 * 自右向左读取的。
					 */
					type = DTK_DAY;
					break;
				}

				/*
				 * 否则落入 DTK_NUMBER 情形，能处理带符号
				 * 的浮点数与带符号的年-月值。
				 */

				/* FALLTHROUGH */

			case DTK_DATE:
			case DTK_NUMBER:
				if (type == IGNORE_DTF)
				{
					/* 以 typmod 决定最右字段 */
					switch (range)
					{
						case INTERVAL_MASK(YEAR):
							type = DTK_YEAR;
							break;
						case INTERVAL_MASK(MONTH):
						case INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH):
							type = DTK_MONTH;
							break;
						case INTERVAL_MASK(DAY):
							type = DTK_DAY;
							break;
						case INTERVAL_MASK(HOUR):
						case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR):
							type = DTK_HOUR;
							break;
						case INTERVAL_MASK(MINUTE):
						case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
						case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
							type = DTK_MINUTE;
							break;
						case INTERVAL_MASK(SECOND):
						case INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
						case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
						case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
							type = DTK_SECOND;
							break;
						default:
							type = DTK_SECOND;
							break;
					}
				}

				errno = 0;
				val = strtoi64(field[i], &cp, 10);
				if (errno == ERANGE)
					return DTERR_FIELD_OVERFLOW;

				if (*cp == '-')
				{
					/* SQL "年-月" 语法 */
					int			val2;

					val2 = strtoint(cp + 1, &cp, 10);
					if (errno == ERANGE || val2 < 0 || val2 >= MONTHS_PER_YEAR)
						return DTERR_FIELD_OVERFLOW;
					if (*cp != '\0')
						return DTERR_BAD_FORMAT;
					type = DTK_MONTH;
					if (*field[i] == '-')
						val2 = -val2;
					if (pg_mul_s64_overflow(val, MONTHS_PER_YEAR, &val))
						return DTERR_FIELD_OVERFLOW;
					if (pg_add_s64_overflow(val, val2, &val))
						return DTERR_FIELD_OVERFLOW;
					fval = 0;
				}
				else if (*cp == '.')
				{
					dterr = ParseFraction(cp, &fval);
					if (dterr)
						return dterr;
					if (*field[i] == '-')
						fval = -fval;
				}
				else if (*cp == '\0')
					fval = 0;
				else
					return DTERR_BAD_FORMAT;

				tmask = 0;		/* DTK_M(type); */

				if (force_negative)
				{
					/* val 与 fval 应同号，但仍测试 */
					if (val > 0)
						val = -val;
					if (fval > 0)
						fval = -fval;
				}

				switch (type)
				{
					case DTK_MICROSEC:
						if (!AdjustMicroseconds(val, fval, 1, itm_in))
							return DTERR_FIELD_OVERFLOW;
						tmask = DTK_M(MICROSECOND);
						break;

					case DTK_MILLISEC:
						if (!AdjustMicroseconds(val, fval, 1000, itm_in))
							return DTERR_FIELD_OVERFLOW;
						tmask = DTK_M(MILLISECOND);
						break;

					case DTK_SECOND:
						if (!AdjustMicroseconds(val, fval, USECS_PER_SEC, itm_in))
							return DTERR_FIELD_OVERFLOW;

						/*
						 * 若指定任何亚秒，也把这些微秒与
						 * 毫秒输入考虑在内。
						 */
						if (fval == 0)
							tmask = DTK_M(SECOND);
						else
							tmask = DTK_ALL_SECS_M;
						break;

					case DTK_MINUTE:
						if (!AdjustMicroseconds(val, fval, USECS_PER_MINUTE, itm_in))
							return DTERR_FIELD_OVERFLOW;
						tmask = DTK_M(MINUTE);
						break;

					case DTK_HOUR:
						if (!AdjustMicroseconds(val, fval, USECS_PER_HOUR, itm_in))
							return DTERR_FIELD_OVERFLOW;
						tmask = DTK_M(HOUR);
						type = DTK_DAY; /* 供下一字段用 */
						break;

					case DTK_DAY:
						if (!AdjustDays(val, 1, itm_in) ||
							!AdjustFractMicroseconds(fval, USECS_PER_DAY, itm_in))
							return DTERR_FIELD_OVERFLOW;
						tmask = DTK_M(DAY);
						break;

					case DTK_WEEK:
						if (!AdjustDays(val, 7, itm_in) ||
							!AdjustFractDays(fval, 7, itm_in))
							return DTERR_FIELD_OVERFLOW;
						tmask = DTK_M(WEEK);
						break;

					case DTK_MONTH:
						if (!AdjustMonths(val, itm_in) ||
							!AdjustFractDays(fval, DAYS_PER_MONTH, itm_in))
							return DTERR_FIELD_OVERFLOW;
						tmask = DTK_M(MONTH);
						break;

					case DTK_YEAR:
						if (!AdjustYears(val, 1, itm_in) ||
							!AdjustFractYears(fval, 1, itm_in))
							return DTERR_FIELD_OVERFLOW;
						tmask = DTK_M(YEAR);
						break;

					case DTK_DECADE:
						if (!AdjustYears(val, 10, itm_in) ||
							!AdjustFractYears(fval, 10, itm_in))
							return DTERR_FIELD_OVERFLOW;
						tmask = DTK_M(DECADE);
						break;

					case DTK_CENTURY:
						if (!AdjustYears(val, 100, itm_in) ||
							!AdjustFractYears(fval, 100, itm_in))
							return DTERR_FIELD_OVERFLOW;
						tmask = DTK_M(CENTURY);
						break;

					case DTK_MILLENNIUM:
						if (!AdjustYears(val, 1000, itm_in) ||
							!AdjustFractYears(fval, 1000, itm_in))
							return DTERR_FIELD_OVERFLOW;
						tmask = DTK_M(MILLENNIUM);
						break;

					default:
						return DTERR_BAD_FORMAT;
				}
				break;

			case DTK_STRING:
			case DTK_SPECIAL:
				type = DecodeUnits(i, field[i], &uval);
				if (type == IGNORE_DTF)
					continue;

				tmask = 0;		/* DTK_M(type); */
				switch (type)
				{
					case UNITS:
						type = uval;
						break;

					case AGO:
						is_before = true;
						type = uval;
						break;

					case RESERV:
						tmask = (DTK_DATE_M | DTK_TIME_M);
						*dtype = uval;
						break;

					default:
						return DTERR_BAD_FORMAT;
				}
				break;

			default:
				return DTERR_BAD_FORMAT;
		}

		if (tmask & fmask)
			return DTERR_BAD_FORMAT;
		fmask |= tmask;
	}

	/* 确保至少找到一个时间字段 */
	if (fmask == 0)
		return DTERR_BAD_FORMAT;

	/* AGO 一律取反 */
	if (is_before)
	{
		if (itm_in->tm_usec == PG_INT64_MIN ||
			itm_in->tm_mday == INT_MIN ||
			itm_in->tm_mon == INT_MIN ||
			itm_in->tm_year == INT_MIN)
			return DTERR_FIELD_OVERFLOW;

		itm_in->tm_usec = -itm_in->tm_usec;
		itm_in->tm_mday = -itm_in->tm_mday;
		itm_in->tm_mon = -itm_in->tm_mon;
		itm_in->tm_year = -itm_in->tm_year;
	}

	return 0;
}


/*
 * 用于避免 DecodeISO8601Interval 代码重复的辅助函数。
 *
 * 解析十进制值并把它拆分为整数与小数部分。
 * 把 *endptr 置为已解析子串 end+1。
 * 返回 0 或 DTERR 码
 */
static int
ParseISO8601Number(char *str, char **endptr, int64 *ipart, double *fpart)
{
	double		val;

	/*
	 * 历史上它接受 strtod() 会接受的任何东西，
	 * 尤其包括 "e" 记法，所以继续这么做。这有点
	 * 令人不快，因为 double 的精度低于 int64，所以
	 * 对 2^53 以上的输入我们会损失精度。
	 * 不过历史上我们拒绝过 int32 范围之外的输入，
	 * 这让该顾虑变得无关紧要。我们现在拒绝 abs(val)
	 * 超过 1.0e15（比 2^50 略小的整数），这样任何
	 * 被接受的值的整数部分都是精确的，从而小数部分的
	 * abs(*fpart) 小于 1。在没有现场投诉的情况下，
	 * 似乎不值得更细。
	 */
	if (!(isdigit((unsigned char) *str) || *str == '-' || *str == '.'))
		return DTERR_BAD_FORMAT;
	errno = 0;
	val = strtod(str, endptr);
	/* 没看到形似 double 的东西？ */
	if (*endptr == str || errno != 0)
		return DTERR_BAD_FORMAT;
	/* 当心溢出（包括无穷），也拒绝 NaN */
	if (isnan(val) || val < -1.0e15 || val > 1.0e15)
		return DTERR_FIELD_OVERFLOW;
	/* 确保向零截断（cf dtrunc()） */
	if (val >= 0)
		*ipart = (int64) floor(val);
	else
		*ipart = (int64) -floor(-val);
	*fpart = val - *ipart;
	/* 预期成立 */
	Assert(*fpart > -1.0 && *fpart < 1.0);
	return 0;
}

/*
 * 确定有效 ISO 8601 数字字段中整数部分的位数
 * （应忽略符号与任何小数部分）
 */
static int
ISO8601IntegerWidth(char *fieldstart)
{
	/* 可能有前导 '-' */
	if (*fieldstart == '-')
		fieldstart++;
	return strspn(fieldstart, "0123456789");
}


/* DecodeISO8601Interval()
 *	解码 "指示符格式"（4.4.3.2 节）或"替代格式"
 *	（4.4.3.3 节）的 ISO 8601 时间间隔。
 *	例:  P1D	为 1 天
 *			   PT1H: 1 小时
 *			   P2Y6M7DT1H30M 为 2 年 6 月 7 天 1 小时 30 分
 *			   P0002-06-07T01:30:00 替代格式的同值
 *
 * 成功返回 0，检测到伪输入则返回 DTERR 码。
 * 注意：若输入不像 ISO8601，错误码应为 DTERR_BAD_FORMAT，
 * 否则可能导致意外的错误消息。
 * dtype 与 itm_in 是输出参数。
 *
 *	与规范有两处不同：
 *	 - 周字段（'W'）可与其他单位共存
 *	 - 允许在非最低有效单位的字段中使用小数。
 */
int
DecodeISO8601Interval(char *str,
					  int *dtype, struct pg_itm_in *itm_in)
{
	bool		datepart = true;
	bool		havefield = false;

	*dtype = DTK_DELTA;
	ClearPgItmIn(itm_in);

	if (strlen(str) < 2 || str[0] != 'P')
		return DTERR_BAD_FORMAT;

	str++;
	while (*str)
	{
		char	   *fieldstart;
		int64		val;
		double		fval;
		char		unit;
		int			dterr;

		if (*str == 'T')		/* T 表示时间部分的开始 */
		{
			datepart = false;
			havefield = false;
			str++;
			continue;
		}

		fieldstart = str;
		dterr = ParseISO8601Number(str, &str, &val, &fval);
		if (dterr)
			return dterr;

		/*
		 * 注意：这里可能越过字符串末尾。代码在 unit
		 * 为 '\0' 时须退出循环。
		 */
		unit = *str++;

		if (datepart)
		{
			switch (unit)		/* T 之前: Y M W D */
			{
				case 'Y':
					if (!AdjustYears(val, 1, itm_in) ||
						!AdjustFractYears(fval, 1, itm_in))
						return DTERR_FIELD_OVERFLOW;
					break;
				case 'M':
					if (!AdjustMonths(val, itm_in) ||
						!AdjustFractDays(fval, DAYS_PER_MONTH, itm_in))
						return DTERR_FIELD_OVERFLOW;
					break;
				case 'W':
					if (!AdjustDays(val, 7, itm_in) ||
						!AdjustFractDays(fval, 7, itm_in))
						return DTERR_FIELD_OVERFLOW;
					break;
				case 'D':
					if (!AdjustDays(val, 1, itm_in) ||
						!AdjustFractMicroseconds(fval, USECS_PER_DAY, itm_in))
						return DTERR_FIELD_OVERFLOW;
					break;
				case 'T':		/* ISO 8601 4.4.3.3 替代格式/基本格式 */
				case '\0':
					if (ISO8601IntegerWidth(fieldstart) == 8 && !havefield)
					{
						if (!AdjustYears(val / 10000, 1, itm_in) ||
							!AdjustMonths((val / 100) % 100, itm_in) ||
							!AdjustDays(val % 100, 1, itm_in) ||
							!AdjustFractMicroseconds(fval, USECS_PER_DAY, itm_in))
							return DTERR_FIELD_OVERFLOW;
						if (unit == '\0')
							return 0;
						datepart = false;
						havefield = false;
						continue;
					}
					/* 否则落到扩展替代格式 */
					/* FALLTHROUGH */
				case '-':		/* ISO 8601 4.4.3.3 备用格式,
								 * 扩展 */
					if (havefield)
						return DTERR_BAD_FORMAT;

					if (!AdjustYears(val, 1, itm_in) ||
						!AdjustFractYears(fval, 1, itm_in))
						return DTERR_FIELD_OVERFLOW;
					if (unit == '\0')
						return 0;
					if (unit == 'T')
					{
						datepart = false;
						havefield = false;
						continue;
					}

					dterr = ParseISO8601Number(str, &str, &val, &fval);
					if (dterr)
						return dterr;
					if (!AdjustMonths(val, itm_in) ||
						!AdjustFractDays(fval, DAYS_PER_MONTH, itm_in))
						return DTERR_FIELD_OVERFLOW;
					if (*str == '\0')
						return 0;
					if (*str == 'T')
					{
						datepart = false;
						havefield = false;
						continue;
					}
					if (*str != '-')
						return DTERR_BAD_FORMAT;
					str++;

					dterr = ParseISO8601Number(str, &str, &val, &fval);
					if (dterr)
						return dterr;
					if (!AdjustDays(val, 1, itm_in) ||
						!AdjustFractMicroseconds(fval, USECS_PER_DAY, itm_in))
						return DTERR_FIELD_OVERFLOW;
					if (*str == '\0')
						return 0;
					if (*str == 'T')
					{
						datepart = false;
						havefield = false;
						continue;
					}
					return DTERR_BAD_FORMAT;
				default:
					/* 日期单位无效 */
					return DTERR_BAD_FORMAT;
			}
		}
		else
		{
			switch (unit)		/* T 后: H M S */
			{
				case 'H':
					if (!AdjustMicroseconds(val, fval, USECS_PER_HOUR, itm_in))
						return DTERR_FIELD_OVERFLOW;
					break;
				case 'M':
					if (!AdjustMicroseconds(val, fval, USECS_PER_MINUTE, itm_in))
						return DTERR_FIELD_OVERFLOW;
					break;
				case 'S':
					if (!AdjustMicroseconds(val, fval, USECS_PER_SEC, itm_in))
						return DTERR_FIELD_OVERFLOW;
					break;
				case '\0':		/* ISO 8601 4.4.3.3 替代格式 */
					if (ISO8601IntegerWidth(fieldstart) == 6 && !havefield)
					{
						if (!AdjustMicroseconds(val / 10000, 0, USECS_PER_HOUR, itm_in) ||
							!AdjustMicroseconds((val / 100) % 100, 0, USECS_PER_MINUTE, itm_in) ||
							!AdjustMicroseconds(val % 100, 0, USECS_PER_SEC, itm_in) ||
							!AdjustFractMicroseconds(fval, 1, itm_in))
							return DTERR_FIELD_OVERFLOW;
						return 0;
					}
					/* 否则落到扩展替代格式 */
					/* FALLTHROUGH */
				case ':':		/* ISO 8601 4.4.3.3 备用格式,
								 * 扩展 */
					if (havefield)
						return DTERR_BAD_FORMAT;

					if (!AdjustMicroseconds(val, fval, USECS_PER_HOUR, itm_in))
						return DTERR_FIELD_OVERFLOW;
					if (unit == '\0')
						return 0;

					dterr = ParseISO8601Number(str, &str, &val, &fval);
					if (dterr)
						return dterr;
					if (!AdjustMicroseconds(val, fval, USECS_PER_MINUTE, itm_in))
						return DTERR_FIELD_OVERFLOW;
					if (*str == '\0')
						return 0;
					if (*str != ':')
						return DTERR_BAD_FORMAT;
					str++;

					dterr = ParseISO8601Number(str, &str, &val, &fval);
					if (dterr)
						return dterr;
					if (!AdjustMicroseconds(val, fval, USECS_PER_SEC, itm_in))
						return DTERR_FIELD_OVERFLOW;
					if (*str == '\0')
						return 0;
					return DTERR_BAD_FORMAT;

				default:
					/* 时间单位无效 */
					return DTERR_BAD_FORMAT;
			}
		}

		havefield = true;
	}

	return 0;
}


/* DecodeUnits()
 * 用查找表解码文本字符串。
 *
 * 本例程识别与时间间隔单位相关联的关键字。
 *
 * 给定字符串必须已为小写。
 *
 * 实现缓存查找，因为各日期很可能
 *	在格式上相关联。
 */
int
DecodeUnits(int field, const char *lowtoken, int *val)
{
	int			type;
	const datetkn *tp;

	tp = deltacache[field];
	/* 用 strncmp 以匹配被截断的记号 */
	if (tp == NULL || strncmp(lowtoken, tp->token, TOKMAXLEN) != 0)
	{
		tp = datebsearch(lowtoken, deltatktbl, szdeltatktbl);
	}
	if (tp == NULL)
	{
		type = UNKNOWN_FIELD;
		*val = 0;
	}
	else
	{
		deltacache[field] = tp;
		type = tp->type;
		*val = tp->value;
	}

	return type;
}								/* DecodeUnits() */

/*
 * 报告由某个日期时间输入处理例程检测到的错误。
 *
 * dterr 是错误码，*extra 含错误报告所需的
 * 任何辅助信息。特定 dterr 值不需要时
 * 则为 NULL
 *
 * str 是原始输入字符串，datatype 是我们试图接受的
 * 数据类型的名称。（某些 DTERR 码下这些不用、可为
 * NULL）。
 *
 * 若 escontext 指向 ErrorSaveContext 节点，则填充它
 * 而不是抛错。
 *
 * 注意：把 DTERR_INTERVAL_OVERFLOW 与 DTERR_TZDISP_OVERFLOW
 * 从 DTERR_FIELD_OVERFLOW 分出看似无用，但 SQL99 强制三个
 * SQLSTATE 码。
 */
void
DateTimeParseError(int dterr, DateTimeErrorExtra *extra,
				   const char *str, const char *datatype,
				   Node *escontext)
{
	switch (dterr)
	{
		case DTERR_FIELD_OVERFLOW:
			errsave(escontext,
					(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
					 errmsg("date/time field value out of range: \"%s\"",
							str)));
			break;
		case DTERR_MD_FIELD_OVERFLOW:
			/* 同上，但加上 DateStyle 提示 */
			errsave(escontext,
					(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
					 errmsg("date/time field value out of range: \"%s\"",
							str),
					 errhint("Perhaps you need a different \"datestyle\" setting.")));
			break;
		case DTERR_INTERVAL_OVERFLOW:
			errsave(escontext,
					(errcode(ERRCODE_INTERVAL_FIELD_OVERFLOW),
					 errmsg("interval field value out of range: \"%s\"",
							str)));
			break;
		case DTERR_TZDISP_OVERFLOW:
			errsave(escontext,
					(errcode(ERRCODE_INVALID_TIME_ZONE_DISPLACEMENT_VALUE),
					 errmsg("time zone displacement out of range: \"%s\"",
							str)));
			break;
		case DTERR_BAD_TIMEZONE:
			errsave(escontext,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone \"%s\" not recognized",
							extra->dtee_timezone)));
			break;
		case DTERR_BAD_ZONE_ABBREV:
			errsave(escontext,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("time zone \"%s\" not recognized",
							extra->dtee_timezone),
					 errdetail("This time zone name appears in the configuration file for time zone abbreviation \"%s\".",
							   extra->dtee_abbrev)));
			break;
		case DTERR_BAD_FORMAT:
		default:
			errsave(escontext,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							datatype, str)));
			break;
	}
}

/* datebsearch()
 * 二分查找 -- 出自 Knuth (6.2.1) 算法 B。这类特化
 * 情形远快于通用 bsearch()。
 */
static const datetkn *
datebsearch(const char *key, const datetkn *base, int nel)
{
	if (nel > 0)
	{
		const datetkn *last = base + nel - 1,
				   *position;
		int			result;

		while (last >= base)
		{
			position = base + ((last - base) >> 1);
			/* 预检首字符换取额外速度 */
			result = (int) key[0] - (int) position->token[0];
			if (result == 0)
			{
				/* 用 strncmp 以匹配被截断的记号 */
				result = strncmp(key, position->token, TOKMAXLEN);
				if (result == 0)
					return position;
			}
			if (result < 0)
				last = position - 1;
			else
				base = position + 1;
		}
	}
	return NULL;
}

/* EncodeTimezone()
 *		把数字时区偏移的表示复制到 str。
 *
 * 返回指向字符串新末尾的指针。不在那里放
 * NUL 终止符；调用方需自行负责 str 的 NUL 终止。
 */
static char *
EncodeTimezone(char *str, int tz, int style)
{
	int			hour,
				min,
				sec;

	sec = abs(tz);
	min = sec / SECS_PER_MINUTE;
	sec -= min * SECS_PER_MINUTE;
	hour = min / MINS_PER_HOUR;
	min -= hour * MINS_PER_HOUR;

	/* TZ 相对我们欲显示的符号取反 */
	*str++ = (tz <= 0 ? '+' : '-');

	if (sec != 0)
	{
		str = pg_ultostr_zeropad(str, hour, 2);
		*str++ = ':';
		str = pg_ultostr_zeropad(str, min, 2);
		*str++ = ':';
		str = pg_ultostr_zeropad(str, sec, 2);
	}
	else if (min != 0 || style == USE_XSD_DATES)
	{
		str = pg_ultostr_zeropad(str, hour, 2);
		*str++ = ':';
		str = pg_ultostr_zeropad(str, min, 2);
	}
	else
		str = pg_ultostr_zeropad(str, hour, 2);
	return str;
}

/* EncodeDateOnly()
 * 按本地时间编码
 */
void
EncodeDateOnly(struct pg_tm *tm, int style, char *str)
{
	Assert(tm->tm_mon >= 1 && tm->tm_mon <= MONTHS_PER_YEAR);

	switch (style)
	{
		case USE_ISO_DATES:
		case USE_XSD_DATES:
			/* 与 ISO 格式兼容 */
			str = pg_ultostr_zeropad(str,
									 (tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1), 4);
			*str++ = '-';
			str = pg_ultostr_zeropad(str, tm->tm_mon, 2);
			*str++ = '-';
			str = pg_ultostr_zeropad(str, tm->tm_mday, 2);
			break;

		case USE_SQL_DATES:
			/* 兼容 Oracle/Ingres 格式 */
			if (DateOrder == DATEORDER_DMY)
			{
				str = pg_ultostr_zeropad(str, tm->tm_mday, 2);
				*str++ = '/';
				str = pg_ultostr_zeropad(str, tm->tm_mon, 2);
			}
			else
			{
				str = pg_ultostr_zeropad(str, tm->tm_mon, 2);
				*str++ = '/';
				str = pg_ultostr_zeropad(str, tm->tm_mday, 2);
			}
			*str++ = '/';
			str = pg_ultostr_zeropad(str,
									 (tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1), 4);
			break;

		case USE_GERMAN_DATES:
			/* 德国式格式 */
			str = pg_ultostr_zeropad(str, tm->tm_mday, 2);
			*str++ = '.';
			str = pg_ultostr_zeropad(str, tm->tm_mon, 2);
			*str++ = '.';
			str = pg_ultostr_zeropad(str,
									 (tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1), 4);
			break;

		case USE_POSTGRES_DATES:
		default:
			/* Postgres 仅日期风格 */
			if (DateOrder == DATEORDER_DMY)
			{
				str = pg_ultostr_zeropad(str, tm->tm_mday, 2);
				*str++ = '-';
				str = pg_ultostr_zeropad(str, tm->tm_mon, 2);
			}
			else
			{
				str = pg_ultostr_zeropad(str, tm->tm_mon, 2);
				*str++ = '-';
				str = pg_ultostr_zeropad(str, tm->tm_mday, 2);
			}
			*str++ = '-';
			str = pg_ultostr_zeropad(str,
									 (tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1), 4);
			break;
	}

	if (tm->tm_year <= 0)
	{
		memcpy(str, " BC", 3);	/* 不复制 NUL */
		str += 3;
	}
	*str = '\0';
}


/* EncodeTimeOnly()
 * 仅编码时间字段。
 *
 * tm 与 fsec 是要编码的值，print_tz 决定是否包含
 * 时区（time 与 timetz 类型的差别所在），tz 是
 * 数字时区偏移，style 是日期风格，str 是写出之
 * 处
 */
void
EncodeTimeOnly(struct pg_tm *tm, fsec_t fsec, bool print_tz, int tz, int style, char *str)
{
	str = pg_ultostr_zeropad(str, tm->tm_hour, 2);
	*str++ = ':';
	str = pg_ultostr_zeropad(str, tm->tm_min, 2);
	*str++ = ':';
	str = AppendSeconds(str, tm->tm_sec, fsec, MAX_TIME_PRECISION, true);
	if (print_tz)
		str = EncodeTimezone(str, tz, style);
	*str = '\0';
}


/* EncodeDateTime()
 * 编码解释为本地时间的日期时间。
 *
 * tm 与 fsec 是要编码的值，print_tz 决定是否包含
 * 时区（timestamp 与 timestamptz 类型的差别），tz 是
 * 数字时区偏移，tzn 是文本时区，若指定它则某些
 * 风格会用它替代 tz，style 是日期风格，str 是输出
 * 位置。
 *
 * 日期风格：
 *	Postgres - day mon hh:mm:ss yyyy tz
 *	SQL - mm/dd/yyyy hh:mm:ss.ss tz
 *	ISO - yyyy-mm-dd hh:mm:ss+/-tz
 *	German - dd.mm.yyyy hh:mm:ss tz
 *	XSD - yyyy-mm-ddThh:mm:ss.ss+/-tz
 */
void
EncodeDateTime(struct pg_tm *tm, fsec_t fsec, bool print_tz, int tz, const char *tzn, int style, char *str)
{
	int			day;

	Assert(tm->tm_mon >= 1 && tm->tm_mon <= MONTHS_PER_YEAR);

	/*
	 * 负 tm_isdst 表示我们没有有效的时区转换。
	 */
	if (tm->tm_isdst < 0)
		print_tz = false;

	switch (style)
	{
		case USE_ISO_DATES:
		case USE_XSD_DATES:
			/* 兼容 ISO-8601 格式 */
			str = pg_ultostr_zeropad(str,
									 (tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1), 4);
			*str++ = '-';
			str = pg_ultostr_zeropad(str, tm->tm_mon, 2);
			*str++ = '-';
			str = pg_ultostr_zeropad(str, tm->tm_mday, 2);
			*str++ = (style == USE_ISO_DATES) ? ' ' : 'T';
			str = pg_ultostr_zeropad(str, tm->tm_hour, 2);
			*str++ = ':';
			str = pg_ultostr_zeropad(str, tm->tm_min, 2);
			*str++ = ':';
			str = AppendTimestampSeconds(str, tm, fsec);
			if (print_tz)
				str = EncodeTimezone(str, tz, style);
			break;

		case USE_SQL_DATES:
			/* 兼容 Oracle/Ingres 格式 */
			if (DateOrder == DATEORDER_DMY)
			{
				str = pg_ultostr_zeropad(str, tm->tm_mday, 2);
				*str++ = '/';
				str = pg_ultostr_zeropad(str, tm->tm_mon, 2);
			}
			else
			{
				str = pg_ultostr_zeropad(str, tm->tm_mon, 2);
				*str++ = '/';
				str = pg_ultostr_zeropad(str, tm->tm_mday, 2);
			}
			*str++ = '/';
			str = pg_ultostr_zeropad(str,
									 (tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1), 4);
			*str++ = ' ';
			str = pg_ultostr_zeropad(str, tm->tm_hour, 2);
			*str++ = ':';
			str = pg_ultostr_zeropad(str, tm->tm_min, 2);
			*str++ = ':';
			str = AppendTimestampSeconds(str, tm, fsec);

			/*
			 * 注意：若时区名将来包含非 ASCII 字符，这里
			 * %.*s 的使用就有风险，因为我们没有小心地做
			 * 编码感知的裁剪。不过 IANA 数据库中的
			 * TZ 缩写都是纯 ASCII。
			 */
			if (print_tz)
			{
				if (tzn)
				{
					sprintf(str, " %.*s", MAXTZLEN, tzn);
					str += strlen(str);
				}
				else
					str = EncodeTimezone(str, tz, style);
			}
			break;

		case USE_GERMAN_DATES:
			/* 德国风格变体 */
			str = pg_ultostr_zeropad(str, tm->tm_mday, 2);
			*str++ = '.';
			str = pg_ultostr_zeropad(str, tm->tm_mon, 2);
			*str++ = '.';
			str = pg_ultostr_zeropad(str,
									 (tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1), 4);
			*str++ = ' ';
			str = pg_ultostr_zeropad(str, tm->tm_hour, 2);
			*str++ = ':';
			str = pg_ultostr_zeropad(str, tm->tm_min, 2);
			*str++ = ':';
			str = AppendTimestampSeconds(str, tm, fsec);

			if (print_tz)
			{
				if (tzn)
				{
					sprintf(str, " %.*s", MAXTZLEN, tzn);
					str += strlen(str);
				}
				else
					str = EncodeTimezone(str, tz, style);
			}
			break;

		case USE_POSTGRES_DATES:
		default:
			/* 向后兼容传统 Postgres abstime 日期 */
			day = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
			tm->tm_wday = j2day(day);
			memcpy(str, days[tm->tm_wday], 3);
			str += 3;
			*str++ = ' ';
			if (DateOrder == DATEORDER_DMY)
			{
				str = pg_ultostr_zeropad(str, tm->tm_mday, 2);
				*str++ = ' ';
				memcpy(str, months[tm->tm_mon - 1], 3);
				str += 3;
			}
			else
			{
				memcpy(str, months[tm->tm_mon - 1], 3);
				str += 3;
				*str++ = ' ';
				str = pg_ultostr_zeropad(str, tm->tm_mday, 2);
			}
			*str++ = ' ';
			str = pg_ultostr_zeropad(str, tm->tm_hour, 2);
			*str++ = ':';
			str = pg_ultostr_zeropad(str, tm->tm_min, 2);
			*str++ = ':';
			str = AppendTimestampSeconds(str, tm, fsec);
			*str++ = ' ';
			str = pg_ultostr_zeropad(str,
									 (tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1), 4);

			if (print_tz)
			{
				if (tzn)
				{
					sprintf(str, " %.*s", MAXTZLEN, tzn);
					str += strlen(str);
				}
				else
				{
					/*
					 * 有时区但无字符串版。用数字形式，
					 * 但务必加上前导空格，以免把之后会被
					 * 日期/时间解析器拒绝的东西格式化。
					 * - thomas 2001-10-19
					 */
					*str++ = ' ';
					str = EncodeTimezone(str, tz, style);
				}
			}
			break;
	}

	if (tm->tm_year <= 0)
	{
		memcpy(str, " BC", 3);	/* 不复制 NUL */
		str += 3;
	}
	*str = '\0';
}


/*
 * 用于避免 EncodeInterval 代码重复的辅助函数。
 */

/* 追加 ISO-8601 风格的 interval 字段，但仅当值非零 */
static char *
AddISO8601IntPart(char *cp, int64 value, char units)
{
	if (value == 0)
		return cp;
	sprintf(cp, "%lld%c", (long long) value, units);
	return cp + strlen(cp);
}

/* 追加 postgres 风格的 interval 字段，但仅当值非零 */
static char *
AddPostgresIntPart(char *cp, int64 value, const char *units,
				   bool *is_zero, bool *is_before)
{
	if (value == 0)
		return cp;
	sprintf(cp, "%s%s%lld %s%s",
			(!*is_zero) ? " " : "",
			(*is_before && value > 0) ? "+" : "",
			(long long) value,
			units,
			(value != 1) ? "s" : "");

	/*
	 * 每个非零字段为（仅）下一个字段设置 is_before。
	 * 这有点怪，但过去就这样……
	 */
	*is_before = (value < 0);
	*is_zero = false;
	return cp + strlen(cp);
}

/* 追加详细风格的 interval 字段，但仅当值非零时 */
static char *
AddVerboseIntPart(char *cp, int64 value, const char *units,
				  bool *is_zero, bool *is_before)
{
	if (value == 0)
		return cp;
	/* 先设 is_before */
	if (*is_zero)
	{
		*is_before = (value < 0);
		value = i64abs(value);
	}
	else if (*is_before)
		value = -value;
	sprintf(cp, " %lld %s%s", (long long) value, units, (value == 1) ? "" : "s");
	*is_zero = false;
	return cp + strlen(cp);
}


/* EncodeInterval()
 * 把时间结构解释为差值时间并转换为字符串。
 *
 * 支持 "传统 Postgres" 与 ISO-8601 风格。
 * 其实，据我所知 ISO 并未规定时间间隔格式化，
 *	但这个看起来类似绝对日期/时间的规范。
 * - thomas 1998-04-30
 *
 * 其实，据我所知 ISO 8601 确实为 "时间
 * 间隔……时间单位指示符格式"做了规定，
 * 它们相当丑陋。格式如下：
 *	   P1Y1M1DT1H1M1.12345S
 * 但它适合在计算机之间而非与人交换数据。
 * - ron 2003-07-14
 *
 * 而 ISO 的 SQL 2008 标准为
 * "年-月字面值"（形如 '2-3'）以及
 * "日-时字面值"（形如 '4 5:6:7'）
 */
void
EncodeInterval(struct pg_itm *itm, int style, char *str)
{
	char	   *cp = str;
	int			year = itm->tm_year;
	int			mon = itm->tm_mon;
	int64		mday = itm->tm_mday;	/* tm_mday 可为 INT_MIN */
	int64		hour = itm->tm_hour;
	int			min = itm->tm_min;
	int			sec = itm->tm_sec;
	int			fsec = itm->tm_usec;
	bool		is_before = false;
	bool		is_zero = true;

	/*
	 * 年与月的符号保证一致，因为它们内部以
	 * "month" 存储。但判定日与时/分/秒字段
	 * 符号时，还需检查 is_before 与 is_zero 这两
	 * 项。
	 */
	switch (style)
	{
			/* SQL 标准格式 */
		case INTSTYLE_SQL_STANDARD:
			{
				bool		has_negative = year < 0 || mon < 0 ||
					mday < 0 || hour < 0 ||
					min < 0 || sec < 0 || fsec < 0;
				bool		has_positive = year > 0 || mon > 0 ||
					mday > 0 || hour > 0 ||
					min > 0 || sec > 0 || fsec > 0;
				bool		has_year_month = year != 0 || mon != 0;
				bool		has_day_time = mday != 0 || hour != 0 ||
					min != 0 || sec != 0 || fsec != 0;
				bool		has_day = mday != 0;
				bool		sql_standard_value = !(has_negative && has_positive) &&
					!(has_year_month && has_day_time);

				/*
				 * SQL 标准要求 interval 前只有一个符号，
				 * 但若符号混合则做不到。
				 */
				if (has_negative && sql_standard_value)
				{
					*cp++ = '-';
					year = -year;
					mon = -mon;
					mday = -mday;
					hour = -hour;
					min = -min;
					sec = -sec;
					fsec = -fsec;
				}

				if (!has_negative && !has_positive)
				{
					sprintf(cp, "0");
				}
				else if (!sql_standard_value)
				{
					/*
					 * 非 sql 标准 interval 强制输出符号，
					 * 以免与带混合符号的 interval
					 * 分量产生歧义。
					 */
					char		year_sign = (year < 0 || mon < 0) ? '-' : '+';
					char		day_sign = (mday < 0) ? '-' : '+';
					char		sec_sign = (hour < 0 || min < 0 ||
											sec < 0 || fsec < 0) ? '-' : '+';

					sprintf(cp, "%c%d-%d %c%lld %c%lld:%02d:",
							year_sign, abs(year), abs(mon),
							day_sign, (long long) i64abs(mday),
							sec_sign, (long long) i64abs(hour), abs(min));
					cp += strlen(cp);
					cp = AppendSeconds(cp, sec, fsec, MAX_INTERVAL_PRECISION, true);
					*cp = '\0';
				}
				else if (has_year_month)
				{
					sprintf(cp, "%d-%d", year, mon);
				}
				else if (has_day)
				{
					sprintf(cp, "%lld %lld:%02d:",
							(long long) mday, (long long) hour, min);
					cp += strlen(cp);
					cp = AppendSeconds(cp, sec, fsec, MAX_INTERVAL_PRECISION, true);
					*cp = '\0';
				}
				else
				{
					sprintf(cp, "%lld:%02d:", (long long) hour, min);
					cp += strlen(cp);
					cp = AppendSeconds(cp, sec, fsec, MAX_INTERVAL_PRECISION, true);
					*cp = '\0';
				}
			}
			break;

			/* ISO 8601 "仅时长时距" */
		case INTSTYLE_ISO_8601:
			/* 特判零以免打印空 */
			if (year == 0 && mon == 0 && mday == 0 &&
				hour == 0 && min == 0 && sec == 0 && fsec == 0)
			{
				sprintf(cp, "PT0S");
				break;
			}
			*cp++ = 'P';
			cp = AddISO8601IntPart(cp, year, 'Y');
			cp = AddISO8601IntPart(cp, mon, 'M');
			cp = AddISO8601IntPart(cp, mday, 'D');
			if (hour != 0 || min != 0 || sec != 0 || fsec != 0)
				*cp++ = 'T';
			cp = AddISO8601IntPart(cp, hour, 'H');
			cp = AddISO8601IntPart(cp, min, 'M');
			if (sec != 0 || fsec != 0)
			{
				if (sec < 0 || fsec < 0)
					*cp++ = '-';
				cp = AppendSeconds(cp, sec, fsec, MAX_INTERVAL_PRECISION, false);
				*cp++ = 'S';
				*cp++ = '\0';
			}
			break;

			/* 与 postgresql<8.4 兼容（iso 时） */
		case INTSTYLE_POSTGRES:
			cp = AddPostgresIntPart(cp, year, "year", &is_zero, &is_before);

			/*
			 * 理想情况下我们应像对 "year" 与 "day" 那样拼出
			 * "month"，但向后兼容使我们难以修复它。
			 * bjm 2011-05-24
			 */
			cp = AddPostgresIntPart(cp, mon, "mon", &is_zero, &is_before);
			cp = AddPostgresIntPart(cp, mday, "day", &is_zero, &is_before);
			if (is_zero || hour != 0 || min != 0 || sec != 0 || fsec != 0)
			{
				bool		minus = (hour < 0 || min < 0 || sec < 0 || fsec < 0);

				sprintf(cp, "%s%s%02lld:%02d:",
						is_zero ? "" : " ",
						(minus ? "-" : (is_before ? "+" : "")),
						(long long) i64abs(hour), abs(min));
				cp += strlen(cp);
				cp = AppendSeconds(cp, sec, fsec, MAX_INTERVAL_PRECISION, true);
				*cp = '\0';
			}
			break;

			/* 与 postgresql<8.4 兼容（非 iso 时） */
		case INTSTYLE_POSTGRES_VERBOSE:
		default:
			strcpy(cp, "@");
			cp++;
			cp = AddVerboseIntPart(cp, year, "year", &is_zero, &is_before);
			cp = AddVerboseIntPart(cp, mon, "mon", &is_zero, &is_before);
			cp = AddVerboseIntPart(cp, mday, "day", &is_zero, &is_before);
			cp = AddVerboseIntPart(cp, hour, "hour", &is_zero, &is_before);
			cp = AddVerboseIntPart(cp, min, "min", &is_zero, &is_before);
			if (sec != 0 || fsec != 0)
			{
				*cp++ = ' ';
				if (sec < 0 || (sec == 0 && fsec < 0))
				{
					if (is_zero)
						is_before = true;
					else if (!is_before)
						*cp++ = '-';
				}
				else if (is_before)
					*cp++ = '-';
				cp = AppendSeconds(cp, sec, fsec, MAX_INTERVAL_PRECISION, false);
				/* 输出 "ago" 故用 abs() */
				sprintf(cp, " sec%s",
						(abs(sec) != 1 || fsec != 0) ? "s" : "");
				is_zero = false;
			}
			/* 恰为零？则放入无单位零 */
			if (is_zero)
				strcat(cp, " 0");
			if (is_before)
				strcat(cp, " ago");
			break;
	}
}


/*
 * 我们被 datetkn 表排序中的愚蠢错误坑过太
 * 多次了。安排在 postmaster 启动时检查它们。
 */
static bool
CheckDateTokenTable(const char *tablename, const datetkn *base, int nel)
{
	bool		ok = true;
	int			i;

	for (i = 0; i < nel; i++)
	{
		/* 查放不下的记号串 */
		if (strlen(base[i].token) > TOKMAXLEN)
		{
			/* %.*s 安全：全 ASCII 记号 */
			elog(LOG, "token too long in %s table: \"%.*s\"",
				 tablename,
				 TOKMAXLEN + 1, base[i].token);
			ok = false;
			break;				/* 不冒险调用 strcmp */
		}
		/* 顺序检查 */
		if (i > 0 &&
			strcmp(base[i - 1].token, base[i].token) >= 0)
		{
			elog(LOG, "ordering error in %s table: \"%s\" >= \"%s\"",
				 tablename,
				 base[i - 1].token,
				 base[i].token);
			ok = false;
		}
	}
	return ok;
}

bool
CheckDateTokenTables(void)
{
	bool		ok = true;

	Assert(UNIX_EPOCH_JDATE == date2j(1970, 1, 1));
	Assert(POSTGRES_EPOCH_JDATE == date2j(2000, 1, 1));

	ok &= CheckDateTokenTable("datetktbl", datetktbl, szdatetktbl);
	ok &= CheckDateTokenTable("deltatktbl", deltatktbl, szdeltatktbl);
	return ok;
}

/*
 * 时间类型 prosupport 函数的公共代码：如可能，
 * 化简时间类型长度强制函数的调用。
 *
 * time、timetz、timestamp 与 timestamptz 各有一段允许
 * 的精度范围。未指定的精度严格等价于可指定
 * 的最高精度。若该调用强制的精度等于或高于
 * 已知输入所具有的精度，则以 RelabelType 空操作
 * 取代。
 *
 * 输入 Node 总是 FuncExpr，但为了减小 datetime.h
 * 的包含足迹，声明为 Node *。
 *
 * 注意：typmod 越界时 timestamp_scale 会抛错，但从
 * cast 到达不了那里：我们的 typmodin 早已把它拦下。
 */
Node *
TemporalSimplify(int32 max_precis, Node *node)
{
	FuncExpr   *expr = castNode(FuncExpr, node);
	Node	   *ret = NULL;
	Node	   *typmod;

	Assert(list_length(expr->args) >= 2);

	typmod = (Node *) lsecond(expr->args);

	if (IsA(typmod, Const) && !((Const *) typmod)->constisnull)
	{
		Node	   *source = (Node *) linitial(expr->args);
		int32		old_precis = exprTypmod(source);
		int32		new_precis = DatumGetInt32(((Const *) typmod)->constvalue);

		if (new_precis < 0 || new_precis == max_precis ||
			(old_precis >= 0 && new_precis >= old_precis))
			ret = relabel_to_typmod(source, new_precis);
	}

	return ret;
}

/*
 * 本函数在时区配置文件装载或重载期间被调用，
 * 以创建最终的时区记号数组。参数数组已按名称
 * 顺序排好序。
 *
 * 结果是一个 TimeZoneAbbrevTable（必须是单块
 * guc_malloc 块）或分配失败时为 NULL。无其他错误条件。
 */
TimeZoneAbbrevTable *
ConvertTimeZoneAbbrevs(struct tzEntry *abbrevs, int n)
{
	TimeZoneAbbrevTable *tbl;
	Size		tbl_size;
	int			i;

	/* 固定字段与数组区 */
	tbl_size = offsetof(TimeZoneAbbrevTable, abbrevs) +
		n * sizeof(datetkn);
	tbl_size = MAXALIGN(tbl_size);
	/* 累计动态缩写空间 */
	for (i = 0; i < n; i++)
	{
		struct tzEntry *abbr = abbrevs + i;

		if (abbr->zone != NULL)
		{
			Size		dsize;

			dsize = offsetof(DynamicZoneAbbrev, zone) +
				strlen(abbr->zone) + 1;
			tbl_size += MAXALIGN(dsize);
		}
	}

	/* 分配 */
	tbl = guc_malloc(LOG, tbl_size);
	if (!tbl)
		return NULL;

	/* 填充 */
	tbl->tblsize = tbl_size;
	tbl->numabbrevs = n;
	/* 此循环中 tbl_size 重演上面的空间计算 */
	tbl_size = offsetof(TimeZoneAbbrevTable, abbrevs) +
		n * sizeof(datetkn);
	tbl_size = MAXALIGN(tbl_size);
	for (i = 0; i < n; i++)
	{
		struct tzEntry *abbr = abbrevs + i;
		datetkn    *dtoken = tbl->abbrevs + i;

		/* 需要时以 strlcpy 截断 */
		strlcpy(dtoken->token, abbr->abbrev, TOKMAXLEN + 1);
		if (abbr->zone != NULL)
		{
			/* 为此缩写分配 DynamicZoneAbbrev */
			DynamicZoneAbbrev *dtza;
			Size		dsize;

			dtza = (DynamicZoneAbbrev *) ((char *) tbl + tbl_size);
			dtza->tz = NULL;
			strcpy(dtza->zone, abbr->zone);

			dtoken->type = DYNTZ;
			/* 值为表起点到 DynamicZoneAbbrev 偏移 */
			dtoken->value = (int32) tbl_size;

			dsize = offsetof(DynamicZoneAbbrev, zone) +
				strlen(abbr->zone) + 1;
			tbl_size += MAXALIGN(dsize);
		}
		else
		{
			dtoken->type = abbr->is_dst ? DTZ : TZ;
			dtoken->value = abbr->offset;
		}
	}

	/* 断言两循环的大小计算一致 */
	Assert(tbl->tblsize == tbl_size);

	/* 测试时验证 */
	Assert(CheckDateTokenTable("timezone abbreviations", tbl->abbrevs, n));

	return tbl;
}

/*
 * 安装 TimeZoneAbbrevTable 为活动表。
 *
 * 调用方负责确保所传入的表在使用期间不会消失。
 */
void
InstallTimeZoneAbbrevs(TimeZoneAbbrevTable *tbl)
{
	zoneabbrevtbl = tbl;
	/* 重置 abbrevcache，它可能含指向旧表的指针 */
	memset(abbrevcache, 0, sizeof(abbrevcache));
}

/*
 * 为动态缩写定位 pg_tz 时区的辅助子例程。
 *
 * 失败时返回 NULL 并为 DTERR_BAD_ZONE_ABBREV 错误填充 *extra。
 */
static pg_tz *
FetchDynamicTimeZone(TimeZoneAbbrevTable *tbl, const datetkn *tp,
					 DateTimeErrorExtra *extra)
{
	DynamicZoneAbbrev *dtza;

	/* 防止索引越界的健全性检查 */
	Assert(tp->type == DYNTZ);
	Assert(tp->value > 0 && tp->value < tbl->tblsize);

	dtza = (DynamicZoneAbbrev *) ((char *) tbl + tp->value);

	/* 若尚未查找则查找底层时区 */
	if (dtza->tz == NULL)
	{
		dtza->tz = pg_tzset(dtza->zone);
		if (dtza->tz == NULL)
		{
			/* 配置条目中时区名有误 */
			extra->dtee_timezone = dtza->zone;
			extra->dtee_abbrev = tp->token;
		}
	}
	return dtza->tz;
}


/*
 * 本集合返回函数读取所有可用的时区缩写，
 * 并返回 (abbrev, utc_offset, is_dst) 集合。
 */
Datum
pg_timezone_abbrevs(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int		   *pindex;
	Datum		result;
	HeapTuple	tuple;
	Datum		values[3];
	bool		nulls[3] = {0};
	const datetkn *tp;
	char		buffer[TOKMAXLEN + 1];
	int			gmtoffset;
	bool		is_dst;
	unsigned char *p;
	struct pg_itm_in itm_in;
	Interval   *resInterval;

	/* 只在函数首次调用时做 */
	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		/* 创建跨调用的函数上下文以持久化 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * 切换到适合多次函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* 分配用户内存 */
		pindex = (int *) palloc(sizeof(int));
		*pindex = 0;
		funcctx->user_fctx = (void *) pindex;

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = tupdesc;

		MemoryContextSwitchTo(oldcontext);
	}

	/* 每次函数调用都做 */
	funcctx = SRF_PERCALL_SETUP();
	pindex = (int *) funcctx->user_fctx;

	if (zoneabbrevtbl == NULL ||
		*pindex >= zoneabbrevtbl->numabbrevs)
		SRF_RETURN_DONE(funcctx);

	tp = zoneabbrevtbl->abbrevs + *pindex;

	switch (tp->type)
	{
		case TZ:
			gmtoffset = tp->value;
			is_dst = false;
			break;
		case DTZ:
			gmtoffset = tp->value;
			is_dst = true;
			break;
		case DYNTZ:
			{
				/* 定该缩写当前含义 */
				pg_tz	   *tzp;
				DateTimeErrorExtra extra;
				TimestampTz now;
				int			isdst;

				tzp = FetchDynamicTimeZone(zoneabbrevtbl, tp, &extra);
				if (tzp == NULL)
					DateTimeParseError(DTERR_BAD_ZONE_ABBREV, &extra,
									   NULL, NULL, NULL);
				now = GetCurrentTransactionStartTimestamp();
				gmtoffset = -DetermineTimeZoneAbbrevOffsetTS(now,
															 tp->token,
															 tzp,
															 &isdst);
				is_dst = (bool) isdst;
				break;
			}
		default:
			elog(ERROR, "unrecognized timezone type %d", (int) tp->type);
			gmtoffset = 0;		/* 让编译器安静 */
			is_dst = false;
			break;
	}

	/*
	 * 把名称转换为文本，使用与 ParseDateTime() 所用
	 * 相反大写转换。
	 */
	strlcpy(buffer, tp->token, sizeof(buffer));
	for (p = (unsigned char *) buffer; *p; p++)
		*p = pg_toupper(*p);

	values[0] = CStringGetTextDatum(buffer);

	/* 偏移（秒）转 interval；不溢出 */
	MemSet(&itm_in, 0, sizeof(struct pg_itm_in));
	itm_in.tm_usec = (int64) gmtoffset * USECS_PER_SEC;
	resInterval = (Interval *) palloc(sizeof(Interval));
	(void) itmin2interval(&itm_in, resInterval);
	values[1] = IntervalPGetDatum(resInterval);

	values[2] = BoolGetDatum(is_dst);

	(*pindex)++;

	tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	SRF_RETURN_NEXT(funcctx, result);
}

/*
 * 本集合返回函数读取所有可用的完整时区，
 * 并返回 (name, abbrev, utc_offset, is_dst) 集合。
 */
Datum
pg_timezone_names(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	pg_tzenum  *tzenum;
	pg_tz	   *tz;
	Datum		values[4];
	bool		nulls[4] = {0};
	int			tzoff;
	struct pg_tm tm;
	fsec_t		fsec;
	const char *tzn;
	Interval   *resInterval;
	struct pg_itm_in itm_in;

	InitMaterializedSRF(fcinfo, 0);

	/* 初始时区扫描 */
	tzenum = pg_tzenumerate_start();

	/* 搜下一个时区 */
	for (;;)
	{
		tz = pg_tzenumerate_next(tzenum);
		if (!tz)
			break;

		/* now() 转本区本地时间 */
		if (timestamp2tm(GetCurrentTransactionStartTimestamp(),
						 &tzoff, &tm, &fsec, &tzn, tz) != 0)
			continue;			/* 若转换失败则忽略 */

		/*
		 * IANA 那个相当愚蠢的 "Factory" 时区过去会发出
		 * 荒谬的长 "缩写"，如 "Local time zone must be set--see zic
		 * 手册页"或 "Local time zone must be set--use tzsetup"。虽然
		 * 现代版本的 tzdb 发出合理得多的 "-00"，但似乎
		 * 某些蒙昧的打包者仍在黑改 IANA 数据，使其继续
		 * 产生这些字符串。为防止产生过宽的缩写列，
		 * 拒绝荒谬的长缩写。
		 */
		if (tzn && strlen(tzn) > 31)
			continue;

		values[0] = CStringGetTextDatum(pg_get_timezone_name(tz));
		values[1] = CStringGetTextDatum(tzn ? tzn : "");

		/* tzoff 转 interval；不溢出 */
		MemSet(&itm_in, 0, sizeof(struct pg_itm_in));
		itm_in.tm_usec = (int64) -tzoff * USECS_PER_SEC;
		resInterval = (Interval *) palloc(sizeof(Interval));
		(void) itmin2interval(&itm_in, resInterval);
		values[2] = IntervalPGetDatum(resInterval);

		values[3] = BoolGetDatum(tm.tm_isdst > 0);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	pg_tzenumerate_end(tzenum);
	return (Datum) 0;
}
