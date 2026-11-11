/*-------------------------------------------------------------------------
 *
 * analyze.c
 *	  将原始解析树转换为查询树          
 *
 * 对于可优化的语句，我们小心地获取合适的锁并        
 * 作用于每个被引用的表，后端的其他模块会保留    
 * 或在依赖结果之前重新获取这些锁。因此对这些        
 * 语句做深入的语义分析是没有问题的。对于实用   
 * 命令，这里不会获取任何锁（即便获取了，也无         
 * 法确定执行时仍持有它们）。因此对实用命令的        
 * 一般规则是将其不加变换地倒入 Query 节点。                
 * DECLARE CURSOR、EXPLAIN 和 CREATE TABLE AS 是例外，因为它们   
 * 包含可优化的语句，需要加以变换。          
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/parser/analyze.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/queryjumble.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_cte.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_merge.h"
#include "parser/parse_oper.h"
#include "parser/parse_param.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/* 供插件在解析分析结束时取得控制权的钩子*/
post_parse_analyze_hook_type post_parse_analyze_hook = NULL;

static Query *transformOptionalSelectInto(ParseState *pstate, Node *parseTree);
static Query *transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt);
static Query *transformInsertStmt(ParseState *pstate, InsertStmt *stmt);
static OnConflictExpr *transformOnConflictClause(ParseState *pstate,
												 OnConflictClause *onConflictClause);
static int	count_rowexpr_columns(ParseState *pstate, Node *expr);
static Query *transformSelectStmt(ParseState *pstate, SelectStmt *stmt);
static Query *transformValuesClause(ParseState *pstate, SelectStmt *stmt);
static Query *transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt);
static Node *transformSetOperationTree(ParseState *pstate, SelectStmt *stmt,
									   bool isTopLevel, List **targetlist);
static void determineRecursiveColTypes(ParseState *pstate,
									   Node *larg, List *nrtargetlist);
static Query *transformReturnStmt(ParseState *pstate, ReturnStmt *stmt);
static Query *transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt);
static List *transformReturningList(ParseState *pstate, List *returningList);
static Query *transformPLAssignStmt(ParseState *pstate,
									PLAssignStmt *stmt);
static Query *transformDeclareCursorStmt(ParseState *pstate,
										 DeclareCursorStmt *stmt);
static Query *transformExplainStmt(ParseState *pstate,
								   ExplainStmt *stmt);
static Query *transformCreateTableAsStmt(ParseState *pstate,
										 CreateTableAsStmt *stmt);
static Query *transformCallStmt(ParseState *pstate,
								CallStmt *stmt);
static void transformLockingClause(ParseState *pstate, Query *qry,
								   LockingClause *lc, bool pushedDown);
#ifdef RAW_EXPRESSION_COVERAGE_TEST
static bool test_raw_expression_coverage(Node *node, void *context);
#endif


/*
 * parse_analyze_fixedparams
 *		分析原始解析树并将其转换为 Query 形式。 
 *
 * 可选择提供关于 $n 参数类型的信息。                
 * paramTypes[] 未定义的 $n 索引引用会被拒绝。            
 *
 * 结果是一个 Query 节点。可优化的语句需要大量的        
 * 变换，而实用型语句只是挂在虚拟的                 
 * CMD_UTILITY Query 节点上。 
 */
Query *
parse_analyze_fixedparams(RawStmt *parseTree, const char *sourceText,
						  const Oid *paramTypes, int numParams,
						  QueryEnvironment *queryEnv)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;
	JumbleState *jstate = NULL;

	Assert(sourceText != NULL); /* 自 8.4 起必需  */

	pstate->p_sourcetext = sourceText;

	if (numParams > 0)
		setup_parse_fixed_parameters(pstate, paramTypes, numParams);

	pstate->p_queryEnv = queryEnv;

	query = transformTopLevelStmt(pstate, parseTree);

	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);

	free_parsestate(pstate);

	pgstat_report_query_id(query->queryId, false);

	return query;
}

/*
 * parse_analyze_varparams
 *
 * 当允许从上下文推断 $n 符号的数据类型时，        
 * 可使用此变体。传入的 paramTypes[] 数组可以修改，   
 * 或通过 repalloc 扩大。           
 */
Query *
parse_analyze_varparams(RawStmt *parseTree, const char *sourceText,
						Oid **paramTypes, int *numParams,
						QueryEnvironment *queryEnv)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;
	JumbleState *jstate = NULL;

	Assert(sourceText != NULL); /* 自 8.4 起必需  */

	pstate->p_sourcetext = sourceText;

	setup_parse_variable_parameters(pstate, paramTypes, numParams);

	pstate->p_queryEnv = queryEnv;

	query = transformTopLevelStmt(pstate, parseTree);

	/* 确保参数类型没有问题             */
	check_variable_parameters(pstate, query);

	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);

	free_parsestate(pstate);

	pgstat_report_query_id(query->queryId, false);

	return query;
}

/*
 * parse_analyze_withcb
 *
 * 当调用者提供自己的解析器回调来解析参数                 
 * 以及可能的其他事项时使用。      
 */
Query *
parse_analyze_withcb(RawStmt *parseTree, const char *sourceText,
					 ParserSetupHook parserSetup,
					 void *parserSetupArg,
					 QueryEnvironment *queryEnv)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;
	JumbleState *jstate = NULL;

	Assert(sourceText != NULL); /* 自 8.4 起必需  */

	pstate->p_sourcetext = sourceText;
	pstate->p_queryEnv = queryEnv;
	(*parserSetup) (pstate, parserSetupArg);

	query = transformTopLevelStmt(pstate, parseTree);

	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);

	free_parsestate(pstate);

	pgstat_report_query_id(query->queryId, false);

	return query;
}


/*
 * parse_sub_analyze
 *		递归分析子语句的入口点。                  
 */
Query *
parse_sub_analyze(Node *parseTree, ParseState *parentParseState,
				  CommonTableExpr *parentCTE,
				  bool locked_from_parent,
				  bool resolve_unknowns)
{
	ParseState *pstate = make_parsestate(parentParseState);
	Query	   *query;

	pstate->p_parent_cte = parentCTE;
	pstate->p_locked_from_parent = locked_from_parent;
	pstate->p_resolve_unknowns = resolve_unknowns;

	query = transformStmt(pstate, parseTree);

	free_parsestate(pstate);

	return query;
}

/*
 * transformTopLevelStmt -
 *	  将解析树变换成查询树。        
 *
 * 此函数只负责把语句位置数据从 RawStmt 转移到              
 * 最终的 Query 中。                   
 */
Query *
transformTopLevelStmt(ParseState *pstate, RawStmt *parseTree)
{
	Query	   *result;

	/* 当前处于顶层，允许 SELECT INTO  */
	result = transformOptionalSelectInto(pstate, parseTree->stmt);

	result->stmt_location = parseTree->stmt_location;
	result->stmt_len = parseTree->stmt_len;

	return result;
}

/*
 * transformOptionalSelectInto -
 *	  若 SELECT 带 INTO，转换为 CREATE TABLE AS。
 *
 * 这里唯一一件与 transformStmt() 不同的事，是把        
 * SELECT ... INTO 转换成 CREATE TABLE AS。由于实用语句         
 * 不允许出现在更大的语句中，所以只有位于解析树      
 * 顶层时才被允许，所以我们只在进入递归                
 * transformStmt() 之前。  
 */
static Query *
transformOptionalSelectInto(ParseState *pstate, Node *parseTree)
{
	if (IsA(parseTree, SelectStmt))
	{
		SelectStmt *stmt = (SelectStmt *) parseTree;

		/* 若是集合操作树，则下钻到最左侧的 SelectStmt     */
		while (stmt && stmt->op != SETOP_NONE)
			stmt = stmt->larg;
		Assert(stmt && IsA(stmt, SelectStmt) && stmt->larg == NULL);

		if (stmt->intoClause)
		{
			CreateTableAsStmt *ctas = makeNode(CreateTableAsStmt);

			ctas->query = parseTree;
			ctas->into = stmt->intoClause;
			ctas->objtype = OBJECT_TABLE;
			ctas->is_select_into = true;

			/*
			 * 从 SelectStmt 中移除 intoClause。这样即使            
			 * transformSelectStmt 发现它已被设置，也能安全      
			 * 地报错（说明 INTO 在不允许的位置）。     
			 */
			stmt->intoClause = NULL;

			parseTree = (Node *) ctas;
		}
	}

	return transformStmt(pstate, parseTree);
}

/*
 * transformStmt -
 *	  递归地把解析树变换成查询树。           
 */
Query *
transformStmt(ParseState *pstate, Node *parseTree)
{
	Query	   *result;

	/*
	 * 我们对基础 DML 语句应用 RAW_EXPRESSION_COVERAGE_TEST         
	 * testing；不能对一切语句都运行，是因为函数             
	 * raw_expression_tree_walker() 不处理。  
	 */
#ifdef RAW_EXPRESSION_COVERAGE_TEST
	switch (nodeTag(parseTree))
	{
		case T_SelectStmt:
		case T_InsertStmt:
		case T_UpdateStmt:
		case T_DeleteStmt:
		case T_MergeStmt:
			(void) test_raw_expression_coverage(parseTree, NULL);
			break;
		default:
			break;
	}
#endif							/* RAW_EXPRESSION_COVERAGE_TEST */

	/*
	 * 注意：当修改此处具有非默认处理的语句类型           
	 * 集合时参见 stmt_requires_parse_analysis() 与          
	 * analyze_requires_snapshot() 
	 */
	switch (nodeTag(parseTree))
	{
			/*
			 * 可优化的语句    
			 */
		case T_InsertStmt:
			result = transformInsertStmt(pstate, (InsertStmt *) parseTree);
			break;

		case T_DeleteStmt:
			result = transformDeleteStmt(pstate, (DeleteStmt *) parseTree);
			break;

		case T_UpdateStmt:
			result = transformUpdateStmt(pstate, (UpdateStmt *) parseTree);
			break;

		case T_MergeStmt:
			result = transformMergeStmt(pstate, (MergeStmt *) parseTree);
			break;

		case T_SelectStmt:
			{
				SelectStmt *n = (SelectStmt *) parseTree;

				if (n->valuesLists)
					result = transformValuesClause(pstate, n);
				else if (n->op == SETOP_NONE)
					result = transformSelectStmt(pstate, n);
				else
					result = transformSetOperationStmt(pstate, n);
			}
			break;

		case T_ReturnStmt:
			result = transformReturnStmt(pstate, (ReturnStmt *) parseTree);
			break;

		case T_PLAssignStmt:
			result = transformPLAssignStmt(pstate,
										   (PLAssignStmt *) parseTree);
			break;

			/*
			 * 特殊情况 
			 */
		case T_DeclareCursorStmt:
			result = transformDeclareCursorStmt(pstate,
												(DeclareCursorStmt *) parseTree);
			break;

		case T_ExplainStmt:
			result = transformExplainStmt(pstate,
										  (ExplainStmt *) parseTree);
			break;

		case T_CreateTableAsStmt:
			result = transformCreateTableAsStmt(pstate,
												(CreateTableAsStmt *) parseTree);
			break;

		case T_CallStmt:
			result = transformCallStmt(pstate,
									   (CallStmt *) parseTree);
			break;

		default:

			/*
			 * 其他语句不需要任何变换；只需返回              
			 * 在顶部罩上一个 Query 节点的原始解析树。   
			 */
			result = makeNode(Query);
			result->commandType = CMD_UTILITY;
			result->utilityStmt = (Node *) parseTree;
			break;
	}

	/* 在得知实情之前，先标记为原始查询  */
	result->querySource = QSRC_ORIGINAL;
	result->canSetTag = true;

	return result;
}

/*
 * stmt_requires_parse_analysis
 *		如果解析分析对给定的原始解析树会做非平  
 *		凡处理，就返回真。   
 *
 * 一般而言，对任何一种语句类型，只要                
 * transformStmt() 对它的处理超出了用 CMD_UTILITY Query     
 * 包装，就应返回真。当它返回假时，调用者可以        
 * 认定不存在任何需要重做原始语句解析分析的情形。 
 *
 * 当前，由于重写器和规划器对 CMD_UTILITY 查询          
 * 不做任何处理，返回假就意味着解析分析/重写/           
 * 规划流水线永远无需重做。如果这一点发生变化，    
 * 调用者或需调整。    
 */
bool
stmt_requires_parse_analysis(RawStmt *parseTree)
{
	bool		result;

	switch (nodeTag(parseTree->stmt))
	{
			/*
			 * 可优化的语句    
			 */
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
		case T_MergeStmt:
		case T_SelectStmt:
		case T_ReturnStmt:
		case T_PLAssignStmt:
			result = true;
			break;

			/*
			 * 特殊情况 
			 */
		case T_DeclareCursorStmt:
		case T_ExplainStmt:
		case T_CreateTableAsStmt:
		case T_CallStmt:
			result = true;
			break;

		default:
			/* 其余语句只是被包进一个 CMD_UTILITY Query 中。   */
			result = false;
			break;
	}

	return result;
}

/*
 * analyze_requires_snapshot
 *		如果在解析分析之前必须为给定的原始解析树      
 *		设置快照则返回真。 
 */
