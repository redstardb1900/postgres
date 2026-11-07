/*-------------------------------------------------------------------------
 *
 * parse_target.c
 *	  处理目标列表 
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_target.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/typcache.h"

static void markTargetListOrigin(ParseState *pstate, TargetEntry *tle,
								 Var *var, int levelsup);
static Node *transformAssignmentSubscripts(ParseState *pstate,
										   Node *basenode,
										   const char *targetName,
										   Oid targetTypeId,
										   int32 targetTypMod,
										   Oid targetCollation,
										   List *subscripts,
										   List *indirection,
										   ListCell *next_indirection,
										   Node *rhs,
										   CoercionContext ccontext,
										   int location);
static List *ExpandColumnRefStar(ParseState *pstate, ColumnRef *cref,
								 bool make_target_entry);
static List *ExpandAllTables(ParseState *pstate, int location);
static List *ExpandIndirectionStar(ParseState *pstate, A_Indirection *ind,
								   bool make_target_entry, ParseExprKind exprKind);
static List *ExpandSingleTable(ParseState *pstate, ParseNamespaceItem *nsitem,
							   int sublevels_up, int location,
							   bool make_target_entry);
static List *ExpandRowReference(ParseState *pstate, Node *expr,
								bool make_target_entry);
static int	FigureColnameInternal(Node *node, char **name);


/*
 * transformTargetEntry()
 *	把普通"表达式类型"节点转换成目标列表条目。        
 *	此函数被导出，以便 parse_clause.c 能为目标列表中       
 *	尚未出现的 ORDER/GROUP BY 项生成目标列表条目。   
 *
 * node 值表达式的（未变换）解析树                  
 * expr 已变换的表达式，调用者未处理时为 NULL           
 * exprKind 表达式种类，如 EXPR_KIND_SELECT_TARGET 
 * colname 要赋予的列名，若尚未设置则为 NULL         
 * resjunk 为 true 时该目标应标记为 resjunk，即           
 *			最终投影元组不需要该项。
 */
TargetEntry *
transformTargetEntry(ParseState *pstate,
					 Node *node,
					 Node *expr,
					 ParseExprKind exprKind,
					 char *colname,
					 bool resjunk)
{
	/* 若调用者尚未变换，则在此变换之     */
	if (expr == NULL)
	{
		/*
		 * 若为 SetToDefault 且允许使用，则原样传递。     
		 * （若不允许，transformExpr 会抛出相应错误）      
		 *                                
		 */
		if (exprKind == EXPR_KIND_UPDATE_SOURCE && IsA(node, SetToDefault))
			expr = node;
		else
			expr = transformExpr(pstate, node, exprKind);
	}

	if (colname == NULL && !resjunk)
	{
		/*
		 * 为没有显式 'AS 列名' 子句的列生成合适的列名    
		 *                        
		 */
		colname = FigureColname(node);
	}

	return makeTargetEntry((Expr *) expr,
						   (AttrNumber) pstate->p_next_resno++,
						   colname,
						   resjunk);
}


/*
 * transformTargetList()
 * 把 ResTarget 列表转换成 TargetEntry 列表。      
 *
 * 这段代码对 SELECT、UPDATE 或 RETURNING 列表的处理         
 * 大体相同；主要是变换给定表达式（即 "val" 字段）。  
 * 必要时由 exprKind 参数来区分这些情形。            
 */
List *
transformTargetList(ParseState *pstate, List *targetlist,
					ParseExprKind exprKind)
{
	List	   *p_target = NIL;
	bool		expand_star;
	ListCell   *o_target;

	/* 起点处不应残留任何 multiassign 项            */
	Assert(pstate->p_multiassign_exprs == NIL);

	/* 在 SELECT/RETURNING 中展开 "something.*"，UPDATE 除外 */
	expand_star = (exprKind != EXPR_KIND_UPDATE_SOURCE);

	foreach(o_target, targetlist)
	{
		ResTarget  *res = (ResTarget *) lfirst(o_target);

		/*
		 * 检查 "something.*"。视 "something" 的复杂程度，   
		 * 星号可能作为 ColumnRef 的最后字段出现，             
		 * 或为 A_Indirection 的末个间接引用项    
		 */
		if (expand_star)
		{
			if (IsA(res->val, ColumnRef))
			{
				ColumnRef  *cref = (ColumnRef *) res->val;

				if (IsA(llast(cref->fields), A_Star))
				{
					/* 是 something.*，展开成多个项          */
					p_target = list_concat(p_target,
										   ExpandColumnRefStar(pstate,
															   cref,
															   true));
					continue;
				}
			}
			else if (IsA(res->val, A_Indirection))
			{
				A_Indirection *ind = (A_Indirection *) res->val;

				if (IsA(llast(ind->indirection), A_Star))
				{
					/* 是 something.*，展开成多个项          */
					p_target = list_concat(p_target,
										   ExpandIndirectionStar(pstate,
																 ind,
																 true,
																 exprKind));
					continue;
				}
			}
		}

		/*
		 * 并非 "something.*"，或要将其当作普通整行变量     
		 * 处理，故按单一表达式进行变换   
		 */
		p_target = lappend(p_target,
						   transformTargetEntry(pstate,
												res->val,
												NULL,
												exprKind,
												res->name,
												false));
	}

	/*
	 * 若创建了 multiassign resjunk 项，则将它们附到            
	 * 目标列表末尾。这只应发生在 UPDATE 的 tlist 中。    
	 * 无需操心这些项的编号；transformUpdateStmt                   
	 * 会设置其 resnos。
	 */
	if (pstate->p_multiassign_exprs)
	{
		Assert(exprKind == EXPR_KIND_UPDATE_SOURCE);
		p_target = list_concat(p_target, pstate->p_multiassign_exprs);
		pstate->p_multiassign_exprs = NIL;
	}

	return p_target;
}


/*
 * transformExpressionList()
 *
 * 这与 transformTargetList 的变换完全相同，除了输入列表   
 * 元素是不带 ResTarget 修饰的裸表达式，输出元素同样只是
 * 不带 TargetEntry 修饰的表达式。此外，列表中不应出现   
 * multiassign 结构，故无需处理。该函数用于 ROW() 和        
 *                                                                    
 * VALUES() 结构。  
 *
 * exprKind 不足以表明是否允许 SetToDefault，因此         
 * 还需要一个额外的标志。     
 */
List *
transformExpressionList(ParseState *pstate, List *exprlist,
						ParseExprKind exprKind, bool allowDefault)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, exprlist)
	{
		Node	   *e = (Node *) lfirst(lc);

		/*
		 * 检查 "something.*"。视 "something" 的复杂程度，   
		 * 星号可能作为 ColumnRef 的最后字段出现，             
		 * 或为 A_Indirection 的末个间接引用项    
		 */
		if (IsA(e, ColumnRef))
		{
			ColumnRef  *cref = (ColumnRef *) e;

			if (IsA(llast(cref->fields), A_Star))
			{
				/* 是 something.*，展开成多个项          */
				result = list_concat(result,
									 ExpandColumnRefStar(pstate, cref,
														 false));
				continue;
			}
		}
		else if (IsA(e, A_Indirection))
		{
			A_Indirection *ind = (A_Indirection *) e;

			if (IsA(llast(ind->indirection), A_Star))
			{
				/* 是 something.*，展开成多个项          */
				result = list_concat(result,
									 ExpandIndirectionStar(pstate, ind,
														   false, exprKind));
				continue;
			}
		}

		/*
		 * 并非 "something.*"，按单一表达式变换。若为          
		 * SetToDefault 节点且我们允许它，则原样传递。 
		 * （若不允许，transformExpr 会抛出相应错误）       
		 *                       
		 */
		if (allowDefault && IsA(e, SetToDefault))
			 /* 空操作  */ ;
		else
			e = transformExpr(pstate, e, exprKind);

		result = lappend(result, e);
	}

	return result;
}


