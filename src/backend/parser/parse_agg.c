/*-------------------------------------------------------------------------
 *
 * parse_agg.c
 *	  在解析器中处理聚合与窗口函数      
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_agg.c
 *
 *---------------------------------------------------------------------    
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

typedef struct
{
	ParseState *pstate;
	int			min_varlevel;
	int			min_agglevel;
	int			sublevels_up;
} check_agg_arguments_context;

typedef struct
{
	ParseState *pstate;
	Query	   *qry;
	bool		hasJoinRTEs;
	List	   *groupClauses;
	List	   *groupClauseCommonVars;
	bool		have_non_var_grouping;
	List	  **func_grouped_rels;
	int			sublevels_up;
	bool		in_agg_direct_args;
} check_ungrouped_columns_context;

static int	check_agg_arguments(ParseState *pstate,
								List *directargs,
								List *args,
								Expr *filter);
static bool check_agg_arguments_walker(Node *node,
									   check_agg_arguments_context *context);
static void check_ungrouped_columns(Node *node, ParseState *pstate, Query *qry,
									List *groupClauses, List *groupClauseCommonVars,
									bool have_non_var_grouping,
									List **func_grouped_rels);
static bool check_ungrouped_columns_walker(Node *node,
										   check_ungrouped_columns_context *context);
static void finalize_grouping_exprs(Node *node, ParseState *pstate, Query *qry,
									List *groupClauses, bool hasJoinRTEs,
									bool have_non_var_grouping);
static bool finalize_grouping_exprs_walker(Node *node,
										   check_ungrouped_columns_context *context);
static void check_agglevels_and_constraints(ParseState *pstate, Node *expr);
static List *expand_groupingset_node(GroupingSet *gs);
static Node *make_agg_arg(Oid argtype, Oid argcollation);


/*
 * transformAggregateCall -
 *		完成聚合调用的初始转换                 
 *
 * parse_func.c 已认定该函数为聚合，并已设置好              
 * Aggref 中除 aggargtypes、aggdirectargs、args、                  
 * aggorder、aggdistinct 与 agglevelsup 之外的各字段。            
 * 传入的 args 列表已经过标准表达式转换和类型强转，    
 * 以匹配聚合声明的参数类型；而传入的 aggorder 列表    
 * 完全未转换。 
 *
 * 这里我们把 args 列表拆分为直接参数与聚合参数，         
 * 前者存入 agg->aggdirectargs，后者存入 agg->args。           
 * 普通参数（但不含直接参数）会通过插入 TargetEntry         
 * 节点而转换为目标列表；随后我们转换 aggorder 与   
 * agg_distinct 规格，为 agg->aggorder 与 agg->aggdistinct              
 * 生成 SortGroupClause 节点列表。（对普通聚合，          
 * 这可能给目标列表添入 resjunk 表达式；有序集     
 * 聚合的 aggorder 列表则始终与聚合参数一一对应。）       
 *       
 *
 * 我们还必须判定该聚合实际属于哪个查询层级，相应      
 * 设置 agglevelsup，并在相应 pstate 层级把 p_hasAggs             
 * 置为真。 
 */
void
transformAggregateCall(ParseState *pstate, Aggref *agg,
					   List *args, List *aggorder, bool agg_distinct)
{
	List	   *argtypes = NIL;
	List	   *tlist = NIL;
	List	   *torder = NIL;
	List	   *tdistinct = NIL;
	AttrNumber	attno = 1;
	int			save_next_resno;
	ListCell   *lc;

	if (AGGKIND_IS_ORDERED_SET(agg->aggkind))
	{
		/*
		 * 对有序集聚合，args 列表同时包含直接参数与   
		 * 聚合参数；必须拆分开来。      
		 */
		int			numDirectArgs = list_length(args) - list_length(aggorder);
		List	   *aargs;
		ListCell   *lc2;

		Assert(numDirectArgs >= 0);

		aargs = list_copy_tail(args, numDirectArgs);
		agg->aggdirectargs = list_truncate(args, numDirectArgs);

		/*
		 * 由聚合参数构建一个目标列表，并为每个参数     
		 * 建一个排序项。注意 SortBy 节点中的表达式        
		 * 会被忽略（它们是已转换参数的原始版本）；我们 
		 * 只关注 SortBy 节点中的排序信息。             
		 */
		forboth(lc, aargs, lc2, aggorder)
		{
			Expr	   *arg = (Expr *) lfirst(lc);
			SortBy	   *sortby = (SortBy *) lfirst(lc2);
			TargetEntry *tle;

			/* 不必为这些条目分配列名                     */
			tle = makeTargetEntry(arg, attno++, NULL, false);
			tlist = lappend(tlist, tle);

			torder = addTargetToSortList(pstate, tle,
										 torder, tlist, sortby);
		}

		/* 有序集聚合中绝无 DISTINCT        */
		Assert(!agg_distinct);
	}
	else
	{
		/* 普通聚合，因此没有直接参数     */
		agg->aggdirectargs = NIL;

		/*
		 * 把普通的 Exprs 列表转换成目标列表。   
		 */
		foreach(lc, args)
		{
			Expr	   *arg = (Expr *) lfirst(lc);
			TargetEntry *tle;

			/* 不必为这些条目分配列名                     */
			tle = makeTargetEntry(arg, attno++, NULL, false);
			tlist = lappend(tlist, tle);
		}

		/*
		 * 若有 ORDER BY，就转换它。这会给目标列表添加       
		 * 出现在 ORDER BY 中但原本不在参数列表里的列；  
		 * 它们会被标记 resjunk = true，以便日后能与普通       
		 * 聚合参数区分开来。            
		 *
		 * 我们需要改动 p_next_resno，因为它将被用来给       
		 * 新增目标项编号。   
		 */
		save_next_resno = pstate->p_next_resno;
		pstate->p_next_resno = attno;

		torder = transformSortClause(pstate,
									 aggorder,
									 &tlist,
									 EXPR_KIND_ORDER_BY,
									 true /* 按 SQL99 规则  */ );

		/*
		 * 若有 DISTINCT，则转换它以生成 distinctList。       
		 */
		if (agg_distinct)
		{
			tdistinct = transformDistinctClause(pstate, &tlist, torder, true);

			/*
			 * 若将来为聚合加了哈希去重的执行器支持，    
			 * 可移除此项检查。 
			 */
			foreach(lc, tdistinct)
			{
				SortGroupClause *sortcl = (SortGroupClause *) lfirst(lc);

				if (!OidIsValid(sortcl->sortop))
				{
					Node	   *expr = get_sortgroupclause_expr(sortcl, tlist);

					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_FUNCTION),
							 errmsg("could not identify an ordering operator for type %s",
									format_type_be(exprType(expr))),
							 errdetail("Aggregates with DISTINCT must be able to sort their inputs."),
							 parser_errposition(pstate, exprLocation(expr))));
				}
			}
		}

		pstate->p_next_resno = save_next_resno;
	}

	/* 用转换结果更新 Aggref                      */
	agg->args = tlist;
	agg->aggorder = torder;
	agg->aggdistinct = tdistinct;

	/*
	 * 现在用直接参数与聚合参数的类型 OID 来构建        
	 * aggargtypes 列表，忽略 ORDER BY/DISTINCT 处理              
	 * 可能加进来的 resjunk 项。这件事无法更早做，    
	 * 因为前述处理会改动某些参数的数据类型，尤其        
	 * 是把先前未决的 "unknown" 字面量确定。    
	 */
	foreach(lc, agg->aggdirectargs)
	{
		Expr	   *arg = (Expr *) lfirst(lc);

		argtypes = lappend_oid(argtypes, exprType((Node *) arg));
	}
	foreach(lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tle->resjunk)
			continue;			/* 忽略垃圾*/
		argtypes = lappend_oid(argtypes, exprType((Node *) tle->expr));
	}
	agg->aggargtypes = argtypes;

	check_agglevels_and_constraints(pstate, (Node *) agg);
}

/*
 * transformGroupingFunc
 *		转换 GROUPING 表达式      
 *
 * GROUPING() 的行为非常类似聚合。层级与嵌套的                 
 * 处理与聚合相同。对这些表达式同样会设置 p_hasAggs。 
 */
Node *
transformGroupingFunc(ParseState *pstate, GroupingFunc *p)
{
	ListCell   *lc;
	List	   *args = p->args;
	List	   *result_list = NIL;
	GroupingFunc *result = makeNode(GroupingFunc);

	if (list_length(args) > 31)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("GROUPING must have fewer than 32 arguments"),
				 parser_errposition(pstate, p->location)));

	foreach(lc, args)
	{
		Node	   *current_result;

		current_result = transformExpr(pstate, (Node *) lfirst(lc), pstate->p_expr_kind);

		/* 表达式的可接受性稍后检查          */

		result_list = lappend(result_list, current_result);
	}

	result->args = result_list;
	result->location = p->location;

	check_agglevels_and_constraints(pstate, (Node *) result);

	return (Node *) result;
}

