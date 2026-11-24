/*-------------------------------------------------------------------------
 *
 * jsonb.c
 *		jsonb 类型的 I/O 例程
 *
 * Copyright (c) 2014-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonb.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/transam.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/jsonfuncs.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

typedef struct JsonbInState
{
	JsonbParseState *parseState;
	JsonbValue *res;
	Node	   *escontext;
} JsonbInState;

/* 与 json 类别不同,json 与 jsonb 须区别对待 */
typedef enum					/* datum_to_jsonb 的类型类别 */
{
	JSONBTYPE_NULL,				/* 为 NULL,故未费心识别 */
	JSONBTYPE_BOOL,				/* 布尔(仅内建类型) */
	JSONBTYPE_NUMERIC,			/* 数值(同) */
	JSONBTYPE_DATE,				/* 对日期时间类型用特殊格式化 */
	JSONBTYPE_TIMESTAMP,		/* 对 timestamp 用特殊格式化 */
	JSONBTYPE_TIMESTAMPTZ,		/* 及 timestamptz */
	JSONBTYPE_JSON,				/* JSON */
	JSONBTYPE_JSONB,			/* JSONB */
	JSONBTYPE_ARRAY,			/*数组*/
	JSONBTYPE_COMPOSITE,		/* 复合 */
	JSONBTYPE_JSONCAST,			/* 具有到 JSON 的显式转换 */
	JSONBTYPE_OTHER				/* 其余 */
} JsonbTypeCategory;

typedef struct JsonbAggState
{
	JsonbInState *res;
	JsonbTypeCategory key_category;
	Oid			key_output_func;
	JsonbTypeCategory val_category;
	Oid			val_output_func;
} JsonbAggState;

static inline Datum jsonb_from_cstring(char *json, int len, Node *escontext);
static bool checkStringLen(size_t len, Node *escontext);
static JsonParseErrorType jsonb_in_object_start(void *pstate);
static JsonParseErrorType jsonb_in_object_end(void *pstate);
static JsonParseErrorType jsonb_in_array_start(void *pstate);
static JsonParseErrorType jsonb_in_array_end(void *pstate);
static JsonParseErrorType jsonb_in_object_field_start(void *pstate, char *fname, bool isnull);
static void jsonb_put_escaped_value(StringInfo out, JsonbValue *scalarVal);
static JsonParseErrorType jsonb_in_scalar(void *pstate, char *token, JsonTokenType tokentype);
static void jsonb_categorize_type(Oid typoid,
								  JsonbTypeCategory *tcategory,
								  Oid *outfuncoid);
static void composite_to_jsonb(Datum composite, JsonbInState *result);
static void array_dim_to_jsonb(JsonbInState *result, int dim, int ndims, int *dims,
							   Datum *vals, bool *nulls, int *valcount,
							   JsonbTypeCategory tcategory, Oid outfuncoid);
static void array_to_jsonb_internal(Datum array, JsonbInState *result);
static void jsonb_categorize_type(Oid typoid,
								  JsonbTypeCategory *tcategory,
								  Oid *outfuncoid);
static void datum_to_jsonb(Datum val, bool is_null, JsonbInState *result,
						   JsonbTypeCategory tcategory, Oid outfuncoid,
						   bool key_scalar);
static void add_jsonb(Datum val, bool is_null, JsonbInState *result,
					  Oid val_type, bool key_scalar);
static JsonbParseState *clone_parse_state(JsonbParseState *state);
static char *JsonbToCStringWorker(StringInfo out, JsonbContainer *in, int estimated_len, bool indent);
static void add_indent(StringInfo out, bool indent, int level);

/*
 * jsonb 类型输入函数
 */
Datum
jsonb_in(PG_FUNCTION_ARGS)
{
	char	   *json = PG_GETARG_CSTRING(0);

	return jsonb_from_cstring(json, strlen(json), fcinfo->context);
}

/*
 * jsonb 类型 recv 函数
 *
 * 二进制模式下该类型以文本发送,故几乎等同
 * 于输入函数,但前缀有版本号,以便日后
 * 必要时可改二进制格式。目前
 * 仅支持版本 1。
 */
Datum
jsonb_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int			version = pq_getmsgint(buf, 1);
	char	   *str;
	int			nbytes;

	if (version == 1)
		str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	else
		elog(ERROR, "unsupported jsonb version number %d", version);

	return jsonb_from_cstring(str, nbytes, NULL);
}

/*
 * jsonb 类型输出函数
 */
Datum
jsonb_out(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	char	   *out;

	out = JsonbToCString(NULL, &jb->root, VARSIZE(jb));

	PG_RETURN_CSTRING(out);
}

/*
 * jsonb 类型 send 函数
 *
 * 先发版本号,再发 jsonb 的文本串
 */