/*
 * resolveTargetListUnknowns()
 *		把未知类型的目标列表条目转换为 TEXT。    
 *
 * 我们是在穷尽了识别查询输出列类型的所有其他途径    
 * 之后才这样做。   
 */
void
resolveTargetListUnknowns(ParseState *pstate, List *targetlist)
{
	ListCell   *l;

	foreach(l, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Oid			restype = exprType((Node *) tle->expr);

		if (restype == UNKNOWNOID)
		{
			tle->expr = (Expr *) coerce_type(pstate, (Node *) tle->expr,
											 restype, TEXTOID, -1,
											 COERCION_IMPLICIT,
											 COERCE_IMPLICIT_CAST,
											 -1);
		}
	}
}


/*
 * markTargetListOrigins()
 *		给简单 Var 的目标列表列标记其来源表的       
 *		OID 和列号。              
 *
 * 目前只对 SELECT 目标列表和 RETURNING 列表这样做，        
 * 因为只有要把信息发送给前端时才需要它。              
 */
void
markTargetListOrigins(ParseState *pstate, List *targetlist)
{
	ListCell   *l;

	foreach(l, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		markTargetListOrigin(pstate, tle, (Var *) tle->expr, 0);
	}
}

/*
 * markTargetListOrigin()
 *		若 'var' 是普通关系的 Var，则用其来源标记 'tle'   
 *
 * levelsup 是解释 Var 的 varlevelsup 时所需的额外偏移量。    
 *
 * 注意我们不会深入视图内部，而是把视图报告为列的
 * 所有者。也无需深入连接：若看到的是连接别名 Var， 
 * 它必是已合并的 JOIN USING 列（或可能是整行 Var）；   
 * 这并非对任何普通表列的直接引用，所以                   
 * 不报告它。       
 */
static void
markTargetListOrigin(ParseState *pstate, TargetEntry *tle,
					 Var *var, int levelsup)
{
	int			netlevelsup;
	RangeTblEntry *rte;
	AttrNumber	attnum;

	if (var == NULL || !IsA(var, Var))
		return;
	netlevelsup = var->varlevelsup + levelsup;
	rte = GetRTEByRangeTablePosn(pstate, var->varno, netlevelsup);
	attnum = var->varattno;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* 是表或视图，报告它     */
			tle->resorigtbl = rte->relid;
			tle->resorigcol = attnum;
			break;
		case RTE_SUBQUERY:
			/* FROM 子查询：从子查询向上复制     */
			if (attnum != InvalidAttrNumber)
			{
				TargetEntry *ste = get_tle_by_resno(rte->subquery->targetList,
													attnum);

				if (ste == NULL || ste->resjunk)
					elog(ERROR, "subquery %s does not have attribute %d",
						 rte->eref->aliasname, attnum);
				tle->resorigtbl = ste->resorigtbl;
				tle->resorigcol = ste->resorigcol;
			}
			break;
		case RTE_JOIN:
		case RTE_FUNCTION:
		case RTE_VALUES:
		case RTE_TABLEFUNC:
		case RTE_NAMEDTUPLESTORE:
		case RTE_RESULT:
			/* 并非简单关系，保持不标记     */
			break;
		case RTE_CTE:

			/*
			 * CTE 引用：可能时从子查询向上复制。若 RTE 是 
			 * 递归自引用则无法处理，因为尚未分析完；  
			 * 但损失不大，因为我们必处于递归 CTE 的递归项
			 * 内部，当前目标列表上的标记反正不会影响      
			 *                                                             
			 * 结果。                                  
			 */
			if (attnum != InvalidAttrNumber && !rte->self_reference)
			{
				CommonTableExpr *cte = GetCTEForRTE(pstate, rte, netlevelsup);
				TargetEntry *ste;
				List	   *tl = GetCTETargetList(cte);
				int			extra_cols = 0;

				/*
				 * CTE 的 RTE 已带 search/cycle 列，子查询没有，  
				 * 故跳过对它们的查找。                          
				 */
				if (cte->search_clause)
					extra_cols += 1;
				if (cte->cycle_clause)
					extra_cols += 2;
				if (extra_cols &&
					attnum > list_length(tl) &&
					attnum <= list_length(tl) + extra_cols)
					break;

				ste = get_tle_by_resno(tl, attnum);
				if (ste == NULL || ste->resjunk)
					elog(ERROR, "CTE %s does not have attribute %d",
						 rte->eref->aliasname, attnum);
				tle->resorigtbl = ste->resorigtbl;
				tle->resorigcol = ste->resorigcol;
			}
			break;
	}
}


/*
 * transformAssignedExpr()
 *	这只用于 INSERT 和 UPDATE 语句，为目标表的某列     
 *	准备要赋值的表达式。                            
 *	这包括将给定值强制转换为目标列类型（如有      
 *	必要），并处理附着在目标列本身的子字段名或  
 *	下标。输入表达式已经过 transformExpr() 处理。    
 *	                                     
 *
 * pstate 解析状态
 * expr 待修改的表达式      
 * exprKind 指明我们在处理哪种类型的语句           
 * colname 目标列名（即被赋值属性的名字）                
 * attrno 目标属性号         
 * indirection 目标列的下标/字段名（如有）        
 * location 目标列的错误游标位置，或 -1            
 *
 * 返回修改后的表达式。  
 *
 * 注意：location 指向目标列名（SET 目标或 INSERT 列名   
 * 列表条目），故省略列名列表的 INSERT 中必为 -1。  
 * 所以对于默认 INSERT 中可能发生的错误，更宜用 
 * exprLocation(expr) 报告。                                      
 */
