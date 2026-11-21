/*
 * 使用 getopt(3) 的 Postgres 文件总包含本文件。
 * 须应对三种情形：
 * 1. 用平台 getopt()，只需导入
 *    适当声明。
 * 2. 平台无 getopt()，须自行全部声明。
 * 3. 平台有 getopt()，但因不喜欢其行为而不用。
 *    此处声明须同时兼容平台 getopt() 与
 *    我们的 src/port/getopt.c。
 *
 * Portions Copyright (c) 1987, 1993, 1994
 * The Regents of the University of California.  All rights reserved.
 *
 * Portions Copyright (c) 2003-2023, PostgreSQL Global Development Group
 *
 * src/include/pg_getopt.h
 */
#ifndef PG_GETOPT_H
#define PG_GETOPT_H

/*  POSIX 说 getopt() 由 unistd.h 提供  */
#include <unistd.h>

/*  若有则依赖系统的 getopt.h  */
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

/*
 * 若有 <getopt.h>，假定它声明这些变量，否则自行
 * 声明。（曾无条件声明，但 Cygwin
 * 不喜欢。）
 */
#ifndef HAVE_GETOPT_H

extern PGDLLIMPORT char *optarg;
extern PGDLLIMPORT int optind;
extern PGDLLIMPORT int opterr;
extern PGDLLIMPORT int optopt;

#endif							/* HAVE_GETOPT_H */

/*
 * 有些平台有 optreset 却未在 <getopt.h> 声明，须应付。
 * 但 Cygwin 也不喜欢这样。
 */
#if defined(HAVE_INT_OPTRESET) && !defined(__CYGWIN__)
extern PGDLLIMPORT int optreset;
#endif

/*  平台无 getopt() 时提供其声明  */
#ifndef HAVE_GETOPT
extern int	getopt(int nargc, char *const *nargv, const char *ostr);
#endif

#endif							/* PG_GETOPT_H */
