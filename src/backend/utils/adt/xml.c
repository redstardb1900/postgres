/*-------------------------------------------------------------------------
 *
 * xml.c
 *	  XML 类型支持。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/utils/adt/xml.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * 通常，XML 类型支持仅在构建期间配置了
 * libxml 时才可用。即使未配置，类型与所有函数
 * 都仍可用，只是多数会失败。一方面，这避免了
 * 管理多种系统目录安装的工作。但它也有不错的
 * 效果，例如即使服务器未链接 libxml，也可以转储
 * 包含 XML 类型数据的数据库。因此，要确保即使
 * 其他都不行，xml_out() 也必须能
 * 工作。
 */

/*
 * 内存管理注意事项：
 *
 * libxml 有时会分配一些全局结构体，指望之后能够
 * 复用它们。这使得动态更改 xmlMemSetup 函数变
 * 得不切实际；那很可能导致对以 malloc() 分配的
 * 块调用 pfree()，或者相反。由于 libxml 可能会被
 * 可加载模块（例如 libperl）使用，唯一安全的选择
 * 要么是在 postmaster/backend 启动时更改这些函数，
 * 要么完全不更改。我们不想在可能永远用不到 libxml
 * 的会话中激活它，因此后一种选择是更可取的。但出
 * 于调试目的，把 libxml 的内存分配约束在特定的
 * palloc 上下文中进行会非常方便，因为在那里易于
 * 追踪。因此这里有一些代码，可在调试构建中启用，
 * 它能把 libxml 的分配重定向到 LibxmlContext 这一特
 * 殊的上下文。生产构建不建议开启，恐与外部模块
 * 不良交互。
 */
/* #define USE_LIBXMLCONTEXT */

#include "postgres.h"

#ifdef USE_LIBXML
#include <libxml/chvalid.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/tree.h>
#include <libxml/uri.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlsave.h>
#include <libxml/xmlversion.h>
#include <libxml/xmlwriter.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

/*
 * 过去我们用 configure 测试来检查 xmlStructuredErrorContext，
 * 但在 Windows 上行不通，故改用更粗糙的检验方法：
 * 查库的版本号。
 */
#if LIBXML_VERSION >= 20704
#define HAVE_XMLSTRUCTUREDERRORCONTEXT 1
#endif

/*
 * libxml2 2.12 决定在错误处理 API 中插入 "const"。
 */
#if LIBXML_VERSION >= 21200
#define PgXmlErrorPtr const xmlError *
#else
#define PgXmlErrorPtr xmlErrorPtr
#endif

#endif							/* USE_LIBXML */

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "executor/tablefunc.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/xml.h"


/* GUC 变量 */
int			xmlbinary = XMLBINARY_BASE64;
int			xmloption = XMLOPTION_CONTENT;

#ifdef USE_LIBXML

/* 用于标识 PgXmlErrorContext 的随机数 */
#define ERRCXT_MAGIC	68275028

struct PgXmlErrorContext
{
	int			magic;
	/* 传给 pg_xml_init 的严格性参数 */
	PgXmlStrictness strictness;
	/* 当前错误状态及累积的消息（如有） */
	bool		err_occurred;
	StringInfoData err_buf;
	/* 原 libxml 错误状态（pg_xml_init 保存） */
	xmlStructuredErrorFunc saved_errfunc;
	void	   *saved_errcxt;
	/* 原 libxml 实体处理器（pg_xml_init 保存） */
	xmlExternalEntityLoader saved_entityfunc;
};

static xmlParserInputPtr xmlPgEntityLoader(const char *URL, const char *ID,
										   xmlParserCtxtPtr ctxt);
static void xml_errsave(Node *escontext, PgXmlErrorContext *errcxt,
						int sqlcode, const char *msg);
static void xml_errorHandler(void *data, PgXmlErrorPtr error);
static int	errdetail_for_xml_code(int code);
static void chopStringInfoNewlines(StringInfo str);
static void appendStringInfoLineSeparator(StringInfo str);

#ifdef USE_LIBXMLCONTEXT

static MemoryContext LibxmlContext = NULL;

static void xml_memory_init(void);
static void *xml_palloc(size_t size);
static void *xml_repalloc(void *ptr, size_t size);
static void xml_pfree(void *ptr);
static char *xml_pstrdup(const char *string);
#endif							/* USE_LIBXMLCONTEXT */

static xmlChar *xml_text2xmlChar(text *in);
static int	parse_xml_decl(const xmlChar *str, size_t *lenp,
						   xmlChar **version, xmlChar **encoding, int *standalone);
static bool print_xml_decl(StringInfo buf, const xmlChar *version,
						   pg_enc encoding, int standalone);
static bool xml_doctype_in_content(const xmlChar *str);
static xmlDocPtr xml_parse(text *data, XmlOptionType xmloption_arg,
						   bool preserve_whitespace, int encoding,
						   XmlOptionType *parsed_xmloptiontype,
						   xmlNodePtr *parsed_nodes,
						   Node *escontext);
static text *xml_xmlnodetoxmltype(xmlNodePtr cur, PgXmlErrorContext *xmlerrcxt);
static int	xml_xpathobjtoxmlarray(xmlXPathObjectPtr xpathobj,
								   ArrayBuildState *astate,
								   PgXmlErrorContext *xmlerrcxt);
static xmlChar *pg_xmlCharStrndup(const char *str, size_t len);
#endif							/* USE_LIBXML */

static void xmldata_root_element_start(StringInfo result, const char *eltname,
									   const char *xmlschema, const char *targetns,
									   bool top_level);
static void xmldata_root_element_end(StringInfo result, const char *eltname);
static StringInfo query_to_xml_internal(const char *query, char *tablename,
										const char *xmlschema, bool nulls, bool tableforest,
										const char *targetns, bool top_level);
static const char *map_sql_table_to_xmlschema(TupleDesc tupdesc, Oid relid,
											  bool nulls, bool tableforest, const char *targetns);
static const char *map_sql_schema_to_xmlschema_types(Oid nspid,
													 List *relid_list, bool nulls,
													 bool tableforest, const char *targetns);
static const char *map_sql_catalog_to_xmlschema_types(List *nspid_list,
													  bool nulls, bool tableforest,
													  const char *targetns);
static const char *map_sql_type_to_xml_name(Oid typeoid, int typmod);
static const char *map_sql_typecoll_to_xmlschema_types(List *tupdesc_list);
static const char *map_sql_type_to_xmlschema_type(Oid typeoid, int typmod);
static void SPI_sql_row_to_xmlelement(uint64 rownum, StringInfo result,
									  char *tablename, bool nulls, bool tableforest,
									  const char *targetns, bool top_level);

/* XMLTABLE 支持 */
#ifdef USE_LIBXML
/* 用于标识 XmlTableContext 的随机数 */
#define XMLTABLE_CONTEXT_MAGIC	46922182
typedef struct XmlTableBuilderData
{
	int			magic;
	int			natts;
	long int	row_count;
	PgXmlErrorContext *xmlerrcxt;
	xmlParserCtxtPtr ctxt;
	xmlDocPtr	doc;
	xmlXPathContextPtr xpathcxt;
	xmlXPathCompExprPtr xpathcomp;
	xmlXPathObjectPtr xpathobj;
	xmlXPathCompExprPtr *xpathscomp;
} XmlTableBuilderData;
#endif

static void XmlTableInitOpaque(struct TableFuncScanState *state, int natts);
static void XmlTableSetDocument(struct TableFuncScanState *state, Datum value);
static void XmlTableSetNamespace(struct TableFuncScanState *state, const char *name,
								 const char *uri);
static void XmlTableSetRowFilter(struct TableFuncScanState *state, const char *path);
static void XmlTableSetColumnFilter(struct TableFuncScanState *state,
									const char *path, int colnum);
static bool XmlTableFetchRow(struct TableFuncScanState *state);
static Datum XmlTableGetValue(struct TableFuncScanState *state, int colnum,
							  Oid typid, int32 typmod, bool *isnull);
static void XmlTableDestroyOpaque(struct TableFuncScanState *state);

const TableFuncRoutine XmlTableRoutine =
{
	XmlTableInitOpaque,
	XmlTableSetDocument,
	XmlTableSetNamespace,
	XmlTableSetRowFilter,
	XmlTableSetColumnFilter,
	XmlTableFetchRow,
	XmlTableGetValue,
	XmlTableDestroyOpaque
};

#define NO_XML_SUPPORT() \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("unsupported XML feature"), \
			 errdetail("This functionality requires the server to be built with libxml support.")))


/* 来自 SQL/XML:2008 4.9 节 */
#define NAMESPACE_XSD "http://www.w3.org/2001/XMLSchema"
#define NAMESPACE_XSI "http://www.w3.org/2001/XMLSchema-instance"
#define NAMESPACE_SQLXML "http://standards.iso.org/iso/9075/2003/sqlxml"


#ifdef USE_LIBXML

static int
xmlChar_to_encoding(const xmlChar *encoding_name)
{
	int			encoding = pg_char_to_encoding((const char *) encoding_name);

	if (encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid encoding name \"%s\"",
						(const char *) encoding_name)));
	return encoding;
}
#endif


/*
 * xml_in 使用普通的 C 字符串到 VARDATA 转换，所以暂时
 * 使用 text 数据类型的转换函数。
 *
 * 只要 xmltype 与 text 表示形式相同，这样做就是可
 * 接受的。
 */
Datum
xml_in(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	char	   *s = PG_GETARG_CSTRING(0);
	xmltype    *vardata;
	xmlDocPtr	doc;

	/* 构建结果对象。 */
	vardata = (xmltype *) cstring_to_text(s);

	/*
	 * 解析数据以检查其是否为良构 XML 数据。
	 *
	 * 注意：我们无需关心是否检测到软错误。
	 */
	doc = xml_parse(vardata, xmloption, true, GetDatabaseEncoding(),
					NULL, NULL, fcinfo->context);
	if (doc != NULL)
		xmlFreeDoc(doc);

	PG_RETURN_XML_P(vardata);
#else
	NO_XML_SUPPORT();
	return 0;
#endif
}


#define PG_XML_DEFAULT_VERSION "1.0"


/*
 * xml_out_internal 使用普通的 VARDATA 到 C 字符串转换，所以
 * 暂时使用 text 数据类型的转换函数。
 *
 * 只要 xmltype 与 text 表示形式相同，这样做就是可
 * 接受的。
 */
static char *
xml_out_internal(xmltype *x, pg_enc target_encoding)
{
	char	   *str = text_to_cstring((text *) x);

#ifdef USE_LIBXML
	size_t		len = strlen(str);
	xmlChar    *version;
	int			standalone;
	int			res_code;

	if ((res_code = parse_xml_decl((xmlChar *) str,
								   &len, &version, NULL, &standalone)) == 0)
	{
		StringInfoData buf;

		initStringInfo(&buf);

		if (!print_xml_decl(&buf, version, target_encoding, standalone))
		{
			/*
			 * 如果不产出 XML 声明，就吃掉原字符串中的一个
			 * 换行符，以免输出的第一行为
			 * 空。
			 */
			if (*(str + len) == '\n')
				len += 1;
		}
		appendStringInfoString(&buf, str + len);

		pfree(str);

		return buf.data;
	}

	ereport(WARNING,
			errcode(ERRCODE_INTERNAL_ERROR),
			errmsg_internal("could not parse XML declaration in stored value"),
			errdetail_for_xml_code(res_code));
#endif
	return str;
}


Datum
xml_out(PG_FUNCTION_ARGS)
{
	xmltype    *x = PG_GETARG_XML_P(0);

	/*
	 * xml_out 在任何情况下都移除编码属性。这是因为我们
	 * 无法在这里控制 datum 之后会不会被转换为某种不
	 * 同的客户端编码，把它包含在内反而会弊大于
	 * 利
	 */
	PG_RETURN_CSTRING(xml_out_internal(x, 0));
}


Datum
xml_recv(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	xmltype    *result;
	char	   *str;
	char	   *newstr;
	int			nbytes;
	xmlDocPtr	doc;
	xmlChar    *encodingStr = NULL;
	int			encoding;

	/*
	 * 以原始格式读取数据。编码未知，因为该信息嵌在 XML
	 * 声明里；所以必须在转为服务器编码之前
	 * 先对它进行解析。
	 */
	nbytes = buf->len - buf->cursor;
	str = (char *) pq_getmsgbytes(buf, nbytes);

	/*
	 * 必须给 parse_xml_decl() 传入以空字符结尾的字符串。
	 * 与其单独复制一份，不如让临时结果比所需的再多
	 * 出一个字节。
	 */
	result = palloc(nbytes + 1 + VARHDRSZ);
	SET_VARSIZE(result, nbytes + VARHDRSZ);
	memcpy(VARDATA(result), str, nbytes);
	str = VARDATA(result);
	str[nbytes] = '\0';

	parse_xml_decl((const xmlChar *) str, NULL, NULL, &encodingStr, NULL);

	/*
	 * 若 XML 头未显式指定编码，则将其视为 UTF-8，因为
	 * 这是 XML 的默认值。这点与 xml_in() 不同，后者的输
	 * 入必须走常规的客户端到服务器编码
	 * 转换。
	 */
	encoding = encodingStr ? xmlChar_to_encoding(encodingStr) : PG_UTF8;

	/*
	 * 解析数据以检查其是否为良构 XML 数据。假定
	 * 否则 xml_parse 将抛 ERROR。
	 */
	doc = xml_parse(result, xmloption, true, encoding, NULL, NULL, NULL);
	xmlFreeDoc(doc);

	/* 既然已经知道要处理的对象，就转换到服务器编码 */
	newstr = pg_any_to_server(str, nbytes, encoding);

	if (newstr != str)
	{
		pfree(result);
		result = (xmltype *) cstring_to_text(newstr);
		pfree(newstr);
	}

	PG_RETURN_XML_P(result);
#else
	NO_XML_SUPPORT();
	return 0;
#endif
}


Datum
xml_send(PG_FUNCTION_ARGS)
{
	xmltype    *x = PG_GETARG_XML_P(0);
	char	   *outval;
	StringInfoData buf;

	/*
	 * xml_out_internal 不转换编码，只打印正确的声明。
	 * pq_sendtext 会进行转换。
	 */
	outval = xml_out_internal(x, pg_get_client_encoding());

	pq_begintypsend(&buf);
	pq_sendtext(&buf, outval, strlen(outval));
	pfree(outval);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


#ifdef USE_LIBXML
static void
appendStringInfoText(StringInfo str, const text *t)
{
	appendBinaryStringInfo(str, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
}
#endif


static xmltype *
stringinfo_to_xmltype(StringInfo buf)
{
	return (xmltype *) cstring_to_text_with_len(buf->data, buf->len);
}


static xmltype *
cstring_to_xmltype(const char *string)
{
	return (xmltype *) cstring_to_text(string);
}


#ifdef USE_LIBXML
static xmltype *
xmlBuffer_to_xmltype(xmlBufferPtr buf)
{
	return (xmltype *) cstring_to_text_with_len((const char *) xmlBufferContent(buf),
												xmlBufferLength(buf));
}
#endif


Datum
xmlcomment(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *arg = PG_GETARG_TEXT_PP(0);
	char	   *argdata = VARDATA_ANY(arg);
	int			len = VARSIZE_ANY_EXHDR(arg);
	StringInfoData buf;
	int			i;

	/* 检查字符串中的 "--" 或结尾 "-" */
	for (i = 1; i < len; i++)
	{
		if (argdata[i] == '-' && argdata[i - 1] == '-')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_XML_COMMENT),
					 errmsg("invalid XML comment")));
	}
	if (len > 0 && argdata[len - 1] == '-')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_XML_COMMENT),
				 errmsg("invalid XML comment")));

	initStringInfo(&buf);
	appendStringInfoString(&buf, "<!--");
	appendStringInfoText(&buf, arg);
	appendStringInfoString(&buf, "-->");

	PG_RETURN_XML_P(stringinfo_to_xmltype(&buf));
#else
	NO_XML_SUPPORT();
	return 0;
#endif
}



/*
 * TODO：xmlconcat 需合并参数值的记法与未解析实体。
 * 不过实践中这不太重要。
 */
xmltype *
xmlconcat(List *args)
{
#ifdef USE_LIBXML
	int			global_standalone = 1;
	xmlChar    *global_version = NULL;
	bool		global_version_no_value = false;
	StringInfoData buf;
	ListCell   *v;

	initStringInfo(&buf);
	foreach(v, args)
	{
		xmltype    *x = DatumGetXmlP(PointerGetDatum(lfirst(v)));
		size_t		len;
		xmlChar    *version;
		int			standalone;
		char	   *str;

		len = VARSIZE(x) - VARHDRSZ;
		str = text_to_cstring((text *) x);

		parse_xml_decl((xmlChar *) str, &len, &version, NULL, &standalone);

		if (standalone == 0 && global_standalone == 1)
			global_standalone = 0;
		if (standalone < 0)
			global_standalone = -1;

		if (!version)
			global_version_no_value = true;
		else if (!global_version)
			global_version = version;
		else if (xmlStrcmp(version, global_version) != 0)
			global_version_no_value = true;

		appendStringInfoString(&buf, str + len);
		pfree(str);
	}

	if (!global_version_no_value || global_standalone >= 0)
	{
		StringInfoData buf2;

		initStringInfo(&buf2);

		print_xml_decl(&buf2,
					   (!global_version_no_value) ? global_version : NULL,
					   0,
					   global_standalone);

		appendBinaryStringInfo(&buf2, buf.data, buf.len);
		buf = buf2;
	}

	return stringinfo_to_xmltype(&buf);
#else
	NO_XML_SUPPORT();
	return NULL;
#endif
}


/*
 * XMLAGG 支持
 */
Datum
xmlconcat2(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
	{
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();
		else
			PG_RETURN_XML_P(PG_GETARG_XML_P(1));
	}
	else if (PG_ARGISNULL(1))
		PG_RETURN_XML_P(PG_GETARG_XML_P(0));
	else
		PG_RETURN_XML_P(xmlconcat(list_make2(PG_GETARG_XML_P(0),
											 PG_GETARG_XML_P(1))));
}


Datum
texttoxml(PG_FUNCTION_ARGS)
{
	text	   *data = PG_GETARG_TEXT_PP(0);

	PG_RETURN_XML_P(xmlparse(data, xmloption, true));
}


Datum
xmltotext(PG_FUNCTION_ARGS)
{
	xmltype    *data = PG_GETARG_XML_P(0);

	/* 实际上二进制兼容。 */
	PG_RETURN_TEXT_P((text *) data);
}


