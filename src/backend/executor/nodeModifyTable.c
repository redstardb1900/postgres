/*-------------------------------------------------------------------------
 *
 * nodeModifyTable.c
 *	  处理 ModifyTable 节点的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeModifyTable.c
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitModifyTable - 初始化 ModifyTable 节点   
 *		ExecModifyTable		- 抽取节点的下一个元组       
 *		ExecEndModifyTable	- 关闭 ModifyTable 节点     
 *		ExecReScanModifyTable - 重扫 ModifyTable 节点  
 *
 *	 NOTES
 *		ModifyTable 节点从其 outerPlan 接收输入：可能是     
 *		INSERT 场景下是要插入的数据；UPDATE 与 MERGE     
 *		场景下是变更列的新值加行定位信息；DELETE 场景     
 *		下则只有行定位信息。     
 *
 *		要修改的关系可以是普通表、带 INSTEAD OF 触发器  
 *		的视图或外部表。前面的处理已把所有非 INSTEAD OF  
 *		方式的自动可更新视图都指向底层关系，因此这里  
 *		可以假定它不会以这种视图为修改目标。本节点 
 *		确实会处理 ri_WithCheckOptions，其中可能                
 *		含有来自那些自动可更新视图的                        
 *		表达式。                  
 *
 *		MERGE 对源关系与目标表执行连接；若存在任何
 *		WHEN NOT MATCHED 子句，那么该连接就是外连接。 
 *		此时未匹配元组的行定位信息均为 NULL，          
 *		且只能执行 INSERT。而对已匹配元组，则用行定   
 *		信息确定要 UPDATE 或 DELETE 的元组。当所有子句  
 *		是 WHEN MATCHED 时就用内连接，因此所有元组     
 *		都会包含行定位信息。                                      
 *
 *		如果查询指定了 RETURNING，那么 ModifyTable 会在每  
 *		完成一行插入、更新或删除之后返回一个 RETURNING 元
 *		组。之后必须再次调用才能继续操作。若无 RETURNING，
 *		节点内循环，直到工作全部完成，返回 NULL。  
 *		这避免了无用的调用/返回开销。（MERGE 并不支持    
 *		RETURNING。）        
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "commands/trigger.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"


typedef struct MTTargetRelLookup
{
	Oid			relationOid;	/* 哈希键，必须居首*/
	int			relationIndex;	/* rel 在 resultRelInfo[] 中的下标 */
} MTTargetRelLookup;

/*
 * ModifyTable 操作的上下文结构体，包含基本的执行状态 
 * 以及一些输出变量，由 ExecUpdateAct() 与              
 * ExecDeleteAct() 填充，用于向调用者报告动作结果。 
 */
typedef struct ModifyTableContext
{
	/* 操作状态    */
	ModifyTableState *mtstate;
	EPQState   *epqstate;
	EState	   *estate;

	/*
	 * 存放从 ModifyTable 的子计划取得的元组的槽。用于访
	 * 问那些不会被存储的 "junk" 列。             
	 */
	TupleTableSlot *planSlot;

	/* MERGE 专属   */
	MergeActionState *relaction;	/* MERGE 动作进行中    */

	/*
	 * 有关某元组被并发更改的信息，该元组正在被更     
	 * 新或删除            
	 */
	TM_FailureData tmfd;

	/*
	 * 执行跨分区 UPDATE 时，由 INSERT 的 RETURNING 子句投   
	 * 影出来的元组    
	 */
	TupleTableSlot *cpUpdateReturningSlot;
} ModifyTableContext;

/*
 * 包含 UPDATE 操作专属输出数据的上下文结构体。      
 */
typedef struct UpdateContext
{
	bool		updated;		/* UPDATE 是否发生过？  */
	bool		crossPartUpdate;	/* 是否为跨分区更新？      */
	TU_UpdateIndexes updateIndexes; /* 需要哪些索引更新？       */

	/*
	 * 在对最新元组版本执行 EvalPlanQual 之前要获取的    
	 * 锁模式         
	 */
	LockTupleMode lockmode;
} UpdateContext;


static void ExecBatchInsert(ModifyTableState *mtstate,
							ResultRelInfo *resultRelInfo,
							TupleTableSlot **slots,
							TupleTableSlot **planSlots,
							int numSlots,
							EState *estate,
							bool canSetTag);
static void ExecPendingInserts(EState *estate);
static void ExecCrossPartitionUpdateForeignKey(ModifyTableContext *context,
											   ResultRelInfo *sourcePartInfo,
											   ResultRelInfo *destPartInfo,
											   ItemPointer tupleid,
											   TupleTableSlot *oldslot,
											   TupleTableSlot *newslot);
static bool ExecOnConflictUpdate(ModifyTableContext *context,
								 ResultRelInfo *resultRelInfo,
								 ItemPointer conflictTid,
								 TupleTableSlot *excludedSlot,
								 bool canSetTag,
								 TupleTableSlot **returning);
static TupleTableSlot *ExecPrepareTupleRouting(ModifyTableState *mtstate,
											   EState *estate,
											   PartitionTupleRouting *proute,
											   ResultRelInfo *targetRelInfo,
											   TupleTableSlot *slot,
											   ResultRelInfo **partRelInfo);

static TupleTableSlot *ExecMerge(ModifyTableContext *context,
								 ResultRelInfo *resultRelInfo,
								 ItemPointer tupleid,
								 bool canSetTag);
static void ExecInitMerge(ModifyTableState *mtstate, EState *estate);
static bool ExecMergeMatched(ModifyTableContext *context,
							 ResultRelInfo *resultRelInfo,
							 ItemPointer tupleid,
							 bool canSetTag);
static void ExecMergeNotMatched(ModifyTableContext *context,
								ResultRelInfo *resultRelInfo,
								bool canSetTag);


/*
 * 校验 INSERT 将产生的元组是否与目标关系的 
 * 行类型相符          
 *
 * 这样做是为防范过期的计划。如果计划失效机制  
 * 运转正常，那么这里本不应出现失败，但稳妥总比      
 * 后悔好。注意本函数在已获得目标关系上的锁之后    
 * 才被调用，所以行类型不会在背后变化。       
 *
 * 计划输出用其目标列表来表示，因为这样更便于     
 * 处理被删除列的情形。          
 *
 * 以前 UPDATE 也使用这一校验，但等价的检查现          
 * 改在 ExecBuildUpdateProjection 做  
 */
static void
ExecCheckPlanOutput(Relation resultRel, List *targetList)
{
	TupleDesc	resultDesc = RelationGetDescr(resultRel);
	int			attno = 0;
	ListCell   *lc;

	foreach(lc, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Form_pg_attribute attr;

		Assert(!tle->resjunk);	/* 调用者已移除 junk 项       */

		if (attno >= resultDesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table row type and query-specified row type do not match"),
					 errdetail("Query has too many columns.")));
		attr = TupleDescAttr(resultDesc, attno);
		attno++;

		/*
		 * 这里的特例处理应与规划器的 expand_insert_targetlist   
		 */
		if (attr->attisdropped)
		{
			/*
			 * 对于被删除的列，无法检查 atttypid（其值可能  
			 * 是 0）。反正规划器多半已插入了 INT4 空值。   
			 * 坚持要求的只是*某个* NULL 常量。   
			 */
			if (!IsA(tle->expr, Const) ||
				!((Const *) tle->expr)->constisnull)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail("Query provides a value for a dropped column at ordinal position %d.",
								   attno)));
		}
		else if (attr->attgenerated)
		{
			/*
			 * 对于生成列，规划器已插入一个该列基类型的 
			 * 空值（避免可能违反域的非空约束）。但        
			 * 似乎不值得坚持恰好为该类型，因为空值与      
			 * 类型无关。所以与上文一样，只要求是*某个*   
			 * NULL 常量。                             
			 */
			if (!IsA(tle->expr, Const) ||
				!((Const *) tle->expr)->constisnull)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail("Query provides a value for a generated column at ordinal position %d.",
								   attno)));
		}
		else
		{
			/* 常规情况：类型须匹配 */
			if (exprType((Node *) tle->expr) != attr->atttypid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail("Table has type %s at ordinal position %d, but query expects %s.",
								   format_type_be(attr->atttypid),
								   attno,
								   format_type_be(exprType((Node *) tle->expr)))));
		}
	}
	if (attno != resultDesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("table row type and query-specified row type do not match"),
				 errdetail("Query has too few columns.")));
}

/*
 * ExecProcessReturning --- 计算 RETURNING 列表  
 *
 * resultRelInfo: 当前结果 rel  
 * tupleSlot: 保存实际被插入/更新/删除的元组的槽  
 * planSlot: 保存顶层子计划节点返回元组的槽  
 *
 * 注意：若 tupleSlot 为 NULL，FDW 应已提供 econtext 的扫描      
 * 元组。  
 *
 * 返回一个保存结果元组的槽   
 */
static TupleTableSlot *
ExecProcessReturning(ResultRelInfo *resultRelInfo,
					 TupleTableSlot *tupleSlot,
					 TupleTableSlot *planSlot)
{
	ProjectionInfo *projectReturning = resultRelInfo->ri_projectReturning;
	ExprContext *econtext = projectReturning->pi_exprContext;

	/* 把元组以及所需的连接变量提供给 ExecProject 使用  */
	if (tupleSlot)
		econtext->ecxt_scantuple = tupleSlot;
	econtext->ecxt_outertuple = planSlot;

	/*
	 * RETURNING 表达式可能引用 tableoid 列，因此要在  
	 * 求值之前重新初始化 tts_tableOid。      
	 */
	econtext->ecxt_scantuple->tts_tableOid =
		RelationGetRelid(resultRelInfo->ri_RelationDesc);

	/* 计算 RETURNING 表达式        */
	return ExecProject(projectReturning);
}

/*
 * ExecCheckTupleVisible -- 检查元组可见性  
 *
 * 如果基于另一个对 MVCC 快照不可见的元组来继续回避插    
 * 入（走推测性插入的替代路径），这与更高隔离级别      
 * 所提供的保证不一致。检查是否需要报告序列化          
 * 失败，必要时就如此处理。                                        
 */
static void
ExecCheckTupleVisible(EState *estate,
					  Relation rel,
					  TupleTableSlot *slot)
{
	if (!IsolationUsesXactSnapshot())
		return;

	if (!table_tuple_satisfies_snapshot(rel, slot, estate->es_snapshot))
	{
		Datum		xminDatum;
		TransactionId xmin;
		bool		isnull;

		xminDatum = slot_getsysattr(slot, MinTransactionIdAttributeNumber, &isnull);
		Assert(!isnull);
		xmin = DatumGetTransactionId(xminDatum);

		/*
		 * 如果冲突对象是自己事务插入的元组，则不应  
		 * 报告序列化失败，即便它对我们的快照不可见。  
		 * （例如，在同一条命令内为插入提议相互冲突 
		 * 的键。）                                                     
		 */
		if (!TransactionIdIsCurrentTransactionId(xmin))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not serialize access due to concurrent update")));
	}
}

/*
 * ExecCheckTIDVisible -- ExecCheckTupleVisible() 的便捷变体       
 */
static void
ExecCheckTIDVisible(EState *estate,
					ResultRelInfo *relinfo,
					ItemPointer tid,
					TupleTableSlot *tempSlot)
{
	Relation	rel = relinfo->ri_RelationDesc;

	/* 重复检查一次隔离级别    */
	if (!IsolationUsesXactSnapshot())
		return;

	if (!table_tuple_fetch_row_version(rel, tid, SnapshotAny, tempSlot))
		elog(ERROR, "failed to fetch conflicting tuple for ON CONFLICT");
	ExecCheckTupleVisible(estate, rel, tempSlot);
	ExecClearTuple(tempSlot);
}

/*
 * 初始化，以便为一个元组计算存储生成列    
 *
 * 本函数填充 resultRelInfo 的 ri_GeneratedExprsI/                   
 * ri_NumGeneratedNeededI 或 ri_GeneratedExprsU/                            
 * ri_NumGeneratedNeededU 字段，具体取决于 cmdtype。            
 *
 * 注意：通常，给定查询对每个结果关系只需要              
 * ri_GeneratedExprsI 与 ri_GeneratedExprsU 之一；但 MERGE          
 * 可能两者都需要，跨分区 UPDATE 亦然，因某分区可能  
 * 同为 UPDATE/INSERT。   
 */
void
ExecInitStoredGenerated(ResultRelInfo *resultRelInfo,
						EState *estate,
						CmdType cmdtype)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			natts = tupdesc->natts;
	ExprState **ri_GeneratedExprs;
	int			ri_NumGeneratedNeeded;
	Bitmapset  *updatedCols;
	MemoryContext oldContext;

	/* 若无生成列则无需做任何事  */
	if (!(tupdesc->constr && tupdesc->constr->has_generated_stored))
		return;

	/*
	 * 在 UPDATE 中，可以跳过计算那些不依赖任何 UPDATE      
	 * 目标列的生成列。但如果存在 BEFORE ROW UPDATE        
	 * 触发器，不能跳过，因为该触发器可能更改更多的  
	 * 列。  
	 */
	if (cmdtype == CMD_UPDATE &&
		!(rel->trigdesc && rel->trigdesc->trig_update_before_row))
		updatedCols = ExecGetUpdatedCols(resultRelInfo, estate);
	else
		updatedCols = NULL;

	/*
	 * 务必让这些数据结构建在每查询内存上下文中，  
	 * 以便它们能在整个查询期间存活。   
	 */
	oldContext = MemoryContextSwitchTo(estate->es_query_cxt);

	ri_GeneratedExprs = (ExprState **) palloc0(natts * sizeof(ExprState *));
	ri_NumGeneratedNeeded = 0;

	for (int i = 0; i < natts; i++)
	{
		if (TupleDescAttr(tupdesc, i)->attgenerated == ATTRIBUTE_GENERATED_STORED)
		{
			Expr	   *expr;

			/* 取出 GENERATED AS 表达式树       */
			expr = (Expr *) build_column_default(rel, i + 1);
			if (expr == NULL)
				elog(ERROR, "no generation expression found for column number %d of table \"%s\"",
					 i + 1, RelationGetRelationName(rel));

			/*
			 * 若这次更新有已知的目标列集合，则看看能否
			 * 跳过该计算。                 
			 */
			if (updatedCols)
			{
				Bitmapset  *attrs_used = NULL;

				pull_varattnos((Node *) expr, 1, &attrs_used);

				if (!bms_overlap(updatedCols, attrs_used))
					continue;	/* 无需更新此列          */
			}

			/* 未能跳过，故准备该表达式以供执行 */
			ri_GeneratedExprs[i] = ExecPrepareExpr(expr, estate);
			ri_NumGeneratedNeeded++;

			/* UPDATE 时在 resultRelInfo->ri_extraUpdatedCols 中标记   */
			if (cmdtype == CMD_UPDATE)
				resultRelInfo->ri_extraUpdatedCols =
					bms_add_member(resultRelInfo->ri_extraUpdatedCols,
								   i + 1 - FirstLowInvalidHeapAttributeNumber);
		}
	}

	/* 保存到相应的一组字段中 */
	if (cmdtype == CMD_UPDATE)
	{
		/* 勿调用两次  */
		Assert(resultRelInfo->ri_GeneratedExprsU == NULL);

		resultRelInfo->ri_GeneratedExprsU = ri_GeneratedExprs;
		resultRelInfo->ri_NumGeneratedNeededU = ri_NumGeneratedNeeded;
	}
	else
	{
		/* 勿调用两次  */
		Assert(resultRelInfo->ri_GeneratedExprsI == NULL);

		resultRelInfo->ri_GeneratedExprsI = ri_GeneratedExprs;
		resultRelInfo->ri_NumGeneratedNeededI = ri_NumGeneratedNeeded;
	}

	MemoryContextSwitchTo(oldContext);
}

/*
 * 为一个元组计算存储生成列        
 */
void
ExecComputeStoredGenerated(ResultRelInfo *resultRelInfo,
						   EState *estate, TupleTableSlot *slot,
						   CmdType cmdtype)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			natts = tupdesc->natts;
	ExprContext *econtext = GetPerTupleExprContext(estate);
	ExprState **ri_GeneratedExprs;
	MemoryContext oldContext;
	Datum	   *values;
	bool	   *nulls;

	/* 若非如此，本函数不应被调用     */
	Assert(tupdesc->constr && tupdesc->constr->has_generated_stored);

	/*
	 * 若尚未初始化这些表达式则先初始化，并检查是否   
	 * 可提前退出，因为无需计算任何内容。 
	 */
	if (cmdtype == CMD_UPDATE)
	{
		if (resultRelInfo->ri_GeneratedExprsU == NULL)
			ExecInitStoredGenerated(resultRelInfo, estate, cmdtype);
		if (resultRelInfo->ri_NumGeneratedNeededU == 0)
			return;
		ri_GeneratedExprs = resultRelInfo->ri_GeneratedExprsU;
	}
	else
	{
		if (resultRelInfo->ri_GeneratedExprsI == NULL)
			ExecInitStoredGenerated(resultRelInfo, estate, cmdtype);
		/* 有了前面的 Assert，提前退出不可能  */
		Assert(resultRelInfo->ri_NumGeneratedNeededI > 0);
		ri_GeneratedExprs = resultRelInfo->ri_GeneratedExprsI;
	}

	oldContext = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

	values = palloc(sizeof(*values) * natts);
	nulls = palloc(sizeof(*nulls) * natts);

	slot_getallattrs(slot);
	memcpy(nulls, slot->tts_isnull, sizeof(*nulls) * natts);

	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		if (ri_GeneratedExprs[i])
		{
			Datum		val;
			bool		isnull;

			Assert(attr->attgenerated == ATTRIBUTE_GENERATED_STORED);

			econtext->ecxt_scantuple = slot;

			val = ExecEvalExpr(ri_GeneratedExprs[i], econtext, &isnull);

			/*
			 * 必须复制一份 val，因为我们无法保证传引用     
			 * Datum 的内存位于何处。                  
			 */
			if (!isnull)
				val = datumCopy(val, attr->attbyval, attr->attlen);

			values[i] = val;
			nulls[i] = isnull;
		}
		else
		{
			if (!nulls[i])
				values[i] = datumCopy(slot->tts_values[i], attr->attbyval, attr->attlen);
		}
	}

	ExecClearTuple(slot);
	memcpy(slot->tts_values, values, sizeof(*values) * natts);
	memcpy(slot->tts_isnull, nulls, sizeof(*nulls) * natts);
	ExecStoreVirtualTuple(slot);
	ExecMaterializeSlot(slot);

	MemoryContextSwitchTo(oldContext);
}

/*
 * ExecInitInsertProjection
 *		对 INSERT 元组的投影数据做一次性初始化。        
 *
 * INSERT 查询可能需要投影来过滤掉 tlist 中的 junk 属性。    
 *
 * 也便于校验 INSERT 输出是否匹配目标   
 * 表。                                       
 */
static void
ExecInitInsertProjection(ModifyTableState *mtstate,
						 ResultRelInfo *resultRelInfo)
{
	ModifyTable *node = (ModifyTable *) mtstate->ps.plan;
	Plan	   *subplan = outerPlan(node);
	EState	   *estate = mtstate->ps.state;
	List	   *insertTargetList = NIL;
	bool		need_projection = false;
	ListCell   *l;

	/* 抽取子计划结果 tlist 的非 junk 列。          */
	foreach(l, subplan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (!tle->resjunk)
			insertTargetList = lappend(insertTargetList, tle);
		else
			need_projection = true;
	}

	/*
	 * 去除 junk 后的列表必须产生适合结果关系的      
	 * 元组。
	 */
	ExecCheckPlanOutput(resultRelInfo->ri_RelationDesc, insertTargetList);

	/* 需要一个与表格式相匹配的槽。     */
	resultRelInfo->ri_newTupleSlot =
		table_slot_create(resultRelInfo->ri_RelationDesc,
						  &estate->es_tupleTable);

	/* 按需构建 ProjectionInfo（多半不需要）。 */
	if (need_projection)
	{
		TupleDesc	relDesc = RelationGetDescr(resultRelInfo->ri_RelationDesc);

		/* 需要一个表达式上下文来做投影      */
		if (mtstate->ps.ps_ExprContext == NULL)
			ExecAssignExprContext(estate, &mtstate->ps);

		resultRelInfo->ri_projectNew =
			ExecBuildProjectionInfo(insertTargetList,
									mtstate->ps.ps_ExprContext,
									resultRelInfo->ri_newTupleSlot,
									&mtstate->ps,
									relDesc);
	}

	resultRelInfo->ri_projectNewInfoValid = true;
}

