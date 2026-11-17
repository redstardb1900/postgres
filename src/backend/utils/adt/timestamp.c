/*-------------------------------------------------------------------------
 *
 * timestamp.c
 *	  内建 SQL 类型 "timestamp" 与 "interval" 的函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/timestamp.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>
#include <limits.h>
#include <sys/time.h>

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "common/int128.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "parser/scansup.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/float.h"
#include "utils/numeric.h"
#include "utils/sortsupport.h"

/*
 * gcc 的 -ffast-math 开关会破坏期望 timeval / SECS_PER_HOUR 类
 * 表达式精确结果的例程（timeval 为 double）。
 */
#ifdef __FAST_MATH__
#error -ffast-math is known to break this code
#endif

#define SAMESIGN(a,b)	(((a) < 0) == ((b) < 0))

/* postmaster 启动设置 */
TimestampTz PgStartTime;

/* 配置重载时设置 */
TimestampTz PgReloadTime;

typedef struct
{
	Timestamp	current;
	Timestamp	finish;
	Interval	step;
	int			step_sign;
} generate_series_timestamp_fctx;

typedef struct
{
	TimestampTz current;
	TimestampTz finish;
	Interval	step;
	int			step_sign;
	pg_tz	   *attimezone;
} generate_series_timestamptz_fctx;


static TimeOffset time2t(const int hour, const int min, const int sec, const fsec_t fsec);
static Timestamp dt2local(Timestamp dt, int timezone);
static bool AdjustIntervalForTypmod(Interval *interval, int32 typmod,
									Node *escontext);
static TimestampTz timestamp2timestamptz(Timestamp timestamp);
static Timestamp timestamptz2timestamp(TimestampTz timestamp);


/* timestamptypmodin/timestamptztypmodin 公共代码 */
static int32
anytimestamp_typmodin(bool istz, ArrayType *ta)
{
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);

	/*
	 * 此处我们并不苛求好的错误消息，因为语法不应
	 * 允许 TIMESTAMP 的修饰符个数出错
	 */
	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));

	return anytimestamp_typmod_check(istz, tl[0]);
}

/* 导出供 parse_expr.c 使用 */
int32
anytimestamp_typmod_check(bool istz, int32 typmod)
{
	if (typmod < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("TIMESTAMP(%d)%s precision must not be negative",
						typmod, (istz ? " WITH TIME ZONE" : ""))));
	if (typmod > MAX_TIMESTAMP_PRECISION)
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("TIMESTAMP(%d)%s precision reduced to maximum allowed, %d",
						typmod, (istz ? " WITH TIME ZONE" : ""),
						MAX_TIMESTAMP_PRECISION)));
		typmod = MAX_TIMESTAMP_PRECISION;
	}

	return typmod;
}

/* timestamptypmodout/timestamptztypmodout 公共代码 */
static char *
anytimestamp_typmodout(bool istz, int32 typmod)
{
	const char *tz = istz ? " with time zone" : " without time zone";

	if (typmod >= 0)
		return psprintf("(%d)%s", (int) typmod, tz);
	else
		return pstrdup(tz);
}


/*****************************************************************************
 *	 用户 I/O 例程               *
 *****************************************************************************/

/* timestamp_in()
 * 把字符串转为内部表示。
 */
Datum
timestamp_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	Timestamp	result;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;
	int			tz;
	int			dtype;
	int			nf;
	int			dterr;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		workbuf[MAXDATELEN + MAXDATEFIELDS];
	DateTimeErrorExtra extra;

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeDateTime(field, ftype, nf,
							   &dtype, tm, &fsec, &tz, &extra);
	if (dterr != 0)
	{
		DateTimeParseError(dterr, &extra, str, "timestamp", escontext);
		PG_RETURN_NULL();
	}

	switch (dtype)
	{
		case DTK_DATE:
			if (tm2timestamp(tm, fsec, NULL, &result) != 0)
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range: \"%s\"", str)));
			break;

		case DTK_EPOCH:
			result = SetEpochTimestamp();
			break;

		case DTK_LATE:
			TIMESTAMP_NOEND(result);
			break;

		case DTK_EARLY:
			TIMESTAMP_NOBEGIN(result);
			break;

		default:
			elog(ERROR, "unexpected dtype %d while parsing timestamp \"%s\"",
				 dtype, str);
			TIMESTAMP_NOEND(result);
	}

	AdjustTimestampForTypmod(&result, typmod, escontext);

	PG_RETURN_TIMESTAMP(result);
}

/* timestamp_out()
 * 把 timestamp 转为外部表示。
 */
Datum
timestamp_out(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	char	   *result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	char		buf[MAXDATELEN + 1];

	if (TIMESTAMP_NOT_FINITE(timestamp))
		EncodeSpecialTimestamp(timestamp, buf);
	else if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) == 0)
		EncodeDateTime(tm, fsec, false, 0, NULL, DateStyle, buf);
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		timestamp_recv			- 将外部二进制格式转换为 timestamp
 */
Datum
timestamp_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Timestamp	timestamp;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;

	timestamp = (Timestamp) pq_getmsgint64(buf);

	/* 范围检查：timestamp_out 是否会接受 */
	if (TIMESTAMP_NOT_FINITE(timestamp))
		 /* ok */ ;
	else if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0 ||
			 !IS_VALID_TIMESTAMP(timestamp))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	AdjustTimestampForTypmod(&timestamp, typmod, NULL);

	PG_RETURN_TIMESTAMP(timestamp);
}

/*
 *		timestamp_send			- 把 timestamp 转为二进制格式
 */
Datum
timestamp_send(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, timestamp);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
timestamptypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anytimestamp_typmodin(false, ta));
}

Datum
timestamptypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anytimestamp_typmodout(false, typmod));
}


/*
 * timestamp_support()
 *
 * timestamp_scale() 与 timestamptz_scale() 长度强制
 * 函数的计划器支持函数（无须区分二者）。
 */
Datum
timestamp_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;

		ret = TemporalSimplify(MAX_TIMESTAMP_PRECISION, (Node *) req->fcall);
	}

	PG_RETURN_POINTER(ret);
}

/* timestamp_scale()
 * 按指定刻度调整 time 类型。
 * 由 PostgreSQL 类型系统用于填列
 */
Datum
timestamp_scale(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	int32		typmod = PG_GETARG_INT32(1);
	Timestamp	result;

	result = timestamp;

	AdjustTimestampForTypmod(&result, typmod, NULL);

	PG_RETURN_TIMESTAMP(result);
}

/*
 * AdjustTimestampForTypmod --- 把 timestamp 舍入至给定 typmod
 * 对 timestamp 或 timestamptz 皆适用。
 *
 * 成功时返回 true，失败时返回 false（若 escontext 指向
 * ErrorSaveContext，否则会抛出错误）。
 */
bool
AdjustTimestampForTypmod(Timestamp *time, int32 typmod, Node *escontext)
{
	static const int64 TimestampScales[MAX_TIMESTAMP_PRECISION + 1] = {
		INT64CONST(1000000),
		INT64CONST(100000),
		INT64CONST(10000),
		INT64CONST(1000),
		INT64CONST(100),
		INT64CONST(10),
		INT64CONST(1)
	};

	static const int64 TimestampOffsets[MAX_TIMESTAMP_PRECISION + 1] = {
		INT64CONST(500000),
		INT64CONST(50000),
		INT64CONST(5000),
		INT64CONST(500),
		INT64CONST(50),
		INT64CONST(5),
		INT64CONST(0)
	};

	if (!TIMESTAMP_NOT_FINITE(*time)
		&& (typmod != -1) && (typmod != MAX_TIMESTAMP_PRECISION))
	{
		if (typmod < 0 || typmod > MAX_TIMESTAMP_PRECISION)
			ereturn(escontext, false,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("timestamp(%d) precision must be between %d and %d",
							typmod, 0, MAX_TIMESTAMP_PRECISION)));

		if (*time >= INT64CONST(0))
		{
			*time = ((*time + TimestampOffsets[typmod]) / TimestampScales[typmod]) *
				TimestampScales[typmod];
		}
		else
		{
			*time = -((((-*time) + TimestampOffsets[typmod]) / TimestampScales[typmod])
					  * TimestampScales[typmod]);
		}
	}

	return true;
}

/* timestamptz_in()
 * 把字符串转为内部表示。
 */
Datum
timestamptz_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	TimestampTz result;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;
	int			tz;
	int			dtype;
	int			nf;
	int			dterr;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		workbuf[MAXDATELEN + MAXDATEFIELDS];
	DateTimeErrorExtra extra;

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeDateTime(field, ftype, nf,
							   &dtype, tm, &fsec, &tz, &extra);
	if (dterr != 0)
	{
		DateTimeParseError(dterr, &extra, str, "timestamp with time zone",
						   escontext);
		PG_RETURN_NULL();
	}

	switch (dtype)
	{
		case DTK_DATE:
			if (tm2timestamp(tm, fsec, &tz, &result) != 0)
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range: \"%s\"", str)));
			break;

		case DTK_EPOCH:
			result = SetEpochTimestamp();
			break;

		case DTK_LATE:
			TIMESTAMP_NOEND(result);
			break;

		case DTK_EARLY:
			TIMESTAMP_NOBEGIN(result);
			break;

		default:
			elog(ERROR, "unexpected dtype %d while parsing timestamptz \"%s\"",
				 dtype, str);
			TIMESTAMP_NOEND(result);
	}

	AdjustTimestampForTypmod(&result, typmod, escontext);

	PG_RETURN_TIMESTAMPTZ(result);
}

/*
 * 尝试解析时区说明，若其可接受则返回其时区偏移值。
 * 否则会抛出错误。
 *
 * 注意：部分代码路径会更新 tm->tm_isdst，部分不会；
 * 调用方并不在意，故无需保持一致。
 */
static int
parse_sane_timezone(struct pg_tm *tm, text *zone)
{
	char		tzname[TZ_STRLEN_MAX + 1];
	int			dterr;
	int			tz;

	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	/*
	 * 查找所请求的时区。首先尝试将其解释为
	 * 数字时区说明；若 DecodeTimezone 不接受该格式，
	 * 则改试时区缩写与时区名。
	 *
	 * 注意 pg_tzset 能解析 DecodeTimezone 拒绝的数字输入。
	 * 为避免它接受本会被视为无效的输入，
	 * 只需确保输入串的第一个字符
	 * 不是数字即可。
	 */
	if (isdigit((unsigned char) *tzname))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"numeric time zone", tzname),
				 errhint("Numeric time zones must have \"-\" or \"+\" as first character.")));

	dterr = DecodeTimezone(tzname, &tz);
	if (dterr != 0)
	{
		int			type,
					val;
		pg_tz	   *tzp;

		if (dterr == DTERR_TZDISP_OVERFLOW)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("numeric time zone \"%s\" out of range", tzname)));
		else if (dterr != DTERR_BAD_FORMAT)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone \"%s\" not recognized", tzname)));

		type = DecodeTimezoneName(tzname, &val, &tzp);

		if (type == TZNAME_FIXED_OFFSET)
		{
			/* 固定偏移量缩写 */
			tz = -val;
		}
		else if (type == TZNAME_DYNTZ)
		{
			/* 动态偏移量缩写，用指定时间解析 */
			tz = DetermineTimeZoneAbbrevOffset(tm, tzname, tzp);
		}
		else
		{
			/* full zone name */
			tz = DetermineTimeZoneOffset(tm, tzp);
		}
	}

	return tz;
}

/*
 * 查找所请求的时区，返回 pg_tz 结构体。
 *
 * 与 DecodeTimezoneNameToTz 相同，但以 text Datum 为起点。
 */
static pg_tz *
lookup_timezone(text *zone)
{
	char		tzname[TZ_STRLEN_MAX + 1];

	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	return DecodeTimezoneNameToTz(tzname);
}

/*
 * make_timestamp_internal
 *		make_timestamp/make_timestamptz 的主力函数
 */
static Timestamp
make_timestamp_internal(int year, int month, int day,
						int hour, int min, double sec)
{
	struct pg_tm tm;
	TimeOffset	date;
	TimeOffset	time;
	int			dterr;
	bool		bc = false;
	Timestamp	result;

	tm.tm_year = year;
	tm.tm_mon = month;
	tm.tm_mday = day;

	/* 按公元前处理负年份 */
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
						year, month, day)));

	if (!IS_VALID_JULIAN(tm.tm_year, tm.tm_mon, tm.tm_mday))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range: %d-%02d-%02d",
						year, month, day)));

	date = date2j(tm.tm_year, tm.tm_mon, tm.tm_mday) - POSTGRES_EPOCH_JDATE;

	/* 检查时间溢出 */
	if (float_time_overflows(hour, min, sec))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("time field value out of range: %d:%02d:%02g",
						hour, min, sec)));

	/* 应与 tm2time 一致 */
	time = (((hour * MINS_PER_HOUR + min) * SECS_PER_MINUTE)
			* USECS_PER_SEC) + (int64) rint(sec * USECS_PER_SEC);

	result = date * USECS_PER_DAY + time;
	/* 检查重大溢出 */
	if ((result - time) / USECS_PER_DAY != date)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range: %d-%02d-%02d %d:%02d:%02g",
						year, month, day,
						hour, min, sec)));

	/* 检查刚好越界的溢出（时刻回绕除外均可） */
	/* 注意：我们要允许 1999-12-31 24:00:00 */
	if ((result < 0 && date > 0) ||
		(result > 0 && date < -1))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range: %d-%02d-%02d %d:%02d:%02g",
						year, month, day,
						hour, min, sec)));

	/* 最终范围检查会截住刚好越界的时间戳 */
	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range: %d-%02d-%02d %d:%02d:%02g",
						year, month, day,
						hour, min, sec)));

	return result;
}

/*
 * make_timestamp() - timestamp 构造器
 */
Datum
make_timestamp(PG_FUNCTION_ARGS)
{
	int32		year = PG_GETARG_INT32(0);
	int32		month = PG_GETARG_INT32(1);
	int32		mday = PG_GETARG_INT32(2);
	int32		hour = PG_GETARG_INT32(3);
	int32		min = PG_GETARG_INT32(4);
	float8		sec = PG_GETARG_FLOAT8(5);
	Timestamp	result;

	result = make_timestamp_internal(year, month, mday,
									 hour, min, sec);

	PG_RETURN_TIMESTAMP(result);
}

/*
 * make_timestamptz() - 带时区 timestamp 构造器
 */
Datum
make_timestamptz(PG_FUNCTION_ARGS)
{
	int32		year = PG_GETARG_INT32(0);
	int32		month = PG_GETARG_INT32(1);
	int32		mday = PG_GETARG_INT32(2);
	int32		hour = PG_GETARG_INT32(3);
	int32		min = PG_GETARG_INT32(4);
	float8		sec = PG_GETARG_FLOAT8(5);
	Timestamp	result;

	result = make_timestamp_internal(year, month, mday,
									 hour, min, sec);

	PG_RETURN_TIMESTAMPTZ(timestamp2timestamptz(result));
}

/*
 * 构造一个带时区的 timestamp。
 *		同上，但时区由第七个参数指定。
 */
Datum
make_timestamptz_at_timezone(PG_FUNCTION_ARGS)
{
	int32		year = PG_GETARG_INT32(0);
	int32		month = PG_GETARG_INT32(1);
	int32		mday = PG_GETARG_INT32(2);
	int32		hour = PG_GETARG_INT32(3);
	int32		min = PG_GETARG_INT32(4);
	float8		sec = PG_GETARG_FLOAT8(5);
	text	   *zone = PG_GETARG_TEXT_PP(6);
	TimestampTz result;
	Timestamp	timestamp;
	struct pg_tm tt;
	int			tz;
	fsec_t		fsec;

	timestamp = make_timestamp_internal(year, month, mday,
										hour, min, sec);

	if (timestamp2tm(timestamp, NULL, &tt, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	tz = parse_sane_timezone(&tt, zone);

	result = dt2local(timestamp, -tz);

	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMPTZ(result);
}

/*
 * to_timestamp(double precision)
 * UNIX epoch 转为 timestamptz。
 */
Datum
float8_timestamptz(PG_FUNCTION_ARGS)
{
	float8		seconds = PG_GETARG_FLOAT8(0);
	TimestampTz result;

	/* 处理 NaN 与无穷输入 ... */
	if (isnan(seconds))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp cannot be NaN")));

	if (isinf(seconds))
	{
		if (seconds < 0)
			TIMESTAMP_NOBEGIN(result);
		else
			TIMESTAMP_NOEND(result);
	}
	else
	{
		/* 越界？ */
		if (seconds <
			(float8) SECS_PER_DAY * (DATETIME_MIN_JULIAN - UNIX_EPOCH_JDATE)
			|| seconds >=
			(float8) SECS_PER_DAY * (TIMESTAMP_END_JULIAN - UNIX_EPOCH_JDATE))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range: \"%g\"", seconds)));

		/* UNIX epoch 转为 Postgres 纪元 */
		seconds -= ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

		seconds = rint(seconds * USECS_PER_SEC);
		result = (int64) seconds;

		/* 重新检查，以防舍入产生刚好越界的值 */
		if (!IS_VALID_TIMESTAMP(result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range: \"%g\"",
							PG_GETARG_FLOAT8(0))));
	}

	PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_out()
 * 把 timestamp 转为外部表示。
 */
