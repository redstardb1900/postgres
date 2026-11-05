/*-------------------------------------------------------------------------
 *
 * nodeBitmapHeapscan.c
 *	  支撑关系位图扫描的例程               
 *
 * 注意：该计划类型只能与符合 MVCC 的快照一起使          
 * 用（即常规快照，而非 SnapshotAny 或其他特殊快        
 * 照）。原因在于：由于索引扫描与堆扫描是解耦       
 * 的，无法保证当真正访问某个堆 TID 时，促使这       
 * 次访问的索引元组仍然存在。因此该元组可能也    
 * 已不存在（这没有问题，因为 heap_fetch 会应对）        
 * --- 但更糟的是，元组槽可能已被复用于更新的      
 * 元组。在 MVCC 快照下，更新的元组必定无法通过     
 * 时间限定条件，所以不会被错误地返回；但换成         
 * 其他任何快照，我们可能返回不满足所需索引限     
 * 定条件的元组。          
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeBitmapHeapscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *		ExecBitmapHeapScan			用位图信息扫描关系       
 *		ExecBitmapHeapNext			上述例程主力 
 *		ExecInitBitmapHeapScan		创建并初始化状态信息。  
 *		ExecReScanBitmapHeapScan	准备重新扫描计划。 
 *		ExecEndBitmapHeapScan		释放所有存储。
 */
#include "postgres.h"

#include <math.h>

#include "access/relscan.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/visibilitymap.h"
#include "executor/execdebug.h"
#include "executor/nodeBitmapHeapscan.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/spccache.h"

static TupleTableSlot *BitmapHeapNext(BitmapHeapScanState *node);
static inline void BitmapDoneInitializingSharedState(ParallelBitmapHeapState *pstate);
static inline void BitmapAdjustPrefetchIterator(BitmapHeapScanState *node,
												TBMIterateResult *tbmres);
static inline void BitmapAdjustPrefetchTarget(BitmapHeapScanState *node);
static inline void BitmapPrefetch(BitmapHeapScanState *node,
								  TableScanDesc scan);
static bool BitmapShouldInitializeSharedState(ParallelBitmapHeapState *pstate);


/* ----------------------------------------------------------------
 *		BitmapHeapNext
 *
 *		从 BitmapHeapScan 节点的 currentRelation 中取回下一      
 * 个元组                                                       
 */
