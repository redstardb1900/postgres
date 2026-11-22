/*-------------------------------------------------------------------------
 *
 * pg_range.h
 *	  definition of the "range type" system catalog (pg_range)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_range.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_RANGE_H
#define PG_RANGE_H

#include "catalog/genbki.h"
#include "catalog/pg_range_d.h"

/* ----------------
 *		pg_range definition.  cpp turns this into
 *		typedef struct FormData_pg_range
 * ----------------
 */
CATALOG(pg_range,3541,RangeRelationId)
{
	/*  所属范围类型 OID  */
	Oid			rngtypid BKI_LOOKUP(pg_type);

	/*  范围元素类型(子类型) OID  */
	Oid			rngsubtype BKI_LOOKUP(pg_type);

	/*  范围的多范围类型 OID  */
	Oid			rngmultitypid BKI_LOOKUP(pg_type);

	/* collation for this range type, or 0 */
	Oid			rngcollation BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_collation);

	/* subtype's btree opclass */
	Oid			rngsubopc BKI_LOOKUP(pg_opclass);

	/*  规范化范围,或 0  */
	regproc		rngcanonical BKI_LOOKUP_OPT(pg_proc);

	/*  子类型差值为 float8,或 0  */
	regproc		rngsubdiff BKI_LOOKUP_OPT(pg_proc);
} FormData_pg_range;

/* ----------------
 *		Form_pg_range corresponds to a pointer to a tuple with
 *		the format of pg_range relation.
 * ----------------
 */
typedef FormData_pg_range *Form_pg_range;

DECLARE_UNIQUE_INDEX_PKEY(pg_range_rngtypid_index, 3542, RangeTypidIndexId, on pg_range using btree(rngtypid oid_ops));
DECLARE_UNIQUE_INDEX(pg_range_rngmultitypid_index, 2228, RangeMultirangeTypidIndexId, on pg_range using btree(rngmultitypid oid_ops));

/*
 * pg_range.c 中函数原型
 */

extern void RangeCreate(Oid rangeTypeOid, Oid rangeSubType, Oid rangeCollation,
						Oid rangeSubOpclass, RegProcedure rangeCanonical,
						RegProcedure rangeSubDiff, Oid multirangeTypeOid);
extern void RangeDelete(Oid rangeTypeOid);

#endif							/* PG_RANGE_H */