bool
analyze_requires_snapshot(RawStmt *parseTree)
{
	/*
	 * 当前，它应当在恰好与 stmt_requires_parse_analysis()    
	 * 相同的情形下返回真，因此我们直接调用该函数，  
	 * 而不重复实现一份。为了调用者的清晰性，我们       
	 * 把这两个入口点分开保留，因为从调用者的角度 
	 * 条件不相同。   
	 *
	 * 虽然将来也许会有某种语句类型，其 transformStmt()     
	 * 会做实质处理，但该处理并不需要快照。不过    
	 * 做出这样的选择似乎会很难维护。如果你想安装        
	 * 例外，请在注释中说明这样做的理由。                    
	 *         
	 */
	return stmt_requires_parse_analysis(parseTree);
}

/*
 * transformDeleteStmt -
 *	  变换 DELETE 语句。      
 */
static Query *
transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ParseNamespaceItem *nsitem;
	Node	   *qual;

	qry->commandType = CMD_DELETE;

	/* 独立于其他一切来处理 WITH 子句        */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/* 只用结果关系建立范围表           */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 stmt->relation->inh,
										 true,
										 ACL_DELETE);
	nsitem = pstate->p_target_nsitem;

	/* DELETE 中没有 DISTINCT     */
	qry->distinctClause = NIL;

	/* USING 中的子查询无法访问结果关系         */
	nsitem->p_lateral_only = true;
	nsitem->p_lateral_ok = false;

	/*
	 * USING 子句并非标准 SQL 语法，功能上等价于         
	 * 可为 UPDATE 指定的 FROM 列表。使用 USING 关键字        
	 * 而非 FROM，因为 FROM 已经是 DELETE                      
	 * 语法中的关键字。     
	 */
	transformFromClause(pstate, stmt->usingClause);

	/* 其余子句可以正常引用结果关系                   */
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	qry->returningList = transformReturningList(pstate, stmt->returningList);

	/* 范围表和连接树的构建已完成    */
	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	assign_query_collations(pstate, qry);

	/* 为可靠比较表达式，此步必须在排序规则之后完成   */
	if (pstate->p_hasAggs)
		parseCheckAggregates(pstate, qry);

	return qry;
}

/*
 * transformInsertStmt -
 *	  变换 INSERT 语句。      
 */
static Query *
transformInsertStmt(ParseState *pstate, InsertStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *selectStmt = (SelectStmt *) stmt->selectStmt;
	List	   *exprList = NIL;
	bool		isGeneralSelect;
	List	   *sub_rtable;
	List	   *sub_rteperminfos;
	List	   *sub_namespace;
	List	   *icolumns;
	List	   *attrnos;
	ParseNamespaceItem *nsitem;
	RTEPermissionInfo *perminfo;
	ListCell   *icols;
	ListCell   *attnos;
	ListCell   *lc;
	bool		isOnConflictUpdate;
	AclMode		targetPerms;

	/* 不会有任何需要操心的外层 WITH    */
	Assert(pstate->p_ctenamespace == NIL);

	qry->commandType = CMD_INSERT;
	pstate->p_is_insert = true;

	/* 独立于其他一切来处理 WITH 子句        */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	qry->override = stmt->override;

	isOnConflictUpdate = (stmt->onConflictClause &&
						  stmt->onConflictClause->action == ONCONFLICT_UPDATE);

	/*
	 * 我们要处理三种情形：DEFAULT VALUES（selectStmt             
	 *  == NULL）、VALUES 列表或一般 SELECT 输入。我们            
	 * VALUES 做特判，为效率，也为处理 DEFAULT。 
	 *
	 * 语法允许把 ORDER BY、LIMIT、FOR UPDATE 或 WITH 附            
	 * 加到 VALUES 子句上。若存在其中一种，就把它当        
	 * 作一般 SELECT；可行，但 DEFAULT 项不能与之同用。     
	 */
	isGeneralSelect = (selectStmt && (selectStmt->valuesLists == NIL ||
									  selectStmt->sortClause != NIL ||
									  selectStmt->limitOffset != NULL ||
									  selectStmt->limitCount != NULL ||
									  selectStmt->lockingClause != NIL ||
									  selectStmt->withClause != NULL));

	/*
	 * 若传入了非空的 rangetable/namespace，且我们正在     
	 * 做 INSERT/SELECT，就要安排把 rangetable/rteperminfos/        
	 * namespace 传递给 SELECT。这只可能发生在 CREATE          
	 * RULE 之内，此时我们希望规则的 OLD 和 NEW rtable           
	 * 条目作为 SELECT 的 rtable 部分出现，而不是作            
	 * 为它的外层引用。（权宜之计！）SELECT 的 joinlist   
	 * 则不受影响。且须先于目标表加入 INSERT rtable
	 */
	if (isGeneralSelect)
	{
		sub_rtable = pstate->p_rtable;
		pstate->p_rtable = NIL;
		sub_rteperminfos = pstate->p_rteperminfos;
		pstate->p_rteperminfos = NIL;
		sub_namespace = pstate->p_namespace;
		pstate->p_namespace = NIL;
	}
	else
	{
		sub_rtable = NIL;		/* 未使用，避免编译器告警 */
		sub_rteperminfos = NIL;
		sub_namespace = NIL;
	}

	/*
	 * 扫描 SELECT 之前必须先获取 INSERT 目标表的写锁，       
	 * 否则若目标表也出现在 SELECT 部分中，我们最初拿      
	 * 到的初始锁类型就是错的。注意目标表不会被加       
	 * 入 joinlist/namespace。    
	 */
	targetPerms = ACL_INSERT;
	if (isOnConflictUpdate)
		targetPerms |= ACL_UPDATE;
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 false, false, targetPerms);

	/* 校验 stmt->cols 列表，若未给列表则构建默认列表  */
	icolumns = checkInsertTargets(pstate, stmt->cols, &attrnos);
	Assert(list_length(icolumns) == list_length(attrnos));

	/*
	 * 判定当前是哪种 INSERT 变体。    
	 */
	if (selectStmt == NULL)
	{
		/*
		 * 我们遇到的是 INSERT ... DEFAULT VALUES。这种情形   
		 * 可以这样处理：发出空目标列表，这样当规划器    
		 * 展开时各列将取默认值。  
		 */
		exprList = NIL;
	}
	else if (isGeneralSelect)
	{
		/*
		 * 我们把子 pstate 设为外层 pstate 的子节点，这样它 
		 * 就能看到来自上层提供的任何 Param 定义。        
		 * 由于外层 pstate 的 rtable 与命名空间当前为空，  
		 * 不会有暴露子 SELECT 不应见名称的副作用。          
		 *     
		 */
		ParseState *sub_pstate = make_parsestate(pstate);
		Query	   *selectQuery;

		/*
		 * 处理源 SELECT。       
		 *
		 * 关键是这里的处理必须与独立 SELECT 完全一样；     
		 * 否则 INSERT 内 SELECT 的行为就可能与独立 SELECT      
		 * 不同。（事实上，6.5 及之前的 Postgres 有此类    
		 * 缺陷。）                
		 *
		 * 唯一的例外是：我们阻止把未知类型输出解   
		 * 析为 TEXT。语义不受影响，因为若列的类型         
		 * 在语义上有影响，那么它无论如何都会被解析为 
		 * 其他类型。这样做让我们能把此类输出解析成      
		 * 目标列的类型，下面会处理。         
		 */
		sub_pstate->p_rtable = sub_rtable;
		sub_pstate->p_rteperminfos = sub_rteperminfos;
		sub_pstate->p_joinexprs = NIL;	/* sub_rtable 无连接    */
		sub_pstate->p_nullingrels = NIL;
		sub_pstate->p_namespace = sub_namespace;
		sub_pstate->p_resolve_unknowns = false;

		selectQuery = transformStmt(sub_pstate, stmt->selectStmt);

		free_parsestate(sub_pstate);

		/* 语法应该已经生成了 SELECT        */
		if (!IsA(selectQuery, Query) ||
			selectQuery->commandType != CMD_SELECT)
			elog(ERROR, "unexpected non-SELECT command in INSERT ... SELECT");

		/*
		 * 让源成为 INSERT 的 rangetable 中的子查询，并把它   
		 * 加入 INSERT joinlist（命名空间除外）。   
		 */
		nsitem = addRangeTableEntryForSubquery(pstate,
											   selectQuery,
											   makeAlias("*SELECT*", NIL),
											   false,
											   false);
		addNSItemToQuery(pstate, nsitem, true, false, false);

		/*----------
		 * 为 INSERT 生成表达式列表，从中选出子查询的    
		 * 全部非 resjunk 列。（INSERT 的 tlist 必须与子查询 
		 * 的 tlist 相互独立，因为要添加列、插入类型    
		 * 强制转换等操作。）     
		 *
		 * HACK：SELECT 目标列表里的未知类型常量和参数       
		 * 会被原样复制，而非作为子查询输出引用。   
		 * 这样做是为了确保当我们把它们强转到目标列  
		 * 数据类型时行为正确（见 coerce_type 中的      
		 * 特例）。否则，如下语句会失败：         
		 *		INSERT INTO foo SELECT 'bar', ... FROM baz
		 *----------
		 */
		exprList = NIL;
		foreach(lc, selectQuery->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Expr	   *expr;

			if (tle->resjunk)
				continue;
			if (tle->expr &&
				(IsA(tle->expr, Const) || IsA(tle->expr, Param)) &&
				exprType((Node *) tle->expr) == UNKNOWNOID)
				expr = tle->expr;
			else
			{
				Var		   *var = makeVarFromTargetEntry(nsitem->p_rtindex, tle);

				var->location = exprLocation((Node *) tle->expr);
				expr = (Expr *) var;
			}
			exprList = lappend(exprList, expr);
		}

		/* 准备用于赋给目标表的行          */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos,
									  false);
	}
	else if (list_length(selectStmt->valuesLists) > 1)
	{
		/*
		 * 处理带多个 VALUES 子列表的 INSERT ... VALUES。   
		 * 会生成 VALUES RTE 来容纳变换后的表达式列表，并    
		 * 构建目标列表，其中的 Var 引用 VALUES RTE。        
		 *     
		 */
		List	   *exprsLists = NIL;
		List	   *coltypes = NIL;
		List	   *coltypmods = NIL;
		List	   *colcollations = NIL;
		int			sublist_length = -1;
		bool		lateral = false;

		Assert(selectStmt->intoClause == NULL);

		foreach(lc, selectStmt->valuesLists)
		{
			List	   *sublist = (List *) lfirst(lc);

			/*
			 * 做基本的表达式变换（与 ROW() 表达式相同，   
			 * 顶层允许 SetToDefault）    
			 */
			sublist = transformExpressionList(pstate, sublist,
											  EXPR_KIND_VALUES, true);

			/*
			 * 所有子列表必须在完成变换             
			 * *之后*才相同（变换可能把 '*' 展开成多项）  
			 * VALUES RTE 不能处理任何差异。         
			 */
			if (sublist_length < 0)
			{
				/* 记住第一个子列表变换后的长度           */
				sublist_length = list_length(sublist);
			}
			else if (sublist_length != list_length(sublist))
			{
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("VALUES lists must all be the same length"),
						 parser_errposition(pstate,
											exprLocation((Node *) sublist))));
			}

			/*
			 * 准备用于赋给目标表的行。我们会正常处理  
			 * 目标列规格上的任何间接引用，但随后会剥离  
			 * 由此得到的字段/数组赋值节点，因为我们不想 
			 * 让解析后的语句在每个 VALUES 行里都带有这些 
			 * 节点的副本。（反复变换间接引用规    
			 * 格确实麻烦，但要避免它就得对                
			 * transformAssignmentIndirection 进行      
			 * 真正烦琐的重构。）     
			 */
			sublist = transformInsertRow(pstate, sublist,
										 stmt->cols,
										 icolumns, attrnos,
										 true);

			/*
			 * 必须现在指定排序规则：assign_query_collations     
			 * 不处理范围表条目。我们让每行都独立地     
			 * 指定排序规则，不必担心各行在竖直方向上  
			 * 是否一致。外层 INSERT 查询不在乎 VALUES 列的     
			 * 排序规则，因此不值得在这里为每一列找出      
			 * 一个公共排序规则。（不过要注意，这确实有   
			 * 一个用户可见的后果：INSERT ... VALUES 不会就
			 * 某列中冲突的显式 COLLATE 报错，而同样的         
			 * VALUES 构造放到其他上下文则会报错。）  
			 *                                              
			 */
			assign_list_collations(pstate, sublist);

			exprsLists = lappend(exprsLists, sublist);
		}

		/*
		 * 为 VALUES RTE 构建列类型/typmod/排序规则列表。     
		 * 每一列中的每个表达式都已被强转为对应目标       
		 * 列或子字段的类型/typmod，所以只需查看第一行     
		 * 的 exprType/exprTypmod 就足够了。排序规则标签          
		 * 无须关心，直接为其填入 InvalidOid 即可。            
		 */
		foreach(lc, (List *) linitial(exprsLists))
		{
			Node	   *val = (Node *) lfirst(lc);

			coltypes = lappend_oid(coltypes, exprType(val));
			coltypmods = lappend_int(coltypmods, exprTypmod(val));
			colcollations = lappend_oid(colcollations, InvalidOid);
		}

		/*
		 * 通常情况下，表达式列表中不可能有当前层         
		 * 的 Var，因为命名空间为空……但若我们处于    
		 * CREATE RULE 内，则可能出现 NEW/OLD 引用。那时          
		 * 须将 VALUES RTE 标为 LATERAL。    
		 */
		if (list_length(pstate->p_rtable) != 1 &&
			contain_vars_of_level((Node *) exprsLists, 0))
			lateral = true;

		/*
		 * 生成 VALUES RTE      
		 */
		nsitem = addRangeTableEntryForValues(pstate, exprsLists,
											 coltypes, coltypmods, colcollations,
											 NULL, lateral, true);
		addNSItemToQuery(pstate, nsitem, true, false, false);

		/*
		 * 生成引用该 RTE 的 Var 列表       
		 */
		exprList = expandNSItemVars(pstate, nsitem, 0, -1, NULL);

		/*
		 * 把目标列规格上的间接引用重新应用到 Var        
		 */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos,
									  false);
	}
	else
	{
		/*
		 * 处理带单个 VALUES 子列表的 INSERT ... VALUES。这种   
		 * 情形单独处理是为了效率。子列表直接作为         
		 * Query 的目标列表直接算出，不设 VALUES             
		 *  RTE。效果同不带 FROM 的 SELECT。 
		 */
		List	   *valuesLists = selectStmt->valuesLists;

		Assert(list_length(valuesLists) == 1);
		Assert(selectStmt->intoClause == NULL);

		/*
		 * 做基本的表达式变换（与 ROW() 表达式相同，顶层   
		 * 可用 SetToDefault）    
		 */
		exprList = transformExpressionList(pstate,
										   (List *) linitial(valuesLists),
										   EXPR_KIND_VALUES_SINGLE,
										   true);

		/* 准备用于赋给目标表的行          */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos,
									  false);
	}

	/*
	 * 用算出的表达式列表生成查询的目标列表。           
	 * 同时把所有目标列标记为需要插入权限。          
	 */
	perminfo = pstate->p_target_nsitem->p_perminfo;
	qry->targetList = NIL;
	Assert(list_length(exprList) <= list_length(icolumns));
	forthree(lc, exprList, icols, icolumns, attnos, attrnos)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		ResTarget  *col = lfirst_node(ResTarget, icols);
		AttrNumber	attr_num = (AttrNumber) lfirst_int(attnos);
		TargetEntry *tle;

		tle = makeTargetEntry(expr,
							  attr_num,
							  col->name,
							  false);
		qry->targetList = lappend(qry->targetList, tle);

		perminfo->insertedCols = bms_add_member(perminfo->insertedCols,
												attr_num - FirstLowInvalidHeapAttributeNumber);
	}

	/*
	 * 如果还有待处理的子句，就把查询命名空间        
	 * 设为只含目标关系，并移除在子 SELECT 或            
	 * VALUES 加的条目。    
	 */
	if (stmt->onConflictClause || stmt->returningList)
	{
		pstate->p_namespace = NIL;
		addNSItemToQuery(pstate, pstate->p_target_nsitem,
						 false, true, true);
	}

	/* 有 ON CONFLICT 就处理。 */
	if (stmt->onConflictClause)
		qry->onConflict = transformOnConflictClause(pstate,
													stmt->onConflictClause);

	/* 有 RETURNING 就处理。 */
	if (stmt->returningList)
		qry->returningList = transformReturningList(pstate,
													stmt->returningList);

	/* 范围表和连接树的构建已完成    */
	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * 准备一条要赋给目标表的 INSERT 行。          
 *
 * exprlist：源值的变换后表达式；它们可能来自               
 * VALUES 行，或是引用子 SELECT 或 VALUES RTE 输出              
 * 的 Var。stmtcols：INSERT 原始目标列规格（只测试 NIL）    
 * icolumns：有效目标列规格（ResTarget 列表）     
 * attrnos：整型列号（长度必须与 icolumns 相同）      
 * strip_indirection：为真时移除任何字段/数组赋值节点 
 */