Datum
timestamptz_out(PG_FUNCTION_ARGS)
{
	TimestampTz dt = PG_GETARG_TIMESTAMPTZ(0);
	char	   *result;
	int			tz;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	const char *tzn;
	char		buf[MAXDATELEN + 1];

	if (TIMESTAMP_NOT_FINITE(dt))
		EncodeSpecialTimestamp(dt, buf);
	else if (timestamp2tm(dt, &tz, tm, &fsec, &tzn, NULL) == 0)
		EncodeDateTime(tm, fsec, true, tz, tzn, DateStyle, buf);
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		timestamptz_recv			- 将外部二进制格式转换为 timestamptz
 */
Datum
timestamptz_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	TimestampTz timestamp;
	int			tz;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;

	timestamp = (TimestampTz) pq_getmsgint64(buf);

	/* 范围检查：timestamptz_out 是否会接受 */
	if (TIMESTAMP_NOT_FINITE(timestamp))
		 /* ok */ ;
	else if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0 ||
			 !IS_VALID_TIMESTAMP(timestamp))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	AdjustTimestampForTypmod(&timestamp, typmod, NULL);

	PG_RETURN_TIMESTAMPTZ(timestamp);
}

/*
 *		timestamptz_send			- 把 timestamptz 转为二进制格式
 */
Datum
timestamptz_send(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, timestamp);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
timestamptztypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anytimestamp_typmodin(true, ta));
}

Datum
timestamptztypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anytimestamp_typmodout(true, typmod));
}


/* timestamptz_scale()
 * 按指定刻度调整 time 类型。
 * 由 PostgreSQL 类型系统用于填列
 */
Datum
timestamptz_scale(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	int32		typmod = PG_GETARG_INT32(1);
	TimestampTz result;

	result = timestamp;

	AdjustTimestampForTypmod(&result, typmod, NULL);

	PG_RETURN_TIMESTAMPTZ(result);
}


/* interval_in()
 * 把字符串转为内部表示。
 *
 * 外部格式：
 *	使用通用的日期/时间解析与解码例程。
 */
Datum
interval_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	Interval   *result;
	struct pg_itm_in tt,
			   *itm_in = &tt;
	int			dtype;
	int			nf;
	int			range;
	int			dterr;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		workbuf[256];
	DateTimeErrorExtra extra;

	itm_in->tm_year = 0;
	itm_in->tm_mon = 0;
	itm_in->tm_mday = 0;
	itm_in->tm_usec = 0;

	if (typmod >= 0)
		range = INTERVAL_RANGE(typmod);
	else
		range = INTERVAL_FULL_RANGE;

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf), field,
						  ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeInterval(field, ftype, nf, range,
							   &dtype, itm_in);

	/* 若上述函数认为格式不佳，则改试 ISO8601 风格 */
	if (dterr == DTERR_BAD_FORMAT)
		dterr = DecodeISO8601Interval(str,
									  &dtype, itm_in);

	if (dterr != 0)
	{
		if (dterr == DTERR_FIELD_OVERFLOW)
			dterr = DTERR_INTERVAL_OVERFLOW;
		DateTimeParseError(dterr, &extra, str, "interval", escontext);
		PG_RETURN_NULL();
	}

	result = (Interval *) palloc(sizeof(Interval));

	switch (dtype)
	{
		case DTK_DELTA:
			if (itmin2interval(itm_in, result) != 0)
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("interval out of range")));
			break;

		default:
			elog(ERROR, "unexpected dtype %d while parsing interval \"%s\"",
				 dtype, str);
	}

	AdjustIntervalForTypmod(result, typmod, escontext);

	PG_RETURN_INTERVAL_P(result);
}

/* interval_out()
 * 把时间跨度转为外部表示。
 */
Datum
interval_out(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	char	   *result;
	struct pg_itm tt,
			   *itm = &tt;
	char		buf[MAXDATELEN + 1];

	interval2itm(*span, itm);
	EncodeInterval(itm, IntervalStyle, buf);

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		interval_recv			- 将外部二进制格式转换为 interval
 */
Datum
interval_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Interval   *interval;

	interval = (Interval *) palloc(sizeof(Interval));

	interval->time = pq_getmsgint64(buf);
	interval->day = pq_getmsgint(buf, sizeof(interval->day));
	interval->month = pq_getmsgint(buf, sizeof(interval->month));

	AdjustIntervalForTypmod(interval, typmod, NULL);

	PG_RETURN_INTERVAL_P(interval);
}

/*
 *		interval_send			- 把 interval 转为二进制格式
 */
Datum
interval_send(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, interval->time);
	pq_sendint32(&buf, interval->day);
	pq_sendint32(&buf, interval->month);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * interval 的 typmod 在其高 16 位存放"范围"，
 * 低 16 位存放"精度"。二者共同决定类型的分辨率。
 * range 处理大于一秒的分辨率粒度，
 * precision 指定低于一秒的分辨率。这种表示法能够
 * 表达所有 SQL 标准的分辨率，但我们对它们的实现都是
 * 从某个位置向右截断。range 是允许字段的位图，
 * 但对计算而言只有其中时间上最小的字段才起作用。
 * precision 是要保留的亚秒小数位个数。
 * 全部置位（INTERVAL_FULL_PRECISION）时的截断语义
 * 等同于选择 MAX_INTERVAL_PRECISION。
 */
Datum
intervaltypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);
	int32	   *tl;
	int			n;
	int32		typmod;

	tl = ArrayGetIntegerTypmods(ta, &n);

	/*
	 * tl[0] - interval 范围（字段位图）	tl[1] - 精度（可选）
	 *
	 * 注意我们必须校验 tl[0]，尽管它通常由语法保证
	 * 正确 --- 例如 SELECT 'foo'::"interval"(1000)。
	 */
	if (n > 0)
	{
		switch (tl[0])
		{
			case INTERVAL_MASK(YEAR):
			case INTERVAL_MASK(MONTH):
			case INTERVAL_MASK(DAY):
			case INTERVAL_MASK(HOUR):
			case INTERVAL_MASK(MINUTE):
			case INTERVAL_MASK(SECOND):
			case INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH):
			case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR):
			case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
			case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
			case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			case INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			case INTERVAL_FULL_RANGE:
				/* 正常 */
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid INTERVAL type modifier")));
		}
	}

	if (n == 1)
	{
		if (tl[0] != INTERVAL_FULL_RANGE)
			typmod = INTERVAL_TYPMOD(INTERVAL_FULL_PRECISION, tl[0]);
		else
			typmod = -1;
	}
	else if (n == 2)
	{
		if (tl[1] < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("INTERVAL(%d) precision must not be negative",
							tl[1])));
		if (tl[1] > MAX_INTERVAL_PRECISION)
		{
			ereport(WARNING,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("INTERVAL(%d) precision reduced to maximum allowed, %d",
							tl[1], MAX_INTERVAL_PRECISION)));
			typmod = INTERVAL_TYPMOD(MAX_INTERVAL_PRECISION, tl[0]);
		}
		else
			typmod = INTERVAL_TYPMOD(tl[1], tl[0]);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid INTERVAL type modifier")));
		typmod = 0;				/* 让编译器安静 */
	}

	PG_RETURN_INT32(typmod);
}

Datum
intervaltypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);
	char	   *res = (char *) palloc(64);
	int			fields;
	int			precision;
	const char *fieldstr;

	if (typmod < 0)
	{
		*res = '\0';
		PG_RETURN_CSTRING(res);
	}

	fields = INTERVAL_RANGE(typmod);
	precision = INTERVAL_PRECISION(typmod);

	switch (fields)
	{
		case INTERVAL_MASK(YEAR):
			fieldstr = " year";
			break;
		case INTERVAL_MASK(MONTH):
			fieldstr = " month";
			break;
		case INTERVAL_MASK(DAY):
			fieldstr = " day";
			break;
		case INTERVAL_MASK(HOUR):
			fieldstr = " hour";
			break;
		case INTERVAL_MASK(MINUTE):
			fieldstr = " minute";
			break;
		case INTERVAL_MASK(SECOND):
			fieldstr = " second";
			break;
		case INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH):
			fieldstr = " year to month";
			break;
		case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR):
			fieldstr = " day to hour";
			break;
		case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
			fieldstr = " day to minute";
			break;
		case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			fieldstr = " day to second";
			break;
		case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
			fieldstr = " hour to minute";
			break;
		case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			fieldstr = " hour to second";
			break;
		case INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			fieldstr = " minute to second";
			break;
		case INTERVAL_FULL_RANGE:
			fieldstr = "";
			break;
		default:
			elog(ERROR, "invalid INTERVAL typmod: 0x%x", typmod);
			fieldstr = "";
			break;
	}

	if (precision != INTERVAL_FULL_PRECISION)
		snprintf(res, 64, "%s(%d)", fieldstr, precision);
	else
		snprintf(res, 64, "%s", fieldstr);

	PG_RETURN_CSTRING(res);
}

/*
 * 给定一个 interval typmod 值，返回该 typmod 所允许非零的
 * 最低有效字段的编码。例如给定
 * INTERVAL DAY TO HOUR 需识别出"hour"。
 *
 * 结果应按字段重要性排序，这意味着
 * 不能用 dt.h 的 YEAR 等宏，因为出于某个奇怪原因
 * 它们并未如此排序。因此人为约定
 * SECOND = 0, MINUTE = 1, HOUR = 2, DAY = 3, MONTH = 4, YEAR = 5.
 */
static int
intervaltypmodleastfield(int32 typmod)
{
	if (typmod < 0)
		return 0;				/* SECOND */

	switch (INTERVAL_RANGE(typmod))
	{
		case INTERVAL_MASK(YEAR):
			return 5;			/* YEAR */
		case INTERVAL_MASK(MONTH):
			return 4;			/* MONTH */
		case INTERVAL_MASK(DAY):
			return 3;			/* DAY */
		case INTERVAL_MASK(HOUR):
			return 2;			/* HOUR */
		case INTERVAL_MASK(MINUTE):
			return 1;			/* MINUTE */
		case INTERVAL_MASK(SECOND):
			return 0;			/* SECOND */
		case INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH):
			return 4;			/* MONTH */
		case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR):
			return 2;			/* HOUR */
		case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
			return 1;			/* MINUTE */
		case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			return 0;			/* SECOND */
		case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
			return 1;			/* MINUTE */
		case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			return 0;			/* SECOND */
		case INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			return 0;			/* SECOND */
		case INTERVAL_FULL_RANGE:
			return 0;			/* SECOND */
		default:
			elog(ERROR, "invalid INTERVAL typmod: 0x%x", typmod);
			break;
	}
	return 0;					/* 到不了这里，让编译器安静 */
}


/*
 * interval_support()
 *
 * interval_scale() 的计划器支持函数。
 *
 * 熨平对 interval_scale() 的多余调用。interval 的 typmod
 * 之所以复杂，是为了能接受并回吐所有 SQL 标准变体。
 * 就截断而言，它归结为单一、简单的粒度。
 */
Datum
interval_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;
		FuncExpr   *expr = req->fcall;
		Node	   *typmod;

		Assert(list_length(expr->args) >= 2);

		typmod = (Node *) lsecond(expr->args);

		if (IsA(typmod, Const) && !((Const *) typmod)->constisnull)
		{
			Node	   *source = (Node *) linitial(expr->args);
			int32		new_typmod = DatumGetInt32(((Const *) typmod)->constvalue);
			bool		noop;

			if (new_typmod < 0)
				noop = true;
			else
			{
				int32		old_typmod = exprTypmod(source);
				int			old_least_field;
				int			new_least_field;
				int			old_precis;
				int			new_precis;

				old_least_field = intervaltypmodleastfield(old_typmod);
				new_least_field = intervaltypmodleastfield(new_typmod);
				if (old_typmod < 0)
					old_precis = INTERVAL_FULL_PRECISION;
				else
					old_precis = INTERVAL_PRECISION(old_typmod);
				new_precis = INTERVAL_PRECISION(new_typmod);

				/*
				 * 若最低字段保持不变或减小，而
				 * 精度不变或增大，则转换无操作。
				 * 但 precision，也就是亚秒精度，只影响
				 * 包含 SECOND 的范围。
				 */
				noop = (new_least_field <= old_least_field) &&
					(old_least_field > 0 /* SECOND */ ||
					 new_precis >= MAX_INTERVAL_PRECISION ||
					 new_precis >= old_precis);
			}
			if (noop)
				ret = relabel_to_typmod(source, new_typmod);
		}
	}

	PG_RETURN_POINTER(ret);
}

/* interval_scale()
 * 按指定字段调整 interval 类型。
 * 由 PostgreSQL 类型系统用于填列
 */
Datum
interval_scale(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	Interval   *result;

	result = palloc(sizeof(Interval));
	*result = *interval;

	AdjustIntervalForTypmod(result, typmod, NULL);

	PG_RETURN_INTERVAL_P(result);
}

/*
 *	按指定精度调整 interval，同时覆盖 YEAR 到 SECOND
 *	范围与亚秒精度。
 *
 * 成功时返回 true，失败时返回 false（若 escontext 指向
 * ErrorSaveContext，否则会抛出错误）。
 */
static bool
AdjustIntervalForTypmod(Interval *interval, int32 typmod,
						Node *escontext)
{
	static const int64 IntervalScales[MAX_INTERVAL_PRECISION + 1] = {
		INT64CONST(1000000),
		INT64CONST(100000),
		INT64CONST(10000),
		INT64CONST(1000),
		INT64CONST(100),
		INT64CONST(10),
		INT64CONST(1)
	};

	static const int64 IntervalOffsets[MAX_INTERVAL_PRECISION + 1] = {
		INT64CONST(500000),
		INT64CONST(50000),
		INT64CONST(5000),
		INT64CONST(500),
		INT64CONST(50),
		INT64CONST(5),
		INT64CONST(0)
	};

	/*
	 * 范围与精度未指定？那就无须调整。将 typmod 设为 -1
	 * 是所有数据类型的惯例。
	 */
	if (typmod >= 0)
	{
		int			range = INTERVAL_RANGE(typmod);
		int			precision = INTERVAL_PRECISION(typmod);

		/*
		 * 我们对字段集受限的 interval 的解释是：
		 * 最后一个指定字段右侧的字段全部清零，
		 * 而其左侧的字段仍然有效。因此例如
		 * INTERVAL YEAR TO MONTH 与 INTERVAL MONTH 之间在行为上
		 * 并无差别。某些情形下我们本可以有意义地强制
		 * 高阶字段为零；例如 INTERVAL DAY 可以拒绝非零的
		 * "month" 字段。但由于我们无法一致地做到这一点，
		 * 这样做似乎没什么意义。（我们无法对预期的
		 * 最高字段施加范围限制，因为没有等价于 SQL
		 * <interval leading field precision> 的手段。）如果我们
		 * 重审此点，interval_support 可能也需要调整。
		 *
		 * 注意：PG 8.4 前我们把字段集受限解释为
		 * 实际会对给定值执行"取模"操作，可能同时丢失
		 * 高阶与低阶信息。但标准中并不支持这种行为，
		 * 且从数据一致性角度看也相当不可取。
		 * 现在我们只会对低阶字段执行
		 * 截断或舍入。
		 */
		if (range == INTERVAL_FULL_RANGE)
		{
			/* 无操作… */
		}
		else if (range == INTERVAL_MASK(YEAR))
		{
			interval->month = (interval->month / MONTHS_PER_YEAR) * MONTHS_PER_YEAR;
			interval->day = 0;
			interval->time = 0;
		}
		else if (range == INTERVAL_MASK(MONTH))
		{
			interval->day = 0;
			interval->time = 0;
		}
		/* YEAR TO MONTH */
		else if (range == (INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH)))
		{
			interval->day = 0;
			interval->time = 0;
		}
		else if (range == INTERVAL_MASK(DAY))
		{
			interval->time = 0;
		}
		else if (range == INTERVAL_MASK(HOUR))
		{
			interval->time = (interval->time / USECS_PER_HOUR) *
				USECS_PER_HOUR;
		}
		else if (range == INTERVAL_MASK(MINUTE))
		{
			interval->time = (interval->time / USECS_PER_MINUTE) *
				USECS_PER_MINUTE;
		}
		else if (range == INTERVAL_MASK(SECOND))
		{
			/* 小数秒舍入将在下方处理 */
		}
		/* DAY TO HOUR */
		else if (range == (INTERVAL_MASK(DAY) |
						   INTERVAL_MASK(HOUR)))
		{
			interval->time = (interval->time / USECS_PER_HOUR) *
				USECS_PER_HOUR;
		}
		/* DAY TO MINUTE */
		else if (range == (INTERVAL_MASK(DAY) |
						   INTERVAL_MASK(HOUR) |
						   INTERVAL_MASK(MINUTE)))
		{
			interval->time = (interval->time / USECS_PER_MINUTE) *
				USECS_PER_MINUTE;
		}
		/* DAY TO SECOND */
		else if (range == (INTERVAL_MASK(DAY) |
						   INTERVAL_MASK(HOUR) |
						   INTERVAL_MASK(MINUTE) |
						   INTERVAL_MASK(SECOND)))
		{
			/* 小数秒舍入将在下方处理 */
		}
		/* HOUR TO MINUTE */
		else if (range == (INTERVAL_MASK(HOUR) |
						   INTERVAL_MASK(MINUTE)))
		{
			interval->time = (interval->time / USECS_PER_MINUTE) *
				USECS_PER_MINUTE;
		}
		/* HOUR TO SECOND */
		else if (range == (INTERVAL_MASK(HOUR) |
						   INTERVAL_MASK(MINUTE) |
						   INTERVAL_MASK(SECOND)))
		{
			/* 小数秒舍入将在下方处理 */
		}
		/* MINUTE TO SECOND */
		else if (range == (INTERVAL_MASK(MINUTE) |
						   INTERVAL_MASK(SECOND)))
		{
			/* 小数秒舍入将在下方处理 */
		}
		else
			elog(ERROR, "unrecognized interval typmod: %d", typmod);

		/* 需要调整亚秒精度吗？ */
		if (precision != INTERVAL_FULL_PRECISION)
		{
			if (precision < 0 || precision > MAX_INTERVAL_PRECISION)
				ereturn(escontext, false,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("interval(%d) precision must be between %d and %d",
								precision, 0, MAX_INTERVAL_PRECISION)));

			if (interval->time >= INT64CONST(0))
			{
				interval->time = ((interval->time +
								   IntervalOffsets[precision]) /
								  IntervalScales[precision]) *
					IntervalScales[precision];
			}
			else
			{
				interval->time = -(((-interval->time +
									 IntervalOffsets[precision]) /
									IntervalScales[precision]) *
								   IntervalScales[precision]);
			}
		}
	}

	return true;
}