Expr *
transformAssignedExpr(ParseState *pstate,
					  Expr *expr,
					  ParseExprKind exprKind,
					  const char *colname,
					  int attrno,
					  List *indirection,
					  int location)
{
	Relation	rd = pstate->p_target_relation;
	Oid			type_id;		/* 所提供值的类型  */
	Oid			attrtype;		/* 目标列的类型    */
	int32		attrtypmod;
	Oid			attrcollation;	/* 目标列的排序规则   */
	ParseExprKind sv_expr_kind;

	/*
	 * 保存并恢复我们正在解析的表达式种类标识。必须  
	 * 在此设置 p_expr_kind，因为下标的解析可不经过      
	 * transformExpr()。      
	 */
	Assert(exprKind != EXPR_KIND_NONE);
	sv_expr_kind = pstate->p_expr_kind;
	pstate->p_expr_kind = exprKind;

	Assert(rd != NULL);
	if (attrno <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot assign to system column \"%s\"",
						colname),
				 parser_errposition(pstate, location)));
	attrtype = attnumTypeId(rd, attrno);
	attrtypmod = TupleDescAttr(rd->rd_att, attrno - 1)->atttypmod;
	attrcollation = TupleDescAttr(rd->rd_att, attrno - 1)->attcollation;

	/*
	 * 若表达式是 DEFAULT 占位符，就把该属性的 type/      
	 * typmod/collation 插进去，使 exprType 等报告正确。      
	 * （我们预期最终换入的默认表达式实际上就具有  
	 * 此类型和 typmod。排序规则多半无关紧要，但我们 
	 * 还是把它设对。）另外，拒绝用 DEFAULT 去更新     
	 * 子字段/数组元素；列的部分无默认值                      
	 *                                                     
	 */
	if (expr && IsA(expr, SetToDefault))
	{
		SetToDefault *def = (SetToDefault *) expr;

		def->typeId = attrtype;
		def->typeMod = attrtypmod;
		def->collation = attrcollation;
		if (indirection)
		{
			if (IsA(linitial(indirection), A_Indices))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot set an array element to DEFAULT"),
						 parser_errposition(pstate, location)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot set a subfield to DEFAULT"),
						 parser_errposition(pstate, location)));
		}
	}

	/* 现在能安全用 exprType()。  */
	type_id = exprType((Node *) expr);

	/*
	 * 若目标列上有间接引用，则准备数组或子字段     
	 * 赋值表达式。这会生成一个已插入源值的新列值，    
	 * 随后可把它放进 INSERT 或 UPDATE 构造的新元组中。      
	 *                                                  
	 */
	if (indirection)
	{
		Node	   *colVar;

		if (pstate->p_is_insert)
		{
			/*
			 * 命令是 INSERT INTO table (col.something) ...，故        
			 * 并无真正可用的源值。插入 NULL 常量        
			 * 作为源值。              
			 */
			colVar = (Node *) makeNullConst(attrtype, attrtypmod,
											attrcollation);
		}
		else
		{
			/*
			 * 为待更新的列构建一个 Var。    
			 */
			Var		   *var;

			var = makeVar(pstate->p_target_nsitem->p_rtindex, attrno,
						  attrtype, attrtypmod, attrcollation, 0);
			var->location = location;

			colVar = (Node *) var;
		}

		expr = (Expr *)
			transformAssignmentIndirection(pstate,
										   colVar,
										   colname,
										   false,
										   attrtype,
										   attrtypmod,
										   attrcollation,
										   indirection,
										   list_head(indirection),
										   (Node *) expr,
										   COERCION_ASSIGNMENT,
										   location);
	}
	else
	{
		/*
		 * 对普通非限定目标列做类型检查和强制转换   
		 *          
		 */
		Node	   *orig_expr = (Node *) expr;

		expr = (Expr *)
			coerce_to_target_type(pstate,
								  orig_expr, type_id,
								  attrtype, attrtypmod,
								  COERCION_ASSIGNMENT,
								  COERCE_IMPLICIT_CAST,
								  -1);
		if (expr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" is of type %s"
							" but expression is of type %s",
							colname,
							format_type_be(attrtype),
							format_type_be(type_id)),
					 errhint("You will need to rewrite or cast the expression."),
					 parser_errposition(pstate, exprLocation(orig_expr))));
	}

	pstate->p_expr_kind = sv_expr_kind;

	return expr;
}


/*
 * updateTargetListEntry()
 *	只用于 UPDATE 语句（及 ON CONFLICT DO UPDATE）。    
 *	它准备一个 UPDATE TargetEntry 用于给目标表的    
 *	某列赋值。这包括把给定值强制转换为目标列 
 *	的类型（如有必要），并处理附着在目标列本身   
 *	上的任何子字段名或下标。                          
 *	       
 *
 * pstate 解析状态
 * tle 待修改的目标列表条目    
 * colname 目标列名（即被赋值属性的名字）                
 * attrno 目标属性号         
 * indirection 目标列的下标/字段名（如有）        
 * location 错误游标位置（应指向列名），或 -1           
 */
void
updateTargetListEntry(ParseState *pstate,
					  TargetEntry *tle,
					  char *colname,
					  int attrno,
					  List *indirection,
					  int location)
{
	/* 按需修正表达式       */
	tle->expr = transformAssignedExpr(pstate,
									  tle->expr,
									  EXPR_KIND_UPDATE_TARGET,
									  colname,
									  attrno,
									  indirection,
									  location);

	/*
	 * 设置 resno 标识目标列 --- 重写器与规划器依赖它 
	 * 我们也设置 resname 来标识目标列，但只为调试目的；  
	 * 不应依赖它。（特别是在存储的规则中它可能已  
	 * 过时。）                                                          
	 */
	tle->resno = (AttrNumber) attrno;
	tle->resname = colname;
}


/*
 * 处理 INSERT/UPDATE/赋值中目标列的间接引用（字段    
 * 选择或下标）。本例程对多级间接引用递归处理，     
 * 但注意 indirection 列表中多个相邻的 A_Indices 节点会      
 * 被视为单个多维下标操作。                                      
 *           
 *
 * 初次调用时，basenode 在 UPDATE 中是目标列的 Var，        
 * INSERT 中是目标类型的空 Const，PL/pgSQL 赋值中是            
 * 目标变量的 Param。递归调用时 basenode 为 NULL，           
 * 表示需要时应构造一个替代节点。                   
 *
 * targetName 是我们要赋值的字段/子字段的名字，           
 * targetIsSubscripting 为 true 表示对它做下标，这                 
 * 用于报错。 
 *
 * targetTypeId、targetTypMod、targetCollation 给出被赋值        
 * 对象的数据类型和排序规则（起初是目标列，之后     
 * 是某子对象）。 
 *
 * indirection 是间接引用节点列表，indirection_cell 是           
 * 尚待处理的子列表起点。当其为 NULL 时递归结束，    
 * 可以直接强制转换并返回右侧值。    
 *
 * rhs 是待赋值的已变换值；注意它尚未被强制转换         
 * 成任何特定类型。       
 *
 * ccontext 是强制转换 rhs 时所用的强制转换级别。    
 * 普通语句中它是 COERCION_ASSIGNMENT，但 PL/pgSQL 使用  
 * 特殊值。    
 *
 * location 是任何错误的错误游标位置。（注意：它指向    
 * 目标子句的开头，如 "foo.bar[baz]" 中的 "foo"。之后       
 * 我们可能想给各间接引用单元标注自己的位置信息， 
 * 那时 location 参数或许可以去掉。）                 
 */
