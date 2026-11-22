/*-------------------------------------------------------------------------
 *
 * genbki.h
 *	  Required include file for all POSTGRES catalog header files
 *
 * genbki.h defines CATALOG(), BKI_BOOTSTRAP and related macros
 * so that the catalog header files can be read by the C compiler.
 * (These same words are recognized by genbki.pl to build the BKI
 * bootstrap file from these header files.)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/genbki.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GENBKI_H
#define GENBKI_H

/*  引入系统目录的结构定义  */
#define CATALOG(name,oid,oidmacro)	typedef struct CppConcat(FormData_,name)

/*  可出现在 CATALOG 之后的选项(同行)  */
#define BKI_BOOTSTRAP
#define BKI_SHARED_RELATION
#define BKI_ROWTYPE_OID(oid,oidmacro)
#define BKI_SCHEMA_MACRO

/*  可出现在属性之后的选项(同行)  */
#define BKI_FORCE_NULL
#define BKI_FORCE_NOT_NULL
/*  指定目录字段的默认值  */
#define BKI_DEFAULT(value)
/*  指定自动生成数组类型的默认值  */
#define BKI_ARRAY_DEFAULT(value)
/*
 * 表示该属性含引用具名目录的 OIDs;
 * 可用于 oid、regproc、oid[] 或 oidvector 列。
 * genbki.pl 借此知如何在初始数据(若有)中做
 * 名称查找,并供回归测试有效性检查。
 * _OPT 后缀表示值可为零而非
 * 有效 OID 引用。
 */
#define BKI_LOOKUP(catalog)
#define BKI_LOOKUP_OPT(catalog)

/*
 * These lines are processed by genbki.pl to create the statements
 * the bootstrap parser will turn into BootstrapToastTable commands.
 * Each line specifies the system catalog that needs a toast table,
 * the OID to assign to the toast table, and the OID to assign to the
 * toast table's index.  The reason we hard-wire these OIDs is that we
 * need stable OIDs for shared relations, and that includes toast tables
 * of shared relations.
 *
 * The DECLARE_TOAST_WITH_MACRO variant is used when C macros are needed
 * for the toast table/index OIDs (usually only for shared catalogs).
 *
 * The macro definitions are just to keep the C compiler from spitting up.
 */
#define DECLARE_TOAST(name,toastoid,indexoid) extern int no_such_variable
#define DECLARE_TOAST_WITH_MACRO(name,toastoid,indexoid,toastoidmacro,indexoidmacro) extern int no_such_variable

/*
 * These lines are processed by genbki.pl to create the statements
 * the bootstrap parser will turn into DefineIndex calls.
 *
 * The keyword is DECLARE_INDEX or DECLARE_UNIQUE_INDEX or
 * DECLARE_UNIQUE_INDEX_PKEY.  ("PKEY" marks the index as being the catalog's
 * primary key; currently this is only cosmetically different from a regular
 * unique index.  By convention, we usually make a catalog's OID column its
 * pkey, if it has one.)
 *
 * The first two arguments are the index's name and OID.  The third argument
 * is the name of a #define to generate for its OID.  References to the index
 * in the C code should always use these #defines, not the actual index name
 * (much less the numeric OID).  The rest is much like a standard 'create
 * index' SQL command.
 *
 * The macro definitions are just to keep the C compiler from spitting up.
 */
#define DECLARE_INDEX(name,oid,oidmacro,decl) extern int no_such_variable
#define DECLARE_UNIQUE_INDEX(name,oid,oidmacro,decl) extern int no_such_variable
#define DECLARE_UNIQUE_INDEX_PKEY(name,oid,oidmacro,decl) extern int no_such_variable

/*
 * These lines inform genbki.pl about manually-assigned OIDs that do not
 * correspond to any entry in the catalog *.dat files, but should be subject
 * to uniqueness verification and renumber_oids.pl renumbering.  A C macro
 * to #define the given name is emitted into the corresponding *_d.h file.
 */
#define DECLARE_OID_DEFINING_MACRO(name,oid) extern int no_such_variable

/*
 * These lines are processed by genbki.pl to create a table for use
 * by the pg_get_catalog_foreign_keys() function.  We do not have any
 * mechanism that actually enforces foreign-key relationships in the
 * system catalogs, but it is still useful to record the intended
 * relationships in a machine-readable form.
 *
 * The keyword is DECLARE_FOREIGN_KEY[_OPT] or DECLARE_ARRAY_FOREIGN_KEY[_OPT].
 * The first argument is a parenthesized list of the referencing columns;
 * the second, the name of the referenced table; the third, a parenthesized
 * list of the referenced columns.  Use of the ARRAY macros means that the
 * last referencing column is an array, each of whose elements is supposed
 * to match some entry in the last referenced column.  Use of the OPT suffix
 * indicates that the referencing column(s) can be zero instead of a valid
 * reference.
 *
 * Columns that are marked with a BKI_LOOKUP rule do not need an explicit
 * DECLARE_FOREIGN_KEY macro, as genbki.pl can infer the FK relationship
 * from that.  Thus, these macros are only needed in special cases.
 *
 * The macro definitions are just to keep the C compiler from spitting up.
 */
#define DECLARE_FOREIGN_KEY(cols,reftbl,refcols) extern int no_such_variable
#define DECLARE_FOREIGN_KEY_OPT(cols,reftbl,refcols) extern int no_such_variable
#define DECLARE_ARRAY_FOREIGN_KEY(cols,reftbl,refcols) extern int no_such_variable
#define DECLARE_ARRAY_FOREIGN_KEY_OPT(cols,reftbl,refcols) extern int no_such_variable

/*  以下永不定义;仅供文档。  */

/*
 * 变长目录字段(除可能的首个非空者外)不应在 C
 * 结构中可见,故用未定义符号的 #ifdef 隐藏。另见
 * bootstrap.c 中 BOOTCOL_NULL_AUTO 代码了解
 * 处理方式。
 */
#undef CATALOG_VARLEN

/*
 * 某些目录头中有需对客户端可见的代码,
 * 但不想因头中其他代码的安全问题让客户
 * 端包含完整头。为此,将对客户端可见的代码
 * 用 "#ifdef EXPOSE_TO_CLIENT_CODE" 包围。这指示
 * genbki.pl 在生成对应 "_d" 头时复制该段,该头
 * 可被客户端与后端代码共同包含。
 */
#undef EXPOSE_TO_CLIENT_CODE

#endif							/* GENBKI_H */