text *
xmltotext_with_options(xmltype *data, XmlOptionType xmloption_arg, bool indent)
{
#ifdef USE_LIBXML
	text	   *volatile result;
	xmlDocPtr	doc;
	XmlOptionType parsed_xmloptiontype;
	xmlNodePtr	content_nodes;
	volatile xmlBufferPtr buf = NULL;
	volatile xmlSaveCtxtPtr ctxt = NULL;
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	PgXmlErrorContext *volatile xmlerrcxt = NULL;
#endif

	if (xmloption_arg != XMLOPTION_DOCUMENT && !indent)
	{
		/*
		 * 其实无需做任何事，直接返回二进制兼容
		 * 输入即可。为向后兼容，即使无 USE_LIBXML
		 * 也允许此类情况成功。
		 */
		return (text *) data;
	}

#ifdef USE_LIBXML

	/*
	 * 根据 xmloption 解析输入。
	 *
	 * 如果我们打算缩进，就把 preserve_whitespace 设为 false，
	 * 否则 libxml2 将无法缩进彼此之间有空白的元素。
	 */
	doc = xml_parse(data, xmloption_arg, !indent, GetDatabaseEncoding(),
					&parsed_xmloptiontype, &content_nodes,
					(Node *) &escontext);
	if (doc == NULL || escontext.error_occurred)
	{
		if (doc)
			xmlFreeDoc(doc);
		/* 软错误必须是不符合 XMLOPTION_DOCUMENT 的情形 */
		ereport(ERROR,
				(errcode(ERRCODE_NOT_AN_XML_DOCUMENT),
				 errmsg("not an XML document")));
	}

	/* 如果未要求缩进，即告完成。 */
	if (!indent)
	{
		xmlFreeDoc(doc);
		return (text *) data;
	}

	/*
	 * 否则就得准备错误处理。与模块中大多数其他例程
	 * 不同，我们已有待释放的 libxml "doc" 结构，因此要
	 * 在 PG_TRY 内调用 pg_xml_init()，并准备好接受它
	 * 失败（通常因 palloc OOM）。
	 */
	PG_TRY();
	{
		size_t		decl_len = 0;

		xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

		/* 序列化后的数据将进入此缓冲区。 */
		buf = xmlBufferCreate();

		if (buf == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate xmlBuffer");

		/* 检测是否存在 XML 声明 */
		parse_xml_decl(xml_text2xmlChar(data), &decl_len, NULL, NULL, NULL);

		/*
		 * 仅当输入带有声明时才输出它。注意：某些版本的
		 * xmlSaveToBuffer 在传入非空 encoding 参数时会泄
		 * 漏内存，故勿那样做。反正不需要任何编码转
		 * 换。
		 */
		if (decl_len == 0)
			ctxt = xmlSaveToBuffer(buf, NULL,
								   XML_SAVE_NO_DECL | XML_SAVE_FORMAT);
		else
			ctxt = xmlSaveToBuffer(buf, NULL,
								   XML_SAVE_FORMAT);

		if (ctxt == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate xmlSaveCtxt");

		if (parsed_xmloptiontype == XMLOPTION_DOCUMENT)
		{
			/* 若是文档，保存就简单。 */
			if (xmlSaveDoc(ctxt, doc) == -1 || xmlerrcxt->err_occurred)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_INTERNAL_ERROR,
							"could not save document to xmlBuffer");
		}
		else if (content_nodes != NULL)
		{
			/*
			 * 处理非单一根 XML 的情形。
			 * 没有帮助时 libxml 的转储函数对此处理得不好。
			 * 构建伪根节点作为内容节点的容器，然后
			 * 遍历这些节点。
			 */
			xmlNodePtr	root;
			xmlNodePtr	oldroot;
			xmlNodePtr	newline;

			root = xmlNewNode(NULL, (const xmlChar *) "content-root");
			if (root == NULL || xmlerrcxt->err_occurred)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
							"could not allocate xml node");

			/*
			 * 这会把 root 挂到 doc 上，无需单独释放它……
			 * 但相反，如果之前有旧根，就必须释放它。
			 */
			oldroot = xmlDocSetRootElement(doc, root);
			if (oldroot != NULL)
				xmlFreeNode(oldroot);

			xmlAddChildList(root, content_nodes);

			/*
			 * 以该节点在转储中插入换行。注意：至少一
			 * 些 libxml 版本里，即便传文档，xmlNewDocText 也不
			 * 会把节点挂上文档。因此要手动管理其释放，
			 * 并在此传入 NULL 以确保不会留下悬空链
			 * 接。
			 */
			newline = xmlNewDocText(NULL, (const xmlChar *) "\n");
			if (newline == NULL || xmlerrcxt->err_occurred)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
							"could not allocate xml node");

			for (xmlNodePtr node = root->children; node; node = node->next)
			{
				/* 在节点之间插入换行 */
				if (node->type != XML_TEXT_NODE && node->prev != NULL)
				{
					if (xmlSaveTree(ctxt, newline) == -1 || xmlerrcxt->err_occurred)
					{
						xmlFreeNode(newline);
						xml_ereport(xmlerrcxt, ERROR, ERRCODE_INTERNAL_ERROR,
									"could not save newline to xmlBuffer");
					}
				}

				if (xmlSaveTree(ctxt, node) == -1 || xmlerrcxt->err_occurred)
				{
					xmlFreeNode(newline);
					xml_ereport(xmlerrcxt, ERROR, ERRCODE_INTERNAL_ERROR,
								"could not save content to xmlBuffer");
				}
			}

			xmlFreeNode(newline);
		}

		if (xmlSaveClose(ctxt) == -1 || xmlerrcxt->err_occurred)
		{
			ctxt = NULL;		/* 不要再次尝试关闭 */
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INTERNAL_ERROR,
						"could not close xmlSaveCtxtPtr");
		}

		/*
		 * xmlDocContentDumpOutput 可能会加尾随换行，故移除它。
		 */
		if (xmloption_arg == XMLOPTION_DOCUMENT)
		{
			const char *str = (const char *) xmlBufferContent(buf);
			int			len = xmlBufferLength(buf);

			while (len > 0 && (str[len - 1] == '\n' ||
							   str[len - 1] == '\r'))
				len--;

			result = cstring_to_text_with_len(str, len);
		}
		else
			result = (text *) xmlBuffer_to_xmltype(buf);
	}
	PG_CATCH();
	{
		if (ctxt)
			xmlSaveClose(ctxt);
		if (buf)
			xmlBufferFree(buf);
		xmlFreeDoc(doc);

		if (xmlerrcxt)
			pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	xmlBufferFree(buf);
	xmlFreeDoc(doc);

	pg_xml_done(xmlerrcxt, false);

	return result;
#else
	NO_XML_SUPPORT();
	return NULL;
#endif
}


xmltype *
xmlelement(XmlExpr *xexpr,
		   Datum *named_argvalue, bool *named_argnull,
		   Datum *argvalue, bool *argnull)
{
#ifdef USE_LIBXML
	xmltype    *result;
	List	   *named_arg_strings;
	List	   *arg_strings;
	int			i;
	ListCell   *arg;
	ListCell   *narg;
	PgXmlErrorContext *xmlerrcxt;
	volatile xmlBufferPtr buf = NULL;
	volatile xmlTextWriterPtr writer = NULL;

	/*
	 * 所有参数均已求值，其值通过 named_argvalue/named_argnull
	 * 或 argvalue/argnull 数组传入。这可避免某参数牵涉调
	 * 用其他函数或子系统的情况，那些函数想按自己的
	 * 方式使用 libxml。我们考察原始 XmlExpr 来确定参数的
	 * 数量与类型。
	 */
	named_arg_strings = NIL;
	i = 0;
	foreach(arg, xexpr->named_args)
	{
		Expr	   *e = (Expr *) lfirst(arg);
		char	   *str;

		if (named_argnull[i])
			str = NULL;
		else
			str = map_sql_value_to_xml_value(named_argvalue[i],
											 exprType((Node *) e),
											 false);
		named_arg_strings = lappend(named_arg_strings, str);
		i++;
	}

	arg_strings = NIL;
	i = 0;
	foreach(arg, xexpr->args)
	{
		Expr	   *e = (Expr *) lfirst(arg);
		char	   *str;

		/* 这里可以直接忽略 NULL 元素 */
		if (!argnull[i])
		{
			str = map_sql_value_to_xml_value(argvalue[i],
											 exprType((Node *) e),
											 true);
			arg_strings = lappend(arg_strings, str);
		}
		i++;
	}

	xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

	PG_TRY();
	{
		buf = xmlBufferCreate();
		if (buf == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate xmlBuffer");
		writer = xmlNewTextWriterMemory(buf, 0);
		if (writer == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate xmlTextWriter");

		xmlTextWriterStartElement(writer, (xmlChar *) xexpr->name);

		forboth(arg, named_arg_strings, narg, xexpr->arg_names)
		{
			char	   *str = (char *) lfirst(arg);
			char	   *argname = strVal(lfirst(narg));

			if (str)
				xmlTextWriterWriteAttribute(writer,
											(xmlChar *) argname,
											(xmlChar *) str);
		}

		foreach(arg, arg_strings)
		{
			char	   *str = (char *) lfirst(arg);

			xmlTextWriterWriteRaw(writer, (xmlChar *) str);
		}

		xmlTextWriterEndElement(writer);

		/* 现在必须这样做，把数据刷入缓冲区…… */
		xmlFreeTextWriter(writer);
		writer = NULL;

		result = xmlBuffer_to_xmltype(buf);
	}
	PG_CATCH();
	{
		if (writer)
			xmlFreeTextWriter(writer);
		if (buf)
			xmlBufferFree(buf);

		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	xmlBufferFree(buf);

	pg_xml_done(xmlerrcxt, false);

	return result;
#else
	NO_XML_SUPPORT();
	return NULL;
#endif
}


xmltype *
xmlparse(text *data, XmlOptionType xmloption_arg, bool preserve_whitespace)
{
#ifdef USE_LIBXML
	xmlDocPtr	doc;

	doc = xml_parse(data, xmloption_arg, preserve_whitespace,
					GetDatabaseEncoding(), NULL, NULL, NULL);
	xmlFreeDoc(doc);

	return (xmltype *) data;
#else
	NO_XML_SUPPORT();
	return NULL;
#endif
}


xmltype *
xmlpi(const char *target, text *arg, bool arg_is_null, bool *result_is_null)
{
#ifdef USE_LIBXML
	xmltype    *result;
	StringInfoData buf;

	if (pg_strcasecmp(target, "xml") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR), /* 确实 */
				 errmsg("invalid XML processing instruction"),
				 errdetail("XML processing instruction target name cannot be \"%s\".", target)));

	/*
	 * 按照 SQL 标准，空值检查要放在上文语法检查之后。
	 * 
	 */
	*result_is_null = arg_is_null;
	if (*result_is_null)
		return NULL;

	initStringInfo(&buf);

	appendStringInfo(&buf, "<?%s", target);

	if (arg != NULL)
	{
		char	   *string;

		string = text_to_cstring(arg);
		if (strstr(string, "?>") != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_XML_PROCESSING_INSTRUCTION),
					 errmsg("invalid XML processing instruction"),
					 errdetail("XML processing instruction cannot contain \"?>\".")));

		appendStringInfoChar(&buf, ' ');
		appendStringInfoString(&buf, string + strspn(string, " "));
		pfree(string);
	}
	appendStringInfoString(&buf, "?>");

	result = stringinfo_to_xmltype(&buf);
	pfree(buf.data);
	return result;
#else
	NO_XML_SUPPORT();
	return NULL;
#endif
}


xmltype *
xmlroot(xmltype *data, text *version, int standalone)
{
#ifdef USE_LIBXML
	char	   *str;
	size_t		len;
	xmlChar    *orig_version;
	int			orig_standalone;
	StringInfoData buf;

	len = VARSIZE(data) - VARHDRSZ;
	str = text_to_cstring((text *) data);

	parse_xml_decl((xmlChar *) str, &len, &orig_version, NULL, &orig_standalone);

	if (version)
		orig_version = xml_text2xmlChar(version);
	else
		orig_version = NULL;

	switch (standalone)
	{
		case XML_STANDALONE_YES:
			orig_standalone = 1;
			break;
		case XML_STANDALONE_NO:
			orig_standalone = 0;
			break;
		case XML_STANDALONE_NO_VALUE:
			orig_standalone = -1;
			break;
		case XML_STANDALONE_OMITTED:
			/* 保留原值 */
			break;
	}

	initStringInfo(&buf);
	print_xml_decl(&buf, orig_version, 0, orig_standalone);
	appendStringInfoString(&buf, str + len);

	return stringinfo_to_xmltype(&buf);
#else
	NO_XML_SUPPORT();
	return NULL;
#endif
}


/*
 * 针对 DTD（以外链给定）校验文档（以字符串给定）
 *
 * 此功能已移除，因为它是个安全漏洞：无特权用户不
 * 应能用 Postgres 获取任意外部文件，遗憾的是这恰
 * 恰是 libxml 愿意借 DTD 参数去做的事
 * 情。
 */
Datum
xmlvalidate(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("xmlvalidate is not implemented")));
	return 0;
}


bool
xml_is_document(xmltype *arg)
{
#ifdef USE_LIBXML
	xmlDocPtr	doc;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	/*
	 * 若 xml_parse() 未报告软错误，我们将报告 "true"。
	 */
	doc = xml_parse((text *) arg, XMLOPTION_DOCUMENT, true,
					GetDatabaseEncoding(), NULL, NULL, (Node *) &escontext);
	if (doc)
		xmlFreeDoc(doc);

	return !escontext.error_occurred;
#else							/* not USE_LIBXML */
	NO_XML_SUPPORT();
	return false;
#endif							/* not USE_LIBXML */
}


#ifdef USE_LIBXML

/*
 * pg_xml_init_library --- 准备使用 libxml
 *
 * 每个将使用 libxml 设施但不需要错误处理的函数
 * 都应调用本函数。它会初始化 libxml 并校验
 * 与所加载 libxml 版本的兼容性。这些都是每会话一次
 * 活动。
 *
 * TODO：xmlChar 为 utf8 字符，需调优（initdb 在 enc!=utf8 时验
 * 证）
 */
void
pg_xml_init_library(void)
{
	static bool first_time = true;

	if (first_time)
	{
		/* 每会话只需做一次的事情 */

		/*
		 * 目前内部尚不支持纯 UTF-8 —— 检查我们能否工
		 * 作。
		 */
		if (sizeof(char) != sizeof(xmlChar))
			ereport(ERROR,
					(errmsg("could not initialize XML library"),
					 errdetail("libxml2 has incompatible char type: sizeof(char)=%zu, sizeof(xmlChar)=%zu.",
							   sizeof(char), sizeof(xmlChar))));

#ifdef USE_LIBXMLCONTEXT
		/* 按我们方式设置 libxml 内存分配 */
		xml_memory_init();
#endif

		/* 检查库兼容性 */
		LIBXML_TEST_VERSION;

		first_time = false;
	}
}

/*
 * pg_xml_init --- 准备使用 libxml 并注册错误处理器
 *
 * 每个将使用 libxml 设施且需要错误处理的函数都
 * 应调用本函数。它先用 pg_xml_init_library() 初
 * 始化 libxml，并建立我们的 libxml 错误处理器。
 *
 * strictness 决定哪些错误被报告，哪些被忽略。
 *
 * 对此函数的调用之后必须紧跟一个 PG_TRY 块，以保证在
 * 正常退出或错误退出时都会调用 pg_xml_done()。
 *
 * 此函数导出供 contrib/xml2 使用，也供其他可能希望共享本
 * 模块 libxml 错误处理器的代码使用。
 */
PgXmlErrorContext *
pg_xml_init(PgXmlStrictness strictness)
{
	PgXmlErrorContext *errcxt;
	void	   *new_errcxt;

	/* 需要时做一次性设置 */
	pg_xml_init_library();

	/* 创建错误处理上下文结构 */
	errcxt = (PgXmlErrorContext *) palloc(sizeof(PgXmlErrorContext));
	errcxt->magic = ERRCXT_MAGIC;
	errcxt->strictness = strictness;
	errcxt->err_occurred = false;
	initStringInfo(&errcxt->err_buf);

	/*
	 * 保存原错误处理器并安装我们的。libxml 最初并不区
	 * 分通用与结构化错误处理器的上下文；所以若用旧版
	 * 本 libxml，就必须保存通用错误上下文，即便我们用的
	 * 是结构化错误处理
	 * 器。
	 */
	errcxt->saved_errfunc = xmlStructuredError;

#ifdef HAVE_XMLSTRUCTUREDERRORCONTEXT
	errcxt->saved_errcxt = xmlStructuredErrorContext;
#else
	errcxt->saved_errcxt = xmlGenericErrorContext;
#endif

	xmlSetStructuredErrorFunc((void *) errcxt, xml_errorHandler);

	/*
	 * 校验 xmlSetStructuredErrorFunc 是否设置了我们期望的
	 * 上下文变量。若否，我们刚保存的错误上下文指针就
	 * 不是应恢复的正确对象，而这使我们在 pg_xml_done
	 * 中无法恢复上下文，故必须失败。
	 *
	 * 已知唯一会使此检验失败的情形：编译用头文件来自
	 * 不跟踪结构化错误上下文的 libxml2（< 2.7.4），但运
	 * 行时却用会跟踪的版本，或反之。libxml2 作者不认为
	 * 这一变更构成 ABI 破坏，所以 pg_xml_init_library 中的
	 * LIBXML_TEST_VERSION 检验无法保护我们免受
	 * 此问题。
	 */

#ifdef HAVE_XMLSTRUCTUREDERRORCONTEXT
	new_errcxt = xmlStructuredErrorContext;
#else
	new_errcxt = xmlGenericErrorContext;
#endif

	if (new_errcxt != (void *) errcxt)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not set up XML error handler"),
				 errhint("This probably indicates that the version of libxml2"
						 " being used is not compatible with the libxml2"
						 " header files that PostgreSQL was built with.")));

	/*
	 * 另外，安装实体加载器，防止获取不想要的外部文件
	 * 和 URL。
	 */
	errcxt->saved_entityfunc = xmlGetExternalEntityLoader();
	xmlSetExternalEntityLoader(xmlPgEntityLoader);

	return errcxt;
}


/*
 * pg_xml_done --- 恢复原 libxml 错误处理
 *
 * 把 libxml 的全局错误处理状态重置回 pg_xml_init() 被
 * 调用之前的状态。
 *
 * 本例程校验所有未决错误已处理（至少在启用断言
 * 的构建中）。
 */