/*
 * 聚合函数与分组操作（在标准中合称                           
 * <set function specification>）就层级和嵌套限制而言             
 * 非常相似（尽管我们允许的东西远多于标准）。             
 * 这里集中处理这些限制。  
 */
static void
check_agglevels_and_constraints(ParseState *pstate, Node *expr)
{
	List	   *directargs = NIL;
	List	   *args = NIL;
	Expr	   *filter = NULL;
	int			min_varlevel;
	int			location = -1;
	Index	   *p_levelsup;
	const char *err;
	bool		errkind;
	bool		isAgg = IsA(expr, Aggref);

	if (isAgg)
	{
		Aggref	   *agg = (Aggref *) expr;

		directargs = agg->aggdirectargs;
		args = agg->args;
		filter = agg->aggfilter;
		location = agg->location;
		p_levelsup = &agg->agglevelsup;
	}
	else
	{
		GroupingFunc *grp = (GroupingFunc *) expr;

		args = grp->args;
		location = grp->location;
		p_levelsup = &grp->agglevelsup;
	}

	/*
	 * 检查各参数以算出聚合的层级，并检测            
	 * 不当嵌套。  
	 */
	min_varlevel = check_agg_arguments(pstate,
									   directargs,
									   args,
									   filter);

	*p_levelsup = min_varlevel;

	/* 在正确的 pstate 层级标记含有聚合       */
	while (min_varlevel-- > 0)
		pstate = pstate->parentParseState;
	pstate->p_hasAggs = true;

	/*
	 * 检查聚合函数是否处于其聚合查询内的无效           
	 * 位置上。          
	 *
	 * 为简洁起见，这里支持两种报错方式：把 "err"       
	 * 设为自定义消息；或在错误上下文已能被               
	 * ParseExprKindName 的返回值充分标识，*且*该                  
	 * 返回值只是单个 SQL 关键字。（否则就用自定         
	 * 义消息，以免制造翻译问题。）      
	 */
	err = NULL;
	errkind = false;
	switch (pstate->p_expr_kind)
	{
		case EXPR_KIND_NONE:
			Assert(false);		/* 不会发生 */
			break;
		case EXPR_KIND_OTHER:

			/*
			 * 此处接受聚合/分组；如有需要，调用者须   
			 * 报错
			 */
			break;
		case EXPR_KIND_JOIN_ON:
		case EXPR_KIND_JOIN_USING:
			if (isAgg)
				err = _("aggregate functions are not allowed in JOIN conditions");
			else
				err = _("grouping operations are not allowed in JOIN conditions");

			break;
		case EXPR_KIND_FROM_SUBSELECT:

			/*
			 * 聚合/分组的作用域规则值得在此明确指出。 
			 *     
			 */
			if (isAgg)
				err = _("aggregate functions are not allowed in FROM clause of their own query level");
			else
				err = _("grouping operations are not allowed in FROM clause of their own query level");

			break;
		case EXPR_KIND_FROM_FUNCTION:
			if (isAgg)
				err = _("aggregate functions are not allowed in functions in FROM");
			else
				err = _("grouping operations are not allowed in functions in FROM");

			break;
		case EXPR_KIND_WHERE:
			errkind = true;
			break;
		case EXPR_KIND_POLICY:
			if (isAgg)
				err = _("aggregate functions are not allowed in policy expressions");
			else
				err = _("grouping operations are not allowed in policy expressions");

			break;
		case EXPR_KIND_HAVING:
			/* 可  */
			break;
		case EXPR_KIND_FILTER:
			errkind = true;
			break;
		case EXPR_KIND_WINDOW_PARTITION:
			/* 可  */
			break;
		case EXPR_KIND_WINDOW_ORDER:
			/* 可  */
			break;
		case EXPR_KIND_WINDOW_FRAME_RANGE:
			if (isAgg)
				err = _("aggregate functions are not allowed in window RANGE");
			else
				err = _("grouping operations are not allowed in window RANGE");

			break;
		case EXPR_KIND_WINDOW_FRAME_ROWS:
			if (isAgg)
				err = _("aggregate functions are not allowed in window ROWS");
			else
				err = _("grouping operations are not allowed in window ROWS");

			break;
		case EXPR_KIND_WINDOW_FRAME_GROUPS:
			if (isAgg)
				err = _("aggregate functions are not allowed in window GROUPS");
			else
				err = _("grouping operations are not allowed in window GROUPS");

			break;
		case EXPR_KIND_SELECT_TARGET:
			/* 可  */
			break;
		case EXPR_KIND_INSERT_TARGET:
		case EXPR_KIND_UPDATE_SOURCE:
		case EXPR_KIND_UPDATE_TARGET:
			errkind = true;
			break;
		case EXPR_KIND_MERGE_WHEN:
			if (isAgg)
				err = _("aggregate functions are not allowed in MERGE WHEN conditions");
			else
				err = _("grouping operations are not allowed in MERGE WHEN conditions");

			break;
		case EXPR_KIND_GROUP_BY:
			errkind = true;
			break;
		case EXPR_KIND_ORDER_BY:
			/* 可  */
			break;
		case EXPR_KIND_DISTINCT_ON:
			/* 可  */
			break;
		case EXPR_KIND_LIMIT:
		case EXPR_KIND_OFFSET:
			errkind = true;
			break;
		case EXPR_KIND_RETURNING:
			errkind = true;
			break;
		case EXPR_KIND_VALUES:
		case EXPR_KIND_VALUES_SINGLE:
			errkind = true;
			break;
		case EXPR_KIND_CHECK_CONSTRAINT:
		case EXPR_KIND_DOMAIN_CHECK:
			if (isAgg)
				err = _("aggregate functions are not allowed in check constraints");
			else
				err = _("grouping operations are not allowed in check constraints");

			break;
		case EXPR_KIND_COLUMN_DEFAULT:
		case EXPR_KIND_FUNCTION_DEFAULT:

			if (isAgg)
				err = _("aggregate functions are not allowed in DEFAULT expressions");
			else
				err = _("grouping operations are not allowed in DEFAULT expressions");

			break;
		case EXPR_KIND_INDEX_EXPRESSION:
			if (isAgg)
				err = _("aggregate functions are not allowed in index expressions");
			else
				err = _("grouping operations are not allowed in index expressions");

			break;
		case EXPR_KIND_INDEX_PREDICATE:
			if (isAgg)
				err = _("aggregate functions are not allowed in index predicates");
			else
				err = _("grouping operations are not allowed in index predicates");

			break;
		case EXPR_KIND_STATS_EXPRESSION:
			if (isAgg)
				err = _("aggregate functions are not allowed in statistics expressions");
			else
				err = _("grouping operations are not allowed in statistics expressions");

			break;
		case EXPR_KIND_ALTER_COL_TRANSFORM:
			if (isAgg)
				err = _("aggregate functions are not allowed in transform expressions");
			else
				err = _("grouping operations are not allowed in transform expressions");

			break;
		case EXPR_KIND_EXECUTE_PARAMETER:
			if (isAgg)
				err = _("aggregate functions are not allowed in EXECUTE parameters");
			else
				err = _("grouping operations are not allowed in EXECUTE parameters");

			break;
		case EXPR_KIND_TRIGGER_WHEN:
			if (isAgg)
				err = _("aggregate functions are not allowed in trigger WHEN conditions");
			else
				err = _("grouping operations are not allowed in trigger WHEN conditions");

			break;
		case EXPR_KIND_PARTITION_BOUND:
			if (isAgg)
				err = _("aggregate functions are not allowed in partition bound");
			else
				err = _("grouping operations are not allowed in partition bound");

			break;
		case EXPR_KIND_PARTITION_EXPRESSION:
			if (isAgg)
				err = _("aggregate functions are not allowed in partition key expressions");
			else
				err = _("grouping operations are not allowed in partition key expressions");

			break;
		case EXPR_KIND_GENERATED_COLUMN:

			if (isAgg)
				err = _("aggregate functions are not allowed in column generation expressions");
			else
				err = _("grouping operations are not allowed in column generation expressions");

			break;

		case EXPR_KIND_CALL_ARGUMENT:
			if (isAgg)
				err = _("aggregate functions are not allowed in CALL arguments");
			else
				err = _("grouping operations are not allowed in CALL arguments");

			break;

		case EXPR_KIND_COPY_WHERE:
			if (isAgg)
				err = _("aggregate functions are not allowed in COPY FROM WHERE conditions");
			else
				err = _("grouping operations are not allowed in COPY FROM WHERE conditions");

			break;

		case EXPR_KIND_CYCLE_MARK:
			errkind = true;
			break;

			/*
			 * 这里有意不设 default: 分支，这样在新增     
			 * ParseExprKind 而不扩展此 switch 时，编译器会 
			 * 发出警告。若运行时遇到无法识别的值，       
			 * 其行为与 EXPR_KIND_OTHER 相同，                        
			 * 反正也合理。   
			 */
	}

	if (err)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg_internal("%s", err),
				 parser_errposition(pstate, location)));

	if (errkind)
	{
		if (isAgg)
			/* translator: %s is name of a SQL construct, eg GROUP BY */
			err = _("aggregate functions are not allowed in %s");
		else
			/* translator: %s is name of a SQL construct, eg GROUP BY */
			err = _("grouping operations are not allowed in %s");

		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg_internal(err,
								 ParseExprKindName(pstate->p_expr_kind)),
				 parser_errposition(pstate, location)));
	}
}

