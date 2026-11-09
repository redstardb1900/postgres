/*-------------------------------------------------------------------------
 *
 * explain.c
 *	  解释查询执行计划
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/explain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/createas.h"
#include "commands/defrem.h"
#include "commands/prepare.h"
#include "executor/nodeHash.h"
#include "foreign/fdwapi.h"
#include "jit/jit.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "storage/bufmgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc_tables.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"
#include "utils/xml.h"


/* 插件获取 ExplainOneQuery() 控制权的钩子    */
ExplainOneQuery_hook_type ExplainOneQuery_hook = NULL;

/* 插件获取 explain_get_index_name() 控制权的钩子    */
explain_get_index_name_hook_type explain_get_index_name_hook = NULL;


/* ExplainXMLTag() OR 组合标志   */
#define X_OPENING 0
#define X_CLOSING 1
#define X_CLOSE_IMMEDIATE 2
#define X_NOWHITESPACE 4

static void ExplainOneQuery(Query *query, int cursorOptions,
							IntoClause *into, ExplainState *es,
							const char *queryString, ParamListInfo params,
							QueryEnvironment *queryEnv);
static void ExplainPrintJIT(ExplainState *es, int jit_flags,
							JitInstrumentation *ji);
static void report_triggers(ResultRelInfo *rInfo, bool show_relname,
							ExplainState *es);
static double elapsed_time(instr_time *starttime);
static bool ExplainPreScanNode(PlanState *planstate, Bitmapset **rels_used);
static void ExplainNode(PlanState *planstate, List *ancestors,
						const char *relationship, const char *plan_name,
						ExplainState *es);
static void show_plan_tlist(PlanState *planstate, List *ancestors,
							ExplainState *es);
static void show_expression(Node *node, const char *qlabel,
							PlanState *planstate, List *ancestors,
							bool useprefix, ExplainState *es);
static void show_qual(List *qual, const char *qlabel,
					  PlanState *planstate, List *ancestors,
					  bool useprefix, ExplainState *es);
static void show_scan_qual(List *qual, const char *qlabel,
						   PlanState *planstate, List *ancestors,
						   ExplainState *es);
static void show_upper_qual(List *qual, const char *qlabel,
							PlanState *planstate, List *ancestors,
							ExplainState *es);
static void show_sort_keys(SortState *sortstate, List *ancestors,
						   ExplainState *es);
static void show_incremental_sort_keys(IncrementalSortState *incrsortstate,
									   List *ancestors, ExplainState *es);
static void show_merge_append_keys(MergeAppendState *mstate, List *ancestors,
								   ExplainState *es);
static void show_agg_keys(AggState *astate, List *ancestors,
						  ExplainState *es);
static void show_grouping_sets(PlanState *planstate, Agg *agg,
							   List *ancestors, ExplainState *es);
static void show_grouping_set_keys(PlanState *planstate,
								   Agg *aggnode, Sort *sortnode,
								   List *context, bool useprefix,
								   List *ancestors, ExplainState *es);
static void show_group_keys(GroupState *gstate, List *ancestors,
							ExplainState *es);
static void show_sort_group_keys(PlanState *planstate, const char *qlabel,
								 int nkeys, int nPresortedKeys, AttrNumber *keycols,
								 Oid *sortOperators, Oid *collations, bool *nullsFirst,
								 List *ancestors, ExplainState *es);
static void show_sortorder_options(StringInfo buf, Node *sortexpr,
								   Oid sortOperator, Oid collation, bool nullsFirst);
static void show_tablesample(TableSampleClause *tsc, PlanState *planstate,
							 List *ancestors, ExplainState *es);
static void show_sort_info(SortState *sortstate, ExplainState *es);
static void show_incremental_sort_info(IncrementalSortState *incrsortstate,
									   ExplainState *es);
static void show_hash_info(HashState *hashstate, ExplainState *es);
static void show_memoize_info(MemoizeState *mstate, List *ancestors,
							  ExplainState *es);
static void show_hashagg_info(AggState *aggstate, ExplainState *es);
static void show_tidbitmap_info(BitmapHeapScanState *planstate,
								ExplainState *es);
static void show_instrumentation_count(const char *qlabel, int which,
									   PlanState *planstate, ExplainState *es);
static void show_foreignscan_info(ForeignScanState *fsstate, ExplainState *es);
static void show_eval_params(Bitmapset *bms_params, ExplainState *es);
static const char *explain_get_index_name(Oid indexId);
static void show_buffer_usage(ExplainState *es, const BufferUsage *usage,
							  bool planning);
static void show_wal_usage(ExplainState *es, const WalUsage *usage);
static void ExplainIndexScanDetails(Oid indexid, ScanDirection indexorderdir,
									ExplainState *es);
static void ExplainScanTarget(Scan *plan, ExplainState *es);
static void ExplainModifyTarget(ModifyTable *plan, ExplainState *es);
static void ExplainTargetRel(Plan *plan, Index rti, ExplainState *es);
static void show_modifytable_info(ModifyTableState *mtstate, List *ancestors,
								  ExplainState *es);
static void ExplainMemberNodes(PlanState **planstates, int nplans,
							   List *ancestors, ExplainState *es);
static void ExplainMissingMembers(int nplans, int nchildren, ExplainState *es);
static void ExplainSubPlans(List *plans, List *ancestors,
							const char *relationship, ExplainState *es);
static void ExplainCustomChildren(CustomScanState *css,
								  List *ancestors, ExplainState *es);
static ExplainWorkersState *ExplainCreateWorkersState(int num_workers);
static void ExplainOpenWorker(int n, ExplainState *es);
static void ExplainCloseWorker(int n, ExplainState *es);
static void ExplainFlushWorkersState(ExplainState *es);
static void ExplainProperty(const char *qlabel, const char *unit,
							const char *value, bool numeric, ExplainState *es);
static void ExplainOpenSetAsideGroup(const char *objtype, const char *labelname,
									 bool labeled, int depth, ExplainState *es);
static void ExplainSaveGroup(ExplainState *es, int depth, int *state_save);
static void ExplainRestoreGroup(ExplainState *es, int depth, int *state_save);
static void ExplainDummyGroup(const char *objtype, const char *labelname,
							  ExplainState *es);
static void ExplainXMLTag(const char *tagname, int flags, ExplainState *es);
static void ExplainIndentText(ExplainState *es);
static void ExplainJSONLineEnding(ExplainState *es);
static void ExplainYAMLLineStarting(ExplainState *es);
static void escape_yaml(StringInfo buf, const char *str);



/*
 * ExplainQuery  
 *	  执行 EXPLAIN 命令     
 */
void
ExplainQuery(ParseState *pstate, ExplainStmt *stmt,
			 ParamListInfo params, DestReceiver *dest)
{
	ExplainState *es = NewExplainState();
	TupOutputState *tstate;
	JumbleState *jstate = NULL;
	Query	   *query;
	List	   *rewritten;
	ListCell   *lc;
	bool		timing_set = false;
	bool		summary_set = false;

	/* 解析选项列表  */
	foreach(lc, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "analyze") == 0)
			es->analyze = defGetBoolean(opt);
		else if (strcmp(opt->defname, "verbose") == 0)
			es->verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "costs") == 0)
			es->costs = defGetBoolean(opt);
		else if (strcmp(opt->defname, "buffers") == 0)
			es->buffers = defGetBoolean(opt);
		else if (strcmp(opt->defname, "wal") == 0)
			es->wal = defGetBoolean(opt);
		else if (strcmp(opt->defname, "settings") == 0)
			es->settings = defGetBoolean(opt);
		else if (strcmp(opt->defname, "generic_plan") == 0)
			es->generic = defGetBoolean(opt);
		else if (strcmp(opt->defname, "timing") == 0)
		{
			timing_set = true;
			es->timing = defGetBoolean(opt);
		}
		else if (strcmp(opt->defname, "summary") == 0)
		{
			summary_set = true;
			es->summary = defGetBoolean(opt);
		}
		else if (strcmp(opt->defname, "format") == 0)
		{
			char	   *p = defGetString(opt);

			if (strcmp(p, "text") == 0)
				es->format = EXPLAIN_FORMAT_TEXT;
			else if (strcmp(p, "xml") == 0)
				es->format = EXPLAIN_FORMAT_XML;
			else if (strcmp(p, "json") == 0)
				es->format = EXPLAIN_FORMAT_JSON;
			else if (strcmp(p, "yaml") == 0)
				es->format = EXPLAIN_FORMAT_YAML;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized value for EXPLAIN option \"%s\": \"%s\"",
								opt->defname, p),
						 parser_errposition(pstate, opt->location)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized EXPLAIN option \"%s\"",
							opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	/* 检查 WAL 是否与 EXPLAIN ANALYZE 同用 */
	if (es->wal && !es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN option WAL requires ANALYZE")));

	/* 如果计时未被显式设置，则设置默认值     */
	es->timing = (timing_set) ? es->timing : es->analyze;

	/* 检查计时是否与 EXPLAIN ANALYZE 同用   */
	if (es->timing && !es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN option TIMING requires ANALYZE")));

	/* 检查 GENERIC_PLAN 未与 EXPLAIN ANALYZE 同用        */
	if (es->generic && es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN options ANALYZE and GENERIC_PLAN cannot be used together")));

	/* 如果摘要未被显式设置，则设置默认值      */
	es->summary = (summary_set) ? es->summary : es->analyze;

	query = castNode(Query, stmt->query);
	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);

	/*
	 * 解析分析已经完成，但仍需运行规则重写器。      
	 * 我们不执行 AcquireRewriteLocks：假定查询要么               
	 * 直接来自解析器，要么合适的锁由 plancache.c        
	 * 获取。   
	 */
	rewritten = QueryRewrite(castNode(Query, stmt->query));

	/* 输出开头样板内容 */
	ExplainBeginOutput(es);

	if (rewritten == NIL)
	{
		/*
		 * 在 INSTEAD NOTHING 情况下，至少要告知这一点。   
		 * 但在非文本格式中输出有分隔符，故无必要。      
		 */
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(es->str, "Query rewrites to nothing\n");
	}
	else
	{
		ListCell   *l;

		/* 解释每个计划 */
		foreach(l, rewritten)
		{
			ExplainOneQuery(lfirst_node(Query, l),
							CURSOR_OPT_PARALLEL_OK, NULL, es,
							pstate->p_sourcetext, params, pstate->p_queryEnv);

			/* 使用适当的分隔符分隔各计划      */
			if (lnext(rewritten, l) != NULL)
				ExplainSeparatePlans(es);
		}
	}

	/* 输出结尾样板内容 */
	ExplainEndOutput(es);
	Assert(es->indent == 0);

	/* 输出元组  */
	tstate = begin_tup_output_tupdesc(dest, ExplainResultDesc(stmt),
									  &TTSOpsVirtual);
	if (es->format == EXPLAIN_FORMAT_TEXT)
		do_text_output_multiline(tstate, es->str->data);
	else
		do_text_output_oneline(tstate, es->str->data);
	end_tup_output(tstate);

	pfree(es->str->data);
}

/*
 * 创建一个以默认选项初始化的新 ExplainState 结构体 
 */
ExplainState *
NewExplainState(void)
{
	ExplainState *es = (ExplainState *) palloc0(sizeof(ExplainState));

	/* 设置默认选项（大多数字段可保持为零）。*/
	es->costs = true;
	/* 准备输出缓冲区  */
	es->str = makeStringInfo();

	return es;
}

/*
 * ExplainResultDesc  
 *	  为 EXPLAIN 构造结果元组描述符      
 */
TupleDesc
ExplainResultDesc(ExplainStmt *stmt)
{
	TupleDesc	tupdesc;
	ListCell   *lc;
	Oid			result_type = TEXTOID;

	/* 检查 XML 格式选项     */
	foreach(lc, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "format") == 0)
		{
			char	   *p = defGetString(opt);

			if (strcmp(p, "xml") == 0)
				result_type = XMLOID;
			else if (strcmp(p, "json") == 0)
				result_type = JSONOID;
			else
				result_type = TEXTOID;
			/* 不要 "break"，因为 ExplainQuery 将用最后的值*/
		}
	}

	/* 需要一个表示单个 TEXT 或 XML 列的元组描述符      */
	tupdesc = CreateTemplateTupleDesc(1);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "QUERY PLAN",
					   result_type, -1, 0);
	return tupdesc;
}

/*
 * ExplainOneQuery  
 *	  输出一个 Query 的执行计划        
 *
 * "into" 为 NULL，除非我们正在解释 CreateTableAsStmt 的内容。   
 */
static void
ExplainOneQuery(Query *query, int cursorOptions,
				IntoClause *into, ExplainState *es,
				const char *queryString, ParamListInfo params,
				QueryEnvironment *queryEnv)
{
	/* 规划器无法处理实用语句             */
	if (query->commandType == CMD_UTILITY)
	{
		ExplainOneUtility(query->utilityStmt, into, es, queryString, params,
						  queryEnv);
		return;
	}

	/* 如果存在顾问插件，则交由它处理         */
	if (ExplainOneQuery_hook)
		(*ExplainOneQuery_hook) (query, cursorOptions, into, es,
								 queryString, params, queryEnv);
	else
	{
		PlannedStmt *plan;
		instr_time	planstart,
					planduration;
		BufferUsage bufusage_start,
					bufusage;

		if (es->buffers)
			bufusage_start = pgBufferUsage;
		INSTR_TIME_SET_CURRENT(planstart);

		/* 规划查询   */
		plan = pg_plan_query(query, queryString, cursorOptions, params);

		INSTR_TIME_SET_CURRENT(planduration);
		INSTR_TIME_SUBTRACT(planduration, planstart);

		/* 计算缓冲区计数器的差值。 */
		if (es->buffers)
		{
			memset(&bufusage, 0, sizeof(BufferUsage));
			BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);
		}

		/* 按需运行并生成输出           */
		ExplainOnePlan(plan, into, es, queryString, params, queryEnv,
					   &planduration, (es->buffers ? &bufusage : NULL));
	}
}

/*
 * ExplainOneUtility  
 *	  输出一个实用语句的执行计划               
 *	  （一般而言，实用语句没有计划，但有一些           
 *	  被当作特例处理）  
 *
 * "into" 为 NULL，除非我们正在解释 CreateTableAsStmt 的内容。   
 *
 * 此函数被导出是因为 prepare.c 会在                   
 * EXPLAIN EXECUTE 情况下回调它。那时我们要处理的是      
 * 计划缓存中的语句，因此必须确保不修改它。      
 */
void
ExplainOneUtility(Node *utilityStmt, IntoClause *into, ExplainState *es,
				  const char *queryString, ParamListInfo params,
				  QueryEnvironment *queryEnv)
{
	if (utilityStmt == NULL)
		return;

	if (IsA(utilityStmt, CreateTableAsStmt))
	{
		/*
		 * 我们必须重写其中包含的 SELECT，然后传回给      
		 * ExplainOneQuery。在 EXPLAIN EXECUTE 时复制以防万一。
		 */
		CreateTableAsStmt *ctas = (CreateTableAsStmt *) utilityStmt;
		List	   *rewritten;

		/*
		 * 检查该关系是否存在。在此阶段做此检查是          
		 * 以免规划或执行查询。    
		 */
		if (CreateTableAsRelExists(ctas))
		{
			if (ctas->objtype == OBJECT_TABLE)
				ExplainDummyGroup("CREATE TABLE AS", NULL, es);
			else if (ctas->objtype == OBJECT_MATVIEW)
				ExplainDummyGroup("CREATE MATERIALIZED VIEW", NULL, es);
			else
				elog(ERROR, "unexpected object type: %d",
					 (int) ctas->objtype);
			return;
		}

		rewritten = QueryRewrite(castNode(Query, copyObject(ctas->query)));
		Assert(list_length(rewritten) == 1);
		ExplainOneQuery(linitial_node(Query, rewritten),
						CURSOR_OPT_PARALLEL_OK, ctas->into, es,
						queryString, params, queryEnv);
	}
	else if (IsA(utilityStmt, DeclareCursorStmt))
	{
		/*
		 * DECLARE CURSOR 同理。    
		 *
		 * 注意，如果使用 EXPLAIN ANALYZE DECLARE CURSOR，我们   
		 * 将实际运行该查询。这与 8.3 之前的行为不同，  
		 * 但看起来比不运行查询更有用。不过，不会创建  
		 * 任何游标。     
		 */
		DeclareCursorStmt *dcs = (DeclareCursorStmt *) utilityStmt;
		List	   *rewritten;

		rewritten = QueryRewrite(castNode(Query, copyObject(dcs->query)));
		Assert(list_length(rewritten) == 1);
		ExplainOneQuery(linitial_node(Query, rewritten),
						dcs->options, NULL, es,
						queryString, params, queryEnv);
	}
	else if (IsA(utilityStmt, ExecuteStmt))
		ExplainExecuteQuery((ExecuteStmt *) utilityStmt, into, es,
							queryString, params, queryEnv);
	else if (IsA(utilityStmt, NotifyStmt))
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(es->str, "NOTIFY\n");
		else
			ExplainDummyGroup("Notify", NULL, es);
	}
	else
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(es->str,
								   "Utility statements have no plan structure\n");
		else
			ExplainDummyGroup("Utility Statement", NULL, es);
	}
}

/*
 * ExplainOnePlan  
 *		给定已规划查询，按需执行然后打印 EXPLAIN   
 *		输出        
 *
 * "into" 为 NULL，除非我们正在解释 CreateTableAsStmt 的内容，   
 * 此时执行该查询应导致创建那个表。                       
 *
 * 此函数被导出是因为它会被 prepare.c 在             
 * EXPLAIN EXECUTE 情况下回调，且索引顾问插件需要       
 * 调用它  
 */