void
pg_xml_done(PgXmlErrorContext *errcxt, bool isError)
{
	void	   *cur_errcxt;

	/* 断言在此似乎已是足够的保护 */
	Assert(errcxt->magic == ERRCXT_MAGIC);

	/*
	 * 正常退出时不应有未处理的 libxml 错误。但在错误恢
	 * 复期间不应强制这一点，因为 longjmp 可能在 xml_ereport
	 * 有机会运行之前就被抛出。
	 */
	Assert(!errcxt->err_occurred || isError);

	/*
	 * 检查 libxml 全局状态是否正确，不对则告警。这是真
	 * 正的检验而非 Assert，因为它发生的机会更大一
	 * 些。
	 */
#ifdef HAVE_XMLSTRUCTUREDERRORCONTEXT
	cur_errcxt = xmlStructuredErrorContext;
#else
	cur_errcxt = xmlGenericErrorContext;
#endif

	if (cur_errcxt != (void *) errcxt)
		elog(WARNING, "libxml error handling state is out of sync with xml.c");

	/* 恢复保存的处理器 */
	xmlSetStructuredErrorFunc(errcxt->saved_errcxt, errcxt->saved_errfunc);
	xmlSetExternalEntityLoader(errcxt->saved_entityfunc);

	/*
	 * 将结构标记为无效，以防有人不知怎么竟用它调用
	 * xml_errorHandler 或 xml_ereport。
	 */
	errcxt->magic = 0;

	/* 释放内存 */
	pfree(errcxt->err_buf.data);
	pfree(errcxt);
}


/*
 * pg_xml_error_occurred() --- 测试错误标志
 */
bool
pg_xml_error_occurred(PgXmlErrorContext *errcxt)
{
	return errcxt->err_occurred;
}


/*
 * SQL/XML 允许存储 "XML 文档"或 "XML 内容"。
 * "XML 文档"由 XML 规范定义，libxml 很容易解析。
 * "XML content" 由 SQL/XML 指定为产生式 "XMLDecl?
 *  content"。但 libxml 只能解析 "content" 部分，故须
 * 自行解析 XML 声明来完成这
 * 件事。
 */

#define CHECK_XML_SPACE(p) \
	do { \
		if (!xmlIsBlank_ch(*(p))) \
			return XML_ERR_SPACE_REQUIRED; \
	} while (0)

#define SKIP_XML_SPACE(p) \
	while (xmlIsBlank_ch(*(p))) (p)++

/* Letter | Digit | '.' | '-' | '_' | ':' | CombiningChar | Extender */
/* 当心参数被多次求值！ */
#define PG_XMLISNAMECHAR(c) \
	(xmlIsBaseChar_ch(c) || xmlIsIdeographicQ(c) \
			|| xmlIsDigit_ch(c) \
			|| c == '.' || c == '-' || c == '_' || c == ':' \
			|| xmlIsCombiningQ(c) \
			|| xmlIsExtender_ch(c))

/* pnstrdup，但面向 xmlChar 而非 char；len 按 xmlChar 计 */
static xmlChar *
xml_pnstrdup(const xmlChar *str, size_t len)
{
	xmlChar    *result;

	result = (xmlChar *) palloc((len + 1) * sizeof(xmlChar));
	memcpy(result, str, len * sizeof(xmlChar));
	result[len] = 0;
	return result;
}

/* 同上，但输入为 char* */
static xmlChar *
pg_xmlCharStrndup(const char *str, size_t len)
{
	xmlChar    *result;

	result = (xmlChar *) palloc((len + 1) * sizeof(xmlChar));
	memcpy(result, str, len);
	result[len] = '\0';

	return result;
}

/*
 * 把 xmlChar 串复制到 PostgreSQL 自有内存，释放输入。
 *
 * 无论复制成败与否，输入的 xmlChar 都将释放。
 */
static char *
xml_pstrdup_and_free(xmlChar *str)
{
	char	   *result;

	if (str)
	{
		PG_TRY();
		{
			result = pstrdup((char *) str);
		}
		PG_FINALLY();
		{
			xmlFree(str);
		}
		PG_END_TRY();
	}
	else
		result = NULL;

	return result;
}

/*
 * str 是空值结尾的输入串。其余参数是输出参数；
 * 每个都可以在不需要该值时传入 NULL。
 * version 与 encoding 以本地 palloc 的字符串返回。
 * 成功时为 0，否则为错误码。
 */
static int
parse_xml_decl(const xmlChar *str, size_t *lenp,
			   xmlChar **version, xmlChar **encoding, int *standalone)
{
	const xmlChar *p;
	const xmlChar *save_p;
	size_t		len;
	int			utf8char;
	int			utf8len;

	/*
	 * 仅初始化 libxml。此处无需错误处理，但在调用其任
	 * 何函数之前必须确保 libxml 已初始化。注意若已
	 * 经调用过 pg_xml_init()，这是安全的（也是
	 * 空操作）。
	 */
	pg_xml_init_library();

	/* 把输出参数初始化为"不存在" */
	if (version)
		*version = NULL;
	if (encoding)
		*encoding = NULL;
	if (standalone)
		*standalone = -1;

	p = str;

	if (xmlStrncmp(p, (xmlChar *) "<?xml", 5) != 0)
		goto finished;

	/*
	 * 若下一字符是名字字符，那就是形如 <?xml-stylesheet
	 *  ...?> 的 PI 而非 XMLDecl，故已做完该做的事，并无
	 * XMLDecl
	 *
	 * xmlGetUTF8Char 需要输入长度值，但无需统计整个文档
	 * 的大小，所以用 strnlen 而非 strlen。
	 */
	utf8len = strnlen((const char *) (p + 5), MAX_MULTIBYTE_CHAR_LEN);
	utf8char = xmlGetUTF8Char(p + 5, &utf8len);
	if (PG_XMLISNAMECHAR(utf8char))
		goto finished;

	p += 5;

	/* version */
	CHECK_XML_SPACE(p);
	SKIP_XML_SPACE(p);
	if (xmlStrncmp(p, (xmlChar *) "version", 7) != 0)
		return XML_ERR_VERSION_MISSING;
	p += 7;
	SKIP_XML_SPACE(p);
	if (*p != '=')
		return XML_ERR_VERSION_MISSING;
	p += 1;
	SKIP_XML_SPACE(p);

	if (*p == '\'' || *p == '"')
	{
		const xmlChar *q;

		q = xmlStrchr(p + 1, *p);
		if (!q)
			return XML_ERR_VERSION_MISSING;

		if (version)
			*version = xml_pnstrdup(p + 1, q - p - 1);
		p = q + 1;
	}
	else
		return XML_ERR_VERSION_MISSING;

	/* encoding */
	save_p = p;
	SKIP_XML_SPACE(p);
	if (xmlStrncmp(p, (xmlChar *) "encoding", 8) == 0)
	{
		CHECK_XML_SPACE(save_p);
		p += 8;
		SKIP_XML_SPACE(p);
		if (*p != '=')
			return XML_ERR_MISSING_ENCODING;
		p += 1;
		SKIP_XML_SPACE(p);

		if (*p == '\'' || *p == '"')
		{
			const xmlChar *q;

			q = xmlStrchr(p + 1, *p);
			if (!q)
				return XML_ERR_MISSING_ENCODING;

			if (encoding)
				*encoding = xml_pnstrdup(p + 1, q - p - 1);
			p = q + 1;
		}
		else
			return XML_ERR_MISSING_ENCODING;
	}
	else
	{
		p = save_p;
	}

	/* standalone */
	save_p = p;
	SKIP_XML_SPACE(p);
	if (xmlStrncmp(p, (xmlChar *) "standalone", 10) == 0)
	{
		CHECK_XML_SPACE(save_p);
		p += 10;
		SKIP_XML_SPACE(p);
		if (*p != '=')
			return XML_ERR_STANDALONE_VALUE;
		p += 1;
		SKIP_XML_SPACE(p);
		if (xmlStrncmp(p, (xmlChar *) "'yes'", 5) == 0 ||
			xmlStrncmp(p, (xmlChar *) "\"yes\"", 5) == 0)
		{
			if (standalone)
				*standalone = 1;
			p += 5;
		}
		else if (xmlStrncmp(p, (xmlChar *) "'no'", 4) == 0 ||
				 xmlStrncmp(p, (xmlChar *) "\"no\"", 4) == 0)
		{
			if (standalone)
				*standalone = 0;
			p += 4;
		}
		else
			return XML_ERR_STANDALONE_VALUE;
	}
	else
	{
		p = save_p;
	}

	SKIP_XML_SPACE(p);
	if (xmlStrncmp(p, (xmlChar *) "?>", 2) != 0)
		return XML_ERR_XMLDECL_NOT_FINISHED;
	p += 2;

finished:
	len = p - str;

	for (p = str; p < str + len; p++)
		if (*p > 127)
			return XML_ERR_INVALID_CHAR;

	if (lenp)
		*lenp = len;

	return XML_ERR_OK;
}


/*
 * 写 XML 声明。输出时，我们按如下规则调整 XML 声明
 * （这些规则等同于 SQL 标准的"XML 值序列化"条
 * 款。）
 *
 * 我们尽量避免生成 XML 声明。这是为了免得出现像
 * xml '<foo/>' 产出 '<?xml version="1.0"?><foo/>' 这种
 * 绝对恼人的结果。
 * 若指定了 standalone 属性或包含编码声明，就必须
 * 提供声明。
 * 有声明时必须指定版本（XML 要求）；否则只
 * 在版本不是 "1.0" 时才生成声明，"1.0" 是 SQL:2003 中
 * 指定的默认版本。
 */
static bool
print_xml_decl(StringInfo buf, const xmlChar *version,
			   pg_enc encoding, int standalone)
{
	if ((version && strcmp((const char *) version, PG_XML_DEFAULT_VERSION) != 0)
		|| (encoding && encoding != PG_UTF8)
		|| standalone != -1)
	{
		appendStringInfoString(buf, "<?xml");

		if (version)
			appendStringInfo(buf, " version=\"%s\"", version);
		else
			appendStringInfo(buf, " version=\"%s\"", PG_XML_DEFAULT_VERSION);

		if (encoding && encoding != PG_UTF8)
		{
			/*
			 * XXX 把这里的值转成 IANA 名（用 ISO-8859-1 替代
			 * LATIN1 等）；需要实践检验。
			 */
			appendStringInfo(buf, " encoding=\"%s\"",
							 pg_encoding_to_char(encoding));
		}

		if (standalone == 1)
			appendStringInfoString(buf, " standalone=\"yes\"");
		else if (standalone == 0)
			appendStringInfoString(buf, " standalone=\"no\"");
		appendStringInfoString(buf, "?>");

		return true;
	}
	else
		return false;
}

/*
 * 测试将要按 CONTENT 解析的输入是否包含 DTD。
 *
 * SQL/XML:2003 对 CONTENT（"XMLDecl? content"）的定义并
 * 未被带 DTD 的文档所满足，这有点瑕疵，因为它意味着
 * CONTENT 类型并非 DOCUMENT 的真超集。SQL/XML:2006 及
 * 后续版本修正了它：借 XQuery/XPath 数据模型中
 * "更宽松"的 Document Node 重新定义 content，使得任何
 * DOCUMENT 值确实也是 CONTENT 值。该定义更有用，因
 * 为 CONTENT 可用于解析未知形态的输入(如 pg_
 * restore)。
 *
 * 正如下方 parse_xml 按 CONTENT 解析时所用的那样，libxml
 * 并不会给我们 2006+ 的行为，而只给 2003 的；若输入带
 * 有 DTD 它就会噎住。但我们可以相当轻松地提供 2006+
 * 的 CONTENT 定义：先检测出这种情形，然后直接按
 *
 * DTD 可出现在任意深处，但那是刻意构造的案例；
 * 它通常会在头几十个字符内开始。唯一能出现在它之前
 * 的有 XMLDecl（此处调用方已调 parse_xml_decl）、空白、
 * 注释和处理指令。本函数只要看到通向 <!DOCTYPE 的
 * 有效序列返回 true 即可。其余任何情形（包括畸形输
 * 入在内）都可返回 false；即输入将按原计划
 * 作为 CONTENT 解析，并由 libxml 报告任何错
 * 误。
 *
 * 只能从 xml_parse 内调用，且前提是 pg_xml_init 已被调
 * 用过。输入已处于 UTF8 编码。
 */
static bool
xml_doctype_in_content(const xmlChar *str)
{
	const xmlChar *p = str;

	for (;;)
	{
		const xmlChar *e;

		SKIP_XML_SPACE(p);
		if (*p != '<')
			return false;
		p++;

		if (*p == '!')
		{
			p++;

			/* 看到 <!DOCTYPE 即可返回 true */
			if (xmlStrncmp(p, (xmlChar *) "DOCTYPE", 7) == 0)
				return true;

			/* 否则，若不是注释则失败 */
			if (xmlStrncmp(p, (xmlChar *) "--", 2) != 0)
				return false;
			/* 找注释结尾：找 -- 且其后必有 > */
			p = xmlStrstr(p + 2, (xmlChar *) "--");
			if (!p || p[2] != '>')
				return false;
			/* 越过注释并继续扫描 */
			p += 3;
			continue;
		}

		/* 否则，若不是 PI <?target something?> 则失败 */
		if (*p != '?')
			return false;
		p++;

		/* 找 PI 结尾（PI 内禁止出现 ?> 字符串） */
		e = xmlStrstr(p, (xmlChar *) "?>");
		if (!e)
			return false;

		/* 越过 PI 并继续扫描 */
		p = e + 2;
	}
}


/*
 * 把 text 对象转换为 XML 内部表示
 *
 * data 为源数据（不得为已 TOAST 的！），encoding 为其
 * 编码，xmloption_arg 与 preserve_whitespace 是转换
 * 的选项。
 *
 * 若 parsed_xmloptiontype 非 NULL，*parsed_xmloptiontype 将被
 * 设为实际用于解析输入的 XmlOptionType（通常与
 * xmloption_arg 相同，有 DOCTYPE 时可强制 DOCUMENT 模式）。
 *
 * 若 parsed_nodes 非 NULL 且按 CONTENT 模式解析，
 * xmlParseBalancedChunkMemory 调用所解析出的节点列表将
 * 返回到 *parsed_nodes。（释放它是调用方责任。）
 *
 * 错误通常导致 ereport(ERROR)，但若 escontext 是
 * ErrorSaveContext，则"安全"错误改在那里报告，调用方
 * 必须检查 SOFT_ERROR_OCCURRED() 来确认是否发生了此事。
 *
 * 注意：对结果调用 xmlFreeDoc() 是调用方的责任，
 * 否则将永久泄漏内存！但注意软错误后结果可能
 * 为 NULL。
 *
 * TODO 或许 libxml2 的 xmlreader 更好？(不构建 DOM，
 * 也不用 SAX —— 见 xmlreader.c)
 */
static xmlDocPtr
xml_parse(text *data, XmlOptionType xmloption_arg,
		  bool preserve_whitespace, int encoding,
		  XmlOptionType *parsed_xmloptiontype, xmlNodePtr *parsed_nodes,
		  Node *escontext)
{
	int32		len;
	xmlChar    *string;
	xmlChar    *utf8string;
	PgXmlErrorContext *xmlerrcxt;
	volatile xmlParserCtxtPtr ctxt = NULL;
	volatile xmlDocPtr doc = NULL;
	volatile int save_keep_blanks = -1;

	/*
	 * 这步看似多余，但为了在无需编码转换时也得到
	 * 空字符结尾字符串而必须这样做。
	 */
	len = VARSIZE_ANY_EXHDR(data);	/* 稍后会用到 */
	string = xml_text2xmlChar(data);

	/*
	 * 若数据非 UTF8，必须在交给 libxml 之前先做转换。
	 *
	 * XXX 理想情况是捕获任何编码转换失败并返回软错
	 * 误。不过转换到 UTF8 失败应该相当罕见，所以暂不
	 * 处理。
	 */
	utf8string = pg_do_encoding_conversion(string,
										   len,
										   encoding,
										   PG_UTF8);

	/* 启动 libxml 和解析器 */
	xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_WELLFORMED);

	/* 用 TRY 块来确保正确清理 */
	PG_TRY();
	{
		bool		parse_as_document = false;
		int			res_code;
		size_t		count = 0;
		xmlChar    *version = NULL;
		int			standalone = 0;

		/* 此处任何错误都按硬 ereport 报告 */
		xmlInitParser();

		/* 决定按 DOCUMENT 还是 CONTENT 解析 */
		if (xmloption_arg == XMLOPTION_DOCUMENT)
			parse_as_document = true;
		else
		{
			/* 解析并跳过 XML 声明（如有） */
			res_code = parse_xml_decl(utf8string,
									  &count, &version, NULL, &standalone);
			if (res_code != 0)
			{
				errsave(escontext,
						errcode(ERRCODE_INVALID_XML_CONTENT),
						errmsg_internal("invalid XML content: invalid XML declaration"),
						errdetail_for_xml_code(res_code));
				goto fail;
			}

			/* 有 DOCTYPE 元素吗 */
			if (xml_doctype_in_content(utf8string + count))
				parse_as_document = true;
		}

		/* 初始化输出参数 */
		if (parsed_xmloptiontype != NULL)
			*parsed_xmloptiontype = parse_as_document ? XMLOPTION_DOCUMENT :
				XMLOPTION_CONTENT;
		if (parsed_nodes != NULL)
			*parsed_nodes = NULL;

		if (parse_as_document)
		{
			int			options;

			/* 配置 xmlCtxtReadDoc 用解析器上下文 */
			ctxt = xmlNewParserCtxt();
			if (ctxt == NULL || xmlerrcxt->err_occurred)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
							"could not allocate parser context");

			/*
			 * 选择解析选项。
			 *
			 * 注意这里尝试应用 DTD 默认值(XML_PARSE_DTDATTR)
			 * 依据 SQL/XML:2008 GR 10.16.7.d：'内部 DTD 定义的默
			 * 认值会被应用'。至于外部 DTD 也尝试支持(见
			 * SQL/XML:2008 GR 10.16.7.e)，实际不会发生，因为
			 * xmlPgEntityLoader 会阻止它。
			 */
			options = XML_PARSE_NOENT | XML_PARSE_DTDATTR
				| (preserve_whitespace ? 0 : XML_PARSE_NOBLANKS);

			doc = xmlCtxtReadDoc(ctxt, utf8string,
								 NULL,	/* 无URL */
								 "UTF-8",
								 options);

			if (doc == NULL || xmlerrcxt->err_occurred)
			{
				/* 用原始选项决定报告哪个错误码 */
				if (xmloption_arg == XMLOPTION_DOCUMENT)
					xml_errsave(escontext, xmlerrcxt,
								ERRCODE_INVALID_XML_DOCUMENT,
								"invalid XML document");
				else
					xml_errsave(escontext, xmlerrcxt,
								ERRCODE_INVALID_XML_CONTENT,
								"invalid XML content");
				goto fail;
			}
		}
		else
		{
			/* 设置 xmlParseBalancedChunkMemory 将加入的文档 */
			doc = xmlNewDoc(version);
			if (doc == NULL || xmlerrcxt->err_occurred)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
							"could not allocate XML document");

			Assert(doc->encoding == NULL);
			doc->encoding = xmlStrdup((const xmlChar *) "UTF-8");
			if (doc->encoding == NULL || xmlerrcxt->err_occurred)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
							"could not allocate XML document");
			doc->standalone = standalone;

			/* 设置解析选项 --- 只好采用丑陋做法 */
			save_keep_blanks = xmlKeepBlanksDefault(preserve_whitespace ? 1 : 0);

			/* 允许空 content */
			if (*(utf8string + count))
			{
				res_code = xmlParseBalancedChunkMemory(doc, NULL, NULL, 0,
													   utf8string + count,
													   parsed_nodes);
				if (res_code != 0 || xmlerrcxt->err_occurred)
				{
					xml_errsave(escontext, xmlerrcxt,
								ERRCODE_INVALID_XML_CONTENT,
								"invalid XML content");
					goto fail;
				}
			}
		}

