/*-------------------------------------------------------------------------
 *
 * date.c
 *	  实现 SQL 标准规定的 DATE 与 TIME 数据类型
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/date.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <time.h>

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/supportnodes.h"
#include "parser/scansup.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/numeric.h"
#include "utils/sortsupport.h"

/*
 * gcc 的 -ffast-math 开关会破坏那些期望
 * timeval / SECS_PER_HOUR 之类表达式给出精确结果的
 */
#ifdef __FAST_MATH__
#error -ffast-math is known to break this code
#endif


/* timetypmodin/timetztypmodin 的公共代码 */
static int32
anytime_typmodin(bool istz, ArrayType *ta)
{
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);

	/*
	 * 我们没在这里花大力气打磨错误消息，因为文法
	 * 不应允许 TIME 出现错误的修饰符数量
	 */
	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));

	return anytime_typmod_check(istz, tl[0]);
}

/* 导出以便 parse_expr.c 使用 */
int32
anytime_typmod_check(bool istz, int32 typmod)
{
	if (typmod < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("TIME(%d)%s precision must not be negative",
						typmod, (istz ? " WITH TIME ZONE" : ""))));
	if (typmod > MAX_TIME_PRECISION)
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("TIME(%d)%s precision reduced to maximum allowed, %d",
						typmod, (istz ? " WITH TIME ZONE" : ""),
						MAX_TIME_PRECISION)));
		typmod = MAX_TIME_PRECISION;
	}

	return typmod;
}

/* timetypmodout 与 timetztypmodout 的公共代码 */
static char *
anytime_typmodout(bool istz, int32 typmod)
{
	const char *tz = istz ? " with time zone" : " without time zone";

	if (typmod >= 0)
		return psprintf("(%d)%s", (int) typmod, tz);
	else
		return pstrdup(tz);
}


/*****************************************************************************
 *	 Date ADT
 *****************************************************************************/


/* date_in()
 * 给定日期文本串，转换为内部日期格式。
 */
Datum
date_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	DateADT		date;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;
	int			tzp;
	int			dtype;
	int			nf;
	int			dterr;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		workbuf[MAXDATELEN + 1];
	DateTimeErrorExtra extra;

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeDateTime(field, ftype, nf,
							   &dtype, tm, &fsec, &tzp, &extra);
	if (dterr != 0)
	{
		DateTimeParseError(dterr, &extra, str, "date", escontext);
		PG_RETURN_NULL();
	}

	switch (dtype)
	{
		case DTK_DATE:
			break;

		case DTK_EPOCH:
			GetEpochTime(tm);
			break;

		case DTK_LATE:
			DATE_NOEND(date);
			PG_RETURN_DATEADT(date);

		case DTK_EARLY:
			DATE_NOBEGIN(date);
			PG_RETURN_DATEADT(date);

		default:
			DateTimeParseError(DTERR_BAD_FORMAT, &extra, str, "date", escontext);
			PG_RETURN_NULL();
	}

	/* 在儒略日例程中防止溢出 */
	if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range: \"%s\"", str)));

	date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;

	/* 现在检查刚好越界的日期 */
	if (!IS_VALID_DATE(date))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range: \"%s\"", str)));

	PG_RETURN_DATEADT(date);
}

/* date_out()
 * 给定内部格式日期，转成文本串。
 */
Datum
date_out(PG_FUNCTION_ARGS)
{
	DateADT		date = PG_GETARG_DATEADT(0);
	char	   *result;
	struct pg_tm tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];

	if (DATE_NOT_FINITE(date))
		EncodeSpecialDate(date, buf);
	else
	{
		j2date(date + POSTGRES_EPOCH_JDATE,
			   &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
		EncodeDateOnly(tm, DateStyle, buf);
	}

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		date_recv			- 将外部二进制格式转换为 date
 */
Datum
date_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	DateADT		result;

	result = (DateADT) pq_getmsgint(buf, sizeof(DateADT));

	/* 限制到 date_in() 接受的同一范围 */
	if (DATE_NOT_FINITE(result))
		 /* ok */ ;
	else if (!IS_VALID_DATE(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range")));

	PG_RETURN_DATEADT(result);
}

/*
 *		date_send - 把日期转成二进制格式
 */
Datum
date_send(PG_FUNCTION_ARGS)
{
	DateADT		date = PG_GETARG_DATEADT(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, date);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 *		make_date - date 构造器
 */
Datum
make_date(PG_FUNCTION_ARGS)
{
	struct pg_tm tm;
	DateADT		date;
	int			dterr;
	bool		bc = false;

	tm.tm_year = PG_GETARG_INT32(0);
	tm.tm_mon = PG_GETARG_INT32(1);
	tm.tm_mday = PG_GETARG_INT32(2);

	/* 负年份按 BC 处理 */
	if (tm.tm_year < 0)
	{
		bc = true;
		tm.tm_year = -tm.tm_year;
	}

	dterr = ValidateDate(DTK_DATE_M, false, false, bc, &tm);

	if (dterr != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("date field value out of range: %d-%02d-%02d",
						tm.tm_year, tm.tm_mon, tm.tm_mday)));

	/* 在儒略日例程中防止溢出 */
	if (!IS_VALID_JULIAN(tm.tm_year, tm.tm_mon, tm.tm_mday))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range: %d-%02d-%02d",
						tm.tm_year, tm.tm_mon, tm.tm_mday)));

	date = date2j(tm.tm_year, tm.tm_mon, tm.tm_mday) - POSTGRES_EPOCH_JDATE;

	/* 现在检查刚好越界的日期 */
	if (!IS_VALID_DATE(date))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range: %d-%02d-%02d",
						tm.tm_year, tm.tm_mon, tm.tm_mday)));

	PG_RETURN_DATEADT(date);
}

/*
 * 将保留日期值转换为字符串。
 */
void
EncodeSpecialDate(DateADT dt, char *str)
{
	if (DATE_IS_NOBEGIN(dt))
		strcpy(str, EARLY);
	else if (DATE_IS_NOEND(dt))
		strcpy(str, LATE);
	else						/* 不应发生 */
		elog(ERROR, "invalid argument for EncodeSpecialDate");
}


/*
 * GetSQLCurrentDate -- 实现 CURRENT_DATE
 */
DateADT
GetSQLCurrentDate(void)
{
	struct pg_tm tm;

	static int	cache_year = 0;
	static int	cache_mon = 0;
	static int	cache_mday = 0;
	static DateADT cache_date;

	GetCurrentDateTime(&tm);

	/*
	 * date2j 涉及多次整数除法；而且，除非我们的会话
	 * 跨过本地午夜，否则其实不必执行不止一次。
	 * 所以似乎值得在此设一个单独的缓存。
	 */
	if (tm.tm_year != cache_year ||
		tm.tm_mon != cache_mon ||
		tm.tm_mday != cache_mday)
	{
		cache_date = date2j(tm.tm_year, tm.tm_mon, tm.tm_mday) - POSTGRES_EPOCH_JDATE;
		cache_year = tm.tm_year;
		cache_mon = tm.tm_mon;
		cache_mday = tm.tm_mday;
	}

	return cache_date;
}

/*
 * GetSQLCurrentTime -- 实现 CURRENT_TIME 等
 */
TimeTzADT *
GetSQLCurrentTime(int32 typmod)
{
	TimeTzADT  *result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	GetCurrentTimeUsec(tm, &fsec, &tz);

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));
	tm2timetz(tm, fsec, tz, result);
	AdjustTimeForTypmod(&(result->time), typmod);
	return result;
}

/*
 * GetSQLLocalTime -- 实现 LOCALTIME 等
 */
TimeADT
GetSQLLocalTime(int32 typmod)
{
	TimeADT		result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	GetCurrentTimeUsec(tm, &fsec, &tz);

	tm2time(tm, fsec, &result);
	AdjustTimeForTypmod(&result, typmod);
	return result;
}


/*
 * 日期比较函数
 */

Datum
date_eq(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 == dateVal2);
}

Datum
date_ne(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 != dateVal2);
}

Datum
date_lt(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 < dateVal2);
}

Datum
date_le(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 <= dateVal2);
}

Datum
date_gt(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 > dateVal2);
}

Datum
date_ge(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 >= dateVal2);
}

