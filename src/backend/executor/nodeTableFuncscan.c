/*-------------------------------------------------------------------------
 *
 * nodeTableFuncscan.c
 *	  扫描 RangeTableFunc（类 XMLTABLE 函数）的支撑例程。       
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeTableFuncscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *		ExecTableFuncScan - 扫描函数    
 *		ExecFunctionNext - 按顺序取下一个元组            
 *		ExecInitTableFuncScan - 创建并初始化 TableFuncscan 节点    
 *		ExecEndTableFuncScan - 释放所有已分配存储   
 *		ExecReScanTableFuncScan 重新扫描函数  
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeTableFuncscan.h"
#include "executor/tablefunc.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/xml.h"

static TupleTableSlot *TableFuncNext(TableFuncScanState *node);
static bool TableFuncRecheck(TableFuncScanState *node, TupleTableSlot *slot);

static void tfuncFetchRows(TableFuncScanState *tstate, ExprContext *econtext);
static void tfuncInitialize(TableFuncScanState *tstate, ExprContext *econtext, Datum doc);
static void tfuncLoadRows(TableFuncScanState *tstate, ExprContext *econtext);

/* ----------------------------------------------------------------
 *						扫描支持
 * ----------------------------------------------------------------
 */
/* ----------------------------------------------------------------
 *		TableFuncNext
 *
 *		这是 ExecTableFuncScan 的主力函数 
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
TableFuncNext(TableFuncScanState *node)
{
	TupleTableSlot *scanslot;

	scanslot = node->ss.ss_ScanTupleSlot;

	/*
	 * 第一次经过时，从函数读取所有元组并把它们放入    
	 * tuplestore。后续调用只需从 tuplestore 取元组。     
	 */
	if (node->tupstore == NULL)
		tfuncFetchRows(node, node->ss.ps.ps_ExprContext);

	/*
	 * 从 tuplestore 取下一元组。  
	 */
	(void) tuplestore_gettupleslot(node->tupstore,
								   true,
								   false,
								   scanslot);
	return scanslot;
}

/*
 * TableFuncRecheck -- 在 EvalPlanQual 中重查元组的访问方法例程   
 */
static bool
TableFuncRecheck(TableFuncScanState *node, TupleTableSlot *slot)
{
	/* 无需检查     */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecTableFuncScan(node)
 *
 *		顺序扫描该函数，返回下一个合格的               
 *		元组
 *		我们调用 ExecScan() 例程，并向其传入适当的 
 *		访问方法函数。   
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecTableFuncScan(PlanState *pstate)
{
	TableFuncScanState *node = castNode(TableFuncScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) TableFuncNext,
					(ExecScanRecheckMtd) TableFuncRecheck);
}

/* ----------------------------------------------------------------
 *		ExecInitTableFuncScan
 * ----------------------------------------------------------------
 */
