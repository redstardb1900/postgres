/*-------------------------------------------------------------------------
 *
 * nodeMergeAppend.c
 *	  处理 MergeAppend 节点的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeMergeAppend.c
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitMergeAppend		- 初始化 MergeAppend 节点   
 *		ExecMergeAppend			- 从该节点取下一个元组       
 *		ExecEndMergeAppend		- 关闭 MergeAppend 节点     
 *		ExecReScanMergeAppend	- 重扫 MergeAppend 节点  
 *
 *	 NOTES
 *		MergeAppend 节点含一个或多个子计划的列表。  
 *		这些子计划都应递送按公共排序键排好序的元组， 
 *		MergeAppend 节点合并这些流，                            
 *		产生相同方式排序的输出。  
 *
 *		MergeAppend 节点并不使用其左右子树，        
 *		而是维护一个子计划列表，因此          
 *		典型的 MergeAppend 节点在计划树中如下所示：  
 *
 *				   ...
 *				   /
 *				MergeAppend---+------+------+--- nil
 *				/	\		  |		 |		|
 *			  nil	nil		 ...    ...    ...
 *								 subplans
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/execPartition.h"
#include "executor/nodeMergeAppend.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"

/*
 * 堆数组中的每一项各有一个槽。我们用 SlotNumber      
 * 来存储槽序号。这实际上并未提供任何正式的    
 * 类型安全，但能让代码更具自解释性。      
 */
typedef int32 SlotNumber;

static TupleTableSlot *ExecMergeAppend(PlanState *pstate);
static int	heap_compare_slots(Datum a, Datum b, void *arg);


/* ----------------------------------------------------------------
 *		ExecInitMergeAppend
 *
 *		启动 MergeAppend 节点的所有子扫描。    
 * ----------------------------------------------------------------
 */
