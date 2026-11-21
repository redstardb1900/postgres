/*-------------------------------------------------------------------------
 *
 * psqlscan.h
 *	  SQL 命令的词法扫描器
 *
 * 本词法器原属 psql，该渊源仍反映在文件名与
 * 函数/typedef 名中，现也可被其他前端程序使用。
 * 还可用兼容的附加词法器扩展，以处理程序专用
 * 反斜杠命令。
 * 略
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/psqlscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLSCAN_H
#define PSQLSCAN_H

#include "pqexpbuffer.h"


/*  词法器内部状态的抽象类型  */
typedef struct PsqlScanStateData *PsqlScanState;

/*  psql_scan() 的终止状态  */
typedef enum
{
	PSCAN_SEMICOLON,			/*  找到结束命令的分号  */
	PSCAN_BACKSLASH,			/*  找到反斜杠命令  */
	PSCAN_INCOMPLETE,			/*  行末，SQL 语句未完成  */
	PSCAN_EOL					/*  行末，SQL 可能已完成  */
} PsqlScanResult;

/*  psql_scan() 返回的提示类型  */
typedef enum _promptStatus
{
	PROMPT_READY,
	PROMPT_CONTINUE,
	PROMPT_COMMENT,
	PROMPT_SINGLEQUOTE,
	PROMPT_DOUBLEQUOTE,
	PROMPT_DOLLARQUOTE,
	PROMPT_PAREN,
	PROMPT_COPY
} promptStatus_t;

/*  get_variable() 回调的引号请求类型  */
typedef enum
{
	PQUOTE_PLAIN,				/*  只返回实际值  */
	PQUOTE_SQL_LITERAL,			/*  加引号使成合法 SQL 字面量  */
	PQUOTE_SQL_IDENT,			/*  若需则加引号使成 SQL 标识符  */
	PQUOTE_SHELL_ARG			/* 略 */
} PsqlScanQuoteType;

/*  词法器所用的回调函数  */
typedef struct PsqlScanCallbacks
{
	/*  取变量值，为可 free 的串；未知则为 NULL  */
	/*  本指针可为 NULL（若不需要变量替换）  */
	char	   *(*get_variable) (const char *varname, PsqlScanQuoteType quote,
								 void *passthrough);
} PsqlScanCallbacks;


extern PsqlScanState psql_scan_create(const PsqlScanCallbacks *callbacks);
extern void psql_scan_destroy(PsqlScanState state);

extern void psql_scan_set_passthrough(PsqlScanState state, void *passthrough);

extern void psql_scan_setup(PsqlScanState state,
							const char *line, int line_len,
							int encoding, bool std_strings);
extern void psql_scan_finish(PsqlScanState state);

extern PsqlScanResult psql_scan(PsqlScanState state,
								PQExpBuffer query_buf,
								promptStatus_t *prompt);

extern void psql_scan_reset(PsqlScanState state);

extern void psql_scan_reselect_sql_lexer(PsqlScanState state);

extern bool psql_scan_in_quote(PsqlScanState state);

#endif							/* PSQLSCAN_H */
