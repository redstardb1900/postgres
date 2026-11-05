/*-------------------------------------------------------------------------
 *
 * nodeSetOp.c
 *	  处理 INTERSECT 与 EXCEPT 选择的例程      
 *
 * SetOp 节点的输入由来自两个关系的元组组成，    
 * 它们被合并成一个数据集，并添加 junk 属性，          
 * 标明每个元组来自哪个关系。在 SETOP_SORTED 模式下，  
 * 此外，输入还按所有分组列（即所有非 junk 属性） 
 * 做了排序。SetOp 节点扫描每一组相同的元组，         
 * 以确定每组中有多少元组来自各个输入关系。        
 * 之后即可轻松生成 SQL 规范对 INTERSECT、INTERSECT ALL、     
 * 或 EXCEPT ALL 所要求的输出。                         
 *
 * 在 SETOP_HASHED 模式下，输入无特定顺序送达，         
 * 只是我们知道一个输入关系的全部元组都会排在另         
 * 一关系之前。规划器保证 EXCEPT 的第一个输入关系        
 * 是左边的那个，并尽量让 INTERSECT 中较小的输入         
 * 关系排在前面。我们在内存中构建一个哈希表，为每组 
 * 相同元组各建一个条目，并统计组内来自每个关系的     
 * 元组数目。看完所有输入后，我们扫描哈希表并利用  
 * 这些计数生成正确的输出。对于只出现在第二个输入 
 * 关系中的元组，我们可以避免为其创建哈希表条目，     
 * 因为它们不会产生任何输出。                       
 *
 * 该节点类型不用于 UNION 或 UNION ALL，因为那两种情况 
 * 可用更低的代价实现（无需用 junk 属性来标识来源 
 * 关系）。                  
 *
 * 注意 SetOp 不做 qual 检查也不做投影。交付的输出    
 * 元组只是每组中最先到达的元组的副本。            
 * 输入组。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSetOp.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "executor/executor.h"
#include "executor/nodeSetOp.h"
#include "miscadmin.h"
#include "utils/memutils.h"


/*
 * SetOpStatePerGroupData - 每组的工作状态  
 *
 * 这些值是工作状态，在一个输入元组组的开始处   
 * 初始化，并随每个输入元组而更新。      
 *
 * 在 SETOP_SORTED 模式下，我们只需要一个这样的结构体，  
 * 它保存在计划状态节点中。在 SETOP_HASHED 模式下，      
 * 哈希表中每组有一个。
 */
typedef struct SetOpStatePerGroupData
{
	long		numLeft;		/* 组内左输入的重复元组数  */
	long		numRight;		/* 组内右输入的重复元组数   */
}			SetOpStatePerGroupData;


static TupleTableSlot *setop_retrieve_direct(SetOpState *setopstate);
static void setop_fill_hash_table(SetOpState *setopstate);
static TupleTableSlot *setop_retrieve_hash_table(SetOpState *setopstate);


/*
 * 为一组新的输入值初始化状态。       
 */
static inline void
initialize_counts(SetOpStatePerGroup pergroup)
{
	pergroup->numLeft = pergroup->numRight = 0;
}

/*
 * 为一个输入元组递增相应的计数器。    
 */
static inline void
advance_counts(SetOpStatePerGroup pergroup, int flag)
{
	if (flag)
		pergroup->numRight++;
	else
		pergroup->numLeft++;
}

/*
 * 从输入元组中取出 "flag" 列。      
 * 该列是整数列，左侧取值为 0，右侧取值为 1。          
 */
static int
fetch_tuple_flag(SetOpState *setopstate, TupleTableSlot *inputslot)
{
	SetOp	   *node = (SetOp *) setopstate->ps.plan;
	int			flag;
	bool		isNull;

	flag = DatumGetInt32(slot_getattr(inputslot,
									  node->flagColIdx,
									  &isNull));
	Assert(!isNull);
	Assert(flag == 0 || flag == 1);
	return flag;
}

/*
 * 把哈希表初始化为空。     
 */