void
ExplainOnePlan(PlannedStmt *plannedstmt, IntoClause *into, ExplainState *es,
			   const char *queryString, ParamListInfo params,
			   QueryEnvironment *queryEnv, const instr_time *planduration,
			   const BufferUsage *bufusage)
{
	DestReceiver *dest;
	QueryDesc  *queryDesc;
	instr_time	starttime;
	double		totaltime = 0;
	int			eflags;
	int			instrument_option = 0;

	Assert(plannedstmt->commandType != CMD_UTILITY);

	if (es->analyze && es->timing)
		instrument_option |= INSTRUMENT_TIMER;
	else if (es->analyze)
		instrument_option |= INSTRUMENT_ROWS;

	if (es->buffers)
		instrument_option |= INSTRUMENT_BUFFERS;
	if (es->wal)
		instrument_option |= INSTRUMENT_WAL;

	/*
	 * 我们始终收集整个语句的计时信息，即使节点级别的  
	 * 计时已关闭，所以此处不检查 es->timing。（即使     
	 * !es->summary 则可跳过，但增加复杂度不值。）    
	 */
	INSTR_TIME_SET_CURRENT(starttime);

	/*
	 * 使用命令 ID 已更新的快照，确保此查询能看到      
	 * 之前执行的任何查询的结果。    
	 */
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();

	/*
	 * 通常我们丢弃查询的输出，但如果正在解释 CREATE TABLE
	 * AS，最好使用适当的元组接收器。       
	 */
	if (into)
		dest = CreateIntoRelDestReceiver(into);
	else
		dest = None_Receiver;

	/* 为该查询创建 QueryDesc     */
	queryDesc = CreateQueryDesc(plannedstmt, queryString,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, params, queryEnv, instrument_option);

	/* 选择执行选项       */
	if (es->analyze)
		eflags = 0;				/* 默认的运行至完成标志  */
	else
		eflags = EXEC_FLAG_EXPLAIN_ONLY;
	if (es->generic)
		eflags |= EXEC_FLAG_EXPLAIN_GENERIC;
	if (into)
		eflags |= GetIntoRelEFlags(into);

	/* 调用 ExecutorStart 准备要执行的计划        */
	ExecutorStart(queryDesc, eflags);

	/* 如需要则执行计划收集统计信息   */
	if (es->analyze)
	{
		ScanDirection dir;

		/* EXPLAIN ANALYZE CREATE TABLE AS WITH NO DATA 很怪异*/
		if (into && into->skipData)
			dir = NoMovementScanDirection;
		else
			dir = ForwardScanDirection;

		/* 运行计划 */
		ExecutorRun(queryDesc, dir, 0, true);

		/* 也运行清理 */
		ExecutorFinish(queryDesc);

		/* 在打印完统计信息之前无法运行 ExecutorEnd……    */
		totaltime += elapsed_time(&starttime);
	}

	ExplainOpenGroup("Query", NULL, true, es);

	/* 创建计划树的文本转储   */
	ExplainPrintPlan(es, queryDesc);

	/* 显示规划的缓冲区用量*/
	if (bufusage)
	{
		ExplainOpenGroup("Planning", "Planning", true, es);
		show_buffer_usage(es, bufusage, true);
		ExplainCloseGroup("Planning", "Planning", true, es);
	}

	if (es->summary && planduration)
	{
		double		plantime = INSTR_TIME_GET_DOUBLE(*planduration);

		ExplainPropertyFloat("Planning Time", "ms", 1000.0 * plantime, 3, es);
	}

	/* 输出触发器运行时信息       */
	if (es->analyze)
		ExplainPrintTriggers(es, queryDesc);

	/*
	 * 输出 JIT 编译信息。此项与 es->costs 绑定，因为我们
	 * 不想在回归测试中显示它，因为不同构建选项会   
	 * 导致输出差异。后续阶段可能要将其从 COSTS 中          
	 * 分离出来。  
	 */
	if (es->costs)
		ExplainPrintJITSummary(es, queryDesc);

	/*
	 * 关闭查询并释放资源。这部分耗时也要计入             
	 * 总执行时间（尽管它应当非常短）。            
	 */
	INSTR_TIME_SET_CURRENT(starttime);

	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);

	PopActiveSnapshot();

	/* 我们需要一个 CCI，以防查询扩展为多个计划  */
	if (es->analyze)
		CommandCounterIncrement();

	totaltime += elapsed_time(&starttime);

	/*
	 * 只有当我们确实运行查询（即用户指定了 ANALYZE），
	 * 并且摘要报告已启用（用户可设置 SUMMARY OFF 使        
	 * 输出中不包含计时信息）时，才报告执行时间。默认  
	 * 情况下 ANALYZE 会将 SUMMARY 设为 true。        
	 */
	if (es->summary && es->analyze)
		ExplainPropertyFloat("Execution Time", "ms", 1000.0 * totaltime, 3,
							 es);

	ExplainCloseGroup("Query", NULL, true, es);
}

/*
 * ExplainPrintSettings  
 *    输出影响查询规划的已修改设置摘要。         
 */
static void
ExplainPrintSettings(ExplainState *es)
{
	int			num;
	struct config_generic **gucs;

	/* 如果未请求设置信息则退出                 */
	if (!es->settings)
		return;

	/* 请求相关设置的数组           */
	gucs = get_explain_guc_options(&num);

	if (es->format != EXPLAIN_FORMAT_TEXT)
	{
		ExplainOpenGroup("Settings", "Settings", true, es);

		for (int i = 0; i < num; i++)
		{
			char	   *setting;
			struct config_generic *conf = gucs[i];

			setting = GetConfigOptionByName(conf->name, NULL, true);

			ExplainPropertyText(conf->name, setting, es);
		}

		ExplainCloseGroup("Settings", "Settings", true, es);
	}
	else
	{
		StringInfoData str;

		/* TEXT 模式下无选项则不输出任何内容     */
		if (num <= 0)
			return;

		initStringInfo(&str);

		for (int i = 0; i < num; i++)
		{
			char	   *setting;
			struct config_generic *conf = gucs[i];

			if (i > 0)
				appendStringInfoString(&str, ", ");

			setting = GetConfigOptionByName(conf->name, NULL, true);

			if (setting)
				appendStringInfo(&str, "%s = '%s'", conf->name, setting);
			else
				appendStringInfo(&str, "%s = NULL", conf->name);
		}

		ExplainPropertyText("Settings", str.data, es);
	}
}

/*
 * ExplainPrintPlan  
 *	  将 QueryDesc 的计划树转换为文本并追加到 es->str   
 *
 * 调用者应已设置 *es 的选项字段，并已初始化输出  
 * 缓冲区 es->str。此外，输出格式化状态（例如缩进     
 * 级别）假定有效。*es 中计划树专用的字段在此        
 * 初始化。                
 *
 * NB：不适用于实用语句          
 */
void
ExplainPrintPlan(ExplainState *es, QueryDesc *queryDesc)
{
	Bitmapset  *rels_used = NULL;
	PlanState  *ps;

	/* 设置与此计划树关联的 ExplainState 字段        */
	Assert(queryDesc->plannedstmt != NULL);
	es->pstmt = queryDesc->plannedstmt;
	es->rtable = queryDesc->plannedstmt->rtable;
	ExplainPreScanNode(queryDesc->planstate, &rels_used);
	es->rtable_names = select_rtable_names_for_explain(es->rtable, rels_used);
	es->deparse_cxt = deparse_context_for_plan_tree(queryDesc->plannedstmt,
													es->rtable_names);
	es->printed_subplans = NULL;

	/*
	 * 有时我们将 Gather 节点标记为 "invisible"，这意味着它 
	 * 不应显示在 EXPLAIN 输出中。这样做的目的是让           
	 * 使用 debug_parallel_query=regress 运行回归测试时所得      
	 * 与用 debug_parallel_query=off 运行时结果相同。             
	 * 此类标记目前仅支持位于计划顶层的 Gather 节点。     
	 * 我们跳过该节点，并且还必须隐藏计划树更下层的    
	 * 工作进程详细数据。   
	 */
	ps = queryDesc->planstate;
	if (IsA(ps, GatherState) && ((Gather *) ps->plan)->invisible)
	{
		ps = outerPlanState(ps);
		es->hide_workers = true;
	}
	ExplainNode(ps, NIL, NULL, NULL, es);

	/*
	 * 如果被请求，则包含值与内置默认值不匹配的           
	 * GUC 参数的相关信息。      
	 */
	ExplainPrintSettings(es);

	/*
	 * COMPUTE_QUERY_ID_REGRESS 意味着 COMPUTE_QUERY_ID_AUTO，但         
	 * 我们不在任何 EXPLAIN 计划中显示 queryid，以保持    
	 * 回归测试套件结果稳定。   
	 */
	if (es->verbose && queryDesc->plannedstmt->queryId != UINT64CONST(0) &&
		compute_query_id != COMPUTE_QUERY_ID_REGRESS)
	{
		/*
		 * 以 int64 而非 uint64 输出 queryid，以匹配             
		 * 在 BIGINT 类型的 pg_stat_statements.queryid 列中所见。    
		 */
		ExplainPropertyInteger("Query Identifier", NULL, (int64)
							   queryDesc->plannedstmt->queryId, es);
	}
}

/*
 * ExplainPrintTriggers  
 *	  将 QueryDesc 的触发器统计信息转换为文本并追加到
 *	  es->str
 *
 * 调用者应已设置 *es 的选项字段，并已初始化输出  
 * 缓冲区 es->str。*es 中的其他字段                      
 * 在此初始化  
 */
void
ExplainPrintTriggers(ExplainState *es, QueryDesc *queryDesc)
{
	ResultRelInfo *rInfo;
	bool		show_relname;
	List	   *resultrels;
	List	   *routerels;
	List	   *targrels;
	ListCell   *l;

	resultrels = queryDesc->estate->es_opened_result_relations;
	routerels = queryDesc->estate->es_tuple_routing_result_relations;
	targrels = queryDesc->estate->es_trig_target_relations;

	ExplainOpenGroup("Triggers", "Triggers", false, es);

	show_relname = (list_length(resultrels) > 1 ||
					routerels != NIL || targrels != NIL);
	foreach(l, resultrels)
	{
		rInfo = (ResultRelInfo *) lfirst(l);
		report_triggers(rInfo, show_relname, es);
	}

	foreach(l, routerels)
	{
		rInfo = (ResultRelInfo *) lfirst(l);
		report_triggers(rInfo, show_relname, es);
	}

	foreach(l, targrels)
	{
		rInfo = (ResultRelInfo *) lfirst(l);
		report_triggers(rInfo, show_relname, es);
	}

	ExplainCloseGroup("Triggers", "Triggers", false, es);
}

/*
 * ExplainPrintJITSummary  
 *    输出 leader 与工作进程的汇总 JIT 仪表信息     
 */
void
ExplainPrintJITSummary(ExplainState *es, QueryDesc *queryDesc)
{
	JitInstrumentation ji = {0};

	if (!(queryDesc->estate->es_jit_flags & PGJIT_PERFORM))
		return;

	/*
	 * 使用副本而不是修改 leader 状态，因为此函数可能 
	 * 被调用两次。          
	 */
	if (queryDesc->estate->es_jit)
		InstrJitAgg(&ji, &queryDesc->estate->es_jit->instr);

	/* 如果曾在并行工作进程做过 JIT 则合并统计      */
	if (queryDesc->estate->es_jit_worker_instr)
		InstrJitAgg(&ji, queryDesc->estate->es_jit_worker_instr);

	ExplainPrintJIT(es, queryDesc->estate->es_jit_flags, &ji);
}

/*
 * ExplainPrintJIT  
 *	  将 JIT 编译信息追加到 es->str。   
 */
static void
ExplainPrintJIT(ExplainState *es, int jit_flags, JitInstrumentation *ji)
{
	instr_time	total_time;

	/* 无 JIT 编译则不输出信息              */
	if (!ji || ji->created_functions == 0)
		return;

	/* 计算总时间      */
	INSTR_TIME_SET_ZERO(total_time);
	INSTR_TIME_ADD(total_time, ji->generation_counter);
	INSTR_TIME_ADD(total_time, ji->inlining_counter);
	INSTR_TIME_ADD(total_time, ji->optimization_counter);
	INSTR_TIME_ADD(total_time, ji->emission_counter);

	ExplainOpenGroup("JIT", "JIT", true, es);

	/* 为提高密度，直接编码文本输出格式     */
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		ExplainIndentText(es);
		appendStringInfoString(es->str, "JIT:\n");
		es->indent++;

		ExplainPropertyInteger("Functions", NULL, ji->created_functions, es);

		ExplainIndentText(es);
		appendStringInfo(es->str, "Options: %s %s, %s %s, %s %s, %s %s\n",
						 "Inlining", jit_flags & PGJIT_INLINE ? "true" : "false",
						 "Optimization", jit_flags & PGJIT_OPT3 ? "true" : "false",
						 "Expressions", jit_flags & PGJIT_EXPR ? "true" : "false",
						 "Deforming", jit_flags & PGJIT_DEFORM ? "true" : "false");

		if (es->analyze && es->timing)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Timing: %s %.3f ms, %s %.3f ms, %s %.3f ms, %s %.3f ms, %s %.3f ms\n",
							 "Generation", 1000.0 * INSTR_TIME_GET_DOUBLE(ji->generation_counter),
							 "Inlining", 1000.0 * INSTR_TIME_GET_DOUBLE(ji->inlining_counter),
							 "Optimization", 1000.0 * INSTR_TIME_GET_DOUBLE(ji->optimization_counter),
							 "Emission", 1000.0 * INSTR_TIME_GET_DOUBLE(ji->emission_counter),
							 "Total", 1000.0 * INSTR_TIME_GET_DOUBLE(total_time));
		}

		es->indent--;
	}
	else
	{
		ExplainPropertyInteger("Functions", NULL, ji->created_functions, es);

		ExplainOpenGroup("Options", "Options", true, es);
		ExplainPropertyBool("Inlining", jit_flags & PGJIT_INLINE, es);
		ExplainPropertyBool("Optimization", jit_flags & PGJIT_OPT3, es);
		ExplainPropertyBool("Expressions", jit_flags & PGJIT_EXPR, es);
		ExplainPropertyBool("Deforming", jit_flags & PGJIT_DEFORM, es);
		ExplainCloseGroup("Options", "Options", true, es);

		if (es->analyze && es->timing)
		{
			ExplainOpenGroup("Timing", "Timing", true, es);

			ExplainPropertyFloat("Generation", "ms",
								 1000.0 * INSTR_TIME_GET_DOUBLE(ji->generation_counter),
								 3, es);
			ExplainPropertyFloat("Inlining", "ms",
								 1000.0 * INSTR_TIME_GET_DOUBLE(ji->inlining_counter),
								 3, es);
			ExplainPropertyFloat("Optimization", "ms",
								 1000.0 * INSTR_TIME_GET_DOUBLE(ji->optimization_counter),
								 3, es);
			ExplainPropertyFloat("Emission", "ms",
								 1000.0 * INSTR_TIME_GET_DOUBLE(ji->emission_counter),
								 3, es);
			ExplainPropertyFloat("Total", "ms",
								 1000.0 * INSTR_TIME_GET_DOUBLE(total_time),
								 3, es);

			ExplainCloseGroup("Timing", "Timing", true, es);
		}
	}

	ExplainCloseGroup("JIT", "JIT", true, es);
}

/*
 * ExplainQueryText  
 *	  添加一个包含查询实际文本的 "Query Text" 节点       
 *
 * 调用者应已设置 *es 的选项字段，并已初始化输出  
 * 缓冲区 es->str。                   
 *
 */
void
ExplainQueryText(ExplainState *es, QueryDesc *queryDesc)
{
	if (queryDesc->sourceText)
		ExplainPropertyText("Query Text", queryDesc->sourceText, es);
}

/*
 * ExplainQueryParameters  
 *	  添加一个描述查询参数的 "Query Parameters" 节点             
 *
 * 调用者应已设置 *es 的选项字段，并已初始化输出  
 * 缓冲区 es->str。                   
 *
 */
void
ExplainQueryParameters(ExplainState *es, ParamListInfo params, int maxlen)
{
	char	   *str;

	/* 此检查与 errdetail_params() 保持一致     */
	if (params == NULL || params->numParams <= 0 || maxlen == 0)
		return;

	str = BuildParamLogString(params, NULL, maxlen);
	if (str && str[0] != '\0')
		ExplainPropertyText("Query Parameters", str, es);
}

/*
 * report_triggers  
 *		报告单个关系的触发器执行统计信息       
 */
static void
report_triggers(ResultRelInfo *rInfo, bool show_relname, ExplainState *es)
{
	int			nt;

	if (!rInfo->ri_TrigDesc || !rInfo->ri_TrigInstrument)
		return;
	for (nt = 0; nt < rInfo->ri_TrigDesc->numtriggers; nt++)
	{
		Trigger    *trig = rInfo->ri_TrigDesc->triggers + nt;
		Instrumentation *instr = rInfo->ri_TrigInstrument + nt;
		char	   *relname;
		char	   *conname = NULL;

		/* 必须清理仪表状态            */
		InstrEndLoop(instr);

		/*
		 * 我们忽略从未被调用的触发器；它们很可能     
		 * 与当前查询类型无关。     
		 */
		if (instr->ntuples == 0)
			continue;

		ExplainOpenGroup("Trigger", NULL, true, es);

		relname = RelationGetRelationName(rInfo->ri_RelationDesc);
		if (OidIsValid(trig->tgconstraint))
			conname = get_constraint_name(trig->tgconstraint);

		/*
		 * 在文本格式中，除非指定 VERBOSE，否则我们避免 
		 * 同时输出触发器名称与约束名称；非文本格式中  
		 * 则全部输出。       
		 */
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			if (es->verbose || conname == NULL)
				appendStringInfo(es->str, "Trigger %s", trig->tgname);
			else
				appendStringInfoString(es->str, "Trigger");
			if (conname)
				appendStringInfo(es->str, " for constraint %s", conname);
			if (show_relname)
				appendStringInfo(es->str, " on %s", relname);
			if (es->timing)
				appendStringInfo(es->str, ": time=%.3f calls=%.0f\n",
								 1000.0 * instr->total, instr->ntuples);
			else
				appendStringInfo(es->str, ": calls=%.0f\n", instr->ntuples);
		}
		else
		{
			ExplainPropertyText("Trigger Name", trig->tgname, es);
			if (conname)
				ExplainPropertyText("Constraint Name", conname, es);
			ExplainPropertyText("Relation", relname, es);
			if (es->timing)
				ExplainPropertyFloat("Time", "ms", 1000.0 * instr->total, 3,
									 es);
			ExplainPropertyFloat("Calls", NULL, instr->ntuples, 0, es);
		}

		if (conname)
			pfree(conname);

		ExplainCloseGroup("Trigger", NULL, true, es);
	}
}

/* 根据给定时间戳计算已流逝的秒数         */
static double
elapsed_time(instr_time *starttime)
{
	instr_time	endtime;

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_SUBTRACT(endtime, *starttime);
	return INSTR_TIME_GET_DOUBLE(endtime);
}

/*
 * ExplainPreScanNode  
 *	  预扫描 planstate 树，以识别引用了哪些 RTE          
 *
 * 将每个被引用 RTE 的 relid 加入 *rels_used。其结果决定     
 * select_rtable_names_for_explain 为哪些 RTE 分配别名。      
 * 这确保我们不会令人困惑地把无后缀别名分配给          
 * 从未出现在 EXPLAIN 输出中的 RTE（例如继承父表）。   
 */
static bool
ExplainPreScanNode(PlanState *planstate, Bitmapset **rels_used)
{
	Plan	   *plan = planstate->plan;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_NamedTuplestoreScan:
		case T_WorkTableScan:
			*rels_used = bms_add_member(*rels_used,
										((Scan *) plan)->scanrelid);
			break;
		case T_ForeignScan:
			*rels_used = bms_add_members(*rels_used,
										 ((ForeignScan *) plan)->fs_base_relids);
			break;
		case T_CustomScan:
			*rels_used = bms_add_members(*rels_used,
										 ((CustomScan *) plan)->custom_relids);
			break;
		case T_ModifyTable:
			*rels_used = bms_add_member(*rels_used,
										((ModifyTable *) plan)->nominalRelation);
			if (((ModifyTable *) plan)->exclRelRTI)
				*rels_used = bms_add_member(*rels_used,
											((ModifyTable *) plan)->exclRelRTI);
			break;
		case T_Append:
			*rels_used = bms_add_members(*rels_used,
										 ((Append *) plan)->apprelids);
			break;
		case T_MergeAppend:
			*rels_used = bms_add_members(*rels_used,
										 ((MergeAppend *) plan)->apprelids);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, ExplainPreScanNode, rels_used);
}

/*
 * ExplainNode  
 *	  将计划树的描述追加到 es->str         
 *
 * planstate 指向当前计划节点的执行器状态节点。         
 * 我们需要从 PlanState 节点入手，而不只是 Plan 节点，    
 * 以便获取仪表数据（如果有）以及子计划列表。          
 *
 * ancestors 是父 Plan 与 SubPlan 节点的列表，最内层            
 * 优先。这些是解释 PARAM_EXEC 参数所必需的。        
 *
 * relationship 描述此计划节点与其父节点的                   
 * 关系（例如 "Outer"、"Inner"）；在顶层可以为 NULL。    
 * plan_name 是节点的可选名称。    
 *
 * 在文本格式中，es->indent 由本函数控制，因为我们      
 * 只希望它在计划节点边界处改变（但少数子例程      
 * 会暂时递增它）。在非文本格式中，es->indent 对应      
 * 逻辑输出组的嵌套深度，因此由 ExplainOpenGroup/              
 * ExplainCloseGroup 控制。           
 */
