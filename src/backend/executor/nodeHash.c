/*-------------------------------------------------------------------------
 *
 * nodeHash.c
 *	  为哈希连接散列关系的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeHash.c
 *
 * 并行性说明见 nodeHashjoin.c。      
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *		MultiExecHash 生成关系的内存中哈希表。              
 *		ExecInitHash 初始化节点及子节点。
 *		ExecEndHash 关闭节点及子节点     
 */

#include "postgres.h"

#include <math.h>
#include <limits.h>

#include "access/htup_details.h"
#include "access/parallel.h"
#include "catalog/pg_statistic.h"
#include "commands/tablespace.h"
#include "executor/execdebug.h"
#include "executor/hashjoin.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "utils/dynahash.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

static void ExecHashIncreaseNumBatches(HashJoinTable hashtable);
static void ExecHashIncreaseNumBuckets(HashJoinTable hashtable);
static void ExecParallelHashIncreaseNumBatches(HashJoinTable hashtable);
static void ExecParallelHashIncreaseNumBuckets(HashJoinTable hashtable);
static void ExecHashBuildSkewHash(HashJoinTable hashtable, Hash *node,
								  int mcvsToUse);
static void ExecHashSkewTableInsert(HashJoinTable hashtable,
									TupleTableSlot *slot,
									uint32 hashvalue,
									int bucketNumber);
static void ExecHashRemoveNextSkewBucket(HashJoinTable hashtable);

static void *dense_alloc(HashJoinTable hashtable, Size size);
static HashJoinTuple ExecParallelHashTupleAlloc(HashJoinTable hashtable,
												size_t size,
												dsa_pointer *shared);
static void MultiExecPrivateHash(HashState *node);
static void MultiExecParallelHash(HashState *node);
static inline HashJoinTuple ExecParallelHashFirstTuple(HashJoinTable hashtable,
													   int bucketno);
static inline HashJoinTuple ExecParallelHashNextTuple(HashJoinTable hashtable,
													  HashJoinTuple tuple);
static inline void ExecParallelHashPushTuple(dsa_pointer_atomic *head,
											 HashJoinTuple tuple,
											 dsa_pointer tuple_shared);
static void ExecParallelHashJoinSetUpBatches(HashJoinTable hashtable, int nbatch);
static void ExecParallelHashEnsureBatchAccessors(HashJoinTable hashtable);
static void ExecParallelHashRepartitionFirst(HashJoinTable hashtable);
static void ExecParallelHashRepartitionRest(HashJoinTable hashtable);
static HashMemoryChunk ExecParallelHashPopChunkQueue(HashJoinTable hashtable,
													 dsa_pointer *shared);
static bool ExecParallelHashTuplePrealloc(HashJoinTable hashtable,
										  int batchno,
										  size_t size);
static void ExecParallelHashMergeCounters(HashJoinTable hashtable);
static void ExecParallelHashCloseBatchAccessors(HashJoinTable hashtable);


/* ----------------------------------------------------------------
 *		ExecHash
 *
 *		为形式合规而设的桩  
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecHash(PlanState *pstate)
{
	elog(ERROR, "Hash node does not support ExecProcNode call convention");
	return NULL;
}

/* ----------------------------------------------------------------
 *		MultiExecHash
 *
 *		为哈希连接构建哈希表，若需要不止一个   
 *		批，则做分区。      
 * ----------------------------------------------------------------
 */
Node *
MultiExecHash(HashState *node)
{
	/* 必须自行提供 instrumentation 支持    */
	if (node->ps.instrument)
		InstrStartNode(node->ps.instrument);

	if (node->parallel_state != NULL)
		MultiExecParallelHash(node);
	else
		MultiExecPrivateHash(node);

	/* 必须自行提供 instrumentation 支持    */
	if (node->ps.instrument)
		InstrStopNode(node->ps.instrument, node->hashtable->partialTuples);

	/*
	 * 我们不直接返回哈希表，因为它并非 Node 的子类型， 
	 * 因此会违反 MultiExecProcNode API。作为替代，我们的   
	 * 父节点 Hashjoin 应当知道如何从我们的节点状态中       
	 * 把它取出。这很丑，但不值得清理，因为 Hashjoin    
	 * 此外对 Hash 的了解还多得多。  
	 */
	return NULL;
}

/* ----------------------------------------------------------------
 *		MultiExecPrivateHash
 *
 *		并行无感知版本，构建后端私有的         
 *		哈希表与（必要时）批文件。   
 * ----------------------------------------------------------------
 */
static void
MultiExecPrivateHash(HashState *node)
{
	PlanState  *outerNode;
	List	   *hashkeys;
	HashJoinTable hashtable;
	TupleTableSlot *slot;
	ExprContext *econtext;
	uint32		hashvalue;

	/*
	 * 从节点取状态信息
	 */
	outerNode = outerPlanState(node);
	hashtable = node->hashtable;

	/*
	 * 设表达式上下文 
	 */
	hashkeys = node->hashkeys;
	econtext = node->ps.ps_ExprContext;

	/*
	 * 从 Hash 节点下方的节点取得所有元组，并插入        
	 * 哈希表或临时文件。
	 */
	for (;;)
	{
		slot = ExecProcNode(outerNode);
		if (TupIsNull(slot))
			break;
		/* 我们必须计算哈希值       */
		econtext->ecxt_outertuple = slot;
		if (ExecHashGetHashValue(hashtable, econtext, hashkeys,
								 false, hashtable->keepNulls,
								 &hashvalue))
		{
			int			bucketNumber;

			bucketNumber = ExecHashGetSkewBucket(hashtable, hashvalue);
			if (bucketNumber != INVALID_SKEW_BUCKET_NO)
			{
				/* 这是倾斜元组，故放入该哈希表        */
				ExecHashSkewTableInsert(hashtable, slot, hashvalue,
										bucketNumber);
				hashtable->skewTuples += 1;
			}
			else
			{
				/* 不适用倾斜优化，因此正常插入           */
				ExecHashTableInsert(hashtable, slot, hashvalue);
			}
			hashtable->totalTuples += 1;
		}
	}

	/* 必要时调整哈希表大小（NTUP_PER_BUCKET 超出） */
	if (hashtable->nbuckets != hashtable->nbuckets_optimal)
		ExecHashIncreaseNumBuckets(hashtable);

	/* 把桶计入 spaceUsed（在 EXPLAIN ANALYZE 中报告）          */
	hashtable->spaceUsed += hashtable->nbuckets * sizeof(HashJoinTuple);
	if (hashtable->spaceUsed > hashtable->spacePeak)
		hashtable->spacePeak = hashtable->spaceUsed;

	hashtable->partialTuples = hashtable->totalTuples;
}

/* ----------------------------------------------------------------
 *		MultiExecParallelHash
 *
 *		并行感知版本，构建共享哈希表以及        
 *		（必要时）批文件，动用一组协作          
 *		后端的共同努力。       
 * ----------------------------------------------------------------
 */
static void
MultiExecParallelHash(HashState *node)
{
	ParallelHashJoinState *pstate;
	PlanState  *outerNode;
	List	   *hashkeys;
	HashJoinTable hashtable;
	TupleTableSlot *slot;
	ExprContext *econtext;
	uint32		hashvalue;
	Barrier    *build_barrier;
	int			i;

	/*
	 * 从节点取状态信息
	 */
	outerNode = outerPlanState(node);
	hashtable = node->hashtable;

	/*
	 * 设表达式上下文 
	 */
	hashkeys = node->hashkeys;
	econtext = node->ps.ps_ExprContext;

	/*
	 * 同步并行哈希表的构建。此时我们知道共享哈希表    
	 * 已由 ExecHashTableCreate() 建好或在建，     
	 * 但我们不知道各同伴是已从那里返回，还是仍在    
	 * MultiExecParallelHash() 里；若是，又进行到了              
	 * 哪一步。为此我们检查 build_barrier 的阶段，然后      
	 * 跳到构建算法中正确的步骤。           
	 */
	pstate = hashtable->parallel_state;
	build_barrier = &pstate->build_barrier;
	Assert(BarrierPhase(build_barrier) >= PHJ_BUILD_ALLOCATE);
	switch (BarrierPhase(build_barrier))
	{
		case PHJ_BUILD_ALLOCATE:

			/*
			 * 要么我刚在 ExecHashTableCreate() 中分配  
			 * 初始哈希表，要么别人正在这么做。无论哪种 
			 * 情况，都等大家到达此处后才能继续。     
			 */
			BarrierArriveAndWait(build_barrier, WAIT_EVENT_HASH_BUILD_ALLOCATE);
			/* Fall through. */

		case PHJ_BUILD_HASH_INNER:

			/*
			 * 到了该开始做哈希的时候；如果我们刚到达  
			 * 此处，则说明哈希已在进行，就加入其中。  
			 * 做哈希期间，我们必须随时准备帮忙增加批    
			 * 数或桶数；而如果我们到达时该项工作已在  
			 * 进行，就必须立刻帮忙把它完成，以便之后能   
			 * 安全地访问下方的批与                             
			 * 桶。
			 */
			if (PHJ_GROW_BATCHES_PHASE(BarrierAttach(&pstate->grow_batches_barrier)) !=
				PHJ_GROW_BATCHES_ELECT)
				ExecParallelHashIncreaseNumBatches(hashtable);
			if (PHJ_GROW_BUCKETS_PHASE(BarrierAttach(&pstate->grow_buckets_barrier)) !=
				PHJ_GROW_BUCKETS_ELECT)
				ExecParallelHashIncreaseNumBuckets(hashtable);
			ExecParallelHashEnsureBatchAccessors(hashtable);
			ExecParallelHashTableSetCurrentBatch(hashtable, 0);
			for (;;)
			{
				slot = ExecProcNode(outerNode);
				if (TupIsNull(slot))
					break;
				econtext->ecxt_outertuple = slot;
				if (ExecHashGetHashValue(hashtable, econtext, hashkeys,
										 false, hashtable->keepNulls,
										 &hashvalue))
					ExecParallelHashTableInsert(hashtable, slot, hashvalue);
				hashtable->partialTuples++;
			}

			/*
			 * 确保我们写入磁盘的任何元组在有人尝试   
			 * 加载它们之前对他人可见。    
			 */
			for (i = 0; i < hashtable->nbatch; ++i)
				sts_end_write(hashtable->batches[i].inner_tuples);

			/*
			 * 更新共享计数器。我们需要准确的总元组数     
			 * 来控制空表优化。                
			 */
			ExecParallelHashMergeCounters(hashtable);

			BarrierDetach(&pstate->grow_buckets_barrier);
			BarrierDetach(&pstate->grow_batches_barrier);

			/*
			 * 等待所有人完成构建并刷写完文件与计数     
			 * 器。   
			 */
			if (BarrierArriveAndWait(build_barrier,
									 WAIT_EVENT_HASH_BUILD_HASH_INNER))
			{
				/*
				 * 推选一个后端来禁止任何后续增长。此后批
				 * 数就此固定。构建时我们已确保之后重新加  
				 * 载回时它们能装进内存预算——或者我们     
				 * 尝试过却因检测到极端倾斜而最终           
				 * 放弃
				 */
				pstate->growth = PHJ_GROWTH_DISABLED;
			}
	}

	/*
	 * 我们尚未附加到批上。大家对维度与内层元             
	 * 组数都有一致认知（用于空表优化）。       
	 */
	hashtable->curbatch = -1;
	hashtable->nbuckets = pstate->nbuckets;
	hashtable->log2_nbuckets = my_log2(hashtable->nbuckets);
	hashtable->totalTuples = pstate->total_tuples;

	/*
	 * 除非我们已彻底完成且批状态已被释放，否            
	 * 则确保有访问器  
	 */
	if (BarrierPhase(build_barrier) < PHJ_BUILD_FREE)
		ExecParallelHashEnsureBatchAccessors(hashtable);

	/*
	 * 下一个同步点位于 ExecHashJoin 的 HJ_BUILD_HASHTABLE          
	 * 分支，它会把构建阶段推进到 PHJ_BUILD_RUN（如果      
	 * 尚未）。   
	 */
	Assert(BarrierPhase(build_barrier) == PHJ_BUILD_HASH_OUTER ||
		   BarrierPhase(build_barrier) == PHJ_BUILD_RUN ||
		   BarrierPhase(build_barrier) == PHJ_BUILD_FREE);
}

/* ----------------------------------------------------------------
 *		ExecInitHash
 *
 *		Hash 节点初始化例程
 * ----------------------------------------------------------------
 */
HashState *
ExecInitHash(Hash *node, EState *estate, int eflags)
{
	HashState  *hashstate;

	/* 检查不受支持的标志 */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 创建状态结构    
	 */
	hashstate = makeNode(HashState);
	hashstate->ps.plan = (Plan *) node;
	hashstate->ps.state = estate;
	hashstate->ps.ExecProcNode = ExecHash;
	hashstate->hashtable = NULL;
	hashstate->hashkeys = NIL;	/* 将由父节点 HashJoin 设置*/

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &hashstate->ps);

	/*
	 * 初始化子节点    
	 */
	outerPlanState(hashstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * 初始化我们的结果槽与类型。无需构建投影，    
	 * 因为本节点不做投影。           
	 */
	ExecInitResultTupleSlotTL(&hashstate->ps, &TTSOpsMinimalTuple);
	hashstate->ps.ps_ProjInfo = NULL;

	/*
	 * 初始化子表达式       
	 */
	Assert(node->plan.qual == NIL);
	hashstate->hashkeys =
		ExecInitExprList(node->hashkeys, (PlanState *) hashstate);

	return hashstate;
}

/* ---------------------------------------------------------------
 *		ExecEndHash
 *
 *		Hash 节点的清理例程    
 * ----------------------------------------------------------------
 */
void
ExecEndHash(HashState *node)
{
	PlanState  *outerPlan;

	/*
	 * 释放上下文 
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * 关闭子计划      
	 */
	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}


/* ----------------------------------------------------------------
 *		ExecHashTableCreate
 *
 *		为哈希连接创建空的哈希表数据结构。   
 * ----------------------------------------------------------------
 */