Node *
transformAssignmentIndirection(ParseState *pstate,
							   Node *basenode,
							   const char *targetName,
							   bool targetIsSubscripting,
							   Oid targetTypeId,
							   int32 targetTypMod,
							   Oid targetCollation,
							   List *indirection,
							   ListCell *indirection_cell,
							   Node *rhs,
							   CoercionContext ccontext,
							   int location)
{
	Node	   *result;
	List	   *subscripts = NIL;
	ListCell   *i;

	if (indirection_cell && !basenode)
	{
		/*
		 * 设置一个替换。我们为此滥用了 CaseTestExpr。        
		 * 这样做是安全的，因为在最终表达式里位于          
		 * CaseTestExpr 之上的节点只可能是 FieldStore 和          
		 * SubscriptingRef 节点。（树里可能还有其他内容， 
		 * 但都在那些类型的其他子字段内）  
		 */
		CaseTestExpr *ctest = makeNode(CaseTestExpr);

		ctest->typeId = targetTypeId;
		ctest->typeMod = targetTypMod;
		ctest->collation = targetCollation;
		basenode = (Node *) ctest;
	}

	/*
	 * 必须把字段选择操作与下标操作拆分开来。 
	 * 相邻的 A_Indices 节点必须被视为单个多维下标           
	 * 操作。                            
	 */
	for_each_cell(i, indirection, indirection_cell)
	{
		Node	   *n = lfirst(i);

		if (IsA(n, A_Indices))
			subscripts = lappend(subscripts, n);
		else if (IsA(n, A_Star))
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("row expansion via \"*\" is not supported here"),
					 parser_errposition(pstate, location)));
		}
		else
		{
			FieldStore *fstore;
			Oid			baseTypeId;
			int32		baseTypeMod;
			Oid			typrelid;
			AttrNumber	attnum;
			Oid			fieldTypeId;
			int32		fieldTypMod;
			Oid			fieldCollation;

			Assert(IsA(n, String));

			/* 在此字段选择之前先处理下标        */
			if (subscripts)
			{
				/* 递归，然后返回，因为已处理完  */
				return transformAssignmentSubscripts(pstate,
													 basenode,
													 targetName,
													 targetTypeId,
													 targetTypMod,
													 targetCollation,
													 subscripts,
													 indirection,
													 i,
													 rhs,
													 ccontext,
													 location);
			}

			/* 没有下标，故可在此处处理字段选择   */

			/*
			 * 查找复合类型，同时考虑到给我们的可能是  
			 * 复合类型之上的域。                  
			 */
			baseTypeMod = targetTypMod;
			baseTypeId = getBaseTypeAndTypmod(targetTypeId, &baseTypeMod);

			typrelid = typeidTypeRelid(baseTypeId);
			if (!typrelid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("cannot assign to field \"%s\" of column \"%s\" because its type %s is not a composite type",
								strVal(n), targetName,
								format_type_be(targetTypeId)),
						 parser_errposition(pstate, location)));

			attnum = get_attnum(typrelid, strVal(n));
			if (attnum == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("cannot assign to field \"%s\" of column \"%s\" because there is no such column in data type %s",
								strVal(n), targetName,
								format_type_be(targetTypeId)),
						 parser_errposition(pstate, location)));
			if (attnum < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("cannot assign to system column \"%s\"",
								strVal(n)),
						 parser_errposition(pstate, location)));

			get_atttypetypmodcoll(typrelid, attnum,
								  &fieldTypeId, &fieldTypMod, &fieldCollation);

			/* 递归以构造字段赋值所需的右侧值      */
			rhs = transformAssignmentIndirection(pstate,
												 NULL,
												 strVal(n),
												 false,
												 fieldTypeId,
												 fieldTypMod,
												 fieldCollation,
												 indirection,
												 lnext(indirection, i),
												 rhs,
												 ccontext,
												 location);

			/* 并构建 FieldStore 节点 */
			fstore = makeNode(FieldStore);
			fstore->arg = (Expr *) basenode;
			fstore->newvals = list_make1(rhs);
			fstore->fieldnums = list_make1_int(attnum);
			fstore->resulttype = baseTypeId;

			/*
			 * 若目标为域则施加约束；注意这不完全对：  
			 * 我们构建的表达式树会在仅填充或更新了这    
			 * 一个字段的复合值上检查域约束，可能导致   
			 * 不期望的失败。重写器会把对同一表列的所有   
			 * 子字段赋值合并起来，让域约束只在给    
			 * INSERT 或 UPDATE 想要的所有字段赋完值之后，      
			 * 才被检查一次。                                          
			 *                                               
			 */
			if (baseTypeId != targetTypeId)
				return coerce_to_domain((Node *) fstore,
										baseTypeId, baseTypeMod,
										targetTypeId,
										COERCION_IMPLICIT,
										COERCE_IMPLICIT_CAST,
										location,
										false);

			return (Node *) fstore;
		}
	}

	/* 如有尾部下标则处理之      */
	if (subscripts)
	{
		/* 递归，然后返回，因为已处理完  */
		return transformAssignmentSubscripts(pstate,
											 basenode,
											 targetName,
											 targetTypeId,
											 targetTypMod,
											 targetCollation,
											 subscripts,
											 indirection,
											 NULL,
											 rhs,
											 ccontext,
											 location);
	}

	/* 基本情形：将 RHS 强制转换成目标类型 */

	result = coerce_to_target_type(pstate,
								   rhs, exprType(rhs),
								   targetTypeId, targetTypMod,
								   ccontext,
								   COERCE_IMPLICIT_CAST,
								   -1);
	if (result == NULL)
	{
		if (targetIsSubscripting)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("subscripted assignment to \"%s\" requires type %s"
							" but expression is of type %s",
							targetName,
							format_type_be(targetTypeId),
							format_type_be(exprType(rhs))),
					 errhint("You will need to rewrite or cast the expression."),
					 parser_errposition(pstate, location)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("subfield \"%s\" is of type %s"
							" but expression is of type %s",
							targetName,
							format_type_be(targetTypeId),
							format_type_be(exprType(rhs))),
					 errhint("You will need to rewrite or cast the expression."),
					 parser_errposition(pstate, location)));
	}

	return result;
}

/*
 * transformAssignmentIndirection 的辅助函数：处理容器赋值    
 */
static Node *
transformAssignmentSubscripts(ParseState *pstate,
							  Node *basenode,
							  const char *targetName,
							  Oid targetTypeId,
							  int32 targetTypMod,
							  Oid targetCollation,
							  List *subscripts,
							  List *indirection,
							  ListCell *next_indirection,
							  Node *rhs,
							  CoercionContext ccontext,
							  int location)
{
	Node	   *result;
	SubscriptingRef *sbsref;
	Oid			containerType;
	int32		containerTypMod;
	Oid			typeNeeded;
	int32		typmodNeeded;
	Oid			collationNeeded;

	Assert(subscripts != NIL);

	/* 确定实际涉及的容器类型           */
	containerType = targetTypeId;
	containerTypMod = targetTypMod;
	transformContainerType(&containerType, &containerTypMod);

	/* 处理下标并确定 RHS 所需的类型             */
	sbsref = transformContainerSubscripts(pstate,
										  basenode,
										  containerType,
										  containerTypMod,
										  subscripts,
										  true);

	typeNeeded = sbsref->refrestype;
	typmodNeeded = sbsref->reftypmod;

	/*
	 * 容器通常与元素具有相同的排序规则，但有例外：   
	 * 我们可能在对容器类型之上的域做下标操作。那种     
	 * 情况下应使用基本类型的排序规则。（对于任意的     
	 * 下标语义这并不可靠，但这无关紧要，因为我们     
	 * 只用它标记可能的 CaseTestExpr 的排序规则。）      
	 */
	if (containerType == targetTypeId)
		collationNeeded = targetCollation;
	else
		collationNeeded = get_typcollation(containerType);

	/* 递归以构造容器赋值所需的右侧值          */
	rhs = transformAssignmentIndirection(pstate,
										 NULL,
										 targetName,
										 true,
										 typeNeeded,
										 typmodNeeded,
										 collationNeeded,
										 indirection,
										 next_indirection,
										 rhs,
										 ccontext,
										 location);

	/*
	 * 把已正确强制转换的 RHS 插入 SubscriptingRef。然后        
	 * 把 refrestype 和 reftypmod 设回容器类型的值。         
	 */
	sbsref->refassgnexpr = (Expr *) rhs;
	sbsref->refrestype = containerType;
	sbsref->reftypmod = containerTypMod;

	result = (Node *) sbsref;

	/*
	 * 若目标是容器之上的域，则需要强制转换到该域。     
	 * 与 transformAssignmentIndirection 一样，若查询给容器的      
	 * 多个元素赋值，这种强制转换就为时过早；但我们     
	 * 重写时修正。   
	 */
	if (containerType != targetTypeId)
	{
		Oid			resulttype = exprType(result);

		result = coerce_to_target_type(pstate,
									   result, resulttype,
									   targetTypeId, targetTypMod,
									   ccontext,
									   COERCE_IMPLICIT_CAST,
									   -1);
		/* 若为 int2vector/oidvector 可能失败，但对真域不会     */
		if (result == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CANNOT_COERCE),
					 errmsg("cannot cast type %s to %s",
							format_type_be(resulttype),
							format_type_be(targetTypeId)),
					 parser_errposition(pstate, location)));
	}

	return result;
}


