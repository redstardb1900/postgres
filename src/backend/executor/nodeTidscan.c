/*-------------------------------------------------------------------------
 *
 * nodeTidscan.c
 *	  支撑关系直接 tid 扫描的例程           
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeTidscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *
 *		ExecTidScan			使用 tid 扫描关系    
 *		ExecInitTidScan		创建并初始化状态信息。  
 *		ExecReScanTidScan	重新扫描 tid 关系  
 *		ExecEndTidScan		释放所有存储。
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "access/tableam.h"
#include "catalog/pg_type.h"
#include "executor/execdebug.h"
#include "executor/nodeTidscan.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/rel.h"


/*
 * 要识别 CTID 变量，只需检查 varattno 即可，因为本关系 
 * 扫描 qual 中的任何 Var 都必然属于我们的表。（即便 
 * 它是引用其他表 CTID 的参数化扫描，那张表的 Var 到     
 * 这里时早已变成了 Param。）                    
 */
#define IsCTIDVar(node)  \
	((node) != NULL && \
	 IsA((node), Var) && \
	 ((Var *) (node))->varattno == SelfItemPointerAttributeNumber)

/* tss_tidexprs 的一个元素*/
typedef struct TidExpr
{
	ExprState  *exprstate;		/* TID 子表达式对应的 ExprState  */
	bool		isarray;		/* 若为真，则产出 tid[] 而非 tid*/
	CurrentOfExpr *cexpr;		/* 或者也可以使用 CURRENT OF      */
} TidExpr;

static void TidExprListCreate(TidScanState *tidstate);
static void TidListEval(TidScanState *tidstate);
static int	itemptr_comparator(const void *a, const void *b);
static TupleTableSlot *TidNext(TidScanState *node);


/*
 * 抽取 qual 中产生待查找 TID 的子表达式，         
 * 若它们是普通表达式，则把它们编译为 ExprState。 
 *
 * CURRENT OF 是特例，无法将其有效地编译；       
 * 只需把它原样放入 TidExpr 列表  
 */