HashJoinTable
ExecHashTableCreate(HashState *state, List *hashOperators, List *hashCollations, bool keepNulls)
{
	Hash	   *node;
	HashJoinTable hashtable;
	Plan	   *outerNode;
	size_t		space_allowed;
	int			nbuckets;
	int			nbatch;
	double		rows;
	int			num_skew_mcvs;
	int			log2_nbuckets;
	int			nkeys;
	int			i;
	ListCell   *ho;
	ListCell   *hc;
	MemoryContext oldcxt;

	/*
	 * 获取待哈希关系的大小信息（它是本节点的            
	 * "outer" 子树，但也是哈希连接的内关系）。              
	 * 计算哈希表的恰当大小。              
	 */
	node = (Hash *) state->ps.plan;
	outerNode = outerPlan(node);

	/*
	 * 如果这是带有部分计划的共享哈希表，就不能       
	 * 用 outerNode->plan_rows 来估计其大小。我们需要            
	 * 对所有部分计划副本的总行数做出估计。     
	 */
	rows = node->plan.parallel_aware ? node->rows_total : outerNode->plan_rows;

	ExecChooseHashTableSize(rows, outerNode->plan_width,
							OidIsValid(node->skewTable),
							state->parallel_state != NULL,
							state->parallel_state != NULL ?
							state->parallel_state->nparticipants - 1 : 0,
							&space_allowed,
							&nbuckets, &nbatch, &num_skew_mcvs);

	/* nbuckets 必须是 2 的幂   */
	log2_nbuckets = my_log2(nbuckets);
	Assert(nbuckets == (1 << log2_nbuckets));

	/*
	 * 初始化哈希表控制块。          
	 *
	 * 哈希表控制块只是从执行器的每查询内存          
	 * 上下文中 palloc 出来的。其余一切都应保留在         
	 * 下属 hashCxt、batchCxt 或 spillCxt。
	 */
	hashtable = palloc_object(HashJoinTableData);
	hashtable->nbuckets = nbuckets;
	hashtable->nbuckets_original = nbuckets;
	hashtable->nbuckets_optimal = nbuckets;
	hashtable->log2_nbuckets = log2_nbuckets;
	hashtable->log2_nbuckets_optimal = log2_nbuckets;
	hashtable->buckets.unshared = NULL;
	hashtable->keepNulls = keepNulls;
	hashtable->skewEnabled = false;
	hashtable->skewBucket = NULL;
	hashtable->skewBucketLen = 0;
	hashtable->nSkewBuckets = 0;
	hashtable->skewBucketNums = NULL;
	hashtable->nbatch = nbatch;
	hashtable->curbatch = 0;
	hashtable->nbatch_original = nbatch;
	hashtable->nbatch_outstart = nbatch;
	hashtable->growEnabled = true;
	hashtable->totalTuples = 0;
	hashtable->partialTuples = 0;
	hashtable->skewTuples = 0;
	hashtable->innerBatchFile = NULL;
	hashtable->outerBatchFile = NULL;
	hashtable->spaceUsed = 0;
	hashtable->spacePeak = 0;
	hashtable->spaceAllowed = space_allowed;
	hashtable->spaceUsedSkew = 0;
	hashtable->spaceAllowedSkew =
		hashtable->spaceAllowed * SKEW_HASH_MEM_PERCENT / 100;
	hashtable->chunks = NULL;
	hashtable->current_chunk = NULL;
	hashtable->parallel_state = state->parallel_state;
	hashtable->area = state->ps.state->es_query_dsa;
	hashtable->batches = NULL;

#ifdef HJDEBUG
	printf("Hashjoin %p: initial nbatch = %d, nbuckets = %d\n",
		   hashtable, nbatch, nbuckets);
#endif

	/*
	 * 创建临时内存上下文，用于存放哈希表的工作存储，  
	 * 详见 executor/hashjoin.h。              
	 */
	hashtable->hashCxt = AllocSetContextCreate(CurrentMemoryContext,
											   "HashTableContext",
											   ALLOCSET_DEFAULT_SIZES);

	hashtable->batchCxt = AllocSetContextCreate(hashtable->hashCxt,
												"HashBatchContext",
												ALLOCSET_DEFAULT_SIZES);

	hashtable->spillCxt = AllocSetContextCreate(hashtable->hashCxt,
												"HashSpillContext",
												ALLOCSET_DEFAULT_SIZES);

	/* 分配生命期贯穿整个哈希连接的数据          */

	oldcxt = MemoryContextSwitchTo(hashtable->hashCxt);

	/*
	 * 获取每个哈希键要使用的哈希函数的信息。另外     
	 * 还需记住连接算子是否严格。        
	 */
	nkeys = list_length(hashOperators);
	hashtable->outer_hashfunctions = palloc_array(FmgrInfo, nkeys);
	hashtable->inner_hashfunctions = palloc_array(FmgrInfo, nkeys);
	hashtable->hashStrict = palloc_array(bool, nkeys);
	hashtable->collations = palloc_array(Oid, nkeys);
	i = 0;
	forboth(ho, hashOperators, hc, hashCollations)
	{
		Oid			hashop = lfirst_oid(ho);
		Oid			left_hashfn;
		Oid			right_hashfn;

		if (!get_op_hash_functions(hashop, &left_hashfn, &right_hashfn))
			elog(ERROR, "could not find hash function for hash operator %u",
				 hashop);
		fmgr_info(left_hashfn, &hashtable->outer_hashfunctions[i]);
		fmgr_info(right_hashfn, &hashtable->inner_hashfunctions[i]);
		hashtable->hashStrict[i] = op_strict(hashop);
		hashtable->collations[i] = lfirst_oid(hc);
		i++;
	}

	if (nbatch > 1 && hashtable->parallel_state == NULL)
	{
		MemoryContext oldctx;

		/*
		 * 在 hashCxt 中分配并初始化文件数组（并行情形      
		 * 不需要，它们改用共享 tuplestore 而非原始文件）  
		 */
		oldctx = MemoryContextSwitchTo(hashtable->spillCxt);

		hashtable->innerBatchFile = palloc0_array(BufFile *, nbatch);
		hashtable->outerBatchFile = palloc0_array(BufFile *, nbatch);

		MemoryContextSwitchTo(oldctx);

		/* 这些文件要到需要时才会打开…   */
		/* …但要确保已为它们设置好临时表空间             */
		PrepareTempTablespaces();
	}

	MemoryContextSwitchTo(oldcxt);

	if (hashtable->parallel_state)
	{
		ParallelHashJoinState *pstate = hashtable->parallel_state;
		Barrier    *build_barrier;

		/*
		 * 附加到构建屏障。对应的分离操作在                   
		 * ExecHashTableDetach 中。注意我们还不会附加到 
		 * 批 0 的 batch_barrier。我们稍后才附加，并让它以     
		 * PHJ_BATCH_PROBE 阶段启动，因为批 0 是预先分配、      
		 * 在哈希期间加载的（标准的混合哈希连接  
		 * 算法），协调工作将用 build_barrier 来做。    
		 */
		build_barrier = &pstate->build_barrier;
		BarrierAttach(build_barrier);

		/*
		 * 到目前为止我们还完全不知道是否有其他参与    
		 * 者，若有，它们正在做哪个阶段。此时我们唯一   
		 * 关心的是是否有人已经创建了 SharedHashJoinBatch   
		 * 对象以及批 0 的哈希表。如有必要，现在会推选 
		 * 一个后端来做这件事。                      
		 */
		if (BarrierPhase(build_barrier) == PHJ_BUILD_ELECT &&
			BarrierArriveAndWait(build_barrier, WAIT_EVENT_HASH_BUILD_ELECT))
		{
			pstate->nbatch = nbatch;
			pstate->space_allowed = space_allowed;
			pstate->growth = PHJ_GROWTH_OK;

			/* 设置用于协调各批的共享状态。        */
			ExecParallelHashJoinSetUpBatches(hashtable, nbatch);

			/*
			 * 预分配批 0 哈希表，以便做哈希时能直接  
			 * 加载它。           
			 */
			pstate->nbuckets = nbuckets;
			ExecParallelHashTableAlloc(hashtable, 0);
		}

		/*
		 * 下一个并行哈希同步点位于              
		 * MultiExecParallelHash() 中，它会把同步一路推进到  
		 * PHJ_BUILD_RUN。在此之前调用方不得把控制权     
		 * 从此执行器节点返回。     
		 */
	}
	else
	{
		/*
		 * 为首次扫描的空间分配准备好上下文；在其中      
		 * 分配哈希桶数组，并把每个桶置为"空"。 
		 */
		MemoryContextSwitchTo(hashtable->batchCxt);

		hashtable->buckets.unshared = palloc0_array(HashJoinTuple, nbuckets);

		/*
		 * 如果可能且需要不止一个批，则为倾斜优化做    
		 * 准备。（在单批连接中，这样做没有意义。）   
		 *     
		 */
		if (nbatch > 1)
			ExecHashBuildSkewHash(hashtable, node, num_skew_mcvs);

		MemoryContextSwitchTo(oldcxt);
	}

	return hashtable;
}


/*
 * 根据待哈希关系的估计大小（行数与平均行宽）       
 * 计算哈希表的恰当大小。                            
 *
 * 导出它以供规划器的 costsize.c 使用。             
 */

/* 目标桶负载（每桶的元组数）   */
#define NTUP_PER_BUCKET			1

void
ExecChooseHashTableSize(double ntuples, int tupwidth, bool useskew,
						bool try_combined_hash_mem,
						int parallel_workers,
						size_t *space_allowed,
						int *numbuckets,
						int *numbatches,
						int *num_skew_mcvs)
{
	int			tupsize;
	double		inner_rel_bytes;
	size_t		hash_table_bytes;
	size_t		bucket_bytes;
	size_t		max_pointers;
	int			nbatch = 1;
	int			nbuckets;
	double		dbuckets;

	/* 无信息时强制取合理的关系大小 */
	if (ntuples <= 0.0)
		ntuples = 1000.0;

	/*
	 * 根据元组在哈希表中的占用空间估计 tupsize……        
	 * 注意这未计入任何 palloc 开销。对 spaceUsed 的各          
	 * 种操作也不计 palloc 开销。
	 */
	tupsize = HJTUPLE_OVERHEAD +
		MAXALIGN(SizeofMinimalTupleHeader) +
		MAXALIGN(tupwidth);
	inner_rel_bytes = ntuples * tupsize;

	/*
	 * 由 GUC 计算内存哈希表大小上限。     
	 */
	hash_table_bytes = get_hash_memory_limit();

	/*
	 * 并行哈希尝试使用所有 worker 合计的 hash_mem 来      
	 * 避免分批。如果那样行不通，就退回到每个              
	 * worker 一份 hash_mem，并尝试并行处理批。
	 */
	if (try_combined_hash_mem)
	{
		/* 小心，这可能会溢出 size_t  */
		double		newlimit;

		newlimit = (double) hash_table_bytes * (double) (parallel_workers + 1);
		newlimit = Min(newlimit, (double) SIZE_MAX);
		hash_table_bytes = (size_t) newlimit;
	}

	*space_allowed = hash_table_bytes;

	/*
	 * 如果倾斜优化可行，则估计在允许的内存中能         
	 * 容纳的倾斜桶数量，并相应扣减假定的主哈希        
	 * 表可用空间。                            
	 *
	 * 我们乐观地假定每个倾斜桶将包含一个内关系        
	 * 元组。若实际证明该值偏低，我们会在运行时           
	 * 通过减少倾斜桶的数量来恢复。        
	 *
	 * hashtable->skewBucket 拥有的 HashSkewBucket 指针               
	 * 最多可达到我们所允许 MCV 数量的 8 倍，因为         
	 * ExecHashBuildSkewHash 会取整到 2 的幂，再乘 4 以减少     
	 * 冲突。  
	 */
	if (useskew)
	{
		size_t		bytes_per_mcv;
		size_t		skew_mcvs;

		/*----------
		 * 计算 hash_table_bytes 中能容纳的 MCV 数量      
		 *
		 * 除数：  
		 * 哈希元组大小 +  
		 * 每 MCV 的 skewBucket[] 最坏大小 +  
		 * skewBucketNums[] 条目大小 + 
		 * 倾斜桶结构本身的大小   
		 *----------
		 */
		bytes_per_mcv = tupsize +
			(8 * sizeof(HashSkewBucket *)) +
			sizeof(int) +
			SKEW_BUCKET_OVERHEAD;
		skew_mcvs = hash_table_bytes / bytes_per_mcv;

		/*
		 * 现在乘以 SKEW_HASH_MEM_PERCENT（按此顺序进行，      
		 * 这样就不必担心乘法中的 size_t 溢出）       
		 */
		skew_mcvs = (skew_mcvs * SKEW_HASH_MEM_PERCENT) / 100;

		/* 现在限制到整数范围*/
		skew_mcvs = Min(skew_mcvs, INT_MAX);

		*num_skew_mcvs = (int) skew_mcvs;

		/* 从 hash_table_bytes 中扣减倾斜表所需的数量          */
		if (skew_mcvs > 0)
			hash_table_bytes -= skew_mcvs * bytes_per_mcv;
	}
	else
		*num_skew_mcvs = 0;

	/*
	 * 把 nbuckets 设为使得内存装满时（假定单个批）平      
	 * 均桶负载达到 NTUP_PER_BUCKET；但要对该值加以限制，   
	 * 使我们试图分配的指针数组不超过 hash_table_bytes 及     
	 * MaxAllocSize。  
	 *
	 * 注意 nbuckets 与 nbatch 都须为 2 的幂，以便加快  
	 * ExecHashGetBucketAndBatch速度
	 */
	max_pointers = hash_table_bytes / sizeof(HashJoinTuple);
	max_pointers = Min(max_pointers, MaxAllocSize / sizeof(HashJoinTuple));
	/* 若 max_pointers 非 2 的幂，须向下取整为 2 的幂   */
	max_pointers = pg_prevpower2_size_t(max_pointers);

	/* 同时确保在 nbatch 和 nbuckets 中避免整数溢出    */
	/* （鉴于 MaxAllocSize 的当前值，此步骤是多余的）   */
	max_pointers = Min(max_pointers, INT_MAX / 2 + 1);

	dbuckets = ceil(ntuples / NTUP_PER_BUCKET);
	dbuckets = Min(dbuckets, max_pointers);
	nbuckets = (int) dbuckets;
	/* 不过也别让 nbuckets 太小……          */
	nbuckets = Max(nbuckets, 1024);
	/* ……并强制它为 2 的幂。    */
	nbuckets = pg_nextpower2_32(nbuckets);

	/*
	 * 如果空间不足以存放预计的元组数以及所需的           
	 * 桶头，我们将需要多个批。                       
	 */
	bucket_bytes = sizeof(HashJoinTuple) * nbuckets;
	if (inner_rel_bytes + bucket_bytes > hash_table_bytes)
	{
		/* 我们将需要多个批    */
		size_t		sbuckets;
		double		dbatch;
		int			minbatch;
		size_t		bucket_size;

		/*
		 * 如果使用合计 hash_mem 的并行哈希仍需要多个       
		 * 批，就必须退回到常规 hash_mem 预算。           
		 */
		if (try_combined_hash_mem)
		{
			ExecChooseHashTableSize(ntuples, tupwidth, useskew,
									false, parallel_workers,
									space_allowed,
									numbuckets,
									numbatches,
									num_skew_mcvs);
			return;
		}

		/*
		 * 估计当 hash_mem 全满时我们希望拥有的桶数。        
		 * 每个桶将包含一个桶指针外加                       
		 * NTUP_PER_BUCKET 个元组，其预计大小已包含         
		 * 哈希码、指向下一元组的指针等的开销。     
		 */
		bucket_size = (tupsize * NTUP_PER_BUCKET + sizeof(HashJoinTuple));
		if (hash_table_bytes <= bucket_size)
			sbuckets = 1;		/* 避免 pg_nextpower2_size_t(0)*/
		else
			sbuckets = pg_nextpower2_size_t(hash_table_bytes / bucket_size);
		sbuckets = Min(sbuckets, max_pointers);
		nbuckets = (int) sbuckets;
		nbuckets = pg_nextpower2_32(nbuckets);
		bucket_bytes = nbuckets * sizeof(HashJoinTuple);

		/*
		 * 桶是指向哈希连接元组的简单指针，tupsize      
		 * 则包含指针、哈希码和 MinimalTupleData。因此桶       
		 * 绝不应真正超过 hash_mem 的 25%（即便     
		 * NTUP_PER_BUCKET=1 时也是如此）；除非 hash_mem 的值    
		 * 不是 2^N 字节，此时因倍增可能更多。因此        
		 * 寻找 50%。     
		 */
		Assert(bucket_bytes <= hash_table_bytes / 2);

		/* 计算所需的批数。              */
		dbatch = ceil(inner_rel_bytes / (hash_table_bytes - bucket_bytes));
		dbatch = Min(dbatch, max_pointers);
		minbatch = (int) dbatch;
		nbatch = pg_nextpower2_32(Max(2, minbatch));
	}

	Assert(nbuckets > 0);
	Assert(nbatch > 0);

	*numbuckets = nbuckets;
	*numbatches = nbatch;
}


/* ----------------------------------------------------------------
 *		ExecHashTableDestroy
 *
 *		销毁哈希表     
 * ----------------------------------------------------------------
 */
void
ExecHashTableDestroy(HashJoinTable hashtable)
{
	int			i;

	/*
	 * 确保所有临时文件都已关闭。批 0 被跳过，因为    
	 * 它不可能有任何临时文件（若 nbatch 只有 1，这    
	 * 些数组甚至可能不存在）。并行哈希不用它们  
	 */
	if (hashtable->innerBatchFile != NULL)
	{
		for (i = 1; i < hashtable->nbatch; i++)
		{
			if (hashtable->innerBatchFile[i])
				BufFileClose(hashtable->innerBatchFile[i]);
			if (hashtable->outerBatchFile[i])
				BufFileClose(hashtable->outerBatchFile[i]);
		}
	}

	/* 释放工作内存（batchCxt 是子上下文，会随之消失）*/
	MemoryContextDelete(hashtable->hashCxt);

	/* 然后丢弃控制块      */
	pfree(hashtable);
}

/*
 * ExecHashIncreaseNumBatches
 *		增加原始批数以减少                               
 *		当前的内存消耗。  
 */