static TupleTableSlot *
BitmapHeapNext(BitmapHeapScanState *node)
{
	ExprContext *econtext;
	TableScanDesc scan;
	TIDBitmap  *tbm;
	TBMIterator *tbmiterator = NULL;
	TBMSharedIterator *shared_tbmiterator = NULL;
	TBMIterateResult *tbmres;
	TupleTableSlot *slot;
	ParallelBitmapHeapState *pstate = node->pstate;
	dsa_area   *dsa = node->ss.ps.state->es_query_dsa;

	/*
	 * 从索引扫描节点中抽取必要信息        
	 */
	econtext = node->ss.ps.ps_ExprContext;
	slot = node->ss.ss_ScanTupleSlot;
	scan = node->ss.ss_currentScanDesc;
	tbm = node->tbm;
	if (pstate == NULL)
		tbmiterator = node->tbmiterator;
	else
		shared_tbmiterator = node->shared_tbmiterator;
	tbmres = node->tbmres;

	/*
	 * 如果尚未执行底层的索引扫描，就执行它，并开        
	 * 始对位图的迭代。      
	 *
	 * 对于预取，我们使用*两个*迭代器：一个用于我   
	 * 们实际扫描的页面，另一个领先前者预取。     
	 * node->prefetch_pages 精确跟踪预取迭代器领先的             
	 * 页面数。另外，node->prefetch_target 跟踪期望的       
	 * 预取距离，它从小开始并逐步增大到                     
	 * node->prefetch_maximum。这是为了避免在扫描因                
	 * LIMIT 而只取几行就停时大量预取。           
	 */
	if (!node->initialized)
	{
		if (!pstate)
		{
			tbm = (TIDBitmap *) MultiExecProcNode(outerPlanState(node));

			if (!tbm || !IsA(tbm, TIDBitmap))
				elog(ERROR, "unrecognized result from subplan");

			node->tbm = tbm;
			node->tbmiterator = tbmiterator = tbm_begin_iterate(tbm);
			node->tbmres = tbmres = NULL;

#ifdef USE_PREFETCH
			if (node->prefetch_maximum > 0)
			{
				node->prefetch_iterator = tbm_begin_iterate(tbm);
				node->prefetch_pages = 0;
				node->prefetch_target = -1;
			}
#endif							/* USE_PREFETCH */
		}
		else
		{
			/*
			 * 领导者立刻从函数中返回，而其他进程则被
			 * 阻塞，直到领导者填充好 TBM 并唤醒它们          
			 * 为止  
			 */
			if (BitmapShouldInitializeSharedState(pstate))
			{
				tbm = (TIDBitmap *) MultiExecProcNode(outerPlanState(node));
				if (!tbm || !IsA(tbm, TIDBitmap))
					elog(ERROR, "unrecognized result from subplan");

				node->tbm = tbm;

				/*
				 * 准备迭代 TBM。这将返回迭代器状态的    
				 * dsa_pointer，该指针会被多个进程              
				 * 用来共同迭代。                 
				 */
				pstate->tbmiterator = tbm_prepare_shared_iterate(tbm);
#ifdef USE_PREFETCH
				if (node->prefetch_maximum > 0)
				{
					pstate->prefetch_iterator =
						tbm_prepare_shared_iterate(tbm);

					/*
					 * 这里无需互斥锁，因为尚未唤醒别的      
					 * 进程 
					 */
					pstate->prefetch_pages = 0;
					pstate->prefetch_target = -1;
				}
#endif

				/* 共享状态已初始化好，唤醒其他进程。     */
				BitmapDoneInitializingSharedState(pstate);
			}

			/* 分配一个私有迭代器，并把共享状态附到其上  */
			node->shared_tbmiterator = shared_tbmiterator =
				tbm_attach_shared_iterate(dsa, pstate->tbmiterator);
			node->tbmres = tbmres = NULL;

#ifdef USE_PREFETCH
			if (node->prefetch_maximum > 0)
			{
				node->shared_prefetch_iterator =
					tbm_attach_shared_iterate(dsa, pstate->prefetch_iterator);
			}
#endif							/* USE_PREFETCH */
		}
		node->initialized = true;
	}

	for (;;)
	{
		bool		skip_fetch;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 如有需要，取下一页结果 
		 */
		if (tbmres == NULL)
		{
			if (!pstate)
				node->tbmres = tbmres = tbm_iterate(tbmiterator);
			else
				node->tbmres = tbmres = tbm_shared_iterate(shared_tbmiterator);
			if (tbmres == NULL)
			{
				/* 位图中没有更多条目了*/
				break;
			}

			BitmapAdjustPrefetchIterator(node, tbmres);

			if (tbmres->ntuples >= 0)
				node->exact_pages++;
			else
				node->lossy_pages++;

			/*
			 * 若我们不需要堆上的任何字段，位图条目不需  
			 * 要复查，且页上所有元组对本事务可见，      
			 * 就可以跳过获取堆页面。                         
			 *
			 * XXX: 在 tableam 之上做这些检查是破坏分层的行 
			 * 为，或许将来应该把它们移到 tableam 之下。  
			 */
			skip_fetch = (node->can_skip_fetch &&
						  !tbmres->recheck &&
						  VM_ALL_VISIBLE(node->ss.ss_currentRelation,
										 tbmres->blockno,
										 &node->vmbuffer));

			if (skip_fetch)
			{
				/* skip_fetch 情形下不可能有损   */
				Assert(tbmres->ntuples >= 0);

				/*
				 * 本页面的元组数会被放入            
				 * node->return_empty_tuples 
				 */
				node->return_empty_tuples = tbmres->ntuples;
			}
			else if (!table_scan_bitmap_next_block(scan, tbmres))
			{
				/* AM 认为该块无效，跳过             */
				continue;
			}

			/* 调整预取目标         */
			BitmapAdjustPrefetchTarget(node);
		}
		else
		{
			/*
			 * 继续处理之前获取的页面。   
			 */

#ifdef USE_PREFETCH

			/*
			 * 甚至在第二页之前，也尽量预取至少几页，     
			 * 如果我们不打算在首个元组后就停止读取。  
			 */
			if (!pstate)
			{
				if (node->prefetch_target < node->prefetch_maximum)
					node->prefetch_target++;
			}
			else if (pstate->prefetch_target < node->prefetch_maximum)
			{
				/* 更新共享状态时持有自旋锁      */
				SpinLockAcquire(&pstate->mutex);
				if (pstate->prefetch_target < node->prefetch_maximum)
					pstate->prefetch_target++;
				SpinLockRelease(&pstate->mutex);
			}
#endif							/* USE_PREFETCH */
		}

		/*
		 * 我们在获取当前页面*之后*才发出预取请求，        
		 * 以尽量避免预取干扰主 I/O。此外，只有当我们      
		 * 确定当前页面上还有事情要做时才应这样做，否    
		 * 则我们可能把即将真正要请求的同一页面白白预   
		 * 取一遍。                               
		 *
		 * XXX: 在 tableam 之上做这些检查是破坏分层的行 
		 * 为，或许将来应该把它们移到 tableam 之下。  
		 */
		BitmapPrefetch(node, scan);

		if (node->return_empty_tuples > 0)
		{
			/*
			 * 若不必获取元组，就直接返回空值。       
			 */
			ExecStoreAllNullTuple(slot);

			if (--node->return_empty_tuples == 0)
			{
				/* 下一轮没有更多要返回的元组了 */
				node->tbmres = tbmres = NULL;
			}
		}
		else
		{
			/*
			 * 尝试从 AM 获取元组。   
			 */
			if (!table_scan_bitmap_next_tuple(scan, tbmres, slot))
			{
				/* 此页上没有更多可看的了    */
				node->tbmres = tbmres = NULL;
				continue;
			}

			/*
			 * 若有损信息，每个元组都要重新检查限    
			 * 定条件。              
			 */
			if (tbmres->recheck)
			{
				econtext->ecxt_scantuple = slot;
				if (!ExecQualAndReset(node->bitmapqualorig, econtext))
				{
					/* 重查失败，丢弃它并循环回去取下一个 */
					InstrCountFiltered2(node, 1);
					ExecClearTuple(slot);
					continue;
				}
			}
		}

		/* 可以返回该元组   */
		return slot;
	}

	/*
	 * 如果走到这里，说明我们已到扫描末端。 
	 */
	return ExecClearTuple(slot);
}