/*
 * ExecInitUpdateProjection
 *		对 UPDATE 元组的投影数据做一次性初始化。        
 *
 * UPDATE 总是需要投影，因为 (1) 总是存在一些 junk        
 * 属性；(2) 可能需要把未更新列的值从旧元组合并      
 * 到最终元组中。在 UPDATE 里，来自子计划的元组只      
 * 包含变更列的新值，外加 junk 属性中的行标识          
 * 信息。                       
 *
 * 对任一给定的结果关系而言这都是"一次性"的，           
 * 但在继承 UPDATE 过程中可能触及多个结果关系，         
 * 每个都需自己的投影，因列顺序可能不同。 
 *
 * 这里也便于校验 UPDATE 的输出是否匹配目标              
 * 表（ExecBuildUpdateProjection 会做这件事）。          
 */
static void
ExecInitUpdateProjection(ModifyTableState *mtstate,
						 ResultRelInfo *resultRelInfo)
{
	ModifyTable *node = (ModifyTable *) mtstate->ps.plan;
	Plan	   *subplan = outerPlan(node);
	EState	   *estate = mtstate->ps.state;
	TupleDesc	relDesc = RelationGetDescr(resultRelInfo->ri_RelationDesc);
	int			whichrel;
	List	   *updateColnos;

	/*
	 * 通常 mt_lastResultIndex 与目标关系相匹配。如果           
	 * 碰巧不匹配，则可用整数除法算出该下标。      
	 */
	whichrel = mtstate->mt_lastResultIndex;
	if (resultRelInfo != mtstate->resultRelInfo + whichrel)
	{
		whichrel = resultRelInfo - mtstate->resultRelInfo;
		Assert(whichrel >= 0 && whichrel < mtstate->mt_nrels);
	}

	updateColnos = (List *) list_nth(node->updateColnosLists, whichrel);

	/*
	 * 对于 UPDATE，我们用旧元组来填充子计划产出的          
	 * 元组中缺失的值，从而得到新元组。需要两个          
	 * 均匹配表期望格式的槽。   
	 */
	resultRelInfo->ri_oldTupleSlot =
		table_slot_create(resultRelInfo->ri_RelationDesc,
						  &estate->es_tupleTable);
	resultRelInfo->ri_newTupleSlot =
		table_slot_create(resultRelInfo->ri_RelationDesc,
						  &estate->es_tupleTable);

	/* 需要一个表达式上下文来做投影      */
	if (mtstate->ps.ps_ExprContext == NULL)
		ExecAssignExprContext(estate, &mtstate->ps);

	resultRelInfo->ri_projectNew =
		ExecBuildUpdateProjection(subplan->targetlist,
								  false,	/* 求值已由子计划完成*/
								  updateColnos,
								  relDesc,
								  mtstate->ps.ps_ExprContext,
								  resultRelInfo->ri_newTupleSlot,
								  &mtstate->ps);

	resultRelInfo->ri_projectNewInfoValid = true;
}

/*
 * ExecGetInsertNewTuple
 *		本函数准备一个可插入给定结果关系的"新"元组， 
 *		做法是移除计划输出元组中的所有 junk 列，        
 *		并（如有必要）把元组强转成正确的元组格式。 
 */
static TupleTableSlot *
ExecGetInsertNewTuple(ResultRelInfo *relinfo,
					  TupleTableSlot *planSlot)
{
	ProjectionInfo *newProj = relinfo->ri_projectNew;
	ExprContext *econtext;

	/*
	 * 如果没有要做的投影，只需确保槽的类型适合          
	 * 目标关系。若 planSlot 本身类型正确，可以直接       
	 * 使用它，否则把数据复制进 ri_newTupleSlot。   
	 */
	if (newProj == NULL)
	{
		if (relinfo->ri_newTupleSlot->tts_ops != planSlot->tts_ops)
		{
			ExecCopySlot(relinfo->ri_newTupleSlot, planSlot);
			return relinfo->ri_newTupleSlot;
		}
		else
			return planSlot;
	}

	/*
	 * 否则执行投影；由于投影的输出槽是 ri_newTupleSlot，    
	 * 这也能解决槽类型问题。   
	 *
	 * 注意：目前这是死代码，因为 INSERT 场景根本           
	 * 不会收到任何 junk 列，因此无需做投影。    
	 */
	econtext = newProj->pi_exprContext;
	econtext->ecxt_outertuple = planSlot;
	return ExecProject(newProj);
}

/*
 * ExecGetUpdateNewTuple
 *		本函数把 UPDATE 子计划的输出元组（其中包含        
 *		变更列值）与来自旧元组的未变更列组合起      
 *		来，得到一个"新"元组。 
 *
 * 子计划元组中可能还含有 junk 列，它们会被忽略。   
 * 注意该投影还确保我们得到类型正确的槽。              
 */
TupleTableSlot *
ExecGetUpdateNewTuple(ResultRelInfo *relinfo,
					  TupleTableSlot *planSlot,
					  TupleTableSlot *oldSlot)
{
	ProjectionInfo *newProj = relinfo->ri_projectNew;
	ExprContext *econtext;

	/* 额外多用几个 Assert 来防范外部调用者         */
	Assert(relinfo->ri_projectNewInfoValid);
	Assert(planSlot != NULL && !TTS_EMPTY(planSlot));
	Assert(oldSlot != NULL && !TTS_EMPTY(oldSlot));

	econtext = newProj->pi_exprContext;
	econtext->ecxt_outertuple = planSlot;
	econtext->ecxt_scantuple = oldSlot;
	return ExecProject(newProj);
}

/* ----------------------------------------------------------------
 *		ExecInsert
 *
 *		对 INSERT 而言，我们必须把元组插入目标关系     
 *		（或其分区）中，并向各索引关系插入相应的       
 *		元组。 
 *
 *		slot 中包含待存储的新元组值。      
 *
 *		有 RETURNING 结果就返回，否则 NULL。  
 *		*inserted_tuple 是实际被插入的元组；           
 *		*insert_destrel 是其被插入其中的关系。     
 *		两者仅成功时才设置。
 *
 *		本函数可能改变 mtstate->mt_transition_capture 里    
 *		当前活跃的元组转换映射，调用者须小心保存旧 
 *		值，以免失去对它的追踪。                
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecInsert(ModifyTableContext *context,
		   ResultRelInfo *resultRelInfo,
		   TupleTableSlot *slot,
		   bool canSetTag,
		   TupleTableSlot **inserted_tuple,
		   ResultRelInfo **insert_destrel)
{
	ModifyTableState *mtstate = context->mtstate;
	EState	   *estate = context->estate;
	Relation	resultRelationDesc;
	List	   *recheckIndexes = NIL;
	TupleTableSlot *planSlot = context->planSlot;
	TupleTableSlot *result = NULL;
	TransitionCaptureState *ar_insert_trig_tcs;
	ModifyTable *node = (ModifyTable *) mtstate->ps.plan;
	OnConflictAction onconflict = node->onConflictAction;
	PartitionTupleRouting *proute = mtstate->mt_partition_tuple_routing;
	MemoryContext oldContext;

	/*
	 * 如果输入的结果关系是分区表，则找到要插入      
	 * 该元组的叶子分区。        
	 */
	if (proute)
	{
		ResultRelInfo *partRelInfo;

		slot = ExecPrepareTupleRouting(mtstate, estate, proute,
									   resultRelInfo, slot,
									   &partRelInfo);
		resultRelInfo = partRelInfo;
	}

	ExecMaterializeSlot(slot);

	resultRelationDesc = resultRelInfo->ri_RelationDesc;

	/*
	 * 如果尚未打开该表的索引，现在就打开，以便        
	 * 能为被插入的元组添加新索引条目。 
	 */
	if (resultRelationDesc->rd_rel->relhasindex &&
		resultRelInfo->ri_IndexRelationDescs == NULL)
		ExecOpenIndices(resultRelInfo, onconflict != ONCONFLICT_NONE);

	/*
	 * BEFORE ROW INSERT 触发器
	 *
	 * 注意：在 INSERT ... ON CONFLICT 语句中，我们会为每      
	 * 一次尝试的插入都触发 BEFORE ROW 触发器。我们不  
	 * 能在触发这些触发器之前检查约束违反，因为        
	 * 它们可能更改待插入的值。而且它们能运行任意       
	 * 用户代码，其副作用无法仅靠不插入元组来取消。
	 */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_insert_before_row)
	{
		/* 冲刷所有待处理的插入，让行对触发器可见      */
		if (estate->es_insert_pending_result_relations != NIL)
			ExecPendingInserts(estate);

		if (!ExecBRInsertTriggers(estate, resultRelInfo, slot))
			return NULL;		/* 无操作    */
	}

	/* INSTEAD OF ROW INSERT 触发器*/
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_insert_instead_row)
	{
		if (!ExecIRInsertTriggers(estate, resultRelInfo, slot))
			return NULL;		/* 无操作    */
	}
	else if (resultRelInfo->ri_FdwRoutine)
	{
		/*
		 * GENERATED 表达式可能引用 tableoid 列，因此要     
		 * 在求值之前（重新）初始化 tts_tableOid。
		 */
		slot->tts_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);

		/*
		 * 计算存储生成列           
		 */
		if (resultRelationDesc->rd_att->constr &&
			resultRelationDesc->rd_att->constr->has_generated_stored)
			ExecComputeStoredGenerated(resultRelInfo, estate, slot,
									   CMD_INSERT);

		/*
		 * 如果 FDW 支持批量操作且请求了批量，则累积        
		 * 各行并分批插入。否则使用逐行插入。                
		 */
		if (resultRelInfo->ri_BatchSize > 1)
		{
			bool		flushed = false;

			/*
			 * 当达到所需的批量大小后，就执行实际   
			 * 插入。 
			 */
			if (resultRelInfo->ri_NumSlots == resultRelInfo->ri_BatchSize)
			{
				ExecBatchInsert(mtstate, resultRelInfo,
								resultRelInfo->ri_Slots,
								resultRelInfo->ri_PlanSlots,
								resultRelInfo->ri_NumSlots,
								estate, canSetTag);
				flushed = true;
			}

			oldContext = MemoryContextSwitchTo(estate->es_query_cxt);

			if (resultRelInfo->ri_Slots == NULL)
			{
				resultRelInfo->ri_Slots = palloc(sizeof(TupleTableSlot *) *
												 resultRelInfo->ri_BatchSize);
				resultRelInfo->ri_PlanSlots = palloc(sizeof(TupleTableSlot *) *
													 resultRelInfo->ri_BatchSize);
			}

			/*
			 * 初始化批量槽。我们不知道需要多少个槽，    
			 * 因此随批量增长逐步初始化，并让其跨批量  
			 * 保持不变。为缓解资源所有者处理多引用对象
			 * 时的低效问题（例如许多槽都引用同一个元组
			 * 描述符的情形），为每个槽复制相应的元组    
			 * 描述符。                                   
			 */
			if (resultRelInfo->ri_NumSlots >= resultRelInfo->ri_NumSlotsInitialized)
			{
				TupleDesc	tdesc = CreateTupleDescCopy(slot->tts_tupleDescriptor);
				TupleDesc	plan_tdesc =
					CreateTupleDescCopy(planSlot->tts_tupleDescriptor);

				resultRelInfo->ri_Slots[resultRelInfo->ri_NumSlots] =
					MakeSingleTupleTableSlot(tdesc, slot->tts_ops);

				resultRelInfo->ri_PlanSlots[resultRelInfo->ri_NumSlots] =
					MakeSingleTupleTableSlot(plan_tdesc, planSlot->tts_ops);

				/* 记住我们已初始化了多少个批量槽*/
				resultRelInfo->ri_NumSlotsInitialized++;
			}

			ExecCopySlot(resultRelInfo->ri_Slots[resultRelInfo->ri_NumSlots],
						 slot);

			ExecCopySlot(resultRelInfo->ri_PlanSlots[resultRelInfo->ri_NumSlots],
						 planSlot);

			/*
			 * 如果这些是存入缓冲区的首批元组，就把目标
			 * 关系和 mtstate 分别加进   
			 * es_insert_pending_result_relations 和
			 * es_insert_pending_modifytables 列表，除非上面的     
			 * 冲刷已执行过；那种情况下它们早已被加入 
			 * 列表，因此无须再这样                              
			 * 做  
			 */
			if (resultRelInfo->ri_NumSlots == 0 && !flushed)
			{
				Assert(!list_member_ptr(estate->es_insert_pending_result_relations,
										resultRelInfo));
				estate->es_insert_pending_result_relations =
					lappend(estate->es_insert_pending_result_relations,
							resultRelInfo);
				estate->es_insert_pending_modifytables =
					lappend(estate->es_insert_pending_modifytables, mtstate);
			}
			Assert(list_member_ptr(estate->es_insert_pending_result_relations,
								   resultRelInfo));

			resultRelInfo->ri_NumSlots++;

			MemoryContextSwitchTo(oldContext);

			return NULL;
		}

		/*
		 * 插入外部表：交给 FDW 处理         
		 */
		slot = resultRelInfo->ri_FdwRoutine->ExecForeignInsert(estate,
															   resultRelInfo,
															   slot,
															   planSlot);

		if (slot == NULL)		/* 无操作    */
			return NULL;

		/*
		 * AFTER ROW 触发器或 RETURNING 表达式可能引用         
		 * tableoid 列，因此要在求值之前（重新）初始化      
		 * tts_tableOid。（这涵盖了 FDW 替换槽的情形。）   
		 */
		slot->tts_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);
	}
	else
	{
		WCOKind		wco_kind;

		/*
		 * 约束和 GENERATED 表达式可能引用 tableoid 列，         
		 * 因此要在求值之前（重新）初始化 tts_tableOid。  
		 */
		slot->tts_tableOid = RelationGetRelid(resultRelationDesc);

		/*
		 * 计算存储生成列           
		 */
		if (resultRelationDesc->rd_att->constr &&
			resultRelationDesc->rd_att->constr->has_generated_stored)
			ExecComputeStoredGenerated(resultRelInfo, estate, slot,
									   CMD_INSERT);

		/*
		 * 检查 RLS WITH CHECK 策略。   
		 *
		 * 通常应检查 INSERT 策略。但如果该插入是把元       
		 * 组移到新分区的分区键更新的结果，那就应改  
		 * 查 UPDATE 策略，因为我们执行的是定义在目标       
		 * 表上的策略，而不是定义在子分区上的          
		 * 那些策略。                 
		 *
		 * 如果正在运行 MERGE，则要看正在执行的动作，       
		 * 以此得知我们是在对分区表做 INSERT 还是 UPDATE。 
		 */
		if (mtstate->operation == CMD_UPDATE)
			wco_kind = WCO_RLS_UPDATE_CHECK;
		else if (mtstate->operation == CMD_MERGE)
			wco_kind = (context->relaction->mas_action->commandType == CMD_UPDATE) ?
				WCO_RLS_UPDATE_CHECK : WCO_RLS_INSERT_CHECK;
		else
			wco_kind = WCO_RLS_INSERT_CHECK;

		/*
		 * ExecWithCheckOptions() 会跳过所有与此时要找的           
		 * 类型不同的 WCO。           
		 */
		if (resultRelInfo->ri_WithCheckOptions != NIL)
			ExecWithCheckOptions(wco_kind, resultRelInfo, slot, estate);

		/*
		 * 检查元组的约束。           
		 */
		if (resultRelationDesc->rd_att->constr)
			ExecConstraints(resultRelInfo, slot, estate);

		/*
		 * 另外，若存在分区约束，还要对照它检查元组；   
		 * 但如果元组是经路由到达此处，且该分区上没       
		 * 有定义 BR 触发器，就无须检查。       
		 */
		if (resultRelationDesc->rd_rel->relispartition &&
			(resultRelInfo->ri_RootResultRelInfo == NULL ||
			 (resultRelInfo->ri_TrigDesc &&
			  resultRelInfo->ri_TrigDesc->trig_insert_before_row)))
			ExecPartitionCheck(resultRelInfo, slot, estate, true);

		if (onconflict != ONCONFLICT_NONE && resultRelInfo->ri_NumIndices > 0)
		{
			/* 执行一次推测性插入。   */
			uint32		specToken;
			ItemPointerData conflictTid;
			bool		specConflict;
			List	   *arbiterIndexes;

			arbiterIndexes = resultRelInfo->ri_onConflictArbiterIndexes;

			/*
			 * 首先做一次非结论性的冲突检查。 
			 *
			 * 此时我们尚未持有任何锁，所以这并不能保证   
			 * 稍后的插入不会冲突。但这能避免在大量执行  
			 * 相互冲突的 INSERT ON CONFLICT 语句时，留下大     
			 * 量被取消的推测性插入。              
			 *
			 * 如果在后面发现冲突，无论是在预检查期间，
			 * 或在推测插入元组后复查时回到这里，         
			 * 最好允许中断，以防某个 bug 使这里变成无限   
			 * 循环。             
			 */
	vlock:
			CHECK_FOR_INTERRUPTS();
			specConflict = false;
			if (!ExecCheckIndexConstraints(resultRelInfo, slot, estate,
										   &conflictTid, arbiterIndexes))
			{
				/* 找到已提交的冲突元组 */
				if (onconflict == ONCONFLICT_UPDATE)
				{
					/*
					 * 若是 ON CONFLICT DO UPDATE，则执行其中的   
					 * UPDATE 部分。若该 UPDATE 因其他并发          
					 * 的 UPDATE/DELETE 冲突元组而失败，须重   
					 * 试。
					 */
					TupleTableSlot *returning = NULL;

					if (ExecOnConflictUpdate(context, resultRelInfo,
											 &conflictTid, slot, canSetTag,
											 &returning))
					{
						InstrCountTuples2(&mtstate->ps, 1);
						return returning;
					}
					else
						goto vlock;
				}
				else
				{
					/*
					 * 若是 ON CONFLICT DO NOTHING，则什么都不做。  
					 * 但在更高隔离级别下，须校验其对执行器 
					 * 的 MVCC 快照是否可见。      
					 *
					 * 用 ExecGetReturningSlot() 来存放该元组以供复 
					 * 查不优雅，但不能简单用输入槽，因为  
					 * 其类型可能不兼容。而 DO NOTHING 情形下    
					 * ExecGetReturningSlot() 无冲突用途  
					 * 因此可放心用它存放待复检元组。   
					 */
					Assert(onconflict == ONCONFLICT_NOTHING);
					ExecCheckTIDVisible(estate, resultRelInfo, &conflictTid,
										ExecGetReturningSlot(estate, resultRelInfo));
					InstrCountTuples2(&mtstate->ps, 1);
					return NULL;
				}
			}

			/*
			 * 在正式开始插入之前，取得我们的"推测性   
			 * 插入锁"。其他人可以利用它来等待我们决定    
			 * 是否继续执行插入，而不必去等               
			 * 待整个事务完成。                      
			 */
			specToken = SpeculativeInsertionLockAcquire(GetCurrentTransactionId());

			/* 插入该元组，附带推测性令牌      */
			table_tuple_insert_speculative(resultRelationDesc, slot,
										   estate->es_output_cid,
										   0,
										   NULL,
										   specToken);

			/* 为元组插入索引条目    */
			recheckIndexes = ExecInsertIndexTuples(resultRelInfo,
												   slot, estate, false, true,
												   &specConflict,
												   arbiterIndexes,
												   false);

			/* 相应地调整该元组的状态    */
			table_tuple_complete_speculative(resultRelationDesc, slot,
											 specToken, !specConflict);

			/*
			 * 唤醒所有等待我们决定的人。他们会重新检查
			 * 该元组，发现它不再是推测性的，然后像对待   
			 * 常规插入的元组那样等待我们的 XID。而如果我  
			 * 们已杀死了该元组，他们会看到它已死，当作它
			 * 从未存在过。      
			 */
			SpeculativeInsertionLockRelease(GetCurrentTransactionId());

			/*
			 * 如果曾有冲突，就从头开始。我们会再次执   
			 * 行预检查，这次会找到那个冲突元组（除非它  
			 * 在到达之前就已中止）。      
			 */
			if (specConflict)
			{
				list_free(recheckIndexes);
				goto vlock;
			}

			/* 既然没有插入冲突，我们就完成了     */
		}
		else
		{
			/* 正常插入该元组     */
			table_tuple_insert(resultRelationDesc, slot,
							   estate->es_output_cid,
							   0, NULL);

			/* 为元组插入索引条目    */
			if (resultRelInfo->ri_NumIndices > 0)
				recheckIndexes = ExecInsertIndexTuples(resultRelInfo,
													   slot, estate, false,
													   false, NULL, NIL,
													   false);
		}
	}

	if (canSetTag)
		(estate->es_processed)++;

	/*
	 * 若此次插入源于分区键更新把元组移到了新分区，   
	 * 则把该行放入转换 NEW TABLE（若存在的话）。           
	 * 对 DELETE 和 INSERT 需要分开处理，                         
	 * 因为二者发生在不同的表上。 
	 */
	ar_insert_trig_tcs = mtstate->mt_transition_capture;
	if (mtstate->operation == CMD_UPDATE && mtstate->mt_transition_capture
		&& mtstate->mt_transition_capture->tcs_update_new_table)
	{
		ExecARUpdateTriggers(estate, resultRelInfo,
							 NULL, NULL,
							 NULL,
							 NULL,
							 slot,
							 NULL,
							 mtstate->mt_transition_capture,
							 false);

		/*
		 * NEW TABLE 行已被捕获，须确保下方触发的任何   
		 * AR INSERT 触发器不会再次捕获它。         
		 */
		ar_insert_trig_tcs = NULL;
	}

	/* AFTER ROW INSERT 触发器*/
	ExecARInsertTriggers(estate, resultRelInfo, slot, recheckIndexes,
						 ar_insert_trig_tcs);

	list_free(recheckIndexes);

	/*
	 * 检查来自父视图的 WITH CHECK OPTION 约束。按照 SQL    
	 * 规范要求，我们必须在测完所有约束和唯一性    
	 * 冲突之后再执行此项检查，因此在把记录实际插入   
	 * 堆和所有索引之后才进行。 
	 *
	 * 如果发现违反，ExecWithCheckOptions 会 elog(ERROR)，         
	 * 因此违反 WITH CHECK OPTION 的元组将永远不可见。  
	 *
	 * ExecWithCheckOptions() 会跳过所有此时并非我们所           
	 * 要查找类型的 WCO。     
	 */
	if (resultRelInfo->ri_WithCheckOptions != NIL)
		ExecWithCheckOptions(WCO_VIEW_CHECK, resultRelInfo, slot, estate);

	/* 如有则处理 RETURNING    */
	if (resultRelInfo->ri_projectReturning)
		result = ExecProcessReturning(resultRelInfo, slot, planSlot);

	if (inserted_tuple)
		*inserted_tuple = slot;
	if (insert_destrel)
		*insert_destrel = resultRelInfo;

	return result;
}

