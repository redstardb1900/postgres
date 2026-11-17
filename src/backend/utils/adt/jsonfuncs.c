/*-------------------------------------------------------------------------
 *
 * jsonfuncs.c
 *		处理 JSON 数据类型的函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "common/jsonapi.h"
#include "common/string.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/jsonfuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* setPath 可用的操作 */
#define JB_PATH_CREATE					0x0001
#define JB_PATH_DELETE					0x0002
#define JB_PATH_REPLACE					0x0004
#define JB_PATH_INSERT_BEFORE			0x0008
#define JB_PATH_INSERT_AFTER			0x0010
#define JB_PATH_CREATE_OR_INSERT \
	(JB_PATH_INSERT_BEFORE | JB_PATH_INSERT_AFTER | JB_PATH_CREATE)
#define JB_PATH_FILL_GAPS				0x0020
#define JB_PATH_CONSISTENT_POSITION		0x0040

/* json_object_keys 的状态 */
typedef struct OkeysState
{
	JsonLexContext *lex;
	char	  **result;
	int			result_size;
	int			result_count;
	int			sent_count;
} OkeysState;

/* iterate_json_values 函数的状态 */
typedef struct IterateJsonStringValuesState
{
	JsonLexContext *lex;
	JsonIterateStringValuesAction action;	/* 将应用到每个
											 * json 值的操作 */
	void	   *action_state;	/* 迭代所需的上下文 */
	uint32		flags;			/* 想要从 json 中迭代的
								 * 元素 */
} IterateJsonStringValuesState;

/* transform_json_string_values 函数的状态 */
typedef struct TransformJsonStringValuesState
{
	JsonLexContext *lex;
	StringInfo	strval;			/* 结果 json */
	JsonTransformStringValuesAction action; /* 将应用到每个
											 * json 值的操作 */
	void	   *action_state;	/* 变换所需的上下文 */
} TransformJsonStringValuesState;

/* json_get* 函数的状态 */
typedef struct GetState
{
	JsonLexContext *lex;
	text	   *tresult;
	char	   *result_start;
	bool		normalize_results;
	bool		next_scalar;
	int			npath;			/* 各路径相关数组的长度 */
	char	  **path_names;		/* 要查找的字段名 */
	int		   *path_indexes;	/* 要查找的数组下标 */
	bool	   *pathok;			/* 路径是否匹配当前深度? */
	int		   *array_cur_index;	/* 各路径层级当前元素
									 * 下标*/
} GetState;

/* json_array_length 的状态 */
typedef struct AlenState
{
	JsonLexContext *lex;
	int			count;
} AlenState;

/* json_each 的状态 */
typedef struct EachState
{
	JsonLexContext *lex;
	Tuplestorestate *tuple_store;
	TupleDesc	ret_tdesc;
	MemoryContext tmp_cxt;
	char	   *result_start;
	bool		normalize_results;
	bool		next_scalar;
	char	   *normalized_scalar;
} EachState;

/* json_array_elements 的状态 */
typedef struct ElementsState
{
	JsonLexContext *lex;
	const char *function_name;
	Tuplestorestate *tuple_store;
	TupleDesc	ret_tdesc;
	MemoryContext tmp_cxt;
	char	   *result_start;
	bool		normalize_results;
	bool		next_scalar;
	char	   *normalized_scalar;
} ElementsState;

/* get_json_object_as_hash 的状态 */
typedef struct JHashState
{
	JsonLexContext *lex;
	const char *function_name;
	HTAB	   *hash;
	char	   *saved_scalar;
	char	   *save_json_start;
	JsonTokenType saved_token_type;
} JHashState;

/* 哈希表元素 */
typedef struct JsonHashEntry
{
	char		fname[NAMEDATALEN]; /* 哈希键 (必须居首) */
	char	   *val;
	JsonTokenType type;
} JsonHashEntry;

/* 缓存 populate_scalar() 所需类型 I/O 元数据的结构 */
typedef struct ScalarIOData
{
	Oid			typioparam;
	FmgrInfo	typiofunc;
} ScalarIOData;

/* 这两个结构会被递归使用 */
typedef struct ColumnIOData ColumnIOData;
typedef struct RecordIOData RecordIOData;

/* 缓存 populate_array() 所需元数据的结构 */
typedef struct ArrayIOData
{
	ColumnIOData *element_info; /* 元数据缓存*/
	Oid			element_type;	/* 数组元素类型 id */
	int32		element_typmod; /* 数组元素类型修饰符 */
} ArrayIOData;

/* 缓存 populate_composite() 所需元数据的结构 */
typedef struct CompositeIOData
{
	/*
	 * 此处用 RecordIOData 指针，因为变长结构
	 * RecordIOData 无法直接用于 ColumnIOData.io 联合
	 */
	RecordIOData *record_io;	/* populate_record() 的元数据缓存 */
	TupleDesc	tupdesc;		/* 缓存的元组描述符*/
	/* 仅当目标类型为复合上的域时这些字段才与之不同: */
	Oid			base_typid;		/* 基类型 id */
	int32		base_typmod;	/* 基类型修饰符 */
	/* 仅当目标类型为复合上的域时才使用此字段: */
	void	   *domain_info;	/* 域检查的不透明缓存*/
} CompositeIOData;

/* 缓存 populate_domain() 所需元数据的结构 */
typedef struct DomainIOData
{
	ColumnIOData *base_io;		/* 元数据缓存*/
	Oid			base_typid;		/* 基类型 id */
	int32		base_typmod;	/* 基类型修饰符 */
	void	   *domain_info;	/* 域检查的不透明缓存*/
} DomainIOData;

/* 枚举类型类别 */
typedef enum TypeCat
{
	TYPECAT_SCALAR = 's',
	TYPECAT_ARRAY = 'a',
	TYPECAT_COMPOSITE = 'c',
	TYPECAT_COMPOSITE_DOMAIN = 'C',
	TYPECAT_DOMAIN = 'd'
} TypeCat;

/* 这两个取自 hstore / record_out，用于 populate_record* */

/* 缓存 populate_record_field() 所需记录元数据的结构 */
struct ColumnIOData
{
	Oid			typid;			/* 列类型 id */
	int32		typmod;			/* 列类型修饰符 */
	TypeCat		typcat;			/* 列类型类别 */
	ScalarIOData scalar_io;		/* 元数据缓存，供直接
								 * 经输入函数转换 */
	union
	{
		ArrayIOData array;
		CompositeIOData composite;
		DomainIOData domain;
	}			io;				/* 各种列类型类别共用的
								 * 缓存 */
};

/* 缓存 populate_record() 所需记录元数据的结构 */
struct RecordIOData
{
	Oid			record_type;
	int32		record_typmod;
	int			ncolumns;
	ColumnIOData columns[FLEXIBLE_ARRAY_MEMBER];
};

/* populate_record_worker 与 populate_recordset_worker 的每查询缓存 */
typedef struct PopulateRecordCache
{
	Oid			argtype;		/* record 参数的声明类型 */
	ColumnIOData c;				/* populate_composite() 的元数据缓存 */
	MemoryContext fn_mcxt;		/* 其存放位置 */
} PopulateRecordCache;

/* populate_recordset 每次调用状态*/
typedef struct PopulateRecordsetState
{
	JsonLexContext *lex;
	const char *function_name;
	HTAB	   *json_hash;
	char	   *saved_scalar;
	char	   *save_json_start;
	JsonTokenType saved_token_type;
	Tuplestorestate *tuple_store;
	HeapTupleHeader rec;
	PopulateRecordCache *cache;
} PopulateRecordsetState;

/* populate_array_json() 与 populate_array_dim_jsonb() 的公共数据 */
typedef struct PopulateArrayContext
{
	ArrayBuildState *astate;	/* 数组构建状态*/
	ArrayIOData *aio;			/* 元数据缓存*/
	MemoryContext acxt;			/* 数组构建内存上下文*/
	MemoryContext mcxt;			/* 缓存内存上下文*/
	const char *colname;		/* 仅用于诊断 */
	int		   *dims;			/* 维度 */
	int		   *sizes;			/* 各维度当前计数 */
	int			ndims;			/* 维度数 */
} PopulateArrayContext;

/* populate_array_json() 的状态 */
typedef struct PopulateArrayState
{
	JsonLexContext *lex;		/* json 词法*/
	PopulateArrayContext *ctx;	/*上下文*/
	char	   *element_start;	/* 当前数组元素的起始处 */
	char	   *element_scalar; /* 当前数组元素记号，若
								 * 标量 */
	JsonTokenType element_type; /* 当前数组元素类型 */
} PopulateArrayState;

/* json_strip_nulls 的状态 */
typedef struct StripnullState
{
	JsonLexContext *lex;
	StringInfo	strval;
	bool		skip_next_null;
} StripnullState;

/* 广义的 json/jsonb 值传递结构 */
typedef struct JsValue
{
	bool		is_json;		/* json/jsonb */
	union
	{
		struct
		{
			char	   *str;	/* json 串 */
			int			len;	/* json 串长度，null 结尾时为 -1 */
			JsonTokenType type; /* 类型 */
		}			json;		/* json 值 */

		JsonbValue *jsonb;		/* jsonb 值 */
	}			val;
} JsValue;

typedef struct JsObject
{
	bool		is_json;		/* json/jsonb */
	union
	{
		HTAB	   *json_hash;
		JsonbContainer *jsonb_cont;
	}			val;
} JsObject;

/* 测试 JsValue 属性的实用宏 */
#define JsValueIsNull(jsv) \
	((jsv)->is_json ?  \
		(!(jsv)->val.json.str || (jsv)->val.json.type == JSON_TOKEN_NULL) : \
		(!(jsv)->val.jsonb || (jsv)->val.jsonb->type == jbvNull))

#define JsValueIsString(jsv) \
	((jsv)->is_json ? (jsv)->val.json.type == JSON_TOKEN_STRING \
		: ((jsv)->val.jsonb && (jsv)->val.jsonb->type == jbvString))

#define JsObjectIsEmpty(jso) \
	((jso)->is_json \
		? hash_get_num_entries((jso)->val.json_hash) == 0 \
		: ((jso)->val.jsonb_cont == NULL || \
		   JsonContainerSize((jso)->val.jsonb_cont) == 0))

#define JsObjectFree(jso) \
	do { \
		if ((jso)->is_json) \
			hash_destroy((jso)->val.json_hash); \
	} while (0)

static int	report_json_context(JsonLexContext *lex);

/* json_object_keys 的语义动作函数 */
static JsonParseErrorType okeys_object_field_start(void *state, char *fname, bool isnull);
static JsonParseErrorType okeys_array_start(void *state);
static JsonParseErrorType okeys_scalar(void *state, char *token, JsonTokenType tokentype);

/* json_get* 函数的语义动作函数 */
static JsonParseErrorType get_object_start(void *state);
static JsonParseErrorType get_object_end(void *state);
static JsonParseErrorType get_object_field_start(void *state, char *fname, bool isnull);
static JsonParseErrorType get_object_field_end(void *state, char *fname, bool isnull);
static JsonParseErrorType get_array_start(void *state);
static JsonParseErrorType get_array_end(void *state);
static JsonParseErrorType get_array_element_start(void *state, bool isnull);
static JsonParseErrorType get_array_element_end(void *state, bool isnull);
static JsonParseErrorType get_scalar(void *state, char *token, JsonTokenType tokentype);

/* json 取值函数的公共工作函数 */
static Datum get_path_all(FunctionCallInfo fcinfo, bool as_text);
static text *get_worker(text *json, char **tpath, int *ipath, int npath,
						bool normalize_results);
static Datum get_jsonb_path_all(FunctionCallInfo fcinfo, bool as_text);
static text *JsonbValueAsText(JsonbValue *v);

/* json_array_length 的语义动作函数 */
static JsonParseErrorType alen_object_start(void *state);
static JsonParseErrorType alen_scalar(void *state, char *token, JsonTokenType tokentype);
static JsonParseErrorType alen_array_element_start(void *state, bool isnull);

/* json{b}_each* 的公共工作函数 */
static Datum each_worker(FunctionCallInfo fcinfo, bool as_text);
static Datum each_worker_jsonb(FunctionCallInfo fcinfo, const char *funcname,
							   bool as_text);

/* json_each 的语义动作函数 */
static JsonParseErrorType each_object_field_start(void *state, char *fname, bool isnull);
static JsonParseErrorType each_object_field_end(void *state, char *fname, bool isnull);
static JsonParseErrorType each_array_start(void *state);
static JsonParseErrorType each_scalar(void *state, char *token, JsonTokenType tokentype);

/* json{b}_array_elements_* 函数的公共工作函数 */
static Datum elements_worker(FunctionCallInfo fcinfo, const char *funcname,
							 bool as_text);
static Datum elements_worker_jsonb(FunctionCallInfo fcinfo, const char *funcname,
								   bool as_text);

/* json_array_elements 的语义动作函数 */
static JsonParseErrorType elements_object_start(void *state);
static JsonParseErrorType elements_array_element_start(void *state, bool isnull);
static JsonParseErrorType elements_array_element_end(void *state, bool isnull);
static JsonParseErrorType elements_scalar(void *state, char *token, JsonTokenType tokentype);

/* 把 json 对象转成哈希表 */
static HTAB *get_json_object_as_hash(char *json, int len, const char *funcname);

/* populate_array_json 的语义动作 */
static JsonParseErrorType populate_array_object_start(void *_state);
static JsonParseErrorType populate_array_array_end(void *_state);
static JsonParseErrorType populate_array_element_start(void *_state, bool isnull);
static JsonParseErrorType populate_array_element_end(void *_state, bool isnull);
static JsonParseErrorType populate_array_scalar(void *_state, char *token, JsonTokenType tokentype);

/* get_json_object_as_hash 的语义动作函数 */
static JsonParseErrorType hash_object_field_start(void *state, char *fname, bool isnull);
static JsonParseErrorType hash_object_field_end(void *state, char *fname, bool isnull);
static JsonParseErrorType hash_array_start(void *state);
static JsonParseErrorType hash_scalar(void *state, char *token, JsonTokenType tokentype);

/* populate_recordset 的语义动作函数 */
static JsonParseErrorType populate_recordset_object_field_start(void *state, char *fname, bool isnull);
static JsonParseErrorType populate_recordset_object_field_end(void *state, char *fname, bool isnull);
static JsonParseErrorType populate_recordset_scalar(void *state, char *token, JsonTokenType tokentype);
static JsonParseErrorType populate_recordset_object_start(void *state);
static JsonParseErrorType populate_recordset_object_end(void *state);
static JsonParseErrorType populate_recordset_array_start(void *state);
static JsonParseErrorType populate_recordset_array_element_start(void *state, bool isnull);

/* json_strip_nulls 的语义动作函数 */
static JsonParseErrorType sn_object_start(void *state);
static JsonParseErrorType sn_object_end(void *state);
static JsonParseErrorType sn_array_start(void *state);
static JsonParseErrorType sn_array_end(void *state);
static JsonParseErrorType sn_object_field_start(void *state, char *fname, bool isnull);
static JsonParseErrorType sn_array_element_start(void *state, bool isnull);
static JsonParseErrorType sn_scalar(void *state, char *token, JsonTokenType tokentype);

/* populate_record、to_record、populate_recordset 和 to_recordset 的工作函数 */
static Datum populate_recordset_worker(FunctionCallInfo fcinfo, const char *funcname,
									   bool is_json, bool have_record_arg);
static Datum populate_record_worker(FunctionCallInfo fcinfo, const char *funcname,
									bool is_json, bool have_record_arg);

/* populate_record[set] 的辅助函数 */
static HeapTupleHeader populate_record(TupleDesc tupdesc, RecordIOData **record_p,
									   HeapTupleHeader defaultval, MemoryContext mcxt,
									   JsObject *obj);
static void get_record_type_from_argument(FunctionCallInfo fcinfo,
										  const char *funcname,
										  PopulateRecordCache *cache);
static void get_record_type_from_query(FunctionCallInfo fcinfo,
									   const char *funcname,
									   PopulateRecordCache *cache);
static void JsValueToJsObject(JsValue *jsv, JsObject *jso);
static Datum populate_composite(CompositeIOData *io, Oid typid,
								const char *colname, MemoryContext mcxt,
								HeapTupleHeader defaultval, JsValue *jsv, bool isnull);
static Datum populate_scalar(ScalarIOData *io, Oid typid, int32 typmod, JsValue *jsv);
static void prepare_column_cache(ColumnIOData *column, Oid typid, int32 typmod,
								 MemoryContext mcxt, bool need_scalar);
static Datum populate_record_field(ColumnIOData *col, Oid typid, int32 typmod,
								   const char *colname, MemoryContext mcxt, Datum defaultval,
								   JsValue *jsv, bool *isnull);
static RecordIOData *allocate_record_info(MemoryContext mcxt, int ncolumns);
static bool JsObjectGetField(JsObject *obj, char *field, JsValue *jsv);
static void populate_recordset_record(PopulateRecordsetState *state, JsObject *obj);
static void populate_array_json(PopulateArrayContext *ctx, char *json, int len);
static void populate_array_dim_jsonb(PopulateArrayContext *ctx, JsonbValue *jbv,
									 int ndim);
static void populate_array_report_expected_array(PopulateArrayContext *ctx, int ndim);
static void populate_array_assign_ndims(PopulateArrayContext *ctx, int ndims);
static void populate_array_check_dimension(PopulateArrayContext *ctx, int ndim);
static void populate_array_element(PopulateArrayContext *ctx, int ndim, JsValue *jsv);
static Datum populate_array(ArrayIOData *aio, const char *colname,
							MemoryContext mcxt, JsValue *jsv);
static Datum populate_domain(DomainIOData *io, Oid typid, const char *colname,
							 MemoryContext mcxt, JsValue *jsv, bool isnull);

/* 支持 jsonb_delete、jsonb_set 和 jsonb_concat 的函数 */
static JsonbValue *IteratorConcat(JsonbIterator **it1, JsonbIterator **it2,
								  JsonbParseState **state);
static JsonbValue *setPath(JsonbIterator **it, Datum *path_elems,
						   bool *path_nulls, int path_len,
						   JsonbParseState **st, int level, JsonbValue *newval,
						   int op_type);
static void setPathObject(JsonbIterator **it, Datum *path_elems,
						  bool *path_nulls, int path_len, JsonbParseState **st,
						  int level,
						  JsonbValue *newval, uint32 npairs, int op_type);
static void setPathArray(JsonbIterator **it, Datum *path_elems,
						 bool *path_nulls, int path_len, JsonbParseState **st,
						 int level,
						 JsonbValue *newval, uint32 nelems, int op_type);

/* 支持 iterate_json_values 的函数 */
static JsonParseErrorType iterate_values_scalar(void *state, char *token, JsonTokenType tokentype);
static JsonParseErrorType iterate_values_object_field_start(void *state, char *fname, bool isnull);

/* 支持 transform_json_string_values 的函数 */
static JsonParseErrorType transform_string_values_object_start(void *state);
static JsonParseErrorType transform_string_values_object_end(void *state);
static JsonParseErrorType transform_string_values_array_start(void *state);
static JsonParseErrorType transform_string_values_array_end(void *state);
static JsonParseErrorType transform_string_values_object_field_start(void *state, char *fname, bool isnull);
static JsonParseErrorType transform_string_values_array_element_start(void *state, bool isnull);
static JsonParseErrorType transform_string_values_scalar(void *state, char *token, JsonTokenType tokentype);


/*
 * pg_parse_json_or_errsave
 *
 * 本函数类似于 pg_parse_json，但不返回
 * JsonParseErrorType。发生任何失败时，本函数都会
 * 若为 ErrorSaveContext 则将错误数据存入 *escontext，否则
 * ereport(ERROR).
 *
 * 返回一个布尔值指示成败（失败只会在 escontext 为
 * ErrorSaveContext 时返回）。
 */
bool
pg_parse_json_or_errsave(JsonLexContext *lex, JsonSemAction *sem,
						 Node *escontext)
{
	JsonParseErrorType result;

	result = pg_parse_json(lex, sem);
	if (result != JSON_SUCCESS)
	{
		json_errsave_error(result, lex, escontext);
		return false;
	}
	return true;
}

/*
 * makeJsonLexContext
 *
 * 类似 makeJsonLexContextCstringLen，但直接接受 text
 * 值。
 */
