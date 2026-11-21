/*-------------------------------------------------------------------------
 *
 * prepare.h
 *	PREPARE、EXECUTE、DEALLOCATE 命令与预备语句存储
 *
 *
 * Copyright (c) 2002-2023, PostgreSQL Global Development Group
 *
 * src/include/commands/prepare.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREPARE_H
#define PREPARE_H

#include "commands/explain.h"
#include "datatype/timestamp.h"
#include "utils/plancache.h"

/*
 * 表示预备语句的数据结构。现仅为 plancache
 * 条目上的薄包装——主要增添是名字。
 *
 *
 * 注意：一切附属存储住在所引用的 plancache 条目中。
 */
typedef struct
{
	/* dynahash.c 要求键为第一字段 */
	char		stmt_name[NAMEDATALEN];
	CachedPlanSource *plansource;	/* 实际缓存的计划 */
	bool		from_sql;		/* 经 SQL 预备，非 FE/BE 协议？ */
	TimestampTz prepare_time;	/* 语句被预备的时间 */
} PreparedStatement;


/* 实用语句 PREPARE、EXECUTE、DEALLOCATE、EXPLAIN EXECUTE */
extern void PrepareQuery(ParseState *pstate, PrepareStmt *stmt,
						 int stmt_location, int stmt_len);
extern void ExecuteQuery(ParseState *pstate,
						 ExecuteStmt *stmt, IntoClause *intoClause,
						 ParamListInfo params,
						 DestReceiver *dest, QueryCompletion *qc);
extern void DeallocateQuery(DeallocateStmt *stmt);
extern void ExplainExecuteQuery(ExecuteStmt *execstmt, IntoClause *into,
								ExplainState *es, const char *queryString,
								ParamListInfo params, QueryEnvironment *queryEnv);

/* 对已存预备语句的底层访问 */
extern void StorePreparedStatement(const char *stmt_name,
								   CachedPlanSource *plansource,
								   bool from_sql);
extern PreparedStatement *FetchPreparedStatement(const char *stmt_name,
												 bool throwError);
extern void DropPreparedStatement(const char *stmt_name, bool showError);
extern TupleDesc FetchPreparedStatementResultDesc(PreparedStatement *stmt);
extern List *FetchPreparedStatementTargetList(PreparedStatement *stmt);

extern void DropAllPreparedStatements(void);

#endif							/* PREPARE_H */
