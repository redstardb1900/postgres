/*-------------------------------------------------------------------------
 *
 * parse_expr.c
 *	  解析器中处理表达式 
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_expr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"
#include "utils/xml.h"

/* GUC 参数     */
bool		Transform_null_equals = false;


static Node *transformExprRecurse(ParseState *pstate, Node *expr);
static Node *transformParamRef(ParseState *pstate, ParamRef *pref);
static Node *transformAExprOp(ParseState *pstate, A_Expr *a);
static Node *transformAExprOpAny(ParseState *pstate, A_Expr *a);
static Node *transformAExprOpAll(ParseState *pstate, A_Expr *a);
static Node *transformAExprDistinct(ParseState *pstate, A_Expr *a);
static Node *transformAExprNullIf(ParseState *pstate, A_Expr *a);
static Node *transformAExprIn(ParseState *pstate, A_Expr *a);
static Node *transformAExprBetween(ParseState *pstate, A_Expr *a);
static Node *transformBoolExpr(ParseState *pstate, BoolExpr *a);
static Node *transformFuncCall(ParseState *pstate, FuncCall *fn);
static Node *transformMultiAssignRef(ParseState *pstate, MultiAssignRef *maref);
static Node *transformCaseExpr(ParseState *pstate, CaseExpr *c);
static Node *transformSubLink(ParseState *pstate, SubLink *sublink);
static Node *transformArrayExpr(ParseState *pstate, A_ArrayExpr *a,
								Oid array_type, Oid element_type, int32 typmod);
static Node *transformRowExpr(ParseState *pstate, RowExpr *r, bool allowDefault);
static Node *transformCoalesceExpr(ParseState *pstate, CoalesceExpr *c);
static Node *transformMinMaxExpr(ParseState *pstate, MinMaxExpr *m);
static Node *transformSQLValueFunction(ParseState *pstate,
									   SQLValueFunction *svf);
static Node *transformXmlExpr(ParseState *pstate, XmlExpr *x);
static Node *transformXmlSerialize(ParseState *pstate, XmlSerialize *xs);
static Node *transformBooleanTest(ParseState *pstate, BooleanTest *b);
static Node *transformCurrentOfExpr(ParseState *pstate, CurrentOfExpr *cexpr);
static Node *transformColumnRef(ParseState *pstate, ColumnRef *cref);
static Node *transformWholeRowRef(ParseState *pstate,
								  ParseNamespaceItem *nsitem,
								  int sublevels_up, int location);
static Node *transformIndirection(ParseState *pstate, A_Indirection *ind);
static Node *transformTypeCast(ParseState *pstate, TypeCast *tc);
static Node *transformCollateClause(ParseState *pstate, CollateClause *c);
static Node *transformJsonObjectConstructor(ParseState *pstate,
											JsonObjectConstructor *ctor);
static Node *transformJsonArrayConstructor(ParseState *pstate,
										   JsonArrayConstructor *ctor);
static Node *transformJsonArrayQueryConstructor(ParseState *pstate,
												JsonArrayQueryConstructor *ctor);
static Node *transformJsonObjectAgg(ParseState *pstate, JsonObjectAgg *agg);
static Node *transformJsonArrayAgg(ParseState *pstate, JsonArrayAgg *agg);
static Node *transformJsonIsPredicate(ParseState *pstate, JsonIsPredicate *pred);
static Node *make_row_comparison_op(ParseState *pstate, List *opname,
									List *largs, List *rargs, int location);
static Node *make_row_distinct_op(ParseState *pstate, List *opname,
								  RowExpr *lrow, RowExpr *rrow, int location);
static Expr *make_distinct_op(ParseState *pstate, List *opname,
							  Node *ltree, Node *rtree, int location);
static Node *make_nulltest_from_distinct(ParseState *pstate,
										 A_Expr *distincta, Node *arg);


/*
 * transformExpr -
 *	  分析并转换表达式。类型检查与类型                    
 *	  转换都在这里完成。此处理把语法的原始          
 *	  输出转成语义完全确定的表达式树    
 */
Node *
transformExpr(ParseState *pstate, Node *expr, ParseExprKind exprKind)
{
	Node	   *result;
	ParseExprKind sv_expr_kind;

	/* 保存并恢复所解析表达式类型的标识           */
	Assert(exprKind != EXPR_KIND_NONE);
	sv_expr_kind = pstate->p_expr_kind;
	pstate->p_expr_kind = exprKind;

	result = transformExprRecurse(pstate, expr);

	pstate->p_expr_kind = sv_expr_kind;

	return result;
}

static Node *
transformExprRecurse(ParseState *pstate, Node *expr)
{
	Node	   *result;

	if (expr == NULL)
		return NULL;

	/* 防止过于复杂的表达式导致栈溢出                  */
	check_stack_depth();

	switch (nodeTag(expr))
	{
		case T_ColumnRef:
			result = transformColumnRef(pstate, (ColumnRef *) expr);
			break;

		case T_ParamRef:
			result = transformParamRef(pstate, (ParamRef *) expr);
			break;

		case T_A_Const:
			result = (Node *) make_const(pstate, (A_Const *) expr);
			break;

		case T_A_Indirection:
			result = transformIndirection(pstate, (A_Indirection *) expr);
			break;

		case T_A_ArrayExpr:
			result = transformArrayExpr(pstate, (A_ArrayExpr *) expr,
										InvalidOid, InvalidOid, -1);
			break;

		case T_TypeCast:
			result = transformTypeCast(pstate, (TypeCast *) expr);
			break;

		case T_CollateClause:
			result = transformCollateClause(pstate, (CollateClause *) expr);
			break;

		case T_A_Expr:
			{
				A_Expr	   *a = (A_Expr *) expr;

				switch (a->kind)
				{
					case AEXPR_OP:
						result = transformAExprOp(pstate, a);
						break;
					case AEXPR_OP_ANY:
						result = transformAExprOpAny(pstate, a);
						break;
					case AEXPR_OP_ALL:
						result = transformAExprOpAll(pstate, a);
						break;
					case AEXPR_DISTINCT:
					case AEXPR_NOT_DISTINCT:
						result = transformAExprDistinct(pstate, a);
						break;
					case AEXPR_NULLIF:
						result = transformAExprNullIf(pstate, a);
						break;
					case AEXPR_IN:
						result = transformAExprIn(pstate, a);
						break;
					case AEXPR_LIKE:
					case AEXPR_ILIKE:
					case AEXPR_SIMILAR:
						/* 这些可像 AEXPR_OP 一样转换        */
						result = transformAExprOp(pstate, a);
						break;
					case AEXPR_BETWEEN:
					case AEXPR_NOT_BETWEEN:
					case AEXPR_BETWEEN_SYM:
					case AEXPR_NOT_BETWEEN_SYM:
						result = transformAExprBetween(pstate, a);
						break;
					default:
						elog(ERROR, "unrecognized A_Expr kind: %d", a->kind);
						result = NULL;	/* 让编译器安静  */
						break;
				}
				break;
			}

		case T_BoolExpr:
			result = transformBoolExpr(pstate, (BoolExpr *) expr);
			break;

		case T_FuncCall:
			result = transformFuncCall(pstate, (FuncCall *) expr);
			break;

		case T_MultiAssignRef:
			result = transformMultiAssignRef(pstate, (MultiAssignRef *) expr);
			break;

		case T_GroupingFunc:
			result = transformGroupingFunc(pstate, (GroupingFunc *) expr);
			break;

		case T_NamedArgExpr:
			{
				NamedArgExpr *na = (NamedArgExpr *) expr;

				na->arg = (Expr *) transformExprRecurse(pstate, (Node *) na->arg);
				result = expr;
				break;
			}

		case T_SubLink:
			result = transformSubLink(pstate, (SubLink *) expr);
			break;

		case T_CaseExpr:
			result = transformCaseExpr(pstate, (CaseExpr *) expr);
			break;

		case T_RowExpr:
			result = transformRowExpr(pstate, (RowExpr *) expr, false);
			break;

		case T_CoalesceExpr:
			result = transformCoalesceExpr(pstate, (CoalesceExpr *) expr);
			break;

		case T_MinMaxExpr:
			result = transformMinMaxExpr(pstate, (MinMaxExpr *) expr);
			break;

		case T_SQLValueFunction:
			result = transformSQLValueFunction(pstate,
											   (SQLValueFunction *) expr);
			break;

		case T_XmlExpr:
			result = transformXmlExpr(pstate, (XmlExpr *) expr);
			break;

		case T_XmlSerialize:
			result = transformXmlSerialize(pstate, (XmlSerialize *) expr);
			break;

		case T_NullTest:
			{
				NullTest   *n = (NullTest *) expr;

				n->arg = (Expr *) transformExprRecurse(pstate, (Node *) n->arg);
				/* 实参可为任意类型，故不强制转换    */
				n->argisrow = type_is_rowtype(exprType((Node *) n->arg));
				result = expr;
				break;
			}

		case T_BooleanTest:
			result = transformBooleanTest(pstate, (BooleanTest *) expr);
			break;

		case T_CurrentOfExpr:
			result = transformCurrentOfExpr(pstate, (CurrentOfExpr *) expr);
			break;

			/*
			 * 凡 DEFAULT 合法之处，调用方都应已               
			 * 处理好它，而不是把它传给 transformExpr()。
			 */
		case T_SetToDefault:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("DEFAULT is not allowed in this context"),
					 parser_errposition(pstate,
										((SetToDefault *) expr)->location)));
			break;

			/*
			 * CaseTestExpr 无需任何处理；它只会            
			 * 以完全成形的形态注入解析树。        
			 *
			 * 通常我们不应在这里看到 Var，但对               
			 * transformJoinUsingClause() 直接创建这种未转换         
			 * 操作符树（Var 已转换）很方便。最好的
			 * 替代方案是先把列引用拆解再重建，          
			 * 这似乎是代价高昂的无谓之举。所以允许它。
			 */
		case T_CaseTestExpr:
		case T_Var:
			{
				result = (Node *) expr;
				break;
			}

		case T_JsonObjectConstructor:
			result = transformJsonObjectConstructor(pstate, (JsonObjectConstructor *) expr);
			break;

		case T_JsonArrayConstructor:
			result = transformJsonArrayConstructor(pstate, (JsonArrayConstructor *) expr);
			break;

		case T_JsonArrayQueryConstructor:
			result = transformJsonArrayQueryConstructor(pstate, (JsonArrayQueryConstructor *) expr);
			break;

		case T_JsonObjectAgg:
			result = transformJsonObjectAgg(pstate, (JsonObjectAgg *) expr);
			break;

		case T_JsonArrayAgg:
			result = transformJsonArrayAgg(pstate, (JsonArrayAgg *) expr);
			break;

		case T_JsonIsPredicate:
			result = transformJsonIsPredicate(pstate, (JsonIsPredicate *) expr);
			break;

		default:
			/* 不应到达此处    */
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(expr));
			result = NULL;		/* 让编译器安静  */
			break;
	}

	return result;
}

/*
 * 用于报告"列不存在"错误消息的辅助例程              
 *
 * （通常我们不必如此费事，但从任意节点进行字段     
 * 选择的一般情况则需要它。）    
 */
static void
unknown_attribute(ParseState *pstate, Node *relref, const char *attname,
				  int location)
{
	RangeTblEntry *rte;

	if (IsA(relref, Var) &&
		((Var *) relref)->varattno == InvalidAttrNumber)
	{
		/* 用别名而非实际表名来引用 RTE            */
		rte = GetRTEByRangeTablePosn(pstate,
									 ((Var *) relref)->varno,
									 ((Var *) relref)->varlevelsup);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column %s.%s does not exist",
						rte->eref->aliasname, attname),
				 parser_errposition(pstate, location)));
	}
	else
	{
		/* 只能通过表达式的类型来引用                  */
		Oid			relTypeId = exprType(relref);

		if (ISCOMPLEX(relTypeId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" not found in data type %s",
							attname, format_type_be(relTypeId)),
					 parser_errposition(pstate, location)));
		else if (relTypeId == RECORDOID)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("could not identify column \"%s\" in record data type",
							attname),
					 parser_errposition(pstate, location)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("column notation .%s applied to type %s, "
							"which is not a composite type",
							attname, format_type_be(relTypeId)),
					 parser_errposition(pstate, location)));
	}
}

static Node *
transformIndirection(ParseState *pstate, A_Indirection *ind)
{
	Node	   *last_srf = pstate->p_last_srf;
	Node	   *result = transformExprRecurse(pstate, ind->arg);
	List	   *subscripts = NIL;
	int			location = exprLocation(result);
	ListCell   *i;

	/*
	 * 必须把字段选择操作从下标操作中             
	 * 拆分出来。相邻的 A_Indices 节点必须当作单个           
	 * 多维下标操作来对待。       
	 */
	foreach(i, ind->indirection)
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
			Node	   *newresult;

			Assert(IsA(n, String));

			/* 在本字段选择之前先处理下标        */
			if (subscripts)
				result = (Node *) transformContainerSubscripts(pstate,
															   result,
															   exprType(result),
															   exprTypmod(result),
															   subscripts,
															   false);
			subscripts = NIL;

			newresult = ParseFuncOrColumn(pstate,
										  list_make1(n),
										  list_make1(result),
										  last_srf,
										  NULL,
										  false,
										  location);
			if (newresult == NULL)
				unknown_attribute(pstate, result, strVal(n), location);
			result = newresult;
		}
	}
	/* 处理尾部下标（若有）      */
	if (subscripts)
		result = (Node *) transformContainerSubscripts(pstate,
													   result,
													   exprType(result),
													   exprTypmod(result),
													   subscripts,
													   false);

	return result;
}

/*
 * 转换 ColumnRef。   
 *
 * 如果你要修改这段代码，另请参见 ExpandColumnRefStar。  
 */
