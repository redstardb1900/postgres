/*-------------------------------------------------------------------------
 *
 * parse_clause.c
 *	  解析器中处理子句
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_clause.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "access/tsmapi.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


static int	extractRemainingColumns(ParseState *pstate,
									ParseNamespaceColumn *src_nscolumns,
									List *src_colnames,
									List **src_colnos,
									List **res_colnames, List **res_colvars,
									ParseNamespaceColumn *res_nscolumns);
static Node *transformJoinUsingClause(ParseState *pstate,
									  List *leftVars, List *rightVars);
static Node *transformJoinOnClause(ParseState *pstate, JoinExpr *j,
								   List *namespace);
static ParseNamespaceItem *transformTableEntry(ParseState *pstate, RangeVar *r);
static ParseNamespaceItem *transformRangeSubselect(ParseState *pstate,
												   RangeSubselect *r);
static ParseNamespaceItem *transformRangeFunction(ParseState *pstate,
												  RangeFunction *r);
static ParseNamespaceItem *transformRangeTableFunc(ParseState *pstate,
												   RangeTableFunc *rtf);
static TableSampleClause *transformRangeTableSample(ParseState *pstate,
													RangeTableSample *rts);
static ParseNamespaceItem *getNSItemForSpecialRelationTypes(ParseState *pstate,
															RangeVar *rv);
static Node *transformFromClauseItem(ParseState *pstate, Node *n,
									 ParseNamespaceItem **top_nsitem,
									 List **namespace);
static Var *buildVarFromNSColumn(ParseState *pstate,
								 ParseNamespaceColumn *nscol);
static Node *buildMergedJoinVar(ParseState *pstate, JoinType jointype,
								Var *l_colvar, Var *r_colvar);
static void markRelsAsNulledBy(ParseState *pstate, Node *n, int jindex);
static void setNamespaceColumnVisibility(List *namespace, bool cols_visible);
static void setNamespaceLateralState(List *namespace,
									 bool lateral_only, bool lateral_ok);
static void checkExprIsVarFree(ParseState *pstate, Node *n,
							   const char *constructName);
static TargetEntry *findTargetlistEntrySQL92(ParseState *pstate, Node *node,
											 List **tlist, ParseExprKind exprKind);
static TargetEntry *findTargetlistEntrySQL99(ParseState *pstate, Node *node,
											 List **tlist, ParseExprKind exprKind);
static int	get_matching_location(int sortgroupref,
								  List *sortgrouprefs, List *exprs);
static List *resolve_unique_index_expr(ParseState *pstate, InferClause *infer,
									   Relation heapRel);
static List *addTargetToGroupList(ParseState *pstate, TargetEntry *tle,
								  List *grouplist, List *targetlist, int location);
static WindowClause *findWindowClause(List *wclist, const char *name);
static Node *transformFrameOffset(ParseState *pstate, int frameOptions,
								  Oid rangeopfamily, Oid rangeopcintype, Oid *inRangeFunc,
								  Node *clause);


/*
 * transformFromClause -
 *	  处理 FROM 子句，把各项加入查询的范围表、        
 *	  joinlist与命名空间 
 *
 * 注意：我们假定 pstate 的 p_rtable、p_joinlist、              
 * 和 p_namespace 列表在创建时已初始化为 NIL。  
 * 对已有条目会在其上追加 --- 这是规则处理                
 * 以及 UPDATE 和 DELETE 所需要的。     
 */
void
transformFromClause(ParseState *pstate, List *frmList)
{
	ListCell   *fl;

	/*
	 * 语法此时已生成 RangeVar、RangeSubselect、                  
	 * RangeFunction 或 JoinExpr 的列表。逐个转换（可能         
	 * 向 rtable 添加条目），检查重名，然后加入             
	 * joinlist 与命名空间。   
	 *
	 * 注意：要正确处理 LATERAL 引用，必须从左到右       
	 * 处理各条目。 
	 */
	foreach(fl, frmList)
	{
		Node	   *n = lfirst(fl);
		ParseNamespaceItem *nsitem;
		List	   *namespace;

		n = transformFromClauseItem(pstate, n,
									&nsitem,
									&namespace);

		checkNameSpaceConflicts(pstate, pstate->p_namespace, namespace);

		/* 将新命名空间项标记为仅对 LATERAL 可见     */
		setNamespaceLateralState(namespace, true, true);

		pstate->p_joinlist = lappend(pstate->p_joinlist, n);
		pstate->p_namespace = list_concat(pstate->p_namespace, namespace);
	}

	/*
	 * FROM 列表解析完毕，现在让所有命名空间项     
	 * 无条件地可见。注意这也会重置 lateral_only，           
	 * 对于调用时已存在的命名空间项同样生效；             
	 * 不过它们本来就该如此。           
	 */
	setNamespaceLateralState(pstate->p_namespace, false, true);
}

/*
 * setTargetTable
 *	  把 INSERT/UPDATE/DELETE/MERGE 的目标关系加入范围表，         
 *	  在 ParseState 中建立指向它的特殊链接。
 *
 *	  我们还会打开目标关系并获取其写锁。             
 *	  这一步必须在处理 FROM 列表之前完成，以防目标      
 *	  关系同时作为源关系被提及 --- 我们要确保在任何    
 *	  读锁之前拿到写锁。         
 *
 *	  若 alsoSource 为真，则把目标加入查询的 joinlist 和 
 *	  命名空间。对 INSERT，我们不希望目标被连接；    
 *	  它是元组目的地而非源。MERGE 实际两者皆是，  
 *	  但会单独加入 joinlist 与命名空间，                 
 *	  所以此处与 INSERT 一样不加处理就是正确的。     
 *	  对 UPDATE/DELETE 则需要扫描或连接目标。（注意：
 *	  我们不会费心检查命名空间冲突；我们假定此时  
 *	  命名空间初始为空。）  
 *
 *	  最后，我们将该关系标记为需要指定的权限，        
 *	  即requiredPerms 
 *
 *	  返回目标关系在范围表中的下标。       
 */
int
setTargetTable(ParseState *pstate, RangeVar *relation,
			   bool inh, bool alsoSource, AclMode requiredPerms)
{
	ParseNamespaceItem *nsitem;

	/*
	 * ENR 会隐藏同名表，所以需要先检查它们。               
	 * 相比之下，CTE 不会为此隐藏表。            
	 */
	if (relation->schemaname == NULL &&
		scanNameSpaceForENR(pstate, relation->relname))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("relation \"%s\" cannot be the target of a modifying statement",
						relation->relname)));

	/* 关闭旧目标；这只可能发生在多动作规则中       */
	if (pstate->p_target_relation != NULL)
		table_close(pstate->p_target_relation, NoLock);

	/*
	 * 打开目标关系并获取合适的锁（将持有到事务结束    
	 * ）。       
	 *
	 * free_parsestate() 最终会执行对应的 table_close()，          
	 * 但*不会*释放锁。    
	 */
	pstate->p_target_relation = parserOpenTable(pstate, relation,
												RowExclusiveLock);

	/*
	 * 现在构建 RTE 与 ParseNamespaceItem。
	 */
	nsitem = addRangeTableEntryForRelation(pstate, pstate->p_target_relation,
										   RowExclusiveLock,
										   relation->alias, inh, false);

	/* 记住该 RTE/nsitem 是查询目标              */
	pstate->p_target_nsitem = nsitem;

	/*
	 * 覆盖 addRangeTableEntry 默认的 ACL_SELECT 权限检查，         
	 * 改为把目标表标记为恰好需要指定的            
	 * 权限。   
	 *
	 * 若稍后在解析分析中发现该关系的显式引用，  
	 * 我们会把 ACL_SELECT 位加回来；参见           
	 * markVarForSelectPriv 及其调用者 
	 */
	nsitem->p_perminfo->requiredPerms = requiredPerms;

	/*
	 * UPDATE/DELETE则把表加入joinlist与命名空间。
	 */
	if (alsoSource)
		addNSItemToQuery(pstate, nsitem, true, true, true);

	return nsitem->p_rtindex;
}

/*
 * 从源表的列列表中提取所有非公共列                     
 *
 * src_nscolumns 和 src_colnames 描述源表。           
 *
 * *src_colnos 初始时包含已合并列的列号。                    
 * 每新增一列，我们都把其列号加入其中。             
 * 同时把每个新增列的名字追加到 *res_colnames，     
 * 为每个新增列向 *res_colvars 追加一个 Var，               
 * 并把各列的 nscolumns 数据复制到 res_nscolumns[]              
 * （调用者分配，须够大）。 
 *
 * 返回新增列的数目。         
 */
static int
extractRemainingColumns(ParseState *pstate,
						ParseNamespaceColumn *src_nscolumns,
						List *src_colnames,
						List **src_colnos,
						List **res_colnames, List **res_colvars,
						ParseNamespaceColumn *res_nscolumns)
{
	int			colcount = 0;
	Bitmapset  *prevcols;
	int			attnum;
	ListCell   *lc;

	/*
	 * 虽然用 "list_member_int(*src_colnos, attnum)" 也能检测     
	 * 下面循环中已合并的列，但对宽输入表而言那         
	 * 会是 O(N^2) 的代价。为此只针对已合并的 USING 列      
	 * 构建一个 bitmapset，主循环中不会加入。      
	 */
	prevcols = NULL;
	foreach(lc, *src_colnos)
	{
		prevcols = bms_add_member(prevcols, lfirst_int(lc));
	}

	attnum = 0;
	foreach(lc, src_colnames)
	{
		char	   *colname = strVal(lfirst(lc));

		attnum++;
		/* 未被删除且尚未合并？      */
		if (colname[0] != '\0' && !bms_is_member(attnum, prevcols))
		{
			/* 是，作为下一个输出列发出  */
			*src_colnos = lappend_int(*src_colnos, attnum);
			*res_colnames = lappend(*res_colnames, lfirst(lc));
			*res_colvars = lappend(*res_colvars,
								   buildVarFromNSColumn(pstate,
														src_nscolumns + attnum - 1));
			/* 复制该列在输入关系中的 nscolumn 数据       */
			res_nscolumns[colcount] = src_nscolumns[attnum - 1];
			colcount++;
		}
	}
	return colcount;
}

/* transformJoinUsingClause()
 *	  从部分转换的 USING 列表构建完整的 ON 子句。        
 *	  入参为表示左右匹配列的节点列表。                      
 *	  结果为一个转换后的限定表达式。    
 */
static Node *
transformJoinUsingClause(ParseState *pstate,
						 List *leftVars, List *rightVars)
{
	Node	   *result;
	List	   *andargs = NIL;
	ListCell   *lvars,
			   *rvars;

	/*
	 * 这里我们耍了个小花招：构建一棵未转换的            
	 * 操作符树，其叶子已是转换过的 Var。这需要             
	 * transformExpr() 的配合，通常它本应对已转换的        
	 * 子节点提出异议。不过，这确实意味着我们           
	 * 必须自己去把这些列标记为需要 SELECT 特权；          
	 * 它不会代劳。          
	 */
	forboth(lvars, leftVars, rvars, rightVars)
	{
		Var		   *lvar = (Var *) lfirst(lvars);
		Var		   *rvar = (Var *) lfirst(rvars);
		A_Expr	   *e;

		/* 需要对连接变量有读权限         */
		markVarForSelectPriv(pstate, lvar);
		markVarForSelectPriv(pstate, rvar);

		/* 现在创建 lvar = rvar 连接条件     */
		e = makeSimpleA_Expr(AEXPR_OP, "=",
							 (Node *) copyObject(lvar), (Node *) copyObject(rvar),
							 -1);

		/* 若有多个连接列，准备组合成一个 AND 子句        */
		andargs = lappend(andargs, e);
	}

	/* 只有连接列多于一个时才需要 AND           */
	if (list_length(andargs) == 1)
		result = (Node *) linitial(andargs);
	else
		result = (Node *) makeBoolExpr(AND_EXPR, andargs, -1);

	/*
	 * 由于这些引用已是 Var，且必然来自输入关系，          
	 * 因此无需像 transformJoinOnClause()                     
	 * 那样大费周折，只要调用 transformExpr() 修好            
	 * 操作符就大功告成。   
	 */
	result = transformExpr(pstate, result, EXPR_KIND_JOIN_USING);

	result = coerce_to_boolean(pstate, result, "JOIN/USING");

	return result;
}

/* transformJoinOnClause()
 *	  转换 JOIN/ON 的限定条件。         
 *	  结果为一个转换后的限定表达式。    
 */
static Node *
transformJoinOnClause(ParseState *pstate, JoinExpr *j, List *namespace)
{
	Node	   *result;
	List	   *save_namespace;

	/*
	 * 连接表达式应可见的命名空间只有 JOIN 的两        
	 * 棵子树，加上来自上层 pstate 级的外部引用。     
	 * 暂时相应设置本 pstate 的命名空间。（无需               
	 * 检查引用名冲突，因为 transformFromClauseItem()          
	 * 已做过检查。）不论 LATERAL 状态如何，                
	 * 皆可见。  
	 */
	setNamespaceLateralState(namespace, false, true);

	save_namespace = pstate->p_namespace;
	pstate->p_namespace = namespace;

	result = transformWhereClause(pstate, j->quals,
								  EXPR_KIND_JOIN_ON, "JOIN/ON");

	pstate->p_namespace = save_namespace;

	return result;
}

/*
 * transformTableEntry --- 转换 RangeVar（简单关系引用）         
 */
static ParseNamespaceItem *
transformTableEntry(ParseState *pstate, RangeVar *r)
{
	/* addRangeTableEntry 包办全部工作*/
	return addRangeTableEntry(pstate, r, r->alias, r->inh, true);
}

/*
 * transformRangeSubselect --- 转换 FROM 中的子 SELECT            
 */
static ParseNamespaceItem *
transformRangeSubselect(ParseState *pstate, RangeSubselect *r)
{
	Query	   *query;

	/*
	 * 设置 p_expr_kind 以表明本解析层正递归进入子选择。  
	 * 我们无法嵌套在任何表达式内，无需保存/恢复逻辑 
	 *            
	 */
	Assert(pstate->p_expr_kind == EXPR_KIND_NONE);
	pstate->p_expr_kind = EXPR_KIND_FROM_SUBSELECT;

	/*
	 * 若子选择是 LATERAL 的，则让本层的 lateral_only        
	 * 名字对它可见。（LATERAL 无法嵌套于单个                  
	 * 层内，无需保存恢复。）   
	 */
	Assert(!pstate->p_lateral_active);
	pstate->p_lateral_active = r->lateral;

	/*
	 * 分析并转换子查询。注意若子查询没有                   
	 * 别名，就无法显式选择它加锁，但仍可能需要           
	 * 加锁（若存在针对所有表的加锁子句）。             
	 */
	query = parse_sub_analyze(r->subquery, pstate, NULL,
							  isLockedRefname(pstate,
											  r->alias == NULL ? NULL :
											  r->alias->aliasname),
							  true);

	/* 恢复状态  */
	pstate->p_lateral_active = false;
	pstate->p_expr_kind = EXPR_KIND_NONE;

	/*
	 * 检查我们拿到的是否为 SELECT。考虑语法的              
	 * 限制，其余不可能，但仍要检查。 
	 */
	if (!IsA(query, Query) ||
		query->commandType != CMD_SELECT)
		elog(ERROR, "unexpected non-SELECT command in subquery in FROM");

	/*
	 * 好，为子查询构建 RTE 和 nsitem。   
	 */
	return addRangeTableEntryForSubquery(pstate,
										 query,
										 r->alias,
										 r->lateral,
										 true);
}


/*
 * transformRangeFunction --- 转换 FROM 中的函数调用             
 */