/* ----------------------------------------------------------------
 *		ExecBatchInsert
 *
 *		以高效的方式插入多个元组。    
 *		当前此函数处理向外表的插入，且该外表没有  
 *		RETURNING 子句 
 * ----------------------------------------------------------------
 */
static void
ExecBatchInsert(ModifyTableState *mtstate,
				ResultRelInfo *resultRelInfo,
				TupleTableSlot **slots,
				TupleTableSlot **planSlots,
				int numSlots,
				EState *estate,
				bool canSetTag)
{
	int			i;
	int			numInserted = numSlots;
	TupleTableSlot *slot = NULL;
	TupleTableSlot **rslots;

	/*
	 * 插入外表：交给 FDW 处理            
	 */
	rslots = resultRelInfo->ri_FdwRoutine->ExecForeignBatchInsert(estate,
																  resultRelInfo,
																  slots,
																  planSlots,
																  &numInserted);

	for (i = 0; i < numInserted; i++)
	{
		slot = rslots[i];

		/*
		 * AFTER ROW 触发器可能引用 tableoid 列，因此在  
		 * 求值前要（重新）初始化 tts_tableOid。   
		 */
		slot->tts_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);

		/* AFTER ROW INSERT 触发器*/
		ExecARInsertTriggers(estate, resultRelInfo, slot, NIL,
							 mtstate->mt_transition_capture);

		/*
		 * 检查来自父视图的 WITH CHECK OPTION 约束。               
		 * 详见 ExecInsert。  
		 */
		if (resultRelInfo->ri_WithCheckOptions != NIL)
			ExecWithCheckOptions(WCO_VIEW_CHECK, resultRelInfo, slot, estate);
	}

	if (canSetTag && numInserted > 0)
		estate->es_processed += numInserted;

	/* 清理所有槽，为下一批做好准备       */
	for (i = 0; i < numSlots; i++)
	{
		ExecClearTuple(slots[i]);
		ExecClearTuple(planSlots[i]);
	}
	resultRelInfo->ri_NumSlots = 0;
}

/*
 * ExecPendingInserts -- 把所有待处理插入刷入外表             
 */
static void
ExecPendingInserts(EState *estate)
{
	ListCell   *l1,
			   *l2;

	forboth(l1, estate->es_insert_pending_result_relations,
			l2, estate->es_insert_pending_modifytables)
	{
		ResultRelInfo *resultRelInfo = (ResultRelInfo *) lfirst(l1);
		ModifyTableState *mtstate = (ModifyTableState *) lfirst(l2);

		Assert(mtstate);
		ExecBatchInsert(mtstate, resultRelInfo,
						resultRelInfo->ri_Slots,
						resultRelInfo->ri_PlanSlots,
						resultRelInfo->ri_NumSlots,
						estate, mtstate->canSetTag);
	}

	list_free(estate->es_insert_pending_result_relations);
	list_free(estate->es_insert_pending_modifytables);
	estate->es_insert_pending_result_relations = NIL;
	estate->es_insert_pending_modifytables = NIL;
}

/*
 * ExecDeletePrologue -- ExecDelete 的子例程  
 *
 * 为 DELETE 准备执行器状态。实际上，这里唯一要做的      
 * 就是执行 BEFORE ROW 触发器。若其中某触发器使删除成为  
 * 无操作则返回 false，否则 true。  
 */
static bool
ExecDeletePrologue(ModifyTableContext *context, ResultRelInfo *resultRelInfo,
				   ItemPointer tupleid, HeapTuple oldtuple,
				   TupleTableSlot **epqreturnslot, TM_Result *result)
{
	if (result)
		*result = TM_Ok;

	/* BEFORE ROW DELETE 触发器*/
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_delete_before_row)
	{
		/* 刷出所有待处理插入，让行对触发器可见         */
		if (context->estate->es_insert_pending_result_relations != NIL)
			ExecPendingInserts(context->estate);

		return ExecBRDeleteTriggers(context->estate, context->epqstate,
									resultRelInfo, tupleid, oldtuple,
									epqreturnslot, result, &context->tmfd);
	}

	return true;
}

/*
 * ExecDeleteAct -- ExecDelete 的子例程  
 *
 * 实际从普通表中删除元组。         
 *
 * 必要时由调用者负责执行 EvalPlanQual。     
 */
static TM_Result
ExecDeleteAct(ModifyTableContext *context, ResultRelInfo *resultRelInfo,
			  ItemPointer tupleid, bool changingPart)
{
	EState	   *estate = context->estate;

	return table_tuple_delete(resultRelInfo->ri_RelationDesc, tupleid,
							  estate->es_output_cid,
							  estate->es_snapshot,
							  estate->es_crosscheck_snapshot,
							  true /* 等待提交    */ ,
							  &context->tmfd,
							  changingPart);
}

/*
 * ExecDeleteEpilogue -- ExecDelete 的子例程  
 *
 * 元组删除的收尾步骤；此函数调用 AFTER FOR EACH ROW          
 * 触发器；若该删除是跨分区元组移动的一部分，也包括
 * UPDATE 触发器。        
 */
static void
ExecDeleteEpilogue(ModifyTableContext *context, ResultRelInfo *resultRelInfo,
				   ItemPointer tupleid, HeapTuple oldtuple, bool changingPart)
{
	ModifyTableState *mtstate = context->mtstate;
	EState	   *estate = context->estate;
	TransitionCaptureState *ar_delete_trig_tcs;

	/*
	 * 若此次删除源于分区键更新把元组移到了新分区，   
	 * 则把该行放入转换 OLD TABLE（若存在的话）。           
	 * 对 DELETE 和 INSERT 需要分开处理，                         
	 * 因为二者发生在不同的表上。 
	 */
	ar_delete_trig_tcs = mtstate->mt_transition_capture;
	if (mtstate->operation == CMD_UPDATE && mtstate->mt_transition_capture &&
		mtstate->mt_transition_capture->tcs_update_old_table)
	{
		ExecARUpdateTriggers(estate, resultRelInfo,
							 NULL, NULL,
							 tupleid, oldtuple,
							 NULL, NULL, mtstate->mt_transition_capture,
							 false);

		/*
		 * OLD TABLE 行已被捕获，须确保下方触发的任何   
		 * AR DELETE 触发器不会再次捕获它。         
		 */
		ar_delete_trig_tcs = NULL;
	}

	/* AFTER ROW DELETE 触发器*/
	ExecARDeleteTriggers(estate, resultRelInfo, tupleid, oldtuple,
						 ar_delete_trig_tcs, changingPart);
}

/* ----------------------------------------------------------------
 *		ExecDelete
 *
 *		DELETE 与 UPDATE 类似，只是我们删除元组，       
 *		并且不需要修改索引。 
 *
 *		从表中删除时，tupleid 标识要删除的元组，              
 *		oldtuple 为 NULL。通过视图 INSTEAD OF 触发器删除时，  
 *		oldtuple 传给触发器并标识要删除的内容，               
 *		tupleid 无效。从外表删除时，tupleid 无效；            
 *		FDW 必须使用 planSlot 中的数据确定要删除哪一行。     
 *		oldtuple 会传给外表触发器；当外表没有               
 *		相关触发器时其为 NULL。我们用 tupleDeleted       
 *		来指示元组是否真的被删除了，调用者可以用它
 *		来决定是否继续该操作。当此 DELETE 是分区键          
 *		UPDATE 的一部分时，EvalPlanQual() 返回的槽              
 *		会通过输出参数传回，该参数即                      
 *		epqreturnslot 
 *
 *		有则返回 RETURNING 结果，否则 NULL。  
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecDelete(ModifyTableContext *context,
		   ResultRelInfo *resultRelInfo,
		   ItemPointer tupleid,
		   HeapTuple oldtuple,
		   bool processReturning,
		   bool changingPart,
		   bool canSetTag,
		   TM_Result *tmresult,
		   bool *tupleDeleted,
		   TupleTableSlot **epqreturnslot)
{
	EState	   *estate = context->estate;
	Relation	resultRelationDesc = resultRelInfo->ri_RelationDesc;
	TupleTableSlot *slot = NULL;
	TM_Result	result;

	if (tupleDeleted)
		*tupleDeleted = false;

	/*
	 * 为删除做准备。这包括 BEFORE ROW 触发器，若它说     
	 * 完成即完成。     
	 */
	if (!ExecDeletePrologue(context, resultRelInfo, tupleid, oldtuple,
							epqreturnslot, tmresult))
		return NULL;

	/* INSTEAD OF ROW DELETE 触发器*/
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_delete_instead_row)
	{
		bool		dodelete;

		Assert(oldtuple != NULL);
		dodelete = ExecIRDeleteTriggers(estate, resultRelInfo, oldtuple);

		if (!dodelete)			/* "do nothing" */
			return NULL;
	}
	else if (resultRelInfo->ri_FdwRoutine)
	{
		/*
		 * 从外表删除：交给 FDW 处理         
		 *
		 * 我们提供 returning 槽来存放 RETURNING 数据，        
		 * 不过 FDW 愿意的话可以返回其他槽。         
		 */
		slot = ExecGetReturningSlot(estate, resultRelInfo);
		slot = resultRelInfo->ri_FdwRoutine->ExecForeignDelete(estate,
															   resultRelInfo,
															   slot,
															   context->planSlot);

		if (slot == NULL)		/* "do nothing" */
			return NULL;

		/*
		 * RETURNING 表达式可能引用 tableoid 列，因此在     
		 * 求值前要（重新）初始化 tts_tableOid。  
		 */
		if (TTS_EMPTY(slot))
			ExecStoreAllNullTuple(slot);

		slot->tts_tableOid = RelationGetRelid(resultRelationDesc);
	}
	else
	{
		/*
		 * 删除该元组 
		 *
		 * 注意：若 context->estate->es_crosscheck_snapshot  
		 * 不是 InvalidSnapshot 时，检查待删行是否可见，       
		 * 若不可见则抛出无法序列化错误。这是一个         
		 * 特殊行为，是事务快照模式下引用完整性更新     
		 * 所必需的。                        
		 */
ldelete:
		result = ExecDeleteAct(context, resultRelInfo, tupleid, changingPart);

		if (tmresult)
			*tmresult = result;

		switch (result)
		{
			case TM_SelfModified:

				/*
				 * 目标元组已经被当前命令、或者被当前   
				 * 事务中更晚的命令更新或删除。前一种  
				 * 情况可能出现在联接 DELETE 中：有多个        
				 * 元组联接到了同一个目标元组上。这种      
				 * 做法有点可疑，但 Postgres 向来都允许        
				 * 它：我们只是忽略额外的删除尝试。
				 *
				 * 后一种情况出现在元组被某个 BEFORE      
				 * 触发器中的命令修改，或者也许是被查      
				 * 询中所用易变函数里的命令修改。此种        
				 * 情况下我们不应忽略该删除，但继续执        
				 * 行同样是不安全的。我们不想丢弃原来   
				 * 的 DELETE 却保留基于其删除而触发的           
				 * 动作；但允许原来的 DELETE 而丢弃它       
				 * 所触发的更新也好不到哪里去。被更新        
				 * 的行所携带的信息可能按业务规则而言       
				 * 相当重要；所以抛出错误是唯一安全的     
				 * 做法 
				 *
				 * 如果触发器确实想要这种类型的交互，       
				 * 它可以重新执行 DELETE 并返回 NULL 取消      
				 * 外层删除。  
				 */
				if (context->tmfd.cmax != estate->es_output_cid)
					ereport(ERROR,
							(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
							 errmsg("tuple to be deleted was already modified by an operation triggered by the current command"),
							 errhint("Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.")));

				/* 否则，已被自己删除；无事可做   */
				return NULL;

			case TM_Ok:
				break;

			case TM_Updated:
				{
					TupleTableSlot *inputslot;
					TupleTableSlot *epqslot;

					if (IsolationUsesXactSnapshot())
						ereport(ERROR,
								(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
								 errmsg("could not serialize access due to concurrent update")));

					/*
					 * 已知需要做 EPQ，因此直接把元组取到  
					 * 正确的槽中。                       
					 */
					EvalPlanQualBegin(context->epqstate);
					inputslot = EvalPlanQualSlot(context->epqstate, resultRelationDesc,
												 resultRelInfo->ri_RangeTableIndex);

					result = table_tuple_lock(resultRelationDesc, tupleid,
											  estate->es_snapshot,
											  inputslot, estate->es_output_cid,
											  LockTupleExclusive, LockWaitBlock,
											  TUPLE_LOCK_FLAG_FIND_LAST_VERSION,
											  &context->tmfd);

					switch (result)
					{
						case TM_Ok:
							Assert(context->tmfd.traversed);
							epqslot = EvalPlanQual(context->epqstate,
												   resultRelationDesc,
												   resultRelInfo->ri_RangeTableIndex,
												   inputslot);
							if (TupIsNull(epqslot))
								/* 元组不再满足限定条件，退出…  */
								return NULL;

							/*
							 * 如果被要求，跳过删除并传回    
							 * 更新行。
							 */
							if (epqreturnslot)
							{
								*epqreturnslot = epqslot;
								return NULL;
							}
							else
								goto ldelete;

						case TM_SelfModified:

							/*
							 * 当沿更新链追踪一个被另一会话  
							 * 更新的元组，并到本事务中已更新 
							 * 的元组时就会到这里。若先前是  
							 * 由本命令更新，则忽略此次删除，  
							 * 否则将报                               
							 * 错 
							 *
							 * 见前文 table_tuple_delete() 对  
							 * TM_SelfModified 响应     
							 */
							if (context->tmfd.cmax != estate->es_output_cid)
								ereport(ERROR,
										(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
										 errmsg("tuple to be deleted was already modified by an operation triggered by the current command"),
										 errhint("Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.")));
							return NULL;

						case TM_Deleted:
							/* 元组已被删除；无事可做    */
							return NULL;

						default:

							/*
							 * TM_Invisible 应该不可能出现，因为    
							 * 我们正等待被更新行的版本，如 
							 * 果第一个版本不可见则早已报      
							 * 错了。    
							 *
							 * TM_Updated 也应不可能，因为           
							 * 正用它锁定最新版本：
							 * TUPLE_LOCK_FLAG_FIND_LAST_VERSION 
							 */
							elog(ERROR, "unexpected table_tuple_lock status: %u",
								 result);
							return NULL;
					}

					Assert(false);
					break;
				}

			case TM_Deleted:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent delete")));
				/* 元组已被删除；无事可做    */
				return NULL;

			default:
				elog(ERROR, "unrecognized table_tuple_delete status: %u",
					 result);
				return NULL;
		}

		/*
		 * 注意：通常人们会认为现在必须删除与该堆         
		 * 元组关联的索引元组……    
		 *
		 * ……但在 POSTGRES 中我们无需这样做，因为            
		 * VACUUM 以后会处理它。反正我们也无法立即         
		 * 删除它们，因为该元组对其他事务仍可见。      
		 */
	}

	if (canSetTag)
		(estate->es_processed)++;

	/* 告知调用者删除确实发生了。        */
	if (tupleDeleted)
		*tupleDeleted = true;

	ExecDeleteEpilogue(context, resultRelInfo, tupleid, oldtuple, changingPart);

	/* 如有且被要求则处理 RETURNING         */
	if (processReturning && resultRelInfo->ri_projectReturning)
	{
		/*
		 * 我们必须把目标元组放入某个槽中，这意味着     
		 * 首先得取出它。可用触发器元组槽。   
		 */
		TupleTableSlot *rslot;

		if (resultRelInfo->ri_FdwRoutine)
		{
			/* FDW 必须已提供了包含被删除行的槽           */
			Assert(!TupIsNull(slot));
		}
		else
		{
			slot = ExecGetReturningSlot(estate, resultRelInfo);
			if (oldtuple != NULL)
			{
				ExecForceStoreHeapTuple(oldtuple, slot, false);
			}
			else
			{
				if (!table_tuple_fetch_row_version(resultRelationDesc, tupleid,
												   SnapshotAny, slot))
					elog(ERROR, "failed to fetch deleted tuple for DELETE RETURNING");
			}
		}

		rslot = ExecProcessReturning(resultRelInfo, slot, context->planSlot);

		/*
		 * 在再次释放目标元组之前，确保 rslot 拥有       
		 * 所有引用传递值的本地副本。    
		 */
		ExecMaterializeSlot(rslot);

		ExecClearTuple(slot);

		return rslot;
	}

	return NULL;
}

/*
 * ExecCrossPartitionUpdate --- 把已更新元组移往另一分区       
 *
 * 做法是先从当前分区删除旧元组，再把新元组插入    
 * 根父表，即 mtstate->rootResultRelInfo。元组                     
 * 会从那里被重新路由到正确的                            
 * 分区。         
 *
 * 如果元组已成功移动，或者发现该元组已被并           
 * 发删除、调用者无需再做任何事，则返回               
 * true。        
 *
 * 如果发现要移动的元组已被并发更新，则返回 false。    
 * 在这种情况下，调用者必须检查 *retry_slot 中返回的      
 * 已更新元组是否仍需重新路由，并据此再次调用本函数  
 * 或相应地执行常规更新。对 MERGE 而言，更新后的元组  
 * 不会在 *retry_slot 中返回；它有自己的重试               
 * 逻辑
 */
