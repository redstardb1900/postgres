/*-------------------------------------------------------------------------
 *
 * pg_rusage.c
 *	  资源使用情况度量支持例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/pg_rusage.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "utils/pg_rusage.h"


/*
 * 建立用量快照。
 */
void
pg_rusage_init(PGRUsage *ru0)
{
	getrusage(RUSAGE_SELF, &ru0->ru);
	gettimeofday(&ru0->tv, NULL);
}

/*
 * 计算自 ru0 用量快照以来经过时间，并格式化成
 * 可显示的字符串。结果存于静态字符串，这种
 * 做法很逊，但从来没人声称 Postgres 后端是可
 * 线程化的
 */
const char *
pg_rusage_show(const PGRUsage *ru0)
{
	static char result[100];
	PGRUsage	ru1;

	pg_rusage_init(&ru1);

	if (ru1.tv.tv_usec < ru0->tv.tv_usec)
	{
		ru1.tv.tv_sec--;
		ru1.tv.tv_usec += 1000000;
	}
	if (ru1.ru.ru_stime.tv_usec < ru0->ru.ru_stime.tv_usec)
	{
		ru1.ru.ru_stime.tv_sec--;
		ru1.ru.ru_stime.tv_usec += 1000000;
	}
	if (ru1.ru.ru_utime.tv_usec < ru0->ru.ru_utime.tv_usec)
	{
		ru1.ru.ru_utime.tv_sec--;
		ru1.ru.ru_utime.tv_usec += 1000000;
	}

	snprintf(result, sizeof(result),
			 _("CPU: user: %d.%02d s, system: %d.%02d s, elapsed: %d.%02d s"),
			 (int) (ru1.ru.ru_utime.tv_sec - ru0->ru.ru_utime.tv_sec),
			 (int) (ru1.ru.ru_utime.tv_usec - ru0->ru.ru_utime.tv_usec) / 10000,
			 (int) (ru1.ru.ru_stime.tv_sec - ru0->ru.ru_stime.tv_sec),
			 (int) (ru1.ru.ru_stime.tv_usec - ru0->ru.ru_stime.tv_usec) / 10000,
			 (int) (ru1.tv.tv_sec - ru0->tv.tv_sec),
			 (int) (ru1.tv.tv_usec - ru0->tv.tv_usec) / 10000);

	return result;
}
