/*-------------------------------------------------------------------------
 *
 * pg_proc.h
 *	  definition of the "procedure" system catalog (pg_proc)
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_proc.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PROC_H
#define PG_PROC_H

#include "catalog/genbki.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_proc_d.h"
#include "nodes/pg_list.h"

/* ----------------
 *		pg_proc definition.  cpp turns this into
 *		typedef struct FormData_pg_proc
 * ----------------
 */
CATALOG(pg_proc,1255,ProcedureRelationId) BKI_BOOTSTRAP BKI_ROWTYPE_OID(81,ProcedureRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	Oid			oid;			/* oid */

	/*  过程名  */
	NameData	proname;

	/*  含本过程的命名空间 OID  */
	Oid			pronamespace BKI_DEFAULT(pg_catalog) BKI_LOOKUP(pg_namespace);

	/* 过程所有者 */
	Oid			proowner BKI_DEFAULT(POSTGRES) BKI_LOOKUP(pg_authid);

	/*  pg_language 条目 OID  */
	Oid			prolang BKI_DEFAULT(internal) BKI_LOOKUP(pg_language);

	/*  估计执行代价  */
	float4		procost BKI_DEFAULT(1);

	/*  估计输出行数(若 proretset)  */
	float4		prorows BKI_DEFAULT(0);

	/*  可变参数数组元素类型,非可变则为 0  */
	Oid			provariadic BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_type);

	/*  本函数的规划器支持函数,无则为 0  */
	regproc		prosupport BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_proc);

	/*  见下方 PROKIND_ 类别  */
	char		prokind BKI_DEFAULT(f);

	/* 安全定义者 */
	bool		prosecdef BKI_DEFAULT(f);

	/*  是否为防泄漏函数?  */
	bool		proleakproof BKI_DEFAULT(f);

	/*  对 NULL 是否严格?  */
	bool		proisstrict BKI_DEFAULT(t);

	/* returns a set? */
	bool		proretset BKI_DEFAULT(f);

	/*  见下方 PROVOLATILE_ 类别  */
	char		provolatile BKI_DEFAULT(i);

	/*  见下方 PROPARALLEL_ 类别  */
	char		proparallel BKI_DEFAULT(s);

	/*  参数个数  */
	/*  注:不必在 pg_proc.dat 中给出;由 genbki.pl 计算  */
	int16		pronargs;

	/*  带默认值的参数个数  */
	int16		pronargdefaults BKI_DEFAULT(0);

	/*  结果类型 OID  */
	Oid			prorettype BKI_LOOKUP(pg_type);

	/*
	 * 变长字段自此开始,但允许直接访问
	 * proargtypes
	 */

	/*  参数类型(不含 OUT 参数)  */
	oidvector	proargtypes BKI_LOOKUP(pg_type) BKI_FORCE_NOT_NULL;

#ifdef CATALOG_VARLEN

	/* all param types (NULL if IN only) */
	Oid			proallargtypes[1] BKI_DEFAULT(_null_) BKI_LOOKUP(pg_type);

	/* 参数模式(仅 IN 则为 NULL) */
	char		proargmodes[1] BKI_DEFAULT(_null_);

	/*  参数名(无名则为 NULL)  */
	text		proargnames[1] BKI_DEFAULT(_null_);

	/*  参数默认值表达式树列表(无则为 NULL)  */
	pg_node_tree proargdefaults BKI_DEFAULT(_null_);

	/*  对其应用变换的类型  */
	Oid			protrftypes[1] BKI_DEFAULT(_null_) BKI_LOOKUP(pg_type);

	/*  过程源文本  */
	text		prosrc BKI_FORCE_NOT_NULL;

	/*  次要过程信息(可为 NULL)  */
	text		probin BKI_DEFAULT(_null_);

	/*  预解析的 SQL 函数体  */
	pg_node_tree prosqlbody BKI_DEFAULT(_null_);

	/*  过程局部 GUC 设置  */
	text		proconfig[1] BKI_DEFAULT(_null_);

	/*  访问权限  */
	aclitem		proacl[1] BKI_DEFAULT(_null_);
#endif
} FormData_pg_proc;

/* ----------------
 *		Form_pg_proc corresponds to a pointer to a tuple with
 *		the format of pg_proc relation.
 * ----------------
 */
typedef FormData_pg_proc *Form_pg_proc;

DECLARE_TOAST(pg_proc, 2836, 2837);

DECLARE_UNIQUE_INDEX_PKEY(pg_proc_oid_index, 2690, ProcedureOidIndexId, on pg_proc using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_proc_proname_args_nsp_index, 2691, ProcedureNameArgsNspIndexId, on pg_proc using btree(proname name_ops, proargtypes oidvector_ops, pronamespace oid_ops));

#ifdef EXPOSE_TO_CLIENT_CODE

/*
 * prokind 列的符号值
 */
#define PROKIND_FUNCTION 'f'
#define PROKIND_AGGREGATE 'a'
#define PROKIND_WINDOW 'w'
#define PROKIND_PROCEDURE 'p'

/*
 * Symbolic values for provolatile column: these indicate whether the result
 * of a function is dependent *only* on the values of its explicit arguments,
 * or can change due to outside factors (such as parameter variables or
 * table contents).  NOTE: functions having side-effects, such as setval(),
 * must be labeled volatile to ensure they will not get optimized away,
 * even if the actual return value is not changeable.
 */
#define PROVOLATILE_IMMUTABLE	'i' /*  给定输入永不改变  */
#define PROVOLATILE_STABLE		's' /*  单次扫描内不变  */
#define PROVOLATILE_VOLATILE	'v' /* can change even within a scan */

/*
 * Symbolic values for proparallel column: these indicate whether a function
 * can be safely be run in a parallel backend, during parallelism but
 * necessarily in the leader, or only in non-parallel mode.
 */
#define PROPARALLEL_SAFE		's' /* can run in worker or leader */
#define PROPARALLEL_RESTRICTED	'r' /* can run in parallel leader only */
#define PROPARALLEL_UNSAFE		'u' /*  并行模式下禁止  */

/*
 * Symbolic values for proargmodes column.  Note that these must agree with
 * the FunctionParameterMode enum in parsenodes.h; we declare them here to
 * be accessible from either header.
 */
#define PROARGMODE_IN		'i'
#define PROARGMODE_OUT		'o'
#define PROARGMODE_INOUT	'b'
#define PROARGMODE_VARIADIC 'v'
#define PROARGMODE_TABLE	't'

#endif							/* EXPOSE_TO_CLIENT_CODE */


extern ObjectAddress ProcedureCreate(const char *procedureName,
									 Oid procNamespace,
									 bool replace,
									 bool returnsSet,
									 Oid returnType,
									 Oid proowner,
									 Oid languageObjectId,
									 Oid languageValidator,
									 const char *prosrc,
									 const char *probin,
									 Node *prosqlbody,
									 char prokind,
									 bool security_definer,
									 bool isLeakProof,
									 bool isStrict,
									 char volatility,
									 char parallel,
									 oidvector *parameterTypes,
									 Datum allParameterTypes,
									 Datum parameterModes,
									 Datum parameterNames,
									 List *parameterDefaults,
									 Datum trftypes,
									 Datum proconfig,
									 Oid prosupport,
									 float4 procost,
									 float4 prorows);

extern bool function_parse_error_transpose(const char *prosrc);

extern List *oid_array_to_list(Datum datum);

#endif							/* PG_PROC_H */
