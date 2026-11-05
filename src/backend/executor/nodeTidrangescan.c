/*-------------------------------------------------------------------------
 *
 * nodeTidrangescan.c
 *	  支持关系 TID 范围扫描的例程          
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeTidrangescan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "catalog/pg_operator.h"
#include "executor/execdebug.h"
#include "executor/nodeTidrangescan.h"
#include "nodes/nodeFuncs.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"


/*
 * 识别 CTID 变量时只需检查 varattno 即可，因为关系扫描 
 * qual 中的 Var 必然针对本表。（即便它是引用其他表  
 * CTID 的参数化扫描，该 Var 到达此处时也已变成            
 * Param。）                                             
 */
#define IsCTIDVar(node)  \
	((node) != NULL && \
	 IsA((node), Var) && \
	 ((Var *) (node))->varattno == SelfItemPointerAttributeNumber)

typedef enum
{
	TIDEXPR_UPPER_BOUND,
	TIDEXPR_LOWER_BOUND
} TidExprType;

/* 扫描的范围上界或下界      */
typedef struct TidOpExpr
{
	TidExprType exprtype;		/* op 类型；上界或下界*/
	ExprState  *exprstate;		/* TID 子表达式用的 ExprState     */
	bool		inclusive;		/* op 是否为包含      */
} TidOpExpr;

/*
 * 对于给定的表达式 expr，构建并返回合适的 TidOpExpr，      
 * 需考虑其操作符与操作数顺序。    
 */
static TidOpExpr *
MakeTidOpExpr(OpExpr *expr, TidRangeScanState *tidstate)
{
	Node	   *arg1 = get_leftop((Expr *) expr);
	Node	   *arg2 = get_rightop((Expr *) expr);
	ExprState  *exprstate = NULL;
	bool		invert = false;
	TidOpExpr  *tidopexpr;

	if (IsCTIDVar(arg1))
		exprstate = ExecInitExpr((Expr *) arg2, &tidstate->ss.ps);
	else if (IsCTIDVar(arg2))
	{
		exprstate = ExecInitExpr((Expr *) arg1, &tidstate->ss.ps);
		invert = true;
	}
	else
		elog(ERROR, "could not identify CTID variable");

	tidopexpr = (TidOpExpr *) palloc(sizeof(TidOpExpr));
	tidopexpr->inclusive = false;	/* 暂时  */

	switch (expr->opno)
	{
		case TIDLessEqOperator:
			tidopexpr->inclusive = true;
			/* fall through */
		case TIDLessOperator:
			tidopexpr->exprtype = invert ? TIDEXPR_LOWER_BOUND : TIDEXPR_UPPER_BOUND;
			break;
		case TIDGreaterEqOperator:
			tidopexpr->inclusive = true;
			/* fall through */
		case TIDGreaterOperator:
			tidopexpr->exprtype = invert ? TIDEXPR_UPPER_BOUND : TIDEXPR_LOWER_BOUND;
			break;
		default:
			elog(ERROR, "could not identify CTID operator");
	}

	tidopexpr->exprstate = exprstate;

	return tidopexpr;
}

/*
 * 提取 qual 中产出待搜索 TID 的子表达式，         
 * 若其为普通表达式则编译成 ExprState。                
 */
static void
TidExprListCreate(TidRangeScanState *tidrangestate)
{
	TidRangeScan *node = (TidRangeScan *) tidrangestate->ss.ps.plan;
	List	   *tidexprs = NIL;
	ListCell   *l;

	foreach(l, node->tidrangequals)
	{
		OpExpr	   *opexpr = lfirst(l);
		TidOpExpr  *tidopexpr;

		if (!IsA(opexpr, OpExpr))
			elog(ERROR, "could not identify CTID expression");

		tidopexpr = MakeTidOpExpr(opexpr, tidrangestate);
		tidexprs = lappend(tidexprs, tidopexpr);
	}

	tidrangestate->trss_tidexprs = tidexprs;
}

/* ----------------------------------------------------------------
 *		TidRangeEval
 *
 *		通过对 trss_tidexprs 求值，计算并设置节点扫描的    
 *		块号与偏移范围。若发现该范围不可能包含任何
 *		元组则返回 false；若范围可能包含元组则返回       
 *		true。        
 * ----------------------------------------------------------------
 */
