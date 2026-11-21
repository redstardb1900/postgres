/*-------------------------------------------------------------------------
 *
 * copy.h
 *	POSTGRES COPY 命令所用定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/copy.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_H
#define COPY_H

#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "tcop/dest.h"

/*
 * 表示是否应有表头行，以及表头是否必须
 * 与实际列名匹配（意味着为真）。
 */
typedef enum CopyHeaderChoice
{
	COPY_HEADER_FALSE = 0,
	COPY_HEADER_TRUE,
	COPY_HEADER_MATCH,
} CopyHeaderChoice;

/*
 * 保存已解析 COPY 选项的结构。除 freeze 外均与
 * 格式相关；freeze 本不属此，但与其他选项
 * 一并解析较方便。
 */
typedef struct CopyFormatOptions
{
	/* 来自 COPY 命令的参数 */
	int			file_encoding;	/* 文件或远端的字符编码，
								 * -1 表示未指定 */
	bool		binary;			/* 二进制？ */
	bool		freeze;			/* 装载时冻结行？ */
	bool		csv_mode;		/* 逗号分隔值格式？ */
	CopyHeaderChoice header_line;	/* 表头行？ */
	char	   *null_print;		/* NULL 标串(服务器编码!) */
	int			null_print_len; /* 同上长度 */
	char	   *null_print_client;	/* 已转文件编码的同上 */
	char	   *default_print;	/* DEFAULT 标记串 */
	int			default_print_len;	/* 同上长度 */
	char	   *delim;			/* 列分隔符（须为 1 字节） */
	char	   *quote;			/* CSV 引号（1 字节） */
	char	   *escape;			/* CSV 转义（1 字节） */
	List	   *force_quote;	/* 列名列表 */
	bool		force_quote_all;	/* FORCE_QUOTE *? */
	bool	   *force_quote_flags;	/* 每列 CSV FQ 标志 */
	List	   *force_notnull;	/* 列名列表 */
	bool	   *force_notnull_flags;	/* 每列 CSV FNN 标志 */
	List	   *force_null;		/* 列名列表 */
	bool	   *force_null_flags;	/* 每列 CSV FN 标志 */
	bool		convert_selectively;	/* 做选择性二进制转换？ */
	List	   *convert_select; /* 列名列表（可为 NIL） */
} CopyFormatOptions;

/* 在 commands/copy[from|to].c 中私有 */
typedef struct CopyFromStateData *CopyFromState;
typedef struct CopyToStateData *CopyToState;

typedef int (*copy_data_source_cb) (void *outbuf, int minread, int maxread);
typedef void (*copy_data_dest_cb) (void *data, int len);

extern void DoCopy(ParseState *pstate, const CopyStmt *stmt,
				   int stmt_location, int stmt_len,
				   uint64 *processed);

extern void ProcessCopyOptions(ParseState *pstate, CopyFormatOptions *opts_out, bool is_from, List *options);
extern CopyFromState BeginCopyFrom(ParseState *pstate, Relation rel, Node *whereClause,
								   const char *filename,
								   bool is_program, copy_data_source_cb data_source_cb, List *attnamelist, List *options);
extern void EndCopyFrom(CopyFromState cstate);
extern bool NextCopyFrom(CopyFromState cstate, ExprContext *econtext,
						 Datum *values, bool *nulls);
extern bool NextCopyFromRawFields(CopyFromState cstate,
								  char ***fields, int *nfields);
extern void CopyFromErrorCallback(void *arg);

extern uint64 CopyFrom(CopyFromState cstate);

extern DestReceiver *CreateCopyDestReceiver(void);

/*
 * 内部原型
 */
extern CopyToState BeginCopyTo(ParseState *pstate, Relation rel, RawStmt *raw_query,
							   Oid queryRelId, const char *filename, bool is_program,
							   copy_data_dest_cb data_dest_cb, List *attnamelist, List *options);
extern void EndCopyTo(CopyToState cstate);
extern uint64 DoCopyTo(CopyToState cstate);
extern List *CopyGetAttnums(TupleDesc tupDesc, Relation rel,
							List *attnamelist);

#endif							/* COPY_H */
