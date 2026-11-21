/*-------------------------------------------------------------------------
 *
 * header.h
 *		Snowball 词干模块的替换头文件
 *
 * Snowball 词干模块会 #include "header.h"，并以为
 * 包含的是 snowball/libstemmer/header.h。我们调整 CPPFLAGS
 * 使找到本文件，从而可修改它们所见的头。
 * 要点是确保 pg_config.h 在任何系统头（如 <stdio.h>）
 * 之前被包含；否则因后端各模块 largefile 选项
 * 不一致，某些平台会有可移植性问题。
 * 略
 *
 * 注意：非 snowball 源文件不应包含本文件！
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * src/include/snowball/header.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SNOWBALL_HEADR_H
#define SNOWBALL_HEADR_H

/*
 * 按 Postgres 编码约定，头文件中不应包含
 * postgres.h，但此处允许违规，因替代方案是
 * 修改 Snowball 项目提供的机器生成 .c 文件。
 */
#include "postgres.h"

/*  有些平台定义 MAXINT/MININT，造成冲突  */
#ifdef MAXINT
#undef MAXINT
#endif
#ifdef MININT
#undef MININT
#endif

/* 现在可包含原版 Snowball 头文件。 */
#include "snowball/libstemmer/header.h" /* pgrminclude ignore */

/*
 * 将标准内存分配接口重定义为 pgsql 的。
 * 这样可控制 Snowball 代码在何处分配。
 */
#ifdef malloc
#undef malloc
#endif
#define malloc(a)		palloc(a)

#ifdef calloc
#undef calloc
#endif
#define calloc(a,b)		palloc0((a) * (b))

#ifdef realloc
#undef realloc
#endif
#define realloc(a,b)	repalloc(a,b)

#ifdef free
#undef free
#endif
#define free(a)			pfree(a)

#endif							/* SNOWBALL_HEADR_H */
