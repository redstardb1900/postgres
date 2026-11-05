/*-------------------------------------------------------------------------
 *
 * execScan.c
 *	  这段代码为广义关系扫描提供支持。ExecScan           
 *	  接收一个节点及一个函数指针，用于"做正确的事"并
 *	  从关系中返回元组。然后 ExecScan 会做那些乏味的    
 *	  工作 - 检查 qual 条件并对元组作出              
 *	  适当投影  
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execScan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/memutils.h"



/*
 * ExecScanFetch -- 检查中断并获取下一个候选元组   
 *
 * 如果我们处于 EvalPlanQual 重查中，此例程负责替换 
 * 测试元组。否则，只需执行访问方法的        
 * 下一元组例程。                  
 */
static inline TupleTableSlot *
ExecScanFetch(ScanState *node,
			  ExecScanAccessMtd accessMtd,
			  ExecScanRecheckMtd recheckMtd)
{
	EState	   *estate = node->ps.state;

	CHECK_FOR_INTERRUPTS();

	if (estate->es_epq_active != NULL)
	{
		EPQState   *epqstate = estate->es_epq_active;

		/*
		 * 若处于 EvalPlanQual 重查中且有测试元组则返回，  
		 * 但要先重查访问方法特有的                         
		 * 条件。  
		 */
		Index		scanrelid = ((Scan *) node->ps.plan)->scanrelid;

		if (scanrelid == 0)
		{
			/*
			 * 这是一个 ForeignScan 或 CustomScan，它已将        
			 * 连接下推到了远端。若它是 EPQ 重查计划树中的 
			 * 后代节点，则运行重查方法函数。否则，         
			 * 运行下面的访问方法函数。 
			 */
			if (bms_is_member(epqstate->epqParam, node->ps.plan->extParam))
			{
				/*
				 * 重查方法不仅负责重查扫描/连接 qual，      
				 * 还负责把正确的元组存储                         
				 * 到槽中。
				 */

				TupleTableSlot *slot = node->ss_ScanTupleSlot;

				if (!(*recheckMtd) (node, slot))
					ExecClearTuple(slot);	/* 扫描将不会返回该元组*/
				return slot;
			}
		}
		else if (epqstate->relsubs_done[scanrelid - 1])
		{
			/*
			 * 返回空槽，因为要么此 rel 没有 EPQ 元组，        
			 * 要么已返回过它。  
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			return ExecClearTuple(slot);
		}
		else if (epqstate->relsubs_slot[scanrelid - 1] != NULL)
		{
			/*
			 * 返回 EPQ 调用者提供的替代元组。        
			 */

			TupleTableSlot *slot = epqstate->relsubs_slot[scanrelid - 1];

			Assert(epqstate->relsubs_rowmark[scanrelid - 1] == NULL);

			/* 做好标记，记住不再返回它               */
			epqstate->relsubs_done[scanrelid - 1] = true;

			/* 若无测试元组，则返回空槽             */
			if (TupIsNull(slot))
				return NULL;

			/* 检查它是否满足访问方法条件        */
			if (!(*recheckMtd) (node, slot))
				return ExecClearTuple(slot);	/* would not be returned by
												 * scan */
			return slot;
		}
		else if (epqstate->relsubs_rowmark[scanrelid - 1] != NULL)
		{
			/*
			 * 使用非锁定 rowmark 获取并返回替代元组。         
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			/* 做好标记，记住不再返回更多元组  */
			epqstate->relsubs_done[scanrelid - 1] = true;

			if (!EvalPlanQualFetchRowMark(epqstate, scanrelid, slot))
				return NULL;

			/* 若无测试元组，则返回空槽             */
			if (TupIsNull(slot))
				return NULL;

			/* 检查它是否满足访问方法条件        */
			if (!(*recheckMtd) (node, slot))
				return ExecClearTuple(slot);	/* would not be returned by
												 * scan */
			return slot;
		}
	}

	/*
	 * 运行节点类型特定的访问方法函数以获取下一个元组  
	 */
	return (*accessMtd) (node);
}