static Node *
transformColumnRef(ParseState *pstate, ColumnRef *cref)
{
	Node	   *node = NULL;
	char	   *nspname = NULL;
	char	   *relname = NULL;
	char	   *colname = NULL;
	ParseNamespaceItem *nsitem;
	int			levels_up;
	enum
	{
		CRERR_NO_COLUMN,
		CRERR_NO_RTE,
		CRERR_WRONG_DB,
		CRERR_TOO_MANY
	}			crerr = CRERR_NO_COLUMN;
	const char *err;

	/*
	 * 检查列引用是否处于查询内的无效位置。我们允许    
	 * 大多数位置的列引用，但默认值表达式与              
	 * 分区边界表达式中除外。           
	 */
	err = NULL;
	switch (pstate->p_expr_kind)
	{
		case EXPR_KIND_NONE:
			Assert(false);		/* 不会发生 */
			break;
		case EXPR_KIND_OTHER:
		case EXPR_KIND_JOIN_ON:
		case EXPR_KIND_JOIN_USING:
		case EXPR_KIND_FROM_SUBSELECT:
		case EXPR_KIND_FROM_FUNCTION:
		case EXPR_KIND_WHERE:
		case EXPR_KIND_POLICY:
		case EXPR_KIND_HAVING:
		case EXPR_KIND_FILTER:
		case EXPR_KIND_WINDOW_PARTITION:
		case EXPR_KIND_WINDOW_ORDER:
		case EXPR_KIND_WINDOW_FRAME_RANGE:
		case EXPR_KIND_WINDOW_FRAME_ROWS:
		case EXPR_KIND_WINDOW_FRAME_GROUPS:
		case EXPR_KIND_SELECT_TARGET:
		case EXPR_KIND_INSERT_TARGET:
		case EXPR_KIND_UPDATE_SOURCE:
		case EXPR_KIND_UPDATE_TARGET:
		case EXPR_KIND_MERGE_WHEN:
		case EXPR_KIND_GROUP_BY:
		case EXPR_KIND_ORDER_BY:
		case EXPR_KIND_DISTINCT_ON:
		case EXPR_KIND_LIMIT:
		case EXPR_KIND_OFFSET:
		case EXPR_KIND_RETURNING:
		case EXPR_KIND_VALUES:
		case EXPR_KIND_VALUES_SINGLE:
		case EXPR_KIND_CHECK_CONSTRAINT:
		case EXPR_KIND_DOMAIN_CHECK:
		case EXPR_KIND_FUNCTION_DEFAULT:
		case EXPR_KIND_INDEX_EXPRESSION:
		case EXPR_KIND_INDEX_PREDICATE:
		case EXPR_KIND_STATS_EXPRESSION:
		case EXPR_KIND_ALTER_COL_TRANSFORM:
		case EXPR_KIND_EXECUTE_PARAMETER:
		case EXPR_KIND_TRIGGER_WHEN:
		case EXPR_KIND_PARTITION_EXPRESSION:
		case EXPR_KIND_CALL_ARGUMENT:
		case EXPR_KIND_COPY_WHERE:
		case EXPR_KIND_GENERATED_COLUMN:
		case EXPR_KIND_CYCLE_MARK:
			/* 好  */
			break;

		case EXPR_KIND_COLUMN_DEFAULT:
			err = _("cannot use column reference in DEFAULT expression");
			break;
		case EXPR_KIND_PARTITION_BOUND:
			err = _("cannot use column reference in partition bound expression");
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
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg_internal("%s", err),
				 parser_errposition(pstate, cref->location)));

	/*
	 * 若存在 PreParseColumnRefHook，先让它处理。若其返回  
	 * 非 NULL 则就此了结。     
	 */
	if (pstate->p_pre_columnref_hook != NULL)
	{
		node = pstate->p_pre_columnref_hook(pstate, cref);
		if (node != NULL)
			return node;
	}

	/*----------
	 * 允许的语法有：    
	 *
	 * A		先尝试解析为非限定列名；            
	 * 			若失败，再尝试解析为非限定表名（A.*）。 
	 * A.B		A 是非限定表名；B 可以是           
	 * 			列名或函数名（优先尝试列名）。     
	 * A.B.C	模式 A, 表 B, 列/函数 C。      
	 * A.B.C.D	目录 A, 模式 B, 表 C, 列/函数 D。  
	 * A.*		A 是非限定表名；表示整行。                
	 * A.B.*	模式 A 中表 B 的整行。         
	 * A.B.C.*	目录 A 中模式 B 下表 C 的整行。          
	 *
	 * 我们无需处理单独的"*"；只有 SELECT 列表              
	 * 顶层语法接受它，而且 transformTargetList                    
	 * 会在到达此处之前处理它。另外，"A.*" 等              
	 * 若出现在 SELECT 顶层，会被 transformTargetList 展开，     
	 * 所以这里只会把它们视为函数或操作符的输入。      
	 *
	 * 目前若给出了目录名，则它必须等于当前              
	 * 数据库名；在此检查后即予丢弃。       
	 *----------
	 */
	switch (list_length(cref->fields))
	{
		case 1:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);

				colname = strVal(field1);

				/* 尝试识别为非限定列名           */
				node = colNameToVar(pstate, colname, false, cref->location);

				if (node == NULL)
				{
					/*
					 * 它不是任何范围表条目的列。        
					 *
					 * 尝试把该名称当作关系查找。注意只有
					 * 已加入范围表的关系才会被识别。        
					 *            
					 *
					 * 这是为兼容 PostQUEL 风格语法的       
					 * 权宜做法。现在首选形式是"rel.*"。      
					 *         
					 */
					nsitem = refnameNamespaceItem(pstate, NULL, colname,
												  cref->location,
												  &levels_up);
					if (nsitem)
						node = transformWholeRowRef(pstate, nsitem, levels_up,
													cref->location);
				}
				break;
			}
		case 2:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);
				Node	   *field2 = (Node *) lsecond(cref->fields);

				relname = strVal(field1);

				/* 定位被引用的 nsitem    */
				nsitem = refnameNamespaceItem(pstate, nspname, relname,
											  cref->location,
											  &levels_up);
				if (nsitem == NULL)
				{
					crerr = CRERR_NO_RTE;
					break;
				}

				/* 整行引用？      */
				if (IsA(field2, A_Star))
				{
					node = transformWholeRowRef(pstate, nsitem, levels_up,
												cref->location);
					break;
				}

				colname = strVal(field2);

				/* 尝试识别为该 nsitem 的列          */
				node = scanNSItemForColumn(pstate, nsitem, levels_up, colname,
										   cref->location);
				if (node == NULL)
				{
					/* 尝试将其作为对整行的函数调用 */
					node = transformWholeRowRef(pstate, nsitem, levels_up,
												cref->location);
					node = ParseFuncOrColumn(pstate,
											 list_make1(makeString(colname)),
											 list_make1(node),
											 pstate->p_last_srf,
											 NULL,
											 false,
											 cref->location);
				}
				break;
			}
		case 3:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);
				Node	   *field2 = (Node *) lsecond(cref->fields);
				Node	   *field3 = (Node *) lthird(cref->fields);

				nspname = strVal(field1);
				relname = strVal(field2);

				/* 定位被引用的 nsitem    */
				nsitem = refnameNamespaceItem(pstate, nspname, relname,
											  cref->location,
											  &levels_up);
				if (nsitem == NULL)
				{
					crerr = CRERR_NO_RTE;
					break;
				}

				/* 整行引用？      */
				if (IsA(field3, A_Star))
				{
					node = transformWholeRowRef(pstate, nsitem, levels_up,
												cref->location);
					break;
				}

				colname = strVal(field3);

				/* 尝试识别为该 nsitem 的列          */
				node = scanNSItemForColumn(pstate, nsitem, levels_up, colname,
										   cref->location);
				if (node == NULL)
				{
					/* 尝试将其作为对整行的函数调用 */
					node = transformWholeRowRef(pstate, nsitem, levels_up,
												cref->location);
					node = ParseFuncOrColumn(pstate,
											 list_make1(makeString(colname)),
											 list_make1(node),
											 pstate->p_last_srf,
											 NULL,
											 false,
											 cref->location);
				}
				break;
			}
		case 4:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);
				Node	   *field2 = (Node *) lsecond(cref->fields);
				Node	   *field3 = (Node *) lthird(cref->fields);
				Node	   *field4 = (Node *) lfourth(cref->fields);
				char	   *catname;

				catname = strVal(field1);
				nspname = strVal(field2);
				relname = strVal(field3);

				/*
				 * 我们检查目录名，然后将其忽略。
				 */
				if (strcmp(catname, get_database_name(MyDatabaseId)) != 0)
				{
					crerr = CRERR_WRONG_DB;
					break;
				}

				/* 定位被引用的 nsitem    */
				nsitem = refnameNamespaceItem(pstate, nspname, relname,
											  cref->location,
											  &levels_up);
				if (nsitem == NULL)
				{
					crerr = CRERR_NO_RTE;
					break;
				}

				/* 整行引用？      */
				if (IsA(field4, A_Star))
				{
					node = transformWholeRowRef(pstate, nsitem, levels_up,
												cref->location);
					break;
				}

				colname = strVal(field4);

				/* 尝试识别为该 nsitem 的列          */
				node = scanNSItemForColumn(pstate, nsitem, levels_up, colname,
										   cref->location);
				if (node == NULL)
				{
					/* 尝试将其作为对整行的函数调用 */
					node = transformWholeRowRef(pstate, nsitem, levels_up,
												cref->location);
					node = ParseFuncOrColumn(pstate,
											 list_make1(makeString(colname)),
											 list_make1(node),
											 pstate->p_last_srf,
											 NULL,
											 false,
											 cref->location);
				}
				break;
			}
		default:
			crerr = CRERR_TOO_MANY; /* 带点名称过多    */
			break;
	}

	/*
	 * 若存在 PostParseColumnRefHook，现在轮到它。我们把     
	 * 目前为止的转换结果传给它，以便它在认为对        
	 * ColumnRef 有冲突解读时抛出错误。（若它                  
	 * 无论如何都强行翻译，我们会抛错，因为无法撤销 
	 * 前面步骤可能已对 pstate 产生的影响。）若它返回      
	 * NULL，就用标准转换结果，若没有则抛出                 
	 * 合适错误。  
	 */
	if (pstate->p_post_columnref_hook != NULL)
	{
		Node	   *hookresult;

		hookresult = pstate->p_post_columnref_hook(pstate, cref, node);
		if (node == NULL)
			node = hookresult;
		else if (hookresult != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_COLUMN),
					 errmsg("column reference \"%s\" is ambiguous",
							NameListToString(cref->fields)),
					 parser_errposition(pstate, cref->location)));
	}

	/*
	 * 若无转换结果则抛错。      
	 */
	if (node == NULL)
	{
		switch (crerr)
		{
			case CRERR_NO_COLUMN:
				errorMissingColumn(pstate, relname, colname, cref->location);
				break;
			case CRERR_NO_RTE:
				errorMissingRTE(pstate, makeRangeVar(nspname, relname,
													 cref->location));
				break;
			case CRERR_WRONG_DB:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cross-database references are not implemented: %s",
								NameListToString(cref->fields)),
						 parser_errposition(pstate, cref->location)));
				break;
			case CRERR_TOO_MANY:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("improper qualified name (too many dotted names): %s",
								NameListToString(cref->fields)),
						 parser_errposition(pstate, cref->location)));
				break;
		}
	}

	return node;
}

static Node *
transformParamRef(ParseState *pstate, ParamRef *pref)
{
	Node	   *result;

	/*
	 * 核心解析器对 Param 一无所知。若提供了钩子，      
	 * 就调用它。若无钩子或返回 NULL，则抛通用错误。    
	 */
	if (pstate->p_paramref_hook != NULL)
		result = pstate->p_paramref_hook(pstate, pref);
	else
		result = NULL;

	if (result == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PARAMETER),
				 errmsg("there is no parameter $%d", pref->number),
				 parser_errposition(pstate, pref->location)));

	return result;
}

/* 检测 a_expr 是否为普通 NULL 常量              */
static bool
exprIsNullConstant(Node *arg)
{
	if (arg && IsA(arg, A_Const))
	{
		A_Const    *con = (A_Const *) arg;

		if (con->isnull)
			return true;
	}
	return false;
}

static Node *
transformAExprOp(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = a->lexpr;
	Node	   *rexpr = a->rexpr;
	Node	   *result;

	/*
	 * 为兼容破坏标准的产品（如微软的），特判        
	 * "foo = NULL" 与 "NULL = foo"，把它们变成 IS NULL               
	 * 表达式。（若任一侧是 CaseTestExpr，则该表达式是 
	 * 由 CASE-WHEN 表达式在内部生成的，此时   
	 * transform_null_equals 不适用。） 
	 */
	if (Transform_null_equals &&
		list_length(a->name) == 1 &&
		strcmp(strVal(linitial(a->name)), "=") == 0 &&
		(exprIsNullConstant(lexpr) || exprIsNullConstant(rexpr)) &&
		(!IsA(lexpr, CaseTestExpr) && !IsA(rexpr, CaseTestExpr)))
	{
		NullTest   *n = makeNode(NullTest);

		n->nulltesttype = IS_NULL;
		n->location = a->location;

		if (exprIsNullConstant(lexpr))
			n->arg = (Expr *) rexpr;
		else
			n->arg = (Expr *) lexpr;

		result = transformExprRecurse(pstate, (Node *) n);
	}
	else if (lexpr && IsA(lexpr, RowExpr) &&
			 rexpr && IsA(rexpr, SubLink) &&
			 ((SubLink *) rexpr)->subLinkType == EXPR_SUBLINK)
	{
		/*
		 * 把"行 op 子查询"转换为 ROWCOMPARE 子链接。以前      
		 * 由语法完成此事，但现在行构造可出现在表达式   
		 * 的任何位置，在这里做更容易。
		 */
		SubLink    *s = (SubLink *) rexpr;

		s->subLinkType = ROWCOMPARE_SUBLINK;
		s->testexpr = lexpr;
		s->operName = a->name;
		s->location = a->location;
		result = transformExprRecurse(pstate, (Node *) s);
	}
	else if (lexpr && IsA(lexpr, RowExpr) &&
			 rexpr && IsA(rexpr, RowExpr))
	{
		/* ROW() op ROW() 被特殊处理      */
		lexpr = transformExprRecurse(pstate, lexpr);
		rexpr = transformExprRecurse(pstate, rexpr);

		result = make_row_comparison_op(pstate,
										a->name,
										castNode(RowExpr, lexpr)->args,
										castNode(RowExpr, rexpr)->args,
										a->location);
	}
	else
	{
		/* 普通标量操作符    */
		Node	   *last_srf = pstate->p_last_srf;

		lexpr = transformExprRecurse(pstate, lexpr);
		rexpr = transformExprRecurse(pstate, rexpr);

		result = (Node *) make_op(pstate,
								  a->name,
								  lexpr,
								  rexpr,
								  last_srf,
								  a->location);
	}

	return result;
}

static Node *
transformAExprOpAny(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = transformExprRecurse(pstate, a->lexpr);
	Node	   *rexpr = transformExprRecurse(pstate, a->rexpr);

	return (Node *) make_scalar_array_op(pstate,
										 a->name,
										 true,
										 lexpr,
										 rexpr,
										 a->location);
}

static Node *
transformAExprOpAll(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = transformExprRecurse(pstate, a->lexpr);
	Node	   *rexpr = transformExprRecurse(pstate, a->rexpr);

	return (Node *) make_scalar_array_op(pstate,
										 a->name,
										 false,
										 lexpr,
										 rexpr,
										 a->location);
}

static Node *
transformAExprDistinct(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = a->lexpr;
	Node	   *rexpr = a->rexpr;
	Node	   *result;

	/*
	 * 若任一输入是未修饰的 NULL 字面量，则转换为对另     
	 * 一输入的 NullTest。这比完整的 DistinctExpr 更易处理，   
	 * 且无需要求该数据类型必须有 = 操作符。                
	 */
	if (exprIsNullConstant(rexpr))
		return make_nulltest_from_distinct(pstate, a, lexpr);
	if (exprIsNullConstant(lexpr))
		return make_nulltest_from_distinct(pstate, a, rexpr);

	lexpr = transformExprRecurse(pstate, lexpr);
	rexpr = transformExprRecurse(pstate, rexpr);

	if (lexpr && IsA(lexpr, RowExpr) &&
		rexpr && IsA(rexpr, RowExpr))
	{
		/* ROW() op ROW() 被特殊处理      */
		result = make_row_distinct_op(pstate, a->name,
									  (RowExpr *) lexpr,
									  (RowExpr *) rexpr,
									  a->location);
	}
	else
	{
		/* 普通标量操作符    */
		result = (Node *) make_distinct_op(pstate,
										   a->name,
										   lexpr,
										   rexpr,
										   a->location);
	}

	/*
	 * 若是 NOT DISTINCT，则先构建 DistinctExpr，然后            
	 * 再套 NOT 
	 */
	if (a->kind == AEXPR_NOT_DISTINCT)
		result = (Node *) makeBoolExpr(NOT_EXPR,
									   list_make1(result),
									   a->location);

	return result;
}

