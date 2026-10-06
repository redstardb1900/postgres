/*-------------------------------------------------------------------------
 *
 * indexing.c
 *	  本文件包含支持定义在系统
 *	  目录上的索引的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/indexing.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "executor/executor.h"
#include "utils/rel.h"


/*
 * CatalogOpenIndexes - 打开系统目录上的索引。
 *
 * 在系统目录中插入或更新元组时，调用本函数
 * 以准备更新该目录的索引。
 *
 * 在当前实现中，打开/关闭索引的代码与
 * execUtils.c 共用。但我们不使用 ExecInsertIndexTuples，
 * 因为不想创建 EState。这意味着我们
 * 不支持系统目录上的部分索引或表达式索引，
 * 也不支持广义排除约束。
 * 若愿意承担构建 EState 的额外开销，
 * 可用局部改动在此修复。
 */
CatalogIndexState
CatalogOpenIndexes(Relation heapRel)
{
	ResultRelInfo *resultRelInfo;

	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 0;	/* 哑元 */
	resultRelInfo->ri_RelationDesc = heapRel;
	resultRelInfo->ri_TrigDesc = NULL;	/* 我们不触发触发器 */

	ExecOpenIndices(resultRelInfo, false);

	return resultRelInfo;
}

/*
 * CatalogCloseIndexes - 清理 CatalogOpenIndexes 分配的资源
 */
void
CatalogCloseIndexes(CatalogIndexState indstate)
{
	ExecCloseIndices(indstate);
	pfree(indstate);
}

/*
 * CatalogIndexInsert - 为一个目录元组插入索引项
 *
 * 每插入或更新一个目录元组都应调用本函数。
 *
 * 这实际上是 ExecInsertIndexTuples 的精简版。
 */
static void
CatalogIndexInsert(CatalogIndexState indstate, HeapTuple heapTuple,
				   TU_UpdateIndexes updateIndexes)
{
	int			i;
	int			numIndexes;
	RelationPtr relationDescs;
	Relation	heapRelation;
	TupleTableSlot *slot;
	IndexInfo **indexInfoArray;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	bool		onlySummarized = (updateIndexes == TU_Summarizing);

	/*
	 * HOT 更新不需要插入索引。但在启用断言时我们
	 * 希望检查当前向表/索引插入
	 * 在法律上是否合法。
	 */
#ifndef USE_ASSERT_CHECKING
	if (HeapTupleIsHeapOnly(heapTuple) && !onlySummarized)
		return;
#endif

	/* 仅更新汇总索引时，元组必须是 HOT。 */
	Assert((!onlySummarized) || HeapTupleIsHeapOnly(heapTuple));

	/*
	 * 从状态结构获取信息。若无事可做则退出。
	 */
	numIndexes = indstate->ri_NumIndices;
	if (numIndexes == 0)
		return;
	relationDescs = indstate->ri_IndexRelationDescs;
	indexInfoArray = indstate->ri_IndexRelationInfo;
	heapRelation = indstate->ri_RelationDesc;

	/* 需要一个槽来存放正在检查的元组 */
	slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRelation),
									&TTSOpsHeapTuple);
	ExecStoreHeapTuple(heapTuple, slot, false);

	/*
	 * 对每个索引，构造并插入索引元组
	 */
	for (i = 0; i < numIndexes; i++)
	{
		IndexInfo  *indexInfo;
		Relation	index;

		indexInfo = indexInfoArray[i];
		index = relationDescs[i];

		/* 若索引被标为只读，则忽略它 */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/*
		 * 不支持系统目录上的表达式索引和部分
		 * 索引，也不支持排除约束，也不支持延迟唯一性
		 */
		Assert(indexInfo->ii_Expressions == NIL);
		Assert(indexInfo->ii_Predicate == NIL);
		Assert(indexInfo->ii_ExclusionOps == NULL);
		Assert(index->rd_index->indimmediate);
		Assert(indexInfo->ii_NumIndexKeyAttrs != 0);

		/* 参见上文较早的检查 */
#ifdef USE_ASSERT_CHECKING
		if (HeapTupleIsHeapOnly(heapTuple) && !onlySummarized)
		{
			Assert(!ReindexIsProcessingIndex(RelationGetRelid(index)));
			continue;
		}
#endif							/* USE_ASSERT_CHECKING */

		/*
		 * 若我们只需更新汇总索引，则跳过
		 * 向非汇总索引的插入。
		 */
		if (onlySummarized && !indexInfo->ii_Summarizing)
			continue;

		/*
		 * FormIndexDatum 用索引 column(s) 的适当值填入其
		 * values 和 isnull 参数。
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   NULL,	/* 无需做表达式求值 */
					   values,
					   isnull);

		/*
		 * 其余工作由索引 AM 完成。
		 */
		index_insert(index,		/* 索引关系 */
					 values,	/* 索引 Datum 数组 */
					 isnull,	/* 是否为 null 的标志 */
					 &(heapTuple->t_self),	/* 堆元组的 tid */
					 heapRelation,
					 index->rd_index->indisunique ?
					 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
					 false,
					 indexInfo);
	}

	ExecDropSingleTupleTableSlot(slot);
}

