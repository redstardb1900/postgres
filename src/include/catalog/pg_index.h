/*-------------------------------------------------------------------------
 *
 * pg_index.h
 *	  definition of the "index" system catalog (pg_index)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_index.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_INDEX_H
#define PG_INDEX_H

#include "catalog/genbki.h"
#include "catalog/pg_index_d.h"

/* ----------------
 *		pg_index definition.  cpp turns this into
 *		typedef struct FormData_pg_index.
 * ----------------
 */
CATALOG(pg_index,2610,IndexRelationId) BKI_SCHEMA_MACRO
{
	Oid			indexrelid BKI_LOOKUP(pg_class);	/*  索引 OID  */
	Oid			indrelid BKI_LOOKUP(pg_class);	/* OID of the relation it
												 * indexes */
	int16		indnatts;		/*  索引中总列数  */
	int16		indnkeyatts;	/*  索引中键列数  */
	bool		indisunique;	/*  是否唯一索引?  */
	bool		indnullsnotdistinct;	/* 唯一索引中的 null 处理 */
	bool		indisprimary;	/*  是否为主键索引?  */
	bool		indisexclusion; /*  是否为排他约束索引?  */
	bool		indimmediate;	/*  唯一性是否立即强制?  */
	bool		indisclustered; /* is this the index last clustered by? */
	bool		indisvalid;		/*  本索引是否可供查询使用?  */
	bool		indcheckxmin;	/*  是否须等待 xmin 变旧?  */
	bool		indisready;		/*  本索引是否已可插入?  */
	bool		indislive;		/*  本索引是否仍存活?  */
	bool		indisreplident; /*  本索引是否为复制标识?  */

	/*  变长字段自此开始,但允许直接访问 indkey  */
	int2vector	indkey BKI_FORCE_NOT_NULL;	/* column numbers of indexed cols,
											 * or 0 */

#ifdef CATALOG_VARLEN
	oidvector	indcollation BKI_LOOKUP_OPT(pg_collation) BKI_FORCE_NOT_NULL;	/* 排序规则标识符 */
	oidvector	indclass BKI_LOOKUP(pg_opclass) BKI_FORCE_NOT_NULL; /* 算子类标识符 */
	int2vector	indoption BKI_FORCE_NOT_NULL;	/* per-column flags
												 * (AM-specific meanings) */
	pg_node_tree indexprs;		/* expression trees for index attributes that
								 * are not simple column references; one for
								 * each zero entry in indkey[] */
	pg_node_tree indpred;		/* 若为部分索引则为谓词表达式树;
								 * 否则为 NULL  */
#endif
} FormData_pg_index;

/* ----------------
 *		Form_pg_index corresponds to a pointer to a tuple with
 *		the format of pg_index relation.
 * ----------------
 */
typedef FormData_pg_index *Form_pg_index;

DECLARE_INDEX(pg_index_indrelid_index, 2678, IndexIndrelidIndexId, on pg_index using btree(indrelid oid_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_index_indexrelid_index, 2679, IndexRelidIndexId, on pg_index using btree(indexrelid oid_ops));

/*  indkey 可含零(InvalidAttrNumber)表示表达式  */
DECLARE_ARRAY_FOREIGN_KEY_OPT((indrelid, indkey), pg_attribute, (attrelid, attnum));

#ifdef EXPOSE_TO_CLIENT_CODE

/*
 * Index AMs that support ordered scans must support these two indoption
 * bits.  Otherwise, the content of the per-column indoption fields is
 * open for future definition.
 */
#define INDOPTION_DESC			0x0001	/*  值按逆序  */
#define INDOPTION_NULLS_FIRST	0x0002	/*  NULL 在前而非在后  */

#endif							/* EXPOSE_TO_CLIENT_CODE */

#endif							/* PG_INDEX_H */