static void
ExplainNode(PlanState *planstate, List *ancestors,
			const char *relationship, const char *plan_name,
			ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	const char *pname;			/* 文本输出节点类型名称 */
	const char *sname;			/* 非文本输出节点类型名称  */
	const char *strategy = NULL;
	const char *partialmode = NULL;
	const char *operation = NULL;
	const char *custom_name = NULL;
	ExplainWorkersState *save_workers_state = es->workers_state;
	int			save_indent = es->indent;
	bool		haschildren;

	/*
	 * 如果需要，准备每个工作进程的输出缓冲区。           
	 * 稍后将其追加到主输出字符串。   
	 */
	if (planstate->worker_instrument && es->analyze && !es->hide_workers)
		es->workers_state = ExplainCreateWorkersState(planstate->worker_instrument->num_workers);
	else
		es->workers_state = NULL;

	/* 识别计划节点类型并输出通用细节      */
	switch (nodeTag(plan))
	{
		case T_Result:
			pname = sname = "Result";
			break;
		case T_ProjectSet:
			pname = sname = "ProjectSet";
			break;
		case T_ModifyTable:
			sname = "ModifyTable";
			switch (((ModifyTable *) plan)->operation)
			{
				case CMD_INSERT:
					pname = operation = "Insert";
					break;
				case CMD_UPDATE:
					pname = operation = "Update";
					break;
				case CMD_DELETE:
					pname = operation = "Delete";
					break;
				case CMD_MERGE:
					pname = operation = "Merge";
					break;
				default:
					pname = "???";
					break;
			}
			break;
		case T_Append:
			pname = sname = "Append";
			break;
		case T_MergeAppend:
			pname = sname = "Merge Append";
			break;
		case T_RecursiveUnion:
			pname = sname = "Recursive Union";
			break;
		case T_BitmapAnd:
			pname = sname = "BitmapAnd";
			break;
		case T_BitmapOr:
			pname = sname = "BitmapOr";
			break;
		case T_NestLoop:
			pname = sname = "Nested Loop";
			break;
		case T_MergeJoin:
			pname = "Merge";	/* "Join" 由 jointype switch 添加    */
			sname = "Merge Join";
			break;
		case T_HashJoin:
			pname = "Hash";		/* "Join" 由 jointype switch 添加    */
			sname = "Hash Join";
			break;
		case T_SeqScan:
			pname = sname = "Seq Scan";
			break;
		case T_SampleScan:
			pname = sname = "Sample Scan";
			break;
		case T_Gather:
			pname = sname = "Gather";
			break;
		case T_GatherMerge:
			pname = sname = "Gather Merge";
			break;
		case T_IndexScan:
			pname = sname = "Index Scan";
			break;
		case T_IndexOnlyScan:
			pname = sname = "Index Only Scan";
			break;
		case T_BitmapIndexScan:
			pname = sname = "Bitmap Index Scan";
			break;
		case T_BitmapHeapScan:
			pname = sname = "Bitmap Heap Scan";
			break;
		case T_TidScan:
			pname = sname = "Tid Scan";
			break;
		case T_TidRangeScan:
			pname = sname = "Tid Range Scan";
			break;
		case T_SubqueryScan:
			pname = sname = "Subquery Scan";
			break;
		case T_FunctionScan:
			pname = sname = "Function Scan";
			break;
		case T_TableFuncScan:
			pname = sname = "Table Function Scan";
			break;
		case T_ValuesScan:
			pname = sname = "Values Scan";
			break;
		case T_CteScan:
			pname = sname = "CTE Scan";
			break;
		case T_NamedTuplestoreScan:
			pname = sname = "Named Tuplestore Scan";
			break;
		case T_WorkTableScan:
			pname = sname = "WorkTable Scan";
			break;
		case T_ForeignScan:
			sname = "Foreign Scan";
			switch (((ForeignScan *) plan)->operation)
			{
				case CMD_SELECT:
					pname = "Foreign Scan";
					operation = "Select";
					break;
				case CMD_INSERT:
					pname = "Foreign Insert";
					operation = "Insert";
					break;
				case CMD_UPDATE:
					pname = "Foreign Update";
					operation = "Update";
					break;
				case CMD_DELETE:
					pname = "Foreign Delete";
					operation = "Delete";
					break;
				default:
					pname = "???";
					break;
			}
			break;
		case T_CustomScan:
			sname = "Custom Scan";
			custom_name = ((CustomScan *) plan)->methods->CustomName;
			if (custom_name)
				pname = psprintf("Custom Scan (%s)", custom_name);
			else
				pname = sname;
			break;
		case T_Material:
			pname = sname = "Materialize";
			break;
		case T_Memoize:
			pname = sname = "Memoize";
			break;
		case T_Sort:
			pname = sname = "Sort";
			break;
		case T_IncrementalSort:
			pname = sname = "Incremental Sort";
			break;
		case T_Group:
			pname = sname = "Group";
			break;
		case T_Agg:
			{
				Agg		   *agg = (Agg *) plan;

				sname = "Aggregate";
				switch (agg->aggstrategy)
				{
					case AGG_PLAIN:
						pname = "Aggregate";
						strategy = "Plain";
						break;
					case AGG_SORTED:
						pname = "GroupAggregate";
						strategy = "Sorted";
						break;
					case AGG_HASHED:
						pname = "HashAggregate";
						strategy = "Hashed";
						break;
					case AGG_MIXED:
						pname = "MixedAggregate";
						strategy = "Mixed";
						break;
					default:
						pname = "Aggregate ???";
						strategy = "???";
						break;
				}

				if (DO_AGGSPLIT_SKIPFINAL(agg->aggsplit))
				{
					partialmode = "Partial";
					pname = psprintf("%s %s", partialmode, pname);
				}
				else if (DO_AGGSPLIT_COMBINE(agg->aggsplit))
				{
					partialmode = "Finalize";
					pname = psprintf("%s %s", partialmode, pname);
				}
				else
					partialmode = "Simple";
			}
			break;
		case T_WindowAgg:
			pname = sname = "WindowAgg";
			break;
		case T_Unique:
			pname = sname = "Unique";
			break;
		case T_SetOp:
			sname = "SetOp";
			switch (((SetOp *) plan)->strategy)
			{
				case SETOP_SORTED:
					pname = "SetOp";
					strategy = "Sorted";
					break;
				case SETOP_HASHED:
					pname = "HashSetOp";
					strategy = "Hashed";
					break;
				default:
					pname = "SetOp ???";
					strategy = "???";
					break;
			}
			break;
		case T_LockRows:
			pname = sname = "LockRows";
			break;
		case T_Limit:
			pname = sname = "Limit";
			break;
		case T_Hash:
			pname = sname = "Hash";
			break;
		default:
			pname = sname = "???";
			break;
	}

	ExplainOpenGroup("Plan",
					 relationship ? NULL : "Plan",
					 true, es);

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		if (plan_name)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "%s\n", plan_name);
			es->indent++;
		}
		if (es->indent)
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "->  ");
			es->indent += 2;
		}
		if (plan->parallel_aware)
			appendStringInfoString(es->str, "Parallel ");
		if (plan->async_capable)
			appendStringInfoString(es->str, "Async ");
		appendStringInfoString(es->str, pname);
		es->indent++;
	}
	else
	{
		ExplainPropertyText("Node Type", sname, es);
		if (strategy)
			ExplainPropertyText("Strategy", strategy, es);
		if (partialmode)
			ExplainPropertyText("Partial Mode", partialmode, es);
		if (operation)
			ExplainPropertyText("Operation", operation, es);
		if (relationship)
			ExplainPropertyText("Parent Relationship", relationship, es);
		if (plan_name)
			ExplainPropertyText("Subplan Name", plan_name, es);
		if (custom_name)
			ExplainPropertyText("Custom Plan Provider", custom_name, es);
		ExplainPropertyBool("Parallel Aware", plan->parallel_aware, es);
		ExplainPropertyBool("Async Capable", plan->async_capable, es);
	}

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
			ExplainScanTarget((Scan *) plan, es);
			break;
		case T_ForeignScan:
		case T_CustomScan:
			if (((Scan *) plan)->scanrelid > 0)
				ExplainScanTarget((Scan *) plan, es);
			break;
		case T_IndexScan:
			{
				IndexScan  *indexscan = (IndexScan *) plan;

				ExplainIndexScanDetails(indexscan->indexid,
										indexscan->indexorderdir,
										es);
				ExplainScanTarget((Scan *) indexscan, es);
			}
			break;
		case T_IndexOnlyScan:
			{
				IndexOnlyScan *indexonlyscan = (IndexOnlyScan *) plan;

				ExplainIndexScanDetails(indexonlyscan->indexid,
										indexonlyscan->indexorderdir,
										es);
				ExplainScanTarget((Scan *) indexonlyscan, es);
			}
			break;
		case T_BitmapIndexScan:
			{
				BitmapIndexScan *bitmapindexscan = (BitmapIndexScan *) plan;
				const char *indexname =
					explain_get_index_name(bitmapindexscan->indexid);

				if (es->format == EXPLAIN_FORMAT_TEXT)
					appendStringInfo(es->str, " on %s",
									 quote_identifier(indexname));
				else
					ExplainPropertyText("Index Name", indexname, es);
			}
			break;
		case T_ModifyTable:
			ExplainModifyTarget((ModifyTable *) plan, es);
			break;
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			{
				const char *jointype;

				switch (((Join *) plan)->jointype)
				{
					case JOIN_INNER:
						jointype = "Inner";
						break;
					case JOIN_LEFT:
						jointype = "Left";
						break;
					case JOIN_FULL:
						jointype = "Full";
						break;
					case JOIN_RIGHT:
						jointype = "Right";
						break;
					case JOIN_SEMI:
						jointype = "Semi";
						break;
					case JOIN_ANTI:
						jointype = "Anti";
						break;
					case JOIN_RIGHT_ANTI:
						jointype = "Right Anti";
						break;
					default:
						jointype = "???";
						break;
				}
				if (es->format == EXPLAIN_FORMAT_TEXT)
				{
					/*
					 * 出于历史原因，连接类型被内插到节点  
					 * 类型名称中……     
					 */
					if (((Join *) plan)->jointype != JOIN_INNER)
						appendStringInfo(es->str, " %s Join", jointype);
					else if (!IsA(plan, NestLoop))
						appendStringInfoString(es->str, " Join");
				}
				else
					ExplainPropertyText("Join Type", jointype, es);
			}
			break;
		case T_SetOp:
			{
				const char *setopcmd;

				switch (((SetOp *) plan)->cmd)
				{
					case SETOPCMD_INTERSECT:
						setopcmd = "Intersect";
						break;
					case SETOPCMD_INTERSECT_ALL:
						setopcmd = "Intersect All";
						break;
					case SETOPCMD_EXCEPT:
						setopcmd = "Except";
						break;
					case SETOPCMD_EXCEPT_ALL:
						setopcmd = "Except All";
						break;
					default:
						setopcmd = "???";
						break;
				}
				if (es->format == EXPLAIN_FORMAT_TEXT)
					appendStringInfo(es->str, " %s", setopcmd);
				else
					ExplainPropertyText("Command", setopcmd, es);
			}
			break;
		default:
			break;
	}

	if (es->costs)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfo(es->str, "  (cost=%.2f..%.2f rows=%.0f width=%d)",
							 plan->startup_cost, plan->total_cost,
							 plan->plan_rows, plan->plan_width);
		}
		else
		{
			ExplainPropertyFloat("Startup Cost", NULL, plan->startup_cost,
								 2, es);
			ExplainPropertyFloat("Total Cost", NULL, plan->total_cost,
								 2, es);
			ExplainPropertyFloat("Plan Rows", NULL, plan->plan_rows,
								 0, es);
			ExplainPropertyInteger("Plan Width", NULL, plan->plan_width,
								   es);
		}
	}

	/*
	 * 我们必须强制清理仪表状态，因为尚未执行        
	 * ExecutorEnd。这种做法相当丑陋……            
	 *
	 * 注意：contrib/auto_explain 可能导致设置仪表，          
	 * 即使我们在此并未要求这样做。当心不要输出       
	 * 任何用户未要求的仪表结果。但我们仍会         
	 * 尽可能执行 InstrEndLoop 调用，以减少 auto_explain        
	 * 需要应付的情况数量。   
	 */
	if (planstate->instrument)
		InstrEndLoop(planstate->instrument);

	if (es->analyze &&
		planstate->instrument && planstate->instrument->nloops > 0)
	{
		double		nloops = planstate->instrument->nloops;
		double		startup_ms = 1000.0 * planstate->instrument->startup / nloops;
		double		total_ms = 1000.0 * planstate->instrument->total / nloops;
		double		rows = planstate->instrument->ntuples / nloops;

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			if (es->timing)
				appendStringInfo(es->str,
								 " (actual time=%.3f..%.3f rows=%.0f loops=%.0f)",
								 startup_ms, total_ms, rows, nloops);
			else
				appendStringInfo(es->str,
								 " (actual rows=%.0f loops=%.0f)",
								 rows, nloops);
		}
		else
		{
			if (es->timing)
			{
				ExplainPropertyFloat("Actual Startup Time", "ms", startup_ms,
									 3, es);
				ExplainPropertyFloat("Actual Total Time", "ms", total_ms,
									 3, es);
			}
			ExplainPropertyFloat("Actual Rows", NULL, rows, 0, es);
			ExplainPropertyFloat("Actual Loops", NULL, nloops, 0, es);
		}
	}
	else if (es->analyze)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(es->str, " (never executed)");
		else
		{
			if (es->timing)
			{
				ExplainPropertyFloat("Actual Startup Time", "ms", 0.0, 3, es);
				ExplainPropertyFloat("Actual Total Time", "ms", 0.0, 3, es);
			}
			ExplainPropertyFloat("Actual Rows", NULL, 0.0, 0, es);
			ExplainPropertyFloat("Actual Loops", NULL, 0.0, 0, es);
		}
	}

	/* 文本格式中第一行在此结束 */
	if (es->format == EXPLAIN_FORMAT_TEXT)
		appendStringInfoChar(es->str, '\n');

	/* 准备各工作进程的通用执行细节   */
	if (es->workers_state && es->verbose)
	{
		WorkerInstrumentation *w = planstate->worker_instrument;

		for (int n = 0; n < w->num_workers; n++)
		{
			Instrumentation *instrument = &w->instrument[n];
			double		nloops = instrument->nloops;
			double		startup_ms;
			double		total_ms;
			double		rows;

			if (nloops <= 0)
				continue;
			startup_ms = 1000.0 * instrument->startup / nloops;
			total_ms = 1000.0 * instrument->total / nloops;
			rows = instrument->ntuples / nloops;

			ExplainOpenWorker(n, es);

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				ExplainIndentText(es);
				if (es->timing)
					appendStringInfo(es->str,
									 "actual time=%.3f..%.3f rows=%.0f loops=%.0f\n",
									 startup_ms, total_ms, rows, nloops);
				else
					appendStringInfo(es->str,
									 "actual rows=%.0f loops=%.0f\n",
									 rows, nloops);
			}
			else
			{
				if (es->timing)
				{
					ExplainPropertyFloat("Actual Startup Time", "ms",
										 startup_ms, 3, es);
					ExplainPropertyFloat("Actual Total Time", "ms",
										 total_ms, 3, es);
				}
				ExplainPropertyFloat("Actual Rows", NULL, rows, 0, es);
				ExplainPropertyFloat("Actual Loops", NULL, nloops, 0, es);
			}

			ExplainCloseWorker(n, es);
		}
	}

	/* 目标列表*/
	if (es->verbose)
		show_plan_tlist(planstate, ancestors, es);

	/* 唯一连接*/
	switch (nodeTag(plan))
	{
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			/* 在文本模式下尽量少输出这方面内容 */
			if (es->format != EXPLAIN_FORMAT_TEXT ||
				(es->verbose && ((Join *) plan)->inner_unique))
				ExplainPropertyBool("Inner Unique",
									((Join *) plan)->inner_unique,
									es);
			break;
		default:
			break;
	}

	/* quals、排序键等  */
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			show_scan_qual(((IndexScan *) plan)->indexqualorig,
						   "Index Cond", planstate, ancestors, es);
			if (((IndexScan *) plan)->indexqualorig)
				show_instrumentation_count("Rows Removed by Index Recheck", 2,
										   planstate, es);
			show_scan_qual(((IndexScan *) plan)->indexorderbyorig,
						   "Order By", planstate, ancestors, es);
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_IndexOnlyScan:
			show_scan_qual(((IndexOnlyScan *) plan)->indexqual,
						   "Index Cond", planstate, ancestors, es);
			if (((IndexOnlyScan *) plan)->recheckqual)
				show_instrumentation_count("Rows Removed by Index Recheck", 2,
										   planstate, es);
			show_scan_qual(((IndexOnlyScan *) plan)->indexorderby,
						   "Order By", planstate, ancestors, es);
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			if (es->analyze)
				ExplainPropertyFloat("Heap Fetches", NULL,
									 planstate->instrument->ntuples2, 0, es);
			break;
		case T_BitmapIndexScan:
			show_scan_qual(((BitmapIndexScan *) plan)->indexqualorig,
						   "Index Cond", planstate, ancestors, es);
			break;
		case T_BitmapHeapScan:
			show_scan_qual(((BitmapHeapScan *) plan)->bitmapqualorig,
						   "Recheck Cond", planstate, ancestors, es);
			if (((BitmapHeapScan *) plan)->bitmapqualorig)
				show_instrumentation_count("Rows Removed by Index Recheck", 2,
										   planstate, es);
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			if (es->analyze)
				show_tidbitmap_info((BitmapHeapScanState *) planstate, es);
			break;
		case T_SampleScan:
			show_tablesample(((SampleScan *) plan)->tablesample,
							 planstate, ancestors, es);
			/* fall through to print additional fields the same as SeqScan */
			/* FALLTHROUGH */
		case T_SeqScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_NamedTuplestoreScan:
		case T_WorkTableScan:
		case T_SubqueryScan:
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_Gather:
			{
				Gather	   *gather = (Gather *) plan;

				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
				ExplainPropertyInteger("Workers Planned", NULL,
									   gather->num_workers, es);

				/* 显示 Gather 节点求值的参数  */
				if (gather->initParam)
					show_eval_params(gather->initParam, es);

				if (es->analyze)
				{
					int			nworkers;

					nworkers = ((GatherState *) planstate)->nworkers_launched;
					ExplainPropertyInteger("Workers Launched", NULL,
										   nworkers, es);
				}

				if (gather->single_copy || es->format != EXPLAIN_FORMAT_TEXT)
					ExplainPropertyBool("Single Copy", gather->single_copy, es);
			}
			break;
		case T_GatherMerge:
			{
				GatherMerge *gm = (GatherMerge *) plan;

				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
				ExplainPropertyInteger("Workers Planned", NULL,
									   gm->num_workers, es);

				/* 显示 Gather Merge 节点求值的参数  */
				if (gm->initParam)
					show_eval_params(gm->initParam, es);

				if (es->analyze)
				{
					int			nworkers;

					nworkers = ((GatherMergeState *) planstate)->nworkers_launched;
					ExplainPropertyInteger("Workers Launched", NULL,
										   nworkers, es);
				}
			}
			break;
		case T_FunctionScan:
			if (es->verbose)
			{
				List	   *fexprs = NIL;
				ListCell   *lc;

				foreach(lc, ((FunctionScan *) plan)->functions)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);

					fexprs = lappend(fexprs, rtfunc->funcexpr);
				}
				/* 我们依赖 show_expression 按需插入逗号       */
				show_expression((Node *) fexprs,
								"Function Call", planstate, ancestors,
								es->verbose, es);
			}
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_TableFuncScan:
			if (es->verbose)
			{
				TableFunc  *tablefunc = ((TableFuncScan *) plan)->tablefunc;

				show_expression((Node *) tablefunc,
								"Table Function Call", planstate, ancestors,
								es->verbose, es);
			}
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_TidScan:
			{
				/*
				 * tidquals 列表具有 OR 语义，因此务必以        
				 * OR 条件显示    
				 */
				List	   *tidquals = ((TidScan *) plan)->tidquals;

				if (list_length(tidquals) > 1)
					tidquals = list_make1(make_orclause(tidquals));
				show_scan_qual(tidquals, "TID Cond", planstate, ancestors, es);
				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
			}
			break;
		case T_TidRangeScan:
			{
				/*
				 * tidrangequals 列表为 AND 语义，故务必将其   
				 * 显示为 AND 条件。     
				 */
				List	   *tidquals = ((TidRangeScan *) plan)->tidrangequals;

				if (list_length(tidquals) > 1)
					tidquals = list_make1(make_andclause(tidquals));
				show_scan_qual(tidquals, "TID Cond", planstate, ancestors, es);
				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
			}
			break;
		case T_ForeignScan:
			show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			show_foreignscan_info((ForeignScanState *) planstate, es);
			break;
		case T_CustomScan:
			{
				CustomScanState *css = (CustomScanState *) planstate;

				show_scan_qual(plan->qual, "Filter", planstate, ancestors, es);
				if (plan->qual)
					show_instrumentation_count("Rows Removed by Filter", 1,
											   planstate, es);
				if (css->methods->ExplainCustomScan)
					css->methods->ExplainCustomScan(css, ancestors, es);
			}
			break;
		case T_NestLoop:
			show_upper_qual(((NestLoop *) plan)->join.joinqual,
							"Join Filter", planstate, ancestors, es);
			if (((NestLoop *) plan)->join.joinqual)
				show_instrumentation_count("Rows Removed by Join Filter", 1,
										   planstate, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 2,
										   planstate, es);
			break;
		case T_MergeJoin:
			show_upper_qual(((MergeJoin *) plan)->mergeclauses,
							"Merge Cond", planstate, ancestors, es);
			show_upper_qual(((MergeJoin *) plan)->join.joinqual,
							"Join Filter", planstate, ancestors, es);
			if (((MergeJoin *) plan)->join.joinqual)
				show_instrumentation_count("Rows Removed by Join Filter", 1,
										   planstate, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 2,
										   planstate, es);
			break;
		case T_HashJoin:
			show_upper_qual(((HashJoin *) plan)->hashclauses,
							"Hash Cond", planstate, ancestors, es);
			show_upper_qual(((HashJoin *) plan)->join.joinqual,
							"Join Filter", planstate, ancestors, es);
			if (((HashJoin *) plan)->join.joinqual)
				show_instrumentation_count("Rows Removed by Join Filter", 1,
										   planstate, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 2,
										   planstate, es);
			break;
		case T_Agg:
			show_agg_keys(castNode(AggState, planstate), ancestors, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			show_hashagg_info((AggState *) planstate, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_WindowAgg:
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			show_upper_qual(((WindowAgg *) plan)->runConditionOrig,
							"Run Condition", planstate, ancestors, es);
			break;
		case T_Group:
			show_group_keys(castNode(GroupState, planstate), ancestors, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_Sort:
			show_sort_keys(castNode(SortState, planstate), ancestors, es);
			show_sort_info(castNode(SortState, planstate), es);
			break;
		case T_IncrementalSort:
			show_incremental_sort_keys(castNode(IncrementalSortState, planstate),
									   ancestors, es);
			show_incremental_sort_info(castNode(IncrementalSortState, planstate),
									   es);
			break;
		case T_MergeAppend:
			show_merge_append_keys(castNode(MergeAppendState, planstate),
								   ancestors, es);
			break;
		case T_Result:
			show_upper_qual((List *) ((Result *) plan)->resconstantqual,
							"One-Time Filter", planstate, ancestors, es);
			show_upper_qual(plan->qual, "Filter", planstate, ancestors, es);
			if (plan->qual)
				show_instrumentation_count("Rows Removed by Filter", 1,
										   planstate, es);
			break;
		case T_ModifyTable:
			show_modifytable_info(castNode(ModifyTableState, planstate), ancestors,
								  es);
			break;
		case T_Hash:
			show_hash_info(castNode(HashState, planstate), es);
			break;
		case T_Memoize:
			show_memoize_info(castNode(MemoizeState, planstate), ancestors,
							  es);
			break;
		default:
			break;
	}

	/*
	 * 准备各工作进程的 JIT 仪表。与整体 JIT 摘要      
	 * 一样，仅当启用了代价打印时才打印。        
	 */
	if (es->workers_state && es->costs && es->verbose)
	{
		SharedJitInstrumentation *w = planstate->worker_jit_instrument;

		if (w)
		{
			for (int n = 0; n < w->num_workers; n++)
			{
				ExplainOpenWorker(n, es);
				ExplainPrintJIT(es, planstate->state->es_jit_flags,
								&w->jit_instr[n]);
				ExplainCloseWorker(n, es);
			}
		}
	}

	/* 缓冲区/WAL 用量  */
	if (es->buffers && planstate->instrument)
		show_buffer_usage(es, &planstate->instrument->bufusage, false);
	if (es->wal && planstate->instrument)
		show_wal_usage(es, &planstate->instrument->walusage);

	/* 准备工作进程缓冲区/WAL     */
	if (es->workers_state && (es->buffers || es->wal) && es->verbose)
	{
		WorkerInstrumentation *w = planstate->worker_instrument;

		for (int n = 0; n < w->num_workers; n++)
		{
			Instrumentation *instrument = &w->instrument[n];
			double		nloops = instrument->nloops;

			if (nloops <= 0)
				continue;

			ExplainOpenWorker(n, es);
			if (es->buffers)
				show_buffer_usage(es, &instrument->bufusage, false);
			if (es->wal)
				show_wal_usage(es, &instrument->walusage);
			ExplainCloseWorker(n, es);
		}
	}

	/* 显示此节点的工作进程详细信息，然后弹出该栈 */
	if (es->workers_state)
		ExplainFlushWorkersState(es);
	es->workers_state = save_workers_state;

	/*
	 * 如果分区裁剪在执行器初始化期间完成，则下面  
	 * 显示的子计划数量将少于计划中指定的                   
	 * 子计划数量。为了让这一点不那么费解，我们将     
	 * 输出一个标记说明发生了这种情况。注意，此      
	 * 字段现在输出是因为我们希望它成为父节点的          
	 * 属性；它*无法*在下面要打开的 Plans 子节点中输出。  
	 */
	switch (nodeTag(plan))
	{
		case T_Append:
			ExplainMissingMembers(((AppendState *) planstate)->as_nplans,
								  list_length(((Append *) plan)->appendplans),
								  es);
			break;
		case T_MergeAppend:
			ExplainMissingMembers(((MergeAppendState *) planstate)->ms_nplans,
								  list_length(((MergeAppend *) plan)->mergeplans),
								  es);
			break;
		default:
			break;
	}

	/* 准备显示各子计划             */
	haschildren = planstate->initPlan ||
		outerPlanState(planstate) ||
		innerPlanState(planstate) ||
		IsA(plan, Append) ||
		IsA(plan, MergeAppend) ||
		IsA(plan, BitmapAnd) ||
		IsA(plan, BitmapOr) ||
		IsA(plan, SubqueryScan) ||
		(IsA(planstate, CustomScanState) &&
		 ((CustomScanState *) planstate)->custom_ps != NIL) ||
		planstate->subPlan;
	if (haschildren)
	{
		ExplainOpenGroup("Plans", "Plans", false, es);
		/* 把当前 Plan 传为子节点 ancestors 列表的表头 */
		ancestors = lcons(plan, ancestors);
	}

	/* initPlan-s */
	if (planstate->initPlan)
		ExplainSubPlans(planstate->initPlan, ancestors, "InitPlan", es);

	/* lefttree */
	if (outerPlanState(planstate))
		ExplainNode(outerPlanState(planstate), ancestors,
					"Outer", NULL, es);

	/* righttree */
	if (innerPlanState(planstate))
		ExplainNode(innerPlanState(planstate), ancestors,
					"Inner", NULL, es);

	/* 特殊的子计划  */
	switch (nodeTag(plan))
	{
		case T_Append:
			ExplainMemberNodes(((AppendState *) planstate)->appendplans,
							   ((AppendState *) planstate)->as_nplans,
							   ancestors, es);
			break;
		case T_MergeAppend:
			ExplainMemberNodes(((MergeAppendState *) planstate)->mergeplans,
							   ((MergeAppendState *) planstate)->ms_nplans,
							   ancestors, es);
			break;
		case T_BitmapAnd:
			ExplainMemberNodes(((BitmapAndState *) planstate)->bitmapplans,
							   ((BitmapAndState *) planstate)->nplans,
							   ancestors, es);
			break;
		case T_BitmapOr:
			ExplainMemberNodes(((BitmapOrState *) planstate)->bitmapplans,
							   ((BitmapOrState *) planstate)->nplans,
							   ancestors, es);
			break;
		case T_SubqueryScan:
			ExplainNode(((SubqueryScanState *) planstate)->subplan, ancestors,
						"Subquery", NULL, es);
			break;
		case T_CustomScan:
			ExplainCustomChildren((CustomScanState *) planstate,
								  ancestors, es);
			break;
		default:
			break;
	}

	/* subPlan-s */
	if (planstate->subPlan)
		ExplainSubPlans(planstate->subPlan, ancestors, "SubPlan", es);

	/* 子计划结束    */
	if (haschildren)
	{
		ancestors = list_delete_first(ancestors);
		ExplainCloseGroup("Plans", "Plans", false, es);
	}

	/* 文本格式中撤销我们添加的所有缩进   */
	if (es->format == EXPLAIN_FORMAT_TEXT)
		es->indent = save_indent;

	ExplainCloseGroup("Plan",
					  relationship ? NULL : "Plan",
					  true, es);
}

/*
 * 显示计划节点的目标列表 
 */
static void
show_plan_tlist(PlanState *planstate, List *ancestors, ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	List	   *context;
	List	   *result = NIL;
	bool		useprefix;
	ListCell   *lc;

	/* tlist 为空则无事可做（如位图索引扫描）       */
	if (plan->targetlist == NIL)
		return;
	/* Append 的 tlist 没什么实际用处，所以隐藏它  */
	if (IsA(plan, Append))
		return;
	/* MergeAppend 和 RecursiveUnion 同理       */
	if (IsA(plan, MergeAppend))
		return;
	if (IsA(plan, RecursiveUnion))
		return;

	/*
	 * 对于执行直接 INSERT/UPDATE/DELETE 的 ForeignScan 同理      
	 *
	 * 注意：执行直接 INSERT/UPDATE 的 ForeignScan，其 tlist       
	 * 可能含有在此上下文中混淆的子计划输出表达式。 
	 * 而执行直接 UPDATE/DELETE 的 ForeignScan，其 tlist           
	 * 总是包含用于确定待更新或删除的确切行的 "junk"      
	 * 目标列，这在此上下文中会造成混淆。因此我们在    
	 * 所有情况都隐藏它。  
	 */
	if (IsA(plan, ForeignScan) &&
		((ForeignScan *) plan)->operation != CMD_SELECT)
		return;

	/* 设置反解析上下文 */
	context = set_deparse_context_plan(es->deparse_cxt,
									   plan,
									   ancestors);
	useprefix = list_length(es->rtable) > 1;

	/* 反解析每个结果列（现在也包括 resjunk 列）*/
	foreach(lc, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		result = lappend(result,
						 deparse_expression((Node *) tle->expr, context,
											useprefix, false));
	}

	/* 输出结果  */
	ExplainPropertyList("Output", result, es);
}

/*
 * 显示通用表达式    
 */
static void
show_expression(Node *node, const char *qlabel,
				PlanState *planstate, List *ancestors,
				bool useprefix, ExplainState *es)
{
	List	   *context;
	char	   *exprstr;

	/* 设置反解析上下文 */
	context = set_deparse_context_plan(es->deparse_cxt,
									   planstate->plan,
									   ancestors);

	/* 反解析该表达式  */
	exprstr = deparse_expression(node, context, useprefix, false);

	/* 再加入 es->str  */
	ExplainPropertyText(qlabel, exprstr, es);
}

/*
 * 显示限定条件表达式（它是具有隐式 AND 语义的列表）  
 */
static void
show_qual(List *qual, const char *qlabel,
		  PlanState *planstate, List *ancestors,
		  bool useprefix, ExplainState *es)
{
	Node	   *node;

	/* qual 为空则跳过  */
	if (qual == NIL)
		return;

	/* 把 AND 列表转换为显式 AND*/
	node = (Node *) make_ands_explicit(qual);

	/* 显示它   */
	show_expression(node, qlabel, planstate, ancestors, useprefix, es);
}

/*
 * 显示扫描计划节点的限定条件表达式
 */
static void
show_scan_qual(List *qual, const char *qlabel,
			   PlanState *planstate, List *ancestors,
			   ExplainState *es)
{
	bool		useprefix;

	useprefix = (IsA(planstate->plan, SubqueryScan) || es->verbose);
	show_qual(qual, qlabel, planstate, ancestors, useprefix, es);
}

/*
 * 显示上层计划节点的限定条件表达式        
 */
static void
show_upper_qual(List *qual, const char *qlabel,
				PlanState *planstate, List *ancestors,
				ExplainState *es)
{
	bool		useprefix;

	useprefix = (list_length(es->rtable) > 1 || es->verbose);
	show_qual(qual, qlabel, planstate, ancestors, useprefix, es);
}

/*
 * 显示 Sort 节点的排序键。  
 */
static void
show_sort_keys(SortState *sortstate, List *ancestors, ExplainState *es)
{
	Sort	   *plan = (Sort *) sortstate->ss.ps.plan;

	show_sort_group_keys((PlanState *) sortstate, "Sort Key",
						 plan->numCols, 0, plan->sortColIdx,
						 plan->sortOperators, plan->collations,
						 plan->nullsFirst,
						 ancestors, es);
}

/*
 * 显示 IncrementalSort 节点的排序键。  
 */
static void
show_incremental_sort_keys(IncrementalSortState *incrsortstate,
						   List *ancestors, ExplainState *es)
{
	IncrementalSort *plan = (IncrementalSort *) incrsortstate->ss.ps.plan;

	show_sort_group_keys((PlanState *) incrsortstate, "Sort Key",
						 plan->sort.numCols, plan->nPresortedCols,
						 plan->sort.sortColIdx,
						 plan->sort.sortOperators, plan->sort.collations,
						 plan->sort.nullsFirst,
						 ancestors, es);
}

/*
 * MergeAppend 节点同理。      
 */
static void
show_merge_append_keys(MergeAppendState *mstate, List *ancestors,
					   ExplainState *es)
{
	MergeAppend *plan = (MergeAppend *) mstate->ps.plan;

	show_sort_group_keys((PlanState *) mstate, "Sort Key",
						 plan->numCols, 0, plan->sortColIdx,
						 plan->sortOperators, plan->collations,
						 plan->nullsFirst,
						 ancestors, es);
}

/*
 * 显示 Agg 节点的分组键。       
 */
static void
show_agg_keys(AggState *astate, List *ancestors,
			  ExplainState *es)
{
	Agg		   *plan = (Agg *) astate->ss.ps.plan;

	if (plan->numCols > 0 || plan->groupingSets)
	{
		/* 键列引用的是子计划 tlist 中的列          */
		ancestors = lcons(plan, ancestors);

		if (plan->groupingSets)
			show_grouping_sets(outerPlanState(astate), plan, ancestors, es);
		else
			show_sort_group_keys(outerPlanState(astate), "Group Key",
								 plan->numCols, 0, plan->grpColIdx,
								 NULL, NULL, NULL,
								 ancestors, es);

		ancestors = list_delete_first(ancestors);
	}
}

static void
show_grouping_sets(PlanState *planstate, Agg *agg,
				   List *ancestors, ExplainState *es)
{
	List	   *context;
	bool		useprefix;
	ListCell   *lc;

	/* 设置反解析上下文 */
	context = set_deparse_context_plan(es->deparse_cxt,
									   planstate->plan,
									   ancestors);
	useprefix = (list_length(es->rtable) > 1 || es->verbose);

	ExplainOpenGroup("Grouping Sets", "Grouping Sets", false, es);

	show_grouping_set_keys(planstate, agg, NULL,
						   context, useprefix, ancestors, es);

	foreach(lc, agg->chain)
	{
		Agg		   *aggnode = lfirst(lc);
		Sort	   *sortnode = (Sort *) aggnode->plan.lefttree;

		show_grouping_set_keys(planstate, aggnode, sortnode,
							   context, useprefix, ancestors, es);
	}

	ExplainCloseGroup("Grouping Sets", "Grouping Sets", false, es);
}

static void
show_grouping_set_keys(PlanState *planstate,
					   Agg *aggnode, Sort *sortnode,
					   List *context, bool useprefix,
					   List *ancestors, ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	char	   *exprstr;
	ListCell   *lc;
	List	   *gsets = aggnode->groupingSets;
	AttrNumber *keycols = aggnode->grpColIdx;
	const char *keyname;
	const char *keysetname;

	if (aggnode->aggstrategy == AGG_HASHED || aggnode->aggstrategy == AGG_MIXED)
	{
		keyname = "Hash Key";
		keysetname = "Hash Keys";
	}
	else
	{
		keyname = "Group Key";
		keysetname = "Group Keys";
	}

	ExplainOpenGroup("Grouping Set", NULL, true, es);

	if (sortnode)
	{
		show_sort_group_keys(planstate, "Sort Key",
							 sortnode->numCols, 0, sortnode->sortColIdx,
							 sortnode->sortOperators, sortnode->collations,
							 sortnode->nullsFirst,
							 ancestors, es);
		if (es->format == EXPLAIN_FORMAT_TEXT)
			es->indent++;
	}

	ExplainOpenGroup(keysetname, keysetname, false, es);

	foreach(lc, gsets)
	{
		List	   *result = NIL;
		ListCell   *lc2;

		foreach(lc2, (List *) lfirst(lc))
		{
			Index		i = lfirst_int(lc2);
			AttrNumber	keyresno = keycols[i];
			TargetEntry *target = get_tle_by_resno(plan->targetlist,
												   keyresno);

			if (!target)
				elog(ERROR, "no tlist entry for key %d", keyresno);
			/* 反解析表达式，并显示任何顶层 cast    */
			exprstr = deparse_expression((Node *) target->expr, context,
										 useprefix, true);

			result = lappend(result, exprstr);
		}

		if (!result && es->format == EXPLAIN_FORMAT_TEXT)
			ExplainPropertyText(keyname, "()", es);
		else
			ExplainPropertyListNested(keyname, result, es);
	}

	ExplainCloseGroup(keysetname, keysetname, false, es);

	if (sortnode && es->format == EXPLAIN_FORMAT_TEXT)
		es->indent--;

	ExplainCloseGroup("Grouping Set", NULL, true, es);
}

/*
 * 显示 Group 节点的分组键。      
 */
static void
show_group_keys(GroupState *gstate, List *ancestors,
				ExplainState *es)
{
	Group	   *plan = (Group *) gstate->ss.ps.plan;

	/* 这些键列引用子计划的 tlist                 */
	ancestors = lcons(plan, ancestors);
	show_sort_group_keys(outerPlanState(gstate), "Group Key",
						 plan->numCols, 0, plan->grpColIdx,
						 NULL, NULL, NULL,
						 ancestors, es);
	ancestors = list_delete_first(ancestors);
}

/*
 * 显示排序/分组键的公共代码，它们在计划节点中表示  
 * 为目标列表索引的数组。若为排序键而非分组键，还   
 * 需传递排序操作符/collation/nullsFirst 数组。    
 */
static void
show_sort_group_keys(PlanState *planstate, const char *qlabel,
					 int nkeys, int nPresortedKeys, AttrNumber *keycols,
					 Oid *sortOperators, Oid *collations, bool *nullsFirst,
					 List *ancestors, ExplainState *es)
{
	Plan	   *plan = planstate->plan;
	List	   *context;
	List	   *result = NIL;
	List	   *resultPresorted = NIL;
	StringInfoData sortkeybuf;
	bool		useprefix;
	int			keyno;

	if (nkeys <= 0)
		return;

	initStringInfo(&sortkeybuf);

	/* 设置反解析上下文 */
	context = set_deparse_context_plan(es->deparse_cxt,
									   plan,
									   ancestors);
	useprefix = (list_length(es->rtable) > 1 || es->verbose);

	for (keyno = 0; keyno < nkeys; keyno++)
	{
		/* 在 tlist 中找键表达式 */
		AttrNumber	keyresno = keycols[keyno];
		TargetEntry *target = get_tle_by_resno(plan->targetlist,
											   keyresno);
		char	   *exprstr;

		if (!target)
			elog(ERROR, "no tlist entry for key %d", keyresno);
		/* 反解析表达式，并显示任何顶层 cast    */
		exprstr = deparse_expression((Node *) target->expr, context,
									 useprefix, true);
		resetStringInfo(&sortkeybuf);
		appendStringInfoString(&sortkeybuf, exprstr);
		/* 如相关，则追加排序顺序信息    */
		if (sortOperators != NULL)
			show_sortorder_options(&sortkeybuf,
								   (Node *) target->expr,
								   sortOperators[keyno],
								   collations[keyno],
								   nullsFirst[keyno]);
		/* 每个排序键输出一属性列表项  */
		result = lappend(result, pstrdup(sortkeybuf.data));
		if (keyno < nPresortedKeys)
			resultPresorted = lappend(resultPresorted, exprstr);
	}

	ExplainPropertyList(qlabel, result, es);
	if (nPresortedKeys > 0)
		ExplainPropertyList("Presorted Key", resultPresorted, es);
}

/*
 * 把某列排序顺序的非默认特性（collation、方向、          
 * NULLS FIRST/LAST）追加到 buf 中    
 */
static void
show_sortorder_options(StringInfo buf, Node *sortexpr,
					   Oid sortOperator, Oid collation, bool nullsFirst)
{
	Oid			sortcoltype = exprType(sortexpr);
	bool		reverse = false;
	TypeCacheEntry *typentry;

	typentry = lookup_type_cache(sortcoltype,
								 TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);

	/*
	 * 若 COLLATE 不是该列类型的默认值，则打印它。有一 
	 * 些情形下这是冗余的：表达式为某列且其声明排序规 
	 * 则就是该排序规则，但此处很难区分（况且在这些情  
	 * 形下，显式打印 COLLATE 反正也是个好主意               
	 * ）。         
	 */
	if (OidIsValid(collation) && collation != get_typcollation(sortcoltype))
	{
		char	   *collname = get_collation_name(collation);

		if (collname == NULL)
			elog(ERROR, "cache lookup failed for collation %u", collation);
		appendStringInfo(buf, " COLLATE %s", quote_identifier(collname));
	}

	/* 打印方向（若非 ASC）或 USING（若非默认操作符）  */
	if (sortOperator == typentry->gt_opr)
	{
		appendStringInfoString(buf, " DESC");
		reverse = true;
	}
	else if (sortOperator != typentry->lt_opr)
	{
		char	   *opname = get_opname(sortOperator);

		if (opname == NULL)
			elog(ERROR, "cache lookup failed for operator %u", sortOperator);
		appendStringInfo(buf, " USING %s", opname);
		/* 判断该操作符会被视为 ASC 还是 DESC             */
		(void) get_equality_op_for_ordering_op(sortOperator, &reverse);
	}

	/* 仅当非默认时才加 NULLS FIRST/LAST           */
	if (nullsFirst && !reverse)
	{
		appendStringInfoString(buf, " NULLS FIRST");
	}
	else if (!nullsFirst && reverse)
	{
		appendStringInfoString(buf, " NULLS LAST");
	}
}

/*
 * TABLESAMPLE 属性显示   
 */
static void
show_tablesample(TableSampleClause *tsc, PlanState *planstate,
				 List *ancestors, ExplainState *es)
{
	List	   *context;
	bool		useprefix;
	char	   *method_name;
	List	   *params = NIL;
	char	   *repeatable;
	ListCell   *lc;

	/* 设置反解析上下文 */
	context = set_deparse_context_plan(es->deparse_cxt,
									   planstate->plan,
									   ancestors);
	useprefix = list_length(es->rtable) > 1;

	/* 获取表采样方法名称     */
	method_name = get_func_name(tsc->tsmhandler);

	/* 反解析参数表达式      */
	foreach(lc, tsc->args)
	{
		Node	   *arg = (Node *) lfirst(lc);

		params = lappend(params,
						 deparse_expression(arg, context,
											useprefix, false));
	}
	if (tsc->repeatable)
		repeatable = deparse_expression((Node *) tsc->repeatable, context,
										useprefix, false);
	else
		repeatable = NULL;

	/* 输出结果  */
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		bool		first = true;

		ExplainIndentText(es);
		appendStringInfo(es->str, "Sampling: %s (", method_name);
		foreach(lc, params)
		{
			if (!first)
				appendStringInfoString(es->str, ", ");
			appendStringInfoString(es->str, (const char *) lfirst(lc));
			first = false;
		}
		appendStringInfoChar(es->str, ')');
		if (repeatable)
			appendStringInfo(es->str, " REPEATABLE (%s)", repeatable);
		appendStringInfoChar(es->str, '\n');
	}
	else
	{
		ExplainPropertyText("Sampling Method", method_name, es);
		ExplainPropertyList("Sampling Parameters", params, es);
		if (repeatable)
			ExplainPropertyText("Repeatable Seed", repeatable, es);
	}
}