List *
transformInsertRow(ParseState *pstate, List *exprlist,
				   List *stmtcols, List *icolumns, List *attrnos,
				   bool strip_indirection)
{
	List	   *result;
	ListCell   *lc;
	ListCell   *icols;
	ListCell   *attnos;

	/*
	 * 检查表达式列表的长度。它的表达式不能多于      
	 * 目标列的数目。可以更少，但前提是未给出显式   
	 * 列清单（其余列隐式取默认值）。注意要      
	 * 必须在变换*之后*检查这一点，因为变换可能        
	 * 把 '*' 展开成多个条目。          
	 */
	if (list_length(exprlist) > list_length(icolumns))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INSERT has more expressions than target columns"),
				 parser_errposition(pstate,
									exprLocation(list_nth(exprlist,
														  list_length(icolumns))))));
	if (stmtcols != NIL &&
		list_length(exprlist) < list_length(icolumns))
	{
		/*
		 * 对于 INSERT ... SELECT (a,b,c) FROM ... 这类情形，        
		 * 我们也会到达此处，即用户误建了一个              
		 * RowExpr 而非分离的列。若看起来问题在此，      
		 * 就追加一条合适的提示，因为对这种情形来说     
		 * 主错误消息对此颇有误导。（没有 stmtcols 时   
		 * 会得到数据类型不匹配的报错，误导较小，故不    
		 * 给提示。）    
		 */
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INSERT has more target columns than expressions"),
				 ((list_length(exprlist) == 1 &&
				   count_rowexpr_columns(pstate, linitial(exprlist)) ==
				   list_length(icolumns)) ?
				  errhint("The insertion source is a row expression containing the same number of columns expected by the INSERT. Did you accidentally use extra parentheses?") : 0),
				 parser_errposition(pstate,
									exprLocation(list_nth(icolumns,
														  list_length(exprlist))))));
	}

	/*
	 * 准备要赋给目标表的各列。           
	 */
	result = NIL;
	forthree(lc, exprlist, icols, icolumns, attnos, attrnos)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		ResTarget  *col = lfirst_node(ResTarget, icols);
		int			attno = lfirst_int(attnos);

		expr = transformAssignedExpr(pstate, expr,
									 EXPR_KIND_INSERT_TARGET,
									 col->name,
									 attno,
									 col->indirection,
									 col->location);

		if (strip_indirection)
		{
			/*
			 * 须移除顶层的 FieldStore 和 SubscriptingRef，         
			 * 以及位于其上方的任何 CoerceToDomain —— 但不   
			 * 包括不位于其上方的 CoerceToDomain。          
			 */
			while (expr)
			{
				Expr	   *subexpr = expr;

				while (IsA(subexpr, CoerceToDomain))
				{
					subexpr = ((CoerceToDomain *) subexpr)->arg;
				}
				if (IsA(subexpr, FieldStore))
				{
					FieldStore *fstore = (FieldStore *) subexpr;

					expr = (Expr *) linitial(fstore->newvals);
				}
				else if (IsA(subexpr, SubscriptingRef))
				{
					SubscriptingRef *sbsref = (SubscriptingRef *) subexpr;

					if (sbsref->refassgnexpr == NULL)
						break;

					expr = sbsref->refassgnexpr;
				}
				else
					break;
			}
		}

		result = lappend(result, expr);
	}

	return result;
}

/*
 * transformOnConflictClause -
 *	  变换 INSERT 中的 OnConflictClause。   
 */
static OnConflictExpr *
transformOnConflictClause(ParseState *pstate,
						  OnConflictClause *onConflictClause)
{
	ParseNamespaceItem *exclNSItem = NULL;
	List	   *arbiterElems;
	Node	   *arbiterWhere;
	Oid			arbiterConstraint;
	List	   *onConflictSet = NIL;
	Node	   *onConflictWhere = NULL;
	int			exclRelIndex = 0;
	List	   *exclRelTlist = NIL;
	OnConflictExpr *result;

	/*
	 * 如果这是 ON CONFLICT ... UPDATE，先创建 EXCLUDED             
	 * 伪关系的范围表条目，以便处理仲裁器表达式        
	 * 时它已经在场。（你无法真正从那里引用它，          
	 * 但若尝试，这会给出有用的错误消息。）      
	 */
	if (onConflictClause->action == ONCONFLICT_UPDATE)
	{
		Relation	targetrel = pstate->p_target_relation;
		RangeTblEntry *exclRte;

		exclNSItem = addRangeTableEntryForRelation(pstate,
												   targetrel,
												   RowExclusiveLock,
												   makeAlias("excluded", NIL),
												   false, false);
		exclRte = exclNSItem->p_rte;
		exclRelIndex = exclNSItem->p_rtindex;

		/*
		 * 将 relkind 设为 composite，表示我们处理的并非       
		 * 是一个真实的关系，并且无需对它做权限检查。 
		 * （我们改为检查真正的目标关系。）  
		 */
		exclRte->relkind = RELKIND_COMPOSITE_TYPE;

		/* 为 EXPLAIN 创建 EXCLUDED 关系的目标列表   */
		exclRelTlist = BuildOnConflictExcludedTargetlist(targetrel,
														 exclRelIndex);
	}

	/* 处理仲裁器子句 ON CONFLICT ON (...)       */
	transformOnConflictArbiter(pstate, onConflictClause, &arbiterElems,
							   &arbiterWhere, &arbiterConstraint);

	/* 处理 DO UPDATE  */
	if (onConflictClause->action == ONCONFLICT_UPDATE)
	{
		/*
		 * UPDATE 目标列表里的表达式须按 UPDATE 而非             
		 * INSERT 来处理。我们无需保存/恢复它，因为所有    
		 * INSERT 表达式已解析完毕。   
		 */
		pstate->p_is_insert = false;

		/*
		 * 把 EXCLUDED 伪关系加入查询命名空间，使其在        
		 * UPDATE 子表达式中可用。        
		 */
		addNSItemToQuery(pstate, exclNSItem, false, true, true);

		/*
		 * 现在变换 UPDATE 子表达式。     
		 */
		onConflictSet =
			transformUpdateTargetList(pstate, onConflictClause->targetList);

		onConflictWhere = transformWhereClause(pstate,
											   onConflictClause->whereClause,
											   EXPR_KIND_WHERE, "WHERE");

		/*
		 * 从查询命名空间中移除 EXCLUDED 伪关系，因为它      
		 * 不应在 RETURNING 中可用。（也许某天我们可以       
		 * 允许它，并删去此步骤。）  
		 */
		Assert((ParseNamespaceItem *) llast(pstate->p_namespace) == exclNSItem);
		pstate->p_namespace = list_delete_last(pstate->p_namespace);
	}

	/* 最后构建 ON CONFLICT DO [NOTHING | UPDATE] 表达式    */
	result = makeNode(OnConflictExpr);

	result->action = onConflictClause->action;
	result->arbiterElems = arbiterElems;
	result->arbiterWhere = arbiterWhere;
	result->constraint = arbiterConstraint;
	result->onConflictSet = onConflictSet;
	result->onConflictWhere = onConflictWhere;
	result->exclRelIndex = exclRelIndex;
	result->exclRelTlist = exclRelTlist;

	return result;
}


/*
 * BuildOnConflictExcludedTargetlist
 *		为 ON CONFLICT 的 EXCLUDED 伪关系构建目标列表，        
 *		以 varno exclRelIndex 表示 targetrel 的各列。          
 *
 * 注：导出供重写器使用。      
 */
List *
BuildOnConflictExcludedTargetlist(Relation targetrel,
								  Index exclRelIndex)
{
	List	   *result = NIL;
	int			attno;
	Var		   *var;
	TargetEntry *te;

	/*
	 * tlist 的 resno 必须与底层关系的 attno 相对应，   
	 * 因此被删除的列也需要有对应的条目。                
	 */
	for (attno = 0; attno < RelationGetNumberOfAttributes(targetrel); attno++)
	{
		Form_pg_attribute attr = TupleDescAttr(targetrel->rd_att, attno);
		char	   *name;

		if (attr->attisdropped)
		{
			/*
			 * 这里无法使用 atttypid，不过 Const 声称的类型     
			 * 其实无关紧要。  
			 */
			var = (Var *) makeNullConst(INT4OID, -1, InvalidOid);
			name = NULL;
		}
		else
		{
			var = makeVar(exclRelIndex, attno + 1,
						  attr->atttypid, attr->atttypmod,
						  attr->attcollation,
						  0);
			name = pstrdup(NameStr(attr->attname));
		}

		te = makeTargetEntry((Expr *) var,
							 attno + 1,
							 name,
							 false);

		result = lappend(result, te);
	}

	/*
	 * 添加 whole-row Var 条目以支持对 "EXCLUDED.*" 的引用。     
	 * 如同 EXCLUDED tlist 中其他条目，其 resno 必须与该 Var 的 
	 * varattno 一致，否则 setrefs.c 里解析引用时会出错。   
	 * 这违背目标列表的常规约定，但我们不会把它用作 
	 * tlist，所以没有问题。                     
	 */
	var = makeVar(exclRelIndex, InvalidAttrNumber,
				  targetrel->rd_rel->reltype,
				  -1, InvalidOid, 0);
	te = makeTargetEntry((Expr *) var, InvalidAttrNumber, NULL, true);
	result = lappend(result, te);

	return result;
}


/*
 * count_rowexpr_columns -
 *	  取得 ROW() 表达式所包含的列数；           
 *	  若表达式不是 RowExpr 或引用它的 Var 则返回 -1。   
 *
 * 目前这只用于提示目的，无需苛求识别所有情形。  
 * Var 情形很有意思，                                               
 * 因为 INSERT ... SELECT (...) 场景中得到的正是它。     
 */
