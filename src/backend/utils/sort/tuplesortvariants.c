/*-------------------------------------------------------------------------
 *
 * tuplesortvariants.c
 *	  元组排序变体的实现。
 *
 * 本模块处理堆元组、索引元组或单个 Datum 的排序。
 * 其实现基于 tuplesort.c 中给出的通用化元组
 * 排序设施。对其他种类可排序对象的支持可以
 * 轻松地添加到此处、别的模块甚至扩展中。
 *
 *
 * Copyright (c) 2022-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/tuplesortvariants.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "pg_trace.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/guc.h"
#include "utils/tuplesort.h"


/* sort__start 探针的排序类型码 */
#define HEAP_SORT		0
#define INDEX_SORT		1
#define DATUM_SORT		2
#define CLUSTER_SORT	3

static void removeabbrev_heap(Tuplesortstate *state, SortTuple *stups,
							  int count);
static void removeabbrev_cluster(Tuplesortstate *state, SortTuple *stups,
								 int count);
static void removeabbrev_index(Tuplesortstate *state, SortTuple *stups,
							   int count);
static void removeabbrev_datum(Tuplesortstate *state, SortTuple *stups,
							   int count);
static int	comparetup_heap(const SortTuple *a, const SortTuple *b,
							Tuplesortstate *state);
static void writetup_heap(Tuplesortstate *state, LogicalTape *tape,
						  SortTuple *stup);
static void readtup_heap(Tuplesortstate *state, SortTuple *stup,
						 LogicalTape *tape, unsigned int len);
static int	comparetup_cluster(const SortTuple *a, const SortTuple *b,
							   Tuplesortstate *state);
static void writetup_cluster(Tuplesortstate *state, LogicalTape *tape,
							 SortTuple *stup);
static void readtup_cluster(Tuplesortstate *state, SortTuple *stup,
							LogicalTape *tape, unsigned int tuplen);
static int	comparetup_index_btree(const SortTuple *a, const SortTuple *b,
								   Tuplesortstate *state);
static int	comparetup_index_hash(const SortTuple *a, const SortTuple *b,
								  Tuplesortstate *state);
static void writetup_index(Tuplesortstate *state, LogicalTape *tape,
						   SortTuple *stup);
static void readtup_index(Tuplesortstate *state, SortTuple *stup,
						  LogicalTape *tape, unsigned int len);
static int	comparetup_datum(const SortTuple *a, const SortTuple *b,
							 Tuplesortstate *state);
static void writetup_datum(Tuplesortstate *state, LogicalTape *tape,
						   SortTuple *stup);
static void readtup_datum(Tuplesortstate *state, SortTuple *stup,
						  LogicalTape *tape, unsigned int len);
static void freestate_cluster(Tuplesortstate *state);

/*
 * "TuplesortPublic.arg" 所指向的 CLUSTER 情形数据结构，设置者为
 * tuplesort_begin_cluster。
 */
typedef struct
{
	TupleDesc	tupDesc;

	IndexInfo  *indexInfo;		/* 关于用作参照的索引的信息 */
	EState	   *estate;			/* 用于计算索引表达式 */
} TuplesortClusterArg;

/*
 * "TuplesortPublic.arg" 所指向的 IndexTuple 情形数据结构。
 * 由 tuplesort_begin_index_xxx 设置，仅由 IndexTuple 例程使用。
 */
typedef struct
{
	Relation	heapRel;		/* 正在其上构建索引的表 */
	Relation	indexRel;		/* 在建索引 */
} TuplesortIndexArg;

/*
 * "TuplesortPublic.arg" 所指向的 index_btree 子情形数据结构。
 */
typedef struct
{
	TuplesortIndexArg index;

	bool		enforceUnique;	/* 若发现重复元组则报错 */
	bool		uniqueNullsNotDistinct; /* 唯一约束 NULL 处理方式 */
} TuplesortIndexBTreeArg;

/*
 * "TuplesortPublic.arg" 所指向的 index_hash 子情形数据结构。
 */
typedef struct
{
	TuplesortIndexArg index;

	uint32		high_mask;		/* 哈希码可排序部分的掩码 */
	uint32		low_mask;
	uint32		max_buckets;
} TuplesortIndexHashArg;

/*
 * "TuplesortPublic.arg" 所指向的 Datum 情形数据结构。
 * 由 tuplesort_begin_datum 设置，仅由 DatumTuple 例程使用。
 */
typedef struct
{
	/* 待排序 Datum 的数据类型 OID */
	Oid			datumType;
	/* 需要 typelen 来了解如何复制 Datum。 */
	int			datumTypeLen;
} TuplesortDatumArg;

Tuplesortstate *
tuplesort_begin_heap(TupleDesc tupDesc,
					 int nkeys, AttrNumber *attNums,
					 Oid *sortOperators, Oid *sortCollations,
					 bool *nullsFirstFlags,
					 int workMem, SortCoordinate coordinate, int sortopt)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   sortopt);
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MemoryContext oldcontext;
	int			i;

	oldcontext = MemoryContextSwitchTo(base->maincontext);

	Assert(nkeys > 0);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin tuple sort: nkeys = %d, workMem = %d, randomAccess = %c",
			 nkeys, workMem, sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');
#endif

	base->nKeys = nkeys;

	TRACE_POSTGRESQL_SORT_START(HEAP_SORT,
								false,	/* 无唯一检查 */
								nkeys,
								workMem,
								sortopt & TUPLESORT_RANDOMACCESS,
								PARALLEL_SORT(coordinate));

	base->removeabbrev = removeabbrev_heap;
	base->comparetup = comparetup_heap;
	base->writetup = writetup_heap;
	base->readtup = readtup_heap;
	base->haveDatum1 = true;
	base->arg = tupDesc;		/* 假定无需复制 tupDesc */

	/* 为每个列准备 SortSupport 数据 */
	base->sortKeys = (SortSupport) palloc0(nkeys * sizeof(SortSupportData));

	for (i = 0; i < nkeys; i++)
	{
		SortSupport sortKey = base->sortKeys + i;

		Assert(attNums[i] != 0);
		Assert(sortOperators[i] != 0);

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = sortCollations[i];
		sortKey->ssup_nulls_first = nullsFirstFlags[i];
		sortKey->ssup_attno = attNums[i];
		/* 传达缩略优化原则上是否适用 */
		sortKey->abbreviate = (i == 0 && base->haveDatum1);

		PrepareSortSupportFromOrderingOp(sortOperators[i], sortKey);
	}

	/*
	 * "onlyKey" 优化无法与缩略键一同使用，因为
	 * 可能需要决胜比较。通常，这种优化本来就只对
	 * 按值传递类型有价值，而缩略键通常只对按引用
	 * 传递类型有价值。
	 */
	if (nkeys == 1 && !base->sortKeys->abbrev_converter)
		base->onlyKey = base->sortKeys;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_cluster(TupleDesc tupDesc,
						Relation indexRel,
						int workMem,
						SortCoordinate coordinate, int sortopt)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   sortopt);
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	BTScanInsert indexScanKey;
	MemoryContext oldcontext;
	TuplesortClusterArg *arg;
	int			i;

	Assert(indexRel->rd_rel->relam == BTREE_AM_OID);

	oldcontext = MemoryContextSwitchTo(base->maincontext);
	arg = (TuplesortClusterArg *) palloc0(sizeof(TuplesortClusterArg));

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin tuple sort: nkeys = %d, workMem = %d, randomAccess = %c",
			 RelationGetNumberOfAttributes(indexRel),
			 workMem, sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');