static void
build_hash_table(SetOpState *setopstate)
{
	SetOp	   *node = (SetOp *) setopstate->ps.plan;
	ExprContext *econtext = setopstate->ps.ps_ExprContext;
	TupleDesc	desc = ExecGetResultType(outerPlanState(setopstate));

	Assert(node->strategy == SETOP_HASHED);
	Assert(node->numGroups > 0);

	setopstate->hashtable = BuildTupleHashTableExt(&setopstate->ps,
												   desc,
												   node->numCols,
												   node->dupColIdx,
												   setopstate->eqfuncoids,
												   setopstate->hashfunctions,
												   node->dupCollations,
												   node->numGroups,
												   0,
												   setopstate->ps.state->es_query_cxt,
												   setopstate->tableContext,
												   econtext->ecxt_per_tuple_memory,
												   false);
}

/*
 * 我们已完成一个元组组的处理。决定要发出其代表行的  
 * 多少份副本（如果有的话），并把计数存入 numOutput。
 * 这段逻辑直接来自 SQL92 规范。            
 */
static void
set_output_count(SetOpState *setopstate, SetOpStatePerGroup pergroup)
{
	SetOp	   *plannode = (SetOp *) setopstate->ps.plan;

	switch (plannode->cmd)
	{
		case SETOPCMD_INTERSECT:
			if (pergroup->numLeft > 0 && pergroup->numRight > 0)
				setopstate->numOutput = 1;
			else
				setopstate->numOutput = 0;
			break;
		case SETOPCMD_INTERSECT_ALL:
			setopstate->numOutput =
				(pergroup->numLeft < pergroup->numRight) ?
				pergroup->numLeft : pergroup->numRight;
			break;
		case SETOPCMD_EXCEPT:
			if (pergroup->numLeft > 0 && pergroup->numRight == 0)
				setopstate->numOutput = 1;
			else
				setopstate->numOutput = 0;
			break;
		case SETOPCMD_EXCEPT_ALL:
			setopstate->numOutput =
				(pergroup->numLeft < pergroup->numRight) ?
				0 : (pergroup->numLeft - pergroup->numRight);
			break;
		default:
			elog(ERROR, "unrecognized set op: %d", (int) plannode->cmd);
			break;
	}
}


/* ----------------------------------------------------------------
 *		ExecSetOp
 * ----------------------------------------------------------------
 */
static TupleTableSlot *			/* 返回: 元组或 NULL  */
ExecSetOp(PlanState *pstate)
{
	SetOpState *node = castNode(SetOpState, pstate);
	SetOp	   *plannode = (SetOp *) node->ps.plan;
	TupleTableSlot *resultTupleSlot = node->ps.ps_ResultTupleSlot;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 如果先前返回的元组需要被返回多次，                  
	 * 则继续返回它
	 */
	if (node->numOutput > 0)
	{
		node->numOutput--;
		return resultTupleSlot;
	}

	/* 否则，如果没有剩余的组就完成了 */
	if (node->setop_done)
		return NULL;

	/* 按照正确的策略取下一个元组组                   */
	if (plannode->strategy == SETOP_HASHED)
	{
		if (!node->table_filled)
			setop_fill_hash_table(node);
		return setop_retrieve_hash_table(node);
	}
	else
		return setop_retrieve_direct(node);
}

/*
 * 非哈希情况下 ExecSetOp 
 */
