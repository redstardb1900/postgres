/*-------------------------------------------------------------------------
 *
 * nodeMaterial.c
 *	  处理物化节点的例程。           
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeMaterial.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *		ExecMaterial - 物化子计划的结果             
 *		ExecInitMaterial - 初始化节点与子节点  
 *		ExecEndMaterial - 关闭节点与子节点    
 *
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeMaterial.h"
#include "miscadmin.h"

/* ----------------------------------------------------------------
 *		ExecMaterial
 *
 *		只要我们处于 tuplestore 中已收集数据的末尾，         
 *		每次调用都会从子计划收集一个新行，并在返回   
 *		之前把它存入 tuplestore。只有在我们被要求反向 
 *		扫描、重新扫描或标记/恢复时才会读取它。           
 *
 * ----------------------------------------------------------------
 */
static TupleTableSlot *			/* 子计划的结果元组  */
ExecMaterial(PlanState *pstate)
{
	MaterialState *node = castNode(MaterialState, pstate);
	EState	   *estate;
	ScanDirection dir;
	bool		forward;
	Tuplestorestate *tuplestorestate;
	bool		eof_tuplestore;
	TupleTableSlot *slot;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 取节点状态信息   
	 */
	estate = node->ss.ps.state;
	dir = estate->es_direction;
	forward = ScanDirectionIsForward(dir);
	tuplestorestate = node->tuplestorestate;

	/*
	 * 首遍执行且需要 tuplestore 时，初始化它。         
	 */
	if (tuplestorestate == NULL && node->eflags != 0)
	{
		tuplestorestate = tuplestore_begin_heap(true, false, work_mem);
		tuplestore_set_eflags(tuplestorestate, node->eflags);
		if (node->eflags & EXEC_FLAG_MARK)
		{
			/*
			 * 分配第二个读指针来充当标记。我们知道它      
			 * 索引必为 1，无需存储该值。   
			 */
			int			ptrno PG_USED_FOR_ASSERTS_ONLY;

			ptrno = tuplestore_alloc_read_pointer(tuplestorestate,
												  node->eflags);
			Assert(ptrno == 1);
		}
		node->tuplestorestate = tuplestorestate;
	}

	/*
	 * 如果我们不在 tuplestore 末尾，或者正在反向行进，     
	 * 则从 tuplestore 取元组。   
	 */
	eof_tuplestore = (tuplestorestate == NULL) ||
		tuplestore_ateof(tuplestorestate);

	if (!forward && eof_tuplestore)
	{
		if (!node->eof_underlying)
		{
			/*
			 * 在 tuplestore EOF 处反向时，第一次          
			 * gettupleslot 调用会取到最后加入的元组；但我们 
			 * 想返回它前面那个，如可能，则额外再取    
			 * 一次
			 */
			if (!tuplestore_advance(tuplestorestate, forward))
				return NULL;	/* 此时 tuplestore 必为空  */
		}
		eof_tuplestore = false;
	}

	/*
	 * 若能从 tuplestore 再取到一个元组，就返回它。 
	 */
	slot = node->ss.ps.ps_ResultTupleSlot;
	if (!eof_tuplestore)
	{
		if (tuplestore_gettupleslot(tuplestorestate, forward, false, slot))
			return slot;
		if (forward)
			eof_tuplestore = true;
	}

	/*
	 * 如有必要，尝试从子计划再取一行。        
	 *
	 * 注意：eof_underlying 状态变量的存在是为了短路           
	 * 后续的子计划调用。遗憾的是它并非可有可无，    
	 * 因为某些计划节点类型在返回 NULL 后再次被调用        
	 * 时易出错  
	 */
	if (eof_tuplestore && !node->eof_underlying)
	{
		PlanState  *outerNode;
		TupleTableSlot *outerslot;

		/*
		 * 只会在 forward==true 时到达这里，因此无需担心      
		 * 子计划将往哪个方向走。   
		 */
		outerNode = outerPlanState(node);
		outerslot = ExecProcNode(outerNode);
		if (TupIsNull(outerslot))
		{
			node->eof_underlying = true;
			return NULL;
		}

		/*
		 * 把返回元组的一份副本追加到 tuplestore。注意：   
		 * 由于 tuplestore 必然处于 EOF 状态，其读位置将     
		 * 向前移过新增元组。这正是我们想要的。   
		 */
		if (tuplestorestate)
			tuplestore_puttupleslot(tuplestorestate, outerslot);

		ExecCopySlot(slot, outerslot);
		return slot;
	}

	/*
	 * 没有剩余… 
	 */
	return ExecClearTuple(slot);
}

/* ----------------------------------------------------------------
 *		ExecInitMaterial
 * ----------------------------------------------------------------
 */