/*
 * checkInsertTargets -
 *	  若无则生成 INSERT 的列目标，或检查所给列名 
 *	  确保它们都在目标表中。                                
 *	  同时返回列属性号的整数列表。                    
 */
List *
checkInsertTargets(ParseState *pstate, List *cols, List **attrnos)
{
	*attrnos = NIL;

	if (cols == NIL)
	{
		/*
		 * 为 INSERT 生成默认列列表。     
		 */
		int			numcol = RelationGetNumberOfAttributes(pstate->p_target_relation);

		int			i;

		for (i = 0; i < numcol; i++)
		{
			ResTarget  *col;
			Form_pg_attribute attr;

			attr = TupleDescAttr(pstate->p_target_relation->rd_att, i);

			if (attr->attisdropped)
				continue;

			col = makeNode(ResTarget);
			col->name = pstrdup(NameStr(attr->attname));
			col->indirection = NIL;
			col->val = NULL;
			col->location = -1;
			cols = lappend(cols, col);
			*attrnos = lappend_int(*attrnos, i + 1);
		}
	}
	else
	{
		/*
		 * 对用户提供的 INSERT 列列表做初始验证。     
		 */
		Bitmapset  *wholecols = NULL;
		Bitmapset  *partialcols = NULL;
		ListCell   *tl;

		foreach(tl, cols)
		{
			ResTarget  *col = (ResTarget *) lfirst(tl);
			char	   *name = col->name;
			int			attrno;

			/* 查找列名，失败则 ereport       */
			attrno = attnameAttNum(pstate->p_target_relation, name, false);
			if (attrno == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" does not exist",
								name,
								RelationGetRelationName(pstate->p_target_relation)),
						 parser_errposition(pstate, col->location)));

			/*
			 * 检查重复，但只针对整列 --- 我们允许          
			 * INSERT INTO foo (col.subcol1, col.subcol2)
			 */
			if (col->indirection == NIL)
			{
				/* 整列：不得有任何其他赋值             */
				if (bms_is_member(attrno, wholecols) ||
					bms_is_member(attrno, partialcols))
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
							 errmsg("column \"%s\" specified more than once",
									name),
							 parser_errposition(pstate, col->location)));
				wholecols = bms_add_member(wholecols, attrno);
			}
			else
			{
				/* 部分列：不得有任何整列赋值            */
				if (bms_is_member(attrno, wholecols))
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
							 errmsg("column \"%s\" specified more than once",
									name),
							 parser_errposition(pstate, col->location)));
				partialcols = bms_add_member(partialcols, attrno);
			}

			*attrnos = lappend_int(*attrnos, attrno);
		}
	}

	return cols;
}

/*
 * ExpandColumnRefStar()
 *		把 foo.* 变换成表达式列表或目标列表条目。        
 *
 * 这处理 '*' 作为 ColumnRef 中最后一项或唯一项出现     
 * 的情形。该代码由两种情形共享：SELECT 目标列表顶层   
 * 的 foo.*（此时结果中要 TargetEntry 节点），以及 ROW()    
 * 或 VALUES() 中的 foo.*（此时只要裸表达式）。         
 *              
 *
 * 被引用的列被标记为需要 SELECT 权限。           
 */
static List *
ExpandColumnRefStar(ParseState *pstate, ColumnRef *cref,
					bool make_target_entry)
{
	List	   *fields = cref->fields;
	int			numnames = list_length(fields);

	if (numnames == 1)
	{
		/*
		 * 目标项是裸 '*'，展开所有表       
		 *
		 * (如 SELECT * FROM emp, dept)  
		 *
		 * 由于语法只在 SELECT 顶层接受裸 '*'，此处无需处理
		 * make_target_entry==false 的情形。                  
		 */
		Assert(make_target_entry);
		return ExpandAllTables(pstate, cref->location);
	}
	else
	{
		/*
		 * 目标项是 relation.*，展开该表      
		 *
		 * (如 SELECT emp.*, dname FROM emp, dept)  
		 *
		 * 注意：这段代码与 transformColumnRef 颇为相似；       
		 * 很诱人的做法是改为调用它，然后把得到的整行    
		 * Var 换成一个 Var 列表。但那样会使关系的           
		 * selectedCols 位图既显示整行需要 select 权限，  
		 * 又显示各列需要权限。这不正确（因为之后    
		 * 新增的列不应需要 select 权限）。              
		 * 本可以事后删除整行权限位，但复制代码没那么   
		 * 凌乱。                                          
		 */
		char	   *nspname = NULL;
		char	   *relname = NULL;
		ParseNamespaceItem *nsitem = NULL;
		int			levels_up;
		enum
		{
			CRSERR_NO_RTE,
			CRSERR_WRONG_DB,
			CRSERR_TOO_MANY
		}			crserr = CRSERR_NO_RTE;

		/*
		 * 让 PreParseColumnRefHook（如有）先处理。若它返回     
		 * 非空值，就应使用该表达式。     
		 */
		if (pstate->p_pre_columnref_hook != NULL)
		{
			Node	   *node;

			node = pstate->p_pre_columnref_hook(pstate, cref);
			if (node != NULL)
				return ExpandRowReference(pstate, node, make_target_entry);
		}

		switch (numnames)
		{
			case 2:
				relname = strVal(linitial(fields));
				nsitem = refnameNamespaceItem(pstate, nspname, relname,
											  cref->location,
											  &levels_up);
				break;
			case 3:
				nspname = strVal(linitial(fields));
				relname = strVal(lsecond(fields));
				nsitem = refnameNamespaceItem(pstate, nspname, relname,
											  cref->location,
											  &levels_up);
				break;
			case 4:
				{
					char	   *catname = strVal(linitial(fields));

					/*
					 * 检查系统目录名，然后忽略它。   
					 */
					if (strcmp(catname, get_database_name(MyDatabaseId)) != 0)
					{
						crserr = CRSERR_WRONG_DB;
						break;
					}
					nspname = strVal(lsecond(fields));
					relname = strVal(lthird(fields));
					nsitem = refnameNamespaceItem(pstate, nspname, relname,
												  cref->location,
												  &levels_up);
					break;
				}
			default:
				crserr = CRSERR_TOO_MANY;
				break;
		}

		/*
		 * 现在给 PostParseColumnRefHook（如有）一个机会。我们
		 * 耍点花招：传 RangeTblEntry 而非 Var 作翻译。   
		 * （单个 Var 本就不严格正确。该约定让              
		 * 真正关心的钩子知道正在发生什么。传 nsitem   
		 * 也许更好，但须把该结构升级成完整的 Node 类型， 
		 * 被调函数才能识别其类型。）                        
		 *                          
		 */
		if (pstate->p_post_columnref_hook != NULL)
		{
			Node	   *node;

			node = pstate->p_post_columnref_hook(pstate, cref,
												 (Node *) (nsitem ? nsitem->p_rte : NULL));
			if (node != NULL)
			{
				if (nsitem != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_AMBIGUOUS_COLUMN),
							 errmsg("column reference \"%s\" is ambiguous",
									NameListToString(cref->fields)),
							 parser_errposition(pstate, cref->location)));
				return ExpandRowReference(pstate, node, make_target_entry);
			}
		}

		/*
		 * 找不到翻译就报错。         
		 */
		if (nsitem == NULL)
		{
			switch (crserr)
			{
				case CRSERR_NO_RTE:
					errorMissingRTE(pstate, makeRangeVar(nspname, relname,
														 cref->location));
					break;
				case CRSERR_WRONG_DB:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cross-database references are not implemented: %s",
									NameListToString(cref->fields)),
							 parser_errposition(pstate, cref->location)));
					break;
				case CRSERR_TOO_MANY:
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("improper qualified name (too many dotted names): %s",
									NameListToString(cref->fields)),
							 parser_errposition(pstate, cref->location)));
					break;
			}
		}

		/*
		 * 将 nsitem 展开为各字段。  
		 */
		return ExpandSingleTable(pstate, nsitem, levels_up, cref->location,
								 make_target_entry);
	}
}