static Node *
transformAExprNullIf(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = transformExprRecurse(pstate, a->lexpr);
	Node	   *rexpr = transformExprRecurse(pstate, a->rexpr);
	OpExpr	   *result;

	result = (OpExpr *) make_op(pstate,
								a->name,
								lexpr,
								rexpr,
								pstate->p_last_srf,
								a->location);

	/*
	 * 比较操作符本身应产生布尔结果 ...         
	 */
	if (result->opresulttype != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("NULLIF requires = operator to yield boolean"),
				 parser_errposition(pstate, a->location)));
	if (result->opretset)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
		/* translator: %s is name of a SQL construct, eg NULLIF */
				 errmsg("%s must not return a set", "NULLIF"),
				 parser_errposition(pstate, a->location)));

	/*
	 * ... 但 NullIfExpr 将产生第一个操作数的类型。 
	 */
	result->opresulttype = exprType((Node *) linitial(result->args));

	/*
	 * 依赖 NullIfExpr 与 OpExpr 共用同一结构体    
	 */
	NodeSetTag(result, T_NullIfExpr);

	return (Node *) result;
}

static Node *
transformAExprIn(ParseState *pstate, A_Expr *a)
{
	Node	   *result = NULL;
	Node	   *lexpr;
	List	   *rexprs;
	List	   *rvars;
	List	   *rnonvars;
	bool		useOr;
	ListCell   *l;

	/*
	 * 若操作符为 <>，用 AND 而非 OR。      
	 */
	if (strcmp(strVal(linitial(a->name)), "<>") == 0)
		useOr = false;
	else
		useOr = true;

	/*
	 * 我们尝试从 IN/NOT IN 生成 ScalarArrayOpExpr，但这只有      
	 * 在存在合适的数组类型时才可行。若没有，就回退到 
	 * 用多个左表达式副本构成的布尔条件树。此外，      
	 * 任何含 Var 的 IN 列表项都作为独立的布尔条件处理，
	 * 因为这样给规划器对此类子句提供了更大                
	 * 的优化空间。               
	 *
	 * 第一步：转换所有输入，并检测其中是否含 Var。    
	 *      
	 */
	lexpr = transformExprRecurse(pstate, a->lexpr);
	rexprs = rvars = rnonvars = NIL;
	foreach(l, (List *) a->rexpr)
	{
		Node	   *rexpr = transformExprRecurse(pstate, lfirst(l));

		rexprs = lappend(rexprs, rexpr);
		if (contain_vars_of_level(rexpr, 0))
			rvars = lappend(rvars, rexpr);
		else
			rnonvars = lappend(rnonvars, rexpr);
	}

	/*
	 * ScalarArrayOpExpr 只有在非 Var 的右端项多于                
	 * 一个时才有用。  
	 */
	if (list_length(rnonvars) > 1)
	{
		List	   *allexprs;
		Oid			scalar_type;
		Oid			array_type;

		/*
		 * 尝试为数组元素选择一个公共类型。注意        
		 * 由于左侧类型在列表中排在首位，在有疑问时       
		 * （例如右端项全为未知字面量时）优先选它。     
		 *
		 * 注意：用 list_concat 而非 lcons，以免破坏 rnonvars。 
		 */
		allexprs = list_concat(list_make1(lexpr), rnonvars);
		scalar_type = select_common_type(pstate, allexprs, NULL, NULL);

		/* 必须验证所选类型确实可用                    */
		if (OidIsValid(scalar_type) &&
			!verify_common_type(scalar_type, allexprs))
			scalar_type = InvalidOid;

		/*
		 * 有可用数组类型吗？除了不存在该类型的情形     
		 * 外，当公共类型是 RECORD 时我们也不会冒险使用  
		 * ScalarArrayOpExpr，因为下方的 RowExpr 比较逻辑           
		 * 可应对部分行类型不同的情况。 
		 */
		if (OidIsValid(scalar_type) && scalar_type != RECORDOID)
			array_type = get_array_type(scalar_type);
		else
			array_type = InvalidOid;
		if (array_type != InvalidOid)
		{
			/*
			 * 好：把右端全部非 Var 输入强制转换为公共类型 
			 * 并为它们构建 ArrayExpr。 
			 */
			List	   *aexprs;
			ArrayExpr  *newa;

			aexprs = NIL;
			foreach(l, rnonvars)
			{
				Node	   *rexpr = (Node *) lfirst(l);

				rexpr = coerce_to_common_type(pstate, rexpr,
											  scalar_type,
											  "IN");
				aexprs = lappend(aexprs, rexpr);
			}
			newa = makeNode(ArrayExpr);
			newa->array_typeid = array_type;
			/* array_collid 由 parse_collate.c 设置     */
			newa->element_typeid = scalar_type;
			newa->elements = aexprs;
			newa->multidims = false;
			newa->location = -1;

			result = (Node *) make_scalar_array_op(pstate,
												   a->name,
												   useOr,
												   lexpr,
												   (Node *) newa,
												   a->location);

			/* 在下面的循环中只考虑 Var（如有）    */
			rexprs = rvars;
		}
	}

	/*
	 * 必须用笨办法，即布尔表达式树来做。         
	 */
	foreach(l, rexprs)
	{
		Node	   *rexpr = (Node *) lfirst(l);
		Node	   *cmp;

		if (IsA(lexpr, RowExpr) &&
			IsA(rexpr, RowExpr))
		{
			/* ROW() op ROW() 被特殊处理      */
			cmp = make_row_comparison_op(pstate,
										 a->name,
										 copyObject(((RowExpr *) lexpr)->args),
										 ((RowExpr *) rexpr)->args,
										 a->location);
		}
		else
		{
			/* 普通标量操作符    */
			cmp = (Node *) make_op(pstate,
								   a->name,
								   copyObject(lexpr),
								   rexpr,
								   pstate->p_last_srf,
								   a->location);
		}

		cmp = coerce_to_boolean(pstate, cmp, "IN");
		if (result == NULL)
			result = cmp;
		else
			result = (Node *) makeBoolExpr(useOr ? OR_EXPR : AND_EXPR,
										   list_make2(result, cmp),
										   a->location);
	}

	return result;
}

static Node *
transformAExprBetween(ParseState *pstate, A_Expr *a)
{
	Node	   *aexpr;
	Node	   *bexpr;
	Node	   *cexpr;
	Node	   *result;
	Node	   *sub1;
	Node	   *sub2;
	List	   *args;

	/* 把 A_Expr 分解为三个子表达式 */
	aexpr = a->lexpr;
	args = castNode(List, a->rexpr);
	Assert(list_length(args) == 2);
	bexpr = (Node *) linitial(args);
	cexpr = (Node *) lsecond(args);

	/*
	 * 构建等价的比较表达式。为安全起见，对被  
	 * 多次引用的子表达式制作副本。（XXX 这实在         
	 * 有问题，因为这会对可能是易变的表达式               
	 * 求值多次 ...）      
	 *
	 * 理想情况下此处不应使用硬编码的操作符，而应   
	 * 使用操作符类。但混合数据类型等问题使这一点 
	 * 很难： 
	 * http://archives.postgresql.org/pgsql-hackers/2008-08/msg01142.php
	 */
	switch (a->kind)
	{
		case AEXPR_BETWEEN:
			args = list_make2(makeSimpleA_Expr(AEXPR_OP, ">=",
											   aexpr, bexpr,
											   a->location),
							  makeSimpleA_Expr(AEXPR_OP, "<=",
											   copyObject(aexpr), cexpr,
											   a->location));
			result = (Node *) makeBoolExpr(AND_EXPR, args, a->location);
			break;
		case AEXPR_NOT_BETWEEN:
			args = list_make2(makeSimpleA_Expr(AEXPR_OP, "<",
											   aexpr, bexpr,
											   a->location),
							  makeSimpleA_Expr(AEXPR_OP, ">",
											   copyObject(aexpr), cexpr,
											   a->location));
			result = (Node *) makeBoolExpr(OR_EXPR, args, a->location);
			break;
		case AEXPR_BETWEEN_SYM:
			args = list_make2(makeSimpleA_Expr(AEXPR_OP, ">=",
											   aexpr, bexpr,
											   a->location),
							  makeSimpleA_Expr(AEXPR_OP, "<=",
											   copyObject(aexpr), cexpr,
											   a->location));
			sub1 = (Node *) makeBoolExpr(AND_EXPR, args, a->location);
			args = list_make2(makeSimpleA_Expr(AEXPR_OP, ">=",
											   copyObject(aexpr), copyObject(cexpr),
											   a->location),
							  makeSimpleA_Expr(AEXPR_OP, "<=",
											   copyObject(aexpr), copyObject(bexpr),
											   a->location));
			sub2 = (Node *) makeBoolExpr(AND_EXPR, args, a->location);
			args = list_make2(sub1, sub2);
			result = (Node *) makeBoolExpr(OR_EXPR, args, a->location);
			break;
		case AEXPR_NOT_BETWEEN_SYM:
			args = list_make2(makeSimpleA_Expr(AEXPR_OP, "<",
											   aexpr, bexpr,
											   a->location),
							  makeSimpleA_Expr(AEXPR_OP, ">",
											   copyObject(aexpr), cexpr,
											   a->location));
			sub1 = (Node *) makeBoolExpr(OR_EXPR, args, a->location);
			args = list_make2(makeSimpleA_Expr(AEXPR_OP, "<",
											   copyObject(aexpr), copyObject(cexpr),
											   a->location),
							  makeSimpleA_Expr(AEXPR_OP, ">",
											   copyObject(aexpr), copyObject(bexpr),
											   a->location));
			sub2 = (Node *) makeBoolExpr(OR_EXPR, args, a->location);
			args = list_make2(sub1, sub2);
			result = (Node *) makeBoolExpr(AND_EXPR, args, a->location);
			break;
		default:
			elog(ERROR, "unrecognized A_Expr kind: %d", a->kind);
			result = NULL;		/* 让编译器安静  */
			break;
	}

	return transformExprRecurse(pstate, result);
}

static Node *
transformBoolExpr(ParseState *pstate, BoolExpr *a)
{
	List	   *args = NIL;
	const char *opname;
	ListCell   *lc;

	switch (a->boolop)
	{
		case AND_EXPR:
			opname = "AND";
			break;
		case OR_EXPR:
			opname = "OR";
			break;
		case NOT_EXPR:
			opname = "NOT";
			break;
		default:
			elog(ERROR, "unrecognized boolop: %d", (int) a->boolop);
			opname = NULL;		/* 让编译器安静  */
			break;
	}

	foreach(lc, a->args)
	{
		Node	   *arg = (Node *) lfirst(lc);

		arg = transformExprRecurse(pstate, arg);
		arg = coerce_to_boolean(pstate, arg, opname);
		args = lappend(args, arg);
	}

	return (Node *) makeBoolExpr(a->boolop, args, a->location);
}

static Node *
transformFuncCall(ParseState *pstate, FuncCall *fn)
{
	Node	   *last_srf = pstate->p_last_srf;
	List	   *targs;
	ListCell   *args;

	/* 转换实参列表 ...              */
	targs = NIL;
	foreach(args, fn->args)
	{
		targs = lappend(targs, transformExprRecurse(pstate,
													(Node *) lfirst(args)));
	}

	/*
	 * 使用 WITHIN GROUP 时，我们把其 ORDER BY 表达式      
	 * 当作函数的附加实参，用于函数查找与实参类型      
	 * 强制转换。因此，转换每个这类表达式并把它们        
	 * 加入 targs 列表。我们不会显式标记每个实参的        
	 * 来源，但 ParseFuncOrColumn 能借助下列表达式分辨：     
	 * list_length(fn->agg_order) 
	 */
	if (fn->agg_within_group)
	{
		Assert(fn->agg_order != NIL);
		foreach(args, fn->agg_order)
		{
			SortBy	   *arg = (SortBy *) lfirst(args);

			targs = lappend(targs, transformExpr(pstate, arg->node,
												 EXPR_KIND_ORDER_BY));
		}
	}

	/* ... 交给 ParseFuncOrColumn          */
	return ParseFuncOrColumn(pstate,
							 fn->funcname,
							 targs,
							 last_srf,
							 fn,
							 false,
							 fn->location);
}

static Node *
transformMultiAssignRef(ParseState *pstate, MultiAssignRef *maref)
{
	SubLink    *sublink;
	RowExpr    *rexpr;
	Query	   *qtree;
	TargetEntry *tle;

	/* 只应出现在 UPDATE 目标列表的第一阶段处理中        */
	Assert(pstate->p_expr_kind == EXPR_KIND_UPDATE_SOURCE);

	/* 仅当这是第一列时才需要转换源表达式              */
	if (maref->colno == 1)
	{
		/*
		 * 目前我们只允许 EXPR 子链接与 RowExpr 作为            
		 * UPDATE 多重赋值的来源。这足以覆盖各种有意义      
		 * 的情形；最坏情况下，得写 (SELECT * FROM expr)        
		 * 来展开其他形式的复合返回表达式。           
		 */
		if (IsA(maref->source, SubLink) &&
			((SubLink *) maref->source)->subLinkType == EXPR_SUBLINK)
		{
			/* 将其重标为 MULTIEXPR_SUBLINK */
			sublink = (SubLink *) maref->source;
			sublink->subLinkType = MULTIEXPR_SUBLINK;
			/* 然后转换它  */
			sublink = (SubLink *) transformExprRecurse(pstate,
													   (Node *) sublink);

			qtree = castNode(Query, sublink->subselect);

			/* 检查子查询是否返回了所需列数        */
			if (count_nonjunk_tlist_entries(qtree->targetList) != maref->ncolumns)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("number of columns does not match number of values"),
						 parser_errposition(pstate, sublink->location)));

			/*
			 * 构建含 MULTIEXPR SubLink 的 resjunk 目标列表项，  
			 * 并将其加入 pstate->p_multiassign_exprs，它稍后        
			 * 会追加到完成的目标列表。无需为它操心     
			 * resno 的选择；transformUpdateStmt 会完成            
			 *      
			 */
			tle = makeTargetEntry((Expr *) sublink, 0, NULL, true);
			pstate->p_multiassign_exprs = lappend(pstate->p_multiassign_exprs,
												  tle);

			/*
			 * 给 MULTIEXPR SubLink 分配一个此目标列表内      
			 * 唯一的 ID。用其在 p_multiassign_exprs 
			 * 中的位置即可。    
			 */
			sublink->subLinkId = list_length(pstate->p_multiassign_exprs);
		}
		else if (IsA(maref->source, RowExpr))
		{
			/* 转换 RowExpr，允许 SetToDefault 项           */
			rexpr = (RowExpr *) transformRowExpr(pstate,
												 (RowExpr *) maref->source,
												 true);

			/* 检查其是否返回所需列数           */
			if (list_length(rexpr->args) != maref->ncolumns)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("number of columns does not match number of values"),
						 parser_errposition(pstate, rexpr->location)));

			/*
			 * 临时把它追加到 p_multiassign_exprs，之后处理      
			 * 后续列时好取回它。                        
			 */
			tle = makeTargetEntry((Expr *) rexpr, 0, NULL, true);
			pstate->p_multiassign_exprs = lappend(pstate->p_multiassign_exprs,
												  tle);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("source for a multiple-column UPDATE item must be a sub-SELECT or ROW() expression"),
					 parser_errposition(pstate, exprLocation(maref->source))));
	}
	else
	{
		/*
		 * 多重赋值的第二列或更后面的列。重新取回 
		 * 已转换的 SubLink 或 RowExpr，假定它仍是               
		 * p_multiassign_exprs 末项   
		 */
		Assert(pstate->p_multiassign_exprs != NIL);
		tle = (TargetEntry *) llast(pstate->p_multiassign_exprs);
	}

	/*
	 * 为当前列发出适当的输出表达式                   
	 */
	if (IsA(tle->expr, SubLink))
	{
		Param	   *param;

		sublink = (SubLink *) tle->expr;
		Assert(sublink->subLinkType == MULTIEXPR_SUBLINK);
		qtree = castNode(Query, sublink->subselect);

		/* 构建代表当前子查询输出列的 Param                 */
		tle = (TargetEntry *) list_nth(qtree->targetList, maref->colno - 1);
		Assert(!tle->resjunk);

		param = makeNode(Param);
		param->paramkind = PARAM_MULTIEXPR;
		param->paramid = (sublink->subLinkId << 16) | maref->colno;
		param->paramtype = exprType((Node *) tle->expr);
		param->paramtypmod = exprTypmod((Node *) tle->expr);
		param->paramcollid = exprCollation((Node *) tle->expr);
		param->location = exprLocation((Node *) tle->expr);

		return (Node *) param;
	}

	if (IsA(tle->expr, RowExpr))
	{
		Node	   *result;

		rexpr = (RowExpr *) tle->expr;

		/* 只需提取并返回 RowExpr 的下一个元素        */
		result = (Node *) list_nth(rexpr->args, maref->colno - 1);

		/*
		 * 如果处于最后一列，就从                   
		 * p_multiassign_exprs 中删除该 RowExpr；不再需要它，      
		 * 也不希望它出现在完成的 UPDATE 目标列表中。假定 
		 * 它仍是末项        
		 */
		if (maref->colno == maref->ncolumns)
			pstate->p_multiassign_exprs =
				list_delete_last(pstate->p_multiassign_exprs);

		return result;
	}

	elog(ERROR, "unexpected expr type in multiassign list");
	return NULL;				/* 让编译器安静  */
}