Datum
jsonb_send(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	StringInfoData buf;
	StringInfo	jtext = makeStringInfo();
	int			version = 1;

	(void) JsonbToCString(jtext, &jb->root, VARSIZE(jb));

	pq_begintypsend(&buf);
	pq_sendint8(&buf, version);
	pq_sendtext(&buf, jtext->data, jtext->len);
	pfree(jtext->data);
	pfree(jtext);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * 取 jsonb 容器的类型名。
 */
static const char *
JsonbContainerTypeName(JsonbContainer *jbc)
{
	JsonbValue	scalar;

	if (JsonbExtractScalar(jbc, &scalar))
		return JsonbTypeName(&scalar);
	else if (JsonContainerIsArray(jbc))
		return "array";
	else if (JsonContainerIsObject(jbc))
		return "object";
	else
	{
		elog(ERROR, "invalid jsonb container type: 0x%08x", jbc->header);
		return "unknown";
	}
}

/*
 * 取 jsonb 值的类型名。
 */
const char *
JsonbTypeName(JsonbValue *val)
{
	switch (val->type)
	{
		case jbvBinary:
			return JsonbContainerTypeName(val->val.binary.data);
		case jbvObject:
			return "object";
		case jbvArray:
			return "array";
		case jbvNumeric:
			return "number";
		case jbvString:
			return "string";
		case jbvBool:
			return "boolean";
		case jbvNull:
			return "null";
		case jbvDatetime:
			switch (val->val.datetime.typid)
			{
				case DATEOID:
					return "date";
				case TIMEOID:
					return "time without time zone";
				case TIMETZOID:
					return "time with time zone";
				case TIMESTAMPOID:
					return "timestamp without time zone";
				case TIMESTAMPTZOID:
					return "timestamp with time zone";
				default:
					elog(ERROR, "unrecognized jsonb value datetime type: %d",
						 val->val.datetime.typid);
			}
			return "unknown";
		default:
			elog(ERROR, "unrecognized jsonb value type: %d", val->type);
			return "unknown";
	}
}

/*
 * SQL 函数 jsonb_typeof(jsonb) -> text
 *
 * 本函数放此是因对应的 json 函数在 json.c,因其
 * 使用未在别处暴露的 json 解析器内部。
 */
Datum
jsonb_typeof(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	const char *result = JsonbContainerTypeName(&in->root);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * jsonb_from_cstring
 *
 * 把 json 串转为 jsonb Datum。
 *
 * 用带钩子的 json 解析器构造 jsonb。
 *
 * 若 escontext 指向 ErrorSaveContext,则错误报到该处
 * 而不抛出。
 */
static inline Datum
jsonb_from_cstring(char *json, int len, Node *escontext)
{
	JsonLexContext *lex;
	JsonbInState state;
	JsonSemAction sem;

	memset(&state, 0, sizeof(state));
	memset(&sem, 0, sizeof(sem));
	lex = makeJsonLexContextCstringLen(json, len, GetDatabaseEncoding(), true);

	state.escontext = escontext;
	sem.semstate = (void *) &state;

	sem.object_start = jsonb_in_object_start;
	sem.array_start = jsonb_in_array_start;
	sem.object_end = jsonb_in_object_end;
	sem.array_end = jsonb_in_array_end;
	sem.scalar = jsonb_in_scalar;
	sem.object_field_start = jsonb_in_object_field_start;

	if (!pg_parse_json_or_errsave(lex, &sem, escontext))
		return (Datum) 0;

	/* 解析后,item 成员已持有组成好的 jsonb 结构 */
	PG_RETURN_POINTER(JsonbValueToJsonb(state.res));
}

static bool
checkStringLen(size_t len, Node *escontext)
{
	if (len > JENTRY_OFFLENMASK)
		ereturn(escontext, false,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("string too long to represent as jsonb string"),
				 errdetail("Due to an implementation restriction, jsonb strings cannot exceed %d bytes.",
						   JENTRY_OFFLENMASK)));

	return true;
}

static JsonParseErrorType
jsonb_in_object_start(void *pstate)
{
	JsonbInState *_state = (JsonbInState *) pstate;

	_state->res = pushJsonbValue(&_state->parseState, WJB_BEGIN_OBJECT, NULL);

	return JSON_SUCCESS;
}

static JsonParseErrorType
jsonb_in_object_end(void *pstate)
{
	JsonbInState *_state = (JsonbInState *) pstate;

	_state->res = pushJsonbValue(&_state->parseState, WJB_END_OBJECT, NULL);

	return JSON_SUCCESS;
}

static JsonParseErrorType
jsonb_in_array_start(void *pstate)
{
	JsonbInState *_state = (JsonbInState *) pstate;

	_state->res = pushJsonbValue(&_state->parseState, WJB_BEGIN_ARRAY, NULL);

	return JSON_SUCCESS;
}

static JsonParseErrorType
jsonb_in_array_end(void *pstate)
{
	JsonbInState *_state = (JsonbInState *) pstate;

	_state->res = pushJsonbValue(&_state->parseState, WJB_END_ARRAY, NULL);

	return JSON_SUCCESS;
}

static JsonParseErrorType
jsonb_in_object_field_start(void *pstate, char *fname, bool isnull)
{
	JsonbInState *_state = (JsonbInState *) pstate;
	JsonbValue	v;

	Assert(fname != NULL);
	v.type = jbvString;
	v.val.string.len = strlen(fname);
	if (!checkStringLen(v.val.string.len, _state->escontext))
		return JSON_SEM_ACTION_FAILED;
	v.val.string.val = fname;

	_state->res = pushJsonbValue(&_state->parseState, WJB_KEY, &v);

	return JSON_SUCCESS;
}

static void
jsonb_put_escaped_value(StringInfo out, JsonbValue *scalarVal)
{
	switch (scalarVal->type)
	{
		case jbvNull:
			appendBinaryStringInfo(out, "null", 4);
			break;
		case jbvString:
			escape_json(out, pnstrdup(scalarVal->val.string.val, scalarVal->val.string.len));
			break;
		case jbvNumeric:
			appendStringInfoString(out,
								   DatumGetCString(DirectFunctionCall1(numeric_out,
																	   PointerGetDatum(scalarVal->val.numeric))));
			break;
		case jbvBool:
			if (scalarVal->val.boolean)
				appendBinaryStringInfo(out, "true", 4);
			else
				appendBinaryStringInfo(out, "false", 5);
			break;
		default:
			elog(ERROR, "unknown jsonb scalar type");
	}
}

/*
 * 对 jsonb 我们总要去转义后的值——即词元中所存
 */
static JsonParseErrorType
jsonb_in_scalar(void *pstate, char *token, JsonTokenType tokentype)
{
	JsonbInState *_state = (JsonbInState *) pstate;
	JsonbValue	v;
	Datum		numd;

	switch (tokentype)
	{

		case JSON_TOKEN_STRING:
			Assert(token != NULL);
			v.type = jbvString;
			v.val.string.len = strlen(token);
			if (!checkStringLen(v.val.string.len, _state->escontext))
				return JSON_SEM_ACTION_FAILED;
			v.val.string.val = token;
			break;
		case JSON_TOKEN_NUMBER:

			/*
			 * 无须检查数值大小,因最大
			 * 数值尺寸远低于 JsonbValue 限制
			 */
			Assert(token != NULL);
			v.type = jbvNumeric;
			if (!DirectInputFunctionCallSafe(numeric_in, token,
											 InvalidOid, -1,
											 _state->escontext,
											 &numd))
				return JSON_SEM_ACTION_FAILED;
			v.val.numeric = DatumGetNumeric(numd);
			break;
		case JSON_TOKEN_TRUE:
			v.type = jbvBool;
			v.val.boolean = true;
			break;
		case JSON_TOKEN_FALSE:
			v.type = jbvBool;
			v.val.boolean = false;
			break;
		case JSON_TOKEN_NULL:
			v.type = jbvNull;
			break;
		default:
			/* 不应发生 */
			elog(ERROR, "invalid json token type");
			break;
	}

	if (_state->parseState == NULL)
	{
		/* 单个标量 */
		JsonbValue	va;

		va.type = jbvArray;
		va.val.array.rawScalar = true;
		va.val.array.nElems = 1;

		_state->res = pushJsonbValue(&_state->parseState, WJB_BEGIN_ARRAY, &va);
		_state->res = pushJsonbValue(&_state->parseState, WJB_ELEM, &v);
		_state->res = pushJsonbValue(&_state->parseState, WJB_END_ARRAY, NULL);
	}
	else
	{
		JsonbValue *o = &_state->parseState->contVal;

		switch (o->type)
		{
			case jbvArray:
				_state->res = pushJsonbValue(&_state->parseState, WJB_ELEM, &v);
				break;
			case jbvObject:
				_state->res = pushJsonbValue(&_state->parseState, WJB_VALUE, &v);
				break;
			default:
				elog(ERROR, "unexpected parent of nested structure");
		}
	}

	return JSON_SUCCESS;
}

/*
 * JsonbToCString
 *	   把 jsonb 值转为 C 字符串。
 *
 * 若 'out' 非空,结果 C 字符串存入
 * StringBuffer。结果串总会返回。
 *
 * 传入 StringInfo 而非 NULL 的典型情形是调用方
 * 想直接读 len 而不调 strlen,例如
 * 要把它转成 text* 对象时。
 */
char *
JsonbToCString(StringInfo out, JsonbContainer *in, int estimated_len)
{
	return JsonbToCStringWorker(out, in, estimated_len, false);
}

/*
 * 同上,但开启缩进
 */
char *
JsonbToCStringIndent(StringInfo out, JsonbContainer *in, int estimated_len)
{
	return JsonbToCStringWorker(out, in, estimated_len, true);
}

/*
 * 上述两函数的公共工作函数
 */
static char *
JsonbToCStringWorker(StringInfo out, JsonbContainer *in, int estimated_len, bool indent)
{
	bool		first = true;
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken type = WJB_DONE;
	int			level = 0;
	bool		redo_switch = false;

	/* 缩进时,逗号后不加空格 */
	int			ispaces = indent ? 1 : 2;

	/*
	 * 首项不缩进。循环末尾会把此标志设为
	 * indent 标志。
	 */
	bool		use_indent = false;
	bool		raw_scalar = false;
	bool		last_was_key = false;

	if (out == NULL)
		out = makeStringInfo();

	enlargeStringInfo(out, (estimated_len >= 0) ? estimated_len : 64);

	it = JsonbIteratorInit(in);

	while (redo_switch ||
		   ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE))
	{
		redo_switch = false;
		switch (type)
		{
			case WJB_BEGIN_ARRAY:
				if (!first)
					appendBinaryStringInfo(out, ", ", ispaces);

				if (!v.val.array.rawScalar)
				{
					add_indent(out, use_indent && !last_was_key, level);
					appendStringInfoCharMacro(out, '[');
				}
				else
					raw_scalar = true;

				first = true;
				level++;
				break;
			case WJB_BEGIN_OBJECT:
				if (!first)
					appendBinaryStringInfo(out, ", ", ispaces);

				add_indent(out, use_indent && !last_was_key, level);
				appendStringInfoCharMacro(out, '{');

				first = true;
				level++;
				break;
			case WJB_KEY:
				if (!first)
					appendBinaryStringInfo(out, ", ", ispaces);
				first = true;

				add_indent(out, use_indent, level);

				/* json 规则保证这是字符串 */
				jsonb_put_escaped_value(out, &v);
				appendBinaryStringInfo(out, ": ", 2);

				type = JsonbIteratorNext(&it, &v, false);
				if (type == WJB_VALUE)
				{
					first = false;
					jsonb_put_escaped_value(out, &v);
				}
				else
				{
					Assert(type == WJB_BEGIN_OBJECT || type == WJB_BEGIN_ARRAY);

					/*
					 * 须重跑当前 switch(),因须先
					 * 输出刚从迭代器取得的对象
					 * 再调用迭代器。
					 */
					redo_switch = true;
				}
				break;
			case WJB_ELEM:
				if (!first)
					appendBinaryStringInfo(out, ", ", ispaces);
				first = false;

				if (!raw_scalar)
					add_indent(out, use_indent, level);
				jsonb_put_escaped_value(out, &v);
				break;
			case WJB_END_ARRAY:
				level--;
				if (!raw_scalar)
				{
					add_indent(out, use_indent, level);
					appendStringInfoCharMacro(out, ']');
				}
				first = false;
				break;
			case WJB_END_OBJECT:
				level--;
				add_indent(out, use_indent, level);
				appendStringInfoCharMacro(out, '}');
				first = false;
				break;
			default:
				elog(ERROR, "unknown jsonb iterator token type");
		}
		use_indent = indent;
		last_was_key = redo_switch;
	}

	Assert(level == 0);

	return out->data;
}