#endif

	base->nKeys = IndexRelationGetNumberOfKeyAttributes(indexRel);

	TRACE_POSTGRESQL_SORT_START(CLUSTER_SORT,
								false,	/* 无唯一检查 */
								base->nKeys,
								workMem,
								sortopt & TUPLESORT_RANDOMACCESS,
								PARALLEL_SORT(coordinate));

	base->removeabbrev = removeabbrev_cluster;
	base->comparetup = comparetup_cluster;
	base->writetup = writetup_cluster;
	base->readtup = readtup_cluster;
	base->freestate = freestate_cluster;
	base->arg = arg;

	arg->indexInfo = BuildIndexInfo(indexRel);

	/*
	 * 如果没有简单的前导属性，我们目前就不会
	 * 初始化 datum1，因此禁用需要它的优化。
	 */
	if (arg->indexInfo->ii_IndexAttrNumbers[0] == 0)
		base->haveDatum1 = false;
	else
		base->haveDatum1 = true;

	arg->tupDesc = tupDesc;		/* 假定无需复制 tupDesc */

	indexScanKey = _bt_mkscankey(indexRel, NULL);

	if (arg->indexInfo->ii_Expressions != NULL)
	{
		TupleTableSlot *slot;
		ExprContext *econtext;

		/*
		 * 我们将需要使用 FormIndexDatum 来计算索引
		 * 表达式。为此需要 EState，以及一个用于
		 * 容纳表元组的 TupleTableSlot。econtext 的 scantuple
		 * 也必须指向该槽位。
		 */
		arg->estate = CreateExecutorState();
		slot = MakeSingleTupleTableSlot(tupDesc, &TTSOpsHeapTuple);
		econtext = GetPerTupleExprContext(arg->estate);
		econtext->ecxt_scantuple = slot;
	}

	/* 为每个列准备 SortSupport 数据 */
	base->sortKeys = (SortSupport) palloc0(base->nKeys *
										   sizeof(SortSupportData));

	for (i = 0; i < base->nKeys; i++)
	{
		SortSupport sortKey = base->sortKeys + i;
		ScanKey		scanKey = indexScanKey->scankeys + i;
		int16		strategy;

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = scanKey->sk_collation;
		sortKey->ssup_nulls_first =
			(scanKey->sk_flags & SK_BT_NULLS_FIRST) != 0;
		sortKey->ssup_attno = scanKey->sk_attno;
		/* 传达缩略优化原则上是否适用 */
		sortKey->abbreviate = (i == 0 && base->haveDatum1);

		Assert(sortKey->ssup_attno != 0);

		strategy = (scanKey->sk_flags & SK_BT_DESC) != 0 ?
			BTGreaterStrategyNumber : BTLessStrategyNumber;

		PrepareSortSupportFromIndexRel(indexRel, strategy, sortKey);
	}

	pfree(indexScanKey);

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_index_btree(Relation heapRel,
							Relation indexRel,
							bool enforceUnique,
							bool uniqueNullsNotDistinct,
							int workMem,
							SortCoordinate coordinate,
							int sortopt)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   sortopt);
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	BTScanInsert indexScanKey;
	TuplesortIndexBTreeArg *arg;
	MemoryContext oldcontext;
	int			i;

	oldcontext = MemoryContextSwitchTo(base->maincontext);
	arg = (TuplesortIndexBTreeArg *) palloc(sizeof(TuplesortIndexBTreeArg));

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin index sort: unique = %c, workMem = %d, randomAccess = %c",
			 enforceUnique ? 't' : 'f',
			 workMem, sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');
#endif

	base->nKeys = IndexRelationGetNumberOfKeyAttributes(indexRel);

	TRACE_POSTGRESQL_SORT_START(INDEX_SORT,
								enforceUnique,
								base->nKeys,
								workMem,
								sortopt & TUPLESORT_RANDOMACCESS,
								PARALLEL_SORT(coordinate));

	base->removeabbrev = removeabbrev_index;
	base->comparetup = comparetup_index_btree;
	base->writetup = writetup_index;
	base->readtup = readtup_index;
	base->haveDatum1 = true;
	base->arg = arg;

	arg->index.heapRel = heapRel;
	arg->index.indexRel = indexRel;
	arg->enforceUnique = enforceUnique;
	arg->uniqueNullsNotDistinct = uniqueNullsNotDistinct;

	indexScanKey = _bt_mkscankey(indexRel, NULL);

	/* 为每个列准备 SortSupport 数据 */
	base->sortKeys = (SortSupport) palloc0(base->nKeys *
										   sizeof(SortSupportData));

	for (i = 0; i < base->nKeys; i++)
	{
		SortSupport sortKey = base->sortKeys + i;
		ScanKey		scanKey = indexScanKey->scankeys + i;
		int16		strategy;

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = scanKey->sk_collation;
		sortKey->ssup_nulls_first =
			(scanKey->sk_flags & SK_BT_NULLS_FIRST) != 0;
		sortKey->ssup_attno = scanKey->sk_attno;
		/* 传达缩略优化原则上是否适用 */
		sortKey->abbreviate = (i == 0 && base->haveDatum1);

		Assert(sortKey->ssup_attno != 0);

		strategy = (scanKey->sk_flags & SK_BT_DESC) != 0 ?
			BTGreaterStrategyNumber : BTLessStrategyNumber;

		PrepareSortSupportFromIndexRel(indexRel, strategy, sortKey);
	}

	pfree(indexScanKey);

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_index_hash(Relation heapRel,
						   Relation indexRel,
						   uint32 high_mask,
						   uint32 low_mask,
						   uint32 max_buckets,
						   int workMem,
						   SortCoordinate coordinate,
						   int sortopt)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   sortopt);
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MemoryContext oldcontext;
	TuplesortIndexHashArg *arg;

	oldcontext = MemoryContextSwitchTo(base->maincontext);
	arg = (TuplesortIndexHashArg *) palloc(sizeof(TuplesortIndexHashArg));

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin index sort: high_mask = 0x%x, low_mask = 0x%x, "
			 "max_buckets = 0x%x, workMem = %d, randomAccess = %c",
			 high_mask,
			 low_mask,
			 max_buckets,
			 workMem,
			 sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');
