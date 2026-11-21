/*-------------------------------------------------------------------------
 *
 * postgres_ext.h
 *
 *	   本文件声明处处可见、且对 PostgreSQL 前端接口库客户
 *	亦可见的事物。例如 Oid 类型是 libpq 及其他库
 *	API 的一部分。
 *
 *	   特定接口专用的声明应放在该接口的头文件
 *	中（如 libpq-fe.h）。本文件仅放基本的
 *	Postgres 声明。
 *
 *	   用户写的 C 函数不算"Postgres 外部"。
 *	它们更像对后端本身的本地修改，并用本属
 *	Postgres 内部的头文件与后端接口。
 *	略
 *
 * src/include/postgres_ext.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POSTGRES_EXT_H
#define POSTGRES_EXT_H

#include "pg_config_ext.h"

/*
 * 对象 ID 是 Postgres 的基本类型。
 */
typedef unsigned int Oid;

#ifdef __cplusplus
#define InvalidOid		(Oid(0))
#else
#define InvalidOid		((Oid) 0)
#endif

#define OID_MAX  UINT_MAX
/*  使用上述 #define 须包含 <limits.h>  */

#define atooid(x) ((Oid) strtoul((x), NULL, 10))
/*  上述需要 <stdlib.h>  */


/*  定义有符号 64 位整数类型供客户端 API 声明使用。  */
typedef PG_INT64_TYPE pg_int64;

/*
 * 略
 * 共用，并向 libpq 应用导出。
 * 略
 */
#define PG_DIAG_SEVERITY		'S'
#define PG_DIAG_SEVERITY_NONLOCALIZED 'V'
#define PG_DIAG_SQLSTATE		'C'
#define PG_DIAG_MESSAGE_PRIMARY 'M'
#define PG_DIAG_MESSAGE_DETAIL	'D'
#define PG_DIAG_MESSAGE_HINT	'H'
#define PG_DIAG_STATEMENT_POSITION 'P'
#define PG_DIAG_INTERNAL_POSITION 'p'
#define PG_DIAG_INTERNAL_QUERY	'q'
#define PG_DIAG_CONTEXT			'W'
#define PG_DIAG_SCHEMA_NAME		's'
#define PG_DIAG_TABLE_NAME		't'
#define PG_DIAG_COLUMN_NAME		'c'
#define PG_DIAG_DATATYPE_NAME	'd'
#define PG_DIAG_CONSTRAINT_NAME 'n'
#define PG_DIAG_SOURCE_FILE		'F'
#define PG_DIAG_SOURCE_LINE		'L'
#define PG_DIAG_SOURCE_FUNCTION 'R'

#endif							/* POSTGRES_EXT_H */
