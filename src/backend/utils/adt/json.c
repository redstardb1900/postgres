/*-------------------------------------------------------------------------
 *
 * json.c
 *		JSON 类型支持。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/json.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/json.h"
#include "utils/jsonfuncs.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

typedef enum					/* datum_to_json 的类型类别 */
{
	JSONTYPE_NULL,				/* 为 NULL，故未费心识别 */
	JSONTYPE_BOOL,				/* 布尔（仅内建类型） */
	JSONTYPE_NUMERIC,			/* 数值（同） */
	JSONTYPE_DATE,				/* 对日期时间类型用特殊格式化 */
	JSONTYPE_TIMESTAMP,
	JSONTYPE_TIMESTAMPTZ,
	JSONTYPE_JSON,				/* JSON（含 JSONB） */
	JSONTYPE_ARRAY,				/*数组*/
	JSONTYPE_COMPOSITE,			/*复合*/
	JSONTYPE_CAST,				/* 具有到 JSON 的显式转换 */
	JSONTYPE_OTHER				/*其余*/
} JsonTypeCategory;


/*
 * 支持快速检查键的唯一性。
 *
 * 我们为 JSON 对象中已用的键维护一张哈希表，以便快速
 * 查重。
 */
/* 键唯一性检查的公共上下文 */
typedef struct HTAB *JsonUniqueCheckState;	/* 存放键名的哈希表 */

/* JsonUniqueCheckState 哈希条目 */
typedef struct JsonUniqueHashEntry
{
	const char *key;
	int			key_len;
	int			object_id;
} JsonUniqueHashEntry;

/* JSON 解析期间键唯一性检查的栈元素 */
typedef struct JsonUniqueStackEntry
{
	struct JsonUniqueStackEntry *parent;
	int			object_id;
} JsonUniqueStackEntry;

/* JSON 解析期间键唯一性检查的上下文结构体 */
typedef struct JsonUniqueParsingState
{
	JsonLexContext *lex;
	JsonUniqueCheckState check;
	JsonUniqueStackEntry *stack;
	int			id_counter;
	bool		unique;
} JsonUniqueParsingState;

/* JSON 构建期间键唯一性检查的上下文结构体 */
typedef struct JsonUniqueBuilderState
{
	JsonUniqueCheckState check; /* 唯一检查 */
	StringInfoData skipped_keys;	/* 跳过值为 NULL 的键 */
	MemoryContext mcxt;			/* 保存跳过键的上下文 */
} JsonUniqueBuilderState;


/* JSON 聚合的状态结构体 */
typedef struct JsonAggState
{
	StringInfo	str;
	JsonTypeCategory key_category;
	Oid			key_output_func;
	JsonTypeCategory val_category;
	Oid			val_output_func;
	JsonUniqueBuilderState unique_check;
} JsonAggState;

static void composite_to_json(Datum composite, StringInfo result,
							  bool use_line_feeds);
static void array_dim_to_json(StringInfo result, int dim, int ndims, int *dims,
							  Datum *vals, bool *nulls, int *valcount,
							  JsonTypeCategory tcategory, Oid outfuncoid,
							  bool use_line_feeds);
static void array_to_json_internal(Datum array, StringInfo result,
								   bool use_line_feeds);
static void json_categorize_type(Oid typoid,
								 JsonTypeCategory *tcategory,
								 Oid *outfuncoid);
static void datum_to_json(Datum val, bool is_null, StringInfo result,
						  JsonTypeCategory tcategory, Oid outfuncoid,
						  bool key_scalar);
static void add_json(Datum val, bool is_null, StringInfo result,
					 Oid val_type, bool key_scalar);
static text *catenate_stringinfo_string(StringInfo buffer, const char *addon);

/*
 * 输入
 */
Datum
json_in(PG_FUNCTION_ARGS)
{
	char	   *json = PG_GETARG_CSTRING(0);
	text	   *result = cstring_to_text(json);
	JsonLexContext *lex;

	/* 先校验 */
	lex = makeJsonLexContext(result, false);
	if (!pg_parse_json_or_errsave(lex, &nullSemAction, fcinfo->context))
		PG_RETURN_NULL();

	/* 内部表示与 text 相同 */
	PG_RETURN_TEXT_P(result);
}

/*
 * 输出
 */
Datum
json_out(PG_FUNCTION_ARGS)
{
	/* 无须 detoast，text_to_cstring 会处理它 */
	Datum		txt = PG_GETARG_DATUM(0);

	PG_RETURN_CSTRING(TextDatumGetCString(txt));
}

/*
 * Binary send.
 */
Datum
json_send(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendtext(&buf, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * 二进制接收
 */
Datum
json_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	char	   *str;
	int			nbytes;
	JsonLexContext *lex;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);

	/* 先校验。 */
	lex = makeJsonLexContextCstringLen(str, nbytes, GetDatabaseEncoding(), false);
	pg_parse_json_or_ereport(lex, &nullSemAction);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(str, nbytes));
}

/*
 * 确定在 datum_to_json 中如何打印给定类型的值。
 *
 * 给定数据类型 OID，返回其 JsonTypeCategory，
 * 以及该类型的输出函数 OID。若返回的类别是
 * JSONTYPE_CAST 则返回到 JSON 的转换函数 OID
 */
