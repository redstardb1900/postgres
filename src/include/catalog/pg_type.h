/*-------------------------------------------------------------------------
 *
 * pg_type.h
 *	  definition of the "type" system catalog (pg_type)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_type.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TYPE_H
#define PG_TYPE_H

#include "catalog/genbki.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodes.h"

/* ----------------
 *		pg_type definition.  cpp turns this into
 *		typedef struct FormData_pg_type
 *
 *		Some of the values in a pg_type instance are copied into
 *		pg_attribute instances.  Some parts of Postgres use the pg_type copy,
 *		while others use the pg_attribute copy, so they must match.
 *		See struct FormData_pg_attribute for details.
 * ----------------
 */
CATALOG(pg_type,1247,TypeRelationId) BKI_BOOTSTRAP BKI_ROWTYPE_OID(71,TypeRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	Oid			oid;			/* oid */

	/* 类型名 */
	NameData	typname;

	/*  含本类型的命名空间 OID  */
	Oid			typnamespace BKI_DEFAULT(pg_catalog) BKI_LOOKUP(pg_namespace);

	/* type owner */
	Oid			typowner BKI_DEFAULT(POSTGRES) BKI_LOOKUP(pg_authid);

	/*
	 * For a fixed-size type, typlen is the number of bytes we use to
	 * represent a value of this type, e.g. 4 for an int4.  But for a
	 * variable-length type, typlen is negative.  We use -1 to indicate a
	 * "varlena" type (one that has a length word), -2 to indicate a
	 * null-terminated C string.
	 */
	int16		typlen BKI_ARRAY_DEFAULT(-1);

	/*
	 * typbyval determines whether internal Postgres routines pass a value of
	 * this type by value or by reference.  typbyval had better be false if
	 * the length is not 1, 2, or 4 (or 8 on 8-byte-Datum machines).
	 * Variable-length types are always passed by reference. Note that
	 * typbyval can be false even if the length would allow pass-by-value; for
	 * example, type macaddr8 is pass-by-ref even when Datum is 8 bytes.
	 */
	bool		typbyval BKI_ARRAY_DEFAULT(f);

	/*
	 * typtype is 'b' for a base type, 'c' for a composite type (e.g., a
	 * table's rowtype), 'd' for a domain, 'e' for an enum type, 'p' for a
	 * pseudo-type, or 'r' for a range type. (Use the TYPTYPE macros below.)
	 *
	 * If typtype is 'c', typrelid is the OID of the class' entry in pg_class.
	 */
	char		typtype BKI_DEFAULT(b) BKI_ARRAY_DEFAULT(b);

	/*
	 * typcategory and typispreferred help the parser distinguish preferred
	 * and non-preferred coercions.  The category can be any single ASCII
	 * character (but not \0).  The categories used for built-in types are
	 * identified by the TYPCATEGORY macros below.
	 */

	/*  任意类型分类  */
	char		typcategory BKI_ARRAY_DEFAULT(A);

	/* 在其类别内是否为"首选"类型? */
	bool		typispreferred BKI_DEFAULT(f) BKI_ARRAY_DEFAULT(f);

	/*
	 * If typisdefined is false, the entry is only a placeholder (forward
	 * reference).  We know the type's name and owner, but not yet anything
	 * else about it.
	 */
	bool		typisdefined BKI_DEFAULT(t);

	/*  本类型数组的分隔符  */
	char		typdelim BKI_DEFAULT(',');

	/*  复合类型则关联 pg_class OID,否则 0  */
	Oid			typrelid BKI_DEFAULT(0) BKI_ARRAY_DEFAULT(0) BKI_LOOKUP_OPT(pg_class);

	/*
	 * Type-specific subscripting handler.  If typsubscript is 0, it means
	 * that this type doesn't support subscripting.  Note that various parts
	 * of the system deem types to be "true" array types only if their
	 * typsubscript is array_subscript_handler.
	 */
	regproc		typsubscript BKI_DEFAULT(-) BKI_ARRAY_DEFAULT(array_subscript_handler) BKI_LOOKUP_OPT(pg_proc);

	/*
	 * If typelem is not 0 then it identifies another row in pg_type, defining
	 * the type yielded by subscripting.  This should be 0 if typsubscript is
	 * 0.  However, it can be 0 when typsubscript isn't 0, if the handler
	 * doesn't need typelem to determine the subscripting result type.  Note
	 * that a typelem dependency is considered to imply physical containment
	 * of the element type in this type; so DDL changes on the element type
	 * might be restricted by the presence of this type.
	 */
	Oid			typelem BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_type);

	/*
	 * If there is a "true" array type having this type as element type,
	 * typarray links to it.  Zero if no associated "true" array type.
	 */
	Oid			typarray BKI_DEFAULT(0) BKI_ARRAY_DEFAULT(0) BKI_LOOKUP_OPT(pg_type);

	/*
	 * I/O conversion procedures for the datatype.
	 */

	/*  文本格式(必需)  */
	regproc		typinput BKI_ARRAY_DEFAULT(array_in) BKI_LOOKUP(pg_proc);
	regproc		typoutput BKI_ARRAY_DEFAULT(array_out) BKI_LOOKUP(pg_proc);

	/* 二进制格式(可选) */
	regproc		typreceive BKI_ARRAY_DEFAULT(array_recv) BKI_LOOKUP_OPT(pg_proc);
	regproc		typsend BKI_ARRAY_DEFAULT(array_send) BKI_LOOKUP_OPT(pg_proc);

	/*
	 * 可选类型修饰符的 I/O 函数。
	 */
	regproc		typmodin BKI_DEFAULT(-) BKI_LOOKUP_OPT(pg_proc);
	regproc		typmodout BKI_DEFAULT(-) BKI_LOOKUP_OPT(pg_proc);

	/*
	 * 数据类型的自定义 ANALYZE 过程(0 选默认)。
	 */
	regproc		typanalyze BKI_DEFAULT(-) BKI_ARRAY_DEFAULT(array_typanalyze) BKI_LOOKUP_OPT(pg_proc);

	/* ----------------
	 * typalign is the alignment required when storing a value of this
	 * type.  It applies to storage on disk as well as most
	 * representations of the value inside Postgres.  When multiple values
	 * are stored consecutively, such as in the representation of a
	 * complete row on disk, padding is inserted before a datum of this
	 * type so that it begins on the specified boundary.  The alignment
	 * reference is the beginning of the first datum in the sequence.
	 *
	 * 'c' = CHAR alignment, ie no alignment needed.
	 * 's' = SHORT alignment (2 bytes on most machines).
	 * 'i' = INT alignment (4 bytes on most machines).
	 * 'd' = DOUBLE alignment (8 bytes on many machines, but by no means all).
	 * (Use the TYPALIGN macros below for these.)
	 *
	 * See include/access/tupmacs.h for the macros that compute these
	 * alignment requirements.  Note also that we allow the nominal alignment
	 * to be violated when storing "packed" varlenas; the TOAST mechanism
	 * takes care of hiding that from most code.
	 *
	 * NOTE: for types used in system tables, it is critical that the
	 * size and alignment defined in pg_type agree with the way that the
	 * compiler will lay out the field in a struct representing a table row.
	 * ----------------
	 */
	char		typalign;

	/* ----------------
	 * typstorage tells if the type is prepared for toasting and what
	 * the default strategy for attributes of this type should be.
	 *
	 * 'p' PLAIN	  type not prepared for toasting
	 * 'e' EXTERNAL   external storage possible, don't try to compress
	 * 'x' EXTENDED   try to compress and store external if required
	 * 'm' MAIN		  like 'x' but try to keep in main tuple
	 * (Use the TYPSTORAGE macros below for these.)
	 *
	 * Note that 'm' fields can also be moved out to secondary storage,
	 * but only as a last resort ('e' and 'x' fields are moved first).
	 * ----------------
	 */
	char		typstorage BKI_DEFAULT(p) BKI_ARRAY_DEFAULT(x);

	/*
	 * This flag represents a "NOT NULL" constraint against this datatype.
	 *
	 * If true, the attnotnull column for a corresponding table column using
	 * this datatype will always enforce the NOT NULL constraint.
	 *
	 * Used primarily for domain types.
	 */
	bool		typnotnull BKI_DEFAULT(f);

	/*
	 * Domains use typbasetype to show the base (or domain) type that the
	 * domain is based on.  Zero if the type is not a domain.
	 */
	Oid			typbasetype BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_type);

	/*
	 * Domains use typtypmod to record the typmod to be applied to their base
	 * type (-1 if base type does not use a typmod).  -1 if this type is not a
	 * domain.
	 */
	int32		typtypmod BKI_DEFAULT(-1);

	/*
	 * typndims is the declared number of dimensions for an array domain type
	 * (i.e., typbasetype is an array type).  Otherwise zero.
	 */
	int32		typndims BKI_DEFAULT(0);

	/*
	 * Collation: 0 if type cannot use collations, nonzero (typically
	 * DEFAULT_COLLATION_OID) for collatable base types, possibly some other
	 * OID for domains over collatable types
	 */
	Oid			typcollation BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_collation);

