/*-------------------------------------------------------------------------
 *
 * fmgrtab.h
 *	  函数管理器的内部函数表。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/fmgrtab.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FMGRTAB_H
#define FMGRTAB_H

#include "access/transam.h"
#include "fmgr.h"


/*
 * 本表存储所有内建函数的信息（即编译进
 * Postgres 可执行文件的函数）。
 */

typedef struct
{
	Oid			foid;			/* 函数的 OID */
	short		nargs;			/* 0..FUNC_MAX_ARGS，-1 为可变 */
	bool		strict;			/* "strict" 函数则为 T */
	bool		retset;			/* 函数返回集合则为 T */
	const char *funcName;		/* 函数的 C 名 */
	PGFunction	func;			/* 已编译函数指针 */
} FmgrBuiltin;

extern PGDLLIMPORT const FmgrBuiltin fmgr_builtins[];

extern PGDLLIMPORT const int fmgr_nbuiltins;	/* 表中条目数 */

extern PGDLLIMPORT const Oid fmgr_last_builtin_oid; /* 表中最高函数 OID
													 *  */

/*
 * 从内建函数 OID 到其在 fmgr_builtins
 * 数组中下标的映射。下标 0..fmgr_last_builtin_oid。
 */
#define InvalidOidBuiltinMapping PG_UINT16_MAX
extern PGDLLIMPORT const uint16 fmgr_builtin_oid_index[];

#endif							/* FMGRTAB_H */
