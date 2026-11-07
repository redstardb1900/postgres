/*-------------------------------------------------------------------------
 *
 * parse_collate.c
 *		用于分配排序规则信息的例程。   
 *
 * 我们选择把排序规则分析作为表达式解析分析之后的
 * 一个后处理阶段来进行。这是因为这一处理所需的  
 * 状态多于最终树中所需。如果在                         
 * 构建树时即时完成，所有这些状态都将不得           
 * 不永久保存在表达式节点树中。这样一来，            
 * 额外存储只是这个递归例程中的局部变量。 
 *
 * 实际保存在最终树中的信息是：              
 * 1. 每个表达式节点的输出排序规则；若节点返回的  
 * 是非可排序数据类型，则其为 InvalidOid。若结果        
 * 类型可排序但排序规则不确定，为 InvalidOid。  
 * 2. 执行每个函数时所用的排序规则。InvalidOid 表示        
 * 不存在可排序输入，或其排序规则不确定。该值仅      
 * 保存在可能调用使用排序规则函数的节点类型中。     
 *           
 *
 * 你也许认为每个节点只存一个排序规则即可，但这两
 * 个概念确实必须区分开。否则，当一个函数产出的    
 * 是可排序的输出类型却没有可排序输入，或者从可排   
 * 序输入得到不可排序输出时，情况就太令人困惑了。    
 *        
 *
 * 排序规则不确定的情形可能会导致在运行时抛出错误。
 * 若确切知道哪些函数需要排序规则信息，就可以  
 * 改为在解析阶段抛出这些错误。                     
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_collate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_collation.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_collate.h"
#include "utils/lsyscache.h"


/*
 * 排序规则强度（SQL 标准称之为"派生性"）。该顺序      
 * 的选取是为了让比较能有效地工作。注意：标准          
 * 似乎并未区分 NONE 与 CONFLICT。       
 */
typedef enum
{
	COLLATE_NONE,				/* 表达式属于不可排序数据类型   */
	COLLATE_IMPLICIT,			/* 排序规则为隐式派生      */
	COLLATE_CONFLICT,			/* 隐式排序规则之间发生了冲突  */
	COLLATE_EXPLICIT			/* 排序规则为显式派生      */
} CollateStrength;

typedef struct
{
	ParseState *pstate;			/* 解析状态（用于报错）    */
	Oid			collation;		/* 当前排序规则的 OID        */
	CollateStrength strength;	/* 当前排序规则选择的强度    */
	int			location;		/* 设置排序规则的表达式位置*/
	/* 以下字段仅在 strength == COLLATE_CONFLICT 时有效         */
	Oid			collation2;		/* 冲突排序规则的 OID    */
	int			location2;		/* 设置 collation2 的表达式位置 */
} assign_collations_context;

static bool assign_query_collations_walker(Node *node, ParseState *pstate);
static bool assign_collations_walker(Node *node,
									 assign_collations_context *context);
static void merge_collation_state(Oid collation,
								  CollateStrength strength,
								  int location,
								  Oid collation2,
								  int location2,
								  assign_collations_context *context);
static void assign_aggregate_collations(Aggref *aggref,
										assign_collations_context *loccontext);
static void assign_ordered_set_collations(Aggref *aggref,
										  assign_collations_context *loccontext);
static void assign_hypothetical_collations(Aggref *aggref,
										   assign_collations_context *loccontext);


/*
 * assign_query_collations()
 *		为给定 Query 中的所有表达式标记排序规则信息。   
 *
 * 应在表达式解析分析完成后对每个 Query 应用本函数。 
 * 注意我们不会递归进入子 Query，因为它们               
 * 在构建时就应已处理完毕。        
 */
void
assign_query_collations(ParseState *pstate, Query *query)
{
	/*
	 * 我们只用 query_tree_walker() 来访问所有内含的表达式。  
	 * 不过我们可以跳过范围表和 CTE 子查询，因为 RTE 和  
	 * 子查询最好都已经处理完毕（否则引用它们的 Var      
	 * 将无法以正确的排序规则创建）。           
	 */
	(void) query_tree_walker(query,
							 assign_query_collations_walker,
							 (void *) pstate,
							 QTW_IGNORE_RANGE_TABLE |
							 QTW_IGNORE_CTE_SUBQUERIES);
}