/*
 *	BitmapDoneInitializingSharedState - 共享状态已初始化   
 *
 *	此时领导者已经填充好 TBM 并初始化了共享状态，        
 *	所以要唤醒其他进程。          
 */
static inline void
BitmapDoneInitializingSharedState(ParallelBitmapHeapState *pstate)
{
	SpinLockAcquire(&pstate->mutex);
	pstate->state = BM_FINISHED;
	SpinLockRelease(&pstate->mutex);
	ConditionVariableBroadcast(&pstate->cv);
}

/*
 *	BitmapAdjustPrefetchIterator - 调整预取迭代器       
 */
static inline void
BitmapAdjustPrefetchIterator(BitmapHeapScanState *node,
							 TBMIterateResult *tbmres)
{
#ifdef USE_PREFETCH
	ParallelBitmapHeapState *pstate = node->pstate;

	if (pstate == NULL)
	{
		TBMIterator *prefetch_iterator = node->prefetch_iterator;

		if (node->prefetch_pages > 0)
		{
			/* 主迭代器已把差距缩短了一页               */
			node->prefetch_pages--;
		}
		else if (prefetch_iterator)
		{
			/* 不要让预取迭代器落后于主迭代器            */
			TBMIterateResult *tbmpre = tbm_iterate(prefetch_iterator);

			if (tbmpre == NULL || tbmpre->blockno != tbmres->blockno)
				elog(ERROR, "prefetch and main iterators are out of sync");
		}
		return;
	}

	if (node->prefetch_maximum > 0)
	{
		TBMSharedIterator *prefetch_iterator = node->shared_prefetch_iterator;

		SpinLockAcquire(&pstate->mutex);
		if (pstate->prefetch_pages > 0)
		{
			pstate->prefetch_pages--;
			SpinLockRelease(&pstate->mutex);
		}
		else
		{
			/* 在迭代之前释放互斥锁     */
			SpinLockRelease(&pstate->mutex);

			/*
			 * 共享模式无法保证主迭代器与预取迭代器的 
			 * 当前块号相同。我们正在预取的块号有可能由  
			 * 另一个进程处理。                             
			 * 因此，我们在这里不像非并行情形那样在       
			 * 此校验块                                               
			 * 号  
			 */
			if (prefetch_iterator)
				tbm_shared_iterate(prefetch_iterator);
		}
	}
#endif							/* USE_PREFETCH */
}