static int
count_rowexpr_columns(ParseState *pstate, Node *expr)
{
	if (expr == NULL)
		return -1;
	if (IsA(expr, RowExpr))
		return list_length(((RowExpr *) expr)->args);
	if (IsA(expr, Var))
	{
		Var		   *var = (Var *) expr;
		AttrNumber	attnum = var->varattno;

		if (attnum > 0 && var->vartype == RECORDOID)
		{
			RangeTblEntry *rte;

			rte = GetRTEByRangeTablePosn(pstate, var->varno, var->varlevelsup);
			if (rte->rtekind == RTE_SUBQUERY)
			{
				/* FROM 子查询：检查子查询的输出表达式  */
				TargetEntry *ste = get_tle_by_resno(rte->subquery->targetList,
													attnum);

				if (ste == NULL || ste->resjunk)
					return -1;
				expr = (Node *) ste->expr;
				if (IsA(expr, RowExpr))
					return list_length(((RowExpr *) expr)->args);
			}
		}
	}
	return -1;
}


/*
 * transformSelectStmt -
 *	  转换 Select 语句         
 *
 * 注：只涵盖无集合操作且无 VALUES 列表的情形；          
 * 其他情形见下文。      
 */
static Query *
transformSelectStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	Node	   *qual;
	ListCell   *l;

	qry->commandType = CMD_SELECT;

	/* 独立于其他部分处理 WITH 子句           */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/* 如果在不允许 INTO 的地方被调用则报错                */
	if (stmt->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("SELECT ... INTO is not allowed here"),
				 parser_errposition(pstate,
									exprLocation((Node *) stmt->intoClause))));

	/* 把 FOR UPDATE/FOR SHARE 信息提供给 addRangeTableEntry    */
	pstate->p_locking_clause = stmt->lockingClause;

	/* 也让窗口函数能获取 WINDOW 信息            */
	pstate->p_windowdefs = stmt->windowClause;

	/* 处理 FROM 子句      */
	transformFromClause(pstate, stmt->fromClause);

	/* 转换目标列表   */
	qry->targetList = transformTargetList(pstate, stmt->targetList,
										  EXPR_KIND_SELECT_TARGET);

	/* 标记列的来源  */
	markTargetListOrigins(pstate, qry->targetList);

	/* 转换 WHERE    */
	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	/* HAVING 子句的初步处理与 WHERE 子句十分相似      */
	qry->havingQual = transformWhereClause(pstate, stmt->havingClause,
										   EXPR_KIND_HAVING, "HAVING");

	/*
	 * 转换排序/分组相关内容。先处理 ORDER BY，因为    
	 * transformGroupClause 和 transformDistinctClause 都需要其结果。 
	 * 注意这些函数还可能修改 targetList，故按引用传入。  
	 *                   
	 */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* 按 SQL92 规则  */ );

	qry->groupClause = transformGroupClause(pstate,
											stmt->groupClause,
											&qry->groupingSets,
											&qry->targetList,
											qry->sortClause,
											EXPR_KIND_GROUP_BY,
											false /* 按 SQL92 规则  */ );
	qry->groupDistinct = stmt->groupDistinct;

	if (stmt->distinctClause == NIL)
	{
		qry->distinctClause = NIL;
		qry->hasDistinctOn = false;
	}
	else if (linitial(stmt->distinctClause) == NULL)
	{
		/* 原为 SELECT DISTINCT */
		qry->distinctClause = transformDistinctClause(pstate,
													  &qry->targetList,
													  qry->sortClause,
													  false);
		qry->hasDistinctOn = false;
	}
	else
	{
		/* 原为 SELECT DISTINCT ON */
		qry->distinctClause = transformDistinctOnClause(pstate,
														stmt->distinctClause,
														&qry->targetList,
														qry->sortClause);
		qry->hasDistinctOn = true;
	}

	/* 转换 LIMIT    */
	qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											stmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   stmt->limitOption);
	qry->limitOption = stmt->limitOption;

	/* 在看完所有窗口函数之后再转换窗口子句           */
	qry->windowClause = transformWindowDefinitions(pstate,
												   pstate->p_windowdefs,
												   &qry->targetList);

	/* 把仍未确定类型的输出列定为 text 类型            */
	if (pstate->p_resolve_unknowns)
		resolveTargetListUnknowns(pstate, qry->targetList);

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	foreach(l, stmt->lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	assign_query_collations(pstate, qry);

	/* 必须在排序规则之后做，才能可靠地比较表达式      */
	if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
		parseCheckAggregates(pstate, qry);

	return qry;
}

/*
 * transformValuesClause -
 *	  转换被当作独立 SELECT 使用的 VALUES 子句               
 *
 * 我们构建一个包含 VALUES RTE 的 Query，就像写的是       
 *			SELECT * FROM (VALUES ...) AS "*VALUES*"
 */
static Query *
transformValuesClause(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	List	   *exprsLists = NIL;
	List	   *coltypes = NIL;
	List	   *coltypmods = NIL;
	List	   *colcollations = NIL;
	List	  **colexprs = NULL;
	int			sublist_length = -1;
	bool		lateral = false;
	ParseNamespaceItem *nsitem;
	ListCell   *lc;
	ListCell   *lc2;
	int			i;

	qry->commandType = CMD_SELECT;

	/* 多数 SELECT 处理都不适用于 VALUES 子句  */
	Assert(stmt->distinctClause == NIL);
	Assert(stmt->intoClause == NULL);
	Assert(stmt->targetList == NIL);
	Assert(stmt->fromClause == NIL);
	Assert(stmt->whereClause == NULL);
	Assert(stmt->groupClause == NIL);
	Assert(stmt->havingClause == NULL);
	Assert(stmt->windowClause == NIL);
	Assert(stmt->op == SETOP_NONE);

	/* 独立于其他部分处理 WITH 子句           */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * 对 VALUES 的每一行，转换其原始表达式。 
	 *
	 * 注意我们构建的中间表示是按列组织的，                
	 * 而非按行组织。这简化了后面类型与排序规则处理。
	 *       
	 */
	foreach(lc, stmt->valuesLists)
	{
		List	   *sublist = (List *) lfirst(lc);

		/*
		 * 做基本的表达式转换（与 ROW() 相同，但这里        
		 * 禁用 SetToDefault）。
		 */
		sublist = transformExpressionList(pstate, sublist,
										  EXPR_KIND_VALUES, false);

		/*
		 * 所有子列表都必须在转换*之后*具有相同长度     
		 * （转换可能把 '*' 展开为多个条目）。VALUES RTE 无法
		 * 处理任何差异。     
		 */
		if (sublist_length < 0)
		{
			/* 记住第一个子列表转换后的长度           */
			sublist_length = list_length(sublist);
			/* 并为按列列表分配数组          */
			colexprs = (List **) palloc0(sublist_length * sizeof(List *));
		}
		else if (sublist_length != list_length(sublist))
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("VALUES lists must all be the same length"),
					 parser_errposition(pstate,
										exprLocation((Node *) sublist))));
		}

		/* 构建按列的表达式列表    */
		i = 0;
		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);

			colexprs[i] = lappend(colexprs[i], col);
			i++;
		}

		/* 释放子列表的单元以节省内存 */
		list_free(sublist);

		/* 为该行准备一个 exprsLists 元素    */
		exprsLists = lappend(exprsLists, NIL);
	}

	/*
	 * 现在解析各列的共同类型，并把全部表达式强转到   
	 * 这些类型。然后确定每列共同的 typmod 与排序规则     
	 * （若存在）。  
	 *
	 * 必须现在做排序规则处理，因为 (1) assign_query_collations 
	 * 不处理范围表项，且 (2) 我们需要为 VALUES RTE 标注      
	 * 列排序规则供外层查询使用。我们在这里          
	 * 不把隐式排序规则之间的冲突视为错误；相反，      
	 * 该列的排序规则只会显示为 InvalidOid，如果这导致无法
	 * 解析出排序规则，稍后就会报错。                   
	 *
	 * 注意我们会就地修改按列的表达式列表。  
	 */
	for (i = 0; i < sublist_length; i++)
	{
		Oid			coltype;
		int32		coltypmod;
		Oid			colcoll;

		coltype = select_common_type(pstate, colexprs[i], "VALUES", NULL);

		foreach(lc, colexprs[i])
		{
			Node	   *col = (Node *) lfirst(lc);

			col = coerce_to_common_type(pstate, col, coltype, "VALUES");
			lfirst(lc) = (void *) col;
		}

		coltypmod = select_common_typmod(pstate, colexprs[i], coltype);
		colcoll = select_common_collation(pstate, colexprs[i], true);

		coltypes = lappend_oid(coltypes, coltype);
		coltypmods = lappend_int(coltypmods, coltypmod);
		colcollations = lappend_oid(colcollations, colcoll);
	}

	/*
	 * 最后把强转后的表达式重排为按行组织的列表。     
	 */
	for (i = 0; i < sublist_length; i++)
	{
		forboth(lc, colexprs[i], lc2, exprsLists)
		{
			Node	   *col = (Node *) lfirst(lc);
			List	   *sublist = lfirst(lc2);

			sublist = lappend(sublist, col);
			lfirst(lc2) = sublist;
		}
		list_free(colexprs[i]);
	}

	/*
	 * 正常情况下表达式列表中不会有本层的 Var，因为  
	 * 命名空间为空 ... 但如果在 CREATE RULE 内部，            
	 * 那么可能出现 NEW/OLD 引用。那种情况下我们必须把  
	 * VALUES RTE 标为 LATERAL。   
	 */
	if (pstate->p_rtable != NIL &&
		contain_vars_of_level((Node *) exprsLists, 0))
		lateral = true;

	/*
	 * 生成 VALUES RTE      
	 */
	nsitem = addRangeTableEntryForValues(pstate, exprsLists,
										 coltypes, coltypmods, colcollations,
										 NULL, lateral, true);
	addNSItemToQuery(pstate, nsitem, true, true, true);

	/*
	 * 如同展开 "*" 一样生成目标列表    
	 */
	Assert(pstate->p_next_resno == 1);
	qry->targetList = expandNSItemAttrs(pstate, nsitem, 0, true, -1);

	/*
	 * 语法允许把 ORDER BY、LIMIT 和 FOR UPDATE 附加到 VALUES 
	 * 故需处理。 
	 */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* 按 SQL92 规则  */ );

	qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											stmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   stmt->limitOption);
	qry->limitOption = stmt->limitOption;

	if (stmt->lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s cannot be applied to VALUES",
						LCS_asString(((LockingClause *)
									  linitial(stmt->lockingClause))->strength))));

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * transformSetOperationStmt -
 *	  转换集合操作树           
 *
 * 集合操作树本质上就是一个 SELECT，只是带有            
 * UNION/INTERSECT/EXCEPT 结构。我们必须转换每个叶子          
 * SELECT，并构建一个顶级 Query，将叶子 SELECT 作为子查询    
 * 放进其范围表。集合操作树则被存入顶级 Query 的       
 * 字段。           
 */
