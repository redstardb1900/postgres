/*-------------------------------------------------------------------------
 *
 * nodeRecursiveunion.c
 *	  处理 RecursiveUnion 节点的例程。
 *
 * 为了实现 UNION(不带 ALL)，我们需要一个哈希表            
 * 来存储已见过的元组。哈希键由分组列计算出。   
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeRecursiveunion.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeRecursiveunion.h"
#include "miscadmin.h"
#include "utils/memutils.h"



/*
 * 把哈希表初始化为空。     
 */
static void
build_hash_table(RecursiveUnionState *rustate)
{
	RecursiveUnion *node = (RecursiveUnion *) rustate->ps.plan;
	TupleDesc	desc = ExecGetResultType(outerPlanState(rustate));

	Assert(node->numCols > 0);
	Assert(node->numGroups > 0);

	rustate->hashtable = BuildTupleHashTableExt(&rustate->ps,
												desc,
												node->numCols,
												node->dupColIdx,
												rustate->eqfuncoids,
												rustate->hashfunctions,
												node->dupCollations,
												node->numGroups,
												0,
												rustate->ps.state->es_query_cxt,
												rustate->tableContext,
												rustate->tempContext,
												false);
}


/* ----------------------------------------------------------------
 *		ExecRecursiveUnion(node)
 *
 *		顺序扫描递归查询并返回下一个满足条件的  
 *		元组。        
 *
 * 1. 求值非递归项并把结果赋给 RT                
 *
 * 2. 执行递归项        
 *
 * 2.1 WT := RT
 * 2.2 当 WT 非空时重复 2.3 到 2.6；若 WT 为空则返回 RT    
 * 2.3 用 WT 替换递归项的名字           
 * 2.4 求值递归项并存入 WT                  
 * 2.5 WT 并入 RT   
 * 2.6 回到 2.2    
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecRecursiveUnion(PlanState *pstate)
{
	RecursiveUnionState *node = castNode(RecursiveUnionState, pstate);
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);
	RecursiveUnion *plan = (RecursiveUnion *) node->ps.plan;
	TupleTableSlot *slot;
	bool		isnew;

	CHECK_FOR_INTERRUPTS();

	/* 1. 求值非递归项          */
	if (!node->recursing)
	{
		for (;;)
		{
			slot = ExecProcNode(outerPlan);
			if (TupIsNull(slot))
				break;
			if (plan->numCols > 0)
			{
				/* 查找或构建该元组所在组的哈希表项     */
				LookupTupleHashEntry(node->hashtable, slot, &isnew, NULL);
				/* 每次哈希表查找后必须重置临时上下文 */
				MemoryContextReset(node->tempContext);
				/* 若元组已见过则忽略  */
				if (!isnew)
					continue;
			}
			/* 每个非重复的元组都进入工作表...          */
			tuplestore_puttupleslot(node->working_table, slot);
			/* ...并交给调用者 */
			return slot;
		}
		node->recursing = true;
	}

	/* 2. 执行递归项        */
	for (;;)
	{
		slot = ExecProcNode(innerPlan);
		if (TupIsNull(slot))
		{
			/* 如果中间表中没有任何内容则完成     */
			if (node->intermediate_empty)
				break;

			/* 用完旧的工作表...        */
			tuplestore_end(node->working_table);

			/* 中间表变成工作表                 */
			node->working_table = node->intermediate_table;

			/* 创建新的空中间表            */
			node->intermediate_table = tuplestore_begin_heap(false, false,
															 work_mem);
			node->intermediate_empty = true;

			/* 重置递归项          */
			innerPlan->chgParam = bms_add_member(innerPlan->chgParam,
												 plan->wtParam);

			/* 然后继续从递归项取元组         */
			continue;
		}

		if (plan->numCols > 0)
		{
			/* 查找或构建该元组所在组的哈希表项     */
			LookupTupleHashEntry(node->hashtable, slot, &isnew, NULL);
			/* 每次哈希表查找后必须重置临时上下文 */
			MemoryContextReset(node->tempContext);
			/* 若元组已见过则忽略  */
			if (!isnew)
				continue;
		}

		/* 否则该元组合格；将它存入中间表...        */
		node->intermediate_empty = false;
		tuplestore_puttupleslot(node->intermediate_table, slot);
		/* ...并返回它   */
		return slot;
	}

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitRecursiveUnion
 * ----------------------------------------------------------------
 */
