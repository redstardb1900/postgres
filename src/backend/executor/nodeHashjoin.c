/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.c
 *	  hash join 节点的处理例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeHashjoin.c
 *
 * HASH JOIN
 *
 * 这基于下列页面中简述的 "hybrid hash join"                      
 * 算法。     
 *
 *   https://en.wikipedia.org/wiki/Hash_join#Hybrid_hash_join
 *
 * 详见所引论文：                 
 *
 *   "An Adaptive Hash Join Algorithm for Multiuser Environments"
 *   Hansjörg Zeller; Jim Gray (1990). Proceedings of the 16th VLDB conference.
 *   Brisbane: 186–197.
 *
 * 如果 hash join 的内侧元组放不进内存，                         
 * 可以分多个批次执行。      
 *
 * 如果内侧关系的统计信息足够准确，规划器会选择          
 * 多批次策略并估算批次数。                     
 *
 * 查询执行器会测量哈希表的实际大小，如果哈希表          
 * 增长过大就增加批次数。                  
 *
 * 批次数总是 2 的幂，因此批次数的增加                         
 * 会使其翻倍。    
 *
 * 串行 hash join 惰性地测量批次大小 -- 直到加载某个          
 * 批次时才判断它能否放入内存。向哈希表插入元组          
 * 时，如果该元组会超过 work_mem，串行 hash join              
 * 就会倾倒出哈希表，并把元组重新分配到其他批次         
 * 文件或哈希表当前驻留批次。 
 *
 * 而并行 hash join 则会在构建阶段完成对批次数的            
 * 全部修改。如果它增加批次数，就会把所有批次中          
 * 的全部元组都倾倒出来，重新分配到全新的批次文件。    
 * 然后它会检查每个批次，确保其能放入查询的              
 * 空间预算内。   
 *
 * 在并行和串行 hash join 中，执行器目前都只是尽力         
 * 而为。如果某个批次放不进内存，它会尝试把批次         
 * 数翻倍。如果增加批次后，某个批次保留了自己     
 * 全部或没有任何元组，执行器就会全局禁用批次        
 * 数的增长。禁用增长后，所有原本会触发批次数             
 * 增加的批次，改为超出                                        
 * 允许的空间。       
 *
 * PARALLELISM
 *
 * hash join 能以多种方式参与并行查询执行。一种             
 * 是并行无关的 hash join，节点并未意识到自己              
 * 处于并行计划中。这种情况下，每个后端都会用内         
 * 侧计划的副本构建一份哈希表副本，而外层计划既       
 * 可以来自部分路径，也可以来自完整路径，因此该         
 * hash join 的结果也相应地为部分或完整。另一种并           
 * 行感知的 hash join 行为不同，会在多个后端间协      
 * 调工作，并在 EXPLAIN 输出中显示为 Parallel Hash               
 * Join，且总伴随 Parallel Hash 节点出现。  
 *
 * 并行感知的 hash join 使用与并行无关 hash join 相同         
 * 的每后端状态机来跟踪 hash join 算法的执行进度。         
 * 在并行感知的 hash join 中，还有一个共享状态机，       
 * 协作的后端用它来同步各自的本地状态机和程序        
 * 计数器。共享状态机由 Barrier IPC 原语来管理。            
 * 当所有已加入的参与者都到达屏障时，阶段就会前       
 * 进，所有等待中的参与者都会被释放。
 *
 * 当参与者开始在并行 hash join 上工作时，必须先弄       
 * 清楚已经完成了多少进度，因为参与者之间并不会      
 * 互相等待对方开始。因此代码中的关键位置设有      
 * switch 语句，我们需要在这些位置把本地状态机与           
 * 阶段进行同步，然后跳转到算法的正确位置，以便    
 * 开始投入工作。                
 *
 * 一个名为 build_barrier 的屏障用于协调哈希的各              
 * 阶段由一个整数表示，它从 0 开始逐次加一，但在代     
 * 码中以符号名来指代，如下所示。                              
 * 星号表示由任意选出的单个进程所执行的                 
 * 某个阶段。
 *
 *   PHJ_BUILD_ELECT                 -- 初始状态 
 *   PHJ_BUILD_ALLOCATE*             -- 一个进程建立批次和表 0   
 *   PHJ_BUILD_HASH_INNER            -- 均哈希内关系    
 *   PHJ_BUILD_HASH_OUTER            -- （仅多批次）均哈希外关系 
 *   PHJ_BUILD_RUN                   -- 构建完成，可以探测     
 *   PHJ_BUILD_FREE*                 -- 全部完成，某进程释放批次
 *
 * 在 PHJ_BUILD_HASH_INNER 阶段期间，可以按需反复             
 * 使用另一对屏障来协调批次数或桶数的扩展。它         
 * 们的阶段如下：                            
 *
 *   PHJ_GROW_BATCHES_ELECT          -- 初始状态 
 *   PHJ_GROW_BATCHES_REALLOCATE*    -- 某进程分配新批次 
 *   PHJ_GROW_BATCHES_REPARTITION    -- 均重新分区
 *   PHJ_GROW_BATCHES_DECIDE*        -- 某进程检测偏斜并清理
 *   PHJ_GROW_BATCHES_FINISH         -- 完成一轮增长周期 
 *
 *   PHJ_GROW_BUCKETS_ELECT          -- 初始状态 
 *   PHJ_GROW_BUCKETS_REALLOCATE*    -- 某进程分配新桶    
 *   PHJ_GROW_BUCKETS_REINSERT       -- 均重新插入  
 *
 * 如果规划器把批次和桶的数量估计对了，这些步骤        
 * 就无需进行；但另一方面，在对内关系做哈希的过       
 * 程中，为了不超出内存预算和负载因子目标，可能        
 * 需要多次扩展桶或批次。因此这里用的是                  
 * 一对采用循环阶段的独立屏障。      
 *
 * 只有多批次 join 才需要 PHJ_BUILD_HASH_OUTER 阶段，         
 * 因为必须预先把外层关系划分成批次，才能让各批         
 * 次完全独立地处理。相比之下，并行无关算法只是     
 * 在扫描和探测过程中遇到元组时，直接把它们'向前'  
 * 地抛给'靠后'的批次，因为它按串行顺序处理批次，    
 * 所以可以这样做。                            
 *
 * 到达 PHJ_BUILD_RUN 后，各后端就会分开处理不同     
 * 的批次，或者如果批次不够分，就联合起来共同探         
 * 测批次。每个批次都有一个独立的屏障，其阶段如     
 * 下：                    
 *
 *  PHJ_BATCH_ELECT          -- 初始状态 
 *  PHJ_BATCH_ALLOCATE*      -- 某进程分配桶   
 *  PHJ_BATCH_LOAD           -- 全部从磁盘装入哈希表   
 *  PHJ_BATCH_PROBE          -- 均探测
 *  PHJ_BATCH_SCAN*          -- 某进程做右/右反/全未匹配扫描    
 *  PHJ_BATCH_FREE*          -- 内存被释放 
 *
 * 批次 0 是特例，因为它一开始就处于         
 * PHJ_BATCH_PROBE 阶段；批次 0 的哈希表在              
 * PHJ_BUILD_HASH_INNER期间完成可跳过。
 *
 * 最初我们尝试按单批次 hash join 来做计划，利用所       
 * 有参与者的合并 hash_mem 创建一个大型共享哈希             
 * 表。如果在计划或执行时发现这行不通，就退回到     
 * 常规 hash_mem 大小的哈希表。           
 *
 * 为避免死锁，除非已知所有加入该屏障的其他后端       
 * 正在主动执行该节点或已完成，否则我们绝不在任       
 * 何屏障上等待。实际上，这意味着在加入屏障期间          
 * 我们绝不输出元组，除非屏障已到达这样的阶段：     
 * 不会再有进程在它上面等待。我们在 PHJ_BUILD_RUN            
 * 阶段时加入构建屏障输出元组，在 PHJ_BATCH_PROBE      
 * 阶段加入每批次屏障。它们分别无等待地推进到          
 * PHJ_BUILD_FREE 和 PHJ_BATCH_SCAN，分别使用                
 * BarrierArriveAndDetach() 和 BarrierArriveAndDetachExceptLast()。  
 * 最后分离者会收到不同的返回值，从而知道可以安  
 * 全地清理。在该阶段之后才加入的任何落后进程，        
 * 都会发现再想参与或访问相关共享内存对象               
 * 为时已晚。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "executor/executor.h"
