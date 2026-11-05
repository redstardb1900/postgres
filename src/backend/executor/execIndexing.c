/*-------------------------------------------------------------------------
 *
 * execIndexing.c
 *	  用于插入索引元组并强制唯一约束与排除      
 *	  约束的例程。    
 *
 * ExecInsertIndexTuples() 是主入口。它在向堆插入          
 * 元组后被调用，它会向所有索引插入对应的索引         
 * 元组。同时，它会强制所有唯一约束和            
 * 排除约束：       
 *
 * 唯一索引  
 * --------------
 *
 * 强制唯一约束很直接。当索引 AM 将元组插入索引    
 * 时，它还会检查索引中是否已存在冲突的元组。 
 * 这一检查是原子性地进行的，因此即便两个后端         
 * 并发地插入相同的键，也只会有一个成功。确保       
 * 原子性的全部逻辑，以及等待进行中的事务结束      
 * 的逻辑，都由索引 AM 处理。                              
 *
 * 如果唯一约束是可延迟的，我们会请求索引 AM      
 * 在发现冲突时不要抛出错误。相应地，我们会记         
 * 下存在冲突这一事实，并把有冲突的索引列表返    
 * 回给调用者。调用者之后必须带 UNIQUE_CHECK_EXISTING       
 * 选项重新调用 index_insert()。  
 *
 * 排除约束         
 * ---------------------
 *
 * 排除约束与唯一索引的不同之处在于：向索引插         
 * 入元组时，索引 AM 并不会同时检查重复的键。  
 * 插入完成之后，我们会单独对索引执行一次扫描    
 * 来检查是否存在冲突元组；如果找到，我们会抛       
 * 出错误并且事务会被中止。如果冲突元组的          
 * 插入者或删除者仍在进行中，我们会先等待它          
 * 完成。       
 *
 * 如果两个后端同时插入一个元组，然后又执行检     
 * 查冲突的扫描，就有可能发生死锁。它们会彼此        
 * 发现对方的元组，并且都试图等待对方。死锁检   
 * 测器会检测到这种情况，并中止其中一个事务。       
 * 这相当无害，因为其中一个事务反正注定要以    
 * "duplicate key error"而中止，尽管你得到的错误消息 
 * 不同  
 *
 * 如果排除约束是可延迟的，我们仍会在插入索引      
 * 元组之后立即执行冲突检查扫描。但不同于唯一        
 * 约束之处在于：发现冲突时不抛出错误，而是把        
 * 该信息返回给调用者。调用者稍后须再次调用  
 * check_exclusion_constraint() 
 *
 * 投机性插入      
 * ---------------------
 *
 * 投机插入是一种两阶段机制，用于实现             
 * INSERT ... ON CONFLICT DO UPDATE/NOTHING。元组首                  
 * 先被插入堆中并照常更新索引；但如果某约束被    
 * 违反，我们仍可以撤回该次插入而不用中止整个         
 * 事务。在 INSERT ... ON CONFLICT 语句中，一旦检             
 * 测到冲突，被插入的元组会被撤回，转而执行            
 * ON CONFLICT动作
 *
 * 向唯一索引插入的工作方式照旧：索引 AM 以原子   
 * 方式在插入的同时检查重复键。但遇到冲突时不       
 * 抛出错误，而是把投机插入的堆元组撤                    
 * 回 
 *
 * 排除约束的情况要更复杂一些。如前所述，当两   
 * 个后端并发插入相同的键时存在死锁风险。对于       
 * 常规插入这不成问题，因为反正总有一个事务要       
 * 被中止；但投机插入不允许发生死锁，因为我们         
 * 我们只希望在冲突时撤回投机插入的元组，              
 * 而不是中止整个                                                
 * 事务。   
 *
 * 当后端检测到投机插入与另一个进行中的元组相     
 * 冲突时，它有两个选择：             
 *
 * 1. 撤回投机插入的元组，然后等待另一个事务，      
 *	  再重试。或者，      
 * 2. 等待另一个事务，同时让投机插入的元组保持         
 *	  原位不动。
 *
 * 如果两个后端同时插入，并且都试图等待对方，       
 * 就会死锁。因此选择 2 不可接受。选择 1 避免了死  
 * 锁，但反而容易出现活锁。两个事务都会在对方  
 * 撤回时立即唤醒。随后二者都重试，并再次相互     
 * 冲突，周而复始，无限                                         
 * 往复。     
 *
 * 为避免活锁，必须有一个后端先撤回，然后等待；      
 * 而另一个则不撤回直接等待。由谁撤回并不重要，     
 * 因此我们采用一条任意规则：让 XID 较大的事务          
 * 撤回。                     
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execIndexing.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "storage/lmgr.h"
#include "utils/snapmgr.h"

/* check_exclusion_or_unique_constraint() 的 waitMode 参数  */
typedef enum
{
	CEOUC_WAIT,
	CEOUC_NOWAIT,
	CEOUC_LIVELOCK_PREVENTING_WAIT
} CEOUC_WAIT_MODE;