Datum
date_cmp(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	if (dateVal1 < dateVal2)
		PG_RETURN_INT32(-1);
	else if (dateVal1 > dateVal2)
		PG_RETURN_INT32(1);
	PG_RETURN_INT32(0);
}

Datum
date_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = ssup_datum_int32_cmp;
	PG_RETURN_VOID();
}

Datum
date_finite(PG_FUNCTION_ARGS)
{
	DateADT		date = PG_GETARG_DATEADT(0);

	PG_RETURN_BOOL(!DATE_NOT_FINITE(date));
}

Datum
date_larger(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_DATEADT((dateVal1 > dateVal2) ? dateVal1 : dateVal2);
}

Datum
date_smaller(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_DATEADT((dateVal1 < dateVal2) ? dateVal1 : dateVal2);
}

/* 计算两个日期间的天数差。
 */
Datum
date_mi(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	if (DATE_NOT_FINITE(dateVal1) || DATE_NOT_FINITE(dateVal2))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("cannot subtract infinite dates")));

	PG_RETURN_INT32((int32) (dateVal1 - dateVal2));
}

/* 把若干天加到日期上，得到新日期。
 * 必须同时处理正负天数。
 */
Datum
date_pli(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	int32		days = PG_GETARG_INT32(1);
	DateADT		result;

	if (DATE_NOT_FINITE(dateVal))
		PG_RETURN_DATEADT(dateVal); /* 无法改变无穷 */

	result = dateVal + days;

	/* 检查整数溢出与超出允许范围 */
	if ((days >= 0 ? (result < dateVal) : (result > dateVal)) ||
		!IS_VALID_DATE(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range")));

	PG_RETURN_DATEADT(result);
}

/* 从日期减去若干天，得到新日期。
 */
Datum
date_mii(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	int32		days = PG_GETARG_INT32(1);
	DateADT		result;

	if (DATE_NOT_FINITE(dateVal))
		PG_RETURN_DATEADT(dateVal); /* 无法改变无穷 */

	result = dateVal - days;

	/* 检查整数溢出与超出允许范围 */
	if ((days >= 0 ? (result > dateVal) : (result < dateVal)) ||
		!IS_VALID_DATE(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range")));

	PG_RETURN_DATEADT(result);
}


/*
 * 日期提升为 timestamp
 *
 * 转换成功时，若 *overflow 非 NULL，则把它设为零。
 *
 * 如果日期有限但超出 timestamp 的有效范围，那么：
 * 若 overflow 为 NULL，则抛出越界错误。
 * 若 overflow 非 NULL，则在其中存入 +1 或 -1 以标示
 * 溢出的符号，并返回相应的 timestamp 无穷值。
 *
 * 注意：*overflow = -1 目前实际不可能，因为两
 * 种类型的下界相同，皆为儒略日零。
 */
Timestamp
date2timestamp_opt_overflow(DateADT dateVal, int *overflow)
{
	Timestamp	result;

	if (overflow)
		*overflow = 0;

	if (DATE_IS_NOBEGIN(dateVal))
		TIMESTAMP_NOBEGIN(result);
	else if (DATE_IS_NOEND(dateVal))
		TIMESTAMP_NOEND(result);
	else
	{
		/*
		 * 由于日期与 timestamp 具有相同的最小值，只需检查
		 * 上边界是否溢出。
		 */
		if (dateVal >= (TIMESTAMP_END_JULIAN - POSTGRES_EPOCH_JDATE))
		{
			if (overflow)
			{
				*overflow = 1;
				TIMESTAMP_NOEND(result);
				return result;
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("date out of range for timestamp")));
			}
		}

		/* date 为 2000 起天数，timestamp 为同期微秒... */
		result = dateVal * USECS_PER_DAY;
	}

	return result;
}

/*
 * 把日期提升为 timestamp，溢出则报错。
 */
static TimestampTz
date2timestamp(DateADT dateVal)
{
	return date2timestamp_opt_overflow(dateVal, NULL);
}

/*
 * 提升为带时区 timestamp
 *
 * 转换成功时，若 *overflow 非 NULL，则把它设为零。
 *
 * 如果日期有限但超出 timestamptz 的有效范围，那么：
 * 若 overflow 为 NULL，则抛出越界错误。
 * 若 overflow 非 NULL，则在其中存入 +1 或 -1 以标示
 * 溢出的符号，并返回相应的 timestamptz 无穷值。
 */
TimestampTz
date2timestamptz_opt_overflow(DateADT dateVal, int *overflow)
{
	TimestampTz result;
	struct pg_tm tt,
			   *tm = &tt;
	int			tz;

	if (overflow)
		*overflow = 0;

	if (DATE_IS_NOBEGIN(dateVal))
		TIMESTAMP_NOBEGIN(result);
	else if (DATE_IS_NOEND(dateVal))
		TIMESTAMP_NOEND(result);
	else
	{
		/*
		 * 由于日期与 timestamp 具有相同的最小值，只需检查
		 * 上边界是否溢出。
		 */
		if (dateVal >= (TIMESTAMP_END_JULIAN - POSTGRES_EPOCH_JDATE))
		{
			if (overflow)
			{
				*overflow = 1;
				TIMESTAMP_NOEND(result);
				return result;
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("date out of range for timestamp")));
			}
		}

		j2date(dateVal + POSTGRES_EPOCH_JDATE,
			   &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
		tm->tm_hour = 0;
		tm->tm_min = 0;
		tm->tm_sec = 0;
		tz = DetermineTimeZoneOffset(tm, session_timezone);

		result = dateVal * USECS_PER_DAY + tz * USECS_PER_SEC;

		/*
		 * 由于时区可能超出允许的 timestamptz 范围，所以在
		 * 加上时区之后检查允许的 timestamp 范围。
		 */
		if (!IS_VALID_TIMESTAMP(result))
		{
			if (overflow)
			{
				if (result < MIN_TIMESTAMP)
				{
					*overflow = -1;
					TIMESTAMP_NOBEGIN(result);
				}
				else
				{
					*overflow = 1;
					TIMESTAMP_NOEND(result);
				}
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("date out of range for timestamp")));
			}
		}
	}

	return result;
}

/*
 * 把日期提升为 timestamptz，溢出则报错。
 */
static TimestampTz
date2timestamptz(DateADT dateVal)
{
	return date2timestamptz_opt_overflow(dateVal, NULL);
}

/*
 * date2timestamp_no_overflow
 *
 * 本函数的任务是产出 double 值，只要日期处于
 * Timestamp 的有效范围内，就在数值上等于对应的
 * Timestamp 值；但无论如何都不抛出溢出错误。
 * 可以这样做，因为 double 的数值范围大于
 * 无错 timestamp 的范围。这些结果目前只用于
 * 统计估算目的。
 */
double
date2timestamp_no_overflow(DateADT dateVal)
{
	double		result;

	if (DATE_IS_NOBEGIN(dateVal))
		result = -DBL_MAX;
	else if (DATE_IS_NOEND(dateVal))
		result = DBL_MAX;
	else
	{
		/* date 为 2000 起天数，timestamp 为同期微秒... */
		result = dateVal * (double) USECS_PER_DAY;
	}

	return result;
}


/*
 * 日期的跨类型比较函数
 */

int32
date_cmp_timestamp_internal(DateADT dateVal, Timestamp dt2)
{
	Timestamp	dt1;
	int			overflow;

	dt1 = date2timestamp_opt_overflow(dateVal, &overflow);
	if (overflow > 0)
	{
		/* dt1 大于任何有限 timestamp，但小于无穷 */
		return TIMESTAMP_IS_NOEND(dt2) ? -1 : +1;
	}
	Assert(overflow == 0);		/* -1 不会出现 */

	return timestamp_cmp_internal(dt1, dt2);
}

Datum
date_eq_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt2) == 0);
}

Datum
date_ne_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt2) != 0);
}

Datum
date_lt_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt2) < 0);
}

Datum
date_gt_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt2) > 0);
}

Datum
date_le_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt2) <= 0);
}

Datum
date_ge_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt2) >= 0);
}

Datum
date_cmp_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_INT32(date_cmp_timestamp_internal(dateVal, dt2));
}

