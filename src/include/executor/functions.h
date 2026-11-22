/*-------------------------------------------------------------------------
 *
 * functions.h
 *		Declarations for execution of SQL-language functions.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/functions.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FUNCTIONS_H
#define FUNCTIONS_H

#include "nodes/execnodes.h"
#include "tcop/dest.h"

/*
 * Data structure needed by the parser callback hooks to resolve parameter
 * references during parsing of a SQL function's body.  This is separate from
 * SQLFunctionCache since we sometimes do parsing separately from execution.
 */
typedef struct SQLFunctionParseInfo
{
	char	   *fname;			/*  函数名  */
	int			nargs;			/*  输入参数个数  */
	Oid		   *argtypes;		/*  已解析的输入参数类型  */
	char	  **argnames;		/*  输入参数名;无则 NULL  */
	/*  注意 argnames[i] 可为 NULL,若某些参数无名  */
	Oid			collation;		/*  函数输入排序规则,若已知  */
} SQLFunctionParseInfo;

typedef SQLFunctionParseInfo *SQLFunctionParseInfoPtr;

extern Datum fmgr_sql(PG_FUNCTION_ARGS);

extern SQLFunctionParseInfoPtr prepare_sql_fn_parse_info(HeapTuple procedureTuple,
														 Node *call_expr,
														 Oid inputCollation);

extern void sql_fn_parser_setup(struct ParseState *pstate,
								SQLFunctionParseInfoPtr pinfo);

extern void check_sql_fn_statements(List *queryTreeLists);

extern bool check_sql_fn_retval(List *queryTreeLists,
								Oid rettype, TupleDesc rettupdesc,
								bool insertDroppedCols,
								List **resultTargetList);

extern bool check_sql_fn_retval_ext(List *queryTreeLists,
									Oid rettype, TupleDesc rettupdesc,
									char prokind,
									bool insertDroppedCols,
									List **resultTargetList);

extern DestReceiver *CreateSQLFunctionDestReceiver(void);

#endif							/* FUNCTIONS_H */