static void
json_categorize_type(Oid typoid,
					 JsonTypeCategory *tcategory,
					 Oid *outfuncoid)
{
	bool		typisvarlena;

	/* 穿透任何域 */
	typoid = getBaseType(typoid);

	*outfuncoid = InvalidOid;

	/*
	 * 除了 date/timestamp 类型、数组与复合类型、
	 * 布尔值以及存在到 json 转换的非内建类型，
	 * 都需要获取其输出函数。
	 */

	switch (typoid)
	{
		case BOOLOID:
			*tcategory = JSONTYPE_BOOL;
			break;

		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
			*tcategory = JSONTYPE_NUMERIC;
			break;

		case DATEOID:
			*tcategory = JSONTYPE_DATE;
			break;

		case TIMESTAMPOID:
			*tcategory = JSONTYPE_TIMESTAMP;
			break;

		case TIMESTAMPTZOID:
			*tcategory = JSONTYPE_TIMESTAMPTZ;
			break;

		case JSONOID:
		case JSONBOID:
			getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
			*tcategory = JSONTYPE_JSON;
			break;

		default:
			/* 检查数组和复合类型 */
			if (OidIsValid(get_element_type(typoid)) || typoid == ANYARRAYOID
				|| typoid == ANYCOMPATIBLEARRAYOID || typoid == RECORDARRAYOID)
				*tcategory = JSONTYPE_ARRAY;
			else if (type_is_rowtype(typoid))	/* 含 RECORDOID */
				*tcategory = JSONTYPE_COMPOSITE;
			else
			{
				/* 多半是一般情形 ... */
				*tcategory = JSONTYPE_OTHER;
				/* 但若非内建，则查找是否有到 json 的转换 */
				if (typoid >= FirstNormalObjectId)
				{
					Oid			castfunc;
					CoercionPathType ctype;

					ctype = find_coercion_pathway(JSONOID, typoid,
												  COERCION_EXPLICIT,
												  &castfunc);
					if (ctype == COERCION_PATH_FUNC && OidIsValid(castfunc))
					{
						*tcategory = JSONTYPE_CAST;
						*outfuncoid = castfunc;
					}
					else
					{
						/* 无转换的非内建类型 */
						getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
					}
				}
				else
				{
					/* 其他内建类型 */
					getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
				}
			}
			break;
	}
}

/*
 * 把 Datum 转成 JSON 文本并追加到 "result"。
 *
 * tcategory 与 outfuncoid 来自此前一次 json_categorize_type 调用，
 * 但若 is_null 为真则二者可以为无效值。
 *
 * 若 key_scalar 为真，说明该值正作为键打印，必须
 * 确保其类型可接受，并强制加引号。
 */
static void
datum_to_json(Datum val, bool is_null, StringInfo result,
			  JsonTypeCategory tcategory, Oid outfuncoid,
			  bool key_scalar)
{
	char	   *outputstr;
	text	   *jsontext;

	check_stack_depth();

	/* 调用方应确保不传入 NULL 键 */
	Assert(!(key_scalar && is_null));

	if (is_null)
	{
		appendStringInfoString(result, "null");
		return;
	}

	if (key_scalar &&
		(tcategory == JSONTYPE_ARRAY ||
		 tcategory == JSONTYPE_COMPOSITE ||
		 tcategory == JSONTYPE_JSON ||
		 tcategory == JSONTYPE_CAST))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("key value must be scalar, not array, composite, or json")));

	switch (tcategory)
	{
		case JSONTYPE_ARRAY:
			array_to_json_internal(val, result, false);
			break;
		case JSONTYPE_COMPOSITE:
			composite_to_json(val, result, false);
			break;
		case JSONTYPE_BOOL:
			outputstr = DatumGetBool(val) ? "true" : "false";
			if (key_scalar)
				escape_json(result, outputstr);
			else
				appendStringInfoString(result, outputstr);
			break;
		case JSONTYPE_NUMERIC:
			outputstr = OidOutputFunctionCall(outfuncoid, val);

			/*
			 * 非键若为合法 JSON 数字则不调用 escape_json
			 * 即可
			 */
			if (!key_scalar && IsValidJsonNumber(outputstr, strlen(outputstr)))
				appendStringInfoString(result, outputstr);
			else
				escape_json(result, outputstr);
			pfree(outputstr);
			break;
		case JSONTYPE_DATE:
			{
				char		buf[MAXDATELEN + 1];

				JsonEncodeDateTime(buf, val, DATEOID, NULL);
				appendStringInfo(result, "\"%s\"", buf);
			}
			break;
		case JSONTYPE_TIMESTAMP:
			{
				char		buf[MAXDATELEN + 1];

				JsonEncodeDateTime(buf, val, TIMESTAMPOID, NULL);
				appendStringInfo(result, "\"%s\"", buf);
			}
			break;
		case JSONTYPE_TIMESTAMPTZ:
			{
				char		buf[MAXDATELEN + 1];

				JsonEncodeDateTime(buf, val, TIMESTAMPTZOID, NULL);
				appendStringInfo(result, "\"%s\"", buf);
			}
			break;
		case JSONTYPE_JSON:
			/* JSON 与 JSONB 的输出已转义过 */
			outputstr = OidOutputFunctionCall(outfuncoid, val);
			appendStringInfoString(result, outputstr);
			pfree(outputstr);
			break;
		case JSONTYPE_CAST:
			/* outfuncoid 指向的是转换函数而非输出函数 */
			jsontext = DatumGetTextPP(OidFunctionCall1(outfuncoid, val));
			outputstr = text_to_cstring(jsontext);
			appendStringInfoString(result, outputstr);
			pfree(outputstr);
			pfree(jsontext);
			break;
		default:
			outputstr = OidOutputFunctionCall(outfuncoid, val);
			escape_json(result, outputstr);
			pfree(outputstr);
			break;
	}
}

/*
 * 将 datetime 类型 'typid' 的值 'value' 按 ISO 格式编码，
 * 写入可选预分配的缓冲区 'buf'。可选的 'tzp' 决定显示
 * timestamptz 时的时区偏移量（单位为秒）。
 */