static TupleTableSlot *
setop_retrieve_direct(SetOpState *setopstate)
{
	PlanState  *outerPlan;
	SetOpStatePerGroup pergroup;
	TupleTableSlot *outerslot;
	TupleTableSlot *resultTupleSlot;
	ExprContext *econtext = setopstate->ps.ps_ExprContext;

	/*
	 * 从节点取状态信息
	 */
	outerPlan = outerPlanState(setopstate);
	pergroup = (SetOpStatePerGroup) setopstate->pergroup;
	resultTupleSlot = setopstate->ps.ps_ResultTupleSlot;

	/*
	 * 我们循环取出各组，直到找到应返回的组为止
	 */
	while (!setopstate->setop_done)
	{
		/*
		 * 如果我们还没有新组的第一个元组，就从外层计划 
		 * 取回它。        
		 */
		if (setopstate->grp_firstTuple == NULL)
		{
			outerslot = ExecProcNode(outerPlan);
			if (!TupIsNull(outerslot))
			{
				/* 为第一个输入元组制作拷贝 */
				setopstate->grp_firstTuple = ExecCopySlotHeapTuple(outerslot);
			}
			else
			{
				/* 外层计划根本没产生元组    */
				setopstate->setop_done = true;
				return NULL;
			}
		}

		/*
		 * 把拷贝的第一个输入元组存入保留给它的元组表槽 
		 * 中。该元组从槽中被清除时，它将被删           
		 * 除  
		 */
		ExecStoreHeapTuple(setopstate->grp_firstTuple,
						   resultTupleSlot,
						   true);
		setopstate->grp_firstTuple = NULL;	/* 不要保留两个指针*/

		/* 为一个新的输入元组组初始化工作状态  */
		initialize_counts(pergroup);

		/* 统计第一个输入元组 */
		advance_counts(pergroup,
					   fetch_tuple_flag(setopstate, resultTupleSlot));

		/*
		 * 扫描外层计划，直到耗尽它或跨越组边界为止。   
		 */
		for (;;)
		{
			outerslot = ExecProcNode(outerPlan);
			if (TupIsNull(outerslot))
			{
				/* 外层计划已没有可用元组   */
				setopstate->setop_done = true;
				break;
			}

			/*
			 * 检查我们是否跨越了组边界。      
			 */
			econtext->ecxt_outertuple = resultTupleSlot;
			econtext->ecxt_innertuple = outerslot;

			if (!ExecQualAndReset(setopstate->eqfunction, econtext))
			{
				/*
				 * 保存下一组的第一个输入元组。   
				 */
				setopstate->grp_firstTuple = ExecCopySlotHeapTuple(outerslot);
				break;
			}

			/* 仍在同一组，因此统计该元组  */
			advance_counts(pergroup,
						   fetch_tuple_flag(setopstate, outerslot));
		}

		/*
		 * 输入元组组扫描完毕。查看是否应发出结果元组的
		 * 若干副本，若是则返回第一份副本。 
		 */
		set_output_count(setopstate, pergroup);

		if (setopstate->numOutput > 0)
		{
			setopstate->numOutput--;
			return resultTupleSlot;
		}
	}

	/* 已无更多组*/
	ExecClearTuple(resultTupleSlot);
	return NULL;
}

/*
 * 哈希情形的 ExecSetOp：阶段 1，读取输入并建哈希表 
 */
