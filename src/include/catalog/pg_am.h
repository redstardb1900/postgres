/*-------------------------------------------------------------------------
 *
 * pg_am.h
 *	  definition of the "access method" system catalog (pg_am)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_am.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AM_H
#define PG_AM_H

#include "catalog/genbki.h"
#include "catalog/pg_am_d.h"

/* ----------------
 *		pg_am definition.  cpp turns this into
 *		typedef struct FormData_pg_am
 * ----------------
 */
CATALOG(pg_am,2601,AccessMethodRelationId)
{
	Oid			oid;			/* oid */

	/*  访问方法名  */
	NameData	amname;

	/*  处理函数  */
	regproc		amhandler BKI_LOOKUP(pg_proc);

	/*  见下方 AMTYPE_xxx 常量  */
	char		amtype;
} FormData_pg_am;

/* ----------------
 *		Form_pg_am corresponds to a pointer to a tuple with
 *		the format of pg_am relation.
 * ----------------
 */
typedef FormData_pg_am *Form_pg_am;

DECLARE_UNIQUE_INDEX(pg_am_name_index, 2651, AmNameIndexId, on pg_am using btree(amname name_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_am_oid_index, 2652, AmOidIndexId, on pg_am using btree(oid oid_ops));

#ifdef EXPOSE_TO_CLIENT_CODE

/*
 * amtype 的允许取值
 */
#define AMTYPE_INDEX					'i' /* 索引访问方法 */
#define AMTYPE_TABLE					't' /*  表访问方法  */

#endif							/* EXPOSE_TO_CLIENT_CODE */

#endif							/* PG_AM_H */