static void
add_indent(StringInfo out, bool indent, int level)
{
	if (indent)
	{
		appendStringInfoCharMacro(out, '\n');
		appendStringInfoSpaces(out, level * 4);
	}
}


/*
 * 确定在 datum_to_jsonb 中如何呈现给定类型的值。
 *
 * 给定数据类型 OID,返回其 JsonbTypeCategory,以及该类型的
 * 输出函数 OID。若返回类别为 JSONBTYPE_JSONCAST,
 * 则改为返回相应转换函数的 OID。
 */
static void
jsonb_categorize_type(Oid typoid,
					  JsonbTypeCategory *tcategory,
					  Oid *outfuncoid)
{
	bool		typisvarlena;

	/* 穿透任何域 */
	typoid = getBaseType(typoid);

	*outfuncoid = InvalidOid;

	/*
	 * 除了 date/timestamp 类型、数组与复合类型、
	 * 布尔、json 与 jsonb，以及存在到 json 转换的
	 * 非内建类型外，都需取输出函数。最后一种情形
	 * 则返回转换函数的 oid。
	 */

	switch (typoid)
	{
		case BOOLOID:
			*tcategory = JSONBTYPE_BOOL;
			break;

		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
			*tcategory = JSONBTYPE_NUMERIC;
			break;

		case DATEOID:
			*tcategory = JSONBTYPE_DATE;
			break;

		case TIMESTAMPOID:
			*tcategory = JSONBTYPE_TIMESTAMP;
			break;

		case TIMESTAMPTZOID:
			*tcategory = JSONBTYPE_TIMESTAMPTZ;
			break;

		case JSONBOID:
			*tcategory = JSONBTYPE_JSONB;
			break;

		case JSONOID:
			*tcategory = JSONBTYPE_JSON;
			break;

		default:
			/* 检查数组和复合类型 */
			if (OidIsValid(get_element_type(typoid)) || typoid == ANYARRAYOID
				|| typoid == ANYCOMPATIBLEARRAYOID || typoid == RECORDARRAYOID)
				*tcategory = JSONBTYPE_ARRAY;
			else if (type_is_rowtype(typoid))	/* 含 RECORDOID */
				*tcategory = JSONBTYPE_COMPOSITE;
			else
			{
				/* 多半是一般情形 ... */
				*tcategory = JSONBTYPE_OTHER;

				/*
				 * 但非内建时先查到 json 的转换(注意
				 * 不是到 jsonb)。
				 */
				if (typoid >= FirstNormalObjectId)
				{
					Oid			castfunc;
					CoercionPathType ctype;

					ctype = find_coercion_pathway(JSONOID, typoid,
												  COERCION_EXPLICIT, &castfunc);
					if (ctype == COERCION_PATH_FUNC && OidIsValid(castfunc))
					{
						*tcategory = JSONBTYPE_JSONCAST;
						*outfuncoid = castfunc;
					}
					else
					{
						/* 非转换类型,故取通常输出函数 */
						getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
					}
				}
				else
				{
					/* 其他内建类型 */
					getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
				}
				break;
			}
	}
}

/*
 * 把 Datum 转为 jsonb,并加入结果 JsonbInState。
 *
 * tcategory 与 outfuncoid 来自此前一次 json_categorize_type 调用，
 * 但若 is_null 为真则二者可以为无效值。
 *
 * 若 key_scalar 为真,则值存为键,故须
 * 类型可接受,并强制为 jbvString。
 *
 * 注意:目前假定 result->escontext 为 NULL,错误
 * 将被抛出。
 */