static void
ExecHashIncreaseNumBatches(HashJoinTable hashtable)
{
	int			oldnbatch = hashtable->nbatch;
	int			curbatch = hashtable->curbatch;
	int			nbatch;
	long		ninmemory;
	long		nfreed;
	HashMemoryChunk oldchunks;

	/* 若已决定停止增长，则什么也不做  */
	if (!hashtable->growEnabled)
		return;

	/* 防止溢出的安全检查    */
	if (oldnbatch > Min(INT_MAX / 2, MaxAllocSize / (sizeof(void *) * 2)))
		return;

	nbatch = oldnbatch * 2;
	Assert(nbatch > 1);

#ifdef HJDEBUG
	printf("Hashjoin %p: increasing nbatch to %d because space = %zu\n",
		   hashtable, nbatch, hashtable->spaceUsed);
#endif

	if (hashtable->innerBatchFile == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(hashtable->spillCxt);

		/* 此前没有文件数组     */
		hashtable->innerBatchFile = palloc0_array(BufFile *, nbatch);
		hashtable->outerBatchFile = palloc0_array(BufFile *, nbatch);

		MemoryContextSwitchTo(oldcxt);

		/* 现在也要建立临时表空间           */
		PrepareTempTablespaces();
	}
	else
	{
		/* 扩大数组并将新增条目清零      */
		hashtable->innerBatchFile = repalloc0_array(hashtable->innerBatchFile, BufFile *, oldnbatch, nbatch);
		hashtable->outerBatchFile = repalloc0_array(hashtable->outerBatchFile, BufFile *, oldnbatch, nbatch);
	}

	hashtable->nbatch = nbatch;

	/*
	 * 扫描现有的哈希表条目，凡是不再属于当前批的       
	 * 条目都倒出去。          
	 */
	ninmemory = nfreed = 0;

	/* 若已知要调整 nbuckets，可在重新分批时做。          */
	if (hashtable->nbuckets_optimal != hashtable->nbuckets)
	{
		/* 桶的数量从不会减少             */
		Assert(hashtable->nbuckets_optimal > hashtable->nbuckets);

		hashtable->nbuckets = hashtable->nbuckets_optimal;
		hashtable->log2_nbuckets = hashtable->log2_nbuckets_optimal;

		hashtable->buckets.unshared =
			repalloc_array(hashtable->buckets.unshared,
						   HashJoinTuple, hashtable->nbuckets);
	}

	/*
	 * 我们将直接扫描各内存块，这样就能现在重置      
	 * 各个桶，而无需跟踪桶里哪些元组已被处理过。        
	 * 旧内存块将在遍历过程中逐一释放。             
	 */
	memset(hashtable->buckets.unshared, 0,
		   sizeof(HashJoinTuple) * hashtable->nbuckets);
	oldchunks = hashtable->chunks;
	hashtable->chunks = NULL;

	/* 下面扫描旧内存块以及各块中的全部元组              */
	while (oldchunks != NULL)
	{
		HashMemoryChunk nextchunk = oldchunks->next.unshared;

		/* 缓冲区内位置（至多 oldchunks->used）     */
		size_t		idx = 0;

		/* 处理此内存块中的全部元组（然后释放）     */
		while (idx < oldchunks->used)
		{
			HashJoinTuple hashTuple = (HashJoinTuple) (HASH_CHUNK_DATA(oldchunks) + idx);
			MinimalTuple tuple = HJTUPLE_MINTUPLE(hashTuple);
			int			hashTupleSize = (HJTUPLE_OVERHEAD + tuple->t_len);
			int			bucketno;
			int			batchno;

			ninmemory++;
			ExecHashGetBucketAndBatch(hashtable, hashTuple->hashvalue,
									  &bucketno, &batchno);

			if (batchno == curbatch)
			{
				/* 元组留在内存，复制进新内存块        */
				HashJoinTuple copyTuple;

				copyTuple = (HashJoinTuple) dense_alloc(hashtable, hashTupleSize);
				memcpy(copyTuple, hashTuple, hashTupleSize);

				/* 并将它放回相应的桶               */
				copyTuple->next.unshared = hashtable->buckets.unshared[bucketno];
				hashtable->buckets.unshared[bucketno] = copyTuple;
			}
			else
			{
				/* 倒出去   */
				Assert(batchno > curbatch);
				ExecHashJoinSaveTuple(HJTUPLE_MINTUPLE(hashTuple),
									  hashTuple->hashvalue,
									  &hashtable->innerBatchFile[batchno],
									  hashtable);

				hashtable->spaceUsed -= hashTupleSize;
				nfreed++;
			}

			/* 块内下一个元组    */
			idx += MAXALIGN(hashTupleSize);

			/* 允许此循环可被取消       */
			CHECK_FOR_INTERRUPTS();
		}

		/* 此内存块已处理完毕，释放它并继续处理下一个  */
		pfree(oldchunks);
		oldchunks = nextchunk;
	}

#ifdef HJDEBUG
	printf("Hashjoin %p: freed %ld of %ld tuples, space now %zu\n",
		   hashtable, nfreed, ninmemory, hashtable->spaceUsed);
#endif

	/*
	 * 若表中元组已被全部倒出，或一个都没倒出，则禁止  
	 * nbatch 进一步扩大。这种情况表明，哈希值相同的 
	 * 元组足够多，以致溢出 spaceAllowed。                 
	 * 增加 nbatch 并不能解决问题，因为已无法把这组元组   
	 * 再细分了。我们只能硬扛，指望服务器                  
	 * 内存够用。
	 */
	if (nfreed == 0 || nfreed == ninmemory)
	{
		hashtable->growEnabled = false;
#ifdef HJDEBUG
		printf("Hashjoin %p: disabling further increase of nbatch\n",
			   hashtable);
#endif
	}
}

/*
 * ExecParallelHashIncreaseNumBatches
 *		每个附加到 grow_batches_barrier 的参与者观察到      
 *		growth == PHJ_GROWTH_NEED_MORE_BATCHES 时必须运行。        
 */
static void
ExecParallelHashIncreaseNumBatches(HashJoinTable hashtable)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;

	Assert(BarrierPhase(&pstate->build_barrier) == PHJ_BUILD_HASH_INNER);

	/*
	 * 可能性虽小，但我们仍需做好准备：操作期间可能有 
	 * 新参与者加入，因此此处需要按屏障                      
	 * 阶段来分支。 
	 */
	switch (PHJ_GROW_BATCHES_PHASE(BarrierPhase(&pstate->grow_batches_barrier)))
	{
		case PHJ_GROW_BATCHES_ELECT:

			/*
			 * 选出一名参与者，为增加批数做准备。            
			 * 这包括重新分配或重置 0 号批的桶数组，        
			 * 以便所有参与者随后开始重新分区                  
			 * 元组 
			 */
			if (BarrierArriveAndWait(&pstate->grow_batches_barrier,
									 WAIT_EVENT_HASH_GROW_BATCHES_ELECT))
			{
				dsa_pointer_atomic *buckets;
				ParallelHashJoinBatch *old_batch0;
				int			new_nbatch;
				int			i;

				/* 把旧批挪开。                 */
				old_batch0 = hashtable->batches[0].shared;
				pstate->old_batches = pstate->batches;
				pstate->old_nbatch = hashtable->nbatch;
				pstate->batches = InvalidDsaPointer;

				/* 释放本后端旧的访问器。  */
				ExecParallelHashCloseBatchAccessors(hashtable);

				/* 算出要使用多少个批。      */
				if (hashtable->nbatch == 1)
				{
					/*
					 * 我们将从单批转为多批，因此需要          
					 * 从一份大的合并内存预算切换到常规的   
					 * hash_mem 预算。      
					 */
					pstate->space_allowed = get_hash_memory_limit();

					/*
					 * 所有参与者的合并 hash_mem 不足以     
					 * 支撑。每个参与者一个批大致相当，    
					 * 且很可能同样不够。因此让每个参与者
					 * 试两个批，                                   
					 * 向上取整为 2 的幂。  
					 */
					new_nbatch = pg_nextpower2_32(pstate->nparticipants * 2);
				}
				else
				{
					/*
					 * 我们本来就已经是多批了，试着将批数    
					 * 翻倍。  
					 */
					new_nbatch = hashtable->nbatch * 2;
				}

				/* 分配一个更大的批世代。          */
				Assert(hashtable->nbatch == pstate->nbatch);
				ExecParallelHashJoinSetUpBatches(hashtable, new_nbatch);
				Assert(hashtable->nbatch == pstate->nbatch);

				/* 替换或回收 0 号批的桶数组。    */
				if (pstate->old_nbatch == 1)
				{
					double		dtuples;
					double		dbuckets;
					int			new_nbuckets;
					uint32		max_buckets;

					/*
					 * 我们可能还需要更小的桶数组。预计每    
					 * 个批会有多少元组？假设目前只得到    
					 * 一半。通常无需改变桶数组的大小，因为
					 * 批数增多时每个批的大小保持不变，但在 
					 * 这种特殊场景下，要从单个大批变为  
					 * 许多更小的批。                                  
					 * 此时保留大数组就太                       
					 * 浪费
					 */
					dtuples = (old_batch0->ntuples * 2.0) / new_nbatch;

					/*
					 * 我们需要算出桶数的最大值，确保不会  
					 * 超出 MaxAllocSize 边界。鉴于稍后还要  
					 * 向上取整到下一个 2 的幂，先把最大值 
					 * 向下取整到前一个 2 的幂。             
					 */
					max_buckets = pg_prevpower2_32((uint32)
												   (MaxAllocSize / sizeof(dsa_pointer_atomic)));
					dbuckets = ceil(dtuples / NTUP_PER_BUCKET);
					dbuckets = Min(dbuckets, max_buckets);
					new_nbuckets = (int) dbuckets;
					new_nbuckets = Max(new_nbuckets, 1024);
					new_nbuckets = pg_nextpower2_32(new_nbuckets);
					dsa_free(hashtable->area, old_batch0->buckets);
					hashtable->batches[0].shared->buckets =
						dsa_allocate(hashtable->area,
									 sizeof(dsa_pointer_atomic) * new_nbuckets);
					buckets = (dsa_pointer_atomic *)
						dsa_get_address(hashtable->area,
										hashtable->batches[0].shared->buckets);
					for (i = 0; i < new_nbuckets; ++i)
						dsa_pointer_atomic_init(&buckets[i], InvalidDsaPointer);
					pstate->nbuckets = new_nbuckets;
				}
				else
				{
					/* 回收现有的桶数组。        */
					hashtable->batches[0].shared->buckets = old_batch0->buckets;
					buckets = (dsa_pointer_atomic *)
						dsa_get_address(hashtable->area, old_batch0->buckets);
					for (i = 0; i < hashtable->nbuckets; ++i)
						dsa_pointer_atomic_write(&buckets[i], InvalidDsaPointer);
				}

				/* 把所有内存块移入工作队列以并行处理。     */
				pstate->chunk_work_queue = old_batch0->chunks;

				/* 扩展进行期间，暂时禁止继续增长。        */
				pstate->growth = PHJ_GROWTH_DISABLED;
			}
			else
			{
				/* 其余所有参与者只需把元组刷入磁盘。     */
				ExecParallelHashCloseBatchAccessors(hashtable);
			}
			/* Fall through. */

		case PHJ_GROW_BATCHES_REALLOCATE:
			/* 等待上述操作完成。        */
			BarrierArriveAndWait(&pstate->grow_batches_barrier,
								 WAIT_EVENT_HASH_GROW_BATCHES_REALLOCATE);
			/* Fall through. */

		case PHJ_GROW_BATCHES_REPARTITION:
			/* 确保我们拿到当前的规模参数与桶。           */
			ExecParallelHashEnsureBatchAccessors(hashtable);
			ExecParallelHashTableSetCurrentBatch(hashtable, 0);
			/* 随后分区、刷入计数器  */
			ExecParallelHashRepartitionFirst(hashtable);
			ExecParallelHashRepartitionRest(hashtable);
			ExecParallelHashMergeCounters(hashtable);
			/* 等待上述操作完成。        */
			BarrierArriveAndWait(&pstate->grow_batches_barrier,
								 WAIT_EVENT_HASH_GROW_BATCHES_REPARTITION);
			/* Fall through. */

		case PHJ_GROW_BATCHES_DECIDE:

			/*
			 * 选出一名参与者负责清理，并判断是否         
			 * 还需要继续重新分区，抑或因它无济于事      
			 * 而禁用   
			 */
			if (BarrierArriveAndWait(&pstate->grow_batches_barrier,
									 WAIT_EVENT_HASH_GROW_BATCHES_DECIDE))
			{
				ParallelHashJoinBatch *old_batches;
				bool		space_exhausted = false;
				bool		extreme_skew_detected = false;

				/* 确保我们拿到当前的规模参数与桶。           */
				ExecParallelHashEnsureBatchAccessors(hashtable);
				ExecParallelHashTableSetCurrentBatch(hashtable, 0);

				old_batches = dsa_get_address(hashtable->area, pstate->old_batches);

				/* 新一代的批中是否已有用尽的？          */
				for (int i = 0; i < hashtable->nbatch; ++i)
				{
					ParallelHashJoinBatch *batch;
					ParallelHashJoinBatch *old_batch;
					int			parent;

					batch = hashtable->batches[i].shared;
					if (batch->space_exhausted ||
						batch->estimated_size > pstate->space_allowed)
						space_exhausted = true;

					parent = i % pstate->old_nbatch;
					old_batch = NthParallelHashJoinBatch(old_batches, parent);
					if (old_batch->space_exhausted ||
						batch->estimated_size > pstate->space_allowed)
					{
						/*
						 * 本批是否接收了来自其父批的全部    
						 * 元组？若真如此，表明进一步的     
						 * 重新分区已无济于事（哈希值很可能   
						 * 全都相同。）         
						 */
						if (batch->ntuples == hashtable->batches[parent].shared->old_ntuples)
							extreme_skew_detected = true;
					}
				}

				/* 若无济于事或将要溢出，就不要再扩大。   */
				if (extreme_skew_detected || hashtable->nbatch >= INT_MAX / 2)
					pstate->growth = PHJ_GROWTH_DISABLED;
				else if (space_exhausted)
					pstate->growth = PHJ_GROWTH_NEED_MORE_BATCHES;
				else
					pstate->growth = PHJ_GROWTH_OK;

				/* 释放共享内存中的旧批。      */
				dsa_free(hashtable->area, pstate->old_batches);
				pstate->old_batches = InvalidDsaPointer;
			}
			/* Fall through. */

		case PHJ_GROW_BATCHES_FINISH:
			/* 等待上述操作完成。     */
			BarrierArriveAndWait(&pstate->grow_batches_barrier,
								 WAIT_EVENT_HASH_GROW_BATCHES_FINISH);
	}
}

/*
 * 由于批数已经增加，需要对内批 0 当前载入            
 * 内存中的元组重新分区。一部分元组留在内存，            
 * 另一部分则写出到更靠后的批。          
 */