/*
 * assign_query_collations 遍历器 
 *
 * query_tree_walker 找到的每个表达式都被独立处理。       
 * 注意 query_tree_walker 可能传给我们整个 List，比如  
 * 目标列表，此时每个子表达式都必须被           
 * 独立处理 --- 我们不希望因两个不同目标项的排序规   
 * 则不同而抱怨。     
 */
static bool
assign_query_collations_walker(Node *node, ParseState *pstate)
{
	/* 对空子表达式无需做任何处理  */
	if (node == NULL)
		return false;

	/*
	 * 我们不想递归进入集合操作树；它已经在                
	 * transformSetOperationStmt 中处理完毕。 
	 */
	if (IsA(node, SetOperationStmt))
		return false;

	if (IsA(node, List))
		assign_list_collations(pstate, (List *) node);
	else
		assign_expr_collations(pstate, node);

	return false;
}

/*
 * assign_list_collations()
 *		为表达式列表中的所有节点标记排序规则信息。      
 *
 * 列表成员表达式被独立处理；它们不必                      
 * 共享同一排序规则。 
 */
void
assign_list_collations(ParseState *pstate, List *exprs)
{
	ListCell   *lc;

	foreach(lc, exprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		assign_expr_collations(pstate, node);
	}
}

/*
 * assign_expr_collations()
 *		为给定表达式树中的所有节点标记排序规则信息。     
 *
 * 导出本函数是为了帮助那些处理表达式但不构建          
 * 完整 Query 的实用命令。应当在调用 transformExpr()            
 * 之后进行，外加任何修改表达式的操作，例如            
 * coerce_to_boolean().
 */
void
assign_expr_collations(ParseState *pstate, Node *expr)
{
	assign_collations_context context;

	/* 为树遍历初始化上下文   */
	context.pstate = pstate;
	context.collation = InvalidOid;
	context.strength = COLLATE_NONE;
	context.location = -1;

	/* 开始遍历   */
	(void) assign_collations_walker(expr, &context);
}

/*
 * select_common_collation()
 *		为一个表达式列表确定共同排序规则。   
 *
 * 这些表达式应当都返回相同数据类型，否则这一操作
 * 意义不大。     
 *
 * none_ok 表示允许返回 InvalidOid，即表示无法                 
 * 确定出共同排序规则，即使对可排序类型亦然。        
 * 否则，遇到隐式排序规则冲突时将抛出错误。      
 *
 * 理论上，none_ok = true 反映 SQL 标准中"数据类型组合         
 * 的结果"条款，none_ok = false 反映"排序规则确定"条款     
 * （某些情况下经由"分组操作"条款调用）。      
 *              
 */
Oid
select_common_collation(ParseState *pstate, List *exprs, bool none_ok)
{
	assign_collations_context context;

	/* 为树遍历初始化上下文   */
	context.pstate = pstate;
	context.collation = InvalidOid;
	context.strength = COLLATE_NONE;
	context.location = -1;

	/* 开始遍历   */
	(void) assign_collations_walker((Node *) exprs, &context);

	/* 处理排序规则冲突     */
	if (context.strength == COLLATE_CONFLICT)
	{
		if (none_ok)
			return InvalidOid;
		ereport(ERROR,
				(errcode(ERRCODE_COLLATION_MISMATCH),
				 errmsg("collation mismatch between implicit collations \"%s\" and \"%s\"",
						get_collation_name(context.collation),
						get_collation_name(context.collation2)),
				 errhint("You can choose the collation by applying the COLLATE clause to one or both expressions."),
				 parser_errposition(context.pstate, context.location2)));
	}

	/*
	 * 注意：若 strength 仍为 COLLATE_NONE，我们将返回          
	 * InvalidOid，但这无妨：这必然意味着所有表达式都 
	 * 非可排序类型。
	 */
	return context.collation;
}

/*
 * assign_collations_walker()
 *		排序规则处理的递归内核。   
 *
 * 无子节点的节点（如 Vars、Consts、Params）在构建时      
 * 就必须已被标记；上层节点在此标记。
 *
 * 注意：若直接以一个 List 调用本函数，它将尝试推断  
 * 所有列表成员的共同排序规则。特别是，若不同成员   
 * 的显式排序规则相互冲突，将抛出错误。                   
 */