static ParseNamespaceItem *
transformRangeFunction(ParseState *pstate, RangeFunction *r)
{
	List	   *funcexprs = NIL;
	List	   *funcnames = NIL;
	List	   *coldeflists = NIL;
	bool		is_lateral;
	ListCell   *lc;

	/*
	 * 无论 RangeFunction 是否显式标记为 LATERAL，我们都让   
	 * 本层的 lateral_only 名字可见。对 UNNEST() 而言，       
	 * 为符合 SQL 标准；也让 FROM 中的所有函数       
	 * 都如此处理更显便利。                
	 *
	 * （LATERAL 无法嵌套于单个 pstate 层内，因此无需     
	 * 保存恢复。）       
	 */
	Assert(!pstate->p_lateral_active);
	pstate->p_lateral_active = true;

	/*
	 * 转换原始表达式。      
	 *
	 * 转换的同时，也保存函数名，以便可能用作             
	 * 别名和列名。我们使用与 SELECT 输出表达式               
	 * 相同的转换规则。对 FuncCall 节点，结果将是     
	 * 函数名，但语法也可能交回其他类型的节点。        
	 *            
	 *
	 * 必须现在就取得该信息，因为 FigureColname 只能         
	 * 处理原始解析树。至于拿这些名字做什么则由          
	 * addRangeTableEntryForFunction 
	 *
	 * 同样，若存在列定义列表也一并收集。但           
	 * 若这里发现一个且 RangeFunction 也有，则报错。   
	 */
	foreach(lc, r->functions)
	{
		List	   *pair = (List *) lfirst(lc);
		Node	   *fexpr;
		List	   *coldeflist;
		Node	   *newfexpr;
		Node	   *last_srf;

		/* 拆解函数调用/列定义列表对               */
		Assert(list_length(pair) == 2);
		fexpr = (Node *) linitial(pair);
		coldeflist = (List *) lsecond(pair);

		/*
		 * 若发现带多个参数且无特殊修饰的 unnest()             
		 * 函数调用，就把它转换为分别作用于各参数的       
		 * 独立的 unnest() 调用。这肯定是个杂凑手段，但总比
		 * 实现 SQL 标准 UNNEST() 语法的其他办法要好。   
		 *
		 * 若存在任何修饰（含 coldeflist），就不转换，  
		 * 这多半意味着稍后报 no-such-function 错误。我们       
		 * 本可以立刻就抛错，但那似乎没多大用处。          
		 *                                                                
		 * 若有人使用这类修饰，那就不是在用 SQL 标准     
		 * 语法，而是想要未改动的调用。   
		 *
		 * 注意：该转换会把不带模式限定的 unnest()          
		 * 函数名变成带模式限定的 pg_catalog.unnest()。      
		 * 这个选择也有点可争议，但在做这种转换时       
		 * 强制使用内建的 unnest() 看来是合理的。      
		 */
		if (IsA(fexpr, FuncCall))
		{
			FuncCall   *fc = (FuncCall *) fexpr;

			if (list_length(fc->funcname) == 1 &&
				strcmp(strVal(linitial(fc->funcname)), "unnest") == 0 &&
				list_length(fc->args) > 1 &&
				fc->agg_order == NIL &&
				fc->agg_filter == NULL &&
				fc->over == NULL &&
				!fc->agg_star &&
				!fc->agg_distinct &&
				!fc->func_variadic &&
				coldeflist == NIL)
			{
				ListCell   *lc2;

				foreach(lc2, fc->args)
				{
					Node	   *arg = (Node *) lfirst(lc2);
					FuncCall   *newfc;

					last_srf = pstate->p_last_srf;

					newfc = makeFuncCall(SystemFuncName("unnest"),
										 list_make1(arg),
										 COERCE_EXPLICIT_CALL,
										 fc->location);

					newfexpr = transformExpr(pstate, (Node *) newfc,
											 EXPR_KIND_FROM_FUNCTION);

					/* nodeFunctionscan.c 需要 SRF 位于顶层          */
					if (pstate->p_last_srf != last_srf &&
						pstate->p_last_srf != newfexpr)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("set-returning functions must appear at top level of FROM"),
								 parser_errposition(pstate,
													exprLocation(pstate->p_last_srf))));

					funcexprs = lappend(funcexprs, newfexpr);

					funcnames = lappend(funcnames,
										FigureColname((Node *) newfc));

					/* coldeflist 为空，因此不可能出错    */

					coldeflists = lappend(coldeflists, coldeflist);
				}
				continue;		/* 此函数项处理完毕     */
			}
		}

		/* 常规情形... */
		last_srf = pstate->p_last_srf;

		newfexpr = transformExpr(pstate, fexpr,
								 EXPR_KIND_FROM_FUNCTION);

		/* nodeFunctionscan.c 需要 SRF 位于顶层          */
		if (pstate->p_last_srf != last_srf &&
			pstate->p_last_srf != newfexpr)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-returning functions must appear at top level of FROM"),
					 parser_errposition(pstate,
										exprLocation(pstate->p_last_srf))));

		funcexprs = lappend(funcexprs, newfexpr);

		funcnames = lappend(funcnames,
							FigureColname(fexpr));

		if (coldeflist && r->coldeflist)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("multiple column definition lists are not allowed for the same function"),
					 parser_errposition(pstate,
										exprLocation((Node *) r->coldeflist))));

		coldeflists = lappend(coldeflists, coldeflist);
	}

	pstate->p_lateral_active = false;

	/*
	 * 必须现在就指定排序规则，使 RTE 能为从它创建的 Var  
	 * 暴露正确排序规则信息
	 */
	assign_list_collations(pstate, funcexprs);

	/*
	 * 若存在顶层 coldeflist 就安装它（我们已检查过         
	 * 不存在与之冲突的逐函数 coldeflist）。     
	 *
	 * 仅当只有单个函数时才允许这样做（即便经过        
	 * UNNEST 展开）且没有 WITH ORDINALITY。后一条        
	 * 限制的理由是：序数列到底该不该放进                  
	 * coldeflist 并不清楚，而且用户在任一方向上              
	 * 出错的几率都太高。把 coldeflist 放进                  
	 * ROWS FROM() 里则清晰得多。   
	 */
	if (r->coldeflist)
	{
		if (list_length(funcexprs) != 1)
		{
			if (r->is_rowsfrom)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("ROWS FROM() with multiple functions cannot have a column definition list"),
						 errhint("Put a separate column definition list for each function inside ROWS FROM()."),
						 parser_errposition(pstate,
											exprLocation((Node *) r->coldeflist))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("UNNEST() with multiple arguments cannot have a column definition list"),
						 errhint("Use separate UNNEST() calls inside ROWS FROM(), and attach a column definition list to each one."),
						 parser_errposition(pstate,
											exprLocation((Node *) r->coldeflist))));
		}
		if (r->ordinality)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("WITH ORDINALITY cannot be used with a column definition list"),
					 errhint("Put the column definition list inside ROWS FROM()."),
					 parser_errposition(pstate,
										exprLocation((Node *) r->coldeflist))));

		coldeflists = list_make1(r->coldeflist);
	}

	/*
	 * 若用户显式写了 LATERAL，或其中存在任何横向       
	 * 交叉引用，则 RTE 标为 LATERAL。     
	 */
	is_lateral = r->lateral || contain_vars_of_level((Node *) funcexprs, 0);

	/*
	 * 好，为该函数构建 RTE 和 nsitem。   
	 */
	return addRangeTableEntryForFunction(pstate,
										 funcnames, funcexprs, coldeflists,
										 r, is_lateral, true);
}

/*
 * transformRangeTableFunc -
 *			把原始 RangeTableFunc 转成 TableFunc。  
 *
 * 转换命名空间子句、生成文档的表达式、生成            
 * 行的表达式、生成列的表达式以及默认值               
 * 表达式。              
 */
static ParseNamespaceItem *
transformRangeTableFunc(ParseState *pstate, RangeTableFunc *rtf)
{
	TableFunc  *tf = makeNode(TableFunc);
	const char *constructName;
	Oid			docType;
	bool		is_lateral;
	ListCell   *col;
	char	  **names;
	int			colno;

	/* 当前仅支持 XMLTABLE             */
	constructName = "XMLTABLE";
	docType = XMLOID;

	/*
	 * 无论 RangeTableFunc 是否显式标记为 LATERAL，              
	 * 我们都让本层的 lateral_only 名字可见。这是            
	 * 符合 SQL 标准所必需；对 FROM 中的所有函数       
	 * 亦较便利。   
	 *
	 * （LATERAL 无法嵌套于单个 pstate 层内，因此无需     
	 * 保存恢复。）       
	 */
	Assert(!pstate->p_lateral_active);
	pstate->p_lateral_active = true;

	/* 转换行生成表达式并对其实施类型转换...            */
	Assert(rtf->rowexpr != NULL);
	tf->rowexpr = coerce_to_specific_type(pstate,
										  transformExpr(pstate, rtf->rowexpr, EXPR_KIND_FROM_FUNCTION),
										  TEXTOID,
										  constructName);
	assign_expr_collations(pstate, tf->rowexpr);

	/* ...以及对文档本身       */
	Assert(rtf->docexpr != NULL);
	tf->docexpr = coerce_to_specific_type(pstate,
										  transformExpr(pstate, rtf->docexpr, EXPR_KIND_FROM_FUNCTION),
										  docType,
										  constructName);
	assign_expr_collations(pstate, tf->docexpr);

	/* 未定义的序数列号       */
	tf->ordinalitycol = -1;

	/* 处理列规范      */
	names = palloc(sizeof(char *) * list_length(rtf->columns));

	colno = 0;
	foreach(col, rtf->columns)
	{
		RangeTableFuncCol *rawc = (RangeTableFuncCol *) lfirst(col);
		Oid			typid;
		int32		typmod;
		Node	   *colexpr;
		Node	   *coldefexpr;
		int			j;

		tf->colnames = lappend(tf->colnames,
							   makeString(pstrdup(rawc->colname)));

		/*
		 * 确定新列的类型和 typmod。FOR ORDINALITY 列            
		 * 按标准是 INTEGER；其余列由用户指定。          
		 */
		if (rawc->for_ordinality)
		{
			if (tf->ordinalitycol != -1)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("only one FOR ORDINALITY column is allowed"),
						 parser_errposition(pstate, rawc->location)));

			typid = INT4OID;
			typmod = -1;
			tf->ordinalitycol = colno;
		}
		else
		{
			if (rawc->typeName->setof)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("column \"%s\" cannot be declared SETOF",
								rawc->colname),
						 parser_errposition(pstate, rawc->location)));

			typenameTypeIdAndMod(pstate, rawc->typeName,
								 &typid, &typmod);
		}

		tf->coltypes = lappend_oid(tf->coltypes, typid);
		tf->coltypmods = lappend_int(tf->coltypmods, typmod);
		tf->colcollations = lappend_oid(tf->colcollations,
										get_typcollation(typid));

		/* 转换 PATH 与 DEFAULT 表达式          */
		if (rawc->colexpr)
		{
			colexpr = coerce_to_specific_type(pstate,
											  transformExpr(pstate, rawc->colexpr,
															EXPR_KIND_FROM_FUNCTION),
											  TEXTOID,
											  constructName);
			assign_expr_collations(pstate, colexpr);
		}
		else
			colexpr = NULL;

		if (rawc->coldefexpr)
		{
			coldefexpr = coerce_to_specific_type_typmod(pstate,
														transformExpr(pstate, rawc->coldefexpr,
																	  EXPR_KIND_FROM_FUNCTION),
														typid, typmod,
														constructName);
			assign_expr_collations(pstate, coldefexpr);
		}
		else
			coldefexpr = NULL;

		tf->colexprs = lappend(tf->colexprs, colexpr);
		tf->coldefexprs = lappend(tf->coldefexprs, coldefexpr);

		if (rawc->is_not_null)
			tf->notnulls = bms_add_member(tf->notnulls, colno);

		/* 确保列名唯一                */
		for (j = 0; j < colno; j++)
			if (strcmp(names[j], rawc->colname) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("column name \"%s\" is not unique",
								rawc->colname),
						 parser_errposition(pstate, rawc->location)));
		names[colno] = rawc->colname;

		colno++;
	}
	pfree(names);

	/* 命名空间（若有）也需要转换         */
	if (rtf->namespaces != NIL)
	{
		ListCell   *ns;
		ListCell   *lc2;
		List	   *ns_uris = NIL;
		List	   *ns_names = NIL;
		bool		default_ns_seen = false;

		foreach(ns, rtf->namespaces)
		{
			ResTarget  *r = (ResTarget *) lfirst(ns);
			Node	   *ns_uri;

			Assert(IsA(r, ResTarget));
			ns_uri = transformExpr(pstate, r->val, EXPR_KIND_FROM_FUNCTION);
			ns_uri = coerce_to_specific_type(pstate, ns_uri,
											 TEXTOID, constructName);
			assign_expr_collations(pstate, ns_uri);
			ns_uris = lappend(ns_uris, ns_uri);

			/* 校验名字列表：无重复，仅一个 DEFAULT          */
			if (r->name != NULL)
			{
				foreach(lc2, ns_names)
				{
					String	   *ns_node = lfirst_node(String, lc2);

					if (ns_node == NULL)
						continue;
					if (strcmp(strVal(ns_node), r->name) == 0)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("namespace name \"%s\" is not unique",
										r->name),
								 parser_errposition(pstate, r->location)));
				}
			}
			else
			{
				if (default_ns_seen)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("only one default namespace is allowed"),
							 parser_errposition(pstate, r->location)));
				default_ns_seen = true;
			}

			/* 我们用空指针表示 DEFAULT       */
			ns_names = lappend(ns_names,
							   r->name ? makeString(r->name) : NULL);
		}

		tf->ns_uris = ns_uris;
		tf->ns_names = ns_names;
	}

	tf->location = rtf->location;

	pstate->p_lateral_active = false;

	/*
	 * 若用户显式写了 LATERAL，或其中存在任何横向       
	 * 交叉引用，则 RTE 标为 LATERAL。     
	 */
	is_lateral = rtf->lateral || contain_vars_of_level((Node *) tf, 0);

	return addRangeTableEntryForTableFunc(pstate,
										  tf, rtf->alias, is_lateral, true);
}

/*
 * transformRangeTableSample --- 转换 TABLESAMPLE 子句     
 *
 * 调用者已转换过 rts->relation，我们只需校验              
 * 其余字段并创建 TableSampleClause 节点。        
 */
static TableSampleClause *
transformRangeTableSample(ParseState *pstate, RangeTableSample *rts)
{
	TableSampleClause *tablesample;
	Oid			handlerOid;
	Oid			funcargtypes[1];
	TsmRoutine *tsm;
	List	   *fargs;
	ListCell   *larg,
			   *ltyp;

	/*
	 * 为校验采样方法名，查找处理函数，该函数与           
	 * 方法同名，带一个哑 INTERNAL 参数，结果类型为       
	 * tsm_handler。（注意：SQL 标准中 tablesample 方法名         
	 * 不带模式限定；但对我们而言它们只是函数，          
	 * 故允许用模式限定来消除潜在歧义。）      
	 */
	funcargtypes[0] = INTERNALOID;

	handlerOid = LookupFuncName(rts->method, 1, funcargtypes, true);

	/* 我们要让错误报告说方法不存在，而非函数不存在   */
	if (!OidIsValid(handlerOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablesample method %s does not exist",
						NameListToString(rts->method)),
				 parser_errposition(pstate, rts->location)));

	/* 检查处理函数返回类型是否正确 */
	if (get_func_rettype(handlerOid) != TSM_HANDLEROID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("function %s must return type %s",
						NameListToString(rts->method), "tsm_handler"),
				 parser_errposition(pstate, rts->location)));

	/* 好，运行处理函数以取得 TsmRoutine 及参数类型  */
	tsm = GetTsmRoutine(handlerOid);

	tablesample = makeNode(TableSampleClause);
	tablesample->tsmhandler = handlerOid;

	/* 检查用户是否提供了预期数目的参数     */
	if (list_length(rts->args) != list_length(tsm->parameterTypes))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLESAMPLE_ARGUMENT),
				 errmsg_plural("tablesample method %s requires %d argument, not %d",
							   "tablesample method %s requires %d arguments, not %d",
							   list_length(tsm->parameterTypes),
							   NameListToString(rts->method),
							   list_length(tsm->parameterTypes),
							   list_length(rts->args)),
				 parser_errposition(pstate, rts->location)));

	/*
	 * 转换各参数并按需做类型转换。注意我们还必须        
	 * 现在就指定排序规则，因为 assign_query_collations()  
	 * 不检查 RTE 的子结构。    
	 */
	fargs = NIL;
	forboth(larg, rts->args, ltyp, tsm->parameterTypes)
	{
		Node	   *arg = (Node *) lfirst(larg);
		Oid			argtype = lfirst_oid(ltyp);

		arg = transformExpr(pstate, arg, EXPR_KIND_FROM_FUNCTION);
		arg = coerce_to_specific_type(pstate, arg, argtype, "TABLESAMPLE");
		assign_expr_collations(pstate, arg);
		fargs = lappend(fargs, arg);
	}
	tablesample->args = fargs;

	/* 处理 REPEATABLE 种子  */
	if (rts->repeatable != NULL)
	{
		Node	   *arg;

		if (!tsm->repeatable_across_queries)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("tablesample method %s does not support REPEATABLE",
							NameListToString(rts->method)),
					 parser_errposition(pstate, rts->location)));

		arg = transformExpr(pstate, rts->repeatable, EXPR_KIND_FROM_FUNCTION);
		arg = coerce_to_specific_type(pstate, arg, FLOAT8OID, "REPEATABLE");
		assign_expr_collations(pstate, arg);
		tablesample->repeatable = (Expr *) arg;
	}
	else
		tablesample->repeatable = NULL;

	return tablesample;
}