static void
datum_to_jsonb(Datum val, bool is_null, JsonbInState *result,
			   JsonbTypeCategory tcategory, Oid outfuncoid,
			   bool key_scalar)
{
	char	   *outputstr;
	bool		numeric_error;
	JsonbValue	jb;
	bool		scalar_jsonb = false;

	check_stack_depth();

	/*多数情形下把 val 转为 jb 中的 JsonbValue*/
	if (is_null)
	{
		Assert(!key_scalar);
		jb.type = jbvNull;
	}
	else if (key_scalar &&
			 (tcategory == JSONBTYPE_ARRAY ||
			  tcategory == JSONBTYPE_COMPOSITE ||
			  tcategory == JSONBTYPE_JSON ||
			  tcategory == JSONBTYPE_JSONB ||
			  tcategory == JSONBTYPE_JSONCAST))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("key value must be scalar, not array, composite, or json")));
	}
	else
	{
		if (tcategory == JSONBTYPE_JSONCAST)
			val = OidFunctionCall1(outfuncoid, val);

		switch (tcategory)
		{
			case JSONBTYPE_ARRAY:
				array_to_jsonb_internal(val, result);
				break;
			case JSONBTYPE_COMPOSITE:
				composite_to_jsonb(val, result);
				break;
			case JSONBTYPE_BOOL:
				if (key_scalar)
				{
					outputstr = DatumGetBool(val) ? "true" : "false";
					jb.type = jbvString;
					jb.val.string.len = strlen(outputstr);
					jb.val.string.val = outputstr;
				}
				else
				{
					jb.type = jbvBool;
					jb.val.boolean = DatumGetBool(val);
				}
				break;
			case JSONBTYPE_NUMERIC:
				outputstr = OidOutputFunctionCall(outfuncoid, val);
				if (key_scalar)
				{
					/*键总是加引号*/
					jb.type = jbvString;
					jb.val.string.len = strlen(outputstr);
					jb.val.string.val = outputstr;
				}
				else
				{
					/*
					 * 若为合法 JSON 数则作 numeric,否则
					 * 作字符串。无效数值输出总会含 'N'
					 * 或 'n'(我想)。
					 */
					numeric_error = (strchr(outputstr, 'N') != NULL ||
									 strchr(outputstr, 'n') != NULL);
					if (!numeric_error)
					{
						Datum		numd;

						jb.type = jbvNumeric;
						numd = DirectFunctionCall3(numeric_in,
												   CStringGetDatum(outputstr),
												   ObjectIdGetDatum(InvalidOid),
												   Int32GetDatum(-1));
						jb.val.numeric = DatumGetNumeric(numd);
						pfree(outputstr);
					}
					else
					{
						jb.type = jbvString;
						jb.val.string.len = strlen(outputstr);
						jb.val.string.val = outputstr;
					}
				}
				break;
			case JSONBTYPE_DATE:
				jb.type = jbvString;
				jb.val.string.val = JsonEncodeDateTime(NULL, val,
													   DATEOID, NULL);
				jb.val.string.len = strlen(jb.val.string.val);
				break;
			case JSONBTYPE_TIMESTAMP:
				jb.type = jbvString;
				jb.val.string.val = JsonEncodeDateTime(NULL, val,
													   TIMESTAMPOID, NULL);
				jb.val.string.len = strlen(jb.val.string.val);
				break;
			case JSONBTYPE_TIMESTAMPTZ:
				jb.type = jbvString;
				jb.val.string.val = JsonEncodeDateTime(NULL, val,
													   TIMESTAMPTZOID, NULL);
				jb.val.string.len = strlen(jb.val.string.val);
				break;
			case JSONBTYPE_JSONCAST:
			case JSONBTYPE_JSON:
				{
					/* 把 json 直接解析进已有 result 对象 */
					JsonLexContext *lex;
					JsonSemAction sem;
					text	   *json = DatumGetTextPP(val);

					lex = makeJsonLexContext(json, true);

					memset(&sem, 0, sizeof(sem));

					sem.semstate = (void *) result;

					sem.object_start = jsonb_in_object_start;
					sem.array_start = jsonb_in_array_start;
					sem.object_end = jsonb_in_object_end;
					sem.array_end = jsonb_in_array_end;
					sem.scalar = jsonb_in_scalar;
					sem.object_field_start = jsonb_in_object_field_start;

					pg_parse_json_or_ereport(lex, &sem);
				}
				break;
			case JSONBTYPE_JSONB:
				{
					Jsonb	   *jsonb = DatumGetJsonbP(val);
					JsonbIterator *it;

					it = JsonbIteratorInit(&jsonb->root);

					if (JB_ROOT_IS_SCALAR(jsonb))
					{
						(void) JsonbIteratorNext(&it, &jb, true);
						Assert(jb.type == jbvArray);
						(void) JsonbIteratorNext(&it, &jb, true);
						scalar_jsonb = true;
					}
					else
					{
						JsonbIteratorToken type;

						while ((type = JsonbIteratorNext(&it, &jb, false))
							   != WJB_DONE)
						{
							if (type == WJB_END_ARRAY || type == WJB_END_OBJECT ||
								type == WJB_BEGIN_ARRAY || type == WJB_BEGIN_OBJECT)
								result->res = pushJsonbValue(&result->parseState,
															 type, NULL);
							else
								result->res = pushJsonbValue(&result->parseState,
															 type, &jb);
						}
					}
				}
				break;
			default:
				outputstr = OidOutputFunctionCall(outfuncoid, val);
				jb.type = jbvString;
				jb.val.string.len = strlen(outputstr);
				(void) checkStringLen(jb.val.string.len, NULL);
				jb.val.string.val = outputstr;
				break;
		}
	}

	/* 现把 jb 插入 result,除非已递归完成 */
	if (!is_null && !scalar_jsonb &&
		tcategory >= JSONBTYPE_JSON && tcategory <= JSONBTYPE_JSONCAST)
	{
		/* 工作已递归完成 */
		return;
	}
	else if (result->parseState == NULL)
	{
		/* 单个根标量 */
		JsonbValue	va;

		va.type = jbvArray;
		va.val.array.rawScalar = true;
		va.val.array.nElems = 1;

		result->res = pushJsonbValue(&result->parseState, WJB_BEGIN_ARRAY, &va);
		result->res = pushJsonbValue(&result->parseState, WJB_ELEM, &jb);
		result->res = pushJsonbValue(&result->parseState, WJB_END_ARRAY, NULL);
	}
	else
	{
		JsonbValue *o = &result->parseState->contVal;

		switch (o->type)
		{
			case jbvArray:
				result->res = pushJsonbValue(&result->parseState, WJB_ELEM, &jb);
				break;
			case jbvObject:
				result->res = pushJsonbValue(&result->parseState,
											 key_scalar ? WJB_KEY : WJB_VALUE,
											 &jb);
				break;
			default:
				elog(ERROR, "unexpected parent of nested structure");
		}
	}
}

/*
 * 处理数组的一个维度。
 * 若是最内层维度则输出各值，否则递归调用自身
 * 去处理下一个维度。
 */
static void
array_dim_to_jsonb(JsonbInState *result, int dim, int ndims, int *dims, Datum *vals,
				   bool *nulls, int *valcount, JsonbTypeCategory tcategory,
				   Oid outfuncoid)
{
	int			i;

	Assert(dim < ndims);

	result->res = pushJsonbValue(&result->parseState, WJB_BEGIN_ARRAY, NULL);

	for (i = 1; i <= dims[dim]; i++)
	{
		if (dim + 1 == ndims)
		{
			datum_to_jsonb(vals[*valcount], nulls[*valcount], result, tcategory,
						   outfuncoid, false);
			(*valcount)++;
		}
		else
		{
			array_dim_to_jsonb(result, dim + 1, ndims, dims, vals, nulls,
							   valcount, tcategory, outfuncoid);
		}
	}

	result->res = pushJsonbValue(&result->parseState, WJB_END_ARRAY, NULL);
}

/*
 * 把数组转成 JSON。
 */
static void
array_to_jsonb_internal(Datum array, JsonbInState *result)
{
	ArrayType  *v = DatumGetArrayTypeP(array);
	Oid			element_type = ARR_ELEMTYPE(v);
	int		   *dim;
	int			ndim;
	int			nitems;
	int			count = 0;
	Datum	   *elements;
	bool	   *nulls;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	JsonbTypeCategory tcategory;
	Oid			outfuncoid;

	ndim = ARR_NDIM(v);
	dim = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndim, dim);

	if (nitems <= 0)
	{
		result->res = pushJsonbValue(&result->parseState, WJB_BEGIN_ARRAY, NULL);
		result->res = pushJsonbValue(&result->parseState, WJB_END_ARRAY, NULL);
		return;
	}

	get_typlenbyvalalign(element_type,
						 &typlen, &typbyval, &typalign);

	jsonb_categorize_type(element_type,
						  &tcategory, &outfuncoid);

	deconstruct_array(v, element_type, typlen, typbyval,
					  typalign, &elements, &nulls,
					  &nitems);

	array_dim_to_jsonb(result, 0, ndim, dim, elements, nulls, &count, tcategory,
					   outfuncoid);

	pfree(elements);
	pfree(nulls);
}