/*
 * 用于验证目录约束是否得到遵守的子例程。
 *
 * 经 CatalogTupleInsert/CatalogTupleUpdate 插入的元组通常是
 * "手工构造"的，因此可能不满足若由执行器插入时
 * 会被检查的约束。那是
 * 编码错误，因此我们只在启用断言的构建中检查。
 */
#ifdef USE_ASSERT_CHECKING

static void
CatalogTupleCheckConstraints(Relation heapRel, HeapTuple tup)
{
	/*
	 * 目前，为系统目录实现的唯一约束是
	 * attnotnull 约束。
	 */
	if (HeapTupleHasNulls(tup))
	{
		TupleDesc	tupdesc = RelationGetDescr(heapRel);
		bits8	   *bp = tup->t_data->t_bits;

		for (int attnum = 0; attnum < tupdesc->natts; attnum++)
		{
			Form_pg_attribute thisatt = TupleDescAttr(tupdesc, attnum);

			Assert(!(thisatt->attnotnull && att_isnull(attnum, bp)));
		}
	}
}

#else							/* !USE_ASSERT_CHECKING */

#define CatalogTupleCheckConstraints(heapRel, tup)  ((void) 0)

#endif							/* USE_ASSERT_CHECKING */

/*
 * CatalogTupleInsert - 为新目录元组完成堆和索引工作
 *
 * 将 "tup" 中的元组数据插入指定目录关系。
 *
 * 这是在系统目录中插入单个元组这一常见情形的
 * 便捷例程；它插入新的堆元组并保持索引
 * 最新。避免用于多个元组，因为打开索引
 * 并构建索引信息结构的开销不小。
 * （那种情况请使用 CatalogTupleInsertWithInfo。）
 */
void
CatalogTupleInsert(Relation heapRel, HeapTuple tup)
{
	CatalogIndexState indstate;

	CatalogTupleCheckConstraints(heapRel, tup);

	indstate = CatalogOpenIndexes(heapRel);

	simple_heap_insert(heapRel, tup);

	CatalogIndexInsert(indstate, tup, TU_All);
	CatalogCloseIndexes(indstate);
}

/*
 * CatalogTupleInsertWithInfo - 同上，但由调用方提供索引信息
 *
 * 当需要把 CatalogOpenIndexes/
 * CatalogCloseIndexes 的工作摊到多次插入上时，应使用本函数。将来某个时候我们
 * 或许会在某处缓存 CatalogIndexState 数据（也许在 relcache 中），
 * 这样调用方就不必操心……但今天还没有这样做。
 */
void
CatalogTupleInsertWithInfo(Relation heapRel, HeapTuple tup,
						   CatalogIndexState indstate)
{
	CatalogTupleCheckConstraints(heapRel, tup);

	simple_heap_insert(heapRel, tup);

	CatalogIndexInsert(indstate, tup, TU_All);
}