/*
 * getNSItemForSpecialRelationTypes
 *
 * 若给定 RangeVar 引用 CTE 或 EphemeralNamedRelation，    
 * 则构建并返回相应 ParseNamespaceItem，否则返回 NULL          
 */
static ParseNamespaceItem *
getNSItemForSpecialRelationTypes(ParseState *pstate, RangeVar *rv)
{
	ParseNamespaceItem *nsitem;
	CommonTableExpr *cte;
	Index		levelsup;

	/*
	 * 若是带限定的名字，就不可能是 CTE 或 tuplestore 引用
	 */
	if (rv->schemaname)
		return NULL;

	cte = scanNameSpaceForCTE(pstate, rv->relname, &levelsup);
	if (cte)
		nsitem = addRangeTableEntryForCTE(pstate, cte, levelsup, rv, true);
	else if (scanNameSpaceForENR(pstate, rv->relname))
		nsitem = addRangeTableEntryForENR(pstate, rv, true);
	else
		nsitem = NULL;

	return nsitem;
}

/*
 * transformFromClauseItem -
 *	  转换一个 FROM 子句项，把所需的条目加入          
 *	  ParseState 中正在构建的范围表列表，并返回      
 *	  转换后的项，以便加入 joinlist。同时构建一个    
 *	  描述该项所暴露名字的 ParseNamespaceItem 列表。       
 *	  本例程可递归处理 SQL92 的 JOIN 表达式。      
 *
 * 函数返回值是加入连接树的节点（一个            
 * RangeTblRef 或 JoinExpr）。其他输出参数有：      
 *
 * *top_nsitem：接收与连接树项直接对应的                        
 * ParseNamespaceItem。（仅供内部递归，外部调用不用）  
 *                  
 *
 * *namespace：接收一个 List，内含为该项暴露的 RTE          
 * 构建的 ParseNamespaceItem（这些项中的 lateral_only                
 * 标志是不确定的，调用者使用前必须显式设置。）       
 */
static Node *
transformFromClauseItem(ParseState *pstate, Node *n,
						ParseNamespaceItem **top_nsitem,
						List **namespace)
{
	/* 防止子树过深导致栈溢出                       */
	check_stack_depth();

	if (IsA(n, RangeVar))
	{
		/* 普通关系引用，也可能是 CTE 引用         */
		RangeVar   *rv = (RangeVar *) n;
		RangeTblRef *rtr;
		ParseNamespaceItem *nsitem;

		/* 检查是否为 CTE 或 tuplestore 引用   */
		nsitem = getNSItemForSpecialRelationTypes(pstate, rv);

		/* 若上面没找到，必是表引用          */
		if (!nsitem)
			nsitem = transformTableEntry(pstate, rv);

		*top_nsitem = nsitem;
		*namespace = list_make1(nsitem);
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = nsitem->p_rtindex;
		return (Node *) rtr;
	}
	else if (IsA(n, RangeSubselect))
	{
		/* 子 SELECT 如同普通关系       */
		RangeTblRef *rtr;
		ParseNamespaceItem *nsitem;

		nsitem = transformRangeSubselect(pstate, (RangeSubselect *) n);
		*top_nsitem = nsitem;
		*namespace = list_make1(nsitem);
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = nsitem->p_rtindex;
		return (Node *) rtr;
	}
	else if (IsA(n, RangeFunction))
	{
		/* 函数如同普通关系          */
		RangeTblRef *rtr;
		ParseNamespaceItem *nsitem;

		nsitem = transformRangeFunction(pstate, (RangeFunction *) n);
		*top_nsitem = nsitem;
		*namespace = list_make1(nsitem);
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = nsitem->p_rtindex;
		return (Node *) rtr;
	}
	else if (IsA(n, RangeTableFunc))
	{
		/* 表函数如同普通关系             */
		RangeTblRef *rtr;
		ParseNamespaceItem *nsitem;

		nsitem = transformRangeTableFunc(pstate, (RangeTableFunc *) n);
		*top_nsitem = nsitem;
		*namespace = list_make1(nsitem);
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = nsitem->p_rtindex;
		return (Node *) rtr;
	}
	else if (IsA(n, RangeTableSample))
	{
		/* TABLESAMPLE 子句（包裹其他有效的 FROM 节点）*/
		RangeTableSample *rts = (RangeTableSample *) n;
		Node	   *rel;
		RangeTblEntry *rte;

		/* 递归转换其中包含的关系            */
		rel = transformFromClauseItem(pstate, rts->relation,
									  top_nsitem, namespace);
		rte = (*top_nsitem)->p_rte;
		/* 我们仅对普通关系和物化视图支持它     */
		if (rte->rtekind != RTE_RELATION ||
			(rte->relkind != RELKIND_RELATION &&
			 rte->relkind != RELKIND_MATVIEW &&
			 rte->relkind != RELKIND_PARTITIONED_TABLE))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("TABLESAMPLE clause can only be applied to tables and materialized views"),
					 parser_errposition(pstate, exprLocation(rts->relation))));

		/* 转换 TABLESAMPLE 细节并挂到 RTE 上          */
		rte->tablesample = transformRangeTableSample(pstate, rts);
		return rel;
	}
	else if (IsA(n, JoinExpr))
	{
		/* 一种新式连接表达式  */
		JoinExpr   *j = (JoinExpr *) n;
		ParseNamespaceItem *nsitem;
		ParseNamespaceItem *l_nsitem;
		ParseNamespaceItem *r_nsitem;
		List	   *l_namespace,
				   *r_namespace,
				   *my_namespace,
				   *l_colnames,
				   *r_colnames,
				   *res_colnames,
				   *l_colnos,
				   *r_colnos,
				   *res_colvars;
		ParseNamespaceColumn *l_nscolumns,
				   *r_nscolumns,
				   *res_nscolumns;
		int			res_colindex;
		bool		lateral_ok;
		int			sv_namespace_length;
		int			k;

		/*
		 * 先递归处理左子树，再处理右子树。我们必须     
		 * 按此顺序进行，LATERAL 引用的可见性才正确。   
		 */
		j->larg = transformFromClauseItem(pstate, j->larg,
										  &l_nsitem,
										  &l_namespace);

		/*
		 * 通过把左侧 RTE 临时加入 pstate 的命名空间列表  
		 * 让右侧可以经 LATERAL 访问这些 RTE。按 SQL:2008，   
		 * 若连接类型非 INNER 或 LEFT，左侧名字仍须             
		 * 暴露，但引用它们则报错（设计很蠢，但规  
		 * 范就是如此。）因此我们总会把它们压入命名      
		 * 空间，jointype不对时就标为非                      
		 *  lateral_ok。                      
		 *
		 * 注意，我们并不需要合并后的命名空间列表   
		 * 无冲突。详见 scanNameSpaceForRefname() 的注释。      
		 */
		lateral_ok = (j->jointype == JOIN_INNER || j->jointype == JOIN_LEFT);
		setNamespaceLateralState(l_namespace, true, lateral_ok);

		sv_namespace_length = list_length(pstate->p_namespace);
		pstate->p_namespace = list_concat(pstate->p_namespace, l_namespace);

		/* 现在可以处理右侧了    */
		j->rarg = transformFromClauseItem(pstate, j->rarg,
										  &r_nsitem,
										  &r_namespace);

		/* 再次从命名空间列表中移除左侧 RTE          */
		pstate->p_namespace = list_truncate(pstate->p_namespace,
											sv_namespace_length);

		/*
		 * 检查左右子树中是否存在冲突的引用名。必须      
		 * 这样做，因为上层会假定我交回的是自洽的 
		 * 命名空间列表。     
		 */
		checkNameSpaceConflicts(pstate, l_namespace, r_namespace);

		/*
		 * 生成合并的命名空间信息，供下文使用。  
		 */
		my_namespace = list_concat(l_namespace, r_namespace);

		/*
		 * 我们将基于各输入 nsitem 的 nscolumns 数据与           
		 * eref 别名列名开展工作。注意其中包含被      
		 * 删除的列，这很有用，因为物理输入列号更容   
		 * 易跟踪。                     
		 */
		l_nscolumns = l_nsitem->p_nscolumns;
		l_colnames = l_nsitem->p_names->colnames;
		r_nscolumns = r_nsitem->p_nscolumns;
		r_colnames = r_nsitem->p_names->colnames;

		/*
		 * 自然连接不显式指定列；必须生成要连接的列。
		 * 需要遍历每个表或连接结果的列列表，并            
		 * 匹配列名。使用第一个表，并检查第二个表中     
		 * 的每一列是否有匹配。                                
		 * （稍后会检查匹配是否唯一。）这一步的结果      
		 * 是一个列名列表，就像显式写出的 USING 列表         
		 * 一样。  
		 */
		if (j->isNatural)
		{
			List	   *rlist = NIL;
			ListCell   *lx,
					   *rx;

			Assert(j->usingClause == NIL);	/* 不应同时有 USING()    */

			foreach(lx, l_colnames)
			{
				char	   *l_colname = strVal(lfirst(lx));
				String	   *m_name = NULL;

				if (l_colname[0] == '\0')
					continue;	/* 忽略已删除的列  */

				foreach(rx, r_colnames)
				{
					char	   *r_colname = strVal(lfirst(rx));

					if (strcmp(l_colname, r_colname) == 0)
					{
						m_name = makeString(l_colname);
						break;
					}
				}

				/* 匹配到右列？若是则保留为连接列...    */
				if (m_name != NULL)
					rlist = lappend(rlist, m_name);
			}

			j->usingClause = rlist;
		}

		/*
		 * 若指定了 USING 别名，则把 USING 列保存为           
		 * 其列列表    
		 */
		if (j->join_using_alias)
			j->join_using_alias->colnames = j->usingClause;

		/*
		 * 现在转换连接限定条件（如有）。 
		 */
		l_colnos = NIL;
		r_colnos = NIL;
		res_colnames = NIL;
		res_colvars = NIL;

		/* 它可能比实际需要的大，但没必要做得那么精确*/
		res_nscolumns = (ParseNamespaceColumn *)
			palloc0((list_length(l_colnames) + list_length(r_colnames)) *
					sizeof(ParseNamespaceColumn));
		res_colindex = 0;

		if (j->usingClause)
		{
			/*
			 * JOIN/USING（或经上文转换的 NATURAL JOIN）。        
			 * 把列表转换为显式 ON 条件。  
			 */
			List	   *ucols = j->usingClause;
			List	   *l_usingvars = NIL;
			List	   *r_usingvars = NIL;
			ListCell   *ucol;

			Assert(j->quals == NULL);	/* 不应同时有 ON()    */

			foreach(ucol, ucols)
			{
				char	   *u_colname = strVal(lfirst(ucol));
				ListCell   *col;
				int			ndx;
				int			l_index = -1;
				int			r_index = -1;
				Var		   *l_colvar,
						   *r_colvar;

				Assert(u_colname[0] != '\0');

				/* 检查 USING(foo,foo)    */
				foreach(col, res_colnames)
				{
					char	   *res_colname = strVal(lfirst(col));

					if (strcmp(res_colname, u_colname) == 0)
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_COLUMN),
								 errmsg("column name \"%s\" appears more than once in USING clause",
										u_colname)));
				}

				/* 在左输入中查找 */
				ndx = 0;
				foreach(col, l_colnames)
				{
					char	   *l_colname = strVal(lfirst(col));

					if (strcmp(l_colname, u_colname) == 0)
					{
						if (l_index >= 0)
							ereport(ERROR,
									(errcode(ERRCODE_AMBIGUOUS_COLUMN),
									 errmsg("common column name \"%s\" appears more than once in left table",
											u_colname)));
						l_index = ndx;
					}
					ndx++;
				}
				if (l_index < 0)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" specified in USING clause does not exist in left table",
									u_colname)));
				l_colnos = lappend_int(l_colnos, l_index + 1);

				/* 在右输入中查找  */
				ndx = 0;
				foreach(col, r_colnames)
				{
					char	   *r_colname = strVal(lfirst(col));

					if (strcmp(r_colname, u_colname) == 0)
					{
						if (r_index >= 0)
							ereport(ERROR,
									(errcode(ERRCODE_AMBIGUOUS_COLUMN),
									 errmsg("common column name \"%s\" appears more than once in right table",
											u_colname)));
						r_index = ndx;
					}
					ndx++;
				}
				if (r_index < 0)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" specified in USING clause does not exist in right table",
									u_colname)));
				r_colnos = lappend_int(r_colnos, r_index + 1);

				/* 构建用于生成的 JOIN ON 子句的 Var 节点*/
				l_colvar = buildVarFromNSColumn(pstate, l_nscolumns + l_index);
				l_usingvars = lappend(l_usingvars, l_colvar);
				r_colvar = buildVarFromNSColumn(pstate, r_nscolumns + r_index);
				r_usingvars = lappend(r_usingvars, r_colvar);

				/*
				 * 趁此顺便把列名加入 res_colnames 列表。    
				 * 在这里做有点难看，因为对应的      
				 * res_colvars 条目要到后面才创建，                
				 * 但放到后面做需要对 usingClause 列表            
				 * 多做一次遍历。   
				 */
				res_colnames = lappend(res_colnames, lfirst(ucol));
			}

			/* 构建生成的 JOIN ON 子句         */
			j->quals = transformJoinUsingClause(pstate,
												l_usingvars,
												r_usingvars);
		}
		else if (j->quals)
		{
			/* 用户手写的 ON 条件；转换之   */
			j->quals = transformJoinOnClause(pstate, j, my_namespace);
		}
		else
		{
			/* CROSS JOIN 无条件 */
		}

		/*
		 * 如果这是外连接，现在把相应的子 RTE 标记为     
		 * 会被本连接置空。子连接表达式已经处理完毕，  
		 * 本连接的限定条件也已处理，它们处理的都是       
		 * 未被置空的输入列。此后对这些 RTE 的引用将        
		 * 看到可能被置空的值，并且应把生成的 Var 标记  
		 * 得顾及这点。特别是，即将构建的连接别名       
		 * Var 应当反映本连接的置空效果。                    
		 *
		 * 这样做的难点在于我们需要连接的 RT 索引，       
		 * 而它尚未被正式确定。不过，在此处与               
		 * addRangeTableEntryForJoin 调用之间不会有别的 RTE，       
		 * 因此可以预测其分配结果。（另一种做法是先调用 
		 * addRangeTableEntryForJoin，即在算出全部数据之前，      
		 * 但那更丑。）    
		 */
		j->rtindex = list_length(pstate->p_rtable) + 1;

		switch (j->jointype)
		{
			case JOIN_INNER:
				break;
			case JOIN_LEFT:
				markRelsAsNulledBy(pstate, j->rarg, j->rtindex);
				break;
			case JOIN_FULL:
				markRelsAsNulledBy(pstate, j->larg, j->rtindex);
				markRelsAsNulledBy(pstate, j->rarg, j->rtindex);
				break;
			case JOIN_RIGHT:
				markRelsAsNulledBy(pstate, j->larg, j->rtindex);
				break;
			default:
				/* 此处不应出现其他类型     */
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				break;
		}

		/*
		 * 现在可以为 USING 列构建连接别名表达式了。        
		 */
		if (j->usingClause)
		{
			ListCell   *lc1,
					   *lc2;

			/* 扫描 colnos 列表以恢复前一个循环中的信息     */
			forboth(lc1, l_colnos, lc2, r_colnos)
			{
				int			l_index = lfirst_int(lc1) - 1;
				int			r_index = lfirst_int(lc2) - 1;
				Var		   *l_colvar,
						   *r_colvar;
				Node	   *u_colvar;
				ParseNamespaceColumn *res_nscolumn;

				/*
				 * 注意我们要重建这些 Var：它们可能与     
				 * 上一轮所建 Var 的 varnullingrels 不同。       
				 */
				l_colvar = buildVarFromNSColumn(pstate, l_nscolumns + l_index);
				r_colvar = buildVarFromNSColumn(pstate, r_nscolumns + r_index);

				/* 为该列构建连接别名 Var              */
				u_colvar = buildMergedJoinVar(pstate,
											  j->jointype,
											  l_colvar,
											  r_colvar);
				res_colvars = lappend(res_colvars, u_colvar);

				/* 构建该列的 res_nscolumns[] 条目   */
				res_nscolumn = res_nscolumns + res_colindex;
				res_colindex++;
				if (u_colvar == (Node *) l_colvar)
				{
					/* 合并列等价于左输入               */
					*res_nscolumn = l_nscolumns[l_index];
				}
				else if (u_colvar == (Node *) r_colvar)
				{
					/* 合并列等价于右输入                */
					*res_nscolumn = r_nscolumns[r_index];
				}
				else
				{
					/*
					 * 合并列在语义上不等价于任何               
					 * 个输入，所以需按连接输出列来引用它   
					 *        
					 */
					res_nscolumn->p_varno = j->rtindex;
					res_nscolumn->p_varattno = res_colindex;
					res_nscolumn->p_vartype = exprType(u_colvar);
					res_nscolumn->p_vartypmod = exprTypmod(u_colvar);
					res_nscolumn->p_varcollid = exprCollation(u_colvar);
					res_nscolumn->p_varnosyn = j->rtindex;
					res_nscolumn->p_varattnosyn = res_colindex;
				}
			}
		}

		/* 把各侧其余的列加入输出列                       */
		res_colindex +=
			extractRemainingColumns(pstate,
									l_nscolumns, l_colnames, &l_colnos,
									&res_colnames, &res_colvars,
									res_nscolumns + res_colindex);
		res_colindex +=
			extractRemainingColumns(pstate,
									r_nscolumns, r_colnames, &r_colnos,
									&res_colnames, &res_colvars,
									res_nscolumns + res_colindex);

		/* 若连接有别名，它在语法上隐藏全部输入  */
		if (j->alias)
		{
			for (k = 0; k < res_colindex; k++)
			{
				ParseNamespaceColumn *nscol = res_nscolumns + k;

				nscol->p_varnosyn = j->rtindex;
				nscol->p_varattnosyn = k + 1;
			}
		}

		/*
		 * 现在为连接的结果构建 RTE 和 nsitem。       
		 */
		nsitem = addRangeTableEntryForJoin(pstate,
										   res_colnames,
										   res_nscolumns,
										   j->jointype,
										   list_length(j->usingClause),
										   res_colvars,
										   l_colnos,
										   r_colnos,
										   j->join_using_alias,
										   j->alias,
										   true);

		/* 验证我们正确预测了连接的 RT 索引         */
		Assert(j->rtindex == nsitem->p_rtindex);
		/* 同时交叉检查列数           */
		Assert(res_colindex == list_length(nsitem->p_names->colnames));

		/*
		 * 把指向 JoinExpr 的链接存进 p_joinexprs 相应元素。   
		 * 由于我们惰性地维护该列表，可能需要先填入      
		 * 空条目，然后才能把 JoinExpr 加入正确位置。      
		 */
		for (k = list_length(pstate->p_joinexprs) + 1; k < j->rtindex; k++)
			pstate->p_joinexprs = lappend(pstate->p_joinexprs, NULL);
		pstate->p_joinexprs = lappend(pstate->p_joinexprs, j);
		Assert(list_length(pstate->p_joinexprs) == j->rtindex);

		/*
		 * 若连接有 USING 别名，则为其构建 ParseNamespaceItem， 
		 * 并把它加入连接输入的 nsitem 列表。       
		 */
		if (j->join_using_alias)
		{
			ParseNamespaceItem *jnsitem;

			jnsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
			jnsitem->p_names = j->join_using_alias;
			jnsitem->p_rte = nsitem->p_rte;
			jnsitem->p_rtindex = nsitem->p_rtindex;
			jnsitem->p_perminfo = NULL;
			/* 无需复制前 N 列，直接使用 res_nscolumns 即可   */
			jnsitem->p_nscolumns = res_nscolumns;
			/* 设置默认可见性标志；后面可能被改变   */
			jnsitem->p_rel_visible = true;
			jnsitem->p_cols_visible = true;
			jnsitem->p_lateral_only = false;
			jnsitem->p_lateral_ok = true;
			/* 按 SQL 规范，必须检查别名冲突  */
			checkNameSpaceConflicts(pstate, list_make1(jnsitem), my_namespace);
			my_namespace = lappend(my_namespace, jnsitem);
		}

		/*
		 * 准备返回的命名空间列表。若 JOIN 有别名，则它   
		 * 会完全隐藏其包含的 RTE；否则，所包含的 RTE       
		 * 作为表名仍可见，但对于                        
		 * 无限定列名不可访问。 
		 *
		 * 注意：若存在嵌套的无别名 JOIN，较低层的        
		 * JOIN 仍留在列表中，尽管既未设 p_rel_visible         
		 * 也没设 p_cols_visible。我们可以删除这些列表项，   
		 * 但这样做是否值得花代价并不明确。  
		 */
		if (j->alias != NULL)
			my_namespace = NIL;
		else
			setNamespaceColumnVisibility(my_namespace, false);

		/*
		 * 连接 RTE 本身总是对不带限定的                        
		 * 列名可见。只有带别名时才可作为关系名可见。 
		 */
		nsitem->p_rel_visible = (j->alias != NULL);
		nsitem->p_cols_visible = true;
		nsitem->p_lateral_only = false;
		nsitem->p_lateral_ok = true;

		*top_nsitem = nsitem;
		*namespace = lappend(my_namespace, nsitem);

		return (Node *) j;
	}
	else
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(n));
	return NULL;				/* 不应到此，为安抚编译器   */
}