char *
JsonEncodeDateTime(char *buf, Datum value, Oid typid, const int *tzp)
{
	if (!buf)
		buf = palloc(MAXDATELEN + 1);

	switch (typid)
	{
		case DATEOID:
			{
				DateADT		date;
				struct pg_tm tm;

				date = DatumGetDateADT(value);

				/* 同 date_out()，但强制 DateStyle */
				if (DATE_NOT_FINITE(date))
					EncodeSpecialDate(date, buf);
				else
				{
					j2date(date + POSTGRES_EPOCH_JDATE,
						   &(tm.tm_year), &(tm.tm_mon), &(tm.tm_mday));
					EncodeDateOnly(&tm, USE_XSD_DATES, buf);
				}
			}
			break;
		case TIMEOID:
			{
				TimeADT		time = DatumGetTimeADT(value);
				struct pg_tm tt,
						   *tm = &tt;
				fsec_t		fsec;

				/* 同 time_out()，但强制 DateStyle */
				time2tm(time, tm, &fsec);
				EncodeTimeOnly(tm, fsec, false, 0, USE_XSD_DATES, buf);
			}
			break;
		case TIMETZOID:
			{
				TimeTzADT  *time = DatumGetTimeTzADTP(value);
				struct pg_tm tt,
						   *tm = &tt;
				fsec_t		fsec;
				int			tz;

				/* 同 timetz_out()，但强制 DateStyle */
				timetz2tm(time, tm, &fsec, &tz);
				EncodeTimeOnly(tm, fsec, true, tz, USE_XSD_DATES, buf);
			}
			break;
		case TIMESTAMPOID:
			{
				Timestamp	timestamp;
				struct pg_tm tm;
				fsec_t		fsec;

				timestamp = DatumGetTimestamp(value);
				/* 同 timestamp_out()，但强制 DateStyle */
				if (TIMESTAMP_NOT_FINITE(timestamp))
					EncodeSpecialTimestamp(timestamp, buf);
				else if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, NULL) == 0)
					EncodeDateTime(&tm, fsec, false, 0, NULL, USE_XSD_DATES, buf);
				else
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));
			}
			break;
		case TIMESTAMPTZOID:
			{
				TimestampTz timestamp;
				struct pg_tm tm;
				int			tz;
				fsec_t		fsec;
				const char *tzn = NULL;

				timestamp = DatumGetTimestampTz(value);

				/*
				 * 若指定了时区，则先应用时区偏移，
				 * 再把 timestamptz 视作无时区转为 pg_tm，
				 * 最后再以指定的时区把时间戳
				 * 转换成字符串。
				 */
				if (tzp)
				{
					tz = *tzp;
					timestamp -= (TimestampTz) tz * USECS_PER_SEC;
				}

				/* 同 timestamptz_out()，但强制 DateStyle */
				if (TIMESTAMP_NOT_FINITE(timestamp))
					EncodeSpecialTimestamp(timestamp, buf);
				else if (timestamp2tm(timestamp, tzp ? NULL : &tz, &tm, &fsec,
									  tzp ? NULL : &tzn, NULL) == 0)
				{
					if (tzp)
						tm.tm_isdst = 1;	/* 置时区存在标志 */

					EncodeDateTime(&tm, fsec, true, tz, tzn, USE_XSD_DATES, buf);
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));
			}
			break;
		default:
			elog(ERROR, "unknown jsonb value datetime type oid %u", typid);
			return NULL;
	}

	return buf;
}

/*
 * 处理数组的一个维度。
 * 若是最内层维度则输出各值，否则递归调用自身
 * 去处理下一个维度。
 */
static void
array_dim_to_json(StringInfo result, int dim, int ndims, int *dims, Datum *vals,
				  bool *nulls, int *valcount, JsonTypeCategory tcategory,
				  Oid outfuncoid, bool use_line_feeds)
{
	int			i;
	const char *sep;

	Assert(dim < ndims);

	sep = use_line_feeds ? ",\n " : ",";

	appendStringInfoChar(result, '[');

	for (i = 1; i <= dims[dim]; i++)
	{
		if (i > 1)
			appendStringInfoString(result, sep);

		if (dim + 1 == ndims)
		{
			datum_to_json(vals[*valcount], nulls[*valcount], result, tcategory,
						  outfuncoid, false);
			(*valcount)++;
		}
		else
		{
			/*
			 * 要不要在数组的内层维度上也换行？目前
			 * 先不换。
			 */
			array_dim_to_json(result, dim + 1, ndims, dims, vals, nulls,
							  valcount, tcategory, outfuncoid, false);
		}
	}

	appendStringInfoChar(result, ']');
}

/*
 * 把数组转成 JSON。
 */
static void
array_to_json_internal(Datum array, StringInfo result, bool use_line_feeds)
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
	JsonTypeCategory tcategory;
	Oid			outfuncoid;

	ndim = ARR_NDIM(v);
	dim = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndim, dim);

	if (nitems <= 0)
	{
		appendStringInfoString(result, "[]");
		return;
	}

	get_typlenbyvalalign(element_type,
						 &typlen, &typbyval, &typalign);

	json_categorize_type(element_type,
						 &tcategory, &outfuncoid);

	deconstruct_array(v, element_type, typlen, typbyval,
					  typalign, &elements, &nulls,
					  &nitems);

	array_dim_to_json(result, 0, ndim, dim, elements, nulls, &count, tcategory,
					  outfuncoid, use_line_feeds);

	pfree(elements);
	pfree(nulls);
}

/*
 * 把复合类型/记录转成 JSON。
 */
