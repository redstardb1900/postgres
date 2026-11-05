/*-------------------------------------------------------------------------
 *
 * nodeLockRows.c
 *	  处理 FOR UPDATE/FOR SHARE 行锁定的例程     
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeLockRows.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *		ExecLockRows - 获取锁定的行
 *		ExecInitLockRows - 初始化节点及子节点.. 
 *		ExecEndLockRows - 关闭节点及子节点   
 */

#include "postgres.h"

#include "access/tableam.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "executor/nodeLockRows.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "utils/rel.h"


/* ----------------------------------------------------------------
 *		ExecLockRows
 * ----------------------------------------------------------------
 */
static TupleTableSlot *			/* 返回: 元组或 NULL  */
ExecLockRows(PlanState *pstate)
{
	LockRowsState *node = castNode(LockRowsState, pstate);
	TupleTableSlot *slot;
	EState	   *estate;
	PlanState  *outerPlan;
	bool		epq_needed;
	ListCell   *lc;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 从节点获取信息        
	 */
	estate = node->ps.state;
	outerPlan = outerPlanState(node);

	/*
	 * 从子计划取下一元组(若有) 
	 */
lnext:
	slot = ExecProcNode(outerPlan);

	if (TupIsNull(slot))
	{
		/* 退出前释放 EPQ 机制持有的所有资源            */
		EvalPlanQualEnd(&node->lr_epqstate);
		return NULL;
	}

	/* 除非取到更新后的元组版本，否则无需 EvalPlanQual  */
	epq_needed = false;

	/*
	 * 尝试锁定源元组。（注意，lr_arowMarks 中            
	 * 只有锁定型 rowmark）
	 */
	foreach(lc, node->lr_arowMarks)
	{
		ExecAuxRowMark *aerm = (ExecAuxRowMark *) lfirst(lc);
		ExecRowMark *erm = aerm->rowmark;
		Datum		datum;
		bool		isNull;
		ItemPointerData tid;
		TM_FailureData tmfd;
		LockTupleMode lockmode;
		int			lockflags = 0;
		TM_Result	test;
		TupleTableSlot *markSlot;

		/* 清除该 rel 遗留的测试元组        */
		markSlot = EvalPlanQualSlot(&node->lr_epqstate, erm->relation, erm->rti);
		ExecClearTuple(markSlot);

		/* 若是子 rel，必须检查其是否产生此行     */
		if (erm->rti != erm->prti)
		{
			Oid			tableoid;

			datum = ExecGetJunkAttribute(slot,
										 aerm->toidAttNo,
										 &isNull);
			/* 绝不该得到 NULL 结果..       */
			if (isNull)
				elog(ERROR, "tableoid is NULL");
			tableoid = DatumGetObjectId(datum);

			Assert(OidIsValid(erm->relid));
			if (tableoid != erm->relid)
			{
				/* 该子节点当前不活跃      */
				erm->ermActive = false;
				ItemPointerSetInvalid(&(erm->curCtid));
				continue;
			}
		}
		erm->ermActive = true;

		/* 获取元组的 ctid   */
		datum = ExecGetJunkAttribute(slot,
									 aerm->ctidAttNo,
									 &isNull);
		/* 绝不该得到 NULL 结果..       */
		if (isNull)
			elog(ERROR, "ctid is NULL");

		/* 针对外表的请求必须传递给其 FDW             */
		if (erm->relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		{
			FdwRoutine *fdwroutine;
			bool		updated = false;

			fdwroutine = GetFdwRoutineForRelation(erm->relation, false);
			/* 这本应已检查过，但为稳妥起见再查一次   */
			if (fdwroutine->RefetchForeignRow == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot lock rows in foreign table \"%s\"",
								RelationGetRelationName(erm->relation))));

			fdwroutine->RefetchForeignRow(estate,
										  erm,
										  datum,
										  markSlot,
										  &updated);
			if (TupIsNull(markSlot))
			{
				/* 无法获得锁，因此跳过该行    */
				goto lnext;
			}

			/*
			 * 若 FDW 表示元组在获得锁之前被更新过，我们就 
			 * 需要执行 EPQ 测试，看看 quals 是否仍满足 
			 */
			if (updated)
				epq_needed = true;

			continue;
		}

		/* 好，尝试锁定(并获取)该元组  */
		tid = *((ItemPointer) DatumGetPointer(datum));
		switch (erm->markType)
		{
			case ROW_MARK_EXCLUSIVE:
				lockmode = LockTupleExclusive;
				break;
			case ROW_MARK_NOKEYEXCLUSIVE:
				lockmode = LockTupleNoKeyExclusive;
				break;
			case ROW_MARK_SHARE:
				lockmode = LockTupleShare;
				break;
			case ROW_MARK_KEYSHARE:
				lockmode = LockTupleKeyShare;
				break;
			default:
				elog(ERROR, "unsupported rowmark type");
				lockmode = LockTupleNoKeyExclusive; /* 让编译器安静  */
				break;
		}

		lockflags = TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS;
		if (!IsolationUsesXactSnapshot())
			lockflags |= TUPLE_LOCK_FLAG_FIND_LAST_VERSION;

		test = table_tuple_lock(erm->relation, &tid, estate->es_snapshot,
								markSlot, estate->es_output_cid,
								lockmode, erm->waitPolicy,
								lockflags,
								&tmfd);

		switch (test)
		{
			case TM_WouldBlock:
				/* SKIP LOCKED 模式下无法锁定元组 */
				goto lnext;

			case TM_SelfModified:

				/*
				 * 目标元组已经被本命令更新或删除，      
				 * 或被当前事务中更晚的命令。              
				 * 前一种情况我们必须忽略该元组，         
				 * 以避免反复尝试更新造成"万圣节问题"，   
				 * 对后一种情况，改为取回更新后的元组或许
				 * 更合理，但这样做就需要修改 heap_update   
				 * 与 heap_delete，使其不抱怨更新"不可见" 
				 * 元组，这一点看起来相当吓人                
				 * (table_tuple_lock 不会抱怨，但很少有调用者   
				 * 预期 TM_Invisible，我们并非其中之一)。所以   
				 * 目前，将该元组视为已删除，不作处理。 
				 */
				goto lnext;

			case TM_Ok:

				/*
				 * 成功获得锁，锁定的元组保存在 markSlot 
				 * 中，以备下方进行 EvalPlanQual 测试。     
				 */
				if (tmfd.traversed)
					epq_needed = true;
				break;

			case TM_Updated:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent update")));
				elog(ERROR, "unexpected table_tuple_lock status: %u",
					 test);
				break;

			case TM_Deleted:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent update")));
				/* 元组已删除，因此不返回它 */
				goto lnext;

			case TM_Invisible:
				elog(ERROR, "attempted to lock invisible tuple");
				break;

			default:
				elog(ERROR, "unrecognized table_tuple_lock status: %u",
					 test);
		}

		/* 记住锁定元组的 TID，用于 EPQ 测试和 WHERE CURRENT OF*/
		erm->curCtid = tid;
	}

	/*
	 * 如需 EvalPlanQual 测试，则执行。    
	 */
	if (epq_needed)
	{
		/* 初始化 EPQ 机制     */
		EvalPlanQualBegin(&node->lr_epqstate);

		/*
		 * 要取回未锁定的源行，EPQ 逻辑需要访问              
		 * 被测元组中的 junk 列。      
		 */
		EvalPlanQualSetSlot(&node->lr_epqstate, slot);

		/*
		 * 最后即可重新评估该元组。     
		 */
		slot = EvalPlanQualNext(&node->lr_epqstate);
		if (TupIsNull(slot))
		{
			/* 更新后元组未通过 qual，忽略并继续  */
			goto lnext;
		}
	}

	/* 获得所有锁后，返回当前元组    */
	return slot;
}