#endif

	base->nKeys = 1;			/* 仅一个排序列，即哈希码 */

	base->removeabbrev = removeabbrev_index;
	base->comparetup = comparetup_index_hash;
	base->writetup = writetup_index;
	base->readtup = readtup_index;
	base->haveDatum1 = true;
	base->arg = arg;

	arg->index.heapRel = heapRel;
	arg->index.indexRel = indexRel;

	arg->high_mask = high_mask;
	arg->low_mask = low_mask;
	arg->max_buckets = max_buckets;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_index_gist(Relation heapRel,
						   Relation indexRel,
						   int workMem,
						   SortCoordinate coordinate,
						   int sortopt)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   sortopt);
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MemoryContext oldcontext;
	TuplesortIndexBTreeArg *arg;
	int			i;

	oldcontext = MemoryContextSwitchTo(base->maincontext);
	arg = (TuplesortIndexBTreeArg *) palloc(sizeof(TuplesortIndexBTreeArg));

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin index sort: workMem = %d, randomAccess = %c",
			 workMem, sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');
#endif

	base->nKeys = IndexRelationGetNumberOfKeyAttributes(indexRel);

	base->removeabbrev = removeabbrev_index;
	base->comparetup = comparetup_index_btree;
	base->writetup = writetup_index;
	base->readtup = readtup_index;
	base->haveDatum1 = true;
	base->arg = arg;

	arg->index.heapRel = heapRel;
	arg->index.indexRel = indexRel;
	arg->enforceUnique = false;
	arg->uniqueNullsNotDistinct = false;

	/* 为每个列准备 SortSupport 数据 */
	base->sortKeys = (SortSupport) palloc0(base->nKeys *
										   sizeof(SortSupportData));

	for (i = 0; i < base->nKeys; i++)
	{
		SortSupport sortKey = base->sortKeys + i;

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = indexRel->rd_indcollation[i];
		sortKey->ssup_nulls_first = false;
		sortKey->ssup_attno = i + 1;
		/* 传达缩略优化原则上是否适用 */
		sortKey->abbreviate = (i == 0 && base->haveDatum1);

		Assert(sortKey->ssup_attno != 0);

		/* 查找排序支持函数 */
		PrepareSortSupportFromGistIndexRel(indexRel, sortKey);
	}

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_datum(Oid datumType, Oid sortOperator, Oid sortCollation,
					  bool nullsFirstFlag, int workMem,
					  SortCoordinate coordinate, int sortopt)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   sortopt);
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortDatumArg *arg;
	MemoryContext oldcontext;
	int16		typlen;
	bool		typbyval;

	oldcontext = MemoryContextSwitchTo(base->maincontext);
	arg = (TuplesortDatumArg *) palloc(sizeof(TuplesortDatumArg));

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin datum sort: workMem = %d, randomAccess = %c",
			 workMem, sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');
#endif

	base->nKeys = 1;			/* 总是单列排序 */

	TRACE_POSTGRESQL_SORT_START(DATUM_SORT,
								false,	/* 无唯一检查 */
								1,
								workMem,
								sortopt & TUPLESORT_RANDOMACCESS,
								PARALLEL_SORT(coordinate));

	base->removeabbrev = removeabbrev_datum;
	base->comparetup = comparetup_datum;
	base->writetup = writetup_datum;
	base->readtup = readtup_datum;
	base->haveDatum1 = true;
	base->arg = arg;

	arg->datumType = datumType;

	/* 查找该 datum 类型的必要属性 */
	get_typlenbyval(datumType, &typlen, &typbyval);
	arg->datumTypeLen = typlen;
	base->tuples = !typbyval;

	/* 准备 SortSupport 数据*/
	base->sortKeys = (SortSupport) palloc0(sizeof(SortSupportData));

	base->sortKeys->ssup_cxt = CurrentMemoryContext;
	base->sortKeys->ssup_collation = sortCollation;
	base->sortKeys->ssup_nulls_first = nullsFirstFlag;

	/*
	 * 这里只有按引用传递的类型才可能缩略。理论上，
	 * 按值传递的数据类型也可能有比较起来更廉价
	 * 的缩略形式。在元组排序中我们可以支持这一点，
	 * 因为总能在需要时从元组中提取原 datum。但在
	 * 这里做不到，因为 datum 排序只存储一份 datum 副本；
	 * SortTuple 的"tuple"字段为 NULL。
	 */
	base->sortKeys->abbreviate = !typbyval;

	PrepareSortSupportFromOrderingOp(sortOperator, base->sortKeys);

	/*
	 * "onlyKey" 优化无法与缩略键一同使用，因为
	 * 可能需要决胜比较。通常，这种优化本来就只对
	 * 按值传递类型有价值，而缩略键通常只对按引用
	 * 传递类型有价值。
	 */
	if (!base->sortKeys->abbrev_converter)
		base->onlyKey = base->sortKeys;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

/*
 * 在收集排序输入数据时接收一个元组。
 *
 * 注意输入数据总是会被复制；调用者无须保存它。
 */
void
tuplesort_puttupleslot(Tuplesortstate *state, TupleTableSlot *slot)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MemoryContext oldcontext = MemoryContextSwitchTo(base->tuplecontext);
	TupleDesc	tupDesc = (TupleDesc) base->arg;
	SortTuple	stup;
	MinimalTuple tuple;
	HeapTupleData htup;

	/* 把元组复制进排序存储 */
	tuple = ExecCopySlotMinimalTuple(slot);
	stup.tuple = (void *) tuple;
	/* 设置第一列键值 */
	htup.t_len = tuple->t_len + MINIMAL_TUPLE_OFFSET;
	htup.t_data = (HeapTupleHeader) ((char *) tuple - MINIMAL_TUPLE_OFFSET);
	stup.datum1 = heap_getattr(&htup,
							   base->sortKeys[0].ssup_attno,
							   tupDesc,
							   &stup.isnull1);

	tuplesort_puttuple_common(state, &stup,
							  base->sortKeys->abbrev_converter &&
							  !stup.isnull1);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * 在收集排序输入数据时接收一个元组。
 *
 * 注意输入数据总是会被复制；调用者无须保存它。
 */