/*
 * 若 EXPLAIN ANALYZE，显示该排序节点 tuplesort 统计 
 */
static void
show_sort_info(SortState *sortstate, ExplainState *es)
{
	if (!es->analyze)
		return;

	if (sortstate->sort_Done && sortstate->tuplesortstate != NULL)
	{
		Tuplesortstate *state = (Tuplesortstate *) sortstate->tuplesortstate;
		TuplesortInstrumentation stats;
		const char *sortMethod;
		const char *spaceType;
		int64		spaceUsed;

		tuplesort_get_stats(state, &stats);
		sortMethod = tuplesort_method_name(stats.sortMethod);
		spaceType = tuplesort_space_type_name(stats.spaceType);
		spaceUsed = stats.spaceUsed;

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Sort Method: %s  %s: " INT64_FORMAT "kB\n",
							 sortMethod, spaceType, spaceUsed);
		}
		else
		{
			ExplainPropertyText("Sort Method", sortMethod, es);
			ExplainPropertyInteger("Sort Space Used", "kB", spaceUsed, es);
			ExplainPropertyText("Sort Space Type", spaceType, es);
		}
	}

	/*
	 * 你可能认为当 es->hide_workers 为真时可以完全    
	 * 跳过这一段，但那样会得不到任何排序方法输出。  
	 * 我们得让它看起来像 worker 0 的数据就是顶层数据。  
	 * 这很容易做到：只需跳过 OpenWorker/CloseWorker 调用。    
	 * 目前我们不担心这种情况下存在多个 worker 的可能      
	 * 性；若果真有，就会发出重复的输出字段               
	 * 。     
	 */
	if (sortstate->shared_info != NULL)
	{
		int			n;

		for (n = 0; n < sortstate->shared_info->num_workers; n++)
		{
			TuplesortInstrumentation *sinstrument;
			const char *sortMethod;
			const char *spaceType;
			int64		spaceUsed;

			sinstrument = &sortstate->shared_info->sinstrument[n];
			if (sinstrument->sortMethod == SORT_TYPE_STILL_IN_PROGRESS)
				continue;		/* 忽略未填满的槽位  */
			sortMethod = tuplesort_method_name(sinstrument->sortMethod);
			spaceType = tuplesort_space_type_name(sinstrument->spaceType);
			spaceUsed = sinstrument->spaceUsed;

			if (es->workers_state)
				ExplainOpenWorker(n, es);

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				ExplainIndentText(es);
				appendStringInfo(es->str,
								 "Sort Method: %s  %s: " INT64_FORMAT "kB\n",
								 sortMethod, spaceType, spaceUsed);
			}
			else
			{
				ExplainPropertyText("Sort Method", sortMethod, es);
				ExplainPropertyInteger("Sort Space Used", "kB", spaceUsed, es);
				ExplainPropertyText("Sort Space Type", spaceType, es);
			}

			if (es->workers_state)
				ExplainCloseWorker(n, es);
		}
	}
}

