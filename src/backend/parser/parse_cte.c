/*-------------------------------------------------------------------------
 *
 * parse_cte.c
 *	  解析器中处理 CTE（公共表表达式）  
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_cte.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_cte.h"
#include "parser/parse_expr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"


/* 枚举不允许自引用出现的上下文                      */
typedef enum
{
	RECURSION_OK,
	RECURSION_NONRECURSIVETERM, /* 位于左侧项内        */
	RECURSION_SUBLINK,			/* 子链接内     */
	RECURSION_OUTERJOIN,		/* 位于外连接的可空一侧        */
	RECURSION_INTERSECT,		/* 位于 INTERSECT (ALL) 下 */
	RECURSION_EXCEPT			/* 位于 EXCEPT (ALL) 下 */
} RecursionContext;

/* 关联的错误消息 --- 每条须含一个 %s 代表 CTE 名   */
static const char *const recursion_errormsgs[] = {
	/* RECURSION_OK */
	NULL,
	/* RECURSION_NONRECURSIVETERM */
	gettext_noop("recursive reference to query \"%s\" must not appear within its non-recursive term"),
	/* RECURSION_SUBLINK */
	gettext_noop("recursive reference to query \"%s\" must not appear within a subquery"),
	/* RECURSION_OUTERJOIN */
	gettext_noop("recursive reference to query \"%s\" must not appear within an outer join"),
	/* RECURSION_INTERSECT */
	gettext_noop("recursive reference to query \"%s\" must not appear within INTERSECT"),
	/* RECURSION_EXCEPT */
	gettext_noop("recursive reference to query \"%s\" must not appear within EXCEPT")
};

/*
 * 对于 WITH RECURSIVE，必须找出子句成员的一种            
 * 无前向引用的排列顺序，并判定哪些成员是递归的   
 * （即自引用）。用 CteItem 数组来做这件事，比用     
 * CommonTableExpr 列表更方便                   
 */
typedef struct CteItem
{
	CommonTableExpr *cte;		/* 待检查的 CTE   */
	int			id;				/* 依赖项的 ID 编号         */
	Bitmapset  *depends_on;		/* 所依赖的 CTE（不含自身）    */
} CteItem;

/* CteState 是树遍历过程中需要传递的状态         */
typedef struct CteState
{
	/* 全局状态  */
	ParseState *pstate;			/* 全局解析状态 */
	CteItem    *items;			/* CTE 数组及附加数据    */
	int			numitems;		/* CTE 的数量  */
	/* 树遍历期间的工作状态： */
	int			curitem;		/* 当前正被检查的条目的下标   */
	List	   *innerwiths;		/* CommonTableExpr 列表的列表  */
	/* 仅供 checkWellFormedRecursion 遍历用的状态： */
	int			selfrefcount;	/* 检测到的自引用数量        */
	RecursionContext context;	/* 允许或禁止自引用的上下文  */
} CteState;


static void analyzeCTE(ParseState *pstate, CommonTableExpr *cte);

/* 依赖处理函数              */
static void makeDependencyGraph(CteState *cstate);
static bool makeDependencyGraphWalker(Node *node, CteState *cstate);
static void WalkInnerWith(Node *stmt, WithClause *withClause, CteState *cstate);
static void TopologicalSort(ParseState *pstate, CteItem *items, int numitems);

/* 递归有效性检查函数          */
static void checkWellFormedRecursion(CteState *cstate);
static bool checkWellFormedRecursionWalker(Node *node, CteState *cstate);
static void checkWellFormedSelectStmt(SelectStmt *stmt, CteState *cstate);


/*
 * transformWithClause -
 *	  把 WITH 子句的"公共表表达式"列表转换为 Query      
 *	  节点。   
 *
 * 结果是转换后的 CTE 列表，将被放入输出               
 * Query 中。（实际上这与 p_ctenamespace 的最终值相同，     
 * 但不把它暴露在函数 API 中似乎更干净。）       
 */
