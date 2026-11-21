/*-------------------------------------------------------------------------
 *
 * pgtime.h
 *	  PostgreSQL 内部时区库
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/include/pgtime.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PGTIME_H
#define _PGTIME_H


/*
 * 本库 API 大体类似对应的 C 库函数，但我们用
 * pg_time_t（希望是 64 位），且明确为有符号
 * 而非无符号。
 */

typedef int64 pg_time_t;

/*
 * 表示分解时间戳的数据结构。
 *
 * CAUTION：IANA 时区库（src/timezone/）遵循 POSIX
 * 约定：tm_mon 从 0 计，tm_year 相对 1900。
 * 而 Postgres 日期时间函数通常把 tm_mon 从 1
 * 计、tm_year 相对公元前 1 年。跨代码域移动时
 * 务必做相应调整。
 */
struct pg_tm
{
	int			tm_sec;
	int			tm_min;
	int			tm_hour;
	int			tm_mday;
	int			tm_mon;			/* 略 */
	int			tm_year;		/* 略 */
	int			tm_wday;
	int			tm_yday;
	int			tm_isdst;
	long int	tm_gmtoff;
	const char *tm_zone;
};

/*  这些结构在时区库外不透明  */
typedef struct pg_tz pg_tz;
typedef struct pg_tzenum pg_tzenum;

/*  时区名最大长度（不含结尾空字符）  */
#define TZ_STRLEN_MAX 255

/*  这些函数在 localtime.c  */

extern struct pg_tm *pg_localtime(const pg_time_t *timep, const pg_tz *tz);
extern struct pg_tm *pg_gmtime(const pg_time_t *timep);
extern int	pg_next_dst_boundary(const pg_time_t *timep,
								 long int *before_gmtoff,
								 int *before_isdst,
								 pg_time_t *boundary,
								 long int *after_gmtoff,
								 int *after_isdst,
								 const pg_tz *tz);
extern bool pg_interpret_timezone_abbrev(const char *abbrev,
										 const pg_time_t *timep,
										 long int *gmtoff,
										 int *isdst,
										 const pg_tz *tz);
extern bool pg_get_timezone_offset(const pg_tz *tz, long int *gmtoff);
extern const char *pg_get_timezone_name(pg_tz *tz);
extern bool pg_tz_acceptable(pg_tz *tz);

/*  这些函数在 strftime.c  */

extern size_t pg_strftime(char *s, size_t maxsize, const char *format,
						  const struct pg_tm *t);

/*  这些函数与变量在 pgtz.c  */

extern PGDLLIMPORT pg_tz *session_timezone;
extern PGDLLIMPORT pg_tz *log_timezone;

extern void pg_timezone_initialize(void);
extern pg_tz *pg_tzset(const char *tzname);
extern pg_tz *pg_tzset_offset(long gmtoffset);

extern pg_tzenum *pg_tzenumerate_start(void);
extern pg_tz *pg_tzenumerate_next(pg_tzenum *dir);
extern void pg_tzenumerate_end(pg_tzenum *dir);

#endif							/* _PGTIME_H */