/*
 * buildVarFromNSColumn -
 *	  用 ParseNamespaceColumn 数据构建 Var       
 *
 * 它用于构建 joinaliasvars 条目。         
 * 可以假定 varlevelsup 应为 0，且不指定位置。           
 * 还要注意这里不请求列上的 SELECT 权限；只有当         
 * 该列实际被查询引用时才会发生。                 
 */
static Var *
buildVarFromNSColumn(ParseState *pstate, ParseNamespaceColumn *nscol)
{
	Var		   *var;

	Assert(nscol->p_varno > 0); /* 即非已删除列       */
	var = makeVar(nscol->p_varno,
				  nscol->p_varattno,
				  nscol->p_vartype,
				  nscol->p_vartypmod,
				  nscol->p_varcollid,
				  0);
	/* makeVar 没有为这些提供参数，因此手工设置： */
	var->varnosyn = nscol->p_varnosyn;
	var->varattnosyn = nscol->p_varattnosyn;

	/* ... 并更新 varnullingrels  */
	markNullableIfNeeded(pstate, var);

	return var;
}

/*
 * buildMergedJoinVar -
 *	  为合并的连接列生成合适的替换表达式                
 */
static Node *
buildMergedJoinVar(ParseState *pstate, JoinType jointype,
				   Var *l_colvar, Var *r_colvar)
{
	Oid			outcoltype;
	int32		outcoltypmod;
	Node	   *l_node,
			   *r_node,
			   *res_node;

	outcoltype = select_common_type(pstate,
									list_make2(l_colvar, r_colvar),
									"JOIN/USING",
									NULL);
	outcoltypmod = select_common_typmod(pstate,
										list_make2(l_colvar, r_colvar),
										outcoltype);

	/*
	 * 如需要则插入强制转换函数。注意 typmod 上的差异     
	 * 只在输入有 typmod 而 outcoltypmod 为 -1 时发生。那种  
	 * 情况会插入 RelabelType，明确表明结果的 typmod          
	 * 与输入不同。无需 coerce_type_typmod。       
	 */
	if (l_colvar->vartype != outcoltype)
		l_node = coerce_type(pstate, (Node *) l_colvar, l_colvar->vartype,
							 outcoltype, outcoltypmod,
							 COERCION_IMPLICIT, COERCE_IMPLICIT_CAST, -1);
	else if (l_colvar->vartypmod != outcoltypmod)
		l_node = (Node *) makeRelabelType((Expr *) l_colvar,
										  outcoltype, outcoltypmod,
										  InvalidOid,	/* 稍后修正*/
										  COERCE_IMPLICIT_CAST);
	else
		l_node = (Node *) l_colvar;

	if (r_colvar->vartype != outcoltype)
		r_node = coerce_type(pstate, (Node *) r_colvar, r_colvar->vartype,
							 outcoltype, outcoltypmod,
							 COERCION_IMPLICIT, COERCE_IMPLICIT_CAST, -1);
	else if (r_colvar->vartypmod != outcoltypmod)
		r_node = (Node *) makeRelabelType((Expr *) r_colvar,
										  outcoltype, outcoltypmod,
										  InvalidOid,	/* 稍后修正*/
										  COERCE_IMPLICIT_CAST);
	else
		r_node = (Node *) r_colvar;

	/*
	 * 选择输出什么 
	 */
	switch (jointype)
	{
		case JOIN_INNER:

			/*
			 * 两个 var 都可用；如有未强转的则优先用它。
			 */
			if (IsA(l_node, Var))
				res_node = l_node;
			else if (IsA(r_node, Var))
				res_node = r_node;
			else
				res_node = l_node;
			break;
		case JOIN_LEFT:
			/* 总是用左 var    */
			res_node = l_node;
			break;
		case JOIN_RIGHT:
			/* 总是用右 var     */
			res_node = r_node;
			break;
		case JOIN_FULL:
			{
				/*
				 * 这里我们必须构建 COALESCE 表达式，以保证    
				 * 任一输入非空则连接输出非空。 
				 */
				CoalesceExpr *c = makeNode(CoalesceExpr);

				c->coalescetype = outcoltype;
				/* coalescecollid 将在后面设置 */
				c->args = list_make2(l_node, r_node);
				c->location = -1;
				res_node = (Node *) c;
				break;
			}
		default:
			elog(ERROR, "unrecognized join type: %d", (int) jointype);
			res_node = NULL;	/* 安抚编译器     */
			break;
	}

	/*
	 * 应用 assign_expr_collations 来修正强制转换与          
	 * CoalesceExpr 的 collation 信息（若创建过）。现在须做，  
	 * 以便连接节点别名 var 显示正确 collation 信息。  
	 */
	assign_expr_collations(pstate, res_node);

	return res_node;
}

/*
 * markRelsAsNulledBy -
 *	  把给定 jointree 节点及其子节点标为被连接 jindex 置空 
 */
static void
markRelsAsNulledBy(ParseState *pstate, Node *n, int jindex)
{
	int			varno;
	ListCell   *lc;

	/* 注意：此处见不到 FromExpr*/
	if (IsA(n, RangeTblRef))
	{
		varno = ((RangeTblRef *) n)->rtindex;
	}
	else if (IsA(n, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) n;

		/* 递归到子节点  */
		markRelsAsNulledBy(pstate, j->larg, jindex);
		markRelsAsNulledBy(pstate, j->rarg, jindex);
		varno = j->rtindex;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(n));
		varno = 0;				/* 安抚编译器     */
	}

	/*
	 * 现在把 jindex 加到关系 varno 的 p_nullingrels                
	 * 集合。我们惰性维护 p_nullingrels 列表，可能需要      
	 * 扩展使 varno 项存在。  
	 */
	while (list_length(pstate->p_nullingrels) < varno)
		pstate->p_nullingrels = lappend(pstate->p_nullingrels, NULL);
	lc = list_nth_cell(pstate->p_nullingrels, varno - 1);
	lfirst(lc) = bms_add_member((Bitmapset *) lfirst(lc), jindex);
}

/*
 * setNamespaceColumnVisibility -
 *	  更新命名空间列表中 cols_visible 标志的便捷子例程。    
 */
static void
setNamespaceColumnVisibility(List *namespace, bool cols_visible)
{
	ListCell   *lc;

	foreach(lc, namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(lc);

		nsitem->p_cols_visible = cols_visible;
	}
}

/*
 * setNamespaceLateralState -
 *	  更新命名空间列表中 LATERAL 标志的便捷子例程。    
 */
static void
setNamespaceLateralState(List *namespace, bool lateral_only, bool lateral_ok)
{
	ListCell   *lc;

	foreach(lc, namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(lc);

		nsitem->p_lateral_only = lateral_only;
		nsitem->p_lateral_ok = lateral_ok;
	}
}


/*
 * transformWhereClause -
 *	  转换限定条件，并确保其类型为 boolean。           
 *	  用于 WHERE 及类似子句。   
 *
 * constructName 不影响语义，但用于错误消息。                  
 */
Node *
transformWhereClause(ParseState *pstate, Node *clause,
					 ParseExprKind exprKind, const char *constructName)
{
	Node	   *qual;

	if (clause == NULL)
		return NULL;

	qual = transformExpr(pstate, clause, exprKind);

	qual = coerce_to_boolean(pstate, qual, constructName);

	return qual;
}


/*
 * transformLimitClause -
 *	  转换表达式，并确保其类型为 bigint。           
 *	  用于 LIMIT 及类似子句。   
 *
 * 注意：从 Postgres 8.2 起，LIMIT 表达式应产出 int8，       
 * 而非以前的 int4。    
 *
 * constructName 不影响语义，但用于错误消息。                  
 */
Node *
transformLimitClause(ParseState *pstate, Node *clause,
					 ParseExprKind exprKind, const char *constructName,
					 LimitOption limitOption)
{
	Node	   *qual;

	if (clause == NULL)
		return NULL;

	qual = transformExpr(pstate, clause, exprKind);

	qual = coerce_to_specific_type(pstate, qual, INT8OID, constructName);

	/* LIMIT 无法引用当前查询的任何变量           */
	checkExprIsVarFree(pstate, qual, constructName);

	/*
	 * 不允许 FETCH FIRST .. WITH TIES 中出现 NULL。此检查丑陋  
	 * 且简单：把 NULL 藏入表达式仍可传入；但它能保护   
	 * ruleutils 不发出光秃的 NULL，                                   
	 * 因为语法不接受回读这样的 NULL。           
	 */
	if (exprKind == EXPR_KIND_LIMIT && limitOption == LIMIT_OPTION_WITH_TIES &&
		IsA(clause, A_Const) && castNode(A_Const, clause)->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ROW_COUNT_IN_LIMIT_CLAUSE),
				 errmsg("row count cannot be null in FETCH FIRST ... WITH TIES clause")));

	return qual;
}

/*
 * checkExprIsVarFree
 *		检查给定表达式不含当前查询层的 Var           
 *		（聚集和窗口函数应当已被拒绝）。                    
 *
 * 它用于检查必须在查询所有行上取值一致的表达式， 
 * 比如 LIMIT。严格说来它也应拒绝易变函数，                
 * 但实际没这么做：函数首次执行得到什么值，        
 * 你得到的就是什么。                       
 *
 * constructName 不影响语义，但用于错误消息。                  
 */
static void
checkExprIsVarFree(ParseState *pstate, Node *n, const char *constructName)
{
	if (contain_vars_of_level(n, 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
		/* translator: %s is name of a SQL construct, eg LIMIT */
				 errmsg("argument of %s must not contain variables",
						constructName),
				 parser_errposition(pstate,
									locate_var_of_level(n, 0))));
	}
}


/*
 * checkTargetlistEntrySQL92 -
 *	  校验 findTargetlistEntrySQL92 找到的目标列表条目  
 *
 * 我们因 "GROUP BY 1" 这类语法选中既有 tlist 条目时，  
 * 必须确保它在所指的子句类型中可用；transformExpr()   
 * 会把它当作普通目标列表条目来处理。                     
 *                 
 */