JsonLexContext *
makeJsonLexContext(text *json, bool need_escapes)
{
	/*
	 * 多数调用者传入去 toast 的 datum，但未必全都如此。
	 * pg_detoast_datum_packed() 是廉价保险。
	 */
	json = pg_detoast_datum_packed(json);

	return makeJsonLexContextCstringLen(VARDATA_ANY(json),
										VARSIZE_ANY_EXHDR(json),
										GetDatabaseEncoding(),
										need_escapes);
}

/*
 * SQL 函数 json_object_keys
 *
 * 返回对象参数的键集合。
 *
 * 本 SRF 以 value-per-call 模式运作，首次调用时
 * 处理对象，键只是被存入一个数组，其大小
 * 按需扩展。这对单个对象的键列表而言大概是
 * 安全的，因为键的大小受限于 NAMEDATALEN，且
 * 键的数量不至于大到
 * 对内存有重大影响。
 */
Datum
jsonb_object_keys(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	OkeysState *state;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		Jsonb	   *jb = PG_GETARG_JSONB_P(0);
		bool		skipNested = false;
		JsonbIterator *it;
		JsonbValue	v;
		JsonbIteratorToken r;

		if (JB_ROOT_IS_SCALAR(jb))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot call %s on a scalar",
							"jsonb_object_keys")));
		else if (JB_ROOT_IS_ARRAY(jb))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot call %s on an array",
							"jsonb_object_keys")));

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		state = palloc(sizeof(OkeysState));

		state->result_size = JB_ROOT_COUNT(jb);
		state->result_count = 0;
		state->sent_count = 0;
		state->result = palloc(state->result_size * sizeof(char *));

		it = JsonbIteratorInit(&jb->root);

		while ((r = JsonbIteratorNext(&it, &v, skipNested)) != WJB_DONE)
		{
			skipNested = true;

			if (r == WJB_KEY)
			{
				char	   *cstr;

				cstr = palloc(v.val.string.len + 1 * sizeof(char));
				memcpy(cstr, v.val.string.val, v.val.string.len);
				cstr[v.val.string.len] = '\0';
				state->result[state->result_count++] = cstr;
			}
		}

		MemoryContextSwitchTo(oldcontext);
		funcctx->user_fctx = (void *) state;
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (OkeysState *) funcctx->user_fctx;

	if (state->sent_count < state->result_count)
	{
		char	   *nxt = state->result[state->sent_count++];

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(nxt));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * 报告JSON 错误。
 */
void
json_errsave_error(JsonParseErrorType error, JsonLexContext *lex,
				   Node *escontext)
{
	if (error == JSON_UNICODE_HIGH_ESCAPE ||
		error == JSON_UNICODE_UNTRANSLATABLE ||
		error == JSON_UNICODE_CODE_POINT_ZERO)
		errsave(escontext,
				(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
				 errmsg("unsupported Unicode escape sequence"),
				 errdetail_internal("%s", json_errdetail(error, lex)),
				 report_json_context(lex)));
	else if (error == JSON_SEM_ACTION_FAILED)
	{
		/* 语义动作函数最好已经报告了些什么 */
		if (!SOFT_ERROR_OCCURRED(escontext))
			elog(ERROR, "JSON semantic action function did not provide error information");
	}
	else
		errsave(escontext,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "json"),
				 errdetail_internal("%s", json_errdetail(error, lex)),
				 report_json_context(lex)));
}

/*
 * 为无效 JSON 输入报告 CONTEXT 行。
 *
 * 必须设置 lex->token_terminator 以标识我们检测到错误的
 * 位置。注意 lex->token_start 可能为 NULL，比如 EOF 处识别出
 * 错误。
 *
 * 返回值并无意义，但我们将其设为非 void，以便本函数
 * 能在 ereport() 内被调用。
 */
static int
report_json_context(JsonLexContext *lex)
{
	const char *context_start;
	const char *context_end;
	const char *line_start;
	char	   *ctxt;
	int			ctxtlen;
	const char *prefix;
	const char *suffix;

	/* 为要显示的输入部分选择边界 */
	line_start = lex->line_start;
	context_start = line_start;
	context_end = lex->token_terminator;
	Assert(context_end >= context_start);

	/* 前进到足够接近 context_end 为止 */
	while (context_end - context_start >= 50)
	{
		/* 前进到下一个多字节字符 */
		if (IS_HIGHBIT_SET(*context_start))
			context_start += pg_mblen(context_start);
		else
			context_start++;
	}

	/*
	 * 我们加上 "..." 来指示摘录不是从行首开始的，
	 * 但如果距行首不足 3 个字符，那不如干脆显示
	 * 整行。
	 */
	if (context_start - line_start <= 3)
		context_start = line_start;

	/* 获取以 null 结尾的数据副本供展示 */
	ctxtlen = context_end - context_start;
	ctxt = palloc(ctxtlen + 1);
	memcpy(ctxt, context_start, ctxtlen);
	ctxt[ctxtlen] = '\0';

	/*
	 * 显示上下文，若不是从行首开始则前缀 "..."，若不是
	 * 在行尾结束则后缀 "..."。
	 */
	prefix = (context_start > line_start) ? "..." : "";
	suffix = (lex->token_type != JSON_TOKEN_END &&
			  context_end - lex->input < lex->input_length &&
			  *context_end != '\n' && *context_end != '\r') ? "..." : "";

	return errcontext("JSON data, line %d: %s%s%s",
					  lex->line_number, prefix, ctxt, suffix);
}


Datum
json_object_keys(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	OkeysState *state;

	if (SRF_IS_FIRSTCALL())
	{
		text	   *json = PG_GETARG_TEXT_PP(0);
		JsonLexContext *lex = makeJsonLexContext(json, true);
		JsonSemAction *sem;
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		state = palloc(sizeof(OkeysState));
		sem = palloc0(sizeof(JsonSemAction));

		state->lex = lex;
		state->result_size = 256;
		state->result_count = 0;
		state->sent_count = 0;
		state->result = palloc(256 * sizeof(char *));

		sem->semstate = (void *) state;
		sem->array_start = okeys_array_start;
		sem->scalar = okeys_scalar;
		sem->object_field_start = okeys_object_field_start;
		/* 其余都是 NULL，拜上面的 palloc0 所赐 */

		pg_parse_json_or_ereport(lex, sem);
		/* 键现在位于 state->result */

		pfree(lex->strval->data);
		pfree(lex->strval);
		pfree(lex);
		pfree(sem);

		MemoryContextSwitchTo(oldcontext);
		funcctx->user_fctx = (void *) state;
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (OkeysState *) funcctx->user_fctx;

	if (state->sent_count < state->result_count)
	{
		char	   *nxt = state->result[state->sent_count++];

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(nxt));
	}

	SRF_RETURN_DONE(funcctx);
}

static JsonParseErrorType
okeys_object_field_start(void *state, char *fname, bool isnull)
{
	OkeysState *_state = (OkeysState *) state;

	/* 只为顶层对象收集键 */
	if (_state->lex->lex_level != 1)
		return JSON_SUCCESS;

	/* 必要时扩大结果数组 */
	if (_state->result_count >= _state->result_size)
	{
		_state->result_size *= 2;
		_state->result = (char **)
			repalloc(_state->result, sizeof(char *) * _state->result_size);
	}

	/* 保存字段名的副本 */
	_state->result[_state->result_count++] = pstrdup(fname);

	return JSON_SUCCESS;
}

static JsonParseErrorType
okeys_array_start(void *state)
{
	OkeysState *_state = (OkeysState *) state;

	/* 顶层必须是 json 对象 */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on an array",
						"json_object_keys")));

	return JSON_SUCCESS;
}

static JsonParseErrorType
okeys_scalar(void *state, char *token, JsonTokenType tokentype)
{
	OkeysState *_state = (OkeysState *) state;

	/* 顶层必须是 json 对象 */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on a scalar",
						"json_object_keys")));

	return JSON_SUCCESS;
}

/*
 * json 与 jsonb 取值函数
 * 它们实现了 -> ->> #> 和 #>> 算子
 * 以及 json{b?}_extract_path*(json, text, ...) 函数
 */