/* ----------------------------------------------------------------
 *		ExecInitLockRows
 *
 *		它初始化 LockRows 节点状态结构与其         
 *		子计划。       
 * ----------------------------------------------------------------
 */
LockRowsState *
ExecInitLockRows(LockRows *node, EState *estate, int eflags)
{
	LockRowsState *lrstate;
	Plan	   *outerPlan = outerPlan(node);
	List	   *epq_arowmarks;
	ListCell   *lc;

	/* 检查不支持的标志    */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * 创建状态结构    
	 */
	lrstate = makeNode(LockRowsState);
	lrstate->ps.plan = (Plan *) node;
	lrstate->ps.state = estate;
	lrstate->ps.ExecProcNode = ExecLockRows;

	/*
	 * 其他初始化             
	 *
	 * LockRows 节点从不调用 ExecQual 或 ExecProject，因此  
	 * 无需 ExprContext。 
	 */

	/*
	 * 初始化结果类型  
	 */
	ExecInitResultTypeTL(&lrstate->ps);

	/*
	 * 再初始化外层计划  
	 */
	outerPlanState(lrstate) = ExecInitNode(outerPlan, estate, eflags);

	/* 节点返回来自外层计划的未修改槽     */
	lrstate->ps.resultopsset = true;
	lrstate->ps.resultops = ExecGetResultSlotOps(outerPlanState(lrstate),
												 &lrstate->ps.resultopsfixed);

	/*
	 * LockRows 节点不做投影，因此要为此节点初始化       
	 * 相应投影信息。  
	 */
	lrstate->ps.ps_ProjInfo = NULL;

	/*
	 * 找出由本节点负责的 ExecRowMark，并为它们          
	 * 构建 ExecAuxRowMarks。(InitPlan 应该已经构建过          
	 * 全局的 ExecRowMarks 列表。)      
	 */
	lrstate->lr_arowMarks = NIL;
	epq_arowmarks = NIL;
	foreach(lc, node->rowMarks)
	{
		PlanRowMark *rc = lfirst_node(PlanRowMark, lc);
		ExecRowMark *erm;
		ExecAuxRowMark *aerm;

		/* 忽略"parent" rowmark，它们在运行时不相关     */
		if (rc->isParent)
			continue;

		/* 找到 ExecRowMark 并建 ExecAuxRowMark  */
		erm = ExecFindRowMark(estate, rc->rti, false);
		aerm = ExecBuildAuxRowMark(erm, outerPlan->targetlist);

		/*
		 * 只有锁定型 rowmark 进入我们自己的列表。非锁定型
		 * mark 则交给 EvalPlanQual 机制处理。这是因为            
		 * 我们不想费事去取非锁定的行，除非真的要         
		 * 做 EPQ 重查。 
		 */
		if (RowMarkRequiresRowShareLock(erm->markType))
			lrstate->lr_arowMarks = lappend(lrstate->lr_arowMarks, aerm);
		else
			epq_arowmarks = lappend(epq_arowmarks, aerm);
	}

	/* 至此我们已有设置 EPQ 状态所需信息 */
	EvalPlanQualInit(&lrstate->lr_epqstate, estate,
					 outerPlan, epq_arowmarks, node->epqParam, NIL);

	return lrstate;
}

/* ----------------------------------------------------------------
 *		ExecEndLockRows
 *
 *		它关闭子计划，并释放分配给本节点         
 *		的资源。 
 * ----------------------------------------------------------------
 */
void
ExecEndLockRows(LockRowsState *node)
{
	/* 我们可能已关闭过 EPQ，但再调用一次也无妨     */
	EvalPlanQualEnd(&node->lr_epqstate);
	ExecEndNode(outerPlanState(node));
}


void
ExecReScanLockRows(LockRowsState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/*
	 * 若子节点的 chgParam 非空，则计划会被第一次        
	 * ExecProcNode 重扫
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}
