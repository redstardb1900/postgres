/*-------------------------------------------------------------------------
 *
 * nodeIndexonlyscan.c
 *	  支持仅索引扫描的例程      
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeIndexonlyscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *		ExecIndexOnlyScan - 扫描索引  
 *		IndexOnlyNext - 取下一个元组  
 *		ExecInitIndexOnlyScan - 创建并初始化状态信息。 
 *		ExecReScanIndexOnlyScan - 重新扫描索引关系。 
 *		ExecEndIndexOnlyScan - 释放所有存储  
 *		ExecIndexOnlyMarkPos - 标记扫描位置 
 *		ExecIndexOnlyRestrPos - 恢复扫描位置    
 *		ExecIndexOnlyScanEstimate - 估算并行仅索引扫描 
 *						所需的 DSM 空间    
 *		ExecIndexOnlyScanInitializeDSM - 为并行仅索引扫描 
 *						初始化 DSM  
 *		ExecIndexOnlyScanReInitializeDSM - 为新扫描重新初始化  
 *		ExecIndexOnlyScanInitializeWorker - 在并行 worker 中附着 DSM     
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/tupdesc.h"
#include "access/visibilitymap.h"
#include "catalog/pg_type.h"
#include "executor/execdebug.h"
#include "executor/nodeIndexonlyscan.h"
#include "executor/nodeIndexscan.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"


static TupleTableSlot *IndexOnlyNext(IndexOnlyScanState *node);
static void StoreIndexTuple(IndexOnlyScanState *node, TupleTableSlot *slot,
							IndexTuple itup, TupleDesc itupdesc);


/* ----------------------------------------------------------------
 *		IndexOnlyNext
 *
 *		从 IndexOnlyScan 节点索引中取回元组。     
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
IndexOnlyNext(IndexOnlyScanState *node)
{
	EState	   *estate;
	ExprContext *econtext;
	ScanDirection direction;
	IndexScanDesc scandesc;
	TupleTableSlot *slot;
	ItemPointer tid;

	/*
	 * 从索引扫描节点提取必要信息           
	 */
	estate = node->ss.ps.state;

	/*
	 * 根据计划的扫描方向和当前执行方向，确定以何种     
	 * 方向扫描索引。                            
	 */
	direction = ScanDirectionCombine(estate->es_direction,
									 ((IndexOnlyScan *) node->ss.ps.plan)->indexorderdir);
	scandesc = node->ioss_ScanDesc;
	econtext = node->ss.ps.ps_ExprContext;
	slot = node->ss.ss_ScanTupleSlot;

	if (scandesc == NULL)
	{
		/*
		 * 如果仅索引扫描并非并行执行，或者我们在串行  
		 * 执行一个计划为并行的仅索引扫描，就会到   
		 * 达这里
		 */
		scandesc = index_beginscan(node->ss.ss_currentRelation,
								   node->ioss_RelationDesc,
								   estate->es_snapshot,
								   node->ioss_NumScanKeys,
								   node->ioss_NumOrderByKeys);

		node->ioss_ScanDesc = scandesc;


		/* 将其设置为仅索引扫描*/
		node->ioss_ScanDesc->xs_want_itup = true;
		node->ioss_VMBuffer = InvalidBuffer;

		/*
		 * 如果没有要计算的运行时键，或它们已就绪，就 
		 * 把扫描键传给索引 AM。    
		 */
		if (node->ioss_NumRuntimeKeys == 0 || node->ioss_RuntimeKeysReady)
			index_rescan(scandesc,
						 node->ioss_ScanKeys,
						 node->ioss_NumScanKeys,
						 node->ioss_OrderByKeys,
						 node->ioss_NumOrderByKeys);
	}

	/*
	 * 好，所需的东西都有了，取下一个元组。  
	 */
	while ((tid = index_getnext_tid(scandesc, direction)) != NULL)
	{
		bool		tuple_from_heap = false;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 若 TID 引用的堆页上所有元组均对所有人可见， 
		 * 就能跳过堆获取。总之我们都会用索引元组而  
		 * 非堆元组作为数据源。                                  
		 *
		 * 关于内存序效应的说明：visibilitymap_get_status 不     
		 * 会锁定可见性映射缓冲区，所以这里读到的结果 
		 * 可能略有滞后。但它不可能滞后到会造成影响的    
		 * 地步 
		 *
		 * 我们需要立即检测到因插入而清除 VM 位的情况，
		 * 因为元组存在于索引页中但不可见。本次扫描对    
		 * TID 的读取（索引缓冲区加共享锁）与 TID 的插入  
		 * （索引缓冲区加排他锁）串行化。因为 VM 位     
		 * 在更新索引之前就被清除，而索引页的加锁/解锁 
		 * 操作充当完整的内存屏障，所以我们若看到一个   
		 * 最近插入的 TID，就必定能看到被清除               
		 * 的位。                                     
		 *
		 * 删除操作不更新索引页（只有 VACUUM 会清除        
		 * TID），因此删除所引发的 VM 位清除与下方   
		 * 测试并未串行化，我们可能看到显著滞后的值。
		 * 不过我们不必立即关心这次删除，因为元组在删除
		 * 事务提交或语句结束前均可见（若是我们自  
		 * 己的事务）。无论哪种，清除该位之后 VM
		 * 缓冲区上的锁都已被释放（充当写屏障）。而要  
		 * 让我们的快照包含删除事务（使元组不可见），  
		 * 我们必定是在那之后才获取过                 
		 * ProcArrayLock，                                               
		 * 这充当了读屏障。                                
		 *
		 * 值得经历这些复杂操作，以避免锁定 VM 缓冲区， 
		 * 因为那可能造成严重的争用。                 
		 */
		if (!VM_ALL_VISIBLE(scandesc->heapRelation,
							ItemPointerGetBlockNumber(tid),
							&node->ioss_VMBuffer))
		{
			/*
			 * 糟糕，我们必须访问堆来检查可见性。 
			 */
			InstrCountTuples2(node, 1);
			if (!index_fetch_heap(scandesc, node->ioss_TableSlot))
				continue;		/* 无可见元组，试下一索引条目*/

			ExecClearTuple(node->ioss_TableSlot);

			/*
			 * 这里只支持 MVCC 快照，一旦找到可见条目，就 
			 * 无需继续沿 HOT 链追踪。若想允许那样做，则  
			 * 需要保留更多状态，记住下次不调用            
			 * index_getnext_tid。                                           
			 */
			if (scandesc->xs_heap_continue)
				elog(ERROR, "non-MVCC snapshots are not supported in index-only scans");

			/*
			 * 注意：此时我们按 scandesc->xs_cbuf 的记录持有堆
			 * 页上的 pin。现在可以释放它，但不清楚这       
			 * 做是否划算。下一个索引条目可能需要访问      
			 * 同一个堆页。                                
			 */

			tuple_from_heap = true;
		}

		/*
		 * 用索引中的数据填充扫描元组槽。数据可能以     
		 * HeapTuple 或 IndexTuple 两种格式之一提供。可以        
		 * 设想索引 AM 会同时填充两个字段，此时我们优    
		 * 先堆格式，因为用它填槽可能稍微便宜些。     
		 */
		if (scandesc->xs_hitup)
		{
			/*
			 * 我们不想费神去验证所提供的元组是否恰好具有
			 * 槽的格式，但似乎值得对字段数量做一个     
			 * 快速检查。               
			 */
			Assert(slot->tts_tupleDescriptor->natts ==
				   scandesc->xs_hitupdesc->natts);
			ExecForceStoreHeapTuple(scandesc->xs_hitup, slot, false);
		}
		else if (scandesc->xs_itup)
			StoreIndexTuple(node, slot, scandesc->xs_itup, scandesc->xs_itupdesc);
		else
			elog(ERROR, "no data returned for index-only scan");

		/*
		 * 若索引有损，就必须重新检查索引条件。     
		 */
		if (scandesc->xs_recheck)
		{
			econtext->ecxt_scantuple = slot;
			if (!ExecQualAndReset(node->recheckqual, econtext))
			{
				/* 重新检查未通过，丢弃并循环取下一个 */
				InstrCountFiltered2(node, 1);
				continue;
			}
		}

		/*
		 * 我们目前不支持重新检查 ORDER BY 距离。（原理 
		 * 上，如果索引能够支持取回原始被索引值，它也
		 * 应能给出精确的距离计算。因此在此添加用于 
		 * 重新检查/重新排序的代码是否值得并不明确。
		 * 但至少应在有人尝试时，                                  
		 * 抛出一个错误。）            
		 */
		if (scandesc->numberOfOrderBys > 0 && scandesc->xs_recheckorderby)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("lossy distance functions are not supported in index-only scans")));

		/*
		 * 若我们未访问堆，就需要显式地获取谓词锁，     
		 * 就像访问过一样。目前我们在页面级进行。        
		 */
		if (!tuple_from_heap)
			PredicateLockPage(scandesc->heapRelation,
							  ItemPointerGetBlockNumber(tid),
							  estate->es_snapshot);

		return slot;
	}

	/*
	 * 如果执行到这里，说明索引扫描已失败，扫描         
	 * 结束..  
	 */
	return ExecClearTuple(slot);
}