fail:
		;
	}
	PG_CATCH();
	{
		if (save_keep_blanks != -1)
			xmlKeepBlanksDefault(save_keep_blanks);
		if (doc != NULL)
			xmlFreeDoc(doc);
		if (ctxt != NULL)
			xmlFreeParserCtxt(ctxt);

		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	if (save_keep_blanks != -1)
		xmlKeepBlanksDefault(save_keep_blanks);

	if (ctxt != NULL)
		xmlFreeParserCtxt(ctxt);

	pg_xml_done(xmlerrcxt, false);

	return doc;
}


/*
 * xmlChar<->text 转换
 */
static xmlChar *
xml_text2xmlChar(text *in)
{
	return (xmlChar *) text_to_cstring(in);
}


#ifdef USE_LIBXMLCONTEXT

/*
 * 管理 libxml 分配所用的特殊上下文（仅特殊调试构建
 * 时启用；见文件顶部注释）。
 */
static void
xml_memory_init(void)
{
	/* 若尚未存在则创建内存上下文 */
	if (LibxmlContext == NULL)
		LibxmlContext = AllocSetContextCreate(TopMemoryContext,
											  "Libxml context",
											  ALLOCSET_DEFAULT_SIZES);

	/* 即使已设置也要重新建立回调 */
	xmlMemSetup(xml_pfree, xml_palloc, xml_repalloc, xml_pstrdup);
}

/*
 * 内存管理函数的包装器
 */
static void *
xml_palloc(size_t size)
{
	return MemoryContextAlloc(LibxmlContext, size);
}


static void *
xml_repalloc(void *ptr, size_t size)
{
	return repalloc(ptr, size);
}


static void
xml_pfree(void *ptr)
{
	/* libxml 至少部分假定 xmlFree(NULL) 是允许的 */
	if (ptr)
		pfree(ptr);
}


static char *
xml_pstrdup(const char *string)
{
	return MemoryContextStrdup(LibxmlContext, string);
}
#endif							/* USE_LIBXMLCONTEXT */


/*
 * xmlPgEntityLoader --- 实体加载器回调函数
 *
 * 静默阻止任何外部实体 URL 被加载。我们不想抛出错误，
 * 所以改为让实体看似展开成空
 * 串。
 *
 * 我们其实更愿意允许加载存在于系统 XML 目录的实
 * 体；但现有 libxml2 API 使之成为复杂而脆弱的任务。
 * 目前，直接关闭所有外部访问。
 */
static xmlParserInputPtr
xmlPgEntityLoader(const char *URL, const char *ID,
				  xmlParserCtxtPtr ctxt)
{
	return xmlNewStringInputStream(ctxt, (const xmlChar *) "");
}


/*
 * xml_ereport --- 报告 XML 相关错误
 *
 * "msg" 是 SQL 级的消息；有些可以沿用 SQL/XML 标准的。
 * 本函数把 libxml 原生错误消息（若有）作为 detail 附
 * 加。
 *
 * 该函数导出给希望共享核心 libxml 错误处理器的模
 * 块。注意此前必须已调用过 pg_xml_init()。
 */
void
xml_ereport(PgXmlErrorContext *errcxt, int level, int sqlcode, const char *msg)
{
	char	   *detail;

	/* 防止有人给我们传入伪造的上下文结构 */
	if (errcxt->magic != ERRCXT_MAGIC)
		elog(ERROR, "xml_ereport called with invalid PgXmlErrorContext");

	/* 标记当前 libxml 错误已被报告 */
	errcxt->err_occurred = false;

	/* 仅当有 libxml 文本时才包含 detail */
	if (errcxt->err_buf.len > 0)
		detail = errcxt->err_buf.data;
	else
		detail = NULL;

	ereport(level,
			(errcode(sqlcode),
			 errmsg_internal("%s", msg),
			 detail ? errdetail_internal("%s", detail) : 0));
}


/*
 * xml_errsave --- 保存 XML 相关错误
 *
 * 若 escontext 是 ErrorSaveContext，错误详情保存进去，
 * 然后控制正常返回。
 *
 * 否则错误被抛出，因此这等价于 level == ERROR
 * 的 xml_ereport()。
 *
 * 只应用于我们确定不需要事务中止来清理善后的
 * 错误。
 */
static void
xml_errsave(Node *escontext, PgXmlErrorContext *errcxt,
			int sqlcode, const char *msg)
{
	char	   *detail;

	/* 防止有人给我们传入伪造的上下文结构 */
	if (errcxt->magic != ERRCXT_MAGIC)
		elog(ERROR, "xml_errsave called with invalid PgXmlErrorContext");

	/* 标记当前 libxml 错误已被报告 */
	errcxt->err_occurred = false;

	/* 仅当有 libxml 文本时才包含 detail */
	if (errcxt->err_buf.len > 0)
		detail = errcxt->err_buf.data;
	else
		detail = NULL;

	errsave(escontext,
			(errcode(sqlcode),
			 errmsg_internal("%s", msg),
			 detail ? errdetail_internal("%s", detail) : 0));
}


/*
 * libxml 错误与警告的错误处理器
 */
static void
xml_errorHandler(void *data, PgXmlErrorPtr error)
{
	PgXmlErrorContext *xmlerrcxt = (PgXmlErrorContext *) data;
	xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) error->ctxt;
	xmlParserInputPtr input = (ctxt != NULL) ? ctxt->input : NULL;
	xmlNodePtr	node = error->node;
	const xmlChar *name = (node != NULL &&
						   node->type == XML_ELEMENT_NODE) ? node->name : NULL;
	int			domain = error->domain;
	int			level = error->level;
	StringInfo	errorBuf;

	/*
	 * 防止有人给我们传入伪造的上下文结构。
	 *
	 * 若此检查失败则强制后端退出，因为从 libxml 里
	 * longjmp 跳出可能使其此后不安全。
	 */
	if (xmlerrcxt->magic != ERRCXT_MAGIC)
		elog(FATAL, "xml_errorHandler called with invalid PgXmlErrorContext");

	/*----------
	 * 旧版 libxml 对某些错误的报告方式不同。
	 * 首先，一些错误此前报告来自解析器域，现在报告自
	 * 命名空间域。
	 * 其次，部分警告升级为错误。我们
	 * 试图在此补偿这种差异。
	 *----------
	 */
	switch (error->code)
	{
		case XML_WAR_NS_URI:
			level = XML_ERR_ERROR;
			domain = XML_FROM_NAMESPACE;
			break;

		case XML_ERR_NS_DECL_ERROR:
		case XML_WAR_NS_URI_RELATIVE:
		case XML_WAR_NS_COLUMN:
		case XML_NS_ERR_XML_NAMESPACE:
		case XML_NS_ERR_UNDEFINED_NAMESPACE:
		case XML_NS_ERR_QNAME:
		case XML_NS_ERR_ATTRIBUTE_REDEFINED:
		case XML_NS_ERR_EMPTY:
			domain = XML_FROM_NAMESPACE;
			break;
	}

	/* 决定是否对该错误采取行动 */
	switch (domain)
	{
		case XML_FROM_PARSER:

			/*
			 * XML_ERR_NOT_WELL_BALANCED 通常是在某个其他更
			 * 切题的错误之后才被报告。此外，libxml2 2.13 报
			 * 告它依据的规则集与旧版完全不同。为免
			 * 跨版本行为差异，只要已记录过某个错误就
			 * 抑制它。
			 */
			if (error->code == XML_ERR_NOT_WELL_BALANCED &&
				xmlerrcxt->err_occurred)
				return;
			/* fall through */

		case XML_FROM_NONE:
		case XML_FROM_MEMORY:
		case XML_FROM_IO:

			/*
			 * 抑制未声明实体警告。这样做以免因不加载
			 * DTD 定义而产生问题。
			 */
			if (error->code == XML_WAR_UNDECLARED_ENTITY)
				return;

			/* 否则，无论解析目的如何都接受该错误 */
			break;

		default:
			/* 若只做良构性检查则忽略错误 */
			if (xmlerrcxt->strictness == PG_XML_STRICTNESS_WELLFORMED)
				return;
			break;
	}

	/* 在 errorBuf 写错误消息 */
	errorBuf = makeStringInfo();

	if (error->line > 0)
		appendStringInfo(errorBuf, "line %d: ", error->line);
	if (name != NULL)
		appendStringInfo(errorBuf, "element %s: ", name);
	if (error->message != NULL)
		appendStringInfoString(errorBuf, error->message);
	else
		appendStringInfoString(errorBuf, "(no message provided)");

	/*
	 * 向 errorBuf 追加上下文信息。
	 *
	 * xmlParserPrintFileContext() 用 libxml 的"通用"错误处理器
	 * 来写上下文。不想重复 libxml 功能，所以暂时
	 * 在此临时设置一个通用错误处理器。
	 *
	 * 我们直接把 appendStringInfo() 用作 libxml 的通用错误处
	 * 理器。这应当可行，因为其签名与 libxml 所期望的
	 * 基本一致，即 (void *ptr, const char *msg, ...)。
	 */
	if (input != NULL)
	{
		xmlGenericErrorFunc errFuncSaved = xmlGenericError;
		void	   *errCtxSaved = xmlGenericErrorContext;

		xmlSetGenericErrorFunc((void *) errorBuf,
							   (xmlGenericErrorFunc) appendStringInfo);

		/* 向 errorBuf 加上下文信息 */
		appendStringInfoLineSeparator(errorBuf);

		xmlParserPrintFileContext(input);

		/* 恢复通用错误函数 */
		xmlSetGenericErrorFunc(errCtxSaved, errFuncSaved);
	}

	/* 去掉 errorBuf 中的所有尾随换行 */
	chopStringInfoNewlines(errorBuf);

	/*
	 * 旧式错误处理模式。err_occurred 从不设置，只把消息加
	 * 入 err_buf。此模式存在的原因是 xml2 contrib 模块用我
	 * 们的错误处理设施，但我们不想改变其行为，因它已
	 * 被弃用。这也是我们不在此区分 notice、warning 与
	 *  error 的原因 --- 旧式通用错误处理器本来也不会
	 * 做这种区分。
	 */
	if (xmlerrcxt->strictness == PG_XML_STRICTNESS_LEGACY)
	{
		appendStringInfoLineSeparator(&xmlerrcxt->err_buf);
		appendBinaryStringInfo(&xmlerrcxt->err_buf, errorBuf->data,
							   errorBuf->len);

		pfree(errorBuf->data);
		pfree(errorBuf);
		return;
	}

	/*
	 * 我们不想在此 ereport()，因为那很可能让 libxml 处于不
	 * 一致状态。作为替代，我们记住该错误，并交由
	 * xml_ereport()。
	 *
	 * 警告和通知可以立即报告，因为它们不会导致从 libxml
	 * 中 longjmp() 跳出。
	 */
	if (level >= XML_ERR_ERROR)
	{
		appendStringInfoLineSeparator(&xmlerrcxt->err_buf);
		appendBinaryStringInfo(&xmlerrcxt->err_buf, errorBuf->data,
							   errorBuf->len);

		xmlerrcxt->err_occurred = true;
	}
	else if (level >= XML_ERR_WARNING)
	{
		ereport(WARNING,
				(errmsg_internal("%s", errorBuf->data)));
	}
	else
	{
		ereport(NOTICE,
				(errmsg_internal("%s", errorBuf->data)));
	}

	pfree(errorBuf->data);
	pfree(errorBuf);
}


/*
 * 把 libxml 错误码转换为文本 errdetail 消息。
 *
 * 应当在 ereport 或 errsave 调用内部调用本函数，
 * 就像 errdetail 那样。
 *
 * 目前我们只需覆盖那些本文件可能
 * 引发的错误码。
 */
static int
errdetail_for_xml_code(int code)
{
	const char *det;

	switch (code)
	{
		case XML_ERR_INVALID_CHAR:
			det = gettext_noop("Invalid character value.");
			break;
		case XML_ERR_SPACE_REQUIRED:
			det = gettext_noop("Space required.");
			break;
		case XML_ERR_STANDALONE_VALUE:
			det = gettext_noop("standalone accepts only 'yes' or 'no'.");
			break;
		case XML_ERR_VERSION_MISSING:
			det = gettext_noop("Malformed declaration: missing version.");
			break;
		case XML_ERR_MISSING_ENCODING:
			det = gettext_noop("Missing encoding in text declaration.");
			break;
		case XML_ERR_XMLDECL_NOT_FINISHED:
			det = gettext_noop("Parsing XML declaration: '?>' expected.");
			break;
		default:
			det = gettext_noop("Unrecognized libxml error code: %d.");
			break;
	}

	return errdetail(det, code);
}


/*
 * 从 StringInfo 字符串移除全部尾随换行
 */
static void
chopStringInfoNewlines(StringInfo str)
{
	while (str->len > 0 && str->data[str->len - 1] == '\n')
		str->data[--str->len] = '\0';
}


/*
 * 先移除已有的尾随换行，再附加一个换行
 */
static void
appendStringInfoLineSeparator(StringInfo str)
{
	chopStringInfoNewlines(str);
	if (str->len > 0)
		appendStringInfoChar(str, '\n');
}


/*
 * 把当前服务器编码中的一个字符转换成 Unicode 码点。
 */
static pg_wchar
sqlchar_to_unicode(const char *s)
{
	char	   *utf8string;
	pg_wchar	ret[2];			/* 需要结尾零的空间 */

	/* 注意我们不假定 s 以空字符结尾 */
	utf8string = pg_server_to_any(s, pg_mblen(s), PG_UTF8);

	pg_encoding_mb2wchar_with_len(PG_UTF8, utf8string, ret,
								  pg_encoding_mblen(PG_UTF8, utf8string));

	if (utf8string != s)
		pfree(utf8string);

	return ret[0];
}


static bool
is_valid_xml_namefirst(pg_wchar c)
{
	/* (Letter | '_' | ':') */
	return (xmlIsBaseCharQ(c) || xmlIsIdeographicQ(c)
			|| c == '_' || c == ':');
}


static bool
is_valid_xml_namechar(pg_wchar c)
{
	/* Letter | Digit | '.' | '-' | '_' | ':' | CombiningChar | Extender */
	return (xmlIsBaseCharQ(c) || xmlIsIdeographicQ(c)
			|| xmlIsDigitQ(c)
			|| c == '.' || c == '-' || c == '_' || c == ':'
			|| xmlIsCombiningQ(c)
			|| xmlIsExtenderQ(c));
}
#endif							/* USE_LIBXML */


/*
 * SQL 标识符映射为 XML 名；见 SQL/XML:2008 9.1 节。
 */
char *
map_sql_identifier_to_xml_name(const char *ident, bool fully_escaped,
							   bool escape_period)
{
#ifdef USE_LIBXML
	StringInfoData buf;
	const char *p;

	/*
	 * SQL/XML 未在任何处使用此情形，多半是个错
	 * 误。
	 */
	Assert(fully_escaped || !escape_period);

	initStringInfo(&buf);

	for (p = ident; *p; p += pg_mblen(p))
	{
		if (*p == ':' && (p == ident || fully_escaped))
			appendStringInfoString(&buf, "_x003A_");
		else if (*p == '_' && *(p + 1) == 'x')
			appendStringInfoString(&buf, "_x005F_");
		else if (fully_escaped && p == ident &&
				 pg_strncasecmp(p, "xml", 3) == 0)
		{
			if (*p == 'x')
				appendStringInfoString(&buf, "_x0078_");
			else
				appendStringInfoString(&buf, "_x0058_");
		}
		else if (escape_period && *p == '.')
			appendStringInfoString(&buf, "_x002E_");
		else
		{
			pg_wchar	u = sqlchar_to_unicode(p);

			if ((p == ident)
				? !is_valid_xml_namefirst(u)
				: !is_valid_xml_namechar(u))
				appendStringInfo(&buf, "_x%04X_", (unsigned int) u);
			else
				appendBinaryStringInfo(&buf, p, pg_mblen(p));
		}
	}

	return buf.data;
#else							/* not USE_LIBXML */
	NO_XML_SUPPORT();
	return NULL;
#endif							/* not USE_LIBXML */
}


/*
 * XML 名映射为 SQL 标识符；见 SQL/XML:2008 9.3 节。
 */
char *
map_xml_name_to_sql_identifier(const char *name)
{
	StringInfoData buf;
	const char *p;

	initStringInfo(&buf);

	for (p = name; *p; p += pg_mblen(p))
	{
		if (*p == '_' && *(p + 1) == 'x'
			&& isxdigit((unsigned char) *(p + 2))
			&& isxdigit((unsigned char) *(p + 3))
			&& isxdigit((unsigned char) *(p + 4))
			&& isxdigit((unsigned char) *(p + 5))
			&& *(p + 6) == '_')
		{
			char		cbuf[MAX_UNICODE_EQUIVALENT_STRING + 1];
			unsigned int u;

			sscanf(p + 2, "%X", &u);
			pg_unicode_to_server(u, (unsigned char *) cbuf);
			appendStringInfoString(&buf, cbuf);
			p += 6;
		}
		else
			appendBinaryStringInfo(&buf, p, pg_mblen(p));
	}

	return buf.data;
}

/*
 * SQL 值映射为 XML 值；见 SQL/XML:2008 9.8 节。
 *
 * 当 xml_escape_strings 为 true 时，字符串值中某些字符
 * 会被实体引用（&lt; 等）替换，如 SQL/XML:2008 9.8 节
 * GR 9) a) iii) 所规定。这通常正是想要的。false 情形
 * 主要用于结果值要与 xmlTextWriterWriteAttribute()
 * 一起使用来写出属性时，该函数会自行
 * 做转义。
 */
