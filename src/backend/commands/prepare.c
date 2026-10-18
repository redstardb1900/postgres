/*-------------------------------------------------------------------------
 *
 * prepare.c
 *	  通过 PREPARE、EXECUTE 和 DEALLOCATE 实现的可准备 SQL 语句
 *
 * 本模块还实现经由扩展 FE/BE 查询协议访问的
 * 预备语句的存储。
 *
 *
 * Copyright (c) 2002-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/commands/prepare.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/createas.h"
#include "commands/prepare.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"


/*
 * 存储预备查询的哈希表。这是每个后端
 * 一份：查询计划不在后端之间共享。
 * 此哈希表的键是 PREPARE 和 EXECUTE 的参数
 * （语句名）；条目是 PreparedStatement 结构体。
 */
static HTAB *prepared_queries = NULL;

static void InitQueryHashTable(void);
static ParamListInfo EvaluateParams(ParseState *pstate,
									PreparedStatement *pstmt, List *params,
									EState *estate);
static Datum build_regtype_array(Oid *param_types, int num_params);

/*
 * 实现 'PREPARE' 工具语句。
 */
void
PrepareQuery(ParseState *pstate, PrepareStmt *stmt,
			 int stmt_location, int stmt_len)
{
	RawStmt    *rawstmt;
	CachedPlanSource *plansource;
	Oid		   *argtypes = NULL;
	int			nargs;
	List	   *query_list;

	/*
	 * 禁止空字符串语句名（与协议级未命名语句
	 * 冲突）。
	 */
	if (!stmt->name || stmt->name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PSTATEMENT_DEFINITION),
				 errmsg("invalid statement name: must not be empty")));

	/*
	 * 需要把内含语句包进 RawStmt 节点，
	 * 才能交给解析分析。
	 */
	rawstmt = makeNode(RawStmt);
	rawstmt->stmt = stmt->query;
	rawstmt->stmt_location = stmt_location;
	rawstmt->stmt_len = stmt_len;

	/*
	 * 在做解析分析之前创建 CachedPlanSource，
	 * 因为它需要看到未修改的原始解析树。
	 */
	plansource = CreateCachedPlan(rawstmt, pstate->p_sourcetext,
								  CreateCommandTag(stmt->query));

	/* 把 TypeName 列表转换为类型 OID 数组 */
	nargs = list_length(stmt->argtypes);

	if (nargs)
	{
		int			i;
		ListCell   *l;

		argtypes = palloc_array(Oid, nargs);
		i = 0;

		foreach(l, stmt->argtypes)
		{
			TypeName   *tn = lfirst(l);
			Oid			toid = typenameTypeId(pstate, tn);

			argtypes[i++] = toid;
		}
	}

	/*
	 * 用这些参数类型分析语句（从我们上方传入的任何
	 * 参数对它不可见），允许从上下文
	 * 推断未知参数的信息。
	 * 重写查询。结果可能是 0、1 或多个查询。
	 */
	query_list = pg_analyze_and_rewrite_varparams(rawstmt, pstate->p_sourcetext,
												  &argtypes, &nargs, NULL);

	/* 完成 CachedPlanSource 的填充 */
	CompleteCachedPlan(plansource,
					   query_list,
					   NULL,
					   argtypes,
					   nargs,
					   NULL,
					   NULL,
					   CURSOR_OPT_PARALLEL_OK,	/* 允许并行模式 */
					   true);	/* 固定结果 */

	/*
	 * 保存结果。
	 */
	StorePreparedStatement(stmt->name,
						   plansource,
						   true);
}

/*
 * ExecuteQuery --- 实现 'EXECUTE' 工具语句。
 *
 * 此代码还支持 CREATE TABLE ... AS EXECUTE。这种情况
 * 以传入非空 intoClause 表示。DestReceiver 已经为
 * CREATE TABLE AS 正确设置，但我们仍须在这里
 * 做几处其他调整。
 */
