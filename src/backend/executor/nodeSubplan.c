/*-------------------------------------------------------------------------
 *
 * nodeSubplan.c
 *	  用于支持表达式中出现的子选择的例程     
 *
 * 本模块负责执行 SubPlan 表达式节点，它不应与           
 * FROM 中出现的子选择相混淆。SubPlan 可分为两类：        
 * 其中"initplans"（初始化计划）每条查询只需                   
 * 求值一次（此外还有其他限制，例如它们不会          
 * 使用来自父计划层的直接相关变量），而                 
 * "regular"（常规）子计划每当需要其结果时重新求值。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSubplan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 接口例程      
 *		ExecSubPlan  - 处理子选择    
 *		ExecInitSubPlan - 初始化子选择    
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "executor/executor.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

static Datum ExecHashSubPlan(SubPlanState *node,
							 ExprContext *econtext,
							 bool *isNull);
static Datum ExecScanSubPlan(SubPlanState *node,
							 ExprContext *econtext,
							 bool *isNull);
static void buildSubPlanHash(SubPlanState *node, ExprContext *econtext);
static bool findPartialMatch(TupleHashTable hashtable, TupleTableSlot *slot,
							 FmgrInfo *eqfunctions);
static bool slotAllNulls(TupleTableSlot *slot);
static bool slotNoNulls(TupleTableSlot *slot);


/* ----------------------------------------------------------------
 *		ExecSubPlan
 *
 * 这是执行常规 SubPlan 的主入口。                      
 * ----------------------------------------------------------------
 */
Datum
ExecSubPlan(SubPlanState *node,
			ExprContext *econtext,
			bool *isNull)
{
	SubPlan    *subplan = node->subplan;
	EState	   *estate = node->planstate->state;
	ScanDirection dir = estate->es_direction;
	Datum		retval;

	CHECK_FOR_INTERRUPTS();

	/* 默认设为非空      */
	*isNull = false;

	/* 预防检查  */
	if (subplan->subLinkType == CTE_SUBLINK)
		elog(ERROR, "CTE subplans should not be executed via ExecSubPlan");
	if (subplan->setParam != NIL && subplan->subLinkType != MULTIEXPR_SUBLINK)
		elog(ERROR, "cannot set parent params from subquery");

	/* 求值时强制采用前向扫描模式*/
	estate->es_direction = ForwardScanDirection;

	/* 选择合适的求值策略            */
	if (subplan->useHashTable)
		retval = ExecHashSubPlan(node, econtext, isNull);
	else
		retval = ExecScanSubPlan(node, econtext, isNull);

	/* 恢复扫描方向     */
	estate->es_direction = dir;

	return retval;
}

/*
 * ExecHashSubPlan: 将子选择结果存入内存哈希表          
 */
static Datum
ExecHashSubPlan(SubPlanState *node,
				ExprContext *econtext,
				bool *isNull)
{
	bool		result = false;
	SubPlan    *subplan = node->subplan;
	PlanState  *planstate = node->planstate;
	TupleTableSlot *slot;

	/* 不应有任何直接相关变量          */
	if (subplan->parParam != NIL || node->args != NIL)
		elog(ERROR, "hashed subplan with direct correlation not supported");

	/*
	 * 如果是第一次，或需要重新扫描子计划，就构建哈希 
	 * 表。
	 */
	if (node->hashtable == NULL || planstate->chgParam != NULL)
		buildSubPlanHash(node, econtext);

	/*
	 * 空子计划的结果总是 FALSE；无需再求值左              
	 * 表达式。  
	 */
	*isNull = false;
	if (!node->havehashrows && !node->havenullrows)
		return BoolGetDatum(false);

	/*
	 * 求值左侧表达式并构成投影元组。我们必须          
	 * 先设置好 econtext（别扭做法！）。 
	 */
	node->projLeft->pi_exprContext = econtext;
	slot = ExecProject(node->projLeft);

	/*
	 * 若 LHS 全为非空，就在主哈希表中探测精确匹配；    
	 * 若找到，结果即为 TRUE。否则，扫描部分 NULL     
	 * 的表，看看是否有行无法被证明与 LHS 不相等；     
	 * 若有，结果为 UNKNOWN。（如果我们并不关心             
	 * UNKNOWN，则可跳过这一步。）否则，结果为 FALSE。
	 *
	 * 注：我们之所以能避免对主哈希表做全表扫描，    
	 * 是因为假定组合操作符在两个输入均为非空时          
	 * 绝不会返回 NULL。如果它们会这样，我们可能          
	 * 就需要产生 UNKNOWN 而非 FALSE，因为在将 LHS               
	 * 与主表某项比较时出现了 UNKNOWN 结果 --- 除非哈         
	 * 键偶然相撞，否则甚至不会做比较。 
	 */
	if (slotNoNulls(slot))
	{
		if (node->havehashrows &&
			FindTupleHashEntry(node->hashtable,
							   slot,
							   node->cur_eq_comp,
							   node->lhs_hash_funcs) != NULL)
			result = true;
		else if (node->havenullrows &&
				 findPartialMatch(node->hashnulls, slot, node->cur_eq_funcs))
			*isNull = true;
	}

	/*
	 * 当 LHS 部分或全部为 NULL 时，绝不可能返回 TRUE。      
	 * 如果不关心 UNKNOWN，就直接返回 FALSE。否则，           
	 * 若 LHS 全为 NULL，则立即返回 UNKNOWN。（由于     
	 * 组合操作符是严格的，结果只有在子选择为空时       
	 * 才可能是 FALSE，但我们已经处理过该情况。）否则    
	 * 我们必须扫描主表和部分为 NULL 的表，看看是否有行 
	 * 无法被证明与 LHS 不相等；若有，结果为 UNKNOWN。 
	 * 否则，结果为 FALSE。    
	 */
	else if (node->hashnulls == NULL)
		 /* 直接返回 FALSE*/ ;
	else if (slotAllNulls(slot))
		*isNull = true;
	/* 先扫描部分为 NULL 的表，因为更有可能找到匹配*/
	else if (node->havenullrows &&
			 findPartialMatch(node->hashnulls, slot, node->cur_eq_funcs))
		*isNull = true;
	else if (node->havehashrows &&
			 findPartialMatch(node->hashtable, slot, node->cur_eq_funcs))
		*isNull = true;

	/*
	 * 注：由于我们通常在每元组上下文中被调用，         
	 * 因此必须在返回前显式清除投影出的元组。           
	 * 否则会出现二次释放：每元组上下文很可能              
	 * 在我们再次被调用前就被重置，然后元组槽             
	 * 会认为仍需释放该元组。
	 */
	ExecClearTuple(slot);

	/* 每次哈希表查找后还必须重置 hashtempcxt。       */
	MemoryContextReset(node->hashtempcxt);

	return BoolGetDatum(result);
}

