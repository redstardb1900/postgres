/*-------------------------------------------------------------------------
 *
 * nodeCtescan.c
 *	  处理 CteScan 节点的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeCtescan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeCtescan.h"
#include "miscadmin.h"

static TupleTableSlot *CteScanNext(CteScanState *node);

/* ----------------------------------------------------------------
 *		CteScanNext
 *
 *		这是 ExecCteScan 的主力函数 
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
CteScanNext(CteScanState *node)
{
	EState	   *estate;
	ScanDirection dir;
	bool		forward;
	Tuplestorestate *tuplestorestate;
	bool		eof_tuplestore;
	TupleTableSlot *slot;

	/*
	 * 从节点取状态信息
	 */
	estate = node->ss.ps.state;
	dir = estate->es_direction;
	forward = ScanDirectionIsForward(dir);
	tuplestorestate = node->leader->cte_table;
	tuplestore_select_read_pointer(tuplestorestate, node->readptr);
	slot = node->ss.ss_ScanTupleSlot;

	/*
	 * 若我们不在 tuplestore 末尾，或者正在反向行进，尝试  
	 * 从 tuplestore 取一个元组。
	 */
	eof_tuplestore = tuplestore_ateof(tuplestorestate);

	if (!forward && eof_tuplestore)
	{
		if (!node->leader->eof_cte)
		{
			/*
			 * 在 tuplestore EOF 处反向时，第一次          
			 * gettupleslot 调用会取最后加入的元组；但我们想 
			 * 返回它前面那个元组（如可能）。因此额外 
			 * 取。
			 */
			if (!tuplestore_advance(tuplestorestate, forward))
				return NULL;	/* tuplestore 必为空         */
		}
		eof_tuplestore = false;
	}

	/*
	 * 如果能从 tuplestore 再取一个元组，就返回它。 
	 *
	 * 注意：我们必须在 tuplestore_gettupleslot 调用中使用   
	 * copy=true，因为我们与其他节点共享 tuplestore，其他      
	 * 节点可能在下次调用前写入其中。  
	 */
	if (!eof_tuplestore)
	{
		if (tuplestore_gettupleslot(tuplestorestate, forward, true, slot))
			return slot;
		if (forward)
			eof_tuplestore = true;
	}

	/*
	 * 如有必要，尝试从 CTE 查询再取一行。        
	 *
	 * 注意：eof_cte 状态变量的存在是为了短路后续对 CTE    
	 * 计划的调用。可惜它并非可选，因为某些计划节点   
	 * 类型在被再次调用时并不健壮，即使它们已经返回过  
	 * NULL。       
	 */
	if (eof_tuplestore && !node->leader->eof_cte)
	{
		TupleTableSlot *cteslot;

		/*
		 * 只有在 forward==true 时才能到达这里，因此无需担心
		 * 子计划将朝哪个方向行进。
		 */
		cteslot = ExecProcNode(node->cteplanstate);
		if (TupIsNull(cteslot))
		{
			node->leader->eof_cte = true;
			return NULL;
		}

		/*
		 * 存在一些边角情况，子计划可能改变           
		 * 哪个 tuplestore 读指针活跃，存入取得的元组前  
		 * 务必重选我们的指针。  
		 */
		tuplestore_select_read_pointer(tuplestorestate, node->readptr);

		/*
		 * 把返回元组的副本追加到 tuplestore。注意：因为   
		 * 我们的读指针肯定处于 EOF 状态，其读位置会向前 
		 * 移过新增元组。这正是我们想要的。同时，其他 
		 * 读取者*不会*越过新元组前移，而这是它们想要     
		 * 的。    
		 */
		tuplestore_puttupleslot(tuplestorestate, cteslot);

		/*
		 * 必须把 CTE 查询输出元组复制到自己的槽中。这是
		 * 因为其他 CteScan 节点可能在我们再被调用前推     
		 * 进 CTE 查询，而我们的输出元组必须保持稳定不 
		 * 变  
		 */
		return ExecCopySlot(slot, cteslot);
	}

	/*
	 * 已无剩余。 
	 */
	return ExecClearTuple(slot);
}

/*
 * CteScanRecheck -- 在 EvalPlanQual 中重查元组的访问方法例程   
 */
static bool
CteScanRecheck(CteScanState *node, TupleTableSlot *slot)
{
	/* 无需检查     */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecCteScan(node)
 *
 *		顺序扫描 CTE 并返回下一个满足条件的元组。      
 *		调用 ExecScan() 例程，并传入相应的访问方法 
 *		函数。               
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecCteScan(PlanState *pstate)
{
	CteScanState *node = castNode(CteScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) CteScanNext,
					(ExecScanRecheckMtd) CteScanRecheck);
}


/* ----------------------------------------------------------------
 *		ExecInitCteScan
 * ----------------------------------------------------------------
 */