/*
 * make_interval - 数值 Interval 构造器
 */
Datum
make_interval(PG_FUNCTION_ARGS)
{
	int32		years = PG_GETARG_INT32(0);
	int32		months = PG_GETARG_INT32(1);
	int32		weeks = PG_GETARG_INT32(2);
	int32		days = PG_GETARG_INT32(3);
	int32		hours = PG_GETARG_INT32(4);
	int32		mins = PG_GETARG_INT32(5);
	double		secs = PG_GETARG_FLOAT8(6);
	Interval   *result;

	/*
	 * 拒绝越界输入。其实我们也应该检查整数输入，
	 * 但目前并不完全清楚该应用什么限制。
	 */
	if (isinf(secs) || isnan(secs))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	result = (Interval *) palloc(sizeof(Interval));
	result->month = years * MONTHS_PER_YEAR + months;
	result->day = weeks * 7 + days;

	secs = rint(secs * USECS_PER_SEC);
	result->time = hours * ((int64) SECS_PER_HOUR * USECS_PER_SEC) +
		mins * ((int64) SECS_PER_MINUTE * USECS_PER_SEC) +
		(int64) secs;

	PG_RETURN_INTERVAL_P(result);
}

/* EncodeSpecialTimestamp()
 * 把保留 timestamp 类型转为字符串。
 */
void
EncodeSpecialTimestamp(Timestamp dt, char *str)
{
	if (TIMESTAMP_IS_NOBEGIN(dt))
		strcpy(str, EARLY);
	else if (TIMESTAMP_IS_NOEND(dt))
		strcpy(str, LATE);
	else						/* 不应发生 */
		elog(ERROR, "invalid argument for EncodeSpecialTimestamp");
}

Datum
now(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(GetCurrentTransactionStartTimestamp());
}

Datum
statement_timestamp(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(GetCurrentStatementStartTimestamp());
}

Datum
clock_timestamp(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(GetCurrentTimestamp());
}

Datum
pg_postmaster_start_time(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(PgStartTime);
}

Datum
pg_conf_load_time(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(PgReloadTime);
}

/*
 * GetCurrentTimestamp -- 取得当前操作系统时间
 *
 * 结果为 TimestampTz 值，精度达 gettimeofday() 系统调用
 * 的全精度。
 */
TimestampTz
GetCurrentTimestamp(void)
{
	TimestampTz result;
	struct timeval tp;

	gettimeofday(&tp, NULL);

	result = (TimestampTz) tp.tv_sec -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
	result = (result * USECS_PER_SEC) + tp.tv_usec;

	return result;
}

/*
 * GetSQLCurrentTimestamp -- 实现 CURRENT_TIMESTAMP、CURRENT_TIMESTAMP(n)
 */
TimestampTz
GetSQLCurrentTimestamp(int32 typmod)
{
	TimestampTz ts;

	ts = GetCurrentTransactionStartTimestamp();
	if (typmod >= 0)
		AdjustTimestampForTypmod(&ts, typmod, NULL);
	return ts;
}

/*
 * GetSQLLocalTimestamp -- 实现 LOCALTIMESTAMP、LOCALTIMESTAMP(n)
 */
Timestamp
GetSQLLocalTimestamp(int32 typmod)
{
	Timestamp	ts;

	ts = timestamptz2timestamp(GetCurrentTransactionStartTimestamp());
	if (typmod >= 0)
		AdjustTimestampForTypmod(&ts, typmod, NULL);
	return ts;
}

/*
 * timeofday(*) -- 以文本返回当前时间。
 */
Datum
timeofday(PG_FUNCTION_ARGS)
{
	struct timeval tp;
	char		templ[128];
	char		buf[128];
	pg_time_t	tt;

	gettimeofday(&tp, NULL);
	tt = (pg_time_t) tp.tv_sec;
	pg_strftime(templ, sizeof(templ), "%a %b %d %H:%M:%S.%%06d %Y %Z",
				pg_localtime(&tt, session_timezone));
	snprintf(buf, sizeof(buf), templ, tp.tv_usec);

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * TimestampDifference -- 将两个 timestamp 的差转换为整数秒
 *		与微秒
 *
 * 这通常用于为 select(2) 计算等待超时，
 * 这解释了输出格式初看费解的原因。
 *
 * 两者都必须是普通有限 timestamp（在目前用法中，
 * 它们是 GetCurrentTimestamp() 的结果）。
 *
 * 预期 start_time <= stop_time。若非如此，返回零，
 * 因为那时已经过了先前确定的 stop_time。
 */
void
TimestampDifference(TimestampTz start_time, TimestampTz stop_time,
					long *secs, int *microsecs)
{
	TimestampTz diff = stop_time - start_time;

	if (diff <= 0)
	{
		*secs = 0;
		*microsecs = 0;
	}
	else
	{
		*secs = (long) (diff / USECS_PER_SEC);
		*microsecs = (int) (diff % USECS_PER_SEC);
	}
}

/*
 * TimestampDifferenceMilliseconds -- 将两个 timestamp 的差
 * 		转换为整数毫秒
 *
 * 这通常用于为 WaitLatch() 等函数计算等待超时。
 * 结果类型选为 "long" 正是为了与之匹配；此外，
 * 我们将结果钳制到最多 INT_MAX 毫秒，
 * 因为那是 WaitLatch() 所允许的上限。
 *
 * 预期 start_time <= stop_time。否则就返回零，
 * 因为那时已经过了先前确定的 stop_time。
 *
 * 有限与无限 timestamp 相减可正确工作，视情况返回零
 * 或 INT_MAX。
 *
 * 注意会把不足一毫秒的部分向上取整，因为等待
 * 比预期超时略短一点并不可取。
 */
long
TimestampDifferenceMilliseconds(TimestampTz start_time, TimestampTz stop_time)
{
	TimestampTz diff;

	/* 快速处理零或负的流逝时间。 */
	if (start_time >= stop_time)
		return 0;
	/* 为免在 timestamp 无穷值上出错，必须检测溢出。 */
	if (pg_sub_s64_overflow(stop_time, start_time, &diff))
		return (long) INT_MAX;
	if (diff >= (INT_MAX * INT64CONST(1000) - 999))
		return (long) INT_MAX;
	else
		return (long) ((diff + 999) / 1000);
}

/*
 * TimestampDifferenceExceeds -- 报告两个 timestamp 之差
 *		是否 >= 以毫秒表示的阈值
 *
 * 两者都必须是普通有限 timestamp（在目前用法中，
 * 它们是 GetCurrentTimestamp() 的结果）。
 */
bool
TimestampDifferenceExceeds(TimestampTz start_time,
						   TimestampTz stop_time,
						   int msec)
{
	TimestampTz diff = stop_time - start_time;

	return (diff >= msec * INT64CONST(1000));
}

/*
 * time_t 转 TimestampTz。
 *
 * Postgres 内部并不使用 time_t，但提供此函数供需要解释
 * 诸如 stat(2) 结果的函数使用。
 *
 * 为了避免该函数的 ABI 随 time_t 宽度而变化，
 * 我们把参数声明为 pg_time_t，它与 time_t 转型兼容，
 * 但宽度恒为 64 位（除非平台没有 64 位类型）。
 * 该细节应对调用方不可见，至少在源代码层面如此。
 */
TimestampTz
time_t_to_timestamptz(pg_time_t tm)
{
	TimestampTz result;

	result = (TimestampTz) tm -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
	result *= USECS_PER_SEC;

	return result;
}

/*
 * TimestampTz 转 time_t。
 *
 * 它同样只是略有用处，但有些地方需要它。
 *
 * 为了避免该函数的 ABI 随 time_t 宽度而变化，
 * 我们把结果声明为 pg_time_t，与 time_t 转型兼容，
 * 但宽度恒为 64 位（除非平台没有 64 位类型）。
 * 该细节应对调用方不可见，至少在源代码层面如此。
 */
pg_time_t
timestamptz_to_time_t(TimestampTz t)
{
	pg_time_t	result;

	result = (pg_time_t) (t / USECS_PER_SEC +
						  ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY));

	return result;
}

/*
 * 生成 TimestampTz 的 C 字符串表示。
 *
 * 主要用于发送消息。与 timestamptz_out 相比的主要差别
 * 在于我们强制输出格式为 ISO。另注意结果存于静态
 * 缓冲区，而非 pstrdup 出来的。
 *
 * 另见 pg_strftime。
 */
const char *
timestamptz_to_str(TimestampTz t)
{
	static char buf[MAXDATELEN + 1];
	int			tz;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	const char *tzn;

	if (TIMESTAMP_NOT_FINITE(t))
		EncodeSpecialTimestamp(t, buf);
	else if (timestamp2tm(t, &tz, tm, &fsec, &tzn, NULL) == 0)
		EncodeDateTime(tm, fsec, true, tz, tzn, USE_ISO_DATES, buf);
	else
		strlcpy(buf, "(timestamp out of range)", sizeof(buf));

	return buf;
}


void
dt2time(Timestamp jd, int *hour, int *min, int *sec, fsec_t *fsec)
{
	TimeOffset	time;

	time = jd;

	*hour = time / USECS_PER_HOUR;
	time -= (*hour) * USECS_PER_HOUR;
	*min = time / USECS_PER_MINUTE;
	time -= (*min) * USECS_PER_MINUTE;
	*sec = time / USECS_PER_SEC;
	*fsec = time - (*sec * USECS_PER_SEC);
}								/* dt2time() */


/*
 * timestamp2tm() - 把 timestamp 转为 POSIX 时间结构。
 *
 * 注意年份并非以 1900 为基准，而是显式的完整值。
 * 另外，月份从 1 起算而非 0。
 * Returns:
 *	 0 为成功
 *	-1 表示越界
 *
 * 若 attimezone 为 NULL，则使用全局时区设置。
 */
int
timestamp2tm(Timestamp dt, int *tzp, struct pg_tm *tm, fsec_t *fsec, const char **tzn, pg_tz *attimezone)
{
	Timestamp	date;
	Timestamp	time;
	pg_time_t	utime;

	/* 调用方要求默认时用会话时区 */
	if (attimezone == NULL)
		attimezone = session_timezone;

	time = dt;
	TMODULO(time, date, USECS_PER_DAY);

	if (time < INT64CONST(0))
	{
		time += USECS_PER_DAY;
		date -= 1;
	}

	/* 加上偏移量以便从 J2000 回到标准儒略日 */
	date += POSTGRES_EPOCH_JDATE;

	/* 儒略日例程不能处理负儒略日 */
	if (date < 0 || date > (Timestamp) INT_MAX)
		return -1;

	j2date((int) date, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
	dt2time(time, &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);

	/* 若不需时区转换则完成 */
	if (tzp == NULL)
	{
		tm->tm_isdst = -1;
		tm->tm_gmtoff = 0;
		tm->tm_zone = NULL;
		if (tzn != NULL)
			*tzn = NULL;
		return 0;
	}

	/*
	 * 若时间落在 pg_time_t 的范围内，则用 pg_localtime() 转换
	 * 到本地时区。
	 *
	 * 先取整为 timestamp，避免平台向错误方向
	 * 舍入的误差，并调整到 Unix 纪元。
	 * 然后看能否无损转换为 pg_time_t。这种
	 * 编码方式避免硬编码任何关于 pg_time_t 宽度的假设，
	 * 因此在无 int64 的机器上也能表现正常。
	 */
	dt = (dt - *fsec) / USECS_PER_SEC +
		(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY;
	utime = (pg_time_t) dt;
	if ((Timestamp) utime == dt)
	{
		struct pg_tm *tx = pg_localtime(&utime, attimezone);

		tm->tm_year = tx->tm_year + 1900;
		tm->tm_mon = tx->tm_mon + 1;
		tm->tm_mday = tx->tm_mday;
		tm->tm_hour = tx->tm_hour;
		tm->tm_min = tx->tm_min;
		tm->tm_sec = tx->tm_sec;
		tm->tm_isdst = tx->tm_isdst;
		tm->tm_gmtoff = tx->tm_gmtoff;
		tm->tm_zone = tx->tm_zone;
		*tzp = -tm->tm_gmtoff;
		if (tzn != NULL)
			*tzn = tm->tm_zone;
	}
	else
	{
		/*
		 * 超出 pg_time_t 范围时，按 GMT 处理
		 */
		*tzp = 0;
		/* 标记为*无*可用时区 */
		tm->tm_isdst = -1;
		tm->tm_gmtoff = 0;
		tm->tm_zone = NULL;
		if (tzn != NULL)
			*tzn = NULL;
	}

	return 0;
}


/* tm2timestamp()
 * 把 tm 结构转换为 timestamp 数据类型。
 * 注意年份并非以 1900 为基准，而是显式的完整值。
 * 另外，月份从 1 起算而非 0。
 *
 * 失败时返回 -1（值越界）。
 */
int
tm2timestamp(struct pg_tm *tm, fsec_t fsec, int *tzp, Timestamp *result)
{
	TimeOffset	date;
	TimeOffset	time;

	/* 在儒略日例程中防止溢出 */
	if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday))
	{
		*result = 0;			/* 让编译器安静 */
		return -1;
	}

	date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;
	time = time2t(tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);

	*result = date * USECS_PER_DAY + time;
	/* 检查重大溢出 */
	if ((*result - time) / USECS_PER_DAY != date)
	{
		*result = 0;			/* 让编译器安静 */
		return -1;
	}
	/* 检查刚好越界的溢出（时刻回绕除外均可） */
	/* 注意：我们要允许 1999-12-31 24:00:00 */
	if ((*result < 0 && date > 0) ||
		(*result > 0 && date < -1))
	{
		*result = 0;			/* 让编译器安静 */
		return -1;
	}
	if (tzp != NULL)
		*result = dt2local(*result, -(*tzp));

	/* 最终范围检查会截住刚好越界的时间戳 */
	if (!IS_VALID_TIMESTAMP(*result))
	{
		*result = 0;			/* 让编译器安静 */
		return -1;
	}

	return 0;
}


/* interval2itm()
 * 把 Interval 转换为 pg_itm 结构。
 * 注意：不可能溢出；pg_itm 各字段足够宽，
 * 足以容纳所有可能的转换结果。
 */
void
interval2itm(Interval span, struct pg_itm *itm)
{
	TimeOffset	time;
	TimeOffset	tfrac;

	itm->tm_year = span.month / MONTHS_PER_YEAR;
	itm->tm_mon = span.month % MONTHS_PER_YEAR;
	itm->tm_mday = span.day;
	time = span.time;

	tfrac = time / USECS_PER_HOUR;
	time -= tfrac * USECS_PER_HOUR;
	itm->tm_hour = tfrac;
	tfrac = time / USECS_PER_MINUTE;
	time -= tfrac * USECS_PER_MINUTE;
	itm->tm_min = (int) tfrac;
	tfrac = time / USECS_PER_SEC;
	time -= tfrac * USECS_PER_SEC;
	itm->tm_sec = (int) tfrac;
	itm->tm_usec = (int) time;
}

/* itm2interval()
 * 把 pg_itm 结构转换为 Interval。
 * 成功返回 0，溢出 -1。
 */
int
itm2interval(struct pg_itm *itm, Interval *span)
{
	int64		total_months = (int64) itm->tm_year * MONTHS_PER_YEAR + itm->tm_mon;

	if (total_months > INT_MAX || total_months < INT_MIN)
		return -1;
	span->month = (int32) total_months;
	span->day = itm->tm_mday;
	if (pg_mul_s64_overflow(itm->tm_hour, USECS_PER_HOUR,
							&span->time))
		return -1;
	/* tm_min 与 tm_sec 是 32 位，因此中间乘积不会溢出 */
	if (pg_add_s64_overflow(span->time, itm->tm_min * USECS_PER_MINUTE,
							&span->time))
		return -1;
	if (pg_add_s64_overflow(span->time, itm->tm_sec * USECS_PER_SEC,
							&span->time))
		return -1;
	if (pg_add_s64_overflow(span->time, itm->tm_usec,
							&span->time))
		return -1;
	return 0;
}

/* itmin2interval()
 * 把 pg_itm_in 结构转换为 Interval。
 * 成功返回 0，溢出 -1。
 */
int
itmin2interval(struct pg_itm_in *itm_in, Interval *span)
{
	int64		total_months = (int64) itm_in->tm_year * MONTHS_PER_YEAR + itm_in->tm_mon;

	if (total_months > INT_MAX || total_months < INT_MIN)
		return -1;
	span->month = (int32) total_months;
	span->day = itm_in->tm_mday;
	span->time = itm_in->tm_usec;
	return 0;
}

static TimeOffset
time2t(const int hour, const int min, const int sec, const fsec_t fsec)
{
	return (((((hour * MINS_PER_HOUR) + min) * SECS_PER_MINUTE) + sec) * USECS_PER_SEC) + fsec;
}

static Timestamp
dt2local(Timestamp dt, int timezone)
{
	dt -= (timezone * USECS_PER_SEC);
	return dt;
}


/*****************************************************************************
 *	 公共例程                  *
 *****************************************************************************/


Datum
timestamp_finite(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);

	PG_RETURN_BOOL(!TIMESTAMP_NOT_FINITE(timestamp));
}

Datum
interval_finite(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(true);
}


/*----------------------------------------------------------
 *	timestamp 的关系算子。
 *---------------------------------------------------------*/

void
GetEpochTime(struct pg_tm *tm)
{
	struct pg_tm *t0;
	pg_time_t	epoch = 0;

	t0 = pg_gmtime(&epoch);

	if (t0 == NULL)
		elog(ERROR, "could not convert epoch to timestamp: %m");

	tm->tm_year = t0->tm_year;
	tm->tm_mon = t0->tm_mon;
	tm->tm_mday = t0->tm_mday;
	tm->tm_hour = t0->tm_hour;
	tm->tm_min = t0->tm_min;
	tm->tm_sec = t0->tm_sec;

	tm->tm_year += 1900;
	tm->tm_mon++;
}