MaterialState *
ExecInitMaterial(Material *node, EState *estate, int eflags)
{
	MaterialState *matstate;
	Plan	   *outerPlan;

	/*
	 * 创建状态结构    
	 */
	matstate = makeNode(MaterialState);
	matstate->ss.ps.plan = (Plan *) node;
	matstate->ss.ps.state = estate;
	matstate->ss.ps.ExecProcNode = ExecMaterial;

	/*
	 * 要进行反向扫描或标记/恢复，必须有缓冲子计划     
	 * 输出的 tuplestore。如果可能被要求多次回绕重放，      
	 * 我们也倾向于物化子计划输出。但若都不适用，      
	 * 我们可以跳过数据存储。                          
	 */
	matstate->eflags = (eflags & (EXEC_FLAG_REWIND |
								  EXEC_FLAG_BACKWARD |
								  EXEC_FLAG_MARK));

	/*
	 * tuplestore 对标志位的解读与执行器的通用含义          
	 * 有微妙差别：它不认为 BACKWARD 一定意味着            
	 * "从头开始一直反向"。若被要求支持 BACKWARD，            
	 * 必须在 tuplestore eflags 中含 REWIND，否则 tuplestore_trim
	 * 可能丢得过多。     
	 */
	if (eflags & EXEC_FLAG_BACKWARD)
		matstate->eflags |= EXEC_FLAG_REWIND;

	matstate->eof_underlying = false;
	matstate->tuplestorestate = NULL;

	/*
	 * 杂项初始化             
	 *
	 * 物化节点不需要 ExprContext，因为它们从不调用         
	 * ExecQual 或 ExecProject
	 */

	/*
	 * 初始化子节点    
	 *
	 * 我们让子节点无需支持 REWIND、BACKWARD 或                  
	 * MARK/RESTORE 
	 */
	eflags &= ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);

	outerPlan = outerPlan(node);
	outerPlanState(matstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * 初始化结果类型与槽。无需初始化投影信息，因为    
	 * 该节点不做投影。                 
	 *
	 * 物化节点只返回来自其物化关系的元组。             
	 */
	ExecInitResultTupleSlotTL(&matstate->ss.ps, &TTSOpsMinimalTuple);
	matstate->ss.ps.ps_ProjInfo = NULL;

	/*
	 * 初始化元组类型 
	 */
	ExecCreateScanSlotFromOuterPlan(estate, &matstate->ss, &TTSOpsMinimalTuple);

	return matstate;
}

/* ----------------------------------------------------------------
 *		ExecEndMaterial
 * ----------------------------------------------------------------
 */
void
ExecEndMaterial(MaterialState *node)
{
	/*
	 * 清空元组表          
	 */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * 释放 tuplestore 资源    
	 */
	if (node->tuplestorestate != NULL)
		tuplestore_end(node->tuplestorestate);
	node->tuplestorestate = NULL;

	/*
	 * 关闭子计划      
	 */
	ExecEndNode(outerPlanState(node));
}

/* ----------------------------------------------------------------
 *		ExecMaterialMarkPos
 *
 *		调用 tuplestore 保存存储文件中的当前位置。        
 * ----------------------------------------------------------------
 */
void
ExecMaterialMarkPos(MaterialState *node)
{
	Assert(node->eflags & EXEC_FLAG_MARK);

	/*
	 * 如果尚未物化，则直接返回。     
	 */
	if (!node->tuplestorestate)
		return;

	/*
	 * 把活动读指针复制到标记。     
	 */
	tuplestore_copy_read_pointer(node->tuplestorestate, 0, 1);

	/*
	 * 由于我们可能已前移标记，尝试截断 tuplestore。      
	 */
	tuplestore_trim(node->tuplestorestate);
}

/* ----------------------------------------------------------------
 *		ExecMaterialRestrPos
 *
 *		调用 tuplestore 恢复上次保存的文件位置。   
 * ----------------------------------------------------------------
 */
void
ExecMaterialRestrPos(MaterialState *node)
{
	Assert(node->eflags & EXEC_FLAG_MARK);

	/*
	 * 如果尚未物化，则直接返回。     
	 */
	if (!node->tuplestorestate)
		return;

	/*
	 * 把标记复制到活动读指针。     
	 */
	tuplestore_copy_read_pointer(node->tuplestorestate, 1, 0);
}

/* ----------------------------------------------------------------
 *		ExecReScanMaterial
 *
 *		重新扫描物化关系。       
 * ----------------------------------------------------------------
 */
void
ExecReScanMaterial(MaterialState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	if (node->eflags != 0)
	{
		/*
		 * 如果尚未物化，则直接返回。如果外计划的  
		 * chgParam 非 NULL，那么 ExecProcNode 会重新扫描它，   
		 * 否则毫无必要重新扫描。   
		 */
		if (!node->tuplestorestate)
			return;

		/*
		 * 如果子节点将被重新扫描，那么我们丢弃之前
		 * 存储的结果；必须重读子计划并重新存储。另外，
		 * 若我们曾告诉 tuplestore 无需支持重扫，就会  
		 * 吃亏必须重读。（最后这种情形在常见场合不应 
		 * 发生；否则调用者没传 EXEC_FLAG_REWIND） 
		 *
		 * 否则可以直接回绕并重新扫描已存储的输出。  
		 * 子节点的状态不发生改变。 
		 */
		if (outerPlan->chgParam != NULL ||
			(node->eflags & EXEC_FLAG_REWIND) == 0)
		{
			tuplestore_end(node->tuplestorestate);
			node->tuplestorestate = NULL;
			if (outerPlan->chgParam == NULL)
				ExecReScan(outerPlan);
			node->eof_underlying = false;
		}
		else
			tuplestore_rescan(node->tuplestorestate);
	}
	else
	{
		/* 这种情况下我们只是原样传递子查询的输出 */

		/*
		 * 如果子节点的 chgParam 非空，则该计划会被           
		 * ExecProcNode 重扫
		 */
		if (outerPlan->chgParam == NULL)
			ExecReScan(outerPlan);
		node->eof_underlying = false;
	}
}