char *
map_sql_value_to_xml_value(Datum value, Oid type, bool xml_escape_strings)
{
	if (type_is_array_domain(type))
	{
		ArrayType  *array;
		Oid			elmtype;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			num_elems;
		Datum	   *elem_values;
		bool	   *elem_nulls;
		StringInfoData buf;
		int			i;

		array = DatumGetArrayTypeP(value);
		elmtype = ARR_ELEMTYPE(array);
		get_typlenbyvalalign(elmtype, &elmlen, &elmbyval, &elmalign);

		deconstruct_array(array, elmtype,
						  elmlen, elmbyval, elmalign,
						  &elem_values, &elem_nulls,
						  &num_elems);

		initStringInfo(&buf);

		for (i = 0; i < num_elems; i++)
		{
			if (elem_nulls[i])
				continue;
			appendStringInfoString(&buf, "<element>");
			appendStringInfoString(&buf,
								   map_sql_value_to_xml_value(elem_values[i],
															  elmtype, true));
			appendStringInfoString(&buf, "</element>");
		}

		pfree(elem_values);
		pfree(elem_nulls);

		return buf.data;
	}
	else
	{
		Oid			typeOut;
		bool		isvarlena;
		char	   *str;

		/*
		 * 展平域；下方特殊情形处理应当也适用于，比如说
		 * boolean 上的域而不仅是 boolean。
		 */
		type = getBaseType(type);

		/*
		 * 某些数据类型的特殊 XSD 格式化
		 */
		switch (type)
		{
			case BOOLOID:
				if (DatumGetBool(value))
					return "true";
				else
					return "false";

			case DATEOID:
				{
					DateADT		date;
					struct pg_tm tm;
					char		buf[MAXDATELEN + 1];

					date = DatumGetDateADT(value);
					/* XSD 不支持无限值 */
					if (DATE_NOT_FINITE(date))
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("date out of range"),
								 errdetail("XML does not support infinite date values.")));
					j2date(date + POSTGRES_EPOCH_JDATE,
						   &(tm.tm_year), &(tm.tm_mon), &(tm.tm_mday));
					EncodeDateOnly(&tm, USE_XSD_DATES, buf);

					return pstrdup(buf);
				}

			case TIMESTAMPOID:
				{
					Timestamp	timestamp;
					struct pg_tm tm;
					fsec_t		fsec;
					char		buf[MAXDATELEN + 1];

					timestamp = DatumGetTimestamp(value);

					/* XSD 不支持无限值 */
					if (TIMESTAMP_NOT_FINITE(timestamp))
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("timestamp out of range"),
								 errdetail("XML does not support infinite timestamp values.")));
					else if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, NULL) == 0)
						EncodeDateTime(&tm, fsec, false, 0, NULL, USE_XSD_DATES, buf);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("timestamp out of range")));

					return pstrdup(buf);
				}

			case TIMESTAMPTZOID:
				{
					TimestampTz timestamp;
					struct pg_tm tm;
					int			tz;
					fsec_t		fsec;
					const char *tzn = NULL;
					char		buf[MAXDATELEN + 1];

					timestamp = DatumGetTimestamp(value);

					/* XSD 不支持无限值 */
					if (TIMESTAMP_NOT_FINITE(timestamp))
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("timestamp out of range"),
								 errdetail("XML does not support infinite timestamp values.")));
					else if (timestamp2tm(timestamp, &tz, &tm, &fsec, &tzn, NULL) == 0)
						EncodeDateTime(&tm, fsec, true, tz, tzn, USE_XSD_DATES, buf);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("timestamp out of range")));

					return pstrdup(buf);
				}

#ifdef USE_LIBXML
			case BYTEAOID:
				{
					bytea	   *bstr = DatumGetByteaPP(value);
					PgXmlErrorContext *xmlerrcxt;
					volatile xmlBufferPtr buf = NULL;
					volatile xmlTextWriterPtr writer = NULL;
					char	   *result;

					xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

					PG_TRY();
					{
						buf = xmlBufferCreate();
						if (buf == NULL || xmlerrcxt->err_occurred)
							xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
										"could not allocate xmlBuffer");
						writer = xmlNewTextWriterMemory(buf, 0);
						if (writer == NULL || xmlerrcxt->err_occurred)
							xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
										"could not allocate xmlTextWriter");

						if (xmlbinary == XMLBINARY_BASE64)
							xmlTextWriterWriteBase64(writer, VARDATA_ANY(bstr),
													 0, VARSIZE_ANY_EXHDR(bstr));
						else
							xmlTextWriterWriteBinHex(writer, VARDATA_ANY(bstr),
													 0, VARSIZE_ANY_EXHDR(bstr));

						/* 现在必须这么做，把数据刷到缓冲区 */
						xmlFreeTextWriter(writer);
						writer = NULL;

						result = pstrdup((const char *) xmlBufferContent(buf));
					}
					PG_CATCH();
					{
						if (writer)
							xmlFreeTextWriter(writer);
						if (buf)
							xmlBufferFree(buf);

						pg_xml_done(xmlerrcxt, true);

						PG_RE_THROW();
					}
					PG_END_TRY();

					xmlBufferFree(buf);

					pg_xml_done(xmlerrcxt, false);

					return result;
				}
#endif							/* USE_LIBXML */

		}

		/*
		 * 否则，直接使用该类型的原生文本表示
		 */
		getTypeOutputInfo(type, &typeOut, &isvarlena);
		str = OidOutputFunctionCall(typeOut, value);

		/* ……对 XML 保持原样，且不想要转义时 */
		if (type == XMLOID || !xml_escape_strings)
			return str;

		/* 否则按需转义特殊字符 */
		return escape_xml(str);
	}
}


/*
 * 转义 text 中在 XML 里有特殊含义的字符。
 *
 * 返回 palloc 字符串。
 *
 * 注意：这里有意不依赖 libxml。
 */
char *
escape_xml(const char *str)
{
	StringInfoData buf;
	const char *p;

	initStringInfo(&buf);
	for (p = str; *p; p++)
	{
		switch (*p)
		{
			case '&':
				appendStringInfoString(&buf, "&amp;");
				break;
			case '<':
				appendStringInfoString(&buf, "&lt;");
				break;
			case '>':
				appendStringInfoString(&buf, "&gt;");
				break;
			case '\r':
				appendStringInfoString(&buf, "&#x0d;");
				break;
			default:
				appendStringInfoCharMacro(&buf, *p);
				break;
		}
	}
	return buf.data;
}


static char *
_SPI_strdup(const char *s)
{
	size_t		len = strlen(s) + 1;
	char	   *ret = SPI_palloc(len);

	memcpy(ret, s, len);
	return ret;
}


/*
 * SQL 到 XML 的映射函数
 *
 * 以下内容曾一度有意加以组织，使得你可以顺着
 *  SQL/XML 标准阅读。这些函数大多按条款在标准
 * 文档中的排布拆分，标识符也与标准正文
 * 对齐。遗憾的是，SQL/XML:2006 对条款的排序
 * 不同于 SQL/XML:2003，所以下面的顺序已没
 * 有多大意
 * 义了。
 *
 * 其中发生着很多事情：
 *
 * 映射有两类：把 SQL 数据（表内容）映射成 XML 文
 * 档，以及把 SQL 结构（"模式"）映射为 XML Schema。
 * 还有一些函数同时做这两件事。
 *
 * 然后能够映射数据库、模式或表，各自按
 * 这两种方式进行。这会递归分解：映射数据库
 * 引发映射模式，后者引发映射表，再引发映
 * 射行与映射列，只是最后两者不能从外部调
 * 用。正因如此，有许多 xyz_internal() 函数既要
 * 从函数管理器包装器来调用，也要从递归调用
 * 里的某个上层来调
 * 用。
 *
 * 关于公共函数参数 nulls、tableforest 和
 * targetns 的含义请见文档。
 *
 * XML 输出的一些风格准则：引用 XML 属性时要用双
 * 引号。XML 元素缩进两空格；许多代码会在不同
 * 层级上递归调用，因此宁可不缩进，也不要造
 * 出缩进与退格怪异的输出。加换行让输
 * 出好看
 */


/*
 * XML 映射的对象可见性；见 SQL/XML:2008 第 4.10.8
 * 节。
 */

/*
 * 给定一个首列须返回 oid 类型的查询，产生一个
 * 含查询结果的 Oid 列表。
 */
static List *
query_to_oid_list(const char *query)
{
	uint64		i;
	List	   *list = NIL;
	int			spi_result;

	spi_result = SPI_execute(query, true, 0);
	if (spi_result != SPI_OK_SELECT)
		elog(ERROR, "SPI_execute returned %s for %s",
			 SPI_result_code_string(spi_result), query);

	for (i = 0; i < SPI_processed; i++)
	{
		Datum		oid;
		bool		isnull;

		oid = SPI_getbinval(SPI_tuptable->vals[i],
							SPI_tuptable->tupdesc,
							1,
							&isnull);
		if (!isnull)
			list = lappend_oid(list, DatumGetObjectId(oid));
	}

	return list;
}


static List *
schema_get_xml_visible_tables(Oid nspid)
{
	StringInfoData query;

	initStringInfo(&query);
	appendStringInfo(&query, "SELECT oid FROM pg_catalog.pg_class"
					 " WHERE relnamespace = %u AND relkind IN ("
					 CppAsString2(RELKIND_RELATION) ","
					 CppAsString2(RELKIND_MATVIEW) ","
					 CppAsString2(RELKIND_VIEW) ")"
					 " AND pg_catalog.has_table_privilege (oid, 'SELECT')"
					 " ORDER BY relname;", nspid);

	return query_to_oid_list(query.data);
}


/*
 * 就数据库映射而言，包含系统模式多半没有用
 * 处。
 */
#define XML_VISIBLE_SCHEMAS_EXCLUDE "(nspname ~ '^pg_' OR nspname = 'information_schema')"

#define XML_VISIBLE_SCHEMAS "SELECT oid FROM pg_catalog.pg_namespace WHERE pg_catalog.has_schema_privilege (oid, 'USAGE') AND NOT " XML_VISIBLE_SCHEMAS_EXCLUDE


static List *
database_get_xml_visible_schemas(void)
{
	return query_to_oid_list(XML_VISIBLE_SCHEMAS " ORDER BY nspname;");
}


static List *
database_get_xml_visible_tables(void)
{
	/* 目前这里不需要任何排序。 */
	return query_to_oid_list("SELECT oid FROM pg_catalog.pg_class"
							 " WHERE relkind IN ("
							 CppAsString2(RELKIND_RELATION) ","
							 CppAsString2(RELKIND_MATVIEW) ","
							 CppAsString2(RELKIND_VIEW) ")"
							 " AND pg_catalog.has_table_privilege(pg_class.oid, 'SELECT')"
							 " AND relnamespace IN (" XML_VISIBLE_SCHEMAS ");");
}


/*
 * 把 SQL 表映射为 XML 和/或 XML Schema 文档；见
 * 9.11 节。
 */

static StringInfo
table_to_xml_internal(Oid relid,
					  const char *xmlschema, bool nulls, bool tableforest,
					  const char *targetns, bool top_level)
{
	StringInfoData query;

	initStringInfo(&query);
	appendStringInfo(&query, "SELECT * FROM %s",
					 DatumGetCString(DirectFunctionCall1(regclassout,
														 ObjectIdGetDatum(relid))));
	return query_to_xml_internal(query.data, get_rel_name(relid),
								 xmlschema, nulls, tableforest,
								 targetns, top_level);
}


Datum
table_to_xml(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

	PG_RETURN_XML_P(stringinfo_to_xmltype(table_to_xml_internal(relid, NULL,
																nulls, tableforest,
																targetns, true)));
}


Datum
query_to_xml(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

	PG_RETURN_XML_P(stringinfo_to_xmltype(query_to_xml_internal(query, NULL,
																NULL, nulls, tableforest,
																targetns, true)));
}


Datum
cursor_to_xml(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int32		count = PG_GETARG_INT32(1);
	bool		nulls = PG_GETARG_BOOL(2);
	bool		tableforest = PG_GETARG_BOOL(3);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(4));

	StringInfoData result;
	Portal		portal;
	uint64		i;

	initStringInfo(&result);

	if (!tableforest)
	{
		xmldata_root_element_start(&result, "table", NULL, targetns, true);
		appendStringInfoChar(&result, '\n');
	}

	SPI_connect();
	portal = SPI_cursor_find(name);
	if (portal == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", name)));

	SPI_cursor_fetch(portal, true, count);
	for (i = 0; i < SPI_processed; i++)
		SPI_sql_row_to_xmlelement(i, &result, NULL, nulls,
								  tableforest, targetns, true);

	SPI_finish();

	if (!tableforest)
		xmldata_root_element_end(&result, "table");

	PG_RETURN_XML_P(stringinfo_to_xmltype(&result));
}


/*
 * 写出数据映射之根元素的开始标签。
 *
 * top_level 表示这是最终输出的最顶层。如用户调
 * table_to_xml 时，用表名对该函数的调用就是顶层。当
 * 用户调用 database_to_xml 时，用模式名对该函数的调
 * 用并非顶层。若 top_level 为 false，则 XML 命名
 * 空间声明会被省略，因为它们应已在此前输出中出
 * 现过。重复它们并不算错，只是看起来
 * 有点难
 * 看。
 */
static void
xmldata_root_element_start(StringInfo result, const char *eltname,
						   const char *xmlschema, const char *targetns,
						   bool top_level)
{
	/* 这其实不算错，但目前毫无意义。 */
	Assert(top_level || !xmlschema);

	appendStringInfo(result, "<%s", eltname);
	if (top_level)
	{
		appendStringInfoString(result, " xmlns:xsi=\"" NAMESPACE_XSI "\"");
		if (strlen(targetns) > 0)
			appendStringInfo(result, " xmlns=\"%s\"", targetns);
	}
	if (xmlschema)
	{
		/* FIXME:更佳目标 */
		if (strlen(targetns) > 0)
			appendStringInfo(result, " xsi:schemaLocation=\"%s #\"", targetns);
		else
			appendStringInfoString(result, " xsi:noNamespaceSchemaLocation=\"#\"");
	}
	appendStringInfoString(result, ">\n");
}


static void
xmldata_root_element_end(StringInfo result, const char *eltname)
{
	appendStringInfo(result, "</%s>\n", eltname);
}


static StringInfo
query_to_xml_internal(const char *query, char *tablename,
					  const char *xmlschema, bool nulls, bool tableforest,
					  const char *targetns, bool top_level)
{
	StringInfo	result;
	char	   *xmltn;
	uint64		i;

	if (tablename)
		xmltn = map_sql_identifier_to_xml_name(tablename, true, false);
	else
		xmltn = "table";

	result = makeStringInfo();

	SPI_connect();
	if (SPI_execute(query, true, 0) != SPI_OK_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("invalid query")));

	if (!tableforest)
	{
		xmldata_root_element_start(result, xmltn, xmlschema,
								   targetns, top_level);
		appendStringInfoChar(result, '\n');
	}

	if (xmlschema)
		appendStringInfo(result, "%s\n\n", xmlschema);

	for (i = 0; i < SPI_processed; i++)
		SPI_sql_row_to_xmlelement(i, result, tablename, nulls,
								  tableforest, targetns, top_level);

	if (!tableforest)
		xmldata_root_element_end(result, xmltn);

	SPI_finish();

	return result;
}


Datum
table_to_xmlschema(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
	const char *result;
	Relation	rel;

	rel = table_open(relid, AccessShareLock);
	result = map_sql_table_to_xmlschema(rel->rd_att, relid, nulls,
										tableforest, targetns);
	table_close(rel, NoLock);

	PG_RETURN_XML_P(cstring_to_xmltype(result));
}


Datum
query_to_xmlschema(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
	const char *result;
	SPIPlanPtr	plan;
	Portal		portal;

	SPI_connect();

	if ((plan = SPI_prepare(query, 0, NULL)) == NULL)
		elog(ERROR, "SPI_prepare(\"%s\") failed", query);

	if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, true)) == NULL)
		elog(ERROR, "SPI_cursor_open(\"%s\") failed", query);

	result = _SPI_strdup(map_sql_table_to_xmlschema(portal->tupDesc,
													InvalidOid, nulls,
													tableforest, targetns));
	SPI_cursor_close(portal);
	SPI_finish();

	PG_RETURN_XML_P(cstring_to_xmltype(result));
}


Datum
cursor_to_xmlschema(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
	const char *xmlschema;
	Portal		portal;

	SPI_connect();
	portal = SPI_cursor_find(name);
	if (portal == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", name)));
	if (portal->tupDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("portal \"%s\" does not return tuples", name)));

	xmlschema = _SPI_strdup(map_sql_table_to_xmlschema(portal->tupDesc,
													   InvalidOid, nulls,
													   tableforest, targetns));
	SPI_finish();

	PG_RETURN_XML_P(cstring_to_xmltype(xmlschema));
}


Datum
table_to_xml_and_xmlschema(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
	Relation	rel;
	const char *xmlschema;

	rel = table_open(relid, AccessShareLock);
	xmlschema = map_sql_table_to_xmlschema(rel->rd_att, relid, nulls,
										   tableforest, targetns);
	table_close(rel, NoLock);

	PG_RETURN_XML_P(stringinfo_to_xmltype(table_to_xml_internal(relid,
																xmlschema, nulls, tableforest,
																targetns, true)));
}


Datum
query_to_xml_and_xmlschema(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

	const char *xmlschema;
	SPIPlanPtr	plan;
	Portal		portal;

	SPI_connect();

	if ((plan = SPI_prepare(query, 0, NULL)) == NULL)
		elog(ERROR, "SPI_prepare(\"%s\") failed", query);

	if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, true)) == NULL)
		elog(ERROR, "SPI_cursor_open(\"%s\") failed", query);

	xmlschema = _SPI_strdup(map_sql_table_to_xmlschema(portal->tupDesc,
													   InvalidOid, nulls, tableforest, targetns));
	SPI_cursor_close(portal);
	SPI_finish();

	PG_RETURN_XML_P(stringinfo_to_xmltype(query_to_xml_internal(query, NULL,
																xmlschema, nulls, tableforest,
																targetns, true)));
}


/*
 * 把 SQL 模式映射为 XML 和/或 XML Schema 文档；见
 * 9.13、9.14 节。
 */

static StringInfo
schema_to_xml_internal(Oid nspid, const char *xmlschema, bool nulls,
					   bool tableforest, const char *targetns, bool top_level)
{
	StringInfo	result;
	char	   *xmlsn;
	List	   *relid_list;
	ListCell   *cell;

	xmlsn = map_sql_identifier_to_xml_name(get_namespace_name(nspid),
										   true, false);
	result = makeStringInfo();

	xmldata_root_element_start(result, xmlsn, xmlschema, targetns, top_level);
	appendStringInfoChar(result, '\n');

	if (xmlschema)
		appendStringInfo(result, "%s\n\n", xmlschema);

	SPI_connect();

	relid_list = schema_get_xml_visible_tables(nspid);

	foreach(cell, relid_list)
	{
		Oid			relid = lfirst_oid(cell);
		StringInfo	subres;

		subres = table_to_xml_internal(relid, NULL, nulls, tableforest,
									   targetns, false);

		appendBinaryStringInfo(result, subres->data, subres->len);
		appendStringInfoChar(result, '\n');
	}

	SPI_finish();

	xmldata_root_element_end(result, xmlsn);

	return result;
}