static void
composite_to_json(Datum composite, StringInfo result, bool use_line_feeds)
{
	HeapTupleHeader td;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tmptup,
			   *tuple;
	int			i;
	bool		needsep = false;
	const char *sep;

	sep = use_line_feeds ? ",\n " : ",";

	td = DatumGetHeapTupleHeader(composite);

	/* 提取行类型信息并找到 tupdesc */
	tupType = HeapTupleHeaderGetTypeId(td);
	tupTypmod = HeapTupleHeaderGetTypMod(td);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	/* 构建临时 HeapTuple 控制结构 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	tmptup.t_data = td;
	tuple = &tmptup;

	appendStringInfoChar(result, '{');

	for (i = 0; i < tupdesc->natts; i++)
	{
		Datum		val;
		bool		isnull;
		char	   *attname;
		JsonTypeCategory tcategory;
		Oid			outfuncoid;
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;

		if (needsep)
			appendStringInfoString(result, sep);
		needsep = true;

		attname = NameStr(att->attname);
		escape_json(result, attname);
		appendStringInfoChar(result, ':');

		val = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		if (isnull)
		{
			tcategory = JSONTYPE_NULL;
			outfuncoid = InvalidOid;
		}
		else
			json_categorize_type(att->atttypid, &tcategory, &outfuncoid);

		datum_to_json(val, isnull, result, tcategory, outfuncoid, false);
	}

	appendStringInfoChar(result, '}');
	ReleaseTupleDesc(tupdesc);
}

/*
 * 向结果追加 "val" 的 JSON 文本
 *
 * 这只是 datum_to_json 的一个薄封装。若同一类型要打印
 * 很多次，应避免使用它；最好只做一次 json_categorize_type
 * 的查找。
 */
static void
add_json(Datum val, bool is_null, StringInfo result,
		 Oid val_type, bool key_scalar)
{
	JsonTypeCategory tcategory;
	Oid			outfuncoid;

	if (val_type == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine input data type")));

	if (is_null)
	{
		tcategory = JSONTYPE_NULL;
		outfuncoid = InvalidOid;
	}
	else
		json_categorize_type(val_type,
							 &tcategory, &outfuncoid);

	datum_to_json(val, is_null, result, tcategory, outfuncoid, key_scalar);
}

/*
 * SQL 函数 array_to_json(row)
 */
Datum
array_to_json(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	StringInfo	result;

	result = makeStringInfo();

	array_to_json_internal(array, result, false);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
}

/*
 * SQL 函数 array_to_json(row, prettybool)
 */
Datum
array_to_json_pretty(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	bool		use_line_feeds = PG_GETARG_BOOL(1);
	StringInfo	result;

	result = makeStringInfo();

	array_to_json_internal(array, result, use_line_feeds);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
}

/*
 * SQL 函数 row_to_json(row)
 */
Datum
row_to_json(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	StringInfo	result;

	result = makeStringInfo();

	composite_to_json(array, result, false);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
}

/*
 * SQL 函数 row_to_json(row, prettybool)
 */
Datum
row_to_json_pretty(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	bool		use_line_feeds = PG_GETARG_BOOL(1);
	StringInfo	result;

	result = makeStringInfo();

	composite_to_json(array, result, use_line_feeds);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
}

/*
 * 给定类型脱离 JSON 上下文后还算不可变吗？
 *
 * 目前所有日期时间类型都被视为可变，因为
 * 依赖时区。XXX 另外还应深入对象与数组，
 * 但并未这样做。
 */
bool
to_json_is_immutable(Oid typoid)
{
	JsonTypeCategory tcategory;
	Oid			outfuncoid;

	json_categorize_type(typoid, &tcategory, &outfuncoid);

	switch (tcategory)
	{
		case JSONTYPE_BOOL:
		case JSONTYPE_JSON:
		case JSONTYPE_NULL:
			return true;

		case JSONTYPE_DATE:
		case JSONTYPE_TIMESTAMP:
		case JSONTYPE_TIMESTAMPTZ:
			return false;

		case JSONTYPE_ARRAY:
			return false;		/* TODO 递归处理元素 */

		case JSONTYPE_COMPOSITE:
			return false;		/* TODO 递归处理字段 */

		case JSONTYPE_NUMERIC:
		case JSONTYPE_CAST:
		case JSONTYPE_OTHER:
			return func_volatile(outfuncoid) == PROVOLATILE_IMMUTABLE;
	}

	return false;				/* 不可达 */
}

/*
 * SQL 函数 to_json(anyvalue)
 */
Datum
to_json(PG_FUNCTION_ARGS)
{
	Datum		val = PG_GETARG_DATUM(0);
	Oid			val_type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	StringInfo	result;
	JsonTypeCategory tcategory;
	Oid			outfuncoid;

	if (val_type == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine input data type")));

	json_categorize_type(val_type,
						 &tcategory, &outfuncoid);

	result = makeStringInfo();

	datum_to_json(val, false, result, tcategory, outfuncoid, false);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(result->data, result->len));
}

/*
 * json_agg 转换函数
 *
 * 把输入列聚合为一个 json 数组值。
 */
static Datum
json_agg_transfn_worker(FunctionCallInfo fcinfo, bool absent_on_null)
{
	MemoryContext aggcontext,
				oldcontext;
	JsonAggState *state;
	Datum		val;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* 因参数为 internal 类型而无法直接调用 */
		elog(ERROR, "json_agg_transfn called in non-aggregate context");
	}

	if (PG_ARGISNULL(0))
	{
		Oid			arg_type = get_fn_expr_argtype(fcinfo->flinfo, 1);

		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine input data type")));

		/*
		 * 此状态对象要创建在能存活整个聚合调用期间的
		 * 上下文中。MemoryContextSwitchTo 只在第一次
		 * 时才需要，因为 StringInfo 例程会确保在必要时
		 * 使用正确的上下文来扩大该对象。
		 */
		oldcontext = MemoryContextSwitchTo(aggcontext);
		state = (JsonAggState *) palloc(sizeof(JsonAggState));
		state->str = makeStringInfo();
		MemoryContextSwitchTo(oldcontext);

		appendStringInfoChar(state->str, '[');
		json_categorize_type(arg_type, &state->val_category,
							 &state->val_output_func);
	}
	else
	{
		state = (JsonAggState *) PG_GETARG_POINTER(0);
	}

	if (absent_on_null && PG_ARGISNULL(1))
		PG_RETURN_POINTER(state);

	if (state->str->len > 1)
		appendStringInfoString(state->str, ", ");

	/* NULLs 快速路径 */
	if (PG_ARGISNULL(1))
	{
		datum_to_json((Datum) 0, true, state->str, JSONTYPE_NULL,
					  InvalidOid, false);
		PG_RETURN_POINTER(state);
	}

	val = PG_GETARG_DATUM(1);

	/* 若为结构化类型且非首项则加些空白 */
	if (!PG_ARGISNULL(0) && state->str->len > 1 &&
		(state->val_category == JSONTYPE_ARRAY ||
		 state->val_category == JSONTYPE_COMPOSITE))
	{
		appendStringInfoString(state->str, "\n ");
	}

	datum_to_json(val, false, state->str, state->val_category,
				  state->val_output_func, false);

	/*
	 * json_agg() 的转换类型被声明为 "internal"，这是一个
	 * 与指针同尺寸的按值传递类型。因此我们可以放心地
	 * 把 JsonAggState 指针传过 nodeAgg.c 的种种处理。
	 */
	PG_RETURN_POINTER(state);
}


/*
 * json_agg 聚合函数
 */
Datum
json_agg_transfn(PG_FUNCTION_ARGS)
{
	return json_agg_transfn_worker(fcinfo, false);
}

/*
 * json_agg_strict 聚合函数
 */
Datum
json_agg_strict_transfn(PG_FUNCTION_ARGS)
{
	return json_agg_transfn_worker(fcinfo, true);
}

/*
 * json_agg 最终函数
 */
Datum
json_agg_finalfn(PG_FUNCTION_ARGS)
{
	JsonAggState *state;

	/* 因参数为 internal 类型而无法直接调用 */
	Assert(AggCheckCallContext(fcinfo, NULL));

	state = PG_ARGISNULL(0) ?
		NULL :
		(JsonAggState *) PG_GETARG_POINTER(0);

	/* 无输入行时返回 NULL，与聚合的标准相同 */
	if (state == NULL)
		PG_RETURN_NULL();

	/* 否则返回加上相应数组终止符后的状态 */
	PG_RETURN_TEXT_P(catenate_stringinfo_string(state->str, "]"));
}

/* 实现键唯一性检查哈希表的函数 */
static uint32
json_unique_hash(const void *key, Size keysize)
{
	const JsonUniqueHashEntry *entry = (JsonUniqueHashEntry *) key;
	uint32		hash = hash_bytes_uint32(entry->object_id);

	hash ^= hash_bytes((const unsigned char *) entry->key, entry->key_len);

	return DatumGetUInt32(hash);
}

static int
json_unique_hash_match(const void *key1, const void *key2, Size keysize)
{
	const JsonUniqueHashEntry *entry1 = (const JsonUniqueHashEntry *) key1;
	const JsonUniqueHashEntry *entry2 = (const JsonUniqueHashEntry *) key2;

	if (entry1->object_id != entry2->object_id)
		return entry1->object_id > entry2->object_id ? 1 : -1;

	if (entry1->key_len != entry2->key_len)
		return entry1->key_len > entry2->key_len ? 1 : -1;

	return strncmp(entry1->key, entry2->key, entry1->key_len);
}

/*
 * 唯一性检测支持。
 *
 * 为了在构建或解析 JSON 对象期间检测唯一性，
 * 我们维护一张已见键名的哈希表。
 */
static void
json_unique_check_init(JsonUniqueCheckState *cxt)
{
	HASHCTL		ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(JsonUniqueHashEntry);
	ctl.entrysize = sizeof(JsonUniqueHashEntry);
	ctl.hcxt = CurrentMemoryContext;
	ctl.hash = json_unique_hash;
	ctl.match = json_unique_hash_match;

	*cxt = hash_create("json object hashtable",
					   32,
					   &ctl,
					   HASH_ELEM | HASH_CONTEXT | HASH_FUNCTION | HASH_COMPARE);
}

static void
json_unique_builder_init(JsonUniqueBuilderState *cxt)
{
	json_unique_check_init(&cxt->check);
	cxt->mcxt = CurrentMemoryContext;
	cxt->skipped_keys.data = NULL;
}

static bool
json_unique_check_key(JsonUniqueCheckState *cxt, const char *key, int object_id)
{
	JsonUniqueHashEntry entry;
	bool		found;

	entry.key = key;
	entry.key_len = strlen(key);
	entry.object_id = object_id;

	(void) hash_search(*cxt, &entry, HASH_ENTER, &found);

	return !found;
}

/*
 * 按需初始化一个用后即弃的 StringInfo。它用于
 * 读取无需存入输出对象的键名，用于值为 NULL 时
 * 的重复键检测。
 */
static StringInfo
json_unique_builder_get_throwawaybuf(JsonUniqueBuilderState *cxt)
{
	StringInfo	out = &cxt->skipped_keys;

	if (!out->data)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(cxt->mcxt);

		initStringInfo(out);
		MemoryContextSwitchTo(oldcxt);
	}
	else
		/* 把字符串重置为空 */
		out->len = 0;

	return out;
}