static bool
assign_collations_walker(Node *node, assign_collations_context *context)
{
	assign_collations_context loccontext;
	Oid			collation;
	CollateStrength strength;
	int			location;

	/* 对空子表达式无需做任何处理  */
	if (node == NULL)
		return false;

	/*
	 * 准备递归。对多数（虽非全部）节点类型而言，第一 
	 * 件事就是递归处理本节点之下的所有节点。树的每    
	 * 层都各有局部上下文。        
	 */
	loccontext.pstate = context->pstate;
	loccontext.collation = InvalidOid;
	loccontext.strength = COLLATE_NONE;
	loccontext.location = -1;
	/* 设置这些字段只是为了消除未初始化值警告：    */
	loccontext.collation2 = InvalidOid;
	loccontext.location2 = -1;

	/*
	 * 视情况递归，然后确定本节点的排序规则。          
	 *
	 * 注意：一般情形位于 switch 的底部，在若干特例        
	 * 之后。     
	 */
	switch (nodeTag(node))
	{
		case T_CollateExpr:
			{
				/*
				 * COLLATE 会设置一个显式派生的排序规则，无论
				 * 子节点状态如何。但必须递归，以设置    
				 * 此处的排序规则。  
				 */
				CollateExpr *expr = (CollateExpr *) node;

				(void) expression_tree_walker(node,
											  assign_collations_walker,
											  (void *) &loccontext);

				collation = expr->collOid;
				Assert(OidIsValid(collation));
				strength = COLLATE_EXPLICIT;
				location = expr->location;
			}
			break;
		case T_FieldSelect:
			{
				/*
				 * 对 FieldSelect 而言，结果的排序规则就是
				 * 该字段声明的排序规则，与参数中发生的     
				 * 情况无关。（直接参数必为复合类型，故
				 * 必然不可排序。）字段的排序规则早已    
				 * 查询并保存在节点中。  
				 */
				FieldSelect *expr = (FieldSelect *) node;

				/* ... 但先递归       */
				(void) expression_tree_walker(node,
											  assign_collations_walker,
											  (void *) &loccontext);

				if (OidIsValid(expr->resultcollid))
				{
					/* 节点的结果类型可排序。 */
					/* 向上传递字段排序规则作隐式选择。 */
					collation = expr->resultcollid;
					strength = COLLATE_IMPLICIT;
					location = exprLocation(node);
				}
				else
				{
					/* 节点的结果类型不可排序。 */
					collation = InvalidOid;
					strength = COLLATE_NONE;
					location = -1;	/* 不会用到  */
				}
			}
			break;
		case T_RowExpr:
			{
				/*
				 * RowExpr 是一种特例，因为其子表达式         
				 * 相互独立：即便其中一些具有互不兼容的     
				 * 显式排序规则也不报错。
				 */
				RowExpr    *expr = (RowExpr *) node;

				assign_list_collations(context->pstate, expr->args);

				/*
				 * 由于结果总是复合类型，因而永远没有     
				 * 排序规则，我们可以就此停止：本节点对  
				 * 父节点的排序规则无影响。  
				 */
				return false;	/* 毕  */
			}
		case T_RowCompareExpr:
			{
				/*
				 * 对 RowCompare，必须求出每一对输入列        
				 * 的共同排序规则并构建一个列表。若找      
				 * 不到共同排序规则，就把 InvalidOid 放入     
				 * 列表，运行时可能报错，也可能不报。  
				 */
				RowCompareExpr *expr = (RowCompareExpr *) node;
				List	   *colls = NIL;
				ListCell   *l;
				ListCell   *r;

				forboth(l, expr->largs, r, expr->rargs)
				{
					Node	   *le = (Node *) lfirst(l);
					Node	   *re = (Node *) lfirst(r);
					Oid			coll;

					coll = select_common_collation(context->pstate,
												   list_make2(le, re),
												   true);
					colls = lappend_oid(colls, coll);
				}
				expr->inputcollids = colls;

				/*
				 * 由于结果总是布尔类型，因而永远没有       
				 * 排序规则，我们可以就此停止：本节点对其  
				 * 父节点排序规则无碍。 
				 */
				return false;	/* 毕  */
			}
		case T_CoerceToDomain:
			{
				/*
				 * 若域声明里含非默认 COLLATE 指定，就用该  
				 * 排序规则作为强制转换的输出排序规则。  
				 * 否则就让输入排序规则继续                 
				 * 向上冒泡。（输入应当属于域的基础类型，  
				 * 因此不必担心输入在域可排序时         
				 * 自身不可排序。）       
				 */
				CoerceToDomain *expr = (CoerceToDomain *) node;
				Oid			typcollation = get_typcollation(expr->resulttype);

				/* ... 但先递归       */
				(void) expression_tree_walker(node,
											  assign_collations_walker,
											  (void *) &loccontext);

				if (OidIsValid(typcollation))
				{
					/* 节点的结果类型可排序。 */
					if (typcollation == DEFAULT_COLLATION_OID)
					{
						/* 排序规则状态由子节点冒泡。*/
						collation = loccontext.collation;
						strength = loccontext.strength;
						location = loccontext.location;
					}
					else
					{
						/* 将域的排序规则作为隐式选择。    */
						collation = typcollation;
						strength = COLLATE_IMPLICIT;
						location = exprLocation(node);
					}
				}
				else
				{
					/* 节点的结果类型不可排序。 */
					collation = InvalidOid;
					strength = COLLATE_NONE;
					location = -1;	/* 不会用到  */
				}

				/*
				 * 将状态保存进表达式节点。我们知道它 
				 * 不关心输入排序规则。     
				 */
				if (strength == COLLATE_CONFLICT)
					exprSetCollation(node, InvalidOid);
				else
					exprSetCollation(node, collation);
			}
			break;
		case T_TargetEntry:
			(void) expression_tree_walker(node,
										  assign_collations_walker,
										  (void *) &loccontext);

			/*
			 * TargetEntry 只能有一个子节点，并且应将该     
			 * 状态向上冒泡给父节点。下方通用情形代码  
			 * 因 exprType 等对 TargetEntry 不适用而无法使用。  
			 */
			collation = loccontext.collation;
			strength = loccontext.strength;
			location = loccontext.location;

			/*
			 * 若作为排序/分组目标的 TargetEntry 排序规则不    
			 * 确定就抛出错误。我们更愿意现在就做这一      
			 * 检查，而不是让比较函数在运行时失败，因为   
			 * 我们可以给出语法错误指针以帮助定位问题。  
			 * 有些情形下可能不会失败，例如规划器         
			 * 选择使用哈希聚合而非排序来完成分组；        
			 * 但似乎最好还是可预期地抛出错误。           
			 * （对比 transformSetOperationTree，它会对集合操作    
			 * 列的排序规则不确定的情况抛出错误，即使      
			 * 规划器也许能够不用排序就实现该集合     
			 * 操作。）     
			 */
			if (strength == COLLATE_CONFLICT &&
				((TargetEntry *) node)->ressortgroupref != 0)
				ereport(ERROR,
						(errcode(ERRCODE_COLLATION_MISMATCH),
						 errmsg("collation mismatch between implicit collations \"%s\" and \"%s\"",
								get_collation_name(loccontext.collation),
								get_collation_name(loccontext.collation2)),
						 errhint("You can choose the collation by applying the COLLATE clause to one or both expressions."),
						 parser_errposition(context->pstate,
											loccontext.location2)));
			break;
		case T_InferenceElem:
		case T_RangeTblRef:
		case T_JoinExpr:
		case T_FromExpr:
		case T_OnConflictExpr:
		case T_SortGroupClause:
		case T_MergeAction:
			(void) expression_tree_walker(node,
										  assign_collations_walker,
										  (void *) &loccontext);

			/*
			 * 当我们在查询的连接树上被调用时，除了       
			 * 递归穿过连接节点以处理 WHERE/ON 表达式           
			 * 之外，无需对连接节点做任何事，就此停止。 
			 * 排序/分组列表被调用时也无需做事。    
			 */
			return false;
		case T_Query:
			{
				/*
				 * 当我们被调用于一个 SubLink 所属的 Query 时，
				 * 我们假装该 Query 返回其第一个输出列，对   
				 * EXPR_SUBLINK 和 ARRAY_SUBLINK 情形确实如此。     
				 * SubLink 返回布尔时，这条信息将被        
				 * 忽略。特例：在 EXISTS 中，Query 可能不        
				 * 返回任何列，这种情况下我们无需              
				 * 做任何事情。 
				 *
				 * 无需递归，因为该 Query 已被处理过。        
				 */
				Query	   *qtree = (Query *) node;
				TargetEntry *tent;

				if (qtree->targetList == NIL)
					return false;
				tent = linitial_node(TargetEntry, qtree->targetList);
				if (tent->resjunk)
					return false;

				collation = exprCollation((Node *) tent->expr);
				/* 转换为数组后排序规则不变                */
				strength = COLLATE_IMPLICIT;
				location = exprLocation((Node *) tent->expr);
			}
			break;
		case T_List:
			(void) expression_tree_walker(node,
										  assign_collations_walker,
										  (void *) &loccontext);

			/*
			 * 处理列表时，排序规则状态直接从列表         
			 * 元素冒泡。   
			 */
			collation = loccontext.collation;
			strength = loccontext.strength;
			location = loccontext.location;
			break;

		case T_Var:
		case T_Const:
		case T_Param:
		case T_CoerceToDomainValue:
		case T_CaseTestExpr:
		case T_SetToDefault:
		case T_CurrentOfExpr:

			/*
			 * 无子节点表达式节点的一般情形。这些       
			 * 节点应当已被赋予排序规则；到系统表         
			 * 中查找基础情形信息并非本函数的职责。
			 *             
			 */
			collation = exprCollation(node);

			/*
			 * 注意：多数情况下，只要                       
			 * type_is_collatable(exprType(node)) 为真就会有           
			 * 已赋的排序规则；但有一种例外，即 Var 引用    
			 * 某个无法确定唯一排序规则的子查询输出列。
			 * 若对该 Var 应用排序规则敏感函数，就会失败。 
			 *         
			 */

			if (OidIsValid(collation))
				strength = COLLATE_IMPLICIT;
			else
				strength = COLLATE_NONE;
			location = exprLocation(node);
			break;

		default:
			{
				/*
				 * 多数带子节点的表达式节点的一般情形。先  
				 * 递归，然后再确定给本节点赋予什么。  
				 */
				Oid			typcollation;

				/*
				 * 对多数节点类型，我们想一视同仁地   
				 * 对待所有子表达式；但有少数例外，因此  
				 * 设内层分支。
				 */
				switch (nodeTag(node))
				{
					case T_Aggref:
						{
							/*
							 * Aggref 太过繁杂，以至于我们为它   
							 * 单独提供了函数，实际有三个。  
							 * FILTER 子句独立于聚合其余      
							 * 部分，因此可以单独处理。      
							 *            
							 */
							Aggref	   *aggref = (Aggref *) node;

							switch (aggref->aggkind)
							{
								case AGGKIND_NORMAL:
									assign_aggregate_collations(aggref,
																&loccontext);
									break;
								case AGGKIND_ORDERED_SET:
									assign_ordered_set_collations(aggref,
																  &loccontext);
									break;
								case AGGKIND_HYPOTHETICAL:
									assign_hypothetical_collations(aggref,
																   &loccontext);
									break;
								default:
									elog(ERROR, "unrecognized aggkind: %d",
										 (int) aggref->aggkind);
							}

							assign_expr_collations(context->pstate,
												   (Node *) aggref->aggfilter);
						}
						break;
					case T_WindowFunc:
						{
							/*
							 * WindowFunc 只需对它的 aggfilter 子句    
							 * 做特殊处理，与聚合时相同。 
							 */
							WindowFunc *wfunc = (WindowFunc *) node;

							(void) assign_collations_walker((Node *) wfunc->args,
															&loccontext);

							assign_expr_collations(context->pstate,
												   (Node *) wfunc->aggfilter);
						}
						break;
					case T_CaseExpr:
						{
							/*
							 * CaseExpr 是特例，因为我们不想     
							 * 递归进入测试表达式（若有），  
							 * 它已由 transformCaseExpr 标记过       
							 * 排序规则；而且其排序规则与 CASE 
							 * 的结果无关 --- 只有输出表达式     
							 * 才与结果相关。                    
							 */
							CaseExpr   *expr = (CaseExpr *) node;
							ListCell   *lc;

							foreach(lc, expr->args)
							{
								CaseWhen   *when = lfirst_node(CaseWhen, lc);

								/*
								 * 条件表达式也不得影响 CASE 的 
								 * 结果排序规则；但已知它们   
								 * 产生布尔值，因此直接对它们    
								 * 递归是安全的 --- 它们不会改变
								 * loccontext。           
								 */
								(void) assign_collations_walker((Node *) when->expr,
																&loccontext);
								(void) assign_collations_walker((Node *) when->result,
																&loccontext);
							}
							(void) assign_collations_walker((Node *) expr->defresult,
															&loccontext);
						}
						break;
					case T_SubscriptingRef:
						{
							/*
							 * 下标被视为独立的表达式，不  
							 * 会影响节点的排序规则。只有   
							 * 容器本身，以及源表达式（如有） 
							 * 参与其中。（这模拟了旧行为：  
							 * 那时下标必定为整数，因此           
							 * 不会贡献任何东西。）           
							 *                      
							 */
							SubscriptingRef *sbsref = (SubscriptingRef *) node;

							assign_expr_collations(context->pstate,
												   (Node *) sbsref->refupperindexpr);
							assign_expr_collations(context->pstate,
												   (Node *) sbsref->reflowerindexpr);
							(void) assign_collations_walker((Node *) sbsref->refexpr,
															&loccontext);
							(void) assign_collations_walker((Node *) sbsref->refassgnexpr,
															&loccontext);
						}
						break;
					default:

						/*
						 * 一般情形：所有子表达式同等      
						 * 影响 loccontext。  
						 */
						(void) expression_tree_walker(node,
													  assign_collations_walker,
													  (void *) &loccontext);
						break;
				}

				/*
				 * 现在确定要赋予本节点什么排序规则。  
				 */
				typcollation = get_typcollation(exprType(node));
				if (OidIsValid(typcollation))
				{
					/* 节点结果可排序；其输入呢？            */
					if (loccontext.strength > COLLATE_NONE)
					{
						/* 排序规则状态从子节点冒泡。   */
						collation = loccontext.collation;
						strength = loccontext.strength;
						location = loccontext.location;
					}
					else
					{
						/*
						 * 没有可排序输入却产出可排序输出。 
						 * 采用类型的排序规则（通常              
						 * 为 DEFAULT_COLLATION_OID，域可能不同）。  
						 *         
						 */
						collation = typcollation;
						strength = COLLATE_IMPLICIT;
						location = exprLocation(node);
					}
				}
				else
				{
					/* 节点的结果类型不可排序。 */
					collation = InvalidOid;
					strength = COLLATE_NONE;
					location = -1;	/* 不会用到  */
				}

				/*
				 * 把结果排序规则保存进表达式节点。若       
				 * 状态为 COLLATE_CONFLICT，我们会把排序      
				 * 规则设为 InvalidOid，运行时可能报错。    
				 */
				if (strength == COLLATE_CONFLICT)
					exprSetCollation(node, InvalidOid);
				else
					exprSetCollation(node, collation);

				/*
				 * 同样保存输入排序规则，即本节点所        
				 * 调用函数应使用的那一个。        
				 */
				if (loccontext.strength == COLLATE_CONFLICT)
					exprSetInputCollation(node, InvalidOid);
				else
					exprSetInputCollation(node, loccontext.collation);
			}
			break;
	}

	/*
	 * 现在把自身信息并入父节点状态。    
	 */
	merge_collation_state(collation,
						  strength,
						  location,
						  loccontext.collation2,
						  loccontext.location2,
						  context);

	return false;
}