static Node *
transformCaseExpr(ParseState *pstate, CaseExpr *c)
{
	CaseExpr   *newc = makeNode(CaseExpr);
	Node	   *last_srf = pstate->p_last_srf;
	Node	   *arg;
	CaseTestExpr *placeholder;
	List	   *newargs;
	List	   *resultexprs;
	ListCell   *l;
	Node	   *defresult;
	Oid			ptype;

	/* 转换测试表达式（如有）     */
	arg = transformExprRecurse(pstate, (Node *) c->arg);

	/* 为测试表达式生成占位符        */
	if (arg)
	{
		/*
		 * 若测试表达式是未定类型字面量，则强制为 text。  
		 * 必须现在就处理，因为到占位符上就无法再做       
		 * 这种强制转换了。这不如 7.4 及之前版本的做法   
		 * 灵活，但足以处理常见的那种糟糕写法。             
		 *               
		 */
		if (exprType(arg) == UNKNOWNOID)
			arg = coerce_to_common_type(pstate, arg, TEXTOID, "CASE");

		/*
		 * 对测试表达式执行排序规则分配，以便知道      
		 * 该用哪种排序规则标记占位符。原则上可以          
		 * 把这件事留给 parse_collate.c 以后处理，但把结果   
		 * 传播给 CaseTestExpr 会不必要地复杂。               
		 */
		assign_expr_collations(pstate, arg);

		placeholder = makeNode(CaseTestExpr);
		placeholder->typeId = exprType(arg);
		placeholder->typeMod = exprTypmod(arg);
		placeholder->collation = exprCollation(arg);
	}
	else
		placeholder = NULL;

	newc->arg = (Expr *) arg;

	/* 转换参数列表              */
	newargs = NIL;
	resultexprs = NIL;
	foreach(l, c->args)
	{
		CaseWhen   *w = lfirst_node(CaseWhen, l);
		CaseWhen   *neww = makeNode(CaseWhen);
		Node	   *warg;

		warg = (Node *) w->expr;
		if (placeholder)
		{
			/* 已指定简写形式，故进行展开... */
			warg = (Node *) makeSimpleA_Expr(AEXPR_OP, "=",
											 (Node *) placeholder,
											 warg,
											 w->location);
		}
		neww->expr = (Expr *) transformExprRecurse(pstate, warg);

		neww->expr = (Expr *) coerce_to_boolean(pstate,
												(Node *) neww->expr,
												"CASE/WHEN");

		warg = (Node *) w->result;
		neww->result = (Expr *) transformExprRecurse(pstate, warg);
		neww->location = w->location;

		newargs = lappend(newargs, neww);
		resultexprs = lappend(resultexprs, neww->result);
	}

	newc->args = newargs;

	/* 转换 DEFAULT 子句        */
	defresult = (Node *) c->defresult;
	if (defresult == NULL)
	{
		A_Const    *n = makeNode(A_Const);

		n->isnull = true;
		n->location = -1;
		defresult = (Node *) n;
	}
	newc->defresult = (Expr *) transformExprRecurse(pstate, defresult);

	/*
	 * 注意：默认结果被视为决定首选类型时            
	 * 权重最高的类型。此前的代码就是这样处理的，       
	 * 但我觉得有点不妥 --- tgl  
	 */
	resultexprs = lcons(newc->defresult, resultexprs);

	ptype = select_common_type(pstate, resultexprs, "CASE", NULL);
	Assert(OidIsValid(ptype));
	newc->casetype = ptype;
	/* casecollid 由 parse_collate.c 设置     */

	/* 如有必要，转换默认结果子句     */
	newc->defresult = (Expr *)
		coerce_to_common_type(pstate,
							  (Node *) newc->defresult,
							  ptype,
							  "CASE/ELSE");

	/* 如有必要，转换 WHEN 子句结果   */
	foreach(l, newc->args)
	{
		CaseWhen   *w = (CaseWhen *) lfirst(l);

		w->result = (Expr *)
			coerce_to_common_type(pstate,
								  (Node *) w->result,
								  ptype,
								  "CASE/WHEN");
	}

	/* 若任一子表达式包含 SRF，则报错    */
	if (pstate->p_last_srf != last_srf)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/* translator: %s is name of a SQL construct, eg GROUP BY */
				 errmsg("set-returning functions are not allowed in %s",
						"CASE"),
				 errhint("You might be able to move the set-returning function into a LATERAL FROM item."),
				 parser_errposition(pstate,
									exprLocation(pstate->p_last_srf))));

	newc->location = c->location;

	return (Node *) newc;
}

static Node *
transformSubLink(ParseState *pstate, SubLink *sublink)
{
	Node	   *result = (Node *) sublink;
	Query	   *qtree;
	const char *err;

	/*
	 * 检查 sublink 是否位于查询中的无效位置。                 
	 * 在 SELECT/INSERT/UPDATE/DELETE/MERGE 中随处允许 sublink，   
	 * 工具命令一般不允许。      
	 */
	err = NULL;
	switch (pstate->p_expr_kind)
	{
		case EXPR_KIND_NONE:
			Assert(false);		/* 不可能    */
			break;
		case EXPR_KIND_OTHER:
			/* 在此接受 sublink；如需报错由调用者负责  */
			break;
		case EXPR_KIND_JOIN_ON:
		case EXPR_KIND_JOIN_USING:
		case EXPR_KIND_FROM_SUBSELECT:
		case EXPR_KIND_FROM_FUNCTION:
		case EXPR_KIND_WHERE:
		case EXPR_KIND_POLICY:
		case EXPR_KIND_HAVING:
		case EXPR_KIND_FILTER:
		case EXPR_KIND_WINDOW_PARTITION:
		case EXPR_KIND_WINDOW_ORDER:
		case EXPR_KIND_WINDOW_FRAME_RANGE:
		case EXPR_KIND_WINDOW_FRAME_ROWS:
		case EXPR_KIND_WINDOW_FRAME_GROUPS:
		case EXPR_KIND_SELECT_TARGET:
		case EXPR_KIND_INSERT_TARGET:
		case EXPR_KIND_UPDATE_SOURCE:
		case EXPR_KIND_UPDATE_TARGET:
		case EXPR_KIND_MERGE_WHEN:
		case EXPR_KIND_GROUP_BY:
		case EXPR_KIND_ORDER_BY:
		case EXPR_KIND_DISTINCT_ON:
		case EXPR_KIND_LIMIT:
		case EXPR_KIND_OFFSET:
		case EXPR_KIND_RETURNING:
		case EXPR_KIND_VALUES:
		case EXPR_KIND_VALUES_SINGLE:
		case EXPR_KIND_CYCLE_MARK:
			/* 可  */
			break;
		case EXPR_KIND_CHECK_CONSTRAINT:
		case EXPR_KIND_DOMAIN_CHECK:
			err = _("cannot use subquery in check constraint");
			break;
		case EXPR_KIND_COLUMN_DEFAULT:
		case EXPR_KIND_FUNCTION_DEFAULT:
			err = _("cannot use subquery in DEFAULT expression");
			break;
		case EXPR_KIND_INDEX_EXPRESSION:
			err = _("cannot use subquery in index expression");
			break;
		case EXPR_KIND_INDEX_PREDICATE:
			err = _("cannot use subquery in index predicate");
			break;
		case EXPR_KIND_STATS_EXPRESSION:
			err = _("cannot use subquery in statistics expression");
			break;
		case EXPR_KIND_ALTER_COL_TRANSFORM:
			err = _("cannot use subquery in transform expression");
			break;
		case EXPR_KIND_EXECUTE_PARAMETER:
			err = _("cannot use subquery in EXECUTE parameter");
			break;
		case EXPR_KIND_TRIGGER_WHEN:
			err = _("cannot use subquery in trigger WHEN condition");
			break;
		case EXPR_KIND_PARTITION_BOUND:
			err = _("cannot use subquery in partition bound");
			break;
		case EXPR_KIND_PARTITION_EXPRESSION:
			err = _("cannot use subquery in partition key expression");
			break;
		case EXPR_KIND_CALL_ARGUMENT:
			err = _("cannot use subquery in CALL argument");
			break;
		case EXPR_KIND_COPY_WHERE:
			err = _("cannot use subquery in COPY FROM WHERE condition");
			break;
		case EXPR_KIND_GENERATED_COLUMN:
			err = _("cannot use subquery in column generation expression");
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
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg_internal("%s", err),
				 parser_errposition(pstate, sublink->location)));

	pstate->p_hasSubLinks = true;

	/*
	 * 好，下面转换子 SELECT。    
	 */
	qtree = parse_sub_analyze(sublink->subselect, pstate, NULL, false, true);

	/*
	 * 检查我们得到的是否为 SELECT。给定语法的限制，     
	 * 其余本不该出现，但仍检查。       
	 */
	if (!IsA(qtree, Query) ||
		qtree->commandType != CMD_SELECT)
		elog(ERROR, "unexpected non-SELECT command in SubLink");

	sublink->subselect = (Node *) qtree;

	if (sublink->subLinkType == EXISTS_SUBLINK)
	{
		/*
		 * EXISTS 不需要测试表达式或组合操作符。这些字段   
		 * 本应为空，但请确认。        
		 */
		sublink->testexpr = NULL;
		sublink->operName = NIL;
	}
	else if (sublink->subLinkType == EXPR_SUBLINK ||
			 sublink->subLinkType == ARRAY_SUBLINK)
	{
		/*
		 * 确保子选择只输出单列（忽略 resjunk 目标列）。   
		 *          
		 */
		if (count_nonjunk_tlist_entries(qtree->targetList) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("subquery must return only one column"),
					 parser_errposition(pstate, sublink->location)));

		/*
		 * EXPR 与 ARRAY 不需要测试表达式或组合操作符。       
		 * 这些字段本应为空，但请确认。   
		 */
		sublink->testexpr = NULL;
		sublink->operName = NIL;
	}
	else if (sublink->subLinkType == MULTIEXPR_SUBLINK)
	{
		/* 与 EXPR 情形相同，只是对列数没有限制           */
		sublink->testexpr = NULL;
		sublink->operName = NIL;
	}
	else
	{
		/* ALL、ANY 或 ROWCOMPARE：生成行比较表达式        */
		Node	   *lefthand;
		List	   *left_list;
		List	   *right_list;
		ListCell   *l;

		/*
		 * 若源为 "x IN (select)"，则转换为 "x = ANY (select)"。   
		 */
		if (sublink->operName == NIL)
			sublink->operName = list_make1(makeString("="));

		/*
		 * 转换左侧表达式，并转换为列表          
		 */
		lefthand = transformExprRecurse(pstate, sublink->testexpr);
		if (lefthand && IsA(lefthand, RowExpr))
			left_list = ((RowExpr *) lefthand)->args;
		else
			left_list = list_make1(lefthand);

		/*
		 * 构建 PARAM_SUBLINK 节点列表，表示子查询的输出列。 
		 *                 
		 */
		right_list = NIL;
		foreach(l, qtree->targetList)
		{
			TargetEntry *tent = (TargetEntry *) lfirst(l);
			Param	   *param;

			if (tent->resjunk)
				continue;

			param = makeNode(Param);
			param->paramkind = PARAM_SUBLINK;
			param->paramid = tent->resno;
			param->paramtype = exprType((Node *) tent->expr);
			param->paramtypmod = exprTypmod((Node *) tent->expr);
			param->paramcollid = exprCollation((Node *) tent->expr);
			param->location = -1;

			right_list = lappend(right_list, param);
		}

		/*
		 * 若列表长度不同，其实可以让 make_row_comparison_op 
		 * 去报错，但我们更希望生成更具体的错误消息。
		 *         
		 */
		if (list_length(left_list) < list_length(right_list))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("subquery has too many columns"),
					 parser_errposition(pstate, sublink->location)));
		if (list_length(left_list) > list_length(right_list))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("subquery has too few columns"),
					 parser_errposition(pstate, sublink->location)));

		/*
		 * 确定组合操作符，并生成合适的                
		 * 行比较表达式。     
		 */
		sublink->testexpr = make_row_comparison_op(pstate,
												   sublink->operName,
												   left_list,
												   right_list,
												   sublink->location);
	}

	return result;
}

/*
 * transformArrayExpr
 *
 * 若调用者指定了目标类型，则结果数组的类型     
 * 将与该类型完全一致。否则会用 select_common_type()  
 * 为元素推断公共类型。              
 */