static bool
ExecCrossPartitionUpdate(ModifyTableContext *context,
						 ResultRelInfo *resultRelInfo,
						 ItemPointer tupleid, HeapTuple oldtuple,
						 TupleTableSlot *slot,
						 bool canSetTag,
						 UpdateContext *updateCxt,
						 TM_Result *tmresult,
						 TupleTableSlot **retry_slot,
						 TupleTableSlot **inserted_tuple,
						 ResultRelInfo **insert_destrel)
{
	ModifyTableState *mtstate = context->mtstate;
	EState	   *estate = mtstate->ps.state;
	TupleConversionMap *tupconv_map;
	bool		tuple_deleted;
	TupleTableSlot *epqslot = NULL;

	context->cpUpdateReturningSlot = NULL;
	*retry_slot = NULL;

	/*
	 * 禁止会导致原行迁移到不同分区的 INSERT ON CONFLICT     
	 * DO UPDATE。也许某天能够实现，但它似乎是个没什么 
	 * 价值的小众特性。                                            
	 */
	if (((ModifyTable *) mtstate->ps.plan)->onConflictAction == ONCONFLICT_UPDATE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("invalid ON UPDATE specification"),
				 errdetail("The result tuple would appear in a different partition than the original tuple.")));

	/*
	 * 当直接在叶子分区上运行 UPDATE 时，只需让它以分区  
	 * 约束违反错误失败。          
	 */
	if (resultRelInfo == mtstate->rootResultRelInfo)
		ExecPartitionCheckEmitError(resultRelInfo, slot, estate);

	/* 若元组路由信息未初始化则初始化。   */
	if (mtstate->mt_partition_tuple_routing == NULL)
	{
		Relation	rootRel = mtstate->rootResultRelInfo->ri_RelationDesc;
		MemoryContext oldcxt;

		/* 这里构建的东西必须存活整个查询期。    */
		oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);

		mtstate->mt_partition_tuple_routing =
			ExecSetupPartitionTupleRouting(estate, rootRel);

		/*
		 * 分区的元组在被重新路由之前，必须先转换    
		 * 为根的格式，因此我们需要一个槽来存放          
		 * 元组。   
		 */
		Assert(mtstate->mt_root_tuple_slot == NULL);
		mtstate->mt_root_tuple_slot = table_slot_create(rootRel, NULL);

		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * 行移动，第 1 部分。删除元组，但跳过 RETURNING 处理，
	 * 我们要从 INSERT 返回行。   
	 */
	ExecDelete(context, resultRelInfo,
			   tupleid, oldtuple,
			   false,			/* processReturning */
			   true,			/* changingPart */
			   false,			/* canSetTag */
			   tmresult, &tuple_deleted, &epqslot);

	/*
	 * 由于某种原因，如果 DELETE 没有发生（例如触发器     
	 * 阻止了它，或已被自己删除，或被另一事务并发  
	 * 删除），那么我们也应跳过插入；                
	 * 否则，一个 UPDATE 可能导致所有分区的总行数        
	 * 增加，这显然是错误的。                  
	 *
	 * 对于普通 UPDATE 而言，元组被并发 UPDATE 或 DELETE         
	 * 作用的情况本由 EvalPlanQual 机制处理，但对我们  
	 * 把 UPDATE 转换为从此分区 DELETE 并向其他分区           
	 * INSERT 的情形，该机制不可用，因为 CTID 链无法     
	 * 跨越关系边界。我们有限度地模拟其语义：如果   
	 * DELETE 未能找到元组就跳过 INSERT。这确保两个并       
	 * 发尝试同时 UPDATE 同一元组不会把一个元组       
	 * 变成两个；而且对刚被删除的元组                          
	 * UPDATE 也不会让它复                                           
	 * 活
	 */
	if (!tuple_deleted)
	{
		/*
		 * epqslot 通常为 NULL。但当 ExecDelete() 发现另一        
		 * 个事务并发更新了同一行，它就会重新取回该 
		 * 行、跳过删除，并把 epqslot 设为重新取回的元组
		 * 槽。在这种情况下，我们需要再次做所有的检查。
		 * 对 MERGE 来说，一切交由调用者处理（它必须做   
		 * 额外的复查，且最终可能执行完全不同          
		 * 的动作）。  
		 */
		if (context->relaction != NULL)
			return *tmresult == TM_Ok;
		else if (TupIsNull(epqslot))
			return true;
		else
		{
			/* 取回旧元组的最新版本。           */
			TupleTableSlot *oldSlot;

			/* ……但首先要确保 ri_oldTupleSlot 已初始化。 */
			if (unlikely(!resultRelInfo->ri_projectNewInfoValid))
				ExecInitUpdateProjection(mtstate, resultRelInfo);
			oldSlot = resultRelInfo->ri_oldTupleSlot;
			if (!table_tuple_fetch_row_version(resultRelInfo->ri_RelationDesc,
											   tupleid,
											   SnapshotAny,
											   oldSlot))
				elog(ERROR, "failed to fetch tuple being updated");
			/* 并把新元组投影出来以便重试 UPDATE     */
			*retry_slot = ExecGetUpdateNewTuple(resultRelInfo, epqslot,
												oldSlot);
			return false;
		}
	}

	/*
	 * resultRelInfo 是各关系的 resultRelInfo 之一。所以           
	 * 必要时应把元组转换为根的元组描述符，因为   
	 * ExecInsert() 从根开始搜索。       
	 */
	tupconv_map = ExecGetChildToRootMap(resultRelInfo);
	if (tupconv_map != NULL)
		slot = execute_attr_map_slot(tupconv_map->attrMap,
									 slot,
									 mtstate->mt_root_tuple_slot);

	/* 元组路由从根表开始。            */
	context->cpUpdateReturningSlot =
		ExecInsert(context, mtstate->rootResultRelInfo, slot, canSetTag,
				   inserted_tuple, insert_destrel);

	/*
	 * 重置可能已被 INSERT 写入的转换状态。               
	 *        
	 */
	if (mtstate->mt_transition_capture)
		mtstate->mt_transition_capture->tcs_original_insert_tuple = NULL;

	/* 移动完成。    */
	return true;
}

/*
 * ExecUpdatePrologue -- ExecUpdate 的子例程  
 *
 * 为 UPDATE 准备执行器状态。这包括运行 BEFORE ROW       
 * 触发器。若其中某触发器令更新成为无操作，返回 
 * false，否则 true。 
 */
static bool
ExecUpdatePrologue(ModifyTableContext *context, ResultRelInfo *resultRelInfo,
				   ItemPointer tupleid, HeapTuple oldtuple, TupleTableSlot *slot,
				   TM_Result *result)
{
	Relation	resultRelationDesc = resultRelInfo->ri_RelationDesc;

	if (result)
		*result = TM_Ok;

	ExecMaterializeSlot(slot);

	/*
	 * 如果尚未打开该表的索引，则将其打开，以便能     
	 * 为更新后的元组添加新的索引条目。
	 */
	if (resultRelationDesc->rd_rel->relhasindex &&
		resultRelInfo->ri_IndexRelationDescs == NULL)
		ExecOpenIndices(resultRelInfo, false);

	/* BEFORE ROW UPDATE 触发器*/
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_update_before_row)
	{
		/* 刷出所有待处理插入，让行对触发器可见         */
		if (context->estate->es_insert_pending_result_relations != NIL)
			ExecPendingInserts(context->estate);

		return ExecBRUpdateTriggers(context->estate, context->epqstate,
									resultRelInfo, tupleid, oldtuple, slot,
									result, &context->tmfd);
	}

	return true;
}

/*
 * ExecUpdatePrepareSlot -- ExecUpdateAct 的子例程  
 *
 * 在更新之前对元组槽应用最终的修改。               
 * （把它单独拆出来是因为外表代码路径也需要它。）      
 */
static void
ExecUpdatePrepareSlot(ResultRelInfo *resultRelInfo,
					  TupleTableSlot *slot,
					  EState *estate)
{
	Relation	resultRelationDesc = resultRelInfo->ri_RelationDesc;

	/*
	 * 约束和 GENERATED 表达式可能引用 tableoid 列，         
	 * 因此在求值之前要（重新）初始化 tts_tableOid。  
	 */
	slot->tts_tableOid = RelationGetRelid(resultRelationDesc);

	/*
	 * 计算存储的生成列        
	 */
	if (resultRelationDesc->rd_att->constr &&
		resultRelationDesc->rd_att->constr->has_generated_stored)
		ExecComputeStoredGenerated(resultRelInfo, estate, slot,
								   CMD_UPDATE);
}

/*
 * ExecUpdateAct -- ExecUpdate 的子例程  
 *
 * 在普通表上操作时实际更新该元组。如果表是       
 * 一个分区、且命令是以引用某个祖先分区表的            
 * 方式调用的，此例程会把所得元组迁移到另一个        
 * 分区。 
 *
 * 必要时由调用者负责保持索引为最新。                  
 * 若发现元组已被并发更新，也由调用者负责              
 * 执行 EvalPlanQual。但在跨分区更新的情况下，             
 * 由本例程执行。
 */
static TM_Result
ExecUpdateAct(ModifyTableContext *context, ResultRelInfo *resultRelInfo,
			  ItemPointer tupleid, HeapTuple oldtuple, TupleTableSlot *slot,
			  bool canSetTag, UpdateContext *updateCxt)
{
	EState	   *estate = context->estate;
	Relation	resultRelationDesc = resultRelInfo->ri_RelationDesc;
	bool		partition_constraint_failed;
	TM_Result	result;

	updateCxt->crossPartUpdate = false;

	/*
	 * 若把元组移动到新分区，就会循环回到这里重新        
	 * 计算 GENERATED 值（各分区之间允许它们不同），并     
	 * 重新检查 RLS 策略和约束。但不会触发新分区的   
	 * 任何 BEFORE 触发器。                    
	 */
lreplace:
	/* 填入 GENERATED 列      */
	ExecUpdatePrepareSlot(resultRelInfo, slot, estate);

	/* 确保槽是独立的，例如考虑 EPQ      */
	ExecMaterializeSlot(slot);

	/*
	 * 如果分区约束检查失败，该行可能会被移动到另一
	 * 个分区；那种情况下我们应在向新分区插入之前   
	 * 检查 RLS CHECK 策略，而不是在这里检查。这是因     
	 * 为该分区上的触发器可能再次修改该行。                 
	 * 因此若分区约束失败，就跳过 WCO 检查。    
	 */
	partition_constraint_failed =
		resultRelationDesc->rd_rel->relispartition &&
		!ExecPartitionCheck(resultRelInfo, slot, estate, false);

	/* 检查任何 RLS UPDATE WITH CHECK 策略*/
	if (!partition_constraint_failed &&
		resultRelInfo->ri_WithCheckOptions != NIL)
	{
		/*
		 * ExecWithCheckOptions() 会跳过所有此时并非我们所        
		 * 要查找类型的 WCO。        
		 */
		ExecWithCheckOptions(WCO_RLS_UPDATE_CHECK,
							 resultRelInfo, slot, estate);
	}

	/*
	 * 如果分区检查失败，则尝试把该行移动到正确的
	 * 分区。 
	 */
	if (partition_constraint_failed)
	{
		TupleTableSlot *inserted_tuple,
				   *retry_slot;
		ResultRelInfo *insert_destrel = NULL;

		/*
		 * ExecCrossPartitionUpdate 会先从该行当前所在的    
		 * 分区中 DELETE 掉它，然后再把它插回根表，由根   
		 * 表把它重新路由到正确分区。但是，如果该元组  
		 * 已被并发更新，就需要进行重试。                 
		 */
		if (ExecCrossPartitionUpdate(context, resultRelInfo,
									 tupleid, oldtuple, slot,
									 canSetTag, updateCxt,
									 &result,
									 &retry_slot,
									 &inserted_tuple,
									 &insert_destrel))
		{
			/* 成功！*/
			updateCxt->updated = true;
			updateCxt->crossPartUpdate = true;

			/*
			 * 如果被更新的分区表被外键所引用，则排队      
			 * 触发器事件，以检查这些外键没有一个被违    
			 * 反。非跨分区更新时无需任何     
			 * 特殊处理，因为叶子分区的 AR 更新         
			 * 触发器会处理好这件事。而在以对源分区        
			 * 删除、向目标分区插入来实现的跨分区更     
			 * 新中，必须触发根表——也就是该查询里    
			 * 提到的那张根表——的 AR-UPDATE 触发         
			 * 器。                                
			 *
			 * NULL insert_destrel 表示移动未能发生，也就        
			 * 是说更新已失败，这种情况无需做任何事。  
			 */
			if (insert_destrel &&
				resultRelInfo->ri_TrigDesc &&
				resultRelInfo->ri_TrigDesc->trig_update_after_row)
				ExecCrossPartitionUpdateForeignKey(context,
												   resultRelInfo,
												   insert_destrel,
												   tupleid, slot,
												   inserted_tuple);

			return TM_Ok;
		}

		/*
		 * 没能成功，需要重试。如果正在运行 MERGE，     
		 * 我们不在此时重试；由它自行处理。  
		 */
		if (context->relaction != NULL)
			return result;

		/*
		 * ExecCrossPartitionUpdate 已在重试槽中装入了新         
		 * 元组的更新版；重新开始。
		 */
		slot = retry_slot;
		goto lreplace;
	}

	/*
	 * 检查该元组的约束。分区约束已在上面检查过  
	 * 了；但我们仍须确保该元组通过所有其他约束，    
	 * 因此我们会调用 ExecConstraints()，让它验证所有       
	 * 其余的检查。                    
	 */
	if (resultRelationDesc->rd_att->constr)
		ExecConstraints(resultRelInfo, slot, estate);

	/*
	 * 替换该堆元组    
	 *
	 * 注意：若 es_crosscheck_snapshot 不是 InvalidSnapshot，       
	 * 则检查待更新行对该快照是否可见，若不可见  
	 * 则抛出无法序列化的错误。这是一种特殊行为，     
	 * 是事务快照模式下引用完整性更新所必需        
	 * 的。       
	 */
	result = table_tuple_update(resultRelationDesc, tupleid, slot,
								estate->es_output_cid,
								estate->es_snapshot,
								estate->es_crosscheck_snapshot,
								true /* 等待提交    */ ,
								&context->tmfd, &updateCxt->lockmode,
								&updateCxt->updateIndexes);
	if (result == TM_Ok)
		updateCxt->updated = true;

	return result;
}

/*
 * ExecUpdateEpilogue -- ExecUpdate 的子例程  
 *
 * 元组更新的收尾步骤。当 ExecUpdateAct 返回并          
 * 表明元组已更新时，必须调用它。 
 */
static void
ExecUpdateEpilogue(ModifyTableContext *context, UpdateContext *updateCxt,
				   ResultRelInfo *resultRelInfo, ItemPointer tupleid,
				   HeapTuple oldtuple, TupleTableSlot *slot)
{
	ModifyTableState *mtstate = context->mtstate;
	List	   *recheckIndexes = NIL;

	/* 必要时为元组插入索引条目        */
	if (resultRelInfo->ri_NumIndices > 0 && (updateCxt->updateIndexes != TU_None))
		recheckIndexes = ExecInsertIndexTuples(resultRelInfo,
											   slot, context->estate,
											   true, false,
											   NULL, NIL,
											   (updateCxt->updateIndexes == TU_Summarizing));

	/* AFTER ROW UPDATE 触发器*/
	ExecARUpdateTriggers(context->estate, resultRelInfo,
						 NULL, NULL,
						 tupleid, oldtuple, slot,
						 recheckIndexes,
						 mtstate->operation == CMD_INSERT ?
						 mtstate->mt_oc_transition_capture :
						 mtstate->mt_transition_capture,
						 false);

	list_free(recheckIndexes);

	/*
	 * 检查来自父视图的 WITH CHECK OPTION 约束。按照        
	 * SQL 规范的要求，我们必须在测完所有约束和唯一
	 * 性冲突之后才能做这项检查，因此须在更新           
	 * 堆和所有索引后进行。     
	 *
	 * ExecWithCheckOptions() 会跳过所有此时并非我们所           
	 * 要查找类型的 WCO。     
	 */
	if (resultRelInfo->ri_WithCheckOptions != NIL)
		ExecWithCheckOptions(WCO_VIEW_CHECK, resultRelInfo,
							 slot, context->estate);
}

/*
 * 使用目标根分区表的触发器来排队更新事件，       
 * 以检查跨分区更新没有破坏指向它的任何外               
 * 键。                
 */
static void
ExecCrossPartitionUpdateForeignKey(ModifyTableContext *context,
								   ResultRelInfo *sourcePartInfo,
								   ResultRelInfo *destPartInfo,
								   ItemPointer tupleid,
								   TupleTableSlot *oldslot,
								   TupleTableSlot *newslot)
{
	ListCell   *lc;
	ResultRelInfo *rootRelInfo;
	List	   *ancestorRels;

	rootRelInfo = sourcePartInfo->ri_RootResultRelInfo;
	ancestorRels = ExecGetAncestorResultRels(context->estate, sourcePartInfo);

	/*
	 * 对于直接指向源分区的非根祖先的外键，理论上      
	 * 我们可以触发一个更新事件、用它们的触发器来       
	 * 强制这些约束——前提是我们能判断源分区与目       
	 * 标分区都位于同一个祖先之下。但就目前而言，       
	 * 我们只是报告这些约束无法被强制的错误。        
	 */
	foreach(lc, ancestorRels)
	{
		ResultRelInfo *rInfo = lfirst(lc);
		TriggerDesc *trigdesc = rInfo->ri_TrigDesc;
		bool		has_noncloned_fkey = false;

		/* 根祖先的触发器将被处理。        */
		if (rInfo == rootRelInfo)
			continue;

		if (trigdesc && trigdesc->trig_update_after_row)
		{
			for (int i = 0; i < trigdesc->numtriggers; i++)
			{
				Trigger    *trig = &trigdesc->triggers[i];

				if (!trig->tgisclone &&
					RI_FKey_trigger_type(trig->tgfoid) == RI_TRIGGER_PK)
				{
					has_noncloned_fkey = true;
					break;
				}
			}
		}

		if (has_noncloned_fkey)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot move tuple across partitions when a non-root ancestor of the source partition is directly referenced in a foreign key"),
					 errdetail("A foreign key points to ancestor \"%s\" but not the root ancestor \"%s\".",
							   RelationGetRelationName(rInfo->ri_RelationDesc),
							   RelationGetRelationName(rootRelInfo->ri_RelationDesc)),
					 errhint("Consider defining the foreign key on table \"%s\".",
							 RelationGetRelationName(rootRelInfo->ri_RelationDesc))));
	}

	/* 执行根表的触发器。        */
	ExecARUpdateTriggers(context->estate,
						 rootRelInfo, sourcePartInfo, destPartInfo,
						 tupleid, NULL, newslot, NIL, NULL, true);
}