#include "executor/hashjoin.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/memutils.h"
#include "utils/sharedtuplestore.h"


/*
 * ExecHashJoin 状态机的各个状态   
 */
#define HJ_BUILD_HASHTABLE		1
#define HJ_NEED_NEW_OUTER		2
#define HJ_SCAN_BUCKET			3
#define HJ_FILL_OUTER_TUPLE		4
#define HJ_FILL_INNER_TUPLES	5
#define HJ_NEED_NEW_BATCH		6

/* 若正对外层关系做空填充则返回 true   */
#define HJ_FILL_OUTER(hjstate)	((hjstate)->hj_NullInnerTupleSlot != NULL)
/* 若正对内层关系做空填充则返回 true   */
#define HJ_FILL_INNER(hjstate)	((hjstate)->hj_NullOuterTupleSlot != NULL)

static TupleTableSlot *ExecHashJoinOuterGetTuple(PlanState *outerNode,
												 HashJoinState *hjstate,
												 uint32 *hashvalue);
static TupleTableSlot *ExecParallelHashJoinOuterGetTuple(PlanState *outerNode,
														 HashJoinState *hjstate,
														 uint32 *hashvalue);
static TupleTableSlot *ExecHashJoinGetSavedTuple(HashJoinState *hjstate,
												 BufFile *file,
												 uint32 *hashvalue,
												 TupleTableSlot *tupleSlot);
static bool ExecHashJoinNewBatch(HashJoinState *hjstate);
static bool ExecParallelHashJoinNewBatch(HashJoinState *hjstate);
static void ExecParallelHashJoinPartitionOuter(HashJoinState *hjstate);


/* ----------------------------------------------------------------
 *		ExecHashJoinImpl
 *
 *		该函数实现了 Hybrid Hashjoin 算法。它被标记为          
 *		always-inline 属性，这样 ExecHashJoin() 和          
 *		ExecParallelHashJoin() 可以内联它。遵循该属性的      
 *		编译器应当生成针对 parallel == true 和                     
 *		== false 的特化版本，去掉多余分支。    
 *
 *		注意：其上建立哈希表的关系是 "inner"，   
 *			  另一个是 "outer"。  
 * ----------------------------------------------------------------
 */