List *
transformWithClause(ParseState *pstate, WithClause *withClause)
{
	ListCell   *lc;

	/* 每查询层级仅一个 WITH 子句 */
	Assert(pstate->p_ctenamespace == NIL);
	Assert(pstate->p_future_ctes == NIL);

	/*
	 * 对于两种 WITH，列表中都不允许出现重复的             
	 * CTE 名称。立刻做此检查，以免日后操心。
	 *
	 * 另外，先把每个 CTE 暂标记为非递归，并将其         
	 * 引用计数置零，并按需设置 pstate->p_hasModifyingCTE。    
	 */
	foreach(lc, withClause->ctes)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);
		ListCell   *rest;

		/* 解析器允许 MERGE，但尚未实现。暂时拒绝        */
		if (IsA(cte->ctequery, MergeStmt))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("MERGE not supported in WITH query"),
					parser_errposition(pstate, cte->location));

		for_each_cell(rest, withClause->ctes, lnext(withClause->ctes, lc))
		{
			CommonTableExpr *cte2 = (CommonTableExpr *) lfirst(rest);

			if (strcmp(cte->ctename, cte2->ctename) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_ALIAS),
						 errmsg("WITH query name \"%s\" specified more than once",
								cte2->ctename),
						 parser_errposition(pstate, cte2->location)));
		}

		cte->cterecursive = false;
		cte->cterefcount = 0;

		if (!IsA(cte->ctequery, SelectStmt))
		{
			/* 必须是数据修改语句        */
			Assert(IsA(cte->ctequery, InsertStmt) ||
				   IsA(cte->ctequery, UpdateStmt) ||
				   IsA(cte->ctequery, DeleteStmt));

			pstate->p_hasModifyingCTE = true;
		}
	}

	if (withClause->recursive)
	{
		/*
		 * 对 WITH RECURSIVE，必要时会重排列表元素以         
		 * 消除前向引用。首先构建工作数组，并建立树       
		 * 遍历器所需的数据结构。             
		 */
		CteState	cstate;
		int			i;

		cstate.pstate = pstate;
		cstate.numitems = list_length(withClause->ctes);
		cstate.items = (CteItem *) palloc0(cstate.numitems * sizeof(CteItem));
		i = 0;
		foreach(lc, withClause->ctes)
		{
			cstate.items[i].cte = (CommonTableExpr *) lfirst(lc);
			cstate.items[i].id = i;
			i++;
		}

		/*
		 * 找出全部依赖，并把 CteItem 排成安全的        
		 * 处理顺序。同时标记含自引用的 CTE。               
		 */
		makeDependencyGraph(&cstate);

		/*
		 * 检查递归查询是否形式良好。      
		 */
		checkWellFormedRecursion(&cstate);

		/*
		 * 为解析分析建立 ctenamespace。按标准，所有 WITH       
		 * 条目彼此互相可见，因此要在解析分析之前全部   
		 * 放入。我们按安全的处理顺序构建列表，以便     
		 * 规划器能依次处理这些查询。     
		 */
		for (i = 0; i < cstate.numitems; i++)
		{
			CommonTableExpr *cte = cstate.items[i].cte;

			pstate->p_ctenamespace = lappend(pstate->p_ctenamespace, cte);
		}

		/*
		 * 按拓扑排序确定的顺序执行解析分析。               
		 */
		for (i = 0; i < cstate.numitems; i++)
		{
			CommonTableExpr *cte = cstate.items[i].cte;

			analyzeCTE(pstate, cte);
		}
	}
	else
	{
		/*
		 * 对于非递归 WITH，只需依次分析每个 CTE，然后      
		 * 把它加入 ctenamespace。这对应于标准对各       
		 * 个 WITH 名称作用域的定义。不过，为了让错误       
		 * 报告能察觉错误引用的可能性，我们在              
		 * p_future_ctes 中维护尚不可见的 CTE 列表。            
		 */
		pstate->p_future_ctes = list_copy(withClause->ctes);

		foreach(lc, withClause->ctes)
		{
			CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

			analyzeCTE(pstate, cte);
			pstate->p_ctenamespace = lappend(pstate->p_ctenamespace, cte);
			pstate->p_future_ctes = list_delete_first(pstate->p_future_ctes);
		}
	}

	return pstate->p_ctenamespace;
}


/*
 * 对一个 CTE 执行实际的解析分析转换。它依赖的   
 * 全部 CTE 都已装入 pstate->p_ctenamespace，                       
 * 并且已标记了正确的输出列名/类型。               
 */