/*
 * json_object_agg 转换函数。
 *
 * 把两个输入列聚合为单个 json 对象值。
 */
static Datum
json_object_agg_transfn_worker(FunctionCallInfo fcinfo,
							   bool absent_on_null, bool unique_keys)
{
	MemoryContext aggcontext,
				oldcontext;
	JsonAggState *state;
	StringInfo	out;
	Datum		arg;
	bool		skip;
	int			key_offset;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* 因参数为 internal 类型而无法直接调用 */
		elog(ERROR, "json_object_agg_transfn called in non-aggregate context");
	}

	if (PG_ARGISNULL(0))
	{
		Oid			arg_type;

		/*
		 * StringInfo 要创建在能存活整个聚合调用期间的
		 * 上下文中。切换上下文只在最初这一步需要，
		 * 因为 StringInfo 与 dynahash 例程会确保在必要时
		 * 使用正确的上下文来扩大对象。
		 */
		oldcontext = MemoryContextSwitchTo(aggcontext);
		state = (JsonAggState *) palloc(sizeof(JsonAggState));
		state->str = makeStringInfo();
		if (unique_keys)
			json_unique_builder_init(&state->unique_check);
		else
			memset(&state->unique_check, 0, sizeof(state->unique_check));
		MemoryContextSwitchTo(oldcontext);

		arg_type = get_fn_expr_argtype(fcinfo->flinfo, 1);

		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine data type for argument %d", 1)));

		json_categorize_type(arg_type, &state->key_category,
							 &state->key_output_func);

		arg_type = get_fn_expr_argtype(fcinfo->flinfo, 2);

		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine data type for argument %d", 2)));

		json_categorize_type(arg_type, &state->val_category,
							 &state->val_output_func);

		appendStringInfoString(state->str, "{ ");
	}
	else
	{
		state = (JsonAggState *) PG_GETARG_POINTER(0);
	}

	/*
	 * 注意：由于 json_object_agg() 声明为接受 "any" 类型，
	 * 解析器不会对未知类型字面量（即未加修饰的字符串
	 * 或 NULL）做任何类型转换。这类值会以 UNKNOWN
	 * 类型到来，所幸这对我们无关紧要，因为
	 * unknownout() 正常
	 */

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("null value not allowed for object key")));

	/* absent_on_null 时跳过 NULL 值 */
	skip = absent_on_null && PG_ARGISNULL(2);

	if (skip)
	{
		/*
		 * 我们得到 NULL 值且不打算存储它；若不检测
		 * 键唯一性，那就完成了。若要检测，就得借助
		 * 用后即弃缓冲区存下键名以便检查。
		 */
		if (!unique_keys)
			PG_RETURN_POINTER(state);

		out = json_unique_builder_get_throwawaybuf(&state->unique_check);
	}
	else
	{
		out = state->str;

		/*
		 * 只有在初始字符串 "{ " 之后已输出过一些字段时
		 * 才追加逗号分隔符。
		 */
		if (out->len > 2)
			appendStringInfoString(out, ", ");
	}

	arg = PG_GETARG_DATUM(1);

	key_offset = out->len;

	datum_to_json(arg, false, out, state->key_category,
				  state->key_output_func, true);

	if (unique_keys)
	{
		/*
		 * 先复制键而非指向缓冲区内部。它会被
		 * 加入哈希表，但缓冲区可能因继续追加
		 * 数据而被重新分配，这会使指向当前缓冲区
		 * 中键的指针失效。
		 */
		const char *key = MemoryContextStrdup(aggcontext,
											  &out->data[key_offset]);

		if (!json_unique_check_key(&state->unique_check.check, key, 0))
			ereport(ERROR,
					errcode(ERRCODE_DUPLICATE_JSON_OBJECT_KEY_VALUE),
					errmsg("duplicate JSON object key value: %s", key));

		if (skip)
			PG_RETURN_POINTER(state);
	}

	appendStringInfoString(state->str, " : ");

	if (PG_ARGISNULL(2))
		arg = (Datum) 0;
	else
		arg = PG_GETARG_DATUM(2);

	datum_to_json(arg, PG_ARGISNULL(2), state->str, state->val_category,
				  state->val_output_func, false);

	PG_RETURN_POINTER(state);
}