Datum
json_object_field(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_PP(0);
	text	   *fname = PG_GETARG_TEXT_PP(1);
	char	   *fnamestr = text_to_cstring(fname);
	text	   *result;

	result = get_worker(json, &fnamestr, NULL, 1, false);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

Datum
jsonb_object_field(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	text	   *key = PG_GETARG_TEXT_PP(1);
	JsonbValue *v;
	JsonbValue	vbuf;

	if (!JB_ROOT_IS_OBJECT(jb))
		PG_RETURN_NULL();

	v = getKeyJsonValueFromContainer(&jb->root,
									 VARDATA_ANY(key),
									 VARSIZE_ANY_EXHDR(key),
									 &vbuf);

	if (v != NULL)
		PG_RETURN_JSONB_P(JsonbValueToJsonb(v));

	PG_RETURN_NULL();
}

Datum
json_object_field_text(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_PP(0);
	text	   *fname = PG_GETARG_TEXT_PP(1);
	char	   *fnamestr = text_to_cstring(fname);
	text	   *result;

	result = get_worker(json, &fnamestr, NULL, 1, true);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

Datum
jsonb_object_field_text(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	text	   *key = PG_GETARG_TEXT_PP(1);
	JsonbValue *v;
	JsonbValue	vbuf;

	if (!JB_ROOT_IS_OBJECT(jb))
		PG_RETURN_NULL();

	v = getKeyJsonValueFromContainer(&jb->root,
									 VARDATA_ANY(key),
									 VARSIZE_ANY_EXHDR(key),
									 &vbuf);

	if (v != NULL && v->type != jbvNull)
		PG_RETURN_TEXT_P(JsonbValueAsText(v));

	PG_RETURN_NULL();
}

Datum
json_array_element(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_PP(0);
	int			element = PG_GETARG_INT32(1);
	text	   *result;

	result = get_worker(json, NULL, &element, 1, false);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

Datum
jsonb_array_element(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	int			element = PG_GETARG_INT32(1);
	JsonbValue *v;

	if (!JB_ROOT_IS_ARRAY(jb))
		PG_RETURN_NULL();

	/* 处理负下标 */
	if (element < 0)
	{
		uint32		nelements = JB_ROOT_COUNT(jb);

		if (-element > nelements)
			PG_RETURN_NULL();
		else
			element += nelements;
	}

	v = getIthJsonbValueFromContainer(&jb->root, element);
	if (v != NULL)
		PG_RETURN_JSONB_P(JsonbValueToJsonb(v));

	PG_RETURN_NULL();
}

Datum
json_array_element_text(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_PP(0);
	int			element = PG_GETARG_INT32(1);
	text	   *result;

	result = get_worker(json, NULL, &element, 1, true);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

Datum
jsonb_array_element_text(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	int			element = PG_GETARG_INT32(1);
	JsonbValue *v;

	if (!JB_ROOT_IS_ARRAY(jb))
		PG_RETURN_NULL();

	/* 处理负下标 */
	if (element < 0)
	{
		uint32		nelements = JB_ROOT_COUNT(jb);

		if (-element > nelements)
			PG_RETURN_NULL();
		else
			element += nelements;
	}

	v = getIthJsonbValueFromContainer(&jb->root, element);

	if (v != NULL && v->type != jbvNull)
		PG_RETURN_TEXT_P(JsonbValueAsText(v));

	PG_RETURN_NULL();
}

Datum
json_extract_path(PG_FUNCTION_ARGS)
{
	return get_path_all(fcinfo, false);
}

Datum
json_extract_path_text(PG_FUNCTION_ARGS)
{
	return get_path_all(fcinfo, true);
}

/*
 * extract_path 系列函数的公共例程
 */
static Datum
get_path_all(FunctionCallInfo fcinfo, bool as_text)
{
	text	   *json = PG_GETARG_TEXT_PP(0);
	ArrayType  *path = PG_GETARG_ARRAYTYPE_P(1);
	text	   *result;
	Datum	   *pathtext;
	bool	   *pathnulls;
	int			npath;
	char	  **tpath;
	int		   *ipath;
	int			i;

	/*
	 * 若数组含有任何 null 元素则返回 NULL——因为在逐层
	 * 应用 -> 算子的序列中，任一 RHS 值为 NULL 都将得到
	 * NULL。（注：由于像无此字段这样的错误情况我们
	 * 也会返回 NULL，所以无论数组其余内容如何这一
	 * 点都成立。）
	 */
	if (array_contains_nulls(path))
		PG_RETURN_NULL();

	deconstruct_array_builtin(path, TEXTOID, &pathtext, &pathnulls, &npath);

	tpath = palloc(npath * sizeof(char *));
	ipath = palloc(npath * sizeof(int));

	for (i = 0; i < npath; i++)
	{
		Assert(!pathnulls[i]);
		tpath[i] = TextDatumGetCString(pathtext[i]);

		/*
			 * 此阶段不知文档结构，故把路径中能转成整数
			 * 的都转为整数，其余设为永不匹配的 INT_MIN。
			 *
		 */
		if (*tpath[i] != '\0')
		{
			int			ind;
			char	   *endptr;

			errno = 0;
			ind = strtoint(tpath[i], &endptr, 10);
			if (endptr == tpath[i] || *endptr != '\0' || errno != 0)
				ipath[i] = INT_MIN;
			else
				ipath[i] = ind;
		}
		else
			ipath[i] = INT_MIN;
	}

	result = get_worker(json, tpath, ipath, npath, as_text);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

/*
 * get_worker
 *
 * 所有 json 取值函数的公共工作函数
 *
 * json: 文本形式 JSON 对象
 * tpath[]: 要提取的字段名
 * ipath[]: 要提取的数组下标（0 起算），接受负值
 * npath: tpath[] / ipath[] 的长度
 * normalize_results: true 时去转义字符串和 null 标量
 *
 * tpath 可为 NULL，或任一 tpath[] 元素可为 NULL——若
 * 对象字段无需在该嵌套层级匹配。类似地，ipath 可为
 * NULL，或任一 ipath[] 元素可为 INT_MIN——若数组元素无
 * 需在该嵌套层级匹配（json 数据绝不可能大到拥有
 * -INT_MIN 个元素，因为受 MaxAllocSize 限制）。
 */
static text *
get_worker(text *json,
		   char **tpath,
		   int *ipath,
		   int npath,
		   bool normalize_results)
{
	JsonLexContext *lex = makeJsonLexContext(json, true);
	JsonSemAction *sem = palloc0(sizeof(JsonSemAction));
	GetState   *state = palloc0(sizeof(GetState));

	Assert(npath >= 0);

	state->lex = lex;
	/* 是 "_as_text" 变体吗? */
	state->normalize_results = normalize_results;
	state->npath = npath;
	state->path_names = tpath;
	state->path_indexes = ipath;
	state->pathok = palloc0(sizeof(bool) * npath);
	state->array_cur_index = palloc(sizeof(int) * npath);

	if (npath > 0)
		state->pathok[0] = true;

	sem->semstate = (void *) state;

	/*
	 * 并非所有变体需要全部语义例程。只设置实际需要
	 * 的那些，以求最大效率。
	 */
	sem->scalar = get_scalar;
	if (npath == 0)
	{
		sem->object_start = get_object_start;
		sem->object_end = get_object_end;
		sem->array_start = get_array_start;
		sem->array_end = get_array_end;
	}
	if (tpath != NULL)
	{
		sem->object_field_start = get_object_field_start;
		sem->object_field_end = get_object_field_end;
	}
	if (ipath != NULL)
	{
		sem->array_start = get_array_start;
		sem->array_element_start = get_array_element_start;
		sem->array_element_end = get_array_element_end;
	}

	pg_parse_json_or_ereport(lex, sem);

	return state->tresult;
}

static JsonParseErrorType
get_object_start(void *state)
{
	GetState   *_state = (GetState *) state;
	int			lex_level = _state->lex->lex_level;

	if (lex_level == 0 && _state->npath == 0)
	{
		/*
			 * 特例：应匹配整个对象。这只需
			 * 在最外层做，因为嵌套层级的匹配已由外层
			 * 字段或数组元素回调启动。
		 */
		_state->result_start = _state->lex->token_start;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
get_object_end(void *state)
{
	GetState   *_state = (GetState *) state;
	int			lex_level = _state->lex->lex_level;

	if (lex_level == 0 && _state->npath == 0)
	{
		/* 特例：返回整个对象 */
		char	   *start = _state->result_start;
		int			len = _state->lex->prev_token_terminator - start;

		_state->tresult = cstring_to_text_with_len(start, len);
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
get_object_field_start(void *state, char *fname, bool isnull)
{
	GetState   *_state = (GetState *) state;
	bool		get_next = false;
	int			lex_level = _state->lex->lex_level;

	if (lex_level <= _state->npath &&
		_state->pathok[lex_level - 1] &&
		_state->path_names != NULL &&
		_state->path_names[lex_level - 1] != NULL &&
		strcmp(fname, _state->path_names[lex_level - 1]) == 0)
	{
		if (lex_level < _state->npath)
		{
			/* 若未到路径末尾则只标记匹配 */
			_state->pathok[lex_level] = true;
		}
		else
		{
			/* 路径结束，我们要这个值 */
			get_next = true;
		}
	}

	if (get_next)
	{
		/* 该对象覆盖之前任何匹配的对象 */
		_state->tresult = NULL;
		_state->result_start = NULL;

		if (_state->normalize_results &&
			_state->lex->token_type == JSON_TOKEN_STRING)
		{
			/* 对 as_text 变体，让 get_scalar 代为设置 */
			_state->next_scalar = true;
		}
		else
		{
			/* 对非 as_text 变体，只记录 json 起始点 */
			_state->result_start = _state->lex->token_start;
		}
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
get_object_field_end(void *state, char *fname, bool isnull)
{
	GetState   *_state = (GetState *) state;
	bool		get_last = false;
	int			lex_level = _state->lex->lex_level;

	/* 同 get_object_field_start 的测试 */
	if (lex_level <= _state->npath &&
		_state->pathok[lex_level - 1] &&
		_state->path_names != NULL &&
		_state->path_names[lex_level - 1] != NULL &&
		strcmp(fname, _state->path_names[lex_level - 1]) == 0)
	{
		if (lex_level < _state->npath)
		{
			/* 此字段完毕，重置 pathok */
			_state->pathok[lex_level] = false;
		}
		else
		{
			/* 路径结束，我们要这个值 */
			get_last = true;
		}
	}

	/* as_text 标量情形下工作已完成 */
	if (get_last && _state->result_start != NULL)
	{
		/*
			 * 用先前记录的 json 起点到上一记号末尾的字符串
			 * 构造 text 对象（词法分析器已越过
			 * 我们感兴趣的内容之后的部分）。
		 */
		if (isnull && _state->normalize_results)
			_state->tresult = (text *) NULL;
		else
		{
			char	   *start = _state->result_start;
			int			len = _state->lex->prev_token_terminator - start;

			_state->tresult = cstring_to_text_with_len(start, len);
		}

		/* 这本无必要，但为了整洁还是做一下： */
		_state->result_start = NULL;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
get_array_start(void *state)
{
	GetState   *_state = (GetState *) state;
	int			lex_level = _state->lex->lex_level;

	if (lex_level < _state->npath)
	{
		/* 初始化该数组中元素的计数 */
		_state->array_cur_index[lex_level] = -1;

		/* INT_MIN 值保留用于表示无效下标 */
		if (_state->path_indexes[lex_level] < 0 &&
			_state->path_indexes[lex_level] != INT_MIN)
		{
			/* 负下标——转换为正向计的下标 */
			JsonParseErrorType error;
			int			nelements;

			error = json_count_array_elements(_state->lex, &nelements);
			if (error != JSON_SUCCESS)
				json_errsave_error(error, _state->lex, NULL);

			if (-_state->path_indexes[lex_level] <= nelements)
				_state->path_indexes[lex_level] += nelements;
		}
	}
	else if (lex_level == 0 && _state->npath == 0)
	{
		/*
			 * 特例：应匹配整个数组。这只需在最外层做，
			 * 因为嵌套层级的匹配已由外层字段或数组元素
			 * 回调启动。
		 */
		_state->result_start = _state->lex->token_start;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
get_array_end(void *state)
{
	GetState   *_state = (GetState *) state;
	int			lex_level = _state->lex->lex_level;

	if (lex_level == 0 && _state->npath == 0)
	{
		/* 特例：返回整个数组 */
		char	   *start = _state->result_start;
		int			len = _state->lex->prev_token_terminator - start;

		_state->tresult = cstring_to_text_with_len(start, len);
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
get_array_element_start(void *state, bool isnull)
{
	GetState   *_state = (GetState *) state;
	bool		get_next = false;
	int			lex_level = _state->lex->lex_level;

	/* 更新数组元素计数器 */
	if (lex_level <= _state->npath)
		_state->array_cur_index[lex_level - 1]++;

	if (lex_level <= _state->npath &&
		_state->pathok[lex_level - 1] &&
		_state->path_indexes != NULL &&
		_state->array_cur_index[lex_level - 1] == _state->path_indexes[lex_level - 1])
	{
		if (lex_level < _state->npath)
		{
			/* 若未到路径末尾则只标记匹配 */
			_state->pathok[lex_level] = true;
		}
		else
		{
			/* 路径结束，我们要这个值 */
			get_next = true;
		}
	}

	/* 与对象相同的逻辑 */
	if (get_next)
	{
		_state->tresult = NULL;
		_state->result_start = NULL;

		if (_state->normalize_results &&
			_state->lex->token_type == JSON_TOKEN_STRING)
		{
			_state->next_scalar = true;
		}
		else
		{
			_state->result_start = _state->lex->token_start;
		}
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
get_array_element_end(void *state, bool isnull)
{
	GetState   *_state = (GetState *) state;
	bool		get_last = false;
	int			lex_level = _state->lex->lex_level;

	/* 同 get_array_element_start 的测试 */
	if (lex_level <= _state->npath &&
		_state->pathok[lex_level - 1] &&
		_state->path_indexes != NULL &&
		_state->array_cur_index[lex_level - 1] == _state->path_indexes[lex_level - 1])
	{
		if (lex_level < _state->npath)
		{
			/* 此元素完毕，重置 pathok */
			_state->pathok[lex_level] = false;
		}
		else
		{
			/* 路径结束，我们要这个值 */
			get_last = true;
		}
	}

	/* 与对象相同的逻辑 */
	if (get_last && _state->result_start != NULL)
	{
		if (isnull && _state->normalize_results)
			_state->tresult = (text *) NULL;
		else
		{
			char	   *start = _state->result_start;
			int			len = _state->lex->prev_token_terminator - start;

			_state->tresult = cstring_to_text_with_len(start, len);
		}

		_state->result_start = NULL;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
get_scalar(void *state, char *token, JsonTokenType tokentype)
{
	GetState   *_state = (GetState *) state;
	int			lex_level = _state->lex->lex_level;

	/* 检查整个对象匹配 */
	if (lex_level == 0 && _state->npath == 0)
	{
		if (_state->normalize_results && tokentype == JSON_TOKEN_STRING)
		{
			/* 要去转义后的字符串 */
			_state->next_scalar = true;
		}
		else if (_state->normalize_results && tokentype == JSON_TOKEN_NULL)
		{
			_state->tresult = (text *) NULL;
		}
		else
		{
			/*
				 * 有点凑合：我们抑制标量记号后的空白，
				 * 但不抑制其前空白。大概不值得为此专门
				 * 跳过空格来避免它。
			 */
			char	   *start = _state->lex->input;
			int			len = _state->lex->prev_token_terminator - start;

			_state->tresult = cstring_to_text_with_len(start, len);
		}
	}

	if (_state->next_scalar)
	{
		/* 需要去转义的文本值，因此提供它 */
		_state->tresult = cstring_to_text(token);
		/* 确保下一次 get_scalar 调用不会覆盖它 */
		_state->next_scalar = false;
	}

	return JSON_SUCCESS;
}

Datum
jsonb_extract_path(PG_FUNCTION_ARGS)
{
	return get_jsonb_path_all(fcinfo, false);
}

Datum
jsonb_extract_path_text(PG_FUNCTION_ARGS)
{
	return get_jsonb_path_all(fcinfo, true);
}

static Datum
get_jsonb_path_all(FunctionCallInfo fcinfo, bool as_text)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	ArrayType  *path = PG_GETARG_ARRAYTYPE_P(1);
	Datum	   *pathtext;
	bool	   *pathnulls;
	bool		isnull;
	int			npath;
	Datum		res;

	/*
	 * 若数组含有任何 null 元素则返回 NULL——因为在逐层
	 * 应用 -> 算子的序列中，任一 RHS 值为 NULL 都将得到
	 * NULL。（注：由于像无此字段这样的错误情况我们
	 * 也会返回 NULL，所以无论数组其余内容如何这一
	 * 点都成立。）
	 */
	if (array_contains_nulls(path))
		PG_RETURN_NULL();

	deconstruct_array_builtin(path, TEXTOID, &pathtext, &pathnulls, &npath);

	res = jsonb_get_element(jb, pathtext, npath, &isnull, as_text);

	if (isnull)
		PG_RETURN_NULL();
	else
		PG_RETURN_DATUM(res);
}

Datum
jsonb_get_element(Jsonb *jb, Datum *path, int npath, bool *isnull, bool as_text)
{
	JsonbContainer *container = &jb->root;
	JsonbValue *jbvp = NULL;
	int			i;
	bool		have_object = false,
				have_array = false;

	*isnull = false;

	/* 判明顶层是对象、数组还是标量 */
	if (JB_ROOT_IS_OBJECT(jb))
		have_object = true;
	else if (JB_ROOT_IS_ARRAY(jb) && !JB_ROOT_IS_SCALAR(jb))
		have_array = true;
	else
	{
		Assert(JB_ROOT_IS_ARRAY(jb) && JB_ROOT_IS_SCALAR(jb));
		/* 若要返回的是标量值则提取它 */
		if (npath <= 0)
			jbvp = getIthJsonbValueFromContainer(container, 0);
	}

	/*
	 * 若数组为空，返回整个 LHS 对象，理由是我们应当
	 * 做零次字段或元素提取。对非标量情形可以
	 * 不费什么事就把对象交回去。对标量情形则落到
	 * 循环下方处理该值。
	 * （这种不一致源于无法轻松地为根级容器直接生成
	 * JsonbValue。）
	 */
	if (npath <= 0 && jbvp == NULL)
	{
		if (as_text)
		{
			return PointerGetDatum(cstring_to_text(JsonbToCString(NULL,
																  container,
																  VARSIZE(jb))));
		}
		else
		{
			/* 非文本模式——直接交回 jsonb */
			PG_RETURN_JSONB_P(jb);
		}
	}

	for (i = 0; i < npath; i++)
	{
		if (have_object)
		{
			text	   *subscr = DatumGetTextPP(path[i]);

			jbvp = getKeyJsonValueFromContainer(container,
												VARDATA_ANY(subscr),
												VARSIZE_ANY_EXHDR(subscr),
												NULL);
		}
		else if (have_array)
		{
			int			lindex;
			uint32		index;
			char	   *indextext = TextDatumGetCString(path[i]);
			char	   *endptr;

			errno = 0;
			lindex = strtoint(indextext, &endptr, 10);
			if (endptr == indextext || *endptr != '\0' || errno != 0)
			{
				*isnull = true;
				return PointerGetDatum(NULL);
			}

			if (lindex >= 0)
			{
				index = (uint32) lindex;
			}
			else
			{
				/* 处理负下标 */
				uint32		nelements;

				/* 容器必须是数组，但需确认 */
				if (!JsonContainerIsArray(container))
					elog(ERROR, "not a jsonb array");

				nelements = JsonContainerSize(container);

				if (lindex == INT_MIN || -lindex > nelements)
				{
					*isnull = true;
					return PointerGetDatum(NULL);
				}
				else
					index = nelements + lindex;
			}

			jbvp = getIthJsonbValueFromContainer(container, index);
		}
		else
		{
			/* 标量，则提取得到 null */
			*isnull = true;
			return PointerGetDatum(NULL);
		}

		if (jbvp == NULL)
		{
			*isnull = true;
			return PointerGetDatum(NULL);
		}
		else if (i == npath - 1)
			break;

		if (jbvp->type == jbvBinary)
		{
			container = jbvp->val.binary.data;
			have_object = JsonContainerIsObject(container);
			have_array = JsonContainerIsArray(container);
			Assert(!JsonContainerIsScalar(container));
		}
		else
		{
			Assert(IsAJsonbScalar(jbvp));
			have_object = false;
			have_array = false;
		}
	}

	if (as_text)
	{
		if (jbvp->type == jbvNull)
		{
			*isnull = true;
			return PointerGetDatum(NULL);
		}

		return PointerGetDatum(JsonbValueAsText(jbvp));
	}
	else
	{
		Jsonb	   *res = JsonbValueToJsonb(jbvp);

		/* 非文本模式——直接交回 jsonb */
		PG_RETURN_JSONB_P(res);
	}
}

Datum
jsonb_set_element(Jsonb *jb, Datum *path, int path_len,
				  JsonbValue *newval)
{
	JsonbValue *res;
	JsonbParseState *state = NULL;
	JsonbIterator *it;
	bool	   *path_nulls = palloc0(path_len * sizeof(bool));

	if (newval->type == jbvArray && newval->val.array.rawScalar)
		*newval = newval->val.array.elems[0];

	it = JsonbIteratorInit(&jb->root);

	res = setPath(&it, path, path_nulls, path_len, &state, 0, newval,
				  JB_PATH_CREATE | JB_PATH_FILL_GAPS |
				  JB_PATH_CONSISTENT_POSITION);

	pfree(path_nulls);

	PG_RETURN_JSONB_P(JsonbValueToJsonb(res));
}

static void
push_null_elements(JsonbParseState **ps, int num)
{
	JsonbValue	null;

	null.type = jbvNull;

	while (num-- > 0)
		pushJsonbValue(ps, WJB_ELEM, &null);
}

/*
 * 准备一个包含嵌套空对象和空数组的新结构，
 * 使之对应指定路径，并在该路径末尾赋予新值。
 * 例如带新值 1 的路径 [a][0][b] 将产生结构
 * {a: [{b: 1}]}。
 *
 * 调用者负责确保这样的路径尚不存在。
 */
static void
push_path(JsonbParseState **st, int level, Datum *path_elems,
		  bool *path_nulls, int path_len, JsonbValue *newval)
{
	/*
	 * tpath 包含每一层级上创建的空 jsonb 的预期类型，
	 * 即高于或等于当前层级者，为 jbvObject 或 jbvArray。
	 * 由于它只包含从某层级到末尾的路径切片信息，
	 * 访问下标必须按层级归一化。
	 */
	enum jbvType *tpath = palloc0((path_len - level) * sizeof(enum jbvType));
	JsonbValue	newkey;

	/*
	 * 用起始记号创建链的第一部分。当前层级的
	 * WJB_BEGIN_OBJECT/WJB_BEGIN_ARRAY 已创建，故从
	 * 下一项起。
	 */
	for (int i = level + 1; i < path_len; i++)
	{
		char	   *c,
				   *badp;
		int			lindex;

		if (path_nulls[i])
			break;

		/*
			 * 尝试转成整数以查明预期类型是对象还是数组。
			 *
		 */
		c = TextDatumGetCString(path_elems[i]);
		errno = 0;
		lindex = strtoint(c, &badp, 10);
		if (badp == c || *badp != '\0' || errno != 0)
		{
			/* 文本，预期是对象 */
			newkey.type = jbvString;
			newkey.val.string.val = c;
			newkey.val.string.len = strlen(c);

			(void) pushJsonbValue(st, WJB_BEGIN_OBJECT, NULL);
			(void) pushJsonbValue(st, WJB_KEY, &newkey);

			tpath[i - level] = jbvObject;
		}
		else
		{
			/* 整数，预期是数组 */
			(void) pushJsonbValue(st, WJB_BEGIN_ARRAY, NULL);

			push_null_elements(st, lindex);

			tpath[i - level] = jbvArray;
		}
	}

	/* 为对象或数组插入实际值 */
	if (tpath[(path_len - level) - 1] == jbvArray)
	{
		(void) pushJsonbValue(st, WJB_ELEM, newval);
	}
	else
		(void) pushJsonbValue(st, WJB_VALUE, newval);

	/*
	 * 闭合到倒数第二层为止的一切。最后一层将由
	 * 本函数之外闭合。
	 */
	for (int i = path_len - 1; i > level; i--)
	{
		if (path_nulls[i])
			break;

		if (tpath[i - level] == jbvObject)
			(void) pushJsonbValue(st, WJB_END_OBJECT, NULL);
		else
			(void) pushJsonbValue(st, WJB_END_ARRAY, NULL);
	}
}

/*
 * 返回给定 JsonbValue 的文本表示。
 */
static text *
JsonbValueAsText(JsonbValue *v)
{
	switch (v->type)
	{
		case jbvNull:
			return NULL;

		case jbvBool:
			return v->val.boolean ?
				cstring_to_text_with_len("true", 4) :
				cstring_to_text_with_len("false", 5);

		case jbvString:
			return cstring_to_text_with_len(v->val.string.val,
											v->val.string.len);

		case jbvNumeric:
			{
				Datum		cstr;

				cstr = DirectFunctionCall1(numeric_out,
										   PointerGetDatum(v->val.numeric));

				return cstring_to_text(DatumGetCString(cstr));
			}

		case jbvBinary:
			{
				StringInfoData jtext;

				initStringInfo(&jtext);
				(void) JsonbToCString(&jtext, v->val.binary.data,
									  v->val.binary.len);

				return cstring_to_text_with_len(jtext.data, jtext.len);
			}

		default:
			elog(ERROR, "unrecognized jsonb type: %d", (int) v->type);
			return NULL;
	}
}

/*
 * SQL 函数 json_array_length(json) -> int
 */
Datum
json_array_length(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_PP(0);
	AlenState  *state;
	JsonLexContext *lex;
	JsonSemAction *sem;

	lex = makeJsonLexContext(json, false);
	state = palloc0(sizeof(AlenState));
	sem = palloc0(sizeof(JsonSemAction));

	/* palloc0 代劳 */
#if 0
	state->count = 0;
#endif
	state->lex = lex;

	sem->semstate = (void *) state;
	sem->object_start = alen_object_start;
	sem->scalar = alen_scalar;
	sem->array_element_start = alen_array_element_start;

	pg_parse_json_or_ereport(lex, sem);

	PG_RETURN_INT32(state->count);
}

Datum
jsonb_array_length(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);

	if (JB_ROOT_IS_SCALAR(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot get array length of a scalar")));
	else if (!JB_ROOT_IS_ARRAY(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot get array length of a non-array")));

	PG_RETURN_INT32(JB_ROOT_COUNT(jb));
}

/*
 * 接下来这两个检查确保 json 是数组（因为它不可能是
 * 标量或对象）。
 */

static JsonParseErrorType
alen_object_start(void *state)
{
	AlenState  *_state = (AlenState *) state;

	/* json 结构检查 */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot get array length of a non-array")));

	return JSON_SUCCESS;
}

static JsonParseErrorType
alen_scalar(void *state, char *token, JsonTokenType tokentype)
{
	AlenState  *_state = (AlenState *) state;

	/* json 结构检查 */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot get array length of a scalar")));

	return JSON_SUCCESS;
}

static JsonParseErrorType
alen_array_element_start(void *state, bool isnull)
{
	AlenState  *_state = (AlenState *) state;

	/* 只需数一遍所有 1 层元素 */
	if (_state->lex->lex_level == 1)
		_state->count++;

	return JSON_SUCCESS;
}

/*
 * SQL 函数 json_each 与 json_each_text
 *
 * 把 json 对象分解为键值对。
 *
 * 与 json_object_keys() 不同，它们以物化模式运作，
 * 边执行边把结果存入 Tuplestore 对象。
 * 元组的构造用一个临时内存上下文完成，该上下文
 * 于每个元组构建后清空。
 */
Datum
json_each(PG_FUNCTION_ARGS)
{
	return each_worker(fcinfo, false);
}

Datum
jsonb_each(PG_FUNCTION_ARGS)
{
	return each_worker_jsonb(fcinfo, "jsonb_each", false);
}

Datum
json_each_text(PG_FUNCTION_ARGS)
{
	return each_worker(fcinfo, true);
}

Datum
jsonb_each_text(PG_FUNCTION_ARGS)
{
	return each_worker_jsonb(fcinfo, "jsonb_each_text", true);
}

static Datum
each_worker_jsonb(FunctionCallInfo fcinfo, const char *funcname, bool as_text)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	ReturnSetInfo *rsi;
	MemoryContext old_cxt,
				tmp_cxt;
	bool		skipNested = false;
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken r;

	if (!JB_ROOT_IS_OBJECT(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on a non-object",
						funcname)));

	rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	InitMaterializedSRF(fcinfo, MAT_SRF_BLESS);

	tmp_cxt = AllocSetContextCreate(CurrentMemoryContext,
									"jsonb_each temporary cxt",
									ALLOCSET_DEFAULT_SIZES);

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, skipNested)) != WJB_DONE)
	{
		skipNested = true;

		if (r == WJB_KEY)
		{
			text	   *key;
			Datum		values[2];
			bool		nulls[2] = {false, false};

			/* 使用 tmp 上下文，以便每个元组完成后清理 */
			old_cxt = MemoryContextSwitchTo(tmp_cxt);

			key = cstring_to_text_with_len(v.val.string.val, v.val.string.len);

			/*
			 * 迭代器取的下一个东西应是值，无论其形状
			 * 如何。
			 */
			r = JsonbIteratorNext(&it, &v, skipNested);
			Assert(r != WJB_DONE);

			values[0] = PointerGetDatum(key);

			if (as_text)
			{
				if (v.type == jbvNull)
				{
					/* 文本模式下 json null 即 sql null */
					nulls[1] = true;
					values[1] = (Datum) NULL;
				}
				else
					values[1] = PointerGetDatum(JsonbValueAsText(&v));
			}
			else
			{
				/* 非文本模式，直接返回 Jsonb */
				Jsonb	   *val = JsonbValueToJsonb(&v);

				values[1] = PointerGetDatum(val);
			}

			tuplestore_putvalues(rsi->setResult, rsi->setDesc, values, nulls);

			/* 清理并切回 */
			MemoryContextSwitchTo(old_cxt);
			MemoryContextReset(tmp_cxt);
		}
	}

	MemoryContextDelete(tmp_cxt);

	PG_RETURN_NULL();
}


static Datum
each_worker(FunctionCallInfo fcinfo, bool as_text)
{
	text	   *json = PG_GETARG_TEXT_PP(0);
	JsonLexContext *lex;
	JsonSemAction *sem;
	ReturnSetInfo *rsi;
	EachState  *state;

	lex = makeJsonLexContext(json, true);
	state = palloc0(sizeof(EachState));
	sem = palloc0(sizeof(JsonSemAction));

	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, MAT_SRF_BLESS);
	state->tuple_store = rsi->setResult;
	state->ret_tdesc = rsi->setDesc;

	sem->semstate = (void *) state;
	sem->array_start = each_array_start;
	sem->scalar = each_scalar;
	sem->object_field_start = each_object_field_start;
	sem->object_field_end = each_object_field_end;

	state->normalize_results = as_text;
	state->next_scalar = false;
	state->lex = lex;
	state->tmp_cxt = AllocSetContextCreate(CurrentMemoryContext,
										   "json_each temporary cxt",
										   ALLOCSET_DEFAULT_SIZES);

	pg_parse_json_or_ereport(lex, sem);

	MemoryContextDelete(state->tmp_cxt);

	PG_RETURN_NULL();
}


static JsonParseErrorType
each_object_field_start(void *state, char *fname, bool isnull)
{
	EachState  *_state = (EachState *) state;

	/* 保存指向值起始处的指针 */
	if (_state->lex->lex_level == 1)
	{
		/*
		 * next_scalar 会在 object_field_end 处理器中被重置，
		 * 由于已知值是标量，它在沿树向下递归时保持
		 * 开启并无危险。
		 */
		if (_state->normalize_results && _state->lex->token_type == JSON_TOKEN_STRING)
			_state->next_scalar = true;
		else
			_state->result_start = _state->lex->token_start;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
each_object_field_end(void *state, char *fname, bool isnull)
{
	EachState  *_state = (EachState *) state;
	MemoryContext old_cxt;
	int			len;
	text	   *val;
	HeapTuple	tuple;
	Datum		values[2];
	bool		nulls[2] = {false, false};

	/* 跳过嵌套对象 */
	if (_state->lex->lex_level != 1)
		return JSON_SUCCESS;

	/* 使用 tmp 上下文，以便每个元组完成后清理 */
	old_cxt = MemoryContextSwitchTo(_state->tmp_cxt);

	values[0] = CStringGetTextDatum(fname);

	if (isnull && _state->normalize_results)
	{
		nulls[1] = true;
		values[1] = (Datum) 0;
	}
	else if (_state->next_scalar)
	{
		values[1] = CStringGetTextDatum(_state->normalized_scalar);
		_state->next_scalar = false;
	}
	else
	{
		len = _state->lex->prev_token_terminator - _state->result_start;
		val = cstring_to_text_with_len(_state->result_start, len);
		values[1] = PointerGetDatum(val);
	}

	tuple = heap_form_tuple(_state->ret_tdesc, values, nulls);

	tuplestore_puttuple(_state->tuple_store, tuple);

	/* 清理并切回 */
	MemoryContextSwitchTo(old_cxt);
	MemoryContextReset(_state->tmp_cxt);

	return JSON_SUCCESS;
}

static JsonParseErrorType
each_array_start(void *state)
{
	EachState  *_state = (EachState *) state;

	/* json 结构检查 */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot deconstruct an array as an object")));

	return JSON_SUCCESS;
}

static JsonParseErrorType
each_scalar(void *state, char *token, JsonTokenType tokentype)
{
	EachState  *_state = (EachState *) state;

	/* json 结构检查 */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot deconstruct a scalar")));

	/* 需要时提供去转义的值 */
	if (_state->next_scalar)
		_state->normalized_scalar = token;

	return JSON_SUCCESS;
}

/*
 * SQL 函数 json_array_elements 与 json_array_elements_text
 *
 * 从 json 数组中获取元素
 *
 * 这里的许多处理与 json_each* 函数类似
 */

Datum
jsonb_array_elements(PG_FUNCTION_ARGS)
{
	return elements_worker_jsonb(fcinfo, "jsonb_array_elements", false);
}

Datum
jsonb_array_elements_text(PG_FUNCTION_ARGS)
{
	return elements_worker_jsonb(fcinfo, "jsonb_array_elements_text", true);
}

static Datum
elements_worker_jsonb(FunctionCallInfo fcinfo, const char *funcname,
					  bool as_text)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	ReturnSetInfo *rsi;
	MemoryContext old_cxt,
				tmp_cxt;
	bool		skipNested = false;
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken r;

	if (JB_ROOT_IS_SCALAR(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot extract elements from a scalar")));
	else if (!JB_ROOT_IS_ARRAY(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot extract elements from an object")));

	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC | MAT_SRF_BLESS);

	tmp_cxt = AllocSetContextCreate(CurrentMemoryContext,
									"jsonb_array_elements temporary cxt",
									ALLOCSET_DEFAULT_SIZES);

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, skipNested)) != WJB_DONE)
	{
		skipNested = true;

		if (r == WJB_ELEM)
		{
			Datum		values[1];
			bool		nulls[1] = {false};

			/* 使用 tmp 上下文，以便每个元组完成后清理 */
			old_cxt = MemoryContextSwitchTo(tmp_cxt);

			if (as_text)
			{
				if (v.type == jbvNull)
				{
					/* 文本模式下 json null 即 sql null */
					nulls[0] = true;
					values[0] = (Datum) NULL;
				}
				else
					values[0] = PointerGetDatum(JsonbValueAsText(&v));
			}
			else
			{
				/* 非文本模式，直接返回 Jsonb */
				Jsonb	   *val = JsonbValueToJsonb(&v);

				values[0] = PointerGetDatum(val);
			}

			tuplestore_putvalues(rsi->setResult, rsi->setDesc, values, nulls);

			/* 清理并切回 */
			MemoryContextSwitchTo(old_cxt);
			MemoryContextReset(tmp_cxt);
		}
	}

	MemoryContextDelete(tmp_cxt);

	PG_RETURN_NULL();
}

Datum
json_array_elements(PG_FUNCTION_ARGS)
{
	return elements_worker(fcinfo, "json_array_elements", false);
}

Datum
json_array_elements_text(PG_FUNCTION_ARGS)
{
	return elements_worker(fcinfo, "json_array_elements_text", true);
}

static Datum
elements_worker(FunctionCallInfo fcinfo, const char *funcname, bool as_text)
{
	text	   *json = PG_GETARG_TEXT_PP(0);

	/* 仅 as_text 时元素才需要转义字符串 */
	JsonLexContext *lex = makeJsonLexContext(json, as_text);
	JsonSemAction *sem;
	ReturnSetInfo *rsi;
	ElementsState *state;

	state = palloc0(sizeof(ElementsState));
	sem = palloc0(sizeof(JsonSemAction));

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC | MAT_SRF_BLESS);
	rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	state->tuple_store = rsi->setResult;
	state->ret_tdesc = rsi->setDesc;

	sem->semstate = (void *) state;
	sem->object_start = elements_object_start;
	sem->scalar = elements_scalar;
	sem->array_element_start = elements_array_element_start;
	sem->array_element_end = elements_array_element_end;

	state->function_name = funcname;
	state->normalize_results = as_text;
	state->next_scalar = false;
	state->lex = lex;
	state->tmp_cxt = AllocSetContextCreate(CurrentMemoryContext,
										   "json_array_elements temporary cxt",
										   ALLOCSET_DEFAULT_SIZES);

	pg_parse_json_or_ereport(lex, sem);

	MemoryContextDelete(state->tmp_cxt);

	PG_RETURN_NULL();
}

static JsonParseErrorType
elements_array_element_start(void *state, bool isnull)
{
	ElementsState *_state = (ElementsState *) state;

	/* 保存指向值起始处的指针 */
	if (_state->lex->lex_level == 1)
	{
		/*
		 * next_scalar 会在 array_element_end 处理器中被重置，
		 * 由于已知值是标量，它在沿树向下递归时保持
		 * 开启并无危险。
		 */
		if (_state->normalize_results && _state->lex->token_type == JSON_TOKEN_STRING)
			_state->next_scalar = true;
		else
			_state->result_start = _state->lex->token_start;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
elements_array_element_end(void *state, bool isnull)
{
	ElementsState *_state = (ElementsState *) state;
	MemoryContext old_cxt;
	int			len;
	text	   *val;
	HeapTuple	tuple;
	Datum		values[1];
	bool		nulls[1] = {false};

	/* 跳过嵌套对象 */
	if (_state->lex->lex_level != 1)
		return JSON_SUCCESS;

	/* 使用 tmp 上下文，以便每个元组完成后清理 */
	old_cxt = MemoryContextSwitchTo(_state->tmp_cxt);

	if (isnull && _state->normalize_results)
	{
		nulls[0] = true;
		values[0] = (Datum) NULL;
	}
	else if (_state->next_scalar)
	{
		values[0] = CStringGetTextDatum(_state->normalized_scalar);
		_state->next_scalar = false;
	}
	else
	{
		len = _state->lex->prev_token_terminator - _state->result_start;
		val = cstring_to_text_with_len(_state->result_start, len);
		values[0] = PointerGetDatum(val);
	}

	tuple = heap_form_tuple(_state->ret_tdesc, values, nulls);

	tuplestore_puttuple(_state->tuple_store, tuple);

	/* 清理并切回 */
	MemoryContextSwitchTo(old_cxt);
	MemoryContextReset(_state->tmp_cxt);

	return JSON_SUCCESS;
}

static JsonParseErrorType
elements_object_start(void *state)
{
	ElementsState *_state = (ElementsState *) state;

	/* json 结构检查 */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on a non-array",
						_state->function_name)));

	return JSON_SUCCESS;
}

static JsonParseErrorType
elements_scalar(void *state, char *token, JsonTokenType tokentype)
{
	ElementsState *_state = (ElementsState *) state;

	/* json 结构检查 */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on a scalar",
						_state->function_name)));

	/* 需要时提供去转义的值 */
	if (_state->next_scalar)
		_state->normalized_scalar = token;

	return JSON_SUCCESS;
}

/*
 * SQL 函数 json_populate_record
 *
 * 根据参数 json 设置记录中的字段
 *
 * 代码无耻地改编自 hstore 的 populate_record
 * 而后者又部分改编自 record_out。
 *
 * json 被分解成哈希表，随后按名查找每个
 * 记录字段。对 jsonb 则直接从对象取值。
 *
 */
Datum
jsonb_populate_record(PG_FUNCTION_ARGS)
{
	return populate_record_worker(fcinfo, "jsonb_populate_record",
								  false, true);
}

Datum
jsonb_to_record(PG_FUNCTION_ARGS)
{
	return populate_record_worker(fcinfo, "jsonb_to_record",
								  false, false);
}

Datum
json_populate_record(PG_FUNCTION_ARGS)
{
	return populate_record_worker(fcinfo, "json_populate_record",
								  true, true);
}

Datum
json_to_record(PG_FUNCTION_ARGS)
{
	return populate_record_worker(fcinfo, "json_to_record",
								  true, false);
}

/* 诊断用辅助函数 */
static void
populate_array_report_expected_array(PopulateArrayContext *ctx, int ndim)
{
	if (ndim <= 0)
	{
		if (ctx->colname)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("expected JSON array"),
					 errhint("See the value of key \"%s\".", ctx->colname)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("expected JSON array")));
	}
	else
	{
		StringInfoData indices;
		int			i;

		initStringInfo(&indices);

		Assert(ctx->ndims > 0 && ndim < ctx->ndims);

		for (i = 0; i < ndim; i++)
			appendStringInfo(&indices, "[%d]", ctx->sizes[i]);

		if (ctx->colname)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("expected JSON array"),
					 errhint("See the array element %s of key \"%s\".",
							 indices.data, ctx->colname)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("expected JSON array"),
					 errhint("See the array element %s.",
							 indices.data)));
	}
}

/* 在已知时设置已填充数组的维度数 */
static void
populate_array_assign_ndims(PopulateArrayContext *ctx, int ndims)
{
	int			i;

	Assert(ctx->ndims <= 0);

	if (ndims <= 0)
		populate_array_report_expected_array(ctx, ndims);

	ctx->ndims = ndims;
	ctx->dims = palloc(sizeof(int) * ndims);
	ctx->sizes = palloc0(sizeof(int) * ndims);

	for (i = 0; i < ndims; i++)
		ctx->dims[i] = -1;		/* 维度尚不可知 */
}

/* 检查已填充子数组维度 */
static void
populate_array_check_dimension(PopulateArrayContext *ctx, int ndim)
{
	int			dim = ctx->sizes[ndim]; /* 当前维度计数器 */

	if (ctx->dims[ndim] == -1)
		ctx->dims[ndim] = dim;	/* 若尚不知则赋维度 */
	else if (ctx->dims[ndim] != dim)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed JSON array"),
				 errdetail("Multidimensional arrays must have "
						   "sub-arrays with matching dimensions.")));

	/* 重置当前数组维度大小计数器 */
	ctx->sizes[ndim] = 0;

	/* 若是嵌套子数组则递增父维度计数器 */
	if (ndim > 0)
		ctx->sizes[ndim - 1]++;
}

static void
populate_array_element(PopulateArrayContext *ctx, int ndim, JsValue *jsv)
{
	Datum		element;
	bool		element_isnull;

	/* 填充数组元素 */
	element = populate_record_field(ctx->aio->element_info,
									ctx->aio->element_type,
									ctx->aio->element_typmod,
									NULL, ctx->mcxt, PointerGetDatum(NULL),
									jsv, &element_isnull);

	accumArrayResult(ctx->astate, element, element_isnull,
					 ctx->aio->element_type, ctx->acxt);

	Assert(ndim > 0);
	ctx->sizes[ndim - 1]++;		/* 递增当前维度计数器 */
}

/* populate_array_json() 的 json 对象开始回调 */
static JsonParseErrorType
populate_array_object_start(void *_state)
{
	PopulateArrayState *state = (PopulateArrayState *) _state;
	int			ndim = state->lex->lex_level;

	if (state->ctx->ndims <= 0)
		populate_array_assign_ndims(state->ctx, ndim);
	else if (ndim < state->ctx->ndims)
		populate_array_report_expected_array(state->ctx, ndim);

	return JSON_SUCCESS;
}

/* populate_array_json() 的json 数组结束回调 */
static JsonParseErrorType
populate_array_array_end(void *_state)
{
	PopulateArrayState *state = (PopulateArrayState *) _state;
	PopulateArrayContext *ctx = state->ctx;
	int			ndim = state->lex->lex_level;

	if (ctx->ndims <= 0)
		populate_array_assign_ndims(ctx, ndim + 1);

	if (ndim < ctx->ndims)
		populate_array_check_dimension(ctx, ndim);

	return JSON_SUCCESS;
}

/* populate_array_json() 的 json 数组元素开始回调 */
static JsonParseErrorType
populate_array_element_start(void *_state, bool isnull)
{
	PopulateArrayState *state = (PopulateArrayState *) _state;
	int			ndim = state->lex->lex_level;

	if (state->ctx->ndims <= 0 || ndim == state->ctx->ndims)
	{
		/* 记住当前数组元素起始处 */
		state->element_start = state->lex->token_start;
		state->element_type = state->lex->token_type;
		state->element_scalar = NULL;
	}

	return JSON_SUCCESS;
}

/* populate_array_json() 的 json 数组元素结束回调 */
static JsonParseErrorType
populate_array_element_end(void *_state, bool isnull)
{
	PopulateArrayState *state = (PopulateArrayState *) _state;
	PopulateArrayContext *ctx = state->ctx;
	int			ndim = state->lex->lex_level;

	Assert(ctx->ndims > 0);

	if (ndim == ctx->ndims)
	{
		JsValue		jsv;

		jsv.is_json = true;
		jsv.val.json.type = state->element_type;

		if (isnull)
		{
			Assert(jsv.val.json.type == JSON_TOKEN_NULL);
			jsv.val.json.str = NULL;
			jsv.val.json.len = 0;
		}
		else if (state->element_scalar)
		{
			jsv.val.json.str = state->element_scalar;
			jsv.val.json.len = -1;	/* null 结尾 */
		}
		else
		{
			jsv.val.json.str = state->element_start;
			jsv.val.json.len = (state->lex->prev_token_terminator -
								state->element_start) * sizeof(char);
		}

		populate_array_element(ctx, ndim, &jsv);
	}

	return JSON_SUCCESS;
}

/* populate_array_json() 的 json 标量回调 */
static JsonParseErrorType
populate_array_scalar(void *_state, char *token, JsonTokenType tokentype)
{
	PopulateArrayState *state = (PopulateArrayState *) _state;
	PopulateArrayContext *ctx = state->ctx;
	int			ndim = state->lex->lex_level;

	if (ctx->ndims <= 0)
		populate_array_assign_ndims(ctx, ndim);
	else if (ndim < ctx->ndims)
		populate_array_report_expected_array(ctx, ndim);

	if (ndim == ctx->ndims)
	{
		/* 记住标量元素记号 */
		state->element_scalar = token;
		/* element_type 必须已在 populate_array_element_start() 中设置 */
		Assert(state->element_type == tokentype);
	}

	return JSON_SUCCESS;
}

/* 解析 json 数组并填充数组 */
static void
populate_array_json(PopulateArrayContext *ctx, char *json, int len)
{
	PopulateArrayState state;
	JsonSemAction sem;

	state.lex = makeJsonLexContextCstringLen(json, len, GetDatabaseEncoding(), true);
	state.ctx = ctx;

	memset(&sem, 0, sizeof(sem));
	sem.semstate = (void *) &state;
	sem.object_start = populate_array_object_start;
	sem.array_end = populate_array_array_end;
	sem.array_element_start = populate_array_element_start;
	sem.array_element_end = populate_array_element_end;
	sem.scalar = populate_array_scalar;

	pg_parse_json_or_ereport(state.lex, &sem);

	/* 维度数应当已知 */
	Assert(ctx->ndims > 0 && ctx->dims);

	pfree(state.lex);
}

/*
 * populate_array_dim_jsonb() —— 递归遍历 jsonb 子数组
 *		元素，并用给定的 ArrayBuildState 累积结果。
 */
static void
populate_array_dim_jsonb(PopulateArrayContext *ctx, /*上下文*/
						 JsonbValue *jbv,	/* jsonb 子数组*/
						 int ndim)	/* 当前维度 */
{
	JsonbContainer *jbc = jbv->val.binary.data;
	JsonbIterator *it;
	JsonbIteratorToken tok;
	JsonbValue	val;
	JsValue		jsv;

	check_stack_depth();

	if (jbv->type != jbvBinary || !JsonContainerIsArray(jbc))
		populate_array_report_expected_array(ctx, ndim - 1);

	Assert(!JsonContainerIsScalar(jbc));

	it = JsonbIteratorInit(jbc);

	tok = JsonbIteratorNext(&it, &val, true);
	Assert(tok == WJB_BEGIN_ARRAY);

	tok = JsonbIteratorNext(&it, &val, true);

	/*
	 * 若维度数尚不可知且我们已找到数组结尾，或第一个
	 * 子元素不是数组，则此刻就赋予
	 * 维度数。
	 */
	if (ctx->ndims <= 0 &&
		(tok == WJB_END_ARRAY ||
		 (tok == WJB_ELEM &&
		  (val.type != jbvBinary ||
		   !JsonContainerIsArray(val.val.binary.data)))))
		populate_array_assign_ndims(ctx, ndim);

	jsv.is_json = false;
	jsv.val.jsonb = &val;

	/* 处理所有数组元素 */
	while (tok == WJB_ELEM)
	{
		/*
		 * 仅当维度数仍未知，或其并非最内层
		 * 维度时才递归。
		 */
		if (ctx->ndims > 0 && ndim >= ctx->ndims)
			populate_array_element(ctx, ndim, &jsv);
		else
		{
			/* 填充下级子数组 */
			populate_array_dim_jsonb(ctx, &val, ndim + 1);

			/* 维度数应当已知 */
			Assert(ctx->ndims > 0 && ctx->dims);

			populate_array_check_dimension(ctx, ndim);
		}

		tok = JsonbIteratorNext(&it, &val, true);
	}

	Assert(tok == WJB_END_ARRAY);

	/* 释放迭代器，迭代直至 WJB_DONE */
	tok = JsonbIteratorNext(&it, &val, true);
	Assert(tok == WJB_DONE && !it);
}

/* 从 json/jsonb 递归填充数组 */
static Datum
populate_array(ArrayIOData *aio,
			   const char *colname,
			   MemoryContext mcxt,
			   JsValue *jsv)
{
	PopulateArrayContext ctx;
	Datum		result;
	int		   *lbs;
	int			i;

	ctx.aio = aio;
	ctx.mcxt = mcxt;
	ctx.acxt = CurrentMemoryContext;
	ctx.astate = initArrayResult(aio->element_type, ctx.acxt, true);
	ctx.colname = colname;
	ctx.ndims = 0;				/* 尚不可知*/
	ctx.dims = NULL;
	ctx.sizes = NULL;

	if (jsv->is_json)
		populate_array_json(&ctx, jsv->val.json.str,
							jsv->val.json.len >= 0 ? jsv->val.json.len
							: strlen(jsv->val.json.str));
	else
	{
		populate_array_dim_jsonb(&ctx, jsv->val.jsonb, 1);
		ctx.dims[0] = ctx.sizes[0];
	}

	Assert(ctx.ndims > 0);

	lbs = palloc(sizeof(int) * ctx.ndims);

	for (i = 0; i < ctx.ndims; i++)
		lbs[i] = 1;

	result = makeMdArrayResult(ctx.astate, ctx.ndims, ctx.dims, lbs,
							   ctx.acxt, true);

	pfree(ctx.dims);
	pfree(ctx.sizes);
	pfree(lbs);

	return result;
}

static void
JsValueToJsObject(JsValue *jsv, JsObject *jso)
{
	jso->is_json = jsv->is_json;

	if (jsv->is_json)
	{
		/* 把纯文本 json 转成哈希表 */
		jso->val.json_hash =
			get_json_object_as_hash(jsv->val.json.str,
									jsv->val.json.len >= 0
									? jsv->val.json.len
									: strlen(jsv->val.json.str),
									"populate_composite");
	}
	else
	{
		JsonbValue *jbv = jsv->val.jsonb;

		if (jbv->type == jbvBinary &&
			JsonContainerIsObject(jbv->val.binary.data))
		{
			jso->val.jsonb_cont = jbv->val.binary.data;
		}
		else
		{
			bool		is_scalar;

			is_scalar = IsAJsonbScalar(jbv) ||
				(jbv->type == jbvBinary &&
				 JsonContainerIsScalar(jbv->val.binary.data));
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 is_scalar
					 ? errmsg("cannot call %s on a scalar",
							  "populate_composite")
					 : errmsg("cannot call %s on an array",
							  "populate_composite")));
		}
	}
}

/* 获取或更新复合类型的缓存元组描述符 */
static void
update_cached_tupdesc(CompositeIOData *io, MemoryContext mcxt)
{
	if (!io->tupdesc ||
		io->tupdesc->tdtypeid != io->base_typid ||
		io->tupdesc->tdtypmod != io->base_typmod)
	{
		TupleDesc	tupdesc = lookup_rowtype_tupdesc(io->base_typid,
													 io->base_typmod);
		MemoryContext oldcxt;

		if (io->tupdesc)
			FreeTupleDesc(io->tupdesc);

		/* 把无约束元组描述符复制进缓存内存上下文 */
		oldcxt = MemoryContextSwitchTo(mcxt);
		io->tupdesc = CreateTupleDescCopy(tupdesc);
		MemoryContextSwitchTo(oldcxt);

		ReleaseTupleDesc(tupdesc);
	}
}

/* 从 json/jsonb 递归填充复合（行类型）值 */
static Datum
populate_composite(CompositeIOData *io,
				   Oid typid,
				   const char *colname,
				   MemoryContext mcxt,
				   HeapTupleHeader defaultval,
				   JsValue *jsv,
				   bool isnull)
{
	Datum		result;

	/* 获取/更新缓存的元组描述符 */
	update_cached_tupdesc(io, mcxt);

	if (isnull)
		result = (Datum) 0;
	else
	{
		HeapTupleHeader tuple;
		JsObject	jso;

		/* 准备输入值 */
		JsValueToJsObject(jsv, &jso);

		/* 填充结果记录元组 */
		tuple = populate_record(io->tupdesc, &io->record_io,
								defaultval, mcxt, &jso);
		result = HeapTupleHeaderGetDatum(tuple);

		JsObjectFree(&jso);
	}

	/*
	 * 若是复合上的域，则检查域约束。（或许应重构以能
	 * 看到 TYPECAT 值，但现在可以通过比较 typid 与
	 * base_typid 来判别。）
	 */
	if (typid != io->base_typid && typid != RECORDOID)
		domain_check(result, isnull, typid, &io->domain_info, mcxt);

	return result;
}

/* 从 json/jsonb 值填充非空标量值 */
static Datum
populate_scalar(ScalarIOData *io, Oid typid, int32 typmod, JsValue *jsv)
{
	Datum		res;
	char	   *str = NULL;
	char	   *json = NULL;

	if (jsv->is_json)
	{
		int			len = jsv->val.json.len;

		json = jsv->val.json.str;
		Assert(json);
		if (len >= 0)
		{
			/* 需要复制非 null 结尾的字符串 */
			str = palloc(len + 1 * sizeof(char));
			memcpy(str, json, len);
			str[len] = '\0';
		}
		else
			str = json;			/* 字符串已以 null 结尾 */

		/* 若转成 json/jsonb，把字符串变成合法 JSON 字面量 */
		if ((typid == JSONOID || typid == JSONBOID) &&
			jsv->val.json.type == JSON_TOKEN_STRING)
		{
			StringInfoData buf;

			initStringInfo(&buf);
			escape_json(&buf, str);
			/* 释放临时缓冲区 */
			if (str != json)
				pfree(str);
			str = buf.data;
		}
	}
	else
	{
		JsonbValue *jbv = jsv->val.jsonb;

		if (typid == JSONBOID)
		{
			Jsonb	   *jsonb = JsonbValueToJsonb(jbv); /* 直接使用 jsonb */

			return JsonbPGetDatum(jsonb);
		}
		/* 把 jsonb 转成字符串供 typio 用 */
		else if (typid == JSONOID && jbv->type != jbvBinary)
		{
			/*
				 * 把标量 jsonb 转成 json 串（非标量以 jbvBinary
				 * 传入），保留顶层字符串两侧的引号。
			 */
			Jsonb	   *jsonb = JsonbValueToJsonb(jbv);

			str = JsonbToCString(NULL, &jsonb->root, VARSIZE(jsonb));
		}
		else if (jbv->type == jbvString)	/* 引号已被剥离 */
			str = pnstrdup(jbv->val.string.val, jbv->val.string.len);
		else if (jbv->type == jbvBool)
			str = pstrdup(jbv->val.boolean ? "true" : "false");
		else if (jbv->type == jbvNumeric)
			str = DatumGetCString(DirectFunctionCall1(numeric_out,
													  PointerGetDatum(jbv->val.numeric)));
		else if (jbv->type == jbvBinary)
			str = JsonbToCString(NULL, jbv->val.binary.data,
								 jbv->val.binary.len);
		else
			elog(ERROR, "unrecognized jsonb type: %d", (int) jbv->type);
	}

	res = InputFunctionCall(&io->typiofunc, str, io->typioparam, typmod);

	/* 释放临时缓冲区 */
	if (str != json)
		pfree(str);

	return res;
}

static Datum
populate_domain(DomainIOData *io,
				Oid typid,
				const char *colname,
				MemoryContext mcxt,
				JsValue *jsv,
				bool isnull)
{
	Datum		res;

	if (isnull)
		res = (Datum) 0;
	else
	{
		res = populate_record_field(io->base_io,
									io->base_typid, io->base_typmod,
									colname, mcxt, PointerGetDatum(NULL),
									jsv, &isnull);
		Assert(!isnull);
	}

	domain_check(res, isnull, typid, &io->domain_info, mcxt);

	return res;
}

/* 为给定类型准备列元数据缓存 */
static void
prepare_column_cache(ColumnIOData *column,
					 Oid typid,
					 int32 typmod,
					 MemoryContext mcxt,
					 bool need_scalar)
{
	HeapTuple	tup;
	Form_pg_type type;

	column->typid = typid;
	column->typmod = typmod;

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typid);

	type = (Form_pg_type) GETSTRUCT(tup);

	if (type->typtype == TYPTYPE_DOMAIN)
	{
		/*
		 * 我们可以直接移到最底层基类型；domain_check() 会
		 * 负责检查一叠域的所有约束。
		 */
		Oid			base_typid;
		int32		base_typmod = typmod;

		base_typid = getBaseTypeAndTypmod(typid, &base_typmod);
		if (get_typtype(base_typid) == TYPTYPE_COMPOSITE)
		{
			/* 复合上的域有专门的代码路径 */
			column->typcat = TYPECAT_COMPOSITE_DOMAIN;
			column->io.composite.record_io = NULL;
			column->io.composite.tupdesc = NULL;
			column->io.composite.base_typid = base_typid;
			column->io.composite.base_typmod = base_typmod;
			column->io.composite.domain_info = NULL;
		}
		else
		{
			/* 其他类型上的域 */
			column->typcat = TYPECAT_DOMAIN;
			column->io.domain.base_typid = base_typid;
			column->io.domain.base_typmod = base_typmod;
			column->io.domain.base_io =
				MemoryContextAllocZero(mcxt, sizeof(ColumnIOData));
			column->io.domain.domain_info = NULL;
		}
	}
	else if (type->typtype == TYPTYPE_COMPOSITE || typid == RECORDOID)
	{
		column->typcat = TYPECAT_COMPOSITE;
		column->io.composite.record_io = NULL;
		column->io.composite.tupdesc = NULL;
		column->io.composite.base_typid = typid;
		column->io.composite.base_typmod = typmod;
		column->io.composite.domain_info = NULL;
	}
	else if (IsTrueArrayType(type))
	{
		column->typcat = TYPECAT_ARRAY;
		column->io.array.element_info = MemoryContextAllocZero(mcxt,
															   sizeof(ColumnIOData));
		column->io.array.element_type = type->typelem;
		/* 数组元素 typemod 存于属性的 typmod */
		column->io.array.element_typmod = typmod;
	}
	else
	{
		column->typcat = TYPECAT_SCALAR;
		need_scalar = true;
	}

	/* 调用者可强制我们对非标量也查找 scalar_io 信息 */
	if (need_scalar)
	{
		Oid			typioproc;

		getTypeInputInfo(typid, &typioproc, &column->scalar_io.typioparam);
		fmgr_info_cxt(typioproc, &column->scalar_io.typiofunc, mcxt);
	}

	ReleaseSysCache(tup);
}

/* 从 json/jsonb 值递归填充记录字段或数组元素 */
static Datum
populate_record_field(ColumnIOData *col,
					  Oid typid,
					  int32 typmod,
					  const char *colname,
					  MemoryContext mcxt,
					  Datum defaultval,
					  JsValue *jsv,
					  bool *isnull)
{
	TypeCat		typcat;

	check_stack_depth();

	/*
	 * 为给定类型准备列元数据缓存。强制查找 scalar_io
	 * 数据，以便下面的 json 字符串技巧能生效。
	 */
	if (col->typid != typid || col->typmod != typmod)
		prepare_column_cache(col, typid, typmod, mcxt, true);

	*isnull = JsValueIsNull(jsv);

	typcat = col->typcat;

	/* 尝试经由输入函数把 json 字符串转成非标量类型 */
	if (JsValueIsString(jsv) &&
		(typcat == TYPECAT_ARRAY ||
		 typcat == TYPECAT_COMPOSITE ||
		 typcat == TYPECAT_COMPOSITE_DOMAIN))
		typcat = TYPECAT_SCALAR;

	/* 对 NULL 也必须执行域检查，否则立即退出 */
	if (*isnull &&
		typcat != TYPECAT_DOMAIN &&
		typcat != TYPECAT_COMPOSITE_DOMAIN)
		return (Datum) 0;

	switch (typcat)
	{
		case TYPECAT_SCALAR:
			return populate_scalar(&col->scalar_io, typid, typmod, jsv);

		case TYPECAT_ARRAY:
			return populate_array(&col->io.array, colname, mcxt, jsv);

		case TYPECAT_COMPOSITE:
		case TYPECAT_COMPOSITE_DOMAIN:
			return populate_composite(&col->io.composite, typid,
									  colname, mcxt,
									  DatumGetPointer(defaultval)
									  ? DatumGetHeapTupleHeader(defaultval)
									  : NULL,
									  jsv, *isnull);

		case TYPECAT_DOMAIN:
			return populate_domain(&col->io.domain, typid, colname, mcxt,
								   jsv, *isnull);

		default:
			elog(ERROR, "unrecognized type category '%c'", typcat);
			return (Datum) 0;
	}
}

static RecordIOData *
allocate_record_info(MemoryContext mcxt, int ncolumns)
{
	RecordIOData *data = (RecordIOData *)
		MemoryContextAlloc(mcxt,
						   offsetof(RecordIOData, columns) +
						   ncolumns * sizeof(ColumnIOData));

	data->record_type = InvalidOid;
	data->record_typmod = 0;
	data->ncolumns = ncolumns;
	MemSet(data->columns, 0, sizeof(ColumnIOData) * ncolumns);

	return data;
}

static bool
JsObjectGetField(JsObject *obj, char *field, JsValue *jsv)
{
	jsv->is_json = obj->is_json;

	if (jsv->is_json)
	{
		JsonHashEntry *hashentry = hash_search(obj->val.json_hash, field,
											   HASH_FIND, NULL);

		jsv->val.json.type = hashentry ? hashentry->type : JSON_TOKEN_NULL;
		jsv->val.json.str = jsv->val.json.type == JSON_TOKEN_NULL ? NULL :
			hashentry->val;
		jsv->val.json.len = jsv->val.json.str ? -1 : 0; /* null 结尾 */

		return hashentry != NULL;
	}
	else
	{
		jsv->val.jsonb = !obj->val.jsonb_cont ? NULL :
			getKeyJsonValueFromContainer(obj->val.jsonb_cont, field, strlen(field),
										 NULL);

		return jsv->val.jsonb != NULL;
	}
}

/* 从 json/jsonb 值填充记录元组 */
static HeapTupleHeader
populate_record(TupleDesc tupdesc,
				RecordIOData **record_p,
				HeapTupleHeader defaultval,
				MemoryContext mcxt,
				JsObject *obj)
{
	RecordIOData *record = *record_p;
	Datum	   *values;
	bool	   *nulls;
	HeapTuple	res;
	int			ncolumns = tupdesc->natts;
	int			i;

	/*
	 * 若输入 json 为空，只有在我们被传入的是非空记录时
	 * 才能跳过其余步骤，否则可能有域 null 相关的问题。
	 *
	 */
	if (defaultval && JsObjectIsEmpty(obj))
		return defaultval;

	/* 再分配元数据缓存 */
	if (record == NULL ||
		record->ncolumns != ncolumns)
		*record_p = record = allocate_record_info(mcxt, ncolumns);

	/* 若记录类型已变则令元数据缓存失效 */
	if (record->record_type != tupdesc->tdtypeid ||
		record->record_typmod != tupdesc->tdtypmod)
	{
		MemSet(record, 0, offsetof(RecordIOData, columns) +
			   ncolumns * sizeof(ColumnIOData));
		record->record_type = tupdesc->tdtypeid;
		record->record_typmod = tupdesc->tdtypmod;
		record->ncolumns = ncolumns;
	}

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	if (defaultval)
	{
		HeapTupleData tuple;

		/* 构建临时 HeapTuple 控制结构 */
		tuple.t_len = HeapTupleHeaderGetDatumLength(defaultval);
		ItemPointerSetInvalid(&(tuple.t_self));
		tuple.t_tableOid = InvalidOid;
		tuple.t_data = defaultval;

		/* 将元组分解为字段 */
		heap_deform_tuple(&tuple, tupdesc, values, nulls);
	}
	else
	{
		for (i = 0; i < ncolumns; ++i)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
		}
	}

	for (i = 0; i < ncolumns; ++i)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		char	   *colname = NameStr(att->attname);
		JsValue		field = {0};
		bool		found;

		/* 忽略类型中已丢弃的列 */
		if (att->attisdropped)
		{
			nulls[i] = true;
			continue;
		}

		found = JsObjectGetField(obj, colname, &field);

		/*
			 * 若键未找到，此处不能直接跳过，因为可能还有
			 * 域要处理。若传入非空记录 datum，则假定
			 * 现有值有效（错也不怪我们），但若传入
			 * 的是 null，则未填充的字段都需过一遍
			 * 输入函数，以防它是域类型。
			 *
		 */
		if (defaultval && !found)
			continue;

		values[i] = populate_record_field(&record->columns[i],
										  att->atttypid,
										  att->atttypmod,
										  colname,
										  mcxt,
										  nulls[i] ? (Datum) 0 : values[i],
										  &field,
										  &nulls[i]);
	}

	res = heap_form_tuple(tupdesc, values, nulls);

	pfree(values);
	pfree(nulls);

	return res->t_data;
}

/*
 * json{b}_populate_record{set} 的设置：结果类型与第一个
 * 参数的类型相同——除非第一个参数是 "null::record"，
 * 它无法提取类型信息；稍后处理。
 */
static void
get_record_type_from_argument(FunctionCallInfo fcinfo,
							  const char *funcname,
							  PopulateRecordCache *cache)
{
	cache->argtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	prepare_column_cache(&cache->c,
						 cache->argtype, -1,
						 cache->fn_mcxt, false);
	if (cache->c.typcat != TYPECAT_COMPOSITE &&
		cache->c.typcat != TYPECAT_COMPOSITE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
		/* translator: %s is a function name, eg json_to_record */
				 errmsg("first argument of %s must be a row type",
						funcname)));
}

/*
 * json{b}_to_record{set} 的设置：结果类型由调用查询
 * 指定。若发现首参数是 RECORD 类型的 null，这段
 * 代码也用于 json{b}_populate_record{set}。
 *
 * 此处语法上无法把目标类型指定为
 * 复合上的域。
 */
static void
get_record_type_from_query(FunctionCallInfo fcinfo,
						   const char *funcname,
						   PopulateRecordCache *cache)
{
	TupleDesc	tupdesc;
	MemoryContext old_cxt;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/* translator: %s is a function name, eg json_to_record */
				 errmsg("could not determine row type for result of %s",
						funcname),
				 errhint("Provide a non-null record argument, "
						 "or call the function in the FROM clause "
						 "using a column definition list.")));

	Assert(tupdesc);
	cache->argtype = tupdesc->tdtypeid;

	/* 若不止一次经过此处，避免内存泄漏 */
	if (cache->c.io.composite.tupdesc)
		FreeTupleDesc(cache->c.io.composite.tupdesc);

	/* 保存已判明 tupdesc */
	old_cxt = MemoryContextSwitchTo(cache->fn_mcxt);
	cache->c.io.composite.tupdesc = CreateTupleDescCopy(tupdesc);
	cache->c.io.composite.base_typid = tupdesc->tdtypeid;
	cache->c.io.composite.base_typmod = tupdesc->tdtypmod;
	MemoryContextSwitchTo(old_cxt);
}