#ifdef CATALOG_VARLEN			/*  变长字段自此开始  */

	/*
	 * If typdefaultbin is not NULL, it is the nodeToString representation of
	 * a default expression for the type.  Currently this is only used for
	 * domains.
	 */
	pg_node_tree typdefaultbin BKI_DEFAULT(_null_) BKI_ARRAY_DEFAULT(_null_);

	/*
	 * typdefault is NULL if the type has no associated default value. If
	 * typdefaultbin is not NULL, typdefault must contain a human-readable
	 * version of the default expression represented by typdefaultbin. If
	 * typdefaultbin is NULL and typdefault is not, then typdefault is the
	 * external representation of the type's default value, which may be fed
	 * to the type's input converter to produce a constant.
	 */
	text		typdefault BKI_DEFAULT(_null_) BKI_ARRAY_DEFAULT(_null_);

	/*
	 * 访问权限
	 */
	aclitem		typacl[1] BKI_DEFAULT(_null_);
#endif
} FormData_pg_type;

/* ----------------
 *		Form_pg_type corresponds to a pointer to a row with
 *		the format of pg_type relation.
 * ----------------
 */
typedef FormData_pg_type *Form_pg_type;

DECLARE_TOAST(pg_type, 4171, 4172);

DECLARE_UNIQUE_INDEX_PKEY(pg_type_oid_index, 2703, TypeOidIndexId, on pg_type using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_type_typname_nsp_index, 2704, TypeNameNspIndexId, on pg_type using btree(typname name_ops, typnamespace oid_ops));