/*
 * ExpandAllTables()
 *		把 '*'（目标列表中）变换成目标列表条目列表。      
 *
 * 为每个对非限定列名可见的关系生成目标列表条目。
 * 我们不考虑仅以限定名可见的条目，因为那会包括         
 * 无别名 JOIN 的输入表、NEW/OLD 伪条目等。                      
 *     
 *
 * 被引用的关系/列被标记为需要 SELECT 权限。              
 */
static List *
ExpandAllTables(ParseState *pstate, int location)
{
	List	   *target = NIL;
	bool		found_table = false;
	ListCell   *l;

	foreach(l, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);

		/* 忽略仅表项         */
		if (!nsitem->p_cols_visible)
			continue;
		/* 解析目标列表时不应有任何仅 lateral 项            */
		Assert(!nsitem->p_lateral_only);
		/* 记住发现了 p_cols_visible 项      */
		found_table = true;

		target = list_concat(target,
							 expandNSItemAttrs(pstate,
											   nsitem,
											   0,
											   true,
											   location));
	}

	/*
	 * 检查 "SELECT *;"。这样做而非检查 target == NIL，        
	 * 因为我们要允许 SELECT * FROM 作用于零列的表          
	 *       
	 */
	if (!found_table)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("SELECT * with no tables specified is not valid"),
				 parser_errposition(pstate, location)));

	return target;
}

/*
 * ExpandIndirectionStar()
 *		把 foo.* 变换成表达式列表或目标列表条目。        
 *
 * 这处理 '*' 作为 A_Indirection 最后一项出现的情形。         
 * 该代码由两种情形共享：SELECT 目标列表顶层的 foo.*      
 * （此时结果中要 TargetEntry 节点），以及 ROW() 或 VALUES() 
 * 中的 foo.*（此时只要裸表达式）。为稳健起见，用  
 * 单独的 "make_target_entry" 标志来控制，而不依赖         
 * exprKind。                          
 */
static List *
ExpandIndirectionStar(ParseState *pstate, A_Indirection *ind,
					  bool make_target_entry, ParseExprKind exprKind)
{
	Node	   *expr;

	/* 剥去 '*' 以创建对行类型对象的引用               */
	ind = copyObject(ind);
	ind->indirection = list_truncate(ind->indirection,
									 list_length(ind->indirection) - 1);

	/* 然后变换它    */
	expr = transformExpr(pstate, (Node *) ind, exprKind);

	/* 把行类型表达式展开成各个字段           */
	return ExpandRowReference(pstate, expr, make_target_entry);
}

/*
 * ExpandSingleTable()
 *		把 foo.* 变换成表达式列表或目标列表条目。        
 *
 * 这处理 foo 已被确定为对 RTE 的简单引用的情形，  
 * 因此直接为表达式生成 Var 即可。                          
 *
 * 被引用的列被标记为需要 SELECT 权限。           
 */
static List *
ExpandSingleTable(ParseState *pstate, ParseNamespaceItem *nsitem,
				  int sublevels_up, int location, bool make_target_entry)
{
	if (make_target_entry)
	{
		/* expandNSItemAttrs 会处理权限标记       */
		return expandNSItemAttrs(pstate, nsitem, sublevels_up, true, location);
	}
	else
	{
		RangeTblEntry *rte = nsitem->p_rte;
		RTEPermissionInfo *perminfo = nsitem->p_perminfo;
		List	   *vars;
		ListCell   *l;

		vars = expandNSItemVars(pstate, nsitem, sublevels_up, location, NULL);

		/*
		 * 需要对该表的读权限。这通常与下方的               
		 * markVarForSelectPriv 调用重复，但表无列时例外。       
		 * 若 nsitem 属于连接则无需做任何事：其成员表       
		 * 加入范围表时已被标记 ACL_SELECT。（这只影响     
		 * UPDATE/DELETE 的目标关系，它无法处于连接下。）  
		 *                                                                 
		 */
		if (rte->rtekind == RTE_RELATION)
		{
			Assert(perminfo != NULL);
			perminfo->requiredPerms |= ACL_SELECT;
		}

		/* 需要对每列的读权限        */
		foreach(l, vars)
		{
			Var		   *var = (Var *) lfirst(l);

			markVarForSelectPriv(pstate, var);
		}

		return vars;
	}
}

/*
 * ExpandRowReference()
 *		把 foo.* 变换成表达式列表或目标列表条目。        
 *
 * 这处理 foo 是复合类型的任意表达式的情形。            
 *      
 */
static List *
ExpandRowReference(ParseState *pstate, Node *expr,
				   bool make_target_entry)
{
	List	   *result = NIL;
	TupleDesc	tupleDesc;
	int			numAttrs;
	int			i;

	/*
	 * 若行类型表达式是整行 Var，我们可以把字段展开成   
	 * 简单 Var。注意：若 RTE 是关系，这种情形会让我们  
	 * 的 RTEPermissionInfo 的 selectedCols 位图既显示整行          
	 * 需要 select 权限，又显示各个列也需要。           
	 * 不过，只有 (table.*).* 这类奇怪记法才会走到这里，   
	 * 所以不值得去清理 --- 可以说，这类情形的权限标记   
	 * 本来就是对的。            
	 */
	if (IsA(expr, Var) &&
		((Var *) expr)->varattno == InvalidAttrNumber)
	{
		Var		   *var = (Var *) expr;
		ParseNamespaceItem *nsitem;

		nsitem = GetNSItemByRangeTablePosn(pstate, var->varno, var->varlevelsup);
		return ExpandSingleTable(pstate, nsitem, var->varlevelsup, var->location, make_target_entry);
	}

	/*
	 * 否则只能用笨办法。我们当前的实现是生成表达式的  
	 * 多个副本，然后做 FieldSelect。                           
	 * （若表达式包含不平凡的计算，这可能相当低效。）
	 *                  
	 *
	 * 验证它是复合类型，并取得 tupdesc。   
	 * get_expr_result_tupdesc() 用来处理很方便。  
	 *
	 * 若是 RECORD 类型的 Var，就必须更费力气：必须找出   
	 * Var 引用的对象并把它传给 get_expr_result_tupdesc。         
	 * 该任务由 expandRecordVariable() 处理。  
	 */
	if (IsA(expr, Var) &&
		((Var *) expr)->vartype == RECORDOID)
		tupleDesc = expandRecordVariable(pstate, (Var *) expr, 0);
	else
		tupleDesc = get_expr_result_tupdesc(expr, false);
	Assert(tupleDesc);

	/* 生成对各个字段的引用的列表                */
	numAttrs = tupleDesc->natts;
	for (i = 0; i < numAttrs; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDesc, i);
		FieldSelect *fselect;

		if (att->attisdropped)
			continue;

		fselect = makeNode(FieldSelect);
		fselect->arg = (Expr *) copyObject(expr);
		fselect->fieldnum = i + 1;
		fselect->resulttype = att->atttypid;
		fselect->resulttypmod = att->atttypmod;
		/* 记下属性排序规则供 parse_collate.c 用*/
		fselect->resultcollid = att->attcollation;

		if (make_target_entry)
		{
			/* 为 TargetEntry 加装饰  */
			TargetEntry *te;

			te = makeTargetEntry((Expr *) fselect,
								 (AttrNumber) pstate->p_next_resno++,
								 pstrdup(NameStr(att->attname)),
								 false);
			result = lappend(result, te);
		}
		else
			result = lappend(result, fselect);
	}

	return result;
}