static bool check_exclusion_or_unique_constraint(Relation heap, Relation index,
												 IndexInfo *indexInfo,
												 ItemPointer tupleid,
												 Datum *values, bool *isnull,
												 EState *estate, bool newIndex,
												 CEOUC_WAIT_MODE waitMode,
												 bool violationOK,
												 ItemPointer conflictTid);

static bool index_recheck_constraint(Relation index, Oid *constr_procs,
									 Datum *existing_values, bool *existing_isnull,
									 Datum *new_values);
static bool index_unchanged_by_update(ResultRelInfo *resultRelInfo,
									  EState *estate, IndexInfo *indexInfo,
									  Relation indexRelation);
static bool index_expression_changed_walker(Node *node,
											Bitmapset *allUpdatedCols);

/* ----------------------------------------------------------------
 *		ExecOpenIndices
 *
 *		找出与结果关系相关联的索引并打开它们，     
 *		并将它们的信息保存到结果的 ResultRelInfo 中。
 *
 *		进入时，调用者已经打开并锁定了 
 *		resultRelInfo->ri_RelationDesc.
 * ----------------------------------------------------------------
 */
void
ExecOpenIndices(ResultRelInfo *resultRelInfo, bool speculative)
{
	Relation	resultRelation = resultRelInfo->ri_RelationDesc;
	List	   *indexoidlist;
	ListCell   *l;
	int			len,
				i;
	RelationPtr relationDescs;
	IndexInfo **indexInfoArray;

	resultRelInfo->ri_NumIndices = 0;

	/* 无索引的快速路径*/
	if (!RelationGetForm(resultRelation)->relhasindex)
		return;

	/*
	 * 获取缓存索引 OID 列表
	 */
	indexoidlist = RelationGetIndexList(resultRelation);
	len = list_length(indexoidlist);
	if (len == 0)
		return;

	/*
	 * 为结果数组分配空间     
	 */
	relationDescs = (RelationPtr) palloc(len * sizeof(Relation));
	indexInfoArray = (IndexInfo **) palloc(len * sizeof(IndexInfo *));

	resultRelInfo->ri_NumIndices = len;
	resultRelInfo->ri_IndexRelationDescs = relationDescs;
	resultRelInfo->ri_IndexRelationInfo = indexInfoArray;

	/*
	 * 对每个索引，打开索引关系并保存 pg_index 信息。  
	 * 获取 RowExclusiveLock，表明我们将更新该索引。   
	 *
	 * 注意：即使索引不是 indisready，我们也照样这样      
	 * 做；为它不是的情形做优化并不值得。 
	 */
	i = 0;
	foreach(l, indexoidlist)
	{
		Oid			indexOid = lfirst_oid(l);
		Relation	indexDesc;
		IndexInfo  *ii;

		indexDesc = index_open(indexOid, RowExclusiveLock);

		/* 从索引的 pg_index 信息中提取索引键信息         */
		ii = BuildIndexInfo(indexDesc);

		/*
		 * 如果这些索引将用于投机插入，则添加唯一索引   
		 * 条目所需的额外信息                  
		 */
		if (speculative && ii->ii_Unique)
			BuildSpeculativeIndexInfo(indexDesc, ii);

		relationDescs[i] = indexDesc;
		indexInfoArray[i] = ii;
		i++;
	}

	list_free(indexoidlist);
}

/* ----------------------------------------------------------------
 *		ExecCloseIndices
 *
 *		关闭 resultRelInfo 中存储的索引关系    
 * ----------------------------------------------------------------
 */
void
ExecCloseIndices(ResultRelInfo *resultRelInfo)
{
	int			i;
	int			numIndices;
	RelationPtr indexDescs;

	numIndices = resultRelInfo->ri_NumIndices;
	indexDescs = resultRelInfo->ri_IndexRelationDescs;

	for (i = 0; i < numIndices; i++)
	{
		if (indexDescs[i] == NULL)
			continue;			/* 不该发生？   */

		/* 释放 ExecOpenIndices 获取的锁   */
		index_close(indexDescs[i], RowExclusiveLock);
	}

	/*
	 * XXX 这里是否也应当释放 indexInfo 数组？目前我们    
	 * 假定这类内容会在 FreeExecutorState 中自动清理。    
	 */
}