static void
checkTargetlistEntrySQL92(ParseState *pstate, TargetEntry *tle,
						  ParseExprKind exprKind)
{
	switch (exprKind)
	{
		case EXPR_KIND_GROUP_BY:
			/* 拒绝聚集和窗口函数            */
			if (pstate->p_hasAggs &&
				contain_aggs_of_level((Node *) tle->expr, 0))
				ereport(ERROR,
						(errcode(ERRCODE_GROUPING_ERROR),
				/* translator: %s is name of a SQL construct, eg GROUP BY */
						 errmsg("aggregate functions are not allowed in %s",
								ParseExprKindName(exprKind)),
						 parser_errposition(pstate,
											locate_agg_of_level((Node *) tle->expr, 0))));
			if (pstate->p_hasWindowFuncs &&
				contain_windowfuncs((Node *) tle->expr))
				ereport(ERROR,
						(errcode(ERRCODE_WINDOWING_ERROR),
				/* translator: %s is name of a SQL construct, eg GROUP BY */
						 errmsg("window functions are not allowed in %s",
								ParseExprKindName(exprKind)),
						 parser_errposition(pstate,
											locate_windowfunc((Node *) tle->expr))));
			break;
		case EXPR_KIND_ORDER_BY:
			/* 无需额外检查     */
			break;
		case EXPR_KIND_DISTINCT_ON:
			/* 无需额外检查     */
			break;
		default:
			elog(ERROR, "unexpected exprKind in checkTargetlistEntrySQL92");
			break;
	}
}

/*
 *	findTargetlistEntrySQL92 -
 *	  返回与给定（未转换）节点匹配的目标列表条目。   
 *	  若无匹配项，则新建 resjunk 节点追加到目标列表。    
 *	                           
 *
 * 本函数支持旧的 SQL92 ORDER BY 解释，其中表达式是       
 * 输出列名或编号。若找不到这类匹配，则落入        
 * SQL99 规则。出于历史原因，Postgres 也允许 GROUP BY        
 * 使用这种解释，尽管标准从未如此。                      
 * 不过对 GROUP BY 我们优先采用 SQL99 匹配。                  
 * 本函数*不*用于 WINDOW 定义。              
 *
 * node		待匹配的 ORDER BY、GROUP BY 或 DISTINCT ON 表达式     
 * tlist	目标列表（按引用传入，以便追加）            
 * exprKind 标识正在处理的子句类型     
 */
static TargetEntry *
findTargetlistEntrySQL92(ParseState *pstate, Node *node, List **tlist,
						 ParseExprKind exprKind)
{
	ListCell   *tl;

	/*----------
	 * 按 SQL92 规范的要求处理两个特例：         
	 *
	 * 1. 裸列名（无限定符或下标）        
	 *	  对于裸标识符，我们在既有目标列表中查找  
	 *	  匹配的列名。多个匹配是错误，                 
	 *	  除非它们引用相同的值；例如，         
	 *	  允许	SELECT a, a FROM table ORDER BY a  
	 *	  拒绝	SELECT a AS b, b FROM table ORDER BY b 
	 *	  若找不到匹配，则按普通表达式处理标识符     
	 *	                   
	 *	  对 GROUP BY 而言，把分组项与目标列表条目         
	 *	  匹配是不正确的：按 SQL92，GROUP BY 中的标识符     
	 *	  引用的是 FROM 暴露的列名，而非目标列表列。    
	 *	  不过许多实现（包括 7.0 前的                        
	 *	  PostgreSQL）都接受这种做法。所以对 GROUP BY，     
	 *	  我们先看标识符是否匹配任何 FROM 列名，只有   
	 *	  不匹配时才试目标列表名。这确保了在名称     
	 *	  两者皆可的情况下遵循规范。                     
	 *	  DISTINCT ON 不在标准中，因此我们可以随意处理；    
	 *	  我们选择让它像 ORDER BY 那样工作，理由相当   
	 *	  牵强：普通 DISTINCT 就作用于目标列表条目。 
	 *
	 * 2. 整数常量   
	 *	  这表示使用既有目标列表的第 n 项。            
	 *	  注意按真正的常量排序/分组/去重毫无意义，   
	 *	  因此这与 SQL99 并不冲突。                             
	 *	  SQL92 不允许 GROUP BY 列编号，但由于           
	 *	  标准对该语法没有定义其他行为，              
	 *	  我们不妨接受这一常见扩展。     
	 *
	 * 注意：两种情况下都不得使用既有 resjunk 目标，        
	 * 因为用户未在 SELECT 列表中写出它们。  
	 *
	 * 若两个特例都不适用，则按 SQL99 把该项当作        
	 * 表达式处理。      
	 *----------
	 */
	if (IsA(node, ColumnRef) &&
		list_length(((ColumnRef *) node)->fields) == 1 &&
		IsA(linitial(((ColumnRef *) node)->fields), String))
	{
		char	   *name = strVal(linitial(((ColumnRef *) node)->fields));
		int			location = ((ColumnRef *) node)->location;

		if (exprKind == EXPR_KIND_GROUP_BY)
		{
			/*
			 * GROUP BY 中，我们必须优先选择与 FROM 子句   
			 * 列的匹配而非目标列表的匹配。看看是否存在  
			 * 匹配的列。若有，则落入以使用 SQL99 规则。 
			 * 注意：如果该名称有歧义地引用多个             
			 * FROM 暴露的列名，colNameToVar 会 ereport(ERROR)。那  
			 * 正合我们所需。  
			 *
			 * 7.4.3 的小调整：忽略上层查询层中的匹配。   
			 * 这实际上把裸名的搜索顺序改为 (1) 本地          
			 * FROM 变量，(2) 本地目标列表别名，(3) 外层      
			 * FROM 变量，而之前是 (1) (3) (2)。SQL92 与          
			 * SQL99 不允许按外层引用分组，因此这          
			 * 不会破坏规范允许的情形，而且看起来是      
			 * 更自洽的行为。    
			 */
			if (colNameToVar(pstate, name, true, location) != NULL)
				name = NULL;
		}

		if (name != NULL)
		{
			TargetEntry *target_result = NULL;

			foreach(tl, *tlist)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tl);

				if (!tle->resjunk &&
					strcmp(tle->resname, name) == 0)
				{
					if (target_result != NULL)
					{
						if (!equal(target_result->expr, tle->expr))
							ereport(ERROR,
									(errcode(ERRCODE_AMBIGUOUS_COLUMN),

							/*------
							  translator: first %s is name of a SQL construct, eg ORDER BY */
									 errmsg("%s \"%s\" is ambiguous",
											ParseExprKindName(exprKind),
											name),
									 parser_errposition(pstate, location)));
					}
					else
						target_result = tle;
					/* 留在循环中以检查歧义      */
				}
			}
			if (target_result != NULL)
			{
				/* 经适当校验后返回第一个匹配           */
				checkTargetlistEntrySQL92(pstate, target_result, exprKind);
				return target_result;
			}
		}
	}
	if (IsA(node, A_Const))
	{
		A_Const    *aconst = castNode(A_Const, node);
		int			targetlist_pos = 0;
		int			target_pos;

		if (!IsA(&aconst->val, Integer))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			/* translator: %s is name of a SQL construct, eg ORDER BY */
					 errmsg("non-integer constant in %s",
							ParseExprKindName(exprKind)),
					 parser_errposition(pstate, aconst->location)));

		target_pos = intVal(&aconst->val);
		foreach(tl, *tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tl);

			if (!tle->resjunk)
			{
				if (++targetlist_pos == target_pos)
				{
					/* 经适当校验后返回唯一匹配               */
					checkTargetlistEntrySQL92(pstate, tle, exprKind);
					return tle;
				}
			}
		}
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
		/* translator: %s is name of a SQL construct, eg ORDER BY */
				 errmsg("%s position %d is not in select list",
						ParseExprKindName(exprKind), target_pos),
				 parser_errposition(pstate, aconst->location)));
	}

	/*
	 * 否则我们得到表达式，因此按 SQL99 规则处理。   
	 */
	return findTargetlistEntrySQL99(pstate, node, tlist, exprKind);
}

/*
 *	findTargetlistEntrySQL99 -
 *	  返回与给定（未转换）节点匹配的目标列表条目。   
 *	  若无匹配项，则新建 resjunk 节点追加到目标列表。    
 *	                           
 *
 * 本函数支持 SQL99 解释，其中表达式只是引用             
 * 输入列名的普通表达式。                             
 *
 * node		待匹配的 ORDER BY、GROUP BY 等表达式       
 * tlist	目标列表（按引用传入，以便追加）            
 * exprKind 标识正在处理的子句类型     
 */
static TargetEntry *
findTargetlistEntrySQL99(ParseState *pstate, Node *node, List **tlist,
						 ParseExprKind exprKind)
{
	TargetEntry *target_result;
	ListCell   *tl;
	Node	   *expr;

	/*
	 * 把未转换节点转换为已转换的表达式，并在 tlist       
	 * 中搜索匹配。注意：是否有多个匹配并不重要。        
	 * 另外，这里也愿意匹配既有的 resjunk 目标，         
	 * 尽管上面的 SQL92 情形必须忽略 resjunk 目标。           
	 *         
	 */
	expr = transformExpr(pstate, node, exprKind);

	foreach(tl, *tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);
		Node	   *texpr;

		/*
		 * 忽略既有 tlist 表达式上的隐式类型转换。   
		 *
		 * 这实质上允许 ORDER/GROUP 等项采用与文本等价的     
		 * tlist 项先前选定的相同数据类型。                      
		 * 普通 SELECT tlist 在这一阶段的顶层不可能有隐式     
		 * 转换，但聚集函数中的 ORDER BY 确实会出现         
		 * 这种情况。    
		 */
		texpr = strip_implicit_coercions((Node *) tle->expr);

		if (equal(expr, texpr))
			return tle;
	}

	/*
	 * 如果没有匹配，则构建一个新目标条目追加到        
	 * 目标列表末尾。该目标被置 resjunk = true，以便它        
	 * 不会被投影到最终元组中。       
	 */
	target_result = transformTargetEntry(pstate, node, expr, exprKind,
										 NULL, true);

	*tlist = lappend(*tlist, target_result);

	return target_result;
}

/*-------------------------------------------------------------------------
 * 展平分组列表中带括号的子列表，以及某些嵌套     
 * 分组集的情形。   
 *
 * 在分组集（ROLLUP、CUBE 或 GROUPING SETS）内部，           
 * 我们期望内容嵌套不超过 2 层：即 ROLLUP((a,b),(c,d)) 是
 * 合法的，但 ROLLUP((a,(b,c)),d) 会被展平为 ((a,b,c),d)，    
 * 再规范化为 ((a,b,c),(d))。   
 *
 * CUBE 或 ROLLUP 可以嵌套在 GROUPING SETS 内（反之不行），  
 * 发现这种情况时我们不作处理。但若看到 GROUPING SETS     
 * 嵌于 GROUPING SETS 中，可如下展平规范化： 
 *	 GROUPING SETS (a, (b,c), GROUPING SETS ((c,d),(e)), (f,g))
 * 变为 
 *	 GROUPING SETS ((a), (b,c), (c,d), (e), (f,g))
 *
 * 这符合规范的语法变换，但这些是我们在解析分析        
 * 中仅做的变换，这样查询在反解析时能尽可能       
 * 保留 CUBE 和 ROLLUP 原本指定的分组集语法。               
 * （把结果完整展开成分组集列表的工作留给           
 * 规划器。）                       
 *
 * 完成后，结果列表应只包含以下可能的元素：          
 *          
 *	 - 表达式    
 *	 - 嵌套 2 层表达式列表的 CUBE 或 ROLLUP           
 *	 - GROUPING SET，可包含：      
 *		- 表达式列表 
 *		- 空分组集       
 *		- 列表嵌套 2 层的 CUBE/ROLLUP 节点     
 * 返回的是新列表，但除 GroupingSet 节点外不会              
 * 深拷贝旧节点
 *
 * 作为副作用，标记列表是否含 GroupingSet 节点。     
 *-------------------------------------------------------------------------
 */
static Node *
flatten_grouping_sets(Node *expr, bool toplevel, bool *hasGroupingSets)
{
	/* 以防出现病态输入           */
	check_stack_depth();

	if (expr == (Node *) NIL)
		return (Node *) NIL;

	switch (expr->type)
	{
		case T_RowExpr:
			{
				RowExpr    *r = (RowExpr *) expr;

				if (r->row_format == COERCE_IMPLICIT_CAST)
					return flatten_grouping_sets((Node *) r->args,
												 false, NULL);
			}
			break;
		case T_GroupingSet:
			{
				GroupingSet *gset = (GroupingSet *) expr;
				ListCell   *l2;
				List	   *result_set = NIL;

				if (hasGroupingSets)
					*hasGroupingSets = true;

				/*
				 * 在顶层跳过所有空分组集；若无剩余内容，  
				 * 调用者可提供规范的 GROUP BY ()。               
				 *      
				 */

				if (toplevel && gset->kind == GROUPING_SET_EMPTY)
					return (Node *) NIL;

				foreach(l2, gset->content)
				{
					Node	   *n1 = lfirst(l2);
					Node	   *n2 = flatten_grouping_sets(n1, false, NULL);

					if (IsA(n1, GroupingSet) &&
						((GroupingSet *) n1)->kind == GROUPING_SET_SETS)
						result_set = list_concat(result_set, (List *) n2);
					else
						result_set = lappend(result_set, n2);
				}

				/*
				 * 在顶层，保留分组集节点；但如果我们在     
				 * 嵌套分组集中，简单嵌套时需要把展平结果
				 * 拼接进外层列表。                         
				 */

				if (toplevel || (gset->kind != GROUPING_SET_SETS))
				{
					return (Node *) makeGroupingSet(gset->kind, result_set, gset->location);
				}
				else
					return (Node *) result_set;
			}
		case T_List:
			{
				List	   *result = NIL;
				ListCell   *l;

				foreach(l, (List *) expr)
				{
					Node	   *n = flatten_grouping_sets(lfirst(l), toplevel, hasGroupingSets);

					if (n != (Node *) NIL)
					{
						if (IsA(n, List))
							result = list_concat(result, (List *) n);
						else
							result = lappend(result, n);
					}
				}

				return (Node *) result;
			}
		default:
			break;
	}

	return expr;
}

/*
 * 转换 GROUP BY 子句或分组集内的单个表达式。             
 *
 * 若目标列表中尚无该表达式，则把它加入目标列表；       
 * 若 flatresult 列表（将变成 groupClause）中也没有，           
 * 则加入其中。sortClause 提供运算符与排序次序提示。  
 *
 * 返回该表达式的 ressortgroupref。      
 *
 * flatresult	SortGroupClause 节点扁平列表的引用    
 * seen_local	本地层已见过的 sortgroupref 的 bitmapset          
 * pstate		ParseState
 * gexpr		待转换节点  
 * targetlist	TargetEntry 列表的引用  
 * sortClause	ORDER BY 子句 (SortGroupClause)      
 * exprKind		表达式种类
 * useSQL99		SQL99 而非 SQL92 语法     
 * toplevel		在分组集内则为 false     
 */
