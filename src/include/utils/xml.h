/*-------------------------------------------------------------------------
 *
 * xml.h
 *	  XML 数据类型支持声明。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/xml.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef XML_H
#define XML_H

#include "executor/tablefunc.h"
#include "fmgr.h"
#include "nodes/execnodes.h"
#include "nodes/primnodes.h"

typedef struct varlena xmltype;

typedef enum
{
	XML_STANDALONE_YES,
	XML_STANDALONE_NO,
	XML_STANDALONE_NO_VALUE,
	XML_STANDALONE_OMITTED
}			XmlStandaloneType;

typedef enum
{
	XMLBINARY_BASE64,
	XMLBINARY_HEX
}			XmlBinaryType;

typedef enum
{
	PG_XML_STRICTNESS_LEGACY,	/*  忽略错误，除非函数结果
								 * 表明出错条件  */
	PG_XML_STRICTNESS_WELLFORMED,	/*  忽略非解析器消息  */
	PG_XML_STRICTNESS_ALL		/*  报告全部通知/警告/错误  */
} PgXmlStrictness;

/*  PgXmlErrorContext 为 xml.c 私有  */
typedef struct PgXmlErrorContext PgXmlErrorContext;

static inline xmltype *
DatumGetXmlP(Datum X)
{
	return (xmltype *) PG_DETOAST_DATUM(X);
}

static inline Datum
XmlPGetDatum(const xmltype *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_XML_P(n)	DatumGetXmlP(PG_GETARG_DATUM(n))
#define PG_RETURN_XML_P(x)	PG_RETURN_POINTER(x)

extern void pg_xml_init_library(void);
extern PgXmlErrorContext *pg_xml_init(PgXmlStrictness strictness);
extern void pg_xml_done(PgXmlErrorContext *errcxt, bool isError);
extern bool pg_xml_error_occurred(PgXmlErrorContext *errcxt);
extern void xml_ereport(PgXmlErrorContext *errcxt, int level, int sqlcode,
						const char *msg);

extern xmltype *xmlconcat(List *args);
extern xmltype *xmlelement(XmlExpr *xexpr,
						   Datum *named_argvalue, bool *named_argnull,
						   Datum *argvalue, bool *argnull);
extern xmltype *xmlparse(text *data, XmlOptionType xmloption_arg, bool preserve_whitespace);
extern xmltype *xmlpi(const char *target, text *arg, bool arg_is_null, bool *result_is_null);
extern xmltype *xmlroot(xmltype *data, text *version, int standalone);
extern bool xml_is_document(xmltype *arg);
extern text *xmltotext_with_options(xmltype *data, XmlOptionType xmloption_arg,
									bool indent);
extern char *escape_xml(const char *str);

extern char *map_sql_identifier_to_xml_name(const char *ident, bool fully_escaped, bool escape_period);
extern char *map_xml_name_to_sql_identifier(const char *name);
extern char *map_sql_value_to_xml_value(Datum value, Oid type, bool xml_escape_strings);

extern PGDLLIMPORT int xmlbinary;	/*  XmlBinaryType，guc 枚举用 int  */

extern PGDLLIMPORT int xmloption;	/*  XmlOptionType，guc 枚举用 int  */

extern PGDLLIMPORT const TableFuncRoutine XmlTableRoutine;

#endif							/* XML_H */