static void
ExecParallelHashRepartitionFirst(HashJoinTable hashtable)
{
	dsa_pointer chunk_shared;
	HashMemoryChunk chunk;

	Assert(hashtable->nbatch == hashtable->parallel_state->nbatch);

	while ((chunk = ExecParallelHashPopChunkQueue(hashtable, &chunk_shared)))
	{
		size_t		idx = 0;

		/* 重新分区本块内全部元组。  */
		while (idx < chunk->used)
		{
			HashJoinTuple hashTuple = (HashJoinTuple) (HASH_CHUNK_DATA(chunk) + idx);
			MinimalTuple tuple = HJTUPLE_MINTUPLE(hashTuple);
			HashJoinTuple copyTuple;
			dsa_pointer shared;
			int			bucketno;
			int			batchno;

			ExecHashGetBucketAndBatch(hashtable, hashTuple->hashvalue,
									  &bucketno, &batchno);

			Assert(batchno < hashtable->nbatch);
			if (batchno == 0)
			{
				/* 它仍属于 0 号批，复制到新内存块。   */
				copyTuple =
					ExecParallelHashTupleAlloc(hashtable,
											   HJTUPLE_OVERHEAD + tuple->t_len,
											   &shared);
				copyTuple->hashvalue = hashTuple->hashvalue;
				memcpy(HJTUPLE_MINTUPLE(copyTuple), tuple, tuple->t_len);
				ExecParallelHashPushTuple(&hashtable->buckets.shared[bucketno],
										  copyTuple, shared);
			}
			else
			{
				size_t		tuple_size =
					MAXALIGN(HJTUPLE_OVERHEAD + tuple->t_len);

				/* 它属于更靠后的批。  */
				hashtable->batches[batchno].estimated_size += tuple_size;
				sts_puttuple(hashtable->batches[batchno].inner_tuples,
							 &hashTuple->hashvalue, tuple);
			}

			/* 统计该元组。*/
			++hashtable->batches[0].old_ntuples;
			++hashtable->batches[batchno].ntuples;

			idx += MAXALIGN(HJTUPLE_OVERHEAD +
							HJTUPLE_MINTUPLE(hashTuple)->t_len);
		}

		/* 释放该块。  */
		dsa_free(hashtable->area, chunk_shared);

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * 协助重新分区内批 1..n。    
 */
static void
ExecParallelHashRepartitionRest(HashJoinTable hashtable)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	int			old_nbatch = pstate->old_nbatch;
	SharedTuplestoreAccessor **old_inner_tuples;
	ParallelHashJoinBatch *old_batches;
	int			i;

	/* 取得前一代的批。                             */
	old_batches = (ParallelHashJoinBatch *)
		dsa_get_address(hashtable->area, pstate->old_batches);
	old_inner_tuples = palloc0_array(SharedTuplestoreAccessor *, old_nbatch);
	for (i = 1; i < old_nbatch; ++i)
	{
		ParallelHashJoinBatch *shared =
			NthParallelHashJoinBatch(old_batches, i);

		old_inner_tuples[i] = sts_attach(ParallelHashJoinBatchInner(shared),
										 ParallelWorkerNumber + 1,
										 &pstate->fileset);
	}

	/* 参与重新分区工作。             */
	for (i = 1; i < old_nbatch; ++i)
	{
		MinimalTuple tuple;
		uint32		hashvalue;

		/* 扫描前一代中的一个分区。             */
		sts_begin_parallel_scan(old_inner_tuples[i]);
		while ((tuple = sts_parallel_scan_next(old_inner_tuples[i], &hashvalue)))
		{
			size_t		tuple_size = MAXALIGN(HJTUPLE_OVERHEAD + tuple->t_len);
			int			bucketno;
			int			batchno;

			/* 决定它在新生代中归属哪个分区。            */
			ExecHashGetBucketAndBatch(hashtable, hashvalue, &bucketno,
									  &batchno);

			hashtable->batches[batchno].estimated_size += tuple_size;
			++hashtable->batches[batchno].ntuples;
			++hashtable->batches[i].old_ntuples;

			/* 把元组存入其新批。    */
			sts_puttuple(hashtable->batches[batchno].inner_tuples,
						 &hashvalue, tuple);

			CHECK_FOR_INTERRUPTS();
		}
		sts_end_parallel_scan(old_inner_tuples[i]);
	}

	pfree(old_inner_tuples);
}

/*
 * 把后端本地的各批计数器转移到共享总计中。       
 */
static void
ExecParallelHashMergeCounters(HashJoinTable hashtable)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	int			i;

	LWLockAcquire(&pstate->lock, LW_EXCLUSIVE);
	pstate->total_tuples = 0;
	for (i = 0; i < hashtable->nbatch; ++i)
	{
		ParallelHashJoinBatchAccessor *batch = &hashtable->batches[i];

		batch->shared->size += batch->size;
		batch->shared->estimated_size += batch->estimated_size;
		batch->shared->ntuples += batch->ntuples;
		batch->shared->old_ntuples += batch->old_ntuples;
		batch->size = 0;
		batch->estimated_size = 0;
		batch->ntuples = 0;
		batch->old_ntuples = 0;
		pstate->total_tuples += batch->shared->ntuples;
	}
	LWLockRelease(&pstate->lock);
}

/*
 * ExecHashIncreaseNumBuckets
 *		增大最初的桶数，以便减少每个桶内          
 *		的元组数量            
 */
static void
ExecHashIncreaseNumBuckets(HashJoinTable hashtable)
{
	HashMemoryChunk chunk;

	/* 非增大则不做任何事（既然叫 increase 就有道理）  */
	if (hashtable->nbuckets >= hashtable->nbuckets_optimal)
		return;

#ifdef HJDEBUG
	printf("Hashjoin %p: increasing nbuckets %d => %d\n",
		   hashtable, hashtable->nbuckets, hashtable->nbuckets_optimal);
#endif

	hashtable->nbuckets = hashtable->nbuckets_optimal;
	hashtable->log2_nbuckets = hashtable->log2_nbuckets_optimal;

	Assert(hashtable->nbuckets > 1);
	Assert(hashtable->nbuckets <= (INT_MAX / 2));
	Assert(hashtable->nbuckets == (1 << hashtable->log2_nbuckets));

	/*
	 * 只需重新分配合适数量的桶，无需遍历                 
	 * 它们本身；可以直接遍历密集分配的内存块（正如 
	 * ExecHashIncreaseNumBatches 中的做法，但不必复制到新 
	 * 块） 
	 */
	hashtable->buckets.unshared =
		repalloc_array(hashtable->buckets.unshared,
					   HashJoinTuple, hashtable->nbuckets);

	memset(hashtable->buckets.unshared, 0,
		   hashtable->nbuckets * sizeof(HashJoinTuple));

	/* 遍历所有内存块中的全部元组，重建哈希表       */
	for (chunk = hashtable->chunks; chunk != NULL; chunk = chunk->next.unshared)
	{
		/* 处理此内存块内的全部元组    */
		size_t		idx = 0;

		while (idx < chunk->used)
		{
			HashJoinTuple hashTuple = (HashJoinTuple) (HASH_CHUNK_DATA(chunk) + idx);
			int			bucketno;
			int			batchno;

			ExecHashGetBucketAndBatch(hashtable, hashTuple->hashvalue,
									  &bucketno, &batchno);

			/* 把元组加入相应的桶        */
			hashTuple->next.unshared = hashtable->buckets.unshared[bucketno];
			hashtable->buckets.unshared[bucketno] = hashTuple;

			/* 索引前移越过该元组  */
			idx += MAXALIGN(HJTUPLE_OVERHEAD +
							HJTUPLE_MINTUPLE(hashTuple)->t_len);
		}

		/* 允许此循环可被取消       */
		CHECK_FOR_INTERRUPTS();
	}
}

static void
ExecParallelHashIncreaseNumBuckets(HashJoinTable hashtable)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	int			i;
	HashMemoryChunk chunk;
	dsa_pointer chunk_s;

	Assert(BarrierPhase(&pstate->build_barrier) == PHJ_BUILD_HASH_INNER);

	/*
	 * 可能性虽小，但我们仍需做好准备：操作期间可能有 
	 * 新参与者加入，因此此处需要按屏障阶段                
	 * 来分支。       
	 */
	switch (PHJ_GROW_BUCKETS_PHASE(BarrierPhase(&pstate->grow_buckets_barrier)))
	{
		case PHJ_GROW_BUCKETS_ELECT:
			/* 选出一名参与者准备增大 nbuckets。          */
			if (BarrierArriveAndWait(&pstate->grow_buckets_barrier,
									 WAIT_EVENT_HASH_GROW_BUCKETS_ELECT))
			{
				size_t		size;
				dsa_pointer_atomic *buckets;

				/* 把桶数组的大小翻倍。       */
				pstate->nbuckets *= 2;
				size = pstate->nbuckets * sizeof(dsa_pointer_atomic);
				hashtable->batches[0].shared->size += size / 2;
				dsa_free(hashtable->area, hashtable->batches[0].shared->buckets);
				hashtable->batches[0].shared->buckets =
					dsa_allocate(hashtable->area, size);
				buckets = (dsa_pointer_atomic *)
					dsa_get_address(hashtable->area,
									hashtable->batches[0].shared->buckets);
				for (i = 0; i < pstate->nbuckets; ++i)
					dsa_pointer_atomic_init(&buckets[i], InvalidDsaPointer);

				/* 把内存块链表放入工作队列。 */
				pstate->chunk_work_queue = hashtable->batches[0].shared->chunks;

				/* 清除标志。 */
				pstate->growth = PHJ_GROWTH_OK;
			}
			/* Fall through. */

		case PHJ_GROW_BUCKETS_REALLOCATE:
			/* 等待上述操作完成。     */
			BarrierArriveAndWait(&pstate->grow_buckets_barrier,
								 WAIT_EVENT_HASH_GROW_BUCKETS_REALLOCATE);
			/* Fall through. */

		case PHJ_GROW_BUCKETS_REINSERT:
			/* 把全部元组重新插入哈希表。  */
			ExecParallelHashEnsureBatchAccessors(hashtable);
			ExecParallelHashTableSetCurrentBatch(hashtable, 0);
			while ((chunk = ExecParallelHashPopChunkQueue(hashtable, &chunk_s)))
			{
				size_t		idx = 0;

				while (idx < chunk->used)
				{
					HashJoinTuple hashTuple = (HashJoinTuple) (HASH_CHUNK_DATA(chunk) + idx);
					dsa_pointer shared = chunk_s + HASH_CHUNK_HEADER_SIZE + idx;
					int			bucketno;
					int			batchno;

					ExecHashGetBucketAndBatch(hashtable, hashTuple->hashvalue,
											  &bucketno, &batchno);
					Assert(batchno == 0);

					/* 把元组加入相应的桶        */
					ExecParallelHashPushTuple(&hashtable->buckets.shared[bucketno],
											  hashTuple, shared);

					/* 索引前移越过该元组  */
					idx += MAXALIGN(HJTUPLE_OVERHEAD +
									HJTUPLE_MINTUPLE(hashTuple)->t_len);
				}

				/* 允许此循环可被取消       */
				CHECK_FOR_INTERRUPTS();
			}
			BarrierArriveAndWait(&pstate->grow_buckets_barrier,
								 WAIT_EVENT_HASH_GROW_BUCKETS_REINSERT);
	}
}

/*
 * ExecHashTableInsert
 *		根据哈希值把元组插入哈希表，也可能直接     
 *		进入临时文件，留待后续的批使用  
 *
 * 注意：传入的 TupleTableSlot 可能包含常规、最小或虚拟    
 * 元组；最小元组的情形尤其必然出现于从批文件重新     
 * 载入元组之时。对常规元组情形，若不强制将槽的      
 * 内容转为最小形式本可省下一些开销；但是否值得为     
 * 此引入混乱并不明确  
 */
void
ExecHashTableInsert(HashJoinTable hashtable,
					TupleTableSlot *slot,
					uint32 hashvalue)
{
	bool		shouldFree;
	MinimalTuple tuple = ExecFetchSlotMinimalTuple(slot, &shouldFree);
	int			bucketno;
	int			batchno;

	ExecHashGetBucketAndBatch(hashtable, hashvalue,
							  &bucketno, &batchno);

	/*
	 * 决定把元组放入哈希表还是临时文件                
	 */
	if (batchno == hashtable->curbatch)
	{
		/*
		 * 把元组放入哈希表   
		 */
		HashJoinTuple hashTuple;
		int			hashTupleSize;
		double		ntuples = (hashtable->totalTuples - hashtable->skewTuples);

		/* 创建 HashJoinTuple     */
		hashTupleSize = HJTUPLE_OVERHEAD + tuple->t_len;
		hashTuple = (HashJoinTuple) dense_alloc(hashtable, hashTupleSize);

		hashTuple->hashvalue = hashvalue;
		memcpy(HJTUPLE_MINTUPLE(hashTuple), tuple, tuple->t_len);

		/*
		 * 插入时总会重置元组已匹配标志。即便从批文件   
		 * 重新载入元组时也无妨，因为该元组在           
		 * 进入批文件之前，不可能已经与任何外层          
		 * 元组匹配过。       
		 */
		HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(hashTuple));

		/* 把它推到该桶链表的头部           */
		hashTuple->next.unshared = hashtable->buckets.unshared[bucketno];
		hashtable->buckets.unshared[bucketno] = hashTuple;

		/*
		 * 若刚超过 NTUP_PER_BUCKET 阈值，则增大桶数           
		 * （最优值），但仅当仍然只有一个                  
		 * 批。
		 */
		if (hashtable->nbatch == 1 &&
			ntuples > (hashtable->nbuckets_optimal * NTUP_PER_BUCKET))
		{
			/* 防止整数溢出以及分配大小溢出             */
			if (hashtable->nbuckets_optimal <= INT_MAX / 2 &&
				hashtable->nbuckets_optimal * 2 <= MaxAllocSize / sizeof(HashJoinTuple))
			{
				hashtable->nbuckets_optimal *= 2;
				hashtable->log2_nbuckets_optimal += 1;
			}
		}

		/* 统计已用空间，若用量过大则回退               */
		hashtable->spaceUsed += hashTupleSize;
		if (hashtable->spaceUsed > hashtable->spacePeak)
			hashtable->spacePeak = hashtable->spaceUsed;
		if (hashtable->spaceUsed +
			hashtable->nbuckets_optimal * sizeof(HashJoinTuple)
			> hashtable->spaceAllowed)
			ExecHashIncreaseNumBatches(hashtable);
	}
	else
	{
		/*
		 * 将元组放入临时文件供后续批次使用
		 */
		Assert(batchno > hashtable->curbatch);
		ExecHashJoinSaveTuple(tuple,
							  hashvalue,
							  &hashtable->innerBatchFile[batchno],
							  hashtable);
	}

	if (shouldFree)
		heap_free_minimal_tuple(tuple);
}

/*
 * ExecParallelHashTableInsert
 *		将元组插入共享哈希表或共享批元组存储中         
 */
void
ExecParallelHashTableInsert(HashJoinTable hashtable,
							TupleTableSlot *slot,
							uint32 hashvalue)
{
	bool		shouldFree;
	MinimalTuple tuple = ExecFetchSlotMinimalTuple(slot, &shouldFree);
	dsa_pointer shared;
	int			bucketno;
	int			batchno;

retry:
	ExecHashGetBucketAndBatch(hashtable, hashvalue, &bucketno, &batchno);

	if (batchno == 0)
	{
		HashJoinTuple hashTuple;

		/* 尝试将其载入内存    */
		Assert(BarrierPhase(&hashtable->parallel_state->build_barrier) ==
			   PHJ_BUILD_HASH_INNER);
		hashTuple = ExecParallelHashTupleAlloc(hashtable,
											   HJTUPLE_OVERHEAD + tuple->t_len,
											   &shared);
		if (hashTuple == NULL)
			goto retry;

		/* 把哈希值存入 HashJoinTuple 头部中        */
		hashTuple->hashvalue = hashvalue;
		memcpy(HJTUPLE_MINTUPLE(hashTuple), tuple, tuple->t_len);
		HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(hashTuple));

		/* 将它推到桶链表的前端              */
		ExecParallelHashPushTuple(&hashtable->buckets.shared[bucketno],
								  hashTuple, shared);
	}
	else
	{
		size_t		tuple_size = MAXALIGN(HJTUPLE_OVERHEAD + tuple->t_len);

		Assert(batchno > 0);

		/* 如有必要，尝试在批中预分配空间       */
		if (hashtable->batches[batchno].preallocated < tuple_size)
		{
			if (!ExecParallelHashTuplePrealloc(hashtable, batchno, tuple_size))
				goto retry;
		}

		Assert(hashtable->batches[batchno].preallocated >= tuple_size);
		hashtable->batches[batchno].preallocated -= tuple_size;
		sts_puttuple(hashtable->batches[batchno].inner_tuples, &hashvalue,
					 tuple);
	}
	++hashtable->batches[batchno].ntuples;

	if (shouldFree)
		heap_free_minimal_tuple(tuple);
}

/*
 * 向当前哈希表插入一个元组。不同于   
 * ExecParallelHashTableInsert，此版本不准备将元组发送到其他   
 * 批次或应对内存耗尽，且只应在禁用增长之后，对      
 * 属于当前批次的元组调用它。                               
 */
void
ExecParallelHashTableInsertCurrentBatch(HashJoinTable hashtable,
										TupleTableSlot *slot,
										uint32 hashvalue)
{
	bool		shouldFree;
	MinimalTuple tuple = ExecFetchSlotMinimalTuple(slot, &shouldFree);
	HashJoinTuple hashTuple;
	dsa_pointer shared;
	int			batchno;
	int			bucketno;

	ExecHashGetBucketAndBatch(hashtable, hashvalue, &bucketno, &batchno);
	Assert(batchno == hashtable->curbatch);
	hashTuple = ExecParallelHashTupleAlloc(hashtable,
										   HJTUPLE_OVERHEAD + tuple->t_len,
										   &shared);
	hashTuple->hashvalue = hashvalue;
	memcpy(HJTUPLE_MINTUPLE(hashTuple), tuple, tuple->t_len);
	HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(hashTuple));
	ExecParallelHashPushTuple(&hashtable->buckets.shared[bucketno],
							  hashTuple, shared);

	if (shouldFree)
		heap_free_minimal_tuple(tuple);
}