/*
 * json_object_agg 聚合函数
 */
Datum
json_object_agg_transfn(PG_FUNCTION_ARGS)
{
	return json_object_agg_transfn_worker(fcinfo, false, false);
}

/*
 * json_object_agg_strict 聚合函数
 */
Datum
json_object_agg_strict_transfn(PG_FUNCTION_ARGS)
{
	return json_object_agg_transfn_worker(fcinfo, true, false);
}

/*
 * json_object_agg_unique 聚合函数
 */
Datum
json_object_agg_unique_transfn(PG_FUNCTION_ARGS)
{
	return json_object_agg_transfn_worker(fcinfo, false, true);
}

/*
 * json_object_agg_unique_strict 聚合函数
 */
Datum
json_object_agg_unique_strict_transfn(PG_FUNCTION_ARGS)
{
	return json_object_agg_transfn_worker(fcinfo, true, true);
}

/*
 * json_object_agg 最终函数。
 */
Datum
json_object_agg_finalfn(PG_FUNCTION_ARGS)
{
	JsonAggState *state;

	/* 因参数为 internal 类型而无法直接调用 */
	Assert(AggCheckCallContext(fcinfo, NULL));

	state = PG_ARGISNULL(0) ? NULL : (JsonAggState *) PG_GETARG_POINTER(0);

	/* 无输入行时返回 NULL，与聚合的标准相同 */
	if (state == NULL)
		PG_RETURN_NULL();

	/* 否则返回加上相应对象终止符后的状态 */
	PG_RETURN_TEXT_P(catenate_stringinfo_string(state->str, " }"));
}

/*
 * 聚合辅助函数：返回给定 StringInfo 的内容加上
 * 指定结尾串而成的 text 值。之所以需要它，是因为聚合
 * 的最终函数不允许修改聚合状态。
 */
static text *
catenate_stringinfo_string(StringInfo buffer, const char *addon)
{
	/* cstring_to_text_with_len 定制版本 */
	int			buflen = buffer->len;
	int			addlen = strlen(addon);
	text	   *result = (text *) palloc(buflen + addlen + VARHDRSZ);

	SET_VARSIZE(result, buflen + addlen + VARHDRSZ);
	memcpy(VARDATA(result), buffer->data, buflen);
	memcpy(VARDATA(result) + buflen, addon, addlen);

	return result;
}

