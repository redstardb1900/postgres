/*-------------------------------------------------------------------------
 *
 * pg_cast.h
 *	  definition of the "type casts" system catalog (pg_cast)
 *
 * As of Postgres 8.0, pg_cast describes not only type coercion functions
 * but also length coercion functions.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_cast.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CAST_H
#define PG_CAST_H

#include "catalog/dependency.h"
#include "catalog/genbki.h"
#include "catalog/pg_cast_d.h"

/* ----------------
 *		pg_cast definition.  cpp turns this into
 *		typedef struct FormData_pg_cast
 * ----------------
 */
CATALOG(pg_cast,2605,CastRelationId)
{
	Oid			oid;			/* oid */

	/* 转换的源数据类型 */
	Oid			castsource BKI_LOOKUP(pg_type);

	/*  转换的目标数据类型  */
	Oid			casttarget BKI_LOOKUP(pg_type);

	/*  转换函数;0=二进制可强制  */
	Oid			castfunc BKI_LOOKUP_OPT(pg_proc);

	/*  可使用转换的上下文  */
	char		castcontext;

	/* cast method */
	char		castmethod;
} FormData_pg_cast;

/* ----------------
 *		Form_pg_cast corresponds to a pointer to a tuple with
 *		the format of pg_cast relation.
 * ----------------
 */
typedef FormData_pg_cast *Form_pg_cast;

DECLARE_UNIQUE_INDEX_PKEY(pg_cast_oid_index, 2660, CastOidIndexId, on pg_cast using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_cast_source_target_index, 2661, CastSourceTargetIndexId, on pg_cast using btree(castsource oid_ops, casttarget oid_ops));

#ifdef EXPOSE_TO_CLIENT_CODE

/*
 * The allowable values for pg_cast.castcontext are specified by this enum.
 * Since castcontext is stored as a "char", we use ASCII codes for human
 * convenience in reading the table.  Note that internally to the backend,
 * these values are converted to the CoercionContext enum (see primnodes.h),
 * which is defined to sort in a convenient order; the ASCII codes don't
 * have to sort in any special order.
 */

typedef enum CoercionCodes
{
	COERCION_CODE_IMPLICIT = 'i',	/* coercion in context of expression */
	COERCION_CODE_ASSIGNMENT = 'a', /* 赋值上下文中的强制转换 */
	COERCION_CODE_EXPLICIT = 'e'	/*  显式转换操作  */
} CoercionCodes;

/*
 * The allowable values for pg_cast.castmethod are specified by this enum.
 * Since castmethod is stored as a "char", we use ASCII codes for human
 * convenience in reading the table.
 */
typedef enum CoercionMethod
{
	COERCION_METHOD_FUNCTION = 'f', /*  使用函数  */
	COERCION_METHOD_BINARY = 'b',	/*  类型二进制兼容  */
	COERCION_METHOD_INOUT = 'i' /* 使用输入/输出函数 */
} CoercionMethod;

#endif							/* EXPOSE_TO_CLIENT_CODE */


extern ObjectAddress CastCreate(Oid sourcetypeid,
								Oid targettypeid,
								Oid funcid,
								Oid incastid,
								Oid outcastid,
								char castcontext,
								char castmethod,
								DependencyType behavior);

#endif							/* PG_CAST_H */