static pg_attribute_always_inline TupleTableSlot *
ExecHashJoinImpl(PlanState *pstate, bool parallel)
{
	HashJoinState *node = castNode(HashJoinState, pstate);
	PlanState  *outerNode;
	HashState  *hashNode;
	ExprState  *joinqual;
	ExprState  *otherqual;
	ExprContext *econtext;
	HashJoinTable hashtable;
	TupleTableSlot *outerTupleSlot;
	uint32		hashvalue;
	int			batchno;
	ParallelHashJoinState *parallel_state;

	/*
	 * 从 HashJoin 节点获取信息   
	 */
	joinqual = node->js.joinqual;
	otherqual = node->js.ps.qual;
	hashNode = (HashState *) innerPlanState(node);
	outerNode = outerPlanState(node);
	hashtable = node->hj_HashTable;
	econtext = node->js.ps.ps_ExprContext;
	parallel_state = hashNode->parallel_state;

	/*
	 * 重置每元组内存上下文，释放上一个元组周期里 
	 * 分配的表达式求值存储。             
	 */
	ResetExprContext(econtext);

	/*
	 * 运行 hash join 状态机     
	 */
	for (;;)
	{
		/*
		 * 在有些异常情形下，该循环可能迭代多次才返回 
		 * 一条元组，比如要把当前批次一大半移到           
		 * 后面的批次。因此每轮循环都                           
		 * 检查中断。   
		 */
		CHECK_FOR_INTERRUPTS();

		switch (node->hj_JoinState)
		{
			case HJ_BUILD_HASHTABLE:

				/*
				 * 第一遍时：为内侧关系构建哈希表。        
				 */
				Assert(hashtable == NULL);

				/*
				 * 如果外层关系完全为空，且这不是          
				 * 右/右反/全连接，就可以不构建               
				 * 哈希表而退出。但对内连接而言，只有当  
				 * 外层关系的启动成本低于构建哈希表的        
				 * 预估成本时才有利，可进行检查。           
				 * 否则最好先构建哈希表，再看                  
				 * （若为左连接，则                                   
				 * 都应做这项检查，因为无论如何也无法       
				 * 凭内关系为空这一理由跳过该                
				 * 连接。）     
				 *
				 * 如果在重扫该连接，则会利用上次扫描      
				 * 得到的信息：不必再去尝试预取，       
				 * 如果上次扫描发现外层关系非空。         
				 * 这并非 100% 可靠，因为新参数下        
				 * 外层关系可能给出不同结果，但这是   
				 * 不错的启发式方法。        
				 *
				 * 做检查的唯一办法是尝试从外层计划节点   
				 * 取元组。如果取到了，就得把它暂存起来， 
				 * 供 ExecHashJoinOuterGetTuple 稍后取用。              
				 */
				if (HJ_FILL_INNER(node))
				{
					/* 没有机会不构建哈希表        */
					node->hj_FirstOuterTupleSlot = NULL;
				}
				else if (parallel)
				{
					/*
					 * 共享哈希表没有实现空外层优化，      
					 * 参与者都无法断定外层无元组，而且  
					 * 还不清楚为弄清这点而达成共识的         
					 * 同步开销是否值得。                           
					 * 所以必须构建                                   
					 * 哈希表。            
					 */
					node->hj_FirstOuterTupleSlot = NULL;
				}
				else if (HJ_FILL_OUTER(node) ||
						 (outerNode->plan->startup_cost < hashNode->ps.plan->total_cost &&
						  !node->hj_OuterNotEmpty))
				{
					node->hj_FirstOuterTupleSlot = ExecProcNode(outerNode);
					if (TupIsNull(node->hj_FirstOuterTupleSlot))
					{
						node->hj_OuterNotEmpty = false;
						return NULL;
					}
					else
						node->hj_OuterNotEmpty = true;
				}
				else
					node->hj_FirstOuterTupleSlot = NULL;

				/*
				 * 创建哈希表。若使用 Parallel Hash，那么  
				 * 最先到达这里的进程会创建哈希表，而任何 
				 * 后到者只是加入它。             
				 */
				hashtable = ExecHashTableCreate(hashNode,
												node->hj_HashOperators,
												node->hj_Collations,
												HJ_FILL_INNER(node));
				node->hj_HashTable = hashtable;

				/*
				 * 执行 Hash 节点以构建哈希表。若使用         
				 * Parallel Hash，我们会尽力帮忙做哈希，除非
				 * 来得太晚。  
				 */
				hashNode->hashtable = hashtable;
				(void) MultiExecProcNode((PlanState *) hashNode);

				/*
				 * 如果内关系完全为空，而且我们做的不是  
				 * 左外连接，就可以不扫描外层关系            
				 * 直接退出。
				 */
				if (hashtable->totalTuples == 0 && !HJ_FILL_OUTER(node))
				{
					if (parallel)
					{
						/*
						 * 先把构建屏障推到 PHJ_BUILD_RUN，以便  
						 * 协商资源清理事宜。                     
						 */
						Barrier    *build_barrier = &parallel_state->build_barrier;

						while (BarrierPhase(build_barrier) < PHJ_BUILD_RUN)
							BarrierArriveAndWait(build_barrier, 0);
					}
					return NULL;
				}

				/*
				 * 需要记住 nbatch 自我们开始扫描外层关系 
				 * 以来是否有所增加         
				 */
				hashtable->nbatch_outstart = hashtable->nbatch;

				/*
				 * 为扫描重置 OuterNotEmpty。（即使上面已     
				 * 取到元组也无妨，ExecHashJoinOuterGetTuple  
				 * 会立刻再次置位）  
				 */
				node->hj_OuterNotEmpty = false;

				if (parallel)
				{
					Barrier    *build_barrier;

					build_barrier = &parallel_state->build_barrier;
					Assert(BarrierPhase(build_barrier) == PHJ_BUILD_HASH_OUTER ||
						   BarrierPhase(build_barrier) == PHJ_BUILD_RUN ||
						   BarrierPhase(build_barrier) == PHJ_BUILD_FREE);
					if (BarrierPhase(build_barrier) == PHJ_BUILD_HASH_OUTER)
					{
						/*
						 * 若是多批次，需要预先对外层关系     
						 * 做哈希
						 */
						if (hashtable->nbatch > 1)
							ExecParallelHashJoinPartitionOuter(node);
						BarrierArriveAndWait(build_barrier,
											 WAIT_EVENT_HASH_BUILD_HASH_OUTER);
					}
					else if (BarrierPhase(build_barrier) == PHJ_BUILD_FREE)
					{
						/*
						 * 如果加入得太晚，作业已完成且批次   
						 * 状态已释放，就可以立即            
						 * 返回。   
						 */
						return NULL;
					}

					/* 现在每个后端应选好要处理的批次      */
					Assert(BarrierPhase(build_barrier) == PHJ_BUILD_RUN);
					hashtable->curbatch = -1;
					node->hj_JoinState = HJ_NEED_NEW_BATCH;

					continue;
				}
				else
					node->hj_JoinState = HJ_NEED_NEW_OUTER;

				/* FALL THRU */

			case HJ_NEED_NEW_OUTER:

				/*
				 * 我们没有外层元组了，尝试取下一个     
				 */
				if (parallel)
					outerTupleSlot =
						ExecParallelHashJoinOuterGetTuple(outerNode, node,
														  &hashvalue);
				else
					outerTupleSlot =
						ExecHashJoinOuterGetTuple(outerNode, node, &hashvalue);

				if (TupIsNull(outerTupleSlot))
				{
					/* 本批或整个 join 结束       */
					if (HJ_FILL_INNER(node))
					{
						/* 准备扫描未匹配的内侧元组      */
						if (parallel)
						{
							/*
							 * 并行 join 中，只允许一个进程处理
							 * 各批次的未匹配元组，这是并行  
							 * join 
							 */
							if (ExecParallelPrepHashTableForUnmatched(node))
								node->hj_JoinState = HJ_FILL_INNER_TUPLES;
							else
								node->hj_JoinState = HJ_NEED_NEW_BATCH;
						}
						else
						{
							ExecPrepHashTableForUnmatched(node);
							node->hj_JoinState = HJ_FILL_INNER_TUPLES;
						}
					}
					else
						node->hj_JoinState = HJ_NEED_NEW_BATCH;
					continue;
				}

				econtext->ecxt_outertuple = outerTupleSlot;
				node->hj_MatchedOuter = false;

				/*
				 * 在主哈希表或偏斜哈希表中找到该元组     
				 * 对应的桶。               
				 */
				node->hj_CurHashValue = hashvalue;
				ExecHashGetBucketAndBatch(hashtable, hashvalue,
										  &node->hj_CurBucketNo, &batchno);
				node->hj_CurSkewBucketNo = ExecHashGetSkewBucket(hashtable,
																 hashvalue);
				node->hj_CurTuple = NULL;

				/*
				 * 该元组可能不属于当前批次（这里的      
				 * "当前批次"包括偏斜桶，若有）。      
				 */
				if (batchno != hashtable->curbatch &&
					node->hj_CurSkewBucketNo == INVALID_SKEW_BUCKET_NO)
				{
					bool		shouldFree;
					MinimalTuple mintuple = ExecFetchSlotMinimalTuple(outerTupleSlot,
																	  &shouldFree);

					/*
					 * 需要把该外层元组推迟到后续批次。   
					 * 存入相应的外层批次文件。          
					 */
					Assert(parallel_state == NULL);
					Assert(batchno > hashtable->curbatch);
					ExecHashJoinSaveTuple(mintuple, hashvalue,
										  &hashtable->outerBatchFile[batchno],
										  hashtable);

					if (shouldFree)
						heap_free_minimal_tuple(mintuple);

					/* 绕回循环，保持 HJ_NEED_NEW_OUTER 状态  */
					continue;
				}

				/* 好，扫描该桶找匹配           */
				node->hj_JoinState = HJ_SCAN_BUCKET;

				/* FALL THRU */

			case HJ_SCAN_BUCKET:

				/*
				 * 在选定哈希桶中扫描与当前外层匹配的项    
				 */
				if (parallel)
				{
					if (!ExecParallelScanHashBucket(node, econtext))
					{
						/* 匹配已用尽；检查是否需要外连接填充*/
						node->hj_JoinState = HJ_FILL_OUTER_TUPLE;
						continue;
					}
				}
				else
				{
					if (!ExecScanHashBucket(node, econtext))
					{
						/* 匹配已用尽；检查是否需要外连接填充*/
						node->hj_JoinState = HJ_FILL_OUTER_TUPLE;
						continue;
					}
				}

				/*
				 * 找到一个匹配，但仍需测试非哈希条件。     
				 * ExecScanHashBucket 已设置好调用 ExecQual           
				 * 所需状态  
				 *
				 * 如果通过条件，就保存状态供下次调用，让  
				 * ExecProject 做投影，把结果存入元组         
				 * 表中，并返回该槽。
				 *
				 * 只有 joinquals 决定元组匹配状态，但要真正
				 * 返回元组则必须通过所有条件。   
				 */
				if (joinqual == NULL || ExecQual(joinqual, econtext))
				{
					node->hj_MatchedOuter = true;


					/*
					 * 其实只在 HJ_FILL_INNER(node) 时才需要，      
					 * 但为免分支我们总是设置它。       
					 */
					if (!HeapTupleHeaderHasMatch(HJTUPLE_MINTUPLE(node->hj_CurTuple)))
						HeapTupleHeaderSetMatch(HJTUPLE_MINTUPLE(node->hj_CurTuple));

					/* 在反连接中，我们绝不返回匹配元组*/
					if (node->js.jointype == JOIN_ANTI)
					{
						node->hj_JoinState = HJ_NEED_NEW_OUTER;
						continue;
					}

					/*
					 * 若只需考虑第一条匹配的内侧元组，    
					 * 那么处理完后就前进到下一条            
					 * 外层元组。    
					 */
					if (node->js.single_match)
						node->hj_JoinState = HJ_NEED_NEW_OUTER;

					/*
					 * 右反连接中，我们绝不返回匹配元组。  
					 * 若不是 inner_unique 连接，就需要留在     
					 * 当前外层元组，继续扫描内                  
					 * 侧找匹配。  
					 */
					if (node->js.jointype == JOIN_RIGHT_ANTI)
						continue;

					if (otherqual == NULL || ExecQual(otherqual, econtext))
						return ExecProject(node->js.ps.ps_ProjInfo);
					else
						InstrCountFiltered2(node, 1);
				}
				else
					InstrCountFiltered1(node, 1);
				break;

			case HJ_FILL_OUTER_TUPLE:

				/*
				 * 当前外层元组的匹配已用尽，因此要检查  
				 * 是否输出虚拟外连接元组。无论是否输出， 
				 * 下一个状态都是 NEED_NEW_OUTER。      
				 */
				node->hj_JoinState = HJ_NEED_NEW_OUTER;

				if (!node->hj_MatchedOuter &&
					HJ_FILL_OUTER(node))
				{
					/*
					 * 生成内元组为空值的伪连接元组，      
					 * 如果它通过非连接条件则返回。           
					 */
					econtext->ecxt_innertuple = node->hj_NullInnerTupleSlot;

					if (otherqual == NULL || ExecQual(otherqual, econtext))
						return ExecProject(node->js.ps.ps_ProjInfo);
					else
						InstrCountFiltered2(node, 1);
				}
				break;

			case HJ_FILL_INNER_TUPLES:

				/*
				 * 我们完成一个批次，但当前是   
				 * 右/右反/全连接，所以哈希表中所有未匹配 
				 * 内侧元组必须先输出完毕，                     
				 * 才能继续。
				 */
				if (!(parallel ? ExecParallelScanHashTableForUnmatched(node, econtext)
					  : ExecScanHashTableForUnmatched(node, econtext)))
				{
					/* 已无未匹配元组    */
					node->hj_JoinState = HJ_NEED_NEW_BATCH;
					continue;
				}

				/*
				 * 生成一个外元组为空值的伪连接元组，       
				 * 如果它通过非连接条件则返回。    
				 */
				econtext->ecxt_outertuple = node->hj_NullOuterTupleSlot;

				if (otherqual == NULL || ExecQual(otherqual, econtext))
					return ExecProject(node->js.ps.ps_ProjInfo);
				else
					InstrCountFiltered2(node, 1);
				break;

			case HJ_NEED_NEW_BATCH:

				/*
				 * 尝试前进到下一批次。无更多批次则完成。
				 */
				if (parallel)
				{
					if (!ExecParallelHashJoinNewBatch(node))
						return NULL;	/* 并行感知 join 结束   */
				}
				else
				{
					if (!ExecHashJoinNewBatch(node))
						return NULL;	/* 并行无关 join 结束       */
				}
				node->hj_JoinState = HJ_NEED_NEW_OUTER;
				break;

			default:
				elog(ERROR, "unrecognized hashjoin state: %d",
					 (int) node->hj_JoinState);
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecHashJoin
 *
 *		并行无关版本。      
 * ----------------------------------------------------------------
 */
static TupleTableSlot *			/* 返回：元组或 NULL */
ExecHashJoin(PlanState *pstate)
{
	/*
	 * 在足够聪明的编译器上，这应内联，且并行感知
	 * 的分支已被去掉。        
	 */
	return ExecHashJoinImpl(pstate, false);
}

/* ----------------------------------------------------------------
 *		ExecParallelHashJoin
 *
 *		并行感知版本     
 * ----------------------------------------------------------------
 */
static TupleTableSlot *			/* 返回：元组或 NULL */
ExecParallelHashJoin(PlanState *pstate)
{
	/*
	 * 在足够聪明的编译器上，这应内联，且并行无关
	 * 的分支已被去掉。            
	 */
	return ExecHashJoinImpl(pstate, true);
}

/* ----------------------------------------------------------------
 *		ExecInitHashJoin
 *
 *		HashJoin 初始化例程。    
 * ----------------------------------------------------------------
 */
HashJoinState *
ExecInitHashJoin(HashJoin *node, EState *estate, int eflags)
{
	HashJoinState *hjstate;
	Plan	   *outerNode;
	Hash	   *hashNode;
	TupleDesc	outerDesc,
				innerDesc;
	const TupleTableSlotOps *ops;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 创建状态结构    
	 */
	hjstate = makeNode(HashJoinState);
	hjstate->js.ps.plan = (Plan *) node;
	hjstate->js.ps.state = estate;

	/*
	 * 参见 ExecHashJoinInitializeDSM() 和                            
	 * ExecHashJoinInitializeWorker()，若并行查询启动成功，   
	 * 函数将被并行版本替换     
	 */
	hjstate->js.ps.ExecProcNode = ExecHashJoin;
	hjstate->js.jointype = node->join.jointype;

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &hjstate->js.ps);

	/*
	 * 初始化子节点    
	 *
	 * 注意：我们可以抑制内侧输入的 REWIND 标志，这    
	 * 等于押注 hash 只会有单个批次。不清楚这样         
	 * 做是否有利。                  
	 */
	outerNode = outerPlan(node);
	hashNode = (Hash *) innerPlan(node);

	outerPlanState(hjstate) = ExecInitNode(outerNode, estate, eflags);
	outerDesc = ExecGetResultType(outerPlanState(hjstate));
	innerPlanState(hjstate) = ExecInitNode((Plan *) hashNode, estate, eflags);
	innerDesc = ExecGetResultType(innerPlanState(hjstate));

	/*
	 * 初始化结果槽、类型和投影。     
	 */
	ExecInitResultTupleSlotTL(&hjstate->js.ps, &TTSOpsVirtual);
	ExecAssignProjectionInfo(&hjstate->js.ps, NULL);

	/*
	 * 元组表初始化        
	 */
	ops = ExecGetResultSlotOps(outerPlanState(hjstate), NULL);
	hjstate->hj_OuterTupleSlot = ExecInitExtraTupleSlot(estate, outerDesc,
														ops);

	/*
	 * 检测我们是否只需考虑第一条匹配的内侧元组       
	 */
	hjstate->js.single_match = (node->join.inner_unique ||
								node->join.jointype == JOIN_SEMI);

	/* 如需要，为外连接设置空元组       */
	switch (node->join.jointype)
	{
		case JOIN_INNER:
		case JOIN_SEMI:
			break;
		case JOIN_LEFT:
		case JOIN_ANTI:
			hjstate->hj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate, innerDesc, &TTSOpsVirtual);
			break;
		case JOIN_RIGHT:
		case JOIN_RIGHT_ANTI:
			hjstate->hj_NullOuterTupleSlot =
				ExecInitNullTupleSlot(estate, outerDesc, &TTSOpsVirtual);
			break;
		case JOIN_FULL:
			hjstate->hj_NullOuterTupleSlot =
				ExecInitNullTupleSlot(estate, outerDesc, &TTSOpsVirtual);
			hjstate->hj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate, innerDesc, &TTSOpsVirtual);
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) node->join.jointype);
	}

	/*
	 * 接下来来点魔法。我们的临时元组槽实际是            
	 * Hash 节点的结果元组槽（即我们的内层计划）。        
	 * 可以这样做，因为 Hash 节点不通过 ExecProcNode() 返    
	 * 回元组 -- 而是 hash join 节点用 ExecScanHashBucket() 来      
	 * 访问哈希表 -cim 6/9/91 
	 */
	{
		HashState  *hashstate = (HashState *) innerPlanState(hjstate);
		TupleTableSlot *slot = hashstate->ps.ps_ResultTupleSlot;

		hjstate->hj_HashTupleSlot = slot;
	}

	/*
	 * 初始化子表达式       
	 */
	hjstate->js.ps.qual =
		ExecInitQual(node->join.plan.qual, (PlanState *) hjstate);
	hjstate->js.joinqual =
		ExecInitQual(node->join.joinqual, (PlanState *) hjstate);
	hjstate->hashclauses =
		ExecInitQual(node->hashclauses, (PlanState *) hjstate);

	/*
	 * 初始化 hash 专属信息  
	 */
	hjstate->hj_HashTable = NULL;
	hjstate->hj_FirstOuterTupleSlot = NULL;

	hjstate->hj_CurHashValue = 0;
	hjstate->hj_CurBucketNo = 0;
	hjstate->hj_CurSkewBucketNo = INVALID_SKEW_BUCKET_NO;
	hjstate->hj_CurTuple = NULL;

	hjstate->hj_OuterHashKeys = ExecInitExprList(node->hashkeys,
												 (PlanState *) hjstate);
	hjstate->hj_HashOperators = node->hashoperators;
	hjstate->hj_Collations = node->hashcollations;

	hjstate->hj_JoinState = HJ_BUILD_HASHTABLE;
	hjstate->hj_MatchedOuter = false;
	hjstate->hj_OuterNotEmpty = false;

	return hjstate;
}