int32
date_cmp_timestamptz_internal(DateADT dateVal, TimestampTz dt2)
{
	TimestampTz dt1;
	int			overflow;

	dt1 = date2timestamptz_opt_overflow(dateVal, &overflow);
	if (overflow > 0)
	{
		/* dt1 大于任何有限 timestamp，但小于无穷 */
		return TIMESTAMP_IS_NOEND(dt2) ? -1 : +1;
	}
	if (overflow < 0)
	{
		/* dt1 小于任何有限 timestamp，但大于负无穷 */
		return TIMESTAMP_IS_NOBEGIN(dt2) ? +1 : -1;
	}

	return timestamptz_cmp_internal(dt1, dt2);
}

Datum
date_eq_timestamptz(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt2) == 0);
}

Datum
date_ne_timestamptz(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt2) != 0);
}

Datum
date_lt_timestamptz(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt2) < 0);
}

Datum
date_gt_timestamptz(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt2) > 0);
}

Datum
date_le_timestamptz(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt2) <= 0);
}

Datum
date_ge_timestamptz(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt2) >= 0);
}

Datum
date_cmp_timestamptz(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_INT32(date_cmp_timestamptz_internal(dateVal, dt2));
}

Datum
timestamp_eq_date(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt1) == 0);
}

Datum
timestamp_ne_date(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt1) != 0);
}

Datum
timestamp_lt_date(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt1) > 0);
}

Datum
timestamp_gt_date(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt1) < 0);
}

Datum
timestamp_le_date(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt1) >= 0);
}

Datum
timestamp_ge_date(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt1) <= 0);
}

Datum
timestamp_cmp_date(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_INT32(-date_cmp_timestamp_internal(dateVal, dt1));
}

Datum
timestamptz_eq_date(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt1) == 0);
}

Datum
timestamptz_ne_date(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt1) != 0);
}

Datum
timestamptz_lt_date(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt1) > 0);
}

Datum
timestamptz_gt_date(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt1) < 0);
}

Datum
timestamptz_le_date(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt1) >= 0);
}

Datum
timestamptz_ge_date(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt1) <= 0);
}

Datum
timestamptz_cmp_date(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	DateADT		dateVal = PG_GETARG_DATEADT(1);

	PG_RETURN_INT32(-date_cmp_timestamptz_internal(dateVal, dt1));
}

/*
 * date 的 in_range 支持函数
 *
 * 我们的实现方式是把日期提升为 timestamp（不带时
 * 区），再用 timestamp+interval in_range 函数。
 */
Datum
in_range_date_interval(PG_FUNCTION_ARGS)
{
	DateADT		val = PG_GETARG_DATEADT(0);
	DateADT		base = PG_GETARG_DATEADT(1);
	Interval   *offset = PG_GETARG_INTERVAL_P(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	Timestamp	valStamp;
	Timestamp	baseStamp;

	/* XXX 或许可以在这里支持越界情形 */
	valStamp = date2timestamp(val);
	baseStamp = date2timestamp(base);

	return DirectFunctionCall5(in_range_timestamp_interval,
							   TimestampGetDatum(valStamp),
							   TimestampGetDatum(baseStamp),
							   IntervalPGetDatum(offset),
							   BoolGetDatum(sub),
							   BoolGetDatum(less));
}


/* extract_date()
 * 自 date 提取指定字段。
 */
Datum
extract_date(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	DateADT		date = PG_GETARG_DATEADT(1);
	int64		intresult;
	int			type,
				val;
	char	   *lowunits;
	int			year,
				mon,
				mday;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (DATE_NOT_FINITE(date) && (type == UNITS || type == RESERV))
	{
		switch (val)
		{
				/* 振荡类单位 */
			case DTK_DAY:
			case DTK_MONTH:
			case DTK_QUARTER:
			case DTK_WEEK:
			case DTK_DOW:
			case DTK_ISODOW:
			case DTK_DOY:
				PG_RETURN_NULL();
				break;

				/* 单调递增类单位 */
			case DTK_YEAR:
			case DTK_DECADE:
			case DTK_CENTURY:
			case DTK_MILLENNIUM:
			case DTK_JULIAN:
			case DTK_ISOYEAR:
			case DTK_EPOCH:
				if (DATE_IS_NOBEGIN(date))
					PG_RETURN_NUMERIC(DatumGetNumeric(DirectFunctionCall3(numeric_in,
																		  CStringGetDatum("-Infinity"),
																		  ObjectIdGetDatum(InvalidOid),
																		  Int32GetDatum(-1))));
				else
					PG_RETURN_NUMERIC(DatumGetNumeric(DirectFunctionCall3(numeric_in,
																		  CStringGetDatum("Infinity"),
																		  ObjectIdGetDatum(InvalidOid),
																		  Int32GetDatum(-1))));
			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(DATEOID))));
		}
	}
	else if (type == UNITS)
	{
		j2date(date + POSTGRES_EPOCH_JDATE, &year, &mon, &mday);

		switch (val)
		{
			case DTK_DAY:
				intresult = mday;
				break;

			case DTK_MONTH:
				intresult = mon;
				break;

			case DTK_QUARTER:
				intresult = (mon - 1) / 3 + 1;
				break;

			case DTK_WEEK:
				intresult = date2isoweek(year, mon, mday);
				break;

			case DTK_YEAR:
				if (year > 0)
					intresult = year;
				else
					/* 无 0 年，仅 BC 1 与 AD 1 */
					intresult = year - 1;
				break;

			case DTK_DECADE:
				/* 见 timestamp_part 注释 */
				if (year >= 0)
					intresult = year / 10;
				else
					intresult = -((8 - (year - 1)) / 10);
				break;

			case DTK_CENTURY:
				/* 见 timestamp_part 注释 */
				if (year > 0)
					intresult = (year + 99) / 100;
				else
					intresult = -((99 - (year - 1)) / 100);
				break;

			case DTK_MILLENNIUM:
				/* 见 timestamp_part 注释 */
				if (year > 0)
					intresult = (year + 999) / 1000;
				else
					intresult = -((999 - (year - 1)) / 1000);
				break;

			case DTK_JULIAN:
				intresult = date + POSTGRES_EPOCH_JDATE;
				break;

			case DTK_ISOYEAR:
				intresult = date2isoyear(year, mon, mday);
				/* 调整 BC 年 */
				if (intresult <= 0)
					intresult -= 1;
				break;

			case DTK_DOW:
			case DTK_ISODOW:
				intresult = j2day(date + POSTGRES_EPOCH_JDATE);
				if (val == DTK_ISODOW && intresult == 0)
					intresult = 7;
				break;

			case DTK_DOY:
				intresult = date2j(year, mon, mday) - date2j(year, 1, 1) + 1;
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(DATEOID))));
				intresult = 0;
		}
	}
	else if (type == RESERV)
	{
		switch (val)
		{
			case DTK_EPOCH:
				intresult = ((int64) date + POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY;
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(DATEOID))));
				intresult = 0;
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(DATEOID))));
		intresult = 0;
	}

	PG_RETURN_NUMERIC(int64_to_numeric(intresult));
}


/* interval 与日期相加得新日期。
 * 必须同时处理正负 interval。
 *
 * 我们的实现方式是把日期提升为 timestamp（不带时
 * 区），用 timestamp+interval 函数
 */
Datum
date_pl_interval(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	Timestamp	dateStamp;

	dateStamp = date2timestamp(dateVal);

	return DirectFunctionCall2(timestamp_pl_interval,
							   TimestampGetDatum(dateStamp),
							   PointerGetDatum(span));
}

/* 从日期减去 interval，得新日期。
 * 必须同时处理正负 interval。
 *
 * 我们的实现方式是把日期提升为 timestamp（不带时
 * 区），用 timestamp-interval 函数
 */
Datum
date_mi_interval(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	Timestamp	dateStamp;

	dateStamp = date2timestamp(dateVal);

	return DirectFunctionCall2(timestamp_mi_interval,
							   TimestampGetDatum(dateStamp),
							   PointerGetDatum(span));
}

/* date_timestamp()
 * 把 date 转为 timestamp
 */
Datum
date_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Timestamp	result;

	result = date2timestamp(dateVal);

	PG_RETURN_TIMESTAMP(result);
}

/* timestamp_date()
 * 把 timestamp 转为 date
 */