TableFuncScanState *
ExecInitTableFuncScan(TableFuncScan *node, EState *estate, int eflags)
{
	TableFuncScanState *scanstate;
	TableFunc  *tf = node->tablefunc;
	TupleDesc	tupdesc;
	int			i;

	/* 检查不支持的标志    */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * TableFuncscan 不应有任何子节点。  
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * 为节点新建 ScanState    
	 */
	scanstate = makeNode(TableFuncScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecTableFuncScan;

	/*
	 * 其他初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * 初始化源元组类型    
	 */
	tupdesc = BuildDescFromLists(tf->colnames,
								 tf->coltypes,
								 tf->coltypmods,
								 tf->colcollations);
	/* 以及相应的扫描槽        */
	ExecInitScanTupleSlot(estate, &scanstate->ss, tupdesc,
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
		ExecInitQual(node->scan.plan.qual, &scanstate->ss.ps);

	/* 目前仅支持 XMLTABLE             */
	scanstate->routine = &XmlTableRoutine;

	scanstate->perTableCxt =
		AllocSetContextCreate(CurrentMemoryContext,
							  "TableFunc per value context",
							  ALLOCSET_DEFAULT_SIZES);
	scanstate->opaque = NULL;	/* 在运行时初始化  */

	scanstate->ns_names = tf->ns_names;

	scanstate->ns_uris =
		ExecInitExprList(tf->ns_uris, (PlanState *) scanstate);
	scanstate->docexpr =
		ExecInitExpr((Expr *) tf->docexpr, (PlanState *) scanstate);
	scanstate->rowexpr =
		ExecInitExpr((Expr *) tf->rowexpr, (PlanState *) scanstate);
	scanstate->colexprs =
		ExecInitExprList(tf->colexprs, (PlanState *) scanstate);
	scanstate->coldefexprs =
		ExecInitExprList(tf->coldefexprs, (PlanState *) scanstate);

	scanstate->notnulls = tf->notnulls;

	/* 这些现在分配，稍后初始化          */
	scanstate->in_functions = palloc(sizeof(FmgrInfo) * tupdesc->natts);
	scanstate->typioparams = palloc(sizeof(Oid) * tupdesc->natts);

	/*
	 * 填写必要的 fmgr 信息。   
	 */
	for (i = 0; i < tupdesc->natts; i++)
	{
		Oid			in_funcid;

		getTypeInputInfo(TupleDescAttr(tupdesc, i)->atttypid,
						 &in_funcid, &scanstate->typioparams[i]);
		fmgr_info(in_funcid, &scanstate->in_functions[i]);
	}

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndTableFuncScan
 *
 *		释放在 C 例程中分配的所有存储。  
 * ----------------------------------------------------------------
 */
void
ExecEndTableFuncScan(TableFuncScanState *node)
{
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
	 * 释放 tuplestore 资源    
	 */
	if (node->tupstore != NULL)
		tuplestore_end(node->tupstore);
	node->tupstore = NULL;
}

/* ----------------------------------------------------------------
 *		ExecReScanTableFuncScan
 *
 *		重新扫描该关系
 * ----------------------------------------------------------------
 */
void
ExecReScanTableFuncScan(TableFuncScanState *node)
{
	Bitmapset  *chgparam = node->ss.ps.chgParam;

	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecScanReScan(&node->ss);

	/*
	 * 参数变化时重新计算。        
	 */
	if (chgparam)
	{
		if (node->tupstore != NULL)
		{
			tuplestore_end(node->tupstore);
			node->tupstore = NULL;
		}
	}

	if (node->tupstore != NULL)
		tuplestore_rescan(node->tupstore);
}

/* ----------------------------------------------------------------
 *		tfuncFetchRows
 *
 *		从 TableFunc 生产者读取行   
 * ----------------------------------------------------------------
 */
static void
tfuncFetchRows(TableFuncScanState *tstate, ExprContext *econtext)
{
	const TableFuncRoutine *routine = tstate->routine;
	MemoryContext oldcxt;
	Datum		value;
	bool		isnull;

	Assert(tstate->opaque == NULL);

	/* 为结果构建 tuplestore      */
	oldcxt = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
	tstate->tupstore = tuplestore_begin_heap(false, false, work_mem);

	/*
	 * 每次取一组新行的调用——若在 LATERAL 连接中使用    
	 * XMLTABLE 则可能有很多组——都可能分配相当多的       
	 * 内存，因此我们无法在这里使用每查询上下文。     
	 * perTableCxt 现在起着与 FunctionScan 中"argcontext"              
	 * 相同的作用——存放每次调用(即一个结果表)生存期
	 * 数据(而非每查询或每结果元组)。                
	 */
	MemoryContextSwitchTo(tstate->perTableCxt);

	PG_TRY();
	{
		routine->InitOpaque(tstate,
							tstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts);

		/*
		 * 若文档表达式的求值返回 NULL，则表表达式为  
		 * 空，我们立即返回。                   
		 */
		value = ExecEvalExpr(tstate->docexpr, econtext, &isnull);

		if (!isnull)
		{
			/* 否则，把文档值传给表构建器                 */
			tfuncInitialize(tstate, econtext, value);

			/* 初始化 ordinality 计数器*/
			tstate->ordinal = 1;

			/* 把所有行载入 tuplestore，之后就完成了*/
			tfuncLoadRows(tstate, econtext);
		}
	}
	PG_CATCH();
	{
		if (tstate->opaque != NULL)
			routine->DestroyOpaque(tstate);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* 清理并返回原来的内存上下文        */

	if (tstate->opaque != NULL)
	{
		routine->DestroyOpaque(tstate);
		tstate->opaque = NULL;
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(tstate->perTableCxt);
}

/*
 * 在表表达式构建器上下文中填入命名空间声明、        
 * 行过滤器以及列过滤器。
 */
static void
tfuncInitialize(TableFuncScanState *tstate, ExprContext *econtext, Datum doc)
{
	const TableFuncRoutine *routine = tstate->routine;
	TupleDesc	tupdesc;
	ListCell   *lc1,
			   *lc2;
	bool		isnull;
	int			colno;
	Datum		value;
	int			ordinalitycol =
		((TableFuncScan *) (tstate->ss.ps.plan))->tablefunc->ordinalitycol;

	/*
	 * 把文档作为可能被 TOAST 的 Datum 安装到 tablefunc 上下
	 * 文中  
	 */
	routine->SetDocument(tstate, doc);

	/* 求值命名空间规格说明    */
	forboth(lc1, tstate->ns_uris, lc2, tstate->ns_names)
	{
		ExprState  *expr = (ExprState *) lfirst(lc1);
		String	   *ns_node = lfirst_node(String, lc2);
		char	   *ns_uri;
		char	   *ns_name;

		value = ExecEvalExpr((ExprState *) expr, econtext, &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("namespace URI must not be null")));
		ns_uri = TextDatumGetCString(value);

		/* DEFAULT 作为 NULL 向下传给 SetNamespace  */
		ns_name = ns_node ? strVal(ns_node) : NULL;

		routine->SetNamespace(tstate, ns_name, ns_uri);
	}

	/* 把行过滤器表达式安装到表构建器上下文中        */
	value = ExecEvalExpr(tstate->rowexpr, econtext, &isnull);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("row filter expression must not be null")));

	routine->SetRowFilter(tstate, TextDatumGetCString(value));

	/*
	 * 把列过滤器表达式安装到表构建器上下文中。         
	 * 如果给定了表达式，就使用它；否则列名本身         
	 * 就是列过滤器。
	 */
	colno = 0;
	tupdesc = tstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	foreach(lc1, tstate->colexprs)
	{
		char	   *colfilter;
		Form_pg_attribute att = TupleDescAttr(tupdesc, colno);

		if (colno != ordinalitycol)
		{
			ExprState  *colexpr = lfirst(lc1);

			if (colexpr != NULL)
			{
				value = ExecEvalExpr(colexpr, econtext, &isnull);
				if (isnull)
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("column filter expression must not be null"),
							 errdetail("Filter for column \"%s\" is null.",
									   NameStr(att->attname))));
				colfilter = TextDatumGetCString(value);
			}
			else
				colfilter = NameStr(att->attname);

			routine->SetColumnFilter(tstate, colfilter, colno);
		}

		colno++;
	}
}