Datum
schema_to_xml(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

	char	   *schemaname;
	Oid			nspid;

	schemaname = NameStr(*name);
	nspid = LookupExplicitNamespace(schemaname, false);

	PG_RETURN_XML_P(stringinfo_to_xmltype(schema_to_xml_internal(nspid, NULL,
																 nulls, tableforest, targetns, true)));
}


/*
 * 写出 XML Schema 映射之根元素的开始元素。
 */
static void
xsd_schema_element_start(StringInfo result, const char *targetns)
{
	appendStringInfoString(result,
						   "<xsd:schema\n"
						   "    xmlns:xsd=\"" NAMESPACE_XSD "\"");
	if (strlen(targetns) > 0)
		appendStringInfo(result,
						 "\n"
						 "    targetNamespace=\"%s\"\n"
						 "    elementFormDefault=\"qualified\"",
						 targetns);
	appendStringInfoString(result,
						   ">\n\n");
}


static void
xsd_schema_element_end(StringInfo result)
{
	appendStringInfoString(result, "</xsd:schema>");
}


static StringInfo
schema_to_xmlschema_internal(const char *schemaname, bool nulls,
							 bool tableforest, const char *targetns)
{
	Oid			nspid;
	List	   *relid_list;
	List	   *tupdesc_list;
	ListCell   *cell;
	StringInfo	result;

	result = makeStringInfo();

	nspid = LookupExplicitNamespace(schemaname, false);

	xsd_schema_element_start(result, targetns);

	SPI_connect();

	relid_list = schema_get_xml_visible_tables(nspid);

	tupdesc_list = NIL;
	foreach(cell, relid_list)
	{
		Relation	rel;

		rel = table_open(lfirst_oid(cell), AccessShareLock);
		tupdesc_list = lappend(tupdesc_list, CreateTupleDescCopy(rel->rd_att));
		table_close(rel, NoLock);
	}

	appendStringInfoString(result,
						   map_sql_typecoll_to_xmlschema_types(tupdesc_list));

	appendStringInfoString(result,
						   map_sql_schema_to_xmlschema_types(nspid, relid_list,
															 nulls, tableforest, targetns));

	xsd_schema_element_end(result);

	SPI_finish();

	return result;
}


Datum
schema_to_xmlschema(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

	PG_RETURN_XML_P(stringinfo_to_xmltype(schema_to_xmlschema_internal(NameStr(*name),
																	   nulls, tableforest, targetns)));
}


Datum
schema_to_xml_and_xmlschema(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
	char	   *schemaname;
	Oid			nspid;
	StringInfo	xmlschema;

	schemaname = NameStr(*name);
	nspid = LookupExplicitNamespace(schemaname, false);

	xmlschema = schema_to_xmlschema_internal(schemaname, nulls,
											 tableforest, targetns);

	PG_RETURN_XML_P(stringinfo_to_xmltype(schema_to_xml_internal(nspid,
																 xmlschema->data, nulls,
																 tableforest, targetns, true)));
}


/*
 * 把 SQL 数据库映射为 XML 和/或 XML Schema 文档；见
 * 9.16、9.17 节。
 */

static StringInfo
database_to_xml_internal(const char *xmlschema, bool nulls,
						 bool tableforest, const char *targetns)
{
	StringInfo	result;
	List	   *nspid_list;
	ListCell   *cell;
	char	   *xmlcn;

	xmlcn = map_sql_identifier_to_xml_name(get_database_name(MyDatabaseId),
										   true, false);
	result = makeStringInfo();

	xmldata_root_element_start(result, xmlcn, xmlschema, targetns, true);
	appendStringInfoChar(result, '\n');

	if (xmlschema)
		appendStringInfo(result, "%s\n\n", xmlschema);

	SPI_connect();

	nspid_list = database_get_xml_visible_schemas();

	foreach(cell, nspid_list)
	{
		Oid			nspid = lfirst_oid(cell);
		StringInfo	subres;

		subres = schema_to_xml_internal(nspid, NULL, nulls,
										tableforest, targetns, false);

		appendBinaryStringInfo(result, subres->data, subres->len);
		appendStringInfoChar(result, '\n');
	}

	SPI_finish();

	xmldata_root_element_end(result, xmlcn);

	return result;
}


Datum
database_to_xml(PG_FUNCTION_ARGS)
{
	bool		nulls = PG_GETARG_BOOL(0);
	bool		tableforest = PG_GETARG_BOOL(1);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(2));

	PG_RETURN_XML_P(stringinfo_to_xmltype(database_to_xml_internal(NULL, nulls,
																   tableforest, targetns)));
}


static StringInfo
database_to_xmlschema_internal(bool nulls, bool tableforest,
							   const char *targetns)
{
	List	   *relid_list;
	List	   *nspid_list;
	List	   *tupdesc_list;
	ListCell   *cell;
	StringInfo	result;

	result = makeStringInfo();

	xsd_schema_element_start(result, targetns);

	SPI_connect();

	relid_list = database_get_xml_visible_tables();
	nspid_list = database_get_xml_visible_schemas();

	tupdesc_list = NIL;
	foreach(cell, relid_list)
	{
		Relation	rel;

		rel = table_open(lfirst_oid(cell), AccessShareLock);
		tupdesc_list = lappend(tupdesc_list, CreateTupleDescCopy(rel->rd_att));
		table_close(rel, NoLock);
	}

	appendStringInfoString(result,
						   map_sql_typecoll_to_xmlschema_types(tupdesc_list));

	appendStringInfoString(result,
						   map_sql_catalog_to_xmlschema_types(nspid_list, nulls, tableforest, targetns));

	xsd_schema_element_end(result);

	SPI_finish();

	return result;
}


Datum
database_to_xmlschema(PG_FUNCTION_ARGS)
{
	bool		nulls = PG_GETARG_BOOL(0);
	bool		tableforest = PG_GETARG_BOOL(1);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(2));

	PG_RETURN_XML_P(stringinfo_to_xmltype(database_to_xmlschema_internal(nulls,
																		 tableforest, targetns)));
}


Datum
database_to_xml_and_xmlschema(PG_FUNCTION_ARGS)
{
	bool		nulls = PG_GETARG_BOOL(0);
	bool		tableforest = PG_GETARG_BOOL(1);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(2));
	StringInfo	xmlschema;

	xmlschema = database_to_xmlschema_internal(nulls, tableforest, targetns);

	PG_RETURN_XML_P(stringinfo_to_xmltype(database_to_xml_internal(xmlschema->data,
																   nulls, tableforest, targetns)));
}


/*
 * 把多段 SQL 名映射为 XML 名；见 SQL/XML:2008
 * 9.2.
 */
static char *
map_multipart_sql_identifier_to_xml_name(const char *a, const char *b, const char *c, const char *d)
{
	StringInfoData result;

	initStringInfo(&result);

	if (a)
		appendStringInfoString(&result,
							   map_sql_identifier_to_xml_name(a, true, true));
	if (b)
		appendStringInfo(&result, ".%s",
						 map_sql_identifier_to_xml_name(b, true, true));
	if (c)
		appendStringInfo(&result, ".%s",
						 map_sql_identifier_to_xml_name(c, true, true));
	if (d)
		appendStringInfo(&result, ".%s",
						 map_sql_identifier_to_xml_name(d, true, true));

	return result.data;
}


/*
 * 把 SQL 表映射为 XML Schema 文档；见
 * 9.11 节。
 *
 * 把 SQL 表映射为 XML Schema 数据类型；见
 * 9.9.
 */
static const char *
map_sql_table_to_xmlschema(TupleDesc tupdesc, Oid relid, bool nulls,
						   bool tableforest, const char *targetns)
{
	int			i;
	char	   *xmltn;
	char	   *tabletypename;
	char	   *rowtypename;
	StringInfoData result;

	initStringInfo(&result);

	if (OidIsValid(relid))
	{
		HeapTuple	tuple;
		Form_pg_class reltuple;

		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", relid);
		reltuple = (Form_pg_class) GETSTRUCT(tuple);

		xmltn = map_sql_identifier_to_xml_name(NameStr(reltuple->relname),
											   true, false);

		tabletypename = map_multipart_sql_identifier_to_xml_name("TableType",
																 get_database_name(MyDatabaseId),
																 get_namespace_name(reltuple->relnamespace),
																 NameStr(reltuple->relname));

		rowtypename = map_multipart_sql_identifier_to_xml_name("RowType",
															   get_database_name(MyDatabaseId),
															   get_namespace_name(reltuple->relnamespace),
															   NameStr(reltuple->relname));

		ReleaseSysCache(tuple);
	}
	else
	{
		if (tableforest)
			xmltn = "row";
		else
			xmltn = "table";

		tabletypename = "TableType";
		rowtypename = "RowType";
	}

	xsd_schema_element_start(&result, targetns);

	appendStringInfoString(&result,
						   map_sql_typecoll_to_xmlschema_types(list_make1(tupdesc)));

	appendStringInfo(&result,
					 "<xsd:complexType name=\"%s\">\n"
					 "  <xsd:sequence>\n",
					 rowtypename);

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		appendStringInfo(&result,
						 "    <xsd:element name=\"%s\" type=\"%s\"%s></xsd:element>\n",
						 map_sql_identifier_to_xml_name(NameStr(att->attname),
														true, false),
						 map_sql_type_to_xml_name(att->atttypid, -1),
						 nulls ? " nillable=\"true\"" : " minOccurs=\"0\"");
	}

	appendStringInfoString(&result,
						   "  </xsd:sequence>\n"
						   "</xsd:complexType>\n\n");

	if (!tableforest)
	{
		appendStringInfo(&result,
						 "<xsd:complexType name=\"%s\">\n"
						 "  <xsd:sequence>\n"
						 "    <xsd:element name=\"row\" type=\"%s\" minOccurs=\"0\" maxOccurs=\"unbounded\"/>\n"
						 "  </xsd:sequence>\n"
						 "</xsd:complexType>\n\n",
						 tabletypename, rowtypename);

		appendStringInfo(&result,
						 "<xsd:element name=\"%s\" type=\"%s\"/>\n\n",
						 xmltn, tabletypename);
	}
	else
		appendStringInfo(&result,
						 "<xsd:element name=\"%s\" type=\"%s\"/>\n\n",
						 xmltn, rowtypename);

	xsd_schema_element_end(&result);

	return result.data;
}


/*
 * 把 SQL 模式映射成 XML Schema 数据类型；见
 * 9.12 节。
 */
static const char *
map_sql_schema_to_xmlschema_types(Oid nspid, List *relid_list, bool nulls,
								  bool tableforest, const char *targetns)
{
	char	   *dbname;
	char	   *nspname;
	char	   *xmlsn;
	char	   *schematypename;
	StringInfoData result;
	ListCell   *cell;

	dbname = get_database_name(MyDatabaseId);
	nspname = get_namespace_name(nspid);

	initStringInfo(&result);

	xmlsn = map_sql_identifier_to_xml_name(nspname, true, false);

	schematypename = map_multipart_sql_identifier_to_xml_name("SchemaType",
															  dbname,
															  nspname,
															  NULL);

	appendStringInfo(&result,
					 "<xsd:complexType name=\"%s\">\n", schematypename);
	if (!tableforest)
		appendStringInfoString(&result,
							   "  <xsd:all>\n");
	else
		appendStringInfoString(&result,
							   "  <xsd:sequence>\n");

	foreach(cell, relid_list)
	{
		Oid			relid = lfirst_oid(cell);
		char	   *relname = get_rel_name(relid);
		char	   *xmltn = map_sql_identifier_to_xml_name(relname, true, false);
		char	   *tabletypename = map_multipart_sql_identifier_to_xml_name(tableforest ? "RowType" : "TableType",
																			 dbname,
																			 nspname,
																			 relname);

		if (!tableforest)
			appendStringInfo(&result,
							 "    <xsd:element name=\"%s\" type=\"%s\"/>\n",
							 xmltn, tabletypename);
		else
			appendStringInfo(&result,
							 "    <xsd:element name=\"%s\" type=\"%s\" minOccurs=\"0\" maxOccurs=\"unbounded\"/>\n",
							 xmltn, tabletypename);
	}

	if (!tableforest)
		appendStringInfoString(&result,
							   "  </xsd:all>\n");
	else
		appendStringInfoString(&result,
							   "  </xsd:sequence>\n");
	appendStringInfoString(&result,
						   "</xsd:complexType>\n\n");

	appendStringInfo(&result,
					 "<xsd:element name=\"%s\" type=\"%s\"/>\n\n",
					 xmlsn, schematypename);

	return result.data;
}


/*
 * 把 SQL 目录映射成 XML Schema 数据类型；见
 * 9.15 节。
 */
static const char *
map_sql_catalog_to_xmlschema_types(List *nspid_list, bool nulls,
								   bool tableforest, const char *targetns)
{
	char	   *dbname;
	char	   *xmlcn;
	char	   *catalogtypename;
	StringInfoData result;
	ListCell   *cell;

	dbname = get_database_name(MyDatabaseId);

	initStringInfo(&result);

	xmlcn = map_sql_identifier_to_xml_name(dbname, true, false);

	catalogtypename = map_multipart_sql_identifier_to_xml_name("CatalogType",
															   dbname,
															   NULL,
															   NULL);

	appendStringInfo(&result,
					 "<xsd:complexType name=\"%s\">\n", catalogtypename);
	appendStringInfoString(&result,
						   "  <xsd:all>\n");

	foreach(cell, nspid_list)
	{
		Oid			nspid = lfirst_oid(cell);
		char	   *nspname = get_namespace_name(nspid);
		char	   *xmlsn = map_sql_identifier_to_xml_name(nspname, true, false);
		char	   *schematypename = map_multipart_sql_identifier_to_xml_name("SchemaType",
																			  dbname,
																			  nspname,
																			  NULL);

		appendStringInfo(&result,
						 "    <xsd:element name=\"%s\" type=\"%s\"/>\n",
						 xmlsn, schematypename);
	}

	appendStringInfoString(&result,
						   "  </xsd:all>\n");
	appendStringInfoString(&result,
						   "</xsd:complexType>\n\n");

	appendStringInfo(&result,
					 "<xsd:element name=\"%s\" type=\"%s\"/>\n\n",
					 xmlcn, catalogtypename);

	return result.data;
}


/*
 * 把 SQL 数据类型映射为 XML 名；见 SQL/XML:2008 9.4 节
 */
static const char *
map_sql_type_to_xml_name(Oid typeoid, int typmod)
{
	StringInfoData result;

	initStringInfo(&result);

	switch (typeoid)
	{
		case BPCHAROID:
			if (typmod == -1)
				appendStringInfoString(&result, "CHAR");
			else
				appendStringInfo(&result, "CHAR_%d", typmod - VARHDRSZ);
			break;
		case VARCHAROID:
			if (typmod == -1)
				appendStringInfoString(&result, "VARCHAR");
			else
				appendStringInfo(&result, "VARCHAR_%d", typmod - VARHDRSZ);
			break;
		case NUMERICOID:
			if (typmod == -1)
				appendStringInfoString(&result, "NUMERIC");
			else
				appendStringInfo(&result, "NUMERIC_%d_%d",
								 ((typmod - VARHDRSZ) >> 16) & 0xffff,
								 (typmod - VARHDRSZ) & 0xffff);
			break;
		case INT4OID:
			appendStringInfoString(&result, "INTEGER");
			break;
		case INT2OID:
			appendStringInfoString(&result, "SMALLINT");
			break;
		case INT8OID:
			appendStringInfoString(&result, "BIGINT");
			break;
		case FLOAT4OID:
			appendStringInfoString(&result, "REAL");
			break;
		case FLOAT8OID:
			appendStringInfoString(&result, "DOUBLE");
			break;
		case BOOLOID:
			appendStringInfoString(&result, "BOOLEAN");
			break;
		case TIMEOID:
			if (typmod == -1)
				appendStringInfoString(&result, "TIME");
			else
				appendStringInfo(&result, "TIME_%d", typmod);
			break;
		case TIMETZOID:
			if (typmod == -1)
				appendStringInfoString(&result, "TIME_WTZ");
			else
				appendStringInfo(&result, "TIME_WTZ_%d", typmod);
			break;
		case TIMESTAMPOID:
			if (typmod == -1)
				appendStringInfoString(&result, "TIMESTAMP");
			else
				appendStringInfo(&result, "TIMESTAMP_%d", typmod);
			break;
		case TIMESTAMPTZOID:
			if (typmod == -1)
				appendStringInfoString(&result, "TIMESTAMP_WTZ");
			else
				appendStringInfo(&result, "TIMESTAMP_WTZ_%d", typmod);
			break;
		case DATEOID:
			appendStringInfoString(&result, "DATE");
			break;
		case XMLOID:
			appendStringInfoString(&result, "XML");
			break;
		default:
			{
				HeapTuple	tuple;
				Form_pg_type typtuple;

				tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeoid));
				if (!HeapTupleIsValid(tuple))
					elog(ERROR, "cache lookup failed for type %u", typeoid);
				typtuple = (Form_pg_type) GETSTRUCT(tuple);

				appendStringInfoString(&result,
									   map_multipart_sql_identifier_to_xml_name((typtuple->typtype == TYPTYPE_DOMAIN) ? "Domain" : "UDT",
																				get_database_name(MyDatabaseId),
																				get_namespace_name(typtuple->typnamespace),
																				NameStr(typtuple->typname)));

				ReleaseSysCache(tuple);
			}
	}

	return result.data;
}


/*
 * SQL 数据类型集合映射为 XML Schema 数据类型；见
 * SQL/XML:2008 9.7 节。
 */
static const char *
map_sql_typecoll_to_xmlschema_types(List *tupdesc_list)
{
	List	   *uniquetypes = NIL;
	int			i;
	StringInfoData result;
	ListCell   *cell0;

	/* 提取 TupleDesc 集合中用到的所有列类型 */
	foreach(cell0, tupdesc_list)
	{
		TupleDesc	tupdesc = (TupleDesc) lfirst(cell0);

		for (i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);

			if (att->attisdropped)
				continue;
			uniquetypes = list_append_unique_oid(uniquetypes, att->atttypid);
		}
	}

	/* 加入域的基类型 */
	foreach(cell0, uniquetypes)
	{
		Oid			typid = lfirst_oid(cell0);
		Oid			basetypid = getBaseType(typid);

		if (basetypid != typid)
			uniquetypes = list_append_unique_oid(uniquetypes, basetypid);
	}

	/* 转换为文本形式 */
	initStringInfo(&result);

	foreach(cell0, uniquetypes)
	{
		appendStringInfo(&result, "%s\n",
						 map_sql_type_to_xmlschema_type(lfirst_oid(cell0),
														-1));
	}

	return result.data;
}