void
ExecuteQuery(ParseState *pstate,
			 ExecuteStmt *stmt, IntoClause *intoClause,
			 ParamListInfo params,
			 DestReceiver *dest, QueryCompletion *qc)
{
	PreparedStatement *entry;
	CachedPlan *cplan;
	List	   *plan_list;
	ParamListInfo paramLI = NULL;
	EState	   *estate = NULL;
	Portal		portal;
	char	   *query_string;
	int			eflags;
	long		count;

	/* 在哈希表中查找它 */
	entry = FetchPreparedStatement(stmt->name, true);

	/* 不应找到非固定结果的缓存计划 */
	if (!entry->plansource->fixed_result)
		elog(ERROR, "EXECUTE does not support variable-result cached plans");

	/* 若有参数，就求值 */
	if (entry->plansource->num_params > 0)
	{
		/*
		 * 需要一个 EState 来求参数值；直到查询结束
		 * 都不能删除它，以防参数是按引用传递的。
		 * 注意传入的 "params" 可能被参数
		 * 表达式引用。
		 */
		estate = CreateExecutorState();
		estate->es_param_list_info = params;
		paramLI = EvaluateParams(pstate, entry, stmt->params, estate);
	}

	/* 创建新门户来运行查询 */
	portal = CreateNewPortal();
	/* 不要在 pg_cursors 中显示该门户，它仅供内部使用 */
	portal->visible = false;

	/* 把计划保存的查询字符串复制到门户内存中 */
	query_string = MemoryContextStrdup(portal->portalContext,
									   entry->plansource->query_string);

	/* 如有需要就重新规划，并为门户递增计划引用计数 */
	cplan = GetCachedPlan(entry->plansource, paramLI, NULL, NULL);
	plan_list = cplan->stmt_list;

	/*
	 * 不要在 GetCachedPlan 和 PortalDefineQuery 之间添加任何
	 * 可能抛出错误的逻辑，否则会泄漏计划引用计数。
	 */
	PortalDefineQuery(portal,
					  NULL,
					  query_string,
					  entry->plansource->commandTag,
					  plan_list,
					  cplan);

	/*
	 * 对于 CREATE TABLE ... AS EXECUTE，我们必须验证预备语句
	 * 是产生元组的语句。目前我们坚持它必须是普通 SELECT。
	 * 将来我们可能考虑支持其他东西，比如
	 * INSERT ... RETURNING，但有几个问题要先解决，特别是
	 * 这种情况下 WITH NO DATA 应如何处理（我们真的想
	 * 抑制执行吗？），以及如何把决定 OID 的 eflags
	 * 传下去（PortalStart 在这种情况下不会处理它们，
	 * 而且执行器会不会处理也不清楚）。
	 *
	 * 对于 CREATE TABLE ... AS EXECUTE，我们还必须确保合适的
	 * eflags 和 fetch 计数传给 PortalStart/PortalRun。
	 */
	if (intoClause)
	{
		PlannedStmt *pstmt;

		if (list_length(plan_list) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("prepared statement is not a SELECT")));
		pstmt = linitial_node(PlannedStmt, plan_list);
		if (pstmt->commandType != CMD_SELECT)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("prepared statement is not a SELECT")));

		/* 设置合适的 eflags */
		eflags = GetIntoRelEFlags(intoClause);

		/* 并告诉 PortalRun 是否要执行到底 */
		if (intoClause->skipData)
			count = 0;
		else
			count = FETCH_ALL;
	}
	else
	{
		/* 普通 EXECUTE */
		eflags = 0;
		count = FETCH_ALL;
	}

	/*
	 * 按需运行门户。
	 */
	PortalStart(portal, paramLI, eflags, GetActiveSnapshot());

	(void) PortalRun(portal, count, false, true, dest, dest, qc);

	PortalDrop(portal, false);

	if (estate)
		FreeExecutorState(estate);

	/* 无需 pfree 其他内存，MemoryContext 会被重置 */
}

/*
 * EvaluateParams: 求一组参数的值。
 *
 * pstate: 解析状态
 * pstmt: 我们为其获取参数的语句。
 * params: 给定参数表达式的列表（原始解析器输出！）
 * estate: 要使用的执行器状态。
 *
 * 返回填充好的 ParamListInfo —— 它稍后可传给
 * CreateQueryDesc()，使执行器能在查询执行期间
 * 使用这些参数。
 */
