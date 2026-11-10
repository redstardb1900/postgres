/*-------------------------------------------------------------------------
 *
 * nodeIndexscan.c
 *	  支撑关系索引扫描的例程。          
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeIndexscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *		ExecIndexScan 用索引扫描关系            
 *		IndexNext 用索引取回下一个元组    
 *		IndexNextWithReorder 同上，但重查 ORDER BY 表达式 
 *		ExecInitIndexScan 创建并初始化状态信息      
 *		ExecReScanIndexScan 重扫被索引的关系      
 *		ExecEndIndexScan 释放所有存储。 
 *		ExecIndexMarkPos 标记扫描位置。
 *		ExecIndexRestrPos 恢复扫描位置。   
 *		ExecIndexScanEstimate 估算并行索引扫描所需的 DSM 空间      
 *		ExecIndexScanInitializeDSM 为并行索引扫描初始化 DSM   
 *		ExecIndexScanReInitializeDSM 重置 DSM 以备全新扫描  
 *		ExecIndexScanInitializeWorker 在并行 worker 中附着 DSM 信息
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "catalog/pg_am.h"
#include "executor/execdebug.h"
#include "executor/nodeIndexscan.h"
#include "lib/pairingheap.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * 使用排序算子时，从索引中取出且需要重排序的元组
 * 会作为 ReorderTuples 排入配对堆中。                       
 */
typedef struct
{
	pairingheap_node ph_node;
	HeapTuple	htup;
	Datum	   *orderbyvals;
	bool	   *orderbynulls;
} ReorderTuple;

static TupleTableSlot *IndexNext(IndexScanState *node);
static TupleTableSlot *IndexNextWithReorder(IndexScanState *node);
static void EvalOrderByExpressions(IndexScanState *node, ExprContext *econtext);
static bool IndexRecheck(IndexScanState *node, TupleTableSlot *slot);
static int	cmp_orderbyvals(const Datum *adist, const bool *anulls,
							const Datum *bdist, const bool *bnulls,
							IndexScanState *node);
static int	reorderqueue_cmp(const pairingheap_node *a,
							 const pairingheap_node *b, void *arg);
static void reorderqueue_push(IndexScanState *node, TupleTableSlot *slot,
							  Datum *orderbyvals, bool *orderbynulls);
static HeapTuple reorderqueue_pop(IndexScanState *node);


/* ----------------------------------------------------------------
 *		IndexNext
 *
 *		从 IndexScan 节点的 currentRelation 中取回元组   
 *		所用的索引由 IndexScanState 信息指定。           
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
IndexNext(IndexScanState *node)
{
	EState	   *estate;
	ExprContext *econtext;
	ScanDirection direction;
	IndexScanDesc scandesc;
	TupleTableSlot *slot;

	/*
	 * 从索引扫描节点中提取必要的信息     
	 */
	estate = node->ss.ps.state;

	/*
	 * 根据计划的扫描方向和当前执行方向，                    
	 * 确定应沿哪个方向扫描索引。          
	 */
	direction = ScanDirectionCombine(estate->es_direction,
									 ((IndexScan *) node->ss.ps.plan)->indexorderdir);
	scandesc = node->iss_ScanDesc;
	econtext = node->ss.ps.ps_ExprContext;
	slot = node->ss.ss_ScanTupleSlot;

	if (scandesc == NULL)
	{
		/*
		 * 若索引扫描并非并行，或者我们正在串行执行
		 * 一个被规划为并行的索引扫描，就会走到这里。  
		 */
		scandesc = index_beginscan(node->ss.ss_currentRelation,
								   node->iss_RelationDesc,
								   estate->es_snapshot,
								   node->iss_NumScanKeys,
								   node->iss_NumOrderByKeys);

		node->iss_ScanDesc = scandesc;

		/*
		 * 若没有需要计算的运行时键，或它们已就绪，    
		 * 就把扫描键传给索引 AM。 
		 */
		if (node->iss_NumRuntimeKeys == 0 || node->iss_RuntimeKeysReady)
			index_rescan(scandesc,
						 node->iss_ScanKeys, node->iss_NumScanKeys,
						 node->iss_OrderByKeys, node->iss_NumOrderByKeys);
	}

	/*
	 * 好，既然所需已齐，就取下一个元组。     
	 */
	while (index_getnext_slot(scandesc, direction, slot))
	{
		CHECK_FOR_INTERRUPTS();

		/*
		 * 若索引是有损的，就必须用取到的元组重查       
		 * 索引条件。   
		 */
		if (scandesc->xs_recheck)
		{
			econtext->ecxt_scantuple = slot;
			if (!ExecQualAndReset(node->indexqualorig, econtext))
			{
				/* 重查未过，丢弃该元组再取下一个       */
				InstrCountFiltered2(node, 1);
				continue;
			}
		}

		return slot;
	}

	/*
	 * 走到这里说明索引扫描失败，本次扫描已到            
	 * 尽头。 
	 */
	node->iss_ReachedEnd = true;
	return ExecClearTuple(slot);
}

/* ----------------------------------------------------------------
 *		IndexNextWithReorder
 *
 *		与 IndexNext 类似，但此版本还能重查            
 *		ORDER BY 表达式，并按需重排元组。    
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
IndexNextWithReorder(IndexScanState *node)
{
	EState	   *estate;
	ExprContext *econtext;
	IndexScanDesc scandesc;
	TupleTableSlot *slot;
	ReorderTuple *topmost = NULL;
	bool		was_exact;
	Datum	   *lastfetched_vals;
	bool	   *lastfetched_nulls;
	int			cmp;

	estate = node->ss.ps.state;

	/*
	 * 重排序只支持正向扫描。注意：这里仅用 Assert         
	 * 来断言就够了，因为只要 ExecSupportsBackwardScan()       
	 * 说行不通，系统就不会尝试反向运行该计划。    
	 * 目前这一点有保证，因为无索引 AM 同时支持     
	 * amcanorderbyop/amcanbackward；一旦两者兼具
	 * 时，ExecSupportsBackwardScan() 需显式考虑 indexorderbys
	 * 。        
	 */
	Assert(!ScanDirectionIsBackward(((IndexScan *) node->ss.ps.plan)->indexorderdir));
	Assert(ScanDirectionIsForward(estate->es_direction));

	scandesc = node->iss_ScanDesc;
	econtext = node->ss.ps.ps_ExprContext;
	slot = node->ss.ss_ScanTupleSlot;

	if (scandesc == NULL)
	{
		/*
		 * 若索引扫描并非并行，或者我们正在串行执行
		 * 一个被规划为并行的索引扫描，就会走到这里。  
		 */
		scandesc = index_beginscan(node->ss.ss_currentRelation,
								   node->iss_RelationDesc,
								   estate->es_snapshot,
								   node->iss_NumScanKeys,
								   node->iss_NumOrderByKeys);

		node->iss_ScanDesc = scandesc;

		/*
		 * 若没有需要计算的运行时键，或它们已就绪，    
		 * 就把扫描键传给索引 AM。 
		 */
		if (node->iss_NumRuntimeKeys == 0 || node->iss_RuntimeKeysReady)
			index_rescan(scandesc,
						 node->iss_ScanKeys, node->iss_NumScanKeys,
						 node->iss_OrderByKeys, node->iss_NumOrderByKeys);
	}

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		/*
		 * 先检查重排序队列。若队首元组的 ORDER BY 值       
		 * 小于（或等于）索引上次返回的值，               
		 * 现在就可以将其返回。              
		 */
		if (!pairingheap_is_empty(node->iss_ReorderQueue))
		{
			topmost = (ReorderTuple *) pairingheap_first(node->iss_ReorderQueue);

			if (node->iss_ReachedEnd ||
				cmp_orderbyvals(topmost->orderbyvals,
								topmost->orderbynulls,
								scandesc->xs_orderbyvals,
								scandesc->xs_orderbynulls,
								node) <= 0)
			{
				HeapTuple	tuple;

				tuple = reorderqueue_pop(node);

				/* 传入 true，因为队列里元组是 palloc 出的副本*/
				ExecForceStoreHeapTuple(tuple, slot, true);
				return slot;
			}
		}
		else if (node->iss_ReachedEnd)
		{
			/* 队列已空，索引亦无更多元组。至此完成。   */
			return ExecClearTuple(slot);
		}

		/*
		 * 从索引取回下一个元组  
		 */