static Node *
transformArrayExpr(ParseState *pstate, A_ArrayExpr *a,
				   Oid array_type, Oid element_type, int32 typmod)
{
	ArrayExpr  *newa = makeNode(ArrayExpr);
	List	   *newelems = NIL;
	List	   *newcoercedelems = NIL;
	ListCell   *element;
	Oid			coerce_type;
	bool		coerce_hard;

	/*
	 * 转换各元素表达式         
	 *
	 * 除非找到数组类型的元素表达式，否则将假定         
	 * 数组为一维。 
	 */
	newa->multidims = false;
	foreach(element, a->elements)
	{
		Node	   *e = (Node *) lfirst(element);
		Node	   *newe;

		/*
		 * 若某元素本身是 A_ArrayExpr，则直接递归，以下传    
		 * 给定的任何目标类型。              
		 */
		if (IsA(e, A_ArrayExpr))
		{
			newe = transformArrayExpr(pstate,
									  (A_ArrayExpr *) e,
									  array_type,
									  element_type,
									  typmod);
			/* 此处必然为数组           */
			Assert(array_type == InvalidOid || array_type == exprType(newe));
			newa->multidims = true;
		}
		else
		{
			newe = transformExprRecurse(pstate, e);

			/*
			 * 若尚未找到子数组表达式，则继续检查。      
			 * 注意，数组上的域不被接受为子数组；         
			 * int2vector 与 oidvector 亦不可；它们自有约束，     
			 * 难以良好地映射为子数组。     
			 */
			if (!newa->multidims)
			{
				Oid			newetype = exprType(newe);

				if (newetype != INT2VECTOROID && newetype != OIDVECTOROID &&
					type_is_array(newetype))
					newa->multidims = true;
			}
		}

		newelems = lappend(newelems, newe);
	}

	/*
	 * 为元素选定一个目标类型。  
	 *
	 * 若未给定目标数组类型，就必须设法根据               
	 * 现有各元素的类型推断出公共类型。                  
	 */
	if (OidIsValid(array_type))
	{
		/* 调用者须确保 array_type 匹配 element_type  */
		Assert(OidIsValid(element_type));
		coerce_type = (newa->multidims ? array_type : element_type);
		coerce_hard = true;
	}
	else
	{
		/* 没有目标类型时无法处理空数组        */
		if (newelems == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_DATATYPE),
					 errmsg("cannot determine type of empty array"),
					 errhint("Explicitly cast to the desired type, "
							 "for example ARRAY[]::integer[]."),
					 parser_errposition(pstate, a->location)));

		/* 为元素选定公共类型           */
		coerce_type = select_common_type(pstate, newelems, "ARRAY", NULL);

		if (newa->multidims)
		{
			array_type = coerce_type;
			element_type = get_element_type(array_type);
			if (!OidIsValid(element_type))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("could not find element type for data type %s",
								format_type_be(array_type)),
						 parser_errposition(pstate, a->location)));
		}
		else
		{
			element_type = coerce_type;
			array_type = get_array_type(element_type);
			if (!OidIsValid(array_type))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("could not find array type for data type %s",
								format_type_be(element_type)),
						 parser_errposition(pstate, a->location)));
		}
		coerce_hard = false;
	}

	/*
	 * 强制转换至目标类型   
	 *
	 * 若数组经过显式 cast，则元素同样被显式强制转换。
	 *                    
	 *
	 * 若数组类型只是从其元素的公共类型推导而来，   
	 * 则元素会被隐式强制转换为该公共类型。                
	 * 这与 select_common_type() 的其他用法保持一致。 
	 */
	foreach(element, newelems)
	{
		Node	   *e = (Node *) lfirst(element);
		Node	   *newe;

		if (coerce_hard)
		{
			newe = coerce_to_target_type(pstate, e,
										 exprType(e),
										 coerce_type,
										 typmod,
										 COERCION_EXPLICIT,
										 COERCE_EXPLICIT_CAST,
										 -1);
			if (newe == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_CANNOT_COERCE),
						 errmsg("cannot cast type %s to %s",
								format_type_be(exprType(e)),
								format_type_be(coerce_type)),
						 parser_errposition(pstate, exprLocation(e))));
		}
		else
			newe = coerce_to_common_type(pstate, e,
										 coerce_type,
										 "ARRAY");
		newcoercedelems = lappend(newcoercedelems, newe);
	}

	newa->array_typeid = array_type;
	/* array_collid 由 parse_collate.c 设置     */
	newa->element_typeid = element_type;
	newa->elements = newcoercedelems;
	newa->location = a->location;

	return (Node *) newa;
}

static Node *
transformRowExpr(ParseState *pstate, RowExpr *r, bool allowDefault)
{
	RowExpr    *newr;
	char		fname[16];
	int			fnum;

	newr = makeNode(RowExpr);

	/* 转换各字段表达式        */
	newr->args = transformExpressionList(pstate, r->args,
										 pstate->p_expr_kind, allowDefault);

	/* 不允许列数超出元组容量              */
	if (list_length(newr->args) > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("ROW expressions can have at most %d entries",
						MaxTupleAttributeNumber),
				 parser_errposition(pstate, r->location)));

	/* 若无后续 cast，类型视为 RECORD            */
	newr->row_typeid = RECORDOID;
	newr->row_format = COERCE_IMPLICIT_CAST;

	/* ROW() 的列是匿名的，因此虚构一些字段名  */
	newr->colnames = NIL;
	for (fnum = 1; fnum <= list_length(newr->args); fnum++)
	{
		snprintf(fname, sizeof(fname), "f%d", fnum);
		newr->colnames = lappend(newr->colnames, makeString(pstrdup(fname)));
	}

	newr->location = r->location;

	return (Node *) newr;
}

static Node *
transformCoalesceExpr(ParseState *pstate, CoalesceExpr *c)
{
	CoalesceExpr *newc = makeNode(CoalesceExpr);
	Node	   *last_srf = pstate->p_last_srf;
	List	   *newargs = NIL;
	List	   *newcoercedargs = NIL;
	ListCell   *args;

	foreach(args, c->args)
	{
		Node	   *e = (Node *) lfirst(args);
		Node	   *newe;

		newe = transformExprRecurse(pstate, e);
		newargs = lappend(newargs, newe);
	}

	newc->coalescetype = select_common_type(pstate, newargs, "COALESCE", NULL);
	/* coalescecollid 由 parse_collate.c 设置     */

	/* 如有必要则转换参数    */
	foreach(args, newargs)
	{
		Node	   *e = (Node *) lfirst(args);
		Node	   *newe;

		newe = coerce_to_common_type(pstate, e,
									 newc->coalescetype,
									 "COALESCE");
		newcoercedargs = lappend(newcoercedargs, newe);
	}

	/* 若任一子表达式包含 SRF，则报错    */
	if (pstate->p_last_srf != last_srf)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/* translator: %s is name of a SQL construct, eg GROUP BY */
				 errmsg("set-returning functions are not allowed in %s",
						"COALESCE"),
				 errhint("You might be able to move the set-returning function into a LATERAL FROM item."),
				 parser_errposition(pstate,
									exprLocation(pstate->p_last_srf))));

	newc->args = newcoercedargs;
	newc->location = c->location;
	return (Node *) newc;
}

static Node *
transformMinMaxExpr(ParseState *pstate, MinMaxExpr *m)
{
	MinMaxExpr *newm = makeNode(MinMaxExpr);
	List	   *newargs = NIL;
	List	   *newcoercedargs = NIL;
	const char *funcname = (m->op == IS_GREATEST) ? "GREATEST" : "LEAST";
	ListCell   *args;

	newm->op = m->op;
	foreach(args, m->args)
	{
		Node	   *e = (Node *) lfirst(args);
		Node	   *newe;

		newe = transformExprRecurse(pstate, e);
		newargs = lappend(newargs, newe);
	}

	newm->minmaxtype = select_common_type(pstate, newargs, funcname, NULL);
	/* minmaxcollid 与 inputcollid 由 parse_collate.c 设置     */

	/* 如有必要则转换参数    */
	foreach(args, newargs)
	{
		Node	   *e = (Node *) lfirst(args);
		Node	   *newe;

		newe = coerce_to_common_type(pstate, e,
									 newm->minmaxtype,
									 funcname);
		newcoercedargs = lappend(newcoercedargs, newe);
	}

	newm->args = newcoercedargs;
	newm->location = m->location;
	return (Node *) newm;
}

static Node *
transformSQLValueFunction(ParseState *pstate, SQLValueFunction *svf)
{
	/*
	 * 我们所需做的只是插入正确的结果类型，并在需要时 
	 * 校验 typmod，因此直接就地修改该节点。     
	 */
	switch (svf->op)
	{
		case SVFOP_CURRENT_DATE:
			svf->type = DATEOID;
			break;
		case SVFOP_CURRENT_TIME:
			svf->type = TIMETZOID;
			break;
		case SVFOP_CURRENT_TIME_N:
			svf->type = TIMETZOID;
			svf->typmod = anytime_typmod_check(true, svf->typmod);
			break;
		case SVFOP_CURRENT_TIMESTAMP:
			svf->type = TIMESTAMPTZOID;
			break;
		case SVFOP_CURRENT_TIMESTAMP_N:
			svf->type = TIMESTAMPTZOID;
			svf->typmod = anytimestamp_typmod_check(true, svf->typmod);
			break;
		case SVFOP_LOCALTIME:
			svf->type = TIMEOID;
			break;
		case SVFOP_LOCALTIME_N:
			svf->type = TIMEOID;
			svf->typmod = anytime_typmod_check(false, svf->typmod);
			break;
		case SVFOP_LOCALTIMESTAMP:
			svf->type = TIMESTAMPOID;
			break;
		case SVFOP_LOCALTIMESTAMP_N:
			svf->type = TIMESTAMPOID;
			svf->typmod = anytimestamp_typmod_check(false, svf->typmod);
			break;
		case SVFOP_CURRENT_ROLE:
		case SVFOP_CURRENT_USER:
		case SVFOP_USER:
		case SVFOP_SESSION_USER:
		case SVFOP_CURRENT_CATALOG:
		case SVFOP_CURRENT_SCHEMA:
			svf->type = NAMEOID;
			break;
	}

	return (Node *) svf;
}

static Node *
transformXmlExpr(ParseState *pstate, XmlExpr *x)
{
	XmlExpr    *newx;
	ListCell   *lc;
	int			i;

	newx = makeNode(XmlExpr);
	newx->op = x->op;
	if (x->name)
		newx->name = map_sql_identifier_to_xml_name(x->name, false, false);
	else
		newx->name = NULL;
	newx->xmloption = x->xmloption;
	newx->type = XMLOID;		/* 这只是标记该节点已完成转换 */
	newx->typmod = -1;
	newx->location = x->location;

	/*
	 * gram.y 将命名参数构建为 ResTarget 列表。逐个转换，  
	 * 并将名字拆出为单独的列表。    
	 */
	newx->named_args = NIL;
	newx->arg_names = NIL;

	foreach(lc, x->named_args)
	{
		ResTarget  *r = lfirst_node(ResTarget, lc);
		Node	   *expr;
		char	   *argname;

		expr = transformExprRecurse(pstate, r->val);

		if (r->name)
			argname = map_sql_identifier_to_xml_name(r->name, false, false);
		else if (IsA(r->val, ColumnRef))
			argname = map_sql_identifier_to_xml_name(FigureColname(r->val),
													 true, false);
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 x->op == IS_XMLELEMENT
					 ? errmsg("unnamed XML attribute value must be a column reference")
					 : errmsg("unnamed XML element value must be a column reference"),
					 parser_errposition(pstate, r->location)));
			argname = NULL;		/* 让编译器安静  */
		}

		/* 仅在 XMLELEMENT 中拒绝重复参数名   */
		if (x->op == IS_XMLELEMENT)
		{
			ListCell   *lc2;

			foreach(lc2, newx->arg_names)
			{
				if (strcmp(argname, strVal(lfirst(lc2))) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("XML attribute name \"%s\" appears more than once",
									argname),
							 parser_errposition(pstate, r->location)));
			}
		}

		newx->named_args = lappend(newx->named_args, expr);
		newx->arg_names = lappend(newx->arg_names, makeString(argname));
	}

	/* 其余参数的类型视具体函数而定，各不相同。       */
	newx->args = NIL;
	i = 0;
	foreach(lc, x->args)
	{
		Node	   *e = (Node *) lfirst(lc);
		Node	   *newe;

		newe = transformExprRecurse(pstate, e);
		switch (x->op)
		{
			case IS_XMLCONCAT:
				newe = coerce_to_specific_type(pstate, newe, XMLOID,
											   "XMLCONCAT");
				break;
			case IS_XMLELEMENT:
				/* 无需强制转换    */
				break;
			case IS_XMLFOREST:
				newe = coerce_to_specific_type(pstate, newe, XMLOID,
											   "XMLFOREST");
				break;
			case IS_XMLPARSE:
				if (i == 0)
					newe = coerce_to_specific_type(pstate, newe, TEXTOID,
												   "XMLPARSE");
				else
					newe = coerce_to_boolean(pstate, newe, "XMLPARSE");
				break;
			case IS_XMLPI:
				newe = coerce_to_specific_type(pstate, newe, TEXTOID,
											   "XMLPI");
				break;
			case IS_XMLROOT:
				if (i == 0)
					newe = coerce_to_specific_type(pstate, newe, XMLOID,
												   "XMLROOT");
				else if (i == 1)
					newe = coerce_to_specific_type(pstate, newe, TEXTOID,
												   "XMLROOT");
				else
					newe = coerce_to_specific_type(pstate, newe, INT4OID,
												   "XMLROOT");
				break;
			case IS_XMLSERIALIZE:
				/* 此处不处理  */
				Assert(false);
				break;
			case IS_DOCUMENT:
				newe = coerce_to_specific_type(pstate, newe, XMLOID,
											   "IS DOCUMENT");
				break;
		}
		newx->args = lappend(newx->args, newe);
		i++;
	}

	return (Node *) newx;
}

static Node *
transformXmlSerialize(ParseState *pstate, XmlSerialize *xs)
{
	Node	   *result;
	XmlExpr    *xexpr;
	Oid			targetType;
	int32		targetTypmod;

	xexpr = makeNode(XmlExpr);
	xexpr->op = IS_XMLSERIALIZE;
	xexpr->args = list_make1(coerce_to_specific_type(pstate,
													 transformExprRecurse(pstate, xs->expr),
													 XMLOID,
													 "XMLSERIALIZE"));

	typenameTypeIdAndMod(pstate, xs->typeName, &targetType, &targetTypmod);

	xexpr->xmloption = xs->xmloption;
	xexpr->indent = xs->indent;
	xexpr->location = xs->location;
	/* 实际上，我们只是需要有办法把表达式解析回来。   */
	xexpr->type = targetType;
	xexpr->typmod = targetTypmod;

	/*
	 * 实际目标类型按如下方式确定。SQL 允许 char 与      
	 * varchar 作为目标类型。凡能从 text 隐式 cast 的类型，   
	 * 我们都允许。用户定义的类文本类型即自动适用。   
	 *        
	 */
	result = coerce_to_target_type(pstate, (Node *) xexpr,
								   TEXTOID, targetType, targetTypmod,
								   COERCION_IMPLICIT,
								   COERCE_IMPLICIT_CAST,
								   -1);
	if (result == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CANNOT_COERCE),
				 errmsg("cannot cast XMLSERIALIZE result to %s",
						format_type_be(targetType)),
				 parser_errposition(pstate, xexpr->location)));
	return result;
}

static Node *
transformBooleanTest(ParseState *pstate, BooleanTest *b)
{
	const char *clausename;

	switch (b->booltesttype)
	{
		case IS_TRUE:
			clausename = "IS TRUE";
			break;
		case IS_NOT_TRUE:
			clausename = "IS NOT TRUE";
			break;
		case IS_FALSE:
			clausename = "IS FALSE";
			break;
		case IS_NOT_FALSE:
			clausename = "IS NOT FALSE";
			break;
		case IS_UNKNOWN:
			clausename = "IS UNKNOWN";
			break;
		case IS_NOT_UNKNOWN:
			clausename = "IS NOT UNKNOWN";
			break;
		default:
			elog(ERROR, "unrecognized booltesttype: %d",
				 (int) b->booltesttype);
			clausename = NULL;	/* 让编译器安静  */
	}

	b->arg = (Expr *) transformExprRecurse(pstate, (Node *) b->arg);

	b->arg = (Expr *) coerce_to_boolean(pstate,
										(Node *) b->arg,
										clausename);

	return (Node *) b;
}

