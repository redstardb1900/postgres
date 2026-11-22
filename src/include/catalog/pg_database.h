/*-------------------------------------------------------------------------
 *
 * pg_database.h
 *	  definition of the "database" system catalog (pg_database)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_database.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DATABASE_H
#define PG_DATABASE_H

#include "catalog/genbki.h"
#include "catalog/pg_database_d.h"

/* ----------------
 *		pg_database definition.  cpp turns this into
 *		typedef struct FormData_pg_database
 * ----------------
 */
CATALOG(pg_database,1262,DatabaseRelationId) BKI_SHARED_RELATION BKI_ROWTYPE_OID(1248,DatabaseRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	/* oid */
	Oid			oid;

	/* 数据库名 */
	NameData	datname;

	/* owner of database */
	Oid			datdba BKI_DEFAULT(POSTGRES) BKI_LOOKUP(pg_authid);

	/*  字符编码  */
	int32		encoding;

	/* 区域提供者,见 pg_collation.collprovider */
	char		datlocprovider;

	/* 可否用作 CREATE DATABASE 模板? */
	bool		datistemplate;

	/*  是否允许新连接?  */
	bool		datallowconn;

	/*
	 * 允许的最大连接数。负值有特殊含义,见
	 * 下方 DATCONNLIMIT_* 定义.
	 */
	int32		datconnlimit;

	/* all Xids < this are frozen in this DB */
	TransactionId datfrozenxid;

	/* all multixacts in the DB are >= this */
	TransactionId datminmxid;

	/*  本库的默认表空间  */
	Oid			dattablespace BKI_LOOKUP(pg_tablespace);

#ifdef CATALOG_VARLEN			/*  变长字段自此开始  */
	/* LC_COLLATE 设置 */
	text		datcollate BKI_FORCE_NOT_NULL;

	/* LC_CTYPE 设置 */
	text		datctype BKI_FORCE_NOT_NULL;

	/* ICU locale ID */
	text		daticulocale;

	/*  ICU 排序规则  */
	text		daticurules;

	/*  依赖提供者的排序规则数据版本  */
	text		datcollversion BKI_DEFAULT(_null_);

	/*  访问权限  */
	aclitem		datacl[1];
#endif
} FormData_pg_database;

/* ----------------
 *		Form_pg_database corresponds to a pointer to a tuple with
 *		the format of pg_database relation.
 * ----------------
 */
typedef FormData_pg_database *Form_pg_database;

DECLARE_TOAST_WITH_MACRO(pg_database, 4177, 4178, PgDatabaseToastTable, PgDatabaseToastIndex);

DECLARE_UNIQUE_INDEX(pg_database_datname_index, 2671, DatabaseNameIndexId, on pg_database using btree(datname name_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_database_oid_index, 2672, DatabaseOidIndexId, on pg_database using btree(oid oid_ops));

/*
 * pg_database.dat contains an entry for template1, but not for the template0
 * or postgres databases, because those are created later in initdb.
 * However, we still want to manually assign the OIDs for template0 and
 * postgres, so declare those here.
 */
DECLARE_OID_DEFINING_MACRO(Template0DbOid, 4);
DECLARE_OID_DEFINING_MACRO(PostgresDbOid, 5);

/*
 * pg_database.datconnlimit 特殊值。正常值 >= 0。
 */
#define		  DATCONNLIMIT_UNLIMITED	-1	/* no limit */

/*
 * A database is set to invalid partway through being dropped.  Using
 * datconnlimit=-2 for this purpose isn't particularly clean, but is
 * backpatchable.
 */
#define		  DATCONNLIMIT_INVALID_DB	-2

extern bool database_is_invalid_form(Form_pg_database datform);
extern bool database_is_invalid_oid(Oid dboid);

#endif							/* PG_DATABASE_H */