/*
 * 把子表达式的排序规则状态合并进其父节点的上下文。 
 */
static void
merge_collation_state(Oid collation,
					  CollateStrength strength,
					  int location,
					  Oid collation2,
					  int location2,
					  assign_collations_context *context)
{
	/*
	 * 若本节点的排序规则强度不同于                      
	 * *context 中已有者，则本节点与早先兄弟必分高下。     
	 *                  
	 */
	if (strength > context->strength)
	{
		/* 覆盖先前的父状态       */
		context->collation = collation;
		context->strength = strength;
		context->location = location;
		/* 适用时向上冒泡错误信息  */
		if (strength == COLLATE_CONFLICT)
		{
			context->collation2 = collation2;
			context->location2 = location2;
		}
	}
	else if (strength == context->strength)
	{
		/* 合并，或在排序规则冲突时检测错误       */
		switch (strength)
		{
			case COLLATE_NONE:
				/* 无 + 无仍是无                 */
				break;
			case COLLATE_IMPLICIT:
				if (collation != context->collation)
				{
					/*
					 * 非默认的隐式排序规则总是胜过默认。 
					 */
					if (context->collation == DEFAULT_COLLATION_OID)
					{
						/* 覆盖先前的父状态       */
						context->collation = collation;
						context->strength = strength;
						context->location = location;
					}
					else if (collation != DEFAULT_COLLATION_OID)
					{
						/*
						 * 糟糕，出现了冲突。这里无法抛出   
						 * 错误，因为冲突可能被后面的兄弟  
						 * CollateExpr 化解，或者父节点反正        
						 * 不在乎排序规则。返回足够的信息，   
						 * 以便日后需要时抛错。   
						 */
						context->strength = COLLATE_CONFLICT;
						context->collation2 = collation;
						context->location2 = location;
					}
				}
				break;
			case COLLATE_CONFLICT:
				/* 仍处于冲突状态 ...  */
				break;
			case COLLATE_EXPLICIT:
				if (collation != context->collation)
				{
					/*
					 * 糟糕，显式 COLLATE 子句间出现了冲突。  
					 * 这里我们选择立即抛出错误；这正是       
					 * SQL 标准要求的做法，也没有充分理由      
					 * 放宽要求。   
					 */
					ereport(ERROR,
							(errcode(ERRCODE_COLLATION_MISMATCH),
							 errmsg("collation mismatch between explicit collations \"%s\" and \"%s\"",
									get_collation_name(context->collation),
									get_collation_name(collation)),
							 parser_errposition(context->pstate, location)));
				}
				break;
		}
	}
}

