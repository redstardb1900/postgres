/*-------------------------------------------------------------------------
 *
 * pg_rusage.h
 *	  资源用量测量支持例程的头文件
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/pg_rusage.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_RUSAGE_H
#define PG_RUSAGE_H

#include <sys/resource.h>
#include <sys/time.h>


/*  pg_rusage_init/pg_rusage_show 的状态结构  */
typedef struct PGRUsage
{
	struct timeval tv;
	struct rusage ru;
} PGRUsage;


extern void pg_rusage_init(PGRUsage *ru0);
extern const char *pg_rusage_show(const PGRUsage *ru0);

#endif							/* PG_RUSAGE_H */