/*
 * json{b}_populate_record() 与 json{b}_to_record() 的工作函数
 * is_json 与 have_record_arg 标识具体函数
 */
static Datum
populate_record_worker(FunctionCallInfo fcinfo, const char *funcname,
					   bool is_json, bool have_record_arg)
{
	int			json_arg_num = have_record_arg ? 1 : 0;
	JsValue		jsv = {0};
	HeapTupleHeader rec;
	Datum		rettuple;
	JsonbValue	jbv;
	MemoryContext fnmcxt = fcinfo->flinfo->fn_mcxt;
	PopulateRecordCache *cache = fcinfo->flinfo->fn_extra;

	/*
	 * 若是首次执行，判明输入/结果记录类型。注意此段
	 * 只查看 fcinfo 上下文，而它在查询期间不会变；因此
	 * 我们可能尚不能完全解析 RECORD 输入类型。
	 */
	if (!cache)
	{
		fcinfo->flinfo->fn_extra = cache =
			MemoryContextAllocZero(fnmcxt, sizeof(*cache));
		cache->fn_mcxt = fnmcxt;

		if (have_record_arg)
			get_record_type_from_argument(fcinfo, funcname, cache);
		else
			get_record_type_from_query(fcinfo, funcname, cache);
	}

	/* 若有 record 参数则收集 */
	if (!have_record_arg)
		rec = NULL;				/* 即 json{b}_to_record() */
	else if (!PG_ARGISNULL(0))
	{
		rec = PG_GETARG_HEAPTUPLEHEADER(0);

		/*
			 * 当声明参数类型为 RECORD 时，判明实际记录类型
			 * 取自元组。
		 */
		if (cache->argtype == RECORDOID)
		{
			cache->c.io.composite.base_typid = HeapTupleHeaderGetTypeId(rec);
			cache->c.io.composite.base_typmod = HeapTupleHeaderGetTypMod(rec);
		}
	}
	else
	{
		rec = NULL;

		/*
			 * 当声明参数类型为 RECORD 时，判明实际记录类型
			 * 取自调用查询或失败。
		 */
		if (cache->argtype == RECORDOID)
		{
			get_record_type_from_query(fcinfo, funcname, cache);
			/* 这不会改变 argtype，这对下次很重要 */
			Assert(cache->argtype == RECORDOID);
		}
	}

	/* 若无 JSON 参数，则原样返回记录（若有） */
	if (PG_ARGISNULL(json_arg_num))
	{
		if (rec)
			PG_RETURN_POINTER(rec);
		else
			PG_RETURN_NULL();
	}

	jsv.is_json = is_json;

	if (is_json)
	{
		text	   *json = PG_GETARG_TEXT_PP(json_arg_num);

		jsv.val.json.str = VARDATA_ANY(json);
		jsv.val.json.len = VARSIZE_ANY_EXHDR(json);
		jsv.val.json.type = JSON_TOKEN_INVALID; /* 不用于
												 * populate_composite() */
	}
	else
	{
		Jsonb	   *jb = PG_GETARG_JSONB_P(json_arg_num);

		jsv.val.jsonb = &jbv;

		/* 填充指向 jb 的二进制 jsonb 值 */
		jbv.type = jbvBinary;
		jbv.val.binary.data = &jb->root;
		jbv.val.binary.len = VARSIZE(jb) - VARHDRSZ;
	}

	rettuple = populate_composite(&cache->c.io.composite, cache->argtype,
								  NULL, fnmcxt, rec, &jsv, false);

	PG_RETURN_DATUM(rettuple);
}