Timestamp
SetEpochTimestamp(void)
{
	Timestamp	dt;
	struct pg_tm tt,
			   *tm = &tt;

	GetEpochTime(tm);
	/* 我们懒得测试失败... */
	tm2timestamp(tm, 0, NULL, &dt);

	return dt;
}								/* SetEpochTimestamp() */

/*
 * 目前我们在 timestamp 与 timestamptz 之间共享部分代码。
 * 比较函数即在其中。 - thomas 2001-09-25
 *
 *		timestamp_relop - is timestamp1 relop timestamp2
 */
int
timestamp_cmp_internal(Timestamp dt1, Timestamp dt2)
{
	return (dt1 < dt2) ? -1 : ((dt1 > dt2) ? 1 : 0);
}

Datum
timestamp_eq(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) == 0);
}

Datum
timestamp_ne(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) != 0);
}

Datum
timestamp_lt(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) < 0);
}

Datum
timestamp_gt(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) > 0);
}

Datum
timestamp_le(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) <= 0);
}

Datum
timestamp_ge(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) >= 0);
}

Datum
timestamp_cmp(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_INT32(timestamp_cmp_internal(dt1, dt2));
}

#if SIZEOF_DATUM < 8
/* 注意：这也用于 timestamptz */
static int
timestamp_fastcmp(Datum x, Datum y, SortSupport ssup)
{
	Timestamp	a = DatumGetTimestamp(x);
	Timestamp	b = DatumGetTimestamp(y);

	return timestamp_cmp_internal(a, b);
}
#endif

Datum
timestamp_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

#if SIZEOF_DATUM >= 8

	/*
	 * 若本构建的 timestamp 按值传递，那么可以改用标准的
	 * 比较器函数。
	 */
	ssup->comparator = ssup_datum_signed_cmp;
#else
	ssup->comparator = timestamp_fastcmp;
#endif
	PG_RETURN_VOID();
}

Datum
timestamp_hash(PG_FUNCTION_ARGS)
{
	return hashint8(fcinfo);
}

Datum
timestamp_hash_extended(PG_FUNCTION_ARGS)
{
	return hashint8extended(fcinfo);
}

/*
 * timestamp 与 timestamptz 的跨类型比较函数
 */

int32
timestamp_cmp_timestamptz_internal(Timestamp timestampVal, TimestampTz dt2)
{
	TimestampTz dt1;
	int			overflow;

	dt1 = timestamp2timestamptz_opt_overflow(timestampVal, &overflow);
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
timestamp_eq_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) == 0);
}

Datum
timestamp_ne_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) != 0);
}

Datum
timestamp_lt_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) < 0);
}

Datum
timestamp_gt_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) > 0);
}

Datum
timestamp_le_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) <= 0);
}

Datum
timestamp_ge_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) >= 0);
}

Datum
timestamp_cmp_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_INT32(timestamp_cmp_timestamptz_internal(timestampVal, dt2));
}

Datum
timestamptz_eq_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) == 0);
}

Datum
timestamptz_ne_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) != 0);
}

Datum
timestamptz_lt_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) > 0);
}

Datum
timestamptz_gt_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) < 0);
}

Datum
timestamptz_le_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) >= 0);
}

Datum
timestamptz_ge_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) <= 0);
}

Datum
timestamptz_cmp_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_INT32(-timestamp_cmp_timestamptz_internal(timestampVal, dt1));
}


/*
 *		interval_relop	- is interval1 relop interval2
 *
 * interval 比较基于把 interval 值转换为线性表示，
 * 该表示以 time 字段的单位表达（整数 timestamp 情形下
 * 为微秒），并假定天恒为 24 小时、月恒为 30 天。
 * 为避免溢出，我们需要一个比 int64 更宽的数据
 * 类型来表达线性表示，因此使用 INT128。
 */

static inline INT128
interval_cmp_value(const Interval *interval)
{
	INT128		span;
	int64		days;

	/*
	 * 把 month 与 day 字段合成整数天数。
	 * 由于输入是 int32，int64 运算在此足够。
	 */
	days = interval->month * INT64CONST(30);
	days += interval->day;

	/* 加宽 time 至 128 位 */
	span = int64_to_int128(interval->time);

	/* 把天数放大到微秒，形成 128 位乘积 */
	int128_add_int64_mul_int64(&span, days, USECS_PER_DAY);

	return span;
}

static int
interval_cmp_internal(const Interval *interval1, const Interval *interval2)
{
	INT128		span1 = interval_cmp_value(interval1);
	INT128		span2 = interval_cmp_value(interval2);

	return int128_compare(span1, span2);
}

Datum
interval_eq(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) == 0);
}

Datum
interval_ne(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) != 0);
}

Datum
interval_lt(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) < 0);
}

Datum
interval_gt(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) > 0);
}

Datum
interval_le(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) <= 0);
}

Datum
interval_ge(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) >= 0);
}

Datum
interval_cmp(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_INT32(interval_cmp_internal(interval1, interval2));
}

/*
 * interval 的哈希
 *
 * 对于 interval_cmp_internal() 认为相等的值，必须
 * 产生相等的哈希值。所以以同样方式计算净跨
 * 度，再哈希。
 */
Datum
interval_hash(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	INT128		span = interval_cmp_value(interval);
	int64		span64;

	/*
	 * 哈希只使用最低有效 64 位。高 64 位很少能提供
	 * 有用信息，而且为了与引入 INT128 之前所计算的哈希
	 * 保持兼容，我们也必须这样做。
	 *
	 */
	span64 = int128_to_int64(span);

	return DirectFunctionCall1(hashint8, Int64GetDatumFast(span64));
}

Datum
interval_hash_extended(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	INT128		span = interval_cmp_value(interval);
	int64		span64;

	/* 与 interval_hash 同法 */
	span64 = int128_to_int64(span);

	return DirectFunctionCall2(hashint8extended, Int64GetDatumFast(span64),
							   PG_GETARG_DATUM(1));
}

/* overlaps_timestamp() --- 实现 SQL OVERLAPS 算子。
 *
 * 算法依 SQL 标准而定。这比你想的要难得多，
 * 因为标准要求我们在某些输入为 null 的情形下也必须
 * 给出非 null 的答案。
 */
Datum
overlaps_timestamp(PG_FUNCTION_ARGS)
{
	/*
	 * 参数是 timestamp，但我们把它们留作通用 Datum，
	 * 以避免值形式与引用形式之间不必要的转换 --- 更
	 * 不必说还可能对 null 指针解引用。
	 */
	Datum		ts1 = PG_GETARG_DATUM(0);
	Datum		te1 = PG_GETARG_DATUM(1);
	Datum		ts2 = PG_GETARG_DATUM(2);
	Datum		te2 = PG_GETARG_DATUM(3);
	bool		ts1IsNull = PG_ARGISNULL(0);
	bool		te1IsNull = PG_ARGISNULL(1);
	bool		ts2IsNull = PG_ARGISNULL(2);
	bool		te2IsNull = PG_ARGISNULL(3);

#define TIMESTAMP_GT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timestamp_gt,t1,t2))
#define TIMESTAMP_LT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timestamp_lt,t1,t2))

	/*
	 * 若 interval 1 两端点均为 null，则结果为 null（未知）。
	 * 若仅一端为 null，则取非 null 端为 ts1。否则，
	 * 取较小端点为 ts1。
	 */
	if (ts1IsNull)
	{
		if (te1IsNull)
			PG_RETURN_NULL();
		/* 非 null 换 null */
		ts1 = te1;
		te1IsNull = true;
	}
	else if (!te1IsNull)
	{
		if (TIMESTAMP_GT(ts1, te1))
		{
			Datum		tt = ts1;

			ts1 = te1;
			te1 = tt;
		}
	}

	/* interval 2 同理。 */
	if (ts2IsNull)
	{
		if (te2IsNull)
			PG_RETURN_NULL();
		/* 非 null 换 null */
		ts2 = te2;
		te2IsNull = true;
	}
	else if (!te2IsNull)
	{
		if (TIMESTAMP_GT(ts2, te2))
		{
			Datum		tt = ts2;

			ts2 = te2;
			te2 = tt;
		}
	}

	/*
	 * 此时 ts1 与 ts2 均不为 null，情形有三种：
	 * ts1 > ts2、ts1 < ts2、ts1 = ts2
	 */
	if (TIMESTAMP_GT(ts1, ts2))
	{
		/*
		 * 此情形为 ts1 < te2 OR te1 < te2，看似冗余，
		 * 但在有 null 的情况下并不完全如此。
		 */
		if (te2IsNull)
			PG_RETURN_NULL();
		if (TIMESTAMP_LT(ts1, te2))
			PG_RETURN_BOOL(true);
		if (te1IsNull)
			PG_RETURN_NULL();

		/*
		 * 若 te1 非 null，则前文已有 ts1 <= te1，且刚找到
		 * ts1 >= te2，故 te1 >= te2
		 */
		PG_RETURN_BOOL(false);
	}
	else if (TIMESTAMP_LT(ts1, ts2))
	{
		/* 此情形为 ts2 < te1 OR te2 < te1 */
		if (te1IsNull)
			PG_RETURN_NULL();
		if (TIMESTAMP_LT(ts2, te1))
			PG_RETURN_BOOL(true);
		if (te2IsNull)
			PG_RETURN_NULL();

		/*
		 * 若 te2 非 null，则前文已有 ts2 <= te2，且刚找到
		 * ts2 >= te1，故 te2 >= te1
		 */
		PG_RETURN_BOOL(false);
	}
	else
	{
		/*
		 * 对 ts1 = ts2，标准说 te1 <> te2 OR te1 = te2，这是
		 * 绕圈子的说法：两者均非 null 则真，否则为 null。
		 */
		if (te1IsNull || te2IsNull)
			PG_RETURN_NULL();
		PG_RETURN_BOOL(true);
	}

#undef TIMESTAMP_GT
#undef TIMESTAMP_LT
}


/*----------------------------------------------------------
 *	日期/时间的"算术"算子。
 *---------------------------------------------------------*/

Datum
timestamp_smaller(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Timestamp	result;

	/* 使用 timestamp_cmp_internal 以确保与比较行为一致 */
	if (timestamp_cmp_internal(dt1, dt2) < 0)
		result = dt1;
	else
		result = dt2;
	PG_RETURN_TIMESTAMP(result);
}

Datum
timestamp_larger(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Timestamp	result;

	if (timestamp_cmp_internal(dt1, dt2) > 0)
		result = dt1;
	else
		result = dt2;
	PG_RETURN_TIMESTAMP(result);
}


Datum
timestamp_mi(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	if (TIMESTAMP_NOT_FINITE(dt1) || TIMESTAMP_NOT_FINITE(dt2))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("cannot subtract infinite timestamps")));

	if (unlikely(pg_sub_s64_overflow(dt1, dt2, &result->time)))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	result->month = 0;
	result->day = 0;

	/*----------
	 *	这不对，但移除它会破坏大量回归测试。
	 *	例如：
	 *
	 *	test=> SET timezone = 'EST5EDT';
	 *	test=> SELECT
	 *	test-> ('2005-10-30 13:22:00-05'::timestamptz -
	 *	test(>	'2005-10-29 13:22:00-04'::timestamptz);
	 *	?column?
	 *	----------------
	 *	 1 day 01:00:00
	 *	 (1 row)
	 *
	 *	故把它加到首个 timestamp 得到：
	 *
	 *	 test=> SELECT
	 *	 test-> ('2005-10-29 13:22:00-04'::timestamptz +
	 *	 test(> ('2005-10-30 13:22:00-05'::timestamptz -
	 *	 test(>  '2005-10-29 13:22:00-04'::timestamptz)) at time zone 'EST';
	 *		timezone
	 *	--------------------
	 *	2005-10-30 14:22:00
	 *	(1 row)
	 *----------
	 */
	result = DatumGetIntervalP(DirectFunctionCall1(interval_justify_hours,
												   IntervalPGetDatum(result)));

	PG_RETURN_INTERVAL_P(result);
}

/*
 *	interval_justify_interval()
 *
 *	调整 interval，使 'month'、'day' 与 'time' 部分都在
 *	常规界限内。即：
 *
 *		0 <= abs(time) < 24 hours
 *		0 <= abs(day)  < 30 days
 *
 *	另外，三个字段的符号位改为一致，于是要么
 *	全为负，要么全为正。
 */
Datum
interval_justify_interval(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	Interval   *result;
	TimeOffset	wholeday;
	int32		wholemonth;

	result = (Interval *) palloc(sizeof(Interval));
	result->month = span->month;
	result->day = span->day;
	result->time = span->time;

	/* 若可防止溢出则预先 justify day */
	if ((result->day > 0 && result->time > 0) ||
		(result->day < 0 && result->time < 0))
	{
		wholemonth = result->day / DAYS_PER_MONTH;
		result->day -= wholemonth * DAYS_PER_MONTH;
		if (pg_add_s32_overflow(result->month, wholemonth, &result->month))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
	}

	/*
	 * 由于 TimeOffset 是 int64，abs(wholeday) 不会超过约 1.07e8。
	 * 若我们做了预 justify，则 abs(result->day) 小于
	 * DAYS_PER_MONTH，因此此加法不会溢出。若未做预
	 * justify，则 day 与 time 异号，仍不会溢出。
	 */
	TMODULO(result->time, wholeday, USECS_PER_DAY);
	result->day += wholeday;

	wholemonth = result->day / DAYS_PER_MONTH;
	result->day -= wholemonth * DAYS_PER_MONTH;
	if (pg_add_s32_overflow(result->month, wholemonth, &result->month))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	if (result->month > 0 &&
		(result->day < 0 || (result->day == 0 && result->time < 0)))
	{
		result->day += DAYS_PER_MONTH;
		result->month--;
	}
	else if (result->month < 0 &&
			 (result->day > 0 || (result->day == 0 && result->time > 0)))
	{
		result->day -= DAYS_PER_MONTH;
		result->month++;
	}

	if (result->day > 0 && result->time < 0)
	{
		result->time += USECS_PER_DAY;
		result->day--;
	}
	else if (result->day < 0 && result->time > 0)
	{
		result->time -= USECS_PER_DAY;
		result->day++;
	}

	PG_RETURN_INTERVAL_P(result);
}

/*
 *	interval_justify_hours()
 *
 *	调整 interval，使 'time' 含不足一整天，并把超出
 *	部分加到 'day' 上。这对
 *	诸如非 TZ 这类 '1 day' = '24 hours' 成立的情形
 *	有用，如 interval 减法/除法。
 */
Datum
interval_justify_hours(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	Interval   *result;
	TimeOffset	wholeday;

	result = (Interval *) palloc(sizeof(Interval));
	result->month = span->month;
	result->day = span->day;
	result->time = span->time;

	TMODULO(result->time, wholeday, USECS_PER_DAY);
	if (pg_add_s32_overflow(result->day, wholeday, &result->day))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	if (result->day > 0 && result->time < 0)
	{
		result->time += USECS_PER_DAY;
		result->day--;
	}
	else if (result->day < 0 && result->time > 0)
	{
		result->time -= USECS_PER_DAY;
		result->day++;
	}

	PG_RETURN_INTERVAL_P(result);
}

/*
 *	interval_justify_days()
 *
 *	调整 interval，使 'day' 少于 30 天，超出则
 *	加到 'month' 上。
 */
Datum
interval_justify_days(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	Interval   *result;
	int32		wholemonth;

	result = (Interval *) palloc(sizeof(Interval));
	result->month = span->month;
	result->day = span->day;
	result->time = span->time;

	wholemonth = result->day / DAYS_PER_MONTH;
	result->day -= wholemonth * DAYS_PER_MONTH;
	if (pg_add_s32_overflow(result->month, wholemonth, &result->month))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	if (result->month > 0 && result->day < 0)
	{
		result->day += DAYS_PER_MONTH;
		result->month--;
	}
	else if (result->month < 0 && result->day > 0)
	{
		result->day -= DAYS_PER_MONTH;
		result->month++;
	}

	PG_RETURN_INTERVAL_P(result);
}

/* timestamp_pl_interval()
 * 给 timestamp 加上 interval。
 * 注意 interval 带有定性 year/month 与 day 单位的机制，
 *	所以要对它们做恰当处理。
 * 加一个月时，递增月份，并使用月内同一日。
 * 然后若下月天数较少，则把月内之日设为该月
 *	最后一日。
 * 加一天时，递增 mday，并使用一天内同一时刻。
 * 最后，加上"定量时间"。
 */