/*
 * 增量排序节点分（可能极为庞大的）批次进行排序，       
 * 故 EXPLAIN ANALYZE 须把每个批次的 tuplesort 统计信息归并为   
 * 易懂的摘要。      
 *
 * 此函数用于非并行节点和并行增量排序节点里的        
 * 每个 worker。               
 */
static void
show_incremental_sort_group_info(IncrementalSortGroupInfo *groupInfo,
								 const char *groupLabel, bool indent, ExplainState *es)
{
	ListCell   *methodCell;
	List	   *methodNames = NIL;

	/* 生成所有组共用的排序方法列表。           */
	for (int bit = 0; bit < NUM_TUPLESORTMETHODS; bit++)
	{
		TuplesortMethod sortMethod = (1 << bit);

		if (groupInfo->sortMethods & sortMethod)
		{
			const char *methodName = tuplesort_method_name(sortMethod);

			methodNames = lappend(methodNames, unconstify(char *, methodName));
		}
	}

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		if (indent)
			appendStringInfoSpaces(es->str, es->indent * 2);
		appendStringInfo(es->str, "%s Groups: " INT64_FORMAT "  Sort Method", groupLabel,
						 groupInfo->groupCount);
		/* 按 methodNames 的大小定单复数     */
		if (list_length(methodNames) > 1)
			appendStringInfoString(es->str, "s: ");
		else
			appendStringInfoString(es->str, ": ");
		foreach(methodCell, methodNames)
		{
			appendStringInfoString(es->str, (char *) methodCell->ptr_value);
			if (foreach_current_index(methodCell) < list_length(methodNames) - 1)
				appendStringInfoString(es->str, ", ");
		}

		if (groupInfo->maxMemorySpaceUsed > 0)
		{
			int64		avgSpace = groupInfo->totalMemorySpaceUsed / groupInfo->groupCount;
			const char *spaceTypeName;

			spaceTypeName = tuplesort_space_type_name(SORT_SPACE_TYPE_MEMORY);
			appendStringInfo(es->str, "  Average %s: " INT64_FORMAT "kB  Peak %s: " INT64_FORMAT "kB",
							 spaceTypeName, avgSpace,
							 spaceTypeName, groupInfo->maxMemorySpaceUsed);
		}

		if (groupInfo->maxDiskSpaceUsed > 0)
		{
			int64		avgSpace = groupInfo->totalDiskSpaceUsed / groupInfo->groupCount;

			const char *spaceTypeName;

			spaceTypeName = tuplesort_space_type_name(SORT_SPACE_TYPE_DISK);
			appendStringInfo(es->str, "  Average %s: " INT64_FORMAT "kB  Peak %s: " INT64_FORMAT "kB",
							 spaceTypeName, avgSpace,
							 spaceTypeName, groupInfo->maxDiskSpaceUsed);
		}
	}
	else
	{
		StringInfoData groupName;

		initStringInfo(&groupName);
		appendStringInfo(&groupName, "%s Groups", groupLabel);
		ExplainOpenGroup("Incremental Sort Groups", groupName.data, true, es);
		ExplainPropertyInteger("Group Count", NULL, groupInfo->groupCount, es);

		ExplainPropertyList("Sort Methods Used", methodNames, es);

		if (groupInfo->maxMemorySpaceUsed > 0)
		{
			int64		avgSpace = groupInfo->totalMemorySpaceUsed / groupInfo->groupCount;
			const char *spaceTypeName;
			StringInfoData memoryName;

			spaceTypeName = tuplesort_space_type_name(SORT_SPACE_TYPE_MEMORY);
			initStringInfo(&memoryName);
			appendStringInfo(&memoryName, "Sort Space %s", spaceTypeName);
			ExplainOpenGroup("Sort Space", memoryName.data, true, es);

			ExplainPropertyInteger("Average Sort Space Used", "kB", avgSpace, es);
			ExplainPropertyInteger("Peak Sort Space Used", "kB",
								   groupInfo->maxMemorySpaceUsed, es);

			ExplainCloseGroup("Sort Space", memoryName.data, true, es);
		}
		if (groupInfo->maxDiskSpaceUsed > 0)
		{
			int64		avgSpace = groupInfo->totalDiskSpaceUsed / groupInfo->groupCount;
			const char *spaceTypeName;
			StringInfoData diskName;

			spaceTypeName = tuplesort_space_type_name(SORT_SPACE_TYPE_DISK);
			initStringInfo(&diskName);
			appendStringInfo(&diskName, "Sort Space %s", spaceTypeName);
			ExplainOpenGroup("Sort Space", diskName.data, true, es);

			ExplainPropertyInteger("Average Sort Space Used", "kB", avgSpace, es);
			ExplainPropertyInteger("Peak Sort Space Used", "kB",
								   groupInfo->maxDiskSpaceUsed, es);

			ExplainCloseGroup("Sort Space", diskName.data, true, es);
		}

		ExplainCloseGroup("Incremental Sort Groups", groupName.data, true, es);
	}
}

/*
 * 若 EXPLAIN ANALYZE，显示该增量排序节点 tuplesort 统计        
 */
static void
show_incremental_sort_info(IncrementalSortState *incrsortstate,
						   ExplainState *es)
{
	IncrementalSortGroupInfo *fullsortGroupInfo;
	IncrementalSortGroupInfo *prefixsortGroupInfo;

	fullsortGroupInfo = &incrsortstate->incsort_info.fullsortGroupInfo;

	if (!es->analyze)
		return;

	/*
	 * 除非先排过完整组并切换模式（元组拷入前缀组）， 
	 * 否则不会有任何前缀组，故完整组数为 0 时无需做任  
	 * 何事。                                                
	 *
	 * 但没有完整组时仍要继续此块之后的代码，因为        
	 * 可能有确实干过活的 worker，即使                        
	 * leader 并未参与。                
	 */
	if (fullsortGroupInfo->groupCount > 0)
	{
		show_incremental_sort_group_info(fullsortGroupInfo, "Full-sort", true, es);
		prefixsortGroupInfo = &incrsortstate->incsort_info.prefixsortGroupInfo;
		if (prefixsortGroupInfo->groupCount > 0)
		{
			if (es->format == EXPLAIN_FORMAT_TEXT)
				appendStringInfoChar(es->str, '\n');
			show_incremental_sort_group_info(prefixsortGroupInfo, "Pre-sorted", true, es);
		}
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoChar(es->str, '\n');
	}

	if (incrsortstate->shared_info != NULL)
	{
		int			n;
		bool		indent_first_line;

		for (n = 0; n < incrsortstate->shared_info->num_workers; n++)
		{
			IncrementalSortInfo *incsort_info =
				&incrsortstate->shared_info->sinfo[n];

			/*
			 * 若某 worker 完全没处理过排序组，             
			 * 就把它从输出排除：它没启动，或没有实质     
			 * 贡献。                      
			 */
			fullsortGroupInfo = &incsort_info->fullsortGroupInfo;

			/*
			 * 除非先排过完整组并切换模式，否则无前缀组   
			 * （把元组拷入前缀组），所以完整组数为 0 时   
			 * 无需做任何事                                         
			 * 。         
			 */
			if (fullsortGroupInfo->groupCount == 0)
				continue;

			if (es->workers_state)
				ExplainOpenWorker(n, es);

			indent_first_line = es->workers_state == NULL || es->verbose;
			show_incremental_sort_group_info(fullsortGroupInfo, "Full-sort",
											 indent_first_line, es);
			prefixsortGroupInfo = &incsort_info->prefixsortGroupInfo;
			if (prefixsortGroupInfo->groupCount > 0)
			{
				if (es->format == EXPLAIN_FORMAT_TEXT)
					appendStringInfoChar(es->str, '\n');
				show_incremental_sort_group_info(prefixsortGroupInfo, "Pre-sorted", true, es);
			}
			if (es->format == EXPLAIN_FORMAT_TEXT)
				appendStringInfoChar(es->str, '\n');

			if (es->workers_state)
				ExplainCloseWorker(n, es);
		}
	}
}

/*
 * 显示哈希桶/批次的信息。       
 */
static void
show_hash_info(HashState *hashstate, ExplainState *es)
{
	HashInstrumentation hinstrument = {0};

	/*
	 * 即使这是并行查询，也要收集本地进程的统计信息。 
	 * 并行查询中 leader 进程可能跑过哈希连接，也可能  
	 * 没跑过；即使跑了，也可能因时机问题没建哈希表     
	 * （若启动较晚，可能在外关系没见元组，因而跳过  
	 * 了哈希表的构建）。因此必须准备从所有参与者获取  
	 * instrumentation 数据。                                  
	 */
	if (hashstate->hinstrument)
		memcpy(&hinstrument, hashstate->hinstrument,
			   sizeof(HashInstrumentation));

	/*
	 * 合并来自 worker 的结果。在 parallel-oblivious 的      
	 * 情形下，所有参与者结果都相同，除非其根本   
	 * 没跑连接而无数据。parallel-aware 情形下，我们    
	 * 须考虑所有结果。每个 worker 可能见到不同的批次子  
	 * 集，我们要报告所有批次中最高的内存使用量。      
	 * 对其他值同样取最大值，理由与                           
	 * ExecHashAccumInstrumentation 中相同。                           
	 */
	if (hashstate->shared_info)
	{
		SharedHashInfo *shared_info = hashstate->shared_info;
		int			i;

		for (i = 0; i < shared_info->num_workers; ++i)
		{
			HashInstrumentation *worker_hi = &shared_info->hinstrument[i];

			hinstrument.nbuckets = Max(hinstrument.nbuckets,
									   worker_hi->nbuckets);
			hinstrument.nbuckets_original = Max(hinstrument.nbuckets_original,
												worker_hi->nbuckets_original);
			hinstrument.nbatch = Max(hinstrument.nbatch,
									 worker_hi->nbatch);
			hinstrument.nbatch_original = Max(hinstrument.nbatch_original,
											  worker_hi->nbatch_original);
			hinstrument.space_peak = Max(hinstrument.space_peak,
										 worker_hi->space_peak);
		}
	}

	if (hinstrument.nbatch > 0)
	{
		long		spacePeakKb = (hinstrument.space_peak + 1023) / 1024;

		if (es->format != EXPLAIN_FORMAT_TEXT)
		{
			ExplainPropertyInteger("Hash Buckets", NULL,
								   hinstrument.nbuckets, es);
			ExplainPropertyInteger("Original Hash Buckets", NULL,
								   hinstrument.nbuckets_original, es);
			ExplainPropertyInteger("Hash Batches", NULL,
								   hinstrument.nbatch, es);
			ExplainPropertyInteger("Original Hash Batches", NULL,
								   hinstrument.nbatch_original, es);
			ExplainPropertyInteger("Peak Memory Usage", "kB",
								   spacePeakKb, es);
		}
		else if (hinstrument.nbatch_original != hinstrument.nbatch ||
				 hinstrument.nbuckets_original != hinstrument.nbuckets)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Buckets: %d (originally %d)  Batches: %d (originally %d)  Memory Usage: %ldkB\n",
							 hinstrument.nbuckets,
							 hinstrument.nbuckets_original,
							 hinstrument.nbatch,
							 hinstrument.nbatch_original,
							 spacePeakKb);
		}
		else
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Buckets: %d  Batches: %d  Memory Usage: %ldkB\n",
							 hinstrument.nbuckets, hinstrument.nbatch,
							 spacePeakKb);
		}
	}
}