static void
analyzeCTE(ParseState *pstate, CommonTableExpr *cte)
{
	Query	   *query;
	CTESearchClause *search_clause = cte->search_clause;
	CTECycleClause *cycle_clause = cte->cycle_clause;

	/* 尚未做过分析        */
	Assert(!IsA(cte->ctequery, Query));

	/*
	 * 在分析 CTE 查询之前，最好先确定循环标记列            
	 * （若有）的数据类型，因为查询可能引用它。      
	 * cycle 子句的其他有效性检查随后进行。               
	 */
	if (cycle_clause)
	{
		TypeCacheEntry *typentry;
		Oid			op;

		cycle_clause->cycle_mark_value =
			transformExpr(pstate, cycle_clause->cycle_mark_value,
						  EXPR_KIND_CYCLE_MARK);
		cycle_clause->cycle_mark_default =
			transformExpr(pstate, cycle_clause->cycle_mark_default,
						  EXPR_KIND_CYCLE_MARK);

		cycle_clause->cycle_mark_type =
			select_common_type(pstate,
							   list_make2(cycle_clause->cycle_mark_value,
										  cycle_clause->cycle_mark_default),
							   "CYCLE", NULL);
		cycle_clause->cycle_mark_value =
			coerce_to_common_type(pstate,
								  cycle_clause->cycle_mark_value,
								  cycle_clause->cycle_mark_type,
								  "CYCLE/SET/TO");
		cycle_clause->cycle_mark_default =
			coerce_to_common_type(pstate,
								  cycle_clause->cycle_mark_default,
								  cycle_clause->cycle_mark_type,
								  "CYCLE/SET/DEFAULT");

		cycle_clause->cycle_mark_typmod =
			select_common_typmod(pstate,
								 list_make2(cycle_clause->cycle_mark_value,
											cycle_clause->cycle_mark_default),
								 cycle_clause->cycle_mark_type);

		cycle_clause->cycle_mark_collation =
			select_common_collation(pstate,
									list_make2(cycle_clause->cycle_mark_value,
											   cycle_clause->cycle_mark_default),
									true);

		/* 不妨顺带查找相关的 <> 操作符                          */
		typentry = lookup_type_cache(cycle_clause->cycle_mark_type,
									 TYPECACHE_EQ_OPR);
		if (!OidIsValid(typentry->eq_opr))
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_FUNCTION),
					errmsg("could not identify an equality operator for type %s",
						   format_type_be(cycle_clause->cycle_mark_type)));
		op = get_negator(typentry->eq_opr);
		if (!OidIsValid(op))
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_FUNCTION),
					errmsg("could not identify an inequality operator for type %s",
						   format_type_be(cycle_clause->cycle_mark_type)));

		cycle_clause->cycle_mark_neop = op;
	}

	/* 现在可以继续分析该 CTE 的查询了     */
	query = parse_sub_analyze(cte->ctequery, pstate, cte, false, true);
	cte->ctequery = (Node *) query;

	/*
	 * 检查我们得到的东西是否合理。头两种情况            
	 * 本应由文法阻止。    
	 */
	if (!IsA(query, Query))
		elog(ERROR, "unexpected non-Query statement in WITH");
	if (query->utilityStmt != NULL)
		elog(ERROR, "unexpected utility statement in WITH");

	/*
	 * 我们禁止数据修改型 WITH 出现在查询顶层以外，    
	 * 因为这类修改应在何时执行并不明确。                
	 */
	if (query->commandType != CMD_SELECT &&
		pstate->parentParseState != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("WITH clause containing a data-modifying statement must be at the top level"),
				 parser_errposition(pstate, cte->location)));

	/*
	 * CTE 查询总被标记为 canSetTag 为假。（目前这只      
	 * 对数据修改语句有影响，该标志将被传播到        
	 * ModifyTable 计划节点。）           
	 */
	query->canSetTag = false;

	if (!cte->cterecursive)
	{
		/* 若尚未完成，则计算输出列名/类型        */
		analyzeCTETargetList(pstate, cte, GetCTETargetList(cte));
	}
	else
	{
		/*
		 * 校验先前确定的输出列类型和排序规则          
		 * 是否与查询实际产出相符。必须做这项检查，      
		 * 因为递归项可能覆盖了非递归项，而我们   
		 * 没有任何简单的办法来修复。                        
		 */
		ListCell   *lctlist,
				   *lctyp,
				   *lctypmod,
				   *lccoll;
		int			varattno;

		lctyp = list_head(cte->ctecoltypes);
		lctypmod = list_head(cte->ctecoltypmods);
		lccoll = list_head(cte->ctecolcollations);
		varattno = 0;
		foreach(lctlist, GetCTETargetList(cte))
		{
			TargetEntry *te = (TargetEntry *) lfirst(lctlist);
			Node	   *texpr;

			if (te->resjunk)
				continue;
			varattno++;
			Assert(varattno == te->resno);
			if (lctyp == NULL || lctypmod == NULL || lccoll == NULL)	/* 不应发生     */
				elog(ERROR, "wrong number of output columns in WITH");
			texpr = (Node *) te->expr;
			if (exprType(texpr) != lfirst_oid(lctyp) ||
				exprTypmod(texpr) != lfirst_int(lctypmod))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("recursive query \"%s\" column %d has type %s in non-recursive term but type %s overall",
								cte->ctename, varattno,
								format_type_with_typemod(lfirst_oid(lctyp),
														 lfirst_int(lctypmod)),
								format_type_with_typemod(exprType(texpr),
														 exprTypmod(texpr))),
						 errhint("Cast the output of the non-recursive term to the correct type."),
						 parser_errposition(pstate, exprLocation(texpr))));
			if (exprCollation(texpr) != lfirst_oid(lccoll))
				ereport(ERROR,
						(errcode(ERRCODE_COLLATION_MISMATCH),
						 errmsg("recursive query \"%s\" column %d has collation \"%s\" in non-recursive term but collation \"%s\" overall",
								cte->ctename, varattno,
								get_collation_name(lfirst_oid(lccoll)),
								get_collation_name(exprCollation(texpr))),
						 errhint("Use the COLLATE clause to set the collation of the non-recursive term."),
						 parser_errposition(pstate, exprLocation(texpr))));
			lctyp = lnext(cte->ctecoltypes, lctyp);
			lctypmod = lnext(cte->ctecoltypmods, lctypmod);
			lccoll = lnext(cte->ctecolcollations, lccoll);
		}
		if (lctyp != NULL || lctypmod != NULL || lccoll != NULL)	/* 不应发生     */
			elog(ERROR, "wrong number of output columns in WITH");
	}

	/*
	 * 现在对存在的 SEARCH 和 CYCLE 子句做有效性检查。      
	 */
	if (search_clause || cycle_clause)
	{
		Query	   *ctequery;
		SetOperationStmt *sos;

		if (!cte->cterecursive)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("WITH query is not recursive"),
					 parser_errposition(pstate, cte->location)));

		/*
		 * SQL 需要 WITH 列表元素（CTE）必须是"可展开"的，   
		 * 才允许带 search 或 cycle 子句。这比仅仅递归是      
		 * 更强的要求。它基本上意味着查询表达式形如       
		 *           
		 *
		 * non-recursive query UNION [ALL] recursive query
		 *
		 * 且递归查询本身不是集合操作。                 
		 *
		 * 撰写本文时，PostgreSQL 允许的所有递归 CTE 都         
		 * 已满足这些条件的大部分。将来若接受更多   
		 * 变体的递归 CTE，此处可能需要进一步检查来  
		 * 判定什么才算"可展开"。                               
		 */

		ctequery = castNode(Query, cte->ctequery);
		Assert(ctequery->setOperations);
		sos = castNode(SetOperationStmt, ctequery->setOperations);

		/*
		 * 这一左侧检查并非可展开性所必需，但        
		 * rewriteSearchAndCycle() 目前尚不支持它，所以           
		 * 在此捕获。  
		 */
		if (!IsA(sos->larg, RangeTblRef))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("with a SEARCH or CYCLE clause, the left side of the UNION must be a SELECT")));

		if (!IsA(sos->rarg, RangeTblRef))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("with a SEARCH or CYCLE clause, the right side of the UNION must be a SELECT")));
	}

	if (search_clause)
	{
		ListCell   *lc;
		List	   *seen = NIL;

		foreach(lc, search_clause->search_col_list)
		{
			String	   *colname = lfirst_node(String, lc);

			if (!list_member(cte->ctecolnames, colname))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("search column \"%s\" not in WITH query column list",
								strVal(colname)),
						 parser_errposition(pstate, search_clause->location)));

			if (list_member(seen, colname))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("search column \"%s\" specified more than once",
								strVal(colname)),
						 parser_errposition(pstate, search_clause->location)));
			seen = lappend(seen, colname);
		}

		if (list_member(cte->ctecolnames, makeString(search_clause->search_seq_column)))
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("search sequence column name \"%s\" already used in WITH query column list",
						   search_clause->search_seq_column),
					parser_errposition(pstate, search_clause->location));
	}

	if (cycle_clause)
	{
		ListCell   *lc;
		List	   *seen = NIL;

		foreach(lc, cycle_clause->cycle_col_list)
		{
			String	   *colname = lfirst_node(String, lc);

			if (!list_member(cte->ctecolnames, colname))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cycle column \"%s\" not in WITH query column list",
								strVal(colname)),
						 parser_errposition(pstate, cycle_clause->location)));

			if (list_member(seen, colname))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("cycle column \"%s\" specified more than once",
								strVal(colname)),
						 parser_errposition(pstate, cycle_clause->location)));
			seen = lappend(seen, colname);
		}

		if (list_member(cte->ctecolnames, makeString(cycle_clause->cycle_mark_column)))
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("cycle mark column name \"%s\" already used in WITH query column list",
						   cycle_clause->cycle_mark_column),
					parser_errposition(pstate, cycle_clause->location));

		if (list_member(cte->ctecolnames, makeString(cycle_clause->cycle_path_column)))
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("cycle path column name \"%s\" already used in WITH query column list",
						   cycle_clause->cycle_path_column),
					parser_errposition(pstate, cycle_clause->location));

		if (strcmp(cycle_clause->cycle_mark_column,
				   cycle_clause->cycle_path_column) == 0)
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("cycle mark column name and cycle path column name are the same"),
					parser_errposition(pstate, cycle_clause->location));
	}

	if (search_clause && cycle_clause)
	{
		if (strcmp(search_clause->search_seq_column,
				   cycle_clause->cycle_mark_column) == 0)
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("search sequence column name and cycle mark column name are the same"),
					parser_errposition(pstate, search_clause->location));

		if (strcmp(search_clause->search_seq_column,
				   cycle_clause->cycle_path_column) == 0)
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("search sequence column name and cycle path column name are the same"),
					parser_errposition(pstate, search_clause->location));
	}
}

