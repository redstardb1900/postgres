/*-------------------------------------------------------------------------
 *
 * datetime.h
 *	  日期/时间支持代码的定义。
 *	  支持代码与其他日期数据类型共享，
 *	   包括 date 与 time。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/datetime.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATETIME_H
#define DATETIME_H

#include "utils/timestamp.h"

/* 此结构在 utils/tzparser.h 中声明： */
struct tzEntry;


/* ----------------------------------------------------------------
 *				time 类型 + 支持宏
 *
 * 标准时间量的字符串定义。
 *
 * 这些字符串是形成输出时间串的默认值。
 * 其他备选形式硬编码在 datetime.c 的词元表中。
 * ----------------------------------------------------------------
 */

#define DAGO			"ago"
#define DCURRENT		"current"
#define EPOCH			"epoch"
#define INVALID			"invalid"
#define EARLY			"-infinity"
#define LATE			"infinity"
#define NOW				"now"
#define TODAY			"today"
#define TOMORROW		"tomorrow"
#define YESTERDAY		"yesterday"
#define ZULU			"zulu"

#define DMICROSEC		"usecond"
#define DMILLISEC		"msecond"
#define DSECOND			"second"
#define DMINUTE			"minute"
#define DHOUR			"hour"
#define DDAY			"day"
#define DWEEK			"week"
#define DMONTH			"month"
#define DQUARTER		"quarter"
#define DYEAR			"year"
#define DDECADE			"decade"
#define DCENTURY		"century"
#define DMILLENNIUM		"millennium"
#define DA_D			"ad"
#define DB_C			"bc"
#define DTIMEZONE		"timezone"

/*
 * 用于解析的基本时间字段定义。
 *
 *	子午线: am、pm 或 24 时制。
 *	千禧：ad、bc
 */

#define AM		0
#define PM		1
#define HR24	2

#define AD		0
#define BC		1

/*
 * 时间解码的字段类型。
 *
 * 不得超过 unsigned 的位数，
 * 因解析解码时会转为位掩码。
 *
 * 此外 YEAR、MONTH、DAY、HOUR、MINUTE、SECOND
 * 的值须在 0..14，以便相关位掩码能放入
 * INTERVAL typmod 值的左半。这些位存于 typmod，
 * 更改须 initdb！
 */

#define RESERV	0
#define MONTH	1
#define YEAR	2
#define DAY		3
#define JULIAN	4
#define TZ		5				/* 固定偏移时区缩写 */
#define DTZ		6				/* 固定偏移时区缩写，DST */
#define DYNTZ	7				/* 动态时区缩写 */
#define IGNORE_DTF	8
#define AMPM	9
#define HOUR	10
#define MINUTE	11
#define SECOND	12
#define MILLISECOND 13
#define MICROSECOND 14
#define DOY		15
#define DOW		16
#define UNITS	17
#define ADBC	18
/* 这些仅用于相对日期 */
#define AGO		19
#define ABS_BEFORE		20
#define ABS_AFTER		21
/* 辅助解析的通用字段 */
#define ISODATE 22
#define ISOTIME 23
/* 这些仅用于解析区间 */
#define WEEK		24
#define DECADE		25
#define CENTURY		26
#define MILLENNIUM	27
/* 解析双词时区规格 "MET DST" 等的技巧 */
#define DTZMOD	28				/* "DST" 作为单独词 */
/* reserved for unrecognized string values */
#define UNKNOWN_FIELD	31

/*
 * 时间解析与解码的词元字段定义。
 *
 * 某些字段类型码（见上）在 datetktbl 中用这些作 "value"
 * DecodeDateTime 等也用它们作位掩码，故暂限在
 *	[0,31] 内。
 * - thomas 97/06/19
 * 并非所有这些字段都用于 DecodeDateTime 掩码，
 *	故允许部分大于 31。- thomas 1997-11-17
 *
 * 注意：代码中有未文档化假定，多数这些值不等于
 * IGNORE_DTF 或 RESERV。重编号任一侧看似独立的
 * 列表时务必小心 :-(
 */

#define DTK_NUMBER		0
#define DTK_STRING		1

#define DTK_DATE		2
#define DTK_TIME		3
#define DTK_TZ			4
#define DTK_AGO			5

#define DTK_SPECIAL		6
#define DTK_EARLY		9
#define DTK_LATE		10
#define DTK_EPOCH		11
#define DTK_NOW			12
#define DTK_YESTERDAY	13
#define DTK_TODAY		14
#define DTK_TOMORROW	15
#define DTK_ZULU		16