/*
 * check_agg_arguments
 *	  扫描聚合函数的参数，以判定该聚合的语义   
 *	  层级（零为当前 select 的层级，一为其父层，    
 *	  依此类推）。      
 *
 * 聚合的层级等同于其聚合参数（含任何 ORDER BY               
 * 列）或过滤表达式中最低层级变量或聚合的层级；       
 * 若其中完全不含变量，我们就假定它是局部的。           
 *             
 *
 * 直接参数中的 Var/Agg *不*计入层级的判定，               
 * 因为那些参数并非逐行求值，而是每组只求        
 * 值一次，某种意义上并不算真正的聚合参数。          
 * 不过这可能导致：即便直接参数里含有更低层           
 * 级的 Var/Agg，我们仍判定聚合为上层，而这种情况      
 * 必须被禁止。（这有点怪，但 SQL 标准似乎相当            
 * 明确：设定聚合层级时不应考虑直接参数。）       
 *
 * 我们也借机检测参数中嵌套的聚合或窗口函数。           
 * 若发现窗口函数可立即报错。聚合则麻烦一些，        
 * 因为只有当内层聚合与外层处于同一语义层级          
 * 时才算错误，而这要等参数扫描完毕才能知道。         
 *                                                            
 */
static int
check_agg_arguments(ParseState *pstate,
					List *directargs,
					List *args,
					Expr *filter)
{
	int			agglevel;
	check_agg_arguments_context context;

	context.pstate = pstate;
	context.min_varlevel = -1;	/* 尚未找到任何内容    */
	context.min_agglevel = -1;
	context.sublevels_up = 0;

	(void) check_agg_arguments_walker((Node *) args, &context);
	(void) check_agg_arguments_walker((Node *) filter, &context);

	/*
	 * 若完全没找到变量和聚合，它就是零层聚合；     
	 * 否则其层级取变量或聚合中的最小值。 
	 */
	if (context.min_varlevel < 0)
	{
		if (context.min_agglevel < 0)
			agglevel = 0;
		else
			agglevel = context.min_agglevel;
	}
	else if (context.min_agglevel < 0)
		agglevel = context.min_varlevel;
	else
		agglevel = Min(context.min_varlevel, context.min_agglevel);

	/*
	 * 若存在同一语义层级的嵌套聚合，则报错。          
	 */
	if (agglevel == context.min_agglevel)
	{
		int			aggloc;

		aggloc = locate_agg_of_level((Node *) args, agglevel);
		if (aggloc < 0)
			aggloc = locate_agg_of_level((Node *) filter, agglevel);
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("aggregate function calls cannot be nested"),
				 parser_errposition(pstate, aggloc)));
	}

	/*
	 * 现在检查直接参数中的变量/聚合，必要时报         
	 * 错。注意我们允许聚合语义层级的 Var，但不允许      
	 * 该层级的 Agg。原则上这类 Agg 或许可以支持，   
	 * 但那会在执行期造成聚合之间的顺序依赖。      
	 * 既然这一情形看起来既非标准所要求，也不          
	 * 特别有用，我们就把它当作嵌套聚合情形来      
	 * 处理。                  
	 */
	if (directargs)
	{
		context.min_varlevel = -1;
		context.min_agglevel = -1;
		(void) check_agg_arguments_walker((Node *) directargs, &context);
		if (context.min_varlevel >= 0 && context.min_varlevel < agglevel)
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("outer-level aggregate cannot contain a lower-level variable in its direct arguments"),
					 parser_errposition(pstate,
										locate_var_of_level((Node *) directargs,
															context.min_varlevel))));
		if (context.min_agglevel >= 0 && context.min_agglevel <= agglevel)
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("aggregate function calls cannot be nested"),
					 parser_errposition(pstate,
										locate_agg_of_level((Node *) directargs,
															context.min_agglevel))));
	}
	return agglevel;
}

static bool
check_agg_arguments_walker(Node *node,
						   check_agg_arguments_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		int			varlevelsup = ((Var *) node)->varlevelsup;

		/* 把 levelsup 换算回最初查询的参照系           */
		varlevelsup -= context->sublevels_up;
		/* 忽略子查询的局部变量  */
		if (varlevelsup >= 0)
		{
			if (context->min_varlevel < 0 ||
				context->min_varlevel > varlevelsup)
				context->min_varlevel = varlevelsup;
		}
		return false;
	}
	if (IsA(node, Aggref))
	{
		int			agglevelsup = ((Aggref *) node)->agglevelsup;

		/* 把 levelsup 换算回最初查询的参照系           */
		agglevelsup -= context->sublevels_up;
		/* 忽略子查询的局部聚合  */
		if (agglevelsup >= 0)
		{
			if (context->min_agglevel < 0 ||
				context->min_agglevel > agglevelsup)
				context->min_agglevel = agglevelsup;
		}
		/* 继续并下探子树             */
	}
	if (IsA(node, GroupingFunc))
	{
		int			agglevelsup = ((GroupingFunc *) node)->agglevelsup;

		/* 把 levelsup 换算回最初查询的参照系           */
		agglevelsup -= context->sublevels_up;
		/* 忽略子查询的局部聚合  */
		if (agglevelsup >= 0)
		{
			if (context->min_agglevel < 0 ||
				context->min_agglevel > agglevelsup)
				context->min_agglevel = agglevelsup;
		}
		/* 继续并下探子树             */
	}

	/*
	 * SRF 和窗口函数可立即拒绝，除非我们正处在          
	 * 聚合参数内部的子选择中；那种情况下它们         
	 * 可以。  
	 */
	if (context->sublevels_up == 0)
	{
		if ((IsA(node, FuncExpr) && ((FuncExpr *) node)->funcretset) ||
			(IsA(node, OpExpr) && ((OpExpr *) node)->opretset))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("aggregate function calls cannot contain set-returning function calls"),
					 errhint("You might be able to move the set-returning function into a LATERAL FROM item."),
					 parser_errposition(context->pstate, exprLocation(node))));
		if (IsA(node, WindowFunc))
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("aggregate function calls cannot contain window function calls"),
					 parser_errposition(context->pstate,
										((WindowFunc *) node)->location)));
	}

	if (IsA(node, RangeTblEntry))
	{
		/*
		 * CTE 引用的行为类似 CTE 层级的变量。若无此        
		 * 处理，我们可能误判 Agg 能在 CTE 之上求值，         
		 * 进而出问题。 
		 */
		RangeTblEntry *rte = (RangeTblEntry *) node;

		if (rte->rtekind == RTE_CTE)
		{
			int			ctelevelsup = rte->ctelevelsup;

			/* 把 levelsup 换算回最初查询的参照系           */
			ctelevelsup -= context->sublevels_up;
			/* 忽略子查询的局部 CTE    */
			if (ctelevelsup >= 0)
			{
				if (context->min_varlevel < 0 ||
					context->min_varlevel > ctelevelsup)
					context->min_varlevel = ctelevelsup;
			}
		}
		return false;			/* 允许 range_table_walker 继续     */
	}
	if (IsA(node, Query))
	{
		/* 递归进入子选择   */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   check_agg_arguments_walker,
								   (void *) context,
								   QTW_EXAMINE_RTES_BEFORE);
		context->sublevels_up--;
		return result;
	}

	return expression_tree_walker(node,
								  check_agg_arguments_walker,
								  (void *) context);
}

/*
 * transformWindowFuncCall -
 *		完成窗口函数调用的初始转换                
 *
 * parse_func.c 已认定该函数为窗口函数，并已设置             
 * 好 WindowFunc 除 winref 外的全部字段。这里必须             
 * (1) 把 WindowDef 加入 pstate（若非已有窗口的                    
 * 副本）并设置 winref 链接到它；(2) 在 pstate 里把             
 * p_hasWindowFuncs 置真。与聚合不同，只需考虑            
 * 最贴近的 pstate 层级 --- SQL 并无"外层窗口函数"。  
 */