/*
 * 给定转换后的输出目标列表，计算 CTE 的衍生字段       
 *
 * 对非递归 CTE，在转换该 CTE 的查询之后调用。              
 * 对递归 CTE，在转换非递归项之后调用，并且只             
 * 传入非递归项产生的目标列表。                     
 *
 * 注意：递归情形下传入的 pstate 实际上是正用于          
 * 分析该 CTE 查询的那个，因此比非递归情形低           
 * 一个层级。这无关紧要，因为我们只用它提供        
 * 错误消息上下文。     
 */
void
analyzeCTETargetList(ParseState *pstate, CommonTableExpr *cte, List *tlist)
{
	int			numaliases;
	int			varattno;
	ListCell   *tlistitem;

	/* 尚未完成……   */
	Assert(cte->ctecolnames == NIL);

	/*
	 * 我们需要确定列名、类型和排序规则。别名           
	 * 列名会覆盖来自查询本身的任何名称。（注意：     
	 * SQL 标准规定别名列表必须为空，或与输出列            
	 * 集合完全等长；但为与 Alias 处理保持一致，我们       
	 * 允许更短。）   
	 */
	cte->ctecolnames = copyObject(cte->aliascolnames);
	cte->ctecoltypes = cte->ctecoltypmods = cte->ctecolcollations = NIL;
	numaliases = list_length(cte->aliascolnames);
	varattno = 0;
	foreach(tlistitem, tlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(tlistitem);
		Oid			coltype;
		int32		coltypmod;
		Oid			colcoll;

		if (te->resjunk)
			continue;
		varattno++;
		Assert(varattno == te->resno);
		if (varattno > numaliases)
		{
			char	   *attrname;

			attrname = pstrdup(te->resname);
			cte->ctecolnames = lappend(cte->ctecolnames, makeString(attrname));
		}
		coltype = exprType((Node *) te->expr);
		coltypmod = exprTypmod((Node *) te->expr);
		colcoll = exprCollation((Node *) te->expr);

		/*
		 * 若 CTE 是递归的，则把任何"unknown"列的暴露     
		 * 类型强制为"text"。必须在此处理这一点，因为    
		 * 在对非递归项调用我们时，尚无人尝试过把     
		 * unknown 输出列强制为其他类型。必须在查看       
		 * 递归项之前解决 unknown 的问题。                    
		 *
		 * 该列可能含有 'foo' COLLATE "bar"，若排序规则       
		 * 已设置则勿覆盖。      
		 */
		if (cte->cterecursive && coltype == UNKNOWNOID)
		{
			coltype = TEXTOID;
			coltypmod = -1;		/* 本应为 -1，但需确认       */
			if (!OidIsValid(colcoll))
				colcoll = DEFAULT_COLLATION_OID;
		}
		cte->ctecoltypes = lappend_oid(cte->ctecoltypes, coltype);
		cte->ctecoltypmods = lappend_int(cte->ctecoltypmods, coltypmod);
		cte->ctecolcollations = lappend_oid(cte->ctecolcollations, colcoll);
	}
	if (varattno < numaliases)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("WITH query \"%s\" has %d columns available but %d columns specified",
						cte->ctename, varattno, numaliases),
				 parser_errposition(pstate, cte->location)));
}