void
tuplesort_putheaptuple(Tuplesortstate *state, HeapTuple tup)
{
	SortTuple	stup;
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MemoryContext oldcontext = MemoryContextSwitchTo(base->tuplecontext);
	TuplesortClusterArg *arg = (TuplesortClusterArg *) base->arg;

	/* 把元组复制进排序存储 */
	tup = heap_copytuple(tup);
	stup.tuple = (void *) tup;

	/*
	 * 设置第一列键值；若是简单列，还可能一并
	 * 缩略。
	 */
	if (base->haveDatum1)
	{
		stup.datum1 = heap_getattr(tup,
								   arg->indexInfo->ii_IndexAttrNumbers[0],
								   arg->tupDesc,
								   &stup.isnull1);
	}

	tuplesort_puttuple_common(state, &stup,
							  base->haveDatum1 &&
							  base->sortKeys->abbrev_converter &&
							  !stup.isnull1);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * 在收集排序输入数据时接收一个索引元组，用
 * 调用者给的值构建。
 */
void
tuplesort_putindextuplevalues(Tuplesortstate *state, Relation rel,
							  ItemPointer self, Datum *values,
							  bool *isnull)
{
	SortTuple	stup;
	IndexTuple	tuple;
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortIndexArg *arg = (TuplesortIndexArg *) base->arg;

	stup.tuple = index_form_tuple_context(RelationGetDescr(rel), values,
										  isnull, base->tuplecontext);
	tuple = ((IndexTuple) stup.tuple);
	tuple->t_tid = *self;
	/* 设置第一列键值 */
	stup.datum1 = index_getattr(tuple,
								1,
								RelationGetDescr(arg->indexRel),
								&stup.isnull1);

	tuplesort_puttuple_common(state, &stup,
							  base->sortKeys &&
							  base->sortKeys->abbrev_converter &&
							  !stup.isnull1);
}

/*
 * 在收集排序输入时接收一个 Datum。
 *
 * 若 Datum 是按引用传递类型，其值将被复制。
 */
void
tuplesort_putdatum(Tuplesortstate *state, Datum val, bool isNull)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MemoryContext oldcontext = MemoryContextSwitchTo(base->tuplecontext);
	TuplesortDatumArg *arg = (TuplesortDatumArg *) base->arg;
	SortTuple	stup;

	/*
	 * 按值传递类型或空值直接存储在 stup.datum1 中
	 * （stup.tuple 不使用并设为 NULL）。
	 *
	 * 非空的按引用传递值需要复制到我们控制的
	 * 内存中，并可能被缩略。复制出的值由 stup.tuple
	 * 指向，并被当作规范副本（例如经由
	 * tuplesort_getdatum 返回或写往磁带时）；stup.datum1
	 * 在发生缩略时取得缩略值，否则它与
	 * stup.tuple 相同。
	 */

	if (isNull || !base->tuples)
	{
		/*
		 * 把 NULLs 的 datum1 设为全零表示（为保持一致，
		 * 并为 NULL 缩略键支持廉价的不等测试）。
		 */
		stup.datum1 = !isNull ? val : (Datum) 0;
		stup.isnull1 = isNull;
		stup.tuple = NULL;		/* 无单独存储 */
	}
	else
	{
		stup.isnull1 = false;
		stup.datum1 = datumCopy(val, false, arg->datumTypeLen);
		stup.tuple = DatumGetPointer(stup.datum1);
	}

	tuplesort_puttuple_common(state, &stup,
							  base->tuples &&
							  base->sortKeys->abbrev_converter && !isNull);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * 以正向或反向任一方向获取下一个元组。
 * 若成功，把元组放入槽位并返回真；否则清空槽
 * 位返回假。
 *
 * 缩略被使用时，调用者可选地拿回缩略值（在返回
 * 值为 true 时），可用它廉价避免原本需要的
 * 相等检查。调用者可以安全地基于简单二进制不等
 * 性作出"元组不等"的判定。
 * 前导属性中 NULL 值会把缩略值设为全零表示，
 * 调用者可以在缩略不等检查中依赖这一点。
 *
 * 若 copy 为 true，槽位收到的元组已被复制进调用
 * 者的内存上下文，这样无论未来对 tuplesort 状态
 * 作何种操纵（直至包括删除 tuplesort），它都会保持
 * 有效。若 copy 为 false，槽位只会收到指向
 * tuplesort 内持有的元组的指针，这更高效，但只对
 * 准备好让之后对 tuplesort 状态的任何操纵使槽位
 * 内容失效的调用者才是安全的。
 */
bool
tuplesort_gettupleslot(Tuplesortstate *state, bool forward, bool copy,
					   TupleTableSlot *slot, Datum *abbrev)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MemoryContext oldcontext = MemoryContextSwitchTo(base->sortcontext);
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup))
		stup.tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	if (stup.tuple)
	{
		/* 为调用者记录缩略键 */
		if (base->sortKeys->abbrev_converter && abbrev)
			*abbrev = stup.datum1;

		if (copy)
			stup.tuple = heap_copy_minimal_tuple((MinimalTuple) stup.tuple);

		ExecStoreMinimalTuple((MinimalTuple) stup.tuple, slot, copy);
		return true;
	}
	else
	{
		ExecClearTuple(slot);
		return false;
	}
}

/*
 * 以正向或反向任一方向获取下一个元组。
 * 若无更多元组则返回 NULL。返回的元组属于
 * tuplesort 内存上下文，不得由调用者释放。调用者
 * 勿依赖元组在进一步操纵 tuplesort 后仍有效。
 */
HeapTuple
tuplesort_getheaptuple(Tuplesortstate *state, bool forward)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MemoryContext oldcontext = MemoryContextSwitchTo(base->sortcontext);
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup))
		stup.tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	return stup.tuple;
}

/*
 * 以正向或反向获取下一个索引元组。
 * 若无更多元组则返回 NULL。返回的元组属于
 * tuplesort 内存上下文，不得由调用者释放。调用者
 * 勿依赖元组在进一步操纵 tuplesort 后仍有效。
 */
IndexTuple
tuplesort_getindextuple(Tuplesortstate *state, bool forward)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MemoryContext oldcontext = MemoryContextSwitchTo(base->sortcontext);
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup))
		stup.tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	return (IndexTuple) stup.tuple;
}