static Node *
transformCurrentOfExpr(ParseState *pstate, CurrentOfExpr *cexpr)
{
	/* CURRENT OF 只能出现在 UPDATE/DELETE 的最顶层    */
	Assert(pstate->p_target_nsitem != NULL);
	cexpr->cvarno = pstate->p_target_nsitem->p_rtindex;

	/*
	 * 检查游标名是否匹配某个 REFCURSOR 类型的参数。        
	 * 若匹配，则把原始名字引用替换为参数引用。（这是  
	 * 专为方便 plpgsql 而设。）         
	 */
	if (cexpr->cursor_name != NULL) /* 以防已经转换过       */
	{
		ColumnRef  *cref = makeNode(ColumnRef);
		Node	   *node = NULL;

		/* 用给定名字构建一个非限定的 ColumnRef  */
		cref->fields = list_make1(makeString(cexpr->cursor_name));
		cref->location = -1;

		/* 看看解析器钩子是否提供了转换结果           */
		if (pstate->p_pre_columnref_hook != NULL)
			node = pstate->p_pre_columnref_hook(pstate, cref);
		if (node == NULL && pstate->p_post_columnref_hook != NULL)
			node = pstate->p_post_columnref_hook(pstate, cref, NULL);

		/*
		 * XXX 若得到的转换不是 refcursor Param，是否该报错？
		 * 目前最好还是默默忽略误匹配。                      
		 *         
		 */
		if (node != NULL && IsA(node, Param))
		{
			Param	   *p = (Param *) node;

			if (p->paramkind == PARAM_EXTERN &&
				p->paramtype == REFCURSOROID)
			{
				/* 匹配，故将 CURRENT OF 转为参数引用       */
				cexpr->cursor_name = NULL;
				cexpr->cursor_param = p->paramid;
			}
		}
	}

	return (Node *) cexpr;
}

/*
 * 构建一个整行引用来表示 "relation.*" 这种记法。         
 */
static Node *
transformWholeRowRef(ParseState *pstate, ParseNamespaceItem *nsitem,
					 int sublevels_up, int location)
{
	/*
	 * 构建合适的引用节点。通常这可以是一个         
	 * 整行 Var，但如果 nsitem 是 JOIN USING 别名，则它只包含  
	 * 底层 join RTE 各列的子集，那样行不通。于是我们     
	 * 立即将该引用展开为 RowExpr。                                
	 * 由于 JOIN USING 的公共列在此时已完全确定，         
	 * 所以现在就展开而不留到规划期，似无妨害。     
	 *          
	 *
	 * 注意，若 RTE 是返回标量的函数，我们只创建一个    
	 * 对函数值的普通引用，而非包含单列的复合类型。 
	 * 这乍看之下相当不一致，但这是我们一贯的做法。  
	 * 一个理由是：对复合关系而言，"rel" 和                 
	 * "rel.*" 含义相同，那么对标量函数为何不行呢...     
	 *                    
	 */
	if (nsitem->p_names == nsitem->p_rte->eref)
	{
		Var		   *result;

		result = makeWholeRowVar(nsitem->p_rte, nsitem->p_rtindex,
								 sublevels_up, true);

		/* makeWholeRowVar 不会填 location           */
		result->location = location;

		/* 若 Var 被任意外连接置空则标记  */
		markNullableIfNeeded(pstate, result);

		/* 将关系标记为需要整行 SELECT 权限       */
		markVarForSelectPriv(pstate, result);

		return (Node *) result;
	}
	else
	{
		RowExpr    *rowexpr;
		List	   *fields;

		/*
		 * 我们只想要 p_names->colnames 中列出的那些列，      
		 * 并且应使用那些名字，而非 RTE 中可能的别名。   
		 * 无须担心为 RTE 标记 SELECT 权限，                       
		 * 因为公共列肯定已被如此标记。                 
		 */
		expandRTE(nsitem->p_rte, nsitem->p_rtindex,
				  sublevels_up, location, false,
				  NULL, &fields);
		rowexpr = makeNode(RowExpr);
		rowexpr->args = list_truncate(fields,
									  list_length(nsitem->p_names->colnames));
		rowexpr->row_typeid = RECORDOID;
		rowexpr->row_format = COERCE_IMPLICIT_CAST;
		rowexpr->colnames = copyObject(nsitem->p_names->colnames);
		rowexpr->location = location;

		/* XXX 我们应把该行标记为可能为 NULL     */

		return (Node *) rowexpr;
	}
}

/*
 * 处理显式的 CAST 构造。    
 *
 * 转换参数，查找类型名，并应用任何必要的             
 * 强制转换函数。
 */
static Node *
transformTypeCast(ParseState *pstate, TypeCast *tc)
{
	Node	   *result;
	Node	   *arg = tc->arg;
	Node	   *expr;
	Oid			inputType;
	Oid			targetType;
	int32		targetTypmod;
	int			location;

	/* 先查找类型名          */
	typenameTypeIdAndMod(pstate, tc->typeName, &targetType, &targetTypmod);

	/*
	 * 若类型转换的对象是 ARRAY[] 构造且目标类型为         
	 * 数组类型，则直接调用 transformArrayExpr()，以便传递    
	 * 类型信息。这可避免某些 transformArrayExpr() 可能       
	 * 推断不出正确类型的情况。否则照常转换参数。        
	 *                                 
	 */
	if (IsA(arg, A_ArrayExpr))
	{
		Oid			targetBaseType;
		int32		targetBaseTypmod;
		Oid			elementType;

		/*
		 * 若目标为数组上的域，则先改用基础数组类型，
		 * 之后再 cast 为该域。若目标并非域，              
		 * 那么余下的步骤将不做任何事。                     
		 *                 
		 */
		targetBaseTypmod = targetTypmod;
		targetBaseType = getBaseTypeAndTypmod(targetType, &targetBaseTypmod);
		elementType = get_element_type(targetBaseType);
		if (OidIsValid(elementType))
		{
			expr = transformArrayExpr(pstate,
									  (A_ArrayExpr *) arg,
									  targetBaseType,
									  elementType,
									  targetBaseTypmod);
		}
		else
			expr = transformExprRecurse(pstate, arg);
	}
	else
		expr = transformExprRecurse(pstate, arg);

	inputType = exprType(expr);
	if (inputType == InvalidOid)
		return expr;			/* NULL 输入则不处理  */

	/*
	 * 强制转换的位置优先取 :: 或 CAST 符号的位置，       
	 * 但若没有，则使用类型名的位置                         
	 * （例如 TypeName 'string' 语法中就可能出现）。       
	 */
	location = tc->location;
	if (location < 0)
		location = tc->typeName->location;

	result = coerce_to_target_type(pstate, expr, inputType,
								   targetType, targetTypmod,
								   COERCION_EXPLICIT,
								   COERCE_EXPLICIT_CAST,
								   location);
	if (result == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CANNOT_COERCE),
				 errmsg("cannot cast type %s to %s",
						format_type_be(inputType),
						format_type_be(targetType)),
				 parser_coercion_errposition(pstate, location, expr)));

	return result;
}

/*
 * 处理显式 COLLATE 子句。    
 *
 * 转换参数，并查找排序规则名。             
 */
static Node *
transformCollateClause(ParseState *pstate, CollateClause *c)
{
	CollateExpr *newc;
	Oid			argtype;

	newc = makeNode(CollateExpr);
	newc->arg = (Expr *) transformExprRecurse(pstate, c->arg);

	argtype = exprType((Node *) newc->arg);

	/*
	 * 未知类型不可用于排序，但 coerce_type() 会另行处理， 
	 * 所以这里放行即可。         
	 */
	if (!type_is_collatable(argtype) && argtype != UNKNOWNOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("collations are not supported by type %s",
						format_type_be(argtype)),
				 parser_errposition(pstate, c->location)));

	newc->collOid = LookupCollation(pstate, c->collname, c->location);
	newc->location = c->location;

	return (Node *) newc;
}

/*
 * 转换 "row compare-op row" 构造        
 *
 * 输入是已经转换好的表达式列表。           
 * 和 coerce_type 一样，若不需要未知参数特殊处理，   
 * pstate 可为 NULL   
 *
 * 输出可能是单个 OpExpr、多个 OpExpr 的 AND 或 OR 组合，  
 * 也可能是 RowCompareExpr。任何情况下都保证返回布尔。 
 * AND、OR 与 RowCompareExpr 这几种情况还进一步蕴含       
 * 了操作符的行为（如同 =、<> 或 < <= > >=）。          
 */
static Node *
make_row_comparison_op(ParseState *pstate, List *opname,
					   List *largs, List *rargs, int location)
{
	RowCompareExpr *rcexpr;
	RowCompareType rctype;
	List	   *opexprs;
	List	   *opnos;
	List	   *opfamilies;
	ListCell   *l,
			   *r;
	List	  **opinfo_lists;
	Bitmapset  *strats;
	int			nopers;
	int			i;

	nopers = list_length(largs);
	if (nopers != list_length(rargs))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("unequal number of entries in row expressions"),
				 parser_errposition(pstate, location)));

	/*
	 * 我们无法比较零长度行，因为没有原则性的依据来    
	 * 确定该用什么操作符。        
	 */
	if (nopers == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot compare rows of zero length"),
				 parser_errposition(pstate, location)));

	/*
	 * 找出全部成对操作符，使用 make_op 以使行为与          
	 * 简单标量情形一致。           
	 */
	opexprs = NIL;
	forboth(l, largs, r, rargs)
	{
		Node	   *larg = (Node *) lfirst(l);
		Node	   *rarg = (Node *) lfirst(r);
		OpExpr	   *cmp;

		cmp = castNode(OpExpr, make_op(pstate, opname, larg, rarg,
									   pstate->p_last_srf, location));

		/*
		 * 我们不用 coerce_to_boolean，因为坚持要求         
		 * 操作符直接产生布尔而非经强制转换。若其   
		 * 非布尔，就不会属于任何索引操作符族...   
		 */
		if (cmp->opresulttype != BOOLOID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("row comparison operator must yield type boolean, "
							"not type %s",
							format_type_be(cmp->opresulttype)),
					 parser_errposition(pstate, location)));
		if (expression_returns_set((Node *) cmp))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("row comparison operator must not return a set"),
					 parser_errposition(pstate, location)));
		opexprs = lappend(opexprs, cmp);
	}

	/*
	 * 若行长为 1，就直接返回单个操作符。此时我们         
	 * 并不坚持要为该操作符确定 btree 语义（但仍需要其 
	 * 返回布尔值）。               
	 */
	if (nopers == 1)
		return (Node *) linitial(opexprs);

	/*
	 * 现在我们必须确定这组操作符适用哪种行比较语义   
	 * （= <> < <= > >=）。接着查找包含这些操作符的  
	 * btree 操作符族，并查看每个操作符存在哪些解释  
	 * （策略号）。               
	 */
	opinfo_lists = (List **) palloc(nopers * sizeof(List *));
	strats = NULL;
	i = 0;
	foreach(l, opexprs)
	{
		Oid			opno = ((OpExpr *) lfirst(l))->opno;
		Bitmapset  *this_strats;
		ListCell   *j;

		opinfo_lists[i] = get_op_btree_interpretation(opno);

		/*
		 * 把策略号转换成 Bitmapset，使交集计算变得容易。  
		 *                  
		 */
		this_strats = NULL;
		foreach(j, opinfo_lists[i])
		{
			OpBtreeInterpretation *opinfo = lfirst(j);

			this_strats = bms_add_member(this_strats, opinfo->strategy);
		}
		if (i == 0)
			strats = this_strats;
		else
			strats = bms_int_members(strats, this_strats);
		i++;
	}

	/*
	 * 若存在多种公共解释，可以任选其一...                
	 * 此代码随意地选择最小的 btree 策略号。            
	 *        
	 */
	i = bms_next_member(strats, -1);
	if (i < 0)
	{
		/* 无公共解释，因此失败    */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine interpretation of row comparison operator %s",
						strVal(llast(opname))),
				 errhint("Row comparison operators must be associated with btree operator families."),
				 parser_errposition(pstate, location)));
	}
	rctype = (RowCompareType) i;

	/*
	 * 对于 = 和 <> 情形，我们只是用 AND 或 OR 分别组合      
	 * 各操作符。 
	 */
	if (rctype == ROWCOMPARE_EQ)
		return (Node *) makeBoolExpr(AND_EXPR, opexprs, location);
	if (rctype == ROWCOMPARE_NE)
		return (Node *) makeBoolExpr(OR_EXPR, opexprs, location);

	/*
	 * 否则我们需要精确选择每个操作符关联的操作符族  
	 *               
	 */
	opfamilies = NIL;
	for (i = 0; i < nopers; i++)
	{
		Oid			opfamily = InvalidOid;
		ListCell   *j;

		foreach(j, opinfo_lists[i])
		{
			OpBtreeInterpretation *opinfo = lfirst(j);

			if (opinfo->strategy == rctype)
			{
				opfamily = opinfo->opfamily_id;
				break;
			}
		}
		if (OidIsValid(opfamily))
			opfamilies = lappend_oid(opfamilies, opfamily);
		else					/* 不应发生      */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not determine interpretation of row comparison operator %s",
							strVal(llast(opname))),
					 errdetail("There are multiple equally-plausible candidates."),
					 parser_errposition(pstate, location)));
	}

	/*
	 * 现在拆解各 OpExpr 并创建 RowCompareExpr。      
	 *
	 * 注意：无法复用传入的 largs/rargs 列表，因为     
	 * make_op 可能插入了强制转换操作。          
	 */
	opnos = NIL;
	largs = NIL;
	rargs = NIL;
	foreach(l, opexprs)
	{
		OpExpr	   *cmp = (OpExpr *) lfirst(l);

		opnos = lappend_oid(opnos, cmp->opno);
		largs = lappend(largs, linitial(cmp->args));
		rargs = lappend(rargs, lsecond(cmp->args));
	}

	rcexpr = makeNode(RowCompareExpr);
	rcexpr->rctype = rctype;
	rcexpr->opnos = opnos;
	rcexpr->opfamilies = opfamilies;
	rcexpr->inputcollids = NIL; /* assign_expr_collations 自会修正  */
	rcexpr->largs = largs;
	rcexpr->rargs = rargs;

	return (Node *) rcexpr;
}

/*
 * 转换 "row IS DISTINCT FROM row" 构造        
 *
 * 输入的 RowExpr 已经过转换         
 */
static Node *
make_row_distinct_op(ParseState *pstate, List *opname,
					 RowExpr *lrow, RowExpr *rrow,
					 int location)
{
	Node	   *result = NULL;
	List	   *largs = lrow->args;
	List	   *rargs = rrow->args;
	ListCell   *l,
			   *r;

	if (list_length(largs) != list_length(rargs))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("unequal number of entries in row expressions"),
				 parser_errposition(pstate, location)));

	forboth(l, largs, r, rargs)
	{
		Node	   *larg = (Node *) lfirst(l);
		Node	   *rarg = (Node *) lfirst(r);
		Node	   *cmp;

		cmp = (Node *) make_distinct_op(pstate, opname, larg, rarg, location);
		if (result == NULL)
			result = cmp;
		else
			result = (Node *) makeBoolExpr(OR_EXPR,
										   list_make2(result, cmp),
										   location);
	}

	if (result == NULL)
	{
		/* 零长度行？生成常量 FALSE          */
		result = makeBoolConst(false, false);
	}

	return result;
}

/*
 * 为 IS DISTINCT FROM 操作符构建节点    
 */
static Expr *
make_distinct_op(ParseState *pstate, List *opname, Node *ltree, Node *rtree,
				 int location)
{
	Expr	   *result;

	result = make_op(pstate, opname, ltree, rtree,
					 pstate->p_last_srf, location);
	if (((OpExpr *) result)->opresulttype != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("IS DISTINCT FROM requires = operator to yield boolean"),
				 parser_errposition(pstate, location)));
	if (((OpExpr *) result)->opretset)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
		/* translator: %s is name of a SQL construct, eg NULLIF */
				 errmsg("%s must not return a set", "IS DISTINCT FROM"),
				 parser_errposition(pstate, location)));

	/*
	 * 我们依赖DistinctExpr与OpExpr为同一结构体 
	 */
	NodeSetTag(result, T_DistinctExpr);

	return result;
}

