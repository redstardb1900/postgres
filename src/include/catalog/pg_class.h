/*-------------------------------------------------------------------------
 *
 * pg_class.h
 *	  definition of the "relation" system catalog (pg_class)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_class.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CLASS_H
#define PG_CLASS_H

#include "catalog/genbki.h"
#include "catalog/pg_class_d.h"

/* ----------------
 *		pg_class definition.  cpp turns this into
 *		typedef struct FormData_pg_class
 *
 * Note that the BKI_DEFAULT values below are only used for rows describing
 * BKI_BOOTSTRAP catalogs, since only those rows appear in pg_class.dat.
 * ----------------
 */
CATALOG(pg_class,1259,RelationRelationId) BKI_BOOTSTRAP BKI_ROWTYPE_OID(83,RelationRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	/* oid */
	Oid			oid;

	/*  类名  */
	NameData	relname;

	/*  含本类的命名空间 OID  */
	Oid			relnamespace BKI_DEFAULT(pg_catalog) BKI_LOOKUP(pg_namespace);

	/*  关系隐式行类型在 pg_type 中的条目 OID(若有)  */
	Oid			reltype BKI_LOOKUP_OPT(pg_type);

	/*  底层复合类型在 pg_type 中的条目 OID(若有)  */
	Oid			reloftype BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_type);

	/* class owner */
	Oid			relowner BKI_DEFAULT(POSTGRES) BKI_LOOKUP(pg_authid);

	/*  访问方法;非表/索引则为 0  */
	Oid			relam BKI_DEFAULT(heap) BKI_LOOKUP_OPT(pg_am);

	/*  物理存储文件标识符  */
	/*  relfilenode == 0 表示"映射"关系,见 relmapper.c  */
	Oid			relfilenode BKI_DEFAULT(0);

	/*  关系表空间标识符(0 表示库默认)  */
	Oid			reltablespace BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_tablespace);

	/*  块数(不一定最新)  */
	int32		relpages BKI_DEFAULT(0);

	/*  元组数(不一定最新;-1="未知")  */
	float4		reltuples BKI_DEFAULT(-1);

	/*  全可见块数(不一定最新)  */
	int32		relallvisible BKI_DEFAULT(0);

	/*  toast 表 OID;无则为 0  */
	Oid			reltoastrelid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_class);

	/* T if has (or has had) any indexes */
	bool		relhasindex BKI_DEFAULT(f);

	/*  跨数据库共享则为 T  */
	bool		relisshared BKI_DEFAULT(f);

	/*  见下方 RELPERSISTENCE_xxx 常量  */
	char		relpersistence BKI_DEFAULT(p);

	/*  见下方 RELKIND_xxx 常量  */
	char		relkind BKI_DEFAULT(r);

	/*  用户属性数  */
	int16		relnatts BKI_DEFAULT(0);	/*  由 genbki.pl 填入  */

	/*
	 * Class pg_attribute must contain exactly "relnatts" user attributes
	 * (with attnums ranging from 1 to relnatts) for this class.  It may also
	 * contain entries with negative attnums for system attributes.
	 */

	/*  类的 CHECK 约束数  */
	int16		relchecks BKI_DEFAULT(0);

	/* 有(或曾有)任何规则 */
	bool		relhasrules BKI_DEFAULT(f);

	/* 有(或曾有)任何 TRIGGER */
	bool		relhastriggers BKI_DEFAULT(f);

	/*  有(或曾有)子表或索引  */
	bool		relhassubclass BKI_DEFAULT(f);

	/*  行级安全是否启用  */
	bool		relrowsecurity BKI_DEFAULT(f);

	/* 是否对所有者强制行级安全 */
	bool		relforcerowsecurity BKI_DEFAULT(f);

	/* matview currently holds query results */
	bool		relispopulated BKI_DEFAULT(t);

	/*  见 REPLICA_IDENTITY_xxx 常量  */
	char		relreplident BKI_DEFAULT(n);

	/*  关系是否为分区?  */
	bool		relispartition BKI_DEFAULT(f);

	/*  表改写期间链到原关系;否则为 0  */
	Oid			relrewrite BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_class);

	/* all Xids < this are frozen in this rel */
	TransactionId relfrozenxid BKI_DEFAULT(3);	/* FirstNormalTransactionId */

	/*  本关系中所有多事务均 >= 此值;实为 MultiXactId  */
	TransactionId relminmxid BKI_DEFAULT(1);	/* FirstMultiXactId */