/*
 * 以正向或反向任一方向获取下一个 Datum。
 * datum 用尽则返回 false。
 *
 * 若 Datum 是按引用传递类型，返回值是新 palloc 出来的、
 * 位于调用者的上下文中且归调用者所有（这一点
 * 与其他类型 tuplesort 的例程不同）。
 *
 * 缩略被使用时，调用者可选地拿回缩略值（在返回
 * 值为 true 时），可用它廉价避免原本需要的
 * 相等检查。调用者可以安全地基于简单二进制不等
 * 性作出"元组不等"的判定。
 * NULL 值将具有全零的缩略值表示，调用者可以在缩略
 * 不等检查中依赖这一点。
 *
 * 对按引用传递的 Datum，若 copy 为 true，*val 会被设为
 * 该 Datum 的一个副本，副本复制进了调用者的内存
 * 上下文中，因此无论未来对 tuplesort 状态作何种
 * 操纵（直至包括删除 tuplesort），它都保持有效。若
 * copy 为 false，*val 只会设为指向 tuplesort 内所持
 * 有 Datum 的指针，更高效，但只对准备好让之后对
 * tuplesort 状态的操纵使槽位内容失效的调用者才安全。
 * 对按值传递的 Datum，'copy' 参数的值没有影响。

 */
bool
tuplesort_getdatum(Tuplesortstate *state, bool forward, bool copy,
				   Datum *val, bool *isNull, Datum *abbrev)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MemoryContext oldcontext = MemoryContextSwitchTo(base->sortcontext);
	TuplesortDatumArg *arg = (TuplesortDatumArg *) base->arg;
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup))
	{
		MemoryContextSwitchTo(oldcontext);
		return false;
	}

	/* 确保复制进调用者的内存上下文 */
	MemoryContextSwitchTo(oldcontext);

	/* 为调用者记录缩略键 */
	if (base->sortKeys->abbrev_converter && abbrev)
		*abbrev = stup.datum1;

	if (stup.isnull1 || !base->tuples)
	{
		*val = stup.datum1;
		*isNull = stup.isnull1;
	}
	else
	{
		/* 用 stup.tuple，因 stup.datum1 可能是缩略值 */
		if (copy)
			*val = datumCopy(PointerGetDatum(stup.tuple), false,
							 arg->datumTypeLen);
		else
			*val = PointerGetDatum(stup.tuple);
		*isNull = false;
	}

	return true;
}


/*
 * 为 HeapTuple（实为 MinimalTuple）情形专门化例程
 */

static void
removeabbrev_heap(Tuplesortstate *state, SortTuple *stups, int count)
{
	int			i;
	TuplesortPublic *base = TuplesortstateGetPublic(state);

	for (i = 0; i < count; i++)
	{
		HeapTupleData htup;

		htup.t_len = ((MinimalTuple) stups[i].tuple)->t_len +
			MINIMAL_TUPLE_OFFSET;
		htup.t_data = (HeapTupleHeader) ((char *) stups[i].tuple -
										 MINIMAL_TUPLE_OFFSET);
		stups[i].datum1 = heap_getattr(&htup,
									   base->sortKeys[0].ssup_attno,
									   (TupleDesc) base->arg,
									   &stups[i].isnull1);
	}
}

static int
comparetup_heap(const SortTuple *a, const SortTuple *b, Tuplesortstate *state)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	SortSupport sortKey = base->sortKeys;
	HeapTupleData ltup;
	HeapTupleData rtup;
	TupleDesc	tupDesc;
	int			nkey;
	int32		compare;
	AttrNumber	attno;
	Datum		datum1,
				datum2;
	bool		isnull1,
				isnull2;


	/* 比较前导排序键 */
	compare = ApplySortComparator(a->datum1, a->isnull1,
								  b->datum1, b->isnull1,
								  sortKey);
	if (compare != 0)
		return compare;

	/* 比较其余排序键 */
	ltup.t_len = ((MinimalTuple) a->tuple)->t_len + MINIMAL_TUPLE_OFFSET;
	ltup.t_data = (HeapTupleHeader) ((char *) a->tuple - MINIMAL_TUPLE_OFFSET);
	rtup.t_len = ((MinimalTuple) b->tuple)->t_len + MINIMAL_TUPLE_OFFSET;
	rtup.t_data = (HeapTupleHeader) ((char *) b->tuple - MINIMAL_TUPLE_OFFSET);
	tupDesc = (TupleDesc) base->arg;

	if (sortKey->abbrev_converter)
	{
		attno = sortKey->ssup_attno;

		datum1 = heap_getattr(&ltup, attno, tupDesc, &isnull1);
		datum2 = heap_getattr(&rtup, attno, tupDesc, &isnull2);

		compare = ApplySortAbbrevFullComparator(datum1, isnull1,
												datum2, isnull2,
												sortKey);
		if (compare != 0)
			return compare;
	}

	sortKey++;
	for (nkey = 1; nkey < base->nKeys; nkey++, sortKey++)
	{
		attno = sortKey->ssup_attno;

		datum1 = heap_getattr(&ltup, attno, tupDesc, &isnull1);
		datum2 = heap_getattr(&rtup, attno, tupDesc, &isnull2);

		compare = ApplySortComparator(datum1, isnull1,
									  datum2, isnull2,
									  sortKey);
		if (compare != 0)
			return compare;
	}

	return 0;
}

static void
writetup_heap(Tuplesortstate *state, LogicalTape *tape, SortTuple *stup)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MinimalTuple tuple = (MinimalTuple) stup->tuple;

	/* 将写出的 MinimalTuple 部分： */
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;
	unsigned int tupbodylen = tuple->t_len - MINIMAL_TUPLE_DATA_OFFSET;

	/* 磁盘总占用： */
	unsigned int tuplen = tupbodylen + sizeof(int);

	LogicalTapeWrite(tape, &tuplen, sizeof(tuplen));
	LogicalTapeWrite(tape, tupbody, tupbodylen);
	if (base->sortopt & TUPLESORT_RANDOMACCESS) /* 要尾随长度字？ */
		LogicalTapeWrite(tape, &tuplen, sizeof(tuplen));
}

static void
readtup_heap(Tuplesortstate *state, SortTuple *stup,
			 LogicalTape *tape, unsigned int len)
{
	unsigned int tupbodylen = len - sizeof(int);
	unsigned int tuplen = tupbodylen + MINIMAL_TUPLE_DATA_OFFSET;
	MinimalTuple tuple = (MinimalTuple) tuplesort_readtup_alloc(state, tuplen);
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	HeapTupleData htup;

	/* 读入元组本体 */
	tuple->t_len = tuplen;
	LogicalTapeReadExact(tape, tupbody, tupbodylen);
	if (base->sortopt & TUPLESORT_RANDOMACCESS) /* 要尾随长度字？ */
		LogicalTapeReadExact(tape, &tuplen, sizeof(tuplen));
	stup->tuple = (void *) tuple;
	/* 设置第一列键值 */
	htup.t_len = tuple->t_len + MINIMAL_TUPLE_OFFSET;
	htup.t_data = (HeapTupleHeader) ((char *) tuple - MINIMAL_TUPLE_OFFSET);
	stup->datum1 = heap_getattr(&htup,
								base->sortKeys[0].ssup_attno,
								(TupleDesc) base->arg,
								&stup->isnull1);
}