void
transformWindowFuncCall(ParseState *pstate, WindowFunc *wfunc,
						WindowDef *windef)
{
	const char *err;
	bool		errkind;

	/*
	 * 窗口函数调用不能再包含窗口函数调用（但聚           
	 * 合可以）。XXX：这是标准要求，还是未实现  
	 *
	 * 注：这里无需检查过滤表达式，因为下文与           
	 * transformAggregateCall 中所做的上下文检查                
	 * 早已拒绝过滤中的任何窗口函数或聚合。      
	 */
	if (pstate->p_hasWindowFuncs &&
		contain_windowfuncs((Node *) wfunc->args))
		ereport(ERROR,
				(errcode(ERRCODE_WINDOWING_ERROR),
				 errmsg("window function calls cannot be nested"),
				 parser_errposition(pstate,
									locate_windowfunc((Node *) wfunc->args))));

	/*
	 * 检查窗口函数是否处于查询内的无效位置。            
	 *       
	 *
	 * 为简洁起见，这里支持两种报错方式：把 "err"       
	 * 设为自定义消息；或在错误上下文已能被               
	 * ParseExprKindName 的返回值充分标识，*且*该                  
	 * 返回值只是单个 SQL 关键字。（否则就用自定         
	 * 义消息，以免制造翻译问题。）      
	 */
	err = NULL;
	errkind = false;
	switch (pstate->p_expr_kind)
	{
		case EXPR_KIND_NONE:
			Assert(false);		/* 不会发生 */
			break;
		case EXPR_KIND_OTHER:
			/* 此处接受窗口函数；调用者须按需报错        */
			break;
		case EXPR_KIND_JOIN_ON:
		case EXPR_KIND_JOIN_USING:
			err = _("window functions are not allowed in JOIN conditions");
			break;
		case EXPR_KIND_FROM_SUBSELECT:
			/* 不会到这里，但保险起见抛出错误    */
			errkind = true;
			break;
		case EXPR_KIND_FROM_FUNCTION:
			err = _("window functions are not allowed in functions in FROM");
			break;
		case EXPR_KIND_WHERE:
			errkind = true;
			break;
		case EXPR_KIND_POLICY:
			err = _("window functions are not allowed in policy expressions");
			break;
		case EXPR_KIND_HAVING:
			errkind = true;
			break;
		case EXPR_KIND_FILTER:
			errkind = true;
			break;
		case EXPR_KIND_WINDOW_PARTITION:
		case EXPR_KIND_WINDOW_ORDER:
		case EXPR_KIND_WINDOW_FRAME_RANGE:
		case EXPR_KIND_WINDOW_FRAME_ROWS:
		case EXPR_KIND_WINDOW_FRAME_GROUPS:
			err = _("window functions are not allowed in window definitions");
			break;
		case EXPR_KIND_SELECT_TARGET:
			/* 可  */
			break;
		case EXPR_KIND_INSERT_TARGET:
		case EXPR_KIND_UPDATE_SOURCE:
		case EXPR_KIND_UPDATE_TARGET:
			errkind = true;
			break;
		case EXPR_KIND_MERGE_WHEN:
			err = _("window functions are not allowed in MERGE WHEN conditions");
			break;
		case EXPR_KIND_GROUP_BY:
			errkind = true;
			break;
		case EXPR_KIND_ORDER_BY:
			/* 可  */
			break;
		case EXPR_KIND_DISTINCT_ON:
			/* 可  */
			break;
		case EXPR_KIND_LIMIT:
		case EXPR_KIND_OFFSET:
			errkind = true;
			break;
		case EXPR_KIND_RETURNING:
			errkind = true;
			break;
		case EXPR_KIND_VALUES:
		case EXPR_KIND_VALUES_SINGLE:
			errkind = true;
			break;
		case EXPR_KIND_CHECK_CONSTRAINT:
		case EXPR_KIND_DOMAIN_CHECK:
			err = _("window functions are not allowed in check constraints");
			break;
		case EXPR_KIND_COLUMN_DEFAULT:
		case EXPR_KIND_FUNCTION_DEFAULT:
			err = _("window functions are not allowed in DEFAULT expressions");
			break;
		case EXPR_KIND_INDEX_EXPRESSION:
			err = _("window functions are not allowed in index expressions");
			break;
		case EXPR_KIND_STATS_EXPRESSION:
			err = _("window functions are not allowed in statistics expressions");
			break;
		case EXPR_KIND_INDEX_PREDICATE:
			err = _("window functions are not allowed in index predicates");
			break;
		case EXPR_KIND_ALTER_COL_TRANSFORM:
			err = _("window functions are not allowed in transform expressions");
			break;
		case EXPR_KIND_EXECUTE_PARAMETER:
			err = _("window functions are not allowed in EXECUTE parameters");
			break;
		case EXPR_KIND_TRIGGER_WHEN:
			err = _("window functions are not allowed in trigger WHEN conditions");
			break;
		case EXPR_KIND_PARTITION_BOUND:
			err = _("window functions are not allowed in partition bound");
			break;
		case EXPR_KIND_PARTITION_EXPRESSION:
			err = _("window functions are not allowed in partition key expressions");
			break;
		case EXPR_KIND_CALL_ARGUMENT:
			err = _("window functions are not allowed in CALL arguments");
			break;
		case EXPR_KIND_COPY_WHERE:
			err = _("window functions are not allowed in COPY FROM WHERE conditions");
			break;
		case EXPR_KIND_GENERATED_COLUMN:
			err = _("window functions are not allowed in column generation expressions");
			break;
		case EXPR_KIND_CYCLE_MARK:
			errkind = true;
			break;

			/*
			 * 这里有意不设 default: 分支，这样在新增     
			 * ParseExprKind 而不扩展此 switch 时，编译器会 
			 * 发出警告。若运行时遇到无法识别的值，       
			 * 其行为与 EXPR_KIND_OTHER 相同，                        
			 * 反正也合理。   
			 */
	}
	if (err)
		ereport(ERROR,
				(errcode(ERRCODE_WINDOWING_ERROR),
				 errmsg_internal("%s", err),
				 parser_errposition(pstate, wfunc->location)));
	if (errkind)
		ereport(ERROR,
				(errcode(ERRCODE_WINDOWING_ERROR),
		/* translator: %s is name of a SQL construct, eg GROUP BY */
				 errmsg("window functions are not allowed in %s",
						ParseExprKindName(pstate->p_expr_kind)),
				 parser_errposition(pstate, wfunc->location)));

	/*
	 * 若 OVER 子句只指定了窗口名，就找出那个           
	 * WINDOW 子句（最好确实存在）。否则，尝试匹配         
	 * OVER 子句的全部属性，无匹配则在 p_windowdefs              
	 * 列表加项。 
	 */
	if (windef->name)
	{
		Index		winref = 0;
		ListCell   *lc;

		Assert(windef->refname == NULL &&
			   windef->partitionClause == NIL &&
			   windef->orderClause == NIL &&
			   windef->frameOptions == FRAMEOPTION_DEFAULTS);

		foreach(lc, pstate->p_windowdefs)
		{
			WindowDef  *refwin = (WindowDef *) lfirst(lc);

			winref++;
			if (refwin->name && strcmp(refwin->name, windef->name) == 0)
			{
				wfunc->winref = winref;
				break;
			}
		}
		if (lc == NULL)			/* 没找到？    */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("window \"%s\" does not exist", windef->name),
					 parser_errposition(pstate, windef->location)));
	}
	else
	{
		Index		winref = 0;
		ListCell   *lc;

		foreach(lc, pstate->p_windowdefs)
		{
			WindowDef  *refwin = (WindowDef *) lfirst(lc);

			winref++;
			if (refwin->refname && windef->refname &&
				strcmp(refwin->refname, windef->refname) == 0)
				 /* 凭 refname 匹配 */ ;
			else if (!refwin->refname && !windef->refname)
				 /* 匹配，无 refname*/ ;
			else
				continue;

			/*
			 * 另见 optimize_window_clauses 中类似的去重代码        
			 */
			if (equal(refwin->partitionClause, windef->partitionClause) &&
				equal(refwin->orderClause, windef->orderClause) &&
				refwin->frameOptions == windef->frameOptions &&
				equal(refwin->startOffset, windef->startOffset) &&
				equal(refwin->endOffset, windef->endOffset))
			{
				/* 发现重复的窗口定义            */
				wfunc->winref = winref;
				break;
			}
		}
		if (lc == NULL)			/* 没找到？    */
		{
			pstate->p_windowdefs = lappend(pstate->p_windowdefs, windef);
			wfunc->winref = list_length(pstate->p_windowdefs);
		}
	}

	pstate->p_hasWindowFuncs = true;
}

