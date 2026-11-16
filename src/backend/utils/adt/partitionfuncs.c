/*-------------------------------------------------------------------------
 *
 * partitionfuncs.c
 *	  用于访问分区相关元数据的函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/partitionfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/partition.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * 检查给定关系能否作为分区树的成员。如果
 * 该关系无法处理则返回 false，此时如何处置
 * 由调用方自行决定：既可以抛出错误，也可以
 * 做其他事。
 */
static bool
check_rel_can_be_partition(Oid relid)
{
	char		relkind;
	bool		relispartition;

	/* 检查关系存在性 */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
		return false;

	relkind = get_rel_relkind(relid);
	relispartition = get_rel_relispartition(relid);

	/* 只允许可能出现在分区树中的关系类型。 */
	if (!relispartition && !RELKIND_HAS_PARTITIONS(relkind))
		return false;

	return true;
}

/*
 * pg_partition_tree
 *
 * 产出一个视图，分区树的每个成员各占一行，从
 * 调用方给出的最顶层父开始。这里给出每个
 * 分区的信息：其直接分区父节点、是否为叶子
 * 分区及其在层级中的级别。
 */
Datum
pg_partition_tree(PG_FUNCTION_ARGS)
{
#define PG_PARTITION_TREE_COLS	4
	Oid			rootrelid = PG_GETARG_OID(0);
	FuncCallContext *funcctx;
	List	   *partitions;

	/* 仅在函数首次调用时执行的操作 */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcxt;
		TupleDesc	tupdesc;

		/* 创建跨调用持久的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		if (!check_rel_can_be_partition(rootrelid))
			SRF_RETURN_DONE(funcctx);

		/* 切换到适合多次函数调用的内存上下文 */
		oldcxt = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * 找出继承集合的所有成员。对子节点只需
		 * AccessShareLock 来查询分区信息。
		 */
		partitions = find_all_inheritors(rootrelid, AccessShareLock, NULL);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = tupdesc;

		/* 唯一需要的状态是分区列表 */
		funcctx->user_fctx = (void *) partitions;

		MemoryContextSwitchTo(oldcxt);
	}

	/* 每次函数调用都执行的操作 */
	funcctx = SRF_PERCALL_SETUP();
	partitions = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < list_length(partitions))
	{
		Datum		result;
		Datum		values[PG_PARTITION_TREE_COLS] = {0};
		bool		nulls[PG_PARTITION_TREE_COLS] = {0};
		HeapTuple	tuple;
		Oid			parentid = InvalidOid;
		Oid			relid = list_nth_oid(partitions, funcctx->call_cntr);
		char		relkind = get_rel_relkind(relid);
		int			level = 0;
		List	   *ancestors = get_partition_ancestors(relid);
		ListCell   *lc;

		/*
		 * 用适当数据构造元组。
		 */

		/* relid */
		values[0] = ObjectIdGetDatum(relid);

		/* parentid */
		if (ancestors != NIL)
			parentid = linitial_oid(ancestors);
		if (OidIsValid(parentid))
			values[1] = ObjectIdGetDatum(parentid);
		else
			nulls[1] = true;

		/* isleaf */
		values[2] = BoolGetDatum(!RELKIND_HAS_PARTITIONS(relkind));

		/* level */
		if (relid != rootrelid)
		{
			foreach(lc, ancestors)
			{
				level++;
				if (lfirst_oid(lc) == rootrelid)
					break;
			}
		}
		values[3] = Int32GetDatum(level);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	/* 没有剩余元素时即完成 */
	SRF_RETURN_DONE(funcctx);
}

/*
 * pg_partition_root
 *
 * 返回给定关系所属分区树的
 * 最顶层父节点；若不属于任何分区树则
 * 返回 NULL。
 */
Datum
pg_partition_root(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Oid			rootrelid;
	List	   *ancestors;

	if (!check_rel_can_be_partition(relid))
		PG_RETURN_NULL();

	/* 获取祖先列表 */
	ancestors = get_partition_ancestors(relid);

	/*
	 * 如果输入关系已经是最顶层父，直接返回自
	 * 身。
	 */
	if (ancestors == NIL)
		PG_RETURN_OID(relid);

	rootrelid = llast_oid(ancestors);
	list_free(ancestors);

	/*
	 * 鉴于输入关系是上面已检查过的合法分区树
	 * 成员，"rootrelid" 必须包含有效 OID。
	 */
	Assert(OidIsValid(rootrelid));
	PG_RETURN_OID(rootrelid);
}

/*
 * pg_partition_ancestors
 *
 * 产出一个视图，给定分区的每个祖先各占一行，
 * 包括输入关系本身。
 */
Datum
pg_partition_ancestors(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	FuncCallContext *funcctx;
	List	   *ancestors;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcxt;

		funcctx = SRF_FIRSTCALL_INIT();

		if (!check_rel_can_be_partition(relid))
			SRF_RETURN_DONE(funcctx);

		oldcxt = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		ancestors = get_partition_ancestors(relid);
		ancestors = lcons_oid(relid, ancestors);

		/* 唯一需要的状态就是祖先列表 */
		funcctx->user_fctx = (void *) ancestors;

		MemoryContextSwitchTo(oldcxt);
	}

	funcctx = SRF_PERCALL_SETUP();
	ancestors = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < list_length(ancestors))
	{
		Oid			resultrel = list_nth_oid(ancestors, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, ObjectIdGetDatum(resultrel));
	}

	SRF_RETURN_DONE(funcctx);
}