/*
 * Aggref 是一种特例，因为仅用于排序的表达式         
 * 不应被视为彼此冲突或与常规参数冲突，              
 * 实际上完全不影响聚合的结果排序规则。          
 * 我们对此的办法是应用 assign_expr_collations()                
 * 而非下传 loccontext。  
 *
 * 注意我们递归到每个 TargetEntry，而不是直接递归到其  
 * 内含表达式，这样上述 T_TargetEntry 情形就会在我们无法
 * 为某个 ORDER BY 项确定排序规则时抱怨（无论是否也是普
 * 通聚合参数）。   
 *
 * 我们无需递归进入 aggorder 或 aggdistinct 列表，              
 * 因为其中只含 SortGroupClause 节点，无需处理。  
 */
static void
assign_aggregate_collations(Aggref *aggref,
							assign_collations_context *loccontext)
{
	ListCell   *lc;

	/* 普通聚合没有直接参数       */
	Assert(aggref->aggdirectargs == NIL);

	/* 处理聚合参数，但把 resjunk 参数区别对待        */
	foreach(lc, aggref->args)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (tle->resjunk)
			assign_expr_collations(loccontext->pstate, (Node *) tle);
		else
			(void) assign_collations_walker((Node *) tle, loccontext);
	}
}

/*
 * 对于有序集聚合，最佳处理方式有些不明确。          
 * 规范定义的逆分布函数只有一个排序列，                   
 * 且不返回可排序类型，但这在一般情形下                   
 * 显然过于受限。我们的方案是：认为聚合的直接           
 * 参数正常参与确定聚合自身的排序规则，               
 * 而聚合的参数仅当聚合被设计为                              
 * 恰好只有一个聚合参数（即仅有一个                          
 * 聚合参数且非变参）时才参与。若它可能有              
 * 多个聚合参数，我们就把聚合参数当作独立              
 * 的排序列来处理。这避免了为类似               
 *		agg(...) within group (order by x collate "foo", y collate "bar")
 * 的情形抛错，同时又保证变参聚合的行为不会             
 * 随特定调用实际拥有的排序列数量而改变。            
 *
 * 其余部分与普通聚合情形非常相似。     
 */