/* ----------------------------------------------------------------
 *		ExecScan
 *
 *		用指定的访问方法扫描关系，返回下一个    
 *		满足条件的元组。          
 *		访问方法返回下一个元组，ExecScan() 负责     
 *		对照 qual 子句检查返回的元组。                          
 *
 *		还须提供重查方法，以便对照访问方法内部 
 *		实现的任意 qual 条件，来检查关系的           
 *		任意元组。                                    
 *
 *		条件：  
 *		  -- AMI 维护的"游标"定位在上次返回的                
 *			 元组处。        
 *
 *		初始状态：
 *		  -- 指定的关系已为扫描而打开，使游标         
 *			 定位在首个满足条件的元组之前。            
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecScan(ScanState *node,
		 ExecScanAccessMtd accessMtd,	/* 返回元组的函数      */
		 ExecScanRecheckMtd recheckMtd)
{
	ExprContext *econtext;
	ExprState  *qual;
	ProjectionInfo *projInfo;

	/*
	 * 从节点取数据  
	 */
	qual = node->ps.qual;
	projInfo = node->ps.ps_ProjInfo;
	econtext = node->ps.ps_ExprContext;

	/* 中断检查位于 ExecScanFetch 中  */

	/*
	 * 若既无 qual 可查，又无投影要做，就跳过全部        
	 * 开销，直接返回原始扫描元组。     
	 */
	if (!qual && !projInfo)
	{
		ResetExprContext(econtext);
		return ExecScanFetch(node, accessMtd, recheckMtd);
	}

	/*
	 * 重置每元组内存上下文，以释放上个元组周期    
	 * 中分配的表达式求值存储。          
	 */
	ResetExprContext(econtext);

	/*
	 * 从访问方法获取元组。循环直到取得一个                
	 * 满足条件的元组。 
	 */
	for (;;)
	{
		TupleTableSlot *slot;

		slot = ExecScanFetch(node, accessMtd, recheckMtd);

		/*
		 * 若 accessMtd 返回的槽包含 NULL，则表示已经没有     
		 * 可扫描的内容，所以我们只需返回一个空槽，  
		 * 注意要使用投影结果槽，使其具有正确的           
		 * tupleDesc 
		 */
		if (TupIsNull(slot))
		{
			if (projInfo)
				return ExecClearTuple(projInfo->pi_state.resultslot);
			else
				return slot;
		}

		/*
		 * 将当前元组放入 expr 上下文中      
		 */
		econtext->ecxt_scantuple = slot;

		/*
		 * 检查当前元组是否满足 qual 子句            
		 *
		 * 此处检查 qual 是否为空，以避免在 qual 为空时调用 
		 * ExecQual()。这样只节省几个周期，但积少成多        
		 * ...
		 */
		if (qual == NULL || ExecQual(qual, econtext))
		{
			/*
			 * 找到合格的扫描元组。  
			 */
			if (projInfo)
			{
				/*
				 * 构造投影元组，将其存入结果元组槽          
				 * 并返回。  
				 */
				return ExecProject(projInfo);
			}
			else
			{
				/*
				 * 此处不做投影，直接返回扫描元组。      
				 */
				return slot;
			}
		}
		else
			InstrCountFiltered1(node, 1);

		/*
		 * 元组未通过 qual，释放每元组内存并重试。 
		 */
		ResetExprContext(econtext);
	}
}

/*
 * ExecAssignScanProjectionInfo
 *		必要时为扫描节点设置投影信息。        
 *
 * 如果请求的 tlist 与底层元组类型完全匹配，则可以  
 * 省去投影步骤。此时只需把 ps_ProjInfo 置为 NULL。    
 * 注意这种情况不仅出现在简单的 "SELECT * FROM ..." 中，  
 * 在扫描节点上方有连接或其他处理节点的大多数情况下
 * 也会出现这种情况，因为规划器会优先生成匹配的        
 * tlist.
 *
 * 扫描槽的描述符必须已经设置。            
 */
void
ExecAssignScanProjectionInfo(ScanState *node)
{
	Scan	   *scan = (Scan *) node->ps.plan;
	TupleDesc	tupdesc = node->ss_ScanTupleSlot->tts_tupleDescriptor;

	ExecConditionalAssignProjectionInfo(&node->ps, tupdesc, scan->scanrelid);
}

/*
 * ExecAssignScanProjectionInfoWithVarno
 *		同上，但调用者可指定 tlist 中 Vars 预期的 varno。     
 */
void
ExecAssignScanProjectionInfoWithVarno(ScanState *node, int varno)
{
	TupleDesc	tupdesc = node->ss_ScanTupleSlot->tts_tupleDescriptor;

	ExecConditionalAssignProjectionInfo(&node->ps, tupdesc, varno);
}

/*
 * ExecScanReScan
 *
 * 任何使用 ExecScan() 的计划节点类型都必须在其 ReScan 
 * 函数中调用。   
 */
void
ExecScanReScan(ScanState *node)
{
	EState	   *estate = node->ps.state;

	/*
	 * 必须清除扫描元组，让观察者（如 execCurrent.c）能够
	 * 看出该计划节点并未定位在某个元组上。    
	 */
	ExecClearTuple(node->ss_ScanTupleSlot);

	/*
	 * 如果处于 EvalPlanQual 重查中，则重新扫描 EPQ 元组，  
	 * 但不要丢失被阻塞目标关系的"阻塞"状态。        
	 */
	if (estate->es_epq_active != NULL)
	{
		EPQState   *epqstate = estate->es_epq_active;
		Index		scanrelid = ((Scan *) node->ps.plan)->scanrelid;

		if (scanrelid > 0)
			epqstate->relsubs_done[scanrelid - 1] =
				epqstate->relsubs_blocked[scanrelid - 1];
		else
		{
			Bitmapset  *relids;
			int			rtindex = -1;

			/*
			 * 如果 FDW 或自定义扫描提供者已把连接替换为   
			 * 扫描，则有多个 RTI，需全部重置其 relsubs_done   
			 * 标志。   
			 */
			if (IsA(node->ps.plan, ForeignScan))
				relids = ((ForeignScan *) node->ps.plan)->fs_base_relids;
			else if (IsA(node->ps.plan, CustomScan))
				relids = ((CustomScan *) node->ps.plan)->custom_relids;
			else
				elog(ERROR, "unexpected scan node: %d",
					 (int) nodeTag(node->ps.plan));

			while ((rtindex = bms_next_member(relids, rtindex)) >= 0)
			{
				Assert(rtindex > 0);
				epqstate->relsubs_done[rtindex - 1] =
					epqstate->relsubs_blocked[rtindex - 1];
			}
		}
	}
}
