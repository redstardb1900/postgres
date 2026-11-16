/*-------------------------------------------------------------------------
 *
 * trigfuncs.c
 *	  触发器实用支持的内置函数。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/utils/adt/trigfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "commands/trigger.h"
#include "utils/builtins.h"
#include "utils/rel.h"


/*
 * suppress_redundant_updates_trigger
 *
 * 本触发器函数将阻止更新被执行，
 * 如果 OLD 与 NEW 记录完全相同。
 */
Datum
suppress_redundant_updates_trigger(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	HeapTuple	newtuple,
				oldtuple,
				rettuple;
	HeapTupleHeader newheader,
				oldheader;

	/* 确认其以触发器方式调用 */
	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("suppress_redundant_updates_trigger: must be called as trigger")));

	/* 且针对 UPDATE 事件调用 */
	if (!TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("suppress_redundant_updates_trigger: must be called on update")));

	/* 且在 UPDATE 之前调用 */
	if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("suppress_redundant_updates_trigger: must be called before update")));

	/* 且为每行各调用一次 */
	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("suppress_redundant_updates_trigger: must be called for each row")));

	/* 取元组数据并设默认结果 */
	rettuple = newtuple = trigdata->tg_newtuple;
	oldtuple = trigdata->tg_trigtuple;

	newheader = newtuple->t_data;
	oldheader = oldtuple->t_data;

	/* 如果元组载荷相同... */
	if (newtuple->t_len == oldtuple->t_len &&
		newheader->t_hoff == oldheader->t_hoff &&
		(HeapTupleHeaderGetNatts(newheader) ==
		 HeapTupleHeaderGetNatts(oldheader)) &&
		((newheader->t_infomask & ~HEAP_XACT_MASK) ==
		 (oldheader->t_infomask & ~HEAP_XACT_MASK)) &&
		memcmp(((char *) newheader) + SizeofHeapTupleHeader,
			   ((char *) oldheader) + SizeofHeapTupleHeader,
			   newtuple->t_len - SizeofHeapTupleHeader) == 0)
	{
		/* ... 则抑制该更新 */
		rettuple = NULL;
	}

	return PointerGetDatum(rettuple);
}