static Index
transformGroupClauseExpr(List **flatresult, Bitmapset *seen_local,
						 ParseState *pstate, Node *gexpr,
						 List **targetlist, List *sortClause,
						 ParseExprKind exprKind, bool useSQL99, bool toplevel)
{
	TargetEntry *tle;
	bool		found = false;

	if (useSQL99)
		tle = findTargetlistEntrySQL99(pstate, gexpr,
									   targetlist, exprKind);
	else
		tle = findTargetlistEntrySQL92(pstate, gexpr,
									   targetlist, exprKind);

	if (tle->ressortgroupref > 0)
	{
		ListCell   *sl;

		/*
		 * 消除重复项（GROUP BY x, x），但只在本地层。   
		 * （分组集中的重复项会影响返回行数，           
		 * 因此无法随意丢弃。）              
		 *
		 * 由于我们只关心 sortgroupref，所以可以用              
		 * bitmapset 而不必扫描列表。         
		 */
		if (bms_is_member(tle->ressortgroupref, seen_local))
			return 0;

		/*
		 * 如果我们已在扁平子句列表中，就无需考虑          
		 * 再次加入自己。  
		 */
		found = targetIsInSortList(tle, InvalidOid, *flatresult);
		if (found)
			return tle->ressortgroupref;

		/*
		 * 如果 GROUP BY 目标列表条目也出现在 ORDER BY 中，     
		 * 则从（首个）匹配的 ORDER BY 项复制操作符信息。 
		 * 这意味着如果写 "GROUP BY foo ORDER BY foo USING <<<"，     
		 * GROUP BY 操作会悄然采用该 ORDER BY 隐含的相等语义。
		 * 这样做有两个理由：一是更有可能用单个排序步骤 
		 * 同时实现 GROUP BY 和 ORDER BY，二是让用户能够选择   
		 * GROUP BY 所用的相等语义（倘若用户所用数据类型   
		 * 具有多个相等操作符的话）。                         
		 *                                 
		 *
		 * 不过，在分组集内，我们强制将请求的排序设为    
		 * NULLS LAST，因为只要还有希望用排序聚合来完成      
		 * 这项工作，就会在相应分组后附加生成 NULL 值。
		 * 如果用户要求 NULL 在前，就难免再来一轮排序， 
		 *                                                            
		 * 归规划器管。
		 */

		foreach(sl, sortClause)
		{
			SortGroupClause *sc = (SortGroupClause *) lfirst(sl);

			if (sc->tleSortGroupRef == tle->ressortgroupref)
			{
				SortGroupClause *grpc = copyObject(sc);

				if (!toplevel)
					grpc->nulls_first = false;
				*flatresult = lappend(*flatresult, grpc);
				found = true;
				break;
			}
		}
	}

	/*
	 * 若在 ORDER BY 中无匹配，就用默认排序/分组语义  
	 * 把它加入结果。
	 */
	if (!found)
		*flatresult = addTargetToGroupList(pstate, tle,
										   *flatresult, *targetlist,
										   exprLocation(gexpr));

	/*
	 * 至此必已有代码为我们分配了 sortgroupref……
	 */

	return tle->ressortgroupref;
}

/*
 * 转换 GROUP BY 子句或分组集内的表达式列表。               
 *
 * 该表达式列表属于单个子句，其中的重复项                 
 * 可被安全地消除。 
 *
 * 返回 ressortgroupref 值组成的整数列表。
 *
 * flatresult SortGroupClause 节点扁平列表的引用    
 * pstate ParseState 
 * list 待转换的节点  
 * targetlist TargetEntry 列表的引用  
 * sortClause ORDER BY 子句(SortGroupClause 节点)
 * exprKind 表达式种类 
 * useSQL99 用 SQL99 而非 SQL92 语法  
 * toplevel 在任何分组集内时为 false
 */
static List *
transformGroupClauseList(List **flatresult,
						 ParseState *pstate, List *list,
						 List **targetlist, List *sortClause,
						 ParseExprKind exprKind, bool useSQL99, bool toplevel)
{
	Bitmapset  *seen_local = NULL;
	List	   *result = NIL;
	ListCell   *gl;

	foreach(gl, list)
	{
		Node	   *gexpr = (Node *) lfirst(gl);

		Index		ref = transformGroupClauseExpr(flatresult,
												   seen_local,
												   pstate,
												   gexpr,
												   targetlist,
												   sortClause,
												   exprKind,
												   useSQL99,
												   toplevel);

		if (ref > 0)
		{
			seen_local = bms_add_member(seen_local, ref);
			result = lappend_int(result, ref);
		}
	}

	return result;
}

/*
 * 转换一个分组集及其（递归地）内容。    
 *
 * 该分组集可能是 GROUPING SETS 节点，内部还嵌有其他     
 * 分组集，但 SETS 套 SETS 在到达这里之前已被展平。    
 *               
 *
 * 返回转换后的节点，它现在包含 SIMPLE 节点，其内是    
 * ressortgroupref 列表而非表达式。    
 *
 * flatresult SortGroupClause 节点扁平列表的引用    
 * pstate ParseState 
 * gset 待转换的分组集      
 * targetlist TargetEntry 列表的引用  
 * sortClause ORDER BY 子句(SortGroupClause 节点)
 * exprKind 表达式种类 
 * useSQL99 用 SQL99 而非 SQL92 语法  
 * toplevel 在任何分组集内时为 false
 */
static Node *
transformGroupingSet(List **flatresult,
					 ParseState *pstate, GroupingSet *gset,
					 List **targetlist, List *sortClause,
					 ParseExprKind exprKind, bool useSQL99, bool toplevel)
{
	ListCell   *gl;
	List	   *content = NIL;

	Assert(toplevel || gset->kind != GROUPING_SET_SETS);

	foreach(gl, gset->content)
	{
		Node	   *n = lfirst(gl);

		if (IsA(n, List))
		{
			List	   *l = transformGroupClauseList(flatresult,
													 pstate, (List *) n,
													 targetlist, sortClause,
													 exprKind, useSQL99, false);

			content = lappend(content, makeGroupingSet(GROUPING_SET_SIMPLE,
													   l,
													   exprLocation(n)));
		}
		else if (IsA(n, GroupingSet))
		{
			GroupingSet *gset2 = (GroupingSet *) lfirst(gl);

			content = lappend(content, transformGroupingSet(flatresult,
															pstate, gset2,
															targetlist, sortClause,
															exprKind, useSQL99, false));
		}
		else
		{
			Index		ref = transformGroupClauseExpr(flatresult,
													   NULL,
													   pstate,
													   n,
													   targetlist,
													   sortClause,
													   exprKind,
													   useSQL99,
													   false);

			content = lappend(content, makeGroupingSet(GROUPING_SET_SIMPLE,
													   list_make1_int(ref),
													   exprLocation(n)));
		}
	}

	/* 任意为 CUBE 的大小设上限，其增长呈指数级      */
	if (gset->kind == GROUPING_SET_CUBE)
	{
		if (list_length(content) > 12)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("CUBE is limited to 12 elements"),
					 parser_errposition(pstate, gset->location)));
	}

	return (Node *) makeGroupingSet(gset->kind, content, gset->location);
}


/*
 * transformGroupClause -
 *	  转换 GROUP BY 子句     
 *
 * GROUP BY 项会被加入目标列表（作为 resjunk 列），    
 * 若尚不存在则加入，因此目标列表必须按引用传递。 
 *
 * 它也用于窗口 PARTITION BY 子句（行为几乎相同，         
 * 但总是按 SQL99 规则解释）。             
 *
 * 分组集让它比过去复杂得多。我们的目标有两重：      
 * 一是建立 SortGroupClause 节点的扁平列表，引用每个     
 * 用于分组的去重表达式，并把那些表达式按需加入        
 * 目标列表。与此同时，我们构建 groupingSets 树，         
 * 其中在 GroupingSet 节点内仅以整数列表存储 ressortgroupref     
 * （可能嵌套，但深度有限：一个 GROUPING_SET_SETS                
 * 节点，可含 SIMPLE、CUBE 或 ROLLUP，多余集合会被展平；  
 * 而 CUBE 与 ROLLUP 只能包含 SIMPLE 节点）。      
 *
 * 若无分组集，可跳过大部分繁琐工作。         
 *
 * 一个微妙之处是：groupClause 列表最终可能为空，     
 * 而 groupingSets 列表却不为空；这发生在只有空分组      
 * 集或显式 GROUP BY () 时。这与指定                             
 * 聚合或带 HAVING 子句而无 GROUP BY 的情形等效；此时       
 * 输入为空时每组也输出一行。 
 *
 * 返回转换后（扁平）groupClause。  
 *
 * pstate ParseState 
 * grouplist 待转换的子句 
 * groupingSets 引用，指向容纳分组集树的列表        
 * targetlist TargetEntry 列表的引用  
 * sortClause ORDER BY 子句(SortGroupClause 节点)
 * exprKind 表达式种类 
 * useSQL99 用 SQL99 而非 SQL92 语法  
 */
List *
transformGroupClause(ParseState *pstate, List *grouplist, List **groupingSets,
					 List **targetlist, List *sortClause,
					 ParseExprKind exprKind, bool useSQL99)
{
	List	   *result = NIL;
	List	   *flat_grouplist;
	List	   *gsets = NIL;
	ListCell   *gl;
	bool		hasGroupingSets = false;
	Bitmapset  *seen_local = NULL;

	/*
	 * 递归展平隐式 RowExpr。（严格来说，只有 GROUP BY         
	 * 因分组集语法规则才需要，不过我们照样执行。）
	 *         
	 */
	flat_grouplist = (List *) flatten_grouping_sets((Node *) grouplist,
													true,
													&hasGroupingSets);

	/*
	 * 如果列表现在为空，但 hasGroupingSets 为真，那是因为  
	 * 我们剔除了冗余空分组集。恢复单个空分组集以保持
	 * 规范形式：GROUP BY ()                
	 */

	if (flat_grouplist == NIL && hasGroupingSets)
	{
		flat_grouplist = list_make1(makeGroupingSet(GROUPING_SET_EMPTY,
													NIL,
													exprLocation((Node *) grouplist)));
	}

	foreach(gl, flat_grouplist)
	{
		Node	   *gexpr = (Node *) lfirst(gl);

		if (IsA(gexpr, GroupingSet))
		{
			GroupingSet *gset = (GroupingSet *) gexpr;

			switch (gset->kind)
			{
				case GROUPING_SET_EMPTY:
					gsets = lappend(gsets, gset);
					break;
				case GROUPING_SET_SIMPLE:
					/* 不会发生 */
					Assert(false);
					break;
				case GROUPING_SET_SETS:
				case GROUPING_SET_CUBE:
				case GROUPING_SET_ROLLUP:
					gsets = lappend(gsets,
									transformGroupingSet(&result,
														 pstate, gset,
														 targetlist, sortClause,
														 exprKind, useSQL99, true));
					break;
			}
		}
		else
		{
			Index		ref = transformGroupClauseExpr(&result, seen_local,
													   pstate, gexpr,
													   targetlist, sortClause,
													   exprKind, useSQL99, true);

			if (ref > 0)
			{
				seen_local = bms_add_member(seen_local, ref);
				if (hasGroupingSets)
					gsets = lappend(gsets,
									makeGroupingSet(GROUPING_SET_SIMPLE,
													list_make1_int(ref),
													exprLocation(gexpr)));
			}
		}
	}

	/* 解析器应阻止此情况*/
	Assert(gsets == NIL || groupingSets != NULL);

	if (groupingSets)
		*groupingSets = gsets;

	return result;
}

/*
 * transformSortClause -
 *	  转换 ORDER BY 子句      
 *
 * ORDER BY 项会被加入目标列表（作为 resjunk 列），    
 * 若尚不存在则加入，因此目标列表必须按引用传递。 
 *
 * 它也用于窗口和聚合的 ORDER BY 子句（行为几乎相同，
 * 但总是按 SQL99 规则解释）。                        
 */
List *
transformSortClause(ParseState *pstate,
					List *orderlist,
					List **targetlist,
					ParseExprKind exprKind,
					bool useSQL99)
{
	List	   *sortlist = NIL;
	ListCell   *olitem;

	foreach(olitem, orderlist)
	{
		SortBy	   *sortby = (SortBy *) lfirst(olitem);
		TargetEntry *tle;

		if (useSQL99)
			tle = findTargetlistEntrySQL99(pstate, sortby->node,
										   targetlist, exprKind);
		else
			tle = findTargetlistEntrySQL92(pstate, sortby->node,
										   targetlist, exprKind);

		sortlist = addTargetToSortList(pstate, tle,
									   sortlist, *targetlist, sortby);
	}

	return sortlist;
}

/*
 * transformWindowDefinitions -
 *		转换窗口定义（WindowDef 转为 WindowClause）   
 */
List *
transformWindowDefinitions(ParseState *pstate,
						   List *windowdefs,
						   List **targetlist)
{
	List	   *result = NIL;
	Index		winref = 0;
	ListCell   *lc;

	foreach(lc, windowdefs)
	{
		WindowDef  *windef = (WindowDef *) lfirst(lc);
		WindowClause *refwc = NULL;
		List	   *partitionClause;
		List	   *orderClause;
		Oid			rangeopfamily = InvalidOid;
		Oid			rangeopcintype = InvalidOid;
		WindowClause *wc;

		winref++;

		/*
		 * 检查重复的窗口名。      
		 */
		if (windef->name &&
			findWindowClause(result, windef->name) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WINDOWING_ERROR),
					 errmsg("window \"%s\" is already defined", windef->name),
					 parser_errposition(pstate, windef->location)));

		/*
		 * 若引用了先前窗口，则查找它。       
		 */
		if (windef->refname)
		{
			refwc = findWindowClause(result, windef->refname);
			if (refwc == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("window \"%s\" does not exist",
								windef->refname),
						 parser_errposition(pstate, windef->location)));
		}

		/*
		 * 转换 PARTITION 与 ORDER 规格（如有）。对其         
		 * 的处理方式与顶层 GROUP BY 和 ORDER BY 子句几乎 
		 * 一样，包括对非默认操作符语义的特殊处理。    
		 */
		orderClause = transformSortClause(pstate,
										  windef->orderClause,
										  targetlist,
										  EXPR_KIND_WINDOW_ORDER,
										  true /* 强制SQL99规则 */ );
		partitionClause = transformGroupClause(pstate,
											   windef->partitionClause,
											   NULL,
											   targetlist,
											   orderClause,
											   EXPR_KIND_WINDOW_PARTITION,
											   true /* 强制SQL99规则 */ );

		/*
		 * 再准备新的 WindowClause。  
		 */
		wc = makeNode(WindowClause);
		wc->name = windef->name;
		wc->refname = windef->refname;

		/*
		 * 按规范，引用前一个 windowdef 的窗口定义会复制 
		 * 前者的分区子句（且不得指定自己的）。它可以 
		 * 指定自己的排序子句，但前提是前一个没有排序  
		 * 子句。它总是指定自己的帧子句，且前一个不得  
		 * 已有帧子句。没错，这几种情形的处理各不相同，
		 * 确实很怪，但 SQL:2008 就这样规定；参见         
		 * 7.11 <window clause> 语法规则 10 与通用规则 1。帧 
		 * 子句规则尤其怪异：它让 "OVER foo" 与 "OVER       
		 * （foo）"不同，并需要后者在 foo 带非默认帧子句 
		 * 时抛错。对此我们无需深究缘由，但我们确实  
		 * 特意为此类情形抛出有用的错误消息。            
		 *                        
		 */
		if (refwc)
		{
			if (partitionClause)
				ereport(ERROR,
						(errcode(ERRCODE_WINDOWING_ERROR),
						 errmsg("cannot override PARTITION BY clause of window \"%s\"",
								windef->refname),
						 parser_errposition(pstate, windef->location)));
			wc->partitionClause = copyObject(refwc->partitionClause);
		}
		else
			wc->partitionClause = partitionClause;
		if (refwc)
		{
			if (orderClause && refwc->orderClause)
				ereport(ERROR,
						(errcode(ERRCODE_WINDOWING_ERROR),
						 errmsg("cannot override ORDER BY clause of window \"%s\"",
								windef->refname),
						 parser_errposition(pstate, windef->location)));
			if (orderClause)
			{
				wc->orderClause = orderClause;
				wc->copiedOrder = false;
			}
			else
			{
				wc->orderClause = copyObject(refwc->orderClause);
				wc->copiedOrder = true;
			}
		}
		else
		{
			wc->orderClause = orderClause;
			wc->copiedOrder = false;
		}
		if (refwc && refwc->frameOptions != FRAMEOPTION_DEFAULTS)
		{
			/*
			 * 如果这是 WINDOW 子句，或者 OVER 子句包含 ORDER    
			 * BY 或帧子句，就使用这条消息。（我们已经     
			 * 拒绝了 PARTITION BY，故无需再检查。）      
			 */
			if (windef->name ||
				orderClause || windef->frameOptions != FRAMEOPTION_DEFAULTS)
				ereport(ERROR,
						(errcode(ERRCODE_WINDOWING_ERROR),
						 errmsg("cannot copy window \"%s\" because it has a frame clause",
								windef->refname),
						 parser_errposition(pstate, windef->location)));
			/* 否则该子句就是 OVER (foo)，此时说：   */
			ereport(ERROR,
					(errcode(ERRCODE_WINDOWING_ERROR),
					 errmsg("cannot copy window \"%s\" because it has a frame clause",
							windef->refname),
					 errhint("Omit the parentheses in this OVER clause."),
					 parser_errposition(pstate, windef->location)));
		}
		wc->frameOptions = windef->frameOptions;

		/*
		 * RANGE 偏移 PRECEDING/FOLLOWING 需要恰好一个 ORDER BY  
		 * 列；检查并获取排序操作符族信息。  
		 */
		if ((wc->frameOptions & FRAMEOPTION_RANGE) &&
			(wc->frameOptions & (FRAMEOPTION_START_OFFSET |
								 FRAMEOPTION_END_OFFSET)))
		{
			SortGroupClause *sortcl;
			Node	   *sortkey;
			int16		rangestrategy;

			if (list_length(wc->orderClause) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_WINDOWING_ERROR),
						 errmsg("RANGE with offset PRECEDING/FOLLOWING requires exactly one ORDER BY column"),
						 parser_errposition(pstate, windef->location)));
			sortcl = linitial_node(SortGroupClause, wc->orderClause);
			sortkey = get_sortgroupclause_expr(sortcl, *targetlist);
			/* 在 pg_amop 找排序操作符    */
			if (!get_ordering_op_properties(sortcl->sortop,
											&rangeopfamily,
											&rangeopcintype,
											&rangestrategy))
				elog(ERROR, "operator %u is not a valid ordering operator",
					 sortcl->sortop);
			/* 记录排序次序的各属性     */
			wc->inRangeColl = exprCollation(sortkey);
			wc->inRangeAsc = (rangestrategy == BTLessStrategyNumber);
			wc->inRangeNullsFirst = sortcl->nulls_first;
		}

		/* 按规范，GROUPS 模式需要 ORDER BY 子句   */
		if (wc->frameOptions & FRAMEOPTION_GROUPS)
		{
			if (wc->orderClause == NIL)
				ereport(ERROR,
						(errcode(ERRCODE_WINDOWING_ERROR),
						 errmsg("GROUPS mode requires an ORDER BY clause"),
						 parser_errposition(pstate, windef->location)));
		}

		/* 处理窗口帧偏移表达式   */
		wc->startOffset = transformFrameOffset(pstate, wc->frameOptions,
											   rangeopfamily, rangeopcintype,
											   &wc->startInRangeFunc,
											   windef->startOffset);
		wc->endOffset = transformFrameOffset(pstate, wc->frameOptions,
											 rangeopfamily, rangeopcintype,
											 &wc->endInRangeFunc,
											 windef->endOffset);
		wc->runCondition = NIL;
		wc->winref = winref;

		result = lappend(result, wc);
	}

	return result;
}