Datum
timestamp_date(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	DateADT		result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;

	if (TIMESTAMP_IS_NOBEGIN(timestamp))
		DATE_NOBEGIN(result);
	else if (TIMESTAMP_IS_NOEND(timestamp))
		DATE_NOEND(result);
	else
	{
		if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		result = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;
	}

	PG_RETURN_DATEADT(result);
}


/* date_timestamptz()
 * 把 date 转为带时区的 timestamp
 */
Datum
date_timestamptz(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	TimestampTz result;

	result = date2timestamptz(dateVal);

	PG_RETURN_TIMESTAMP(result);
}


/* timestamptz_date()
 * 把带时区的 timestamp 转为 date
 */
Datum
timestamptz_date(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMP(0);
	DateADT		result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	if (TIMESTAMP_IS_NOBEGIN(timestamp))
		DATE_NOBEGIN(result);
	else if (TIMESTAMP_IS_NOEND(timestamp))
		DATE_NOEND(result);
	else
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		result = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;
	}

	PG_RETURN_DATEADT(result);
}


/*****************************************************************************
 *	 Time ADT
 *****************************************************************************/

Datum
time_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	TimeADT		result;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;
	int			tz;
	int			nf;
	int			dterr;
	char		workbuf[MAXDATELEN + 1];
	char	   *field[MAXDATEFIELDS];
	int			dtype;
	int			ftype[MAXDATEFIELDS];
	DateTimeErrorExtra extra;

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeTimeOnly(field, ftype, nf,
							   &dtype, tm, &fsec, &tz, &extra);
	if (dterr != 0)
	{
		DateTimeParseError(dterr, &extra, str, "time", escontext);
		PG_RETURN_NULL();
	}

	tm2time(tm, fsec, &result);
	AdjustTimeForTypmod(&result, typmod);

	PG_RETURN_TIMEADT(result);
}

/* tm2time()
 * 把 tm 结构转为 time 类型
 */
int
tm2time(struct pg_tm *tm, fsec_t fsec, TimeADT *result)
{
	*result = ((((tm->tm_hour * MINS_PER_HOUR + tm->tm_min) * SECS_PER_MINUTE) + tm->tm_sec)
			   * USECS_PER_SEC) + fsec;
	return 0;
}

/* time_overflows()
 * 检查拆解后的时刻是否越界。
 */
bool
time_overflows(int hour, int min, int sec, fsec_t fsec)
{
	/* 逐个对各字段做范围检查 */
	if (hour < 0 || hour > HOURS_PER_DAY ||
		min < 0 || min >= MINS_PER_HOUR ||
		sec < 0 || sec > SECS_PER_MINUTE ||
		fsec < 0 || fsec > USECS_PER_SEC)
		return true;

	/*
	 * 因为我们允许诸如 hour = 24 或 sec = 60 之类，必须
	 * 单独检查总时刻值不超过 24:00:00。
	 */
	if ((((((hour * MINS_PER_HOUR + min) * SECS_PER_MINUTE)
		   + sec) * USECS_PER_SEC) + fsec) > USECS_PER_DAY)
		return true;

	return false;
}

/* float_time_overflows()
 * 同上，但此时秒与小数秒合成一个 "double" 值。
 */
bool
float_time_overflows(int hour, int min, double sec)
{
	/* 逐个对各字段做范围检查 */
	if (hour < 0 || hour > HOURS_PER_DAY ||
		min < 0 || min >= MINS_PER_HOUR)
		return true;

	/*
	 * "sec" 是 double，需要额外当心。要处理 NaN，并在
	 * 应用范围检查前先行取整，以免输入不精确
	 * 出现意外错误。（假定 rint() 对无穷行为正常。）
	 */
	if (isnan(sec))
		return true;
	sec = rint(sec * USECS_PER_SEC);
	if (sec < 0 || sec > SECS_PER_MINUTE * USECS_PER_SEC)
		return true;

	/*
	 * 因为我们允许诸如 hour = 24 或 sec = 60 之类，必须
	 * 单独检查总时刻值不超过 24:00:00。这必须与调用
	 * 者把各字段转换为 time 的方式一致。
	 */
	if (((((hour * MINS_PER_HOUR + min) * SECS_PER_MINUTE)
		  * USECS_PER_SEC) + (int64) sec) > USECS_PER_DAY)
		return true;

	return false;
}


/* time2tm()
 * 把 time 类型转为 POSIX 结构
 *
 * 注意只有 hour/min/sec/小数秒字段会被填充。
 */
int
time2tm(TimeADT time, struct pg_tm *tm, fsec_t *fsec)
{
	tm->tm_hour = time / USECS_PER_HOUR;
	time -= tm->tm_hour * USECS_PER_HOUR;
	tm->tm_min = time / USECS_PER_MINUTE;
	time -= tm->tm_min * USECS_PER_MINUTE;
	tm->tm_sec = time / USECS_PER_SEC;
	time -= tm->tm_sec * USECS_PER_SEC;
	*fsec = time;
	return 0;
}

Datum
time_out(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	char	   *result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	char		buf[MAXDATELEN + 1];

	time2tm(time, tm, &fsec);
	EncodeTimeOnly(tm, fsec, false, 0, DateStyle, buf);

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		time_recv - 将外部二进制格式转为 time
 */
Datum
time_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	TimeADT		result;

	result = pq_getmsgint64(buf);

	if (result < INT64CONST(0) || result > USECS_PER_DAY)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("time out of range")));

	AdjustTimeForTypmod(&result, typmod);

	PG_RETURN_TIMEADT(result);
}

/*
 *		time_send - 把 time 转为二进制
 */
Datum
time_send(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, time);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
timetypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anytime_typmodin(false, ta));
}

Datum
timetypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anytime_typmodout(false, typmod));
}

/*
 *		make_time - time 构造器
 */
Datum
make_time(PG_FUNCTION_ARGS)
{
	int			tm_hour = PG_GETARG_INT32(0);
	int			tm_min = PG_GETARG_INT32(1);
	double		sec = PG_GETARG_FLOAT8(2);
	TimeADT		time;

	/* 检查 time 溢出 */
	if (float_time_overflows(tm_hour, tm_min, sec))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("time field value out of range: %d:%02d:%02g",
						tm_hour, tm_min, sec)));

	/* 这应与 tm2time 一致 */
	time = (((tm_hour * MINS_PER_HOUR + tm_min) * SECS_PER_MINUTE)
			* USECS_PER_SEC) + (int64) rint(sec * USECS_PER_SEC);

	PG_RETURN_TIMEADT(time);
}


/* time_support()
 *
 * 供 time_scale() 与 timetz_scale() 长度强制函数
 * 使用的规划器支持函数（无需区分二者）。
 */
Datum
time_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;

		ret = TemporalSimplify(MAX_TIME_PRECISION, (Node *) req->fcall);
	}

	PG_RETURN_POINTER(ret);
}

/* time_scale()
 * 按指定刻度调整 time 类型。
 * 由 PostgreSQL 类型系统用于填列
 */
Datum
time_scale(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	int32		typmod = PG_GETARG_INT32(1);
	TimeADT		result;

	result = time;
	AdjustTimeForTypmod(&result, typmod);

	PG_RETURN_TIMEADT(result);
}

/* AdjustTimeForTypmod()
 * 把 time 值的精度强制为指定值。
 * 使用与 AdjustTimestampForTypmod() 中*完全*相同的
 * 代码，但单独复制一份，因为这些类型
 * 并没有根本性的关联，而只是实现的巧合。
 * implementation. - thomas
 */
void
AdjustTimeForTypmod(TimeADT *time, int32 typmod)
{
	static const int64 TimeScales[MAX_TIME_PRECISION + 1] = {
		INT64CONST(1000000),
		INT64CONST(100000),
		INT64CONST(10000),
		INT64CONST(1000),
		INT64CONST(100),
		INT64CONST(10),
		INT64CONST(1)
	};

	static const int64 TimeOffsets[MAX_TIME_PRECISION + 1] = {
		INT64CONST(500000),
		INT64CONST(50000),
		INT64CONST(5000),
		INT64CONST(500),
		INT64CONST(50),
		INT64CONST(5),
		INT64CONST(0)
	};

	if (typmod >= 0 && typmod <= MAX_TIME_PRECISION)
	{
		if (*time >= INT64CONST(0))
			*time = ((*time + TimeOffsets[typmod]) / TimeScales[typmod]) *
				TimeScales[typmod];
		else
			*time = -((((-*time) + TimeOffsets[typmod]) / TimeScales[typmod]) *
					  TimeScales[typmod]);
	}
}