/*
 * CatalogTuplesMultiInsertWithInfo - 同上，但用于多个元组
 *
 * 一次向给定目录关系插入多个元组，
 * CatalogOpenIndexes 的成本被摊销。
 */
void
CatalogTuplesMultiInsertWithInfo(Relation heapRel, TupleTableSlot **slot,
								 int ntuples, CatalogIndexState indstate)
{
	/* 无事可做 */
	if (ntuples <= 0)
		return;

	heap_multi_insert(heapRel, slot, ntuples,
					  GetCurrentCommandId(true), 0, NULL);

	/*
	 * 目录索引没有与 heap_multi_insert 对等的接口，因此
	 * 我们必须循环并逐个插入。
	 */
	for (int i = 0; i < ntuples; i++)
	{
		bool		should_free;
		HeapTuple	tuple;

		tuple = ExecFetchSlotHeapTuple(slot[i], true, &should_free);
		tuple->t_tableOid = slot[i]->tts_tableOid;
		CatalogIndexInsert(indstate, tuple, TU_All);

		if (should_free)
			heap_freetuple(tuple);
	}
}

/*
 * CatalogTupleUpdate - 为更新目录元组完成堆和索引工作
 *
 * 更新由 "otid" 标识的元组，用 "tup" 中的数据替换它。
 *
 * 这是在系统目录中更新单个元组这一常见情形的
 * 便捷例程；它更新一个堆元组并保持索引
 * 最新。避免用于多个元组，因为打开索引
 * 并构建索引信息结构的开销不小。
 * （那种情况请使用 CatalogTupleUpdateWithInfo。）
 */
void
CatalogTupleUpdate(Relation heapRel, ItemPointer otid, HeapTuple tup)
{
	CatalogIndexState indstate;
	TU_UpdateIndexes updateIndexes = TU_All;

	CatalogTupleCheckConstraints(heapRel, tup);

	indstate = CatalogOpenIndexes(heapRel);

	simple_heap_update(heapRel, otid, tup, &updateIndexes);

	CatalogIndexInsert(indstate, tup, updateIndexes);
	CatalogCloseIndexes(indstate);
}

/*
 * CatalogTupleUpdateWithInfo - 同上，但由调用方提供索引信息
 *
 * 当需要把 CatalogOpenIndexes/
 * CatalogCloseIndexes 的工作摊到多次更新上时，应使用本函数。将来某个时候我们
 * 或许会在某处缓存 CatalogIndexState 数据（也许在 relcache 中），
 * 这样调用方就不必操心……但今天还没有这样做。
 */
void
CatalogTupleUpdateWithInfo(Relation heapRel, ItemPointer otid, HeapTuple tup,
						   CatalogIndexState indstate)
{
	TU_UpdateIndexes updateIndexes = TU_All;

	CatalogTupleCheckConstraints(heapRel, tup);

	simple_heap_update(heapRel, otid, tup, &updateIndexes);

	CatalogIndexInsert(indstate, tup, updateIndexes);
}

/*
 * CatalogTupleDelete - 为删除目录元组完成堆和索引工作
 *
 * 删除指定目录中由 "tid" 标识的元组。
 *
 * 对于 Postgres 堆，删除时没有索引工作要做；
 * 清理将稍后由 VACUUM 完成。不过，本函数的调用方
 * 不必知道这一点；我们希望对所有
 * 目录元组变更提供统一抽象。因此提供这个目前很简单的包装。
 *
 * 该抽象略有泄漏：我们没有提供优化的
 * CatalogTupleDeleteWithInfo 版本，因为目前没有什么可
 * 优化。若将来需要，与其改动大量调用点，
 * 不如考虑缓存 CatalogIndexState。
 */
void
CatalogTupleDelete(Relation heapRel, ItemPointer tid)
{
	simple_heap_delete(heapRel, tid);
}