/*
 * transformDistinctClause -
 *	  转换 DISTINCT 子句     
 *
 * 由于可能需要向查询的目标列表添加条目，该列表 
 * 按引用传递。     
 *
 * 与 GROUP BY 一样，我们把 ORDER BY 的排序语义尽量          
 * 吸收进 distinctClause。这避免了可能需要重新排序，        
 * 并让用户能够选择 DISTINCT 所用的相等语义，            
 * 如果用户使用的数据类型具有不止一个相等操作符。
 *          
 *
 * is_agg 为真表示我们正在转换 aggregate(DISTINCT ...)   
 * 函数调用。这不影响任何行为，只影响错误消息的  
 * 措辞。         
 */
List *
transformDistinctClause(ParseState *pstate,
						List **targetlist, List *sortClause, bool is_agg)
{
	List	   *result = NIL;
	ListCell   *slitem;
	ListCell   *tlitem;

	/*
	 * distinctClause 应由全部 ORDER BY 项加上所有其他             
	 * 非 resjunk 的目标列表条目构成。不得有任何 resjunk 
	 * 的 ORDER BY 项 —— 那意味着我们正在按一个在           
	 * DISTINCT 组内未必唯一的值排序，这样结果将不是 
	 * 良定义的。这种构造确保我们遵守 sortClause 与           
	 * distinctClause 匹配的规则；实际上 sortClause 必为         
	 * distinctClause 的前缀。          
	 *
	 * 注意一个边界情形：同一个 TLE 可能以不同 sortop        
	 * 在 ORDER BY 列表中出现多次。必须以同样的    
	 * 方式把它纳入 distinctClause，以保持前缀性质。最终  
	 * 效果是该 TLE 值将按两个 sortop 都变为唯一。             
	 *         
	 */
	foreach(slitem, sortClause)
	{
		SortGroupClause *scl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(scl, *targetlist);

		if (tle->resjunk)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 is_agg ?
					 errmsg("in an aggregate with DISTINCT, ORDER BY expressions must appear in argument list") :
					 errmsg("for SELECT DISTINCT, ORDER BY expressions must appear in select list"),
					 parser_errposition(pstate,
										exprLocation((Node *) tle->expr))));
		result = lappend(result, copyObject(scl));
	}

	/*
	 * 现在加入其余非 resjunk 的 tlist 条目，对其数据类型    
	 * 用默认排序/分组语义。
	 */
	foreach(tlitem, *targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tlitem);

		if (tle->resjunk)
			continue;			/* 忽略垃圾*/
		result = addTargetToGroupList(pstate, tle,
									  result, *targetlist,
									  exprLocation((Node *) tle->expr));
	}

	/*
	 * 如果没找到任何可用于 DISTINCT 的东西就报错。返回    
	 * 空列表会让解析出的 Query 看起来像没有 DISTINCT，       
	 * 其结果多半会让用户意外。注意：这种情形目前   
	 * 对聚合而言不可能出现（语法限制所致），但我们  
	 * 仍要检查。     
	 */
	if (result == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 is_agg ?
				 errmsg("an aggregate with DISTINCT must have at least one argument") :
				 errmsg("SELECT DISTINCT must have at least one column")));

	return result;
}

/*
 * transformDistinctOnClause -
 *	  转换 DISTINCT ON 子句     
 *
 * 由于可能需要向查询的目标列表添加条目，该列表 
 * 按引用传递。     
 *
 * 与 GROUP BY 一样，我们把 ORDER BY 的排序语义尽量          
 * 吸收进 distinctClause。这避免了可能需要重新排序，        
 * 并让用户能够选择 DISTINCT 所用的相等语义，            
 * 如果用户使用的数据类型具有不止一个相等操作符。
 *          
 */
List *
transformDistinctOnClause(ParseState *pstate, List *distinctlist,
						  List **targetlist, List *sortClause)
{
	List	   *result = NIL;
	List	   *sortgrouprefs = NIL;
	bool		skipped_sortitem;
	ListCell   *lc;
	ListCell   *lc2;

	/*
	 * 把所有 DISTINCT ON 表达式加入 tlist（若不存在，   
	 * 则作为 resjunk 项加入）。为它们分配 sortgroupref 编号，
	 * 并把这些编号做成一个列表。（注意：下面我们依赖
	 * sortgrouprefs 列表与原 distinctlist 一一对应。             
	 * 还要注意：DISTINCT ON 表达式可能重复，因此           
	 * sortgrouprefs 中或有重复条目。） 
	 */
	foreach(lc, distinctlist)
	{
		Node	   *dexpr = (Node *) lfirst(lc);
		int			sortgroupref;
		TargetEntry *tle;

		tle = findTargetlistEntrySQL92(pstate, dexpr, targetlist,
									   EXPR_KIND_DISTINCT_ON);
		sortgroupref = assignSortGroupRef(tle, *targetlist);
		sortgrouprefs = lappend_int(sortgrouprefs, sortgroupref);
	}

	/*
	 * 如果用户同时写了 DISTINCT ON 与 ORDER BY，就采纳      
	 * 与 DISTINCT ON 项匹配的 ORDER BY 项的排序语义，         
	 * 并采纳其列排序次序。我们坚持 distinctClause 与        
	 * sortClause 匹配；因此若在跳过不属于 DISTINCT ON         
	 * 的 ORDER BY 项后发现还需增加更多 distinctClause           
	 * 条目即报错
	 */
	skipped_sortitem = false;
	foreach(lc, sortClause)
	{
		SortGroupClause *scl = (SortGroupClause *) lfirst(lc);

		if (list_member_int(sortgrouprefs, scl->tleSortGroupRef))
		{
			if (skipped_sortitem)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("SELECT DISTINCT ON expressions must match initial ORDER BY expressions"),
						 parser_errposition(pstate,
											get_matching_location(scl->tleSortGroupRef,
																  sortgrouprefs,
																  distinctlist))));
			else
				result = lappend(result, copyObject(scl));
		}
		else
			skipped_sortitem = true;
	}

	/*
	 * 现在加入其余 DISTINCT ON 项，对其数据类型使用    
	 * 默认排序/分组语义。（注意：这很成问题；如果       
	 * ORDER BY 列表未包含全部 DISTINCT ON 项再加更多，       
	 * 那你肯定没有按预期方式使用 DISTINCT ON，              
	 * 结果也很可能不一致。或许在这里抛错或警告         
	 * 会更好。但传统上我们允许，所以继续如此。）  
	 *                               
	 */
	forboth(lc, distinctlist, lc2, sortgrouprefs)
	{
		Node	   *dexpr = (Node *) lfirst(lc);
		int			sortgroupref = lfirst_int(lc2);
		TargetEntry *tle = get_sortgroupref_tle(sortgroupref, *targetlist);

		if (targetIsInSortList(tle, InvalidOid, result))
			continue;			/* 已在列表中（带某种语义）  */
		if (skipped_sortitem)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("SELECT DISTINCT ON expressions must match initial ORDER BY expressions"),
					 parser_errposition(pstate, exprLocation(dexpr))));
		result = addTargetToGroupList(pstate, tle,
									  result, *targetlist,
									  exprLocation(dexpr));
	}

	/*
	 * 因语法限制，这里结果列表不可能为空。    
	 *              
	 */
	Assert(result != NIL);

	return result;
}

/*
 * get_matching_location
 *		取 exprs 成员中对应于 sortgrouprefs 中等于         
 *		sortgroupref 的（首个）成员的 exprLocation。    
 *
 * 这用于让我们能指向出问题的 DISTINCT ON 条目。        
 * （注意：必须使用未经转换的原始 DISTINCT ON 列表     
 * 条目，因为它所对应的 TLE 的解析位置很可能指向
 * SELECT 列表或 ORDER BY 列表中的某个匹配条目。）   
 *                   
 */
static int
get_matching_location(int sortgroupref, List *sortgrouprefs, List *exprs)
{
	ListCell   *lcs;
	ListCell   *lce;

	forboth(lcs, sortgrouprefs, lce, exprs)
	{
		if (lfirst_int(lcs) == sortgroupref)
			return exprLocation((Node *) lfirst(lce));
	}
	/* 无匹配，调用方有误 */
	elog(ERROR, "get_matching_location: no matching sortgroupref");
	return -1;					/* 安抚编译器     */
}

/*
 * resolve_unique_index_expr
 *		从 indexElems 列表推断唯一索引，用于 ON   
 *		CONFLICT 子句
 *
 * 对 ON CONFLICT 子句中出现的表达式和列执行              
 * 解析分析。在规划阶段，返回的表达式列表被用来         
 * 推断该用哪个唯一索引。  
 */
static List *
resolve_unique_index_expr(ParseState *pstate, InferClause *infer,
						  Relation heapRel)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, infer->indexElems)
	{
		IndexElem  *ielem = (IndexElem *) lfirst(l);
		InferenceElem *pInfer = makeNode(InferenceElem);
		Node	   *parse;

		/*
		 * 原始语法为唯一索引推断子句复用 CREATE INDEX      
		 * 基础设施，故会接受按名称给出的操作符类等。  
		 *
		 * 不去匹配 ASC 或 DESC 排序方向或 NULLS FIRST/NULLS       
		 * LAST 排序方向，因为它们对推断而言并不重要 
		 * （任何在其他方面匹配推断规格的唯一索引都会   
		 * 被不加区分地接受）。要明确拒绝这种本末倒置   
		 * 的做法。 
		 */
		if (ielem->ordering != SORTBY_DEFAULT)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("ASC/DESC is not allowed in ON CONFLICT clause"),
					 parser_errposition(pstate,
										exprLocation((Node *) infer))));
		if (ielem->nulls_ordering != SORTBY_NULLS_DEFAULT)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("NULLS FIRST/LAST is not allowed in ON CONFLICT clause"),
					 parser_errposition(pstate,
										exprLocation((Node *) infer))));

		if (!ielem->expr)
		{
			/* 简单索引属性     */
			ColumnRef  *n;

			/*
			 * 普通列引用，语法不会替我们构建原始表达 
			 * 式。需直接创建并转换表达式。规划器      
			 * 期望这一点，并会自行做规范化以便与 pg_index  
			 * 进行匹配。                                           
			 *          
			 */
			n = makeNode(ColumnRef);
			n->fields = list_make1(makeString(ielem->name));
			/* 位置大致就是推断规格所在的位置             */
			n->location = infer->location;
			parse = (Node *) n;
		}
		else
		{
			/* 对原始表达式做解析转换             */
			parse = (Node *) ielem->expr;
		}

		/*
		 * transformExpr() 会拒绝子查询、聚合、窗口函数
		 * 和 SRF：因为向它传入的是        
		 * EXPR_KIND_INDEX_EXPRESSION，所以无需再担心这些    
		 * 了……何况它们也不会匹配任何可用的      
		 * 索引表达式。
		 */
		pInfer->expr = transformExpr(pstate, parse, EXPR_KIND_INDEX_EXPRESSION);

		/* 按需查找排序规则和操作符类                    */
		if (!ielem->collation)
			pInfer->infercollid = InvalidOid;
		else
			pInfer->infercollid = LookupCollation(pstate, ielem->collation,
												  exprLocation(pInfer->expr));

		if (!ielem->opclass)
			pInfer->inferopclass = InvalidOid;
		else
			pInfer->inferopclass = get_opclass_oid(BTREE_AM_OID,
												   ielem->opclass, false);

		result = lappend(result, pInfer);
	}

	return result;
}

/*
 * transformOnConflictArbiter -
 *		转换 ON CONFLICT 子句中的仲裁器表达式。   
 *
 * 转换后的表达式用于推断一个唯一索引关系，作为         
 * ON CONFLICT 仲裁器。可使用推断规格子句中的                  
 * WHERE 子句来推断部分唯一索引。 
 */
void
transformOnConflictArbiter(ParseState *pstate,
						   OnConflictClause *onConflictClause,
						   List **arbiterExpr, Node **arbiterWhere,
						   Oid *constraint)
{
	InferClause *infer = onConflictClause->infer;

	*arbiterExpr = NIL;
	*arbiterWhere = NULL;
	*constraint = InvalidOid;

	if (onConflictClause->action == ONCONFLICT_UPDATE && !infer)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("ON CONFLICT DO UPDATE requires inference specification or constraint name"),
				 errhint("For example, ON CONFLICT (column_name)."),
				 parser_errposition(pstate,
									exprLocation((Node *) onConflictClause))));

	/*
	 * 为了简化其设计的某些方面，不允许向系统目录      
	 * 进行推测插入。        
	 */
	if (IsCatalogRelation(pstate->p_target_relation))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ON CONFLICT is not supported with system catalog tables"),
				 parser_errposition(pstate,
									exprLocation((Node *) onConflictClause))));

	/* 被逻辑解码用作目录表的表同样适用此限制       */
	if (RelationIsUsedAsCatalogTable(pstate->p_target_relation))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ON CONFLICT is not supported on table \"%s\" used as a catalog table",
						RelationGetRelationName(pstate->p_target_relation)),
				 parser_errposition(pstate,
									exprLocation((Node *) onConflictClause))));

	/* ON CONFLICT DO NOTHING 不需要推断子句                */
	if (infer)
	{
		if (infer->indexElems)
			*arbiterExpr = resolve_unique_index_expr(pstate, infer,
													 pstate->p_target_relation);

		/*
		 * 处理 WHERE 推断子句（用于部分唯一索引）  
		 *           
		 */
		if (infer->whereClause)
			*arbiterWhere = transformExpr(pstate, infer->whereClause,
										  EXPR_KIND_INDEX_PREDICATE);

		/*
		 * 如果仲裁器用约束名指定，则取出该约束的 OID，  
		 * 并把这些受约束列标记为需要 SELECT 权限，就像     
		 * 仲裁器被显式指定为约束索引列时本会发生的  
		 * 那样。                                                         
		 */
		if (infer->conname)
		{
			Oid			relid = RelationGetRelid(pstate->p_target_relation);
			RTEPermissionInfo *perminfo = pstate->p_target_nsitem->p_perminfo;
			Bitmapset  *conattnos;

			conattnos = get_relation_constraint_attnos(relid, infer->conname,
													   false, constraint);

			/* 确保整个 rel 也被标记为需要 SELECT 访问     */
			perminfo->requiredPerms |= ACL_SELECT;
			/* 把这些受约束列标记为需要 SELECT 访问      */
			perminfo->selectedCols = bms_add_members(perminfo->selectedCols,
													 conattnos);
		}
	}

	/*
	 * 基于 CREATE INDEX 所用的表示形式构建表达式  
	 * 列表会很方便，因为同样的限制也适用（例如        
	 * 对子查询）。但从这里开始，推断元素改用            
	 * 专用的 primnode 表示形式，因此可以相信          
	 * assign_query_collations() 对解析分析后的查询树               
	 * 推断子句表示形式做出正确处理。                  
	 */
}

