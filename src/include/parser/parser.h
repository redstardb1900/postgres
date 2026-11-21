/*-------------------------------------------------------------------------
 *
 * parser.h
 *		"原始"解析器定义（仅 flex 与 bison 阶段）
 *
 * 原始词法/解析函数的外部 API。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parser.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSER_H
#define PARSER_H

#include "nodes/parsenodes.h"


/*
 * RawParseMode 决定 raw_parser() 所接受字符串的形式：
 *
 * RAW_PARSE_DEFAULT：解析分号分隔的 SQL 命令列表，
 * 并返回 RawStmt 节点的 List。
 *
 * RAW_PARSE_TYPE_NAME：解析类型名，返回单元素 List
 * 内含 TypeName 节点。
 *
 * RAW_PARSE_PLPGSQL_EXPR：解析 PL/pgSQL 表达式，返回
 * 单元素 List，内含 RawStmt 节点。
 *
 * RAW_PARSE_PLPGSQL_ASSIGNn：解析 PL/pgSQL 赋值语句，
 * 返回单元素 List，内含 RawStmt 节点。"n"
 * 给出目标 ColumnRef 所含带点名称的个数。
 */
typedef enum
{
	RAW_PARSE_DEFAULT = 0,
	RAW_PARSE_TYPE_NAME,
	RAW_PARSE_PLPGSQL_EXPR,
	RAW_PARSE_PLPGSQL_ASSIGN1,
	RAW_PARSE_PLPGSQL_ASSIGN2,
	RAW_PARSE_PLPGSQL_ASSIGN3
} RawParseMode;

/*  backslash_quote GUC 的取值  */
typedef enum
{
	BACKSLASH_QUOTE_OFF,
	BACKSLASH_QUOTE_ON,
	BACKSLASH_QUOTE_SAFE_ENCODING
}			BackslashQuoteType;

/*  scan.l 中的 GUC 变量（个个都是馊主意 :-()  */
extern PGDLLIMPORT int backslash_quote;
extern PGDLLIMPORT bool escape_string_warning;
extern PGDLLIMPORT bool standard_conforming_strings;


/*  原始解析函数的主入口  */
extern List *raw_parser(const char *str, RawParseMode mode);

/*  gram.y 导出的工具函数（或许应放别处）  */
extern List *SystemFuncName(char *name);
extern TypeName *SystemTypeName(char *name);

#endif							/* PARSER_H */
