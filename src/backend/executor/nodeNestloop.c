/*-------------------------------------------------------------------------
 *
 * nodeNestloop.c
 *	  支持嵌套循环连接的例程  
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeNestloop.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 接口例程      
 *		ExecNestLoop - 处理两计划的嵌套循环连接 
 *		ExecInitNestLoop - 初始化该连接 
 *		ExecEndNestLoop  - 关闭该连接   
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeNestloop.h"
#include "miscadmin.h"
#include "utils/memutils.h"


/* ----------------------------------------------------------------
 *		ExecNestLoop(node)
 *
 * 旧注释   
 *		返回由内层元组与外层元组连接而成且满足 
 *		限定子句的元组。           
 *
 *		它扫描内层关系来与当前外层元组进行连接。 
 *
 *		如果没找到这样的元组，则取外层关系的下一个  
 *		元组，并从头重新扫描内层关系来与外层元组      
 *		进行连接。      
 *
 *		如果所有剩余的外层元组都已尝试且都无法与    
 *		内层元组连接，则返回 NULL。 
 *
 *		如果内层关系没有元组，同样返回 NULL。          
 *
 *		条件：  
 *		  -- outerTuple 保存来自外层关系的当前元组，    
 *			 右儿子(内层关系)在先前返回的元组处维护     
 *			 的 "cursor"。     
 *				这通过在外层关系上维护扫描位置来            
 *				实现。
 *
 *		初始状态：
 *		  -- 外层子节点和内层子节点  
 *			   都已准备好返回第一个元组。
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecNestLoop(PlanState *pstate)
{
	NestLoopState *node = castNode(NestLoopState, pstate);
	NestLoop   *nl;
	PlanState  *innerPlan;
	PlanState  *outerPlan;
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *innerTupleSlot;
	ExprState  *joinqual;
	ExprState  *otherqual;
	ExprContext *econtext;
	ListCell   *lc;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 从节点获取信息        
	 */
	ENL1_printf("getting info from node");

	nl = (NestLoop *) node->js.ps.plan;
	joinqual = node->js.joinqual;
	otherqual = node->js.ps.qual;
	outerPlan = outerPlanState(node);
	innerPlan = innerPlanState(node);
	econtext = node->js.ps.ps_ExprContext;

	/*
	 * 重置每元组内存上下文，释放上一个元组周期中 
	 * 分配的表达式求值存储。             
	 */
	ResetExprContext(econtext);

	/*
	 * 好的，连接的一切都已就绪，现在循环直到返回   
	 * 合格连接元组。 
	 */
	ENL1_printf("entering main loop");

	for (;;)
	{
		/*
		 * 如果没有外层元组，则取下一个并重置内层的   
		 * 扫描。  
		 */
		if (node->nl_NeedNewOuter)
		{
			ENL1_printf("getting new outer tuple");
			outerTupleSlot = ExecProcNode(outerPlan);

			/*
			 * 如果没有更多外层元组，则连接完成..            
			 */
			if (TupIsNull(outerTupleSlot))
			{
				ENL1_printf("no outer tuple, ending join");
				return NULL;
			}

			ENL1_printf("saving new outer tuple information");
			econtext->ecxt_outertuple = outerTupleSlot;
			node->nl_NeedNewOuter = false;
			node->nl_MatchedOuter = false;

			/*
			 * 取出必须传递给内层扫描的所有外层 Var 值，  
			 * 并将它们存入相应的 PARAM_EXEC 槽位。               
			 */
			foreach(lc, nl->nestParams)
			{
				NestLoopParam *nlp = (NestLoopParam *) lfirst(lc);
				int			paramno = nlp->paramno;
				ParamExecData *prm;

				prm = &(econtext->ecxt_param_exec_vals[paramno]);
				/* 参数值应为 OUTER_VAR 变量       */
				Assert(IsA(nlp->paramval, Var));
				Assert(nlp->paramval->varno == OUTER_VAR);
				Assert(nlp->paramval->varattno > 0);
				prm->value = slot_getattr(outerTupleSlot,
										  nlp->paramval->varattno,
										  &(prm->isnull));
				/* 标记该参数值已更改     */
				innerPlan->chgParam = bms_add_member(innerPlan->chgParam,
													 paramno);
			}

			/*
			 * 重新扫描内层计划 
			 */
			ENL1_printf("rescanning inner plan");
			ExecReScan(innerPlan);
		}

		/*
		 * 已有外层元组，尝试取下一个内层元组。 
		 */
		ENL1_printf("getting new inner tuple");

		innerTupleSlot = ExecProcNode(innerPlan);
		econtext->ecxt_innertuple = innerTupleSlot;

		if (TupIsNull(innerTupleSlot))
		{
			ENL1_printf("no inner tuple, need new outer tuple");

			node->nl_NeedNewOuter = true;

			if (!node->nl_MatchedOuter &&
				(node->js.jointype == JOIN_LEFT ||
				 node->js.jointype == JOIN_ANTI))
			{
				/*
				 * 我们在做外连接，该外层元组没有匹配项，
				 * 生成一个内层为空值的假连接元组，      
				 * 若它能通过非连接限定条件，                  
				 * 则返回它。
				 */
				econtext->ecxt_innertuple = node->nl_NullInnerTupleSlot;

				ENL1_printf("testing qualification for outer-join tuple");

				if (otherqual == NULL || ExecQual(otherqual, econtext))
				{
					/*
					 * 限定条件满足，因此我们投影并返回含 
					 * 结果元组的槽，使用               
					 * ExecProject().
					 */
					ENL1_printf("qualification succeeded, projecting tuple");

					return ExecProject(node->js.ps.ps_ProjInfo);
				}
				else
					InstrCountFiltered2(node, 1);
			}

			/*
			 * 否则回到循环顶部以获取新的外层元组。     
			 */
			continue;
		}

		/*
		 * 此时我们得到了一对新的内外元组，因此我们    
		 * 要测试这两个元组是否满足节点的                    
		 * 条件。     
		 *
		 * 只有 joinquals 决定 MatchedOuter 状态，但              
		 * 须全部通过才能返回该元组。
		 */
		ENL1_printf("testing qualification");

		if (ExecQual(joinqual, econtext))
		{
			node->nl_MatchedOuter = true;

			/* 反连接中我们从不返回已匹配的元组*/
			if (node->js.jointype == JOIN_ANTI)
			{
				node->nl_NeedNewOuter = true;
				continue;		/* 回到循环顶部    */
			}

			/*
			 * 如果我们只需连接到第一个匹配的内层元组，   
			 * 那么考虑返回这一个，但之后继续处理下一个  
			 * 外层元组
			 */
			if (node->js.single_match)
				node->nl_NeedNewOuter = true;

			if (otherqual == NULL || ExecQual(otherqual, econtext))
			{
				/*
				 * 限定条件已满足，因此我们投影并返回该  
				 * 包含结果元组的槽，使用 ExecProject()。   
				 */
				ENL1_printf("qualification succeeded, projecting tuple");

				return ExecProject(node->js.ps.ps_ProjInfo);
			}
			else
				InstrCountFiltered2(node, 1);
		}
		else
			InstrCountFiltered1(node, 1);

		/*
		 * 元组未通过 qual，释放每元组内存并重试。 
		 */
		ResetExprContext(econtext);

		ENL1_printf("qualification failed, looping");
	}
}