static void
setop_fill_hash_table(SetOpState *setopstate)
{
	SetOp	   *node = (SetOp *) setopstate->ps.plan;
	PlanState  *outerPlan;
	int			firstFlag;
	bool		in_first_rel PG_USED_FOR_ASSERTS_ONLY;
	ExprContext *econtext = setopstate->ps.ps_ExprContext;

	/*
	 * 从节点取状态信息
	 */
	outerPlan = outerPlanState(setopstate);
	firstFlag = node->firstFlag;
	/* 确认规划器没搞错      */
	Assert(firstFlag == 0 ||
		   (firstFlag == 1 &&
			(node->cmd == SETOPCMD_INTERSECT ||
			 node->cmd == SETOPCMD_INTERSECT_ALL)));

	/*
	 * 处理每个外层计划元组，然后取下一个，直到耗尽  
	 * 外层计划。        
	 */
	in_first_rel = true;
	for (;;)
	{
		TupleTableSlot *outerslot;
		int			flag;
		TupleHashEntryData *entry;
		bool		isnew;

		outerslot = ExecProcNode(outerPlan);
		if (TupIsNull(outerslot))
			break;

		/* 判断它是左输入还是右输入      */
		flag = fetch_tuple_flag(setopstate, outerslot);

		if (flag == firstFlag)
		{
			/* (仍)在第一个输入关系中*/
			Assert(in_first_rel);

			/* 查找或构建该元组所属组的哈希表条目  */
			entry = LookupTupleHashEntry(setopstate->hashtable, outerslot,
										 &isnew, NULL);

			/* 若为新元组组则初始化计数  */
			if (isnew)
			{
				entry->additional = (SetOpStatePerGroup)
					MemoryContextAlloc(setopstate->hashtable->tablecxt,
									   sizeof(SetOpStatePerGroupData));
				initialize_counts((SetOpStatePerGroup) entry->additional);
			}

			/* 递增计数       */
			advance_counts((SetOpStatePerGroup) entry->additional, flag);
		}
		else
		{
			/* 已到达第二个关系*/
			in_first_rel = false;

			/* 对于先前未见过的元组，不要创建哈希表条目*/
			entry = LookupTupleHashEntry(setopstate->hashtable, outerslot,
										 NULL, NULL);

			/* 若条目已存在则递增计数              */
			if (entry)
				advance_counts((SetOpStatePerGroup) entry->additional, flag);
		}

		/* 每次哈希表查找后必须重置表达式上下文    */
		ResetExprContext(econtext);
	}

	setopstate->table_filled = true;
	/* 初始化以遍历哈希表       */
	ResetTupleHashIterator(setopstate->hashtable, &setopstate->hashiter);
}

/*
 * 哈希情形的 ExecSetOp：阶段 2，从哈希表检索各元组组
 */