/*
 * ExecScanSubPlan: 每次都必须重新扫描子计划的默认情形   
 */
static Datum
ExecScanSubPlan(SubPlanState *node,
				ExprContext *econtext,
				bool *isNull)
{
	SubPlan    *subplan = node->subplan;
	PlanState  *planstate = node->planstate;
	SubLinkType subLinkType = subplan->subLinkType;
	MemoryContext oldcontext;
	TupleTableSlot *slot;
	Datum		result;
	bool		found = false;	/* 得到至少一个子计划元组为真*/
	ListCell   *pvar;
	ListCell   *l;
	ArrayBuildStateAny *astate = NULL;

	/* 在调用者上下文中按需初始化 ArrayBuildStateAny   */
	if (subLinkType == ARRAY_SUBLINK)
		astate = initArrayResultAny(subplan->firstColType,
									CurrentMemoryContext, true);

	/*
	 * 我们很可能正处于短暂的表达式求值上下文中。       
	 * 为操纵子计划的 chgParam，应切换到每查询上下文，  
	 * 对其调用 ExecProcNode 等。
	 */
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

	/*
	 * 用父计划的相关性值设置本计划的参数。（任何  
	 * 所需的计算都在父 econtext 中完成，因为 Param         
	 * 值并不需要具有每查询的生命周期。） 
	 */
	Assert(list_length(subplan->parParam) == list_length(node->args));

	forboth(l, subplan->parParam, pvar, node->args)
	{
		int			paramid = lfirst_int(l);
		ParamExecData *prm = &(econtext->ecxt_param_exec_vals[paramid]);

		prm->value = ExecEvalExprSwitchContext((ExprState *) lfirst(pvar),
											   econtext,
											   &(prm->isnull));
		planstate->chgParam = bms_add_member(planstate->chgParam, paramid);
	}

	/*
	 * 参数已设置好，现在可以重置子计划了。         
	 */
	ExecReScan(planstate);

	/*
	 * 对于除 EXPR_SUBLINK 和 ARRAY_SUBLINK 之外的所有               
	 * sublink 类型，结果为布尔值，组合操作符结果亦然。
	 * 我们在多个元组间组合结果（如果子计划产出多于    
	 * 一个）时，对 ANY_SUBLINK 使用 OR 语义，对      
	 * ALL_SUBLINK 则使用 AND 语义。（ROWCOMPARE_SUBLINK            
	 * 不允许子计划返回多个元组。）来自组合操作符的    
	 * NULL 结果按 OR 和 AND 的常规 SQL 语义处理。无输入元组
	 * ANY_SUBLINK 为 FALSE, ALL_SUBLINK 为 TRUE, NULL： 
	 * ROWCOMPARE_SUBLINK 
	 *
	 * 对于 EXPR_SUBLINK，要求子计划最多产出一个元组，   
	 * 否则就会报错。如果产出零个元组，我们返回 NULL。   
	 * 若得到了元组，我们只用其第一列（该情形下只有一  
	 * 个非垃圾列）。                     
	 *
	 * 对于 MULTIEXPR_SUBLINK，我们把按列的子计划输出        
	 * 推送到 setParams，然后返回一个哑假值。子计划不得  
	 * 返回多个元组；如果产出零个元组，则把 setParams       
	 * 设为 NULL。            
	 *
	 * 对于 ARRAY_SUBLINK，我们允许子计划产出任意数量         
	 * 的元组，将第一列的值构成一个数组。特别要注意 
	 * 的是，若无元组产出，我们会产出一个零元素数组     
	 * （这与 8.3 之前返回 NULL 的行为不同）
	 */
	result = BoolGetDatum(subLinkType == ALL_SUBLINK);
	*isNull = false;

	for (slot = ExecProcNode(planstate);
		 !TupIsNull(slot);
		 slot = ExecProcNode(planstate))
	{
		TupleDesc	tdesc = slot->tts_tupleDescriptor;
		Datum		rowresult;
		bool		rownull;
		int			col;
		ListCell   *plst;

		if (subLinkType == EXISTS_SUBLINK)
		{
			found = true;
			result = BoolGetDatum(true);
			break;
		}

		if (subLinkType == EXPR_SUBLINK)
		{
			/* EXPR sublink 不允许多个输入元组            */
			if (found)
				ereport(ERROR,
						(errcode(ERRCODE_CARDINALITY_VIOLATION),
						 errmsg("more than one row returned by a subquery used as an expression")));
			found = true;

			/*
			 * 须复制子计划的元组，以防结果是引用传递类
			 * 型 --- 我们的返回值将指向拷贝元组！无法  
			 * 使用子计划的那份元组实例，因为它在下一次
			 * ExecProcNode() 调用之后就不再有效。                
			 * node->curTuple 会跟踪这份拷贝元组以便最终     
			 * 释放  
			 */
			if (node->curTuple)
				heap_freetuple(node->curTuple);
			node->curTuple = ExecCopySlotHeapTuple(slot);

			result = heap_getattr(node->curTuple, 1, tdesc, isNull);
			/* 继续扫描子计划以确保只有一个元组          */
			continue;
		}

		if (subLinkType == MULTIEXPR_SUBLINK)
		{
			/* MULTIEXPR sublink 不允许多个输入元组            */
			if (found)
				ereport(ERROR,
						(errcode(ERRCODE_CARDINALITY_VIOLATION),
						 errmsg("more than one row returned by a subquery used as an expression")));
			found = true;

			/*
			 * 需复制子计划的元组，以防结果为引用传递类
			 * 型 --- 我们的输出值将指向这份拷贝元组！无
			 * 法使用子计划的那份元组，因为它在下一次   
			 * ExecProcNode() 调用后就不再有效。node->curTuple     
			 * 会跟踪这份拷贝元组以便最终                    
			 * 释放  
			 */
			if (node->curTuple)
				heap_freetuple(node->curTuple);
			node->curTuple = ExecCopySlotHeapTuple(slot);

			/*
			 * 现在从元组的各列设置所有 setParam 参数         
			 */
			col = 1;
			foreach(plst, subplan->setParam)
			{
				int			paramid = lfirst_int(plst);
				ParamExecData *prmdata;

				prmdata = &(econtext->ecxt_param_exec_vals[paramid]);
				Assert(prmdata->execPlan == NULL);
				prmdata->value = heap_getattr(node->curTuple, col, tdesc,
											  &(prmdata->isnull));
				col++;
			}

			/* 继续扫描子计划以确保只有一个元组          */
			continue;
		}

		if (subLinkType == ARRAY_SUBLINK)
		{
			Datum		dvalue;
			bool		disnull;

			found = true;
			/* 把当前值暂存起来 */
			Assert(subplan->firstColType == TupleDescAttr(tdesc, 0)->atttypid);
			dvalue = slot_getattr(slot, 1, &disnull);
			astate = accumArrayResultAny(astate, dvalue, disnull,
										 subplan->firstColType, oldcontext);
			/* 继续扫描子计划以收集所有值     */
			continue;
		}

		/* ROWCOMPARE sublink 同样不允许多个输入元组             */
		if (subLinkType == ROWCOMPARE_SUBLINK && found)
			ereport(ERROR,
					(errcode(ERRCODE_CARDINALITY_VIOLATION),
					 errmsg("more than one row returned by a subquery used as an expression")));

		found = true;

		/*
		 * 对于 ALL、ANY 和 ROWCOMPARE sublink，装载代表   
		 * 子选择各列的 Params，然后求值                         
		 * 组合表达式。   
		 */
		col = 1;
		foreach(plst, subplan->paramIds)
		{
			int			paramid = lfirst_int(plst);
			ParamExecData *prmdata;

			prmdata = &(econtext->ecxt_param_exec_vals[paramid]);
			Assert(prmdata->execPlan == NULL);
			prmdata->value = slot_getattr(slot, col, &(prmdata->isnull));
			col++;
		}

		rowresult = ExecEvalExprSwitchContext(node->testexpr, econtext,
											  &rownull);

		if (subLinkType == ANY_SUBLINK)
		{
			/* 按 OR 语义跨行组合            */
			if (rownull)
				*isNull = true;
			else if (DatumGetBool(rowresult))
			{
				result = BoolGetDatum(true);
				*isNull = false;
				break;			/* 无需再查看更多行      */
			}
		}
		else if (subLinkType == ALL_SUBLINK)
		{
			/* 按 AND 语义跨行组合            */
			if (rownull)
				*isNull = true;
			else if (!DatumGetBool(rowresult))
			{
				result = BoolGetDatum(false);
				*isNull = false;
				break;			/* 无需再查看更多行      */
			}
		}
		else
		{
			/* 必为 ROWCOMPARE_SUBLINK  */
			result = rowresult;
			*isNull = rownull;
		}
	}

	MemoryContextSwitchTo(oldcontext);

	if (subLinkType == ARRAY_SUBLINK)
	{
		/* 结果将在调用者的上下文中返回   */
		result = makeArrayResultAny(astate, oldcontext, true);
	}
	else if (!found)
	{
		/*
		 * 处理空子计划结果。result/isNull 此前已为除 EXPR 
		 * 和 ROWCOMPARE 之外的所有 sublink 类型正确做了  
		 * 初始化；这两类返回 NULL。
		 */
		if (subLinkType == EXPR_SUBLINK ||
			subLinkType == ROWCOMPARE_SUBLINK)
		{
			result = (Datum) 0;
			*isNull = true;
		}
		else if (subLinkType == MULTIEXPR_SUBLINK)
		{
			/* 我们不关心函数结果，但要设置 setParams       */
			foreach(l, subplan->setParam)
			{
				int			paramid = lfirst_int(l);
				ParamExecData *prmdata;

				prmdata = &(econtext->ecxt_param_exec_vals[paramid]);
				Assert(prmdata->execPlan == NULL);
				prmdata->value = (Datum) 0;
				prmdata->isnull = true;
			}
		}
	}

	return result;
}