next_indextuple:
		if (!index_getnext_slot(scandesc, ForwardScanDirection, slot))
		{
			/*
			 * 索引已无更多元组。但在结束之前，还需把     
			 * 队列中剩余的元组取完。                 
			 */
			node->iss_ReachedEnd = true;
			continue;
		}

		/*
		 * 若索引是有损的，就必须用取到的元组重查     
		 * 索引条件和 ORDER BY 表达式。        
		 */
		if (scandesc->xs_recheck)
		{
			econtext->ecxt_scantuple = slot;
			if (!ExecQualAndReset(node->indexqualorig, econtext))
			{
				/* 重查未过，丢弃该元组再取下一个       */
				InstrCountFiltered2(node, 1);
				/* 让此循环可被取消          */
				CHECK_FOR_INTERRUPTS();
				goto next_indextuple;
			}
		}

		if (scandesc->xs_recheckorderby)
		{
			econtext->ecxt_scantuple = slot;
			ResetExprContext(econtext);
			EvalOrderByExpressions(node, econtext);

			/*
			 * 索引返回的 ORDER BY 值准确吗？recheck 标志     
			 * 意味着索引可能返回不准确的值，但话说回来，
			 * 对任何特定元组返回的值也可能完全正确。  
			 * 将索引返回的值与重新计算的值作比较。（若 
			 * 索引返回的值恰好完全正确，通常就能免去     
			 * 把元组压入队列、只为随后再把它弹出来     
			 * 白忙一场。）                                             
			 */
			cmp = cmp_orderbyvals(node->iss_OrderByValues,
								  node->iss_OrderByNulls,
								  scandesc->xs_orderbyvals,
								  scandesc->xs_orderbynulls,
								  node);
			if (cmp < 0)
				elog(ERROR, "index returned tuples in wrong order");
			else if (cmp == 0)
				was_exact = true;
			else
				was_exact = false;
			lastfetched_vals = node->iss_OrderByValues;
			lastfetched_nulls = node->iss_OrderByNulls;
		}
		else
		{
			was_exact = true;
			lastfetched_vals = scandesc->xs_orderbyvals;
			lastfetched_nulls = scandesc->xs_orderbynulls;
		}

		/*
		 * 能否立即返回该元组，还是需要把它压入            
		 * 重排序队列？若索引返回的 ORDER BY 表达式值       
		 * 并不准确，那就还不能返回它，因为索引的        
		 * 下一个元组可能需要排在这个元组之前。             
		 * 此外，若队列中已有任何更小的元组，也不能       
		 * 返回  
		 */
		if (!was_exact || (topmost && cmp_orderbyvals(lastfetched_vals,
													  lastfetched_nulls,
													  topmost->orderbyvals,
													  topmost->orderbynulls,
													  node) > 0))
		{
			/* 把该元组放入队列    */
			reorderqueue_push(node, slot, lastfetched_vals, lastfetched_nulls);
			continue;
		}
		else
		{
			/* 可以立即返回该元组。     */
			return slot;
		}
	}

	/*
	 * 走到这里说明索引扫描失败，本次扫描已到            
	 * 尽头。 
	 */
	return ExecClearTuple(slot);
}

/*
 * 基于堆元组计算 ORDER BY 子句中的表达式。                   
 */
static void
EvalOrderByExpressions(IndexScanState *node, ExprContext *econtext)
{
	int			i;
	ListCell   *l;
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	i = 0;
	foreach(l, node->indexorderbyorig)
	{
		ExprState  *orderby = (ExprState *) lfirst(l);

		node->iss_OrderByValues[i] = ExecEvalExpr(orderby,
												  econtext,
												  &node->iss_OrderByNulls[i]);
		i++;
	}

	MemoryContextSwitchTo(oldContext);
}

/*
 * IndexRecheck -- 在 EvalPlanQual 中重查元组的访问方法例程   
 */
static bool
IndexRecheck(IndexScanState *node, TupleTableSlot *slot)
{
	ExprContext *econtext;

	/*
	 * 从索引扫描节点中提取必要的信息     
	 */
	econtext = node->ss.ps.ps_ExprContext;

	/* 该元组满足 indexqual 条件吗？       */
	econtext->ecxt_scantuple = slot;
	return ExecQualAndReset(node->indexqualorig, econtext);
}


/*
 * 比较 ORDER BY 表达式的值。 
 */
static int
cmp_orderbyvals(const Datum *adist, const bool *anulls,
				const Datum *bdist, const bool *bnulls,
				IndexScanState *node)
{
	int			i;
	int			result;

	for (i = 0; i < node->iss_NumOrderByKeys; i++)
	{
		SortSupport ssup = &node->iss_SortSupport[i];

		/*
		 * 处理空值。我们只需支持 NULLS LAST 排序，             
		 * 因为否则 match_pathkeys_to_index() 不会考虑    
		 * indexorderby 实现      
		 */
		if (anulls[i] && !bnulls[i])
			return 1;
		else if (!anulls[i] && bnulls[i])
			return -1;
		else if (anulls[i] && bnulls[i])
			return 0;

		result = ssup->comparator(adist[i], bdist[i], ssup);
		if (result != 0)
			return result;
	}

	return 0;
}

/*
 * 配对堆提供取顶部（最大）元素的能力，而 KNN              
 * 提供升序排序，故我们反转排序方向。  
 */
static int
reorderqueue_cmp(const pairingheap_node *a, const pairingheap_node *b,
				 void *arg)
{
	ReorderTuple *rta = (ReorderTuple *) a;
	ReorderTuple *rtb = (ReorderTuple *) b;
	IndexScanState *node = (IndexScanState *) arg;

	/* 交换参数顺序以反转排序方向          */
	return cmp_orderbyvals(rtb->orderbyvals, rtb->orderbynulls,
						   rta->orderbyvals, rta->orderbynulls,
						   node);
}

/*
 * 把元组压入重排序队列的辅助函数。     
 */
