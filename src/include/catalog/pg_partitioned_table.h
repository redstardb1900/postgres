/*-------------------------------------------------------------------------
 *
 * pg_partitioned_table.h
 *	  definition of the "partitioned table" system catalog
 *	  (pg_partitioned_table)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_partitioned_table.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PARTITIONED_TABLE_H
#define PG_PARTITIONED_TABLE_H

#include "catalog/genbki.h"
#include "catalog/pg_partitioned_table_d.h"

/* ----------------
 *		pg_partitioned_table definition.  cpp turns this into
 *		typedef struct FormData_pg_partitioned_table
 * ----------------
 */
CATALOG(pg_partitioned_table,3350,PartitionedRelationId)
{
	Oid			partrelid BKI_LOOKUP(pg_class); /*  分区表 oid  */
	char		partstrat;		/*  分区策略  */
	int16		partnatts;		/*  分区键列数  */
	Oid			partdefid BKI_LOOKUP_OPT(pg_class); /*  默认分区 oid;
													 * 若无则为 0  */

	/*
	 * variable-length fields start here, but we allow direct access to
	 * partattrs via the C struct.  That's because the first variable-length
	 * field of a heap tuple can be reliably accessed using its C struct
	 * offset, as previous fields are all non-nullable fixed-length fields.
	 */
	int2vector	partattrs BKI_FORCE_NOT_NULL;	/*  数组每项为
												 * 某分区键列的
												 * 属性号,或 0
												 * 若该列实际是
												 * 表达式  */

#ifdef CATALOG_VARLEN
	oidvector	partclass BKI_LOOKUP(pg_opclass) BKI_FORCE_NOT_NULL;	/* operator class to
																		 * compare keys */
	oidvector	partcollation BKI_LOOKUP_OPT(pg_collation) BKI_FORCE_NOT_NULL;	/* user-specified
																				 * collation for keys */
	pg_node_tree partexprs;		/*  分区键中的表达式列表;
								 * partattrs[] 中每个零项对应一项  */
#endif
} FormData_pg_partitioned_table;

/* ----------------
 *		Form_pg_partitioned_table corresponds to a pointer to a tuple with
 *		the format of pg_partitioned_table relation.
 * ----------------
 */
typedef FormData_pg_partitioned_table *Form_pg_partitioned_table;

DECLARE_TOAST(pg_partitioned_table, 4165, 4166);

DECLARE_UNIQUE_INDEX_PKEY(pg_partitioned_table_partrelid_index, 3351, PartitionedRelidIndexId, on pg_partitioned_table using btree(partrelid oid_ops));

/*  partattrs 可含零(InvalidAttrNumber)表示表达式  */
DECLARE_ARRAY_FOREIGN_KEY_OPT((partrelid, partattrs), pg_attribute, (attrelid, attnum));

#endif							/* PG_PARTITIONED_TABLE_H */