/*
 * ExecHashGetHashValue
 *		计算一个元组的哈希值    
 *
 * 待检测的元组必须位于 econtext->ecxt_outertuple 中（因此    
 * hashkeys 表达式里的变量需以 OUTER_VAR 作为 varno）。若     
 * outer_tuple 为假（即它是 HashJoin 的内节点 Hash），则
 * econtext、hashkeys 与 slot 必须来自 Hash,hashkeys/slot 引用且适  
 * 配来自 Hash 下方节点的元组。反之，若                   
 * outer_tuple 为真，则 econtext 来自 HashJoin，且 hashkeys/slot    
 * 需适用于 HashJoin 外节点传来的元组。    
 *
 * 返回真表示该元组的哈希值已成功计算出来并             
 * 存放在 *hashvalue 处。返回假表示该元组无法匹配，    
 * 因为它包含空属性，因此应立即丢弃。                   
 * （若 keep_nulls 为真则永不返回假。）                   
 */
bool
ExecHashGetHashValue(HashJoinTable hashtable,
					 ExprContext *econtext,
					 List *hashkeys,
					 bool outer_tuple,
					 bool keep_nulls,
					 uint32 *hashvalue)
{
	uint32		hashkey = 0;
	FmgrInfo   *hashfunctions;
	ListCell   *hk;
	int			i = 0;
	MemoryContext oldContext;

	/*
	 * 我们每次都重置求值上下文，以回收散列键表达式中  
	 * 泄漏的内存。  
	 */
	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	if (outer_tuple)
		hashfunctions = hashtable->outer_hashfunctions;
	else
		hashfunctions = hashtable->inner_hashfunctions;

	foreach(hk, hashkeys)
	{
		ExprState  *keyexpr = (ExprState *) lfirst(hk);
		Datum		keyval;
		bool		isNull;

		/* 通过旋转合并相继的散列键    */
		hashkey = pg_rotate_left32(hashkey, 1);

		/*
		 * 取得该元组的连接属性值        
		 */
		keyval = ExecEvalExpr(keyexpr, econtext, &isNull);

		/*
		 * 如果属性为 NULL，且连接操作符是严格的，那么 
		 * 该元组无法通过连接条件，我们可以立即  
		 * 拒绝它（除非我们正在扫描外连接的外侧，此时    
		 * 我们绝不能拒绝它）。否则，我们把 NULL        
		 * 的哈希码当作零来处理（这将支持行为类似          
		 * IS NOT DISTINCT 的操作符，但不支持更随机行为）。
		 * 即使操作符非严格，仍视哈希支持函数为严格。 
		 *
		 * 注意：当前可哈希连接的操作符都必须严格，    
		 * 哈希索引 AM 便做了这种假定。不过，在这里支持  
		 * 非严格只需极少额外代码，不妨这么做。  
		 */
		if (isNull)
		{
			if (hashtable->hashStrict[i] && !keep_nulls)
			{
				MemoryContextSwitchTo(oldContext);
				return false;	/* 无法匹配 */
			}
			/* 否则保持 hashkey 不变，相当于哈希码为 0    */
		}
		else
		{
			/* 计算哈希函数        */
			uint32		hkey;

			hkey = DatumGetUInt32(FunctionCall1Coll(&hashfunctions[i], hashtable->collations[i], keyval));
			hashkey ^= hkey;
		}

		i++;
	}

	MemoryContextSwitchTo(oldContext);

	*hashvalue = hashkey;
	return true;
}

/*
 * ExecHashGetBucketAndBatch
 *		确定一个哈希值对应的桶号与批号                
 *
 * 注意：动态增大 nbatch 绝不能改变桶号                    
 * 对于给定的哈希码而言（因为我们不会把元组       
 * 移到别的哈希链上），且必须只会使批号不变或增 
 * 大。我们的算法是   
 *		bucketno = hashvalue MOD nbuckets
 *		batchno = ROR(hashvalue, log2_nbuckets) MOD nbatch
 * 其中 nbuckets 和 nbatch 都应当是 2 的幂，这样我们         
 * 就能用移位和掩码来完成计算。（这里假定所有哈希    
 * 函数都能很好地随机化所有输出位，否则我们很        
 * 可能会得到非常倾斜的桶或批。）         
 *
 * 在 nbatch == 1 期间，由于动态桶数增长，nbuckets 和          
 * log2_nbuckets 可能变化。一旦开始分批，该值就固定了，  
 * 在整个连接期间保持不变（这让我们能以这里的            
 * 方式计算批号）。   
 *
 * nbatch 总是 2 的幂；我们只会通过翻倍来增大它。这     
 * 实际上相当于给批号顶端再增加一位。在非常大的     
 * 连接中，位可能会用完，所以我们通过旋转哈希值来     
 * 做到这一点。当虚拟桶数超过 2^32 时，batchno 会从         
 * bucketno 处窃取一些位。宁可让桶链变得更长，           
 * 也不要失去划分批次的能力。    
 */
void
ExecHashGetBucketAndBatch(HashJoinTable hashtable,
						  uint32 hashvalue,
						  int *bucketno,
						  int *batchno)
{
	uint32		nbuckets = (uint32) hashtable->nbuckets;
	uint32		nbatch = (uint32) hashtable->nbatch;

	if (nbatch > 1)
	{
		*bucketno = hashvalue & (nbuckets - 1);
		*batchno = pg_rotate_right32(hashvalue,
									 hashtable->log2_nbuckets) & (nbatch - 1);
	}
	else
	{
		*bucketno = hashvalue & (nbuckets - 1);
		*batchno = 0;
	}
}

/*
 * ExecScanHashBucket
 *		扫描一个哈希桶，寻找与当前外元组匹配者
 *
 * 当前的外元组必须存放在 econtext->ecxt_outertuple 中。  
 *
 * 成功时，内元组会存入 hjstate->hj_CurTuple 以及         
 * econtext->ecxt_innertuple，使用 hjstate->hj_HashTupleSlot 作为   
 * 其槽位。   
 */
bool
ExecScanHashBucket(HashJoinState *hjstate,
				   ExprContext *econtext)
{
	ExprState  *hjclauses = hjstate->hashclauses;
	HashJoinTable hashtable = hjstate->hj_HashTable;
	HashJoinTuple hashTuple = hjstate->hj_CurTuple;
	uint32		hashvalue = hjstate->hj_CurHashValue;

	/*
	 * hj_CurTuple 是上次从当前桶返回的元组的地址，          
	 * 如果该开始扫描新桶了则为 NULL。                
	 *
	 * 如果元组散列到了倾斜桶，就扫描该倾斜桶，  
	 * 否则扫描标准哈希表的桶。         
	 */
	if (hashTuple != NULL)
		hashTuple = hashTuple->next.unshared;
	else if (hjstate->hj_CurSkewBucketNo != INVALID_SKEW_BUCKET_NO)
		hashTuple = hashtable->skewBucket[hjstate->hj_CurSkewBucketNo]->tuples;
	else
		hashTuple = hashtable->buckets.unshared[hjstate->hj_CurBucketNo];

	while (hashTuple != NULL)
	{
		if (hashTuple->hashvalue == hashvalue)
		{
			TupleTableSlot *inntuple;

			/* 把哈希表的元组放入执行槽，让 ExecQual 看到  */
			inntuple = ExecStoreMinimalTuple(HJTUPLE_MINTUPLE(hashTuple),
											 hjstate->hj_HashTupleSlot,
											 false);	/* 不要 pfree */
			econtext->ecxt_innertuple = inntuple;

			if (ExecQualAndReset(hjclauses, econtext))
			{
				hjstate->hj_CurTuple = hashTuple;
				return true;
			}
		}

		hashTuple = hashTuple->next.unshared;
	}

	/*
	 * no match
	 */
	return false;
}

/*
 * ExecParallelScanHashBucket
 *		扫描一个哈希桶，寻找与当前外元组匹配者
 *
 * 当前的外元组必须存放在 econtext->ecxt_outertuple 中。  
 *
 * 成功时，内元组会存入 hjstate->hj_CurTuple 以及         
 * econtext->ecxt_innertuple，使用 hjstate->hj_HashTupleSlot 作为   
 * 其槽位。   
 */
bool
ExecParallelScanHashBucket(HashJoinState *hjstate,
						   ExprContext *econtext)
{
	ExprState  *hjclauses = hjstate->hashclauses;
	HashJoinTable hashtable = hjstate->hj_HashTable;
	HashJoinTuple hashTuple = hjstate->hj_CurTuple;
	uint32		hashvalue = hjstate->hj_CurHashValue;

	/*
	 * hj_CurTuple 是上次从当前桶返回的元组的地址，          
	 * 如果该开始扫描新桶了则为 NULL。                
	 */
	if (hashTuple != NULL)
		hashTuple = ExecParallelHashNextTuple(hashtable, hashTuple);
	else
		hashTuple = ExecParallelHashFirstTuple(hashtable,
											   hjstate->hj_CurBucketNo);

	while (hashTuple != NULL)
	{
		if (hashTuple->hashvalue == hashvalue)
		{
			TupleTableSlot *inntuple;

			/* 把哈希表的元组放入执行槽，让 ExecQual 看到  */
			inntuple = ExecStoreMinimalTuple(HJTUPLE_MINTUPLE(hashTuple),
											 hjstate->hj_HashTupleSlot,
											 false);	/* 不要 pfree */
			econtext->ecxt_innertuple = inntuple;

			if (ExecQualAndReset(hjclauses, econtext))
			{
				hjstate->hj_CurTuple = hashTuple;
				return true;
			}
		}

		hashTuple = ExecParallelHashNextTuple(hashtable, hashTuple);
	}

	/*
	 * no match
	 */
	return false;
}

/*
 * ExecPrepHashTableForUnmatched
 *		为一系列 ExecScanHashTableForUnmatched 调用做准备
 */
void
ExecPrepHashTableForUnmatched(HashJoinState *hjstate)
{
	/*----------
	 * 扫描期间我们如下使用 HashJoinState 各字段：   
	 *
	 * hj_CurBucketNo: 下个待扫描的常规桶
	 * hj_CurSkewBucketNo: 下一倾斜桶（skewBucketNums 的下标）  
	 * hj_CurTuple: 上次返回的元组，NULL 即开始下一桶  
	 *----------
	 */
	hjstate->hj_CurBucketNo = 0;
	hjstate->hj_CurSkewBucketNo = 0;
	hjstate->hj_CurTuple = NULL;
}

/*
 * 判断本进程是否被允许执行未匹配扫描。若允许，      
 * 批屏障就会推进到 PHJ_BATCH_SCAN 并返回真。          
 * 否则该批会被分离，并返回假。            
 */
bool
ExecParallelPrepHashTableForUnmatched(HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			curbatch = hashtable->curbatch;
	ParallelHashJoinBatch *batch = hashtable->batches[curbatch].shared;

	Assert(BarrierPhase(&batch->batch_barrier) == PHJ_BATCH_PROBE);

	/*
	 * 在批屏障上等待并非无死锁，因为它处于                
	 * PHJ_BATCH_PROBE 阶段，因而附属于它的进程已经         
	 * 发出过元组。因此，我们将举行一次无等待选举：  
	 * 只有一个进程能继续进入下一阶段，所有其他进程    
	 * 都从该批分离。如果还有其他批次，它们仍能继续     
	 * 干活  
	 */
	if (!BarrierArriveAndDetachExceptLast(&batch->batch_barrier))
	{
		/* 该进程认为此批已经完成。         */
		hashtable->batches[hashtable->curbatch].done = true;

		/* 确保所有临时文件都已关闭。   */
		sts_end_parallel_scan(hashtable->batches[curbatch].inner_tuples);
		sts_end_parallel_scan(hashtable->batches[curbatch].outer_tuples);

		/*
		 * 记录我们见过的最大批，这通常发生在           
		 * ExecHashTableDetachBatch().
		 */
		hashtable->spacePeak =
			Max(hashtable->spacePeak,
				batch->size + sizeof(dsa_pointer_atomic) * hashtable->nbuckets);
		hashtable->curbatch = -1;
		return false;
	}

	/* 现在我们独占该批。       */
	Assert(BarrierPhase(&batch->batch_barrier) == PHJ_BATCH_SCAN);

	/*
	 * 是否有别的进程决定提前放弃，并命令所有进程       
	 * 跳过未匹配扫描？   
	 */
	if (batch->skip_unmatched)
	{
		hashtable->batches[hashtable->curbatch].done = true;
		ExecHashTableDetachBatch(hashtable);
		return false;
	}

	/* 现在准备进程本地状态，就像非并行连接那样。     */
	ExecPrepHashTableForUnmatched(hjstate);

	return true;
}

/*
 * ExecScanHashTableForUnmatched
 *		扫描哈希表中未匹配的内元组       
 *
 * 成功时，内元组会存入 hjstate->hj_CurTuple 以及         
 * econtext->ecxt_innertuple，使用 hjstate->hj_HashTupleSlot 作为   
 * 其槽位。   
 */
bool
ExecScanHashTableForUnmatched(HashJoinState *hjstate, ExprContext *econtext)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	HashJoinTuple hashTuple = hjstate->hj_CurTuple;

	for (;;)
	{
		/*
		 * hj_CurTuple 是上次从当前桶返回的元组的地址，  
		 * 如果该开始扫描一个新桶，则为                  
		 * NULL。
		 */
		if (hashTuple != NULL)
			hashTuple = hashTuple->next.unshared;
		else if (hjstate->hj_CurBucketNo < hashtable->nbuckets)
		{
			hashTuple = hashtable->buckets.unshared[hjstate->hj_CurBucketNo];
			hjstate->hj_CurBucketNo++;
		}
		else if (hjstate->hj_CurSkewBucketNo < hashtable->nSkewBuckets)
		{
			int			j = hashtable->skewBucketNums[hjstate->hj_CurSkewBucketNo];

			hashTuple = hashtable->skewBucket[j]->tuples;
			hjstate->hj_CurSkewBucketNo++;
		}
		else
			break;				/* 已扫完所有桶   */

		while (hashTuple != NULL)
		{
			if (!HeapTupleHeaderHasMatch(HJTUPLE_MINTUPLE(hashTuple)))
			{
				TupleTableSlot *inntuple;

				/* 把哈希表的元组插入执行槽    */
				inntuple = ExecStoreMinimalTuple(HJTUPLE_MINTUPLE(hashTuple),
												 hjstate->hj_HashTupleSlot,
												 false);	/* 不要 pfree */
				econtext->ecxt_innertuple = inntuple;

				/*
				 * 每次都重置临时内存；虽然本函数不做任何  
				 * 条件求值，调用方会做，故让它保持与
				 * ExecScanHashBucket 平行。   
				 */
				ResetExprContext(econtext);

				hjstate->hj_CurTuple = hashTuple;
				return true;
			}

			hashTuple = hashTuple->next.unshared;
		}

		/* 让此循环可以被取消       */
		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * 再无未匹配的元组
	 */
	return false;
}

/*
 * ExecParallelScanHashTableForUnmatched
 *		在并行连接中扫描哈希表中未匹配的内元组       
 *
 * 成功时，内元组会存入 hjstate->hj_CurTuple 以及         
 * econtext->ecxt_innertuple，使用 hjstate->hj_HashTupleSlot 作为   
 * 其槽位。   
 */