Datum
timestamp_pl_interval(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	Timestamp	result;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else
	{
		if (span->month != 0)
		{
			struct pg_tm tt,
					   *tm = &tt;
			fsec_t		fsec;

			if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			if (pg_add_s32_overflow(tm->tm_mon, span->month, &tm->tm_mon))
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			if (tm->tm_mon > MONTHS_PER_YEAR)
			{
				tm->tm_year += (tm->tm_mon - 1) / MONTHS_PER_YEAR;
				tm->tm_mon = ((tm->tm_mon - 1) % MONTHS_PER_YEAR) + 1;
			}
			else if (tm->tm_mon < 1)
			{
				tm->tm_year += tm->tm_mon / MONTHS_PER_YEAR - 1;
				tm->tm_mon = tm->tm_mon % MONTHS_PER_YEAR + MONTHS_PER_YEAR;
			}

			/* 针对月末边界问题做调整... */
			if (tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1])
				tm->tm_mday = (day_tab[isleap(tm->tm_year)][tm->tm_mon - 1]);

			if (tm2timestamp(tm, fsec, NULL, &timestamp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}

		if (span->day != 0)
		{
			struct pg_tm tt,
					   *tm = &tt;
			fsec_t		fsec;
			int			julian;

			if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			/*
			 * 以儒略日往复转换的方式加天数。此处需溢出
			 * 检查，因为 j2date 要求非负整数输入。
			 */
			julian = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
			if (pg_add_s32_overflow(julian, span->day, &julian) ||
				julian < 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			j2date(julian, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);

			if (tm2timestamp(tm, fsec, NULL, &timestamp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}

		if (pg_add_s64_overflow(timestamp, span->time, &timestamp))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		if (!IS_VALID_TIMESTAMP(timestamp))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		result = timestamp;
	}

	PG_RETURN_TIMESTAMP(result);
}

Datum
timestamp_mi_interval(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	Interval	tspan;

	tspan.month = -span->month;
	tspan.day = -span->day;
	tspan.time = -span->time;

	return DirectFunctionCall2(timestamp_pl_interval,
							   TimestampGetDatum(timestamp),
							   PointerGetDatum(&tspan));
}


/* timestamptz_pl_interval_internal()
 * 在给定（或会话）时区下把 interval 加到 timestamptz。
 *
 * 注意 interval 带有定性 year/month 与 day 单位的机制，
 *	所以要对它们做恰当处理。
 * 加一个月时，递增月份，并使用月内同一日。
 * 然后若下月天数较少，则把月内之日设为该月
 *	最后一日。
 * 加一天时，递增 mday，并使用一天内同一时刻。
 * 最后，加上"定量时间"。
 */
static TimestampTz
timestamptz_pl_interval_internal(TimestampTz timestamp,
								 Interval *span,
								 pg_tz *attimezone)
{
	TimestampTz result;
	int			tz;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else
	{
		/* 调用方要求默认时用会话时区 */
		if (attimezone == NULL)
			attimezone = session_timezone;

		if (span->month != 0)
		{
			struct pg_tm tt,
					   *tm = &tt;
			fsec_t		fsec;

			if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, attimezone) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			if (pg_add_s32_overflow(tm->tm_mon, span->month, &tm->tm_mon))
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			if (tm->tm_mon > MONTHS_PER_YEAR)
			{
				tm->tm_year += (tm->tm_mon - 1) / MONTHS_PER_YEAR;
				tm->tm_mon = ((tm->tm_mon - 1) % MONTHS_PER_YEAR) + 1;
			}
			else if (tm->tm_mon < 1)
			{
				tm->tm_year += tm->tm_mon / MONTHS_PER_YEAR - 1;
				tm->tm_mon = tm->tm_mon % MONTHS_PER_YEAR + MONTHS_PER_YEAR;
			}

			/* 针对月末边界问题做调整... */
			if (tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1])
				tm->tm_mday = (day_tab[isleap(tm->tm_year)][tm->tm_mon - 1]);

			tz = DetermineTimeZoneOffset(tm, attimezone);

			if (tm2timestamp(tm, fsec, &tz, &timestamp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}

		if (span->day != 0)
		{
			struct pg_tm tt,
					   *tm = &tt;
			fsec_t		fsec;
			int			julian;

			if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, attimezone) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			/*
			 * 以儒略日往复转换的方式加天数。此处需溢出
			 * 检查，因为 j2date 要求非负整数输入。
			 * 不过实践上小负儒略日它会给出正确答案；
			 * 我们应允许 -1，以避免
			 * 依赖时区的失败，如 timestamp.h 中所讨论。
			 */
			julian = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
			if (pg_add_s32_overflow(julian, span->day, &julian) ||
				julian < -1)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			j2date(julian, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);

			tz = DetermineTimeZoneOffset(tm, attimezone);

			if (tm2timestamp(tm, fsec, &tz, &timestamp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}

		if (pg_add_s64_overflow(timestamp, span->time, &timestamp))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		if (!IS_VALID_TIMESTAMP(timestamp))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		result = timestamp;
	}

	return result;
}

/* timestamptz_mi_interval_internal()
 * 同上，但减去 interval。
 */
static TimestampTz
timestamptz_mi_interval_internal(TimestampTz timestamp,
								 Interval *span,
								 pg_tz *attimezone)
{
	Interval	tspan;

	tspan.month = -span->month;
	tspan.day = -span->day;
	tspan.time = -span->time;

	return timestamptz_pl_interval_internal(timestamp, &tspan, attimezone);
}

/* timestamptz_pl_interval()
 * 在会话时区下把 interval 加到 timestamptz。
 */
Datum
timestamptz_pl_interval(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_TIMESTAMP(timestamptz_pl_interval_internal(timestamp, span, NULL));
}

Datum
timestamptz_mi_interval(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_TIMESTAMP(timestamptz_mi_interval_internal(timestamp, span, NULL));
}

/* timestamptz_pl_interval_at_zone()
 * 在指定时区下把 interval 加到 timestamptz。
 */
Datum
timestamptz_pl_interval_at_zone(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	text	   *zone = PG_GETARG_TEXT_PP(2);
	pg_tz	   *attimezone = lookup_timezone(zone);

	PG_RETURN_TIMESTAMP(timestamptz_pl_interval_internal(timestamp, span, attimezone));
}

Datum
timestamptz_mi_interval_at_zone(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	text	   *zone = PG_GETARG_TEXT_PP(2);
	pg_tz	   *attimezone = lookup_timezone(zone);

	PG_RETURN_TIMESTAMP(timestamptz_mi_interval_internal(timestamp, span, attimezone));
}

Datum
interval_um(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->time = -interval->time;
	/* 溢出检查复制自 int4um */
	if (interval->time != 0 && SAMESIGN(result->time, interval->time))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
	result->day = -interval->day;
	if (interval->day != 0 && SAMESIGN(result->day, interval->day))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
	result->month = -interval->month;
	if (interval->month != 0 && SAMESIGN(result->month, interval->month))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	PG_RETURN_INTERVAL_P(result);
}


Datum
interval_smaller(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;

	/* 使用 interval_cmp_internal 以确保与比较行为一致 */
	if (interval_cmp_internal(interval1, interval2) < 0)
		result = interval1;
	else
		result = interval2;
	PG_RETURN_INTERVAL_P(result);
}

Datum
interval_larger(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;

	if (interval_cmp_internal(interval1, interval2) > 0)
		result = interval1;
	else
		result = interval2;
	PG_RETURN_INTERVAL_P(result);
}

Datum
interval_pl(PG_FUNCTION_ARGS)
{
	Interval   *span1 = PG_GETARG_INTERVAL_P(0);
	Interval   *span2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->month = span1->month + span2->month;
	/* 溢出检查复制自 int4pl */
	if (SAMESIGN(span1->month, span2->month) &&
		!SAMESIGN(result->month, span1->month))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	result->day = span1->day + span2->day;
	if (SAMESIGN(span1->day, span2->day) &&
		!SAMESIGN(result->day, span1->day))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	result->time = span1->time + span2->time;
	if (SAMESIGN(span1->time, span2->time) &&
		!SAMESIGN(result->time, span1->time))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	PG_RETURN_INTERVAL_P(result);
}

Datum
interval_mi(PG_FUNCTION_ARGS)
{
	Interval   *span1 = PG_GETARG_INTERVAL_P(0);
	Interval   *span2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->month = span1->month - span2->month;
	/* 溢出检查复制自 int4mi */
	if (!SAMESIGN(span1->month, span2->month) &&
		!SAMESIGN(result->month, span1->month))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	result->day = span1->day - span2->day;
	if (!SAMESIGN(span1->day, span2->day) &&
		!SAMESIGN(result->day, span1->day))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	result->time = span1->time - span2->time;
	if (!SAMESIGN(span1->time, span2->time) &&
		!SAMESIGN(result->time, span1->time))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	PG_RETURN_INTERVAL_P(result);
}

/*
 *	没有 interval_abs()：不清楚该返回什么值：
 *	  http://archives.postgresql.org/pgsql-general/2009-10/msg01031.php
 *	  http://archives.postgresql.org/pgsql-general/2009-11/msg00041.php
 */

Datum
interval_mul(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	float8		factor = PG_GETARG_FLOAT8(1);
	double		month_remainder_days,
				sec_remainder,
				result_double;
	int32		orig_month = span->month,
				orig_day = span->day;
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result_double = span->month * factor;
	if (isnan(result_double) || !FLOAT8_FITS_IN_INT32(result_double))
		goto out_of_range;
	result->month = (int32) result_double;

	result_double = span->day * factor;
	if (isnan(result_double) || !FLOAT8_FITS_IN_INT32(result_double))
		goto out_of_range;
	result->day = (int32) result_double;

	/*
	 * 上面正确处理了 month 与 day 乘积的整数部分，
	 * 但当因子非整数时产生的小数部分仍需处理。
	 * 我们使用 DAYS_PER_MONTH 与 SECS_PER_DAY 这两个
	 * 换算因子把小数部分向下级联到更低位，
	 * 注意我们_不_向上级联，因为表示法并未强迫我们这
	 * 样做。用户之后可以选择用 justify_hours 和/或
	 * justify_days 来向上级联。
	 */

	/*
	 * 小数月的整天并入 day。
	 *
	 * 浮点计算天生不精确，因此这些计算经过精心
	 * 设计以求尽可能可靠的结果。
	 * 在适当时需要 TSROUND() 才能更准确地产生整数。
	 *
	 */
	month_remainder_days = (orig_month * factor - result->month) * DAYS_PER_MONTH;
	month_remainder_days = TSROUND(month_remainder_days);
	sec_remainder = (orig_day * factor - result->day +
					 month_remainder_days - (int) month_remainder_days) * SECS_PER_DAY;
	sec_remainder = TSROUND(sec_remainder);

	/*
	 * 可能因舍入得到 24:00:00 小时，或因 month 与 day
	 * 的时间级联而大于 24 小时。若级联与秒因子运算本身
	 * 的组合作用仍然可能大于 24。
	 */
	if (fabs(sec_remainder) >= SECS_PER_DAY)
	{
		if (pg_add_s32_overflow(result->day,
								(int) (sec_remainder / SECS_PER_DAY),
								&result->day))
			goto out_of_range;
		sec_remainder -= (int) (sec_remainder / SECS_PER_DAY) * SECS_PER_DAY;
	}

	/* 向下级联 */
	if (pg_add_s32_overflow(result->day, (int32) month_remainder_days,
							&result->day))
		goto out_of_range;
	result_double = rint(span->time * factor + sec_remainder * USECS_PER_SEC);
	if (isnan(result_double) || !FLOAT8_FITS_IN_INT64(result_double))
		goto out_of_range;
	result->time = (int64) result_double;

	PG_RETURN_INTERVAL_P(result);

out_of_range:
	ereport(ERROR,
			errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
			errmsg("interval out of range"));

	PG_RETURN_NULL();			/* 让编译器安静 */
}

Datum
mul_d_interval(PG_FUNCTION_ARGS)
{
	/* 参数是 float8 与 Interval *，但留作通用 Datum */
	Datum		factor = PG_GETARG_DATUM(0);
	Datum		span = PG_GETARG_DATUM(1);

	return DirectFunctionCall2(interval_mul, span, factor);
}

Datum
interval_div(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	float8		factor = PG_GETARG_FLOAT8(1);
	double		month_remainder_days,
				sec_remainder,
				result_double;
	int32		orig_month = span->month,
				orig_day = span->day;
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	if (factor == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result_double = span->month / factor;
	if (isnan(result_double) || !FLOAT8_FITS_IN_INT32(result_double))
		goto out_of_range;
	result->month = (int32) result_double;

	result_double = span->day / factor;
	if (isnan(result_double) || !FLOAT8_FITS_IN_INT32(result_double))
		goto out_of_range;
	result->day = (int32) result_double;

	/*
	 * 小数月的整天并入 day。参见 interval_mul() 中的注释。
	 */
	month_remainder_days = (orig_month / factor - result->month) * DAYS_PER_MONTH;
	month_remainder_days = TSROUND(month_remainder_days);
	sec_remainder = (orig_day / factor - result->day +
					 month_remainder_days - (int) month_remainder_days) * SECS_PER_DAY;
	sec_remainder = TSROUND(sec_remainder);
	if (fabs(sec_remainder) >= SECS_PER_DAY)
	{
		if (pg_add_s32_overflow(result->day,
								(int) (sec_remainder / SECS_PER_DAY),
								&result->day))
			goto out_of_range;
		sec_remainder -= (int) (sec_remainder / SECS_PER_DAY) * SECS_PER_DAY;
	}

	/* 向下级联 */
	if (pg_add_s32_overflow(result->day, (int32) month_remainder_days,
							&result->day))
		goto out_of_range;
	result_double = rint(span->time / factor + sec_remainder * USECS_PER_SEC);
	if (isnan(result_double) || !FLOAT8_FITS_IN_INT64(result_double))
		goto out_of_range;
	result->time = (int64) result_double;

	PG_RETURN_INTERVAL_P(result);

out_of_range:
	ereport(ERROR,
			errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
			errmsg("interval out of range"));

	PG_RETURN_NULL();			/* 让编译器安静 */
}


/*
 * timestamp 与 interval 的 in_range 支持函数。
 *
 * 依 SQL 标准，以 interval 为偏移类型支持它们。
 * 标准关于偏移不得为负的限制对 interval 来说有点
 * 难以解读，但我们选择按我们自己的 interval 比较
 * 算子的语义来解读。
 */

Datum
in_range_timestamptz_interval(PG_FUNCTION_ARGS)
{
	TimestampTz val = PG_GETARG_TIMESTAMPTZ(0);
	TimestampTz base = PG_GETARG_TIMESTAMPTZ(1);
	Interval   *offset = PG_GETARG_INTERVAL_P(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	TimestampTz sum;

	if (int128_compare(interval_cmp_value(offset), int64_to_int128(0)) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	/* 我们目前不费心去规避此处的溢出风险 */
	if (sub)
		sum = timestamptz_mi_interval_internal(base, offset, NULL);
	else
		sum = timestamptz_pl_interval_internal(base, offset, NULL);

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}

Datum
in_range_timestamp_interval(PG_FUNCTION_ARGS)
{
	Timestamp	val = PG_GETARG_TIMESTAMP(0);
	Timestamp	base = PG_GETARG_TIMESTAMP(1);
	Interval   *offset = PG_GETARG_INTERVAL_P(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	Timestamp	sum;

	if (int128_compare(interval_cmp_value(offset), int64_to_int128(0)) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	/* 我们目前不费心去规避此处的溢出风险 */
	if (sub)
		sum = DatumGetTimestamp(DirectFunctionCall2(timestamp_mi_interval,
													TimestampGetDatum(base),
													IntervalPGetDatum(offset)));
	else
		sum = DatumGetTimestamp(DirectFunctionCall2(timestamp_pl_interval,
													TimestampGetDatum(base),
													IntervalPGetDatum(offset)));

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}

Datum
in_range_interval_interval(PG_FUNCTION_ARGS)
{
	Interval   *val = PG_GETARG_INTERVAL_P(0);
	Interval   *base = PG_GETARG_INTERVAL_P(1);
	Interval   *offset = PG_GETARG_INTERVAL_P(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	Interval   *sum;

	if (int128_compare(interval_cmp_value(offset), int64_to_int128(0)) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	/* 我们目前不费心去规避此处的溢出风险 */
	if (sub)
		sum = DatumGetIntervalP(DirectFunctionCall2(interval_mi,
													IntervalPGetDatum(base),
													IntervalPGetDatum(offset)));
	else
		sum = DatumGetIntervalP(DirectFunctionCall2(interval_pl,
													IntervalPGetDatum(base),
													IntervalPGetDatum(offset)));

	if (less)
		PG_RETURN_BOOL(interval_cmp_internal(val, sum) <= 0);
	else
		PG_RETURN_BOOL(interval_cmp_internal(val, sum) >= 0);
}


/*
 * interval_accum、interval_accum_inv 与 interval_avg 实现
 * AVG(interval) 聚合。
 *
 * 该聚合的迁移数据类型为二元素 interval 数组，
 * 第一个元素是至今的累计和，第二个元素在其 'time'
 * 字段中存放至今的值个数。这有点丑，
 * 但总比为此专门发明一种数据类型要好。
 */

Datum
interval_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Interval   *newval = PG_GETARG_INTERVAL_P(1);
	Datum	   *transdatums;
	int			ndatums;
	Interval	sumX,
				N;
	Interval   *newsum;
	ArrayType  *result;

	deconstruct_array(transarray,
					  INTERVALOID, sizeof(Interval), false, TYPALIGN_DOUBLE,
					  &transdatums, NULL, &ndatums);
	if (ndatums != 2)
		elog(ERROR, "expected 2-element interval array");

	sumX = *(DatumGetIntervalP(transdatums[0]));
	N = *(DatumGetIntervalP(transdatums[1]));

	newsum = DatumGetIntervalP(DirectFunctionCall2(interval_pl,
												   IntervalPGetDatum(&sumX),
												   IntervalPGetDatum(newval)));
	N.time += 1;

	transdatums[0] = IntervalPGetDatum(newsum);
	transdatums[1] = IntervalPGetDatum(&N);

	result = construct_array(transdatums, 2,
							 INTERVALOID, sizeof(Interval), false, TYPALIGN_DOUBLE);

	PG_RETURN_ARRAYTYPE_P(result);
}

Datum
interval_combine(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *transarray2 = PG_GETARG_ARRAYTYPE_P(1);
	Datum	   *transdatums1;
	Datum	   *transdatums2;
	int			ndatums1;
	int			ndatums2;
	Interval	sum1,
				N1;
	Interval	sum2,
				N2;

	Interval   *newsum;
	ArrayType  *result;

	deconstruct_array(transarray1,
					  INTERVALOID, sizeof(Interval), false, TYPALIGN_DOUBLE,
					  &transdatums1, NULL, &ndatums1);
	if (ndatums1 != 2)
		elog(ERROR, "expected 2-element interval array");

	sum1 = *(DatumGetIntervalP(transdatums1[0]));
	N1 = *(DatumGetIntervalP(transdatums1[1]));

	deconstruct_array(transarray2,
					  INTERVALOID, sizeof(Interval), false, TYPALIGN_DOUBLE,
					  &transdatums2, NULL, &ndatums2);
	if (ndatums2 != 2)
		elog(ERROR, "expected 2-element interval array");

	sum2 = *(DatumGetIntervalP(transdatums2[0]));
	N2 = *(DatumGetIntervalP(transdatums2[1]));

	newsum = DatumGetIntervalP(DirectFunctionCall2(interval_pl,
												   IntervalPGetDatum(&sum1),
												   IntervalPGetDatum(&sum2)));
	N1.time += N2.time;

	transdatums1[0] = IntervalPGetDatum(newsum);
	transdatums1[1] = IntervalPGetDatum(&N1);

	result = construct_array(transdatums1, 2,
							 INTERVALOID, sizeof(Interval), false, TYPALIGN_DOUBLE);

	PG_RETURN_ARRAYTYPE_P(result);
}

Datum
interval_accum_inv(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Interval   *newval = PG_GETARG_INTERVAL_P(1);
	Datum	   *transdatums;
	int			ndatums;
	Interval	sumX,
				N;
	Interval   *newsum;
	ArrayType  *result;

	deconstruct_array(transarray,
					  INTERVALOID, sizeof(Interval), false, TYPALIGN_DOUBLE,
					  &transdatums, NULL, &ndatums);
	if (ndatums != 2)
		elog(ERROR, "expected 2-element interval array");

	sumX = *(DatumGetIntervalP(transdatums[0]));
	N = *(DatumGetIntervalP(transdatums[1]));

	newsum = DatumGetIntervalP(DirectFunctionCall2(interval_mi,
												   IntervalPGetDatum(&sumX),
												   IntervalPGetDatum(newval)));
	N.time -= 1;

	transdatums[0] = IntervalPGetDatum(newsum);
	transdatums[1] = IntervalPGetDatum(&N);

	result = construct_array(transdatums, 2,
							 INTERVALOID, sizeof(Interval), false, TYPALIGN_DOUBLE);

	PG_RETURN_ARRAYTYPE_P(result);
}

Datum
interval_avg(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Datum	   *transdatums;
	int			ndatums;
	Interval	sumX,
				N;

	deconstruct_array(transarray,
					  INTERVALOID, sizeof(Interval), false, TYPALIGN_DOUBLE,
					  &transdatums, NULL, &ndatums);
	if (ndatums != 2)
		elog(ERROR, "expected 2-element interval array");

	sumX = *(DatumGetIntervalP(transdatums[0]));
	N = *(DatumGetIntervalP(transdatums[1]));

	/* SQL 定义空输入的 AVG 为 NULL */
	if (N.time == 0)
		PG_RETURN_NULL();

	return DirectFunctionCall2(interval_div,
							   IntervalPGetDatum(&sumX),
							   Float8GetDatum((double) N.time));
}


/* timestamp_age()
 * 计算时间差，同时保留 year/month 字段。
 * 注意这并不会得到精确的绝对时间跨度，
 *	因为算完之后 year 与 month 便脱离上下文。
 *
 */
Datum
timestamp_age(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Interval   *result;
	fsec_t		fsec1,
				fsec2;
	struct pg_itm tt,
			   *tm = &tt;
	struct pg_tm tt1,
			   *tm1 = &tt1;
	struct pg_tm tt2,
			   *tm2 = &tt2;

	result = (Interval *) palloc(sizeof(Interval));

	if (timestamp2tm(dt1, NULL, tm1, &fsec1, NULL, NULL) == 0 &&
		timestamp2tm(dt2, NULL, tm2, &fsec2, NULL, NULL) == 0)
	{
		/* 构造符号化差值 */
		tm->tm_usec = fsec1 - fsec2;
		tm->tm_sec = tm1->tm_sec - tm2->tm_sec;
		tm->tm_min = tm1->tm_min - tm2->tm_min;
		tm->tm_hour = tm1->tm_hour - tm2->tm_hour;
		tm->tm_mday = tm1->tm_mday - tm2->tm_mday;
		tm->tm_mon = tm1->tm_mon - tm2->tm_mon;
		tm->tm_year = tm1->tm_year - tm2->tm_year;

		/* 必要时翻转符号... */
		if (dt1 < dt2)
		{
			tm->tm_usec = -tm->tm_usec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		/* 把负字段传播进更高一级字段 */
		while (tm->tm_usec < 0)
		{
			tm->tm_usec += USECS_PER_SEC;
			tm->tm_sec--;
		}

		while (tm->tm_sec < 0)
		{
			tm->tm_sec += SECS_PER_MINUTE;
			tm->tm_min--;
		}

		while (tm->tm_min < 0)
		{
			tm->tm_min += MINS_PER_HOUR;
			tm->tm_hour--;
		}

		while (tm->tm_hour < 0)
		{
			tm->tm_hour += HOURS_PER_DAY;
			tm->tm_mday--;
		}

		while (tm->tm_mday < 0)
		{
			if (dt1 < dt2)
			{
				tm->tm_mday += day_tab[isleap(tm1->tm_year)][tm1->tm_mon - 1];
				tm->tm_mon--;
			}
			else
			{
				tm->tm_mday += day_tab[isleap(tm2->tm_year)][tm2->tm_mon - 1];
				tm->tm_mon--;
			}
		}

		while (tm->tm_mon < 0)
		{
			tm->tm_mon += MONTHS_PER_YEAR;
			tm->tm_year--;
		}

		/* 必要时恢复符号... */
		if (dt1 < dt2)
		{
			tm->tm_usec = -tm->tm_usec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		if (itm2interval(tm, result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_INTERVAL_P(result);
}


/* timestamptz_age()
 * 计算时间差，同时保留 year/month 字段。
 * 注意这并不会得到精确的绝对时间跨度，
 *	因为算完之后 year 与 month 便脱离上下文。
 *
 */
Datum
timestamptz_age(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);
	Interval   *result;
	fsec_t		fsec1,
				fsec2;
	struct pg_itm tt,
			   *tm = &tt;
	struct pg_tm tt1,
			   *tm1 = &tt1;
	struct pg_tm tt2,
			   *tm2 = &tt2;
	int			tz1;
	int			tz2;

	result = (Interval *) palloc(sizeof(Interval));

	if (timestamp2tm(dt1, &tz1, tm1, &fsec1, NULL, NULL) == 0 &&
		timestamp2tm(dt2, &tz2, tm2, &fsec2, NULL, NULL) == 0)
	{
		/* 构造符号化差值 */
		tm->tm_usec = fsec1 - fsec2;
		tm->tm_sec = tm1->tm_sec - tm2->tm_sec;
		tm->tm_min = tm1->tm_min - tm2->tm_min;
		tm->tm_hour = tm1->tm_hour - tm2->tm_hour;
		tm->tm_mday = tm1->tm_mday - tm2->tm_mday;
		tm->tm_mon = tm1->tm_mon - tm2->tm_mon;
		tm->tm_year = tm1->tm_year - tm2->tm_year;

		/* 必要时翻转符号... */
		if (dt1 < dt2)
		{
			tm->tm_usec = -tm->tm_usec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		/* 把负字段传播进更高一级字段 */
		while (tm->tm_usec < 0)
		{
			tm->tm_usec += USECS_PER_SEC;
			tm->tm_sec--;
		}

		while (tm->tm_sec < 0)
		{
			tm->tm_sec += SECS_PER_MINUTE;
			tm->tm_min--;
		}

		while (tm->tm_min < 0)
		{
			tm->tm_min += MINS_PER_HOUR;
			tm->tm_hour--;
		}

		while (tm->tm_hour < 0)
		{
			tm->tm_hour += HOURS_PER_DAY;
			tm->tm_mday--;
		}

		while (tm->tm_mday < 0)
		{
			if (dt1 < dt2)
			{
				tm->tm_mday += day_tab[isleap(tm1->tm_year)][tm1->tm_mon - 1];
				tm->tm_mon--;
			}
			else
			{
				tm->tm_mday += day_tab[isleap(tm2->tm_year)][tm2->tm_mon - 1];
				tm->tm_mon--;
			}
		}

		while (tm->tm_mon < 0)
		{
			tm->tm_mon += MONTHS_PER_YEAR;
			tm->tm_year--;
		}

		/*
		 * 注意：我们有意忽略 tz1 与 tz2 之间的任何差异。
		 */

		/* 必要时恢复符号... */
		if (dt1 < dt2)
		{
			tm->tm_usec = -tm->tm_usec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		if (itm2interval(tm, result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_INTERVAL_P(result);
}


/*----------------------------------------------------------
 *	转换算子。
 *---------------------------------------------------------*/


/* timestamp_bin()
 * 以 interval 分箱 timestamp。
 */
Datum
timestamp_bin(PG_FUNCTION_ARGS)
{
	Interval   *stride = PG_GETARG_INTERVAL_P(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	Timestamp	origin = PG_GETARG_TIMESTAMP(2);
	Timestamp	result,
				stride_usecs,
				tm_diff,
				tm_modulo,
				tm_delta;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	if (TIMESTAMP_NOT_FINITE(origin))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("origin out of range")));

	if (stride->month != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("timestamps cannot be binned into intervals containing months or years")));

	if (unlikely(pg_mul_s64_overflow(stride->day, USECS_PER_DAY, &stride_usecs)) ||
		unlikely(pg_add_s64_overflow(stride_usecs, stride->time, &stride_usecs)))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	if (stride_usecs <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("stride must be greater than zero")));

	if (unlikely(pg_sub_s64_overflow(timestamp, origin, &tm_diff)))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	/* 这些计算不会溢出 */
	tm_modulo = tm_diff % stride_usecs;
	tm_delta = tm_diff - tm_modulo;
	result = origin + tm_delta;

	/*
	 * 当 tm_diff 为负且并非 stride_usecs 的倍数时，
	 * 我们要向负无穷舍入而非向 0 舍入。此调整*可能*
	 * 导致溢出，因为结果可能超出 origin .. timestamp。
	 */
	if (tm_modulo < 0)
	{
		if (unlikely(pg_sub_s64_overflow(result, stride_usecs, &result)) ||
			!IS_VALID_TIMESTAMP(result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}

	PG_RETURN_TIMESTAMP(result);
}

/* timestamp_trunc()
 * 把 timestamp 截断到指定单位。
 */
Datum
timestamp_trunc(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	Timestamp	result;
	int			type,
				val;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_WEEK:
				{
					int			woy;

					woy = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);

					/*
					 * 若为 52/53 周且月份为一月，该周必属于
					 * 上一年。此外，有些十二月日期属于
					 * 下一年。
					 */
					if (woy >= 52 && tm->tm_mon == 1)
						--tm->tm_year;
					if (woy <= 1 && tm->tm_mon == MONTHS_PER_YEAR)
						++tm->tm_year;
					isoweek2date(woy, &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
					tm->tm_hour = 0;
					tm->tm_min = 0;
					tm->tm_sec = 0;
					fsec = 0;
					break;
				}
			case DTK_MILLENNIUM:
				/* 见 timestamptz_trunc 注释 */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 999) / 1000) * 1000 - 999;
				else
					tm->tm_year = -((999 - (tm->tm_year - 1)) / 1000) * 1000 + 1;
				/* FALL THRU */
			case DTK_CENTURY:
				/* 见 timestamptz_trunc 注释 */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 99) / 100) * 100 - 99;
				else
					tm->tm_year = -((99 - (tm->tm_year - 1)) / 100) * 100 + 1;
				/* FALL THRU */
			case DTK_DECADE:
				/* 见 timestamptz_trunc 注释 */
				if (val != DTK_MILLENNIUM && val != DTK_CENTURY)
				{
					if (tm->tm_year > 0)
						tm->tm_year = (tm->tm_year / 10) * 10;
					else
						tm->tm_year = -((8 - (tm->tm_year - 1)) / 10) * 10;
				}
				/* FALL THRU */
			case DTK_YEAR:
				tm->tm_mon = 1;
				/* FALL THRU */
			case DTK_QUARTER:
				tm->tm_mon = (3 * ((tm->tm_mon - 1) / 3)) + 1;
				/* FALL THRU */
			case DTK_MONTH:
				tm->tm_mday = 1;
				/* FALL THRU */
			case DTK_DAY:
				tm->tm_hour = 0;
				/* FALL THRU */
			case DTK_HOUR:
				tm->tm_min = 0;
				/* FALL THRU */
			case DTK_MINUTE:
				tm->tm_sec = 0;
				/* FALL THRU */
			case DTK_SECOND:
				fsec = 0;
				break;

			case DTK_MILLISEC:
				fsec = (fsec / 1000) * 1000;
				break;

			case DTK_MICROSEC:
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMESTAMPOID))));
				result = 0;
		}

		if (tm2timestamp(tm, fsec, NULL, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(TIMESTAMPOID))));
		result = 0;
	}

	PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_bin()
 * 用指定起点把 timestamptz 按指定 interval 分箱。
 */
Datum
timestamptz_bin(PG_FUNCTION_ARGS)
{
	Interval   *stride = PG_GETARG_INTERVAL_P(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz origin = PG_GETARG_TIMESTAMPTZ(2);
	TimestampTz result,
				stride_usecs,
				tm_diff,
				tm_modulo,
				tm_delta;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMPTZ(timestamp);

	if (TIMESTAMP_NOT_FINITE(origin))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("origin out of range")));

	if (stride->month != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("timestamps cannot be binned into intervals containing months or years")));

	if (unlikely(pg_mul_s64_overflow(stride->day, USECS_PER_DAY, &stride_usecs)) ||
		unlikely(pg_add_s64_overflow(stride_usecs, stride->time, &stride_usecs)))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	if (stride_usecs <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("stride must be greater than zero")));

	if (unlikely(pg_sub_s64_overflow(timestamp, origin, &tm_diff)))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	/* 这些计算不会溢出 */
	tm_modulo = tm_diff % stride_usecs;
	tm_delta = tm_diff - tm_modulo;
	result = origin + tm_delta;

	/*
	 * 当 tm_diff 为负且并非 stride_usecs 的倍数时，
	 * 我们要向负无穷舍入而非向 0 舍入。此调整*可能*
	 * 导致溢出，因为结果可能超出 origin .. timestamp。
	 */
	if (tm_modulo < 0)
	{
		if (unlikely(pg_sub_s64_overflow(result, stride_usecs, &result)) ||
			!IS_VALID_TIMESTAMP(result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}

	PG_RETURN_TIMESTAMPTZ(result);
}

/*
 * timestamptz_trunc()/timestamptz_trunc_zone() 公共代码。
 *
 * tzp 标识截断所参照的时区。我们假定
 * 无限 timestamp 已被拒绝。
 */
static TimestampTz
timestamptz_trunc_internal(text *units, TimestampTz timestamp, pg_tz *tzp)
{
	TimestampTz result;
	int			tz;
	int			type,
				val;
	bool		redotz = false;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, tzp) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_WEEK:
				{
					int			woy;

					woy = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);

					/*
					 * 若为 52/53 周且月份为一月，该周必属于
					 * 上一年。此外，有些十二月日期属于
					 * 下一年。
					 */
					if (woy >= 52 && tm->tm_mon == 1)
						--tm->tm_year;
					if (woy <= 1 && tm->tm_mon == MONTHS_PER_YEAR)
						++tm->tm_year;
					isoweek2date(woy, &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
					tm->tm_hour = 0;
					tm->tm_min = 0;
					tm->tm_sec = 0;
					fsec = 0;
					redotz = true;
					break;
				}
				/* 可考虑 DTK_THOUSAND 与 DTK_HUNDRED... */
			case DTK_MILLENNIUM:

				/*
				 * 截断到千年？什么意思？我们取
				 * 千年的第一年……也就是：
				 * -1000, 1, 1001, 2001...
				 */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 999) / 1000) * 1000 - 999;
				else
					tm->tm_year = -((999 - (tm->tm_year - 1)) / 1000) * 1000 + 1;
				/* FALL THRU */
			case DTK_CENTURY:
				/* 截断到世纪？同上：-100, 1, 101... */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 99) / 100) * 100 - 99;
				else
					tm->tm_year = -((99 - (tm->tm_year - 1)) / 100) * 100 + 1;
				/* FALL THRU */
			case DTK_DECADE:

				/*
				 * 截断到十年？十年首年。若年份
				 * 之前已被截断，则不得应用！
				 */
				if (val != DTK_MILLENNIUM && val != DTK_CENTURY)
				{
					if (tm->tm_year > 0)
						tm->tm_year = (tm->tm_year / 10) * 10;
					else
						tm->tm_year = -((8 - (tm->tm_year - 1)) / 10) * 10;
				}
				/* FALL THRU */
			case DTK_YEAR:
				tm->tm_mon = 1;
				/* FALL THRU */
			case DTK_QUARTER:
				tm->tm_mon = (3 * ((tm->tm_mon - 1) / 3)) + 1;
				/* FALL THRU */
			case DTK_MONTH:
				tm->tm_mday = 1;
				/* FALL THRU */
			case DTK_DAY:
				tm->tm_hour = 0;
				redotz = true;	/* 凡 >= DAY 之情形 */
				/* FALL THRU */
			case DTK_HOUR:
				tm->tm_min = 0;
				/* FALL THRU */
			case DTK_MINUTE:
				tm->tm_sec = 0;
				/* FALL THRU */
			case DTK_SECOND:
				fsec = 0;
				break;
			case DTK_MILLISEC:
				fsec = (fsec / 1000) * 1000;
				break;
			case DTK_MICROSEC:
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMESTAMPTZOID))));
				result = 0;
		}

		if (redotz)
			tz = DetermineTimeZoneOffset(tm, tzp);

		if (tm2timestamp(tm, fsec, &tz, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(TIMESTAMPTZOID))));
		result = 0;
	}

	return result;
}

