/*-------------------------------------------------------------------------
 *
 * explain.h
 *	explain.c 的原型
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/explain.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_H
#define EXPLAIN_H

#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "parser/parse_node.h"

typedef enum ExplainFormat
{
	EXPLAIN_FORMAT_TEXT,
	EXPLAIN_FORMAT_XML,
	EXPLAIN_FORMAT_JSON,
	EXPLAIN_FORMAT_YAML
} ExplainFormat;

typedef struct ExplainWorkersState
{
	int			num_workers;	/* 计划所用工作进程数 */
	bool	   *worker_inited;	/* 每工人已初始化标志 */
	StringInfoData *worker_str; /* 每工作进程临时输出缓冲 */
	int		   *worker_state_save;	/* 每工作进程分组状态保存区 */
	StringInfo	prev_str;		/* 重定向时保存的输出缓冲 */
} ExplainWorkersState;

typedef struct ExplainState
{
	StringInfo	str;			/* 输出缓冲 */
	/* 选项 */
	bool		verbose;		/* 详尽 */
	bool		analyze;		/* 打印实际耗时 */
	bool		costs;			/* 打印估计代价 */
	bool		buffers;		/* 打印缓冲用量 */
	bool		wal;			/* 打印 WAL */
	bool		timing;			/* 打印详细节点计时 */
	bool		summary;		/* 打印总规划与执行计时 */
	bool		settings;		/* 打印已修改设置 */
	bool		generic;		/* 生成通用计划 */
	ExplainFormat format;		/* 输出格式 */
	/* 输出格式化状态——不随每棵新计划树复位 */
	int			indent;			/* 当前缩进级别 */
	List	   *grouping_stack; /* 格式相关分组状态 */
	/* 与当前计划树相关的状态（由 ExplainPrintPlan 填充） */
	PlannedStmt *pstmt;			/* 计划顶 */
	List	   *rtable;			/* 范围表 */
	List	   *rtable_names;	/* RTEs 别名 */
	List	   *deparse_cxt;	/* 反解析表达式的上下文列表 */
	Bitmapset  *printed_subplans;	/* 已打印 SubPlans 的 id */
	bool		hide_workers;	/* 发现不可见 Gather 则置位 */
	/* 与当前计划节点相关的状态 */
	ExplainWorkersState *workers_state; /* 并行计划时需要 */
} ExplainState;

/* 插件在 ExplainOneQuery() 中获得控制的钩子 */
typedef void (*ExplainOneQuery_hook_type) (Query *query,
										   int cursorOptions,
										   IntoClause *into,
										   ExplainState *es,
										   const char *queryString,
										   ParamListInfo params,
										   QueryEnvironment *queryEnv);
extern PGDLLIMPORT ExplainOneQuery_hook_type ExplainOneQuery_hook;

/* 插件在 explain_get_index_name() 中获得控制的钩子 */
typedef const char *(*explain_get_index_name_hook_type) (Oid indexId);
extern PGDLLIMPORT explain_get_index_name_hook_type explain_get_index_name_hook;


extern void ExplainQuery(ParseState *pstate, ExplainStmt *stmt,
						 ParamListInfo params, DestReceiver *dest);

extern ExplainState *NewExplainState(void);

extern TupleDesc ExplainResultDesc(ExplainStmt *stmt);

extern void ExplainOneUtility(Node *utilityStmt, IntoClause *into,
							  ExplainState *es, const char *queryString,
							  ParamListInfo params, QueryEnvironment *queryEnv);

extern void ExplainOnePlan(PlannedStmt *plannedstmt, IntoClause *into,
						   ExplainState *es, const char *queryString,
						   ParamListInfo params, QueryEnvironment *queryEnv,
						   const instr_time *planduration,
						   const BufferUsage *bufusage);

extern void ExplainPrintPlan(ExplainState *es, QueryDesc *queryDesc);
extern void ExplainPrintTriggers(ExplainState *es, QueryDesc *queryDesc);

extern void ExplainPrintJITSummary(ExplainState *es, QueryDesc *queryDesc);

extern void ExplainQueryText(ExplainState *es, QueryDesc *queryDesc);
extern void ExplainQueryParameters(ExplainState *es, ParamListInfo params, int maxlen);

extern void ExplainBeginOutput(ExplainState *es);
extern void ExplainEndOutput(ExplainState *es);
extern void ExplainSeparatePlans(ExplainState *es);

extern void ExplainPropertyList(const char *qlabel, List *data,
								ExplainState *es);
extern void ExplainPropertyListNested(const char *qlabel, List *data,
									  ExplainState *es);
extern void ExplainPropertyText(const char *qlabel, const char *value,
								ExplainState *es);
extern void ExplainPropertyInteger(const char *qlabel, const char *unit,
								   int64 value, ExplainState *es);
extern void ExplainPropertyUInteger(const char *qlabel, const char *unit,
									uint64 value, ExplainState *es);
extern void ExplainPropertyFloat(const char *qlabel, const char *unit,
								 double value, int ndigits, ExplainState *es);
extern void ExplainPropertyBool(const char *qlabel, bool value,
								ExplainState *es);

extern void ExplainOpenGroup(const char *objtype, const char *labelname,
							 bool labeled, ExplainState *es);
extern void ExplainCloseGroup(const char *objtype, const char *labelname,
							  bool labeled, ExplainState *es);

#endif							/* EXPLAIN_H */