static ParamListInfo
EvaluateParams(ParseState *pstate, PreparedStatement *pstmt, List *params,
			   EState *estate)
{
	Oid		   *param_types = pstmt->plansource->param_types;
	int			num_params = pstmt->plansource->num_params;
	int			nparams = list_length(params);
	ParamListInfo paramLI;
	List	   *exprstates;
	ListCell   *l;
	int			i;

	if (nparams != num_params)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("wrong number of parameters for prepared statement \"%s\"",
						pstmt->stmt_name),
				 errdetail("Expected %d parameters but got %d.",
						   num_params, nparams)));

	/* 若没有参数就快速退出 */
	if (num_params == 0)
		return NULL;

	/*
	 * 我们必须对表达式做解析分析。由于解析器
	 * 不喜欢其输入被涂改，先复制一份。
	 */
	params = copyObject(params);

	i = 0;
	foreach(l, params)
	{
		Node	   *expr = lfirst(l);
		Oid			expected_type_id = param_types[i];
		Oid			given_type_id;

		expr = transformExpr(pstate, expr, EXPR_KIND_EXECUTE_PARAMETER);

		given_type_id = exprType(expr);

		expr = coerce_to_target_type(pstate, expr, given_type_id,
									 expected_type_id, -1,
									 COERCION_ASSIGNMENT,
									 COERCE_IMPLICIT_CAST,
									 -1);

		if (expr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("parameter $%d of type %s cannot be coerced to the expected type %s",
							i + 1,
							format_type_be(given_type_id),
							format_type_be(expected_type_id)),
					 errhint("You will need to rewrite or cast the expression."),
					 parser_errposition(pstate, exprLocation(lfirst(l)))));

		/* 处理成品表达式中的排序规则。 */
		assign_expr_collations(pstate, expr);

		lfirst(l) = expr;
		i++;
	}

	/* 为执行准备表达式 */
	exprstates = ExecPrepareExprList(params, estate);

	paramLI = makeParamList(num_params);

	i = 0;
	foreach(l, exprstates)
	{
		ExprState  *n = (ExprState *) lfirst(l);
		ParamExternData *prm = &paramLI->params[i];

		prm->ptype = param_types[i];
		prm->pflags = PARAM_FLAG_CONST;
		prm->value = ExecEvalExprSwitchContext(n,
											   GetPerTupleExprContext(estate),
											   &prm->isnull);

		i++;
	}

	return paramLI;
}


/*
 * 首次使用时初始化查询哈希表。
 */
static void
InitQueryHashTable(void)
{
	HASHCTL		hash_ctl;

	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(PreparedStatement);

	prepared_queries = hash_create("Prepared Queries",
								   32,
								   &hash_ctl,
								   HASH_ELEM | HASH_STRINGS);
}

/*
 * 用指定键把与查询有关的所有数据存入哈希表。
 * 传入的 CachedPlanSource 应为 "unsaved" 状态，
 * 以防这里出错；一旦创建了哈希表条目，
 * 我们就会保存它。
 */
void
StorePreparedStatement(const char *stmt_name,
					   CachedPlanSource *plansource,
					   bool from_sql)
{
	PreparedStatement *entry;
	TimestampTz cur_ts = GetCurrentStatementStartTimestamp();
	bool		found;

	/* 如有必要，初始化哈希表 */
	if (!prepared_queries)
		InitQueryHashTable();

	/* 向哈希表添加条目 */
	entry = (PreparedStatement *) hash_search(prepared_queries,
											  stmt_name,
											  HASH_ENTER,
											  &found);

	/* 不应得到重复条目 */
	if (found)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_PSTATEMENT),
				 errmsg("prepared statement \"%s\" already exists",
						stmt_name)));

	/* 填充哈希表条目 */
	entry->plansource = plansource;
	entry->from_sql = from_sql;
	entry->prepare_time = cur_ts;

	/* 现在把 CachedPlanSource 移到永久内存就安全了 */
	SaveCachedPlan(plansource);
}

/*
 * 在哈希表中查找现有查询。若查询实际上不存在，
 * 根据第二个参数抛 ereport(ERROR) 或返回 NULL。
 *
 * 注意：这不会强制被引用的 plancache 条目有效，
 * 因为并非所有调用方都在意。
 */
PreparedStatement *
FetchPreparedStatement(const char *stmt_name, bool throwError)
{
	PreparedStatement *entry;

	/*
	 * 若哈希表尚未初始化，它不可能存储任何东西，
	 * 因此也不可能存储我们的计划。
	 */
	if (prepared_queries)
		entry = (PreparedStatement *) hash_search(prepared_queries,
												  stmt_name,
												  HASH_FIND,
												  NULL);
	else
		entry = NULL;

	if (!entry && throwError)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
				 errmsg("prepared statement \"%s\" does not exist",
						stmt_name)));

	return entry;
}

