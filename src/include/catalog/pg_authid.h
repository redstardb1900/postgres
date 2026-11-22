/*-------------------------------------------------------------------------
 *
 * pg_authid.h
 *	  definition of the "authorization identifier" system catalog (pg_authid)
 *
 *	  pg_shadow and pg_group are now publicly accessible views on pg_authid.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_authid.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AUTHID_H
#define PG_AUTHID_H

#include "catalog/genbki.h"
#include "catalog/pg_authid_d.h"

/* ----------------
 *		pg_authid definition.  cpp turns this into
 *		typedef struct FormData_pg_authid
 * ----------------
 */
CATALOG(pg_authid,1260,AuthIdRelationId) BKI_SHARED_RELATION BKI_ROWTYPE_OID(2842,AuthIdRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	Oid			oid;			/* oid */
	NameData	rolname;		/*  角色名  */
	bool		rolsuper;		/* read this field via superuser() only! */
	bool		rolinherit;		/*  是否从其他角色继承特权?  */
	bool		rolcreaterole;	/* allowed to create more roles? */
	bool		rolcreatedb;	/* 是否允许创建数据库? */
	bool		rolcanlogin;	/* allowed to log in as session user? */
	bool		rolreplication; /*  用于流复制的角色  */
	bool		rolbypassrls;	/*  是否绕过行级安全?  */
	int32		rolconnlimit;	/* max connections allowed (-1=no limit) */

	/*  其余字段可为 null;用 heap_getattr 读取!  */
#ifdef CATALOG_VARLEN			/*  变长字段自此开始  */
	text		rolpassword;	/*  口令(若有)  */
	timestamptz rolvaliduntil;	/*  口令过期时间(若有)  */
#endif
} FormData_pg_authid;

/* ----------------
 *		Form_pg_authid corresponds to a pointer to a tuple with
 *		the format of pg_authid relation.
 * ----------------
 */
typedef FormData_pg_authid *Form_pg_authid;

DECLARE_TOAST_WITH_MACRO(pg_authid, 4175, 4176, PgAuthidToastTable, PgAuthidToastIndex);

DECLARE_UNIQUE_INDEX(pg_authid_rolname_index, 2676, AuthIdRolnameIndexId, on pg_authid using btree(rolname name_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_authid_oid_index, 2677, AuthIdOidIndexId, on pg_authid using btree(oid oid_ops));

#endif							/* PG_AUTHID_H */