static Query *
transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *leftmostSelect;
	int			leftmostRTI;
	Query	   *leftmostQuery;
	SetOperationStmt *sostmt;
	List	   *sortClause;
	Node	   *limitOffset;
	Node	   *limitCount;
	List	   *lockingClause;
	WithClause *withClause;
	Node	   *node;
	ListCell   *left_tlist,
			   *lct,
			   *lcm,
			   *lcc,
			   *l;
	List	   *targetvars,
			   *targetnames,
			   *sv_namespace;
	int			sv_rtable_length;
	ParseNamespaceItem *jnsitem;
	ParseNamespaceColumn *sortnscolumns;
	int			sortcolindex;
	int			tllen;

	qry->commandType = CMD_SELECT;

	/*
	 * 找出最左边的叶子 SelectStmt。目前只需用它来         
	 * 在有 INTO 子句时给出合适的错误消息，因为这暗示 
	 * 集合操作树所在的上下文不允许 INTO。（本来       
	 * transformSetOperationTree 也会报错，但针对非最左侧    
	 * 的 INTO 单独报错似乎值得，因此该错误由             
	 * transformSetOperationTree 产生。）                       
	 */
	leftmostSelect = stmt->larg;
	while (leftmostSelect && leftmostSelect->op != SETOP_NONE)
		leftmostSelect = leftmostSelect->larg;
	Assert(leftmostSelect && IsA(leftmostSelect, SelectStmt) &&
		   leftmostSelect->larg == NULL);
	if (leftmostSelect->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("SELECT ... INTO is not allowed here"),
				 parser_errposition(pstate,
									exprLocation((Node *) leftmostSelect->intoClause))));

	/*
	 * 我们需要在这里提取 ORDER BY 和其他顶级子句，不能 
	 * 让 transformSetOperationTree() 看到它们---否则它只会递归
	 * 回这里！    
	 */
	sortClause = stmt->sortClause;
	limitOffset = stmt->limitOffset;
	limitCount = stmt->limitCount;
	lockingClause = stmt->lockingClause;
	withClause = stmt->withClause;

	stmt->sortClause = NIL;
	stmt->limitOffset = NULL;
	stmt->limitCount = NULL;
	stmt->lockingClause = NIL;
	stmt->withClause = NULL;

	/* 目前我们不支持对集合操作使用 FOR UPDATE/SHARE。*/
	if (lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(((LockingClause *)
									  linitial(lockingClause))->strength))));

	/* 独立于其他部分处理 WITH 子句           */
	if (withClause)
	{
		qry->hasRecursive = withClause->recursive;
		qry->cteList = transformWithClause(pstate, withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * 递归转换该树的各个组成部分。       
	 */
	sostmt = castNode(SetOperationStmt,
					  transformSetOperationTree(pstate, stmt, true, NULL));
	Assert(sostmt);
	qry->setOperations = (Node *) sostmt;

	/*
	 * 重新找出最左 SELECT（现为范围表内子查询）  
	 */
	node = sostmt->larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) node)->rtindex;
	leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * 使用最左 select 的列名与最顶层集合操作的共同 
	 * 数据类型/排序规则为外层生成虚拟目标列表， 
	 * 还要为解析 ORDER BY 制作虚拟 Var 及其名称的列表。   
	 *                     
	 *
	 * 注：我们用 leftmostRTI 作为虚拟变量的 varno。只要
	 * 它们的 RT 下标是什么不要紧，只要对应一个真实的
	 * 范围表项即可；否则这棵树被规则重写搅乱时可能 
	 * 出现怪事。                                  
	 */
	qry->targetList = NIL;
	targetvars = NIL;
	targetnames = NIL;
	sortnscolumns = (ParseNamespaceColumn *)
		palloc0(list_length(sostmt->colTypes) * sizeof(ParseNamespaceColumn));
	sortcolindex = 0;

	forfour(lct, sostmt->colTypes,
			lcm, sostmt->colTypmods,
			lcc, sostmt->colCollations,
			left_tlist, leftmostQuery->targetList)
	{
		Oid			colType = lfirst_oid(lct);
		int32		colTypmod = lfirst_int(lcm);
		Oid			colCollation = lfirst_oid(lcc);
		TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
		char	   *colName;
		TargetEntry *tle;
		Var		   *var;

		Assert(!lefttle->resjunk);
		colName = pstrdup(lefttle->resname);
		var = makeVar(leftmostRTI,
					  lefttle->resno,
					  colType,
					  colTypmod,
					  colCollation,
					  0);
		var->location = exprLocation((Node *) lefttle->expr);
		tle = makeTargetEntry((Expr *) var,
							  (AttrNumber) pstate->p_next_resno++,
							  colName,
							  false);
		qry->targetList = lappend(qry->targetList, tle);
		targetvars = lappend(targetvars, var);
		targetnames = lappend(targetnames, makeString(colName));
		sortnscolumns[sortcolindex].p_varno = leftmostRTI;
		sortnscolumns[sortcolindex].p_varattno = lefttle->resno;
		sortnscolumns[sortcolindex].p_vartype = colType;
		sortnscolumns[sortcolindex].p_vartypmod = colTypmod;
		sortnscolumns[sortcolindex].p_varcollid = colCollation;
		sortnscolumns[sortcolindex].p_varnosyn = leftmostRTI;
		sortnscolumns[sortcolindex].p_varattnosyn = lefttle->resno;
		sortcolindex++;
	}

	/*
	 * 为支持用输出列表达式写排序子句，第一步我们先  
	 * 生成一个让输出列可见的命名空间条目。Join RTE     
	 * 节点很适合此事，因为我们能轻松控制匹配时所     
	 * 生成的 Var。                                   
	 *
	 * 注：对这类情形我们暂时做不了有用的事，但至少 
	 * "ORDER BY upper(foo)" 会给出正确的错误消息，而不是   
	 * "foo 未找到" 
	 */
	sv_rtable_length = list_length(pstate->p_rtable);

	jnsitem = addRangeTableEntryForJoin(pstate,
										targetnames,
										sortnscolumns,
										JOIN_INNER,
										0,
										targetvars,
										NIL,
										NIL,
										NULL,
										NULL,
										false);

	sv_namespace = pstate->p_namespace;
	pstate->p_namespace = NIL;

	/* 只把 jnsitem 加入列命名空间 */
	addNSItemToQuery(pstate, jnsitem, false, false, true);

	/*
	 * 暂不支持集合操作树输出上的 resjunk 排序子句     
	 * --- 只能使用 SQL92 规定的做法：按名称或编号     
	 * 选择输出列。强制手段是检查 transformSortClause 不会     
	 * 向 tlist 添加任何条目。                    
	 */
	tllen = list_length(qry->targetList);

	qry->sortClause = transformSortClause(pstate,
										  sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* 按 SQL92 规则  */ );

	/* 恢复命名空间，从 rtable 移除 join RTE*/
	pstate->p_namespace = sv_namespace;
	pstate->p_rtable = list_truncate(pstate->p_rtable, sv_rtable_length);

	if (tllen != list_length(qry->targetList))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("invalid UNION/INTERSECT/EXCEPT ORDER BY clause"),
				 errdetail("Only result column names can be used, not expressions or functions."),
				 errhint("Add the expression/function to every SELECT, or move the UNION into a FROM clause."),
				 parser_errposition(pstate,
									exprLocation(list_nth(qry->targetList, tllen)))));

	qry->limitOffset = transformLimitClause(pstate, limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											stmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   stmt->limitOption);
	qry->limitOption = stmt->limitOption;

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	foreach(l, lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	assign_query_collations(pstate, qry);

	/* 必须在排序规则之后做，才能可靠地比较表达式      */
	if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
		parseCheckAggregates(pstate, qry);

	return qry;
}

/*
 * 为 SetOperationStmt 的 groupClauses 建 SortGroupClause 节点 
 *
 * 如果 require_hash 为真，表示调用者需要哈希支持，    
 * 否则将失败。所以要格外用力地找哈希支持。    
 */
SortGroupClause *
makeSortGroupClauseForSetOp(Oid rescoltype, bool require_hash)
{
	SortGroupClause *grpcl = makeNode(SortGroupClause);
	Oid			sortop;
	Oid			eqop;
	bool		hashable;

	/* 确定 eqop 以及可选的 sortop     */
	get_sort_group_operators(rescoltype,
							 false, true, false,
							 &sortop, &eqop, NULL,
							 &hashable);

	/*
	 * 类型缓存不认为 record 可哈希（见               
	 * cache_record_field_properties()），但如果调用者确实需要  
	 * 哈希支持，我们可以假定它有。最坏情形是 record 的 
	 * 某组件不支持哈希，就会在执行时失败。  
	 */
	if (require_hash && (rescoltype == RECORDOID || rescoltype == RECORDARRAYOID))
		hashable = true;

	/* 此时还没有 tlist，因此无法分配 sortgroupref  */
	grpcl->tleSortGroupRef = 0;
	grpcl->eqop = eqop;
	grpcl->sortop = sortop;
	grpcl->nulls_first = false; /* 有无 sortop 都可以   */
	grpcl->hashable = hashable;

	return grpcl;
}

/*
 * transformSetOperationTree
 *		递归转换集合操作树的叶子节点与内部节点       
 *
 * 除了返回转换后的节点之外，如果 targetlist 不是 NULL，
 * 我们还会返回其非 resjunk TargetEntry 节点列表。对叶子  
 * 集合操作节点而言，这些是实际目标列表条目；否则  
 * 它们用来携带类型、typmod、排序规则和位置（供         
 * 错误消息用）集合操作节点各输出列的信息。这些信息 
 * 只在本函数的内部递归期间需要，所以外部调用者给    
 * targetlist 传 NULL。注：传叶子实际目标列表条目的   
 * 原因是让上层能够把 UNKNOWN Const 替换为正确强转     
 * 的常量。                                           
 */
static Node *
transformSetOperationTree(ParseState *pstate, SelectStmt *stmt,
						  bool isTopLevel, List **targetlist)
{
	bool		isLeaf;

	Assert(stmt && IsA(stmt, SelectStmt));

	/* 防范因集合表达式过于复杂而导致的栈溢出          */
	check_stack_depth();

	/*
	 * 对叶子和内部 SELECT 检查是否含不允许的操作。   
	 */
	if (stmt->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INTO is only allowed on first SELECT of UNION/INTERSECT/EXCEPT"),
				 parser_errposition(pstate,
									exprLocation((Node *) stmt->intoClause))));

	/* 目前我们不支持对集合操作使用 FOR UPDATE/SHARE。*/
	if (stmt->lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(((LockingClause *)
									  linitial(stmt->lockingClause))->strength))));

	/*
	 * 如果集合操作树的内部节点带有 ORDER BY、LIMIT、FOR    
	 * UPDATE 或 WITH 子句，就要把它当叶子节点处理，    
	 * 以生成独立的子 Query 树。否则它可由            
	 * SetOperationStmt 节点表示，位于父 Query 之下。          
	 */
	if (stmt->op == SETOP_NONE)
	{
		Assert(stmt->larg == NULL && stmt->rarg == NULL);
		isLeaf = true;
	}
	else
	{
		Assert(stmt->larg != NULL && stmt->rarg != NULL);
		if (stmt->sortClause || stmt->limitOffset || stmt->limitCount ||
			stmt->lockingClause || stmt->withClause)
			isLeaf = true;
		else
			isLeaf = false;
	}

	if (isLeaf)
	{
		/* 处理叶子 SELECT */
		Query	   *selectQuery;
		char		selectName[32];
		ParseNamespaceItem *nsitem;
		RangeTblRef *rtr;
		ListCell   *tl;

		/*
		 * 把 SelectStmt 转换为 Query。 
		 *
		 * 这一步与通常的 SELECT 转换一样，但我们会阻止把  
		 * 未知类型输出按 TEXT 解析，这不改变子查询语义，
		 * 因为若列类型有语义作用，它早就会被解析   
		 * 成别的。这样我们就能在下方用 select_common_type()    
		 * 解析这些输出。                                 
		 *                             
		 *
		 * 注：已转换子查询不影响本子查询解析，因其     
		 * 并不在顶层 pstate 的命名空间                          
		 * 列表中。   
		 */
		selectQuery = parse_sub_analyze((Node *) stmt, pstate,
										NULL, false, false);

		/*
		 * 检查当前层 Var 的非法引用（上层引用无妨），    
		 * 正常情况下这不会发生，因为命名空间为空；
		 * 但如果我们位于规则内部，这种情况就可能发生。
		 *               
		 */
		if (pstate->p_namespace)
		{
			if (contain_vars_of_level((Node *) selectQuery, 1))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("UNION/INTERSECT/EXCEPT member statement cannot refer to other relations of same query level"),
						 parser_errposition(pstate,
											locate_var_of_level((Node *) selectQuery, 1))));
		}

		/*
		 * 提取非垃圾 TLE 的列表供上层处理使用。          
		 */
		if (targetlist)
		{
			*targetlist = NIL;
			foreach(tl, selectQuery->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tl);

				if (!tle->resjunk)
					*targetlist = lappend(*targetlist, tle);
			}
		}

		/*
		 * 让叶子查询成为顶级范围表中的一个子查询。  
		 */
		snprintf(selectName, sizeof(selectName), "*SELECT* %d",
				 list_length(pstate->p_rtable) + 1);
		nsitem = addRangeTableEntryForSubquery(pstate,
											   selectQuery,
											   makeAlias(selectName, NIL),
											   false,
											   false);

		/*
		 * 返回 RangeTblRef 以替换集合操作树中的 SelectStmt。   
		 */
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = nsitem->p_rtindex;
		return (Node *) rtr;
	}
	else
	{
		/* 处理内部节点（集合操作节点）    */
		SetOperationStmt *op = makeNode(SetOperationStmt);
		List	   *ltargetlist;
		List	   *rtargetlist;
		ListCell   *ltl;
		ListCell   *rtl;
		const char *context;
		bool		recursive = (pstate->p_parent_cte &&
								 pstate->p_parent_cte->cterecursive);

		context = (stmt->op == SETOP_UNION ? "UNION" :
				   (stmt->op == SETOP_INTERSECT ? "INTERSECT" :
					"EXCEPT"));

		op->op = stmt->op;
		op->all = stmt->all;

		/*
		 * 递归转换左子节点。               
		 */
		op->larg = transformSetOperationTree(pstate, stmt->larg,
											 false,
											 &ltargetlist);

		/*
		 * 若在处理递归 union 查询，现在就该检查非递归项
		 * 的输出列，并把包含这些列的 CTE 标记为拥有 
		 * 这些结果列。当然只在 CTE 顶层 setop 上这样做，  
		 *                                                 
		 */
		if (isTopLevel && recursive)
			determineRecursiveColTypes(pstate, op->larg, ltargetlist);

		/*
		 * 递归转换右子节点。                
		 */
		op->rarg = transformSetOperationTree(pstate, stmt->rarg,
											 false,
											 &rtargetlist);

		/*
		 * 验证两个子节点非垃圾列数相同，并确定合并 
		 * 后输出列的类型。                                      
		 */
		if (list_length(ltargetlist) != list_length(rtargetlist))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("each %s query must have the same number of columns",
							context),
					 parser_errposition(pstate,
										exprLocation((Node *) rtargetlist))));

		if (targetlist)
			*targetlist = NIL;
		op->colTypes = NIL;
		op->colTypmods = NIL;
		op->colCollations = NIL;
		op->groupClauses = NIL;
		forboth(ltl, ltargetlist, rtl, rtargetlist)
		{
			TargetEntry *ltle = (TargetEntry *) lfirst(ltl);
			TargetEntry *rtle = (TargetEntry *) lfirst(rtl);
			Node	   *lcolnode = (Node *) ltle->expr;
			Node	   *rcolnode = (Node *) rtle->expr;
			Oid			lcoltype = exprType(lcolnode);
			Oid			rcoltype = exprType(rcolnode);
			Node	   *bestexpr;
			int			bestlocation;
			Oid			rescoltype;
			int32		rescoltypmod;
			Oid			rescolcoll;

			/* 选择共同类型，与 CASE 等相同*/
			rescoltype = select_common_type(pstate,
											list_make2(lcolnode, rcolnode),
											context,
											&bestexpr);
			bestlocation = exprLocation(bestexpr);

			/*
			 * 验证强转确实可行；否则稍后会失败，但我们  
			 * 希望现在就失败，趁有足够上下文可给出错误  
			 * 光标位置。                             
			 *
			 * 对非 UNKNOWN 类型情形，只验证可强转性，并不 
			 * 修改子表达式，以免改变子查询的语义。       
			 *                         
			 *
			 * 若子表达式为 UNKNOWN 的 Const 或 Param，我们    
			 * 想把它替换为强转后的表达式。这只可能        
			 * 发生在子节点为叶子集合操作节点时。替换 
			 * 表达式是安全的，因为若子查询语义依赖它， 
			 * 输出列类型，那早就会把 UNKNOWN 强转成别的    
			 * 这么做是因为 (a) 要验证 Const 对目标类型    
			 * 有效，或解析 UNKNOWN Param 的实际类型；(b)     
			 * 我们想避免子查询输出类型与解析目标类型间
			 * 出现不必要的差异。                                   
			 *                                                             
			 * 这种差异会禁用规划器中的优化。                
			 *
			 * 若是其他 UNKNOWN 节点（如 Var），我们就不做，
			 * （coerce_to_common_type 会失败）。规划器有时能   
			 * 在强转类型前把 UNKNOWN Var 折叠为常量，故      
			 * 现在就失败只会破坏原本可能成功的情形。 
			 *                       
			 */
			if (lcoltype != UNKNOWNOID)
				lcolnode = coerce_to_common_type(pstate, lcolnode,
												 rescoltype, context);
			else if (IsA(lcolnode, Const) ||
					 IsA(lcolnode, Param))
			{
				lcolnode = coerce_to_common_type(pstate, lcolnode,
												 rescoltype, context);
				ltle->expr = (Expr *) lcolnode;
			}

			if (rcoltype != UNKNOWNOID)
				rcolnode = coerce_to_common_type(pstate, rcolnode,
												 rescoltype, context);
			else if (IsA(rcolnode, Const) ||
					 IsA(rcolnode, Param))
			{
				rcolnode = coerce_to_common_type(pstate, rcolnode,
												 rescoltype, context);
				rtle->expr = (Expr *) rcolnode;
			}

			rescoltypmod = select_common_typmod(pstate,
												list_make2(lcolnode, rcolnode),
												rescoltype);

			/*
			 * 选共同排序规则。UNION ALL 以外的集合操作     
			 * 都需要共同排序规则；见 SQL:2008 7.13 <query      
			 * expression> 语法规则 15c。（如果无法为 UNION ALL   
			 * 的列找到共同排序规则，colCollations 元素设为
			 * InvalidOid，若上层查询要用该列的排序规则，就 
			 * 可能在运行时出错。                                   
			 *            
			 */
			rescolcoll = select_common_collation(pstate,
												 list_make2(lcolnode, rcolnode),
												 (op->op == SETOP_UNION && op->all));

			/* 产出结果 */
			op->colTypes = lappend_oid(op->colTypes, rescoltype);
			op->colTypmods = lappend_int(op->colTypmods, rescoltypmod);
			op->colCollations = lappend_oid(op->colCollations, rescolcoll);

			/*
			 * 除 UNION ALL 之外的情形，找出将用于消除重复的 
			 * 分组操作符（及排序操作符，若有）。        
			 *                      
			 */
			if (op->op != SETOP_UNION || !op->all)
			{
				ParseCallbackState pcbstate;

				setup_parser_errposition_callback(&pcbstate, pstate,
												  bestlocation);

				/*
				 * 若是递归 union，就需要哈希支持。        
				 *         
				 */
				op->groupClauses = lappend(op->groupClauses,
										   makeSortGroupClauseForSetOp(rescoltype, recursive));

				cancel_parser_errposition_callback(&pcbstate);
			}

			/*
			 * 构建一个返回用的虚拟 tlist 条目。用 SetToDefault 
			 * 节点作表达式，因为它正好带有需要的字段，
			 * 但任何其他表达式节点类型也同样可行。      
			 */
			if (targetlist)
			{
				SetToDefault *rescolnode = makeNode(SetToDefault);
				TargetEntry *restle;

				rescolnode->typeId = rescoltype;
				rescolnode->typeMod = rescoltypmod;
				rescolnode->collation = rescolcoll;
				rescolnode->location = bestlocation;
				restle = makeTargetEntry((Expr *) rescolnode,
										 0, /* 无需设置 resno   */
										 NULL,
										 false);
				*targetlist = lappend(*targetlist, restle);
			}
		}

		return (Node *) op;
	}
}

