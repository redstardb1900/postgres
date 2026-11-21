/*-------------------------------------------------------------------------
 *
 * utility.h
 *	  utility.c 的原型。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/tcop/utility.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UTILITY_H
#define UTILITY_H

#include "tcop/cmdtag.h"
#include "tcop/tcopprot.h"

typedef enum
{
	PROCESS_UTILITY_TOPLEVEL,	/* 顶层交互式命令 */
	PROCESS_UTILITY_QUERY,		/* 完整查询，但非顶层 */
	PROCESS_UTILITY_QUERY_NONATOMIC,	/* 完整查询，非原子
										 * 执行上下文 */
	PROCESS_UTILITY_SUBCOMMAND	/* 查询的一部分 */
} ProcessUtilityContext;

/* 从 ALTER TABLE 递归时所需信息 */
typedef struct AlterTableUtilityContext
{
	PlannedStmt *pstmt;			/* 外层 ALTER TABLE 的 PlannedStmt */
	const char *queryString;	/* 其查询串 */
	Oid			relid;			/* ALTER 目标表 OID */
	ParamListInfo params;		/* ALTER TABLE 可用的任何参数 */
	QueryEnvironment *queryEnv; /* ALTER TABLE 的执行环境 */
} AlterTableUtilityContext;

/*
 * 这些常量描述某命令只读的程度。
 * 。
 *
 * COMMAND_OK_IN_READ_ONLY_TXN 表示即使 XactReadOnly
 * 置位也允许该命令。应给不改变数据库状态（数据或
 * 模式）以致影响 pg_dump 输出的命令置此位。
 * 。
 *
 * COMMAND_OK_IN_PARALLEL_MODE 表示并行模式下也允许。
 * 禁止写元组，以及任何可能迷惑协作进程的事。
 * 。
 *
 * COMMAND_OK_IN_RECOVERY 表示恢复中也允许。不能写
 * WAL，也不能做危及将来主库 WAL 重放的事。
 * 。
 */
#define COMMAND_OK_IN_READ_ONLY_TXN	0x0001
#define COMMAND_OK_IN_PARALLEL_MODE	0x0002
#define	COMMAND_OK_IN_RECOVERY		0x0004

/*
 * 若对所有目的都足够只读，则称命令严格只读。为清晰，
 * 也有常量表示完全不只读的命令。
 * 。
 */
#define COMMAND_IS_STRICTLY_READ_ONLY \
	(COMMAND_OK_IN_READ_ONLY_TXN | COMMAND_OK_IN_RECOVERY | \
	 COMMAND_OK_IN_PARALLEL_MODE)
#define COMMAND_IS_NOT_READ_ONLY	0

/* 插件在 ProcessUtility() 中取得控制的钩子 */
typedef void (*ProcessUtility_hook_type) (PlannedStmt *pstmt,
										  const char *queryString,
										  bool readOnlyTree,
										  ProcessUtilityContext context,
										  ParamListInfo params,
										  QueryEnvironment *queryEnv,
										  DestReceiver *dest, QueryCompletion *qc);
extern PGDLLIMPORT ProcessUtility_hook_type ProcessUtility_hook;

extern void ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						   bool readOnlyTree,
						   ProcessUtilityContext context, ParamListInfo params,
						   QueryEnvironment *queryEnv,
						   DestReceiver *dest, QueryCompletion *qc);
extern void standard_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
									bool readOnlyTree,
									ProcessUtilityContext context, ParamListInfo params,
									QueryEnvironment *queryEnv,
									DestReceiver *dest, QueryCompletion *qc);

extern void ProcessUtilityForAlterTable(Node *stmt,
										AlterTableUtilityContext *context);

extern bool UtilityReturnsTuples(Node *parsetree);

extern TupleDesc UtilityTupleDescriptor(Node *parsetree);

extern Query *UtilityContainsQuery(Node *parsetree);

extern CommandTag CreateCommandTag(Node *parsetree);

static inline const char *
CreateCommandName(Node *parsetree)
{
	return GetCommandTagName(CreateCommandTag(parsetree));
}

extern LogStmtLevel GetCommandLogLevel(Node *parsetree);

extern bool CommandIsReadOnly(PlannedStmt *pstmt);

#endif							/* UTILITY_H */