RecursiveUnionState *
ExecInitRecursiveUnion(RecursiveUnion *node, EState *estate, int eflags)
{
	RecursiveUnionState *rustate;
	ParamExecData *prmdata;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 创建状态结构    
	 */
	rustate = makeNode(RecursiveUnionState);
	rustate->ps.plan = (Plan *) node;
	rustate->ps.state = estate;
	rustate->ps.ExecProcNode = ExecRecursiveUnion;

	rustate->eqfuncoids = NULL;
	rustate->hashfunctions = NULL;
	rustate->hashtable = NULL;
	rustate->tempContext = NULL;
	rustate->tableContext = NULL;

	/* 初始化处理状态       */
	rustate->recursing = false;
	rustate->intermediate_empty = true;
	rustate->working_table = tuplestore_begin_heap(false, false, work_mem);
	rustate->intermediate_table = tuplestore_begin_heap(false, false, work_mem);

	/*
	 * 如果使用哈希，就需要一个用于比较的每元组内存   
	 * 上下文，以及一个寿命更长的上下文来存放哈希表。 
	 * 该表不能只保存在每查询上下文中，因为我们希望  
	 * 重扫时丢弃它。
	 */
	if (node->numCols > 0)
	{
		rustate->tempContext =
			AllocSetContextCreate(CurrentMemoryContext,
								  "RecursiveUnion",
								  ALLOCSET_DEFAULT_SIZES);
		rustate->tableContext =
			AllocSetContextCreate(CurrentMemoryContext,
								  "RecursiveUnion hash table",
								  ALLOCSET_DEFAULT_SIZES);
	}

	/*
	 * 通过为它保留的 Param 槽位，让状态结构对后代       
	 * WorkTableScan 节点可用。      
	 */
	prmdata = &(estate->es_param_exec_vals[node->wtParam]);
	Assert(prmdata->execPlan == NULL);
	prmdata->value = PointerGetDatum(rustate);
	prmdata->isnull = false;

	/*
	 * 其他初始化             
	 *
	 * RecursiveUnion 计划没有表达式上下文，因其从不调用    
	 * ExecQual 或 ExecProject.    
	 */
	Assert(node->plan.qual == NIL);

	/*
	 * RecursiveUnion 节点仍拥有 Result 槽，其中保存            
	 * 着元组指针，故须初始化。  
	 */
	ExecInitResultTypeTL(&rustate->ps);

	/*
	 * 初始化结果元组类型。(注意：必须在初始化子节       
	 * 点之前设置好它，因为 nodeWorktablescan.c 期望              
	 * 它有效。 
	 */
	rustate->ps.ps_ProjInfo = NULL;

	/*
	 * 初始化子节点    
	 */
	outerPlanState(rustate) = ExecInitNode(outerPlan(node), estate, eflags);
	innerPlanState(rustate) = ExecInitNode(innerPlan(node), estate, eflags);

	/*
	 * 如果使用哈希，则为内层循环预计算 fmgr 数据并创建 
	 * hash table.
	 */
	if (node->numCols > 0)
	{
		execTuplesHashPrepare(node->numCols,
							  node->dupOperators,
							  &rustate->eqfuncoids,
							  &rustate->hashfunctions);
		build_hash_table(rustate);
	}

	return rustate;
}

/* ----------------------------------------------------------------
 *		ExecEndRecursiveUnion
 *
 *		释放经 C 例程分配的所有存储。     
 * ----------------------------------------------------------------
 */
void
ExecEndRecursiveUnion(RecursiveUnionState *node)
{
	/* 释放 tuplestore   */
	tuplestore_end(node->working_table);
	tuplestore_end(node->intermediate_table);

	/* 释放包括哈希表在内的附属内容*/
	if (node->tempContext)
		MemoryContextDelete(node->tempContext);
	if (node->tableContext)
		MemoryContextDelete(node->tableContext);

	/*
	 * 关闭子计划    
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));
}

/* ----------------------------------------------------------------
 *		ExecReScanRecursiveUnion
 *
 *		重新扫描关系。
 * ----------------------------------------------------------------
 */
void
ExecReScanRecursiveUnion(RecursiveUnionState *node)
{
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);
	RecursiveUnion *plan = (RecursiveUnion *) node->ps.plan;

	/*
	 * 将递归项的 chgParam 设置为告知它：我们将修改         
	 * 工作表，故它必须重扫描。 
	 */
	innerPlan->chgParam = bms_add_member(innerPlan->chgParam, plan->wtParam);

	/*
	 * 如果子节点的 chgParam 非空，则计划会被第一个     
	 * ExecProcNode 重扫描。因此我们只需对非递归项           
	 * 这样做。       
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);

	/* 释放所有哈希表存储   */
	if (node->tableContext)
		MemoryContextResetAndDeleteChildren(node->tableContext);

	/* 必要时清空哈希表  */
	if (plan->numCols > 0)
		ResetTupleHashTable(node->hashtable);

	/* 重置处理状态     */
	node->recursing = false;
	node->intermediate_empty = true;
	tuplestore_clear(node->working_table);
	tuplestore_clear(node->intermediate_table);
}