/*
 * expandRecordVariable
 *		尽可能获取 RECORD 类型 Var 的元组描述符。        
 *
 * 由于实际的表或视图列均不允许为 RECORD 类型，故这种  
 * Var 必须引用 JOIN、FUNCTION RTE 或子查询输出。我们       
 * 要一路下钻找出最终的定义表达式，并尝试从中推断   
 * tupdesc。若无法确定 tupdesc，我们会 ereport。           
 *
 * levelsup 是额外偏移量，用于在递归时正确解释 Var 的    
 * varlevelsup。外部调用者应传入零。      
 */
TupleDesc
expandRecordVariable(ParseState *pstate, Var *var, int levelsup)
{
	TupleDesc	tupleDesc;
	int			netlevelsup;
	RangeTblEntry *rte;
	AttrNumber	attnum;
	Node	   *expr;

	/* 确认调用方未出错       */
	Assert(IsA(var, Var));
	Assert(var->vartype == RECORDOID);

	/*
	 * 注意：此处似乎可改用 GetNSItemByRangeTablePosn，从而   
	 * 用 expandNSItemVars 代替 expandRTE；但这在下方某些递归  
	 * 场景中行不通，因这些场景构造的 ParseState 缺少 
	 * p_namespace 数据。                  
	 */
	netlevelsup = var->varlevelsup + levelsup;
	rte = GetRTEByRangeTablePosn(pstate, var->varno, netlevelsup);
	attnum = var->varattno;

	if (attnum == InvalidAttrNumber)
	{
		/* 对 RTE 的整行引用，故展开已知字段           */
		List	   *names,
				   *vars;
		ListCell   *lname,
				   *lvar;
		int			i;

		expandRTE(rte, var->varno, 0, var->location, false,
				  &names, &vars);

		tupleDesc = CreateTemplateTupleDesc(list_length(vars));
		i = 1;
		forboth(lname, names, lvar, vars)
		{
			char	   *label = strVal(lfirst(lname));
			Node	   *varnode = (Node *) lfirst(lvar);

			TupleDescInitEntry(tupleDesc, i,
							   label,
							   exprType(varnode),
							   exprTypmod(varnode),
							   0);
			TupleDescInitEntryCollation(tupleDesc, i,
										exprCollation(varnode));
			i++;
		}
		Assert(lname == NULL && lvar == NULL);	/* 列表等长吗？ */

		return tupleDesc;
	}

	expr = (Node *) var;		/* 无法下钻时的默认值    */

	switch (rte->rtekind)
	{
		case RTE_RELATION:
		case RTE_VALUES:
		case RTE_NAMEDTUPLESTORE:
		case RTE_RESULT:

			/*
			 * 这种情况不应出现：表、values 列表或 ENR 的列 
			 * 不应具有 RECORD 类型。随后落到下方，多半       
			 * 会在那里失败。 
			 */
			break;
		case RTE_SUBQUERY:
			{
				/* FROM 子查询：检查子查询的输出表达式  */
				TargetEntry *ste = get_tle_by_resno(rte->subquery->targetList,
													attnum);

				if (ste == NULL || ste->resjunk)
					elog(ERROR, "subquery %s does not have attribute %d",
						 rte->eref->aliasname, attnum);
				expr = (Node *) ste->expr;
				if (IsA(expr, Var))
				{
					/*
					 * 递归进入子查询，看看其 Var 到底引用    
					 * 什么。必须再构建一层 ParseState，用于     
					 * 同步子查询的 varlevelsup；另外子查询  
					 * RTE 可能来自外层查询级，此时子查询   
					 * 的 ParseState 必须以该外层层级为父    
					 *                                                
					 */
					ParseState	mypstate = {0};
					Index		levelsup;

					/* GetRTEByRangeTablePosn 已命中，循环必能命中 */
					for (levelsup = 0; levelsup < netlevelsup; levelsup++)
						pstate = pstate->parentParseState;
					mypstate.parentParseState = pstate;
					mypstate.p_rtable = rte->subquery->rtable;
					/* 无需费力填充伪 pstate 的其余部分     */

					return expandRecordVariable(&mypstate, (Var *) expr, 0);
				}
				/* 否则继续向下检查表达式           */
			}
			break;
		case RTE_JOIN:
			/* Join RTE：递归检查别名变量                 */
			Assert(attnum > 0 && attnum <= list_length(rte->joinaliasvars));
			expr = (Node *) list_nth(rte->joinaliasvars, attnum - 1);
			Assert(expr != NULL);
			/* 此处刻意不移除隐式类型强制转换        */
			if (IsA(expr, Var))
				return expandRecordVariable(pstate, (Var *) expr, netlevelsup);
			/* 否则继续向下检查表达式           */
			break;
		case RTE_FUNCTION:

			/*
			 * 除非函数被声明为某个结果列是 RECORD，          
			 * 而这不被允许，否则不会到这里。      
			 */
			break;
		case RTE_TABLEFUNC:

			/*
			 * 表函数无法拥有 RECORD 类型的列。        
			 */
			break;
		case RTE_CTE:
			/* CTE 引用：检查子查询输出表达式   */
			if (!rte->self_reference)
			{
				CommonTableExpr *cte = GetCTEForRTE(pstate, rte, netlevelsup);
				TargetEntry *ste;

				ste = get_tle_by_resno(GetCTETargetList(cte), attnum);
				if (ste == NULL || ste->resjunk)
					elog(ERROR, "CTE %s does not have attribute %d",
						 rte->eref->aliasname, attnum);
				expr = (Node *) ste->expr;
				if (IsA(expr, Var))
				{
					/*
					 * 递归进入 CTE，看其 Var 引用了什么。要   
					 * 再构建一层 ParseState，用于同步 CTE 内的   
					 * varlevelsup；它还可能是外层 CTE（对照   
					 * 上文 SUBQUERY 情形）。                        
					 */
					ParseState	mypstate = {0};
					Index		levelsup;

					/* GetCTEForRTE 已找到，循环必能找到 */
					for (levelsup = 0;
						 levelsup < rte->ctelevelsup + netlevelsup;
						 levelsup++)
						pstate = pstate->parentParseState;
					mypstate.parentParseState = pstate;
					mypstate.p_rtable = ((Query *) cte->ctequery)->rtable;
					/* 无需费力填充伪 pstate 的其余部分     */

					return expandRecordVariable(&mypstate, (Var *) expr, 0);
				}
				/* 否则继续向下检查表达式           */
			}
			break;
	}

	/*
	 * 至此得到再也无法展开的表达式，那就看看    
	 * get_expr_result_tupdesc() 能否处理它。      
	 */
	return get_expr_result_tupdesc(expr, false);
}


/*
 * FigureColname -
 *	  若结果列名字未在目标列表中指定，就得猜一个   
 *	  合适的名字。SQL 标准对此给出了一些指引，         
 *	  但不多……          
 *
 * 注意参数是目标项的*未转换*解析树。这比转换后      
 * 的解析树更好处理一点。                                    
 */
char *
FigureColname(Node *node)
{
	char	   *name = NULL;

	(void) FigureColnameInternal(node, &name);
	if (name != NULL)
		return name;
	/* 猜不出任何名字时的默认结果   */
	return "?column?";
}

/*
 * FigureIndexColname -
 *	  为索引中的表达式列挑选名字。          
 *
 * 这其实与 FigureColname 几乎相同，只是若挑不出好   
 * 名字就返回 NULL。   
 */
