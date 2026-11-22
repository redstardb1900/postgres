/*-------------------------------------------------------------------------
 *
 * pg_statistic_ext.h
 *	  definition of the "extended statistics" system catalog
 *	  (pg_statistic_ext)
 *
 * Note that pg_statistic_ext contains the definitions of extended statistics
 * objects, created by CREATE STATISTICS, but not the actual statistical data,
 * created by running ANALYZE.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_statistic_ext.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_STATISTIC_EXT_H
#define PG_STATISTIC_EXT_H

#include "catalog/genbki.h"
#include "catalog/pg_statistic_ext_d.h"

/* ----------------
 *		pg_statistic_ext definition.  cpp turns this into
 *		typedef struct FormData_pg_statistic_ext
 * ----------------
 */
CATALOG(pg_statistic_ext,3381,StatisticExtRelationId)
{
	Oid			oid;			/* oid */

	Oid			stxrelid BKI_LOOKUP(pg_class);	/* relation containing
												 * attributes */

	/*  这两字段构成本条目的唯一键:  */
	NameData	stxname;		/*  统计对象名  */
	Oid			stxnamespace BKI_LOOKUP(pg_namespace);	/* OID of statistics
														 * object's namespace */

	Oid			stxowner BKI_LOOKUP(pg_authid); /*  统计对象所有者  */
	int32		stxstattarget BKI_DEFAULT(-1);	/*  统计目标  */

	/*
	 * 变长字段自此开始,但允许直接访问
	 * stxkeys
	 */
	int2vector	stxkeys BKI_FORCE_NOT_NULL; /*  列键数组  */

#ifdef CATALOG_VARLEN
	char		stxkind[1] BKI_FORCE_NOT_NULL;	/* statistics kinds requested
												 * to build */
	pg_node_tree stxexprs;		/*  统计用表达式树列表,
								 * 对应非简单列引用的
								 * 属性。  */
#endif

} FormData_pg_statistic_ext;

/* ----------------
 *		Form_pg_statistic_ext corresponds to a pointer to a tuple with
 *		the format of pg_statistic_ext relation.
 * ----------------
 */
typedef FormData_pg_statistic_ext *Form_pg_statistic_ext;

DECLARE_TOAST(pg_statistic_ext, 3439, 3440);

DECLARE_UNIQUE_INDEX_PKEY(pg_statistic_ext_oid_index, 3380, StatisticExtOidIndexId, on pg_statistic_ext using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_statistic_ext_name_index, 3997, StatisticExtNameIndexId, on pg_statistic_ext using btree(stxname name_ops, stxnamespace oid_ops));
DECLARE_INDEX(pg_statistic_ext_relid_index, 3379, StatisticExtRelidIndexId, on pg_statistic_ext using btree(stxrelid oid_ops));

DECLARE_ARRAY_FOREIGN_KEY((stxrelid, stxkeys), pg_attribute, (attrelid, attnum));

#ifdef EXPOSE_TO_CLIENT_CODE

#define STATS_EXT_NDISTINCT			'd'
#define STATS_EXT_DEPENDENCIES		'f'
#define STATS_EXT_MCV				'm'
#define STATS_EXT_EXPRESSIONS		'e'

#endif							/* EXPOSE_TO_CLIENT_CODE */

#endif							/* PG_STATISTIC_EXT_H */