/*
 * StoreIndexTuple
 *		用索引元组中的数据填充槽。      
 *
 * 某个时候这可能成为普遍有用的功能，但目前   
 * 我们在别处并不需要它。    
 */
static void
StoreIndexTuple(IndexOnlyScanState *node, TupleTableSlot *slot,
				IndexTuple itup, TupleDesc itupdesc)
{
	/*
	 * 注意：在 index_deform_tuple 中我们必须使用 AM 提供的     
	 * tupdesc，而不是槽的 tupdesc，以防后者具有不同的   
	 * 数据类型（btree 的 name_ops 尤其会这样）。它们       
	 * 最好具有相同的列数，且数据类型兼容，                 
	 * 这一点我们无法轻易检查。        
	 */
	Assert(slot->tts_tupleDescriptor->natts == itupdesc->natts);

	ExecClearTuple(slot);
	index_deform_tuple(itup, itupdesc, slot->tts_values, slot->tts_isnull);

	/*
	 * 把所有以 cstring 形式存储的 name 列拷回一个            
	 * NAMEDATALEN 字节大小的分配中。我们将此分支标记       
	 * 为不太可能发生，因为 "name" 通常只用于系统目录， 
	 * 这只会是运行在这些目录或其他带 name 列索引的        
	 * 查询 
	 */
	if (unlikely(node->ioss_NameCStringAttNums != NULL))
	{
		int			attcount = node->ioss_NameCStringCount;

		for (int idx = 0; idx < attcount; idx++)
		{
			int			attnum = node->ioss_NameCStringAttNums[idx];
			Name		name;

			/* 跳过空 Datum  */
			if (slot->tts_isnull[attnum])
				continue;

			/* 分配 NAMEDATALEN 并把 Datum 拷入该内存              */
			name = (Name) MemoryContextAlloc(node->ss.ps.ps_ExprContext->ecxt_per_tuple_memory,
											 NAMEDATALEN);

			/* 用 namestrcpy 把所有尾部字节补零    */
			namestrcpy(name, DatumGetCString(slot->tts_values[attnum]));
			slot->tts_values[attnum] = NameGetDatum(name);
		}
	}

	ExecStoreVirtualTuple(slot);
}