static void
reorderqueue_push(IndexScanState *node, TupleTableSlot *slot,
				  Datum *orderbyvals, bool *orderbynulls)
{
	IndexScanDesc scandesc = node->iss_ScanDesc;
	EState	   *estate = node->ss.ps.state;
	MemoryContext oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
	ReorderTuple *rt;
	int			i;

	rt = (ReorderTuple *) palloc(sizeof(ReorderTuple));
	rt->htup = ExecCopySlotHeapTuple(slot);
	rt->orderbyvals =
		(Datum *) palloc(sizeof(Datum) * scandesc->numberOfOrderBys);
	rt->orderbynulls =
		(bool *) palloc(sizeof(bool) * scandesc->numberOfOrderBys);
	for (i = 0; i < node->iss_NumOrderByKeys; i++)
	{
		if (!orderbynulls[i])
			rt->orderbyvals[i] = datumCopy(orderbyvals[i],
										   node->iss_OrderByTypByVals[i],
										   node->iss_OrderByTypLens[i]);
		else
			rt->orderbyvals[i] = (Datum) 0;
		rt->orderbynulls[i] = orderbynulls[i];
	}
	pairingheap_add(node->iss_ReorderQueue, &rt->ph_node);

	MemoryContextSwitchTo(oldContext);
}

/*
 * 从重排序队列弹出下一个元组的辅助函数。    
 */
static HeapTuple
reorderqueue_pop(IndexScanState *node)
{
	HeapTuple	result;
	ReorderTuple *topmost;
	int			i;

	topmost = (ReorderTuple *) pairingheap_remove_first(node->iss_ReorderQueue);

	result = topmost->htup;
	for (i = 0; i < node->iss_NumOrderByKeys; i++)
	{
		if (!node->iss_OrderByTypByVals[i] && !topmost->orderbynulls[i])
			pfree(DatumGetPointer(topmost->orderbyvals[i]));
	}
	pfree(topmost->orderbyvals);
	pfree(topmost->orderbynulls);
	pfree(topmost);

	return result;
}


/* ----------------------------------------------------------------
 *		ExecIndexScan(node)
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecIndexScan(PlanState *pstate)
{
	IndexScanState *node = castNode(IndexScanState, pstate);

	/*
	 * 若有运行时键且尚未设置，现在就设置它们。           
	 */
	if (node->iss_NumRuntimeKeys != 0 && !node->iss_RuntimeKeysReady)
		ExecReScan((PlanState *) node);

	if (node->iss_NumOrderByKeys > 0)
		return ExecScan(&node->ss,
						(ExecScanAccessMtd) IndexNextWithReorder,
						(ExecScanRecheckMtd) IndexRecheck);
	else
		return ExecScan(&node->ss,
						(ExecScanAccessMtd) IndexNext,
						(ExecScanRecheckMtd) IndexRecheck);
}

/* ----------------------------------------------------------------
 *		ExecReScanIndexScan(node)
 *
 *		重新计算任何取值依赖于运行时信息的扫描键，
 *		然后重新扫描被索引的关系。                         
 *
 *		更新扫描键以前是在 ExecUpdateIndexScanKeys  
 *		中完成。并入 ReScan 让索引与关系/一般流   
 *		的重扫更加统一。                                      
 * ----------------------------------------------------------------
 */
void
ExecReScanIndexScan(IndexScanState *node)
{
	/*
	 * 若正在进行运行时键计算（即索引键值并非都      
	 * 是简单 Const），则计算新的键值。但首先需重置       
	 * 上下文，以免每扫描一个外层元组就泄漏内存。 
	 * 注意：这里假定每次调用都会重新计算 *all*              
	 * 运行时键 
	 */
	if (node->iss_NumRuntimeKeys != 0)
	{
		ExprContext *econtext = node->iss_RuntimeContext;

		ResetExprContext(econtext);
		ExecIndexEvalRuntimeKeys(econtext,
								 node->iss_RuntimeKeys,
								 node->iss_NumRuntimeKeys);
	}
	node->iss_RuntimeKeysReady = true;

	/* 清空重排序队列   */
	if (node->iss_ReorderQueue)
	{
		HeapTuple	tuple;

		while (!pairingheap_is_empty(node->iss_ReorderQueue))
		{
			tuple = reorderqueue_pop(node);
			heap_freetuple(tuple);
		}
	}

	/* 重置扫描     */
	if (node->iss_ScanDesc)
		index_rescan(node->iss_ScanDesc,
					 node->iss_ScanKeys, node->iss_NumScanKeys,
					 node->iss_OrderByKeys, node->iss_NumOrderByKeys);
	node->iss_ReachedEnd = false;

	ExecScanReScan(&node->ss);
}


/*
 * ExecIndexEvalRuntimeKeys
 *		求值所有运行时键值，并更新扫描键。      
 */
void
ExecIndexEvalRuntimeKeys(ExprContext *econtext,
						 IndexRuntimeKeyInfo *runtimeKeys, int numRuntimeKeys)
{
	int			j;
	MemoryContext oldContext;

	/* 我们希望把键值保存在每元组内存中   */
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	for (j = 0; j < numRuntimeKeys; j++)
	{
		ScanKey		scan_key = runtimeKeys[j].scan_key;
		ExprState  *key_expr = runtimeKeys[j].key_expr;
		Datum		scanvalue;
		bool		isNull;

		/*
		 * 对每个运行时键，提取其运行时表达式，并          
		 * 结合当前上下文对其求值。随后我们把结果        
		 * 填入对应扫描键。 
		 *
		 * 注意：求值结果可能是一个按引用传递的值，    
		 * 它存于外层扫描元组中，而非  
		 * econtext->ecxt_per_tuple_memory。我们假定外层            
		 * 元组在整个扫描期间保持不动。若这一点不成立， 
		 * 可以把结果显式复制进本上下文，不过我认为  
		 * 没必要 
		 *
		 * 求值结果也可能是一个 toasted 值。此时我们   
		 * 应当强制对它进行 detoast，以免每次由索引      
		 * 支持函数检查该值时都要重复进行                 
		 * detoast。             
		 */
		scanvalue = ExecEvalExpr(key_expr,
								 econtext,
								 &isNull);
		if (isNull)
		{
			scan_key->sk_argument = scanvalue;
			scan_key->sk_flags |= SK_ISNULL;
		}
		else
		{
			if (runtimeKeys[j].key_toastable)
				scanvalue = PointerGetDatum(PG_DETOAST_DATUM(scanvalue));
			scan_key->sk_argument = scanvalue;
			scan_key->sk_flags &= ~SK_ISNULL;
		}
	}

	MemoryContextSwitchTo(oldContext);
}

/*
 * ExecIndexEvalArrayKeys
 *		求值所有数组键值，并准备遍历各个数组。           
 *
 * 若有数组元素需要考虑则返回 true；false 表示至         
 * 少有一个 null 或空数组，因而不可能匹配。返回       
 * true 时，扫描键会用各数组的第一个元素来初始化。       
 */