/*
 * 处理递归 UNION 非递归项的输出，                       
 * 以建立父 CTE 的列           
 */
static void
determineRecursiveColTypes(ParseState *pstate, Node *larg, List *nrtargetlist)
{
	Node	   *node;
	int			leftmostRTI;
	Query	   *leftmostQuery;
	List	   *targetList;
	ListCell   *left_tlist;
	ListCell   *nrtl;
	int			next_resno;

	/*
	 * 找最左叶子 SELECT   
	 */
	node = larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) node)->rtindex;
	leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * 使用最左侧 select 的列名与非递归项的                 
	 * 哑结果表达式生成哑目标列表。         
	 */
	targetList = NIL;
	next_resno = 1;

	forboth(nrtl, nrtargetlist, left_tlist, leftmostQuery->targetList)
	{
		TargetEntry *nrtle = (TargetEntry *) lfirst(nrtl);
		TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
		char	   *colName;
		TargetEntry *tle;

		Assert(!lefttle->resjunk);
		colName = pstrdup(lefttle->resname);
		tle = makeTargetEntry(nrtle->expr,
							  next_resno++,
							  colName,
							  false);
		targetList = lappend(targetList, tle);
	}

	/* 现在用哑目标列表构建 CTE 的输出列信息     */
	analyzeCTETargetList(pstate, pstate->p_parent_cte, targetList);
}


/*
 * transformReturnStmt -
 *	  转换 return 语句         
 */
static Query *
transformReturnStmt(ParseState *pstate, ReturnStmt *stmt)
{
	Query	   *qry = makeNode(Query);

	qry->commandType = CMD_SELECT;
	qry->isReturn = true;

	qry->targetList = list_make1(makeTargetEntry((Expr *) transformExpr(pstate, stmt->returnval, EXPR_KIND_SELECT_TARGET),
												 1, NULL, false));

	if (pstate->p_resolve_unknowns)
		resolveTargetListUnknowns(pstate, qry->targetList);
	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);
	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	assign_query_collations(pstate, qry);

	return qry;
}


/*
 * transformUpdateStmt -
 *	  转换 UPDATE 语句          
 */
static Query *
transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ParseNamespaceItem *nsitem;
	Node	   *qual;

	qry->commandType = CMD_UPDATE;
	pstate->p_is_insert = false;

	/* 独立于其他一切单独处理 WITH 子句     */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 stmt->relation->inh,
										 true,
										 ACL_UPDATE);
	nsitem = pstate->p_target_nsitem;

	/* FROM 中的子查询无法访问结果关系         */
	nsitem->p_lateral_only = true;
	nsitem->p_lateral_ok = false;

	/*
	 * FROM 子句是非标准的 SQL 语法。我们过去能够在       
	 * POSTQUEL 中以 REPLACE 实现之，故保留。     
	 */
	transformFromClause(pstate, stmt->fromClause);

	/* 其余子句可正常引用结果关系                      */
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	qry->returningList = transformReturningList(pstate, stmt->returningList);

	/*
	 * 现在 SELECT 类处理已完成，可以继续                 
	 * 转换目标列表以匹配 UPDATE 目标列。                 
	 */
	qry->targetList = transformUpdateTargetList(pstate, stmt->targetList);

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * transformUpdateTargetList -
 *	UPDATE/MERGE/INSERT ... ON CONFLICT UPDATE 的 SET 子句处理
 */
List *
transformUpdateTargetList(ParseState *pstate, List *origTlist)
{
	List	   *tlist = NIL;
	RTEPermissionInfo *target_perminfo;
	ListCell   *orig_tl;
	ListCell   *tl;

	tlist = transformTargetList(pstate, origTlist,
								EXPR_KIND_UPDATE_SOURCE);

	/* 准备为 resjunk 属性分配互不冲突的 resno            */
	if (pstate->p_next_resno <= RelationGetNumberOfAttributes(pstate->p_target_relation))
		pstate->p_next_resno = RelationGetNumberOfAttributes(pstate->p_target_relation) + 1;

	/* 准备非垃圾列以便赋给目标表                 */
	target_perminfo = pstate->p_target_nsitem->p_perminfo;
	orig_tl = list_head(origTlist);

	foreach(tl, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);
		ResTarget  *origTarget;
		int			attrno;

		if (tle->resjunk)
		{
			/*
			 * resjunk 节点无需额外处理，但务必确保           
			 * 其 resno 不与任何目标列匹配；否则重写器        
			 * 或规划器可能被搞糊涂。resname 也无需。    
			 *        
			 */
			tle->resno = (AttrNumber) pstate->p_next_resno++;
			tle->resname = NULL;
			continue;
		}
		if (orig_tl == NULL)
			elog(ERROR, "UPDATE target count mismatch --- internal error");
		origTarget = lfirst_node(ResTarget, orig_tl);

		attrno = attnameAttNum(pstate->p_target_relation,
							   origTarget->name, true);
		if (attrno == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							origTarget->name,
							RelationGetRelationName(pstate->p_target_relation)),
					 parser_errposition(pstate, origTarget->location)));

		updateTargetListEntry(pstate, tle, origTarget->name,
							  attrno,
							  origTarget->indirection,
							  origTarget->location);

		/* 将目标列标记为需要 UPDATE 权限              */
		target_perminfo->updatedCols = bms_add_member(target_perminfo->updatedCols,
													  attrno - FirstLowInvalidHeapAttributeNumber);

		orig_tl = lnext(origTlist, orig_tl);
	}
	if (orig_tl != NULL)
		elog(ERROR, "UPDATE target count mismatch --- internal error");

	return tlist;
}

/*
 * transformReturningList -
 *	INSERT/UPDATE/DELETE 的 RETURNING 子句处理  
 */
static List *
transformReturningList(ParseState *pstate, List *returningList)
{
	List	   *rlist;
	int			save_next_resno;

	if (returningList == NIL)
		return NIL;				/* 无事可做  */

	/*
	 * RETURNING 列表的 resno 需从 1 开始分配。为此            
	 * 要保存并恢复主 tlist 的 p_next_resno 值，以防       
	 * 日后有人查看（大概率不会发生）。  
	 */
	save_next_resno = pstate->p_next_resno;
	pstate->p_next_resno = 1;

	/* 把 RETURNING 按 SELECT 目标列表同样转换      */
	rlist = transformTargetList(pstate, returningList, EXPR_KIND_RETURNING);

	/*
	 * 若非空 tlist 展开后为空则报错（当它只含              
	 * 零列表的星号展开时便可能发生）。我们若           
	 * 允许，解析出的 Query 就会看似没有 RETURNING，          
	 * 其结果多半会让用户感到意外。         
	 */
	if (rlist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("RETURNING must have at least one column"),
				 parser_errposition(pstate,
									exprLocation(linitial(returningList)))));

	/* 标记列来源     */
	markTargetListOrigins(pstate, rlist);

	/* 把仍未定型的输出列定为 text 类型                  */
	if (pstate->p_resolve_unknowns)
		resolveTargetListUnknowns(pstate, rlist);

	/* 恢复状态  */
	pstate->p_next_resno = save_next_resno;

	return rlist;
}


/*
 * transformPLAssignStmt -
 *	  转换 PL/pgSQL 赋值语句             
 *
 * 若没有 opt_indirection，转换后的语句形如                
 * "SELECT a_expr ..."，只是表达式已被                            
 * 强制转换为目标类型。若有下标，它仍是 SELECT，           
 * 但表达式会纳入 FieldStore 和/或赋值 SubscriptingRef 节点，   
 * 为该目标代表的容器类型变量计算新值。                 
 * 表达式将目标引用为容器来源。               
 */