/* ----------------------------------------------------------------
 *		ExecUpdate
 *
 *		注意：事务关闭时无法运行 UPDATE 查询 
 *		因为 UPDATE 实为 INSERT，我们的扫描会
 *		错误地无限循环，不停更新它刚插入的  
 *		那个元组。该问题应修复，但修复前，  
 *		我们可不想陷入那种会                    
 *		损坏数据库的无限循环
 *
 *		更新表时，tupleid 标识要更新的元组，而            
 *		oldtuple 为 NULL。通过视图 INSTEAD OF 触发器更新时，  
 *		oldtuple 传给触发器并标识要更新的内容，               
 *		tupleid 无效。更新外表时，tupleid 无效；          
 *		FDW 必须使用 planSlot 中的数据确定要更新哪一行。     
 *		oldtuple 会传给外表触发器；当外表没有               
 *		相关触发器时其为 NULL。                     
 *
 *		slot 包含要存储的新元组值。         
 *		planSlot 是 ModifyTable 子计划的输出；我们用它    
 *		来访问其他输入表的值（用于 RETURNING）、  
 *		行 ID junk 列等等。 
 *
 *		有则返回 RETURNING 结果，否则 NULL。退出时，           
 *		若 tupleid 标识了待更新元组，则它将标识经     
 *		EvalPlanQual 更新的元组。     
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecUpdate(ModifyTableContext *context, ResultRelInfo *resultRelInfo,
		   ItemPointer tupleid, HeapTuple oldtuple, TupleTableSlot *slot,
		   bool canSetTag)
{
	EState	   *estate = context->estate;
	Relation	resultRelationDesc = resultRelInfo->ri_RelationDesc;
	UpdateContext updateCxt = {0};
	TM_Result	result;

	/*
	 * 如果事务没有在运行则中止该操作  
	 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "cannot UPDATE during bootstrap");

	/*
	 * 为更新做准备。这包括 BEFORE ROW 触发器，若它说     
	 * 完成即完成。     
	 */
	if (!ExecUpdatePrologue(context, resultRelInfo, tupleid, oldtuple, slot, NULL))
		return NULL;

	/* INSTEAD OF ROW UPDATE 触发器*/
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_update_instead_row)
	{
		if (!ExecIRUpdateTriggers(estate, resultRelInfo,
								  oldtuple, slot))
			return NULL;		/* "do nothing" */
	}
	else if (resultRelInfo->ri_FdwRoutine)
	{
		/* 填写 GENERATEd 列      */
		ExecUpdatePrepareSlot(resultRelInfo, slot, estate);

		/*
		 * 更新外部表：交给 FDW 处理       
		 */
		slot = resultRelInfo->ri_FdwRoutine->ExecForeignUpdate(estate,
															   resultRelInfo,
															   slot,
															   context->planSlot);

		if (slot == NULL)		/* "do nothing" */
			return NULL;

		/*
		 * AFTER ROW 触发器或 RETURNING 表达式可能引用         
		 * tableoid 列，因此在求值之前先（重新）初始化      
		 * tts_tableOid。（这覆盖 FDW 替换了 slot 的情况。）
		 */
		slot->tts_tableOid = RelationGetRelid(resultRelationDesc);
	}
	else
	{
		ItemPointerData lockedtid;

		/*
		 * 如果在 EvalPlanQual 测试之后生成了新的候选元组，  
		 * 就必须循环回来再试。（不过触发器无需重做：    
		 * 如果存在 BEFORE 触发器，trigger.c 已经调用             
		 * table_tuple_lock 锁好了元组，无需                          
		 * 再次锁定。）
		 */
redo_act:
		lockedtid = *tupleid;
		result = ExecUpdateAct(context, resultRelInfo, tupleid, oldtuple, slot,
							   canSetTag, &updateCxt);

		/*
		 * 如果 ExecUpdateAct 报告已执行跨分区更新，          
		 * 那么 RETURNING 元组（如果有）已被投影，           
		 * 无需再做其他事。  
		 */
		if (updateCxt.crossPartUpdate)
			return context->cpUpdateReturningSlot;

		switch (result)
		{
			case TM_SelfModified:

				/*
				 * 目标元组已被当前命令，或本事务中      
				 * 较晚的命令更新或删除。前者见于        
				 * 连接 UPDATE：多个元组连接到同一               
				 * 目标元组。虽可疑，但 Postgres 一直           
				 * 允许这种做法：我们只执行第一个              
				 * 更新动作并忽略                             
				 * 其余更新尝试。      
				 *
				 * 后者为 BEFORE 触发器中的命令所致，     
				 * 或查询中易变函数里的命令修改时。此时   
				 * 不应忽略该更新，但继续执行同样不安全。  
				 * 我们不想丢弃原始 UPDATE 却保留其             
				 * 基于它的触发动作；也没有原则性         
				 * 方法把这次更新与之前几次合并。          
				 * 所以抛错是唯一                               
				 * 安全的                                            
				 * 做法 
				 *
				 * 若触发器确需这种交互，它可以（在能       
				 * 搞清楚怎么做前提下）重新执行该 UPDATE，   
				 * 然后返回 NULL 以取消外层更新。      
				 */
				if (context->tmfd.cmax != estate->es_output_cid)
					ereport(ERROR,
							(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
							 errmsg("tuple to be updated was already modified by an operation triggered by the current command"),
							 errhint("Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.")));

				/* 否则，已被自身更新；无事可做   */
				return NULL;

			case TM_Ok:
				break;

			case TM_Updated:
				{
					TupleTableSlot *inputslot;
					TupleTableSlot *epqslot;
					TupleTableSlot *oldSlot;

					if (IsolationUsesXactSnapshot())
						ereport(ERROR,
								(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
								 errmsg("could not serialize access due to concurrent update")));

					/*
					 * 已经知道需要执行 EPQ，所以              
					 * 直接取元组到正确的 slot 中。  
					 */
					inputslot = EvalPlanQualSlot(context->epqstate, resultRelationDesc,
												 resultRelInfo->ri_RangeTableIndex);

					result = table_tuple_lock(resultRelationDesc, tupleid,
											  estate->es_snapshot,
											  inputslot, estate->es_output_cid,
											  updateCxt.lockmode, LockWaitBlock,
											  TUPLE_LOCK_FLAG_FIND_LAST_VERSION,
											  &context->tmfd);

					switch (result)
					{
						case TM_Ok:
							Assert(context->tmfd.traversed);

							epqslot = EvalPlanQual(context->epqstate,
												   resultRelationDesc,
												   resultRelInfo->ri_RangeTableIndex,
												   inputslot);
							if (TupIsNull(epqslot))
								/* 元组不再通过 qual，退出...         */
								return NULL;

							/* 确保 ri_oldTupleSlot 已初始化。    */
							if (unlikely(!resultRelInfo->ri_projectNewInfoValid))
								ExecInitUpdateProjection(context->mtstate,
														 resultRelInfo);

							if (resultRelInfo->ri_needLockTagTuple)
							{
								UnlockTuple(resultRelationDesc,
											&lockedtid, InplaceUpdateTupleLock);
								LockTuple(resultRelationDesc,
										  tupleid, InplaceUpdateTupleLock);
							}

							/* 获取旧元组的最新版本。           */
							oldSlot = resultRelInfo->ri_oldTupleSlot;
							if (!table_tuple_fetch_row_version(resultRelationDesc,
															   tupleid,
															   SnapshotAny,
															   oldSlot))
								elog(ERROR, "failed to fetch tuple being updated");
							slot = ExecGetUpdateNewTuple(resultRelInfo,
														 epqslot, oldSlot);
							goto redo_act;

						case TM_Deleted:
							/* 元组已被删除；无事可做    */
							return NULL;

						case TM_SelfModified:

							/*
							 * 从其他会话更新的元组沿更新链  
							 * 追踪而来，到达一个本事务中已被 
							 * 更新的元组，就到这里。若先前  
							 * 被本命令修改过，就忽略          
							 * 这次冗余更新，                     
							 * 否则报错。     
							 *
							 * 另见上文 table_tuple_update()   
							 * TM_SelfModified 响应。  
							 */
							if (context->tmfd.cmax != estate->es_output_cid)
								ereport(ERROR,
										(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
										 errmsg("tuple to be updated was already modified by an operation triggered by the current command"),
										 errhint("Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.")));
							return NULL;

						default:
							/* 见 ExecDelete() 中 table_tuple_lock     */
							elog(ERROR, "unexpected table_tuple_lock status: %u",
								 result);
							return NULL;
					}
				}

				break;

			case TM_Deleted:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent delete")));
				/* 元组已被删除；无事可做    */
				return NULL;

			default:
				elog(ERROR, "unrecognized table_tuple_update status: %u",
					 result);
				return NULL;
		}
	}

	if (canSetTag)
		(estate->es_processed)++;

	ExecUpdateEpilogue(context, &updateCxt, resultRelInfo, tupleid, oldtuple,
					   slot);

	/* 如有 RETURNING 则处理   */
	if (resultRelInfo->ri_projectReturning)
		return ExecProcessReturning(resultRelInfo, slot, context->planSlot);

	return NULL;
}

/*
 * ExecOnConflictUpdate --- 执行 INSERT ON CONFLICT DO UPDATE           
 *
 * 在投机插入过程中尝试为更新锁定元组。如果      
 * 源自 ON CONFLICT DO UPDATE 的 qual 满足，则更新          
 * （但仍锁行，即使可能不满足 estate 快照       
 * ）。    
 *
 * 如果已完成（无论有无更新）则返回 true；调用者  
 * 须从头重试 INSERT 则返回 false。     
 */
static bool
ExecOnConflictUpdate(ModifyTableContext *context,
					 ResultRelInfo *resultRelInfo,
					 ItemPointer conflictTid,
					 TupleTableSlot *excludedSlot,
					 bool canSetTag,
					 TupleTableSlot **returning)
{
	ModifyTableState *mtstate = context->mtstate;
	ExprContext *econtext = mtstate->ps.ps_ExprContext;
	Relation	relation = resultRelInfo->ri_RelationDesc;
	ExprState  *onConflictSetWhere = resultRelInfo->ri_onConflict->oc_WhereClause;
	TupleTableSlot *existing = resultRelInfo->ri_onConflict->oc_Existing;
	TM_FailureData tmfd;
	LockTupleMode lockmode;
	TM_Result	test;
	Datum		xminDatum;
	TransactionId xmin;
	bool		isnull;

	/*
	 * 解析分析应已对所有系统关系（含这些关系） 
	 * 禁止了 ON CONFLICT。支持它并没有根本性的              
	 * 障碍；只是需要像其他 ExecUpdate() 调用者那样           
	 * 处理 LOCKTAG_TUPLE
	 */
	Assert(!resultRelInfo->ri_needLockTagTuple);

	/* 确定要使用的锁模式*/
	lockmode = ExecUpdateLockMode(context->estate, resultRelInfo);

	/*
	 * 为更新锁定元组。若不跟随更新就无法锁定时，  
	 * 才去跟随更新。这里的行锁冲突意味着            
	 * 我们先前关于该元组确定已提交的结论                
	 * 不再成立 
	 */
	test = table_tuple_lock(relation, conflictTid,
							context->estate->es_snapshot,
							existing, context->estate->es_output_cid,
							lockmode, LockWaitBlock, 0,
							&tmfd);
	switch (test)
	{
		case TM_Ok:
			/* 成功！*/
			break;

		case TM_Invisible:

			/*
			 * 刚插入的元组在同一命令内又被更新时          
			 * 便可能发生：例如插入多行具有相同的       
			 * 冲突键值的行。               
			 *
			 * 这有点类似于 ExecUpdate() 的 TM_SelfModified         
			 * 情形。我们不想继续，因为那会导致             
			 * 同一行以某种未指定的顺序被更新第二次，      
			 * 而且与普通 UPDATE 不同，这里没有可供打破的    
			 * 先例。
			 *
			 * 防止这种情况发生是用户的责任。这正是        
			 * SQL 标准同样规定，对 SQL MERGE 而言，             
			 * 试图把同一行更新两次时，必须                  
			 * 抛出异常。                                      
			 */
			xminDatum = slot_getsysattr(existing,
										MinTransactionIdAttributeNumber,
										&isnull);
			Assert(!isnull);
			xmin = DatumGetTransactionId(xminDatum);

			if (TransactionIdIsCurrentTransactionId(xmin))
				ereport(ERROR,
						(errcode(ERRCODE_CARDINALITY_VIOLATION),
				/* translator: %s is a SQL command name */
						 errmsg("%s command cannot affect row a second time",
								"ON CONFLICT DO UPDATE"),
						 errhint("Ensure that no rows proposed for insertion within the same command have duplicate constrained values.")));

			/* 不应发生这种事 */
			elog(ERROR, "attempted to lock invisible tuple");
			break;

		case TM_SelfModified:

			/*
			 * 绝不应到达这种状态。由于使用脏快照            
			 * 来寻找冲突元组，投机插入不可能                  
			 * 看到过这一冲突行。    
			 */
			elog(ERROR, "unexpected self-updated tuple");
			break;

		case TM_Updated:
			if (IsolationUsesXactSnapshot())
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not serialize access due to concurrent update")));

			/*
			 * 只要仍不支持分区表的 INSERT ON CONFLICT DO UPDATE， 
			 * 就不应到达这种情况：待锁定的元组               
			 * 因分区键被并发更新而移动到另                    
			 * 一个分区。      
			 */
			Assert(!ItemPointerIndicatesMovedPartitions(&tmfd.ctid));

			/*
			 * 告诉调用者从头开始重试。         
			 *
			 * 这里不适合使用通常的 EvalPlanQual() 式           
			 * 循环，因为行的新版本可能不再冲突，        
			 * 或者冲突元组实际上已被删除。                  
			 */
			ExecClearTuple(existing);
			return false;

		case TM_Deleted:
			if (IsolationUsesXactSnapshot())
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not serialize access due to concurrent delete")));

			/* TM_Updated 情形   */
			Assert(!ItemPointerIndicatesMovedPartitions(&tmfd.ctid));
			ExecClearTuple(existing);
			return false;

		default:
			elog(ERROR, "unrecognized table_tuple_lock status: %u", test);
	}

	/* 成功，元组已锁定。   */

	/*
	 * 若当前隔离级别有此要求，则验证元组                 
	 * 对 MVCC 快照可见。      
	 *
	 * 仅依赖 ExecUpdate() 内部的检查并不足够，因为例如   
	 * CONFLICT ... WHERE 子句可能使我们到不了那里。  
	 *
	 * 这意味着只有当前事务中的新命令能看到该行时   
	 * 我们才会继续，尽管在 READ COMMITTED 模式下，           
	 * 按当前语句的快照该元组并不可见。              
	 * 这与 UPDATE 处理较新元组版本的方式                      
	 * 一致。
	 */
	ExecCheckTupleVisible(context->estate, relation, existing);

	/*
	 * 把元组及所需的连接变量提供给 ExecQual 和           
	 * ExecProject。EXCLUDED 元组放入 ecxt_innertuple，                 
	 * 目标的既有元组放入 scantuple。EXCLUDED                    
	 * 已在 setrefs.c 中改为引用 INNER_VAR，                     
	 * 别无重定向。
	 */
	econtext->ecxt_scantuple = existing;
	econtext->ecxt_innertuple = excludedSlot;
	econtext->ecxt_outertuple = NULL;

	if (!ExecQual(onConflictSetWhere, econtext))
	{
		ExecClearTuple(existing);	/* 见下方返回  */
		InstrCountFiltered1(&mtstate->ps, 1);
		return true;			/* 元组处理完毕  */
	}

	if (resultRelInfo->ri_WithCheckOptions != NIL)
	{
		/*
		 * 对照适用于 UPDATE 的 USING 安全 qual（如有），  
		 * 在此以 RLS 检查/WCO 的形式强制执行。                 
		 *
		 * 重写器为 UPDATE 安全 qual 创建 UPDATE RLS 检查/WCO，
		 * 并把它们存为 "kind" 为 WCO_RLS_CONFLICT_CHECK 的 WCO， 
		 * 不过，它对 ON CONFLICT DO UPDATE 的特殊处理      
		 * 到此为止。    
		 *
		 * 重写器也会为随后的 ExecUpdate() 调用关联适用于   
		 * UPDATE 的直接 RLS 检查/WCO。INSERT 和 UPDATE 的 WCO  
		 * 种类天然互斥，因此                                        
		 * 在 INSERT 或 UPDATE 路径中不会出现虚假的            
		 * 过度强制。       
		 */
		ExecWithCheckOptions(WCO_RLS_CONFLICT_CHECK, resultRelInfo,
							 existing,
							 mtstate->ps.state);
	}

	/* 投影新的元组版本      */
	ExecProject(resultRelInfo->ri_onConflict->oc_ProjInfo);

	/*
	 * 注意，在上面的 table_tuple_lock 之后，目标元组       
	 * 可能已在本会话中被修改。我们选择此时不报错，    
	 * 与 ExecUpdate 对类似情形的处理一致。                       
	 * 如果 UPDATE 是从上面的 ExecQual()、                      
	 * ExecWithCheckOptions() 或 ExecProject() 内触发，例如 ON CONFLICT
	 * 的 SET 中选择 wCTE 时。 
	 */

	/* 用投影执行 UPDATE         */
	*returning = ExecUpdate(context, resultRelInfo,
							conflictTid, NULL,
							resultRelInfo->ri_onConflict->oc_ProjSlot,
							canSetTag);

	/*
	 * 清掉既有元组，因为后续输入行中可能不再有冲突。 
	 * 不想把资源一直持有到查询                                 
	 * 结束
	 */
	ExecClearTuple(existing);
	return true;
}

/*
 * 执行 MERGE  
 */
static TupleTableSlot *
ExecMerge(ModifyTableContext *context, ResultRelInfo *resultRelInfo,
		  ItemPointer tupleid, bool canSetTag)
{
	bool		matched;

	/*-----
	 * 若处理 WHEN MATCHED 情形（tupleid 有效），我们       
	 * 执行第一个满足附加 WHEN MATCHED AND qual 的动作。    
	 * 如果找到一个不带 qual 的动作，就                   
	 * 执行它
	 *
	 * 类似地，若处理 WHEN NOT MATCHED 情形，我们按顺序    
	 * 逐一查看 WHEN NOT MATCHED 动作，直到一个通过。    
	 *
	 * 当目标元组被并发更新/删除时，情况变得有趣。 
	 * 这种并发更新/删除会在我们执行 WHEN MATCHED 动作     
	 * 期间被检测到。           
	 *
	 * 并发更新可以：   
	 *
	 * 1. 修改目标元组，使它不再满足                   
	 *    附加在当前 WHEN MATCHED 动作上的附加 qual        
	 *
	 *    此时我们处理的仍是 WHEN MATCHED 情形。          
	 *    我们从头重新检查 WHEN MATCHED 动作列表，         
	 *    选择第一个满足新目标元组的动作。         
	 *
	 * 2. 修改目标元组，使连接 qual 不再通过，              
	 *    因而源元组不再有匹配。            
	 *
	 *    在这种情况下，源元组不再匹配目标元组，         
	 *    于是转而寻找可执行的 WHEN NOT MATCHED 动作        
	 *    执行  
	 *
	 * XXX 嗯，若更新后的元组此时能匹配此前判为      
	 * NOT MATCHED 的元组？      
	 *
	 * 并发删除会把 WHEN MATCHED 情形变为 WHEN NOT MATCHED。    
	 *
	 * 只要更新后的元组仍满足连接 qual（仍是 WHEN   
	 * MATCHED 情形），ExecMergeMatched 就会负责沿着更新链     
	 * 追踪并重新寻找可满足的 WHEN MATCHED 动作。如果元组
	 * 被删除或连接 qual 失败，它就返回，我们转而尝试
	 * ExecMergeNotMatched。鉴于 ExecMergeMatched 总是通过         
	 * 沿更新链追踪来取得进展，而且我们从不会从          
	 * ExecMergeNotMatched 切换回 ExecMergeMatched，因此不存在   
	 * 活锁。
	 */
	matched = tupleid != NULL;
	if (matched)
		matched = ExecMergeMatched(context, resultRelInfo, tupleid, canSetTag);

	/*
	 * 要么处理的是 NOT MATCHED 元组，要么 ExecMergeMatched()    
	 * 返回了 "false"，表示先前 MATCHED 的元组不再           
	 * 匹配  
	 */
	if (!matched)
		ExecMergeNotMatched(context, resultRelInfo, canSetTag);

	/* 尚不支持 RETURNING   */
	return NULL;
}

/*
 * 检查并执行第一个满足条件的 MATCHED 动作。当前目标    
 * 元组由 tupleid 标识。    
 *
 * 我们从第一个 WHEN MATCHED 动作开始，检查其 WHEN qual     
 * （如有）是否通过。若第一个动作的 WHEN qual 不过，
 * 就检查第二个、第三个，依此类推。若到末尾，       
 * 则不采取任何动作并返回 true，表示该元组无需          
 * 进一步的处理。   
 *
 * 如果确实找到满足条件的动作，就尝试执行该动作。    
 *
 * 如果元组被并发更新，则用更新后的元组运行 EvalPlanQual 
 * 来复查连接 qual。注意，各动作关联的附加 qual             
 * 由本例程通过 ExecQual 求值，                                    
 * 而连接 qual 由 EvalPlanQual 检查。如果 EvalPlanQual 告诉我们
 * 更新后的元组仍通过连接 qual，则从第一个动作           
 * 重新开始寻找满足条件的动作。否则返回 false --      
 * 表示须为当前源元组执行 NOT MATCHED                         
 * 动作。    
 */