/*
 * 把复合类型/记录转成 JSON。
 */
static void
composite_to_jsonb(Datum composite, JsonbInState *result)
{
	HeapTupleHeader td;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tmptup,
			   *tuple;
	int			i;

	td = DatumGetHeapTupleHeader(composite);

	/* 提取行类型信息并找到 tupdesc */
	tupType = HeapTupleHeaderGetTypeId(td);
	tupTypmod = HeapTupleHeaderGetTypMod(td);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	/* 构建临时 HeapTuple 控制结构 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	tmptup.t_data = td;
	tuple = &tmptup;

	result->res = pushJsonbValue(&result->parseState, WJB_BEGIN_OBJECT, NULL);

	for (i = 0; i < tupdesc->natts; i++)
	{
		Datum		val;
		bool		isnull;
		char	   *attname;
		JsonbTypeCategory tcategory;
		Oid			outfuncoid;
		JsonbValue	v;
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;

		attname = NameStr(att->attname);

		v.type = jbvString;
		/* 此处无须 checkStringLen——不会超过最大名字长度 */
		v.val.string.len = strlen(attname);
		v.val.string.val = attname;

		result->res = pushJsonbValue(&result->parseState, WJB_KEY, &v);

		val = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		if (isnull)
		{
			tcategory = JSONBTYPE_NULL;
			outfuncoid = InvalidOid;
		}
		else
			jsonb_categorize_type(att->atttypid, &tcategory, &outfuncoid);

		datum_to_jsonb(val, isnull, result, tcategory, outfuncoid, false);
	}

	result->res = pushJsonbValue(&result->parseState, WJB_END_OBJECT, NULL);
	ReleaseTupleDesc(tupdesc);
}

/*
 * 向结果追加 "val" 的 JSON 文本
 *
 * 这只是 datum_to_jsonb 的薄封装。若同一类型将
 * 打印多次,勿用本函数;宜只做一次 jsonb_categorize_type
 * 的查找。
 */

static void
add_jsonb(Datum val, bool is_null, JsonbInState *result,
		  Oid val_type, bool key_scalar)
{
	JsonbTypeCategory tcategory;
	Oid			outfuncoid;

	if (val_type == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine input data type")));

	if (is_null)
	{
		tcategory = JSONBTYPE_NULL;
		outfuncoid = InvalidOid;
	}
	else
		jsonb_categorize_type(val_type,
							  &tcategory, &outfuncoid);

	datum_to_jsonb(val, is_null, result, tcategory, outfuncoid, key_scalar);
}

/*
 * 给定类型脱离 JSONB 上下文后还算不可变吗?
 *
 * 目前所有日期时间类型都被视为可变，因为
 * 依赖时区。XXX 还应深入对象与
 * 数组,但未做。
 */
bool
to_jsonb_is_immutable(Oid typoid)
{
	JsonbTypeCategory tcategory;
	Oid			outfuncoid;

	jsonb_categorize_type(typoid, &tcategory, &outfuncoid);

	switch (tcategory)
	{
		case JSONBTYPE_NULL:
		case JSONBTYPE_BOOL:
		case JSONBTYPE_JSON:
		case JSONBTYPE_JSONB:
			return true;

		case JSONBTYPE_DATE:
		case JSONBTYPE_TIMESTAMP:
		case JSONBTYPE_TIMESTAMPTZ:
			return false;

		case JSONBTYPE_ARRAY:
			return false;		/* TODO 递归处理元素 */

		case JSONBTYPE_COMPOSITE:
			return false;		/* TODO 递归处理字段 */

		case JSONBTYPE_NUMERIC:
		case JSONBTYPE_JSONCAST:
		case JSONBTYPE_OTHER:
			return func_volatile(outfuncoid) == PROVOLATILE_IMMUTABLE;
	}

	return false;				/* 不可达 */
}

/*
 * SQL 函数 to_jsonb(anyvalue)
 */
Datum
to_jsonb(PG_FUNCTION_ARGS)
{
	Datum		val = PG_GETARG_DATUM(0);
	Oid			val_type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	JsonbInState result;
	JsonbTypeCategory tcategory;
	Oid			outfuncoid;

	if (val_type == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine input data type")));

	jsonb_categorize_type(val_type,
						  &tcategory, &outfuncoid);

	memset(&result, 0, sizeof(JsonbInState));

	datum_to_jsonb(val, false, &result, tcategory, outfuncoid, false);

	PG_RETURN_POINTER(JsonbValueToJsonb(result.res));
}

Datum
jsonb_build_object_worker(int nargs, Datum *args, bool *nulls, Oid *types,
						  bool absent_on_null, bool unique_keys)
{
	int			i;
	JsonbInState result;

	if (nargs % 2 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument list must have even number of elements"),
		/* translator: %s is a SQL function name */
				 errhint("The arguments of %s must consist of alternating keys and values.",
						 "jsonb_build_object()")));

	memset(&result, 0, sizeof(JsonbInState));

	result.res = pushJsonbValue(&result.parseState, WJB_BEGIN_OBJECT, NULL);
	result.parseState->unique_keys = unique_keys;
	result.parseState->skip_nulls = absent_on_null;

	for (i = 0; i < nargs; i += 2)
	{
		/* 处理键 */
		bool		skip;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("argument %d: key must not be null", i + 1)));

		/* absent_on_null 时跳过 NULL 值 */
		skip = absent_on_null && nulls[i + 1];

		/* 须保存被跳过的键以便键唯一性检查 */
		if (skip && !unique_keys)
			continue;

		add_jsonb(args[i], false, &result, types[i], true);

		/* 处理值 */
		add_jsonb(args[i + 1], nulls[i + 1], &result, types[i + 1], false);
	}

	result.res = pushJsonbValue(&result.parseState, WJB_END_OBJECT, NULL);

	return JsonbPGetDatum(JsonbValueToJsonb(result.res));
}

/*
 * SQL 函数 jsonb_build_object(variadic "any")
 */