static void
TidExprListCreate(TidScanState *tidstate)
{
	TidScan    *node = (TidScan *) tidstate->ss.ps.plan;
	ListCell   *l;

	tidstate->tss_tidexprs = NIL;
	tidstate->tss_isCurrentOf = false;

	foreach(l, node->tidquals)
	{
		Expr	   *expr = (Expr *) lfirst(l);
		TidExpr    *tidexpr = (TidExpr *) palloc0(sizeof(TidExpr));

		if (is_opclause(expr))
		{
			Node	   *arg1;
			Node	   *arg2;

			arg1 = get_leftop(expr);
			arg2 = get_rightop(expr);
			if (IsCTIDVar(arg1))
				tidexpr->exprstate = ExecInitExpr((Expr *) arg2,
												  &tidstate->ss.ps);
			else if (IsCTIDVar(arg2))
				tidexpr->exprstate = ExecInitExpr((Expr *) arg1,
												  &tidstate->ss.ps);
			else
				elog(ERROR, "could not identify CTID variable");
			tidexpr->isarray = false;
		}
		else if (expr && IsA(expr, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saex = (ScalarArrayOpExpr *) expr;

			Assert(IsCTIDVar(linitial(saex->args)));
			tidexpr->exprstate = ExecInitExpr(lsecond(saex->args),
											  &tidstate->ss.ps);
			tidexpr->isarray = true;
		}
		else if (expr && IsA(expr, CurrentOfExpr))
		{
			CurrentOfExpr *cexpr = (CurrentOfExpr *) expr;

			tidexpr->cexpr = cexpr;
			tidstate->tss_isCurrentOf = true;
		}
		else
			elog(ERROR, "could not identify CTID expression");

		tidstate->tss_tidexprs = lappend(tidstate->tss_tidexprs, tidexpr);
	}

	/* CurrentOfExpr 绝不会与其他东西 OR 起来出现    */
	Assert(list_length(tidstate->tss_tidexprs) == 1 ||
		   !tidstate->tss_isCurrentOf);
}

/*
 * 对这些表达式求值，以计算出需要访问的 TID           
 * 列表。
 *
 * （结果其实是数组，而非列表）    
 */
static void
TidListEval(TidScanState *tidstate)
{
	ExprContext *econtext = tidstate->ss.ps.ps_ExprContext;
	TableScanDesc scan;
	ItemPointerData *tidList;
	int			numAllocTids;
	int			numTids;
	ListCell   *l;

	/*
	 * 按需启动扫描 - 初始化扫描并非免费（例如堆要统计 
	 * 表的大小），所以推迟到需要时再初始化才有意义 -  
	 * 该节点可能从不执行。    
	 */
	if (tidstate->ss.ss_currentScanDesc == NULL)
		tidstate->ss.ss_currentScanDesc =
			table_beginscan_tid(tidstate->ss.ss_currentRelation,
								tidstate->ss.ps.state->es_snapshot);
	scan = tidstate->ss.ss_currentScanDesc;

	/*
	 * 初始化数组时预留了足够槽位，应对所有 qual 均为   
	 * 简单 OpExpr 或 CurrentOfExpr。若存在任何      
	 * ScalarArrayOpExpr，则可能必须扩大该数组。
	 */
	numAllocTids = list_length(tidstate->tss_tidexprs);
	tidList = (ItemPointerData *)
		palloc(numAllocTids * sizeof(ItemPointerData));
	numTids = 0;

	foreach(l, tidstate->tss_tidexprs)
	{
		TidExpr    *tidexpr = (TidExpr *) lfirst(l);
		ItemPointer itemptr;
		bool		isNull;

		if (tidexpr->exprstate && !tidexpr->isarray)
		{
			itemptr = (ItemPointer)
				DatumGetPointer(ExecEvalExprSwitchContext(tidexpr->exprstate,
														  econtext,
														  &isNull));
			if (isNull)
				continue;

			/*
			 * 我们会静默丢弃任何被 AM 视为无效的 TID     
			 * （例如堆表，扫描开始时它们可能已经越界。  
			 * 由于我们对表至少持有 AccessShareLock，因此        
			 * 不可能有人截断掉我们打算访问的               
			 * 那些块。）   
			 */
			if (!table_tuple_tid_valid(scan, itemptr))
				continue;

			if (numTids >= numAllocTids)
			{
				numAllocTids *= 2;
				tidList = (ItemPointerData *)
					repalloc(tidList,
							 numAllocTids * sizeof(ItemPointerData));
			}
			tidList[numTids++] = *itemptr;
		}
		else if (tidexpr->exprstate && tidexpr->isarray)
		{
			Datum		arraydatum;
			ArrayType  *itemarray;
			Datum	   *ipdatums;
			bool	   *ipnulls;
			int			ndatums;
			int			i;

			arraydatum = ExecEvalExprSwitchContext(tidexpr->exprstate,
												   econtext,
												   &isNull);
			if (isNull)
				continue;
			itemarray = DatumGetArrayTypeP(arraydatum);
			deconstruct_array_builtin(itemarray, TIDOID, &ipdatums, &ipnulls, &ndatums);
			if (numTids + ndatums > numAllocTids)
			{
				numAllocTids = numTids + ndatums;
				tidList = (ItemPointerData *)
					repalloc(tidList,
							 numAllocTids * sizeof(ItemPointerData));
			}
			for (i = 0; i < ndatums; i++)
			{
				if (ipnulls[i])
					continue;

				itemptr = (ItemPointer) DatumGetPointer(ipdatums[i]);

				if (!table_tuple_tid_valid(scan, itemptr))
					continue;

				tidList[numTids++] = *itemptr;
			}
			pfree(ipdatums);
			pfree(ipnulls);
		}
		else
		{
			ItemPointerData cursor_tid;

			Assert(tidexpr->cexpr);
			if (execCurrentOf(tidexpr->cexpr, econtext,
							  RelationGetRelid(tidstate->ss.ss_currentRelation),
							  &cursor_tid))
			{
				if (numTids >= numAllocTids)
				{
					numAllocTids *= 2;
					tidList = (ItemPointerData *)
						repalloc(tidList,
								 numAllocTids * sizeof(ItemPointerData));
				}
				tidList[numTids++] = cursor_tid;
			}
		}
	}

	/*
	 * 将 TID 数组排成有序，并消除其中的重复项。 
	 * 消除重复是必要的，因为需要在整个列表上获得 OR   
	 * 语义。排序使重复更易检测，也顺带确保以最有效     
	 * 的方式访问堆。                                         
	 */
	if (numTids > 1)
	{
		/* CurrentOfExpr 绝不会与其他东西 OR 起来出现    */
		Assert(!tidstate->tss_isCurrentOf);

		qsort(tidList, numTids, sizeof(ItemPointerData),
			  itemptr_comparator);
		numTids = qunique(tidList, numTids, sizeof(ItemPointerData),
						  itemptr_comparator);
	}

	tidstate->tss_TidList = tidList;
	tidstate->tss_NumTids = numTids;
	tidstate->tss_TidPtr = -1;
}

/*
 * ItemPointerData 项的 qsort 比较器    
 */
static int
itemptr_comparator(const void *a, const void *b)
{
	const ItemPointerData *ipa = (const ItemPointerData *) a;
	const ItemPointerData *ipb = (const ItemPointerData *) b;
	BlockNumber ba = ItemPointerGetBlockNumber(ipa);
	BlockNumber bb = ItemPointerGetBlockNumber(ipb);
	OffsetNumber oa = ItemPointerGetOffsetNumber(ipa);
	OffsetNumber ob = ItemPointerGetOffsetNumber(ipb);

	if (ba < bb)
		return -1;
	if (ba > bb)
		return 1;
	if (oa < ob)
		return -1;
	if (oa > ob)
		return 1;
	return 0;
}

/* ----------------------------------------------------------------
 *		TidNext
 *
 *		从 TidScan 节点的 currentRelation 中取回元组，
 *		即使用 TidScanState 信息里的那些 tid  
 *
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
TidNext(TidScanState *node)
{
	EState	   *estate;
	ScanDirection direction;
	Snapshot	snapshot;
	TableScanDesc scan;
	Relation	heapRelation;
	TupleTableSlot *slot;
	ItemPointerData *tidList;
	int			numTids;
	bool		bBackward;

	/*
	 * 从 tid 扫描节点中抽取必要信息       
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	snapshot = estate->es_snapshot;
	heapRelation = node->ss.ss_currentRelation;
	slot = node->ss.ss_ScanTupleSlot;

	/*
	 * 首次经过时，计算出要访问的 TID 列表        
	 */
	if (node->tss_TidList == NULL)
		TidListEval(node);

	scan = node->ss.ss_currentScanDesc;
	tidList = node->tss_TidList;
	numTids = node->tss_NumTids;

	/*
	 * 根据方向来初始化或推进扫描位置。            
	 */
	bBackward = ScanDirectionIsBackward(direction);
	if (bBackward)
	{
		if (node->tss_TidPtr < 0)
		{
			/* 为反向扫描做初始化  */
			node->tss_TidPtr = numTids - 1;
		}
		else
			node->tss_TidPtr--;
	}
	else
	{
		if (node->tss_TidPtr < 0)
		{
			/* 为前向扫描做初始化 */
			node->tss_TidPtr = 0;
		}
		else
			node->tss_TidPtr++;
	}

	while (node->tss_TidPtr >= 0 && node->tss_TidPtr < numTids)
	{
		ItemPointerData tid = tidList[node->tss_TidPtr];

		/*
		 * 对于 WHERE CURRENT OF，从游标取回的元组此后可能 
		 * 已被更新；若是，我们就应该取回在当前快照下    
		 * 仍然有效的那个版本。    
		 */
		if (node->tss_isCurrentOf)
			table_tuple_get_latest_tid(scan, &tid);

		if (table_tuple_fetch_row_version(heapRelation, &tid, snapshot, slot))
			return slot;

		/* 坏TID或快照qual失败；试下一个  */
		if (bBackward)
			node->tss_TidPtr--;
		else
			node->tss_TidPtr++;

		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * 走到这里就意味着 tid 扫描失败，即已到达扫描         
	 * 末端
	 */
	return ExecClearTuple(slot);
}

/*
 * TidRecheck -- 于 EvalPlanQual 中复查元组的访问方法例程   
 */
static bool
TidRecheck(TidScanState *node, TupleTableSlot *slot)
{
	ItemPointer match;

	/* WHERE CURRENT OF 总是想要定位到最新的那个元组    */
	if (node->tss_isCurrentOf)
		return true;

	if (node->tss_TidList == NULL)
		TidListEval(node);

	/*
	 * 二分查找 TidList，检查是否提到该 ctid，若是           
	 * 则返回真  
	 */
	match = (ItemPointer) bsearch(&slot->tts_tid, node->tss_TidList,
								  node->tss_NumTids, sizeof(ItemPointerData),
								  itemptr_comparator);
	return match != NULL;
}


/* ----------------------------------------------------------------
 *		ExecTidScan(node)
 *
 *		使用 tid 扫描关系，并返回      
 *		   指定方向上的下一个满足条件的元组。  
 *		我们调用 ExecScan() 例程，并传入相应的       
 *		访问方法函数。   
 *
 *		条件：  
 *		  -- 由 AMI 维护的"cursor"定位在该元组                  
 *			 即前次返回的  
 *
 *		初始状态：
 *		  -- 所指明的关系已为扫描而打开，从而         
 *			 "cursor"定位于第一个满足条件的元组之前。 
 *		  -- tss_TidPtr 为 -1
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecTidScan(PlanState *pstate)
{
	TidScanState *node = castNode(TidScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) TidNext,
					(ExecScanRecheckMtd) TidRecheck);
}

/* ----------------------------------------------------------------
 *		ExecReScanTidScan(node)
 * ----------------------------------------------------------------
 */
void
ExecReScanTidScan(TidScanState *node)
{
	if (node->tss_TidList)
		pfree(node->tss_TidList);
	node->tss_TidList = NULL;
	node->tss_NumTids = 0;
	node->tss_TidPtr = -1;

	/* 并非真正必要，但似乎是好习惯*/
	if (node->ss.ss_currentScanDesc)
		table_rescan(node->ss.ss_currentScanDesc, NULL);

	ExecScanReScan(&node->ss);
}

/* ----------------------------------------------------------------
 *		ExecEndTidScan
 *
 *		释放通过 C 例程分配的所有存储。     
 *		无返回值。 
 * ----------------------------------------------------------------
 */
void
ExecEndTidScan(TidScanState *node)
{
	if (node->ss.ss_currentScanDesc)
		table_endscan(node->ss.ss_currentScanDesc);

	/*
	 * 释放 exprcontext  
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * 清空元组表槽位      
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecInitTidScan
 *
 *		初始化 tid 扫描的状态信息，创建         
 *		扫描键，并打开基础关系与 tid 关系 
 *
 *		参数：  
 *		  node：规划器产生的 TidScan 节点。
 *		  estate：在 InitPlan 中初始化的执行状态   
 * ----------------------------------------------------------------
 */
TidScanState *
ExecInitTidScan(TidScan *node, EState *estate, int eflags)
{
	TidScanState *tidstate;
	Relation	currentRelation;

	/*
	 * 创建状态结构    
	 */
	tidstate = makeNode(TidScanState);
	tidstate->ss.ps.plan = (Plan *) node;
	tidstate->ss.ps.state = estate;
	tidstate->ss.ps.ExecProcNode = ExecTidScan;

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &tidstate->ss.ps);

	/*
	 * 标记 tid 列表为尚未计算 
	 */
	tidstate->tss_TidList = NULL;
	tidstate->tss_NumTids = 0;
	tidstate->tss_TidPtr = -1;

	/*
	 * 打开扫描关系    
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid, eflags);

	tidstate->ss.ss_currentRelation = currentRelation;
	tidstate->ss.ss_currentScanDesc = NULL; /* 此处无堆扫描*/

	/*
	 * 从关系描述符中获取扫描类型。     
	 */
	ExecInitScanTupleSlot(estate, &tidstate->ss,
						  RelationGetDescr(currentRelation),
						  table_slot_callbacks(currentRelation));

	/*
	 * 初始化结果类型与投影。     
	 */
	ExecInitResultTypeTL(&tidstate->ss.ps);
	ExecAssignScanProjectionInfo(&tidstate->ss);

	/*
	 * 初始化子表达式       
	 */
	tidstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) tidstate);

	TidExprListCreate(tidstate);

	/*
	 * 完成。
	 */
	return tidstate;
}
