/*-------------------------------------------------------------------------
 *
 * jsonapi.h
 *	  JSON API 支持的声明。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/jsonapi.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSONAPI_H
#define JSONAPI_H

#include "lib/stringinfo.h"

typedef enum JsonTokenType
{
	JSON_TOKEN_INVALID,
	JSON_TOKEN_STRING,
	JSON_TOKEN_NUMBER,
	JSON_TOKEN_OBJECT_START,
	JSON_TOKEN_OBJECT_END,
	JSON_TOKEN_ARRAY_START,
	JSON_TOKEN_ARRAY_END,
	JSON_TOKEN_COMMA,
	JSON_TOKEN_COLON,
	JSON_TOKEN_TRUE,
	JSON_TOKEN_FALSE,
	JSON_TOKEN_NULL,
	JSON_TOKEN_END
} JsonTokenType;

typedef enum JsonParseErrorType
{
	JSON_SUCCESS,
	JSON_ESCAPING_INVALID,
	JSON_ESCAPING_REQUIRED,
	JSON_EXPECTED_ARRAY_FIRST,
	JSON_EXPECTED_ARRAY_NEXT,
	JSON_EXPECTED_COLON,
	JSON_EXPECTED_END,
	JSON_EXPECTED_JSON,
	JSON_EXPECTED_MORE,
	JSON_EXPECTED_OBJECT_FIRST,
	JSON_EXPECTED_OBJECT_NEXT,
	JSON_EXPECTED_STRING,
	JSON_INVALID_TOKEN,
	JSON_UNICODE_CODE_POINT_ZERO,
	JSON_UNICODE_ESCAPE_FORMAT,
	JSON_UNICODE_HIGH_ESCAPE,
	JSON_UNICODE_UNTRANSLATABLE,
	JSON_UNICODE_HIGH_SURROGATE,
	JSON_UNICODE_LOW_SURROGATE,
	JSON_SEM_ACTION_FAILED		/*  错误应已报告  */
} JsonParseErrorType;


/*
 * 本结构全部字段应视为只读。
 *
 * 若 strval 非空，则含字符串词元的去转义值。
 * 否则多数字段名应自明。
 * 略
 *
 * line_number 与 line_start 主要为解析器错误
 * 报告例程所用。
 * token_terminator 与 prev_token_terminator 指向 AFTER
 * 词元结束后的字符，即若用空终止串则会有
 * 空字节之处。
 */
typedef struct JsonLexContext
{
	char	   *input;
	int			input_length;
	int			input_encoding;
	char	   *token_start;
	char	   *token_terminator;
	char	   *prev_token_terminator;
	JsonTokenType token_type;
	int			lex_level;
	int			line_number;	/*  行号，从 1 起  */
	char	   *line_start;		/*  该行在输入中的起点  */
	StringInfo	strval;
} JsonLexContext;

typedef JsonParseErrorType (*json_struct_action) (void *state);
typedef JsonParseErrorType (*json_ofield_action) (void *state, char *fname, bool isnull);
typedef JsonParseErrorType (*json_aelem_action) (void *state, bool isnull);
typedef JsonParseErrorType (*json_scalar_action) (void *state, char *token, JsonTokenType tokentype);


/*
 * 解析 JSON 时用的语义动作结构。
 *
 * 任一动作可为 NULL，则该点什么也不做。
 * semstate 也可为 NULL。全 NULL 结构等于纯解析
 * 无副作用，正是 json 输入例程所为。
 * 略
 *
 * 传给这些动作的 'fname' 与 'token' 串为 palloc。
 * 解析器不再释放或使用，动作函数可自行处置。
 * 略
 *
 * 全部动作函数返回 JsonParseErrorType。结果非
 * JSON_SUCCESS 则放弃解析并返回该错误码。
 * 若为 JSON_SEM_ACTION_FAILED，动作函数负责以
 * 适当方式报告错误。
 */
typedef struct JsonSemAction
{
	void	   *semstate;
	json_struct_action object_start;
	json_struct_action object_end;
	json_struct_action array_start;
	json_struct_action array_end;
	json_ofield_action object_field_start;
	json_ofield_action object_field_end;
	json_aelem_action array_element_start;
	json_aelem_action array_element_end;
	json_scalar_action scalar;
} JsonSemAction;

/*
 * pg_parse_json 解析 lex 中的串，在适当时点调用
 * 略
 * 若需访问词法器状态，应把其指针作为 semstate
 * 略
 * 也不做，只继续。
 * 略
 * 略
 */
extern JsonParseErrorType pg_parse_json(JsonLexContext *lex,
										JsonSemAction *sem);

/*  用于纯校验的空动作对象  */
extern PGDLLIMPORT JsonSemAction nullSemAction;

/*
 * json_count_array_elements 做快速二次解析以确定
 * 所传数组词法上下文中的元素个数。应自 array_start
 * 动作调用。
 *
 * 返回值表示是否出错；元素个数写入 *elements
 * （仅当返回 JSON_SUCCESS）。
 * 略
 */
extern JsonParseErrorType json_count_array_elements(JsonLexContext *lex,
													int *elements);

/*
 * JsonLexContext 构造函数，可带或不带 strval。
 * 若提供，strval 含词元的去转义版。但这有性能
 * 代价，若不需要去转义词元应避免。
 * 略
 */
extern JsonLexContext *makeJsonLexContextCstringLen(char *json,
													int len,
													int encoding,
													bool need_escapes);

/* 略 */
extern JsonParseErrorType json_lex(JsonLexContext *lex);

/*  为 json 错误构造错误细节串  */
extern char *json_errdetail(JsonParseErrorType error, JsonLexContext *lex);

/*
 * 检查串是否为合法 JSON 数的工具函数。
 *
 * str 参数不必以空结尾。
 */
extern bool IsValidJsonNumber(const char *str, int len);

#endif							/* JSONAPI_H */