/*
 * 为 CLUSTER 情形专门化的例程（HeapTuple 数据，
 * 按 btree 索引定义进行比较）
 */

static void
removeabbrev_cluster(Tuplesortstate *state, SortTuple *stups, int count)
{
	int			i;
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortClusterArg *arg = (TuplesortClusterArg *) base->arg;

	for (i = 0; i < count; i++)
	{
		HeapTuple	tup;

		tup = (HeapTuple) stups[i].tuple;
		stups[i].datum1 = heap_getattr(tup,
									   arg->indexInfo->ii_IndexAttrNumbers[0],
									   arg->tupDesc,
									   &stups[i].isnull1);
	}
}

static int
comparetup_cluster(const SortTuple *a, const SortTuple *b,
				   Tuplesortstate *state)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortClusterArg *arg = (TuplesortClusterArg *) base->arg;
	SortSupport sortKey = base->sortKeys;
	HeapTuple	ltup;
	HeapTuple	rtup;
	TupleDesc	tupDesc;
	int			nkey;
	int32		compare;
	Datum		datum1,
				datum2;
	bool		isnull1,
				isnull2;

	/* 准备好比较其余排序键 */
	ltup = (HeapTuple) a->tuple;
	rtup = (HeapTuple) b->tuple;
	tupDesc = arg->tupDesc;

	/* 比较前导排序键（若其简单） */
	if (base->haveDatum1)
	{
		compare = ApplySortComparator(a->datum1, a->isnull1,
									  b->datum1, b->isnull1,
									  sortKey);
		if (compare != 0)
			return compare;

		if (sortKey->abbrev_converter)
		{
			AttrNumber	leading = arg->indexInfo->ii_IndexAttrNumbers[0];

			datum1 = heap_getattr(ltup, leading, tupDesc, &isnull1);
			datum2 = heap_getattr(rtup, leading, tupDesc, &isnull2);

			compare = ApplySortAbbrevFullComparator(datum1, isnull1,
													datum2, isnull2,
													sortKey);
		}
		if (compare != 0 || base->nKeys == 1)
			return compare;
		/* 用笨办法比较其余的列 */
		sortKey++;
		nkey = 1;
	}
	else
	{
		/* 必须用笨办法比较所有键 */
		nkey = 0;
	}

	if (arg->indexInfo->ii_Expressions == NULL)
	{
		/* 若非表达式索引，仅直接比较相应堆属性 */

		for (; nkey < base->nKeys; nkey++, sortKey++)
		{
			AttrNumber	attno = arg->indexInfo->ii_IndexAttrNumbers[nkey];

			datum1 = heap_getattr(ltup, attno, tupDesc, &isnull1);
			datum2 = heap_getattr(rtup, attno, tupDesc, &isnull2);

			compare = ApplySortComparator(datum1, isnull1,
										  datum2, isnull2,
										  sortKey);
			if (compare != 0)
				return compare;
		}
	}
	else
	{
		/*
		 * 在表达式索引情形下，算出整个索引元组然后
		 * 比较其值。只计算需要比较的列或许更快，
		 * 但那需要复制
		 * FormIndexDatum 的全部逻辑。
		 */
		Datum		l_index_values[INDEX_MAX_KEYS];
		bool		l_index_isnull[INDEX_MAX_KEYS];
		Datum		r_index_values[INDEX_MAX_KEYS];
		bool		r_index_isnull[INDEX_MAX_KEYS];
		TupleTableSlot *ecxt_scantuple;

		/* 每次重置上下文以防内存泄漏 */
		ResetPerTupleExprContext(arg->estate);

		ecxt_scantuple = GetPerTupleExprContext(arg->estate)->ecxt_scantuple;

		ExecStoreHeapTuple(ltup, ecxt_scantuple, false);
		FormIndexDatum(arg->indexInfo, ecxt_scantuple, arg->estate,
					   l_index_values, l_index_isnull);

		ExecStoreHeapTuple(rtup, ecxt_scantuple, false);
		FormIndexDatum(arg->indexInfo, ecxt_scantuple, arg->estate,
					   r_index_values, r_index_isnull);

		for (; nkey < base->nKeys; nkey++, sortKey++)
		{
			compare = ApplySortComparator(l_index_values[nkey],
										  l_index_isnull[nkey],
										  r_index_values[nkey],
										  r_index_isnull[nkey],
										  sortKey);
			if (compare != 0)
				return compare;
		}
	}

	return 0;
}

static void
writetup_cluster(Tuplesortstate *state, LogicalTape *tape, SortTuple *stup)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	HeapTuple	tuple = (HeapTuple) stup->tuple;
	unsigned int tuplen = tuple->t_len + sizeof(ItemPointerData) + sizeof(int);

	/* 需存 t_self，其余 HeapTupleData 字段不必存 */
	LogicalTapeWrite(tape, &tuplen, sizeof(tuplen));
	LogicalTapeWrite(tape, &tuple->t_self, sizeof(ItemPointerData));
	LogicalTapeWrite(tape, tuple->t_data, tuple->t_len);
	if (base->sortopt & TUPLESORT_RANDOMACCESS) /* 要尾随长度字？ */
		LogicalTapeWrite(tape, &tuplen, sizeof(tuplen));
}

static void
readtup_cluster(Tuplesortstate *state, SortTuple *stup,
				LogicalTape *tape, unsigned int tuplen)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortClusterArg *arg = (TuplesortClusterArg *) base->arg;
	unsigned int t_len = tuplen - sizeof(ItemPointerData) - sizeof(int);
	HeapTuple	tuple = (HeapTuple) tuplesort_readtup_alloc(state,
															t_len + HEAPTUPLESIZE);

	/* 重建 HeapTupleData 头部 */
	tuple->t_data = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);
	tuple->t_len = t_len;
	LogicalTapeReadExact(tape, &tuple->t_self, sizeof(ItemPointerData));
	/* 我们目前不打算重建 t_tableOid */
	tuple->t_tableOid = InvalidOid;
	/* 读入元组体 */
	LogicalTapeReadExact(tape, tuple->t_data, tuple->t_len);
	if (base->sortopt & TUPLESORT_RANDOMACCESS) /* 要尾随长度字？ */
		LogicalTapeReadExact(tape, &tuplen, sizeof(tuplen));
	stup->tuple = (void *) tuple;
	/* 若是简单列，则设置第一列键值 */
	if (base->haveDatum1)
		stup->datum1 = heap_getattr(tuple,
									arg->indexInfo->ii_IndexAttrNumbers[0],
									arg->tupDesc,
									&stup->isnull1);
}