/*
 * get_json_object_as_hash
 *
 * 把 json 对象分解为哈希表。
 */
static HTAB *
get_json_object_as_hash(char *json, int len, const char *funcname)
{
	HASHCTL		ctl;
	HTAB	   *tab;
	JHashState *state;
	JsonLexContext *lex = makeJsonLexContextCstringLen(json, len, GetDatabaseEncoding(), true);
	JsonSemAction *sem;

	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = sizeof(JsonHashEntry);
	ctl.hcxt = CurrentMemoryContext;
	tab = hash_create("json object hashtable",
					  100,
					  &ctl,
					  HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

	state = palloc0(sizeof(JHashState));
	sem = palloc0(sizeof(JsonSemAction));

	state->function_name = funcname;
	state->hash = tab;
	state->lex = lex;

	sem->semstate = (void *) state;
	sem->array_start = hash_array_start;
	sem->scalar = hash_scalar;
	sem->object_field_start = hash_object_field_start;
	sem->object_field_end = hash_object_field_end;

	pg_parse_json_or_ereport(lex, sem);

	return tab;
}

static JsonParseErrorType
hash_object_field_start(void *state, char *fname, bool isnull)
{
	JHashState *_state = (JHashState *) state;

	if (_state->lex->lex_level > 1)
		return JSON_SUCCESS;

	/* 记住记号类型 */
	_state->saved_token_type = _state->lex->token_type;

	if (_state->lex->token_type == JSON_TOKEN_ARRAY_START ||
		_state->lex->token_type == JSON_TOKEN_OBJECT_START)
	{
		/* 记住子对象整个文本的起始位置 */
		_state->save_json_start = _state->lex->token_start;
	}
	else
	{
		/* 必须是标量 */
		_state->save_json_start = NULL;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
hash_object_field_end(void *state, char *fname, bool isnull)
{
	JHashState *_state = (JHashState *) state;
	JsonHashEntry *hashentry;
	bool		found;

	/*
	 * 忽略嵌套字段。
	 */
	if (_state->lex->lex_level > 1)
		return JSON_SUCCESS;

	/*
	 * 忽略 >= NAMEDATALEN 的字段名——它们匹配不了
	 * 记录字段。（注：若无此测试，哈希代码会在
	 * NAMEDATALEN-1 处截断字符串，进而可能匹配到同样被
	 * 截断的记录字段名。那本是合理行为，但此代码素来
	 * 坚持精确相等，所以我们保留这一行为。）
	 */
	if (strlen(fname) >= NAMEDATALEN)
		return JSON_SUCCESS;

	hashentry = hash_search(_state->hash, fname, HASH_ENTER, &found);

	/*
	 * found 为真表示重复。对此我们不做处理——同名
	 * 的后一个字段会覆盖先前的字段。
	 */

	hashentry->type = _state->saved_token_type;
	Assert(isnull == (hashentry->type == JSON_TOKEN_NULL));

	if (_state->save_json_start != NULL)
	{
		int			len = _state->lex->prev_token_terminator - _state->save_json_start;
		char	   *val = palloc((len + 1) * sizeof(char));

		memcpy(val, _state->save_json_start, len);
		val[len] = '\0';
		hashentry->val = val;
	}
	else
	{
		/* 本应是标量才对 */
		hashentry->val = _state->saved_scalar;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
hash_array_start(void *state)
{
	JHashState *_state = (JHashState *) state;

	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on an array", _state->function_name)));

	return JSON_SUCCESS;
}

static JsonParseErrorType
hash_scalar(void *state, char *token, JsonTokenType tokentype)
{
	JHashState *_state = (JHashState *) state;

	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on a scalar", _state->function_name)));

	if (_state->lex->lex_level == 1)
	{
		_state->saved_scalar = token;
		/* saved_token_type 必须已在 hash_object_field_start() 中设置 */
		Assert(_state->saved_token_type == tokentype);
	}

	return JSON_SUCCESS;
}


/*
 * SQL 函数 json_populate_recordset
 *
 * 根据参数 json 设置一组记录中的字段，
 * 参数必须为对象数组。
 *
 * 与 json_populate_record 类似，但元组构建代码
 * 下推到语义动作处理器，按数组中每个
 * 对象执行。
 */
Datum
jsonb_populate_recordset(PG_FUNCTION_ARGS)
{
	return populate_recordset_worker(fcinfo, "jsonb_populate_recordset",
									 false, true);
}

Datum
jsonb_to_recordset(PG_FUNCTION_ARGS)
{
	return populate_recordset_worker(fcinfo, "jsonb_to_recordset",
									 false, false);
}

Datum
json_populate_recordset(PG_FUNCTION_ARGS)
{
	return populate_recordset_worker(fcinfo, "json_populate_recordset",
									 true, true);
}

Datum
json_to_recordset(PG_FUNCTION_ARGS)
{
	return populate_recordset_worker(fcinfo, "json_to_recordset",
									 true, false);
}

static void
populate_recordset_record(PopulateRecordsetState *state, JsObject *obj)
{
	PopulateRecordCache *cache = state->cache;
	HeapTupleHeader tuphead;
	HeapTupleData tuple;

	/* 获取/更新缓存的元组描述符 */
	update_cached_tupdesc(&cache->c.io.composite, cache->fn_mcxt);

	/* 以 json 替换记录字段 */
	tuphead = populate_record(cache->c.io.composite.tupdesc,
							  &cache->c.io.composite.record_io,
							  state->rec,
							  cache->fn_mcxt,
							  obj);

	/* 若是复合上的域，检查域约束 */
	if (cache->c.typcat == TYPECAT_COMPOSITE_DOMAIN)
		domain_check(HeapTupleHeaderGetDatum(tuphead), false,
					 cache->argtype,
					 &cache->c.io.composite.domain_info,
					 cache->fn_mcxt);

	/* 好，存入 tuplestore */
	tuple.t_len = HeapTupleHeaderGetDatumLength(tuphead);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = tuphead;

	tuplestore_puttuple(state->tuple_store, &tuple);
}

/*
 * json{b}_populate_recordset() 与 json{b}_to_recordset() 的工作函数
 * is_json 与 have_record_arg 标识具体函数
 */
static Datum
populate_recordset_worker(FunctionCallInfo fcinfo, const char *funcname,
						  bool is_json, bool have_record_arg)
{
	int			json_arg_num = have_record_arg ? 1 : 0;
	ReturnSetInfo *rsi;
	MemoryContext old_cxt;
	HeapTupleHeader rec;
	PopulateRecordCache *cache = fcinfo->flinfo->fn_extra;
	PopulateRecordsetState *state;

	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	if (!rsi || !IsA(rsi, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	if (!(rsi->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	rsi->returnMode = SFRM_Materialize;

	/*
	 * 若是首次执行，判明输入/结果记录类型。注意此段
	 * 只查看 fcinfo 上下文，而它在查询期间不会变；因此
	 * 我们可能尚不能完全解析 RECORD 输入类型。
	 */
	if (!cache)
	{
		fcinfo->flinfo->fn_extra = cache =
			MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, sizeof(*cache));
		cache->fn_mcxt = fcinfo->flinfo->fn_mcxt;

		if (have_record_arg)
			get_record_type_from_argument(fcinfo, funcname, cache);
		else
			get_record_type_from_query(fcinfo, funcname, cache);
	}

	/* 若有 record 参数则收集 */
	if (!have_record_arg)
		rec = NULL;				/* 即 json{b}_to_recordset() */
	else if (!PG_ARGISNULL(0))
	{
		rec = PG_GETARG_HEAPTUPLEHEADER(0);

		/*
			 * 当声明参数类型为 RECORD 时，判明实际记录类型
			 * 取自元组。
		 */
		if (cache->argtype == RECORDOID)
		{
			cache->c.io.composite.base_typid = HeapTupleHeaderGetTypeId(rec);
			cache->c.io.composite.base_typmod = HeapTupleHeaderGetTypMod(rec);
		}
	}
	else
	{
		rec = NULL;

		/*
			 * 当声明参数类型为 RECORD 时，判明实际记录类型
			 * 取自调用查询或失败。
		 */
		if (cache->argtype == RECORDOID)
		{
			get_record_type_from_query(fcinfo, funcname, cache);
			/* 这不会改变 argtype，这对下次很重要 */
			Assert(cache->argtype == RECORDOID);
		}
	}

	/* 若 json 为 null 则返回空集 */
	if (PG_ARGISNULL(json_arg_num))
		PG_RETURN_NULL();

	/*
	 * 强制更新缓存 tupdesc，确保即使 JSON 不含行我们也有
	 * 正确的 tupdesc 可返回。
	 */
	update_cached_tupdesc(&cache->c.io.composite, cache->fn_mcxt);

	state = palloc0(sizeof(PopulateRecordsetState));

	/* 在足够长寿命的内存上下文中创建 tuplestore */
	old_cxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
	state->tuple_store = tuplestore_begin_heap(rsi->allowedModes &
											   SFRM_Materialize_Random,
											   false, work_mem);
	MemoryContextSwitchTo(old_cxt);

	state->function_name = funcname;
	state->cache = cache;
	state->rec = rec;

	if (is_json)
	{
		text	   *json = PG_GETARG_TEXT_PP(json_arg_num);
		JsonLexContext *lex;
		JsonSemAction *sem;

		sem = palloc0(sizeof(JsonSemAction));

		lex = makeJsonLexContext(json, true);

		sem->semstate = (void *) state;
		sem->array_start = populate_recordset_array_start;
		sem->array_element_start = populate_recordset_array_element_start;
		sem->scalar = populate_recordset_scalar;
		sem->object_field_start = populate_recordset_object_field_start;
		sem->object_field_end = populate_recordset_object_field_end;
		sem->object_start = populate_recordset_object_start;
		sem->object_end = populate_recordset_object_end;

		state->lex = lex;

		pg_parse_json_or_ereport(lex, sem);
	}
	else
	{
		Jsonb	   *jb = PG_GETARG_JSONB_P(json_arg_num);
		JsonbIterator *it;
		JsonbValue	v;
		bool		skipNested = false;
		JsonbIteratorToken r;

		if (JB_ROOT_IS_SCALAR(jb) || !JB_ROOT_IS_ARRAY(jb))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot call %s on a non-array",
							funcname)));

		it = JsonbIteratorInit(&jb->root);

		while ((r = JsonbIteratorNext(&it, &v, skipNested)) != WJB_DONE)
		{
			skipNested = true;

			if (r == WJB_ELEM)
			{
				JsObject	obj;

				if (v.type != jbvBinary ||
					!JsonContainerIsObject(v.val.binary.data))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("argument of %s must be an array of objects",
									funcname)));

				obj.is_json = false;
				obj.val.jsonb_cont = v.val.binary.data;

				populate_recordset_record(state, &obj);
			}
		}
	}

	/*
	 * 注意：必须复制缓存 tupdesc，因为执行器会释放传回
	 * 的 setDesc，但我们要保留缓存以备同一查询内
	 * 被再次调用。
	 */
	rsi->setResult = state->tuple_store;
	rsi->setDesc = CreateTupleDescCopy(cache->c.io.composite.tupdesc);

	PG_RETURN_NULL();
}

static JsonParseErrorType
populate_recordset_object_start(void *state)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;
	int			lex_level = _state->lex->lex_level;
	HASHCTL		ctl;

	/* 拒绝顶层对象：0 层必须是数组 */
	if (lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on an object",
						_state->function_name)));

	/* 嵌套对象无需特殊处理 */
	if (lex_level > 1)
		return JSON_SUCCESS;

	/* 1 层对象：为该对象建立新哈希表 */
	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = sizeof(JsonHashEntry);
	ctl.hcxt = CurrentMemoryContext;
	_state->json_hash = hash_create("json object hashtable",
									100,
									&ctl,
									HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

	return JSON_SUCCESS;
}

static JsonParseErrorType
populate_recordset_object_end(void *state)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;
	JsObject	obj;

	/* 嵌套对象无需特殊处理 */
	if (_state->lex->lex_level > 1)
		return JSON_SUCCESS;

	obj.is_json = true;
	obj.val.json_hash = _state->json_hash;

	/* 否则，基于该 1 层对象构造并返回一个元组 */
	populate_recordset_record(_state, &obj);

	/* 本对象哈希完毕 */
	hash_destroy(_state->json_hash);
	_state->json_hash = NULL;

	return JSON_SUCCESS;
}