/*
 * 识别 WITH RECURSIVE 条目列表的交叉引用，            
 * 并排序成无前向引用的顺序。               
 */
static void
makeDependencyGraph(CteState *cstate)
{
	int			i;

	for (i = 0; i < cstate->numitems; i++)
	{
		CommonTableExpr *cte = cstate->items[i].cte;

		cstate->curitem = i;
		cstate->innerwiths = NIL;
		makeDependencyGraphWalker((Node *) cte->ctequery, cstate);
		Assert(cstate->innerwiths == NIL);
	}

	TopologicalSort(cstate->pstate, cstate->items, cstate->numitems);
}

/*
 * 树遍历函数，用于检测 WITH RECURSIVE 列表中                   
 * CTE 的交叉/自引用。    
 */
static bool
makeDependencyGraphWalker(Node *node, CteState *cstate)
{
	if (node == NULL)
		return false;
	if (IsA(node, RangeVar))
	{
		RangeVar   *rv = (RangeVar *) node;

		/* 若是非限定名，可能是 CTE 引用     */
		if (!rv->schemaname)
		{
			ListCell   *lc;
			int			i;

			/* ……但先看它是否被内层 WITH 捕获       */
			foreach(lc, cstate->innerwiths)
			{
				List	   *withlist = (List *) lfirst(lc);
				ListCell   *lc2;

				foreach(lc2, withlist)
				{
					CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc2);

					if (strcmp(rv->relname, cte->ctename) == 0)
						return false;	/* 是，故退出  */
				}
			}

			/* 否，可能是对我们正在处理的查询层级的引用  */
			for (i = 0; i < cstate->numitems; i++)
			{
				CommonTableExpr *cte = cstate->items[i].cte;

				if (strcmp(rv->relname, cte->ctename) == 0)
				{
					int			myindex = cstate->curitem;

					if (i != myindex)
					{
						/* 添加跨条目依赖     */
						cstate->items[myindex].depends_on =
							bms_add_member(cstate->items[myindex].depends_on,
										   cstate->items[i].id);
					}
					else
					{
						/* 发现此条目是自引用的         */
						cte->cterecursive = true;
					}
					break;
				}
			}
		}
		return false;
	}
	if (IsA(node, SelectStmt))
	{
		SelectStmt *stmt = (SelectStmt *) node;

		if (stmt->withClause)
		{
			/* 检查 WITH 子句和 SelectStmt           */
			WalkInnerWith(node, stmt->withClause, cstate);
			/* SelectStmt 检查完毕             */
			return false;
		}
		/* 若无 WITH 子句，则直接落入常规处理           */
	}
	else if (IsA(node, InsertStmt))
	{
		InsertStmt *stmt = (InsertStmt *) node;

		if (stmt->withClause)
		{
			/* 检查 WITH 子句和 InsertStmt           */
			WalkInnerWith(node, stmt->withClause, cstate);
			/* InsertStmt 检查完毕             */
			return false;
		}
		/* 若无 WITH 子句，则直接落入常规处理           */
	}
	else if (IsA(node, DeleteStmt))
	{
		DeleteStmt *stmt = (DeleteStmt *) node;

		if (stmt->withClause)
		{
			/* 检查 WITH 子句和 DeleteStmt           */
			WalkInnerWith(node, stmt->withClause, cstate);
			/* DeleteStmt 检查完毕             */
			return false;
		}
		/* 若无 WITH 子句，则直接落入常规处理           */
	}
	else if (IsA(node, UpdateStmt))
	{
		UpdateStmt *stmt = (UpdateStmt *) node;

		if (stmt->withClause)
		{
			/* 检查 WITH 子句和 UpdateStmt           */
			WalkInnerWith(node, stmt->withClause, cstate);
			/* UpdateStmt 检查完毕             */
			return false;
		}
		/* 若无 WITH 子句，则直接落入常规处理           */
	}
	else if (IsA(node, MergeStmt))
	{
		MergeStmt  *stmt = (MergeStmt *) node;

		if (stmt->withClause)
		{
			/* 检查 WITH 子句和 MergeStmt           */
			WalkInnerWith(node, stmt->withClause, cstate);
			/* MergeStmt 检查完毕             */
			return false;
		}
		/* 若无 WITH 子句，则直接落入常规处理           */
	}
	else if (IsA(node, WithClause))
	{
		/*
		 * 阻止 raw_expression_tree_walker 直接递归进入             
		 * WITH 子句。我们只希望它在上方代码控制下          
		 * 发生。  
		 */
		return false;
	}
	return raw_expression_tree_walker(node,
									  makeDependencyGraphWalker,
									  (void *) cstate);
}