/* ----------------------------------------------------------------
 *		ExecInsertIndexTuples
 *
 *		当堆元组插入结果关系时，本例程    
 *		负责向对该结果关系建索引的所有关系
 *		插入索引元组。                                  
 *
 *		当 'update' 为真且 'onlySummarizing' 为假时，
 *		执行器在执行无法利用 heapam 的 HOT 类     
 *		优化的 UPDATE（更一般地说，即发生过      
 *		table_tuple_update() 调用且 'update_indexes'
 *		被设为 TU_All）。收到该提示会考虑是否   
 *		向下传递 'indexUnchanged' 提示。这件事仅    
 *		当 'update' 为真时才对每次 index_insert()     
 *		调用来判定。（'update' 为假时已知 
 *		不会把该提示传给                               
 *		任何索引。） 
 *
 *		若设置了 onlySummarizing，说明已应用了与    
 *		HOT 等价的优化，更新过的列只由汇总索    
 *		引建立索引（更一般地说，发生过          
 *		table_tuple_update() 调用并设              
 *		'update_indexes' 为 TU_Summarizing）。因此可    
 *		以（且必须）只更新 'amsummarizing'
 *		= true 的索引。    
 *
 *		唯一约束与排除约束会同时被强制执行。   
 *		返回那些已延迟且有潜在（未确认）冲突的 
 *		唯一或排除约束的索引 OID 列表。        
 *		（若 noDupErr == true，对非延迟约束也如此， 
 *		但会向调用者报告冲突为投机性或延迟      
 *		性的。）                                               
 *
 *		若 'arbiterIndexes' 非空，则 noDupErr 只适用     
 *		于这些索引。NIL 则表示适用于所有索引。   
 * ----------------------------------------------------------------
 */
