/*-------------------------------------------------------------------------
 *
 * nodeFunctionscan.c
 *	  RangeFunction（rangetable 中的函数）扫描的支撑例程。     
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeFunctionscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *		ExecFunctionScan 扫描函数。   
 *		ExecFunctionNext 按顺序取回下一个元组。        
 *		ExecInitFunctionScan 创建并初始化 functionscan 节点。   
 *		ExecEndFunctionScan 释放所有已分配存储。  
 *		ExecReScanFunctionScan 重新扫描函数  
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/nodeFunctionscan.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/memutils.h"


/*
 * 每个被扫描函数的运行时数据。   
 */
typedef struct FunctionScanPerFuncState
{
	SetExprState *setexpr;		/* 正在求值的表达式状态          */
	TupleDesc	tupdesc;		/* 函数结果类型的描述      */
	int			colcount;		/* 预期的结果列数量          */
	Tuplestorestate *tstore;	/* 保存函数结果集         */
	int64		rowcount;		/* 结果集中的行数，未知则为 -1  */
	TupleTableSlot *func_slot;	/* 函数结果槽（或 NULL）  */
} FunctionScanPerFuncState;

static TupleTableSlot *FunctionNext(FunctionScanState *node);


/* ----------------------------------------------------------------
 *						扫描支持
 * ----------------------------------------------------------------
 */
/* ----------------------------------------------------------------
 *		FunctionNext
 *
 *		这是 ExecFunctionScan 的主力函数 
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
FunctionNext(FunctionScanState *node)
{
	EState	   *estate;
	ScanDirection direction;
	TupleTableSlot *scanslot;
	bool		alldone;
	int64		oldpos;
	int			funcno;
	int			att;

	/*
	 * 从 estate 和扫描状态中获取信息     
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	scanslot = node->ss.ss_ScanTupleSlot;

	if (node->simple)
	{
		/*
		 * 简单情形的快速路径：函数返回类型与扫描结果  
		 * 类型相同，所以我们将函数结果直接取到扫描      
		 * 结果槽中，因此也无需更新 ordinality 或       
		 * rowcount。      
		 */
		Tuplestorestate *tstore = node->funcstates[0].tstore;

		/*
		 * 首次执行时，从函数读取所有元组并放入           
		 * tuplestore。后续调用只需从该 tuplestore        
		 * 取元组  
		 */
		if (tstore == NULL)
		{
			node->funcstates[0].tstore = tstore =
				ExecMakeTableFunctionResult(node->funcstates[0].setexpr,
											node->ss.ps.ps_ExprContext,
											node->argcontext,
											node->funcstates[0].tupdesc,
											node->eflags & EXEC_FLAG_BACKWARD);

			/*
			 * 防范措施 - 若函数可能自行构建了                  
			 * tuplestore，却没有让它指向起始处，则需          
			 * 应对。调用很快，开销应该不成问题。 
			 */
			tuplestore_rescan(tstore);
		}

		/*
		 * 自 tuplestore 取下个元组。  
		 */
		(void) tuplestore_gettupleslot(tstore,
									   ScanDirectionIsForward(direction),
									   false,
									   scanslot);
		return scanslot;
	}

	/*
	 * 在检查数据结束前递增或递减序数计数器，              
	 * 这样我们可以离开结果的任一端 1（且最多               
	 * 1）而不丢失正确计数。参见 PortalRunSelect               
	 * 了解为何可假定不会在数据结束状态被反复调用。 
	 */
	oldpos = node->ordinal;
	if (ScanDirectionIsForward(direction))
		node->ordinal++;
	else
		node->ordinal--;

	/*
	 * 对函数的主循环。 
	 *
	 * 我们把函数结果取入 func_slots（它与函数返回类型      
	 * 匹配），然后将值复制到 scanslot（它与扫描结果类型 
	 * 匹配），并同时设置序数列（如果有）。         
	 */
	ExecClearTuple(scanslot);
	att = 0;
	alldone = true;
	for (funcno = 0; funcno < node->nfuncs; funcno++)
	{
		FunctionScanPerFuncState *fs = &node->funcstates[funcno];
		int			i;

		/*
		 * 首次执行时，从函数读取所有元组并放入           
		 * tuplestore。后续调用只需从该 tuplestore        
		 * 取元组  
		 */
		if (fs->tstore == NULL)
		{
			fs->tstore =
				ExecMakeTableFunctionResult(fs->setexpr,
											node->ss.ps.ps_ExprContext,
											node->argcontext,
											fs->tupdesc,
											node->eflags & EXEC_FLAG_BACKWARD);

			/*
			 * 防范措施 - 若函数可能自行构建了                  
			 * tuplestore，却没有让它指向起始处，则需          
			 * 应对。调用很快，开销应该不成问题。 
			 */
			tuplestore_rescan(fs->tstore);
		}

		/*
		 * 自 tuplestore 取下个元组。  
		 *
		 * 如果我们有该函数的行计数，且已知上一次读取 
		 * 位置越界，则不要尝试读取。这样当存在混合行 
		 * 计数时，向后扫描也能工作。                       
		 */
		if (fs->rowcount != -1 && fs->rowcount < oldpos)
			ExecClearTuple(fs->func_slot);
		else
			(void) tuplestore_gettupleslot(fs->tstore,
										   ScanDirectionIsForward(direction),
										   false,
										   fs->func_slot);

		if (TupIsNull(fs->func_slot))
		{
			/*
			 * 如果向前方向已耗尽该函数的数据，那么
			 * 我们现在知道它返回了多少行。我们需要知道 
			 * 这一点以便处理向后扫描。我们存储的行计数  
			 * 实际是真实数字加 1，因为有时我们必须把     
			 * tuplestore 定位到其末尾之外 1 处。    
			 */
			if (ScanDirectionIsForward(direction) && fs->rowcount == -1)
				fs->rowcount = node->ordinal;

			/*
			 * 用 NULL 填充结果列           
			 */
			for (i = 0; i < fs->colcount; i++)
			{
				scanslot->tts_values[att] = (Datum) 0;
				scanslot->tts_isnull[att] = true;
				att++;
			}
		}
		else
		{
			/*
			 * 已有结果时，只需将它复制到结果列。  
			 */
			slot_getallattrs(fs->func_slot);

			for (i = 0; i < fs->colcount; i++)
			{
				scanslot->tts_values[att] = fs->func_slot->tts_values[i];
				scanslot->tts_isnull[att] = fs->func_slot->tts_isnull[i];
				att++;
			}

			/*
			 * 直到每个函数结果都耗尽才算完成；在那之前   
			 * 我们用 NULL 填充较短的结果。   
			 */
			alldone = false;
		}
	}

	/*
	 * 按规范，序数列总在最后。 
	 */
	if (node->ordinality)
	{
		scanslot->tts_values[att] = Int64GetDatumFast(node->ordinal);
		scanslot->tts_isnull[att] = false;
	}

	/*
	 * 若 alldone，我们只需返回之前已清空的 scanslot。         
	 * 否则完成虚拟元组构建。 
	 */
	if (!alldone)
		ExecStoreVirtualTuple(scanslot);

	return scanslot;
}