Datum
json_build_object_worker(int nargs, Datum *args, bool *nulls, Oid *types,
						 bool absent_on_null, bool unique_keys)
{
	int			i;
	const char *sep = "";
	StringInfo	result;
	JsonUniqueBuilderState unique_check;

	if (nargs % 2 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument list must have even number of elements"),
		/* translator: %s is a SQL function name */
				 errhint("The arguments of %s must consist of alternating keys and values.",
						 "json_build_object()")));

	result = makeStringInfo();

	appendStringInfoChar(result, '{');

	if (unique_keys)
		json_unique_builder_init(&unique_check);

	for (i = 0; i < nargs; i += 2)
	{
		StringInfo	out;
		bool		skip;
		int			key_offset;

		/* absent_on_null 时跳过 NULL 值 */
		skip = absent_on_null && nulls[i + 1];

		if (skip)
		{
			/* 若需要键唯一性检查则必须保存被跳过的键 */
			if (!unique_keys)
				continue;

			out = json_unique_builder_get_throwawaybuf(&unique_check);
		}
		else
		{
			appendStringInfoString(result, sep);
			sep = ", ";
			out = result;
		}

		/* 处理键 */
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for object key")));

		/* 在追加前保存键偏移量 */
		key_offset = out->len;

		add_json(args[i], false, out, types[i], true);

		if (unique_keys)
		{
			/*
			 * 在追加键之后检查键唯一性
			 *
			 * 先复制键而非指向缓冲区内部。它会被
			 * 加入哈希表，但缓冲区可能因继续追加
			 * 数据而被重新分配，这会使指向当前缓冲区
			 * 中键的指针失效。
			 */
			const char *key = pstrdup(&out->data[key_offset]);

			if (!json_unique_check_key(&unique_check.check, key, 0))
				ereport(ERROR,
						errcode(ERRCODE_DUPLICATE_JSON_OBJECT_KEY_VALUE),
						errmsg("duplicate JSON object key value: %s", key));

			if (skip)
				continue;
		}

		appendStringInfoString(result, " : ");

		/* 处理值 */
		add_json(args[i + 1], nulls[i + 1], result, types[i + 1], false);
	}

	appendStringInfoChar(result, '}');

	return PointerGetDatum(cstring_to_text_with_len(result->data, result->len));
}

/*
 * SQL 函数 json_build_object(variadic "any")
 */
Datum
json_build_object(PG_FUNCTION_ARGS)
{
	Datum	   *args;
	bool	   *nulls;
	Oid		   *types;

	/* 构建组成该对象的参数值 */
	int			nargs = extract_variadic_args(fcinfo, 0, true,
											  &args, &types, &nulls);

	if (nargs < 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(json_build_object_worker(nargs, args, nulls, types, false, false));
}

/*
 * json_build_object 收到 0 个参数时的退化情形。
 */
Datum
json_build_object_noargs(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text_with_len("{}", 2));
}

Datum
json_build_array_worker(int nargs, Datum *args, bool *nulls, Oid *types,
						bool absent_on_null)
{
	int			i;
	const char *sep = "";
	StringInfo	result;

	result = makeStringInfo();

	appendStringInfoChar(result, '[');

	for (i = 0; i < nargs; i++)
	{
		if (absent_on_null && nulls[i])
			continue;

		appendStringInfoString(result, sep);
		sep = ", ";
		add_json(args[i], nulls[i], result, types[i], false);
	}

	appendStringInfoChar(result, ']');

	return PointerGetDatum(cstring_to_text_with_len(result->data, result->len));
}

/*
 * SQL 函数 json_build_array(variadic "any")
 */
Datum
json_build_array(PG_FUNCTION_ARGS)
{
	Datum	   *args;
	bool	   *nulls;
	Oid		   *types;

	/* 构建组成该对象的参数值 */
	int			nargs = extract_variadic_args(fcinfo, 0, true,
											  &args, &types, &nulls);

	if (nargs < 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(json_build_array_worker(nargs, args, nulls, types, false));
}

/*
 * json_build_array 收到 0 个参数时的退化情形。
 */
Datum
json_build_array_noargs(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text_with_len("[]", 2));
}

/*
 * SQL 函数 json_object(text[])
 *
 * 取一维或二维 text 数组作为 json 对象的
 * 键/值对。
 */
Datum
json_object(PG_FUNCTION_ARGS)
{
	ArrayType  *in_array = PG_GETARG_ARRAYTYPE_P(0);
	int			ndims = ARR_NDIM(in_array);
	StringInfoData result;
	Datum	   *in_datums;
	bool	   *in_nulls;
	int			in_count,
				count,
				i;
	text	   *rval;
	char	   *v;

	switch (ndims)
	{
		case 0:
			PG_RETURN_DATUM(CStringGetTextDatum("{}"));
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

	initStringInfo(&result);

	appendStringInfoChar(&result, '{');

	for (i = 0; i < count; ++i)
	{
		if (in_nulls[i * 2])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for object key")));

		v = TextDatumGetCString(in_datums[i * 2]);
		if (i > 0)
			appendStringInfoString(&result, ", ");
		escape_json(&result, v);
		appendStringInfoString(&result, " : ");
		pfree(v);
		if (in_nulls[i * 2 + 1])
			appendStringInfoString(&result, "null");
		else
		{
			v = TextDatumGetCString(in_datums[i * 2 + 1]);
			escape_json(&result, v);
			pfree(v);
		}
	}

	appendStringInfoChar(&result, '}');

	pfree(in_datums);
	pfree(in_nulls);

	rval = cstring_to_text_with_len(result.data, result.len);
	pfree(result.data);

	PG_RETURN_TEXT_P(rval);
}

/*
 * SQL 函数 json_object(text[], text[])
 *
 * 取分开的 text 键数组和值数组来按对构造
 * 对象。
 */
Datum
json_object_two_arg(PG_FUNCTION_ARGS)
{
	ArrayType  *key_array = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *val_array = PG_GETARG_ARRAYTYPE_P(1);
	int			nkdims = ARR_NDIM(key_array);
	int			nvdims = ARR_NDIM(val_array);
	StringInfoData result;
	Datum	   *key_datums,
			   *val_datums;
	bool	   *key_nulls,
			   *val_nulls;
	int			key_count,
				val_count,
				i;
	text	   *rval;
	char	   *v;

	if (nkdims > 1 || nkdims != nvdims)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	if (nkdims == 0)
		PG_RETURN_DATUM(CStringGetTextDatum("{}"));

	deconstruct_array_builtin(key_array, TEXTOID, &key_datums, &key_nulls, &key_count);
	deconstruct_array_builtin(val_array, TEXTOID, &val_datums, &val_nulls, &val_count);

	if (key_count != val_count)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("mismatched array dimensions")));

	initStringInfo(&result);

	appendStringInfoChar(&result, '{');

	for (i = 0; i < key_count; ++i)
	{
		if (key_nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for object key")));

		v = TextDatumGetCString(key_datums[i]);
		if (i > 0)
			appendStringInfoString(&result, ", ");
		escape_json(&result, v);
		appendStringInfoString(&result, " : ");
		pfree(v);
		if (val_nulls[i])
			appendStringInfoString(&result, "null");
		else
		{
			v = TextDatumGetCString(val_datums[i]);
			escape_json(&result, v);
			pfree(v);
		}
	}

	appendStringInfoChar(&result, '}');

	pfree(key_datums);
	pfree(key_nulls);
	pfree(val_datums);
	pfree(val_nulls);

	rval = cstring_to_text_with_len(result.data, result.len);
	pfree(result.data);

	PG_RETURN_TEXT_P(rval);
}