/*
 * parseCheckAggregates
 *	检查不该出现的聚合以及不当的分组。                
 *	本函数应在目标列表和限定条件定稿之后再调用。     
 *	              
 *
 *	放错位置的聚合如今大多已在 transformAggregateCall         
 *	中检测到，但对递归查询里的聚合检查似乎            
 *	放到一切定稿之后再做更稳妥。无论如何，           
 *	不当分组很难即时察觉，所以还得对查询再             
 *	扫一遍。   
 */
void
parseCheckAggregates(ParseState *pstate, Query *qry)
{
	List	   *gset_common = NIL;
	List	   *groupClauses = NIL;
	List	   *groupClauseCommonVars = NIL;
	bool		have_non_var_grouping;
	List	   *func_grouped_rels = NIL;
	ListCell   *l;
	bool		hasJoinRTEs;
	bool		hasSelfRefRTEs;
	Node	   *clause;

	/* 仅在发现聚合或分组时才应调用本函数           */
	Assert(pstate->p_hasAggs || qry->groupClause || qry->havingQual || qry->groupingSets);

	/*
	 * 若有分组集，则展开它们并求所有集合的交集。       
	 *      
	 */
	if (qry->groupingSets)
	{
		/*
		 * 4096 这一上限是随意选的，只是为了避免             
		 * 病态构造耗尽资源。         
		 */
		List	   *gsets = expand_grouping_sets(qry->groupingSets, qry->groupDistinct, 4096);

		if (!gsets)
			ereport(ERROR,
					(errcode(ERRCODE_STATEMENT_TOO_COMPLEX),
					 errmsg("too many grouping sets present (maximum 4096)"),
					 parser_errposition(pstate,
										qry->groupClause
										? exprLocation((Node *) qry->groupClause)
										: exprLocation((Node *) qry->groupingSets))));

		/*
		 * 交集常常为空，所以先用最小的集合来播种    
		 * 交集运算，好让事情顺利些。     
		 */
		gset_common = linitial(gsets);

		if (gset_common)
		{
			for_each_from(l, gsets, 1)
			{
				gset_common = list_intersection_int(gset_common, lfirst(l));
				if (!gset_common)
					break;
			}
		}

		/*
		 * 若展开后只剩一个分组集，并且 groupClause         
		 * 非空（即该分组集也非空），那我们就可以     
		 * 丢弃分组集，装作原本只是普通 GROUP BY。          
		 *                            
		 */
		if (list_length(gsets) == 1 && qry->groupClause)
			qry->groupingSets = NIL;
	}

	/*
	 * 扫描范围表，看是否存在 JOIN 或自引用 CTE            
	 * 项。下文会用到此信息。    
	 */
	hasJoinRTEs = hasSelfRefRTEs = false;
	foreach(l, pstate->p_rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind == RTE_JOIN)
			hasJoinRTEs = true;
		else if (rte->rtekind == RTE_CTE && rte->self_reference)
			hasSelfRefRTEs = true;
	}

	/*
	 * 构建一份可接受的 GROUP BY 表达式列表，供       
	 * check_ungrouped_columns().
	 *
	 * 我们取的是 TLE 而非仅表达式，因为 GROUPING 要知道   
	 * sortgroupref.
	 */
	foreach(l, qry->groupClause)
	{
		SortGroupClause *grpcl = (SortGroupClause *) lfirst(l);
		TargetEntry *expr;

		expr = get_sortgroupclause_tle(grpcl, qry->targetList);
		if (expr == NULL)
			continue;			/* 多半不会发生     */

		groupClauses = lappend(groupClauses, expr);
	}

	/*
	 * 若涉及连接别名变量，就必须把它们展平为            
	 * 底层变量，这样带别名与不带别名的变量才            
	 * 会被正确视为相等。若没有 RTE_JOIN 类的范围             
	 * 表项，可省开销。  
	 */
	if (hasJoinRTEs)
		groupClauses = (List *) flatten_join_alias_vars(NULL, qry,
														(Node *) groupClauses);

	/*
	 * 检测分组表达式是否都为简单 Var；若全是 Var，       
	 * 后面的递归扫描就不用那么费力。（注意此前        
	 * 必须先展平别名。）                           
	 *
	 * 被所有分组集都包含的 Var 单独记录在             
	 * groupClauseCommonVars 里，因为只有它们能用于           
	 * 函数依赖检查。             
	 */
	have_non_var_grouping = false;
	foreach(l, groupClauses)
	{
		TargetEntry *tle = lfirst(l);

		if (!IsA(tle->expr, Var))
		{
			have_non_var_grouping = true;
		}
		else if (!qry->groupingSets ||
				 list_member_int(gset_common, tle->ressortgroupref))
		{
			groupClauseCommonVars = lappend(groupClauseCommonVars, tle->expr);
		}
	}

	/*
	 * 检查目标列表与 HAVING 子句中的未分组变量。    
	 *
	 * 注：因为我们连 resjunk 目标列表项也一并检查，       
	 * 这也会找出源自 ORDER BY 与 WINDOW 子句的未分组       
	 * 变量。而且它还会检查分组表达式本身 --- 不过  
	 * 它们全都通得过该测试……                                 
	 *
	 * 我们还要定稿 GROUPING 表达式，但为此需要遍历          
	 * 原始（未展平）子句才能修改节点。           
	 */
	clause = (Node *) qry->targetList;
	finalize_grouping_exprs(clause, pstate, qry,
							groupClauses, hasJoinRTEs,
							have_non_var_grouping);
	if (hasJoinRTEs)
		clause = flatten_join_alias_vars(NULL, qry, clause);
	check_ungrouped_columns(clause, pstate, qry,
							groupClauses, groupClauseCommonVars,
							have_non_var_grouping,
							&func_grouped_rels);

	clause = (Node *) qry->havingQual;
	finalize_grouping_exprs(clause, pstate, qry,
							groupClauses, hasJoinRTEs,
							have_non_var_grouping);
	if (hasJoinRTEs)
		clause = flatten_join_alias_vars(NULL, qry, clause);
	check_ungrouped_columns(clause, pstate, qry,
							groupClauses, groupClauseCommonVars,
							have_non_var_grouping,
							&func_grouped_rels);

	/*
	 * 按标准，聚合不能出现在递归项中。      
	 */
	if (pstate->p_hasAggs && hasSelfRefRTEs)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_RECURSION),
				 errmsg("aggregate functions are not allowed in a recursive query's recursive term"),
				 parser_errposition(pstate,
									locate_agg_of_level((Node *) qry, 0))));
}

/*
 * check_ungrouped_columns -
 *	  扫描给定的表达式树，查找未分组变量（即        
 *	  未列入 groupClauses 列表、也不在聚合函数          
 *	  参数内的变量）。一经发现，就发出适当的            
 *	  错误消息。  
 *
 * 注：我们假定给定子句已经过适合解析器输出           
 * 的转换，故可直接用 expression_tree_walker。        
 *
 * 注：我们在主查询中识别分组表达式，但在子       
 * 查询中只识别分组 Var。举例来说，下面这条       
 * 本可放行却遭拒绝：  
 *		SELECT
 *			(SELECT x FROM bar where y = (foo.a + foo.b))
 *		FROM foo
 *		GROUP BY a + b;
 * 难点在于必须考虑不同的 sublevels_up。                
 * 这看起来需要一个完全定制的 equal() 版本，          
 * 其痛苦远超该功能的价值。       
 */
static void
check_ungrouped_columns(Node *node, ParseState *pstate, Query *qry,
						List *groupClauses, List *groupClauseCommonVars,
						bool have_non_var_grouping,
						List **func_grouped_rels)
{
	check_ungrouped_columns_context context;

	context.pstate = pstate;
	context.qry = qry;
	context.hasJoinRTEs = false;	/* 调用者应已展平连接变量 */
	context.groupClauses = groupClauses;
	context.groupClauseCommonVars = groupClauseCommonVars;
	context.have_non_var_grouping = have_non_var_grouping;
	context.func_grouped_rels = func_grouped_rels;
	context.sublevels_up = 0;
	context.in_agg_direct_args = false;
	check_ungrouped_columns_walker(node, &context);
}