static void
assign_ordered_set_collations(Aggref *aggref,
							  assign_collations_context *loccontext)
{
	bool		merge_sort_collations;
	ListCell   *lc;

	/* 仅当只可能有一个排序规则时才向父合并        */
	merge_sort_collations = (list_length(aggref->args) == 1 &&
							 get_func_variadictype(aggref->aggfnoid) == InvalidOid);

	/* 直接参数（如有）是 Aggref 节点的普通子节点 */
	(void) assign_collations_walker((Node *) aggref->aggdirectargs,
									loccontext);

	/* 以适当方式处理聚合参数     */
	foreach(lc, aggref->args)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (merge_sort_collations)
			(void) assign_collations_walker((Node *) tle, loccontext);
		else
			assign_expr_collations(loccontext->pstate, (Node *) tle);
	}
}

/*
 * 假想集聚合甚至更为特殊：按照规范，我们需要        
 * 统一每一对假设参数与聚合参数的排序规则，          
 * 并且要把排序规则的选择强制下沉到排序列，          
 * 以确保排序确实按照所选定的排序规则进行。          
 * 除此之外，其行为与普通有序集聚合类似。            
 * 请注意，假设性直接参数仅当配对聚合参数参与时 
 * 才计入聚合的排序规则。               
 */
static void
assign_hypothetical_collations(Aggref *aggref,
							   assign_collations_context *loccontext)
{
	ListCell   *h_cell = list_head(aggref->aggdirectargs);
	ListCell   *s_cell = list_head(aggref->args);
	bool		merge_sort_collations;
	int			extra_args;

	/* 仅在只可能有一个排序规则时才向父级合并。  */
	merge_sort_collations = (list_length(aggref->args) == 1 &&
							 get_func_variadictype(aggref->aggfnoid) == InvalidOid);

	/* 处理所有非假设性直接参数     */
	extra_args = list_length(aggref->aggdirectargs) - list_length(aggref->args);
	Assert(extra_args >= 0);
	while (extra_args-- > 0)
	{
		(void) assign_collations_walker((Node *) lfirst(h_cell), loccontext);
		h_cell = lnext(aggref->aggdirectargs, h_cell);
	}

	/* 并行扫描假设性参数与聚合参数             */
	while (h_cell && s_cell)
	{
		Node	   *h_arg = (Node *) lfirst(h_cell);
		TargetEntry *s_tle = (TargetEntry *) lfirst(s_cell);
		assign_collations_context paircontext;

		/*
		 * 先在这对表达式内部为其各自赋予排序规则，  
		 * 然后再为这两者选定共同排序规则。      
		 * 此逻辑应当与 select_common_collation() 一致，         
		 * 但无法原样使用该函数，因为需要访问整个    
		 * 排序规则状态以便冒泡到聚合层。  
		 */
		paircontext.pstate = loccontext->pstate;
		paircontext.collation = InvalidOid;
		paircontext.strength = COLLATE_NONE;
		paircontext.location = -1;
		/* 设置这些字段只是为了消除未初始化值警告：    */
		paircontext.collation2 = InvalidOid;
		paircontext.location2 = -1;

		(void) assign_collations_walker(h_arg, &paircontext);
		(void) assign_collations_walker((Node *) s_tle->expr, &paircontext);

		/* 处理排序规则冲突     */
		if (paircontext.strength == COLLATE_CONFLICT)
			ereport(ERROR,
					(errcode(ERRCODE_COLLATION_MISMATCH),
					 errmsg("collation mismatch between implicit collations \"%s\" and \"%s\"",
							get_collation_name(paircontext.collation),
							get_collation_name(paircontext.collation2)),
					 errhint("You can choose the collation by applying the COLLATE clause to one or both expressions."),
					 parser_errposition(paircontext.pstate,
										paircontext.location2)));

		/*
		 * 此时 paircontext.collation 只可能在类型不可排序时   
		 * 才为 InvalidOid；这种情况下无需做任何事。           
		 * 如果确实需要更改排序列的排序规则，                
		 * 即向排序列 TLE 插入 RelabelType。 
		 *
		 * XXX 这段代码相当粗糙，原因有二：    
		 * assign_collations_walker 本不该像这样修改         
		 * 表达式结构，而且解析期的排序规则变更    
		 * 应当用 CollateExpr 而非 RelabelType 表示（               
		 * 用 RelabelType 标记排序规则本应只是                   
		 * 规划器/执行器阶段的事）。但我们没有             
		 * 更好的替代方案。特别是，注入 CollateExpr    
		 * 可能导致表达式在 dump/reload 之后被                   
		 * 以不同方式解读，因为我们可能实际上       
		 * 把隐式排序规则提升成了显式。此做法依赖     
		 * ruleutils.c 不为 RelabelType 打印 COLLATE 子句，     
		 * 等脆弱行为。
		 */
		if (OidIsValid(paircontext.collation) &&
			paircontext.collation != exprCollation((Node *) s_tle->expr))
		{
			s_tle->expr = (Expr *)
				makeRelabelType(s_tle->expr,
								exprType((Node *) s_tle->expr),
								exprTypmod((Node *) s_tle->expr),
								paircontext.collation,
								COERCE_IMPLICIT_CAST);
		}

		/*
		 * 如适用，就把该列的排序规则状态向上合并到 
		 * 聚合函数。    
		 */
		if (merge_sort_collations)
			merge_collation_state(paircontext.collation,
								  paircontext.strength,
								  paircontext.location,
								  paircontext.collation2,
								  paircontext.location2,
								  loccontext);

		h_cell = lnext(aggref->aggdirectargs, h_cell);
		s_cell = lnext(aggref->args, s_cell);
	}
	Assert(h_cell == NULL && s_cell == NULL);
}