/* timestamptz_trunc()
 * 在会话时区中把 timestamptz 截断到指定单位。
 */
Datum
timestamptz_trunc(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz result;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMPTZ(timestamp);

	result = timestamptz_trunc_internal(units, timestamp, session_timezone);

	PG_RETURN_TIMESTAMPTZ(result);
}

/* timestamptz_trunc_zone()
 * 在指定时区中把 timestamptz 截断到指定单位。
 */
Datum
timestamptz_trunc_zone(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	text	   *zone = PG_GETARG_TEXT_PP(2);
	TimestampTz result;
	pg_tz	   *tzp;

	/*
	 * timestamptz_zone() 对无限输入不会查找时区，因此这里
	 * 同样不做。
	 */
	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	/*
	 * 查找所请求的时区。
	 */
	tzp = lookup_timezone(zone);

	result = timestamptz_trunc_internal(units, timestamp, tzp);

	PG_RETURN_TIMESTAMPTZ(result);
}

/* interval_trunc()
 * 从 interval 中抽取指定字段。
 */
Datum
interval_trunc(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Interval   *interval = PG_GETARG_INTERVAL_P(1);
	Interval   *result;
	int			type,
				val;
	char	   *lowunits;
	struct pg_itm tt,
			   *tm = &tt;

	result = (Interval *) palloc(sizeof(Interval));

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);

	if (type == UNITS)
	{
		interval2itm(*interval, tm);
		switch (val)
		{
			case DTK_MILLENNIUM:
				/* 注意：C 的除法余数可能为负 */
				tm->tm_year = (tm->tm_year / 1000) * 1000;
				/* FALL THRU */
			case DTK_CENTURY:
				/* 注意：C 的除法余数可能为负 */
				tm->tm_year = (tm->tm_year / 100) * 100;
				/* FALL THRU */
			case DTK_DECADE:
				/* 注意：C 的除法余数可能为负 */
				tm->tm_year = (tm->tm_year / 10) * 10;
				/* FALL THRU */
			case DTK_YEAR:
				tm->tm_mon = 0;
				/* FALL THRU */
			case DTK_QUARTER:
				tm->tm_mon = 3 * (tm->tm_mon / 3);
				/* FALL THRU */
			case DTK_MONTH:
				tm->tm_mday = 0;
				/* FALL THRU */
			case DTK_DAY:
				tm->tm_hour = 0;
				/* FALL THRU */
			case DTK_HOUR:
				tm->tm_min = 0;
				/* FALL THRU */
			case DTK_MINUTE:
				tm->tm_sec = 0;
				/* FALL THRU */
			case DTK_SECOND:
				tm->tm_usec = 0;
				break;
			case DTK_MILLISEC:
				tm->tm_usec = (tm->tm_usec / 1000) * 1000;
				break;
			case DTK_MICROSEC:
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(INTERVALOID)),
						 (val == DTK_WEEK) ? errdetail("Months usually have fractional weeks.") : 0));
		}

		if (itm2interval(tm, result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(INTERVALOID))));
	}

	PG_RETURN_INTERVAL_P(result);
}

