/*-------------------------------------------------------------------------
 *
 * readfuncs.h
 *	read.c 与 readfuncs.c 的头文件。这些函数仅供
 *	stringToNode 接口内部使用，他人勿用。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/readfuncs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef READFUNCS_H
#define READFUNCS_H

#include "nodes/nodes.h"

/*
 * read.c 中需供 readfuncs.c 访问的变量
 */
#ifdef WRITE_READ_PARSE_PLAN_TREES
extern PGDLLIMPORT bool restore_location_fields;
#endif

/*
 * read.c 中函数的原型（lisp 记号解析器）
 */
extern const char *pg_strtok(int *length);
extern char *debackslash(const char *token, int length);
extern void *nodeRead(const char *token, int tok_len);

/*
 * readfuncs.c 中函数的原型
 */
extern Node *parseNodeString(void);

#endif							/* READFUNCS_H */