Datum
time_eq(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 == time2);
}

Datum
time_ne(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 != time2);
}

Datum
time_lt(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 < time2);
}

Datum
time_le(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 <= time2);
}

Datum
time_gt(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 > time2);
}

Datum
time_ge(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 >= time2);
}

Datum
time_cmp(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	if (time1 < time2)
		PG_RETURN_INT32(-1);
	if (time1 > time2)
		PG_RETURN_INT32(1);
	PG_RETURN_INT32(0);
}

Datum
time_hash(PG_FUNCTION_ARGS)
{
	return hashint8(fcinfo);
}

Datum
time_hash_extended(PG_FUNCTION_ARGS)
{
	return hashint8extended(fcinfo);
}

Datum
time_larger(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_TIMEADT((time1 > time2) ? time1 : time2);
}

Datum
time_smaller(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_TIMEADT((time1 < time2) ? time1 : time2);
}

/* overlaps_time() --- 实现 SQL OVERLAPS 算子。
 *
 * 算法按 SQL 规范。这比想象的难，因为规范要求
 * 我们在某些输入为 null 的情形下也必须给出非 null
 * 答案。
 */
Datum
overlaps_time(PG_FUNCTION_ARGS)
{
	/*
	 * 参数是 TimeADT，但我们把它们留作通用 Datum，
	 * 以免解引用 null（按引用传递！）
	 */
	Datum		ts1 = PG_GETARG_DATUM(0);
	Datum		te1 = PG_GETARG_DATUM(1);
	Datum		ts2 = PG_GETARG_DATUM(2);
	Datum		te2 = PG_GETARG_DATUM(3);
	bool		ts1IsNull = PG_ARGISNULL(0);
	bool		te1IsNull = PG_ARGISNULL(1);
	bool		ts2IsNull = PG_ARGISNULL(2);
	bool		te2IsNull = PG_ARGISNULL(3);

#define TIMEADT_GT(t1,t2) \
	(DatumGetTimeADT(t1) > DatumGetTimeADT(t2))
#define TIMEADT_LT(t1,t2) \
	(DatumGetTimeADT(t1) < DatumGetTimeADT(t2))

	/*
	 * 若区间 1 两端点均为 null，则结果为 null（未知）。
	 * 如果只有一个端点为 null，则取非 null 的那个；
	 * 否则取较小端点为 ts1。
	 */
	if (ts1IsNull)
	{
		if (te1IsNull)
			PG_RETURN_NULL();
		/* 换入非 null */
		ts1 = te1;
		te1IsNull = true;
	}
	else if (!te1IsNull)
	{
		if (TIMEADT_GT(ts1, te1))
		{
			Datum		tt = ts1;

			ts1 = te1;
			te1 = tt;
		}
	}

	/* 区间 2 同理 */
	if (ts2IsNull)
	{
		if (te2IsNull)
			PG_RETURN_NULL();
		/* 换入非 null */
		ts2 = te2;
		te2IsNull = true;
	}
	else if (!te2IsNull)
	{
		if (TIMEADT_GT(ts2, te2))
		{
			Datum		tt = ts2;

			ts2 = te2;
			te2 = tt;
		}
	}

	/*
	 * 此时 ts1 与 ts2 均不为 null，可以分三种情况考虑：
	 * ts1 > ts2、ts1 < ts2、ts1 = ts2
	 */
	if (TIMEADT_GT(ts1, ts2))
	{
		/*
		 * 本情形是 ts1 < te2 或 te1 < te2，看起来可能有
		 * 冗余，但在有 null 存在时并不完全如此。
		 */
		if (te2IsNull)
			PG_RETURN_NULL();
		if (TIMEADT_LT(ts1, te2))
			PG_RETURN_BOOL(true);
		if (te1IsNull)
			PG_RETURN_NULL();

		/*
		 * 若 te1 非 null，则前面已有 ts1 <= te1，且刚发现
		 * ts1 >= te2 故 te1 >= te2。
		 */
		PG_RETURN_BOOL(false);
	}
	else if (TIMEADT_LT(ts1, ts2))
	{
		/* 情形是 ts2 < te1 或 te2 < te1 */
		if (te1IsNull)
			PG_RETURN_NULL();
		if (TIMEADT_LT(ts2, te1))
			PG_RETURN_BOOL(true);
		if (te2IsNull)
			PG_RETURN_NULL();

		/*
		 * 若 te2 非 null，则前面已有 ts2 <= te2，且刚发现
		 * ts2 >= te1 故 te2 >= te1。
		 */
		PG_RETURN_BOOL(false);
	}
	else
	{
		/*
		 * 对 ts1 = ts2 情形，规范说 te1 <> te2 或 te1 = te2，
		 * 这说法很傻，意为"两均非 null 则真，否则 null"
		 */
		if (te1IsNull || te2IsNull)
			PG_RETURN_NULL();
		PG_RETURN_BOOL(true);
	}

#undef TIMEADT_GT
#undef TIMEADT_LT
}

/* timestamp_time()
 * 把 timestamp 转为 time
 */
Datum
timestamp_time(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	TimeADT		result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_NULL();

	if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	/*
	 * 也可以这样算：time = (timestamp / USECS_PER_DAY *
	 * USECS_PER_DAY) - timestamp;
	 */
	result = ((((tm->tm_hour * MINS_PER_HOUR + tm->tm_min) * SECS_PER_MINUTE) + tm->tm_sec) *
			  USECS_PER_SEC) + fsec;

	PG_RETURN_TIMEADT(result);
}

/* timestamptz_time()
 * 把 timestamptz 转为 time
 */
Datum
timestamptz_time(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMP(0);
	TimeADT		result;
	struct pg_tm tt,
			   *tm = &tt;
	int			tz;
	fsec_t		fsec;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_NULL();

	if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	/*
	 * 也可以这样算：time = (timestamp / USECS_PER_DAY *
	 * USECS_PER_DAY) - timestamp;
	 */
	result = ((((tm->tm_hour * MINS_PER_HOUR + tm->tm_min) * SECS_PER_MINUTE) + tm->tm_sec) *
			  USECS_PER_SEC) + fsec;

	PG_RETURN_TIMEADT(result);
}

/* datetime_timestamp()
 * 把 date 与 time 转为 timestamp
 */
