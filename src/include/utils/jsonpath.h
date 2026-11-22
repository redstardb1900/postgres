/*-------------------------------------------------------------------------
 *
 * jsonpath.h
 *	jsonpath 数据类型的定义
 *
 * Copyright (c) 2019-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	src/include/utils/jsonpath.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSONPATH_H
#define JSONPATH_H

#include "fmgr.h"
#include "nodes/pg_list.h"
#include "utils/jsonb.h"

typedef struct
{
	int32		vl_len_;		/* varlena 头（勿直接触碰！） */
	uint32		header;			/* 版本与标志（见下） */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} JsonPath;

#define JSONPATH_VERSION	(0x01)
#define JSONPATH_LAX		(0x80000000)
#define JSONPATH_HDRSZ		(offsetof(JsonPath, data))

static inline JsonPath *
DatumGetJsonPathP(Datum d)
{
	return (JsonPath *) PG_DETOAST_DATUM(d);
}

static inline JsonPath *
DatumGetJsonPathPCopy(Datum d)
{
	return (JsonPath *) PG_DETOAST_DATUM_COPY(d);
}

#define PG_GETARG_JSONPATH_P(x)			DatumGetJsonPathP(PG_GETARG_DATUM(x))
#define PG_GETARG_JSONPATH_P_COPY(x)	DatumGetJsonPathPCopy(PG_GETARG_DATUM(x))
#define PG_RETURN_JSONPATH_P(p)			PG_RETURN_POINTER(p)

#define jspIsScalar(type) ((type) >= jpiNull && (type) <= jpiBool)

/*
 * jsonpath 表达式的节点类型
 */
typedef enum JsonPathItemType
{
	jpiNull = jbvNull,			/* NULL 字面 */
	jpiString = jbvString,		/* string 字面 */
	jpiNumeric = jbvNumeric,	/* 数值字面量 */
	jpiBool = jbvBool,			/* 布尔字面：TRUE/FALSE */
	jpiAnd,						/* 谓词 && 谓词 */
	jpiOr,						/* 谓词 || 谓词 */
	jpiNot,						/* ! 谓词 */
	jpiIsUnknown,				/* (谓词) IS UNKNOWN */
	jpiEqual,					/* expr == expr */
	jpiNotEqual,				/* expr != expr */
	jpiLess,					/* expr < expr */
	jpiGreater,					/* expr > expr */
	jpiLessOrEqual,				/* expr <= expr */
	jpiGreaterOrEqual,			/* expr >= expr */
	jpiAdd,						/* expr + expr */
	jpiSub,						/* expr - expr */
	jpiMul,						/* expr * expr */
	jpiDiv,						/* expr / expr */
	jpiMod,						/* expr % expr */
	jpiPlus,					/* + expr */
	jpiMinus,					/* - expr */
	jpiAnyArray,				/* [*] */
	jpiAnyKey,					/* .* */
	jpiIndexArray,				/* [下标, ...] */
	jpiAny,						/* .** */
	jpiKey,						/* .key */
	jpiCurrent,					/* @ */
	jpiRoot,					/* $ */
	jpiVariable,				/* $variable */
	jpiFilter,					/* ? (谓词) */
	jpiExists,					/* EXISTS (expr) 谓词 */
	jpiType,					/* .type() 项方法 */
	jpiSize,					/* .size() 项方法 */
	jpiAbs,						/* .abs() 项方法 */
	jpiFloor,					/* .floor() 项方法 */
	jpiCeiling,					/* .ceiling() 项方法 */
	jpiDouble,					/* .double() 项方法 */
	jpiDatetime,				/* .datetime() 项方法 */
	jpiKeyValue,				/* .keyvalue() 项方法 */
	jpiSubscript,				/* 数组下标：'expr' 或 'expr TO expr' */
	jpiLast,					/* LAST 数组下标 */
	jpiStartsWith,				/* STARTS WITH 谓词 */
	jpiLikeRegex,				/* LIKE_REGEX 谓词 */
} JsonPathItemType;