/*
 * makeDependencyGraphWalker 对带 WITH 子句语句的递归处理。        
 *
 * 此子程序负责按 CTE 名称的可见性规则正确                
 * 更新 innerwiths 列表。                 
 */
static void
WalkInnerWith(Node *stmt, WithClause *withClause, CteState *cstate)
{
	ListCell   *lc;

	if (withClause->recursive)
	{
		/*
		 * RECURSIVE 情形下，WITH 的全部查询名称对              
		 * 所有 WITH 条目和主查询均可见。故全部压入，   
		 * 处理，随后弹出。  
		 */
		cstate->innerwiths = lcons(withClause->ctes, cstate->innerwiths);
		foreach(lc, withClause->ctes)
		{
			CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

			(void) makeDependencyGraphWalker(cte->ctequery, cstate);
		}
		(void) raw_expression_tree_walker(stmt,
										  makeDependencyGraphWalker,
										  (void *) cstate);
		cstate->innerwiths = list_delete_first(cstate->innerwiths);
	}
	else
	{
		/*
		 * 非 RECURSIVE 情形下，查询名称对其后的 WITH       
		 * 条目以及主查询可见。         
		 */
		cstate->innerwiths = lcons(NIL, cstate->innerwiths);
		foreach(lc, withClause->ctes)
		{
			CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);
			ListCell   *cell1;

			(void) makeDependencyGraphWalker(cte->ctequery, cstate);
			/* 注意递归可能修改 innerwiths 列表       */
			cell1 = list_head(cstate->innerwiths);
			lfirst(cell1) = lappend((List *) lfirst(cell1), cte);
		}
		(void) raw_expression_tree_walker(stmt,
										  makeDependencyGraphWalker,
										  (void *) cstate);
		cstate->innerwiths = list_delete_first(cstate->innerwiths);
	}
}

/*
 * 按依赖关系排序，采用标准拓扑排序操作           
 */
static void
TopologicalSort(ParseState *pstate, CteItem *items, int numitems)
{
	int			i,
				j;

	/* 对序列中的每个位置…… */
	for (i = 0; i < numitems; i++)
	{
		/* ……扫描其余条目，找出无依赖的一个               */
		for (j = i; j < numitems; j++)
		{
			if (bms_is_empty(items[j].depends_on))
				break;
		}

		/* 若没找到，说明依赖图中存在环              */
		if (j >= numitems)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("mutual recursion between WITH items is not implemented"),
					 parser_errposition(pstate, items[i].cte->location)));

		/*
		 * 找到了。把它移到最前，并从其他条目的依赖      
		 * 中删除。 
		 */
		if (i != j)
		{
			CteItem		tmp;

			tmp = items[i];
			items[i] = items[j];
			items[j] = tmp;
		}

		/*
		 * 下标 i 及其之前的条目都已确认不再有依赖，    
		 * 本循环可跳过它们。
		 */
		for (j = i + 1; j < numitems; j++)
		{
			items[j].depends_on = bms_del_member(items[j].depends_on,
												 items[i].id);
		}
	}
}


/*
 * 检查递归查询是否形式良好。      
 */