List *
ExecInsertIndexTuples(ResultRelInfo *resultRelInfo,
					  TupleTableSlot *slot,
					  EState *estate,
					  bool update,
					  bool noDupErr,
					  bool *specConflict,
					  List *arbiterIndexes,
					  bool onlySummarizing)
{
	ItemPointer tupleid = &slot->tts_tid;
	List	   *result = NIL;
	int			i;
	int			numIndices;
	RelationPtr relationDescs;
	Relation	heapRelation;
	IndexInfo **indexInfoArray;
	ExprContext *econtext;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	Assert(ItemPointerIsValid(tupleid));

	/*
	 * 从结果关系信息结构中获取信息。           
	 */
	numIndices = resultRelInfo->ri_NumIndices;
	relationDescs = resultRelInfo->ri_IndexRelationDescs;
	indexInfoArray = resultRelInfo->ri_IndexRelationInfo;
	heapRelation = resultRelInfo->ri_RelationDesc;

	/* 完整性检查：slot 必须属于与 resultRelInfo 相同的关系 */
	Assert(slot->tts_tableOid == RelationGetRelid(heapRelation));

	/*
	 * 我们将使用 EState 的每元组上下文来求值谓词         
	 * 和索引表达式（若尚未创建则先创建）。        
	 */
	econtext = GetPerTupleExprContext(estate);

	/* 安排 econtext 的扫描元组指向待检测的元组      */
	econtext->ecxt_scantuple = slot;

	/*
	 * 对每个索引，构造并插入索引元组  
	 */
	for (i = 0; i < numIndices; i++)
	{
		Relation	indexRelation = relationDescs[i];
		IndexInfo  *indexInfo;
		bool		applyNoDupErr;
		IndexUniqueCheck checkUnique;
		bool		indexUnchanged;
		bool		satisfiesConstraint;

		if (indexRelation == NULL)
			continue;

		indexInfo = indexInfoArray[i];

		/* 若索引被标记为只读，则忽略它     */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/*
		 * 若只更新汇总索引，则跳过非汇总               
		 * 索引的处理    
		 */
		if (onlySummarizing && !indexInfo->ii_Summarizing)
			continue;

		/* 检查部分索引      */
		if (indexInfo->ii_Predicate != NIL)
		{
			ExprState  *predicate;

			/*
			 * 若谓词状态未建立，则在 estate 的每查询上下  
			 * 文中创建      
			 */
			predicate = indexInfo->ii_PredicateState;
			if (predicate == NULL)
			{
				predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);
				indexInfo->ii_PredicateState = predicate;
			}

			/* 若谓词不满足，则跳过这次索引更新        */
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * FormIndexDatum 会用索引各列的适当取值来填充        
		 * 它的 values 和 isnull 参数。                
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/* 检查是否对该索引应用 noDupErr       */
		applyNoDupErr = noDupErr &&
			(arbiterIndexes == NIL ||
			 list_member_oid(arbiterIndexes,
							 indexRelation->rd_index->indexrelid));

		/*
		 * 索引 AM 负责执行实际的插入，外加唯一性检查。 
		 *
		 * 对立即模式唯一索引，我们只要求索引 AM          
		 * 在不唯一时抛错。  
		 *
		 * 对可延迟唯一索引，我们让索引 AM 只检测可能     
		 * 的不唯一性，并在需要进一步检查时把该索引的
		 * OID 加进结果列表。          
		 *
		 * 对于投机插入（INSERT ... ON CONFLICT 所用），做     
		 * 法与可延迟唯一索引相同。      
		 */
		if (!indexRelation->rd_index->indisunique)
			checkUnique = UNIQUE_CHECK_NO;
		else if (applyNoDupErr)
			checkUnique = UNIQUE_CHECK_PARTIAL;
		else if (indexRelation->rd_index->indimmediate)
			checkUnique = UNIQUE_CHECK_YES;
		else
			checkUnique = UNIQUE_CHECK_PARTIAL;

		/*
		 * 对这个索引必然会有一次 index_insert() 调用。    
		 * 若我们正作为 UPDATE 语句的一部分被调用，     
		 * 则考虑是否应传 'indexUnchanged' = true 提示。       
		 */
		indexUnchanged = update && index_unchanged_by_update(resultRelInfo,
															 estate,
															 indexInfo,
															 indexRelation);

		satisfiesConstraint =
			index_insert(indexRelation, /* 索引关系   */
						 values,	/* 索引 Datum 数组   */
						 isnull,	/* NULL 标志*/
						 tupleid,	/* 堆元组的 TID  */
						 heapRelation,	/* 堆关系     */
						 checkUnique,	/* 要做的唯一性检查类型 */
						 indexUnchanged,	/* UPDATE 且无逻辑改动？   */
						 indexInfo);	/* 索引 AM 可能需要 */

		/*
		 * 如果该索引关联有排除约束，则对此进行检查。 
		 * 这比唯一性检查的过程简单，因为我们总是先插
		 * 入然后才检查。如果约束是可延迟的，我们现在    
		 * 也会执行检查，但在违反时不抛错，也不等待并    
		 * 发插入给出确定性的结果；相反，我们会排队一
		 * 个重查事件。类似地，noDupErr 调用者（投机插入 
		 * 者）稍后也会重新检查，并在那时等待确定的结 
		 * 果  
		 *
		 * 排除约束的索引不能同时还是 UNIQUE（这并非本质 
		 * 属性，只是语法中不允许它），所以无需保留     
		 * satisfiesConstraint 的先前状态。                  
		 */
		if (indexInfo->ii_ExclusionOps != NULL)
		{
			bool		violationOK;
			CEOUC_WAIT_MODE waitMode;

			if (applyNoDupErr)
			{
				violationOK = true;
				waitMode = CEOUC_LIVELOCK_PREVENTING_WAIT;
			}
			else if (!indexRelation->rd_index->indimmediate)
			{
				violationOK = true;
				waitMode = CEOUC_NOWAIT;
			}
			else
			{
				violationOK = false;
				waitMode = CEOUC_WAIT;
			}

			satisfiesConstraint =
				check_exclusion_or_unique_constraint(heapRelation,
													 indexRelation, indexInfo,
													 tupleid, values, isnull,
													 estate, false,
													 waitMode, violationOK, NULL);
		}

		if ((checkUnique == UNIQUE_CHECK_PARTIAL ||
			 indexInfo->ii_ExclusionOps != NULL) &&
			!satisfiesConstraint)
		{
			/*
			 * 元组可能违反唯一或排除约束，因此记下该 
			 * 索引以便稍后重查。投机插入者会被告知         
			 * 是否发生了投机冲突，因为那总是需要     
			 * 重新开始。                                            
			 */
			result = lappend_oid(result, RelationGetRelid(indexRelation));
			if (indexRelation->rd_index->indimmediate && specConflict)
				*specConflict = true;
		}
	}

	return result;
}

/* ----------------------------------------------------------------
 *		ExecCheckIndexConstraints
 *
 *		本例程检查元组是否违反任何唯一或     
 *		排除约束。无冲突则返回真，否则返回假，    
 *		冲突元组的 TID 会通过                          
 *		*conflictTid 返回。            
 *
 *		若给定了 'arbiterIndexes'，则只检查那些索引。  
 *		NIL 即全部索引   
 *
 *		注意它不会以任何方式锁定值，故本例程返 
 *		回后立即插入冲突元组是可能的。但它可用
 *		以用于插入前的                                    
 *		预检查。     
 * ----------------------------------------------------------------
 */