/* isoweek2j()
 *
 *	返回与给定 ISO 8601 年和周相对应的儒略日。
 *	儒略日用于在 ISO 周日期与公历日期之间转换。
 */
int
isoweek2j(int year, int week)
{
	int			day0,
				day4;

	/* 当年的第四天 */
	day4 = date2j(year, 1, 4);

	/* day0 == 到一周首日（周一）的偏移 */
	day0 = j2day(day4 - 1);

	return ((week - 1) * 7) + (day4 - day0);
}

/* isoweek2date()
 * 把 ISO 年内周数转换为日期。
 * year 字段必须给定 ISO 年！
 * karel 2000/08/07
 */
void
isoweek2date(int woy, int *year, int *mon, int *mday)
{
	j2date(isoweek2j(*year, woy), year, mon, mday);
}

/* isoweekdate2date()
 *
 *	把 ISO 8601 周日期（ISO 年、周）转换为公历日期。
 *	传入公历的周内日，以便能提供周几字符串。
 *	以正确的公历值填充 year、mon 与 mday。
 *	year 必须以 ISO 年传入。
 */
void
isoweekdate2date(int isoweek, int wday, int *year, int *mon, int *mday)
{
	int			jday;

	jday = isoweek2j(*year, isoweek);
	/* 把公历周首日（周日=1）转成 ISO 周首日（周一=1） */
	if (wday > 1)
		jday += wday - 2;
	else
		jday += 6;
	j2date(jday, year, mon, mday);
}

/* date2isoweek()
 *
 *	返回 ISO 年内周数。
 */
int
date2isoweek(int year, int mon, int mday)
{
	float8		result;
	int			day0,
				day4,
				dayn;

	/* 当天 */
	dayn = date2j(year, mon, mday);

	/* 当年的第四天 */
	day4 = date2j(year, 1, 4);

	/* day0 == 到一周首日（周一）的偏移 */
	day0 = j2day(day4 - 1);

	/*
	 * 我们需要包含周四的第一周，否则按周计数而言
	 * 这一天会落入上一年。
	 */
	if (dayn < day4 - day0)
	{
		day4 = date2j(year - 1, 1, 4);

		/* day0 == 到一周首日（周一）的偏移 */
		day0 = j2day(day4 - 1);
	}

	result = (dayn - (day4 - day0)) / 7 + 1;

	/*
	 * 有时年末最后几天会落入下一年的第一周，所以要
	 * 对此进行检查。
	 */
	if (result >= 52)
	{
		day4 = date2j(year + 1, 1, 4);

		/* day0 == 到一周首日（周一）的偏移 */
		day0 = j2day(day4 - 1);

		if (dayn >= day4 - day0)
			result = (dayn - (day4 - day0)) / 7 + 1;
	}

	return (int) result;
}


/* date2isoyear()
 *
 *	返回 ISO 8601 年数。
 *	注意：零或负结果遵循"零年存在"约定。
 */
int
date2isoyear(int year, int mon, int mday)
{
	float8		result;
	int			day0,
				day4,
				dayn;

	/* 当天 */
	dayn = date2j(year, mon, mday);

	/* 当年的第四天 */
	day4 = date2j(year, 1, 4);

	/* day0 == 到一周首日（周一）的偏移 */
	day0 = j2day(day4 - 1);

	/*
	 * 我们需要包含周四的第一周，否则按周计数而言
	 * 这一天会落入上一年。
	 */
	if (dayn < day4 - day0)
	{
		day4 = date2j(year - 1, 1, 4);

		/* day0 == 到一周首日（周一）的偏移 */
		day0 = j2day(day4 - 1);

		year--;
	}

	result = (dayn - (day4 - day0)) / 7 + 1;

	/*
	 * 有时年末最后几天会落入下一年的第一周，所以要
	 * 对此进行检查。
	 */
	if (result >= 52)
	{
		day4 = date2j(year + 1, 1, 4);

		/* day0 == 到一周首日（周一）的偏移 */
		day0 = j2day(day4 - 1);

		if (dayn >= day4 - day0)
			year++;
	}

	return year;
}


/* date2isoyearday()
 *
 *	给定公历年、月、日，返回 ISO 8601 年内日数。
 *	可能的返回值为 1 到 371（非闰年为 364）。
 */
int
date2isoyearday(int year, int mon, int mday)
{
	return date2j(year, mon, mday) - isoweek2j(date2isoyear(year, mon, mday), 1) + 1;
}

/*
 * NonFiniteTimestampTzPart
 *
 *	供 timestamp_part 与 timestamptz_part 在从无限
 *	timestamp[tz] 抽取时用。适当时返回 +/-Infinity，
 *	否则返回零（应理解为返回 NULL）。
 *
 *	此处对无效单位抛出的错误必须与调用函数中会
 *	抛出的错误完全一致，否则有限输入与无限输入两
 *	种情形之间会出现意料之外的差异。
 */
static float8
NonFiniteTimestampTzPart(int type, int unit, char *lowunits,
						 bool isNegative, bool isTz)
{
	if ((type != UNITS) && (type != RESERV))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits,
						format_type_be(isTz ? TIMESTAMPTZOID : TIMESTAMPOID))));

	switch (unit)
	{
			/* 振荡型单位 */
		case DTK_MICROSEC:
		case DTK_MILLISEC:
		case DTK_SECOND:
		case DTK_MINUTE:
		case DTK_HOUR:
		case DTK_DAY:
		case DTK_MONTH:
		case DTK_QUARTER:
		case DTK_WEEK:
		case DTK_DOW:
		case DTK_ISODOW:
		case DTK_DOY:
		case DTK_TZ:
		case DTK_TZ_MINUTE:
		case DTK_TZ_HOUR:
			return 0.0;

			/* 单调递增型单位 */
		case DTK_YEAR:
		case DTK_DECADE:
		case DTK_CENTURY:
		case DTK_MILLENNIUM:
		case DTK_JULIAN:
		case DTK_ISOYEAR:
		case DTK_EPOCH:
			if (isNegative)
				return -get_float8_infinity();
			else
				return get_float8_infinity();

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unit \"%s\" not supported for type %s",
							lowunits,
							format_type_be(isTz ? TIMESTAMPTZOID : TIMESTAMPOID))));
			return 0.0;			/* 让编译器安静 */
	}
}

/* timestamp_part() and extract_timestamp()
 * 从 timestamp 中抽取指定字段。
 */
static Datum
timestamp_part_common(PG_FUNCTION_ARGS, bool retnumeric)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	int64		intresult;
	Timestamp	epoch;
	int			type,
				val;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		double		r = NonFiniteTimestampTzPart(type, val, lowunits,
												 TIMESTAMP_IS_NOBEGIN(timestamp),
												 false);

		if (r)
		{
			if (retnumeric)
			{
				if (r < 0)
					return DirectFunctionCall3(numeric_in,
											   CStringGetDatum("-Infinity"),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
				else if (r > 0)
					return DirectFunctionCall3(numeric_in,
											   CStringGetDatum("Infinity"),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
			}
			else
				PG_RETURN_FLOAT8(r);
		}
		else
			PG_RETURN_NULL();
	}

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

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

			case DTK_DAY:
				intresult = tm->tm_mday;
				break;

			case DTK_MONTH:
				intresult = tm->tm_mon;
				break;

			case DTK_QUARTER:
				intresult = (tm->tm_mon - 1) / 3 + 1;
				break;

			case DTK_WEEK:
				intresult = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);
				break;

			case DTK_YEAR:
				if (tm->tm_year > 0)
					intresult = tm->tm_year;
				else
					/* 无零年，仅公元前 1 与公元 1 */
					intresult = tm->tm_year - 1;
				break;

			case DTK_DECADE:

				/*
				 * 十年段对日期为何？我们假定 199 十年段
				 * 为 1990-1999…十年段 0 始于公元前 1 年，-1
				 * 为前 11 到前 2…
				 */
				if (tm->tm_year >= 0)
					intresult = tm->tm_year / 10;
				else
					intresult = -((8 - (tm->tm_year - 1)) / 10);
				break;

			case DTK_CENTURY:

				/* ----
				 * centuries AD, c>0: year in [ (c-1)* 100 + 1 : c*100 ]
				 * centuries BC, c<0: year in [ c*100 : (c+1) * 100 - 1]
				 * 不存在第 0 世纪。
				 * ----
				 */
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 99) / 100;
				else
					/* 注意：C 的除法余数可能为负 */
					intresult = -((99 - (tm->tm_year - 1)) / 100);
				break;

			case DTK_MILLENNIUM:
				/* 见上文注释。 */
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 999) / 1000;
				else
					intresult = -((999 - (tm->tm_year - 1)) / 1000);
				break;

			case DTK_JULIAN:
				if (retnumeric)
					PG_RETURN_NUMERIC(numeric_add_opt_error(int64_to_numeric(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)),
															numeric_div_opt_error(int64_to_numeric(((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) + tm->tm_sec) * INT64CONST(1000000) + fsec),
																				  int64_to_numeric(SECS_PER_DAY * INT64CONST(1000000)),
																				  NULL),
															NULL));
				else
					PG_RETURN_FLOAT8(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) +
									 ((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) +
									  tm->tm_sec + (fsec / 1000000.0)) / (double) SECS_PER_DAY);
				break;

			case DTK_ISOYEAR:
				intresult = date2isoyear(tm->tm_year, tm->tm_mon, tm->tm_mday);
				/* 调整 BC 年 */
				if (intresult <= 0)
					intresult -= 1;
				break;

			case DTK_DOW:
			case DTK_ISODOW:
				intresult = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (val == DTK_ISODOW && intresult == 0)
					intresult = 7;
				break;

			case DTK_DOY:
				intresult = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
							 - date2j(tm->tm_year, 1, 1) + 1);
				break;

			case DTK_TZ:
			case DTK_TZ_MINUTE:
			case DTK_TZ_HOUR:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMESTAMPOID))));
				intresult = 0;
		}
	}
	else if (type == RESERV)
	{
		switch (val)
		{
			case DTK_EPOCH:
				epoch = SetEpochTimestamp();
				/* (timestamp - epoch) / 1000000 */
				if (retnumeric)
				{
					Numeric		result;

					if (timestamp < (PG_INT64_MAX + epoch))
						result = int64_div_fast_to_numeric(timestamp - epoch, 6);
					else
					{
						result = numeric_div_opt_error(numeric_sub_opt_error(int64_to_numeric(timestamp),
																			 int64_to_numeric(epoch),
																			 NULL),
													   int64_to_numeric(1000000),
													   NULL);
						result = DatumGetNumeric(DirectFunctionCall2(numeric_round,
																	 NumericGetDatum(result),
																	 Int32GetDatum(6)));
					}
					PG_RETURN_NUMERIC(result);
				}
				else
				{
					float8		result;

					/* 尽量避免减法中的精度损失 */
					if (timestamp < (PG_INT64_MAX + epoch))
						result = (timestamp - epoch) / 1000000.0;
					else
						result = ((float8) timestamp - epoch) / 1000000.0;
					PG_RETURN_FLOAT8(result);
				}
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMESTAMPOID))));
				intresult = 0;
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(TIMESTAMPOID))));
		intresult = 0;
	}

	if (retnumeric)
		PG_RETURN_NUMERIC(int64_to_numeric(intresult));
	else
		PG_RETURN_FLOAT8(intresult);
}

Datum
timestamp_part(PG_FUNCTION_ARGS)
{
	return timestamp_part_common(fcinfo, false);
}

Datum
extract_timestamp(PG_FUNCTION_ARGS)
{
	return timestamp_part_common(fcinfo, true);
}

/* timestamptz_part() and extract_timestamptz()
 * 从带时区 timestamp 中抽取指定字段。
 */
static Datum
timestamptz_part_common(PG_FUNCTION_ARGS, bool retnumeric)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	int64		intresult;
	Timestamp	epoch;
	int			tz;
	int			type,
				val;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		double		r = NonFiniteTimestampTzPart(type, val, lowunits,
												 TIMESTAMP_IS_NOBEGIN(timestamp),
												 true);

		if (r)
		{
			if (retnumeric)
			{
				if (r < 0)
					return DirectFunctionCall3(numeric_in,
											   CStringGetDatum("-Infinity"),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
				else if (r > 0)
					return DirectFunctionCall3(numeric_in,
											   CStringGetDatum("Infinity"),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
			}
			else
				PG_RETURN_FLOAT8(r);
		}
		else
			PG_RETURN_NULL();
	}

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

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
				intresult = tm->tm_mday;
				break;

			case DTK_MONTH:
				intresult = tm->tm_mon;
				break;

			case DTK_QUARTER:
				intresult = (tm->tm_mon - 1) / 3 + 1;
				break;

			case DTK_WEEK:
				intresult = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);
				break;

			case DTK_YEAR:
				if (tm->tm_year > 0)
					intresult = tm->tm_year;
				else
					/* 无零年，仅公元前 1 与公元 1 */
					intresult = tm->tm_year - 1;
				break;

			case DTK_DECADE:
				/* 见 timestamp_part 注释 */
				if (tm->tm_year > 0)
					intresult = tm->tm_year / 10;
				else
					intresult = -((8 - (tm->tm_year - 1)) / 10);
				break;

			case DTK_CENTURY:
				/* 见 timestamp_part 注释 */
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 99) / 100;
				else
					intresult = -((99 - (tm->tm_year - 1)) / 100);
				break;

			case DTK_MILLENNIUM:
				/* 见 timestamp_part 注释 */
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 999) / 1000;
				else
					intresult = -((999 - (tm->tm_year - 1)) / 1000);
				break;

			case DTK_JULIAN:
				if (retnumeric)
					PG_RETURN_NUMERIC(numeric_add_opt_error(int64_to_numeric(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)),
															numeric_div_opt_error(int64_to_numeric(((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) + tm->tm_sec) * INT64CONST(1000000) + fsec),
																				  int64_to_numeric(SECS_PER_DAY * INT64CONST(1000000)),
																				  NULL),
															NULL));
				else
					PG_RETURN_FLOAT8(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) +
									 ((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) +
									  tm->tm_sec + (fsec / 1000000.0)) / (double) SECS_PER_DAY);
				break;

			case DTK_ISOYEAR:
				intresult = date2isoyear(tm->tm_year, tm->tm_mon, tm->tm_mday);
				/* 调整 BC 年 */
				if (intresult <= 0)
					intresult -= 1;
				break;

			case DTK_DOW:
			case DTK_ISODOW:
				intresult = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (val == DTK_ISODOW && intresult == 0)
					intresult = 7;
				break;

			case DTK_DOY:
				intresult = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
							 - date2j(tm->tm_year, 1, 1) + 1);
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMESTAMPTZOID))));
				intresult = 0;
		}
	}
	else if (type == RESERV)
	{
		switch (val)
		{
			case DTK_EPOCH:
				epoch = SetEpochTimestamp();
				/* (timestamp - epoch) / 1000000 */
				if (retnumeric)
				{
					Numeric		result;

					if (timestamp < (PG_INT64_MAX + epoch))
						result = int64_div_fast_to_numeric(timestamp - epoch, 6);
					else
					{
						result = numeric_div_opt_error(numeric_sub_opt_error(int64_to_numeric(timestamp),
																			 int64_to_numeric(epoch),
																			 NULL),
													   int64_to_numeric(1000000),
													   NULL);
						result = DatumGetNumeric(DirectFunctionCall2(numeric_round,
																	 NumericGetDatum(result),
																	 Int32GetDatum(6)));
					}
					PG_RETURN_NUMERIC(result);
				}
				else
				{
					float8		result;

					/* 尽量避免减法中的精度损失 */
					if (timestamp < (PG_INT64_MAX + epoch))
						result = (timestamp - epoch) / 1000000.0;
					else
						result = ((float8) timestamp - epoch) / 1000000.0;
					PG_RETURN_FLOAT8(result);
				}
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMESTAMPTZOID))));
				intresult = 0;
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(TIMESTAMPTZOID))));

		intresult = 0;
	}

	if (retnumeric)
		PG_RETURN_NUMERIC(int64_to_numeric(intresult));
	else
		PG_RETURN_FLOAT8(intresult);
}