/* ----------------------------------------------------------------
 *		ExecEndHashJoin
 *
 *		HashJoin 节点的清理例程    
 * ----------------------------------------------------------------
 */
void
ExecEndHashJoin(HashJoinState *node)
{
	/*
	 * 释放哈希表
	 */
	if (node->hj_HashTable)
	{
		ExecHashTableDestroy(node->hj_HashTable);
		node->hj_HashTable = NULL;
	}

	/*
	 * 释放 exprcontext  
	 */
	ExecFreeExprContext(&node->js.ps);

	/*
	 * 清空元组表          
	 */
	ExecClearTuple(node->js.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->hj_OuterTupleSlot);
	ExecClearTuple(node->hj_HashTupleSlot);

	/*
	 * 清理子树     
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));
}

/*
 * ExecHashJoinOuterGetTuple
 *
 *		为并行无关 hashjoin 获取下一条外层元组：第一遍     
 *		通过执行外层计划节点取得，或从 hashjoin 批次的 
 *		临时文件中取得。       
 *
 * 若（当前批次内）没有更多外层元组，则返回空槽。  
 *
 * 成功时，元组的哈希值存入 *hashvalue -- 该值要么       
 * 要么最初算出，要么从临时文件重读。       
 */
static TupleTableSlot *
ExecHashJoinOuterGetTuple(PlanState *outerNode,
						  HashJoinState *hjstate,
						  uint32 *hashvalue)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			curbatch = hashtable->curbatch;
	TupleTableSlot *slot;

	if (curbatch == 0)			/* 如果是第一遍扫描*/
	{
		/*
		 * 检查第一条外层元组是否已被 ExecHashJoin()  
		 * 取走但还没被使用。     
		 */
		slot = hjstate->hj_FirstOuterTupleSlot;
		if (!TupIsNull(slot))
			hjstate->hj_FirstOuterTupleSlot = NULL;
		else
			slot = ExecProcNode(outerNode);

		while (!TupIsNull(slot))
		{
			/*
			 * 我们必须计算该元组的哈希值。
			 */
			ExprContext *econtext = hjstate->js.ps.ps_ExprContext;

			econtext->ecxt_outertuple = slot;
			if (ExecHashGetHashValue(hashtable, econtext,
									 hjstate->hj_OuterHashKeys,
									 true,	/* outer tuple */
									 HJ_FILL_OUTER(hjstate),
									 hashvalue))
			{
				/* 记住外层关系非空，以备可能的重扫         */
				hjstate->hj_OuterNotEmpty = true;

				return slot;
			}

			/*
			 * 该元组因含有 NULL 而无法匹配，因此丢弃它并  
			 * 继续处理下一个。   
			 */
			slot = ExecProcNode(outerNode);
		}
	}
	else if (curbatch < hashtable->nbatch)
	{
		BufFile    *file = hashtable->outerBatchFile[curbatch];

		/*
		 * 在外连接情形下，即使批次文件为空也可能        
		 * 到此。
		 */
		if (file == NULL)
			return NULL;

		slot = ExecHashJoinGetSavedTuple(hjstate,
										 file,
										 hashvalue,
										 hjstate->hj_OuterTupleSlot);
		if (!TupIsNull(slot))
			return slot;
	}

	/* 本批次结束   */
	return NULL;
}