/*
 * IndexOnlyRecheck -- 在 EvalPlanQual 中重查元组的访问方法例程   
 *
 * 这实际上不可能发生，因为索引无法提供任何潜在    
 * EvalPlanQual 目标关系所必需的 CTID。若真的发生，EPQ      
 * 代码会传给我们错误的数据，即堆元组而非索引     
 * 元组，故报错。                        
 */
static bool
IndexOnlyRecheck(IndexOnlyScanState *node, TupleTableSlot *slot)
{
	elog(ERROR, "EvalPlanQual recheck is not supported in index-only scans");
	return false;				/* 让编译器安静  */
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyScan(node)
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecIndexOnlyScan(PlanState *pstate)
{
	IndexOnlyScanState *node = castNode(IndexOnlyScanState, pstate);

	/*
	 * 如果我们有运行时键且尚未设置，现在就设置它们。  
	 */
	if (node->ioss_NumRuntimeKeys != 0 && !node->ioss_RuntimeKeysReady)
		ExecReScan((PlanState *) node);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) IndexOnlyNext,
					(ExecScanRecheckMtd) IndexOnlyRecheck);
}

/* ----------------------------------------------------------------
 *		ExecReScanIndexOnlyScan(node)
 *
 *		重新计算任何取值依赖于运行时信息的扫描键，
 *		然后重新扫描被索引的关系。                         
 *
 *		以前更新扫描键在 ExecUpdateIndexScanKeys 内 
 *		将其并入 ReScan 使索引与关系/通用流的重新
 *		扫描更加统一。                                         
 * ----------------------------------------------------------------
 */
