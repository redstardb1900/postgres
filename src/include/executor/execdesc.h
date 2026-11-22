/*-------------------------------------------------------------------------
 *
 * execdesc.h
 *	  plan and query descriptor accessor macros used by the executor
 *	  and related modules.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/execdesc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECDESC_H
#define EXECDESC_H

#include "nodes/execnodes.h"
#include "tcop/dest.h"


/* ----------------
 *		query descriptor:
 *
 *	a QueryDesc encapsulates everything that the executor
 *	needs to execute the query.
 *
 *	For the convenience of SQL-language functions, we also support QueryDescs
 *	containing utility statements; these must not be passed to the executor
 *	however.
 * ---------------------
 */
typedef struct QueryDesc
{
	/*  这些字段由 CreateQueryDesc 提供  */
	CmdType		operation;		/*  CMD_SELECT, CMD_UPDATE 等  */
	PlannedStmt *plannedstmt;	/*  规划器输出(也可以是 utility)  */
	const char *sourceText;		/*  查询的源文本  */
	Snapshot	snapshot;		/*  查询使用的快照  */
	Snapshot	crosscheck_snapshot;	/* crosscheck for RI update/delete */
	DestReceiver *dest;			/*  元组输出目的地  */
	ParamListInfo params;		/*  传入的参数值  */
	QueryEnvironment *queryEnv; /*  传入的查询环境  */
	int			instrument_options; /* OR of InstrumentOption flags */

	/* These fields are set by ExecutorStart */
	TupleDesc	tupDesc;		/*  结果元组的描述符  */
	EState	   *estate;			/*  执行器查询级状态  */
	PlanState  *planstate;		/*  每计划节点状态树  */

	/* This field is set by ExecutePlan */
	bool		already_executed;	/*  先前已执行则为真  */

	/*  核心系统总置 NULL,但插件可改  */
	struct Instrumentation *totaltime;	/* total time spent in ExecutorRun */
} QueryDesc;

/* in pquery.c */
extern QueryDesc *CreateQueryDesc(PlannedStmt *plannedstmt,
								  const char *sourceText,
								  Snapshot snapshot,
								  Snapshot crosscheck_snapshot,
								  DestReceiver *dest,
								  ParamListInfo params,
								  QueryEnvironment *queryEnv,
								  int instrument_options);

extern void FreeQueryDesc(QueryDesc *qdesc);

#endif							/* EXECDESC_H  */