/*
 * 并行情形用的 ExecHashJoinOuterGetTuple 变体     
 */
static TupleTableSlot *
ExecParallelHashJoinOuterGetTuple(PlanState *outerNode,
								  HashJoinState *hjstate,
								  uint32 *hashvalue)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			curbatch = hashtable->curbatch;
	TupleTableSlot *slot;

	/*
	 * 在 Parallel Hash 情形下，我们只对单批次 hash join    
	 * 直接运行外层计划；其他情况必须使用批次文件，    
	 * 含 0 号。
	 */
	if (curbatch == 0 && hashtable->nbatch == 1)
	{
		slot = ExecProcNode(outerNode);

		while (!TupIsNull(slot))
		{
			ExprContext *econtext = hjstate->js.ps.ps_ExprContext;

			econtext->ecxt_outertuple = slot;
			if (ExecHashGetHashValue(hashtable, econtext,
									 hjstate->hj_OuterHashKeys,
									 true,	/* outer tuple */
									 HJ_FILL_OUTER(hjstate),
									 hashvalue))
				return slot;

			/*
			 * 该元组因含有 NULL 而无法匹配，因此丢弃它并  
			 * 继续处理下一个。   
			 */
			slot = ExecProcNode(outerNode);
		}
	}
	else if (curbatch < hashtable->nbatch)
	{
		MinimalTuple tuple;

		tuple = sts_parallel_scan_next(hashtable->batches[curbatch].outer_tuples,
									   hashvalue);
		if (tuple != NULL)
		{
			ExecForceStoreMinimalTuple(tuple,
									   hjstate->hj_OuterTupleSlot,
									   false);
			slot = hjstate->hj_OuterTupleSlot;
			return slot;
		}
		else
			ExecClearTuple(hjstate->hj_OuterTupleSlot);
	}

	/* 本批次结束   */
	hashtable->batches[curbatch].outer_eof = true;

	return NULL;
}

/*
 * ExecHashJoinNewBatch
 *		切换到新 hashjoin 批次  
 *
 * 成功返回 true，没有更多批次则返回 false。       
 */