bool
ExecParallelScanHashTableForUnmatched(HashJoinState *hjstate,
									  ExprContext *econtext)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	HashJoinTuple hashTuple = hjstate->hj_CurTuple;

	for (;;)
	{
		/*
		 * hj_CurTuple 是上次从当前桶返回的元组的地址，  
		 * 如果该开始扫描一个新桶，则为                  
		 * NULL。
		 */
		if (hashTuple != NULL)
			hashTuple = ExecParallelHashNextTuple(hashtable, hashTuple);
		else if (hjstate->hj_CurBucketNo < hashtable->nbuckets)
			hashTuple = ExecParallelHashFirstTuple(hashtable,
												   hjstate->hj_CurBucketNo++);
		else
			break;				/* 已扫完所有桶   */

		while (hashTuple != NULL)
		{
			if (!HeapTupleHeaderHasMatch(HJTUPLE_MINTUPLE(hashTuple)))
			{
				TupleTableSlot *inntuple;

				/* 把哈希表的元组插入执行槽    */
				inntuple = ExecStoreMinimalTuple(HJTUPLE_MINTUPLE(hashTuple),
												 hjstate->hj_HashTupleSlot,
												 false);	/* 不要 pfree */
				econtext->ecxt_innertuple = inntuple;

				/*
				 * 每次都重置临时内存；虽然本函数不做任何  
				 * 条件求值，调用方会做，故让它保持与
				 * ExecScanHashBucket 平行。   
				 */
				ResetExprContext(econtext);

				hjstate->hj_CurTuple = hashTuple;
				return true;
			}

			hashTuple = ExecParallelHashNextTuple(hashtable, hashTuple);
		}

		/* 让此循环可以被取消       */
		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * 再无未匹配的元组
	 */
	return false;
}

/*
 * ExecHashTableReset
 *
 *		为新批重置哈希表头部       
 */
void
ExecHashTableReset(HashJoinTable hashtable)
{
	MemoryContext oldcxt;
	int			nbuckets = hashtable->nbuckets;

	/*
	 * 释放上一轮获得的全部哈希桶和元组，并为              
	 * 新一轮重新初始化上下文。    
	 */
	MemoryContextReset(hashtable->batchCxt);
	oldcxt = MemoryContextSwitchTo(hashtable->batchCxt);

	/* 重新分配并重新初始化哈希桶头部。     */
	hashtable->buckets.unshared = palloc0_array(HashJoinTuple, nbuckets);

	hashtable->spaceUsed = 0;

	MemoryContextSwitchTo(oldcxt);

	/* 忘记这些块（内存已由上面的上下文重置释放）。   */
	hashtable->chunks = NULL;
}

/*
 * ExecHashTableResetMatchFlags
 *		清除表中所有的 HeapTupleHeaderHasMatch 标志    
 */
void
ExecHashTableResetMatchFlags(HashJoinTable hashtable)
{
	HashJoinTuple tuple;
	int			i;

	/* 重置主表中的所有标志...     */
	for (i = 0; i < hashtable->nbuckets; i++)
	{
		for (tuple = hashtable->buckets.unshared[i]; tuple != NULL;
			 tuple = tuple->next.unshared)
			HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(tuple));
	}

	/* ...倾斜桶（若有的话）也同样处理 */
	for (i = 0; i < hashtable->nSkewBuckets; i++)
	{
		int			j = hashtable->skewBucketNums[i];
		HashSkewBucket *skewBucket = hashtable->skewBucket[j];

		for (tuple = skewBucket->tuples; tuple != NULL; tuple = tuple->next.unshared)
			HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(tuple));
	}
}