Datum
jsonb_build_object(PG_FUNCTION_ARGS)
{
	Datum	   *args;
	bool	   *nulls;
	Oid		   *types;

	/* 构建组成该对象的参数值 */
	int			nargs = extract_variadic_args(fcinfo, 0, true,
											  &args, &types, &nulls);

	if (nargs < 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(jsonb_build_object_worker(nargs, args, nulls, types, false, false));
}

/*
 * jsonb_build_object 收到 0 个参数时的退化情形。
 */
Datum
jsonb_build_object_noargs(PG_FUNCTION_ARGS)
{
	JsonbInState result;

	memset(&result, 0, sizeof(JsonbInState));

	(void) pushJsonbValue(&result.parseState, WJB_BEGIN_OBJECT, NULL);
	result.res = pushJsonbValue(&result.parseState, WJB_END_OBJECT, NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(result.res));
}

Datum
jsonb_build_array_worker(int nargs, Datum *args, bool *nulls, Oid *types,
						 bool absent_on_null)
{
	int			i;
	JsonbInState result;

	memset(&result, 0, sizeof(JsonbInState));

	result.res = pushJsonbValue(&result.parseState, WJB_BEGIN_ARRAY, NULL);

	for (i = 0; i < nargs; i++)
	{
		if (absent_on_null && nulls[i])
			continue;

		add_jsonb(args[i], nulls[i], &result, types[i], false);
	}

	result.res = pushJsonbValue(&result.parseState, WJB_END_ARRAY, NULL);

	return JsonbPGetDatum(JsonbValueToJsonb(result.res));
}

/*
 * SQL 函数 jsonb_build_array(variadic "any")
 */
Datum
jsonb_build_array(PG_FUNCTION_ARGS)
{
	Datum	   *args;
	bool	   *nulls;
	Oid		   *types;

	/* 构建组成该对象的参数值 */
	int			nargs = extract_variadic_args(fcinfo, 0, true,
											  &args, &types, &nulls);

	if (nargs < 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(jsonb_build_array_worker(nargs, args, nulls, types, false));
}


/*
 * jsonb_build_array 收到 0 个参数时的退化情形。
 */
Datum
jsonb_build_array_noargs(PG_FUNCTION_ARGS)
{
	JsonbInState result;

	memset(&result, 0, sizeof(JsonbInState));

	(void) pushJsonbValue(&result.parseState, WJB_BEGIN_ARRAY, NULL);
	result.res = pushJsonbValue(&result.parseState, WJB_END_ARRAY, NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(result.res));
}


/*
 * SQL 函数 jsonb_object(text[])
 *
 * 取一维或二维 text 数组作为 jsonb 对象的
 * 键/值对。
 *
 */
Datum
jsonb_object(PG_FUNCTION_ARGS)
{
	ArrayType  *in_array = PG_GETARG_ARRAYTYPE_P(0);
	int			ndims = ARR_NDIM(in_array);
	Datum	   *in_datums;
	bool	   *in_nulls;
	int			in_count,
				count,
				i;
	JsonbInState result;

	memset(&result, 0, sizeof(JsonbInState));

	(void) pushJsonbValue(&result.parseState, WJB_BEGIN_OBJECT, NULL);

	switch (ndims)
	{
		case 0:
			goto close_object;
			break;

		case 1:
			if ((ARR_DIMS(in_array)[0]) % 2)
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array must have even number of elements")));
			break;

		case 2:
			if ((ARR_DIMS(in_array)[1]) != 2)
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array must have two columns")));
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong number of array subscripts")));
	}

	deconstruct_array_builtin(in_array, TEXTOID, &in_datums, &in_nulls, &in_count);

	count = in_count / 2;

	for (i = 0; i < count; ++i)
	{
		JsonbValue	v;
		char	   *str;
		int			len;

		if (in_nulls[i * 2])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for object key")));

		str = TextDatumGetCString(in_datums[i * 2]);
		len = strlen(str);

		v.type = jbvString;

		v.val.string.len = len;
		v.val.string.val = str;

		(void) pushJsonbValue(&result.parseState, WJB_KEY, &v);

		if (in_nulls[i * 2 + 1])
		{
			v.type = jbvNull;
		}
		else
		{
			str = TextDatumGetCString(in_datums[i * 2 + 1]);
			len = strlen(str);

			v.type = jbvString;

			v.val.string.len = len;
			v.val.string.val = str;
		}

		(void) pushJsonbValue(&result.parseState, WJB_VALUE, &v);
	}

	pfree(in_datums);
	pfree(in_nulls);

close_object:
	result.res = pushJsonbValue(&result.parseState, WJB_END_OBJECT, NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(result.res));
}

/*
 * SQL 函数 jsonb_object(text[], text[])
 *
 * 取分开的 text 键与值数组两两配对构造 jsonb
 * 对象。
 */
Datum
jsonb_object_two_arg(PG_FUNCTION_ARGS)
{
	ArrayType  *key_array = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *val_array = PG_GETARG_ARRAYTYPE_P(1);
	int			nkdims = ARR_NDIM(key_array);
	int			nvdims = ARR_NDIM(val_array);
	Datum	   *key_datums,
			   *val_datums;
	bool	   *key_nulls,
			   *val_nulls;
	int			key_count,
				val_count,
				i;
	JsonbInState result;

	memset(&result, 0, sizeof(JsonbInState));

	(void) pushJsonbValue(&result.parseState, WJB_BEGIN_OBJECT, NULL);

	if (nkdims > 1 || nkdims != nvdims)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	if (nkdims == 0)
		goto close_object;

	deconstruct_array_builtin(key_array, TEXTOID, &key_datums, &key_nulls, &key_count);
	deconstruct_array_builtin(val_array, TEXTOID, &val_datums, &val_nulls, &val_count);

	if (key_count != val_count)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("mismatched array dimensions")));

	for (i = 0; i < key_count; ++i)
	{
		JsonbValue	v;
		char	   *str;
		int			len;

		if (key_nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for object key")));

		str = TextDatumGetCString(key_datums[i]);
		len = strlen(str);

		v.type = jbvString;

		v.val.string.len = len;
		v.val.string.val = str;

		(void) pushJsonbValue(&result.parseState, WJB_KEY, &v);

		if (val_nulls[i])
		{
			v.type = jbvNull;
		}
		else
		{
			str = TextDatumGetCString(val_datums[i]);
			len = strlen(str);

			v.type = jbvString;

			v.val.string.len = len;
			v.val.string.val = str;
		}

		(void) pushJsonbValue(&result.parseState, WJB_VALUE, &v);
	}

	pfree(key_datums);
	pfree(key_nulls);
	pfree(val_datums);
	pfree(val_nulls);

close_object:
	result.res = pushJsonbValue(&result.parseState, WJB_END_OBJECT, NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(result.res));
}


/*
 * 解析状态的浅克隆,适于仅追加值而不
 * 修改它们的聚合最终函数
 * 使用。
 */
static JsonbParseState *
clone_parse_state(JsonbParseState *state)
{
	JsonbParseState *result,
			   *icursor,
			   *ocursor;

	if (state == NULL)
		return NULL;

	result = palloc(sizeof(JsonbParseState));
	icursor = state;
	ocursor = result;
	for (;;)
	{
		ocursor->contVal = icursor->contVal;
		ocursor->size = icursor->size;
		ocursor->unique_keys = icursor->unique_keys;
		ocursor->skip_nulls = icursor->skip_nulls;
		icursor = icursor->next;
		if (icursor == NULL)
			break;
		ocursor->next = palloc(sizeof(JsonbParseState));
		ocursor = ocursor->next;
	}
	ocursor->next = NULL;

	return result;
}

static Datum
jsonb_agg_transfn_worker(FunctionCallInfo fcinfo, bool absent_on_null)
{
	MemoryContext oldcontext,
				aggcontext;
	JsonbAggState *state;
	JsonbInState elem;
	Datum		val;
	JsonbInState *result;
	bool		single_scalar = false;
	JsonbIterator *it;
	Jsonb	   *jbelem;
	JsonbValue	v;
	JsonbIteratorToken type;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* 因参数为 internal 类型而无法直接调用 */
		elog(ERROR, "jsonb_agg_transfn called in non-aggregate context");
	}

	/* 首轮时建立累加器 */

	if (PG_ARGISNULL(0))
	{
		Oid			arg_type = get_fn_expr_argtype(fcinfo->flinfo, 1);

		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine input data type")));

		oldcontext = MemoryContextSwitchTo(aggcontext);
		state = palloc(sizeof(JsonbAggState));
		result = palloc0(sizeof(JsonbInState));
		state->res = result;
		result->res = pushJsonbValue(&result->parseState,
									 WJB_BEGIN_ARRAY, NULL);
		MemoryContextSwitchTo(oldcontext);

		jsonb_categorize_type(arg_type, &state->val_category,
							  &state->val_output_func);
	}
	else
	{
		state = (JsonbAggState *) PG_GETARG_POINTER(0);
		result = state->res;
	}

	if (absent_on_null && PG_ARGISNULL(1))
		PG_RETURN_POINTER(state);

	/* 在普通函数上下文中把参数转为 jsonb */

	val = PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1);

	memset(&elem, 0, sizeof(JsonbInState));

	datum_to_jsonb(val, PG_ARGISNULL(1), &elem, state->val_category,
				   state->val_output_func, false);

	jbelem = JsonbValueToJsonb(elem.res);

	/* 切到聚合上下文以做累积操作 */

	oldcontext = MemoryContextSwitchTo(aggcontext);

	it = JsonbIteratorInit(&jbelem->root);

	while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		switch (type)
		{
			case WJB_BEGIN_ARRAY:
				if (v.val.array.rawScalar)
					single_scalar = true;
				else
					result->res = pushJsonbValue(&result->parseState,
												 type, NULL);
				break;
			case WJB_END_ARRAY:
				if (!single_scalar)
					result->res = pushJsonbValue(&result->parseState,
												 type, NULL);
				break;
			case WJB_BEGIN_OBJECT:
			case WJB_END_OBJECT:
				result->res = pushJsonbValue(&result->parseState,
											 type, NULL);
				break;
			case WJB_ELEM:
			case WJB_KEY:
			case WJB_VALUE:
				if (v.type == jbvString)
				{
					/* 在聚合上下文中复制字符串值 */
					char	   *buf = palloc(v.val.string.len + 1);

					snprintf(buf, v.val.string.len + 1, "%s", v.val.string.val);
					v.val.string.val = buf;
				}
				else if (v.type == jbvNumeric)
				{
					/* 数值同理 */
					v.val.numeric =
						DatumGetNumeric(DirectFunctionCall1(numeric_uplus,
															NumericGetDatum(v.val.numeric)));
				}
				result->res = pushJsonbValue(&result->parseState,
											 type, &v);
				break;
			default:
				elog(ERROR, "unknown jsonb iterator token type");
		}
	}

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_POINTER(state);
}