static void
freestate_cluster(Tuplesortstate *state)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortClusterArg *arg = (TuplesortClusterArg *) base->arg;

	/* 释放为 CLUSTER 情形创建的执行状态 */
	if (arg->estate != NULL)
	{
		ExprContext *econtext = GetPerTupleExprContext(arg->estate);

		ExecDropSingleTupleTableSlot(econtext->ecxt_scantuple);
		FreeExecutorState(arg->estate);
	}
}

/*
 * 为 IndexTuple 情形专门化的例程
 *
 * btree 与 hash 情形需要各自的比较函数，但两者的
 * IndexTuple 表示是相同的，所以复制/写出/读取支持
 * 函数可以共享。
 */

static void
removeabbrev_index(Tuplesortstate *state, SortTuple *stups, int count)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortIndexArg *arg = (TuplesortIndexArg *) base->arg;
	int			i;

	for (i = 0; i < count; i++)
	{
		IndexTuple	tuple;

		tuple = stups[i].tuple;
		stups[i].datum1 = index_getattr(tuple,
										1,
										RelationGetDescr(arg->indexRel),
										&stups[i].isnull1);
	}
}

static int
comparetup_index_btree(const SortTuple *a, const SortTuple *b,
					   Tuplesortstate *state)
{
	/*
	 * 这与 comparetup_heap() 类似，但预期的是索引元组。
	 * 另有强制唯一性的特殊处理，末尾对相等
	 * 键的特殊处理。
	 */
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortIndexBTreeArg *arg = (TuplesortIndexBTreeArg *) base->arg;
	SortSupport sortKey = base->sortKeys;
	IndexTuple	tuple1;
	IndexTuple	tuple2;
	int			keysz;
	TupleDesc	tupDes;
	bool		equal_hasnull = false;
	int			nkey;
	int32		compare;
	Datum		datum1,
				datum2;
	bool		isnull1,
				isnull2;


	/* 比较前导排序键 */
	compare = ApplySortComparator(a->datum1, a->isnull1,
								  b->datum1, b->isnull1,
								  sortKey);
	if (compare != 0)
		return compare;

	/* 比较其余排序键 */
	tuple1 = (IndexTuple) a->tuple;
	tuple2 = (IndexTuple) b->tuple;
	keysz = base->nKeys;
	tupDes = RelationGetDescr(arg->index.indexRel);

	if (sortKey->abbrev_converter)
	{
		datum1 = index_getattr(tuple1, 1, tupDes, &isnull1);
		datum2 = index_getattr(tuple2, 1, tupDes, &isnull2);

		compare = ApplySortAbbrevFullComparator(datum1, isnull1,
												datum2, isnull2,
												sortKey);
		if (compare != 0)
			return compare;
	}

	/* 二者相等，因此只需检查其中一个空标志 */
	if (a->isnull1)
		equal_hasnull = true;

	sortKey++;
	for (nkey = 2; nkey <= keysz; nkey++, sortKey++)
	{
		datum1 = index_getattr(tuple1, nkey, tupDes, &isnull1);
		datum2 = index_getattr(tuple2, nkey, tupDes, &isnull2);

		compare = ApplySortComparator(datum1, isnull1,
									  datum2, isnull2,
									  sortKey);
		if (compare != 0)
			return compare;		/* 发现不等属性时即完成 */

		/* 二者相等，因此只需检查其中一个空标志 */
		if (isnull1)
			equal_hasnull = true;
	}

	/*
	 * 若 btree 要求我们强制唯一性，则在检测到两个
	 * 相等元组时 complain（除非有 NULL 字段且未设
	 * NULLS NOT DISTINCT）。
	 *
	 * 在这里做这一检验就足够了，因为如果两个元组
	 * 相等，它们*必然*会在排序的某个阶段被比较 ---
	 * 否则我们就不会检查过二者中哪个必须居
	 * 前。
	 */
	if (arg->enforceUnique && !(!arg->uniqueNullsNotDistinct && equal_hasnull))
	{
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];
		char	   *key_desc;

		/*
		 * 某些相当笨拙的 qsort 实现（例如
		 * QNX 4 中的那个）有时会调用比较例程把一个值
		 * 与自身比较，但总是使用自己的实现，它不会
		 * 这样。
		 */
		Assert(tuple1 != tuple2);

		index_deform_tuple(tuple1, tupDes, values, isnull);

		key_desc = BuildIndexValueDescription(arg->index.indexRel, values, isnull);

		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("could not create unique index \"%s\"",
						RelationGetRelationName(arg->index.indexRel)),
				 key_desc ? errdetail("Key %s is duplicated.", key_desc) :
				 errdetail("Duplicate keys exist."),
				 errtableconstraint(arg->index.heapRel,
									RelationGetRelationName(arg->index.indexRel))));
	}

	/*
	 * 若键值相等，我们按 ItemPointer 排序。这对 btree 索引
	 * 来说是必需的，堆 TID 被当作隐式的最后一个键
	 * 属性，以确保索引中所有键在物理上都唯一。
	 *
	 */
	{
		BlockNumber blk1 = ItemPointerGetBlockNumber(&tuple1->t_tid);
		BlockNumber blk2 = ItemPointerGetBlockNumber(&tuple2->t_tid);

		if (blk1 != blk2)
			return (blk1 < blk2) ? -1 : 1;
	}
	{
		OffsetNumber pos1 = ItemPointerGetOffsetNumber(&tuple1->t_tid);
		OffsetNumber pos2 = ItemPointerGetOffsetNumber(&tuple2->t_tid);

		if (pos1 != pos2)
			return (pos1 < pos2) ? -1 : 1;
	}

	/* ItemPointer 值绝不应相等 */
	Assert(false);

	return 0;
}