static JsonParseErrorType
populate_recordset_array_element_start(void *state, bool isnull)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;

	if (_state->lex->lex_level == 1 &&
		_state->lex->token_type != JSON_TOKEN_OBJECT_START)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument of %s must be an array of objects",
						_state->function_name)));

	return JSON_SUCCESS;
}

static JsonParseErrorType
populate_recordset_array_start(void *state)
{
	/* 无事可做 */
	return JSON_SUCCESS;
}

static JsonParseErrorType
populate_recordset_scalar(void *state, char *token, JsonTokenType tokentype)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;

	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on a scalar",
						_state->function_name)));

	if (_state->lex->lex_level == 2)
		_state->saved_scalar = token;

	return JSON_SUCCESS;
}

static JsonParseErrorType
populate_recordset_object_field_start(void *state, char *fname, bool isnull)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;

	if (_state->lex->lex_level > 2)
		return JSON_SUCCESS;

	_state->saved_token_type = _state->lex->token_type;

	if (_state->lex->token_type == JSON_TOKEN_ARRAY_START ||
		_state->lex->token_type == JSON_TOKEN_OBJECT_START)
	{
		_state->save_json_start = _state->lex->token_start;
	}
	else
	{
		_state->save_json_start = NULL;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
populate_recordset_object_field_end(void *state, char *fname, bool isnull)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;
	JsonHashEntry *hashentry;
	bool		found;

	/*
	 * 忽略嵌套字段。
	 */
	if (_state->lex->lex_level > 2)
		return JSON_SUCCESS;

	/*
	 * 忽略 >= NAMEDATALEN 的字段名——它们匹配不了
	 * 记录字段。（注：若无此测试，哈希代码会在
	 * NAMEDATALEN-1 处截断字符串，进而可能匹配到同样被
	 * 截断的记录字段名。那本是合理行为，但此代码素来
	 * 坚持精确相等，所以我们保留这一行为。）
	 */
	if (strlen(fname) >= NAMEDATALEN)
		return JSON_SUCCESS;

	hashentry = hash_search(_state->json_hash, fname, HASH_ENTER, &found);

	/*
	 * found 为真表示重复。对此我们不做处理——同名
	 * 的后一个字段会覆盖先前的字段。
	 */

	hashentry->type = _state->saved_token_type;
	Assert(isnull == (hashentry->type == JSON_TOKEN_NULL));

	if (_state->save_json_start != NULL)
	{
		int			len = _state->lex->prev_token_terminator - _state->save_json_start;
		char	   *val = palloc((len + 1) * sizeof(char));

		memcpy(val, _state->save_json_start, len);
		val[len] = '\0';
		hashentry->val = val;
	}
	else
	{
		/* 本应是标量才对 */
		hashentry->val = _state->saved_scalar;
	}

	return JSON_SUCCESS;
}

/*
 * json_strip_nulls 的语义动作。
 *
 * 简单地将输入重复到输出，除非遇到 null 的
 * 对象字段。相关状态在字段开始时设置，
 * 在标量动作（必须是下一个）被调用时
 * 重置。
 */

static JsonParseErrorType
sn_object_start(void *state)
{
	StripnullState *_state = (StripnullState *) state;

	appendStringInfoCharMacro(_state->strval, '{');

	return JSON_SUCCESS;
}

static JsonParseErrorType
sn_object_end(void *state)
{
	StripnullState *_state = (StripnullState *) state;

	appendStringInfoCharMacro(_state->strval, '}');

	return JSON_SUCCESS;
}

static JsonParseErrorType
sn_array_start(void *state)
{
	StripnullState *_state = (StripnullState *) state;

	appendStringInfoCharMacro(_state->strval, '[');

	return JSON_SUCCESS;
}

static JsonParseErrorType
sn_array_end(void *state)
{
	StripnullState *_state = (StripnullState *) state;

	appendStringInfoCharMacro(_state->strval, ']');

	return JSON_SUCCESS;
}

static JsonParseErrorType
sn_object_field_start(void *state, char *fname, bool isnull)
{
	StripnullState *_state = (StripnullState *) state;

	if (isnull)
	{
		/*
			 * 下一个必是标量否则 isnull 不可能为真，因此
			 * 无风险把这一状态带入嵌套的对象或数组。
			 * 该标志将在标量动作中被重置。
		 */
		_state->skip_next_null = true;
		return JSON_SUCCESS;
	}

	if (_state->strval->data[_state->strval->len - 1] != '{')
		appendStringInfoCharMacro(_state->strval, ',');

	/*
	 * 遗憾的是带引号的转义字符串已不复存在，所以必须
	 * 对其重新转义。
	 */
	escape_json(_state->strval, fname);

	appendStringInfoCharMacro(_state->strval, ':');

	return JSON_SUCCESS;
}

static JsonParseErrorType
sn_array_element_start(void *state, bool isnull)
{
	StripnullState *_state = (StripnullState *) state;

	if (_state->strval->data[_state->strval->len - 1] != '[')
		appendStringInfoCharMacro(_state->strval, ',');

	return JSON_SUCCESS;
}

static JsonParseErrorType
sn_scalar(void *state, char *token, JsonTokenType tokentype)
{
	StripnullState *_state = (StripnullState *) state;

	if (_state->skip_next_null)
	{
		Assert(tokentype == JSON_TOKEN_NULL);
		_state->skip_next_null = false;
		return JSON_SUCCESS;
	}

	if (tokentype == JSON_TOKEN_STRING)
		escape_json(_state->strval, token);
	else
		appendStringInfoString(_state->strval, token);

	return JSON_SUCCESS;
}

/*
 * SQL 函数 json_strip_nulls(json) -> json
 */
Datum
json_strip_nulls(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_PP(0);
	StripnullState *state;
	JsonLexContext *lex;
	JsonSemAction *sem;

	lex = makeJsonLexContext(json, true);
	state = palloc0(sizeof(StripnullState));
	sem = palloc0(sizeof(JsonSemAction));

	state->strval = makeStringInfo();
	state->skip_next_null = false;
	state->lex = lex;

	sem->semstate = (void *) state;
	sem->object_start = sn_object_start;
	sem->object_end = sn_object_end;
	sem->array_start = sn_array_start;
	sem->array_end = sn_array_end;
	sem->scalar = sn_scalar;
	sem->array_element_start = sn_array_element_start;
	sem->object_field_start = sn_object_field_start;

	pg_parse_json_or_ereport(lex, sem);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(state->strval->data,
											  state->strval->len));
}

/*
 * SQL 函数 jsonb_strip_nulls(jsonb) -> jsonb
 */
Datum
jsonb_strip_nulls(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	JsonbIterator *it;
	JsonbParseState *parseState = NULL;
	JsonbValue *res = NULL;
	JsonbValue	v,
				k;
	JsonbIteratorToken type;
	bool		last_was_key = false;

	if (JB_ROOT_IS_SCALAR(jb))
		PG_RETURN_POINTER(jb);

	it = JsonbIteratorInit(&jb->root);

	while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		Assert(!(type == WJB_KEY && last_was_key));

		if (type == WJB_KEY)
		{
			/* 暂存键，直到得知其值是否为 null */
			k = v;
			last_was_key = true;
			continue;
		}

		if (last_was_key)
		{
			/* 若上一元素是键则这个不能是 */
			last_was_key = false;

			/* 值为null 则跳过此字段 */
			if (type == WJB_VALUE && v.type == jbvNull)
				continue;

			/* 否则延迟压入该键 */
			(void) pushJsonbValue(&parseState, WJB_KEY, &k);
		}

		if (type == WJB_VALUE || type == WJB_ELEM)
			res = pushJsonbValue(&parseState, type, &v);
		else
			res = pushJsonbValue(&parseState, type, NULL);
	}

	Assert(res != NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(res));
}

/*
 * SQL 函数 jsonb_pretty (jsonb)
 *
 * jsonb 的美化打印文本
 */
Datum
jsonb_pretty(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	StringInfo	str = makeStringInfo();

	JsonbToCStringIndent(str, &jb->root, VARSIZE(jb));

	PG_RETURN_TEXT_P(cstring_to_text_with_len(str->data, str->len));
}

/*
 * SQL 函数 jsonb_concat (jsonb, jsonb)
 *
 * || 算子的函数
 */
Datum
jsonb_concat(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb1 = PG_GETARG_JSONB_P(0);
	Jsonb	   *jb2 = PG_GETARG_JSONB_P(1);
	JsonbParseState *state = NULL;
	JsonbValue *res;
	JsonbIterator *it1,
			   *it2;

	/*
	 * 若一个 jsonb 为空，另一个非标量且二者同类，则直接
	 * 返回另一个。若其为标量或二者类别不同，
	 * 则即使其中一个为空也要执行连接。
	 *
	 */
	if (JB_ROOT_IS_OBJECT(jb1) == JB_ROOT_IS_OBJECT(jb2))
	{
		if (JB_ROOT_COUNT(jb1) == 0 && !JB_ROOT_IS_SCALAR(jb2))
			PG_RETURN_JSONB_P(jb2);
		else if (JB_ROOT_COUNT(jb2) == 0 && !JB_ROOT_IS_SCALAR(jb1))
			PG_RETURN_JSONB_P(jb1);
	}

	it1 = JsonbIteratorInit(&jb1->root);
	it2 = JsonbIteratorInit(&jb2->root);

	res = IteratorConcat(&it1, &it2, &state);

	Assert(res != NULL);

	PG_RETURN_JSONB_P(JsonbValueToJsonb(res));
}


