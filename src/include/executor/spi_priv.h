/*-------------------------------------------------------------------------
 *
 * spi_priv.h
 *				Server Programming Interface private declarations
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/spi_priv.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPI_PRIV_H
#define SPI_PRIV_H

#include "executor/spi.h"
#include "utils/queryenvironment.h"


#define _SPI_PLAN_MAGIC		569278163

typedef struct
{
	/*  当前结果  */
	uint64		processed;		/* by Executor */
	SPITupleTable *tuptable;	/*  正在构建的 tuptable  */

	/*  当前 Executor 调用启动时所在子事务  */
	SubTransactionId execSubid;

	/*  本执行上下文的资源  */
	slist_head	tuptables;		/* list of all live SPITupleTables */
	MemoryContext procCxt;		/*  过程上下文  */
	MemoryContext execCxt;		/* executor context */
	MemoryContext savedcxt;		/* context of SPI_connect's caller */
	SubTransactionId connectSubid;	/*  连接子事务的 ID  */
	QueryEnvironment *queryEnv; /*  为本 SPI 级设置的查询环境  */

	/*  事务管理支持  */
	bool		atomic;			/*  原子执行上下文,不允许
								 * 事务  */
	bool		internal_xact;	/*  SPI 管理的事务边界,跳过
								 * 清理  */

	/* saved values of API global variables for previous nesting level */
	uint64		outer_processed;
	SPITupleTable *outer_tuptable;
	int			outer_result;
} _SPI_connection;

/*
 * SPI plans have three states: saved, unsaved, or temporary.
 *
 * Ordinarily, the _SPI_plan struct itself as well as the argtypes array
 * are in a dedicated memory context identified by plancxt (which can be
 * really small).  All the other subsidiary state is in plancache entries
 * identified by plancache_list (note: the list cells themselves are in
 * plancxt).
 *
 * In an unsaved plan, the plancxt as well as the plancache entries' contexts
 * are children of the SPI procedure context, so they'll all disappear at
 * function exit.  plancache.c also knows that the plancache entries are
 * "unsaved", so it doesn't link them into its global list; hence they do
 * not respond to inval events.  This is OK since we are presumably holding
 * adequate locks to prevent other backends from messing with the tables.
 *
 * For a saved plan, the plancxt is made a child of CacheMemoryContext
 * since it should persist until explicitly destroyed.  Likewise, the
 * plancache entries will be under CacheMemoryContext since we tell
 * plancache.c to save them.  We rely on plancache.c to keep the cache
 * entries up-to-date as needed in the face of invalidation events.
 *
 * There are also "temporary" SPI plans, in which the _SPI_plan struct is
 * not even palloc'd but just exists in some function's local variable.
 * The plancache entries are unsaved and exist under the SPI executor context,
 * while additional data such as argtypes and list cells is loose in the SPI
 * executor context.  Such plans can be identified by having plancxt == NULL.
 *
 * We can also have "one-shot" SPI plans (which are typically temporary,
 * as described above).  These are meant to be executed once and discarded,
 * and various optimizations are made on the assumption of single use.
 * Note in particular that the CachedPlanSources within such an SPI plan
 * are not "complete" until execution.
 *
 * Note: if the original query string contained only whitespace and comments,
 * the plancache_list will be NIL and so there is no place to store the
 * query string.  We don't care about that, but we do care about the
 * argument type array, which is why it's seemingly-redundantly stored.
 */
typedef struct _SPI_plan
{
	int			magic;			/*  应等于 _SPI_PLAN_MAGIC  */
	bool		saved;			/* saved or unsaved plan? */
	bool		oneshot;		/* one-shot plan? */
	List	   *plancache_list; /* one CachedPlanSource per parsetree */
	MemoryContext plancxt;		/*  含 _SPI_plan 与数据的上下文  */
	RawParseMode parse_mode;	/* raw_parser() mode */
	int			cursor_options; /*  规划时使用的游标选项  */
	int			nargs;			/*  计划参数个数  */
	Oid		   *argtypes;		/* Argument types (NULL if nargs is 0) */
	ParserSetupHook parserSetup;	/*  替代参数规格方法  */
	void	   *parserSetupArg;
} _SPI_plan;

#endif							/* SPI_PRIV_H */