static bool
check_ungrouped_columns_walker(Node *node,
							   check_ungrouped_columns_context *context)
{
	ListCell   *gl;

	if (node == NULL)
		return false;
	if (IsA(node, Const) ||
		IsA(node, Param))
		return false;			/* 常量总是可接受           */

	if (IsA(node, Aggref))
	{
		Aggref	   *agg = (Aggref *) node;

		if ((int) agg->agglevelsup == context->sublevels_up)
		{
			/*
			 * 若发现属于原始层级的聚合调用，就不要    
			 * 再下探其普通参数、ORDER BY 参数或过滤      
			 * 条件；其中的未分组变量不算错误。但应当    
			 * 把直接参数当作不在聚合内来检查。              
			 * 我们会在上下文里设一个特殊标志，以便为    
			 * 直接参数内未分组变量给出有用报错。  
			 */
			bool		result;

			Assert(!context->in_agg_direct_args);
			context->in_agg_direct_args = true;
			result = check_ungrouped_columns_walker((Node *) agg->aggdirectargs,
													context);
			context->in_agg_direct_args = false;
			return result;
		}

		/*
		 * 更高层级的聚合可以完全跳过不下探，               
		 * 因为它们绝不可能含有我们关心的 Var（参见      
		 * transformAggregateCall）。但更低层级的聚合               
		 * 仍须查看。 
		 */
		if ((int) agg->agglevelsup > context->sublevels_up)
			return false;
	}

	if (IsA(node, GroupingFunc))
	{
		GroupingFunc *grp = (GroupingFunc *) node;

		/* GroupingFunc 已单独处理，无需在本层复查              */

		if ((int) grp->agglevelsup >= context->sublevels_up)
			return false;
	}

	/*
	 * 若存在非简单 Var 的 GROUP BY 项，就检查该                 
	 * 子表达式整体是否匹配任何 GROUP BY 项。这一检查必须
	 * 在每层递归都做，以便在进入其中的变量之前就     
	 * 认出按 GROUP BY 分组的表达式；不过仅在外层查询层级 
	 * 有效。如前所述。    
	 */
	if (context->have_non_var_grouping && context->sublevels_up == 0)
	{
		foreach(gl, context->groupClauses)
		{
			TargetEntry *tle = lfirst(gl);

			if (equal(node, tle->expr))
				return false;	/* 可接受，不再下探        */
		}
	}

	/*
	 * 若发现原始查询层级的未分组 Var，即告失败。     
	 * 低于原始查询层级的变量不成问题，来自其           
	 * 上方的变量同样无碍。（若这类变量在其自身          
	 * 查询层级看来未分组，那是别人的问题……）         
	 */
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		RangeTblEntry *rte;
		char	   *attname;

		if (var->varlevelsup != context->sublevels_up)
			return false;		/* 非本查询局部变量，忽略  */

		/*
		 * 若上文未做，则检查匹配。        
		 */
		if (!context->have_non_var_grouping || context->sublevels_up != 0)
		{
			foreach(gl, context->groupClauses)
			{
				Var		   *gvar = (Var *) ((TargetEntry *) lfirst(gl))->expr;

				if (IsA(gvar, Var) &&
					gvar->varno == var->varno &&
					gvar->varattno == var->varattno &&
					gvar->varlevelsup == 0)
					return false;	/* 可接受，没问题  */
			}
		}

		/*
		 * 检查 Var 是否已知函数依赖于 GROUP                      
		 * BY 列。若是，则允许使用该 Var，因为对该表      
		 * 的分组实为空操作。不过此推断                         
		 * 依赖于该表的一个或多个约束，所以必须把这些   
		 * 约束加入查询的 constraintDeps 列表，因为一旦约束  
		 * 被删除，查询在语义上便不再有效。                
		 * （因此该检查必须作为报错前的最后一招：          
		 * 我们不希望非必要地添加依赖。）           
		 *
		 * 由于此检查开销较大，且对表各列结果相同，    
		 * 所以我们在 func_grouped_rels 列表中记住哪些 RTE     
		 * 已证明存在函数依赖。这一测试也防止我们向     
		 * constraintDeps 列表添加重复条目。                        
		 *                         
		 */
		if (list_member_int(*context->func_grouped_rels, var->varno))
			return false;		/* 此前已证明可接受     */

		Assert(var->varno > 0 &&
			   (int) var->varno <= list_length(context->pstate->p_rtable));
		rte = rt_fetch(var->varno, context->pstate->p_rtable);
		if (rte->rtekind == RTE_RELATION)
		{
			if (check_functional_grouping(rte->relid,
										  var->varno,
										  0,
										  context->groupClauseCommonVars,
										  &context->qry->constraintDeps))
			{
				*context->func_grouped_rels =
					lappend_int(*context->func_grouped_rels, var->varno);
				return false;	/* 可接受  */
			}
		}

		/* 发现未分组的局部变量；生成错误消息       */
		attname = get_rte_attribute_name(rte, var->varattno);
		if (context->sublevels_up == 0)
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("column \"%s.%s\" must appear in the GROUP BY clause or be used in an aggregate function",
							rte->eref->aliasname, attname),
					 context->in_agg_direct_args ?
					 errdetail("Direct arguments of an ordered-set aggregate must use only grouped columns.") : 0,
					 parser_errposition(context->pstate, var->location)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("subquery uses ungrouped column \"%s.%s\" from outer query",
							rte->eref->aliasname, attname),
					 parser_errposition(context->pstate, var->location)));
	}

	if (IsA(node, Query))
	{
		/* 递归处理子查询   */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   check_ungrouped_columns_walker,
								   (void *) context,
								   0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, check_ungrouped_columns_walker,
								  (void *) context);
}

/*
 * finalize_grouping_exprs -
 *	  扫描给定表达式树，寻找 GROUPING() 及相关调用， 
 *	  并对其参数进行验证和处理。  
 *
 * 本函数从 check_ungrouped_columns 中拆出，因为它需要     
 * 就地修改节点（不经由 mutator）；而由于连接别名    
 * 变量被扁平化，check_ungrouped_columns 可能只见副本，   
 * 因此这里在比较之前，会先对所见的每个 GROUPING   
 * 参数逐个扁平化，然后再进行比较。   
 */
static void
finalize_grouping_exprs(Node *node, ParseState *pstate, Query *qry,
						List *groupClauses, bool hasJoinRTEs,
						bool have_non_var_grouping)
{
	check_ungrouped_columns_context context;

	context.pstate = pstate;
	context.qry = qry;
	context.hasJoinRTEs = hasJoinRTEs;
	context.groupClauses = groupClauses;
	context.groupClauseCommonVars = NIL;
	context.have_non_var_grouping = have_non_var_grouping;
	context.func_grouped_rels = NULL;
	context.sublevels_up = 0;
	context.in_agg_direct_args = false;
	finalize_grouping_exprs_walker(node, &context);
}

static bool
finalize_grouping_exprs_walker(Node *node,
							   check_ungrouped_columns_context *context)
{
	ListCell   *gl;

	if (node == NULL)
		return false;
	if (IsA(node, Const) ||
		IsA(node, Param))
		return false;			/* 常量总是可接受的        */

	if (IsA(node, Aggref))
	{
		Aggref	   *agg = (Aggref *) node;

		if ((int) agg->agglevelsup == context->sublevels_up)
		{
			/*
			 * 若遇到本层级的聚合调用，则不要             
			 * 递归进入其普通参数、ORDER BY 参数或         
			 * filter；该处不允许本层级的 GROUPING 表达式。但  
			 * 需检查直接参数，如同其不在聚合内一样。     
			 */
			bool		result;

			Assert(!context->in_agg_direct_args);
			context->in_agg_direct_args = true;
			result = finalize_grouping_exprs_walker((Node *) agg->aggdirectargs,
													context);
			context->in_agg_direct_args = false;
			return result;
		}

		/*
		 * 更高层级的聚合可以完全跳过递归，                  
		 * 因为其中不可能包含我们所关心的表达式（见     
		 * transformAggregateCall）。但较低层级的聚合               
		 * 仍需检查。 
		 */
		if ((int) agg->agglevelsup > context->sublevels_up)
			return false;
	}

	if (IsA(node, GroupingFunc))
	{
		GroupingFunc *grp = (GroupingFunc *) node;

		/*
		 * 我们只需在 GroupingFunc 节点所属的确切层级      
		 * 上检查它们，因为其参数无法混合层级。       
		 */

		if ((int) grp->agglevelsup == context->sublevels_up)
		{
			ListCell   *lc;
			List	   *ref_list = NIL;

			foreach(lc, grp->args)
			{
				Node	   *expr = lfirst(lc);
				Index		ref = 0;

				if (context->hasJoinRTEs)
					expr = flatten_join_alias_vars(NULL, context->qry, expr);

				/*
				 * 每个表达式都必须匹配处于当前查询层级    
				 * 的某个分组项。与普通表达式不同，这里   
				 * 不允许函数依赖或外层引用。           
				 */

				if (IsA(expr, Var))
				{
					Var		   *var = (Var *) expr;

					if (var->varlevelsup == context->sublevels_up)
					{
						foreach(gl, context->groupClauses)
						{
							TargetEntry *tle = lfirst(gl);
							Var		   *gvar = (Var *) tle->expr;

							if (IsA(gvar, Var) &&
								gvar->varno == var->varno &&
								gvar->varattno == var->varattno &&
								gvar->varlevelsup == 0)
							{
								ref = tle->ressortgroupref;
								break;
							}
						}
					}
				}
				else if (context->have_non_var_grouping &&
						 context->sublevels_up == 0)
				{
					foreach(gl, context->groupClauses)
					{
						TargetEntry *tle = lfirst(gl);

						if (equal(expr, tle->expr))
						{
							ref = tle->ressortgroupref;
							break;
						}
					}
				}

				if (ref == 0)
					ereport(ERROR,
							(errcode(ERRCODE_GROUPING_ERROR),
							 errmsg("arguments to GROUPING must be grouping expressions of the associated query level"),
							 parser_errposition(context->pstate,
												exprLocation(expr))));

				ref_list = lappend_int(ref_list, ref);
			}

			grp->refs = ref_list;
		}

		if ((int) grp->agglevelsup > context->sublevels_up)
			return false;
	}

	if (IsA(node, Query))
	{
		/* 递归处理子查询   */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   finalize_grouping_exprs_walker,
								   (void *) context,
								   0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, finalize_grouping_exprs_walker,
								  (void *) context);
}