#ifdef EXPOSE_TO_CLIENT_CODE

/*
 * 简陋枚举类型列取值的宏
 */
#define  TYPTYPE_BASE		'b' /*  基类型(普通标量类型)  */
#define  TYPTYPE_COMPOSITE	'c' /*  复合(如表的行类型)  */
#define  TYPTYPE_DOMAIN		'd' /*  另一类型上的域  */
#define  TYPTYPE_ENUM		'e' /*  枚举类型  */
#define  TYPTYPE_MULTIRANGE	'm' /* 多范围类型 */
#define  TYPTYPE_PSEUDO		'p' /*  伪类型  */
#define  TYPTYPE_RANGE		'r' /* range type */

#define  TYPCATEGORY_INVALID	'\0'	/*  非允许的类别  */
#define  TYPCATEGORY_ARRAY		'A'
#define  TYPCATEGORY_BOOLEAN	'B'
#define  TYPCATEGORY_COMPOSITE	'C'
#define  TYPCATEGORY_DATETIME	'D'
#define  TYPCATEGORY_ENUM		'E'
#define  TYPCATEGORY_GEOMETRIC	'G'
#define  TYPCATEGORY_NETWORK	'I' /*  如 INET  */
#define  TYPCATEGORY_NUMERIC	'N'
#define  TYPCATEGORY_PSEUDOTYPE 'P'
#define  TYPCATEGORY_RANGE		'R'
#define  TYPCATEGORY_STRING		'S'
#define  TYPCATEGORY_TIMESPAN	'T'
#define  TYPCATEGORY_USER		'U'
#define  TYPCATEGORY_BITSTRING	'V' /* 呃..."varbit"? */
#define  TYPCATEGORY_UNKNOWN	'X'
#define  TYPCATEGORY_INTERNAL	'Z'