bool
ExecIndexEvalArrayKeys(ExprContext *econtext,
					   IndexArrayKeyInfo *arrayKeys, int numArrayKeys)
{
	bool		result = true;
	int			j;
	MemoryContext oldContext;

	/* 希望把数组保存在每元组内存中     */
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	for (j = 0; j < numArrayKeys; j++)
	{
		ScanKey		scan_key = arrayKeys[j].scan_key;
		ExprState  *array_expr = arrayKeys[j].array_expr;
		Datum		arraydatum;
		bool		isNull;
		ArrayType  *arrayval;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			num_elems;
		Datum	   *elem_values;
		bool	   *elem_nulls;

		/*
		 * 计算并解构数组表达式。（注释与          
		 * ExecIndexEvalRuntimeKeys() 相同。）    
		 */
		arraydatum = ExecEvalExpr(array_expr,
								  econtext,
								  &isNull);
		if (isNull)
		{
			result = false;
			break;				/* 再求值已无意义       */
		}
		arrayval = DatumGetArrayTypeP(arraydatum);
		/* 可以缓存这些数据，但不清楚是否值得   */
		get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
							 &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arrayval,
						  ARR_ELEMTYPE(arrayval),
						  elmlen, elmbyval, elmalign,
						  &elem_values, &elem_nulls, &num_elems);
		if (num_elems <= 0)
		{
			result = false;
			break;				/* 再求值已无意义       */
		}

		/*
		 * 注意：先前的数组数据（若有）预期会随
		 * 重置每元组上下文而自动释放；因此这里          
		 * 不必释放 
		 */
		arrayKeys[j].elem_values = elem_values;
		arrayKeys[j].elem_nulls = elem_nulls;
		arrayKeys[j].num_elems = num_elems;
		scan_key->sk_argument = elem_values[0];
		if (elem_nulls[0])
			scan_key->sk_flags |= SK_ISNULL;
		else
			scan_key->sk_flags &= ~SK_ISNULL;
		arrayKeys[j].next_elem = 1;
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}

/*
 * ExecIndexAdvanceArrayKeys
 *		前进到下一组数组键值（如果还有）。 
 *
 * 若还有下一组值待考虑则返回 true，否则返回              
 * false。返回 true 时，扫描键会用下一组值来初始化。     
 */
bool
ExecIndexAdvanceArrayKeys(IndexArrayKeyInfo *arrayKeys, int numArrayKeys)
{
	bool		found = false;
	int			j;

	/*
	 * 注意我们让最右边的数组键前进得最快，因为       
	 * 它对应于可用条件中最低阶的索引列。据推测，
	 * 这能带来索引中更好的访问                                 
	 * 局部性。        
	 */
	for (j = numArrayKeys - 1; j >= 0; j--)
	{
		ScanKey		scan_key = arrayKeys[j].scan_key;
		int			next_elem = arrayKeys[j].next_elem;
		int			num_elems = arrayKeys[j].num_elems;
		Datum	   *elem_values = arrayKeys[j].elem_values;
		bool	   *elem_nulls = arrayKeys[j].elem_nulls;

		if (next_elem >= num_elems)
		{
			next_elem = 0;
			found = false;		/* 需要推进下一个数组键 */
		}
		else
			found = true;
		scan_key->sk_argument = elem_values[next_elem];
		if (elem_nulls[next_elem])
			scan_key->sk_flags |= SK_ISNULL;
		else
			scan_key->sk_flags &= ~SK_ISNULL;
		arrayKeys[j].next_elem = next_elem + 1;
		if (found)
			break;
	}

	return found;
}


/* ----------------------------------------------------------------
 *		ExecEndIndexScan
 * ----------------------------------------------------------------
 */
void
ExecEndIndexScan(IndexScanState *node)
{
	Relation	indexRelationDesc;
	IndexScanDesc indexScanDesc;

	/*
	 * 从节点中提取信息         
	 */
	indexRelationDesc = node->iss_RelationDesc;
	indexScanDesc = node->iss_ScanDesc;

	/*
	 * 释放表达式上下文……死代码，见 ExecFreeExprContext 
	 */
#ifdef NOT_USED
	ExecFreeExprContext(&node->ss.ps);
	if (node->iss_RuntimeContext)
		FreeExprContext(node->iss_RuntimeContext, true);
#endif

	/*
	 * 清空元组表槽位      
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * 关闭索引关系（未打开过则为空操作）  
	 */
	if (indexScanDesc)
		index_endscan(indexScanDesc);
	if (indexRelationDesc)
		index_close(indexRelationDesc, NoLock);
}

/* ----------------------------------------------------------------
 *		ExecIndexMarkPos
 *
 * 注意：我们假定不会有调用者在读到至少一个            
 * 元组前设置标记。否则 iss_ScanDesc 可能仍为 NULL。 
 * ----------------------------------------------------------------
 */