/*
 * 显示 memoize 命中/未命中/逐出与内存用量信息。     
 */
static void
show_memoize_info(MemoizeState *mstate, List *ancestors, ExplainState *es)
{
	Plan	   *plan = ((PlanState *) mstate)->plan;
	ListCell   *lc;
	List	   *context;
	StringInfoData keystr;
	char	   *separator = "";
	bool		useprefix;
	int64		memPeakKb;

	initStringInfo(&keystr);

	/*
	 * 很难想象会有 RTE 少于 2 个的 memoize 节点，但           
	 * 我们还是沿用本文件其他处的 useprefix 逻辑。        
	 */
	useprefix = list_length(es->rtable) > 1 || es->verbose;

	/* 设置反解析上下文 */
	context = set_deparse_context_plan(es->deparse_cxt,
									   plan,
									   ancestors);

	foreach(lc, ((Memoize *) plan)->param_exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);

		appendStringInfoString(&keystr, separator);

		appendStringInfoString(&keystr, deparse_expression(expr, context,
														   useprefix, false));
		separator = ", ";
	}

	if (es->format != EXPLAIN_FORMAT_TEXT)
	{
		ExplainPropertyText("Cache Key", keystr.data, es);
		ExplainPropertyText("Cache Mode", mstate->binary_mode ? "binary" : "logical", es);
	}
	else
	{
		ExplainIndentText(es);
		appendStringInfo(es->str, "Cache Key: %s\n", keystr.data);
		ExplainIndentText(es);
		appendStringInfo(es->str, "Cache Mode: %s\n", mstate->binary_mode ? "binary" : "logical");
	}

	pfree(keystr.data);

	if (!es->analyze)
		return;

	if (mstate->stats.cache_misses > 0)
	{
		/*
		 * mem_peak 仅在释放内存时设置，为 0 时须用            
		 * mem_used。        
		 */
		if (mstate->stats.mem_peak > 0)
			memPeakKb = (mstate->stats.mem_peak + 1023) / 1024;
		else
			memPeakKb = (mstate->mem_used + 1023) / 1024;

		if (es->format != EXPLAIN_FORMAT_TEXT)
		{
			ExplainPropertyInteger("Cache Hits", NULL, mstate->stats.cache_hits, es);
			ExplainPropertyInteger("Cache Misses", NULL, mstate->stats.cache_misses, es);
			ExplainPropertyInteger("Cache Evictions", NULL, mstate->stats.cache_evictions, es);
			ExplainPropertyInteger("Cache Overflows", NULL, mstate->stats.cache_overflows, es);
			ExplainPropertyInteger("Peak Memory Usage", "kB", memPeakKb, es);
		}
		else
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Hits: " UINT64_FORMAT "  Misses: " UINT64_FORMAT "  Evictions: " UINT64_FORMAT "  Overflows: " UINT64_FORMAT "  Memory Usage: " INT64_FORMAT "kB\n",
							 mstate->stats.cache_hits,
							 mstate->stats.cache_misses,
							 mstate->stats.cache_evictions,
							 mstate->stats.cache_overflows,
							 memPeakKb);
		}
	}

	if (mstate->shared_info == NULL)
		return;

	/* 显示来自并行 worker 的细节*/
	for (int n = 0; n < mstate->shared_info->num_workers; n++)
	{
		MemoizeInstrumentation *si;

		si = &mstate->shared_info->sinstrument[n];

		/*
		 * 跳过没干活的 worker。无需费心检查缓存命中       
		 * 情况，因为未命中总是先于命中发生。           
		 */
		if (si->cache_misses == 0)
			continue;

		if (es->workers_state)
			ExplainOpenWorker(n, es);

		/*
		 * 由于我们拿不到 worker 的 MemoizeState.mem_used          
		 * 字段，ExecEndMemoize 已设好   
		 * MemoizeInstrumentation.mem_peak 字段。无需像上面串行  
		 * 情形那样做零值检查了。                 
		 */
		memPeakKb = (si->mem_peak + 1023) / 1024;

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Hits: " UINT64_FORMAT "  Misses: " UINT64_FORMAT "  Evictions: " UINT64_FORMAT "  Overflows: " UINT64_FORMAT "  Memory Usage: " INT64_FORMAT "kB\n",
							 si->cache_hits, si->cache_misses,
							 si->cache_evictions, si->cache_overflows,
							 memPeakKb);
		}
		else
		{
			ExplainPropertyInteger("Cache Hits", NULL,
								   si->cache_hits, es);
			ExplainPropertyInteger("Cache Misses", NULL,
								   si->cache_misses, es);
			ExplainPropertyInteger("Cache Evictions", NULL,
								   si->cache_evictions, es);
			ExplainPropertyInteger("Cache Overflows", NULL,
								   si->cache_overflows, es);
			ExplainPropertyInteger("Peak Memory Usage", "kB", memPeakKb,
								   es);
		}

		if (es->workers_state)
			ExplainCloseWorker(n, es);
	}
}

/*
 * 显示哈希聚合的内存使用与批次信息。         
 */
static void
show_hashagg_info(AggState *aggstate, ExplainState *es)
{
	Agg		   *agg = (Agg *) aggstate->ss.ps.plan;
	int64		memPeakKb = (aggstate->hash_mem_peak + 1023) / 1024;

	if (agg->aggstrategy != AGG_HASHED &&
		agg->aggstrategy != AGG_MIXED)
		return;

	if (es->format != EXPLAIN_FORMAT_TEXT)
	{
		if (es->costs)
			ExplainPropertyInteger("Planned Partitions", NULL,
								   aggstate->hash_planned_partitions, es);

		/*
		 * 并行查询中 leader 可能没出过力，                 
		 * 可通过检查其内存用量发现。若它没干过活，   
		 * 就不显示其属性。                             
		 */
		if (es->analyze && aggstate->hash_mem_peak > 0)
		{
			ExplainPropertyInteger("HashAgg Batches", NULL,
								   aggstate->hash_batches_used, es);
			ExplainPropertyInteger("Peak Memory Usage", "kB", memPeakKb, es);
			ExplainPropertyInteger("Disk Usage", "kB",
								   aggstate->hash_disk_used, es);
		}
	}
	else
	{
		bool		gotone = false;

		if (es->costs && aggstate->hash_planned_partitions > 0)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Planned Partitions: %d",
							 aggstate->hash_planned_partitions);
			gotone = true;
		}

		/*
		 * 并行查询中 leader 可能没出过力，                 
		 * 可通过检查其内存用量发现。若它没干过活，   
		 * 就不显示其属性。                             
		 */
		if (es->analyze && aggstate->hash_mem_peak > 0)
		{
			if (!gotone)
				ExplainIndentText(es);
			else
				appendStringInfoSpaces(es->str, 2);

			appendStringInfo(es->str, "Batches: %d  Memory Usage: " INT64_FORMAT "kB",
							 aggstate->hash_batches_used, memPeakKb);
			gotone = true;

			/* 只在溢出到磁盘时才显示磁盘用量 */
			if (aggstate->hash_batches_used > 1)
			{
				appendStringInfo(es->str, "  Disk Usage: " UINT64_FORMAT "kB",
								 aggstate->hash_disk_used);
			}
		}

		if (gotone)
			appendStringInfoChar(es->str, '\n');
	}

	/* 显示各并行 worker 的统计信息 */
	if (es->analyze && aggstate->shared_info != NULL)
	{
		for (int n = 0; n < aggstate->shared_info->num_workers; n++)
		{
			AggregateInstrumentation *sinstrument;
			uint64		hash_disk_used;
			int			hash_batches_used;

			sinstrument = &aggstate->shared_info->sinstrument[n];
			/* 跳过没做任何事的 worker      */
			if (sinstrument->hash_mem_peak == 0)
				continue;
			hash_disk_used = sinstrument->hash_disk_used;
			hash_batches_used = sinstrument->hash_batches_used;
			memPeakKb = (sinstrument->hash_mem_peak + 1023) / 1024;

			if (es->workers_state)
				ExplainOpenWorker(n, es);

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				ExplainIndentText(es);

				appendStringInfo(es->str, "Batches: %d  Memory Usage: " INT64_FORMAT "kB",
								 hash_batches_used, memPeakKb);

				/* 只在溢出到磁盘时才显示磁盘用量 */
				if (hash_batches_used > 1)
					appendStringInfo(es->str, "  Disk Usage: " UINT64_FORMAT "kB",
									 hash_disk_used);
				appendStringInfoChar(es->str, '\n');
			}
			else
			{
				ExplainPropertyInteger("HashAgg Batches", NULL,
									   hash_batches_used, es);
				ExplainPropertyInteger("Peak Memory Usage", "kB", memPeakKb,
									   es);
				ExplainPropertyInteger("Disk Usage", "kB", hash_disk_used, es);
			}

			if (es->workers_state)
				ExplainCloseWorker(n, es);
		}
	}
}

/*
 * 若 EXPLAIN ANALYZE，显示 BitmapHeapScan 节点精确/有损页数    
 */
static void
show_tidbitmap_info(BitmapHeapScanState *planstate, ExplainState *es)
{
	if (es->format != EXPLAIN_FORMAT_TEXT)
	{
		ExplainPropertyInteger("Exact Heap Blocks", NULL,
							   planstate->exact_pages, es);
		ExplainPropertyInteger("Lossy Heap Blocks", NULL,
							   planstate->lossy_pages, es);
	}
	else
	{
		if (planstate->exact_pages > 0 || planstate->lossy_pages > 0)
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "Heap Blocks:");
			if (planstate->exact_pages > 0)
				appendStringInfo(es->str, " exact=%ld", planstate->exact_pages);
			if (planstate->lossy_pages > 0)
				appendStringInfo(es->str, " lossy=%ld", planstate->lossy_pages);
			appendStringInfoChar(es->str, '\n');
		}
	}
}

/*
 * 若是 EXPLAIN ANALYZE，则显示计划节点的 instrumentation 信息 
 *
 * "which" 标识要输出哪个 instrumentation 计数器  
 */
static void
show_instrumentation_count(const char *qlabel, int which,
						   PlanState *planstate, ExplainState *es)
{
	double		nfiltered;
	double		nloops;

	if (!es->analyze || !planstate->instrument)
		return;

	if (which == 2)
		nfiltered = planstate->instrument->nfiltered2;
	else
		nfiltered = planstate->instrument->nfiltered1;
	nloops = planstate->instrument->nloops;

	/* 文本模式下抑制零计数；它们没多大意思。          */
	if (nfiltered > 0 || es->format != EXPLAIN_FORMAT_TEXT)
	{
		if (nloops > 0)
			ExplainPropertyFloat(qlabel, NULL, nfiltered / nloops, 0, es);
		else
			ExplainPropertyFloat(qlabel, NULL, 0.0, 0, es);
	}
}

/*
 * 显示 ForeignScan 节点的额外信息。   
 */
static void
show_foreignscan_info(ForeignScanState *fsstate, ExplainState *es)
{
	FdwRoutine *fdwroutine = fsstate->fdwroutine;

	/* 让 FDW 输出它想要的任何字段    */
	if (((ForeignScan *) fsstate->ss.ps.plan)->operation != CMD_SELECT)
	{
		if (fdwroutine->ExplainDirectModify != NULL)
			fdwroutine->ExplainDirectModify(fsstate, es);
	}
	else
	{
		if (fdwroutine->ExplainForeignScan != NULL)
			fdwroutine->ExplainForeignScan(fsstate, es);
	}
}

/*
 * 显示在 Gather/Gather Merge 处求值的 initplan 参数。 
 */
static void
show_eval_params(Bitmapset *bms_params, ExplainState *es)
{
	int			paramid = -1;
	List	   *params = NIL;

	Assert(bms_params);

	while ((paramid = bms_next_member(bms_params, paramid)) >= 0)
	{
		char		param[32];

		snprintf(param, sizeof(param), "$%d", paramid);
		params = lappend(params, pstrdup(param));
	}

	if (params)
		ExplainPropertyList("Params Evaluated", params, es);
}

/*
 * 获取 EXPLAIN 中某个索引的名称 
 *
 * 我们允许插件在此取得控制，使含假想索引的计划得以 
 * 被解释。             
 *
 * 注意：返回的名称应是"原始"的；调用方将按需        
 * 施加引号。以前惯例是在此做引号处理，但我们不希望 
 * 在非文本输出格式中这样做。           
 */
static const char *
explain_get_index_name(Oid indexId)
{
	const char *result;

	if (explain_get_index_name_hook)
		result = (*explain_get_index_name_hook) (indexId);
	else
		result = NULL;
	if (result == NULL)
	{
		/* 默认行为：查系统目录               */
		result = get_rel_name(indexId);
		if (result == NULL)
			elog(ERROR, "cache lookup failed for index %u", indexId);
	}
	return result;
}

/*
 * 显示缓冲区详情。  
 */
static void
show_buffer_usage(ExplainState *es, const BufferUsage *usage, bool planning)
{
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		bool		has_shared = (usage->shared_blks_hit > 0 ||
								  usage->shared_blks_read > 0 ||
								  usage->shared_blks_dirtied > 0 ||
								  usage->shared_blks_written > 0);
		bool		has_local = (usage->local_blks_hit > 0 ||
								 usage->local_blks_read > 0 ||
								 usage->local_blks_dirtied > 0 ||
								 usage->local_blks_written > 0);
		bool		has_temp = (usage->temp_blks_read > 0 ||
								usage->temp_blks_written > 0);
		bool		has_timing = (!INSTR_TIME_IS_ZERO(usage->blk_read_time) ||
								  !INSTR_TIME_IS_ZERO(usage->blk_write_time));
		bool		has_temp_timing = (!INSTR_TIME_IS_ZERO(usage->temp_blk_read_time) ||
									   !INSTR_TIME_IS_ZERO(usage->temp_blk_write_time));
		bool		show_planning = (planning && (has_shared ||
												  has_local || has_temp || has_timing ||
												  has_temp_timing));

		if (show_planning)
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "Planning:\n");
			es->indent++;
		}

		/* 只显示正的计数器值。     */
		if (has_shared || has_local || has_temp)
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "Buffers:");

			if (has_shared)
			{
				appendStringInfoString(es->str, " shared");
				if (usage->shared_blks_hit > 0)
					appendStringInfo(es->str, " hit=%lld",
									 (long long) usage->shared_blks_hit);
				if (usage->shared_blks_read > 0)
					appendStringInfo(es->str, " read=%lld",
									 (long long) usage->shared_blks_read);
				if (usage->shared_blks_dirtied > 0)
					appendStringInfo(es->str, " dirtied=%lld",
									 (long long) usage->shared_blks_dirtied);
				if (usage->shared_blks_written > 0)
					appendStringInfo(es->str, " written=%lld",
									 (long long) usage->shared_blks_written);
				if (has_local || has_temp)
					appendStringInfoChar(es->str, ',');
			}
			if (has_local)
			{
				appendStringInfoString(es->str, " local");
				if (usage->local_blks_hit > 0)
					appendStringInfo(es->str, " hit=%lld",
									 (long long) usage->local_blks_hit);
				if (usage->local_blks_read > 0)
					appendStringInfo(es->str, " read=%lld",
									 (long long) usage->local_blks_read);
				if (usage->local_blks_dirtied > 0)
					appendStringInfo(es->str, " dirtied=%lld",
									 (long long) usage->local_blks_dirtied);
				if (usage->local_blks_written > 0)
					appendStringInfo(es->str, " written=%lld",
									 (long long) usage->local_blks_written);
				if (has_temp)
					appendStringInfoChar(es->str, ',');
			}
			if (has_temp)
			{
				appendStringInfoString(es->str, " temp");
				if (usage->temp_blks_read > 0)
					appendStringInfo(es->str, " read=%lld",
									 (long long) usage->temp_blks_read);
				if (usage->temp_blks_written > 0)
					appendStringInfo(es->str, " written=%lld",
									 (long long) usage->temp_blks_written);
			}
			appendStringInfoChar(es->str, '\n');
		}

		/* 如上，只显示正的计数器值。      */
		if (has_timing || has_temp_timing)
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "I/O Timings:");

			if (has_timing)
			{
				appendStringInfoString(es->str, " shared");
				if (!INSTR_TIME_IS_ZERO(usage->blk_read_time))
					appendStringInfo(es->str, " read=%0.3f",
									 INSTR_TIME_GET_MILLISEC(usage->blk_read_time));
				if (!INSTR_TIME_IS_ZERO(usage->blk_write_time))
					appendStringInfo(es->str, " write=%0.3f",
									 INSTR_TIME_GET_MILLISEC(usage->blk_write_time));
				if (has_temp_timing)
					appendStringInfoChar(es->str, ',');
			}
			if (has_temp_timing)
			{
				appendStringInfoString(es->str, " temp");
				if (!INSTR_TIME_IS_ZERO(usage->temp_blk_read_time))
					appendStringInfo(es->str, " read=%0.3f",
									 INSTR_TIME_GET_MILLISEC(usage->temp_blk_read_time));
				if (!INSTR_TIME_IS_ZERO(usage->temp_blk_write_time))
					appendStringInfo(es->str, " write=%0.3f",
									 INSTR_TIME_GET_MILLISEC(usage->temp_blk_write_time));
			}
			appendStringInfoChar(es->str, '\n');
		}

		if (show_planning)
			es->indent--;
	}
	else
	{
		ExplainPropertyInteger("Shared Hit Blocks", NULL,
							   usage->shared_blks_hit, es);
		ExplainPropertyInteger("Shared Read Blocks", NULL,
							   usage->shared_blks_read, es);
		ExplainPropertyInteger("Shared Dirtied Blocks", NULL,
							   usage->shared_blks_dirtied, es);
		ExplainPropertyInteger("Shared Written Blocks", NULL,
							   usage->shared_blks_written, es);
		ExplainPropertyInteger("Local Hit Blocks", NULL,
							   usage->local_blks_hit, es);
		ExplainPropertyInteger("Local Read Blocks", NULL,
							   usage->local_blks_read, es);
		ExplainPropertyInteger("Local Dirtied Blocks", NULL,
							   usage->local_blks_dirtied, es);
		ExplainPropertyInteger("Local Written Blocks", NULL,
							   usage->local_blks_written, es);
		ExplainPropertyInteger("Temp Read Blocks", NULL,
							   usage->temp_blks_read, es);
		ExplainPropertyInteger("Temp Written Blocks", NULL,
							   usage->temp_blks_written, es);
		if (track_io_timing)
		{
			ExplainPropertyFloat("I/O Read Time", "ms",
								 INSTR_TIME_GET_MILLISEC(usage->blk_read_time),
								 3, es);
			ExplainPropertyFloat("I/O Write Time", "ms",
								 INSTR_TIME_GET_MILLISEC(usage->blk_write_time),
								 3, es);
			ExplainPropertyFloat("Temp I/O Read Time", "ms",
								 INSTR_TIME_GET_MILLISEC(usage->temp_blk_read_time),
								 3, es);
			ExplainPropertyFloat("Temp I/O Write Time", "ms",
								 INSTR_TIME_GET_MILLISEC(usage->temp_blk_write_time),
								 3, es);
		}
	}
}