/* LIKE_REGEX 谓词的 XQuery 正则模式标志 */
#define JSP_REGEX_ICASE		0x01	/* i：忽略大小写 */
#define JSP_REGEX_DOTALL	0x02	/* s 标志，点匹配换行 */
#define JSP_REGEX_MLINE		0x04	/* m：^/$ 匹配换行 */
#define JSP_REGEX_WSPACE	0x08	/* x 标志，忽略模式中空白 */
#define JSP_REGEX_QUOTE		0x10	/* q 标志，无特殊字符 */

/*
 * 解析/构造二进制值的支持函数。
 * 与许多其他表达式表示不同，首个/主节点
 * 不是运算而是表达式的左操作数。这样可在
 * jsonb 中廉价沿路径下行，再对右操作数
 * 执行运算符。
 */

typedef struct JsonPathItem
{
	JsonPathItemType type;

	/* 基址到下一节点的位置 */
	int32		nextPos;

	/*
	 * 指向 JsonPath 值中当前节点的指针，当前节点的
	 * 位置均相对此基址
	 */
	char	   *base;

	union
	{
		/* 经典双操作数运算符：and、or 等 */
		struct
		{
			int32		left;
			int32		right;
		}			args;

		/* 任意一元运算 */
		int32		arg;

		/* jpiIndexArray 的存储：数组下标 */
		struct
		{
			int32		nelems;
			struct
			{
				int32		from;
				int32		to;
			}		   *elems;
		}			array;

		/* jpiAny 层级 */
		struct
		{
			uint32		first;
			uint32		last;
		}			anybounds;

		struct
		{
			char	   *data;	/* 用于 bool/numeric/string/key */
			int32		datalen;	/* 仅 string/key 时填充 */
		}			value;

		struct
		{
			int32		expr;
			char	   *pattern;
			int32		patternlen;
			uint32		flags;
		}			like_regex;
	}			content;
} JsonPathItem;

#define jspHasNext(jsp) ((jsp)->nextPos > 0)

extern void jspInit(JsonPathItem *v, JsonPath *js);
extern void jspInitByBuffer(JsonPathItem *v, char *base, int32 pos);
extern bool jspGetNext(JsonPathItem *v, JsonPathItem *a);
extern void jspGetArg(JsonPathItem *v, JsonPathItem *a);
extern void jspGetLeftArg(JsonPathItem *v, JsonPathItem *a);
extern void jspGetRightArg(JsonPathItem *v, JsonPathItem *a);
extern Numeric jspGetNumeric(JsonPathItem *v);
extern bool jspGetBool(JsonPathItem *v);
extern char *jspGetString(JsonPathItem *v, int32 *len);
extern bool jspGetArraySubscript(JsonPathItem *v, JsonPathItem *from,
								 JsonPathItem *to, int i);

extern const char *jspOperationName(JsonPathItemType type);

/*
 * 解析支持数据结构。
 */

typedef struct JsonPathParseItem JsonPathParseItem;

struct JsonPathParseItem
{
	JsonPathItemType type;
	JsonPathParseItem *next;	/* 下一项 */

	union
	{

		/* 经典双操作数运算符：and、or 等 */
		struct
		{
			JsonPathParseItem *left;
			JsonPathParseItem *right;
		}			args;

		/* 任意一元运算 */
		JsonPathParseItem *arg;

		/* jpiIndexArray 的存储：数组下标 */
		struct
		{
			int			nelems;
			struct
			{
				JsonPathParseItem *from;
				JsonPathParseItem *to;
			}		   *elems;
		}			array;

		/* jpiAny 层级 */
		struct
		{
			uint32		first;
			uint32		last;
		}			anybounds;

		struct
		{
			JsonPathParseItem *expr;
			char	   *pattern;	/* 可能非以空字符结尾 */
			uint32		patternlen;
			uint32		flags;
		}			like_regex;

		/* 标量 */
		Numeric numeric;
		bool		boolean;
		struct
		{
			uint32		len;
			char	   *val;	/* 可能非以空字符结尾 */
		}			string;
	}			value;
};

typedef struct JsonPathParseResult
{
	JsonPathParseItem *expr;
	bool		lax;
} JsonPathParseResult;

extern JsonPathParseResult *parsejsonpath(const char *str, int len,
										  struct Node *escontext);

extern bool jspConvertRegexFlags(uint32 xflags, int *result,
								 struct Node *escontext);


#endif