static Query *
transformPLAssignStmt(ParseState *pstate, PLAssignStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ColumnRef  *cref = makeNode(ColumnRef);
	List	   *indirection = stmt->indirection;
	int			nnames = stmt->nnames;
	SelectStmt *sstmt = stmt->val;
	Node	   *target;
	Oid			targettype;
	int32		targettypmod;
	Oid			targetcollation;
	List	   *tlist;
	TargetEntry *tle;
	Oid			type_id;
	Node	   *qual;
	ListCell   *l;

	/*
	 * 首先，为目标变量构造 ColumnRef。若目标                
	 * 带有多个点分名称，就得把多余名称从间接            
	 * 列表中移出。   
	 */
	cref->fields = list_make1(makeString(stmt->name));
	cref->location = stmt->location;
	if (nnames > 1)
	{
		/* 避免弄乱原始解析树     */
		indirection = list_copy(indirection);
		while (--nnames > 0 && indirection != NIL)
		{
			Node	   *ind = (Node *) linitial(indirection);

			if (!IsA(ind, String))
				elog(ERROR, "invalid name count in PLAssignStmt");
			cref->fields = lappend(cref->fields, ind);
			indirection = list_delete_first(indirection);
		}
	}

	/*
	 * 转换目标引用。通常我们会得到一个 Param             
	 * 节点，但无需过分挑剔其类型。                 
	 */
	target = transformExpr(pstate, (Node *) cref,
						   EXPR_KIND_UPDATE_TARGET);
	targettype = exprType(target);
	targettypmod = exprTypmod(target);
	targetcollation = exprCollation(target);

	/*
	 * 其余部分大多与 transformSelectStmt 相同，只是无需    
	 * 考虑 WITH 或 INTO，且自行构建目标列表。        
	 */
	qry->commandType = CMD_SELECT;
	pstate->p_is_insert = false;

	/* 把 FOR UPDATE/FOR SHARE 信息提供给 addRangeTableEntry    */
	pstate->p_locking_clause = sstmt->lockingClause;

	/* 也让窗口函数能拿到 WINDOW 信息            */
	pstate->p_windowdefs = sstmt->windowClause;

	/* 处理 FROM 子句      */
	transformFromClause(pstate, sstmt->fromClause);

	/* 先按 SELECT 的情形初步转换目标列表    */
	tlist = transformTargetList(pstate, sstmt->targetList,
								EXPR_KIND_SELECT_TARGET);

	/* 应当恰好只有一个目标列表项    */
	if (list_length(tlist) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg_plural("assignment source returned %d column",
							   "assignment source returned %d columns",
							   list_length(tlist),
							   list_length(tlist))));

	tle = linitial_node(TargetEntry, tlist);

	/*
	 * 本段与 transformAssignedExpr 类似；关键差别是采用       
	 * COERCION_PLPGSQL 而非 COERCION_ASSIGNMENT。     
	 */
	type_id = exprType((Node *) tle->expr);

	pstate->p_expr_kind = EXPR_KIND_UPDATE_TARGET;

	if (indirection)
	{
		tle->expr = (Expr *)
			transformAssignmentIndirection(pstate,
										   target,
										   stmt->name,
										   false,
										   targettype,
										   targettypmod,
										   targetcollation,
										   indirection,
										   list_head(indirection),
										   (Node *) tle->expr,
										   COERCION_PLPGSQL,
										   exprLocation(target));
	}
	else if (targettype != type_id &&
			 (targettype == RECORDOID || ISCOMPLEX(targettype)) &&
			 (type_id == RECORDOID || ISCOMPLEX(type_id)))
	{
		/*
		 * Hack：别让 coerce_to_target_type() 处理不一致的       
		 * 组合类型。表达式结果原样透传即可，             
		 * 类型转换交给 PL/pgSQL 执行器自行完成。             
		 * 这做法颇为拙劣，但为向后兼容所必需。    
		 */
	}
	else
	{
		/*
		 * 对普通未限定的目标列做类型检查与转换。   
		 *          
		 */
		Node	   *orig_expr = (Node *) tle->expr;

		tle->expr = (Expr *)
			coerce_to_target_type(pstate,
								  orig_expr, type_id,
								  targettype, targettypmod,
								  COERCION_PLPGSQL,
								  COERCE_IMPLICIT_CAST,
								  -1);
		/* 在 COERCION_PLPGSQL 下此错误多半不可达          */
		if (tle->expr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("variable \"%s\" is of type %s"
							" but expression is of type %s",
							stmt->name,
							format_type_be(targettype),
							format_type_be(type_id)),
					 errhint("You will need to rewrite or cast the expression."),
					 parser_errposition(pstate, exprLocation(orig_expr))));
	}

	pstate->p_expr_kind = EXPR_KIND_NONE;

	qry->targetList = list_make1(tle);

	/* 转换 WHERE    */
	qual = transformWhereClause(pstate, sstmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	/* HAVING 子句的初步处理与 WHERE 子句十分类似      */
	qry->havingQual = transformWhereClause(pstate, sstmt->havingClause,
										   EXPR_KIND_HAVING, "HAVING");

	/*
	 * 转换排序/分组相关内容。先做 ORDER BY，因为       
	 * transformGroupClause 与 transformDistinctClause 都需要             
	 * 其结果。另请注意这些函数还会改动 targetList，        
	 * 按引用传入。
	 */
	qry->sortClause = transformSortClause(pstate,
										  sstmt->sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* 按 SQL92 规则  */ );

	qry->groupClause = transformGroupClause(pstate,
											sstmt->groupClause,
											&qry->groupingSets,
											&qry->targetList,
											qry->sortClause,
											EXPR_KIND_GROUP_BY,
											false /* 按 SQL92 规则  */ );
	qry->groupDistinct = sstmt->groupDistinct;

	if (sstmt->distinctClause == NIL)
	{
		qry->distinctClause = NIL;
		qry->hasDistinctOn = false;
	}
	else if (linitial(sstmt->distinctClause) == NULL)
	{
		/* 原为 SELECT DISTINCT */
		qry->distinctClause = transformDistinctClause(pstate,
													  &qry->targetList,
													  qry->sortClause,
													  false);
		qry->hasDistinctOn = false;
	}
	else
	{
		/* 原为 SELECT DISTINCT ON */
		qry->distinctClause = transformDistinctOnClause(pstate,
														sstmt->distinctClause,
														&qry->targetList,
														qry->sortClause);
		qry->hasDistinctOn = true;
	}

	/* 转换 LIMIT    */
	qry->limitOffset = transformLimitClause(pstate, sstmt->limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											sstmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, sstmt->limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   sstmt->limitOption);
	qry->limitOption = sstmt->limitOption;

	/* 在见过所有窗口函数之后再转换窗口子句           */
	qry->windowClause = transformWindowDefinitions(pstate,
												   pstate->p_windowdefs,
												   &qry->targetList);

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	foreach(l, sstmt->lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	assign_query_collations(pstate, qry);

	/* 此步须在排序规则之后做，以便可靠地比较表达式   */
	if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
		parseCheckAggregates(pstate, qry);

	return qry;
}


/*
 * transformDeclareCursorStmt -
 *	转换 DECLARE CURSOR 语句        
 *
 * DECLARE CURSOR 与其他实用语句类似：我们也把它作为     
 * CMD_UTILITY Query 节点发出；但必须先转换其                
 * 所含查询。过去我们把它推迟到执行时，但                  
 * 确实必须在正常解析分析阶段完成，从而确保               
 * 解析器钩子的副作用如期发生。  
 */
static Query *
transformDeclareCursorStmt(ParseState *pstate, DeclareCursorStmt *stmt)
{
	Query	   *result;
	Query	   *query;

	if ((stmt->options & CURSOR_OPT_SCROLL) &&
		(stmt->options & CURSOR_OPT_NO_SCROLL))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
		/* translator: %s is a SQL keyword */
				 errmsg("cannot specify both %s and %s",
						"SCROLL", "NO SCROLL")));

	if ((stmt->options & CURSOR_OPT_ASENSITIVE) &&
		(stmt->options & CURSOR_OPT_INSENSITIVE))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
		/* translator: %s is a SQL keyword */
				 errmsg("cannot specify both %s and %s",
						"ASENSITIVE", "INSENSITIVE")));

	/* 转换所含查询，不允许 SELECT INTO          */
	query = transformStmt(pstate, stmt->query);
	stmt->query = (Node *) query;

	/* 文法本不应允许 SELECT 以外的任何东西  */
	if (!IsA(query, Query) ||
		query->commandType != CMD_SELECT)
		elog(ERROR, "unexpected non-SELECT command in DECLARE CURSOR");

	/*
	 * 游标中同样禁止数据修改 WITH。（这本可以         
	 * 允许，但更新发生时机的语义可能意外。）    
	 *             
	 */
	if (query->hasModifyingCTE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("DECLARE CURSOR must not contain data-modifying statements in WITH")));

	/* FOR UPDATE 与 WITH HOLD 不兼容          */
	if (query->rowMarks != NIL && (stmt->options & CURSOR_OPT_HOLD))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("DECLARE CURSOR WITH HOLD ... %s is not supported",
						LCS_asString(((RowMarkClause *)
									  linitial(query->rowMarks))->strength)),
				 errdetail("Holdable cursors must be READ ONLY.")));

	/* FOR UPDATE 与 SCROLL 不兼容          */
	if (query->rowMarks != NIL && (stmt->options & CURSOR_OPT_SCROLL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("DECLARE SCROLL CURSOR ... %s is not supported",
						LCS_asString(((RowMarkClause *)
									  linitial(query->rowMarks))->strength)),
				 errdetail("Scrollable cursors must be READ ONLY.")));

	/* FOR UPDATE 与 INSENSITIVE 不兼容          */
	if (query->rowMarks != NIL && (stmt->options & CURSOR_OPT_INSENSITIVE))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("DECLARE INSENSITIVE CURSOR ... %s is not valid",
						LCS_asString(((RowMarkClause *)
									  linitial(query->rowMarks))->strength)),
				 errdetail("Insensitive cursors must be READ ONLY.")));

	/* 把该命令表示为实用 Query        */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}


/*
 * transformExplainStmt -
 *	转换 EXPLAIN 语句         
 *
 * EXPLAIN 与其他实用语句类似：我们也把它作为     
 * CMD_UTILITY Query 节点发出；但必须先转换其                
 * 所含查询。过去我们把它推迟到执行时，但                  
 * 确实必须在正常解析分析阶段完成，从而确保               
 * 解析器钩子的副作用如期发生。  
 */
static Query *
transformExplainStmt(ParseState *pstate, ExplainStmt *stmt)
{
	Query	   *result;
	bool		generic_plan = false;
	Oid		   *paramTypes = NULL;
	int			numParams = 0;

	/*
	 * 若没有参数定义的外部来源，且指定了            
	 * GENERIC_PLAN 选项，则接受可变参数定义               
	 * （例如类似 PREPARE 的做法）。         
	 */
	if (pstate->p_paramref_hook == NULL)
	{
		ListCell   *lc;

		foreach(lc, stmt->options)
		{
			DefElem    *opt = (DefElem *) lfirst(lc);

			if (strcmp(opt->defname, "generic_plan") == 0)
				generic_plan = defGetBoolean(opt);
			/* 勿 "break"，我们要取最后的值   */
		}
		if (generic_plan)
			setup_parse_variable_parameters(pstate, &paramTypes, &numParams);
	}

	/* 转换所含查询，允许 SELECT INTO         */
	stmt->query = (Node *) transformOptionalSelectInto(pstate, stmt->query);

	/* 确保参数类型没有问题             */
	if (generic_plan)
		check_variable_parameters(pstate, (Query *) stmt->query);

	/* 把该命令表示为实用 Query        */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}


/*
 * transformCreateTableAsStmt -
 *	转换 CREATE TABLE AS, SELECT ... INTO 或 CREATE MATERIALIZED VIEW     
 *	语句   
 *
 * 与 DECLARE CURSOR 和 EXPLAIN 一样，现在就转换所含语句。    
 */