/*
 * 生成一个 JSON 字符串字面量，正确转义文本中的字符。
 */
void
escape_json(StringInfo buf, const char *str)
{
	const char *p;

	appendStringInfoCharMacro(buf, '"');
	for (p = str; *p; p++)
	{
		switch (*p)
		{
			case '\b':
				appendStringInfoString(buf, "\\b");
				break;
			case '\f':
				appendStringInfoString(buf, "\\f");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			case '"':
				appendStringInfoString(buf, "\\\"");
				break;
			case '\\':
				appendStringInfoString(buf, "\\\\");
				break;
			default:
				if ((unsigned char) *p < ' ')
					appendStringInfo(buf, "\\u%04x", (int) *p);
				else
					appendStringInfoCharMacro(buf, *p);
				break;
		}
	}
	appendStringInfoCharMacro(buf, '"');
}

/* 键唯一性检查的语义动作 */
static JsonParseErrorType
json_unique_object_start(void *_state)
{
	JsonUniqueParsingState *state = _state;
	JsonUniqueStackEntry *entry;

	if (!state->unique)
		return JSON_SUCCESS;

	/* 把对象条目压栈 */
	entry = palloc(sizeof(*entry));
	entry->object_id = state->id_counter++;
	entry->parent = state->stack;
	state->stack = entry;

	return JSON_SUCCESS;
}

static JsonParseErrorType
json_unique_object_end(void *_state)
{
	JsonUniqueParsingState *state = _state;
	JsonUniqueStackEntry *entry;

	if (!state->unique)
		return JSON_SUCCESS;

	entry = state->stack;
	state->stack = entry->parent;	/* 弹出对象 */
	pfree(entry);
	return JSON_SUCCESS;
}

static JsonParseErrorType
json_unique_object_field_start(void *_state, char *field, bool isnull)
{
	JsonUniqueParsingState *state = _state;
	JsonUniqueStackEntry *entry;

	if (!state->unique)
		return JSON_SUCCESS;

	/* 在当前对象中查找键冲突 */
	if (json_unique_check_key(&state->check, field, state->stack->object_id))
		return JSON_SUCCESS;

	state->unique = false;

	/* 弹出所有条目 */
	while ((entry = state->stack))
	{
		state->stack = entry->parent;
		pfree(entry);
	}
	return JSON_SUCCESS;
}

/* 校验 JSON 文本并额外检查键唯一性 */
bool
json_validate(text *json, bool check_unique_keys, bool throw_error)
{
	JsonLexContext *lex = makeJsonLexContext(json, check_unique_keys);
	JsonSemAction uniqueSemAction = {0};
	JsonUniqueParsingState state;
	JsonParseErrorType result;

	if (check_unique_keys)
	{
		state.lex = lex;
		state.stack = NULL;
		state.id_counter = 0;
		state.unique = true;
		json_unique_check_init(&state.check);

		uniqueSemAction.semstate = &state;
		uniqueSemAction.object_start = json_unique_object_start;
		uniqueSemAction.object_field_start = json_unique_object_field_start;
		uniqueSemAction.object_end = json_unique_object_end;
	}

	result = pg_parse_json(lex, check_unique_keys ? &uniqueSemAction : &nullSemAction);

	if (result != JSON_SUCCESS)
	{
		if (throw_error)
			json_errsave_error(result, lex, NULL);

		return false;			/* json 无效 */
	}

	if (check_unique_keys && !state.unique)
	{
		if (throw_error)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_JSON_OBJECT_KEY_VALUE),
					 errmsg("duplicate JSON object key value")));

		return false;			/* 键不唯一 */
	}

	return true;				/* ok */
}

/*
 * SQL 函数 json_typeof(json) -> text
 *
 * 以 TEXT 返回最外层 JSON 值的类型。可能的类型为
 * "object"、"array"、"string"、"number"、"boolean"、"null"
 *
 * 对给定值执行一次 json_lex() 调用来取得其第一个词位。
 * 该初始词位唯一决定值的类型。由于我们的输入必
 * 已经先经 json_in() 或 json_recv() 校验过，因此初始
 * 词位绝不应当是 JSON_TOKEN_OBJECT_END、JSON_TOKEN_ARRAY_END、
 * JSON_TOKEN_COLON、JSON_TOKEN_COMMA、JSON_TOKEN_END
 */
Datum
json_typeof(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_PP(0);
	JsonLexContext *lex = makeJsonLexContext(json, false);
	char	   *type;
	JsonTokenType tok;
	JsonParseErrorType result;

	/* 从输入精确解析一个词位并检查其类型。 */
	result = json_lex(lex);
	if (result != JSON_SUCCESS)
		json_errsave_error(result, lex, NULL);
	tok = lex->token_type;

	switch (tok)
	{
		case JSON_TOKEN_OBJECT_START:
			type = "object";
			break;
		case JSON_TOKEN_ARRAY_START:
			type = "array";
			break;
		case JSON_TOKEN_STRING:
			type = "string";
			break;
		case JSON_TOKEN_NUMBER:
			type = "number";
			break;
		case JSON_TOKEN_TRUE:
		case JSON_TOKEN_FALSE:
			type = "boolean";
			break;
		case JSON_TOKEN_NULL:
			type = "null";
			break;
		default:
			elog(ERROR, "unexpected json token: %d", tok);
	}

	PG_RETURN_TEXT_P(cstring_to_text(type));
}