MergeAppendState *
ExecInitMergeAppend(MergeAppend *node, EState *estate, int eflags)
{
	MergeAppendState *mergestate = makeNode(MergeAppendState);
	PlanState **mergeplanstates;
	Bitmapset  *validsubplans;
	int			nplans;
	int			i,
				j;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 为节点创建新的 MergeAppendState  
	 */
	mergestate->ps.plan = (Plan *) node;
	mergestate->ps.state = estate;
	mergestate->ps.ExecProcNode = ExecMergeAppend;

	/* 如果启用了运行时分区剪枝，那么现在设置好   */
	if (node->part_prune_info != NULL)
	{
		PartitionPruneState *prunestate;

		/*
		 * 建立剪枝数据结构。这同时会初始化子计划集合 
		 * （validsubplans），其中考虑了初始剪枝的             
		 * 结果（如果有的话）。              
		 */
		prunestate = ExecInitPartitionPruning(&mergestate->ps,
											  list_length(node->mergeplans),
											  node->part_prune_info,
											  &validsubplans);
		mergestate->ms_prune_state = prunestate;
		nplans = bms_num_members(validsubplans);

		/*
		 * 当无需运行时剪枝且至少有一子计划时，我们 
		 * 可以立即填充 ms_valid_subplans，从而避免以后     
		 * 调用 ExecFindMatchingSubPlans。      
		 */
		if (!prunestate->do_exec_prune && nplans > 0)
			mergestate->ms_valid_subplans = bms_add_range(NULL, 0, nplans - 1);
	}
	else
	{
		nplans = list_length(node->mergeplans);

		/*
		 * 当运行时分区剪枝未启用时，我们可以直接把全部 
		 * 子计划标为有效；它们也须全部初始化  
		 */
		Assert(nplans > 0);
		mergestate->ms_valid_subplans = validsubplans =
			bms_add_range(NULL, 0, nplans - 1);
		mergestate->ms_prune_state = NULL;
	}

	mergeplanstates = (PlanState **) palloc(nplans * sizeof(PlanState *));
	mergestate->mergeplans = mergeplanstates;
	mergestate->ms_nplans = nplans;

	mergestate->ms_slots = (TupleTableSlot **) palloc0(sizeof(TupleTableSlot *) * nplans);
	mergestate->ms_heap = binaryheap_allocate(nplans, heap_compare_slots,
											  mergestate);

	/*
	 * 杂项初始化             
	 *
	 * MergeAppend 节点确有 Result 槽，存放元组指针，           
	 * 我们必须初始化它们。 FIXME 
	 */
	ExecInitResultTupleSlotTL(&mergestate->ps, &TTSOpsVirtual);

	/* 节点从它的每个子节点返回槽位，因此并不固定   */
	mergestate->ps.resultopsset = true;
	mergestate->ps.resultopsfixed = false;

	/*
	 * 对每个待执行的有效计划调用 ExecInitNode，并将结果 
	 * 保存到 mergeplanstates 数组中。     
	 */
	j = 0;
	i = -1;
	while ((i = bms_next_member(validsubplans, i)) >= 0)
	{
		Plan	   *initNode = (Plan *) list_nth(node->mergeplans, i);

		mergeplanstates[j++] = ExecInitNode(initNode, estate, eflags);
	}

	mergestate->ps.ps_ProjInfo = NULL;

	/*
	 * 初始化排序键信息       
	 */
	mergestate->ms_nkeys = node->numCols;
	mergestate->ms_sortkeys = palloc0(sizeof(SortSupportData) * node->numCols);

	for (i = 0; i < node->numCols; i++)
	{
		SortSupport sortKey = mergestate->ms_sortkeys + i;

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = node->collations[i];
		sortKey->ssup_nulls_first = node->nullsFirst[i];
		sortKey->ssup_attno = node->sortColIdx[i];

		/*
		 * 缩写键转换并不可行，因为元组是按需           
		 * 被拉入 mergestate 的二叉堆中的。当元组           
		 * 被拉出时就将其转换为缩写表示，很可能      
		 * 适得其反，所以我们完全放弃这项                      
		 * 额外的优化。               
		 */
		sortKey->abbreviate = false;

		PrepareSortSupportFromOrderingOp(node->sortOperators[i], sortKey);
	}

	/*
	 * 初始化以表明我们尚未运行各子计划   
	 */
	mergestate->ms_initialized = false;

	return mergestate;
}

/* ----------------------------------------------------------------
 *	   ExecMergeAppend
 *
 *		处理对多个子计划的迭代。     
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecMergeAppend(PlanState *pstate)
{
	MergeAppendState *node = castNode(MergeAppendState, pstate);
	TupleTableSlot *result;
	SlotNumber	i;

	CHECK_FOR_INTERRUPTS();

	if (!node->ms_initialized)
	{
		/* 若子计划已全部剪枝则无事可做*/
		if (node->ms_nplans == 0)
			return ExecClearTuple(node->ps.ps_ResultTupleSlot);

		/*
		 * 如果尚未确定有效子计划，那么现在就确定。若 
		 * 运行时剪枝被禁用，则有效子计划总是会被设为    
		 * 全部子计划。  
		 */
		if (node->ms_valid_subplans == NULL)
			node->ms_valid_subplans =
				ExecFindMatchingSubPlans(node->ms_prune_state, false);

		/*
		 * 首次执行：从每个有效子计划拉取第一个元组，  
		 * 并建立堆。     
		 */
		i = -1;
		while ((i = bms_next_member(node->ms_valid_subplans, i)) >= 0)
		{
			node->ms_slots[i] = ExecProcNode(node->mergeplans[i]);
			if (!TupIsNull(node->ms_slots[i]))
				binaryheap_add_unordered(node->ms_heap, Int32GetDatum(i));
		}
		binaryheap_build(node->ms_heap);
		node->ms_initialized = true;
	}
	else
	{
		/*
		 * 否则，从上次返回元组的子计划拉取下一元组，  
		 * 并把该子计划序号重新插入堆中，因它与堆内 
		 * 现有元素的比较结果现在可能不同了。（我们 
		 * 若在前次调用返回之前先做，逻辑可稍简化，     
		 * 但最好必要时才拉取                                        
		 * 元组。）                        
		 */
		i = DatumGetInt32(binaryheap_first(node->ms_heap));
		node->ms_slots[i] = ExecProcNode(node->mergeplans[i]);
		if (!TupIsNull(node->ms_slots[i]))
			binaryheap_replace_first(node->ms_heap, Int32GetDatum(i));
		else
			(void) binaryheap_remove_first(node->ms_heap);
	}

	if (binaryheap_empty(node->ms_heap))
	{
		/* 所有子计划均已耗尽，堆也已耗尽      */
		result = ExecClearTuple(node->ps.ps_ResultTupleSlot);
	}
	else
	{
		i = DatumGetInt32(binaryheap_first(node->ms_heap));
		result = node->ms_slots[i];
	}

	return result;
}