bool
ExecCheckIndexConstraints(ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
						  EState *estate, ItemPointer conflictTid,
						  List *arbiterIndexes)
{
	int			i;
	int			numIndices;
	RelationPtr relationDescs;
	Relation	heapRelation;
	IndexInfo **indexInfoArray;
	ExprContext *econtext;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ItemPointerData invalidItemPtr;
	bool		checkedIndex = false;

	ItemPointerSetInvalid(conflictTid);
	ItemPointerSetInvalid(&invalidItemPtr);

	/*
	 * 从结果关系信息结构中获取信息。           
	 */
	numIndices = resultRelInfo->ri_NumIndices;
	relationDescs = resultRelInfo->ri_IndexRelationDescs;
	indexInfoArray = resultRelInfo->ri_IndexRelationInfo;
	heapRelation = resultRelInfo->ri_RelationDesc;

	/*
	 * 我们将使用 EState 的每元组上下文来求值谓词         
	 * 和索引表达式（若尚未创建则先创建）。        
	 */
	econtext = GetPerTupleExprContext(estate);

	/* 安排 econtext 的扫描元组指向待检测的元组      */
	econtext->ecxt_scantuple = slot;

	/*
	 * 对每个索引，构造索引元组并检查它是否满足  
	 * 约束。  
	 */
	for (i = 0; i < numIndices; i++)
	{
		Relation	indexRelation = relationDescs[i];
		IndexInfo  *indexInfo;
		bool		satisfiesConstraint;

		if (indexRelation == NULL)
			continue;

		indexInfo = indexInfoArray[i];

		if (!indexInfo->ii_Unique && !indexInfo->ii_ExclusionOps)
			continue;

		/* 若索引被标记为只读，则忽略它     */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/* 当指定了特定仲裁索引时，只检查它们        */
		if (arbiterIndexes != NIL &&
			!list_member_oid(arbiterIndexes,
							 indexRelation->rd_index->indexrelid))
			continue;

		if (!indexRelation->rd_index->indimmediate)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("ON CONFLICT does not support deferrable unique constraints/exclusion constraints as arbiters"),
					 errtableconstraint(heapRelation,
										RelationGetRelationName(indexRelation))));

		checkedIndex = true;

		/* 检查部分索引      */
		if (indexInfo->ii_Predicate != NIL)
		{
			ExprState  *predicate;

			/*
			 * 若谓词状态未建立，则在 estate 的每查询上下  
			 * 文中创建      
			 */
			predicate = indexInfo->ii_PredicateState;
			if (predicate == NULL)
			{
				predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);
				indexInfo->ii_PredicateState = predicate;
			}

			/* 若谓词不满足，则跳过这次索引更新        */
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * FormIndexDatum 会用索引各列的适当取值来填充        
		 * 它的 values 和 isnull 参数。                
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		satisfiesConstraint =
			check_exclusion_or_unique_constraint(heapRelation, indexRelation,
												 indexInfo, &invalidItemPtr,
												 values, isnull, estate, false,
												 CEOUC_WAIT, true,
												 conflictTid);
		if (!satisfiesConstraint)
			return false;
	}

	if (arbiterIndexes != NIL && !checkedIndex)
		elog(ERROR, "unexpected failure to find arbiter index");

	return true;
}

/*
 * 检查是否违反排除约束或唯一约束           
 *
 * heap：包含新元组的表            
 * index：支持该约束的索引          
 * indexInfo：索引的信息，含排除约束属性                
 * tupleid：我们刚插入的新元组的堆 TID（若尚未            
 *		插入新元组则为无效）   
 * values, isnull：为新元组算出的*索引*列值                
 * estate：可供我们求值用的 EState 
 * newIndex：为真表示我们正在尝试构建新索引（这     
 *		只影响错误消息的措辞）  
 * waitMode：是否等待并发的插入者/删除者        
 * violationOK：为真则违反时不抛错            
 * conflictTid：若非 NULL，冲突元组的 TID 会在此返回            
 *
 * 成功返回真；有实际或潜在冲突则返回假    
 *
 * 'waitMode' 决定当检测到与一个仍处于进行中的事           
 * 务所插入或删除的元组冲突时会发生什么。          
 * CEOUC_WAIT 表示我们等待该事务提交之后再抛错        
 * 或返回。CEOUC_NOWAIT 表示我们立即报告违反；            
 * 因此该违反只是潜在的，调用者稍后必须重新检          
 * 查。这一行为对延迟排除检查很方便；如果在插       
 * 入时肯定没有冲突，我们就不必费心排队延迟事          
 * 件了。                                
 *
 * CEOUC_LIVELOCK_PREVENTING_WAIT 类似 CEOUC_NOWAIT，                     
 * 但有时我们也会等待，以防两个事务同时尝试插         
 * 入时发生活锁。它用于投机插入，即 INSERT ON             
 * CONFLICT 语句。（见文件头说明）     
 *
 * 若 violationOK 为真，我们只需通过返回 'false' 把潜在        
 * 或实际的违反报告给调用者。否则我们在此抛出         
 * 一条描述性错误消息。violationOK 为假时不可能返回假  
 *
 * 注意：唯一约束的检查通常由索引访问方法负责，        
 * 因此它通常只需用于排除约束。但在投机插入时，         
 * 也会调用本函数来对唯一约束的冲突做"预检查"。         
 * 调用者可以用返回的冲突 TID 来采取进一步的               
 * 后续步骤。                    
 */