static void
checkWellFormedRecursion(CteState *cstate)
{
	int			i;

	for (i = 0; i < cstate->numitems; i++)
	{
		CommonTableExpr *cte = cstate->items[i].cte;
		SelectStmt *stmt = (SelectStmt *) cte->ctequery;

		Assert(!IsA(stmt, Query));	/* 尚未分析     */

		/* 忽略未被判定为递归的条目            */
		if (!cte->cterecursive)
			continue;

		/* 必须是 SELECT 语句    */
		if (!IsA(stmt, SelectStmt))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_RECURSION),
					 errmsg("recursive query \"%s\" must not contain data-modifying statements",
							cte->ctename),
					 parser_errposition(cstate->pstate, cte->location)));

		/* 必须有顶层 UNION     */
		if (stmt->op != SETOP_UNION)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_RECURSION),
					 errmsg("recursive query \"%s\" does not have the form non-recursive-term UNION [ALL] recursive-term",
							cte->ctename),
					 parser_errposition(cstate->pstate, cte->location)));

		/*
		 * 严格来说，我们本应坚持不允许出现顶层 WITH，    
		 * 因为语法上它会包住 UNION。不过，过去我们      
		 * 一直没有这么做，现在改恐怕已经太迟。退而     
		 * 求其次：只要求 WITH 内不含自引用。                
		 * 此项测试应在检查 UNION 各分支之前做，以免发出  
		 * 含混报错  
		 */
		if (stmt->withClause)
		{
			cstate->curitem = i;
			cstate->innerwiths = NIL;
			cstate->selfrefcount = 0;
			cstate->context = RECURSION_SUBLINK;
			checkWellFormedRecursionWalker((Node *) stmt->withClause->ctes,
										   cstate);
			Assert(cstate->innerwiths == NIL);
		}

		/*
		 * 禁止在 UNION 之上使用 ORDER BY 等类似修饰。      
		 * 这类东西毫无意义，因为我们只拿到递归查询    
		 * 结果的一部分时，根本无从判断其含义。（若     
		 * 真的允许它们，就必须检查这些子树内部的     
		 * 递归引用。至于 WITH，同样必须在检查            
		 * UNION 各分支之前做此事，以免此处存在递归     
		 * 引用时报出含混错误。）    
		 */
		if (stmt->sortClause)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("ORDER BY in a recursive query is not implemented"),
					 parser_errposition(cstate->pstate,
										exprLocation((Node *) stmt->sortClause))));
		if (stmt->limitOffset)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("OFFSET in a recursive query is not implemented"),
					 parser_errposition(cstate->pstate,
										exprLocation(stmt->limitOffset))));
		if (stmt->limitCount)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("LIMIT in a recursive query is not implemented"),
					 parser_errposition(cstate->pstate,
										exprLocation(stmt->limitCount))));
		if (stmt->lockingClause)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("FOR UPDATE/SHARE in a recursive query is not implemented"),
					 parser_errposition(cstate->pstate,
										exprLocation((Node *) stmt->lockingClause))));

		/*
		 * 现在可以开始检查 UNION 操作数本身了。          
		 *
		 * 左操作数绝不能包含任何自引用。                 
		 */
		cstate->curitem = i;
		cstate->innerwiths = NIL;
		cstate->selfrefcount = 0;
		cstate->context = RECURSION_NONRECURSIVETERM;
		checkWellFormedRecursionWalker((Node *) stmt->larg, cstate);
		Assert(cstate->innerwiths == NIL);

		/* 右操作数应在合法位置含恰好一个自引用           */
		cstate->curitem = i;
		cstate->innerwiths = NIL;
		cstate->selfrefcount = 0;
		cstate->context = RECURSION_OK;
		checkWellFormedRecursionWalker((Node *) stmt->rarg, cstate);
		Assert(cstate->innerwiths == NIL);
		if (cstate->selfrefcount != 1)	/* 不应发生     */
			elog(ERROR, "missing recursive reference");
	}
}

/*
 * 树遍历函数：检测递归查询中无效的自引用。                
 */