/*
 * 给定预备语句，确定它将产生的结果 tupledesc。
 * 若执行不返回元组则返回 NULL。
 *
 * 注意：结果在当前内存上下文中创建或复制。
 */
TupleDesc
FetchPreparedStatementResultDesc(PreparedStatement *stmt)
{
	/*
	 * 因为我们不允许预备语句的结果 tupdesc 改变，
	 * 这里无需担心重新验证缓存计划。
	 */
	Assert(stmt->plansource->fixed_result);
	if (stmt->plansource->resultDesc)
		return CreateTupleDescCopy(stmt->plansource->resultDesc);
	else
		return NULL;
}

/*
 * 给定返回元组的预备语句，提取查询
 * targetlist。若语句没有可确定的 targetlist，
 * 返回 NIL。
 *
 * 注意：这相当丑陋，但由于它只用于像对 EXECUTE 命令
 * 做 Describe Statement 这样的边角情形，
 * 我们不太担心效率。
 */
List *
FetchPreparedStatementTargetList(PreparedStatement *stmt)
{
	List	   *tlist;

	/* 获取计划的主 targetlist */
	tlist = CachedPlanGetTargetList(stmt->plansource, NULL);

	/* 复制到调用方上下文，以防计划失效 */
	return copyObject(tlist);
}

/*
 * 实现 'DEALLOCATE' 工具语句：从存储中
 * 删除指定计划。
 */
void
DeallocateQuery(DeallocateStmt *stmt)
{
	if (stmt->name)
		DropPreparedStatement(stmt->name, true);
	else
		DropAllPreparedStatements();
}

/*
 * DEALLOCATE 的内部版本
 *
 * 若 showError 为 false，删除不存在的语句是空操作。
 */
void
DropPreparedStatement(const char *stmt_name, bool showError)
{
	PreparedStatement *entry;

	/* 找到查询的哈希表条目；若需要就报错 */
	entry = FetchPreparedStatement(stmt_name, showError);

	if (entry)
	{
		/* 释放 plancache 条目 */
		DropCachedPlan(entry->plansource);

		/* 现在可以移除哈希表条目 */
		hash_search(prepared_queries, entry->stmt_name, HASH_REMOVE, NULL);
	}
}

/*
 * 删除所有缓存语句。
 */
void
DropAllPreparedStatements(void)
{
	HASH_SEQ_STATUS seq;
	PreparedStatement *entry;

	/* 没有缓存任何东西 */
	if (!prepared_queries)
		return;

	/* 遍历缓存 */
	hash_seq_init(&seq, prepared_queries);
	while ((entry = hash_seq_search(&seq)) != NULL)
	{
		/* 释放 plancache 条目 */
		DropCachedPlan(entry->plansource);

		/* 现在可以移除哈希表条目 */
		hash_search(prepared_queries, entry->stmt_name, HASH_REMOVE, NULL);
	}
}

/*
 * 实现 'EXPLAIN EXECUTE' 工具语句。
 *
 * 除非我们在做 EXPLAIN CREATE TABLE AS EXECUTE，否则 "into"
 * 为 NULL；在那种情况下执行查询应该会创建那张表。
 *
 * 注意：传入的 queryString 是 EXPLAIN EXECUTE 的，不是
 * 原始 PREPARE 的；后一个字符串我们从 plancache 获取。
 */