/*
 * 给定 GroupingSet 节点，展开并返回一组列表。     
 *
 * 对 EMPTY 节点返回只含空列表的列表。
 *
 * 对 SIMPLE 节点，返回只含该节点内容的列表。            
 *
 * 对 CUBE、ROLLUP 节点，返回各展开结果列表。  
 *
 * 对 SET 节点，递归展开所含的 CUBE 和 ROLLUP。   
 */
static List *
expand_groupingset_node(GroupingSet *gs)
{
	List	   *result = NIL;

	switch (gs->kind)
	{
		case GROUPING_SET_EMPTY:
			result = list_make1(NIL);
			break;

		case GROUPING_SET_SIMPLE:
			result = list_make1(gs->content);
			break;

		case GROUPING_SET_ROLLUP:
			{
				List	   *rollup_val = gs->content;
				ListCell   *lc;
				int			curgroup_size = list_length(gs->content);

				while (curgroup_size > 0)
				{
					List	   *current_result = NIL;
					int			i = curgroup_size;

					foreach(lc, rollup_val)
					{
						GroupingSet *gs_current = (GroupingSet *) lfirst(lc);

						Assert(gs_current->kind == GROUPING_SET_SIMPLE);

						current_result = list_concat(current_result,
													 gs_current->content);

						/* 若当前分组已构造完毕，则跳出循环    */
						if (--i == 0)
							break;
					}

					result = lappend(result, current_result);
					--curgroup_size;
				}

				result = lappend(result, NIL);
			}
			break;

		case GROUPING_SET_CUBE:
			{
				List	   *cube_list = gs->content;
				int			number_bits = list_length(cube_list);
				uint32		num_sets;
				uint32		i;

				/* 解析器应将上限设得更低 */
				Assert(number_bits < 31);

				num_sets = (1U << number_bits);

				for (i = 0; i < num_sets; i++)
				{
					List	   *current_result = NIL;
					ListCell   *lc;
					uint32		mask = 1U;

					foreach(lc, cube_list)
					{
						GroupingSet *gs_current = (GroupingSet *) lfirst(lc);

						Assert(gs_current->kind == GROUPING_SET_SIMPLE);

						if (mask & i)
							current_result = list_concat(current_result,
														 gs_current->content);

						mask <<= 1;
					}

					result = lappend(result, current_result);
				}
			}
			break;

		case GROUPING_SET_SETS:
			{
				ListCell   *lc;

				foreach(lc, gs->content)
				{
					List	   *current_result = expand_groupingset_node(lfirst(lc));

					result = list_concat(result, current_result);
				}
			}
			break;
	}

	return result;
}

/* list_sort 比较函数，按长度排序子列表*/
static int
cmp_list_len_asc(const ListCell *a, const ListCell *b)
{
	int			la = list_length((const List *) lfirst(a));
	int			lb = list_length((const List *) lfirst(b));

	return (la > lb) ? 1 : (la == lb) ? 0 : -1;
}

/* list_sort 比较函数，按长度和内容排序子列表    */
static int
cmp_list_len_contents_asc(const ListCell *a, const ListCell *b)
{
	int			res = cmp_list_len_asc(a, b);

	if (res == 0)
	{
		List	   *la = (List *) lfirst(a);
		List	   *lb = (List *) lfirst(b);
		ListCell   *lca;
		ListCell   *lcb;

		forboth(lca, la, lcb, lb)
		{
			int			va = lfirst_int(lca);
			int			vb = lfirst_int(lcb);

			if (va > vb)
				return 1;
			if (va < vb)
				return -1;
		}
	}

	return res;
}

/*
 * 把 groupingSets 子句展开为扁平的分组集列表。  
 * 返回列表按长度排序，最短的集在前。        
 *
 * 这主要供规划器使用，但这里也会用它做      
 * 一些一致性检查。
 */
List *
expand_grouping_sets(List *groupingSets, bool groupDistinct, int limit)
{
	List	   *expanded_groups = NIL;
	List	   *result = NIL;
	double		numsets = 1;
	ListCell   *lc;

	if (groupingSets == NIL)
		return NIL;

	foreach(lc, groupingSets)
	{
		List	   *current_result = NIL;
		GroupingSet *gs = lfirst(lc);

		current_result = expand_groupingset_node(gs);

		Assert(current_result != NIL);

		numsets *= list_length(current_result);

		if (limit >= 0 && numsets > limit)
			return NIL;

		expanded_groups = lappend(expanded_groups, current_result);
	}

	/*
	 * 对 expanded_groups 的各子列表做笛卡尔积。同时，        
	 * 从各个分组集中移除重复元素（但绝不能              
	 * 改变分组集的数量）。       
	 */

	foreach(lc, (List *) linitial(expanded_groups))
	{
		result = lappend(result, list_union_int(NIL, (List *) lfirst(lc)));
	}

	for_each_from(lc, expanded_groups, 1)
	{
		List	   *p = lfirst(lc);
		List	   *new_result = NIL;
		ListCell   *lc2;

		foreach(lc2, result)
		{
			List	   *q = lfirst(lc2);
			ListCell   *lc3;

			foreach(lc3, p)
			{
				new_result = lappend(new_result,
									 list_union_int(q, (List *) lfirst(lc3)));
			}
		}
		result = new_result;
	}

	/* 现在按长度对列表排序，并视需要去重       */
	if (!groupDistinct || list_length(result) < 2)
		list_sort(result, cmp_list_len_asc);
	else
	{
		ListCell   *cell;
		List	   *prev;

		/* 对每个分组集单独排序  */
		foreach(cell, result)
			list_sort(lfirst(cell), list_int_cmp);

		/* 现在按长度和内容对分组集列表排序      */
		list_sort(result, cmp_list_len_contents_asc);

		/* 最后，去除重复项   */
		prev = linitial(result);
		for_each_from(cell, result, 1)
		{
			if (equal(lfirst(cell), prev))
				result = foreach_delete_current(result, cell);
			else
				prev = lfirst(cell);
		}
	}

	return result;
}

/*
 * get_aggregate_argtypes
 *	确定传递给聚合调用的具体数据类型。         
 *
 * 给定 Aggref，提取输入参数的实际数据类型。           
 * 报告输入数据类型的方式需与聚合声明相匹配，   
 * 即附加在普通聚合上的 ORDER BY 列会被忽略，           
 * 但有序集聚合的直接参数与聚合参数都会报告。        
 *                          
 *
 * 数据类型会写入 inputTypes[]，该数组长度必须             
 * 为 FUNC_MAX_ARGS。    
 *
 * 函数返回值为实际参数的个数。            
 */
int
get_aggregate_argtypes(Aggref *aggref, Oid *inputTypes)
{
	int			numArguments = 0;
	ListCell   *lc;

	Assert(list_length(aggref->aggargtypes) <= FUNC_MAX_ARGS);

	foreach(lc, aggref->aggargtypes)
	{
		inputTypes[numArguments++] = lfirst_oid(lc);
	}

	return numArguments;
}

/*
 * resolve_aggregate_transtype
 *	确定聚合调用的转换状态值的数据类型。               
 *
 * 此函数解析多态聚合的状态数据类型。             
 * 调用时必须传入聚合系统目录项中的 aggtranstype，            
 * 以及 get_aggregate_argtypes 提取出的实际参数类型。          
 * （本可以在内部读取 pg_aggregate.aggtranstype，但           
 * 现有调用者都已持有该值，故要求其传入。）     
 */