/*
 * 把来自 TableFunc 表构建器的所有行载入 tuplestore。     
 */
static void
tfuncLoadRows(TableFuncScanState *tstate, ExprContext *econtext)
{
	const TableFuncRoutine *routine = tstate->routine;
	TupleTableSlot *slot = tstate->ss.ss_ScanTupleSlot;
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	Datum	   *values = slot->tts_values;
	bool	   *nulls = slot->tts_isnull;
	int			natts = tupdesc->natts;
	MemoryContext oldcxt;
	int			ordinalitycol;

	ordinalitycol =
		((TableFuncScan *) (tstate->ss.ps.plan))->tablefunc->ordinalitycol;

	/*
	 * 我们需要一个短命的内存上下文，可在每轮循环时 
	 * 清理它，以免浪费空间。我们默认的每元组上下文    
	 * 完全能胜任此任务，因为在本次元组周期中我们     
	 * 未用过它。  
	 */
	oldcxt = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * 不断向表构建器请求行，直到再也没有行为止。    
	 */
	while (routine->FetchRow(tstate))
	{
		ListCell   *cell = list_head(tstate->coldefexprs);
		int			colno;

		CHECK_FOR_INTERRUPTS();

		ExecClearTuple(tstate->ss.ss_ScanTupleSlot);

		/*
		 * 取得此行各列的值，并把它们安装到槽中；         
		 * 然后把该元组加入 tuplestore。         
		 */
		for (colno = 0; colno < natts; colno++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, colno);

			if (colno == ordinalitycol)
			{
				/* ordinality 列的快速路径   */
				values[colno] = Int32GetDatum(tstate->ordinal++);
				nulls[colno] = false;
			}
			else
			{
				bool		isnull;

				values[colno] = routine->GetValue(tstate,
												  colno,
												  att->atttypid,
												  att->atttypmod,
												  &isnull);

				/* 没有值？对默认值(如有)求值并应用   */
				if (isnull && cell != NULL)
				{
					ExprState  *coldefexpr = (ExprState *) lfirst(cell);

					if (coldefexpr != NULL)
						values[colno] = ExecEvalExpr(coldefexpr, econtext,
													 &isnull);
				}

				/* 校验可能的 NOT NULL 约束       */
				if (isnull && bms_is_member(colno, tstate->notnulls))
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("null is not allowed in column \"%s\"",
									NameStr(att->attname))));

				nulls[colno] = isnull;
			}

			/* 推进默认表达式列表         */
			if (cell != NULL)
				cell = lnext(tstate->coldefexprs, cell);
		}

		tuplestore_putvalues(tstate->tupstore, tupdesc, values, nulls);

		MemoryContextReset(econtext->ecxt_per_tuple_memory);
	}

	MemoryContextSwitchTo(oldcxt);
}