/*
 * jsonb_agg 聚合函数
 */
Datum
jsonb_agg_transfn(PG_FUNCTION_ARGS)
{
	return jsonb_agg_transfn_worker(fcinfo, false);
}

/*
 * jsonb_agg_strict 聚合函数
 */
Datum
jsonb_agg_strict_transfn(PG_FUNCTION_ARGS)
{
	return jsonb_agg_transfn_worker(fcinfo, true);
}

Datum
jsonb_agg_finalfn(PG_FUNCTION_ARGS)
{
	JsonbAggState *arg;
	JsonbInState result;
	Jsonb	   *out;

	/* 因参数为 internal 类型而无法直接调用 */
	Assert(AggCheckCallContext(fcinfo, NULL));

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();		/* 无输入值时才返回 null */

	arg = (JsonbAggState *) PG_GETARG_POINTER(0);

	/*
	 * 须对参数做浅克隆,以防最终
	 * 函数被多次调用而改动参数。浅
	 * 克隆已够用,因我们不会改任何
	 * 值,只加最终数组结束标记。
	 */
	memset(&result, 0, sizeof(JsonbInState));

	result.parseState = clone_parse_state(arg->res->parseState);

	result.res = pushJsonbValue(&result.parseState,
								WJB_END_ARRAY, NULL);

	out = JsonbValueToJsonb(result.res);

	PG_RETURN_POINTER(out);
}

static Datum
jsonb_object_agg_transfn_worker(FunctionCallInfo fcinfo,
								bool absent_on_null, bool unique_keys)
{
	MemoryContext oldcontext,
				aggcontext;
	JsonbInState elem;
	JsonbAggState *state;
	Datum		val;
	JsonbInState *result;
	bool		single_scalar;
	JsonbIterator *it;
	Jsonb	   *jbkey,
			   *jbval;
	JsonbValue	v;
	JsonbIteratorToken type;
	bool		skip;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* 因参数为 internal 类型而无法直接调用 */
		elog(ERROR, "jsonb_object_agg_transfn called in non-aggregate context");
	}

	/* 首轮时建立累加器 */

	if (PG_ARGISNULL(0))
	{
		Oid			arg_type;

		oldcontext = MemoryContextSwitchTo(aggcontext);
		state = palloc(sizeof(JsonbAggState));
		result = palloc0(sizeof(JsonbInState));
		state->res = result;
		result->res = pushJsonbValue(&result->parseState,
									 WJB_BEGIN_OBJECT, NULL);
		result->parseState->unique_keys = unique_keys;
		result->parseState->skip_nulls = absent_on_null;

		MemoryContextSwitchTo(oldcontext);

		arg_type = get_fn_expr_argtype(fcinfo->flinfo, 1);

		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine input data type")));

		jsonb_categorize_type(arg_type, &state->key_category,
							  &state->key_output_func);

		arg_type = get_fn_expr_argtype(fcinfo->flinfo, 2);

		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine input data type")));

		jsonb_categorize_type(arg_type, &state->val_category,
							  &state->val_output_func);
	}
	else
	{
		state = (JsonbAggState *) PG_GETARG_POINTER(0);
		result = state->res;
	}

	/* 在普通函数上下文中把参数转为 jsonb */

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("field name must not be null")));

	/*
	 * absent_on_null 时跳过 NULL 值,除非需要
	 * 键唯一性检查(因须保存键)。
	 */
	skip = absent_on_null && PG_ARGISNULL(2);

	if (skip && !unique_keys)
		PG_RETURN_POINTER(state);

	val = PG_GETARG_DATUM(1);

	memset(&elem, 0, sizeof(JsonbInState));

	datum_to_jsonb(val, false, &elem, state->key_category,
				   state->key_output_func, true);

	jbkey = JsonbValueToJsonb(elem.res);

	val = PG_ARGISNULL(2) ? (Datum) 0 : PG_GETARG_DATUM(2);

	memset(&elem, 0, sizeof(JsonbInState));

	datum_to_jsonb(val, PG_ARGISNULL(2), &elem, state->val_category,
				   state->val_output_func, false);

	jbval = JsonbValueToJsonb(elem.res);

	it = JsonbIteratorInit(&jbkey->root);

	/* 切到聚合上下文以做累积操作 */

	oldcontext = MemoryContextSwitchTo(aggcontext);

	/*
	 * 键应为标量,上方调用 datum_to_jsonb 时
	 * 已检查,故此处只须再看这些
	 * 事项
	 */

	while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		switch (type)
		{
			case WJB_BEGIN_ARRAY:
				if (!v.val.array.rawScalar)
					elog(ERROR, "unexpected structure for key");
				break;
			case WJB_ELEM:
				if (v.type == jbvString)
				{
					/* 在聚合上下文中复制字符串值 */
					char	   *buf = palloc(v.val.string.len + 1);

					snprintf(buf, v.val.string.len + 1, "%s", v.val.string.val);
					v.val.string.val = buf;
				}
				else
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("object keys must be strings")));
				}
				result->res = pushJsonbValue(&result->parseState,
											 WJB_KEY, &v);

				if (skip)
				{
					v.type = jbvNull;
					result->res = pushJsonbValue(&result->parseState,
												 WJB_VALUE, &v);
					MemoryContextSwitchTo(oldcontext);
					PG_RETURN_POINTER(state);
				}

				break;
			case WJB_END_ARRAY:
				break;
			default:
				elog(ERROR, "unexpected structure for key");
				break;
		}
	}

	it = JsonbIteratorInit(&jbval->root);

	single_scalar = false;

	/*
	 * 值可为任意内容(含结构与 null),处理同
	 * json_agg_transfn,唯单标量总压为 WJB_VALUE
	 * 项。
	 */

	while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		switch (type)
		{
			case WJB_BEGIN_ARRAY:
				if (v.val.array.rawScalar)
					single_scalar = true;
				else
					result->res = pushJsonbValue(&result->parseState,
												 type, NULL);
				break;
			case WJB_END_ARRAY:
				if (!single_scalar)
					result->res = pushJsonbValue(&result->parseState,
												 type, NULL);
				break;
			case WJB_BEGIN_OBJECT:
			case WJB_END_OBJECT:
				result->res = pushJsonbValue(&result->parseState,
											 type, NULL);
				break;
			case WJB_ELEM:
			case WJB_KEY:
			case WJB_VALUE:
				if (v.type == jbvString)
				{
					/* 在聚合上下文中复制字符串值 */
					char	   *buf = palloc(v.val.string.len + 1);

					snprintf(buf, v.val.string.len + 1, "%s", v.val.string.val);
					v.val.string.val = buf;
				}
				else if (v.type == jbvNumeric)
				{
					/* 数值同理 */
					v.val.numeric =
						DatumGetNumeric(DirectFunctionCall1(numeric_uplus,
															NumericGetDatum(v.val.numeric)));
				}
				result->res = pushJsonbValue(&result->parseState,
											 single_scalar ? WJB_VALUE : type,
											 &v);
				break;
			default:
				elog(ERROR, "unknown jsonb iterator token type");
		}
	}

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_POINTER(state);
}