Oid
resolve_aggregate_transtype(Oid aggfuncid,
							Oid aggtranstype,
							Oid *inputTypes,
							int numArguments)
{
	/* 若为多态，则解析转换状态的实际类型     */
	if (IsPolymorphicType(aggtranstype))
	{
		/* 必须获取聚合声明的输入类型……   */
		Oid		   *declaredArgTypes;
		int			agg_nargs;

		(void) get_func_signature(aggfuncid, &declaredArgTypes, &agg_nargs);

		/*
		 * VARIADIC ANY 聚合的实际参数可能多于声明参数，但
		 * 这些多余参数不会影响多态类型解析。      
		 */
		Assert(agg_nargs <= numArguments);

		aggtranstype = enforce_generic_type_consistency(inputTypes,
														declaredArgTypes,
														agg_nargs,
														aggtranstype,
														false);
		pfree(declaredArgTypes);
	}
	return aggtranstype;
}

/*
 * agg_args_support_sendreceive
 *		若 aggref 参数的非 byval 类型都有发送和接收函数，
 *		则返回真。   
 */
bool
agg_args_support_sendreceive(Aggref *aggref)
{
	ListCell   *lc;

	foreach(lc, aggref->args)
	{
		HeapTuple	typeTuple;
		Form_pg_type pt;
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Oid			type = exprType((Node *) tle->expr);

		/*
		 * RECORD 是特例：它有 typsend/typreceive 函数，但         
		 * record_recv 只有在传入正确 typmod 以标识具体匿名     
		 * 记录类型时才能工作。array_agg_deserialize 无法      
		 * 做到，因此必须声明不支持该情形。  
		 */
		if (type == RECORDOID)
			return false;

		typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
		if (!HeapTupleIsValid(typeTuple))
			elog(ERROR, "cache lookup failed for type %u", type);

		pt = (Form_pg_type) GETSTRUCT(typeTuple);

		if (!pt->typbyval &&
			(!OidIsValid(pt->typsend) || !OidIsValid(pt->typreceive)))
		{
			ReleaseSysCache(typeTuple);
			return false;
		}
		ReleaseSysCache(typeTuple);
	}
	return true;
}

/*
 * 为聚合的转换函数创建表达式树。                         
 * 这样做是为了让多态函数可在聚合内使用，         
 * 因为没有表达式树，这类函数将无从得知其应            
 * 使用的数据类型。（这些树实际上绝不会                 
 * 被执行，因此可以在正确性上稍有省略。）   
 *
 * agg_input_types 与 agg_state_type 标识聚合的输入            
 * 类型。这些应被解析为具体类型（不应出现            
 * ANYELEMENT 等）。    
 * agg_input_collation 是聚合函数的输入排序规则。     
 *
 * 对有序集聚合，请记住 agg_input_types 描述的是          
 * 直接参数后跟聚合参数。                         
 *
 * transfn_oid 与 invtransfn_oid 标识待调用的函数；         
 * 后者可为 InvalidOid；但若设置了 invtransfn_oid，    
 * transfn_oid 也须设置。  
 *
 * 当 *transfnexpr 用于合并聚合阶段时，transfn_oid 也可          
 * 作为 aggcombinefn 传入。此时 invtransfn_oid 应为                
 * InvalidOid，因为不存在逆向合并函数。                 
 *           
 *
 * 构建好的树的指针会返回到 *transfnexpr 与            
 * *invtransfnexpr。若不存在 invtransfn，则相应指针              
 * 将被设为 NULL。invtransfn 是可选的，因此也允许            
 * 传 NULL。    
 */
void
build_aggregate_transfn_expr(Oid *agg_input_types,
							 int agg_num_inputs,
							 int agg_num_direct_inputs,
							 bool agg_variadic,
							 Oid agg_state_type,
							 Oid agg_input_collation,
							 Oid transfn_oid,
							 Oid invtransfn_oid,
							 Expr **transfnexpr,
							 Expr **invtransfnexpr)
{
	List	   *args;
	FuncExpr   *fexpr;
	int			i;

	/*
	 * 为 transfn FuncExpr 节点构建参数列表。   
	 */
	args = list_make1(make_agg_arg(agg_state_type, agg_input_collation));

	for (i = agg_num_direct_inputs; i < agg_num_inputs; i++)
	{
		args = lappend(args,
					   make_agg_arg(agg_input_types[i], agg_input_collation));
	}

	fexpr = makeFuncExpr(transfn_oid,
						 agg_state_type,
						 args,
						 InvalidOid,
						 agg_input_collation,
						 COERCE_EXPLICIT_CALL);
	fexpr->funcvariadic = agg_variadic;
	*transfnexpr = (Expr *) fexpr;

	/*
	 * 如需则构建 invtransfn 表达式，参数与 transfn 相同。 
	 */
	if (invtransfnexpr != NULL)
	{
		if (OidIsValid(invtransfn_oid))
		{
			fexpr = makeFuncExpr(invtransfn_oid,
								 agg_state_type,
								 args,
								 InvalidOid,
								 agg_input_collation,
								 COERCE_EXPLICIT_CALL);
			fexpr->funcvariadic = agg_variadic;
			*invtransfnexpr = (Expr *) fexpr;
		}
		else
			*invtransfnexpr = NULL;
	}
}

/*
 * 与 build_aggregate_transfn_expr 类似，但为聚合的                
 * 序列化函数创建表达式树。   
 */
void
build_aggregate_serialfn_expr(Oid serialfn_oid,
							  Expr **serialfnexpr)
{
	List	   *args;
	FuncExpr   *fexpr;

	/* serialfn 的参数恒为 INTERNAL，返回 BYTEA */
	args = list_make1(make_agg_arg(INTERNALOID, InvalidOid));

	fexpr = makeFuncExpr(serialfn_oid,
						 BYTEAOID,
						 args,
						 InvalidOid,
						 InvalidOid,
						 COERCE_EXPLICIT_CALL);
	*serialfnexpr = (Expr *) fexpr;
}

/*
 * 与 build_aggregate_transfn_expr 类似，但为聚合的                
 * 反序列化函数创建表达式树。  
 */
void
build_aggregate_deserialfn_expr(Oid deserialfn_oid,
								Expr **deserialfnexpr)
{
	List	   *args;
	FuncExpr   *fexpr;

	/* deserialfn 参数恒为 BYTEA 与 INTERNAL，返回 INTERNAL */
	args = list_make2(make_agg_arg(BYTEAOID, InvalidOid),
					  make_agg_arg(INTERNALOID, InvalidOid));

	fexpr = makeFuncExpr(deserialfn_oid,
						 INTERNALOID,
						 args,
						 InvalidOid,
						 InvalidOid,
						 COERCE_EXPLICIT_CALL);
	*deserialfnexpr = (Expr *) fexpr;
}

/*
 * 与 build_aggregate_transfn_expr 类似，但为聚合的                
 * 最终函数（而非转换函数）创建表达式树。           
 */
void
build_aggregate_finalfn_expr(Oid *agg_input_types,
							 int num_finalfn_inputs,
							 Oid agg_state_type,
							 Oid agg_result_type,
							 Oid agg_input_collation,
							 Oid finalfn_oid,
							 Expr **finalfnexpr)
{
	List	   *args;
	int			i;

	/*
	 * 为最终函数构建表达式树 
	 */
	args = list_make1(make_agg_arg(agg_state_type, agg_input_collation));

	/* finalfn 可带额外参数，其与聚合输入类型匹配     */
	for (i = 0; i < num_finalfn_inputs - 1; i++)
	{
		args = lappend(args,
					   make_agg_arg(agg_input_types[i], agg_input_collation));
	}

	*finalfnexpr = (Expr *) makeFuncExpr(finalfn_oid,
										 agg_result_type,
										 args,
										 InvalidOid,
										 agg_input_collation,
										 COERCE_EXPLICIT_CALL);
	/* 目前从不将 finalfn 按 variadic 处理    */
}

/*
 * 为聚合构建虚设参数表达式的便捷函数。                  
 *
 * 我们真正关心的只是聚合支持函数能在运行时           
 * 通过 get_fn_expr_argtype() 获知实际参数类型，因此            
 * 使用不对应任何真实 Param 的 Param 节点。      
 */
static Node *
make_agg_arg(Oid argtype, Oid argcollation)
{
	Param	   *argp = makeNode(Param);

	argp->paramkind = PARAM_EXEC;
	argp->paramid = -1;
	argp->paramtype = argtype;
	argp->paramtypmod = -1;
	argp->paramcollid = argcollation;
	argp->location = -1;
	return (Node *) argp;
}