void
ExecReScanHash(HashState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/*
	 * 若子节点的 chgParam 非空，首个 ExecProcNode             
	 * 会重扫计划    
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}


/*
 * ExecHashBuildSkewHash
 *
 *		如果我们能识别出外关系连接键的最常用值             
 *		(MCV)，就为倾斜优化做好设置。我们为每个 MCV 的    
 *		哈希值建立一个倾斜哈希桶，数量上限取决于     
 *		可用内存。           
 */
static void
ExecHashBuildSkewHash(HashJoinTable hashtable, Hash *node, int mcvsToUse)
{
	HeapTupleData *statsTuple;
	AttStatsSlot sslot;

	/* 若规划器未识别出外关系的连接键，则什么都不做  */
	if (!OidIsValid(node->skewTable))
		return;
	/* 同样，若空间容不下至少一个倾斜桶，照样不做。  */
	if (mcvsToUse <= 0)
		return;

	/*
	 * 尝试为外关系的连接键找到 MCV 统计信息。         
	 */
	statsTuple = SearchSysCache3(STATRELATTINH,
								 ObjectIdGetDatum(node->skewTable),
								 Int16GetDatum(node->skewColumn),
								 BoolGetDatum(node->skewInherit));
	if (!HeapTupleIsValid(statsTuple))
		return;

	if (get_attstatsslot(&sslot, statsTuple,
						 STATISTIC_KIND_MCV, InvalidOid,
						 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
	{
		double		frac;
		int			nbuckets;
		FmgrInfo   *hashfunctions;
		int			i;

		if (mcvsToUse > sslot.nvalues)
			mcvsToUse = sslot.nvalues;

		/*
		 * 计算外关系中会参与倾斜优化的预期比例。  
		 * 如果该比例没有达到                                  
		 * SKEW_MIN_OUTER_FRACTION，则不使用倾斜优化。
		 */
		frac = 0;
		for (i = 0; i < mcvsToUse; i++)
			frac += sslot.numbers[i];
		if (frac < SKEW_MIN_OUTER_FRACTION)
		{
			free_attstatsslot(&sslot);
			ReleaseSysCache(statsTuple);
			return;
		}

		/*
		 * 好，设置倾斜哈希表。  
		 *
		 * skewBucket[] 是一个开放寻址哈希表，其大小是 2       
		 * 的幂且大于 MCV 值的个数。（这保证了至少会有     
		 * 一个空表项，因此查找总能                    
		 * 终止）  
		 *
		 * 注意：若 mcvsToUse 超过 INT_MAX/8 或                 
		 * MaxAllocSize/sizeof(void *)/8，则这段代码可能失败；   
		 * 因 pg_statistic 条目限制远小于此，不可能。   
		 */
		nbuckets = pg_nextpower2_32(mcvsToUse + 1);
		/* 多用两个位只是为了帮助避免冲突   */
		nbuckets <<= 2;

		hashtable->skewEnabled = true;
		hashtable->skewBucketLen = nbuckets;

		/*
		 * 我们在哈希表的批上下文中分配桶内存。它         
		 * 只在第一批期间需要，这确保它会在首批            
		 * 完成后被自动移除。                        
		 */
		hashtable->skewBucket = (HashSkewBucket **)
			MemoryContextAllocZero(hashtable->batchCxt,
								   nbuckets * sizeof(HashSkewBucket *));
		hashtable->skewBucketNums = (int *)
			MemoryContextAllocZero(hashtable->batchCxt,
								   mcvsToUse * sizeof(int));

		hashtable->spaceUsed += nbuckets * sizeof(HashSkewBucket *)
			+ mcvsToUse * sizeof(int);
		hashtable->spaceUsedSkew += nbuckets * sizeof(HashSkewBucket *)
			+ mcvsToUse * sizeof(int);
		if (hashtable->spaceUsed > hashtable->spacePeak)
			hashtable->spacePeak = hashtable->spaceUsed;

		/*
		 * 为每个 MCV 哈希值建一个倾斜桶。 
		 *
		 * 注意：极其重要的是，我们要按 MCV 频率递减      
		 * 的顺序创建这些桶。若必须移除一些桶，            
		 * 就必须按创建的相反顺序移除（参见          
		 * ExecHashRemoveNextSkewBucket），且最不常见的              
		 * MCV 先移除。 
		 */
		hashfunctions = hashtable->outer_hashfunctions;

		for (i = 0; i < mcvsToUse; i++)
		{
			uint32		hashvalue;
			int			bucket;

			hashvalue = DatumGetUInt32(FunctionCall1Coll(&hashfunctions[0],
														 hashtable->collations[0],
														 sslot.values[i]));

			/*
			 * 当尚未碰到哈希表中的空洞、也尚未碰到        
			 * 想要的桶时，就是与此前某个哈希值发生      
			 * 了碰撞，所以要试下一桶位。此代码须匹配  
			 * ExecHashGetSkewBucket。    
			 */
			bucket = hashvalue & (nbuckets - 1);
			while (hashtable->skewBucket[bucket] != NULL &&
				   hashtable->skewBucket[bucket]->hashvalue != hashvalue)
				bucket = (bucket + 1) & (nbuckets - 1);

			/*
			 * 如果找到了具有相同哈希值的现有桶，就       
			 * 保持原样。两个 MCV 共享哈希值没问题。  
			 */
			if (hashtable->skewBucket[bucket] != NULL)
				continue;

			/* 好，为该哈希值新建一个倾斜桶。      */
			hashtable->skewBucket[bucket] = (HashSkewBucket *)
				MemoryContextAlloc(hashtable->batchCxt,
								   sizeof(HashSkewBucket));
			hashtable->skewBucket[bucket]->hashvalue = hashvalue;
			hashtable->skewBucket[bucket]->tuples = NULL;
			hashtable->skewBucketNums[hashtable->nSkewBuckets] = bucket;
			hashtable->nSkewBuckets++;
			hashtable->spaceUsed += SKEW_BUCKET_OVERHEAD;
			hashtable->spaceUsedSkew += SKEW_BUCKET_OVERHEAD;
			if (hashtable->spaceUsed > hashtable->spacePeak)
				hashtable->spacePeak = hashtable->spaceUsed;
		}

		free_attstatsslot(&sslot);
	}

	ReleaseSysCache(statsTuple);
}

/*
 * ExecHashGetSkewBucket
 *
 *		返回此哈希值对应的倾斜桶下标或           
 *		INVALID_SKEW_BUCKET_NO（若未关联到任何   
 *		活动的倾斜桶。）               
 */
int
ExecHashGetSkewBucket(HashJoinTable hashtable, uint32 hashvalue)
{
	int			bucket;

	/*
	 * 若未进行倾斜优化，则总是返回 INVALID_SKEW                
	 * BUCKET_NO（尤其在初始批完成后会这样）。    
	 */
	if (!hashtable->skewEnabled)
		return INVALID_SKEW_BUCKET_NO;

	/*
	 * 由于 skewBucketLen 是 2 的幂，可用按位与做取模。   
	 */
	bucket = hashvalue & (hashtable->skewBucketLen - 1);

	/*
	 * 当尚未碰到哈希表中的空洞、也尚未碰到            
	 * 想要的桶时，就是与其他某个哈希值碰撞了，           
	 * 故试下一桶位。
	 */
	while (hashtable->skewBucket[bucket] != NULL &&
		   hashtable->skewBucket[bucket]->hashvalue != hashvalue)
		bucket = (bucket + 1) & (hashtable->skewBucketLen - 1);

	/*
	 * 找到想要的桶了？ 
	 */
	if (hashtable->skewBucket[bucket] != NULL)
		return bucket;

	/*
	 * 该哈希值绝不能在哈希表中已有表项。       
	 */
	return INVALID_SKEW_BUCKET_NO;
}

/*
 * ExecHashSkewTableInsert
 *
 *		把一个元组插入倾斜哈希表。
 *
 * 这大体上应当与 ExecHashTableInsert 当前             
 * 批情形一致。  
 */
static void
ExecHashSkewTableInsert(HashJoinTable hashtable,
						TupleTableSlot *slot,
						uint32 hashvalue,
						int bucketNumber)
{
	bool		shouldFree;
	MinimalTuple tuple = ExecFetchSlotMinimalTuple(slot, &shouldFree);
	HashJoinTuple hashTuple;
	int			hashTupleSize;

	/* 创建 HashJoinTuple     */
	hashTupleSize = HJTUPLE_OVERHEAD + tuple->t_len;
	hashTuple = (HashJoinTuple) MemoryContextAlloc(hashtable->batchCxt,
												   hashTupleSize);
	hashTuple->hashvalue = hashvalue;
	memcpy(HJTUPLE_MINTUPLE(hashTuple), tuple, tuple->t_len);
	HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(hashTuple));

	/* 把它推到倾斜桶链表的前端             */
	hashTuple->next.unshared = hashtable->skewBucket[bucketNumber]->tuples;
	hashtable->skewBucket[bucketNumber]->tuples = hashTuple;
	Assert(hashTuple != hashTuple->next.unshared);

	/* 统计已用空间，若用量过大则回退               */
	hashtable->spaceUsed += hashTupleSize;
	hashtable->spaceUsedSkew += hashTupleSize;
	if (hashtable->spaceUsed > hashtable->spacePeak)
		hashtable->spacePeak = hashtable->spaceUsed;
	while (hashtable->spaceUsedSkew > hashtable->spaceAllowedSkew)
		ExecHashRemoveNextSkewBucket(hashtable);

	/* 还要检查我们没有超出总的 spaceAllowed    */
	if (hashtable->spaceUsed > hashtable->spaceAllowed)
		ExecHashIncreaseNumBatches(hashtable);

	if (shouldFree)
		heap_free_minimal_tuple(tuple);
}

/*
 *		ExecHashRemoveNextSkewBucket
 *
 *		通过把其元组推入主哈希表，移除价值最低的    
 *		倾斜桶。        
 */
static void
ExecHashRemoveNextSkewBucket(HashJoinTable hashtable)
{
	int			bucketToRemove;
	HashSkewBucket *bucket;
	uint32		hashvalue;
	int			bucketno;
	int			batchno;
	HashJoinTuple hashTuple;

	/* 找到要移除的桶       */
	bucketToRemove = hashtable->skewBucketNums[hashtable->nSkewBuckets - 1];
	bucket = hashtable->skewBucket[bucketToRemove];

	/*
	 * 计算这些元组在主哈希表中属于哪个                 
	 * 桶和批。它们都有相同的哈希值，因此对                 
	 * 所有元组都一样。还要注意，处理这些元组时           
	 * nbatch 不可能增大。    
	 */
	hashvalue = bucket->hashvalue;
	ExecHashGetBucketAndBatch(hashtable, hashvalue, &bucketno, &batchno);

	/* 处理该桶中的所有元组   */
	hashTuple = bucket->tuples;
	while (hashTuple != NULL)
	{
		HashJoinTuple nextHashTuple = hashTuple->next.unshared;
		MinimalTuple tuple;
		Size		tupleSize;

		/*
		 * 此代码必须与 ExecHashTableInsert 保持一致。       
		 * 我们不直接用 ExecHashTableInsert，因为它           
		 * 要的是 TupleTableSlot，现有 HashJoinTuple。  
		 */
		tuple = HJTUPLE_MINTUPLE(hashTuple);
		tupleSize = HJTUPLE_OVERHEAD + tuple->t_len;

		/* 决定把元组放入哈希表还是临时文件                 */
		if (batchno == hashtable->curbatch)
		{
			/* 把元组移入主哈希表           */
			HashJoinTuple copyTuple;

			/*
			 * 必须把元组复制到稠密存储，否则诸如            
			 * ExecHashIncreaseNumBatches 找不到它。  
			 */
			copyTuple = (HashJoinTuple) dense_alloc(hashtable, tupleSize);
			memcpy(copyTuple, hashTuple, tupleSize);
			pfree(hashTuple);

			copyTuple->next.unshared = hashtable->buckets.unshared[bucketno];
			hashtable->buckets.unshared[bucketno] = copyTuple;

			/* 已缩减倾斜空间，但总空间不变                   */
			hashtable->spaceUsedSkew -= tupleSize;
		}
		else
		{
			/* 把元组放入临时文件留给后续批次    */
			Assert(batchno > hashtable->curbatch);
			ExecHashJoinSaveTuple(tuple, hashvalue,
								  &hashtable->innerBatchFile[batchno],
								  hashtable);
			pfree(hashTuple);
			hashtable->spaceUsed -= tupleSize;
			hashtable->spaceUsedSkew -= tupleSize;
		}

		hashTuple = nextHashTuple;

		/* 允许此循环可被取消       */
		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * 释放桶结构本身，并把哈希表条目重置为 NULL。      
	 *
	 * 注意：这远不像表面上看起来那么简单，因为          
	 * 哈希表中可能发生冲突。假设哈希值 A 和 B             
	 * 在某一哈希表条目处冲突，且 A 先插入，于是 B        
	 * 被移到了另一处条目。如果我们先移除 A，               
	 * 那么 ExecHashGetSkewBucket 就会错误地开始                 
	 * 报告 B 不在哈希表中，因为它会先命中 NULL               
	 * 而后才找到 B。不过，我们总是按创建的相反        
	 * 顺序移除，故此错误不可能发生。    
	 */
	hashtable->skewBucket[bucketToRemove] = NULL;
	hashtable->nSkewBuckets--;
	pfree(bucket);
	hashtable->spaceUsed -= SKEW_BUCKET_OVERHEAD;
	hashtable->spaceUsedSkew -= SKEW_BUCKET_OVERHEAD;

	/*
	 * 如果我们移除了所有倾斜桶，就放弃倾斜优化。       
	 * 释放这些数组，因为它们已不再有用。  
	 */
	if (hashtable->nSkewBuckets == 0)
	{
		hashtable->skewEnabled = false;
		pfree(hashtable->skewBucket);
		pfree(hashtable->skewBucketNums);
		hashtable->skewBucket = NULL;
		hashtable->skewBucketNums = NULL;
		hashtable->spaceUsed -= hashtable->spaceUsedSkew;
		hashtable->spaceUsedSkew = 0;
	}
}

/*
 * 在 DSM 段中为 instrumentation 数据预留空间。   
 */
void
ExecHashEstimate(HashState *node, ParallelContext *pcxt)
{
	size_t		size;

	/* 不做 instrumentation 或无 worker 时不需要  */
	if (!node->ps.instrument || pcxt->nworkers == 0)
		return;

	size = mul_size(pcxt->nworkers, sizeof(HashInstrumentation));
	size = add_size(size, offsetof(SharedHashInfo, hinstrument));
	shm_toc_estimate_chunk(&pcxt->estimator, size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/*
 * 在 DSM 中设一块空间，供所有 worker 记录 instrumentation    
 * 哈希表数据。     
 */
void
ExecHashInitializeDSM(HashState *node, ParallelContext *pcxt)
{
	size_t		size;

	/* 不做 instrumentation 或无 worker 时不需要  */
	if (!node->ps.instrument || pcxt->nworkers == 0)
		return;

	size = offsetof(SharedHashInfo, hinstrument) +
		pcxt->nworkers * sizeof(HashInstrumentation);
	node->shared_info = (SharedHashInfo *) shm_toc_allocate(pcxt->toc, size);

	/* 每个工作者区域必须初始为零。     */
	memset(node->shared_info, 0, size);

	node->shared_info->num_workers = pcxt->nworkers;
	shm_toc_insert(pcxt->toc, node->ps.plan->plan_node_id,
				   node->shared_info);
}

/*
 * 找到哈希表 instrumentation 数据的 DSM 空间，我们在         
 * 关闭时写入。  
 */
void
ExecHashInitializeWorker(HashState *node, ParallelWorkerContext *pwcxt)
{
	SharedHashInfo *shared_info;

	/* 无 instrumentation 时不需要     */
	if (!node->ps.instrument)
		return;

	/*
	 * 在共享区中找到我们的条目，并设置指向它的指针，
	 * 以便在关闭或重建哈希表时把统计累积到其             
	 * 中。
	 */
	shared_info = (SharedHashInfo *)
		shm_toc_lookup(pwcxt->toc, node->ps.plan->plan_node_id, false);
	node->hinstrument = &shared_info->hinstrument[ParallelWorkerNumber];
}

/*
 * 如需要则收集 EXPLAIN 统计，若曾调用过            
 * ExecHashInitializeWorker 就存入 DSM 内存，否则               
 * 存入本地存储。并行场景下，这一步必须在         
 * ExecShutdownHash() 而非 ExecEndHash() 中做，因后者已脱离 DSM
 * 段。  
 */
void
ExecShutdownHash(HashState *node)
{
	/* 若是 EXPLAIN 且尚未分配保存空间，则进行分配   */
	if (node->ps.instrument && !node->hinstrument)
		node->hinstrument = palloc0_object(HashInstrumentation);
	/* 现在为当前（最终）哈希表累积数据       */
	if (node->hinstrument && node->hashtable)
		ExecHashAccumInstrumentation(node->hinstrument, node->hashtable);
}

/*
 * 在 DSM 段分离之前取回各 worker 的 instrumentation         
 * 数据，供 EXPLAIN 访问。          
 */
void
ExecHashRetrieveInstrumentation(HashState *node)
{
	SharedHashInfo *shared_info = node->shared_info;
	size_t		size;

	if (shared_info == NULL)
		return;

	/* 用后端本地内存中的副本替换 node->shared_info。   */
	size = offsetof(SharedHashInfo, hinstrument) +
		shared_info->num_workers * sizeof(HashInstrumentation);
	node->shared_info = palloc(size);
	memcpy(node->shared_info, shared_info, size);
}

/*
 * 把 'hashtable' 的 instrumentation 数据累积到     
 * 初始清零的 HashInstrumentation。      
 *
 * 这用于在单个计划节点内的多个相继哈希表实例         
 * 之间合并信息。我们对每个关键数字取最大值。           
 * 最大的 nbuckets 与最大的 nbatch 可能出现在不同               
 * 实例中，因此把它们一起报告会有混淆的风险；         
 * 但若不报告最大值，误诊性能问题的风险更大。            
 * 同样，无论最大 spacePeak 是否发生在与最大                  
 * nbuckets 或 nbatch 相同的实例中，我们也要报告它。  
 * 所有实例的 nbuckets_original 与 nbatch_original 应当              
 * 相同；但在这里依赖它没什么价值，所以对它们         
 * 也一视同仁地处理。                            
 */
void
ExecHashAccumInstrumentation(HashInstrumentation *instrument,
							 HashJoinTable hashtable)
{
	instrument->nbuckets = Max(instrument->nbuckets,
							   hashtable->nbuckets);
	instrument->nbuckets_original = Max(instrument->nbuckets_original,
										hashtable->nbuckets_original);
	instrument->nbatch = Max(instrument->nbatch,
							 hashtable->nbatch);
	instrument->nbatch_original = Max(instrument->nbatch_original,
									  hashtable->nbatch_original);
	instrument->space_peak = Max(instrument->space_peak,
								 hashtable->spacePeak);
}

/*
 * 从当前活动的 HashMemoryChunk 分配 'size' 字节。     
 */
static void *
dense_alloc(HashJoinTable hashtable, Size size)
{
	HashMemoryChunk newChunk;
	char	   *ptr;

	/* 以防大小尚未按正确方式对齐               */
	size = MAXALIGN(size);

	/*
	 * 若元组大小超过阈值，就单独分配一个内存块。   
	 */
	if (size > HASH_CHUNK_THRESHOLD)
	{
		/* 分配新内存块并将其置于链表开头              */
		newChunk = (HashMemoryChunk) MemoryContextAlloc(hashtable->batchCxt,
														HASH_CHUNK_HEADER_SIZE + size);
		newChunk->maxlen = size;
		newChunk->used = size;
		newChunk->ntuples = 1;

		/*
		 * 把该内存块加到首个现存内存块之后，这样         
		 * 就不会丢失"current"内存块中的剩余空间。   
		 */
		if (hashtable->chunks != NULL)
		{
			newChunk->next = hashtable->chunks->next;
			hashtable->chunks->next.unshared = newChunk;
		}
		else
		{
			newChunk->next.unshared = hashtable->chunks;
			hashtable->chunks = newChunk;
		}

		return HASH_CHUNK_DATA(newChunk);
	}

	/*
	 * 看看当前内存块（若有）中是否尚有足够空间，     
	 * 没有则分配新块。    
	 */
	if ((hashtable->chunks == NULL) ||
		(hashtable->chunks->maxlen - hashtable->chunks->used) < size)
	{
		/* 分配新内存块并将其置于链表开头              */
		newChunk = (HashMemoryChunk) MemoryContextAlloc(hashtable->batchCxt,
														HASH_CHUNK_HEADER_SIZE + HASH_CHUNK_SIZE);

		newChunk->maxlen = HASH_CHUNK_SIZE;
		newChunk->used = size;
		newChunk->ntuples = 1;

		newChunk->next.unshared = hashtable->chunks;
		hashtable->chunks = newChunk;

		return HASH_CHUNK_DATA(newChunk);
	}

	/* 当前内存块中还有足够空间，把元组加进来       */
	ptr = HASH_CHUNK_DATA(hashtable->chunks) + hashtable->chunks->used;
	hashtable->chunks->used += size;
	hashtable->chunks->ntuples += 1;

	/* 返回指向元组内存起始处的指针      */
	return ptr;
}

/*
 * 在共享稠密存储中为一个元组分配空间。这等价于        
 * dense_alloc，但用于共享内存的并行哈希。 
 *
 * 在向共享内存载入元组时，我们可能耗尽内存并         
 * 决定重新分区，或者发现负载因子过高而决定            
 * 扩充桶数组，或者发现另一参与者已命令我们               
 * 协助做这件事。若桶数或批数已改变则返回                 
 * NULL，表示调用者必须重试（要考虑元组可能         
 * 已不再属于同一批）。                                  
 */
static HashJoinTuple
ExecParallelHashTupleAlloc(HashJoinTable hashtable, size_t size,
						   dsa_pointer *shared)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	dsa_pointer chunk_shared;
	HashMemoryChunk chunk;
	Size		chunk_size;
	HashJoinTuple result;
	int			curbatch = hashtable->curbatch;

	size = MAXALIGN(size);

	/*
	 * 快速路径：若本后端的当前内存块中空间足够，     
	 * 则无需加锁即可分配。           
	 */
	chunk = hashtable->current_chunk;
	if (chunk != NULL &&
		size <= HASH_CHUNK_THRESHOLD &&
		chunk->maxlen - chunk->used >= size)
	{

		chunk_shared = hashtable->current_chunk_shared;
		Assert(chunk == dsa_get_address(hashtable->area, chunk_shared));
		*shared = chunk_shared + HASH_CHUNK_HEADER_SIZE + chunk->used;
		result = (HashJoinTuple) (HASH_CHUNK_DATA(chunk) + chunk->used);
		chunk->used += size;

		Assert(chunk->used <= chunk->maxlen);
		Assert(result == dsa_get_address(hashtable->area, *shared));

		return result;
	}

	/* 慢路径：尝试分配新内存块。 */
	LWLockAcquire(&pstate->lock, LW_EXCLUSIVE);

	/*
	 * 检查是否需要协助增加桶数或批数。                   
	 */
	if (pstate->growth == PHJ_GROWTH_NEED_MORE_BATCHES ||
		pstate->growth == PHJ_GROWTH_NEED_MORE_BUCKETS)
	{
		ParallelHashGrowth growth = pstate->growth;

		hashtable->current_chunk = NULL;
		LWLockRelease(&pstate->lock);

		/* 另一参与者已命令我们协助扩充。      */
		if (growth == PHJ_GROWTH_NEED_MORE_BATCHES)
			ExecParallelHashIncreaseNumBatches(hashtable);
		else if (growth == PHJ_GROWTH_NEED_MORE_BUCKETS)
			ExecParallelHashIncreaseNumBuckets(hashtable);

		/* 调用者须重试。  */
		return NULL;
	}

	/* 超大元组有专用内存块。     */
	if (size > HASH_CHUNK_THRESHOLD)
		chunk_size = size + HASH_CHUNK_HEADER_SIZE;
	else
		chunk_size = HASH_CHUNK_SIZE;

	/* 检查是否到了扩充批或桶的时候。  */
	if (pstate->growth != PHJ_GROWTH_DISABLED)
	{
		Assert(curbatch == 0);
		Assert(BarrierPhase(&pstate->build_barrier) == PHJ_BUILD_HASH_INNER);

		/*
		 * 检查是否会超出我们的空间限制。为避免在       
		 * 特大元组或极低 hash_mem 设置而卡住，我们           
		 * 总允许每后端至少一个内存块。  
		 */
		if (hashtable->batches[0].at_least_one_chunk &&
			hashtable->batches[0].shared->size +
			chunk_size > pstate->space_allowed)
		{
			pstate->growth = PHJ_GROWTH_NEED_MORE_BATCHES;
			hashtable->batches[0].shared->space_exhausted = true;
			LWLockRelease(&pstate->lock);

			return NULL;
		}

		/* 检查是否超出我们的负载因子限制。  */
		if (hashtable->nbatch == 1)
		{
			hashtable->batches[0].shared->ntuples += hashtable->batches[0].ntuples;
			hashtable->batches[0].ntuples = 0;
			/* 防范整数溢出以及分配大小溢出             */
			if (hashtable->batches[0].shared->ntuples + 1 >
				hashtable->nbuckets * NTUP_PER_BUCKET &&
				hashtable->nbuckets < (INT_MAX / 2) &&
				hashtable->nbuckets * 2 <=
				MaxAllocSize / sizeof(dsa_pointer_atomic))
			{
				pstate->growth = PHJ_GROWTH_NEED_MORE_BUCKETS;
				LWLockRelease(&pstate->lock);

				return NULL;
			}
		}
	}

	/* 我们已获准分配新内存块。    */
	chunk_shared = dsa_allocate(hashtable->area, chunk_size);
	hashtable->batches[curbatch].shared->size += chunk_size;
	hashtable->batches[curbatch].at_least_one_chunk = true;

	/* 设置内存块。*/
	chunk = (HashMemoryChunk) dsa_get_address(hashtable->area, chunk_shared);
	*shared = chunk_shared + HASH_CHUNK_HEADER_SIZE;
	chunk->maxlen = chunk_size - HASH_CHUNK_HEADER_SIZE;
	chunk->used = size;

	/*
	 * 把它推入内存块链表，这样以后需要增加桶数          
	 * 或批数时（仅批 0）能够找到它，之后也便于           
	 * 释放内存（所有批）。   
	 */
	chunk->next.shared = hashtable->batches[curbatch].shared->chunks;
	hashtable->batches[curbatch].shared->chunks = chunk_shared;

	if (size <= HASH_CHUNK_THRESHOLD)
	{
		/*
		 * 把它设为当前内存块，这样后续调用就能走      
		 * 快速路径来填满其余部分。   
		 */
		hashtable->current_chunk = chunk;
		hashtable->current_chunk_shared = chunk_shared;
	}
	LWLockRelease(&pstate->lock);

	Assert(HASH_CHUNK_DATA(chunk) == dsa_get_address(hashtable->area, *shared));
	result = (HashJoinTuple) HASH_CHUNK_DATA(chunk);

	return result;
}

/*
 * 需要由一个后端来建立共享批状态，包括元组             
 * 存储。其他后端会调用它确保访问器配置正确：       
 * ExecParallelHashEnsureBatchAccessors()。     
 */
static void
ExecParallelHashJoinSetUpBatches(HashJoinTable hashtable, int nbatch)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	ParallelHashJoinBatch *batches;
	MemoryContext oldcxt;
	int			i;

	Assert(hashtable->batches == NULL);

	/* 分配空间。 */
	pstate->batches =
		dsa_allocate0(hashtable->area,
					  EstimateParallelHashJoinBatch(hashtable) * nbatch);
	pstate->nbatch = nbatch;
	batches = dsa_get_address(hashtable->area, pstate->batches);

	/*
	 * 用哈希连接溢出内存上下文来分配访问器，包括    
	 * 临时文件的缓冲区。     
	 */
	oldcxt = MemoryContextSwitchTo(hashtable->spillCxt);

	/* 分配本后端的访问器数组。    */
	hashtable->nbatch = nbatch;
	hashtable->batches =
		palloc0_array(ParallelHashJoinBatchAccessor, hashtable->nbatch);

	/* 设置共享状态、元组存储以及后端本地访问器。   */
	for (i = 0; i < hashtable->nbatch; ++i)
	{
		ParallelHashJoinBatchAccessor *accessor = &hashtable->batches[i];
		ParallelHashJoinBatch *shared = NthParallelHashJoinBatch(batches, i);
		char		name[MAXPGPATH];

		/*
		 * shared 的所有成员均已清零。只需设置                
		 * Barrier。     
		 */
		BarrierInit(&shared->batch_barrier, 0);
		if (i == 0)
		{
			/* 批 0 无需载入。              */
			BarrierAttach(&shared->batch_barrier);
			while (BarrierPhase(&shared->batch_barrier) < PHJ_BATCH_PROBE)
				BarrierArriveAndWait(&shared->batch_barrier, 0);
			BarrierDetach(&shared->batch_barrier);
		}

		/* 初始化访问器状态。所有成员均已初始化为零。*/
		accessor->shared = shared;

		/* 初始化共享元组存储。     */
		snprintf(name, sizeof(name), "i%dof%d", i, hashtable->nbatch);
		accessor->inner_tuples =
			sts_initialize(ParallelHashJoinBatchInner(shared),
						   pstate->nparticipants,
						   ParallelWorkerNumber + 1,
						   sizeof(uint32),
						   SHARED_TUPLESTORE_SINGLE_PASS,
						   &pstate->fileset,
						   name);
		snprintf(name, sizeof(name), "o%dof%d", i, hashtable->nbatch);
		accessor->outer_tuples =
			sts_initialize(ParallelHashJoinBatchOuter(shared,
													  pstate->nparticipants),
						   pstate->nparticipants,
						   ParallelWorkerNumber + 1,
						   sizeof(uint32),
						   SHARED_TUPLESTORE_SINGLE_PASS,
						   &pstate->fileset,
						   name);
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 * 释放当前的这组 ParallelHashJoinBatchAccessor 对象。 
 */
static void
ExecParallelHashCloseBatchAccessors(HashJoinTable hashtable)
{
	int			i;

	for (i = 0; i < hashtable->nbatch; ++i)
	{
		/* 确保无文件保持打开。    */
		sts_end_write(hashtable->batches[i].inner_tuples);
		sts_end_write(hashtable->batches[i].outer_tuples);
		sts_end_parallel_scan(hashtable->batches[i].inner_tuples);
		sts_end_parallel_scan(hashtable->batches[i].outer_tuples);
	}
	pfree(hashtable->batches);
	hashtable->batches = NULL;
}

/*
 * 确保本后端对当前这组批拥有最新的访问                
 * 器。  
 */
static void
ExecParallelHashEnsureBatchAccessors(HashJoinTable hashtable)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	ParallelHashJoinBatch *batches;
	MemoryContext oldcxt;
	int			i;

	if (hashtable->batches != NULL)
	{
		if (hashtable->nbatch == pstate->nbatch)
			return;
		ExecParallelHashCloseBatchAccessors(hashtable);
	}

	/*
	 * 批跟踪数组已被释放的状态是我们绝不该看到        
	 * 的，因为如果我们在构建屏障进入 PHJ_BUILD_FREE    
	 * 阶段时才加入，本应更早放弃。   
	 */
	Assert(DsaPointerIsValid(pstate->batches));

	/*
	 * 用哈希连接溢出内存上下文来分配访问器，包括    
	 * 临时文件的缓冲区。     
	 */
	oldcxt = MemoryContextSwitchTo(hashtable->spillCxt);

	/* 分配本后端的访问器数组。    */
	hashtable->nbatch = pstate->nbatch;
	hashtable->batches =
		palloc0_array(ParallelHashJoinBatchAccessor, hashtable->nbatch);

	/* 找到 ParallelHashJoinBatch 对象伪数组的基址。            */
	batches = (ParallelHashJoinBatch *)
		dsa_get_address(hashtable->area, pstate->batches);

	/* 设置访问器数组并附着到元组存储。         */
	for (i = 0; i < hashtable->nbatch; ++i)
	{
		ParallelHashJoinBatchAccessor *accessor = &hashtable->batches[i];
		ParallelHashJoinBatch *shared = NthParallelHashJoinBatch(batches, i);

		accessor->shared = shared;
		accessor->preallocated = 0;
		accessor->done = false;
		accessor->outer_eof = false;
		accessor->inner_tuples =
			sts_attach(ParallelHashJoinBatchInner(shared),
					   ParallelWorkerNumber + 1,
					   &pstate->fileset);
		accessor->outer_tuples =
			sts_attach(ParallelHashJoinBatchOuter(shared,
												  pstate->nparticipants),
					   ParallelWorkerNumber + 1,
					   &pstate->fileset);
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 * 为给定批分配一个空的共享内存哈希表。       
 */
void
ExecParallelHashTableAlloc(HashJoinTable hashtable, int batchno)
{
	ParallelHashJoinBatch *batch = hashtable->batches[batchno].shared;
	dsa_pointer_atomic *buckets;
	int			nbuckets = hashtable->parallel_state->nbuckets;
	int			i;

	batch->buckets =
		dsa_allocate(hashtable->area, sizeof(dsa_pointer_atomic) * nbuckets);
	buckets = (dsa_pointer_atomic *)
		dsa_get_address(hashtable->area, batch->buckets);
	for (i = 0; i < nbuckets; ++i)
		dsa_pointer_atomic_init(&buckets[i], InvalidDsaPointer);
}

/*
 * 若我们当前附着于共享哈希连接批，则脱离；            
 * 若最后脱离则清理。  
 */
void
ExecHashTableDetachBatch(HashJoinTable hashtable)
{
	if (hashtable->parallel_state != NULL &&
		hashtable->curbatch >= 0)
	{
		int			curbatch = hashtable->curbatch;
		ParallelHashJoinBatch *batch = hashtable->batches[curbatch].shared;
		bool		attached = true;

		/* 确保所有临时文件都已关闭。   */
		sts_end_parallel_scan(hashtable->batches[curbatch].inner_tuples);
		sts_end_parallel_scan(hashtable->batches[curbatch].outer_tuples);

		/* 附着之后我们至少总能到达 PHJ_BATCH_PROBE。    */
		Assert(BarrierPhase(&batch->batch_barrier) == PHJ_BATCH_PROBE ||
			   BarrierPhase(&batch->batch_barrier) == PHJ_BATCH_SCAN);

		/*
		 * 如果我们在尚未走完 PHJ_BATCH_PROBE 阶段时就          
		 * 提前放弃它，意味着计划不再想要更多元组，  
		 * 并且乐于放弃缓冲在本进程子计划中的元组。  
		 * 为了正确性，我们不能允许任何进程执行             
		 * PHJ_BATCH_SCAN 阶段，因为我们永远不会有完整       
		 * 的匹配位集合。因此我们在所有后端中都跳过     
		 * 发出不匹配的元组（如果这是全/右连接），仿佛  
		 * 这些元组本该全部由本进程发出，而它也放弃
		 * 了它们。       
		 */
		if (BarrierPhase(&batch->batch_barrier) == PHJ_BATCH_PROBE &&
			!hashtable->batches[curbatch].outer_eof)
		{
			/*
			 * PHJ_BATCH_PROBE 期间，多个后端都可能写入   
			 * 此标志，但只在 PHJ_BATCH_SCAN 阶段读取，故       
			 * 无需额外加锁。              
			 */
			batch->skip_unmatched = true;
		}

		/*
		 * 即使我们不做全/右外连接，也要走一遍               
		 * PHJ_BATCH_SCAN 阶段，以维持"在 PHJ_BATCH_FREE        
		 * 中释放的不变量"，但那将是无等待的。        
		 */
		if (BarrierPhase(&batch->batch_barrier) == PHJ_BATCH_PROBE)
			attached = BarrierArriveAndDetachExceptLast(&batch->batch_barrier);
		if (attached && BarrierArriveAndDetach(&batch->batch_barrier))
		{
			/*
			 * 我们已不再附着于批屏障，但我们是被选中     
			 * 释放资源的进程，此时断言当前阶段是安全 
			 * 的。只要我们仍附着于构建屏障，                
			 * ParallelHashJoinBatch 就不会在我们脚下消失，       
			 * 故此访问安全。   
			 */
			Assert(BarrierPhase(&batch->batch_barrier) == PHJ_BATCH_FREE);

			/* 释放共享内存块和桶。  */
			while (DsaPointerIsValid(batch->chunks))
			{
				HashMemoryChunk chunk =
					dsa_get_address(hashtable->area, batch->chunks);
				dsa_pointer next = chunk->next.shared;

				dsa_free(hashtable->area, batch->chunks);
				batch->chunks = next;
			}
			if (DsaPointerIsValid(batch->buckets))
			{
				dsa_free(hashtable->area, batch->buckets);
				batch->buckets = InvalidDsaPointer;
			}
		}

		/*
		 * 记录我们附着过的最大批次。虽然每个后端   
		 * 看到的批次子集可能不同，explain.c 将扫描        
		 * 所有后端的结果来找最大值。                      
		 */
		hashtable->spacePeak =
			Max(hashtable->spacePeak,
				batch->size + sizeof(dsa_pointer_atomic) * hashtable->nbuckets);

		/* 记住我们未附着于任何批。          */
		hashtable->curbatch = -1;
	}
}

/*
 * 脱离所有共享资源。若我们是最后脱离的，则清理。 
 */
void
ExecHashTableDetach(HashJoinTable hashtable)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;

	/*
	 * 如果我们参与了并行查询，那么要么已一路走到      
	 * PHJ_BUILD_RUN，要么加入太晚而处于 PHJ_BUILD_FREE。         
	 */
	Assert(!pstate ||
		   BarrierPhase(&pstate->build_barrier) >= PHJ_BUILD_RUN);

	if (pstate && BarrierPhase(&pstate->build_barrier) == PHJ_BUILD_RUN)
	{
		int			i;

		/* 确保所有临时文件都已关闭。   */
		if (hashtable->batches)
		{
			for (i = 0; i < hashtable->nbatch; ++i)
			{
				sts_end_write(hashtable->batches[i].inner_tuples);
				sts_end_write(hashtable->batches[i].outer_tuples);
				sts_end_parallel_scan(hashtable->batches[i].inner_tuples);
				sts_end_parallel_scan(hashtable->batches[i].outer_tuples);
			}
		}

		/* 若最后脱离，则清理共享内存。       */
		if (BarrierArriveAndDetach(&pstate->build_barrier))
		{
			/*
			 * 较晚加入的进程会看到这一状态并立      
			 * 即放弃。
			 */
			Assert(BarrierPhase(&pstate->build_barrier) == PHJ_BUILD_FREE);

			if (DsaPointerIsValid(pstate->batches))
			{
				dsa_free(hashtable->area, pstate->batches);
				pstate->batches = InvalidDsaPointer;
			}
		}
	}
	hashtable->parallel_state = NULL;
}

/*
 * 获取编号所标识的给定桶中的第一个元组。  
 */
static inline HashJoinTuple
ExecParallelHashFirstTuple(HashJoinTable hashtable, int bucketno)
{
	HashJoinTuple tuple;
	dsa_pointer p;

	Assert(hashtable->parallel_state);
	p = dsa_pointer_atomic_read(&hashtable->buckets.shared[bucketno]);
	tuple = (HashJoinTuple) dsa_get_address(hashtable->area, p);

	return tuple;
}

/*
 * 取 'tuple' 所在桶中的下一个元组。    
 */
static inline HashJoinTuple
ExecParallelHashNextTuple(HashJoinTable hashtable, HashJoinTuple tuple)
{
	HashJoinTuple next;

	Assert(hashtable->parallel_state);
	next = (HashJoinTuple) dsa_get_address(hashtable->area, tuple->next.shared);

	return next;
}

/*
 * 把一个元组以原子方式插入 DSA 内存中的元组链头部。   
 */
static inline void
ExecParallelHashPushTuple(dsa_pointer_atomic *head,
						  HashJoinTuple tuple,
						  dsa_pointer tuple_shared)
{
	for (;;)
	{
		tuple->next.shared = dsa_pointer_atomic_read(head);
		if (dsa_pointer_atomic_compare_exchange(head,
												&tuple->next.shared,
												tuple_shared))
			break;
	}
}

/*
 * 准备处理给定批。         
 */
void
ExecParallelHashTableSetCurrentBatch(HashJoinTable hashtable, int batchno)
{
	Assert(hashtable->batches[batchno].shared->buckets != InvalidDsaPointer);

	hashtable->curbatch = batchno;
	hashtable->buckets.shared = (dsa_pointer_atomic *)
		dsa_get_address(hashtable->area,
						hashtable->batches[batchno].shared->buckets);
	hashtable->nbuckets = hashtable->parallel_state->nbuckets;
	hashtable->log2_nbuckets = my_log2(hashtable->nbuckets);
	hashtable->current_chunk = NULL;
	hashtable->current_chunk_shared = InvalidDsaPointer;
	hashtable->batches[batchno].at_least_one_chunk = false;
}

/*
 * 从正被并行处理的内存块队列中取出下一个                
 * 可用内存块；若已没有则返回 NULL，否则返回               
 * 指向该块的指针，并把 *shared 设为其 DSA 指针。
 */
static HashMemoryChunk
ExecParallelHashPopChunkQueue(HashJoinTable hashtable, dsa_pointer *shared)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	HashMemoryChunk chunk;

	LWLockAcquire(&pstate->lock, LW_EXCLUSIVE);
	if (DsaPointerIsValid(pstate->chunk_work_queue))
	{
		*shared = pstate->chunk_work_queue;
		chunk = (HashMemoryChunk)
			dsa_get_address(hashtable->area, *shared);
		pstate->chunk_work_queue = chunk->next.shared;
	}
	else
		chunk = NULL;
	LWLockRelease(&pstate->lock);

	return chunk;
}

/*
 * 把本后端为给定内关系批预分配的空间至少再              
 * 增加给定数量。这让我们能跟踪某批重新载入           
 * 内存时能否装得下。如有需要，还增加批数或         
 * 桶数。                      
 *
 * 它模拟内存块将被分发给 worker 的方式，对我们             
 * 把批重新载入内存时将占用多少空间维持着一               
 * 个动态估计。这并非完全准确，因为元组打包           
 * 进内存块的方式会因 ExecParallelHashTupleAlloc()                  
 * 而有所不同，但应该相当接近。由于所有 worker          
 * 都在散列期间一起做预分配，故往往会把每个             
 * worker 高估约一个内存块的几分之一；但如果批             
 * 数够多，各 worker 往往单独重载，留下较少的半   
 * 满块。     
 *
 * 如果批数或桶数已变化，则返回 false，调用者应当   
 * 重新考虑给定元组现在属于哪个批，并再次调              
 * 用。
 */
static bool
ExecParallelHashTuplePrealloc(HashJoinTable hashtable, int batchno, size_t size)
{
	ParallelHashJoinState *pstate = hashtable->parallel_state;
	ParallelHashJoinBatchAccessor *batch = &hashtable->batches[batchno];
	size_t		want = Max(size, HASH_CHUNK_SIZE - HASH_CHUNK_HEADER_SIZE);

	Assert(batchno > 0);
	Assert(batchno < hashtable->nbatch);
	Assert(size == MAXALIGN(size));

	LWLockAcquire(&pstate->lock, LW_EXCLUSIVE);

	/* 是否已有参与者命令我们协助扩充？   */
	if (pstate->growth == PHJ_GROWTH_NEED_MORE_BATCHES ||
		pstate->growth == PHJ_GROWTH_NEED_MORE_BUCKETS)
	{
		ParallelHashGrowth growth = pstate->growth;

		LWLockRelease(&pstate->lock);
		if (growth == PHJ_GROWTH_NEED_MORE_BATCHES)
			ExecParallelHashIncreaseNumBatches(hashtable);
		else if (growth == PHJ_GROWTH_NEED_MORE_BUCKETS)
			ExecParallelHashIncreaseNumBuckets(hashtable);

		return false;
	}

	if (pstate->growth != PHJ_GROWTH_DISABLED &&
		batch->at_least_one_chunk &&
		(batch->shared->estimated_size + want + HASH_CHUNK_HEADER_SIZE
		 > pstate->space_allowed))
	{
		/*
		 * 我们已判定该批如果载入内存就会超出空间          
		 * 预算。命令所有参与者协助重新分区。               
		 */
		batch->shared->space_exhausted = true;
		pstate->growth = PHJ_GROWTH_NEED_MORE_BATCHES;
		LWLockRelease(&pstate->lock);

		return false;
	}

	batch->at_least_one_chunk = true;
	batch->shared->estimated_size += want + HASH_CHUNK_HEADER_SIZE;
	batch->preallocated = want;
	LWLockRelease(&pstate->lock);

	return true;
}

/*
 * 计算 Hash 及类似计划类型可用内存的限制。             
 * 它等于 work_mem 乘以 hash_mem_multiplier，               
 * 以字节计。    
 *
 * 导出供规划器以及其他类哈希执行节点使用。        
 * 放在这里颇为随意，因为再也找不到更好的地          
 * 方。
 */
size_t
get_hash_memory_limit(void)
{
	double		mem_limit;

	/* 先用双精度算术做初始计算        */
	mem_limit = (double) work_mem * hash_mem_multiplier * 1024.0;

	/* 以防装不进 size_t 而做钳制    */
	mem_limit = Min(mem_limit, (double) SIZE_MAX);

	return (size_t) mem_limit;
}