static bool
TidRangeEval(TidRangeScanState *node)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	ItemPointerData lowerBound;
	ItemPointerData upperBound;
	ListCell   *l;

	/*
	 * 把上界和下界设为 ItemPointer 类型取值范围的绝对     
	 * 极限。随后我们会在两侧尝试借助 TidOpExpr 收窄该    
	 * 范围。                         
	 */
	ItemPointerSet(&lowerBound, 0, 0);
	ItemPointerSet(&upperBound, InvalidBlockNumber, PG_UINT16_MAX);

	foreach(l, node->trss_tidexprs)
	{
		TidOpExpr  *tidopexpr = (TidOpExpr *) lfirst(l);
		ItemPointer itemptr;
		bool		isNull;

		/* 求值此边界。   */
		itemptr = (ItemPointer)
			DatumGetPointer(ExecEvalExprSwitchContext(tidopexpr->exprstate,
													  econtext,
													  &isNull));

		/* 若边界为 NULL，则无元组能满足 qual。 */
		if (isNull)
			return false;

		if (tidopexpr->exprtype == TIDEXPR_LOWER_BOUND)
		{
			ItemPointerData lb;

			ItemPointerCopy(itemptr, &lb);

			/*
			 * 把非包含范围规范化为包含。此处得到     
			 * ItemPointer 可能不是有效的条目指针。           
			 */
			if (!tidopexpr->inclusive)
				ItemPointerInc(&lb);

			/* 检查能否用此 qual 收窄范围             */
			if (ItemPointerCompare(&lb, &lowerBound) > 0)
				ItemPointerCopy(&lb, &lowerBound);
		}

		else if (tidopexpr->exprtype == TIDEXPR_UPPER_BOUND)
		{
			ItemPointerData ub;

			ItemPointerCopy(itemptr, &ub);

			/*
			 * 把非包含范围规范化为包含。此处得到     
			 * ItemPointer 可能不是有效的条目指针。           
			 */
			if (!tidopexpr->inclusive)
				ItemPointerDec(&ub);

			/* 检查能否用此 qual 收窄范围             */
			if (ItemPointerCompare(&ub, &upperBound) < 0)
				ItemPointerCopy(&ub, &upperBound);
		}
	}

	ItemPointerCopy(&lowerBound, &node->trss_mintid);
	ItemPointerCopy(&upperBound, &node->trss_maxtid);

	return true;
}

/* ----------------------------------------------------------------
 *		TidRangeNext
 *
 *		使用 TidRangeScanState 信息中的 TID，从              
 *		TidRangeScan 的 currentRelation 中取回元组    
 *
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
TidRangeNext(TidRangeScanState *node)
{
	TableScanDesc scandesc;
	EState	   *estate;
	ScanDirection direction;
	TupleTableSlot *slot;

	/*
	 * 从 TID 扫描节点提取必要信息          
	 */
	scandesc = node->ss.ss_currentScanDesc;
	estate = node->ss.ps.state;
	slot = node->ss.ss_ScanTupleSlot;
	direction = estate->es_direction;

	if (!node->trss_inScan)
	{
		/* 首次执行时计算要扫描的 TID 范围  */
		if (!TidRangeEval(node))
			return NULL;

		if (scandesc == NULL)
		{
			scandesc = table_beginscan_tidrange(node->ss.ss_currentRelation,
												estate->es_snapshot,
												&node->trss_mintid,
												&node->trss_maxtid);
			node->ss.ss_currentScanDesc = scandesc;
		}
		else
		{
			/* 用新 TID 范围重新扫描     */
			table_rescan_tidrange(scandesc, &node->trss_mintid,
								  &node->trss_maxtid);
		}

		node->trss_inScan = true;
	}

	/* 取下一条元组。 */
	if (!table_scan_getnextslot_tidrange(scandesc, direction, slot))
	{
		node->trss_inScan = false;
		ExecClearTuple(slot);
	}

	return slot;
}