#define  TYPALIGN_CHAR			'c' /*  char 对齐(即不对齐)  */
#define  TYPALIGN_SHORT			's' /*  short 对齐(通常 2 字节)  */
#define  TYPALIGN_INT			'i' /*  int 对齐(通常 4 字节)  */
#define  TYPALIGN_DOUBLE		'd' /*  double 对齐(常为 8 字节)  */

#define  TYPSTORAGE_PLAIN		'p' /*  类型未准备好 toast  */
#define  TYPSTORAGE_EXTERNAL	'e' /*  可 toast,勿尝试压缩  */
#define  TYPSTORAGE_EXTENDED	'x' /* 完全可 toast */
#define  TYPSTORAGE_MAIN		'm' /* 类似 'x' 但尽量内联存储 */

/* Is a type OID a polymorphic pseudotype?	(Beware of multiple evaluation) */
#define IsPolymorphicType(typid)  \
	(IsPolymorphicTypeFamily1(typid) || \
	 IsPolymorphicTypeFamily2(typid))

/* Code not part of polymorphic type resolution should not use these macros: */
#define IsPolymorphicTypeFamily1(typid)  \
	((typid) == ANYELEMENTOID || \
	 (typid) == ANYARRAYOID || \
	 (typid) == ANYNONARRAYOID || \
	 (typid) == ANYENUMOID || \
	 (typid) == ANYRANGEOID || \
	 (typid) == ANYMULTIRANGEOID)

#define IsPolymorphicTypeFamily2(typid)  \
	((typid) == ANYCOMPATIBLEOID || \
	 (typid) == ANYCOMPATIBLEARRAYOID || \
	 (typid) == ANYCOMPATIBLENONARRAYOID || \
	 (typid) == ANYCOMPATIBLERANGEOID || \
	 (typid) == ANYCOMPATIBLEMULTIRANGEOID)

/* Is this a "true" array type?  (Requires fmgroids.h) */
#define IsTrueArrayType(typeForm)  \
	(OidIsValid((typeForm)->typelem) && \
	 (typeForm)->typsubscript == F_ARRAY_SUBSCRIPT_HANDLER)

/*
 * pg_type OID 宏古代拼写的向后兼容。
 * 新代码勿用这些名称。
 */
#define CASHOID	MONEYOID
#define LSNOID	PG_LSNOID

#endif							/* EXPOSE_TO_CLIENT_CODE */


extern ObjectAddress TypeShellMake(const char *typeName,
								   Oid typeNamespace,
								   Oid ownerId);

extern ObjectAddress TypeCreate(Oid newTypeOid,
								const char *typeName,
								Oid typeNamespace,
								Oid relationOid,
								char relationKind,
								Oid ownerId,
								int16 internalSize,
								char typeType,
								char typeCategory,
								bool typePreferred,
								char typDelim,
								Oid inputProcedure,
								Oid outputProcedure,
								Oid receiveProcedure,
								Oid sendProcedure,
								Oid typmodinProcedure,
								Oid typmodoutProcedure,
								Oid analyzeProcedure,
								Oid subscriptProcedure,
								Oid elementType,
								bool isImplicitArray,
								Oid arrayType,
								Oid baseType,
								const char *defaultTypeValue,
								char *defaultTypeBin,
								bool passedByValue,
								char alignment,
								char storage,
								int32 typeMod,
								int32 typNDims,
								bool typeNotNull,
								Oid typeCollation);

extern void GenerateTypeDependencies(HeapTuple typeTuple,
									 Relation typeCatalog,
									 Node *defaultExpr,
									 void *typacl,
									 char relationKind, /* only for relation
														 * rowtypes */
									 bool isImplicitArray,
									 bool isDependentType,
									 bool makeExtensionDep,
									 bool rebuild);

extern void RenameTypeInternal(Oid typeOid, const char *newTypeName,
							   Oid typeNamespace);

extern char *makeArrayTypeName(const char *typeName, Oid typeNamespace);

extern bool moveArrayTypeName(Oid typeOid, const char *typeName,
							  Oid typeNamespace);

extern char *makeMultirangeTypeName(const char *rangeTypeName,
									Oid typeNamespace);

#endif							/* PG_TYPE_H */