/*
 * BitmapAdjustPrefetchTarget - 调整预取目标        
 *
 * 如果预取目标尚未达到最大值，则增大它。注意
 * 取回首个页面/元组后我们会把它增到零，  
 * 取回第二个元组后加到一，之后随着后续页面的
 * 取而翻倍。                       
 */
static inline void
BitmapAdjustPrefetchTarget(BitmapHeapScanState *node)
{
#ifdef USE_PREFETCH
	ParallelBitmapHeapState *pstate = node->pstate;

	if (pstate == NULL)
	{
		if (node->prefetch_target >= node->prefetch_maximum)
			 /* 不再继续增大         */ ;
		else if (node->prefetch_target >= node->prefetch_maximum / 2)
			node->prefetch_target = node->prefetch_maximum;
		else if (node->prefetch_target > 0)
			node->prefetch_target *= 2;
		else
			node->prefetch_target++;
		return;
	}

	/* 先做无锁检查，以节省自旋锁获取的次数。 */
	if (pstate->prefetch_target < node->prefetch_maximum)
	{
		SpinLockAcquire(&pstate->mutex);
		if (pstate->prefetch_target >= node->prefetch_maximum)
			 /* 不再继续增大         */ ;
		else if (pstate->prefetch_target >= node->prefetch_maximum / 2)
			pstate->prefetch_target = node->prefetch_maximum;
		else if (pstate->prefetch_target > 0)
			pstate->prefetch_target *= 2;
		else
			pstate->prefetch_target++;
		SpinLockRelease(&pstate->mutex);
	}
#endif							/* USE_PREFETCH */
}

/*
 * BitmapPrefetch - 若 prefetch_pages 落后于 prefetch_target 则预取
 */
static inline void
BitmapPrefetch(BitmapHeapScanState *node, TableScanDesc scan)
{
#ifdef USE_PREFETCH
	ParallelBitmapHeapState *pstate = node->pstate;

	if (pstate == NULL)
	{
		TBMIterator *prefetch_iterator = node->prefetch_iterator;

		if (prefetch_iterator)
		{
			while (node->prefetch_pages < node->prefetch_target)
			{
				TBMIterateResult *tbmpre = tbm_iterate(prefetch_iterator);
				bool		skip_fetch;

				if (tbmpre == NULL)
				{
					/* 无更多页面可预取  */
					tbm_end_iterate(prefetch_iterator);
					node->prefetch_iterator = NULL;
					break;
				}
				node->prefetch_pages++;

				/*
				 * 若预计不必真正读取此堆页面，则跳过      
				 * 这次预取调用，但仍照常继续运行预取      
				 * 逻辑。（不递增 prefetch_pages 会更好       
				 * 吗？）       
				 *
				 * 这依赖于索引 AM 会为此堆页面给出与    
				 * 当前页面相同的复查标志；这并非必然，但
				 * 在许多情况下都是                                  
				 * 成立的。              
				 */
				skip_fetch = (node->can_skip_fetch &&
							  (node->tbmres ? !node->tbmres->recheck : false) &&
							  VM_ALL_VISIBLE(node->ss.ss_currentRelation,
											 tbmpre->blockno,
											 &node->pvmbuffer));

				if (!skip_fetch)
					PrefetchBuffer(scan->rs_rd, MAIN_FORKNUM, tbmpre->blockno);
			}
		}

		return;
	}

	if (pstate->prefetch_pages < pstate->prefetch_target)
	{
		TBMSharedIterator *prefetch_iterator = node->shared_prefetch_iterator;

		if (prefetch_iterator)
		{
			while (1)
			{
				TBMIterateResult *tbmpre;
				bool		do_prefetch = false;
				bool		skip_fetch;

				/*
				 * 在互斥锁下重查。若其他进程已预取足够， 
				 * 那我们就无需做任何事了。                    
				 */
				SpinLockAcquire(&pstate->mutex);
				if (pstate->prefetch_pages < pstate->prefetch_target)
				{
					pstate->prefetch_pages++;
					do_prefetch = true;
				}
				SpinLockRelease(&pstate->mutex);

				if (!do_prefetch)
					return;

				tbmpre = tbm_shared_iterate(prefetch_iterator);
				if (tbmpre == NULL)
				{
					/* 无更多页面可预取  */
					tbm_end_shared_iterate(prefetch_iterator);
					node->shared_prefetch_iterator = NULL;
					break;
				}

				/* 同上，如果预计不需要页面，则跳过预取*/
				skip_fetch = (node->can_skip_fetch &&
							  (node->tbmres ? !node->tbmres->recheck : false) &&
							  VM_ALL_VISIBLE(node->ss.ss_currentRelation,
											 tbmpre->blockno,
											 &node->pvmbuffer));

				if (!skip_fetch)
					PrefetchBuffer(scan->rs_rd, MAIN_FORKNUM, tbmpre->blockno);
			}
		}
	}
#endif							/* USE_PREFETCH */
}

