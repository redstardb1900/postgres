/*-------------------------------------------------------------------------
 *
 * nodeIncrementalSort.c
 *	  处理关系增量排序的例程。                
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeIncrementalSort.c
 *
 * DESCRIPTION
 *
 *	增量排序是多键排序的优化变体，适用于输入       
 *	已按部分排序键前缀排好序的情形。例如当请求按 
 *	(key1, key2 ... keyN) 排序，而输入已按 (key1, key2        
 *	) 已排好序（M < N）时，我们可以把输入划分为  
 *	若干个 (key1, ... keyM) 相等的组，并且只对剩余       
 *	的列进行排序。                  
 *
 *	考虑下面的例子。输入元组由两个整数 (X, Y)         
 *	已预先按 X 排好序，但要求同时按 X 与 Y 排序。   
 *	设输入元组如下。                                  
 *
 *	(1, 5)
 *	(1, 2)
 *	(2, 9)
 *	(2, 1)
 *	(2, 5)
 *	(3, 3)
 *	(3, 7)
 *
 *	增量排序算法会把输入切成下列各 X 相等的组，       
 *	然后再分别按 Y 对每组进行排序：                    
 *
 *		(1, 5) (1, 2)
 *		(2, 9) (2, 1) (2, 5)
 *		(3, 3) (3, 7)
 *
 *	把这些组排序并合并到一起后，将得到如下结果，  
 *	即所要求的按 X 和 Y 排序的结果：                 
 *
 *	(1, 2)
 *	(1, 5)
 *	(2, 1)
 *	(2, 5)
 *	(2, 9)
 *	(3, 3)
 *	(3, 7)
 *
 *	在大数据集上，增量排序可能比普通排序更高效，  
 *	因为它减少了单次排序的数据量，使其更可能装进  
 *	work_mem（无需溢出到磁盘）。但增量排序的主要      
 *	优势是它可以在对整个数据集排序完成之前          
 *	就开始输出行，这对带 LIMIT 的查询                      
 *	尤其有益。                                                  
 *
 *	这里实现的算法是对上述理论基础的改造，            
 *	它以两种不同的模式运作：                     
 *	  - 取回最少数量的元组，不检查前缀键组归属，   
 *	    安全时对所有列排序。                        
 *	  - 取回单个前缀键组的全部元组，                      
 *	    只对未排序列排序。 
 *	我们总是从第一种模式开始，并在认为有益时       
 *	借助启发式规则切换到第二种模式。   
 *
 *	增量排序可能用更少内存，避免取回和排序数据集中
 *	全部元组，并在整个结果集就绪前开始                     
 *	返回元组。                    
 *
 *	混合模式方法让我们能同时优化极小的组            
 *	（此时新 tuplesort 的开销很高）和极大的组             
 *	（此时不必对已排序的列再排序，从而降低成本），  
 *	尽管在两种模式间切换时会有一些额外成本。      
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "executor/execdebug.h"
#include "executor/nodeIncrementalSort.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/tuplesort.h"

/*
 * 我们需要把 instrumentation 信息存到本地节点的                 
 * sort 信息中，或者对并行 worker 进程存到共享             
 * 信息中（这免去了每次 instrumentation 调用时                 
 * 额外的 memcpy 拷贝）。此宏展开后会选出正确的 sort        
 * 状态和组信息。
 *
 * 参数： 
 * - node：IncrementalSortState *    
 * - groupName：词元 fullsort 或 prefixsort 
 */
