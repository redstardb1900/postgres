/*-------------------------------------------------------------------------
 *
 * nodeLimit.c
 *	  在适当情况下限制查询结果的例程。              
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeLimit.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *		ExecLimit - 抽取一个受限范围的元组 
 *		ExecInitLimit - 初始化节点与子节点   
 *		ExecEndLimit - 关闭节点与子节点  
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeLimit.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"

static void recompute_limits(LimitState *node);
static int64 compute_tuples_needed(LimitState *node);


/* ----------------------------------------------------------------
 *		ExecLimit
 *
 *		这是一个非常简单的节点，只执行 LIMIT/OFFSET 
 *		过滤，作用于子计划返回的元组流上。     
 * ----------------------------------------------------------------
 */
static TupleTableSlot *			/* 返回: 元组或 NULL  */
ExecLimit(PlanState *pstate)
{
	LimitState *node = castNode(LimitState, pstate);
	ExprContext *econtext = node->ps.ps_ExprContext;
	ScanDirection direction;
	TupleTableSlot *slot;
	PlanState  *outerPlan;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 从节点获取信息        
	 */
	direction = node->ps.state->es_direction;
	outerPlan = outerPlanState(node);

	/*
	 * 主逻辑是一个简单的状态机。  
	 */
	switch (node->lstate)
	{
		case LIMIT_INITIAL:

			/*
			 * 本节点首次调用，因此计算 limit/offset。（我们  
			 * 无法更早做这件事，因为上层节点的参数在     
			 * ExecInitLimit 期间不会被设置。）它还会置 position
			 * 为 0，状态改为 LIMIT_RESCAN 
			 */
			recompute_limits(node);

			/* FALL THRU */

		case LIMIT_RESCAN:

			/*
			 * 若是反向扫描，直接返回 NULL 且不改状态。  
			 */
			if (!ScanDirectionIsForward(direction))
				return NULL;

			/*
			 * 检查空窗口；若是，按空子计划处理。     
			 */
			if (node->count <= 0 && !node->noCount)
			{
				node->lstate = LIMIT_EMPTY;
				return NULL;
			}

			/*
			 * 从子计划取行，直到 position > offset 为止。  
			 */
			for (;;)
			{
				slot = ExecProcNode(outerPlan);
				if (TupIsNull(slot))
				{
					/*
					 * 子计划返回的元组过少，我们无法产生 
					 * 任何输出。   
					 */
					node->lstate = LIMIT_EMPTY;
					return NULL;
				}

				/*
				 * 后续执行需用 limit 位置元组进行比较    
				 * 以检测并列情况。 
				 */
				if (node->limitOption == LIMIT_OPTION_WITH_TIES &&
					node->position - node->offset == node->count - 1)
				{
					ExecCopySlot(node->last_slot, slot);
				}
				node->subSlot = slot;
				if (++node->position > node->offset)
					break;
			}

			/*
			 * 好，已拿到窗口的第一个元组。  
			 */
			node->lstate = LIMIT_INWINDOW;
			break;

		case LIMIT_EMPTY:

			/*
			 * 已知子计划不返回元组（或一般不超过       
			 * OFFSET 个元组）。所以我们不返回元组。
			 */
			return NULL;

		case LIMIT_INWINDOW:
			if (ScanDirectionIsForward(direction))
			{
				/*
				 * 前向扫描，故检查是否步出窗口末端。在     
				 * 窗口末端处，行为取决于是否指定 WITH TIES： 
				 * 若指定了，则需要把状态机改为            
				 * WINDOWEND_TIES，并落入该情形对应的代码。      
				 * 若未指定（或指定 ONLY），则返回 NULL，   
				 * 不推进子计划或 position 变量，但改变状态  
				 * 机，用来记录已经这么                         
				 * 做过  
				 *
				 * 理想情况下，应在到达末端后关闭并行  
				 * 资源；但这会销毁重新扫描所需的并行   
				 * 上下文。为此需要找到一种办法，传递    
				 * 更多信息，说明重新                           
				 * 扫描是否可能。        
				 */
				if (!node->noCount &&
					node->position - node->offset >= node->count)
				{
					if (node->limitOption == LIMIT_OPTION_COUNT)
					{
						node->lstate = LIMIT_WINDOWEND;
						return NULL;
					}
					else
					{
						node->lstate = LIMIT_WINDOWEND_TIES;
						/* we'll fall through to the next case */
					}
				}
				else
				{
					/*
					 * 从子计划取下一个元组。   
					 */
					slot = ExecProcNode(outerPlan);
					if (TupIsNull(slot))
					{
						node->lstate = LIMIT_SUBPLANEOF;
						return NULL;
					}

					/*
					 * 若 WITH TIES 生效，且这是窗口内最后       
					 * 一个元组，则保存它，供后续 WINDOWEND_TIES
					 * 处理     
					 */
					if (node->limitOption == LIMIT_OPTION_WITH_TIES &&
						node->position - node->offset == node->count - 1)
					{
						ExecCopySlot(node->last_slot, slot);
					}
					node->subSlot = slot;
					node->position++;
					break;
				}
			}
			else
			{
				/*
				 * 反向扫描，因此检查是否步出窗口起点。    
				 * 如上，仅在是时才改状态机状态。    
				 */
				if (node->position <= node->offset + 1)
				{
					node->lstate = LIMIT_WINDOWSTART;
					return NULL;
				}

				/*
				 * 从子计划取前一个元组；应该有一个！  
				 */
				slot = ExecProcNode(outerPlan);
				if (TupIsNull(slot))
					elog(ERROR, "LIMIT subplan failed to run backwards");
				node->subSlot = slot;
				node->position--;
				break;
			}

			Assert(node->lstate == LIMIT_WINDOWEND_TIES);
			/* FALL THRU */

		case LIMIT_WINDOWEND_TIES:
			if (ScanDirectionIsForward(direction))
			{
				/*
				 * 推进子计划，直到找到第一行具有不同 
				 * 的 ORDER BY pathkeys。    
				 */
				slot = ExecProcNode(outerPlan);
				if (TupIsNull(slot))
				{
					node->lstate = LIMIT_SUBPLANEOF;
					return NULL;
				}

				/*
				 * 测试新元组与最后元组是否匹配。若是，  
				 * 则返回它。  
				 */
				econtext->ecxt_innertuple = slot;
				econtext->ecxt_outertuple = node->last_slot;
				if (ExecQualAndReset(node->eqfunction, econtext))
				{
					node->subSlot = slot;
					node->position++;
				}
				else
				{
					node->lstate = LIMIT_WINDOWEND;
					return NULL;
				}
			}
			else
			{
				/*
				 * 反向扫描，因此检查是否步出窗口起点。    
				 * 若如此，仅改变状态机状态。
				 */
				if (node->position <= node->offset + 1)
				{
					node->lstate = LIMIT_WINDOWSTART;
					return NULL;
				}

				/*
				 * 从子计划取前一个元组；应该有一个！并且
				 * 要改变状态机状态。 
				 */
				slot = ExecProcNode(outerPlan);
				if (TupIsNull(slot))
					elog(ERROR, "LIMIT subplan failed to run backwards");
				node->subSlot = slot;
				node->position--;
				node->lstate = LIMIT_INWINDOW;
			}
			break;

		case LIMIT_SUBPLANEOF:
			if (ScanDirectionIsForward(direction))
				return NULL;

			/*
			 * 从子计划 EOF 处回退，所以重新取前一个元组；
			 * 应该有一个！前一个元组必须在窗口内。
			 */
			slot = ExecProcNode(outerPlan);
			if (TupIsNull(slot))
				elog(ERROR, "LIMIT subplan failed to run backwards");
			node->subSlot = slot;
			node->lstate = LIMIT_INWINDOW;
			/* position 不变，因为之前没有推进它               */
			break;

		case LIMIT_WINDOWEND:
			if (ScanDirectionIsForward(direction))
				return NULL;

			/*
			 * 为检测并列已前进过一个位置，所以重新 
			 * 取前一元组；应该有一个！注意前一元组必须  
			 * 在窗口内 
			 */
			if (node->limitOption == LIMIT_OPTION_WITH_TIES)
			{
				slot = ExecProcNode(outerPlan);
				if (TupIsNull(slot))
					elog(ERROR, "LIMIT subplan failed to run backwards");
				node->subSlot = slot;
				node->lstate = LIMIT_INWINDOW;
			}
			else
			{
				/*
				 * 从窗口末端回退：重新返回上次从子计划     
				 * 取到的那个元组。 
				 */
				slot = node->subSlot;
				node->lstate = LIMIT_INWINDOW;
				/* position 不变，因为之前没有推进它               */
			}
			break;

		case LIMIT_WINDOWSTART:
			if (!ScanDirectionIsForward(direction))
				return NULL;

			/*
			 * 从窗口起点回退后再前进：只需重新返回
			 * 上次从子计划取到的那个元组。        
			 */
			slot = node->subSlot;
			node->lstate = LIMIT_INWINDOW;
			/* position 不变，因为之前没有修改它              */
			break;

		default:
			elog(ERROR, "impossible LIMIT state: %d",
				 (int) node->lstate);
			slot = NULL;		/* 让编译器安静  */
			break;
	}

	/* 返回当前元组       */
	Assert(!TupIsNull(slot));

	return slot;
}

