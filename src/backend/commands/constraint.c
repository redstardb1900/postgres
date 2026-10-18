/*-------------------------------------------------------------------------
 *
 * constraint.c
 *	  PostgreSQL CONSTRAINT 支持代码。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/constraint.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"


/*
 * unique_key_recheck - 执行延迟唯一性检查的触发器函数。
 *
 * 现在它也会执行延迟的排他约束检查，因此这个名称
 * 有点历史遗留的意味。
 *
 * 它作为 INSERT 和 UPDATE 的 AFTER ROW 触发器被调用，
 * 针对任何被记录为可能违反可延迟的唯一约束
 * 或排他约束的行。
 *
 * 这可以是语句结束时的检查、提交时的检查，或者
 * 由 SET CONSTRAINTS 命令触发的检查。
 */
Datum
unique_key_recheck(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	const char *funcname = "unique_key_recheck";
	ItemPointerData checktid;
	ItemPointerData tmptid;
	Relation	indexRel;
	IndexInfo  *indexInfo;
	EState	   *estate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	/*
	 * 确保它被作为 AFTER ROW 触发器调用。注意：
	 * 可翻译的错误字符串与 ri_triggers.c 共享，因此请克制
	 * 将函数名合并到这些字符串中的冲动。
	 */
	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" was not called by trigger manager",
						funcname)));

	if (!TRIGGER_FIRED_AFTER(trigdata->tg_event) ||
		!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" must be fired AFTER ROW",
						funcname)));

	/*
	 * 获取被插入/更新的新数据。
	 */
	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		checktid = trigdata->tg_trigslot->tts_tid;
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		checktid = trigdata->tg_newslot->tts_tid;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" must be fired for INSERT or UPDATE",
						funcname)));
		ItemPointerSetInvalid(&checktid);	/* 让编译器安静 */
	}

	slot = table_slot_create(trigdata->tg_relation, NULL);

	/*
	 * 如果 checktid 所指向的行现在已死（即在我们
	 * 的事务中被插入后又删除），我们可以跳过检查。
	 * 但是，我们必须小心，因为这个触发器只有在响应
	 * 索引插入时才会被排入队列；这意味着它不会为
	 * HOT 更新等而被排入队列。我们被调用来处理的行
	 * 现在可能已死，但拥有一个存活的 HOT 子行，
	 * 在这种情况下我们仍然需要进行检查 --- 实际上，
	 * 我们是对存活的子行执行检查，尽管我们可以使用
	 * 这一行中的值，因为按照定义我们感兴趣的所有列
	 * 都是相同的。
	 *
	 * 这看起来可能只是一种优化，因为索引 AM 在抛出
	 * 错误之前会进行同样的测试。但实际上它对于正确性
	 * 是必需的，因为如果找不到该行的索引条目，索引 AM
	 * 也会抛出错误。如果该行已死，那么索引条目也可能
	 * 已被标记为死，甚至已被移除。
	 */
	tmptid = checktid;
	{
		IndexFetchTableData *scan = table_index_fetch_begin(trigdata->tg_relation);
		bool		call_again = false;

		if (!table_index_fetch_tuple(scan, &tmptid, SnapshotSelf, slot,
									 &call_again, NULL))
		{
			/*
			 * 索引条目引用的所有行都已死，因此跳过
			 * 检查。
			 */
			ExecDropSingleTupleTableSlot(slot);
			table_index_fetch_end(scan);
			return PointerGetDatum(NULL);
		}
		table_index_fetch_end(scan);
	}

	/*
	 * 打开索引并获取 RowExclusiveLock，就好像我们要更新
	 * 它一样。（这可以防止索引模式发生可能的更改，
	 * 而不是防止并发更新。）
	 */
	indexRel = index_open(trigdata->tg_trigger->tgconstrindid,
						  RowExclusiveLock);
	indexInfo = BuildIndexInfo(indexRel);

	/*
	 * 通常索引不会有表达式，但如果有的话，我们需要一个
	 * EState 来计算它们。对于排他约束我们也需要它，
	 * 即使它们只是基于简单列。
	 */
	if (indexInfo->ii_Expressions != NIL ||
		indexInfo->ii_ExclusionOps != NULL)
	{
		estate = CreateExecutorState();
		econtext = GetPerTupleExprContext(estate);
		econtext->ecxt_scantuple = slot;
	}
	else
		estate = NULL;

	/*
	 * 为我们需要检查的索引条目构造索引值和 isnull
	 * 标志。
	 *
	 * 注意：如果索引使用的函数并不像其本应那样不可变，
	 * 这可能会产生与原始元组不同的索引元组。索引 AM
	 * 可以通过验证它是否找到带有该元组 TID 的匹配索引条目
	 * 来捕获此类错误。对于排他约束，我们在
	 * check_exclusion_constraint() 中进行这种检查。
	 */
	FormIndexDatum(indexInfo, slot, estate, values, isnull);

	/*
	 * 现在执行相应的检查。
	 */
	if (indexInfo->ii_ExclusionOps == NULL)
	{
		/*
		 * 注意：这不是真正的插入；它是对已经插入的索引
		 * 条目是否唯一的检查。传递元组的 tid（即未经
		 * table_index_fetch_tuple() 修改）即使该行现在
		 * 已死也是正确的，因为那是索引将会知道的
		 * TID。
		 */
		index_insert(indexRel, values, isnull, &checktid,
					 trigdata->tg_relation, UNIQUE_CHECK_EXISTING,
					 false, indexInfo);
	}
	else
	{
		/*
		 * 对于排他约束，我们只做常规检查，但现在可以
		 * 抛出错误。在 HOT 更新的情况下，我们必须在这里使用
		 * 存活的 HOT 子行的 TID，否则 check_exclusion_constraint
		 * 会认为该子行是冲突的。
		 */
		check_exclusion_constraint(trigdata->tg_relation, indexRel, indexInfo,
								   &tmptid, values, isnull,
								   estate, false);
	}

	/*
	 * 如果上述操作成功，那么这个索引条目就是唯一的或未被排除的，
	 * 我们就完成了。
	 */
	if (estate != NULL)
		FreeExecutorState(estate);

	ExecDropSingleTupleTableSlot(slot);

	index_close(indexRel, RowExclusiveLock);

	return PointerGetDatum(NULL);
}
