/*----------------------------------------------------------------------
 *
 * tableamapi.c
 *		Postgres 表访问方法 API 的支持例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/table/tableamapi.c
 *----------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_proc.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/syscache.h"


/*
 * GetTableAmRoutine
 *		调用指定的访问方法处理例程，以取得它的
 *		TableAmRoutine 结构，该结构将在调用方的
 *		内存上下文中 palloc 出来。
 */
const TableAmRoutine *
GetTableAmRoutine(Oid amhandler)
{
	Datum		datum;
	const TableAmRoutine *routine;

	datum = OidFunctionCall0(amhandler);
	routine = (TableAmRoutine *) DatumGetPointer(datum);

	if (routine == NULL || !IsA(routine, TableAmRoutine))
		elog(ERROR, "table access method handler %u did not return a TableAmRoutine struct",
			 amhandler);

	/*
	 * 断言所有必需的回调都存在。这样在让表 AM
	 * 保持最新时会容易一些，例如把它们向前移植到新的
	 * 主版本时。
	 */
	Assert(routine->scan_begin != NULL);
	Assert(routine->scan_end != NULL);
	Assert(routine->scan_rescan != NULL);
	Assert(routine->scan_getnextslot != NULL);

	Assert(routine->parallelscan_estimate != NULL);
	Assert(routine->parallelscan_initialize != NULL);
	Assert(routine->parallelscan_reinitialize != NULL);

	Assert(routine->index_fetch_begin != NULL);
	Assert(routine->index_fetch_reset != NULL);
	Assert(routine->index_fetch_end != NULL);
	Assert(routine->index_fetch_tuple != NULL);

	Assert(routine->tuple_fetch_row_version != NULL);
	Assert(routine->tuple_tid_valid != NULL);
	Assert(routine->tuple_get_latest_tid != NULL);
	Assert(routine->tuple_satisfies_snapshot != NULL);
	Assert(routine->index_delete_tuples != NULL);

	Assert(routine->tuple_insert != NULL);

	/*
	 * 本可做成可选，但那样就需要在
	 * 解析分析期间抛出错误。
	 */
	Assert(routine->tuple_insert_speculative != NULL);
	Assert(routine->tuple_complete_speculative != NULL);

	Assert(routine->multi_insert != NULL);
	Assert(routine->tuple_delete != NULL);
	Assert(routine->tuple_update != NULL);
	Assert(routine->tuple_lock != NULL);

	Assert(routine->relation_set_new_filelocator != NULL);
	Assert(routine->relation_nontransactional_truncate != NULL);
	Assert(routine->relation_copy_data != NULL);
	Assert(routine->relation_copy_for_cluster != NULL);
	Assert(routine->relation_vacuum != NULL);
	Assert(routine->scan_analyze_next_block != NULL);
	Assert(routine->scan_analyze_next_tuple != NULL);
	Assert(routine->index_build_range_scan != NULL);
	Assert(routine->index_validate_scan != NULL);

	Assert(routine->relation_size != NULL);
	Assert(routine->relation_needs_toast_table != NULL);

	Assert(routine->relation_estimate_size != NULL);

	/* 可选，但有一个回调就意味着另一个也存在 */
	Assert((routine->scan_bitmap_next_block == NULL) ==
		   (routine->scan_bitmap_next_tuple == NULL));
	Assert(routine->scan_sample_next_block != NULL);
	Assert(routine->scan_sample_next_tuple != NULL);

	return routine;
}

/* check_hook：校验新的 default_table_access_method */
bool
check_default_table_access_method(char **newval, void **extra, GucSource source)
{
	if (**newval == '\0')
	{
		GUC_check_errdetail("%s cannot be empty.",
							"default_table_access_method");
		return false;
	}

	if (strlen(*newval) >= NAMEDATALEN)
	{
		GUC_check_errdetail("%s is too long (maximum %d characters).",
							"default_table_access_method", NAMEDATALEN - 1);
		return false;
	}

	/*
	 * 若不在事务内，或者尚未连接到数据库，就
	 * 无法做校验该方法所需的系统目录访问。只能
	 * 先相信这个值。
	 */
	if (IsTransactionState() && MyDatabaseId != InvalidOid)
	{
		if (!OidIsValid(get_table_am_oid(*newval, true)))
		{
			/*
			 * 当 source == PGC_S_TEST 时，对于不存在的
			 * 表访问方法不要抛硬错误，只发 NOTICE。参见
			 * guc.h 中的注释。
			 */
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("table access method \"%s\" does not exist",
								*newval)));
			}
			else
			{
				GUC_check_errdetail("Table access method \"%s\" does not exist.",
									*newval);
				return false;
			}
		}
	}

	return true;
}