static Query *
transformCreateTableAsStmt(ParseState *pstate, CreateTableAsStmt *stmt)
{
	Query	   *result;
	Query	   *query;

	/* 转换所含查询，不允许 SELECT INTO          */
	query = transformStmt(pstate, stmt->query);
	stmt->query = (Node *) query;

	/* CREATE MATERIALIZED VIEW 还需额外处理         */
	if (stmt->objtype == OBJECT_MATVIEW)
	{
		/*
		 * 禁止在创建物化视图的查询中使用数据修改  
		 * 的 CTE。MV 若被刷新或做增量维护，用户想           
		 * 得到什么并不明确。                                       
		 */
		if (query->hasModifyingCTE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views must not use data-modifying statements in WITH")));

		/*
		 * 检查创建查询是否用到了任何临时数据库      
		 * 对象。若某个来源消失，数据将难以刷新           
		 * 或增量维护。                  
		 */
		if (isQueryUsingTempRelation(query))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views must not use temporary tables or views")));

		/*
		 * 物化视图要么得保存参数供维护/加载数据            
		 * 之用，要么完全禁用参数。后者                         
		 * 看来更稳妥合理。  
		 */
		if (query_contains_extern_params(query))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views may not be defined using bound parameters")));

		/*
		 * 目前我们不允许无日志的物化视图，因为            
		 * 崩溃后它们直接变空似乎是个坏主意。（若         
		 * 能将其标记为未填充会更好，但这需要对        
		 * 系统目录的修改，崩溃恢复目前无法处理。） 
		 *         
		 */
		if (stmt->into->rel->relpersistence == RELPERSISTENCE_UNLOGGED)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views cannot be unlogged")));

		/*
		 * 运行时我们需要一份已解析但未重写的                
		 * Query 副本，用于创建视图的 ON SELECT 规则。          
		 * 存入 IntoClause，intorel_startup() 便能方便地       
		 * 从中取到它。       
		 */
		stmt->into->viewQuery = (Node *) copyObject(query);
	}

	/* 把该命令表示为实用 Query        */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}

/*
 * 转换 CallStmt     
 */
static Query *
transformCallStmt(ParseState *pstate, CallStmt *stmt)
{
	List	   *targs;
	ListCell   *lc;
	Node	   *node;
	FuncExpr   *fexpr;
	HeapTuple	proctup;
	Datum		proargmodes;
	bool		isNull;
	List	   *outargs = NIL;
	Query	   *result;

	/*
	 * 首先对过程调用及其参数做标准解析分析，      
	 * 从而识别出被调用的过程。                    
	 */
	targs = NIL;
	foreach(lc, stmt->funccall->args)
	{
		targs = lappend(targs, transformExpr(pstate,
											 (Node *) lfirst(lc),
											 EXPR_KIND_CALL_ARGUMENT));
	}

	node = ParseFuncOrColumn(pstate,
							 stmt->funccall->funcname,
							 targs,
							 pstate->p_last_srf,
							 stmt->funccall,
							 true,
							 stmt->funccall->location);

	assign_expr_collations(pstate, node);

	fexpr = castNode(FuncExpr, node);

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fexpr->funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", fexpr->funcid);

	/*
	 * 展开参数列表以处理命名参数记法和默认           
	 * 参数。对普通 FuncExpr 这本可在规划期间            
	 * 完成，但 CallStmt 不经过规划阶段，                       
	 * 没理由不在此处做。      
	 */
	fexpr->args = expand_function_arguments(fexpr->args,
											true,
											fexpr->funcresulttype,
											proctup);

	/* 获取 proargmodes；若为空则没有输出参数       */
	proargmodes = SysCacheGetAttr(PROCOID, proctup,
								  Anum_pg_proc_proargmodes,
								  &isNull);
	if (!isNull)
	{
		/*
		 * 把列表拆为 fexpr->args 的输入参数与               
		 * stmt->outargs 的输出参数，INOUT 参数两边皆有。       
		 */
		ArrayType  *arr;
		int			numargs;
		char	   *argmodes;
		List	   *inargs;
		int			i;

		arr = DatumGetArrayTypeP(proargmodes);	/* 确保未 TOAST 化*/
		numargs = list_length(fexpr->args);
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "proargmodes is not a 1-D char array of length %d or it contains nulls",
				 numargs);
		argmodes = (char *) ARR_DATA_PTR(arr);

		inargs = NIL;
		i = 0;
		foreach(lc, fexpr->args)
		{
			Node	   *n = lfirst(lc);

			switch (argmodes[i])
			{
				case PROARGMODE_IN:
				case PROARGMODE_VARIADIC:
					inargs = lappend(inargs, n);
					break;
				case PROARGMODE_OUT:
					outargs = lappend(outargs, n);
					break;
				case PROARGMODE_INOUT:
					inargs = lappend(inargs, n);
					outargs = lappend(outargs, copyObject(n));
					break;
				default:
					/* 注意我们不支持 PROARGMODE_TABLE */
					elog(ERROR, "invalid argmode %c for procedure",
						 argmodes[i]);
					break;
			}
			i++;
		}
		fexpr->args = inargs;
	}

	stmt->funcexpr = fexpr;
	stmt->outargs = outargs;

	ReleaseSysCache(proctup);

	/* 将该命令表示为实用命令 Query  */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}

/*
 * 生成 LockClauseStrength 值的字符串表示。            
 * 只应对有效值使用（LCS_NONE 除外）。           
 */
const char *
LCS_asString(LockClauseStrength strength)
{
	switch (strength)
	{
		case LCS_NONE:
			Assert(false);
			break;
		case LCS_FORKEYSHARE:
			return "FOR KEY SHARE";
		case LCS_FORSHARE:
			return "FOR SHARE";
		case LCS_FORNOKEYUPDATE:
			return "FOR NO KEY UPDATE";
		case LCS_FORUPDATE:
			return "FOR UPDATE";
	}
	return "FOR some";			/* 不应发生     */
}

/*
 * 检查 FOR [KEY] UPDATE/SHARE 不支持的特性。                   
 *
 * 导出以便规划器在重写、查询提升等后再次检查       
 */
void
CheckSelectLocking(Query *qry, LockClauseStrength strength)
{
	Assert(strength != LCS_NONE);	/* 否则调用出错*/

	if (qry->setOperations)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(strength))));
	if (qry->distinctClause != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with DISTINCT clause",
						LCS_asString(strength))));
	if (qry->groupClause != NIL || qry->groupingSets != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with GROUP BY clause",
						LCS_asString(strength))));
	if (qry->havingQual != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with HAVING clause",
						LCS_asString(strength))));
	if (qry->hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with aggregate functions",
						LCS_asString(strength))));
	if (qry->hasWindowFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with window functions",
						LCS_asString(strength))));
	if (qry->hasTargetSRFs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with set-returning functions in the target list",
						LCS_asString(strength))));
}

/*
 * 转换 FOR [KEY] UPDATE/SHARE 子句     
 *
 * 其核心是把名称替换为整数 relid。             
 *
 * 注意：若需修改此处，另见 rewriteHandler.c 中的  
 * markQueryForLocking()、parse_relation.c 的 isLockedRefname() 
 */
static void
transformLockingClause(ParseState *pstate, Query *qry, LockingClause *lc,
					   bool pushedDown)
{
	List	   *lockedRels = lc->lockedRels;
	ListCell   *l;
	ListCell   *rt;
	Index		i;
	LockingClause *allrels;

	CheckSelectLocking(qry, lc->strength);

	/* 构造一个下传给子查询以选定所有关系的子句    */
	allrels = makeNode(LockingClause);
	allrels->lockedRels = NIL;	/* 表示所有关系 */
	allrels->strength = lc->strength;
	allrels->waitPolicy = lc->waitPolicy;

	if (lockedRels == NIL)
	{
		/*
		 * 锁定查询及其子查询中使用的所有常规表。    
		 * 检查 inFromCl 以排除自动加入的 RTE，尤其是         
		 * 规则中的 NEW/OLD。这有点滥用大多已过时的           
		 * 标志，但很方便。我们不能依赖已大体取代          
		 * inFromCl 的命名空间机制，例如我们需要锁定   
		 * 基础关系 RTE，即使它们被上层连接所遮掩。
		 */
		i = 0;
		foreach(rt, qry->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

			++i;
			if (!rte->inFromCl)
				continue;
			switch (rte->rtekind)
			{
				case RTE_RELATION:
					{
						RTEPermissionInfo *perminfo;

						applyLockingClause(qry, i,
										   lc->strength,
										   lc->waitPolicy,
										   pushedDown);
						perminfo = getRTEPermissionInfo(qry->rteperminfos, rte);
						perminfo->requiredPerms |= ACL_SELECT_FOR_UPDATE;
					}
					break;
				case RTE_SUBQUERY:
					applyLockingClause(qry, i, lc->strength, lc->waitPolicy,
									   pushedDown);

					/*
					 * 子查询的 FOR UPDATE/SHARE 也会传播到       
					 * 子查询的所有关系。本可稍后再做（基于 
					 * 子查询 RTE 的标记），但让每个查询层次
					 * 都本地了解哪些关系需以 RowShareLock 打开  
					 *                                          
					 */
					transformLockingClause(pstate, rte->subquery,
										   allrels, true);
					break;
				default:
					/* 忽略 JOIN/SPECIAL/FUNCTION/VALUES/CTE 型 RTE  */
					break;
			}
		}
	}
	else
	{
		/*
		 * 只锁定具名的表。同上，我们允许锁定任何基础 
		 * 关系而不论别名可见性规则，因此需要检查   
		 * inFromCl 以排除 OLD/NEW。       
		 */
		foreach(l, lockedRels)
		{
			RangeVar   *thisrel = (RangeVar *) lfirst(l);

			/* 为简单起见，此处要求别名为非限定名      */
			if (thisrel->catalogname || thisrel->schemaname)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				/*------
				  translator: %s is a SQL row locking clause such as FOR UPDATE */
						 errmsg("%s must specify unqualified relation names",
								LCS_asString(lc->strength)),
						 parser_errposition(pstate, thisrel->location)));

			i = 0;
			foreach(rt, qry->rtable)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);
				char	   *rtename = rte->eref->aliasname;

				++i;
				if (!rte->inFromCl)
					continue;

				/*
				 * 不带别名的连接 RTE 不会作为关系名可见，
				 * 需要跳过（否则会遮蔽同名基础关系）， 
				 * 除非它带有 USING 子句别名，此时该别名       
				 * *是*可见的。         
				 *
				 * 不带别名的子查询和 values RTE 从不作为       
				 * 关系名可见，必须始终跳过。      
				 */
				if (rte->alias == NULL)
				{
					if (rte->rtekind == RTE_JOIN)
					{
						if (rte->join_using_alias == NULL)
							continue;
						rtename = rte->join_using_alias->aliasname;
					}
					else if (rte->rtekind == RTE_SUBQUERY ||
							 rte->rtekind == RTE_VALUES)
						continue;
				}

				if (strcmp(rtename, thisrel->relname) == 0)
				{
					switch (rte->rtekind)
					{
						case RTE_RELATION:
							{
								RTEPermissionInfo *perminfo;

								applyLockingClause(qry, i,
												   lc->strength,
												   lc->waitPolicy,
												   pushedDown);
								perminfo = getRTEPermissionInfo(qry->rteperminfos, rte);
								perminfo->requiredPerms |= ACL_SELECT_FOR_UPDATE;
							}
							break;
						case RTE_SUBQUERY:
							applyLockingClause(qry, i, lc->strength,
											   lc->waitPolicy, pushedDown);
							/* 见上文注释   */
							transformLockingClause(pstate, rte->subquery,
												   allrels, true);
							break;
						case RTE_JOIN:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a join",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_FUNCTION:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a function",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_TABLEFUNC:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a table function",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_VALUES:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to VALUES",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_CTE:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a WITH query",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_NAMEDTUPLESTORE:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a named tuplestore",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;

							/* 此处不应出现 RTE_RESULT                */

						default:
							elog(ERROR, "unrecognized RTE type: %d",
								 (int) rte->rtekind);
							break;
					}
					break;		/* foreach 循环外   */
				}
			}
			if (rt == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
				/*------
				  translator: %s is a SQL row locking clause such as FOR UPDATE */
						 errmsg("relation \"%s\" in %s clause not found in FROM clause",
								thisrel->relname,
								LCS_asString(lc->strength)),
						 parser_errposition(pstate, thisrel->location)));
		}
	}
}

/*
 * 为单个范围表项记录锁定信息         
 */
void
applyLockingClause(Query *qry, Index rtindex,
				   LockClauseStrength strength, LockWaitPolicy waitPolicy,
				   bool pushedDown)
{
	RowMarkClause *rc;

	Assert(strength != LCS_NONE);	/* 否则调用出错*/

	/* 若是显式子句，则确保设置 hasForUpdate           */
	if (!pushedDown)
		qry->hasForUpdate = true;

	/* 检查同一 rtindex 是否已有现存条目 */
	if ((rc = get_parse_rowmark(qry, rtindex)) != NULL)
	{
		/*
		 * 若同一 RTE 被指定了不止一种锁定强度，则取      
		 * 最强者。（合理，因为共享锁与排他锁无法          
		 * 同时取得，反正最终都会以排他锁收场。）        
		 *         
		 *
		 * 类似地，若同一 RTE 被指定了不止一种锁等待    
		 * 策略，可认为 NOWAIT 优先于 SKIP LOCKED，               
		 * 而后者又优先于等待锁（默认行为）。这有        
		 * 点争议，但抛出错误似乎没什么帮助。（设想       
		 * 例如从内部含有普通 FOR UPDATE 声明的视图           
		 * 中发出 SELECT FOR UPDATE NOWAIT。）让                    
		 * NOWAIT 优先于 SKIP LOCKED 是合理的，因为              
		 * 前者在遇到已锁定元组时会抛出错误，某些     
		 * 场合下或许欠妥，但总比悄悄返回不一致结果  
		 *         
		 *
		 * 当然，任一子句为显式时 pushedDown 即为 false        
		 */
		rc->strength = Max(rc->strength, strength);
		rc->waitPolicy = Max(rc->waitPolicy, waitPolicy);
		rc->pushedDown &= pushedDown;
		return;
	}

	/* 新建 RowMarkClause     */
	rc = makeNode(RowMarkClause);
	rc->rti = rtindex;
	rc->strength = strength;
	rc->waitPolicy = waitPolicy;
	rc->pushedDown = pushedDown;
	qry->rowMarks = lappend(qry->rowMarks, rc);
}

/*
 * raw_expression_tree_walker() 的覆盖测试      
 *
 * 启用后，我们会对提交给解析分析的每条 DML                
 * 语句运行 raw_expression_tree_walker()。若无此安排，             
 * 该函数只在涉及 CTE 的少数场景下被应用，                 
 * 我们不想对 CTE 内外都做全面测试。    
 */
#ifdef RAW_EXPRESSION_COVERAGE_TEST

static bool
test_raw_expression_coverage(Node *node, void *context)
{
	if (node == NULL)
		return false;
	return raw_expression_tree_walker(node,
									  test_raw_expression_coverage,
									  context);
}

#endif							/* RAW_EXPRESSION_COVERAGE_TEST */