static bool
ExecHashJoinNewBatch(HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			nbatch;
	int			curbatch;
	BufFile    *innerFile;
	TupleTableSlot *slot;
	uint32		hashvalue;

	nbatch = hashtable->nbatch;
	curbatch = hashtable->curbatch;

	if (curbatch > 0)
	{
		/*
		 * 我们不再需要先前的外层批次文件；立即关闭   
		 * 以释放磁盘空间。
		 */
		if (hashtable->outerBatchFile[curbatch])
			BufFileClose(hashtable->outerBatchFile[curbatch]);
		hashtable->outerBatchFile[curbatch] = NULL;
	}
	else						/* 我们刚完成第一个批次   */
	{
		/*
		 * 重置部分偏斜优化状态变量，因为第一批后       
		 * 不再需要考虑偏斜元组。我们即将进行的内     
		 * 存上下文重置会释放偏斜哈希                      
		 * 表本身。     
		 */
		hashtable->skewEnabled = false;
		hashtable->skewBucket = NULL;
		hashtable->skewBucketNums = NULL;
		hashtable->nSkewBuckets = 0;
		hashtable->spaceUsedSkew = 0;
	}

	/*
	 * 两侧完全为空的批次总是可以跳过。有时也可         
	 * 以跳过只在一侧为空的批次，                              
	 * 但存在这些例外：       
	 *
	 * 1. 左/全外连接中，即使内批次为空也必须处理         
	 * 外层批次。同样，在右/右反/全外连接中，即使外     
	 * 层批次为空也须处理内层批                             
	 * 次   
	 *
	 * 2. 如果自最初估算以来 nbatch 已经增加，就必须扫    
	 * 描内批次，因为其中可能含有需要重新分配到      
	 * 后面内批次的元组。       
	 *
	 * 3. 同样，如果自开始外层扫描以来 nbatch 已有增    
	 * 加，就必须重扫外层批次，以防其中含有需要重       
	 * 新分配的元组。 
	 */
	curbatch++;
	while (curbatch < nbatch &&
		   (hashtable->outerBatchFile[curbatch] == NULL ||
			hashtable->innerBatchFile[curbatch] == NULL))
	{
		if (hashtable->outerBatchFile[curbatch] &&
			HJ_FILL_OUTER(hjstate))
			break;				/* 因规则 1 必须处理   */
		if (hashtable->innerBatchFile[curbatch] &&
			HJ_FILL_INNER(hjstate))
			break;				/* 因规则 1 必须处理   */
		if (hashtable->innerBatchFile[curbatch] &&
			nbatch != hashtable->nbatch_original)
			break;				/* 因规则 2 必须处理   */
		if (hashtable->outerBatchFile[curbatch] &&
			nbatch != hashtable->nbatch_outstart)
			break;				/* 因规则 3 必须处理   */
		/* 可以忽略该批次。  */
		/* 立即释放关联的临时文件。      */
		if (hashtable->innerBatchFile[curbatch])
			BufFileClose(hashtable->innerBatchFile[curbatch]);
		hashtable->innerBatchFile[curbatch] = NULL;
		if (hashtable->outerBatchFile[curbatch])
			BufFileClose(hashtable->outerBatchFile[curbatch]);
		hashtable->outerBatchFile[curbatch] = NULL;
		curbatch++;
	}

	if (curbatch >= nbatch)
		return false;			/* 无更多批次 */

	hashtable->curbatch = curbatch;

	/*
	 * 用新的内批次重新装入哈希表（它可能为空）         
	 */
	ExecHashTableReset(hashtable);

	innerFile = hashtable->innerBatchFile[curbatch];

	if (innerFile != NULL)
	{
		if (BufFileSeek(innerFile, 0, 0, SEEK_SET))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rewind hash-join temporary file")));

		while ((slot = ExecHashJoinGetSavedTuple(hjstate,
												 innerFile,
												 &hashvalue,
												 hjstate->hj_HashTupleSlot)))
		{
			/*
			 * 注意：有些元组可能被送往未来批次。而且， 
			 * hashtable->nbatch 有可能在这里被增加！    
			 */
			ExecHashTableInsert(hashtable, slot, hashvalue);
		}

		/*
		 * 构建好哈希表之后，就不再需要内批次文          
		 * 件   
		 */
		BufFileClose(innerFile);
		hashtable->innerBatchFile[curbatch] = NULL;
	}

	/*
	 * 回卷外层批次文件（如果存在），以便开始读取它。 
	 */
	if (hashtable->outerBatchFile[curbatch] != NULL)
	{
		if (BufFileSeek(hashtable->outerBatchFile[curbatch], 0, 0, SEEK_SET))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rewind hash-join temporary file")));
	}

	return true;
}

/*
 * 选择一个要处理的批次并加入它。成功返回                
 * true，无批次可用则 false。 
 */
static bool
ExecParallelHashJoinNewBatch(HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			start_batchno;
	int			batchno;

	/*
	 * 如果我们已经加入了某个批次，记住不必再检查        
	 * 它，并从它分离（若我们最后分离，可能要释放        
	 * 哈希表）。 
	 */
	if (hashtable->curbatch >= 0)
	{
		hashtable->batches[hashtable->curbatch].done = true;
		ExecHashTableDetachBatch(hashtable);
	}

	/*
	 * 搜索一个尚未完成的批次。我们使用原子计数器，    
	 * 让每个参与者从不同的批次开始搜索，这在批次    
	 * 多于参与者时很有用。 
	 */
	batchno = start_batchno =
		pg_atomic_fetch_add_u32(&hashtable->parallel_state->distributor, 1) %
		hashtable->nbatch;
	do
	{
		uint32		hashvalue;
		MinimalTuple tuple;
		TupleTableSlot *slot;

		if (!hashtable->batches[batchno].done)
		{
			SharedTuplestoreAccessor *inner_tuples;
			Barrier    *batch_barrier =
				&hashtable->batches[batchno].shared->batch_barrier;

			switch (BarrierAttach(batch_barrier))
			{
				case PHJ_BATCH_ELECT:

					/* 由一个后端分配哈希表。     */
					if (BarrierArriveAndWait(batch_barrier,
											 WAIT_EVENT_HASH_BATCH_ELECT))
						ExecParallelHashTableAlloc(hashtable, batchno);
					/* Fall through. */

				case PHJ_BATCH_ALLOCATE:
					/* 等待分配完成。            */
					BarrierArriveAndWait(batch_barrier,
										 WAIT_EVENT_HASH_BATCH_ALLOCATE);
					/* Fall through. */

				case PHJ_BATCH_LOAD:
					/* 开始（或加入）装载元组  */
					ExecParallelHashTableSetCurrentBatch(hashtable, batchno);
					inner_tuples = hashtable->batches[batchno].inner_tuples;
					sts_begin_parallel_scan(inner_tuples);
					while ((tuple = sts_parallel_scan_next(inner_tuples,
														   &hashvalue)))
					{
						ExecForceStoreMinimalTuple(tuple,
												   hjstate->hj_HashTupleSlot,
												   false);
						slot = hjstate->hj_HashTupleSlot;
						ExecParallelHashTableInsertCurrentBatch(hashtable, slot,
																hashvalue);
					}
					sts_end_parallel_scan(inner_tuples);
					BarrierArriveAndWait(batch_barrier,
										 WAIT_EVENT_HASH_BATCH_LOAD);
					/* Fall through. */

				case PHJ_BATCH_PROBE:

					/*
					 * 该批次已可探测。控制权交还调用者
					 * 。我们仍加入 batch_barrier，让哈希表在   
					 * 大家探测完之前仍存活，但参与者   
					 * 都不许再在此屏障等待（否则死锁）。 
					 * 所有已加入的参与者最终都必须与       
					 * 屏障分离，且必须有 worker 推进            
					 * 阶段，以便                                     
					 * 最终阶段得以到达。     
					 */
					ExecParallelHashTableSetCurrentBatch(hashtable, batchno);
					sts_begin_parallel_scan(hashtable->batches[batchno].outer_tuples);

					return true;
				case PHJ_BATCH_SCAN:

					/*
					 * 原则上，我们可以协助扫描未匹配元组，
					 * 该阶段已在进行中（按防死锁规则     
					 * 下我们不能做的事是等待他人到达          
					 * PHJ_BATCH_SCAN，因为 PHJ_BATCH_PROBE        
					 * 会输出元组，但这次我们没等待就         
					 * 到了这里）。这一点尚未实现。目前我们
					 * 只是分离然后再绕回来。我们必须使用 
					 * ExecHashTableDetachBatch()，因为我们有可能    
					 * 是最后分离的，届时须由我们释放     
					 * 内存。                      
					 */
					ExecParallelHashTableSetCurrentBatch(hashtable, batchno);
					hashtable->batches[batchno].done = true;
					ExecHashTableDetachBatch(hashtable);
					break;

				case PHJ_BATCH_FREE:

					/*
					 * 已完成。分离出来再绕回（如果还    
					 * 剩）  
					 */
					BarrierDetach(batch_barrier);
					hashtable->batches[batchno].done = true;
					hashtable->curbatch = -1;
					break;

				default:
					elog(ERROR, "unexpected batch phase %d",
						 BarrierPhase(batch_barrier));
			}
		}
		batchno = (batchno + 1) % hashtable->nbatch;
	} while (batchno != start_batchno);

	return false;
}