Datum
datetime_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		date = PG_GETARG_DATEADT(0);
	TimeADT		time = PG_GETARG_TIMEADT(1);
	Timestamp	result;

	result = date2timestamp(date);
	if (!TIMESTAMP_NOT_FINITE(result))
	{
		result += time;
		if (!IS_VALID_TIMESTAMP(result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}

	PG_RETURN_TIMESTAMP(result);
}

/* time_interval()
 * 把 time 转为 interval
 */
Datum
time_interval(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->time = time;
	result->day = 0;
	result->month = 0;

	PG_RETURN_INTERVAL_P(result);
}

/* interval_time()
 * 把 interval 转为 time
 *
 * 本函数定义为产生 interval 的小数日部分。因此，
 * 我们可以直接忽略月字段。对于负 interval 该怎么
 * 处理并不十分清楚，但我们选择减去 floor，这样，
 * 例如 '-2 hours' 变为 '22:00:00'。
 */
Datum
interval_time(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	TimeADT		result;
	int64		days;

	result = span->time;
	if (result >= USECS_PER_DAY)
	{
		days = result / USECS_PER_DAY;
		result -= days * USECS_PER_DAY;
	}
	else if (result < 0)
	{
		days = (-result + USECS_PER_DAY - 1) / USECS_PER_DAY;
		result += days * USECS_PER_DAY;
	}

	PG_RETURN_TIMEADT(result);
}

/* time_mi_time()
 * 两个 time 相减得 interval。
 */
Datum
time_mi_time(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->month = 0;
	result->day = 0;
	result->time = time1 - time2;

	PG_RETURN_INTERVAL_P(result);
}

/* time_pl_interval()
 * 加 interval 到 time
 */
Datum
time_pl_interval(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	TimeADT		result;

	result = time + span->time;
	result -= result / USECS_PER_DAY * USECS_PER_DAY;
	if (result < INT64CONST(0))
		result += USECS_PER_DAY;

	PG_RETURN_TIMEADT(result);
}

/* time_mi_interval()
 * time 减 interval。
 */
Datum
time_mi_interval(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	TimeADT		result;

	result = time - span->time;
	result -= result / USECS_PER_DAY * USECS_PER_DAY;
	if (result < INT64CONST(0))
		result += USECS_PER_DAY;

	PG_RETURN_TIMEADT(result);
}

/*
 * time 的 in_range 支持函数。
 */
Datum
in_range_time_interval(PG_FUNCTION_ARGS)
{
	TimeADT		val = PG_GETARG_TIMEADT(0);
	TimeADT		base = PG_GETARG_TIMEADT(1);
	Interval   *offset = PG_GETARG_INTERVAL_P(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	TimeADT		sum;

	/*
	 * 与 time_pl_interval/time_mi_interval 一样，忽略
	 * 偏移的月与日字段，负值判断也应如此。
	 */
	if (offset->time < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	/*
	 * 我们不能在这里用 time_pl_interval/time_mi_interval，
	 * 因其回绕行为会给出错误（或至少不合意）答案。
	 * 所幸等价的非回绕行为很简单，尤其因为我们不
	 * 担心整数溢出。
	 */
	if (sub)
		sum = base - offset->time;
	else
		sum = base + offset->time;

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}


/* time_part() and extract_time()
 * 自 time 提取指定字段。
 */
static Datum
time_part_common(PG_FUNCTION_ARGS, bool retnumeric)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimeADT		time = PG_GETARG_TIMEADT(1);
	int64		intresult;
	int			type,
				val;
	char	   *lowunits;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (type == UNITS)
	{
		fsec_t		fsec;
		struct pg_tm tt,
				   *tm = &tt;

		time2tm(time, tm, &fsec);

		switch (val)
		{
			case DTK_MICROSEC:
				intresult = tm->tm_sec * INT64CONST(1000000) + fsec;
				break;

			case DTK_MILLISEC:
				if (retnumeric)
					/*---
					 * tm->tm_sec * 1000 + fsec / 1000
					 * = (tm->tm_sec * 1'000'000 + fsec) / 1000
					 */
					PG_RETURN_NUMERIC(int64_div_fast_to_numeric(tm->tm_sec * INT64CONST(1000000) + fsec, 3));
				else
					PG_RETURN_FLOAT8(tm->tm_sec * 1000.0 + fsec / 1000.0);
				break;

			case DTK_SECOND:
				if (retnumeric)
					/*---
					 * tm->tm_sec + fsec / 1'000'000
					 * = (tm->tm_sec * 1'000'000 + fsec) / 1'000'000
					 */
					PG_RETURN_NUMERIC(int64_div_fast_to_numeric(tm->tm_sec * INT64CONST(1000000) + fsec, 6));
				else
					PG_RETURN_FLOAT8(tm->tm_sec + fsec / 1000000.0);
				break;

			case DTK_MINUTE:
				intresult = tm->tm_min;
				break;

			case DTK_HOUR:
				intresult = tm->tm_hour;
				break;

			case DTK_TZ:
			case DTK_TZ_MINUTE:
			case DTK_TZ_HOUR:
			case DTK_DAY:
			case DTK_MONTH:
			case DTK_QUARTER:
			case DTK_YEAR:
			case DTK_DECADE:
			case DTK_CENTURY:
			case DTK_MILLENNIUM:
			case DTK_ISOYEAR:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMEOID))));
				intresult = 0;
		}
	}
	else if (type == RESERV && val == DTK_EPOCH)
	{
		if (retnumeric)
			PG_RETURN_NUMERIC(int64_div_fast_to_numeric(time, 6));
		else
			PG_RETURN_FLOAT8(time / 1000000.0);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(TIMEOID))));
		intresult = 0;
	}

	if (retnumeric)
		PG_RETURN_NUMERIC(int64_to_numeric(intresult));
	else
		PG_RETURN_FLOAT8(intresult);
}

Datum
time_part(PG_FUNCTION_ARGS)
{
	return time_part_common(fcinfo, false);
}

Datum
extract_time(PG_FUNCTION_ARGS)
{
	return time_part_common(fcinfo, true);
}


/*****************************************************************************
 *	 Time With Time Zone ADT
 *****************************************************************************/

/* tm2timetz()
 * 把 tm 结构转为 time 类型
 */
int
tm2timetz(struct pg_tm *tm, fsec_t fsec, int tz, TimeTzADT *result)
{
	result->time = ((((tm->tm_hour * MINS_PER_HOUR + tm->tm_min) * SECS_PER_MINUTE) + tm->tm_sec) *
					USECS_PER_SEC) + fsec;
	result->zone = tz;

	return 0;
}

Datum
timetz_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	TimeTzADT  *result;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;
	int			tz;
	int			nf;
	int			dterr;
	char		workbuf[MAXDATELEN + 1];
	char	   *field[MAXDATEFIELDS];
	int			dtype;
	int			ftype[MAXDATEFIELDS];
	DateTimeErrorExtra extra;

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeTimeOnly(field, ftype, nf,
							   &dtype, tm, &fsec, &tz, &extra);
	if (dterr != 0)
	{
		DateTimeParseError(dterr, &extra, str, "time with time zone",
						   escontext);
		PG_RETURN_NULL();
	}

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));
	tm2timetz(tm, fsec, tz, result);
	AdjustTimeForTypmod(&(result->time), typmod);

	PG_RETURN_TIMETZADT_P(result);
}

Datum
timetz_out(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(0);
	char	   *result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;
	char		buf[MAXDATELEN + 1];

	timetz2tm(time, tm, &fsec, &tz);
	EncodeTimeOnly(tm, fsec, true, tz, DateStyle, buf);

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		timetz_recv - 将外部二进制格式转为 timetz
 */
Datum
timetz_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	TimeTzADT  *result;

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	result->time = pq_getmsgint64(buf);

	if (result->time < INT64CONST(0) || result->time > USECS_PER_DAY)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("time out of range")));

	result->zone = pq_getmsgint(buf, sizeof(result->zone));

	/* 检查 GMT 偏移是否合理；见 datatype/timestamp.h */
	if (result->zone <= -TZDISP_LIMIT || result->zone >= TZDISP_LIMIT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TIME_ZONE_DISPLACEMENT_VALUE),
				 errmsg("time zone displacement out of range")));

	AdjustTimeForTypmod(&(result->time), typmod);

	PG_RETURN_TIMETZADT_P(result);
}

/*
 *		timetz_send - 把 timetz 转为二进制
 */
Datum
timetz_send(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, time->time);
	pq_sendint32(&buf, time->zone);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
timetztypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anytime_typmodin(true, ta));
}

Datum
timetztypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anytime_typmodout(true, typmod));
}


/* timetz2tm()
 * 把 TIME WITH TIME ZONE 转为 POSIX 结构。
 */
int
timetz2tm(TimeTzADT *time, struct pg_tm *tm, fsec_t *fsec, int *tzp)
{
	TimeOffset	trem = time->time;

	tm->tm_hour = trem / USECS_PER_HOUR;
	trem -= tm->tm_hour * USECS_PER_HOUR;
	tm->tm_min = trem / USECS_PER_MINUTE;
	trem -= tm->tm_min * USECS_PER_MINUTE;
	tm->tm_sec = trem / USECS_PER_SEC;
	*fsec = trem - tm->tm_sec * USECS_PER_SEC;

	if (tzp != NULL)
		*tzp = time->zone;

	return 0;
}

/* timetz_scale()
 * 按指定刻度调整 time 类型。
 * 由 PostgreSQL 类型系统用于填列
 */
Datum
timetz_scale(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	TimeTzADT  *result;

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	result->time = time->time;
	result->zone = time->zone;

	AdjustTimeForTypmod(&(result->time), typmod);

	PG_RETURN_TIMETZADT_P(result);
}


static int
timetz_cmp_internal(TimeTzADT *time1, TimeTzADT *time2)
{
	TimeOffset	t1,
				t2;

	/* 首要排序按真实（等效 GMT）时间 */
	t1 = time1->time + (time1->zone * USECS_PER_SEC);
	t2 = time2->time + (time2->zone * USECS_PER_SEC);

	if (t1 > t2)
		return 1;
	if (t1 < t2)
		return -1;

	/*
	 * 如果 GMT 时间相同，则按时区排序；我们只想在
	 * time 与 zone 部分都相等时，两个 timetz 才相等。
	 */
	if (time1->zone > time2->zone)
		return 1;
	if (time1->zone < time2->zone)
		return -1;

	return 0;
}