char *
FigureIndexColname(Node *node)
{
	char	   *name = NULL;

	(void) FigureColnameInternal(node, &name);
	return name;
}

/*
 * FigureColnameInternal -
 *	  FigureColname 的内部主力函数 
 *
 * 返回值表示对结果的信心强弱：              
 *		0 - 无信息     
 *		1 - 次优名字选择     
 *		2 - 好名字选择 
 * 该返回值实际只在内部使用。           
 * 若结果非零，则 *name 会被设为选定的名字。
 */
static int
FigureColnameInternal(Node *node, char **name)
{
	int			strength = 0;

	if (node == NULL)
		return strength;

	switch (nodeTag(node))
	{
		case T_ColumnRef:
			{
				char	   *fname = NULL;
				ListCell   *l;

				/* 如有则取最后字段名，忽略 "*"   */
				foreach(l, ((ColumnRef *) node)->fields)
				{
					Node	   *i = lfirst(l);

					if (IsA(i, String))
						fname = strVal(i);
				}
				if (fname)
				{
					*name = fname;
					return 2;
				}
			}
			break;
		case T_A_Indirection:
			{
				A_Indirection *ind = (A_Indirection *) node;
				char	   *fname = NULL;
				ListCell   *l;

				/* 如有则取最后字段名，忽略 "*" 和下标        */
				foreach(l, ind->indirection)
				{
					Node	   *i = lfirst(l);

					if (IsA(i, String))
						fname = strVal(i);
				}
				if (fname)
				{
					*name = fname;
					return 2;
				}
				return FigureColnameInternal(ind->arg, name);
			}
			break;
		case T_FuncCall:
			*name = strVal(llast(((FuncCall *) node)->funcname));
			return 2;
		case T_A_Expr:
			if (((A_Expr *) node)->kind == AEXPR_NULLIF)
			{
				/* 让 nullif() 表现得像普通函数     */
				*name = "nullif";
				return 2;
			}
			break;
		case T_TypeCast:
			strength = FigureColnameInternal(((TypeCast *) node)->arg,
											 name);
			if (strength <= 1)
			{
				if (((TypeCast *) node)->typeName != NULL)
				{
					*name = strVal(llast(((TypeCast *) node)->typeName->names));
					return 1;
				}
			}
			break;
		case T_CollateClause:
			return FigureColnameInternal(((CollateClause *) node)->arg, name);
		case T_GroupingFunc:
			/* 让 GROUPING() 表现得像普通函数     */
			*name = "grouping";
			return 2;
		case T_SubLink:
			switch (((SubLink *) node)->subLinkType)
			{
				case EXISTS_SUBLINK:
					*name = "exists";
					return 2;
				case ARRAY_SUBLINK:
					*name = "array";
					return 2;
				case EXPR_SUBLINK:
					{
						/* 获取子查询唯一目标条目的列名      */
						SubLink    *sublink = (SubLink *) node;
						Query	   *query = (Query *) sublink->subselect;

						/*
						 * 该子查询多半已被转换过，               
						 * 但仍需谨慎检查。（能看到转换       
						 * 后的子查询，是因为 transformSubLink  
						 * 生性懒惰，会就地修改 SubLink           
						 * 节点。）   
						 */
						if (IsA(query, Query))
						{
							TargetEntry *te = (TargetEntry *) linitial(query->targetList);

							if (te->resname)
							{
								*name = te->resname;
								return 2;
							}
						}
					}
					break;
					/* 与其他形似操作符的节点一样，没有名字 */
				case MULTIEXPR_SUBLINK:
				case ALL_SUBLINK:
				case ANY_SUBLINK:
				case ROWCOMPARE_SUBLINK:
				case CTE_SUBLINK:
					break;
			}
			break;
		case T_CaseExpr:
			strength = FigureColnameInternal((Node *) ((CaseExpr *) node)->defresult,
											 name);
			if (strength <= 1)
			{
				*name = "case";
				return 1;
			}
			break;
		case T_A_ArrayExpr:
			/* 让 ARRAY[] 表现得像函数   */
			*name = "array";
			return 2;
		case T_RowExpr:
			/* 让 ROW() 表现得像函数   */
			*name = "row";
			return 2;
		case T_CoalesceExpr:
			/* 让 coalesce() 表现得像普通函数     */
			*name = "coalesce";
			return 2;
		case T_MinMaxExpr:
			/* 让 greatest/least 表现得像普通函数     */
			switch (((MinMaxExpr *) node)->op)
			{
				case IS_GREATEST:
					*name = "greatest";
					return 2;
				case IS_LEAST:
					*name = "least";
					return 2;
			}
			break;
		case T_SQLValueFunction:
			/* 让它们表现得像函数或变量       */
			switch (((SQLValueFunction *) node)->op)
			{
				case SVFOP_CURRENT_DATE:
					*name = "current_date";
					return 2;
				case SVFOP_CURRENT_TIME:
				case SVFOP_CURRENT_TIME_N:
					*name = "current_time";
					return 2;
				case SVFOP_CURRENT_TIMESTAMP:
				case SVFOP_CURRENT_TIMESTAMP_N:
					*name = "current_timestamp";
					return 2;
				case SVFOP_LOCALTIME:
				case SVFOP_LOCALTIME_N:
					*name = "localtime";
					return 2;
				case SVFOP_LOCALTIMESTAMP:
				case SVFOP_LOCALTIMESTAMP_N:
					*name = "localtimestamp";
					return 2;
				case SVFOP_CURRENT_ROLE:
					*name = "current_role";
					return 2;
				case SVFOP_CURRENT_USER:
					*name = "current_user";
					return 2;
				case SVFOP_USER:
					*name = "user";
					return 2;
				case SVFOP_SESSION_USER:
					*name = "session_user";
					return 2;
				case SVFOP_CURRENT_CATALOG:
					*name = "current_catalog";
					return 2;
				case SVFOP_CURRENT_SCHEMA:
					*name = "current_schema";
					return 2;
			}
			break;
		case T_XmlExpr:
			/* 让 SQL/XML 函数表现得像普通函数         */
			switch (((XmlExpr *) node)->op)
			{
				case IS_XMLCONCAT:
					*name = "xmlconcat";
					return 2;
				case IS_XMLELEMENT:
					*name = "xmlelement";
					return 2;
				case IS_XMLFOREST:
					*name = "xmlforest";
					return 2;
				case IS_XMLPARSE:
					*name = "xmlparse";
					return 2;
				case IS_XMLPI:
					*name = "xmlpi";
					return 2;
				case IS_XMLROOT:
					*name = "xmlroot";
					return 2;
				case IS_XMLSERIALIZE:
					*name = "xmlserialize";
					return 2;
				case IS_DOCUMENT:
					/* 无     */
					break;
			}
			break;
		case T_XmlSerialize:
			/* 让 XMLSERIALIZE 表现得像普通函数     */
			*name = "xmlserialize";
			return 2;
		case T_JsonObjectConstructor:
			/* 让 JSON_OBJECT 表现得像普通函数     */
			*name = "json_object";
			return 2;
		case T_JsonArrayConstructor:
		case T_JsonArrayQueryConstructor:
			/* 让 JSON_ARRAY 表现得像普通函数     */
			*name = "json_array";
			return 2;
		case T_JsonObjectAgg:
			/* 让 JSON_OBJECTAGG 表现得像普通函数     */
			*name = "json_objectagg";
			return 2;
		case T_JsonArrayAgg:
			/* 让 JSON_ARRAYAGG 表现得像普通函数     */
			*name = "json_arrayagg";
			return 2;
		default:
			break;
	}

	return strength;
}