static bool
ExecMergeMatched(ModifyTableContext *context, ResultRelInfo *resultRelInfo,
				 ItemPointer tupleid, bool canSetTag)
{
	ModifyTableState *mtstate = context->mtstate;
	ItemPointerData lockedtid;
	TupleTableSlot *newslot;
	EState	   *estate = context->estate;
	ExprContext *econtext = mtstate->ps.ps_ExprContext;
	bool		isNull;
	EPQState   *epqstate = &mtstate->mt_epqstate;
	ListCell   *l;
	bool		no_further_action = true;

	/*
	 * 若没有 WHEN MATCHED 动作，即告完成。   
	 */
	if (resultRelInfo->ri_matchedMergeAction == NIL)
		return no_further_action;

	/*
	 * 把元组及所需的连接变量提供给 ExecQual 和           
	 * ExecProject。目标的既有元组放入 scantuple。                 
	 * 同样，只有在 MATCHED 元组以及 UPDATE/DELETE 动作的     
	 * 情形下才需目标关系的 slot。  
	 */
	econtext->ecxt_scantuple = resultRelInfo->ri_oldTupleSlot;
	econtext->ecxt_innertuple = context->planSlot;
	econtext->ecxt_outertuple = NULL;

lmerge_matched:

	if (resultRelInfo->ri_needLockTagTuple)
	{
		/*
		 * 对 CMD_DELETE、CMD_NOTHING，以及不匹配 mas_whenqual    
		 * 的元组也照样加锁。系统目录上的 MERGE 是次要      
		 * 用例，不值得为它们做优化。    
		 */
		LockTuple(resultRelInfo->ri_RelationDesc, tupleid,
				  InplaceUpdateTupleLock);
		lockedtid = *tupleid;
	}
	else
		ItemPointerSetInvalid(&lockedtid);

	/*
	 * 本例程只对匹配行调用，此时我们必须已找到         
	 * 目标行的 tupleid；取出该元组。                    
	 *
	 * 为此我们使用 SnapshotAny，因为在 EvalPlanQual 返回     
	 * 新元组后我们可能再次被调用，而其 MVCC 快照未必  
	 * 可见。     
	 */

	if (!table_tuple_fetch_row_version(resultRelInfo->ri_RelationDesc,
									   tupleid,
									   SnapshotAny,
									   resultRelInfo->ri_oldTupleSlot))
		elog(ERROR, "failed to fetch the target tuple");

	foreach(l, resultRelInfo->ri_matchedMergeAction)
	{
		MergeActionState *relaction = (MergeActionState *) lfirst(l);
		CmdType		commandType = relaction->mas_action->commandType;
		TM_Result	result;
		UpdateContext updateCxt = {0};

		/*
		 * 求值 WHEN 条件     
		 *
		 * 在没有任何条件时，我们无条件执行动作
		 * （无需单独检查，因为如果没有条件可求值，      
		 * ExecQual() 会返回 true）。                     
		 */
		if (!ExecQual(relaction->mas_whenqual, econtext))
			continue;

		/*
		 * 检查既有元组是否满足 UPDATE/DELETE RLS 策略的  
		 * USING 检查。如果这些检查失败，我们             
		 * 报错
		 *
		 * UPDATE RLS 策略的 WITH CHECK qual 已在                
		 * ExecUpdateAct() 中应用，故此处无需处理                  
		 * 之  
		 *
		 * 注意：必须在 WHEN qual 求值之后再做这件事，     
		 * 仅在策略有影响时检查。    
		 */
		if (resultRelInfo->ri_WithCheckOptions && commandType != CMD_NOTHING)
		{
			ExecWithCheckOptions(commandType == CMD_UPDATE ?
								 WCO_RLS_MERGE_UPDATE_CHECK : WCO_RLS_MERGE_DELETE_CHECK,
								 resultRelInfo,
								 resultRelInfo->ri_oldTupleSlot,
								 context->mtstate->ps.state);
		}

		/* 执行既定动作    */
		switch (commandType)
		{
			case CMD_UPDATE:

				/*
				 * 投影出输出元组，并用它来更新表。           
				 * 我们不需要过滤掉 junk 属性，因为           
				 * UPDATE 动作目标列表没有此类属性 
				 */
				newslot = ExecProject(relaction->mas_proj);

				context->relaction = relaction;
				if (!ExecUpdatePrologue(context, resultRelInfo,
										tupleid, NULL, newslot, &result))
				{
					if (result == TM_Ok)
						goto out;	/* "do nothing" */
					break;		/* 并发更新/删除      */
				}
				result = ExecUpdateAct(context, resultRelInfo, tupleid, NULL,
									   newslot, canSetTag, &updateCxt);

				/*
				 * 与 ExecUpdate() 一样，若 ExecUpdateAct() 报告 
				 * 跨分区更新，那么我们无需再做任何事 --    
				 * 该 UPDATE 已被拆成一次 DELETE 和一次 INSERT， 
				 * 我们绝不能再执行任何常规的                
				 * 更新后任务。
				 */
				if (updateCxt.crossPartUpdate)
				{
					mtstate->mt_merge_updated += 1;
					goto out;
				}

				if (result == TM_Ok && updateCxt.updated)
				{
					ExecUpdateEpilogue(context, &updateCxt, resultRelInfo,
									   tupleid, NULL, newslot);
					mtstate->mt_merge_updated += 1;
				}
				break;

			case CMD_DELETE:
				context->relaction = relaction;
				if (!ExecDeletePrologue(context, resultRelInfo, tupleid,
										NULL, NULL, &result))
				{
					if (result == TM_Ok)
						goto out;	/* "do nothing" */
					break;		/* 并发更新/删除      */
				}
				result = ExecDeleteAct(context, resultRelInfo, tupleid, false);
				if (result == TM_Ok)
				{
					ExecDeleteEpilogue(context, resultRelInfo, tupleid, NULL,
									   false);
					mtstate->mt_merge_deleted += 1;
				}
				break;

			case CMD_NOTHING:
				/* 什么都不做也可以   */
				result = TM_Ok;
				break;

			default:
				elog(ERROR, "unknown action in MERGE WHEN MATCHED clause");
		}

		switch (result)
		{
			case TM_Ok:
				/* 正常；执行最终动作     */
				if (canSetTag && commandType != CMD_NOTHING)
					(estate->es_processed)++;

				break;

			case TM_SelfModified:

				/*
				 * 目标元组已被当前命令，或本事务中      
				 * 较晚的命令更新或删除。前者              
				 * 被 SQL 标准对 MERGE 明确禁止，标准            
				 * 坚持 MERGE 连接条件不得把一个                
				 * 目标行连接到                                      
				 * 多个源行。
				 *
				 * 后者为 BEFORE 触发器中的命令所致，     
				 * 或查询中易变函数里的命令修改时。此时   
				 * 我们不应忽略该 MERGE 动作，但继续执行       
				 * 同样不安全。我们不想丢弃原始           
				 * MERGE 动作却保留基于它的触发动作；         
				 * 而允许保留原始 MERGE 动作却丢弃它           
				 * 触发的更新也不好。所以                        
				 * 抛错才是唯一安全的                               
				 * 做法。                                 
				 */
				if (context->tmfd.cmax != estate->es_output_cid)
					ereport(ERROR,
							(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
							 errmsg("tuple to be updated or deleted was already modified by an operation triggered by the current command"),
							 errhint("Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.")));

				if (TransactionIdIsCurrentTransactionId(context->tmfd.xmax))
					ereport(ERROR,
							(errcode(ERRCODE_CARDINALITY_VIOLATION),
					/* translator: %s is a SQL command name */
							 errmsg("%s command cannot affect row a second time",
									"MERGE"),
							 errhint("Ensure that not more than one source row matches any one target row.")));

				/* 不应发生这种事 */
				elog(ERROR, "attempted to update or delete invisible tuple");
				break;

			case TM_Deleted:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent delete")));

				/*
				 * 如果元组已被删除，返回让调用者         
				 * 按 NOT MATCHED 子句处理。     
				 */
				no_further_action = false;
				goto out;

			case TM_Updated:
				{
					Relation	resultRelationDesc;
					TupleTableSlot *epqslot,
							   *inputslot;
					LockTupleMode lockmode;

					/*
					 * 目标元组被其他事务并发更新。用          
					 * 新版本的元组运行 EvalPlanQual()。若          
					 * 它不返回元组，则切换到                
					 * NOT MATCHED 动作列表。若它返回              
					 * 元组且连接 qual 仍满足，那么只需       
					 * 从头重新检查 MATCHED 动作，             
					 * 并执行第一个满足条件的                      
					 * 动作 
					 */
					resultRelationDesc = resultRelInfo->ri_RelationDesc;
					lockmode = ExecUpdateLockMode(estate, resultRelInfo);

					inputslot = EvalPlanQualSlot(epqstate, resultRelationDesc,
												 resultRelInfo->ri_RangeTableIndex);

					result = table_tuple_lock(resultRelationDesc, tupleid,
											  estate->es_snapshot,
											  inputslot, estate->es_output_cid,
											  lockmode, LockWaitBlock,
											  TUPLE_LOCK_FLAG_FIND_LAST_VERSION,
											  &context->tmfd);
					switch (result)
					{
						case TM_Ok:
							epqslot = EvalPlanQual(epqstate,
												   resultRelationDesc,
												   resultRelInfo->ri_RangeTableIndex,
												   inputslot);

							/*
							 * 若未得到元组，或所得元组 ctid    
							 * 为 NULL，返回调用者：已非 MATCHED    
							 * 元组，因此调用者可以用 NOT MATCHED
							 * 动作重试。     
							 */
							if (TupIsNull(epqslot))
							{
								no_further_action = false;
								goto out;
							}

							(void) ExecGetJunkAttribute(epqslot,
														resultRelInfo->ri_RowIdAttNo,
														&isNull);
							if (isNull)
							{
								no_further_action = false;
								goto out;
							}

							/*
							 * 当元组被并发更新并迁移到    
							 * 另一分区时，当前的 MERGE          
							 * 实现无法追踪。也许有更好       
							 * 的办法处理这种情况，但那          
							 * 需要识别元组迁移到的                
							 * 关系，并把当前的 resultRelInfo  
							 * 设为该关系。    
							 */
							if (ItemPointerIndicatesMovedPartitions(tupleid))
								ereport(ERROR,
										(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
										 errmsg("tuple to be deleted was already moved to another partition due to concurrent update")));

							/*
							 * 非 NULL 的 ctid 意味着我们处理的     
							 * 仍是 MATCHED 情形。重启循环，       
							 * 重新应用全部 MATCHED 规则，确保  
							 * 第一个满足条件的 WHEN MATCHED 动作 
							 * 得以执行
							 *
							 * tupleid 已被更新为新元组的值，  
							 * 这是我们在顶部重新取数所需        
							 * 的 
							 */
							if (resultRelInfo->ri_needLockTagTuple)
								UnlockTuple(resultRelInfo->ri_RelationDesc,
											&lockedtid,
											InplaceUpdateTupleLock);
							goto lmerge_matched;

						case TM_Deleted:

							/*
							 * 元组已被删除；让调用者执行 NOT  
							 * MATCHED 动作 
							 */
							no_further_action = false;
							goto out;

						case TM_SelfModified:

							/*
							 * 当沿更新链从被其他会话更新的  
							 * 元组追踪，到达一个已被当前命令 
							 * 或本事务较晚命令更新或删除的  
							 * 元组时，就会到这里。如上         
							 * 所述，这始终都应被视为            
							 * 错误。                                 
							 */
							if (context->tmfd.cmax != estate->es_output_cid)
								ereport(ERROR,
										(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
										 errmsg("tuple to be updated or deleted was already modified by an operation triggered by the current command"),
										 errhint("Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.")));

							if (TransactionIdIsCurrentTransactionId(context->tmfd.xmax))
								ereport(ERROR,
										(errcode(ERRCODE_CARDINALITY_VIOLATION),
								/* translator: %s is a SQL command name */
										 errmsg("%s command cannot affect row a second time",
												"MERGE"),
										 errhint("Ensure that not more than one source row matches any one target row.")));

							/* 不应发生这种事 */
							elog(ERROR, "attempted to update or delete invisible tuple");
							no_further_action = false;
							goto out;

						default:
							/* 见 ExecDelete() 中 table_tuple_lock     */
							elog(ERROR, "unexpected table_tuple_lock status: %u",
								 result);
							no_further_action = false;
							goto out;
					}
				}

			case TM_Invisible:
			case TM_WouldBlock:
			case TM_BeingModified:
				/* 这些不应出现     */
				elog(ERROR, "unexpected tuple operation result: %d", result);
				break;
		}

		/*
		 * 已激活某个 WHEN 子句，故不再继续搜索。     
		 * 这是要求的行为，不是优化。                  
		 */
		break;
	}

	/*
	 * 成功执行了某个动作，或未找到满足条件的动作。
	 */
out:
	if (ItemPointerIsValid(&lockedtid))
		UnlockTuple(resultRelInfo->ri_RelationDesc, &lockedtid,
					InplaceUpdateTupleLock);
	return no_further_action;
}

/*
 * 执行首个满足条件的 NOT MATCHED 动作  
 */
static void
ExecMergeNotMatched(ModifyTableContext *context, ResultRelInfo *resultRelInfo,
					bool canSetTag)
{
	ModifyTableState *mtstate = context->mtstate;
	ExprContext *econtext = mtstate->ps.ps_ExprContext;
	List	   *actionStates = NIL;
	ListCell   *l;

	/*
	 * 对 INSERT 动作，使用根关系的 merge 动作即可，因为  
	 * INSERT 的目标列表和 WHEN 条件只能引用                
	 * 源关系，所以使用哪个结果关系                           
	 * 均可    
	 *
	 * XXX 这是否意味着对分区表可以避免为                      
	 * not-matched 动作建 actionStates 副本？
	 */
	actionStates = resultRelInfo->ri_notMatchedMergeAction;

	/*
	 * 把源元组提供给 ExecQual 和 ExecProject。我们不需要      
	 * 目标元组，因为 WHEN qual 和目标列表不能引用         
	 * 目标列。       
	 */
	econtext->ecxt_scantuple = NULL;
	econtext->ecxt_innertuple = context->planSlot;
	econtext->ecxt_outertuple = NULL;

	foreach(l, actionStates)
	{
		MergeActionState *action = (MergeActionState *) lfirst(l);
		CmdType		commandType = action->mas_action->commandType;
		TupleTableSlot *newslot;

		/*
		 * 求值 WHEN 条件     
		 *
		 * 在没有任何条件时，我们无条件执行动作
		 * （无需单独检查，因为如果没有条件可求值，      
		 * ExecQual() 会返回 true）。                     
		 */
		if (!ExecQual(action->mas_whenqual, econtext))
			continue;

		/* 执行既定动作    */
		switch (commandType)
		{
			case CMD_INSERT:

				/*
				 * 投影元组。对分区表而言，投影             
				 * 在构建时就已使用根的描述符，                
				 * 因此这里无需映射元组。      
				 */
				newslot = ExecProject(action->mas_proj);
				context->relaction = action;

				(void) ExecInsert(context, mtstate->rootResultRelInfo, newslot,
								  canSetTag, NULL, NULL);
				mtstate->mt_merge_inserted += 1;
				break;
			case CMD_NOTHING:
				/* 不做事  */
				break;
			default:
				elog(ERROR, "unknown action in MERGE WHEN NOT MATCHED clause");
		}

		/*
		 * 已激活某个 WHEN 子句，故不再继续搜索。     
		 * 这是要求的行为，不是优化。                  
		 */
		break;
	}
}

/*
 * 初始化 MERGE 执行所需的状态。
 */
void
ExecInitMerge(ModifyTableState *mtstate, EState *estate)
{
	ModifyTable *node = (ModifyTable *) mtstate->ps.plan;
	ResultRelInfo *rootRelInfo = mtstate->rootResultRelInfo;
	ResultRelInfo *resultRelInfo;
	ExprContext *econtext;
	ListCell   *lc;
	int			i;

	if (node->mergeActionLists == NIL)
		return;

	mtstate->mt_merge_subcommands = 0;

	if (mtstate->ps.ps_ExprContext == NULL)
		ExecAssignExprContext(estate, &mtstate->ps);
	econtext = mtstate->ps.ps_ExprContext;

	/*
	 * 为 mergeActionList 上每个动作创建一个 MergeActionState， 
	 * 并把它加入匹配动作列表或不匹配动作列表。      
	 *
	 * ExecInitPartitionInfo() 中有类似逻辑，若改此处，    
	 * 那边也要同步修改。    
	 */
	i = 0;
	foreach(lc, node->mergeActionLists)
	{
		List	   *mergeActionList = lfirst(lc);
		TupleDesc	relationDesc;
		ListCell   *l;

		resultRelInfo = mtstate->resultRelInfo + i;
		i++;
		relationDesc = RelationGetDescr(resultRelInfo->ri_RelationDesc);

		/* 为此关系的 MERGE 取数初始化 slot       */
		if (unlikely(!resultRelInfo->ri_projectNewInfoValid))
			ExecInitMergeTupleSlots(mtstate, resultRelInfo);

		foreach(l, mergeActionList)
		{
			MergeAction *action = (MergeAction *) lfirst(l);
			MergeActionState *action_state;
			TupleTableSlot *tgtslot;
			TupleDesc	tgtdesc;
			List	  **list;

			/*
			 * 为该关系构建 merge 动作状态。（对分区， 
			 * ExecInitPartitionInfo 中有等价代码。）   
			 */
			action_state = makeNode(MergeActionState);
			action_state->mas_action = action;
			action_state->mas_whenqual = ExecInitQual((List *) action->qual,
													  &mtstate->ps);

			/*
			 * 创建两列表 -- 一个给 WHEN MATCHED 动作，一个给  
			 * WHEN NOT MATCHED 动作 -- 并把 MergeActionState 放入     
			 * 相应的列表。   
			 */
			if (action_state->mas_action->matched)
				list = &resultRelInfo->ri_matchedMergeAction;
			else
				list = &resultRelInfo->ri_notMatchedMergeAction;
			*list = lappend(*list, action_state);

			switch (action->commandType)
			{
				case CMD_INSERT:
					/* INSERT 动作始终使用 rootRelInfo */
					ExecCheckPlanOutput(rootRelInfo->ri_RelationDesc,
										action->targetList);

					/*
					 * 如果 MERGE 的目标是分区表，任何 INSERT  
					 * 动作都必须经由它路由，而不是子   
					 * 关系。为此初始化路由结构以及根        
					 * 表的 "new" 元组 slot（如果尚未做）。       
					 * 我们为所有关系准备的投影都使用         
					 * 根关系描述符，并指向计划的根           
					 * slot。这与我们上方检查过计划        
					 * 输出匹配根关系这一事实                  
					 * 一致 
					 */
					if (rootRelInfo->ri_RelationDesc->rd_rel->relkind ==
						RELKIND_PARTITIONED_TABLE)
					{
						if (mtstate->mt_partition_tuple_routing == NULL)
						{
							/*
							 * 若尚未做，则初始化路由 planstate 即
							 * 可  
							 *
							 * 注意 slot 由 ModifyTableState 作为独立
							 * slot 来管理，因此第二个参数        
							 * 传 NULL。               
							 */
							mtstate->mt_root_tuple_slot =
								table_slot_create(rootRelInfo->ri_RelationDesc,
												  NULL);
							mtstate->mt_partition_tuple_routing =
								ExecSetupPartitionTupleRouting(estate,
															   rootRelInfo->ri_RelationDesc);
						}
						tgtslot = mtstate->mt_root_tuple_slot;
						tgtdesc = RelationGetDescr(rootRelInfo->ri_RelationDesc);
					}
					else
					{
						/*
						 * 若 MERGE 目标是继承表，则插入根表， 
						 * 须初始化其 "new" slot（若未做），  
						 * 并在投影中使用它的                       
						 * 关系描述符。                     
						 *
						 * 对非继承表，rootRelInfo 与        
						 * resultRelInfo 相同，"new" 元组            
						 * slot 应已初始化。                 
						 */
						if (rootRelInfo->ri_newTupleSlot == NULL)
							rootRelInfo->ri_newTupleSlot =
								table_slot_create(rootRelInfo->ri_RelationDesc,
												  &estate->es_tupleTable);

						tgtslot = rootRelInfo->ri_newTupleSlot;
						tgtdesc = RelationGetDescr(rootRelInfo->ri_RelationDesc);
					}

					action_state->mas_proj =
						ExecBuildProjectionInfo(action->targetList, econtext,
												tgtslot,
												&mtstate->ps,
												tgtdesc);

					mtstate->mt_merge_subcommands |= MERGE_INSERT;
					break;
				case CMD_UPDATE:
					action_state->mas_proj =
						ExecBuildUpdateProjection(action->targetList,
												  true,
												  action->updateColnos,
												  relationDesc,
												  econtext,
												  resultRelInfo->ri_newTupleSlot,
												  &mtstate->ps);
					mtstate->mt_merge_subcommands |= MERGE_UPDATE;
					break;
				case CMD_DELETE:
					mtstate->mt_merge_subcommands |= MERGE_DELETE;
					break;
				case CMD_NOTHING:
					break;
				default:
					elog(ERROR, "unknown action in MERGE WHEN clause");
					break;
			}
		}
	}

	/*
	 * 若 MERGE 目标是继承表，任何 INSERT 动作都会用        
	 * rootRelInfo，而 rootRelInfo 不在 resultRelInfo 数组中。     
	 * 因此必须初始化它的 WITH CHECK OPTION 约束，           
	 * 正如 ExecInitModifyTable 对 resultRelInfo 条目所做的那样；  
	 * 但 RETURNING 无事可做，因为 MERGE 不支持 RETURNING。       
	 *
	 * 注意规划器不会为根关系构建 withCheckOptionList，         
	 * 但与 ExecInitPartitionInfo 一样，可用第一个          
	 * resultRelInfo 条目作为参照来计算                          
	 * attno。   
	 */
	if (rootRelInfo != mtstate->resultRelInfo &&
		rootRelInfo->ri_RelationDesc->rd_rel->relkind != RELKIND_PARTITIONED_TABLE &&
		(mtstate->mt_merge_subcommands & MERGE_INSERT) != 0)
	{
		Relation	rootRelation = rootRelInfo->ri_RelationDesc;
		Relation	firstResultRel = mtstate->resultRelInfo[0].ri_RelationDesc;

		if (node->withCheckOptionLists != NIL)
		{
			List	   *wcoList;
			List	   *wcoExprs = NIL;

			/* WCO 列表数量应与结果关系数量相同   */
			Assert(list_length(node->withCheckOptionLists) ==
				   list_length(node->resultRelations));

			/*
			 * 用第一个 WCO 列表作参照。在最常见的情况下， 
			 * 它对应的关系就是 rootRelInfo 的关系，              
			 * 因而无需调整其 attno。        
			 */
			wcoList = linitial(node->withCheckOptionLists);
			if (rootRelation != firstResultRel)
			{
				int			firstVarno = mtstate->resultRelInfo[0].ri_RangeTableIndex;
				AttrMap    *part_attmap;
				bool		found_whole_row;

				/* 把其中的 Var 转换为包含根的 attno         */
				part_attmap =
					build_attrmap_by_name(RelationGetDescr(rootRelation),
										  RelationGetDescr(firstResultRel),
										  false);

				wcoList = (List *)
					map_variable_attnos((Node *) wcoList,
										firstVarno, 0,
										part_attmap,
										RelationGetForm(rootRelation)->reltype,
										&found_whole_row);
			}

			foreach(lc, wcoList)
			{
				WithCheckOption *wco = lfirst_node(WithCheckOption, lc);
				ExprState  *wcoExpr = ExecInitQual(castNode(List, wco->qual),
												   &mtstate->ps);

				wcoExprs = lappend(wcoExprs, wcoExpr);
			}

			rootRelInfo->ri_WithCheckOptions = wcoList;
			rootRelInfo->ri_WithCheckOptionExprs = wcoExprs;
		}

		/* MERGE 不支持 RETURNING        */
		Assert(node->returningLists == NIL);
	}
}