#ifdef CATALOG_VARLEN			/*  变长字段自此开始  */
	/*  注:这些字段不在 relcache 条目的 rd_rel 中。  */
	/*  访问权限  */
	aclitem		relacl[1] BKI_DEFAULT(_null_);

	/*  访问方法专用选项  */
	text		reloptions[1] BKI_DEFAULT(_null_);

	/*  分区边界节点树  */
	pg_node_tree relpartbound BKI_DEFAULT(_null_);
#endif
} FormData_pg_class;

/*  pg_class 元组固定部分大小,不计变长字段  */
#define CLASS_TUPLE_SIZE \
	 (offsetof(FormData_pg_class,relminmxid) + sizeof(TransactionId))

/* ----------------
 *		Form_pg_class corresponds to a pointer to a tuple with
 *		the format of pg_class relation.
 * ----------------
 */
typedef FormData_pg_class *Form_pg_class;

DECLARE_UNIQUE_INDEX_PKEY(pg_class_oid_index, 2662, ClassOidIndexId, on pg_class using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_class_relname_nsp_index, 2663, ClassNameNspIndexId, on pg_class using btree(relname name_ops, relnamespace oid_ops));
DECLARE_INDEX(pg_class_tblspc_relfilenode_index, 3455, ClassTblspcRelfilenodeIndexId, on pg_class using btree(reltablespace oid_ops, relfilenode oid_ops));

#ifdef EXPOSE_TO_CLIENT_CODE

#define		  RELKIND_RELATION		  'r'	/*  普通表  */
#define		  RELKIND_INDEX			  'i'	/*  二级索引  */
#define		  RELKIND_SEQUENCE		  'S'	/*  序列对象  */
#define		  RELKIND_TOASTVALUE	  't'	/*  用于行外值  */
#define		  RELKIND_VIEW			  'v'	/* view */
#define		  RELKIND_MATVIEW		  'm'	/*  物化视图  */
#define		  RELKIND_COMPOSITE_TYPE  'c'	/*  复合类型  */
#define		  RELKIND_FOREIGN_TABLE   'f'	/*  外部表  */
#define		  RELKIND_PARTITIONED_TABLE 'p' /*  分区表  */
#define		  RELKIND_PARTITIONED_INDEX 'I' /*  分区索引  */

#define		  RELPERSISTENCE_PERMANENT	'p' /*  常规表  */
#define		  RELPERSISTENCE_UNLOGGED	'u' /* 不记日志的永久表 */
#define		  RELPERSISTENCE_TEMP		't' /*  临时表  */

/*  复制标识默认选择(主键或无)  */
#define		  REPLICA_IDENTITY_DEFAULT	'd'
/*  本关系不记录复制标识  */
#define		  REPLICA_IDENTITY_NOTHING	'n'
/*  所有列均作为复制标识记录  */
#define		  REPLICA_IDENTITY_FULL		'f'
/*
 * an explicitly chosen candidate key's columns are used as replica identity.
 * Note this will still be set if the index has been dropped; in that case it
 * has the same meaning as 'n'.
 */
#define		  REPLICA_IDENTITY_INDEX	'i'

/*
 * Relation kinds that have physical storage. These relations normally have
 * relfilenode set to non-zero, but it can also be zero if the relation is
 * mapped.
 */
#define RELKIND_HAS_STORAGE(relkind) \
	((relkind) == RELKIND_RELATION || \
	 (relkind) == RELKIND_INDEX || \
	 (relkind) == RELKIND_SEQUENCE || \
	 (relkind) == RELKIND_TOASTVALUE || \
	 (relkind) == RELKIND_MATVIEW)

#define RELKIND_HAS_PARTITIONS(relkind) \
	((relkind) == RELKIND_PARTITIONED_TABLE || \
	 (relkind) == RELKIND_PARTITIONED_INDEX)

/*
 * Relation kinds that support tablespaces: All relation kinds with storage
 * support tablespaces, except that we don't support moving sequences around
 * into different tablespaces.  Partitioned tables and indexes don't have
 * physical storage, but they have a tablespace settings so that their
 * children can inherit it.
 */
#define RELKIND_HAS_TABLESPACE(relkind) \
	((RELKIND_HAS_STORAGE(relkind) || RELKIND_HAS_PARTITIONS(relkind)) \
	 && (relkind) != RELKIND_SEQUENCE)

/*
 * Relation kinds with a table access method (rd_tableam).  Although sequences
 * use the heap table AM, they are enough of a special case in most uses that
 * they are not included here.
 */
#define RELKIND_HAS_TABLE_AM(relkind) \
	((relkind) == RELKIND_RELATION || \
	 (relkind) == RELKIND_TOASTVALUE || \
	 (relkind) == RELKIND_MATVIEW)

extern int	errdetail_relkind_not_supported(char relkind);

#endif							/* EXPOSE_TO_CLIENT_CODE */

#endif							/* PG_CLASS_H */