/*
 * buildSubPlanHash: 扫描子计划输出以装载哈希表。 
 */
static void
buildSubPlanHash(SubPlanState *node, ExprContext *econtext)
{
	SubPlan    *subplan = node->subplan;
	PlanState  *planstate = node->planstate;
	int			ncols = node->numCols;
	ExprContext *innerecontext = node->innerecontext;
	MemoryContext oldcontext;
	long		nbuckets;
	TupleTableSlot *slot;

	Assert(subplan->subLinkType == ANY_SUBLINK);

	/*
	 * 如果我们已经有哈希表，就重置它们；否则创建空  
	 * 哈希表。  
	 *
	 * 如果我们需要在 FALSE 和 UNKNOWN（即 NULL）两种          
	 * 结果之间精确区分 IN 运算的结果，就必须存储子计    
	 * 划输出中部分或全部为 NULL 的行。我们把这样的行    
	 * 存放在一个单独的哈希表中，预期它比主表小得多。  
	 * （我们可以用哈希去除重复的部分为 NULL 行。           
	 * 单独存放，是为了减少不可避免的全表搜索开销，  
	 * 见 findPartialMatch。）      
	 *
	 * 如果不需要区分 FALSE 和 UNKNOWN，那么就不需要         
	 * 存储包含 NULL 的子计划输出行。          
	 */
	MemoryContextReset(node->hashtablecxt);
	node->havehashrows = false;
	node->havenullrows = false;

	nbuckets = clamp_cardinality_to_long(planstate->plan->plan_rows);
	if (nbuckets < 1)
		nbuckets = 1;

	if (node->hashtable)
		ResetTupleHashTable(node->hashtable);
	else
		node->hashtable = BuildTupleHashTableExt(node->parent,
												 node->descRight,
												 ncols,
												 node->keyColIdx,
												 node->tab_eq_funcoids,
												 node->tab_hash_funcs,
												 node->tab_collations,
												 nbuckets,
												 0,
												 node->planstate->state->es_query_cxt,
												 node->hashtablecxt,
												 node->hashtempcxt,
												 false);

	if (!subplan->unknownEqFalse)
	{
		if (ncols == 1)
			nbuckets = 1;		/* 只可能有一个条目    */
		else
		{
			nbuckets /= 16;
			if (nbuckets < 1)
				nbuckets = 1;
		}

		if (node->hashnulls)
			ResetTupleHashTable(node->hashnulls);
		else
			node->hashnulls = BuildTupleHashTableExt(node->parent,
													 node->descRight,
													 ncols,
													 node->keyColIdx,
													 node->tab_eq_funcoids,
													 node->tab_hash_funcs,
													 node->tab_collations,
													 nbuckets,
													 0,
													 node->planstate->state->es_query_cxt,
													 node->hashtablecxt,
													 node->hashtempcxt,
													 false);
	}
	else
		node->hashnulls = NULL;

	/*
	 * 我们很可能正处于短暂的表达式求值上下文中。       
	 * 应切换到每查询上下文来操纵子计划。      
	 */
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

	/*
	 * 从头重置子计划  
	 */
	ExecReScan(planstate);

	/*
	 * 扫描子计划并装载哈希表。注意当子选择输出重复    
	 * 行时，只会存储其中一份拷贝。                           
	 */
	for (slot = ExecProcNode(planstate);
		 !TupIsNull(slot);
		 slot = ExecProcNode(planstate))
	{
		int			col = 1;
		ListCell   *plst;
		bool		isnew;

		/*
		 * 装载代表子选择原始输出的 Params，再构成存入   
		 * 哈希表的投影元组。                         
		 */
		foreach(plst, subplan->paramIds)
		{
			int			paramid = lfirst_int(plst);
			ParamExecData *prmdata;

			prmdata = &(innerecontext->ecxt_param_exec_vals[paramid]);
			Assert(prmdata->execPlan == NULL);
			prmdata->value = slot_getattr(slot, col,
										  &(prmdata->isnull));
			col++;
		}
		slot = ExecProject(node->projRight);

		/*
		 * 若结果中含 NULL，则单独存储，或者不存储。  
		 */
		if (slotNoNulls(slot))
		{
			(void) LookupTupleHashEntry(node->hashtable, slot, &isnew, NULL);
			node->havehashrows = true;
		}
		else if (node->hashnulls)
		{
			(void) LookupTupleHashEntry(node->hashnulls, slot, &isnew, NULL);
			node->havenullrows = true;
		}

		/*
		 * 在每条内部元组后重置 innerecontext 以释放 ExecProject
		 * 期间所用内存 
		 */
		ResetExprContext(innerecontext);

		/* 每次哈希表查找后还必须重置 hashtempcxt。       */
		MemoryContextReset(node->hashtempcxt);
	}

	/*
	 * 由于投影出的元组位于子查询的上下文而非主上下   
	 * 文中，最好于子查询上下文可能被重置之前，     
	 * 就先清除元组槽。否则就有可能发生二次释放        
	 * （XXX 可能已不再需要，不过也                             
	 * 没坏处。） 
	 */
	ExecClearTuple(node->projRight->pi_state.resultslot);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * execTuplesUnequal
 *		如果两个元组在指定字段上必定不相等，则返回  
 *		真。 
 *
 * 空值与任何其他值相比，谈不上相等或不等。        
 * 仅当存在比较不等的非空字段时才能得到真结果。   
 *
 * slot1, slot2: 待比较的元组（列必须相同！）     
 * numCols: 要检查的属性数量               
 * matchColIdx: 属性列号数组               
 * eqFunctions: 要使用的相等函数的 fmgr 查找信息数组        
 * evalContext: 用于执行这些函数的短期内存上下文     
 */
static bool
execTuplesUnequal(TupleTableSlot *slot1,
				  TupleTableSlot *slot2,
				  int numCols,
				  AttrNumber *matchColIdx,
				  FmgrInfo *eqfunctions,
				  const Oid *collations,
				  MemoryContext evalContext)
{
	MemoryContext oldContext;
	bool		result;
	int			i;

	/* 重置并切换到临时上下文。    */
	MemoryContextReset(evalContext);
	oldContext = MemoryContextSwitchTo(evalContext);

	/*
	 * 我们无法在不检查全部字段的情况下报告匹配，但  
	 * 只要发现不相等的字段就可以立即报告不匹配。 
	 * 因此，从最后一个字段（最不重要的排序键）开始  
	 * 比较；如果输入是已排序的，它最有可能不同。 
	 */
	result = false;

	for (i = numCols; --i >= 0;)
	{
		AttrNumber	att = matchColIdx[i];
		Datum		attr1,
					attr2;
		bool		isNull1,
					isNull2;

		attr1 = slot_getattr(slot1, att, &isNull1);

		if (isNull1)
			continue;			/* 无法证明任何事情  */

		attr2 = slot_getattr(slot2, att, &isNull2);

		if (isNull2)
			continue;			/* 无法证明任何事情  */

		/* 应用该类型专用的相等函数      */
		if (!DatumGetBool(FunctionCall2Coll(&eqfunctions[i],
											collations[i],
											attr1, attr2)))
		{
			result = true;		/* 两者不相等  */
			break;
		}
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}

/*
 * findPartialMatch: 哈希表中是否包含与元组并非        
 * 可证明为不同的条目？   
 *
 * 我们必须扫描整个哈希表；用哈希键来引导探测    
 * 并无用处，因为对于一个元组，我们可能得到与    
 * 给定元组的哈希键大不相同的部分匹配。         
 *
 * 调用者必须提供要使用的相等函数，因为在跨类型    
 * 的情况下，它们与哈希表内部的函数并不相同。   
 */
static bool
findPartialMatch(TupleHashTable hashtable, TupleTableSlot *slot,
				 FmgrInfo *eqfunctions)
{
	int			numCols = hashtable->numCols;
	AttrNumber *keyColIdx = hashtable->keyColIdx;
	TupleHashIterator hashiter;
	TupleHashEntry entry;

	InitTupleHashIterator(hashtable, &hashiter);
	while ((entry = ScanTupleHashTable(hashtable, &hashiter)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();

		ExecStoreMinimalTuple(entry->firstTuple, hashtable->tableslot, false);
		if (!execTuplesUnequal(slot, hashtable->tableslot,
							   numCols, keyColIdx,
							   eqfunctions,
							   hashtable->tab_collations,
							   hashtable->tempcxt))
		{
			TermTupleHashIterator(&hashiter);
			return true;
		}
	}
	/* 此处无需调用 TermTupleHashIterator  */
	return false;
}

/*
 * slotAllNulls: slot 是否全为 NULL？   
 *
 * 该函数不检查已删除的列，这没有问题，因为我们 
 * 只用于投影元组。   
 */
static bool
slotAllNulls(TupleTableSlot *slot)
{
	int			ncols = slot->tts_tupleDescriptor->natts;
	int			i;

	for (i = 1; i <= ncols; i++)
	{
		if (!slot_attisnull(slot, i))
			return false;
	}
	return true;
}

/*
 * slotNoNulls: slot 是否全不为 NULL？  
 *
 * 该函数不检查已删除的列，这没有问题，因为我们 
 * 只用于投影元组。   
 */
static bool
slotNoNulls(TupleTableSlot *slot)
{
	int			ncols = slot->tts_tupleDescriptor->natts;
	int			i;

	for (i = 1; i <= ncols; i++)
	{
		if (slot_attisnull(slot, i))
			return false;
	}
	return true;
}

/* ----------------------------------------------------------------
 *		ExecInitSubPlan
 *
 * 为一个 SubPlan 创建 SubPlanState；这是 ExecInitExpr()         
 * 中属于 SubPlan 的部分。我们把它分离出来，这样它      
 * 既用于常规 SubPlan 也用于 InitPlan。注意我们不会       
 * 把 SubPlan 链入父节点的 subPlan 列表（对 InitPlan           
 * 不应发生）；ExecInitExpr() 负责。 
 * ----------------------------------------------------------------
 */
SubPlanState *
ExecInitSubPlan(SubPlan *subplan, PlanState *parent)
{
	SubPlanState *sstate = makeNode(SubPlanState);
	EState	   *estate = parent->state;

	sstate->subplan = subplan;

	/* 把 SubPlanState 链接到已初始化的子计划   */
	sstate->planstate = (PlanState *) list_nth(estate->es_subplanstates,
											   subplan->plan_id - 1);

	/*
	 * 如果规划器误将并行不安全的子计划放入并行化的  
	 * 子查询，该检查就会失败；见 ExecSerializePlan   
	 */
	if (sstate->planstate == NULL)
		elog(ERROR, "subplan \"%s\" was not initialized",
			 subplan->plan_name);

	/* 也链接到父节点状态 */
	sstate->parent = parent;

	/* 初始化子表达式     */
	sstate->testexpr = ExecInitExpr((Expr *) subplan->testexpr, parent);
	sstate->args = ExecInitExprList(subplan->args, parent);

	/*
	 * 初始化本状态 
	 */
	sstate->curTuple = NULL;
	sstate->curArray = PointerGetDatum(NULL);
	sstate->projLeft = NULL;
	sstate->projRight = NULL;
	sstate->hashtable = NULL;
	sstate->hashnulls = NULL;
	sstate->hashtablecxt = NULL;
	sstate->hashtempcxt = NULL;
	sstate->innerecontext = NULL;
	sstate->keyColIdx = NULL;
	sstate->tab_eq_funcoids = NULL;
	sstate->tab_hash_funcs = NULL;
	sstate->tab_eq_funcs = NULL;
	sstate->tab_collations = NULL;
	sstate->lhs_hash_funcs = NULL;
	sstate->cur_eq_funcs = NULL;

	/*
	 * 如果这是一个 initplan，它有一些父计划会使用的输   
	 * 出参数，所以把这些参数标记为需要求值。我们    
	 * 直到首次需要其某个输出，才真正运行子计划。 
	 *
	 * CTE 子计划的输出参数绝不以常规方式求值，             
	 * 此情况下跳过该步骤。 
	 *
	 * 注意我们在这里不设置 parent->chgParam：父计划         
	 * 尚未运行，无需强制它重新运行。  
	 */
	if (subplan->setParam != NIL && subplan->parParam == NIL &&
		subplan->subLinkType != CTE_SUBLINK)
	{
		ListCell   *lst;

		foreach(lst, subplan->setParam)
		{
			int			paramid = lfirst_int(lst);
			ParamExecData *prm = &(estate->es_param_exec_vals[paramid]);

			prm->execPlan = sstate;
		}
	}

	/*
	 * 如果打算对子查询输出做哈希，就初始化相关内容。  
	 * （不过哈希表要到需要时才创建。）     
	 */
	if (subplan->useHashTable)
	{
		int			ncols,
					i;
		TupleDesc	tupDescLeft;
		TupleDesc	tupDescRight;
		Oid		   *cross_eq_funcoids;
		TupleTableSlot *slot;
		List	   *oplist,
				   *lefttlist,
				   *righttlist;
		ListCell   *l;

		/* 我们需要内存上下文来存放哈希表      */
		sstate->hashtablecxt =
			AllocSetContextCreate(CurrentMemoryContext,
								  "Subplan HashTable Context",
								  ALLOCSET_DEFAULT_SIZES);
		/* 以及一个小上下文供哈希表用作临时存储     */
		sstate->hashtempcxt =
			AllocSetContextCreate(CurrentMemoryContext,
								  "Subplan HashTable Temp Context",
								  ALLOCSET_SMALL_SIZES);
		/* 以及一个函数求值用短暂表达式上下文   */
		sstate->innerecontext = CreateExprContext(estate);

		/*
		 * 我们用 ExecProject 求值左右两侧表达式列表  
		 * 并构成元组。（也许认为可以直接用子选择的输  
		 * 出元组，但如果必须插入对子选择输出数据类型 
		 * 的运行时强制转换，情况就并非如此；而且这样    
		 * 做还能避免存储子选择输出中可能存在的 resjunk  
		 * 列。）遍历各组合表达式，为左侧和           
		 * 右侧构建对应的                                     
		 * tlist。        
		 *
		 * 我们还提取组合操作符本身用来初始化哈希表的 
		 * 相等函数和哈希函数。                         
		 */
		if (IsA(subplan->testexpr, OpExpr))
		{
			/* 单一组合操作符     */
			oplist = list_make1(subplan->testexpr);
		}
		else if (is_andclause(subplan->testexpr))
		{
			/* 多个组合操作符        */
			oplist = castNode(BoolExpr, subplan->testexpr)->args;
		}
		else
		{
			/* 可哈希子计划中不应出现其他情况     */
			elog(ERROR, "unrecognized testexpr type: %d",
				 (int) nodeTag(subplan->testexpr));
			oplist = NIL;		/* 让编译器安静  */
		}
		ncols = list_length(oplist);

		lefttlist = righttlist = NIL;
		sstate->numCols = ncols;
		sstate->keyColIdx = (AttrNumber *) palloc(ncols * sizeof(AttrNumber));
		sstate->tab_eq_funcoids = (Oid *) palloc(ncols * sizeof(Oid));
		sstate->tab_collations = (Oid *) palloc(ncols * sizeof(Oid));
		sstate->tab_hash_funcs = (FmgrInfo *) palloc(ncols * sizeof(FmgrInfo));
		sstate->tab_eq_funcs = (FmgrInfo *) palloc(ncols * sizeof(FmgrInfo));
		sstate->lhs_hash_funcs = (FmgrInfo *) palloc(ncols * sizeof(FmgrInfo));
		sstate->cur_eq_funcs = (FmgrInfo *) palloc(ncols * sizeof(FmgrInfo));
		/* 稍后需要跨类型相等函数，但 sstate 中不需要     */
		cross_eq_funcoids = (Oid *) palloc(ncols * sizeof(Oid));

		i = 1;
		foreach(l, oplist)
		{
			OpExpr	   *opexpr = lfirst_node(OpExpr, l);
			Expr	   *expr;
			TargetEntry *tle;
			Oid			rhs_eq_oper;
			Oid			left_hashfn;
			Oid			right_hashfn;

			Assert(list_length(opexpr->args) == 2);

			/* 处理左侧参数        */
			expr = (Expr *) linitial(opexpr->args);
			tle = makeTargetEntry(expr,
								  i,
								  NULL,
								  false);
			lefttlist = lappend(lefttlist, tle);

			/* 处理右侧参数         */
			expr = (Expr *) lsecond(opexpr->args);
			tle = makeTargetEntry(expr,
								  i,
								  NULL,
								  false);
			righttlist = lappend(righttlist, tle);

			/* 查找相等函数（可能是跨类型的）         */
			cross_eq_funcoids[i - 1] = opexpr->opfuncid;
			fmgr_info(opexpr->opfuncid, &sstate->cur_eq_funcs[i - 1]);
			fmgr_info_set_expr((Node *) opexpr, &sstate->cur_eq_funcs[i - 1]);

			/* 查找适用于 RHS 类型的相等函数      */
			if (!get_compatible_hash_operators(opexpr->opno,
											   NULL, &rhs_eq_oper))
				elog(ERROR, "could not find compatible hash operator for operator %u",
					 opexpr->opno);
			sstate->tab_eq_funcoids[i - 1] = get_opcode(rhs_eq_oper);
			fmgr_info(sstate->tab_eq_funcoids[i - 1],
					  &sstate->tab_eq_funcs[i - 1]);

			/* 查找关联的哈希函数          */
			if (!get_op_hash_functions(opexpr->opno,
									   &left_hashfn, &right_hashfn))
				elog(ERROR, "could not find hash function for hash operator %u",
					 opexpr->opno);
			fmgr_info(left_hashfn, &sstate->lhs_hash_funcs[i - 1]);
			fmgr_info(right_hashfn, &sstate->tab_hash_funcs[i - 1]);

			/* 排序规则  */
			sstate->tab_collations[i - 1] = opexpr->inputcollid;

			/* keyColIdx 只是列号 1..n           */
			sstate->keyColIdx[i - 1] = i;

			i++;
		}

		/*
		 * 为左右两侧构建 tupdesc、slot 和投影节点。左侧    
		 * 表达式将在父节点的 exprcontext 中求值，而我们   
		 * 在这里无法访问它。好在现在只需先传 NULL， 
		 * 稍后再补上（别扭做法！）。右侧表达式将     
		 * 在我们自己的 innerecontext                                  
		 * 中求值。      
		 */
		tupDescLeft = ExecTypeFromTL(lefttlist);
		slot = ExecInitExtraTupleSlot(estate, tupDescLeft, &TTSOpsVirtual);
		sstate->projLeft = ExecBuildProjectionInfo(lefttlist,
												   NULL,
												   slot,
												   parent,
												   NULL);

		sstate->descRight = tupDescRight = ExecTypeFromTL(righttlist);
		slot = ExecInitExtraTupleSlot(estate, tupDescRight, &TTSOpsVirtual);
		sstate->projRight = ExecBuildProjectionInfo(righttlist,
													sstate->innerecontext,
													slot,
													sstate->planstate,
													NULL);

		/*
		 * 创建用于在表中查找行的比较器（可能为跨      
		 * 类型比较）。      
		 */
		sstate->cur_eq_comp = ExecBuildGroupingEqual(tupDescLeft, tupDescRight,
													 &TTSOpsVirtual, &TTSOpsMinimalTuple,
													 ncols,
													 sstate->keyColIdx,
													 cross_eq_funcoids,
													 sstate->tab_collations,
													 parent);
	}

	return sstate;
}

/* ----------------------------------------------------------------
 *		ExecSetParamPlan
 *
 *		执行一个子计划并设置其输出参数。  
 *
 * 本函数由 ExecEvalParamExec() 调用：当请求某个 PARAM_EXEC  
 * 参数的值且该参数的 execPlan 字段已设置时（这表明     
 * 该参数尚未求值）。这使得 initplan 可以惰性求值：     
 * 除非需要其输出，否则不会运行子计划。                 
 * 注意本例程在求值之后必须清除该计划的输出参数 
 * 的 execPlan 字段！                  
 *
 * 本函数的结果存储在与 ExprContext 相关联的 EState 中       
 * （特别是其 ecxt_param_exec_vals）；任何按引用传递      
 * 的结果 Datum 都分配在 EState 的每查询内存中。传入       
 * 的 econtext 可以是该 EState 所属的任何 ExprContext；       
 * 用哪一个仅在如下方面有影响：即 ExprContext 的每    
 * 元组内存上下文被用来求值传递给子计划的参数。    
 * （因此原则上，ExprContext 的寿命越短越好，因为那     
 * 些数据在我们返回后就不再需要了。实践中，由于    
 * initplan 参数的复杂度从不超过 Var、Aggref 等，对它们      
 * 求值反正从不泄漏内存。）     
 * ----------------------------------------------------------------
 */
void
ExecSetParamPlan(SubPlanState *node, ExprContext *econtext)
{
	SubPlan    *subplan = node->subplan;
	PlanState  *planstate = node->planstate;
	SubLinkType subLinkType = subplan->subLinkType;
	EState	   *estate = planstate->state;
	ScanDirection dir = estate->es_direction;
	MemoryContext oldcontext;
	TupleTableSlot *slot;
	ListCell   *l;
	bool		found = false;
	ArrayBuildStateAny *astate = NULL;

	if (subLinkType == ANY_SUBLINK ||
		subLinkType == ALL_SUBLINK)
		elog(ERROR, "ANY/ALL subselect unsupported as initplan");
	if (subLinkType == CTE_SUBLINK)
		elog(ERROR, "CTE subplans should not be executed via ExecSetParamPlan");
	if (subplan->parParam || node->args)
		elog(ERROR, "correlated subplans should not be executed via ExecSetParamPlan");

	/*
	 * 无论调用者是谁，一律强制前向扫描方向。虽然很难 
	 * 经反向扫描到此，但非不可能，仍要让它可行。 
	 */
	estate->es_direction = ForwardScanDirection;

	/* 在调用者上下文中按需初始化 ArrayBuildStateAny   */
	if (subLinkType == ARRAY_SUBLINK)
		astate = initArrayResultAny(subplan->firstColType,
									CurrentMemoryContext, true);

	/*
	 * 必须切换到每查询内存上下文 
	 */
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

	/*
	 * 运行该计划。（若需要重新扫描，第一次             
	 * ExecProcNode 调用即可） 
	 */
	for (slot = ExecProcNode(planstate);
		 !TupIsNull(slot);
		 slot = ExecProcNode(planstate))
	{
		TupleDesc	tdesc = slot->tts_tupleDescriptor;
		int			i = 1;

		if (subLinkType == EXISTS_SUBLINK)
		{
			/* 只可能有一个 setParam...    */
			int			paramid = linitial_int(subplan->setParam);
			ParamExecData *prm = &(econtext->ecxt_param_exec_vals[paramid]);

			prm->execPlan = NULL;
			prm->value = BoolGetDatum(true);
			prm->isnull = false;
			found = true;
			break;
		}

		if (subLinkType == ARRAY_SUBLINK)
		{
			Datum		dvalue;
			bool		disnull;

			found = true;
			/* 把当前值暂存起来 */
			Assert(subplan->firstColType == TupleDescAttr(tdesc, 0)->atttypid);
			dvalue = slot_getattr(slot, 1, &disnull);
			astate = accumArrayResultAny(astate, dvalue, disnull,
										 subplan->firstColType, oldcontext);
			/* 继续扫描子计划以收集所有值     */
			continue;
		}

		if (found &&
			(subLinkType == EXPR_SUBLINK ||
			 subLinkType == MULTIEXPR_SUBLINK ||
			 subLinkType == ROWCOMPARE_SUBLINK))
			ereport(ERROR,
					(errcode(ERRCODE_CARDINALITY_VIOLATION),
					 errmsg("more than one row returned by a subquery used as an expression")));

		found = true;

		/*
		 * 需把子计划的元组复制到我们自己的上下文中，  
		 * 以防某些参数是按引用传递的类型 --- 存放在参数
		 * 结构中的指针将指向这份拷贝的元组！node->curTuple
		 * 会跟踪这份拷贝元组，以便最终释放。  
		 */
		if (node->curTuple)
			heap_freetuple(node->curTuple);
		node->curTuple = ExecCopySlotHeapTuple(slot);

		/*
		 * 现在从元组的各列设置所有 setParam 参数         
		 */
		foreach(l, subplan->setParam)
		{
			int			paramid = lfirst_int(l);
			ParamExecData *prm = &(econtext->ecxt_param_exec_vals[paramid]);

			prm->execPlan = NULL;
			prm->value = heap_getattr(node->curTuple, i, tdesc,
									  &(prm->isnull));
			i++;
		}
	}

	if (subLinkType == ARRAY_SUBLINK)
	{
		/* 只可能有一个 setParam...    */
		int			paramid = linitial_int(subplan->setParam);
		ParamExecData *prm = &(econtext->ecxt_param_exec_vals[paramid]);

		/*
		 * 结果数组要构建在查询上下文中，这样它才不会  
		 * 消失；为了避免在重复调用间泄漏内存，我们必须
		 * 记住最新值，类似上面 curTuple。   
		 */
		if (node->curArray != PointerGetDatum(NULL))
			pfree(DatumGetPointer(node->curArray));
		node->curArray = makeArrayResultAny(astate,
											econtext->ecxt_per_query_memory,
											true);
		prm->execPlan = NULL;
		prm->value = node->curArray;
		prm->isnull = false;
	}
	else if (!found)
	{
		if (subLinkType == EXISTS_SUBLINK)
		{
			/* 只可能有一个 setParam...    */
			int			paramid = linitial_int(subplan->setParam);
			ParamExecData *prm = &(econtext->ecxt_param_exec_vals[paramid]);

			prm->execPlan = NULL;
			prm->value = BoolGetDatum(false);
			prm->isnull = false;
		}
		else
		{
			/* 其他 sublink 类型则把所有输出参数设为 NULL   */
			foreach(l, subplan->setParam)
			{
				int			paramid = lfirst_int(l);
				ParamExecData *prm = &(econtext->ecxt_param_exec_vals[paramid]);

				prm->execPlan = NULL;
				prm->value = (Datum) 0;
				prm->isnull = true;
			}
		}
	}

	MemoryContextSwitchTo(oldcontext);

	/* 恢复扫描方向     */
	estate->es_direction = dir;
}

/*
 * ExecSetParamPlanMulti
 *
 * 应用 ExecSetParamPlan 来求值所有尚未求值的、其            
 * ParamID 列在 "params" 中的 initplan 输出参数。列出的         
 * 非 initplan 输出的则忽略。   
 *
 * 与 ExecSetParamPlan 一样，属于当前 EState 的任何              
 * ExprContext 都可以使用，但原则上寿命较短的优于           
 * 长寿命的。  
 */
void
ExecSetParamPlanMulti(const Bitmapset *params, ExprContext *econtext)
{
	int			paramid;

	paramid = -1;
	while ((paramid = bms_next_member(params, paramid)) >= 0)
	{
		ParamExecData *prm = &(econtext->ecxt_param_exec_vals[paramid]);

		if (prm->execPlan != NULL)
		{
			/* 参数尚未求值，现在就去求值  */
			ExecSetParamPlan(prm->execPlan, econtext);
			/* ExecSetParamPlan 本应已处理过该参数...      */
			Assert(prm->execPlan == NULL);
		}
	}
}

/*
 * 把 initplan 标记为需要重算       
 */
void
ExecReScanSetParamPlan(SubPlanState *node, PlanState *parent)
{
	PlanState  *planstate = node->planstate;
	SubPlan    *subplan = node->subplan;
	EState	   *estate = parent->state;
	ListCell   *l;

	/* 预防检查  */
	if (subplan->parParam != NIL)
		elog(ERROR, "direct correlated subquery unsupported as initplan");
	if (subplan->setParam == NIL)
		elog(ERROR, "setParam list of initplan is empty");
	if (bms_is_empty(planstate->plan->extParam))
		elog(ERROR, "extParam set of initplan is empty");

	/*
	 * 并不真正重新扫描：需要时在 ExecSetParamPlan 内发生。  
	 */

	/*
	 * 把本子计划的输出参数标记为需要重新计算。   
	 *
	 * CTE 子计划绝不会经由参数重算来执行；相反，它们 
	 * 是在被 nodeCtescan.c 调用时运行。因此不要把 CTE 子   
	 * 计划的输出参数标记为脏，但确实要为它设置           
	 * chgParam 位，通知依赖节点重新扫描。     
	 */
	foreach(l, subplan->setParam)
	{
		int			paramid = lfirst_int(l);
		ParamExecData *prm = &(estate->es_param_exec_vals[paramid]);

		if (subplan->subLinkType != CTE_SUBLINK)
			prm->execPlan = node;

		parent->chgParam = bms_add_member(parent->chgParam, paramid);
	}
}