static bool
check_exclusion_or_unique_constraint(Relation heap, Relation index,
									 IndexInfo *indexInfo,
									 ItemPointer tupleid,
									 Datum *values, bool *isnull,
									 EState *estate, bool newIndex,
									 CEOUC_WAIT_MODE waitMode,
									 bool violationOK,
									 ItemPointer conflictTid)
{
	Oid		   *constr_procs;
	uint16	   *constr_strats;
	Oid		   *index_collations = index->rd_indcollation;
	int			indnkeyatts = IndexRelationGetNumberOfKeyAttributes(index);
	IndexScanDesc index_scan;
	ScanKeyData scankeys[INDEX_MAX_KEYS];
	SnapshotData DirtySnapshot;
	int			i;
	bool		conflict;
	bool		found_self;
	ExprContext *econtext;
	TupleTableSlot *existing_slot;
	TupleTableSlot *save_scantuple;

	if (indexInfo->ii_ExclusionOps)
	{
		constr_procs = indexInfo->ii_ExclusionProcs;
		constr_strats = indexInfo->ii_ExclusionStrats;
	}
	else
	{
		constr_procs = indexInfo->ii_UniqueProcs;
		constr_strats = indexInfo->ii_UniqueStrats;
	}

	/*
	 * 如果任一输入值为 NULL，且该索引使用默认的        
	 * nulls-are-distinct 模式，则假定约束检查通过（即          
	 * 我们假定操作符是严格的）。否则，我们会把     
	 * 约束解释为对每个输入值为 NULL 的列指定 IS            
	 * NULL 
	 */
	if (!indexInfo->ii_NullsNotDistinct)
	{
		for (i = 0; i < indnkeyatts; i++)
		{
			if (isnull[i])
				return true;
		}
	}

	/*
	 * 在索引中已有的元组里搜索任何违反情况，包括      
	 * 尚不可见的元组。       
	 */
	InitDirtySnapshot(DirtySnapshot);

	for (i = 0; i < indnkeyatts; i++)
	{
		ScanKeyEntryInitialize(&scankeys[i],
							   isnull[i] ? SK_ISNULL | SK_SEARCHNULL : 0,
							   i + 1,
							   constr_strats[i],
							   InvalidOid,
							   index_collations[i],
							   constr_procs[i],
							   values[i]);
	}

	/*
	 * 需要一个 TupleTableSlot 放现有元组。  
	 *
	 * 要使用 FormIndexDatum，我们必须让 econtext 的              
	 * scantuple 指向该 slot。务必保存并恢复调用者的 
	 * scantuple 
	 */
	existing_slot = table_slot_create(heap, NULL);

	econtext = GetPerTupleExprContext(estate);
	save_scantuple = econtext->ecxt_scantuple;
	econtext->ecxt_scantuple = existing_slot;

	/*
	 * 若发现潜在冲突，则可能必须从此处重新开始       
	 * 扫描
	 */
retry:
	conflict = false;
	found_self = false;
	index_scan = index_beginscan(heap, index, &DirtySnapshot, indnkeyatts, 0);
	index_rescan(index_scan, scankeys, indnkeyatts, NULL, 0);

	while (index_getnext_slot(index_scan, ForwardScanDirection, existing_slot))
	{
		TransactionId xwait;
		XLTW_Oper	reason_wait;
		Datum		existing_values[INDEX_MAX_KEYS];
		bool		existing_isnull[INDEX_MAX_KEYS];
		char	   *error_new;
		char	   *error_existing;

		/*
		 * 忽略对应我们正要检查的元组的条目。  
		 */
		if (ItemPointerIsValid(tupleid) &&
			ItemPointerEquals(tupleid, &existing_slot->tts_tid))
		{
			if (found_self)		/* 不应发生      */
				elog(ERROR, "found self tuple multiple times in index \"%s\"",
					 RelationGetRelationName(index));
			found_self = true;
			continue;
		}

		/*
		 * 从现有的元组中提取索引列值与 isnull 标             
		 * 志   
		 */
		FormIndexDatum(indexInfo, existing_slot, estate,
					   existing_values, existing_isnull);

		/* 若为有损索引扫描，须重查该条件  */
		if (index_scan->xs_recheck)
		{
			if (!index_recheck_constraint(index,
										  constr_procs,
										  existing_values,
										  existing_isnull,
										  values))
				continue;		/* tuple doesn't actually match, so no
								 * conflict */
		}

		/*
		 * 此时我们要么有冲突，要么有潜在冲突。          
		 *
		 * 如果有一个进行中的事务正在影响此元组的可见  
		 * 性，我们就需要等它完成然后重新检查（除非调   
		 * 用者要求不必如此）。为简单起见，我们的重新  
		 * 检查即重启整个扫描 --- 此种情形不常发生，  
		 * 生得不够频繁，不值得更费心，况且等待时我们   
		 * 无论如何都不想持有任何索引内部锁。 
		 */
		xwait = TransactionIdIsValid(DirtySnapshot.xmin) ?
			DirtySnapshot.xmin : DirtySnapshot.xmax;

		if (TransactionIdIsValid(xwait) &&
			(waitMode == CEOUC_WAIT ||
			 (waitMode == CEOUC_LIVELOCK_PREVENTING_WAIT &&
			  DirtySnapshot.speculativeToken &&
			  TransactionIdPrecedes(GetCurrentTransactionId(), xwait))))
		{
			reason_wait = indexInfo->ii_ExclusionOps ?
				XLTW_RecheckExclusionConstr : XLTW_InsertIndex;
			index_endscan(index_scan);
			if (DirtySnapshot.speculativeToken)
				SpeculativeInsertionWait(DirtySnapshot.xmin,
										 DirtySnapshot.speculativeToken);
			else
				XactLockTableWait(xwait, heap,
								  &existing_slot->tts_tid, reason_wait);
			goto retry;
		}

		/*
		 * 我们有一个确定的冲突（或者是潜在冲突，但调
		 * 用者不愿等）。返回给调用者，或者报告。
		 */
		if (violationOK)
		{
			conflict = true;
			if (conflictTid)
				*conflictTid = existing_slot->tts_tid;
			break;
		}

		error_new = BuildIndexValueDescription(index, values, isnull);
		error_existing = BuildIndexValueDescription(index, existing_values,
													existing_isnull);
		if (newIndex)
			ereport(ERROR,
					(errcode(ERRCODE_EXCLUSION_VIOLATION),
					 errmsg("could not create exclusion constraint \"%s\"",
							RelationGetRelationName(index)),
					 error_new && error_existing ?
					 errdetail("Key %s conflicts with key %s.",
							   error_new, error_existing) :
					 errdetail("Key conflicts exist."),
					 errtableconstraint(heap,
										RelationGetRelationName(index))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_EXCLUSION_VIOLATION),
					 errmsg("conflicting key value violates exclusion constraint \"%s\"",
							RelationGetRelationName(index)),
					 error_new && error_existing ?
					 errdetail("Key %s conflicts with existing key %s.",
							   error_new, error_existing) :
					 errdetail("Key conflicts with existing key."),
					 errtableconstraint(heap,
										RelationGetRelationName(index))));
	}

	index_endscan(index_scan);

	/*
	 * 通常，此时搜索应当已经找到了最初插入的那个      
	 * 元组（如果有的话），除非我们因冲突而提前退    
	 * 出了循环。不过，也可能定义出使这一点不成立       
	 * 的排除约束 --- 比如操作符是 <>。所以如果               
	 * found_self 仍为假，我们也不再抱怨。    
	 */

	econtext->ecxt_scantuple = save_scantuple;

	ExecDropSingleTupleTableSlot(existing_slot);

	return !conflict;
}