/*
 * jsonb_object_agg 聚合函数
 */
Datum
jsonb_object_agg_transfn(PG_FUNCTION_ARGS)
{
	return jsonb_object_agg_transfn_worker(fcinfo, false, false);
}


/*
 * jsonb_object_agg_strict 聚合函数
 */
Datum
jsonb_object_agg_strict_transfn(PG_FUNCTION_ARGS)
{
	return jsonb_object_agg_transfn_worker(fcinfo, true, false);
}

/*
 * jsonb_object_agg_unique 聚合函数
 */
Datum
jsonb_object_agg_unique_transfn(PG_FUNCTION_ARGS)
{
	return jsonb_object_agg_transfn_worker(fcinfo, false, true);
}

/*
 * jsonb_object_agg_unique_strict 聚合函数
 */
Datum
jsonb_object_agg_unique_strict_transfn(PG_FUNCTION_ARGS)
{
	return jsonb_object_agg_transfn_worker(fcinfo, true, true);
}

Datum
jsonb_object_agg_finalfn(PG_FUNCTION_ARGS)
{
	JsonbAggState *arg;
	JsonbInState result;
	Jsonb	   *out;

	/* 因参数为 internal 类型而无法直接调用 */
	Assert(AggCheckCallContext(fcinfo, NULL));

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();		/* 无输入值时才返回 null */

	arg = (JsonbAggState *) PG_GETARG_POINTER(0);

	/*
	 * 须浅克隆参数的 res 字段,以防最终
	 * 函数被多次调用而改动
	 * 聚合状态值。浅克隆已够用,因我们不会
	 * 改任何值,只加最终对象的结束
	 * 标记
	 */
	memset(&result, 0, sizeof(JsonbInState));

	result.parseState = clone_parse_state(arg->res->parseState);

	result.res = pushJsonbValue(&result.parseState,
								WJB_END_OBJECT, NULL);

	out = JsonbValueToJsonb(result.res);

	PG_RETURN_POINTER(out);
}


/*
 * 从 raw-scalar 伪数组 jsonb 提取标量值。
 */
bool
JsonbExtractScalar(JsonbContainer *jbc, JsonbValue *res)
{
	JsonbIterator *it;
	JsonbIteratorToken tok PG_USED_FOR_ASSERTS_ONLY;
	JsonbValue	tmp;

	if (!JsonContainerIsArray(jbc) || !JsonContainerIsScalar(jbc))
	{
		/* 告知调用方容器的实际类型 */
		res->type = (JsonContainerIsArray(jbc)) ? jbvArray : jbvObject;
		return false;
	}

	/*
	 * 根标量存为一元数组,故先取该数组
	 * 再取其首个(唯一)成员。
	 */
	it = JsonbIteratorInit(jbc);

	tok = JsonbIteratorNext(&it, &tmp, true);
	Assert(tok == WJB_BEGIN_ARRAY);
	Assert(tmp.val.array.nElems == 1 && tmp.val.array.rawScalar);

	tok = JsonbIteratorNext(&it, res, true);
	Assert(tok == WJB_ELEM);
	Assert(IsAJsonbScalar(res));

	tok = JsonbIteratorNext(&it, &tmp, true);
	Assert(tok == WJB_END_ARRAY);

	tok = JsonbIteratorNext(&it, &tmp, true);
	Assert(tok == WJB_DONE);

	return true;
}

/*
 * 发出正确且可翻译的转换错误消息
 */
static void
cannotCastJsonbValue(enum jbvType type, const char *sqltype)
{
	static const struct
	{
		enum jbvType type;
		const char *msg;
	}
				messages[] =
	{
		{jbvNull, gettext_noop("cannot cast jsonb null to type %s")},
		{jbvString, gettext_noop("cannot cast jsonb string to type %s")},
		{jbvNumeric, gettext_noop("cannot cast jsonb numeric to type %s")},
		{jbvBool, gettext_noop("cannot cast jsonb boolean to type %s")},
		{jbvArray, gettext_noop("cannot cast jsonb array to type %s")},
		{jbvObject, gettext_noop("cannot cast jsonb object to type %s")},
		{jbvBinary, gettext_noop("cannot cast jsonb array or object to type %s")}
	};
	int			i;

	for (i = 0; i < lengthof(messages); i++)
		if (messages[i].type == type)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg(messages[i].msg, sqltype)));

	/* 应不可达 */
	elog(ERROR, "unknown jsonb type: %d", (int) type);
}

Datum
jsonb_bool(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvBool)
		cannotCastJsonbValue(v.type, "boolean");

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_BOOL(v.val.boolean);
}

Datum
jsonb_numeric(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;
	Numeric		retValue;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvNumeric)
		cannotCastJsonbValue(v.type, "numeric");

	/*
	 * v.val.numeric 指向 jsonb 体,故须复制后再
	 * 返回
	 */
	retValue = DatumGetNumericCopy(NumericGetDatum(v.val.numeric));

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_NUMERIC(retValue);
}

Datum
jsonb_int2(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;
	Datum		retValue;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvNumeric)
		cannotCastJsonbValue(v.type, "smallint");

	retValue = DirectFunctionCall1(numeric_int2,
								   NumericGetDatum(v.val.numeric));

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_DATUM(retValue);
}

Datum
jsonb_int4(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;
	Datum		retValue;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvNumeric)
		cannotCastJsonbValue(v.type, "integer");

	retValue = DirectFunctionCall1(numeric_int4,
								   NumericGetDatum(v.val.numeric));

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_DATUM(retValue);
}

Datum
jsonb_int8(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;
	Datum		retValue;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvNumeric)
		cannotCastJsonbValue(v.type, "bigint");

	retValue = DirectFunctionCall1(numeric_int8,
								   NumericGetDatum(v.val.numeric));

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_DATUM(retValue);
}

Datum
jsonb_float4(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;
	Datum		retValue;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvNumeric)
		cannotCastJsonbValue(v.type, "real");

	retValue = DirectFunctionCall1(numeric_float4,
								   NumericGetDatum(v.val.numeric));

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_DATUM(retValue);
}

Datum
jsonb_float8(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;
	Datum		retValue;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvNumeric)
		cannotCastJsonbValue(v.type, "double precision");

	retValue = DirectFunctionCall1(numeric_float8,
								   NumericGetDatum(v.val.numeric));

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_DATUM(retValue);
}