/*
 * addTargetToSortList
 *		如果给定的目标列表条目尚不在 SortGroupClause        
 *		列表中，就用给定的排序信息把它加入列表末尾   
 *		     
 *
 * 返回更新后 SortGroupClause 列表。
 */
List *
addTargetToSortList(ParseState *pstate, TargetEntry *tle,
					List *sortlist, List *targetlist, SortBy *sortby)
{
	Oid			restype = exprType((Node *) tle->expr);
	Oid			sortop;
	Oid			eqop;
	bool		hashable;
	bool		reverse;
	int			location;
	ParseCallbackState pcbstate;

	/* 如果 tlist 项是 UNKNOWN 字面量，则改成 TEXT */
	if (restype == UNKNOWNOID)
	{
		tle->expr = (Expr *) coerce_type(pstate, (Node *) tle->expr,
										 restype, TEXTOID, -1,
										 COERCION_IMPLICIT,
										 COERCE_IMPLICIT_CAST,
										 -1);
		restype = TEXTOID;
	}

	/*
	 * 为了不去弄乱 get_sort_group_operators 以及其他             
	 * 即将用到的函数的 API，我们利用错误上下文回调   
	 * 来给所有错误报告标注解析位置。若操作符位置        
	 * 存在就指向它，否则指向被排序的表达式。（注意：  
	 * 这里要用未经转换的原始表达式；TLE 条目很可能      
	 * 指向 SELECT 列表中的重复表达式。）       
	 */
	location = sortby->location;
	if (location < 0)
		location = exprLocation(sortby->node);
	setup_parser_errposition_callback(&pcbstate, pstate, location);

	/* 确定 sortop、eqop 以及方向性           */
	switch (sortby->sortby_dir)
	{
		case SORTBY_DEFAULT:
		case SORTBY_ASC:
			get_sort_group_operators(restype,
									 true, true, false,
									 &sortop, &eqop, NULL,
									 &hashable);
			reverse = false;
			break;
		case SORTBY_DESC:
			get_sort_group_operators(restype,
									 false, true, true,
									 NULL, &eqop, &sortop,
									 &hashable);
			reverse = true;
			break;
		case SORTBY_USING:
			Assert(sortby->useOp != NIL);
			sortop = compatible_oper_opid(sortby->useOp,
										  restype,
										  restype,
										  false);

			/*
			 * 验证它是有效的排序操作符，取出相应的相等  
			 * 操作符，并确定应当把它视为 ASC 还是          
			 * DESC。     
			 */
			eqop = get_equality_op_for_ordering_op(sortop, &reverse);
			if (!OidIsValid(eqop))
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("operator %s is not a valid ordering operator",
								strVal(llast(sortby->useOp))),
						 errhint("Ordering operators must be \"<\" or \">\" members of btree operator families.")));

			/*
			 * 另外看看相等操作符是否可哈希。 
			 */
			hashable = op_hashjoinable(eqop, restype);
			break;
		default:
			elog(ERROR, "unrecognized sortby_dir: %d", sortby->sortby_dir);
			sortop = InvalidOid;	/* 安抚编译器     */
			eqop = InvalidOid;
			hashable = false;
			reverse = false;
			break;
	}

	cancel_parser_errposition_callback(&pcbstate);

	/* 避免产生重复的 sortlist 条目   */
	if (!targetIsInSortList(tle, sortop, sortlist))
	{
		SortGroupClause *sortcl = makeNode(SortGroupClause);

		sortcl->tleSortGroupRef = assignSortGroupRef(tle, targetlist);

		sortcl->eqop = eqop;
		sortcl->sortop = sortop;
		sortcl->hashable = hashable;

		switch (sortby->sortby_nulls)
		{
			case SORTBY_NULLS_DEFAULT:
				/* DESC 默认 NULLS FIRST；ASC 则相反            */
				sortcl->nulls_first = reverse;
				break;
			case SORTBY_NULLS_FIRST:
				sortcl->nulls_first = true;
				break;
			case SORTBY_NULLS_LAST:
				sortcl->nulls_first = false;
				break;
			default:
				elog(ERROR, "unrecognized sortby_nulls: %d",
					 sortby->sortby_nulls);
				break;
		}

		sortlist = lappend(sortlist, sortcl);
	}

	return sortlist;
}

/*
 * addTargetToGroupList
 *		如果给定的目标列表条目尚不在 SortGroupClause        
 *		列表中，就用默认排序/分组语义把它加入列表
 *		          
 *
 * 这与 addTargetToSortList 非常相似，但允许只找到         
 * 分组（相等）操作符的情形，而且只要条目带着      
 * 任意排序语义出现，就认为该 TLE "已在列表中"。        
 *                   
 *
 * location 是出问题时要指认的解析位置。注意我们         
 * 无法依赖 exprLocation(tle->expr)，因为它可能指向           
 * 与该 GROUP BY 项匹配的某个 SELECT 条目；报告                
 * 这会让人非常困惑  
 *
 * 返回更新后 SortGroupClause 列表。
 */
static List *
addTargetToGroupList(ParseState *pstate, TargetEntry *tle,
					 List *grouplist, List *targetlist, int location)
{
	Oid			restype = exprType((Node *) tle->expr);

	/* 如果 tlist 项是 UNKNOWN 字面量，则改成 TEXT */
	if (restype == UNKNOWNOID)
	{
		tle->expr = (Expr *) coerce_type(pstate, (Node *) tle->expr,
										 restype, TEXTOID, -1,
										 COERCION_IMPLICIT,
										 COERCE_IMPLICIT_CAST,
										 -1);
		restype = TEXTOID;
	}

	/* 避免产生重复的 grouplist 条目   */
	if (!targetIsInSortList(tle, InvalidOid, grouplist))
	{
		SortGroupClause *grpcl = makeNode(SortGroupClause);
		Oid			sortop;
		Oid			eqop;
		bool		hashable;
		ParseCallbackState pcbstate;

		setup_parser_errposition_callback(&pcbstate, pstate, location);

		/* 确定 eqop 和可选的 sortop        */
		get_sort_group_operators(restype,
								 false, true, false,
								 &sortop, &eqop, NULL,
								 &hashable);

		cancel_parser_errposition_callback(&pcbstate);

		grpcl->tleSortGroupRef = assignSortGroupRef(tle, targetlist);
		grpcl->eqop = eqop;
		grpcl->sortop = sortop;
		grpcl->nulls_first = false; /* 有或没有 sortop 均可*/
		grpcl->hashable = hashable;

		grouplist = lappend(grouplist, grpcl);
	}

	return grouplist;
}

/*
 * assignSortGroupRef
 *	  如果该目标条目尚无 ressortgroupref，就给它分配  
 *	  未用的 ressortgroupref。返回新分配或既有 refnumber。
 *
 * 'tlist' 是包含（或将包含）给定目标条目的目标列表。    
 */
Index
assignSortGroupRef(TargetEntry *tle, List *tlist)
{
	Index		maxRef;
	ListCell   *l;

	if (tle->ressortgroupref)	/* 已有一个？  */
		return tle->ressortgroupref;

	/* 取未用 refnumber 的最简方法：最大已用+1  */
	maxRef = 0;
	foreach(l, tlist)
	{
		Index		ref = ((TargetEntry *) lfirst(l))->ressortgroupref;

		if (ref > maxRef)
			maxRef = ref;
	}
	tle->ressortgroupref = maxRef + 1;
	return tle->ressortgroupref;
}

/*
 * targetIsInSortList
 *		给定的目标项是否已在排序列表中？ 
 *		若 sortop 非 InvalidOid，还测试是否匹配该 sortop。   
 *
 * 本函数忽略 nulls_first 标志并非疏忽。                      
 * 在判断一个 ORDER BY 项是否与更早的 ORDER BY               
 * 项冗余时会检查 sortop，因为 "ORDER BY foo USING      
 *  <, foo USING <<<" 并非冗余——倘若 <<< 能区分         
 * < 视为相等的值。不过无需检查 nulls_first，     
 * 因为 sortop 相同而 null 方向相反的更低阶列      
 * 是冗余的。另外，ORDER BY foo ASC 与 foo             
 * DESC 也算冗余，所以要检查交换算子匹配。              
 *
 * 它对排序和分组列表都适用（考虑分组时 sortop       
 * 通常应为 InvalidOid）。注意我们需要此例程的主要        
 * 原因（而不只是快速测试 ressortgroupref 非零）是：          
 * 一个 TLE 可能只在其中一个列表中。 
 */
bool
targetIsInSortList(TargetEntry *tle, Oid sortop, List *sortList)
{
	Index		ref = tle->ressortgroupref;
	ListCell   *l;

	/* 若 tle 无标记则无需扫描列表    */
	if (ref == 0)
		return false;

	foreach(l, sortList)
	{
		SortGroupClause *scl = (SortGroupClause *) lfirst(l);

		if (scl->tleSortGroupRef == ref &&
			(sortop == InvalidOid ||
			 sortop == scl->sortop ||
			 sortop == get_commutator(scl->sortop)))
			return true;
	}
	return false;
}

/*
 * findWindowClause
 *		在列表中查找具名的 WindowClause，找不到则返回 NULL  
 */
static WindowClause *
findWindowClause(List *wclist, const char *name)
{
	ListCell   *l;

	foreach(l, wclist)
	{
		WindowClause *wc = (WindowClause *) lfirst(l);

		if (wc->name && strcmp(wc->name, name) == 0)
			return wc;
	}

	return NULL;
}

/*
 * transformFrameOffset
 *		处理窗口帧偏移表达式          
 *
 * 在 RANGE 模式下，rangeopfamily 是输入 ORDER BY 列              
 * 的排序操作符族，而 rangeopcintype 是排序操作符         
 * 注册所用的输入数据类型。我们期望 in_range 函数       
 * 也注册在该类型上。（在二进制兼容的情形下，它      
 * 可能与输入列的实际类型不同，因此查找时无法使用 
 * 它。）in_range 函数 OID 会经 *inRangeFunc 返回。    
 */
static Node *
transformFrameOffset(ParseState *pstate, int frameOptions,
					 Oid rangeopfamily, Oid rangeopcintype, Oid *inRangeFunc,
					 Node *clause)
{
	const char *constructName = NULL;
	Node	   *node;

	*inRangeFunc = InvalidOid;	/* 默认结果   */

	/* 无偏移表达式时快速返回  */
	if (clause == NULL)
		return NULL;

	if (frameOptions & FRAMEOPTION_ROWS)
	{
		/* 转换原始表达式树          */
		node = transformExpr(pstate, clause, EXPR_KIND_WINDOW_FRAME_ROWS);

		/*
		 * 同 LIMIT 子句，简单转换为 int8 
		 */
		constructName = "ROWS";
		node = coerce_to_specific_type(pstate, node, INT8OID, constructName);
	}
	else if (frameOptions & FRAMEOPTION_RANGE)
	{
		/*
		 * 我们必须查找将要使用的 in_range 支持函数，      
		 * 可能要从多个中挑选一个，并把 "offset" 值转换为  
		 * 合适的输入类型。   
		 */
		Oid			nodeType;
		Oid			preferredType;
		int			nfuncs = 0;
		int			nmatches = 0;
		Oid			selectedType = InvalidOid;
		Oid			selectedFunc = InvalidOid;
		CatCList   *proclist;
		int			i;

		/* 转换原始表达式树          */
		node = transformExpr(pstate, clause, EXPR_KIND_WINDOW_FRAME_RANGE);
		nodeType = exprType(node);

		/*
		 * 如果有多个候选，我们会优先选择与 nodeType 完全   
		 * 匹配的那个；或者当 nodeType 尚未知时，优先选择  
		 * 与排序列类型完全匹配的那个。（第二条规则   
		 * 类似 "known_type operator unknown" 处理。）  
		 */
		preferredType = (nodeType != UNKNOWNOID) ? nodeType : rangeopcintype;

		/* 找出适用于此情形的 in_range 支持函数           */
		proclist = SearchSysCacheList2(AMPROCNUM,
									   ObjectIdGetDatum(rangeopfamily),
									   ObjectIdGetDatum(rangeopcintype));
		for (i = 0; i < proclist->n_members; i++)
		{
			HeapTuple	proctup = &proclist->members[i]->tuple;
			Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);

			/* 搜索会找到所有支持过程类型；忽略其他     */
			if (procform->amprocnum != BTINRANGE_PROC)
				continue;
			nfuncs++;

			/* 如果给定值无法转换为该类型，忽略该函数    */
			if (!can_coerce_type(1, &nodeType, &procform->amprocrighttype,
								 COERCION_IMPLICIT))
				continue;
			nmatches++;

			/* 记住优先匹配；若没找到则记住任一匹配     */
			if (selectedType != preferredType)
			{
				selectedType = procform->amprocrighttype;
				selectedFunc = procform->amproc;
			}
		}
		ReleaseCatCacheList(proclist);

		/*
		 * 需要时就抛错。值得费力区分 "毫无支持" 与  
		 * "未匹配任何可用偏移类型" 这两种情形       
		 *                        
		 */
		if (nfuncs == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("RANGE with offset PRECEDING/FOLLOWING is not supported for column type %s",
							format_type_be(rangeopcintype)),
					 parser_errposition(pstate, exprLocation(node))));
		if (nmatches == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("RANGE with offset PRECEDING/FOLLOWING is not supported for column type %s and offset type %s",
							format_type_be(rangeopcintype),
							format_type_be(nodeType)),
					 errhint("Cast the offset value to an appropriate type."),
					 parser_errposition(pstate, exprLocation(node))));
		if (nmatches != 1 && selectedType != preferredType)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("RANGE with offset PRECEDING/FOLLOWING has multiple interpretations for column type %s and offset type %s",
							format_type_be(rangeopcintype),
							format_type_be(nodeType)),
					 errhint("Cast the offset value to the exact intended type."),
					 parser_errposition(pstate, exprLocation(node))));

		/* 好，把偏移转换为正确的类型 */
		constructName = "RANGE";
		node = coerce_to_specific_type(pstate, node,
									   selectedType, constructName);
		*inRangeFunc = selectedFunc;
	}
	else if (frameOptions & FRAMEOPTION_GROUPS)
	{
		/* 转换原始表达式树          */
		node = transformExpr(pstate, clause, EXPR_KIND_WINDOW_FRAME_GROUPS);

		/*
		 * 同 LIMIT 子句，简单转换为 int8 
		 */
		constructName = "GROUPS";
		node = coerce_to_specific_type(pstate, node, INT8OID, constructName);
	}
	else
	{
		Assert(false);
		node = NULL;
	}

	/* 禁止在帧偏移中使用变量   */
	checkExprIsVarFree(pstate, node, constructName);

	return node;
}