/*
 * 显示 WAL 用量。   
 */
static void
show_wal_usage(ExplainState *es, const WalUsage *usage)
{
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		/* 只显示正的计数器值。     */
		if ((usage->wal_records > 0) || (usage->wal_fpi > 0) ||
			(usage->wal_bytes > 0))
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "WAL:");

			if (usage->wal_records > 0)
				appendStringInfo(es->str, " records=%lld",
								 (long long) usage->wal_records);
			if (usage->wal_fpi > 0)
				appendStringInfo(es->str, " fpi=%lld",
								 (long long) usage->wal_fpi);
			if (usage->wal_bytes > 0)
				appendStringInfo(es->str, " bytes=" UINT64_FORMAT,
								 usage->wal_bytes);
			appendStringInfoChar(es->str, '\n');
		}
	}
	else
	{
		ExplainPropertyInteger("WAL Records", NULL,
							   usage->wal_records, es);
		ExplainPropertyInteger("WAL FPI", NULL,
							   usage->wal_fpi, es);
		ExplainPropertyUInteger("WAL Bytes", NULL,
								usage->wal_bytes, es);
	}
}

/*
 * 为 IndexScan 或 IndexOnlyScan 追加一些额外细节。    
 */
static void
ExplainIndexScanDetails(Oid indexid, ScanDirection indexorderdir,
						ExplainState *es)
{
	const char *indexname = explain_get_index_name(indexid);

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		if (ScanDirectionIsBackward(indexorderdir))
			appendStringInfoString(es->str, " Backward");
		appendStringInfo(es->str, " using %s", quote_identifier(indexname));
	}
	else
	{
		const char *scandir;

		switch (indexorderdir)
		{
			case BackwardScanDirection:
				scandir = "Backward";
				break;
			case ForwardScanDirection:
				scandir = "Forward";
				break;
			default:
				scandir = "???";
				break;
		}
		ExplainPropertyText("Scan Direction", scandir, es);
		ExplainPropertyText("Index Name", indexname, es);
	}
}

/*
 * 显示 Scan 节点目标。   
 */
static void
ExplainScanTarget(Scan *plan, ExplainState *es)
{
	ExplainTargetRel((Plan *) plan, plan->scanrelid, es);
}

/*
 * 显示 ModifyTable 节点的目标   
 *
 * 此处显示名义目标（即原始查询中被指名的关系）。  
 * 若实际目标与名义目标不同，则由 show_modifytable_info()      
 * 来显示它们。         
 */
static void
ExplainModifyTarget(ModifyTable *plan, ExplainState *es)
{
	ExplainTargetRel((Plan *) plan, plan->nominalRelation, es);
}

/*
 * 显示扫描或修改节点的目标关系。    
 */
static void
ExplainTargetRel(Plan *plan, Index rti, ExplainState *es)
{
	char	   *objectname = NULL;
	char	   *namespace = NULL;
	const char *objecttag = NULL;
	RangeTblEntry *rte;
	char	   *refname;

	rte = rt_fetch(rti, es->rtable);
	refname = (char *) list_nth(es->rtable_names, rti - 1);
	if (refname == NULL)
		refname = rte->eref->aliasname;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_ForeignScan:
		case T_CustomScan:
		case T_ModifyTable:
			/* 断言在真实关系上       */
			Assert(rte->rtekind == RTE_RELATION);
			objectname = get_rel_name(rte->relid);
			if (es->verbose)
				namespace = get_namespace_name_or_temp(get_rel_namespace(rte->relid));
			objecttag = "Relation Name";
			break;
		case T_FunctionScan:
			{
				FunctionScan *fscan = (FunctionScan *) plan;

				/* 断言在 RangeFunction 上    */
				Assert(rte->rtekind == RTE_FUNCTION);

				/*
				 * 若表达式仍是单个函数的调用，            
				 * 我们就能取得该函数的真实名称。      
				 * 否则就作罢。（即使原是单函数调用，     
				 * 也可能已把它化简掉了。）                     
				 */
				if (list_length(fscan->functions) == 1)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) linitial(fscan->functions);

					if (IsA(rtfunc->funcexpr, FuncExpr))
					{
						FuncExpr   *funcexpr = (FuncExpr *) rtfunc->funcexpr;
						Oid			funcid = funcexpr->funcid;

						objectname = get_func_name(funcid);
						if (es->verbose)
							namespace = get_namespace_name_or_temp(get_func_namespace(funcid));
					}
				}
				objecttag = "Function Name";
			}
			break;
		case T_TableFuncScan:
			Assert(rte->rtekind == RTE_TABLEFUNC);
			objectname = "xmltable";
			objecttag = "Table Function Name";
			break;
		case T_ValuesScan:
			Assert(rte->rtekind == RTE_VALUES);
			break;
		case T_CteScan:
			/* 断言其位于非自引用 CTE 上     */
			Assert(rte->rtekind == RTE_CTE);
			Assert(!rte->self_reference);
			objectname = rte->ctename;
			objecttag = "CTE Name";
			break;
		case T_NamedTuplestoreScan:
			Assert(rte->rtekind == RTE_NAMEDTUPLESTORE);
			objectname = rte->enrname;
			objecttag = "Tuplestore Name";
			break;
		case T_WorkTableScan:
			/* 断言其位于自引用 CTE 上    */
			Assert(rte->rtekind == RTE_CTE);
			Assert(rte->self_reference);
			objectname = rte->ctename;
			objecttag = "CTE Name";
			break;
		default:
			break;
	}

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		appendStringInfoString(es->str, " on");
		if (namespace != NULL)
			appendStringInfo(es->str, " %s.%s", quote_identifier(namespace),
							 quote_identifier(objectname));
		else if (objectname != NULL)
			appendStringInfo(es->str, " %s", quote_identifier(objectname));
		if (objectname == NULL || strcmp(refname, objectname) != 0)
			appendStringInfo(es->str, " %s", quote_identifier(refname));
	}
	else
	{
		if (objecttag != NULL && objectname != NULL)
			ExplainPropertyText(objecttag, objectname, es);
		if (namespace != NULL)
			ExplainPropertyText("Schema", namespace, es);
		ExplainPropertyText("Alias", refname, es);
	}
}

/*
 * 显示 ModifyTable 节点的额外信息     
 *
 * 此处有三个目标。其一，若目标表不止一个，或与    
 * 名义目标不同，就标出实际目标。其二，给 FDW       
 * 有机会显示外部目标的额外信息。其三，显示 ON           
 * CONFLICT 的相关信息。                         
 */
static void
show_modifytable_info(ModifyTableState *mtstate, List *ancestors,
					  ExplainState *es)
{
	ModifyTable *node = (ModifyTable *) mtstate->ps.plan;
	const char *operation;
	const char *foperation;
	bool		labeltargets;
	int			j;
	List	   *idxNames = NIL;
	ListCell   *lst;

	switch (node->operation)
	{
		case CMD_INSERT:
			operation = "Insert";
			foperation = "Foreign Insert";
			break;
		case CMD_UPDATE:
			operation = "Update";
			foperation = "Foreign Update";
			break;
		case CMD_DELETE:
			operation = "Delete";
			foperation = "Foreign Delete";
			break;
		case CMD_MERGE:
			operation = "Merge";
			/* XXX 目前不支持，但避免编译器噪声    */
			foperation = "Foreign Merge";
			break;
		default:
			operation = "???";
			foperation = "Foreign ???";
			break;
	}

	/* 是否应显式标记目标关系？         */
	labeltargets = (mtstate->mt_nrels > 1 ||
					(mtstate->mt_nrels == 1 &&
					 mtstate->resultRelInfo[0].ri_RangeTableIndex != node->nominalRelation));

	if (labeltargets)
		ExplainOpenGroup("Target Tables", "Target Tables", false, es);

	for (j = 0; j < mtstate->mt_nrels; j++)
	{
		ResultRelInfo *resultRelInfo = mtstate->resultRelInfo + j;
		FdwRoutine *fdwroutine = resultRelInfo->ri_FdwRoutine;

		if (labeltargets)
		{
			/* 为此目标开一个组     */
			ExplainOpenGroup("Target Table", NULL, true, es);

			/*
			 * 文本模式下给每个目标加操作类型修饰，以便   
			 * ExplainTargetRel 输出的 " on foo" 会更顺口。    
			 */
			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				ExplainIndentText(es);
				appendStringInfoString(es->str,
									   fdwroutine ? foperation : operation);
			}

			/* 标识目标    */
			ExplainTargetRel((Plan *) node,
							 resultRelInfo->ri_RangeTableIndex,
							 es);

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				appendStringInfoChar(es->str, '\n');
				es->indent++;
			}
		}

		/* 需要时给 FDW 机会     */
		if (!resultRelInfo->ri_usesFdwDirectModify &&
			fdwroutine != NULL &&
			fdwroutine->ExplainForeignModify != NULL)
		{
			List	   *fdw_private = (List *) list_nth(node->fdwPrivLists, j);

			fdwroutine->ExplainForeignModify(mtstate,
											 resultRelInfo,
											 fdw_private,
											 j,
											 es);
		}

		if (labeltargets)
		{
			/* 撤销我们在文本格式中添加的缩进*/
			if (es->format == EXPLAIN_FORMAT_TEXT)
				es->indent--;

			/* 关闭该组    */
			ExplainCloseGroup("Target Table", NULL, true, es);
		}
	}

	/* 收集 ON CONFLICT 仲裁索引的名称    */
	foreach(lst, node->arbiterIndexes)
	{
		char	   *indexname = get_rel_name(lfirst_oid(lst));

		idxNames = lappend(idxNames, indexname);
	}

	if (node->onConflictAction != ONCONFLICT_NONE)
	{
		ExplainPropertyText("Conflict Resolution",
							node->onConflictAction == ONCONFLICT_NOTHING ?
							"NOTHING" : "UPDATE",
							es);

		/*
		 * 当 DO NOTHING 变体隐式忽略所有冲突时，         
		 * 就完全不显示仲裁索引  
		 */
		if (idxNames)
			ExplainPropertyList("Conflict Arbiter Indexes", idxNames, es);

		/* ON CONFLICT DO UPDATE WHERE 条件被特殊显示       */
		if (node->onConflictWhere)
		{
			show_upper_qual((List *) node->onConflictWhere, "Conflict Filter",
							&mtstate->ps, ancestors, es);
			show_instrumentation_count("Rows Removed by Conflict Filter", 1, &mtstate->ps, es);
		}

		/* EXPLAIN ANALYZE 显示每个拟插入元组的实际结果        */
		if (es->analyze && mtstate->ps.instrument)
		{
			double		total;
			double		insert_path;
			double		other_path;

			InstrEndLoop(outerPlanState(mtstate)->instrument);

			/* 统计源行数                 */
			total = outerPlanState(mtstate)->instrument->ntuples;
			other_path = mtstate->ps.instrument->ntuples2;
			insert_path = total - other_path;

			ExplainPropertyFloat("Tuples Inserted", NULL,
								 insert_path, 0, es);
			ExplainPropertyFloat("Conflicting Tuples", NULL,
								 other_path, 0, es);
		}
	}
	else if (node->operation == CMD_MERGE)
	{
		/* EXPLAIN ANALYZE 显示已处理的元组数 */
		if (es->analyze && mtstate->ps.instrument)
		{
			double		total;
			double		insert_path;
			double		update_path;
			double		delete_path;
			double		skipped_path;

			InstrEndLoop(outerPlanState(mtstate)->instrument);

			/* 统计源行数                 */
			total = outerPlanState(mtstate)->instrument->ntuples;
			insert_path = mtstate->mt_merge_inserted;
			update_path = mtstate->mt_merge_updated;
			delete_path = mtstate->mt_merge_deleted;
			skipped_path = total - insert_path - update_path - delete_path;
			Assert(skipped_path >= 0);

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				if (total > 0)
				{
					ExplainIndentText(es);
					appendStringInfoString(es->str, "Tuples:");
					if (insert_path > 0)
						appendStringInfo(es->str, " inserted=%.0f", insert_path);
					if (update_path > 0)
						appendStringInfo(es->str, " updated=%.0f", update_path);
					if (delete_path > 0)
						appendStringInfo(es->str, " deleted=%.0f", delete_path);
					if (skipped_path > 0)
						appendStringInfo(es->str, " skipped=%.0f", skipped_path);
					appendStringInfoChar(es->str, '\n');
				}
			}
			else
			{
				ExplainPropertyFloat("Tuples Inserted", NULL, insert_path, 0, es);
				ExplainPropertyFloat("Tuples Updated", NULL, update_path, 0, es);
				ExplainPropertyFloat("Tuples Deleted", NULL, delete_path, 0, es);
				ExplainPropertyFloat("Tuples Skipped", NULL, skipped_path, 0, es);
			}
		}
	}

	if (labeltargets)
		ExplainCloseGroup("Target Tables", "Target Tables", false, es);
}

/*
 * 解释 Append、MergeAppend、BitmapAnd 或 BitmapOr    
 * 节点的组成计划。    
 *
 * ancestors 列表应当已包含这些计划的直接父节点          
 * 。   
 */
static void
ExplainMemberNodes(PlanState **planstates, int nplans,
				   List *ancestors, ExplainState *es)
{
	int			j;

	for (j = 0; j < nplans; j++)
		ExplainNode(planstates[j], ancestors,
					"Member", NULL, es);
}

/*
 * 报告 Append 或 MergeAppend 节点被剪枝的子节点。      
 *
 * nplans 表示存活子计划的数量。     
 * nchildren 表示 Plan 中子节点的原始数量；            
 * 其中一些可能已被运行期剪枝代码剪掉。          
 */
static void
ExplainMissingMembers(int nplans, int nchildren, ExplainState *es)
{
	if (nplans < nchildren || es->format != EXPLAIN_FORMAT_TEXT)
		ExplainPropertyInteger("Subplans Removed", NULL,
							   nchildren - nplans, es);
}

/*
 * 解释一组 SubPlan（或 initPlan，其也用 SubPlan 节点）。    
 *
 * ancestors 列表应当已包含这些 SubPlan 的直接父节点       
 * 。      
 */
static void
ExplainSubPlans(List *plans, List *ancestors,
				const char *relationship, ExplainState *es)
{
	ListCell   *lst;

	foreach(lst, plans)
	{
		SubPlanState *sps = (SubPlanState *) lfirst(lst);
		SubPlan    *sp = sps->subplan;

		/*
		 * 多个 SubPlan 节点可引用同一物理子计划（相同     
		 * 的 plan_id，即它在 PlannedStmt.subplans 中的下标）。    
		 * 子计划只应打印一次，所以要记录哪些已打印过。 
		 * 此状态必须跨整棵计划树全局有效，因为重复节点 
		 * 或位于不同的计划节点中，例如位图索引扫描的   
		 * indexqual 与父堆扫描的 recheck qual 都可能引用它。   
		 * （这种情形下我们不介意把子计划显示为挂在哪   
		 * 个计划节点上。）   
		 */
		if (bms_is_member(sp->plan_id, es->printed_subplans))
			continue;
		es->printed_subplans = bms_add_member(es->printed_subplans,
											  sp->plan_id);

		/*
		 * 把 SubPlan 节点当作其内部计划节点的祖先，这样 
		 * ruleutils.c 才能找到子计划参数的引用对象   
		 * 。        
		 */
		ancestors = lcons(sp, ancestors);

		ExplainNode(sps->planstate, ancestors,
					relationship, sp->plan_name, es);

		ancestors = list_delete_first(ancestors);
	}
}

/*
 * 解释 CustomScan 的一组子节点。    
 */
static void
ExplainCustomChildren(CustomScanState *css, List *ancestors, ExplainState *es)
{
	ListCell   *cell;
	const char *label =
		(list_length(css->custom_ps) != 1 ? "children" : "child");

	foreach(cell, css->custom_ps)
		ExplainNode((PlanState *) lfirst(cell), ancestors, label, NULL, es);
}

/*
 * 创建收集各 worker 数据的按计划节点工作区。     
 *
 * 与各 worker 相关的输出先被临时"搁置"进一个单独    
 * 缓冲区；处理完该节点所有数据后，再把它合并    
 * 进主输出流。这样就能为每个 worker 生成一个内聚     
 * 的字段子组，即使产生这些字段的代码散落在本文件   
 * 的不同位置。这类被搁置的字段组，其格式化由           
 * ExplainOpenSetAsideGroup 和                            
 * ExplainSaveGroup/ExplainRestoreGroup 管理。                    
 */
static ExplainWorkersState *
ExplainCreateWorkersState(int num_workers)
{
	ExplainWorkersState *wstate;

	wstate = (ExplainWorkersState *) palloc(sizeof(ExplainWorkersState));
	wstate->num_workers = num_workers;
	wstate->worker_inited = (bool *) palloc0(num_workers * sizeof(bool));
	wstate->worker_str = (StringInfoData *)
		palloc0(num_workers * sizeof(StringInfoData));
	wstate->worker_state_save = (int *) palloc(num_workers * sizeof(int));
	return wstate;
}

/*
 * 开始或恢复向 worker N 的搁置组输出。            
 */
static void
ExplainOpenWorker(int n, ExplainState *es)
{
	ExplainWorkersState *wstate = es->workers_state;

	Assert(wstate);
	Assert(n >= 0 && n < wstate->num_workers);

	/* 保存原输出缓冲区指针   */
	wstate->prev_str = es->str;

	if (!wstate->worker_inited[n])
	{
		/* 第一次经过，因此为该 worker 创建缓冲区    */
		initStringInfo(&wstate->worker_str[n]);
		es->str = &wstate->worker_str[n];

		/*
		 * 为该 worker 字段组压入合适的初始格式状态。   
		 * 我们允许额外一层逻辑嵌套，因为该组最终会被   
		 * 包进外层的 "Workers" 组。                       
		 */
		ExplainOpenSetAsideGroup("Worker", NULL, true, 2, es);

		/*
		 * 在非 TEXT 格式下总会输出一个 "Worker Number" 字段，  
		 * 即使该 worker 没别的数据。   
		 */
		if (es->format != EXPLAIN_FORMAT_TEXT)
			ExplainPropertyInteger("Worker Number", NULL, n, es);

		wstate->worker_inited[n] = true;
	}
	else
	{
		/* 恢复对已为其输出过一些数据的 worker 的输出      */
		es->str = &wstate->worker_str[n];

		/* 恢复上次 ExplainCloseWorker() 保存的格式化状态  */
		ExplainRestoreGroup(es, 2, &wstate->worker_state_save[n]);
	}

	/*
	 * 在 TEXT 格式下，给该 worker 的第一行输出加上      
	 * "Worker N:" 前缀。此后所有额外的行都应比 "Worker N"   
	 * 行多缩进一级。            
	 */
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		if (es->str->len == 0)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Worker %d:  ", n);
		}

		es->indent++;
	}
}

/*
 * 结束 worker N 的输出；须与之前 ExplainOpenWorker 调用配对   
 */