#define DTK_DELTA		17
#define DTK_SECOND		18
#define DTK_MINUTE		19
#define DTK_HOUR		20
#define DTK_DAY			21
#define DTK_WEEK		22
#define DTK_MONTH		23
#define DTK_QUARTER		24
#define DTK_YEAR		25
#define DTK_DECADE		26
#define DTK_CENTURY		27
#define DTK_MILLENNIUM	28
#define DTK_MILLISEC	29
#define DTK_MICROSEC	30
#define DTK_JULIAN		31

#define DTK_DOW			32
#define DTK_DOY			33
#define DTK_TZ_HOUR		34
#define DTK_TZ_MINUTE	35
#define DTK_ISOYEAR		36
#define DTK_ISODOW		37


/*
 * 时间解析的位掩码定义。
 */

#define DTK_M(t)		(0x01 << (t))

/* 便利：一秒，加任何小数分量 */
#define DTK_ALL_SECS_M	(DTK_M(SECOND) | DTK_M(MILLISECOND) | DTK_M(MICROSECOND))
#define DTK_DATE_M		(DTK_M(YEAR) | DTK_M(MONTH) | DTK_M(DAY))
#define DTK_TIME_M		(DTK_M(HOUR) | DTK_M(MINUTE) | DTK_ALL_SECS_M)

/*
 * interval、timestamp 等输入输出的工作缓冲区大小。
 * 需更多工作空间的输入将早拒。更长输出会冲出缓冲，
 * 故须足以覆盖所有可能输出。撰写时 interval_out()
 * 需最多空间，约 90 字节。
 */
#define MAXDATELEN		128
/* 日期串中可能的最大字段数 */
#define MAXDATEFIELDS	25
/* datetktbl 中只存这么多字符 */
#define TOKMAXLEN		10

/* 保持此结构小；用得很多 */
typedef struct
{
	char		token[TOKMAXLEN + 1];	/* 总是以 NUL 结尾 */
	char		type;			/* 见上方字段类型码 */
	int32		value;			/* 含义取决于类型 */
} datetkn;

/* 用途之一是时区缩写表 */
typedef struct TimeZoneAbbrevTable
{
	Size		tblsize;		/* TimeZoneAbbrevTable 的字节大小 */
	int			numabbrevs;		/* abbrevs[] 数组中的条目数 */
	datetkn		abbrevs[FLEXIBLE_ARRAY_MEMBER];
	/* DynamicZoneAbbrev 可跟在 abbrevs[] 数组后 */
} TimeZoneAbbrevTable;

/* 动态时区缩写的辅助数据（非固定偏移） */
typedef struct DynamicZoneAbbrev
{
	pg_tz	   *tz;				/* 尚未查找则为 NULL */
	char		zone[FLEXIBLE_ARRAY_MEMBER];	/* NUL 结尾的时区名 */
} DynamicZoneAbbrev;


/* FMODULO()
 * 替代某些平台上有问题的 modf() 的宏。
 * t = 输入与余数
 * q = 整数部分
 * u = 除数
 */
#define FMODULO(t,q,u) \
do { \
	(q) = (((t) < 0) ? ceil((t) / (u)) : floor((t) / (u))); \
	if ((q) != 0) (t) -= rint((q) * (u)); \
} while(0)

/* TMODULO()
 * 类似 FMODULO()，但作用于 timestamp（现恒 int64）。
 * 假定 int64 遵循 C99 除法语义（负商向零截断）。
 * 见上。
 */
#define TMODULO(t,q,u) \
do { \
	(q) = ((t) / (u)); \
	if ((q) != 0) (t) -= ((q) * (u)); \
} while(0)

/*
 * 日期/时间校验
 * 含闰年检查。
 */

extern PGDLLIMPORT const char *const months[];	/* 月份(3字
												 * 缩写） */
extern PGDLLIMPORT const char *const days[];	/* 日（全名） */
extern PGDLLIMPORT const int day_tab[2][13];

/*
 * 这些是格里高利历规则（1582 年采用）。但我们
 * 对更早年份也用此计算，因 SQL 标准规定用格里
 * 高利历。这使 1500-02-29 无法存储，尽管在儒略
 * 历中有效。
 * 见上。
 */
#define isleap(y) (((y) % 4) == 0 && (((y) % 100) != 0 || ((y) % 400) == 0))


/*
 * 日期时间输入解析例程(ParseDateTime、DecodeDateTime 等)
 * 成功时返回零或正值。失败时返回下列负码之一。
 * 可用 DateTimeParseError 产生合适错误报告。对某些
 * 码，DateTimeParseError 还需额外信息，由结构
 * DateTimeErrorExtra 携带。
 * 见上。
 */