/* ----------------------------------------------------------------
 *		ExecInitNestLoop
 * ----------------------------------------------------------------
 */
NestLoopState *
ExecInitNestLoop(NestLoop *node, EState *estate, int eflags)
{
	NestLoopState *nlstate;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	NL1_printf("ExecInitNestLoop: %s\n",
			   "initializing node");

	/*
	 * 创建状态结构    
	 */
	nlstate = makeNode(NestLoopState);
	nlstate->js.ps.plan = (Plan *) node;
	nlstate->js.ps.state = estate;
	nlstate->js.ps.ExecProcNode = ExecNestLoop;

	/*
	 * 其他初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &nlstate->js.ps);

	/*
	 * 初始化子节点    
	 *
	 * 如果我们没有要从外层传给内层关系的参数，       
	 * 就告知内层子节点廉价重扫描会很有用。如果有      
	 * 这类参数，那么内层子节点中 REWIND 支持完全            
	 * 没有意义，因为它总是带着新的参数值                  
	 * values.
	 */
	outerPlanState(nlstate) = ExecInitNode(outerPlan(node), estate, eflags);
	if (node->nestParams == NIL)
		eflags |= EXEC_FLAG_REWIND;
	else
		eflags &= ~EXEC_FLAG_REWIND;
	innerPlanState(nlstate) = ExecInitNode(innerPlan(node), estate, eflags);

	/*
	 * 初始化结果槽、类型和投影。     
	 */
	ExecInitResultTupleSlotTL(&nlstate->js.ps, &TTSOpsVirtual);
	ExecAssignProjectionInfo(&nlstate->js.ps, NULL);

	/*
	 * 初始化子表达式       
	 */
	nlstate->js.ps.qual =
		ExecInitQual(node->join.plan.qual, (PlanState *) nlstate);
	nlstate->js.jointype = node->join.jointype;
	nlstate->js.joinqual =
		ExecInitQual(node->join.joinqual, (PlanState *) nlstate);

	/*
	 * 检测我们是否只需考虑第一个匹配的内层元组       
	 */
	nlstate->js.single_match = (node->join.inner_unique ||
								node->join.jointype == JOIN_SEMI);

	/* 如果需要，为外连接设置空元组    */
	switch (node->join.jointype)
	{
		case JOIN_INNER:
		case JOIN_SEMI:
			break;
		case JOIN_LEFT:
		case JOIN_ANTI:
			nlstate->nl_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate,
									  ExecGetResultType(innerPlanState(nlstate)),
									  &TTSOpsVirtual);
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) node->join.jointype);
	}

	/*
	 * 最后，清除当前的外层元组。     
	 */
	nlstate->nl_NeedNewOuter = true;
	nlstate->nl_MatchedOuter = false;

	NL1_printf("ExecInitNestLoop: %s\n",
			   "node initialized");

	return nlstate;
}

/* ----------------------------------------------------------------
 *		ExecEndNestLoop
 *
 *		关闭扫描并释放已分配的存储      
 * ----------------------------------------------------------------
 */
void
ExecEndNestLoop(NestLoopState *node)
{
	NL1_printf("ExecEndNestLoop: %s\n",
			   "ending node processing");

	/*
	 * 释放 exprcontext  
	 */
	ExecFreeExprContext(&node->js.ps);

	/*
	 * 清空元组表          
	 */
	ExecClearTuple(node->js.ps.ps_ResultTupleSlot);

	/*
	 * 关闭子计划    
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));

	NL1_printf("ExecEndNestLoop: %s\n",
			   "node processing ended");
}

/* ----------------------------------------------------------------
 *		ExecReScanNestLoop
 * ----------------------------------------------------------------
 */
void
ExecReScanNestLoop(NestLoopState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/*
	 * 若 outerPlan->chgParam 非空，则计划会被第一个         
	 * ExecProcNode 自动重扫。     
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);

	/*
	 * 内层计划会为每个新的外层元组重新扫描，绝对 
	 * 不能在这里重扫描它，否则当外层 Var 被用作运         
	 * 行时键时会遇到麻烦...         
	 */

	node->nl_NeedNewOuter = true;
	node->nl_MatchedOuter = false;
}