/*
 * ExecHashJoinSaveTuple
 *		把元组存到批次文件  
 *
 * 文件中为每个元组记录的数据是其哈希值，      
 * 然后是 MinimalTuple 格式元组   
 *
 * fileptr 指向哈希表数组之一中的某个批次文件。    
 *
 * 批次文件（及其缓冲区）是在为哈希表创建的          
 * 溢出上下文中分配  
 */
void
ExecHashJoinSaveTuple(MinimalTuple tuple, uint32 hashvalue,
					  BufFile **fileptr, HashJoinTable hashtable)
{
	BufFile    *file = *fileptr;

	/*
	 * 批次文件是惰性创建的。如果这是写入该批次           
	 * 的第一条元组，就会创建批次文件，并在 spillCxt     
	 * 上下文中分配，非 batchCxt。      
	 *
	 * 在构建阶段，会为内批次创建缓冲文件。每个批次   
	 * 的缓冲文件在外侧扫描期间该批次装入内存之后      
	 * 就会关闭（其缓冲区被释放）。因此，必须在比      
	 * 批次本身更长寿的内存上下文中分配批次文件缓   
	 * 冲区才行。                 
	 *
	 * 另外，我们使用 spillCxt 而非 hashCxt，以便更好地        
	 * 核算其溢出内存。    
	 */
	if (file == NULL)
	{
		MemoryContext oldctx = MemoryContextSwitchTo(hashtable->spillCxt);

		file = BufFileCreateTemp(false);
		*fileptr = file;

		MemoryContextSwitchTo(oldctx);
	}

	BufFileWrite(file, &hashvalue, sizeof(uint32));
	BufFileWrite(file, tuple, tuple->t_len);
}

/*
 * ExecHashJoinGetSavedTuple
 *		从批次文件读下一条元组，无更多则返回 NULL。 
 *
 * 成功时，*hashvalue 会被设为该元组的哈希值，元组     
 * 本身则存入给定的槽中。  
 */
static TupleTableSlot *
ExecHashJoinGetSavedTuple(HashJoinState *hjstate,
						  BufFile *file,
						  uint32 *hashvalue,
						  TupleTableSlot *tupleSlot)
{
	uint32		header[2];
	size_t		nread;
	MinimalTuple tuple;

	/*
	 * 我们在这里检查中断，因为这里通常是               
	 * ExecProcNode() 调用的替代路径，它包含                    
	 * 此检查。 
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * 由于哈希值和 MinimalTuple 长度字都是 uint32，我们       
	 * 能够在一次 BufFileRead() 调用中读出两者，而无需 
	 * 作假。
	 */
	nread = BufFileReadMaybeEOF(file, header, sizeof(header), true);
	if (nread == 0)				/* 文件结束*/
	{
		ExecClearTuple(tupleSlot);
		return NULL;
	}
	*hashvalue = header[0];
	tuple = (MinimalTuple) palloc(header[1]);
	tuple->t_len = header[1];
	BufFileReadExact(file,
					 (char *) tuple + sizeof(uint32),
					 header[1] - sizeof(uint32));
	ExecForceStoreMinimalTuple(tuple, tupleSlot, true);
	return tupleSlot;
}


void
ExecReScanHashJoin(HashJoinState *node)
{
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);

	/*
	 * 多批次 join 中目前只能用费劲的方式重扫，           
	 * 主要原因是批次临时文件可能已被释放。但如果是    
	 * 单批次 join，而且内层子节点没有参数变化，那么    
	 * 我们就可以直接复用现有哈希表，而无需                
	 * 重建。     
	 */
	if (node->hj_HashTable != NULL)
	{
		if (node->hj_HashTable->nbatch == 1 &&
			innerPlan->chgParam == NULL)
		{
			/*
			 * 可以复用哈希表；也不需要重扫内侧。        
			 *
			 * 不过若是右/右反/全连接，最好重置表中     
			 * 包含的内元组匹配标志。                        
			 */
			if (HJ_FILL_INNER(node))
				ExecHashTableResetMatchFlags(node->hj_HashTable);

			/*
			 * 另外，还需重置关于外层关系是否为空        
			 * 的状态，这样新外层扫描若这次发现外层为    
			 * 空，就能正确地更新它。（现在把它清掉并没有
			 * 坏处，因为 ExecHashJoin 不需要该信息。在其   
			 * 他情况，即哈希表不存在或我们正销毁它       
			 * 的情况下，我们不去动这个状态，因为       
			 * ExecHashJoin 第一次时会需要它。）       
			 */
			node->hj_OuterNotEmpty = false;

			/* ExecHashJoin 可跳过 BUILD_HASHTABLE 步骤  */
			node->hj_JoinState = HJ_NEED_NEW_OUTER;
		}
		else
		{
			/* 必须销毁并重建哈希表      */
			HashState  *hashNode = castNode(HashState, innerPlan);

			Assert(hashNode->hashtable == node->hj_HashTable);
			/* 如需要，从旧哈希表累加统计信息   */
			/* （与 ExecShutdownHash 一致）    */
			if (hashNode->ps.instrument && !hashNode->hinstrument)
				hashNode->hinstrument = (HashInstrumentation *)
					palloc0(sizeof(HashInstrumentation));
			if (hashNode->hinstrument)
				ExecHashAccumInstrumentation(hashNode->hinstrument,
											 hashNode->hashtable);
			/* 为安全起见，务必也清除子计划节点的指针  */
			hashNode->hashtable = NULL;

			ExecHashTableDestroy(node->hj_HashTable);
			node->hj_HashTable = NULL;
			node->hj_JoinState = HJ_BUILD_HASHTABLE;

			/*
			 * 如果子节点 chgParam 非空，计划在首次 ExecProcNode 
			 * 时重扫。          
			 */
			if (innerPlan->chgParam == NULL)
				ExecReScan(innerPlan);
		}
	}

	/* 总是重置元组内状态    */
	node->hj_CurHashValue = 0;
	node->hj_CurBucketNo = 0;
	node->hj_CurSkewBucketNo = INVALID_SKEW_BUCKET_NO;
	node->hj_CurTuple = NULL;

	node->hj_MatchedOuter = false;
	node->hj_FirstOuterTupleSlot = NULL;

	/*
	 * 如果子节点 chgParam 非空，计划在首次 ExecProcNode    
	 * 时重扫。       
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}

void
ExecShutdownHashJoin(HashJoinState *node)
{
	if (node->hj_HashTable)
	{
		/*
		 * 在 DSM 内存消失之前从共享状态分离。这样         
		 * 能确保到 ExecEndHashJoin 运行时，我们不再有任何  
		 * DSM 内存指针。  
		 */
		ExecHashTableDetachBatch(node->hj_HashTable);
		ExecHashTableDetach(node->hj_HashTable);
	}
}