/*
 * BitmapHeapRecheck -- 在 EvalPlanQual 中复查元组的访问方法例程   
 */
static bool
BitmapHeapRecheck(BitmapHeapScanState *node, TupleTableSlot *slot)
{
	ExprContext *econtext;

	/*
	 * 从索引扫描节点中抽取必要信息        
	 */
	econtext = node->ss.ps.ps_ExprContext;

	/* 该元组满足原始的 qual 条件吗？        */
	econtext->ecxt_scantuple = slot;
	return ExecQualAndReset(node->bitmapqualorig, econtext);
}

/* ----------------------------------------------------------------
 *		ExecBitmapHeapScan(node)
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecBitmapHeapScan(PlanState *pstate)
{
	BitmapHeapScanState *node = castNode(BitmapHeapScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) BitmapHeapNext,
					(ExecScanRecheckMtd) BitmapHeapRecheck);
}

/* ----------------------------------------------------------------
 *		ExecReScanBitmapHeapScan(node)
 * ----------------------------------------------------------------
 */
void
ExecReScanBitmapHeapScan(BitmapHeapScanState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/* 重新扫描以释放页面 pin*/
	table_rescan(node->ss.ss_currentScanDesc, NULL);

	/* 有则释放位图和缓冲区     */
	if (node->tbmiterator)
		tbm_end_iterate(node->tbmiterator);
	if (node->prefetch_iterator)
		tbm_end_iterate(node->prefetch_iterator);
	if (node->shared_tbmiterator)
		tbm_end_shared_iterate(node->shared_tbmiterator);
	if (node->shared_prefetch_iterator)
		tbm_end_shared_iterate(node->shared_prefetch_iterator);
	if (node->tbm)
		tbm_free(node->tbm);
	if (node->vmbuffer != InvalidBuffer)
		ReleaseBuffer(node->vmbuffer);
	if (node->pvmbuffer != InvalidBuffer)
		ReleaseBuffer(node->pvmbuffer);
	node->tbm = NULL;
	node->tbmiterator = NULL;
	node->tbmres = NULL;
	node->prefetch_iterator = NULL;
	node->initialized = false;
	node->shared_tbmiterator = NULL;
	node->shared_prefetch_iterator = NULL;
	node->vmbuffer = InvalidBuffer;
	node->pvmbuffer = InvalidBuffer;

	ExecScanReScan(&node->ss);

	/*
	 * 若子节点的 chgParam 非空，则第一次 ExecProcNode       
	 * 会重扫计划。 
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}

/* ----------------------------------------------------------------
 *		ExecEndBitmapHeapScan
 * ----------------------------------------------------------------
 */
void
ExecEndBitmapHeapScan(BitmapHeapScanState *node)
{
	TableScanDesc scanDesc;

	/*
	 * 从节点中抽取信息         
	 */
	scanDesc = node->ss.ss_currentScanDesc;

	/*
	 * 释放 exprcontext  
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * 清空元组表槽         
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * 关闭子计划    
	 */
	ExecEndNode(outerPlanState(node));

	/*
	 * 若有则释放位图和缓冲区 
	 */
	if (node->tbmiterator)
		tbm_end_iterate(node->tbmiterator);
	if (node->prefetch_iterator)
		tbm_end_iterate(node->prefetch_iterator);
	if (node->tbm)
		tbm_free(node->tbm);
	if (node->shared_tbmiterator)
		tbm_end_shared_iterate(node->shared_tbmiterator);
	if (node->shared_prefetch_iterator)
		tbm_end_shared_iterate(node->shared_prefetch_iterator);
	if (node->vmbuffer != InvalidBuffer)
		ReleaseBuffer(node->vmbuffer);
	if (node->pvmbuffer != InvalidBuffer)
		ReleaseBuffer(node->pvmbuffer);

	/*
	 * 关闭堆扫描
	 */
	table_endscan(scanDesc);
}

/* ----------------------------------------------------------------
 *		ExecInitBitmapHeapScan
 *
 *		初始化扫描的状态信息。        
 * ----------------------------------------------------------------
 */
BitmapHeapScanState *
ExecInitBitmapHeapScan(BitmapHeapScan *node, EState *estate, int eflags)
{
	BitmapHeapScanState *scanstate;
	Relation	currentRelation;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 断言调用者没有要求不安全的快照 --- 参见文件头  
	 * 部注释。 
	 */
	Assert(IsMVCCSnapshot(estate->es_snapshot));

	/*
	 * 创建状态结构    
	 */
	scanstate = makeNode(BitmapHeapScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecBitmapHeapScan;

	scanstate->tbm = NULL;
	scanstate->tbmiterator = NULL;
	scanstate->tbmres = NULL;
	scanstate->return_empty_tuples = 0;
	scanstate->vmbuffer = InvalidBuffer;
	scanstate->pvmbuffer = InvalidBuffer;
	scanstate->exact_pages = 0;
	scanstate->lossy_pages = 0;
	scanstate->prefetch_iterator = NULL;
	scanstate->prefetch_pages = 0;
	scanstate->prefetch_target = 0;
	scanstate->pscan_len = 0;
	scanstate->initialized = false;
	scanstate->shared_tbmiterator = NULL;
	scanstate->shared_prefetch_iterator = NULL;
	scanstate->pstate = NULL;

	/*
	 * 遗憾的是，事实证明下面的优化并未考虑         
	 * 并发清理移除的 TID。并发清理能移除死
	 * TID 并把页面置为 ALL_VISIBLE，而这些死 TID 仍被
	 * 位图引用。这会导致 !need_tuples 的扫描返回     
	 * 过多的元                                                
	 * 组。      
	 *
	 * 所以在后向分支干脆禁用该优化。        
	 * 删除所有相关代码太具侵入性（且向后移植
	 * 补丁会非常痛苦）。              
	 */
	scanstate->can_skip_fetch = false;
#ifdef NOT_ANYMORE
	/*
	 * 如果我们不需要表的任何列，就可能跳过获取堆  
	 * 页面——无论是为检查不可索引的 qual 还是为返回  
	 * 数据。这个测试有点简单化，因为它检查的是更 
	 * 强的条件：根本不存在 qual 或返回 tlist。但在大        
	 * 多数情况下，大概不值得在这方面更费心。   
	 */
	scanstate->can_skip_fetch = (node->scan.plan.qual == NIL &&
								 node->scan.plan.targetlist == NIL);
#endif

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * 打开扫描关系    
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid, eflags);

	/*
	 * 初始化子节点    
	 */
	outerPlanState(scanstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * 从关系描述符中获取扫描类型。     
	 */
	ExecInitScanTupleSlot(estate, &scanstate->ss,
						  RelationGetDescr(currentRelation),
						  table_slot_callbacks(currentRelation));

	/*
	 * 初始化结果类型和投影。     
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * 初始化子表达式       
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);
	scanstate->bitmapqualorig =
		ExecInitQual(node->bitmapqualorig, (PlanState *) scanstate);

	/*
	 * 若已配置，则为该表空间的最大预取数；否则  
	 * 为 effective_io_concurrency GUC 的当前值。                
	 */
	scanstate->prefetch_maximum =
		get_tablespace_io_concurrency(currentRelation->rd_rel->reltablespace);

	scanstate->ss.ss_currentRelation = currentRelation;

	scanstate->ss.ss_currentScanDesc = table_beginscan_bm(currentRelation,
														  estate->es_snapshot,
														  0,
														  NULL);

	/*
	 * 完成。
	 */
	return scanstate;
}

/*----------------
 *		BitmapShouldInitializeSharedState
 *
 *		第一个到达此处并看到状态仍为 BM_INITIAL 的进      
 *		程将成为并行位图扫描的领导者，并负责填充   
 *		TIDBitmap。其他进程将被条件变量阻塞，直到领导    
 *		者唤醒它们。                                                  
 * ---------------
 */
static bool
BitmapShouldInitializeSharedState(ParallelBitmapHeapState *pstate)
{
	SharedBitmapState state;

	while (1)
	{
		SpinLockAcquire(&pstate->mutex);
		state = pstate->state;
		if (pstate->state == BM_INITIAL)
			pstate->state = BM_INPROGRESS;
		SpinLockRelease(&pstate->mutex);

		/* 位图完成或我们是领导者，则退出。*/
		if (state != BM_INPROGRESS)
			break;

		/* 等待领导者唤醒我们。     */
		ConditionVariableSleep(&pstate->cv, WAIT_EVENT_PARALLEL_BITMAP_SCAN);
	}

	ConditionVariableCancelSleep();

	return (state == BM_INITIAL);
}

/* ----------------------------------------------------------------
 *		ExecBitmapHeapEstimate
 *
 *		计算我们在并行查询 DSM 中需多少空间， 
 *		并把我们的需求告知 pcxt->estimator。        
 * ----------------------------------------------------------------
 */
void
ExecBitmapHeapEstimate(BitmapHeapScanState *node,
					   ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;

	node->pscan_len = add_size(offsetof(ParallelBitmapHeapState,
										phs_snapshot_data),
							   EstimateSnapshotSpace(estate->es_snapshot));

	shm_toc_estimate_chunk(&pcxt->estimator, node->pscan_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/* ----------------------------------------------------------------
 *		ExecBitmapHeapInitializeDSM
 *
 *		建立一个并行位图堆扫描描述符。 
 * ----------------------------------------------------------------
 */
void
ExecBitmapHeapInitializeDSM(BitmapHeapScanState *node,
							ParallelContext *pcxt)
{
	ParallelBitmapHeapState *pstate;
	EState	   *estate = node->ss.ps.state;
	dsa_area   *dsa = node->ss.ps.state->es_query_dsa;

	/* 没有 DSA 就没有 worker；什么都不初始化。       */
	if (dsa == NULL)
		return;

	pstate = shm_toc_allocate(pcxt->toc, node->pscan_len);

	pstate->tbmiterator = 0;
	pstate->prefetch_iterator = 0;

	/* 初始化互斥锁   */
	SpinLockInit(&pstate->mutex);
	pstate->prefetch_pages = 0;
	pstate->prefetch_target = 0;
	pstate->state = BM_INITIAL;

	ConditionVariableInit(&pstate->cv);
	SerializeSnapshot(estate->es_snapshot, pstate->phs_snapshot_data);

	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id, pstate);
	node->pstate = pstate;
}

/* ----------------------------------------------------------------
 *		ExecBitmapHeapReInitializeDSM
 *
 *		在开始全新扫描前重置共享状态。    
 * ----------------------------------------------------------------
 */
void
ExecBitmapHeapReInitializeDSM(BitmapHeapScanState *node,
							  ParallelContext *pcxt)
{
	ParallelBitmapHeapState *pstate = node->pstate;
	dsa_area   *dsa = node->ss.ps.state->es_query_dsa;

	/* 没有 DSA 就没有 worker；什么都不做。     */
	if (dsa == NULL)
		return;

	pstate->state = BM_INITIAL;

	if (DsaPointerIsValid(pstate->tbmiterator))
		tbm_free_shared_area(dsa, pstate->tbmiterator);

	if (DsaPointerIsValid(pstate->prefetch_iterator))
		tbm_free_shared_area(dsa, pstate->prefetch_iterator);

	pstate->tbmiterator = InvalidDsaPointer;
	pstate->prefetch_iterator = InvalidDsaPointer;
}

/* ----------------------------------------------------------------
 *		ExecBitmapHeapInitializeWorker
 *
 *		把 TOC 中的相关信息复制到 planstate。  
 * ----------------------------------------------------------------
 */
void
ExecBitmapHeapInitializeWorker(BitmapHeapScanState *node,
							   ParallelWorkerContext *pwcxt)
{
	ParallelBitmapHeapState *pstate;
	Snapshot	snapshot;

	Assert(node->ss.ps.state->es_query_dsa != NULL);

	pstate = shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, false);
	node->pstate = pstate;

	snapshot = RestoreSnapshot(pstate->phs_snapshot_data);
	table_scan_update_snapshot(node->ss.ss_currentScanDesc, snapshot);
}