/*
 * 比较给定两个槽中的元组。      
 */
static int32
heap_compare_slots(Datum a, Datum b, void *arg)
{
	MergeAppendState *node = (MergeAppendState *) arg;
	SlotNumber	slot1 = DatumGetInt32(a);
	SlotNumber	slot2 = DatumGetInt32(b);

	TupleTableSlot *s1 = node->ms_slots[slot1];
	TupleTableSlot *s2 = node->ms_slots[slot2];
	int			nkey;

	Assert(!TupIsNull(s1));
	Assert(!TupIsNull(s2));

	for (nkey = 0; nkey < node->ms_nkeys; nkey++)
	{
		SortSupport sortKey = node->ms_sortkeys + nkey;
		AttrNumber	attno = sortKey->ssup_attno;
		Datum		datum1,
					datum2;
		bool		isNull1,
					isNull2;
		int			compare;

		datum1 = slot_getattr(s1, attno, &isNull1);
		datum2 = slot_getattr(s2, attno, &isNull2);

		compare = ApplySortComparator(datum1, isNull1,
									  datum2, isNull2,
									  sortKey);
		if (compare != 0)
		{
			INVERT_COMPARE_RESULT(compare);
			return compare;
		}
	}
	return 0;
}

/* ----------------------------------------------------------------
 *		ExecEndMergeAppend
 *
 *		关闭 MergeAppend 节点的各个子扫描。  
 *
 *		无有价值的返回值。 
 * ----------------------------------------------------------------
 */
void
ExecEndMergeAppend(MergeAppendState *node)
{
	PlanState **mergeplans;
	int			nplans;
	int			i;

	/*
	 * 从节点获取信息        
	 */
	mergeplans = node->mergeplans;
	nplans = node->ms_nplans;

	/*
	 * 关闭各个子扫描         
	 */
	for (i = 0; i < nplans; i++)
		ExecEndNode(mergeplans[i]);
}

void
ExecReScanMergeAppend(MergeAppendState *node)
{
	int			i;

	/*
	 * 如果剪枝表达式用到的任何 PARAM_EXEC 参数发生变化，  
	 * 那么最好清空有效子计划集合，以便按新参数值     
	 * 重新挑选子计划。 
	 */
	if (node->ms_prune_state &&
		bms_overlap(node->ps.chgParam,
					node->ms_prune_state->execparamids))
	{
		bms_free(node->ms_valid_subplans);
		node->ms_valid_subplans = NULL;
	}

	for (i = 0; i < node->ms_nplans; i++)
	{
		PlanState  *subnode = node->mergeplans[i];

		/*
		 * ExecReScan 并不了解我的子计划，我必须自己  
		 * 完成已变参数的信号传递  
		 */
		if (node->ps.chgParam != NULL)
			UpdateChangedParamSet(subnode, node->ps.chgParam);

		/*
		 * 如果子节点的 chgParam 非空，则第一个 ExecProcNode    
		 * 会重新扫描。 
		 */
		if (subnode->chgParam == NULL)
			ExecReScan(subnode);
	}
	binaryheap_reset(node->ms_heap);
	node->ms_initialized = false;
}