static bool
checkWellFormedRecursionWalker(Node *node, CteState *cstate)
{
	RecursionContext save_context = cstate->context;

	if (node == NULL)
		return false;
	if (IsA(node, RangeVar))
	{
		RangeVar   *rv = (RangeVar *) node;

		/* 若是非限定名，可能是 CTE 引用     */
		if (!rv->schemaname)
		{
			ListCell   *lc;
			CommonTableExpr *mycte;

			/* ……但先看它是否被内层 WITH 捕获       */
			foreach(lc, cstate->innerwiths)
			{
				List	   *withlist = (List *) lfirst(lc);
				ListCell   *lc2;

				foreach(lc2, withlist)
				{
					CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc2);

					if (strcmp(rv->relname, cte->ctename) == 0)
						return false;	/* 是，故退出  */
				}
			}

			/* 否，可能是对我们正在处理的查询层级的引用  */
			mycte = cstate->items[cstate->curitem].cte;
			if (strcmp(rv->relname, mycte->ctename) == 0)
			{
				/* 发现对当前活动查询的递归引用      */
				if (cstate->context != RECURSION_OK)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_RECURSION),
							 errmsg(recursion_errormsgs[cstate->context],
									mycte->ctename),
							 parser_errposition(cstate->pstate,
												rv->location)));
				/* 统计引用     */
				if (++(cstate->selfrefcount) > 1)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_RECURSION),
							 errmsg("recursive reference to query \"%s\" must not appear more than once",
									mycte->ctename),
							 parser_errposition(cstate->pstate,
												rv->location)));
			}
		}
		return false;
	}
	if (IsA(node, SelectStmt))
	{
		SelectStmt *stmt = (SelectStmt *) node;
		ListCell   *lc;

		if (stmt->withClause)
		{
			if (stmt->withClause->recursive)
			{
				/*
				 * 在 RECURSIVE 情形下，WITH 的所有查询名     
				 * 对主查询以及所有 WITH 条目均可见。因此 
				 * 全部压入，处理，再全部弹出。  
				 */
				cstate->innerwiths = lcons(stmt->withClause->ctes,
										   cstate->innerwiths);
				foreach(lc, stmt->withClause->ctes)
				{
					CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

					(void) checkWellFormedRecursionWalker(cte->ctequery, cstate);
				}
				checkWellFormedSelectStmt(stmt, cstate);
				cstate->innerwiths = list_delete_first(cstate->innerwiths);
			}
			else
			{
				/*
				 * 在非 RECURSIVE 情形下，查询名称对其后的    
				 * WITH 条目以及主查询可见。         
				 */
				cstate->innerwiths = lcons(NIL, cstate->innerwiths);
				foreach(lc, stmt->withClause->ctes)
				{
					CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);
					ListCell   *cell1;

					(void) checkWellFormedRecursionWalker(cte->ctequery, cstate);
					/* 注意递归可能修改 innerwiths 列表       */
					cell1 = list_head(cstate->innerwiths);
					lfirst(cell1) = lappend((List *) lfirst(cell1), cte);
				}
				checkWellFormedSelectStmt(stmt, cstate);
				cstate->innerwiths = list_delete_first(cstate->innerwiths);
			}
		}
		else
			checkWellFormedSelectStmt(stmt, cstate);
		/* SelectStmt 检查完毕             */
		return false;
	}
	if (IsA(node, WithClause))
	{
		/*
		 * 阻止 raw_expression_tree_walker 直接递归进入             
		 * WITH 子句。我们只希望它在上方代码控制下          
		 * 发生。  
		 */
		return false;
	}
	if (IsA(node, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) node;

		switch (j->jointype)
		{
			case JOIN_INNER:
				checkWellFormedRecursionWalker(j->larg, cstate);
				checkWellFormedRecursionWalker(j->rarg, cstate);
				checkWellFormedRecursionWalker(j->quals, cstate);
				break;
			case JOIN_LEFT:
				checkWellFormedRecursionWalker(j->larg, cstate);
				if (save_context == RECURSION_OK)
					cstate->context = RECURSION_OUTERJOIN;
				checkWellFormedRecursionWalker(j->rarg, cstate);
				cstate->context = save_context;
				checkWellFormedRecursionWalker(j->quals, cstate);
				break;
			case JOIN_FULL:
				if (save_context == RECURSION_OK)
					cstate->context = RECURSION_OUTERJOIN;
				checkWellFormedRecursionWalker(j->larg, cstate);
				checkWellFormedRecursionWalker(j->rarg, cstate);
				cstate->context = save_context;
				checkWellFormedRecursionWalker(j->quals, cstate);
				break;
			case JOIN_RIGHT:
				if (save_context == RECURSION_OK)
					cstate->context = RECURSION_OUTERJOIN;
				checkWellFormedRecursionWalker(j->larg, cstate);
				cstate->context = save_context;
				checkWellFormedRecursionWalker(j->rarg, cstate);
				checkWellFormedRecursionWalker(j->quals, cstate);
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
		}
		return false;
	}
	if (IsA(node, SubLink))
	{
		SubLink    *sl = (SubLink *) node;

		/*
		 * 有意覆盖外层上下文，因为子查询是          
		 * 独立的  
		 */
		cstate->context = RECURSION_SUBLINK;
		checkWellFormedRecursionWalker(sl->subselect, cstate);
		cstate->context = save_context;
		checkWellFormedRecursionWalker(sl->testexpr, cstate);
		return false;
	}
	return raw_expression_tree_walker(node,
									  checkWellFormedRecursionWalker,
									  (void *) cstate);
}

/*
 * checkWellFormedRecursionWalker 的子例程：处理 SelectStmt， 
 * 无需理会其 WITH 子句           
 */
static void
checkWellFormedSelectStmt(SelectStmt *stmt, CteState *cstate)
{
	RecursionContext save_context = cstate->context;

	if (save_context != RECURSION_OK)
	{
		/* 仅继续递归，不改变状态   */
		raw_expression_tree_walker((Node *) stmt,
								   checkWellFormedRecursionWalker,
								   (void *) cstate);
	}
	else
	{
		switch (stmt->op)
		{
			case SETOP_NONE:
			case SETOP_UNION:
				raw_expression_tree_walker((Node *) stmt,
										   checkWellFormedRecursionWalker,
										   (void *) cstate);
				break;
			case SETOP_INTERSECT:
				if (stmt->all)
					cstate->context = RECURSION_INTERSECT;
				checkWellFormedRecursionWalker((Node *) stmt->larg,
											   cstate);
				checkWellFormedRecursionWalker((Node *) stmt->rarg,
											   cstate);
				cstate->context = save_context;
				checkWellFormedRecursionWalker((Node *) stmt->sortClause,
											   cstate);
				checkWellFormedRecursionWalker((Node *) stmt->limitOffset,
											   cstate);
				checkWellFormedRecursionWalker((Node *) stmt->limitCount,
											   cstate);
				checkWellFormedRecursionWalker((Node *) stmt->lockingClause,
											   cstate);
				/* 此处有意忽略 stmt->withClause            */
				break;
			case SETOP_EXCEPT:
				if (stmt->all)
					cstate->context = RECURSION_EXCEPT;
				checkWellFormedRecursionWalker((Node *) stmt->larg,
											   cstate);
				cstate->context = RECURSION_EXCEPT;
				checkWellFormedRecursionWalker((Node *) stmt->rarg,
											   cstate);
				cstate->context = save_context;
				checkWellFormedRecursionWalker((Node *) stmt->sortClause,
											   cstate);
				checkWellFormedRecursionWalker((Node *) stmt->limitOffset,
											   cstate);
				checkWellFormedRecursionWalker((Node *) stmt->limitCount,
											   cstate);
				checkWellFormedRecursionWalker((Node *) stmt->lockingClause,
											   cstate);
				/* 此处有意忽略 stmt->withClause            */
				break;
			default:
				elog(ERROR, "unrecognized set op: %d",
					 (int) stmt->op);
		}
	}
}