static TupleTableSlot *
setop_retrieve_hash_table(SetOpState *setopstate)
{
	TupleHashEntryData *entry;
	TupleTableSlot *resultTupleSlot;

	/*
	 * 从节点取状态信息
	 */
	resultTupleSlot = setopstate->ps.ps_ResultTupleSlot;

	/*
	 * 我们循环取出各组，直到找到应返回的组为止
	 */
	while (!setopstate->setop_done)
	{
		CHECK_FOR_INTERRUPTS();

		/*
		 * 在哈希表中找下一个条目    
		 */
		entry = ScanTupleHashTable(setopstate->hashtable, &setopstate->hashiter);
		if (entry == NULL)
		{
			/* 哈希表无更多条目，故完成  */
			setopstate->setop_done = true;
			return NULL;
		}

		/*
		 * 查看是否应发出该元组的若干副本，若是则返回 
		 * 第一份副本
		 */
		set_output_count(setopstate, (SetOpStatePerGroup) entry->additional);

		if (setopstate->numOutput > 0)
		{
			setopstate->numOutput--;
			return ExecStoreMinimalTuple(entry->firstTuple,
										 resultTupleSlot,
										 false);
		}
	}

	/* 已无更多组*/
	ExecClearTuple(resultTupleSlot);
	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitSetOp
 *
 *		初始化 setop 节点状态结构以及该节点的
 *		子计划。       
 * ----------------------------------------------------------------
 */
SetOpState *
ExecInitSetOp(SetOp *node, EState *estate, int eflags)
{
	SetOpState *setopstate;
	TupleDesc	outerDesc;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 创建状态结构    
	 */
	setopstate = makeNode(SetOpState);
	setopstate->ps.plan = (Plan *) node;
	setopstate->ps.state = estate;
	setopstate->ps.ExecProcNode = ExecSetOp;

	setopstate->eqfuncoids = NULL;
	setopstate->hashfunctions = NULL;
	setopstate->setop_done = false;
	setopstate->numOutput = 0;
	setopstate->pergroup = NULL;
	setopstate->grp_firstTuple = NULL;
	setopstate->hashtable = NULL;
	setopstate->tableContext = NULL;

	/*
	 * 创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &setopstate->ps);

	/*
	 * 哈希时还需一个寿命更长的上下文存放哈希表。  
	 * 该表不能只放在每查询上下文中，因为我们希望在   
	 * ExecReScanSetOp 中把它丢弃。                     
	 */
	if (node->strategy == SETOP_HASHED)
		setopstate->tableContext =
			AllocSetContextCreate(CurrentMemoryContext,
								  "SetOp hash table",
								  ALLOCSET_DEFAULT_SIZES);

	/*
	 * 初始化子节点    
	 *
	 * 如果采用哈希，则子计划无需高效地处理 REWIND；    
	 * 参见 ExecReScanSetOp。        
	 */
	if (node->strategy == SETOP_HASHED)
		eflags &= ~EXEC_FLAG_REWIND;
	outerPlanState(setopstate) = ExecInitNode(outerPlan(node), estate, eflags);
	outerDesc = ExecGetResultType(outerPlanState(setopstate));

	/*
	 * 初始化结果槽与类型。Setop 节点不做投影，因此   
	 * 适当地初始化该节点的投影信息。          
	 */
	ExecInitResultTupleSlotTL(&setopstate->ps,
							  node->strategy == SETOP_HASHED ?
							  &TTSOpsMinimalTuple : &TTSOpsHeapTuple);
	setopstate->ps.ps_ProjInfo = NULL;

	/*
	 * 为内层循环预计算 fmgr 查找数据。若按哈希方式做，
	 * 等值与哈希函数都要；不按哈希则只需等值      
	 * 函数  
	 */
	if (node->strategy == SETOP_HASHED)
		execTuplesHashPrepare(node->numCols,
							  node->dupOperators,
							  &setopstate->eqfuncoids,
							  &setopstate->hashfunctions);
	else
		setopstate->eqfunction =
			execTuplesMatchPrepare(outerDesc,
								   node->numCols,
								   node->dupColIdx,
								   node->dupOperators,
								   node->dupCollations,
								   &setopstate->ps);

	if (node->strategy == SETOP_HASHED)
	{
		build_hash_table(setopstate);
		setopstate->table_filled = false;
	}
	else
	{
		setopstate->pergroup =
			(SetOpStatePerGroup) palloc0(sizeof(SetOpStatePerGroupData));
	}

	return setopstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSetOp
 *
 *		它关闭子计划并释放分配给该节点            
 *		的资源    
 * ----------------------------------------------------------------
 */
void
ExecEndSetOp(SetOpState *node)
{
	/* 清理元组表      */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/* 释放包括哈希表在内的附属内容*/
	if (node->tableContext)
		MemoryContextDelete(node->tableContext);
	ExecFreeExprContext(&node->ps);

	ExecEndNode(outerPlanState(node));
}


void
ExecReScanSetOp(SetOpState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	ExecClearTuple(node->ps.ps_ResultTupleSlot);
	node->setop_done = false;
	node->numOutput = 0;

	if (((SetOp *) node->ps.plan)->strategy == SETOP_HASHED)
	{
		/*
		 * 哈希情况下，若还没建哈希表，直接返回即可；   
		 * 尚未做任何事，因此无需撤销。如果子节点的       
		 * chgParam 非 NULL，则它会被 ExecProcNode 重新扫描，   
		 * 否则没有理由重新扫描它。
		 */
		if (!node->table_filled)
			return;

		/*
		 * 若已有哈希表且子计划无参数变化，则只需     
		 * 重新扫描现有的哈希表即可；                            
		 * 无需重建它。        
		 */
		if (outerPlan->chgParam == NULL)
		{
			ResetTupleHashIterator(node->hashtable, &node->hashiter);
			return;
		}
	}

	/* 若已制作副本，则释放组的第一个元组  */
	if (node->grp_firstTuple != NULL)
	{
		heap_freetuple(node->grp_firstTuple);
		node->grp_firstTuple = NULL;
	}

	/* 释放所有哈希表存储   */
	if (node->tableContext)
		MemoryContextResetAndDeleteChildren(node->tableContext);

	/* 并在需要时重建空哈希表     */
	if (((SetOp *) node->ps.plan)->strategy == SETOP_HASHED)
	{
		ResetTupleHashTable(node->hashtable);
		node->table_filled = false;
	}

	/*
	 * 若子节点的 chgParam 非空，计划将由第一次           
	 * ExecProcNode 重扫
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}