/*
 * 为 MERGE 动作初始化 ResultRelInfo 中的元组 slot。        
 *
 * 即便投影本身并不在此处初始化，仍标记              
 * 'projectNewInfoValid'    
 */
void
ExecInitMergeTupleSlots(ModifyTableState *mtstate,
						ResultRelInfo *resultRelInfo)
{
	EState	   *estate = mtstate->ps.state;

	Assert(!resultRelInfo->ri_projectNewInfoValid);

	resultRelInfo->ri_oldTupleSlot =
		table_slot_create(resultRelInfo->ri_RelationDesc,
						  &estate->es_tupleTable);
	resultRelInfo->ri_newTupleSlot =
		table_slot_create(resultRelInfo->ri_RelationDesc,
						  &estate->es_tupleTable);
	resultRelInfo->ri_projectNewInfoValid = true;
}

/*
 * 处理 BEFORE EACH STATEMENT 触发器
 */
static void
fireBSTriggers(ModifyTableState *node)
{
	ModifyTable *plan = (ModifyTable *) node->ps.plan;
	ResultRelInfo *resultRelInfo = node->rootResultRelInfo;

	switch (node->operation)
	{
		case CMD_INSERT:
			ExecBSInsertTriggers(node->ps.state, resultRelInfo);
			if (plan->onConflictAction == ONCONFLICT_UPDATE)
				ExecBSUpdateTriggers(node->ps.state,
									 resultRelInfo);
			break;
		case CMD_UPDATE:
			ExecBSUpdateTriggers(node->ps.state, resultRelInfo);
			break;
		case CMD_DELETE:
			ExecBSDeleteTriggers(node->ps.state, resultRelInfo);
			break;
		case CMD_MERGE:
			if (node->mt_merge_subcommands & MERGE_INSERT)
				ExecBSInsertTriggers(node->ps.state, resultRelInfo);
			if (node->mt_merge_subcommands & MERGE_UPDATE)
				ExecBSUpdateTriggers(node->ps.state, resultRelInfo);
			if (node->mt_merge_subcommands & MERGE_DELETE)
				ExecBSDeleteTriggers(node->ps.state, resultRelInfo);
			break;
		default:
			elog(ERROR, "unknown operation");
			break;
	}
}

/*
 * 处理 AFTER EACH STATEMENT 触发器
 */
static void
fireASTriggers(ModifyTableState *node)
{
	ModifyTable *plan = (ModifyTable *) node->ps.plan;
	ResultRelInfo *resultRelInfo = node->rootResultRelInfo;

	switch (node->operation)
	{
		case CMD_INSERT:
			if (plan->onConflictAction == ONCONFLICT_UPDATE)
				ExecASUpdateTriggers(node->ps.state,
									 resultRelInfo,
									 node->mt_oc_transition_capture);
			ExecASInsertTriggers(node->ps.state, resultRelInfo,
								 node->mt_transition_capture);
			break;
		case CMD_UPDATE:
			ExecASUpdateTriggers(node->ps.state, resultRelInfo,
								 node->mt_transition_capture);
			break;
		case CMD_DELETE:
			ExecASDeleteTriggers(node->ps.state, resultRelInfo,
								 node->mt_transition_capture);
			break;
		case CMD_MERGE:
			if (node->mt_merge_subcommands & MERGE_DELETE)
				ExecASDeleteTriggers(node->ps.state, resultRelInfo,
									 node->mt_transition_capture);
			if (node->mt_merge_subcommands & MERGE_UPDATE)
				ExecASUpdateTriggers(node->ps.state, resultRelInfo,
									 node->mt_transition_capture);
			if (node->mt_merge_subcommands & MERGE_INSERT)
				ExecASInsertTriggers(node->ps.state, resultRelInfo,
									 node->mt_transition_capture);
			break;
		default:
			elog(ERROR, "unknown operation");
			break;
	}
}

/*
 * 设置收集 AFTER 触发器过渡元组所需的                 
 * 状态。
 */
static void
ExecSetupTransitionCaptureState(ModifyTableState *mtstate, EState *estate)
{
	ModifyTable *plan = (ModifyTable *) mtstate->ps.plan;
	ResultRelInfo *targetRelInfo = mtstate->rootResultRelInfo;

	/* 检查直接目标关系上的过渡表。                     */
	mtstate->mt_transition_capture =
		MakeTransitionCaptureState(targetRelInfo->ri_TrigDesc,
								   RelationGetRelid(targetRelInfo->ri_RelationDesc),
								   mtstate->operation);
	if (plan->operation == CMD_INSERT &&
		plan->onConflictAction == ONCONFLICT_UPDATE)
		mtstate->mt_oc_transition_capture =
			MakeTransitionCaptureState(targetRelInfo->ri_TrigDesc,
									   RelationGetRelid(targetRelInfo->ri_RelationDesc),
									   CMD_UPDATE);
}

/*
 * ExecPrepareTupleRouting --- 为元组路由做准备     
 *
 * 确定 slot 中的元组要插入哪个分区                        
 * 并把其 ResultRelInfo 存入 *partRelInfo。返回值是一个  
 * 容纳该分区行类型元组的 slot。         
 *
 * 这还会根据所选分区，在 mtstate 中设置过渡表           
 * 信息。          
 */
static TupleTableSlot *
ExecPrepareTupleRouting(ModifyTableState *mtstate,
						EState *estate,
						PartitionTupleRouting *proute,
						ResultRelInfo *targetRelInfo,
						TupleTableSlot *slot,
						ResultRelInfo **partRelInfo)
{
	ResultRelInfo *partrel;
	TupleConversionMap *map;

	/*
	 * 查找目标分区的 ResultRelInfo。若 ExecFindPartition            
	 * 为 'slot' 中的元组找不到有效分区则报错。           
	 * 若找到的分区不是 INSERT 的有效目标也会报错。      
	 * 这一检查是必需的，因为分区表 UPDATE 到另一分区    
	 * 会变成 DELETE+INSERT。                          
	 */
	partrel = ExecFindPartition(mtstate, targetRelInfo, proute, slot, estate);

	/*
	 * 若正在收集过渡元组，可能需要把分区行类型转换为  
	 * 根分区表的行类型。但如果该分区上没有能改变     
	 * 元组的 BEFORE 触发器，我们可以直接记住原始未转换   
	 * 元组，以免做一次不必要的                                
	 * 往返转换。       
	 */
	if (mtstate->mt_transition_capture != NULL)
	{
		bool		has_before_insert_row_trig;

		has_before_insert_row_trig = (partrel->ri_TrigDesc &&
									  partrel->ri_TrigDesc->trig_insert_before_row);

		mtstate->mt_transition_capture->tcs_original_insert_tuple =
			!has_before_insert_row_trig ? slot : NULL;
	}

	/*
	 * 如有必要则转换元组。  
	 */
	map = ExecGetRootToChildMap(partrel, estate);
	if (map != NULL)
	{
		TupleTableSlot *new_slot = partrel->ri_PartitionTupleSlot;

		slot = execute_attr_map_slot(map->attrMap, slot, new_slot);
	}

	*partRelInfo = partrel;
	return slot;
}

/* ----------------------------------------------------------------
 *	   ExecModifyTable
 *
 *		按需执行表修改，并在需要时返回 RETURNING              
 *		结果。 
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecModifyTable(PlanState *pstate)
{
	ModifyTableState *node = castNode(ModifyTableState, pstate);
	ModifyTableContext context;
	EState	   *estate = node->ps.state;
	CmdType		operation = node->operation;
	ResultRelInfo *resultRelInfo;
	PlanState  *subplanstate;
	TupleTableSlot *slot;
	TupleTableSlot *oldSlot;
	ItemPointerData tuple_ctid;
	HeapTupleData oldtupdata;
	HeapTuple	oldtuple;
	ItemPointer tupleid;
	bool		tuplock;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 本函数不应在 EvalPlanQual 期间被调用；                     
	 * 而应把子计划树传给 EvalPlanQual。要用运行时测试   
	 * 而非仅靠 Assert，因为测试中极易漏掉此情形。    
	 * （注意：虽然 ModifyTable 不应在 EvalPlanQual 中执行， 
	 * 但我们必须允许其被初始化和关闭，以防它位于     
	 * CTE 子计划中，因此该测试必须放在此处而不是         
	 * ExecInitModifyTable  
	 */
	if (estate->es_epq_active != NULL)
		elog(ERROR, "ModifyTable should not be called during EvalPlanQual");

	/*
	 * 若已处理完毕就不要再做。我们需要这一测试，      
	 * 因为 ExecPostprocessPlan 可能会额外多调用我们一次，    
	 * 而我们的子计划节点未必经得起额外                  
	 * 调用。   
	 */
	if (node->mt_done)
		return NULL;

	/*
	 * 首次调用时先触发 BEFORE STATEMENT 触发器再继续。 
	 */
	if (node->fireBSTriggers)
	{
		fireBSTriggers(node);
		node->fireBSTriggers = false;
	}

	/* 预加载局部变量   */
	resultRelInfo = node->resultRelInfo + node->mt_lastResultIndex;
	subplanstate = outerPlanState(node);

	/* 设全局上下文 */
	context.mtstate = node;
	context.epqstate = &node->mt_epqstate;
	context.estate = estate;

	/*
	 * 从子计划取出各行，并对每行执行所需的              
	 * 表修改。 
	 */
	for (;;)
	{
		/*
		 * 重置每个输出元组的 exprcontext。之所以需要，   
		 * 是因为触发器要把该上下文当作工作区使用。      
		 * 不过把这一步放在计划顶层之下略显别扭，          
		 * 日后或需斟酌。 
		 */
		ResetPerTupleExprContext(estate);

		/*
		 * 重置用于处理 ON CONFLICT 与 RETURNING 子句的            
		 * 每元组内存上下文，以释放上一轮分配的      
		 * 表达式求值存储。        
		 */
		if (pstate->ps_ExprContext)
			ResetExprContext(pstate->ps_ExprContext);

		context.planSlot = ExecProcNode(subplanstate);

		/* 已无待处理元组？   */
		if (TupIsNull(context.planSlot))
			break;

		/*
		 * 当有多个结果关系时，每个元组都含 junk 列，   
		 * 它给出元组来源关系的 OID。需提取该值，      
		 * 并据此选出正确的结果关系。           
		 */
		if (AttributeNumberIsValid(node->mt_resultOidAttno))
		{
			Datum		datum;
			bool		isNull;
			Oid			resultoid;

			datum = ExecGetJunkAttribute(context.planSlot, node->mt_resultOidAttno,
										 &isNull);
			if (isNull)
			{
				/*
				 * 对 MERGE 以外的命令，tableoid 为 InvalidOid 的    
				 * 元组都是错误。对 MERGE，可能需把它们按    
				 * WHEN NOT MATCHED 子句处理，若有则照做。  
				 *
				 * 注意我们使用节点的顶层 resultRelInfo，         
				 * 非特定分区的。
				 */
				if (operation == CMD_MERGE)
				{
					EvalPlanQualSetSlot(&node->mt_epqstate, context.planSlot);

					ExecMerge(&context, node->resultRelInfo, NULL, node->canSetTag);
					continue;	/* 尚不支持 RETURNING   */
				}

				elog(ERROR, "tableoid is NULL");
			}
			resultoid = DatumGetObjectId(datum);

			/* 若与上次不同，则需要重新定位关系             */
			if (resultoid != node->mt_lastResultOid)
				resultRelInfo = ExecLookupResultRelByOid(node, resultoid,
														 false, true);
		}

		/*
		 * 若 resultRelInfo->ri_usesFdwDirectModify 为真，我们在此    
		 * 只需计算 RETURNING 表达式。       
		 */
		if (resultRelInfo->ri_usesFdwDirectModify)
		{
			Assert(resultRelInfo->ri_projectReturning);

			/*
			 * 实际插入、更新或删除数据的扫描 slot 已由  
			 * IterateDirectModify 提供给 ExecProcessReturning，
			 * 因此无需                                              
			 * 在此提供。 
			 */
			slot = ExecProcessReturning(resultRelInfo, NULL, context.planSlot);

			return slot;
		}

		EvalPlanQualSetSlot(&node->mt_epqstate, context.planSlot);
		slot = context.planSlot;

		tupleid = NULL;
		oldtuple = NULL;

		/*
		 * 对 UPDATE/DELETE/MERGE，要取回待更新/删除/合并        
		 * 元组的行标识信息。堆关系对应 TID；否则可能是 
		 * 可整体携带旧元组的 wholerow junk 属性。请与      
		 * ExecInitModifyTable 设置 ri_RowIdAttNo 的部分
		 * 保持同步。                                
		 */
		if (operation == CMD_UPDATE || operation == CMD_DELETE ||
			operation == CMD_MERGE)
		{
			char		relkind;
			Datum		datum;
			bool		isNull;

			relkind = resultRelInfo->ri_RelationDesc->rd_rel->relkind;
			if (relkind == RELKIND_RELATION ||
				relkind == RELKIND_MATVIEW ||
				relkind == RELKIND_PARTITIONED_TABLE)
			{
				/* ri_RowIdAttNo 指向一个 ctid 属性   */
				Assert(AttributeNumberIsValid(resultRelInfo->ri_RowIdAttNo));
				datum = ExecGetJunkAttribute(slot,
											 resultRelInfo->ri_RowIdAttNo,
											 &isNull);

				/*
				 * 对 MERGE 以外的命令，任何行标识为空的元组 
				 * 都是错误。对 MERGE，我们可能需把它们按  
				 * WHEN NOT MATCHED 子句处理，若有则照做。  
				 *
				 * 注意我们使用节点的顶层 resultRelInfo，         
				 * 非特定分区的。
				 */
				if (isNull)
				{
					if (operation == CMD_MERGE)
					{
						EvalPlanQualSetSlot(&node->mt_epqstate, context.planSlot);

						ExecMerge(&context, node->resultRelInfo, NULL, node->canSetTag);
						continue;	/* 尚不支持 RETURNING   */
					}

					elog(ERROR, "ctid is NULL");
				}

				tupleid = (ItemPointer) DatumGetPointer(datum);
				tuple_ctid = *tupleid;	/* 切记不要释放 ctid！   */
				tupleid = &tuple_ctid;
			}

			/*
			 * 在可用时利用 wholerow 属性来重建旧关系元组。 
			 * 旧元组承担以下两项用途之一或两者：         
			 * 1) 作为行触发器的 OLD 元组，2) 为 UPDATE 的 NEW    
			 * 元组提供未改动列的值，因为子计划              
			 * 并不会产出                                                
			 * 目标表各列。  
			 *
			 * 注意 wholerow 属性不携带系统列，                    
			 * 外表触发器看不到它们；但我们在此足以     
			 * 设置 t_tableOid。另外与此完全无关的是，     
			 * FDW 也可能取自己的 junk 属性来识别该行。        
			 *
			 * 其他相关 relkind（目前仅限于有 INSTEAD OF      
			 * 触发器的视图）总是拥有 wholerow 属性。  
			 */
			else if (AttributeNumberIsValid(resultRelInfo->ri_RowIdAttNo))
			{
				datum = ExecGetJunkAttribute(slot,
											 resultRelInfo->ri_RowIdAttNo,
											 &isNull);
				/* 按理不应得到空结果……   */
				if (isNull)
					elog(ERROR, "wholerow is NULL");

				oldtupdata.t_data = DatumGetHeapTupleHeader(datum);
				oldtupdata.t_len =
					HeapTupleHeaderGetDatumLength(oldtupdata.t_data);
				ItemPointerSetInvalid(&(oldtupdata.t_self));
				/* 按惯例视图触发器看到的 t_tableOid 无效 */
				oldtupdata.t_tableOid =
					(relkind == RELKIND_VIEW) ? InvalidOid :
					RelationGetRelid(resultRelInfo->ri_RelationDesc);

				oldtuple = &oldtupdata;
			}
			else
			{
				/* 只有外表才允许省略行标识属性            */
				Assert(relkind == RELKIND_FOREIGN_TABLE);
			}
		}

		switch (operation)
		{
			case CMD_INSERT:
				/* 若该表是首次处理，则初始化投影信息     */
				if (unlikely(!resultRelInfo->ri_projectNewInfoValid))
					ExecInitInsertProjection(node, resultRelInfo);
				slot = ExecGetInsertNewTuple(resultRelInfo, context.planSlot);
				slot = ExecInsert(&context, resultRelInfo, slot,
								  node->canSetTag, NULL, NULL);
				break;

			case CMD_UPDATE:
				tuplock = false;

				/* 若该表是首次处理，则初始化投影信息     */
				if (unlikely(!resultRelInfo->ri_projectNewInfoValid))
					ExecInitUpdateProjection(node, resultRelInfo);

				/*
				 * 把计划输出元组与被更新的旧元组组合，  
				 * 以构造新元组。       
				 */
				oldSlot = resultRelInfo->ri_oldTupleSlot;
				if (oldtuple != NULL)
				{
					Assert(!resultRelInfo->ri_needLockTagTuple);
					/* 用 wholerow junk 属性作为旧元组。   */
					ExecForceStoreHeapTuple(oldtuple, oldSlot, false);
				}
				else
				{
					/* 取回旧元组的最新版本。           */
					Relation	relation = resultRelInfo->ri_RelationDesc;

					if (resultRelInfo->ri_needLockTagTuple)
					{
						LockTuple(relation, tupleid, InplaceUpdateTupleLock);
						tuplock = true;
					}
					if (!table_tuple_fetch_row_version(relation, tupleid,
													   SnapshotAny,
													   oldSlot))
						elog(ERROR, "failed to fetch tuple being updated");
				}
				slot = ExecGetUpdateNewTuple(resultRelInfo, context.planSlot,
											 oldSlot);
				context.relaction = NULL;

				/* 现在执行更新。 */
				slot = ExecUpdate(&context, resultRelInfo, tupleid, oldtuple,
								  slot, node->canSetTag);
				if (tuplock)
					UnlockTuple(resultRelInfo->ri_RelationDesc, tupleid,
								InplaceUpdateTupleLock);
				break;

			case CMD_DELETE:
				slot = ExecDelete(&context, resultRelInfo, tupleid, oldtuple,
								  true, false, node->canSetTag, NULL, NULL, NULL);
				break;

			case CMD_MERGE:
				slot = ExecMerge(&context, resultRelInfo, tupleid, node->canSetTag);
				break;

			default:
				elog(ERROR, "unknown operation");
				break;
		}

		/*
		 * 若得到 RETURNING 结果，就把它返回给调用者。下次 
		 * 调用时继续。    
		 */
		if (slot)
			return slot;
	}

	/*
	 * 插入剩余元组完成批量插入。  
	 */
	if (estate->es_insert_pending_result_relations != NIL)
		ExecPendingInserts(estate);

	/*
	 * 已完成，退出前须触发 AFTER STATEMENT 触发器。  
	 */
	fireASTriggers(node);

	node->mt_done = true;

	return NULL;
}

/*
 * ExecLookupResultRelByOid
 * 		若给定 OID 的表属于该 ModifyTable 节点要更新的   
 * 		结果关系之一，则返回其 ResultRelInfo。              
 *
 * 未找到时 missing_ok 则返回 NULL，否则报错。  
 *
 * 若 update_cache 为真，则查找成功后会更新节点的         
 * 单元素缓存。只有 ExecModifyTable 可为此传真值。  
 */