/*
 * 把 SQL 数据类型映射为具名 XML Schema 数据类
 * 型；见 SQL/XML:2008 9.5/9.6 节
 *
 * （9.5 与 9.6 的区别基本在于 9.6 加了一个 name 属
 * 性，本函数正是这样做的。无名的 9.5 版似乎任何
 * 地方都不需要用到它。）
 */
static const char *
map_sql_type_to_xmlschema_type(Oid typeoid, int typmod)
{
	StringInfoData result;
	const char *typename = map_sql_type_to_xml_name(typeoid, typmod);

	initStringInfo(&result);

	if (typeoid == XMLOID)
	{
		appendStringInfoString(&result,
							   "<xsd:complexType mixed=\"true\">\n"
							   "  <xsd:sequence>\n"
							   "    <xsd:any name=\"element\" minOccurs=\"0\" maxOccurs=\"unbounded\" processContents=\"skip\"/>\n"
							   "  </xsd:sequence>\n"
							   "</xsd:complexType>\n");
	}
	else
	{
		appendStringInfo(&result,
						 "<xsd:simpleType name=\"%s\">\n", typename);

		switch (typeoid)
		{
			case BPCHAROID:
			case VARCHAROID:
			case TEXTOID:
				appendStringInfoString(&result,
									   "  <xsd:restriction base=\"xsd:string\">\n");
				if (typmod != -1)
					appendStringInfo(&result,
									 "    <xsd:maxLength value=\"%d\"/>\n",
									 typmod - VARHDRSZ);
				appendStringInfoString(&result, "  </xsd:restriction>\n");
				break;

			case BYTEAOID:
				appendStringInfo(&result,
								 "  <xsd:restriction base=\"xsd:%s\">\n"
								 "  </xsd:restriction>\n",
								 xmlbinary == XMLBINARY_BASE64 ? "base64Binary" : "hexBinary");
				break;

			case NUMERICOID:
				if (typmod != -1)
					appendStringInfo(&result,
									 "  <xsd:restriction base=\"xsd:decimal\">\n"
									 "    <xsd:totalDigits value=\"%d\"/>\n"
									 "    <xsd:fractionDigits value=\"%d\"/>\n"
									 "  </xsd:restriction>\n",
									 ((typmod - VARHDRSZ) >> 16) & 0xffff,
									 (typmod - VARHDRSZ) & 0xffff);
				break;

			case INT2OID:
				appendStringInfo(&result,
								 "  <xsd:restriction base=\"xsd:short\">\n"
								 "    <xsd:maxInclusive value=\"%d\"/>\n"
								 "    <xsd:minInclusive value=\"%d\"/>\n"
								 "  </xsd:restriction>\n",
								 SHRT_MAX, SHRT_MIN);
				break;

			case INT4OID:
				appendStringInfo(&result,
								 "  <xsd:restriction base=\"xsd:int\">\n"
								 "    <xsd:maxInclusive value=\"%d\"/>\n"
								 "    <xsd:minInclusive value=\"%d\"/>\n"
								 "  </xsd:restriction>\n",
								 INT_MAX, INT_MIN);
				break;

			case INT8OID:
				appendStringInfo(&result,
								 "  <xsd:restriction base=\"xsd:long\">\n"
								 "    <xsd:maxInclusive value=\"" INT64_FORMAT "\"/>\n"
								 "    <xsd:minInclusive value=\"" INT64_FORMAT "\"/>\n"
								 "  </xsd:restriction>\n",
								 PG_INT64_MAX,
								 PG_INT64_MIN);
				break;

			case FLOAT4OID:
				appendStringInfoString(&result,
									   "  <xsd:restriction base=\"xsd:float\"></xsd:restriction>\n");
				break;

			case FLOAT8OID:
				appendStringInfoString(&result,
									   "  <xsd:restriction base=\"xsd:double\"></xsd:restriction>\n");
				break;

			case BOOLOID:
				appendStringInfoString(&result,
									   "  <xsd:restriction base=\"xsd:boolean\"></xsd:restriction>\n");
				break;

			case TIMEOID:
			case TIMETZOID:
				{
					const char *tz = (typeoid == TIMETZOID ? "(\\+|-)\\p{Nd}{2}:\\p{Nd}{2}" : "");

					if (typmod == -1)
						appendStringInfo(&result,
										 "  <xsd:restriction base=\"xsd:time\">\n"
										 "    <xsd:pattern value=\"\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}(.\\p{Nd}+)?%s\"/>\n"
										 "  </xsd:restriction>\n", tz);
					else if (typmod == 0)
						appendStringInfo(&result,
										 "  <xsd:restriction base=\"xsd:time\">\n"
										 "    <xsd:pattern value=\"\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}%s\"/>\n"
										 "  </xsd:restriction>\n", tz);
					else
						appendStringInfo(&result,
										 "  <xsd:restriction base=\"xsd:time\">\n"
										 "    <xsd:pattern value=\"\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}.\\p{Nd}{%d}%s\"/>\n"
										 "  </xsd:restriction>\n", typmod - VARHDRSZ, tz);
					break;
				}

			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
				{
					const char *tz = (typeoid == TIMESTAMPTZOID ? "(\\+|-)\\p{Nd}{2}:\\p{Nd}{2}" : "");

					if (typmod == -1)
						appendStringInfo(&result,
										 "  <xsd:restriction base=\"xsd:dateTime\">\n"
										 "    <xsd:pattern value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}T\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}(.\\p{Nd}+)?%s\"/>\n"
										 "  </xsd:restriction>\n", tz);
					else if (typmod == 0)
						appendStringInfo(&result,
										 "  <xsd:restriction base=\"xsd:dateTime\">\n"
										 "    <xsd:pattern value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}T\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}%s\"/>\n"
										 "  </xsd:restriction>\n", tz);
					else
						appendStringInfo(&result,
										 "  <xsd:restriction base=\"xsd:dateTime\">\n"
										 "    <xsd:pattern value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}T\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}.\\p{Nd}{%d}%s\"/>\n"
										 "  </xsd:restriction>\n", typmod - VARHDRSZ, tz);
					break;
				}

			case DATEOID:
				appendStringInfoString(&result,
									   "  <xsd:restriction base=\"xsd:date\">\n"
									   "    <xsd:pattern value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}\"/>\n"
									   "  </xsd:restriction>\n");
				break;

			default:
				if (get_typtype(typeoid) == TYPTYPE_DOMAIN)
				{
					Oid			base_typeoid;
					int32		base_typmod = -1;

					base_typeoid = getBaseTypeAndTypmod(typeoid, &base_typmod);

					appendStringInfo(&result,
									 "  <xsd:restriction base=\"%s\"/>\n",
									 map_sql_type_to_xml_name(base_typeoid, base_typmod));
				}
				break;
		}
		appendStringInfoString(&result, "</xsd:simpleType>\n");
	}

	return result.data;
}


/*
 * 把 SQL 行映射成 XML 元素，行取自活动 SPI 游标。
 * 另见 SQL/XML:2008 9.10 节。
 */
static void
SPI_sql_row_to_xmlelement(uint64 rownum, StringInfo result, char *tablename,
						  bool nulls, bool tableforest,
						  const char *targetns, bool top_level)
{
	int			i;
	char	   *xmltn;

	if (tablename)
		xmltn = map_sql_identifier_to_xml_name(tablename, true, false);
	else
	{
		if (tableforest)
			xmltn = "row";
		else
			xmltn = "table";
	}

	if (tableforest)
		xmldata_root_element_start(result, xmltn, NULL, targetns, top_level);
	else
		appendStringInfoString(result, "<row>\n");

	for (i = 1; i <= SPI_tuptable->tupdesc->natts; i++)
	{
		char	   *colname;
		Datum		colval;
		bool		isnull;

		colname = map_sql_identifier_to_xml_name(SPI_fname(SPI_tuptable->tupdesc, i),
												 true, false);
		colval = SPI_getbinval(SPI_tuptable->vals[rownum],
							   SPI_tuptable->tupdesc,
							   i,
							   &isnull);
		if (isnull)
		{
			if (nulls)
				appendStringInfo(result, "  <%s xsi:nil=\"true\"/>\n", colname);
		}
		else
			appendStringInfo(result, "  <%s>%s</%s>\n",
							 colname,
							 map_sql_value_to_xml_value(colval,
														SPI_gettypeid(SPI_tuptable->tupdesc, i), true),
							 colname);
	}

	if (tableforest)
	{
		xmldata_root_element_end(result, xmltn);
		appendStringInfoChar(result, '\n');
	}
	else
		appendStringInfoString(result, "</row>\n\n");
}


/*
 * XPath 相关函数
 */

#ifdef USE_LIBXML

/*
 * XML 节点转文本。
 *
 * 对属性和文本节点，返回转义后的文本。对其他任何
 * 情形转储子树。
 */
static text *
xml_xmlnodetoxmltype(xmlNodePtr cur, PgXmlErrorContext *xmlerrcxt)
{
	xmltype    *result = NULL;

	if (cur->type != XML_ATTRIBUTE_NODE && cur->type != XML_TEXT_NODE)
	{
		void		(*volatile nodefree) (xmlNodePtr) = NULL;
		volatile xmlBufferPtr buf = NULL;
		volatile xmlNodePtr cur_copy = NULL;

		PG_TRY();
		{
			int			bytes;

			buf = xmlBufferCreate();
			if (buf == NULL || xmlerrcxt->err_occurred)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
							"could not allocate xmlBuffer");

			/*
			 * 产出该节点的一个可供序列化的转储。xmlNodeDump
			 * 能做到，但其结果不含祖先节点命名空间
			 * 定义，因此我们先执行一次 xmlCopyNode()，它
			 * 会把节点连同其所需的命名空间定义一起复
			 * 制过来。
			 *
			 * 某些旧 libxml2（如 2.7.6）复制会产生部分损
			 * 坏的 XML_DOCUMENT_NODE 节点（content 字段未设
			 * 置）。该节点用 xmlNodeDump 没问题，但
			 * xmlFreeNode 会崩溃；要安排改调 xmlFreeDoc。
			 */
			cur_copy = xmlCopyNode(cur, 1);
			if (cur_copy == NULL || xmlerrcxt->err_occurred)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
							"could not copy node");
			nodefree = (cur_copy->type == XML_DOCUMENT_NODE) ?
				(void (*) (xmlNodePtr)) xmlFreeDoc : xmlFreeNode;

			bytes = xmlNodeDump(buf, NULL, cur_copy, 0, 0);
			if (bytes == -1 || xmlerrcxt->err_occurred)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
							"could not dump node");

			result = xmlBuffer_to_xmltype(buf);
		}
		PG_FINALLY();
		{
			if (nodefree)
				nodefree(cur_copy);
			if (buf)
				xmlBufferFree(buf);
		}
		PG_END_TRY();
	}
	else
	{
		xmlChar    *str;

		str = xmlXPathCastNodeToString(cur);
		PG_TRY();
		{
			/* 这里依赖 XML 与 TEXT 表示形式相同这一事实 */
			char	   *escaped = escape_xml((char *) str);

			result = (xmltype *) cstring_to_text(escaped);
			pfree(escaped);
		}
		PG_FINALLY();
		{
			xmlFree(str);
		}
		PG_END_TRY();
	}

	return result;
}

/*
 * 把 XML XPath 对象（对 XPath 表达式求值的结果）转换
 * 为 xml 值数组，这些值会附加到 astate。函数的返回值
 * 是数组中元素的个数。
 *
 * 如果 "astate" 为 NULL，那么就不生成数组值，但仍返回
 * 它本应拥有的元素个数。
 *
 * 节点集转换成一个包含节点文本表示的数组。原
 * 始值（float、double、string）会转换成单元素数组，
 * 其中含有该值的字符串表示。
 */
static int
xml_xpathobjtoxmlarray(xmlXPathObjectPtr xpathobj,
					   ArrayBuildState *astate,
					   PgXmlErrorContext *xmlerrcxt)
{
	int			result = 0;
	Datum		datum;
	Oid			datumtype;
	char	   *result_str;

	switch (xpathobj->type)
	{
		case XPATH_NODESET:
			if (xpathobj->nodesetval != NULL)
			{
				result = xpathobj->nodesetval->nodeNr;
				if (astate != NULL)
				{
					int			i;

					for (i = 0; i < result; i++)
					{
						datum = PointerGetDatum(xml_xmlnodetoxmltype(xpathobj->nodesetval->nodeTab[i],
																	 xmlerrcxt));
						(void) accumArrayResult(astate, datum, false,
												XMLOID, CurrentMemoryContext);
					}
				}
			}
			return result;

		case XPATH_BOOLEAN:
			if (astate == NULL)
				return 1;
			datum = BoolGetDatum(xpathobj->boolval);
			datumtype = BOOLOID;
			break;

		case XPATH_NUMBER:
			if (astate == NULL)
				return 1;
			datum = Float8GetDatum(xpathobj->floatval);
			datumtype = FLOAT8OID;
			break;

		case XPATH_STRING:
			if (astate == NULL)
				return 1;
			datum = CStringGetDatum((char *) xpathobj->stringval);
			datumtype = CSTRINGOID;
			break;

		default:
			elog(ERROR, "xpath expression result type %d is unsupported",
				 xpathobj->type);
			return 0;			/* 让编译器安静 */
	}

	/* 标量值情形的公共代码 */
	result_str = map_sql_value_to_xml_value(datum, datumtype, true);
	datum = PointerGetDatum(cstring_to_xmltype(result_str));
	(void) accumArrayResult(astate, datum, false,
							XMLOID, CurrentMemoryContext);
	return 1;
}


/*
 * xpath() 与 xmlexists() 的公共代码
 *
 * 求值 XPath 表达式，节点数返回在 res_nitems，而
 * XML 值数组则返回在 astate。这两个指针中任何一个
 * 可为 NULL，如果不需要对应结果。
 *
 * 需由用户确保传入的 XML 确实是 XML 文档 ——
 * XPath 在缺少已知上下文节点的情
 * 况下难以处理。
 */
static void
xpath_internal(text *xpath_expr_text, xmltype *data, ArrayType *namespaces,
			   int *res_nitems, ArrayBuildState *astate)
{
	PgXmlErrorContext *xmlerrcxt;
	volatile xmlParserCtxtPtr ctxt = NULL;
	volatile xmlDocPtr doc = NULL;
	volatile xmlXPathContextPtr xpathctx = NULL;
	volatile xmlXPathCompExprPtr xpathcomp = NULL;
	volatile xmlXPathObjectPtr xpathobj = NULL;
	char	   *datastr;
	int32		len;
	int32		xpath_len;
	xmlChar    *string;
	xmlChar    *xpath_expr;
	size_t		xmldecl_len = 0;
	int			i;
	int			ndim;
	Datum	   *ns_names_uris;
	bool	   *ns_names_uris_nulls;
	int			ns_count;

	/*
	 * 命名空间映射以 text[] 传入。如果传入的是空数组
	 * （ndim = 0，"0 维"），则无命名空间映射。否则
	 * 应传入一个二维数组，其第二轴长度应等于
	 *  2，即每个子数组包含 2 个元素，第一个元素定义名
	 * 称，第二个元素是 URI。例如：
	 * ARRAY[ARRAY['myns', 'http://example.com'], ARRAY['myns2',
	 * 'http://example2.com']].
	 */
	ndim = namespaces ? ARR_NDIM(namespaces) : 0;
	if (ndim != 0)
	{
		int		   *dims;

		dims = ARR_DIMS(namespaces);

		if (ndim != 2 || dims[1] != 2)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("invalid array for XML namespace mapping"),
					 errdetail("The array must be two-dimensional with length of the second axis equal to 2.")));

		Assert(ARR_ELEMTYPE(namespaces) == TEXTOID);

		deconstruct_array_builtin(namespaces, TEXTOID,
								  &ns_names_uris, &ns_names_uris_nulls,
								  &ns_count);

		Assert((ns_count % 2) == 0);	/* 上面查过 */
		ns_count /= 2;			/* 只计配对 */
	}
	else
	{
		ns_names_uris = NULL;
		ns_names_uris_nulls = NULL;
		ns_count = 0;
	}

	datastr = VARDATA(data);
	len = VARSIZE(data) - VARHDRSZ;
	xpath_len = VARSIZE_ANY_EXHDR(xpath_expr_text);
	if (xpath_len == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("empty XPath expression")));

	string = pg_xmlCharStrndup(datastr, len);
	xpath_expr = pg_xmlCharStrndup(VARDATA_ANY(xpath_expr_text), xpath_len);

	/*
	 * UTF8 数据库中，跳过任何 xml 声明，它可能断言另
	 * 一种编码。忽略 parse_xml_decl() 的失败，让
	 * xmlCtxtReadMemory() 报告解析错误。文档不承诺非
	 * UTF8 数据库中非 ASCII 数据的 xpath() 支持，所以让这
	 * 些情形按 bug 兼容方式保持历史行为。
	 */
	if (GetDatabaseEncoding() == PG_UTF8)
		parse_xml_decl(string, &xmldecl_len, NULL, NULL, NULL);

	xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

	PG_TRY();
	{
		xmlInitParser();

		/*
		 * 冗余的 XML 解析（同一命令执行期间可能对同一值
		 * 解析两次）
		 */
		ctxt = xmlNewParserCtxt();
		if (ctxt == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate parser context");
		doc = xmlCtxtReadMemory(ctxt, (char *) string + xmldecl_len,
								len - xmldecl_len, NULL, NULL, 0);
		if (doc == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_XML_DOCUMENT,
						"could not parse XML document");
		xpathctx = xmlXPathNewContext(doc);
		if (xpathctx == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate XPath context");
		xpathctx->node = (xmlNodePtr) doc;

		/* 如有则注册命名空间 */
		if (ns_count > 0)
		{
			for (i = 0; i < ns_count; i++)
			{
				char	   *ns_name;
				char	   *ns_uri;

				if (ns_names_uris_nulls[i * 2] ||
					ns_names_uris_nulls[i * 2 + 1])
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("neither namespace name nor URI may be null")));
				ns_name = TextDatumGetCString(ns_names_uris[i * 2]);
				ns_uri = TextDatumGetCString(ns_names_uris[i * 2 + 1]);
				if (xmlXPathRegisterNs(xpathctx,
									   (xmlChar *) ns_name,
									   (xmlChar *) ns_uri) != 0)
					ereport(ERROR,	/* 这是内部错误吗 */
							(errmsg("could not register XML namespace with name \"%s\" and URI \"%s\"",
									ns_name, ns_uri)));
			}
		}

		/*
		 * 注意：此处和其他地方要小心使用 xmlXPathCtxtCompile
		 * 而非 xmlXPathCompile。在 libxml2 2.13.3 及更老版本中，
		 * 后一函数不能自防御递归到栈溢出。见
		 * https://gitlab.gnome.org/GNOME/libxml2/-/issues/799
		 */
		xpathcomp = xmlXPathCtxtCompile(xpathctx, xpath_expr);
		if (xpathcomp == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INTERNAL_ERROR,
						"invalid XPath expression");

		/*
		 * 2.6.27 版引入了一个名为
		 * xmlXPathCompiledEvalToBoolean 的函数，它对 xmlexists
		 * 已足够，但我们可以由是否返回节点推断存在性，
		 * 从而避免库版本升级，并让代码保持
		 * 不变。
		 */
		xpathobj = xmlXPathCompiledEval(xpathcomp, xpathctx);
		if (xpathobj == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INTERNAL_ERROR,
						"could not create XPath object");

		/*
		 * 按要求提取结果。
		 */
		if (res_nitems != NULL)
			*res_nitems = xml_xpathobjtoxmlarray(xpathobj, astate, xmlerrcxt);
		else
			(void) xml_xpathobjtoxmlarray(xpathobj, astate, xmlerrcxt);
	}
	PG_CATCH();
	{
		if (xpathobj)
			xmlXPathFreeObject(xpathobj);
		if (xpathcomp)
			xmlXPathFreeCompExpr(xpathcomp);
		if (xpathctx)
			xmlXPathFreeContext(xpathctx);
		if (doc)
			xmlFreeDoc(doc);
		if (ctxt)
			xmlFreeParserCtxt(ctxt);

		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	xmlXPathFreeObject(xpathobj);
	xmlXPathFreeCompExpr(xpathcomp);
	xmlXPathFreeContext(xpathctx);
	xmlFreeDoc(doc);
	xmlFreeParserCtxt(ctxt);

	pg_xml_done(xmlerrcxt, false);
}
#endif							/* USE_LIBXML */