/*
 * 求值 limit/offset 表达式 --- 在启动或重新扫描时做。 
 *
 * 这里也是重置当前位置状态信息的好地方。           
 */
static void
recompute_limits(LimitState *node)
{
	ExprContext *econtext = node->ps.ps_ExprContext;
	Datum		val;
	bool		isNull;

	if (node->limitOffset)
	{
		val = ExecEvalExprSwitchContext(node->limitOffset,
										econtext,
										&isNull);
		/* NULL offset 视为无 offset       */
		if (isNull)
			node->offset = 0;
		else
		{
			node->offset = DatumGetInt64(val);
			if (node->offset < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_ROW_COUNT_IN_RESULT_OFFSET_CLAUSE),
						 errmsg("OFFSET must not be negative")));
		}
	}
	else
	{
		/* 未提供 OFFSET   */
		node->offset = 0;
	}

	if (node->limitCount)
	{
		val = ExecEvalExprSwitchContext(node->limitCount,
										econtext,
										&isNull);
		/* NULL count 视为无 count（LIMIT ALL）    */
		if (isNull)
		{
			node->count = 0;
			node->noCount = true;
		}
		else
		{
			node->count = DatumGetInt64(val);
			if (node->count < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_ROW_COUNT_IN_LIMIT_CLAUSE),
						 errmsg("LIMIT must not be negative")));
			node->noCount = false;
		}
	}
	else
	{
		/* 未提供 COUNT   */
		node->count = 0;
		node->noCount = true;
	}

	/* 重置 position 为扫描起点 */
	node->position = 0;
	node->subSlot = NULL;

	/* 设置状态机状态   */
	node->lstate = LIMIT_RESCAN;

	/*
	 * 把 limit 告知子节点。注意：不要想着在            
	 * compute_tuples_needed 返回 < 0 时跳过                          
	 * ExecSetTupleBound 就偷懒；无论如何都要更新子节点， 
	 * 以防重扫时上次结果不同。    
	 */
	ExecSetTupleBound(compute_tuples_needed(node), outerPlanState(node));
}