/*
 * 由IS [NOT] DISTINCT FROM NULL构造生成NullTest节点             
 *
 * "arg"是未经转换的另一参数      
 */
static Node *
make_nulltest_from_distinct(ParseState *pstate, A_Expr *distincta, Node *arg)
{
	NullTest   *nt = makeNode(NullTest);

	nt->arg = (Expr *) transformExprRecurse(pstate, arg);
	/* 参数可为任意类型，故不做强制转换 */
	if (distincta->kind == AEXPR_NOT_DISTINCT)
		nt->nulltesttype = IS_NULL;
	else
		nt->nulltesttype = IS_NOT_NULL;
	/* 无论arg是否为复合类型，argisrow=false 都正确   */
	nt->argisrow = false;
	nt->location = distincta->location;
	return (Node *) nt;
}

/*
 * 生成按类别标识表达式的字符串。      
 *
 * 注：可行时结果应使用简单 SQL 关键字。若此法         
 * 不奏效，则检查调用点，看是否需要自定义的错误     
 * 消息字符串。   
 */
const char *
ParseExprKindName(ParseExprKind exprKind)
{
	switch (exprKind)
	{
		case EXPR_KIND_NONE:
			return "invalid expression context";
		case EXPR_KIND_OTHER:
			return "extension expression";
		case EXPR_KIND_JOIN_ON:
			return "JOIN/ON";
		case EXPR_KIND_JOIN_USING:
			return "JOIN/USING";
		case EXPR_KIND_FROM_SUBSELECT:
			return "sub-SELECT in FROM";
		case EXPR_KIND_FROM_FUNCTION:
			return "function in FROM";
		case EXPR_KIND_WHERE:
			return "WHERE";
		case EXPR_KIND_POLICY:
			return "POLICY";
		case EXPR_KIND_HAVING:
			return "HAVING";
		case EXPR_KIND_FILTER:
			return "FILTER";
		case EXPR_KIND_WINDOW_PARTITION:
			return "window PARTITION BY";
		case EXPR_KIND_WINDOW_ORDER:
			return "window ORDER BY";
		case EXPR_KIND_WINDOW_FRAME_RANGE:
			return "window RANGE";
		case EXPR_KIND_WINDOW_FRAME_ROWS:
			return "window ROWS";
		case EXPR_KIND_WINDOW_FRAME_GROUPS:
			return "window GROUPS";
		case EXPR_KIND_SELECT_TARGET:
			return "SELECT";
		case EXPR_KIND_INSERT_TARGET:
			return "INSERT";
		case EXPR_KIND_UPDATE_SOURCE:
		case EXPR_KIND_UPDATE_TARGET:
			return "UPDATE";
		case EXPR_KIND_MERGE_WHEN:
			return "MERGE WHEN";
		case EXPR_KIND_GROUP_BY:
			return "GROUP BY";
		case EXPR_KIND_ORDER_BY:
			return "ORDER BY";
		case EXPR_KIND_DISTINCT_ON:
			return "DISTINCT ON";
		case EXPR_KIND_LIMIT:
			return "LIMIT";
		case EXPR_KIND_OFFSET:
			return "OFFSET";
		case EXPR_KIND_RETURNING:
			return "RETURNING";
		case EXPR_KIND_VALUES:
		case EXPR_KIND_VALUES_SINGLE:
			return "VALUES";
		case EXPR_KIND_CHECK_CONSTRAINT:
		case EXPR_KIND_DOMAIN_CHECK:
			return "CHECK";
		case EXPR_KIND_COLUMN_DEFAULT:
		case EXPR_KIND_FUNCTION_DEFAULT:
			return "DEFAULT";
		case EXPR_KIND_INDEX_EXPRESSION:
			return "index expression";
		case EXPR_KIND_INDEX_PREDICATE:
			return "index predicate";
		case EXPR_KIND_STATS_EXPRESSION:
			return "statistics expression";
		case EXPR_KIND_ALTER_COL_TRANSFORM:
			return "USING";
		case EXPR_KIND_EXECUTE_PARAMETER:
			return "EXECUTE";
		case EXPR_KIND_TRIGGER_WHEN:
			return "WHEN";
		case EXPR_KIND_PARTITION_BOUND:
			return "partition bound";
		case EXPR_KIND_PARTITION_EXPRESSION:
			return "PARTITION BY";
		case EXPR_KIND_CALL_ARGUMENT:
			return "CALL";
		case EXPR_KIND_COPY_WHERE:
			return "WHERE";
		case EXPR_KIND_GENERATED_COLUMN:
			return "GENERATED AS";
		case EXPR_KIND_CYCLE_MARK:
			return "CYCLE";

			/*
			 * 这里有意不设 default: 分支，这样新增        
			 * ParseExprKind 而不扩展此 switch 时，编译器会 
			 * 发出警告。若运行时遇到无法识别的值，落到 
			 * "unrecognized" 返回。                                 
			 */
	}
	return "unrecognized expression kind";
}

/*
 * 按JSON编码名构造字符串Const节点。  
 *
 * UTF8为默认编码。   
 */
static Const *
getJsonEncodingConst(JsonFormat *format)
{
	JsonEncoding encoding;
	const char *enc;
	Name		encname = palloc(sizeof(NameData));

	if (!format ||
		format->format_type == JS_FORMAT_DEFAULT ||
		format->encoding == JS_ENC_DEFAULT)
		encoding = JS_ENC_UTF8;
	else
		encoding = format->encoding;

	switch (encoding)
	{
		case JS_ENC_UTF16:
			enc = "UTF16";
			break;
		case JS_ENC_UTF32:
			enc = "UTF32";
			break;
		case JS_ENC_UTF8:
			enc = "UTF8";
			break;
		default:
			elog(ERROR, "invalid JSON encoding: %d", encoding);
			break;
	}

	namestrcpy(encname, enc);

	return makeConst(NAMEOID, -1, InvalidOid, NAMEDATALEN,
					 NameGetDatum(encname), false, false);
}

/*
 * 用指定的JSON格式编码进行bytea => text转换。           
 */
static Node *
makeJsonByteaToTextConversion(Node *expr, JsonFormat *format, int location)
{
	Const	   *encoding = getJsonEncodingConst(format);
	FuncExpr   *fexpr = makeFuncExpr(F_CONVERT_FROM, TEXTOID,
									 list_make2(expr, encoding),
									 InvalidOid, InvalidOid,
									 COERCE_EXPLICIT_CALL);

	fexpr->location = location;

	return (Node *) fexpr;
}

/*
 * 用指定的输入JSON格式转换JSON值表达式，若未指定   
 * 则用默认格式。    
 *
 * 返回的表达式要么是ve->raw_expr强制转换为text（若         
 * 需要）；要么是JsonValueExpr，若格式需要，formatted_expr  
 * 被置为raw_expr的转换副本。 
 */
static Node *
transformJsonValueExpr(ParseState *pstate, const char *constructName,
					   JsonValueExpr *ve, JsonFormatType default_format)
{
	Node	   *expr = transformExprRecurse(pstate, (Node *) ve->raw_expr);
	Node	   *rawexpr;
	JsonFormatType format;
	Oid			exprtype;
	int			location;
	char		typcategory;
	bool		typispreferred;

	if (exprType(expr) == UNKNOWNOID)
		expr = coerce_to_specific_type(pstate, expr, TEXTOID, constructName);

	rawexpr = expr;
	exprtype = exprType(expr);
	location = exprLocation(expr);

	get_type_category_preferred(exprtype, &typcategory, &typispreferred);

	if (ve->format->format_type != JS_FORMAT_DEFAULT)
	{
		if (ve->format->encoding != JS_ENC_DEFAULT && exprtype != BYTEAOID)
			ereport(ERROR,
					errcode(ERRCODE_DATATYPE_MISMATCH),
					errmsg("JSON ENCODING clause is only allowed for bytea input type"),
					parser_errposition(pstate, ve->format->location));

		if (exprtype == JSONOID || exprtype == JSONBOID)
			format = JS_FORMAT_DEFAULT; /* json[b]类型不做格式化*/
		else
			format = ve->format->format_type;
	}
	else if (exprtype == JSONOID || exprtype == JSONBOID)
		format = JS_FORMAT_DEFAULT; /* json[b]类型不做格式化*/
	else
		format = default_format;

	if (format != JS_FORMAT_DEFAULT)
	{
		Oid			targettype = format == JS_FORMAT_JSONB ? JSONBOID : JSONOID;
		Node	   *coerced;

		if (exprtype != BYTEAOID && typcategory != TYPCATEGORY_STRING)
			ereport(ERROR,
					errcode(ERRCODE_DATATYPE_MISMATCH),
					ve->format->format_type == JS_FORMAT_DEFAULT ?
					errmsg("cannot use non-string types with implicit FORMAT JSON clause") :
					errmsg("cannot use non-string types with explicit FORMAT JSON clause"),
					parser_errposition(pstate, ve->format->location >= 0 ?
									   ve->format->location : location));

		/* 从bytea转换已编码JSON文本。  */
		if (format == JS_FORMAT_JSON && exprtype == BYTEAOID)
		{
			expr = makeJsonByteaToTextConversion(expr, ve->format, location);
			exprtype = TEXTOID;
		}

		/* 尝试强制转换为目标类型 */
		coerced = coerce_to_target_type(pstate, expr, exprtype,
										targettype, -1,
										COERCION_EXPLICIT,
										COERCE_EXPLICIT_CAST,
										location);

		if (!coerced)
		{
			/* 若强制转换失败，用to_json()/to_jsonb()函数   */
			Oid			fnoid = targettype == JSONOID ? F_TO_JSON : F_TO_JSONB;
			FuncExpr   *fexpr = makeFuncExpr(fnoid, targettype,
											 list_make1(expr),
											 InvalidOid, InvalidOid,
											 COERCE_EXPLICIT_CALL);

			fexpr->location = location;

			coerced = (Node *) fexpr;
		}

		if (coerced == expr)
			expr = rawexpr;
		else
		{
			ve = copyObject(ve);
			ve->raw_expr = (Expr *) rawexpr;
			ve->formatted_expr = (Expr *) coerced;

			expr = (Node *) ve;
		}
	}

	/* 若返回JsonValueExpr，则formatted_expr必须已被设置。  */
	Assert(!IsA(expr, JsonValueExpr) ||
		   ((JsonValueExpr *) expr)->formatted_expr != NULL);

	return expr;
}

/*
 * 检查指定输出格式是否适用于目标类型。                  
 */
static void
checkJsonOutputFormat(ParseState *pstate, const JsonFormat *format,
					  Oid targettype, bool allow_format_for_non_strings)
{
	if (!allow_format_for_non_strings &&
		format->format_type != JS_FORMAT_DEFAULT &&
		(targettype != BYTEAOID &&
		 targettype != JSONOID &&
		 targettype != JSONBOID))
	{
		char		typcategory;
		bool		typispreferred;

		get_type_category_preferred(targettype, &typcategory, &typispreferred);

		if (typcategory != TYPCATEGORY_STRING)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					parser_errposition(pstate, format->location),
					errmsg("cannot use JSON format with non-string output types"));
	}

	if (format->format_type == JS_FORMAT_JSON)
	{
		JsonEncoding enc = format->encoding != JS_ENC_DEFAULT ?
			format->encoding : JS_ENC_UTF8;

		if (targettype != BYTEAOID &&
			format->encoding != JS_ENC_DEFAULT)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					parser_errposition(pstate, format->location),
					errmsg("cannot set JSON encoding for non-bytea output types"));

		if (enc != JS_ENC_UTF8)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("unsupported JSON encoding"),
					errhint("Only UTF8 JSON encoding is supported."),
					parser_errposition(pstate, format->location));
	}
}

/*
 * 转换JSON输出子句。    
 *
 * 设置目标类型OID与修饰符。 
 * 设置默认格式，或检查指定格式是否适用于目标           
 * 类型。       
 */
static JsonReturning *
transformJsonOutput(ParseState *pstate, const JsonOutput *output,
					bool allow_format)
{
	JsonReturning *ret;

	/* 若未指定输出子句，则构造默认子句值。       */
	if (!output)
	{
		ret = makeNode(JsonReturning);

		ret->format = makeJsonFormat(JS_FORMAT_DEFAULT, JS_ENC_DEFAULT, -1);
		ret->typid = InvalidOid;
		ret->typmod = -1;

		return ret;
	}

	ret = copyObject(output->returning);

	typenameTypeIdAndMod(pstate, output->typeName, &ret->typid, &ret->typmod);

	if (output->typeName->setof)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("returning SETOF types is not supported in SQL/JSON functions"));

	if (ret->format->format_type == JS_FORMAT_DEFAULT)
		/* 返回jsonb时设置JSONB格式，否则设置JSON格式           */
		ret->format->format_type =
			ret->typid == JSONBOID ? JS_FORMAT_JSONB : JS_FORMAT_JSON;
	else
		checkJsonOutputFormat(pstate, ret->format, ret->typid, allow_format);

	return ret;
}

/*
 * 转换JSON构造函数的JSON输出子句。               
 *
 * 若未指定RETURNING类型，则从参数类型推导。    
 */
static JsonReturning *
transformJsonConstructorOutput(ParseState *pstate, JsonOutput *output,
							   List *args)
{
	JsonReturning *returning = transformJsonOutput(pstate, output, true);

	if (!OidIsValid(returning->typid))
	{
		ListCell   *lc;
		bool		have_jsonb = false;

		foreach(lc, args)
		{
			Node	   *expr = lfirst(lc);
			Oid			typid = exprType(expr);

			have_jsonb |= typid == JSONBOID;

			if (have_jsonb)
				break;
		}

		if (have_jsonb)
		{
			returning->typid = JSONBOID;
			returning->format->format_type = JS_FORMAT_JSONB;
		}
		else
		{
			/* XXX标准默认是TEXT，但我们返回JSON            */
			returning->typid = JSONOID;
			returning->format->format_type = JS_FORMAT_JSON;
		}

		returning->typmod = -1;
	}

	return returning;
}

/*
 * 将json[b]值的函数表达式转换为输出类型。      
 */
static Node *
coerceJsonFuncExpr(ParseState *pstate, Node *expr,
				   const JsonReturning *returning, bool report_error)
{
	Node	   *res;
	int			location;
	Oid			exprtype = exprType(expr);

	/* 若输出类型未指定或等于函数类型，则直接返回。 */
	if (!OidIsValid(returning->typid) || returning->typid == exprtype)
		return expr;

	location = exprLocation(expr);

	if (location < 0)
		location = returning->format->location;

	/* RETURNING bytea FORMAT json 的特例        */
	if (returning->format->format_type == JS_FORMAT_JSON &&
		returning->typid == BYTEAOID)
	{
		/* 用pg_convert_to()将json文本编码为bytea     */
		Node	   *texpr = coerce_to_specific_type(pstate, expr, TEXTOID,
													"JSON_FUNCTION");
		Const	   *enc = getJsonEncodingConst(returning->format);
		FuncExpr   *fexpr = makeFuncExpr(F_CONVERT_TO, BYTEAOID,
										 list_make2(texpr, enc),
										 InvalidOid, InvalidOid,
										 COERCE_EXPLICIT_CALL);

		fexpr->location = location;

		return (Node *) fexpr;
	}

	/* 尝试将表达式转换为输出类型     */
	res = coerce_to_target_type(pstate, expr, exprtype,
								returning->typid, returning->typmod,
	/* XXX转换为char(N)时抛出错误          */
								COERCION_EXPLICIT,
								COERCE_EXPLICIT_CAST,
								location);

	if (!res && report_error)
		ereport(ERROR,
				errcode(ERRCODE_CANNOT_COERCE),
				errmsg("cannot cast type %s to %s",
					   format_type_be(exprtype),
					   format_type_be(returning->typid)),
				parser_coercion_errposition(pstate, location, expr));

	return res;
}