Datum
timetz_eq(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) == 0);
}

Datum
timetz_ne(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) != 0);
}

Datum
timetz_lt(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) < 0);
}

Datum
timetz_le(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) <= 0);
}

Datum
timetz_gt(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) > 0);
}

Datum
timetz_ge(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) >= 0);
}

Datum
timetz_cmp(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_INT32(timetz_cmp_internal(time1, time2));
}

Datum
timetz_hash(PG_FUNCTION_ARGS)
{
	TimeTzADT  *key = PG_GETARG_TIMETZADT_P(0);
	uint32		thash;

	/*
	 * 为避免结构体中填充字节带来的任何问题，我们分
	 * 别算字段哈希再异或。
	 */
	thash = DatumGetUInt32(DirectFunctionCall1(hashint8,
											   Int64GetDatumFast(key->time)));
	thash ^= DatumGetUInt32(hash_uint32(key->zone));
	PG_RETURN_UINT32(thash);
}

Datum
timetz_hash_extended(PG_FUNCTION_ARGS)
{
	TimeTzADT  *key = PG_GETARG_TIMETZADT_P(0);
	Datum		seed = PG_GETARG_DATUM(1);
	uint64		thash;

	/* 与 timetz_hash 同法 */
	thash = DatumGetUInt64(DirectFunctionCall2(hashint8extended,
											   Int64GetDatumFast(key->time),
											   seed));
	thash ^= DatumGetUInt64(hash_uint32_extended(key->zone,
												 DatumGetInt64(seed)));
	PG_RETURN_UINT64(thash);
}

Datum
timetz_larger(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);
	TimeTzADT  *result;

	if (timetz_cmp_internal(time1, time2) > 0)
		result = time1;
	else
		result = time2;
	PG_RETURN_TIMETZADT_P(result);
}

Datum
timetz_smaller(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);
	TimeTzADT  *result;

	if (timetz_cmp_internal(time1, time2) < 0)
		result = time1;
	else
		result = time2;
	PG_RETURN_TIMETZADT_P(result);
}

/* timetz_pl_interval()
 * 加 interval 到 timetz
 */
Datum
timetz_pl_interval(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	TimeTzADT  *result;

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	result->time = time->time + span->time;
	result->time -= result->time / USECS_PER_DAY * USECS_PER_DAY;
	if (result->time < INT64CONST(0))
		result->time += USECS_PER_DAY;

	result->zone = time->zone;

	PG_RETURN_TIMETZADT_P(result);
}

/* timetz_mi_interval()
 * timetz 减 interval。
 */
Datum
timetz_mi_interval(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	TimeTzADT  *result;

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	result->time = time->time - span->time;
	result->time -= result->time / USECS_PER_DAY * USECS_PER_DAY;
	if (result->time < INT64CONST(0))
		result->time += USECS_PER_DAY;

	result->zone = time->zone;

	PG_RETURN_TIMETZADT_P(result);
}

/*
 * timetz 的 in_range 支持函数。
 */
Datum
in_range_timetz_interval(PG_FUNCTION_ARGS)
{
	TimeTzADT  *val = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *base = PG_GETARG_TIMETZADT_P(1);
	Interval   *offset = PG_GETARG_INTERVAL_P(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	TimeTzADT	sum;

	/*
	 * 与 timetz_pl_interval/timetz_mi_interval 一样，忽略
	 * 偏移的月与日字段，负值判断也应如此。
	 */
	if (offset->time < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	/*
	 * 这里不能用 timetz_pl_interval/timetz_mi_interval，
	 * 因其回绕行为会给出错误（或至少不合意）答案。
	 * 所幸等价的非回绕行为很简单，尤其因为我们不
	 * 担心整数溢出。
	 */
	if (sub)
		sum.time = base->time - offset->time;
	else
		sum.time = base->time + offset->time;
	sum.zone = base->zone;

	if (less)
		PG_RETURN_BOOL(timetz_cmp_internal(val, &sum) <= 0);
	else
		PG_RETURN_BOOL(timetz_cmp_internal(val, &sum) >= 0);
}

/* overlaps_timetz() --- 实现 SQL OVERLAPS 算子。
 *
 * 算法按 SQL 规范。这比想象的难，因为规范要求
 * 我们在某些输入为 null 的情形下也必须给出非 null
 * 答案。
 */
Datum
overlaps_timetz(PG_FUNCTION_ARGS)
{
	/*
	 * 参数是 TimeTzADT *，但我们把它们留作通用 Datum，
	 * 以便书写方便 --- 并避免解引用 null。
	 */
	Datum		ts1 = PG_GETARG_DATUM(0);
	Datum		te1 = PG_GETARG_DATUM(1);
	Datum		ts2 = PG_GETARG_DATUM(2);
	Datum		te2 = PG_GETARG_DATUM(3);
	bool		ts1IsNull = PG_ARGISNULL(0);
	bool		te1IsNull = PG_ARGISNULL(1);
	bool		ts2IsNull = PG_ARGISNULL(2);
	bool		te2IsNull = PG_ARGISNULL(3);

#define TIMETZ_GT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timetz_gt,t1,t2))
#define TIMETZ_LT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timetz_lt,t1,t2))

	/*
	 * 若区间 1 两端点均为 null，则结果为 null（未知）。
	 * 如果只有一个端点为 null，则取非 null 的那个；
	 * 否则取较小端点为 ts1。
	 */
	if (ts1IsNull)
	{
		if (te1IsNull)
			PG_RETURN_NULL();
		/* 换入非 null */
		ts1 = te1;
		te1IsNull = true;
	}
	else if (!te1IsNull)
	{
		if (TIMETZ_GT(ts1, te1))
		{
			Datum		tt = ts1;

			ts1 = te1;
			te1 = tt;
		}
	}

	/* 区间 2 同理 */
	if (ts2IsNull)
	{
		if (te2IsNull)
			PG_RETURN_NULL();
		/* 换入非 null */
		ts2 = te2;
		te2IsNull = true;
	}
	else if (!te2IsNull)
	{
		if (TIMETZ_GT(ts2, te2))
		{
			Datum		tt = ts2;

			ts2 = te2;
			te2 = tt;
		}
	}

	/*
	 * 此时 ts1 与 ts2 均不为 null，可以分三种情况考虑：
	 * ts1 > ts2、ts1 < ts2、ts1 = ts2
	 */
	if (TIMETZ_GT(ts1, ts2))
	{
		/*
		 * 本情形是 ts1 < te2 或 te1 < te2，看起来可能有
		 * 冗余，但在有 null 存在时并不完全如此。
		 */
		if (te2IsNull)
			PG_RETURN_NULL();
		if (TIMETZ_LT(ts1, te2))
			PG_RETURN_BOOL(true);
		if (te1IsNull)
			PG_RETURN_NULL();

		/*
		 * 若 te1 非 null，则前面已有 ts1 <= te1，且刚发现
		 * ts1 >= te2 故 te1 >= te2。
		 */
		PG_RETURN_BOOL(false);
	}
	else if (TIMETZ_LT(ts1, ts2))
	{
		/* 情形是 ts2 < te1 或 te2 < te1 */
		if (te1IsNull)
			PG_RETURN_NULL();
		if (TIMETZ_LT(ts2, te1))
			PG_RETURN_BOOL(true);
		if (te2IsNull)
			PG_RETURN_NULL();

		/*
		 * 若 te2 非 null，则前面已有 ts2 <= te2，且刚发现
		 * ts2 >= te1 故 te2 >= te1。
		 */
		PG_RETURN_BOOL(false);
	}
	else
	{
		/*
		 * 对 ts1 = ts2 情形，规范说 te1 <> te2 或 te1 = te2，
		 * 这说法很傻，意为"两均非 null 则真，否则 null"
		 */
		if (te1IsNull || te2IsNull)
			PG_RETURN_NULL();
		PG_RETURN_BOOL(true);
	}

#undef TIMETZ_GT
#undef TIMETZ_LT
}


Datum
timetz_time(PG_FUNCTION_ARGS)
{
	TimeTzADT  *timetz = PG_GETARG_TIMETZADT_P(0);
	TimeADT		result;

	/* 吞掉时区，只返回 time */
	result = timetz->time;

	PG_RETURN_TIMEADT(result);
}


Datum
time_timetz(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	TimeTzADT  *result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	GetCurrentDateTime(tm);
	time2tm(time, tm, &fsec);
	tz = DetermineTimeZoneOffset(tm, session_timezone);

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	result->time = time;
	result->zone = tz;

	PG_RETURN_TIMETZADT_P(result);
}


/* timestamptz_timetz()
 * 把 timestamp 转为 timetz
 */
Datum
timestamptz_timetz(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMP(0);
	TimeTzADT  *result;
	struct pg_tm tt,
			   *tm = &tt;
	int			tz;
	fsec_t		fsec;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_NULL();

	if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	tm2timetz(tm, fsec, tz, result);

	PG_RETURN_TIMETZADT_P(result);
}


/* datetimetz_timestamptz()
 * 把 date 与 timetz 转为带时区 timestamp
 * timestamp 存为 GMT，须把 timetz 带的
 * 时区加到结果上。
 * - thomas 2000-03-10
 */
Datum
datetimetz_timestamptz(PG_FUNCTION_ARGS)
{
	DateADT		date = PG_GETARG_DATEADT(0);
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(1);
	TimestampTz result;

	if (DATE_IS_NOBEGIN(date))
		TIMESTAMP_NOBEGIN(result);
	else if (DATE_IS_NOEND(date))
		TIMESTAMP_NOEND(result);
	else
	{
		/*
		 * date 的范围比 timestamp 宽，所以要检查边界。
		 * 由于日期与 timestamp 具有相同的最小值，只需检查
		 * 上边界是否溢出。
		 */
		if (date >= (TIMESTAMP_END_JULIAN - POSTGRES_EPOCH_JDATE))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("date out of range for timestamp")));
		result = date * USECS_PER_DAY + time->time + time->zone * USECS_PER_SEC;

		/*
		 * 由于时区可能超出允许的 timestamptz 范围，所以在
		 * 加上时区之后检查允许的 timestamp 范围。
		 */
		if (!IS_VALID_TIMESTAMP(result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("date out of range for timestamp")));
	}

	PG_RETURN_TIMESTAMP(result);
}