static int
comparetup_index_hash(const SortTuple *a, const SortTuple *b,
					  Tuplesortstate *state)
{
	Bucket		bucket1;
	Bucket		bucket2;
	uint32		hash1;
	uint32		hash2;
	IndexTuple	tuple1;
	IndexTuple	tuple2;
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortIndexHashArg *arg = (TuplesortIndexHashArg *) base->arg;

	/*
	 * 取得哈希键并掩掉我们不想据以排序的位，使得
	 * 初始排序只针对桶号。我们知道索引元组的
	 * 第一列就是哈希键。
	 */
	Assert(!a->isnull1);
	bucket1 = _hash_hashkey2bucket(DatumGetUInt32(a->datum1),
								   arg->max_buckets, arg->high_mask,
								   arg->low_mask);
	Assert(!b->isnull1);
	bucket2 = _hash_hashkey2bucket(DatumGetUInt32(b->datum1),
								   arg->max_buckets, arg->high_mask,
								   arg->low_mask);
	if (bucket1 > bucket2)
		return 1;
	else if (bucket1 < bucket2)
		return -1;

	/*
	 * 若桶值相等，则按哈希值排序。这使我们可以
	 * 直接插入到桶/溢出页上，那里索引元组按哈希序
	 * 存储，以便每页内可进行快速的二分查找。
	 */
	hash1 = DatumGetUInt32(a->datum1);
	hash2 = DatumGetUInt32(b->datum1);
	if (hash1 > hash2)
		return 1;
	else if (hash1 < hash2)
		return -1;

	/*
	 * 若哈希值相等，我们按 ItemPointer 排序。这不影响
	 * 已建成索引的有效性，但让索引扫描保持物理顺序
	 * 可能有用。
	 */
	tuple1 = (IndexTuple) a->tuple;
	tuple2 = (IndexTuple) b->tuple;

	{
		BlockNumber blk1 = ItemPointerGetBlockNumber(&tuple1->t_tid);
		BlockNumber blk2 = ItemPointerGetBlockNumber(&tuple2->t_tid);

		if (blk1 != blk2)
			return (blk1 < blk2) ? -1 : 1;
	}
	{
		OffsetNumber pos1 = ItemPointerGetOffsetNumber(&tuple1->t_tid);
		OffsetNumber pos2 = ItemPointerGetOffsetNumber(&tuple2->t_tid);

		if (pos1 != pos2)
			return (pos1 < pos2) ? -1 : 1;
	}

	/* ItemPointer 值绝不应相等 */
	Assert(false);

	return 0;
}

static void
writetup_index(Tuplesortstate *state, LogicalTape *tape, SortTuple *stup)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	IndexTuple	tuple = (IndexTuple) stup->tuple;
	unsigned int tuplen;

	tuplen = IndexTupleSize(tuple) + sizeof(tuplen);
	LogicalTapeWrite(tape, &tuplen, sizeof(tuplen));
	LogicalTapeWrite(tape, tuple, IndexTupleSize(tuple));
	if (base->sortopt & TUPLESORT_RANDOMACCESS) /* 要尾随长度字？ */
		LogicalTapeWrite(tape, &tuplen, sizeof(tuplen));
}

static void
readtup_index(Tuplesortstate *state, SortTuple *stup,
			  LogicalTape *tape, unsigned int len)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortIndexArg *arg = (TuplesortIndexArg *) base->arg;
	unsigned int tuplen = len - sizeof(unsigned int);
	IndexTuple	tuple = (IndexTuple) tuplesort_readtup_alloc(state, tuplen);

	LogicalTapeReadExact(tape, tuple, tuplen);
	if (base->sortopt & TUPLESORT_RANDOMACCESS) /* 要尾随长度字？ */
		LogicalTapeReadExact(tape, &tuplen, sizeof(tuplen));
	stup->tuple = (void *) tuple;
	/* 设置第一列键值 */
	stup->datum1 = index_getattr(tuple,
								 1,
								 RelationGetDescr(arg->indexRel),
								 &stup->isnull1);
}

/*
 * 为 DatumTuple 情形专门化的例程
 */

static void
removeabbrev_datum(Tuplesortstate *state, SortTuple *stups, int count)
{
	int			i;

	for (i = 0; i < count; i++)
		stups[i].datum1 = PointerGetDatum(stups[i].tuple);
}

static int
comparetup_datum(const SortTuple *a, const SortTuple *b, Tuplesortstate *state)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	int			compare;

	compare = ApplySortComparator(a->datum1, a->isnull1,
								  b->datum1, b->isnull1,
								  base->sortKeys);
	if (compare != 0)
		return compare;

	/* 若有缩略，则"tuple"保有原值 */

	if (base->sortKeys->abbrev_converter)
		compare = ApplySortAbbrevFullComparator(PointerGetDatum(a->tuple), a->isnull1,
												PointerGetDatum(b->tuple), b->isnull1,
												base->sortKeys);

	return compare;
}

static void
writetup_datum(Tuplesortstate *state, LogicalTape *tape, SortTuple *stup)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	TuplesortDatumArg *arg = (TuplesortDatumArg *) base->arg;
	void	   *waddr;
	unsigned int tuplen;
	unsigned int writtenlen;

	if (stup->isnull1)
	{
		waddr = NULL;
		tuplen = 0;
	}
	else if (!base->tuples)
	{
		waddr = &stup->datum1;
		tuplen = sizeof(Datum);
	}
	else
	{
		waddr = stup->tuple;
		tuplen = datumGetSize(PointerGetDatum(stup->tuple), false, arg->datumTypeLen);
		Assert(tuplen != 0);
	}

	writtenlen = tuplen + sizeof(unsigned int);

	LogicalTapeWrite(tape, &writtenlen, sizeof(writtenlen));
	LogicalTapeWrite(tape, waddr, tuplen);
	if (base->sortopt & TUPLESORT_RANDOMACCESS) /* 要尾随长度字？ */
		LogicalTapeWrite(tape, &writtenlen, sizeof(writtenlen));
}

static void
readtup_datum(Tuplesortstate *state, SortTuple *stup,
			  LogicalTape *tape, unsigned int len)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	unsigned int tuplen = len - sizeof(unsigned int);

	if (tuplen == 0)
	{
		/* 为 NULL */
		stup->datum1 = (Datum) 0;
		stup->isnull1 = true;
		stup->tuple = NULL;
	}
	else if (!base->tuples)
	{
		Assert(tuplen == sizeof(Datum));
		LogicalTapeReadExact(tape, &stup->datum1, tuplen);
		stup->isnull1 = false;
		stup->tuple = NULL;
	}
	else
	{
		void	   *raddr = tuplesort_readtup_alloc(state, tuplen);

		LogicalTapeReadExact(tape, raddr, tuplen);
		stup->datum1 = PointerGetDatum(raddr);
		stup->isnull1 = false;
		stup->tuple = raddr;
	}

	if (base->sortopt & TUPLESORT_RANDOMACCESS) /* 要尾随长度字？ */
		LogicalTapeReadExact(tape, &tuplen, sizeof(tuplen));
}