/*
 * FunctionRecheck -- EvalPlanQual 中重新检查元组的访问方法例程 
 */
static bool
FunctionRecheck(FunctionScanState *node, TupleTableSlot *slot)
{
	/* 无需检查     */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecFunctionScan(node)
 *
 *		按顺序扫描函数并返回下一个满足条件的         
 *		元组
 *		我们调用 ExecScan() 例程，并向它传入适当的 
 *		访问方法函数。   
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecFunctionScan(PlanState *pstate)
{
	FunctionScanState *node = castNode(FunctionScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) FunctionNext,
					(ExecScanRecheckMtd) FunctionRecheck);
}

/* ----------------------------------------------------------------
 *		ExecInitFunctionScan
 * ----------------------------------------------------------------
 */
FunctionScanState *
ExecInitFunctionScan(FunctionScan *node, EState *estate, int eflags)
{
	FunctionScanState *scanstate;
	int			nfuncs = list_length(node->functions);
	TupleDesc	scan_tupdesc;
	int			i,
				natts;
	ListCell   *lc;

	/* 检查不支持的标志    */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * FunctionScan 不应有任何子节点。  
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * 为节点创建 ScanState    
	 */
	scanstate = makeNode(FunctionScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecFunctionScan;
	scanstate->eflags = eflags;

	/*
	 * 是否要添加 ordinality 列？  
	 */
	scanstate->ordinality = node->funcordinality;

	scanstate->nfuncs = nfuncs;
	if (nfuncs == 1 && !node->funcordinality)
		scanstate->simple = true;
	else
		scanstate->simple = false;

	/*
	 * 序数 0 表示"第一行之前"的位置。             
	 *
	 * 即使不向结果添加 ordinality 列，我们也需要跟踪     
	 * 序数位置，以便正确处理结果大小不同的多个        
	 * 函数的向后扫描。（我们不能把任何单个函数的        
	 * tuplestore 定位到超出其末尾 1 处以上，                   
	 * 所以在向后扫描时，我们需要知道何时重新     
	 * 把该函数纳入扫描。）            
	 */
	scanstate->ordinal = 0;

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	scanstate->funcstates = palloc(nfuncs * sizeof(FunctionScanPerFuncState));

	natts = 0;
	i = 0;
	foreach(lc, node->functions)
	{
		RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);
		Node	   *funcexpr = rtfunc->funcexpr;
		int			colcount = rtfunc->funccolcount;
		FunctionScanPerFuncState *fs = &scanstate->funcstates[i];
		TupleDesc	tupdesc;

		fs->setexpr =
			ExecInitTableFunctionResult((Expr *) funcexpr,
										scanstate->ss.ps.ps_ExprContext,
										&scanstate->ss.ps);

		/*
		 * 不要分配 tuplestore；函数的实际调用会分配。      
		 * NULL 表示我们尚未调用该函数（或在重新扫描      
		 * 之后需要再次调用它）。     
		 */
		fs->tstore = NULL;
		fs->rowcount = -1;

		/*
		 * 现在构建一个 tupdesc，表示我们期望函数返回   
		 * 的结果类型。如果有 coldeflist 则优先用它（注意  
		 * 若函数的名义输出类型是 RECORD，解析器会强制  
		 * 要求存在一个）。否则用 get_expr_result_type。   
		 *
		 * 注意，如果函数返回命名的复合类型，那么它现在
		 * 包含的列可能比制定计划时更多或有所不同。对于 
		 * 这种情况和 RECORD 情况，我们都需要检查元组     
		 * 兼容性。其中一部分由 ExecMakeTableFunctionResult       
		 * 处理，CheckVarSlotCompatibility 提供兜底。
		 */
		if (rtfunc->funccolnames != NIL)
		{
			tupdesc = BuildDescFromLists(rtfunc->funccolnames,
										 rtfunc->funccoltypes,
										 rtfunc->funccoltypmods,
										 rtfunc->funccolcollations);

			/*
			 * 对于 RECORD 结果，确保已为它分配 typmod。（      
			 * 函数本应自己完成，但我们不妨加以防备，    
			 * 以防万一）  
			 */
			BlessTupleDesc(tupdesc);
		}
		else
		{
			TypeFuncClass functypclass;
			Oid			funcrettype;

			functypclass = get_expr_result_type(funcexpr,
												&funcrettype,
												&tupdesc);

			if (functypclass == TYPEFUNC_COMPOSITE ||
				functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
			{
				/* 复合数据类型，例如表的行类型   */
				Assert(tupdesc);
				/* 为安全起见必须从 typcache 复制*/
				tupdesc = CreateTupleDescCopy(tupdesc);
			}
			else if (functypclass == TYPEFUNC_SCALAR)
			{
				/* 基础类型，即标量    */
				tupdesc = CreateTemplateTupleDesc(1);
				TupleDescInitEntry(tupdesc,
								   (AttrNumber) 1,
								   NULL,	/* 此处不关心名称          */
								   funcrettype,
								   -1,
								   0);
				TupleDescInitEntryCollation(tupdesc,
											(AttrNumber) 1,
											exprCollation(funcexpr));
			}
			else
			{
				/* 拙劣的错误消息，但解析器本应已捕获它   */
				elog(ERROR, "function in FROM has unsupported return type");
			}
		}

		fs->tupdesc = tupdesc;
		fs->colcount = colcount;

		/*
		 * 仅当使用 ordinality 或多个函数时，我们才需要  
		 * 为函数结果准备单独的槽；否则我们会把函数  
		 * 结果直接取到扫描槽中。            
		 */
		if (!scanstate->simple)
		{
			fs->func_slot = ExecInitExtraTupleSlot(estate, fs->tupdesc,
												   &TTSOpsMinimalTuple);
		}
		else
			fs->func_slot = NULL;

		natts += colcount;
		i++;
	}

	/*
	 * 创建组合的 TupleDesc    
	 *
	 * 如果只有一个函数且没有 ordinality，扫描结果的   
	 * tupdesc 与函数结果 tupdesc 相同 --- 只是我们稍后        
	 * 可能向其中填入新名称，故丢弃行类型标签。 
	 */
	if (scanstate->simple)
	{
		scan_tupdesc = CreateTupleDescCopy(scanstate->funcstates[0].tupdesc);
		scan_tupdesc->tdtypeid = RECORDOID;
		scan_tupdesc->tdtypmod = -1;
	}
	else
	{
		AttrNumber	attno = 0;

		if (node->funcordinality)
			natts++;

		scan_tupdesc = CreateTemplateTupleDesc(natts);

		for (i = 0; i < nfuncs; i++)
		{
			TupleDesc	tupdesc = scanstate->funcstates[i].tupdesc;
			int			colcount = scanstate->funcstates[i].colcount;
			int			j;

			for (j = 1; j <= colcount; j++)
				TupleDescCopyEntry(scan_tupdesc, ++attno, tupdesc, j);
		}

		/* 如果使用 ordinality，则在末尾添加 "bigint" 列     */
		if (node->funcordinality)
		{
			TupleDescInitEntry(scan_tupdesc,
							   ++attno,
							   NULL,	/* 此处不关心名称          */
							   INT8OID,
							   -1,
							   0);
		}

		Assert(attno == natts);
	}

	/*
	 * 初始化扫描槽和类型。
	 */
	ExecInitScanTupleSlot(estate, &scanstate->ss, scan_tupdesc,
						  &TTSOpsMinimalTuple);

	/*
	 * 初始化结果槽、类型和投影。     
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * 初始化子表达式       
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	/*
	 * 创建一个 ExecMakeTableFunctionResult 可用来在其中        
	 * 求值函数参数的内存上下文。我们无法为此使用        
	 * 每个元组上下文，因为它重置得太频繁；但我们
	 * 也不想把求值结果泄漏到查询生命期上下文中。    
	 * 我们只需一个上下文，因为每个函数单独求值。
	 */
	scanstate->argcontext = AllocSetContextCreate(CurrentMemoryContext,
												  "Table function arguments",
												  ALLOCSET_DEFAULT_SIZES);

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndFunctionScan
 *
 *		释放通过 C 例程分配的所有存储。  
 * ----------------------------------------------------------------
 */
void
ExecEndFunctionScan(FunctionScanState *node)
{
	int			i;

	/*
	 * 释放 exprcontext  
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * 清空元组表          
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * 释放槽和 tuplestore 资源        
	 */
	for (i = 0; i < node->nfuncs; i++)
	{
		FunctionScanPerFuncState *fs = &node->funcstates[i];

		if (fs->func_slot)
			ExecClearTuple(fs->func_slot);

		if (fs->tstore != NULL)
		{
			tuplestore_end(node->funcstates[i].tstore);
			fs->tstore = NULL;
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecReScanFunctionScan
 *
 *		重新扫描关系。
 * ----------------------------------------------------------------
 */
void
ExecReScanFunctionScan(FunctionScanState *node)
{
	FunctionScan *scan = (FunctionScan *) node->ss.ps.plan;
	int			i;
	Bitmapset  *chgparam = node->ss.ps.chgParam;

	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	for (i = 0; i < node->nfuncs; i++)
	{
		FunctionScanPerFuncState *fs = &node->funcstates[i];

		if (fs->func_slot)
			ExecClearTuple(fs->func_slot);
	}

	ExecScanReScan(&node->ss);

	/*
	 * 这里我们可以选择是丢弃 tuplestore（并重新计算      
	 * 函数输出）还是只重新扫描它们。如果表达式包含 
	 * 已更改参数则必须重算，否则重新扫描。 
	 *
	 * XXX 若函数是 volatile，也许我们应重新计算？        
	 * 但一般来说，执行器不会将其行为以此为条件。 
	 */
	if (chgparam)
	{
		ListCell   *lc;

		i = 0;
		foreach(lc, scan->functions)
		{
			RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);

			if (bms_overlap(chgparam, rtfunc->funcparams))
			{
				if (node->funcstates[i].tstore != NULL)
				{
					tuplestore_end(node->funcstates[i].tstore);
					node->funcstates[i].tstore = NULL;
				}
				node->funcstates[i].rowcount = -1;
			}
			i++;
		}
	}

	/* 重置序数计数器    */
	node->ordinal = 0;

	/* 确保我们回退所有剩余的 tuplestore  */
	for (i = 0; i < node->nfuncs; i++)
	{
		if (node->funcstates[i].tstore != NULL)
			tuplestore_rescan(node->funcstates[i].tstore);
	}
}