CteScanState *
ExecInitCteScan(CteScan *node, EState *estate, int eflags)
{
	CteScanState *scanstate;
	ParamExecData *prmdata;

	/* 检查不支持的标志    */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * 目前我们必须强制 tuplestore 允许 REWIND，因为即使       
	 * 上层并未要求我们为高效重扫做准备，我们也可能 
	 * 被要求重扫 CTE。恼人的是，这会阻止 tuplestore      
	 * 的截断。XXX FIXME                            
	 *
	 * 注意：如果我们处于 EPQ 重查计划树中，很可能完全   
	 * 不需要访问 tuplestore，这会更恼人。只要反正        
	 * 所有读指针都会 REWIND，就不值得改进这一点；但    
	 * 如果我们将来改进此逻辑，那么这                         
	 * 一方面也应考虑。 
	 */
	eflags |= EXEC_FLAG_REWIND;

	/*
	 * CteScan 不应有任何子节点。  
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * 为节点新建 CteScanState    
	 */
	scanstate = makeNode(CteScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecCteScan;
	scanstate->eflags = eflags;
	scanstate->cte_table = NULL;
	scanstate->eof_cte = false;

	/*
	 * 找到 CTE 查询已初始化的计划。           
	 */
	scanstate->cteplanstate = (PlanState *) list_nth(estate->es_subplanstates,
													 node->ctePlanId - 1);

	/*
	 * 与 CTE 查询关联的 Param 槽用于保存一个指针，指向    
	 * 为此 CTE 初始化的第一个 CteScan 节点的 CteState。     
	 * 该节点将是持有所有 CTE 共享状态的那个节点，特别   
	 * 是共享的 tuplestore。               
	 */
	prmdata = &(estate->es_param_exec_vals[node->cteParam]);
	Assert(prmdata->execPlan == NULL);
	Assert(!prmdata->isnull);
	scanstate->leader = castNode(CteScanState, DatumGetPointer(prmdata->value));
	if (scanstate->leader == NULL)
	{
		/* 我是领导者 */
		prmdata->value = PointerGetDatum(scanstate);
		scanstate->leader = scanstate;
		scanstate->cte_table = tuplestore_begin_heap(true, false, work_mem);
		tuplestore_set_eflags(scanstate->cte_table, scanstate->eflags);
		scanstate->readptr = 0;
	}
	else
	{
		/* 并非领导者*/
		/* 创建自己的读指针，并确保它位于起点   */
		scanstate->readptr =
			tuplestore_alloc_read_pointer(scanstate->leader->cte_table,
										  scanstate->eflags);
		tuplestore_select_read_pointer(scanstate->leader->cte_table,
									   scanstate->readptr);
		tuplestore_rescan(scanstate->leader->cte_table);
	}

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * 扫描元组类型（即预期在工作表中找到的行类型）
	 * 与 CTE 查询的结果行类型相同。                 
	 */
	ExecInitScanTupleSlot(estate, &scanstate->ss,
						  ExecGetResultType(scanstate->cteplanstate),
						  &TTSOpsMinimalTuple);

	/*
	 * 初始化结果类型与投影。     
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * 初始化子表达式       
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndCteScan
 *
 *		释放通过 C 例程分配的所有存储。  
 * ----------------------------------------------------------------
 */
void
ExecEndCteScan(CteScanState *node)
{
	/*
	 * 释放表达式 
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * 清空元组表          
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * 若为领导者则释放 tuplestore。  
	 */
	if (node->leader == node)
	{
		tuplestore_end(node->cte_table);
		node->cte_table = NULL;
	}
}

/* ----------------------------------------------------------------
 *		ExecReScanCteScan
 *
 *		重新扫描关系。
 * ----------------------------------------------------------------
 */
void
ExecReScanCteScan(CteScanState *node)
{
	Tuplestorestate *tuplestorestate = node->leader->cte_table;

	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	ExecScanReScan(&node->ss);

	/*
	 * 如果需要对底层 CTE 进行新一轮扫描，则清空          
	 * tuplestore。这会隐式重置 tuplestore 的所有读指针。     
	 * 注意：多个 CTE 节点可能会冗余地清空 tuplestore；     
	 * 这没有问题，代价也不高。一旦有人尝试从底层  
	 * CTE 读取内容（使 chgParam 被清除），我们就会     
	 * 停止走这条路径。                     
	 */
	if (node->leader->cteplanstate->chgParam != NULL)
	{
		tuplestore_clear(tuplestorestate);
		node->leader->eof_cte = false;
	}
	else
	{
		/*
		 * 否则只需回绕自己的读指针。要么底层 CTE     
		 * 不需要重新扫描（这样我们现在就能重读 tuplestore 
		 * 中的内容），要么别人已处理过。  
		 */
		tuplestore_select_read_pointer(tuplestorestate, node->readptr);
		tuplestore_rescan(tuplestorestate);
	}
}
