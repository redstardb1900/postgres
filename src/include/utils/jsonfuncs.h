/*-------------------------------------------------------------------------
 *
 * jsonfuncs.h
 *	  处理 JSON 数据类型的函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/jsonfuncs.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSONFUNCS_H
#define JSONFUNCS_H

#include "common/jsonapi.h"
#include "utils/jsonb.h"

/*
 * iterate_json(b)_values 的标志类型，用以指定要从
 * json(b) 文档迭代哪些元素。
 */
typedef enum JsonToIndex
{
	jtiKey = 0x01,
	jtiString = 0x02,
	jtiNumeric = 0x04,
	jtiBool = 0x08,
	jtiAll = jtiKey | jtiString | jtiNumeric | jtiBool
} JsonToIndex;

/*  将作用于 iterate_json(b)_values 中每个值的动作  */
typedef void (*JsonIterateStringValuesAction) (void *state, char *elem_value, int elem_len);

/*  将作用于 transform_json(b)_values 中每个值的动作  */
typedef text *(*JsonTransformStringValuesAction) (void *state, char *elem_value, int elem_len);

/*  自文本构建 JsonLexContext  */
extern JsonLexContext *makeJsonLexContext(text *json, bool need_escapes);

/*  尝试解析 json，失败则 errsave(escontext)  */
extern bool pg_parse_json_or_errsave(JsonLexContext *lex, JsonSemAction *sem,
									 struct Node *escontext);

#define pg_parse_json_or_ereport(lex, sem) \
	(void) pg_parse_json_or_errsave(lex, sem, NULL)

/*  保存 json 词法/解析错误  */
extern void json_errsave_error(JsonParseErrorType error, JsonLexContext *lex,
							   struct Node *escontext);

/*  取首 JSON 词元  */
extern JsonTokenType json_get_first_token(text *json, bool throw_error);

extern uint32 parse_jsonb_index_flags(Jsonb *jb);
extern void iterate_jsonb_values(Jsonb *jb, uint32 flags, void *state,
								 JsonIterateStringValuesAction action);
extern void iterate_json_values(text *json, uint32 flags, void *action_state,
								JsonIterateStringValuesAction action);
extern Jsonb *transform_jsonb_string_values(Jsonb *jsonb, void *action_state,
											JsonTransformStringValuesAction transform_action);
extern text *transform_json_string_values(text *json, void *action_state,
										  JsonTransformStringValuesAction transform_action);

#endif