/*
 * 检查是否违反排除约束                
 *
 * 本函数是 check_exclusion_or_unique_constraint 的                
 * 简化版，供外部调用者使用，无需特殊模式。
 */
void
check_exclusion_constraint(Relation heap, Relation index,
						   IndexInfo *indexInfo,
						   ItemPointer tupleid,
						   Datum *values, bool *isnull,
						   EState *estate, bool newIndex)
{
	(void) check_exclusion_or_unique_constraint(heap, index, indexInfo, tupleid,
												values, isnull,
												estate, newIndex,
												CEOUC_WAIT, false, NULL);
}

/*
 * 检查现有元组的索引值，看它针对 new_values 是       
 * 否真的匹配排除条件。若冲突则返回真。                
 */
static bool
index_recheck_constraint(Relation index, Oid *constr_procs,
						 Datum *existing_values, bool *existing_isnull,
						 Datum *new_values)
{
	int			indnkeyatts = IndexRelationGetNumberOfKeyAttributes(index);
	int			i;

	for (i = 0; i < indnkeyatts; i++)
	{
		/* 假定排除操作符是严格的         */
		if (existing_isnull[i])
			return false;

		if (!DatumGetBool(OidFunctionCall2Coll(constr_procs[i],
											   index->rd_indcollation[i],
											   existing_values[i],
											   new_values[i])))
			return false;
	}

	return true;
}