static void
ExecParallelHashJoinPartitionOuter(HashJoinState *hjstate)
{
	PlanState  *outerState = outerPlanState(hjstate);
	ExprContext *econtext = hjstate->js.ps.ps_ExprContext;
	HashJoinTable hashtable = hjstate->hj_HashTable;
	TupleTableSlot *slot;
	uint32		hashvalue;
	int			i;

	Assert(hjstate->hj_FirstOuterTupleSlot == NULL);

	/* 执行外层计划，把全部元组写入共享元组存储  */
	for (;;)
	{
		slot = ExecProcNode(outerState);
		if (TupIsNull(slot))
			break;
		econtext->ecxt_outertuple = slot;
		if (ExecHashGetHashValue(hashtable, econtext,
								 hjstate->hj_OuterHashKeys,
								 true,	/* outer tuple */
								 HJ_FILL_OUTER(hjstate),
								 &hashvalue))
		{
			int			batchno;
			int			bucketno;
			bool		shouldFree;
			MinimalTuple mintup = ExecFetchSlotMinimalTuple(slot, &shouldFree);

			ExecHashGetBucketAndBatch(hashtable, hashvalue, &bucketno,
									  &batchno);
			sts_puttuple(hashtable->batches[batchno].outer_tuples,
						 &hashvalue, mintup);

			if (shouldFree)
				heap_free_minimal_tuple(mintup);
		}
		CHECK_FOR_INTERRUPTS();
	}

	/* 确保所有外层分区都能被任何后端读取。      */
	for (i = 0; i < hashtable->nbatch; ++i)
		sts_end_write(hashtable->batches[i].outer_tuples);
}

void
ExecHashJoinEstimate(HashJoinState *state, ParallelContext *pcxt)
{
	shm_toc_estimate_chunk(&pcxt->estimator, sizeof(ParallelHashJoinState));
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

void
ExecHashJoinInitializeDSM(HashJoinState *state, ParallelContext *pcxt)
{
	int			plan_node_id = state->js.ps.plan->plan_node_id;
	HashState  *hashNode;
	ParallelHashJoinState *pstate;

	/*
	 * 如果创建真正的 DSM 段失败，就禁用共享哈希表  
	 * 模式，因为那意味着我们没有可用的 DSA 区域。         
	 */
	if (pcxt->seg == NULL)
		return;

	ExecSetExecProcNode(&state->js.ps, ExecParallelHashJoin);

	/*
	 * 建立协调共享哈希表访问所需的状态，            
	 * 使用计划节点 ID 作为 toc 键。         
	 */
	pstate = shm_toc_allocate(pcxt->toc, sizeof(ParallelHashJoinState));
	shm_toc_insert(pcxt->toc, plan_node_id, pstate);

	/*
	 * 建立共享 hash join 状态，初始无任何批次。    
	 * ExecHashTableCreate() 会准备至少一个批次并设置 nbatch   
	 * 和 space_allowed 
	 */
	pstate->nbatch = 0;
	pstate->space_allowed = 0;
	pstate->batches = InvalidDsaPointer;
	pstate->old_batches = InvalidDsaPointer;
	pstate->nbuckets = 0;
	pstate->growth = PHJ_GROWTH_OK;
	pstate->chunk_work_queue = InvalidDsaPointer;
	pg_atomic_init_u32(&pstate->distributor, 0);
	pstate->nparticipants = pcxt->nworkers + 1;
	pstate->total_tuples = 0;
	LWLockInitialize(&pstate->lock,
					 LWTRANCHE_PARALLEL_HASH_JOIN);
	BarrierInit(&pstate->build_barrier, 0);
	BarrierInit(&pstate->grow_batches_barrier, 0);
	BarrierInit(&pstate->grow_buckets_barrier, 0);

	/* 设置将用于共享临时文件的空间。          */
	SharedFileSetInit(&pstate->fileset, pcxt->seg);

	/* 初始化 hash 节点中的共享状态。    */
	hashNode = (HashState *) innerPlanState(state);
	hashNode->parallel_state = pstate;
}

/* ----------------------------------------------------------------
 *		ExecHashJoinReInitializeDSM
 *
 *		在开始全新扫描之前重置共享状态。 
 * ----------------------------------------------------------------
 */
void
ExecHashJoinReInitializeDSM(HashJoinState *state, ParallelContext *pcxt)
{
	int			plan_node_id = state->js.ps.plan->plan_node_id;
	ParallelHashJoinState *pstate;

	/* 如果创建 DSM 段失败，则无事可做。     */
	if (pcxt->seg == NULL)
		return;

	pstate = shm_toc_lookup(pcxt->toc, plan_node_id, false);

	/*
	 * 在单批次的情况下，其实可以通过把 build_barrier     
	 * 重置然后快进到 PHJ_BUILD_FREE、把批次 0 的      
	 * batch_barrier 快进到 PHJ_BATCH_PROBE 来复用共享哈        
	 * 希表，但目前共享哈希表早已被释放（由最后      
	 * 一个从批次分离的参与者释放）。我们可以考虑     
	 * 在单批次 join 时保留它。我们还需要调整  
	 * finalize_plan()，让它不为 Parallel Hash 节点记录虚假 
	 * 依赖，以免妨碍重扫优化。对此目前                    
	 * 不尝试 
	 */

	/* 分离，释放任何剩余的共享内存。*/
	if (state->hj_HashTable != NULL)
	{
		ExecHashTableDetachBatch(state->hj_HashTable);
		ExecHashTableDetach(state->hj_HashTable);
	}

	/* 清除所有共享批次文件*/
	SharedFileSetDeleteAll(&pstate->fileset);

	/* 把 build_barrier 重置为 PHJ_BUILD_ELECT，好再次循环。  */
	BarrierInit(&pstate->build_barrier, 0);
}

void
ExecHashJoinInitializeWorker(HashJoinState *state,
							 ParallelWorkerContext *pwcxt)
{
	HashState  *hashNode;
	int			plan_node_id = state->js.ps.plan->plan_node_id;
	ParallelHashJoinState *pstate =
		shm_toc_lookup(pwcxt->toc, plan_node_id, false);

	/* 加入共享临时文件的空间。            */
	SharedFileSetAttach(&pstate->fileset, pwcxt->seg);

	/* 加入 hash 节点中的共享状态。      */
	hashNode = (HashState *) innerPlanState(state);
	hashNode->parallel_state = pstate;

	ExecSetExecProcNode(&state->js.ps, ExecParallelHashJoin);
}