/*
 * 计算满足此 Limit 节点所需的最大元组数量。             
 * 如果不存在可确定的限制，则返回负值。       
 */
static int64
compute_tuples_needed(LimitState *node)
{
	if ((node->noCount) || (node->limitOption == LIMIT_OPTION_WITH_TIES))
		return -1;
	/* 注意：若此处溢出，会返回负值，这是可以接受的  */
	return node->count + node->offset;
}

/* ----------------------------------------------------------------
 *		ExecInitLimit
 *
 *		它初始化 limit 节点的状态结构            
 *		及其子计划。 
 * ----------------------------------------------------------------
 */
LimitState *
ExecInitLimit(Limit *node, EState *estate, int eflags)
{
	LimitState *limitstate;
	Plan	   *outerPlan;

	/* 检查不支持的标志    */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * 创建状态结构    
	 */
	limitstate = makeNode(LimitState);
	limitstate->ps.plan = (Plan *) node;
	limitstate->ps.state = estate;
	limitstate->ps.ExecProcNode = ExecLimit;

	limitstate->lstate = LIMIT_INITIAL;

	/*
	 * 杂项初始化             
	 *
	 * Limit 节点从不调用 ExecQual 或 ExecProject，但仍需要
	 * 一个表达式上下文来求值 limit/offset 参数。      
	 */
	ExecAssignExprContext(estate, &limitstate->ps);

	/*
	 * 初始化外层计划
	 */
	outerPlan = outerPlan(node);
	outerPlanState(limitstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * 初始化子表达式       
	 */
	limitstate->limitOffset = ExecInitExpr((Expr *) node->limitOffset,
										   (PlanState *) limitstate);
	limitstate->limitCount = ExecInitExpr((Expr *) node->limitCount,
										  (PlanState *) limitstate);
	limitstate->limitOption = node->limitOption;

	/*
	 * 初始化结果类型  
	 */
	ExecInitResultTypeTL(&limitstate->ps);

	limitstate->ps.resultopsset = true;
	limitstate->ps.resultops = ExecGetResultSlotOps(outerPlanState(limitstate),
													&limitstate->ps.resultopsfixed);

	/*
	 * limit 节点不做投影，因此为本节点适当地初始化      
	 * 投影信息      
	 */
	limitstate->ps.ps_ProjInfo = NULL;

	/*
	 * 初始化相等性求值，用于检测并列。   
	 */
	if (node->limitOption == LIMIT_OPTION_WITH_TIES)
	{
		TupleDesc	desc;
		const TupleTableSlotOps *ops;

		desc = ExecGetResultType(outerPlanState(limitstate));
		ops = ExecGetResultSlotOps(outerPlanState(limitstate), NULL);

		limitstate->last_slot = ExecInitExtraTupleSlot(estate, desc, ops);
		limitstate->eqfunction = execTuplesMatchPrepare(desc,
														node->uniqNumCols,
														node->uniqColIdx,
														node->uniqOperators,
														node->uniqCollations,
														&limitstate->ps);
	}

	return limitstate;
}

/* ----------------------------------------------------------------
 *		ExecEndLimit
 *
 *		它关闭子计划，并释放分配给本节点         
 *		的资源    
 * ----------------------------------------------------------------
 */
void
ExecEndLimit(LimitState *node)
{
	ExecFreeExprContext(&node->ps);
	ExecEndNode(outerPlanState(node));
}


void
ExecReScanLimit(LimitState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/*
	 * 如果参数变了则重新计算 limit/offset，并重置状态      
	 * 机。必须在重新扫描子节点之前做这件事，因为    
	 * 参数向下传递的节点可能是 Sort。           
	 */
	recompute_limits(node);

	/*
	 * 如果子节点的 chgParam 非空，则首次 ExecProcNode 会   
	 * 重新扫描计划 
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}