#define DTERR_BAD_FORMAT		(-1)
#define DTERR_FIELD_OVERFLOW	(-2)
#define DTERR_MD_FIELD_OVERFLOW (-3)	/* 触发 DateStyle 相关提示 */
#define DTERR_INTERVAL_OVERFLOW (-4)
#define DTERR_TZDISP_OVERFLOW	(-5)
#define DTERR_BAD_TIMEZONE		(-6)
#define DTERR_BAD_ZONE_ABBREV	(-7)

typedef struct DateTimeErrorExtra
{
	/* DTERR_BAD_TIMEZONE 与 DTERR_BAD_ZONE_ABBREV 所需： */
	const char *dtee_timezone;	/* 不正确的时区名 */
	/* DTERR_BAD_ZONE_ABBREV 所需： */
	const char *dtee_abbrev;	/* 相关时区缩写 */
} DateTimeErrorExtra;

/* DecodeTimezoneName() 的结果码 */
#define TZNAME_FIXED_OFFSET	0
#define TZNAME_DYNTZ		1
#define TZNAME_ZONE			2


extern void GetCurrentDateTime(struct pg_tm *tm);
extern void GetCurrentTimeUsec(struct pg_tm *tm, fsec_t *fsec, int *tzp);
extern void j2date(int jd, int *year, int *month, int *day);
extern int	date2j(int year, int month, int day);

extern int	ParseDateTime(const char *timestr, char *workbuf, size_t buflen,
						  char **field, int *ftype,
						  int maxfields, int *numfields);
extern int	DecodeDateTime(char **field, int *ftype, int nf,
						   int *dtype, struct pg_tm *tm, fsec_t *fsec, int *tzp,
						   DateTimeErrorExtra *extra);
extern int	DecodeTimezone(const char *str, int *tzp);
extern int	DecodeTimeOnly(char **field, int *ftype, int nf,
						   int *dtype, struct pg_tm *tm, fsec_t *fsec, int *tzp,
						   DateTimeErrorExtra *extra);
extern int	DecodeInterval(char **field, int *ftype, int nf, int range,
						   int *dtype, struct pg_itm_in *itm_in);
extern int	DecodeISO8601Interval(char *str,
								  int *dtype, struct pg_itm_in *itm_in);

extern void DateTimeParseError(int dterr, DateTimeErrorExtra *extra,
							   const char *str, const char *datatype,
							   struct Node *escontext);

extern int	DetermineTimeZoneOffset(struct pg_tm *tm, pg_tz *tzp);
extern int	DetermineTimeZoneAbbrevOffset(struct pg_tm *tm, const char *abbr, pg_tz *tzp);
extern int	DetermineTimeZoneAbbrevOffsetTS(TimestampTz ts, const char *abbr,
											pg_tz *tzp, int *isdst);

extern void EncodeDateOnly(struct pg_tm *tm, int style, char *str);
extern void EncodeTimeOnly(struct pg_tm *tm, fsec_t fsec, bool print_tz, int tz, int style, char *str);
extern void EncodeDateTime(struct pg_tm *tm, fsec_t fsec, bool print_tz, int tz, const char *tzn, int style, char *str);
extern void EncodeInterval(struct pg_itm *itm, int style, char *str);
extern void EncodeSpecialTimestamp(Timestamp dt, char *str);

extern int	ValidateDate(int fmask, bool isjulian, bool is2digits, bool bc,
						 struct pg_tm *tm);

extern int	DecodeTimezoneAbbrev(int field, const char *lowtoken,
								 int *ftype, int *offset, pg_tz **tz,
								 DateTimeErrorExtra *extra);
extern int	DecodeSpecial(int field, const char *lowtoken, int *val);
extern int	DecodeUnits(int field, const char *lowtoken, int *val);

extern int	DecodeTimezoneName(const char *tzname, int *offset, pg_tz **tz);
extern pg_tz *DecodeTimezoneNameToTz(const char *tzname);

extern int	j2day(int date);

extern struct Node *TemporalSimplify(int32 max_precis, struct Node *node);

extern bool CheckDateTokenTables(void);

extern TimeZoneAbbrevTable *ConvertTimeZoneAbbrevs(struct tzEntry *abbrevs,
												   int n);
extern void InstallTimeZoneAbbrevs(TimeZoneAbbrevTable *tbl);

extern bool AdjustTimestampForTypmod(Timestamp *time, int32 typmod,
									 struct Node *escontext);

#endif							/* DATETIME_H */