Datum
timestamptz_part(PG_FUNCTION_ARGS)
{
	return timestamptz_part_common(fcinfo, false);
}

Datum
extract_timestamptz(PG_FUNCTION_ARGS)
{
	return timestamptz_part_common(fcinfo, true);
}


/* interval_part() and extract_interval()
 * 从 interval 中抽取指定字段。
 */
static Datum
interval_part_common(PG_FUNCTION_ARGS, bool retnumeric)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Interval   *interval = PG_GETARG_INTERVAL_P(1);
	int64		intresult;
	int			type,
				val;
	char	   *lowunits;
	struct pg_itm tt,
			   *tm = &tt;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (type == UNITS)
	{
		interval2itm(*interval, tm);
		switch (val)
		{
			case DTK_MICROSEC:
				intresult = tm->tm_sec * INT64CONST(1000000) + tm->tm_usec;
				break;

			case DTK_MILLISEC:
				if (retnumeric)
					/*---
					 * tm->tm_sec * 1000 + fsec / 1000
					 * = (tm->tm_sec * 1'000'000 + fsec) / 1000
					 */
					PG_RETURN_NUMERIC(int64_div_fast_to_numeric(tm->tm_sec * INT64CONST(1000000) + tm->tm_usec, 3));
				else
					PG_RETURN_FLOAT8(tm->tm_sec * 1000.0 + tm->tm_usec / 1000.0);
				break;

			case DTK_SECOND:
				if (retnumeric)
					/*---
					 * tm->tm_sec + fsec / 1'000'000
					 * = (tm->tm_sec * 1'000'000 + fsec) / 1'000'000
					 */
					PG_RETURN_NUMERIC(int64_div_fast_to_numeric(tm->tm_sec * INT64CONST(1000000) + tm->tm_usec, 6));
				else
					PG_RETURN_FLOAT8(tm->tm_sec + tm->tm_usec / 1000000.0);
				break;

			case DTK_MINUTE:
				intresult = tm->tm_min;
				break;

			case DTK_HOUR:
				intresult = tm->tm_hour;
				break;

			case DTK_DAY:
				intresult = tm->tm_mday;
				break;

			case DTK_MONTH:
				intresult = tm->tm_mon;
				break;

			case DTK_QUARTER:
				intresult = (tm->tm_mon / 3) + 1;
				break;

			case DTK_YEAR:
				intresult = tm->tm_year;
				break;

			case DTK_DECADE:
				/* 注意：C 的除法余数可能为负 */
				intresult = tm->tm_year / 10;
				break;

			case DTK_CENTURY:
				/* 注意：C 的除法余数可能为负 */
				intresult = tm->tm_year / 100;
				break;

			case DTK_MILLENNIUM:
				/* 注意：C 的除法余数可能为负 */
				intresult = tm->tm_year / 1000;
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(INTERVALOID))));
				intresult = 0;
		}
	}
	else if (type == RESERV && val == DTK_EPOCH)
	{
		if (retnumeric)
		{
			Numeric		result;
			int64		secs_from_day_month;
			int64		val;

			/*
			 * 虽然 DAYS_PER_YEAR 带小数，仍须以整数
			 * 运算来完成此计算：先把所有值乘以 4，最后再
			 * 除以 4。这依赖于 DAYS_PER_YEAR 是 0.25 的倍数，
			 * 且 SECS_PER_DAY 是 4 的倍数。
			 *
			 */
			secs_from_day_month = ((int64) (4 * DAYS_PER_YEAR) * (interval->month / MONTHS_PER_YEAR) +
								   (int64) (4 * DAYS_PER_MONTH) * (interval->month % MONTHS_PER_YEAR) +
								   (int64) 4 * interval->day) * (SECS_PER_DAY / 4);

			/*---
			 * result = secs_from_day_month + interval->time / 1'000'000
			 * = (secs_from_day_month * 1'000'000 + interval->time) / 1'000'000
			 */

			/*
			 * 先在 int64 内尝试计算；若溢出，则改用
			 * numeric（较慢）。该溢出发生在约 10^9 天附近，
			 * 实践中不常见。
			 */
			if (!pg_mul_s64_overflow(secs_from_day_month, 1000000, &val) &&
				!pg_add_s64_overflow(val, interval->time, &val))
				result = int64_div_fast_to_numeric(val, 6);
			else
				result =
					numeric_add_opt_error(int64_div_fast_to_numeric(interval->time, 6),
										  int64_to_numeric(secs_from_day_month),
										  NULL);

			PG_RETURN_NUMERIC(result);
		}
		else
		{
			float8		result;

			result = interval->time / 1000000.0;
			result += ((double) DAYS_PER_YEAR * SECS_PER_DAY) * (interval->month / MONTHS_PER_YEAR);
			result += ((double) DAYS_PER_MONTH * SECS_PER_DAY) * (interval->month % MONTHS_PER_YEAR);
			result += ((double) SECS_PER_DAY) * interval->day;

			PG_RETURN_FLOAT8(result);
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(INTERVALOID))));
		intresult = 0;
	}

	if (retnumeric)
		PG_RETURN_NUMERIC(int64_to_numeric(intresult));
	else
		PG_RETURN_FLOAT8(intresult);
}

Datum
interval_part(PG_FUNCTION_ARGS)
{
	return interval_part_common(fcinfo, false);
}

Datum
extract_interval(PG_FUNCTION_ARGS)
{
	return interval_part_common(fcinfo, true);
}


/*	timestamp_zone()
 *	用指定时区编码 timestamp 类型。
 *	本函数就是 timestamp2timestamptz()，只不过不是
 *	转换到全局时区，而是转换到指定时区。
 *	这与其他 AT TIME ZONE 用例不同：别的用例是把时间
 *	_转换到_新时区，而它是把时间设为_等于_指定
 *	时区。
 */
Datum
timestamp_zone(PG_FUNCTION_ARGS)
{
	text	   *zone = PG_GETARG_TEXT_PP(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	TimestampTz result;
	int			tz;
	char		tzname[TZ_STRLEN_MAX + 1];
	int			type,
				val;
	pg_tz	   *tzp;
	struct pg_tm tm;
	fsec_t		fsec;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMPTZ(timestamp);

	/*
	 * 查找所请求的时区。
	 */
	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	type = DecodeTimezoneName(tzname, &val, &tzp);

	if (type == TZNAME_FIXED_OFFSET)
	{
		/* 固定偏移量缩写 */
		tz = val;
		result = dt2local(timestamp, tz);
	}
	else if (type == TZNAME_DYNTZ)
	{
		/* 动态偏移量缩写，用指定时间解析 */
		if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, tzp) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		tz = -DetermineTimeZoneAbbrevOffset(&tm, tzname, tzp);
		result = dt2local(timestamp, tz);
	}
	else
	{
		/* 完整时区名，转到该时区 */
		if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, tzp) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		tz = DetermineTimeZoneOffset(&tm, tzp);
		if (tm2timestamp(&tm, fsec, &tz, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}

	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMPTZ(result);
}

/* timestamp_izone()
 * 用指定时间间隔作为时区来编码 timestamp 类型。
 */
Datum
timestamp_izone(PG_FUNCTION_ARGS)
{
	Interval   *zone = PG_GETARG_INTERVAL_P(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	TimestampTz result;
	int			tz;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMPTZ(timestamp);

	if (zone->month != 0 || zone->day != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval time zone \"%s\" must not include months or days",
						DatumGetCString(DirectFunctionCall1(interval_out,
															PointerGetDatum(zone))))));

	tz = zone->time / USECS_PER_SEC;

	result = dt2local(timestamp, tz);

	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMPTZ(result);
}								/* timestamp_izone() */

/* TimestampTimestampTzRequiresRewrite()
 *
 * 若 TimeZone GUC 设置使 timestamp_timestamptz 与
 * timestamptz_timestamp 成为无操作，即返回值与参数
 * 具有相同的位。由于项目惯例是假定 GUC 的变化频率
 * 不高于 STABLE 函数变化频率，故答案在该期限内有效。
 */
bool
TimestampTimestampTzRequiresRewrite(void)
{
	long		offset;

	if (pg_get_timezone_offset(session_timezone, &offset) && offset == 0)
		return false;
	return true;
}

/* timestamp_timestamptz()
 * 把本地 timestamp 转为 GMT timestamp
 */
Datum
timestamp_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);

	PG_RETURN_TIMESTAMPTZ(timestamp2timestamptz(timestamp));
}

/*
 * 把 timestamp 转换为带时区 timestamp。
 *
 * 转换成功时，若 *overflow 非 NULL 则将其置为零。
 *
 * 若 timestamp 有限但超出 timestamptz 的有效范围，那么：
 * 若 overflow 为 NULL，则抛出越界错误。
 * 若 overflow 非 NULL，则在其中存入 +1 或 -1 以标示
 * 溢出的符号，并返回相应的 timestamptz 无穷值。
 */
TimestampTz
timestamp2timestamptz_opt_overflow(Timestamp timestamp, int *overflow)
{
	TimestampTz result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	if (overflow)
		*overflow = 0;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		return timestamp;

	/* 不预期此处失败，但例行检查一下 */
	if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) == 0)
	{
		tz = DetermineTimeZoneOffset(tm, session_timezone);

		result = dt2local(timestamp, -tz);

		if (IS_VALID_TIMESTAMP(result))
		{
			return result;
		}
		else if (overflow)
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
			return result;
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
			 errmsg("timestamp out of range")));

	return 0;
}

/*
 * 把 timestamp 提升为 timestamptz，溢出则报错。
 */
static TimestampTz
timestamp2timestamptz(Timestamp timestamp)
{
	return timestamp2timestamptz_opt_overflow(timestamp, NULL);
}

/* timestamptz_timestamp()
 * GMT 处 timestamp 转为本地 timestamp
 */
Datum
timestamptz_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);

	PG_RETURN_TIMESTAMP(timestamptz2timestamp(timestamp));
}

static Timestamp
timestamptz2timestamp(TimestampTz timestamp)
{
	Timestamp	result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		if (tm2timestamp(tm, fsec, NULL, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}
	return result;
}

/* timestamptz_zone()
 * 在指定时区对带时区类型求值。
 * 返回不带时区的 timestamp。
 */
Datum
timestamptz_zone(PG_FUNCTION_ARGS)
{
	text	   *zone = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	Timestamp	result;
	int			tz;
	char		tzname[TZ_STRLEN_MAX + 1];
	int			type,
				val;
	pg_tz	   *tzp;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	/*
	 * 查找所请求的时区。
	 */
	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	type = DecodeTimezoneName(tzname, &val, &tzp);

	if (type == TZNAME_FIXED_OFFSET)
	{
		/* 固定偏移量缩写 */
		tz = -val;
		result = dt2local(timestamp, tz);
	}
	else if (type == TZNAME_DYNTZ)
	{
		/* 动态偏移量缩写，用指定时间解析 */
		int			isdst;

		tz = DetermineTimeZoneAbbrevOffsetTS(timestamp, tzname, tzp, &isdst);
		result = dt2local(timestamp, tz);
	}
	else
	{
		/* 完整时区名，自该时区转换 */
		struct pg_tm tm;
		fsec_t		fsec;

		if (timestamp2tm(timestamp, &tz, &tm, &fsec, NULL, tzp) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		if (tm2timestamp(&tm, fsec, NULL, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}

	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_izone()
 * 用指定时间间隔作为时区来编码带时区类型的 timestamp。
 * 返回不带时区的 timestamp。
 */
Datum
timestamptz_izone(PG_FUNCTION_ARGS)
{
	Interval   *zone = PG_GETARG_INTERVAL_P(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	Timestamp	result;
	int			tz;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	if (zone->month != 0 || zone->day != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval time zone \"%s\" must not include months or days",
						DatumGetCString(DirectFunctionCall1(interval_out,
															PointerGetDatum(zone))))));

	tz = -(zone->time / USECS_PER_SEC);

	result = dt2local(timestamp, tz);

	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMP(result);
}

/* generate_series_timestamp()
 * 按步长生成从起点到终点的 timestamp 集合
 */
Datum
generate_series_timestamp(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	generate_series_timestamp_fctx *fctx;
	Timestamp	result;

	/* 仅函数首次调用时执行的部分 */
	if (SRF_IS_FIRSTCALL())
	{
		Timestamp	start = PG_GETARG_TIMESTAMP(0);
		Timestamp	finish = PG_GETARG_TIMESTAMP(1);
		Interval   *step = PG_GETARG_INTERVAL_P(2);
		MemoryContext oldcontext;
		const Interval interval_zero = {0};

		/* 创建供跨调用持久化使用的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * 切换到适合多次函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* 为用户上下文分配内存 */
		fctx = (generate_series_timestamp_fctx *)
			palloc(sizeof(generate_series_timestamp_fctx));

		/*
		 * 用 fctx 在多次调用间保存状态。用原始起始值
		 * 为 current 赋值
		 */
		fctx->current = start;
		fctx->finish = finish;
		fctx->step = *step;

		/* 确定 interval 的符号 */
		fctx->step_sign = interval_cmp_internal(&fctx->step, &interval_zero);

		if (fctx->step_sign == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot equal zero")));

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* 函数每次调用时执行的部分 */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * 取回保存的状态，并把 current 用作本轮迭代结果
	 */
	fctx = funcctx->user_fctx;
	result = fctx->current;

	if (fctx->step_sign > 0 ?
		timestamp_cmp_internal(result, fctx->finish) <= 0 :
		timestamp_cmp_internal(result, fctx->finish) >= 0)
	{
		/* 递增 current 为下一轮迭代做准备 */
		fctx->current = DatumGetTimestamp(DirectFunctionCall2(timestamp_pl_interval,
															  TimestampGetDatum(fctx->current),
															  PointerGetDatum(&fctx->step)));

		/* 还有剩余要发送时执行 */
		SRF_RETURN_NEXT(funcctx, TimestampGetDatum(result));
	}
	else
	{
		/* 没有剩余时执行 */
		SRF_RETURN_DONE(funcctx);
	}
}

/* generate_series_timestamptz()
 * 按步长生成从起点到终点的 timestamp 集合，
 * 在指定或会话时区中进行算术运算。
 */
static Datum
generate_series_timestamptz_internal(FunctionCallInfo fcinfo)
{
	FuncCallContext *funcctx;
	generate_series_timestamptz_fctx *fctx;
	TimestampTz result;

	/* 仅函数首次调用时执行的部分 */
	if (SRF_IS_FIRSTCALL())
	{
		TimestampTz start = PG_GETARG_TIMESTAMPTZ(0);
		TimestampTz finish = PG_GETARG_TIMESTAMPTZ(1);
		Interval   *step = PG_GETARG_INTERVAL_P(2);
		text	   *zone = (PG_NARGS() == 4) ? PG_GETARG_TEXT_PP(3) : NULL;
		MemoryContext oldcontext;
		const Interval interval_zero = {0};

		/* 创建供跨调用持久化使用的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * 切换到适合多次函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* 为用户上下文分配内存 */
		fctx = (generate_series_timestamptz_fctx *)
			palloc(sizeof(generate_series_timestamptz_fctx));

		/*
		 * 用 fctx 在多次调用间保存状态。用原始起始值
		 * 为 current 赋值
		 */
		fctx->current = start;
		fctx->finish = finish;
		fctx->step = *step;
		fctx->attimezone = zone ? lookup_timezone(zone) : session_timezone;

		/* 确定 interval 的符号 */
		fctx->step_sign = interval_cmp_internal(&fctx->step, &interval_zero);

		if (fctx->step_sign == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot equal zero")));

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* 函数每次调用时执行的部分 */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * 取回保存的状态，并把 current 用作本轮迭代结果
	 */
	fctx = funcctx->user_fctx;
	result = fctx->current;

	if (fctx->step_sign > 0 ?
		timestamp_cmp_internal(result, fctx->finish) <= 0 :
		timestamp_cmp_internal(result, fctx->finish) >= 0)
	{
		/* 递增 current 为下一轮迭代做准备 */
		fctx->current = timestamptz_pl_interval_internal(fctx->current,
														 &fctx->step,
														 fctx->attimezone);

		/* 还有剩余要发送时执行 */
		SRF_RETURN_NEXT(funcctx, TimestampTzGetDatum(result));
	}
	else
	{
		/* 没有剩余时执行 */
		SRF_RETURN_DONE(funcctx);
	}
}

Datum
generate_series_timestamptz(PG_FUNCTION_ARGS)
{
	return generate_series_timestamptz_internal(fcinfo);
}

Datum
generate_series_timestamptz_at_zone(PG_FUNCTION_ARGS)
{
	return generate_series_timestamptz_internal(fcinfo);
}