/*
 * 检查 ExecInsertIndexTuples() 是否应传递                   
 *
 * indexUnchanged 提示。当执行器执行一次需要新一           
 * 轮索引元组的 UPDATE 时，判断是否应对单个索引传       
 * true 提示  
 */
static bool
index_unchanged_by_update(ResultRelInfo *resultRelInfo, EState *estate,
						  IndexInfo *indexInfo, Relation indexRelation)
{
	Bitmapset  *updatedCols;
	Bitmapset  *extraUpdatedCols;
	Bitmapset  *allUpdatedCols;
	bool		hasexpression = false;
	List	   *idxExprs;

	/*
	 * 先查缓存     
	 */
	if (indexInfo->ii_CheckedUnchanged)
		return indexInfo->ii_IndexUnchanged;
	indexInfo->ii_CheckedUnchanged = true;

	/*
	 * 检查被索引属性与被更新列是否有重叠。   
	 *
	 * 只对键列执行这一检查。INCLUDE 索引中非键列          
	 * 的改动不应在此计入。非键列的值对索引 AM 而       
	 * 言是不透明的载荷状态，有点像额外的表 TID。          
	 *
	 * 注意行级 BEFORE 触发器不会影响我们的行为，因      
	 * 为它们一般不影响 updatedCols 位图。看起来并不        
	 * 值得费心去检查哪些属性被直接改动。                  
	 */
	updatedCols = ExecGetUpdatedCols(resultRelInfo, estate);
	extraUpdatedCols = ExecGetExtraUpdatedCols(resultRelInfo, estate);
	for (int attr = 0; attr < indexInfo->ii_NumIndexKeyAttrs; attr++)
	{
		int			keycol = indexInfo->ii_IndexAttrNumbers[attr];

		if (keycol <= 0)
		{
			/*
			 * 暂时跳过表达式，但记得稍后处理它们           
			 * on
			 */
			hasexpression = true;
			continue;
		}

		if (bms_is_member(keycol - FirstLowInvalidHeapAttributeNumber,
						  updatedCols) ||
			bms_is_member(keycol - FirstLowInvalidHeapAttributeNumber,
						  extraUpdatedCols))
		{
			/* 键列有改动 -- 不为该索引传提示     */
			indexInfo->ii_IndexUnchanged = false;
			return false;
		}
	}

	/*
	 * 当我们走到这里且索引没有表达式时，返回真，       
	 * 让 index_insert() 调用去传递 'indexUnchanged' =               
	 *
	 * true 提示。如果没有任何被索引键属性与被            
	 * 更新属性重叠（加上完全没有被索引表达式），     
	 * 就表明整个索引在逻辑上未被 UPDATE 改变。         
	 */
	if (!hasexpression)
	{
		indexInfo->ii_IndexUnchanged = true;
		return true;
	}

	/*
	 * 只需向 expression_tree_walker 辅助函数传递一个 bms。    
	 * 在没有额外列的常见情况下避免分配内存。            
	 */
	if (!extraUpdatedCols)
		allUpdatedCols = updatedCols;
	else
		allUpdatedCols = bms_union(updatedCols, extraUpdatedCols);

	/*
	 * 如果有被索引表达式，我们就得稍微多费些功夫，  
	 * 但原则与之前相同：设法找出与已知被更新列重    
	 * 叠的列（即 Vars）。                        
	 *
	 * 若找到任何匹配的 Vars，就不为该索引传提示，否  
	 * 则传。 
	 */
	idxExprs = RelationGetIndexExpressions(indexRelation);
	hasexpression = index_expression_changed_walker((Node *) idxExprs,
													allUpdatedCols);
	list_free(idxExprs);
	if (extraUpdatedCols)
		bms_free(allUpdatedCols);

	if (hasexpression)
	{
		indexInfo->ii_IndexUnchanged = false;
		return false;
	}

	/*
	 * 有意不去考虑索引谓词。即便结果关系的                
	 * "被更新元组"没有对应的索引元组，我们也应      
	 * 当给出提示；这在部分索引下可能（只需满足     
	 * 常规条件）。  
	 */
	indexInfo->ii_IndexUnchanged = true;
	return true;
}

/*
 * index_unchanged_by_update() 的被索引表达式辅助   
 *
 * 函数。当在 allUpdatedCols 中找到 Var 时返回真。     
 */
static bool
index_expression_changed_walker(Node *node, Bitmapset *allUpdatedCols)
{
	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (bms_is_member(var->varattno - FirstLowInvalidHeapAttributeNumber,
						  allUpdatedCols))
		{
			/* Var 被更新过 -- 表明我们不应传提示      */
			return true;
		}

		/* 仍未找到不传该提示的理由              */
		return false;
	}

	return expression_tree_walker(node, index_expression_changed_walker,
								  (void *) allUpdatedCols);
}