/*
 * 生成JsonConstructorExpr节点 
 */
static Node *
makeJsonConstructorExpr(ParseState *pstate, JsonConstructorType type,
						List *args, Expr *fexpr, JsonReturning *returning,
						bool unique, bool absent_on_null, int location)
{
	JsonConstructorExpr *jsctor = makeNode(JsonConstructorExpr);
	Node	   *placeholder;
	Node	   *coercion;

	jsctor->args = args;
	jsctor->func = fexpr;
	jsctor->type = type;
	jsctor->returning = returning;
	jsctor->unique = unique;
	jsctor->absent_on_null = absent_on_null;
	jsctor->location = location;

	/*
	 * 若需要，强制转换为RETURNING类型和格式。       
	 * 我们在此把CaseTestExpr当作占位符，用来传递    
	 * 'fexpr'的求值结果，或ExecEvalJsonConstructor()产生的、类型
	 * 为JSON或JSONB的结果，传给强制转换函数。 
	 */
	if (fexpr)
	{
		CaseTestExpr *cte = makeNode(CaseTestExpr);

		cte->typeId = exprType((Node *) fexpr);
		cte->typeMod = exprTypmod((Node *) fexpr);
		cte->collation = exprCollation((Node *) fexpr);

		placeholder = (Node *) cte;
	}
	else
	{
		CaseTestExpr *cte = makeNode(CaseTestExpr);

		cte->typeId = returning->format->format_type == JS_FORMAT_JSONB ?
			JSONBOID : JSONOID;
		cte->typeMod = -1;
		cte->collation = InvalidOid;

		placeholder = (Node *) cte;
	}

	coercion = coerceJsonFuncExpr(pstate, placeholder, returning, true);

	if (coercion != placeholder)
		jsctor->coercion = (Expr *) coercion;

	return (Node *) jsctor;
}

/*
 * 转换JSON_OBJECT()构造函数。  
 *
 * JSON_OBJECT()被转换成类型为JSCTOR_JSON_OBJECT的             
 * JsonConstructorExpr节点。结果强制转换为由ctor->output   
 * 指定类型。 
 */
static Node *
transformJsonObjectConstructor(ParseState *pstate, JsonObjectConstructor *ctor)
{
	JsonReturning *returning;
	List	   *args = NIL;

	/* 如有键值对则进行转换    */
	if (ctor->exprs)
	{
		ListCell   *lc;

		/* 转换并追加键值参数              */
		foreach(lc, ctor->exprs)
		{
			JsonKeyValue *kv = castNode(JsonKeyValue, lfirst(lc));
			Node	   *key = transformExprRecurse(pstate, (Node *) kv->key);
			Node	   *val = transformJsonValueExpr(pstate, "JSON_OBJECT()",
													 kv->value,
													 JS_FORMAT_DEFAULT);

			args = lappend(args, key);
			args = lappend(args, val);
		}
	}

	returning = transformJsonConstructorOutput(pstate, ctor->output, args);

	return makeJsonConstructorExpr(pstate, JSCTOR_JSON_OBJECT, args, NULL,
								   returning, ctor->unique,
								   ctor->absent_on_null, ctor->location);
}

/*
 * 把JSON_ARRAY(query [FORMAT] [RETURNING] [ON NULL])转换成   
 *  (SELECT  JSON_ARRAYAGG(a  [FORMAT] [RETURNING] [ON NULL]) FROM (query) q(a))
 */
static Node *
transformJsonArrayQueryConstructor(ParseState *pstate,
								   JsonArrayQueryConstructor *ctor)
{
	SubLink    *sublink = makeNode(SubLink);
	SelectStmt *select = makeNode(SelectStmt);
	RangeSubselect *range = makeNode(RangeSubselect);
	Alias	   *alias = makeNode(Alias);
	ResTarget  *target = makeNode(ResTarget);
	JsonArrayAgg *agg = makeNode(JsonArrayAgg);
	ColumnRef  *colref = makeNode(ColumnRef);
	Query	   *query;
	ParseState *qpstate;

	/* 转换查询仅为统计目标列表条目数。       */
	qpstate = make_parsestate(pstate);

	query = transformStmt(qpstate, copyObject(ctor->query));

	if (count_nonjunk_tlist_entries(query->targetList) != 1)
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("subquery must return only one column"),
				parser_errposition(pstate, ctor->location));

	free_parsestate(qpstate);

	colref->fields = list_make2(makeString(pstrdup("q")),
								makeString(pstrdup("a")));
	colref->location = ctor->location;

	/*
	 * 无需格式化，故将formatted_expr设为与raw_expr相同。
	 *          
	 */
	agg->arg = makeJsonValueExpr((Expr *) colref, (Expr *) colref,
								 ctor->format);
	agg->absent_on_null = ctor->absent_on_null;
	agg->constructor = makeNode(JsonAggConstructor);
	agg->constructor->agg_order = NIL;
	agg->constructor->output = ctor->output;
	agg->constructor->location = ctor->location;

	target->name = NULL;
	target->indirection = NIL;
	target->val = (Node *) agg;
	target->location = ctor->location;

	alias->aliasname = pstrdup("q");
	alias->colnames = list_make1(makeString(pstrdup("a")));

	range->lateral = false;
	range->subquery = ctor->query;
	range->alias = alias;

	select->targetList = list_make1(target);
	select->fromClause = list_make1(range);

	sublink->subLinkType = EXPR_SUBLINK;
	sublink->subLinkId = 0;
	sublink->testexpr = NULL;
	sublink->operName = NIL;
	sublink->subselect = (Node *) select;
	sublink->location = ctor->location;

	return transformExprRecurse(pstate, (Node *) sublink);
}

/*
 * JSON_OBJECTAGG与JSON_ARRAYAGG转换的公共代码。          
 */
static Node *
transformJsonAggConstructor(ParseState *pstate, JsonAggConstructor *agg_ctor,
							JsonReturning *returning, List *args,
							Oid aggfnoid, Oid aggtype,
							JsonConstructorType ctor_type,
							bool unique, bool absent_on_null)
{
	Node	   *node;
	Expr	   *aggfilter;

	aggfilter = agg_ctor->agg_filter ? (Expr *)
		transformWhereClause(pstate, agg_ctor->agg_filter,
							 EXPR_KIND_FILTER, "FILTER") : NULL;

	if (agg_ctor->over)
	{
		/* 窗口函数    */
		WindowFunc *wfunc = makeNode(WindowFunc);

		wfunc->winfnoid = aggfnoid;
		wfunc->wintype = aggtype;
		/* wincollid与inputcollid由parse_collate.c设置          */
		wfunc->args = args;
		wfunc->aggfilter = aggfilter;
		/* winref由transformWindowFuncCall设置        */
		wfunc->winstar = false;
		wfunc->winagg = true;
		wfunc->location = agg_ctor->location;

		/*
		 * 窗口中尚不允许有序聚合。   
		 */
		if (agg_ctor->agg_order != NIL)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("aggregate ORDER BY is not implemented for window functions"),
					parser_errposition(pstate, agg_ctor->location));

		/* parse_agg.c做额外的窗口函数专用处理             */
		transformWindowFuncCall(pstate, wfunc, agg_ctor->over);

		node = (Node *) wfunc;
	}
	else
	{
		Aggref	   *aggref = makeNode(Aggref);

		aggref->aggfnoid = aggfnoid;
		aggref->aggtype = aggtype;

		/* aggcollid与inputcollid由parse_collate.c设置          */
		/* aggtranstype由规划器设置      */
		/* aggargtypes由transformAggregateCall设置        */
		/* aggdirectargs与args由transformAggregateCall设置          */
		/* aggorder与aggdistinct由transformAggregateCall设置          */
		aggref->aggfilter = aggfilter;
		aggref->aggstar = false;
		aggref->aggvariadic = false;
		aggref->aggkind = AGGKIND_NORMAL;
		aggref->aggpresorted = false;
		/* agglevelsup由transformAggregateCall设置        */
		aggref->aggsplit = AGGSPLIT_SIMPLE; /* 规划器可能改动     */
		aggref->aggno = -1;		/* aggno与aggtransno由规划器设置  */
		aggref->aggtransno = -1;
		aggref->location = agg_ctor->location;

		transformAggregateCall(pstate, aggref, args, agg_ctor->agg_order, false);

		node = (Node *) aggref;
	}

	return makeJsonConstructorExpr(pstate, ctor_type, NIL, (Expr *) node,
								   returning, unique, absent_on_null,
								   agg_ctor->location);
}

/*
 * 转换JSON_OBJECTAGG()聚合函数。         
 *
 * JSON_OBJECTAGG()被转换成类型为JSCTOR_JSON_OBJECTAGG的          
 * JsonConstructorExpr节点，它在运行时变成
 * json[b]_object_agg[_unique][_strict](agg->arg->key, agg->arg->value)     
 * 调用，取决于输出JSON格式。结果被强制转换为目标      
 * 类型，见agg->constructor->output。
 */
static Node *
transformJsonObjectAgg(ParseState *pstate, JsonObjectAgg *agg)
{
	JsonReturning *returning;
	Node	   *key;
	Node	   *val;
	List	   *args;
	Oid			aggfnoid;
	Oid			aggtype;

	key = transformExprRecurse(pstate, (Node *) agg->arg->key);
	val = transformJsonValueExpr(pstate, "JSON_OBJECTAGG()",
								 agg->arg->value,
								 JS_FORMAT_DEFAULT);
	args = list_make2(key, val);

	returning = transformJsonConstructorOutput(pstate, agg->constructor->output,
											   args);

	if (returning->format->format_type == JS_FORMAT_JSONB)
	{
		if (agg->absent_on_null)
			if (agg->unique)
				aggfnoid = F_JSONB_OBJECT_AGG_UNIQUE_STRICT;
			else
				aggfnoid = F_JSONB_OBJECT_AGG_STRICT;
		else if (agg->unique)
			aggfnoid = F_JSONB_OBJECT_AGG_UNIQUE;
		else
			aggfnoid = F_JSONB_OBJECT_AGG;

		aggtype = JSONBOID;
	}
	else
	{
		if (agg->absent_on_null)
			if (agg->unique)
				aggfnoid = F_JSON_OBJECT_AGG_UNIQUE_STRICT;
			else
				aggfnoid = F_JSON_OBJECT_AGG_STRICT;
		else if (agg->unique)
			aggfnoid = F_JSON_OBJECT_AGG_UNIQUE;
		else
			aggfnoid = F_JSON_OBJECT_AGG;

		aggtype = JSONOID;
	}

	return transformJsonAggConstructor(pstate, agg->constructor, returning,
									   args, aggfnoid, aggtype,
									   JSCTOR_JSON_OBJECTAGG,
									   agg->unique, agg->absent_on_null);
}

/*
 * 转换JSON_ARRAYAGG()聚合函数。         
 *
 * JSON_ARRAYAGG()被转换成类型为JSCTOR_JSON_ARRAYAGG的           
 * JsonConstructorExpr节点，运行时变成     
 * json[b]_object_agg[_unique][_strict](agg->arg)调用，取决于输出     
 * JSON格式。结果强制转换为由agg->constructor->output  
 * 给定的类型。       
 */
static Node *
transformJsonArrayAgg(ParseState *pstate, JsonArrayAgg *agg)
{
	JsonReturning *returning;
	Node	   *arg;
	Oid			aggfnoid;
	Oid			aggtype;

	arg = transformJsonValueExpr(pstate, "JSON_ARRAYAGG()",
								 agg->arg,
								 JS_FORMAT_DEFAULT);

	returning = transformJsonConstructorOutput(pstate, agg->constructor->output,
											   list_make1(arg));

	if (returning->format->format_type == JS_FORMAT_JSONB)
	{
		aggfnoid = agg->absent_on_null ? F_JSONB_AGG_STRICT : F_JSONB_AGG;
		aggtype = JSONBOID;
	}
	else
	{
		aggfnoid = agg->absent_on_null ? F_JSON_AGG_STRICT : F_JSON_AGG;
		aggtype = JSONOID;
	}

	return transformJsonAggConstructor(pstate, agg->constructor, returning,
									   list_make1(arg), aggfnoid, aggtype,
									   JSCTOR_JSON_ARRAYAGG,
									   false, agg->absent_on_null);
}

/*
 * 转换JSON_ARRAY()构造函数。  
 *
 * JSON_ARRAY()被转换成类型为JSCTOR_JSON_ARRAY的              
 * JsonConstructorExpr节点。结果强制转换为由ctor->output  
 * 指定类型。 
 */
static Node *
transformJsonArrayConstructor(ParseState *pstate, JsonArrayConstructor *ctor)
{
	JsonReturning *returning;
	List	   *args = NIL;

	/* 如有元素表达式则进行转换  */
	if (ctor->exprs)
	{
		ListCell   *lc;

		/* 转换并追加元素参数            */
		foreach(lc, ctor->exprs)
		{
			JsonValueExpr *jsval = castNode(JsonValueExpr, lfirst(lc));
			Node	   *val = transformJsonValueExpr(pstate, "JSON_ARRAY()",
													 jsval,
													 JS_FORMAT_DEFAULT);

			args = lappend(args, val);
		}
	}

	returning = transformJsonConstructorOutput(pstate, ctor->output, args);

	return makeJsonConstructorExpr(pstate, JSCTOR_JSON_ARRAY, args, NULL,
								   returning, false, ctor->absent_on_null,
								   ctor->location);
}

static Node *
transformJsonParseArg(ParseState *pstate, Node *jsexpr, JsonFormat *format,
					  Oid *exprtype)
{
	Node	   *raw_expr = transformExprRecurse(pstate, jsexpr);
	Node	   *expr = raw_expr;

	*exprtype = exprType(expr);

	/* 准备输入文档     */
	if (*exprtype == BYTEAOID)
	{
		JsonValueExpr *jve;

		expr = raw_expr;
		expr = makeJsonByteaToTextConversion(expr, format, exprLocation(expr));
		*exprtype = TEXTOID;

		jve = makeJsonValueExpr((Expr *) raw_expr, (Expr *) expr, format);
		expr = (Node *) jve;
	}
	else
	{
		char		typcategory;
		bool		typispreferred;

		get_type_category_preferred(*exprtype, &typcategory, &typispreferred);

		if (*exprtype == UNKNOWNOID || typcategory == TYPCATEGORY_STRING)
		{
			expr = coerce_to_target_type(pstate, (Node *) expr, *exprtype,
										 TEXTOID, -1,
										 COERCION_IMPLICIT,
										 COERCE_IMPLICIT_CAST, -1);
			*exprtype = TEXTOID;
		}

		if (format->encoding != JS_ENC_DEFAULT)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 parser_errposition(pstate, format->location),
					 errmsg("cannot use JSON FORMAT ENCODING clause for non-bytea input types")));
	}

	return expr;
}

/*
 * 转换IS JSON谓词。      
 */
static Node *
transformJsonIsPredicate(ParseState *pstate, JsonIsPredicate *pred)
{
	Oid			exprtype;
	Node	   *expr = transformJsonParseArg(pstate, pred->expr, pred->format,
											 &exprtype);

	/* 生成结果表达式     */
	if (exprtype != TEXTOID && exprtype != JSONOID && exprtype != JSONBOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot use type %s in IS JSON predicate",
						format_type_be(exprtype))));

	/* 此处有意(?)地丢弃格式子句。        */
	return makeJsonIsPredicate(expr, NULL, pred->item_type,
							   pred->unique_keys, pred->location);
}