/*
 * SQL 函数 jsonb_delete (jsonb, text)
 *
 * 返回删除了指定条目的 jsonb 副本，
 *
 */
Datum
jsonb_delete(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	text	   *key = PG_GETARG_TEXT_PP(1);
	char	   *keyptr = VARDATA_ANY(key);
	int			keylen = VARSIZE_ANY_EXHDR(key);
	JsonbParseState *state = NULL;
	JsonbIterator *it;
	JsonbValue	v,
			   *res = NULL;
	bool		skipNested = false;
	JsonbIteratorToken r;

	if (JB_ROOT_IS_SCALAR(in))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot delete from scalar")));

	if (JB_ROOT_COUNT(in) == 0)
		PG_RETURN_JSONB_P(in);

	it = JsonbIteratorInit(&in->root);

	while ((r = JsonbIteratorNext(&it, &v, skipNested)) != WJB_DONE)
	{
		skipNested = true;

		if ((r == WJB_ELEM || r == WJB_KEY) &&
			(v.type == jbvString && keylen == v.val.string.len &&
			 memcmp(keyptr, v.val.string.val, keylen) == 0))
		{
			/* 同时跳过对应的值 */
			if (r == WJB_KEY)
				(void) JsonbIteratorNext(&it, &v, true);

			continue;
		}

		res = pushJsonbValue(&state, r, r < WJB_BEGIN_ARRAY ? &v : NULL);
	}

	Assert(res != NULL);

	PG_RETURN_JSONB_P(JsonbValueToJsonb(res));
}

/*
 * SQL 函数 jsonb_delete (jsonb, variadic text[])
 *
 * 返回删除了指定条目的 jsonb 副本，
 *
 */
Datum
jsonb_delete_array(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	ArrayType  *keys = PG_GETARG_ARRAYTYPE_P(1);
	Datum	   *keys_elems;
	bool	   *keys_nulls;
	int			keys_len;
	JsonbParseState *state = NULL;
	JsonbIterator *it;
	JsonbValue	v,
			   *res = NULL;
	bool		skipNested = false;
	JsonbIteratorToken r;

	if (ARR_NDIM(keys) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	if (JB_ROOT_IS_SCALAR(in))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot delete from scalar")));

	if (JB_ROOT_COUNT(in) == 0)
		PG_RETURN_JSONB_P(in);

	deconstruct_array_builtin(keys, TEXTOID, &keys_elems, &keys_nulls, &keys_len);

	if (keys_len == 0)
		PG_RETURN_JSONB_P(in);

	it = JsonbIteratorInit(&in->root);

	while ((r = JsonbIteratorNext(&it, &v, skipNested)) != WJB_DONE)
	{
		skipNested = true;

		if ((r == WJB_ELEM || r == WJB_KEY) && v.type == jbvString)
		{
			int			i;
			bool		found = false;

			for (i = 0; i < keys_len; i++)
			{
				char	   *keyptr;
				int			keylen;

				if (keys_nulls[i])
					continue;

				/* 我们依赖数组元素未被 toast 的前提 */
				keyptr = VARDATA_ANY(keys_elems[i]);
				keylen = VARSIZE_ANY_EXHDR(keys_elems[i]);
				if (keylen == v.val.string.len &&
					memcmp(keyptr, v.val.string.val, keylen) == 0)
				{
					found = true;
					break;
				}
			}
			if (found)
			{
				/* 同时跳过对应的值 */
				if (r == WJB_KEY)
					(void) JsonbIteratorNext(&it, &v, true);

				continue;
			}
		}

		res = pushJsonbValue(&state, r, r < WJB_BEGIN_ARRAY ? &v : NULL);
	}

	Assert(res != NULL);

	PG_RETURN_JSONB_P(JsonbValueToJsonb(res));
}

/*
 * SQL 函数 jsonb_delete (jsonb, int)
 *
 * 返回删除了指定条目的 jsonb 副本，
 * 负 int 表示从条目末尾向前计数。
 *
 */
Datum
jsonb_delete_idx(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	int			idx = PG_GETARG_INT32(1);
	JsonbParseState *state = NULL;
	JsonbIterator *it;
	uint32		i = 0,
				n;
	JsonbValue	v,
			   *res = NULL;
	JsonbIteratorToken r;

	if (JB_ROOT_IS_SCALAR(in))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot delete from scalar")));

	if (JB_ROOT_IS_OBJECT(in))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot delete from object using integer index")));

	if (JB_ROOT_COUNT(in) == 0)
		PG_RETURN_JSONB_P(in);

	it = JsonbIteratorInit(&in->root);

	r = JsonbIteratorNext(&it, &v, false);
	Assert(r == WJB_BEGIN_ARRAY);
	n = v.val.array.nElems;

	if (idx < 0)
	{
		if (-idx > n)
			idx = n;
		else
			idx = n + idx;
	}

	if (idx >= n)
		PG_RETURN_JSONB_P(in);

	pushJsonbValue(&state, r, NULL);

	while ((r = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
	{
		if (r == WJB_ELEM)
		{
			if (i++ == idx)
				continue;
		}

		res = pushJsonbValue(&state, r, r < WJB_BEGIN_ARRAY ? &v : NULL);
	}

	Assert(res != NULL);

	PG_RETURN_JSONB_P(JsonbValueToJsonb(res));
}

/*
 * SQL 函数 jsonb_set(jsonb, text[], jsonb, boolean)
 */
Datum
jsonb_set(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	ArrayType  *path = PG_GETARG_ARRAYTYPE_P(1);
	Jsonb	   *newjsonb = PG_GETARG_JSONB_P(2);
	JsonbValue	newval;
	bool		create = PG_GETARG_BOOL(3);
	JsonbValue *res = NULL;
	Datum	   *path_elems;
	bool	   *path_nulls;
	int			path_len;
	JsonbIterator *it;
	JsonbParseState *st = NULL;

	JsonbToJsonbValue(newjsonb, &newval);

	if (ARR_NDIM(path) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	if (JB_ROOT_IS_SCALAR(in))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot set path in scalar")));

	if (JB_ROOT_COUNT(in) == 0 && !create)
		PG_RETURN_JSONB_P(in);

	deconstruct_array_builtin(path, TEXTOID, &path_elems, &path_nulls, &path_len);

	if (path_len == 0)
		PG_RETURN_JSONB_P(in);

	it = JsonbIteratorInit(&in->root);

	res = setPath(&it, path_elems, path_nulls, path_len, &st,
				  0, &newval, create ? JB_PATH_CREATE : JB_PATH_REPLACE);

	Assert(res != NULL);

	PG_RETURN_JSONB_P(JsonbValueToJsonb(res));
}


/*
 * SQL 函数 jsonb_set_lax(jsonb, text[], jsonb, boolean, text)
 */
Datum
jsonb_set_lax(PG_FUNCTION_ARGS)
{
	/* Jsonb	   *in = PG_GETARG_JSONB_P(0); */
	/* ArrayType  *path = PG_GETARG_ARRAYTYPE_P(1); */
	/* Jsonb	  *newval = PG_GETARG_JSONB_P(2); */
	/* bool		create = PG_GETARG_BOOL(3); */
	text	   *handle_null;
	char	   *handle_val;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(3))
		PG_RETURN_NULL();

	/* 若调用者显式传入 NULL 则可能发生 */
	if (PG_ARGISNULL(4))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("null_value_treatment must be \"delete_key\", \"return_target\", \"use_json_null\", or \"raise_exception\"")));

	/* 若新值不是 SQL NULL 则直接调用 jsonb_set */
	if (!PG_ARGISNULL(2))
		return jsonb_set(fcinfo);

	handle_null = PG_GETARG_TEXT_P(4);
	handle_val = text_to_cstring(handle_null);

	if (strcmp(handle_val, "raise_exception") == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("JSON value must not be null"),
				 errdetail("Exception was raised because null_value_treatment is \"raise_exception\"."),
				 errhint("To avoid, either change the null_value_treatment argument or ensure that an SQL NULL is not passed.")));
		return (Datum) 0;		/* 让较笨编译器安静 */
	}
	else if (strcmp(handle_val, "use_json_null") == 0)
	{
		Datum		newval;

		newval = DirectFunctionCall1(jsonb_in, CStringGetDatum("null"));

		fcinfo->args[2].value = newval;
		fcinfo->args[2].isnull = false;
		return jsonb_set(fcinfo);
	}
	else if (strcmp(handle_val, "delete_key") == 0)
	{
		return jsonb_delete_path(fcinfo);
	}
	else if (strcmp(handle_val, "return_target") == 0)
	{
		Jsonb	   *in = PG_GETARG_JSONB_P(0);

		PG_RETURN_JSONB_P(in);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("null_value_treatment must be \"delete_key\", \"return_target\", \"use_json_null\", or \"raise_exception\"")));
		return (Datum) 0;		/* 让较笨编译器安静 */
	}
}

/*
 * SQL 函数 jsonb_delete_path(jsonb, text[])
 */
Datum
jsonb_delete_path(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	ArrayType  *path = PG_GETARG_ARRAYTYPE_P(1);
	JsonbValue *res = NULL;
	Datum	   *path_elems;
	bool	   *path_nulls;
	int			path_len;
	JsonbIterator *it;
	JsonbParseState *st = NULL;

	if (ARR_NDIM(path) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	if (JB_ROOT_IS_SCALAR(in))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot delete path in scalar")));

	if (JB_ROOT_COUNT(in) == 0)
		PG_RETURN_JSONB_P(in);

	deconstruct_array_builtin(path, TEXTOID, &path_elems, &path_nulls, &path_len);

	if (path_len == 0)
		PG_RETURN_JSONB_P(in);

	it = JsonbIteratorInit(&in->root);

	res = setPath(&it, path_elems, path_nulls, path_len, &st,
				  0, NULL, JB_PATH_DELETE);

	Assert(res != NULL);

	PG_RETURN_JSONB_P(JsonbValueToJsonb(res));
}

/*
 * SQL 函数 jsonb_insert(jsonb, text[], jsonb, boolean)
 */
Datum
jsonb_insert(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	ArrayType  *path = PG_GETARG_ARRAYTYPE_P(1);
	Jsonb	   *newjsonb = PG_GETARG_JSONB_P(2);
	JsonbValue	newval;
	bool		after = PG_GETARG_BOOL(3);
	JsonbValue *res = NULL;
	Datum	   *path_elems;
	bool	   *path_nulls;
	int			path_len;
	JsonbIterator *it;
	JsonbParseState *st = NULL;

	JsonbToJsonbValue(newjsonb, &newval);

	if (ARR_NDIM(path) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	if (JB_ROOT_IS_SCALAR(in))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot set path in scalar")));

	deconstruct_array_builtin(path, TEXTOID, &path_elems, &path_nulls, &path_len);

	if (path_len == 0)
		PG_RETURN_JSONB_P(in);

	it = JsonbIteratorInit(&in->root);

	res = setPath(&it, path_elems, path_nulls, path_len, &st, 0, &newval,
				  after ? JB_PATH_INSERT_AFTER : JB_PATH_INSERT_BEFORE);

	Assert(res != NULL);

	PG_RETURN_JSONB_P(JsonbValueToJsonb(res));
}

/*
 * 遍历所有 jsonb 对象并将之合并成一个。
 * 本函数的逻辑复制自 hstore 的同名函数，
 * 除了 it1 与 it2 表示 jbvObject 的情形。
 * 该情形下我们直接把 it2 的内容不校验地追加
 * 到 it1。
 */
static JsonbValue *
IteratorConcat(JsonbIterator **it1, JsonbIterator **it2,
			   JsonbParseState **state)
{
	JsonbValue	v1,
				v2,
			   *res = NULL;
	JsonbIteratorToken r1,
				r2,
				rk1,
				rk2;

	rk1 = JsonbIteratorNext(it1, &v1, false);
	rk2 = JsonbIteratorNext(it2, &v2, false);

	/*
	 * JsonbIteratorNext 会把 raw scalar 报成单元素数组；
	 * 因此此处只需考虑 "object" 与 "array" 两种情形。
	 */
	if (rk1 == WJB_BEGIN_OBJECT && rk2 == WJB_BEGIN_OBJECT)
	{
		/*
			 * 双方皆为对象。
		 *
			 * 把 v1 的记号都追加到 res，除最后的 WJB_END_OBJECT
			 * （因为 res 尚未完结）。
		 */
		pushJsonbValue(state, rk1, NULL);
		while ((r1 = JsonbIteratorNext(it1, &v1, true)) != WJB_END_OBJECT)
			pushJsonbValue(state, r1, &v1);

		/*
			 * 把 v2 的记号都追加到 res，包括最后的 WJB_END_OBJECT
			 * （拼接即告完成）。任何重复键都会自动
			 * 覆盖第一个对象的值。
		 */
		while ((r2 = JsonbIteratorNext(it2, &v2, true)) != WJB_DONE)
			res = pushJsonbValue(state, r2, r2 != WJB_END_OBJECT ? &v2 : NULL);
	}
	else if (rk1 == WJB_BEGIN_ARRAY && rk2 == WJB_BEGIN_ARRAY)
	{
		/*
			 * 两者皆为数组。
		 */
		pushJsonbValue(state, rk1, NULL);

		while ((r1 = JsonbIteratorNext(it1, &v1, true)) != WJB_END_ARRAY)
		{
			Assert(r1 == WJB_ELEM);
			pushJsonbValue(state, r1, &v1);
		}

		while ((r2 = JsonbIteratorNext(it2, &v2, true)) != WJB_END_ARRAY)
		{
			Assert(r2 == WJB_ELEM);
			pushJsonbValue(state, WJB_ELEM, &v2);
		}

		res = pushJsonbValue(state, WJB_END_ARRAY, NULL /* 通知排序 */ );
	}
	else if (rk1 == WJB_BEGIN_OBJECT)
	{
		/*
			 * 即对象 || 数组。
		 */
		Assert(rk2 == WJB_BEGIN_ARRAY);

		pushJsonbValue(state, WJB_BEGIN_ARRAY, NULL);

		pushJsonbValue(state, WJB_BEGIN_OBJECT, NULL);
		while ((r1 = JsonbIteratorNext(it1, &v1, true)) != WJB_DONE)
			pushJsonbValue(state, r1, r1 != WJB_END_OBJECT ? &v1 : NULL);

		while ((r2 = JsonbIteratorNext(it2, &v2, true)) != WJB_DONE)
			res = pushJsonbValue(state, r2, r2 != WJB_END_ARRAY ? &v2 : NULL);
	}
	else
	{
		/*
			 * 即数组 || 对象。
		 */
		Assert(rk1 == WJB_BEGIN_ARRAY);
		Assert(rk2 == WJB_BEGIN_OBJECT);

		pushJsonbValue(state, WJB_BEGIN_ARRAY, NULL);

		while ((r1 = JsonbIteratorNext(it1, &v1, true)) != WJB_END_ARRAY)
			pushJsonbValue(state, r1, &v1);

		pushJsonbValue(state, WJB_BEGIN_OBJECT, NULL);
		while ((r2 = JsonbIteratorNext(it2, &v2, true)) != WJB_DONE)
			pushJsonbValue(state, r2, r2 != WJB_END_OBJECT ? &v2 : NULL);

		res = pushJsonbValue(state, WJB_END_ARRAY, NULL);
	}

	return res;
}

/*
 * 为 jsonb_set/jsonb_insert 完成大部分重活
 *
 * 若 op_type 中设置了 JB_PATH_DELETE 位，则要移除元素。
 *
 * 若 op_type 设置了 JB_PATH_CREATE_OR_INSERT 中任一位，
 * 则在键或数组下标不存在时创建新值。
 *
 * op_type 中的 JB_PATH_INSERT_BEFORE 与 JB_PATH_INSERT_AFTER
 * 位在新值插入 JsonbObject 时行为如同 JB_PATH_CREATE。
 *
 * 若设置了 JB_PATH_FILL_GAPS 位，当目标为数组时赋值逻辑
 * 将会改变。赋值下标将不受数组元素数量的限制，
 * 且若数组最后一个元素与新元素之间存在空槽，
 * 这些空槽将以 null 填充。若
 * 下标为负，它仍将被视为从数组末尾起算的下标。若
 * 路径的某部分不存在且该部分不止最后一个元素，
 * 此标志将指示创建整条相应对象链并
 * 插入该值。
 *
 * JB_PATH_CONSISTENT_POSITION 对数组而言表示调用者想要
 * 保持具有固定下标的值。现有元素的下标可能被改变
 * （前移）——当数组被前插新值且负下标越界的
 * 时候，这一行为将被阻止并返回
 * 错误。
 *
 * 最后一个之前全部路径元素必须已存在
 * ——无论 op_type 设何位，否则不做。
 */
static JsonbValue *
setPath(JsonbIterator **it, Datum *path_elems,
		bool *path_nulls, int path_len,
		JsonbParseState **st, int level, JsonbValue *newval, int op_type)
{
	JsonbValue	v;
	JsonbIteratorToken r;
	JsonbValue *res;

	check_stack_depth();

	if (path_nulls[level])
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("path element at position %d is null",
						level + 1)));

	r = JsonbIteratorNext(it, &v, false);

	switch (r)
	{
		case WJB_BEGIN_ARRAY:

			/*
				 * 若被指示，对在 raw
				 * scalar 内替换企图也报错，即使当前层级等于
				 * path_len 也如此，因为最后路径键同样应对应
				 * 对象或数组而非 raw scalar。
			 */
			if ((op_type & JB_PATH_FILL_GAPS) && (level <= path_len - 1) &&
				v.val.array.rawScalar)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("cannot replace existing key"),
						 errdetail("The path assumes key is a composite object, "
								   "but it is a scalar value.")));

			(void) pushJsonbValue(st, r, NULL);
			setPathArray(it, path_elems, path_nulls, path_len, st, level,
						 newval, v.val.array.nElems, op_type);
			r = JsonbIteratorNext(it, &v, false);
			Assert(r == WJB_END_ARRAY);
			res = pushJsonbValue(st, r, NULL);
			break;
		case WJB_BEGIN_OBJECT:
			(void) pushJsonbValue(st, r, NULL);
			setPathObject(it, path_elems, path_nulls, path_len, st, level,
						  newval, v.val.object.nPairs, op_type);
			r = JsonbIteratorNext(it, &v, true);
			Assert(r == WJB_END_OBJECT);
			res = pushJsonbValue(st, r, NULL);
			break;
		case WJB_ELEM:
		case WJB_VALUE:

			/*
				 * 若被指示，对试图在
				 * scalar 内替换企图也报错，即使当前层级等于
				 * path_len 也如此，因为最后路径键同样应对应
				 * 对象或数组而非元素或值。
			 */
			if ((op_type & JB_PATH_FILL_GAPS) && (level <= path_len - 1))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("cannot replace existing key"),
						 errdetail("The path assumes key is a composite object, "
								   "but it is a scalar value.")));

			res = pushJsonbValue(st, r, &v);
			break;
		default:
			elog(ERROR, "unrecognized iterator result: %d", (int) r);
			res = NULL;			/* 让编译器安静 */
			break;
	}

	return res;
}

/*
 * setPath的对象遍历器
 */