void
ExecIndexMarkPos(IndexScanState *node)
{
	EState	   *estate = node->ss.ps.state;
	EPQState   *epqstate = estate->es_epq_active;

	if (epqstate != NULL)
	{
		/*
		 * 我们正处于 EvalPlanQual 重查之中。若该关系存在    
		 * 测试元组，那就完全不应访问索引。此时我们       
		 * 反而需保存并在之后恢复 relsubs_done 标志的 
		 * 状态，这样才能重新取回测试元组。不过，既然   
		 * 假定没有调用者会在扫描开始时设置标记，      
		 * 那么只有 relsubs_done[i] 已被置位时才可能到    
		 * 这里，因此无需保存任何状态。 
		 */
		Index		scanrelid = ((Scan *) node->ss.ps.plan)->scanrelid;

		Assert(scanrelid > 0);
		if (epqstate->relsubs_slot[scanrelid - 1] != NULL ||
			epqstate->relsubs_rowmark[scanrelid - 1] != NULL)
		{
			/* 验证上面的论断  */
			if (!epqstate->relsubs_done[scanrelid - 1])
				elog(ERROR, "unexpected ExecIndexMarkPos call in EPQ recheck");
			return;
		}
	}

	index_markpos(node->iss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecIndexRestrPos
 * ----------------------------------------------------------------
 */
void
ExecIndexRestrPos(IndexScanState *node)
{
	EState	   *estate = node->ss.ps.state;
	EPQState   *epqstate = estate->es_epq_active;

	if (estate->es_epq_active != NULL)
	{
		/* 见 ExecIndexMarkPos 中的注释*/
		Index		scanrelid = ((Scan *) node->ss.ps.plan)->scanrelid;

		Assert(scanrelid > 0);
		if (epqstate->relsubs_slot[scanrelid - 1] != NULL ||
			epqstate->relsubs_rowmark[scanrelid - 1] != NULL)
		{
			/* 验证上面的论断  */
			if (!epqstate->relsubs_done[scanrelid - 1])
				elog(ERROR, "unexpected ExecIndexRestrPos call in EPQ recheck");
			return;
		}
	}

	index_restrpos(node->iss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecInitIndexScan
 *
 *		初始化索引扫描的状态信息，创建扫描键 
 *		并打开基础关系与索引关系。           
 *
 *		注意：索引扫描有两套状态信息，因为       
 *			  我们必须同时跟踪基础关系和           
 *			  索引关系。
 * ----------------------------------------------------------------
 */
IndexScanState *
ExecInitIndexScan(IndexScan *node, EState *estate, int eflags)
{
	IndexScanState *indexstate;
	Relation	currentRelation;
	LOCKMODE	lockmode;

	/*
	 * 创建状态结构    
	 */
	indexstate = makeNode(IndexScanState);
	indexstate->ss.ps.plan = (Plan *) node;
	indexstate->ss.ps.state = estate;
	indexstate->ss.ps.ExecProcNode = ExecIndexScan;

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &indexstate->ss.ps);

	/*
	 * 打开扫描关系    
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid, eflags);

	indexstate->ss.ss_currentRelation = currentRelation;
	indexstate->ss.ss_currentScanDesc = NULL;	/* 这里无堆扫描*/

	/*
	 * 从关系描述符中获取扫描类型。     
	 */
	ExecInitScanTupleSlot(estate, &indexstate->ss,
						  RelationGetDescr(currentRelation),
						  table_slot_callbacks(currentRelation));

	/*
	 * 初始化结果类型与投影。     
	 */
	ExecInitResultTypeTL(&indexstate->ss.ps);
	ExecAssignScanProjectionInfo(&indexstate->ss);

	/*
	 * 初始化子表达式       
	 *
	 * 注意：我们不会初始化整个 indexqual 表达式，        
	 * 只初始化与运行时键对应的子部分（见下文）。   
	 * indexorderby（若存在）同样处理。但 indexqualorig      
	 * 表达式则总是被初始化，尽管它只会在一些不           
	 * 常见的情况下才会用到——这点值得改进。（表达式
	 * 内的 SubPlan 必须现在找到…）
	 */
	indexstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) indexstate);
	indexstate->indexqualorig =
		ExecInitQual(node->indexqualorig, (PlanState *) indexstate);
	indexstate->indexorderbyorig =
		ExecInitExprList(node->indexorderbyorig, (PlanState *) indexstate);

	/*
	 * 如果只是执行 EXPLAIN（即不打算运行计划），          
	 * 就到此为止。这样索引顾问插件就能 EXPLAIN               
	 * 引用不存在索引的计划。 
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return indexstate;

	/* 打开索引关系。    */
	lockmode = exec_rt_fetch(node->scan.scanrelid, estate)->rellockmode;
	indexstate->iss_RelationDesc = index_open(node->indexid, lockmode);

	/*
	 * 初始化索引特有的扫描状态
	 */
	indexstate->iss_RuntimeKeysReady = false;
	indexstate->iss_RuntimeKeys = NULL;
	indexstate->iss_NumRuntimeKeys = 0;

	/*
	 * 根据索引条件构建索引扫描键               
	 */
	ExecIndexBuildScanKeys((PlanState *) indexstate,
						   indexstate->iss_RelationDesc,
						   node->indexqual,
						   false,
						   &indexstate->iss_ScanKeys,
						   &indexstate->iss_NumScanKeys,
						   &indexstate->iss_RuntimeKeys,
						   &indexstate->iss_NumRuntimeKeys,
						   NULL,	/* 无 ArrayKeys*/
						   NULL);

	/*
	 * 任何 ORDER BY 表达式都必须同样转为扫描键           
	 */
	ExecIndexBuildScanKeys((PlanState *) indexstate,
						   indexstate->iss_RelationDesc,
						   node->indexorderby,
						   true,
						   &indexstate->iss_OrderByKeys,
						   &indexstate->iss_NumOrderByKeys,
						   &indexstate->iss_RuntimeKeys,
						   &indexstate->iss_NumRuntimeKeys,
						   NULL,	/* 无 ArrayKeys*/
						   NULL);

	/* 若需要重查 ORDER BY 表达式，则初始化排序支持  */
	if (indexstate->iss_NumOrderByKeys > 0)
	{
		int			numOrderByKeys = indexstate->iss_NumOrderByKeys;
		int			i;
		ListCell   *lco;
		ListCell   *lcx;

		/*
		 * 准备排序支持，并为每个 ORDER BY 表达式查找       
		 * 类型     
		 */
		Assert(numOrderByKeys == list_length(node->indexorderbyops));
		Assert(numOrderByKeys == list_length(node->indexorderbyorig));
		indexstate->iss_SortSupport = (SortSupportData *)
			palloc0(numOrderByKeys * sizeof(SortSupportData));
		indexstate->iss_OrderByTypByVals = (bool *)
			palloc(numOrderByKeys * sizeof(bool));
		indexstate->iss_OrderByTypLens = (int16 *)
			palloc(numOrderByKeys * sizeof(int16));
		i = 0;
		forboth(lco, node->indexorderbyops, lcx, node->indexorderbyorig)
		{
			Oid			orderbyop = lfirst_oid(lco);
			Node	   *orderbyexpr = (Node *) lfirst(lcx);
			Oid			orderbyType = exprType(orderbyexpr);
			Oid			orderbyColl = exprCollation(orderbyexpr);
			SortSupport orderbysort = &indexstate->iss_SortSupport[i];

			/* 初始化排序支持   */
			orderbysort->ssup_cxt = CurrentMemoryContext;
			orderbysort->ssup_collation = orderbyColl;
			/* NULLS LAST 见 cmp_orderbyvals() 中注释   */
			orderbysort->ssup_nulls_first = false;
			/* ssup_attno 此处与别处均不使用  */
			orderbysort->ssup_attno = 0;
			/* 不使用缩写 */
			orderbysort->abbreviate = false;
			PrepareSortSupportFromOrderingOp(orderbyop, orderbysort);

			get_typlenbyval(orderbyType,
							&indexstate->iss_OrderByTypLens[i],
							&indexstate->iss_OrderByTypByVals[i]);
			i++;
		}

		/* 分配数组来存放重新计算出的距离       */
		indexstate->iss_OrderByValues = (Datum *)
			palloc(numOrderByKeys * sizeof(Datum));
		indexstate->iss_OrderByNulls = (bool *)
			palloc(numOrderByKeys * sizeof(bool));

		/* 并初始化重排序队列      */
		indexstate->iss_ReorderQueue = pairingheap_allocate(reorderqueue_cmp,
															indexstate);
	}

	/*
	 * 若有运行时键，就需要一个 ExprContext 来对它们        
	 * 求值。节点的标准上下文不行，因为我们想要          
	 * 为每个元组重置它。所以照样再建一个同样的上下文 
	 * -tgl 7/11/00
	 */
	if (indexstate->iss_NumRuntimeKeys != 0)
	{
		ExprContext *stdecontext = indexstate->ss.ps.ps_ExprContext;

		ExecAssignExprContext(estate, &indexstate->ss.ps);
		indexstate->iss_RuntimeContext = indexstate->ss.ps.ps_ExprContext;
		indexstate->ss.ps.ps_ExprContext = stdecontext;
	}
	else
	{
		indexstate->iss_RuntimeContext = NULL;
	}

	/*
	 * 完成了
	 */
	return indexstate;
}


/*
 * ExecIndexBuildScanKeys
 *		根据索引条件表达式构建索引扫描键                  
 *
 * 索引条件以 ScanKey 数组的形式传给索引 AM。                 
 * 本例程设置好各 ScanKey，填入其所有常量字段，        
 * 并为具有非常量比较值的键准备相关信息。我们         
 * 把索引条件表达式分为五类：                              
 *
 * 1. 带常量比较值的简单算子（"indexkey op constant"）。        
 * 对这类，我们只需填入一个含该常量值的 ScanKey。  
 *
 * 2. 带非常量值的简单算子（"indexkey op expression"）。    
 * 对这类，我们创建的 ScanKey 除表达式值外其余       
 * 字段均已填好，并建立 IndexRuntimeKeyInfo 结构来       
 * 在恰当时机驱动表达式的求值。      
 *
 * 3. RowCompareExpr ("(indexkey, indexkey, ...) op (expr, expr, ...)").
 * 对这类，我们会创建一个头 ScanKey 外加一个附属       
 * ScanKey 数组，如 access/skey.h 所规定。行比较的       
 * 各元素的比较值可为常量，也可为非常量。  
 *
 * 4. ScalarArrayOpExpr（"indexkey op ANY (array-expression)"）。若      
 * 索引支持 amsearcharray，我们对它们的处理方式          
 * 与简单算子相同，即设置 SK_SEARCHARRAY 标志                   
 * 让索引 AM 自行处理。否则，我们创建的 ScanKey 除          
 * 比较值外均已填好，并建立 IndexArrayKeyInfo 结构          
 * 来驱动该条件的处理。（注意：若使用该结构，则       
 * 即使它是常量，该表达式总被当作需要运行时求值。） 
 *
 * 5. NullTest（"indexkey IS NULL/IS NOT NULL"）。我们只需    
 * 填好 ScanKey   
 *
 * 此代码也用于为 amcanorderbyop 索引准备 ORDER BY               
 * 表达式。行为完全相同，只是必须以不同的方式           
 * 查找算子。注意目前仅情况 1 与 2 可                     
 * 用于 ORDER BY。    
 *
 * 输入参数：  
 *
 * planstate: 我们服务的执行器状态节点  
 * index: 要为其构建扫描键的索引      
 * quals: indexquals/indexorderbys 表达式       
 * isorderby: 处理 ORDER BY 表达式时为 true，条件时为 false   
 * *runtimeKeys: 既有 IndexRuntimeKeyInfos 的指针，无则为 NULL   
 * *numRuntimeKeys: 既有运行时键的数量        
 *
 * 输出参数：   
 *
 * *scanKeys: 接收 ScanKey 数组的指针   
 * *numScanKeys: 接收扫描键的数量   
 * *runtimeKeys: 接收 IndexRuntimeKeyInfos 数组的指针，无则为 NULL  
 * *numRuntimeKeys: 接收运行时键的数量    
 * *arrayKeys: 接收 IndexArrayKeyInfos 数组的指针，无则为 NULL  
 * *numArrayKeys: 接收数组键的数量     
 *
 * 调用者可为 arrayKeys 与 numArrayKeys 传入 NULL，           
 * 表示不支持 IndexArrayKeyInfo。 
 */
void
ExecIndexBuildScanKeys(PlanState *planstate, Relation index,
					   List *quals, bool isorderby,
					   ScanKey *scanKeys, int *numScanKeys,
					   IndexRuntimeKeyInfo **runtimeKeys, int *numRuntimeKeys,
					   IndexArrayKeyInfo **arrayKeys, int *numArrayKeys)
{
	ListCell   *qual_cell;
	ScanKey		scan_keys;
	IndexRuntimeKeyInfo *runtime_keys;
	IndexArrayKeyInfo *array_keys;
	int			n_scan_keys;
	int			n_runtime_keys;
	int			max_runtime_keys;
	int			n_array_keys;
	int			j;

	/* 分配 ScanKey 结构数组：每条件一个    */
	n_scan_keys = list_length(quals);
	scan_keys = (ScanKey) palloc(n_scan_keys * sizeof(ScanKeyData));

	/*
	 * runtime_keys 数组会按需动态调整大小。我们这样          
	 * 处理是为了让同一个运行时键数组能在          
	 * indexquals 与 indexorderbys 之间共享，二者会在本            
	 * 函数的单独调用中得到处理。首次调用必须传入   
	 * NULL 
	 */
	runtime_keys = *runtimeKeys;
	n_runtime_keys = max_runtime_keys = *numRuntimeKeys;

	/* 把 array_keys 分配为可能需要的最大长度          */
	array_keys = (IndexArrayKeyInfo *)
		palloc0(n_scan_keys * sizeof(IndexArrayKeyInfo));
	n_array_keys = 0;

	/*
	 * 把给定条件中的每个算子子句转换为一个                 
	 * scan key
	 */
	j = 0;
	foreach(qual_cell, quals)
	{
		Expr	   *clause = (Expr *) lfirst(qual_cell);
		ScanKey		this_scan_key = &scan_keys[j++];
		Oid			opno;		/* 算子 OID     */
		RegProcedure opfuncid;	/* 扫描用的算子过程号   */
		Oid			opfamily;	/* 索引列的算子族    */
		int			op_strategy;	/* 算子的策略号         */
		Oid			op_lefttype;	/* 算子声明的输入类型     */
		Oid			op_righttype;
		Expr	   *leftop;		/* 算子左部表达式   */
		Expr	   *rightop;	/* 右部表达式 */
		AttrNumber	varattno;	/* 扫描所用属性号   */
		int			indnkeyatts;

		indnkeyatts = IndexRelationGetNumberOfKeyAttributes(index);
		if (IsA(clause, OpExpr))
		{
			/* indexkey op 常量或 indexkey op 表达式 */
			int			flags = 0;
			Datum		scanvalue;

			opno = ((OpExpr *) clause)->opno;
			opfuncid = ((OpExpr *) clause)->opfuncid;

			/*
			 * leftop 应为索引键 Var，可能已被重新标注 
			 */
			leftop = (Expr *) get_leftop(clause);

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!(IsA(leftop, Var) &&
				  ((Var *) leftop)->varno == INDEX_VAR))
				elog(ERROR, "indexqual doesn't have key on left side");

			varattno = ((Var *) leftop)->varattno;
			if (varattno < 1 || varattno > indnkeyatts)
				elog(ERROR, "bogus index qualification");

			/*
			 * 我们须查找算子的策略号。这样可以        
			 * 交叉验证该算子确实与索引匹配。                 
			 */
			opfamily = index->rd_opfamily[varattno - 1];

			get_op_opfamily_properties(opno, opfamily, isorderby,
									   &op_strategy,
									   &op_lefttype,
									   &op_righttype);

			if (isorderby)
				flags |= SK_ORDER_BY;

			/*
			 * rightop 是常量或变量形式的比较值        
			 */
			rightop = (Expr *) get_rightop(clause);

			if (rightop && IsA(rightop, RelabelType))
				rightop = ((RelabelType *) rightop)->arg;

			Assert(rightop != NULL);

			if (IsA(rightop, Const))
			{
				/* 好，简单的常量比较值       */
				scanvalue = ((Const *) rightop)->constvalue;
				if (((Const *) rightop)->constisnull)
					flags |= SK_ISNULL;
			}
			else
			{
				/* 需要把这个当作运行时键处理 */
				if (n_runtime_keys >= max_runtime_keys)
				{
					if (max_runtime_keys == 0)
					{
						max_runtime_keys = 8;
						runtime_keys = (IndexRuntimeKeyInfo *)
							palloc(max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
					}
					else
					{
						max_runtime_keys *= 2;
						runtime_keys = (IndexRuntimeKeyInfo *)
							repalloc(runtime_keys, max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
					}
				}
				runtime_keys[n_runtime_keys].scan_key = this_scan_key;
				runtime_keys[n_runtime_keys].key_expr =
					ExecInitExpr(rightop, planstate);
				runtime_keys[n_runtime_keys].key_toastable =
					TypeIsToastable(op_righttype);
				n_runtime_keys++;
				scanvalue = (Datum) 0;
			}

			/*
			 * 适当地初始化扫描键的各个字段    
			 */
			ScanKeyEntryInitialize(this_scan_key,
								   flags,
								   varattno,	/* 要扫描的属性号    */
								   op_strategy, /* op 的策略  */
								   op_righttype,	/* 策略子类型  */
								   ((OpExpr *) clause)->inputcollid,	/* collation */
								   opfuncid,	/* 所用 reg proc */
								   scanvalue);	/* 常量   */
		}
		else if (IsA(clause, RowCompareExpr))
		{
			/* (indexkey, indexkey, ...) op (expression, expression, ...) */
			RowCompareExpr *rc = (RowCompareExpr *) clause;
			ScanKey		first_sub_key;
			int			n_sub_key;
			ListCell   *largs_cell;
			ListCell   *rargs_cell;
			ListCell   *opnos_cell;
			ListCell   *collids_cell;

			Assert(!isorderby);

			first_sub_key = (ScanKey)
				palloc(list_length(rc->opnos) * sizeof(ScanKeyData));
			n_sub_key = 0;

			/* 扫描 RowCompare 各列并生成附属 ScanKey 项           */
			forfour(largs_cell, rc->largs, rargs_cell, rc->rargs,
					opnos_cell, rc->opnos, collids_cell, rc->inputcollids)
			{
				ScanKey		this_sub_key = &first_sub_key[n_sub_key];
				int			flags = SK_ROW_MEMBER;
				Datum		scanvalue;
				Oid			inputcollation;

				leftop = (Expr *) lfirst(largs_cell);
				rightop = (Expr *) lfirst(rargs_cell);
				opno = lfirst_oid(opnos_cell);
				inputcollation = lfirst_oid(collids_cell);

				/*
				 * leftop 应为索引键 Var，可能已被重新标注 
				 */
				if (leftop && IsA(leftop, RelabelType))
					leftop = ((RelabelType *) leftop)->arg;

				Assert(leftop != NULL);

				if (!(IsA(leftop, Var) &&
					  ((Var *) leftop)->varno == INDEX_VAR))
					elog(ERROR, "indexqual doesn't have key on left side");

				varattno = ((Var *) leftop)->varattno;

				/*
				 * 我们必须查找算子关联的 btree 支持            
				 * 函数  
				 */
				if (index->rd_rel->relam != BTREE_AM_OID ||
					varattno < 1 || varattno > indnkeyatts)
					elog(ERROR, "bogus RowCompare index qualification");
				opfamily = index->rd_opfamily[varattno - 1];

				get_op_opfamily_properties(opno, opfamily, isorderby,
										   &op_strategy,
										   &op_lefttype,
										   &op_righttype);

				if (op_strategy != rc->rctype)
					elog(ERROR, "RowCompare index qualification contains wrong operator");

				opfuncid = get_opfamily_proc(opfamily,
											 op_lefttype,
											 op_righttype,
											 BTORDER_PROC);
				if (!RegProcedureIsValid(opfuncid))
					elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
						 BTORDER_PROC, op_lefttype, op_righttype, opfamily);

				/*
				 * rightop 是常量或变量形式的比较值        
				 */
				if (rightop && IsA(rightop, RelabelType))
					rightop = ((RelabelType *) rightop)->arg;

				Assert(rightop != NULL);

				if (IsA(rightop, Const))
				{
					/* 好，简单的常量比较值       */
					scanvalue = ((Const *) rightop)->constvalue;
					if (((Const *) rightop)->constisnull)
						flags |= SK_ISNULL;
				}
				else
				{
					/* 需要把这个当作运行时键处理 */
					if (n_runtime_keys >= max_runtime_keys)
					{
						if (max_runtime_keys == 0)
						{
							max_runtime_keys = 8;
							runtime_keys = (IndexRuntimeKeyInfo *)
								palloc(max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
						}
						else
						{
							max_runtime_keys *= 2;
							runtime_keys = (IndexRuntimeKeyInfo *)
								repalloc(runtime_keys, max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
						}
					}
					runtime_keys[n_runtime_keys].scan_key = this_sub_key;
					runtime_keys[n_runtime_keys].key_expr =
						ExecInitExpr(rightop, planstate);
					runtime_keys[n_runtime_keys].key_toastable =
						TypeIsToastable(op_righttype);
					n_runtime_keys++;
					scanvalue = (Datum) 0;
				}

				/*
				 * 适当地初始化附属扫描键的各个字段         
				 */
				ScanKeyEntryInitialize(this_sub_key,
									   flags,
									   varattno,	/* 属性号        */
									   op_strategy, /* op 的策略  */
									   op_righttype,	/* 策略子类型  */
									   inputcollation,	/* collation */
									   opfuncid,	/* 所用 reg proc */
									   scanvalue);	/* 常量   */
				n_sub_key++;
			}

			/* 正确标记最后一个附属扫描键    */
			first_sub_key[n_sub_key - 1].sk_flags |= SK_ROW_END;

			/*
			 * 头扫描键不使用 ScanKeyEntryInitialize 来初始化， 
			 * 因其不会包含有效的 sk_func 指针。  
			 */
			MemSet(this_scan_key, 0, sizeof(ScanKeyData));
			this_scan_key->sk_flags = SK_ROW_HEADER;
			this_scan_key->sk_attno = first_sub_key->sk_attno;
			this_scan_key->sk_strategy = rc->rctype;
			/* 头部不使用 sk_subtype、sk_collation、sk_func    */
			this_scan_key->sk_argument = PointerGetDatum(first_sub_key);
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			/* indexkey op ANY (array-expression) */
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
			int			flags = 0;
			Datum		scanvalue;

			Assert(!isorderby);

			Assert(saop->useOr);
			opno = saop->opno;
			opfuncid = saop->opfuncid;

			/*
			 * leftop 应为索引键 Var，可能已被重新标注 
			 */
			leftop = (Expr *) linitial(saop->args);

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!(IsA(leftop, Var) &&
				  ((Var *) leftop)->varno == INDEX_VAR))
				elog(ERROR, "indexqual doesn't have key on left side");

			varattno = ((Var *) leftop)->varattno;
			if (varattno < 1 || varattno > indnkeyatts)
				elog(ERROR, "bogus index qualification");

			/*
			 * 我们须查找算子的策略号。这样可以        
			 * 交叉验证该算子确实与索引匹配。                 
			 */
			opfamily = index->rd_opfamily[varattno - 1];

			get_op_opfamily_properties(opno, opfamily, isorderby,
									   &op_strategy,
									   &op_lefttype,
									   &op_righttype);

			/*
			 * rightop 是常量或变量形式的数组值   
			 */
			rightop = (Expr *) lsecond(saop->args);

			if (rightop && IsA(rightop, RelabelType))
				rightop = ((RelabelType *) rightop)->arg;

			Assert(rightop != NULL);

			if (index->rd_indam->amsearcharray)
			{
				/* 索引 AM 会像简单算子一样处理它      */
				flags |= SK_SEARCHARRAY;
				if (IsA(rightop, Const))
				{
					/* 好，简单的常量比较值       */
					scanvalue = ((Const *) rightop)->constvalue;
					if (((Const *) rightop)->constisnull)
						flags |= SK_ISNULL;
				}
				else
				{
					/* 需要把这个当作运行时键处理 */
					if (n_runtime_keys >= max_runtime_keys)
					{
						if (max_runtime_keys == 0)
						{
							max_runtime_keys = 8;
							runtime_keys = (IndexRuntimeKeyInfo *)
								palloc(max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
						}
						else
						{
							max_runtime_keys *= 2;
							runtime_keys = (IndexRuntimeKeyInfo *)
								repalloc(runtime_keys, max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
						}
					}
					runtime_keys[n_runtime_keys].scan_key = this_scan_key;
					runtime_keys[n_runtime_keys].key_expr =
						ExecInitExpr(rightop, planstate);

					/*
					 * 注意：运行时表达式的类型不是    
					 * op_righttype 而是同类数组；因此        
					 * TypeIsToastable() 无济于事。不过可       
					 * 以假定所有数组类型均可 TOAST。
					 */
					runtime_keys[n_runtime_keys].key_toastable = true;
					n_runtime_keys++;
					scanvalue = (Datum) 0;
				}
			}
			else
			{
				/* 执行器必须展开该数组值      */
				array_keys[n_array_keys].scan_key = this_scan_key;
				array_keys[n_array_keys].array_expr =
					ExecInitExpr(rightop, planstate);
				/* 其余字段已由 palloc0 清零           */
				n_array_keys++;
				scanvalue = (Datum) 0;
			}

			/*
			 * 适当地初始化扫描键的各个字段    
			 */
			ScanKeyEntryInitialize(this_scan_key,
								   flags,
								   varattno,	/* 要扫描的属性号    */
								   op_strategy, /* op 的策略  */
								   op_righttype,	/* 策略子类型  */
								   saop->inputcollid,	/* collation */
								   opfuncid,	/* 所用 reg proc */
								   scanvalue);	/* 常量   */
		}
		else if (IsA(clause, NullTest))
		{
			/* indexkey IS NULL or indexkey IS NOT NULL */
			NullTest   *ntest = (NullTest *) clause;
			int			flags;

			Assert(!isorderby);

			/*
			 * argument 应为索引键 Var，可能已被重新标注 
			 */
			leftop = ntest->arg;

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!(IsA(leftop, Var) &&
				  ((Var *) leftop)->varno == INDEX_VAR))
				elog(ERROR, "NullTest indexqual has wrong key");

			varattno = ((Var *) leftop)->varattno;

			/*
			 * 适当地初始化扫描键的各个字段    
			 */
			switch (ntest->nulltesttype)
			{
				case IS_NULL:
					flags = SK_ISNULL | SK_SEARCHNULL;
					break;
				case IS_NOT_NULL:
					flags = SK_ISNULL | SK_SEARCHNOTNULL;
					break;
				default:
					elog(ERROR, "unrecognized nulltesttype: %d",
						 (int) ntest->nulltesttype);
					flags = 0;	/* 让编译器安静  */
					break;
			}

			ScanKeyEntryInitialize(this_scan_key,
								   flags,
								   varattno,	/* 要扫描的属性号    */
								   InvalidStrategy, /* 无策略   */
								   InvalidOid,	/* 无策略子类型  */
								   InvalidOid,	/* 无 collation*/
								   InvalidOid,	/* 此项无 reg proc   */
								   (Datum) 0);	/* 常量   */
		}
		else
			elog(ERROR, "unsupported indexqual type: %d",
				 (int) nodeTag(clause));
	}

	Assert(n_runtime_keys <= max_runtime_keys);

	/* 去掉未使用的数组     */
	if (n_array_keys == 0)
	{
		pfree(array_keys);
		array_keys = NULL;
	}

	/*
	 * 将信息返回调用者  
	 */
	*scanKeys = scan_keys;
	*numScanKeys = n_scan_keys;
	*runtimeKeys = runtime_keys;
	*numRuntimeKeys = n_runtime_keys;
	if (arrayKeys)
	{
		*arrayKeys = array_keys;
		*numArrayKeys = n_array_keys;
	}
	else if (n_array_keys != 0)
		elog(ERROR, "ScalarArrayOpExpr index qual found where not allowed");
}

/* ----------------------------------------------------------------
 *						并行扫描支持   
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecIndexScanEstimate
 *
 *		计算并行查询 DSM 中我们所需的空间量， 
 *		并把我们的需求告知 pcxt->estimator。        
 * ----------------------------------------------------------------
 */
void
ExecIndexScanEstimate(IndexScanState *node,
					  ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;

	node->iss_PscanLen = index_parallelscan_estimate(node->iss_RelationDesc,
													 estate->es_snapshot);
	shm_toc_estimate_chunk(&pcxt->estimator, node->iss_PscanLen);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/* ----------------------------------------------------------------
 *		ExecIndexScanInitializeDSM
 *
 *		建立并行索引扫描描述符。    
 * ----------------------------------------------------------------
 */
void
ExecIndexScanInitializeDSM(IndexScanState *node,
						   ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;
	ParallelIndexScanDesc piscan;

	piscan = shm_toc_allocate(pcxt->toc, node->iss_PscanLen);
	index_parallelscan_initialize(node->ss.ss_currentRelation,
								  node->iss_RelationDesc,
								  estate->es_snapshot,
								  piscan);
	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id, piscan);
	node->iss_ScanDesc =
		index_beginscan_parallel(node->ss.ss_currentRelation,
								 node->iss_RelationDesc,
								 node->iss_NumScanKeys,
								 node->iss_NumOrderByKeys,
								 piscan);

	/*
	 * 若无运行时键要计算，或它们已就绪，就把扫描键   
	 * 传给索引 AM。           
	 */
	if (node->iss_NumRuntimeKeys == 0 || node->iss_RuntimeKeysReady)
		index_rescan(node->iss_ScanDesc,
					 node->iss_ScanKeys, node->iss_NumScanKeys,
					 node->iss_OrderByKeys, node->iss_NumOrderByKeys);
}

/* ----------------------------------------------------------------
 *		ExecIndexScanReInitializeDSM
 *
 *		在开始全新扫描前重置共享状态。    
 * ----------------------------------------------------------------
 */
void
ExecIndexScanReInitializeDSM(IndexScanState *node,
							 ParallelContext *pcxt)
{
	index_parallelrescan(node->iss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecIndexScanInitializeWorker
 *
 *		把相关信息从 TOC 复制到 planstate 中。 
 * ----------------------------------------------------------------
 */
void
ExecIndexScanInitializeWorker(IndexScanState *node,
							  ParallelWorkerContext *pwcxt)
{
	ParallelIndexScanDesc piscan;

	piscan = shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, false);
	node->iss_ScanDesc =
		index_beginscan_parallel(node->ss.ss_currentRelation,
								 node->iss_RelationDesc,
								 node->iss_NumScanKeys,
								 node->iss_NumOrderByKeys,
								 piscan);

	/*
	 * 若无运行时键要计算，或它们已就绪，就把扫描键   
	 * 传给索引 AM。           
	 */
	if (node->iss_NumRuntimeKeys == 0 || node->iss_RuntimeKeysReady)
		index_rescan(node->iss_ScanDesc,
					 node->iss_ScanKeys, node->iss_NumScanKeys,
					 node->iss_OrderByKeys, node->iss_NumOrderByKeys);
}