ResultRelInfo *
ExecLookupResultRelByOid(ModifyTableState *node, Oid resultoid,
						 bool missing_ok, bool update_cache)
{
	if (node->mt_resultOidHash)
	{
		/* 用预建的哈希表来定位关系           */
		MTTargetRelLookup *mtlookup;

		mtlookup = (MTTargetRelLookup *)
			hash_search(node->mt_resultOidHash, &resultoid, HASH_FIND, NULL);
		if (mtlookup)
		{
			if (update_cache)
			{
				node->mt_lastResultOid = resultoid;
				node->mt_lastResultIndex = mtlookup->relationIndex;
			}
			return node->resultRelInfo + mtlookup->relationIndex;
		}
	}
	else
	{
		/* 目标关系较少时，直接搜索 ResultRelInfo 数组 */
		for (int ndx = 0; ndx < node->mt_nrels; ndx++)
		{
			ResultRelInfo *rInfo = node->resultRelInfo + ndx;

			if (RelationGetRelid(rInfo->ri_RelationDesc) == resultoid)
			{
				if (update_cache)
				{
					node->mt_lastResultOid = resultoid;
					node->mt_lastResultIndex = ndx;
				}
				return rInfo;
			}
		}
	}

	if (!missing_ok)
		elog(ERROR, "incorrect result relation OID %u", resultoid);
	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitModifyTable
 * ----------------------------------------------------------------
 */
ModifyTableState *
ExecInitModifyTable(ModifyTable *node, EState *estate, int eflags)
{
	ModifyTableState *mtstate;
	Plan	   *subplan = outerPlan(node);
	CmdType		operation = node->operation;
	int			nrels = list_length(node->resultRelations);
	ResultRelInfo *resultRelInfo;
	List	   *arowmarks;
	ListCell   *l;
	int			i;
	Relation	rel;

	/* 检查不支持的标志位 */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 创建状态结构    
	 */
	mtstate = makeNode(ModifyTableState);
	mtstate->ps.plan = (Plan *) node;
	mtstate->ps.state = estate;
	mtstate->ps.ExecProcNode = ExecModifyTable;

	mtstate->operation = operation;
	mtstate->canSetTag = node->canSetTag;
	mtstate->mt_done = false;

	mtstate->mt_nrels = nrels;
	mtstate->resultRelInfo = (ResultRelInfo *)
		palloc(nrels * sizeof(ResultRelInfo));

	mtstate->mt_merge_inserted = 0;
	mtstate->mt_merge_updated = 0;
	mtstate->mt_merge_deleted = 0;

	/*----------
	 * 解析目标关系。这等价于：             
	 *
	 * - 我们将对其触发 FOR STATEMENT 触发器的关系，  
	 * - 收集的过渡元组都必须转换成其元组格式的关系， 
	 *   以及                
	 * - 用于元组路由的根分区表。              
	 *
	 * 若它是分区表或继承表，则根分区或 appendrel RTE 
	 * 不会出现在计划的其他位置，其 RT 索引由               
	 * node->rootRelation 显式给出。否则，目标关系就是          
	 * node->resultRelations 列表中唯一的关系。      
	 *----------
	 */
	if (node->rootRelation > 0)
	{
		mtstate->rootResultRelInfo = makeNode(ResultRelInfo);
		ExecInitResultRelation(estate, mtstate->rootResultRelInfo,
							   node->rootRelation);
	}
	else
	{
		Assert(list_length(node->resultRelations) == 1);
		mtstate->rootResultRelInfo = mtstate->resultRelInfo;
		ExecInitResultRelation(estate, mtstate->resultRelInfo,
							   linitial_int(node->resultRelations));
	}

	/* 暂时用哑子计划数据来设置 epqstate          */
	EvalPlanQualInit(&mtstate->mt_epqstate, estate, NULL, NIL,
					 node->epqParam, node->resultRelations);
	mtstate->fireBSTriggers = true;

	/*
	 * 构建收集过渡元组的状态。这需要有有效的触发器   
	 * 查询上下文，故在 explain-only 模式下跳过。     
	 */
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		ExecSetupTransitionCaptureState(mtstate, estate);

	/*
	 * 打开所有结果关系并初始化 ResultRelInfo 结构。           
	 * 但根关系若属于该数组，则已在上面初始化过。      
	 * 我们必须在初始化子计划之前完成这一步，因为       
	 * direct-modify FDW 要求 ResultRelInfo 可用。 
	 */
	resultRelInfo = mtstate->resultRelInfo;
	i = 0;
	foreach(l, node->resultRelations)
	{
		Index		resultRelation = lfirst_int(l);
		List	   *mergeActions = NIL;

		if (node->mergeActionLists)
			mergeActions = list_nth(node->mergeActionLists, i);

		if (resultRelInfo != mtstate->rootResultRelInfo)
		{
			ExecInitResultRelation(estate, resultRelInfo, resultRelation);

			/*
			 * 对子结果关系，要存储根结果关系指针。    
			 * 这是为了方便那些想查看查询原始目标关系、 
			 * 却没有 mtstate 可用的                                    
			 * 地方。     
			 */
			resultRelInfo->ri_RootResultRelInfo = mtstate->rootResultRelInfo;
		}

		/* 初始化 usesFdwDirectModify 标志    */
		resultRelInfo->ri_usesFdwDirectModify =
			bms_is_member(i, node->fdwDirectModifyPlans);

		/*
		 * 验证结果关系是当前操作的有效目标                  
		 */
		CheckValidResultRelNew(resultRelInfo, operation,
							   node->onConflictAction, mergeActions);

		resultRelInfo++;
		i++;
	}

	/*
	 * 现在可初始化子计划了。 
	 */
	outerPlanState(mtstate) = ExecInitNode(subplan, estate, eflags);

	/*
	 * 为每个结果关系做额外初始化。       
	 */
	for (i = 0; i < nrels; i++)
	{
		resultRelInfo = &mtstate->resultRelInfo[i];

		/* 让 FDW 为外表结果关系自行初始化           */
		if (!resultRelInfo->ri_usesFdwDirectModify &&
			resultRelInfo->ri_FdwRoutine != NULL &&
			resultRelInfo->ri_FdwRoutine->BeginForeignModify != NULL)
		{
			List	   *fdw_private = (List *) list_nth(node->fdwPrivLists, i);

			resultRelInfo->ri_FdwRoutine->BeginForeignModify(mtstate,
															 resultRelInfo,
															 fdw_private,
															 i,
															 eflags);
		}

		/*
		 * 对 UPDATE/DELETE/MERGE，现在要按 relkind 找出合适        
		 * 的 junk 属性，是 'ctid' 还是 'wholerow'。对外表而言，
		 * FDW 可能已创建了额外的 junk 属性，                 
		 * 但这与我们无关。     
		 */
		if (operation == CMD_UPDATE || operation == CMD_DELETE ||
			operation == CMD_MERGE)
		{
			char		relkind;

			relkind = resultRelInfo->ri_RelationDesc->rd_rel->relkind;
			if (relkind == RELKIND_RELATION ||
				relkind == RELKIND_MATVIEW ||
				relkind == RELKIND_PARTITIONED_TABLE)
			{
				resultRelInfo->ri_RowIdAttNo =
					ExecFindJunkAttributeInTlist(subplan->targetlist, "ctid");
				if (!AttributeNumberIsValid(resultRelInfo->ri_RowIdAttNo))
					elog(ERROR, "could not find junk ctid column");
			}
			else if (relkind == RELKIND_FOREIGN_TABLE)
			{
				/*
				 * 我们不支持外表上的 MERGE。（其实现依赖    
				 * CTID，因此有问题。）                      
				 */
				Assert(operation != CMD_MERGE);

				/*
				 * 有行级触发器时就应有 wholerow 属性。   
				 * 我们还要求 UPDATE 和 MERGE 中也存在它，     
				 * 以便能取到未改动列的                         
				 * 值。  
				 */
				resultRelInfo->ri_RowIdAttNo =
					ExecFindJunkAttributeInTlist(subplan->targetlist,
												 "wholerow");
				if ((mtstate->operation == CMD_UPDATE || mtstate->operation == CMD_MERGE) &&
					!AttributeNumberIsValid(resultRelInfo->ri_RowIdAttNo))
					elog(ERROR, "could not find junk wholerow column");
			}
			else
			{
				/* 不支持 MERGE      */
				Assert(operation != CMD_MERGE);
				/* 其他有效目标 relkind 必须提供 wholerow  */
				resultRelInfo->ri_RowIdAttNo =
					ExecFindJunkAttributeInTlist(subplan->targetlist,
												 "wholerow");
				if (!AttributeNumberIsValid(resultRelInfo->ri_RowIdAttNo))
					elog(ERROR, "could not find junk wholerow column");
			}
		}
	}

	/*
	 * 若这是继承的 update/delete/merge，则会有一个名为   
	 * "tableoid" 的 junk 属性。它将用于识别给定待           
	 * 更新/删除/合并的元组所对应的                           
	 * 结果关系。        
	 */
	mtstate->mt_resultOidAttno =
		ExecFindJunkAttributeInTlist(subplan->targetlist, "tableoid");
	Assert(AttributeNumberIsValid(mtstate->mt_resultOidAttno) || nrels == 1);
	mtstate->mt_lastResultOid = InvalidOid; /* 首个元组强制查找    */
	mtstate->mt_lastResultIndex = 0;	/* 若无此属性则为 0      */

	/* 获取根目标关系        */
	rel = mtstate->rootResultRelInfo->ri_RelationDesc;

	/*
	 * 若是分区表 INSERT，则构建元组路由状态。UPDATE         
	 * 或 MERGE 也可能需要它，但仅当其确实在分区间     
	 * 移动元组时，由下述函数完成设置： 
	 * ExecCrossPartitionUpdate.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
		operation == CMD_INSERT)
		mtstate->mt_partition_tuple_routing =
			ExecSetupPartitionTupleRouting(estate, rel);

	/*
	 * 如有需要，初始化 WITH CHECK OPTION 约束。   
	 */
	resultRelInfo = mtstate->resultRelInfo;
	foreach(l, node->withCheckOptionLists)
	{
		List	   *wcoList = (List *) lfirst(l);
		List	   *wcoExprs = NIL;
		ListCell   *ll;

		foreach(ll, wcoList)
		{
			WithCheckOption *wco = (WithCheckOption *) lfirst(ll);
			ExprState  *wcoExpr = ExecInitQual((List *) wco->qual,
											   &mtstate->ps);

			wcoExprs = lappend(wcoExprs, wcoExpr);
		}

		resultRelInfo->ri_WithCheckOptions = wcoList;
		resultRelInfo->ri_WithCheckOptionExprs = wcoExprs;
		resultRelInfo++;
	}

	/*
	 * 需要时初始化 RETURNING 投影。     
	 */
	if (node->returningLists)
	{
		TupleTableSlot *slot;
		ExprContext *econtext;

		/*
		 * 初始化结果元组 slot，并使用第一个 RETURNING 列表   
		 * 来指定其行类型。假定其余的都相同。    
		 */
		mtstate->ps.plan->targetlist = (List *) linitial(node->returningLists);

		/* 为 RETURNING 投影的输出设置一个 slot              */
		ExecInitResultTupleSlotTL(&mtstate->ps, &TTSOpsVirtual);
		slot = mtstate->ps.ps_ResultTupleSlot;

		/* 也需要 econtext   */
		if (mtstate->ps.ps_ExprContext == NULL)
			ExecAssignExprContext(estate, &mtstate->ps);
		econtext = mtstate->ps.ps_ExprContext;

		/*
		 * 为每个结果关系构建投影。   
		 */
		resultRelInfo = mtstate->resultRelInfo;
		foreach(l, node->returningLists)
		{
			List	   *rlist = (List *) lfirst(l);

			resultRelInfo->ri_returningList = rlist;
			resultRelInfo->ri_projectReturning =
				ExecBuildProjectionInfo(rlist, econtext, slot, &mtstate->ps,
										resultRelInfo->ri_RelationDesc->rd_att);
			resultRelInfo++;
		}
	}
	else
	{
		/*
		 * 我们仍须构造一个哑结果元组类型，因为 InitPlan    
		 * 需要一个（也许该改掉？）。 
		 */
		mtstate->ps.plan->targetlist = NIL;
		ExecInitResultTypeTL(&mtstate->ps);

		mtstate->ps.ps_ExprContext = NULL;
	}

	/* ON CONFLICT 需要时设置仲裁索引列表             */
	resultRelInfo = mtstate->resultRelInfo;
	if (node->onConflictAction != ONCONFLICT_NONE)
	{
		/* insert 只能有一个关系，继承未展开                 */
		Assert(nrels == 1);
		resultRelInfo->ri_onConflictArbiterIndexes = node->arbiterIndexes;
	}

	/*
	 * 需要时为 ON CONFLICT DO UPDATE 初始化目标列表、投影     
	 * 与qual。
	 */
	if (node->onConflictAction == ONCONFLICT_UPDATE)
	{
		OnConflictSetState *onconfl = makeNode(OnConflictSetState);
		ExprContext *econtext;
		TupleDesc	relationDesc;

		/* 若由上面的 RETURNING 处理创建，则已存在   */
		if (mtstate->ps.ps_ExprContext == NULL)
			ExecAssignExprContext(estate, &mtstate->ps);

		econtext = mtstate->ps.ps_ExprContext;
		relationDesc = resultRelInfo->ri_RelationDesc->rd_att;

		/* 为 DO UPDATE SET 操作创建状态     */
		resultRelInfo->ri_onConflict = onconfl;

		/* 为现有元组初始化 slot          */
		onconfl->oc_Existing =
			table_slot_create(resultRelInfo->ri_RelationDesc,
							  &mtstate->ps.state->es_tupleTable);

		/*
		 * 为 UPDATE SET 投影创建元组 slot。这里想要表类型     
		 * 的 slot，因为它将用于向表插入，以及用于         
		 * RETURNING 处理，后者可能访问                          
		 * 系统属性。   
		 */
		onconfl->oc_ProjSlot =
			table_slot_create(resultRelInfo->ri_RelationDesc,
							  &mtstate->ps.state->es_tupleTable);

		/* 构建 UPDATE SET 投影状态    */
		onconfl->oc_ProjInfo =
			ExecBuildUpdateProjection(node->onConflictSet,
									  true,
									  node->onConflictCols,
									  relationDesc,
									  econtext,
									  onconfl->oc_ProjSlot,
									  &mtstate->ps);

		/* 初始化 WHERE 子句求值（若有）的状态     */
		if (node->onConflictWhere)
		{
			ExprState  *qualexpr;

			qualexpr = ExecInitQual((List *) node->onConflictWhere,
									&mtstate->ps);
			onconfl->oc_WhereClause = qualexpr;
		}
	}

	/*
	 * 若 UPDATE 或 DELETE 中有次要关系，则需要把                
	 * 它们当作 SELECT FOR UPDATE 中未加锁的关系对待，       
	 * 即需要把 EvalPlanQual 机制告知它们。对 MERGE 的           
	 * 源关系也是如此。请找出相关的 ExecRowMark。          
	 */
	arowmarks = NIL;
	foreach(l, node->rowMarks)
	{
		PlanRowMark *rc = lfirst_node(PlanRowMark, l);
		ExecRowMark *erm;
		ExecAuxRowMark *aerm;

		/* 忽略 "parent" rowmark；它们在运行时无关紧要 */
		if (rc->isParent)
			continue;

		/* 找 ExecRowMark 并建 ExecAuxRowMark     */
		erm = ExecFindRowMark(estate, rc->rti, false);
		aerm = ExecBuildAuxRowMark(erm, subplan->targetlist);
		arowmarks = lappend(arowmarks, aerm);
	}

	/* 对 MERGE 命令，初始化其状态     */
	if (mtstate->operation == CMD_MERGE)
		ExecInitMerge(mtstate, estate);

	EvalPlanQualSetPlan(&mtstate->mt_epqstate, subplan, arowmarks);

	/*
	 * 若结果关系很多，就用哈希表来加速查找。            
	 * 若不多，简单的线性搜索反而更快。                   
	 *
	 * 阈值在哪里并不清楚，先尝试 64。在调试构建中      
	 * 使用小阈值，以便两条代码路径都得到测试      
	 * 覆盖。                   
	 */
#ifdef USE_ASSERT_CHECKING
#define MT_NRELS_HASH 4
#else
#define MT_NRELS_HASH 64
#endif
	if (nrels >= MT_NRELS_HASH)
	{
		HASHCTL		hash_ctl;

		hash_ctl.keysize = sizeof(Oid);
		hash_ctl.entrysize = sizeof(MTTargetRelLookup);
		hash_ctl.hcxt = CurrentMemoryContext;
		mtstate->mt_resultOidHash =
			hash_create("ModifyTable target hash",
						nrels, &hash_ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		for (i = 0; i < nrels; i++)
		{
			Oid			hashkey;
			MTTargetRelLookup *mtlookup;
			bool		found;

			resultRelInfo = &mtstate->resultRelInfo[i];
			hashkey = RelationGetRelid(resultRelInfo->ri_RelationDesc);
			mtlookup = (MTTargetRelLookup *)
				hash_search(mtstate->mt_resultOidHash, &hashkey,
							HASH_ENTER, &found);
			Assert(!found);
			mtlookup->relationIndex = i;
		}
	}
	else
		mtstate->mt_resultOidHash = NULL;

	/*
	 * 确定 FDW 是否支持批量插入，并确定批量大小            
	 * （FDW 可能支持批处理，但可能对服务器/表   
	 * 禁用）。  
	 *
	 * 我们只对 INSERT 这样做，因此 UPDATE/DELETE 的批量      
	 * 大小仍为 0。
	 */
	if (operation == CMD_INSERT)
	{
		/* insert 只能有一个关系，继承未展开                 */
		Assert(nrels == 1);
		resultRelInfo = mtstate->resultRelInfo;
		if (!resultRelInfo->ri_usesFdwDirectModify &&
			resultRelInfo->ri_FdwRoutine != NULL &&
			resultRelInfo->ri_FdwRoutine->GetForeignModifyBatchSize &&
			resultRelInfo->ri_FdwRoutine->ExecForeignBatchInsert)
		{
			resultRelInfo->ri_BatchSize =
				resultRelInfo->ri_FdwRoutine->GetForeignModifyBatchSize(resultRelInfo);
			Assert(resultRelInfo->ri_BatchSize >= 1);
		}
		else
			resultRelInfo->ri_BatchSize = 1;
	}

	/*
	 * 最后，若这不是主（canSetTag）ModifyTable 节点，把它     
	 * 加入 estate->es_auxmodifytables，使其会由                     
	 * ExecPostprocessPlan 运行至完成。（其实加入主         
	 * ModifyTable 节点也可以，但没有必要。）注意用的是    
	 * lcons 而非 lappend：我们需要后初始化的                  
	 * ModifyTable 节点先关闭。这可确保不丢弃                  
	 * 后续 CTE 子计划要看到的 RETURNING 行。
	 */
	if (!mtstate->canSetTag)
		estate->es_auxmodifytables = lcons(mtstate,
										   estate->es_auxmodifytables);

	return mtstate;
}

/* ----------------------------------------------------------------
 *		ExecEndModifyTable
 *
 *		关闭该计划。  
 *
 *		无值得关注的返回。 
 * ----------------------------------------------------------------
 */
void
ExecEndModifyTable(ModifyTableState *node)
{
	int			i;

	/*
	 * 让各 FDW 关闭          
	 */
	for (i = 0; i < node->mt_nrels; i++)
	{
		int			j;
		ResultRelInfo *resultRelInfo = node->resultRelInfo + i;

		if (!resultRelInfo->ri_usesFdwDirectModify &&
			resultRelInfo->ri_FdwRoutine != NULL &&
			resultRelInfo->ri_FdwRoutine->EndForeignModify != NULL)
			resultRelInfo->ri_FdwRoutine->EndForeignModify(node->ps.state,
														   resultRelInfo);

		/*
		 * 清理已初始化的批量 slot。这只对支持批处理的 
		 * FDW 有意义，其他情形下 ri_NumSlotsInitialized             
		 * == 0.
		 */
		for (j = 0; j < resultRelInfo->ri_NumSlotsInitialized; j++)
		{
			ExecDropSingleTupleTableSlot(resultRelInfo->ri_Slots[j]);
			ExecDropSingleTupleTableSlot(resultRelInfo->ri_PlanSlots[j]);
		}
	}

	/*
	 * 关闭所有分区表、叶子分区及其索引，并释放        
	 * 元组路由所用 slot（若已设置）。        
	 */
	if (node->mt_partition_tuple_routing)
	{
		ExecCleanupTupleRouting(node, node->mt_partition_tuple_routing);

		if (node->mt_root_tuple_slot)
			ExecDropSingleTupleTableSlot(node->mt_root_tuple_slot);
	}

	/*
	 * 释放 exprcontext  
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * 清空元组表          
	 */
	if (node->ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/*
	 * 若 EPQ 在执行则终止       
	 */
	EvalPlanQualEnd(&node->mt_epqstate);

	/*
	 * 关闭子计划  
	 */
	ExecEndNode(outerPlanState(node));
}

void
ExecReScanModifyTable(ModifyTableState *node)
{
	/*
	 * 当前我们不需要支持 ModifyTable 节点的重扫描。       
	 * 反正其语义也颇有争议。                 
	 */
	elog(ERROR, "ExecReScanModifyTable is not implemented");
}