static void
setPathObject(JsonbIterator **it, Datum *path_elems, bool *path_nulls,
			  int path_len, JsonbParseState **st, int level,
			  JsonbValue *newval, uint32 npairs, int op_type)
{
	text	   *pathelem = NULL;
	int			i;
	JsonbValue	k,
				v;
	bool		done = false;

	if (level >= path_len || path_nulls[level])
		done = true;
	else
	{
		/* 路径 Datum 可能被 toast，此时必须对其去 toast */
		pathelem = DatumGetTextPP(path_elems[level]);
	}

	/* 空对象是创建时的特例 */
	if ((npairs == 0) && (op_type & JB_PATH_CREATE_OR_INSERT) &&
		(level == path_len - 1))
	{
		JsonbValue	newkey;

		newkey.type = jbvString;
		newkey.val.string.val = VARDATA_ANY(pathelem);
		newkey.val.string.len = VARSIZE_ANY_EXHDR(pathelem);

		(void) pushJsonbValue(st, WJB_KEY, &newkey);
		(void) pushJsonbValue(st, WJB_VALUE, newval);
	}

	for (i = 0; i < npairs; i++)
	{
		JsonbIteratorToken r = JsonbIteratorNext(it, &k, true);

		Assert(r == WJB_KEY);

		if (!done &&
			k.val.string.len == VARSIZE_ANY_EXHDR(pathelem) &&
			memcmp(k.val.string.val, VARDATA_ANY(pathelem),
				   k.val.string.len) == 0)
		{
			done = true;

			if (level == path_len - 1)
			{
				/*
					 * 由 jsonb_insert() 调用，禁止重定义
					 * 已有值
				 */
				if (op_type & (JB_PATH_INSERT_BEFORE | JB_PATH_INSERT_AFTER))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("cannot replace existing key"),
							 errhint("Try using the function jsonb_set "
									 "to replace key value.")));

				r = JsonbIteratorNext(it, &v, true);	/* 跳过值 */
				if (!(op_type & JB_PATH_DELETE))
				{
					(void) pushJsonbValue(st, WJB_KEY, &k);
					(void) pushJsonbValue(st, WJB_VALUE, newval);
				}
			}
			else
			{
				(void) pushJsonbValue(st, r, &k);
				setPath(it, path_elems, path_nulls, path_len,
						st, level + 1, newval, op_type);
			}
		}
		else
		{
			if ((op_type & JB_PATH_CREATE_OR_INSERT) && !done &&
				level == path_len - 1 && i == npairs - 1)
			{
				JsonbValue	newkey;

				newkey.type = jbvString;
				newkey.val.string.val = VARDATA_ANY(pathelem);
				newkey.val.string.len = VARSIZE_ANY_EXHDR(pathelem);

				(void) pushJsonbValue(st, WJB_KEY, &newkey);
				(void) pushJsonbValue(st, WJB_VALUE, newval);
			}

			(void) pushJsonbValue(st, r, &k);
			r = JsonbIteratorNext(it, &v, false);
			(void) pushJsonbValue(st, r, r < WJB_BEGIN_ARRAY ? &v : NULL);
			if (r == WJB_BEGIN_ARRAY || r == WJB_BEGIN_OBJECT)
			{
				int			walking_level = 1;

				while (walking_level != 0)
				{
					r = JsonbIteratorNext(it, &v, false);

					if (r == WJB_BEGIN_ARRAY || r == WJB_BEGIN_OBJECT)
						++walking_level;
					if (r == WJB_END_ARRAY || r == WJB_END_OBJECT)
						--walking_level;

					(void) pushJsonbValue(st, r, r < WJB_BEGIN_ARRAY ? &v : NULL);
				}
			}
		}
	}

	/*--
	 * 走到这里只可能有几种情况：
	 * - 未找到目标路径，且带若干键/值的开放对象被压入
	 *   状态。
	 * - 对象为空，只压入了 WJB_BEGIN_OBJECT
	 *
	 * 两种情况下，若被指示在路径缺失时创建路径，
	 * 就生成整条空对象链并在其中插入新值。
	 *
	 */
	if (!done && (op_type & JB_PATH_FILL_GAPS) && (level < path_len - 1))
	{
		JsonbValue	newkey;

		newkey.type = jbvString;
		newkey.val.string.val = VARDATA_ANY(pathelem);
		newkey.val.string.len = VARSIZE_ANY_EXHDR(pathelem);

		(void) pushJsonbValue(st, WJB_KEY, &newkey);
		(void) push_path(st, level, path_elems, path_nulls,
						 path_len, newval);

		/* 结果在本函数之外以 WJB_END_OBJECT 闭合 */
	}
}

/*
 * setPath 数组遍历器
 */
static void
setPathArray(JsonbIterator **it, Datum *path_elems, bool *path_nulls,
			 int path_len, JsonbParseState **st, int level,
			 JsonbValue *newval, uint32 nelems, int op_type)
{
	JsonbValue	v;
	int			idx,
				i;
	bool		done = false;

	/* 选正确下标 */
	if (level < path_len && !path_nulls[level])
	{
		char	   *c = TextDatumGetCString(path_elems[level]);
		char	   *badp;

		errno = 0;
		idx = strtoint(c, &badp, 10);
		if (badp == c || *badp != '\0' || errno != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("path element at position %d is not an integer: \"%s\"",
							level + 1, c)));
	}
	else
		idx = nelems;

	if (idx < 0)
	{
		if (-idx > nelems)
		{
			/*
				 * 若被要求保持元素位置一致，则不允许前插
				 * 数组。
			 */
			if (op_type & JB_PATH_CONSISTENT_POSITION)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("path element at position %d is out of range: %d",
								level + 1, idx)));
			else
				idx = INT_MIN;
		}
		else
			idx = nelems + idx;
	}

	/*
	 * 填充空隙意味着对正下标不设限制，我们可以
	 * 设置任意元素。否则以 nelems 限制下标。
	 */
	if (!(op_type & JB_PATH_FILL_GAPS))
	{
		if (idx > 0 && idx > nelems)
			idx = nelems;
	}

	/*
	 * 若处于创建态且 idx == INT_MIN，我们会把新值前插到
	 * 数组，若数组为空也是如此——此时我们并不
	 * 在意 idx 的值。
	 */
	if ((idx == INT_MIN || nelems == 0) && (level == path_len - 1) &&
		(op_type & JB_PATH_CREATE_OR_INSERT))
	{
		Assert(newval != NULL);

		if (op_type & JB_PATH_FILL_GAPS && nelems == 0 && idx > 0)
			push_null_elements(st, idx);

		(void) pushJsonbValue(st, WJB_ELEM, newval);

		done = true;
	}

	/* 遍历数组元素 */
	for (i = 0; i < nelems; i++)
	{
		JsonbIteratorToken r;

		if (i == idx && level < path_len)
		{
			done = true;

			if (level == path_len - 1)
			{
				r = JsonbIteratorNext(it, &v, true);	/*跳过*/

				if (op_type & (JB_PATH_INSERT_BEFORE | JB_PATH_CREATE))
					(void) pushJsonbValue(st, WJB_ELEM, newval);

				/*
					 * 只有 JB_PATH_INSERT_BEFORE 或
					 * JB_PATH_INSERT_AFTER 才保留，否则应
					 * 将其删除或替换。
				 */
				if (op_type & (JB_PATH_INSERT_AFTER | JB_PATH_INSERT_BEFORE))
					(void) pushJsonbValue(st, r, &v);

				if (op_type & (JB_PATH_INSERT_AFTER | JB_PATH_REPLACE))
					(void) pushJsonbValue(st, WJB_ELEM, newval);
			}
			else
				(void) setPath(it, path_elems, path_nulls, path_len,
							   st, level + 1, newval, op_type);
		}
		else
		{
			r = JsonbIteratorNext(it, &v, false);

			(void) pushJsonbValue(st, r, r < WJB_BEGIN_ARRAY ? &v : NULL);

			if (r == WJB_BEGIN_ARRAY || r == WJB_BEGIN_OBJECT)
			{
				int			walking_level = 1;

				while (walking_level != 0)
				{
					r = JsonbIteratorNext(it, &v, false);

					if (r == WJB_BEGIN_ARRAY || r == WJB_BEGIN_OBJECT)
						++walking_level;
					if (r == WJB_END_ARRAY || r == WJB_END_OBJECT)
						--walking_level;

					(void) pushJsonbValue(st, r, r < WJB_BEGIN_ARRAY ? &v : NULL);
				}
			}
		}
	}

	if ((op_type & JB_PATH_CREATE_OR_INSERT) && !done && level == path_len - 1)
	{
		/*
			 * 若被要求填空隙，idx 可能大于 nelems，那么
			 * 就用 null 前插新元素。
		 */
		if (op_type & JB_PATH_FILL_GAPS && idx > nelems)
			push_null_elements(st, idx - nelems);

		(void) pushJsonbValue(st, WJB_ELEM, newval);
		done = true;
	}

	/*--
	 * 走到这里只可能有几种情况：
	 * - 未找到目标路径，带若干键/值的开放数组被压入
	 *   状态。
	 * - 数组为空，只压入了 WJB_BEGIN_ARRAY
	 *
	 * 两种情况下，若被指示在路径缺失时创建路径，
	 * 就生成整条空对象链并在其中插入新值。
	 *
	 */
	if (!done && (op_type & JB_PATH_FILL_GAPS) && (level < path_len - 1))
	{
		if (idx > 0)
			push_null_elements(st, idx - nelems);

		(void) push_path(st, level, path_elems, path_nulls,
						 path_len, newval);

		/* 结果在本函数之外以 WJB_END_OBJECT 闭合 */
	}
}

/*
 * 解析我们想在 iterate_json(b)_values 函数中遍历 jsonb 文档
 * 的哪些元素的信息。该信息以 jsonb 格式表示，便于日后
 * 轻松扩展。
 */
uint32
parse_jsonb_index_flags(Jsonb *jb)
{
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken type;
	uint32		flags = 0;

	it = JsonbIteratorInit(&jb->root);

	type = JsonbIteratorNext(&it, &v, false);

	/*
	 * 我们遍历数组（标量内部也表示为数组，故也接受）
	 * 以检查其全部元素。标志名的选取与
	 * jsonb_typeof 所用的一致。
	 */
	if (type != WJB_BEGIN_ARRAY)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("wrong flag type, only arrays and scalars are allowed")));

	while ((type = JsonbIteratorNext(&it, &v, false)) == WJB_ELEM)
	{
		if (v.type != jbvString)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("flag array element is not a string"),
					 errhint("Possible values are: \"string\", \"numeric\", \"boolean\", \"key\", and \"all\".")));

		if (v.val.string.len == 3 &&
			pg_strncasecmp(v.val.string.val, "all", 3) == 0)
			flags |= jtiAll;
		else if (v.val.string.len == 3 &&
				 pg_strncasecmp(v.val.string.val, "key", 3) == 0)
			flags |= jtiKey;
		else if (v.val.string.len == 6 &&
				 pg_strncasecmp(v.val.string.val, "string", 6) == 0)
			flags |= jtiString;
		else if (v.val.string.len == 7 &&
				 pg_strncasecmp(v.val.string.val, "numeric", 7) == 0)
			flags |= jtiNumeric;
		else if (v.val.string.len == 7 &&
				 pg_strncasecmp(v.val.string.val, "boolean", 7) == 0)
			flags |= jtiBool;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("wrong flag in flag array: \"%s\"",
							pnstrdup(v.val.string.val, v.val.string.len)),
					 errhint("Possible values are: \"string\", \"numeric\", \"boolean\", \"key\", and \"all\".")));
	}

	/* 该是数组结尾了 */
	if (type != WJB_END_ARRAY)
		elog(ERROR, "unexpected end of flag array");

	/* 取 WJB_DONE 并释放迭代器 */
	type = JsonbIteratorNext(&it, &v, false);
	if (type != WJB_DONE)
		elog(ERROR, "unexpected end of flag array");

	return flags;
}

/*
 * 按 flags 的指定遍历 jsonb 值或元素，把它们连同迭代
 * 状态一起传给指定的 JsonIterateStringValuesAction。
 */
void
iterate_jsonb_values(Jsonb *jb, uint32 flags, void *state,
					 JsonIterateStringValuesAction action)
{
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken type;

	it = JsonbIteratorInit(&jb->root);

	/*
	 * 只是递归遍历 jsonb 并对所有对应元素调用回调
	 * 即可。
	 */
	while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		if (type == WJB_KEY)
		{
			if (flags & jtiKey)
				action(state, v.val.string.val, v.val.string.len);

			continue;
		}
		else if (!(type == WJB_VALUE || type == WJB_ELEM))
		{
			/* 不为复合 JsonbValue 调用回调 */
			continue;
		}

		/* JsonbValue 是对象的值或数组的元素 */
		switch (v.type)
		{
			case jbvString:
				if (flags & jtiString)
					action(state, v.val.string.val, v.val.string.len);
				break;
			case jbvNumeric:
				if (flags & jtiNumeric)
				{
					char	   *val;

					val = DatumGetCString(DirectFunctionCall1(numeric_out,
															  NumericGetDatum(v.val.numeric)));

					action(state, val, strlen(val));
					pfree(val);
				}
				break;
			case jbvBool:
				if (flags & jtiBool)
				{
					if (v.val.boolean)
						action(state, "true", 4);
					else
						action(state, "false", 5);
				}
				break;
			default:
				/* 不为复合 JsonbValue 调用回调 */
				break;
		}
	}
}

/*
 * 按 flags 的指定遍历 json 值与元素，把它们连同迭代
 * 状态一起传给指定的 JsonIterateStringValuesAction。
 */
void
iterate_json_values(text *json, uint32 flags, void *action_state,
					JsonIterateStringValuesAction action)
{
	JsonLexContext *lex = makeJsonLexContext(json, true);
	JsonSemAction *sem = palloc0(sizeof(JsonSemAction));
	IterateJsonStringValuesState *state = palloc0(sizeof(IterateJsonStringValuesState));

	state->lex = lex;
	state->action = action;
	state->action_state = action_state;
	state->flags = flags;

	sem->semstate = (void *) state;
	sem->scalar = iterate_values_scalar;
	sem->object_field_start = iterate_values_object_field_start;

	pg_parse_json_or_ereport(lex, sem);
}

/*
 * iterate_json_values 的辅助函数，对指定值调用指定的
 * JsonIterateStringValuesAction。
 */
static JsonParseErrorType
iterate_values_scalar(void *state, char *token, JsonTokenType tokentype)
{
	IterateJsonStringValuesState *_state = (IterateJsonStringValuesState *) state;

	switch (tokentype)
	{
		case JSON_TOKEN_STRING:
			if (_state->flags & jtiString)
				_state->action(_state->action_state, token, strlen(token));
			break;
		case JSON_TOKEN_NUMBER:
			if (_state->flags & jtiNumeric)
				_state->action(_state->action_state, token, strlen(token));
			break;
		case JSON_TOKEN_TRUE:
		case JSON_TOKEN_FALSE:
			if (_state->flags & jtiBool)
				_state->action(_state->action_state, token, strlen(token));
			break;
		default:
			/* 不为任何其他记号调用回调 */
			break;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
iterate_values_object_field_start(void *state, char *fname, bool isnull)
{
	IterateJsonStringValuesState *_state = (IterateJsonStringValuesState *) state;

	if (_state->flags & jtiKey)
	{
		char	   *val = pstrdup(fname);

		_state->action(_state->action_state, val, strlen(val));
	}

	return JSON_SUCCESS;
}

/*
 * 遍历一个 jsonb，并对每个字符串值或元素应用一个
 * 指定的 JsonTransformStringValuesAction。所需的任何
 * 上下文都可以经 action_state 变量传入。
 * 函数返回变换后值的原 jsonb 对象副本。
 */
Jsonb *
transform_jsonb_string_values(Jsonb *jsonb, void *action_state,
							  JsonTransformStringValuesAction transform_action)
{
	JsonbIterator *it;
	JsonbValue	v,
			   *res = NULL;
	JsonbIteratorToken type;
	JsonbParseState *st = NULL;
	text	   *out;
	bool		is_scalar = false;

	it = JsonbIteratorInit(&jsonb->root);
	is_scalar = it->isScalar;

	while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		if ((type == WJB_VALUE || type == WJB_ELEM) && v.type == jbvString)
		{
			out = transform_action(action_state, v.val.string.val, v.val.string.len);
			/* out 大概未被 toast，但还是确认一下 */
			out = pg_detoast_datum_packed(out);
			v.val.string.val = VARDATA_ANY(out);
			v.val.string.len = VARSIZE_ANY_EXHDR(out);
			res = pushJsonbValue(&st, type, type < WJB_BEGIN_ARRAY ? &v : NULL);
		}
		else
		{
			res = pushJsonbValue(&st, type, (type == WJB_KEY ||
											 type == WJB_VALUE ||
											 type == WJB_ELEM) ? &v : NULL);
		}
	}

	if (res->type == jbvArray)
		res->val.array.rawScalar = is_scalar;

	return JsonbValueToJsonb(res);
}

/*
 * 遍历一个 json，并对每个字符串值或元素应用一个
 * 指定的 JsonTransformStringValuesAction。所需的任何
 * 上下文都可以经 action_state 变量传入。
 * StringInfo，即原 json 的一份已变换
 * 值的副本。
 */
text *
transform_json_string_values(text *json, void *action_state,
							 JsonTransformStringValuesAction transform_action)
{
	JsonLexContext *lex = makeJsonLexContext(json, true);
	JsonSemAction *sem = palloc0(sizeof(JsonSemAction));
	TransformJsonStringValuesState *state = palloc0(sizeof(TransformJsonStringValuesState));

	state->lex = lex;
	state->strval = makeStringInfo();
	state->action = transform_action;
	state->action_state = action_state;

	sem->semstate = (void *) state;
	sem->object_start = transform_string_values_object_start;
	sem->object_end = transform_string_values_object_end;
	sem->array_start = transform_string_values_array_start;
	sem->array_end = transform_string_values_array_end;
	sem->scalar = transform_string_values_scalar;
	sem->array_element_start = transform_string_values_array_element_start;
	sem->object_field_start = transform_string_values_object_field_start;

	pg_parse_json_or_ereport(lex, sem);

	return cstring_to_text_with_len(state->strval->data, state->strval->len);
}

/*
 * transform_json_string_values 的一组辅助函数，对所有值
 * 调用指定的 JsonTransformStringValuesAction，其余一切
 * 不动。
 */
static JsonParseErrorType
transform_string_values_object_start(void *state)
{
	TransformJsonStringValuesState *_state = (TransformJsonStringValuesState *) state;

	appendStringInfoCharMacro(_state->strval, '{');

	return JSON_SUCCESS;
}

static JsonParseErrorType
transform_string_values_object_end(void *state)
{
	TransformJsonStringValuesState *_state = (TransformJsonStringValuesState *) state;

	appendStringInfoCharMacro(_state->strval, '}');

	return JSON_SUCCESS;
}

static JsonParseErrorType
transform_string_values_array_start(void *state)
{
	TransformJsonStringValuesState *_state = (TransformJsonStringValuesState *) state;

	appendStringInfoCharMacro(_state->strval, '[');

	return JSON_SUCCESS;
}

static JsonParseErrorType
transform_string_values_array_end(void *state)
{
	TransformJsonStringValuesState *_state = (TransformJsonStringValuesState *) state;

	appendStringInfoCharMacro(_state->strval, ']');

	return JSON_SUCCESS;
}

static JsonParseErrorType
transform_string_values_object_field_start(void *state, char *fname, bool isnull)
{
	TransformJsonStringValuesState *_state = (TransformJsonStringValuesState *) state;

	if (_state->strval->data[_state->strval->len - 1] != '{')
		appendStringInfoCharMacro(_state->strval, ',');

	/*
	 * 遗憾的是带引号的转义字符串已不复存在，所以必须
	 * 对其重新转义。
	 */
	escape_json(_state->strval, fname);
	appendStringInfoCharMacro(_state->strval, ':');

	return JSON_SUCCESS;
}

static JsonParseErrorType
transform_string_values_array_element_start(void *state, bool isnull)
{
	TransformJsonStringValuesState *_state = (TransformJsonStringValuesState *) state;

	if (_state->strval->data[_state->strval->len - 1] != '[')
		appendStringInfoCharMacro(_state->strval, ',');

	return JSON_SUCCESS;
}

static JsonParseErrorType
transform_string_values_scalar(void *state, char *token, JsonTokenType tokentype)
{
	TransformJsonStringValuesState *_state = (TransformJsonStringValuesState *) state;

	if (tokentype == JSON_TOKEN_STRING)
	{
		text	   *out = _state->action(_state->action_state, token, strlen(token));

		escape_json(_state->strval, text_to_cstring(out));
	}
	else
		appendStringInfoString(_state->strval, token);

	return JSON_SUCCESS;
}

JsonTokenType
json_get_first_token(text *json, bool throw_error)
{
	JsonLexContext *lex;
	JsonParseErrorType result;

	lex = makeJsonLexContext(json, false);

	/* 从输入只词法分析一个记号并检查类型。 */
	result = json_lex(lex);

	if (result == JSON_SUCCESS)
		return lex->token_type;

	if (throw_error)
		json_errsave_error(result, lex, NULL);

	return JSON_TOKEN_INVALID;	/* 无效 json */
}