/*
 * 对 XPath 表达式求值并返回 XML 值数组。
 *
 * 由于尚不支持 XQuery 序列，本函数似乎是最有用的
 * （XML 函数数组起着某种 XQuery 序列替代品的作
 * 用）。
 */
Datum
xpath(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *xpath_expr_text = PG_GETARG_TEXT_PP(0);
	xmltype    *data = PG_GETARG_XML_P(1);
	ArrayType  *namespaces = PG_GETARG_ARRAYTYPE_P(2);
	ArrayBuildState *astate;

	astate = initArrayResult(XMLOID, CurrentMemoryContext, true);
	xpath_internal(xpath_expr_text, data, namespaces,
				   NULL, astate);
	PG_RETURN_DATUM(makeArrayResult(astate, CurrentMemoryContext));
#else
	NO_XML_SUPPORT();
	return 0;
#endif
}

/*
 * 判断所给 XPath 指定的节点是否在给定 XML 文
 * 档中，返回布尔值。
 */
Datum
xmlexists(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *xpath_expr_text = PG_GETARG_TEXT_PP(0);
	xmltype    *data = PG_GETARG_XML_P(1);
	int			res_nitems;

	xpath_internal(xpath_expr_text, data, NULL,
				   &res_nitems, NULL);

	PG_RETURN_BOOL(res_nitems > 0);
#else
	NO_XML_SUPPORT();
	return 0;
#endif
}

/*
 * 判断所给 XPath 指定的节点是否在给定 XML 文
 * 档中，返回布尔值。它与
 * xmlexists 不同：支持命名空间且不在 SQL/XML 内。
 */
Datum
xpath_exists(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *xpath_expr_text = PG_GETARG_TEXT_PP(0);
	xmltype    *data = PG_GETARG_XML_P(1);
	ArrayType  *namespaces = PG_GETARG_ARRAYTYPE_P(2);
	int			res_nitems;

	xpath_internal(xpath_expr_text, data, namespaces,
				   &res_nitems, NULL);

	PG_RETURN_BOOL(res_nitems > 0);
#else
	NO_XML_SUPPORT();
	return 0;
#endif
}

/*
 * 良构性检查函数
 */

#ifdef USE_LIBXML
static bool
wellformed_xml(text *data, XmlOptionType xmloption_arg)
{
	xmlDocPtr	doc;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	/*
	 * 若 xml_parse() 未报告软错误，我们将报告 "true"。
	 */
	doc = xml_parse(data, xmloption_arg, true,
					GetDatabaseEncoding(), NULL, NULL, (Node *) &escontext);
	if (doc)
		xmlFreeDoc(doc);

	return !escontext.error_occurred;
}
#endif

Datum
xml_is_well_formed(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *data = PG_GETARG_TEXT_PP(0);

	PG_RETURN_BOOL(wellformed_xml(data, xmloption));
#else
	NO_XML_SUPPORT();
	return 0;
#endif							/* not USE_LIBXML */
}

Datum
xml_is_well_formed_document(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *data = PG_GETARG_TEXT_PP(0);

	PG_RETURN_BOOL(wellformed_xml(data, XMLOPTION_DOCUMENT));
#else
	NO_XML_SUPPORT();
	return 0;
#endif							/* not USE_LIBXML */
}

Datum
xml_is_well_formed_content(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *data = PG_GETARG_TEXT_PP(0);

	PG_RETURN_BOOL(wellformed_xml(data, XMLOPTION_CONTENT));
#else
	NO_XML_SUPPORT();
	return 0;
#endif							/* not USE_LIBXML */
}

/*
 * XMLTABLE 的支持函数
 *
 */
#ifdef USE_LIBXML

/*
 * 从执行器状态返回私有数据。通过与 MAGIC 数字校
 * 验。
 */
static inline XmlTableBuilderData *
GetXmlTableBuilderPrivateData(TableFuncScanState *state, const char *fname)
{
	XmlTableBuilderData *result;

	if (!IsA(state, TableFuncScanState))
		elog(ERROR, "%s called with invalid TableFuncScanState", fname);
	result = (XmlTableBuilderData *) state->opaque;
	if (result->magic != XMLTABLE_CONTEXT_MAGIC)
		elog(ERROR, "%s called with invalid TableFuncScanState", fname);

	return result;
}
#endif

/*
 * XmlTableInitOpaque
 *		为 XmlTable 处理器填充 TableFuncScanState->opaque；初始化
 *		XML 解析器
 *
 * 注意：因为我们在此调用 pg_xml_init() 而在
 * XmlTableDestroyOpaque 中调用 pg_xml_done()，所以为了健壮
 * 性起见，本节点处理完毕前不得运行任何其他执行器
 * 节点。调用方必须将其执行到底（多半是填满一个
 * tuplestore 单趟耗尽节点），而非逐行调用模式。
 */
static void
XmlTableInitOpaque(TableFuncScanState *state, int natts)
{
#ifdef USE_LIBXML
	volatile xmlParserCtxtPtr ctxt = NULL;
	XmlTableBuilderData *xtCxt;
	PgXmlErrorContext *xmlerrcxt;

	xtCxt = palloc0(sizeof(XmlTableBuilderData));
	xtCxt->magic = XMLTABLE_CONTEXT_MAGIC;
	xtCxt->natts = natts;
	xtCxt->xpathscomp = palloc0(sizeof(xmlXPathCompExprPtr) * natts);

	xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

	PG_TRY();
	{
		xmlInitParser();

		ctxt = xmlNewParserCtxt();
		if (ctxt == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate parser context");
	}
	PG_CATCH();
	{
		if (ctxt != NULL)
			xmlFreeParserCtxt(ctxt);

		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	xtCxt->xmlerrcxt = xmlerrcxt;
	xtCxt->ctxt = ctxt;

	state->opaque = xtCxt;
#else
	NO_XML_SUPPORT();
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableSetDocument
 *		安装输入文档
 */
static void
XmlTableSetDocument(TableFuncScanState *state, Datum value)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;
	xmltype    *xmlval = DatumGetXmlP(value);
	char	   *str;
	xmlChar    *xstr;
	int			length;
	volatile xmlDocPtr doc = NULL;
	volatile xmlXPathContextPtr xpathcxt = NULL;

	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableSetDocument");

	/*
	 * 用输出函数来转换成字符串（移除编码属性）。见
	 * xml_out 注释。
	 */
	str = xml_out_internal(xmlval, 0);

	length = strlen(str);
	xstr = pg_xmlCharStrndup(str, length);

	PG_TRY();
	{
		doc = xmlCtxtReadMemory(xtCxt->ctxt, (char *) xstr, length, NULL, NULL, 0);
		if (doc == NULL || xtCxt->xmlerrcxt->err_occurred)
			xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_INVALID_XML_DOCUMENT,
						"could not parse XML document");
		xpathcxt = xmlXPathNewContext(doc);
		if (xpathcxt == NULL || xtCxt->xmlerrcxt->err_occurred)
			xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate XPath context");
		xpathcxt->node = (xmlNodePtr) doc;
	}
	PG_CATCH();
	{
		if (xpathcxt != NULL)
			xmlXPathFreeContext(xpathcxt);
		if (doc != NULL)
			xmlFreeDoc(doc);

		PG_RE_THROW();
	}
	PG_END_TRY();

	xtCxt->doc = doc;
	xtCxt->xpathcxt = xpathcxt;
#else
	NO_XML_SUPPORT();
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableSetNamespace
 *		添加命名空间声明
 */
static void
XmlTableSetNamespace(TableFuncScanState *state, const char *name, const char *uri)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;

	if (name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("DEFAULT namespace is not supported")));
	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableSetNamespace");

	if (xmlXPathRegisterNs(xtCxt->xpathcxt,
						   pg_xmlCharStrndup(name, strlen(name)),
						   pg_xmlCharStrndup(uri, strlen(uri))))
		xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_DATA_EXCEPTION,
					"could not set XML namespace");
#else
	NO_XML_SUPPORT();
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableSetRowFilter
 *		安装行过滤 XPath 表达式。
 */
static void
XmlTableSetRowFilter(TableFuncScanState *state, const char *path)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;
	xmlChar    *xstr;

	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableSetRowFilter");

	if (*path == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("row path filter must not be empty string")));

	xstr = pg_xmlCharStrndup(path, strlen(path));

	/* 要求已经执行过 XmlTableSetDocument */
	Assert(xtCxt->xpathcxt != NULL);

	xtCxt->xpathcomp = xmlXPathCtxtCompile(xtCxt->xpathcxt, xstr);
	if (xtCxt->xpathcomp == NULL || xtCxt->xmlerrcxt->err_occurred)
		xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_SYNTAX_ERROR,
					"invalid XPath expression");
#else
	NO_XML_SUPPORT();
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableSetColumnFilter
 *		为给定列安装列过滤 XPath 表达式。
 */
static void
XmlTableSetColumnFilter(TableFuncScanState *state, const char *path, int colnum)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;
	xmlChar    *xstr;

	Assert(PointerIsValid(path));

	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableSetColumnFilter");

	if (*path == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("column path filter must not be empty string")));

	xstr = pg_xmlCharStrndup(path, strlen(path));

	/* 要求已经执行过 XmlTableSetDocument */
	Assert(xtCxt->xpathcxt != NULL);

	xtCxt->xpathscomp[colnum] = xmlXPathCtxtCompile(xtCxt->xpathcxt, xstr);
	if (xtCxt->xpathscomp[colnum] == NULL || xtCxt->xmlerrcxt->err_occurred)
		xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_DATA_EXCEPTION,
					"invalid XPath expression");
#else
	NO_XML_SUPPORT();
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableFetchRow
 *		为后续 GetValue 调用备好下一个"当前"元组。
 *		若行过滤表达式不再返回行，则返回 false。
 */
static bool
XmlTableFetchRow(TableFuncScanState *state)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;

	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableFetchRow");

	/* 把自身错误上下文传播给 libxml2 */
	xmlSetStructuredErrorFunc((void *) xtCxt->xmlerrcxt, xml_errorHandler);

	if (xtCxt->xpathobj == NULL)
	{
		xtCxt->xpathobj = xmlXPathCompiledEval(xtCxt->xpathcomp, xtCxt->xpathcxt);
		if (xtCxt->xpathobj == NULL || xtCxt->xmlerrcxt->err_occurred)
			xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_INTERNAL_ERROR,
						"could not create XPath object");

		xtCxt->row_count = 0;
	}

	if (xtCxt->xpathobj->type == XPATH_NODESET)
	{
		if (xtCxt->xpathobj->nodesetval != NULL)
		{
			if (xtCxt->row_count++ < xtCxt->xpathobj->nodesetval->nodeNr)
				return true;
		}
	}

	return false;
#else
	NO_XML_SUPPORT();
	return false;
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableGetValue
 *		返回当前行列号为 'colnum' 的值。如果请求的列号为
 *		-1，则返回整行的表示形式。
 *
 * 此函数会泄漏内存，故要确保经常重置调用它的上
 * 文。
 */
static Datum
XmlTableGetValue(TableFuncScanState *state, int colnum,
				 Oid typid, int32 typmod, bool *isnull)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;
	Datum		result = (Datum) 0;
	xmlNodePtr	cur;
	char	   *cstr = NULL;
	volatile xmlXPathObjectPtr xpathobj = NULL;

	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableGetValue");

	Assert(xtCxt->xpathobj &&
		   xtCxt->xpathobj->type == XPATH_NODESET &&
		   xtCxt->xpathobj->nodesetval != NULL);

	/* 把自身错误上下文传播给 libxml2 */
	xmlSetStructuredErrorFunc((void *) xtCxt->xmlerrcxt, xml_errorHandler);

	*isnull = false;

	cur = xtCxt->xpathobj->nodesetval->nodeTab[xtCxt->row_count - 1];

	Assert(xtCxt->xpathscomp[colnum] != NULL);

	PG_TRY();
	{
		/* 把当前节点设为 XPath 求值的入口点 */
		xtCxt->xpathcxt->node = cur;

		/* 求值列路径 */
		xpathobj = xmlXPathCompiledEval(xtCxt->xpathscomp[colnum], xtCxt->xpathcxt);
		if (xpathobj == NULL || xtCxt->xmlerrcxt->err_occurred)
			xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_INTERNAL_ERROR,
						"could not create XPath object");

		/*
		 * 依 XPath 表达式返回的节点数与目标列类型，共
		 * 有四种情形：a) XPath 无节点返回；b) 目标类型
		 * 为 XML（全按 XML 返回）。非 XML 返回类型时：c)
		 * 单个节点（返回内容）。d) 多个节点（错
		 * 误）。
		 */
		if (xpathobj->type == XPATH_NODESET)
		{
			int			count = 0;

			if (xpathobj->nodesetval != NULL)
				count = xpathobj->nodesetval->nodeNr;

			if (xpathobj->nodesetval == NULL || count == 0)
			{
				*isnull = true;
			}
			else
			{
				if (typid == XMLOID)
				{
					text	   *textstr;
					StringInfoData str;

					/* 串接序列化后的值 */
					initStringInfo(&str);
					for (int i = 0; i < count; i++)
					{
						textstr =
							xml_xmlnodetoxmltype(xpathobj->nodesetval->nodeTab[i],
												 xtCxt->xmlerrcxt);

						appendStringInfoText(&str, textstr);
					}
					cstr = str.data;
				}
				else
				{
					xmlChar    *str;

					if (count > 1)
						ereport(ERROR,
								(errcode(ERRCODE_CARDINALITY_VIOLATION),
								 errmsg("more than one value returned by column XPath expression")));

					str = xmlXPathCastNodeSetToString(xpathobj->nodesetval);
					cstr = str ? xml_pstrdup_and_free(str) : "";
				}
			}
		}
		else if (xpathobj->type == XPATH_STRING)
		{
			/* 当目标将是 XML 时，content 应被转义 */
			if (typid == XMLOID)
				cstr = escape_xml((char *) xpathobj->stringval);
			else
				cstr = (char *) xpathobj->stringval;
		}
		else if (xpathobj->type == XPATH_BOOLEAN)
		{
			char		typcategory;
			bool		typispreferred;
			xmlChar    *str;

			/* 允许从 boolean 到数字的隐式转换 */
			get_type_category_preferred(typid, &typcategory, &typispreferred);

			if (typcategory != TYPCATEGORY_NUMERIC)
				str = xmlXPathCastBooleanToString(xpathobj->boolval);
			else
				str = xmlXPathCastNumberToString(xmlXPathCastBooleanToNumber(xpathobj->boolval));

			cstr = xml_pstrdup_and_free(str);
		}
		else if (xpathobj->type == XPATH_NUMBER)
		{
			xmlChar    *str;

			str = xmlXPathCastNumberToString(xpathobj->floatval);
			cstr = xml_pstrdup_and_free(str);
		}
		else
			elog(ERROR, "unexpected XPath object type %u", xpathobj->type);

		/*
		 * 到这里，cstr 要么已含结果值，要么已设 isnull 标
		 * 志。
		 */
		Assert(cstr || *isnull);

		if (!*isnull)
			result = InputFunctionCall(&state->in_functions[colnum],
									   cstr,
									   state->typioparams[colnum],
									   typmod);
	}
	PG_FINALLY();
	{
		if (xpathobj != NULL)
			xmlXPathFreeObject(xpathobj);
	}
	PG_END_TRY();

	return result;
#else
	NO_XML_SUPPORT();
	return 0;
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableDestroyOpaque
 *		释放全部 libxml2 资源
 */
static void
XmlTableDestroyOpaque(TableFuncScanState *state)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;

	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableDestroyOpaque");

	/* 把自身错误上下文传播给 libxml2 */
	xmlSetStructuredErrorFunc((void *) xtCxt->xmlerrcxt, xml_errorHandler);

	if (xtCxt->xpathscomp != NULL)
	{
		int			i;

		for (i = 0; i < xtCxt->natts; i++)
			if (xtCxt->xpathscomp[i] != NULL)
				xmlXPathFreeCompExpr(xtCxt->xpathscomp[i]);
	}

	if (xtCxt->xpathobj != NULL)
		xmlXPathFreeObject(xtCxt->xpathobj);
	if (xtCxt->xpathcomp != NULL)
		xmlXPathFreeCompExpr(xtCxt->xpathcomp);
	if (xtCxt->xpathcxt != NULL)
		xmlXPathFreeContext(xtCxt->xpathcxt);
	if (xtCxt->doc != NULL)
		xmlFreeDoc(xtCxt->doc);
	if (xtCxt->ctxt != NULL)
		xmlFreeParserCtxt(xtCxt->ctxt);

	pg_xml_done(xtCxt->xmlerrcxt, true);

	/* 不再有效 */
	xtCxt->magic = 0;
	state->opaque = NULL;

#else
	NO_XML_SUPPORT();
#endif							/* not USE_LIBXML */
}