void
ExplainExecuteQuery(ExecuteStmt *execstmt, IntoClause *into, ExplainState *es,
					const char *queryString, ParamListInfo params,
					QueryEnvironment *queryEnv)
{
	PreparedStatement *entry;
	const char *query_string;
	CachedPlan *cplan;
	List	   *plan_list;
	ListCell   *p;
	ParamListInfo paramLI = NULL;
	EState	   *estate = NULL;
	instr_time	planstart;
	instr_time	planduration;
	BufferUsage bufusage_start,
				bufusage;

	if (es->buffers)
		bufusage_start = pgBufferUsage;
	INSTR_TIME_SET_CURRENT(planstart);

	/* 在哈希表中查找它 */
	entry = FetchPreparedStatement(execstmt->name, true);

	/* 不应找到非固定结果的缓存计划 */
	if (!entry->plansource->fixed_result)
		elog(ERROR, "EXPLAIN EXECUTE does not support variable-result cached plans");

	query_string = entry->plansource->query_string;

	/* 若有参数，就求值 */
	if (entry->plansource->num_params)
	{
		ParseState *pstate;

		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = queryString;

		/*
		 * 需要一个 EState 来求参数值；直到查询结束
		 * 都不能删除它，以防参数是按引用传递的。
		 * 注意传入的 "params" 可能被参数
		 * 表达式引用。
		 */
		estate = CreateExecutorState();
		estate->es_param_list_info = params;

		paramLI = EvaluateParams(pstate, entry, execstmt->params, estate);
	}

	/* 如有需要就重新规划，并获取临时引用计数 */
	cplan = GetCachedPlan(entry->plansource, paramLI,
						  CurrentResourceOwner, queryEnv);

	INSTR_TIME_SET_CURRENT(planduration);
	INSTR_TIME_SUBTRACT(planduration, planstart);

	/* 计算缓冲区计数器的差值。 */
	if (es->buffers)
	{
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);
	}

	plan_list = cplan->stmt_list;

	/* 解释每条查询 */
	foreach(p, plan_list)
	{
		PlannedStmt *pstmt = lfirst_node(PlannedStmt, p);

		if (pstmt->commandType != CMD_UTILITY)
			ExplainOnePlan(pstmt, into, es, query_string, paramLI, queryEnv,
						   &planduration, (es->buffers ? &bufusage : NULL));
		else
			ExplainOneUtility(pstmt->utilityStmt, into, es, query_string,
							  paramLI, queryEnv);

		/* 无需 CommandCounterIncrement，ExplainOnePlan 已做过 */

		/* 用合适的分隔符分隔计划 */
		if (lnext(plan_list, p) != NULL)
			ExplainSeparatePlans(es);
	}

	if (estate)
		FreeExecutorState(estate);

	ReleaseCachedPlan(cplan, CurrentResourceOwner);
}

/*
 * 此集合返回函数读取所有预备语句，并返回
 * (name, statement, prepare_time, param_types, from_sql,
 * generic_plans, custom_plans) 的集合。
 */
Datum
pg_prepared_statement(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/*
	 * 在一次哈希表扫描中把所有元组放入 tuplestore。
	 * 这避免了哈希表可能在调用之间变化的任何问题。
	 */
	InitMaterializedSRF(fcinfo, 0);

	/* 哈希表可能未初始化 */
	if (prepared_queries)
	{
		HASH_SEQ_STATUS hash_seq;
		PreparedStatement *prep_stmt;

		hash_seq_init(&hash_seq, prepared_queries);
		while ((prep_stmt = hash_seq_search(&hash_seq)) != NULL)
		{
			TupleDesc	result_desc;
			Datum		values[8];
			bool		nulls[8] = {0};

			result_desc = prep_stmt->plansource->resultDesc;

			values[0] = CStringGetTextDatum(prep_stmt->stmt_name);
			values[1] = CStringGetTextDatum(prep_stmt->plansource->query_string);
			values[2] = TimestampTzGetDatum(prep_stmt->prepare_time);
			values[3] = build_regtype_array(prep_stmt->plansource->param_types,
											prep_stmt->plansource->num_params);
			if (result_desc)
			{
				Oid		   *result_types;

				result_types = palloc_array(Oid, result_desc->natts);
				for (int i = 0; i < result_desc->natts; i++)
					result_types[i] = result_desc->attrs[i].atttypid;
				values[4] = build_regtype_array(result_types, result_desc->natts);
			}
			else
			{
				/* 没有结果描述符（例如 DML 语句） */
				nulls[4] = true;
			}
			values[5] = BoolGetDatum(prep_stmt->from_sql);
			values[6] = Int64GetDatumFast(prep_stmt->plansource->num_generic_plans);
			values[7] = Int64GetDatumFast(prep_stmt->plansource->num_custom_plans);

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
	}

	return (Datum) 0;
}

/*
 * 此实用函数接收 C 的 Oid 数组，返回指向一维
 * regtype Postgres 数组的 Datum。空数组以零元素
 * 数组返回，而不是 NULL。
 */
static Datum
build_regtype_array(Oid *param_types, int num_params)
{
	Datum	   *tmp_ary;
	ArrayType  *result;
	int			i;

	tmp_ary = palloc_array(Datum, num_params);

	for (i = 0; i < num_params; i++)
		tmp_ary[i] = ObjectIdGetDatum(param_types[i]);

	result = construct_array_builtin(tmp_ary, num_params, REGTYPEOID);
	return PointerGetDatum(result);
}