/*
 * TidRangeRecheck -- 在 EvalPlanQual 中复查元组的访问方法例程   
 */
static bool
TidRangeRecheck(TidRangeScanState *node, TupleTableSlot *slot)
{
	if (!TidRangeEval(node))
		return false;

	Assert(ItemPointerIsValid(&slot->tts_tid));

	/* 重新检查 ctid 是否仍在范围内*/
	if (ItemPointerCompare(&slot->tts_tid, &node->trss_mintid) < 0 ||
		ItemPointerCompare(&slot->tts_tid, &node->trss_maxtid) > 0)
		return false;

	return true;
}

/* ----------------------------------------------------------------
 *		ExecTidRangeScan(node)
 *
 *		使用 TID 扫描关系并返回下一条满足条件的元组。   
 *		我们调用 ExecScan() 例程并传给它合适的       
 *		访问方法函数。   
 *
 *		条件：  
 *		  -- AMI 维护的"游标"定位在先前返回的                
 *			 元组上。        
 *
 *		初始状态：
 *		  -- 指示的关系已为 TID 范围扫描而打开。      
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecTidRangeScan(PlanState *pstate)
{
	TidRangeScanState *node = castNode(TidRangeScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) TidRangeNext,
					(ExecScanRecheckMtd) TidRangeRecheck);
}

/* ----------------------------------------------------------------
 *		ExecReScanTidRangeScan(node)
 * ----------------------------------------------------------------
 */
void
ExecReScanTidRangeScan(TidRangeScanState *node)
{
	/* 标记扫描未进行中，且 TID 范围列表尚未计算          */
	node->trss_inScan = false;

	/*
	 * 必须等到 TidRangeNext 之后才能调用 table_rescan_tidrange。
	 */
	ExecScanReScan(&node->ss);
}

/* ----------------------------------------------------------------
 *		ExecEndTidRangeScan
 *
 *		释放通过 C 例程分配的所有存储。     
 *		无返回值。 
 * ----------------------------------------------------------------
 */
void
ExecEndTidRangeScan(TidRangeScanState *node)
{
	TableScanDesc scan = node->ss.ss_currentScanDesc;

	if (scan != NULL)
		table_endscan(scan);

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
 *		ExecInitTidRangeScan
 *
 *		初始化 TID 范围扫描状态信息，创建扫描      
 *		键，并打开扫描关系。         
 *
 *		参数：  
 *		  node: 规划器产生的 TidRangeScan 节点。 
 *		  estate: 在 InitPlan 中初始化的执行状态。 
 * ----------------------------------------------------------------
 */
TidRangeScanState *
ExecInitTidRangeScan(TidRangeScan *node, EState *estate, int eflags)
{
	TidRangeScanState *tidrangestate;
	Relation	currentRelation;

	/*
	 * 创建状态结构    
	 */
	tidrangestate = makeNode(TidRangeScanState);
	tidrangestate->ss.ps.plan = (Plan *) node;
	tidrangestate->ss.ps.state = estate;
	tidrangestate->ss.ps.ExecProcNode = ExecTidRangeScan;

	/*
	 * 其他初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &tidrangestate->ss.ps);

	/*
	 * 标记扫描未进行中，且 TID 范围尚未计算          
	 */
	tidrangestate->trss_inScan = false;

	/*
	 * 打开扫描关系    
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid, eflags);

	tidrangestate->ss.ss_currentRelation = currentRelation;
	tidrangestate->ss.ss_currentScanDesc = NULL;	/* 此处无表扫描 */

	/*
	 * 从关系描述符获取扫描类型。        
	 */
	ExecInitScanTupleSlot(estate, &tidrangestate->ss,
						  RelationGetDescr(currentRelation),
						  table_slot_callbacks(currentRelation));

	/*
	 * 初始化结果类型与投影。     
	 */
	ExecInitResultTypeTL(&tidrangestate->ss.ps);
	ExecAssignScanProjectionInfo(&tidrangestate->ss);

	/*
	 * 初始化子表达式       
	 */
	tidrangestate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) tidrangestate);

	TidExprListCreate(tidrangestate);

	/*
	 * 完成。
	 */
	return tidrangestate;
}