void
ExecReScanIndexOnlyScan(IndexOnlyScanState *node)
{
	/*
	 * 如果我们在做运行时键计算（即任何索引键值都不
	 * 是简单 Const），就计算新的键值。但首先，重置上下 
	 * 文以免扫描每个外层元组时泄漏内存。             
	 * 注意这假定在每次调用时都会重算 *所有* 运行时       
	 * 键。       
	 */
	if (node->ioss_NumRuntimeKeys != 0)
	{
		ExprContext *econtext = node->ioss_RuntimeContext;

		ResetExprContext(econtext);
		ExecIndexEvalRuntimeKeys(econtext,
								 node->ioss_RuntimeKeys,
								 node->ioss_NumRuntimeKeys);
	}
	node->ioss_RuntimeKeysReady = true;

	/*重置索引扫描*/
	if (node->ioss_ScanDesc)
		index_rescan(node->ioss_ScanDesc,
					 node->ioss_ScanKeys, node->ioss_NumScanKeys,
					 node->ioss_OrderByKeys, node->ioss_NumOrderByKeys);

	ExecScanReScan(&node->ss);
}


/* ----------------------------------------------------------------
 *		ExecEndIndexOnlyScan
 * ----------------------------------------------------------------
 */
void
ExecEndIndexOnlyScan(IndexOnlyScanState *node)
{
	Relation	indexRelationDesc;
	IndexScanDesc indexScanDesc;

	/*
	 * 从节点提取信息            
	 */
	indexRelationDesc = node->ioss_RelationDesc;
	indexScanDesc = node->ioss_ScanDesc;

	/* 如 VM pin 存在则释放     */
	if (node->ioss_VMBuffer != InvalidBuffer)
	{
		ReleaseBuffer(node->ioss_VMBuffer);
		node->ioss_VMBuffer = InvalidBuffer;
	}

	/*
	 * 释放表达式上下文…为死代码，见 ExecFreeExprContext 
	 */
#ifdef NOT_USED
	ExecFreeExprContext(&node->ss.ps);
	if (node->ioss_RuntimeContext)
		FreeExprContext(node->ioss_RuntimeContext, true);
#endif

	/*
	 * 清空元组表槽位      
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * 关闭索引关系（若未打开则为无操作）  
	 */
	if (indexScanDesc)
		index_endscan(indexScanDesc);
	if (indexRelationDesc)
		index_close(indexRelationDesc, NoLock);
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyMarkPos
 *
 * 注意：我们假定没有调用者会在读到至少一个元组      
 * 之前设置标记。否则 ioss_ScanDesc 可能仍为 NULL。    
 * ----------------------------------------------------------------
 */
void
ExecIndexOnlyMarkPos(IndexOnlyScanState *node)
{
	EState	   *estate = node->ss.ps.state;
	EPQState   *epqstate = estate->es_epq_active;

	if (epqstate != NULL)
	{
		/*
		 * 我们正处于 EvalPlanQual 重新检查之中。如果该关系 
		 * 存在测试元组，那么我们根本不应访问索引。我们 
		 * 而需保存并在之后恢复 relsubs_done 标志的    
		 * 状态，这样才可能重新取回测试元组。然而，鉴于
		 * 没有调用者会在扫描开始设置标记这一假设，只
		 * 会在 relsubs_done[i] 已设置的情况下到这里，    
		 * 因此无需保存任何状态。          
		 */
		Index		scanrelid = ((Scan *) node->ss.ps.plan)->scanrelid;

		Assert(scanrelid > 0);
		if (epqstate->relsubs_slot[scanrelid - 1] != NULL ||
			epqstate->relsubs_rowmark[scanrelid - 1] != NULL)
		{
			/* 验证上述论断     */
			if (!epqstate->relsubs_done[scanrelid - 1])
				elog(ERROR, "unexpected ExecIndexOnlyMarkPos call in EPQ recheck");
			return;
		}
	}

	index_markpos(node->ioss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyRestrPos
 * ----------------------------------------------------------------
 */
void
ExecIndexOnlyRestrPos(IndexOnlyScanState *node)
{
	EState	   *estate = node->ss.ps.state;
	EPQState   *epqstate = estate->es_epq_active;

	if (estate->es_epq_active != NULL)
	{
		/* 参见 ExecIndexMarkPos 注释   */
		Index		scanrelid = ((Scan *) node->ss.ps.plan)->scanrelid;

		Assert(scanrelid > 0);
		if (epqstate->relsubs_slot[scanrelid - 1] != NULL ||
			epqstate->relsubs_rowmark[scanrelid - 1] != NULL)
		{
			/* 验证上述论断     */
			if (!epqstate->relsubs_done[scanrelid - 1])
				elog(ERROR, "unexpected ExecIndexOnlyRestrPos call in EPQ recheck");
			return;
		}
	}

	index_restrpos(node->ioss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecInitIndexOnlyScan
 *
 *		初始化索引扫描的状态信息，创建          
 *		扫描键，打开基础关系与索引关系。  
 *
 *		注意：索引扫描有两套状态信息，因为       
 *			  我们必须同时跟踪基础关系              
 *			  与索引关系
 * ----------------------------------------------------------------
 */
IndexOnlyScanState *
ExecInitIndexOnlyScan(IndexOnlyScan *node, EState *estate, int eflags)
{
	IndexOnlyScanState *indexstate;
	Relation	currentRelation;
	Relation	indexRelation;
	LOCKMODE	lockmode;
	TupleDesc	tupDesc;
	int			indnkeyatts;
	int			namecount;

	/*
	 * 创建状态结构    
	 */
	indexstate = makeNode(IndexOnlyScanState);
	indexstate->ss.ps.plan = (Plan *) node;
	indexstate->ss.ps.state = estate;
	indexstate->ss.ps.ExecProcNode = ExecIndexOnlyScan;

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
	indexstate->ss.ss_currentScanDesc = NULL;	/* 此处无堆扫描*/

	/*
	 * 使用规划器生成的 indextlist 构建扫描元组类型。
	 * 我们用它而非索引物理元组描述符，因为它包 
	 * 含的是存储列的类型而非原始数据的类型。（不管  
	 * 怎样，返回合适的数据终究是 AM                            
	 * 的责任。）       
	 */
	tupDesc = ExecTypeFromTL(node->indextlist);
	ExecInitScanTupleSlot(estate, &indexstate->ss, tupDesc,
						  &TTSOpsVirtual);

	/*
	 * 我们还需要另一个槽，采用适合表 AM 的格式，          
	 * 用于需要从表中取元组来重新检查可见性时。           
	 */
	indexstate->ioss_TableSlot =
		ExecAllocTableSlot(&estate->es_tupleTable,
						   RelationGetDescr(currentRelation),
						   table_slot_callbacks(currentRelation));

	/*
	 * 初始化结果类型和投影信息。节点的目标列表将        
	 * 包含 varno = INDEX_VAR 的 Var，引用扫描元组。        
	 */
	ExecInitResultTypeTL(&indexstate->ss.ps);
	ExecAssignScanProjectionInfoWithVarno(&indexstate->ss, INDEX_VAR);

	/*
	 * 初始化子表达式       
	 *
	 * 注意：我们不初始化全部 indexorderby 表达式，           
	 * 只初始化运行时键对应部分（见下）。 
	 */
	indexstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) indexstate);
	indexstate->recheckqual =
		ExecInitQual(node->recheckqual, (PlanState *) indexstate);

	/*
	 * 如果我们只是执行 EXPLAIN（即不打算运行计划），就 
	 * 在这里停止。这使得索引顾问插件可以 EXPLAIN 一个     
	 * 引用不存在索引的计划。 
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return indexstate;

	/* 打开索引关系。    */
	lockmode = exec_rt_fetch(node->scan.scanrelid, estate)->rellockmode;
	indexRelation = index_open(node->indexid, lockmode);
	indexstate->ioss_RelationDesc = indexRelation;

	/*
	 * 初始化索引特定的扫描状态
	 */
	indexstate->ioss_RuntimeKeysReady = false;
	indexstate->ioss_RuntimeKeys = NULL;
	indexstate->ioss_NumRuntimeKeys = 0;

	/*
	 * 根据索引限定条件构建索引扫描键         
	 */
	ExecIndexBuildScanKeys((PlanState *) indexstate,
						   indexRelation,
						   node->indexqual,
						   false,
						   &indexstate->ioss_ScanKeys,
						   &indexstate->ioss_NumScanKeys,
						   &indexstate->ioss_RuntimeKeys,
						   &indexstate->ioss_NumRuntimeKeys,
						   NULL,	/* 无 ArrayKeys*/
						   NULL);

	/*
	 * 任何 ORDER BY 表达式都必须以同样方式转为扫描键  
	 */
	ExecIndexBuildScanKeys((PlanState *) indexstate,
						   indexRelation,
						   node->indexorderby,
						   true,
						   &indexstate->ioss_OrderByKeys,
						   &indexstate->ioss_NumOrderByKeys,
						   &indexstate->ioss_RuntimeKeys,
						   &indexstate->ioss_NumRuntimeKeys,
						   NULL,	/* 无 ArrayKeys*/
						   NULL);

	/*
	 * 如果有运行时键，就需要一个 ExprContext 来对它们     
	 * 求值。节点的标准上下文不行，因为我们想为每个元 
	 * 组重置该上下文。所以，再构建一个同类的上下文... 
	 * -tgl 7/11/00
	 */
	if (indexstate->ioss_NumRuntimeKeys != 0)
	{
		ExprContext *stdecontext = indexstate->ss.ps.ps_ExprContext;

		ExecAssignExprContext(estate, &indexstate->ss.ps);
		indexstate->ioss_RuntimeContext = indexstate->ss.ps.ps_ExprContext;
		indexstate->ss.ps.ps_ExprContext = stdecontext;
	}
	else
	{
		indexstate->ioss_RuntimeContext = NULL;
	}

	indexstate->ioss_NameCStringAttNums = NULL;
	indnkeyatts = indexRelation->rd_index->indnkeyatts;
	namecount = 0;

	/*
	 * btree 的 "name" 类型使用 text_ops，这使得索引键中  
	 * 存储 cstring 而非 name。这里我们以通用的方式检测      
	 * 这种情况，以防其他索引 AM 想做同样的优化。检查    
	 * opcintype 为 NAMEOID 且索引元组描述符为 CSTRINGOID       
	 * 的操作类。若发现任何此类情况，就创建一个数组，  
	 * 标记其中每个操作类的索引属性号。StoreIndexTuple()     
	 * 负责把 name Datum 拷入 NAMEDATALEN 字节的分配中。      
	 */

	/* 先统计这类索引键的数目          */
	for (int attnum = 0; attnum < indnkeyatts; attnum++)
	{
		if (indexRelation->rd_att->attrs[attnum].atttypid == CSTRINGOID &&
			indexRelation->rd_opcintype[attnum] == NAMEOID)
			namecount++;
	}

	if (namecount > 0)
	{
		int			idx = 0;

		/*
		 * 现在创建一个数组，用于标记需要从 cstring 转换为
		 * name 的键的属性号。                
		 */
		indexstate->ioss_NameCStringAttNums = (AttrNumber *)
									palloc(sizeof(AttrNumber) * namecount);

		for (int attnum = 0; attnum < indnkeyatts; attnum++)
		{
			if (indexRelation->rd_att->attrs[attnum].atttypid == CSTRINGOID &&
				indexRelation->rd_opcintype[attnum] == NAMEOID)
				indexstate->ioss_NameCStringAttNums[idx++] = (AttrNumber) attnum;
		}
	}

	indexstate->ioss_NameCStringCount = namecount;

	/*
	 * 完成。
	 */
	return indexstate;
}

/* ----------------------------------------------------------------
 *		并行仅索引扫描支持     
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecIndexOnlyScanEstimate
 *
 *		计算并行查询 DSM 中我们所需的空间量， 
 *		并把我们的需求告知 pcxt->estimator。        
 * ----------------------------------------------------------------
 */
void
ExecIndexOnlyScanEstimate(IndexOnlyScanState *node,
						  ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;

	node->ioss_PscanLen = index_parallelscan_estimate(node->ioss_RelationDesc,
													  estate->es_snapshot);
	shm_toc_estimate_chunk(&pcxt->estimator, node->ioss_PscanLen);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyScanInitializeDSM
 *
 *		设置一个并行仅索引扫描描述符。
 * ----------------------------------------------------------------
 */
void
ExecIndexOnlyScanInitializeDSM(IndexOnlyScanState *node,
							   ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;
	ParallelIndexScanDesc piscan;

	piscan = shm_toc_allocate(pcxt->toc, node->ioss_PscanLen);
	index_parallelscan_initialize(node->ss.ss_currentRelation,
								  node->ioss_RelationDesc,
								  estate->es_snapshot,
								  piscan);
	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id, piscan);
	node->ioss_ScanDesc =
		index_beginscan_parallel(node->ss.ss_currentRelation,
								 node->ioss_RelationDesc,
								 node->ioss_NumScanKeys,
								 node->ioss_NumOrderByKeys,
								 piscan);
	node->ioss_ScanDesc->xs_want_itup = true;
	node->ioss_VMBuffer = InvalidBuffer;

	/*
	 * 如果没有要计算的运行时键，或它们已就绪，就把   
	 * 扫描键传给索引 AM。  
	 */
	if (node->ioss_NumRuntimeKeys == 0 || node->ioss_RuntimeKeysReady)
		index_rescan(node->ioss_ScanDesc,
					 node->ioss_ScanKeys, node->ioss_NumScanKeys,
					 node->ioss_OrderByKeys, node->ioss_NumOrderByKeys);
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyScanReInitializeDSM
 *
 *		在开始全新扫描前重置共享状态。    
 * ----------------------------------------------------------------
 */
void
ExecIndexOnlyScanReInitializeDSM(IndexOnlyScanState *node,
								 ParallelContext *pcxt)
{
	index_parallelrescan(node->ioss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyScanInitializeWorker
 *
 *		把相关信息从 TOC 拷贝到 planstate 中。 
 * ----------------------------------------------------------------
 */
void
ExecIndexOnlyScanInitializeWorker(IndexOnlyScanState *node,
								  ParallelWorkerContext *pwcxt)
{
	ParallelIndexScanDesc piscan;

	piscan = shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, false);
	node->ioss_ScanDesc =
		index_beginscan_parallel(node->ss.ss_currentRelation,
								 node->ioss_RelationDesc,
								 node->ioss_NumScanKeys,
								 node->ioss_NumOrderByKeys,
								 piscan);
	node->ioss_ScanDesc->xs_want_itup = true;

	/*
	 * 如果没有要计算的运行时键，或它们已就绪，就把   
	 * 扫描键传给索引 AM。  
	 */
	if (node->ioss_NumRuntimeKeys == 0 || node->ioss_RuntimeKeysReady)
		index_rescan(node->ioss_ScanDesc,
					 node->ioss_ScanKeys, node->ioss_NumScanKeys,
					 node->ioss_OrderByKeys, node->ioss_NumOrderByKeys);
}