/* timetz_part() and extract_timetz()
 * 自 time 提取指定字段。
 */
static Datum
timetz_part_common(PG_FUNCTION_ARGS, bool retnumeric)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(1);
	int64		intresult;
	int			type,
				val;
	char	   *lowunits;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (type == UNITS)
	{
		int			tz;
		fsec_t		fsec;
		struct pg_tm tt,
				   *tm = &tt;

		timetz2tm(time, tm, &fsec, &tz);

		switch (val)
		{
			case DTK_TZ:
				intresult = -tz;
				break;

			case DTK_TZ_MINUTE:
				intresult = (-tz / SECS_PER_MINUTE) % MINS_PER_HOUR;
				break;

			case DTK_TZ_HOUR:
				intresult = -tz / SECS_PER_HOUR;
				break;

			case DTK_MICROSEC:
				intresult = tm->tm_sec * INT64CONST(1000000) + fsec;
				break;

			case DTK_MILLISEC:
				if (retnumeric)
					/*---
					 * tm->tm_sec * 1000 + fsec / 1000
					 * = (tm->tm_sec * 1'000'000 + fsec) / 1000
					 */
					PG_RETURN_NUMERIC(int64_div_fast_to_numeric(tm->tm_sec * INT64CONST(1000000) + fsec, 3));
				else
					PG_RETURN_FLOAT8(tm->tm_sec * 1000.0 + fsec / 1000.0);
				break;

			case DTK_SECOND:
				if (retnumeric)
					/*---
					 * tm->tm_sec + fsec / 1'000'000
					 * = (tm->tm_sec * 1'000'000 + fsec) / 1'000'000
					 */
					PG_RETURN_NUMERIC(int64_div_fast_to_numeric(tm->tm_sec * INT64CONST(1000000) + fsec, 6));
				else
					PG_RETURN_FLOAT8(tm->tm_sec + fsec / 1000000.0);
				break;

			case DTK_MINUTE:
				intresult = tm->tm_min;
				break;

			case DTK_HOUR:
				intresult = tm->tm_hour;
				break;

			case DTK_DAY:
			case DTK_MONTH:
			case DTK_QUARTER:
			case DTK_YEAR:
			case DTK_DECADE:
			case DTK_CENTURY:
			case DTK_MILLENNIUM:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMETZOID))));
				intresult = 0;
		}
	}
	else if (type == RESERV && val == DTK_EPOCH)
	{
		if (retnumeric)
			/*---
			 * time->time / 1'000'000 + time->zone
			 * = (time->time + time->zone * 1'000'000) / 1'000'000
			 */
			PG_RETURN_NUMERIC(int64_div_fast_to_numeric(time->time + time->zone * INT64CONST(1000000), 6));
		else
			PG_RETURN_FLOAT8(time->time / 1000000.0 + time->zone);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(TIMETZOID))));
		intresult = 0;
	}

	if (retnumeric)
		PG_RETURN_NUMERIC(int64_to_numeric(intresult));
	else
		PG_RETURN_FLOAT8(intresult);
}


Datum
timetz_part(PG_FUNCTION_ARGS)
{
	return timetz_part_common(fcinfo, false);
}

Datum
extract_timetz(PG_FUNCTION_ARGS)
{
	return timetz_part_common(fcinfo, true);
}

/* timetz_zone()
 * 用指定时区编码带时区 time 类型。
 * 按事务开始时刻应用 DST 规则。
 */
Datum
timetz_zone(PG_FUNCTION_ARGS)
{
	text	   *zone = PG_GETARG_TEXT_PP(0);
	TimeTzADT  *t = PG_GETARG_TIMETZADT_P(1);
	TimeTzADT  *result;
	int			tz;
	char		tzname[TZ_STRLEN_MAX + 1];
	int			type,
				val;
	pg_tz	   *tzp;

	/*
	 * 查找所请求的时区。
	 */
	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	type = DecodeTimezoneName(tzname, &val, &tzp);

	if (type == TZNAME_FIXED_OFFSET)
	{
		/* 固定偏移缩写 */
		tz = -val;
	}
	else if (type == TZNAME_DYNTZ)
	{
		/* 动态偏移缩写，用事务开始时刻来解析 */
		TimestampTz now = GetCurrentTransactionStartTimestamp();
		int			isdst;

		tz = DetermineTimeZoneAbbrevOffsetTS(now, tzname, tzp, &isdst);
	}
	else
	{
		/* 取该时区名当前有效的 GMT 偏移量 */
		TimestampTz now = GetCurrentTransactionStartTimestamp();
		struct pg_tm tm;
		fsec_t		fsec;

		if (timestamp2tm(now, &tz, &tm, &fsec, NULL, tzp) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	result->time = t->time + (t->zone - tz) * USECS_PER_SEC;
	/* C99 取模对负数输入的符号约定不对 */
	while (result->time < INT64CONST(0))
		result->time += USECS_PER_DAY;
	if (result->time >= USECS_PER_DAY)
		result->time %= USECS_PER_DAY;

	result->zone = tz;

	PG_RETURN_TIMETZADT_P(result);
}

/* timetz_izone()
 * 用指定的 interval 作为时区来编码带时区 time 类型。
 */
Datum
timetz_izone(PG_FUNCTION_ARGS)
{
	Interval   *zone = PG_GETARG_INTERVAL_P(0);
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(1);
	TimeTzADT  *result;
	int			tz;

	if (zone->month != 0 || zone->day != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval time zone \"%s\" must not include months or days",
						DatumGetCString(DirectFunctionCall1(interval_out,
															PointerGetDatum(zone))))));

	tz = -(zone->time / USECS_PER_SEC);

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	result->time = time->time + (time->zone - tz) * USECS_PER_SEC;
	/* C99 取模对负数输入的符号约定不对 */
	while (result->time < INT64CONST(0))
		result->time += USECS_PER_DAY;
	if (result->time >= USECS_PER_DAY)
		result->time %= USECS_PER_DAY;

	result->zone = tz;

	PG_RETURN_TIMETZADT_P(result);
}