#define INSTRUMENT_SORT_GROUP(node, groupName) \
	do { \
		if ((node)->ss.ps.instrument != NULL) \
		{ \
			if ((node)->shared_info && (node)->am_worker) \
			{ \
				Assert(IsParallelWorker()); \
				Assert(ParallelWorkerNumber <= (node)->shared_info->num_workers); \
				instrumentSortedGroup(&(node)->shared_info->sinfo[ParallelWorkerNumber].groupName##GroupInfo, \
									  (node)->groupName##_state); \
			} \
			else \
			{ \
				instrumentSortedGroup(&(node)->incsort_info.groupName##GroupInfo, \
									  (node)->groupName##_state); \
			} \
		} \
	} while (0)


/* ----------------------------------------------------------------
 * instrumentSortedGroup
 *
 * 由于增量排序会处理（可能许多个）排序批次，我们在   
 * 结束一个排序状态时都捕获 tuplesort 统计数据，这些       
 * 摘要数据供 EXPLAIN ANALYZE 使用。     
 * ----------------------------------------------------------------
 */
static void
instrumentSortedGroup(IncrementalSortGroupInfo *groupInfo,
					  Tuplesortstate *sortState)
{
	TuplesortInstrumentation sort_instr;

	groupInfo->groupCount++;

	tuplesort_get_stats(sortState, &sort_instr);

	/* 计算总内存、最大内存和磁盘空间用量。  */
	switch (sort_instr.spaceType)
	{
		case SORT_SPACE_TYPE_DISK:
			groupInfo->totalDiskSpaceUsed += sort_instr.spaceUsed;
			if (sort_instr.spaceUsed > groupInfo->maxDiskSpaceUsed)
				groupInfo->maxDiskSpaceUsed = sort_instr.spaceUsed;

			break;
		case SORT_SPACE_TYPE_MEMORY:
			groupInfo->totalMemorySpaceUsed += sort_instr.spaceUsed;
			if (sort_instr.spaceUsed > groupInfo->maxMemorySpaceUsed)
				groupInfo->maxMemorySpaceUsed = sort_instr.spaceUsed;

			break;
	}

	/* 记录已使用的排序方法。  */
	groupInfo->sortMethods |= sort_instr.sortMethod;
}

/* ----------------------------------------------------------------
 * preparePresortedCols
 *
 * 为 presorted_keys 比较准备所需信息。     
 * ----------------------------------------------------------------
 */
static void
preparePresortedCols(IncrementalSortState *node)
{
	IncrementalSort *plannode = castNode(IncrementalSort, node->ss.ps.plan);

	node->presorted_keys =
		(PresortedKeyData *) palloc(plannode->nPresortedCols *
									sizeof(PresortedKeyData));

	/* 为每个预排序键预先缓存比较函数。        */
	for (int i = 0; i < plannode->nPresortedCols; i++)
	{
		Oid			equalityOp,
					equalityFunc;
		PresortedKeyData *key;

		key = &node->presorted_keys[i];
		key->attno = plannode->sort.sortColIdx[i];

		equalityOp = get_equality_op_for_ordering_op(plannode->sort.sortOperators[i],
													 NULL);
		if (!OidIsValid(equalityOp))
			elog(ERROR, "missing equality operator for ordering operator %u",
				 plannode->sort.sortOperators[i]);

		equalityFunc = get_opcode(equalityOp);
		if (!OidIsValid(equalityFunc))
			elog(ERROR, "missing function for operator %u", equalityOp);

		/* 查找比较函数             */
		fmgr_info_cxt(equalityFunc, &key->flinfo, CurrentMemoryContext);

		/* 我们可以只初始化一次 callinfo 并复用它。*/
		key->fcinfo = palloc0(SizeForFunctionCallInfo(2));
		InitFunctionCallInfoData(*key->fcinfo, &key->flinfo, 2,
								 plannode->sort.collations[i], NULL, NULL);
		key->fcinfo->args[0].isnull = false;
		key->fcinfo->args[1].isnull = false;
	}
}

/* ----------------------------------------------------------------
 * isCurrentGroup
 *
 * 比较给定元组的预排序列值与当前组的枢轴元组，即可  
 * 判断该元组是否属于当前的排序组。                    
 * ----------------------------------------------------------------
 */
static bool
isCurrentGroup(IncrementalSortState *node, TupleTableSlot *pivot, TupleTableSlot *tuple)
{
	int			nPresortedCols;

	nPresortedCols = castNode(IncrementalSort, node->ss.ps.plan)->nPresortedCols;

	/*
	 * 输入已按键 * (0, ... n) 排好序，这意味着靠后的键  
	 * 更可能发生变化。因此我们从最后一个预排序列开始  
	 * 比较，以便尽早发现不相等，                           
	 * 并尽量减少函数调用次数。                    
	 */
	for (int i = nPresortedCols - 1; i >= 0; i--)
	{
		Datum		datumA,
					datumB,
					result;
		bool		isnullA,
					isnullB;
		AttrNumber	attno = node->presorted_keys[i].attno;
		PresortedKeyData *key;

		datumA = slot_getattr(pivot, attno, &isnullA);
		datumB = slot_getattr(tuple, attno, &isnullB);

		/* NULL 与 NULL 比较的特殊情况，否则用标准比较 */
		if (isnullA || isnullB)
		{
			if (isnullA == isnullB)
				continue;
			else
				return false;
		}

		key = &node->presorted_keys[i];

		key->fcinfo->args[0].value = datumA;
		key->fcinfo->args[1].value = datumB;

		/* 为稳妥起见，每次都重置 isnull            */
		key->fcinfo->isnull = false;

		result = FunctionCallInvoke(key->fcinfo);

		/* 检查结果是否为 NULL，调用者显然不期望空结果   */
		if (key->fcinfo->isnull)
			elog(ERROR, "function %u returned NULL", key->flinfo.fn_oid);

		if (!DatumGetBool(result))
			return false;
	}
	return true;
}

/* ----------------------------------------------------------------
 * switchToPresortedPrefixMode
 *
 * 当我们判定很可能遇到了一大批具有相同预排序前缀值   
 * 的元组时，我们想优化 tuplesort，只对未排序的后缀键   
 * 进行排序。                      
 *
 * 问题在于我们已经把若干元组累积到了另一个 tuplesort 
 * 中，而那个 tuplesort 配置为按所有列排序（因为可能   
 * 存在多个前缀键组）。要进入预排序前缀模式，          
 * 我们得回头检查已累积的所有元组，确认它们都属于  
 * 同一个前缀键组，然后才只用未排序的后缀键对它们   
 * 进行排序。                
 *
 * 虽然很可能已取回的元组都属于同一个前缀组，           
 * 我们还得处理这种可能性：在大前缀键组之前至少还      
 * 存在一个不同的前缀键组。                             
 * ----------------------------------------------------------------
 */
static void
switchToPresortedPrefixMode(PlanState *pstate)
{
	IncrementalSortState *node = castNode(IncrementalSortState, pstate);
	ScanDirection dir;
	int64		nTuples;
	TupleDesc	tupDesc;
	PlanState  *outerNode;
	IncrementalSort *plannode = castNode(IncrementalSort, node->ss.ps.plan);

	dir = node->ss.ps.state->es_direction;
	outerNode = outerPlanState(node);
	tupDesc = ExecGetResultType(outerNode);

	/* 首次执行时配置前缀排序状态。             */
	if (node->prefixsort_state == NULL)
	{
		Tuplesortstate *prefixsort_state;
		int			nPresortedCols = plannode->nPresortedCols;

		/*
		 * 通过假定前缀列全部相等来优化排序，               
		 * 这样只需按其余的列排序。               
		 */
		prefixsort_state = tuplesort_begin_heap(tupDesc,
												plannode->sort.numCols - nPresortedCols,
												&(plannode->sort.sortColIdx[nPresortedCols]),
												&(plannode->sort.sortOperators[nPresortedCols]),
												&(plannode->sort.collations[nPresortedCols]),
												&(plannode->sort.nullsFirst[nPresortedCols]),
												work_mem,
												NULL,
												node->bounded ? TUPLESORT_ALLOWBOUNDED : TUPLESORT_NONE);
		node->prefixsort_state = prefixsort_state;
	}
	else
	{
		/* 下一组预排序数据     */
		tuplesort_reset(node->prefixsort_state);
	}

	/*
	 * 如果当前节点有边界，那么大前缀键组很可能会    
	 * 受益于有界排序，因此配置 tuplesort 允许                  
	 * 这一优化。                          
	 */
	if (node->bounded)
	{
		SO1_printf("Setting bound on presorted prefix tuplesort to: " INT64_FORMAT "\n",
				   node->bound - node->bound_Done);
		tuplesort_set_bound(node->prefixsort_state,
							node->bound - node->bound_Done);
	}

	/*
	 * 从全排序状态向前缀排序状态尽可能多地拷贝元组     
	 * （即同一前缀键组内的元组）。   
	 */
	for (nTuples = 0; nTuples < node->n_fullsort_remaining; nTuples++)
	{
		/*
		 * 当我们在全排序 tuplesort 中遇到多个前缀键组时，
		 * 必须把最后读取的元组结转到下一                    
		 * 批。
		 */
		if (nTuples == 0 && !TupIsNull(node->transfer_tuple))
		{
			tuplesort_puttupleslot(node->prefixsort_state, node->transfer_tuple);
			/* 结转的元组就是我们新的组枢轴元组。  */
			ExecCopySlot(node->group_pivot, node->transfer_tuple);
		}
		else
		{
			tuplesort_gettupleslot(node->fullsort_state,
								   ScanDirectionIsForward(dir),
								   false, node->transfer_tuple, NULL);

			/*
			 * 如果这是第一次经过循环，那么我们需要     
			 * 把得到的第一个元组存为新的组枢轴。
			 */
			if (TupIsNull(node->group_pivot))
				ExecCopySlot(node->group_pivot, node->transfer_tuple);

			if (isCurrentGroup(node, node->group_pivot, node->transfer_tuple))
			{
				tuplesort_puttupleslot(node->prefixsort_state, node->transfer_tuple);
			}
			else
			{
				/*
				 * 该元组不属于当前批次，需要把它结转到 
				 * 下一批从全排序 tuplesort 转入预排序前缀      
				 * tuplesort 的元组中。保存该元组无需      
				 * 特殊操作，因为我们已经把它加载到了        
				 * node->transfer_tuple 槽；而且即使该槽指向  
				 * 的内存位于全排序 tuplesort 内，我们反正
				 * 也要等到把它的元组全部转移出去之后才能
				 * 重置它，所以该引用是安全的。但我们确实
				 * 需要重置组枢轴元组，因为                    
				 * 我们已经完成了当前的前缀键                    
				 * 组。                   
				 */
				ExecClearTuple(node->group_pivot);

				/* 提前跳出 for 循环     */
				break;
			}
		}
	}

	/*
	 * 跟踪全排序批次中还剩多少元组，以便知道是否需要 
	 * 在处理我们认为遇到的大单个前缀键组中剩余的    
	 * 元组之前，先对多个前缀键组                      
	 * 排序。   
	 */
	SO1_printf("Moving " INT64_FORMAT " tuples to presorted prefix tuplesort\n", nTuples);
	node->n_fullsort_remaining -= nTuples;
	SO1_printf("Setting n_fullsort_remaining to " INT64_FORMAT "\n", node->n_fullsort_remaining);

	if (node->n_fullsort_remaining == 0)
	{
		/*
		 * 我们发现全排序批次中剩余的元组都位于同一       
		 * 前缀键组，并且已把所有这些元组移入预排序    
		 * 前缀 tuplesort。我们还不知道当前前缀键组的         
		 * 最后一个元组是否已经出现，因此先保存枢轴 
		 * 比较元组，并继续从外层节点取元组装入      
		 * 预排序前缀 tuplesort。                               
		 */
		ExecCopySlot(node->group_pivot, node->transfer_tuple);
		SO_printf("Setting execution_status to INCSORT_LOADPREFIXSORT (switchToPresortedPrefixMode)\n");
		node->execution_status = INCSORT_LOADPREFIXSORT;

		/*
		 * 务必清空 transfer tuple 槽，这样下次遇到大        
		 * 前缀键组时，不会误认为我们有从上一组结转     
		 * 而来的元组。                                
		 */
		ExecClearTuple(node->transfer_tuple);
	}
	else
	{
		/*
		 * 我们完成了一个组，但并未耗尽全排序状态中     
		 * 的全部元组，所以我们将对该批次排序，让外层   
		 * 节点读出所有这些元组，然后再回来找另            
		 * 批。
		 */
		SO1_printf("Sorting presorted prefix tuplesort with " INT64_FORMAT " tuples\n", nTuples);
		tuplesort_performsort(node->prefixsort_state);

		INSTRUMENT_SORT_GROUP(node, prefixsort);

		if (node->bounded)
		{
			/*
			 * 如果当前节点有边界且已排序 n 个，则剩余 
			 * 的有效边界是（原始边界 - n），故保存已       
			 * 处理的元组数，供配置排序边界                   
			 * 时使用。                 
			 */
			SO2_printf("Changing bound_Done from " INT64_FORMAT " to " INT64_FORMAT "\n",
					   Min(node->bound, node->bound_Done + nTuples), node->bound_Done);
			node->bound_Done = Min(node->bound, node->bound_Done + nTuples);
		}

		SO_printf("Setting execution_status to INCSORT_READPREFIXSORT  (switchToPresortedPrefixMode)\n");
		node->execution_status = INCSORT_READPREFIXSORT;
	}
}

/*
 * 用 tuplesort 对许多小组排序是低效的。为解决这一   
 * 问题，在当前组包含至少 DEFAULT_MIN_GROUP_SIZE 个           
 * 元组之前我们不会开始新组（遗憾的是这也意味着      
 * 我们不能假设小组元组都具有相同的前缀键）。             
 * 当边界小于 DEFAULT_MIN_GROUP_SIZE 时，一旦达到                    
 * 边界我们就会开始寻找新组，以避免取回超过必需     
 * 数量的元组。                      
 */
#define DEFAULT_MIN_GROUP_SIZE 32

/*
 * 虽然我们通过在达到最少元组数之前不开始前缀键比较， 
 * 对小的前缀键组做了优化，但我们不想让这一优化使我     
 * 们失去能够假定大元组组已按其前缀键完全预排序        
 * 所带来的好处。因此我们用 DEFAULT_MAX_FULL_SORT           
 * _GROUP_SIZE 截断值作为启发式标准，用于判定何时            
 * 我们相信自己遇到了大组；并且如果到该点时仍未         
 * 发现新的前缀键组，我们就转入预排序                      
 * 前缀键模式。        
 */
#define DEFAULT_MAX_FULL_SORT_GROUP_SIZE (2 * DEFAULT_MIN_GROUP_SIZE)

/* ----------------------------------------------------------------
 *		ExecIncrementalSort
 *
 *		假定外层子树返回的元组已按目标排序列            
 *		的某前缀预排序，执行增量排序。     
 *
 *		条件：  
 *		  --无。
 *
 *		初始状态：
 *		  -- 外层子节点已准备好返回第一个元组。   
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecIncrementalSort(PlanState *pstate)
{
	IncrementalSortState *node = castNode(IncrementalSortState, pstate);
	EState	   *estate;
	ScanDirection dir;
	Tuplesortstate *read_sortstate;
	Tuplesortstate *fullsort_state;
	TupleTableSlot *slot;
	IncrementalSort *plannode = (IncrementalSort *) node->ss.ps.plan;
	PlanState  *outerNode;
	TupleDesc	tupDesc;
	int64		nTuples = 0;
	int64		minGroupSize;

	CHECK_FOR_INTERRUPTS();

	estate = node->ss.ps.state;
	dir = estate->es_direction;
	fullsort_state = node->fullsort_state;

	/*
	 * 如果上一轮迭代已排好一个批次，那么我们需要     
	 * 先检查该批次中是否还有可返回的元组，然后才能    
	 * 转入其他执行状态。                
	 */
	if (node->execution_status == INCSORT_READFULLSORT
		|| node->execution_status == INCSORT_READPREFIXSORT)
	{
		/*
		 * 如可用，从当前已排序组集返回下一个元组。     
		 */
		read_sortstate = node->execution_status == INCSORT_READFULLSORT ?
			fullsort_state : node->prefixsort_state;
		slot = node->ss.ps.ps_ResultTupleSlot;

		/*
		 * 检查 outerNodeDone 前必须先用 tuplesort 填充槽，    
		 * 因为若无更多元组，它会将该槽置为 NULL。     
		 * 如果 tuplesort 为空，而外层节点也没有更多       
		 * 可用于排序的元组，则outerNodeDone 已置位，
		 * 因此我们会把这个现已为空的槽返回给调         
		 * 用者。          
		 */
		if (tuplesort_gettupleslot(read_sortstate, ScanDirectionIsForward(dir),
								   false, slot, NULL) || node->outerNodeDone)

			/*
			 * 注：对于直接检查 node->outerNodeDone 尚无好的     
			 * 测试用例，但对于任何外层节点会因取回     
			 * 过多元组而失败的计划都需要它。       
			 */
			return slot;
		else if (node->n_fullsort_remaining > 0)
		{
			/*
			 * 转入预排序前缀模式时，全排序 tuplesort 里  
			 * 可能还累积了至少一个额外前缀键组。首次  
			 * switchToPresortedPrefixMode()         
			 * 调用会拉出其中第一个组，并且我们已把         
			 * 这些元组返回给了父节点，但如果此时全排序   
			 * 状态中仍有元组剩余（即 n_fullsort_remaining      
			 *  > 0），就需重新执行前缀模式转换函数，      
			 * 以便拉出                                                  
			 * 下一个前缀键组。      
			 */
			SO1_printf("Re-calling switchToPresortedPrefixMode() because n_fullsort_remaining is > 0 (" INT64_FORMAT ")\n",
					   node->n_fullsort_remaining);
			switchToPresortedPrefixMode(pstate);
		}
		else
		{
			/*
			 * 如果没有已排序元组可读，且当前未处于  
			 * 向预排序前缀模式转换时，那么就该重新开始 
			 * 整个流程：在全排序状态中构建                     
			 * 一个新组。              
			 */
			SO_printf("Setting execution_status to INCSORT_LOADFULLSORT (n_fullsort_remaining > 0)\n");
			node->execution_status = INCSORT_LOADFULLSORT;
		}
	}

	/*
	 * 创建已排序数据的同时按正向方向扫描子计划。    
	 * data.
	 */
	estate->es_direction = ForwardScanDirection;

	outerNode = outerPlanState(node);
	tupDesc = ExecGetResultType(outerNode);

	/* 把元组装入全排序状态。     */
	if (node->execution_status == INCSORT_LOADFULLSORT)
	{
		/*
		 * 初始化排序结构。      
		 */
		if (fullsort_state == NULL)
		{
			/*
			 * 为 isCurrentGroup() 初始化预排序列        
			 * 的支撑结构。它与全排序状态一起初始化  
			 * 是正确的（而非前缀排序状态），因为我们      
			 * 总是先加载全排序状态。                          
			 * first.
			 */
			preparePresortedCols(node);

			/*
			 * 由于我们通过在排序前积累最少数量的元组  
			 * 优化小前缀键组，所以不能假定一组元组都具有
			 * 相同的前缀键值。因此我们配置全排序 tuplesort
			 * 按所有请求的排序键进行排序。                 
			 * keys.
			 */
			fullsort_state = tuplesort_begin_heap(tupDesc,
												  plannode->sort.numCols,
												  plannode->sort.sortColIdx,
												  plannode->sort.sortOperators,
												  plannode->sort.collations,
												  plannode->sort.nullsFirst,
												  work_mem,
												  NULL,
												  node->bounded ?
												  TUPLESORT_ALLOWBOUNDED :
												  TUPLESORT_NONE);
			node->fullsort_state = fullsort_state;
		}
		else
		{
			/* 为下一批重置排序。    */
			tuplesort_reset(fullsort_state);
		}

		/*
		 * 如果有边界，则计算剩余元组数，并相应地配置  
		 * 有界排序和最小组大小这两者。          
		 */
		if (node->bounded)
		{
			int64		currentBound = node->bound - node->bound_Done;

			/*
			 * 有界排序对全排序模式而言不太可能是有效的  
			 * 优化，因为我们把全排序模式限制在相对少量 
			 * 的元组上，而 tuplesort 除非达到 (2 * bound) 个  
			 * 元组时才会切换到 top-n 堆排序。        
			 */
			if (currentBound < DEFAULT_MIN_GROUP_SIZE)
				tuplesort_set_bound(fullsort_state, currentBound);

			minGroupSize = Min(DEFAULT_MIN_GROUP_SIZE, currentBound);
		}
		else
			minGroupSize = DEFAULT_MIN_GROUP_SIZE;

		/*
		 * 因为必须读取下一个元组才能发现遇到了新的 
		 * 前缀键组，所以在后续的组中，我们必须在这里    
		 * 先结转那个多读的元组，把它加入新组的排序，    
		 * 然后才从外层节点读取新元组。        
		 */
		if (!TupIsNull(node->group_pivot))
		{
			tuplesort_puttupleslot(fullsort_state, node->group_pivot);
			nTuples++;

			/*
			 * 我们处于全排序模式，正在积累最少数量的      
			 * 且尚未检查前缀键相等性，所以无法假定   
			 * 组枢轴元组不变，除非所用最小组大小为 1，
			 * 此时枢轴                                                  
			 * 显然仍是枢轴。        
			 */
			if (nTuples != minGroupSize)
				ExecClearTuple(node->group_pivot);
		}


		/*
		 * 在当前运行模式下，从外层节点尽可能多地    
		 * 拉取元组。        
		 */
		for (;;)
		{
			slot = ExecProcNode(outerNode);

			/*
			 * 如果外层节点无法再向我们提供任何元组，那么
			 * 就能对当前组排序并返回元组。     
			 */
			if (TupIsNull(slot))
			{
				/*
				 * 我们稍后需要知道外层节点是否完成，以便  
				 * 区分是完成了一个批次，                      
				 * 还是完成了整个节点。 
				 */
				node->outerNodeDone = true;

				SO1_printf("Sorting fullsort with " INT64_FORMAT " tuples\n", nTuples);
				tuplesort_performsort(fullsort_state);

				INSTRUMENT_SORT_GROUP(node, fullsort);

				SO_printf("Setting execution_status to INCSORT_READFULLSORT (final tuple)\n");
				node->execution_status = INCSORT_READFULLSORT;
				break;
			}

			/* 累积下一组预排序元组。              */
			if (nTuples < minGroupSize)
			{
				/*
				 * 如果尚未达到目标最小组大小，那么无需   
				 * 费心检查是否属于当前前缀组，因为     
				 * 我们将假定对本批做全排序，以避免大量    
				 * 非常微小因而低效的                               
				 * 排序。                         
				 */
				tuplesort_puttupleslot(fullsort_state, slot);
				nTuples++;

				/*
				 * 如果已达到最小组大小，那么需要把        
				 * 最近的元组存为枢轴。         
				 */
				if (nTuples == minGroupSize)
					ExecCopySlot(node->group_pivot, slot);
			}
			else
			{
				/*
				 * 如果我们已经累积了足够多的元组，达到 
				 * 了最小组大小，就需要把任何额外元组与    
				 * 枢轴元组比较，看是否已到该前缀键组    
				 * 的末尾。只有发现前缀键发生变化之后，  
				 * 我们才能保证已累积元组的                    
				 * 排序稳定性。  
				 */
				if (isCurrentGroup(node, node->group_pivot, slot))
				{
					/*
					 * 只要前缀键匹配枢轴元组，就把它装入  
					 * tuplesort。                      
					 */
					tuplesort_puttupleslot(fullsort_state, slot);
					nTuples++;
				}
				else
				{
					/*
					 * 由于取到的元组不属于当前前缀键组， 
					 * 不想把它作为当前批次的一部分排序。 
					 * 我们改用 group_pivot 槽把它结转到下一批  
					 * （尽管                                         
					 * 不会真的把它当作组枢轴）。   
					 */
					ExecCopySlot(node->group_pivot, slot);

					if (node->bounded)
					{
						/*
						 * 若当前节点有边界且已排序 n 个，     
						 * 则剩余边界为（原始边界-n）， 
						 * 保存当前已处理元组数，供配置     
						 * 排序状态的                                 
						 * 边界。                          
						 */
						SO2_printf("Changing bound_Done from " INT64_FORMAT " to " INT64_FORMAT "\n",
								   node->bound_Done,
								   Min(node->bound, node->bound_Done + nTuples));
						node->bound_Done = Min(node->bound, node->bound_Done + nTuples);
					}

					/*
					 * 一旦发现前缀键变化，就能完成排序， 
					 * 并转换模式去读出已排序的元组。      
					 * tuples.
					 */
					SO1_printf("Sorting fullsort tuplesort with " INT64_FORMAT " tuples\n",
							   nTuples);
					tuplesort_performsort(fullsort_state);

					INSTRUMENT_SORT_GROUP(node, fullsort);

					SO_printf("Setting execution_status to INCSORT_READFULLSORT (found end of group)\n");
					node->execution_status = INCSORT_READFULLSORT;
					break;
				}
			}

			/*
			 * 除非我们已转入从全排序状态读取的模式，  
			 * 否则我们就假定：读取的元组数达到至少   
			 * DEFAULT_MAX_FULL_SORT_GROUP_SIZE 个就意味着很            
			 * 可能我们正在处理一个大元组组，其中所有      
			 * 元组的前缀键都相等（但尚未找到该前缀键
			 * 组的最后元组），需转入预排序前缀模式。   
			 */
			if (nTuples > DEFAULT_MAX_FULL_SORT_GROUP_SIZE &&
				node->execution_status != INCSORT_READFULLSORT)
			{
				/*
				 * 已保存的组枢轴已被放入 tuplesort，          
				 * 不想把它结转。虽然尚未找到前缀键组    
				 * 的末尾，看似应当保留它，但我们并不知道  
				 * 全排序状态中可能含有多少个前缀            
				 * 键组，所以就让模式转换函数                    
				 * 替我们管理这一状态即可，                 
				 * 无需我们操心。    
				 */
				ExecClearTuple(node->group_pivot);

				/*
				 * 遗憾的是 tuplesort API 没有提供在排序前     
				 * 取回元组的途径，即使本可轻松依靠 FIFO  
				 * 取回语义将它们转到预排序前缀 tuplesort，  
				 * 也照样执行                                          
				 * 排序操作。            
				 */
				SO1_printf("Sorting fullsort tuplesort with " INT64_FORMAT " tuples\n", nTuples);
				tuplesort_performsort(fullsort_state);

				INSTRUMENT_SORT_GROUP(node, fullsort);

				/*
				 * 若全排序 tuplesort 恰好切到 top-n 堆排序，  
				 * 模式，那么只能取回 currentBound 个元组 
				 * （因为 tuplesort 只保留了前 n 个）。尽管  
				 * 即使尚未取完当前前缀键组，这也是安全 
				 * 的，因为我们已丢失的元组本来就排在被     
				 * 保留元组的后面，而且契约已保证我们绝不 
				 * 会需要多于 currentBound 个                         
				 * 元组。                                         
				 */
				if (tuplesort_used_bound(node->fullsort_state))
				{
					int64		currentBound = node->bound - node->bound_Done;

					SO2_printf("Read " INT64_FORMAT " tuples, but setting to " INT64_FORMAT " because we used bounded sort\n",
							   nTuples, Min(currentBound, nTuples));
					nTuples = Min(currentBound, nTuples);
				}

				SO1_printf("Setting n_fullsort_remaining to " INT64_FORMAT " and calling switchToPresortedPrefixMode()\n",
						   nTuples);

				/*
				 * 全排序状态中可能有多个前缀键组，模式转
				 * 换函数需要知道它必须从全排序转入预排序
				 * 前缀排序。                                       
				 * sort.
				 */
				node->n_fullsort_remaining = nTuples;

				/* 把这些元组转换到预排序前缀 tuplesort。     */
				switchToPresortedPrefixMode(pstate);

				/*
				 * 既然我们知道自己有元组要移到预排序前缀  
				 * tuplesort，就知道：除非该转换已确认所有     
				 * 元组都属于同一个前缀键组（此时            
				 * 可直接继续向该 tuplesort 装元组），否则就  
				 * 应当有一个元组可在这里返回。            
				 * here.
				 *
				 * 无论哪种情况，相应的执行状态都应已由  
				 * switchToPresortedPrefixMode() 设置好，因此我们   
				 * 可以就此退出循环，让相应路径发挥作用。 
				 */
				break;
			}
		}
	}

	if (node->execution_status == INCSORT_LOADPREFIXSORT)
	{
		/*
		 * 只有在模式转换函数确认全排序状态中剩余      
		 * 的所有元组都具有相同前缀，并把它们移入       
		 * 前缀排序状态之后，我们才会进入这一状态。     
		 * 该函数还设置了一个组枢轴元组（它无需结转；    
		 * 它已被放入前缀排序状态）。                         
		 */
		Assert(!TupIsNull(node->group_pivot));

		/*
		 * 从外层节点读取元组并装入前缀排序状态，直到   
		 * 遇到某个前缀键与当前 group_pivot 元组不匹配        
		 * 的元组为止，因为在拥有所有匹配这些前缀键      
		 * 的元组前，无法保证排序的稳定性。    
		 */
		for (;;)
		{
			slot = ExecProcNode(outerNode);

			/*
			 * 如果外层节点的元组已用尽，前缀排序状  
			 * 态的装载即告完成。   
			 */
			if (TupIsNull(slot))
			{
				/*
				 * 我们稍后需要知道外层节点是否完成，以便  
				 * 区分是完成了一个批次，                      
				 * 还是完成了整个节点。 
				 */
				node->outerNodeDone = true;
				break;
			}

			/*
			 * 如果该元组的前缀键与枢轴元组匹配，说明  
			 * 尚未完成，可以把它装入前缀排序状态。如果   
			 * 不匹配，我们不想把它作为当前批次的一部分  
			 * 排序，而是改用 group_pivot 槽把它结转到下一 
			 * 批（尽管不会真的把它当作组枢轴）。       
			 */
			if (isCurrentGroup(node, node->group_pivot, slot))
			{
				tuplesort_puttupleslot(node->prefixsort_state, slot);
				nTuples++;
			}
			else
			{
				ExecCopySlot(node->group_pivot, slot);
				break;
			}
		}

		/*
		 * 执行排序，并开始把元组返回给父计划节点。      
		 * node.
		 */
		SO1_printf("Sorting presorted prefix tuplesort with " INT64_FORMAT " tuples\n", nTuples);
		tuplesort_performsort(node->prefixsort_state);

		INSTRUMENT_SORT_GROUP(node, prefixsort);

		SO_printf("Setting execution_status to INCSORT_READPREFIXSORT (found end of group)\n");
		node->execution_status = INCSORT_READPREFIXSORT;

		if (node->bounded)
		{
			/*
			 * 如果当前节点有边界且已排序 n 个元组，     
			 * 则剩余的有效边界是（原始边界 - n），所以    
			 * 保存当前已处理的元组数，供配置排序边界    
			 * 时使用。                 
			 */
			SO2_printf("Changing bound_Done from " INT64_FORMAT " to " INT64_FORMAT "\n",
					   node->bound_Done,
					   Min(node->bound, node->bound_Done + nTuples));
			node->bound_Done = Min(node->bound, node->bound_Done + nTuples);
		}
	}

	/* 恢复为用户指定的方向。    */
	estate->es_direction = dir;

	/*
	 * 从 tuplesort 取首个或下一元组。无更多元组时返回  
	 * NULL。
	 */
	read_sortstate = node->execution_status == INCSORT_READFULLSORT ?
		fullsort_state : node->prefixsort_state;
	slot = node->ss.ps.ps_ResultTupleSlot;
	(void) tuplesort_gettupleslot(read_sortstate, ScanDirectionIsForward(dir),
								  false, slot, NULL);
	return slot;
}

/* ----------------------------------------------------------------
 *		ExecInitIncrementalSort
 *
 *		为规划器生成的排序节点建运行时状态，  
 *		并初始化其外层子树。                            
 * ----------------------------------------------------------------
 */
IncrementalSortState *
ExecInitIncrementalSort(IncrementalSort *node, EState *estate, int eflags)
{
	IncrementalSortState *incrsortstate;

	SO_printf("ExecInitIncrementalSort: initializing sort node\n");

	/*
	 * 增量排序不能与 EXEC_FLAG_BACKWARD 或             
	 * EXEC_FLAG_MARK 一起使用，因为当前排序状态只包含      
	 * 一个批次而非完整结果集。  
	 */
	Assert((eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) == 0);

	/* 初始化状态结构。    */
	incrsortstate = makeNode(IncrementalSortState);
	incrsortstate->ss.ps.plan = (Plan *) node;
	incrsortstate->ss.ps.state = estate;
	incrsortstate->ss.ps.ExecProcNode = ExecIncrementalSort;

	incrsortstate->execution_status = INCSORT_LOADFULLSORT;
	incrsortstate->bounded = false;
	incrsortstate->outerNodeDone = false;
	incrsortstate->bound_Done = 0;
	incrsortstate->fullsort_state = NULL;
	incrsortstate->prefixsort_state = NULL;
	incrsortstate->group_pivot = NULL;
	incrsortstate->transfer_tuple = NULL;
	incrsortstate->n_fullsort_remaining = 0;
	incrsortstate->presorted_keys = NULL;

	if (incrsortstate->ss.ps.instrument != NULL)
	{
		IncrementalSortGroupInfo *fullsortGroupInfo =
			&incrsortstate->incsort_info.fullsortGroupInfo;
		IncrementalSortGroupInfo *prefixsortGroupInfo =
			&incrsortstate->incsort_info.prefixsortGroupInfo;

		fullsortGroupInfo->groupCount = 0;
		fullsortGroupInfo->maxDiskSpaceUsed = 0;
		fullsortGroupInfo->totalDiskSpaceUsed = 0;
		fullsortGroupInfo->maxMemorySpaceUsed = 0;
		fullsortGroupInfo->totalMemorySpaceUsed = 0;
		fullsortGroupInfo->sortMethods = 0;
		prefixsortGroupInfo->groupCount = 0;
		prefixsortGroupInfo->maxDiskSpaceUsed = 0;
		prefixsortGroupInfo->totalDiskSpaceUsed = 0;
		prefixsortGroupInfo->maxMemorySpaceUsed = 0;
		prefixsortGroupInfo->totalMemorySpaceUsed = 0;
		prefixsortGroupInfo->sortMethods = 0;
	}

	/*
	 * 杂项初始化             
	 *
	 * 排序节点不初始化 ExprContext，因为它们从不调用       
	 * ExecQual/ExecProject。 
	 */

	/*
	 * 初始化子节点。  
	 *
	 * 增量排序不支持反向扫描和 mark/restore，所以这里      
	 * 我们无需费心从 eflags 中移除相应标志位。我们允许传
	 * 入 REWIND 标志，因为虽然增量排序用不上它，但子     
	 * 节点也许能做一些更有用的事情。 
	 */
	outerPlanState(incrsortstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * 初始化扫描槽和类型。
	 */
	ExecCreateScanSlotFromOuterPlan(estate, &incrsortstate->ss, &TTSOpsMinimalTuple);

	/*
	 * 初始化返回槽和类型。无需初始化投影信息，因为    
	 * 我们不做任何投影。         
	 */
	ExecInitResultTupleSlotTL(&incrsortstate->ss.ps, &TTSOpsMinimalTuple);
	incrsortstate->ss.ps.ps_ProjInfo = NULL;

	/*
	 * 初始化独立槽，用于存放枢轴前缀键元组，             
	 * 以及把元组从一批结转到下一批。        
	 */
	incrsortstate->group_pivot =
		MakeSingleTupleTableSlot(ExecGetResultType(outerPlanState(incrsortstate)),
								 &TTSOpsMinimalTuple);
	incrsortstate->transfer_tuple =
		MakeSingleTupleTableSlot(ExecGetResultType(outerPlanState(incrsortstate)),
								 &TTSOpsMinimalTuple);

	SO_printf("ExecInitIncrementalSort: sort node initialized\n");

	return incrsortstate;
}

/* ----------------------------------------------------------------
 *		ExecEndIncrementalSort(node)
 * ----------------------------------------------------------------
 */
void
ExecEndIncrementalSort(IncrementalSortState *node)
{
	SO_printf("ExecEndIncrementalSort: shutting down sort node\n");

	/* 清空扫描元组       */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	/* 必须丢弃排序结果元组的指针*/
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	/* 必须丢弃来自外层节点的独立元组槽 */
	ExecDropSingleTupleTableSlot(node->group_pivot);
	ExecDropSingleTupleTableSlot(node->transfer_tuple);

	/*
	 * 释放 tuplesort 资源     
	 */
	if (node->fullsort_state != NULL)
	{
		tuplesort_end(node->fullsort_state);
		node->fullsort_state = NULL;
	}
	if (node->prefixsort_state != NULL)
	{
		tuplesort_end(node->prefixsort_state);
		node->prefixsort_state = NULL;
	}

	/*
	 * 关闭子计划。    
	 */
	ExecEndNode(outerPlanState(node));

	SO_printf("ExecEndIncrementalSort: sort node shutdown\n");
}

void
ExecReScanIncrementalSort(IncrementalSortState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/*
	 * 即使参数未变（如 rewind），增量排序也无法高效地   
	 * 重新扫描，因为不同于普通排序，我们不会为       
	 * 全排序一次性存储所有元组。    
	 *
	 * 因此即使设置了 EXEC_FLAG_REWIND，我们也只是重置       
	 * 全部状态并随子节点一起重新执行排序。增量排序    
	 * 本身无法更聪明，但子节点也许可以。      
	 *
	 * 理论上若我们只向全排序填充了一个批次（且    
	 * 尚未为新批次重置它），那么我们可以高效地回退，  
	 * 但该情形似乎足够窄，眼下不值得                   
	 * 专门处理。        
	 */

	/* 必须丢弃排序结果元组的指针*/
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	if (node->group_pivot != NULL)
		ExecClearTuple(node->group_pivot);
	if (node->transfer_tuple != NULL)
		ExecClearTuple(node->transfer_tuple);

	node->outerNodeDone = false;
	node->n_fullsort_remaining = 0;
	node->bound_Done = 0;

	node->execution_status = INCSORT_LOADFULLSORT;

	/*
	 * 如果我们已经设置好任意一个排序状态，就需要重置
	 * 它们。可以结束它们并置空指针，但没有理由去      
	 * 重新支付设置成本；而且由于 ExecIncrementalSort 会通过 
	 * 检查全排序状态是否已初始化来守卫预排序          
	 * 列函数，所以在这里把排序状态置空反而可能造成  
	 * 泄漏。    
	 */
	if (node->fullsort_state != NULL)
		tuplesort_reset(node->fullsort_state);
	if (node->prefixsort_state != NULL)
		tuplesort_reset(node->prefixsort_state);

	/*
	 * 如果子节点的 chgParam 非空，那么第一次 ExecProcNode      
	 * 即会重扫计划。  
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}

/* ----------------------------------------------------------------
 *						Parallel Query Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecSortEstimate
 *
 *		估算传播排序统计所需的空间。           
 * ----------------------------------------------------------------
 */
void
ExecIncrementalSortEstimate(IncrementalSortState *node, ParallelContext *pcxt)
{
	Size		size;

	/* 若不做插桩或无 worker，无需此调用     */
	if (!node->ss.ps.instrument || pcxt->nworkers == 0)
		return;

	size = mul_size(pcxt->nworkers, sizeof(IncrementalSortInfo));
	size = add_size(size, offsetof(SharedIncrementalSortInfo, sinfo));
	shm_toc_estimate_chunk(&pcxt->estimator, size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/* ----------------------------------------------------------------
 *		ExecSortInitializeDSM
 *
 *		为排序统计初始化 DSM 空间。   
 * ----------------------------------------------------------------
 */
void
ExecIncrementalSortInitializeDSM(IncrementalSortState *node, ParallelContext *pcxt)
{
	Size		size;

	/* 若不做插桩或无 worker，无需此调用     */
	if (!node->ss.ps.instrument || pcxt->nworkers == 0)
		return;

	size = offsetof(SharedIncrementalSortInfo, sinfo)
		+ pcxt->nworkers * sizeof(IncrementalSortInfo);
	node->shared_info = shm_toc_allocate(pcxt->toc, size);
	/* 确保任何未填充的槽都包含零值    */
	memset(node->shared_info, 0, size);
	node->shared_info->num_workers = pcxt->nworkers;
	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id,
				   node->shared_info);
}

/* ----------------------------------------------------------------
 *		ExecSortInitializeWorker
 *
 *		把 worker 附接到排序统计 DSM 空间。 
 * ----------------------------------------------------------------
 */
void
ExecIncrementalSortInitializeWorker(IncrementalSortState *node, ParallelWorkerContext *pwcxt)
{
	node->shared_info =
		shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, true);
	node->am_worker = true;
}

/* ----------------------------------------------------------------
 *		ExecSortRetrieveInstrumentation
 *
 *		把排序统计从 DSM 传送到私有内存。     
 * ----------------------------------------------------------------
 */
void
ExecIncrementalSortRetrieveInstrumentation(IncrementalSortState *node)
{
	Size		size;
	SharedIncrementalSortInfo *si;

	if (node->shared_info == NULL)
		return;

	size = offsetof(SharedIncrementalSortInfo, sinfo)
		+ node->shared_info->num_workers * sizeof(IncrementalSortInfo);
	si = palloc(size);
	memcpy(si, node->shared_info, size);
	node->shared_info = si;
}