static void
ExplainCloseWorker(int n, ExplainState *es)
{
	ExplainWorkersState *wstate = es->workers_state;

	Assert(wstate);
	Assert(n >= 0 && n < wstate->num_workers);
	Assert(wstate->worker_inited[n]);

	/*
	 * 为再次调用 ExplainOpenWorker() 保存格式化状态，然后   
	 * 弹出格式化栈。    
	 */
	ExplainSaveGroup(es, 2, &wstate->worker_state_save[n]);

	/*
	 * 在 TEXT 格式下，如果我们没有产生任何输出行，      
	 * 那就把 ExplainOpenWorker 发的半截行截掉。（这是为了  
	 * 避免虚假输出，如 show_buffer_usage 选择不输出时）     
	 * 同时还要修正缩进级别。                       
	 */
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		while (es->str->len > 0 && es->str->data[es->str->len - 1] != '\n')
			es->str->data[--(es->str->len)] = '\0';

		es->indent--;
	}

	/* 恢复先前的输出缓冲区指针*/
	es->str = wstate->prev_str;
}

/*
 * 打印当前节点各 worker 的信息，然后释放 ExplainWorkersState 
 */
static void
ExplainFlushWorkersState(ExplainState *es)
{
	ExplainWorkersState *wstate = es->workers_state;

	ExplainOpenGroup("Workers", "Workers", false, es);
	for (int i = 0; i < wstate->num_workers; i++)
	{
		if (wstate->worker_inited[i])
		{
			/* 须与之前的 ExplainOpenSetAsideGroup 调用匹配  */
			ExplainOpenGroup("Worker", NULL, true, es);
			appendStringInfoString(es->str, wstate->worker_str[i].data);
			ExplainCloseGroup("Worker", NULL, true, es);

			pfree(wstate->worker_str[i].data);
		}
	}
	ExplainCloseGroup("Workers", "Workers", false, es);

	pfree(wstate->worker_inited);
	pfree(wstate->worker_str);
	pfree(wstate->worker_state_save);
	pfree(wstate);
}

/*
 * 解释形如未标注项列表的属性，例如排序键或目标。   
 * "data" 是一个 C 字符串列表。                     
 */
void
ExplainPropertyList(const char *qlabel, List *data, ExplainState *es)
{
	ListCell   *lc;
	bool		first = true;

	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			ExplainIndentText(es);
			appendStringInfo(es->str, "%s: ", qlabel);
			foreach(lc, data)
			{
				if (!first)
					appendStringInfoString(es->str, ", ");
				appendStringInfoString(es->str, (const char *) lfirst(lc));
				first = false;
			}
			appendStringInfoChar(es->str, '\n');
			break;

		case EXPLAIN_FORMAT_XML:
			ExplainXMLTag(qlabel, X_OPENING, es);
			foreach(lc, data)
			{
				char	   *str;

				appendStringInfoSpaces(es->str, es->indent * 2 + 2);
				appendStringInfoString(es->str, "<Item>");
				str = escape_xml((const char *) lfirst(lc));
				appendStringInfoString(es->str, str);
				pfree(str);
				appendStringInfoString(es->str, "</Item>\n");
			}
			ExplainXMLTag(qlabel, X_CLOSING, es);
			break;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, es->indent * 2);
			escape_json(es->str, qlabel);
			appendStringInfoString(es->str, ": [");
			foreach(lc, data)
			{
				if (!first)
					appendStringInfoString(es->str, ", ");
				escape_json(es->str, (const char *) lfirst(lc));
				first = false;
			}
			appendStringInfoChar(es->str, ']');
			break;

		case EXPLAIN_FORMAT_YAML:
			ExplainYAMLLineStarting(es);
			appendStringInfo(es->str, "%s: ", qlabel);
			foreach(lc, data)
			{
				appendStringInfoChar(es->str, '\n');
				appendStringInfoSpaces(es->str, es->indent * 2 + 2);
				appendStringInfoString(es->str, "- ");
				escape_yaml(es->str, (const char *) lfirst(lc));
			}
			break;
	}
}

/*
 * 解释一个形如另一个列表内未标注项列表的属性。        
 * "data" 是一个 C 字符串列表。        
 */
void
ExplainPropertyListNested(const char *qlabel, List *data, ExplainState *es)
{
	ListCell   *lc;
	bool		first = true;

	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
		case EXPLAIN_FORMAT_XML:
			ExplainPropertyList(qlabel, data, es);
			return;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfoChar(es->str, '[');
			foreach(lc, data)
			{
				if (!first)
					appendStringInfoString(es->str, ", ");
				escape_json(es->str, (const char *) lfirst(lc));
				first = false;
			}
			appendStringInfoChar(es->str, ']');
			break;

		case EXPLAIN_FORMAT_YAML:
			ExplainYAMLLineStarting(es);
			appendStringInfoString(es->str, "- [");
			foreach(lc, data)
			{
				if (!first)
					appendStringInfoString(es->str, ", ");
				escape_yaml(es->str, (const char *) lfirst(lc));
				first = false;
			}
			appendStringInfoChar(es->str, ']');
			break;
	}
}

/*
 * 解释简单属性。     
 *
 * 若 "numeric" 为真，值就是一个数字（或其他在     
 * JSON 中免引号的值）。 
 *
 * 若 unit 非 NULL，文本格式会在该值之后显示它。      
 *
 * 通常不直接调用它，而应经由某个数据类型专用         
 * 例程 ExplainPropertyText、ExplainPropertyInteger 等。         
 */
static void
ExplainProperty(const char *qlabel, const char *unit, const char *value,
				bool numeric, ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			ExplainIndentText(es);
			if (unit)
				appendStringInfo(es->str, "%s: %s %s\n", qlabel, value, unit);
			else
				appendStringInfo(es->str, "%s: %s\n", qlabel, value);
			break;

		case EXPLAIN_FORMAT_XML:
			{
				char	   *str;

				appendStringInfoSpaces(es->str, es->indent * 2);
				ExplainXMLTag(qlabel, X_OPENING | X_NOWHITESPACE, es);
				str = escape_xml(value);
				appendStringInfoString(es->str, str);
				pfree(str);
				ExplainXMLTag(qlabel, X_CLOSING | X_NOWHITESPACE, es);
				appendStringInfoChar(es->str, '\n');
			}
			break;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, es->indent * 2);
			escape_json(es->str, qlabel);
			appendStringInfoString(es->str, ": ");
			if (numeric)
				appendStringInfoString(es->str, value);
			else
				escape_json(es->str, value);
			break;

		case EXPLAIN_FORMAT_YAML:
			ExplainYAMLLineStarting(es);
			appendStringInfo(es->str, "%s: ", qlabel);
			if (numeric)
				appendStringInfoString(es->str, value);
			else
				escape_yaml(es->str, value);
			break;
	}
}

/*
 * 解释字符串值属性。      
 */
void
ExplainPropertyText(const char *qlabel, const char *value, ExplainState *es)
{
	ExplainProperty(qlabel, NULL, value, false, es);
}

/*
 * 解释一个整数值属性。     
 */
void
ExplainPropertyInteger(const char *qlabel, const char *unit, int64 value,
					   ExplainState *es)
{
	char		buf[32];

	snprintf(buf, sizeof(buf), INT64_FORMAT, value);
	ExplainProperty(qlabel, unit, buf, true, es);
}

/*
 * 解释一个无符号整数值属性。     
 */
void
ExplainPropertyUInteger(const char *qlabel, const char *unit, uint64 value,
						ExplainState *es)
{
	char		buf[32];

	snprintf(buf, sizeof(buf), UINT64_FORMAT, value);
	ExplainProperty(qlabel, unit, buf, true, es);
}

/*
 * 解释一个浮点值属性，使用指定个数的           
 * 小数位。      
 */
void
ExplainPropertyFloat(const char *qlabel, const char *unit, double value,
					 int ndigits, ExplainState *es)
{
	char	   *buf;

	buf = psprintf("%.*f", ndigits, value);
	ExplainProperty(qlabel, unit, buf, true, es);
	pfree(buf);
}

/*
 * 解释布尔值属性。       
 */
void
ExplainPropertyBool(const char *qlabel, bool value, ExplainState *es)
{
	ExplainProperty(qlabel, NULL, value ? "true" : "false", true, es);
}

/*
 * 打开一组相关对象。     
 *
 * objtype 是组对象的类型，labelname 是它在外层对象       
 * 中的标签（如有）。  
 *
 * 若 labeled 为真，成员是带标签的属性；若为假，  
 * 则成员是不带标签的对象。              
 */
void
ExplainOpenGroup(const char *objtype, const char *labelname,
				 bool labeled, ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* 无事可做  */
			break;

		case EXPLAIN_FORMAT_XML:
			ExplainXMLTag(objtype, X_OPENING, es);
			es->indent++;
			break;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, 2 * es->indent);
			if (labelname)
			{
				escape_json(es->str, labelname);
				appendStringInfoString(es->str, ": ");
			}
			appendStringInfoChar(es->str, labeled ? '{' : '[');

			/*
			 * 在 JSON 格式下，grouping_stack 是一个整数列表。0  
			 * 表示此层无输出，1 表示有输出（所以下一   
			 * 一项需要逗号）。参见                             
			 * ExplainJSONLineEnding() 
			 */
			es->grouping_stack = lcons_int(0, es->grouping_stack);
			es->indent++;
			break;

		case EXPLAIN_FORMAT_YAML:

			/*
			 * 在 YAML 格式下，grouping 栈是一个整数列表。0     
			 * 表示此层无输出，且该层无标签、必须带 "- "   
			 * 标记。参见                                      
			 * ExplainYAMLLineStarting() 
			 */
			ExplainYAMLLineStarting(es);
			if (labelname)
			{
				appendStringInfo(es->str, "%s: ", labelname);
				es->grouping_stack = lcons_int(1, es->grouping_stack);
			}
			else
			{
				appendStringInfoString(es->str, "- ");
				es->grouping_stack = lcons_int(0, es->grouping_stack);
			}
			es->indent++;
			break;
	}
}

/*
 * 关闭一组相关对象。      
 * 参数必须与对应的 ExplainOpenGroup 调用匹配。     
 */
void
ExplainCloseGroup(const char *objtype, const char *labelname,
				  bool labeled, ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* 无事可做  */
			break;

		case EXPLAIN_FORMAT_XML:
			es->indent--;
			ExplainXMLTag(objtype, X_CLOSING, es);
			break;

		case EXPLAIN_FORMAT_JSON:
			es->indent--;
			appendStringInfoChar(es->str, '\n');
			appendStringInfoSpaces(es->str, 2 * es->indent);
			appendStringInfoChar(es->str, labeled ? '}' : ']');
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;

		case EXPLAIN_FORMAT_YAML:
			es->indent--;
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;
	}
}

/*
 * 打开一组相关对象，但不实际输出任何数据。  
 *
 * 准备格式化状态，就像我们即将开始一个具有指定   
 * 属性的组，但并不实际输出。此调用之后的输出     
 * 可被重定向到单独的输出缓冲区，最终执行一次常规   
 * 的 ExplainOpenGroup 调用（同参数）之后附加到           
 * 主输出缓冲区。                                    
 *
 * 额外的 "depth" 参数是新组比当前更深的层数。它可       
 * 能大于一，以防最终输出还会包在更多嵌套组层中。  
 * 假定准备该组输出时，无需跟踪这些层级的格式      
 * 化状态。                                                          
 *
 * 不存在 ExplainCloseSetAsideGroup —— 当前用法下总是由  
 * ExplainSaveGroup 弹出此状态。  
 */
static void
ExplainOpenSetAsideGroup(const char *objtype, const char *labelname,
						 bool labeled, int depth, ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* 无事可做  */
			break;

		case EXPLAIN_FORMAT_XML:
			es->indent += depth;
			break;

		case EXPLAIN_FORMAT_JSON:
			es->grouping_stack = lcons_int(0, es->grouping_stack);
			es->indent += depth;
			break;

		case EXPLAIN_FORMAT_YAML:
			if (labelname)
				es->grouping_stack = lcons_int(1, es->grouping_stack);
			else
				es->grouping_stack = lcons_int(0, es->grouping_stack);
			es->indent += depth;
			break;
	}
}

/*
 * 弹出一层分组状态，允许稍后重新压入。        
 *
 * 通常在 ExplainOpenSetAsideGroup 之后使用；传入与    
 * 其相同的 "depth"。    
 *
 * 它不应产生任何输出。若需保存状态就存到    
 * *state_save。目前一个整数保存区对所有格式都够用，  
 * 但将来可能需要重新审视这一点。              
 */
static void
ExplainSaveGroup(ExplainState *es, int depth, int *state_save)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* 无事可做  */
			break;

		case EXPLAIN_FORMAT_XML:
			es->indent -= depth;
			break;

		case EXPLAIN_FORMAT_JSON:
			es->indent -= depth;
			*state_save = linitial_int(es->grouping_stack);
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;

		case EXPLAIN_FORMAT_YAML:
			es->indent -= depth;
			*state_save = linitial_int(es->grouping_stack);
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;
	}
}

/*
 * 重新压入一层分组状态，撤销 ExplainSaveGroup 产生的效果。  
 */
static void
ExplainRestoreGroup(ExplainState *es, int depth, int *state_save)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* 无事可做  */
			break;

		case EXPLAIN_FORMAT_XML:
			es->indent += depth;
			break;

		case EXPLAIN_FORMAT_JSON:
			es->grouping_stack = lcons_int(*state_save, es->grouping_stack);
			es->indent += depth;
			break;

		case EXPLAIN_FORMAT_YAML:
			es->grouping_stack = lcons_int(*state_save, es->grouping_stack);
			es->indent += depth;
			break;
	}
}

/*
 * 发出一个永远不会有成员的"哑"组。 
 *
 * objtype 是组对象的类型，labelname 是它在外层对象       
 * 中的标签（如有）。  
 */
static void
ExplainDummyGroup(const char *objtype, const char *labelname, ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* 无事可做  */
			break;

		case EXPLAIN_FORMAT_XML:
			ExplainXMLTag(objtype, X_CLOSE_IMMEDIATE, es);
			break;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, 2 * es->indent);
			if (labelname)
			{
				escape_json(es->str, labelname);
				appendStringInfoString(es->str, ": ");
			}
			escape_json(es->str, objtype);
			break;

		case EXPLAIN_FORMAT_YAML:
			ExplainYAMLLineStarting(es);
			if (labelname)
			{
				escape_yaml(es->str, labelname);
				appendStringInfoString(es->str, ": ");
			}
			else
			{
				appendStringInfoString(es->str, "- ");
			}
			escape_yaml(es->str, objtype);
			break;
	}
}

/*
 * 发出输出起始的样板内容。 
 *
 * 这与处理一个子组足够不同，故需一对单独的         
 * 子例程。                   
 */
void
ExplainBeginOutput(ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* 无事可做  */
			break;

		case EXPLAIN_FORMAT_XML:
			appendStringInfoString(es->str,
								   "<explain xmlns=\"http://www.postgresql.org/2009/explain\">\n");
			es->indent++;
			break;

		case EXPLAIN_FORMAT_JSON:
			/* 顶层结构是一个计划数组        */
			appendStringInfoChar(es->str, '[');
			es->grouping_stack = lcons_int(0, es->grouping_stack);
			es->indent++;
			break;

		case EXPLAIN_FORMAT_YAML:
			es->grouping_stack = lcons_int(0, es->grouping_stack);
			break;
	}
}

/*
 * 发出输出结尾样板内容。  
 */
void
ExplainEndOutput(ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* 无事可做  */
			break;

		case EXPLAIN_FORMAT_XML:
			es->indent--;
			appendStringInfoString(es->str, "</explain>");
			break;

		case EXPLAIN_FORMAT_JSON:
			es->indent--;
			appendStringInfoString(es->str, "\n]");
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;

		case EXPLAIN_FORMAT_YAML:
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;
	}
}

/*
 * 在多个计划之间放置合适的分隔符      
 */
void
ExplainSeparatePlans(ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* 加一个空行  */
			appendStringInfoChar(es->str, '\n');
			break;

		case EXPLAIN_FORMAT_XML:
		case EXPLAIN_FORMAT_JSON:
		case EXPLAIN_FORMAT_YAML:
			/* 无事可做  */
			break;
	}
}

/*
 * 输出开/闭 XML 标签。     
 *
 * "flags" 须含 X_OPENING、X_CLOSING 或 X_CLOSE_IMMEDIATE。   
 * 可选地，按位或上 X_NOWHITESPACE 以抑制通常加               
 * 的 
 *
 * 空白。XML 对标签名的限制比其他输出格式更严，例如   
 * 其中不得含空白或斜杠。将无效字符替换为短横线，   
 * "I/O Read Time" 就变成 "I-O-Read-Time"。                
 */
static void
ExplainXMLTag(const char *tagname, int flags, ExplainState *es)
{
	const char *s;
	const char *valid = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.";

	if ((flags & X_NOWHITESPACE) == 0)
		appendStringInfoSpaces(es->str, 2 * es->indent);
	appendStringInfoCharMacro(es->str, '<');
	if ((flags & X_CLOSING) != 0)
		appendStringInfoCharMacro(es->str, '/');
	for (s = tagname; *s; s++)
		appendStringInfoChar(es->str, strchr(valid, *s) ? *s : '-');
	if ((flags & X_CLOSE_IMMEDIATE) != 0)
		appendStringInfoString(es->str, " /");
	appendStringInfoCharMacro(es->str, '>');
	if ((flags & X_NOWHITESPACE) == 0)
		appendStringInfoCharMacro(es->str, '\n');
}

/*
 * 缩进文本格式行。  
 *
 * 每级缩进两个空格。但为并行 worker 输出数据时当前  
 * 行上可能已有数据（参见 ExplainOpenWorker）；此时就不要  
 * 再缩进。                                                 
 */
static void
ExplainIndentText(ExplainState *es)
{
	Assert(es->format == EXPLAIN_FORMAT_TEXT);
	if (es->str->len == 0 || es->str->data[es->str->len - 1] == '\n')
		appendStringInfoSpaces(es->str, es->indent * 2);
}

/*
 * 发出 JSON 行尾。   
 *
 * JSON 要求每个属性后有逗号，但最后一个除外。              
 * 为此，JSON 格式下每个属性输出的文本都恰好起始于上一 
 * 换行符之前（如适用包括逗号）。   
 */
static void
ExplainJSONLineEnding(ExplainState *es)
{
	Assert(es->format == EXPLAIN_FORMAT_JSON);
	if (linitial_int(es->grouping_stack) != 0)
		appendStringInfoChar(es->str, ',');
	else
		linitial_int(es->grouping_stack) = 1;
	appendStringInfoChar(es->str, '\n');
}

/*
 * 缩进 YAML 行。 
 *
 * YAML 行通常按每个缩进级别两个空格缩进。               
 * 属性输出的文本起始于上一换行符之前，但未标注   
 * 组的第一个属性例外：它起始于引入组的 "- " 之后。组  
 * 的第一个属性与开头的 "- " 出现在同一行                    
 * 上。                                                             
 */
static void
ExplainYAMLLineStarting(ExplainState *es)
{
	Assert(es->format == EXPLAIN_FORMAT_YAML);
	if (linitial_int(es->grouping_stack) == 0)
	{
		linitial_int(es->grouping_stack) = 1;
	}
	else
	{
		appendStringInfoChar(es->str, '\n');
		appendStringInfoSpaces(es->str, es->indent * 2);
	}
}

/*
 * YAML 是 JSON 的超集；可惜 YAML 的引号规则复杂得离谱 
 * （见 http://yaml.org/spec/1.2/spec.html 的 5.3 与 7.3.3 节）， 
 * 所以我们索性把一切都加引号。空字符串、带前导         
 * 或尾随空白的字符串、含种种特殊字符的串都须加引  
 * 号，否则输出就是无效的；其他看似无害的串（如 "0xa"  
 * 或 "true"）也须加引号，免得被当成十六进制数        
 * 或布尔值，而不是字符串                                          
 * 。                           
 */
static void
escape_yaml(StringInfo buf, const char *str)
{
	escape_json(buf, str);
}
