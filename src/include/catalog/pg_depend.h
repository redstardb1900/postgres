/*-------------------------------------------------------------------------
 *
 * pg_depend.h
 *	  definition of the "dependency" system catalog (pg_depend)
 *
 * pg_depend has no preloaded contents, so there is no pg_depend.dat
 * file; dependencies for system-defined objects are loaded into it
 * on-the-fly during initdb.  Most built-in objects are pinned anyway,
 * and hence need no explicit entries in pg_depend.
 *
 * NOTE: we do not represent all possible dependency pairs in pg_depend;
 * for example, there's not much value in creating an explicit dependency
 * from an attribute to its relation.  Usually we make a dependency for
 * cases where the relationship is conditional rather than essential
 * (for example, not all triggers are dependent on constraints, but all
 * attributes are dependent on relations) or where the dependency is not
 * convenient to find from the contents of other catalogs.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_depend.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DEPEND_H
#define PG_DEPEND_H

#include "catalog/genbki.h"
#include "catalog/pg_depend_d.h"

/* ----------------
 *		pg_depend definition.  cpp turns this into
 *		typedef struct FormData_pg_depend
 * ----------------
 */
CATALOG(pg_depend,2608,DependRelationId)
{
	/*
	 * 从属(引用方)对象的标识。
	 */
	Oid			classid BKI_LOOKUP(pg_class);	/*  含下列项的表 OID
												 * 对象 */
	Oid			objid;			/*  对象自身 OID  */
	int32		objsubid;		/*  列号,未用则为 0  */

	/*
	 * 独立(被引用方)对象的标识。
	 */
	Oid			refclassid BKI_LOOKUP(pg_class);	/*  含下列项的表 OID
													 * 对象 */
	Oid			refobjid;		/*  对象自身 OID  */
	int32		refobjsubid;	/*  列号,未用则为 0  */

	/*
	 * Precise semantics of the relationship are specified by the deptype
	 * field.  See DependencyType in catalog/dependency.h.
	 */
	char		deptype;		/* see codes in dependency.h */
} FormData_pg_depend;

/* ----------------
 *		Form_pg_depend corresponds to a pointer to a row with
 *		the format of pg_depend relation.
 * ----------------
 */
typedef FormData_pg_depend *Form_pg_depend;

DECLARE_INDEX(pg_depend_depender_index, 2673, DependDependerIndexId, on pg_depend using btree(classid oid_ops, objid oid_ops, objsubid int4_ops));
DECLARE_INDEX(pg_depend_reference_index, 2674, DependReferenceIndexId, on pg_depend using btree(refclassid oid_ops, refobjid oid_ops, refobjsubid int4_ops));

#endif							/* PG_DEPEND_H */
